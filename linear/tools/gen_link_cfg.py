#!/usr/bin/env python3
"""Generate the v++ link .cfg for the linear (NW, S1t) kernel.

For TILES=31 the [connectivity] block is byte-identical to the one used for the
measured build (linear/cfg/tile_link_tb3968_1bt.cfg); smaller TILES give a
reduced design for emulation. mem spans HBM[0:TILES]: it must READ every direction
buffer for the on-card traceback (S1t).  Budget: TILES dir ports + mem <= 32 AXI masters.
"""
import argparse
ap = argparse.ArgumentParser()
ap.add_argument("--tiles", type=int, default=31)
ap.add_argument("--pblock", default="", help="optional pblock .tcl for OPT_DESIGN.TCL.PRE")
a = ap.parse_args()
assert 1 <= a.tiles <= 31, "31 dir ports + mem = 32 AXI masters (the shell claims 1 of 33)"
print("[connectivity]")
print("nk=nw_tile_u50:1:nw_tile_u50_1")
print(f"sp=nw_tile_u50_1.mem:HBM[0:{a.tiles}]")
for i in range(a.tiles):
    print(f"sp=nw_tile_u50_1.d{i:02d}:HBM[{i}]")
if a.pblock:
    print("\n[vivado]")
    print(f"prop=run.impl_1.STEPS.OPT_DESIGN.TCL.PRE={a.pblock}")
