#!/usr/bin/env python3
# glstat.py LOG...: eagl's five-second lines (boot-arg glstat, on by default), the busy windows (60+ frames)
# one a row and their totals: how far apart frames came, the window server's time drawing them and its CPU time,
# time in GL calls, waiting for the GPU and for the display, and the GL functions that took the most.
import re, sys
pat = re.compile(rb'redwood-gl: (\d+) frames in (\d+) ms; (\d+) us apart, the longest (\d+), (\d+) over 20 ms and (\d+) over 34; '
                 rb'a frame (\d+) us from its first call to its end, this thread (\d+) us busy; (\d+) us in the library.s calls '
                 rb'\((\d+) waiting for the GPU, (\d+) for the display\); most:([^\n]*)')
for path in sys.argv[1:]:
    rows = [m for m in pat.finditer(open(path, 'rb').read())]
    busy = [m for m in rows if int(m.group(1)) >= 60]
    print(f'{path}: {len(busy)} busy windows')
    print('  frames  apart  >20  >34 | drawing   cpu  in-GL  gpu  disp | top')
    tot = [0] * 11
    for m in busy:
        g = [int(m.group(i)) for i in range(1, 12)]
        f = g[0]
        tot = [a + b * (f if i >= 2 else 1) for i, (a, b) in enumerate(zip(tot, g))]
        print(f'  {g[0]:6} {g[2]:6} {g[4]:4} {g[5]:4} | {g[6]:7} {g[7]:5} {g[8]:6} {g[9]:4} {g[10]:5} |{m.group(12).decode()[:110]}')
    if busy:
        f = tot[0]
        print(f'  all {f} frames: {tot[2] / f:.0f} us apart; drawing {tot[6] / f:.0f}, cpu {tot[7] / f:.0f}, in GL {tot[8] / f:.0f}, '
              f'GPU wait {tot[9] / f:.0f}, display wait {tot[10] / f:.0f} us a frame; >20 ms {tot[4] / f:.1f}% >34 {tot[5] / f:.1f}% (of frames)')
