/* =====================================================================================
 * kernel_tile_1b_s1c.cpp -- S1c: CONTINUOUS BANDS. Derived from kernel_tile_1b_s1.cpp.
 *
 * S1 ran one DATAFLOW region per band. Each region had to DRAIN completely (31 tiles, 31
 * dir writers, their last AXI write responses) before the next band could start, and a
 * sequential on-chip copy moved the boundary between two arrays. Measured on the U50 at
 * 181 kbp (gate 09bbbe4e): 201,520 cycles per band for 181,019 columns -- 10.2% overhead,
 * of which wavefront fill 2.1%, boundary copy 2.8%, region start/stop ~5.3%.
 *
 * S1c runs ONE region for the whole alignment and the array never drains:
 *   - every ring process (head, 31 tiles, seam relays, tail) is an hls::task that loops
 *     over ALL bands; configuration arrives on a stream chain, never as an argument.
 *   - the band boundary goes tail -> head through an on-chip FIFO ("wrap", BRAM),
 *     2 bits per column, 32 per word. Band b+1's head needs column j at (b+1)*S + j; band
 *     b's tail produced it at about b*S + j + PE. Slack S - PE, so for S >= WV_NMIN the
 *     data is always there and nothing waits.
 *   - S = n + g is the band STRIDE in link tokens. g = max(0, WV_NMIN - n) bubbles after
 *     each band (only when n is short) keep the ring flowing: the head must never block on
 *     wrap data that is still inside the chain, or the chain stops and never delivers it
 *     (cosim deadlock, Step-0 toy fbtoy/fbt.cpp, n=37 vs 8 stages). At 181 kbp g = 0.
 *   - a PRE-MARKER bit rides the link one token BEFORE each band's column 0. Each lane
 *     copies its next-band query (nc/nl) into its current registers (qc/ql) at the end of
 *     the pre-marker cycle, so column 0 meets the new query with no mux in the compare
 *     path. Tiles fetch the next band's block (and forward the rest) during the band.
 *   - directions are ONE CONTINUOUS diagonal stream per tile: counted cycle
 *         c = band*S + j + i
 *     with the same base-3 supergroup packing; the host decoder uses that c directly
 *     (host/nw_host_traceback.c, -DNWH_CONT).
 *
 * Why hls::task and not plain DATAFLOW: csim executes DATAFLOW processes one after another
 * in program order, so a feedback stream is read before it is written ("hls::stream is
 * read while empty"). Tasks are threads in csim and free-running in RTL. Measured in the
 * Step-0 toy (2026-10-21): plain DATAFLOW csim FAIL; hls::task csim + cosim PASS, 6 cases.
 * ===================================================================================== */
/* kernel_tile_1b_s1t.cpp -- S1t = S1c (kernel_tile_1b_s1c.cpp @1cc3ea2e, seam16 V12)
 * + ON-CARD TRACEBACK (src/wv_tb_walk.h). Everything else is byte-identical to S1c.
 *
 * kernel_tile_1b.cpp -- SINGLE-BUNDLE variant of kernel_tile.cpp.
 *
 * WHY THIS FILE EXISTS. The U50 HBM subsystem accepts only 33 AXI connections:
 *     ERROR: [VPL-1] You have run out of port connections on /hmss_0.
 *                    All 33 connections are used
 * At 4096 PE (32 tiles) the baseline asks for 37 -- 32 direction ports plus qpack,
 * rpack, bnd_r, bnd_w and score_out -- so it cannot link (job 105389, N45).
 *
 * The five control ports exist ONLY because five different DATAFLOW processes each
 * touch HBM, and two DATAFLOW processes cannot share an m_axi bundle (that is also why
 * bnd_r/bnd_w are already two ports onto ONE physical buffer). The fix is not to merge
 * the ports but to change WHICH PROCESSES TOUCH MEMORY: all external access moves into
 * the SEQUENTIAL outer band loop, which is one process and therefore one bundle, and
 * the DATAFLOW processes read on-chip arrays instead. 32 + 1 = 33, exactly the limit.
 *
 * THE 32 DIRECTION WRITERS ARE UNTOUCHED ON PURPOSE. Their `dir[base + b]` over a
 * counted loop is affine, which is what makes burst inference work. The cheaper-looking
 * alternative -- two tiles per port -- needs a conditionally-incremented index, and
 * N34 records that exact construction inferring NO bursts on any of 16 bundles.
 *
 * WHAT MOVES ON CHIP (URAM is free: the 3072 build used 4 of 640):
 *   ubr   whole reference, 2 bits/base, loaded ONCE rather than re-read every band
 *   ubq   one band's query slice, WV_PE bases
 *   ubiw  the band boundary read  by wv_head  (S1: 2 bits/column, 32/word, on chip)
 *   ubow  the band boundary written by wv_tail (S1: copied ubow->ubiw between bands)
 * ubi and ubo are DISTINCT arrays. In-place reuse is legal in hardware -- the head's
 * read leads the tail's write by WV_PE-1 columns -- but a single on-chip array read by
 * one DATAFLOW process and written by another is a dependence HLS will not allow, which
 * is precisely why the baseline needed two bundles. Routing them through HBM costs
 * ~n/32 cycles per band (~3% at n = 181 kbp) and keeps n_max at 94.4 Mbp instead of the
 * 10.5 Mbp a fully on-chip bnd would impose.
 *
 * MEMORY LAYOUT is the host's choice, passed as word offsets into one buffer:
 *   mem[q_off ..]  query,     2 bits/base, 32 bases per 64-bit word
 *   mem[r_off ..]  reference, same packing
 *   mem[b_off ..]  S1t: on-card traceback descriptor + results (src/wv_tb_walk.h)
 *   mem[s_off]     the score, one int64
 */
/* kernel_tile.cpp -- DP-W as a tiled SOFTCORE. 1024 PEs = 8 instances of one 128-PE
 * module. Score-only by default; -DWV_TRACEBACK adds the direction write path.
 * ===================================================================================
 * WHY TILED, FROM MEASUREMENT AND NOT FROM TASTE
 *
 * The flat kernel is correct and closes II=1, but HLS materialises ~3.12*PE^2
 * instructions at its array-partition phase before simplifying back down to ~15k.
 * Measured peaks: 817,917 at 512 PE and 3,206,252 at 1024. That transient is what makes
 * csynth time scale as PE^2.84 -- 23 min at 512, 2 h 44 at 1024, ~20 h projected at
 * 2048 and ~6 days at 4096. The final design is small and its AREA is linear
 * (28.96 LUT/PE + ~4.9k fixed); only the compiler's intermediate blows up.
 *
 * I first blamed qload's PE-long shift chain, on an arithmetic fit that matched at both
 * 512 and 1024. That fit was worthless: the observed peak and 2*(PE-1)*PE are BOTH
 * quadratic, so any constant multiple agrees at two points. Replacing the shift with an
 * indexed write moved the peak 818,932 -> 815,733, i.e. nothing. The quadratic is the
 * sweep's nine complete-partitioned arrays, accesses x banks before index narrowing.
 *
 * Since the peak is irreducibly ~3.12*TILE_PE^2 per COMPILED MODULE, the only lever is
 * a smaller module compiled once:
 *      flat 1024          3,206,252
 *      256-PE tile          204,800   15.7x
 *      128-PE tile           51,118   63x    <- and under HLS's own 100,000 warning line
 *
 * THE CHANNEL CHOICE IS NOT ARBITRARY -- it is granularity.
 *
 *   STREAM (FIFO) for the per-cycle chain. Between tiles the traffic is ONE 6-bit link
 *   per cycle and tile k needs tile k-1's value from the immediately previous cycle. A
 *   PIPO here would synchronise at ARRAY granularity -- tile k-1 finishing all n columns
 *   before tile k starts -- turning latency from n+PE into n*PE. Catastrophically wrong.
 *
 *   PIPO for the query rows. These are produced ONCE PER BAND by the loader and consumed
 *   once per band by each tile, so array-granularity handshaking is exactly right, and
 *   the double buffer lets the loader fill band k+1 while the tiles still sweep band k.
 *
 * REGISTERING TILE BOUNDARIES IS FREE, which is what makes tiling safe at all. Only dV
 * is loop-carried, and it never leaves a PE; the dh/rc/act chain is feed-forward in
 * space. Adding a register to a boundary delays that tile's whole wavefront by one
 * cycle, and because all three signals travel together the schedule stays consistent --
 * it is a skew, not a hazard. Measured hop cost, routed, FF->FF 6 bits:
 *      same clock region   0.275 ns route
 *      vertical adjacent   0.468
 *      horizontal adjacent 0.907     <- 2x vertical; snake vertically, cross rarely
 *      SLR seam            1.362
 * Against 2.20 ns of usable budget at 400 MHz those all fit unregistered, but they are
 * isolated nets in an empty device. Registering costs 7 cycles total at 8 tiles.
 * ===================================================================================*/
#include <stdint.h>
/* AP_INT_MAX_W BEFORE ap_int.h, or ap_uint<> wider than the 1024-bit DEFAULT fails to
 * compile. The supergroup output register ob is WV_DIR_SBYTES*8 bits:
 *   TILE_PE 128 -> 1024 (the old default, exactly)   160 -> 2560   224 -> 3584
 * 4096 covers every tile width up to 256. It is a compile-time template bound only; it
 * costs some preprocessing time and nothing in hardware. */
#ifndef AP_INT_MAX_W
#define AP_INT_MAX_W 4096
#endif
#include <ap_int.h>
#include <hls_stream.h>
#ifdef WV_DIAG
#include <cstdio>
#endif
#include "nw_wave.h"
#include "nw_wave_lut.h"

#define WV_PE        NW_PE                       /* total PEs across all tiles */
#ifndef WV_TILE_PE
#define WV_TILE_PE   128                         /* PEs per softcore instance */
#endif
#define WV_TILES     (WV_PE / WV_TILE_PE)
#if (WV_PE % WV_TILE_PE) != 0
#error "WV_PE must be a whole number of tiles"
#endif

/* ---- WV_SHARE: how many TILES share one HBM pseudo-channel ----------------
 * The 1:1 tile<->channel binding was never a bandwidth decision, it was an
 * accident of having one wv_dirwriter per tile. Measured: at 199 MHz a 128-PE
 * tile emits 5.09 GB/s and a U50 pseudo-channel delivers 9.875 GB/s, so 31
 * tiles were spending 31 channels on a 19-channel job -- and the 32-port AXI
 * budget (N49) was capping PE for no PHYSICAL reason.
 *
 * WV_SHARE tiles feed ONE writer, GROUP-INTERLEAVED, so the channel address
 * space stays stride-1 and burst inference survives:
 *     beat = band*(nbeats*WV_SHARE) + (g*WV_SHARE + k)*WV_DIR_BPG + sub
 *
 * WV_SHARE must DIVIDE WV_TILES. A short final channel would change the beat
 * stride for that channel only, and the host's formula above is closed-form
 * with no per-channel special case -- it would decode silently wrong.
 * WV_TILES=31 is PRIME, so the shipped 3968 build cannot share; WV_PE=4096
 * gives 32 tiles -> 16 channels -> 17 AXI, which the 32-port budget had made
 * unbuildable.
 *
 * WV_SHARE=1 is the default and selects the ORIGINAL per-tile writer path, so
 * the proven 3968 deliverable is bit-identical. */
#ifndef WV_SHARE
#define WV_SHARE 1
#endif
#if (WV_TILES % WV_SHARE) != 0
#error "WV_SHARE must divide WV_TILES exactly -- a short final channel changes that channel's beat stride and the host traceback formula has no per-channel case, so it would decode silently wrong. WV_TILES=31 is prime: keep WV_SHARE=1, or build WV_PE=4096 for 32 tiles."
#endif
#define WV_NCH       (WV_TILES / WV_SHARE)


#define WV_RBITS     2
#define WV_PER_WORD  (64 / WV_RBITS)

#ifndef WV_QDEPTH
#define WV_QDEPTH 4096
#endif
#ifndef WV_RDEPTH
#define WV_RDEPTH 1048576
#endif
#ifndef WV_BDEPTH
#define WV_BDEPTH 1048576
#endif

/* ---- DIRECTION WRITE PATH (compile-time, -DWV_TRACEBACK) -------------------------
 * ONE port per tile: 128 PEs x 2 bits = 256 bit/cycle, two counted cycles per 512-bit
 * beat. See the BEAT PACKING note below for the full layout.
 *
 * SCALING, with the array emitting 2 bits per PE per cycle and every bit going to HBM:
 *     512 PE   4 ports   51.2 GB/s   4 x 8 PCs
 *    1024 PE   8 ports  102.4 GB/s   8 x 4 PCs
 *    2048 PE  16 ports  204.8 GB/s  16 x 2 PCs  = 65% of the device's 316 at 400 MHz
 *    4096 PE  32 ports  409.6 GB/s  INFEASIBLE, exceeds 316
 * Per port that is 12.8 GB/s, MORE than one pseudo-channel's 9.88, so each takes two --
 * which also puts all 32 PCs in use and so gives the full 8.19 GB, i.e. the 181 kbp
 * square capacity limit. Each pair sits inside one 4-PC mini-switch group, so no master
 * crosses a group boundary.
 *
 * AN EARLIER VERSION USED TWO 128-BIT PORTS PER TILE. It is worse on two counts that are
 * pure arithmetic: twice the 512-bit beat accumulators (4,096 FF vs 2,048 at 512 PE) and
 * twice the AXI masters, every one of which grows the shell crossbar at the bottom of
 * SLR0. Its apparent advantage -- more per-port headroom -- is neutralised by the drain
 * FIFO, since wv_dirwriter empties at full port rate rather than at production rate.
 *
 * HEADROOM IS NOT A LUXURY: backpressure is GLOBAL. The flat design's
 * ap_block_pp0_stage0_subdone carried 12,354 pins, so one stalled port stalls every PE.
 * WV_DIRFIFO is what keeps an HBM hiccup out of the recurrence.
 *
 * POSITION IS COUNTED, NOT INDEXED BY CYCLE. Tile k counts from its own first active
 * cycle; at count c its local PE i holds column c-i. Any FIFO latency upstream shifts
 * every tile's start equally and changes nothing. Indexing by the loop counter instead
 * is what broke wv_tail: it was wrong by 7 AND disagreed between csim and RTL.
 * Layout and host inverse verified over 22,126,416 cells, 0 failures, with a
 * written-at-least-once check so holes would surface too.
 */
