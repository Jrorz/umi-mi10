/*
 * The touch screen of the Xiaomi Mi 10 (umi): a Goodix GT9886 on the I2C bus of GENI serial engine
 * 4 (a94000), as a source of touches for /dev/redwood-touch (sys/redwood_input.h).
 *
 * Boot-arg touchscreen=2 (touchscreen=1 is redwood_goodix.c's SPI chip, a GT9916 on redwood).
 *
 * EVERY constant below has a source; nothing here was worked out by fitting to a crash. The
 * sources, in the order they were used:
 *
 *   the phone's own device tree (umi.dts)      chip address 0x5D, the pins, the panel size, the
 *                                             power sequencing, and the clocks
 *   goodix_ts_i2c.c of the phone's kernel     the register map of the chip and the report format
 *     (KONA-DEVS/kernel_xiaomi_sm8250,
 *      drivers/input/touchscreen/
 *      goodix_driver_gt9886/)
 *   i2c-qcom-geni.c of the phone's kernel     the I2C side of the GENI engine: the command
 *     (drivers/i2c/busses/)                   opcodes, where the slave address goes, and the
 *                                             transfer-length registers
 *   qcom,gcc-sm8250.h (Linux)                 what umi.dts's clock indices 0x74/0x86/0x87 are
 *   redwood_goodix.c (this repository)        the GENI layout and the RPMh/GCC bring-up already
 *                                             known to work on this SoC; its power_on() and
 *                                             clocks_on() are shared here with the umi parameters
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/locks.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);        /* as redwood_goodix.c declares it */
extern void loader_log_line(const char *line);

/*
 * Every diagnostic goes to the loader's own log (1.5 MB, read back on return or with "oem log")
 * in addition to the kernel's printf: the kernel log is a small ring that has overwritten the
 * driver's early lines by the time the machine comes back. That is why the first bring-up attempts
 * showed no goodix-i2c lines at all.
 */
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

/* ---- the addresses ------------------------------------------------------------------------------
 *
 * SE_PHYS, TLMM_PHYS and GCC_PHYS: umi.dts (i2c@a94000, tlmm, gcc@100000) -- the same numbers
 * redwood_goodix.c uses.
 */
#define SE_PHYS                 0x00a94000UL
#define SE_SIZE                 0x4000UL
#define TLMM_PHYS               0x0f100000UL
#define TLMM_SIZE               0x100000UL
#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x60000UL
#define CMD_DB_PHYS             0x80880000UL    /* umi.dts: cmd-db@80880000 */
#define CMD_DB_SIZE             0x14000UL       /* umi.dts: size 0x14000 */
#define RSC_PHYS                0x18220000UL    /* the application processor's "DRV 2" */
#define RSC_SIZE                0x10000UL

static vm_offset_t se, tlmm, gcc, rsc, cmd_db;

/* vm_offset_t: these are 64-bit kernel virtual addresses (a uint32_t truncated them once). */
static inline uint32_t rd(vm_offset_t address) { return *(volatile uint32_t *)address; }
static inline void wr(vm_offset_t address, uint32_t value) { *(volatile uint32_t *)address = value; }

/* ---- the engine, as redwood_goodix.c has it (proven on this SoC) ------------------------------ */

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
#define GENI_TX_FIFO            (se + 0x700)    /* the bytes to send */
#define GENI_RX_FIFO            (se + 0x780)    /* the bytes received */
#define GENI_TX_FIFO_STATUS     (se + 0x800)
#define GENI_RX_FIFO_STATUS     (se + 0x804)
#define GENI_TX_WATERMARK       (se + 0x80c)
#define GENI_RX_WATERMARK       (se + 0x810)
#define GENI_RX_RFR_WATERMARK   (se + 0x814)
#define SE_GSI_EVENT_EN         (se + 0xe18)
#define SE_IRQ_EN               (se + 0xe1c)
#define SE_HW_PARAM_0           (se + 0xe24)    /* [21:16] FIFO depth in words */

/* the I2C transfer lengths: i2c-qcom-geni.c (SE_I2C_TX_TRANS_LEN / SE_I2C_RX_TRANS_LEN) */
#define SE_I2C_TX_TRANS_LEN     (se + 0x26c)
#define SE_I2C_RX_TRANS_LEN     (se + 0x270)
#define SE_I2C_SCL_COUNTERS     (se + 0x278)
#define SE_GENI_IOS             (se + 0x908)    /* bit 1: the transfer ended with a NACK */

