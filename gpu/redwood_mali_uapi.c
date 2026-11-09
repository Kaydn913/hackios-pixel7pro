/*
 * redwood_mali_uapi.c: Linux panthor's interface (include/uapi/drm/panthor_drm.h) and the DRM
 * calls Mesa's panfrost makes beside it, on the driver's core (phase 3c-2; included by
 * redwood_mali.c after redwood_mali_core.c). iokit/Kernel/MaliGPU.cpp is the IOKit side: a
 * program opens the service MaliGPU and makes each request as a call of its user client, the
 * request's number (the low byte of the ioctl code) the selector, its structure passed in and
 * back -- as RedwoodGPU does with msm's for the Adreno. Pointers inside a request are the
 * program's own, read and written with copyin/copyout (the call runs in its thread).
 *
 * Simplest forms first: VM_BIND and jobs' waits happen on the processor, at once; a sync object
 * is a few (point, job) pairs, a job being done when its queue's sync object has counted to it;
 * the streams' events (the tiler out of memory, faults) are looked at while something waits,
 * there being no interrupts yet; and nothing a program made is taken back when it goes but its
 * buffers.
 */
#define DRM_PANTHOR(n)          (0x40 + (n))
#define DRM_GEM_CLOSE           0x09
#define DRM_SYNCOBJ_CREATE      0xbf
#define DRM_SYNCOBJ_DESTROY     0xc0
#define DRM_SYNCOBJ_WAIT        0xc3
#define DRM_SYNCOBJ_RESET       0xc4
#define DRM_SYNCOBJ_SIGNAL      0xc5
#define DRM_SYNCOBJ_TIMELINE_WAIT 0xca
#define DRM_SYNCOBJ_QUERY       0xcb
#define DRM_SYNCOBJ_TRANSFER    0xcc
#define DRM_SYNCOBJ_TIMELINE_SIGNAL 0xcd

/* the IOKit side's (MaliGPU.cpp) */
extern void *maligpu_buffer(uint64_t size, vm_offset_t *kva);
extern void maligpu_buffer_release(void *buffer);
extern void maligpu_lock(void);
extern void maligpu_unlock(void);
extern uint32_t maligpu_surface_seed(void *surface);
extern bool maligpu_surface_shown(void *surface);
extern void maligpu_surface_drop(void *surface);
/* redwood_adreno.c's: memory out of this processor's cache (NULL: a barrier) */
extern void redwood_gpu_to_memory(void *start, uint64_t size);
#include <kern/clock.h>
#include <sys/errno.h>
#ifndef MIN
#define MIN(a, b)               ((a) < (b) ? (a) : (b))
#endif

struct obj_array { uint32_t stride, count; uint64_t array; };
struct sync_op { uint32_t flags, handle; uint64_t timeline_value; };
#define SYNC_OP_TIMELINE        1
#define SYNC_OP_SIGNAL          (1u << 31)

#define MAX_CLIENTS             16
#define MAX_BOS                 4096
#define MAX_SYNCOBJS            1024
#define MAX_FENCES              8

struct mali_client {
	bool used;
	struct mali_vm *vms[MAX_VMS];
	struct mali_group *groups[MAX_SLOTS];
};

struct mali_bo {
	struct mali_client *owner;
	void *memory;                   /* the IOBufferMemoryDescriptor, which mmap maps */
	vm_offset_t kva;
	uint64_t size;
	void *surface;                  /* an IOSurface's memory (mali_bo_adopt): the MiniIOSurface, held */
	uint32_t seed;                  /* its seed when last pushed out of this processor's cache, */
	uint64_t pushed;                /* and when */
	uint32_t import_id;             /* a display framebuffer's IOSurface ID (mali_bo_adopt_id), 0: none */
};

/* a job, as something to wait for: done when its queue's sync object has counted to seqno */
struct fence {
	uint64_t point;
	struct mali_group *g;
	uint32_t q;
	uint64_t seqno;                 /* 0: signaled already */
};

struct syncobj {
	struct mali_client *owner;
	uint32_t count;                 /* fences held, the newest last */
	struct fence fences[MAX_FENCES];
	uint64_t signaled;              /* every point up to this one is signaled */
};

/* a tiler heap, for growing: its context is what the firmware names when the tiler runs out */
struct heap {
	struct mali_vm *vm;
	uint64_t ctx_va;
	uint32_t chunk_size, chunks, max_chunks, target_in_flight;
};

#define MAX_HEAPS               64

static struct mali_client clients[MAX_CLIENTS];
static struct mali_bo bos[MAX_BOS];
static struct syncobj syncobjs[MAX_SYNCOBJS];
static struct heap heaps[MAX_HEAPS];
static uint32_t heap_count;

#define CS_HEAP_START           0x20            /* stream input */
#define CS_HEAP_END             0x28
#define CS_STATUS_WAIT          0x48            /* stream output */
#define CS_STATUS_WAIT_SYNC     0x50
#define CS_STATUS_SCOREBOARDS   0x5c
#define CS_FATAL_INFO           0x90
#define CS_HEAP_VT_START        0xc0
#define CS_HEAP_FRAG_END        0xcc
#define CS_HEAP_ADDRESS         0xd0
#define CS_EVENT_TILER_OOM      (1u << 26)      /* in CS_REQ / CS_ACK */
#define CS_EVENT_FATAL          (1u << 30)
#define CS_EVENT_FAULT          (1u << 31)
#define CSG_CS_IRQ_ACK          0x0c            /* group input; the output's CS_IRQ_REQ is there too */

static uint64_t
get64(volatile uint32_t *block, uint32_t offset)
{
	return block[offset / 4] | (uint64_t)block[offset / 4 + 1] << 32;
}

