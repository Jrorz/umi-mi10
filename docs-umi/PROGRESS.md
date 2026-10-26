# 小米 10（umi / SM8250）HackiOS 移植进度报告

> 日期：2026-10-26
> 设备：Xiaomi Mi 10 (umi, SM8250, 8+128, MIUI V816 Android 14, BL 已解锁, 有 root)
> 结论：**iOS 16.5 完整启动到主屏幕**；触摸与按键仍不可用（本文档说明已确认的原因与下一步）

---

## 一、当前达成状态

| 里程碑 | 状态 | 证据 |
|---|---|---|
| 分区备份 | ✅ 完成 | `umi-backup/` 8 个分区，sha256 全部校验通过 |
| Mu-Silicium UEFI 移植 | ✅ 完成 | `SILICIUM_UEFI.fd` 3,145,728 字节（=0x300000）|
| BootShim | ✅ 完成 | 176 字节，含 FD_BASE=0x9FC00000 / FD_SIZE=0x300000 / PAYLOAD_BASE=0xF0000000 |
| UEFI 启动 | ✅ 完成 | `fastboot boot` → Project Silicium logo 显示在手机屏幕 |
| xnuloader 启动 | ✅ 完成 | USB 设备名变为 `xnuloader`，打印 `platform umi` |
| 内核启动 | ✅ 完成 | `Darwin Kernel Version 22.5.0 ... xnu-8796.121.2` |
| GICv3 / 定时器 | ✅ 完成 | `gic: v3, distributor ..., virtual timer on PPI 27` |
| 显示 | ✅ 完成 | `MiniIOMobileFramebuffer: display 1125x2436 on a panel of 1080x2340` |
| APFS 挂载 | ✅ 完成 | `apfs: volume 0 "System": 45143 files, 16548 directories` |
| launchd → SpringBoard | ✅ 完成 | **用户确认看到 iOS 主屏幕** |
| 触摸 | ❌ 不可用 | 见第五节 |
| 按键 / 电源键 | ❌ 不可用 | 见第四节 |

---

## 二、小米 10 的关键硬件参数（全部真机验证）

| 参数 | 值 | 依据 |
|---|---|---|
| FD_BASE | `0x9FC00000` | xbl.img ELF LOAD 段 `pa=0x9fc00000` |
| FD_SIZE | `0x300000` | 同上（`filesz=0x270000` 向上对齐）|
| PAYLOAD_BASE | `0xF0000000` | xnuloader 约定 |
| FB_BASE | `0x9C000000` | 设备树 `cont_splash_region@0x9C000000` |
| GIC distributor | `0x17A00000` | 设备树 `arm,gic-v3` |
| GIC redistributor | `0x17A60000` | 同上 |
| core_shift | `8` | 设备树 `cpu@0/100/.../700` |
| small_cores | `0x0f` | `capacity-dmips-mhz`（前 4 核 0x400，后 4 核 0x766）|
| 屏幕 | 1080 × 2340 | 设备树 + UEFI 实测 |
| SPMI arbiter | `0xc440000`，**版本 v5 (0x50010000)** | 安卓 dmesg |
| TLMM（引脚控制器）| **`0x0F500000`**（Kona SOUTH，长度 0x46000）| 参考项目 `UmiFtsBus.inc` |
| TLMM NORTH | `0x0F942000`（GPIO66，面板 TE）| 同上 |
| 触摸芯片 | **ST FocalTech FT3680**，I²C 地址 **0x49** | 安卓 dmesg `[ FTS ] fts_probe` |
| 触摸 I²C 引脚 | **SDA = GPIO36，SCL = GPIO37**（GPIO 位模拟）| 参考项目 |
| 触摸复位/中断 | GPIO38 / GPIO39 | 设备树 `fts,reset-gpio` / `fts,irq-gpio` |
| 触摸电源 | VDD = RPMh L1（ldoc1, 1800mV）；AVDD = GPIO69 | 设备树 `avdd-supply` / `vdd-supply` |
| 板级升压 | **PM8150B (SID2) 寄存器 0xC444 = 0x80** | 参考项目 attempt 80 |
| 按键 PON 块 | PM8150 SID0，寄存器 0x800，状态字节 `0x10` | 安卓 dmesg + 参考项目 |
| 音量键 | PM8150 SID0 **GPIO5**（寄存器 0xc500，低有效）| 参考项目 `UmiButtonsDxe` |

