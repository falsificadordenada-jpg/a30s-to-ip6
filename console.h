#pragma once
#include <stdint.h>

#ifndef FB_BASE
#define FB_BASE	0xEC000000UL	/* A30s: reserved-memory fb_handover (dpu_fb_handover) */
#endif
#ifndef FB_W
#define FB_W		720
#endif
#ifndef FB_H
#define FB_H		1560
#endif
#define FB_STRIDE	(FB_W * 4)	/* assumed a8r8g8b8; border test shows if wrong */

#define RED	0xffff0000U
#define BLUE	0xff0000a0U
#define WHITE	0xffffffffU
#define YELLOW	0xffffff00U
#define GREEN	0xff00ff00U

extern volatile uint32_t *con_fb;

void con_clear(uint32_t bg);
void con_skip(void);
void con_str(const char *label, const char *s);
void con_hex(const char *label, uint64_t v);
/* label, then "a=xxxxxxxx b=xxxxxxxx" style pairs of 32-bit values */
void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb);

void con_fill(int x, int y, int w, int h, uint32_t c);
void con_set_row(int r);
void con_str_at(int col, const char *s, uint32_t fg);
/* label, then "AAAAAAAAAAAAAAAA BBBBBBBB" (64-bit a, 32-bit b) */
void con_line(const char *label, uint64_t a, uint32_t b);
