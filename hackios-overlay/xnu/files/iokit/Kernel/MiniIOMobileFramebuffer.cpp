/*
 * MiniIOMobileFramebuffer: a stand-in for Apple's display driver family, as seen by
 * IOMobileFramebuffer.framework (and through it by CoreAnimation's window server).
 *
 * The display is the framebuffer the boot loader left us (the one the kernel's text console
 * draws on). There is no hardware behind it that we drive: a "swap" copies the pixels of the
 * surfaces of its layers into that memory, and "vsync" is a timer.
 *
 * The protocol (selectors and the layout of the swap request) was worked out from the
 * user-space side of iOS 16.5. Everything not implemented is logged once.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <kern/thread_call.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/proc.h>

extern "C" {
#include <console/video_console.h>
extern struct vc_info vinfo;            /* the boot framebuffer, as the text console knows it */
}

#include <IOKit/IOBufferMemoryDescriptor.h>

#include "MiniIOSurface.h"

extern "C" void redwood_tl(const char * format, ...) __printflike(1, 2);

/* IOConnectCallMethod selectors of the framebuffer user client (iOS 16.5) */
enum {
	kFBMethodGetLayerDefaultSurface = 3,
	kFBMethodSwapBegin              = 4,
	kFBMethodSwapEnd                = 5,
	kFBMethodSwapWait               = 6,
	kFBMethodGetID                  = 7,
	kFBMethodGetDisplaySize         = 8,
	kFBMethodRequestPowerChange     = 12,
	kFBMethodSwapSignal             = 20,
	kFBMethodGetDigitalOutState     = 25,
	kFBMethodGetDotPitch            = 28,
	kFBMethodGetDisplayArea         = 29,
	kFBMethodSwapCancel             = 52,
	kFBMethodGetColorRemapMode      = 57,
	kFBMethodNotifications          = 72,
	kFBMethodChangeFrameInfo        = 73,
	kFBMethodSupportedFrameInfo     = 74,
	kFBMethodGetCurrentAbsoluteTime = 77,
	kFBMethodGetBlock               = 79,
	kFBMethodSwapCancelAll          = 81,
	kFBMethodGetProtectionOptions   = 82,
	/* ours (eagl.c's glscale, decon=3): see gpuFrameBegin() */
	kFBMethodGPUFrameBegin          = 100,  /* in: the surface the frame is of; out: buffer (or -1), its surface ID for the GPU, width, height, row bytes */
	kFBMethodGPUFrameDone           = 101,  /* in: buffer, the surface whose frame it holds (0: none) */
};

/* The swap request (IOMobileFramebufferSwapEnd): 0x46c bytes, of which we read these. */
#define SWAP_SIZE               0x46c
#define SWAP_ID                 0x50
#define SWAP_SURFACE(layer)     (0x54 + 4 * (layer))    /* surface number, 0 = none */
#define SWAP_SOURCE(layer)      (0x64 + 16 * (layer))   /* x, y, width, height in the surface */
#define SWAP_ENABLED(layer)     (0xa4 + 4 * (layer))
#define SWAP_DEST(layer)        (0xc4 + 16 * (layer))   /* x, y, width, height on the display */
#define SWAP_LAYERS_SET         0x104                   /* bit per layer given in this swap */
#define SWAP_LAYERS_CLEARED     0x108
#define SWAP_LAYERS             4

#define kFBNotifyVSync          5
#define kFBNotifyTypes          8
#define kFBRefreshHz            60                      /* unless boot-arg fb_hz says otherwise */

#define FRAME_TIMES             64                      /* milliseconds told apart between two frames */
#define FRAME_PAUSE             90                      /* from so many on, nothing was moving: not a slow frame */
#define SWAP_GPU_MS             500                     /* a frame the GPU has not drawn by then is shown as it is */
#define SWAP_HELD_MS            700                     /* a swap that cannot be taken by then is not shown at all */

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

static uint64_t gFBLogged[2];           /* selectors already reported */
static uint8_t  gFBCalls[128];          /* how many calls of each selector were traced */

/*
 * The display user space is told about. The system carries the identity of the iPhone X
 * (see the loader), and iOS knows what screen that phone has: with any other size
 * MobileGestalt reports an unknown screen class and the user interface has no layout for it.
 * So the display is always the iPhone's, and a swap scales the picture to the panel that is
 * really there (in QEMU the loader makes the panel exactly this size and nothing is scaled).
 * Boot-args fbwidth= and fbheight= change the size reported.
 */
#define kFBDefaultWidth         1125
#define kFBDefaultHeight        2436
#define kFBDotsPerInch          458

static TUNABLE(uint32_t, gFBWidth, "fbwidth", kFBDefaultWidth);
static TUNABLE(uint32_t, gFBHeight, "fbheight", kFBDefaultHeight);
/*
 * Boot-arg fbscale=2: the same 375 x 812 points drawn at two pixels a point instead of three.
 * Everything that is drawn is drawn by the processor, and this is 2.25 times fewer pixels to
 * draw; the picture is then enlarged to the panel and is that much less sharp.
 */
#define kFBPointsWide           375
#define kFBPointsHigh           812
#define kFBDefaultScale         3
static TUNABLE(uint32_t, gFBScale, "fbscale", 0);
/* Boot-arg fbtrace=N: log the first N calls of every selector (default 3). */
static TUNABLE(uint32_t, gFBTrace, "fbtrace", 3);

/*
 * A recording of the screen, for a machine one cannot look at while it runs (the phone is
 * reached only through the boot loader, before and after the kernel).
 * Boot-args rec_secs=N [rec_start=S] [rec_mb=M]: from S seconds after the first frame, for N
 * seconds, every frame that is shown is also kept -- one pixel of every REC_STEP in each
 * direction, three bytes a pixel -- in a piece of memory of M megabytes (default 480, which
 * is 520 frames) that is left in place when the kernel returns to the loader. The loader
 * hands it to the host ("xl recording"), where tools/recording.py makes a video of it.
 *
 *   struct rec_header, then per frame: struct rec_frame, width * height pixels (R, G, B)
 *
 * Boot-arg rec_ring=1: when the memory is full the recording does not stop but goes on over its
 * oldest frames, so that what it holds at the end is the last of the run: for something that
 * happens once in a long session, the run is ended right after it (five fingers) and the frames
 * before it are there. The header ("RWREC003") says where the oldest frame is (frames are all
 * the same size, one after the other) and when the recording began, in the kernel's time, for the
 * frames to be matched with what the window server's per-frame lines (glframes=1,
 * userland/src/eagl.c) say. rec_step=N (2 to 8, default 3) keeps one pixel of every N instead of 3:
 * N = 4 holds 1.8 times as many frames, 6 four times. The memory is taken from the system's for the
 * whole run: 640 MB of it under a long session of applications had iOS killing processes for
 * memory, SpringBoard and the window server among them (two respring in one run); 256 did not.
 */
static TUNABLE(uint32_t, gRecSeconds, "rec_secs", 0);
static TUNABLE(uint32_t, gRecStart, "rec_start", 0);
static TUNABLE(uint32_t, gRecMegabytes, "rec_mb", 480);
static TUNABLE(uint32_t, gRecRing, "rec_ring", 0);
static TUNABLE(uint32_t, gRecStep, "rec_step", 3);
static TUNABLE(uint32_t, gDiff, "fb_diff", 0);

/*
 * Boot-arg decon=1 (the Pixel 7 Pro): the panel is a command-mode one, and DECON0 sends the
 * framebuffer to it on every pulse of the panel's TE line (xnuboot unmasks that trigger). A frame
 * scaled into the framebuffer while DECON0 is sending it tears. So, as Linux's decon driver does
 * for such a panel, the trigger is masked while a frame is drawn -- once DECON0 is idle, the
 * transfer under way (if any) finished -- and unmasked after: meanwhile the panel shows the last
 * whole frame from its own memory. (cal_9845 regs-decon.h; gs201's cal_9855 keeps these.)
 */
static TUNABLE_WRITEABLE(uint32_t, gDecon, "decon", 0);    /* (decon=3 may fall back to 2) */
#define DECON0_PHYS             0x1c240000ULL
#define DECON_GLOBAL_CON        0x0020
#define DECON_IDLE_STATUS       (1u << 5)
#define DECON_TRIG_CON          0x0030
#define DECON_HW_TRIG_MASK      (1u << 4)
#define DECON_SHD_REG_UP_REQ    0x0050
#define DECON_SHD_UP_GLOBAL     (1u << 31)      /* cleared by DECON0 when a frame starts */
#define DECON_START_MS          40              /* a frame starts within a refresh of the trigger's unmasking */
#define DECON_IDLE_US           20000           /* a transfer is over within a refresh */
/*
 * Boot-arg decon_trace=N: the N frames from the 100th on, each watched for 40 ms from its trigger's
 * unmasking -- when each of these registers changed, and to what. gl15: the next TE pulse (every
 * 16.6 ms) starts the frame -- FRAME_COUNT up, SHD_UP_GLOBAL and GLOBAL_CON's idle bit cleared at
 * the same microsecond -- and 6856 us later, every time, the output FIFO (OF_LEVEL) is empty and
 * GLOBAL_CON idle again. (The frame-done interrupt's pending bit stays 0 with its source enabled and
 * the interrupt not, INT_EN 0x3000 as the bootloader leaves it; gl14.) The masker sees the start
 * 0.1 to 0.9 ms late.
 *
 * So a frame's 9 ms of scaling and the frame before's 6.9 ms of sending, one after the other, are
 * 16 ms of a 16.6 ms refresh: a frame that comes a little late misses its pulse and waits a whole
 * refresh (gl15, frames from the touch script: 24 a second, 17.6 ms each in the driver). Boot-arg
 * decon=2: the scaling begins as soon as the frame before has started to go out, and each row is
 * drawn only once DECON0 has read it -- its time worked out from the start the masker saw (late,
 * so DECON0 is further on than that says) and DECON_SEND_US for the whole frame. DECON0 reads the
 * framebuffer from the top at 2.2 us a row; the scaling, bandwidth-bound, writes 6 bands at once at
 * 17 us a row each, so only the lower bands' first rows ever wait.
 */
static TUNABLE(uint32_t, gDeconTrace, "decon_trace", 0);
#define DECON_INT_EN            0x0060
#define DECON_INT_PEND          0x0070
#define DECON_TRACE_FROM        100
#define DECON_TRACE_MS          40
#define DECON_SEND_US           7200            /* a frame read out, at most (6856 measured) */
#define DECON_CHASE_MARGIN_US   300

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

static volatile uint32_t * gDecon0;
static uint32_t gDeconLate;                     /* frames drawn without DECON0 having gone idle */
static thread_call_t gDeconMasker;              /* masks the trigger again once the frame has started */
static volatile bool gDeconSending;             /* a frame handed over, its transfer not started yet */
static uint32_t gDeconTries;
static uint32_t gDeconReleases;
static volatile uint64_t gDeconStartedAt;       /* when the masker saw the last frame start (0: it did not) */
static uint64_t gChaseFrom, gChaseSpan;         /* decon=2: row y is read by DECON0 at gChaseFrom + gChaseSpan * (y + 1) / rows */
uint32_t gStatChased;                           /* frames scaled while the one before was going out ... */
uint64_t gStatChaseWait;                        /* ... and how long their drawers waited for DECON0, together */
/*
 * Tearing, several breaks at once, with decon=1 fb_beat=0 (gl13) and decon=2 (gl17): six bands
 * scaled at once each broken where it had got to -- DECON0 reading while the scaling writes? Every
 * frame: did a frame start between the scaling's beginning and its end (FRAME_COUNT), and, decon=1,
 * did DECON0 say idle while its output FIFO was not empty (then it is waited for as well).
 */
#define DECON_FRAME_COUNT       0x0004
#define DECON_OF_LEVEL          0x02b0
static uint32_t gDeconCountAtCopy;
uint32_t gStatCopies, gStatStartedInCopy, gStatIdleButSending;
/*
 * Boot-arg decon=3: two framebuffers. With one, a frame can be scaled only once the frame before
 * has started to go out (the next TE, up to 16.6 ms), and the window server's GPU waits for the
 * surface all that time (gl22: 9.3 ms a wait). Here each frame is scaled at once into the buffer
 * DECON0 is not sending -- waiting only if a frame from it is still going out -- and at the end
 * DPP channel 0's DMA is pointed at it (RDMA_BASEADDR_Y8; its shadow, 0x400 on, is what the frame
 * under way reads) and the shadow update asked for: the next TE sends it, and a frame let go before
 * that and not sent yet is simply not shown. The trigger is never masked for the scaling. The second
 * buffer is free RAM between the boot framebuffer's end (0xfbd23a00) and dss_log (0xfd3f0000), which
 * nothing reserves. (cal_9845 regs-dpp.h, Google's display driver.) A DMA error -- the memory not
 * the display's to read -- or a flip that does not take puts it back to decon=2.
 */