---

## 三、部署流程（每次改动的完整步骤）

**必须按此顺序，否则会覆盖内核 payload：**

```sh
# 1) 手机进入 fastboot，启动我们编译的 UEFI
fastboot boot out/umi-uefi.img
sleep 20
fastboot getvar product          # 应显示 xnuloader

# 2) 告诉 loader 接下来是根镜像（必须先做）
fastboot oem ramdisk

# 3) 传 2.3 GB 系统镜像（约 92 秒）→ 成为 md0
fastboot stage userland/system.apfs

# 4) 传内核 payload（内含 root.apfs → md1）
fastboot stage out/umi-final.bin

# 5) 启动
fastboot oem run
```

一键脚本：`.build/umi-run.sh <payload>`

**关键陷阱**：`oem ramdisk` 必须在大镜像之前。顺序反了会把 2.3GB 写到 `PAYLOAD_BASE`，覆盖内核，症状为 `no payload at f0000000`。

**恢复**：长按电源键 10 秒强制重启即可回安卓；未写入任何分区（`fastboot boot` 是非持久的）。

---

## 四、按键：已定位的确切错误

### 错误对照（参考项目 `UmiButtonsDxe`，真机验证过）

| 项 | 之前（错） | 正确值 | 后果 |
|---|---|---|---|
| GPIO 通道 | `0x0c6`（GPIO6）| **`0x0c5`（GPIO5）** | 音量键读错寄存器 |
| 电源键位 | `0x80`（KPDPWR 中断位）| **bit0（0x01）** | 电源键永远读不到 |
| 音量下位 | `0x40`（RESIN 中断位）| **bit1（0x02）** | 音量下永远读不到 |

**根因**：`0x80` / `0x40` 是 PON 块的**中断使能位**，而状态字节（寄存器 0x10）里**输入本身是 bit0 / bit1**。两套位完全不同。

### 参考项目的读法（权威）

```c
*Mask = ((Gpio & BIT0) ? 0 : BIT0) |      // 音量上：GPIO bit0 == 0（低有效）
        ((Pon  & BIT1) ? BIT1 : 0) |      // 音量下：PON bit1
        ((Pon  & BIT0) ? BIT2 : 0);       // 电源键：PON bit0

FindObserverChannel(0x008, &mPonChannel);   // PON 块
FindObserverChannel(0x0C5, &mGpioChannel);  // GPIO5
```

### 尚未确认的部分

键值修正后**仍未验证到键事件**。缺少的验证环节：
- 按键驱动是否成功找到 SPMI 通道（需要 `buttons: channel N is ...` 日志）
- SPMI 读是否成功（需要 `buttons: ... reads ... failed` 统计）

**日志通道问题**：`redwood_input_init` 在 `bsd_init` 早期运行，此时 `printf` 输出会丢失；而 `loader_log`（1.5MB）**loader 侧从不读取**，是死通道。这是本项目最大的调试障碍。

---

## 五、触摸：已定位的三个根本原因

### 原因 1：从未发送“开始扫描”命令（最关键）

参考项目 `UmiFtsSensing.h`，芯片必须收到这些命令才输出触摸事件：

```c
on[]      = {0xa0, 0x00, 0x01};            // 扫描开启
reports[] = {0xa2, 0x03, 0x00, 0x00, 0x00, 0x03};  // 事件上报开启
off[]     = {0xa0, 0x00, 0x00};            // 初始还需先关

// 启动序列：on → reports → off → 等 50ms → on
```

