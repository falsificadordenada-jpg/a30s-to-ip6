/*
 * A30s marco 2 (M2d): reads the bootloader's DTB, builds page tables from it, turns the MMU and caches on,
 * then brings every other CPU up through PSCI (each enables the MMU too). Everything is printed on
 * the framebuffer; the last line on screen tells where it stopped. Writes nothing but RAM + the fb.
 */
#include <stdint.h>
#include "console.h"
#include "fdt.h"
#include "mmu.h"
#include "dtinfo.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
extern char _head[], _end[], vectors[], svectors[], tramp_start[], tramp_end[], tramp_target[];
extern volatile uint64_t boot_flag;
extern void secondary_entry(void);

static uint64_t smc(uint64_t f, uint64_t a, uint64_t b, uint64_t c)
{
	register uint64_t x0 __asm__("x0") = f, x1 __asm__("x1") = a, x2 __asm__("x2") = b, x3 __asm__("x3") = c;
	__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) ::
			 "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static void mmu_on(uint64_t ttbr, uint64_t tcr, uint64_t mair)
{
	__asm__ volatile(
		"msr mair_el1, %0\n"
		"msr tcr_el1, %1\n"
		"msr ttbr0_el1, %2\n"
		"dsb sy\n isb\n"
		"tlbi vmalle1\n dsb nsh\n isb\n"
		"ic iallu\n dsb nsh\n isb\n"
		"mrs x9, sctlr_el1\n"
		"orr x9, x9, #1\n"
		"orr x9, x9, #4\n"
		"orr x9, x9, #(1 << 12)\n"
		"msr sctlr_el1, x9\n"
		"isb\n"
		:: "r"(mair), "r"(tcr), "r"(ttbr) : "x9", "memory");
}

static void stop(const char *why, uint64_t v)
{
	con_hex(why, v);
	con_str("STOP", "see line above");
	for (;;)
		__asm__ volatile("wfe");
}

void exc_handler(uint64_t slot, uint64_t *regs)
{
	uint64_t el = (rsr("CurrentEL") >> 2) & 3;
	uint64_t esr = rsr("S3_0_C5_C2_0"), elr = rsr("S3_0_C4_C0_1"), far = rsr("S3_0_C6_C0_0");
	(void)el;
	con_clear(BLUE);
	con_str("PANIC", "exception");
	con_hex("SLOT", slot);
	con_hex("ESR", esr);
	con_hex("ELR", elr);
	con_hex("FAR", far);
	con_hex("X0", regs[0]);
	for (;;)
		__asm__ volatile("wfe");
}

uint8_t slots_tab[MAXCPU][4096] __attribute__((aligned(4096)));
static struct info info;

