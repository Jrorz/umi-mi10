# 在 macOS 上构建/运行 HackiOS 内核（本次实测记录）

主机：macOS 26.4 (Darwin 25.4.0) arm64 / Apple Silicon，Xcode 26.6，8 核 16 GB。

## 结果

| 环节 | 状态 |
|---|---|
| `prebuilt/` 校验和 | 7/7 通过 |
| `xnuloader/build.sh` | **原样跑通**，产出 loader.efi 68096 字节（与 prebuilt 同尺寸）|
| `tools/qmi-test/run.sh` | **58 项全过**（ASan+UBSan）|
| `tools/wpa-test/run.sh` | **195 项全过**（ASan+UBSan）|
| `xnu/apply.sh` | 通过：73 个改动文件 + 104 个新文件 |
| `xnu-deps/setup.sh --stage1` | 通过（借 Xcode 工具链替代 theos 的 Linux cctools）|
| `xnu-deps/setup.sh --stage2` | 通过：exporthdrs + libpthread_kext + libfirehose |
| `xnu-build.sh -j8 -k` | **内核链接成功**：mach.development.bcm2837 16,195,960 字节 |
| QEMU 启动自编内核 | **成功**：4 核、framebuffer 1125x2436、minicrypto、等待根设备 |

## 需要的 shim（.build/shim，不在仓库里）

* `nproc`  → `sysctl -n hw.ncpu`
* `tar`    → GNU tar（`--wildcards`；BSD tar 不支持）
* `awk`    → 拦截 `/proc/meminfo`，用本机内存量生成替身文件

## 对仓库做的最小移植改动（5 个文件）

1. `xnu-build.sh` —— `stat -c` → `stat -f`；macOS 上 HOST_CC 用系统 clang（不带 glibc 兼容头）
2. `xnu-deps/host/bin/xcrun` —— 对 macosx SDK 转交系统 xcrun；识别已解析的 device SDK 路径；
   `-show-sdk-platform-path` 否则会让 PLATFORM 误判为 MacOSX
3. `xnu-deps/hostinc/hostcompat.h` —— `__APPLE__` 下走 Darwin 分支
4. `xnu-deps/hostinc/hostcompat_darwin.h` —— **新增**，补 `byteswap.h`/`OSSwapInt*`
5. `xnu-deps/hostinc/architecture/byte_order.h` —— `__APPLE__` 下 `include_next` 真正的 SDK 头
6. `xnu-deps/hostinc/xnu_clang_compat.h` —— Xcode clang 声称有 `__builtin_xnu_type_signature`
   （语义却不同），在 `__apple_build_version__` 下强制走保守分支

## 复现

```sh
brew install automake gnu-tar
mkdir -p .build/shim   # 放上面三个 shim（见本目录说明）
git clone --depth 1 https://github.com/theos/sdks /tmp/sdks && mv /tmp/sdks/iPhoneOS16.5.sdk .build/theos/sdks/
# .build/theos/toolchain/linux/iphone/bin/ 里放 Xcode 工具的符号链接

export THEOS=$PWD/.build/theos PATH="$PWD/.build/shim:$PATH"
xnu/apply.sh && xnu-deps/setup.sh --stage2 && ./xnu-build.sh -j8 -k
# make 退出码 2 是预期的：Private.symbolset 导出检查 + lldbmacros 的 `python`
```

## 还缺什么才能真正进 iOS

**iOS 16.5 系统镜像**（`ios165/fetch.sh`，7.4 GB，从 Apple CDN 下载，需要一个 Debian/Arch Linux
主机跑 7z 拆 IPSW）。没有它，内核停在 `Waiting on ... Apple_HFS`，或（给了空 md0 时）
在 `/sbin/launchd` 不存在处 panic —— 这两个都已实测确认。

---

# 完整 iOS 启动（本次实测）

## 结果

**iOS 16.5 主屏幕在 macOS 上跑起来了。** 从 Apple 官方 IPSW 到 SpringBoard 渲染，全程零 panic。

| 阶段 | 结果 |
|---|---|
| `ios165/fetch.sh` | rootfs 2.9 G + OS cryptex 26 M + App cryptex 13 M + dyld cache 2.5 G（43 个文件）|
| `userland/build.sh` REST=1 | `root.apfs` 4.0 M + `system.apfs` 2,492,257,584 B |
| QEMU 启动 | loader → 内核 → md0 → launchd → **SpringBoard[7]** → **backboardd[11]** |
| 进程 | 28 个 iOS 进程成功启动，0 panic |
| 画面 | 主屏幕完整渲染（图标、Dock、状态栏、实时时钟 10:08→10:09）|
| 渲染路径 | CoreAnimation 软件渲染（QEMU 无 GPU，符合预期）|

