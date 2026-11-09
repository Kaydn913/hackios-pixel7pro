# hackios-overlay: this port's changes to HackiOS itself

These are the files this port changed **inside** HackiOS's own trees (its XNU overlay and its
userland). Copy them into a HackiOS checkout with [`apply.sh`](apply.sh). Wi-Fi has its own script,
[`../wifi/install-kernel.sh`](../wifi/install-kernel.sh).

| File | What changed |
|---|---|
| `xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp` | The Pixel's DECON0 display path: TE-synced triggers (`decon=1`), copy behind the scan-out (`decon=2`), two framebuffers flipped by DPP0's DMA address (`decon=3`), GPU frames into the back buffer (`kFBMethodGPUFrameBegin/Done`, for eagl.c's `glscale`), `fb_inuse`, `fb_threads` |
| `xnu/files/iokit/Kernel/MiniIOSurface.cpp`, `.h` | Surfaces shared between CoreAnimation, the GPU and the display (client memory held for the GPU) |
| `userland/src/eagl.c` | CoreAnimation on Mesa's panfrost: zero-copy surfaces, `glscale` (GPU scaling into the framebuffer), `glstat` frame timing |
| `userland/src/inject.c` | The window server's OpenGL ES hooks that go with eagl.c |
| `userland/src/inject_sb.c` | SpringBoard: the custom wallpaper layer (`/var/mobile/Library/SpringBoard/wallpaper.jpg`, boot-arg `wallpaper=0` to turn it off) |
| `userland/build.sh` | `fw-gpu/` (Mali firmware) into the image, `d` lines for `var/` folders |
| `userland/fix-mkjobs.py` | Fixes `mkjobs.py`, which wrote every job of the REST set over one file, so configd and the other on-demand daemons never ran |
| `userland/home-screen.py` | The home screen without widgets, and dark mode (files for `userland/var/`) |

## RECOVERED: read this first

The original copies of the six files under `xnu/` and `userland/` (except the two `.py` files) were on
a WSL disk that was deleted in November 2026. They were rebuilt from the AI coding assistant's own backups
of each file, taken while the port was being written, using each file's newest backup:

| File | Newest backup |
|---|---|
| `MiniIOMobileFramebuffer.cpp` | 2026-10-25 06:27 UTC |
| `MiniIOSurface.cpp` | 2026-10-25 06:04 UTC |
| `MiniIOSurface.h` | 2026-10-25 04:58 UTC |
| `eagl.c` | 2026-10-25 07:11 UTC |
| `inject.c` | 2026-10-25 07:00 UTC |
| `inject_sb.c` | 2026-10-25 17:39 UTC |
| `build.sh` | 2026-10-25 07:33 UTC |

The method was checked: replaying the edits logged between one backup and the next reproduces the
next backup exactly. No edits to these files were logged after their newest backups, and these are
the versions the GPU kernel and images of 2026-10-25 were built from. Changes made later by scripts
are applied by scripts here too:

- `wifi/build-image.sh` adds `build.sh`'s Wi-Fi firmware block.
- `fix-mkjobs.py` fixes `mkjobs.py`.

**They have not been compiled since the rebuild**, because the build environment was on the same disk.
`apply.sh` was also rebuilt, from the scripts the port's own builds used. It targets HackiOS as of
2026-10-23 (`xnu-8796.121.2`). Upstream has moved files since then, and the script stops and says so
if it can't find what it edits.
