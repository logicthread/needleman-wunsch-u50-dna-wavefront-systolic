#!/usr/bin/env python3
"""Judge for the verification harness. Reads dut_driver output + the case file and compares
every model against references that share no code with the repository:

  ref_py   pure-Python full-matrix Needleman-Wunsch (match +2, mismatch -1, gap -2) with its own
           direction matrix (tie-break DIAG > UP > LEFT) -- only for m*n <= PYDP_MAX cells
  bio      Biopython PairwiseAligner, global, same scoring (a separate C implementation)
  lev      Levenshtein distance; checks score == (m+n) - 3*lev  (docs/ARCHITECTURE.md section 2)

and re-scores the card-walker ops itself (ops walk from cell (m,n) backwards; they must end exactly at
(0,0), and the sum of their scores must equal the optimum). 'N' is a mismatch with everything.

Exit status 0 only if no check failed. Prints a table per class and per check.
usage: check_cases.py cases.txt dut_output.txt [--json out.json]
"""
import sys, json, collections
from Bio import Align

PYDP_MAX = 150_000
NOTRUN = -1_000_000_000_000   # driver marker for "model not run"; -9 is a legal score, so it cannot be the marker
MATCH, MISMATCH, GAP = 2, -1, -2


def sub(a, b):
    return MATCH if (a == b and a != "N") else MISMATCH


def ref_py(q, r, want_dirs):
    m, n = len(q), len(r)
    H = [[0] * (n + 1) for _ in range(m + 1)]
    for i in range(1, m + 1):
        H[i][0] = i * GAP
    for j in range(1, n + 1):
        H[0][j] = j * GAP
    D = [] if want_dirs else None
    for i in range(1, m + 1):
        row = [] if want_dirs else None
        for j in range(1, n + 1):
            d = H[i - 1][j - 1] + sub(q[i - 1], r[j - 1])
            u = H[i - 1][j] + GAP
            l = H[i][j - 1] + GAP
            h = max(d, u, l)
            H[i][j] = h
            if want_dirs:
                row.append("0" if h == d else ("1" if h == u else "2"))
        if want_dirs:
            D.append("".join(row))
    return H[m][n], ("".join(D) if want_dirs else None)


def lev(q, r):
    m, n = len(q), len(r)
    prev = list(range(n + 1))
    for i in range(1, m + 1):
        cur = [i] + [0] * n
        for j in range(1, n + 1):
            cur[j] = min(prev[j - 1] + (q[i - 1] != r[j - 1] or q[i - 1] == "N"), prev[j] + 1, cur[j - 1] + 1)
        prev = cur
    return prev[n]


def rescore(q, r, ops):
    """Re-score a card ops string independently. Returns (score, error-string-or-None)."""
    i, j, sc = len(q), len(r), 0
    for k, c in enumerate(ops):
        if c == "0":
            if i < 1 or j < 1:
                return None, f"DIAG past the boundary at step {k}"
            sc += sub(q[i - 1], r[j - 1]); i -= 1; j -= 1
        elif c == "1":
            if i < 1:
                return None, f"UP past the boundary at step {k}"
            sc += GAP; i -= 1
        elif c == "2":
            if j < 1:
                return None, f"LEFT past the boundary at step {k}"
            sc += GAP; j -= 1
        else:
            return None, f"bad op {c!r} at step {k}"
    if i or j:
        return None, f"path stops at ({i},{j}), not (0,0)"
    return sc, None


