#!/usr/bin/env bash
# Negative controls: plant one deliberate bug at a time in a COPY of linear/src, run the harness at
# NW_PE=128 and require it to FAIL. A harness that cannot fail proves nothing.
# usage: linear/verify/mutants.sh      (needs AP_INT_INC and PY like run_all.sh)
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; LIN="$(dirname "$HERE")"; ROOT="$(dirname "$LIN")"
OUT="${OUT:-$ROOT/results/verification}"; mkdir -p "$OUT"
REPORT="$OUT/mutants.txt"
: > "$REPORT"
# name | file | old | new
MUT=(
"snapshot_removed|nw_wave.c|s_dh[i - 1]|dh[i - 1]"
"lut_entry_flipped|nw_wave_lut.h|0x1b, 0x13, 0x0b, 0x03, 0x84|0x1a, 0x13, 0x0b, 0x03, 0x84"
"lut_tiebreak_changed|nw_wave_lut.h|0x18, 0x10, 0x08, 0x00, 0x81|0x58, 0x10, 0x08, 0x00, 0x81"
"eq_lut_unreachable_entry|nw_wave_lut.h|0x84, 0x85, 0x86, 0x00,|0x04, 0x85, 0x86, 0x00,"
"band_boundary_dropped|nw_wave.c|((band == 0) ? (int8_t)NWW_GAP : bnd[j])|((int8_t)NWW_GAP)"
"score_offset|nw_wave.c|int64_t acc = (int64_t)NWW_GAP * m;|int64_t acc = (int64_t)NWW_GAP * m + 1;"
"dv_reset_wrong|nw_wave.c|dv[i] = (int8_t)NWW_GAP;        /* dV[i][0] = G, from H[i][0] = i*G */|dv[i] = 0;"
"walker_diag_step|wv_tb_walk.h|const int dc = (d == NWW_DIAG) ? 2 : 1;|const int dc = 1;"
"walker_wrong_half_beat|wv_tb_walk.h|(int64_t)grp * WV_DIR_SBEATS + (il >> 6);|(int64_t)grp * WV_DIR_SBEATS;"
"walker_leading_edge|wv_tb_walk.h|d = (i > 0) ? (ap_uint<2>)NWW_UP : (ap_uint<2>)NWW_LEFT;|d = (i > 0) ? (ap_uint<2>)NWW_LEFT : (ap_uint<2>)NWW_UP;"
)
caught=0; total=0
for spec in "${MUT[@]}"; do
  IFS='|' read -r name file old new <<<"$spec"
  T=$(mktemp -d); cp "$LIN"/src/* "$T"/
  python3 - "$T/$file" "$old" "$new" <<'PY' || { echo "$name: PATTERN NOT FOUND (stale mutant)" | tee -a "$REPORT"; continue; }
import sys
p, old, new = sys.argv[1:4]
s = open(p).read()
if old not in s: sys.exit(1)
open(p, "w").write(s.replace(old, new))
PY
  total=$((total+1))
  if SRC="$T" OUT="$T/out" "$HERE/run_all.sh" 128 > "$T/log.txt" 2>&1; then
    case "$name" in
      eq_*) total=$((total-1)); echo "$name: survived, AS EXPECTED (equivalent mutant: that table slot is never indexed; see docs/ARCHITECTURE.md section 3, 18 of 128 reachable)" | tee -a "$REPORT" ;;
      *)    echo "$name: NOT CAUGHT  <-- harness has a blind spot" | tee -a "$REPORT" ;;
    esac
  else
    caught=$((caught+1))
    # which checks fired
    fired=$(grep -E "^\s+\S+\s+pass" "$T/out/report_pe128.txt" 2>/dev/null | awk '$5>0{print $1}' | sort -u | tr '\n' ' ')
    tail -1 "$T/log.txt" | grep -q . >/dev/null
    why=$(grep -E "^FAIL" "$T/out/report_pe128.txt" 2>/dev/null | awk '{print $4}' | sort | uniq -c | sort -rn | head -4 | awk '{printf "%s(x%s) ", $2, $1}')
    [ -z "$why" ] && why="(build or driver failure, see log)"
    echo "$name: caught  first-failing-checks: $why" | tee -a "$REPORT"
  fi
  rm -rf "$T"
done
echo "mutants caught: $caught / $total" | tee -a "$REPORT"
[ "$caught" -eq "$total" ]