/* the command: i2c-qcom-geni.c (I2C_READ, I2C_WRITE_READ, SLV_ADDR_MSK) */
#define GENI_I2C_OP_READ        0x2
#define GENI_I2C_OP_WRITE_READ  0x3
#define GENI_SLV_ADDR_SHIFT     9               /* SLV_ADDR_MSK = GENMASK(15, 9) */
#define GENI_CMD_DONE           0x2             /* M_CMD_DONE_EN, in the status register */

/* ---- the chip: goodix_ts_i2c.c of the phone's kernel -------------------------------------------- */

#define GOODIX_I2C_ADDR         0x5d            /* umi.dts: goodix@5d */
#define TS_ADDR_LENGTH          2               /* the register address is two bytes */
#define TS_REG_COORDS_BASE      0x824E          /* where a report is read from */
#define BYTES_PER_COORD         8               /* one contact in a report */
#define IRQ_HEAD_LEN_NOR        2               /* event byte and touch count */
#define GOODIX_TOUCH_EVENT      0x80            /* in the event byte */
#define GOODIX_MAX_TOUCHES      10              /* umi.dts: goodix,panel-max-id = 0xa */

/* ---- the pins: umi.dts ------------------------------------------------------------------------- */

#define GPIO_RESET              38              /* goodix,reset-gpio = <&tlmm 38> */
#define GPIO_IRQ                39              /* goodix,irq-gpio   = <&tlmm 39> */
#define GPIO_AVDD               69              /* touch_vddio_vreg: gpio = <&tlmm 0x45>, active-high */
#define GOODIX_POWER_ON_DELAY_US 100000         /* umi.dts: goodix,power-on-delay-us = 0x186a0 */
#define GOODIX_POWER_OFF_DELAY_US 5000          /* umi.dts: goodix,power-off-delay-us = 0x1388 */

#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))
#define GPIO_IO(n)              (tlmm + 0x1000UL * (n) + 4)
#define PULL_NONE               0
#define PULL_UP                 3

/* ---- the panel: umi.dts ------------------------------------------------------------------------ */

#define PANEL_MAX_X             1079            /* goodix,panel-max-x = 0x437 */
#define PANEL_MAX_Y             2339            /* goodix,panel-max-y = 0x923 */

static uint32_t fifo_depth, se_source_hz, se_clock_select;

/* ---- pins --------------------------------------------------------------------------------------- */

static void
gpio_configure(unsigned pin, unsigned function, unsigned pull, unsigned ma, bool output)
{
	wr(GPIO_CFG(pin), pull | function << 2 | (ma / 2 - 1) << 6 | (output ? 1u << 9 : 0));
}

static void
gpio_set(unsigned pin, bool high)
{
	wr(GPIO_IO(pin), high ? 2 : 0);
}