#define DPP0_DMA_PHYS           0x1c0b0000ULL
#define RDMA_IRQ                0x0004
#define RDMA_IRQ_ERRORS         ((1u << 26) | (1u << 25) | (1u << 24) | (1u << 21) | (1u << 19) | (1u << 17))
#define RDMA_BASEADDR_Y8        0x0040
#define RDMA_SHADOW             0x0400
#define FB_SECOND_PHYS          0xfbe00000ULL
#define DECON_SHD_UP_WINDOWS    0x3fu
static volatile uint32_t * gDpp0;
static uint32_t gFBPhys[2];                     /* the two buffers: the boot framebuffer, and ours */
static uint8_t * gFBVirt[2];
static unsigned gFBShown, gFBDraw;              /* the buffer let go last (it is sent, or has been); the one drawn into */
static uint32_t gFlipsChecked, gFlipsMissed;
uint32_t gStatFlips, gStatBackBusy, gStatDmaErrors, gDmaErrorBits;
static IOMemoryDescriptor * gFBMemory[2];       /* the two buffers' memory, and a mapping of each (the GPU's way in) */
static IOMemoryMap * gFBMaps[2];
static uint32_t gFBWidthPx, gFBHeightPx, gFBRowBytes, gFBBytes;
extern "C" uint64_t kvtophys(vm_offset_t va);

/* (not ml_io_map_wcomb: its submap has no room for 17 MB, and kmem_alloc panics -- gl23) */
static IOMemoryMap *
fbMap(uint32_t phys, size_t bytes, IOMemoryDescriptor ** memory)
{
	*memory = IOMemoryDescriptor::withPhysicalAddress((IOPhysicalAddress) phys, round_page(bytes), kIODirectionInOut);
	return *memory != NULL ? (*memory)->map(kIOMapWriteCombineCache) : NULL;
}

static void
deconSetup3(uint8_t * base, size_t bytes, uint32_t width, uint32_t height, uint32_t rowBytes)
{
	gDpp0 = (volatile uint32_t *) ml_io_map((vm_offset_t) DPP0_DMA_PHYS, PAGE_SIZE);
	gFBPhys[0] = gDpp0[RDMA_BASEADDR_Y8 / 4];
	gFBVirt[0] = base;
	gFBPhys[1] = (uint32_t) FB_SECOND_PHYS;
	gFBMaps[0] = gFBPhys[0] != 0 ? fbMap(gFBPhys[0], bytes, &gFBMemory[0]) : NULL;
	gFBMaps[1] = fbMap(gFBPhys[1], bytes, &gFBMemory[1]);
	gFBVirt[1] = gFBMaps[1] != NULL ? (uint8_t *) gFBMaps[1]->getVirtualAddress() : NULL;
	gFBWidthPx = width;
	gFBHeightPx = height;
	gFBRowBytes = rowBytes;
	gFBBytes = (uint32_t) round_page(bytes);
	IOLog("MiniIOMobileFramebuffer: decon=3: DPP0 base 0x%x (shadow 0x%x), irq 0x%x; second buffer 0x%x at %p, %zu bytes\n",
	    gFBPhys[0], gDpp0[(RDMA_SHADOW + RDMA_BASEADDR_Y8) / 4], gDpp0[RDMA_IRQ / 4], gFBPhys[1], gFBVirt[1], bytes);
	if (gFBVirt[1] == NULL || gFBPhys[0] == 0 || gFBPhys[0] == gFBPhys[1]) {
		IOLog("MiniIOMobileFramebuffer: decon=3: not possible, decon=2\n");
		gDecon = 2;
		return;
	}
	memcpy(gFBVirt[1], gFBVirt[0], bytes);
	gDpp0[RDMA_IRQ / 4] = RDMA_IRQ_ERRORS;          /* (write 1 to clear) */
	gFBShown = 0;
}

/* decon=3: back to one buffer, the boot framebuffer, as decon=2 has it */
static void
deconBackTo2(const char * why)
{
	IOLog("MiniIOMobileFramebuffer: decon=3: %s (base 0x%x shadow 0x%x irq 0x%x): decon=2 from now\n", why,
	    gDpp0[RDMA_BASEADDR_Y8 / 4], gDpp0[(RDMA_SHADOW + RDMA_BASEADDR_Y8) / 4], gDpp0[RDMA_IRQ / 4]);
	gDpp0[RDMA_BASEADDR_Y8 / 4] = gFBPhys[0];
	gFBShown = gFBDraw = 0;
	gDecon = 2;
}

/*
 * Frames the GPU has put into a framebuffer itself (decon=3; eagl.c's glscale): the copy is the
 * window server's last GPU work -- its picture, scaled to the panel -- and here nothing is copied.
 * eagl asks which buffer it may draw into (kFBMethodGPUFrameBegin: the one DECON0 is not sending,
 * once the GPU frame before has been shown), draws, waits for the GPU, and says which surface's
 * frame that is (kFBMethodGPUFrameDone). When that surface's swap comes -- one layer, the whole
 * display -- the buffer is let go as it is. The buffers are imported by the GPU's driver as the
 * IOSurface IDs FB_GPU_SURFACE(k) (MaliGPU.cpp, request 0x7e): no surface has them.
 */
#define FB_GPU_SURFACE(k)       (0x7fff0000u | (k))
#define FB_GPU_WAIT_MS          40
static IOLock * gGPULock;
static struct {
	bool            reserved;       /* eagl is drawing into `buffer` */
	bool            pending;        /* ... has drawn the frame of `surface` into it, not shown yet */
	bool            copying;        /* the presenter is copying a frame into the back buffer itself */
	unsigned        buffer;
	uint32_t        surface;
} gGPUFrame;
uint32_t gStatGPUShown, gStatGPUDropped, gStatGPUWaits;

/* request 0x7e's answer for FB_GPU_SURFACE(k): framebuffer k's memory (retained), as the GPU is to see it */
bool
MiniIOMobileFramebufferGPUBuffer(uint32_t id, IOMemoryDescriptor ** memory, vm_offset_t * kva, uint32_t * width,
    uint32_t * height, uint32_t * rowBytes, uint32_t * bytes)
{
	unsigned k = id & 0xffff;

	if ((id & 0xffff0000u) != FB_GPU_SURFACE(0) || k > 1 || gDecon != 3 || gFBMaps[k] == NULL) {
		return false;
	}
	*kva = (vm_offset_t) gFBMaps[k]->getVirtualAddress();
	if (kvtophys(*kva) != gFBPhys[k]) {
		IOLog("MiniIOMobileFramebuffer: GPU frames: buffer %u at 0x%x is 0x%llx to the GPU: refused\n", k, gFBPhys[k],
		    (unsigned long long) kvtophys(*kva));
		return false;
	}
	gFBMemory[k]->retain();
	*memory = gFBMemory[k];
	*width = gFBWidthPx;
	*height = gFBHeightPx;
	*rowBytes = gFBRowBytes;
	*bytes = gFBBytes;
	return true;
}

/* kFBMethodGPUFrameBegin: the buffer the GPU may draw the next frame (of `surface`) into, or -1 */
static int
gpuFrameBegin(uint32_t surface)
{
	uint64_t deadline;
	unsigned k;
	int i;

	if (gDecon != 3 || gGPULock == NULL) {
		return -1;
	}
	IOLockLock(gGPULock);
	if (gGPUFrame.pending && gGPUFrame.surface == surface && surface != 0) {
		/* more of the same frame (a second flush before its swap): drawn over, in the same buffer */
		gGPUFrame.pending = false;
		gGPUFrame.reserved = true;
		k = gGPUFrame.buffer;
		IOLockUnlock(gGPULock);
		return (int) k;
	}
	clock_interval_to_deadline(FB_GPU_WAIT_MS, kMillisecondScale, &deadline);
	/* (the GPU frame before must have been shown -- it is in the other buffer then -- or given up; nor a copy under way) */
	while (gGPUFrame.pending || gGPUFrame.reserved || gGPUFrame.copying) {
		gStatGPUWaits++;
		if (IOLockSleepDeadline(gGPULock, &gGPUFrame, deadline, THREAD_UNINT) == THREAD_TIMED_OUT) {
			gStatGPUDropped += gGPUFrame.pending;
			gGPUFrame.pending = gGPUFrame.reserved = false;
			break;
		}
	}
	k = gFBShown ^ 1;
	gGPUFrame.reserved = true;
	gGPUFrame.buffer = k;
	IOLockUnlock(gGPULock);
	/* ... and not while a frame from it is still going out */
	for (i = 0; i < DECON_IDLE_US / 10 && gDecon0 != NULL && !(gDecon0[DECON_GLOBAL_CON / 4] & DECON_IDLE_STATUS) &&
	    gDpp0[(RDMA_SHADOW + RDMA_BASEADDR_Y8) / 4] == gFBPhys[k]; i++) {
		IODelay(10);
	}
	return (int) k;
}

/* kFBMethodGPUFrameDone: buffer k holds the frame of `surface` (0: nothing came of it) */
static void
gpuFrameDone(unsigned k, uint32_t surface)
{
	if (gGPULock == NULL) {
		return;
	}
	IOLockLock(gGPULock);
	if (gGPUFrame.reserved && gGPUFrame.buffer == k) {
		gGPUFrame.reserved = false;
		gGPUFrame.pending = surface != 0;
		gGPUFrame.surface = surface;
	}
	IOLockWakeup(gGPULock, &gGPUFrame, false);
	IOLockUnlock(gGPULock);
}

/*
 * The presenter, for a swap: the buffer the GPU has drawn its frame into, if the GPU has (and it is
 * the whole of what is shown), or -1. A GPU frame that is not this swap's goes: this swap is copied
 * by the processor, into the buffer it would have been in. (`only` -- one layer, all of the display.)
 */
static int
gpuFrameTake(uint32_t surface, bool only)
{
	int k = -1;

	if (gGPULock == NULL) {
		return -1;
	}
	IOLockLock(gGPULock);
	if (gGPUFrame.reserved) {
		/* (eagl is drawing into the back buffer right now: nobody else may write it before it is done) */
		uint64_t deadline;

		clock_interval_to_deadline(FB_GPU_WAIT_MS, kMillisecondScale, &deadline);
		while (gGPUFrame.reserved && IOLockSleepDeadline(gGPULock, &gGPUFrame, deadline, THREAD_UNINT) != THREAD_TIMED_OUT) {
		}
		gGPUFrame.reserved = false;
	}
	if (gGPUFrame.pending) {
		if (only && gGPUFrame.surface == surface) {
			k = (int) gGPUFrame.buffer;
			gStatGPUShown++;
		} else {
			gStatGPUDropped++;
		}
		gGPUFrame.pending = false;
		IOLockWakeup(gGPULock, &gGPUFrame, false);
	}
	gGPUFrame.copying = k < 0;              /* (gpuCopyDone() after it) */
	IOLockUnlock(gGPULock);
	return k;
}

static void
gpuCopyDone(void)
{
	if (gGPULock == NULL) {
		return;
	}
	IOLockLock(gGPULock);
	gGPUFrame.copying = false;
	IOLockWakeup(gGPULock, &gGPUFrame, false);
	IOLockUnlock(gGPULock);
}

static void
deconMaskWhenStarted(thread_call_param_t, thread_call_param_t)
{
	uint64_t deadline;

	if ((gDecon0[DECON_SHD_REG_UP_REQ / 4] & DECON_SHD_UP_GLOBAL) && ++gDeconTries < DECON_START_MS) {
		clock_interval_to_deadline(1, kMillisecondScale, &deadline);
		thread_call_enter_delayed(gDeconMasker, deadline);
		return;
	}
	gDeconStartedAt = gDeconTries < DECON_START_MS ? mach_absolute_time() : 0;
	gDecon0[DECON_TRIG_CON / 4] |= DECON_HW_TRIG_MASK;
	__asm__ volatile ("dsb sy" ::: "memory");
	if (gDeconTries >= DECON_START_MS && gDeconLate++ < 4) {
		IOLog("MiniIOMobileFramebuffer: decon=1: no frame started %u ms after the trigger was unmasked\n", DECON_START_MS);
	}
	gDeconSending = false;
}

/*
 * One frame watched (decon_trace): per register, what it was, how often it changed, when first and
 * last (us from the unmasking) and to what last; for OF_LEVEL, when it was not 0 and its most.
 */
static void
deconTrace(void)
{
	static const uint16_t regs[] = { 0x004, 0x020, 0x030, 0x038, 0x050, 0x070, 0x074, 0x2b0 };
	enum { N = sizeof(regs) / sizeof(regs[0]), OF = N - 1 };
	uint32_t was[N], last[N], changes[N] = {}, first[N] = {}, firstTo[N] = {}, latest[N] = {}, us, ofFrom = 0, ofTo = 0, ofMost = 0;
	uint64_t began = mach_absolute_time(), nanos;
	char line[640];
	int len;

	for (unsigned r = 0; r < N; r++) {
		was[r] = last[r] = gDecon0[regs[r] / 4];
	}
	do {
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &nanos);
		us = (uint32_t) (nanos / NSEC_PER_USEC);
		for (unsigned r = 0; r < N; r++) {
			uint32_t v = gDecon0[regs[r] / 4];

			if (v != last[r]) {
				firstTo[r] = changes[r] ? firstTo[r] : v;
				first[r] = changes[r]++ ? first[r] : us;
				latest[r] = us;
				last[r] = v;
			}
		}
		if ((last[OF] & 0xffff) != 0) {
			ofFrom = ofFrom ? ofFrom : us;
			ofTo = us;
			ofMost = (last[OF] & 0xffff) > ofMost ? (last[OF] & 0xffff) : ofMost;
		}
	} while (us < DECON_TRACE_MS * 1000);
	len = snprintf(line, sizeof(line), "MiniIOMobileFramebuffer: decon_trace %u:", gDeconReleases);
	for (unsigned r = 0; r < N && len < (int) sizeof(line); r++) {
		len += snprintf(line + len, sizeof(line) - len, " %x=%x", regs[r], was[r]);
		if (changes[r] && len < (int) sizeof(line)) {
			len += snprintf(line + len, sizeof(line) - len, " (x%u %u-%u ->%x..%x)", changes[r], first[r], latest[r],
			    firstTo[r], last[r]);
		}
	}
	IOLog("%s; OF_LEVEL not 0 %u-%u us, most %u\n", line, ofFrom, ofTo, ofMost);
}

