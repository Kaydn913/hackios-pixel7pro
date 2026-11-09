/*
 * redwood_mali: the Pixel 7 Pro's GPU, a Mali-G710 (gs201's G3D), woken and its firmware started
 * (boot-arg mali=1). Phase 3a of GPU acceleration (pixel7-ios GPU-PLAN.md); the same steps, bare,
 * are pixelboot/gpufw, which started this firmware first. A driver Mesa can draw with (panthor's
 * interface, as RedwoodGPU has msm's for the Adreno) is to be built on this.
 *
 * The GPU's two power domains are on when the bootloader is done (gpuprobe). Its registers are
 * Linux's drivers/gpu/drm/panthor (panthor_gpu_regs.h, panthor_mmu_regs.h, panthor_fw_regs.h).
 * The firmware is fw/mali_csffw.bin of the small image (userland/build.sh takes fw-gpu/ into it:
 * the phone's /vendor/firmware/mali_csffw-r54p3.bin, the one Android's r54p3 driver loads): a
 * header (magic c3f13a6e) and entries; an entry of type 0 is a section to load at a GPU virtual
 * address (flags: read, write, execute, cache mode, protected, shared, zero). The sections go into
 * kernel memory, mapped there by the GPU's own page tables -- AArch64 stage-1 format, 4K granule,
 * 48-bit addresses -- in address space 0, the firmware's (MCU's). This processor's pages are 16K:
 * a GPU page is a quarter of one. The GPU is not coherent with the processors (no dma-coherent in
 * the device tree): everything written for it is cleaned out of the caches to memory, and what it
 * writes is read after the lines are invalidated. MCU_CONTROL = AUTO, and the firmware says it is
 * up with bit 31 of the job interrupts (GLOBAL_IF); its global interface, at the start of the
 * shared section (GPU address 0x04000000), begins with its version.
 */
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);
extern void flush_dcache(vm_offset_t addr, unsigned length, boolean_t phys);
extern const uint8_t *redwood_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

#define MALI_PHYS               0x28000000ULL
#define MALI_SIZE               0x480000

#define GPU_ID                  0x0
#define GPU_IRQ_RAWSTAT         0x20
#define GPU_STATUS              0x34
#define SHADER_PRESENT          0x100
#define L2_PRESENT              0x120
#define L2_READY                0x160
#define L2_PWRON                0x1a0
#define GPU_COHERENCY_FEATURES  0x300
#define GPU_COHERENCY_PROTOCOL  0x304
#define   GPU_COHERENCY_NONE    31
#define MCU_CONTROL             0x700
#define   MCU_CONTROL_AUTO      2
#define MCU_STATUS              0x704
#define JOB_IRQ_RAWSTAT         0x1000
#define JOB_IRQ_CLEAR           0x1004
#define   JOB_IRQ_GLOBAL_IF     (1u << 31)
#define MMU_IRQ_RAWSTAT         0x2000
#define AS0                     0x2400          /* address space 0: the firmware's */
#define AS_TRANSTAB             0x00
#define AS_MEMATTR              0x08
#define AS_COMMAND              0x18
#define   AS_COMMAND_UPDATE     1
#define   AS_COMMAND_LOCK       2
#define   AS_COMMAND_FLUSH_PT   4
#define AS_LOCKADDR             0x10
#define AS_FAULTSTATUS          0x1c
#define AS_FAULTADDRESS         0x20
#define AS_STATUS               0x28
#define   AS_STATUS_ACTIVE      1
#define AS_TRANSCFG             0x30

/*
 * MAIR 0x44 (index 0: not cacheable), 0xff (1: write-back), 0x04 (2: device), 0xf4 (3) as the
 * GPU's memory attributes, for a GPU that is not coherent (panthor's mair_to_memattr()); page
 * table walks write-back with read-allocate, AArch64 4K, 55 - 48 = 7 for 48 bits of address.
 */
