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
extern void IOSleep(unsigned int milliseconds);   /* a real sleep: delay() spins */
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
/*
 * umi.dts: cmd_db: reserved-memory@80860000 { reg = <0x80860000 0x20000> }. This used to read
 * 0x80880000, which is res_xbl_uefi_log -- the XBL's own log ring -- so the magic came back as
 * ff ff 7f ff and every lookup failed, including "ldoc1", which left the chip without its I/O
 * supply. UniXNU-umi's UmiRpmh.c uses DB_BASE 0x80860000, DB_SIZE 0x20000 for the same reason.
 */
#define CMD_DB_PHYS             0x80860000UL
#define CMD_DB_SIZE             0x20000UL
#define CMD_DB_VERSION          1u
#define CMD_DB_MAGIC            0x0c0330dbu      /* "db 30 03 0c" in memory order */

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

	if (db->version != CMD_DB_VERSION || db->magic[0] != 0xdb || db->magic[1] != 0x30 ||
	    db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		printf("fts: no command database at %#lx (version %u, magic %02x %02x %02x %02x)\n",
		    (uint64_t)CMD_DB_PHYS, db->version, db->magic[0], db->magic[1], db->magic[2],
		    db->magic[3]);
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
			if (memcmp(entry[i].id, id, sizeof(id)) != 0) {
				continue;
			}
			/* UmiRpmh.c: the RPMh resource must look like one this vote can address. */
			if (entry[i].address > 0x7ffff || (entry[i].address & 3) != 0 ||
			    ((entry[i].address >> 16) & 7) != 4 ||
			    (((entry[i].address + 4) >> 16) & 7) != 4) {
				printf("fts: cmd_db: \"%s\" has an address this vote cannot use (%#x)\n", name,
				    entry[i].address);
				return 0;
			}
			return entry[i].address;
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

	if (base == 0) {
		printf("fts: RPMh: no regulator \"ldoc1\" in the command database\n");
		return false;
	}
	/*
	 * One vote, on the enable resource only. UmiRpmh.c is explicit about this: the rail is already
	 * set to 1800 mV by the boot loader, and the vote is "VRM enable index1, data1/0; no
	 * voltage/mode request". Writing the voltage and mode as well is what this used to do, which
	 * programs a regulator the boot loader owns instead of asking RPMh for it.
	 */
	{
		uint32_t address = base + VRM_ENABLE;
		uint32_t data = 1;

		printf("fts: RPMh: ldoc1 at %#x, voting %#x = 1\n", base, address);
		if (!rpmh_write(&address, &data, 1)) {
			printf("fts: RPMh: the ldoc1 enable vote did not complete\n");
			return false;
		}
	}
	printf("fts: I/O supply on (the ldoc1 enable vote went through)\n");
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
#define SPMI_OBSERVER_SIZE      0x10000UL       /* 512 channels of 0x80 */
#define SPMI_WRITE_PAGE_SIZE    0x1000UL        /* one channel's page in the write bank */
#define SPMI_ARB_VERSION        0x50010000u     /* what the arbiter must report */

#define SPMI_CHANNEL_MAP(n)     (spmi_core + 0x900 + 4 * (n))   /* [19:8] ppid */
#define SPMI_OWNER(n)           (spmi_cnfg + 0x700 + 4 * (n))   /* [2:0] the EE that may write */
#define SPMI_CMD_OFFSET         0x00            /* the command word */
#define SPMI_STATUS_OFFSET      0x08            /* bit 0 done, bits 1..3 errors */
#define SPMI_DATA_OFFSET        0x10            /* the byte to write */
#define SPMI_OBS_DATA_OFFSET    0x18            /* the byte read on the observer bank */
#define SPMI_READ_OP            (1u << 27)

static vm_offset_t spmi_core, spmi_cnfg, spmi_observer;
static vm_offset_t spmi_write_page;             /* the write bank, mapped for one channel */
static uint32_t spmi_write_channel = ~0u;
static lck_spin_t *spmi_lock_p;
static lck_grp_t *spmi_lock_grp;
static LCK_SPIN_DECLARE(spmi_guard, &spmi_lock_grp);

/* one SPMI transfer: sid = the PMIC (2 = PM8150B), address = the 16-bit register */
static const char *
spmi_transfer(uint8_t sid, uint16_t address, uint8_t *value, bool write)
{
	uint32_t ppid = ((uint32_t)sid << 8) | (address >> 8);
	uint32_t found = ~0u, owner = 7;
	vm_offset_t channel;
	const char *problem = "the arbiter did not finish";

	/* Which channel serves that peripheral. The two tables are already mapped; the per-channel
	 * banks are not, and are dealt with below. */
	lck_spin_lock(&spmi_guard);
	for (uint32_t i = 0; i < 512; i++) {
		uint32_t map = rd(SPMI_CHANNEL_MAP(i));

		if (map == 0 || ((map >> 8) & 0xfff) != ppid) {
			continue;
		}
		uint32_t who = rd(SPMI_OWNER(i)) & 7;

		if (found == ~0u || (write && who == 0)) {
			found = i;
			owner = who;
		}
		if (write && who == 0) {
			break;                  /* ours to write through: prefer it */
		}
	}
	lck_spin_unlock(&spmi_guard);

	if (found == ~0u) {
		return "no channel of the arbiter serves that peripheral";
	}
	if (write && owner != 0) {
		return "that peripheral is not this EE's to write";
	}

	/*
	 * The window to use, and it has to be a mapped one. SPMI_WRITE_PHYS and SPMI_OBSERVER_PHYS
	 * above are physical addresses: adding a channel offset to them and then reading or writing
	 * through the result is what made this driver die with "Kernel data abort ... far 0xe606700"
	 * the moment the board rail was first looked at. ml_io_map() is called outside the lock
	 * because it may sleep; only one channel is ever written, so its page is kept.
	 */
	if (write) {
		if (spmi_write_channel != found) {
			vm_offset_t page = ml_io_map(SPMI_WRITE_PHYS + (vm_offset_t)found * 0x10000,
			    SPMI_WRITE_PAGE_SIZE);

			if (page == 0) {
				return "the write bank of that channel cannot be mapped";
			}
			spmi_write_page = page;
			spmi_write_channel = found;
		}
		channel = spmi_write_page;
	} else {
		if (spmi_observer == 0) {
			return "the observer bank cannot be mapped";
		}
		channel = spmi_observer + (vm_offset_t)found * 0x80;
	}
	lck_spin_lock(&spmi_guard);
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
	IOSleep(4);                                    /* the 4000us startup delay of that rail */

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

/*
 * Put a line where the bus wants it, as an open drain does: "high" is not the same as driving the
 * pin up, it is letting go of it and trusting the pull-up. Only the output-enable bit changes and
 * the output latch is kept at zero for the whole transfer, which is exactly what UniXNU-umi's
 * Line() does:
 *
 *     MmioWrite32 (GPIO_CTL (Pin), mGpioCtl[Index] | (Released ? 0 : GPIO_OE));
 *
 * This driver used to drive SDA both ways. A push-pull master that holds SDA high while the
 * controller pulls it low to acknowledge a byte wins that fight, so no ACK was ever seen and every
 * read came back 0xff -- which is what "the controller information is not a valid block (0xff 0xff)"
 * was: a bus with nothing answering on it.
 */
static void
i2c_line(unsigned pin, bool high)
{
	uint32_t ctl = i2c_pin_ctl(pin);

	wr(GPIO_CFG(pin), ctl);                                 /* released: an input */
	wr(GPIO_IO(pin), rd(GPIO_IO(pin)) & ~GPIO_OUT_BIT);     /* the latch stays at zero */
	if (!high) {
		wr(GPIO_CFG(pin), ctl | GPIO_OE_BIT);           /* now sink the line */
	}
}

/* kept as the name the transfer code reads better with */
static void
i2c_drive(unsigned pin, bool high)
{
	i2c_line(pin, high);
}

/* back to an input, which is how the chip drives ACK and data */
static void
i2c_release(unsigned pin)
{
	i2c_line(pin, true);
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
	/*
	 * Acknowledge every byte but the last, and the polarity here matters: "high" means the line is
	 * released, so acknowledging is low. This passed !last, which acknowledged only the final byte
	 * and refused every one before it -- the controller then stopped sending, and a sixteen-byte
	 * chunk came back as one good byte followed by 0xff.
	 */
	i2c_drive(PIN_SDA, last);                       /* sink to ACK, release for the last (NACK) */
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

/*
 * A transfer that is cut short can leave the controller holding SDA down, and then every later
 * transfer fails at its start condition ("the bus was busy"). Clocking SCL up to nine times lets
 * the slave finish the byte it was in the middle of, and a stop returns the bus to idle.
 */
static void
i2c_bus_recover(void)
{
	for (unsigned i = 0; i < 9 && !i2c_read_line(PIN_SDA); i++) {
		i2c_clock_high();
		i2c_clock_low();
	}
	i2c_stop();
}

/* write `wn` bytes then read `rn` back, the shape every FTS transfer has */
static const char *
i2c_transfer(const uint8_t *write, uint32_t wn, uint8_t *read, uint32_t rn)
{
	if (!i2c_read_line(PIN_SDA) && i2c_read_line(PIN_SCL)) {
		i2c_bus_recover();
	}
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
	IOSleep(50);                           /* WAIT_AFTER_SENSEOFF */
	problem = i2c_transfer(on, sizeof(on), NULL, 0);
	return problem;
}

/* ---- one event, decoded as UniXNU-umi's umi_fts_parse ----------------------------------------- */

static struct {
	bool down;
	uint32_t x, y;          /* in screen pixels, as the driver posts them */
} contact[FTS_MAX_POINTS];

/*
 * How many samples actually reached /dev/redwood-touch, and the first few of them with their
 * coordinates. "2158 events read" said nothing about whether anything was posted: an event that is
 * a status byte, a lift with nothing down, or out of the panel is read and then dropped, and the run
 * looks identical from the outside.
 */
/*
 * Set to 0 to count what would be posted without putting it on the queue. The kind check was the
 * reason nothing was posted at all, and with it fixed the system started freezing instead, so the
 * two questions -- "is the parse right" and "does handing it to user space hurt" -- are separated
 * by turning the hand-off off for one run.
 */
#define FTS_POST_TO_USER 1      /* 0: count the samples only, 1: hand them to user space */
/*
 * Moves are left out for this run. Handing every sample over froze the system while simply counting
 * them did not, and a tap needs only a down and an up, so this separates "the hand-off does not work
 * at all" from "the hand-off is being handed far too much".
 */
#define FTS_POST_MOVES   1

static uint32_t fts_posts;
static uint32_t fts_shown;

static bool
fts_show_post(void)
{
	return fts_shown++ < 12;
}


/*
 * Returns false when the controller asked to be reset or reported a fault: the working build
 * stops its worker and lets the startup state machine run the reset, rather than reading on.
 */
static bool
fts_parse(const uint8_t ev[FTS_EVENT_LEN], uint32_t max_x, uint32_t max_y)
{
	uint32_t id, type, x, y;
	bool changed;

	if (ev[0] == 0) {
		return true;                                    /* no event in this window */
	}
	if (ev[0] == FTS_EV_RESET || ev[0] == FTS_EV_FAULT) {
		/*
		 * Nothing is printed from here. Formatting a string here is what crashed the kernel in
		 * __doprnt (data abort, far equal to the bytes of "reset"), and the caller already says
		 * that a reset was asked for, so the reason only has to travel back as the return value.
		 */
		for (unsigned i = 0; i < FTS_MAX_POINTS; i++) {
			contact[i].down = false;
		}
		return false;
	}
	/*
	 * Which kind of report this is: the HIGH nibble, 1 to 3. The low nibble carries contact-size
	 * bits and is not a fixed signature -- UniXNU-umi's umi_fts_point_event() is
	 *
	 *     unsigned kind = code >> 4; return kind >= 1 && kind <= 3;
	 *
	 * This used to test "(ev[0] & 0x0f) == 3" and return early, calling it a status byte. Every
	 * report whose size bits happened to be 3 was therefore thrown away before its contact was
	 * looked at, which is why a run could read 370 events and post none of them.
	 */
	if (((ev[0] >> 4) < 1) || ((ev[0] >> 4) > 3)) {
		return true;                                    /* not a contact report */
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
#if FTS_POST_TO_USER
			redwood_touch_post(REDWOOD_TOUCH_UP, id, contact[id].x, contact[id].y);
#endif
			if (fts_show_post()) {
				printf("fts: post up id %u at %u, %u\n", (unsigned)id,
				    (unsigned)contact[id].x, (unsigned)contact[id].y);
			}
			fts_posts++;
		}
		return true;
	}
	if (x > max_x || y > max_y || max_x < 2 || max_y < 2) {
		return true;                                    /* out of the panel: their "bad" */
	}
	/*
	 * To the scale the queue carries, which is 0..65535 and not pixels. redwood_goodix.c, the
	 * driver this device shared the interface with, does exactly this before posting:
	 *
	 *     x = (uint32_t)(((uint64_t)x * 65535) / (max_report_x - 1));
	 *     y = (uint32_t)(((uint64_t)y * 65535) / (max_report_y - 1));
	 *
	 * Posting raw pixels instead put a touch at y = 1700 of 65535, about 2.6% down the screen,
	 * which is why the panel only answered along the very top edge.
	 */
	x = (uint32_t)(((uint64_t)x * 65535) / (max_x - 1));
	y = (uint32_t)(((uint64_t)y * 65535) / (max_y - 1));
	changed = !contact[id].down || contact[id].x != x || contact[id].y != y;
	contact[id].x = x;
	contact[id].y = y;
	if (!contact[id].down) {
		contact[id].down = true;
#if FTS_POST_TO_USER
		redwood_touch_post(REDWOOD_TOUCH_DOWN, id, x, y);
#endif
		if (fts_show_post()) {
			printf("fts: post down id %u at %u, %u\n", (unsigned)id, (unsigned)x, (unsigned)y);
		}
		fts_posts++;
	} else if (changed) {
#if FTS_POST_TO_USER
		redwood_touch_post(REDWOOD_TOUCH_MOVE, id, x, y);
#endif
		if (fts_show_post()) {
			printf("fts: post move id %u at %u, %u\n", (unsigned)id, (unsigned)x, (unsigned)y);
		}
		fts_posts++;
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
	IOSleep(15);

	gpio_drive(GPIO_AVDD, true);                    /* avdd: GPIO 69, enable-active-high */
	IOSleep(20);                                   /* the rail settles before the chip is let out */

	/*
	 * ftsCore.c:85 holds the reset line low for 10 ms before releasing it. It has been low since
	 * the top of this function, so that much is already spent; the release is the pulse.
	 */
	gpio_drive(GPIO_RESET, true);                   /* out of reset; the chip boots */
	/*
	 * And then it is given time to boot. fts_reset_proc() in the vendor driver leaves 300 ms
	 * between releasing reset and the first transfer, because the controller runs its own firmware
	 * before it will answer on the bus. Ten milliseconds was tried here first, and the symptom of
	 * asking too early is exactly what this driver kept reporting: every byte read back as 0xff,
	 * because nothing is on the bus yet to acknowledge anything.
	 */
	IOSleep(300);
	/*
	 * What the three pins actually look like once the rails are up: the level, and whether the
	 * output driver is on. Reading L1 over SPMI to prove the supply is up turned out to be a way to
	 * hang this handset -- probing SIDs it does not have -- so the pins are the evidence instead.
	 * Reset has to read high and AVDD high; if reset sits low the controller is still held in reset
	 * and will not acknowledge anything on the bus, which looks exactly like an empty bus.
	 */
	printf("fts: pins now: reset %u (oe %u), avdd %u (oe %u), irq %u\n",
	    (uint64_t)gpio_get(GPIO_RESET), (uint64_t)((rd(GPIO_CFG(GPIO_RESET)) & GPIO_OE_BIT) != 0),
	    (uint64_t)gpio_get(GPIO_AVDD), (uint64_t)((rd(GPIO_CFG(GPIO_AVDD)) & GPIO_OE_BIT) != 0),
	    (uint64_t)gpio_get(GPIO_IRQ));
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
	/*
	 * What the two bus pins were muxed to before this driver took them. Function 0 is plain GPIO,
	 * function 1 on Kona GPIO36/37 is qup13 -- the GENI I2C engine the boot loader and Android use.
	 * If they read as 1 here, the engine still owns the pins on this handset and a hand-driven
	 * transfer has to win them back rather than assume they are already GPIO.
	 */
	printf("fts: pins before takeover: gpio%d ctl %#x (func %u), gpio%d ctl %#x (func %u)\n", PIN_SDA,
	    rd(GPIO_CFG(PIN_SDA)), (unsigned)((rd(GPIO_CFG(PIN_SDA)) & GPIO_FUNC_MASK) >> 2), PIN_SCL,
	    rd(GPIO_CFG(PIN_SCL)), (unsigned)((rd(GPIO_CFG(PIN_SCL)) & GPIO_FUNC_MASK) >> 2));
	MARK_NOW(1);
	i2c_pins_init();
	delay(5);
	/*
	 * The bus is deliberately NOT checked for idle here. This used to read SDA and SCL at this
	 * point and give up when either was low, which it always was: the controller has no supply
	 * yet, so its pins sit at ground through the chip's own protection diodes. UniXNU-umi checks
	 * the two lines only after AcquirePins() has powered the rails and pulsed reset, and that is
	 * the order used from here on.
	 */
	printf("fts: pins taken; SDA %u, SCL %u before power (the rails are still off)\n",
	    (uint64_t)i2c_read_line(PIN_SDA), (uint64_t)i2c_read_line(PIN_SCL));

	/*
	 * The board's own boost rail first: UniXNU-umi's attempt 80 found this pin latched low and
	 * turning it on is what made touch work on this handset. Everything below depends on it.
	 */
	spmi_core = ml_io_map(SPMI_CORE_PHYS, SPMI_CORE_SIZE);
	spmi_cnfg = ml_io_map(SPMI_CNFG_PHYS, SPMI_CNFG_SIZE);
	/* The read bank too: every register read goes through a channel of it, so it has to be mapped
	 * before the first transfer or the first read is a data abort. */
	spmi_observer = ml_io_map(SPMI_OBSERVER_PHYS, SPMI_OBSERVER_SIZE);
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
	delay(5);
	MARK_OK(2);     /* slot 2 green = powered and out of reset */
	printf("fts: powered and out of reset; irq line reads %u\n", (uint64_t)gpio_get(GPIO_IRQ));

	/*
	 * Now the release check UniXNU-umi's AcquirePins() makes once the rails are up: both lines
	 * must have been let go high by the controller before a start condition can be made. With the
	 * supply on this is a real answer rather than the unpowered chip holding SDA down.
	 */
	if (!i2c_read_line(PIN_SDA) || !i2c_read_line(PIN_SCL)) {
		MARK_BAD(1);
		printf("fts: the bus is still not idle after power-up (SDA %u, SCL %u)\n",
		    (uint64_t)i2c_read_line(PIN_SDA), (uint64_t)i2c_read_line(PIN_SCL));
		thread_terminate(current_thread());
		return;
	}
	MARK_OK(1);     /* slot 1 green = powered, out of reset, and the bus is idle */
	printf("fts: bus idle after power-up (SDA and SCL both high)\n");

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
		struct fts_sysinfo sys = { 0 };
		int attempt;

		/*
		 * The controller answers the first request only once its own firmware is up. Rather than
		 * decide on one attempt, ask a few times with a gap: a bus that reads 0xff is a bus with
		 * nothing listening yet, which is a wait, not a fault.
		 */
		for (attempt = 1; attempt <= 6; attempt++) {
			got = 0;
			problem = fts_read_controller(info, &got);
			if (problem == NULL) {
				sys = fts_sysinfo(info, got);
				if (sys.valid) {
					break;
				}
			}
			printf("fts: attempt %d: no usable answer yet (%s, first bytes %02x %02x)\n", attempt,
			    problem != NULL ? problem : "not a valid block", info[0], info[1]);
			IOSleep(100);
		}
		if (!sys.valid) {
			printf("fts: the controller never answered (%u bytes, first %02x %02x)\n", got, info[0],
			    info[1]);
			thread_terminate(current_thread());
			return;
		}
		printf("fts: answered on attempt %d\n", attempt);
		printf("fts: chip %#x %#x, fw %#x, svn %#x, config %#x\n", sys.chip0, sys.chip1, sys.fw, sys.svn,
		    sys.config);
		/*
		 * The system information calls the long edge resX and the short edge resY, which is not the
		 * way round a display has them: this panel reads back 2339 and 1079, while a touch point is
		 * valid over x 0..1079 and y 0..2339 (UniXNU-umi's umi_fts_parse rejects anything else, and
		 * umi_fts_report scales x by 1079 and y by 2339). Taking them as they come meant every
		 * touch past y = 1079 was thrown away as out of range, which is most of the screen, and the
		 * run looked like "2158 events read, nothing happens".
		 */
		if (sys.res_x != 0 && sys.res_y != 0) {
			max_x = sys.res_y;
			max_y = sys.res_x;
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
				printf("fts: controller asked for a reset (%u events so far)\n", (unsigned)nonempty);
				break;
			}
			if (buf[0] != 0) {
				points++;
			}
		}
		if (mach_absolute_time() >= next_report) {
			next_report = mach_absolute_time() + report_interval;
			printf("fts: %u polls, %u irq-high, %u non-empty, %u events, %u resets, %u errors, "
			    "%u posted\n", (unsigned)polls, (unsigned)irq_high, (unsigned)nonempty,
			    (unsigned)points, (unsigned)resets, (unsigned)errors, (unsigned)fts_posts);
		}
		/*
		 * IOSleep, not delay(): delay() spins without ever giving the processor up, and this loop
		 * never ends, so one core stayed pegged for the whole run and the machine never got as far
		 * as the boot finishing. IOSleep blocks the thread and lets everything else run.
		 */
		IOSleep(irq ? 2 : 8);                        /* 2 ms while a finger is on it */
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
