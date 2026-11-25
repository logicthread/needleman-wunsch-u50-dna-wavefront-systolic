# WORKLOG

Running record. Newest entries last. Every claim cites a file; `UNKNOWN` says what would settle it.

## 2026-11-25 — Step 1: isolate and make shareable

### What was done
1. Located the original workspace: `~/fpga/` on the author's Mac (23 GB, several projects).
   It is **read-only evidence**; nothing in it was edited, moved or deleted
   (checked: no file under `nw_myers/` modified in the 30 min after copying).
2. Created this repo (copies only) and a private evidence folder **outside** the repo
   (`nw-fpga-u50-evidence/`: notes, ledgers, hardware-run outputs, reports; symlinks to the
   large archives and to the agent transcripts). The evidence folder is never committed.
3. Selected files for the presented variants (see "What was copied").
4. Scrubbed (see "Scrub report").
5. Wrote `Makefile`, `scripts/env.sh`, `linear/tools/gen_link_cfg.py`, `docs/ENVIRONMENT.md`, `README.md`.

### Findings that differ from what I was told (files win)
| Told | Files say | Source |
|---|---|---|
| "Smith-Waterman accelerator" | **Needleman–Wunsch, global alignment.** A small separate Smith–Waterman project exists (`dna_wavefront`, score-only, one commit) but it is not this design. | `nw_myers/HANDOVER.md:3`, `nw_myers/README.md:1`; `dna_wavefront/NOTES.md:1` |
| "870 GCUPS, ~230 MHz, peak 913" | The string 870 / 913 / 230 MHz appears **nowhere** in notes, ledgers or INFLIGHT/HANDOVER. Measured: S1 878.6 @ 246.5 MHz (host traceback); S1t 803.4 @ ≈205 MHz (**on-card traceback**). 3968 × 230 MHz = 912.6, so the 913 looks like arithmetic from a 230 MHz assumption — origin **UNKNOWN**. | `results/ledgers/linear.csv`; `grep` over `NOTES.md`, `INFLIGHT.md`, `HANDOVER.md` found no match |
| "181 kbp × 180 kbp" | Reference 181,019; query padded to 182,528 (46 bands × 3968). GCUPS is computed on the padded 182,528 × 181,019 = 33.04 G cells. For S1t the logs give only the padded figure; on the true 181,019² it would be 803.4 × 181019/182528 ≈ 796.8 (**computed here, not in any log**; the S1c ledger rows do state both, e.g. 822.8 padded / 816.0 true). | `results/linear_s1t_gate_35a63b96/results.csv` |

### What was copied (variant S1t = on-card traceback)
* `linear/src`: `kernel_tile_1b_s1t.cpp` (top `nw_tile_u50`), `wv_tb_walk.h` (on-card walker),
  `nw_wave.{c,h}`, `nw_wave_lut.h`, `nw_ref.c`, `nw.h`.
* `linear/host`: `nw_gate_host.cpp` (XRT host), `nw_host_traceback.{c,h}` (host walker / decoder).
* `linear/cfg`: HLS configs, link cfg of the S1t build (recovered from the archived workspace
  tarball: `tile_link_tb3968_1bt.cfg`), `pblock_3968c.tcl`.
* Left out: all `.xclbin/.xo`, `_x/`, `_xl/`, `.ipcache/`, `.Xil/`, logs, `*.rejected`, `*.bak`,
  `*.pre_*` variants, other kernel generations (S1, S1c, S1c2, 1b, v1–v3), the other 17 build dirs.
* Result evidence in-repo (small, no personal data): `results/linear_s1t_gate_35a63b96/`,
  `results/ledgers/linear.csv`.

### Scrub report
Searched the repo for user names, university/host names, absolute paths, license-server
strings, e-mail-like strings, tokens/keys/passwords.
| Found | Where | Action |
|---|---|---|
| Cluster login/roll-number path `/home/<id>/fpga/nw_myers/pblock_3968c.tcl` | `linear/cfg/tile_link_tb3968_1bt.cfg` | replaced by `@PBLOCK_TCL@`; `make` generates the real cfg with `gen_link_cfg.py` |
| `$HOME/fpga/nw_myers/...` | `linear/scripts/build_host.sh`, `verify_hls.sbatch` | now relative to the repo root |
| Scheduler node exclusion `--exclude=node05` | 5 sbatch/shell scripts | removed |
| `/scratch/$USER/...` | 3 scripts | `${SCRATCH_DIR:-${TMPDIR:-/tmp}}` |
| Cluster Vitis path and licence server (`env.sh` of the original) | **not copied**; `scripts/env.sh` is a placeholder template | — |
| Secrets, tokens, API keys, passwords, e-mail addresses | **none found** (hits for "token" are DP data tokens) | — |
Still present deliberately: the platform name `xilinx_u50_gen3x16_xdma_5_202210_1`, tool versions.
Not scanned: git history (fresh repo), the evidence folder (private).

### What runs where
* **Verified on this Mac:** `make test` → walker 13/13 cases. This is the "small alignment" check.
* **Not possible here:** `sw_emu`, `hw_emu`, `hw`, `run`, `bench` — no Vitis, XRT or card on
  this machine. `Makefile` targets are transcribed, not executed. **This is why Step 1 item 7
  ("builds to sw_emu") is not met**; it needs a machine with Vitis 2025.1.

### Open questions
1. Which clock/GCUPS did the "870 @ 230 MHz / 913 peak" come from? (Perhaps a projection or
   a different conversation. Settled by: you telling me, or a build I have not found.)
2. Exact `v++` macro list of the S1t compile (job 113498). Inferred in `Makefile`.
3. Is `end-to-end 566.9 GCUPS` "kernel + on-card walk + ops readback" only? The host appears
   to exclude BO allocation/upload (≈45 KB of 2-bit sequences) — to be checked in Step 5.
4. Does `sw_emu` work with `hls::task` at all? Never tried.
5. Repo name, license, private/public — resolved: MIT, public, name chosen.

### 2026-11-25 — publication decisions (user)
* Headline build: **S1t** (on-card traceback); README states the S1t figures.
* Repo: `needleman-wunsch-u50-dna-wavefront-systolic`, **public**, MIT, copyright holder `logicthread`.
* Commit history rewritten once to the GitHub handle with a GitHub noreply address (placeholder author removed).
