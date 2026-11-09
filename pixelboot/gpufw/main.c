/*
 * gpufw: boot the Mali-G710's firmware -- phase 2 of GPU acceleration. What Linux's panthor
 * driver does (drivers/gpu/drm/panthor: panthor_fw.c, panthor_mmu.c, io-pgtable-arm), bare:
 *
 *   1. the GPU awake and its L2 up (gpuprobe, 2026-10-23: GPU_ID a8620004, 7 cores);
 *   2. the CSF firmware (Android's /vendor/firmware/mali_csffw-r54p3.bin, built in below) taken
 *      apart: a header (magic c3f13a6e) and entries; an entry of type 0 is a section to load at
 *      a GPU virtual address (flags: read/write/exec, cache mode, shared, zero);
 *   3. each section put in RAM and mapped there by page tables of the GPU's own MMU (AArch64
 *      stage-1 format, 4K, 48-bit addresses), address space 0, the MCU's;
 *   4. MCU_CONTROL = AUTO, and the firmware says it is up with JOB_INT_GLOBAL_IF (bit 31 of the
 *      job interrupts); its global interface, at the start of the shared section (VA 0x04000000),
 *      begins with its version.
 * The GPU is not coherent with the CPUs (no dma-coherent in the device tree); with the MMU off
 * here, everything written goes straight to memory.
 */
#include "board.h"

#define GPU		0x28000000ul
#define GPU_IRQ_RAW	(GPU + 0x20)
#define GPU_MMU_FEAT	(GPU + 0x14)
#define L2_PRESENT	(GPU + 0x120)
#define L2_READY	(GPU + 0x160)
#define L2_PWRON	(GPU + 0x1a0)
#define JOB_IRQ_RAW	(GPU + 0x1000)
#define JOB_IRQ_CLEAR	(GPU + 0x1004)
#define MMU_IRQ_RAW	(GPU + 0x2000)
#define AS0		(GPU + 0x2400)
#define MCU_CONTROL	(GPU + 0x700)
#define MCU_STATUS	(GPU + 0x704)

#define MEM		0x900000000ul		/* RAM of ours: page tables, then sections */
#define SHARED_VA	0x04000000u

__asm__(".section .rodata\n.balign 16\n.global fw_start\nfw_start:\n"
	".incbin \"downloads/gpu/mali_csffw-r54p3.bin\"\n"
	".global fw_end\nfw_end:\n.previous\n");
extern const u8 fw_start[], fw_end[];

static u64 next_free = MEM;

static u64
ticks(void)
{
	u64 v;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
	return v;
}

static u64
ms_ticks(u64 n)
{
	u64 f;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
	return f / 1000 * n;
}

