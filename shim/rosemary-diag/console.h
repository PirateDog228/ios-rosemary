#pragma once
#include <stdint.h>

/*
 * Text console on the framebuffer that MediaTek LK leaves lit.
 * Nothing about the framebuffer is hard-coded: con_init() takes the base
 * that LK wrote into the DTB (atag,videolfb-fb_base_l/h) and the geometry.
 */
void con_init(uint64_t base, uint32_t w, uint32_t h, uint32_t stride_bytes,
	      int rgba, int scale);

/* 0xRRGGBB colours, converted to the real pixel byte order by con_color(). */
#define C_BLACK  0x000000U
#define C_RED    0xff0000U
#define C_GREEN  0x00ff00U
#define C_BLUE   0x0000a0U
#define C_WHITE  0xffffffU
#define C_YELLOW 0xffff00U
#define C_CYAN   0x00ffffU
#define C_GREY   0xa0a0a0U

void con_clear(uint32_t rgb);
void con_color_fg(uint32_t rgb);		/* colour of the value column */
void con_skip(void);
void con_fill_bottom(void);
int  con_cols(void);
void con_str(const char *label, const char *s);
void con_status(const char *s); /* fixed status row below the live heartbeat */
void con_hex(const char *label, uint64_t v);
void con_hex2(const char *label, const char *a, uint32_t va,
	      const char *b, uint32_t vb);
void con_hex3(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb,
	      const char *c, uint32_t vc);
/* label, then value text wrapped over as many rows as needed */
void con_wrap(const char *label, const char *s);
/* Two-step line for risky reads: con_begin() draws the label at once, so a
 * hang is attributed to it; con_end_hex() then fills in the value. */
void con_begin(const char *label);
void con_end_hex32(uint32_t v);
void con_end_str(const char *s);
/* fixed-position counter used as heartbeat */
void con_heartbeat(uint64_t seconds);
