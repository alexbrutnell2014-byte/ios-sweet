/*
 * adspd: the audio DSP (LPASS Hexagon, "adsp") of the Poco X3 NFC (surya, SM7150), started and spoken
 * to from userland through /dev/pmem, the way wland starts the modem. Stage 1 of the speakers.
 *
 *   adspd status            SMEM: our partition with the ADSP, its SMP2P item, whether it has run
 *   adspd boot [SECS]       rails, firmware (TrustZone PAS 1), SMP2P "ready", GLINK on the "lpass" edge
 *                           (IPCRTR + apr_audio_svc), QRTR + pd-mapper, then APR: the core service's
 *                           state until it says the audio services are up, and their versions; then
 *                           stays SECS seconds (default 20; 0 = until killed) serving the link
 *   adspd tone [T] [SECS]   boot, then stage 2: the speakers' MI2S port, a stream through ADM/ASM and a
 *                           1 kHz tone at -20 dBFS for T seconds (default 3); the amplifiers stay off
 *   adspd stop              asks the ADSP to stop (SMP2P), then TrustZone shuts it down
 *
 * Numbers (the phone's own device tree, /root/surya/live.dts):
 *   qcom,lpass@62400000: pil-tz-generic, PAS id 1, firmware "adsp" (adsp.mdt + .b00-.b26 from the modem
 *     partition's image/, staged in /var/local/fw/adsp), memory pil_adsp_region 0x95700000 + 0x1e00000,
 *     rails vdd_lpi_cx / vdd_lpi_mx at level 385, qcom,signal-aop ("adsp" load_state)
 *   qcom,smp2p-adsp: SMEM items 443 (the ADSP's, inbound) and 429 (ours), remote pid 2,
 *     doorbell APCS IPC bit 26
 *   glink adsp: SMEM transport with pid 2, mailbox channel 24 (APCS IPC bit 24), label "lpass",
 *     channel IPCRTR (intents 0x800 x5, 0x2000 x3, 0x4400 x2)
 *   pd-mapper: the firmware's adspr.jsn / adsps.jsn / adspua.jsn (instance 74)
 * Layouts and the order of things: Linux qcom_q6v5_pas.c, qcom_smp2p.c, qcom_glink_smem.c / _native.c,
 * net/qrtr, sound/soc/qcom/qdsp6/apr.c + q6core.c, and HackiOS drivers/audio/hakios.adsp.c (APSL 2.0),
 * which does the same for the POCO X5 Pro.
 *
 * Safety (HackiOS): never start the ADSP over the SMEM state of an earlier run in this power cycle --
 * if its SMP2P item already exists, boot refuses (another phone reset itself when that was tried).
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "pmem.h"
#include "smem.h"
#include "qmi.h"

#define HOST_ADSP	2
#define APCS_IPC	0x17c0000cull
#define IPC_AOSS	0
#define IPC_GLINK	24
#define IPC_SMP2P	26
#define SMP2P_OUT	429
#define SMP2P_IN	443
#define PAS_ADSP	1
#define ADSP_PA		0x95700000ull
#define ADSP_SIZE	0x01e00000ull
#define FW_DIR		"/var/local/fw/adsp"

static uint64_t t0;
static uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
#define LOG(fmt, ...)	printf("[%6llu] " fmt "\n", (unsigned long long)(now_ms() - t0), ##__VA_ARGS__)

static void kick(unsigned bit)
{
	wr32(APCS_IPC, 1u << bit);
}

/* ---- cmd-db + RPMh rails (wland.c) ---- */

static int cmddb_aux(const char *id, uint64_t *aux, uint32_t *len)
{
	uint64_t db = rd32(0x0c3f000cull);
	if (rd32(db + 4) != 0x0c0330db)
		return -1;
	uint64_t data = db + 4 + 4 + 8 * 16 + 4 + 4;
	for (uint32_t r = 0; r < 8; r++) {
		uint64_t rh = db + 8 + 16 * r;
		uint32_t w0 = rd32(rh), w1 = rd32(rh + 4);
		uint32_t hoff = w0 >> 16, doff = w1 & 0xffff, cnt = w1 >> 16;
		for (uint32_t e = 0; e < cnt; e++) {
			uint64_t ent = data + hoff + 24 * e;
			char name[9];
			for (uint32_t i = 0; i < 8; i++)
				name[i] = (char)(rd32(ent + (i & ~3u)) >> (8 * (i & 3)));
			name[8] = 0;
			if (strncmp(name, id, 8))
				continue;
			uint32_t w5 = rd32(ent + 20);
			*len = w5 & 0xffff;
			*aux = data + doff + (w5 >> 16);
			return 0;
		}
	}
	return -1;
}

static uint32_t aux16(uint64_t aux, uint32_t i)
{
	return (rd32(aux + 2 * (i & ~1u)) >> (16 * (i & 1))) & 0xffff;
}

/* An ARC rail at the first level >= want (Linux rpmhpd: the corner's value to the index). */
static int rail(const char *res, uint32_t want)
{
	uint64_t aux;
	uint32_t len, idx = 0, val = 0;
	if (cmddb_aux(res, &aux, &len)) {
		LOG("rail %s: not in cmd-db", res);
		return -1;
	}
	/* the first level at or above want; else the highest there is */
	for (uint32_t i = 0; i < len / 2; i++) {
		uint32_t v = aux16(aux, i);
		if (i && v == 0)
			break;
		idx = i;
		val = v;
		if (v >= want)
			break;
	}
	uint32_t off[1] = { 0 }, value[1] = { idx };
	int rc = pm_rpmh(res, 1, off, value);
	LOG("rail %s: level %u (index %u) -> %d", res, val, idx, rc);
	return rc;
}

/* ---- AOSS QMP (wland.c) ---- */

#define QMP_RAM		0x0c300000ull
#define QMP_UP		0x0000ffffu

static int wait_eq(uint64_t pa, uint32_t want, int ms)
{
	for (int i = 0; i < ms; i++) {
		if (rd32(pa) == want)
			return 0;
		usleep(1000);
	}
	return -1;
}

static int qmp_send(const char *msg)
{
	if (rd32(QMP_RAM) != 0x4d41494c || rd32(QMP_RAM + 4) != 1)
		return -1;
	if (!(rd32(QMP_RAM + 0x24) == QMP_UP && rd32(QMP_RAM + 0x2c) == QMP_UP && rd32(QMP_RAM + 0x30) == QMP_UP)) {
		/* wland opens the link at boot; if it has not, open it as it does */
		wr32(QMP_RAM + 0x10, rd32(QMP_RAM + 0x0c));
		wr32(QMP_RAM + 0x24, QMP_UP);
		kick(IPC_AOSS);
		if (wait_eq(QMP_RAM + 0x28, QMP_UP, 1000))
			return -1;
		wr32(QMP_RAM + 0x2c, QMP_UP);
		kick(IPC_AOSS);
		if (wait_eq(QMP_RAM + 0x14, QMP_UP, 1000))
			return -1;
		wr32(QMP_RAM + 0x18, QMP_UP);
		kick(IPC_AOSS);
		if (wait_eq(QMP_RAM + 0x30, QMP_UP, 1000))
			return -1;
	}
	uint32_t off = rd32(QMP_RAM + 0x38), buf[16] = { 0 };
	/* one message at a time: wland may be sending too; wait for the slot to be free */
	if (wait_eq(QMP_RAM + off, 0, 1000))
		return -1;
	strncpy((char *)buf, msg, sizeof(buf) - 1);
	for (int i = 0; i < 16; i++)
		wr32(QMP_RAM + off + 4 + 4 * i, buf[i]);
	wr32(QMP_RAM + off, sizeof(buf));
	(void)rd32(QMP_RAM + off);
	kick(IPC_AOSS);
	int rc = wait_eq(QMP_RAM + off, 0, 1000);
	LOG("qmp: \"%s\" -> %s", msg, rc ? "TIMEOUT" : "acked");
	return rc;
}

/* ---- SMP2P with the ADSP (wland.c, pid 2) ---- */

static void wr_name(uint64_t pa, const char *name)
{
	char n[16] = { 0 };
	strncpy(n, name, sizeof(n));
	for (int i = 0; i < 16; i += 4) {
		uint32_t w;
		memcpy(&w, n + i, 4);
		wr32(pa + i, w);
	}
}

static uint64_t smp2p_out;

static int smp2p_create(void)
{
	uint64_t pa;
	if (smem_alloc(HOST_ADSP, SMP2P_OUT, 20 + 16 * 20, &pa)) {
		LOG("smp2p: cannot allocate item %u", SMP2P_OUT);
		return -1;
	}
	if (rd32(pa) != 0x504d5324) {
		wr32(pa + 8, 0 | (2u << 16));		/* local pid 0, remote pid 2 */
		wr32(pa + 12, 16 | (1u << 16));		/* 16 entries, 1 valid */
		wr32(pa + 16, 0);
		wr_name(pa + 20, "master-kernel");
		wr32(pa + 36, 0);
		wr32(pa + 0, 0x504d5324);
		__asm__ volatile("dsb sy" ::: "memory");
		wr32(pa + 4, 1 | (1u << 8));		/* version 1, features: SSR ack */
	}
	smp2p_out = pa;
	kick(IPC_SMP2P);
	return 0;
}

static int smp2p_in_entry(const char *name, uint64_t *va)
{
	uint64_t pa;
	uint32_t sz;
	if (smem_get(HOST_ADSP, SMP2P_IN, &pa, &sz))
		return -1;
	uint32_t n = rd32(pa + 12) >> 16;
	for (uint32_t i = 0; i < n && i < 16; i++) {
		uint64_t e = pa + 20 + 20ull * i;
		char nm[17];
		for (int k = 0; k < 16; k++)
			nm[k] = (char)(rd32(e + (k & ~3)) >> (8 * (k & 3)));
		nm[16] = 0;
		if (!strcmp(nm, name)) {
			*va = e + 16;
			return 0;
		}
	}
	return -1;
}

static uint32_t adsp_state = 0xffffffff;

static void crash_reason(void)
{
	/* SMEM_SSR_REASON_LPASS0 = 423 (DT qcom,smem-id 0x1a7) */
	static const unsigned hosts[] = { SMEM_HOST_GLOBAL, HOST_ADSP };
	for (unsigned h = 0; h < 2; h++) {
		uint64_t pa;
		uint32_t sz;
		if (smem_get(hosts[h], 423, &pa, &sz))
			continue;
		char buf[513];
		uint32_t n = sz < 512 ? sz : 512;
		for (uint32_t i = 0; i < n; i += 4) {
			uint32_t w = rd32(pa + i);
			memcpy(buf + i, &w, (n - i) < 4 ? (n - i) : 4);
		}
		buf[n] = 0;
		for (uint32_t i = 0; i < n && buf[i]; i++)
			if (buf[i] < 32 || buf[i] > 126)
				buf[i] = '.';
		LOG("adsp crash reason: \"%s\"", buf);
		return;
	}
	LOG("adsp crash reason: item 423 absent");
}

static void smp2p_service(void)
{
	uint64_t ip, va;
	uint32_t sz, v;
	if (!smp2p_out)
		return;
	if (smem_get(HOST_ADSP, SMP2P_IN, &ip, &sz) == 0) {
		uint32_t want = (rd32(ip + 16) & 1) ? 2 : 0;
		if ((rd32(smp2p_out + 16) & 2) != want) {
			wr32(smp2p_out + 16, (rd32(smp2p_out + 16) & ~2u) | want);
			kick(IPC_SMP2P);
		}
	}
	if (smp2p_in_entry("slave-kernel", &va) == 0 && (v = rd32(va)) != adsp_state) {
		LOG("smp2p: adsp state %x%s%s%s%s", v, v & 1 ? " ERR-FATAL" : "", v & 2 ? " ready" : "",
		    v & 4 ? " handover" : "", v & 8 ? " stop-ack" : "");
		if ((v & 1) && !(adsp_state != 0xffffffff && (adsp_state & 1)))
			crash_reason();
		adsp_state = v;
	}
}

/* ---- firmware: TrustZone PAS (wland.c boot_modem with the ADSP's numbers) ---- */

#define SCM_PIL(cmd)	(0x42000000ull | (2u << 8) | (cmd))

static int64_t scm(uint32_t cmd, uint64_t arginfo, uint64_t a, uint64_t b, uint64_t c, uint64_t *r1)
{
	uint64_t x[8] = { SCM_PIL(cmd), arginfo, a, b, c, 0, 0, 0 };
	pm_smc(x);
	if (r1)
		*r1 = x[1];
	return (int64_t)x[0];
}

static uint8_t *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	*len = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *b = malloc(*len + 1);
	if (b && fread(b, 1, *len, f) != *len) {
		free(b);
		b = NULL;
	}
	fclose(f);
	return b;
}

struct phdr { uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align; };

