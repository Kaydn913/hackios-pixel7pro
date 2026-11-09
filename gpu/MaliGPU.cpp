/*
 * MaliGPU: the Pixel 7 Pro's Mali-G710 for programs to draw with (boot-arg mali=3).
 *
 * bsd/dev/redwood_mali.c wakes the GPU, starts its firmware and calls MaliGPUStart(). A program
 * that opens this service speaks the interface of Linux's panthor DRM driver -- what Mesa's
 * panfrost knows: every request is a call of the user client, the request's number (the low
 * byte of its ioctl code) the selector, its structure passed in and back. The requests are
 * answered in C (redwood_mali_uapi.c); here is only IOKit: the service, a client per program,
 * buffers that can be mapped into it (the memory type of a mapping is the buffer's handle), and
 * the lock everything is done under. Shaped after RedwoodGPU.cpp, which does the same for the
 * Adreno with msm's interface.
 *
 * A buffer is write-combined memory, for the processor as for the program: what either writes
 * goes to memory, where the GPU (not coherent with them) reads it.
 */
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <mach/vm_param.h>
#include <sys/errno.h>

#include "MiniIOSurface.h"

extern "C" {
void *mali_client_open(void);
void mali_client_close(void *client);
void *mali_bo_memory(void *client, uint32_t handle);
int mali_ioctl(void *client, uint32_t nr, void *arg, uint32_t size);
int mali_bo_adopt(void *client, void *memory, vm_offset_t kva, uint64_t size, void *surface, uint32_t *handle);
int mali_bo_adopt_id(void *client, void *memory, vm_offset_t kva, uint64_t size, uint32_t id, uint32_t *handle);
uint32_t maligpu_surface_seed(void *surface);
bool maligpu_surface_shown(void *surface);
void maligpu_surface_drop(void *surface);
void MaliGPUStart(void);
void *maligpu_buffer(uint64_t size, vm_offset_t *kva);
void maligpu_buffer_release(void *buffer);
void maligpu_lock(void);
void maligpu_unlock(void);
}

static IOLock *gMaliLock;

void
maligpu_lock(void)
{
	IOLockLock(gMaliLock);
}

void
maligpu_unlock(void)
{
	IOLockUnlock(gMaliLock);
}

void *
maligpu_buffer(uint64_t size, vm_offset_t *kva)
{
	IOBufferMemoryDescriptor *memory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
	    kIODirectionInOut | kIOMemoryKernelUserShared | kIOMapWriteCombineCache, (vm_size_t)size, PAGE_SIZE);

	if (memory == NULL) {
		return NULL;
	}
	bzero(memory->getBytesNoCopy(), (size_t)size);
	*kva = (vm_offset_t)memory->getBytesNoCopy();
	return memory;
}

void
maligpu_buffer_release(void *buffer)
{
	if (buffer != NULL) {
		((IOMemoryDescriptor *)buffer)->release();
	}
}

uint32_t
maligpu_surface_seed(void *surface)
{
	return ((MiniIOSurface *)surface)->shared()->seed;
}

bool
maligpu_surface_shown(void *surface)
{
	return MiniIOSurfaceShown((MiniIOSurface *)surface);
}

void
maligpu_surface_drop(void *surface)
{
	MiniIOSurfaceDrop((MiniIOSurface *)surface);
}

/*
 * Request 0x7e, RedwoodGPU's (iokit/Kernel/RedwoodGPU.cpp, surfaceBuffer()): an IOSurface's
 * memory as a buffer -- its pixels, from the page after the surface's shared state on -- or, with
 * handle 0xffffffff, only what the surface is like. Linux hands such memory around as file
 * descriptors; here it has a name already (src/redwood-drm passes it where the descriptor goes).
 */
struct mali_surface {
	uint32_t surface, handle;
	uint64_t size;
	uint32_t offset, rowBytes, width, height, pixelFormat, planes;
};

#define kMaliSurface            0x7e
#define kMaliDescribe           0xffffffffu

/* MiniIOMobileFramebuffer.cpp's: its framebuffers as GPU buffers, the IOSurface IDs 0x7fff0000 and 0x7fff0001 */
bool MiniIOMobileFramebufferGPUBuffer(uint32_t id, IOMemoryDescriptor **memory, vm_offset_t *kva, uint32_t *width,
    uint32_t *height, uint32_t *rowBytes, uint32_t *bytes);

/*
 * The display's framebuffer k as a buffer (decon=3, eagl.c's glscale: the window server's GPU draws
 * its picture, scaled, into the buffer the panel is sent next): blue-first pixels, the panel's size.
 */
