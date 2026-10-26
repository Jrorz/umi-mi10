/*
 * The touch screen of the Xiaomi Mi 10 (umi): an ST FocalTech FT3680 on the I2C bus of GENI
 * serial engine 4 (a94000), as a source of touches for /dev/redwood-touch (sys/redwood_input.h).
 *
 * Boot-arg touchscreen=2 (touchscreen=1 is redwood_goodix.c's SPI chip; touchscreen=3 is the
 * read-only probe of this same engine).
 *
 * WHICH CHIP THIS PHONE HAS, and how that was settled. umi.dts carries two touch nodes,
 * goodix@5d and fts@49, and `qcom,i2c-touch-active = "st,fts"` says which one is wired. The
 * phone's own Android kernel log is the proof:
 *
 *   [GTP-ERR][goodix_ts_probe:3273] goodix device confirm failed[skip]
 *   [GTP-INF][goodix_ts_probe:3436] goodix_ts_probe OUT, r:-22      <- no Goodix there
 *   [ FTS ] fts_probe: Probe start
 *   input: fts as /devices/platform/soc/a94000.i2c/i2c-3/3-0049/input/input5
 *   fts 3-0049: Linked as a consumer to regulator.37 / regulator.77
 *
 * So: FocalTech at I2C address 0x49, not Goodix at 0x5d.
 *
 * EVERY constant below has a source:
 *
 *   umi.dts fts@49                       address 0x49, IRQ GPIO 39, reset GPIO 38, panel 1080x2340
 *   focaltech_core.h / focaltech_common.h the register map (UMI's kernel, focaltech_touch/)
 *     (KONA-DEVS/kernel_xiaomi_sm8250)
 *   focaltech_i2c.c of the same driver    the framing: one address byte out, then the read; a
 *                                         write is {addr, value}
 *   focaltech_core.c of the same driver   the report: register 0x00, count in byte 2, six bytes
 *                                         per contact, and the power/reset sequence
 *   i2c-qcom-geni.c of the same kernel    the I2C side of the GENI engine (as this file had it)
 *   redwood_touch_i2c.c's read-only probe what the engine reports on this phone
 *
 * Deliberately NOT done here, because on this phone they are not safe or not known:
 *
 *   - No writes to GCC. The clock indices in umi.dts (0x74/0x86/0x87) resolve to the same WRAP1
 *     SE5 engine redwood drives, but SM8250's register layout was never checked, and writing a
 *     clock controller blind is what stopped this machine. The engine's clock is left as UEFI
 *     and the boot loader set it.
 *   - No write to GPIO 69. umi.dts calls that regulator "disp_vddio_vreg": the display's I/O
 *     supply. Driving it during bsd_init took the machine down.
 *   - The chip's own supplies are left on, as they are (Android leaves avdd and vdd enabled for
 *     the panel); the driver only resets the chip and talks to it.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_line(const char *line);

/* Diagnostics go to the loader's 1.5 MB log as well as to the kernel's small ring. */
static int say(const char *format, ...) __printflike(1, 2);
static int
say(const char *format, ...)
{
	char line[192];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
	return (printf)("%s", line);
}
#define printf say

/* ---- the addresses (umi.dts: i2c@a94000, tlmm, gcc@100000) ------------------------------------- */

#define SE_PHYS                 0x00a94000UL
#define SE_SIZE                 0x4000UL
/*
 * The pin controller is Kona's SOUTH block, and neither of the two bases tried before was it.
 *
 * Source: UniXNU-umi's package/touch/UmiFtsBus.inc, the XNU build whose owner confirmed working
 * touch on this handset:
 *
 *   #define TLMM_SOUTH  0x0F500000U
 *   #define GPIO_CTL(N) (TLMM_SOUTH + (N) * 0x1000U)
 *   #define GPIO_IO(N)  (GPIO_CTL(N) + 4U)
 *
 * and its region table maps {0x0f500000, 0x46000} for this -- 0x46000 being 70 pages, exactly
 * enough for pin 69's registers at +0x45000. A second block at 0x0f942000 is Kona's NORTH one
 * (pin 66, the panel's TE input), which is why a single base never covered the pins used here.
 *
 * What this file had: 0x0f100000 (redwood's), then 0x0f000000 (the pinctrl node's reg property,
 * which is the address the SoC's pinctrl *device* answers at, not the pin registers). With the
 * first, GPIO38 lands on pin 294; with the second, the machine stopped bringing up. Both left
 * every pin this driver touched inert.
 */
#define TLMM_PHYS               0x0f500000UL
#define TLMM_SIZE               0x46000UL
#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x60000UL
#define RSC_PHYS                0x18220000UL    /* umi.dts apps_rsc: drv-2 */
#define RSC_SIZE                0x10000UL
#define CMD_DB_PHYS             0x80880000UL    /* umi.dts cmd-db@80880000 */
#define CMD_DB_SIZE             0x14000UL

static vm_offset_t se, tlmm, gcc, rsc, cmd_db;

static inline uint32_t rd(vm_offset_t address) { return *(volatile uint32_t *)address; }
static inline void wr(vm_offset_t address, uint32_t value) { *(volatile uint32_t *)address = value; }

/* ---- GENI, I2C side (i2c-qcom-geni.c; the register offsets are redwood_touch_i2c.c's) ----------- */

#define GENI_FORCE_DEFAULT      (se + 0x020)
#define GENI_OUTPUT_CTRL        (se + 0x024)
#define GENI_CGC_CTRL           (se + 0x028)
#define GENI_STATUS             (se + 0x040)    /* bit 0: a command is running */
#define GENI_SER_M_CLK_CFG      (se + 0x048)
#define GENI_IF_DISABLE_RO      (se + 0x064)
#define GENI_FW_REVISION_RO     (se + 0x068)    /* [15:8] protocol: 0 = I2C, 1 = SPI */
#define GENI_CLK_SEL            (se + 0x07c)
#define GENI_BYTE_GRAN          (se + 0x254)
#define GENI_DMA_MODE_EN        (se + 0x258)
#define GENI_TX_PACKING_CFG0    (se + 0x260)
#define GENI_RX_PACKING_CFG0    (se + 0x284)
#define GENI_M_CMD0             (se + 0x600)    /* [31:27] opcode, [15:9] slave address */
#define GENI_M_IRQ_STATUS       (se + 0x610)
#define GENI_M_IRQ_EN           (se + 0x614)
#define GENI_M_IRQ_CLEAR        (se + 0x618)
#define GENI_S_IRQ_CLEAR        (se + 0x638)
#define GENI_TX_FIFO            (se + 0x700)
#define GENI_RX_FIFO            (se + 0x780)
#define GENI_TX_FIFO_STATUS     (se + 0x800)
#define GENI_RX_FIFO_STATUS     (se + 0x804)
#define GENI_TX_WATERMARK       (se + 0x80c)
#define GENI_RX_WATERMARK       (se + 0x810)
#define GENI_RX_RFR_WATERMARK   (se + 0x814)
#define SE_GSI_EVENT_EN         (se + 0xe18)
#define SE_IRQ_EN               (se + 0xe1c)
#define SE_HW_PARAM_0           (se + 0xe24)    /* [21:16] FIFO depth in words */

