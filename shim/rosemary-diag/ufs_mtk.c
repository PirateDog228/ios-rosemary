/*
 * Rosemary (Redmi Note 10S, MT6785) stage 8: UFS host probe.
 *
 * The phone's storage is UFS (live /proc/iomem: ufshci@11270000; partitions
 * are sda/sdb/sdc, userdata = sdc61), NOT eMMC. This stage answers one
 * question: can our bare-metal code talk to the UFS that LK booted from?
 *
 * Default mode is READ-ONLY: it dumps the UFSHCI registers LK left behind and,
 * if the host is enabled, runs NOP OUT, INQUIRY, TEST UNIT READY, READ
 * CAPACITY and READ(10) of the GPT. There is no SCSI WRITE in this file.
 *
 * Opt-in (cmdline rdiag_ufsinit=1): if LK left the host disabled, enable it,
 * run DME_LINKSTARTUP and set fDeviceInit. This writes host registers (and
 * one device flag) but never touches any LBA. Vendor (MediaTek) clock / PHY
 * setup is NOT done, so it may fail; the screen shows how far it got.
 *
 * Cmdline: rdiag_ufsinit=0|1   rdiag_ufslun=<n> (LUN holding the GPT, default 2)
 *
 * Runs with the MMU off (all memory is Device), so DMA needs no cache
 * maintenance. Register layout and descriptor formats: JEDEC UFSHCI (JESD223).
 */
#include <stdint.h>
#include "console.h"
#include "ufs_mtk.h"

#define UFS_BASE	0x11270000UL	/* ufshci@11270000 (live /proc/iomem) */
#define REG_MTK_IPVER	0x2240		/* ufs-mediatek REG_UFS_MTK_IP_VER */

#define REG_CAP		0x00
#define REG_VER		0x08
#define REG_IS		0x20
#define REG_HCS		0x30
#define REG_HCE		0x34
#define REG_UTRLBA	0x50
#define REG_UTRLBAU	0x54
#define REG_UTRLDBR	0x58
#define REG_UTRLCLR	0x5c
#define REG_UTRLRSR	0x60
#define REG_UICCMD	0x90
#define REG_UCMDARG1	0x94
#define REG_UCMDARG2	0x98
#define REG_UCMDARG3	0x9c

#define HCS_DP		(1u << 0)
#define HCS_UTRLRDY	(1u << 1)
#define HCS_UCRDY	(1u << 3)
#define IS_UCCS		(1u << 10)

#define UIC_LINKSTARTUP	0x16

static inline uint32_t ufs_rd(uint32_t off)
{
	return *(volatile uint32_t *)(UFS_BASE + off);
}

static inline void ufs_wr(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(UFS_BASE + off) = v;
}

/* UTP transfer request descriptor (32 bytes). */
struct utrd {
	uint32_t dw0;		/* [31:28] command type, [26:25] direction, [24] interrupt */
	uint32_t dw1;
	uint32_t dw2;		/* [7:0] OCS */
	uint32_t dw3;
	uint32_t ucdba;		/* command descriptor base, 128-byte aligned */
	uint32_t ucdbau;
	uint32_t resp;		/* [31:16] response UPIU offset, [15:0] length (DWORDs) */
	uint32_t prdt;		/* [31:16] PRDT offset (DWORDs), [15:0] entries */
};

/* UTP command descriptor: command UPIU, response UPIU, PRDT. */
struct ucd {
	uint8_t cmd[512];
	uint8_t resp[512];
	struct {
		uint32_t addr_lo, addr_hi, rsvd, dbc;	/* dbc = byte count - 1 */
	} prdt[1];
};

#define DATA_BYTES	(16 * 1024)	/* 128 GPT entries x 128 bytes */

static struct utrd utrl[32] __attribute__((aligned(1024)));
static struct ucd ucd __attribute__((aligned(128)));
static uint8_t data[DATA_BYTES] __attribute__((aligned(4096)));

static uint64_t freq;

static uint64_t ticks(void)
{
	uint64_t t;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
	return t;
}

static void delay_ms(uint32_t ms)
{
	uint64_t end = ticks() + freq / 1000 * ms;
	while (ticks() < end)
		;
}