static int
framebuffer_buffer(void *client, struct mali_surface *request)
{
	IOMemoryDescriptor *memory = NULL;
	vm_offset_t kva;
	uint32_t width, height, rowBytes, bytes;
	int error;

	if (!MiniIOMobileFramebufferGPUBuffer(request->surface, &memory, &kva, &width, &height, &rowBytes, &bytes)) {
		return ENOENT;
	}
	request->size = bytes;
	request->offset = 0;
	request->rowBytes = rowBytes;
	request->width = width;
	request->height = height;
	request->pixelFormat = 0x42475241;      /* 'BGRA' */
	request->planes = 0;
	if (request->handle == kMaliDescribe) {
		request->handle = 0;
		memory->release();
		return 0;
	}
	error = mali_bo_adopt_id(client, memory, kva, bytes, request->surface, &request->handle);
	if (error != 0) {
		memory->release();              /* (EEXIST: it is a buffer already, whose handle that is) */
	}
	return error == EEXIST ? 0 : error;
}

static int
surface_buffer(void *client, struct mali_surface *request)
{
	MiniIOSurface *s = MiniIOSurfaceCopy(request->surface);
	IOMemoryDescriptor *memory;
	int error;

	if (s == NULL) {
		return (request->surface & 0xffff0000u) == 0x7fff0000u ? framebuffer_buffer(client, request) : ENOENT;
	}
	if (s->client != NULL) {        /* (a program's own memory with a surface made around it) */
		MiniIOSurfaceDrop(s);
		return ENOTSUP;
	}
	request->size = round_page_64(s->info.allocSize);
	request->offset = s->info.offset;
	request->rowBytes = s->info.bytesPerRow;
	request->width = s->info.width;
	request->height = s->info.height;
	request->pixelFormat = s->info.pixelFormat;
	request->planes = s->info.planeCount;
	if (request->handle == kMaliDescribe || request->size == 0) {
		request->handle = 0;
		MiniIOSurfaceDrop(s);
		return request->size == 0 ? ENOMEM : 0;
	}
	memory = IOSubMemoryDescriptor::withSubRange(s->memory, PAGE_SIZE, request->size, kIODirectionInOut);
	if (memory == NULL) {
		MiniIOSurfaceDrop(s);
		return ENOMEM;
	}
	error = mali_bo_adopt(client, memory, (vm_offset_t)(s->kbase + PAGE_SIZE), request->size, s, &request->handle);
	if (error != 0) {
		memory->release();
		MiniIOSurfaceDrop(s);
	}
	return error == EEXIST ? 0 : error;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class MaliGPU : public IOService
{
	OSDeclareDefaultStructors(MaliGPU);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(MaliGPU, IOService);

bool
MaliGPU::start(IOService * provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "MaliGPUUserClient");
	setName("MaliGPU");
	registerService();
	return true;
}

void
MaliGPUStart(void)
{
	IOService * provider = IOService::getPlatform();
	MaliGPU * gpu;

	gMaliLock = IOLockAlloc();
	gpu = OSTypeAlloc(MaliGPU);
	if (gMaliLock == NULL || gpu == NULL || provider == NULL || !gpu->init() || !gpu->attach(provider)) {
		OSSafeReleaseNULL(gpu);
		IOLog("MaliGPU: cannot create the service\n");
		return;
	}
	if (!gpu->start(provider)) {
		gpu->detach(provider);
		IOLog("MaliGPU: the service did not start\n");
	} else {
		IOLog("MaliGPU: the service is up\n");
	}
	gpu->release();
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class MaliGPUUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(MaliGPUUserClient);

	void * fClient;

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(MaliGPUUserClient, IOUserClient);

bool
MaliGPUUserClient::start(IOService * provider)
{
	if (!IOUserClient::start(provider)) {
		return false;
	}
	fClient = mali_client_open();
	return fClient != NULL;
}

IOReturn
MaliGPUUserClient::clientClose(void)
{
	if (fClient != NULL) {
		mali_client_close(fClient);
		fClient = NULL;
	}
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

IOReturn
MaliGPUUserClient::clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory)
{
	IOMemoryDescriptor * m = fClient != NULL ? (IOMemoryDescriptor *)mali_bo_memory(fClient, type) : NULL;

	if (m == NULL) {
		return kIOReturnBadArgument;
	}
	m->retain();
	*memory = m;
	*options = 0;
	return kIOReturnSuccess;
}

/* the request's errno, if any, as a vendor-specific IOReturn (0xe0008000 | errno) */
IOReturn
MaliGPUUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	uint8_t request[256];
	uint32_t size = args->structureInputSize;
	int error;

	if (fClient == NULL || args->structureInput == NULL || size > sizeof(request)) {
		return kIOReturnBadArgument;
	}
	bzero(request, sizeof(request));
	memcpy(request, args->structureInput, size);
	error = selector == kMaliSurface && size >= sizeof(struct mali_surface) ?
	    surface_buffer(fClient, (struct mali_surface *)request) : mali_ioctl(fClient, selector, request, size);
	if (args->structureOutput != NULL) {
		uint32_t out = args->structureOutputSize < size ? args->structureOutputSize : size;

		memcpy(args->structureOutput, request, out);
		args->structureOutputSize = out;
	}
	return error ? iokit_vendor_specific_err(error) : kIOReturnSuccess;
}
