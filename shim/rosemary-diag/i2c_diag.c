/*
 * Rosemary stage 10: touch bus scan by bit-banging I2C0's pins (GPIO51 = SCL0,
 * GPIO52 = SDA0) with the controller out of the picture.
 *
 * Why: the kernel-side probe through the I2C0 controller "acked" every address
 * (even an empty one) and returned junk, so it said nothing about the touch IC.
 * This drives the lines directly (open drain emulated with GPIO direction, the
 * external pull-ups give the high level), scans 0x03..0x77 and reads ID
 * registers from whatever answers. LK left VIO28/VIO18 on and GPIO2 (reset) low,
 * so each pass first releases reset like the Goodix datasheet sequence.
 *
 * Cmdline: rdiag_tp=<mask>  1 = pass A (INT low during reset -> addr 0x5d)
 *                           2 = pass B (INT high during reset -> addr 0x14)
 *                           4 = pass C (no reset, just scan)
 *                           8 = first switch on the touch supply: MT6360 LDO2 ("vtp") over
 *                               bit-banged I2C5 (GPIO28/29, slave 0x64, reg 0x11 bit 6)  default 15
 * Output is on screen only (photograph it). Writes: GPIO 1, 2, 51, 52 only.
 */
#include <stdint.h>
#include "console.h"
#include "i2c_diag.h"

#define GPIO_BASE	0x10005000UL
#define PIN_IRQ		1
#define PIN_RST		2
/* bit-bang bus pins: I2C0 = GPIO51/52 (touch), I2C5 = GPIO28/29 (MT6360 sub-PMIC) */
static unsigned p_scl = 51, p_sda = 52;

static uint64_t freq;

static uint64_t ticks(void)
{
	uint64_t t;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
	return t;
}

static void delay_us(uint32_t us)
{
	uint64_t end = ticks() + freq / 1000000 * us;
	while (ticks() < end)
		;
}

static void delay_ms(uint32_t ms)
{
	while (ms--)
		delay_us(1000);
}

static inline uint32_t grd(uint32_t off)
{
	return *(volatile uint32_t *)(GPIO_BASE + off);
}

static inline void gwr(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(GPIO_BASE + off) = v;
}

static unsigned gpio_mode(unsigned n)
{
	return (grd(0x300 + 0x10 * (n / 8)) >> (4 * (n % 8))) & 7;
}

static unsigned gpio_bit(uint32_t base, unsigned n)
{
	return (grd(base + 0x10 * (n / 32)) >> (n % 32)) & 1;
}

static void gpio_set_mode(unsigned n, unsigned mode)
{
	uint32_t base = 0x300 + 0x10 * (n / 8);
	unsigned sh = 4 * (n % 8);
	gwr(base + 8, 7u << sh);
	gwr(base + 4, (mode & 7u) << sh);
}

static void gpio_out(unsigned n, unsigned level)
{
	uint32_t m = 1u << (n % 32);
	gwr(0x000 + 0x10 * (n / 32) + 4, m);
	gwr(0x100 + 0x10 * (n / 32) + (level ? 4 : 8), m);
}

static void gpio_in(unsigned n)
{
	gwr(0x000 + 0x10 * (n / 32) + 8, 1u << (n % 32));
}

static unsigned gpio_din(unsigned n)
{
	return gpio_bit(0x200, n);
}

/* ---- bit-banged I2C, about 40 kHz ---- */
#define HALF 12

static void scl_lo(void) { gpio_out(p_scl, 0); }
static void sda_lo(void) { gpio_out(p_sda, 0); }
static void sda_hi(void) { gpio_in(p_sda); }
static unsigned sda_rd(void) { return gpio_din(p_sda); }

static void scl_hi(void)
{
	gpio_in(p_scl);
	for (int i = 0; i < 1000 && !gpio_din(p_scl); i++)	/* clock stretching */
		delay_us(1);
}

static void i2c_start(void)
{
	sda_hi();
	scl_hi();
	delay_us(HALF);
	sda_lo();
	delay_us(HALF);
	scl_lo();
}

static void i2c_stop(void)
{
	sda_lo();
	delay_us(HALF);
	scl_hi();
	delay_us(HALF);
	sda_hi();
	delay_us(HALF);
}

/* returns 1 when the device pulled SDA low for the ninth clock (ACK) */
static int i2c_wr(uint8_t b)
{
	for (int i = 7; i >= 0; i--) {
		if ((b >> i) & 1)
			sda_hi();
		else
			sda_lo();
		delay_us(HALF);
		scl_hi();
		delay_us(HALF);
		scl_lo();
	}
	sda_hi();
	delay_us(HALF);
	scl_hi();
	delay_us(HALF);
	int ack = !sda_rd();
	scl_lo();
	return ack;
}

