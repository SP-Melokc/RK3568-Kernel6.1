#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Build the minimal kdump capture-kernel initramfs for ATK-DLRK3568.
#
#   output : initrd.kdump   (cpio + gzip)  -> pass to  kexec -p --initrd=
#   input  : kdump_capture_init.c          -> compiled to /init (static)
#
# Usage:
#   CC=aarch64-none-linux-gnu-gcc ./build_initramfs.sh [output]
#   (CC defaults to aarch64-none-linux-gnu-gcc; point it at your SDK toolchain)
#
# Resulting initramfs tree:
#   /init            static C init (mounts /proc, dumps /proc/vmcore to disk)
#   /dev/console     5,1   <-- init's stdio before devtmpfs exists
#   /dev/null        1,3
#   /proc /sys /mnt  empty mount points
#
# Note: there is NO shell-script init. /init *is* the compiled C program; that
# is deliberate (a shell needs a libc, and pulling busybox + libs into a
# minimal initramfs is exactly the rabbit hole this avoids).

set -e

CC="${CC:-aarch64-none-linux-gnu-gcc}"
OUT="${1:-initrd.kdump}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(mktemp -d)/root"

cleanup() { rm -rf "$(dirname "$ROOT")"; }
trap cleanup EXIT

mkdir -p "$ROOT/dev" "$ROOT/proc" "$ROOT/sys" "$ROOT/mnt"

echo "[*] compiling static /init with: $CC"
"$CC" -static -O2 -s -o "$ROOT/init" "$HERE/kdump_capture_init.c"

# /dev/console must exist in the unpacked initramfs: the kernel opens it and
# hands it to PID 1 as stdin/stdout/stderr, *before* /init mounts devtmpfs.
# mknod needs CAP_MKNOD, so run as root (or fall back to sudo).
MKNOD="mknod"
[ "$(id -u)" -eq 0 ] || MKNOD="sudo mknod"
echo "[*] creating device nodes with '$MKNOD'"
$MKNOD -m 600 "$ROOT/dev/console" c 5 1
$MKNOD -m 666 "$ROOT/dev/null"    c 1 3

echo "[*] packing $OUT"
( cd "$ROOT" && find . | cpio -o -H newc 2>/dev/null | gzip -9 ) > "$OUT"

echo "[*] done:"; ls -l "$OUT"
