#!/usr/bin/env bash
# Source this on the build machine: `source scripts/env.sh`
# Copy to scripts/env.local.sh and edit the three paths; env.local.sh is git-ignored.
# Values below are the ones recorded in the build logs (see docs/ENVIRONMENT.md).
if [ -f "$(dirname "${BASH_SOURCE[0]}")/env.local.sh" ]; then
  source "$(dirname "${BASH_SOURCE[0]}")/env.local.sh"; return 0 2>/dev/null || exit 0
fi
: "${XILINX_ROOT:?set XILINX_ROOT to the Vitis install root containing 2025.1/}"
source "$XILINX_ROOT/2025.1/Vitis/settings64.sh"
source "${XRT_SETUP:-/opt/xilinx/xrt/setup.sh}" >/dev/null 2>&1 || true
export PLATFORM_REPO_PATHS="${PLATFORM_REPO_PATHS:-/opt/xilinx/platforms}"
export PLATFORM=xilinx_u50_gen3x16_xdma_5_202210_1
export PART=xcu50-fsvh2104-2-e
# v++ -t hw/hw_emu needs a license: export LM_LICENSE_FILE=<port@server> yourself.
