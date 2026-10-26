# 小米 10 (umi) 真机移植记录 —— 2026-10-25

## 已达成（真机实测）

| 里程碑 | 证据 |
|---|---|
| **备份全部分区** | 8 分区 sha256 逐一校验（`umi-backup/`，143 MB）|
| **UEFI 构建** | `SILICIUM_UEFI.fd` 3,145,728 字节 = 0x300000 |
| **BootShim** | 176 字节，含 FD_BASE=0x9fc00000 / FD_SIZE=0x300000 / XNUP / PAYLOAD_BASE=0xf0000000 |
| **UEFI 启动** | `fastboot boot` → `Booting OKAY` |
| **loader 启动** | USB 设备名变成 `xnuloader` |
| **平台识别** | `platform umi`、`display 1080 x 2340 at 9c000000` |
| **内核启动** | `Darwin Kernel Version 22.5.0 ... jrace:xnu-8796.121.2` |
| **GICv3** | `gic: v3, distributor ...cpu 0's redistributor... virtual timer on PPI 27` |
| **显示驱动** | `MiniIOMobileFramebuffer: display 1125x2436 on a panel of 1080x2340` |
| **VM 子系统** | `vm_page_bootstrap: 387305 free pages` ≈ 6 GB |
| **等根设备** | `Still waiting for root device`（指南第 5 步的验收标准）|

## 关键参数（全部真机验证）

| 参数 | 值 | 怎么确定的 |
|---|---|---|
| FD_BASE | 0x9FC00000 | xbl.img 的 ELF LOAD 段 `pa=0x9fc00000` + alioth 的 MemoryMapLib |
| FD_SIZE | 0x300000 | 同上（`filesz=0x270000`，对齐到 3 MB）|
| FB_BASE | 0x9C000000 | 设备树 `cont_splash_region@9c000000` |
| GIC | 0x17A00000 / 0x17A60000 | 设备树 `arm,gic-v3` |
| core_shift | 8 | 设备树 `cpu@0/100/.../700` |
| small_cores | 0x0f | `capacity-dmips-mhz` 前 4 核 0x400，后 4 核 0x766 |
| 屏幕 | 1080 × 2340 | 设备树 + UEFI 报告 |

## 已知问题

### 1. PSCI 启动大核失败（8 核时）
```
core 700: the firmware answers 0 to the request to start it
```
`cpus=8` 时内核停在单核（实际是 bootcpu=7 失败后回退）。
**单核 `cpus=1` 时可以完整启动**，符合 porting 指南第 4 步"先单核再上多核"。

### 2. `oem ramdisk` 顺序陷阱
必须先 `fastboot oem ramdisk`（告诉 loader 接下来是镜像），再 `fastboot stage <img>`。
顺序反了会把 2.3 GB 镜像写到 PAYLOAD_BASE 覆盖内核 payload，症状：
```
no payload at f0000000 (magic 3b6537a0)
```

## 下一步

1. 正确顺序装系统镜像 → 看能否到锁屏
2. 修 PSCI 多核（对照 redwood 的 `small_cores_after` 和 PSCI 调用方式）
3. 触屏（UMI 的芯片型号要从设备树确认）

## 2026-10-25 下午：iOS 主屏幕已在真机运行

**用户确认：小米 10 屏幕上出现了 iOS 16.5 主屏幕。** 触摸和按键不可用（预期，见下）。

### 完整成功链路

```
apfs: volume 0 "System" of container 0: 45143 files, 16548 directories, 605725 blocks
BSD root: md0 -> bsd_init: done
load_init_program: /sbin/launchd
→ SpringBoard → 主屏幕
```

### 关键：mi 10 与 redwood 的 PMIC 差异（按键不工作的原因）

| | redwood (SM7325) | umi (SM8250) |
|---|---|---|
| power-on 块 | PMK8350 @ **0x1300** (periph 0x13) | PM8150 @ **0x0800** (periph 0x08) |
| 音量键 GPIO | PM7325 GPIO6 (periph 0x8d) | PM8150 GPIO6 (periph 0xc6) |
| SPMI arbiter | 0xc440000 / 0xe600000 | **相同** |

SPMI 总线和 PWM bit（KPDPWR=0x80, RESIN=0x40）通用，只需换外设 id。
已改 `redwood_buttons.c`：先试 PM8150 布局，找不到再回退 PMK8350（redwood 不受影响）。

### 触摸芯片（尚未适配）

```
umi.dts: goodix@5d, compatible = "goodix,gt9889"
  reg = <0x5d>                     I²C 地址 0x5D
  goodix,reset-gpio = <&tlmm 38>   GPIO 38
  goodix,irq-gpio   = <&tlmm 39>   GPIO 39
  panel-max-x = 1079, panel-max-y = 2339
  cfg-name = "goodix_gt9886_cfg_J2.bin"
```

redwood 的 `redwood_goodix.c` 是 **GT9916 走 SPI 位模拟**，UMI 是 **GT9886 走 I²C**——
完全不同的总线，需要新驱动。

## 真机上装镜像的正确流程（实测）

```sh
fastboot boot out/umi-uefi.img          # UEFI + loader
fastboot oem ramdisk                    # ① 必须先告诉 loader
fastboot stage userland/system.apfs     # ② 2.3 GB，约 91 秒 → md0
fastboot stage out/umi-<k>.bin          # 内核 payload（内含 root.apfs → md1）
fastboot oem run                        # 启动
```

**顺序 ① 在 ② 之前**，否则大镜像会覆盖 PAYLOAD_BASE。
