# pblock_3968c.tcl -- S1c (kernel_tile_1b_s1c.cpp, hls::task ring) variant of pblock_3968.tcl.
# Tiles are hls::task instances of wv_tile_t; seam relays are wv_relay_t<link|qblk|cfg>, 3 chains x WV_SEAM_STAGES.
# 3968 PE across both dies, with the seam registers parked ON the seam.
# Injected via run.impl_1.STEPS.OPT_DESIGN.TCL.PRE (the only method this flow honours).
#
# WHY SPREADING IS MANDATORY HERE, unlike at 2048: at 3072 SLR0 reached 95.80% CLB
# (CLBM 96.63%) with SLR1 at 19.66% and SLL flat at 3,467 -- the placer crammed every
# extra PE into SLR0 rather than pay the seam, and that cost 0.096 ns of slack. 3968
# does not fit one die. At 2048 the same split LOST (300 -> 265.8 MHz) because SLR0 was
# only 70% full, so tripling SLL bought nothing. At 3968 there is no choice.
#
# TWO CONSTRAINTS ONLY, deliberately:
#   1. tiles 16..30 -> SLR1
#   2. the seam relay registers -> the two clock-region rows straddling the seam
# Everything else floats. The dir writers are left UNCONSTRAINED on purpose: pinning
# them is the mistake that produced 11,241 SLLs at 2048, when each moved tile dragged a
# full AXI master across. Let the placer trade dir-FIFO crossings against AXI crossings
# itself, and report where they landed.
#
# =====================================================================================
# NEVER BUILD A CELL NAME FROM A LOGICAL TILE INDEX. This cost job 106219 (1 h 25 m).
# HLS's numeric suffix is its OWN global instance counter, not the logical tile index.
# At 3968 WITH the seam relay the 31 instances are named:
#       wv_tile_U0 (unnumbered), wv_tile_1_U0 .. wv_tile_16_U0,
#       wv_tile_21_U0 .. wv_tile_34_U0          <- 17..20 consumed by other instances
# so tile 0 is unnumbered and tiles 17..30 are shifted by +4. Asking for
# `wv_tile_17_U0` aborted the run. It worked at 3072 only because without the relay the
# numbering happened to be contiguous.
#
# The fix: collect by WILDCARD, assert the count, then sort by the numeric suffix. The
# suffix order IS monotonic in logical order (verified against the .xo netlist of
# 106219: suffixes sort to 0,1..16,21..34 = logical tiles 0..30), so position in the
# sorted list is the logical index. Verify the printed mapping in the log.
# =====================================================================================

set WV_TILES   31
set WV_SEAM_AT 16
set WV_SEAM_STAGES 3   ;# must match the kernel's WV_SEAM_STAGES

# WV_SEAM_AT MUST match the compile-time constant baked into the .xo, because that is
# where wv_linkrelay/wv_qrelay physically sit. Splitting the dies anywhere else would
# leave an UNREGISTERED crossing at the real boundary and park the relay chain in the
# middle of SLR1 -- the opposite of the intent. Changing it means rebuilding the .xo.

puts "### pblock_3968.tcl: tiles ${WV_SEAM_AT}..[expr {$WV_TILES - 1}] -> SLR1, seam relays -> seam rows"

# ---- 1. tiles WV_SEAM_AT..WV_TILES-1 into SLR1 --------------------------------------
# Permissive glob, then an ANCHORED Tcl regexp below does the real filtering -- this
# avoids depending on get_cells -regexp semantics.
set cand [get_cells -quiet -hier -filter {NAME =~ *wv_tile_t*_U0}]
if {[llength $cand] == 0} {
    error "FLOORPLAN ABORT: no wv_tile*_U0 cells at all. Either wv_tile lost its\
`#pragma HLS INLINE off` (then there is no per-tile cell to place), or the hierarchy\
changed. A pblock matching nothing is a SILENT no-op that would be reported as a\
floorplan result."
}

# Keep only the tile process instances and pair each with its numeric suffix.
set pairs {}
foreach c $cand {
    set nm [get_property NAME $c]
    # Two explicit patterns rather than one optional group: tile 0 is UNNUMBERED.
    if {[regexp {wv_tile_t_([0-9]+)_U0$} $nm -> sfx]} {
        lappend pairs [list $sfx $nm]
    } elseif {[regexp {wv_tile_t_U0$} $nm]} {
        lappend pairs [list 0 $nm]
    }
}
set pairs [lsort -integer -index 0 $pairs]

if {[llength $pairs] != $WV_TILES} {
    set found {}
    foreach p $pairs { lappend found [lindex $p 0] }
    error "FLOORPLAN ABORT: matched [llength $pairs] tile instances, expected $WV_TILES.\
The suffixes found were: [join $found ,]. A wrong count means the sorted position is NOT\
the logical tile index and the split would be silently wrong."
}

# Suffixes must be unique, or two logical tiles collapsed onto one sorted slot.
set sfxs {}
foreach p $pairs { lappend sfxs [lindex $p 0] }
if {[llength [lsort -unique $sfxs]] != $WV_TILES} {
    error "FLOORPLAN ABORT: duplicate tile suffixes ($sfxs). Cannot derive logical order."
}

