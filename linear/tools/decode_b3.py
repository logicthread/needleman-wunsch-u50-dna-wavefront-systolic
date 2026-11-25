#!/usr/bin/env python3
"""Test a raw dir dump against the BASE-3 layout (kernel_tile_1b with WV_DIR_B3, SG=1 at
tile_pe=128): cmax = ceil((n+tpe)/5)*5, bpb = cmax/5*2, beat = band*bpb + (c//5)*2 + lane//64,
byte = lane%64, digit k = c%5 of byte = d0*81+d1*27+d2*9+d3*3+d4 (first cycle most significant)."""
import sys, os, numpy as np
d = sys.argv[1]
meta = dict(l.split() for l in open(os.path.join(d, 'dump_meta.txt')))
m, n, tpe, tiles = (int(meta[k]) for k in ('m', 'n', 'tile_pe', 'tiles'))
q = np.fromfile(os.path.join(d, 'q.bin'), np.uint8)[:m]; r = np.fromfile(os.path.join(d, 'r.bin'), np.uint8)[:n]
dirs = [np.fromfile(os.path.join(d, 'dir_%02d.bin' % k), np.uint8) for k in range(tiles)]
allb = np.concatenate(dirs); print("bytes >242:", int((allb > 242).sum()), "of", allb.size, " (0xFF fill =", int((allb == 255).sum()), ")")
H = np.zeros((m + 1, n + 1), np.int64); H[0, :] = np.arange(n + 1) * -2; H[:, 0] = np.arange(m + 1) * -2
s = np.where(q[:, None] == r[None, :], 2, -1)
for i in range(1, m + 1):
    row = np.maximum(H[i - 1, :-1] + s[i - 1], H[i - 1, 1:] - 2); h = H[i]
    for j in range(1, n + 1):
        v = row[j - 1]; l = h[j - 1] - 2; h[j] = v if v > l else l
okD = H[1:, 1:] == H[:-1, :-1] + s; okU = H[1:, 1:] == H[:-1, 1:] - 2; okL = H[1:, 1:] == H[1:, :-1] - 2
cmax = -(-(n + tpe) // 5) * 5; bpb = cmax // 5 * 2
print(f"b3 bpb={bpb} (2-bit host assumed {int(meta['bpb'])})")
P3 = np.array([81, 27, 9, 3, 1])
rates = []
for k in range(tiles):
    tot = cnt = 0
    for lane in range(tpe):
        i = k * tpe + lane
        if i >= m: break
        c = np.arange(n) + lane
        beat = (c // 5) * 2 + lane // 64; byte = dirs[k][beat * 64 + lane % 64].astype(int)
        code = (byte // P3[c % 5]) % 3
        v = np.where(code == 0, okD[i], np.where(code == 1, okU[i], okL[i]))
        tot += v.sum(); cnt += v.size
    rates.append(tot / cnt)
print("per-port valid rate (base-3 layout):", " ".join(f"{x:.3f}" for x in rates))
print(f"overall {np.mean(rates):.4f}")