static int load_and_start(void)
{
	size_t mlen;
	uint8_t *mdt = slurp(FW_DIR "/adsp.mdt", &mlen);
	if (!mdt) {
		LOG("boot: no %s/adsp.mdt", FW_DIR);
		return -1;
	}
	uint32_t phoff, phnum = 0;
	memcpy(&phoff, mdt + 0x1c, 4);
	memcpy(&phnum, mdt + 0x2c, 2);
	struct phdr *ph = (struct phdr *)(mdt + phoff);
	int hash = -1;
	for (uint32_t i = 0; i < phnum; i++)
		if (((ph[i].flags >> 24) & 7) == 2)
			hash = (int)i;
	if (hash < 0) {
		LOG("boot: no hash segment");
		return -1;
	}
	size_t ehsz = phoff + 32ull * phnum, meta_len = ehsz + ph[hash].filesz;
	uint8_t *meta = malloc(meta_len);
	memcpy(meta, mdt, ehsz);
	if (ph[hash].offset + ph[hash].filesz <= mlen) {
		memcpy(meta + ehsz, mdt + ph[hash].offset, ph[hash].filesz);
	} else {
		char path[128];
		size_t hl;
		snprintf(path, sizeof(path), FW_DIR "/adsp.b%02d", hash);
		uint8_t *h = slurp(path, &hl);
		if (!h || hl < ph[hash].filesz) {
			LOG("boot: hash segment %s missing", path);
			return -1;
		}
		memcpy(meta + ehsz, h, ph[hash].filesz);
		free(h);
	}
	/*
	 * Relocation (Linux qcom_mdt_load): if any segment has QCOM_MDT_RELOCATABLE (flags bit 27), the image goes at
	 * the region's base, every segment at base + (paddr - lowest paddr). Then every segment must fit the region,
	 * checked before anything is asked of TrustZone.
	 */
	uint64_t min_pa = ~0ull, max_pa = 0;
	int reloc = 0;
	for (uint32_t i = 0; i < phnum; i++) {
		if (ph[i].type != 1 || ((ph[i].flags >> 24) & 7) == 2 || !ph[i].memsz)
			continue;
		if (ph[i].flags & (1u << 27))
			reloc = 1;
		if (ph[i].paddr < min_pa)
			min_pa = ph[i].paddr;
		if (ph[i].paddr + (uint64_t)ph[i].memsz > max_pa)
			max_pa = ph[i].paddr + (uint64_t)ph[i].memsz;
	}
	int64_t shift = reloc ? (int64_t)ADSP_PA - (int64_t)min_pa : 0;
	LOG("boot: image %llx-%llx (%llu KB), %s", (unsigned long long)min_pa, (unsigned long long)max_pa,
	    (unsigned long long)((max_pa - min_pa) >> 10), reloc ? "relocatable: placed at the region's base" : "fixed");
	for (uint32_t i = 0; i < phnum; i++) {
		if (ph[i].type != 1 || (int)i == hash || !ph[i].memsz)
			continue;
		uint64_t pa = (uint64_t)((int64_t)ph[i].paddr + shift);
		if (pa < ADSP_PA || pa + (uint64_t)ph[i].memsz > ADSP_PA + ADSP_SIZE) {
			LOG("boot: segment %u at %llx+%x outside the region %llx+%llx", i, (unsigned long long)pa, ph[i].memsz,
			    ADSP_PA, ADSP_SIZE);
			return -1;
		}
	}
	uint64_t meta_pa = pm_alloc(meta_len, 0x1000);
	if (!meta_pa)
		return -1;
	pm_wr(meta_pa, meta, meta_len);
	uint64_t r1 = 0;
	int64_t rc = scm(1, 2 | (2u << 6), PAS_ADSP, meta_pa, 0, &r1);
	LOG("boot: init image (%zu bytes of metadata, %u segments) -> %lld/%llx", meta_len, phnum, (long long)rc,
	    (unsigned long long)r1);
	pm_free(meta_pa);
	if (rc || r1)
		return -1;
	rc = scm(2, 3, PAS_ADSP, ADSP_PA, ADSP_SIZE, &r1);
	LOG("boot: mem setup %llx+%llx -> %lld/%llx", ADSP_PA, ADSP_SIZE, (long long)rc, (unsigned long long)r1);
	if (rc || r1)
		return -1;
	uint64_t total = 0;
	static uint8_t zeros[0x10000];
	for (uint32_t i = 0; i < phnum; i++) {
		if (ph[i].type != 1 || (int)i == hash || !ph[i].memsz)
			continue;
		uint64_t pa = (uint64_t)((int64_t)ph[i].paddr + shift);
		if (ph[i].filesz) {
			char path[128];
			size_t sl;
			snprintf(path, sizeof(path), FW_DIR "/adsp.b%02u", i);
			uint8_t *seg = slurp(path, &sl);
			if (!seg || sl < ph[i].filesz) {
				LOG("boot: %s missing or short", path);
				return -1;
			}
			pm_wr(pa, seg, (ph[i].filesz + 3) & ~3u);
			free(seg);
			total += ph[i].filesz;
		}
		for (uint64_t o = (ph[i].filesz + 3) & ~3ull; o < ph[i].memsz; o += sizeof(zeros)) {
			uint64_t n = ph[i].memsz - o < sizeof(zeros) ? ph[i].memsz - o : sizeof(zeros);
			pm_wr(pa + o, zeros, (n + 3) & ~3ull);
		}
	}
	LOG("boot: %llu bytes of segments loaded", (unsigned long long)total);
	qmp_send("{class: image, res: load_state, name: adsp, val: on}");
	uint64_t ve;
	if (smp2p_in_entry("slave-kernel", &ve) == 0)
		wr32(ve, 0);
	adsp_state = 0xffffffff;
	rc = scm(5, 1, PAS_ADSP, 0, 0, &r1);
	LOG("boot: auth and reset -> %lld/%llx", (long long)rc, (unsigned long long)r1);
	if (rc || r1)
		return -1;
	for (int t = 0; t < 150; t++) {
		uint64_t va;
		smp2p_service();
		if (smp2p_in_entry("slave-kernel", &va) == 0 && (rd32(va) & 2) && !(rd32(va) & 1)) {
			LOG("boot: ADSP READY after %d00 ms", t);
			return 0;
		}
		usleep(100000);
	}
	LOG("boot: no ready from the ADSP in 15 s");
	return -1;
}

/* ---- GLINK over SMEM on the "lpass" edge, several channels, polled ---- */

#define G_VERSION	0
#define G_VERSION_ACK	1
#define G_OPEN		2
#define G_CLOSE		3
#define G_OPEN_ACK	4
#define G_INTENT	5
#define G_RX_DONE	6
#define G_RX_INTENT_REQ	7
#define G_RX_INTENT_REQ_ACK 8
#define G_TX_DATA	9
#define G_CLOSE_ACK	11
#define G_TX_DATA_CONT	12
#define G_READ_NOTIF	13
#define G_RX_DONE_W_REUSE 14
#define G_SIGNALS	15
#define FEAT_INTENT_REUSE 1

static uint64_t g_desc, g_tx, g_rx;
static uint32_t g_txlen, g_rxlen;
#define TX_TAIL	(g_desc + 0)
#define TX_HEAD	(g_desc + 4)
#define RX_TAIL	(g_desc + 8)
#define RX_HEAD	(g_desc + 12)

static int glink_init(void)
{
	if (smem_alloc(HOST_ADSP, 478, 32, &g_desc) || smem_alloc(HOST_ADSP, 479, 16384, &g_tx)) {
		LOG("glink: smem items 478/479");
		return -1;
	}
	uint32_t sz;
	if (smem_get(HOST_ADSP, 479, &g_tx, &g_txlen) || smem_get(HOST_ADSP, 480, &g_rx, &g_rxlen)) {
		LOG("glink: the ADSP has no rx fifo (item 480)");
		return -1;
	}
	(void)sz;
	wr32(RX_TAIL, 0);
	wr32(TX_HEAD, 0);
	LOG("glink: tx fifo %llx (%u), rx fifo %llx (%u)", (unsigned long long)g_tx, g_txlen, (unsigned long long)g_rx,
	    g_rxlen);
	return 0;
}

static int g_send(uint16_t cmd, uint16_t p1, uint32_t p2, const void *data, size_t len)
{
	uint8_t buf[8 + 8192 + 16] = { 0 };
	size_t n = 8 + len, padded = (n + 7) & ~(size_t)7;
	if (padded > sizeof(buf))
		return -1;
	memcpy(buf, &cmd, 2);
	memcpy(buf + 2, &p1, 2);
	memcpy(buf + 4, &p2, 4);
	if (len)
		memcpy(buf + 8, data, len);
	uint32_t head = rd32(TX_HEAD), tail = rd32(TX_TAIL);
	uint32_t avail = tail <= head ? g_txlen - head + tail : tail - head;
	if (avail < padded + 16)
		return -1;
	for (size_t i = 0; i < padded; i += 4) {
		uint32_t w;
		memcpy(&w, buf + i, 4);
		wr32(g_tx + head, w);
		head += 4;
		if (head >= g_txlen)
			head -= g_txlen;
	}
	__asm__ volatile("dsb sy" ::: "memory");
	wr32(TX_HEAD, head);
	kick(IPC_GLINK);
	return 0;
}

static size_t g_avail(void)
{
	uint32_t head = rd32(RX_HEAD), tail = rd32(RX_TAIL);
	return head < tail ? g_rxlen - tail + head : head - tail;
}

static void g_peek(void *buf, size_t off, size_t len)
{
	uint32_t t = rd32(RX_TAIL) + (uint32_t)off;
	if (t >= g_rxlen)
		t -= g_rxlen;
	uint8_t *b = buf;
	for (size_t i = 0; i < len; i++) {
		uint32_t w = rd32(g_rx + (t & ~3u));
		b[i] = (uint8_t)(w >> (8 * (t & 3)));
		if (++t >= g_rxlen)
			t = 0;
	}
}

static void g_advance(size_t len)
{
	uint32_t t = rd32(RX_TAIL) + (uint32_t)len;
	if (t >= g_rxlen)
		t -= g_rxlen;
	wr32(RX_TAIL, t);
}

typedef void (*rx_fn)(const uint8_t *d, uint32_t n);
#define MAX_RINT 64
struct chan {
	const char *name;
	uint16_t lcid;
	uint32_t rcid;
	int remote_open, open_sent, open_acked;
	rx_fn rx;
	struct { uint32_t size, iid; int busy; } rint[MAX_RINT];
	int nrint;
	int intent_req_acked;
	uint8_t buf[32768];
	uint32_t off;
};

static void qrtr_rx(const uint8_t *d, uint32_t n);
static void apr_rx(const uint8_t *d, uint32_t n);
static void frpc_rx(const uint8_t *d, uint32_t n);
static struct chan chans[] = {
	{ .name = "IPCRTR", .lcid = 1, .rx = qrtr_rx },
	{ .name = "apr_audio_svc", .lcid = 2, .rx = apr_rx },
	{ .name = "fastrpcglink-apps-dsp", .lcid = 3, .rx = frpc_rx },
};
#define NCHAN	(sizeof(chans) / sizeof(chans[0]))
static int g_version_done;

static struct chan *by_rcid(uint32_t rcid)
{
	for (unsigned i = 0; i < NCHAN; i++)
		if (chans[i].remote_open && chans[i].rcid == rcid)
			return &chans[i];
	return NULL;
}

static void ch_open(struct chan *c)
{
	g_send(G_OPEN, c->lcid, (uint32_t)strlen(c->name) + 1, c->name, strlen(c->name) + 1);
	c->open_sent = 1;
}

static void ch_intents(struct chan *c)
{
	/* ours for the ADSP to write into: as the DT's qcom,intents for IPCRTR, and some for APR */
	uint32_t p[16], n = 0, iid = c->lcid * 100;
	for (int i = 0; i < 5; i++) { p[2 * n] = 0x800; p[2 * n + 1] = iid++; n++; }
	for (int i = 0; i < 3; i++) { p[2 * n] = 0x2000; p[2 * n + 1] = iid++; n++; }
	g_send(G_INTENT, c->lcid, n, p, 8 * n);
}

static int rx_one(void)
{
	size_t avail = g_avail();
	if (avail < 8)
		return 0;
	uint8_t h[16];
	g_peek(h, 0, 8);
	uint16_t cmd, p1;
	uint32_t p2;
	memcpy(&cmd, h, 2);
	memcpy(&p1, h + 2, 2);
	memcpy(&p2, h + 4, 4);
	size_t adv = 8;
	struct chan *c;

	switch (cmd) {
	case G_VERSION:
		LOG("glink: < VERSION %u features %#x", p1, p2);
		g_send(G_VERSION_ACK, 1, p2 & FEAT_INTENT_REUSE, NULL, 0);
		if (!g_version_done)
			g_send(G_VERSION, 1, FEAT_INTENT_REUSE, NULL, 0);
		break;
	case G_VERSION_ACK:
		LOG("glink: < VERSION_ACK %u features %#x", p1, p2);
		g_version_done = 1;
		break;
	case G_OPEN: {
		size_t nl = (p2 + 7) & ~7u;
		if (avail < 8 + nl)
			return 0;
		char name[64] = { 0 };
		g_peek(name, 8, p2 < 63 ? p2 : 63);
		adv += nl;
		LOG("glink: < OPEN rcid %u \"%s\"", p1, name);
		g_send(G_OPEN_ACK, p1, 0, NULL, 0);
		for (unsigned i = 0; i < NCHAN; i++)
			if (!strcmp(name, chans[i].name)) {
				chans[i].rcid = p1;
				chans[i].remote_open = 1;
				if (!chans[i].open_sent)
					ch_open(&chans[i]);
			}
		break;
	}
	case G_OPEN_ACK:
		for (unsigned i = 0; i < NCHAN; i++)
			if (chans[i].lcid == p1 && !chans[i].open_acked) {
				chans[i].open_acked = 1;
				LOG("glink: \"%s\" open both ways", chans[i].name);
				ch_intents(&chans[i]);
			}
		break;
	case G_CLOSE:
		LOG("glink: < CLOSE rcid %u", p1);
		g_send(G_CLOSE_ACK, p1, 0, NULL, 0);
		if ((c = by_rcid(p1)) != NULL) {
			c->remote_open = 0;
			c->nrint = 0;
		}
		break;
	case G_CLOSE_ACK:
		break;
	case G_INTENT: {
		size_t len = 8 + (size_t)p2 * 8;
		if (avail < len)
			return 0;
		c = by_rcid(p1);
		for (uint32_t i = 0; i < p2; i++) {
			uint32_t pr[2];
			g_peek(pr, 8 + i * 8, 8);
			if (c && c->lcid == 2)
				LOG("glink: < INTENT on \"%s\": iid %u size %u", c->name, pr[1], pr[0]);
			if (c && c->nrint < MAX_RINT) {
				c->rint[c->nrint].size = pr[0];
				c->rint[c->nrint].iid = pr[1];
				c->rint[c->nrint].busy = 0;
				c->nrint++;
			}
		}
		adv = (len + 7) & ~7u;
		break;
	}
	case G_RX_DONE:
	case G_RX_DONE_W_REUSE:
		if ((c = by_rcid(p1)) == NULL)
			break;
		if (c->lcid == 2)
			LOG("glink: < RX_DONE%s on \"%s\" for iid %u", cmd == G_RX_DONE ? "" : "_W_REUSE", c->name, p2);
		for (int i = 0; i < c->nrint; i++)
			if (c->rint[i].iid == p2) {
				if (cmd == G_RX_DONE)
					c->rint[i] = c->rint[--c->nrint];
				else
					c->rint[i].busy = 0;
				break;
			}
		break;
	case G_RX_INTENT_REQ:
		if ((c = by_rcid(p1)) != NULL) {
			static uint32_t next = 1000;
			uint32_t p[2] = { p2 > 0x2000 ? p2 : 0x2000, next++ };
			g_send(G_INTENT, c->lcid, 1, p, sizeof(p));
			g_send(G_RX_INTENT_REQ_ACK, c->lcid, 1, NULL, 0);
		} else {
			g_send(G_RX_INTENT_REQ_ACK, 0, 0, NULL, 0);
		}
		break;
	case G_RX_INTENT_REQ_ACK:
		for (unsigned i = 0; i < NCHAN; i++)
			if (chans[i].rcid == p1)
				chans[i].intent_req_acked = 1;
		break;
	case G_TX_DATA:
	case G_TX_DATA_CONT: {
		if (avail < 16)
			return 0;
		g_peek(h, 0, 16);
		uint32_t chunk, left;
		memcpy(&chunk, h + 8, 4);
		memcpy(&left, h + 12, 4);
		size_t len = 16 + chunk;
		if (avail < len)
			return 0;
		adv = (len + 7) & ~7u;
		c = by_rcid(p1);
		if (!c) {
			LOG("glink: < %u bytes on unknown rcid %u", chunk, p1);
			break;
		}
		if (cmd == G_TX_DATA)
			c->off = 0;
		if (c->off + chunk <= sizeof(c->buf))
			g_peek(c->buf + c->off, 16, chunk);
		c->off += chunk;
		if (!left) {
			g_advance(adv);
			uint32_t n = c->off;
			c->off = 0;
			g_send(G_RX_DONE_W_REUSE, c->lcid, p2, NULL, 0);
			if (n <= sizeof(c->buf))
				c->rx(c->buf, n);
			return 1;
		}
		break;
	}
	case G_READ_NOTIF:
		kick(IPC_GLINK);
		break;
	case G_SIGNALS:
		break;
	default:
		LOG("glink: < unknown cmd %u (%u %#x): dropping the FIFO", cmd, p1, p2);
		adv = avail;
		break;
	}
	g_advance(adv);
	return 1;
}

