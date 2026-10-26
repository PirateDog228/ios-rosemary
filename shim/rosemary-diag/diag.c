/*
 * Rosemary (Redmi Note 10S, MT6785) read-only diagnostic.
 * No XNU and no storage writes. The diagnostic disables LK's watchdog, and
 * stage 4 deliberately exercises PSCI, the MMU, and GIC registers.
 *
 * Stage comes from the kernel cmdline (LK copies it into /chosen/bootargs):
 *   rdiag_stage=0  CPU + DTB (watchdog handoff only)   default
 *   rdiag_stage=1  + GIC, watchdog, UART0, timer tick
 *   rdiag_stage=2  + display pipe (mmsys/OVL/RDMA/DSI) and eMMC (MSDC0)
 *   rdiag_stage=3  + USB (xHCI/mtu3), keypad, PWRAP   (may be clock-gated)
 *   rdiag_stage=4/5 PSCI SMP, 16K MMU, and GIC timer/SGI checks
 *   rdiag_stage=8  UFS host probe (ufshci@11270000), read-only unless rdiag_ufsinit=1
 *   rdiag_stage=9  watchdog timing probe: idle 40 s, then reset (no drawing)
 *   rdiag_stage=10 touch bus scan by bit-banging GPIO51/52 (rdiag_tp=<mask>, default 15)
 * Optional: rdiag_stride=<bytes> rdiag_rgba=1 rdiag_scale=<1..4>
 * Every MMIO line is drawn BEFORE the read, so on a hang the last label on
 * screen names the culprit.
 */
#include <stdint.h>
#include "console.h"
#include "fdt.h"
#include "m2.h"
#include "ufs_mtk.h"
#include "i2c_diag.h"

#define SYSREG(reg) ({ uint64_t v; __asm__ volatile("mrs %0, " #reg : "=r"(v)); v; })


static const char *cur_probe;	/* set at runtime: no absolute relocation */

static void halt_forever(void)
{
	for (;;)
		__asm__ volatile("wfe");
}

/* ---- tiny string/number helpers ---- */
static int scmp(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return (unsigned char)*a - (unsigned char)*b;
}

static const char *find_arg(const char *s, const char *key)
{
	int kl = 0;
	while (key[kl])
		kl++;
	for (const char *start = s; s && *s; s++) {
		if (s != start && s[-1] != ' ')
			continue;
		int i = 0;
		while (i < kl && s[i] == key[i])
			i++;
		if (i == kl)
			return s + kl;
	}
	return 0;
}

static uint64_t parse_num(const char *s, uint64_t def)
{
	if (!s)
		return def;
	uint64_t v = 0;
	int base = 10;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		base = 16;
		s += 2;
	}
	int any = 0;
	for (;; s++) {
		int d;
		if (*s >= '0' && *s <= '9')
			d = *s - '0';
		else if (base == 16 && *s >= 'a' && *s <= 'f')
			d = *s - 'a' + 10;
		else if (base == 16 && *s >= 'A' && *s <= 'F')
			d = *s - 'A' + 10;
		else
			break;
		v = v * base + d;
		any = 1;
	}
	return any ? v : def;
}

