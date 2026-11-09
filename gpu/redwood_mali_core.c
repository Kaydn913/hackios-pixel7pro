/*
 * redwood_mali_core.c: the driver's core, below the interface Mesa will speak (phase 3c; included
 * by redwood_mali.c, whose helpers it uses). What Linux's panthor has, at its simplest:
 *
 *   a buffer      kernel memory, zeroed and cleaned out to RAM, mapped into GPU address spaces;
 *   a VM          a GPU address space of its own (page tables), bound for good to one of the
 *                 hardware's address spaces 1-7 (0 is the firmware's);
 *   a group       a command-stream group, bound for good to one of the firmware's 8 slots, with
 *                 up to 8 queues (streams); a queue has a ring (in the group's VM), its pointer
 *                 pages and a 64-bit sync object, and rings doorbell slot + 1;
 *   a job         panthor's sequence in the ring (prepare_job_instrs): caches flushed, a CALL of
 *                 the caller's command stream, then SYNC_ADD64 +1 on the queue's sync object and an
 *                 ERROR_BARRIER -- job n of a queue is done when its sync object reads n.
 *
 * The firmware sees each slot's pages at fixed addresses inside its 27-bit reach, mapped before
 * it starts (so its page tables never change under it): pointer pages at 0x05000000 + 0x40000 *
 * slot + 0x4000 * queue, suspend buffers at 0x06000000 + 0x20000 * slot (protected mode's at
 * +0x10000). Nothing waits on interrupts yet: a job is waited for by reading its sync object.
 */
#define SLOT_IO_VA(slot, q)     (0x05000000U + 0x40000U * (slot) + 0x4000U * (q))
#define SLOT_SUSPEND_VA(slot)   (0x06000000U + 0x20000U * (slot))
#define MAX_SLOTS               8
#define MAX_QUEUES              8
#define MAX_VMS                 7
#define KERNEL_VA_TOP           0xffff00000000ULL       /* the kernel's own allocations in a VM, downwards: above any program's */

#define GPU_DATA                (PTE_ATTR_NC | PTE_SH_OS | PTE_XN)      /* memory both sides touch */

struct mali_vm {
	uint64_t root;
	uint32_t as;                    /* register base of its hardware address space */
	uint64_t kernel_va;             /* the next of the kernel's allocations goes below this */
};

struct mali_queue {
	vm_offset_t ring, io, sync;     /* kernel addresses */
	uint64_t ring_va, sync_va;      /* in the group's VM */
	uint32_t ring_size;
	uint64_t insert, seqno;         /* bytes written; jobs submitted */
	volatile uint32_t *in, *out;    /* the firmware's stream interface */
};

struct mali_group {
	struct mali_vm *vm;
	uint32_t slot, queue_count;
	struct mali_queue queues[MAX_QUEUES];
	volatile uint32_t *in, *out;    /* the firmware's group interface */
};

static struct mali_vm vms[MAX_VMS];
static uint32_t vm_count;
static struct mali_group groups[MAX_SLOTS];
static uint32_t group_count;
static vm_offset_t slot_io[MAX_SLOTS];          /* the pointer pages of each slot's queues */
static volatile uint32_t *glb_in, *glb_out;
static uint32_t cs_regs, cs_scoreboards;

/* the firmware's view of every slot: pointer pages, suspend buffers (before the firmware starts) */
static bool
core_prepare(void)
{
	for (uint32_t s = 0; s < MAX_SLOTS; s++) {
		slot_io[s] = gpu_memory(pgd, SLOT_IO_VA(s, 0), MAX_QUEUES * 0x4000, GPU_DATA);
		if (slot_io[s] == 0 || gpu_memory(pgd, SLOT_SUSPEND_VA(s), 0x20000, GPU_DATA) == 0) {
			return false;
		}
	}
	return true;
}

