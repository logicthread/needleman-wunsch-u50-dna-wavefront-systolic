#!/usr/bin/env python3
"""MEASURED ANCHOR (gate dd368b3d, 2026-10-19): 3968 PE base-3 @199, m=182,528 n=181,019:
kernel 124.4 ms (265.5 GCUPS), traceback 39 ms, end-to-end 163.6 ms = 202 GCUPS. S0 below
reproduces the kernel time within 0.4%.

End-to-end time and GCUPS for ONE global alignment of m = n = 181,019 bp WITH traceback.
Every input is tagged: M = measured on the U50 (gate runs 2026-10-19), X = extrapolated from
measurements, P = projected (not built). GCUPS_e2e = m*n / (kernel + traceback + launch)."""
m = n = 181019
cells = m * n
LAUNCH_MS = 0.10                       # M: 2048 single-band intercept
# --- M: 3968 PE base-3 (kernel_tile_1b) @ 199 MHz, gate 225dd517 + e96b954e ---------------
T1_262k, T4_262k, N_MEAS, PE0, F0 = 1.669, 13.443, 262144, 3968, 199e6
extra_band_262k = (T4_262k - T1_262k) / 3          # 3.925 ms per additional band
TB_MS_181 = 39.0                                   # M: 181 kbp traceback, 1426 fetches (gate dd368b3d)
def tb_ms(): return TB_MS_181
rows = []
def add(name, tag, kern_ms, tb, pe, mhz, note):
    tot = kern_ms + tb + LAUNCH_MS
    rows.append((name, tag, pe, mhz, kern_ms, tb, tot, cells / (kern_ms * 1e6), cells / (tot * 1e6), note))
# S0: the current best design, as built
bands = -(-m // PE0); s = (n + PE0) / (N_MEAS + PE0)
k0 = T1_262k * s + (bands - 1) * extra_band_262k * s
add("S0 current: 3968 PE base-3 (kernel_tile_1b)", "X", k0, tb_ms(), PE0, 199,
    f"{bands} bands; per extra band {extra_band_262k*s:.2f} ms (measured 4-band slope, scaled to n)")
# S1: same kernel, boundary kept on chip packed 8/word: reload n -> n/8, store n/8 -> n/8 (copy)
eff = T1_262k / ((N_MEAS + PE0 + 256) / F0 * 1e3 + 0.04)    # measured/ideal single band = 1.13
band_ms = ((n + PE0 + 256) + n / 32) / F0 * 1e3 * eff        # as built: copy (n+31)/32 words
add("S1 same + on-chip packed boundary copy", "P", band_ms * bands, tb_ms(), PE0, 199,
    f"per band {band_ms:.2f} ms = sweep + n/32 copy, x{eff:.2f} measured overhead")
# S1 AS BUILT AND MEASURED (gate 09bbbe4e, 2026-10-21): link 108331, S1 + seam relays + pblock
# floorplan, auto-scaled 300 -> 246.5 MHz. Kernel 37.607 ms, traceback 31.9 ms, e2e 474.8 GCUPS.
add("S1 MEASURED: on-chip boundary, 3968 PE @246.5", "M", 37.607, 31.9, PE0, 246.5,
    "gate 09bbbe4e: score + optimal path PASS; 1 rep")
_b = ((n + PE0 + 256) + n / 32) / 246.5e6 * 1e3 * eff
add("S1 model re-run at the delivered 246.5 MHz", "X", _b * bands, tb_ms(), PE0, 246.5,
    f"model {_b*bands:.1f} ms vs measured 37.6 -> model is conservative")
# (S1 + parallel traceback fetch was MEASURED WORSE: 422.7 ms vs 39 ms -- gate fc8b511d.
#  Host traceback cost = syncs x ~27 us; syncs = bands x direction buffers. Removed.)
# S2: packed 2-bit PE ring, FULL base-3 direction storage (traceback from stored dirs), HBM-bound
HBM = 31 * 8.1e9                                   # M: ~8.1 GB/s sustained per pseudo-channel
cap = HBM * 8 / 1.6                                # cells/s the direction stream allows
pe2, f2 = 7936, 158e6                              # 256 PE per channel at the per-channel limit
rate = min(pe2 * f2, cap) * 0.90                   # 10% margin on HBM efficiency
k2 = cells / rate * 1e3 + pe2 / f2 * 1e3           # + one fill/drain (ring: no per-band drain)
add("S2 packed ring, full base-3 dirs, 31 HBM ch", "P", k2, tb_ms(), pe2, 158,
    f"HBM caps at {cap/1e12:.2f} TCUPS; 90% of it; 211 MB/ch of 256")
# S3: packed ring across both SLRs, checkpointed traceback K=R=32, host recomputes blocks
pe3, f3, K = 16384, 150e6, 32
k3 = cells / (pe3 * f3) * 1e3 + pe3 / f3 * 1e3
rec_cells = K * (m + n)                            # blocks of K x K along the path
for thr, gc in ((1, 0.5e9), (8, 4e9)):
    add(f"S3 both-SLR ring, checkpoint K=R=32, host recompute x{thr} thr", "P", k3,
        rec_cells / gc * 1e3 + tb_ms(), pe3, 150,
        f"checkpoint store {cells*1.6*2/K/8/1e9:.2f} GB; recompute {rec_cells/1e6:.0f} M cells @ {gc/1e9:.1f} GCUPS")
print(f"workload: m = n = {m:,}  -> {cells:.3e} cells, global alignment WITH traceback\n")
print(f"{'design':48s} {'tag':3s} {'PE':>6s} {'MHz':>4s} {'kernel ms':>9s} {'tb ms':>6s} {'total ms':>8s} {'GCUPS kern':>10s} {'GCUPS e2e':>9s}")
for r in rows:
    print(f"{r[0]:48s} {r[1]:3s} {r[2]:6d} {r[3]:5.1f} {r[4]:9.1f} {r[5]:6.1f} {r[6]:8.1f} {r[7]:10.0f} {r[8]:9.0f}")
    print(f"{'':52s}{r[9]}")
