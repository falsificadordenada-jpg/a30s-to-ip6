/*
 * A30s marco 3b (= 3a + touch probes): DTB -> page tables -> MMU/caches on, then a menu driven by the hardware buttons.
 * Buttons live in the ALIVE pin controller @0x11CB0000, bank gpa1 (+0x60): VOL-=pin6, VOL+=pin5, POWER=pin7.
 * Polarity is learned at start (idle level = whatever the pins read while nothing is pressed).
 * Select = POWER, or VOL- and VOL+ together. Writes only RAM, the fb and CON of gpa1 pins 5-7 (set to input).
 */
#include <stdint.h>
#include "console.h"
#include "fdt.h"
#include "mmu.h"
#include "dtinfo.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
extern char _head[], _end[], vectors[];

/* start.S still carries the (unused) secondary-core stub that points at this table */
uint8_t slots_tab[MAXCPU][4096] __attribute__((aligned(4096)));
static struct info info;

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
	uint64_t esr = rsr("S3_0_C5_C2_0"), elr = rsr("S3_0_C4_C0_1"), far = rsr("S3_0_C6_C0_0");
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

/* ---------- small helpers ---------- */
static uint32_t mr(uint64_t a) { return *(volatile uint32_t *)a; }
static void mw(uint64_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static uint64_t ticks_ms(unsigned ms) { return rsr("cntfrq_el0") / 1000 * ms; }
static void wait_ms(unsigned ms)
{
	uint64_t t = rsr("cntpct_el0"), n = ticks_ms(ms);
	while (rsr("cntpct_el0") - t < n)
		;
}

static void hexs(char *b, uint64_t v, int n)
{
	static const char h[] = "0123456789ABCDEF";
	for (int i = 0; i < n; i++)
		b[i] = h[(v >> (4 * (n - 1 - i))) & 15];
	b[n] = 0;
}
static void bin8(char *b, uint32_t v)
{
	for (int i = 0; i < 8; i++)
		b[i] = ((v >> (7 - i)) & 1) ? '1' : '0';
	b[8] = 0;
}
static int cat(char *d, int n, const char *s)
{
	while (*s && n < 33)
		d[n++] = *s++;
	return n;
}

/* fixed-width line at row r: 8-char yellow label, 33-char value */
static void put(int r, const char *lab, const char *val, uint32_t fg)
{
	char l[10], b[36];
	int i = 0;
	for (; lab[i] && i < 8; i++) l[i] = lab[i];
	for (; i < 8; i++) l[i] = ' ';
	l[8] = 0;
	for (i = 0; val[i] && i < 33; i++) b[i] = val[i];
	for (; i < 33; i++) b[i] = ' ';
	b[33] = 0;
	con_set_row(r);
	con_str_at(0, l, YELLOW);
	con_str_at(9, b, fg);
}
static void putx(int r, const char *lab, uint64_t v, int digits)
{
	char b[20];
	hexs(b, v, digits);
	put(r, lab, b, WHITE);
}
static void screen(const char *title)
{
	con_clear(0xff102040);
	con_fill(0, 0, FB_W, 3, WHITE);
	con_fill(0, FB_H - 3, FB_W, 3, WHITE);
	con_set_row(0);
	put(0, "A30S", title, WHITE);
}

/* ---------- buttons ---------- */
#define PIN_BASE	0x11CB0000UL
#define GPA(n)		(PIN_BASE + 0x40 + 0x20 * (n))
#define K_DN		1u	/* VOL-  gpa1 pin 6 */
#define K_UP		2u	/* VOL+  gpa1 pin 5 */
#define K_PW		4u	/* POWER gpa1 pin 7 */

static uint32_t idle_lv;			/* level of pins 5..7 with nothing pressed (bits 5,6,7) */
static uint32_t k_stable, k_last;
static uint64_t k_t;
static uint32_t con_before, con_after;

static uint32_t gdat(int n) { return mr(GPA(n) + 4) & 0xff; }

static uint32_t raw_keys(void)
{
	uint32_t d = (gdat(1) ^ idle_lv), k = 0;
	if (d & (1u << 6)) k |= K_DN;
	if (d & (1u << 5)) k |= K_UP;
	if (d & (1u << 7)) k |= K_PW;
	return k;
}

static void keys_init(void)
{
	/* gpa1 pins 5..7 -> plain input (function 0), 4 bits per pin in CON */
	con_before = mr(GPA(1));
	mw(GPA(1), con_before & 0x000FFFFF);
	con_after = mr(GPA(1));
	wait_ms(50);
	idle_lv = gdat(1) & 0xE0;
	k_stable = k_last = 0;
	k_t = rsr("cntpct_el0");
}

/* returns newly pressed keys (debounced 30 ms). Both volume keys together count as POWER. */
static uint32_t key_events(void)
{
	uint32_t r = raw_keys();
	uint64_t t = rsr("cntpct_el0");
	if (r != k_last) { k_last = r; k_t = t; }
	if (r != k_stable && t - k_t > ticks_ms(30)) {
		uint32_t ev = r & ~k_stable;
		k_stable = r;
		if ((r & (K_UP | K_DN)) == (K_UP | K_DN) && (ev & (K_UP | K_DN)))
			ev = K_PW;
		return ev;
	}
	return 0;
}

/* ---------- screens ---------- */
static void scr_buttons(void)
{
	screen("Teste de botoes");
	put(2, "idle", "", WHITE);
	put(24, "SAIR", "VOL- + VOL+ juntos, 1 s", GREEN);
	uint32_t evcnt = 0, prev = 0, both_since = 0;
	for (;;) {
		char b[36], t[12];
		for (int n = 0; n < 3; n++) {
			int p = 0;
			char nm[5] = { 'G', 'P', 'A', (char)('0' + n), 0 };
			b[0] = 0;
			p = cat(b, p, "DAT=");
			bin8(t, gdat(n)); p = cat(b, p, t); b[p] = 0;
			p = cat(b, p, " CON=");
			hexs(t, mr(GPA(n)), 8); p = cat(b, p, t); b[p] = 0;
			put(3 + n, nm, b, WHITE);
		}
		int p = 0;
		p = cat(b, p, "p5-7 idle=");
		bin8(t, idle_lv); p = cat(b, p, t); b[p] = 0;
		put(2, "idle", b, WHITE);
		p = 0;
		p = cat(b, p, "CON1 antes ");
		hexs(t, con_before, 8); p = cat(b, p, t); b[p] = 0;
		put(6, "con", b, WHITE);
		p = 0;
		p = cat(b, p, "CON1 depois ");
		hexs(t, con_after, 8); p = cat(b, p, t); b[p] = 0;
		put(7, "con", b, WHITE);
		uint32_t k = raw_keys();
		put(10, "VOL-", (k & K_DN) ? "APERTADO" : "-", (k & K_DN) ? GREEN : WHITE);
		put(11, "VOL+", (k & K_UP) ? "APERTADO" : "-", (k & K_UP) ? GREEN : WHITE);
		put(12, "POWER", (k & K_PW) ? "APERTADO" : "-", (k & K_PW) ? GREEN : WHITE);
		if (k != prev && k) evcnt++;
		prev = k;
		hexs(t, evcnt, 4);
		put(14, "eventos", t, WHITE);
		if ((k & (K_UP | K_DN)) == (K_UP | K_DN)) {
			if (!both_since) both_since = (uint32_t)rsr("cntpct_el0");
			else if ((uint32_t)rsr("cntpct_el0") - both_since > ticks_ms(1000)) break;
		} else {
			both_since = 0;
		}
		wait_ms(40);
	}
	while (raw_keys())
		wait_ms(20);
	key_events();
	k_stable = 0;
}

static void wait_back(void)
{
	for (;;) {
		if (key_events() & K_PW)
			return;
		wait_ms(10);
	}
}

static void scr_info(void)
{
	screen("Info do sistema");
	putx(2, "MIDR", rsr("midr_el1"), 8);
	putx(3, "MPIDR", rsr("mpidr_el1"), 8);
	putx(4, "EL", (rsr("CurrentEL") >> 2) & 3, 1);
	putx(5, "SCTLR", rsr("S3_0_C1_C0_0"), 8);
	putx(6, "CNTFRQ", rsr("cntfrq_el0"), 8);
	putx(7, "PFR0", rsr("S3_0_C0_C4_0"), 16);
	putx(8, "ISAR0", rsr("S3_0_C0_C6_0"), 16);
	putx(9, "MMFR0", rsr("S3_0_C0_C7_0"), 16);
	putx(10, "GICD_TYP", mr(0x12301004), 8);
	putx(11, "GICD_IID", mr(0x12301008), 8);
	putx(12, "MMC VER", mr(0x13500000 + 0x6C), 8);
	putx(13, "DTB cpus", info.ncpu, 2);
	put(24, "VOLTAR", "POWER (ou VOL- + VOL+)", GREEN);
	wait_back();
}

static void scr_mem(void)
{
	screen("Mapa de memoria");
	con_set_row(2);
	for (int i = 0; i < info.nmem; i++)
		con_line("MEM", info.mem[i].base, (uint32_t)info.mem[i].size);
	for (int i = 0; i < info.nko; i++)
		con_line("KEEPOUT", info.ko[i].base, (uint32_t)info.ko[i].size);
	con_line("FB", info.fb.base, (uint32_t)info.fb.size);
	con_line("GICD", info.gicd, 0);
	con_line("GICC", info.gicc, 0);
	con_line("IMAGE", (uint64_t)_head, (uint32_t)(_end - _head));
	put(24, "VOLTAR", "POWER (ou VOL- + VOL+)", GREEN);
	wait_back();
}

static void scr_colors(void)
{
	screen("Teste de cores");
	static const uint32_t c[6] = { 0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff, 0xff000000, 0xffffff00 };
	for (int i = 0; i < 6; i++)
		con_fill(24, 120 + i * 100, FB_W - 48, 80, c[i]);
	put(24, "NOTA", "listras: R G B branco preto amar.", WHITE);
	put(25, "VOLTAR", "POWER (ou VOL- + VOL+)", GREEN);
	wait_back();
}


/* ---------- touch (Imagis IST40xx on I2C4 @0x13870000, 7-bit addr 0x50) ---------- */
#define TOP_PIN		0x139B0000UL		/* pin controller TOP: gpp4 +0xA0 (i2c4), gpg2 +0x160 (tsp ldo en = pin 1) */
#define GPP4		(TOP_PIN + 0xA0)
#define GPG2		(TOP_PIN + 0x160)
#define I2C4		0x13870000UL
#define IICCON		0x00
#define IICSTAT		0x04
#define IICDS		0x0C
#define IICLC		0x10
#define TP_ADDR		0x50
#define HIB(n)		((uint32_t)(n) | (0x800Bu << 16))
#define DIRECT(n)	((uint32_t)(n) | (1u << 31))

static const uint32_t ctl_ack = 0xEF;	/* ACKEN | TXDIV512 | IRQEN | prescaler 15: slowest, safe for any pclk */
static const uint32_t ctl_nak = 0x6F;
static int i2c_err;

static int i2c_pend(void)
{
	uint64_t t = rsr("cntpct_el0"), n = ticks_ms(30);
	while (rsr("cntpct_el0") - t < n)
		if (mr(I2C4 + IICCON) & 0x10)
			return 0;
	return -1;
}
static void i2c_setup(void)
{
	mw(I2C4 + IICLC, 0x7);
	mw(I2C4 + IICCON, ctl_ack & ~0x10u);
	mw(I2C4 + IICSTAT, 0x10);
}
/* mode: 0xC0 master tx, 0x80 master rx. rep: repeated start (release a held byte first) */
static int i2c_addr(uint32_t a8, uint32_t mode, int rep)
{
	mw(I2C4 + IICSTAT, mode | 0x10);
	mw(I2C4 + IICDS, a8);
	__asm__ volatile("dsb sy");
	wait_ms(1);
	if (rep)
		mw(I2C4 + IICCON, ctl_ack & ~0x10u);
	mw(I2C4 + IICSTAT, mode | 0x30);
	if (i2c_pend()) { i2c_err = 1; return -1; }	/* 1 = no event (clock/pins?) */
	if (mr(I2C4 + IICSTAT) & 1) { i2c_err = 2; return -1; }	/* 2 = address NACK */
	return 0;
}
static void i2c_stop(uint32_t mode)
{
	mw(I2C4 + IICSTAT, mode | 0x10);
	mw(I2C4 + IICCON, ctl_nak & ~0x10u);
	for (int i = 0; i < 20 && (mr(I2C4 + IICSTAT) & 0x20); i++)
		wait_ms(1);
	mw(I2C4 + IICSTAT, 0x10);
}
static int i2c_probe(uint32_t a7)
{
	i2c_err = 0;
	int r = i2c_addr(a7 << 1, 0xC0, 0);
	i2c_stop(0xC0);
	return r;
}
static int i2c_rd32(uint32_t reg, uint32_t *v)
{
	i2c_err = 0;
	if (i2c_addr(TP_ADDR << 1, 0xC0, 0)) { i2c_stop(0xC0); return -1; }
	for (int i = 0; i < 4; i++) {
		mw(I2C4 + IICDS, (reg >> (24 - 8 * i)) & 0xff);
		mw(I2C4 + IICCON, ctl_ack & ~0x10u);
		if (i2c_pend()) { i2c_err = 4; i2c_stop(0xC0); return -1; }
		if (mr(I2C4 + IICSTAT) & 1) { i2c_err = 3; i2c_stop(0xC0); return -1; }
	}
	if (i2c_addr((TP_ADDR << 1) | 1, 0x80, 1)) { i2c_stop(0x80); return -1; }
	uint32_t x = 0;
	for (int i = 0; i < 4; i++) {
		mw(I2C4 + IICCON, (i == 3 ? ctl_nak : ctl_ack) & ~0x10u);
		if (i2c_pend()) { i2c_err = 5; i2c_stop(0x80); return -1; }
		x = (x << 8) | (mr(I2C4 + IICDS) & 0xff);
	}
	i2c_stop(0x80);
	*v = x;
	return 0;
}

static void pin_lines(int r)
{
	char b[36], t[12];
	int p;
	p = 0; p = cat(b, p, "CON="); hexs(t, mr(GPG2), 8); p = cat(b, p, t);
	p = cat(b, p, " DAT="); bin8(t, mr(GPG2 + 4) & 0xff); p = cat(b, p, t); b[p] = 0;
	put(r, "gpg2", b, WHITE);
	p = 0; p = cat(b, p, "CON="); hexs(t, mr(GPP4 + 0), 8); p = cat(b, p, t);
	p = cat(b, p, " PUD="); hexs(t, mr(GPP4 + 8), 8); p = cat(b, p, t); b[p] = 0;
	put(r + 1, "gpp4", b, WHITE);
	p = 0; p = cat(b, p, "CON="); hexs(t, mr(GPA(0)), 8); p = cat(b, p, t);
	p = cat(b, p, " DAT="); bin8(t, gdat(0)); p = cat(b, p, t); b[p] = 0;
	put(r + 2, "gpa0", b, WHITE);
}

static void scr_tp_power(void)
{
	screen("Touch: energia+pinos");
	put(2, "ANTES", "", WHITE);
	pin_lines(3);
	/* ldo enable: gpg2 pin1 -> output, high */
	mw(GPG2, (mr(GPG2) & ~0xF0u) | 0x10);
	mw(GPG2 + 4, mr(GPG2 + 4) | 2);
	/* i2c4: gpp4 pins 0,1 -> function 2, pull-up */
	mw(GPP4, (mr(GPP4) & ~0xFFu) | 0x22);
	mw(GPP4 + 8, (mr(GPP4 + 8) & ~0xFFu) | 0x33);
	/* attention/irq pin gpa0-0 -> input */
	mw(GPA(0), mr(GPA(0)) & ~0xFu);
	wait_ms(300);
	put(7, "DEPOIS", "ldo ligado, i2c4 pinos ok", WHITE);
	pin_lines(8);
	put(24, "VOLTAR", "POWER (ou VOL- + VOL+)", GREEN);
	wait_back();
}

static void scr_tp_regs(void)
{
	screen("Touch: regs I2C4");
	put(2, "AVISO", "se travar/panic = clock off", YELLOW);
	putx(4, "IICCON", mr(I2C4 + IICCON), 8);
	putx(5, "IICSTAT", mr(I2C4 + IICSTAT), 8);
	putx(6, "IICDS", mr(I2C4 + IICDS), 8);
	putx(7, "IICLC", mr(I2C4 + IICLC), 8);
	put(24, "VOLTAR", "POWER (ou VOL- + VOL+)", GREEN);
	wait_back();
}

static void scr_tp_id(void)
{
	screen("Touch: chip ID");
	put(2, "init", "i2c_setup...", WHITE);
	i2c_setup();
	put(2, "init", "ok", GREEN);
	put(3, "scan", "buscando enderecos...", WHITE);
	char b[36];
	int p = 0, found = 0;
	for (uint32_t a = 0x08; a < 0x78 && p < 30; a++)
		if (i2c_probe(a) == 0) {
			char t[4];
			hexs(t, a, 2);
			p = cat(b, p, t);
			b[p++] = ' ';
			b[p] = 0;
			found++;
		}
	if (!found) { b[0] = '-'; b[1] = 0; }
	put(3, "ack em", b, found ? GREEN : YELLOW);
	char t[12];
	uint32_t v = 0;
	int r = i2c_rd32(DIRECT(0x40001000), &v);
	p = 0;
	p = cat(b, p, r ? "FALHOU err=" : "OK val=");
	hexs(t, r ? (uint32_t)i2c_err : v, r ? 1 : 8);
	p = cat(b, p, t); b[p] = 0;
	put(5, "chipID", b, r ? YELLOW : GREEN);
	put(6, "esperado", "00004070 ou 00004050", WHITE);
	put(8, "err", "1=sem evento 2=NACK end 3=NACK dado", WHITE);
	put(24, "VOLTAR", "POWER (ou VOL- + VOL+)", GREEN);
	wait_back();
}

static void scr_tp_live(void)
{
	screen("Touch ao vivo");
	i2c_setup();
	put(24, "SAIR", "POWER (ou VOL- + VOL+)", GREEN);
	int lx = -1, ly = -1;
	uint32_t errs = 0, loops = 0;
	for (;;) {
		uint32_t st = 0, msg = 0, c0 = 0, c1 = 0;
		int r = i2c_rd32(HIB(0x30000100), &st);
		r |= i2c_rd32(HIB(0x30000104), &msg);
		r |= i2c_rd32(HIB(0x30000108), &c0);
		r |= i2c_rd32(HIB(0x3000010C), &c1);
		if (r) errs++;
		loops++;
		putx(2, "status", st, 8);
		putx(3, "intrmsg", msg, 8);
		putx(4, "coord0", c0, 8);
		putx(5, "coord1", c1, 8);
		putx(6, "erros", errs, 4);
		putx(7, "leituras", loops, 4);
		char b[36], t[12];
		int p = 0;
		p = cat(b, p, "gpa0 DAT="); bin8(t, gdat(0)); p = cat(b, p, t); b[p] = 0;
		put(8, "irq", b, WHITE);
		uint32_t cnt = (msg >> 12) & 0xF;
		int x = (int)((c0 >> 12) & 0xFFF), y = (int)(c0 & 0xFFF);
		p = 0;
		p = cat(b, p, "n="); hexs(t, cnt, 1); p = cat(b, p, t);
		p = cat(b, p, " x="); hexs(t, x, 3); p = cat(b, p, t);
		p = cat(b, p, " y="); hexs(t, y, 3); p = cat(b, p, t); b[p] = 0;
		put(10, "dedo", b, cnt && !r ? GREEN : WHITE);
		if (lx >= 0)
			con_fill(lx, ly, 24, 24, 0xff102040);
		lx = ly = -1;
		if (!r && cnt && cnt < 6 && x < FB_W && y < FB_H) {
			lx = x < 12 ? 0 : x - 12;
			ly = y < 12 ? 0 : y - 12;
			if (ly > FB_H - 30) ly = FB_H - 30;
			con_fill(lx, ly, 24, 24, GREEN);
		}
		if (key_events() & K_PW)
			break;
		wait_ms(20);
	}
}

static const char *item_name(int i)
{
	switch (i) {
	case 0: return "1  Teste de botoes";
	case 1: return "2  Info do sistema";
	case 2: return "3  Mapa de memoria";
	case 3: return "4  Teste de cores";
	case 4: return "5  Touch: energia+pinos";
	case 5: return "6  Touch: regs I2C";
	case 6: return "7  Touch: chip ID";
	default: return "8  Touch ao vivo";
	}
}

static void draw_menu(int sel)
{
	screen("M3b menu");
	for (int i = 0; i < 8; i++) {
		con_set_row(3 + i * 2);
		con_str_at(0, i == sel ? ">" : " ", GREEN);
		con_str_at(2, item_name(i), i == sel ? GREEN : WHITE);
	}
	put(21, "VOL-/+", "mover", WHITE);
	put(22, "POWER", "escolher (ou VOL-+VOL+)", WHITE);
	put(24, "RESET", "segure POWER ~7 s", YELLOW);
	put(25, "ORDEM", "touch: 5, depois 6, 7, 8", YELLOW);
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
	con_str("A30S", "M3b loader");
	con_line("LOAD", (uint64_t)_head, (uint32_t)(_end - _head));
	con_hex("DTB", dtb);
	if (fdt_walk((const void *)dtb, on_dt, I) < 0)
		stop("DTB BAD", dtb);
	if (!I->nmem || !I->fb.size || !I->ncpu)
		stop("DTB INCOMPLETE", 0);
	uint64_t l1 = mmu_build(I->mem, I->nmem, I->ko, I->nko, I->fb);
	if (!l1)
		stop("PT FAIL", 0);
	uint64_t chk[] = { (uint64_t)_head, (uint64_t)_end - 8, dtb, I->fb.base, (uint64_t)slots_tab,
			   (uint64_t)&info, (uint64_t)vectors, PIN_BASE };
	int want[] = { 2, 2, 2, 3, 2, 2, 2, 1 };
	for (unsigned i = 0; i < sizeof(chk) / sizeof(chk[0]); i++)
		if (mmu_kind(chk[i]) != want[i])
			stop("UNMAPPED", chk[i]);
	uint64_t tcr = TCR_BASE | ((rsr("S3_0_C0_C7_0") & 0xf) << 32);
	mmu_on(l1, tcr, MAIR_VALUE);
	con_hex("MMU", rsr("S3_0_C1_C0_0"));
	con_str("BOTOES", "nao toque por 2 s...");
	wait_ms(2000);
	keys_init();

	int sel = 0;
	draw_menu(sel);
	for (;;) {
		uint32_t ev = key_events();
		if (ev & K_DN) { sel = (sel + 1) & 7; draw_menu(sel); }
		else if (ev & K_UP) { sel = (sel + 7) & 7; draw_menu(sel); }
		else if (ev & K_PW) {
			if (sel == 0) scr_buttons();
			else if (sel == 1) scr_info();
			else if (sel == 2) scr_mem();
			else if (sel == 3) scr_colors();
			else if (sel == 4) scr_tp_power();
			else if (sel == 5) scr_tp_regs();
			else if (sel == 6) scr_tp_id();
			else scr_tp_live();
			draw_menu(sel);
		}
		wait_ms(10);
	}
}