#ifdef WV_TRACEBACK
/* ONE dir port per tile. A tile's 128 PEs emit 128 directions = 256 bits EVERY cycle,
 * so two counted cycles fill a full 512-bit AXI beat = 256 cell values, 16 values per
 * 32-bit word. Layout and host inverse verified standalone over 25,867,509 cells, 0
 * wrong and 0 never-written:
 *
 *   cell (row, j):  band = row/WV_PE,  k = (row%WV_PE)/WV_TILE_PE,  i = (row%WV_PE)%WV_TILE_PE
 *   c    = j + i                 COUNTED from the tile's own first active cycle
 *   beat = band*bpb + c/2        bpb  = ceil((n + WV_TILE_PE)/2)
 *   bit  = 256*(c%2) + 2*i
 *
 * c IS COUNTED, NEVER A CYCLE INDEX. That is what makes every buffer below free: FIFO
 * depth, writer-process latency and per-boundary registers all shift timing without
 * moving a single bit's address. Deriving position from the loop counter is what broke
 * wv_tail (wrong by 7, and csim disagreed with RTL).
 *
 * THE BEAT GOES TO A FIFO, NOT STRAIGHT TO m_axi, and a separate writer process drains
 * it. Two reasons, both measured:
 *   - BURST INFERENCE. Writing m_axi from inside the tile made the address a
 *     conditionally-incremented counter, which HLS cannot prove sequential: all 16
 *     bundles of the 2-per-tile version inferred NO bursts, where the flat design's
 *     affine `t/PACK` index inferred 512-bit bursts on all 8. The writer's loop is
 *     `dir[base + b] = fifo.read()` over b -- affine, so inference returns.
 *   - TIMING ISOLATION. At 16 tiles the worst path was lk_16's FIFO-full signal reaching
 *     back to tile 0's delta register: DATAFLOW backpressure spanning the chain, WNS
 *     -0.212 ns. A deep FIFO between the array and the AXI path stops HBM stalls from
 *     propagating into the recurrence at all.
 */
#define WV_DIR_WBITS      (WV_TILE_PE * 2)                   /* 256 bits per cycle */
#define WV_DIR_BEATBITS   512                                /* AXI data width cap */
#if WV_DIR_WBITS > WV_DIR_BEATBITS
#error "WV_TILE_PE > 256: a tile's per-cycle direction output exceeds one AXI beat"
#endif
#define WV_DIR_PACK       (WV_DIR_BEATBITS / WV_DIR_WBITS)   /* 2 cycles per beat */

/* Reference capacity held on chip. 2 bits/base, so WV_NMAX bases cost WV_NMAX/4 bytes:
 * 2 Mbase = 512 KB = ~15 URAMs of 640. The ceiling if ALL URAM went to the reference is
 * 94.4 Mbase. Raise it for real genomes; the gate runs need only thousands. */
/* ubi and ubo are ONE BYTE PER COLUMN each, so WV_NMAX directly sets the on-chip cost:
 * 2 x WV_NMAX bytes. At 2097152 that is 4 MB, past BRAM and most of URAM -- the gate
 * only reported 84 BRAM because its n is tiny. 262144 covers the project's stated
 * 181 kbp capacity with margin, at 2 x 256 KB. Raise it WITH a storage decision. */
#ifndef WV_NMAX
#define WV_NMAX 262144
#endif
#define WV_RWORDS ((WV_NMAX + WV_PER_WORD - 1) / WV_PER_WORD)
#define WV_QWORDS ((WV_PE   + WV_PER_WORD - 1) / WV_PER_WORD)

/* m_axi depth must EQUAL the testbench allocation (N11/N38) -- a mismatch is undefined
 * behaviour that no level of the tool diagnoses. One buffer now holds query, reference,
 * boundary and score, so the TB must allocate exactly WV_MEMDEPTH 64-bit words. */
/* DERIVED, never hand-written: the TB lays out q at 0, r after it, bnd after that and
 * the score last, using these same macros. A hand-typed depth is exactly how N38
 * happened (WV_QDEPTH=64 against m=3072, undefined behaviour no tool reports), so the
 * two sides are tied together here instead. */
#ifndef WV_MEMDEPTH
#define WV_MEMDEPTH (WV_QDEPTH + WV_RDEPTH + ((WV_BDEPTH + 7) / 8) + 1)
#endif

/* ---- BASE-3 DIRECTION PACKING (-DWV_DIR_B3): 5 directions in 8 bits ---------------
 * A direction has exactly THREE values (DIAG 0, UP 1, LEFT 2), so 2 bits/cell wastes
 * a quarter of every write. 3^5 = 243 <= 256, so five directions fit in one BYTE:
 * 1.600 bits/cell against a log2(3) = 1.585 floor, i.e. within 1% of optimal.
 *
 * WHY THIS MATTERS: the direction stream is the ONLY large HBM consumer. At 4096 PE
 * 2 bits/cell wants 307.2 GB/s of the 316 GB/s HBM, i.e. 97.2% -- unbuildable in
 * practice. At 1.6 bits/cell it is 245.8 GB/s = 77.8%. It also drops 3072 from 72.9%
 * to 58.3%.
 *
 * WHY GROUPING IS ALONG TIME, NOT ALONG THE PE AXIS: 128 lanes is not divisible by 5.
 * Five COUNTED CYCLES give each lane 5 of its own consecutive directions -> one byte
 * per lane -> WV_TILE_PE bytes = 1024 bits = EXACTLY two 512-bit beats, no remainder
 * and no partial-group case.
 *
 * DIGIT ORDER IS FIXED AND MUST MATCH THE HOST: acc = acc*3 + d applied once per
 * counted cycle, so the FIRST of the five cycles is the MOST significant digit:
 *     byte = d0*81 + d1*27 + d2*9 + d3*3 + d4
 *
 * THE MULTIPLY RUNS FOR EVERY LANE ON EVERY COUNTED CYCLE, active or not (d = 0 for
 * inactive). Digit position is defined by HOW MANY times the lane has been multiplied,
 * so skipping inactive lanes would misalign the group -- unlike the 2-bit scheme where
 * each (lane, cycle) owns a fixed bit position and inactive cells are simply ignored.
 *
 * COST: one 8-bit multiply-add and 8 FF per lane. It is NOT in the recurrence -- the
 * direction output is not loop-carried -- so it cannot lengthen the dv path. */
#ifdef WV_DIR_B3
#define WV_DIR_GROUP      5                                  /* cells per byte */
#define WV_DIR_BPG        ((WV_TILE_PE * 8) / WV_DIR_BEATBITS) /* beats per group = 2 */
/* ---- SUPERGROUP: make the byte stream land on WHOLE 512-bit beats ------------
 * A base-3 group is WV_DIR_GROUP (5) cycles and yields WV_TILE_PE BYTES. Those
 * bytes fill a whole number of 512-bit beats only when WV_TILE_PE is a multiple
 * of 64. At 128 it is (2 beats/group). At 160 it is NOT: 160*8/512 = 2.5, and
 * WV_DIR_BPG would integer-divide to 2 and SILENTLY DROP a fifth of every group.
 *
 * So emission is keyed to a SUPERGROUP of WV_DIR_SG groups -- the fewest that
 * land on a beat boundary, i.e. the smallest SG with SG*WV_TILE_PE % 64 == 0.
 *   TILE_PE 128 -> SG 1,  128 B,  2 beats,  5 cycles   (= the shipped design)
 *   TILE_PE 160 -> SG 2,  320 B,  5 beats, 10 cycles
 *   TILE_PE 192 -> SG 1,  192 B,  3 beats,  5 cycles
 *    TILE_PE 64 -> SG 1,   64 B,  1 beat,   5 cycles
 * 64 is a power of two, so gcd(WV_TILE_PE,64) is just the largest power of two
 * dividing WV_TILE_PE, capped at 64. */
#define WV_P2OF64(x)      ( ((x) % 64 == 0) ? 64 : ((x) % 32 == 0) ? 32 \
                          : ((x) % 16 == 0) ? 16 : ((x) %  8 == 0) ?  8 : 1 )
#define WV_DIR_SG         (64 / WV_P2OF64(WV_TILE_PE))
#define WV_DIR_SBYTES     (WV_DIR_SG * WV_TILE_PE)
#define WV_DIR_SBEATS     (WV_DIR_SBYTES / (WV_DIR_BEATBITS / 8))
#define WV_DIR_SCYC       (WV_DIR_SG * WV_DIR_GROUP)
#if (WV_DIR_SBYTES * 8) % WV_DIR_BEATBITS != 0
#error "WV_DIR_SBYTES must be a whole number of 512-bit beats -- WV_TILE_PE must be a multiple of 8 for the supergroup derivation to close."
#endif
#if WV_DIR_SBEATS > WV_DIR_SCYC
#error "WV_DIR_SBEATS > WV_DIR_SCYC: a supergroup cannot drain one beat per cycle before the next completes, so the output register would be overwritten in flight."
#endif

/* WV_DIR_BPG USES INTEGER DIVISION, so WV_TILE_PE*8 must be an exact multiple of
 * WV_DIR_BEATBITS, i.e. WV_TILE_PE must be a multiple of 64. Otherwise it truncates
 * SILENTLY and the addressing is wrong everywhere:
 *     WV_TILE_PE =  96 -> 768/512  = 1 (should be 1.5)  misaligned groups
 *     WV_TILE_PE =  32 -> 256/512  = 0                  nbeats = 0, NOTHING written
 * The second is the dangerous one: a kernel that emits no direction data at all and
 * reports success. 128 is the value in use. */
/* SUPERSEDED BY THE SUPERGROUP. The two guards that stood here required
 *     (WV_TILE_PE * 8) % WV_DIR_BEATBITS == 0   i.e. WV_TILE_PE % 64 == 0
 * which is exactly the restriction WV_DIR_SG lifts: a supergroup of WV_DIR_SG groups
 * always lands on a beat boundary, so 96 (SG 2 -> 3 beats) and 160 (SG 2 -> 5 beats)
 * are now legal where they were rejected. The dangerous cases the old guards existed
 * to catch are still caught, by the WV_DIR_SBYTES checks above:
 *   - a byte stream that does not fill whole beats  -> (SBYTES*8) % BEATBITS != 0
 *   - a supergroup that cannot drain in time        -> SBEATS > SCYC
 * What remains unique to here is the degenerate "no beats at all" case, which was the
 * genuinely silent one: a kernel emitting NO direction data while reporting success. */
#if WV_DIR_SBEATS < 1
#error "WV_DIR_B3: WV_DIR_SBEATS is 0 -- the supergroup would emit NO beats and the kernel would write no direction data while reporting success. WV_TILE_PE is far too small."
#endif
#endif
#define WV_DIR_PORTS      WV_NCH
typedef ap_uint<WV_DIR_BEATBITS> wv_dirbeat;
/* Drain FIFO depth. 32 beats = 2 KB per tile, absorbing 64 cycles of production, which
 * covers an AXI write-response hiccup without ever stalling the array. */
#ifndef WV_DIRFIFO
#if WV_SHARE > 1
/* Sharing makes depth a CORRECTNESS requirement, not a performance tune. The writer
 * serves its tiles in a FIXED group order, but tile k+1 lags tile k by ~WV_TILE_PE
 * cycles of wavefront skew, so the LEADING tile must bank that skew:
 *   WV_TILE_PE * WV_DIR_BPG / WV_DIR_GROUP  =  51.2 beats at 128 PE.
 * Too shallow is a DEADLOCK, not a stall: the writer blocks on the lagging tile, the
 * leading tile fills its FIFO and stalls, and the lagging tile is CHAINED off the
 * leading one, so the awaited beat can never be produced. */
#define WV_DIRFIFO 128
#else
#define WV_DIRFIFO 32
#endif
#endif
#ifndef WV_DIRDEPTH
#define WV_DIRDEPTH 1048576
#endif
#endif

typedef ap_uint<3> wv_enc;
typedef ap_uint<2> wv_sym;
#define WV_ENC_GAP ((wv_enc)NWW_ENC(NWW_GAP))

#include <hls_task.h>

#if !defined(WV_TRACEBACK) || !defined(WV_DIR_B3) || WV_SHARE != 1
#error "S1c supports exactly the shipped configuration: -DWV_TRACEBACK -DWV_DIR_B3, WV_SHARE == 1"
#endif

#if defined(WV_SEAM_RELAY) && WV_TILES > 16
#ifndef WV_SEAM_AT
#define WV_SEAM_AT 16                      /* tiles 0..15 -> SLR0, 16.. -> SLR1 */
#endif
#ifndef WV_SEAM_STAGES
#define WV_SEAM_STAGES 3
#endif
#endif

/* S1C_VAR selects the tile implementation. 12 is the shipped one (csynth 512: 16.4K FF /
 * 27.1K LUT per tile vs S1 15.7K / 25.1K). 1-11 are the recorded experiments (NOTES N70);
 * 4/5/7/8/9 are diagnostics and are functionally WRONG for multi-band. */
#ifndef S1C_VAR
#define S1C_VAR 12
#endif

/* Minimum band stride. Must exceed the chain's in-flight latency (PE lanes + per-tile
 * pipeline depth + FIFOs + relays) plus one 32-column boundary word. WV_PE + 8*WV_TILE_PE
 * leaves ~700 cycles of margin at 3968 PE. Costs nothing when n >= WV_NMIN. */
#ifndef WV_NMIN
#define WV_NMIN (WV_PE + 8 * WV_TILE_PE)
#endif
#if S1C_VAR == 11 || S1C_VAR == 12
/* V11: every band is followed by at least WV_BWIN idle tokens; the tile-wide query swap
 * happens on token WV_BSWAP of that window, when all 128 lanes of a tile are idle
 * (window minus the 127-token lane skew), with >= 2x pipeline depth of margin each side. */
