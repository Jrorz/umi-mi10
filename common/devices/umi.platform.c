/*
 * umi (Xiaomi Mi 10, SM8250 "kona"): paste into xnuloader/loader.c next to platform_redwood
 * and add &platform_umi to platforms[]. Every value is explained in docs/PORTING.md ("The loader");
 * what is below was read from the phone's own device tree (umi.dts) on 2026-10-25.
 */
static const struct platform platform_umi = {
	.name = "umi",
	/* io_base must be non-zero: XNU treats a zero SoC base as "no SoC" and skips clock setup. */
	.io_base = 0x00400000, .io_size = 0x3FC00000,
	.gic_dist = 0x17a00000, .gic_redist = 0x17a60000,   /* arm,gic-v3 in the device tree */
	.uart = 0,                                          /* phones: no PL011 */
	.log_other_cpu = 1,
	/*
	 * The cores are numbered in MPIDR's second affinity field: cpu@0, cpu@100 ... cpu@700 in the
	 * device tree (reg = 0x0, 0x100, ... 0x700), so a core's number is MPIDR >> 8.
	 */
	.cores = 8, .core_shift = 8,
	/*
	 * Cores 0-3 are Cortex-A55 (capacity-dmips-mhz 0x400 = 1024), cores 4-7 Cortex-A77 (0x766 =
	 * 1894). As on redwood the kernel uses the small ones only when the big ones are busy.
	 */
	.small_cores = 0x0f,
	/* the splash framebuffer the bootloader leaves running: reserved-memory/cont_splash_region */
	.fb_base = 0x9C000000,
};