**之前的驱动只读事件，从不发这些命令，芯片完全静默。**

### 原因 2：事件协议用错

```c
// 正确（参考项目 UmiFtsBus.inc）
mLastFifoRead = WriteSize == 1 && Write[0] == 0x86 && ReadSize == 8;
// 即：发命令 0x86 → 收 8 字节

// 8 字节事件格式（UmiFtsCore.h umi_fts_parse）
ev[0]: 0=无事件, 3=复位, 0xf3=故障, (ev[0]&15)==3=状态字节, (ev[0]>>4)==3=抬起
id   = ev[1] >> 4
type = ev[1] & 15            // 只有 1 和 2 是触点
x = ((ev[3] & 15) << 8) | ev[2]
y = (ev[4] << 4) | (ev[3] >> 4)      // 注意 Y 的拼法

// 之前（错）：读寄存器 0x00 收 62 字节标准多点报告
```

### 原因 3：芯片身份校验方式错

```c
// 正确：读 208 字节 sysinfo（分 13 个 16 字节块，命令 A6 hi lo）
// 身份字段（固定偏移）
chip0 = le16(p+10)  必须 0x3639
fw    = le16(p+16)  必须 0x128
svn   = le16(p+18)  必须 0x4053
config= le16(p+20)  必须 0x4b
resX  = le16(p+80), resY = le16(p+82)

// 之前（错）：只读 2 字节 ID 比对 0x54 / 0x52
// 那是原始 I²C 探测值，不是 sysinfo 身份
```

### 另外两个已修正的关键错误

**a) TLMM 基址错了 1 MB**

```c
// 之前（redwood 的）  0x0F100000
// 正确（Kona SOUTH）  0x0F500000，长度 0x46000
```

`0x0F100000` 会让 GPIO38 落到**引脚 294** 的位置，所以复位脚、中断脚、电源脚全都没被操作过。

**b) 板级升压电源没开**

参考项目 attempt 79 记录 PM8150B GPIO5 是 `output-low`，97004 次轮询 **0 个触摸点**；attempt 80 写入 `SID2 0xC444 = 0x80` 后**触摸成功**。

写入前需校验 11 个寄存器的 profile（逐字节对照 `UmiBoardBoost.h`），只写高不写低。

---

## 六、16 MB `kmem_alloc` panic（已用二分定位）

### 现象

```
bsd_init: calling devfs_kernel_mount
bsd_init: calling siginit
cpu_signal failed in DebuggerXCallEnter   ×3
panic: kmem_alloc(..., 16777216, 0x100021): failed with 3 @vm_kern.c:178
```

### 二分结果

| 配置 | 结果 |
|---|---|
| **禁用整个 `redwood_input_init()`** | ✅ **能进 iOS 界面** |
| 启用它 | ❌ 16MB panic |

**结论：panic 来自输入子系统（`redwood_input_init` 里的某个驱动）。**

`redwood_input_init()` 里有 12 个驱动：

```c
redwood_cpufreq_init();      redwood_battery_init();
redwood_virtio_input_init(); redwood_ufs_init();
redwood_goodix_init();       redwood_net_init();
redwood_touch_i2c_init();    redwood_adreno_init();
redwood_touch_probe_init();  redwood_wifi_init();
redwood_buttons_init();      redwood_adsp_init();
```

有同步 `ml_io_map` 的三个（都有 boot-arg 保护，当前 cmdline 不含这些参数）：
`redwood_virtio_input_init`、`redwood_battery_init`、`redwood_ufs_init`

**下一步**：继续二分这 12 个，定位具体是哪一个。

---

## 七、日志通道问题（调试最大障碍）

| 通道 | 大小 | 是否可读 | 说明 |
|---|---|---|---|
| `printf` / `kprintf` | 16 KB 环形 | ⚠️ 只留最后几条 | `bsd_init` 早期的输出会丢失 |
| `loader_log`（1.5 MB）| 1.5 MB | ❌ **loader 从不读取** | 死通道 |
| `logdump` 分区 | 64 MB | ✅ 但只在运行结束时写入 | 崩溃/硬复位时不写 |

