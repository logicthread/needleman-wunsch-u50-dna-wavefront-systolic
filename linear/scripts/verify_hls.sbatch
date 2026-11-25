#!/usr/bin/env bash
#SBATCH --job-name=s1tt
#SBATCH --partition=debug
#SBATCH --cpus-per-task=4
#SBATCH --mem=24G
#SBATCH --time=04:00:00
#SBATCH --output=s1tw/t_%j.log
# S1t verification: csim 512, csim 3968 seam, walker-only csynth+cosim, whole-kernel csynth+cosim.
cd "$SLURM_SUBMIT_DIR" || exit 1   # submit from the repo root
source scripts/env.sh >/dev/null
W=${SCRATCH_DIR:-${TMPDIR:-/tmp}}/s1t_$SLURM_JOB_ID; mkdir -p $W
g(){ grep -h "cases,\|PASSED\|FAILED\|FAIL \|CONFIG ERROR\|ERROR:" "$1" | head -${2:-6}; }
echo "### csim512 $(date +%T)"
vitis-run --mode hls --csim --config hls_s1t_csim512.cfg --work_dir $W/c512 > s1tw/csim512.txt 2>&1; g s1tw/csim512.txt
echo "### csim3968 seam $(date +%T)"
vitis-run --mode hls --csim --config hls_s1t_csim3968.cfg --work_dir $W/c3968 > s1tw/csim3968.txt 2>&1; g s1tw/csim3968.txt
echo "### walker-only csynth+cosim $(date +%T)"
v++ -c --mode hls --config hls_s1t_tbo512.cfg --work_dir $W/tbo > s1tw/tbo_csyn.txt 2>&1; echo "csyn rc=$?"
R=$(find $W/tbo -name csynth.rpt | head -1); [ -n "$R" ] && cp $R s1tw/tbo_csynth.rpt && grep -E "\| *\+ |tb_step|tb_fill" $R | head -12
vitis-run --mode hls --cosim --config hls_s1t_tbo512.cfg --work_dir $W/tbo > s1tw/tbo_cosim.txt 2>&1; g s1tw/tbo_cosim.txt 8
find $W/tbo -name "*cosim.rpt" -exec cp {} s1tw/tbo_cosim.rpt \;
echo "### whole kernel csynth+cosim512 $(date +%T)"
v++ -c --mode hls --config hls_s1t_cosim512.cfg --work_dir $W/co > s1tw/csyn512.txt 2>&1; echo "csyn rc=$?"
R=$(find $W/co -name csynth.rpt | head -1); [ -n "$R" ] && cp $R s1tw/csynth512.rpt && grep -E "^ *\|\+ nw_tile_u50 |\+ wv_tile_t |wv_tb_walk" $R | head -4
vitis-run --mode hls --cosim --config hls_s1t_cosim512.cfg --work_dir $W/co > s1tw/cosim512.txt 2>&1; g s1tw/cosim512.txt
find $W/co -name "*cosim.rpt" -exec cp {} s1tw/cosim512.rpt \;
rm -rf $W
echo "### done $(date +%T)"
