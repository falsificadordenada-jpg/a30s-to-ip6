/*
 * A30s marco 3a: DTB -> page tables -> MMU/caches on, then a menu driven by the hardware buttons.
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

volatile uint32_t g_probe, g_fault, g_esr;

void exc_handler(uint64_t slot, uint64_t *regs)
{
	uint64_t esr = rsr("S3_0_C5_C2_0"), elr = rsr("S3_0_C4_C0_1"), far = rsr("S3_0_C6_C0_0");
	if (g_probe && slot == 4 && (esr >> 26) == 0x25) {	/* data abort at current EL during a probed access */
		g_fault = 1;
		g_esr = (uint32_t)esr;
		__asm__ volatile("msr S3_0_C4_C0_1, %0\n isb" :: "r"(elr + 4));
		return;
	}
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

/* ---------- probed MMIO (a faulting access is skipped instead of panicking) ---------- */
static uint32_t sr(uint64_t a, int *ok)
{
	uint32_t v = 0;
	g_fault = 0; g_probe = 1;
	__asm__ volatile("ldr %w0, [%1]\n dsb sy\n isb" : "+r"(v) : "r"(a) : "memory");
	g_probe = 0;
	*ok = !g_fault;
	return v;
}
static int sw(uint64_t a, uint32_t v)
{
	g_fault = 0; g_probe = 1;
	__asm__ volatile("str %w0, [%1]\n dsb sy\n isb" :: "r"(v), "r"(a) : "memory");
	g_probe = 0;
	return !g_fault;
}
static uint32_t rd(uint64_t a) { int ok; uint32_t v = sr(a, &ok); return ok ? v : 0xFA17FA17u; }

#define CMU_PERI	0x10010000UL
#define GAT_I2C4	(CMU_PERI + 0x2048)
#define QCH_I2C4	(CMU_PERI + 0x3028)
#define PERI_PIN	0x139B0000UL
#define GPP4		(PERI_PIN + 0x0a0)
#define GPG2		(PERI_PIN + 0x160)
#define GPA0		(PIN_BASE + 0x40)
#define I2C4		0x13870000UL
#define IICCON		0x00
#define IICSTAT		0x04
#define IICADD		0x08
#define IICDS		0x0C
#define IICLC		0x10

static void pin_func(uint64_t bank, int pin, uint32_t f)
{
	int ok; uint32_t v = sr(bank, &ok);
	if (!ok) return;
	v &= ~(0xFu << (pin * 4)); v |= f << (pin * 4);
	sw(bank, v);
}
static void pin_pud(uint64_t bank, int pin, uint32_t p)
{
	int ok; uint32_t v = sr(bank + 8, &ok);
	if (!ok) return;
	v &= ~(0xFu << (pin * 4)); v |= p << (pin * 4);
	sw(bank + 8, v);
}
static void pin_out(uint64_t bank, int pin, int hi)
{
	int ok; uint32_t v = sr(bank + 4, &ok);
	if (!ok) return;
	v = hi ? (v | (1u << pin)) : (v & ~(1u << pin));
	sw(bank + 4, v);
	pin_func(bank, pin, 1);
}

static uint32_t i2c_con;
static uint64_t now(void) { return rsr("cntpct_el0"); }

static int i2c_wait_irq(void)
{
	uint64_t t = now();
	for (;;) {
		int ok; uint32_t c = sr(I2C4 + IICCON, &ok);
		if (!ok) return -2;
		if (c & 0x10) return 0;
		if (now() - t > ticks_ms(100)) return -1;
	}
}
static void i2c_clear_irq(void) { sw(I2C4 + IICCON, i2c_con & ~0x10u); }
static int i2c_nack(void) { return (rd(I2C4 + IICSTAT) & 1) != 0; }
static void i2c_stop(int rx)
{
	sw(I2C4 + IICSTAT, rx ? 0x90 : 0xD0);
	i2c_clear_irq();
	uint64_t t = now();
	while ((rd(I2C4 + IICSTAT) & 0x20) && now() - t < ticks_ms(20))
		;
}