#define MEMATTR                 0x8c4c8f4cU
#define TRANSCFG                ((2u << 24) | (1u << 30) | 6 | (7u << 6))

#define PTE_VALID               3ULL            /* a table, or at level 3 a page */
#define PTE_AF                  (1ULL << 10)
#define PTE_NG                  (1ULL << 11)
#define PTE_SH_IS               (3ULL << 8)
#define PTE_SH_OS               (2ULL << 8)
#define PTE_AP_UNPRIV           (1ULL << 6)
#define PTE_AP_RDONLY           (2ULL << 6)
#define PTE_XN                  (3ULL << 53)
#define PTE_ATTR_NC             (0ULL << 2)
#define PTE_ATTR_CACHE          (1ULL << 2)
#define PTE_ADDR                0x0000fffffffff000ULL

#define FW_MAGIC                0xc3f13a6eU
#define FW_SHARED_VA            0x04000000U
#define FW_SECTION_PROT         (1u << 5)

static volatile uint32_t *mali;

static uint32_t
rd(uint32_t off)
{
	return mali[off / 4];
}

static void
wr(uint32_t off, uint32_t value)
{
	mali[off / 4] = value;
}

static uint32_t
le32(const uint8_t *p)
{
	return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* --- the GPU's page tables: 4K tables, each in a 16K page of its own (few are needed) --- */
#define MAX_TABLES              256
static struct {
	uint64_t phys;
	uint64_t *va;
} tables[MAX_TABLES];
static int table_count;
static uint64_t pgd;

static uint64_t
table_new(void)
{
	vm_offset_t va = 0;

	if (table_count == MAX_TABLES ||
	    kmem_alloc(kernel_map, &va, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return 0;
	}
	tables[table_count].va = (uint64_t *)va;
	tables[table_count].phys = kvtophys(va);
	return tables[table_count++].phys;
}

static uint64_t *
table_va(uint64_t phys)
{
	for (int i = 0; i < table_count; i++) {
		if (tables[i].phys == phys) {
			return tables[i].va;
		}
	}
	return NULL;
}

static bool
map_page(uint64_t root, uint64_t va, uint64_t pa, uint64_t bits)
{
	uint64_t *table = table_va(root);

	for (int level = 0; level < 3 && table != NULL; level++) {
		uint64_t *entry = &table[(va >> (39 - 9 * level)) & 511];

		if (!(*entry & 1)) {
			uint64_t next = table_new();

			if (next == 0) {
				return false;
			}
			*entry = next | PTE_VALID;
		}
		table = table_va(*entry & PTE_ADDR);
	}
	if (table == NULL) {
		return false;
	}
	table[(va >> 12) & 511] = pa | PTE_VALID | PTE_AF | PTE_NG | PTE_AP_UNPRIV | bits;
	return true;
}

static bool
wait_as_idle(uint32_t as)
{
	for (int i = 0; i < 1000; i++) {
		if (!(rd(as + AS_STATUS) & AS_STATUS_ACTIVE)) {
			return true;
		}
		delay(100);
	}
	return false;
}

/* an address space (AS0 + 0x40 * n) on the tables at root */
static bool
as_enable(uint32_t as, uint64_t root)
{
	wr(as + AS_TRANSTAB, (uint32_t)root);
	wr(as + AS_TRANSTAB + 4, (uint32_t)(root >> 32));
	wr(as + AS_MEMATTR, MEMATTR);
	wr(as + AS_MEMATTR + 4, 0);
	wr(as + AS_TRANSCFG, TRANSCFG);
	wr(as + AS_TRANSCFG + 4, 0);
	if (!wait_as_idle(as)) {
		return false;
	}
	wr(as + AS_COMMAND, AS_COMMAND_UPDATE);
	return wait_as_idle(as);
}

/* memory of ours for the GPU: zeroed kernel memory, cleaned to RAM, mapped at va in the tables at root */
static vm_offset_t
gpu_memory(uint64_t root, uint64_t va, vm_size_t size, uint64_t bits)
{
	vm_offset_t memory = 0;

	size = round_page(size);
	if (kmem_alloc(kernel_map, &memory, size, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return 0;
	}
	flush_dcache(memory, (unsigned)size, FALSE);
	for (vm_size_t offset = 0; offset < size; offset += 0x1000) {
		if (!map_page(root, va + offset, kvtophys(memory + offset), bits)) {
			return 0;
		}
	}
	return memory;
}

/* one section of the firmware: its memory, filled and mapped; the shared one's address kept */
static vm_offset_t shared_va;
#define MAX_SECTIONS            16
static struct {
	uint32_t va0, va1;
	vm_offset_t memory;
} sections[MAX_SECTIONS];
static int section_count;

/* a word of the firmware's own memory, by its address, written before it runs */
static bool
fw_poke(uint32_t va, uint32_t value)
{
	for (int i = 0; i < section_count; i++) {
		if (va >= sections[i].va0 && va + 4 <= sections[i].va1) {
			vm_offset_t at = sections[i].memory + (va - sections[i].va0);

			*(volatile uint32_t *)at = value;
			flush_dcache(at & ~63UL, 64, FALSE);
			return true;
		}
	}
	printf("mali: no section holds %#x\n", va);
	return false;
}

/*
 * The firmware's trace buffers (entries of type 3; kbase's mali_kbase_csf_trace_buffer.c): seven
 * words -- type, then the firmware's addresses of the buffer's size, insert, extract and data
 * pointers and of its trace-enable mask, and the mask's length -- and a name. kbase gives each a
 * buffer and writes those pointers into the firmware before it starts; left 0, the firmware
 * wrote at 0x1000 when a group started (a permission fault, tests mali3-5). Here each gets 64K,
 * and tracing is off (the masks 0).
 */
#define MAX_TRACES              16
#define FW_TRACE_VARS_VA        0x05300000U     /* extract variables (we write), then insert ones (it writes) */
#define FW_TRACE_DATA_VA        0x05400000U     /* 64K each */
#define TRACE_SIZE              0x10000
static uint32_t traces[MAX_TRACES][7];
static char trace_names[MAX_TRACES][16];
static int trace_count;
static vm_offset_t trace_vars, fwlog_data;      /* fwlog: all of it enabled, as Pixel's crash-report builds have it */
static int fwlog_index = -1;

/*
 * Only those kbase knows (trace_buffer_data[]): set up as well, "firmware trace" and the "debug
 * monitor" pair made the firmware read its protected-mode section while it started (test mali6)
 */
static bool
trace_wanted(const char *name)
{
	return strcmp(name, "fwlog") == 0 || strcmp(name, "benchmark") == 0 || strcmp(name, "timeline") == 0 ||
	       strcmp(name, "gpu_metrics") == 0;
}

static bool
traces_init(void)
{
	vm_offset_t vars = gpu_memory(pgd, FW_TRACE_VARS_VA, PAGE_SIZE, PTE_ATTR_NC | PTE_SH_OS | PTE_XN);

	if (trace_count > 0 && vars == 0) {
		return false;
	}
	trace_vars = vars;
	for (int i = 0; i < trace_count; i++) {
		uint32_t data = FW_TRACE_DATA_VA + (uint32_t)i * TRACE_SIZE, *t = traces[i];

		vm_offset_t memory;
		bool log = strcmp(trace_names[i], "fwlog") == 0;

		if (!trace_wanted(trace_names[i])) {
			continue;
		}
		memory = gpu_memory(pgd, data, TRACE_SIZE, PTE_ATTR_NC | PTE_SH_OS | PTE_XN);
		if (memory == 0) {
			return false;
		}
		if (log) {
			fwlog_index = i;
			fwlog_data = memory;
		}
		fw_poke(t[4], data);
		fw_poke(t[2], FW_TRACE_VARS_VA + 0x2000 + (uint32_t)i * 64);    /* insert */
		fw_poke(t[3], FW_TRACE_VARS_VA + (uint32_t)i * 64);             /* extract */
		fw_poke(t[1], TRACE_SIZE);
		for (uint32_t w = 0; w < (t[6] + 31) / 32; w++) {
			fw_poke(t[5] + w * 4, log ? ~0U : 0);
		}
	}
	return true;
}

static bool
load_section(const uint8_t *fw, uint64_t fw_size, const uint8_t *entry)
{
	uint32_t flags = le32(entry), va0 = le32(entry + 4), va1 = le32(entry + 8);
	uint32_t data0 = le32(entry + 12), data1 = le32(entry + 16);
	vm_size_t size = round_page(va1 - va0);
	vm_offset_t memory = 0;
	uint64_t bits;

	printf("mali: section %#x-%#x flags %#x, %u bytes of data\n", va0, va1, flags, data1 - data0);
	if (va1 <= va0) {
		return true;
	}
	/*
	 * Protected mode's section is loaded too, in ordinary memory: nothing here runs protected,
	 * but with its trace buffers given, the firmware read it while it started (test mali7) --
	 * Pixel's kbase backs it with the protected-memory allocator, where panthor leaves it out
	 */
	if (flags & FW_SECTION_PROT) {
		flags &= ~FW_SECTION_PROT;
	}
	if (data1 < data0 || data1 > fw_size || data1 - data0 > va1 - va0 || (va0 & 0xfff) || (va1 & 0xfff)) {
		printf("mali: the section does not make sense\n");
		return false;
	}
	if (kmem_alloc(kernel_map, &memory, size, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("mali: no memory for the section\n");
		return false;
	}
	memcpy((void *)memory, fw + data0, data1 - data0);
	flush_dcache(memory, (unsigned)size, FALSE);
	bits = ((flags >> 3) & 3) == 1 ? PTE_ATTR_CACHE | PTE_SH_IS : PTE_ATTR_NC | PTE_SH_OS;
	if (!(flags & 2)) {
		bits |= PTE_AP_RDONLY;
	}
	if (!(flags & 4)) {
		bits |= PTE_XN;
	}
	for (uint32_t offset = 0; offset < va1 - va0; offset += 0x1000) {
		if (!map_page(pgd, va0 + offset, kvtophys(memory + offset), bits)) {
			printf("mali: out of page tables\n");
			return false;
		}
	}
	if (va0 == FW_SHARED_VA) {
		shared_va = memory;
	}
	if (section_count < MAX_SECTIONS) {
		sections[section_count].va0 = va0;
		sections[section_count].va1 = va1;
		sections[section_count++].memory = memory;
	}
	return true;
}

/*
 * mali=2: the GPU runs a command stream of ours (phase 3b). The firmware's interfaces are in its
 * shared section (panthor_fw.h): at the start the global one's control block (version, features,
 * its input and output blocks' addresses, the number of command-stream groups and their stride),
 * at 0x1000 + stride * n group n's (suspend buffer size, number of streams and their stride), at
 * 0x40 + stride * m within that stream m's. Every request is made the same way: bits of the input
 * block's req set or toggled, a doorbell rung (0: the global one), and the firmware has done it
 * when the output block's ack has the same bits. A stream has a ring buffer (in its group's
 * address space, here 1) and two blocks of pointers (in the firmware's, address space 0): we
 * write commands, move insert, ring the stream's doorbell (its group's slot + 1), and the
 * firmware moves extract as it goes. The commands are Mesa's (src/panfrost/genxml/v10.xml): 64
 * bits, the opcode in the top 8.
 */
/*
 * The firmware's processor reaches 27 bits: everything of ours it is to see goes in its shared
 * region, 0x04000000-0x07ffffff (as panthor and kbase put it). At 0x08000000 and above an address
 * wraps: the stream's output page at 0x08001000 was written at 0x1000 and a trace buffer at
 * 0x0c070000 at 0x04070000 (tests mali3-10).
 */
#define FW_IO_VA                0x05000000U     /* the stream's insert/extract (input) and extract (output) */
#define FW_SUSPEND_VA           0x05100000U
/*
 * and one for protected mode, though nothing runs protected here: given 0, the firmware wrote at
 * 0x1000 when the group started (a permission fault of address space 0, test mali3); panthor
 * always gives it one
 */
#define FW_PROTM_SUSPEND_VA     0x05200000U
#define FW_SUSPEND_MAX          0x80000
#define HELLO_RING_VA           0x01000000ULL   /* in address space 1 */
#define HELLO_TARGET_VA         0x02000000ULL
#define HELLO_RING_SIZE         0x4000
#define HELLO_VALUE             0xcafe1234U

#define GLB_REQ                 0x00            /* global input */
#define GLB_DOORBELL_REQ        0x08
#define GLB_PROGRESS_TIMER      0x10
#define GLB_POWEROFF_TIMER      0x14
#define GLB_CORE_EN_MASK        0x18
#define GLB_ACK                 0x00            /* global output */
#define GLB_DOORBELL_ACK        0x08
#define   GLB_CFG_PROGRESS_TIMER (1u << 1)
#define   GLB_CFG_ALLOC_EN      (1u << 2)
#define   GLB_CFG_POWEROFF_TIMER (1u << 3)
#define   GLB_COUNTER_EN        (1u << 7)
#define   GLB_PING              (1u << 8)
#define CSG_REQ                 0x00            /* group input */
#define CSG_ACK_IRQ_MASK        0x04
#define CSG_DOORBELL_REQ        0x08
#define CSG_ALLOW_COMPUTE       0x20
#define CSG_ALLOW_FRAGMENT      0x28
#define CSG_ALLOW_OTHER         0x30
#define CSG_ENDPOINT_REQ        0x34
#define CSG_SUSPEND_BUF         0x40
#define CSG_PROTM_SUSPEND_BUF   0x48
#define CSG_CONFIG              0x50
#define CSG_ACK                 0x00            /* group output */
#define CSG_DOORBELL_ACK        0x08
#define CSG_STATUS_STATE        0x18
#define   CSG_STATE_MASK        7u
#define   CSG_STATE_START       1u
#define   CSG_ENDPOINT_CONFIG   (1u << 4)
#define CS_REQ                  0x00            /* stream input */
#define CS_CONFIG               0x04
#define CS_ACK_IRQ_MASK         0x0c
#define CS_RINGBUF_BASE         0x10
#define CS_RINGBUF_SIZE         0x18
#define CS_RINGBUF_INPUT        0x30
#define CS_RINGBUF_OUTPUT       0x38
#define CS_ACK                  0x00            /* stream output */
#define CS_STATUS_CMD_PTR       0x40
#define CS_STATUS_BLOCKED       0x60
#define CS_FAULT                0x80
#define CS_FATAL                0x84
#define CS_FAULT_INFO           0x88
#define   CS_STATE_MASK         7u
#define   CS_STATE_START        1u
#define   CS_IDLE_SYNC_WAIT     (1u << 8)
#define   CS_IDLE_EMPTY         (1u << 10)
#define CSF_DOORBELL(n)         (0x80000 + (n) * 0x10000)

#define CS_MOVE48(reg, value)   ((1ULL << 56) | ((uint64_t)(reg) << 48) | ((value) & 0xffffffffffffULL))
#define CS_MOVE32(reg, value)   ((2ULL << 56) | ((uint64_t)(reg) << 48) | (uint32_t)(value))
#define CS_WAIT(mask)           ((3ULL << 56) | ((uint64_t)(mask) << 16))
#define CS_STORE(base, addr, mask, offset) \
	((21ULL << 56) | ((uint64_t)(base) << 48) | ((uint64_t)(addr) << 40) | ((uint64_t)(mask) << 16) | (uint16_t)(offset))
/* L2 and load/store caches cleaned and invalidated; flush ID compared with a register holding 0: always */
#define CS_FLUSH_CACHES(id_reg, wait) \
	((36ULL << 56) | ((uint64_t)(id_reg) << 40) | ((uint64_t)(wait) << 16) | (3 << 4) | 3)


/* a word of a block of the shared section, by the firmware's address of the block */
static volatile uint32_t *
fw_block(uint32_t mcu_va)
{
	if (mcu_va < FW_SHARED_VA || mcu_va >= FW_SHARED_VA + 0x10000) {
		return NULL;
	}
	return (volatile uint32_t *)(shared_va + (mcu_va - FW_SHARED_VA));
}

/* what the processor wrote, out to memory; what the GPU wrote, in (no line of it is dirty here) */
static void
sync_lines(volatile void *p, unsigned size)
{
	flush_dcache((vm_offset_t)p, size, FALSE);
}

static void
put64(volatile uint32_t *block, uint32_t offset, uint64_t value)
{
	block[offset / 4] = (uint32_t)value;
	block[offset / 4 + 1] = (uint32_t)(value >> 32);
}

/* the bits of mask in req made unlike those in ack: a request, in the firmware's toggle protocol */
static void
toggle(volatile uint32_t *in, uint32_t req, volatile uint32_t *out, uint32_t ack, uint32_t mask)
{
	sync_lines(&out[ack / 4], 4);
	in[req / 4] = ((out[ack / 4] ^ mask) & mask) | (in[req / 4] & ~mask);
}

static bool
acked(volatile uint32_t *in, uint32_t req, volatile uint32_t *out, uint32_t ack, uint32_t mask, const char *what)
{
	for (int i = 0; i < 10000; i++) {
		sync_lines(&out[ack / 4], 4);
		if (((in[req / 4] ^ out[ack / 4]) & mask) == 0) {
			return true;
		}
		delay(100);
	}
	printf("mali: %s: not acknowledged (req %#x, ack %#x)\n", what, in[req / 4], out[ack / 4]);
	return false;
}

/* everything the firmware and the GPU say, when a request was not acknowledged */
static void
words(const char *name, volatile uint32_t *block, unsigned count)
{
	sync_lines(block, count * 4);
	printf("mali: %s:", name);
	for (unsigned i = 0; i < count; i++) {
		printf(" %08x", block[i]);
	}
	printf("\n");
}

static void
hello_dump(volatile uint32_t *glb_in, volatile uint32_t *glb_out, volatile uint32_t *csg_in, volatile uint32_t *csg_out,
    volatile uint32_t *cs_in, volatile uint32_t *cs_out)
{
	/* the firmware's own log: how far it wrote, then what it wrote (text, or else as numbers) */
	if (fwlog_index >= 0 && trace_vars != 0 && fwlog_data != 0) {
		volatile uint32_t *insert = (volatile uint32_t *)(trace_vars + 0x2000 + (vm_offset_t)fwlog_index * 64);
		uint32_t n;
		char line[121];
		unsigned used = 0;

		sync_lines(insert, 4);
		n = *insert < TRACE_SIZE ? *insert : TRACE_SIZE;
		sync_lines((volatile void *)fwlog_data, TRACE_SIZE);
		printf("mali: firmware log: %u bytes\n", *insert);
		for (uint32_t i = 0; i < n && i < 6000; i++) {
			char c = ((volatile char *)fwlog_data)[i];

			if (c == '\n' || used == sizeof(line) - 1) {
				line[used] = 0;
				printf("mali: fw| %s\n", line);
				used = 0;
			}
			if (c != '\n') {
				line[used++] = (c >= 32 && c < 127) ? c : '.';
			}
		}
		if (used) {
			line[used] = 0;
			printf("mali: fw| %s\n", line);
		}
	}
	for (int i = 0; i < section_count; i++) {
		if (sections[i].va0 == 0) {
			words("at 0x1000", (volatile uint32_t *)(sections[i].memory + 0x1000), 32);
			words("at 0x1080", (volatile uint32_t *)(sections[i].memory + 0x1080), 32);
		}
	}
	words("global in", glb_in, 8);
	words("global out", glb_out, 8);
	words("group in", csg_in, 22);
	words("group out", csg_out, 8);
	words("stream in", cs_in, 16);
	words("stream out", cs_out, 8);
	words("stream out +0x40", cs_out + 0x10, 8);
	words("stream out +0x80", cs_out + 0x20, 8);
	printf("mali: MCU status %u; job irq %#x; GPU irq %#x, fault %#x at %#x; MMU irq %#x; AS0 fault %#x at %#x "
	    "(high %#x), status %#x; AS1 fault %#x at %#x, status %#x; shader ready %#x, tiler ready %#x\n",
	    rd(MCU_STATUS), rd(JOB_IRQ_RAWSTAT), rd(GPU_IRQ_RAWSTAT), rd(0x3c), rd(0x40), rd(MMU_IRQ_RAWSTAT),
	    rd(AS0 + AS_FAULTSTATUS), rd(AS0 + AS_FAULTADDRESS) | 0, rd(AS0 + AS_FAULTADDRESS + 4), rd(AS0 + AS_STATUS),
	    rd(AS0 + 0x40 + AS_FAULTSTATUS), rd(AS0 + 0x40 + AS_FAULTADDRESS), rd(AS0 + 0x40 + AS_STATUS),
	    rd(0x140), rd(0x150));
}

#include "redwood_mali_core.c"
#include "redwood_mali_uapi.c"

extern void MaliGPUStart(void);         /* iokit/Kernel/MaliGPU.cpp */

void
redwood_mali_init(void)
{
	uint32_t on = 0, l2, fw_end;
	uint64_t fw_size = 0, t0;
	const uint8_t *fw;
	int disk = -1;

	PE_parse_boot_argn("malistat", &mali_stats, sizeof(mali_stats));
	PE_parse_boot_argn("mali_holdall", &mali_hold_all, sizeof(mali_hold_all));
	if (!PE_parse_boot_argn("mali", &on, sizeof(on)) || !on) {
		return;
	}
	mali = (volatile uint32_t *)ml_io_map(MALI_PHYS, MALI_SIZE);
	printf("mali: GPU_ID %#x, shader cores %#x, CSF firmware status %u\n", rd(GPU_ID), rd(SHADER_PRESENT),
	    rd(MCU_STATUS));

	/*
	 * Not coherent with the processors, and told so before the L2 is up, as panthor does: with
	 * the protocol left as it was, every address space came up with a bus error and the firmware
	 * faulted when a group started (tests mali3, mali4)
	 */
	printf("mali: coherency: supported %#x, was %u\n", rd(GPU_COHERENCY_FEATURES), rd(GPU_COHERENCY_PROTOCOL));
	wr(GPU_COHERENCY_PROTOCOL, GPU_COHERENCY_NONE);

	/* the L2 first: the firmware runs out of it */
	l2 = rd(L2_PRESENT);
	wr(L2_PWRON, l2);
	wr(L2_PWRON + 4, 0);
	for (int i = 0; i < 1000 && (rd(L2_READY) & l2) != l2; i++) {
		delay(100);
	}
	printf("mali: L2 ready %#x\n", rd(L2_READY));

	fw = redwood_image_file("fw", "mali_csffw.bin", &fw_size, &disk);
	if (fw == NULL || fw_size < 20 || le32(fw) != FW_MAGIC) {
		printf("mali: no fw/mali_csffw.bin in the small image (put the phone's /vendor/firmware/"
		    "mali_csffw-r54p3.bin into fw-gpu/ as mali_csffw.bin and run userland/build.sh)\n");
		return;
	}
	printf("mali: firmware: %llu bytes (/dev/md%d), version %#x\n", fw_size, disk, le32(fw + 8));
	pgd = table_new();
	if (pgd == 0) {
		return;
	}
	fw_end = le32(fw + 16);
	for (uint32_t offset = 20; offset + 4 <= fw_end && offset + 4 <= fw_size;) {
		uint32_t header = le32(fw + offset), type = header & 0xff, size = (header >> 8) & 0xff;

		if (size < 4) {
			break;
		}
		if (type == 0 && size >= 24 && !load_section(fw, fw_size, fw + offset + 4)) {
			return;
		}
		if (type == 3 && size >= 4 + 28 && trace_count < MAX_TRACES) {
			char name[16] = "";

			for (int w = 0; w < 7; w++) {
				traces[trace_count][w] = le32(fw + offset + 4 + 4 * w);
			}
			for (uint32_t c = 0; c < sizeof(name) - 1 && 32 + c < size; c++) {
				name[c] = (char)fw[offset + 32 + c];
			}
			strlcpy(trace_names[trace_count], name, sizeof(trace_names[trace_count]));
			trace_count++;
		}
		offset += size;
	}
	for (int i = 0; i < table_count; i++) {
		flush_dcache((vm_offset_t)tables[i].va, PAGE_SIZE, FALSE);
	}

	if (!traces_init()) {
		printf("mali: no memory for the trace buffers\n");
		return;
	}

	/* for the test (mali=2), in the firmware's address space too: a stream's pointers, a group's suspend buffer */
	if (on >= 2 && !core_prepare()) {
		printf("mali: no memory for the test\n");
		on = 1;
	}
	for (int i = 0; i < table_count; i++) {
		flush_dcache((vm_offset_t)tables[i].va, PAGE_SIZE, FALSE);
	}

	/* address space 0 */
	if (!as_enable(AS0, pgd)) {
		printf("mali: address space 0 did not take its tables\n");
		return;
	}

	/* the firmware */
	wr(JOB_IRQ_CLEAR, 0xffffffff);
	wr(MCU_CONTROL, MCU_CONTROL_AUTO);
	t0 = mach_absolute_time();
	for (int i = 0; i < 10000 && !(rd(JOB_IRQ_RAWSTAT) & JOB_IRQ_GLOBAL_IF); i++) {
		delay(100);
	}
	{
		uint64_t ns;

		absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
		printf("mali: firmware %s after %llu us: MCU status %u, job irq %#x, GPU irq %#x, MMU irq %#x, "
		    "AS0 fault %#x at %#x\n", (rd(JOB_IRQ_RAWSTAT) & JOB_IRQ_GLOBAL_IF) ? "UP" : "NOT up", ns / 1000,
		    rd(MCU_STATUS), rd(JOB_IRQ_RAWSTAT), rd(GPU_IRQ_RAWSTAT), rd(MMU_IRQ_RAWSTAT), rd(AS0 + AS_FAULTSTATUS),
		    rd(AS0 + AS_FAULTADDRESS));
	}
	if (shared_va != 0) {
		volatile uint32_t *global = (volatile uint32_t *)shared_va;

		flush_dcache(shared_va, 64, FALSE);
		printf("mali: global interface: version %#x, features %#x, input %#x, output %#x, %u groups of %#x bytes\n",
		    global[0], global[1], global[2], global[3], global[4], global[5]);
		if (on == 2 && (rd(JOB_IRQ_RAWSTAT) & JOB_IRQ_GLOBAL_IF)) {
			core_selftest();
		} else if (on >= 3 && (rd(JOB_IRQ_RAWSTAT) & JOB_IRQ_GLOBAL_IF) && mali_glb_init()) {
			MaliGPUStart();          /* mali=3: the service for programs (Mesa) */
		}
	}
}