static void zero(void *p, unsigned n)
{
	volatile uint8_t *b = p;
	for (unsigned i = 0; i < n; i++)
		b[i] = 0;
}

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint32_t le32(const uint8_t *p)
{
	return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t le64(const uint8_t *p)
{
	return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;
}

/* Result of one request in slot 0. */
struct rq {
	uint32_t ocs;		/* host-side completion status, 0 = ok */
	uint32_t resp_type;	/* response UPIU transaction type */
	uint32_t status;	/* SCSI status / query response code */
};

/*
 * Send one UPIU in slot 0. ucd.cmd must be filled in by the caller (bytes
 * 0..11 header, 12.. transaction specific). Returns 0 on success, otherwise
 * a code naming the stage: 1 timeout, 2 host error (OCS), 3 wrong response.
 */
static uint32_t submit(uint32_t dir, uint32_t len, void *buf, uint8_t want_resp, struct rq *rq)
{
	uintptr_t ucd_pa = (uintptr_t)&ucd;

	zero(&utrl[0], sizeof(utrl[0]));
	utrl[0].dw0 = (1u << 28) | (dir << 25) | (1u << 24);
	utrl[0].dw2 = 0x0f;			/* OCS: invalid until the host updates it */
	utrl[0].ucdba = (uint32_t)ucd_pa;
	utrl[0].ucdbau = (uint32_t)(ucd_pa >> 32);
	utrl[0].resp = (512u / 4) << 16 | (512u / 4);
	if (len) {
		uintptr_t buf_pa = (uintptr_t)buf;
		ucd.prdt[0].addr_lo = (uint32_t)buf_pa;
		ucd.prdt[0].addr_hi = (uint32_t)(buf_pa >> 32);
		ucd.prdt[0].rsvd = 0;
		ucd.prdt[0].dbc = len - 1;
		utrl[0].prdt = (1024u / 4) << 16 | 1;
	}
	__asm__ volatile("dsb sy" ::: "memory");

	ufs_wr(REG_UTRLDBR, 1);
	uint64_t start = ticks();
	while (ufs_rd(REG_UTRLDBR) & 1) {
		if (ticks() - start > freq) {		/* 1 s */
			ufs_wr(REG_UTRLCLR, ~1u);	/* clear slot 0 (write 0 to the bit) */
			rq->ocs = 0xff;
			return 1;
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
	rq->ocs = utrl[0].dw2 & 0xff;
	rq->resp_type = ucd.resp[0];
	if (rq->ocs)
		return 2;
	if (ucd.resp[0] != want_resp)
		return 3;
	return 0;
}

static void cmd_header(uint8_t type, uint8_t flags, uint8_t lun)
{
	static uint8_t tag;
	zero(&ucd, sizeof(ucd));
	ucd.cmd[0] = type;
	ucd.cmd[1] = flags;
	ucd.cmd[2] = lun;
	ucd.cmd[3] = ++tag;
}

/* NOP OUT / NOP IN: checks that host, link and device answer at all. */
static uint32_t nop_out(struct rq *rq)
{
	cmd_header(0x00, 0, 0);
	uint32_t rc = submit(0, 0, 0, 0x20, rq);
	rq->status = ucd.resp[7];
	return rc;
}

/* Query request on a flag: op 5 = READ FLAG, 6 = SET FLAG. Returns rc; flag in *val. */
static uint32_t query_flag(uint8_t op, uint8_t idn, uint32_t *val, struct rq *rq)
{
	cmd_header(0x16, 0, 0);
	ucd.cmd[5] = 0x81;			/* standard write request (also used for set) */
	if (op == 5)
		ucd.cmd[5] = 0x01;		/* standard read request */
	ucd.cmd[12] = op;
	ucd.cmd[13] = idn;
	uint32_t rc = submit(0, 0, 0, 0x36, rq);	/* QUERY RESPONSE */
	rq->status = ucd.resp[6];		/* query response code, 0 = success */
	*val = ucd.resp[23] & 1;
	if (!rc && rq->status)
		rc = 4;
	return rc;
}

/* One SCSI command (no data or device-to-host). */
static uint32_t scsi(uint8_t lun, const uint8_t *cdb, unsigned cdb_len, void *buf, uint32_t len,
		     struct rq *rq)
{
	cmd_header(0x01, len ? 0x40 : 0x00, lun);	/* COMMAND UPIU, R flag for data in */
	ucd.cmd[12] = len >> 24;
	ucd.cmd[13] = len >> 16;
	ucd.cmd[14] = len >> 8;
	ucd.cmd[15] = len;
	for (unsigned i = 0; i < cdb_len; i++)
		ucd.cmd[16 + i] = cdb[i];
	uint32_t rc = submit(len ? 2u : 0u, len, buf, 0x21, rq);	/* RESPONSE UPIU */
	rq->status = ucd.resp[7];		/* SCSI status */
	if (!rc && rq->status)
		rc = 4;
	return rc;
}

static uint32_t read_blocks(uint8_t lun, uint64_t lba, uint32_t count, uint32_t blk, void *buf)
{
	uint8_t cdb[10] = { 0x28, 0, lba >> 24, lba >> 16, lba >> 8, lba, 0, count >> 8, count, 0 };
	struct rq rq;
	if (count * blk > DATA_BYTES)
		return 9;
	return scsi(lun, cdb, sizeof(cdb), buf, count * blk, &rq);
}

static uint32_t lun_blk[3];		/* logical block size per LUN, from READ CAPACITY */

static void show(const char *label, uint32_t rc, const struct rq *rq)
{
	con_hex3(label, "rc=", rc, "ocs=", rq->ocs, "st=", rq->status);
}

/* fs type at the start of partition (first LBA) on lun */
static const char *fs_type(uint8_t lun, uint64_t first, uint32_t blk)
{
	uint32_t count = (2048 + blk - 1) / blk;
	if (read_blocks(lun, first, count, blk, data))
		return "read error";
	const uint8_t *sb = data + 1024;
	if (sb[0] == 0x10 && sb[1] == 0x20 && sb[2] == 0xf5 && sb[3] == 0xf2)
		return "f2fs";
	if (sb[56] == 0x53 && sb[57] == 0xef)
		return "ext4";
	return "other (encrypted/raw?)";
}

/* Read the GPT of lun and print the partitions that matter for the iOS root. */
static void gpt_report(uint8_t lun)
{
	uint32_t blk = lun_blk[lun];
	if (blk != 512 && blk != 4096) {
		con_hex("GPT", blk);
		con_str("GPT", "unexpected block size: stop");
		return;
	}
	if (read_blocks(lun, 1, 1, blk, data)) {
		con_str("GPT", "header read error");
		return;
	}
	char sig[9];
	for (int i = 0; i < 8; i++)
		sig[i] = (data[i] >= 32 && data[i] < 127) ? (char)data[i] : '.';
	sig[8] = 0;
	con_str("GPT", sig);
	uint32_t nent = le32(data + 80), esz = le32(data + 84);
	uint64_t elba = le64(data + 72);
	con_hex2("GPT", "entries=", nent, "esz=", esz);
	if (!streq(sig, "EFI PART") || esz != 128 || nent == 0 || nent > 128)
		return;
	uint32_t blocks = (nent * esz + blk - 1) / blk;
	if (read_blocks(lun, elba, blocks, blk, data)) {
		con_str("GPT", "entries read error");
		return;
	}
	/* fixed-width names, no pointer table: the image is linked at 0 but runs at
	 * 0x40080000, so absolute pointers in data would be wrong */
	static const char want[][12] = { "userdata", "boot_a", "boot_b", "super", "metadata",
					 "cust", "md1img_a", "vbmeta_a", "vbmeta_b" };
	uint32_t used = 0;
	for (uint32_t i = 0; i < nent; i++)
		if (le64(data + i * 128 + 32))
			used++;
	con_hex("GPT used", used);
	for (uint32_t i = 0; i < nent; i++) {
		const uint8_t *e = data + i * 128;
		uint64_t first = le64(e + 32), last = le64(e + 40);
		if (!first)
			continue;
		char name[17];
		for (int c = 0; c < 16; c++) {
			uint8_t ch = e[56 + 2 * c];
			name[c] = (ch >= 32 && ch < 127) ? (char)ch : (ch ? '?' : 0);
		}
		name[16] = 0;
		int hit = 0;
		for (unsigned w = 0; w < sizeof want / sizeof *want; w++)
			hit |= streq(name, want[w]);
		if (!hit)
			continue;
		uint64_t mib = ((last - first + 1) * blk) >> 20;
		con_hex3(name, "n=", i + 1, "lba=", (uint32_t)first, "MiB=", (uint32_t)mib);
		if (streq(name, "userdata")) {
			con_hex("  lba hi", first >> 32);
			con_str("  fs", fs_type(lun, first, blk));
		}
	}
}

/* Host enable + link startup, only on request. Returns 0 if the link is up. */
static int host_bringup(void)
{
	con_str("UFS INIT", "enable host + link startup (opt-in)");
	ufs_wr(REG_HCE, 1);
	uint64_t start = ticks();
	while (!(ufs_rd(REG_HCE) & 1)) {
		if (ticks() - start > freq / 10) {
			con_str("UFS INIT", "HCE never became 1");
			return 1;
		}
	}
	delay_ms(5);
	start = ticks();
	while (!(ufs_rd(REG_HCS) & HCS_UCRDY)) {
		if (ticks() - start > freq / 10) {
			con_hex("UCRDY timeout hcs", ufs_rd(REG_HCS));
			return 2;
		}
	}
	for (int attempt = 0; attempt < 3; attempt++) {
		ufs_wr(REG_IS, ufs_rd(REG_IS));
		ufs_wr(REG_UCMDARG1, 0);
		ufs_wr(REG_UCMDARG2, 0);
		ufs_wr(REG_UCMDARG3, 0);
		ufs_wr(REG_UICCMD, UIC_LINKSTARTUP);
		start = ticks();
		while (!(ufs_rd(REG_IS) & IS_UCCS)) {
			if (ticks() - start > freq / 2)
				break;
		}
		uint32_t is = ufs_rd(REG_IS), res = ufs_rd(REG_UCMDARG2) & 0xff, hcs = ufs_rd(REG_HCS);
		con_hex3("LINKSTARTUP", "is=", is, "res=", res, "hcs=", hcs);
		ufs_wr(REG_IS, IS_UCCS);
		if ((is & IS_UCCS) && res == 0 && (hcs & HCS_DP))
			return 0;
		delay_ms(10);
	}
	return 3;
}

void ufs_stage8(int do_init, int gpt_lun)
{
	freq = 0;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
	if (!freq)
		freq = 13000000;

	con_str("UFS", do_init ? "probe + opt-in init" : "probe (read-only)");
	con_begin("UFS base");
	uint32_t cap = ufs_rd(REG_CAP);
	__asm__ volatile("dsb sy; isb" ::: "memory");	/* surface a bus abort here */
	con_end_hex32(cap);
	uint32_t hce = ufs_rd(REG_HCE), hcs = ufs_rd(REG_HCS);
	con_hex2("UFS", "ver=", ufs_rd(REG_VER), "cap=", cap);
	con_hex2("UFS", "hce=", hce, "hcs=", hcs);
	con_hex2("UFS", "is=", ufs_rd(REG_IS), "rsr=", ufs_rd(REG_UTRLRSR));
	con_hex2("UFS", "utrlba=", ufs_rd(REG_UTRLBA), "hi=", ufs_rd(REG_UTRLBAU));
	con_hex2("UFS", "dbr=", ufs_rd(REG_UTRLDBR), "nutrs=", (cap & 0x1f) + 1);
	con_begin("UFS mtk ipver");
	uint32_t ipver = ufs_rd(REG_MTK_IPVER);
	__asm__ volatile("dsb sy; isb" ::: "memory");
	con_end_hex32(ipver);

	if (!(hce & 1)) {
		if (!do_init) {
			con_color_fg(C_YELLOW);
			con_str("UFS", "host DISABLED (LK reset it)");
			con_str("UFS", "rerun with rdiag_ufsinit=1");
			return;
		}
		if (host_bringup()) {
			con_color_fg(C_RED);
			con_str("UFS", "bring-up failed: see lines above");
			return;
		}
		hcs = ufs_rd(REG_HCS);
		con_hex("UFS hcs", hcs);
	}
	if (!(hcs & HCS_DP))
		con_str("UFS", "HCS.DP=0: no device present at link level");
	if (ufs_rd(REG_UTRLDBR)) {
		con_str("UFS", "requests outstanding: stop");
		return;
	}

	/* Take over the transfer request list: stop, point at ours, run. */
	ufs_wr(REG_UTRLRSR, 0);
	uintptr_t list = (uintptr_t)utrl;
	ufs_wr(REG_UTRLBA, (uint32_t)list);
	ufs_wr(REG_UTRLBAU, (uint32_t)(list >> 32));
	ufs_wr(REG_UTRLRSR, 1);
	con_hex2("UFS", "ours=", (uint32_t)list, "rsr=", ufs_rd(REG_UTRLRSR));
	/* Never ring the doorbell unless the host really uses our list. */
	if (ufs_rd(REG_UTRLBA) != (uint32_t)list || ufs_rd(REG_UTRLBAU) != (uint32_t)(list >> 32) ||
	    !(ufs_rd(REG_UTRLRSR) & 1)) {
		con_str("UFS", "list base not taken: stop");
		return;
	}

	struct rq rq = { 0, 0, 0 };
	uint32_t rc = 1;
	for (int i = 0; i < 3 && rc; i++) {
		rc = nop_out(&rq);
		show("NOP OUT", rc, &rq);
	}
	if (rc) {
		con_color_fg(C_RED);
		con_hex("  resp type", rq.resp_type);
		con_str("UFS", "no NOP IN: link/device not answering");
		return;
	}

	if (do_init) {
		uint32_t flag = 0;
		rc = query_flag(6, 1, &flag, &rq);		/* SET fDeviceInit */
		show("SET fDevInit", rc, &rq);
		uint64_t start = ticks();
		flag = 1;
		while (!rc && flag && ticks() - start < 2 * freq)
			rc = query_flag(5, 1, &flag, &rq);	/* READ until it clears */
		con_hex2("fDevInit", "rc=", rc, "flag=", flag);
	}

	/* Per-LUN capacity (READ CAPACITY(10) reports last LBA and block size). */
	for (uint8_t lun = 0; lun < 3; lun++) {
		static const uint8_t tur[6] = { 0 };
		static const uint8_t cap10[10] = { 0x25 };
		char lab[8] = { 'L', 'U', 'N', (char)('0' + lun), ' ', 'C', 'A', 0 };
		/* a first command often reports a unit attention: retry */
		for (int i = 0; i < 3; i++) {
			rc = scsi(lun, tur, sizeof(tur), 0, 0, &rq);
			if (!rc)
				break;
		}
		rc = scsi(lun, cap10, sizeof(cap10), data, 8, &rq);
		show(lab, rc, &rq);
		if (!rc) {
			lun_blk[lun] = be32(data + 4);
			con_hex2("  LUN", "lastlba=", be32(data), "blk=", lun_blk[lun]);
		}
	}

	/* INQUIRY on the GPT LUN: vendor + product so the device is identified. */
	static const uint8_t inq[6] = { 0x12, 0, 0, 0, 36, 0 };
	rc = scsi((uint8_t)gpt_lun, inq, sizeof(inq), data, 36, &rq);
	show("INQUIRY", rc, &rq);
	if (!rc) {
		char vp[25];
		for (int i = 0; i < 24; i++) {
			uint8_t ch = data[8 + i];
			vp[i] = (ch >= 32 && ch < 127) ? (char)ch : '.';
		}
		vp[24] = 0;
		con_str("  vendor/prod", vp);
	}

	if (gpt_lun < 0 || gpt_lun > 2 || !lun_blk[gpt_lun]) {
		con_str("GPT", "LUN has no capacity: stop");
		return;
	}
	con_hex("GPT LUN", (uint32_t)gpt_lun);
	gpt_report((uint8_t)gpt_lun);
	con_color_fg(C_GREEN);
	con_str("UFS", "stage 8 done");
}
