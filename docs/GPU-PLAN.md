# GPU acceleration on the Pixel 7 Pro (Mali-G710) — plan and status

## Done
- **Phase 1 — wake the GPU** (`pixelboot/gpuprobe`, 2026-10-23): power domains `pd-g3d`
  (PMU 0x18061e00) and `pd-embedded_g3d` (0x18062000) are already on after the bootloader;
  GPU at 0x28000000, `GPU_ID a8620004` (G710, arch 10.8), 7 shader cores (`0x01110055`),
  `CSF_ID 040a0412`; clock: ACPM DVFS domains 5/6 (151 MHz at boot, Android max 900 MHz).
  The L2 powers up on command.
- **Phase 2 — boot the CSF firmware** (`pixelboot/gpufw`): `downloads/gpu/mali_csffw-r54p3.bin`
  (Android's, driver r54p3) parsed, its 8 sections loaded into RAM and mapped with the GPU's
  own page tables (AArch64 4K, 48-bit; address space 0 = the MCU's), `MCU_CONTROL = AUTO`:
  the firmware signals `JOB_INT_GLOBAL_IF` after 188 µs, `MCU_STATUS 1`; global interface
  version 1.5.0, 8 command-stream groups (stride 0xa0), input 0x04002000, output 0x04006000.
  Open: `MMU_INT_RAWSTAT 0x10000` (address space 0 bus error) after boot; Google's driver
  also "patches csffw" at load (what, still unknown).

- **Kernel built here** (2026-10-23 23:30, WSL `~/hackios`: theos Linux toolchain in
  `~/theos/toolchain/linux/iphone`, `uuid-dev`, `xnu-deps/host/bin/dsymutil -> dsymutil-18`;
  `xnu-deps/setup.sh && ./xnu-build.sh -j12 -k` — the errors after "built kernel at" are only
  symbol sets and lldb macros). It boots iOS to the home screen like the prebuilt one.
- **Phase 3a — firmware from the kernel** (`gpu/redwood_mali.c`, copied into `xnu/files/bsd/dev`
  and the applied tree, listed in `bsd/conf/files`, called after `redwood_adreno_init()`;
  boot-arg `mali=1`; firmware as `~/hackios/fw-gpu/mali_csffw.bin`, which userland/build.sh puts
  into the small image's `fw/`): `mali: firmware UP after 170 us`, global interface 1.5.0,
  8 groups, iOS booting to the home screen beside it. Run with
  `KERNEL=downloads/kernel/mach.development.bcm2837.mali RAMDISK=downloads/ios/root-mali.apfs
  XARGS="mali=1" bash pixelboot/tools/ios-run.sh NAME 240`.

- **Phase 3b — THE GPU RAN OUR COMMANDS** (2026-10-24 01:32, test mali11, boot-arg `mali=2`): global
  interface configured and pinged, group 0 / stream 0 started in address space 1, a ring of
  MOVE48/MOVE32/STORE_MULTIPLE executed: `the target holds 0xcafe1234`. What it took, beyond
  panthor: the firmware's processor reaches only 27 bits, so every buffer it sees must be in
  0x04000000-0x07ffffff (above that addresses wrap: faults at 0x1000 and 0x04070000); Pixel's
  trace buffers (fwlog, benchmark, timeline, gpu_metrics) given memory as kbase does; the
  protected-mode section loaded (plain memory); a protected suspend buffer; coherency NONE. MMU irq
  bits 16-31 appear after every AS UPDATE and are ignored (as panthor does). WAIT masks: 8
  scoreboards (0xffff was CS_INSTR_INVALID 0x49).

- **Phase 3c-1 — the driver core** (`gpu/redwood_mali_core.c`, included by redwood_mali.c;
  2026-10-24 01:44, test mali12): VMs (address spaces 1-7, map with a locked-region FLUSH_PT),
  kernel buffers, groups in the firmware's 8 slots with up to 8 queues (rings in the group's VM,
  pointer pages and suspend buffers premapped for every slot inside the firmware's reach), jobs as
  panthor emits them (flush, CALL of the caller's stream, SYNC_ADD64 +1, ERROR_BARRIER), waited
  for on the queue's sync object: `TWO JOBS RAN THROUGH THE DRIVER CORE`.

- **Phase 3c-2 — MaliGPU, the interface for Mesa** (2026-10-24 03:44, test mali13, boot-arg `mali=3`):
  `gpu/MaliGPU.cpp` (IOKit service + user client; buffers are write-combined
  IOBufferMemoryDescriptors, mapped with IOConnectMapMemory64, memory type = buffer handle; errors
  as `0xe0008000 | errno`) over `gpu/redwood_mali_uapi.c` (panthor requests 0x40+n and DRM
  GEM_CLOSE/SYNCOBJ_*; selector = ioctl number's low byte). `gpu/mali_test.c` (built in WSL with
  clang + iPhoneOS16.5.sdk, packed with `EXTRA=... RC_JOBS=rc.jobs.mali`, image
  `downloads/ios/root-malitest.apfs`) ran a job through it from an ordinary program: `the buffer
  holds 0x5ee0beef`. Simplest forms still: CPU-side waits, no tiler-heap growth, nothing torn
  down but buffers.

- **Phase 4 — MESA DRAWS ON THE MALI** (2026-10-24 04:10, test gltest2, boot-arg `mali=3`):
  `gl_test: Mesa; Mali-G710 (Panfrost); OpenGL ES 3.1 Mesa 25.0.7`, the triangle right (corner
  26 26 51, red/green/blue corners 235 12 8 / 12 235 8 / 10 10 235, 26% covered = geometry), 30 ms
  vs softpipe's 77. What it took: the libdrm stand-in for panthor (`gpu/mesa/redwood_drm.c` +
  `xf86drm.h`, copied to `~/hackios/tools/mesa/redwood-drm`: syncobj calls, `redwood_drm_mmap`
  with handle = offset >> 32, the flush-ID page a page of zeros) and `gpu/mesa/panfrost-darwin.patch`
  (in `tools/mesa/`; made with `git diff` in WSL `~/mesa-work/mesa-25.0.7`): stand-in built with
  panfrost's kmod, mmap/munmap through it, OSMesa opening "panthor", and the **G710 (0xa862) added
  to `pan_props.c`** -- Mesa 25.0.7 knows only the G610, and without it panfrost bails silently.
  Build: `DRIVERS=softpipe,panfrost tools/mk-mesa.sh` (llvm-18 on PATH; Mako from apt
  `python3-mako`, venv `--system-site-packages`). Test image: `claude_gt.sh` → `root-gltest.apfs`
  (gl_test + the library in /redwood/bin, `rc.jobs.panfrost`).

- **Fill test runs** (2026-10-24 04:30, test gltest4): `gl_test fill` was not a hang but a
  SIGSEGV -- `redwood_drm_munmap` matched an address against a 4 GB window from each mapping's
  base and unmapped the wrong (live) buffer; now each mapping's own size. Also: sync files
  emulated in the bridge (a /dev/null descriptor standing for a sync object; panfrost's fences, so
  glFinish really waits -- "export failed" gone); kernel `mali_service()` polled from waits (tiler
  OOM answered with a new chunk or 0, CS faults printed + acked, CSG cs_irq acked), a 2 s stuck-job
  dump, a ring-room check in `mali_submit`; `gpu/crash.c` linked into test programs prints pc/lr/
  stack as image+offset on SIGSEGV. Frames of 1125x2436, shortest of 16, Mali vs softpipe:
  clear 497 us vs 9.8 ms; 1 full-screen rect 1.1 ms vs 152 ms; 4 rects 1.1 ms vs 645 ms;
  1 textured+blended 1.2 ms vs 823 ms; 4 such 1.6 ms vs 3.3 s.

- **Tiler heap fault fixed** (2026-10-24 04:50, test gltest6: no faults): the textured+blended
  frames raised `fault 0x270858` (0x58 DATA_INVALID_FAULT) at the start of the last chunk of the
  heap's list -- `tiler_heap_create` wrote `(0 & ~0xfff) | size >> 12` into that chunk's header,
  "next chunk at 0, 2 MB", where panthor leaves the end of the list all zero. Also now as panthor:
  kernel buffers of 2 MB or more on a 2 MB boundary. The faulting frames had been dropping work
  (flat ~1.6 ms); now textured+blended full-screen layers cost ~0.9 ms each: 1/2/4/8 layers =
  1.3/2.1/4.0/7.3 ms (softpipe: 4 layers 3.3 s).

- **Phase 5 — CoreAnimation on the Mali, zero copy** (2026-10-24 05:23, test gl3, boot-args
  `mali=3 gl=1 glwait=1`, image `root-gl.apfs` via `claude_gl.sh` = `REST=1 GL=1 build.sh` with
  `built-from-air` faked so build.sh does not rebuild Mesa for freedreno): reaches the home screen,
  "better for sure, still a little laggy, tearing as before". Kernel: request 0x7e as RedwoodGPU's
  (`surface_buffer()` in MaliGPU.cpp: MiniIOSurface → IOSubMemoryDescriptor → `mali_bo_adopt`),
  surfaces pushed out of the CPU cache at GROUP_SUBMIT (seed changed or every 4 ms). Mesa:
  osmesa_surface_resource for panfrost, import size from the kernel (no lseek), shared BOs synced
  as private ones, imported BO's syncobj created SIGNALED (else every submit waited 10 s: black
  screen, gl2). Copy path (gl1) was 27.5 ms/frame, 19.4 of it copying out; now 14.6 ms/frame:
  ~11.2 ms CPU in the window server (CA + Mesa, unsplit), 2.9 ms waiting for the GPU, 0.3 ms
  textures, 0.2 ms submit.

- **GPU at 572 MHz + the split** (2026-10-24 05:40, test gl4): xnuboot `clocks_up()` sets ACPM DVFS
  domains 5 and 6 (G3D_KHZ 572000; both read back 572000; Android's table: top 302-996, shaders
  202-848 MHz). eagl.c (WSL `userland/src/eagl.c`, original kept as `.orig`, edit script
  `claude_eagl.py`) adds "U us in the library's calls" to its frame lines. Frames: 9.0 ms average
  (was 14.6), of which 8.3 ms inside GL calls (1.2 ms of it waiting for the GPU, was 2.9), ~0.6 ms
  CoreAnimation between calls. Stuttering frames (>12 ms, 63 of 198): 17.5 ms, 135 draws, 15.3 ms
  inside GL calls. So ~100 us of CPU per draw inside the library (Mesa + eagl's own hand-written
  calls) -- far above what panfrost costs on Linux.

- **Slow calls** (gl5, boot-arg `glslow=1000`): glFlush 3283 calls over 1 ms averaging 7.4 ms;
  glTexImage2D 284 x 18 ms; glCompileShader 11 x 187 ms (cold compiles, no shader cache);
  glDrawElements 636 x 2.6 ms. gl6: surfaces now pushed out of the cache only when their seed moved
  (eagl locks the surfaces it drew into, so the display's still covered) -- glFlush unchanged
  (7.8 ms), so that was not it. The display (MiniIOMobileFramebuffer.cpp) copies each swap's
  surfaces into the boot loader's framebuffer with the CPU, "vsync" a timer: that is the tearing.

- **The stutters were logging** (gl7-gl9): kernel request timing (boot-arg `malistat=1`, a line
  per 4096 requests) showed the kernel's share of a submit is ~0.14 ms (cache flush of ~5 MB
  included); keeping buffer mappings in the bridge until GEM_CLOSE (panfrost unmaps on its BO cache
  put, and IOConnectMapMemory64 maps eagerly) changed nothing measurable. The slow glFlushes were
  eagl's own console lines -- a frame line every 20 frames (10-50 ms each) and a summary every 200
  (0.7-1 s) -- written to stderr, which on the Pixel is a slow console. Boot-arg `glquiet=1`
  (eagl.c, `claude_quiet.py`) keeps the first 40 frames' lines only. gl9: "feels smooth, still
  stutters, way more tearing"; calls over 5 ms after frame 40: glTexImage2D 27 x 22 ms,
  glDrawElements 25 x 21 ms (max 96), glFenceSync 10 x 18 ms, glFlush 2.

- **Tearing fixed** (2026-10-24 07:00, test gl12: "i dont see any tearing but still laggy"): boot-arg
  `decon=1` in MiniIOMobileFramebuffer.cpp (WSL `xnu/files/iokit/Kernel`, copied to the applied
  tree; original kept as `.orig`; scripts `claude_decon.py`, `claude_decon3.py`). The panel is
  command-mode: DECON0 (0x1c240000) sends the framebuffer on every TE pulse. Now: before a frame is
  drawn, wait until the previous frame's transfer has started (thread call masked the trigger
  again), mask HW_TRIG (TRIG_CON bit 4), wait GLOBAL_CON IDLE (bit 5); draw; then SHD_REG_UP_REQ
  GLOBAL + unmask, and a thread call polling every 1 ms masks again once SHD clears (the frame
  started) -- each frame sent once, never while being written. gl10 (unmasked between frames):
  less tearing, 11 ms/frame (DECON resent every pulse); gl11 (presenter slept until frame start):
  17 ms/frame.
- **Where the lag is now** (gl12): scrolling gets 39-44 frames a second; each spends 17-22 ms in the
  display driver: ~6 ms the driver's own timer "vsync" (`fb_beat`, now redundant), and ~9 ms the
  CPU scaling copy 1125x2436 -> 1440x3120 into the write-combined boot framebuffer (fb_threads=6:
  9.2 ms vs 3 threads 9.8 -- bandwidth-bound, ~18 MB written a frame).

- **fb_beat=0 is worse** (2026-10-24 12:40, test gl13; first try's USB staging failed, its log
  kept as `gl13-usbfail.txt`): "still laggy and more screen tearing than last time". So the beat
  is not just 6 ms wasted: holding each frame to it gave DECON0 time to finish the transfer before.
  `deconHold()` waits only for the last transfer to *start* (SHD_UP_GLOBAL cleared), then for
  GLOBAL_CON IDLE (bit 5) -- presumably IDLE is set before the transfer is really over. Keep fb_beat=1.
  **The log is lost**: it filled the whole 2 MB console zone (wrapped, start 448955), and the
  bootloader, appending its ~31 KB after the reset, does not cap the size: header size 2128717 >
  2097140, Android's ramoops "found existing invalid buffer" and zapped it. Earlier logs (<= 1.6 MB)
  never filled the zone.

- **Log ring fixed** (2026-10-24 14:00, `pixelboot/common/board.c`): our ring is `PRZ_RING`, 128 KB
  short of the zone, the rest spaces; a header that does not fit it (Android's own full log) starts
  a new one. gl14-16 logs came back (1.2-1.35 MB: none wrapped yet, so the wrap case is untested).
- **Scripted frame pacing** (no hands needed): image `downloads/ios/root-glfps.apfs` = `REST=1 GL=1
  RC_JOBS=rc.jobs.fps60 ./build.sh` (rc.jobs.fps with 60 s instead of 100 before the gestures:
  home-screen swipes, Search, Control Center, Settings fling, three times), session 270 s. Totals
  of the display driver's 5-second lines at 12+ frames a second: `python pixelboot/tools/fbstats.py downloads/probe/glN.txt ...`
  (regex over "N frames in M ms ... us each in the driver ..."). Old kernel kept as
  `downloads/kernel/mach.development.bcm2837.mali.gl12`, the gl14 driver source as `.cpp.gl14` in WSL.
- **gl14** (fb_beat=0, scripted): 24.4 frames/s, 20.0 ms each in the driver. DECON0's frame-done
  pending bit (INT_PEND 0x70 bit 13) never set: INT_EN is 0x3000 (sources on, interrupt bit 0 off,
  as the bootloader leaves it). Not pursued (would need the interrupt line on).
- **gl15** (fb_beat=1, scripted, `decon_trace=6`): 24.0 frames/s, 17.6 ms each -- the beat costs
  nothing measurable. The trace (6 frames, 40 ms each from the unmasking): the next TE pulse (every
  16.6 ms; HW_TE_CNT 0x38 counts them) starts the frame -- FRAME_COUNT +1, SHD_UP_GLOBAL and
  GLOBAL_CON's idle bit cleared at the same microsecond; the masker re-masks 0.1-0.9 ms later -- and
  **6856 us later every time** OF_LEVEL (0x2b0, output FIFO) is empty and GLOBAL_CON idle again. So
  the idle bit is right after all (fbtest's 7 samples all idle were chance), and gl13's "more
  tearing" is unexplained. The lag: the 9 ms scaling waits for the 6.9 ms send to end, 16 ms of a
  16.6 ms refresh, so any late frame misses its pulse and waits a whole one.
- **decon=2, chasing DECON0** (MiniIOMobileFramebuffer.cpp): the scaling starts once the frame before
  has started going out, and drawRows() writes row y only after masker-start + 300 us + 7200 us *
  (y+1)/3120 (DECON reads 2.2 us a row from the top; the masker sees the start late, so this errs
  safe). Stats line "decon=2: N frames scaled while the one before was going out". decon=1 unchanged.

- **gl16** (2026-10-24 14:36, decon=2 fb_beat=0, scripted): 27.4 frames/s (gl14/15: 24), 15.8 ms each
  in the driver (20.0/17.6), swaps held for the one before 30% (48/54%); frame-to-frame under 18 ms
  56% (42/46%), 26-35 ms -- a refresh missed -- 24% (44/46%). Chased frames' drawers waited ~15 ms
  together (6 drawers, ~2.6 ms each). No panic, no "not idle"/"no frame started". Not yet looked at
  by eye: tearing unknown.

- **gl17** (decon=2 fb_beat=0, by hand): "still stutters and screen tearing" -- **several breaks at
  once** (bands). Numbers better than gl12: 40.2 frames/s (36.5), under 18 ms 58% (42%), 26-35 ms
  17% (50%). (The user does not remember gl12 well: "no tearing" there is weak evidence.)
- **gl18** (decon=1 fb_beat=0 + counters, by hand): "more laggy but less tearing". 32.4/s, under
  18 ms 35%. Counters over 2946 frames: **0** frames with FRAME_COUNT changing during the copy, **0**
  with DECON0 idle while OF_LEVEL not 0 (decon=1 now also waits for the FIFO to stay empty 100 us).
  Trace: GLOBAL_CON goes 0x133 -> 0x113 during a send (the idle bit itself). So **DECON0 never reads
  while we write**: the tearing is not on the panel side. Six bands broken at once = the source
  surface changing while the six drawers copy it.
- **Surfaces marked in use** (MiniIOSurface.cpp `MiniIOSurfaceDisplayUse`, MiniIOMobileFramebuffer.cpp
  present()/presenter(); boot-arg fb_inuse=0: not): the display driver never touched IOSurface use
  counts, and CoreAnimation with the GL renderer cycles a small pool of surfaces (the "none twice"
  line never prints in gl12/gl18), so it may draw into one the driver is still copying. Now both
  counts +1 from SwapEnd until the copy is done. Counters per 5 s: "fb_inuse=N: X frames' surfaces
  drawn again after their swap before the copy, Y more by its end" (seed at SwapEnd vs copy start/end).
- **gl19** (decon=2 fb_beat=0 fb_inuse=1, by hand): nobody touched it -- idle 10 frames/s throughout,
  0 rewrites; tells nothing. No panic.

- **gl20** (2026-10-24 23:31, scripted + the user touching, decon=2 fb_beat=0 **fb_inuse=0**; two
  earlier tries failed at USB staging -- a bad cable, `fastboot: error: Command failed`): **the
  cause, confirmed**: of ~5200 frames, 1572 had their surface drawn again after SwapEnd and before
  the copy, **226 more during the copy** (copied half old, half new: the bands). 0 frames started by
  DECON0 during a copy. 29.9 frames/s, under 18 ms 56%. User: "less laggy but a little more tearing".

- **gl21** (scripted, decon=2 fb_beat=0 fb_inuse=1, use counts only): **no help** -- 1011 rewritten
  before the copy, 180 during; 29.2/s. CoreAnimation draws **every frame into the same surface**
  (swaps 1, 2, 6, 8 all surface 1; only 5 in surface 2) as soon as SwapEnd returns, whatever the use
  count says (IOSurfaceIsInUse has no kernel call: userland reads the shared page, so it saw it).
  SwapWait (selector 6) still returns at once here ("on the screen as soon as it is made").
- **GPU work waits for shown surfaces** (gpu/redwood_mali_uapi.c `surfaces_shown_wait()` at the
  start of group_submit, polling 100 us with the lock let go, 40 ms at most; MiniIOSurface
  `displayUse` + `MiniIOSurfaceShown`, bridged by MaliGPU.cpp `maligpu_surface_shown`; only with
  fb_inuse=1). A submission does not name its buffers, so any surface BO of the client counts.
- **gl22** (2026-10-24 23:55, scripted, decon=2 fb_beat=0 fb_inuse=1 + GPU wait): **0 rewritten
  before, 0 during** -- the tearing's cause gone. 27.5/s (gl20 29.9), under 18 ms 52% (56%), 26-35
  ms 27% (22%), 12.6 ms each in the driver, swaps held 1% (CoreAnimation now waits on the GPU, the
  GPU on the display). GPU waits: 1953, 9.3 ms each, 30 gave up at 40 ms. (USB dropped as Android
  came back: log pulled by hand with `adb exec-out "su -c 'cat /sys/fs/pstore/console-ramoops-0'"`.)

- **A mailbox on one buffer would starve the panel** (thought through, not built): the trigger
  must stay masked while the new frame is scaled in (9 of every ~12 ms), so most TE pulses would
  find it masked. Hence two buffers.
- **decon=3, two framebuffers** (MiniIOMobileFramebuffer.cpp `deconSetup3` etc.): DPP channel 0's
  DMA at 0x1c0b0000 (cal_9845 regs-dpp.h, Google's display driver: RDMA_BASEADDR_Y8 0x40, its
  shadow 0x440 = what the frame under way reads, RDMA_IRQ 0x04 with error bits). Second buffer
  0xfbe00000 (free RAM after the boot framebuffer, nothing reserves it), mapped with
  IOMemoryDescriptor::withPhysicalAddress + map(kIOMapWriteCombineCache) -- **not ml_io_map_wcomb**:
  its submap has no room for 17 MB and kmem_alloc panics (gl23: "kmem_alloc(..., 17973248) failed
  with 3"). Each frame is scaled at once into the buffer not being sent (waits only while a frame
  from it is still going out), then RDMA_BASEADDR_Y8 set + SHD_UP_GLOBAL|windows (0x3f) + unmask;
  the trigger is never masked for the scaling; a frame let go and not sent yet is replaced. Falls
  back to decon=2 on DMA error bits or flips not taken. `gDecon` is now TUNABLE_WRITEABLE (a plain
  TUNABLE is SECURITY_READ_ONLY_LATE).
- **gl24** (2026-10-25 00:28, scripted, decon=3 fb_beat=0 fb_inuse=1): **flips take** (shadow follows
  base every time, 16 checked, 0 missed), 0 DMA errors (RDMA_IRQ only 0x10000, frame done), 5015
  flips, 120 frames waited for the other buffer. 0 rewrites. But 26.4 frames/s (gl22 27.5, gl20
  29.9): 9.9 ms each in the driver (12.6), swaps held 0%, 26-35 ms 15% (27%), 18-26 ms 30%; GPU
  waits 1217 x 5.6 ms (1953 x 9.3). **The display is no longer what limits it: CoreAnimation makes
  ~26 frames a second on this script** (its CPU time in the window server + Mesa).

- **The user watched gl24**: "looked smooth, there were still stutter". Goal now: **stable 45 fps
  at least, 60 preferred, 120 if reasonable**.
- **Only eagl's last GPU copy waits for the display now** (gl25): eagl.c `hold_for_display`
  (glhold=0: not) -- before the frame's blit into its IOSurface, glFlush, then IOSurfaceLock(0) +
  Unlock, and MiniIOSurface.cpp trapLock makes a write lock wait (100 us steps, 40 ms at most)
  while `displayUse` != 0. The kernel's GPU-wide wait is now boot-arg `mali_holdall=1` (off). gl25:
  26.5/s, but **under 18 ms 67%** (gl24 51%), 0 rewrites during copies; 23 lock waits, 9 hit 40 ms
  (gl26 log: "surface 1/2 ... the display still has it 1 times" -- a swap waiting long; open).
- **eagl's five-second line** (`glstat`, default on; written by a thread of its own): frames, us
  apart, over 20/34 ms, us from a frame's first GL call to its end, the drawing thread's CPU time
  (thread_info), in-GL / GPU-wait / display-wait per frame, top 5 GL functions by time:
  `python pixelboot/tools/glstat.py downloads/probe/glN.txt`.
- **gl26** (2026-10-25 01:00, scripted, decon=3): over 5413 busy frames **24.1 ms apart; the window
  server's drawing 8.5 ms a frame (its CPU time about the same), 7.4 ms of it in GL calls, GPU wait
  1.3 ms**. So the drawing thread **idles ~60%**: it waits for work (SpringBoard's commits / its
  clock). One window of home-screen swipes: **262 frames 16.7 ms apart, 7 over 20 ms -- a steady
  60** (at 1.43 GHz). Heavy screens (Settings, Control Center): 10-19 ms drawing, frames 20-36 ms
  apart. In GL: glDrawElements 30-60 us a draw (70-160 draws a frame; Mesa is a release build, so
  not that), glBindTexture ~10 us a call (eagl's surface_changed(): a checksum of every copied
  IOSurface texture once a frame -- 26 of 30 are copied: RGBA icons, not BGRA, so not shared),
  glTexImage2D 6-10 ms a call (spikes), glFlush 1.1-2.7 ms a frame (the target surface pushed out
  of the CPU cache at submit, seed bumped by eagl's locks, maybe twice a frame).
- **Thermal throttling in every run**: the governor (pixelboot/common/el2.c: down a step at 80 C,
  up only below 70 C, top step GOV_TOP 2 = 2401/1999 MHz) reaches 80 C during the start-up and the
  first gestures and steps down to **1106/1024 MHz** by the end (gl24-26), never back up. The CPU
  scaling copy (17 MB a frame on 6 cores) is the obvious heat.

- **GPU scaling into the framebuffers** (boot-arg `glscale=1` with decon=3; opt-in for now):
  - MaliGPU.cpp `framebuffer_buffer`: request 0x7e answers the IOSurface IDs 0x7fff0000/0x7fff0001 with
    framebuffer 0/1 (MiniIOMobileFramebufferGPUBuffer: IOMemoryDescriptor::withPhysicalAddress + a WC
    map, kvtophys checked), 1440x3120, 5760 a row, 'BGRA'; adopted by `mali_bo_adopt_id` (deduped by ID).
  - MiniIOMobileFramebuffer.cpp: user-client selectors 100 (gpuFrameBegin(surface): waits until the
    GPU frame before is shown and no copy is under way, returns gFBShown^1 once not being sent; a
    second flush of the same surface gets the same buffer) and 101 (gpuFrameDone(k, surface));
    show() takes it (gpuFrameTake) for a one-layer full-display swap of that surface and only flips.
  - eagl.c `framebuffer_for` / `frame_to_framebuffer`: opens the IOMobileFramebuffer service itself,
    imports the framebuffer as a texture (redwood_gl_surface_texture with the fake ID), blits
    image->texture -> framebuffer GL_LINEAR (scissor off), glFinish, then 101. The blit into the
    window server's own surface stays (for other paths).
  - **gl27 crashed backboardd 7 times** (SIGSEGV at 0x30 on its first scaled frame; after that,
    everything crawled -- see GPU contexts below). Crash handler (userland/src/inject.c) now also
    prints library+offset (dladdr); symbolized with theos `llvm-symbolizer --obj=` Mesa's unstripped
    `build-softpipe-panfrost/src/gallium/targets/osmesa/libOSMesa.8.dylib`: gl28 =
    `panthor_kmod_bo_get_sync_point <- csf_submit_batch_v10`. Cause: Mesa asks for an imported
    buffer's handle twice (panfrost_bo_import, then pan_kmod_bo_import) and needs the same one;
    framebuffer imports weren't deduped (no surface pointer), so the second made a second buffer and
    Mesa's record of the first stayed unmade. Fixed with `import_id` + mali_bo_adopt_id.
  - **gl29** (2026-10-25 01:58, scripted, decon=3 glscale=1): **0 crashes; nearly every frame drawn by
    the GPU** (up to 285 a 5-s window = 57/s); **0.5 ms a frame in the display driver (was 9.9)**;
    33.2 frames/s overall (gl26 30.1), under 18 ms 75% (69%), longer than 35 ms 2% (4%). Window
    server: 9.3 ms drawing a frame (8.5), GPU wait 1.8 ms (1.3). Thermals not comparable: the phone
    started at 80 C (three runs back to back) and throttled to 1106 MHz as before. **The user watched
    gl29: "it was smooth".**
- **GPU contexts leak on a client's exit**: mali_client_close frees buffers and syncobjs, but "its VMs
  and groups stay (the core cannot take them back yet)". Each backboardd crash costs one of the
  firmware's 8 CSG slots for good (and their queues may still be scheduled): gl27 ran at 40-66 ms a
  frame after its crashes. Needs GROUP_DESTROY / CSG terminate in redwood_mali_core.c.

- **The default now** (2026-10-25 02:05): `bash pixelboot/tools/ios-run.sh NAME [SECS]` with nothing
  else boots the GPU setup -- KERNEL downloads/kernel/mach.development.bcm2837.mali, RAMDISK
  downloads/ios/root-gl.apfs, boot-args `mali=3 gl=1 glwait=1 glquiet=1 decon=3 fb_threads=6 fb_beat=0
  fb_inuse=1`; eagl scales into the framebuffers by itself with decon=3 (`glscale=0`: not). `XARGS=...`
  replaces the boot-args, `CPU=1` boots the old way (prebuilt kernel, root.apfs, the processor draws).
  Scripted: `RAMDISK=downloads/ios/root-glfps.apfs bash pixelboot/tools/ios-run.sh NAME 270`.

- **The look** (2026-10-25, the user: "working"): no widgets on the home screen or the Today page,
  dark mode. hackios/userland/var/ (copied into /var at every boot; build.sh now adds the folders
  iOS's template lacks): mobile/Library/SpringBoard/IconState.plist (the default without widgets,
  today []), mobile/Library/Preferences/com.apple.uikitservices.userInterfaceStyleMode.plist
  (UserInterfaceStyleMode 2). The Today page also needed the default emptied in the system image
  (ios165/rootfs/.../SpringBoard.app/DefaultIconState~iphone.plist, .orig kept). The wallpaper is
  black because PosterBoard draws nothing here; the user's picture sits in the default wallpaper's
  Background layer (Collections/iOS_16~iphone/7400.WWDC_2022-375w-812h@3x, .orig kept) for when it
  does. downloads/ios/system.apfs is now the rebuilt image (the old one: system-orig.apfs); it
  carries HackiOS's RedwoodTriangle.app (a Metal test, no use on the Mali) -- leave it out at the
  next system rebuild.

## Next
- One scripted run from a **cool** phone, for the thermals against gl26 (gl29 started at 80 C).
- **GPU contexts on a client's exit** (see above): GROUP_DESTROY / CSG terminate, so a window-server
  crash does not cost a CSG slot for good.
- Stutters: copied IOSurface textures checksummed every frame (eagl surface_changed; RGBA icons not
  shared), glTexImage2D 6-10 ms a call, first-draw shader compiles (no cache).
- Skip the blit into the window server's own surface when the frame went to the framebuffer, if
  nothing else needs it (GPU time, and the display hold).
- Cheaper, smaller: copied textures checksummed every few frames instead of every frame; one surface
  cache flush a frame, not two; why glTexImage2D takes ms (BO allocation per upload?).
- The governor's limits (80/70 C, GOV_TOP) only after the heat is down, and carefully.
- If decon=2 + fb_inuse=1 holds (fast, no tearing): make it the default. Then the 9 ms copy (GPU scaling).
- After decon=2, what's left per frame is the wait for the frame before to start (TE phase, avg ~8 ms
  when frames come back to back) plus the 9 ms scaling. Only GPU scaling (or double buffering via the
  DPP's DMA address) takes the copy out.
- **The 9 ms copy**: have the GPU do the scaling (a BO over the boot framebuffer's physical memory
  and a blit job from the CA surface), or the display hardware (DPP scaler / scan out the surface
  directly; needs DPP registers we don't have). fb_filter=0 (nearest) would tell compute vs bandwidth.
- **Remaining stalls**: uploads (try `glenv=PAN_MESA_DEBUG=noafbc`, no rebuild), first-draw shader
  variants (a shader cache), glFenceSync.
- Make `decon=2 fb_beat=0 glquiet=1` (decon=1 if decon=2 tears; and `mali=3 gl=1 glwait=1`) xnuboot's default boot-args once
  settled.
- Then: stop waiting for the GPU every frame (display waits for the GPU's fence instead), and
  tearing (display path, not GPU: present before too).
- **Phase 3 — the kernel driver.** Needs the HackiOS kernel built here (`xnu/apply.sh &&
  xnu-deps/setup.sh && ./xnu-build.sh`, in WSL `~/hackios`). Then a `MaliGPU` IOKit service
  shaped like HackiOS's `RedwoodGPU` (iokit/Kernel/RedwoodGPU.cpp + bsd/dev/redwood_adreno.c):
  wake + firmware boot (phases 1–2, moved into the kernel), then Linux panthor's interface
  (`include/uapi/drm/panthor_drm.h`) as user-client selectors: DEV_QUERY, VM_CREATE/BIND,
  BO_CREATE/MMAP_OFFSET, GROUP_CREATE/SUBMIT, TILER_HEAP_CREATE, plus the DRM syncobj calls
  panfrost waits with; and RedwoodGPU's two extras (a buffer that is an IOSurface's memory,
  showing a surface). Reference: mainline `drivers/gpu/drm/panthor` (WSL `~/gpu-ref`).
- **Phase 4 — Mesa.** `tools/mk-mesa.sh` with `DRIVERS=panfrost` instead of freedreno, and a
  panthor stand-in for libdrm next to `tools/mesa/redwood-drm`.
- **Phase 5 — CoreAnimation on the GPU** through `userland/src/eagl.c` (boot-arg `gpu=1`), then
  debugging until the home screen draws on it.

## Rules of thumb
- Each session ends with something that boots and says yes or no in the log.
- Probes run without iOS: `bash pixelboot/build.sh NAME && bash pixelboot/tools/run.sh
  pixelboot/out/NAME-boot.img LOGNAME` (log in `downloads/probe/LOGNAME.txt`).
