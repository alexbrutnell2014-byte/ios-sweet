/*
 * adspd: the audio DSP (LPASS Hexagon, "adsp") of the Poco X3 NFC (surya, SM7150), started and spoken
 * to from userland through /dev/pmem, the way wland starts the modem. Stage 1 of the speakers.
 *
 *   adspd status            SMEM: our partition with the ADSP, its SMP2P item, whether it has run
 *   adspd boot [SECS]       rails, firmware (TrustZone PAS 1), SMP2P "ready", GLINK on the "lpass" edge
 *                           (IPCRTR + apr_audio_svc), QRTR + pd-mapper, then APR: the core service's
 *                           state until it says the audio services are up, and their versions; then
 *                           stays SECS seconds (default 20; 0 = until killed) serving the link
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
	/* every loadable segment inside the region, before anything is asked of TrustZone */
	for (uint32_t i = 0; i < phnum; i++) {
		if (ph[i].type != 1 || (int)i == hash || !ph[i].memsz)
			continue;
		if (ph[i].paddr < ADSP_PA || ph[i].paddr + (uint64_t)ph[i].memsz > ADSP_PA + ADSP_SIZE) {
			LOG("boot: segment %u at %x+%x outside the region %llx+%llx", i, ph[i].paddr, ph[i].memsz, ADSP_PA,
			    ADSP_SIZE);
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
		uint64_t pa = ph[i].paddr;
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
static struct chan chans[] = {
	{ .name = "IPCRTR", .lcid = 1, .rx = qrtr_rx },
	{ .name = "apr_audio_svc", .lcid = 2, .rx = apr_rx },
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

static int poll_for(unsigned ms)
{
	uint64_t end = now_ms() + ms;
	int n = 0;
	do {
		while (rx_one())
			n++;
		smp2p_service();
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

static void apr_rx(const uint8_t *d, uint32_t n)
{
	struct apr_hdr h;
	if (n < sizeof(h))
		return;
	memcpy(&h, d, sizeof(h));
	uint32_t hl = ((h.hdr_field >> 4) & 0xf) * 4, len = n > hl ? n - hl : 0;
	apr_ans.opcode = h.opcode;
	apr_ans.token = h.token;
	apr_ans.len = len;
	memset(apr_ans.w, 0, sizeof(apr_ans.w));
	memcpy(apr_ans.w, d + hl, len < sizeof(apr_ans.w) ? len : sizeof(apr_ans.w));
	apr_ans.got = 1;
	LOG("apr: < svc %u dom %u port %u opcode %#x token %u, %u bytes: %08x %08x %08x", h.src_svc, h.src_domain,
	    h.src_port, h.opcode, h.token, len, apr_ans.w[0], apr_ans.w[1], apr_ans.w[2]);
}

static int apr_ask(uint8_t svc, uint32_t opcode, const void *payload, uint32_t plen, unsigned wait_ms)
{
	static uint32_t token = 1;
	uint8_t b[512];
	struct apr_hdr h = { APR_HDR_FIELD, (uint16_t)(sizeof(h) + plen), APR_SVC_CORE, APR_DOMAIN_APPS, 0,
			     svc, APR_DOMAIN_ADSP, 0, token++, opcode };
	if (sizeof(h) + plen > sizeof(b))
		return -1;
	memcpy(b, &h, sizeof(h));
	if (plen)
		memcpy(b + sizeof(h), payload, plen);
	apr_ans.got = 0;
	if (ch_send(&chans[1], b, sizeof(h) + plen))
		return -1;
	uint64_t end = now_ms() + wait_ms;
	while (now_ms() < end && !apr_ans.got)
		poll_for(1);
	return apr_ans.got ? 0 : -1;
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

static volatile sig_atomic_t quit;
static void on_sig(int s) { (void)s; quit = 1; }

static int boot(unsigned secs, int force)
{
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
	signal(SIGTERM, on_sig);
	signal(SIGINT, on_sig);
	uint64_t stay = now_ms() + secs * 1000ull;
	while (!quit && (!secs || now_ms() < stay))
		poll_for(100);
	LOG("adspd: done (the ADSP keeps running; adspd stop shuts it down)");
	return up ? 0 : 2;
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
		fprintf(stderr, "usage: adspd status | boot [SECS] [force] | stop\n");
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
		return boot(argc > 2 ? (unsigned)atoi(argv[2]) : 20, argc > 3 && !strcmp(argv[3], "force"));
	if (!strcmp(argv[1], "stop"))
		return stop();
	fprintf(stderr, "unknown command %s\n", argv[1]);
	return 2;
}
