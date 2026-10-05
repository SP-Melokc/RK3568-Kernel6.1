# initramfs_min —— kdump 捕获内核用的最小 initramfs

给 kdump 的**捕获内核（第二个内核）**用的最小 initramfs。捕获内核起来后，就是靠这里的
`/init` 把 `/proc/vmcore` 落盘。

> 这个目录**不参与内核构建**（kbuild 不管它），放在内核树里只是当 recipe/参考。

## 1. 文件

| 文件 | 作用 |
|------|------|
| `kdump_capture_init.c` | **`/init` 本体**——静态编译的 C 程序，就是捕获内核的 PID 1 |
| `build_initramfs.sh`     | 一键造出 `initrd.kdump`（建目录树、编 init、造设备节点、cpio+gzip） |

> **这里没有 shell 版的 init 脚本，是有意的**：`/init` 就是这个 C 文件**编译出来的**。
> 用 shell 当 init 就得带 libc + busybox，而"最小 initramfs 里放动态 busybox → 起不来"正是当初踩的坑
> （`error while loading shared libraries`）。一个 `-static` 的 C init 直接把这整类问题绕开。

## 2. 最小 initramfs 长什么样

`initrd.kdump` 解包后就是这么几样：

```
/
├── init            ← 静态 C 程序（必须可执行，root:root）
├── dev/
│   ├── console     ← c 5 1   ★ 内核在 init 运行前就要用它；init 挂 devtmpfs 之后才补全剩下的
│   └── null        ← c 1 3
├── proc/           ← 空目录，init 里 mount
├── sys/            ← 空目录
└── mnt/            ← 空目录，挂根文件系统分区用
```

只有 `/init` 和 `/dev/console` 是**必需**的；`/dev/console` 若缺失，PID 1 起手拿不到标准输入输出。
其余目录是给 init 挂载用的空挂载点。

## 3. 怎么造

```sh
# 需要 aarch64 交叉工具链，且能 mknod（root，或脚本会退化成 sudo mknod）
CC=/path/to/aarch64-none-linux-gnu-gcc ./build_initramfs.sh
# 产物：./initrd.kdump
```

或者手动三步：

```sh
CC=aarch64-none-linux-gnu-gcc
ROOT=$(mktemp -d)/root
mkdir -p $ROOT/{dev,proc,sys,mnt}
$CC -static -O2 -s -o $ROOT/init kdump_capture_init.c
sudo mknod -m 600 $ROOT/dev/console c 5 1
sudo mknod -m 666 $ROOT/dev/null    c 1 3
( cd $ROOT && find . | cpio -o -H newc | gzip -9 ) > initrd.kdump
```

## 4. 怎么用（从第一个内核里加载）

```sh
# 1) 把捕获内核 + initramfs 放到板上（adb push 或打包进 rootfs）
#    Image.min 是裁剪过的捕获内核（见 SDK 的 kernel-min）
# 2) 加载成 crash 镜像
kexec -p /root/Image.min --initrd=/root/initrd.kdump \
  -c "console=ttyFIQ0,1500000n8 earlycon=uart8250,mmio32,0xfe660000 \
      rdinit=/init irqpoll nr_cpus=1 reset_devices"
# 3) 验证
cat /sys/kernel/kexec_crash_loaded      # 1
# 4) 触发
echo c > /proc/sysrq-trigger
```

捕获内核起来后：

- 打印 banner `capture-init (STATIC, kdump 2nd kernel)` —— 看到它就说明**跳转成功**了；
- 挂载 `/dev/mmcblk0p6/p7/p5` 里第一个能挂的，把 `/proc/vmcore` 全量写到 `/vmcore.elf`；
- `sync` 后 8 秒自动 `reboot`。

## 5. 注意

- **分区列表是板子相关的**：`kdump_capture_init.c` 里 `parts[] = { "/dev/mmcblk0p6", ... }` 是按
  ATK-DLRK3568 的布局写的，换板子改这里。
- **原始核很大**：2G 内存 → `/proc/vmcore` 有 1.94G。生产上应该在捕获内核里用 **makedumpfile**
  过滤+压缩后再落盘（`makedumpfile -c -d 31 /proc/vmcore /mnt/vmcore.kd`），能把大小压到几十 MB。
  但那需要给捕获内核塞一个 **aarch64 的 makedumpfile**，它硬链接 `-lelf -ldw -lbz2 -lz`，
  得连 elfutils/bzip2/zlib 一起静态编——是独立的下一步。
- **捕获内核必须能打印**：`earlycon` 是轮询的、不依赖中断，所以串口一定能看到捕获内核的输出。
