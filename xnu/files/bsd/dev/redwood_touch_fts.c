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
#define TLMM_PHYS               0x0f100000UL
#define TLMM_SIZE               0x100000UL
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

#define FTS_TOUCH_POINT_NUM     2               /* buf[2] & 0x0F: how many contacts */
#define FTS_ONE_TCH_LEN         6               /* bytes per contact */
#define FTS_TOUCH_EVENT_POS     3               /* within a contact: event in the low nibble */
#define FTS_TOUCH_ID_POS        5               /* within a contact: id in the high nibble */
#define FTS_MAX_POINTS          10              /* FTS_MAX_POINTS_SUPPORT */
#define FTS_POINT_BUF_LEN       (2 + FTS_ONE_TCH_LEN * FTS_MAX_POINTS)

#define FTS_TOUCH_DOWN          0
#define FTS_TOUCH_UP            1
#define FTS_TOUCH_CONTACT       2

/* ---- the pins (umi.dts: fts,reset-gpio = <&tlmm 38>, fts,irq-gpio = <&tlmm 39>) ---------------- */

#define GPIO_RESET              38
#define GPIO_IRQ                39
#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))
#define GPIO_IO(n)              (tlmm + 0x1000UL * (n) + 4)
#define PULL_NONE               0
#define PULL_UP                 3

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
	const char *problem;
	uint32_t got = 0;

	wr(SE_I2C_TX_TRANS_LEN, 1);
	wr(SE_I2C_RX_TRANS_LEN, in_length);
	fifo_put(&reg, 1);
	wr(GENI_M_CMD0, GENI_I2C_OP_WRITE_READ << 27 | (FTS_I2C_ADDR << GENI_SLV_ADDR_SHIFT));
	problem = wait_done();
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

static const char *
fts_write_reg(uint8_t reg, uint8_t value)
{
	uint8_t pair[2] = { reg, value };

	wr(SE_I2C_TX_TRANS_LEN, 2);
	wr(SE_I2C_RX_TRANS_LEN, 0);
	fifo_put(pair, 2);
	wr(GENI_M_CMD0, GENI_I2C_OP_WRITE_READ << 27 | (FTS_I2C_ADDR << GENI_SLV_ADDR_SHIFT));
	return wait_done();
}

/* ---- the report --------------------------------------------------------------------------------- */

static struct {
	bool down, seen;
	uint32_t x, y;
} contact[FTS_MAX_POINTS];

static void
report(const uint8_t *buf, uint32_t count, uint32_t max_x, uint32_t max_y)
{
	static unsigned shown;

	for (int i = 0; i < FTS_MAX_POINTS; i++) {
		contact[i].seen = false;
	}
	for (uint32_t i = 0; i < count && i < FTS_MAX_POINTS; i++) {
		/* focaltech_core.c: base = FTS_ONE_TCH_LEN * i, and every field is read from base */
		uint32_t base = FTS_ONE_TCH_LEN * i;
		uint32_t event = buf[FTS_TOUCH_EVENT_POS + base] >> 6;      /* the event is the top two bits */
		uint32_t id = buf[FTS_TOUCH_ID_POS + base] >> 4;
		uint32_t x, y;

		if (event != FTS_TOUCH_DOWN && event != FTS_TOUCH_CONTACT) {
			continue;
		}
		x = (((uint32_t)buf[FTS_TOUCH_X_H_POS + base] & 0x0f) << 8) | (buf[FTS_TOUCH_X_L_POS + base] & 0xff);
		y = (((uint32_t)buf[FTS_TOUCH_Y_H_POS + base] & 0x0f) << 8) | (buf[FTS_TOUCH_Y_L_POS + base] & 0xff);
		if (id >= FTS_MAX_POINTS) {
			continue;
		}
		if (shown < 40) {
			shown++;
			printf("fts: finger %u at %u, %u\n", id, x, y);
		}
		x = x >= max_x ? 65535 : x * 65535 / max_x;
		y = y >= max_y ? 65535 : y * 65535 / max_y;
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
	for (uint32_t id = 0; id < FTS_MAX_POINTS; id++) {
		if (contact[id].down && !contact[id].seen) {
			contact[id].down = false;
			redwood_touch_post(REDWOOD_TOUCH_UP, id, contact[id].x, contact[id].y);
		}
	}
}

/*
 * focaltech_core.c's power sequence, without the regulators this kernel has no driver for: the
 * chip is already powered by the boot loader (Android leaves avdd for the panel), so only the
 * reset line is driven.
 */
static void
power_on(void)
{
	gpio_set(GPIO_RESET, false);                    /* level first */
	gpio_configure(GPIO_RESET, 0, PULL_UP, 8, true);
	gpio_configure(GPIO_IRQ, 0, PULL_UP, 8, false);
	delay(20000);                                   /* fts,power-delay: 20 ms after power */
	gpio_set(GPIO_RESET, true);
	delay(50000);                                   /* the chip needs time before it answers */
}

static void
touch_thread(void *arg __unused, wait_result_t wresult __unused)
{
	uint8_t buf[FTS_POINT_BUF_LEN];
	uint8_t id[2] = { 0, 0 };
	uint32_t max_x = 1080, max_y = 2340, said = 0;
	const char *problem;

	se = ml_io_map(SE_PHYS, SE_SIZE);
	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
	cmd_db = ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);
	if (se == 0 || tlmm == 0) {
		printf("fts: no registers (se %#x, tlmm %#x)\n", se, tlmm);
		thread_terminate(current_thread());
		return;
	}
	printf("fts: serial engine at %#x, TLMM at %#x\n", se, tlmm);

	problem = engine_init();
	if (problem != NULL) {
		printf("fts: %s\n", problem);
		thread_terminate(current_thread());
		return;
	}
	printf("fts: the engine is in I2C mode, FIFO %u words deep\n", fifo_depth);

	power_on();
	printf("fts: reset released; irq line reads %u\n", (uint64_t)gpio_get(GPIO_IRQ));

	/* who is there? focaltech_core.c reads the two id registers of the chip */
	problem = fts_read(FTS_REG_CHIP_ID, &id[0], 1);
	if (problem == NULL) {
		problem = fts_read(FTS_REG_CHIP_ID2, &id[1], 1);
	}
	if (problem != NULL) {
		printf("fts: reading the chip id: %s\n", problem);
		thread_terminate(current_thread());
		return;
	}
	printf("fts: chip id %#x%02x (FT3680 reports 0x54 for the first byte)\n", id[0], id[1]);

	/* and to the point reports: focaltech_core.c reads register 0 and takes byte 2 */
	{
		uint8_t work = 0;

		if (fts_read(FTS_REG_WORKMODE, &work, 1) == NULL) {
			printf("fts: work mode %#x\n", work);
		}
	}
	for (;;) {
		if (!gpio_get(GPIO_IRQ)) {
			delay(8000);
			continue;
		}
		buf[0] = FTS_REG_WORKMODE;                  /* a report is read from register 0 */
		problem = fts_read(FTS_REG_WORKMODE, buf, FTS_POINT_BUF_LEN);
		if (problem != NULL) {
			if (said < 8) {
				said++;
				printf("fts: %s\n", problem);
			}
			delay(8000);
			continue;
		}
		{
			uint32_t count = buf[FTS_TOUCH_POINT_NUM] & 0x0f;

			if (count > 0 && count <= FTS_MAX_POINTS) {
				report(buf, count, max_x, max_y);
			}
			/* tell the chip the report has been read: FTS_REG_INT_ACK */
			(void)fts_write_reg(FTS_REG_INT_ACK, 0x00);
		}
		delay(8000);
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
