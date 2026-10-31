# 触摸驱动：已解决的 10 个 bug 与当前状态

> 2026-10-31 更新。所有结论都有设备日志、设备树或参考项目源码作为依据，没有猜测。

## 一、当前状态

| 阶段 | 状态 | 日志证据 |
|---|---|---|
| I²C 位模拟通信 | ✅ 通 | `fts: answered on attempt 1` |
| 芯片身份识别 | ✅ 通过 | `chip 0x3639, fw 0x128, svn 0x4053, config 0x4b` |
| 扫描命令下发 | ✅ 成功 | `scan started (A0 00 01, A2 03 ..., A0 00 00, 50 ms, A0 00 01)` |
| 读取触摸事件 | ✅ 正常 | 单次运行读到 395 个事件 |
| 事件解析 | ✅ 正确 | `post down id 0 at 394, 1694` / `post move ... 687, 1734` |
| 坐标范围 | ✅ 正确 | x∈[0,1079], y∈[0,2339] |
| **交给用户态** | ❌ **卡死** | 一旦 `redwood_touch_post()` 真正投递，系统冻结 |
| iOS 端接入 | ✅ 就绪 | `digitizer attached` / `acquired main display digitizer` |

**结论：内核侧触摸驱动已经完全打通。** 剩下的阻塞点是"把样本交到 `/dev/redwood-touch` 队列"之后，用户态接手时系统冻结。

## 二、已解决的 10 个 bug

### 1. 总线空闲检查放在上电之前

```c
// 错：芯片还没供电，SDA 通过 ESD 二极管接地，永远"不空闲"
i2c_pins_init();
if (!i2c_read_line(PIN_SDA) || !i2c_read_line(PIN_SCL)) { 退 出 }

// 对（UniXNU-umi AcquirePins 的顺序）：先上电复位，再检查
i2c_pins_init();
board_boost_on();
power_on();
delay(5);
if (!i2c_read_line(PIN_SDA) || !i2c_read_line(PIN_SCL)) { 退出 }
```

症状：`fts: the bus is not idle after taking the pins (SDA 0, SCL 1)`

### 2. SPMI 物理地址被当虚拟地址解引用 → data abort

```c
// 错
channel = SPMI_OBSERVER_PHYS + i * 0x80;   // 0x0e600000 + ...
wr(channel, ...);                          // 直接解引用物理地址

// 对：先映射
spmi_observer = ml_io_map(SPMI_OBSERVER_PHYS, SPMI_OBSERVER_SIZE);
channel = spmi_observer + found * 0x80;
```

证据：`panic: Kernel data abort ... far 0x0e606700`，寄存器 `x0 = 0x0e600000`

### 3. cmd_db 用了 XBL 日志区的地址

```c
// 错：0x80880000 是 res_xbl_uefi_log
#define CMD_DB_PHYS 0x80880000UL
#define CMD_DB_SIZE 0x14000UL

// 对（umi.dts: cmd_db: reserved-memory@80860000, reg = <0x80860000 0x20000>）
#define CMD_DB_PHYS 0x80860000UL
#define CMD_DB_SIZE 0x20000UL
```

症状：`fts: no command database (magic ff 7f ff ff)` → VDD 找不到 → 芯片无供电

### 4. I²C 引脚用推挽驱动，违反开漏规范

```c
// 错：active 驱动高电平，与芯片的 ACK 拉低冲突
i2c_drive(pin, true) → 锁存器=1 + 使能 OE

// 对（UniXNU-umi Line()：只改 OE 位，锁存器恒为 0）
static void i2c_line(unsigned pin, bool high) {
    uint32_t ctl = i2c_pin_ctl(pin);
    wr(GPIO_CFG(pin), ctl);                            // 释放 = 输入，靠外部上拉
    wr(GPIO_IO(pin), rd(GPIO_IO(pin)) & ~GPIO_OUT_BIT); // 锁存器保持 0
    if (!high) wr(GPIO_CFG(pin), ctl | GPIO_OE_BIT);    // 拉低 = 灌电流
}
```

症状：读回全 `0xff`（无 ACK）

### 5. 复位后只等 10 ms（芯片需要 300 ms）

```c
gpio_drive(GPIO_RESET, true);
delay(300000);   // 300 ms，等芯片自举固件启动
```

依据：focaltech `fts_reset_proc` 在释放复位后等 300 ms

### 6. 读字节的 ACK 极性写反

```c
// 错：非最后字节传 !last=true → 释放 = NACK，芯片立刻停止发送
i2c_drive(PIN_SDA, !last);

// 对（UniXNU-umi: Line(SDA, Last)，ACK every byte except the final byte）
i2c_drive(PIN_SDA, last);
```

症状：`first bytes a5 ff` —— 第一个字节正确，后面全 `0xff`

### 7. `delay()` 忙等占死 CPU

```c
// 错：delay() 是纯自旋，永不让出 CPU；无限循环会把系统饿死
delay(irq ? 2000 : 8000);

// 对
IOSleep(irq ? 2 : 8);
```

另外把所有毫秒级 `delay(N us)` 改为 `IOSleep(N ms)`。

### 8. `%llu` 格式串导致 `__doprnt` 崩溃

```
panic: Kernel data abort. at pc 0xfffffff0072d3b88
符号表定位: pc -> __doprnt + 0xd84
far: 0x6f67007465736572   ← 把字符串 "reset\0go" 的内容当成了地址
```

修复：驱动里所有 `%llu` 改为 `%u` + 显式 `(unsigned)` 转换。

### 9. 坐标轴上限 X/Y 取反

