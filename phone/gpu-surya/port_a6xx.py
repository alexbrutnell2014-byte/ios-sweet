#!/usr/bin/env python3
"""Turn ios-flame's Adreno 640 core (sweet_a6xx.c/.h, MIT) into the surya Adreno 618 one.

  port_a6xx.py FLAME_DIR OUT_DIR

Every change is an exact, asserted replacement, so a flame update that moves the code fails loudly.
Sources: the phone's KGSL driver (refsrc/kgsl) and its device tree; see GPU-PORT-NOTES.md.
"""
import os
import sys

src, out = sys.argv[1], sys.argv[2]
h = open(os.path.join(src, 'sweet_a6xx.h')).read()
c = open(os.path.join(src, 'sweet_a6xx.c')).read()


def sub(text, old, new, what):
    if old not in text:
        raise SystemExit(f'port_a6xx: anchor not found ({what}):\n{old[:200]}')
    return text.replace(old, new, 1)


# ---------------- header: addresses and identity ----------------
h = sub(h, ' * Adreno 640 core shared', ' * Adreno 618 core (surya: Poco X3 NFC, SM7150), ported from ios-flame\'s Adreno 640 core shared', 'h title')
h = sub(h, '#define A6_FB_PA\t\t0x9c400000ull', '#define A6_FB_PA\t\t0x9c000000ull\t/* surya cont_splash_region */', 'fb')
h = sub(h, '#define A6_GPU_ID\t\t640', '#define A6_GPU_ID\t\t618', 'gpu id')
h = sub(h, '#define A6_CHIP_ID\t\t0x06040001ull', '#define A6_CHIP_ID\t\t0x06010800ull\t/* qcom,chipid */', 'chip id')
h = sub(h, '#define A6_GPU_BASE\t\t0x02c00000ull', '#define A6_GPU_BASE\t\t0x05000000ull\t/* surya kgsl-3d0 */', 'gpu base')
h = sub(h, '#define A6_GPUCC\t\t0x02c90000ull', '#define A6_GPUCC\t\t0x05090000ull\t/* surya gpucc-sdmmagpie */', 'gpucc')
h = sub(h, '#define A6_SMMU\t\t\t0x02ca0000ull', '#define A6_SMMU\t\t\t0x05040000ull\t/* surya arm,smmu-kgsl */', 'smmu')

# ---------------- core ----------------
c = sub(c, ' * Adreno 640 core (see sweet_a6xx.h).', ' * Adreno 618 core for surya (see sweet_a6xx.h), ported from ios-flame\'s Adreno 640 core (MIT)\n * with the A618 (a615 family) behaviour of the phone\'s KGSL driver: GPU-PORT-NOTES.md.', 'c title')

# GCC: DDR -> GPU AXI branch moved on SM7150
c = sub(c, 'branch_on("gcc ddrss_gpu_axi", GCC + 0x71154);', 'branch_on("gcc ddrss_gpu_axi", GCC + 0x4452c);\t/* surya gcc-sdmmagpie */', 'ddrss')

# RSCC lives at GMU + 0x23000: GMU at 0x0506a000 on surya
c = sub(c, '#define RSCC\t\t(0x02c6a000ull + 0x23000)', '#define RSCC\t\t(0x0506a000ull + 0x23000)\t/* surya qcom,gmu@506a000 */', 'rscc')

# PDC: A618 TCS3 CMD1 data is 2 (KGSL _load_gmu_rpmh_ucode), A640 3
c = sub(c, '{ 0x15de, 0x30000 }, { 0x15df, 3 },', '{ 0x15de, 0x30000 }, { 0x15df, 2 },\t/* A618: 2 */\n\t\t', 'pdc')
c = sub(c, '/* a6xx_gmu_rpmh_init for the A640 (pdc_address_offset 0x30090). */', '/* KGSL _load_gmu_rpmh_ucode for the A618 (0x30090 like the A640; TCS3 CMD1 data 2). */', 'pdc comment')

