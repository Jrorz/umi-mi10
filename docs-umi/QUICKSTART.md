# 小米 10 (umi) HackiOS 快速上手

> 面向接手的人。假设你已经有这台手机（BL 已解锁、有 root）和 macOS。

## 0. 先读这个

完整背景、硬件参数、已定位的 bug 都在 **[PROGRESS.md](PROGRESS.md)**。本文只讲怎么跑起来。

## 1. 环境准备

```sh
# Xcode 命令行工具（编译内核用）
xcode-select --install

# homebrew 工具（许多 GNU 工具需要）
brew install llvm gnu-tar gnu-sed coreutils p7zip

# adb / fastboot
brew install --cask android-platform-tools
```

仓库里 `.build/shim/` 放了一批 GNU 工具的替代脚本（`nproc` / `awk` / `tar` / `sed` / `timeout` / `stat` / `7z`），构建时要在 PATH 前面。

## 2. 目录结构

```
hackios-xnu-kernel-main/
├── xnu/files/                 我们改的内核源码（覆盖到 xnu-src 后编译）
├── xnu-src/                   XNU 源码 + 构建产物
├── xnuloader/                 loader（扮演 iBoot 角色）
├── Mu-Silicium/               定制 UEFI（含 umiPkg）
├── userland/                  system.apfs / root.apfs 系统镜像
├── common/devices/umi.conf    设备配置
├── umi-backup/                分区备份（重要，别删）
├── out/                       构建产物（boot image、payload）
└── .build/                    脚本与中间文件
```

## 3. 构建内核

```sh
# 把 xnu/files 下的改动同步到 xnu-src
cp xnu/files/bsd/dev/redwood_touch_i2c.c xnu-src/xnu-8796.121.2/bsd/dev/
cp xnu/files/bsd/dev/redwood_buttons.c   xnu-src/xnu-8796.121.2/bsd/dev/
cp xnu/files/bsd/dev/redwood_input.c     xnu-src/xnu-8796.121.2/bsd/dev/
cp xnu/files/bsd/sys/redwood_input.h     xnu-src/xnu-8796.121.2/bsd/sys/

# 删除对应 .o 强制重编
rm -f xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/redwood_touch_i2c.o
rm -f xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/redwood_buttons.o
rm -f xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/redwood_input.o

# 编译
THEOS="$PWD/.build/theos" PATH="$PWD/.build/shim:$PATH" ./xnu-build.sh -j8 -k
```

`exit=2` 是已知的导出符号检查，**只看有没有 `error:`**。

产物：`xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837`

## 4. 打包 payload

```sh
K=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837

python3 xnuloader/mkpayload.py -o out/umi-final.bin \
    kernel=$K \
    ramdisk=userland/root.apfs \
    --cmdline "debug=0x8 serial=3 cpus=4 buttons=3 touchscreen=2"
```

`ramdisk=userland/root.apfs` 会作为 **md1**；`stage` 传的 `system.apfs` 作为 **md0**。

## 5. 部署到手机

```sh
# 手机先进入 fastboot
adb reboot bootloader          # 或长按电源键 10 秒后按住音量下

# 一键脚本（推荐）
.build/umi-run.sh out/umi-final.bin
```

脚本做的事（顺序不能变）：

```sh
fastboot boot out/umi-uefi.img     # UEFI + loader
sleep 20
fastboot oem ramdisk               # ① 必须先告诉 loader
fastboot stage userland/system.apfs  # ② 2.3 GB，约 92 秒 → md0
fastboot stage out/umi-final.bin     # ③ 内核 payload → md1
fastboot oem run                     # ④ 启动
```

**⚠️ `oem ramdisk` 必须在大镜像之前**，否则 2.3 GB 会覆盖内核 payload（症状：`no payload at f0000000`）。

## 6. 回安卓

**长按电源键 10 秒**，或 `fastboot reboot`。

`fastboot boot` 不写任何分区，**随时可恢复**。真出问题用 `umi-backup/` 里的镜像刷回：

```sh
fastboot flash boot   umi-backup/boot.img
fastboot flash dtbo   umi-backup/dtbo.img
fastboot flash vbmeta umi-backup/vbmeta.img
```

## 7. 看日志

```sh
# 系统跑完（或崩溃）后，回安卓读 logdump 分区
adb shell su -c 'dd if=/dev/block/by-name/logdump of=/data/local/tmp/ld.bin bs=4096 count=450'
adb pull /data/local/tmp/ld.bin
strings ld.bin | less
```

**注意**：`fastboot oem log` 在系统运行时取不到（iOS 占着 USB）。内核日志只有 16 KB 环形缓冲，早期输出会被覆盖。

## 8. 现在的状态

✅ 能启动到 **iOS 16.5 主屏幕**
❌ 触摸不可用（已定位 3 个原因，代码已改，未经真机验证）
❌ 按键不可用（已定位 3 个错误，已改，未经验证）
❌ `touchscreen=2` 会触发 16 MB `kmem_alloc` panic（已二分确认来自输入子系统，但未定位到具体驱动）

详见 [PROGRESS.md](PROGRESS.md) 第五、六节。

## 9. 关键参考

**`~/Downloads/UniXNU-umi-main`** —— 专门为小米10做的、**已跑通触摸**的项目（attempt 80 设备所有者确认可用）。有完整 C++ 驱动、编译脚本、50+ 单元测试和详细审计文档。

**强烈建议**：直接用它 `bringup/tools/build_umi_touch.py` 的产物，而不是手工把 C++ 翻译成 C。我手工翻译的过程中引入了大量错误（TLMM 基址、事件协议、sysinfo 身份、sensing 命令），每修一个都要重启手机验证一次，效率极低。

## 10. 踩过的坑（省你时间）

| 坑 | 症状 | 解决 |
|---|---|---|
| `oem ramdisk` 顺序反了 | `no payload at f0000000` | 先 `oem ramdisk` 再 `stage` |
| TLMM 用了 redwood 的 `0x0F100000` | 引脚全部操作无效 | 正确值 **`0x0F500000`**（Kona SOUTH）|
| 触摸芯片认成 Goodix GT9886 | I²C 无响应 | 实际是 **FocalTech FT3680 @ 0x49**（看安卓 dmesg）|
| 不发 sensing 命令 | 读不到任何触摸事件 | 必须发 `A0 00 01` / `A2 03...` |
| 用 `ml_io_map` 映射 16 MB | 启动卡死 | 窗口改小（≤1 MB）|
| `touchscreen=2` 触发 panic | 启不来，只在 Silicium 后回安卓 | 见 PROGRESS.md 第六节，或先不带此参数 |
| 按键位用了 `0x80`/`0x40` | 按键永远无反应 | 那是中断位；状态字节是 **bit0/bit1** |