static void frpc_service(void);

static int poll_for(unsigned ms)
{
	uint64_t end = now_ms() + ms;
	int n = 0;
	do {
		while (rx_one())
			n++;
		smp2p_service();
		frpc_service();
		if (ms)
			usleep(1000);
	} while (now_ms() < end);
	return n;
}

static int ch_send(struct chan *c, const void *data, uint32_t len)
{
	uint64_t end = now_ms() + 3000;
	int best, asked = 0;
	for (;;) {
		best = -1;
		for (int i = 0; i < c->nrint; i++)
			if (!c->rint[i].busy && c->rint[i].size >= len && (best < 0 || c->rint[i].size < c->rint[best].size))
				best = i;
		if (best >= 0)
			break;
		if (!asked || c->intent_req_acked) {
			if (asked++ > 3)
				goto fail;
			c->intent_req_acked = 0;
			g_send(G_RX_INTENT_REQ, c->lcid, len, NULL, 0);
		}
		if (now_ms() > end)
			goto fail;
		poll_for(1);
	}
	c->rint[best].busy = 1;
	if (c->lcid == 2)
		LOG("glink: > %u bytes on \"%s\" in its intent %u (size %u; %d intents)", len, c->name, c->rint[best].iid,
		    c->rint[best].size, c->nrint);
	const uint8_t *d = data;
	for (uint32_t off = 0; off < len;) {
		uint32_t chunk = len - off > 8192 ? 8192 : len - off;
		uint8_t b[8 + 8192];
		uint32_t hdr[2] = { chunk, len - off - chunk };
		memcpy(b, hdr, 8);
		memcpy(b + 8, d + off, chunk);
		while (g_send(off ? G_TX_DATA_CONT : G_TX_DATA, c->lcid, c->rint[best].iid, b, 8 + chunk)) {
			if (now_ms() > end)
				goto fail;
			poll_for(1);
		}
		off += chunk;
	}
	return 0;
fail:
	LOG("glink: cannot send %u bytes on \"%s\" (%d intents)", len, c->name, c->nrint);
	return -1;
}

/* ---- QRTR on IPCRTR, and pd-mapper for the ADSP's protection domains ---- */

#define QRTR_TYPE_DATA		1
#define QRTR_TYPE_HELLO		2
#define QRTR_TYPE_BYE		3
#define QRTR_TYPE_NEW_SERVER	4
#define QRTR_TYPE_DEL_SERVER	5
#define QRTR_TYPE_DEL_CLIENT	6
#define QRTR_TYPE_RESUME_TX	7
#define QRTR_TYPE_NEW_LOOKUP	10
#define QRTR_NODE_APPS		1
#define QRTR_PORT_CTRL		0xfffffffeu
#define PDM_SERVICE		0x40
#define PDM_PORT		0x4000

struct qrtr_hdr { uint32_t version, type, src_node, src_port, confirm_rx, size, dst_node, dst_port; };
static uint32_t adsp_node = 5;
static int qrtr_hello_done;

static int qrtr_send(uint32_t type, uint32_t src_port, uint32_t dst_node, uint32_t dst_port, const void *p, uint32_t len)
{
	static uint8_t b[16384 + 64];
	struct qrtr_hdr h = { 1, type, QRTR_NODE_APPS, src_port, 0, len, dst_node, dst_port };
	uint32_t padded = (len + 3) & ~3u;
	if (sizeof(h) + padded > sizeof(b))
		return -1;
	memcpy(b, &h, sizeof(h));
	memcpy(b + sizeof(h), p, len);
	memset(b + sizeof(h) + len, 0, padded - len);
	return ch_send(&chans[0], b, sizeof(h) + padded);
}

static const struct { const char *service, *domain; uint32_t instance; } pdm_db[] = {
	{ "tms/servreg", "msm/adsp/root_pd", 74 },
	{ "tms/servreg", "msm/adsp/sensor_pd", 74 },
	{ "tms/servreg", "msm/adsp/audio_pd", 74 },
	{ "avs/audio", "msm/adsp/audio_pd", 74 },
};

static void pdm_handle(const struct qrtr_hdr *h, const uint8_t *d, uint32_t n)
{
	struct qmi_msg m;
	struct qmi_buf q;
	if (qmi_parse(d, n, &m) || m.type != QMI_REQUEST)
		return;
	qmi_start(&q, QMI_RESPONSE, m.txn, m.id);
	if (m.id == 0x24) {
		uint16_t l1 = 0, l2 = 0;
		const uint8_t *svc = qmi_get(&m, 1, &l1), *why = qmi_get(&m, 2, &l2);
		LOG("pd-mapper: PD FAILURE \"%.*s\": %.*s", svc ? l1 : 0, svc ? (const char *)svc : "", why ? l2 : 0,
		    why ? (const char *)why : "");
		qmi_tlv_result(&q, 0, 0);
	} else if (m.id != 0x21) {
		qmi_dump(&m);
		qmi_tlv_result(&q, 1, 0x47);
	} else {
		uint16_t l = 0;
		const uint8_t *p = qmi_get(&m, 1, &l);
		char name[80] = { 0 };
		uint32_t offset = 0;
		if (p)
			memcpy(name, p, l < 79 ? l : 79);
		qmi_get_u32(&m, 0x10, &offset);
		uint8_t list[512];
		size_t o = 1;
		uint16_t total = 0;
		list[0] = 0;
		for (unsigned i = 0; i < sizeof(pdm_db) / sizeof(pdm_db[0]); i++) {
			if (strcmp(pdm_db[i].service, name))
				continue;
			if (total++ < offset)
				continue;
			uint8_t nl = (uint8_t)strlen(pdm_db[i].domain);
			list[o++] = nl;
			memcpy(list + o, pdm_db[i].domain, nl);
			o += nl;
			memcpy(list + o, &pdm_db[i].instance, 4);
			o += 4;
			list[o++] = 0;
			memset(list + o, 0, 4);
			o += 4;
			list[0]++;
		}
		LOG("pd-mapper: \"%s\" -> %u domain(s)", name, total);
		qmi_tlv_result(&q, 0, 0);
		qmi_tlv(&q, 0x10, &total, 2);
		uint16_t rev = 1;
		qmi_tlv(&q, 0x11, &rev, 2);
		qmi_tlv(&q, 0x12, list, (uint16_t)o);
	}
	qrtr_send(QRTR_TYPE_DATA, PDM_PORT, h->src_node, h->src_port, q.b, (uint32_t)qmi_finish(&q));
}

/*
 * The service registry's notifier (service 0x42, Linux drivers/soc/qcom/pdr_interface.c): which state a protection
 * domain is in. REGISTER_LISTENER (0x20: enable, service path) answers with the current state; a change comes as
 * STATE_UPDATED (0x22) indications, each acked with SET_ACK (0x23), as Android's audio notifier does.
 */
#define SRVREG_CLIENT_PORT	0x4002
static uint32_t srvreg_node, srvreg_port;

static const char *pd_state(uint32_t s)
{
	return s == 0x1fffffff ? "UP" : s == 0x0fffffff ? "DOWN" : s == 0x2fffffff ? "EARLY DOWN" :
	       s == 0x7fffffff ? "UNINITIALISED" : "?";
}

static void srvreg_query(const char *path)
{
	static uint16_t txn = 1;
	struct qmi_buf q;
	if (!srvreg_port) {
		LOG("pdr: the service registry has not shown up");
		return;
	}
	qmi_start(&q, QMI_REQUEST, txn++, 0x20);
	qmi_tlv_u8(&q, 0x01, 1);
	qmi_tlv(&q, 0x02, path, (uint16_t)strlen(path));
	qrtr_send(QRTR_TYPE_DATA, SRVREG_CLIENT_PORT, srvreg_node, srvreg_port, q.b, (uint32_t)qmi_finish(&q));
}

static void srvreg_handle(const struct qrtr_hdr *h, const uint8_t *d, uint32_t n)
{
	struct qmi_msg m;
	uint32_t st = 0;
	uint16_t l = 0;
	if (qmi_parse(d, n, &m))
		return;
	if (m.type == QMI_RESPONSE && m.id == 0x20) {
		const uint8_t *r = qmi_get(&m, 0x02, &l);
		uint16_t res = r && l >= 4 ? (uint16_t)(r[0] | r[1] << 8) : 0xffff, err = r && l >= 4 ? (uint16_t)(r[2] | r[3] << 8) : 0;
		int has = qmi_get_u32(&m, 0x10, &st) == 0;
		LOG("pdr: registered: result %u error %#x, state %#x %s", res, err, st, has ? pd_state(st) : "(none given)");
	} else if (m.type == QMI_INDICATION && m.id == 0x22) {
		char path[80] = "";
		uint16_t txn = 0;
		const uint8_t *pp = qmi_get(&m, 0x02, &l), *tp;
		qmi_get_u32(&m, 0x01, &st);
		if (pp)
			memcpy(path, pp, l < sizeof(path) - 1 ? l : sizeof(path) - 1);
		if ((tp = qmi_get(&m, 0x03, &l)) && l >= 2)
			txn = (uint16_t)(tp[0] | tp[1] << 8);
		LOG("pdr: \"%s\" is now %s (%#x): acked", path, pd_state(st), st);
		struct qmi_buf q;
		qmi_start(&q, QMI_REQUEST, 0x100 + txn, 0x23);
		qmi_tlv(&q, 0x01, path, (uint16_t)strlen(path));
		qmi_tlv(&q, 0x02, &txn, 2);
		qrtr_send(QRTR_TYPE_DATA, SRVREG_CLIENT_PORT, h->src_node, h->src_port, q.b, (uint32_t)qmi_finish(&q));
	} else if (m.type != QMI_RESPONSE || m.id != 0x23) {
		LOG("pdr: message type %u id %#x", m.type, m.id);
	}
}

static void qrtr_rx(const uint8_t *d, uint32_t n)
{
	struct qrtr_hdr h;
	if (n < sizeof(h))
		return;
	memcpy(&h, d, sizeof(h));
	if (h.version != 1)
		return;
	const uint8_t *p = d + sizeof(h);
	uint32_t len = n - sizeof(h) < h.size ? n - (uint32_t)sizeof(h) : h.size;
	uint32_t w[5] = { 0 };
	memcpy(w, p, len < sizeof(w) ? len : sizeof(w));
	switch (h.type) {
	case QRTR_TYPE_HELLO: {
		adsp_node = h.src_node;
		LOG("qrtr: < HELLO from node %u", h.src_node);
		uint32_t hello[5] = { QRTR_TYPE_HELLO };
		qrtr_send(QRTR_TYPE_HELLO, QRTR_PORT_CTRL, h.src_node, QRTR_PORT_CTRL, hello, sizeof(hello));
		uint32_t srv[5] = { QRTR_TYPE_NEW_SERVER, PDM_SERVICE, 1 | (1u << 8), QRTR_NODE_APPS, PDM_PORT };
		qrtr_send(QRTR_TYPE_NEW_SERVER, QRTR_PORT_CTRL, h.src_node, QRTR_PORT_CTRL, srv, sizeof(srv));
		qrtr_hello_done = 1;
		break;
	}
	case QRTR_TYPE_NEW_SERVER:
		LOG("qrtr: < NEW_SERVER service %#x version %u instance %u at %u:%#x", w[1], w[2] & 0xff, w[2] >> 8, w[3],
		    w[4]);
		if (w[1] == 0x42) {	/* the service registry's notifier (PDR) */
			srvreg_node = w[3];
			srvreg_port = w[4];
		}
		break;
	case QRTR_TYPE_DEL_SERVER:
		LOG("qrtr: < DEL_SERVER service %#x at %u:%#x", w[1], w[3], w[4]);
		break;
	case QRTR_TYPE_NEW_LOOKUP:
		LOG("qrtr: < NEW_LOOKUP service %#x instance %#x", w[1], w[2]);
		break;
	case QRTR_TYPE_DATA:
		if (h.dst_port == PDM_PORT)
			pdm_handle(&h, p, len);
		else if (h.dst_port == SRVREG_CLIENT_PORT)
			srvreg_handle(&h, p, len);
		else
			LOG("qrtr: < DATA %u bytes from %u:%#x to port %#x", len, h.src_node, h.src_port, h.dst_port);
		if (h.confirm_rx) {
			uint32_t pkt[5] = { QRTR_TYPE_RESUME_TX, h.dst_node, h.dst_port, 0, 0 };
			qrtr_send(QRTR_TYPE_RESUME_TX, h.dst_port, h.src_node, h.src_port, pkt, sizeof(pkt));
		}
		break;
	default:
		LOG("qrtr: < type %u from %u:%#x", h.type, h.src_node, h.src_port);
		break;
	}
}