puts "### tile instance map (sorted suffix -> logical index):"
for {set k 0} {$k < $WV_TILES} {incr k} {
    set die [expr {$k < $WV_SEAM_AT ? "SLR0" : "SLR1"}]
    puts "###   logical $k -> [lindex [lindex $pairs $k] 1]  ($die)"
}

set tcells {}
for {set k $WV_SEAM_AT} {$k < $WV_TILES} {incr k} {
    lappend tcells [lindex [lindex $pairs $k] 1]
}
set want [expr {$WV_TILES - $WV_SEAM_AT}]
if {[llength $tcells] != $want} {
    error "FLOORPLAN ABORT: selected [llength $tcells] tiles for SLR1, expected $want"
}

create_pblock pb_tiles_slr1
resize_pblock pb_tiles_slr1 -add {CLOCKREGION_X0Y4:CLOCKREGION_X7Y7}
add_cells_to_pblock pb_tiles_slr1 $tcells
puts "### pb_tiles_slr1: [llength $tcells] tiles pinned to SLR1 (Y4-Y7)"

# ---- 2. seam relays onto the seam rows ---------------------------------------------
# Laguna TX_REG/RX_REG are chosen by the PLACER, not instantiated; it can only use them
# when the crossing is register-to-register with no logic between AND those registers
# sit at the seam. The relay chain supplies the first condition, this pblock the second.
# Every build so far has used 0 of 23,040 possible Laguna registers.
# SAME TRAP AS THE TILES (job 106799). I looked for `grp_wv_linkrelay_fu_<N>`, a form
# that DOES NOT EXIST in this RTL -- so the guard aborted, correctly. The real names are
# DATAFLOW process instances, and as with the tiles the FIRST instance of each kind is
# UNNUMBERED:
#       wv_linkrelay_U0, wv_linkrelay_17_U0, wv_linkrelay_19_U0
#       wv_qrelay_U0,    wv_qrelay_18_U0,    wv_qrelay_20_U0
# (numbers 17..20 are exactly the four missing from the tile sequence.)
# Permissive glob, anchored Tcl regexp, then assert the count.
# S1c (job 108524): the relays are hls::task wrappers around out.write(in.read()) -- no
# data register, only a 1-bit FSM -- and Vivado synth (flatten_hierarchy rebuilt)
# DISSOLVED them, so the old anchored `wv_relay_t*_U0$` hier-cell match found 0 and the
# guard aborted. The registers that actually carry the crossing are the FIFOs between the
# relays: lsr*_U (link), qsr*_i_U (query), csr*_i_U (cfg). So match LEAF primitives by
# hierarchical-name prefix -- that works whether or not a module kept its hierarchy --
# and assert every expected prefix is present.
set seam_re {/KPN_U0/((wv_relay_t_[A-Za-z0-9_]*_U0)|([lqc]sr[A-Za-z0-9_]*_U))/}
set rleaf {}
set rpfx {}
foreach c [get_cells -quiet -hier -filter {IS_PRIMITIVE && (NAME =~ *wv_relay_t_* || NAME =~ *sr*_U/*)}] {
    set nm [get_property NAME $c]
    if {[regexp $seam_re $nm -> pf]} { lappend rleaf $c; lappend rpfx $pf }
}
set rpfx [lsort -unique $rpfx]
set nrel 0; set nfifo 0
foreach pf $rpfx { if {[string match wv_relay_t_* $pf]} { incr nrel } else { incr nfifo } }
puts "### seam leaf cells: [llength $rleaf]  relay prefixes: $nrel  seam FIFO prefixes: $nfifo"
foreach pf $rpfx { puts "###   $pf" }
# WV_SEAM_STAGES stages on each of the three crossing chains (link + qblk + cfg).
set want_r [expr {3 * $WV_SEAM_STAGES}]
if {$nfifo == 0} {
    error "FLOORPLAN ABORT: no seam FIFO (lsr/qsr/csr) cells. Either the design was\
built WITHOUT -DWV_SEAM_RELAY, or WV_TILES <= 16 so the relay guard compiled out. Either\
way the extra seam cycles are NOT in this design and the build would misreport."
}
if {$nfifo != $want_r} {
    error "FLOORPLAN ABORT: matched $nfifo seam FIFOs, expected $want_r\
(= 3 chains x $WV_SEAM_STAGES stages): $rpfx. A partial match would pin only some of the\
chain to the seam and leave the rest to float, which is worse than not pinning at all."
}
create_pblock pb_seam
# Y3 is the last SLR0 row and Y4 the first SLR1 row, so this straddles the Laguna seam.
resize_pblock pb_seam -add {CLOCKREGION_X0Y3:CLOCKREGION_X7Y4}
add_cells_to_pblock pb_seam $rleaf
puts "### pb_seam: [llength $rleaf] leaf cells ($nfifo FIFOs, $nrel relay FSMs) on the seam rows (Y3-Y4)"

puts "### pblocks now: [llength [get_pblocks]]"
