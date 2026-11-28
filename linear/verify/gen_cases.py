#!/usr/bin/env python3
"""Deterministic test-case generator for the verification harness (see docs/VERIFICATION.md).

Output (stdout): one case per line   id class m n query reference     ('-' = empty string)
Classes: X exhaustive short, R random, B boundary lengths (PE/tile/NMIN dependent), M mutated
pair, E hand-built edge shapes, N contains 'N' (model only), Z empty sequence (model only).
Everything is seeded: same --pe and --seed give the same file.
"""
import argparse, random, sys

ACGT = "ACGT"
COMP = {"A": "T", "C": "G", "G": "C", "T": "A"}


def rnd(rng, k, alpha=ACGT):
    return "".join(rng.choice(alpha) for _ in range(k))


def mutate(rng, s, sub, ins, dele):
    out = []
    for ch in s:
        x = rng.random()
        if x < dele:
            continue
        if x < dele + ins:
            out.append(rng.choice(ACGT))
        if rng.random() < sub:
            out.append(rng.choice([c for c in ACGT if c != ch]))
        else:
            out.append(ch)
    return "".join(out) or "A"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pe", type=int, default=512)
    ap.add_argument("--seed", type=int, default=20261128)
    ap.add_argument("--scale", type=float, default=1.0, help="multiplies the number of random cases")
    a = ap.parse_args()
    P = a.pe
    TILE = 128
    NMIN = P + 8 * TILE
    BWIN = 640
    thr = NMIN - BWIN                       # WV_GAP_OF switches here
    rng = random.Random(a.seed)
    cases = []

    def add(cls, q, r):
        cases.append((cls, q, r))

    # X: every pair of strings of length 1..3 over ACGT
    strs = ["".join(p) for L in (1, 2, 3) for p in __import__("itertools").product(ACGT, repeat=L)]
    for q in strs:
        for r in strs:
            add("X", q, r)

    # R: random pairs
    for _ in range(int(150 * a.scale)):
        add("R", rnd(rng, rng.randint(1, 64)), rnd(rng, rng.randint(1, 64)))
    for _ in range(int(80 * a.scale)):
        add("R", rnd(rng, rng.randint(1, int(1.5 * P))), rnd(rng, rng.randint(1, 1200)))
    # B: lengths around the structural boundaries
    ms = sorted({1, 2, 127, 128, 129, 255, 256, 257, P - 1, P, P + 1, 2 * P - 1, 2 * P, 2 * P + 1, 3 * P + 17}
                - {0})
    ns = sorted({1, 2, 63, 64, 65, 127, 128, 129, thr - 1, thr, thr + 1, NMIN - 1, NMIN, NMIN + 1, 700, 1300} - {0})
    grid = [(m, n) for m in ms for n in ns if m * n <= 4_000_000]
    rng.shuffle(grid)
    for m, n in grid[: int(160 * a.scale)]:
        add("B", rnd(rng, m), rnd(rng, n))
    # M: related pairs at several divergences
    for sub, ins, dele in [(0.01, 0.002, 0.002), (0.05, 0.01, 0.01), (0.2, 0.05, 0.05), (0.5, 0.1, 0.1), (0.02, 0.05, 0.0), (0.02, 0.0, 0.05)]:
        for _ in range(int(25 * a.scale)):
            q = rnd(rng, rng.randint(20, int(1.2 * P)))
            add("M", q, mutate(rng, q, sub, ins, dele))
    # E: hand-built shapes
    k = max(P // 2, 200)
    base = rnd(rng, k)
    add("E", base, base)                                              # identical
    add("E", base, base[:7] + ("A" if base[7] != "A" else "C") + base[8:])   # one substitution
    add("E", base, base[:50] + "G" + base[50:])                       # one insertion
    add("E", base, base[:50] + base[51:])                             # one deletion
    add("E", "A" * k, "A" * (k + 17))                                 # homopolymer, unequal length
    add("E", "A" * k, "T" * k)                                        # nothing matches
    add("E", "A" * 5, "T" * 900)
    add("E", "ACGT" * (k // 4), "CGTA" * (k // 4))                    # periodic, rotated
    add("E", "AC" * (k // 2), "CA" * (k // 2))                        # many ties
    add("E", base, "".join(COMP[c] for c in reversed(base)))          # reverse complement
    add("E", base, base[::-1])
    add("E", base + base, base)                                       # tandem duplication
    add("E", base, base + base)
    add("E", rnd(rng, 4 * P), rnd(rng, 7))                            # tall and thin
    add("E", rnd(rng, 9), rnd(rng, 3000))                             # wide and flat
    add("E", rnd(rng, 2 * P), "A")
    add("E", "A", rnd(rng, 700))
    for alpha in ("AC", "AT", "ACG"):                                 # low complexity: ties everywhere
        for _ in range(int(20 * a.scale)):
            add("E", rnd(rng, rng.randint(30, int(1.3 * P)), alpha), rnd(rng, rng.randint(30, 1100), alpha))
    # N: ambiguity code (model only; the card encodes bases in 2 bits and has no N)
    for _ in range(int(40 * a.scale)):
        q = list(rnd(rng, rng.randint(5, 200)))
        r = list(rnd(rng, rng.randint(5, 200)))
        for s in (q, r):
            for _ in range(rng.randint(1, 6)):
                s[rng.randrange(len(s))] = "N"
        add("N", "".join(q), "".join(r))
    add("N", "NNNN", "NNNN")
    add("N", "ACGT", "NNNN")
    # Z: empty sequences (model only)
    add("Z", "", "")
    add("Z", "", "ACGTA")
    add("Z", "ACGTACG", "")
    add("Z", "", "A")

    print(f"# gen_cases.py --pe {P} --seed {a.seed} --scale {a.scale}  cases={len(cases)}")
    for i, (cls, q, r) in enumerate(cases):
        print(f"c{i:05d} {cls} {len(q)} {len(r)} {q or '-'} {r or '-'}")


if __name__ == "__main__":
    main()
