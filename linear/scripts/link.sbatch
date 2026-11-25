#!/usr/bin/env bash
# HARDWARE LINK: 2048-PE tiled softcore WITH TRACEBACK -> .xclbin
#
#   sbatch --export=ALL,FREQ=300,PE=2048 tb_link.sbatch
#
# CONSERVATIVE, DELIBERATELY. Nothing experimental is enabled:
#   - no pblock. OOC P&R measured the floorplanned variant 9 MHz SLOWER than
#     unconstrained (404.5 vs 413.6), because the critical path is the reset tree.
#   - no --optimize 3. On the Myers kernel it bought exactly 0.000 ns (N21).
#   - no phys_opt directives. Same reason.
#   - FREQ=300 against 368.7 MHz measured OOC for 2048 score-only. v++ AUTO-SCALES
#     rather than failing, so a miss still yields a usable bitstream and reports the
#     real shell-loaded clock -- which is the one number simulation cannot give.
#
# --temp_dir MUST STAY ON /home. On node-local /tmp, readdir order makes
# ii_infra_ipdefs win over other_ipdefs for a duplicate IP and synthesis dies with
# "[Synth 8-5809] Error generated from encrypted envelope" (NOTES.md N20). Both temp
# dirs below are under SLURM_SUBMIT_DIR, which is /home.
#
# READ OUT OF THE LOG: the achieved kernel clock, any AUTO-FREQ-SCALING line, and WNS
# from the routed timing summary. GCUPS = PE x f x n/(n+3.4*PE).
#SBATCH --job-name=tblink
#SBATCH --partition=debug
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=12
# 96G, NOT 32G. The "student 32 GB cap" does not exist -- `scontrol show partition debug`
# reports MaxMemPerNode UNLIMITED. 2048 PE fits in 32 G; 3072 PE does NOT and gets
# oom-killed mid-synthesis, which v++ reports as the misleading
#   ERROR: [VPL 60-704] Failed to complete hardware synthesis
# with Slurm recording OUT_OF_MEMORY and `Killed` in runme.log. Job 105278 died exactly
# this way at 00:31:08 after its csim gate and .xo had both succeeded.
#SBATCH --mem=96G
# 06:00:00, NOT 4h. Job 106854 (3968, no pblock, .xo rebuilt) projected LINK DONE at
# ~17:00 against a 17:15:27 kill -- 14 minutes of margin on a 4 h run, and a 4 h limit
# would have lost the whole build at bitstream write if placement had run 1% slower.
# Users CANNOT raise TimeLimit mid-flight: "Access/permission denied for job 106854".
# A link that rebuilds its .xo costs ~27 min on top of ~2h45 of impl, so 4 h was never
# enough headroom. Over-requesting wall time is free on this partition; losing a 4 h
# build is not.
#SBATCH --time=06:00:00
#SBATCH --output=tblink_%j.log

set -eo pipefail
cd "$SLURM_SUBMIT_DIR"
source "$SLURM_SUBMIT_DIR/scripts/env.sh" >/dev/null

TOP=nw_tile_u50
PE=${PE:-2048}
TPE=${TPE:-128}
FREQ=${FREQ:-300}
CFG=${CFG:-tile_link_tb.cfg}
CSIMCFG=${CSIMCFG:-}
# TAG lets a variant build sit beside the baseline instead of overwriting it. Without
# it BUILD collides with link_tb_2048_300, which holds the shipped deliverable.
TAG=${TAG:-}
BUILD="link_tb_${PE}_${FREQ}${TAG}"
mkdir -p "$BUILD"

echo "### Host: $(hostname)  Job: $SLURM_JOB_ID  $(date)"
echo "### TOP=$TOP  PE=$PE TILE_PE=$TPE TILES=$((PE/TPE))  FREQ=${FREQ}MHz  CFG=$CFG"
echo "### PLATFORM=$PLATFORM"
md5sum "$SRC" 2>/dev/null || md5sum src/kernel_tile.cpp
echo "### quota free: $(quota 2>/dev/null|tail -1|awk '{printf "%.1f GB", ($3-$2)/1048576}')"

