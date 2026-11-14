#!/usr/bin/env python3
"""
surya: bring ios-flame's GPU/Metal kernel side (flame patches 0095-0122, MIT) into the surya XNU tree.

  merge_gpu_metal.py FLAMETREE OURTREE

FLAMETREE: XNU with ios-sweet 0001-0081 + every flame patch (0082-0124) applied.
OURTREE:   the surya tree (ios-sweet 0001-0081 + surya 0082-0093), changed in place.

Files flame added are copied; files both sides changed get flame's hunks that matter here, with the surya
differences (panel 1080x2400, Adreno 618 facts, plain-LDR/STR one-page device mappings, RAM below 4 GB
that is free on this phone). Every substitution is asserted.
"""
import os, re, shutil, sys

F, O = sys.argv[1], sys.argv[2]


def rd(p):
    return open(p).read()


def wr(p, s):
    open(p, 'w').write(s)


def sub(s, old, new, what, count=1):
    n = s.count(old)
    assert n == count, f'{what}: found {n} times, expected {count}'
    return s.replace(old, new)


def take(rel):
    shutil.copyfile(os.path.join(F, rel), os.path.join(O, rel))


# ---- new files and files only flame changed (identical to the shared base on our side) ----
for rel in ['iokit/Kernel/arm/SweetAccelerator.cpp', 'iokit/Kernel/arm/SweetGPUABI.h',
            'iokit/Kernel/arm/SweetDisplayMask.h', 'iokit/Kernel/arm/SweetAMFI.cpp',
            'makedefs/MakeInc.def', 'iokit/KernelConfigTables.cpp', 'osfmk/arm/pmap/pmap.c',
            'bsd/kern/sweet_amfi.c', 'iokit/Kernel/arm/SweetSurface.cpp']:
    take(rel)

# iokit/conf/files.arm64: the accelerator and AMFI (not flame's WLAN classes)
p = os.path.join(O, 'iokit/conf/files.arm64')
s = rd(p)
s = sub(s, 'iokit/Kernel/arm/SweetBattery.cpp                                optional iokitcpp\n',
        'iokit/Kernel/arm/SweetBattery.cpp                                optional iokitcpp\n'
        'iokit/Kernel/arm/SweetAccelerator.cpp                            optional iokitcpp\n'
        'iokit/Kernel/arm/SweetAMFI.cpp                                   optional iokitcpp\n', 'files.arm64')
wr(p, s)

# sweet_files.c: truncation fix (flame 0109)
p = os.path.join(O, 'bsd/dev/sweet_files.c')
s = rd(p)
s = sub(s, '''	int err = vnode_open(path, O_CREAT | O_TRUNC | FWRITE, mode & 07777, 0, &vp, ctx);
''', '''	int err = vnode_open(path, O_CREAT | O_TRUNC | FWRITE, mode & 07777, 0, &vp, ctx);
	if (!err) {
		/* O_TRUNC through vnode_open does not shorten an existing file here: a smaller file kept the old tail */
		err = vnode_setsize(vp, 0, 0, ctx);
	}
''', 'files truncate')
wr(p, s)

# sweet_rpmh.c: BCM resources (flame 0120), for the GPU's DDR/NoC vote
p = os.path.join(O, 'bsd/dev/sweet_rpmh.c')
s = rd(p)
s = sub(s, '#define RH_ARC          3u\n', '#define RH_ARC          3u\n#define RH_BCM          5u\n', 'bcm def')
s = sub(s, "if ((slave != RH_VRM && slave != RH_ARC) || (addr >> 16) != slave || (addr & 15u) ||",
        "if ((slave != RH_VRM && slave != RH_ARC && slave != RH_BCM) || (addr >> 16) != slave || (addr & 3u) ||", 'bcm slave')
s = sub(s, 'printf("SWEET RPMH: invalid VRM/ARC entry for %s\\n", res);',
        'printf("SWEET RPMH: invalid VRM/ARC/BCM entry for %s\\n", res);', 'bcm msg')
wr(p, s)

# SweetHID.cpp: touch follows the GPU's fitted image; surya panel 1080x2400
take('iokit/Kernel/arm/hid/SweetHID.cpp')
p = os.path.join(O, 'iokit/Kernel/arm/hid/SweetHID.cpp')
s = rd(p)
s = sub(s, "/* Touch reports panel pixels; flame's panel is 1080x2280 (SWEET.h). */\n#define SWEET_PANEL_W 1080\n#define SWEET_PANEL_H 2280",
        "/* Touch reports panel pixels; surya's panel is 1080x2400. */\n#define SWEET_PANEL_W 1080\n#define SWEET_PANEL_H 2400", 'hid panel')
wr(p, s)