static bool
gpio_get(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

static void
gpio_output(unsigned pin, bool high, unsigned pull, unsigned ma)
{
	gpio_set(pin, high);            /* the level first, so that the pin never drives the wrong one */
	gpio_configure(pin, 0, pull, ma, true);
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
		printf("goodix-i2c: no command database (magic %02x %02x %02x %02x)\n", db->magic[0], db->magic[1],
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
	printf("goodix-i2c: RPMh: timeout waiting for %s (register %#x = %#x)\n", what, reg, rd(TCS(reg)));
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
			printf("goodix-i2c: RPMh: the command slots stay busy\n");
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
		printf("goodix-i2c: RPMh: no regulator \"ldoc1\" (address %#x)\n", base);
		return false;
	}
	printf("goodix-i2c: RPMh: ldoc1 is at %#x\n", base);
	if (!rpmh_write(address, data, 3)) {
		return false;
	}
	printf("goodix-i2c: I/O supply on (1.8 V)\n");
	return true;
}

/* ---- clocks (GCC), as redwood_goodix.c -----------------------------------------------------------
 *
 * umi.dts gives i2c@a94000 the clocks 0x74/0x86/0x87. In qcom,gcc-sm8250.h those are
 * GCC_QUPV3_WRAP1_S5_CLK_SRC, GCC_QUPV3_WRAP_1_S_AHB_CLK and GCC_QUPV3_WRAP_2_M_AHB_CLK:
 * the same WRAP1 "SE5" redwood_goodix.c drives, so its register numbers apply unchanged.
 */

#define GCC_VOTE                (gcc + 0x52008)         /* the application processor's votes */
#define VOTE_QUP1_CORE_2X       (1u << 18)
#define VOTE_QUP1_CORE          (1u << 19)
#define VOTE_QUP1_M_AHB         (1u << 20)
#define VOTE_QUP1_S_AHB         (1u << 21)
#define VOTE_QUP1_SE5           (1u << 27)
#define GCC_QUP1_M_AHB_CBCR     (gcc + 0x18004)         /* bit 31: the clock is off */
#define GCC_QUP1_S_AHB_CBCR     (gcc + 0x18008)
#define GCC_QUP1_SE5_CBCR       (gcc + 0x185fc)
#define GCC_QUP1_SE5_CMD        (gcc + 0x18600)
#define GCC_QUP1_SE5_CFG        (gcc + 0x18604)
#define GCC_QUP1_SE5_DFSR       (gcc + 0x18614)
#define GCC_QUP1_SE5_PERF_CFG(l) (gcc + 0x1861c + 4 * (l))
#define GCC_QUP1_SE5_PERF_M(l)  (gcc + 0x1865c + 4 * (l))
#define GCC_QUP1_SE5_PERF_N(l)  (gcc + 0x1869c + 4 * (l))
#define CLOCK_OFF               (1u << 31)
#define XO_HZ                   19200000

static uint32_t
rcg_rate(uint32_t cfg, uint32_t m, uint32_t not_n_minus_m)
{
	static const uint32_t parent[8] = { XO_HZ, 600000000, 0, 0, 0, 0, 300000000, 0 };
	uint64_t hz = parent[cfg >> 8 & 7];
	uint32_t divider = cfg & 0x1f, mode = cfg >> 12 & 3;

	if (divider) {
		hz = hz * 2 / (divider + 1);
	}
	if (mode) {
		uint32_t n = (~not_n_minus_m & 0xffff) + (m & 0xffff);

		if (n) {
			hz = hz * (m & 0xffff) / n;
		}
	}
	return (uint32_t)hz;
}

static bool
clocks_on(void)
{
	wr(GCC_VOTE, rd(GCC_VOTE) | VOTE_QUP1_CORE_2X | VOTE_QUP1_CORE | VOTE_QUP1_M_AHB | VOTE_QUP1_S_AHB);
	if (rd(GCC_QUP1_SE5_DFSR) & 1) {
		for (uint32_t level = 0; level < 8; level++) {
			uint32_t hz = rcg_rate(rd(GCC_QUP1_SE5_PERF_CFG(level)), rd(GCC_QUP1_SE5_PERF_M(level)),
			    rd(GCC_QUP1_SE5_PERF_N(level)));

			if (hz >= 4000000 && (se_source_hz == 0 || hz < se_source_hz)) {
				se_source_hz = hz;
				se_clock_select = level;
			}
		}
		if (se_source_hz == 0) {
			printf("goodix-i2c: no usable preset clock rate\n");
			return false;
		}
	} else {
		wr(GCC_QUP1_SE5_CFG, (rd(GCC_QUP1_SE5_CFG) & ~0x371fu) | 1);
		wr(GCC_QUP1_SE5_CMD, rd(GCC_QUP1_SE5_CMD) | 1);
		for (int i = 0; i < 500 && (rd(GCC_QUP1_SE5_CMD) & 1); i++) {
			delay(10);
		}
		if (rd(GCC_QUP1_SE5_CMD) & 1) {
			printf("goodix-i2c: the serial engine's clock does not take its configuration (%#x)\n", rd(GCC_QUP1_SE5_CMD));
			return false;
		}
		se_source_hz = XO_HZ;
		se_clock_select = 0;
	}
	wr(GCC_VOTE, rd(GCC_VOTE) | VOTE_QUP1_SE5);
	for (int i = 0; i < 1000; i++) {
		if (!((rd(GCC_QUP1_M_AHB_CBCR) | rd(GCC_QUP1_S_AHB_CBCR) | rd(GCC_QUP1_SE5_CBCR)) & CLOCK_OFF)) {
			return true;
		}
		delay(10);
	}
	printf("goodix-i2c: clocks stay off: %#x %#x %#x\n", rd(GCC_QUP1_M_AHB_CBCR), rd(GCC_QUP1_S_AHB_CBCR),
	    rd(GCC_QUP1_SE5_CBCR));
	return false;
}

/* The chip's supplies, as redwood_goodix.c's power_on() with umi's pins. */
static void
power_on(void)
{
	gpio_output(GPIO_RESET, false, PULL_UP, 8);
	gpio_configure(GPIO_IRQ, 0, PULL_UP, 8, false);
	io_supply_on();
	delay(3000);
	gpio_output(GPIO_AVDD, true, PULL_NONE, 2);    /* touch_vddio_vreg is enable-active-high */
	delay(15000);
}

/* ---- the engine in I2C mode --------------------------------------------------------------------- */

static const char *
engine_init(void)
{
	uint32_t divider;

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

	/* one byte per FIFO word, as an I2C transfer is byte by byte */
	wr(GENI_TX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_RX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_BYTE_GRAN, 0);

	/* 400 kHz: the divider is the engine's clock over the bus rate */
	divider = (se_source_hz + 400000 - 1) / 400000;
	wr(GENI_CLK_SEL, se_clock_select & 7);
	wr(GENI_SER_M_CLK_CFG, (divider ? divider : 1) << 4 | 1);
	return NULL;
}

/* Put the bytes of a transfer into the FIFO, four to a word. */
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

/* Wait for the command started in GENI_M_CMD0 to finish. */
static const char *
wait_done(void)
{
	uint64_t deadline, interval;
	uint32_t status;

	nanoseconds_to_absolutetime(20ull * NSEC_PER_MSEC, &interval);
	deadline = mach_absolute_time() + interval;
	while ((rd(GENI_STATUS) & 1) != 0) {
		if (mach_absolute_time() > deadline) {
			wr(GENI_M_CMD0, 0);             /* cancel */
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

/* One transfer: 2 address bytes out, then (when in_length is not 0) bytes back in. */
static const char *
i2c_transfer(uint16_t reg, uint32_t in_length)
{
	uint8_t addr[TS_ADDR_LENGTH] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xff) };

	wr(SE_I2C_TX_TRANS_LEN, TS_ADDR_LENGTH);
	wr(SE_I2C_RX_TRANS_LEN, in_length);
	fifo_put(addr, TS_ADDR_LENGTH);
	wr(GENI_M_CMD0, (in_length ? GENI_I2C_OP_WRITE_READ : GENI_I2C_OP_READ) << 27 |
	    (GOODIX_I2C_ADDR << GENI_SLV_ADDR_SHIFT));
	return wait_done();
}

static const char *
i2c_read(uint16_t reg, uint8_t *in, uint32_t in_length)
{
	const char *problem = i2c_transfer(reg, in_length);
	uint32_t got = 0;

	if (problem != NULL) {
		return problem;
	}
	while (got < in_length) {
		uint32_t status = rd(GENI_RX_FIFO_STATUS), words = status & 0x1ffffff;

		if (words == 0) {
			break;
		}
		for (uint32_t w = 0; w < words && got < in_length; w++) {
			uint32_t word = rd(GENI_RX_FIFO);

			for (uint32_t k = 0; k < 4 && got < in_length; k++) {
				in[got++] = (uint8_t)(word >> (8 * k));
			}
		}
	}
	return got == in_length ? NULL : "the read came back short";
}

/* ---- the report --------------------------------------------------------------------------------- */

static struct {
	bool down, seen;
	uint32_t x, y;
} contact[GOODIX_MAX_TOUCHES];

static void
report(const uint8_t *touches, uint32_t count)
{
	static unsigned shown;

	for (int i = 0; i < GOODIX_MAX_TOUCHES; i++) {
		contact[i].seen = false;
	}
	for (uint32_t i = 0; i < count && i < GOODIX_MAX_TOUCHES; i++) {
		const uint8_t *t = touches + BYTES_PER_COORD * i;
		uint32_t id = t[0] >> 4, kind = t[0] & 0xf;
		uint32_t x = (uint32_t)t[2] | ((uint32_t)t[3] << 8);
		uint32_t y = (uint32_t)t[4] | ((uint32_t)t[5] << 8);

		if (id >= GOODIX_MAX_TOUCHES || kind == 1 || kind == 3) {
			continue;                       /* a stylus */
		}
		if (shown < 40) {
			shown++;
			printf("goodix-i2c: finger %u at %u, %u\n", id, x, y);
		}
		x = x >= PANEL_MAX_X ? 65535 : x * 65535 / PANEL_MAX_X;
		y = y >= PANEL_MAX_Y ? 65535 : y * 65535 / PANEL_MAX_Y;
		contact[id].seen = true;
		if (!contact[id].down) {
			redwood_touch_post(REDWOOD_TOUCH_DOWN, id, x, y);
		} else if (contact[id].x != x || contact[id].y != y) {
			redwood_touch_post(REDWOOD_TOUCH_MOVE, id, x, y);
		}
		contact[id].down = true;
		contact[id].x = x;
		contact[id].y = y;
	}
	for (uint32_t id = 0; id < GOODIX_MAX_TOUCHES; id++) {
		if (contact[id].down && !contact[id].seen) {
			contact[id].down = false;
			redwood_touch_post(REDWOOD_TOUCH_UP, id, contact[id].x, contact[id].y);
		}
	}
}

/* A thread: look at the interrupt line, read a report when the chip says it has one. */
static void
touch_thread(void *arg __unused, wait_result_t wresult __unused)
{
	uint8_t buf[IRQ_HEAD_LEN_NOR + BYTES_PER_COORD * GOODIX_MAX_TOUCHES];
	static unsigned said;
	const char *problem;

	printf("goodix-i2c: mapping the registers (SE %#x, TLMM %#x, GCC %#x, RSC %#x, cmd_db %#x)\n", SE_PHYS,
	    TLMM_PHYS, GCC_PHYS, RSC_PHYS, CMD_DB_PHYS);
	se = ml_io_map(SE_PHYS, SE_SIZE);
	printf("goodix-i2c: SE mapped at %#x\n", se);
	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	printf("goodix-i2c: TLMM mapped at %#x\n", tlmm);
	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	printf("goodix-i2c: GCC mapped at %#x\n", gcc);
	rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
	printf("goodix-i2c: RSC mapped at %#x\n", rsc);
	cmd_db = ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);   /* memory, not registers */
	printf("goodix-i2c: cmd_db mapped at %#x\n", cmd_db);
	if (se == 0 || tlmm == 0) {
		printf("goodix-i2c: no registers (se %#x, tlmm %#x)\n", se, tlmm);
		thread_terminate(current_thread());
		return;
	}

	/*
	 * READ ONLY. Nothing here writes a clock, a regulator or a pin of the display's supply.
	 *
	 * Two writes were taken out of this path because on umi they are not safe:
	 *
	 *   gpio_output(GPIO_AVDD, ...)  umi.dts calls that regulator "disp_vddio_vreg": it is the
	 *                                display's I/O supply, and driving it during bsd_init took
	 *                                the machine down before it had a log.
	 *   clocks_on()                  the GCC register offsets in this file are redwood's
	 *                                (SM7325). The clock *indices* in umi.dts (0x74/0x86/0x87)
	 *                                say the same WRAP1 SE5 engine is meant, but the register
	 *                                layout of SM8250's GCC was never checked, and writing a
	 *                                clock controller blind stops the machine.
	 *
	 * What follows only reads the state of the engine the touch screen is on, so that the next
	 * step can be built on numbers read from this phone rather than on numbers from redwood.
	 * The chip is a FocalTech, not a Goodix (see the note at the top).
	 */
	printf("goodix-i2c: serial engine state: firmware %#x, interface %#x, parameters %#x\n",
	    rd(GENI_FW_REVISION_RO), rd(GENI_IF_DISABLE_RO), rd(SE_HW_PARAM_0));
	printf("goodix-i2c: TLMM GPIO %d (reset) reads %u, GPIO %d (irq) reads %u\n", GPIO_RESET,
	    (uint64_t)gpio_get(GPIO_RESET), GPIO_IRQ, (uint64_t)gpio_get(GPIO_IRQ));
	printf("goodix-i2c: done; nothing was written\n");
	thread_terminate(current_thread());
}

void
redwood_touch_probe_init(void)
{
	uint32_t mode = 0;
	thread_t thread;

	/* touchscreen=2: this one (touchscreen=1 is redwood_goodix.c's SPI chip) */
	if (!PE_parse_boot_argn("touchscreen", &mode, sizeof(mode)) || mode != 3) {
		return;
	}
	/* nothing but the thread: see the comment at touch_thread() */
	if (kernel_thread_start(touch_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