static uint32_t le32(const uint8_t *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- DTB scan ---- */
#define MAXMEM 8
#define MAXRSV 48
struct rsv { char name[24]; uint64_t base, size; };
struct dtinfo {
	char model[40], compat[64], bootargs[1100], lcm[48];
	uint64_t mem_base[MAXMEM], mem_size[MAXMEM]; int nmem;
	struct rsv rsv[MAXRSV]; int nrsv;
	uint64_t fb_lo, fb_hi, vram; int have_fb, lcm_found, lcm_inited, fps;
	uint64_t cpu_reg[16]; int ncpu;
	int addr_cells, size_cells;
};

static void copy_str(char *d, const uint8_t *v, uint32_t len, int max)
{
	int i = 0;
	for (; i < max - 1 && i < (int)len && v[i]; i++)
		d[i] = (char)v[i];
	d[i] = 0;
}

static int dt_cb(void *c, int depth, const char **n, const char *prop,
		 const uint8_t *v, uint32_t len)
{
	struct dtinfo *d = c;
	if (!prop) {
		if (depth == 3 && fdt_name_is(n[1], "reserved-memory") && d->nrsv < MAXRSV) {
			copy_str(d->rsv[d->nrsv].name, (const uint8_t *)n[2], 23, 24);
			d->rsv[d->nrsv].base = d->rsv[d->nrsv].size = 0;
			d->nrsv++;
		}
		return 0;
	}
	if (depth == 1) {
		if (!scmp(prop, "model"))
			copy_str(d->model, v, len, sizeof d->model);
		else if (!scmp(prop, "compatible"))
			copy_str(d->compat, v, len, sizeof d->compat);
		else if (!scmp(prop, "#address-cells"))
			d->addr_cells = fdt32(v);
		else if (!scmp(prop, "#size-cells"))
			d->size_cells = fdt32(v);
	} else if (depth == 2 && fdt_name_is(n[1], "chosen")) {
		/*
		 * LK fixes up ONLY the raw "atag,videolfb" tag. The split
		 * "atag,videolfb-*" props are stale DTS placeholders (fb_base_l
		 * 0x7e605000 on this phone), so they are used only if no raw tag
		 * exists (have_fb bit 1 = split seen, bit 2 = raw seen).
		 */
		int split = !(d->have_fb & 2);
		if (!scmp(prop, "bootargs")) {
			copy_str(d->bootargs, v, len, sizeof d->bootargs);
		} else if (!scmp(prop, "atag,videolfb") && len >= 21) {
			/* native little-endian: u64 fb_base, u32 islcmfound, u32 fps,
			 * u32 vram, char lcmname[] */
			d->fb_lo = le32(v);
			d->fb_hi = le32(v + 4);
			d->lcm_found = le32(v + 8);
			d->fps = le32(v + 12);
			d->vram = le32(v + 16);
			d->lcm_inited = 1;
			copy_str(d->lcm, v + 20, len - 20, sizeof d->lcm);
			d->have_fb |= 2;
		} else if (split && !scmp(prop, "atag,videolfb-fb_base_l")) {
			d->fb_lo = fdt32(v);
			d->have_fb |= 1;
		} else if (split && !scmp(prop, "atag,videolfb-fb_base_h")) {
			d->fb_hi = fdt32(v);
		} else if (split && !scmp(prop, "atag,videolfb-vramSize")) {
			d->vram = fdt32(v);
		} else if (split && !scmp(prop, "atag,videolfb-islcmfound")) {
			d->lcm_found = fdt32(v);
		} else if (split && !scmp(prop, "atag,videolfb-islcm_inited")) {
			d->lcm_inited = fdt32(v);
		} else if (split && !scmp(prop, "atag,videolfb-fps")) {
			d->fps = fdt32(v);
		} else if (split && !scmp(prop, "atag,videolfb-lcmname")) {
			copy_str(d->lcm, v, len, sizeof d->lcm);
		}
	} else if (depth == 2 && fdt_name_is(n[1], "memory") && !scmp(prop, "reg")) {
		int ac = d->addr_cells ? d->addr_cells : 2, sc = d->size_cells ? d->size_cells : 2;
		uint32_t stride = (ac + sc) * 4;
		for (uint32_t o = 0; o + stride <= len && d->nmem < MAXMEM; o += stride) {
			d->mem_base[d->nmem] = ac == 2 ? fdt64(v + o) : fdt32(v + o);
			d->mem_size[d->nmem] = sc == 2 ? fdt64(v + o + ac * 4) : fdt32(v + o + ac * 4);
			d->nmem++;
		}
	} else if (depth == 3 && fdt_name_is(n[1], "reserved-memory") && d->nrsv &&
		   !scmp(prop, "reg") && len >= 16) {
		d->rsv[d->nrsv - 1].base = fdt64(v);
		d->rsv[d->nrsv - 1].size = fdt64(v + 8);
	} else if (depth == 3 && fdt_name_is(n[1], "cpus") && fdt_name_is(n[2], "cpu") &&
		   !scmp(prop, "reg") && d->ncpu < 16) {
		d->cpu_reg[d->ncpu++] = len >= 8 ? fdt64(v + len - 8) : fdt32(v + len - 4);
	}
	return 0;
}

/* ---- read-only MMIO probe ---- */
static void probe(const char *label, uint64_t addr)
{
	cur_probe = label;
	con_begin(label);
	uint32_t v = *(volatile uint32_t *)addr;
	__asm__ volatile("dsb sy; isb" ::: "memory");	/* surface aborts here */
	con_end_hex32(v);
}

/* names are inline arrays: no pointers in tables, so no absolute relocations */
struct pr { char name[16]; uint64_t addr; };

static void run_probes(const struct pr *p, int n)
{
	for (int i = 0; i < n; i++)
		probe(p[i].name, p[i].addr);
}

static const struct pr st1[] = {
	{ "GICD_CTLR", 0x0c000000 }, { "GICD_TYPER", 0x0c000004 },
	{ "GICD_IIDR", 0x0c000008 }, { "GICD_PIDR2", 0x0c00ffe8 },
	{ "GICR0_IIDR", 0x0c040004 }, { "GICR0_TYPERL", 0x0c040008 },
	{ "GICR0_TYPERH", 0x0c04000c }, { "GICR0_WAKER", 0x0c040014 },
	{ "WDT_MODE", 0x10007000 }, { "WDT_LENGTH", 0x10007004 },
	{ "UART0_LSR", 0x11002014 }, { "UART0_LCR", 0x1100200c },
};
static const struct pr st2[] = {
	{ "MMSYS_CG0", 0x14000100 }, { "MMSYS_CG1", 0x14000110 },
	{ "OVL0_EN", 0x14008000 + 0x00c }, { "OVL0_L0CON", 0x14008000 + 0x020 },
	{ "OVL0_L0PITCH", 0x14008000 + 0x044 }, { "OVL0_L0ADDR", 0x14008000 + 0xf40 },
	{ "OVL2L_L0CON", 0x14009000 + 0x020 }, { "OVL2L_L0PITCH", 0x14009000 + 0x044 },
	{ "OVL2L_L0ADDR", 0x14009000 + 0xf40 },
	{ "RDMA0_W", 0x1400b000 + 0x018 }, { "RDMA0_H", 0x1400b000 + 0x01c },
	{ "RDMA0_ADDR", 0x1400b000 + 0xf00 }, { "RDMA0_PITCH", 0x1400b000 + 0xf08 },
	{ "DSI0_START", 0x14014000 + 0x000 }, { "DSI0_SIZE", 0x14014000 + 0x03c },
	{ "MSDC0_CFG", 0x11230000 }, { "MSDC0_IOCON", 0x11230004 },
	{ "MSDC0_PS", 0x11230008 }, { "MSDC0_INTEN", 0x11230010 },
	{ "MSDC0_SDCCFG", 0x11230030 }, { "MSDC0_SDCSTS", 0x11230034 },
	{ "MSDC0_TOP_CTRL", 0x11f50000 },
};
static const struct pr st3[] = {
	{ "XHCI_CAPLEN", 0x11200000 }, { "MTU3_MAC0", 0x11201000 },
	{ "KPD_STATUS", 0x10010000 }, { "PWRAP_WACS", 0x1000d000 },
};

#define TOPRGU_MODE    0x10007000UL
#define TOPRGU_RESTART 0x10007008UL

/*
 * LK arms the MediaTek watchdog and expects the kernel to take it over.
 * We never kick it, so without this the phone resets after a few seconds
 * and looks like a boot loop. Same sequence as Linux mtk_wdt: key 0x22
 * in the top byte, EN (bit 0) cleared; then one restart (0x1971).
 */
static uint32_t wdt_disable(void)
{
	uint32_t before = *(volatile uint32_t *)TOPRGU_MODE;
	*(volatile uint32_t *)TOPRGU_MODE = 0x22000000;
	*(volatile uint32_t *)TOPRGU_RESTART = 0x1971;
	__asm__ volatile("dsb sy; isb" ::: "memory");
	return before;
}

void shim_main(uint64_t dtb)
{
	static struct dtinfo d;
#if defined(PROBE) && PROBE == 1
	/* Probe 1: reset the SoC at once (TOPRGU WDT_SWRST). Quick reboot => code runs. */
	*(volatile uint32_t *)0x10007014UL = 0x1209;
	__asm__ volatile("dsb sy; isb" ::: "memory");
	for (;;)
		__asm__ volatile("wfe");
#elif defined(PROBE) && PROBE == 2
	/* Probe 2: only disable the watchdog, then idle. Stays on => disable works. */
	(void)wdt_disable();
	for (;;)
		__asm__ volatile("wfe");
#endif
	uint32_t wdt_before = wdt_disable();
	uint64_t mmfr0 = SYSREG(id_aa64mmfr0_el1);
	int ok = fdt_walk((uintptr_t)dtb, dt_cb, &d);
	const char *ba = d.bootargs;

	int stage = (int)parse_num(find_arg(ba, "rdiag_stage="), 0);
	if (stage == 9) {
		/*
		 * Timing probe, no drawing: watchdog already disabled above. Idle
		 * 40 s, then reset the SoC ourselves (TOPRGU WDT_SWRST, key 0x1209).
		 * Phone reboots after ~40 s  => this code runs and the WDT disable works.
		 * Reboots after 10-15 s      => not our code (image rejected) or the
		 *                               watchdog could not be disabled.
		 * Never reboots              => SWRST ineffective (code still runs).
		 */
		uint64_t f = SYSREG(cntfrq_el0);
		if (!f)
			f = 13000000;
		uint64_t t0 = SYSREG(cntpct_el0);
		while (SYSREG(cntpct_el0) - t0 < 40 * f)
			;
		*(volatile uint32_t *)0x10007014UL = 0x1209;
		__asm__ volatile("dsb sy; isb" ::: "memory");
		for (;;)
			__asm__ volatile("wfe");
	}
	uint64_t fb = (d.fb_hi << 32) | d.fb_lo;
	/*
	 * LK carves the framebuffer off the TOP of the memory node (DTS placeholder:
	 * memory ends at 0x7e605000 = fb_base), so the fb is normally OUTSIDE the
	 * reported RAM ranges. Trust the LK-provided value if it is a plausible
	 * physical address: 4 KiB aligned, inside the low DRAM window below 4 GiB.
	 */
#define FB_BYTES (1080ULL * 2400 * 4)
#define FB_PLAUSIBLE(a) ((a) >= 0x40000000ULL && ((a) & 0xfff) == 0 && (a) + FB_BYTES <= 0x100000000ULL)
	int fb_ok = d.have_fb && FB_PLAUSIBLE(fb);
	int fb_src = d.have_fb;	/* 1 split atags, 2 raw atag, 0 none */
	if (!fb_ok) {
		/*
		 * Fallback: ask the display pipe what it scans out (LK's logo is on
		 * screen, so these blocks are clocked). Accept only addresses inside
		 * the RAM LK reported.
		 */
		static const uint64_t regs[] = { 0x14008000UL + 0xf40, 0x14009000UL + 0xf40,
						 0x1400b000UL + 0xf00 };
		for (int i = 0; i < 3 && !fb_ok; i++) {
			uint64_t a = *(volatile uint32_t *)regs[i];
			if (FB_PLAUSIBLE(a)) {
				fb = a;
				fb_ok = 1;
				fb_src = 10 + i;
			}
		}
	}
	if (!fb_ok) {
		/* Never scribble 10 MB over an unverified address: just wait. */
		for (;;)
			__asm__ volatile("wfe");
	}
	uint32_t stride = (uint32_t)parse_num(find_arg(ba, "rdiag_stride="), 1088 * 4);
	int rgba = (int)parse_num(find_arg(ba, "rdiag_rgba="), 0);
	int scale = (int)parse_num(find_arg(ba, "rdiag_scale="), 2);

	con_init(fb, 1080, 2400, stride, rgba, scale);
	con_clear(C_BLUE);
	con_str("ROSEMARY", "CPU/DTB diagnostic (no storage writes)");
	/* Keep a live counter near the top; the DTB/reserved-memory dump can be
	 * taller than the panel at the default text scale. */
	con_heartbeat(0);
	con_status("reading hardware and DTB");
	con_skip();
	con_hex("STAGE", stage);
	con_hex("WDT_MODE", wdt_before);	/* value LK left; shim has disabled it */
	con_hex("FB", fb);
	con_hex("FB SRC", fb_src);	/* 1/2 DTB atag, 10 OVL0, 11 OVL0_2L, 12 RDMA0 */
	con_hex2("FBGEOM", "stride=", stride, "rgba=", rgba);
	/* colour check: names are drawn in their own colour */
	con_color_fg(C_RED);   con_str("COLOR", "RED");
	con_color_fg(C_GREEN); con_str("COLOR", "GREEN");
	con_color_fg(C_CYAN);  con_str("COLOR", "CYAN");
	con_color_fg(C_WHITE);
	con_hex("DTB", dtb);
	con_str("DTB OK", ok ? "yes" : "NO (not an FDT)");
	if (ok)
		con_hex("DTB SIZE", fdt_totalsize((uintptr_t)dtb));
	con_hex("CurrentEL", SYSREG(CurrentEL));
	con_hex("MIDR_EL1", SYSREG(midr_el1));
	con_hex("MPIDR_EL1", SYSREG(mpidr_el1));
	con_hex("MMFR0", mmfr0);
	con_hex2("PARange", "", (uint32_t)(mmfr0 & 0xf), "TGran16=", (uint32_t)((mmfr0 >> 20) & 0xf));
	con_hex("ISAR0", SYSREG(id_aa64isar0_el1));
	con_hex("PFR0", SYSREG(id_aa64pfr0_el1));
	con_hex("CNTFRQ", SYSREG(cntfrq_el0));
	con_hex("SCTLR_EL1", SYSREG(sctlr_el1));
	con_hex("MMFR1", SYSREG(id_aa64mmfr1_el1));
	con_hex("MMFR2", SYSREG(id_aa64mmfr2_el1));
	con_hex("CTR_EL0", SYSREG(ctr_el0));
	con_hex("PFR1", SYSREG(id_aa64pfr1_el1));
	con_wrap("MODEL", d.model);
	con_wrap("COMPAT", d.compat);
	{
		int bl = 0;
		while (ba[bl])
			bl++;
		con_hex("ARGS LEN", bl);
		char head[121];
		int i = 0;
		for (; i < 120 && ba[i]; i++)
			head[i] = ba[i];
		head[i] = 0;
		con_wrap("ARGS HEAD", head);
		con_wrap("ARGS TAIL", bl > 120 ? ba + (bl > 240 ? bl - 120 : 120) : "");
	}
	for (int i = 0; i < d.nmem; i++) {
		con_hex("MEM base", d.mem_base[i]);
		con_hex("MEM size", d.mem_size[i]);
	}
	con_hex2("LCM", "found=", d.lcm_found, "inited=", d.lcm_inited);
	con_hex2("LCM", "fps=", d.fps, "vram=", (uint32_t)d.vram);
	con_wrap("LCMNAME", d.lcm);
	for (int i = 0; i < d.ncpu; i++)
		con_hex("CPU reg", d.cpu_reg[i]);
	for (int i = 0; i < d.nrsv && i < 30; i++) {
		con_hex2(d.rsv[i].name, "", (uint32_t)d.rsv[i].base, "sz=", (uint32_t)d.rsv[i].size);
	}

	if (stage == 8) {
		/* UFS probe on a clean page (storage is UFS, not eMMC) */
		con_clear(C_BLUE);
		con_color_fg(C_WHITE);
		con_hex("STAGE", stage);
		con_heartbeat(0);
		con_status("UFS probe");
		con_skip();
		__asm__ volatile("msr daifclr, #4");	/* unmask SError: report aborts */
		ufs_stage8((int)parse_num(find_arg(ba, "rdiag_ufsinit="), 0),
			   (int)parse_num(find_arg(ba, "rdiag_ufslun="), 2));
		goto finished;
	}
	if (stage == 10) {
		/* touch bus scan, controller bypassed */
		con_clear(C_BLUE);
		con_color_fg(C_WHITE);
		con_hex("STAGE", stage);
		con_heartbeat(0);
		con_status("touch bus scan");
		con_skip();
		__asm__ volatile("msr daifclr, #4");
		i2c_stage9((int)parse_num(find_arg(ba, "rdiag_tp="), 15));
		goto finished;
	}
	if (stage >= 1) {
		/* the stage-0 info filled the whole screen: start a clean page */
		con_clear(C_BLUE);
		con_color_fg(C_WHITE);
		con_hex("PROBES", stage);
		con_heartbeat(0);
		con_status("running hardware probes");
		con_skip();
		con_hex("WDT_MODE", wdt_before);
		con_hex("CurrentEL", SYSREG(CurrentEL));
		__asm__ volatile("msr daifclr, #4");	/* unmask SError: report aborts */
		uint64_t t0 = SYSREG(cntpct_el0);
		con_hex("CNTPCT", t0);
		if (stage == 4 || stage == 5 || stage == 6 || stage == 7) {
			m2_run(fb, d.vram ? d.vram : 1080ULL * 2400 * 4, d.cpu_reg, d.ncpu);
			goto finished;
		}
		run_probes(st1, sizeof st1 / sizeof *st1);
		con_hex("CNTPCT+", SYSREG(cntpct_el0) - t0);
	}
	if (stage >= 2)
		run_probes(st2, sizeof st2 / sizeof *st2);
	if (stage >= 3)
		run_probes(st3, sizeof st3 / sizeof *st3);

finished:
	con_status("complete; counter should keep rising");
	/* heartbeat: proves the CPU and generic timer still run */
	uint64_t freq = SYSREG(cntfrq_el0);
	if (!freq)
		freq = 13000000;
	uint64_t start = SYSREG(cntpct_el0), last = ~0ULL;
	for (;;) {
		uint64_t s = (SYSREG(cntpct_el0) - start) / freq;
		if (s != last) {
			con_heartbeat(s);
			last = s;
		}
	}
}

/* Report where we died without wiping what was already on screen. */
void exc_handler(uint64_t slot, uint64_t *regs)
{
	(void)regs;
	if (slot == 5) {	/* IRQ, current EL, SPx: only enabled by stage 4 */
		m2_irq();
		return;
	}
	uint64_t el = SYSREG(CurrentEL) >> 2, esr, far, elr;
	if (el == 2) {
		esr = SYSREG(esr_el2); far = SYSREG(far_el2); elr = SYSREG(elr_el2);
	} else {
		esr = SYSREG(esr_el1); far = SYSREG(far_el1); elr = SYSREG(elr_el1);
	}
	con_skip();
	con_color_fg(C_RED);
	con_str("EXCEPTION", cur_probe ? cur_probe : "(none)");
	con_hex("SLOT", slot);	/* 0-3 cur SP0, 4-7 cur SPx (sync,irq,fiq,serr) */
	con_hex("ESR", esr);
	con_hex("FAR", far);
	con_hex("ELR", elr);
	halt_forever();
}