static void
deconHold(void)
{
	int i, j;

	if (!gDecon) {
		return;
	}
	if (gDecon0 == NULL) {
		gDeconMasker = thread_call_allocate(deconMaskWhenStarted, NULL);
		gDecon0 = (volatile uint32_t *) ml_io_map((vm_offset_t) DECON0_PHYS, PAGE_SIZE);
		IOLog("MiniIOMobileFramebuffer: decon=1: DECON0 GLOBAL_CON 0x%x TRIG_CON 0x%x INT_EN 0x%x INT_PEND 0x%x\n",
		    gDecon0[DECON_GLOBAL_CON / 4], gDecon0[DECON_TRIG_CON / 4], gDecon0[DECON_INT_EN / 4],
		    gDecon0[DECON_INT_PEND / 4]);
	}
	if (gDecon == 3) {
		uint32_t irq = gDpp0[RDMA_IRQ / 4], shadow = gDpp0[(RDMA_SHADOW + RDMA_BASEADDR_Y8) / 4];

		if (irq & RDMA_IRQ_ERRORS) {
			gStatDmaErrors++;
			gDmaErrorBits |= irq & RDMA_IRQ_ERRORS;
			gDpp0[RDMA_IRQ / 4] = irq & RDMA_IRQ_ERRORS;
			if (gStatDmaErrors >= 3) {
				deconBackTo2("DMA errors");
			}
		}
		/* did the flips take? Once a frame has started since the last, its buffer is what the DMA has */
		if (gDecon == 3 && !gDeconSending && gFlipsChecked < 16) {
			gFlipsChecked++;
			if (shadow != gFBPhys[gFBShown]) {
				gFlipsMissed++;
			}
			if (gFlipsChecked <= 6) {
				IOLog("MiniIOMobileFramebuffer: decon=3: flip %u: base 0x%x shadow 0x%x (buffer %u is 0x%x) irq 0x%x\n",
				    gFlipsChecked, gDpp0[RDMA_BASEADDR_Y8 / 4], shadow, gFBShown, gFBPhys[gFBShown], irq);
			}
			if (gFlipsMissed >= 3) {
				deconBackTo2("flips not taken");
			}
		}
	}
	if (gDecon == 3) {
		/* the other buffer, once no frame from it is going out */
		gFBDraw = gFBShown ^ 1;
		for (i = 0; i < DECON_IDLE_US / 10 && !(gDecon0[DECON_GLOBAL_CON / 4] & DECON_IDLE_STATUS) &&
		    gDpp0[(RDMA_SHADOW + RDMA_BASEADDR_Y8) / 4] == gFBPhys[gFBDraw]; i++) {
			IODelay(10);
		}
		gStatBackBusy += i > 0;
		gDeconCountAtCopy = gDecon0[DECON_FRAME_COUNT / 4];
		return;
	}
	/* (the frame before must have been sent: its transfer started, the trigger masked again) */
	for (i = 0; gDeconSending && i < DECON_START_MS; i++) {
		IOSleep(1);
	}
	gDecon0[DECON_TRIG_CON / 4] |= DECON_HW_TRIG_MASK;
	__asm__ volatile ("dsb sy" ::: "memory");
	if (gDecon == 2) {
		/* (not waiting for the end of it: drawRows() keeps behind DECON0) */
		uint64_t now = mach_absolute_time(), span, margin;

		nanoseconds_to_absolutetime(DECON_SEND_US * NSEC_PER_USEC, &span);
		nanoseconds_to_absolutetime(DECON_CHASE_MARGIN_US * NSEC_PER_USEC, &margin);
		gChaseSpan = 0;
		if (gDeconStartedAt != 0 && now < gDeconStartedAt + margin + span) {
			gChaseFrom = gDeconStartedAt + margin;
			gChaseSpan = span;
			gStatChased++;
		}
		gDeconStartedAt = 0;
		gDeconCountAtCopy = gDecon0[DECON_FRAME_COUNT / 4];
		return;
	}
	for (i = 0; i < DECON_IDLE_US / 10 && !(gDecon0[DECON_GLOBAL_CON / 4] & DECON_IDLE_STATUS); i++) {
		IODelay(10);
	}
	if (i == DECON_IDLE_US / 10 && gDeconLate++ < 4) {
		IOLog("MiniIOMobileFramebuffer: decon=1: DECON0 not idle after %u us (GLOBAL_CON 0x%x)\n",
		    DECON_IDLE_US, gDecon0[DECON_GLOBAL_CON / 4]);
	}
	/* idle, and still sending? Then until the output FIFO has been empty for 100 us */
	if (gDecon0[DECON_OF_LEVEL / 4] & 0xffff) {
		gStatIdleButSending++;
		for (i = 0, j = 0; i < DECON_IDLE_US / 10 && j < 10; i++) {
			j = gDecon0[DECON_OF_LEVEL / 4] & 0xffff ? 0 : j + 1;
			IODelay(10);
		}
	}
	gDeconCountAtCopy = gDecon0[DECON_FRAME_COUNT / 4];
}

/*
 * The frame drawn: sent once. Unmasked, DECON0 would send the same frame again on every pulse and
 * be busy most of the time, so that the next frame waited for it (gl10: 11 ms a frame instead of 3).
 * As Linux's decon_reg_wait_update_done_and_mask(): a shadow update asked for, the trigger
 * unmasked, and masked again when the update is done -- the frame has started.
 */
static void
deconRelease(void)
{
	uint64_t deadline;

	if (gDecon0 == NULL) {
		return;
	}
	__asm__ volatile ("dsb sy" ::: "memory");       /* the frame in memory before DECON0 may read it */
	gStatCopies++;
	gStatStartedInCopy += gDecon0[DECON_FRAME_COUNT / 4] != gDeconCountAtCopy;
	gChaseSpan = 0;
	if (gDecon == 3) {
		gDpp0[RDMA_BASEADDR_Y8 / 4] = gFBPhys[gFBDraw];
		__asm__ volatile ("dsb sy" ::: "memory");
		gFBShown = gFBDraw;
		gStatFlips++;
	}
	gDeconSending = true;
	gDeconTries = 0;
	/* (the windows' too once DPP0's address has been touched: also for the way back to one buffer) */
	gDecon0[DECON_SHD_REG_UP_REQ / 4] |= DECON_SHD_UP_GLOBAL | (gDpp0 != NULL ? DECON_SHD_UP_WINDOWS : 0);
	gDecon0[DECON_TRIG_CON / 4] &= ~DECON_HW_TRIG_MASK;
	clock_interval_to_deadline(1, kMillisecondScale, &deadline);
	thread_call_enter_delayed(gDeconMasker, deadline);
	if (++gDeconReleases >= DECON_TRACE_FROM && gDeconReleases < DECON_TRACE_FROM + gDeconTrace) {
		deconTrace();
	}
}
/*
 * Boot-arg fb_filter=0: the picture is scaled to the panel nearest neighbour, as it once was. The
 * iPhone's 1125 x 2436 on this panel's 1080 x 2400 is 45 columns and 36 rows fewer: nearest
 * neighbour leaves out every 25th column and every 67th row, and where one of them goes through a
 * letter or a slanting line, that is a step in it or a stroke a pixel thinner -- text and lines a
 * little crooked, here and there and never in the same places twice. By default every pixel of the
 * panel is worked out from the four of the picture around where it falls (bilinear).
 */
static TUNABLE(uint32_t, gFilter, "fb_filter", 1);
static TUNABLE(uint32_t, gRefreshHz, "fb_hz", kFBRefreshHz);
static TUNABLE(uint32_t, gOnTheBeat, "fb_beat", 1);
/*
 * Boot-arg fb_threads=N (1 to 8): so many processors scale a frame onto the panel together (see
 * drawRows()). Three by default: a frame is in the panel's memory 3 ms after its turn instead of 9
 * to 10 (the phone, 15 October; fb_threads=1 is how it was).
 */
static TUNABLE(uint32_t, gThreads, "fb_threads", 3);
/* boot-arg fb_inuse=0: the surfaces of a swap are not marked in use while it waits and is copied (MiniIOSurfaceDisplayUse) */
static TUNABLE(uint32_t, gInUse, "fb_inuse", 1);
/* redwood_mali_uapi.c's: the window server's GPU work waiting for a surface the display has (surfaces_shown_wait) */
extern "C" uint32_t mali_shown_waits, mali_shown_timeouts;
extern "C" uint64_t mali_shown_wait_abs;
/* MiniIOSurface.cpp's: write locks that waited for a surface the display has (eagl.c's glhold) */
extern uint32_t gSurfaceDisplayWaits, gSurfaceDisplayTimeouts;
extern uint64_t gSurfaceDisplayWaitAbs;
#define SWAP_QUEUE              2                       /* frames that may wait for the panel at most (fb_queue) */
static TUNABLE(uint32_t, gQueue, "fb_queue", 2);
static inline unsigned
swapQueue(void)
{
	return gQueue >= 1 && gQueue <= SWAP_QUEUE ? gQueue : 1;
}
/*
 * Boot-arg touch_lat=1: a line for every swap handed over ("tl: s ID at US") and for every frame
 * put on the panel ("tl: v ID ready US shown US done US": the GPU done with it, its beat come, the
 * pixels in the panel's memory), in microseconds of mach_absolute_time, for measuring how long a
 * touch takes to show (tools/latency.py; the touch's own lines: bsd/dev/redwood_input.c).
 */
static TUNABLE(uint32_t, gTouchLat, "touch_lat", 0);

static inline uint64_t
micros(uint64_t absolute)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(absolute, &ns);
	return ns / 1000;
}

static inline uint32_t
refreshHz(void)
{
	return gRefreshHz >= 1 && gRefreshHz <= 240 ? gRefreshHz : kFBRefreshHz;
}
static inline uint32_t
recStep(void)
{
	return gRecStep >= 2 && gRecStep <= 8 ? gRecStep : 3;
}
#define REC_STEP        recStep()

struct rec_header {
	char     magic[8];              /* "RWREC003" */
	uint32_t width, height;
	uint32_t frames;
	uint32_t oldest;                /* the place of the oldest frame (rec_ring; 0 until the memory has once been full) */
	uint64_t began;                 /* when the first frame was recorded, in nanoseconds of the kernel's time */
};
struct rec_frame {
	uint64_t nanoseconds;           /* since the first frame of the recording */
};

extern "C" uint64_t loader_recording[3];        /* { physical address, bytes, kernel address }: osfmk/arm64/loader_return.c */