/* ---- APR (Linux sound/soc/qcom/qdsp6/apr.c, q6core.c) ---- */

#define APR_DOMAIN_ADSP		4
#define APR_DOMAIN_APPS		5
#define APR_SVC_CORE		3
#define APR_HDR_FIELD		((1u << 8) | (5u << 4))	/* sequenced command, 5-word header, version 0 */
#define APR_BASIC_RSP		0x000110e8u
#define AVCS_GET_VERSIONS	0x00012905u
#define AVCS_GET_VERSIONS_RSP	0x00012906u
#define AVCS_GET_STATE		0x0001290cu
#define AVCS_GET_STATE_RSP	0x0001290du

struct apr_hdr {
	uint16_t hdr_field, pkt_size;
	uint8_t src_svc, src_domain;
	uint16_t src_port;
	uint8_t dest_svc, dest_domain;
	uint16_t dest_port;
	uint32_t token, opcode;
} __attribute__((packed));

static struct { int got; uint32_t opcode, token, len; uint32_t w[256]; } apr_ans;
static volatile sig_atomic_t quit;
static int apr_event(const struct apr_hdr *h, const uint32_t *w, uint32_t len);	/* the stream's events (tone) */

static void apr_rx(const uint8_t *d, uint32_t n)
{
	struct apr_hdr h;
	if (n < sizeof(h))
		return;
	memcpy(&h, d, sizeof(h));
	uint32_t hl = ((h.hdr_field >> 4) & 0xf) * 4, len = n > hl ? n - hl : 0;
	{
		uint32_t ev[8] = { 0 };
		memcpy(ev, d + hl, len < sizeof(ev) ? len : sizeof(ev));
		if (apr_event(&h, ev, len))
			return;
	}
	apr_ans.opcode = h.opcode;
	apr_ans.token = h.token;
	apr_ans.len = len;
	memset(apr_ans.w, 0, sizeof(apr_ans.w));
	memcpy(apr_ans.w, d + hl, len < sizeof(apr_ans.w) ? len : sizeof(apr_ans.w));
	apr_ans.got = 1;
	LOG("apr: < svc %u dom %u port %u opcode %#x token %u, %u bytes: %08x %08x %08x", h.src_svc, h.src_domain,
	    h.src_port, h.opcode, h.token, len, apr_ans.w[0], apr_ans.w[1], apr_ans.w[2]);
}

/* One APR packet to service `svc', port `port' (ours is the same port: the answer comes back to it). */
static int apr_send(uint8_t svc, uint16_t port, uint32_t token, uint32_t opcode, const void *payload, uint32_t plen)
{
	uint8_t b[512];
	struct apr_hdr h = { APR_HDR_FIELD, (uint16_t)(sizeof(h) + plen), svc, APR_DOMAIN_APPS, port,
			     svc, APR_DOMAIN_ADSP, port, token, opcode };
	if (sizeof(h) + plen > sizeof(b))
		return -1;
	memcpy(b, &h, sizeof(h));
	if (plen)
		memcpy(b + sizeof(h), payload, plen);
	return ch_send(&chans[1], b, sizeof(h) + plen);
}

static int apr_ask_port(uint8_t svc, uint16_t port, uint32_t opcode, const void *payload, uint32_t plen, unsigned wait_ms)
{
	static uint32_t token = 1;
	apr_ans.got = 0;
	if (apr_send(svc, port, token++ & 0x7fffffffu, opcode, payload, plen))
		return -1;
	uint64_t end = now_ms() + wait_ms;
	while (now_ms() < end && !apr_ans.got)
		poll_for(1);
	return apr_ans.got ? 0 : -1;
}

static int apr_ask(uint8_t svc, uint32_t opcode, const void *payload, uint32_t plen, unsigned wait_ms)
{
	return apr_ask_port(svc, 0, opcode, payload, plen, wait_ms);
}

/* ---- stage 2: the speakers' path, with a test tone (the amplifiers stay off) ----
 *
 * Linux sound/soc/qcom/qdsp6 (q6afe.c, q6adm.c, q6asm.c) and HackiOS drivers/audio/hakios.sound.c (APSL 2.0),
 * with surya's numbers (live.dts, the stock kernel's dmesg):
 *   - the two TAS2562 amplifiers sit on the TERTIARY MI2S: sm6150-asoc-snd, qcom,msm-mi2s-master (the SoC
 *     makes the clocks), msm-dai-q6-mi2s-tert rx-lines 1 (SD0 out), tx-lines 2 (SD1: their I/V sense back);
 *     pins ter_mi2s_*_active: GPIO 53 sck, 54 ws, 55 sd0, 56 sd1, function ter_mi2s (1 on all four,
 *     pinctrl-sm7150.c), 8 mA, no pull; TLMM WEST tile 0x03500000
 *   - AFE port 0x1004 (TERTIARY_MI2S_RX), bit clock LPASS clock 0x104 (TER_MI2S_IBIT, attribute couple-no):
 *     16 bits x 2 channels x 48 kHz = 1.536 MHz
 *   - qcom,msm-audio-ion: iommus <&apps_smmu 0x1b21 0>, qcom,smmu-sid-mask 0xf: the DSP takes the low four
 *     bits of the stream from bits 32..35 of the addresses it is given. Here the stream gets an apps SMMU
 *     bank with translation off (as wland's WLAN stream), so those addresses are physical | 1 << 32.
 *   - our buffer: 0xe1000000, RAM below 4 GB that XNU does not manage (between wland's radio pool, which
 *     ends at 0xe1000000, and its rmtfs at 0xe1100000); /dev/pmem maps it uncached: no cache maintenance
 *   - ADM device open v5 to the port (no processing), ASM session 1 stream 1 writing 16-bit PCM, routed to it.
 */
#define APR_SVC_AFE		4
#define APR_SVC_ASM		7
#define APR_SVC_ADM		8

#define TLMM_WEST		0x03500000ull
#define GPIO_CFG(n)		(TLMM_WEST + 0x1000ull * (n))
#define GPIO_IO(n)		(TLMM_WEST + 0x1000ull * (n) + 4)
#define CFG_FUNC1		(1u << 2)
#define CFG_8MA			(3u << 6)
#define CFG_OE			(1u << 9)

#define SMMU_PA			0x15000000ull
#define SMMU_SIZE		0x00100000u
#define AUDIO_SID		0x1b21
#define CB_CBAR			0x0001f000u	/* as wland: S1 context, S2 bypass */
#define CB_CBA2R		0x00000001u
#define CB_SCTLR		0x000000e0u	/* M = 0: no translation */

#define BUF_PA			0xe1000000ull
#define DSP_ADDR(pa)		((pa) | (1ull << 32))	/* the stream's low bits (0x1b21 & 0xf) */
#define PERIODS			8
#define PERIOD_FRAMES		960		/* 20 ms */
#define PERIOD_BYTES		(PERIOD_FRAMES * 4)
#define BUF_BYTES		(PERIODS * 4096)

#define AFE_PORT		0x1004		/* TERTIARY_MI2S_RX */
#define CLK_TER_MI2S_IBIT	0x104
#define CLK_ATTR_COUPLE_NO	1
#define RATE			48000
#define BITS			16

#define AFE_PORT_CMD_DEVICE_START	0x000100e5u
#define AFE_PORT_CMD_DEVICE_STOP	0x000100e6u
#define AFE_PORT_CMD_SET_PARAM_V2	0x000100efu
#define AFE_SVC_CMD_SET_PARAM		0x000100f3u
#define AFE_MODULE_AUDIO_DEV_INTERFACE	0x0001020cu
#define AFE_PARAM_ID_I2S_CONFIG		0x0001020du
#define AFE_MODULE_CLOCK_SET		0x0001028fu
#define AFE_PARAM_ID_CLOCK_SET		0x00010290u

#define ADM_CMD_DEVICE_OPEN_V5		0x00010326u
#define ADM_CMDRSP_DEVICE_OPEN_V5	0x00010329u
#define ADM_CMD_DEVICE_CLOSE_V5		0x00010327u
#define ADM_CMD_MATRIX_MAP_ROUTINGS_V5	0x00010325u
#define ADM_TOPOLOGY_NONE		0x00010312u

#define ASM_CMD_SHARED_MEM_MAP_REGIONS	0x00010d92u
#define ASM_CMDRSP_SHARED_MEM_MAP_REGIONS 0x00010d93u
#define ASM_CMD_SHARED_MEM_UNMAP_REGIONS 0x00010d94u
#define ASM_STREAM_CMD_OPEN_WRITE_V3	0x00010db3u
#define ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2 0x00010d98u
#define ASM_SESSION_CMD_RUN_V2		0x00010daau
#define ASM_DATA_CMD_WRITE_V2		0x00010dabu
#define ASM_DATA_EVENT_WRITE_DONE_V2	0x00010d99u
#define ASM_SESSION_EVENT_RX_UNDERFLOW	0x00010c17u
#define ASM_STREAM_CMD_CLOSE		0x00010bcdu
#define ASM_TOPOLOGY_NONE		0x00010c68u
#define ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V2 0x00010da5u
#define ASM_SHMEM_4K_POOL		3
#define STREAM_PORT			((1u << 8) | 1)	/* session 1, stream 1 */
#define WRITE_TOKEN			0x80000000u

static const char *dsp_error(uint32_t s)
{
	static const char *const n[] = { "ok", "failed", "bad parameter", "unsupported", "version", "unexpected",
		"panic", "no resource", "bad handle", "already", "not ready", "pending", "busy", "aborted", "preempted",
		"continue", "immediate", "not implemented", "need more", "no memory", "not there" };
	return s < sizeof(n) / sizeof(n[0]) ? n[s] : "?";
}

static uint32_t returned, written_done, write_errors, underflows;

static int apr_event(const struct apr_hdr *h, const uint32_t *w, uint32_t len)
{
	if (h->src_svc != APR_SVC_ASM)
		return 0;
	if (h->opcode == ASM_DATA_EVENT_WRITE_DONE_V2 && (h->token & WRITE_TOKEN)) {
		if (len >= 16 && w[3])
			write_errors++;
		written_done++;
		returned |= 1u << (h->token & 31);
		return 1;
	}
	if (h->opcode == APR_BASIC_RSP && (h->token & WRITE_TOKEN)) {
		write_errors++;
		written_done++;
		returned |= 1u << (h->token & 31);
		if (write_errors <= 3)
			LOG("tone: write %u refused: %s", h->token & 31, dsp_error(w[1]));
		return 1;
	}
	if (h->opcode == ASM_SESSION_EVENT_RX_UNDERFLOW) {
		underflows++;
		return 1;
	}
	return 0;
}

/* A command answered with the basic response (0 when status 0), or with opcode `want' (then in apr_ans). */
static int cmd(const char *what, uint8_t svc, uint16_t port, uint32_t opcode, const void *p, uint32_t len, uint32_t want)
{
	if (apr_ask_port(svc, port, opcode, p, len, 1500)) {
		LOG("tone: %s: NO ANSWER in 1.5 s; listening 3 s more", what);
		uint64_t end = now_ms() + 3000;
		while (now_ms() < end && !apr_ans.got)
			poll_for(10);
		LOG("tone: %s: %s (adsp state %x)", what, apr_ans.got ? "a LATE answer came" : "still nothing", adsp_state);
		LOG("tone: fifo: tx head %u tail %u (of %u), rx head %u tail %u (of %u)", rd32(TX_HEAD), rd32(TX_TAIL), g_txlen,
		    rd32(RX_HEAD), rd32(RX_TAIL), g_rxlen);
		return -1;
	}
	if (apr_ans.opcode == APR_BASIC_RSP && apr_ans.w[0] == opcode) {
		LOG("tone: %s: %s%s", what, apr_ans.w[1] ? "REFUSED: " : "done", apr_ans.w[1] ? dsp_error(apr_ans.w[1]) : "");
		return apr_ans.w[1] == 0 && !want ? 0 : -1;
	}
	if (want && apr_ans.opcode == want)
		return 0;
	LOG("tone: %s: unexpected answer %#x: %08x %08x", what, apr_ans.opcode, apr_ans.w[0], apr_ans.w[1]);
	return -1;
}

struct afe_param { uint32_t module, param; uint16_t size, reserved; } __attribute__((packed));

static int afe_svc_param(const char *what, uint32_t module, uint32_t param, const void *data, uint16_t size)
{
	uint8_t pkt[16 + sizeof(struct afe_param) + 64] = { 0 };
	struct afe_param ph = { module, param, size, 0 };
	*(uint32_t *)pkt = (uint32_t)sizeof(ph) + size;		/* in band: address 0, handle 0 */
	memcpy(pkt + 16, &ph, sizeof(ph));
	memcpy(pkt + 16 + sizeof(ph), data, size);
	return cmd(what, APR_SVC_AFE, 0, AFE_SVC_CMD_SET_PARAM, pkt, 16 + (uint32_t)sizeof(ph) + size, 0);
}