static uint8_t i2c_rd(int ack)
{
	uint8_t v = 0;
	sda_hi();
	for (int i = 0; i < 8; i++) {
		delay_us(HALF);
		scl_hi();
		delay_us(HALF);
		v = (uint8_t)((v << 1) | sda_rd());
		scl_lo();
	}
	if (ack)
		sda_lo();
	else
		sda_hi();
	delay_us(HALF);
	scl_hi();
	delay_us(HALF);
	scl_lo();
	sda_hi();
	return v;
}

/* addr, nreg register bytes, then nrd bytes read back after a repeated start */
static int i2c_read(uint8_t addr, const uint8_t *reg, int nreg, uint8_t *out, int nrd)
{
	i2c_start();
	if (!i2c_wr((uint8_t)(addr << 1))) {
		i2c_stop();
		return -1;
	}
	for (int i = 0; i < nreg; i++) {
		if (!i2c_wr(reg[i])) {
			i2c_stop();
			return -2;
		}
	}
	i2c_start();
	if (!i2c_wr((uint8_t)(addr << 1 | 1))) {
		i2c_stop();
		return -3;
	}
	for (int i = 0; i < nrd; i++)
		out[i] = i2c_rd(i < nrd - 1);
	i2c_stop();
	return 0;
}

static const char hexd[] = "0123456789abcdef";

static char *put_hex(char *p, uint32_t v, int digits)
{
	for (int i = digits - 1; i >= 0; i--)
		*p++ = hexd[(v >> (4 * i)) & 15];
	return p;
}

static void show_pins(const char *tag)
{
	char s[64], *p = s;
	*p++ = 'S'; *p++ = 'C'; *p++ = 'L'; *p++ = '='; *p++ = (char)('0' + gpio_din(p_scl));
	*p++ = ' '; *p++ = 'S'; *p++ = 'D'; *p++ = 'A'; *p++ = '='; *p++ = (char)('0' + gpio_din(p_sda));
	*p++ = ' '; *p++ = 'I'; *p++ = 'R'; *p++ = 'Q'; *p++ = '='; *p++ = (char)('0' + gpio_din(PIN_IRQ));
	*p++ = ' '; *p++ = 'R'; *p++ = 'S'; *p++ = 'T'; *p++ = '='; *p++ = (char)('0' + gpio_din(PIN_RST));
	*p = 0;
	con_str(tag, s);
}

/* No pointer tables: the image is not relocated, absolute addresses would be wrong. */
static void try_reg(uint8_t addr, const char *name, const uint8_t *reg, int nreg, int nb)
{
	uint8_t b[8];
	char s[64], *p = s;
	int e = i2c_read(addr, reg, nreg, b, nb);

	*p++ = 'e'; *p++ = '='; *p++ = e < 0 ? '-' : '0'; *p++ = (char)('0' + (e < 0 ? -e : e)); *p++ = ' ';
	if (e == 0)
		for (int i = 0; i < nb; i++)
			p = put_hex(p, b[i], 2), *p++ = ' ';
	*p = 0;
	con_str(name, s);
}

static void read_ids(uint8_t addr)
{
	static const uint8_t g1[] = { 0x40, 0x22 }, g2[] = { 0x41, 0x00 }, g3[] = { 0x81, 0x40 };
	static const uint8_t g4[] = { 0x00, 0x01, 0x00, 0x14 };
	static const uint8_t f1[] = { 0xa3 }, f2[] = { 0xa6 };

	try_reg(addr, "reg 4022", g1, 2, 8);
	try_reg(addr, "reg 4100", g2, 2, 8);
	try_reg(addr, "reg 8140", g3, 2, 8);
	try_reg(addr, "reg 10014", g4, 4, 8);
	try_reg(addr, "reg a3", f1, 1, 2);
	try_reg(addr, "reg a6", f2, 1, 2);
}