#define SE_I2C_TX_TRANS_LEN     (se + 0x26c)
#define SE_I2C_RX_TRANS_LEN     (se + 0x270)
#define SE_GENI_IOS             (se + 0x908)    /* bit 1: the transfer ended with a NACK */

#define GENI_I2C_OP_READ        0x2
#define GENI_I2C_OP_WRITE_READ  0x3
#define GENI_SLV_ADDR_SHIFT     9               /* SLV_ADDR_MSK = GENMASK(15, 9) */
#define GENI_CMD_DONE           0x2

static uint32_t fifo_depth;

/* ---- the chip: focaltech_core.h and focaltech_common.h ----------------------------------------- */

#define FTS_I2C_ADDR            0x49            /* umi.dts: fts@49 */
#define FTS_REG_CHIP_ID         0xA3
#define FTS_REG_CHIP_ID2        0x9F
#define FTS_REG_FW_VER          0xA6
#define FTS_REG_INT_ACK         0x3E
#define FTS_REG_WORKMODE        0x00

/* focaltech_core.h: every field of a contact is read at its base, base = FTS_ONE_TCH_LEN * i */
#define FTS_TOUCH_POINT_NUM     2               /* buf[2] & 0x0F: how many contacts */
#define FTS_ONE_TCH_LEN         6               /* bytes per contact */
#define FTS_TOUCH_EVENT_POS     3               /* [7:6] the event */
#define FTS_TOUCH_X_H_POS       3               /* [3:0] of the same byte: x's high nibble */
#define FTS_TOUCH_X_L_POS       4
#define FTS_TOUCH_Y_H_POS       5
#define FTS_TOUCH_Y_L_POS       6
#define FTS_TOUCH_ID_POS        5               /* [7:4] of the same byte: the id */
#define FTS_MAX_POINTS          10              /* FTS_MAX_POINTS_SUPPORT */
#define FTS_POINT_BUF_LEN       (2 + FTS_ONE_TCH_LEN * FTS_MAX_POINTS)

#define FTS_TOUCH_DOWN          0
#define FTS_TOUCH_UP            1
#define FTS_TOUCH_CONTACT       2

/*
 * The event protocol of this controller, as UniXNU-umi's working build has it
 * (package/touch/UmiFtsBus.inc: "mLastFifoRead = WriteSize == 1 && Write[0] == 0x86 &&
 * ReadSize == 8"; UmiFtsCore.h: umi_fts_parse). It is NOT the multi-touch report this file
 * assumed: one command byte 0x86 brings back eight bytes, and those eight bytes are
 *
 *   ev[0]        0 = nothing; 3 = controller reset; 0xf3 = fault; (ev[0] & 15) == 3 = ignore
 *                otherwise a contact, and (ev[0] >> 4) == 3 means it has lifted
 *   ev[1]        id = ev[1] >> 4, type = ev[1] & 15 (only 1 and 2 are contacts)
 *   x            ((ev[3] & 15) << 8) | ev[2]
 *   y            (ev[4] << 4) | (ev[3] >> 4)
 *
 * Their identity check (UmiFtsStartup.h) wants chip 0x3639, fw 0x128, svn 0x4053, config 0x4b.
 * 0x54/0x52 is what the raw probe returns before sysInfo is read, which is why this file asked
 * for the wrong one.
 */
#define FTS_CMD_READ_EVENT      0x86
#define FTS_EVENT_LEN           8
#define FTS_EV_RESET            0x03
#define FTS_EV_FAULT            0xf3
#define FTS_ID_CHIP0            0x3639
#define FTS_ID_FW               0x128
#define FTS_ID_SVN              0x4053
#define FTS_ID_CONFIG           0x4b

/* UniXNU-umi's READY window: fts_521 GENERAL_TIMEOUT, 100 ms */
#define FTS_READY_TIMEOUT_US    100000
#define FTS_READY_POLL_US       2000

/* ---- the pins (umi.dts: fts,reset-gpio = <&tlmm 38>, fts,irq-gpio = <&tlmm 39>) ---------------- */

#define GPIO_RESET              38
#define GPIO_IRQ                39
#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))
#define GPIO_IO(n)              (tlmm + 0x1000UL * (n) + 4)
#define GPIO_OE_BIT             (1u << 9)       /* drive enable, in the control register */
#define GPIO_OUT_BIT            (1u << 1)       /* the level driven, in the io register */
#define GPIO_IN_BIT             (1u << 0)       /* the level read, in the io register */
/* the field masks, from UniXNU-umi's UmiFtsBus.inc */
#define GPIO_FUNC_MASK          0x3cu           /* [5:2] the pin's mux */
#define GPIO_PULL_MASK          0x3u            /* [1:0] pull */
#define GPIO_DRIVE_MASK         0x1c0u          /* [8:6] drive strength */

#define PULL_NONE               0
#define PULL_UP                 3

/*
 * The pin helpers, matching UniXNU-umi's UmiFtsPinConfig.h and DrivePowerPin() exactly:
 *
 *   ctl &= ~(0x3c | 1<<9)         the mux and the output enable
 *   if pin == 38: ctl = (ctl & ~0x1c3) | 3 | (3<<6)    pull-up, 8 mA (the one pin that differs)
 *   if ctl & 1<<11: ctl |= 1<<12                       eGPIO, as Linux pinctrl-msm wants
 *
 * and driving a pin is three writes in their order: the control word without output enable,
 * the level, then the enable. Setting the level before the pin is an output is what keeps it
 * from ever driving the wrong one.
 */
static uint32_t
gpio_power_ctl(unsigned pin)
{
	uint32_t ctl = rd(GPIO_CFG(pin));

	ctl &= ~(GPIO_FUNC_MASK | GPIO_OE_BIT);
	if (pin == GPIO_RESET) {
		ctl = (ctl & ~0x1c3u) | 3u | (3u << 6);      /* pull-up, drive 8 mA */
	}
	if ((ctl & (1u << 11)) != 0) {
		ctl |= 1u << 12;                             /* eGPIO */
	}
	return ctl;
}

static void
gpio_drive(unsigned pin, bool high)
{
	uint32_t ctl = gpio_power_ctl(pin);

	wr(GPIO_CFG(pin), ctl);
	wr(GPIO_IO(pin), (rd(GPIO_IO(pin)) & ~GPIO_OUT_BIT) | (high ? GPIO_OUT_BIT : 0));
	wr(GPIO_CFG(pin), ctl | GPIO_OE_BIT);
}

static bool
gpio_get(unsigned pin)
{
	return (rd(GPIO_IO(pin)) & GPIO_IN_BIT) != 0;
}

/* ---- the I/O supply (RPMh): as redwood_goodix.c, with umi's regulator name ---------------------- */

#define TCS(reg)                (rsc + 0xd00 + (reg))           /* command slot set 0, an "active" one */
#define TCS_IRQ_ENABLE          0x00
#define TCS_IRQ_STATUS          0x04
#define TCS_IRQ_CLEAR           0x08
#define TCS_WAIT_FOR_COMPLETION 0x10
#define TCS_CONTROL             0x14
#define TCS_CONTROL_ENABLE      (1u << 16)
#define TCS_CONTROL_TRIGGER     (1u << 24)
#define TCS_STATUS              0x18            /* 0 while busy */
#define TCS_CMD_ENABLE          0x1c
#define TCS_CMD_MSGID(n)        (0x30 + 20 * (n))
#define TCS_CMD_ADDR(n)         (0x34 + 20 * (n))
#define TCS_CMD_DATA(n)         (0x38 + 20 * (n))
#define MSGID_WRITE_4_BYTES     0x10008
#define MSGID_RESPONSE          0x100