# depth hints sized to the real capacity limit (181 kbp square), not to a testbench:
#   q/r: 181019 bases / 32 per word = 5657 words  -> 65536 is ample
#   bnd: n bytes                                   -> 1048576
#   dir: per port = 2 PCs = 512 MB / 64 B          -> 8388608 beats
# SRC lets a variant kernel (e.g. src/kernel_tile_ns.cpp for -DWV_PE_NOSHARE) be built
# without touching the baseline file. EXTRADEFS carries the variant's macro.
SRC=${SRC:-src/kernel_tile.cpp}
EXTRADEFS=${EXTRADEFS:-}
DEFS="-D NW_PE=$PE -D WV_TILE_PE=$TPE -D WV_TRACEBACK $EXTRADEFS \
      -D WV_QDEPTH=65536 -D WV_RDEPTH=65536 -D WV_BDEPTH=1048576 -D WV_DIRDEPTH=8388608"

# CSIM GATE. Cheap insurance: a 2 h link must not start on a source that cannot even
# simulate. The audit change since the last full verification was an #ifdef WV_TRACEBACK
# guard around wv_dirwriter (a no-op when that macro IS defined, which it is here) plus
# comment fixes, so the traceback token stream is unchanged -- but I overwrote the file
# without keeping a pre-edit copy, so that is reasoning, not a diff. Two minutes settles it.
# --- per-build IP cache (see N40) ---------------------------------------------------
# v++ defaults --remote_ip_cache to $PWD/.ipcache. Two links launched from the same
# submit dir therefore SHARE it. Seed a private copy so concurrent builds cannot race,
# while keeping the platform IP warm.
IPC="${BUILD}/.ipcache"
if [ ! -d "$IPC" ]; then
  mkdir -p "$IPC"
  if [ -d .ipcache ]; then
    cp -a .ipcache/. "$IPC"/ 2>/dev/null && echo "### ipcache seeded from shared ($(du -sm "$IPC" | cut -f1) MB)"
  else
    echo "### ipcache COLD (no shared cache to seed from)"
  fi
fi
echo "### using private ip cache: $IPC"

if [ -n "$CSIMCFG" ]; then
  echo "### [0/2] CSIM GATE ($CSIMCFG)   $(date +%T)"
  rm -rf "${BUILD}/_gate"; mkdir -p "${BUILD}/_gate"
  vitis-run --mode hls --csim --config "$CSIMCFG" --work_dir "${BUILD}/_gate" > "${BUILD}/gate.txt" 2>&1 || true
  grep -hE "dir ports|cases,|PASSED|FAILED" "${BUILD}/gate.txt" | head -6
  if ! grep -q "TILE TRACEBACK PASSED" "${BUILD}/gate.txt"; then
    echo "### CSIM GATE FAILED -- NOT LINKING"; grep -hE "FAIL |error:" "${BUILD}/gate.txt" | head -8; exit 1
  fi
  echo "### gate clean $(date +%T)"
fi

# REUSE ONLY IF THE .xo IS NEWER THAN EVERY SOURCE IT WAS BUILT FROM.
# The previous version reused on mere existence, and that silently re-implemented a
# stale object: a 2 h link rebuilt the old star design because the .xo predated the
# qloader fix by three hours. The tell was an identical WNS and a critical path through
# a register the fix had deleted. Existence is not freshness.
STALE=0
if [ -s "${BUILD}/${TOP}.xo" ]; then
  # $CFG is deliberately NOT in this list. v++ -c is invoked WITHOUT --config (only
  # v++ -l gets it), so the link cfg -- connectivity, sp=, nk=, vivado props -- cannot
  # affect the .xo. Listing it forced a 27 min recompile after a pure cfg fix (105697).
  for f in "$SRC" src/nw_wave.h src/nw_wave_lut.h; do
    [ -n "$f" ] && [ -f "$f" ] && [ "$f" -nt "${BUILD}/${TOP}.xo" ] && {
      echo "### .xo is STALE: $f is newer -- recompiling"; STALE=1; break; }
  done
