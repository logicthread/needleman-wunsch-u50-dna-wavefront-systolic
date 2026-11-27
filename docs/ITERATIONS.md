# ITERATIONS — every build attempt, what failed, and why

Step 2 of the project record. Everything below is reconstructed from the original workspace
(read-only) and the agent transcripts; nothing was re-run. Each number carries its source;
`UNKNOWN` says what would settle it.

## How to read this

**Sources** (the evidence folder is private and not in this repository; `results/` is):

| Tag | What it is |
|---|---|
| `LOG <job>` | Slurm job log `tblink_<job>.log` — written by the link script: header (job id, start date, `PE/TILE_PE/TILES/FREQ/CFG`, source md5), the csim gate result, the full `v++` output, the end marker. |
| `RPT <build>` | Vivado reports harvested from the build directory: `hw_bb_locked_timing_summary_routed.rpt` (Intra Clock Table), `kernel_util_routed.rpt`, `slr_util_routed.rpt`. |
| `N<k>` | Section *k* of the agent's running notes (`NOTES.md`, 83 sections N1…N82 + N18b). Notes are the agent's own claims: they are cited, and where a file disagrees the file wins (see [Where files and notes disagree](#where-files-and-notes-disagree)). |
| `LEDGER <gate id>` | Row of `results/ledgers/linear.csv` (in this repo): one hardware-gate result. |
| `T db8391ab:<line>` | A user message in the transcripts. Transcript timestamps are **UTC**; log timestamps are **IST** (UTC+5:30). |

**Conventions.** A "build attempt" is one `v++ -l` link job (the expensive step, 1–6 h of
place-and-route), 47 in total: 5 from the earlier bit-parallel (Myers) kernel, 40 link jobs of
the systolic-wavefront kernel and 2 `--reuse_impl` recovery jobs. There was **no git history**
in the original workspace, so the "commit" column is the **job id + the md5 of the kernel
source**, which the link script printed into every log. Kernel clock = clock id 0 line of
`AUTO-FREQ-SCALING`. WNS is the `clk_kernel_00` row of the Intra Clock Table
(failing endpoints / total endpoints). "Peak" is arithmetic, PE × clock; measured numbers are
tagged **[M]** and come from the card, projected ones **[E]**.

**Two traps in reading any build row** (both cost the project real time, see rows 1, 8, 12–16 and 40–45):

1. `v++` does not fail a build that misses timing: it lowers the kernel clock and writes an
   xclbin. "Built" therefore means nothing about speed; only the `AUTO-FREQ-SCALING` line does.
2. Any `--kernel_frequency` other than the platform default (300 MHz) silently made the
   kernel clock constraint **10 ns (100 MHz)** (`N65`). Rows 8, 12, 15, 16 and 33 asked
   for 280/500/350/400/270 MHz and, by that rule, were implemented against a 100 MHz constraint
   (checked in the report for the 500 MHz ask, `N39`; the others follow `N65`). A positive WNS in
   those rows is against 10 ns and says nothing about what the design could do at the asked clock.

---

## 1. Answers to the questions this document was asked

### 1.1 Was the search a grid, or was each step guided by the previous result?

**Mostly guided, by hand, with two small grids. No automatic search harness exists.**

* There is no sweep driver, optimiser or scripted DSE in the archive: `tools/` holds a
  generator for the S1c kernel variants, a doc generator and a watcher template
  (`tools/gen_s1c.py`, `gen_history.py`, `gen_walkthrough.py`, `watch_s1c_template.sh`), and
  the Slurm scripts `tb_link.sbatch`/`g*.sbatch` take one configuration per submission.
  Absence of a search tool in the archive is evidence, not proof.
* **Grid 1 — clock ask on one design (rows 11, 12, 15, 16):** 2048 PE, same source (md5
  `af6762cf`), asked 300/500/350/400 MHz. The 350 and 400 MHz jobs were submitted in the same minute
  (rows 15–16) and the whole batch of asks was explained *afterwards* (`N39`, then reversed by
  `N43`/`N65`, see 2.2). It was a grid in shape but it was run as a probe, not as a design.
* **Grid 2 — S1c seam position × fan-out directive (rows 40–43):** seam at 16 or 10 tiles ×
  with/without `AggressiveFanoutOpt`. Rows 42/43 were submitted in the same minute.
* **Everything else is a chain: result → diagnosis → next change.** Examples that can be
  followed in the notes: 2048 clock collapse → one 8.452 ns path from a star-shaped query
  loader → chain relay (row 8 → 11); 3072 routes but SLR0 is 95.8 % full → try an SLR1 split on
  2048 (row 19, loses) → seam relays (row 29) → remove the pblock (row 30) → try a placer
  directive (row 31, worse) → 160-wide tile (row 32, unroutable) → base-3 at 3968
  (row 33) → boundary on chip (S1, row 37) → continuous bands (S1c, rows 40+) → on-card traceback (S1t, row 45).
* The *parameters* that were varied (PE count 512→2048→3072→3968→4800, tile width 128/160,
  seam position 16/10, packing 2-bit/base-3, SLR pblock yes/no) were chosen one at a time. The
  user's standing instruction to scale "512, then 1024, then 2048, then 4096"
  (session summary, `T db8391ab:20`) set the sequence of PE counts.

### 1.2 How long did each build take?

Per-row wall time is in the tables (job start to job end in the log). Totals:

* **40 link jobs** plus 2 recovery jobs. The 35 jobs whose end time can be read from the log
  (marked ~ where it is the last Vivado timestamp rather than an end marker) add up to
  **≈ 91 h of cluster wall-clock** (jobs overlapped, so elapsed calendar time is shorter).
  The other five died within minutes.
* A successful 3968-PE link is **≈ 3–6 h**: HLS compile 30–40 min (`LOG 108331`: 33 min 33 s;
  `LOG 113498`: 40 min 26 s), then synthesis, placement, routing and `write_bitstream`.
  2048-PE links: 1 h 51 min – 2 h 55 min. A 512-PE link: 1 h 19 min.
* Outcomes of the 40 link jobs: **15 produced an xclbin**, 12 died before place-and-route
  (cfg/tool/quota/OOM/floorplan-guard errors), 9 died *in* place-and-route or at
  `write_bitstream` (7 of these after 3–6 h of work), 4 were cancelled. The S1t xclbin is the
  16th: its link job was killed by the time limit and the xclbin was recovered by two extra jobs (rows 46–47).
* Calendar: link jobs span 2026-10-09 → 2026-10-30; the five earlier Myers links
  2026-10-06/07.

### 1.3 What correctness check did each build have?

Three tiers, and the tier must be read with the row:

1. **Pre-link C simulation gate** (every link job from row 6 on): the Slurm script ran the
   tile traceback testbench in Vitis HLS csim before compiling. Logged result: **18 cases,
   0 failures** for rows 6–37, **23 cases** for rows 38–45 (`LOG <job>`, line after
   `### [0/2] CSIM GATE`). Jobs that reuse an existing `.xo` skipped it (rows 7, 36, 42, 43).
   csim is untimed and runs dataflow processes in program order; it **cannot see** deadlock,
   multi-master AXI ordering or schedule errors (`N29`, `N38`, `N53`, `N66`).
2. **C/RTL cosimulation** of the 512-PE (and for some builds 3072/3968 whole-kernel)
   configuration: cosim 512 5/5 for S1c incl. 3 continuous bands (`N69`), whole-kernel cosim
   5/5 and walker cosim 4/4 for S1t (`N80`). Cosim does not model DRAM throughput or two
   masters aliasing one buffer (`N55`, `N66`).
3. **On the card**, through the course gate service: only the builds in `LEDGER`. Each ledger
   row says whether the score and the traceback path were checked against a CPU reference
   (`score_ok`, `path_ok`). The ledger holds **34 hardware rows**; builds without rows were never run on the card.

For the Myers-era builds correctness was 66.7 M cells against a row-major golden model
(`N7`); no Myers build ever ran on the card.

### 1.4 Iterations that looked successful but were wrong

| # | What looked fine | What was wrong | How it surfaced | Cost | Evidence |
|---|---|---|---|---|---|
| a | Myers v3 "timing MET at 450 MHz, WNS +0.055 ns, 1,791 GCUPS" (first write-up) | Kernel ran at 108 MHz; the +0.055 was the static shell's black-box report; "450 MHz" was `hbm_aclk` | `AUTO-FREQ-SCALING-04 ... 108.0 MHz` read from the log; claim retracted in `N18` | one false headline, ~4× overstated | `LOG v3hw_103762`: "automatically changed to 108.0 MHz" |
| b | Rows 12, 15, 16 (2048 at 500/350/400): xclbin produced, csim/cosim pass, positive WNS | Implemented against a 10 ns constraint; ran at 106–108 MHz | AUTO-FREQ lines; explained only later (`N43`, `N65`) | three 2-hour links used to "learn" a flag behaviour; `N39` built a theory on them | LOGs 105270/105276/105277; `N65` |
| c | Row 18 (3072 PE, 291.6 MHz, ~862 GCUPS [E]) | **Multi-band answers wrong on silicon**, non-deterministically (a RAW race across two m_axi masters); also 8-band perf run hung | first card run, ledger rows `1bc30b7f`; root cause only after 5 card runs (`N66`) | the whole `kernel_tile.cpp` family retired for multi-band; "862 GCUPS" never became a valid number | `LEDGER 1bc30b7f` "score -4869 vs -15742; band 1 garbage"; `LEDGER 164da747` |
| d | Rows 18/11 passed csim 18/18 and cosim | The race is invisible to both | same | — | `N29` ("COSIM CANNOT MODEL IT"), `N66` |
| e | `N59`: "EVERY LINK EVER BUILT IS 2-BIT, NOT BASE-3" | False for the 3968 xclbin, which is base-3 (31 `pk_/ob0/ob1` RTL files; raw dump decodes 100.000 % valid under base-3) | hardware dump decoded under both layouts | one wrong design conclusion + a wrong bandwidth table | `N64`; `LEDGER e96b954e` |
| f | `gsh4096_106835` printed "4096 PE / 16 CHANNELS / 17 AXI VERIFIED" | The cosim had failed (disk quota); the gate `grep -q "TILE TRACEBACK PASSED"` matched C-side output printed *before* RTL simulation starts | raw log read by hand | a verdict gate that lied; replaced by a four-condition gate | `N54` |
| g | Ledger rows with `status=completed` | `score_ok=0` (`1bc30b7f` check, `fd23cd3e` C/D, `164da747` J) — a "completed" run is not a correct run | read score columns | — | `results/ledgers/linear.csv` lines 2, 8, 9, 24 |
| h | `963947c1` (3968): `status=completed`, score correct | Traceback path wrong: the **host** decoded base-3 as 2-bit | decoding the raw direction dump offline | — | `LEDGER 963947c1`, `e96b954e`, `225dd517` |
| i | Row 44 (S1c2): route-intermediate WNS −0.803 "looked like ~218 MHz" | Final routed WNS −3.290 → 153.0 MHz | final Intra Clock Table | one more S1c variant | `N77`; `RPT s1c2` |
| j | `csynth` numbers: "Final II = 1", 1,929 GCUPS (Myers v3), "134 % of the device" (3072) | cosim measured 2.2 cycles/column; real clock 108 MHz (real GCUPS ≈ 430); placed LUT was 21.2 % | cosim, then place-and-route | repeated: `N16`, `N18`, `N42` | `N16`, `N18`, `N42` |
| k | `N64` read the 107868 routed kernel WNS −0.172 ns as "would scale ~258 MHz" | `N65` found the constraint was 10 ns, not 3.7 ns; the raw `repair/timing_after.rpt` shows a **10.000 ns** period with WNS −0.169 (8 endpoints). What this implies for the true achievable clock is **UNKNOWN** — settled only by re-running with `--clock.default_freqhz` (`N65`) | — | wasted interpretation | `N64`, `N65`, `link_tb_3968_270_b3/repair/timing_after.rpt` |

### 1.5 Which decisions were the user's, which the agent's?

The transcripts begin 2026-10-09T22:53Z (a compaction summary); the Myers-era design decisions
(before ~2026-10-09) are **UNKNOWN** in who-decided terms — settled only by the earlier
session transcript, which is not in the archive. From 2026-10-09 on, the user's messages are
available and the table gives each fork.

| When (UTC) | Decision | Who | Evidence | Rows |
|---|---|---|---|---|
| Oct 9 | "pull both sequences within URAM"; "directions to HBM, not post-wave score"; "I want the traceback as well. build with 4x128 PE version first" | **User** | session summary `T db8391ab:20` | 6–11 |
| Oct 9 | "send for 500MHz build to see highest possible clock" | **User** | same | 12 |
| Oct 9 | try 2048 @280, then "cancel it and send at 300MHz" | **User** | same | 8–11 |
| Oct 10 05:13 | "try spreading across both dies in 2048, build @300MHz, measure slack" | **User** | `T db8391ab:448` | 19 (started 10:47 IST) |
| Oct 10 05:19 | "try a softcore as well which forces even CLB slices per PE, don't tell me what's the norm ... just do it" | **User** | `T db8391ab:504` | 20, 21, 23, 24 |
| Oct 10 08:12 | "build 4096@300MHz, adding extra cycles and laguna registers between tiles crossing the seam, keep SLR0 uncongested" | **User** | `T db8391ab:736` | 22, 25–29 |
| Oct 12 09:38–09:59 | "why am i using 37 ports?" → "why can't these all use a single HBM channel to burst write into URAM and let the 4096 still use 32?" | **User** (design proposal) | `T db8391ab:1067`, `:1112` | single-bundle design, row 25 |
| Oct 12 10:17–10:23 | "what about 5 directions in 8 bits?" → "implement 5-in-8" | **User** (idea) → agent implemented | `T db8391ab:1140`, `:1157`; `N44` | base-3 packing |
| Oct 12 20:11–20:14 | "should i keep tiles as a multiple of 5 ... what about 160" | **User** | `T db8391ab:2715`, `:2739`; `N58` | 160-PE ladder (csynth only, `N58`) |
| Oct 13 00:45 | "can you decouple tile to channel ... like an aligned packing FIFO?" | **User** (idea) | `T db8391ab:3688`; `N53` | `WV_SHARE` |
| Oct 13 07:43 | "go, launch the no-pblock 3968 link" | **Agent proposed** (`N55`: "one line removed from the cfg"), user approved | `T db8391ab:4241` | 30 |
| Oct 13 11:49 | "go, launch the SSI_BalanceSLRs attempt" | **Agent proposed** (`N56`), user approved | `T db8391ab:4481` | 31 |
| Oct 13 17:38 | "send 160x30 for final build @250MHz" | **User** | `T db8391ab:5125` | 32 |
| Oct 18 22:15 | "go ahead with 3968 base-3 link" (ask value 270 MHz was the agent's) | **User** go; ask **UNKNOWN who** | `T db8391ab:6262` | 33 |
| Oct 19 14:44 | "traceback is a primary requirement, always keep traceback ... estimate for 181 kbp so i dont look like a fool saying 860 GCUPS when actually achieved is 300" | **User** (standing rule) | `T db8391ab:8563` | 34–47 |
| Oct 19 14:49 | "go ahead with S1, keep traceback, send for link at 300MHz, then test on hardware" | **User** | `T db8391ab:8588` | 34–37 |
| Oct 21 04:45–05:46 | "what is blocking you to eliminate fill and drain per band?" → "keep the band boundary value in bram" → "go ahead" | **User** asked; **agent** designed S1c | `T db8391ab:9708`, `:9764`, `:9772`; `N69`–`N70` | 38–44 |
| Oct 21 19:21 | "keep only 10 tiles on SLR0 in the next build, try to max out SLR1, no performance benefit in symmetry" | **User** — measured **worse** (705.5 vs 822.8 GCUPS) | `T db8391ab:11377`; `LEDGER 538c0166` | 41 |
| Oct 22 (no user message 00:51→13:24 IST) | fan-out relinks 108753/108754; PKFRESH variant 108763 | **Agent** (standing "keep going" from Oct 20 21:01Z `T db8391ab:9384`) | `N75`, `N76`; job start times | 42–44 |
| Oct 22 10:52 | "stop further iteration until further instruction" | **User** | `T db8391ab:11939` | — |
| Oct 22 19:32 | "clean up your builds from the node ... preserve what is not regenerateable on local" | **User** | `T db8391ab:11999` | cleanup |
| Oct 30 10:13 | "put the traceback on card instead of host, keep the band boundary in BRAM/URAM/HBM" ; base design = **S1c** | **User**, S1c base "user's choice over S1" (`N79`) | `T db8391ab:12403` | 45–47 |
| Oct 30 | `v++ --reuse_impl` recovery after the 6 h timeout; `--temp_dir` must be the original | **Agent** | `N81`, `N82` | 46, 47 |

Throughout, the agent chose and ran the debugging between jobs (all diagnoses in the *Error →
root cause* column below are the agent's), and the user steered by (a) directions, (b) questions
that exposed a gap ("why 37 ports?" led to the 32-AXI budget, `N45`–`N49`), (c) stop/start.

---

## 2. Build attempts

### 2.1 Earlier kernel: Myers bit-parallel (5 hardware links, none on the card)

Source `kernel_nw3.cpp` (`src` md5 not logged for these). Config: 16 lanes × 8 blocks × W = 32
= 4,096 cell-lanes per cycle ("PEs" here are bit lanes, not systolic PEs). Platform
`xilinx_u50_gen3x16_xdma_5_202210_1`, Vitis 2025.1. Dates are the log's mtime, in IST.

| # | Job | Ask → got (MHz) | Furthest stage · wall | Error (quoted) → root cause | Kernel WNS (fail/total) | Kernel LUT / FF / BRAM / URAM / DSP | SLL | Peak [E] | Correctness |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 103762 · Oct 6 | 450 → **108.0** | xclbin (34.6 MB, `N18b`); 1 h 20 min (compile 3 min 02 s + link 1 h 17 min 10 s) | no error. `[AUTO-FREQ-SCALING-04] ... automatically changed to 108.0 MHz`. One HLS FSM state bit (`ap_CS_fsm_state31`) clock-enables **21,404** pins of the `pq_*` mask register file, 8.8 ns of pure route at 0 logic levels (`N19`, `N22`) | +0.749 @ 10 ns (0 / 201,007) | 51,972 (7.00 %) / 67,607 / 16 / 0 / 0 | 3,471 (no Laguna regs) | 4,096 × 108 = 442 | csim/cosim only: 66.7 M cells vs a row-major golden (`N7`); cosim 20,000-column batch 1.029 cycles/col after fix (`N16`) |
| 2 | 103831 · Oct 7 | 450 → 111.8 | xclbin; 1 h 51 min | one CU pinned per SLR (`v3_link_2cu.cfg`): no help — the problem is fan-out, not distance (`N19`, `N26`) | n/a (raw not archived) | n/a | n/a | — | as above |
| 3 | 103899 · Oct 7 | 450 | died in block synthesis, ~3 min | `ERROR: [VPL 8-5809] Error generated from encrypted envelope ... clk_metadata_adapter_v1_0_vl_rfs.v` → `--temp_dir /tmp`: the platform ships two copies of `clk_metadata_adapter`; IP-repo scan order (filesystem order) picks the one that cannot elaborate on `/tmp` but not on the NFS home (`N20`) | — | — | — | — | — |
| 4 | 103905 · Oct 7 | 450 | same | same | — | — | — | — | — |
| 5 | 103907 · Oct 7 | 450 → 108.0 | xclbin; ~1 h | `--optimize 3` + `AggressiveExplore` phys_opt (pre- and post-route): **0.000 ns** improvement; same worst path, same destination register (`N21`) | +0.749 @ 10 ns | — | — | — | — |

Real throughput of the Myers kernel if it had run: 3,980 cells/cycle (cosim) × 108 MHz ≈ **430 GCUPS [E]**, below the 500 target and 4.5× below the csynth-derived 1,929 (`N17`, `N18`).

### 2.2 Systolic-wavefront kernel: all 40 link jobs and the 2 recovery jobs

Config: PE = tiles × PE-per-tile. All use `src/kernel_tile*.cpp` (md5 given), platform as
above. **Src** md5 abbreviated to 8 hex. "Gate" = csim gate result printed in the log.

Legend for the *Correctness* column: **csim 18/0** = pre-link csim gate "18 cases, 0 failures";
**silicon** = a ledger gate.

| # | Job · start (IST) · src | PE | Ask → got MHz | Furthest stage · wall | Error (quoted) → root cause | Kernel WNS (fail/total) | Kernel LUT / FF / BRAM / URAM / DSP | SLL (0→1 / 1→0) · CLB % SLR0/SLR1 | Silicon GCUPS [M] and correctness |
|---|---|---|---|---|---|---|---|---|---|
| 6 | **104866** · Oct 9 12:03 · `67e207c4` kernel_tile.cpp | 512 = 4×128 | 300 | HLS compile OK, link rejected at start · 3 min 27 s | `ERROR: [v++ 82-10318] Both platform and part options cannot be specified for link flow` → link cfg carried `part=` (config error; fixed by removing it, session summary) | — | — | — | csim 18/0 |
| 7 | **104873** · Oct 9 12:09 · `67e207c4` (`.xo` reused: "REUSING existing ... Estimated Fmax 411 MHz") | 512 | 300 → **300** (met) | xclbin · 1 h 19 min | none | +0.189 (0 / 159,206); hbm_aclk +0.108; dma +0.074 | 30,527 (4.14 %) / 39,929 / 45 / 0 / 0 | 3,475 (1,765 / 1,710) · 45.26 / 19.49 | csim 18/0 earlier; cosim 512 (`N34`: 2,779/3,379 cycles = 1.000 cycles/col); not run on card |
| 8 | **104950** · Oct 9 14:14 · `67e207c4` | 2048 = 16×128 | **280** → **112.6** | xclbin · 2 h 55 min | no error; `AUTO-FREQ-SCALING-04 → 112.6`. Root cause per the session summary: query loader in a star topology put an 8.452 ns path on one bus (10/10 failing endpoints) → "2.7× clock cut"; chain relay took it to 3.245 ns for −660 LUT (`T db8391ab:20`). Raw routed report **not archived** (no `link_tb_2048_280`) | n/a | n/a | n/a | not run. ~222 GCUPS [E] |
| 9 | **105168** · Oct 9 21:33 · `af6762cf` | 2048 | 280 | routed, then **cancelled at "Creating bitmap"** (log: "Finished 5th of 6 tasks (FPGA routing)", `CANCELLED AT 2026-10-09T23:32:00`) · 1 h 58 min | `scancel`; consistent with the user's "try 280, then cancel it and send at 300MHz" (`T db8391ab:20`, earlier than the retained transcript) — the actual order of the user's message is **UNKNOWN** (settled by the earlier transcript) | — | — | — | csim 18/0 |
| 10 | **105221** · Oct 9 23:33 · `af6762cf` | 2048 | 280 | cancelled after 2 min 17 s | same | — | — | — | — |
| 11 | **105222** · Oct 9 23:35 · `af6762cf` | 2048 | 300 → **300** (met; 304 available per `N39`) | xclbin · 2 h 39 min | none | **+0.044** (0 / 527,295); hbm_aclk +0.009 (0 / 93,694); dma +0.074 | 102,233 (14.18 %) / 134,904 / 141 / 0 / 0 | 3,477 (1,768 / 1,709) · 70.39 / 19.58 | csim 18/0 · **silicon** `fd23cd3e`: 1 band score + path OK, 217.9 GCUPS at n=20,000, 364.7 at n=65,536 (1 band, not representative); **2 bands wrong even with the true boundary pre-loaded** |
| 12 | **105270** · Oct 10 02:33 · `af6762cf` | 2048 | **500** → 106.1 | xclbin · 2 h 11 min | no error; `AUTO-FREQ-SCALING-04 → 106.1`. Mechanism offered in `N39` (HLS re-synthesis, placer loses its gradient) is **superseded by `N65`**: the 500 ask ran against a 10 ns constraint | +0.580 @ **10 ns** (0 / 545,189) | 111,935 (15.52 %) / 143,975 / 141 / 0 / 0 | 3,480 · 68.50 / 21.72 | not run; peak 217 |
| 13, 14 | **105274**, **105275** · Oct 10 07:25 · `af6762cf` | 2048 | 350, 400 | cancelled after 1 min 27 s | two jobs sharing one `$PWD/.ipcache`; cancelled 2 min in and resubmitted (`N40`) | — | — | — | — |
| 15 | **105276** · Oct 10 07:27 · `af6762cf` | 2048 | **350** → 108.1 | xclbin · 2 h 03 min | `AUTO-FREQ → 108.1` (10 ns constraint, `N65`) | +0.754 @ 10 ns (0 / 527,072) | 102,030 / 134,830 / 141 / 0 / 0 | 3,476 · 80.13 / 19.54 | not run |
| 16 | **105277** · Oct 10 07:27 · `af6762cf` | 2048 | **400** → 108.4 | xclbin · 2 h 05 min | `AUTO-FREQ → 108.4`; hbm_aclk 450 → 433.2 (−0.086, 31 endpoints) | +0.823 @ 10 ns (0 / 527,772) | 104,709 / 135,057 / 141 / 0 / 0 | 3,477 · 70.09 / 19.69 | not run |
| 17 | **105278** · Oct 10 07:28 · `af6762cf` | 3072 = 24×128 | 300 | died in synthesis · ~30 min | `ERROR: [VPL 12-13638] Failed runs(s) : 'ulp_nw_tile_u50_1_0_synth_1'` + `slurmstepd: error: Detected 1 oom-kill event(s) in StepId=105278.batch` → **out-of-memory kill** during kernel synthesis. **Not mentioned in the notes** (found in the log); the job was resubmitted as 105286 | — | — | — | csim 18/0 |
| 18 | **105286** · Oct 10 10:39 · `af6762cf` | 3072 | 300 → **291.6** | xclbin · 2 h 55 min | no error; kernel failed by 0.096 ns on 1,018 endpoints; SLR0 CLB 95.80 % because all 24 dir masters sit at the HBM corner in SLR0 (`N42`) | **−0.096** (1,018 / 774,563); hbm_aclk −0.235 (2,105) → 407.0 | 150,733 (21.21 %) / 198,458 / 205 / 0 / 16 | 3,467 (1,763 / 1,704) · 95.80 / 19.66 | csim 18/0; cosim 2/2 (`N41`) · **silicon** `1bc30b7f`: **2-band score WRONG** (−4869 vs −15742), 8-band run hung → gate refused further runs. "~862 GCUPS [E]" (`N42`) was never realised. Peak 895.8 |
| 19 | **105287** · Oct 10 10:47 · `af6762cf` | 2048, tiles 8–15 forced to SLR1 | 300 → 265.8 | xclbin · 2 h 16 min | no error; seam crossings tripled (3,477 → 11,241 per `N42`) because each moved tile drags a full AXI master across; raw report not archived | −0.428 (6,677 failing) [`N42`] | n/a | 11,241 [`N42`] · 61.55 / 30.97 [`N42`] | not run; 544 peak |
| 20 | **105291** · Oct 10 11:57 · `8161c044` kernel_tile_ns.cpp | 512 (one PE per module) | 300 | immediate · seconds | `ERROR: [v++ 60-602] Source file does not exist: <HOME>/fpga/nw_myers/\` → `\\` escaped in a double-quoted `DEFS`; cosmetic string-escaping bug (`N42`) | — | — | — | — |
| 21 | **105297** · Oct 10 13:27 · `8161c044` | 512 (per-PE pblocks) | 300 | died in placement · ~1 h 16 min (compile 19 min 22 s) | `ERROR: [VPL 30-488] Failed to commit 10 instances: ... NWW_LUT_U/q0[1]_i_1__8 ... No site found for this instance.` then `[VPL 30-99] Placer failed with error: 'failed to commit all instances'`. Root cause **not stated in the notes**; the log says the placer ran out of sites for those LUTs. Inference only: the 512 exclusive per-PE pblocks left too few slices (`N42` computed a floor of 5 slices/PE and gave 8) | — | — | — | csim 18/0 |
| 22 | **105389** · Oct 10 15:30 · `220c9457` | 4096 = 32×128 | 300 | died in block-diagram creation · ~28 min | `ERROR: [VPL-1] You have run out of port connections on /hmss_0. All 33 connections are used (0 ports prohibited)...` (asked 37) → AXI budget, `N45`; the real kernel budget is **32** because the shell claims 1 (`N49`) | — | — | — | csim 18/0 |
| 23 | **105390** · Oct 10 15:34 · `8161c044` | 512 | 300 | died in placement · ~1 h 41 min | `ERROR: [VPL 30-488] Failed to commit 11 instances` → same as row 21 | — | — | — | — |
| 24 | **105630** · Oct 12 14:38 · `8161c044` | 512 (per-PE pblocks, private IP cache) | 300 | placement never finished · 4 h 00 min | `slurmstepd: error: *** JOB 105630 ON <node> CANCELLED AT 2026-10-12T18:38:26 DUE TO TIME LIMIT ***` after "Starting logic placement.." (placement from 15:24); not in notes | — | — | — | — |
| 25 | **105691** · Oct 12 20:50 · `a5478dd3` kernel_tile_1b.cpp | 4096 (single control bundle) | 300 | died in block-diagram creation · ~30 min | `ERROR: [VPL-1] You have run out of port connections on /hmss_0. All 33 connections are used` — I had recorded "33, at the limit" as a pass in the gate; the shell takes one (`N49`: "I recorded 33 connections as a pass. It is not a pass") | — | — | — | csim 18/0 |
| 26 | **105697** · Oct 13 00:01 · `3ba10560` kernel_tile_1b.cpp | 3968 = 31×128 | 300 | link rejected at start · ~27 min (incl. HLS compile) | `ERROR: [v++ 60-1520] ***Exception: The options configuration file: tile_link_tb3968_1b.cfg failed with a parsing error.` — cfg syntax; cause not recorded | — | — | — | csim 18/0 |
| 27 | **106219** · Oct 13 00:31 · `3ba10560` | 3968 | 300 | died at `opt_design` · ~1 h 25 min | `ERROR: [VPL 101-2] FLOORPLAN ABORT: no cell for wv_tile_17_U0` → the floorplan script built cell names from the logical tile index; HLS numbers instances with its own global counter (`wv_tile_U0`, `_1`…`_16`, `_21`…`_34`) (`N50`) | — | — | — | csim 18/0 |
| 28 | **106799** · Oct 13 02:02 · `3ba10560` | 3968 | 300 | died at `opt_design` · ~22 min | `FLOORPLAN ABORT: no wv_linkrelay/wv_qrelay instances ... built WITHOUT -DWV_SEAM_RELAY` — the reused `.xo` lacked the macro | — | — | — | — |
| 29 | **106803** · Oct 13 02:27 · `3ba10560` | 3968 | 300 → **199.0** | xclbin · 2 h 50 min | no error; floorplan (seam relays + pblock) **lost**: HBM is SLR0-only so every moved tile pushes a 512-bit dir beat across the die (SLR1→SLR0 10,269 vs SLR0→SLR1 2,451); `N52` | **−1.692** (230,711 / 1,227,718); hbm_aclk −0.836 → 327.0 | 295,027 (42.09 %) / 353,163 / 323 / 0 / 25 | 12,720 (2,451 / 10,269) · 96.44 / 74.26 | csim 18/0 · **silicon** (base-3, kernel_tile_1b): `963947c1` 2-band score OK, 1 band n=262,144 **625.6 GCUPS** (not representative); `225dd517` score + optimal path PASS; `dd368b3d`: **181 kbp kernel 124.4 ms = 265.5 GCUPS, e2e 202** [M], peak 789.6 |
| 30 | **106854** · Oct 13 13:15 · `47c15e26` | 3968, **no pblock** | 300 | routed, died at `write_bitstream` · ~3 h 15 min | `ERROR: [VPL 101-2] design did not meet timing - ... unscalable system clocks ... dma_ip_axi_aclk_1; slack: -0.012 ns` — **12 ps** on the platform's XDMA clock, which `v++` cannot scale | −0.683 (30,512 failing) [`N56`; routed report not archived; log: post-physopt WNS −0.685] | 191,954 (27.39 %) / 251,212 / 323 / 0 / 21 | 9,066 (4,819 / 4,247) · 98.74 / 26.46 | csim 18/0; no xclbin |
| 31 | **106976** · Oct 13 17:23 · `47c15e26` | 3968, `SSI_BalanceSLRs` | 300 | died at `write_bitstream` · ~2 h 58 min | same error, `dma_ip_axi_aclk_1; slack: -0.117 ns` — the directive was applied but the placer declined to rebalance (SLR0 98.74 → 99.05 %) | −0.928 (67,335) [`N57`] | 192,060 / 251,170 / 323 / 0 / 21 | 9,323 · 99.05 / 26.12 | no xclbin |
| 32 | **107115** · Oct 13 23:11 · `4325e837` | **4800 = 30×160**, base-3 | 250 | placed in 1 h 13 min; router quit · ~4 h 36 min | `ERROR: [VPL 35-3] Design is not routable as its global congestion level is 7.` — both SLRs full at once (99.74 % / 93.66 %) with 11,962 seam crossings; my area estimate was off by 52 % (`N60`) | none produced | 427,792 (60.70 %) placed / 499,891 / 316 / 0 / 25 | 11,962 (2,047 / 9,915) · 99.74 / 93.66 | csim 18/0 |
| 33 | **107868** · Oct 19 07:35 · `4325e837` | 3968, base-3, no pblock | **270** → (no xclbin) | routed, died at `write_bitstream` · ~5 h 55 min | `dma_ip_axi_aclk_1; slack: -0.054 ns` (15 endpoints); repair jobs 107924/107951 (phys_opt + incremental reroute) recovered 0 ps because 12/15 endpoints sat in a locked static pipe (`N65`). Kernel timing at this row is **UNKNOWN**: see 1.4(k) | −0.169 @ 10 ns after repair (8 / 1,132,272) | 278,639 (39.77 %) / 317,534 / 323 / 0 / 25 | 9,497 (5,157 / 4,340) · 99.79 / 25.85 | csim 18/0; no xclbin |
| 34 | **107990** · Oct 19 20:31 · `2600fff5` kernel_tile_1b_s1.cpp (S1) | 3968 | 300 | HLS compile died · minutes | `ERROR: [v++ 60-300] Failed to build kernel(ip) nw_tile_u50 ...` → `disk quota exceeded` during HLS export; the only trace is `autopilot.flow.log` line 16096 (`N65`); shared 20 GB home quota | — | — | — | csim 18/0 |
| 35 | **108022** · Oct 20 00:20 · `2600fff5` | 3968 (S1, no pblock) | 300 | died at end of detail placement · 4 h 06 min | `ERROR: [VPL 30-99] Placer failed with error: 'failed to commit all instances'` / `[VPL 17-69] Command failed: Placer could not place all instances` → hypothesis (`N66`): the same-family 107868 already filled SLR0 to 99.79 %; S1 adds 0.5 % and was asked for 300 MHz. **Not proven.** The DMA-clock hook ran as a no-op (`DMAHOOK: axi_regslice (0 cells)`) | — | S1 synth: 289,424 LUT / 323,601 FF / 296 BRAM [`N66`] | — | csim 18/0 |
| 36 | **108321** · Oct 21 01:29 · `2600fff5` | 3968 (S1 + pblock) | 300 | died at `opt_design` · 23 min | `FLOORPLAN ABORT: no wv_linkrelay/wv_qrelay instances` — the reused `.xo` (from 108022) had no `-DWV_SEAM_RELAY`; I had compared configs, not the `.xo` defines (`N67`) | — | — | — | — |
| 37 | **108331** · Oct 21 02:32 · `2600fff5` | 3968 (**S1**) | 300 → **246.5** | xclbin · 4 h 57 min (compile 33 min 33 s + link 4 h 22 min 21 s) | none. First 3968 build whose DMA clock closed unaided (+0.074). | **−0.723** (64,808 / 1,138,186); hbm_aclk −0.613 → 352.7; dma +0.074 | 281,836 (40.22 %) / 320,678 / 296 / 0 / 25 | 12,375 (2,467 / 9,908) · 95.99 / 65.49 | csim 18/0, cosim 512 incl. 3-band (`N65`) · **silicon** `09bbbe4e`: check score + optimal path PASS; 181 kbp **878.6 GCUPS kernel-only [M], 1 rep**, e2e 474.8, score 81,778 == CPU. Peak 978.1 |
| 38 | **108494** · Oct 21 18:28 · `1cc3ea2e` S1c | 3968 | 300 | HLS compile died · minutes | `ERROR: [v++ 60-300] Failed to build kernel(ip)`; `v++ 60-599` with no `vitis_hls.log` — quota again (4.3 GB of HLS scratch; other session 11 GB) (`N71`) | — | — | — | csim 23/0 |
| 39 | **108524** · Oct 21 19:10 · `1cc3ea2e` | 3968 (S1c) | 300 | died at `opt_design` · 2 h 10 min | `FLOORPLAN ABORT: no wv_relay_t instances` → `hls::task` relays `out.write(in.read())` have no data register; synthesis flattened all 9 away. The `.xo` check was correct but insufficient (`N72`) | — | kernel synth 345,386 LUT / 329,924 FF / 469 BRAM | — | csim 23/0 |
| 40 | **108585** · Oct 21 21:31 · `1cc3ea2e` | 3968 (**S1c** seam16) | 300 → **213.9** | xclbin · 4 h 17 min | no error; worst paths are pure routing, 0 logic levels, 4.1–4.5 ns (`N74`) | **−1.340** (186,909 / 1,197,108); hbm_aclk −0.902 → 320.1 | 339,075 (48.39 %) / 325,433 / 469 / 4 / 26 | 12,302 (2,630 / 9,672) · 98.56 / 74.72 | csim 23/0, cosim 512 5/5 · **silicon** `0bcb86cb`: **822.8 GCUPS [M], 3 reps**, 816.0 on the true 181,019²; e2e 395.8. Peak 848.7 |
| 41 | **108717** · Oct 22 00:53 · `2fce0d0d` S1c seam10 | 3968 | 300 → **180.8** | xclbin · 5 h 47 min | no error; denser SLR1 → worst paths `ap_CS_fsm → R of ob/pk` (~1000 regs), `cs empty_n → cfg CE`, ~5.0 ns route (`N75`) | −2.197 (298,118 / 1,197,352); hbm_aclk −1.156 → 296.0 | 338,900 / 325,560 / 469 / 4 / 26 | 15,186 (2,568 / 12,618) · 94.59 / 89.35 | csim 23/0 · **silicon** `538c0166`: **705.5 GCUPS [M]**; e2e 371.4 |
| 42 | **108753** · Oct 22 07:08 · `1cc3ea2e` S1c seam16 + `AggressiveFanoutOpt` + post-route `AggressiveExplore` | 3968 | 300 → 214.4 | xclbin · 3 h 13 min (`.xo` reused) | no error; knobs gave +0.5 MHz | −1.340 (186,909 / 1,197,108) — **identical** | 339,075 / 325,433 / 469 / 4 / 26 | 12,302 · 98.56 / 74.72 | **silicon** `1686a42a`: 823.5 GCUPS [M]; e2e 405.2 |
| 43 | **108754** · Oct 22 07:08 · `2fce0d0d` seam10 + fan-out | 3968 | 300 → 183.7 | xclbin · 4 h 35 min | no error; no gain | −2.197 — **identical** | 338,900 / 325,560 / 469 / 4 / 26 | 15,186 · 94.59 / 89.35 | **silicon** `ae3392d2`: 706.3 GCUPS [M]; e2e 415.1 |
| 44 | **108763** · Oct 22 12:09 · `c86b5ab8` S1c2 (PKFRESH) | 3968 | 300 → **153.0** | xclbin · 5 h 29 min | no error; csynth was cleaner (tile 27.1K → 25.3K LUT, slack −0.07 → 0.00) but final route **worse**; route-intermediate WNS (−0.803) did not predict the final | **−3.290** (282,493 / 1,168,233); hbm_aclk −1.771 → 255.2 | 319,546 (45.59 %) / 342,815 / 469 / 4 / 26 | 12,121 (2,565 / 9,556) · 99.19 / 65.07 | csim 23/0, cosim 512 5/5 · **silicon** `e083126e`: 588.1 GCUPS [M]; e2e 356.0 |
| 45 | **113498** · Oct 30 16:01 · `128591ff` kernel_tile_1b_s1t.cpp (**S1t**: S1c + on-card walker) | 3968 | 300 → **205.3** | routed; **killed in `write_bitstream`** by the 6 h Slurm limit · 6 h 00 min (compile 40 min 26 s, routing finished 21:47) | `slurmstepd: error: *** JOB 113498 ON <node> CANCELLED AT 2026-10-30T22:02:14 DUE TO TIME LIMIT ***` → `--time=06:00:00` too short for 3968 + walker (`N81`) | **−1.536** (216,887 / 1,209,506); hbm_aclk −1.047 → 305.9 | 342,341 (48.84 %) / 330,498 / 477 / 4 / 30 | 12,189 (2,528 / 9,661) · 99.36 / 70.97 | csim 23/0; whole-kernel cosim 5/5, walker cosim 4/4 (`N80`) |
| 46 | **113715** · Oct 30 22:07 | S1t | — | `v++ --reuse_impl` try 1 · 18 min | `ERROR: [CF2SW 83-1001] Cannot open ... /_x/link/int/address_map.xml` / `[v++ 60-398] cf2sw failed` → fresh `--temp_dir` lacks `link/int/address_map.xml`; must be the original `_xl` (`N82`) | — | — | — | — |
| 47 | **113724** · Oct 30 22:39 | S1t | 205.3 (xclbin `DATA_CLK 205 MHz`) | xclbin (49,419,288 B) · 18 min | none. `v++ --reuse_impl` on the routed checkpoint with `--temp_dir` = original `_xl` | (from row 45) | (from row 45) | (from row 45) | **silicon** `35a63b96`: check — card ops **identical** to host walker (20,002 ops), path == optimum; 181 kbp **803.4 GCUPS kernel-only [M], best of 3 (median 803.0)**, on-card walk 13.8 ms, ops readback 3.4 ms, **e2e 566.9 GCUPS [M]** (host-defined), score 81,778 == CPU. Peak 814.6 |

**Not rows, but build-flow runs on existing netlists:** 107769 re-linked the 3072 build
from its preserved `.xo`: every routed number reproduced to the last digit (`N62`: v++ links are reproducible run to run on this setup); 107924/107951 repaired 107868's routed checkpoint (row 33).

### 2.3 Experiments before the link (HLS / out-of-context / cosim)

The links above were the expensive steps; most design decisions were made earlier by
csynth, cosim and out-of-context (OOC) place-and-route. Chronological, from the notes:

| Step | Experiment | Result | Notes |
|---|---|---|---|
| Myers v1 → v2 → v3 | W 64→32, lanes 8→16, retimed loop-carried path | csynth Fmax 298.95 → 422.30 → 470.81 MHz; LUT 85,925 → 184,305 | `N1`–`N14` |
| Myers v3.1 | Peq load as two 512-bit beats | setup 773 → 296 cycles; area *down* | `N15` |
| Myers cosim | `Final II = 1` yet 44,231 cycles/20,000 cols (2.2/col) | `num_read_outstanding=32` vs `m_axi_latency=64`; fixed → 1.029 | `N16` |
| Myers fan-out fixes | double-bank `pq_*`; stagger the copy | both **rejected** (+58 % LUT; +27 % LUT, net moved upstream) | `N23` |
| Wavefront 512, OOC | PE as a 7-bit truth table (98 entries); 40 LUT/PE | arithmetic PE 3.213 ns → ROM PE 1 LUT level; cosim 3,958 cycles | `N25`, `N26` |
| Tiling | 1024 = 8 × 128; per-module compile peak 3,206,252 → 37,151 instructions | csynth 2 h 44 → 3.9 min; OOC WNS +0.082 @ 2.5 ns | `N29` |
| Traceback path | one 256-bit port per tile + FIFO + writer | burst inference restored; cosim 1.000 cycles/col; OOC 512: WNS +0.104 → 417 MHz | `N34` |
| Host traceback | span batching instead of one read per beat | ≈1.4 k reads projected at 181 kbp vs hundreds of thousands per-beat (`N36` quotes both 162,917 and 325,834 for per-beat; all are projections) | `N36` |
| 3072 cosim | config bug: `WV_QDEPTH=64` < m/32 | fixed to 256; guard added | `N38` |
| Base-3 packing | 5 directions in one byte (3⁵ = 243 ≤ 256) | csynth −17 % LUT, −9 % FF, same 1.0 cycles/col | `N44` |
| AXI budget | single control bundle; budget is 32 not 33 | 4096 ruled out at 1:1 tile↔channel | `N45`–`N49` |
| `WV_SHARE` | decouple tile from channel | 1024 PE cosim OK; per-master ≈ 3,420 LUT; 4096/SHARE=2 cosim 1.032 cycles/col but **over the channel's bandwidth** | `N53`–`N55` |
| 160-wide tile | `WV_DIR_SG` supergroup | csim 18/18; −0.41 slack is the 25 % wider II=1 loop, not my divide (corrected) | `N58` |
| 3968 / 3840 / 128 area ladder | csim + csynth, base-3 | 926,348 csynth LUT, placed 278,639 (3.32 ×) | `N61`, `N64` |
| Clock-flag probes | OOC probe to `opt_design` | `--kernel_frequency ≠ 300` ⇒ 10 ns constraint | `N65` |
| S1c area ladder | 12 variants, 512 PE csynth | V1…V12; V12 (groups cut inside an idle window, lane state local) = 13,182 FF / 25,025 LUT per tile | `N69`, `N70` |
| S1t walker | four variants (v1, v2, v3, v1.5) | v1 shipped: tb_step II = 5, tb_fill 1,099 cycles, 8.7K LUT; v1.5 (512-bit fill) built but never linked | `N80` |

---

## 3. Narrative

**Phase 0 — a bit-vector kernel with a clock that did not exist (Oct 6–7, rows 1–5).** The
first design was Myers' bit-parallel algorithm: 1 bit of state per cell, 4,096 cell-lanes per
cycle, csynth Fmax 470.81 MHz, "1,929 GCUPS" on paper. Place-and-route gave **108 MHz** (row 1)
because one FSM state bit clock-enabled 21,404 flip-flops of the mask register file, and
neither a two-CU split (row 2), `--optimize 3` (row 5) nor two rewrites of the copy
(`N23`) moved it. Three consecutive explanations of the clock had been wrong by then
(`N12`: "three wrong models of the same critical path"). The project moved to an architecture
where every connection is nearest-neighbour: a systolic wavefront with the PE as a lookup
table.

**Phase 1 — the wavefront scales until the die and the memory ports intervene (Oct 9–13, rows
6–32).** 512 PE met 300 MHz (row 7). 2048 PE met 300 MHz after a query-distribution star was
turned into a chain (rows 8→11; 8.452 ns → 3.245 ns). 3072 PE routed at 291.6 MHz (row 18) with
SLR0 95.8 % full and SLR1 19.7 % empty — all direction masters must reach HBM in SLR0. Three
attempts to use SLR1 failed (rows 19, 29, 31) and one to avoid the pblock failed on a platform
clock by 12 ps (row 30). Meanwhile the 4096-PE target hit a platform limit (row 22/25,
33 AXI ports requested, 32 usable) and the 4800-PE/160-wide design placed but could not be
routed (row 32). Along the way: base-3 packing (5 directions per byte), the single-bundle
kernel, `WV_SHARE`, and a ladder of cosim/csynth jobs (§2.3).

**Phase 2 — first contact with silicon (Oct 19, ledger).** The first card runs found what no
simulation had: the 3072/2048 `kernel_tile.cpp` family produced wrong multi-band scores
(row 18, ledger `1bc30b7f`, `fd23cd3e`); the cause turned out to be a non-deterministic
read-after-write race between two m_axi masters over one HBM buffer (`LEDGER 164da747`,
`N66`). The 3968 build with the on-chip-boundary kernel (`kernel_tile_1b`) passed. The 181 kbp
measurement was **kernel 124.4 ms = 265.5 GCUPS, end-to-end 202** (row 29): about 3× below that
build's own peak (789.6) because every band reloaded its boundary through HBM.

**Phase 3 — S1, S1c, S1t (Oct 19–30, rows 33–47).** User's rule: traceback in every build,
quote scoring GCUPS and end-to-end next to each other, label measured vs projected.
S1 kept the band boundary on chip: **878.6 GCUPS at 246.5 MHz** (row 37). S1c removed the
per-band pipeline drain (97 % of PE×f) but lost on the clock — every S1c variant was
route-limited with 0–1 logic levels and 4–5 ns of wire (rows 40–44): a symmetric split was
worse (row 41), Vivado fan-out directives were worth 0.5 MHz (rows 42–43), and a smaller tile
made the final route *worse* (row 44). The user then asked for the traceback on the card
(S1t): 803.4 scoring and **566.9 end-to-end GCUPS** (row 45–47), the best end-to-end of any build.

### Frontier (best result per stage; **[M]** measured on the card at 181 kbp unless stated)

| Stage | Build (row) | Clock | Scoring GCUPS | End-to-end with traceback | Verdict |
|---|---|---|---|---|---|
| Myers bit-parallel | row 1 | 108.0 MHz | ~430 **[E]** (cosim cells/cycle × clock) | — | abandoned: clock |
| Wavefront 3072 PE | row 18 | 291.6 MHz | ~862 **[E]**, **no valid measurement**: multi-band wrong on silicon | — | superseded by 1b/S1 |
| 3968 PE, boundary through HBM | row 29 | 199.0 MHz | 265.5 **[M]** | 202 **[M]** | HBM round trip per band |
| **S1** (boundary on chip) | row 37 | 246.5 MHz | **878.6 [M]**, 1 rep | 474.8 **[M]** (host traceback) | **best scoring GCUPS** |
| S1c (continuous bands) | row 40 | 213.9 MHz | 822.8 **[M]**, 3 reps | 395.8 **[M]** | dominated by S1 on scoring; lost on clock |
| **S1t** (on-card traceback) | rows 45/47 | 205.3 MHz | 803.4 **[M]**, 3 reps | **566.9 [M]**, host-defined | **best end-to-end**; shipped build |

The frontier is two-dimensional (scoring vs end-to-end): S1 is not dominated on scoring
GCUPS (878.6 > 803.4), S1t dominates all on end-to-end. Efficiency against peak (PE × clock):
S1 878.6 / 978.1 = **89.8 %**, S1c 822.8 / 848.7 = **96.9 %** (the ledger quotes 97.3 % using
213 MHz, not 213.9), S1t 803.4 / 814.6 = **98.6 %**.

---

## Where files and notes disagree

Files win. Each item is a place where the agent's notes or ledger do not match the primary file.

| # | Claim | File says | Source |
|---|---|---|---|
| 1 | `N68`: S1 kernel "277,966 + 173,699 LUT by SLR" | Those are *SLR totals including the platform shell*; the kernel is **281,836 LUT (40.22 %)** | `RPT s1seam` `kernel_util_routed.rpt` vs `slr_util_routed.rpt` |
| 2 | Ledger/`N75`: seam10 + fan-out clock "180" | Log: **183.7 MHz** (seam10 alone: 180.8). Other ledger clocks (213, 214, 205) are rounded from 213.9, 214.4, 205.3 | `LOG 108754`, `LOG 108717` |
| 3 | `N74`/ledger: S1c efficiency 97.3 % of PE × f | 96.9 % at 213.9 MHz | arithmetic from `LOG 108585` |
| 4 | Compaction summary and `N38`: 2048 @ 300 "WNS +0.009" | Kernel WNS is **+0.044**; +0.009 is `hbm_aclk` | `RPT 2048_300` Intra Clock Table |
| 5 | `N64` vs `N65`: 107868 kernel "would scale ~258 MHz" vs "constraint was 100 MHz" | Raw report: period **10.000 ns**, WNS −0.169 (8 endpoints). Whether the design would have reached ~258 MHz is **UNKNOWN** | `link_tb_3968_270_b3/repair/timing_after.rpt` |
| 6 | Row 17 (105278): not in the notes at all | Failed with an **OOM kill** during synthesis | `LOG 105278` |
| 7 | WORKLOG open question 2 (macro list of the S1t compile) | **Resolved**: the `v++ -c` command line in `nw_tile_u50.xo.compile_summary` is `-D NW_PE=3968 -D WV_TILE_PE=128 -D WV_TRACEBACK -D WV_DIR_B3 -D WV_SEAM_RELAY -D WV_QDEPTH=65536 -D WV_RDEPTH=65536 -D WV_BDEPTH=1048576 -D WV_DIRDEPTH=8388608 --kernel_frequency 300`, identical to the `Makefile` `DEFS` | `link_tb_3968_300_s1t/nw_tile_u50.xo.compile_summary` |
| 8 | `docs/ENVIRONMENT.md`: build-node OS UNKNOWN | Vivado report headers: **Rocky Linux release 8.9** (the v3 build, `Oct 6`) and **CentOS Stream release 8** (S1, `Oct 21`) | `hw_kernel_util_routed.rpt`, `kernel_util_routed.rpt` headers |

## Open items (UNKNOWN) and what would settle them

| Item | Settled by |
|---|---|
| Who chose the 270 MHz ask of row 33, and the 350/400 asks of rows 15/16 | the earlier session transcript or the link-script submission lines |
| Why 105168 (row 9) was cancelled 14 min before completion | the earlier session transcript (before 2026-10-09T22:53Z) |
| Whether 107868's kernel would have reached ~258 MHz (row 33) | relinking with `--clock.default_freqhz` and reading `AUTO-FREQ-SCALING` |
| Whether sw_emu works with the `hls::task` kernel | one `make sw_emu PE=512` on a Vitis machine |
| Cause of 105630's >4 h placement (row 24) and 105297/105390's "failed to commit instances" (rows 21, 23) | the per-PE pblock tcl (`pblock_perpe.tcl`, not archived with the logs) and the placer log |
| The Myers-era decision to abandon bit-vector for a wavefront | the earlier transcript; `N25` only gives the reason (108 MHz) |