# HFI: legacy queue table (a630/a615 family): 4 queues, ids CMD 0, MSG 4, DBG 5, DSP 1 (disabled)
old_tbl = c[c.index('static void hfi_table(struct a6_gpu *g)'):c.index('static int fw_load(struct a6_gpu *g)')]
new_tbl = '''static void hfi_table(struct a6_gpu *g)
{
	/* KGSL hfi_init, legacy firmware (a630/a615 family): queue ids CMD 0, MSG 4, DBG 5, DSP 1 (off) */
	static const uint32_t qid[4] = { 0, 4, 5, 1 }, pri[4] = { 10, 10, 40, 20 }, on[4] = { 1, 1, 1, 0 };
	uint64_t t = GMEM_HFI(g);
	wr32(t + 0, 0);				/* version */
	wr32(t + 4, 6 + 4 * 12);		/* size, dwords */
	wr32(t + 8, 6);				/* qhdr0_offset */
	wr32(t + 12, 12);			/* qhdr_size */
	wr32(t + 16, 4);			/* num_q */
	wr32(t + 20, 4);			/* num_active_q */
	for (uint32_t q = 0; q < 4; q++) {
		uint64_t hq = t + 24 + 48 * q;
		for (uint32_t i = 0; i < 12; i++)
			wr32(hq + 4 * i, 0);
		wr32(hq + 0, on[q]);
		wr32(hq + 4, VA_HFI + 0x1000 * (q + 1));
		wr32(hq + 8, qid[q] | (pri[q] << 8));
		wr32(hq + 12, 0x1000 >> 2);
		wr32(hq + 24, 1);		/* rx_wm */
		wr32(hq + 28, 1);		/* tx_wm */
		wr32(hq + 32, 1);		/* rx_req */
	}
}

'''
c = c.replace(old_tbl, new_tbl, 1)
c = sub(c, 'map_gmu_pages(GMEM_L3_6(g), VA_HFI, GMEM_HFI(g), 0x4000, 0);', 'map_gmu_pages(GMEM_L3_6(g), VA_HFI, GMEM_HFI(g), 0x5000, 0);\t/* table + 4 queues */', 'hfi map')

# GMU firmware: legacy image, written whole into ITCM (KGSL _load_legacy_gmu_fw)
old_fw = c[c.index('static int fw_load(struct a6_gpu *g)'):c.index('static int gmu_boot(struct a6_gpu *g)')]
new_fw = '''static int fw_load(struct a6_gpu *g)
{
	/* KGSL _load_legacy_gmu_fw (a630/a615 family): the whole image into ITCM, at most 32 KB */
	(void)g;
	uint32_t len;
	const uint8_t *fw = a6_firmware("a618_gmu.bin", &len);
	if (!fw)
		return -1;
	if (len > 0x8000 || (len & 3)) {
		a6_log("gmu fw: legacy image size %u not allowed\\n", len);
		return -1;
	}
	a6_write(GMU_REG(GMU_ITCM), fw, len);
	a6_log("gmu fw: %u bytes into ITCM, first words %x %x\\n", len, rd32(GMU_REG(GMU_ITCM)), rd32(GMU_REG(GMU_ITCM) + 4));
	return 1;
}

'''
c = c.replace(old_fw, new_fw, 1)

# GMU start: chip id compressed for the A618 (06 01 08 00 -> 0x06018000); no GBIF QSB writes on a615 family
c = sub(c, 'wr32(GMU_REG(GMU_SFR_ADDR), 0x06040100);', 'wr32(GMU_REG(GMU_SFR_ADDR), 0x06018000);\t/* chip id: core, major, minor<<12, patch<<8 */', 'sfr')
c = sub(c, '''	for (uint32_t i = 0; i < 4; i++)
		wr32(GPU + 4 * (0x3c03 + i), 0x00071620);
''', '''	/* (A640 GBIF_QSB_SIDE0..3 = 0x71620: not on the a615 family, KGSL a615_gbif) */
''', 'qsb')
# after the GMU booted: HFI init handshake (KGSL a6xx_gmu_hfi_start)
c = sub(c, '''	a6_log("gmu: init result %x %s, hfi_ctrl %x\\n", r, ok ? "BOOTED" : "timeout", rd32(GMU_REG(GMU_HFI_CTRL)));
	return ok ? 0 : -1;''', '''	a6_log("gmu: init result %x %s, hfi_ctrl %x\\n", r, ok ? "BOOTED" : "timeout", rd32(GMU_REG(GMU_HFI_CTRL)));
	if (!ok)
		return -1;
	wr32(GMU_REG(0x1f986), 1);				/* HFI_CTRL_INIT */
	int hrc = wait_bit(GMU_REG(GMU_HFI_CTRL), 1, 1, 100);	/* HFI_CTRL_STATUS bit 0 */
	a6_log("gmu: hfi init %s (status %x)\\n", hrc ? "TIMEOUT" : "ok", rd32(GMU_REG(GMU_HFI_CTRL)));
	return hrc ? -1 : 0;''', 'hfi start')

