#!/usr/bin/env bash
# Negative controls for the KERNEL-level run: plant one bug in a copy of the kernel sources (or one -D flag in the
# kernel translation unit only), run the real kernel on the generated cases at NW_PE=512, require failures.
# usage: linear/verify/kernel_mutants.sh     (needs AP_INT_INC, PY)
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; LIN="$(dirname "$HERE")"; ROOT="$(dirname "$LIN")"
OUT="${OUT:-$ROOT/results/verification}"; mkdir -p "$OUT"; REPORT="$OUT/kernel_mutants.txt"; : > "$REPORT"
PY="${PY:-python3}"
# name | file | old | new   (file "-" with KFLAGS in new = flag-only mutant)
MUT=(
"base3_weight_wrong|kernel_tile_1b_s1t.cpp|* 3 + (ap_uint<8>)dcode|* 2 + (ap_uint<8>)dcode"
"band_window_639|-|-|-DWV_BWIN=639"
"walker_diag_step|wv_tb_walk.h|const int dc = (d == NWW_DIAG) ? 2 : 1;|const int dc = 1;"
"lut_tie_dir_changed|nw_wave_lut.h|0x18, 0x10, 0x08, 0x00, 0x81|0x58, 0x10, 0x08, 0x00, 0x81"
"lut_dv_changed|nw_wave_lut.h|0x1b, 0x13, 0x0b, 0x03, 0x84|0x1a, 0x13, 0x0b, 0x03, 0x84"
)
W0=$(mktemp -d); "$PY" "$HERE/gen_cases.py" --pe 512 > "$W0/cases.txt"
caught=0; total=0
for spec in "${MUT[@]}"; do
  IFS='|' read -r name file old new <<<"$spec"
  T=$(mktemp -d); cp "$LIN"/src/* "$T"/; KF=""
  if [ "$file" = "-" ]; then KF="$new"
  else python3 - "$T/$file" "$old" "$new" <<'PY' || { echo "$name: PATTERN NOT FOUND" | tee -a "$REPORT"; continue; }
import sys
p, old, new = sys.argv[1:4]; s = open(p).read()
if old not in s: sys.exit(1)
open(p, "w").write(s.replace(old, new))
PY
  fi
  total=$((total+1))
  if ! SRC="$T" KFLAGS="$KF" DIRDEPTH=65536 QDEPTH=1024 RDEPTH=256 "$HERE/build_kernel.sh" 512 "$T/kd" > "$T/build.log" 2>&1; then
    echo "$name: BUILD FAILED (not a valid mutant)" | tee -a "$REPORT"; total=$((total-1)); continue
  fi
  timeout 900 "$T/kd" "$W0/cases.txt" > "$T/out.txt" 2>"$T/err.txt"; rc=$?
  if "$PY" "$HERE/check_cases.py" "$W0/cases.txt" "$T/out.txt" > "$T/report.txt" 2>&1; then
    echo "$name: NOT CAUGHT  <-- blind spot" | tee -a "$REPORT"
  else
    caught=$((caught+1))
    why=$(grep -E "^FAIL" "$T/report.txt" | awk '{print $4}' | sort | uniq -c | sort -rn | head -4 | awk '{printf "%s(x%s) ", $2, $1}')
    [ -z "$why" ] && why="(driver rc=$rc: $(tail -c 120 "$T/err.txt" | tr '\n' ' '))"
    echo "$name: caught  $(grep -E '^TOTAL' "$T/report.txt" | sed 's/cells=[0-9,]* //')  first-failing: $why" | tee -a "$REPORT"
  fi
  rm -rf "$T"
done
rm -rf "$W0"
echo "kernel mutants caught: $caught / $total" | tee -a "$REPORT"
[ "$caught" -eq "$total" ]