static int afe_port_param(const char *what, uint32_t module, uint32_t param, const void *data, uint16_t size)
{
	uint8_t pkt[16 + sizeof(struct afe_param) + 64] = { 0 };
	struct afe_param ph = { module, param, size, 0 };
	*(uint16_t *)pkt = AFE_PORT;
	*(uint16_t *)(pkt + 2) = (uint16_t)(sizeof(ph) + size);
	memcpy(pkt + 16, &ph, sizeof(ph));
	memcpy(pkt + 16 + sizeof(ph), data, size);
	return cmd(what, APR_SVC_AFE, 0, AFE_PORT_CMD_SET_PARAM_V2, pkt, 16 + (uint32_t)sizeof(ph) + size, 0);
}

static int afe_clock(int on)
{
	struct { uint32_t version, id, hz; uint16_t attr, root; uint32_t enable; } __attribute__((packed)) c =
		{ 1, CLK_TER_MI2S_IBIT, RATE * BITS * 2, CLK_ATTR_COUPLE_NO, 0, (uint32_t)on };
	return afe_svc_param(on ? "bit clock 0x104 at 1.536 MHz, on" : "bit clock, off", AFE_MODULE_CLOCK_SET,
			     AFE_PARAM_ID_CLOCK_SET, &c, sizeof(c));
}

/* the pins */
static const unsigned pins[4] = { 53, 54, 55, 56 };
static const uint32_t pin_cfg[4] = { CFG_FUNC1 | CFG_8MA | CFG_OE, CFG_FUNC1 | CFG_8MA | CFG_OE, CFG_FUNC1 | CFG_8MA | CFG_OE,
				     CFG_FUNC1 | CFG_8MA };
static uint32_t pin_old_cfg[4], pin_old_io[4];
static int pins_taken;

static void pins_on(void)
{
	for (int i = 0; i < 4; i++) {
		pin_old_cfg[i] = rd32(GPIO_CFG(pins[i]));
		pin_old_io[i] = rd32(GPIO_IO(pins[i]));
	}
	pins_taken = 1;
	for (int i = 0; i < 4; i++) {
		wr32(GPIO_IO(pins[i]), (pin_cfg[i] & CFG_OE) ? 2 : 0);
		wr32(GPIO_CFG(pins[i]), pin_cfg[i]);
	}
	LOG("tone: pins 53-56 -> ter_mi2s (were cfg %#x %#x %#x %#x)", pin_old_cfg[0], pin_old_cfg[1], pin_old_cfg[2],
	    pin_old_cfg[3]);
}

static void pins_off(void)
{
	if (!pins_taken)
		return;
	for (int i = 0; i < 4; i++) {
		wr32(GPIO_CFG(pins[i]), pin_old_cfg[i]);
		wr32(GPIO_IO(pins[i]), pin_old_io[i] & 2);
	}
	pins_taken = 0;
}

static void pin_activity(unsigned pin, const char *name)
{
	uint32_t last = rd32(GPIO_IO(pin)) & 1, changes = 0, ones = 0, reads = 20000;
	for (uint32_t i = 0; i < reads; i++) {
		uint32_t v = rd32(GPIO_IO(pin)) & 1;
		changes += v != last;
		ones += v;
		last = v;
	}
	LOG("tone: gpio%u (%s): %u changes, %u ones in %u reads", pin, name, changes, ones, reads);
}

/* a stream's SMMU bank (translation off), as wland's smmu_wlan_identity: [0] the audio buffers, [1] FastRPC */
static int smmu_slots[2] = { -1, -1 }, smmu_cbs[2] = { -1, -1 };
static uint32_t smmu_sids[2];
static uint32_t smmu_psize, smmu_npage;

static int smmu_identity(int which, uint32_t sid)
{
	uint64_t s = SMMU_PA;
	uint32_t id0 = rd32(s + 0x20), id1 = rd32(s + 0x24), nsmr = id0 & 0xff, ncb = id1 & 0xff;
	uint64_t used[4] = { 0 };
	int slot = -1, cb = -1;
	smmu_psize = (id1 >> 31) ? 0x10000 : 0x1000;
	smmu_npage = 1u << (((id1 >> 28) & 7) + 1);
	/* never read sCR0/sCR1 on SM7150 (wland radio.c): the access hangs the CPU */
	for (uint32_t i = 0; i < nsmr && i < 128; i++) {
		uint32_t smr = rd32(s + 0x800 + 4 * i), s2cr = rd32(s + 0xc00 + 4 * i);
		if (!(smr >> 31)) {
			if (slot < 0)
				slot = (int)i;
			continue;
		}
		if (((s2cr >> 16) & 3) == 0)
			used[(s2cr & 0xff) / 64] |= 1ull << ((s2cr & 0xff) % 64);
		uint32_t id = smr & 0x7fff, m = (smr >> 16) & 0x7fff;
		if (((id ^ sid) & ~m & 0x7fff) == 0) {
			LOG("smmu: SMR%u %08x already matches %#x (S2CR %08x): left as it is", i, smr, sid, s2cr);
			return ((s2cr >> 16) & 3) == 1 ? 0 : -1;
		}
	}
	for (uint32_t c = 0; c < ncb && c < 256; c++) {
		uint64_t cbp = s + (uint64_t)smmu_npage * smmu_psize + (uint64_t)c * smmu_psize;
		if (!(used[c / 64] & (1ull << (c % 64))) && !(rd32(cbp) & 1) && rd32(s + smmu_psize + 4 * c) == 0) {
			cb = (int)c;
			break;
		}
	}
	if (slot < 0 || cb < 0 || (uint64_t)(smmu_npage + cb + 1) * smmu_psize > SMMU_SIZE) {
		LOG("smmu: no free SMR/bank for %#x (SMR %d, bank %d)", sid, slot, cb);
		return -1;
	}
	uint64_t gr1 = s + smmu_psize, cbp = s + (uint64_t)smmu_npage * smmu_psize + (uint64_t)cb * smmu_psize;
	wr32(cbp + 0x58, 0xffffffff);
	wr32(cbp + 0x20, 0);
	wr32(cbp + 0x24, 0);
	wr32(cbp + 0x30, 0);
	wr32(cbp + 0x10, 0);
	wr32(cbp + 0x38, 0);
	wr32(cbp + 0x3c, 0);
	wr32(gr1 + 0x800 + 4 * cb, CB_CBA2R);
	wr32(gr1 + 4 * cb, CB_CBAR);
	wr32(cbp, CB_SCTLR);
	wr32(s + 0xc00 + 4 * slot, (uint32_t)cb);
	wr32(s + 0x800 + 4 * slot, (1u << 31) | sid);
	wr32(s + 0x70, 0);
	for (int i = 0; i < 1000 && (rd32(s + 0x74) & 1); i++)
		delay_ms(1);
	uint32_t smr = rd32(s + 0x800 + 4 * slot), s2cr = rd32(s + 0xc00 + 4 * slot);
	int ok = smr == ((1u << 31) | sid) && (s2cr & 0x300ff) == (uint32_t)cb;
	LOG("smmu: stream %#x -> CB%d via SMR%d (translation off): SMR %08x S2CR %08x -> %s", sid, cb, slot, smr, s2cr,
	    ok ? "ok" : "NOT as written");
	smmu_slots[which] = slot;
	smmu_cbs[which] = cb;
	smmu_sids[which] = sid;
	return ok ? 0 : -1;
}

static void smmu_report_and_off(int which)
{
	int slot = smmu_slots[which], cb = smmu_cbs[which];
	if (slot < 0)
		return;
	uint64_t s = SMMU_PA, cbp = s + (uint64_t)smmu_npage * smmu_psize + (uint64_t)cb * smmu_psize;
	uint32_t fsr = rd32(cbp + 0x58);	/* fault bits 0xc00001fe; bits 9-10 are only the format */
	LOG("smmu: stream %#x CB%d FSR %#x%s", smmu_sids[which], cb, fsr,
	    (fsr & 0xc00001feu) ? " (FAULTS: the DSP was refused)" : " (no faults)");
	wr32(s + 0x800 + 4 * slot, 0);
	wr32(s + 0xc00 + 4 * slot, 0x20000);	/* fault, as an unused entry */
	wr32(s + smmu_psize + 4 * cb, 0);
	smmu_slots[which] = -1;
}

/* ---- the audio PD: started over FastRPC, as Android's audioadsprpcd audiopd ----
 *
 * The audio services (pd-mapper: "avs/audio" -> msm/adsp/audio_pd) live in a user PD of the ADSP that the
 * HLOS starts: Linux drivers/misc/fastrpc.c fastrpc_init_create_static_process, on the GLINK channel
 * "fastrpcglink-apps-dsp". Without it the root PD answers the core service and AFE's queries, but every
 * command that sets something is never answered. surya (live.dts): qcom,msm_fastrpc qcom,adsp-remoteheap-vmid
 * <22 37> (LPASS, ADSP heap); adsprpc compute CBs iommus 0x1b23 / 0x1b24 (the low four bits of the stream in
 * bits 32-35 of the addresses the DSP is given, as the audio buffers); the remote heap comes from adsp_region
 * (12 MB below 4 GB, 4 MB aligned): here 4 MB at 0xa1800000, RAM below 4 GB that nothing uses (the fixed
 * carve-outs end at 0xa1600000, the GPU pool starts at 0xa2000000). It is given to VMIDs 22 and 37 with
 * TrustZone's MP_ASSIGN (qcom_scm_assign_mem) and stays theirs until the phone restarts.
 */
#define FRPC_SID		0x1b23
#define FRPC_ADDR(pa)		((pa) | (3ull << 32))
#define FRPC_BUF_PA		0xe1080000ull		/* the invoke's buffer, 64 KB in our 1 MB */
#define SCM_ARGS_PA		0xe10c0000ull		/* MP_ASSIGN's tables */
#define HEAP_PA			0xa1800000ull
#define HEAP_SIZE		0x00400000ull
#define VMID_HLOS		3
#define VMID_LPASS		22
#define VMID_ADSP_HEAP		37
#define PERM_RWX		7

static struct { int got; int32_t retval; } frpc_rsp[16];

static void frpc_rx(const uint8_t *d, uint32_t n)
{
	uint64_t ctx;
	int32_t retval;
	if (n < 12) {
		LOG("fastrpc: < %u bytes (short)", n);
		return;
	}
	memcpy(&ctx, d, 8);
	memcpy(&retval, d + 8, 4);
	unsigned id = (unsigned)(ctx >> 4) & 15;
	frpc_rsp[id].retval = retval;
	frpc_rsp[id].got = 1;
	if (id != 2)
		LOG("fastrpc: < response: ctx %#llx retval %d", (unsigned long long)ctx, retval);
}

/* TrustZone MP_ASSIGN (Linux qcom_scm_assign_mem): HLOS -> LPASS + ADSP heap, read/write/exec. */
static int hyp_assign_heap(void)
{
	uint8_t t[192] = { 0 };
	uint64_t mem[2] = { HEAP_PA, HEAP_SIZE };
	uint32_t src[1] = { VMID_HLOS };
	struct { uint32_t vmid, perm; uint64_t ctx; uint32_t ctx_size, unused; } __attribute__((packed)) dst[2] = {
		{ VMID_LPASS, PERM_RWX, 0, 0, 0 }, { VMID_ADSP_HEAP, PERM_RWX, 0, 0, 0 } };
	/* the tables, each 64-byte aligned: the memory at 0, the source VM list at 64, the destinations at 128 */
	memcpy(t, mem, sizeof(mem));
	memcpy(t + 64, src, sizeof(src));
	memcpy(t + 128, dst, sizeof(dst));
	pm_wr(SCM_ARGS_PA, t, sizeof(t));
	/* args: 0 memory table, 1 its size, 2 source list, 3 its size, 4 destination table, 5 its size, 6 flags.
	 * Past the first three, they go in a buffer whose address is x5 (qcom_scm-smc.c, more than 4 args). */
	uint64_t ext[4] = { sizeof(src), SCM_ARGS_PA + 128, sizeof(dst), 0 };
	pm_wr(SCM_ARGS_PA + 0x400, ext, sizeof(ext));
	uint64_t x[8] = { 0x42000c16ull /* SIP, svc MP 0xc, cmd ASSIGN 0x16 */,
			  7u | (1u << 4) | (1u << 8) | (1u << 12) /* RO VAL RO VAL RO VAL VAL */,
			  SCM_ARGS_PA, sizeof(mem), SCM_ARGS_PA + 64, SCM_ARGS_PA + 0x400, 0, 0 };
	pm_smc(x);
	LOG("fastrpc: heap %#llx+%#llx -> VMIDs %u, %u: MP_ASSIGN -> %lld/%llx", (unsigned long long)HEAP_PA,
	    (unsigned long long)HEAP_SIZE, VMID_LPASS, VMID_ADSP_HEAP, (long long)x[0], (unsigned long long)x[1]);
	return x[0] == 0 ? 0 : -1;
}