#ifndef WV_BWIN
#define WV_BWIN 640
#endif
#define WV_BSWAP (WV_BWIN / 2 + WV_TILE_PE / 2)
#define WV_GAP_OF(n) (((n) + WV_BWIN < WV_NMIN) ? (WV_NMIN - (n)) : WV_BWIN)
#else
#define WV_GAP_OF(n) (((n) < WV_NMIN) ? (WV_NMIN - (n)) : 0)
#endif
/* Whole query on chip (the loader streams every band's blocks). 2 bits/base. */
#ifndef WV_MMAX
#define WV_MMAX 262144
#endif
#define WV_MWORDS ((WV_MMAX + WV_PER_WORD - 1) / WV_PER_WORD)
#if (WV_PE % 128) != 0 || (WV_TILE_PE != 128)
#error "S1c loader assumes WV_TILE_PE == 128 (4 words per block, 4-bank ubq)"
#endif

/* The inter-tile link: dh(3) rc(2) act(1) PRE-MARKER(1) BAND-PARITY(1).
 * The band parity travels WITH each column, exactly like rc: lane i learns which band the
 * column it is processing belongs to from its upstream neighbour, so the per-lane query is
 * a pure select between two band-invariant blocks -- no loop-carried query state. */
typedef ap_uint<8> wv_link;
#define WV_LINK_PACK(dh, rc, act, pm) WV_LINK_PACK5(dh, rc, act, pm, 0)
#define WV_LINK_PACK5(dh, rc, act, pm, bp) \
    ((wv_link)((((ap_uint<8>)(bp)) << 7) | (((ap_uint<8>)(pm)) << 6) | \
               (((ap_uint<8>)(act)) << 5) | (((ap_uint<8>)(rc)) << 3) | ((ap_uint<8>)(dh))))
#define WV_LINK_BP(l)  ((ap_uint<1>)(((l) >> 7) & 0x1))
#define WV_LINK_DH(l)  ((wv_enc)((l) & 0x7))
#define WV_LINK_RC(l)  ((wv_sym)(((l) >> 3) & 0x3))
#define WV_LINK_ACT(l) ((ap_uint<1>)(((l) >> 5) & 0x1))
#define WV_LINK_PM(l)  ((ap_uint<1>)(((l) >> 6) & 0x1))

/* Query rows for one tile, one band. */
struct wv_qblk {
    wv_sym     c[WV_TILE_PE];
    ap_uint<1> live[WV_TILE_PE];
};

/* Configuration, sent once per kernel call down the ring. `rem` counts the tiles still
 * ahead INCLUDING the receiver, so a tile forwards rem-1 blocks per band. */
struct wv_cfg {
    uint32_t T;      /* link tokens every ring process handles */
    uint32_t n;      /* columns per band */
    uint32_t g;      /* bubbles after each band (0 unless n < WV_NMIN) */
    uint32_t nb;     /* bands */
    uint32_t cmax;   /* counted direction cycles per tile, whole alignment */
    uint32_t rem;
    int64_t  acc0;   /* score base G*m */
};

#ifndef __SYNTHESIS__
/* csim-only, kept for the testbench ABI. With per-group blocking block loads a lane can no
 * longer meet a pre-marker before its block, so this stays 0 by construction. */
int wv_qstarve = 0;
#endif

/* ---- free-running relay: one item per invocation, II=1 in RTL ---- */
template <typename T>
static void wv_relay_t(hls::stream<T> &in, hls::stream<T> &out)
{
#pragma HLS PIPELINE II = 1 style = flp
    out.write(in.read());
}

/* =====================================================================
 * THE SOFTCORE, CONTINUOUS. One hls::task per tile. Same recurrence as S1; additions:
 *   - per lane: current query (qc/ql), next query (nc/nl), pre-marker shift (pm)
 *   - in-loop non-blocking fetch of the next band's block and forwarding of the rest
 * ===================================================================== */
#if S1C_VAR == 12
/* One band group: S1's sweep verbatim with q constant and lane state local. */
static void wv_tile_group(hls::stream<wv_link> &lin, hls::stream<wv_link> &lout,
                          hls::stream<wv_dirbeat> &dout, const wv_qblk &q,
                          uint32_t ntok, uint32_t cmax,
                          ap_uint<8> pk[WV_DIR_SBYTES], ap_uint<WV_DIR_SBYTES * 8> &ob,
                          ap_uint<4> &pend, bool &started, uint32_t &c,
                          ap_uint<3> &gpos, ap_uint<6> &sgslot)
{
#pragma HLS INLINE
    wv_enc     dv [WV_TILE_PE], dh[WV_TILE_PE];
    wv_sym     rc [WV_TILE_PE];
    ap_uint<1> act[WV_TILE_PE];
#pragma HLS ARRAY_PARTITION variable = dv  complete dim = 0
#pragma HLS ARRAY_PARTITION variable = dh  complete dim = 0
#pragma HLS ARRAY_PARTITION variable = rc  complete dim = 0
#pragma HLS ARRAY_PARTITION variable = act complete dim = 0
    for (int i = 0; i < WV_TILE_PE; ++i) {
#pragma HLS UNROLL
        dv[i] = WV_ENC_GAP; dh[i] = WV_ENC_GAP; rc[i] = 0; act[i] = 0;
    }
tile_sweep:
    for (uint32_t t = 0; t < ntok; ++t) {
#pragma HLS PIPELINE II = 1
#pragma HLS LOOP_TRIPCOUNT min = 2 max = 16777216
        const wv_link    li    = lin.read();
        const wv_enc     dh_in = WV_LINK_DH(li);
        const wv_sym     rc_in = WV_LINK_RC(li);
        const ap_uint<1> ac_in = WV_LINK_ACT(li);
        if (ac_in) started = true;
        const bool counting = started && (c < cmax);
        const int pkbase = (int)sgslot * WV_TILE_PE;

        wv_enc     s_dh [WV_TILE_PE];
        wv_sym     s_rc [WV_TILE_PE];
        ap_uint<1> s_act[WV_TILE_PE];
#pragma HLS ARRAY_PARTITION variable = s_dh  complete dim = 0
#pragma HLS ARRAY_PARTITION variable = s_rc  complete dim = 0
#pragma HLS ARRAY_PARTITION variable = s_act complete dim = 0
        for (int i = 0; i < WV_TILE_PE; ++i) {
#pragma HLS UNROLL
            s_dh[i] = dh[i]; s_rc[i] = rc[i]; s_act[i] = act[i];
        }
    tile_pes:
        for (int i = 0; i < WV_TILE_PE; ++i) {
#pragma HLS UNROLL
            const wv_enc     dh_up = (i == 0) ? dh_in : s_dh [i - 1];
            const wv_sym     rsym  = (i == 0) ? rc_in : s_rc [i - 1];
            const ap_uint<1> afl   = (i == 0) ? ac_in : s_act[i - 1];
            ap_uint<2> dcode = 0;
            if (afl && q.live[i]) {
                const ap_uint<1> smatch = (q.c[i] == rsym) ? 1 : 0;
                const ap_uint<7> idx = (ap_uint<7>)
                    ((smatch, (ap_uint<3>)dh_up, (ap_uint<3>)dv[i]));
                const ap_uint<8> v = NWW_LUT[idx];
                dv[i] = (wv_enc)v(2, 0);
                dh[i] = (wv_enc)v(5, 3);
                dcode = v(7, 6);
            } else if (afl) {
                dh[i] = dh_up;
            }
            rc[i]  = rsym;
            act[i] = afl;
            if (counting)
                pk[pkbase + i] = (ap_uint<8>)(pk[pkbase + i] * 3 + (ap_uint<8>)dcode);
        }
        if (started && c < cmax + WV_DIR_SBEATS) {
            if (c < cmax && gpos == (WV_DIR_GROUP - 1) && sgslot == (WV_DIR_SG - 1)) {
                for (int i = 0; i < WV_DIR_SBYTES; ++i) {
#pragma HLS UNROLL
                    ob.range(8 * i + 7, 8 * i) = pk[i];
                }
                for (int i = 0; i < WV_DIR_SBYTES; ++i) {
#pragma HLS UNROLL
                    pk[i] = 0;
                }
                pend = WV_DIR_SBEATS;
            }
            if (pend != 0) {
                dout.write(ob.range(WV_DIR_BEATBITS - 1, 0));
                ob >>= WV_DIR_BEATBITS;
                --pend;
            }
            ++c;
            if (gpos == (WV_DIR_GROUP - 1)) {
                gpos = 0;
                sgslot = (sgslot == (WV_DIR_SG - 1)) ? (ap_uint<6>)0 : (ap_uint<6>)(sgslot + 1);
            } else {
                ++gpos;
            }
        }
        /* the swap bit is not used by V12 tiles (they count tokens); pass it on */
        lout.write(WV_LINK_PACK(dh[WV_TILE_PE - 1], rc[WV_TILE_PE - 1],
                                act[WV_TILE_PE - 1], 0));
    }
}
#endif

#if S1C_VAR == 12
/* =====================================================================
 * THE SOFTCORE, CONTINUOUS, V12: S1's lane body, q CONSTANT per sweep, lane state LOCAL
 * to each per-band group. The only combination csynth showed at S1's area (2026-10-21):
 *   V9  S1 body, one sweep, q read before the loop ............ 15.2K FF (= S1)
 *   V8  same body, per-band groups, lane state live across .... 26.9K
 *   V1/V2/V3/V10/V11  q (or a per-lane query) written in-loop . 32-46K
 * Groups are cut at token WV_BSWAP of each inter-band idle window, where every lane of
 * this tile is idle (the window is WV_BWIN tokens, the lane skew 127), so lane state at a
 * group boundary is dead: act = 0, dv resets to G for the new band, dh/rc are rewritten by
 * the next active column before anything reads them. Only the base-3 emitter (pk, c,
 * gpos...) carries across groups. Group b runs:
 *   b = 0       : tokens [0, n + WV_BSWAP)                       (lead + band 0 + half window)
 *   0 < b < nb-1: S tokens
 *   b = nb-1    : the rest of T
 * ===================================================================== */
static void wv_tile_t(hls::stream<wv_cfg> &cin, hls::stream<wv_cfg> &cout,
                      hls::stream<wv_link> &lin, hls::stream<wv_link> &lout,
                      hls::stream<wv_qblk> &qin, hls::stream<wv_qblk> &qfwd,
                      hls::stream<wv_dirbeat> &dout)
{
    const wv_cfg cf = cin.read();
    {
        wv_cfg o = cf;
        o.rem = cf.rem - 1;
        cout.write(o);
    }
    const uint32_t nfwd = cf.rem - 1;
    const uint32_t nt   = cf.T;
    const uint32_t cmax = cf.cmax;
    const uint32_t nb   = cf.nb;
    const uint32_t S    = cf.n + cf.g;
    const uint32_t kdly = (WV_TILES - cf.rem) * (WV_TILE_PE - 1);

    ap_uint<8> pk[WV_DIR_SBYTES];
#pragma HLS ARRAY_PARTITION variable = pk complete dim = 0
    ap_uint<WV_DIR_SBYTES * 8> ob = 0;
    ap_uint<4> pend = 0;
    bool       started = false;
    uint32_t   c = 0;
    ap_uint<3> gpos   = 0;
    ap_uint<6> sgslot = 0;
pk_reset:
    for (int i = 0; i < WV_DIR_SBYTES; ++i) {
#pragma HLS UNROLL
        pk[i] = 0;
    }
    uint32_t used = 0;

groups:
    for (uint32_t b = 0; b < nb; ++b) {
#pragma HLS LOOP_TRIPCOUNT min = 1 max = 128
        wv_qblk q;
#pragma HLS ARRAY_PARTITION variable = q.c    complete dim = 0
#pragma HLS ARRAY_PARTITION variable = q.live complete dim = 0
        /* one loop touches qin/qfwd: own block first, then the downstream tiles' */
    qstep:
        for (uint32_t k = 0; k <= nfwd; ++k) {
#pragma HLS PIPELINE II = 1
#pragma HLS LOOP_TRIPCOUNT min = 1 max = 32
            const wv_qblk blk = qin.read();
            if (k == 0) q = blk; else qfwd.write(blk);
        }
        /* Each tile delays the data by 127 tokens (lane 127's output at iteration t is
         * the column that entered lane 0 at t-127), so tile k's idle windows -- and its
         * group boundaries -- sit 127*k tokens later in the token stream. */
        const uint32_t ntok = (b == nb - 1) ? (nt - used)
                            : (b == 0)      ? (cf.n + WV_BSWAP + kdly)
                                            : S;
        used += ntok;
        wv_tile_group(lin, lout, dout, q, ntok, cmax, pk, ob, pend, started, c, gpos, sgslot);
    }
}
#elif S1C_VAR == 11
/* =====================================================================
 * THE SOFTCORE, CONTINUOUS, V11: S1's lane body UNCHANGED, flat sweep, TILE-WIDE swap.
 *
 * csynth 512 evidence (2026-10-21): S1's lane body swept once in this ring costs exactly
 * S1's area (V9 15.2K FF); ANY per-lane band switch (pre-marker chain, per-lane query
 * registers, parity select, false-dependence refill) made HLS stage lane state ~64 deep
 * (34-46K FF). So lanes here see a query that changes only when the WHOLE tile is idle:
 *   - the head leaves WV_BWIN idle tokens after each band; the swap bit rides token
 *     WV_BSWAP of that window. At a tile's input that token arrives when lanes 0..127 are
 *     all idle (lane 127 left the band 127 tokens after lane 0, the window is WV_BWIN).
 *   - on that token: q = qn, dv = G for every lane (one uniform write).
 *   - the bit is re-emitted to the next tile 127 tokens later by a counter, so it stays in
 *     the same position relative to the data (lane 127's output lags its input by 127).
 *   - qn (next block) is refilled from qin >= WV_QGUARD tokens after a swap. q and qn carry
 *     DEPENDENCE inter false: reads of q by idle lanes are don't-care, and writes are
 *     >= 256 tokens from any active read, twice the pipeline depth.
 * ===================================================================== */
