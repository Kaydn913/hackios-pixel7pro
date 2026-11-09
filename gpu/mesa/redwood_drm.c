/*
 * The libdrm of xf86drm.h (see there): Linux's DRM requests to the kernel's GPU driver as calls
 * of its IOKit user client. A request's number (the low byte of the ioctl code) is the call's
 * selector, its structure goes in and comes back as it is; a buffer's memory is the client
 * memory of the type that is the buffer's handle.
 *
 * freedreno knows its device by a file descriptor. It gets one that means nothing (/dev/null):
 * there is one GPU and one connection to it in a process.
 *
 * The same for panfrost on the Pixel 7 Pro's Mali (drmOpenWithType("panthor"): the service
 * MaliGPU, iokit/Kernel/MaliGPU.cpp, which speaks panthor's interface): sync objects are the
 * kernel's too, and a buffer is mapped by redwood_drm_mmap() where panfrost would mmap() the
 * device at the offset panthor gave it (the buffer's handle in its top 32 bits). The kernel says
 * what went wrong as an errno inside a vendor-specific IOReturn.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <IOKit/IOKitLib.h>

#include "xf86drm.h"

#define REDWOOD_GPU_SERVICE     "RedwoodGPU"
#define REDWOOD_GPU_SURFACE     0x7e            /* the request that is not Linux's */
#define REDWOOD_GPU_DESCRIBE    0xffffffffu     /* ... asked only what the buffer would be like */

#define MALI_GPU_SERVICE        "MaliGPU"

static io_connect_t connection;
static int connection_fd = -1;
static bool panthor;                            /* the Mali's (MaliGPU), not the Adreno's */

int
drmOpenWithType(const char *name, const char *busid, int type)
{
	io_service_t service;

	(void)busid;
	(void)type;
	if (strcmp(name, "msm") != 0 && strcmp(name, "panthor") != 0) {
		return -1;
	}
	if (connection_fd >= 0) {
		return dup(connection_fd);
	}
	panthor = strcmp(name, "panthor") == 0;
	service = IOServiceGetMatchingService(0, IOServiceMatching(panthor ? MALI_GPU_SERVICE : REDWOOD_GPU_SERVICE));
	if (service == 0) {
		return -1;
	}
	if (IOServiceOpen(service, mach_task_self(), 0, &connection) != 0) {
		IOObjectRelease(service);
		return -1;
	}
	IOObjectRelease(service);
	connection_fd = open("/dev/null", O_RDWR);
	return dup(connection_fd);
}

static int
call(unsigned number, void *data, size_t size)
{
	size_t returned = size;
	kern_return_t result = IOConnectCallStructMethod(connection, number, data, size, data, &returned);

	if ((result & ~0x3fff) == iokit_vendor_specific_err(0)) {
		return result & 0x3fff;                 /* MaliGPU's errno */
	}
	switch (result) {
	case 0:
		return 0;
	case kIOReturnNoMemory:
		return ENOMEM;
	case kIOReturnBadArgument:
		return EINVAL;
	case kIOReturnTimeout:
		return ETIMEDOUT;
	case kIOReturnBusy:
		return EBUSY;
	case kIOReturnNotFound:
		return ENOENT;
	case kIOReturnUnsupported:
		return ENOSYS;
	default:
		return EIO;
	}
}

int
drmIoctl(int fd, unsigned long request, void *arg)
{
	int error = call((unsigned)(request & 0xff), arg, IOCPARM_LEN(request));

	(void)fd;
	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}

int
drmCommandWrite(int fd, unsigned long index, const void *data, unsigned long size)
{
	(void)fd;
	return -call((unsigned)(DRM_COMMAND_BASE + index), (void *)(uintptr_t)data, size);
}

int
drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size)
{
	(void)fd;
	return -call((unsigned)(DRM_COMMAND_BASE + index), data, size);
}

/*
 * The driver calls itself what Linux's is called, and is of the version whose interface it has:
 * 1.7, buffers at addresses the kernel chooses and tells ("softpin"), nothing newer.
 */
drmVersionPtr
drmGetVersion(int fd)
{
	drmVersionPtr version = calloc(1, sizeof(*version));

	(void)fd;
	if (version != NULL) {
		/* panthor 1.2: timestamps and group priorities asked, nothing newer (redwood_mali_uapi.c) */
		version->version_major = 1;
		version->version_minor = panthor ? 2 : 7;
		version->name = strdup(panthor ? "panthor" : "msm");
		version->name_len = (int)strlen(version->name);
		version->date = strdup("");
		version->desc = strdup(panthor ? "Mali-G710 on Darwin" : "Adreno on Darwin");
		version->desc_len = (int)strlen(version->desc);
	}
	return version;
}

void
drmFreeVersion(drmVersionPtr version)
{
	if (version != NULL) {
		free(version->name);
		free(version->date);
		free(version->desc);
		free(version);
	}
}

int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
	(void)fd;
	(void)capability;
	*value = 0;
	return 0;
}