/*
 * The tiler out of memory: a chunk more for the heap it names, or none when the heap is at its
 * limit or too many render passes are in flight -- which makes it render what it has
 * (panthor's group_process_tiler_oom). A new chunk's header is zero (kernel buffers are).
 */
static void
tiler_oom(struct mali_group *g, struct mali_queue *queue)
{
	uint64_t address = get64(queue->out, CS_HEAP_ADDRESS), chunk = 0, va;
	uint32_t in_flight = queue->out[CS_HEAP_VT_START / 4] - queue->out[CS_HEAP_FRAG_END / 4];
	static uint32_t told;

	for (uint32_t i = 0; i < heap_count; i++) {
		struct heap *h = &heaps[i];

		if (h->vm == g->vm && h->ctx_va == address && h->chunks < h->max_chunks && in_flight <= h->target_in_flight &&
		    kernel_buffer(h->vm, h->chunk_size, &va) != 0) {
			h->chunks++;
			chunk = (va & ~0xfffULL) | (h->chunk_size >> 12);
		}
	}
	put64(queue->in, CS_HEAP_START, chunk);
	put64(queue->in, CS_HEAP_END, chunk);
	if (told++ < 8) {
		printf("MaliGPU: group %u: the tiler wants memory for heap %#llx (%u passes in flight): %s %#llx\n",
		    g->slot, address, in_flight, chunk ? "a chunk at" : "none, render what it has", chunk);
	}
}

/* what the firmware has raised on the streams: the tiler's want answered, faults told; all acknowledged */
static void
mali_service(void)
{
	static uint32_t told;

	for (uint32_t s = 0; s < group_count; s++) {
		struct mali_group *g = &groups[s];
		uint32_t doorbells = 0;

		sync_lines(g->out, 0x20);
		g->in[CSG_CS_IRQ_ACK / 4] = g->out[CSG_CS_IRQ_ACK / 4];
		for (uint32_t q = 0; q < g->queue_count; q++) {
			struct mali_queue *queue = &g->queues[q];
			uint32_t events;

			sync_lines(queue->out, 0x100);
			events = (queue->in[CS_REQ / 4] ^ queue->out[CS_ACK / 4]) &
			    (CS_EVENT_TILER_OOM | CS_EVENT_FATAL | CS_EVENT_FAULT);
			if (events == 0) {
				continue;
			}
			if ((events & (CS_EVENT_FATAL | CS_EVENT_FAULT)) && told++ < 16) {
				printf("MaliGPU: group %u queue %u: fault %#x (%#llx), fatal %#x (%#llx), the stream at %#llx\n",
				    g->slot, q, queue->out[CS_FAULT / 4], get64(queue->out, CS_FAULT_INFO),
				    queue->out[CS_FATAL / 4], get64(queue->out, CS_FATAL_INFO), get64(queue->out, CS_STATUS_CMD_PTR));
			}
			if (events & CS_EVENT_TILER_OOM) {
				tiler_oom(g, queue);
			}
			queue->in[CS_REQ / 4] = (queue->in[CS_REQ / 4] & ~events) | (queue->out[CS_ACK / 4] & events);
			sync_lines(queue->in, 0x40);
			doorbells |= 1u << q;
		}
		sync_lines(g->in, 0x20);
		if (doorbells) {
			toggle(g->in, CSG_DOORBELL_REQ, g->out, CSG_DOORBELL_ACK, doorbells);
			sync_lines(g->in, 0x20);
			toggle(glb_in, GLB_DOORBELL_REQ, glb_out, GLB_DOORBELL_ACK, 1u << g->slot);
			sync_lines(glb_in, 0x100);
			wr(CSF_DOORBELL(0), 1);
		}
	}
}

/* a wait that has gone on for 2 s: where the job it waits for stands (the first 8 times) */
static void
stuck(struct syncobj *s, uint64_t point)
{
	static uint32_t told;
	const struct fence *f = NULL;
	struct mali_queue *queue;
	volatile uint64_t *io;

	if (told++ >= 8) {
		return;
	}
	for (uint32_t i = 0; i < s->count; i++) {
		if (point == 0 ? i == s->count - 1 : s->fences[i].point >= point) {
			f = &s->fences[i];
			break;
		}
	}
	if (f == NULL || f->g == NULL) {
		printf("MaliGPU: 2 s waiting for point %llu of a sync object without a job for it (signaled to %llu, %u fences)\n",
		    point, s->signaled, s->count);
		return;
	}
	queue = &f->g->queues[f->q];
	io = (volatile uint64_t *)queue->io;
	sync_lines(queue->out, 0x100);
	sync_lines((volatile void *)queue->sync, 16);
	sync_lines((volatile void *)&io[RING_EXTRACT], 8);
	sync_lines(f->g->out, 0x20);
	printf("MaliGPU: 2 s waiting for job %llu of group %u queue %u: sync %llu, ring %llu/%llu read; stream at %#llx, "
	    "req %#x ack %#x, wait %#x on %#llx, scoreboards %#x, blocked %#x, fault %#x fatal %#x, group state %#x\n",
	    f->seqno, f->g->slot, f->q, ((volatile uint64_t *)queue->sync)[0], io[RING_EXTRACT], queue->insert,
	    get64(queue->out, CS_STATUS_CMD_PTR), queue->in[CS_REQ / 4], queue->out[CS_ACK / 4],
	    queue->out[CS_STATUS_WAIT / 4], get64(queue->out, CS_STATUS_WAIT_SYNC), queue->out[CS_STATUS_SCOREBOARDS / 4],
	    queue->out[CS_STATUS_BLOCKED / 4], queue->out[CS_FAULT / 4], queue->out[CS_FATAL / 4],
	    f->g->out[CSG_STATUS_STATE / 4]);
}