# HFI v1 exchange: keep the reply's 4th word (FW_VER answers there)
c = sub(c, '''				if ((hdr & 0xff) != 100 && (buf[1] >> 20) == seq)
					return (int)buf[2];''', '''				if ((hdr & 0xff) == 100)
					a6_log("hfi: GMU error %x %x\\n", buf[1], buf[2]);
				else if ((buf[1] >> 20) == (seq & 0xfff)) {
					hfi_last[0] = buf[2];
					hfi_last[1] = buf[3];
					return (int)buf[2];
				}''', 'hfi ack')
c = sub(c, 'static int hfi_send(struct a6_gpu *g, uint32_t id, uint32_t *msg, uint32_t dwords)\n{',
        'static uint32_t hfi_last[2];\t/* error word and the next one of the last ack */\n\nstatic int hfi_send(struct a6_gpu *g, uint32_t id, uint32_t *msg, uint32_t dwords)\n{', 'hfi_last')

# GPU frequency levels: surya qcom,gpu-pwrlevels (825/800/650/565/430/355/267/180 MHz); bring-up at the low four
c = sub(c, 'static const uint32_t gpu_khz[] = { 0, 257000, 345000, 427000, 499200, 585000 };',
        'static const uint32_t gpu_khz[] = { 0, 180000, 267000, 355000, 430000 };\t/* surya gpu-pwrlevels, low end */', 'gpu khz')
c = sub(c, 'static const uint32_t gpu_lvl[] = { 0, LVL_LOW_SVS, LVL_SVS, LVL_SVS_L1, LVL_SVS_L2, LVL_NOM };',
        'static const uint32_t gpu_lvl[] = { 0, LVL_LOW_SVS, LVL_SVS, LVL_SVS_L1, LVL_NOM };', 'gpu lvl')

# cmd-db: the address of a resource by name (BCM bandwidth votes for the BW table)
c = sub(c, 'static uint32_t aux16(uint64_t aux, uint32_t i)', '''static uint32_t cmddb_addr(const char *id)
{
	uint64_t db = rd32(0x0c3f000cull);
	if (rd32(db + 4) != 0x0c0330db)
		return 0;
	uint64_t data = db + 4 + 4 + 8 * 16 + 4 + 4;
	for (uint32_t r = 0; r < 8; r++) {
		uint64_t rh = db + 8 + 16 * r;
		uint32_t w0 = rd32(rh), w1 = rd32(rh + 4);
		uint32_t hoff = w0 >> 16, cnt = w1 >> 16;
		for (uint32_t e = 0; e < cnt; e++) {
			uint64_t ent = data + hoff + 24 * e;
			char name[9];
			for (uint32_t i = 0; i < 8; i++)
				name[i] = (char)(rd32(ent + (i & ~3u)) >> (8 * (i & 3)));
			name[8] = 0;
			if (!strncmp(name, id, 8))
				return rd32(ent + 16);
		}
	}
	return 0;
}

static uint32_t aux16(uint64_t aux, uint32_t i)''', 'cmddb_addr')

