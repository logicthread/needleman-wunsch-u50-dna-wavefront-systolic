# nw-fpga-u50 -- Needleman-Wunsch (global, linear gap) accelerator for the Alveo U50.
# Build output goes to build/.  `make help` lists targets.
#
# STATUS OF EACH TARGET (see docs/ENVIRONMENT.md and docs/WORKLOG.md):
#   verify  laptop-only, see docs/VERIFICATION.md (VERIFIED on macOS).
#   test    runs on any machine with clang + python3 + the ap_int headers (VERIFIED on macOS).
#   sw_emu / hw_emu / hw / host / run / bench need Vitis 2025.1 + XRT (+ a U50 card for
#           run/bench). They are transcribed from the scripts that produced the measured
#           build and have NOT been executed on the machine that wrote this Makefile
#           (no Vitis there).
SHELL    := /bin/bash
BUILD    ?= build
PE       ?= 3968
TILE_PE  ?= 128
TILES    := $(shell echo $$(( $(PE) / $(TILE_PE) )))
FREQ     ?= 300
PLATFORM ?= xilinx_u50_gen3x16_xdma_5_202210_1
KERNEL   := nw_tile_u50
SRC      := linear/src/kernel_tile_1b_s1t.cpp
# Defines of the measured S1t build. Confirmed against the `v++ -c` command line recorded in
# that build's nw_tile_u50.xo.compile_summary (job 113498): identical macro list.
DEFS     := -D NW_PE=$(PE) -D WV_TILE_PE=$(TILE_PE) -D WV_TRACEBACK -D WV_DIR_B3 -D WV_SEAM_RELAY \
            -D WV_QDEPTH=65536 -D WV_RDEPTH=65536 -D WV_BDEPTH=1048576 -D WV_DIRDEPTH=8388608
# Bench arguments of the 181 kbp measurement (INFLIGHT.md, S1t entry; ledger row 35a63b96).
BENCH_ARGS ?= --check-bands 2 --check-n 20000 --perf-bands 46 --perf-n 181019 --reps 3
PBLOCK   := $(if $(filter 3968,$(PE)),$(CURDIR)/linear/cfg/pblock_3968c.tcl,)
XCLBIN   ?= $(BUILD)/hw/$(KERNEL).xclbin

.PHONY: help test verify sw_emu hw_emu hw host run bench clean
help:
	@echo "targets: test | verify | sw_emu | hw_emu | hw | host | run | bench | clean"
	@echo "vars: PE=$(PE) TILE_PE=$(TILE_PE) (tiles=$(TILES)) FREQ=$(FREQ) PLATFORM=$(PLATFORM)"
	@echo "      AP_INT_INC=<.../HLS_arbitrary_Precision_Types/include> is required by 'test'"
	@echo "emulation needs a small array, e.g.  make sw_emu PE=512"

# ---- local, no Vitis: clang csim of the on-card walker ----
test:
	@test -n "$(AP_INT_INC)" || { echo "set AP_INT_INC (see docs/ENVIRONMENT.md)"; exit 2; }
	AP_INT_INC=$(AP_INT_INC) bash linear/scripts/walk_local.sh 512

# ---- full laptop verification (docs/VERIFICATION.md): needs AP_INT_INC and PY=<python with biopython>; ~21 min (QUICK=1: ~2 min)
verify:
	linear/verify/verify_all.sh

# ---- Vitis flows (run `source scripts/env.sh` first) ----
$(BUILD)/%/link.cfg:
	@mkdir -p $(dir $@)
	python3 linear/tools/gen_link_cfg.py --tiles $(TILES) $(if $(PBLOCK),--pblock $(PBLOCK)) > $@

define VPP_FLOW
$(BUILD)/$(1)/$(KERNEL).xclbin: $(SRC) $(BUILD)/$(1)/link.cfg
	v++ -c -t $(1) --platform $(PLATFORM) -k $(KERNEL) -o $(BUILD)/$(1)/$(KERNEL).xo $(SRC) \
	    --advanced.prop kernel.$(KERNEL).kernel_flags="-std=c++14" -I linear/src $(DEFS) \
	    --kernel_frequency $(FREQ) --temp_dir $(BUILD)/$(1)/_x --log_dir $(BUILD)/$(1)/logs \
	    --report_dir $(BUILD)/$(1)/reports
	v++ -l -t $(1) --save-temps --platform $(PLATFORM) -o $$@ $(BUILD)/$(1)/$(KERNEL).xo \
	    --config $(BUILD)/$(1)/link.cfg --kernel_frequency $(FREQ) \
	    --temp_dir $(BUILD)/$(1)/_xl --log_dir $(BUILD)/$(1)/logs_l --report_dir $(BUILD)/$(1)/reports_l
endef
$(eval $(call VPP_FLOW,sw_emu))
$(eval $(call VPP_FLOW,hw_emu))
$(eval $(call VPP_FLOW,hw))
sw_emu: $(BUILD)/sw_emu/$(KERNEL).xclbin
hw_emu: $(BUILD)/hw_emu/$(KERNEL).xclbin
# hw at PE=3968: ~5 h of place&route (NOTES N81: one run exceeded a 6 h limit). v++ auto-scales
# the kernel clock instead of failing: ALWAYS read AUTO-FREQ-SCALING from the log.
hw: $(BUILD)/hw/$(KERNEL).xclbin
	@grep -rhoE "automatically changed to [0-9.]+ MHz|AUTO-FREQ-SCALING.*" $(BUILD)/hw/logs_l | sort -u || true

host:
	PE=$(PE) OUT_DIR=$(CURDIR)/$(BUILD)/host bash linear/scripts/build_host.sh

# run: 2-band correctness check on the card.  bench: the 181 kbp measurement (3 reps).
run: host
	cd $(BUILD)/host && ./host_$(PE) $(abspath $(XCLBIN)) --check-bands 2 --check-n 20000
bench: host
	cd $(BUILD)/host && ./host_$(PE) $(abspath $(XCLBIN)) $(BENCH_ARGS)

clean:
	$(RM) -r $(BUILD)
