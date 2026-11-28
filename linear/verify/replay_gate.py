#!/usr/bin/env python3
"""Replay of the two workloads of the silicon run (results/linear_s1t_gate_35a63b96/stdout.txt) on a laptop.

nw_gate_host.cpp builds its sequences from an xorshift32 generator (host/nw_gate_host.cpp:181-182,207-210):
seed S, then q[i] = rnd() & 3 for i < m, then r[j] = rnd() & 3 for j < n; check phase seed 12345,
perf phase seed 12346 (--seed 12345, +1). This script regenerates exactly those sequences, runs the repo
models at NW_PE=3968 and compares them with the numbers the CARD printed:

  check  m=7936 n=20000   gold -9960; card kernel score; card walker: 20002 ops, 124 fills, path score
  perf   m=182528 n=181019 gold 81778; card kernel score; card walker: 198650 ops, 1426 fills

For perf the per-cell direction matrix would be 33 GB, so only scores are replayed there (the model of the
PE array and the C golden); nops/fills need the check phase's full path.   usage: replay_gate.py [--phase check|perf|both] [--bio] [--bio-perf] [--kernel]
"""
import os, re, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
STDOUT = os.path.join(ROOT, "results", "linear_s1t_gate_35a63b96", "stdout.txt")
PE = 3968


def xorshift_seq(seed, count):
    rs = seed if seed else 1
    out = bytearray(count)
    for i in range(count):
        rs ^= (rs << 13) & 0xFFFFFFFF
        rs ^= rs >> 17
        rs ^= (rs << 5) & 0xFFFFFFFF
        out[i] = rs & 3
    return out


def silicon():
    t = open(STDOUT).read()
    res = {}
    for ph in ("check", "perf"):
        d = {}
        d["m"], d["n"] = map(int, re.search(rf"\[{ph}\] m=(\d+) n=(\d+)", t).groups())
        d["gold"] = int(re.search(rf"\[{ph}\] CPU reference score (-?\d+)", t).group(1))
        d["kernel"] = int(re.search(rf"\[{ph}\] kernel score (-?\d+) == CPU", t).group(1))
        mo = re.search(rf"\[{ph}\] ON-CARD traceback: status (\d+), (\d+) ops, (\d+) fills, card path score (-?\d+)", t)
        d["status"], d["nops"], d["fills"], d["path"] = map(int, mo.groups())
        res[ph] = d
    return res


def run_kernel(phase, qs, rs, sil):
    """The REAL kernel source (kernel_tile_1b_s1t.cpp, NW_PE=3968, 31 tiles, seam relays) on the laptop stand-ins."""
    m, n = len(qs), len(rs)
    with tempfile.TemporaryDirectory() as w:
        cf = os.path.join(w, "case.txt"); b = os.path.join(w, "kd")
        open(cf, "w").write(f"{phase} R {m} {n} {qs} {rs}\n")
        env = dict(os.environ, DIRDEPTH="65536", QDEPTH="1024", RDEPTH="1024")
        subprocess.check_call([os.path.join(HERE, "build_kernel.sh"), str(PE), b], env=env)
        t0 = time.time(); out = subprocess.check_output([b, cf], text=True); dt = time.time() - t0
    line = next(l for l in out.splitlines() if l.startswith("CASE")).split()
    got = dict(kernel=int(line[6]), status=int(line[10]), nops=int(line[11]), path=int(line[12]), fills=int(line[13]),
               hn=int(line[9]), diff=int(line[14]))
    print(f"[{phase}] REAL KERNEL (hls stand-ins) ran in {dt:.0f} s")
    ok = True
    for what, s_, g in [("kernel score mem[S_OFF]", sil["kernel"], got["kernel"]), ("on-card walker status", sil["status"], got["status"]),
                        ("on-card walker ops", sil["nops"], got["nops"]), ("on-card walker cache fills", sil["fills"], got["fills"]),
                        ("on-card path score", sil["path"], got["path"])]:
        good = s_ == g; ok &= good
        print(f"  {'OK  ' if good else 'DIFF'} {what:<34} silicon {s_:>8}   replay {g:>8}   [kernel_tile_1b_s1t.cpp via stand-ins]")
    good = got["diff"] == -1 and got["nops"] == got["hn"]; ok &= good
    print(f"  {'OK  ' if good else 'DIFF'} kernel's on-card ops == host walker over the kernel's own beats (first diff {got['diff']})")
    return ok