# HFI v1 start sequence (KGSL hfi_start for legacy firmware) and the a615-family OOB bits
old_hfi = c[c.index('static int gmu_hfi(struct a6_gpu *g)'):c.index('/* ---- zap shader through TrustZone PAS ---- */')]
new_hfi = '''static int gmu_hfi(struct a6_gpu *g)
{
	static uint32_t msg[256];
	uint32_t gx[5], cx[2];
	if (arc_votes(gx, gpu_lvl, 5, "gfx.lvl", "mx.lvl") || arc_votes(cx, gmu_lvl, 2, "cx.lvl", "mx.lvl")) {
		a6_log("hfi: cmd-db levels not found\\n");
		return -1;
	}
	/* H2F_MSG_INIT (0): seg_id, dbg buffer, boot state COLD (1) */
	memset(msg, 0, sizeof(msg));
	msg[2] = VA_DUMMY;
	msg[3] = 0x1000;
	msg[4] = 1;
	int rc = hfi_send(g, 0, msg, 5);
	a6_log("hfi v1: INIT -> %d\\n", rc);
	if (rc)
		return -1;
	/* H2F_MSG_FW_VER (1): supported = GMU_VERSION(1, 8) */
	memset(msg, 0, sizeof(msg));
	msg[1] = (1u << 28) | (8u << 16);
	rc = hfi_send(g, 1, msg, 2);
	a6_log("hfi v1: FW_VER -> %d, firmware %u.%u.%u, hfi version %x\\n", rc, hfi_last[1] >> 28,
	    (hfi_last[1] >> 16) & 0xfff, hfi_last[1] & 0xffff, rd32(GMU_REG(0x1f981)));
	/* H2F_MSG_PERF_TBL (4), v1 layout: counts, then {vote, kHz} x MAX_GX_LEVELS 16, x MAX_CX_LEVELS 4 */
	memset(msg, 0, sizeof(msg));
	msg[1] = 5;
	msg[2] = 2;
	for (int i = 0; i < 5; i++) {
		msg[3 + 2 * i] = gx[i];
		msg[4 + 2 * i] = gpu_khz[i];
	}
	for (int i = 0; i < 2; i++) {
		msg[3 + 32 + 2 * i] = cx[i];
		msg[4 + 32 + 2 * i] = gmu_khz[i];
	}
	rc = hfi_send(g, 4, msg, 3 + 32 + 8);
	a6_log("hfi v1: PERF_TBL (5 GX, 2 CX levels) -> %d\\n", rc);
	/*
	 * H2F_MSG_BW_VOTE_TBL (3), one level (as mainline a618_build_bw_table): DDR SH0/MC0/ACV and the config
	 * NoC CN0, by their cmd-db addresses on this SoC. Layout: level, cnoc n, ddr n, cnoc wait, ddr wait,
	 * cnoc addrs[6], cnoc data[2][6], ddr addrs[8], ddr data[16][8].
	 */
	memset(msg, 0, sizeof(msg));
	static const char *const ddr[] = { "SH0", "MC0", "ACV" };
	uint32_t nd = 0, cn0 = cmddb_addr("CN0");
	for (unsigned i = 0; i < 3; i++) {
		uint32_t a = cmddb_addr(ddr[i]);
		a6_log("hfi v1: bcm %s at %x\\n", ddr[i], a);
		if (a) {
			msg[24 + nd] = a;
			msg[32 + nd] = 0x40000000;
			nd++;
		}
	}
	msg[1] = 1;
	msg[3] = nd;
	msg[5] = 1;
	if (cn0) {
		msg[2] = 1;
		msg[4] = 1;
		msg[6] = cn0;
		msg[12] = 0x40000000;
		msg[18] = 0x60000001;
	}
	rc = hfi_send(g, 3, msg, 160);
	a6_log("hfi v1: BW_TBL (%u ddr cmds, cn0 %x) -> %d\\n", nd, cn0, rc);

	/* OOB GPU (a630/a615 family): set BIT(16), ack BIT(24), clear BIT(24) */
	wr32(GMU_REG(GMU_H2G_SET), 1u << 16);
	int ack = wait_bit(GMU_REG(GMU_G2H_INFO), 1u << 24, 1, 1000);
	wr32(GMU_REG(GMU_G2H_CLR), 1u << 24);
	a6_log("oob gpu_set %s, gx gdsc %x\\n", ack ? "TIMEOUT" : "acked", rd32(GPUCC + GPUCC_GX_GDSCR));
	if (!a6_gx_up()) {
		a6_log("GX not powered; not touching GPU registers\\n");
		return -1;
	}
	return 0;
}

'''
c = c.replace(old_hfi, new_hfi, 1)

