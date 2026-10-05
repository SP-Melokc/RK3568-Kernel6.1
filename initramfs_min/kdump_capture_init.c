// SPDX-License-Identifier: GPL-2.0
/*
 * kdump capture-kernel initramfs /init  (static, no busybox)
 *
 * This file is NOT part of the kernel build (kbuild ignores stray .c files at
 * the tree root). It is kept here as a reference/recipe: it is the static
 * /init for the minimal initramfs that the *capture* (second) kernel runs
 * after a kdump panic jump on the ATK-DLRK3568 (RK3568) board.
 *
 * Why a static C init instead of busybox:
 *   The capture initramfs is tiny and has no C library, and a dynamically
 *   linked busybox dies with "error while loading shared libraries". A static
 *   init sidesteps the lib/ld.so.cache problem entirely and needs no busybox
 *   applet symlinks.
 *
 * What it does:
 *   1. mount proc / sysfs / devtmpfs
 *   2. open /proc/vmcore and read ONLY the ELF header + program-header table.
 *      That lives in elfcorehdr, i.e. the capture kernel's *own* memory, so
 *      this never touches the dead kernel's memory. Every PT_LOAD range is
 *      logged to /mnt/vmcore_ranges.txt (great for spotting a corrupt vmcore
 *      header, e.g. a garbage segment with p_vaddr=0xffffffffffffffff).
 *   3. stream /proc/vmcore to /mnt/vmcore.elf on the rootfs partition
 *      (the rootfs auto-grown partition has enough room; /userdata also works)
 *   4. sync, then reboot after a short delay
 *
 * If /proc/vmcore does not exist (e.g. a normal "kexec -e", not a crash), it
 * just prints a marker and reboots.
 *
 * Build / pack (aarch64 cross toolchain):
 *   $CC -static -O2 -s -o root/init kdump_capture_init.c
 *   mkdir -p root/dev root/proc root/sys root/mnt
 *   sudo mknod root/dev/console c 5 1 ; sudo mknod root/dev/null c 1 3
 *   cd root && find . | cpio -o -H newc | gzip -9 > ../initrd.kdump
 *
 * Use (from the first kernel):
 *   kexec -p /root/Image.min --initrd=/root/initrd.kdump \
 *     -c "console=ttyFIQ0,1500000n8 earlycon=uart8250,mmio32,0xfe660000 \
 *         rdinit=/init irqpoll nr_cpus=1 reset_devices"
 *   echo c > /proc/sysrq-trigger
 *
 * Notes:
 *   - The partition list below is board-specific; adjust for your layout.
 *   - For production, replace step 3 with makedumpfile to filter + compress
 *     (needs a static aarch64 makedumpfile, which links -lelf/-ldw/-lbz2):
 *       makedumpfile -c -d 31 /proc/vmcore /mnt/vmcore.kd
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/reboot.h>

struct elf64_hdr {
	unsigned char e_ident[16];
	uint16_t e_type, e_machine;
	uint32_t e_version;
	uint64_t e_entry, e_phoff, e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};
struct elf64_phdr {
	uint32_t p_type, p_flags;
	uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
};

static void show(const char *p, const char *label)
{
	int fd = open(p, O_RDONLY);
	char buf[4096];
	ssize_t n;
	if (fd < 0) { printf("[init] %s: open %s failed (%s)\n", label, p, strerror(errno)); return; }
	printf("[init] %s: ", label);
	while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) { buf[n] = 0; printf("%s", buf); }
	printf("\n");
	close(fd);
}

static void dump_ranges(char *hdr, size_t hl, FILE *out, FILE *con)
{
	struct elf64_hdr eh;
	size_t i;
	if (hl < sizeof(eh)) { fprintf(out, "header too small (%zu)\n", hl); return; }
	memcpy(&eh, hdr, sizeof(eh));
	fprintf(out, "e_phoff=0x%llx e_phnum=%u e_phentsize=%u\n",
		(unsigned long long)eh.e_phoff, eh.e_phnum, eh.e_phentsize);
	fprintf(con, "[init] e_phoff=0x%llx e_phnum=%u\n",
		(unsigned long long)eh.e_phoff, eh.e_phnum);
	for (i = 0; i < eh.e_phnum; i++) {
		struct elf64_phdr ph;
		size_t off = eh.e_phoff + i * eh.e_phentsize;
		const char *nm;
		if (off + sizeof(ph) > hl) { fprintf(out, "  ph[%zu] beyond chunk\n", i); break; }
		memcpy(&ph, hdr + off, sizeof(ph));
		nm = ph.p_type == 1 ? "LOAD" : (ph.p_type == 4 ? "NOTE" : "?   ");
		fprintf(out, "%s[%zu] paddr=0x%016llx memsz=0x%08llx filesz=0x%08llx off=0x%llx\n",
			nm, i, (unsigned long long)ph.p_paddr, (unsigned long long)ph.p_memsz,
			(unsigned long long)ph.p_filesz, (unsigned long long)ph.p_offset);
		if (ph.p_type == 1)
			fprintf(con, "[init] LOAD paddr=0x%llx memsz=0x%llx\n",
				(unsigned long long)ph.p_paddr, (unsigned long long)ph.p_memsz);
	}
	fflush(out);
	fflush(con);
}

int main(void)
{
	const char *parts[] = { "/dev/mmcblk0p6", "/dev/mmcblk0p7", "/dev/mmcblk0p5", NULL };
	char hdr[8192];
	struct elf64_hdr eh;
	size_t need, got;
	ssize_t g2;
	int i, mounted = 0, fd;
	FILE *rf, *cf = stdout;

	printf("\n\n");
	printf("##################################################\n");
	printf("#   capture-init (STATIC, kdump 2nd kernel)       #\n");
	printf("##################################################\n");

	mount("proc", "/proc", "proc", 0, NULL);
	mount("sysfs", "/sys", "sysfs", 0, NULL);
	mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
	show("/proc/version", "version");
	show("/proc/cmdline", "cmdline");

	if (access("/proc/vmcore", F_OK) != 0) {
		printf(">>> [normal kexec] no /proc/vmcore <<<\n");
		goto out;
	}
	printf(">>> [KDUMP MODE] /proc/vmcore EXISTS <<<\n");

	for (i = 0; parts[i] && !mounted; i++) {
		mkdir("/mnt", 0755);
		if (mount(parts[i], "/mnt", "ext4", 0, NULL) == 0) {
			printf("[init] mounted %s -> /mnt\n", parts[i]);
			mounted = 1;
		} else
			printf("[init] mount %s failed (%s)\n", parts[i], strerror(errno));
	}
	if (!mounted) goto out;

	/* (a) read ONLY the ELF header, then exactly the program-header table */
	rf = fopen("/mnt/vmcore_ranges.txt", "w");
	fd = open("/proc/vmcore", O_RDONLY);
	if (fd >= 0) {
		got = read(fd, hdr, sizeof(eh));
		memcpy(&eh, hdr, sizeof(eh));
		need = (size_t)eh.e_phoff + (size_t)eh.e_phnum * eh.e_phentsize;
		if (need < sizeof(eh)) need = sizeof(eh);
		if (need > sizeof(hdr)) need = sizeof(hdr);
		if (need > got) {
			g2 = read(fd, hdr + got, need - got);
			if (g2 > 0) got += g2;
		}
		close(fd);
		if (rf) {
			printf("[init] ---- vmcore ranges ----\n");
			dump_ranges(hdr, got, rf, cf);
			fclose(rf);
			printf("[init] ranges -> /mnt/vmcore_ranges.txt\n");
		}
	}

	/* (b) stream the whole /proc/vmcore to the rootfs partition */
	{
		int in = open("/proc/vmcore", O_RDONLY);
		char *buf = malloc(1 << 20);
		int out = open("/mnt/vmcore.elf", O_WRONLY | O_CREAT | O_TRUNC, 0600);
		unsigned long long off = 0;
		ssize_t n = 0;
		if (in >= 0 && buf && out >= 0) {
			while ((n = read(in, buf, 1 << 20)) > 0) {
				if (write(out, buf, n) != n) {
					printf("[init] write short at 0x%llx\n", off);
					break;
				}
				off += n;
				if (off % (512ull << 20) < (1u << 20)) {
					printf("[init] vmcore read 0x%llx (%llu MB)\n", off, off >> 20);
					fflush(stdout);
				}
			}
			fsync(out);
		}
		printf("[init] READ DONE: %llu bytes (%llu MB), last read n=%zd\n",
			off, off >> 20, n);
		if (in >= 0) close(in);
		if (out >= 0) close(out);
		free(buf);
	}
	umount("/mnt");

out:
	sync();
	printf("[init] rebooting in 8s ...\n");
	sleep(8);
	reboot(RB_AUTOBOOT);
	for (;;) pause();
	return 0;
}