**可用方法**：
1. 让驱动**周期性打印**（每 5 秒），关机前最后一条会留在 16KB 环形里
2. 从 `/dev/block/by-name/logdump` 读取（安卓侧 `su -c 'dd'`）

**不可用**：`fastboot oem log` 在系统运行时——iOS 占用 USB，取不到。

---

## 八、修复清单（按优先级）

### P0：定位 16MB panic

继续二分 `redwood_input_init` 的 12 个驱动。参考项目用 C++ 驱动 + `IODeviceMemory::withRange` 而非 `ml_io_map`，可能是 `ml_io_map` 在内核虚拟内存紧张时的行为问题。

### P1：验证触摸的 sensing 命令

已完成代码（`fts_start_sensing`），但**未在真机验证**。需要看到：

```
fts: controller identified; panel 1080 x 2340
fts: scan started (A0 00 01, A2 03 ..., A0 00 00, 50 ms, A0 00 01)
fts: N polls, N irq-high, N non-empty, N events
```

### P2：按键验证

修正后需确认 `buttons:` 诊断输出，判断是通道未找到还是 SPMI 读失败。

### P3：参考项目的完整方案

`~/Downloads/UniXNU-umi-main` 是**专门为小米10做的、已跑通触摸的项目**（attempt 80 触摸成功）。它有：
- 完整 C++ 驱动（`package/touch/`）
- 编译脚本（`bringup/tools/build_umi_touch.py`）
- 50+ 单元测试
- 详细审计文档（`docs/TOUCH_NATIVE_AUDIT.md`）

**建议**：直接采用它的编译产物，而不是继续手工翻译成 C。

---

## 九、参考文献与数据来源

| 用途 | 来源 |
|---|---|
| 移植方法论 | https://hackiosx.dev/porting.html |
| 小米10 已跑通的移植 | `~/Downloads/UniXNU-umi-main`（README: iPadOS 17.7.11 到 Hello；触摸 attempt 80 确认可用）|
| FTS 触摸协议 | 该项目 `package/touch/UmiFts*.h`、`UmiFtsBus.inc` |
| 按键协议 | 该项目 `sources/uefi-tmp/Platforms/Xiaomi/umiPkg/Drivers/UmiButtonsDxe/UmiButtons.c` |
| SM8250 GCC 时钟 | Linux `drivers/clk/qcom/gcc-sm8250.c` |
| SM8250 时钟索引 | Linux `include/dt-bindings/clock/qcom,gcc-sm8250.h` |
| FocalTech I²C 协议 | `KONA-DEVS/kernel_xiaomi_sm8250` `drivers/input/touchscreen/focaltech_touch/` |
| 硬件参数 | 手机自身 `/proc/device-tree`（最权威）|
| 安卓实际驱动行为 | 手机 `dmesg`（有 root）|

---

## 十、文件位置

| 内容 | 路径 |
|---|---|
| 分区备份（143 MB）| `umi-backup/` |
| 设备树反编译 | `umi.dts`（41652 行）|
| UEFI 固件 | `Mu-Silicium/Build/umiPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd` |
| Boot image | `out/umi-uefi.img` |
| 内核 payload（最终版）| `out/umi-final.bin` |
| 系统镜像 | `userland/system.apfs`（2.3 GB）、`userland/root.apfs`（4 MB）|
| 触摸驱动 | `xnu/files/bsd/dev/redwood_touch_i2c.c` |
| 按键驱动 | `xnu/files/bsd/dev/redwood_buttons.c` |
| 设备配置 | `common/devices/umi.conf`、`umi.platform.c` |
| 部署脚本 | `.build/umi-run.sh` |
| 构建说明 | `.build/README-umi.md` |