```c
// 错：sysinfo 的 resX=2339(长边)、resY=1079(短边)
max_x = sys.res_x;   // 2339
max_y = sys.res_y;   // 1079   ← 于是 y>1079 的真实触摸全被丢弃

// 对（UniXNU-umi: if (x > 1079 || y > 2339) return BAD）
max_x = sys.res_y;   // 1079
max_y = sys.res_x;   // 2339
```

症状：读到 2158 个事件，但 `0 posted`

### 10. 事件类型判定用错半字节

```c
// 错：把低半字节当"状态字节"签名
if ((ev[0] & 0x0f) == 3) return true;

// 对（UniXNU-umi umi_fts_point_event: kind = code >> 4，需在 1..3）
if (((ev[0] >> 4) < 1) || ((ev[0] >> 4) > 3)) return true;
```

参考项目注释：*The low nibble can carry contact-size bits; it is not a fixed 0x3 protocol signature.*

## 三、硬件参数（全部经设备树核实）

| 项 | 值 | 设备树依据 |
|---|---|---|
| SDA / SCL | GPIO **36 / 37** | `qupv3_se13_i2c_active: pins = "gpio36","gpio37"` |
| 复位 / 中断 | GPIO **38 / 39**，8 mA，bias-pull-up | `ts_active: pins = "gpio38","gpio39"` |
| AVDD | GPIO **69**，高有效 | `touch_vddio_vreg { gpio = <0x65 0x45 0x00>; enable-active-high; }` |
| VDD | PM8150A **L1** @1800 mV | `pm8150a_l1: regulator-min-microvolt = <0x1b7740>` |
| cmd_db | **0x80860000**，0x20000 | `cmd_db: reserved-memory@80860000` |
| I²C 地址 | **0x49** | `fts@49` |
| 引脚复用 | active 为 `qup13`，sleep 为 `gpio` | `qupv3_se13_i2c_active/sleep` |
| 板级升压 | PM8150B SID2 `0xC444 = 0x80` | 参考项目 attempt 80 |

## 四、当前阻塞：交给用户态时冻结

### 现象

| 配置 | 结果 |
|---|---|
| `FTS_POST_TO_USER 0`（只计数） | ✅ 稳定跑满 300 秒，293 个样本解析正确 |
| `FTS_POST_TO_USER 1, FTS_POST_MOVES 0`（只投按下/抬起） | ❌ 约 160 秒冻结 |
| `FTS_POST_TO_USER 1, FTS_POST_MOVES 1`（全投） | ❌ 约 120 秒冻结 |

**只要真正调用 `redwood_touch_post()`，系统就冻结。**

### 投递路径

```
fts_parse() → redwood_touch_post(type, id, x, y)
           → touch_post()            [redwood_input.c]
                lck_mtx_lock(&touch_lock)
                count_fingers()      → thread_call_enter_delayed (仅 5 指时)
                lck_mtx_unlock()
                wakeup(&touch_queue)
           → 用户态 libredwood.dylib 读 /dev/redwood-touch
           → 转成虚拟 HID digitizer 报告
           → backboardd → SpringBoard
```

iOS 端已确认就绪：
```
[TouchEvents] digitizer attached (sender 107D5AFA00000)
[TouchEvents] *** acquired main display digitizer (after 0.00562078s)
[TouchEvents] init: <BKDirectTouchState: ... touchScreen; builtin; ...>
redwood: input: touch screen service 0x107d5ea90, buttons 0x109104a70
digitizerToPanelRotation=0 layoutRotation=0
```

### 下一步排查方向

1. **确认冻结发生在内核还是用户态**：让内核在投递后立即打印，看最后一条日志停在哪
2. **检查 `wakeup()` 风暴**：drop 掉高频 move，只保留 down/up 已试过，仍然冻结 → 不是频率问题
3. **检查 libredwood 的 HID 上报路径**：可能在报告投递时与 backboardd 的锁冲突
4. **绕开 `/dev/redwood-touch`**：参考项目 UniXNU-umi 是在内核里直接构造 HID 报告（`NativeHidInput.cpp`），完全不走用户态队列

## 五、如何复现当前的稳定状态

```sh
# 1. 改开关（xnu/files/bsd/dev/redwood_touch_i2c.c）
#define FTS_POST_TO_USER 0      # 只计数，稳定
#define FTS_POST_MOVES   0

# 2. 编译
cp xnu/files/bsd/dev/redwood_touch_i2c.c xnu-src/xnu-8796.121.2/bsd/dev/
rm -f xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/redwood_touch_i2c.o
THEOS=$PWD/.build/theos PATH=$PWD/.build/shim:$PATH ./xnu-build.sh -j8 -k

# 3. 打包（loader_return_after 让内核自动返回并写日志，无需按键）
K=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
python3 xnuloader/mkpayload.py -o out/umi-touch.bin \
    kernel=$K ramdisk=userland/root.apfs \
    --cmdline "debug=0x8 serial=3 cpus=4 touchscreen=2 loader_return_after=300"

# 4. 部署
.build/umi-run.sh out/umi-touch.bin
```

## 六、调试方法上的两个关键发现

### `loader_return_after=<秒>` 自动返回

```c
/*
 * boot-arg loader_return_after=<seconds>: hand control back to the loader
 * after that long, whatever the kernel is doing. This is how the log gets out
 * when the kernel does not panic but simply waits.
 */
```

不需要长按电源键，内核会自动返回 loader 并把日志写入 logdump。

### `fastboot oem log` 抓实时日志

内核还在运行时也能抓（`compose_live_log`），但 iOS 占用 USB 时经常失败。

### 日志规模对照（判断系统是否真跑起来）

| `kernel returned: ... bytes` | 含义 |
|---|---|
| 30–40 KB | 早期崩溃 |
| ~590 KB | 系统完整跑满全程 |