/* write wl bytes, then (repeated start) read rl bytes. 0 = ok, negative/positive = where it failed */
static int i2c_xfer(uint8_t a7, const uint8_t *w, int wl, uint8_t *r, int rl)
{
	int e;
	i2c_con = 0xE0 | 0x0F;			/* ACKEN, TXDIV=512, IRQEN, prescaler 15 */
	sw(I2C4 + IICCON, i2c_con);
	sw(I2C4 + IICDS, (uint32_t)a7 << 1);
	sw(I2C4 + IICSTAT, 0xF0);
	if ((e = i2c_wait_irq())) { i2c_stop(0); return e == -2 ? -2 : 1; }
	if (i2c_nack()) { i2c_stop(0); return 2; }
	for (int i = 0; i < wl; i++) {
		sw(I2C4 + IICDS, w[i]);
		i2c_clear_irq();
		if (i2c_wait_irq()) { i2c_stop(0); return 3; }
		if (i2c_nack()) { i2c_stop(0); return 4; }
	}
	if (rl <= 0) { i2c_stop(0); return 0; }
	sw(I2C4 + IICDS, ((uint32_t)a7 << 1) | 1);
	sw(I2C4 + IICSTAT, 0xB0);
	i2c_clear_irq();
	if (i2c_wait_irq()) { i2c_stop(1); return 5; }
	if (i2c_nack()) { i2c_stop(1); return 6; }
	if (rl == 1) i2c_con &= ~0x80u;
	i2c_clear_irq();
	for (int k = 0; k < rl; k++) {
		if (i2c_wait_irq()) { i2c_stop(1); return 7; }
		r[k] = (uint8_t)rd(I2C4 + IICDS);
		if (k == rl - 1) break;
		if (k == rl - 2) i2c_con &= ~0x80u;
		i2c_clear_irq();
	}
	i2c_stop(1);
	return 0;
}

/* Imagis register access: 32-bit big-endian address, 32-bit big-endian words back */
static int ist_read(uint32_t reg, uint32_t *buf, int n)
{
	uint8_t a[4] = { reg >> 24, reg >> 16, reg >> 8, reg }, d[64];
	if (n > 16) n = 16;
	int e = i2c_xfer(0x50, a, 4, d, n * 4);
	if (e) return e;
	for (int i = 0; i < n; i++)
		buf[i] = ((uint32_t)d[i * 4] << 24) | ((uint32_t)d[i * 4 + 1] << 16) | ((uint32_t)d[i * 4 + 2] << 8) | d[i * 4 + 3];
	return 0;
}

#define IST_CHIPID	0xC0001000u
#define IST_TSTATUS	0xB00B0100u
#define IST_INTRMSG	0xB00B0104u
#define IST_COORD	0xB80B0108u	/* burst access to HIB coord */

static int trow;
static void tline(const char *lab, uint64_t v, int digits)
{
	putx(trow++, lab, v, digits);
}
static void tmsg(const char *lab, const char *s, uint32_t fg)
{
	put(trow++, lab, s, fg);
}

