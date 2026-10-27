#pragma once
#include <stdint.h>
#include "fdt.h"
#include "types.h"

#define MAXMEM 8
#define MAXKO 16
#define MAXCPU 16

struct info {
	uint32_t root_ac, root_sc, rm_ac, rm_sc, cpu_ac;
	range_t mem[MAXMEM]; int nmem;
	range_t ko[MAXKO]; int nko;
	range_t fb; int nresv;
	uint64_t cpu[MAXCPU]; int ncpu;
	uint32_t psci_cpu_on; int psci_smc;
	uint64_t gicd, gicc;
	/* per-node scratch */
	int is_mem, is_resv, is_cpu, is_gic, is_psci, have_reg, ko_flag, fb_flag;
	uint64_t reg_a, reg_b, reg_c, reg_d;	/* first two (addr,size) pairs */
};

static int has_str(const uint8_t *v, uint32_t len, const char *s)
{
	uint32_t i = 0;
	while (i < len) {
		if (fdt_streq((const char *)v + i, s))
			return 1;
		while (i < len && v[i]) i++;
		i++;
	}
	return 0;
}

static void on_dt(void *u, int ev, int depth, const char *node, const char *parent,
		  const char *prop, const uint8_t *val, uint32_t len)
{
	struct info *I = u;
	if (ev == FDT_ENTER) {
		I->is_mem = fdt_starts(node, "memory@");
		I->is_resv = fdt_streq(parent, "reserved-memory");
		I->is_cpu = fdt_streq(parent, "cpus") && fdt_starts(node, "cpu@");
		I->is_gic = depth == 1 && fdt_starts(node, "interrupt-controller@");
		I->is_psci = depth == 1 && fdt_streq(node, "psci");
		I->have_reg = 0; I->ko_flag = 0; I->fb_flag = 0;
		I->reg_a = I->reg_b = I->reg_c = I->reg_d = 0;
		return;
	}
	if (ev == FDT_EXIT) {
		if (I->is_mem && I->have_reg && I->nmem < MAXMEM) {
			I->mem[I->nmem].base = I->reg_a; I->mem[I->nmem].size = I->reg_b; I->nmem++;
		}
		if (I->is_resv) {
			I->nresv++;
			if (I->have_reg && I->ko_flag && I->nko < MAXKO) {
				I->ko[I->nko].base = I->reg_a; I->ko[I->nko].size = I->reg_b; I->nko++;
			}
			if (I->have_reg && I->fb_flag) {
				I->fb.base = I->reg_a; I->fb.size = I->reg_b;
			}
		}
		if (I->is_cpu && I->have_reg && I->ncpu < MAXCPU)
			I->cpu[I->ncpu++] = I->reg_a;
		if (I->is_gic && I->have_reg) {
			I->gicd = I->reg_a; I->gicc = I->reg_c;
		}
		I->is_mem = I->is_resv = I->is_cpu = I->is_gic = I->is_psci = 0;
		return;
	}
	/* FDT_PROP */
	if (depth == 0) {
		if (fdt_streq(prop, "#address-cells")) I->root_ac = fdt_be32(val);
		if (fdt_streq(prop, "#size-cells")) I->root_sc = fdt_be32(val);
		return;
	}
	if (fdt_streq(node, "reserved-memory") && depth == 1) {
		if (fdt_streq(prop, "#address-cells")) I->rm_ac = fdt_be32(val);
		if (fdt_streq(prop, "#size-cells")) I->rm_sc = fdt_be32(val);
		return;
	}
	if (fdt_streq(node, "cpus") && depth == 1) {
		if (fdt_streq(prop, "#address-cells")) I->cpu_ac = fdt_be32(val);
		return;
	}
	if (I->is_psci) {
		if (fdt_streq(prop, "method")) I->psci_smc = fdt_streq((const char *)val, "smc");
		if (fdt_streq(prop, "cpu_on")) I->psci_cpu_on = fdt_be32(val);
		return;
	}
	if (fdt_streq(prop, "compatible") && I->is_resv) {
		if (has_str(val, len, "exynos,modem_if") || has_str(val, len, "exynos,cp_ram_logging") ||
		    has_str(val, len, "exynos,gnss_if") || has_str(val, len, "removed-dma-pool"))
			I->ko_flag = 1;
		if (has_str(val, len, "exynos,dpu_fb_handover"))
			I->fb_flag = 1;
		return;
	}
	if (fdt_streq(prop, "reg") && (I->is_mem || I->is_resv || I->is_cpu || I->is_gic)) {
		int ac = I->is_cpu ? (int)I->cpu_ac : (I->is_resv ? (int)I->rm_ac : (int)I->root_ac);
		int sc = I->is_cpu ? 0 : (I->is_resv ? (int)I->rm_sc : (int)I->root_sc);
		if (ac < 1 || ac > 2 || sc < 0 || sc > 2)
			return;
		uint32_t stride = (ac + sc) * 4;
		if (len >= stride) {
			I->reg_a = fdt_cells(val, ac);
			I->reg_b = sc ? fdt_cells(val + ac * 4, sc) : 0;
			I->have_reg = 1;
		}
		if (I->is_gic && len >= 2 * stride) {
			I->reg_c = fdt_cells(val + stride, ac);
			I->reg_d = sc ? fdt_cells(val + stride + ac * 4, sc) : 0;
		}
	}
}

