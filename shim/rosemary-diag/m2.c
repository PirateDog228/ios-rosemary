/*
 * Rosemary milestone 2 (rdiag_stage=4/5/6/7): the questions that decide the XNU port.
 *   1. SMP: PSCI CPU_ON for cpu1-7 (MMU still off): which EL, MIDR, 16K granule on each,
 *      in particular the two Cortex-A76 cores (cpu6/7).
 *   2. 16K-granule stage-1 MMU on the boot CPU (identity map, image cacheable).
 *   3. GICv3: virtual-timer PPI (INTID 27) and an SGI actually interrupt the CPU.
 * Port of sweet shim.c; MediaTek addresses from mt6785.dts / the stage 1-2 probes.
 * Nothing is written to storage. Watchdog was disabled by diag.c before we got here.
 */
#include <stdint.h>
#include "console.h"
#include "m2.h"

#define GICD_BASE	0x0c000000UL
#define GICR_BASE	0x0c040000UL
#define GICR_SIZE	0x200000UL
#define GICR_PE_SIZE	0x20000UL
#define GICR_TYPER	0x08
#define GICR_WAKER	0x14
#define GICR_SGI	0x10000
#define GICR_IGROUPR0	(GICR_SGI + 0x080)
#define GICR_ISENABLER0	(GICR_SGI + 0x100)
#define GICR_ICENABLER0	(GICR_SGI + 0x180)
#define GICR_IPRIORITYR	(GICR_SGI + 0x400)
#define GICR_ISPENDR0	(GICR_SGI + 0x200)
#define GICR_ISACTIVER0	(GICR_SGI + 0x300)
#define VTIMER_INTID	27	/* PPI 11 (mt6785.dts arch timer, 3rd interrupt) */
#define PTIMER_INTID	30	/* PPI 14, non-secure physical timer */
#define SGI_INTID	1