截图：`logs/screen2.png`（主屏幕）、`logs/screen3.png`（一分钟后，时钟已走）
完整日志：`logs/ios-boot-full.log`（4146 行）

## 本次新增的 macOS 障碍

1. **`os.copy_file_range` 不存在**（Python 在 macOS 上无此 API）→ 改 `qemu/run-ios.sh`，
   Darwin 上退回 pread/pwrite 稀疏拷贝
2. **`timedatectl` 不存在** → shim，从 `/etc/localtime` 的链接读出时区
3. **BSD `sed -i` 需要备份后缀参数** → 用 GNU sed 作 shim
4. **`timeout` / `stat -c` 不存在** → coreutils 的 `gtimeout`、`gstat`
5. **`7z` 命令名** → Homebrew 装的是 `7zz`
6. **QEMU 需要 `-device ramfb`** 才有 framebuffer，否则内核报
   `no usable boot framebuffer (0x0, depth 0)`
7. **`fetch.sh` 在 cryptex 阶段中断**：`7z` 报 980 个 "Unsupported Method" 是预期的
   （LZBITMAP），但脚本没走到 `.done-cryptex`。手动补跑 dyld cache 解包 + unzbm 即可。

## 复现

```sh
brew install automake gnu-tar gnu-sed coreutils sevenzip
export THEOS=$PWD/.build/theos PATH="$PWD/.build/shim:$PWD/xnu-deps/host/bin:$PATH"

common/setup.sh                       # 拉 hackios-xnu-userland
ios165/fetch.sh                       # 5.3 GB，约 20 分钟
REST=1 userland/build.sh              # 生成 system.apfs
QEMU_EFI=/opt/homebrew/share/qemu/edk2-aarch64-code.fd \
QEMU_VARS=/opt/homebrew/share/qemu/edk2-arm-vars.fd \
  bash qemu/run-ios.sh 900 -display none
```

## 已知遗留问题

* **触摸事件没有传进去**：`virtio_tablet` 的动作发出去了（`touch.py` rc=0），
  内核也认到了 `virtio-input: device in slot 30/31`，backboardd 的 HID 系统 ready，
  但 SpringBoard 没响应点击。同类移植里这是常见的一步，需要看
  `bsd/dev/redwood_virtio_input.c` 把 QMP 的 abs 事件翻译成 HID 的哪一段没对上。
  不影响启动结论——主屏幕是真实渲染的，时钟在走。

---

# 小米 10（UMI）移植可行性

## 硬件

| | |
|---|---|
| SoC | Snapdragon 865 (SM8250, "kona") — 与 HackiOS 的 778G 同属 Qualcomm，但**平台包不同** |
| UEFI 参照 | Mu-Silicium 的 **`citrus`**（POCO F3 / K40，同为 SM8250），走 `KamortaPkg` |
| 关键参数 | `FD_BASE=0x5FC00000`、`FD_SIZE=0x300000`（**与 redwood 的 0x9FD00000/0x200000 完全不同**）|
| GPU | Adreno 650（redwood 是 642L，freedreno 驱动同一世代，改动小）|
| Wi-Fi | WCN3990 → **ath10k 而非 ath11k**，`wifi/lx/` 那套要换驱动 |
| 屏幕 | 6.67" AMOLED 2340×1080 90 Hz |
| 存储 | UFS 3.0 128 GB |
| 内存 | 8 GB（QEMU 侧验证过 7 GB 够用，8 GB 余量合适）|

## 站点兼容性表对它的判断

骁龙 865 落在 **"Snapdragon from 2018 on → Good"** 一档：
> 与 732G/778G 手机同一套积木：同样的中断控制器类型、同样的 RPMh 电源控制器、
> 同样的 GENI 串口引擎做 I²C。**主要是地址不同**。

`compatibility.html` 明确点名 Pixel 4 (855)、LG V60 (865) 已经跑起来，
说明 865 这一代是可行区间。

## 起步路线（对照 docs/PORTING.md）

```sh
common/new-device.sh umi path/to/umi.dtb      # 从 vendor_boot 或 adb pull /sys/firmware/fdt 拿
```

1. **UEFI**：抄 Mu-Silicium 的 `citrusPkg`，改名 `umiPkg`；
   `Binaries/umi` 用 UEFIReader 从你家 UMI 的 `xbl.img` 提取（**版本必须匹配你机器的 MIUI**）
2. **loader**：`xnuloader/loader.c` 的 `struct platform` 加一项，`fb_base` 填 UEFI 报的
   framebuffer 地址（屏幕 2340×1080，不是 redwood 的 1080×2400）
3. **内核**：`cpus=8`、`core_shift=8`（Qualcomm 惯例）、`small_cores=0x0f`（4×A55 + 3×A77 + 1×A77）
   —— 具体核心分布要看你 dtb