static uint32_t
swapWord(const uint8_t * swap, unsigned offset)
{
	uint32_t value;

	memcpy(&value, swap + offset, sizeof(value));
	return value;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct RowJob;

class IOMobileFramebuffer : public IOService
{
	OSDeclareDefaultStructors(IOMobileFramebuffer);

public:
	IOLock *        fLock;
	uint8_t *       fBase;                  /* the boot framebuffer: the panel */
	uint32_t        fPanelWidth, fPanelHeight, fRowBytes;
	uint32_t        fWidth, fHeight;        /* the display as user space sees it */
	MiniIOSurface * fDefaultSurface;
	uint32_t        fSwapID;
	uint32_t        fSwapsShown;
	uint32_t        fLayersShown;           /* which layers the last swap had a surface in */
	bool            fGraphics;              /* the text console has been told to stop */
	/*
	 * A frame goes to the panel when the GPU is done with it: a thread of ours waits for that
	 * and copies it there, and whoever made the frame is back at the next one meanwhile. One
	 * frame waits at a time: the swap after it is held, in SwapEnd, until this one is being
	 * copied to the panel. Whoever draws is so at most two frames ahead of the panel, and
	 * needs a third surface to be that: CoreAnimation makes itself a new one for every frame.
	 * By default two frames may wait (boot-arg fb_queue=1: one, as before): CoreAnimation draws on a
	 * clock of its own, and a SwapEnd held past its next tick makes that tick late, the clock going
	 * on from there (rc.jobs.fps: 28% of the frames over 16.7 ms with two, 33% with one).
	 */
	IOLock *        fSwapLock;              /* around the next five */
	uint8_t         fSwap[SWAP_QUEUE][SWAP_SIZE];   /* the swaps on their way to the panel (fb_queue), from fSwapFirst on */
	MiniIOSurface * fSwapSurfaces[SWAP_QUEUE][SWAP_LAYERS]; /* ... their surfaces, held from SwapEnd on: their maker may let go of them */
	uint32_t        fSwapSeeds[SWAP_QUEUE][SWAP_LAYERS];    /* ... and their seeds then: was one drawn again before we were done? */
	uint32_t        fShowSeeds[SWAP_LAYERS];                /* (those of the swap being shown) */
	uint32_t        fStatRewrittenBefore, fStatRewrittenDuring;     /* frames whose surface was, before the copy and by its end */

	/* 1 if a surface of the swap being shown has been written since its SwapEnd */
	uint32_t
	surfaceRewritten(MiniIOSurface * const * surfaces)
	{
		for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
			if (surfaces[layer] != NULL && MiniIOSurfaceSeed(surfaces[layer]) != fShowSeeds[layer]) {
				return 1;
			}
		}
		return 0;
	}
	unsigned        fSwapFirst, fSwapsWaiting;
	/*
	 * ... and not whenever the GPU happens to be done, but on a beat of the refresh rate
	 * (boot-arg fb_beat=0: at once): frames then follow each other at an even pace whatever
	 * each took to draw, as long as there is one for every beat. The beat is this driver's own.
	 * (CoreAnimation does not go by the refresh notifications of this driver -- it never asks
	 * for them -- but by a clock of its own at the rate it reads here; and where the panel's
	 * scan is nobody can read: the display block's register clock is off.) It starts with a
	 * frame that comes later than a beat and a half after the one before -- the first after a
	 * pause, or every one of a renderer that does not keep up, the software one: such a frame
	 * is shown at once, a beat would only make it later still -- and gives way by an eighth of
	 * what a frame is early or late, so that frames are ready some six milliseconds before
	 * their beat: neither a slow one nor a quick one then lands on the wrong side of it.
	 */
	uint64_t        fBeat;                  /* the beat the last frame was shown on */
	uint64_t        fReady;                 /* when the last frame was ready to be shown */
	uint32_t        fStatOnBeat;            /* frames that waited for a refresh, and for how long together */
	uint64_t        fStatBeatWait;
	uint32_t        fStatSurfaces[8];       /* the first different surfaces the bottom layer was given */
	uint32_t        fStatHeld, fStatLost;   /* swaps that waited for the one before; that gave up waiting */
	uint64_t        fSwapTime;              /* when the last swap was handed over */
	uint32_t        fStatSwapsLate;         /* swaps that came more than a refresh and a quarter after the one before (and no pause) */
	uint64_t        fStatWaiting;           /* how long frames waited for the GPU here */
	/* how the display is doing: frames and time spent drawing them here since fStatSince */
	uint32_t        fStatFrames;
	uint64_t        fStatSince, fStatDrawing;
	/* ... and how evenly: the time from each frame to the next, counted by the millisecond (the last count: longer) */
	uint64_t        fLastFrame;
	uint32_t        fFrameTimes[FRAME_TIMES + 1], fFramePauses, fFrameLongest;
	/* boot-arg fb_diff=1: which pixels of the panel each frame changed (it costs: the panel's memory is slow to read) */
	bool            fDiff;
	uint64_t        fDiffPixels;
	uint32_t        fDiffLeft, fDiffTop, fDiffRight, fDiffBottom;
	/* the screen recording, if one was asked for */
	uint32_t *      fColumn;                /* drawLayer(): source column for every panel column */
	uint16_t *      fColumnWeight;          /* ... with fb_filter: how much of the column after it, of 256 */
	uint32_t *      fBlendRow;              /* ... and a row of the picture blended from two (fBlendRowSize pixels) */
	uint32_t        fBlendRowSize;
	uint8_t *       fRecBase;
	uint64_t        fRecSize, fRecUsed;
	uint64_t        fFirstFrame, fRecFirst; /* when the first frame was shown, and the first recorded */
	bool            fRecDone;
	bool            fRecWrapped;            /* (rec_ring) the memory has been full once: frames go over the oldest */

	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	void publishModes(void);
	MiniIOSurface * defaultSurface(void);
	void present(const uint8_t * swap);
	void show(const uint8_t * swap, MiniIOSurface ** surfaces);
	void presenter(void);
	static void presenterMain(void * self, wait_result_t result);
	void reportFrameTimes(void);
	void drawLayer(MiniIOSurface * s, const int32_t * src, const int32_t * dst);
	void drawRows(const struct RowJob * job, int32_t y0, int32_t y1, uint32_t * blendRow);
	static void helperMain(void * self, wait_result_t result);
	/* the scaling shared out (fb_threads): fThreads drawers, the presenter and fThreads - 1 helpers */
	unsigned        fThreads;
	IOLock *        fHelperLock;
	const struct RowJob * fHelperJob;
	uint64_t        fHelperGeneration;
	unsigned        fHelpersBusy, fHelpersStarted;
	uint32_t *      fHelperBlend[7];
	void record(MiniIOSurface * s);
};

OSDefineMetaClassAndStructors(IOMobileFramebuffer, IOService);

static void
setNumber(OSDictionary * dict, const char * key, uint64_t value)
{
	OSNumber * n = OSNumber::withNumber(value, 32);

	if (n != NULL) {
		dict->setObject(key, n);
		n->release();
	}
}

/*
 * The modes of the display, the way CoreAnimation reads them (the properties TimingElements
 * and ColorElements; without a mode it draws nothing). One timing, the panel's size at
 * 60 Hz (boot-arg fb_hz=N: at N), with one colour mode, 8-bit RGB. The identifiers are what "the current mode"
 * (block 0x40) answers with.
 */
#define kFBTimingID     1
#define kFBColorID      1

void
IOMobileFramebuffer::publishModes(void)
{
	OSDictionary * timing = OSDictionary::withCapacity(8);
	OSDictionary * horizontal = OSDictionary::withCapacity(4);
	OSDictionary * vertical = OSDictionary::withCapacity(4);
	OSDictionary * color = OSDictionary::withCapacity(8);
	OSArray * timings = OSArray::withCapacity(1);
	OSArray * colors = OSArray::withCapacity(1);
	const uint32_t rate = refreshHz() << 16;                /* 16.16 fixed point */

	if (timing == NULL || horizontal == NULL || vertical == NULL || color == NULL || timings == NULL || colors == NULL) {
		return;
	}
	setNumber(color, "ID", kFBColorID);
	setNumber(color, "PixelEncoding", 0);                   /* RGB 4:4:4 */
	setNumber(color, "Depth", 8);
	setNumber(color, "Colorimetry", 1);
	setNumber(color, "EOTF", 1);
	setNumber(color, "DynamicRange", 0);
	setNumber(color, "Score", 1);
	color->setObject("IsVirtual", kOSBooleanFalse);
	colors->setObject(color);

	setNumber(horizontal, "Active", fWidth);
	setNumber(horizontal, "Total", fWidth);
	setNumber(horizontal, "SyncRate", rate);
	setNumber(horizontal, "PreciseSyncRate", rate);
	setNumber(vertical, "Active", fHeight);
	setNumber(vertical, "Total", fHeight);
	setNumber(vertical, "SyncRate", rate);
	setNumber(vertical, "PreciseSyncRate", rate);
	setNumber(timing, "ID", kFBTimingID);
	setNumber(timing, "Score", 1);
	timing->setObject("IsVirtual", kOSBooleanFalse);
	timing->setObject("IsOverscanned", kOSBooleanFalse);
	timing->setObject("HorizontalAttributes", horizontal);
	timing->setObject("VerticalAttributes", vertical);
	timing->setObject("ColorModes", colors);
	timings->setObject(timing);

	setProperty("TimingElements", timings);
	setProperty("ColorElements", colors);
	OSSafeReleaseNULL(timings);
	OSSafeReleaseNULL(colors);
	OSSafeReleaseNULL(timing);
	OSSafeReleaseNULL(horizontal);
	OSSafeReleaseNULL(vertical);
	OSSafeReleaseNULL(color);
}

bool
IOMobileFramebuffer::start(IOService * provider)
{
	thread_t thread;

	if (!IOService::start(provider)) {
		return false;
	}
	if (vinfo.v_baseaddr == 0 || vinfo.v_width == 0 || vinfo.v_height == 0 || vinfo.v_depth != 32) {
		IOLog("MiniIOMobileFramebuffer: no usable boot framebuffer (%ux%u, depth %u)\n", vinfo.v_width, vinfo.v_height,
		    vinfo.v_depth);
		return false;
	}
	fLock = IOLockAlloc();
	fSwapLock = IOLockAlloc();
	if (kernel_thread_start(&IOMobileFramebuffer::presenterMain, this, &thread) != KERN_SUCCESS) {
		IOLog("MiniIOMobileFramebuffer: no thread to put frames on the panel with\n");
		return false;
	}
	thread_set_thread_name(thread, "frames to the panel");
	thread_deallocate(thread);
	fThreads = gThreads >= 1 && gThreads <= 8 ? gThreads : 1;
	fHelperLock = IOLockAlloc();
	for (unsigned i = 0; i + 1 < fThreads; i++) {
		if (kernel_thread_start(&IOMobileFramebuffer::helperMain, this, &thread) != KERN_SUCCESS) {
			fThreads = i + 1;
			break;
		}
		thread_set_thread_name(thread, "frames to the panel: a helper");
		thread_deallocate(thread);
	}
	fBase = (uint8_t *) vinfo.v_baseaddr;
	fPanelWidth = vinfo.v_width;
	fPanelHeight = vinfo.v_height;
	fRowBytes = vinfo.v_rowbytes;
	if (gDecon == 3) {
		gGPULock = IOLockAlloc();
		deconSetup3(fBase, (size_t) fRowBytes * fPanelHeight, fPanelWidth, fPanelHeight, fRowBytes);
	}
	fWidth = gFBWidth ? gFBWidth : fPanelWidth;
	fHeight = gFBHeight ? gFBHeight : fPanelHeight;
	if (gFBScale != 0 && gFBScale != kFBDefaultScale) {
		static const char * const places[][2] = { { "/chosen", "display-scale" }, { "/product", "artwork-scale-factor" } };
		uint32_t scale = gFBScale;

		fWidth = kFBPointsWide * scale;
		fHeight = kFBPointsHigh * scale;
		/* what the loader's device tree says about the scale is where MobileGestalt has it from */
		for (unsigned i = 0; i < sizeof(places) / sizeof(places[0]); i++) {
			IORegistryEntry * node = IORegistryEntry::fromPath(places[i][0], gIODTPlane);

			if (node != NULL) {
				node->setProperty(places[i][1], &scale, sizeof(scale));
				node->release();
			}
		}
	}
	fColumn = IONewZero(uint32_t, fPanelWidth);
	fColumnWeight = IONewZero(uint16_t, fPanelWidth);
	fDiff = gDiff != 0;
	fDiffLeft = fDiffTop = ~0u;
	IOLog("MiniIOMobileFramebuffer: display %ux%u on a panel of %ux%u (%u bytes per row)\n", fWidth, fHeight,
	    fPanelWidth, fPanelHeight, fRowBytes);

	if (gRecSeconds != 0) {
		/* one piece of physical memory: the loader will send it over USB as it lies */
		IOBufferMemoryDescriptor * memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
		    kIODirectionInOut | kIOMemoryPhysicallyContiguous, (mach_vm_size_t) gRecMegabytes << 20, ~0ULL);

		if (memory != NULL) {
			struct rec_header * header = (struct rec_header *) memory->getBytesNoCopy();

			fRecBase = (uint8_t *) header;
			fRecSize = (uint64_t) gRecMegabytes << 20;
			fRecUsed = sizeof(*header);
			bzero(header, sizeof(*header));
			memcpy(header->magic, "RWREC003", 8);
			header->width = fWidth / REC_STEP;
			header->height = fHeight / REC_STEP;
			loader_recording[0] = memory->getPhysicalSegment(0, NULL);
			loader_recording[1] = fRecUsed;
			loader_recording[2] = (uint64_t) (uintptr_t) fRecBase;
			IOLog("MiniIOMobileFramebuffer: recording %u s of the screen at %ux%u from second %u, into %u MB at %#llx%s\n",
			    gRecSeconds, header->width, header->height, gRecStart, gRecMegabytes, loader_recording[0],
			    gRecRing ? ", the last of it kept (rec_ring)" : "");
		} else {
			IOLog("MiniIOMobileFramebuffer: no memory for a recording of %u MB\n", gRecMegabytes);
		}
	}

	setProperty(kIOUserClientClassKey, "IOMobileFramebufferUserClient");
	/* for who draws with the GPU (userland/src/eagl.c): a frame need not be waited for before it is handed over */
	setProperty("RedwoodWaitsForGPU", kOSBooleanTrue);
	publishModes();
	setName("disp0");
	registerService();
	return true;
}