static void scan(const char *tag)
{
	char s[96], *p = s;
	int found = 0;

	show_pins(tag);
	for (unsigned a = 0x03; a < 0x78; a++) {
		i2c_start();
		int ack = i2c_wr((uint8_t)(a << 1));
		i2c_stop();
		if (ack && p < s + 80) {
			p = put_hex(p, a, 2);
			*p++ = ' ';
			found++;
		}
		delay_us(50);
	}
	*p = 0;
	con_wrap("ACK addrs", found ? s : "none");
	/* read IDs from the usual touch addresses even if they did not show up, as a control */
	for (unsigned k = 0; k < 3; k++) {
		static const uint8_t ta[] = { 0x5d, 0x14, 0x38 };
		i2c_start();
		int ack = i2c_wr((uint8_t)(ta[k] << 1));
		i2c_stop();
		if (ack) {
			char h[8];
			put_hex(h, ta[k], 2)[0] = 0;
			con_str("ids from", h);
			read_ids(ta[k]);
		}
	}
	con_skip();
}

/* Can we really pull the two lines low and release them? If not, "no ACK" means nothing. */
static void selftest(void)
{
	char s[64], *p = s;
	unsigned scl_lo_v, scl_hi_v, sda_lo_v, sda_hi_v;

	gpio_out(p_scl, 0);
	delay_us(50);
	scl_lo_v = gpio_din(p_scl);
	gpio_in(p_scl);
	delay_us(200);
	scl_hi_v = gpio_din(p_scl);
	gpio_out(p_sda, 0);
	delay_us(50);
	sda_lo_v = gpio_din(p_sda);
	gpio_in(p_sda);
	delay_us(200);
	sda_hi_v = gpio_din(p_sda);
	*p++ = 'S'; *p++ = 'C'; *p++ = 'L'; *p++ = ' '; *p++ = 'l'; *p++ = 'o'; *p++ = '='; *p++ = (char)('0' + scl_lo_v);
	*p++ = ' '; *p++ = 'h'; *p++ = 'i'; *p++ = '='; *p++ = (char)('0' + scl_hi_v);
	*p++ = ' '; *p++ = 'S'; *p++ = 'D'; *p++ = 'A'; *p++ = ' '; *p++ = 'l'; *p++ = 'o'; *p++ = '='; *p++ = (char)('0' + sda_lo_v);
	*p++ = ' '; *p++ = 'h'; *p++ = 'i'; *p++ = '='; *p++ = (char)('0' + sda_hi_v);
	*p = 0;
	con_str("pin test", s);
	con_str("expect", "lo=0 hi=1 on both");
}

/* ---- MT6360 sub-PMIC on I2C5: LDO2 is the touch supply ("vtp", 3.0 V in Android) ---- */
static uint8_t crc8(const uint8_t *d, int n)
{
	uint8_t c = 0;

	while (n--) {
		c ^= *d++;
		for (int i = 0; i < 8; i++)
			c = (uint8_t)((c & 0x80) ? (c << 1) ^ 0x07 : c << 1);
	}
	return c;
}

/* The MT6360 PMIC/LDO blocks check a PEC byte (CRC-8, poly 7) on writes and silently drop bad ones.
 * variant 0: no PEC, 1: CRC over addr_w+reg+val, 2: CRC over reg+val */
static int i2c_wreg(uint8_t addr, uint8_t reg, uint8_t val, int variant)
{
	uint8_t m[3];
	int ok;

	m[0] = (uint8_t)(addr << 1);
	m[1] = reg;
	m[2] = val;
	i2c_start();
	ok = i2c_wr(m[0]) && i2c_wr(reg) && i2c_wr(val);
	if (ok && variant == 1)
		ok = i2c_wr(crc8(m, 3));
	else if (ok && variant == 2)
		ok = i2c_wr(crc8(m + 1, 2));
	i2c_stop();
	return ok;
}

static void show_byte(const char *label, int e, uint8_t v)
{
	char s[16], *p = s;

	if (e) {
		con_str(label, "read failed");
		return;
	}
	p = put_hex(p, v, 2);
	*p = 0;
	con_str(label, s);
}