void *
redwood_drm_map(int fd, uint32_t handle, uint64_t size)
{
	mach_vm_address_t address = 0;
	mach_vm_size_t mapped = 0;

	(void)fd;
	(void)size;
	/*
	 * Linux gives freedreno its buffers write-combined, with no cache of this processor's between
	 * the program and the memory the GPU sees. So does the kernel's driver here: the memory is
	 * made that way, and this mapping is as the memory is.
	 */
	if (IOConnectMapMemory64(connection, handle, mach_task_self(), &address, &mapped, kIOMapAnywhere) != 0) {
		return (void *)-1;              /* mmap()'s MAP_FAILED */
	}
	return (void *)(uintptr_t)address;
}

void
redwood_drm_unmap(int fd, uint32_t handle, void *address)
{
	(void)fd;
	IOConnectUnmapMemory64(connection, handle, mach_task_self(), (mach_vm_address_t)(uintptr_t)address);
}

int
redwood_drm_surface(int fd, struct redwood_drm_surface *surface)
{
	int error;

	(void)fd;
	surface->handle = REDWOOD_GPU_DESCRIBE;
	error = call(REDWOOD_GPU_SURFACE, surface, sizeof(*surface));
	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}

int
drmPrimeFDToHandle(int fd, int surface, uint32_t *handle)
{
	struct redwood_drm_surface request = { .surface = (uint32_t)surface };
	int error = call(REDWOOD_GPU_SURFACE, &request, sizeof(request));

	(void)fd;
	if (error != 0) {
		errno = error;
		return -1;
	}
	*handle = request.handle;
	return 0;
}

static int
none(void)
{
	errno = ENOSYS;
	return -1;
}

/* Buffers and fences as file descriptors, for handing them to other programs and drivers: none of that here. */
#define NONE(name, ...) int name(__VA_ARGS__) { return none(); }
NONE(drmPrimeHandleToFD, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) uint32_t flags, __attribute__((unused)) int *prime_fd)
NONE(drmSyncobjFDToHandle, __attribute__((unused)) int fd, __attribute__((unused)) int obj_fd,
    __attribute__((unused)) uint32_t *handle)
NONE(drmSyncobjHandleToFD, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) int *obj_fd)

/*
 * ... but for panthor, a sync file, which panfrost makes of a context's last job to have a fence
 * (pan_fence.c: glFinish waits on it): a descriptor of /dev/null standing for a sync object of its
 * own that holds that job. The object outlives the descriptor's close() until the number is reused.
 */
#define MAX_SYNC_FILES          256

static uint32_t sync_files[MAX_SYNC_FILES];     /* by descriptor: the sync object; 0, none */

int
drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
	uint32_t copy;
	int file;

	if (!panthor) {
		return none();
	}
	if (drmSyncobjCreate(fd, 0, &copy) != 0) {
		return -1;
	}
	if (drmSyncobjTransfer(fd, copy, 0, handle, 0, 0) != 0 || (file = open("/dev/null", O_RDONLY | O_CLOEXEC)) < 0) {
		drmSyncobjDestroy(fd, copy);
		return -1;
	}
	if (file >= MAX_SYNC_FILES) {
		close(file);
		drmSyncobjDestroy(fd, copy);
		errno = EMFILE;
		return -1;
	}
	if (sync_files[file] != 0) {
		drmSyncobjDestroy(fd, sync_files[file]);        /* the number's last one, closed since */
	}
	sync_files[file] = copy;
	*sync_file_fd = file;
	return 0;
}

int
drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
	if (!panthor) {
		return none();
	}
	if (sync_file_fd < 0 || sync_file_fd >= MAX_SYNC_FILES || sync_files[sync_file_fd] == 0) {
		errno = EINVAL;
		return -1;
	}
	return drmSyncobjTransfer(fd, handle, 0, sync_files[sync_file_fd], 0, 0);
}

/*
 * Sync objects, the kernel's (MaliGPU has them; RedwoodGPU does not, and says so). As libdrm's:
 * 0, or -1 with errno -- the waits, -errno.
 */
int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	struct drm_syncobj_create args = { .flags = flags };
	int ret = drmIoctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &args);

	if (ret == 0) {
		*handle = args.handle;
	}
	return ret;
}

int
drmSyncobjDestroy(int fd, uint32_t handle)
{
	struct drm_syncobj_destroy args = { .handle = handle };

	return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &args);
}

int
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count)
{
	struct drm_syncobj_array args = { .handles = (uintptr_t)handles, .count_handles = count };

	return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_RESET, &args);
}

int
drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t count)
{
	struct drm_syncobj_array args = { .handles = (uintptr_t)handles, .count_handles = count };

	return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_SIGNAL, &args);
}

int
drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points, uint32_t count)
{
	struct drm_syncobj_timeline_array args = {
		.handles = (uintptr_t)handles, .points = (uintptr_t)points, .count_handles = count,
	};

	return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &args);
}