/* Keep a small copy of the frame that is about to be shown (the surface is the whole screen). */
void
IOMobileFramebuffer::record(MiniIOSurface * s)
{
	struct rec_header * header = (struct rec_header *) fRecBase;
	uint64_t now = mach_absolute_time(), since_first, since_start = 0, frame_size;
	struct rec_frame * frame;
	uint8_t * to;

	if (fFirstFrame == 0) {
		fFirstFrame = now;
	}
	if (fRecBase == NULL || fRecDone || s->info.width < fWidth || s->info.height < fHeight) {
		return;
	}
	absolutetime_to_nanoseconds(now - fFirstFrame, &since_first);
	if (since_first < (uint64_t) gRecStart * NSEC_PER_SEC) {
		return;
	}
	if (fRecFirst == 0) {
		uint64_t began;

		fRecFirst = now;
		absolutetime_to_nanoseconds(now, &began);
		header->began = began;
		IOLog("MiniIOMobileFramebuffer: the recording begins at %llu.%03llu s of the kernel's time (mach_absolute_time)\n",
		    began / NSEC_PER_SEC, began / NSEC_PER_MSEC % 1000);
	}
	absolutetime_to_nanoseconds(now - fRecFirst, &since_start);
	frame_size = sizeof(*frame) + (uint64_t) header->width * header->height * 3;
	if (since_start > (uint64_t) gRecSeconds * NSEC_PER_SEC || (fRecUsed + frame_size > fRecSize && !gRecRing)) {
		fRecDone = true;
		IOLog("MiniIOMobileFramebuffer: recording finished: %u frames, %llu MB\n", header->frames, fRecUsed >> 20);
		return;
	}
	if (fRecUsed + frame_size > fRecSize) {
		/* (rec_ring) round again: over the oldest, which is the next one */
		fRecUsed = sizeof(*header);
		fRecWrapped = true;
	}
	frame = (struct rec_frame *) (fRecBase + fRecUsed);
	frame->nanoseconds = since_start;
	to = (uint8_t *) (frame + 1);
	for (uint32_t y = 0; y < header->height; y++) {
		const uint32_t * from = (const uint32_t *) (s->pixels() + (size_t) (y * REC_STEP + 1) * s->info.bytesPerRow);

		for (uint32_t x = 0; x < header->width; x++) {
			uint32_t pixel = from[x * REC_STEP + 1];        /* B G R A in memory */

			*to++ = (uint8_t) (pixel >> 16);
			*to++ = (uint8_t) (pixel >> 8);
			*to++ = (uint8_t) pixel;
		}
	}
	fRecUsed += frame_size;
	if (!fRecWrapped) {
		header->frames++;
		loader_recording[1] = fRecUsed;
	} else {
		/* the memory full of frames: all of them are handed over, from the one after this */
		header->oldest = (uint32_t) ((fRecUsed - sizeof(*header)) / frame_size);
		if (header->oldest >= header->frames) {
			header->oldest = 0;
		}
		loader_recording[1] = sizeof(*header) + (uint64_t) header->frames * frame_size;
	}
}

MiniIOSurface *
IOMobileFramebuffer::defaultSurface(void)
{
	IOLockLock(fLock);
	if (fDefaultSurface == NULL) {
		fDefaultSurface = MiniIOSurfaceCreateSimple(fWidth, fHeight, FOURCC('B', 'G', 'R', 'A'));
	}
	IOLockUnlock(fLock);
	return fDefaultSurface;
}

/*
 * Two pixels mixed: `w` of 256 of the second. Two of the four channels at a time, in one word.
 */
static inline uint32_t
blend(uint32_t a, uint32_t b, uint32_t w)
{
	uint32_t rb = ((a & 0xff00ff) * (256 - w) + (b & 0xff00ff) * w) >> 8 & 0xff00ff;
	uint32_t ga = ((a >> 8 & 0xff00ff) * (256 - w) + (b >> 8 & 0xff00ff) * w) & 0xff00ff00;

	return rb | ga;
}

/*
 * Where the middle of the `i`th of `out` pixels falls among `in` of them, in 256ths of a pixel,
 * kept within the first and the last.
 */
static inline int64_t
sample_at(int64_t i, int64_t in, int64_t out)
{
	int64_t at = ((2 * i + 1) * in - out) * 128 / out;

	return at < 0 ? 0 : at > (in - 1) * 256 ? (in - 1) * 256 : at;
}

/*
 * The rows of one layer, from `y0` to `y1` (panel rows), as drawLayer() has worked them out:
 * scaling a whole frame takes one processor some 9 ms, so with boot-arg fb_threads=N it is shared
 * out between N of them, each with rows of its own and a row of blending of its own.
 */
struct RowJob {
	const uint8_t * pixels;
	uint32_t        stride;
	int32_t         sx, sy, sw, sh, px0, py0, px1, py1, cx0, cx1;
	bool            filter, unscaled;
};

void
IOMobileFramebuffer::drawRows(const RowJob * job, int32_t y0, int32_t y1, uint32_t * blendRow)
{
	const uint8_t * pixels = job->pixels;
	uint32_t stride = job->stride;
	int32_t sx = job->sx, sy = job->sy, sw = job->sw, sh = job->sh;
	int32_t px0 = job->px0, py0 = job->py0, px1 = job->px1, py1 = job->py1, cx0 = job->cx0, cx1 = job->cx1;
	bool filter = job->filter;
	uint8_t * base = gDecon == 3 ? gFBVirt[gFBDraw] : fBase;

	for (int32_t y = y0; y < y1; y++) {
		const uint32_t * from;
		uint32_t * to;

		if (y < 0 || (uint32_t) y >= fPanelHeight) {
			continue;
		}
		if (gChaseSpan != 0) {
			/* decon=2: not before DECON0 has read this row of the frame before */
			uint64_t at = gChaseFrom + gChaseSpan * (uint64_t) (y + 1) / fPanelHeight, began = mach_absolute_time();

			if (began < at) {
				while (mach_absolute_time() < at) {
				}
				__atomic_fetch_add(&gStatChaseWait, mach_absolute_time() - began, __ATOMIC_RELAXED);
			}
		}
		to = (uint32_t *) (base + (size_t) y * fRowBytes);
		if (job->unscaled) {
			/* not scaled at all */
			memcpy(to + px0, pixels + (size_t) (sy + y - py0) * stride + (size_t) sx * 4, (size_t) sw * 4);
			continue;
		}
		if (filter) {
			int64_t at = sample_at(y - py0, sh, py1 - py0);
			uint32_t row = (uint32_t) (at >> 8), weight = (uint32_t) (at & 255);
			const uint32_t * upper = (const uint32_t *) (pixels + (size_t) (sy + row) * stride) + sx;

			if (weight == 0 || row + 1 >= (uint32_t) sh) {
				from = upper;
			} else {
				const uint32_t * lower = (const uint32_t *) (pixels + (size_t) (sy + row + 1) * stride) + sx;
				uint32_t first = fColumn[cx0], last = fColumn[cx1 - 1] + 1 < (uint32_t) sw ? fColumn[cx1 - 1] + 1 : fColumn[cx1 - 1];

				uint32_t * __restrict mixed = blendRow;

				for (uint32_t c = first; c <= last; c++) {
					uint32_t a = upper[c], b = lower[c];

					mixed[c] = a == b ? a : blend(a, b, weight);           /* (most of a screen is the same as next to it) */
				}
				from = blendRow;
			}
		} else {
			from = (const uint32_t *) (pixels + (size_t) (sy + (int64_t) (y - py0) * sh / (py1 - py0)) * stride);
		}
		if (fDiff) {
			for (int32_t x = cx0; x < cx1; x++) {
				uint32_t value = filter ? blend(from[fColumn[x]], from[fColumn[x] + (fColumnWeight[x] != 0)], fColumnWeight[x]) :
				    from[fColumn[x]];

				if (to[x] != value) {
					fDiffPixels++;
					fDiffLeft = (uint32_t) x < fDiffLeft ? (uint32_t) x : fDiffLeft;
					fDiffRight = (uint32_t) x > fDiffRight ? (uint32_t) x : fDiffRight;
					fDiffTop = (uint32_t) y < fDiffTop ? (uint32_t) y : fDiffTop;
					fDiffBottom = (uint32_t) y > fDiffBottom ? (uint32_t) y : fDiffBottom;
				}
			}
		}
		if (filter) {
			const uint32_t * __restrict column = fColumn + cx0;
			const uint16_t * __restrict weights = fColumnWeight + cx0;
			uint32_t * __restrict out = to + cx0;

			for (uint32_t i = 0, n = (uint32_t) (cx1 - cx0); i < n; i++) {
				uint32_t c = column[i], w = weights[i], a = from[c];

				out[i] = w == 0 || a == from[c + 1] ? a : blend(a, from[c + 1], w);
			}
		} else {
			for (int32_t x = cx0; x < cx1; x++) {
				to[x] = from[fColumn[x]];
			}
		}
	}
}

/* A helper of the scaling (fb_threads): waits for rows to do, does them, says so. */
void
IOMobileFramebuffer::helperMain(void * self, wait_result_t result __unused)
{
	IOMobileFramebuffer * fb = (IOMobileFramebuffer *) self;
	unsigned index;
	uint64_t done = 0;

	IOLockLock(fb->fHelperLock);
	index = fb->fHelpersStarted++;
	IOLockUnlock(fb->fHelperLock);
	for (;;) {
		const RowJob * job;
		int32_t rows, y0, y1;
		uint32_t * blend;

		IOLockLock(fb->fHelperLock);
		while (fb->fHelperGeneration == done) {
			IOLockSleep(fb->fHelperLock, &fb->fHelperGeneration, THREAD_UNINT);
		}
		done = fb->fHelperGeneration;
		job = fb->fHelperJob;
		IOLockUnlock(fb->fHelperLock);
		/* share index + 1 of fThreads (the presenter takes share 0) */
		rows = job->py1 - job->py0;
		y0 = job->py0 + (int32_t) ((int64_t) rows * (index + 1) / fb->fThreads);
		y1 = job->py0 + (int32_t) ((int64_t) rows * (index + 2) / fb->fThreads);
		blend = fb->fHelperBlend[index];
		fb->drawRows(job, y0, y1, blend);
		IOLockLock(fb->fHelperLock);
		fb->fHelpersBusy--;
		if (fb->fHelpersBusy == 0) {
			IOLockWakeup(fb->fHelperLock, &fb->fHelpersBusy, true);
		}
		IOLockUnlock(fb->fHelperLock);
	}
}

/*
 * Put a rectangle of a 32-bit surface on the screen. `dst` is in display coordinates; the
 * panel may be of another size, and the layer itself may be scaled: both are done in one
 * step, bilinear (or nearest neighbour: fb_filter=0).
 */
void
IOMobileFramebuffer::drawLayer(MiniIOSurface * s, const int32_t * src, const int32_t * dst)
{
	int32_t sx = src[0], sy = src[1], sw = src[2], sh = src[3];
	int32_t dx = dst[0], dy = dst[1], dw = dst[2], dh = dst[3];
	int32_t px0, py0, px1, py1, cx0, cx1;
	bool filter = gFilter != 0;
	RowJob job;

	if (sw <= 0 || sh <= 0) {
		sx = sy = 0;
		sw = (int32_t) s->info.width;
		sh = (int32_t) s->info.height;
	}
	if (dw <= 0 || dh <= 0) {
		dx = dy = 0;
		dw = (int32_t) fWidth;
		dh = (int32_t) fHeight;
	}
	if (sx < 0 || sy < 0 || (uint32_t) (sx + sw) > s->info.width || (uint32_t) (sy + sh) > s->info.height) {
		return;
	}
	/* the rectangle on the panel */
	px0 = (int32_t) ((int64_t) dx * fPanelWidth / fWidth);
	py0 = (int32_t) ((int64_t) dy * fPanelHeight / fHeight);
	px1 = (int32_t) ((int64_t) (dx + dw) * fPanelWidth / fWidth);
	py1 = (int32_t) ((int64_t) (dy + dh) * fPanelHeight / fHeight);
	if (px1 <= px0 || py1 <= py0) {
		return;
	}
	/* (blending, a row of the picture mixed from two: room for it, for every one who draws rows) */
	if (filter && fBlendRowSize < (uint32_t) sw) {
		if (fBlendRow != NULL) {
			IODelete(fBlendRow, uint32_t, fBlendRowSize);
		}
		for (unsigned i = 0; i + 1 < fThreads; i++) {
			if (fHelperBlend[i] != NULL) {
				IODelete(fHelperBlend[i], uint32_t, fBlendRowSize);
			}
		}
		fBlendRowSize = (uint32_t) sw;
		fBlendRow = IONew(uint32_t, fBlendRowSize);
		for (unsigned i = 0; i + 1 < fThreads; i++) {
			fHelperBlend[i] = IONew(uint32_t, fBlendRowSize);
			if (fHelperBlend[i] == NULL && fBlendRow != NULL) {
				IODelete(fBlendRow, uint32_t, fBlendRowSize);
				fBlendRow = NULL;
			}
		}
		if (fBlendRow == NULL) {
			fBlendRowSize = 0;
		}
	}
	filter = filter && fBlendRow != NULL;
	/* which pixel of a source row each pixel of a panel row takes: worked out once, not 2.6 million times */
	cx0 = px0 < 0 ? 0 : px0;
	cx1 = (uint32_t) px1 > fPanelWidth ? (int32_t) fPanelWidth : px1;
	for (int32_t x = cx0; x < cx1; x++) {
		if (filter) {
			int64_t at = sample_at(x - px0, sw, px1 - px0);

			fColumn[x] = (uint32_t) (at >> 8);              /* (in the blended row: from sx on) */
			fColumnWeight[x] = (uint16_t) (fColumn[x] + 1 < (uint32_t) sw ? at & 255 : 0);
		} else {
			fColumn[x] = (uint32_t) (sx + (int64_t) (x - px0) * sw / (px1 - px0));
		}
	}
	job.pixels = s->pixels();
	job.stride = s->info.bytesPerRow;
	job.sx = sx;
	job.sy = sy;
	job.sw = sw;
	job.sh = sh;
	job.px0 = px0;
	job.py0 = py0;
	job.px1 = px1;
	job.py1 = py1;
	job.cx0 = cx0;
	job.cx1 = cx1;
	job.filter = filter;
	job.unscaled = px1 - px0 == sw && py1 - py0 == sh && px0 >= 0 && (uint32_t) px1 <= fPanelWidth;
	if (fThreads <= 1 || fDiff || fHelpersStarted + 1 < fThreads) {
		drawRows(&job, py0, py1, fBlendRow);
		return;
	}
	/* shared out: the helpers take their rows, this thread the first share, then waits for them */
	IOLockLock(fHelperLock);
	fHelperJob = &job;
	fHelpersBusy = fThreads - 1;
	fHelperGeneration++;
	IOLockWakeup(fHelperLock, &fHelperGeneration, false);
	IOLockUnlock(fHelperLock);
	drawRows(&job, py0, py0 + (int32_t) ((int64_t) (py1 - py0) / fThreads), fBlendRow);
	IOLockLock(fHelperLock);
	while (fHelpersBusy != 0) {
		IOLockSleep(fHelperLock, &fHelpersBusy, THREAD_UNINT);
	}
	IOLockUnlock(fHelperLock);
}