static bool
fence_done(const struct fence *f)
{
	if (f->seqno == 0) {
		return true;
	}
	sync_lines((volatile void *)f->g->queues[f->q].sync, 16);
	return ((volatile uint64_t *)f->g->queues[f->q].sync)[0] >= f->seqno;
}

/* is point signaled (0: a binary object -- its newest fence, if it has one, is done) */
static bool
syncobj_signaled(struct syncobj *s, uint64_t point)
{
	if (point == 0) {
		return s->count > 0 ? fence_done(&s->fences[s->count - 1]) : s->signaled > 0;
	}
	if (s->signaled >= point) {
		return true;
	}
	for (uint32_t i = 0; i < s->count; i++) {
		if (s->fences[i].point >= point && fence_done(&s->fences[i])) {
			return true;
		}
	}
	return false;
}

/* point (0: binary) gets a fence; the oldest fence done falls out when there is no room */
static void
syncobj_add(struct syncobj *s, uint64_t point, struct mali_group *g, uint32_t q, uint64_t seqno)
{
	struct fence f = { point, g, q, seqno };

	if (point == 0) {
		s->count = 0;
		s->signaled = 0;
	}
	if (s->count == MAX_FENCES) {
		if (fence_done(&s->fences[0]) && s->fences[0].point > s->signaled) {
			s->signaled = s->fences[0].point;
		}
		memmove(&s->fences[0], &s->fences[1], sizeof(f) * (MAX_FENCES - 1));
		s->count--;
	}
	if (seqno == 0 && point > s->signaled) {
		s->signaled = point;
		return;
	}
	s->fences[s->count++] = f;
}

static struct syncobj *
syncobj_get(struct mali_client *c, uint32_t handle)
{
	return handle >= 1 && handle <= MAX_SYNCOBJS && syncobjs[handle - 1].owner == c ? &syncobjs[handle - 1] : NULL;
}

static struct mali_bo *
bo_get(struct mali_client *c, uint32_t handle)
{
	return handle >= 1 && handle <= MAX_BOS && bos[handle - 1].owner == c ? &bos[handle - 1] : NULL;
}

static struct mali_vm *
vm_get(struct mali_client *c, uint32_t id)
{
	return id >= 1 && id <= MAX_VMS ? c->vms[id - 1] : NULL;
}

static struct mali_group *
group_get(struct mali_client *c, uint32_t handle)
{
	return handle >= 1 && handle <= MAX_SLOTS ? c->groups[handle - 1] : NULL;
}

/* waits on the processor, the lock let go meanwhile: up to ms milliseconds; 0 if signaled */
static int
syncobj_wait_point(struct syncobj *s, uint64_t point, uint32_t ms)
{
	for (uint32_t i = 0; !syncobj_signaled(s, point); i++) {
		if (i >= ms * 10) {
			return ETIMEDOUT;
		}
		if (i % 10 == 0) {
			mali_service();
		}
		if (i == 20000) {
			stuck(s, point);
		}
		maligpu_unlock();
		delay(100);
		maligpu_lock();
	}
	return 0;
}

/* the sync operations of a request: waits done now; signals given the fence (NULL: signaled now) */
static int
sync_ops(struct mali_client *c, const struct obj_array *a, bool waits, struct mali_group *g, uint32_t q, uint64_t seqno)
{
	for (uint32_t i = 0; i < a->count; i++) {
		struct sync_op op = { 0 };
		struct syncobj *s;
		int error = copyin(a->array + (uint64_t)i * a->stride, &op, MIN(sizeof(op), a->stride));

		if (error) {
			return error;
		}
		if ((s = syncobj_get(c, op.handle)) == NULL) {
			return EINVAL;
		}
		if (!(op.flags & SYNC_OP_SIGNAL) && waits) {
			error = syncobj_wait_point(s, (op.flags & 0xff) == SYNC_OP_TIMELINE ? op.timeline_value : 0, 10000);
			if (error) {
				return error;
			}
		} else if ((op.flags & SYNC_OP_SIGNAL) && !waits) {
			syncobj_add(s, (op.flags & 0xff) == SYNC_OP_TIMELINE ? op.timeline_value : 0, g, q, g ? seqno : 0);
		}
	}
	return 0;
}

/* --- the requests --- */