/* FASTRPC_RMID_INIT_CREATE_STATIC "audiopd" on handle 1 (the init handle), as a kernel invoke. */
static int audiopd_start(void)
{
	struct chan *c = &chans[2];
	if (!(c->open_acked && c->remote_open)) {
		uint64_t end = now_ms() + 2000;
		while (now_ms() < end && !(c->open_acked && c->remote_open))
			poll_for(10);
	}
	if (!(c->open_acked && c->remote_open)) {
		LOG("fastrpc: channel \"%s\" is not open", c->name);
		return -1;
	}
	if (smmu_identity(1, FRPC_SID))
		return -1;
	if (hyp_assign_heap())
		return -1;
	/* the invoke's buffer (fastrpc_get_args): metadata for 3 scalars, then each argument 128-byte aligned */
	enum { NSC = 3, META = (16 + 8 + 16) * NSC + 8 * 16 + 4 * 64 };
	const uint64_t dsp = FRPC_ADDR(FRPC_BUF_PA);
	const char name[] = "audiopd";
	struct { int32_t client_id; uint32_t namelen, pageslen; } inbuf = { (int32_t)getpid(), sizeof(name), 1 };
	struct { uint64_t addr, size; } heap = { HEAP_PA, HEAP_SIZE };
	const struct { const void *p; uint32_t len; } a[NSC] = { { &inbuf, sizeof(inbuf) }, { name, sizeof(name) },
								 { &heap, sizeof(heap) } };
	uint8_t b[1024] = { 0 };
	uint64_t *rpra = (uint64_t *)b;				/* { pv, len } x 3 */
	uint32_t *list = (uint32_t *)(b + 16 * NSC);		/* { num, pgidx } x 3 */
	uint64_t *pages = (uint64_t *)(b + 24 * NSC);		/* { addr, size } x 3 */
	uint32_t off = (META + 127) & ~127u;
	for (int i = 0; i < NSC; i++) {
		off = (off + 127) & ~127u;
		rpra[2 * i] = dsp + off;			/* pv: only its offset in the page matters to the DSP */
		rpra[2 * i + 1] = a[i].len;
		list[2 * i] = 1;
		list[2 * i + 1] = (uint32_t)i;
		pages[2 * i] = (dsp + off) & ~0xfffull;
		pages[2 * i + 1] = 4096;
		memcpy(b + off, a[i].p, a[i].len);
		off += a[i].len;
	}
	uint32_t pkt = off;
	pm_wr(FRPC_BUF_PA, b, sizeof(b));
	struct { int32_t client_id, tid; uint64_t ctx; uint32_t handle, sc; uint64_t addr, size; } __attribute__((packed)) msg = {
		0, (int32_t)getpid(), (1u << 4) | 1 /* ctx id 1, USER_PD */, 1 /* FASTRPC_INIT_HANDLE */,
		(8u << 24) | (3u << 16) /* FASTRPC_SCALARS(INIT_CREATE_STATIC, 3, 0) */, dsp, (pkt + 4095) & ~4095u };
	frpc_rsp[1].got = 0;
	LOG("fastrpc: > create static \"audiopd\": buffer %#llx (%u bytes), heap %#llx+%#llx", (unsigned long long)dsp, pkt,
	    (unsigned long long)HEAP_PA, (unsigned long long)HEAP_SIZE);
	if (ch_send(c, &msg, sizeof(msg)))
		return -1;
	uint64_t end = now_ms() + 5000;
	while (now_ms() < end && !frpc_rsp[1].got)
		poll_for(5);
	if (!frpc_rsp[1].got) {
		LOG("fastrpc: no response in 5 s");
		return -1;
	}
	int32_t rv = frpc_rsp[1].retval;
	LOG("fastrpc: audiopd %s (retval %d)", rv == 0 ? "STARTED" : "REFUSED", rv);
	return rv == 0 ? 0 : -1;
}

/* ---- FastRPC invokes, and the listener: the audio PD's calls back to us (files, logs) ----
 *
 * As quic/fastrpc (BSD-3-Clause): adsp_default_listener.c (open "adsp_default_listener" through the DSP's
 * remotectl, handle 0, and register), listener_android.c (adsp_listener, handle 3: init2, then next2 in a loop:
 * each answer is one call of the DSP's to a module of ours, packed as listener_buf.h; the next next2 carries its
 * result back), apps_remotectl (our handle 0: the DSP opens our modules by name) and apps_std (files). The
 * invoke buffers live in RAM below 4 GB that nothing uses, seen by the DSP through stream 0x1b23 like the
 * audiopd start's: the synchronous calls' at FRPC_BUF_PA, the listener's 4 MB at 0xa1c00000 (behind the remote
 * heap). Files are served read-only from DSP_DIRS (the phone's own /vendor/dsp, copied from backup\dsp.img).
 */
#define AEE_EFAILED_		0x01
#define AEE_EBADPARM_		0x0e
#define AEE_EUNSUPPORTED_	0x14
#define AEE_ERESOURCENOTFOUND_	0x16
#define AEE_ENOSUCHMOD_		0x3e
#define AEE_ENOSUCHFILE_	0x45
#define AEE_EBADSIZE_		0x4d
#define SCALARS(m, in, out)	(((uint32_t)(m) << 24) | ((uint32_t)(in) << 16) | ((uint32_t)(out) << 8))
#define LSN_PA			0xa1c00000ull
#define LSN_SIZE		0x00400000u
#define LSN_IN_CAP		0x00040000u		/* a request's packed input buffers: 256 KB */
#define H_APPS_STD		0x100
#define H_APPS_MEM		0x101
#define H_MSGD			0x102

static const char *const dsp_dirs[] = { "/var/local/fw/dsp/adsp", "/var/local/fw/dsp" };

/* An invoke's buffer at `pa' (fastrpc_get_args): metadata, then each argument 128-byte aligned. Inputs are
 * copied in; outputs get their offsets in outoff[]. Returns the packet's size, 0 when it does not fit. */
static uint32_t frpc_build(uint64_t pa, uint32_t cap, int nin, const void *const *ind, const uint32_t *inl, int nout,
			   const uint32_t *outl, uint32_t *outoff)
{
	int nsc = nin + nout;
	uint32_t meta = (16 + 8 + 16) * (uint32_t)nsc + 8 * 16 + 4 * 64, off = meta;
	uint8_t hdr[16 * 40 + 8 * 16 + 4 * 64] = { 0 };
	uint64_t dsp = FRPC_ADDR(pa);
	if (nsc > 16)
		return 0;
	uint64_t *rpra = (uint64_t *)hdr;
	uint32_t *list = (uint32_t *)(hdr + 16 * nsc);
	uint64_t *pages = (uint64_t *)(hdr + 24 * nsc);
	for (int i = 0; i < nsc; i++) {
		uint32_t len = i < nin ? inl[i] : outl[i - nin];
		list[2 * i + 1] = (uint32_t)i;
		rpra[2 * i + 1] = len;
		if (!len)
			continue;
		off = (off + 127) & ~127u;
		if (off + len > cap)
			return 0;
		uint64_t pv = dsp + off;
		rpra[2 * i] = pv;
		list[2 * i] = 1;
		pages[2 * i] = pv & ~0xfffull;
		pages[2 * i + 1] = (((pv + len - 1) >> 12) - (pv >> 12) + 1) * 4096;
		if (i < nin)
			pm_wr(pa + off, ind[i], len);
		else
			outoff[i - nin] = off;
		off += len;
	}
	pm_wr(pa, hdr, meta);
	return off;
}

static int frpc_send(int id, int tid, uint32_t handle, uint32_t sc, uint64_t pa, uint32_t pkt)
{
	struct { int32_t client_id, tid; uint64_t ctx; uint32_t handle, sc; uint64_t addr, size; } __attribute__((packed)) m = {
		(int32_t)getpid(), tid, ((uint64_t)id << 4) | 1 /* USER_PD */, handle, sc, FRPC_ADDR(pa), (pkt + 4095) & ~4095u };
	frpc_rsp[id].got = 0;
	return ch_send(&chans[2], &m, sizeof(m));
}

/* A synchronous call (context 1): inputs in, outputs copied to out[] (each outl[] bytes). The DSP's result, or -1. */
static int32_t frpc_call(const char *what, uint32_t handle, uint32_t sc, int nin, const void *const *ind, const uint32_t *inl,
			 int nout, const uint32_t *outl, void *const *out)
{
	uint32_t outoff[8] = { 0 };
	uint32_t pkt = frpc_build(FRPC_BUF_PA, 0x10000, nin, ind, inl, nout, outl, outoff);
	if (!pkt || frpc_send(1, (int)getpid(), handle, sc, FRPC_BUF_PA, pkt))
		return -1;
	uint64_t end = now_ms() + 5000;
	while (now_ms() < end && !frpc_rsp[1].got)
		poll_for(2);
	if (!frpc_rsp[1].got) {
		LOG("fastrpc: %s: no response in 5 s", what);
		return -1;
	}
	for (int j = 0; j < nout; j++)
		if (outl[j])
			pm_rd(FRPC_BUF_PA + outoff[j], out[j], outl[j]);
	LOG("fastrpc: %s -> %d", what, frpc_rsp[1].retval);
	return frpc_rsp[1].retval;
}

/* ---- our modules ---- */

static FILE *files[64];
static uint32_t n_requests;

static FILE *file_of(uint32_t f) { return f && f <= 64 ? files[f - 1] : NULL; }

static uint32_t file_add(FILE *fp)
{
	for (uint32_t i = 0; i < 64; i++)
		if (!files[i]) {
			files[i] = fp;
			return i + 1;
		}
	fclose(fp);
	return 0;
}

static const char *base_name(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }

/* Read-only: the name as given (when under /var/local/fw), else its base name in each of dsp_dirs. */
static FILE *dsp_open(const char *name, const char *mode, char *found, size_t flen)
{
	if (strpbrk(mode, "wa+"))
		return NULL;
	if (!strncmp(name, "/var/local/fw/", 14)) {
		FILE *fp = fopen(name, "rb");
		if (fp)
			snprintf(found, flen, "%s", name);
		return fp;
	}
	for (unsigned i = 0; i < sizeof(dsp_dirs) / sizeof(dsp_dirs[0]); i++) {
		snprintf(found, flen, "%s/%s", dsp_dirs[i], base_name(name));
		FILE *fp = fopen(found, "rb");
		if (fp)
			return fp;
	}
	return NULL;
}

/* A string argument: in[i] with at most len bytes, made safe to print */
static void str_arg(char *dst, size_t n, const uint8_t *p, uint32_t len)
{
	size_t k = len < n - 1 ? len : n - 1;
	memcpy(dst, p, k);
	dst[k] = 0;
}

static int32_t mod_remotectl(uint32_t m, uint8_t **in, uint32_t *inl, int nin, uint8_t **out, uint32_t *outl, int nout)
{
	if (m == 0 && nin >= 2 && nout >= 1 && outl[0] >= 8) {	/* open(name, rout handle, rout dlerror, rout nErr) */
		char name[96];
		str_arg(name, sizeof(name), in[1], inl[1]);
		uint32_t h = !strcmp(name, "apps_std") ? H_APPS_STD : !strcmp(name, "apps_mem") ? H_APPS_MEM :
			     !strcmp(name, "adspmsgd_apps") ? H_MSGD : 0;
		uint32_t r[2] = { h, h ? 0u : AEE_ENOSUCHMOD_ };
		memcpy(out[0], r, 8);
		LOG("listener: the DSP opens our \"%s\" -> %s", name, h ? "handle given" : "WE HAVE NO SUCH MODULE");
		return 0;
	}
	if (m == 1 && nout >= 1 && outl[0] >= 4) {			/* close(handle, rout dlerror, rout nErr) */
		uint32_t z = 0;
		memcpy(out[0], &z, 4);
		return 0;
	}
	LOG("listener: remotectl method %u: not handled", m);
	return AEE_EUNSUPPORTED_;
}

static const char *std_name(uint32_t m)
{
	static const char *const n[] = { "fopen", "freopen", "fflush", "fclose", "fread", "fwrite", "fgetpos", "fsetpos",
		"ftell", "fseek", "flen", "rewind", "feof", "ferror", "clearerr", "print_string", "getenv", "setenv",
		"unsetenv", "fopen_with_env", "fgets", "get_search_paths_with_env", "fileExists", "fsync", "fremove",
		"fdopen_decrypt", "opendir", "closedir", "readdir", "mkdir", "rmdir", "stat", "ftrunc", "frename",
		"fopen_fd", "fclose_fd", "fopen_with_env_fd" };
	return m < sizeof(n) / sizeof(n[0]) ? n[m] : "?";
}

static int32_t mod_apps_std(uint32_t m, uint8_t **in, uint32_t *inl, int nin, uint8_t **out, uint32_t *outl, int nout)
{
	uint32_t p[4] = { 0 };
	if (nin >= 1)
		memcpy(p, in[0], inl[0] < 16 ? inl[0] : 16);
	char a[160] = "", b[160] = "", found[200] = "";
	switch (m) {
	case 0:		/* fopen(name, mode, rout FILE) */
	case 19: {	/* fopen_with_env(envvarname, delim, name, mode, rout FILE) */
		int ni = m == 0 ? 1 : 3, mi = ni + 1;
		if (nin <= mi || nout < 1 || outl[0] < 4)
			return AEE_EBADPARM_;
		str_arg(a, sizeof(a), in[ni], inl[ni]);
		str_arg(b, sizeof(b), in[mi], inl[mi]);
		FILE *fp = dsp_open(a, b, found, sizeof(found));
		uint32_t f = fp ? file_add(fp) : 0;
		memcpy(out[0], &f, 4);
		LOG("listener: %s(\"%s\", \"%s\") -> %s", std_name(m), a, b, f ? found : "NOT FOUND");
		return f ? 0 : AEE_ENOSUCHFILE_;
	}
	case 3: {	/* fclose(FILE) */
		FILE *fp = file_of(p[0]);
		if (fp) {
			fclose(fp);
			files[p[0] - 1] = NULL;
		}
		return fp ? 0 : AEE_EBADPARM_;
	}
	case 4: {	/* fread(FILE, rout buf, rout bytesRead, rout bEOF) */
		FILE *fp = file_of(p[0]);
		if (!fp || nout < 2 || outl[0] < 8)
			return AEE_EBADPARM_;
		uint32_t r[2];
		r[0] = (uint32_t)fread(out[1], 1, outl[1], fp);
		r[1] = feof(fp) ? 1 : 0;
		memcpy(out[0], r, 8);
		return 0;
	}
	case 8: {	/* ftell(FILE, rout pos) */
		FILE *fp = file_of(p[0]);
		if (!fp || nout < 1)
			return AEE_EBADPARM_;
		uint32_t pos = (uint32_t)ftell(fp);
		memcpy(out[0], &pos, 4);
		return 0;
	}
	case 9: {	/* fseek(FILE, offset, whence) */
		FILE *fp = file_of(p[0]);
		return fp && fseek(fp, (long)(int32_t)p[1], (int)p[2]) == 0 ? 0 : AEE_EBADPARM_;
	}
	case 10: {	/* flen(FILE, rout uint64) */
		FILE *fp = file_of(p[0]);
		if (!fp || nout < 1 || outl[0] < 8)
			return AEE_EBADPARM_;
		long cur = ftell(fp);
		fseek(fp, 0, SEEK_END);
		uint64_t len = (uint64_t)ftell(fp);
		fseek(fp, cur, SEEK_SET);
		memcpy(out[0], &len, 8);
		return 0;
	}
	case 11: {	/* rewind */
		FILE *fp = file_of(p[0]);
		if (fp)
			rewind(fp);
		return fp ? 0 : AEE_EBADPARM_;
	}
	case 12:	/* feof(FILE, rout bEOF) */
	case 13: {	/* ferror(FILE, rout err) */
		FILE *fp = file_of(p[0]);
		if (!fp || nout < 1)
			return AEE_EBADPARM_;
		uint32_t v = m == 12 ? (feof(fp) ? 1 : 0) : 0;
		memcpy(out[0], &v, 4);
		return 0;
	}
	case 15:	/* print_string(str) */
		if (nin >= 2) {
			str_arg(a, sizeof(a), in[1], inl[1]);
			LOG("dsp says: %s", a);
		}
		return 0;
	case 16: {	/* getenv(name, rout val, rout valLenReq) */
		if (nin < 2 || nout < 2)
			return AEE_EBADPARM_;
		str_arg(a, sizeof(a), in[1], inl[1]);
		const char *v = !strcmp(a, "ADSP_LIBRARY_PATH") ? "/var/local/fw/dsp/adsp;/var/local/fw/dsp" : NULL;
		LOG("listener: getenv(\"%s\") -> %s", a, v ? v : "(not set)");
		if (!v)
			return AEE_ERESOURCENOTFOUND_;
		uint32_t need = (uint32_t)strlen(v) + 1;
		memcpy(out[0], &need, 4);
		if (outl[1])
			snprintf((char *)out[1], outl[1], "%s", v);
		return 0;
	}
	case 22: {	/* fileExists(path, rout bool) */
		if (nin < 2 || nout < 1)
			return AEE_EBADPARM_;
		str_arg(a, sizeof(a), in[1], inl[1]);
		FILE *fp = dsp_open(a, "r", found, sizeof(found));
		uint8_t e = fp ? 1 : 0;
		if (fp)
			fclose(fp);
		memcpy(out[0], &e, 1);
		LOG("listener: fileExists(\"%s\") -> %s", a, e ? "yes" : "no");
		return 0;
	}
	default:
		for (int i = 1; i < nin && i < 4; i++) {
			str_arg(a, sizeof(a), in[i], inl[i]);
			LOG("listener:   arg %d: \"%s\"", i, a);
		}
		LOG("listener: apps_std %s (method %u): NOT HANDLED", std_name(m), m);
		return AEE_EUNSUPPORTED_;
	}
}