static void flush(const void *p, uint64_t n)
{
	uint64_t a = (uint64_t)p & ~63ULL, e = (uint64_t)p + n;
	for (; a < e; a += 64)
		__asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

static unsigned cidx(uint64_t id)
{
	return (unsigned)((((id >> 8) & 0xff) * 4 + (id & 0xff)) & 15);
}

static const int BAR_Y[4] = { 1470, 1486, 1502, 1518 };
static const uint32_t BAR_C[4] = { 0xFFFF00FF, 0xFF00FF00, 0xFFFFFF00, 0xFF00FFFF };

/* which of the 4 bars did core `idx` paint? bit0 magenta(tramp) bit1 green(entry) bit2 yellow bit3 cyan(MMU on) */
static uint32_t bars_of(unsigned idx)
{
	uint32_t f = 0;
	for (int k = 0; k < 4; k++)
		if (con_fb[(BAR_Y[k] + 2) * (FB_STRIDE / 4) + idx * 32 + 4] == BAR_C[k])
			f |= 1u << k;
	return f;
}

struct res { uint64_t rc; uint32_t bars; int up, alive; volatile uint64_t *s; };

/* One CPU_ON attempt: fresh slot, call, wait 0.4 s watching the slot (status) and the framebuffer bars. */
static struct res attempt(uint64_t id, uint32_t fn, uint64_t entry, uint64_t l1, uint64_t tcr)
{
	struct res r;
	unsigned idx = cidx(id);
	volatile uint64_t *s = (volatile uint64_t *)slots_tab[idx];
	for (int k = 0; k < 16; k++) s[k] = 0;
	s[2] = l1; s[3] = tcr; s[4] = MAIR_VALUE; s[5] = (uint64_t)svectors;
	flush((const void *)s, 128);
	r.rc = smc(fn, id, entry, (uint64_t)s);
	r.up = 0;
	uint64_t t0 = rsr("cntpct_el0");
	while (rsr("cntpct_el0") - t0 < 10400000ULL) {	/* 0.4 s */
		flush((const void *)s, 128);
		if (s[0] == 1) { r.up = 1; break; }
	}
	flush((const void *)s, 128);
	r.bars = bars_of(idx);
	r.alive = r.up || s[6] || s[1] || s[14] || r.bars;
	r.s = s;
	return r;
}

static void show_alive(const struct res *r)
{
	volatile uint64_t *s = r->s;
	con_hex2("  dbg", "mk=", (uint32_t)((s[7] << 8) | (s[6] & 0xff)), "mp=", (uint32_t)s[1]);
	con_hex2("  dbg", "x0=", (uint32_t)s[8], "sc=", (uint32_t)s[9]);
	if (s[14])
		con_hex2("  EXC", "slot=", (uint32_t)(s[14] - 1), "esr=", (uint32_t)s[12]);
}

/* copy the trampoline to a low address; returns that address or 0 */
static uint64_t setup_low(void)
{
	const uint64_t dst = 0x80080000ULL;
	uint64_t n = (uint64_t)(tramp_end - tramp_start);
	if (dst + 0x1000 > (uint64_t)_head || mmu_kind(dst) != 2)
		return 0;
	for (uint64_t i = 0; i < n; i++)
		((volatile uint8_t *)dst)[i] = (uint8_t)tramp_start[i];
	*(volatile uint64_t *)(dst + (uint64_t)(tramp_target - tramp_start)) = (uint64_t)secondary_entry;
	flush((const void *)dst, n);
	__asm__ volatile("ic iallu\n dsb sy\n isb" ::: "memory");
	return dst;
}

struct var { char lab[8]; uint32_t fn; int ent; uint64_t id; };
/* ent: 0 = secondary_entry in the image, 1 = image header (_head), 2 = low trampoline.
 * Every variant uses a DIFFERENT core so a monitor that remembers "already on" cannot spoil the next test. */
static const struct var V[5] = {
	{ "E1 64H", 0xC4000003U, 1, 0x101 },
	{ "E2 32H", 0x84000003U, 1, 0x102 },
	{ "E3 64L", 0xC4000003U, 2, 0x103 },
	{ "E4 32I", 0x84000003U, 0, 0x200 },
	{ "E5 64H", 0xC4000003U, 1, 0x000 },
};

static int has_cpu(const struct info *I, uint64_t id)
{
	for (int i = 0; i < I->ncpu; i++)
		if (I->cpu[i] == id)
			return 1;
	return 0;
}

void hello_main(uint64_t dtb)
{
	struct info *I = &info;
	for (unsigned i = 0; i < sizeof(*I); i++)
		((volatile uint8_t *)I)[i] = 0;
	con_clear(0xff102040);
	con_fill(0, 0, FB_W, 3, WHITE);
	con_fill(0, FB_H - 3, FB_W, 3, WHITE);
	con_set_row(0);
	con_str("A30S", "M2d loader (SMP diag)");
	con_line("LOAD", (uint64_t)_head, (uint32_t)(_end - _head));
	con_hex("DTB", dtb);

	if (fdt_walk((const void *)dtb, on_dt, I) < 0)
		stop("DTB BAD", dtb);
	con_hex2("MAP", "mem=", I->nmem, "ko=", I->nko);
	con_line("FB", I->fb.base, (uint32_t)I->fb.size);
	con_hex2("GIC", "d=", (uint32_t)I->gicd, "c=", (uint32_t)I->gicc);
	con_hex2("PSCI", "on=", I->psci_cpu_on, "smc=", (uint32_t)I->psci_smc);
	con_hex2("CPUS", "n=", I->ncpu, "resv=", I->nresv);
	if (!I->nmem || !I->fb.size || !I->ncpu)
		stop("DTB INCOMPLETE", 0);

	uint64_t l1 = mmu_build(I->mem, I->nmem, I->ko, I->nko, I->fb);
	if (!l1)
		stop("PT FAIL", 0);
	uint64_t chk[] = { (uint64_t)_head, (uint64_t)_end - 8, dtb, I->fb.base, (uint64_t)slots_tab,
			   (uint64_t)&info, (uint64_t)vectors, 0x13500000 };
	int want[] = { 2, 2, 2, 3, 2, 2, 2, 1 };
	for (unsigned i = 0; i < sizeof(chk) / sizeof(chk[0]); i++)
		if (mmu_kind(chk[i]) != want[i])
			stop("UNMAPPED", chk[i]);

	uint64_t ips = rsr("S3_0_C0_C7_0") & 0xf;
	uint64_t tcr = TCR_BASE | (ips << 32);
	mmu_on(l1, tcr, MAIR_VALUE);
	con_hex2("MMU", "ON sctlr=", (uint32_t)rsr("S3_0_C1_C0_0"), 0, 0);

	/* ---- SMP through PSCI (M2d: 5 CPU_ON variants, one result line each) ---- */
	uint64_t me = rsr("mpidr_el1") & 0xffffff;
	con_hex2("EL", "cur=", (uint32_t)((rsr("CurrentEL") >> 2) & 3), "mpidr=", (uint32_t)me);
	/* re-arm the "already booted" flag and push the whole image to DRAM (cores start with MMU/caches off) */
	boot_flag = 1;
	flush((const void *)_head, (uint64_t)(_end - _head));
	__asm__ volatile("ic iallu\n dsb sy\n isb" ::: "memory");
	uint64_t low = setup_low();
	con_hex2("ENTRY", "img=", (uint32_t)(uint64_t)secondary_entry, "low=", (uint32_t)low);
	con_str("BARS", "mg=tramp gr=go ye=pre cy=MMU");

	int win = -1, win_up = 0;
	for (int e = 0; e < 5 && win < 0; e++) {
		const struct var *v = &V[e];
		uint64_t entry = v->ent == 0 ? (uint64_t)secondary_entry : v->ent == 1 ? (uint64_t)_head : low;
		if (!entry || !has_cpu(I, v->id) || v->id == me) {
			con_hex2(v->lab, "SKIP id=", (uint32_t)v->id, 0, 0);
			continue;
		}
		con_hex2(v->lab, "go id=", (uint32_t)v->id, "ent=", (uint32_t)entry);
		struct res r = attempt(v->id, v->fn, entry, l1, tcr);
		con_hex2("  ret", "rc=", (uint32_t)r.rc, "bars=", r.bars);
		if (r.alive) {
			show_alive(&r);
			win = e;
			win_up = r.up;
		}
	}

	int nup = 0, nbad = 0;
	if (win >= 0) {
		const struct var *v = &V[win];
		uint64_t entry = v->ent == 0 ? (uint64_t)secondary_entry : v->ent == 1 ? (uint64_t)_head : low;
		con_hex2("WIN", "var=E", win + 1, "id=", (uint32_t)v->id);
		for (int i = 0; i < I->ncpu; i++) {
			uint64_t id = I->cpu[i];
			if (id == me || id == v->id)
				continue;
			struct res r = attempt(id, v->fn, entry, l1, tcr);
			con_hex2(r.up ? "CPU ok" : "CPU BAD", "id=", (uint32_t)id, "rc=", (uint32_t)r.rc);
			if (!r.up) {
				nbad++;
				if (r.alive)
					show_alive(&r);
			} else {
				nup++;
			}
		}
		con_hex2("SMP", "up=", (uint32_t)(nup + win_up), "bad=", (uint32_t)nbad);
	} else {
		con_str("SMP", "NO WINNER: single-core");
	}
	con_str("DONE", "M2d finished; power off");
	for (;;)
		__asm__ volatile("wfe");
}