#ifndef WV_QGUARD
#define WV_QGUARD 256
#endif
static void wv_tile_t(hls::stream<wv_cfg> &cin, hls::stream<wv_cfg> &cout,
                      hls::stream<wv_link> &lin, hls::stream<wv_link> &lout,
                      hls::stream<wv_qblk> &qin, hls::stream<wv_qblk> &qfwd,
                      hls::stream<wv_dirbeat> &dout)
{
    const wv_cfg cf = cin.read();
    {
        wv_cfg o = cf;
        o.rem = cf.rem - 1;
        cout.write(o);
    }
    const ap_uint<6> nfwd = (ap_uint<6>)(cf.rem - 1);
    const uint32_t   nt   = cf.T;
    const uint32_t   cmax = cf.cmax;
    const ap_uint<8> nb   = (ap_uint<8>)cf.nb;

    wv_qblk q, qn;
#pragma HLS ARRAY_PARTITION variable = q.c     complete dim = 0
#pragma HLS ARRAY_PARTITION variable = q.live  complete dim = 0
#pragma HLS ARRAY_PARTITION variable = qn.c    complete dim = 0
#pragma HLS ARRAY_PARTITION variable = qn.live complete dim = 0
#pragma HLS DEPENDENCE variable = q  inter false
#pragma HLS DEPENDENCE variable = qn inter false
    ap_uint<8>  loaded   = 0;      /* blocks taken (q for band 0, then qn per band) */
    ap_uint<1>  own_pend = 0;
    ap_uint<6>  fwd_left = 0;
    ap_uint<9>  guard    = 0;
    ap_uint<8>  swdly    = 0;      /* re-emit the swap bit to the next tile */

    ap_uint<8> pk[WV_DIR_SBYTES];
#pragma HLS ARRAY_PARTITION variable = pk complete dim = 0
    ap_uint<WV_DIR_SBYTES * 8> ob = 0;
    ap_uint<4> pend = 0;
    bool       started = false;
    uint32_t   c = 0;
    ap_uint<3> gpos   = 0;
    ap_uint<6> sgslot = 0;

    wv_enc     dv [WV_TILE_PE], dh[WV_TILE_PE];
    wv_sym     rc [WV_TILE_PE];
    ap_uint<1> act[WV_TILE_PE];
#pragma HLS ARRAY_PARTITION variable = dv  complete dim = 0
#pragma HLS ARRAY_PARTITION variable = dh  complete dim = 0
#pragma HLS ARRAY_PARTITION variable = rc  complete dim = 0
#pragma HLS ARRAY_PARTITION variable = act complete dim = 0
tile_reset:
    for (int i = 0; i < WV_TILE_PE; ++i) {
#pragma HLS UNROLL
        dv[i] = WV_ENC_GAP; dh[i] = WV_ENC_GAP; rc[i] = 0; act[i] = 0;
        q.c[i] = 0; q.live[i] = 0; qn.c[i] = 0; qn.live[i] = 0;
    }
pk_reset:
    for (int i = 0; i < WV_DIR_SBYTES; ++i) {
#pragma HLS UNROLL
        pk[i] = 0;
    }

tile_sweep:
    for (uint32_t t = 0; t <= nt; ++t) {
#pragma HLS PIPELINE II = 1
#pragma HLS LOOP_TRIPCOUNT min = 2 max = 16777216
        if (loaded == 0) {                          /* iteration 0: band 0's block -> q */
            q = qin.read();
            loaded = 1; fwd_left = nfwd;
            if (nb > 1) { own_pend = 1; guard = WV_QGUARD; }
            continue;
        }
        if (fwd_left != 0) {                        /* forwarding first: queue order */
            if (!qin.empty() && !qfwd.full()) { qfwd.write(qin.read()); --fwd_left; }
        } else if (own_pend && guard == 0) {
            if (qin.read_nb(qn)) { own_pend = 0; ++loaded; fwd_left = nfwd; }
        }
        if (guard != 0) --guard;

        const wv_link    li    = lin.read();
        const wv_enc     dh_in = WV_LINK_DH(li);
        const wv_sym     rc_in = WV_LINK_RC(li);
        const ap_uint<1> ac_in = WV_LINK_ACT(li);
        const ap_uint<1> sw_in = WV_LINK_PM(li);    /* tile-wide swap token */
        if (ac_in) started = true;
        const bool counting = started && (c < cmax);
        const int pkbase = (int)sgslot * WV_TILE_PE;

        wv_enc     s_dh [WV_TILE_PE];
        wv_sym     s_rc [WV_TILE_PE];
        ap_uint<1> s_act[WV_TILE_PE];
#pragma HLS ARRAY_PARTITION variable = s_dh  complete dim = 0
#pragma HLS ARRAY_PARTITION variable = s_rc  complete dim = 0
#pragma HLS ARRAY_PARTITION variable = s_act complete dim = 0
        for (int i = 0; i < WV_TILE_PE; ++i) {
#pragma HLS UNROLL
            s_dh[i] = dh[i]; s_rc[i] = rc[i]; s_act[i] = act[i];
        }
    tile_pes:
        for (int i = 0; i < WV_TILE_PE; ++i) {
#pragma HLS UNROLL
            const wv_enc     dh_up = (i == 0) ? dh_in : s_dh [i - 1];
            const wv_sym     rsym  = (i == 0) ? rc_in : s_rc [i - 1];
            const ap_uint<1> afl   = (i == 0) ? ac_in : s_act[i - 1];
            ap_uint<2> dcode = 0;
            if (afl && q.live[i]) {
                const ap_uint<1> smatch = (q.c[i] == rsym) ? 1 : 0;
                const ap_uint<7> idx = (ap_uint<7>)
                    ((smatch, (ap_uint<3>)dh_up, (ap_uint<3>)dv[i]));
                const ap_uint<8> v = NWW_LUT[idx];
                dv[i] = (wv_enc)v(2, 0);
                dh[i] = (wv_enc)v(5, 3);
                dcode = v(7, 6);
            } else if (afl) {
                dh[i] = dh_up;
            }
            rc[i]  = rsym;
            act[i] = afl;
            if (counting)
                pk[pkbase + i] = (ap_uint<8>)(pk[pkbase + i] * 3 + (ap_uint<8>)dcode);
        }
        if (sw_in) {                                /* all lanes idle: uniform swap */
            for (int i = 0; i < WV_TILE_PE; ++i) {
#pragma HLS UNROLL
                q.c[i] = qn.c[i]; q.live[i] = qn.live[i]; dv[i] = WV_ENC_GAP;
            }
            if (loaded < nb) { own_pend = 1; guard = WV_QGUARD; }
            swdly = WV_TILE_PE - 1;
        }
        ap_uint<1> sw_out = 0;
        if (swdly != 0) { --swdly; if (swdly == 0) sw_out = 1; }

        if (started && c < cmax + WV_DIR_SBEATS) {
            if (c < cmax && gpos == (WV_DIR_GROUP - 1) && sgslot == (WV_DIR_SG - 1)) {
                for (int i = 0; i < WV_DIR_SBYTES; ++i) {
#pragma HLS UNROLL
                    ob.range(8 * i + 7, 8 * i) = pk[i];
                }
                for (int i = 0; i < WV_DIR_SBYTES; ++i) {
#pragma HLS UNROLL
                    pk[i] = 0;
                }
                pend = WV_DIR_SBEATS;
            }
            if (pend != 0) {
                dout.write(ob.range(WV_DIR_BEATBITS - 1, 0));
                ob >>= WV_DIR_BEATBITS;
                --pend;
            }
            ++c;
            if (gpos == (WV_DIR_GROUP - 1)) {
                gpos = 0;
                sgslot = (sgslot == (WV_DIR_SG - 1)) ? (ap_uint<6>)0 : (ap_uint<6>)(sgslot + 1);
            } else {
                ++gpos;
            }
        }
        lout.write(WV_LINK_PACK(dh[WV_TILE_PE - 1], rc[WV_TILE_PE - 1],
                                act[WV_TILE_PE - 1], sw_out));
    }
}
#elif S1C_VAR == 10
/* =====================================================================
 * THE SOFTCORE, CONTINUOUS, FLAT (V10). One sweep loop over every token of the call.
 *
 * WHY FLAT: csynth 512 (2026-10-21) put S1's lane body inside a per-band group loop at
 * 26.9K FF vs 15.2K when the same body sweeps once (V9 = S1 exactly): lane state live in
 * and out of the sweep pipeline per group gets staged ~64 deep per lane.
 *
 * WHY nc/nl CARRY `DEPENDENCE inter false`: the next-band block is refilled inside the loop.
 * As a true loop-carried array HLS staged every lane against the refill (41.4K FF, V1).
 * The dependence is false by construction: lane i reads nc[i] only on its pre-marker
 * token (tokens b*S .. b*S+127 of band b), and the tile refills nc only WV_QGUARD tokens
 * after lane 127's switch -- so a read and a write are always > WV_QGUARD > pipeline depth
 * tokens apart, and no reordering inside the pipeline can make them meet. Requires
 * S >= 128 + WV_QGUARD + WV_TILES + slack, which WV_NMIN guarantees.
 * ===================================================================== */