int
drmSyncobjWait(int fd, uint32_t *handles, unsigned count, int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
	struct drm_syncobj_wait args = {
		.handles = (uintptr_t)handles, .timeout_nsec = timeout_nsec, .count_handles = count, .flags = flags,
	};

	if (drmIoctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &args) < 0) {
		return -errno;
	}
	if (first_signaled != NULL) {
		*first_signaled = args.first_signaled;
	}
	return 0;
}

int
drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned count, int64_t timeout_nsec,
    unsigned flags, uint32_t *first_signaled)
{
	struct drm_syncobj_timeline_wait args = {
		.handles = (uintptr_t)handles, .points = (uintptr_t)points, .timeout_nsec = timeout_nsec,
		.count_handles = count, .flags = flags,
	};

	if (drmIoctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &args) < 0) {
		return -errno;
	}
	if (first_signaled != NULL) {
		*first_signaled = args.first_signaled;
	}
	return 0;
}

int
drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points, uint32_t count, uint32_t flags)
{
	struct drm_syncobj_timeline_array args = {
		.handles = (uintptr_t)handles, .points = (uintptr_t)points, .count_handles = count, .flags = flags,
	};

	return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_QUERY, &args);
}

int
drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t count)
{
	return drmSyncobjQuery2(fd, handles, points, count, 0);
}

int
drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle, uint64_t src_point,
    uint32_t flags)
{
	struct drm_syncobj_transfer args = {
		.src_handle = src_handle, .dst_handle = dst_handle, .src_point = src_point, .dst_point = dst_point,
		.flags = flags,
	};

	return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_TRANSFER, &args);
}

static void forget_mappings(uint32_t handle);

int
drmCloseBufferHandle(int fd, uint32_t handle)
{
	struct drm_gem_close args = { .handle = handle };

	forget_mappings(handle);                /* (the kernel gives the number to the next buffer) */
	return drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &args);
}

/*
 * mmap() of the device, for panthor: the offset is the buffer's (its handle in the top 32 bits,
 * the place in it below), or the page of the GPU's LATEST_FLUSH_ID register, which here is a page
 * of zeros -- flush ID 0, caches flushed every time. Mappings are remembered for munmap().
 */
#define FLUSH_ID_OFFSET         (1ull << 56)    /* DRM_PANTHOR_USER_FLUSH_ID_MMIO_OFFSET (64-bit) */
#define MAX_MAPPINGS            4096

static struct mapping {
	void *address;
	uint32_t handle;
	uint64_t size;
} mappings[MAX_MAPPINGS];

void *
redwood_drm_mmap(void *address, size_t size, int prot, int flags, int fd, off_t offset)
{
	uint64_t o = (uint64_t)offset;
	uint32_t handle = (uint32_t)(o >> 32);
	mach_vm_address_t base = 0;
	mach_vm_size_t mapped = 0;
	uint8_t *at;

	(void)address;
	(void)prot;
	(void)flags;
	(void)fd;
	if (o >= FLUSH_ID_OFFSET) {             /* panthor's registers: the flush ID page */
		static uint32_t zeros[4096 / 4];

		return zeros;
	}
	for (int i = 0; i < MAX_MAPPINGS; i++) {
		if (mappings[i].address != NULL && mappings[i].handle == handle && (o & 0xffffffffu) + size <= mappings[i].size) {
			return (uint8_t *)mappings[i].address + (o & 0xffffffffu);      /* mapped before: the same */
		}
	}
	if (IOConnectMapMemory64(connection, handle, mach_task_self(), &base, &mapped, kIOMapAnywhere) != 0 ||
	    (o & 0xffffffffu) + size > mapped) {
		errno = EINVAL;
		return (void *)-1;              /* MAP_FAILED */
	}
	at = (uint8_t *)(uintptr_t)base + (o & 0xffffffffu);
	for (int i = 0; i < MAX_MAPPINGS; i++) {
		if (mappings[i].address == NULL) {
			mappings[i].address = (void *)(uintptr_t)base;
			mappings[i].handle = handle;
			mappings[i].size = mapped;
			break;
		}
	}
	return at;
}

/*
 * munmap() of a buffer: the mapping is kept, for the buffer's next mmap(). panfrost unmaps a
 * buffer when it puts it in its cache and maps it again when it takes it out -- on Linux a
 * mapping costs nothing until it is touched; here IOConnectMapMemory64 makes the whole mapping at
 * once, five times a frame (gl7). It goes when the buffer does (drmCloseBufferHandle).
 */
int
redwood_drm_munmap(void *address, size_t size)
{
	(void)address;
	(void)size;
	return 0;
}

static void
forget_mappings(uint32_t handle)
{
	for (int i = 0; i < MAX_MAPPINGS; i++) {
		if (mappings[i].address != NULL && mappings[i].handle == handle) {
			IOConnectUnmapMemory64(connection, mappings[i].handle, mach_task_self(),
			    (mach_vm_address_t)(uintptr_t)mappings[i].address);
			mappings[i].address = NULL;
		}
	}
}
