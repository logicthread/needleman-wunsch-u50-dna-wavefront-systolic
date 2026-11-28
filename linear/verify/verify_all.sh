#!/usr/bin/env bash
# One command for everything that can run on a laptop. Needs: clang/clang++, python3 with biopython, AP_INT_INC.
#   AP_INT_INC=<.../HLS_arbitrary_Precision_Types/include> PY=<python with biopython> linear/verify/verify_all.sh
# Writes reports to results/verification/. Takes about 15 minutes (the 33 G-cell replay is ~95 s of it, the
# 3968-PE kernel run is the long one); set QUICK=1 to skip the 3968-PE kernel run, the mutants and the perf replay.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; LIN="$(dirname "$HERE")"; ROOT="$(dirname "$LIN")"
OUT="${OUT:-$ROOT/results/verification}"; mkdir -p "$OUT"; export OUT
PY="${PY:-python3}"; export PY
: "${AP_INT_INC:?set AP_INT_INC}"
rc=0
step() { echo; echo "######## $*"; }

step "1. models (nw_wave.c, walker, host walker) vs independent references, NW_PE = 128 512 3968"
"$HERE/run_all.sh" 128 512 3968 || rc=1

step "2. delta-range assertions on (NWW_CHECK_RANGE), NW_PE = 128 512"
OUT="$OUT/range" EXTRA_CFLAGS=-DNWW_CHECK_RANGE "$HERE/run_all.sh" 128 512 || rc=1

step "3. REAL KERNEL source on laptop stand-ins, NW_PE = 512 (+3968 unless QUICK)"
KPES="512"; [ -z "${QUICK:-}" ] && KPES="512 3968"
for PE in $KPES; do
  W=$(mktemp -d)
  "$PY" "$HERE/gen_cases.py" --pe "$PE" > "$W/cases.txt"
  DIRDEPTH=65536 QDEPTH=1024 RDEPTH=256 "$HERE/build_kernel.sh" "$PE" "$W/kd" || { rc=1; continue; }
  "$W/kd" "$W/cases.txt" > "$W/out.txt"
  if "$PY" "$HERE/check_cases.py" "$W/cases.txt" "$W/out.txt" --json "$OUT/kernel_summary_pe$PE.json" > "$OUT/kernel_report_pe$PE.txt"; then
    echo "kernel PE=$PE PASS"; else echo "kernel PE=$PE FAIL"; rc=1; fi
  grep -E "^TOTAL|^SKIPPED|^FAIL" "$OUT/kernel_report_pe$PE.txt" | head
  rm -rf "$W"
done

if [ -z "${QUICK:-}" ]; then
  step "4. negative controls (planted bugs must be caught)"
  "$HERE/mutants.sh" || rc=1
  "$HERE/kernel_mutants.sh" || rc=1
  step "5. replay of the silicon run (same seeds, same sequences)"
  "$PY" "$HERE/replay_gate.py" --phase both --bio --bio-perf --kernel | tee "$OUT/replay_gate.txt" || rc=1
fi
echo; [ $rc -eq 0 ] && echo "ALL VERIFICATION STEPS PASSED" || echo "SOME VERIFICATION STEP FAILED"
exit $rc
