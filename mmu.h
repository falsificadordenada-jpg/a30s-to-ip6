#pragma once
#include "types.h"
/* Builds identity-mapped stage-1 tables (4K granule, T0SZ=25, 39-bit VA).
 * Device: 0-1GB. Normal WB: every `mem` range minus `keepout` ranges. `fb` is Normal non-cacheable.
 * Returns the physical address of the level-1 table, or 0 on error. */
uint64_t mmu_build(const range_t *mem, int nmem, const range_t *ko, int nko, range_t fb);
/* 0 = unmapped, 1 = device, 2 = normal WB, 3 = normal NC */
int mmu_kind(uint64_t addr);
#define MAIR_VALUE	0x0044FF04ULL
#define TCR_BASE	(25ULL | (1ULL << 8) | (1ULL << 10) | (3ULL << 12) | (25ULL << 16) | (1ULL << 23))
