#!/usr/bin/env bash
# build_kernel.sh PE OUT_BINARY -- compile the REAL kernel (linear/src/kernel_tile_1b_s1t.cpp) against the host stand-ins
# for hls_stream.h / hls_task.h (linear/verify/hls_stub) and link kernel_driver.cpp.
# env: AP_INT_INC (required); SRC (default linear/src); KDEFS (extra -D flags, e.g. "-DWV_QDEPTH=2048")
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; LIN="$(dirname "$HERE")"
AP="${AP_INT_INC:?set AP_INT_INC}"; SRC="${SRC:-$LIN/src}"
PE="$1"; BIN="$2"; W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
NPORT=$((PE / 128))
python3 - "$NPORT" > "$W/kernel_proto.inc" <<'PY'
import sys
n = int(sys.argv[1])
print('extern "C" void nw_tile_u50(uint64_t *, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t' + ''.join(', dirbeat *' for _ in range(n)) + ');')
print('#define WV_DIRARGS ' + ', '.join(f'dbuf[{i}].data()' for i in range(n)))
PY
FLAGS=(-DNW_PE="$PE" -DWV_TILE_PE=128 -DWV_TRACEBACK -DWV_DIR_B3 -DWV_DIRDEPTH="${DIRDEPTH:-65536}" -DWV_QDEPTH="${QDEPTH:-1024}" -DWV_RDEPTH="${RDEPTH:-256}" -DWV_BDEPTH=8192)
[ "$PE" -gt 2048 ] && FLAGS+=(-DWV_SEAM_RELAY)
# KFLAGS: extra flags for the KERNEL translation unit only (mutants)
clang++ -std=c++14 -O1 -w -pthread -I"$HERE/hls_stub" -I"$AP" -I"$SRC" "${FLAGS[@]}" ${KFLAGS:-} -c "$SRC/kernel_tile_1b_s1t.cpp" -o "$W/k.o"
clang -O2 -w -c -DNW_PE="$PE" -I"$SRC" "$SRC/nw_wave.c" -o "$W/w.o"
clang -O2 -w -c -I"$SRC" "$SRC/nw_ref.c" -o "$W/r.o"
clang -O2 -w -c -I"$SRC" -I"$LIN/host" -DNWH_CONT -DNWH_DIR_B3 -DNWH_BWIN=640 "$LIN/host/nw_host_traceback.c" -o "$W/tb.o"
clang++ -std=c++14 -O2 -w -pthread -I"$HERE/hls_stub" -I"$W" -I"$AP" -I"$SRC" -I"$LIN/host" "${FLAGS[@]}" -DNWH_CONT -DNWH_DIR_B3 -DNWH_BWIN=640 \
    "$HERE/kernel_driver.cpp" "$W/k.o" "$W/w.o" "$W/r.o" "$W/tb.o" -o "$BIN"