/*
 * How evenly the frames of the last seconds came: how many followed the one before within the
 * time of one refresh at 120, 60, 40 and 30 a second, and the times half and 95 in 100 of them
 * stayed under. With the lock held.
 */
void
IOMobileFramebuffer::reportFrameTimes(void)
{
	static const uint32_t limits[] = { 10, 18, 26, 35, FRAME_TIMES + 1 };
	uint32_t within[sizeof(limits) / sizeof(limits[0])] = { 0 }, total = 0, sum = 0, half = 0, most = 0;

	for (uint32_t ms = 0; ms <= FRAME_TIMES; ms++) {
		for (uint32_t i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
			if (ms < limits[i]) {
				within[i] += fFrameTimes[ms];
				break;
			}
		}
		total += fFrameTimes[ms];
	}
	for (uint32_t ms = 0; ms <= FRAME_TIMES && total != 0; ms++) {
		sum += fFrameTimes[ms];
		if (half == 0 && 2 * sum >= total) {
			half = ms + 1;
		}
		if (most == 0 && 100 * sum >= 95 * total) {
			most = ms + 1;
		}
	}
	if (total != 0) {
		IOLog("MiniIOMobileFramebuffer: from one frame to the next: %u times under 10 ms, %u under 18, %u under 26, %u under 35, "
		    "%u longer (the longest %u ms); half under %u ms, 95 in 100 under %u ms; %u pauses\n", within[0], within[1],
		    within[2], within[3], within[4], fFrameLongest, half, most, fFramePauses);
	}
	bzero(fFrameTimes, sizeof(fFrameTimes));
	fFramePauses = fFrameLongest = 0;
}

/* SwapEnd: the swap is handed to the thread that puts frames on the panel, as soon as that has no other waiting. */
void
IOMobileFramebuffer::present(const uint8_t * swap)
{
	AbsoluteTime deadline;

	uint64_t now = mach_absolute_time(), period, pause;

	/* (a frame that is late on the panel was late here already, or it was not: for telling whose it is) */
	nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz() * 5 / 4, &period);
	nanoseconds_to_absolutetime(FRAME_PAUSE * NSEC_PER_MSEC, &pause);
	clock_interval_to_deadline(SWAP_HELD_MS, kMillisecondScale, &deadline);
	if (gTouchLat) {
		redwood_tl("tl: s %u at %llu\n", swapWord(swap, SWAP_ID), micros(now));
	}
	IOLockLock(fSwapLock);
	fStatSwapsLate += fSwapTime != 0 && now - fSwapTime > period && now - fSwapTime < pause;
	fSwapTime = now;
	fStatHeld += fSwapsWaiting >= swapQueue();
	while (fSwapsWaiting >= swapQueue()) {
		if (IOLockSleepDeadline(fSwapLock, &fSwapsWaiting, deadline, THREAD_UNINT) == THREAD_TIMED_OUT &&
		    fSwapsWaiting >= swapQueue()) {
			fStatLost++;                    /* (the GPU has stopped answering: the frame before is waiting for it) */
			IOLockUnlock(fSwapLock);
			return;
		}
	}
	unsigned slot = (fSwapFirst + fSwapsWaiting) % SWAP_QUEUE;

	memcpy(fSwap[slot], swap, SWAP_SIZE);
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		uint32_t sid = swapWord(swap, SWAP_SURFACE(layer));

		fSwapSurfaces[slot][layer] = sid != 0 ? MiniIOSurfaceCopy(sid) : NULL;
		if (fSwapSurfaces[slot][layer] != NULL) {
			if (gInUse) {
				MiniIOSurfaceDisplayUse(fSwapSurfaces[slot][layer], 1);
			}
			fSwapSeeds[slot][layer] = MiniIOSurfaceSeed(fSwapSurfaces[slot][layer]);
		}
	}
	fSwapsWaiting++;
	IOLockWakeup(fSwapLock, fSwap, true);
	IOLockUnlock(fSwapLock);
}

void
IOMobileFramebuffer::presenterMain(void * self, wait_result_t result __unused)
{
	((IOMobileFramebuffer *) self)->presenter();
}

void
IOMobileFramebuffer::presenter(void)
{
	static uint8_t swap[SWAP_SIZE];         /* (one thread) */
	MiniIOSurface * surfaces[SWAP_LAYERS];

	for (;;) {
		IOLockLock(fSwapLock);
		while (fSwapsWaiting == 0) {
			IOLockSleep(fSwapLock, fSwap, THREAD_UNINT);
		}
		memcpy(swap, fSwap[fSwapFirst], SWAP_SIZE);
		memcpy(surfaces, fSwapSurfaces[fSwapFirst], sizeof(surfaces));
		memcpy(fShowSeeds, fSwapSeeds[fSwapFirst], sizeof(fShowSeeds));
		IOLockUnlock(fSwapLock);
		show(swap, surfaces);
		for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
			if (surfaces[layer] != NULL) {
				if (gInUse) {
					MiniIOSurfaceDisplayUse(surfaces[layer], -1);
				}
				MiniIOSurfaceDrop(surfaces[layer]);
			}
		}
		MiniIOSurfaceClearKept();               /* (the next frame is some milliseconds away) */
	}
}

/* A swap's surfaces onto the panel, once the GPU is done with them. */
void
IOMobileFramebuffer::show(const uint8_t * swap, MiniIOSurface ** surfaces)
{
	uint32_t set = swapWord(swap, SWAP_LAYERS_SET);
	uint64_t began = mach_absolute_time(), nanos, ready, shown, drawn;
	uint32_t layers = 0;
	MiniIOSurface * recorded = NULL;
	bool report;
	int gpuBuffer = -1;                     /* decon=3: the framebuffer the GPU has drawn this frame into */

	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		if (surfaces[layer] != NULL && !MiniIOSurfaceDrawn(surfaces[layer], SWAP_GPU_MS)) {
			IOLog("MiniIOMobileFramebuffer: swap %u: the GPU is not done with surface %u after %u ms; shown as it is\n",
			    swapWord(swap, SWAP_ID), swapWord(swap, SWAP_SURFACE(layer)), SWAP_GPU_MS);
		}
	}
	fStatWaiting += mach_absolute_time() - began;
	began = ready = mach_absolute_time();
	if (gOnTheBeat) {
		uint64_t period, aim, next;

		nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &period);
		aim = period * 3 / 8;
		next = fReady;
		fReady = began;
		if (fBeat == 0 || began - next > period * 3 / 2) {
			fBeat = began;
		} else {
			for (next = fBeat + period; next < began; next += period) {
			}
			if (next - began > aim) {
				next -= (next - began - aim) / 8;
			} else {
				next += (aim - (next - began)) / 8;
			}
			clock_delay_until(next);
			fBeat = next;
			fStatOnBeat++;
			fStatBeatWait += mach_absolute_time() - began;
			began = mach_absolute_time();
		}
	}
	shown = began;
	/* its turn has come: the swap after it may be handed over */
	IOLockLock(fSwapLock);
	fSwapFirst = (fSwapFirst + 1) % SWAP_QUEUE;
	fSwapsWaiting--;
	IOLockWakeup(fSwapLock, &fSwapsWaiting, true);
	IOLockUnlock(fSwapLock);
	IOLockLock(fLock);
	if (fLastFrame != 0) {
		absolutetime_to_nanoseconds(began - fLastFrame, &nanos);
		nanos /= NSEC_PER_MSEC;
		if (nanos >= FRAME_PAUSE) {
			fFramePauses++;
		} else {
			fFrameTimes[nanos < FRAME_TIMES ? nanos : FRAME_TIMES]++;
			fFrameLongest = nanos > fFrameLongest ? (uint32_t) nanos : fFrameLongest;
		}
	}
	fLastFrame = began;
	report = fSwapsShown < 8;
	if (report) {
		IOLog("MiniIOMobileFramebuffer: swap %u (#%u): surfaces %u %u %u %u, layers set 0x%x cleared 0x%x\n",
		    swapWord(swap, SWAP_ID), fSwapsShown, swapWord(swap, SWAP_SURFACE(0)), swapWord(swap, SWAP_SURFACE(1)),
		    swapWord(swap, SWAP_SURFACE(2)), swapWord(swap, SWAP_SURFACE(3)), set, swapWord(swap, SWAP_LAYERS_CLEARED));
	}
	deconHold();
	fStatRewrittenBefore += surfaceRewritten(surfaces);
	if (gDecon == 3) {
		/* drawn into a framebuffer by the GPU already? (one layer, the whole of its surface on the whole display) */
		uint32_t sid0 = swapWord(swap, SWAP_SURFACE(0));
		MiniIOSurface * s0 = surfaces[0];
		int32_t src[4], dst[4];
		bool only = sid0 != 0 && s0 != NULL && fGraphics;

		memcpy(src, swap + SWAP_SOURCE(0), sizeof(src));
		memcpy(dst, swap + SWAP_DEST(0), sizeof(dst));
		for (unsigned layer = 1; layer < SWAP_LAYERS; layer++) {
			only = only && swapWord(swap, SWAP_SURFACE(layer)) == 0;
		}
		only = only && (src[2] <= 0 || src[3] <= 0 || (src[0] == 0 && src[1] == 0 && (uint32_t) src[2] == s0->info.width &&
		    (uint32_t) src[3] == s0->info.height));
		only = only && (dst[2] <= 0 || dst[3] <= 0 || (dst[0] == 0 && dst[1] == 0 && (uint32_t) dst[2] == fWidth &&
		    (uint32_t) dst[3] == fHeight));
		gpuBuffer = gpuFrameTake(sid0, only);
		if (gpuBuffer >= 0) {
			gFBDraw = (unsigned) gpuBuffer;
			layers = 1;
		}
	}
	for (unsigned layer = 0; gpuBuffer < 0 && layer < SWAP_LAYERS; layer++) {
		uint32_t sid = swapWord(swap, SWAP_SURFACE(layer));
		int32_t src[4], dst[4];
		MiniIOSurface * s;

		layers |= sid != 0 ? 1u << layer : 0;
		/* the frames' surfaces: is it a few, in turn? */
		for (unsigned i = 0; layer == 0 && sid != 0 && i < 8; i++) {
			if (fStatSurfaces[i] == 0 || fStatSurfaces[i] == sid) {
				fStatSurfaces[i] = sid;
				break;
			}
		}
		if (sid == 0) {
			/* the bottom layer taken away: nothing is shown, the screen is black */
			if (layer == 0 && (set & 1) && fGraphics) {
				for (uint32_t y = 0; y < fPanelHeight; y++) {
					bzero((gDecon == 3 ? gFBVirt[gFBDraw] : fBase) + (size_t) y * fRowBytes, (size_t) fPanelWidth * 4);
				}
			}
			continue;
		}
		memcpy(src, swap + SWAP_SOURCE(layer), sizeof(src));
		memcpy(dst, swap + SWAP_DEST(layer), sizeof(dst));
		s = surfaces[layer];
		if (report) {
			IOLog("MiniIOMobileFramebuffer: swap %u layer %u: surface %u%s (%ux%u '%c%c%c%c') "
			    "from %d,%d %dx%d to %d,%d %dx%d, layers set 0x%x\n", swapWord(swap, SWAP_ID), layer, sid,
			    s ? "" : " MISSING", s ? s->info.width : 0, s ? s->info.height : 0,
			    s ? (int) (s->info.pixelFormat >> 24 & 0xff) : '?', s ? (int) (s->info.pixelFormat >> 16 & 0xff) : '?',
			    s ? (int) (s->info.pixelFormat >> 8 & 0xff) : '?', s ? (int) (s->info.pixelFormat & 0xff) : '?',
			    src[0], src[1], src[2], src[3], dst[0], dst[1], dst[2], dst[3], set);
		}
		if (s == NULL) {
			continue;
		}
		if (s->info.bytesPerElement == 4 && s->info.planeCount == 0) {
			if (!fGraphics) {
				/* the first picture: the kernel's text console must stop drawing over it */
				fGraphics = true;
				PE_initialize_console(NULL, kPEGraphicsMode);
			}
			drawLayer(s, src, dst);
			if (layer == 0) {
				recorded = s;   /* (recorded once the frame is in the panel's memory: see below) */
			}
		}
	}
	fStatRewrittenDuring += surfaceRewritten(surfaces);
	if (gDecon == 3 && gpuBuffer < 0) {
		gpuCopyDone();
	}
	deconRelease();
	drawn = mach_absolute_time();
	if (recorded != NULL) {
		record(recorded);
	}
	fSwapsShown++;
	if (gTouchLat) {
		redwood_tl("tl: v %u ready %llu shown %llu done %llu\n", swapWord(swap, SWAP_ID), micros(ready), micros(shown),
		    micros(drawn));
	}
	if (layers != fLayersShown) {
		IOLog("MiniIOMobileFramebuffer: swap #%u: surfaces in layers 0x%x now (0x%x before): %u %u %u %u\n", fSwapsShown, layers,
		    fLayersShown, swapWord(swap, SWAP_SURFACE(0)), swapWord(swap, SWAP_SURFACE(1)), swapWord(swap, SWAP_SURFACE(2)),
		    swapWord(swap, SWAP_SURFACE(3)));
		fLayersShown = layers;
	}

	/* every five seconds in which something was drawn: how many frames, and what each cost here */
	fStatFrames++;
	fStatDrawing += mach_absolute_time() - began;
	if (fStatSince == 0) {
		fStatSince = began;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - fStatSince, &nanos);
	if (nanos >= 5 * NSEC_PER_SEC) {
		uint64_t drawing, waiting, beat;
		unsigned different;

		absolutetime_to_nanoseconds(fStatDrawing, &drawing);
		absolutetime_to_nanoseconds(fStatWaiting, &waiting);
		absolutetime_to_nanoseconds(fStatBeatWait, &beat);
		IOLog("MiniIOMobileFramebuffer: %u frames in %llu ms (%llu a second), %llu us each in the driver, after %llu us "
		    "waiting for the GPU and, %u of them, %llu us for the next refresh; %u swaps waited for the frame before, "
		    "%u gave up; %u swaps came a refresh late themselves\n", fStatFrames, nanos / NSEC_PER_MSEC,
		    fStatFrames * NSEC_PER_SEC / nanos, drawing / fStatFrames / NSEC_PER_USEC,
		    waiting / fStatFrames / NSEC_PER_USEC, fStatOnBeat, fStatOnBeat ? beat / fStatOnBeat / NSEC_PER_USEC : 0,
		    fStatHeld, fStatLost, fStatSwapsLate);
		fStatSwapsLate = 0;
		if (gDecon == 2) {
			uint64_t chaseWait;

			absolutetime_to_nanoseconds(gStatChaseWait, &chaseWait);
			IOLog("MiniIOMobileFramebuffer: decon=2: %u frames scaled while the one before was going out; their drawers "
			    "waited %llu us each for DECON0, all of them together\n", gStatChased,
			    gStatChased ? chaseWait / gStatChased / NSEC_PER_USEC : 0);
			gStatChased = 0;
			gStatChaseWait = 0;
		}
		if (gDecon) {
			IOLog("MiniIOMobileFramebuffer: decon=%u: %u frames scaled, %u of them while a frame started; %u found DECON0 "
			    "idle with its output FIFO not empty\n", gDecon, gStatCopies, gStatStartedInCopy, gStatIdleButSending);
			gStatCopies = gStatStartedInCopy = gStatIdleButSending = 0;
		}
		if (gDecon == 3 || gStatFlips) {
			IOLog("MiniIOMobileFramebuffer: decon=3: %u frames drawn into a framebuffer by the GPU, %u GPU frames not "
			    "shown, %u waits for a buffer\n", gStatGPUShown, gStatGPUDropped, gStatGPUWaits);
			gStatGPUShown = gStatGPUDropped = gStatGPUWaits = 0;
			IOLog("MiniIOMobileFramebuffer: decon=3: %u flips; %u frames found the other buffer still going out; %u DMA "
			    "errors (bits 0x%x); flips checked %u, not taken %u\n", gStatFlips, gStatBackBusy, gStatDmaErrors,
			    gDmaErrorBits, gFlipsChecked, gFlipsMissed);
			gStatFlips = gStatBackBusy = 0;
		}
		uint64_t shownWait, lockWait;

		absolutetime_to_nanoseconds(mali_shown_wait_abs, &shownWait);
		absolutetime_to_nanoseconds(gSurfaceDisplayWaitAbs, &lockWait);
		IOLog("MiniIOMobileFramebuffer: fb_inuse=%u: %u frames' surfaces drawn again after their swap before the copy, "
		    "%u more by its end; GPU work waited for a surface being shown %u times, %llu us each, %u gave up; write "
		    "locks waited %u times, %llu us each, %u gave up\n", gInUse,
		    fStatRewrittenBefore, fStatRewrittenDuring - fStatRewrittenBefore, mali_shown_waits,
		    mali_shown_waits ? shownWait / mali_shown_waits / NSEC_PER_USEC : 0, mali_shown_timeouts, gSurfaceDisplayWaits,
		    gSurfaceDisplayWaits ? lockWait / gSurfaceDisplayWaits / NSEC_PER_USEC : 0, gSurfaceDisplayTimeouts);
		fStatRewrittenBefore = fStatRewrittenDuring = 0;
		mali_shown_waits = mali_shown_timeouts = 0;
		mali_shown_wait_abs = 0;
		gSurfaceDisplayWaits = gSurfaceDisplayTimeouts = 0;
		gSurfaceDisplayWaitAbs = 0;
		for (different = 0; different < 8 && fStatSurfaces[different] != 0; different++) {
		}
		if (different == 8 && fStatFrames > 8) {
			uint32_t made, freed, again;
			uint64_t making, freeing;

			/* (CoreAnimation has made itself a new surface for every frame: see "the frames' surfaces" above) */
			MiniIOSurfaceCost(&made, &making, &freed, &freeing, &again);
			IOLog("MiniIOMobileFramebuffer: the frames came in surfaces %u, %u, %u, %u, %u and on, none twice; in that time "
			    "%u surfaces of all kinds were made, in %llu us each (%u of them of memory kept from one before), and %u "
			    "freed, in %llu us each\n", fStatSurfaces[0], fStatSurfaces[1], fStatSurfaces[2], fStatSurfaces[3],
			    fStatSurfaces[4], made, making, again, freed, freeing);
		}
		bzero(fStatSurfaces, sizeof(fStatSurfaces));
		fStatHeld = fStatLost = fStatOnBeat = 0;
		fStatWaiting = fStatBeatWait = 0;
		reportFrameTimes();
		if (fDiff) {
			IOLog("MiniIOMobileFramebuffer: %llu pixels of the panel changed a frame, all within %u,%u to %u,%u\n",
			    fDiffPixels / fStatFrames, fDiffLeft, fDiffTop, fDiffRight, fDiffBottom);
			fDiffPixels = 0;
			fDiffLeft = fDiffTop = ~0u;
			fDiffRight = fDiffBottom = 0;
		}
		fStatFrames = 0;
		fStatDrawing = 0;
		fStatSince = 0;
	}
	IOLockUnlock(fLock);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class IOMobileFramebufferUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(IOMobileFramebufferUserClient);

	task_t                  fTask;
	IOMobileFramebuffer *   fOwner;
	IOLock *                fLock;
	mach_port_t             fPorts[kFBNotifyTypes];
	mach_vm_address_t       fCallbacks[kFBNotifyTypes];
	io_user_reference_t     fRefcons[kFBNotifyTypes];
	thread_call_t           fVSyncCall;
	uint64_t                fVSyncCount;
	uint64_t                fVSyncTime;             /* when the refresh being waited for is due */
	bool                    fVSyncRunning;
	bool                    fClosed;

	static void vsyncFired(thread_call_param_t self, thread_call_param_t unused);
	void vsync(void);
	void armVSync(void);