#define VRM_VOLTAGE             0               /* a regulator's registers, from its address: millivolts */
#define VRM_ENABLE              4
#define VRM_MODE                8
#define VRM_MODE_LDO_HPM        7

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

/* The RPMh address of a resource by its name in the command database the boot loader left; 0 = none. */
static uint32_t
rpmh_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db;
	char id[8] = { 0 };

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		printf("fts: no command database (magic %02x %02x %02x %02x)\n", db->magic[0], db->magic[1],
		    db->magic[2], db->magic[3]);
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id == 0 || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
				return entry[i].address;
			}
		}
	}
	return 0;
}

static bool
rpmh_wait(uint32_t reg, uint32_t mask, uint32_t value, const char *what)
{
	for (int i = 0; i < 2000; i++) {
		if ((rd(TCS(reg)) & mask) == value) {
			return true;
		}
		delay(100);
	}
	printf("fts: RPMh: timeout waiting for %s (register %#x = %#x)\n", what, reg, rd(TCS(reg)));
	return false;
}

/* Send up to three register writes to RPMh and wait until they are done. */
static bool
rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	uint32_t irq_enable = rd(TCS(TCS_IRQ_ENABLE));
	bool done;

	for (int i = 0; rd(TCS(TCS_STATUS)) == 0; i++) {
		if (i > 2000) {
			printf("fts: RPMh: the command slots stay busy\n");
			return false;
		}
		delay(100);
	}
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	for (unsigned i = 0; i < count; i++) {
		wr(TCS(TCS_CMD_MSGID(i)), MSGID_WRITE_4_BYTES | MSGID_RESPONSE);
		wr(TCS(TCS_CMD_ADDR(i)), address[i]);
		wr(TCS(TCS_CMD_DATA(i)), data[i]);
	}
	wr(TCS(TCS_WAIT_FOR_COMPLETION), (1u << count) - 1);
	wr(TCS(TCS_CMD_ENABLE), (1u << count) - 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable | 1);
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CONTROL), TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);

	done = rpmh_wait(TCS_IRQ_STATUS, 1, 1, "the request");
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(TCS_CONTROL), rd(TCS(TCS_CONTROL)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_CMD_ENABLE), 0);
	wr(TCS(TCS_WAIT_FOR_COMPLETION), 0);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable);
	return done;
}

/* The chip's I/O supply: 1.8 V. umi.dts: iotouch-supply = <rpmh-regulator-ldoc1>. */
static bool
io_supply_on(void)
{
	uint32_t base = rpmh_address("ldoc1");
	uint32_t address[3] = { base + VRM_VOLTAGE, base + VRM_ENABLE, base + VRM_MODE };
	uint32_t data[3] = { 1800, 1, VRM_MODE_LDO_HPM };

	if (base == 0 || (base >> 16 & 0xf) != 4) {
		printf("fts: RPMh: no regulator \"ldoc1\" (address %#x)\n", base);
		return false;
	}
	printf("fts: RPMh: ldoc1 is at %#x\n", base);
	if (!rpmh_write(address, data, 3)) {
		return false;
	}
	printf("fts: I/O supply on (1.8 V)\n");
	return true;
}



/* ---- the board's boost rail: SID2 0xC444, the fix that made touch work ------------------------
 *
 * Source: UniXNU-umi's package/touch/UmiBoardBoost.h and UmiSpmi.inc, the build whose owner
 * confirmed working physical touch on this handset (attempt 80). Their words: attempt 79 recorded
 * PM8150B GPIO5 as "enabled digital output-low, source=00", and attempt 80 "writes only
 * SID2/C444=80 and waits 4000us before touch supply/reset".
 *
 * The rail is the board's always-on vdd_boost_vreg (xiaomi-sm8250-common.dtsi:112, active-high,
 * 4000us startup delay). With its GPIO latched low the panel's supply has no boost, and the touch
 * controller produces no coordinate events at all -- 97004 polls and 0 points on attempt 79.
 *
 * The write is guarded the way their code guards it: the whole GPIO profile is read first, only
 * a pin that is currently output-low is turned on, and it is never turned off again.
 */

/* The SPMI register interface, from spmi-pmic-arb.c through UniXNU-umi's UmiSpmi.inc. */
#define SPMI_CORE_PHYS          0x0c440000UL    /* version + the channel map */
#define SPMI_CORE_SIZE          0x1100UL
#define SPMI_CNFG_PHYS          0x0c40a000UL    /* who owns each channel */
#define SPMI_CNFG_SIZE          0x26000UL
#define SPMI_WRITE_PHYS         0x0c600000UL    /* the write channel bank: +0x10000 per channel */
#define SPMI_OBSERVER_PHYS      0x0e600000UL    /* the read bank: +0x80 per channel */
#define SPMI_ARB_VERSION        0x50010000u     /* what the arbiter must report */

#define SPMI_CHANNEL_MAP(n)     (spmi_core + 0x900 + 4 * (n))   /* [19:8] ppid */
#define SPMI_OWNER(n)           (spmi_cnfg + 0x700 + 4 * (n))   /* [2:0] the EE that may write */
#define SPMI_CMD_OFFSET         0x00            /* the command word */
#define SPMI_STATUS_OFFSET      0x08            /* bit 0 done, bits 1..3 errors */
#define SPMI_DATA_OFFSET        0x10            /* the byte to write */
#define SPMI_OBS_DATA_OFFSET    0x18            /* the byte read on the observer bank */
#define SPMI_READ_OP            (1u << 27)

static vm_offset_t spmi_core, spmi_cnfg;
static lck_spin_t *spmi_lock_p;
static lck_grp_t *spmi_lock_grp;
static LCK_SPIN_DECLARE(spmi_guard, &spmi_lock_grp);

/* one SPMI transfer: sid = the PMIC (2 = PM8150B), address = the 16-bit register */
static const char *
spmi_transfer(uint8_t sid, uint16_t address, uint8_t *value, bool write)
{
	uint32_t ppid = ((uint32_t)sid << 8) | (address >> 8);
	uint32_t channel = 0, owner = 7;
	bool found = false;
	const char *problem = "the arbiter did not finish";
	lck_spin_lock(&spmi_guard);

	for (uint32_t i = 0; i < 512; i++) {
		uint32_t map = rd(SPMI_CHANNEL_MAP(i));

		if (map != 0 && ((map >> 8) & 0xfff) == ppid) {
			uint32_t who = rd(SPMI_OWNER(i)) & 7;

			channel = write ? (SPMI_WRITE_PHYS + i * 0x10000) : (SPMI_OBSERVER_PHYS + i * 0x80);
			owner = who;
			found = true;
			if (write && who == 0) {
				break;                  /* ours to write through: prefer it */
			}
		}
	}
	if (!found) {
		lck_spin_unlock(&spmi_guard);
		return "no channel of the arbiter serves that peripheral";
	}
	if (write && owner != 0) {
		lck_spin_unlock(&spmi_guard);
		return "that peripheral is not this EE's to write";
	}
	if (write) {
		wr(channel + SPMI_DATA_OFFSET, *value);
		wr(channel, ((uint32_t)(address & 0xff)) << 4);         /* bit 27 clear = write */
	} else {
		wr(channel, SPMI_READ_OP | ((uint32_t)(address & 0xff)) << 4);
	}
	for (int i = 0; i < 1000; i++) {
		uint32_t status = rd(channel + SPMI_STATUS_OFFSET);

		if (status & 1) {
			if (status & (0xe)) {
				problem = "the transfer reported an error";
			} else {
				if (!write) {
					*value = (uint8_t)rd(channel + SPMI_OBS_DATA_OFFSET);
				}
				problem = NULL;
			}
			break;
		}
		delay(1);
	}
	lck_spin_unlock(&spmi_guard);
	return problem;
}

