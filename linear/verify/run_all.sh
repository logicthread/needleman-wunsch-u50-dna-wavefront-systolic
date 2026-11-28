#!/usr/bin/env bash
# Build the repository models for several array sizes, run the generated cases, judge them.
# usage: linear/verify/run_all.sh [PE ...]        default: 128 512 3968
# env:   AP_INT_INC   include dir of HLS_arbitrary_Precision_Types (needed for the walker)
#        PY           python with biopython (default: python3)
#        OUT          output dir (default: results/verification)
#        EXTRA_CFLAGS extra flags for the model sources (used by mutants.sh)
#        SRC          source dir override (used by mutants.sh); default linear/src
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; LIN="$(dirname "$HERE")"; ROOT="$(dirname "$LIN")"
AP="${AP_INT_INC:?set AP_INT_INC (see docs/ENVIRONMENT.md)}"
PY="${PY:-python3}"
OUT="${OUT:-$ROOT/results/verification}"
SRC="${SRC:-$LIN/src}"
PES=("$@"); [ ${#PES[@]} -eq 0 ] && PES=(128 512 3968)
mkdir -p "$OUT"
W=$(mktemp -d)
rc=0
for PE in "${PES[@]}"; do
  echo "=== NW_PE=$PE"
  "$PY" "$HERE/gen_cases.py" --pe "$PE" > "$W/cases_$PE.txt"
  "$HERE/build_dut.sh" "$PE" "$W/dut_$PE"
  "$W/dut_$PE" "$W/cases_$PE.txt" > "$W/dut_$PE.out"
  if "$PY" "$HERE/check_cases.py" "$W/cases_$PE.txt" "$W/dut_$PE.out" --json "$OUT/summary_pe$PE.json" > "$OUT/report_pe$PE.txt"; then
    echo "PE=$PE PASS"; else echo "PE=$PE FAIL"; rc=1; fi
  tail -n +1 "$OUT/report_pe$PE.txt" | grep -E "^TOTAL|^FAIL" | head -12
done
rm -rf "$W"
exit $rc