def run(phase, seed, sil, bio, kernel=False):
    m, n = sil["m"], sil["n"]
    q = xorshift_seq(seed, m + n)
    qs = "".join("ACGT"[x] for x in q[:m]); rs = "".join("ACGT"[x] for x in q[m:])
    with tempfile.TemporaryDirectory() as w:
        cf = os.path.join(w, "case.txt"); b = os.path.join(w, "dut")
        open(cf, "w").write(f"{phase} R {m} {n} {qs} {rs}\n")
        subprocess.check_call([os.path.join(HERE, "build_dut.sh"), str(PE), b])
        t0 = time.time()
        out = subprocess.check_output([b, cf], text=True)
        dt = time.time() - t0
    line = next(l for l in out.splitlines() if l.startswith("CASE")).split()
    got = dict(gold=int(line[5]), wave=int(line[6]), status=int(line[10]), nops=int(line[11]),
               path=int(line[12]), fills=int(line[13]))
    ops = next((l.split()[1] for l in out.splitlines() if l.startswith("OPS")), "")
    bio_score = None
    if bio:
        from Bio import Align
        al = Align.PairwiseAligner(); al.mode = "global"
        al.match_score, al.mismatch_score, al.open_gap_score, al.extend_gap_score = 2, -1, -2, -2
        t1 = time.time(); bio_score = int(al.score(qs, rs)); print(f"  (biopython took {time.time()-t1:.0f} s)")
    print(f"[{phase}] m={m} n={n} seed={seed}   models ran in {dt:.0f} s")
    rows = [("CPU reference score on card run", sil["gold"], got["gold"], "C golden nw_align_ref"),
            ("kernel score printed by the card", sil["kernel"], got["wave"], "PE-array model nw_wave_scalar NW_PE=3968")]
    if bio_score is not None:
        rows.append(("CPU reference score on card run", sil["gold"], bio_score, "Biopython PairwiseAligner"))
    if got["nops"] > -1_000_000_000:
        rows += [("card walker status", sil["status"], got["status"], "wv_tb_walk (kernel source)"),
                 ("card walker ops", sil["nops"], got["nops"], "wv_tb_walk"),
                 ("card walker cache fills", sil["fills"], got["fills"], "wv_tb_walk"),
                 ("card path score", sil["path"], got["path"], "wv_tb_walk")]
    ok = True
    for what, s, g, who in rows:
        good = (s == g); ok &= good
        print(f"  {'OK  ' if good else 'DIFF'} {what:<34} silicon {s:>8}   replay {g:>8}   [{who}]")
    if ops:
        # independent re-score of the replayed path
        i, j, sc = m, n, 0
        for c in ops:
            if c == "0": sc += 2 if qs[i-1] == rs[j-1] else -1; i -= 1; j -= 1
            elif c == "1": sc -= 2; i -= 1
            else: sc -= 2; j -= 1
        good = (i == 0 and j == 0 and sc == sil["gold"]); ok &= good
        print(f"  {'OK  ' if good else 'DIFF'} independent re-score of replayed ops: {sc} (ends at {i},{j})")
    if kernel and sil["m"] * sil["n"] <= 3_000_000_000:
        ok &= run_kernel(phase, qs, rs, sil)
    return ok


def main():
    ph = sys.argv[sys.argv.index("--phase") + 1] if "--phase" in sys.argv else "both"
    bio = "--bio" in sys.argv
    sil = silicon()
    ok = True
    if ph in ("check", "both"):
        ok &= run("check", 12345, sil["check"], bio, kernel="--kernel" in sys.argv)
    if ph in ("perf", "both"):
        ok &= run("perf", 12346, sil["perf"], bio and "--bio-perf" in sys.argv)
    print("REPLAY", "MATCHES SILICON" if ok else "DIFFERS FROM SILICON")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