/* One call of the DSP's (next2's answer): unpack (listener_buf.h), run, pack the outputs. */
static int32_t lsn_dispatch(uint32_t handle, uint32_t sc, const uint8_t *req, uint32_t reqlen, uint8_t *resp, uint32_t cap,
			    uint32_t *resplen)
{
	int nin = (int)((sc >> 16) & 0xff), nout = (int)((sc >> 8) & 0xff);
	uint32_t m = (sc >> 24) & 0x1f, pos = 0, inl[16] = { 0 }, outl[16] = { 0 };
	uint8_t *in[16] = { 0 }, *out[16] = { 0 };
	int32_t r;
	*resplen = 0;
	if (nin > 16 || nout > 16 || (sc & 0xff))
		return AEE_EBADPARM_;
	for (int i = 0; i < nin; i++) {
		if (pos + 4 > reqlen)
			return AEE_EBADPARM_;
		memcpy(&inl[i], req + pos, 4);
		pos += 4;
		if (inl[i]) {
			pos = (pos + 7) & ~7u;
			if (inl[i] > reqlen - pos)
				return AEE_EBADPARM_;
			in[i] = (uint8_t *)req + pos;
			pos += inl[i];
		}
	}
	for (int j = 0; j < nout; j++) {
		if (pos + 4 > reqlen)
			return AEE_EBADPARM_;
		memcpy(&outl[j], req + pos, 4);
		pos += 4;
	}
	/* where the outputs go in the response (pack_out_bufs), so the modules write them in place */
	uint32_t o = 0;
	for (int j = 0; j < nout; j++) {
		o += 4;
		if (outl[j]) {
			o = (o + 7) & ~7u;
			if (outl[j] > cap || o > cap - outl[j])
				return AEE_EBADSIZE_;
			out[j] = resp + o;
			memset(out[j], 0, outl[j]);
			o += outl[j];
		}
	}
	n_requests++;
	if (handle == 0)
		r = mod_remotectl(m, in, inl, nin, out, outl, nout);
	else if (handle == H_APPS_STD)
		r = mod_apps_std(m, in, inl, nin, out, outl, nout);
	else if (handle == H_MSGD) {
		char a[200] = "";
		if (nin >= 2) {
			str_arg(a, sizeof(a), in[1], inl[1]);
			LOG("dsp log: %s", a);
		}
		r = 0;
	} else {
		LOG("listener: handle %#x method %u (%d in, %d out): NOT HANDLED", handle, m, nin, nout);
		r = AEE_EUNSUPPORTED_;
	}
	if (r == 0) {
		o = 0;
		for (int j = 0; j < nout; j++) {
			memcpy(resp + o, &outl[j], 4);
			o += 4;
			if (outl[j])
				o = ((o + 7) & ~7u) + outl[j];
		}
		*resplen = o;
	}
	return r;
}

static int lsn_on, lsn_busy;
static uint32_t lsn_outoff[2];
static uint8_t *lsn_req, *lsn_resp;

static int lsn_next(uint32_t prev_ctx, int32_t prev_result, const uint8_t *prev, uint32_t prevlen)
{
	uint32_t prim[4] = { prev_ctx, (uint32_t)prev_result, prevlen, LSN_IN_CAP };
	const void *ind[2] = { prim, prev };
	uint32_t inl[2] = { 16, prevlen }, outl[2] = { 16, LSN_IN_CAP };
	uint32_t pkt = frpc_build(LSN_PA, LSN_SIZE, 2, ind, inl, 2, outl, lsn_outoff);
	if (!pkt) {
		LOG("listener: a response of %u bytes does not fit", prevlen);
		return -1;
	}
	return frpc_send(2, (int)getpid() + 1, 3 /* adsp_listener */, SCALARS(4 /* next2 */, 2, 2), LSN_PA, pkt);
}

/* From poll_for: a call of the DSP's has come (next2 answered): serve it and ask for the next. */
static void frpc_service(void)
{
	if (!lsn_on || lsn_busy || !frpc_rsp[2].got)
		return;
	lsn_busy = 1;
	frpc_rsp[2].got = 0;
	if (frpc_rsp[2].retval != 0) {
		LOG("listener: next2 -> %d: the listener stops", frpc_rsp[2].retval);
		lsn_on = 0;
		lsn_busy = 0;
		return;
	}
	uint32_t prim[4];
	pm_rd(LSN_PA + lsn_outoff[0], prim, 16);
	uint32_t ctx = prim[0], handle = prim[1], sc = prim[2], need = prim[3], got = need < LSN_IN_CAP ? need : LSN_IN_CAP;
	pm_rd(LSN_PA + lsn_outoff[1], lsn_req, got);
	uint32_t resplen = 0;
	int32_t r = need > LSN_IN_CAP ? AEE_EBADSIZE_ :
		    lsn_dispatch(handle, sc, lsn_req, got, lsn_resp, LSN_SIZE - LSN_IN_CAP - 0x2000, &resplen);
	if (r && handle != H_MSGD)
		LOG("listener: call ctx %#x handle %#x sc %#x -> %d", ctx, handle, sc, r);
	if (lsn_next(ctx, r, lsn_resp, r ? 0 : resplen))
		lsn_on = 0;
	lsn_busy = 0;
}

/* The listener up (init2, the first next2), then registered as the static PD's default listener. */
static int listener_start(void)
{
	if (!lsn_req) {
		lsn_req = malloc(LSN_IN_CAP);
		lsn_resp = malloc(LSN_SIZE);
	}
	if (!lsn_req || !lsn_resp)
		return -1;
	int32_t r = frpc_call("adsp_listener init2", 3, SCALARS(3, 0, 0), 0, NULL, NULL, 0, NULL, NULL);
	if (r)
		return -1;
	/* (with next2 sent first, this open was never answered: the open goes first, next2 before the register) */
	const char name[] = "adsp_default_listener";
	uint32_t prim_in[2] = { sizeof(name), 128 }, prim_out[2] = { 0, 0 };
	char dlerr[128] = "";
	const void *ind[2] = { prim_in, name };
	uint32_t inl[2] = { 8, sizeof(name) }, outl[2] = { 8, sizeof(dlerr) };
	void *outp[2] = { prim_out, dlerr };
	r = frpc_call("remotectl open \"adsp_default_listener\"", 0, SCALARS(0, 2, 2), 2, ind, inl, 2, outl, outp);
	dlerr[sizeof(dlerr) - 1] = 0;
	LOG("fastrpc: adsp_default_listener: handle %#x nErr %#x%s%s", prim_out[0], prim_out[1], dlerr[0] ? " dlerror " : "",
	    dlerr);
	if (r || prim_out[1])
		return -1;
	lsn_on = 1;
	if (lsn_next(0, -1, NULL, 0)) {
		lsn_on = 0;
		return -1;
	}
	r = frpc_call("adsp_default_listener register", prim_out[0], SCALARS(0, 0, 0), 0, NULL, NULL, 0, NULL, NULL);
	return r ? -1 : 0;
}

/* 1 kHz at 48 kHz, -20 dBFS: one cycle */
static const int16_t tone_cycle[48] = {
	0, 428, 848, 1254, 1638, 1995, 2317, 2600, 2838, 3028, 3165, 3249,
	3277, 3249, 3165, 3028, 2838, 2600, 2317, 1995, 1638, 1254, 848, 428,
	0, -428, -848, -1254, -1638, -1995, -2317, -2600, -2838, -3028, -3165, -3249,
	-3277, -3249, -3165, -3028, -2838, -2600, -2317, -1995, -1638, -1254, -848, -428,
};
static uint32_t tone_at, map_handle;

static void period_fill(unsigned p)
{
	int16_t s[PERIOD_FRAMES * 2];
	for (unsigned f = 0; f < PERIOD_FRAMES; f++)
		s[2 * f] = s[2 * f + 1] = tone_cycle[tone_at++ % 48];
	pm_wr(BUF_PA + 4096ull * p, s, sizeof(s));
}

static int period_write(unsigned p)
{
	uint64_t a = DSP_ADDR(BUF_PA + 4096ull * p);
	uint32_t w[8] = { (uint32_t)a, (uint32_t)(a >> 32), map_handle, PERIOD_BYTES, p, 0, 0, 0 };
	return apr_send(APR_SVC_ASM, STREAM_PORT, WRITE_TOKEN | p, ASM_DATA_CMD_WRITE_V2, w, sizeof(w));
}