#define PSCI_VERSION	0x84000000UL
#define PSCI_CPU_ON	0xc4000003UL
#define PSCI_AFFINITY_INFO	0xc4000004UL
#define NR_CPUS		8

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))
#define isb() __asm__ volatile("isb" ::: "memory")

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline uint64_t rd64(uintptr_t a) { return *(volatile uint64_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void wr8(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }

extern char secondary_entry[];
extern char _head[], _end[];

struct sec_record {
	uint64_t mpidr;
	uint64_t midr;
	uint64_t mmfr0;
	uint64_t el;
	uint32_t entry_seen;
	uint32_t context_id;
	uint32_t reserved[6];
} __attribute__((aligned(64)));

volatile struct sec_record sec_info[NR_CPUS] __attribute__((aligned(64)));
static volatile uint32_t irq_count, irq_other, irq_last;
static volatile int irq_src;

static void dcache_flush_range(uintptr_t start, uintptr_t end);
static void dcache_invalidate_range(uintptr_t start, uintptr_t end);

static uint64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static uint64_t now(void)
{
	isb();
	return rsr("cntpct_el0");
}

static void delay_ms(unsigned ms)
{
	uint64_t end = now() + rsr("cntfrq_el0") / 1000 * ms;
	while (now() < end)
		;
}

/* ---- secondary CPUs (MMU off, own stack from start.S) ---- */

void secondary_main(uint64_t idx)
{
	sec_info[idx].el = rsr("CurrentEL") >> 2;
	sec_info[idx].midr = rsr("midr_el1");
	sec_info[idx].mmfr0 = rsr("id_aa64mmfr0_el1");
	sec_info[idx].mpidr = rsr("mpidr_el1") | (1ULL << 63);
	__asm__ volatile("dsb sy" ::: "memory");
	dcache_flush_range((uintptr_t)&sec_info[idx],
			   (uintptr_t)&sec_info[idx] + sizeof(sec_info[idx]));
	for (;;)
		__asm__ volatile("wfe");
}

/* ---- MMU: 16K granule, identity, T0SZ=28 (36-bit VA), walk starts at L2 ---- */

#define PG_SHIFT	14
#define PG_SIZE		(1UL << PG_SHIFT)
#define BLK_SHIFT	25
#define BLK_SIZE	(1UL << BLK_SHIFT)
#define NENT		2048
#define MAIR_VAL	0x44ff00UL	/* attr0 Device-nGnRnE, attr1 Normal WB, attr2 Normal NC */
#define D_BLOCK		1UL
#define D_TABLE		3UL
#define D_PAGE		3UL
#define D_ATTR(i)	((uint64_t)(i) << 2)
#define D_SH_OUTER	(2UL << 8)
#define D_SH_INNER	(3UL << 8)
#define D_AF		(1UL << 10)
#define D_PXN		(1UL << 53)
#define D_UXN		(1UL << 54)
#define DEV_FLAGS	(D_ATTR(0) | D_AF | D_PXN | D_UXN)
#define WB_FLAGS	(D_ATTR(1) | D_SH_INNER | D_AF | D_UXN)
#define NC_FLAGS	(D_ATTR(2) | D_SH_OUTER | D_AF | D_PXN | D_UXN)
#define TCR_VAL		(28UL | (1UL << 8) | (1UL << 10) | (3UL << 12) | (2UL << 14) | \
			 (28UL << 16) | (1UL << 23) | (1UL << 30) | (1UL << 32))

static uint64_t pt_pool[4 * NENT] __attribute__((aligned(4096)));

static void dcache_flush_range(uintptr_t start, uintptr_t end)
{
	uint64_t line = 4UL << ((rsr("ctr_el0") >> 16) & 0xf);

	/* clean+invalidate: safe whether LK left the lines dirty or not */
	for (uintptr_t a = start & ~(line - 1); a < end; a += line)
		__asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

static void dcache_invalidate_range(uintptr_t start, uintptr_t end)
{
	uint64_t line = 4UL << ((rsr("ctr_el0") >> 16) & 0xf);

	for (uintptr_t a = start & ~(line - 1); a < end; a += line)
		__asm__ volatile("dc ivac, %0" : : "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

static int mmu_enable(uint64_t fb, uint64_t fb_size, uint64_t *ttbr_out, uint64_t *sctlr_out)
{
	uintptr_t img_lo = (uintptr_t)_head & ~(PG_SIZE - 1);
	uintptr_t img_hi = ((uintptr_t)_end + PG_SIZE - 1) & ~(PG_SIZE - 1);
	uintptr_t pool = ((uintptr_t)pt_pool + PG_SIZE - 1) & ~(PG_SIZE - 1);
	uint64_t *l2 = (uint64_t *)pool;
	int n_l3 = 0;

	*ttbr_out = pool;
	if (((img_hi - 1) >> BLK_SHIFT) - (img_lo >> BLK_SHIFT) > 1)
		return -1;
	for (int i = 0; i < NENT; i++)
		l2[i] = 0;
	/* Device everywhere RAM or MMIO can be (first 8 GB; DRAM is 0x40000000..0x1c0000000) */
	for (uint64_t i = 0; i < (8UL << 30) >> BLK_SHIFT; i++)
		l2[i] = (i << BLK_SHIFT) | DEV_FLAGS | D_BLOCK;
	/* framebuffer: Normal non-cacheable (write combining) */
	for (uint64_t b = fb >> BLK_SHIFT; b <= (fb + fb_size - 1) >> BLK_SHIFT; b++)
		l2[b] = (b << BLK_SHIFT) | NC_FLAGS | D_BLOCK;
	for (uint64_t blk = img_lo >> BLK_SHIFT; blk <= (img_hi - 1) >> BLK_SHIFT; blk++) {
		uint64_t *l3 = (uint64_t *)(pool + (1 + n_l3++) * PG_SIZE);
		uint64_t base = blk << BLK_SHIFT;
		for (int i = 0; i < NENT; i++) {
			uint64_t pa = base + ((uint64_t)i << PG_SHIFT);
			int ours = pa >= img_lo && pa < img_hi;
			l3[i] = pa | (ours ? WB_FLAGS : DEV_FLAGS) | D_PAGE;
		}
		l2[blk] = (uintptr_t)l3 | D_TABLE;
	}
	/* page tables were written with the MMU off (uncached): make sure nothing stale is cached */
	dcache_flush_range(img_lo, img_hi);
	__asm__ volatile("ic iallu; dsb sy; isb" ::: "memory");

	wsr("mair_el1", MAIR_VAL);
	wsr("tcr_el1", TCR_VAL);
	wsr("ttbr0_el1", pool);
	__asm__ volatile("isb; tlbi vmalle1; dsb nsh; isb" ::: "memory");
	uint64_t sctlr = rsr("sctlr_el1");
	sctlr &= ~((1UL << 1) | (1UL << 19));		/* A, WXN off */
	sctlr |= (1UL << 0) | (1UL << 2) | (1UL << 12);	/* M, C, I */
	wsr("sctlr_el1", sctlr);
	isb();
	*sctlr_out = rsr("sctlr_el1");
	return 0;
}

static uint64_t mmu_probe(uintptr_t va)
{
	__asm__ volatile("at s1e1r, %0; isb" : : "r"(va) : "memory");
	return rsr("par_el1");
}

/* ---- GICv3 + timer ---- */

static uint64_t sgi_self(unsigned id)
{
	uint64_t m = rsr("mpidr_el1");
	return ((m >> 16) & 0xff) << 32 | ((m >> 8) & 0xff) << 16 | (uint64_t)id << 24 |
	       (1U << (m & 0xf));
}

void m2_irq(void)
{
	uint64_t iar = rsr("icc_iar1_el1");
	uint32_t id = iar & 0xffffff;

	if (id >= 1020 && id <= 1023)
		return;
	irq_last = id;
	if (id == (uint32_t)irq_src)
		irq_count++;
	else
		irq_other++;
	if (id == VTIMER_INTID)
		wsr("cntv_tval_el0", rsr("cntfrq_el0") / 100);
	else if (id == PTIMER_INTID)
		wsr("cntp_tval_el0", rsr("cntfrq_el0") / 100);
	wsr("icc_eoir1_el1", iar);
	isb();
}

static uintptr_t find_gicr(void)
{
	uint64_t aff = rsr("mpidr_el1");
	uint32_t want = (aff & 0xffffff) | ((aff >> 8) & 0xff000000);

	for (uintptr_t off = 0; off < GICR_SIZE; off += GICR_PE_SIZE) {
		uint64_t typer = rd64(GICR_BASE + off + GICR_TYPER);
		if ((uint32_t)(typer >> 32) == want)
			return GICR_BASE + off;
		if (typer & (1 << 4))
			break;
	}
	return 0;
}

static uint32_t irq_window(const char *label, uintptr_t r, int src)
{
	uint64_t f = rsr("cntfrq_el0");
	uint32_t pend, act, isr, hppi;

	irq_count = irq_other = 0;
	irq_last = 0x3ff;
	irq_src = src;
	if (src == VTIMER_INTID) {
		wsr("cntv_tval_el0", f / 100);
		wsr("cntv_ctl_el0", 1);
	}
	isb();
	uint64_t t = now() + f / 50;
	while (now() < t)
		;
	if (src == SGI_INTID)
		wsr("icc_sgi1r_el1", sgi_self(SGI_INTID));
	isb();
	pend = rd32(r + GICR_ISPENDR0);
	act = rd32(r + GICR_ISACTIVER0);
	isr = rsr("isr_el1");
	hppi = rsr("icc_hppir1_el1");

	__asm__ volatile("msr daifclr, #2" ::: "memory");
	t = now() + f * 3 / 10;
	uint64_t next = now() + f / 100;
	for (int sent = 1; now() < t;)
		if (src == SGI_INTID && sent < 10 && now() >= next) {
			wsr("icc_sgi1r_el1", sgi_self(SGI_INTID));
			isb();
			sent++;
			next += f / 100;
		}
	__asm__ volatile("msr daifset, #2" ::: "memory");
	wsr("cntv_ctl_el0", 0);
	isb();
	con_hex3(label, "irqs=", irq_count, "other=", irq_other, "last=", irq_last);
	con_hex3(" pre", "pend=", pend, "act=", act, "hppi=", hppi);
	(void)isr;
	return irq_count;
}

static int test_gic(void)
{
	uint64_t sre = rsr("icc_sre_el1");
	if (!(sre & 1)) {
		wsr("icc_sre_el1", sre | 1);
		isb();
	}
	if (!(rsr("icc_sre_el1") & 1)) {
		con_str("GIC", "no sysreg interface");
		return 0;
	}
	uint32_t dctlr = rd32(GICD_BASE);
	wr32(GICD_BASE, dctlr | 0x13);
	for (int i = 0; i < 1000000 && (rd32(GICD_BASE) & (1U << 31)); i++)
		;
	con_hex3("GICD", "was=", dctlr, "now=", rd32(GICD_BASE), "sre=", (uint32_t)sre);
	uintptr_t r = find_gicr();
	if (!r) {
		con_str("GICR", "not found");
		return 0;
	}
	uint32_t waker = rd32(r + GICR_WAKER);
	if (waker & 2) {
		wr32(r + GICR_WAKER, waker & ~2U);
		for (int i = 0; i < 100000 && (rd32(r + GICR_WAKER) & 4); i++)
			;
	}
	con_hex3("GICR", "base=", (uint32_t)r, "waker=", rd32(r + GICR_WAKER), "", 0);
	wr32(r + GICR_IGROUPR0, 0xffffffff);
	for (int id = 0; id < 32; id++)
		wr8(r + GICR_IPRIORITYR + id, 0xa0);
	wr32(r + GICR_ISENABLER0, (1U << VTIMER_INTID) | (1U << SGI_INTID));
	uint64_t ctlr = rsr("icc_ctlr_el1");
	wsr("icc_ctlr_el1", ctlr & ~2ULL);
	wsr("icc_pmr_el1", 0xf0);
	wsr("icc_bpr1_el1", 0);
	wsr("icc_igrpen1_el1", 1);
	isb();
	uint32_t v = irq_window("VTIMER", r, VTIMER_INTID);
	uint32_t s = irq_window("SGI1", r, SGI_INTID);
	wr32(r + GICR_ICENABLER0, 0xffffffff);
	isb();
	return v > 5 && s > 5;
}

/* ---- entry ---- */

void m2_run(uint64_t fb, uint64_t fb_size, const uint64_t *cpu_mpidrs, int ncpu)
{
	con_color_fg(C_WHITE);
	con_str("M2", "rosemary milestone 2");
	con_hex("SMP entry", (uintptr_t)secondary_entry);

	/* 1. SMP, MMU still off so shared arrays are coherent */
	int64_t ret[NR_CPUS] = { 0 };
	uint64_t boot_mpidr = rsr("mpidr_el1") & 0x000000ff00ffffffULL;
	int boot_index = -1;
	if (ncpu > NR_CPUS)
		ncpu = NR_CPUS;
	for (int i = 0; i < ncpu; i++)
		if ((cpu_mpidrs[i] & 0x000000ff00ffffffULL) == boot_mpidr) {
			boot_index = i;
			break;
		}
	if (boot_index < 0) {
		con_str("SMP", "boot CPU absent from DTB");
		ncpu = 0;
	}
	/* Drop any primary-side cache copies before the secondaries publish results. */
	dcache_flush_range((uintptr_t)sec_info,
			   (uintptr_t)(sec_info + NR_CPUS));
	for (int i = 0; i < ncpu; i++) {
		if (i == boot_index)
			continue;
		uint64_t target = cpu_mpidrs[i] & 0x000000ff00ffffffULL;
		ret[i] = (int64_t)smc(PSCI_CPU_ON, target, (uintptr_t)secondary_entry, i);
		delay_ms(100);
	}
	delay_ms(1000);
	int smp_ok = 0;
	for (int i = 0; i < ncpu; i++) {
		if (i == boot_index)
			continue;
		dcache_invalidate_range((uintptr_t)&sec_info[i],
					(uintptr_t)&sec_info[i] + sizeof(sec_info[i]));
		char label[] = "CPU0";
		label[3] = '0' + i;
		uint64_t seen = sec_info[i].mpidr & ~(1ULL << 63);
		uint32_t tgran16 = (uint32_t)((sec_info[i].mmfr0 >> 20) & 0xf);
		uint64_t target = cpu_mpidrs[i] & 0x000000ff00ffffffULL;
		uint32_t affinity = (uint32_t)smc(PSCI_AFFINITY_INFO, target, 0, 0);
		con_hex3(label, "ret=", (uint32_t)ret[i], "want=", (uint32_t)cpu_mpidrs[i], "seen=", (uint32_t)seen);
		con_hex3("  PSCI", "aff=", affinity, "entry=", sec_info[i].entry_seen,
			 "ctx=", sec_info[i].context_id);
		con_hex3("  ID", "midr=", (uint32_t)sec_info[i].midr, "EL=", (uint32_t)sec_info[i].el, "T16=", tgran16);
		if (sec_info[i].mpidr)
			smp_ok++;
	}
	con_hex2("SMP", "started=", smp_ok, "cpu-nodes=", ncpu);
	con_hex3("PSCI", "ver=", (uint32_t)smc(PSCI_VERSION, 0, 0, 0), "on_feat=", (uint32_t)smc(0x8400000aUL, PSCI_CPU_ON, 0, 0), "", 0);

	/* The MMU test below programs EL1 translation registers. Never report it as
	 * successful if LK entered at EL2 and no EL2-to-EL1 transition was done. */
	if ((rsr("CurrentEL") >> 2) != 1) {
		con_str("MMU", "SKIPPED: entry is not EL1");
		con_str("RESULT", "partial; EL1 handoff needed");
		return;
	}

	/* 2. MMU */
	uint64_t ttbr = 0, sctlr = 0;
	uint64_t t0 = now();
	con_fill_bottom();
	uint64_t fill_off = now() - t0;
	int mrc = mmu_enable(fb, fb_size, &ttbr, &sctlr);
	t0 = now();
	con_fill_bottom();
	uint64_t fill_on = now() - t0;
	con_hex3("MMU", "rc=", (uint32_t)mrc, "sctlr=", (uint32_t)sctlr, "ttbr=", (uint32_t)ttbr);
	con_hex3("PAR", "img=", (uint32_t)mmu_probe((uintptr_t)m2_run), "fb=", (uint32_t)mmu_probe(fb + 0x1000),
		 "gic=", (uint32_t)mmu_probe(GICD_BASE));
	con_hex3("FILL", "off=", (uint32_t)(fill_off * 1000 / (rsr("cntfrq_el0") / 1000)),
		 "on=", (uint32_t)(fill_on * 1000 / (rsr("cntfrq_el0") / 1000)), "us", 0);
	con_hex("TCR", rsr("tcr_el1"));

	/* 3. GIC + timer IRQ */
	int gic_ok = mrc ? 0 : test_gic();

	con_skip();
	/* summary re-drawn compactly below */
	con_str("RESULT", "");
	con_hex3("SUM", "smp=", smp_ok, "mmu=", mrc == 0, "gic=", gic_ok);
}