static int
dev_query(void *arg)
{
	struct { uint32_t type, size; uint64_t pointer; } *q = arg;
	union {
		struct {
			uint32_t gpu_id, gpu_rev, csf_id, l2_features, tiler_features, mem_features, mmu_features,
			    thread_features, max_threads, thread_max_workgroup_size, thread_max_barrier_size,
			    coherency_features, texture_features[4], as_present, selected_coherency;
			uint64_t shader_present, l2_present, tiler_present;
			uint32_t core_features, pad;
			uint64_t gpu_features;
		} gpu;
		struct { uint32_t csg_slots, cs_slots, cs_regs, scoreboards, unpreserved, pad; } csif;
		struct {
			uint64_t frequency, current, offset;
			uint32_t flags, duration_nsec;
			uint64_t cycle_count, cpu_sec, cpu_nsec;
		} time;
		struct { uint8_t allowed_mask, pad[3]; } priorities;
	} r;
	uint32_t size;
	uint64_t f;

	bzero(&r, sizeof(r));
	switch (q->type) {
	case 0:
		size = sizeof(r.gpu);
		r.gpu.gpu_id = rd(GPU_ID);
		r.gpu.gpu_rev = rd(0x280);
		r.gpu.csf_id = rd(0x1c);
		r.gpu.l2_features = rd(0x4);
		r.gpu.tiler_features = rd(0xc);
		r.gpu.mem_features = rd(0x10);
		r.gpu.mmu_features = rd(0x14);
		r.gpu.thread_features = rd(0xac);
		r.gpu.max_threads = rd(0xa0);
		r.gpu.thread_max_workgroup_size = rd(0xa4);
		r.gpu.thread_max_barrier_size = rd(0xa8);
		r.gpu.coherency_features = rd(GPU_COHERENCY_FEATURES);
		for (int i = 0; i < 4; i++) {
			r.gpu.texture_features[i] = rd(0xb0 + 4 * i);
		}
		r.gpu.as_present = rd(0x18);
		r.gpu.selected_coherency = GPU_COHERENCY_NONE;
		r.gpu.shader_present = rd(SHADER_PRESENT) | (uint64_t)rd(SHADER_PRESENT + 4) << 32;
		r.gpu.l2_present = rd(L2_PRESENT) | (uint64_t)rd(L2_PRESENT + 4) << 32;
		r.gpu.tiler_present = rd(0x110) | (uint64_t)rd(0x114) << 32;
		r.gpu.core_features = rd(0x8);
		r.gpu.gpu_features = rd(0x60) | (uint64_t)rd(0x64) << 32;
		break;
	case 1:
		size = sizeof(r.csif);
		r.csif.csg_slots = MAX_SLOTS;
		r.csif.cs_slots = MAX_QUEUES;
		r.csif.cs_regs = cs_regs;
		r.csif.scoreboards = cs_scoreboards;
		r.csif.unpreserved = 4;
		break;
	case 2:
		size = sizeof(r.time);
		__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
		r.time.frequency = f;
		r.time.current = rd(0x98) | (uint64_t)rd(0x9c) << 32;
		r.time.offset = rd(0x88) | (uint64_t)rd(0x8c) << 32;
		break;
	case 3:
		size = sizeof(r.priorities);
		r.priorities.allowed_mask = 0x3;        /* low and medium */
		break;
	default:
		return EINVAL;
	}
	if (q->pointer == 0) {
		q->size = size;
		return 0;
	}
	return copyout(&r, q->pointer, MIN(size, q->size));
}

static int
vm_create(struct mali_client *c, void *arg)
{
	struct { uint32_t flags, id; uint64_t user_va_range; } *a = arg;

	for (uint32_t i = 0; i < MAX_VMS; i++) {
		if (c->vms[i] == NULL) {
			if ((c->vms[i] = mali_vm_create()) == NULL) {
				return ENOMEM;
			}
			a->id = i + 1;
			if (a->user_va_range == 0 || a->user_va_range > KERNEL_VA_TOP - 0x100000000ULL) {
				a->user_va_range = KERNEL_VA_TOP - 0x100000000ULL;
			}
			return 0;
		}
	}
	return ENOSPC;
}

static void
unmap_pages(struct mali_vm *vm, uint64_t va, uint64_t size)
{
	for (uint64_t offset = 0; offset < size; offset += 0x1000) {
		uint64_t *table = table_va(vm->root), at = va + offset;

		for (int level = 0; level < 3 && table != NULL; level++) {
			uint64_t entry = table[(at >> (39 - 9 * level)) & 511];

			table = (entry & 1) ? table_va(entry & PTE_ADDR) : NULL;
		}
		if (table != NULL) {
			table[(at >> 12) & 511] = 0;
		}
	}
	mmu_flush(vm->as, va, size);
}

static int
vm_bind(struct mali_client *c, void *arg)
{
	struct { uint32_t vm_id, flags; struct obj_array ops; } *a = arg;
	struct mali_vm *vm = vm_get(c, a->vm_id);

	if (vm == NULL) {
		return EINVAL;
	}
	for (uint32_t i = 0; i < a->ops.count; i++) {
		struct {
			uint32_t flags, bo_handle;
			uint64_t bo_offset, va, size;
			struct obj_array syncs;
		} op = { 0 };
		struct mali_bo *bo;
		uint64_t bits;
		int error = copyin(a->ops.array + (uint64_t)i * a->ops.stride, &op, MIN(sizeof(op), a->ops.stride));

		if (error || (error = sync_ops(c, &op.syncs, true, NULL, 0, 0))) {
			return error;
		}
		if ((op.va | op.size | op.bo_offset) & 0xfff) {
			return EINVAL;
		}
		switch (op.flags >> 28) {
		case 0:                                         /* map */
			bo = bo_get(c, op.bo_handle);
			if (bo == NULL || op.bo_offset + op.size > bo->size) {
				return EINVAL;
			}
			bits = (op.flags & 4) ? PTE_ATTR_NC | PTE_SH_OS : PTE_ATTR_NC | PTE_SH_OS;   /* all uncached for now */
			if (op.flags & 1) {
				bits |= PTE_AP_RDONLY;
			}
			if (op.flags & 2) {
				bits |= PTE_XN;
			}
			if (!mali_vm_map(vm, op.va, bo->kva + op.bo_offset, op.size, bits)) {
				return ENOMEM;
			}
			break;
		case 1:                                         /* unmap */
			unmap_pages(vm, op.va, op.size);
			break;
		case 2:                                         /* sync only */
			break;
		default:
			return EINVAL;
		}
		if ((error = sync_ops(c, &op.syncs, false, NULL, 0, 0))) {
			return error;
		}
	}
	return 0;
}

