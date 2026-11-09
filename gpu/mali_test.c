/*
 * mali_test: the MaliGPU service (iokit/Kernel/MaliGPU.cpp, boot-arg mali=3) as Mesa's panfrost
 * will use it -- panthor's requests as calls of the user client (the selector is the ioctl's
 * number), buffers mapped with IOConnectMapMemory64 (the memory type is the buffer's handle).
 * Device queries, a VM, two buffers mapped into this program and into the VM, a group with one
 * queue, a command stream written by this program, submitted with a sync object to signal,
 * waited for, and its result read back through this program's mapping; a tiler heap besides.
 */
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PANTHOR(n)      (0x40 + (n))
#define SYNCOBJ_CREATE  0xbf
#define SYNCOBJ_WAIT    0xc3

static io_connect_t connection;

static int
call(uint32_t nr, void *arg, size_t size)
{
	size_t out = size;
	kern_return_t r = IOConnectCallStructMethod(connection, nr, arg, size, arg, &out);

	if (r != KERN_SUCCESS) {
		printf("mali_test: request %#x: %#x\n", nr, r);
		return -1;
	}
	return 0;
}

struct obj_array { uint32_t stride, count; uint64_t array; };

int
main(void)
{
	io_service_t service = IOServiceGetMatchingService(0, IOServiceMatching("MaliGPU"));
	struct { uint32_t type, size; uint64_t pointer; } query;
	uint32_t gpu[32] = { 0 }, csif[6] = { 0 };
	struct { uint32_t flags, id; uint64_t va_range; } vm = { 0 };
	struct { uint64_t size; uint32_t flags, vm, handle, pad; } bo[2];
	struct { uint32_t flags, bo; uint64_t offset, va, size; struct obj_array syncs; } binds[2];
	struct { uint32_t vm, flags; struct obj_array ops; } bind;
	struct { uint32_t handle, flags; } sync = { 0 };
	struct { uint8_t priority, pad[3]; uint32_t ring; } queue = { 1, { 0 }, 0x4000 };
	struct {
		struct obj_array queues;
		uint8_t compute, fragment, tiler, priority;
		uint32_t pad;
		uint64_t compute_mask, fragment_mask, tiler_mask;
		uint32_t vm, handle;
	} group;
	struct { uint32_t flags, handle; uint64_t point; } signal;
	struct { uint32_t queue, size; uint64_t addr; uint32_t flush, pad; struct obj_array syncs; } qsubmit;
	struct { uint32_t group, pad; struct obj_array submits; } submit;
	struct { uint64_t handles; int64_t timeout; uint32_t count, flags, first, pad; } wait;
	struct { uint32_t vm, initial, chunk, max, target, handle; uint64_t ctx, first; } heap;
	mach_vm_address_t mapped[2] = { 0 };
	mach_vm_size_t mapped_size[2];
	uint64_t shaders;
	uint64_t *stream;
	volatile uint32_t *target;

	if (service == 0 || IOServiceOpen(service, mach_task_self(), 0, &connection) != KERN_SUCCESS) {
		printf("mali_test: no MaliGPU service\n");
		return 1;
	}
	query = (typeof(query)){ 0, sizeof(gpu), (uintptr_t)gpu };
	if (call(PANTHOR(0), &query, sizeof(query))) {
		return 1;
	}
	query = (typeof(query)){ 1, sizeof(csif), (uintptr_t)csif };
	if (call(PANTHOR(0), &query, sizeof(query))) {
		return 1;
	}
	shaders = gpu[18] | (uint64_t)gpu[19] << 32;          /* shader_present: words 18-19 */
	printf("mali_test: GPU %#x, shader cores %#llx; %u groups of %u streams, %u registers\n", gpu[0],
	    (unsigned long long)shaders, csif[0], csif[1], csif[2]);

	if (call(PANTHOR(1), &vm, sizeof(vm))) {
		return 1;
	}
	for (int i = 0; i < 2; i++) {
		memset(&bo[i], 0, sizeof(bo[i]));
		bo[i].size = 0x4000;
		if (call(PANTHOR(5), &bo[i], sizeof(bo[i])) ||
		    IOConnectMapMemory64(connection, bo[i].handle, mach_task_self(), &mapped[i], &mapped_size[i],
		    kIOMapAnywhere) != KERN_SUCCESS) {
			printf("mali_test: buffer %d\n", i);
			return 1;
		}
		memset(&binds[i], 0, sizeof(binds[i]));
		binds[i].bo = bo[i].handle;
		binds[i].va = 0x10000000ULL + 0x10000ULL * i;
		binds[i].size = 0x4000;
	}
	bind = (typeof(bind)){ vm.id, 0, { sizeof(binds[0]), 2, (uintptr_t)binds } };
	if (call(PANTHOR(3), &bind, sizeof(bind)) || call(SYNCOBJ_CREATE, &sync, sizeof(sync))) {
		return 1;
	}
	memset(&group, 0, sizeof(group));
	group.queues = (struct obj_array){ sizeof(queue), 1, (uintptr_t)&queue };
	group.compute = group.fragment = 7;
	group.tiler = 1;
	group.priority = 1;
	group.compute_mask = group.fragment_mask = shaders;
	group.tiler_mask = 1;
	group.vm = vm.id;
	if (call(PANTHOR(7), &group, sizeof(group))) {
		return 1;
	}
	printf("mali_test: VM %u, buffers %u and %u (mapped at %#llx, %#llx), group %u\n", vm.id, bo[0].handle,
	    bo[1].handle, (unsigned long long)mapped[0], (unsigned long long)mapped[1], group.handle);

	/* the stream: 0x5ee0beef stored at the second buffer's start (r2:r3 its address) */
	stream = (uint64_t *)mapped[0];
	stream[0] = (1ULL << 56) | (2ULL << 48) | binds[1].va;                  /* MOVE48 r2 */
	stream[1] = (2ULL << 56) | (0ULL << 48) | 0x5ee0beefU;                  /* MOVE32 r0 */
	stream[2] = (21ULL << 56) | (0ULL << 48) | (2ULL << 40) | (1ULL << 16);  /* STORE_MULTIPLE r0 -> [r2] */
	stream[3] = (3ULL << 56) | (0xffULL << 16);                             /* WAIT all */
	signal = (typeof(signal)){ 1u << 31, sync.handle, 0 };
	qsubmit = (typeof(qsubmit)){ 0, 32, binds[0].va, 0, 0, { sizeof(signal), 1, (uintptr_t)&signal } };
	submit = (typeof(submit)){ group.handle, 0, { sizeof(qsubmit), 1, (uintptr_t)&qsubmit } };
	if (call(PANTHOR(9), &submit, sizeof(submit))) {
		return 1;
	}
	wait = (typeof(wait)){ (uintptr_t)&sync.handle, INT64_MAX, 1, 0, 0, 0 };
	if (call(SYNCOBJ_WAIT, &wait, sizeof(wait))) {
		return 1;
	}
	target = (volatile uint32_t *)mapped[1];
	printf("mali_test: the job is done; the buffer holds %#x -- %s\n", target[0],
	    target[0] == 0x5ee0beef ? "A PROGRAM DREW... WELL, STORED, ON THE GPU THROUGH MaliGPU" : "not what was stored");

	heap = (typeof(heap)){ vm.id, 2, 0x40000, 8, 4, 0, 0, 0 };
	if (call(PANTHOR(11), &heap, sizeof(heap)) == 0) {
		printf("mali_test: tiler heap %u: context %#llx, first chunk %#llx\n", heap.handle,
		    (unsigned long long)heap.ctx, (unsigned long long)heap.first);
	}
	return 0;
}