public:
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOMobileFramebufferUserClient, IOUserClient);

bool
IOMobileFramebufferUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
IOMobileFramebufferUserClient::start(IOService * provider)
{
	fOwner = OSDynamicCast(IOMobileFramebuffer, provider);
	if (fOwner == NULL || !IOUserClient::start(provider)) {
		return false;
	}
	fLock = IOLockAlloc();
	fVSyncCall = thread_call_allocate(&IOMobileFramebufferUserClient::vsyncFired, this);
	return fLock != NULL && fVSyncCall != NULL;
}

IOReturn
IOMobileFramebufferUserClient::clientClose(void)
{
	if (fLock != NULL) {
		IOLockLock(fLock);
		fClosed = true;
		IOLockUnlock(fLock);
	}
	if (fVSyncCall != NULL) {
		thread_call_cancel_wait(fVSyncCall);
	}
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

void
IOMobileFramebufferUserClient::free(void)
{
	if (fVSyncCall != NULL) {
		thread_call_cancel_wait(fVSyncCall);
		thread_call_free(fVSyncCall);
		fVSyncCall = NULL;
	}
	for (unsigned i = 0; i < kFBNotifyTypes; i++) {
		if (fPorts[i] != NULL) {
			releaseNotificationPort(fPorts[i]);
			fPorts[i] = NULL;
		}
	}
	if (fLock != NULL) {
		IOLockFree(fLock);
		fLock = NULL;
	}
	IOUserClient::free();
}

IOReturn
IOMobileFramebufferUserClient::registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon)
{
	mach_port_t old;

	IOLog("MiniIOMobileFramebuffer: notification port for type %u\n", (unsigned) type);
	if (type >= kFBNotifyTypes) {
		return kIOReturnBadArgument;
	}
	IOLockLock(fLock);
	old = fPorts[type];
	fPorts[type] = port;
	IOLockUnlock(fLock);
	if (old != NULL) {
		releaseNotificationPort(old);
	}
	return kIOReturnSuccess;
}

void
IOMobileFramebufferUserClient::vsyncFired(thread_call_param_t self, thread_call_param_t unused)
{
	((IOMobileFramebufferUserClient *) self)->vsync();
}

/* With the lock held. */
void
IOMobileFramebufferUserClient::armVSync(void)
{
	uint64_t period, now = mach_absolute_time();

	/* one refresh after the last, not after whenever we got round to this: the rate does not drift */
	nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &period);
	fVSyncTime += period;
	if (fVSyncTime <= now || fVSyncTime > now + 2 * period) {
		fVSyncTime = now + period;
	}
	thread_call_enter_delayed(fVSyncCall, fVSyncTime);
}

/*
 * One refresh: tell the client, which gets three 64-bit numbers as six 32-bit halves --
 * the time of this refresh, the same again (the time it was presented), and a counter.
 */
void
IOMobileFramebufferUserClient::vsync(void)
{
	OSAsyncReference64 ref;
	io_user_reference_t args[6];
	uint64_t now;
	bool send = false;

	IOLockLock(fLock);
	now = fVSyncTime;
	if (fClosed || fCallbacks[kFBNotifyVSync] == 0 || fPorts[kFBNotifyVSync] == NULL) {
		fVSyncRunning = false;
	} else {
		fVSyncCount++;
		setAsyncReference64(ref, fPorts[kFBNotifyVSync], fCallbacks[kFBNotifyVSync], fRefcons[kFBNotifyVSync], fTask);
		args[0] = (uint32_t) now;
		args[1] = now >> 32;
		args[2] = (uint32_t) now;
		args[3] = now >> 32;
		args[4] = (uint32_t) fVSyncCount;
		args[5] = fVSyncCount >> 32;
		send = true;
		armVSync();
	}
	IOLockUnlock(fLock);
	if (send) {
		sendAsyncResult64WithOptions(ref, kIOReturnSuccess, args, 6, kIOUserNotifyOptionCanDrop);
	}
}