# SweetFramebuffer.cpp: flame's (GPU present, fixed-rate vsync with the period, corner mattes) + surya's backlight
ours_fb = rd(os.path.join(O, 'iokit/Kernel/arm/SweetFramebuffer.cpp'))
bl_start = ours_fb.index('extern "C" int sweet_spmi_init(void);')
bl_end = ours_fb.index('#if defined(SWEET)')
backlight = ours_fb[bl_start:bl_end]
assert 'sweet_backlight(bool on)' in backlight
take('iokit/Kernel/arm/SweetFramebuffer.cpp')
p = os.path.join(O, 'iokit/Kernel/arm/SweetFramebuffer.cpp')
s = rd(p)
s = sub(s, '#define SWEET_FB_GPU_ROWBYTES           4352    /* the 2D engine writes 64-byte multiples */\n',
        '#define SWEET_FB_GPU_ROWBYTES           4352    /* the 2D engine writes 64-byte multiples */\n' + backlight, 'fb backlight fn')
s = sub(s, '/* bsd/dev/sweet_gpu.c: the Adreno 640\'s 2D engine', '/* bsd/dev/sweet_gpu.c: the Adreno 618\'s 2D engine', 'fb comment')
s = sub(s, ''' * One full-frame BGRA layer (what backboardd swaps): the GPU scales the whole
 * iOS frame (1125x2436) to fit the panel (1053x2280 at x 13), instead of the''',
        ''' * One full-frame BGRA layer (what backboardd swaps): the GPU scales the whole
 * iOS frame (1125x2436) to fit the panel (surya: 1080x2339 at y 30), instead of the''', 'fb fit comment')
s = sub(s, '''	case kSweetFBRequestPowerChange:
		if (args->scalarInputCount >= 1) {
			bool off''', '''	case kSweetFBRequestPowerChange:
		if (args->scalarInputCount >= 1) {
			uint32_t bl = 1;	/* sweetbl=0 keeps the backlight alone */
			bool off''', 'fb bl var')
s = sub(s, '''			IOLog("SWEET FB: display %s\\n", off ? "off" : "on");
''', '''			IOLog("SWEET FB: display %s\\n", off ? "off" : "on");
			if (!PE_parse_boot_argn("sweetbl", &bl, sizeof(bl)) || bl) {
				sweet_backlight(!off);
			}
''', 'fb bl call')
wr(p, s)

# SweetAccelerator.cpp: Adreno 618 facts for Turnip (what our hw_init programs)
p = os.path.join(O, 'iokit/Kernel/arm/SweetAccelerator.cpp')
s = rd(p)
s = sub(s, 'setProperty("model", "Adreno 640");', 'setProperty("model", "Adreno 618");', 'acc model')
s = sub(s, '		out[0] = 640;\n		out[1] = 0x06040001;', '		out[0] = 618;\n		out[1] = 0x06010800;', 'acc v1 info')
s = sub(s, '''		/* what bsd/dev/sweet_a6xx.c's hw_init programs: GMEM 1 MB at 0x100000, UBWC 3 (hbb 15, lvl 2+3 swizzle) */
		rp->chip_id = 0x06040001;
		rp->gmem_bytes = 0x100000;
		rp->gmem_base = 0x100000;''', '''		/*
		 * surya Adreno 618 (a615 family): chip id 0x06010800 (Turnip: gpu_id 618), GMEM 512 KB at 0x100000
		 * (hw_init's GMEM range 0x100000-0x17ffff), qcom,highest-bank-bit 14, qcom,ubwc-mode 2 (UBWC 2.0).
		 * Turnip programs the UBWC registers itself; TU_DEBUG=noubwc turns it off.
		 */
		rp->chip_id = 0x06010800;
		rp->gmem_bytes = 0x80000;
		rp->gmem_base = 0x100000;''', 'acc device info')
s = sub(s, '''		rp->highest_bank_bit = 15;
		rp->ubwc_swizzle = 0x6;''', '''		rp->highest_bank_bit = 14;
		rp->ubwc_swizzle = 0x7;''', 'acc hbb')
s = sub(s, '		rp->ubwc_version = 3;', '		rp->ubwc_version = 2;', 'acc ubwc')
wr(p, s)

# ---- sweet_gpu.c: flame's final file with the surya environment ----
s = rd(os.path.join(F, 'bsd/dev/sweet_gpu.c'))
s = sub(s, ''' * The Adreno 640 in the kernel: the environment for the shared core''',
        ''' * surya (Poco X3 NFC, SM7150): the Adreno 618, from ios-flame's Adreno 640 file (MIT).
 * The GPU in the kernel: the environment for the shared core''', 'gpu title')