static int
bo_create(struct mali_client *c, void *arg)
{
	struct { uint64_t size; uint32_t flags, exclusive_vm_id, handle, pad; } *a = arg;

	if (a->size == 0) {
		return EINVAL;
	}
	for (uint32_t i = 0; i < MAX_BOS; i++) {
		if (bos[i].owner == NULL) {
			struct mali_bo *bo = &bos[i];

			bo->size = round_page(a->size);
			bo->memory = maligpu_buffer(bo->size, &bo->kva);
			if (bo->memory == NULL) {
				return ENOMEM;
			}
			bo->owner = c;
			a->size = bo->size;
			a->handle = i + 1;
			return 0;
		}
	}
	return ENOSPC;
}

static void
bo_free(struct mali_bo *bo)
{
	maligpu_buffer_release(bo->memory);
	if (bo->surface != NULL) {
		maligpu_surface_drop(bo->surface);
	}
	bzero(bo, sizeof(*bo));
}

/*
 * The IOSurfaces a program has as buffers, out of this processor's cache before the GPU is given
 * work: the GPU reads and writes memory, everybody else the cache. Only those whose seed says
 * somebody has written them since -- a program drawing with the processor, and eagl.c after the
 * GPU has drawn into one (it locks it for that), which the display's driver then reads through
 * the cache. Not RedwoodGPU's "every 4 ms as well": there a submission names its buffers, here
 * that would be every surface the window server has ever shown, at every submission (gl5:
 * glFlush 7.4 ms). A surface somebody writes without saying so may show stale here.
 */
static uint64_t
surfaces_to_memory(struct mali_client *c)
{
	bool any = false;
	uint64_t bytes = 0;

	for (uint32_t i = 0; i < MAX_BOS; i++) {
		struct mali_bo *bo = &bos[i];
		uint32_t seed;

		if (bo->owner != c || bo->surface == NULL) {
			continue;
		}
		seed = maligpu_surface_seed(bo->surface);
		if (seed == bo->seed) {
			continue;
		}
		bo->seed = seed;
		bo->pushed = mach_absolute_time();
		redwood_gpu_to_memory((void *)bo->kva, bo->size);
		bytes += bo->size;
		any = true;
	}
	if (any) {
		redwood_gpu_to_memory(NULL, 0);
	}
	return bytes;
}

/*
 * The window server draws every frame into the same surface (gl21: swaps 1, 2, 6 and 8 all in
 * surface 1) and begins the next as soon as SwapEnd has returned -- while the display's driver
 * still waits to copy the last frame out of it, or is copying it: half old, half new, the six
 * bands of tearing (gl20: 226 of 5200 frames' surfaces drawn again during their copy). A program's
 * GPU work so waited here until no surface it has as a buffer is the display's (MiniIOSurfaceShown:
 * from SwapEnd to the end of the copy), 40 ms at most (gl22, gl24: 0 rewrites). A submission does
 * not say which buffers it writes, so any surface of the program's counted -- and so all of the
 * next frame's work waited, where only eagl's last GPU copy into the surface writes it. That copy
 * now waits by itself (a write lock, MiniIOSurface.cpp trapLock; eagl.c glhold); this is boot-arg
 * mali_holdall=1.
 */
uint32_t mali_shown_waits, mali_shown_timeouts;         /* for the display's five-second line */
uint64_t mali_shown_wait_abs;
static uint32_t mali_hold_all;

static void
surfaces_shown_wait(struct mali_client *c)
{
	uint64_t began = mach_absolute_time();
	uint32_t tries;

	if (!mali_hold_all) {
		return;
	}
	for (tries = 0;; tries++) {
		bool shown = false;

		for (uint32_t i = 0; i < MAX_BOS && !shown; i++) {
			shown = bos[i].owner == c && bos[i].surface != NULL && maligpu_surface_shown(bos[i].surface);
		}
		if (!shown) {
			break;
		}
		if (tries == 400) {
			mali_shown_timeouts++;
			break;
		}
		maligpu_unlock();
		delay(100);
		maligpu_lock();
	}
	if (tries > 0) {
		mali_shown_waits++;
		mali_shown_wait_abs += mach_absolute_time() - began;
	}
}

/*
 * Where the requests' time goes: per request number, and of GROUP_SUBMIT, waiting (its syncs,
 * on the processor), pushing surfaces out of the cache, and the ring; a summary every 4096.
 */
static uint64_t req_abs[256], req_count[256], req_total;
static uint32_t mali_stats;                     /* boot-arg malistat=1: say it */
static uint64_t submit_wait_abs, submit_flush_abs, submit_flush_bytes, submit_ring_abs;

static uint64_t
abs_us(uint64_t abs)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(abs, &ns);
	return ns / 1000;
}

static void
requests_said(void)
{
	printf("MaliGPU: 4096 requests:");
	for (int i = 0; i < 256; i++) {
		if (req_count[i] != 0) {
			printf(" %#x %llux %lluus", i, req_count[i], abs_us(req_abs[i]));
		}
	}
	printf("; submits: %llu us waiting, %llu us pushing %llu MB out of the cache, %llu us the ring\n",
	    abs_us(submit_wait_abs), abs_us(submit_flush_abs), submit_flush_bytes >> 20, abs_us(submit_ring_abs));
	bzero(req_abs, sizeof(req_abs));
	bzero(req_count, sizeof(req_count));
	submit_wait_abs = submit_flush_abs = submit_flush_bytes = submit_ring_abs = 0;
}