def main():
    cases_f, dut_f = sys.argv[1], sys.argv[2]
    jout = sys.argv[sys.argv.index("--json") + 1] if "--json" in sys.argv else None
    cases = {}
    for line in open(cases_f):
        if line.startswith("#") or not line.strip():
            continue
        cid, cls, m, n, q, r = line.split()
        cases[cid] = (cls, "" if q == "-" else q, "" if r == "-" else r)

    al = Align.PairwiseAligner()
    al.mode = "global"
    al.match_score, al.mismatch_score = MATCH, MISMATCH
    al.open_gap_score = al.extend_gap_score = GAP
    # Biopython scores N like any other letter; remap N to a letter that cannot match
    def bio(q, r):
        if "N" in q or "N" in r:
            q = q.replace("N", "x"); r = r.replace("N", "y")
        return int(al.score(q, r)) if q and r else GAP * (len(q) + len(r))

    stats = collections.defaultdict(lambda: collections.Counter())
    fails = []
    header = None
    cur = None
    seen = set()
    skipped = {}
    kernel = False

    def finish(c):
        if c is None:
            return
        cid = c["id"]; cls, q, r = cases[cid]; m, n = len(q), len(r)
        seen.add(cid)
        S = stats[cls]; S["cases"] += 1
        S["cells"] += m * n

        def chk(name, ok, detail=""):
            S[name + (":pass" if ok else ":FAIL")] += 1
            if not ok:
                fails.append((cid, cls, name, detail))

        gold = c["gold"]
        b = bio(q, r)
        chk("bio==gold", b == gold, f"bio={b} gold={gold}")
        chk("kernel_score==gold" if kernel else "wave==gold", c["wave"] == gold, f"model/kernel={c['wave']} gold={gold}")
        if m * n <= PYDP_MAX:
            want_dirs = "DIRS" in c
            p, pd = ref_py(q, r, want_dirs)
            chk("py==gold", p == gold, f"py={p} gold={gold}")
            if want_dirs:
                chk("dirs==py_dirs", c["DIRS"] == pd, "direction matrix differs (tie-break or LUT)")
            if "N" not in q + r:       # the identity is only claimed for the four real bases
                chk("score==(m+n)-3lev", p == (m + n) - 3 * lev(q, r), f"py={p} lev={lev(q, r)}")
        if m > 0 and n > 0 and c["dpath"] != NOTRUN:
            chk("dirs_path==gold", c["dpath"] == gold, f"dirs_path={c['dpath']} gold={gold}")
        if cls not in ("N", "Z") and m > 0 and n > 0 and c["cst"] != NOTRUN:
            chk("host==gold", c["hscore"] == gold, f"host={c['hscore']}")
            chk("card_status==0", c["cst"] == 0, f"status={c['cst']}")
            chk("card_score==gold", c["csc"] == gold, f"card={c['csc']}")
            chk("card_ops==host_ops", c["diff"] == -1 and c["cnops"] == c["hn"],
                f"nops card={c['cnops']} host={c['hn']} first_diff={c['diff']}")
            sc, err = rescore(q, r, c.get("OPS", ""))
            chk("card_ops_rescore==gold", err is None and sc == gold, err or f"rescored={sc} gold={gold}")

    for line in open(dut_f):
        t = line.split()
        if not t:
            continue
        if t[0] == "DRIVER":
            header = line.strip(); kernel = "KERNEL" in header
        elif t[0] == "SKIP":
            skipped[t[1]] = " ".join(t[2:]); seen.add(t[1])
        elif t[0] == "CASE":
            finish(cur)
            cur = dict(id=t[1], gold=int(t[5]), wave=int(t[6]), dpath=int(t[7]), hscore=int(t[8]), hn=int(t[9]),
                       cst=int(t[10]), cnops=int(t[11]), csc=int(t[12]), fills=int(t[13]), diff=int(t[14]))
        elif t[0] in ("OPS", "DIRS") and cur is not None:
            cur[t[0]] = t[1]
        elif t[0] == "BADCASE":
            fails.append((t[1], "?", "badcase", "length mismatch in case file"))
    finish(cur)
    missing = set(cases) - seen
    for cid in sorted(missing):
        fails.append((cid, cases[cid][0], "missing", "driver produced no output"))

    print(header or "no DRIVER line")
    tot = collections.Counter()
    for cls in sorted(stats):
        S = stats[cls]
        checks = sorted({k.rsplit(":", 1)[0] for k in S if k.endswith((":pass", ":FAIL"))})
        print(f"class {cls}: {S['cases']} cases, {S['cells']:,} cells")
        for ck in checks:
            p, f = S[ck + ":pass"], S[ck + ":FAIL"]
            tot[ck + ":pass"] += p; tot[ck + ":FAIL"] += f
            print(f"    {ck:<26} pass {p:>5}  fail {f}")
    ncases = sum(s["cases"] for s in stats.values())
    ncells = sum(s["cells"] for s in stats.values())
    nchk_p = sum(v for k, v in tot.items() if k.endswith(":pass")); nchk_f = sum(v for k, v in tot.items() if k.endswith(":FAIL"))
    nskip = len(skipped)
    print(f"TOTAL cases={ncases} cells={ncells:,} checks_passed={nchk_p} checks_failed={nchk_f} missing={len(missing)} skipped={nskip}")
    if nskip:
        why = collections.Counter(v.split("(")[0].strip() for v in skipped.values())
        print("SKIPPED by reason:", dict(why))
    for f in fails[:25]:
        print("FAIL", *f)
    if jout:
        json.dump({"header": header, "cases": ncases, "cells": ncells, "passed": nchk_p, "failed": nchk_f,
                   "missing": len(missing), "fails": fails[:200]}, open(jout, "w"), indent=1)
    sys.exit(1 if (nchk_f or missing) else 0)


if __name__ == "__main__":
    main()