else
  STALE=1
fi
if [ "$STALE" = "0" ]; then
  echo "### [1/2] reusing ${BUILD}/${TOP}.xo (verified newer than all sources)"
  ls -la "${BUILD}/${TOP}.xo"
else
echo "### [1/2] v++ compile -> ${BUILD}/${TOP}.xo   $(date +%T)"
v++ -c -t hw --platform "$PLATFORM" -k "$TOP" \
    -o "${BUILD}/${TOP}.xo" "$SRC" \
    --advanced.prop kernel."$TOP".kernel_flags="-std=c++14" \
    -I src $DEFS \
    --kernel_frequency "$FREQ" \
    --remote_ip_cache "$IPC" \
    --temp_dir "${BUILD}/_x" --log_dir "${BUILD}/logs" --report_dir "${BUILD}/reports"
echo "### .xo done $(date +%T)"; ls -la "${BUILD}/${TOP}.xo"
fi

echo "### [2/2] v++ link -> ${BUILD}/${TOP}_hw.xclbin   $(date +%T)"
# --save-temps RETAINS _xl, which holds Vivado's routed checkpoint. Without it v++
# deletes its temp dir on success and the .dcp is gone -- which is why clock-region
# placement could not be reported for the 512 build. Costs disk, not time; clean up
# after extracting the data.
v++ -l -t hw --save-temps --platform "$PLATFORM" \
    -o "${BUILD}/${TOP}_hw.xclbin" "${BUILD}/${TOP}.xo" \
    --config "$CFG" \
    --kernel_frequency "$FREQ" \
    --remote_ip_cache "$IPC" \
    --temp_dir "${BUILD}/_xl" --log_dir "${BUILD}/logs_l" --report_dir "${BUILD}/reports_l"

echo "### LINK DONE $(date +%T)"
ls -la "${BUILD}"/*.xclbin 2>/dev/null || true
echo "### ===== ACHIEVED CLOCK ====="
grep -rhoE "automatically changed to [0-9.]+ MHz|AUTO-FREQ-SCALING" "${BUILD}/logs_l" 2>/dev/null | sort -u | head -5 || echo "  (no auto-scaling: target met)"
grep -rhE "Kernel Frequency|kernel_frequency" "${BUILD}/reports_l" 2>/dev/null | head -4
echo "### ===== TIMING ====="
for f in $(find "${BUILD}/reports_l" -name "*timing_summary_routed.rpt" 2>/dev/null | head -2); do
  echo "--- $f"; grep -E "^ *WNS|Design Timing Summary" -A4 "$f" | head -10
done
echo "### ===== UTILISATION ====="
for f in $(find "${BUILD}/reports_l" -name "*full_util_routed.rpt" -o -name "*slr_util_routed.rpt" 2>/dev/null | head -2); do
  echo "--- $f"; grep -E "CLB LUTs|CLB Registers|Block RAM Tile|URAM|DSP" "$f" | head -6
done
echo "### ===== CLOCK REGION PLACEMENT (kernel cells) ====="
DCP=$(find "${BUILD}/_xl" -name "*routed.dcp" 2>/dev/null | head -1)
if [ -n "$DCP" ]; then
  cat > "${BUILD}/cr.tcl" <<TCL
open_checkpoint $DCP
set leaf [get_cells -quiet -hier -filter {NAME =~ "*nw_tile_u50_1*" && IS_PRIMITIVE}]
puts "CRTOTAL [llength \$leaf]"
array set cr {}
foreach c \$leaf {
  set s [get_property SITE \$c]
  if {\$s eq ""} continue
  set t [get_property CLOCK_REGION [get_sites \$s]]
  if {\$t ne ""} { incr cr(\$t) }
}
foreach r [lsort [array names cr]] { puts "CRCELL \$r [set cr(\$r)]" }
TCL
  vivado -nolog -nojournal -mode batch -source "${BUILD}/cr.tcl" 2>&1 | grep -E "^CRTOTAL|^CRCELL" | head -70
else echo "  no routed dcp found"; fi
echo "### DONE $(date)"