static int
group_create(struct mali_client *c, void *arg)
{
	struct {
		struct obj_array queues;
		uint8_t max_compute, max_fragment, max_tiler, priority;
		uint32_t pad;
		uint64_t compute_mask, fragment_mask, tiler_mask;
		uint32_t vm_id, group_handle;
	} *a = arg;
	struct mali_vm *vm = vm_get(c, a->vm_id);
	uint32_t ring = 0x1000;

	if (vm == NULL || a->queues.count == 0 || a->queues.count > MAX_QUEUES) {
		return EINVAL;
	}
	for (uint32_t i = 0; i < a->queues.count; i++) {
		struct { uint8_t priority, pad[3]; uint32_t ringbuf_size; } q = { 0 };
		int error = copyin(a->queues.array + (uint64_t)i * a->queues.stride, &q, MIN(sizeof(q), a->queues.stride));

		if (error) {
			return error;
		}
		if (q.ringbuf_size > ring) {
			ring = q.ringbuf_size;
		}
	}
	for (uint32_t i = 0; i < MAX_SLOTS; i++) {
		if (c->groups[i] == NULL) {
			if ((c->groups[i] = mali_group_create(vm, a->queues.count, ring)) == NULL) {
				return ENOMEM;
			}
			a->group_handle = i + 1;
			return 0;
		}
	}
	return ENOSPC;
}

static int
group_submit(struct mali_client *c, void *arg)
{
	struct { uint32_t group_handle, pad; struct obj_array submits; } *a = arg;
	struct mali_group *g = group_get(c, a->group_handle);

	if (g == NULL) {
		return EINVAL;
	}
	uint64_t t, t2;

	surfaces_shown_wait(c);
	t = mach_absolute_time();
	submit_flush_bytes += surfaces_to_memory(c);
	submit_flush_abs += mach_absolute_time() - t;
	for (uint32_t i = 0; i < a->submits.count; i++) {
		struct {
			uint32_t queue_index, stream_size;
			uint64_t stream_addr;
			uint32_t latest_flush, pad;
			struct obj_array syncs;
		} s = { 0 };
		uint64_t seqno;
		int error = copyin(a->submits.array + (uint64_t)i * a->submits.stride, &s, MIN(sizeof(s), a->submits.stride));

		if (error) {
			return error;
		}
		if (s.queue_index >= g->queue_count) {
			return EINVAL;
		}
		t = mach_absolute_time();
		if ((error = sync_ops(c, &s.syncs, true, NULL, 0, 0))) {
			return error;
		}
		t2 = mach_absolute_time();
		submit_wait_abs += t2 - t;
		seqno = s.stream_size ? mali_submit(g, s.queue_index, s.stream_addr, s.stream_size) :
		    g->queues[s.queue_index].seqno;
		submit_ring_abs += mach_absolute_time() - t2;
		if ((error = sync_ops(c, &s.syncs, false, seqno ? g : NULL, s.queue_index, seqno))) {
			return error;
		}
	}
	return 0;
}

static int
group_get_state(struct mali_client *c, void *arg)
{
	struct { uint32_t group_handle, state, fatal_queues, pad; } *a = arg;
	struct mali_group *g = group_get(c, a->group_handle);

	if (g == NULL) {
		return EINVAL;
	}
	a->state = 0;
	a->fatal_queues = 0;
	for (uint32_t q = 0; q < g->queue_count; q++) {
		sync_lines(g->queues[q].out, 0x100);
		if (g->queues[q].out[CS_FATAL / 4] != 0) {
			a->fatal_queues |= 1u << q;
			a->state |= 2;                          /* DRM_PANTHOR_GROUP_STATE_FATAL_FAULT */
		}
	}
	return 0;
}

/* a heap's context (32 bytes, zero) and its chunks, linked: each begins (next & ~0xfff) | (size >> 12) */
static int
tiler_heap_create(struct mali_client *c, void *arg)
{
	struct {
		uint32_t vm_id, initial_chunk_count, chunk_size, max_chunks, target_in_flight, handle;
		uint64_t ctx_va, first_chunk_va;
	} *a = arg;
	struct mali_vm *vm = vm_get(c, a->vm_id);
	uint64_t ctx_va, va, next = 0;

	if (vm == NULL || a->initial_chunk_count == 0 || a->chunk_size < 0x40000 || (a->chunk_size & 0xfff)) {
		return EINVAL;
	}
	if (heap_count == MAX_HEAPS) {
		return ENOSPC;
	}
	if (kernel_buffer(vm, 64, &ctx_va) == 0) {
		return ENOMEM;
	}
	for (uint32_t i = 0; i < a->initial_chunk_count; i++) {
		vm_offset_t chunk = kernel_buffer(vm, a->chunk_size, &va);

		if (chunk == 0) {
			return ENOMEM;
		}
		if (next != 0) {        /* the last of the list keeps a zero header (its end), as panthor's does */
			*(volatile uint64_t *)chunk = (next & ~0xfffULL) | (a->chunk_size >> 12);
			sync_lines((volatile void *)chunk, 64);
		}
		next = va;
	}
	a->ctx_va = ctx_va;
	a->first_chunk_va = next;
	heaps[heap_count] = (struct heap){ vm, ctx_va, a->chunk_size, a->initial_chunk_count, a->max_chunks,
		                           a->target_in_flight };
	a->handle = ++heap_count;
	return 0;
}