old_env = s[s.index('/* ---- environment for sweet_a6xx.c ---- */'):s.index('/*\n * Pool: through its kernel mapping')]
new_env = '''/* ---- environment for sweet_a6xx.c ---- */

/*
 * surya: one-page ml_io_map mappings (kept), one plain LDR/STR per word. SM7150's hypervisor emulates EL1
 * accesses to the GPU/GMU SMMUs and needs a valid syndrome (single, non-writeback accesses), and 2 MB
 * windows through ml_io_map_unmappable hung there (sweet_pmem.c, patch 0086).
 */
#define GPU_IOMAPS      256
static struct {
	vm_offset_t va;
	uint64_t page;
} gpu_map[GPU_IOMAPS];
static unsigned gpu_map_n;
static LCK_MTX_DECLARE(sweet_gpu_io_lock, &sweet_gpu_grp);

static volatile uint32_t *
gpu_io(uint64_t pa)
{
	uint64_t page = pa & ~((uint64_t)PAGE_SIZE - 1);
	unsigned n = __atomic_load_n(&gpu_map_n, __ATOMIC_ACQUIRE);
	for (unsigned i = 0; i < n; i++) {
		if (gpu_map[i].page == page) {
			return (volatile uint32_t *)(gpu_map[i].va + (pa - page));
		}
	}
	lck_mtx_lock(&sweet_gpu_io_lock);
	for (unsigned i = 0; i < gpu_map_n; i++) {
		if (gpu_map[i].page == page) {
			lck_mtx_unlock(&sweet_gpu_io_lock);
			return (volatile uint32_t *)(gpu_map[i].va + (pa - page));
		}
	}
	if (gpu_map_n == GPU_IOMAPS) {
		panic("sweet_gpu: out of device pages at 0x%llx", pa);
	}
	vm_offset_t va = ml_io_map((vm_offset_t)page, PAGE_SIZE);
	gpu_map[gpu_map_n].va = va;
	gpu_map[gpu_map_n].page = page;
	__atomic_store_n(&gpu_map_n, gpu_map_n + 1, __ATOMIC_RELEASE);
	lck_mtx_unlock(&sweet_gpu_io_lock);
	return (volatile uint32_t *)(va + (pa - page));
}

static bool
in_pool(uint64_t pa, size_t len)
{
	return (pool_kva && pa >= gpu.pool_pa && pa + len <= gpu.pool_pa + A6_POOL_SIZE) ||
	       (pt_kva && pa >= pt_pa && pa + len <= pt_pa + PT_EXTRA_SIZE);
}

/* kernel VA of pool or page-table memory (in_pool) */
static vm_offset_t
pool_addr(uint64_t pa)
{
	return pa >= gpu.pool_pa && pa < gpu.pool_pa + A6_POOL_SIZE ? pool_kva + (pa - gpu.pool_pa) : pt_kva + (pa - pt_pa);
}

uint64_t
a6_pt_extra(uint32_t n)
{
	return pt_kva && (n + 1) * 0x1000ULL <= PT_EXTRA_SIZE ? pt_pa + n * 0x1000ULL : 0;
}

uint32_t
a6_rd(uint64_t pa)
{
	uint32_t v;
	if (in_pool(pa, 4)) {
		(void)a6_read(pa, &v, 4);
		return v;
	}
	volatile uint32_t *p = gpu_io(pa);
	__asm__ volatile ("ldr %w0, [%1]" : "=r"(v) : "r"(p) : "memory");
	__asm__ volatile ("dmb oshld" ::: "memory");
	return v;
}

void
a6_wr(uint64_t pa, uint32_t v)
{
	if (in_pool(pa, 4)) {
		(void)a6_write(pa, &v, 4);
		return;
	}
	volatile uint32_t *p = gpu_io(pa);
	__asm__ volatile ("dmb oshst" ::: "memory");
	__asm__ volatile ("str %w0, [%1]" : : "r"(v), "r"(p) : "memory");
	__asm__ volatile ("dsb osh" ::: "memory");
}

'''
s = s.replace(old_env, new_env, 1)
s = sub(s, 'extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);\n',
        'extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);\n'
        'extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);\n', 'ml_io_map decl')
# the lock group must be declared before the io lock: flame declares both right after the statics
assert s.index('static LCK_GRP_DECLARE(sweet_gpu_grp') < s.index('static LCK_MTX_DECLARE(sweet_gpu_io_lock')
s = sub(s, 'a6_log("no firmware %s in the device tree (mkpayload.py --gpufw)\\n", name);',
        'a6_log("no firmware %s in the device tree (mkadt.py --gpu-fw)\\n", name);', 'fw msg')