/*
 * The guarded board-rail enable: the eleven registers and their values are UniXNU-umi's
 * UmiBoardBoost.h verbatim, read from this handset and qualified there in attempt 79.
 */
static const char *
board_boost_on(void)
{
	static const uint16_t addr[11] = { 0x0104, 0x0105, 0xc404, 0xc405, 0xc440, 0xc441,
	    0xc442, 0xc443, 0xc444, 0xc445, 0xc446 };
	static const uint8_t expected[11] = { 0x51, 0x20, 0x10, 0x11, 1, 0, 4, 0, 0, 1, 0x80 };
	uint8_t value[11] = { 0 };
	const char *problem;

	for (unsigned i = 0; i < 11; i++) {
		problem = spmi_transfer(2, addr[i], &value[i], false);
		if (problem != NULL) {
			return problem;
		}
	}
	printf("fts: PM8150B GPIO5 profile: c444=%#x (0 = output-low, 0x80 = on)\n", value[8]);
	for (unsigned i = 0; i < 11; i++) {
		if (i == 8 ? (value[i] != 0 && value[i] != 0x80) : value[i] != expected[i]) {
			return "the PM8150B GPIO profile is not the one this phone was qualified on";
		}
	}
	if (value[8] == 0) {
		uint8_t on = 0x80;

		problem = spmi_transfer(2, 0xc444, &on, true);
		if (problem != NULL) {
			return problem;
		}
		printf("fts: wrote SID2 c444 = 0x80 (the board's boost rail on)\n");
	} else {
		printf("fts: the boost rail was already on\n");
	}
	delay(4000);                                    /* the 4000us startup delay of that rail */

	/* and read the whole profile back: the rail must now be where it was asked to be */
	for (unsigned i = 0; i < 11; i++) {
		problem = spmi_transfer(2, addr[i], &value[i], false);
		if (problem != NULL) {
			return problem;
		}
		if (i == 8 ? value[i] != 0x80 : value[i] != expected[i]) {
			return "the boost rail did not read back as on";
		}
	}
	printf("fts: board boost on and verified\n");
	return NULL;
}

/* ---- clocks (GCC) -------------------------------------------------------------------------------
 *
 * The register numbers are Linux's drivers/clk/qcom/gcc-sm8250.c, not redwood's: the same engine
 * (WRAP1 SE5) and the same vote register, but this file now says where each number comes from.
 *
 *   gcc_qupv3_wrap1_s5_clk      halt 0x185fc, enable 0x52008 BIT(27)
 *   gcc_qupv3_wrap_1_m_ahb_clk  halt 0x18004, enable 0x52008 BIT(20)
 *   gcc_qupv3_wrap_1_s_ahb_clk  halt 0x18008, enable 0x52008 BIT(21)
 *   gcc_qupv3_wrap1_core_clk    halt 0x23138, enable 0x52008 BIT(19)
 *   gcc_qupv3_wrap1_core_2x_clk halt (voted), enable 0x52008 BIT(18)
 *   gcc_qupv3_wrap1_s5_clk_src  cmd_rcgr 0x18600
 *
 * A branch clock is "on" when bit 31 of its halt register is clear (BRANCH_HALT_VOTED); the
 * enable itself is a bit in 0x52008, which is the vote register.
 */
#define GCC_VOTE                (gcc + 0x52008)
#define VOTE_CORE_2X            (1u << 18)      /* gcc_qupv3_wrap1_core_2x_clk */
#define VOTE_CORE               (1u << 19)      /* gcc_qupv3_wrap1_core_clk */
#define VOTE_M_AHB              (1u << 20)      /* gcc_qupv3_wrap_1_m_ahb_clk */
#define VOTE_S_AHB              (1u << 21)      /* gcc_qupv3_wrap_1_s_ahb_clk */
#define VOTE_SE5                (1u << 27)      /* gcc_qupv3_wrap1_s5_clk */

#define GCC_HALT_M_AHB          (gcc + 0x18004)
#define GCC_HALT_S_AHB          (gcc + 0x18008)
#define GCC_HALT_SE5            (gcc + 0x185fc)
#define GCC_HALT_CORE_SRC       (gcc + 0x23138) /* the CORE source runs off a different block */
#define GCC_SE5_CMD_RCGR        (gcc + 0x18600) /* the SE5 clock source's cmd_rcgr */
#define CLOCK_HALTED            (1u << 31)

static bool
clocks_on(void)
{
	/* all five votes at once, as the branch tables above put them */
	wr(GCC_VOTE, rd(GCC_VOTE) | VOTE_CORE_2X | VOTE_CORE | VOTE_M_AHB | VOTE_S_AHB | VOTE_SE5);

	/* and wait for the three that gate this engine to come out of halt */
	for (int i = 0; i < 1000; i++) {
		uint32_t halted = rd(GCC_HALT_M_AHB) | rd(GCC_HALT_S_AHB) | rd(GCC_HALT_SE5);

		if ((halted & CLOCK_HALTED) == 0) {
			return true;
		}
		delay(10);
	}
	printf("fts: clocks stay off: m_ahb %#x, s_ahb %#x, se5 %#x\n", rd(GCC_HALT_M_AHB), rd(GCC_HALT_S_AHB),
	    rd(GCC_HALT_SE5));
	return false;
}

/* ---- a visible mark on the screen, so the driver can be followed without a log ------------------
 *
 * The kernel's log cannot be had while iOS owns the USB port, and the loader's log buffer is not
 * handed back on a clean return. A row of coloured squares at the top-left of the framebuffer the
 * boot loader left says what the driver got to: the step number and pass/fail, in a place a photo
 * of the screen can be read from.
 */
struct vc_info {
	unsigned int v_height, v_width, v_depth, v_rowbytes;
	unsigned long v_baseaddr;
	unsigned int v_type;
	char v_name[32];
	uint64_t v_physaddr;
	unsigned int v_rows, v_columns, v_rowscanbytes, v_scale;
};
extern struct vc_info vinfo;