static int tone(unsigned secs)
{
	int ok = 0, clock_on = 0, port_on = 0, mapped = 0, copp_open = 0, stream_open = 0;
	uint32_t copp = 0;
	uint16_t start[2] = { AFE_PORT, 0 };

	/* ADSPD_STEPS=N stops after step N: 1 the SMMU bank and ASM's map, 2 the port's settings, 3 the bit clock,
	 * 4 the port's start; default all (the stream and the tone) */
	const char *e = getenv("ADSPD_STEPS");
	unsigned steps = e ? (unsigned)atoi(e) : 99;
	if (secs > 10)
		secs = 10;
	LOG("tone: up to step %u", steps);
	{
		const char *pe = getenv("ADSPD_AUDIOPD");
		srvreg_query("msm/adsp/root_pd");
		poll_for(200);
		srvreg_query("msm/adsp/audio_pd");
		poll_for(300);
		if (!pe || atoi(pe)) {
			if (audiopd_start())
				goto out;
			poll_for(500);
			srvreg_query("msm/adsp/audio_pd");
			poll_for(300);
			if (listener_start())
				LOG("tone: the listener did not start (going on)");
			poll_for(3000);	/* let the PD come up, asking for what it needs */
			LOG("tone: the DSP made %u calls to us so far", n_requests);
		}
	}
	if (steps == 0) {
		/* probes that touch nothing: which services answer, and more than once? */
		uint32_t bogus = 0x1234;
		uint8_t gp[16 + sizeof(struct afe_param)] = { 0 };
		struct afe_param ph = { AFE_MODULE_AUDIO_DEV_INTERFACE, AFE_PARAM_ID_I2S_CONFIG, 20, 0 };
		*(uint16_t *)gp = AFE_PORT;
		*(uint16_t *)(gp + 2) = (uint16_t)(sizeof(ph) + 20);
		memcpy(gp + 16, &ph, sizeof(ph));
		for (int round = 0; round < 2; round++) {
			cmd("probe: core state", APR_SVC_CORE, 0, AVCS_GET_STATE, NULL, 0, AVCS_GET_STATE_RSP);
			cmd("probe: ASM unmap of a bogus handle", APR_SVC_ASM, 0, ASM_CMD_SHARED_MEM_UNMAP_REGIONS, &bogus,
			    sizeof(bogus), 0);
			cmd("probe: core state", APR_SVC_CORE, 0, AVCS_GET_STATE, NULL, 0, AVCS_GET_STATE_RSP);
			cmd("probe: AFE get port 0x1004's I2S config", APR_SVC_AFE, 0, 0x000100f0u /* GET_PARAM_V2 */, gp,
			    sizeof(gp), 0x00010106u /* GET_PARAM_V2 response */);
			cmd("probe: core state", APR_SVC_CORE, 0, AVCS_GET_STATE, NULL, 0, AVCS_GET_STATE_RSP);
		}
		goto out;
	}
	if (smmu_identity(0, AUDIO_SID))
		goto out;
	/*
	 * In the order that tells most when something stays silent: ASM first (memory only, no hardware), then
	 * the port's settings (no hardware yet), then the bit clock and the port's start (LPASS hardware).
	 */
	{
		uint64_t a = DSP_ADDR(BUF_PA);
		struct { uint16_t pool, regions; uint32_t property, lo, hi, size; } __attribute__((packed)) map =
			{ ASM_SHMEM_4K_POOL, 1, 0, (uint32_t)a, (uint32_t)(a >> 32), BUF_BYTES };
		uint8_t zero[4096] = { 0 };
		for (unsigned p = 0; p < PERIODS; p++)
			pm_wr(BUF_PA + 4096ull * p, zero, sizeof(zero));
		if (cmd("our periods, mapped for the DSP", APR_SVC_ASM, 0, ASM_CMD_SHARED_MEM_MAP_REGIONS, &map, sizeof(map),
			ASM_CMDRSP_SHARED_MEM_MAP_REGIONS))
			goto out;
		mapped = 1;
		map_handle = apr_ans.w[0];
		LOG("tone: %u KB at %#llx are the DSP's handle %#x", BUF_BYTES / 1024, (unsigned long long)a, map_handle);
	}
	if (steps < 2)
		goto out;
	/* the port */
	{
		struct { uint32_t version; uint16_t bits, lines, stereo, ws_src; uint32_t rate; uint16_t format, reserved; }
			__attribute__((packed)) i2s = { 1, BITS, 1 /* SD0 */, 1 /* stereo */, 1 /* internal */, RATE, 0, 0 };
		if (afe_port_param("port 0x1004: I2S SD0, stereo, 16 bits, 48 kHz, master", AFE_MODULE_AUDIO_DEV_INTERFACE,
				   AFE_PARAM_ID_I2S_CONFIG, &i2s, sizeof(i2s)))
			goto out;
	}
	if (steps < 3)
		goto out;
	pins_on();
	/* ADSPD_CLK: 2 the AFE service's clock set (v2, as sm6150's machine driver; hung once), 1 the port's own
	 * LPAIF clock config (clk-ver 1), 0 none (the port is started without a clock request) */
	{
		const char *ce = getenv("ADSPD_CLK");
		unsigned clk = ce ? (unsigned)atoi(ce) : 2;
		LOG("tone: clock mode %u", clk);
		if (clk == 2) {
			clock_on = afe_clock(1) == 0;
			if (!clock_on)
				goto out;
		} else if (clk == 1) {
			struct { uint32_t version, bit_hz, osr_hz; uint16_t src, root, mode, reserved; } __attribute__((packed))
				lc = { 1, RATE * BITS * 2, 0, 1 /* internal */, 0 /* default root */, 1 /* clk1 valid */, 0 };
			if (afe_port_param("port 0x1004: LPAIF bit clock 1.536 MHz, internal", AFE_MODULE_AUDIO_DEV_INTERFACE,
					   0x00010238u /* AFE_PARAM_ID_LPAIF_CLK_CONFIG */, &lc, sizeof(lc)))
				goto out;
		}
	}
	if (steps < 4)
		goto out;
	port_on = cmd("port 0x1004, start", APR_SVC_AFE, 0, AFE_PORT_CMD_DEVICE_START, start, sizeof(start), 0) == 0;
	if (!port_on)
		goto out;
	pin_activity(53, "bit clock");
	pin_activity(54, "word select");
	/* the stream */
	{
		struct { uint16_t flags, mode, ep1, ep2; uint32_t topology; uint16_t channels, bits; uint32_t rate; uint8_t map[8]; }
			__attribute__((packed)) open = { 0, 1 /* playback */, AFE_PORT, 0xffff, ADM_TOPOLOGY_NONE, 2, BITS, RATE,
							 { 1, 2 } };
		if (cmd("device path to the port (ADM)", APR_SVC_ADM, AFE_PORT, ADM_CMD_DEVICE_OPEN_V5, &open, sizeof(open),
			ADM_CMDRSP_DEVICE_OPEN_V5))
			goto out;
		if (apr_ans.w[0]) {
			LOG("tone: device path REFUSED: %s", dsp_error(apr_ans.w[0]));
			goto out;
		}
		copp = apr_ans.w[1] & 0xffff;
		copp_open = 1;
		LOG("tone: device path open: COPP %u", copp);
	}
	{
		struct { uint32_t flags; uint16_t sink, bits; uint32_t topology, format; } __attribute__((packed)) w =
			{ 0, 0, BITS, ASM_TOPOLOGY_NONE, ASM_MEDIA_FMT_MULTI_CHANNEL_PCM_V2 };
		if (cmd("stream 1, opened for writing", APR_SVC_ASM, STREAM_PORT, ASM_STREAM_CMD_OPEN_WRITE_V3, &w, sizeof(w), 0))
			goto out;
		stream_open = 1;
	}
	{
		struct { uint32_t matrix, sessions; uint16_t session, copps; uint16_t copp[2]; } __attribute__((packed)) r =
			{ 0, 1, 1, 1, { (uint16_t)copp, 0 } };
		struct { uint32_t size; uint16_t channels, bits; uint32_t rate; uint16_t is_signed, reserved; uint8_t map[8]; }
			__attribute__((packed)) f = { 20, 2, BITS, RATE, 1, 0, { 1, 2 } };
		uint32_t run[3] = { 0, 0, 0 };
		if (cmd("session 1 routed to the device path", APR_SVC_ADM, 0, ADM_CMD_MATRIX_MAP_ROUTINGS_V5, &r, sizeof(r), 0) ||
		    cmd("stream format: 2 x 16 bits, 48 kHz", APR_SVC_ASM, STREAM_PORT, ASM_DATA_CMD_MEDIA_FMT_UPDATE_V2, &f,
			sizeof(f), 0) ||
		    cmd("session, run", APR_SVC_ASM, STREAM_PORT, ASM_SESSION_CMD_RUN_V2, run, sizeof(run), 0))
			goto out;
	}
	/* the tone: periods written as the DSP gives them back */
	{
		uint32_t sent = 0, done0 = written_done;
		int over = 0;
		returned = 0;
		tone_at = 0;
		uint64_t began = now_ms();
		for (unsigned p = 0; p < PERIODS; p++) {
			period_fill(p);
			if (period_write(p))
				goto out;
			sent++;
		}
		for (;;) {
			poll_for(2);
			uint32_t back = returned;
			uint64_t now = now_ms();
			returned &= ~back;
			for (unsigned p = 0; p < PERIODS && !over; p++) {
				if (!(back & (1u << p)))
					continue;
				period_fill(p);
				if (period_write(p)) {
					over = 1;
					break;
				}
				sent++;
			}
			if (!over && (now - began >= secs * 1000ull || quit))
				over = 1;
			if (over && written_done - done0 >= sent)
				break;
			if (written_done == done0 && now - began > 1000) {
				LOG("tone: THE DSP HAS GIVEN NO PERIOD BACK IN A SECOND");
				break;
			}
			if (now - began > secs * 1000ull + 2000) {
				LOG("tone: not every period came back 2 s after the end (%u of %u)", written_done - done0, sent);
				break;
			}
		}
		uint64_t ms = now_ms() - began;
		uint32_t done = written_done - done0;
		LOG("tone: %u periods written, %u given back (%u with an error), %u underflows, in %llu ms: %llu frames/s",
		    sent, done, write_errors, underflows, (unsigned long long)ms,
		    ms ? (unsigned long long)done * PERIOD_FRAMES * 1000 / ms : 0);
		ok = done > PERIODS && !write_errors;
	}
out:
	if (stream_open)
		cmd("stream, closed", APR_SVC_ASM, STREAM_PORT, ASM_STREAM_CMD_CLOSE, NULL, 0, 0);
	if (copp_open)
		cmd("device path, closed", APR_SVC_ADM, (uint16_t)copp, ADM_CMD_DEVICE_CLOSE_V5, NULL, 0, 0);
	if (mapped)
		cmd("periods, unmapped", APR_SVC_ASM, 0, ASM_CMD_SHARED_MEM_UNMAP_REGIONS, &map_handle, sizeof(map_handle), 0);
	if (port_on)
		cmd("port 0x1004, stop", APR_SVC_AFE, 0, AFE_PORT_CMD_DEVICE_STOP, start, sizeof(start), 0);
	if (clock_on)
		afe_clock(0);
	smmu_report_and_off(0);
	smmu_report_and_off(1);
	pins_off();
	LOG("tone: %s", ok ? "THE DSP PLAYED THE TONE TO THE SPEAKER PORT (amplifiers off: silent)" : "FAILED");
	return ok ? 0 : 3;
}

/* ---- the steps ---- */

static int status(void)
{
	uint64_t pa;
	uint32_t sz;
	int ran = smem_get(HOST_ADSP, SMP2P_IN, &pa, &sz) == 0;
	printf("adsp partition: %s\n", smem_get(HOST_ADSP, 478, &pa, &sz) == 0 ? "glink desc present" : "no glink desc yet");
	printf("adsp SMP2P item %u: %s\n", SMP2P_IN, ran ? "PRESENT (the ADSP has run since power-on)" : "absent (never ran)");
	if (ran) {
		uint64_t va;
		if (smp2p_in_entry("slave-kernel", &va) == 0)
			printf("adsp state: %x\n", rd32(va));
	}
	printf("pas region %llx: first word %08x\n", ADSP_PA, rd32(ADSP_PA));
	return 0;
}

static void on_sig(int s) { (void)s; quit = 1; }

static int boot(unsigned secs, int force, unsigned tone_secs)
{
	int tone_rc = 0;
	signal(SIGTERM, on_sig);
	signal(SIGINT, on_sig);
	uint64_t pa;
	uint32_t sz;
	if (smem_get(HOST_ADSP, SMP2P_IN, &pa, &sz) == 0 && !force) {
		LOG("boot: the ADSP has run since power-on (SMP2P item %u exists): refusing (reboot the phone first)",
		    SMP2P_IN);
		return 1;
	}
	if (rail("lcx.lvl", 384) || rail("lmx.lvl", 384))
		return 1;
	if (smp2p_create())
		return 1;
	if (load_and_start())
		return 1;
	if (glink_init())
		return 1;
	g_send(G_VERSION, 1, FEAT_INTENT_REUSE, NULL, 0);
	uint64_t end = now_ms() + 5000;
	while (now_ms() < end && !(chans[1].open_acked && chans[1].remote_open))
		poll_for(10);
	LOG("link: version %s, IPCRTR %s, apr_audio_svc %s", g_version_done ? "agreed" : "NOT AGREED",
	    chans[0].open_acked && chans[0].remote_open ? "open" : "NOT OPEN",
	    chans[1].open_acked && chans[1].remote_open ? "open" : "NOT OPEN");
	int up = 0;
	if (chans[1].open_acked && chans[1].remote_open) {
		uint64_t began = now_ms();
		unsigned asked = 0;
		while (now_ms() - began < 15000) {
			asked++;
			if (apr_ask(APR_SVC_CORE, AVCS_GET_STATE, NULL, 0, 500) == 0 && apr_ans.opcode == AVCS_GET_STATE_RSP &&
			    apr_ans.w[0] == 1) {
				up = 1;
				break;
			}
			poll_for(200);
		}
		LOG("apr: %s after %llu ms, %u questions", up ? "THE AUDIO SERVICES ARE UP" : "the audio services did NOT come up",
		    (unsigned long long)(now_ms() - began), asked);
		if (apr_ask(APR_SVC_CORE, AVCS_GET_VERSIONS, NULL, 0, 1000) == 0) {
			printf("apr: versions (opcode %#x, %u bytes): build %#x, %u services:\n", apr_ans.opcode, apr_ans.len,
			       apr_ans.w[0], apr_ans.w[1]);
			for (uint32_t i = 0; i < apr_ans.w[1] && i < 100 && 2 + 2 * i + 1 < 256; i++)
				printf("  service %#x version %#x\n", apr_ans.w[2 + 2 * i], apr_ans.w[3 + 2 * i]);
		}
	}
	if (up && tone_secs)
		tone_rc = tone(tone_secs);
	uint64_t stay = now_ms() + secs * 1000ull;
	while (!quit && (!secs || now_ms() < stay))
		poll_for(100);
	LOG("adspd: done (the ADSP keeps running; adspd stop shuts it down)");
	return !up ? 2 : tone_rc;
}

static int stop(void)
{
	uint64_t va;
	if (!smp2p_out && smem_get(HOST_ADSP, SMP2P_OUT, &smp2p_out, &(uint32_t){ 0 })) {
		LOG("stop: no SMP2P item of ours: never started");
		return 1;
	}
	/* bit 0 of master-kernel: "shut down" (Linux qcom_q6v5_request_stop), then wait for stop-ack */
	wr32(smp2p_out + 36, rd32(smp2p_out + 36) | 1);
	kick(IPC_SMP2P);
	int acked = 0;
	for (int t = 0; t < 50 && !acked; t++) {
		if (smp2p_in_entry("slave-kernel", &va) == 0 && (rd32(va) & 8))
			acked = 1;
		usleep(100000);
	}
	LOG("stop: %s", acked ? "stop-ack" : "no stop-ack in 5 s");
	uint64_t r1 = 0;
	int64_t rc = scm(6, 1, PAS_ADSP, 0, 0, &r1);
	LOG("stop: PAS shutdown -> %lld/%llx", (long long)rc, (unsigned long long)r1);
	wr32(smp2p_out + 36, rd32(smp2p_out + 36) & ~1u);
	kick(IPC_SMP2P);
	qmp_send("{class: image, res: load_state, name: adsp, val: off}");
	return rc ? 1 : 0;
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	t0 = now_ms();
	if (argc < 2) {
		fprintf(stderr, "usage: adspd status | boot [SECS] [force] | tone [TONE_SECS] [SECS] | stop\n");
		return 2;
	}
	pm_init();
	if (smem_init()) {
		fprintf(stderr, "smem: not found\n");
		return 1;
	}
	if (!strcmp(argv[1], "status"))
		return status();
	if (!strcmp(argv[1], "boot"))
		return boot(argc > 2 ? (unsigned)atoi(argv[2]) : 20, argc > 3 && !strcmp(argv[3], "force"), 0);
	if (!strcmp(argv[1], "tone"))	/* boot, then the speakers' path with a tone of TONE_SECS (default 3, at most 10) */
		return boot(argc > 3 ? (unsigned)atoi(argv[3]) : 2, 0, argc > 2 ? (unsigned)atoi(argv[2]) : 3);
	if (!strcmp(argv[1], "stop"))
		return stop();
	fprintf(stderr, "unknown command %s\n", argv[1]);
	return 2;
}