IOReturn
IOMobileFramebufferUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	const uint64_t * in = args->scalarInput;
	uint64_t * out = args->scalarOutput;
	MiniIOSurface * s;

	if (selector < 128 && gFBCalls[selector] < gFBTrace) {
		char procname[32] = "?";

		gFBCalls[selector]++;
		proc_selfname(procname, sizeof(procname));
		IOLog("MiniIOMobileFramebuffer: %s[%d] selector %u (scalars %u in [0x%llx 0x%llx 0x%llx 0x%llx], %u out; "
		    "structure %u in, %u out)\n", procname, proc_selfpid(), selector, args->scalarInputCount,
		    args->scalarInputCount > 0 ? in[0] : 0, args->scalarInputCount > 1 ? in[1] : 0,
		    args->scalarInputCount > 2 ? in[2] : 0, args->scalarInputCount > 3 ? in[3] : 0, args->scalarOutputCount,
		    args->structureInputSize, args->structureOutputSize);
	}

	switch (selector) {
	case kFBMethodGPUFrameBegin: {
		int k;

		if (args->scalarOutputCount < 5) {
			return kIOReturnBadArgument;
		}
		k = gpuFrameBegin(args->scalarInputCount > 0 ? (uint32_t) in[0] : 0);
		out[0] = (uint64_t) (int64_t) k;
		out[1] = k >= 0 ? FB_GPU_SURFACE((unsigned) k) : 0;
		out[2] = gFBWidthPx;
		out[3] = gFBHeightPx;
		out[4] = gFBRowBytes;
		args->scalarOutputCount = 5;
		return k >= 0 ? kIOReturnSuccess : kIOReturnUnsupported;
	}
	case kFBMethodGPUFrameDone:
		if (args->scalarInputCount < 2 || in[0] > 1) {
			return kIOReturnBadArgument;
		}
		gpuFrameDone((unsigned) in[0], (uint32_t) in[1]);
		return kIOReturnSuccess;

	case kFBMethodGetDisplaySize:                   /* out: width, height */
		if (args->scalarOutputCount < 2) {
			return kIOReturnBadArgument;
		}
		out[0] = fOwner->fWidth;
		out[1] = fOwner->fHeight;
		args->scalarOutputCount = 2;
		return kIOReturnSuccess;

	case kFBMethodGetID:
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = 1;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodGetDotPitch:                      /* out: pixels per inch */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = kFBDotsPerInch * fOwner->fWidth / kFBDefaultWidth;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodGetLayerDefaultSurface:           /* in: size wanted (or 0, 0); out: a send right for the surface */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		s = fOwner->defaultSurface();
		if (s == NULL) {
			return kIOReturnNoMemory;
		}
		out[0] = MiniIOSurfaceMakeSendRight(fTask, s);
		args->scalarOutputCount = 1;
		return out[0] != 0 ? kIOReturnSuccess : kIOReturnNoMemory;

	case kFBMethodSwapBegin:                        /* out: the number of the new swap */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = OSIncrementAtomic(&fOwner->fSwapID) + 1;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodSwapEnd:
		if (args->structureInput == NULL || args->structureInputSize < SWAP_SIZE) {
			IOLog("MiniIOMobileFramebuffer: swap request of %u bytes, expected %u\n", args->structureInputSize, SWAP_SIZE);
			return kIOReturnBadArgument;
		}
		fOwner->present((const uint8_t *) args->structureInput);
		return kIOReturnSuccess;

	case kFBMethodSwapWait:                         /* a swap is on the screen as soon as it is made */
		if (gTouchLat) {
			redwood_tl("tl: w %llu %#llx %#llx at %llu\n", args->scalarInputCount > 0 ? in[0] : 0,
			    args->scalarInputCount > 1 ? in[1] : 0, args->scalarInputCount > 2 ? in[2] : 0, micros(mach_absolute_time()));
		}
		return kIOReturnSuccess;
	case kFBMethodSwapSignal:
	case kFBMethodSwapCancel:
	case kFBMethodSwapCancelAll:
	case kFBMethodRequestPowerChange:
	case kFBMethodChangeFrameInfo:
		return kIOReturnSuccess;

	case kFBMethodNotifications:                    /* in: callback (0 = off), its argument, type, 0 */
		if (args->scalarInputCount < 3 || in[2] >= kFBNotifyTypes) {
			return kIOReturnBadArgument;
		}
		IOLockLock(fLock);
		fCallbacks[in[2]] = in[0];
		fRefcons[in[2]] = in[1];
		if (in[2] == kFBNotifyVSync && in[0] != 0 && !fVSyncRunning && !fClosed) {
			fVSyncRunning = true;
			armVSync();
		}
		IOLockUnlock(fLock);
		return kIOReturnSuccess;

	case kFBMethodSupportedFrameInfo:               /* in: type; out: how many, and their tags: none */
		if (args->scalarOutputCount >= 1) {
			out[0] = 0;
			args->scalarOutputCount = 1;
		}
		if (args->structureOutput != NULL) {
			bzero(args->structureOutput, args->structureOutputSize);
		}
		return kIOReturnSuccess;

	case kFBMethodGetBlock:                         /* in: which block; out: its bytes */
		if (args->structureOutput == NULL) {
			return kIOReturnBadArgument;
		}
		bzero(args->structureOutput, args->structureOutputSize);
		if (args->scalarInputCount >= 1 && in[0] == 0x40 && args->structureOutputSize >= 12) {
			/* the current mode: its colour and timing identifiers */
			uint32_t ids[2] = { kFBColorID, kFBTimingID };

			memcpy((uint8_t *) args->structureOutput + 4, ids, sizeof(ids));
		}
		return kIOReturnSuccess;

	case kFBMethodGetCurrentAbsoluteTime:
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = mach_absolute_time();
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	default:
		break;
	}

	if (selector < 128 && !(gFBLogged[selector / 64] & (1ULL << (selector % 64)))) {
		gFBLogged[selector / 64] |= 1ULL << (selector % 64);
		IOLog("MiniIOMobileFramebuffer: selector %u not implemented (scalars %u in [0x%llx 0x%llx 0x%llx], %u out; "
		    "structure %u in, %u out)\n", selector, args->scalarInputCount, args->scalarInputCount > 0 ? in[0] : 0,
		    args->scalarInputCount > 1 ? in[1] : 0, args->scalarInputCount > 2 ? in[2] : 0, args->scalarOutputCount,
		    args->structureInputSize, args->structureOutputSize);
	}
	/* a request that only sets something is accepted; one that wants an answer gets zeros */
	if (args->scalarOutputCount != 0) {
		bzero(out, args->scalarOutputCount * sizeof(out[0]));
	}
	if (args->structureOutput != NULL && args->structureOutputSize != 0) {
		bzero(args->structureOutput, args->structureOutputSize);
	}
	return kIOReturnSuccess;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * IOMobileFramebuffer.framework counts the displays it should wait for by looking for
 * children of /arm-io in the device tree whose device_type is "display-subsystem".
 */
static void
publishDisplayNode(void)
{
	IORegistryEntry * armio = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
	IORegistryEntry * node;
	OSDictionary * props;
	OSData * data;

	if (armio == NULL) {
		IOLog("MiniIOMobileFramebuffer: no /arm-io in the device tree\n");
		return;
	}
	node = OSTypeAlloc(IORegistryEntry);
	props = OSDictionary::withCapacity(4);
	if (node != NULL && props != NULL) {
		data = OSData::withBytes("display-subsystem", sizeof("display-subsystem"));
		props->setObject("device_type", data);
		OSSafeReleaseNULL(data);
		data = OSData::withBytes("redwood,framebuffer", sizeof("redwood,framebuffer"));
		props->setObject("compatible", data);
		OSSafeReleaseNULL(data);
		data = OSData::withBytes("disp0", sizeof("disp0"));
		props->setObject("name", data);
		OSSafeReleaseNULL(data);
		if (node->init(props)) {
			node->setName("disp0");
			node->attachToParent(armio, gIODTPlane);
		}
	}
	OSSafeReleaseNULL(props);
	OSSafeReleaseNULL(node);
	armio->release();
}

/*
 * The graphics accelerator. There is none, but CoreAnimation waits half a minute for a
 * service of this class to appear before it looks for a GPU at all (once per process). An
 * accelerator without the properties that name its Metal and OpenGL ES drivers is passed
 * over by both, which leaves CoreAnimation with its software renderer, as before.
 *
 * With boot-arg metal=1 a user process may put those two properties here, which is the whole
 * of what keeps Metal from finding our driver; see setProperties below, and docs/metal.md 10.31
 * (10.14 for the measurement that made it a boot-arg).
 */

/* Metal's step 2 reads exactly these two, in this order, off this service (docs/metal.md 1). */
#define kAcceleratorPluginNameKey      "MetalPluginName"
#define kAcceleratorPluginClassNameKey "MetalPluginClassName"

static bool gAcceleratorTakesProperties;        /* boot-arg metal=1 */

class IOAcceleratorES : public IOService
{
	OSDeclareDefaultStructors(IOAcceleratorES);

public:
	virtual IOReturn setProperties(OSObject * properties) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOAcceleratorES, IOService);

/*
 * Switch Metal's own device discovery on, from user space: IORegistryEntrySetCFProperties() with
 * MetalPluginName and MetalPluginClassName. Metal reads the first (getMetalPluginClassForService,
 * 0x181a39ba4), builds "/System/Library/Extensions/" + it + ".bundle", opens that with NSBundle
 * and takes the class the second names; with the first missing it returns nil before it even
 * reads the second, and that single `cbz` is where the chain breaks on this port today.
 *
 * This method is the only thing that was in the way. IORegistryEntry::setProperties is
 * "return kIOReturnUnsupported" (IORegistryEntry.cpp) and IOService does not override it; the
 * MAC check in front of the call has no policy to ask, because this kernel registers no iokit
 * hook (bsd/kern/kern_minimac.c); and this service carries no property whitelist.
 *
 * Why it is gated on a boot-arg, which is a measured risk and not caution in general
 * (docs/metal.md 10.14): the conditions Metal puts on a device object are asserts, not soft
 * refusals (10.2-10.3). A process that asks Metal and is handed a half-made device does not
 * degrade to another renderer -- it aborts ("Assertion failed: ([self
 * conformsToProtocol:@protocol(MTLDeviceSPI)])", "failed assertion _limits.maxColorAttachments
 * > 0"), and an aborting daemon is restarted by launchd, which is a loop. Published instead at
 * registerService() time, the properties would exist before launchd, backboardd and SpringBoard
 * are born, so each of the three would take the Metal road on its first call, with nobody left
 * to read the log. Behind a boot-arg, a machine that has them is a machine booted on purpose to
 * have them, and the first such boot is meant to be RC=rc.ios, where launchd is never born.
 *
 * An empty string removes a property rather than setting it. That is the way out of the loop
 * above without a reboot: the properties live only in the registry, so processes started after
 * they are gone go back to the software road.
 *
 * Everything else is refused, and refused whole, before anything is set: every process on the
 * machine can reach this method and the only thing it exists for is this one switch.
 */
IOReturn
IOAcceleratorES::setProperties(OSObject * properties)
{
	OSDictionary * dict = OSDynamicCast(OSDictionary, properties);
	OSCollectionIterator * keys;
	OSObject * next;
	char who[32] = "?";

	if (!gAcceleratorTakesProperties) {
		static unsigned said;

		if (said++ < 4) {
			proc_selfname(who, sizeof(who));
			IOLog("IOAcceleratorES: %s asks to set properties on it; boot-arg metal=1 was not given\n", who);
		}
		return kIOReturnUnsupported;
	}
	if (dict == NULL || dict->getCount() == 0) {
		return kIOReturnBadArgument;
	}
	keys = OSCollectionIterator::withCollection(dict);
	if (keys == NULL) {
		return kIOReturnNoMemory;
	}
	proc_selfname(who, sizeof(who));
	while ((next = keys->getNextObject()) != NULL) {
		const OSSymbol * key = OSDynamicCast(OSSymbol, next);

		if (key == NULL || (!key->isEqualTo(kAcceleratorPluginNameKey) &&
		    !key->isEqualTo(kAcceleratorPluginClassNameKey))) {
			IOLog("IOAcceleratorES: refusing what %s sent: only " kAcceleratorPluginNameKey " and "
			    kAcceleratorPluginClassNameKey " are taken here\n", who);
			keys->release();
			return kIOReturnUnsupported;
		}
		if (OSDynamicCast(OSString, dict->getObject(key)) == NULL) {
			IOLog("IOAcceleratorES: refusing \"%s\" from %s: Metal reads it as a CFString\n",
			    key->getCStringNoCopy(), who);
			keys->release();
			return kIOReturnBadArgument;
		}
	}
	keys->reset();
	while ((next = keys->getNextObject()) != NULL) {
		const OSSymbol * key = OSDynamicCast(OSSymbol, next);
		OSString * value = OSDynamicCast(OSString, dict->getObject(key));

		if (value->getLength() == 0) {
			removeProperty(key);
			IOLog("IOAcceleratorES: %s removed by %s\n", key->getCStringNoCopy(), who);
		} else if (setProperty(key, value)) {
			IOLog("IOAcceleratorES: %s = %s, set by %s\n", key->getCStringNoCopy(),
			    value->getCStringNoCopy(), who);
		} else {
			IOLog("IOAcceleratorES: %s could not be set\n", key->getCStringNoCopy());
			keys->release();
			return kIOReturnNoMemory;
		}
	}
	keys->release();
	return kIOReturnSuccess;
}

static void
publishAccelerator(IOService * provider)
{
	IOAcceleratorES * accelerator = OSTypeAlloc(IOAcceleratorES);
	uint32_t metal = 0;

	gAcceleratorTakesProperties = PE_parse_boot_argn("metal", &metal, sizeof(metal)) && metal != 0;
	if (accelerator != NULL && accelerator->init() && accelerator->attach(provider)) {
		accelerator->setName("sgx");
		accelerator->registerService();
		/* the first line to look for in the log of such a boot: the gate is open, nothing is set yet */
		if (gAcceleratorTakesProperties) {
			IOLog("IOAcceleratorES: metal=%u: this service takes " kAcceleratorPluginNameKey
			    " and " kAcceleratorPluginClassNameKey " from user space\n", metal);
		}
	}
	OSSafeReleaseNULL(accelerator);
}

/*
 * A picture from inside the kernel (the GPU driver's first ones): rows of 32-bit pixels, put at
 * the top left corner of the panel as they are and cut to its size.
 */
extern "C" void
MiniIOMobileFramebufferShow(const uint8_t * pixels, uint32_t width, uint32_t height, uint32_t rowBytes)
{
	uint32_t columns = width < vinfo.v_width ? width : (uint32_t) vinfo.v_width;
	uint32_t rows = height < vinfo.v_height ? height : (uint32_t) vinfo.v_height;

	if (vinfo.v_baseaddr == 0 || vinfo.v_depth != 32) {
		return;
	}
	for (uint32_t y = 0; y < rows; y++) {
		memcpy((void *) (vinfo.v_baseaddr + (size_t) y * vinfo.v_rowbytes), pixels + (size_t) y * rowBytes,
		    (size_t) columns * 4);
	}
}

void
MiniIOMobileFramebufferStart(IOService * provider)
{
	IOMobileFramebuffer * fb = OSTypeAlloc(IOMobileFramebuffer);

	if (fb == NULL || !fb->init() || !fb->attach(provider)) {
		OSSafeReleaseNULL(fb);
		return;
	}
	if (fb->start(provider)) {
		publishDisplayNode();
		publishAccelerator(provider);
	} else {
		fb->detach(provider);
	}
	fb->release();
}