/* square `slot` at the top-left, in `colour` (0xRRGGBB) */
static void
mark(unsigned slot, uint32_t colour, bool tall)
{
	uint32_t *base = (uint32_t *)(uintptr_t)vinfo.v_baseaddr;
	uint32_t stride = vinfo.v_rowbytes / 4, size = 16, x0, y0;

	/*
	 * Only draw while the framebuffer is the one the boot loader left: it is 1080 wide with a
	 * 4320-byte row on this phone, and iOS replaces it once it owns the display. Drawing into a
	 * stale or moved buffer would write over memory that is not ours.
	 */
	if (base == NULL || vinfo.v_width != 1080 || vinfo.v_rowbytes != 4320 ||
	    vinfo.v_height < 64 || vinfo.v_depth < 24) {
		return;
	}
	x0 = 4 + slot * (size + 4);
	y0 = 4;
	for (uint32_t y = y0; y < y0 + (tall ? size * 2 : size) && y < vinfo.v_height; y++) {
		for (uint32_t x = x0; x < x0 + size && x < vinfo.v_width; x++) {
			base[y * stride + x] = colour;
		}
	}
}
#define MARK_OK(n)      mark((n), 0x00ff00, false)      /* green: that step finished */
#define MARK_BAD(n)     mark((n), 0xff0000, false)      /* red: that step failed */
#define MARK_NOW(n)     mark((n), 0xffff00, false)      /* yellow: at that step */


/* ---- the I2C bus of this chip: bit-banged on GPIO 36 and 37 -----------------------------------
 *
 * Source: UniXNU-umi's package/touch/UmiFtsBus.inc, the XNU build whose owner confirmed working
 * touch. Their words and code:
 *
 *   #define SDA 36U / #define SCL 37U / #define HALF_CYCLE_US 5U
 *   WriteByte(0x92);   // 0x49 << 1 | 0: the only slave address used
 *   WriteByte(0x93);   // 0x49 << 1 | 1
 *
 * They drive the pins themselves, start -> address -> data -> repeated start -> address -> read
 * -> stop, and do not use the GENI engine at all. Every attempt in this file that went through
 * GENI's I2C mode produced no client traffic, which matches their choice.
 */
#define PIN_SDA                 36
#define PIN_SCL                 37
#define SCL_HALF_US             5               /* 100 kHz */
#define I2C_WRITE_ADDR          0x92            /* 0x49 << 1 | 0 */
#define I2C_READ_ADDR           0x93            /* 0x49 << 1 | 1 */

static void
i2c_line(unsigned pin, bool high)
{
	uint32_t io = rd(GPIO_IO(pin));

	wr(GPIO_IO(pin), (io & ~GPIO_OUT_BIT) | (high ? GPIO_OUT_BIT : 0));
}

static bool
i2c_read_line(unsigned pin)
{
	return (rd(GPIO_IO(pin)) & GPIO_IN_BIT) != 0;
}

/* The pin's control word the way AcquirePins() makes it: function 0, no pull, no drive, and the
 * eGPIO bit carried over from bit 11 when the pin has one. */
static uint32_t
i2c_pin_ctl(unsigned pin)
{
	uint32_t ctl = rd(GPIO_CFG(pin)) & ~(GPIO_FUNC_MASK | GPIO_OE_BIT | GPIO_PULL_MASK | GPIO_DRIVE_MASK);

	if ((ctl & (1u << 11)) != 0) {
		ctl |= 1u << 12;
	}
	return ctl;
}

/* make a pin an output at `high`: control word first without drive, then the level, then drive */
static void
i2c_drive(unsigned pin, bool high)
{
	uint32_t ctl = i2c_pin_ctl(pin);

	wr(GPIO_CFG(pin), ctl);
	i2c_line(pin, high);
	wr(GPIO_CFG(pin), ctl | GPIO_OE_BIT);
}

/* and back to an input, which is how the chip drives ACK and data */
static void
i2c_release(unsigned pin)
{
	uint32_t ctl = i2c_pin_ctl(pin);

	wr(GPIO_CFG(pin), ctl);
	wr(GPIO_IO(pin), rd(GPIO_IO(pin)) & ~GPIO_OUT_BIT);    /* latch low, as AcquirePins does */
}

static void
i2c_clock_high(void)
{
	i2c_line(PIN_SCL, true);
	for (unsigned i = 0; i < 100 && !i2c_read_line(PIN_SCL); i++) {
		delay(1);                               /* a slow slave may hold it down */
	}
	delay(SCL_HALF_US);
}

static void
i2c_clock_low(void)
{
	i2c_line(PIN_SCL, false);
	delay(SCL_HALF_US);
}

static bool
i2c_start(bool repeated)
{
	i2c_release(PIN_SDA);
	delay(SCL_HALF_US);
	i2c_clock_high();
	if (!i2c_read_line(PIN_SDA)) {
		return false;                           /* somebody is holding the bus */
	}
	i2c_drive(PIN_SDA, false);
	delay(SCL_HALF_US);
	i2c_clock_low();
	return true;
}

static void
i2c_stop(void)
{
	i2c_clock_low();
	i2c_drive(PIN_SDA, false);
	delay(SCL_HALF_US);
	i2c_clock_high();
	i2c_release(PIN_SDA);
	delay(SCL_HALF_US);
}

/* one byte out; true when the slave acknowledged it */
static bool
i2c_write_byte(uint8_t value)
{
	for (int bit = 7; bit >= 0; bit--) {
		i2c_drive(PIN_SDA, ((value >> bit) & 1) != 0);
		delay(SCL_HALF_US);
		i2c_clock_high();
		i2c_clock_low();
	}
	i2c_release(PIN_SDA);                           /* the slave pulls this low to acknowledge */
	delay(SCL_HALF_US);
	i2c_clock_high();
	{
		bool acked = !i2c_read_line(PIN_SDA);

		i2c_clock_low();
		return acked;
	}
}

static uint8_t
i2c_read_byte(bool last)
{
	uint8_t value = 0;

	i2c_release(PIN_SDA);
	for (int bit = 0; bit < 8; bit++) {
		i2c_clock_high();
		value = (uint8_t)((value << 1) | (i2c_read_line(PIN_SDA) ? 1 : 0));
		i2c_clock_low();
	}
	i2c_drive(PIN_SDA, !last);                      /* drive low to acknowledge, release on the last */
	delay(SCL_HALF_US);
	i2c_clock_high();
	i2c_clock_low();
	i2c_release(PIN_SDA);
	return value;
}

/* set the two pins up the way the proven build leaves them */
static void
i2c_pins_init(void)
{
	i2c_release(PIN_SDA);
	i2c_release(PIN_SCL);
	delay(SCL_HALF_US);
	i2c_drive(PIN_SCL, true);                       /* the clock idles high */
	i2c_release(PIN_SDA);
	delay(SCL_HALF_US);
}

/* write `wn` bytes then read `rn` back, the shape every FTS transfer has */
static const char *
i2c_transfer(const uint8_t *write, uint32_t wn, uint8_t *read, uint32_t rn)
{
	if (!i2c_start(false)) {
		return "the bus was busy";
	}
	if (!i2c_write_byte(I2C_WRITE_ADDR)) {
		i2c_stop();
		return "the chip did not acknowledge its address";
	}
	for (uint32_t i = 0; i < wn; i++) {
		if (!i2c_write_byte(write[i])) {
			i2c_stop();
			return "the chip did not acknowledge a byte";
		}
	}
	if (rn != 0) {
		if (!i2c_start(true)) {
			i2c_stop();
			return "the repeated start failed";
		}
		if (!i2c_write_byte(I2C_READ_ADDR)) {
			i2c_stop();
			return "the chip did not acknowledge its read address";
		}
		for (uint32_t i = 0; i < rn; i++) {
			read[i] = i2c_read_byte(i + 1 == rn);
		}
	}
	i2c_stop();
	return NULL;
}