#ifndef WV_QGUARD
#define WV_QGUARD 192
#endif
static void wv_tile_t(hls::stream<wv_cfg> &cin, hls::stream<wv_cfg> &cout,
                      hls::stream<wv_link> &lin, hls::stream<wv_link> &lout,
                      hls::stream<wv_qblk> &qin, hls::stream<wv_qblk> &qfwd,
                      hls::stream<wv_dirbeat> &dout)
{
    const wv_cfg cf = cin.read();
    {
        wv_cfg o = cf;
        o.rem = cf.rem - 1;
        cout.write(o);
    }
    const ap_uint<6> nfwd = (ap_uint<6>)(cf.rem - 1);
    const uint32_t   nt   = cf.T;
    const uint32_t   cmax = cf.cmax;
    const ap_uint<8> nb   = (ap_uint<8>)cf.nb;

    wv_sym     qc[WV_TILE_PE], nc[WV_TILE_PE];
    ap_uint<1> ql[WV_TILE_PE], nl[WV_TILE_PE];
#pragma HLS ARRAY_PARTITION variable = qc complete dim = 0
#pragma HLS ARRAY_PARTITION variable = nc complete dim = 0
#pragma HLS ARRAY_PARTITION variable = ql complete dim = 0
#pragma HLS ARRAY_PARTITION variable = nl complete dim = 0
#pragma HLS DEPENDENCE variable = nc inter false
#pragma HLS DEPENDENCE variable = nl inter false
    for (int i = 0; i < WV_TILE_PE; ++i) {
#pragma HLS UNROLL
        nc[i] = 0; nl[i] = 0; qc[i] = 0; ql[i] = 0;
    }
    ap_uint<8>  loaded   = 0;      /* band blocks taken into nc/nl */
    ap_uint<1>  own_pend = 0;
    ap_uint<6>  fwd_left = 0;
    ap_uint<9>  guard    = 0;      /* tokens left before the refill may happen */

    ap_uint<8> pk[WV_DIR_SBYTES];
#pragma HLS ARRAY_PARTITION variable = pk complete dim = 0
    ap_uint<WV_DIR_SBYTES * 8> ob = 0;
    ap_uint<4> pend = 0;
    bool       started = false;
    uint32_t   c = 0;
    ap_uint<3> gpos   = 0;
    ap_uint<6> sgslot = 0;

    wv_enc     dv [WV_TILE_PE], dh[WV_TILE_PE];
    wv_sym     rc [WV_TILE_PE];
    ap_uint<1> act[WV_TILE_PE], pm[WV_TILE_PE];
#pragma HLS ARRAY_PARTITION variable = dv  complete dim = 0
#pragma HLS ARRAY_PARTITION variable = dh  complete dim = 0
#pragma HLS ARRAY_PARTITION variable = rc  complete dim = 0
#pragma HLS ARRAY_PARTITION variable = act complete dim = 0
#pragma HLS ARRAY_PARTITION variable = pm  complete dim = 0
tile_reset:
    for (int i = 0; i < WV_TILE_PE; ++i) {
#pragma HLS UNROLL
        dv[i] = WV_ENC_GAP; dh[i] = WV_ENC_GAP; rc[i] = 0; act[i] = 0; pm[i] = 0;
    }
pk_reset:
    for (int i = 0; i < WV_DIR_SBYTES; ++i) {
#pragma HLS UNROLL
        pk[i] = 0;
    }

tile_sweep:
    for (uint32_t t = 0; t <= nt; ++t) {
#pragma HLS PIPELINE II = 1
#pragma HLS LOOP_TRIPCOUNT min = 2 max = 16777216
        /* ---- query block traffic: the ONLY loop touching qin/qfwd ---- */
        if (loaded == 0) {
            const wv_qblk b0 = qin.read();          /* iteration 0: blocking start-up */
            for (int i = 0; i < WV_TILE_PE; ++i) {
#pragma HLS UNROLL
                nc[i] = b0.c[i]; nl[i] = b0.live[i];
            }
            loaded = 1; fwd_left = nfwd;
            continue;
        }
        if (fwd_left != 0) {                        /* forwarding first: queue order */
            if (!qin.empty() && !qfwd.full()) { qfwd.write(qin.read()); --fwd_left; }
        } else if (own_pend && guard == 0) {
            wv_qblk nbk;
            if (qin.read_nb(nbk)) {
                for (int i = 0; i < WV_TILE_PE; ++i) {
#pragma HLS UNROLL
                    nc[i] = nbk.c[i]; nl[i] = nbk.live[i];
                }
                own_pend = 0; ++loaded; fwd_left = nfwd;
            }
        }
        if (guard != 0) --guard;

        const wv_link    li    = lin.read();
        const wv_enc     dh_in = WV_LINK_DH(li);
        const wv_sym     rc_in = WV_LINK_RC(li);
        const ap_uint<1> ac_in = WV_LINK_ACT(li);
        const ap_uint<1> pm_in = WV_LINK_PM(li);
        if (ac_in) started = true;
        const bool counting = started && (c < cmax);
        const int pkbase = (int)sgslot * WV_TILE_PE;

        wv_enc     s_dh [WV_TILE_PE];
        wv_sym     s_rc [WV_TILE_PE];
        ap_uint<1> s_act[WV_TILE_PE], s_pm[WV_TILE_PE];
#pragma HLS ARRAY_PARTITION variable = s_dh  complete dim = 0
#pragma HLS ARRAY_PARTITION variable = s_rc  complete dim = 0
#pragma HLS ARRAY_PARTITION variable = s_act complete dim = 0
#pragma HLS ARRAY_PARTITION variable = s_pm  complete dim = 0
        for (int i = 0; i < WV_TILE_PE; ++i) {
#pragma HLS UNROLL
            s_dh[i] = dh[i]; s_rc[i] = rc[i]; s_act[i] = act[i]; s_pm[i] = pm[i];
        }
    tile_pes:
        for (int i = 0; i < WV_TILE_PE; ++i) {
#pragma HLS UNROLL
            const wv_enc     dh_up = (i == 0) ? dh_in : s_dh [i - 1];
            const wv_sym     rsym  = (i == 0) ? rc_in : s_rc [i - 1];
            const ap_uint<1> afl   = (i == 0) ? ac_in : s_act[i - 1];
            const ap_uint<1> pfl   = (i == 0) ? pm_in : s_pm [i - 1];
            ap_uint<2> dcode = 0;
            if (afl && ql[i]) {
                const ap_uint<1> smatch = (qc[i] == rsym) ? 1 : 0;
                const ap_uint<7> idx = (ap_uint<7>)
                    ((smatch, (ap_uint<3>)dh_up, (ap_uint<3>)dv[i]));
                const ap_uint<8> v = NWW_LUT[idx];
                dv[i] = (wv_enc)v(2, 0);
                dh[i] = (wv_enc)v(5, 3);
                dcode = v(7, 6);
            } else if (afl) {
                dh[i] = dh_up;
            }
            rc[i]  = rsym;
            act[i] = afl;
            pm[i]  = pfl;
            /* BAND SWITCH, registered: column 0 (next token) sees the new query, dV = G */
            if (pfl) {
                qc[i] = nc[i]; ql[i] = nl[i]; dv[i] = WV_ENC_GAP;
            }
            if (counting)
                pk[pkbase + i] = (ap_uint<8>)(pk[pkbase + i] * 3 + (ap_uint<8>)dcode);
        }
        /* lane 127 switched: next refill allowed after the guard */
        if (s_pm[WV_TILE_PE - 2] && loaded < nb) { own_pend = 1; guard = WV_QGUARD; }

        if (started && c < cmax + WV_DIR_SBEATS) {
            if (c < cmax && gpos == (WV_DIR_GROUP - 1) && sgslot == (WV_DIR_SG - 1)) {
                for (int i = 0; i < WV_DIR_SBYTES; ++i) {
#pragma HLS UNROLL
                    ob.range(8 * i + 7, 8 * i) = pk[i];
                }
                for (int i = 0; i < WV_DIR_SBYTES; ++i) {
#pragma HLS UNROLL
                    pk[i] = 0;
                }
                pend = WV_DIR_SBEATS;
            }
            if (pend != 0) {
                dout.write(ob.range(WV_DIR_BEATBITS - 1, 0));
                ob >>= WV_DIR_BEATBITS;
                --pend;
            }
            ++c;
            if (gpos == (WV_DIR_GROUP - 1)) {
                gpos = 0;
                sgslot = (sgslot == (WV_DIR_SG - 1)) ? (ap_uint<6>)0 : (ap_uint<6>)(sgslot + 1);
            } else {
                ++gpos;
            }
        }
        lout.write(WV_LINK_PACK(dh[WV_TILE_PE - 1], rc[WV_TILE_PE - 1],
                                act[WV_TILE_PE - 1], pm[WV_TILE_PE - 1]));
    }
}
#else
static void wv_tile_t(hls::stream<wv_cfg> &cin, hls::stream<wv_cfg> &cout,
                      hls::stream<wv_link> &lin, hls::stream<wv_link> &lout,
                      hls::stream<wv_qblk> &qin, hls::stream<wv_qblk> &qfwd,
                      hls::stream<wv_dirbeat> &dout)
{
    const wv_cfg cf = cin.read();
    {
        wv_cfg o = cf;
        o.rem = cf.rem - 1;
        cout.write(o);
    }
    const uint32_t nfwd = cf.rem - 1;
    const uint32_t nt   = cf.T;
    const uint32_t cmax = cf.cmax;
    const uint32_t nb   = cf.nb;
    const uint32_t S    = cf.n + cf.g;

    /* nc/nl: the CURRENT GROUP's block, constant for the whole sweep below -- exactly the
     * role S1's `q` had. qc/ql: per-lane registers, loop-carried at the lane's own stage
     * like dv. A first version kept nc as loop-carried state refilled INSIDE the sweep;
     * with a 133-stage pipeline HLS carried copies through every stage: 41.8K FF / 52.8K
     * LUT per tile against S1's 15.7K / 25.1K, and the fetch counters became the critical
     * path (-0.62 ns). */
    wv_sym     qc[WV_TILE_PE], nc[WV_TILE_PE];
    ap_uint<1> ql[WV_TILE_PE], nl[WV_TILE_PE];
#pragma HLS ARRAY_PARTITION variable = qc complete dim = 0
#pragma HLS ARRAY_PARTITION variable = nc complete dim = 0
#pragma HLS ARRAY_PARTITION variable = ql complete dim = 0
#pragma HLS ARRAY_PARTITION variable = nl complete dim = 0
    for (int i = 0; i < WV_TILE_PE; ++i) {
#pragma HLS UNROLL
        nc[i] = 0; nl[i] = 0; qc[i] = 0; ql[i] = 0;
    }
#if S1C_VAR == 3 || S1C_VAR == 7 || S1C_VAR == 8 || S1C_VAR == 9
    /* V3: two band-invariant block slots selected by the column's band parity. */
    wv_sym     qe[WV_TILE_PE], qo[WV_TILE_PE];
    ap_uint<1> le[WV_TILE_PE], lo[WV_TILE_PE], bpr[WV_TILE_PE];
#pragma HLS ARRAY_PARTITION variable = qe  complete dim = 0
#pragma HLS ARRAY_PARTITION variable = qo  complete dim = 0
#pragma HLS ARRAY_PARTITION variable = le  complete dim = 0
#pragma HLS ARRAY_PARTITION variable = lo  complete dim = 0
#pragma HLS ARRAY_PARTITION variable = bpr complete dim = 0
    for (int i = 0; i < WV_TILE_PE; ++i) {
#pragma HLS UNROLL
        qe[i] = 0; qo[i] = 0; le[i] = 0; lo[i] = 0; bpr[i] = 0;
    }
#endif

    /* base-3 supergroup emitter: unchanged from S1, state persists across groups */
    ap_uint<8> pk[WV_DIR_SBYTES];
#pragma HLS ARRAY_PARTITION variable = pk complete dim = 0
    ap_uint<WV_DIR_SBYTES * 8> ob = 0;
    ap_uint<4> pend = 0;
    bool       started = false;
    uint32_t   c = 0;
    ap_uint<3> gpos   = 0;
    ap_uint<6> sgslot = 0;

    wv_enc     dv [WV_TILE_PE], dh[WV_TILE_PE];
    wv_sym     rc [WV_TILE_PE];
    ap_uint<1> act[WV_TILE_PE], pm[WV_TILE_PE];
#pragma HLS ARRAY_PARTITION variable = dv  complete dim = 0
#pragma HLS ARRAY_PARTITION variable = dh  complete dim = 0
#pragma HLS ARRAY_PARTITION variable = rc  complete dim = 0
#pragma HLS ARRAY_PARTITION variable = act complete dim = 0
#pragma HLS ARRAY_PARTITION variable = pm  complete dim = 0
tile_reset:
    for (int i = 0; i < WV_TILE_PE; ++i) {
#pragma HLS UNROLL
        dv[i] = WV_ENC_GAP; dh[i] = WV_ENC_GAP; rc[i] = 0; act[i] = 0; pm[i] = 0;
    }
pk_reset:
    for (int i = 0; i < WV_DIR_SBYTES; ++i) {
#pragma HLS UNROLL
        pk[i] = 0;
    }

    /* GROUP b = link tokens [b*S, (b+1)*S): it begins with the token carrying band b's
     * pre-marker (token 0 is the lead pre-marker), so lanes switch to block b at tokens
     * b*S .. b*S+127 -- all inside group b, since S >= WV_NMIN > 128. The last group also
     * takes the trailing flush tokens. Between groups only THIS tile's sweep pipeline
     * restarts (~depth cycles); head, tail and the wrap FIFO keep streaming. */
#if S1C_VAR == 9
    {   /* DIAGNOSTIC: single block, single sweep (no per-band groups) */
        const wv_qblk blk0 = qin.read();
        for (int i = 0; i < WV_TILE_PE; ++i) {
#pragma HLS UNROLL
            qe[i] = blk0.c[i]; le[i] = blk0.live[i];
        }
        const uint32_t b = 0; (void)b;
        const uint32_t ntok = nt;
#else
groups:
    for (uint32_t b = 0; b < nb; ++b) {
#pragma HLS LOOP_TRIPCOUNT min = 1 max = 128
        /* ONE loop touches qin/qfwd (hls::task rule: a stream may be accessed by only one
         * loop -- RTGEN 206-102 otherwise). Own block first, then the downstream tiles'. */
    qstep:
        for (uint32_t k = 0; k <= nfwd; ++k) {
#pragma HLS PIPELINE II = 1
#pragma HLS LOOP_TRIPCOUNT min = 1 max = 32
            const wv_qblk blk = qin.read();
            if (k == 0) {
#if S1C_VAR == 3 || S1C_VAR == 7 || S1C_VAR == 8 || S1C_VAR == 9
                if (b & 1) {
                    for (int i = 0; i < WV_TILE_PE; ++i) {
#pragma HLS UNROLL
                        qo[i] = blk.c[i]; lo[i] = blk.live[i];
                    }
                } else {
                    for (int i = 0; i < WV_TILE_PE; ++i) {
#pragma HLS UNROLL
                        qe[i] = blk.c[i]; le[i] = blk.live[i];
                    }
                }
#else
                for (int i = 0; i < WV_TILE_PE; ++i) {
#pragma HLS UNROLL
                    nc[i] = blk.c[i]; nl[i] = blk.live[i];
                }
#endif
            } else {
                qfwd.write(blk);
            }
        }
        const uint32_t ntok = (b == nb - 1) ? (nt - b * S) : S;
#endif
    tile_sweep:
        for (uint32_t t = 0; t < ntok; ++t) {
#pragma HLS PIPELINE II = 1
#pragma HLS LOOP_TRIPCOUNT min = 2 max = 16777216
            const wv_link    li    = lin.read();
            const wv_enc     dh_in = WV_LINK_DH(li);
            const wv_sym     rc_in = WV_LINK_RC(li);
            const ap_uint<1> ac_in = WV_LINK_ACT(li);
            const ap_uint<1> pm_in = WV_LINK_PM(li);
#if S1C_VAR == 3 || S1C_VAR == 7 || S1C_VAR == 8 || S1C_VAR == 9
            const ap_uint<1> bp_in = WV_LINK_BP(li);
            ap_uint<1> s_bp[WV_TILE_PE];
#pragma HLS ARRAY_PARTITION variable = s_bp complete dim = 0
            for (int i = 0; i < WV_TILE_PE; ++i) {
#pragma HLS UNROLL
                s_bp[i] = bpr[i];
            }
#endif
            if (ac_in) started = true;
            const bool counting = started && (c < cmax);
            const int pkbase = (int)sgslot * WV_TILE_PE;

            wv_enc     s_dh [WV_TILE_PE];
            wv_sym     s_rc [WV_TILE_PE];
            ap_uint<1> s_act[WV_TILE_PE], s_pm[WV_TILE_PE];
#pragma HLS ARRAY_PARTITION variable = s_dh  complete dim = 0
#pragma HLS ARRAY_PARTITION variable = s_rc  complete dim = 0
#pragma HLS ARRAY_PARTITION variable = s_act complete dim = 0
#pragma HLS ARRAY_PARTITION variable = s_pm  complete dim = 0
            for (int i = 0; i < WV_TILE_PE; ++i) {
#pragma HLS UNROLL
                s_dh[i] = dh[i]; s_rc[i] = rc[i]; s_act[i] = act[i]; s_pm[i] = pm[i];
            }

        tile_pes:
            for (int i = 0; i < WV_TILE_PE; ++i) {
#pragma HLS UNROLL
                const wv_enc     dh_up = (i == 0) ? dh_in : s_dh [i - 1];
                const wv_sym     rsym  = (i == 0) ? rc_in : s_rc [i - 1];
                const ap_uint<1> afl   = (i == 0) ? ac_in : s_act[i - 1];
                const ap_uint<1> pfl   = (i == 0) ? pm_in : s_pm [i - 1];
                ap_uint<2> dcode = 0;
#if S1C_VAR == 3 || S1C_VAR == 7 || S1C_VAR == 8 || S1C_VAR == 9
                /* Column's band parity arrives with the column (feed-forward, like rc).
                 * Query = pure select between band-invariant slots. dV resets to G on the
                 * pre-marker token, one token before column 0. */
                const ap_uint<1> bpl = (i == 0) ? bp_in : s_bp[i - 1];
#if S1C_VAR == 8 || S1C_VAR == 9
                const wv_sym     qsel = qe[i];          /* DIAGNOSTIC: S1 body, one block */
                const ap_uint<1> lsel = le[i];
#else
                const wv_sym     qsel = bpl ? qo[i] : qe[i];
                const ap_uint<1> lsel = bpl ? lo[i] : le[i];
#endif
                if (afl && lsel) {
                    const ap_uint<1> smatch = (qsel == rsym) ? 1 : 0;
                    const ap_uint<7> idx = (ap_uint<7>)
                        ((smatch, (ap_uint<3>)dh_up, (ap_uint<3>)dv[i]));
                    const ap_uint<8> v = NWW_LUT[idx];
                    dv[i] = (wv_enc)v(2, 0);
                    dh[i] = (wv_enc)v(5, 3);
                    dcode = v(7, 6);
                } else if (afl) {
                    dh[i] = dh_up;             /* dead PE: pass through */
                }
                rc[i]  = rsym;
                act[i] = afl;
#if S1C_VAR == 3
                pm[i]  = pfl;
                if (pfl) dv[i] = WV_ENC_GAP;
#endif
#if S1C_VAR != 8 && S1C_VAR != 9
                bpr[i] = bpl;
#endif
#elif S1C_VAR == 2
                /* BAND SWITCH AT USE: s_pm[i] = this lane saw the pre-marker on the previous
                 * token, so THIS token is column 0 of the new band for lane i. */
                const ap_uint<1> sw  = s_pm[i];
                const wv_sym     qcu = sw ? nc[i] : qc[i];
                const ap_uint<1> qlu = sw ? nl[i] : ql[i];
                const wv_enc     dvu = sw ? (wv_enc)WV_ENC_GAP : dv[i];
                qc[i] = qcu; ql[i] = qlu; dv[i] = dvu;
                if (afl && qlu) {
                    const ap_uint<1> smatch = (qcu == rsym) ? 1 : 0;
                    const ap_uint<7> idx = (ap_uint<7>)
                        ((smatch, (ap_uint<3>)dh_up, (ap_uint<3>)dvu));
                    const ap_uint<8> v = NWW_LUT[idx];
                    dv[i] = (wv_enc)v(2, 0);
                    dh[i] = (wv_enc)v(5, 3);
                    dcode = v(7, 6);
                } else if (afl) {
                    dh[i] = dh_up;             /* dead PE: pass through */
                }
                rc[i]  = rsym;
                act[i] = afl;
                pm[i]  = pfl;
#else
#if S1C_VAR == 5
                if (afl) {                     /* DIAGNOSTIC: ql ignored */
#else
                if (afl && ql[i]) {
#endif
                    const ap_uint<1> smatch = (qc[i] == rsym) ? 1 : 0;
                    const ap_uint<7> idx = (ap_uint<7>)
                        ((smatch, (ap_uint<3>)dh_up, (ap_uint<3>)dv[i]));
                    const ap_uint<8> v = NWW_LUT[idx];
                    dv[i] = (wv_enc)v(2, 0);
                    dh[i] = (wv_enc)v(5, 3);
                    dcode = v(7, 6);
                } else if (afl) {
                    dh[i] = dh_up;             /* dead PE: pass through */
                }
                rc[i]  = rsym;
                act[i] = afl;
                pm[i]  = pfl;
                /* BAND SWITCH, registered, after this cycle's use of qc/ql/dv. The
                 * pre-marker reaches lane i one token before column 0 does, so column 0
                 * sees the new query and dV = G: H[i][0] = i*G in every band, exactly
                 * S1's fresh start. */
                if (pfl) {
#if S1C_VAR == 4
                    qc[i] = nc[i]; ql[i] = nl[i];          /* DIAGNOSTIC: no dv reset */
#else
                    qc[i] = nc[i]; ql[i] = nl[i]; dv[i] = WV_ENC_GAP;
#endif
                }
#endif
                if (counting)
                    pk[pkbase + i] = (ap_uint<8>)(pk[pkbase + i] * 3 + (ap_uint<8>)dcode);
            }

            if (started && c < cmax + WV_DIR_SBEATS) {
                if (c < cmax && gpos == (WV_DIR_GROUP - 1) && sgslot == (WV_DIR_SG - 1)) {
                    for (int i = 0; i < WV_DIR_SBYTES; ++i) {
#pragma HLS UNROLL
                        ob.range(8 * i + 7, 8 * i) = pk[i];
                    }
                    for (int i = 0; i < WV_DIR_SBYTES; ++i) {
#pragma HLS UNROLL
                        pk[i] = 0;
                    }
                    pend = WV_DIR_SBEATS;
                }
                if (pend != 0) {
                    dout.write(ob.range(WV_DIR_BEATBITS - 1, 0));
                    ob >>= WV_DIR_BEATBITS;
                    --pend;
                }
                ++c;
                if (gpos == (WV_DIR_GROUP - 1)) {
                    gpos = 0;
                    sgslot = (sgslot == (WV_DIR_SG - 1)) ? (ap_uint<6>)0 : (ap_uint<6>)(sgslot + 1);
                } else {
                    ++gpos;
                }
            }
#if S1C_VAR == 3 || S1C_VAR == 7 || S1C_VAR == 8 || S1C_VAR == 9
            lout.write(WV_LINK_PACK5(dh[WV_TILE_PE - 1], rc[WV_TILE_PE - 1],
                                     act[WV_TILE_PE - 1], pm[WV_TILE_PE - 1], bpr[WV_TILE_PE - 1]));
#else
            lout.write(WV_LINK_PACK(dh[WV_TILE_PE - 1], rc[WV_TILE_PE - 1],
                                    act[WV_TILE_PE - 1], pm[WV_TILE_PE - 1]));
#endif
        }
    }
}

#endif  /* S1C_VAR == 10 */

/* =====================================================================
 * HEAD (task). Token 0 is a lone pre-marker. Then for each band: n real columns, then g
 * bubbles; the pre-marker rides the token just before the next band's column 0. Band 0's
 * top row is dH = G; later bands take the previous band's bottom row from `wrap`.
 * ===================================================================== */
static void wv_head_t(hls::stream<wv_cfg> &cin, hls::stream<uint64_t> &rws,
                      hls::stream<uint64_t> &wrap, hls::stream<wv_link> &lout)
{
    const wv_cfg cf = cin.read();
    uint32_t b = 0, j = 0, gl = 0;
    bool     lead = true;
    uint64_t rw = 0, bw = 0;
head:
    for (uint32_t t = 0; t < cf.T; ++t) {
#pragma HLS PIPELINE II = 1
#pragma HLS LOOP_TRIPCOUNT min = 2 max = 16777216
        wv_enc     dh = WV_ENC_GAP;
        wv_sym     r  = 0;
        ap_uint<1> a  = 0, p = 0;
        uint32_t   b_of_col = 0;
        if (lead) {
#if S1C_VAR != 11 && S1C_VAR != 12
            p = 1;                             /* V11: bit 6 means "tile-wide swap" */
#endif
            lead = false;
        } else if (b < cf.nb) {
            if (gl != 0) {
                --gl;
#if S1C_VAR == 11 || S1C_VAR == 12
                if (gl == cf.g - WV_BSWAP) p = 1;   /* tile-wide swap token */
#else
                if (gl == 0) p = 1;            /* last bubble precedes column 0 */
#endif
            } else {
                const uint32_t s = j & (WV_PER_WORD - 1);
                if (s == 0) {
                    rw = rws.read();
                    if (b != 0) bw = wrap.read();
                }
                r = (wv_sym)((rw >> (WV_RBITS * s)) & 0x3);
                a = 1;
                b_of_col = b;
                if (b != 0) {
                    const int bcode = (int)((bw >> (2 * s)) & 0x3);
                    dh = (wv_enc)NWW_ENC(bcode * 3 - 2);
                }
                if (j == cf.n - 1) {
                    j = 0; ++b;
                    if (b < cf.nb) {
#if S1C_VAR == 11 || S1C_VAR == 12
                        gl = cf.g;
#else
                        if (cf.g == 0) p = 1; else gl = cf.g;
#endif
                    }
                } else {
                    ++j;
                }
            }
        }
        lout.write(WV_LINK_PACK5(dh, r, a, p, (ap_uint<1>)(a ? (b_of_col & 1) : 0)));
    }
}

/* =====================================================================
 * TAIL (task). Counts real columns (act) -- position is counted, never derived from t.
 * Bands 0..nb-2: bottom row -> wrap (code = (dH+2)/3, 2 bits, 32 per word, partial word
 * flushed at the band's last column). Last band: telescope dH into the score.
 * ===================================================================== */
static void wv_tail_t(hls::stream<wv_cfg> &cin, hls::stream<wv_link> &lin,
                      hls::stream<uint64_t> &wrap, hls::stream<int64_t> &sout)
{
    const wv_cfg cf = cin.read();
    int64_t  acc  = cf.acc0;
    uint32_t j = 0, b = 0;
    uint64_t wacc = 0;
tail:
    for (uint32_t t = 0; t < cf.T; ++t) {
#pragma HLS PIPELINE II = 1
#pragma HLS LOOP_TRIPCOUNT min = 2 max = 16777216
        const wv_link l = lin.read();
        if (WV_LINK_ACT(l)) {
            const int dh_out = NWW_DEC((int)WV_LINK_DH(l));
            if (b == cf.nb - 1) {
                acc += (int64_t)dh_out;
            } else {
                const uint64_t code = (uint64_t)((dh_out + 2) / 3);   /* -2,1,4 -> 0,1,2 */
                wacc |= code << (2 * (j & 31));
                if ((j & 31) == 31 || j == cf.n - 1) { wrap.write(wacc); wacc = 0; }
            }
            if (j == cf.n - 1) { j = 0; ++b; } else { ++j; }
        }
    }
    sout.write(acc);
}

/* =====================================================================
 * Control-driven processes (they touch on-chip ARRAYS, which tasks cannot).
 * ===================================================================== */
static void wv_feed(const uint64_t *ubr, uint32_t n, uint32_t nb, uint32_t g, uint32_t T,
                    uint32_t cmax, int64_t acc0,
                    hls::stream<wv_cfg> &ch, hls::stream<wv_cfg> &cc,
                    hls::stream<uint64_t> &rws)
{
#pragma HLS INLINE off
    wv_cfg cf;
    cf.T = T; cf.n = n; cf.g = g; cf.nb = nb; cf.cmax = cmax; cf.acc0 = acc0;
    cf.rem = WV_TILES;
    ch.write(cf);
    cc.write(cf);
    const uint32_t rwords = (n + WV_PER_WORD - 1) / WV_PER_WORD;
    uint32_t w = 0, b = 0;
feed:
    for (uint32_t k = 0; k < nb * rwords; ++k) {
#pragma HLS PIPELINE II = 1
#pragma HLS LOOP_TRIPCOUNT min = 1 max = 16777216
        rws.write(ubr[w]);
        if (w == rwords - 1) { w = 0; ++b; } else { ++w; }
    }
}

/* One block per cycle: a block is 128 rows = 4 query words, and its first row
 * band*WV_PE + k*128 is a multiple of 128, so it is exactly one 256-bit ubq4 entry. */
static void wv_qload(const ap_uint<256> *ubq4, int64_t m, uint32_t nb, hls::stream<wv_qblk> &qout)
{
#pragma HLS INLINE off
    uint32_t k = 0;
    int64_t  row0 = 0;
qload:
    for (uint32_t x = 0; x < nb * WV_TILES; ++x) {
#pragma HLS PIPELINE II = 1
#pragma HLS LOOP_TRIPCOUNT min = 1 max = 2097152
        wv_qblk blk;
        const ap_uint<256> q4 = ubq4[(uint32_t)(row0 / WV_TILE_PE)];
        for (int w = 0; w < 4; ++w) {
#pragma HLS UNROLL
            const uint64_t qw = (uint64_t)q4.range(64 * w + 63, 64 * w);
            for (int l = 0; l < WV_PER_WORD; ++l) {
#pragma HLS UNROLL
                const int     i   = w * WV_PER_WORD + l;
                const int64_t row = row0 + i;
                blk.c[i]    = (row < m) ? (wv_sym)((qw >> (WV_RBITS * l)) & 0x3) : (wv_sym)0;
                blk.live[i] = (row < m) ? (ap_uint<1>)1 : (ap_uint<1>)0;
            }
        }
        qout.write(blk);
        row0 += WV_TILE_PE;
    }
}

static void wv_dirwriter(hls::stream<wv_dirbeat> &din, wv_dirbeat *dir, uint32_t nbeats)
{
#pragma HLS INLINE off
dirw:
    for (uint32_t b = 0; b < nbeats; ++b) {
#pragma HLS PIPELINE II = 1
#pragma HLS LOOP_TRIPCOUNT min = 1 max = 8388608
        dir[b] = din.read();
    }
}

static void wv_collect(hls::stream<int64_t> &sin, int64_t *score_out)
{
#pragma HLS INLINE off
    score_out[0] = sin.read();
}

static void wv_ring(const ap_uint<256> *ubq4, const uint64_t *ubr, int64_t *score_out,
                    int64_t m, uint32_t n, uint32_t nb, uint32_t g, uint32_t T,
                    uint32_t cmax, int64_t acc0, uint32_t nbeats
#if WV_NCH > 0
                  , wv_dirbeat *d00
#endif
#if WV_NCH > 1
                  , wv_dirbeat *d01
#endif
#if WV_NCH > 2
                  , wv_dirbeat *d02
#endif
#if WV_NCH > 3
                  , wv_dirbeat *d03
#endif
#if WV_NCH > 4
                  , wv_dirbeat *d04
#endif
#if WV_NCH > 5
                  , wv_dirbeat *d05
#endif
#if WV_NCH > 6
                  , wv_dirbeat *d06
#endif
#if WV_NCH > 7
                  , wv_dirbeat *d07
#endif
#if WV_NCH > 8
                  , wv_dirbeat *d08
#endif
#if WV_NCH > 9
                  , wv_dirbeat *d09
#endif
#if WV_NCH > 10
                  , wv_dirbeat *d10
#endif
#if WV_NCH > 11
                  , wv_dirbeat *d11
#endif
#if WV_NCH > 12
                  , wv_dirbeat *d12
#endif
#if WV_NCH > 13
                  , wv_dirbeat *d13
#endif
#if WV_NCH > 14
                  , wv_dirbeat *d14
#endif
#if WV_NCH > 15
                  , wv_dirbeat *d15
#endif
#if WV_NCH > 16
                  , wv_dirbeat *d16
#endif
#if WV_NCH > 17
                  , wv_dirbeat *d17
#endif
#if WV_NCH > 18
                  , wv_dirbeat *d18
#endif
#if WV_NCH > 19
                  , wv_dirbeat *d19
#endif
#if WV_NCH > 20
                  , wv_dirbeat *d20
#endif
#if WV_NCH > 21
                  , wv_dirbeat *d21
#endif
#if WV_NCH > 22
                  , wv_dirbeat *d22
#endif
#if WV_NCH > 23
                  , wv_dirbeat *d23
#endif
#if WV_NCH > 24
                  , wv_dirbeat *d24
#endif
#if WV_NCH > 25
                  , wv_dirbeat *d25
#endif
#if WV_NCH > 26
                  , wv_dirbeat *d26
#endif
#if WV_NCH > 27
                  , wv_dirbeat *d27
#endif
#if WV_NCH > 28
                  , wv_dirbeat *d28
#endif
#if WV_NCH > 29
                  , wv_dirbeat *d29
#endif
#if WV_NCH > 30
                  , wv_dirbeat *d30
#endif
#if WV_NCH > 31
                  , wv_dirbeat *d31
#endif
                    )
{
#pragma HLS INLINE off
#pragma HLS DATAFLOW
    /* lk DEPTH 256: each tile pauses ~170 cycles per band (qstep + pipeline restart).
     * A shallow link FIFO would back that pause into every upstream tile, so the 31
     * pauses would ADD (~2.7% at 181k); 256 lets them overlap (~0.1%). 8-bit x 256 is SRL. */
    HLS_TASK_STREAM<wv_link, 256>  lk[WV_TILES + 1];
    /* qs DEPTH = a whole band of downstream blocks. Tile k forwards nfwd blocks at its
     * group boundary; tile k+1 reads them only at ITS boundary, 127 tokens later, and
     * reaching it needs tokens tile k emits only after forwarding. Depth 2 deadlocked
     * in cosim 512 (tile0 blocked on full qs_1, tile1 on empty lk). */
    HLS_TASK_STREAM<wv_qblk, WV_TILES + 1>  qs[WV_TILES + 1];
#pragma HLS BIND_STORAGE variable = qs type = fifo impl = bram
    HLS_TASK_STREAM<wv_cfg, 2>   cs[WV_TILES + 1];
    HLS_TASK_STREAM<wv_dirbeat, WV_DIRFIFO> ds[WV_TILES];
    HLS_TASK_STREAM<wv_cfg, 2>   ch;
    HLS_TASK_STREAM<uint64_t, 16> rws;
    HLS_TASK_STREAM<uint64_t, (WV_NMAX + 31) / 32> wrap;
#pragma HLS BIND_STORAGE variable = wrap type = fifo impl = bram
    HLS_TASK_STREAM<int64_t, 2>  sc;
#if defined(WV_SEAM_RELAY) && WV_TILES > 16
    HLS_TASK_STREAM<wv_link, 256>  lsr[WV_SEAM_STAGES];
    HLS_TASK_STREAM<wv_qblk, WV_TILES + 1>  qsr[WV_SEAM_STAGES];
#pragma HLS BIND_STORAGE variable = qsr type = fifo impl = bram
    HLS_TASK_STREAM<wv_cfg, 2>   csr[WV_SEAM_STAGES];
#endif

    wv_feed(ubr, n, nb, g, T, cmax, acc0, ch, cs[0], rws);
    wv_qload(ubq4, m, nb, qs[0]);
    HLS_TASK t_head(wv_head_t, ch, rws, wrap, lk[0]);
#if WV_TILES > 0
    HLS_TASK t_tile00(wv_tile_t, cs[0], cs[1], lk[0], lk[1], qs[0], qs[1], ds[0]);
#endif
#if WV_TILES > 1
    HLS_TASK t_tile01(wv_tile_t, cs[1], cs[2], lk[1], lk[2], qs[1], qs[2], ds[1]);
#endif
#if WV_TILES > 2
    HLS_TASK t_tile02(wv_tile_t, cs[2], cs[3], lk[2], lk[3], qs[2], qs[3], ds[2]);
#endif
#if WV_TILES > 3
    HLS_TASK t_tile03(wv_tile_t, cs[3], cs[4], lk[3], lk[4], qs[3], qs[4], ds[3]);
#endif
#if WV_TILES > 4
    HLS_TASK t_tile04(wv_tile_t, cs[4], cs[5], lk[4], lk[5], qs[4], qs[5], ds[4]);
#endif
#if WV_TILES > 5
    HLS_TASK t_tile05(wv_tile_t, cs[5], cs[6], lk[5], lk[6], qs[5], qs[6], ds[5]);
#endif
#if WV_TILES > 6
    HLS_TASK t_tile06(wv_tile_t, cs[6], cs[7], lk[6], lk[7], qs[6], qs[7], ds[6]);
#endif
#if WV_TILES > 7
    HLS_TASK t_tile07(wv_tile_t, cs[7], cs[8], lk[7], lk[8], qs[7], qs[8], ds[7]);
#endif
#if WV_TILES > 8
    HLS_TASK t_tile08(wv_tile_t, cs[8], cs[9], lk[8], lk[9], qs[8], qs[9], ds[8]);
#endif
#if WV_TILES > 9
    HLS_TASK t_tile09(wv_tile_t, cs[9], cs[10], lk[9], lk[10], qs[9], qs[10], ds[9]);
#endif
#if WV_TILES > 10
    HLS_TASK t_tile10(wv_tile_t, cs[10], cs[11], lk[10], lk[11], qs[10], qs[11], ds[10]);
#endif
#if WV_TILES > 11
    HLS_TASK t_tile11(wv_tile_t, cs[11], cs[12], lk[11], lk[12], qs[11], qs[12], ds[11]);
#endif
#if WV_TILES > 12
    HLS_TASK t_tile12(wv_tile_t, cs[12], cs[13], lk[12], lk[13], qs[12], qs[13], ds[12]);
#endif
#if WV_TILES > 13
    HLS_TASK t_tile13(wv_tile_t, cs[13], cs[14], lk[13], lk[14], qs[13], qs[14], ds[13]);
#endif
#if WV_TILES > 14
    HLS_TASK t_tile14(wv_tile_t, cs[14], cs[15], lk[14], lk[15], qs[14], qs[15], ds[14]);
#endif
#if WV_TILES > 15
    HLS_TASK t_tile15(wv_tile_t, cs[15], cs[16], lk[15], lk[16], qs[15], qs[16], ds[15]);
#endif
#if defined(WV_SEAM_RELAY) && WV_TILES > 16
#if WV_SEAM_AT != 16
#error "S1c instantiates the seam relay between tiles 15 and 16 only"
#endif
#if WV_SEAM_STAGES != 3
#error "S1c instantiates exactly 3 seam relay stages"
#endif
    HLS_TASK t_lr0(wv_relay_t<wv_link>, lk[16], lsr[0]);
    HLS_TASK t_lr1(wv_relay_t<wv_link>, lsr[0], lsr[1]);
    HLS_TASK t_lr2(wv_relay_t<wv_link>, lsr[1], lsr[2]);
    HLS_TASK t_qr0(wv_relay_t<wv_qblk>, qs[16], qsr[0]);
    HLS_TASK t_qr1(wv_relay_t<wv_qblk>, qsr[0], qsr[1]);
    HLS_TASK t_qr2(wv_relay_t<wv_qblk>, qsr[1], qsr[2]);
    HLS_TASK t_cr0(wv_relay_t<wv_cfg>,  cs[16], csr[0]);
    HLS_TASK t_cr1(wv_relay_t<wv_cfg>,  csr[0], csr[1]);
    HLS_TASK t_cr2(wv_relay_t<wv_cfg>,  csr[1], csr[2]);
#if WV_TILES > 16
    HLS_TASK t_tile16(wv_tile_t, csr[2], cs[17], lsr[2], lk[17], qsr[2], qs[17], ds[16]);
#endif
#else
#if WV_TILES > 16
    HLS_TASK t_tile16(wv_tile_t, cs[16], cs[17], lk[16], lk[17], qs[16], qs[17], ds[16]);
#endif
#endif
#if WV_TILES > 17
    HLS_TASK t_tile17(wv_tile_t, cs[17], cs[18], lk[17], lk[18], qs[17], qs[18], ds[17]);
#endif
#if WV_TILES > 18
    HLS_TASK t_tile18(wv_tile_t, cs[18], cs[19], lk[18], lk[19], qs[18], qs[19], ds[18]);
#endif
#if WV_TILES > 19
    HLS_TASK t_tile19(wv_tile_t, cs[19], cs[20], lk[19], lk[20], qs[19], qs[20], ds[19]);
#endif
#if WV_TILES > 20
    HLS_TASK t_tile20(wv_tile_t, cs[20], cs[21], lk[20], lk[21], qs[20], qs[21], ds[20]);
#endif
#if WV_TILES > 21
    HLS_TASK t_tile21(wv_tile_t, cs[21], cs[22], lk[21], lk[22], qs[21], qs[22], ds[21]);
#endif
#if WV_TILES > 22
    HLS_TASK t_tile22(wv_tile_t, cs[22], cs[23], lk[22], lk[23], qs[22], qs[23], ds[22]);
#endif
#if WV_TILES > 23
    HLS_TASK t_tile23(wv_tile_t, cs[23], cs[24], lk[23], lk[24], qs[23], qs[24], ds[23]);
#endif
#if WV_TILES > 24
    HLS_TASK t_tile24(wv_tile_t, cs[24], cs[25], lk[24], lk[25], qs[24], qs[25], ds[24]);
#endif
#if WV_TILES > 25
    HLS_TASK t_tile25(wv_tile_t, cs[25], cs[26], lk[25], lk[26], qs[25], qs[26], ds[25]);
#endif
#if WV_TILES > 26
    HLS_TASK t_tile26(wv_tile_t, cs[26], cs[27], lk[26], lk[27], qs[26], qs[27], ds[26]);
#endif
#if WV_TILES > 27
    HLS_TASK t_tile27(wv_tile_t, cs[27], cs[28], lk[27], lk[28], qs[27], qs[28], ds[27]);
#endif
#if WV_TILES > 28
    HLS_TASK t_tile28(wv_tile_t, cs[28], cs[29], lk[28], lk[29], qs[28], qs[29], ds[28]);
#endif
#if WV_TILES > 29
    HLS_TASK t_tile29(wv_tile_t, cs[29], cs[30], lk[29], lk[30], qs[29], qs[30], ds[29]);
#endif
#if WV_TILES > 30
    HLS_TASK t_tile30(wv_tile_t, cs[30], cs[31], lk[30], lk[31], qs[30], qs[31], ds[30]);
#endif
#if WV_TILES > 31
    HLS_TASK t_tile31(wv_tile_t, cs[31], cs[32], lk[31], lk[32], qs[31], qs[32], ds[31]);
#endif
    HLS_TASK t_tail(wv_tail_t, cs[WV_TILES], lk[WV_TILES], wrap, sc);
#if WV_TILES > 0
    wv_dirwriter(ds[0], d00, nbeats);
#endif
#if WV_TILES > 1
    wv_dirwriter(ds[1], d01, nbeats);
#endif
#if WV_TILES > 2
    wv_dirwriter(ds[2], d02, nbeats);
#endif
#if WV_TILES > 3
    wv_dirwriter(ds[3], d03, nbeats);
#endif
#if WV_TILES > 4
    wv_dirwriter(ds[4], d04, nbeats);
#endif
#if WV_TILES > 5
    wv_dirwriter(ds[5], d05, nbeats);
#endif
#if WV_TILES > 6
    wv_dirwriter(ds[6], d06, nbeats);
#endif
#if WV_TILES > 7
    wv_dirwriter(ds[7], d07, nbeats);
#endif
#if WV_TILES > 8
    wv_dirwriter(ds[8], d08, nbeats);
#endif
#if WV_TILES > 9
    wv_dirwriter(ds[9], d09, nbeats);
#endif
#if WV_TILES > 10
    wv_dirwriter(ds[10], d10, nbeats);
#endif
#if WV_TILES > 11
    wv_dirwriter(ds[11], d11, nbeats);
#endif
#if WV_TILES > 12
    wv_dirwriter(ds[12], d12, nbeats);
#endif
#if WV_TILES > 13
    wv_dirwriter(ds[13], d13, nbeats);
#endif
#if WV_TILES > 14
    wv_dirwriter(ds[14], d14, nbeats);
#endif
#if WV_TILES > 15
    wv_dirwriter(ds[15], d15, nbeats);
#endif
#if WV_TILES > 16
    wv_dirwriter(ds[16], d16, nbeats);
#endif
#if WV_TILES > 17
    wv_dirwriter(ds[17], d17, nbeats);
#endif
#if WV_TILES > 18
    wv_dirwriter(ds[18], d18, nbeats);
#endif
#if WV_TILES > 19
    wv_dirwriter(ds[19], d19, nbeats);
#endif
#if WV_TILES > 20
    wv_dirwriter(ds[20], d20, nbeats);
#endif
#if WV_TILES > 21
    wv_dirwriter(ds[21], d21, nbeats);
#endif
#if WV_TILES > 22
    wv_dirwriter(ds[22], d22, nbeats);
#endif
#if WV_TILES > 23
    wv_dirwriter(ds[23], d23, nbeats);
#endif
#if WV_TILES > 24
    wv_dirwriter(ds[24], d24, nbeats);
#endif
#if WV_TILES > 25
    wv_dirwriter(ds[25], d25, nbeats);
#endif
#if WV_TILES > 26
    wv_dirwriter(ds[26], d26, nbeats);
#endif
#if WV_TILES > 27
    wv_dirwriter(ds[27], d27, nbeats);
#endif
#if WV_TILES > 28
    wv_dirwriter(ds[28], d28, nbeats);
#endif
#if WV_TILES > 29
    wv_dirwriter(ds[29], d29, nbeats);
#endif
#if WV_TILES > 30
    wv_dirwriter(ds[30], d30, nbeats);
#endif
#if WV_TILES > 31
    wv_dirwriter(ds[31], d31, nbeats);
#endif
    wv_collect(sc, score_out);
}

/* S1t: on-card traceback, run after the ring when mem[b_off + WV_TBD_EN] == 1 */
#include "wv_tb_walk.h"

extern "C" {

void nw_tile_u50(uint64_t       *mem,        /* THE one control bundle */
                 int64_t         q_off,      /* word offsets into mem */
                 int64_t         r_off,
                 int64_t         b_off,
                 int64_t         s_off,
                 int64_t         m,
                 int64_t         n
#ifdef WV_TRACEBACK
#if WV_NCH > 0
               , wv_dirbeat *d00
#endif
#if WV_NCH > 1
               , wv_dirbeat *d01
#endif
#if WV_NCH > 2
               , wv_dirbeat *d02
#endif
#if WV_NCH > 3
               , wv_dirbeat *d03
#endif
#if WV_NCH > 4
               , wv_dirbeat *d04
#endif
#if WV_NCH > 5
               , wv_dirbeat *d05
#endif
#if WV_NCH > 6
               , wv_dirbeat *d06
#endif
#if WV_NCH > 7
               , wv_dirbeat *d07
#endif
#if WV_NCH > 8
               , wv_dirbeat *d08
#endif
#if WV_NCH > 9
               , wv_dirbeat *d09
#endif
#if WV_NCH > 10
               , wv_dirbeat *d10
#endif
#if WV_NCH > 11
               , wv_dirbeat *d11
#endif
#if WV_NCH > 12
               , wv_dirbeat *d12
#endif
#if WV_NCH > 13
               , wv_dirbeat *d13
#endif
#if WV_NCH > 14
               , wv_dirbeat *d14
#endif
#if WV_NCH > 15
               , wv_dirbeat *d15
#endif
#if WV_NCH > 16
               , wv_dirbeat *d16
#endif
#if WV_NCH > 17
               , wv_dirbeat *d17
#endif
#if WV_NCH > 18
               , wv_dirbeat *d18
#endif
#if WV_NCH > 19
               , wv_dirbeat *d19
#endif
#if WV_NCH > 20
               , wv_dirbeat *d20
#endif
#if WV_NCH > 21
               , wv_dirbeat *d21
#endif
#if WV_NCH > 22
               , wv_dirbeat *d22
#endif
#if WV_NCH > 23
               , wv_dirbeat *d23
#endif
#if WV_NCH > 24
               , wv_dirbeat *d24
#endif
#if WV_NCH > 25
               , wv_dirbeat *d25
#endif
#if WV_NCH > 26
               , wv_dirbeat *d26
#endif
#if WV_NCH > 27
               , wv_dirbeat *d27
#endif
#if WV_NCH > 28
               , wv_dirbeat *d28
#endif
#if WV_NCH > 29
               , wv_dirbeat *d29
#endif
#if WV_NCH > 30
               , wv_dirbeat *d30
#endif
#if WV_NCH > 31
               , wv_dirbeat *d31
#endif
#endif
                 )
{
/* ONE bundle for every control/data access. This is the whole point of the file: it is
 * touched only by the sequential code below, never by a DATAFLOW process, so HLS emits
 * a single m_axi master and the design costs 1 of the 33 HBM connections instead of 5.
 * Reads (query, reference, boundary) and the one write (score, boundary) share it; a
 * bundle carries both channels, and only the single-PROCESS rule ever mattered. */
#pragma HLS INTERFACE m_axi port = mem bundle = gmem depth = WV_MEMDEPTH \
    num_read_outstanding = 32 num_write_outstanding = 32 \
    max_read_burst_length = 64 max_write_burst_length = 64
#ifdef WV_TRACEBACK
/* One bundle per tile. Each needs 12.8 GB/s (256 bit/cycle at 400 MHz), which is MORE
 * than one pseudo-channel's 9.88, so each maps to a RANGE of PCs -- see tile_link_tb.cfg.
 * Eight PCs each puts all 32 in use, giving 8.19 GB and the 181 kbp square capacity
 * limit, with 6.2x bandwidth headroom per port. The headroom matters because DATAFLOW
 * backpressure is global: a stalled writer fills its FIFO, and once that is full the
 * tile stalls, and the whole chain behind it. WV_DIRFIFO is what buys the slack. */
#if WV_NCH > 0
#pragma HLS INTERFACE m_axi port = d00 bundle = gd00 depth = WV_DIRDEPTH \
    num_write_outstanding = 32 max_write_burst_length = 64
#endif
#if WV_NCH > 1
#pragma HLS INTERFACE m_axi port = d01 bundle = gd01 depth = WV_DIRDEPTH \
    num_write_outstanding = 32 max_write_burst_length = 64
#endif
#if WV_NCH > 2
#pragma HLS INTERFACE m_axi port = d02 bundle = gd02 depth = WV_DIRDEPTH \
    num_write_outstanding = 32 max_write_burst_length = 64
#endif
#if WV_NCH > 3
#pragma HLS INTERFACE m_axi port = d03 bundle = gd03 depth = WV_DIRDEPTH \
    num_write_outstanding = 32 max_write_burst_length = 64
#endif
#if WV_NCH > 4
#pragma HLS INTERFACE m_axi port = d04 bundle = gd04 depth = WV_DIRDEPTH \
    num_write_outstanding = 32 max_write_burst_length = 64
#endif
#if WV_NCH > 5
#pragma HLS INTERFACE m_axi port = d05 bundle = gd05 depth = WV_DIRDEPTH \
    num_write_outstanding = 32 max_write_burst_length = 64
#endif
#if WV_NCH > 6
#pragma HLS INTERFACE m_axi port = d06 bundle = gd06 depth = WV_DIRDEPTH \
    num_write_outstanding = 32 max_write_burst_length = 64
#endif
#if WV_NCH > 7
#pragma HLS INTERFACE m_axi port = d07 bundle = gd07 depth = WV_DIRDEPTH \
    num_write_outstanding = 32 max_write_burst_length = 64
#endif
#if WV_NCH > 8
#pragma HLS INTERFACE m_axi port = d08 bundle = gd08 depth = WV_DIRDEPTH \
    num_write_outstanding = 32 max_write_burst_length = 64
#endif
#if WV_NCH > 9
#pragma HLS INTERFACE m_axi port = d09 bundle = gd09 depth = WV_DIRDEPTH \
    num_write_outstanding = 32 max_write_burst_length = 64
#endif
#if WV_NCH > 10
#pragma HLS INTERFACE m_axi port = d10 bundle = gd10 depth = WV_DIRDEPTH \
    num_write_outstanding = 32 max_write_burst_length = 64
#endif
#if WV_NCH > 11
#pragma HLS INTERFACE m_axi port = d11 bundle = gd11 depth = WV_DIRDEPTH \
    num_write_outstanding = 32 max_write_burst_length = 64
#endif
#if WV_NCH > 12
#pragma HLS INTERFACE m_axi port = d12 bundle = gd12 depth = WV_DIRDEPTH \
    num_write_outstanding = 32 max_write_burst_length = 64
#endif
#if WV_NCH > 13
#pragma HLS INTERFACE m_axi port = d13 bundle = gd13 depth = WV_DIRDEPTH \
    num_write_outstanding = 32 max_write_burst_length = 64
#endif
#if WV_NCH > 14
#pragma HLS INTERFACE m_axi port = d14 bundle = gd14 depth = WV_DIRDEPTH \
    num_write_outstanding = 32 max_write_burst_length = 64
#endif
#if WV_NCH > 15
#pragma HLS INTERFACE m_axi port = d15 bundle = gd15 depth = WV_DIRDEPTH \
    num_write_outstanding = 32 max_write_burst_length = 64
#endif
#if WV_NCH > 16
#pragma HLS INTERFACE m_axi port = d16 bundle = gd16 depth = WV_DIRDEPTH \
    num_write_outstanding = 32 max_write_burst_length = 64
#endif
#if WV_NCH > 17
#pragma HLS INTERFACE m_axi port = d17 bundle = gd17 depth = WV_DIRDEPTH \
    num_write_outstanding = 32 max_write_burst_length = 64
#endif
#if WV_NCH > 18
#pragma HLS INTERFACE m_axi port = d18 bundle = gd18 depth = WV_DIRDEPTH \
    num_write_outstanding = 32 max_write_burst_length = 64
#endif
#if WV_NCH > 19
#pragma HLS INTERFACE m_axi port = d19 bundle = gd19 depth = WV_DIRDEPTH \
    num_write_outstanding = 32 max_write_burst_length = 64
#endif
#if WV_NCH > 20
#pragma HLS INTERFACE m_axi port = d20 bundle = gd20 depth = WV_DIRDEPTH \
    num_write_outstanding = 32 max_write_burst_length = 64
#endif
#if WV_NCH > 21
#pragma HLS INTERFACE m_axi port = d21 bundle = gd21 depth = WV_DIRDEPTH \
    num_write_outstanding = 32 max_write_burst_length = 64
#endif
#if WV_NCH > 22
#pragma HLS INTERFACE m_axi port = d22 bundle = gd22 depth = WV_DIRDEPTH \
    num_write_outstanding = 32 max_write_burst_length = 64
#endif
#if WV_NCH > 23
#pragma HLS INTERFACE m_axi port = d23 bundle = gd23 depth = WV_DIRDEPTH \
    num_write_outstanding = 32 max_write_burst_length = 64
#endif
#if WV_NCH > 24
#pragma HLS INTERFACE m_axi port = d24 bundle = gd24 depth = WV_DIRDEPTH \
    num_write_outstanding = 32 max_write_burst_length = 64
#endif
#if WV_NCH > 25
#pragma HLS INTERFACE m_axi port = d25 bundle = gd25 depth = WV_DIRDEPTH \
    num_write_outstanding = 32 max_write_burst_length = 64
#endif
#if WV_NCH > 26
#pragma HLS INTERFACE m_axi port = d26 bundle = gd26 depth = WV_DIRDEPTH \
    num_write_outstanding = 32 max_write_burst_length = 64
#endif
#if WV_NCH > 27
#pragma HLS INTERFACE m_axi port = d27 bundle = gd27 depth = WV_DIRDEPTH \
    num_write_outstanding = 32 max_write_burst_length = 64
#endif
#if WV_NCH > 28
#pragma HLS INTERFACE m_axi port = d28 bundle = gd28 depth = WV_DIRDEPTH \
    num_write_outstanding = 32 max_write_burst_length = 64
#endif
#if WV_NCH > 29
#pragma HLS INTERFACE m_axi port = d29 bundle = gd29 depth = WV_DIRDEPTH \
    num_write_outstanding = 32 max_write_burst_length = 64
#endif
#if WV_NCH > 30
#pragma HLS INTERFACE m_axi port = d30 bundle = gd30 depth = WV_DIRDEPTH \
    num_write_outstanding = 32 max_write_burst_length = 64
#endif
#if WV_NCH > 31
#pragma HLS INTERFACE m_axi port = d31 bundle = gd31 depth = WV_DIRDEPTH \
    num_write_outstanding = 32 max_write_burst_length = 64
#endif
#endif

    /* ---- ON-CHIP BUFFERS. The whole query AND the whole reference live on chip; the
     * ring reads them through control-driven feeder processes. The query is stored as
     * one 256-bit entry per 128-row block so the loader builds a block per cycle. ---- */
    static uint64_t     ubr[WV_RWORDS];
    static ap_uint<256> ubq4[(WV_MWORDS + 3) / 4];
    static int64_t  accbuf[1];

    const uint32_t nb     = (uint32_t)((m + WV_PE - 1) / WV_PE);
    const uint32_t n32    = (uint32_t)n;
    const uint32_t g      = (uint32_t)WV_GAP_OF(n32);
    const uint32_t S      = n32 + g;
    /* counted cycles per tile: from band 0 column 0 at lane 0 to the last band's last
     * column at lane 127, rounded up to whole supergroups */
    const uint32_t clast  = (nb - 1) * S + n32 + WV_TILE_PE;
    const uint32_t cmax   = ((clast + WV_DIR_SCYC - 1) / WV_DIR_SCYC) * WV_DIR_SCYC;
    const uint32_t nbeats = (cmax / WV_DIR_SCYC) * WV_DIR_SBEATS;
    /* tokens: 1 lead pre-marker + every band's stride + the chain's lane delay + margin */
    const uint32_t T      = 1 + nb * S + WV_PE + 2 * WV_TILE_PE;

    const uint32_t rwords = (n32 + WV_PER_WORD - 1) / WV_PER_WORD;
    const uint64_t *rsrc = mem + r_off;
load_ref:
    for (uint32_t w = 0; w < rwords; ++w) {
#pragma HLS PIPELINE II = 1
#pragma HLS LOOP_TRIPCOUNT min = 1 max = WV_RWORDS
        ubr[w] = rsrc[w];
    }
    /* whole query, zero-padded to a whole block so the loader never reads past it */
    const uint32_t qwords = (uint32_t)((m + WV_PER_WORD - 1) / WV_PER_WORD);
    const uint32_t qpad   = nb * (WV_PE / WV_PER_WORD);
    const uint64_t *qsrc = mem + q_off;
    ap_uint<256> q4 = 0;
load_query:
    for (uint32_t w = 0; w < qpad; ++w) {
#pragma HLS PIPELINE II = 1
#pragma HLS LOOP_TRIPCOUNT min = 1 max = WV_MWORDS
        const uint64_t v = (w < qwords) ? qsrc[w] : (uint64_t)0;
        q4.range(64 * (w & 3) + 63, 64 * (w & 3)) = v;
        if ((w & 3) == 3) ubq4[w >> 2] = q4;
    }

    wv_ring(ubq4, ubr, accbuf, m, n32, nb, g, T, cmax, (int64_t)NWW_GAP * m, nbeats
#if WV_NCH > 0
            , d00
#endif
#if WV_NCH > 1
            , d01
#endif
#if WV_NCH > 2
            , d02
#endif
#if WV_NCH > 3
            , d03
#endif
#if WV_NCH > 4
            , d04
#endif
#if WV_NCH > 5
            , d05
#endif
#if WV_NCH > 6
            , d06
#endif
#if WV_NCH > 7
            , d07
#endif
#if WV_NCH > 8
            , d08
#endif
#if WV_NCH > 9
            , d09
#endif
#if WV_NCH > 10
            , d10
#endif
#if WV_NCH > 11
            , d11
#endif
#if WV_NCH > 12
            , d12
#endif
#if WV_NCH > 13
            , d13
#endif
#if WV_NCH > 14
            , d14
#endif
#if WV_NCH > 15
            , d15
#endif
#if WV_NCH > 16
            , d16
#endif
#if WV_NCH > 17
            , d17
#endif
#if WV_NCH > 18
            , d18
#endif
#if WV_NCH > 19
            , d19
#endif
#if WV_NCH > 20
            , d20
#endif
#if WV_NCH > 21
            , d21
#endif
#if WV_NCH > 22
            , d22
#endif
#if WV_NCH > 23
            , d23
#endif
#if WV_NCH > 24
            , d24
#endif
#if WV_NCH > 25
            , d25
#endif
#if WV_NCH > 26
            , d26
#endif
#if WV_NCH > 27
            , d27
#endif
#if WV_NCH > 28
            , d28
#endif
#if WV_NCH > 29
            , d29
#endif
#if WV_NCH > 30
            , d30
#endif
#if WV_NCH > 31
            , d31
#endif
            );

    mem[s_off] = accbuf[0];

    /* S1t: walk the optimal path on the card (directions are complete: every
     * dirwriter has had its last write response before wv_ring returns) */
    wv_tb_walk(mem, b_off, ubq4, ubr, m, (int64_t)n32, S, nbeats);
}

}  /* extern "C" */