4. **触屏**：redwood 用的 Goodix GT9916，UMI 大概率是 **GT9886 或 FocalTech**
   → 需要新的协议实现（`redwood_goodix.c` 里 FocalTech 分支已有一半代码可参考）
5. **Wi-Fi**：ath10k 路线，不能直接用 `tools/mk-ath11k.sh`

## 诚实的前提

* 需要**解锁 bootloader**（小米 10 可解，但要 168 小时绑定账号）
* 需要从你自己的机器上 `adb pull` 出固件（`fw-gpu`/`fw-wifi`/`fw-audio`），仓库存不了
* **触屏驱动是最大工作量**，其余多为改地址
* 8+128 的存储够，但 QEMU 验证时 `ram.img` 会占 12 GB 稀疏文件（实际不占盘）

---

# 触摸修复记录（实测）

按 how-it-works 的原理执行：触摸必须是一个 **iOS 认得的 Apple 风格数字化仪**，
而不只是"能把坐标喂进去"。

## 修好之前

```
redwood: input: pair ... usage 读成 0
[TouchEvents] *** unexpected digitizer usage pair 0xD0000 ***
[TouchEvents] init: (unknown digitizer usage: 0)
[TouchEvents] *** still waiting for a main screen digitizer ***
[HID] ignoring connection; bundleID is missing
```

触摸事件**根本到不了 backboardd**：内核收到了（`used->idx` 从 0 涨到 11），
但 backboardd 不知道哪个设备是触摸屏，全部丢弃。

## 两处真实修复

### 1. 触摸屏少了它作为"设备"的身份（userland/src/inject.c）

`make_service()` 只公布了 `PrimaryUsagePage` / `PrimaryUsage` /
`DeviceUsagePairs` 等基础键，缺 `HIDEventServiceProperties` 这个**描述设备本身**的
子字典。backboardd 从中读不出"这是屏幕集成的多点触控数字化仪"，
于是落进 `unknown digitizer usage: 0` 分支，服务被保留但永不启用。

补上（iPhone X 的规格）：
`DisplayIntegrated`、`Multitouch`、`MaxFingerCount=11`、
`MaxInputReportSize=960`、`ReportInterval=8000`（125 Hz，与驱动轮询一致）。

**效果**：`unexpected digitizer usage pair` 与 `unknown digitizer usage` 消失，
`primaryUsage` 从 `0x0` 变成 `0x4`，`still waiting for a main screen digitizer` 归零。

### 2. 显示器少了 identity（xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp）

`displayUUID: 0x0` —— backboardd 要求数字化仪**指名它属于哪块屏幕**，
而 framebuffer 替代品没公布任何身份。给它加上 `ContainerID`
（`"REDWOOD-DISPLAY1"` 的 16 字节）。

## 修好之后

```
[TouchEvents] *** acquired main display digitizer (after 0.036761s) ***
[TouchEvents] init: <BKDirectTouchState: ...; touchScreen; builtin; ...>
[UISensor] instantiated <BKHIDUITouchDetectionModeDigitizerServiceWrapper: ...>
[UISensor] send mode (wakeOnTouch/0xFE) → (pocketTouchesExpected/0x1) → (on/0x0)
redwood: input: touch detection mode 0x1 / 0
SpringBoard; reason: SB + backlight-on; digitizerEnabled: YES; displayState: on
[TouchEvents] contact 1 presence: touching
[TouchEvents] contact 1 presence: none
```

**触摸从"完全不通"走到了"backboardd 完全接受"**：数字化仪被认作主屏触摸屏、
UISensor 开始下发模式（0xFE→0x1→0x0）、SpringBoard 的 `dispatchMode` 从
`ignoring` 变可用、每次点击都产生 `presence: touching/none`。

## 仍然没解决的（诚实记录）

**点击主屏图标不会打开应用。** 状态全对、事件全到，但 SpringBoard 不执行启动。
日志里唯一还异常的是 SpringBoard 的 `system-gestures` 触摸流创建时就是
`dispatchMode: ignoring`，之后再没重建；正常 iOS 在屏幕点亮时应重建为派发模式。

试过并**否决**的：给 HID 连接加 `bundleID`。它消掉了 `bundleID is missing`
的抱怨，但 `BKHIDUITouchDetectionModeDigitizerServiceWrapper` 从此不再出现
（0 次 vs 1 次），模式命令一个都不下发——比不改更糟。已回滚。

下一步该看的是 `bsd/dev/redwood_input.c` 里 `TOUCH_MODE_WAKE` 之后
SpringBoard 如何重新订阅 touch stream，那是"唤醒后第一次派发"的缺口。