static void enable_vtp(void)
{
	uint8_t reg, v = 0;
	int e;

	p_scl = 28;
	p_sda = 29;
	gpio_set_mode(p_scl, 0);
	gpio_set_mode(p_sda, 0);
	sda_hi();
	gpio_in(p_scl);
	delay_ms(1);
	show_pins("bus5 idle");
	/* who is on I2C5 */
	{
		char s[96], *p = s;
		static const uint8_t cand[] = { 0x1a, 0x34, 0x4e, 0x64 };
		for (unsigned k = 0; k < sizeof(cand); k++) {
			i2c_start();
			int ack = i2c_wr((uint8_t)(cand[k] << 1));
			i2c_stop();
			p = put_hex(p, cand[k], 2);
			*p++ = ack ? '+' : '-';
			*p++ = ' ';
		}
		*p = 0;
		con_str("bus5 acks", s);
	}
	reg = 0x11;
	e = i2c_read(0x64, &reg, 1, &v, 1);
	show_byte("LDO2 en 0x11", e, v);
	reg = 0x15;
	e = i2c_read(0x64, &reg, 1, &v, 1);
	show_byte("LDO2 vsel 0x15", e, v);
	if (e == 0) {
		uint8_t en = 0, got = 0;
		int variant, good = -1;

		reg = 0x11;
		if (i2c_read(0x64, &reg, 1, &en, 1) == 0) {
			/* find which write format the chip accepts, by changing vsel (LDO2 is still off) */
			for (variant = 1; variant <= 2 && good < 0; variant++) {
				i2c_wreg(0x64, 0x15, 0xaa, variant);
				delay_ms(5);
				reg = 0x15;
				if (i2c_read(0x64, &reg, 1, &got, 1) == 0 && got == 0xaa)
					good = variant;
			}
			if (good < 0) {
				i2c_wreg(0x64, 0x15, 0xaa, 0);
				delay_ms(5);
				reg = 0x15;
				if (i2c_read(0x64, &reg, 1, &got, 1) == 0 && got == 0xaa)
					good = 0;
			}
			if (good == 1)
				con_str("write format", "PEC addr+reg+val");
			else if (good == 2)
				con_str("write format", "PEC reg+val");
			else if (good == 0)
				con_str("write format", "plain");
			else
				con_str("write format", "NONE worked");
			if (good >= 0) {
				i2c_wreg(0x64, 0x11, (uint8_t)(en | 0x40), good);
				delay_ms(20);
			}
			reg = 0x11;
			e = i2c_read(0x64, &reg, 1, &v, 1);
			show_byte("LDO2 en now", e, v);
			reg = 0x15;
			e = i2c_read(0x64, &reg, 1, &v, 1);
			show_byte("LDO2 vsel now", e, v);
		}
	}
	con_skip();
	/* back to the touch bus */
	gpio_in(p_scl);
	gpio_in(p_sda);
	p_scl = 51;
	p_sda = 52;
}

static void reset_pulse(int irq_level)
{
	gpio_set_mode(PIN_IRQ, 0);
	gpio_set_mode(PIN_RST, 0);
	gpio_out(PIN_IRQ, (unsigned)irq_level);
	gpio_out(PIN_RST, 0);
	delay_ms(10);
	gpio_out(PIN_RST, 1);
	delay_ms(5);
	gpio_in(PIN_IRQ);
	delay_ms(300);
}

void i2c_stage9(int mask)
{
	char s[40], *p;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
	if (!freq)
		freq = 13000000;

	p = s;
	p = put_hex(p, (uint32_t)freq, 8);
	*p = 0;
	con_str("cntfrq", s);
	for (unsigned i = 0; i < 4; i++) {
		unsigned pin[4];
		pin[0] = PIN_IRQ; pin[1] = PIN_RST; pin[2] = p_scl; pin[3] = p_sda;
		p = s;
		*p++ = 'm'; *p++ = (char)('0' + gpio_mode(pin[i]));
		*p++ = ' '; *p++ = 'd'; *p++ = (char)('0' + gpio_bit(0x000, pin[i]));
		*p++ = ' '; *p++ = 'o'; *p++ = (char)('0' + gpio_bit(0x100, pin[i]));
		*p++ = ' '; *p++ = 'i'; *p++ = (char)('0' + gpio_din(pin[i]));
		*p = 0;
		/* explicit calls: gcc turns a chain of string selects into a table of absolute pointers */
		if (i == 0)
			con_str("gpio1 irq", s);
		else if (i == 1)
			con_str("gpio2 rst", s);
		else if (i == 2)
			con_str("gpio51 scl", s);
		else
			con_str("gpio52 sda", s);
	}
	con_skip();
	/* both pins as plain GPIO: the controller must not drive them */
	gpio_set_mode(p_scl, 0);
	gpio_set_mode(p_sda, 0);
	sda_hi();
	gpio_in(p_scl);
	delay_ms(1);

	if (mask & 8)
		enable_vtp();
	/* the touch bus pins */
	gpio_set_mode(p_scl, 0);
	gpio_set_mode(p_sda, 0);
	sda_hi();
	gpio_in(p_scl);
	selftest();
	con_skip();
	if (mask & 4)
		scan("C no reset");
	if (mask & 1) {
		reset_pulse(0);
		scan("A irq low");
	}
	if (mask & 2) {
		reset_pulse(1);
		scan("B irq high");
	}
	con_status("stage 9 done");
}
