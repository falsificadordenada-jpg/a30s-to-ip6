#pragma once
#include <stdint.h>
enum { FDT_ENTER = 0, FDT_PROP = 1, FDT_EXIT = 2 };
/* node/parent are node names ("" for root). prop/val/len only for FDT_PROP. */
typedef void (*fdt_cb)(void *u, int ev, int depth, const char *node, const char *parent,
		       const char *prop, const uint8_t *val, uint32_t len);
int fdt_walk(const void *dtb, fdt_cb cb, void *u);	/* <0: bad DTB */
uint32_t fdt_be32(const uint8_t *p);
uint64_t fdt_cells(const uint8_t *p, int ncells);
int fdt_streq(const char *a, const char *b);
int fdt_starts(const char *s, const char *prefix);
