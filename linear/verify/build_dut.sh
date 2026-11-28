#!/usr/bin/env bash
# build_dut.sh PE OUT_BINARY  -- compile the repository models + dut_driver for NW_PE=PE.
# env: AP_INT_INC (required), SRC (default linear/src), EXTRA_CFLAGS (extra flags for nw_wave.c)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; LIN="$(dirname "$HERE")"
AP="${AP_INT_INC:?set AP_INT_INC (see docs/ENVIRONMENT.md)}"; SRC="${SRC:-$LIN/src}"
PE="$1"; BIN="$2"; W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
clang -O2 -w -c ${EXTRA_CFLAGS:-} -DNW_PE="$PE" -I"$SRC" "$SRC/nw_wave.c" -o "$W/wave.o"
clang -O2 -w -c -I"$SRC" "$SRC/nw_ref.c" -o "$W/ref.o"
clang -O2 -w -c -I"$SRC" -I"$LIN/host" -DNWH_CONT -DNWH_DIR_B3 -DNWH_BWIN=640 "$LIN/host/nw_host_traceback.c" -o "$W/tb.o"
clang++ -std=c++14 -O2 -w -I"$AP" -I"$SRC" -I"$LIN/host" -DNW_PE="$PE" -DNWH_CONT -DNWH_DIR_B3 -DNWH_BWIN=640 \
    "$HERE/dut_driver.cpp" "$W/wave.o" "$W/ref.o" "$W/tb.o" -o "$BIN"