static void scr_touch(void)
{
	int ok;
	screen("Touch probe M3b");
	trow = 2;
	/* 1. clock for I2C4 */
	tline("GAT old", rd(GAT_I2C4), 8);
	tline("QCH old", rd(QCH_I2C4), 8);
	sw(GAT_I2C4, rd(GAT_I2C4) | (1u << 20) | (1u << 21));	/* manual + pass */
	sw(QCH_I2C4, rd(QCH_I2C4) | 3u);				/* enable + clock request */
	tline("GAT new", rd(GAT_I2C4), 8);
	tline("QCH new", rd(QCH_I2C4), 8);
	/* 2. pins: SDA/SCL = gpp4-0/1 function 2, pull-up; irq = gpa0-0 input; LDO = gpg2-1 high */
	pin_func(GPP4, 0, 2); pin_func(GPP4, 1, 2);
	pin_pud(GPP4, 0, 3); pin_pud(GPP4, 1, 3);
	pin_func(GPA0, 0, 0);
	tline("GPG2 old", rd(GPG2) , 8);
	pin_out(GPG2, 1, 1);
	tline("GPG2 new", rd(GPG2), 8);
	tline("GPP4", rd(GPP4), 8);
	tmsg("LDO", "tsp_ldo_en=1, aguardando 400ms", WHITE);
	wait_ms(400);
	tline("IRQ pin", rd(GPA0 + 4) & 1, 1);
	/* 3. i2c controller */
	uint32_t c = sr(I2C4 + IICCON, &ok);
	if (!ok) {
		tmsg("I2C", "FALHA ao ler IICCON (clock?)", RED);
		tline("ESR", g_esr, 8);
		put(24, "VOLTAR", "VOL- + VOL+ juntos", GREEN);
		goto wait_exit;
	}
	tline("IICCON", c, 8);
	tline("IICSTAT", rd(I2C4 + IICSTAT), 8);
	sw(I2C4 + IICADD, 0x10);
	sw(I2C4 + IICLC, 0x07);
	sw(I2C4 + IICSTAT, 0x10);
	tmsg("I2C", "controlador respondeu", GREEN);
	/* 4. chip id + status */
	uint32_t v[16];
	int e = ist_read(IST_CHIPID, v, 1);
	if (e) { tmsg("CHIPID", "erro i2c", RED); tline("  err", (uint32_t)e, 2); }
	else tline("CHIPID", v[0], 8);
	e = ist_read(IST_TSTATUS, v, 1);
	if (!e) tline("TSTATUS", v[0], 8); else tline("TSTAT e", (uint32_t)e, 2);
	e = ist_read(IST_INTRMSG, v, 1);
	if (!e) tline("INTRMSG", v[0], 8); else tline("INTR e", (uint32_t)e, 2);
	if (e) {
		put(24, "VOLTAR", "VOL- + VOL+ juntos", GREEN);
		goto wait_exit;
	}
	tmsg("TOQUE", "toque na tela (sair: VOL-+VOL+ 1 s)", GREEN);
	int px = -1, py = -1;
	uint32_t both = 0;
	for (;;) {
		uint32_t m = 0;
		int fe = ist_read(IST_INTRMSG, &m, 1);
		int nf = 0;
		uint32_t x = 0, y = 0;
		if (!fe && (m & 0x800)) {
			nf = (m >> 12) & 0xF;
			if (nf > 0 && nf <= 10) {
				uint32_t f[16];
				if (!ist_read(IST_COORD, f, 1)) {
					y = f[0] & 0xFFF;
					x = (f[0] >> 12) & 0xFFF;
				} else nf = 0;
			} else nf = 0;
		}
		if (px >= 0) { con_fill(px, py, 28, 28, 0xff102040); px = -1; }
		if (nf) {
			int dx = x > FB_W - 28 ? FB_W - 28 : (int)x, dy = y > FB_H - 40 ? FB_H - 40 : (int)y;
			if (dy < 300) dy = 300;
			con_fill(dx, dy, 28, 28, GREEN);
			px = dx; py = dy;
		}
		char b[34]; int p = 0; char t[12];
		p = cat(b, p, "n="); hexs(t, nf, 1); p = cat(b, p, t);
		p = cat(b, p, " x="); hexs(t, x, 3); p = cat(b, p, t);
		p = cat(b, p, " y="); hexs(t, y, 3); p = cat(b, p, t);
		b[p] = 0;
		put(22, "LIVE", b, WHITE);
		p = 0;
		p = cat(b, p, "msg="); hexs(t, m, 8); p = cat(b, p, t);
		p = cat(b, p, " e="); hexs(t, (uint32_t)fe, 2); p = cat(b, p, t);
		b[p] = 0;
		put(23, "RAW", b, WHITE);
		uint32_t k = raw_keys();
		if ((k & (K_UP | K_DN)) == (K_UP | K_DN)) {
			if (!both) both = (uint32_t)now() | 1;
			else if ((uint32_t)now() - (both & ~1u) > ticks_ms(1000)) break;
		} else both = 0;
		wait_ms(15);
	}
	goto done;
wait_exit:
	for (;;) {
		uint32_t k = raw_keys();
		if ((k & (K_UP | K_DN)) == (K_UP | K_DN)) { wait_ms(1000); if ((raw_keys() & (K_UP | K_DN)) == (K_UP | K_DN)) break; }
		wait_ms(20);
	}
done:
	while (raw_keys())
		wait_ms(20);
	key_events();
	k_stable = 0;
}

static const char *item_name(int i)
{
	switch (i) {
	case 0: return "1  Teste de botoes";
	case 1: return "2  Info do sistema";
	case 2: return "3  Mapa de memoria";
	case 3: return "4  Teste de cores";
	default: return "5  Touch (I2C)";
	}
}

static void draw_menu(int sel)
{
	screen("M3b menu");
	for (int i = 0; i < 5; i++) {
		con_set_row(3 + i * 2);
		con_str_at(0, i == sel ? ">" : " ", GREEN);
		con_str_at(2, item_name(i), i == sel ? GREEN : WHITE);
	}
	put(15, "VOL-/+", "mover", WHITE);
	put(16, "POWER", "escolher (ou VOL-+VOL+)", WHITE);
	put(18, "RESET", "segure POWER ~7 s", YELLOW);
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
		if (ev & K_DN) { sel = (sel + 1) % 5; draw_menu(sel); }
		else if (ev & K_UP) { sel = (sel + 4) % 5; draw_menu(sel); }
		else if (ev & K_PW) {
			if (sel == 0) scr_buttons();
			else if (sel == 1) scr_info();
			else if (sel == 2) scr_mem();
			else if (sel == 3) scr_colors();
			else scr_touch();
			draw_menu(sel);
		}
		wait_ms(10);
	}
}