static int
syncobj_request(struct mali_client *c, uint32_t nr, void *arg)
{
	struct { uint32_t handle, flags; } *create = arg;
	struct { uint64_t handles; int64_t timeout; uint32_t count, flags, first, pad; } *wait = arg;
	struct { uint64_t handles, points; int64_t timeout; uint32_t count, flags, first, pad; } *twait = arg;
	struct { uint64_t handles; uint32_t count, pad; } *array = arg;
	struct { uint64_t handles, points; uint32_t count, flags; } *tarray = arg;
	struct { uint32_t src, dst; uint64_t src_point, dst_point; uint32_t flags, pad; } *transfer = arg;
	uint32_t handles[64];
	uint64_t points[64];
	uint32_t count = 0;
	int error = 0;

	switch (nr) {
	case DRM_SYNCOBJ_CREATE:
		for (uint32_t i = 0; i < MAX_SYNCOBJS; i++) {
			if (syncobjs[i].owner == NULL) {
				bzero(&syncobjs[i], sizeof(syncobjs[i]));
				syncobjs[i].owner = c;
				syncobjs[i].signaled = (create->flags & 1) ? 1 : 0;     /* DRM_SYNCOBJ_CREATE_SIGNALED */
				create->handle = i + 1;
				return 0;
			}
		}
		return ENOSPC;
	case DRM_SYNCOBJ_DESTROY:
		if (syncobj_get(c, create->handle) == NULL) {
			return EINVAL;
		}
		bzero(&syncobjs[create->handle - 1], sizeof(syncobjs[0]));
		return 0;
	case DRM_SYNCOBJ_TRANSFER: {
		struct syncobj *src = syncobj_get(c, transfer->src), *dst = syncobj_get(c, transfer->dst);
		struct fence f = { 0 };

		if (src == NULL || dst == NULL) {
			return EINVAL;
		}
		for (uint32_t i = 0; i < src->count; i++) {
			if (transfer->src_point == 0 ? i == src->count - 1 : src->fences[i].point >= transfer->src_point) {
				f = src->fences[i];
				break;
			}
		}
		syncobj_add(dst, transfer->dst_point, f.g, f.q, f.g ? f.seqno : 0);
		return 0;
	}
	}

	/* the rest take arrays of handles (and points) */
	if (nr == DRM_SYNCOBJ_WAIT || nr == DRM_SYNCOBJ_RESET || nr == DRM_SYNCOBJ_SIGNAL) {
		count = nr == DRM_SYNCOBJ_WAIT ? wait->count : array->count;
		error = count > 64 ? EINVAL : copyin(nr == DRM_SYNCOBJ_WAIT ? wait->handles : array->handles, handles, count * 4);
		bzero(points, sizeof(points));
	} else {
		count = nr == DRM_SYNCOBJ_TIMELINE_WAIT ? twait->count : tarray->count;
		error = count > 64 ? EINVAL : copyin(nr == DRM_SYNCOBJ_TIMELINE_WAIT ? twait->handles : tarray->handles, handles, count * 4);
		if (!error && (nr == DRM_SYNCOBJ_TIMELINE_WAIT ? twait->points : tarray->points) != 0) {
			error = copyin(nr == DRM_SYNCOBJ_TIMELINE_WAIT ? twait->points : tarray->points, points, count * 8);
		}
	}
	if (error) {
		return error;
	}
	for (uint32_t i = 0; i < count; i++) {
		struct syncobj *s = syncobj_get(c, handles[i]);

		if (s == NULL) {
			return EINVAL;
		}
		switch (nr) {
		case DRM_SYNCOBJ_RESET:
			s->count = 0;
			s->signaled = 0;
			break;
		case DRM_SYNCOBJ_SIGNAL:
		case DRM_SYNCOBJ_TIMELINE_SIGNAL:
			syncobj_add(s, points[i] ? points[i] : 0, NULL, 0, 0);
			if (points[i] == 0) {
				s->signaled = 1;
			}
			break;
		case DRM_SYNCOBJ_QUERY:
			points[i] = s->signaled;
			for (uint32_t f = 0; f < s->count; f++) {
				if (fence_done(&s->fences[f]) && s->fences[f].point > points[i]) {
					points[i] = s->fences[f].point;
				}
			}
			break;
		case DRM_SYNCOBJ_WAIT:
		case DRM_SYNCOBJ_TIMELINE_WAIT: {
			/* a timeout of 0 asks only; any other waits, here up to 10 s */
			int64_t timeout = nr == DRM_SYNCOBJ_WAIT ? wait->timeout : twait->timeout;

			error = syncobj_wait_point(s, points[i], timeout == 0 ? 0 : 10000);
			if (error) {
				return ETIME;
			}
			break;
		}
		}
	}
	if (nr == DRM_SYNCOBJ_QUERY) {
		return copyout(points, tarray->points, count * 8);
	}
	return 0;
}

/* --- for MaliGPU.cpp --- */

void *
mali_client_open(void)
{
	maligpu_lock();
	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (!clients[i].used) {
			bzero(&clients[i], sizeof(clients[i]));
			clients[i].used = true;
			maligpu_unlock();
			return &clients[i];
		}
	}
	maligpu_unlock();
	return NULL;
}

void
mali_client_close(void *client)
{
	struct mali_client *c = client;

	maligpu_lock();
	for (int i = 0; i < MAX_BOS; i++) {
		if (bos[i].owner == c) {
			bo_free(&bos[i]);
		}
	}
	for (int i = 0; i < MAX_SYNCOBJS; i++) {
		if (syncobjs[i].owner == c) {
			bzero(&syncobjs[i], sizeof(syncobjs[0]));
		}
	}
	/* its VMs and groups stay (the core cannot take them back yet): not used again */
	c->used = false;
	maligpu_unlock();
}

/* the memory mmap maps for a buffer (its handle the memory type): the IOBufferMemoryDescriptor */
void *
mali_bo_memory(void *client, uint32_t handle)
{
	struct mali_bo *bo;
	void *memory;

	maligpu_lock();
	bo = bo_get(client, handle);
	memory = bo != NULL ? bo->memory : NULL;
	maligpu_unlock();
	return memory;
}