/* --- the MMU: tables changed while an address space is live are flushed in a locked region --- */
static void
mmu_flush(uint32_t as, uint64_t va, uint64_t size)
{
	uint32_t width = 15;                    /* log2 of the locked region, at least 32K */

	while (width < 48 && (1ULL << width) < size) {
		width++;
	}
	va &= ~((1ULL << width) - 1);
	for (int i = 0; i < table_count; i++) {
		flush_dcache((vm_offset_t)tables[i].va, PAGE_SIZE, FALSE);
	}
	wait_as_idle(as);
	wr(as + AS_LOCKADDR, (uint32_t)va | (width - 1));
	wr(as + AS_LOCKADDR + 4, (uint32_t)(va >> 32));
	wr(as + AS_COMMAND, AS_COMMAND_LOCK);
	wait_as_idle(as);
	wr(as + AS_COMMAND, AS_COMMAND_FLUSH_PT);
	wait_as_idle(as);
}

static struct mali_vm *
mali_vm_create(void)
{
	struct mali_vm *vm;

	if (vm_count == MAX_VMS) {
		return NULL;
	}
	vm = &vms[vm_count];
	vm->root = table_new();
	vm->as = AS0 + 0x40 * (vm_count + 1);
	vm->kernel_va = KERNEL_VA_TOP;
	if (vm->root == 0 || !as_enable(vm->as, vm->root)) {
		return NULL;
	}
	vm_count++;
	return vm;
}

/* size bytes of kernel memory at kva into the VM at va (4K pages; kva from mali_bo_alloc) */
static bool
mali_vm_map(struct mali_vm *vm, uint64_t va, vm_offset_t kva, uint64_t size, uint64_t bits)
{
	for (uint64_t offset = 0; offset < size; offset += 0x1000) {
		if (!map_page(vm->root, va + offset, kvtophys(kva + offset), bits)) {
			return false;
		}
	}
	mmu_flush(vm->as, va, size);
	return true;
}

