/*
 * A30s hello-framebuffer. Draws a calibration screen on the framebuffer the
 * bootloader hands over (fb_handover, 0xEC000000, 720x1560) and prints what
 * the CPU tells us about the world we were started in. Read-only: it never
 * writes anywhere except the framebuffer.
 *
 * Build with -DPROBE to also read a few SoC MMIO ID registers (GIC, eMMC).
 * A bad MMIO read paints a panic screen instead of hanging silently.
 */
#include <stdint.h>
#include "console.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }

static uint32_t be32(uint32_t v)
{
	return (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24);
}

static void bars(int y)
{
	con_fill(24, y, 210, 24, 0xffff0000);		/* if this looks blue, pixels are BGR */
	con_fill(255, y, 210, 24, 0xff00ff00);
	con_fill(486, y, 210, 24, 0xff0000ff);
}

void exc_handler(uint64_t slot, uint64_t *regs)
{
	uint64_t el = (rsr("CurrentEL") >> 2) & 3;
	uint64_t esr, elr, far;
	if (el == 2) {
		esr = rsr("S3_4_C5_C2_0");	/* ESR_EL2 */
		elr = rsr("S3_4_C4_C0_1");	/* ELR_EL2 */
		far = rsr("S3_4_C6_C0_0");	/* FAR_EL2 */
	} else {
		esr = rsr("S3_0_C5_C2_0");	/* ESR_EL1 */
		elr = rsr("S3_0_C4_C0_1");	/* ELR_EL1 */
		far = rsr("S3_0_C6_C0_0");	/* FAR_EL1 */
	}
	con_clear(BLUE);
	con_str("PANIC", "exception");
	con_hex("SLOT", slot);
	con_hex("EL", el);
	con_hex("ESR", esr);
	con_hex("ELR", elr);
	con_hex("FAR", far);
	con_hex("X0", regs[0]);
	for (;;)
		__asm__ volatile("wfe");
}

void hello_main(uint64_t dtb)
{
	uint64_t el = (rsr("CurrentEL") >> 2) & 3;
	uint64_t isar1 = rsr("S3_0_C0_C6_1");	/* ID_AA64ISAR1_EL1 */
	uint64_t pac = ((isar1 >> 4) | (isar1 >> 8) | (isar1 >> 24) | (isar1 >> 28)) & 0xf;
	uint64_t sctlr = (el == 2) ? rsr("S3_4_C1_C0_0") : rsr("S3_0_C1_C0_0");

	con_clear(0xff102040);

	/* border: if the stride is wrong this will look skewed or broken */
	con_fill(0, 0, FB_W, 3, WHITE);
	con_fill(0, FB_H - 3, FB_W, 3, WHITE);
	con_fill(0, 0, 3, FB_H, WHITE);
	con_fill(FB_W - 3, 0, 3, FB_H, WHITE);

	con_set_row(0);
	con_str("A30S", "hello framebuffer");
	con_str("FB", "0xEC000000 720x1560");
	bars(100);
	con_set_row(4);
	con_str_at(1, "RED", WHITE);
	con_str_at(16, "GREEN", WHITE);
	con_str_at(31, "BLUE", WHITE);
	con_set_row(6);
	con_hex("EL", el);
	con_hex("MIDR", rsr("S3_0_C0_C0_0"));
	con_hex("MPIDR", rsr("S3_0_C0_C0_5"));
	con_hex("PFR0", rsr("S3_0_C0_C4_0"));
	con_hex("MMFR0", rsr("S3_0_C0_C7_0"));
	con_hex("ISAR1", isar1);
	con_str("PAC", pac ? "present" : "absent");
	con_hex("CNTFRQ", rsr("cntfrq_el0"));
	con_hex("SCTLR", sctlr);
	con_hex("X0", dtb);

#ifdef PROBE
	con_skip();
	con_hex2("GICD", "typer=", rd32(0x12301004), "iidr=", rd32(0x12301008));
	con_hex2("DWMMC", "verid=", rd32(0x1350006c), "hcon=", rd32(0x13500070));
#endif

#ifdef CRASHTEST
	__asm__ volatile(".inst 0x00000000");	/* UDF: tests the panic screen */
#endif

	/* DTB probe goes last: if x0 is not a pointer this faults into the panic screen */
	con_skip();
	if (dtb && !(dtb & 3)) {
		uint32_t magic = be32(rd32(dtb));
		con_hex2("DTB", "magic=", magic, 0, 0);
		if (magic == 0xd00dfeed)
			con_hex2("DTB", "size=", be32(rd32(dtb + 4)), 0, 0);
		else
			con_str("DTB", "magic INVALID");
	} else {
		con_str("DTB", "x0 not a pointer");
	}
	con_skip();
	con_str("DONE", "power off to exit");
	for (;;)
		__asm__ volatile("wfe");
}