s = sub(s, '	uint32_t lvl = 4;	/* 499 MHz; boot-arg sweetgpufreq=1 (257) .. 5 (585) */',
        '	uint32_t lvl = 4;	/* 430 MHz; boot-arg sweetgpufreq=1 (180) .. 4 (430) */', 'freq')

# the panel: 1080x2400; which source pipes ABL scans out through is found at run time
s = sub(s, '#define PANEL_H         2280', '#define PANEL_H         2400', 'panel h')
old_stride = s[s.index('/* ABL scans out through VIG0 + VIG1'):s.index('/* ---- surfaces ---- */')]
new_stride = '''/*
 * surya: ABL's splash pipes are whichever source pipes fetch 4320-byte rows (1080 x 4) from the splash memory;
 * switch those to the new stride and flush them in CTL0 (SDE flush bits: VIG0-2 = 0-2, VIG3 = 18, DMA0-1 =
 * 11-12, DMA2-3 = 24-25). Autorefresh picks a flushed change up.
 */
void
sweet_gpu_panel_stride(uint32_t stride)
{
	static const struct { uint32_t off, flush; const char *name; } pipes[] = {
		{ 0x5000, 1u << 0, "VIG0" }, { 0x7000, 1u << 1, "VIG1" }, { 0x9000, 1u << 2, "VIG2" },
		{ 0xb000, 1u << 18, "VIG3" }, { 0x25000, 1u << 11, "DMA0" }, { 0x27000, 1u << 12, "DMA1" },
		{ 0x29000, 1u << 24, "DMA2" }, { 0x2b000, 1u << 25, "DMA3" },
	};
	uint32_t flush = 0;
	for (unsigned i = 0; i < sizeof(pipes) / sizeof(pipes[0]); i++) {
		uint64_t base = MDP + pipes[i].off;
		uint32_t ys = a6_rd(base + SSPP_YSTRIDE0), addr = a6_rd(base + 0x14);
		if ((ys & 0xffff) == PANEL_W * 4 && addr >= A6_FB_PA && addr < A6_FB_PA + A6_FB_SIZE) {
			a6_wr(base + SSPP_YSTRIDE0, (ys & 0xffff0000u) | stride);
			flush |= pipes[i].flush;
			a6_log("panel: %s stride %u -> %u (fetch %x)\\n", pipes[i].name, ys & 0xffff, stride, addr);
		}
	}
	if (flush) {
		a6_wr(CTL0_FLUSH, flush);
	} else {
		a6_log("panel: no source pipe fetches the splash memory with %u-byte rows\\n", PANEL_W * 4);
	}
	a6_log("panel stride %u (flush %x)\\n", stride, flush);
}

'''
s = s.replace(old_stride, new_stride, 1)
# PANEL_W is defined further down (present); the stride code above needs it
s = sub(s, '#define MDP             0x0ae00000ULL\n', '#define MDP             0x0ae00000ULL\n#define PANEL_W         1080\n', 'panel w early')
s = sub(s, '#define PANEL_W         1080\n#define PANEL_H         2400', '#define PANEL_H         2400', 'panel w dup')

# v2 buffer pool: low-bank RAM XNU never maps on surya
s = sub(s, ''' * test v2test). v2 buffers come from HLOS RAM below 4 GB that XNU never maps instead (the flame memory
 * node is <0xc0000000 0x140000000>; XNU takes 4 GB up; the boot ramdisk at 0xc0000000 is dead once the
 * shim has jumped; phone/wifi's radio uses 0xe0000000-0xe1000000). Units of 16 KB (a CPU page).
 */''', ''' * test v2test). v2 buffers come from HLOS RAM below 4 GB that XNU never maps instead. surya: XNU takes the
 * upper bank (4 GB up, tools/mkadt.py --highmem); the low bank's fixed carve-outs end at 0xa1600000
 * (cdsp_sec_regions); phone/wifi-surya uses 0xe0000000-0xe1000000 (radio) and 0xe1100000-0xe1302000
 * (rmtfs + guards). Units of 16 KB (a CPU page).
 */''', 'gpool comment')
s = sub(s, '''	{ 0xc4000000ULL, 0x1c000000ULL },       /* 448 MB (the first 64 MB left to ABL's ramdisk) */
	{ 0xe1000000ULL, 0x1f000000ULL },       /* 496 MB */''', '''	{ 0xa2000000ULL, 0x3e000000ULL },       /* 992 MB: 0xa2000000-0xe0000000 */
	{ 0xe1400000ULL, 0x1ec00000ULL },       /* 492 MB: 0xe1400000-0x100000000 */''', 'gpool ranges')
wr(os.path.join(O, 'bsd/dev/sweet_gpu.c'), s)
print('merged')