/*
 * A buffer that is an IOSurface's memory (MaliGPU.cpp: request 0x7e, RedwoodGPU's): memory and
 * surface are the buffer's from here on -- or, when the program has that surface as a buffer
 * already, that one is the answer again (EEXIST: the caller lets go of what it brought).
 */
int
mali_bo_adopt(void *client, void *memory, vm_offset_t kva, uint64_t size, void *surface, uint32_t *handle)
{
	int error = ENOSPC;

	maligpu_lock();
	for (uint32_t i = 0; i < MAX_BOS && surface != NULL; i++) {
		if (bos[i].owner == client && bos[i].surface == surface) {
			*handle = i + 1;
			maligpu_unlock();
			return EEXIST;
		}
	}
	for (uint32_t i = 0; i < MAX_BOS; i++) {
		if (bos[i].owner == NULL) {
			bos[i] = (struct mali_bo){ client, memory, kva, size, surface, ~0U, 0, 0 };
			*handle = i + 1;
			error = 0;
			break;
		}
	}
	maligpu_unlock();
	return error;
}

/*
 * The same for a display framebuffer (MaliGPU.cpp, framebuffer_buffer), which no surface has: known
 * by its ID instead. Mesa asks for an imported buffer's handle twice (panfrost_bo_import, then
 * pan_kmod_bo_import) and must get the same one, as Linux gives for a dma-buf; a second buffer
 * left Mesa's own record of the first unmade, and the window server died in
 * panthor_kmod_bo_get_sync_point at its first submission (gl27, gl28: "signal 11 at 0x30").
 */
int
mali_bo_adopt_id(void *client, void *memory, vm_offset_t kva, uint64_t size, uint32_t id, uint32_t *handle)
{
	int error = ENOSPC;

	maligpu_lock();
	for (uint32_t i = 0; i < MAX_BOS; i++) {
		if (bos[i].owner == client && bos[i].surface == NULL && bos[i].import_id == id) {
			*handle = i + 1;
			maligpu_unlock();
			return EEXIST;
		}
	}
	for (uint32_t i = 0; i < MAX_BOS; i++) {
		if (bos[i].owner == NULL) {
			bos[i] = (struct mali_bo){ client, memory, kva, size, NULL, ~0U, 0, id };
			*handle = i + 1;
			error = 0;
			break;
		}
	}
	maligpu_unlock();
	return error;
}

/* one request: 0 or an errno */
int
mali_ioctl(void *client, uint32_t nr, void *arg, uint32_t size)
{
	struct mali_client *c = client;
	struct mali_bo *bo;
	int error = EINVAL;

	uint64_t began = mach_absolute_time();

	(void)size;
	maligpu_lock();
	switch (nr) {
	case DRM_PANTHOR(0):
		error = dev_query(arg);
		break;
	case DRM_PANTHOR(1):
		error = vm_create(c, arg);
		break;
	case DRM_PANTHOR(2):                                    /* VM_DESTROY: kept (see mali_client_close) */
	case DRM_PANTHOR(8):                                    /* GROUP_DESTROY */
	case DRM_PANTHOR(12):                                   /* TILER_HEAP_DESTROY */
	case DRM_PANTHOR(13):                                   /* BO_SET_LABEL */
		error = 0;
		break;
	case DRM_PANTHOR(3):
		error = vm_bind(c, arg);
		break;
	case DRM_PANTHOR(4):                                    /* VM_GET_STATE: usable */
		((uint32_t *)arg)[1] = 0;
		error = 0;
		break;
	case DRM_PANTHOR(5):
		error = bo_create(c, arg);
		break;
	case DRM_PANTHOR(6): {                                  /* BO_MMAP_OFFSET: the handle, as mmap's memory type */
		struct { uint32_t handle, pad; uint64_t offset; } *a = arg;

		if (bo_get(c, a->handle) != NULL) {
			a->offset = (uint64_t)a->handle << 32;
			error = 0;
		}
		break;
	}
	case DRM_PANTHOR(7):
		error = group_create(c, arg);
		break;
	case DRM_PANTHOR(9):
		error = group_submit(c, arg);
		break;
	case DRM_PANTHOR(10):
		error = group_get_state(c, arg);
		break;
	case DRM_PANTHOR(11):
		error = tiler_heap_create(c, arg);
		break;
	case DRM_GEM_CLOSE:
		if ((bo = bo_get(c, ((uint32_t *)arg)[0])) != NULL) {
			bo_free(bo);
			error = 0;
		}
		break;
	case DRM_SYNCOBJ_CREATE:
	case DRM_SYNCOBJ_DESTROY:
	case DRM_SYNCOBJ_WAIT:
	case DRM_SYNCOBJ_RESET:
	case DRM_SYNCOBJ_SIGNAL:
	case DRM_SYNCOBJ_TIMELINE_WAIT:
	case DRM_SYNCOBJ_QUERY:
	case DRM_SYNCOBJ_TRANSFER:
	case DRM_SYNCOBJ_TIMELINE_SIGNAL:
		error = syncobj_request(c, nr, arg);
		break;
	default:
		printf("mali: request %#x is not known here\n", nr);
		error = ENOTTY;
	}
	req_abs[nr & 0xff] += mach_absolute_time() - began;
	req_count[nr & 0xff]++;
	if (++req_total % 4096 == 0 && mali_stats) {   /* boot-arg malistat=1 (a console line holds the window server up) */
		requests_said();
	}
	maligpu_unlock();
	return error;
}
