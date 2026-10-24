/*
 * Minimal text console on the framebuffer left by LK. MMU is off, so every
 * access is Device memory: keep stores 32-bit aligned (-mstrict-align).
 */
#include "console.h"
#include "font.h"

#define LABEL_COLS 10
#define MARGIN     16
#define STATUS_ROW 2

static volatile uint32_t *fb;
static uint32_t fb_w, fb_h, fb_stride_px;
static int px_rgba;
static int scale = 2;
static uint32_t bg_px, fg_val = 0xffffff;
static int row, hb_row = -1, hb_col;
static int begin_col;

static uint32_t px(uint32_t rgb)
{
	uint32_t r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
	return px_rgba ? 0xff000000u | (b << 16) | (g << 8) | r
		       : 0xff000000u | (r << 16) | (g << 8) | b;
}

void con_init(uint64_t base, uint32_t w, uint32_t h, uint32_t stride_bytes,
	      int rgba, int sc)
{
	fb = (volatile uint32_t *)base;
	fb_w = w;
	fb_h = h;
	fb_stride_px = stride_bytes / 4;
	px_rgba = rgba;
	scale = sc < 1 ? 1 : sc > 4 ? 4 : sc;
}

int con_cols(void)
{
	return (int)(fb_w - 2 * MARGIN) / (8 * scale);
}

void con_color_fg(uint32_t rgb)
{
	fg_val = rgb;
}

void con_clear(uint32_t rgb)
{
	bg_px = px(rgb);
	for (uint32_t y = 0; y < fb_h; y++) {
		volatile uint32_t *line = fb + y * fb_stride_px;
		for (uint32_t x = 0; x < fb_w; x++)
			line[x] = bg_px;
	}
	row = 0;
	hb_row = -1;
}

/* repaint the bottom 300 rows in the background colour: a visible-memory write
 * workload for timing that does not erase the text above it */
void con_fill_bottom(void)
{
	for (uint32_t y = fb_h - 300; y < fb_h; y++) {
		volatile uint32_t *line = fb + y * fb_stride_px;
		for (uint32_t x = 0; x < fb_w; x++)
			line[x] = bg_px;
	}
}

static void put_char(int col, int r, char ch, uint32_t fg_rgb)
{
	if (ch < 0x20 || ch > 0x7e)
		ch = '?';
	const unsigned char *g = font8x16[ch - 0x20];
	uint32_t fg = px(fg_rgb);
	uint32_t cw = 8 * scale, chh = 16 * scale;
	uint32_t x0 = MARGIN + col * cw, y0 = MARGIN + r * chh;
	if (x0 + cw > fb_w || y0 + chh > fb_h)
		return;
	for (int gy = 0; gy < 16; gy++)
		for (int gx = 0; gx < 8; gx++) {
			uint32_t c = (g[gy] & (0x80 >> gx)) ? fg : bg_px;
			for (int sy = 0; sy < scale; sy++) {
				volatile uint32_t *p = fb +
					(y0 + gy * scale + sy) * fb_stride_px + x0 + gx * scale;
				for (int sx = 0; sx < scale; sx++)
					p[sx] = c;
			}
		}
}

static int put_str(int col, int r, const char *s, uint32_t fg)
{
	while (*s)
		put_char(col++, r, *s++, fg);
	return col;
}

static void fmt_hex(char *buf, uint64_t v, int digits)
{
	static const char hex[] = "0123456789ABCDEF";
	for (int i = 0; i < digits; i++)
		buf[i] = hex[(v >> (4 * (digits - 1 - i))) & 0xf];
	buf[digits] = 0;
}

void con_skip(void)
{
	row++;
}

void con_str(const char *label, const char *s)
{
	put_str(0, row, label, C_YELLOW);
	put_str(LABEL_COLS, row, s, fg_val);
	row++;
}

/* Fixed top-of-screen status line, independent of the scrolling diagnostic dump. */
void con_status(const char *s)
{
	int cols = con_cols();
	put_str(0, STATUS_ROW, "STATUS", C_YELLOW);
	for (int col = LABEL_COLS; col < cols; col++)
		put_char(col, STATUS_ROW, ' ', fg_val);
	put_str(LABEL_COLS, STATUS_ROW, s, fg_val);
}

void con_hex(const char *label, uint64_t v)
{
	char buf[17];
	fmt_hex(buf, v, 16);
	con_str(label, buf);
}

void con_hex2(const char *label, const char *a, uint32_t va,
	      const char *b, uint32_t vb)
{
	char buf[9];
	int col = put_str(0, row, label, C_YELLOW);
	col = col < LABEL_COLS ? LABEL_COLS : col + 1;
	col = put_str(col, row, a, C_YELLOW);
	fmt_hex(buf, va, 8);
	col = put_str(col, row, buf, fg_val);
	if (b) {
		col = put_str(col + 1, row, b, C_YELLOW);
		fmt_hex(buf, vb, 8);
		put_str(col, row, buf, fg_val);
	}
	row++;
}

void con_hex3(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb,
	      const char *c, uint32_t vc)
{
	char buf[9];
	int col = put_str(0, row, label, C_YELLOW);
	col = col < 6 ? 6 : col + 1;
	col = put_str(col, row, a, C_YELLOW);
	fmt_hex(buf, va, 8);
	col = put_str(col, row, buf, fg_val);
	col = put_str(col + 1, row, b, C_YELLOW);
	fmt_hex(buf, vb, 8);
	col = put_str(col, row, buf, fg_val);
	col = put_str(col + 1, row, c, C_YELLOW);
	fmt_hex(buf, vc, 8);
	put_str(col, row, buf, fg_val);
	row++;
}

void con_wrap(const char *label, const char *s)
{
	int width = con_cols() - LABEL_COLS;
	put_str(0, row, label, C_YELLOW);
	do {
		int n = 0;
		while (s[n] && n < width)
			n++;
		for (int i = 0; i < n; i++)
			put_char(LABEL_COLS + i, row, s[i], fg_val);
		s += n;
		row++;
	} while (*s);
}

void con_begin(const char *label)
{
	begin_col = put_str(0, row, label, C_YELLOW);
	if (begin_col < LABEL_COLS + 6)
		begin_col = LABEL_COLS + 6;
	put_str(begin_col, row, "...", C_GREY);
}

void con_end_hex32(uint32_t v)
{
	char buf[9];
	fmt_hex(buf, v, 8);
	put_str(begin_col, row, "        ", fg_val);
	put_str(begin_col, row, buf, fg_val);
	row++;
}

void con_end_str(const char *s)
{
	put_str(begin_col, row, "        ", fg_val);
	put_str(begin_col, row, s, fg_val);
	row++;
}

void con_heartbeat(uint64_t seconds)
{
	if (hb_row < 0) {
		hb_row = row++;
		hb_col = put_str(0, hb_row, "ALIVE", C_GREEN) + 1;
	}
	char buf[9];
	fmt_hex(buf, seconds, 8);
	put_str(hb_col, hb_row, buf, C_GREEN);
	put_str(hb_col + 9, hb_row, "s", C_GREEN);
}