# zap shader: a615_zap, region gpu_region@97715000 (0x2000), segments placed by the mdt's program headers
c = sub(c, '#define ZAP_REGION\t0x9a915000ull\t/* reserved-memory pil_gpu_region */', '#define ZAP_REGION\t0x97715000ull\t/* surya reserved-memory gpu_region@97715000 */', 'zap region')
old_zap = c[c.index('static int zap_load(struct a6_gpu *g)'):c.index('/* ---- ring ---- */')]
new_zap = '''static int zap_load(struct a6_gpu *g)
{
	uint64_t r1 = 0;
	uint32_t mlen;
	const uint8_t *mdt = a6_firmware("a615_zap.mdt", &mlen);
	if (!mdt || mlen < 52)
		return -1;
	a6_write(a6_pool_pa(g, A6_OFF_ZAP_META), mdt, (mlen + 3) & ~3u);
	int64_t rc = scm(1, 2 | (2u << 6), PAS_GPU, a6_pool_pa(g, A6_OFF_ZAP_META), 0, &r1);
	if (rc || r1) {
		a6_log("zap: init image rc %lld r1 %llx\\n", (long long)rc, (unsigned long long)r1);
		return -1;
	}
	rc = scm(2, 3, PAS_GPU, ZAP_REGION, ZAP_SIZE, &r1);
	if (rc || r1) {
		a6_log("zap: mem setup rc %lld r1 %llx\\n", (long long)rc, (unsigned long long)r1);
		return -1;
	}
	/* ELF32: e_phoff 0x1c, e_phentsize 0x2a, e_phnum 0x2c; PT_LOAD (1) segments with data, not the hash (flags 24-26 = 2) */
	uint32_t phoff, base = 0xffffffffu;
	uint16_t phent, phnum;
	memcpy(&phoff, mdt + 0x1c, 4);
	memcpy(&phent, mdt + 0x2a, 2);
	memcpy(&phnum, mdt + 0x2c, 2);
	if (phent < 32 || phoff + (uint32_t)phent * phnum > mlen)
		return -1;
	for (uint16_t i = 0; i < phnum; i++) {
		const uint8_t *p = mdt + phoff + (uint32_t)phent * i;
		uint32_t type, paddr, flags;
		memcpy(&type, p, 4);
		memcpy(&paddr, p + 12, 4);
		memcpy(&flags, p + 24, 4);
		if (type == 1 && ((flags >> 24) & 7) != 2 && paddr < base)
			base = paddr;
	}
	static uint8_t seg[ZAP_SIZE];
	memset(seg, 0, sizeof(seg));
	for (uint16_t i = 0; i < phnum; i++) {
		const uint8_t *p = mdt + phoff + (uint32_t)phent * i;
		uint32_t type, paddr, filesz, flags, slen;
		memcpy(&type, p, 4);
		memcpy(&paddr, p + 12, 4);
		memcpy(&filesz, p + 16, 4);
		memcpy(&flags, p + 24, 4);
		if (type != 1 || ((flags >> 24) & 7) == 2 || !filesz)
			continue;
		char name[24];
		snprintf(name, sizeof(name), "a615_zap.b%02u", (unsigned)i);
		const uint8_t *s = a6_firmware(name, &slen);
		if (!s || slen < filesz || paddr - base + filesz > ZAP_SIZE) {
			a6_log("zap: segment %u (%s) paddr %x size %x does not fit\\n", i, name, paddr, filesz);
			return -1;
		}
		memcpy(seg + (paddr - base), s, filesz);
	}
	a6_write(ZAP_REGION, seg, ZAP_SIZE);
	rc = scm(5, 1, PAS_GPU, 0, 0, &r1);
	a6_log("zap: auth and reset rc %lld r1 %llx\\n", (long long)rc, (unsigned long long)r1);
	return rc || r1 ? -1 : 0;
}

'''
c = c.replace(old_zap, new_zap, 1)

# CP init: A618 values from KGSL a6xx_start (ROQ, GMEM 512K); no A640 UBWC config for the first milestone
c = sub(c, '/* a6xx hw_init for the A640 (a6xx_gpu.c), minus clock gating, CP protection and interrupts */',
        '/* a6xx hw_init for the A618 (KGSL a6xx_start), minus clock gating, CP protection and interrupts */', 'cp comment')
c = sub(c, '{ 0x0e0b, 0x100000 }, { 0x0e0c, 0 }, { 0x0e0d, 0x1fffff }, { 0x0e0e, 0 },\t/* GMEM range */',
        '{ 0x0e0b, 0x100000 }, { 0x0e0c, 0 }, { 0x0e0d, 0x17ffff }, { 0x0e0e, 0 },\t/* GMEM range: 512K on the A618 */', 'gmem')
c = sub(c, '{ 0x08c2, 0x02000140 }, { 0x08c1, 0x8040362c }, { 0x08c3, 128 },\t/* ROQ, mem pool */',
        '{ 0x08c2, 0x010000c0 }, { 0x08c1, 0x8040362c }, { 0x08c3, 128 },\t/* ROQ (A618), mem pool */', 'roq')
c = sub(c, '''		/* a6xx_set_ubwc_config: highest bank bit 15 (hbb_lo 2), AMSBC, UBWC 3 */
		{ 0x8e08, (1u << 4) | (2u << 1) }, { 0xb604, 2u << 1 }, { 0xae02, 2u << 1 },
		{ 0x0e01, 2u << 21 }, { 0x0534, 0 },
''', '''		/* (A640 UBWC config left out: the A618 values come with real rendering) */
''', 'ubwc')

os.makedirs(out, exist_ok=True)
open(os.path.join(out, 'sweet_a6xx.h'), 'w').write(h)
open(os.path.join(out, 'sweet_a6xx.c'), 'w').write(c)
print('ported:', len(h.splitlines()), 'header lines,', len(c.splitlines()), 'core lines')
