#include "mmu.h"

#define A4K __attribute__((aligned(4096)))
#define PTE_BLOCK	1ULL
#define PTE_TABLE	3ULL
#define PTE_AF		(1ULL << 10)
#define PTE_SH_INNER	(3ULL << 8)
#define PTE_SH_OUTER	(2ULL << 8)
#define PTE_PXN		(1ULL << 53)
#define PTE_UXN		(1ULL << 54)
#define ATTR_DEV	(0ULL << 2)
#define ATTR_WB		(1ULL << 2)
#define ATTR_NC		(2ULL << 2)
#define ADDR_MASK	0x0000FFFFFFFFF000ULL
#define MB2		0x200000ULL
#define NL2		6

static uint64_t l1[512] A4K;
static uint64_t l2pool[NL2][512] A4K;
static int nl2;

static uint64_t *l2_for(uint64_t gb, int create)
{
	uint64_t e = l1[gb];
	if ((e & 3) == PTE_TABLE)
		return (uint64_t *)(e & ADDR_MASK);
	if ((e & 3) == PTE_BLOCK || !create || nl2 >= NL2)
		return 0;
	uint64_t *t = l2pool[nl2++];
	l1[gb] = (uint64_t)t | PTE_TABLE;
	return t;
}

static int map2m(uint64_t pa, uint64_t bits)
{
	if ((pa >> 30) >= 512)
		return -1;
	uint64_t *t = l2_for(pa >> 30, 1);
	if (!t)
		return -1;
	t[(pa >> 21) & 511] = pa | bits | PTE_BLOCK;
	return 0;
}

static void unmap2m(uint64_t pa)
{
	if ((pa >> 30) >= 512)
		return;
	uint64_t *t = l2_for(pa >> 30, 0);
	if (t)
		t[(pa >> 21) & 511] = 0;
}

uint64_t mmu_build(const range_t *mem, int nmem, const range_t *ko, int nko, range_t fb)
{
	const uint64_t wb = PTE_AF | PTE_SH_INNER | ATTR_WB;
	const uint64_t nc = PTE_AF | PTE_SH_INNER | ATTR_NC;
	for (int i = 0; i < 512; i++)
		l1[i] = 0;
	nl2 = 0;
	/* 0-1GB: SoC peripherals, Device-nGnRE, never executable */
	l1[0] = 0 | PTE_AF | PTE_SH_OUTER | ATTR_DEV | PTE_PXN | PTE_UXN | PTE_BLOCK;

	for (int i = 0; i < nmem; i++) {
		uint64_t a = mem[i].base & ~(MB2 - 1), e = (mem[i].base + mem[i].size) & ~(MB2 - 1);
		for (; a < e; a += MB2)
			if (map2m(a, wb))
				return 0;
	}
	for (int i = 0; i < nko; i++) {
		uint64_t a = ko[i].base & ~(MB2 - 1), e = (ko[i].base + ko[i].size + MB2 - 1) & ~(MB2 - 1);
		for (; a < e; a += MB2)
			unmap2m(a);
	}
	if (fb.size) {
		uint64_t a = fb.base & ~(MB2 - 1), e = (fb.base + fb.size + MB2 - 1) & ~(MB2 - 1);
		for (; a < e; a += MB2)
			if (map2m(a, nc))
				return 0;
	}
	return (uint64_t)l1;
}

int mmu_kind(uint64_t addr)
{
	if ((addr >> 30) >= 512)
		return 0;
	uint64_t e = l1[addr >> 30];
	if ((e & 3) == PTE_TABLE) {
		e = ((uint64_t *)(e & ADDR_MASK))[(addr >> 21) & 511];
		if ((e & 3) != PTE_BLOCK)
			return 0;
	} else if ((e & 3) != PTE_BLOCK) {
		return 0;
	}
	switch ((e >> 2) & 7) {
	case 0: return 1;
	case 1: return 2;
	case 2: return 3;
	}
	return 0;
}
