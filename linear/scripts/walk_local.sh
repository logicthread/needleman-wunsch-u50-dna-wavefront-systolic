#!/usr/bin/env bash
# Local unit test of the on-card traceback walker (src/wv_tb_walk.h) -- Mac clang,
# open-source ap_int headers (AP_INT_INC, see docs/ENVIRONMENT.md). Usage: scripts/walk_local.sh [NW_PE]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"   # = linear/
AP="${AP_INT_INC:?set AP_INT_INC to the include/ dir of github.com/Xilinx/HLS_arbitrary_Precision_Types}"
PE=${1:-512}
O=$(mktemp -d)
clang -O2 -w -c -I"$ROOT/src" "$ROOT/src/nw_wave.c" -o "$O/nw_wave.o"
clang -O2 -w -c -I"$ROOT/src" "$ROOT/src/nw_ref.c"  -o "$O/nw_ref.o"
clang -O2 -w -c -I"$ROOT/src" -I"$ROOT/host" -DNWH_CONT -DNWH_DIR_B3 -DNWH_BWIN=640 \
  "$ROOT/host/nw_host_traceback.c" -o "$O/tb.o"
clang++ -std=c++14 -O2 -w -I"$AP" -I"$ROOT/src" -I"$ROOT/host" -DNW_PE="$PE" \
  -DNWH_CONT -DNWH_DIR_B3 -DNWH_BWIN=640 \
  "$ROOT/test/tb_walk_local.cpp" "$O/nw_wave.o" "$O/nw_ref.o" "$O/tb.o" -o "$O/walk"
"$O/walk"