/* ---- the engine --------------------------------------------------------------------------------- */

static const char *
engine_init(void)
{
	if ((rd(GENI_FW_REVISION_RO) >> 8 & 0xff) != 0) {
		return "the serial engine has no I2C firmware";
	}
	if (rd(GENI_IF_DISABLE_RO) & 1) {
		return "the serial engine's FIFO interface is disabled";
	}
	fifo_depth = rd(SE_HW_PARAM_0) >> 16 & 0x3f;
	if (fifo_depth < 4) {
		return "the serial engine reports no FIFO";
	}
	wr(SE_GSI_EVENT_EN, 0);
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(GENI_S_IRQ_CLEAR, 0xffffffff);
	wr(GENI_CGC_CTRL, rd(GENI_CGC_CTRL) | 0x7f);
	wr(GENI_OUTPUT_CTRL, 0x7f);
	wr(GENI_FORCE_DEFAULT, 1);
	wr(GENI_DMA_MODE_EN, rd(GENI_DMA_MODE_EN) & ~1u);
	wr(GENI_RX_WATERMARK, fifo_depth - 3);
	wr(GENI_RX_RFR_WATERMARK, fifo_depth - 2);
	wr(GENI_TX_WATERMARK, 0);
	wr(SE_IRQ_EN, rd(SE_IRQ_EN) | 0xf);
	wr(GENI_M_IRQ_EN, rd(GENI_M_IRQ_EN) | 0x7fc0007f);

	/* one byte per FIFO word */
	wr(GENI_TX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_RX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_BYTE_GRAN, 0);
	return NULL;
}

static void
fifo_put(const uint8_t *bytes, uint32_t length)
{
	for (uint32_t i = 0; i < length; i += 4) {
		uint32_t word = 0;

		for (uint32_t k = 0; k < 4 && i + k < length; k++) {
			word |= (uint32_t)bytes[i + k] << (8 * k);
		}
		wr(GENI_TX_FIFO, word);
	}
}

static const char *
wait_done(void)
{
	uint64_t deadline, interval;
	uint32_t status;

	nanoseconds_to_absolutetime(20ull * NSEC_PER_MSEC, &interval);
	deadline = mach_absolute_time() + interval;
	while ((rd(GENI_STATUS) & 1) != 0) {
		if (mach_absolute_time() > deadline) {
			wr(GENI_M_CMD0, 0);
			return "the engine did not finish";
		}
	}
	status = rd(GENI_M_IRQ_STATUS);
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	if ((status & GENI_CMD_DONE) == 0) {
		return "the command did not complete";
	}
	if (rd(SE_GENI_IOS) & 0x2) {
		return "the chip did not answer (NACK)";
	}
	return NULL;
}

/*
 * focaltech_i2c.c: a read is one address byte out and then the bytes back in; a write of data is
 * the address and the data in one transfer. Both are GENI's I2C_WRITE_READ with the transmit
 * length counting the address bytes.
 */
static const char *
fts_read(uint8_t reg, uint8_t *in, uint32_t in_length)
{
	return i2c_transfer(&reg, 1, in, in_length);
}

static const char *
fts_write_reg(uint8_t reg, uint8_t value)
{
	uint8_t pair[2] = { reg, value };

	return i2c_transfer(pair, 2, NULL, 0);
}

/*
 * One FIFO read: one byte out (the command 0x86), eight bytes back. This is the shape the working
 * build uses for an event ("WriteSize == 1 && Write[0] == 0x86 && ReadSize == 8").
 */
static const char *
fts_read_events(const uint8_t *cmd, uint8_t *event)
{
	return i2c_transfer(cmd, 1, event, FTS_EVENT_LEN);
}


/* ---- the controller's own protocol discovery and the scan commands ----------------------------
 *
 * All of this is UniXNU-umi's package/touch, the build whose owner confirmed working touch:
 *
 *   UmiFtsSysInfo.h    reading the 208-byte system information in 16-byte chunks with command
 *                      A6 hi lo, then the CRC with FA 20 00 00 78; the identity fields live at
 *                      fixed offsets in that block (see umi_fts_sysinfo below).
 *   UmiFtsSensing.h    the commands that actually start the controller reporting:
 *                        A0 00 01                 scan on
 *                        A2 03 00 00 00 03        events on
 *                        A0 00 00  then 50 ms, then A0 00 01   (the initial FOD branch)
 *
 * This driver used to read events from a chip it had never told to scan, which is why it saw
 * nothing at all no matter how the transport was fixed.
 */
#define FTS_SYSINFO_SIZE        208
#define FTS_SYSINFO_CHUNK       16

struct fts_sysinfo {
	bool valid;
	uint16_t chip0, chip1, fw, svn, config, cx;
	uint16_t res_x, res_y;
	uint8_t cfg_afe, cx_afe, panel_afe, protocol, tx, rx;
};

static uint16_t
fts_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((unsigned)p[1] << 8));
}

/* umi_fts_sysinfo: the offsets are the vendor parser's, checked against it in their repository */
static struct fts_sysinfo
fts_sysinfo(const uint8_t *p, unsigned bytes)
{
	struct fts_sysinfo s = { 0 };

	if (bytes != FTS_SYSINFO_SIZE || p[0] != 0xa5 || p[1] != 1) {
		return s;
	}
	s.valid = true;
	s.chip0 = fts_le16(p + 10);
	s.chip1 = fts_le16(p + 14);
	s.fw = fts_le16(p + 16);
	s.svn = fts_le16(p + 18);
	s.config = fts_le16(p + 20);
	s.cx = fts_le16(p + 24);
	s.cfg_afe = p[28];
	s.cx_afe = p[29];
	s.panel_afe = p[30];
	s.protocol = p[31];
	s.res_x = fts_le16(p + 80);
	s.res_y = fts_le16(p + 82);
	s.tx = p[84];
	s.rx = p[85];
	return s;
}

/* Read the whole 208-byte block in 16-byte chunks, then the CRC byte. */
static const char *
fts_read_controller(uint8_t *bytes, unsigned *got)
{
	uint8_t crc = 0;
	const char *problem;

	*got = 0;
	for (unsigned off = 0; off < FTS_SYSINFO_SIZE; off += FTS_SYSINFO_CHUNK) {
		uint8_t cmd[3] = { 0xa6, (uint8_t)(off >> 8), (uint8_t)off };

		problem = i2c_transfer(cmd, sizeof(cmd), bytes + off, FTS_SYSINFO_CHUNK);
		if (problem != NULL) {
			return problem;
		}
		*got += FTS_SYSINFO_CHUNK;
	}
	{
		const uint8_t crc_cmd[5] = { 0xfa, 0x20, 0, 0, 0x78 };

		problem = i2c_transfer(crc_cmd, sizeof(crc_cmd), &crc, 1);
		if (problem != NULL) {
			return problem;
		}
	}
	return NULL;
}

