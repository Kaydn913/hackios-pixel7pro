#!/usr/bin/env python3
# fbstats.py LOG...: frames a second and ms per frame in the display driver, over its 5-second lines
# while frames came (12 a second or more), and the spread of frame-to-frame times -- for comparing
# scripted runs (root-glfps.apfs).
import re, sys
pat = re.compile(rb'MiniIOMobileFramebuffer: (\d+) frames in (\d+) ms \(\d+ a second\), (\d+) us each in the driver, '
                 rb'after (\d+) us waiting for the GPU and, (\d+) of them, (\d+) us for the next refresh; (\d+) swaps waited')
for path in sys.argv[1:]:
    rows = [tuple(map(int, m.groups())) for m in pat.finditer(open(path, 'rb').read())]
    act = [r for r in rows if r[0] * 1000 / r[1] >= 12]
    fr = sum(r[0] for r in act)
    ms = sum(r[1] for r in act)
    drv = sum(r[0] * r[2] for r in act) / fr / 1000
    beat = sum(r[4] for r in act)
    bw = sum(r[4] * r[5] for r in act) / beat / 1000 if beat else 0
    held = sum(r[6] for r in act)
    print(f'{path}: {len(act)} windows, {fr} frames in {ms / 1000:.1f} s = {fr * 1000 / ms:.1f}/s; '
          f'{drv:.1f} ms each in the driver; {beat * 100 / fr:.0f}% waited for the beat, {bw:.1f} ms each; '
          f'{held * 100 / fr:.0f}% of swaps held for the frame before')
    # ... and how evenly: frame-to-frame times over the windows of 60+ (reportFrameTimes' lines)
    spread = re.compile(rb'from one frame to the next: (\d+) times under 10 ms, (\d+) under 18, (\d+) under 26, (\d+) under 35, (\d+) longer')
    tot = [0] * 5
    for m in spread.finditer(open(path, 'rb').read()):
        v = list(map(int, m.groups()))
        if sum(v) >= 60:
            tot = [a + b for a, b in zip(tot, v)]
    n = sum(tot) or 1
    print(f'  {sum(tot)} frame-to-frame times: under 18 ms {100 * (tot[0] + tot[1]) / n:.0f}%, 18-26 {100 * tot[2] / n:.0f}%, '
          f'26-35 {100 * tot[3] / n:.0f}%, longer {100 * tot[4] / n:.0f}%')
