/* Minimal flattened-device-tree walker. No libc, no static pointer tables (image is position independent). */
#include "fdt.h"

uint32_t fdt_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

uint64_t fdt_cells(const uint8_t *p, int n)
{
	uint64_t v = 0;
	while (n-- > 0) {
		v = (v << 32) | fdt_be32(p);
		p += 4;
	}
	return v;
}

int fdt_streq(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return *a == *b;
}

int fdt_starts(const char *s, const char *prefix)
{
	while (*prefix) {
		if (*s++ != *prefix++)
			return 0;
	}
	return 1;
}

int fdt_walk(const void *dtb, fdt_cb cb, void *u)
{
	const uint8_t *b = dtb;
	if (fdt_be32(b) != 0xd00dfeedU)
		return -1;
	uint32_t off_struct = fdt_be32(b + 8), off_str = fdt_be32(b + 12), size_struct = fdt_be32(b + 36);
	const uint8_t *p = b + off_struct, *end = p + size_struct;
	const char *strs = (const char *)(b + off_str);
	const char *names[24];
	int depth = -1;

	while (p + 4 <= end) {
		uint32_t tag = fdt_be32(p);
		p += 4;
		if (tag == 1) {			/* FDT_BEGIN_NODE */
			const char *name = (const char *)p;
			uint32_t n = 0;
			while (p[n]) n++;
			p += (n + 1 + 3) & ~3U;
			depth++;
			if (depth >= 24)
				return -2;
			names[depth] = name;
			cb(u, FDT_ENTER, depth, name, depth ? names[depth - 1] : "", 0, 0, 0);
		} else if (tag == 2) {		/* FDT_END_NODE */
			cb(u, FDT_EXIT, depth, names[depth], depth ? names[depth - 1] : "", 0, 0, 0);
			depth--;
		} else if (tag == 3) {		/* FDT_PROP */
			uint32_t len = fdt_be32(p), nameoff = fdt_be32(p + 4);
			const uint8_t *val = p + 8;
			p += 8 + ((len + 3) & ~3U);
			if (depth < 0)
				return -3;
			cb(u, FDT_PROP, depth, names[depth], depth ? names[depth - 1] : "", strs + nameoff, val, len);
		} else if (tag == 4) {		/* FDT_NOP */
		} else if (tag == 9) {		/* FDT_END */
			return 0;
		} else {
			return -4;
		}
	}
	return 0;
}