/* umi_fts_start_sensing: the scan commands, in their order. */
static const char *
fts_start_sensing(bool initial)
{
	static const uint8_t on[3] = { 0xa0, 0x00, 0x01 };
	static const uint8_t reports[6] = { 0xa2, 0x03, 0, 0, 0, 0x03 };
	static const uint8_t off[3] = { 0xa0, 0x00, 0x00 };
	const char *problem;

	problem = i2c_transfer(on, sizeof(on), NULL, 0);
	if (problem != NULL) {
		return problem;
	}
	problem = i2c_transfer(reports, sizeof(reports), NULL, 0);
	if (problem != NULL) {
		return problem;
	}
	if (!initial) {
		return NULL;                    /* a repeat run only needs the two above */
	}
	problem = i2c_transfer(off, sizeof(off), NULL, 0);
	if (problem != NULL) {
		return problem;
	}
	delay(50000);                           /* WAIT_AFTER_SENSEOFF */
	problem = i2c_transfer(on, sizeof(on), NULL, 0);
	return problem;
}

/* ---- one event, decoded as UniXNU-umi's umi_fts_parse ----------------------------------------- */

static struct {
	bool down;
	uint32_t x, y;          /* in screen pixels, as the driver posts them */
} contact[FTS_MAX_POINTS];

/*
 * Returns false when the controller asked to be reset or reported a fault: the working build
 * stops its worker and lets the startup state machine run the reset, rather than reading on.
 */
static bool
fts_parse(const uint8_t ev[FTS_EVENT_LEN], uint32_t max_x, uint32_t max_y)
{
	char line[64];
	uint32_t id, type, x, y;
	bool changed;

	if (ev[0] == 0) {
		return true;                                    /* no event in this window */
	}
	if (ev[0] == FTS_EV_RESET || ev[0] == FTS_EV_FAULT) {
		vsnprintf(line, sizeof(line), "controller %s\n",
		    ev[0] == FTS_EV_RESET ? "reset" : "fault");
		loader_log_line(line);
		for (unsigned i = 0; i < FTS_MAX_POINTS; i++) {
			contact[i].down = false;
		}
		return false;
	}
	if ((ev[0] & 0x0f) == 3) {
		return true;                                    /* a status byte, not a contact */
	}
	id = ev[1] >> 4;
	type = ev[1] & 0x0f;
	if (id >= FTS_MAX_POINTS) {
		return true;
	}
	x = (((uint32_t)ev[3] & 0x0f) << 8) | ev[2];
	y = ((uint32_t)ev[4] << 4) | (ev[3] >> 4);

	/* a lift: the top nibble is 3, or the type is not one of the two contact types */
	if ((ev[0] >> 4) == 3 || (type != 1 && type != 2)) {
		changed = contact[id].down;
		contact[id].down = false;
		if (changed) {
			redwood_touch_post(REDWOOD_TOUCH_UP, id, contact[id].x, contact[id].y);
		}
		return true;
	}
	if (x > max_x || y > max_y) {
		return true;                                    /* out of the panel: their "bad" */
	}
	changed = !contact[id].down || contact[id].x != x || contact[id].y != y;
	contact[id].x = x;
	contact[id].y = y;
	if (!contact[id].down) {
		contact[id].down = true;
		redwood_touch_post(REDWOOD_TOUCH_DOWN, id, x, y);
	} else if (changed) {
		redwood_touch_post(REDWOOD_TOUCH_MOVE, id, x, y);
	}
	return true;
}

/*
 * focaltech_core.c's power sequence, without the regulators this kernel has no driver for: the
 * chip is already powered by the boot loader (Android leaves avdd for the panel), so only the
 * reset line is driven.
 */
/*
 * The chip's supplies, from the two regulators its device tree node names:
 *
 *   avdd-supply = <&touch_vddio_vreg>   GPIO 69, enable-active-high (umi.dts)
 *   vdd-supply  = <&rpmh-regulator-ldoc1>   RPMh: pm8150a_l1
 *
 * Both were left off by the previous run of this driver, and a chip with no supply cannot answer
 * on I2C at all -- that is what "chip id" was going to read. They are turned on here, in the
 * order the vendor driver does (vdd first, then avdd, then the reset line).
 *
 * What is still not touched: GCC. The clock indices in umi.dts point at the same WRAP1 SE5 engine
 * redwood drives, but SM8250's register layout was never checked, and a blind clock write is what
 * used to stop this machine.
 */
#define GPIO_AVDD               69              /* umi.dts: touch_vddio_vreg gpio = <&tlmm 0x45> */

static void
power_on(void)
{
	/*
	 * focaltech_core.c's power sequence, in its order: hold the chip in reset, bring the
	 * supplies up, let the panel's rail settle, then release the reset and let the chip boot.
	 * The reset is held from before the power comes up, so the chip never starts on a rail that
	 * is still rising.
	 */
	/*
	 * The interrupt pin first, as ConfigureIrq() does: input, pull-up, drive 8 mA (the official
	 * umi ts_active state). The reset pin is held low from before the supplies come up.
	 */
	{
		uint32_t irq = rd(GPIO_CFG(GPIO_IRQ));

		irq &= ~(GPIO_FUNC_MASK | GPIO_OE_BIT | GPIO_PULL_MASK | GPIO_DRIVE_MASK);
		irq |= 3u | (3u << 6);                       /* pull-up, 8 mA */
		if ((irq & (1u << 11)) != 0) {
			irq |= 1u << 12;
		}
		wr(GPIO_CFG(GPIO_IRQ), irq);
	}
	gpio_drive(GPIO_RESET, false);                  /* held in reset, level set before the enable */

	/* fts.c:6803 (through UniXNU-umi's audit): VDD, 15 ms, then AVDD -- in that order */
	if (!io_supply_on()) {                          /* vdd: RPMh L1C, 1800 mV */
		printf("fts: vdd was not turned on\n");
	}
	delay(15000);

	gpio_drive(GPIO_AVDD, true);                    /* avdd: GPIO 69, enable-active-high */
	delay(20000);                                   /* the rail settles before the chip is let out */

	/*
	 * ftsCore.c:85 holds the reset line low for 10 ms before releasing it. It has been low since
	 * the top of this function, so that much is already spent; the release is the pulse.
	 */
	gpio_drive(GPIO_RESET, true);                   /* out of reset; the chip boots */
}