static u32
rd32(const u8 *p)
{
	return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static u64
alloc_pages(u64 bytes)
{
	u64 a = next_free;
	next_free += (bytes + 0xfff) & ~0xffful;
	for (u64 x = a; x < next_free; x += 8)
		*(volatile u64 *)x = 0;
	return a;
}

/* --- the GPU's page tables: 4 levels of 512 entries, VA bits 47-39, 38-30, 29-21, 20-12 --- */
static u64 pgd;

static u64
table(u64 parent, u64 index)
{
	volatile u64 *e = (volatile u64 *)(parent + 8 * index);
	if (!(*e & 1))
		*e = alloc_pages(4096) | 3;
	return *e & 0x0000fffffffff000ul;
}

#define PTE_AF		(1ul << 10)
#define PTE_NG		(1ul << 11)
#define PTE_SH_IS	(3ul << 8)
#define PTE_SH_OS	(2ul << 8)
#define PTE_AP_UNPRIV	(1ul << 6)
#define PTE_AP_RDONLY	(2ul << 6)
#define PTE_XN		(3ul << 53)
#define ATTR_NC		0		/* MAIR index: 0x44 normal non-cacheable */
#define ATTR_CACHE	1		/* 0xff write-back */

static void
map_page(u64 va, u64 pa, u64 bits)
{
	u64 l1 = table(pgd, (va >> 39) & 511), l2 = table(l1, (va >> 30) & 511), l3 = table(l2, (va >> 21) & 511);
	*(volatile u64 *)(l3 + 8 * ((va >> 12) & 511)) = pa | 3 | PTE_AF | PTE_NG | PTE_AP_UNPRIV | bits;
}

static int
wait_clear(u64 addr, u32 bit, u64 ms)
{
	for (u64 t = ticks(); ticks() - t < ms_ticks(ms);)
		if (!(*reg(addr) & bit))
			return 0;
	return -1;
}

static void
show(const char *name, u64 addr)
{
	puts_("gpufw: ");
	puts_(name);
	puts_(" = ");
	hex(*reg(addr), 8);
	put('\n');
}

void
at_entry(u64 el, u64 dtb)
{
	(void)el, (void)dtb;
	log_open();
	wdt_pet();
	puts_("\ngpufw: booting the Mali-G710's CSF firmware\n");
	u64 size = fw_end - fw_start;
	kv("gpufw: firmware bytes", size);
	if (size < 20 || rd32(fw_start) != 0xc3f13a6e) {
		puts_("gpufw: not a CSF firmware\n");
		return;
	}
	u32 end = rd32(fw_start + 16);
	kv("gpufw: firmware version hash", rd32(fw_start + 8));

	/* the L2 (MCU_CONTROL needs it) */
	u32 l2 = *reg(L2_PRESENT);
	*reg(L2_PWRON) = l2;
	*reg(L2_PWRON + 4) = 0;
	for (u64 t = ticks(); ticks() - t < ms_ticks(100) && (*reg(L2_READY) & l2) != l2;)
		;
	show("L2_READY", L2_READY);
	show("MMU_FEATURES", GPU_MMU_FEAT);

	pgd = alloc_pages(4096);
	u64 shared_pa = 0;
	for (u32 off = 20; off + 4 <= end && off + 4 <= size;) {
		u32 ehdr = rd32(fw_start + off), type = ehdr & 0xff, esize = (ehdr >> 8) & 0xff;
		if (esize < 4)
			break;
		const u8 *e = fw_start + off + 4;
		if (type == 0 && esize >= 24) {
			u32 flags = rd32(e), va0 = rd32(e + 4), va1 = rd32(e + 8), d0 = rd32(e + 12), d1 = rd32(e + 16);
			puts_("gpufw: section va ");
			hex(va0, 8);
			put('-');
			hex(va1, 8);
			puts_(" flags ");
			hex(flags, 8);
			puts_(" data ");
			hex(d1 - d0, 6);
			puts_(" ");
			for (u32 i = 20; i < esize - 4 && e[i]; i++)
				put((char)e[i]);
			put('\n');
			if (flags & (1u << 5)) {	/* protected mode: not for us */
				off += esize;
				continue;
			}
			u64 pa = alloc_pages(va1 - va0);
			for (u32 i = 0; i < d1 - d0 && d0 + i < size; i++)
				*(volatile u8 *)(pa + i) = fw_start[d0 + i];
			u64 bits = (((flags >> 3) & 3) == 1 ? (ATTR_CACHE << 2) | PTE_SH_IS : (ATTR_NC << 2) | PTE_SH_OS);
			if (!(flags & 2))
				bits |= PTE_AP_RDONLY;
			if (!(flags & 4))
				bits |= PTE_XN;
			for (u64 v = va0; v < va1; v += 4096)
				map_page(v, pa + (v - va0), bits);
			if (va0 == SHARED_VA)
				shared_pa = pa;
		} else {
			puts_("gpufw: entry type ");
			hex(type, 2);
			puts_(" size ");
			hex(esize, 2);
			put('\n');
		}
		off += esize;
	}
	kv("gpufw: memory used", next_free - MEM);
	kv("gpufw: shared section at", shared_pa);
	log_sync();

	/* address space 0: the tables above; MAIR 0x44 / 0xff / 0x04 / 0xf4 as memattr, not coherent */
	*reg(AS0 + 0x00) = (u32)pgd;
	*reg(AS0 + 0x04) = (u32)(pgd >> 32);
	*reg(AS0 + 0x08) = 0x8c4c8f4c;
	*reg(AS0 + 0x0c) = 0;
	*reg(AS0 + 0x30) = (2u << 24) | (1u << 30) | 6 | (7u << 6);	/* PTW WB+RA, AArch64 4K, 48-bit VA */
	*reg(AS0 + 0x34) = 0;
	int r = wait_clear(AS0 + 0x28, 1, 100);
	*reg(AS0 + 0x18) = 1;						/* UPDATE */
	r |= wait_clear(AS0 + 0x28, 1, 100);
	puts_(r ? "gpufw: AS0 update TIMEOUT\n" : "gpufw: AS0 set up\n");
	show("AS0_STATUS", AS0 + 0x28);
	log_sync();

	/* go */
	*reg(JOB_IRQ_CLEAR) = 0xffffffff;
	*reg(MCU_CONTROL) = 2;						/* AUTO */
	u64 t0 = ticks();
	while (ticks() - t0 < ms_ticks(1000) && !(*reg(JOB_IRQ_RAW) & (1u << 31)))
		;
	kv("gpufw: waited us", (ticks() - t0) * 1000 / ms_ticks(1));
	show("JOB_INT_RAWSTAT", JOB_IRQ_RAW);
	show("MCU_STATUS", MCU_STATUS);
	show("GPU_INT_RAWSTAT", GPU_IRQ_RAW);
	show("MMU_INT_RAWSTAT", MMU_IRQ_RAW);
	show("AS0_FAULTSTATUS", AS0 + 0x1c);
	show("AS0_FAULTADDRESS lo", AS0 + 0x20);
	show("AS0_FAULTADDRESS hi", AS0 + 0x24);
	if (shared_pa) {
		puts_("gpufw: global interface:");
		for (u32 i = 0; i < 8; i++) {
			put(' ');
			hex(*reg(shared_pa + 4 * i), 8);
		}
		put('\n');
	}
	puts_("gpufw: done; the watchdog resets the phone\n");
	log_sync();
}

void
at_el1_main(u64 el, u64 entry_el, u64 dtb)
{
	(void)el, (void)entry_el, (void)dtb;
	hang();
}