static vm_offset_t
mali_bo_alloc(uint64_t size)
{
	vm_offset_t memory = 0;

	size = round_page(size);
	if (kmem_alloc(kernel_map, &memory, size, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return 0;
	}
	flush_dcache(memory, (unsigned)size, FALSE);
	return memory;
}

/*
 * a buffer of the kernel's in a VM, at an address of the kernel's (its top end); one of 2 MB or
 * more on a 2 MB boundary, as panthor places them -- the tiler wants its heap chunks so (a chunk
 * off the boundary: DATA_INVALID_FAULT once a frame's lists reach the next chunk)
 */
static vm_offset_t
kernel_buffer(struct mali_vm *vm, uint64_t size, uint64_t *va)
{
	vm_offset_t memory = mali_bo_alloc(size);

	size = round_page(size);
	if (memory == 0) {
		return 0;
	}
	vm->kernel_va -= size;
	if (size >= 0x200000) {
		vm->kernel_va &= ~0x1fffffULL;
	}
	*va = vm->kernel_va;
	return mali_vm_map(vm, *va, memory, size, GPU_DATA) ? memory : 0;
}

/* --- the global interface: all cores, panthor's timers, and a ping to see it answers --- */
static bool
mali_glb_init(void)
{
	volatile uint32_t *control = (volatile uint32_t *)shared_va;
	volatile uint32_t *csg_control = (volatile uint32_t *)(shared_va + 0x1000);
	volatile uint32_t *cs_control = (volatile uint32_t *)(shared_va + 0x1000 + 0x40);
	uint64_t shaders = rd(SHADER_PRESENT) | (uint64_t)rd(SHADER_PRESENT + 4) << 32;
	const uint32_t cfg = GLB_CFG_ALLOC_EN | GLB_CFG_POWEROFF_TIMER | GLB_CFG_PROGRESS_TIMER;

	sync_lines(control, 0x40);
	sync_lines(csg_control, 0x100);
	glb_in = fw_block(control[2]);
	glb_out = fw_block(control[3]);
	cs_regs = (cs_control[0] & 0xff) + 1;
	cs_scoreboards = (cs_control[0] >> 8) & 0xff;
	if (glb_in == NULL || glb_out == NULL || control[4] < MAX_SLOTS || csg_control[3] > 0x10000 ||
	    csg_control[4] > 0x10000 || csg_control[5] < MAX_QUEUES || cs_regs < 8 || cs_scoreboards == 0) {
		printf("mali: the firmware's interfaces are not as expected\n");
		return false;
	}
	put64(glb_in, GLB_CORE_EN_MASK, shaders);
	glb_in[GLB_POWEROFF_TIMER / 4] = 240;                   /* ~10 ms of the 24.576 MHz counter, >> 10 */
	glb_in[GLB_PROGRESS_TIMER / 4] = (uint32_t)((5ULL * 500 * 1024 * 1024) >> 10);
	glb_in[GLB_REQ / 4] |= GLB_COUNTER_EN;
	toggle(glb_in, GLB_REQ, glb_out, GLB_ACK, cfg);
	sync_lines(glb_in, 0x100);
	wr(CSF_DOORBELL(0), 1);
	if (!acked(glb_in, GLB_REQ, glb_out, GLB_ACK, cfg, "global configuration")) {
		return false;
	}
	toggle(glb_in, GLB_REQ, glb_out, GLB_ACK, GLB_PING);
	sync_lines(glb_in, 0x100);
	wr(CSF_DOORBELL(0), 1);
	if (!acked(glb_in, GLB_REQ, glb_out, GLB_ACK, GLB_PING, "ping")) {
		return false;
	}
	printf("mali: global interface up: %u registers and %u scoreboards per stream\n", cs_regs, cs_scoreboards);
	return true;
}

/* --- groups: a slot, queues with rings of ring_size bytes, started at once --- */
static struct mali_group *
mali_group_create(struct mali_vm *vm, uint32_t queue_count, uint32_t ring_size)
{
	uint64_t shaders = rd(SHADER_PRESENT) | (uint64_t)rd(SHADER_PRESENT + 4) << 32;
	uint32_t cores = (uint32_t)__builtin_popcountll(shaders), queue_mask = 0;
	volatile uint32_t *csg_control;
	struct mali_group *g;

	if (group_count == MAX_SLOTS || queue_count == 0 || queue_count > MAX_QUEUES) {
		return NULL;
	}
	g = &groups[group_count];
	g->vm = vm;
	g->slot = group_count;
	g->queue_count = queue_count;
	csg_control = (volatile uint32_t *)(shared_va + 0x1000 + (vm_offset_t)g->slot * ((volatile uint32_t *)shared_va)[5]);
	sync_lines(csg_control, 0x100);
	g->in = fw_block(csg_control[1]);
	g->out = fw_block(csg_control[2]);
	if (g->in == NULL || g->out == NULL) {
		return NULL;
	}
	for (uint32_t q = 0; q < queue_count; q++) {
		struct mali_queue *queue = &g->queues[q];
		volatile uint32_t *cs_control = (volatile uint32_t *)((vm_offset_t)csg_control + 0x40 + q * csg_control[6]);

		queue->in = fw_block(cs_control[1]);
		queue->out = fw_block(cs_control[2]);
		queue->ring = kernel_buffer(vm, ring_size, &queue->ring_va);
		queue->sync = kernel_buffer(vm, 64, &queue->sync_va);
		queue->io = slot_io[g->slot] + q * 0x4000;
		queue->ring_size = ring_size;
		if (queue->in == NULL || queue->out == NULL || queue->ring == 0 || queue->sync == 0) {
			return NULL;
		}
		put64(queue->in, CS_RINGBUF_BASE, queue->ring_va);
		queue->in[CS_RINGBUF_SIZE / 4] = ring_size;
		put64(queue->in, CS_RINGBUF_INPUT, SLOT_IO_VA(g->slot, q));
		put64(queue->in, CS_RINGBUF_OUTPUT, SLOT_IO_VA(g->slot, q) + 0x1000);
		queue->in[CS_CONFIG / 4] = 1 | ((g->slot + 1) << 8);    /* priority 1, the group's doorbell */
		queue->in[CS_ACK_IRQ_MASK / 4] = ~0U;
		queue->in[CS_REQ / 4] = (queue->in[CS_REQ / 4] & ~(CS_STATE_MASK | CS_IDLE_SYNC_WAIT | CS_IDLE_EMPTY)) |
		    CS_STATE_START | CS_IDLE_SYNC_WAIT | CS_IDLE_EMPTY;
		sync_lines(queue->in, 0x60);
		queue_mask |= 1u << q;
	}
	put64(g->in, CSG_ALLOW_COMPUTE, shaders);
	put64(g->in, CSG_ALLOW_FRAGMENT, shaders);
	g->in[CSG_ALLOW_OTHER / 4] = 1;                         /* the tiler */
	g->in[CSG_ENDPOINT_REQ / 4] = cores | (cores << 8) | (1 << 16) | (1u << 28);
	g->in[CSG_CONFIG / 4] = (vm->as - AS0) / 0x40;
	put64(g->in, CSG_SUSPEND_BUF, SLOT_SUSPEND_VA(g->slot));
	put64(g->in, CSG_PROTM_SUSPEND_BUF, SLOT_SUSPEND_VA(g->slot) + 0x10000);
	g->in[CSG_ACK_IRQ_MASK / 4] = ~0U;
	toggle(g->in, CSG_DOORBELL_REQ, g->out, CSG_DOORBELL_ACK, queue_mask);
	g->in[CSG_REQ / 4] = (g->in[CSG_REQ / 4] & ~CSG_STATE_MASK) | CSG_STATE_START;
	sync_lines(g->in, 0x60);
	toggle(glb_in, GLB_DOORBELL_REQ, glb_out, GLB_DOORBELL_ACK, 1u << g->slot);
	sync_lines(glb_in, 0x100);
	wr(CSF_DOORBELL(0), 1);
	if (!acked(g->in, CSG_REQ, g->out, CSG_ACK, CSG_STATE_MASK, "group start")) {
		return NULL;
	}
	group_count++;
	return g;
}

/*
 * One job: cs_size bytes of commands at cs_va (in the group's VM) called from the ring, the queue's
 * sync object counted up after. Returns the job's number (what the sync object reads when done).
 */
#define CS_REG_ADDR             (cs_regs - 4)   /* the 4 registers a CALL does not keep (panthor) */
#define CS_REG_VAL              (cs_regs - 2)
#define RING_EXTRACT            (0x1000 / 8)    /* the pointer pages: what the firmware has read (output page) */

static uint64_t
mali_submit(struct mali_group *g, uint32_t q, uint64_t cs_va, uint32_t cs_size)
{
	struct mali_queue *queue = &g->queues[q];
	volatile uint64_t *ring = (volatile uint64_t *)queue->ring, *io = (volatile uint64_t *)queue->io;
	uint64_t job[16], waitall = (1ULL << cs_scoreboards) - 1;
	uint32_t n = 0;

	job[n++] = (2ULL << 56) | ((uint64_t)CS_REG_VAL << 48) | 0;                     /* MOV32 val, latest flush */
	job[n++] = (36ULL << 56) | ((uint64_t)CS_REG_VAL << 40) | 0x233;                /* FLUSH_CACHE2 all, signal 0 */
	job[n++] = (1ULL << 56) | ((uint64_t)CS_REG_ADDR << 48) | (cs_va & 0xffffffffffffULL); /* MOV48 addr, cs */
	job[n++] = (2ULL << 56) | ((uint64_t)CS_REG_VAL << 48) | cs_size;              /* MOV32 val, size */
	job[n++] = (3ULL << 56) | (1ULL << 16);                                        /* WAIT slot 0: the flush */
	job[n++] = (32ULL << 56) | ((uint64_t)CS_REG_ADDR << 40) | ((uint64_t)CS_REG_VAL << 32); /* CALL */
	job[n++] = (1ULL << 56) | ((uint64_t)CS_REG_ADDR << 48) | queue->sync_va;       /* MOV48 addr, sync */
	job[n++] = (1ULL << 56) | ((uint64_t)CS_REG_VAL << 48) | 1;                     /* MOV48 val, 1 */
	job[n++] = (3ULL << 56) | (waitall << 16);                                     /* WAIT all */
	job[n++] = (51ULL << 56) | ((uint64_t)CS_REG_ADDR << 40) | ((uint64_t)CS_REG_VAL << 32) | 1; /* SYNC_ADD64 */
	job[n++] = 47ULL << 56;                                                        /* ERROR_BARRIER */
	while (n % 8) {
		job[n++] = 0;                                                          /* NOP: a cache line each */
	}
	/* room in the ring: what the firmware has not read yet must stay (up to 10 s) */
	for (int i = 0; i < 100000; i++) {
		sync_lines((volatile void *)&io[RING_EXTRACT], 8);
		if (queue->insert + n * 8 - io[RING_EXTRACT] <= queue->ring_size) {
			break;
		}
		delay(100);
	}
	for (uint32_t i = 0; i < n; i++) {
		ring[((queue->insert / 8) + i) % (queue->ring_size / 8)] = job[i];
	}
	sync_lines((volatile void *)queue->ring, queue->ring_size);
	queue->insert += n * 8;
	io[0] = queue->insert;
	sync_lines((volatile void *)queue->io, 64);
	wr(CSF_DOORBELL(g->slot + 1), 1);
	return ++queue->seqno;
}

static bool
mali_wait(struct mali_group *g, uint32_t q, uint64_t seqno, uint32_t timeout_ms)
{
	volatile uint64_t *sync = (volatile uint64_t *)g->queues[q].sync;

	for (uint32_t i = 0; i < timeout_ms * 10; i++) {
		sync_lines(sync, 16);
		if (sync[0] >= seqno) {
			return true;
		}
		delay(100);
	}
	return false;
}

/*
 * mali=2: the core tried as Mesa will use it -- a VM, buffers of the caller's at addresses of its
 * choosing, a group, and two jobs whose command streams are in a buffer of the caller's.
 */
#define TEST_CS_VA              0x10000000ULL
#define TEST_TARGET_VA          0x10010000ULL

static void
core_selftest(void)
{
	struct mali_vm *vm;
	struct mali_group *g;
	vm_offset_t cs, target;
	volatile uint64_t *stream;
	volatile uint32_t *word;
	uint64_t job;
	uint32_t n = 0, values[2] = { 0x600df00d, 0x0ddba11 };

	if (!mali_glb_init() || (vm = mali_vm_create()) == NULL) {
		printf("mali: self-test: no VM\n");
		return;
	}
	cs = mali_bo_alloc(PAGE_SIZE);
	target = mali_bo_alloc(PAGE_SIZE);
	if (cs == 0 || target == 0 || !mali_vm_map(vm, TEST_CS_VA, cs, PAGE_SIZE, GPU_DATA & ~PTE_XN) ||
	    !mali_vm_map(vm, TEST_TARGET_VA, target, PAGE_SIZE, GPU_DATA)) {
		printf("mali: self-test: no buffers\n");
		return;
	}
	g = mali_group_create(vm, 2, 0x4000);
	if (g == NULL) {
		printf("mali: self-test: no group\n");
		hello_dump(glb_in, glb_out, groups[0].in, groups[0].out, groups[0].queues[0].in, groups[0].queues[0].out);
		return;
	}
	printf("mali: self-test: VM in address space %u, group in slot %u, rings at %#llx\n", (vm->as - AS0) / 0x40,
	    g->slot, g->queues[0].ring_va);
	stream = (volatile uint64_t *)cs;
	word = (volatile uint32_t *)target;
	for (uint32_t j = 0; j < 2; j++) {
		/* the caller's stream: a number stored at the target (word j), as a CALL leaves it to */
		uint32_t start = n;

		stream[n++] = CS_MOVE48(2, TEST_TARGET_VA + 4 * j);
		stream[n++] = CS_MOVE32(0, values[j]);
		stream[n++] = CS_STORE(0, 2, 1, 0);
		stream[n++] = CS_WAIT((1u << cs_scoreboards) - 1);
		sync_lines(stream, PAGE_SIZE);
		job = mali_submit(g, j, TEST_CS_VA + 8 * start, (n - start) * 8);
		if (!mali_wait(g, j, job, 1000)) {
			sync_lines(g->queues[j].out, 0x100);
			printf("mali: self-test: job %llu on queue %u never finished (sync %llu); blocked %u, fatal %#x\n", job,
			    j, ((volatile uint64_t *)g->queues[j].sync)[0], g->queues[j].out[CS_STATUS_BLOCKED / 4],
			    g->queues[j].out[CS_FATAL / 4]);
			return;
		}
	}
	sync_lines(word, 16);
	printf("mali: self-test: targets %#x %#x -- %s\n", word[0], word[1],
	    word[0] == values[0] && word[1] == values[1] ? "TWO JOBS RAN THROUGH THE DRIVER CORE" : "not what was stored");
}