static void
touch_thread(void *arg __unused, wait_result_t wresult __unused)
{
	uint8_t buf[FTS_EVENT_LEN];
	uint8_t id[2] = { 0, 0 };
	uint32_t max_x = 1080, max_y = 2340, said = 0;
	const char *problem;

	/*
	 * Only what this driver actually touches. Each ml_io_map() takes kernel virtual memory, and
	 * the boot path already maps a great deal; asking for regions that are not used was enough to
	 * run the kernel out of it -- the machine panicked in kmem_alloc on 16 MB right after this
	 * driver started. SE and GCC are not used any more (the bus is driven by hand), so they are
	 * not mapped.
	 */
	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
	cmd_db = ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);
	if (tlmm == 0) {
		printf("fts: no pin controller (tlmm %#x)\n", tlmm);
		thread_terminate(current_thread());
		return;
	}
	printf("fts: TLMM at %#x\n", tlmm);
	MARK_BAD(0);    /* slot 0 red = the thread ran at all */

	/*
	 * The order here follows UniXNU-umi's AcquirePins(), which is the sequence that works on this
	 * handset: the two bus pins are taken and put in their input state first, then the supply and
	 * the reset, and only then is the bus checked to be idle. There is no GENI setup at all --
	 * that engine is not used for this chip.
	 */
	MARK_NOW(1);
	i2c_pins_init();
	delay(5);
	if (!i2c_read_line(PIN_SDA) || !i2c_read_line(PIN_SCL)) {
		MARK_BAD(1);
		printf("fts: the bus is not idle after taking the pins (SDA %u, SCL %u)\n",
		    (uint64_t)i2c_read_line(PIN_SDA), (uint64_t)i2c_read_line(PIN_SCL));
		thread_terminate(current_thread());
		return;
	}
	MARK_OK(1);     /* slot 1 green = the pins are taken and the bus is idle */
	printf("fts: pins taken; SDA and SCL are both high\n");

	/*
	 * The board's own boost rail first: UniXNU-umi's attempt 80 found this pin latched low and
	 * turning it on is what made touch work on this handset. Everything below depends on it.
	 */
	spmi_core = ml_io_map(SPMI_CORE_PHYS, SPMI_CORE_SIZE);
	spmi_cnfg = ml_io_map(SPMI_CNFG_PHYS, SPMI_CNFG_SIZE);
	if (spmi_core == 0 || spmi_cnfg == 0) {
		printf("fts: no SPMI registers\n");
	} else if (rd(spmi_core) != SPMI_ARB_VERSION) {
		printf("fts: the PMIC arbiter is not v5 (%#x); the boost rail is left alone\n", rd(spmi_core));
	} else {
		problem = board_boost_on();
		if (problem != NULL) {
			printf("fts: the board boost rail: %s\n", problem);
		}
		MARK_OK(5);     /* slot 5 green = the board rail was handled */
	}

	/*
	 * The chip is powered and out of reset before the engine is set up: an I2C controller wired
	 * to a chip that has no supply has nothing to talk to, and its first transfer only shows a
	 * NACK that says nothing about why.
	 */
	power_on();
	MARK_OK(2);     /* slot 2 green = powered and out of reset */
	printf("fts: powered and out of reset; irq line reads %u\n", (uint64_t)gpio_get(GPIO_IRQ));

	MARK_OK(3);     /* slot 3 green = the chip is powered */
	printf("fts: bus on GPIO %d/%d, supply up, out of reset\n", PIN_SDA, PIN_SCL);

	/*
	 * Bring the controller up the way the working build does: read its 208-byte system
	 * information, check that the identity is the one this phone has, then send the commands that
	 * start it scanning. Reading events without those commands yields nothing at all -- that is
	 * the mistake this driver made for a long time.
	 */
	{
		uint8_t info[FTS_SYSINFO_SIZE];
		unsigned got = 0;
		struct fts_sysinfo sys;

		problem = fts_read_controller(info, &got);
		if (problem != NULL) {
			printf("fts: reading the controller information: %s (%u of %u bytes)\n", problem, got,
			    (unsigned)FTS_SYSINFO_SIZE);
			thread_terminate(current_thread());
			return;
		}
		sys = fts_sysinfo(info, got);
		if (!sys.valid) {
			printf("fts: the controller information is not a valid block (%#x %#x, %u bytes)\n",
			    info[0], info[1], got);
			thread_terminate(current_thread());
			return;
		}
		printf("fts: chip %#x %#x, fw %#x, svn %#x, config %#x\n", sys.chip0, sys.chip1, sys.fw, sys.svn,
		    sys.config);
		if (sys.res_x != 0 && sys.res_y != 0) {
			max_x = sys.res_x;
			max_y = sys.res_y;
		}
		/* umi_fts_native_identity_check: the identity this handset reports */
		if (sys.chip0 != 0x3639 || sys.fw != 0x128 || sys.svn != 0x4053 || sys.config != 0x4b) {
			printf("fts: the controller is not the identified one (want 3639/128/4053/4b)\n");
			thread_terminate(current_thread());
			return;
		}
		MARK_OK(4);     /* slot 4 green = the controller answered and was identified */
		printf("fts: controller identified; panel %u x %u\n", max_x, max_y);

		problem = fts_start_sensing(true);
		if (problem != NULL) {
			printf("fts: starting the scan: %s\n", problem);
			thread_terminate(current_thread());
			return;
		}
		printf("fts: scan started (A0 00 01, A2 03 ..., A0 00 00, 50 ms, A0 00 01)\n");
	}

	/* and to the point reports: focaltech_core.c reads register 0 and takes byte 2 */
	{
		uint8_t work = 0;

		if (fts_read(FTS_REG_WORKMODE, &work, 1) == NULL) {
			printf("fts: work mode %#x\n", work);
		}
	}
	/*
	 * The read loop of the working build (UmiFtsTouch.cpp): the command byte 0x86 brings back one
	 * eight-byte event, and up to eight of them are taken per pass. Their worker reads whether or
	 * not the interrupt line is high, because the controller's FIFO is the source of truth; a
	 * quiet line with a non-empty FIFO is a missed edge, and a fallback read every 100 ms covers
	 * it. Reading only on a high line is what left this driver with no events at all.
	 */
	uint8_t cmd = FTS_CMD_READ_EVENT;
	bool awaiting_ready = false;                     /* the chip answered its id already */
	/*
	 * A running tally, printed every few seconds. The kernel's own log is a 16 KB ring that has
	 * overwritten everything by the time the machine comes back, so what survives is whatever was
	 * written last: a periodic line means the state at the end of the run is always readable.
	 */
	uint64_t polls = 0, nonempty = 0, points = 0, resets = 0, errors = 0, irq_high = 0;
	uint64_t next_report = mach_absolute_time();
	uint64_t report_interval;

	nanoseconds_to_absolutetime(5ull * NSEC_PER_SEC, &report_interval);
	for (;;) {
		bool irq = gpio_get(GPIO_IRQ);

		if (irq) {
			irq_high++;
		}
		for (unsigned n = 0; n < 8; n++) {
			polls++;
			problem = fts_read_events(&cmd, buf);
			if (problem != NULL) {
				errors++;
				if (said < 8) {
					said++;
					printf("fts: reading an event: %s\n", problem);
				}
				break;
			}
			if (buf[0] == 0) {
				break;                   /* the FIFO is empty: nothing else to take */
			}
			nonempty++;
			if (!fts_parse(buf, max_x, max_y)) {
				resets++;
				printf("fts: controller asked for a reset (%llu events so far)\n", nonempty);
				break;
			}
			if (buf[0] != 0) {
				points++;
			}
		}
		if (mach_absolute_time() >= next_report) {
			next_report = mach_absolute_time() + report_interval;
			printf("fts: %llu polls, %llu irq-high, %llu non-empty, %llu events, %llu resets, %llu errors\n",
			    polls, irq_high, nonempty, points, resets, errors);
		}
		delay(irq ? 2000 : 8000);                    /* 2 ms while a finger is on it */
	}
}

void
redwood_touch_i2c_init(void)
{
	uint32_t mode = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("touchscreen", &mode, sizeof(mode)) || mode != 2) {
		return;
	}
	/* nothing but the thread: see the note at touch_thread() */
	if (kernel_thread_start(touch_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
