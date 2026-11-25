/* nw_wave.h — 512-PE anti-diagonal wavefront, linear gap, GLOBAL alignment.
 * =================================================================================
 * WHAT THIS IS AND WHY IT EXISTS ALONGSIDE THE MYERS KERNEL
 *
 * The Myers design (kernel_nw3.cpp) retires 4096 cells/cycle at 12.7 LUT/cell, but it
 * is stuck at 108 MHz: one HLS FSM state clock-enables the whole Peq register file, a
 * 21,336-pin net with ZERO logic levels and 8.8 ns of pure routing. Two restructurings
 * of that copy were measured and both failed (NOTES.md N19/N21/N22/N23).
 *
 * This array is the other end of the trade. 512 PEs retire 512 cells/cycle -- 8x fewer
 * -- but every PE talks only to its immediate neighbour through ONE register, so there
 * is no high-fanout control net to route and nothing that grows with array size. It is
 * sized to CLOSE TIMING first and scale by replication afterwards.
 *
 * HONEST THROUGHPUT: 512 cells/cycle is 230 GCUPS at 450 MHz, 256 at 500. That is
 * BELOW the 500+ GCUPS target; 500+ needs ~1112 PEs at 450 MHz, i.e. 2048 PEs for
 * 922 GCUPS. The architecture scales linearly by replication, so this is a question of
 * instance count once the clock is proven, not of redesign.
 *
 * IT IS ALSO NOT TRIPLE-SPECIFIC. Myers only computes this score because of the
 * identity score = (m+n) - 3*editdist, which holds ONLY for global alignment with
 * exactly match +2 / mismatch -1 / gap -2 (NOTES.md N1). This array computes the score
 * directly from the recurrence, so it survives a change of scoring parameters.
 *
 * ---------------------------------------------------------------------------------
 * THE DELTA RECURRENCE. PEs store score DIFFERENCES, never absolute scores, so the
 * registers stay 3 bits wide no matter how long the genomes are.
 *
 *   dH[i][j] = H[i][j] - H[i][j-1]        horizontal difference
 *   dV[i][j] = H[i][j] - H[i-1][j]        vertical difference
 *   e [i][j] = H[i][j] - H[i-1][j-1]
 *
 * From H[i][j] = max(H[i-1][j-1]+s, H[i-1][j]+G, H[i][j-1]+G), subtracting
 * H[i-1][j-1] from every term:
 *
 *   e [i][j] = max(s, dH[i-1][j] + G, dV[i][j-1] + G)
 *   dV[i][j] = e[i][j] - dH[i-1][j]
 *   dH[i][j] = e[i][j] - dV[i][j-1]
 *
 * MEASURED RANGES over 1.56M cells of full DP for match +2 / mismatch -1 / gap -2:
 *   dH, dV in [-2, 4]   (7 values -> 3 bits with a +2 bias)
 *   e      in [-1, 2]
 * NOTE the upper bound is 4, NOT the textbook [gap, match] = [-2, 2]. Measured, not
 * assumed; sizing the register from the textbook bound would have been wrong.
 *
 * SCHEDULE. At cycle t, PE i processes reference column j = t - i, which is the
 * anti-diagonal. Both operands then come from one cycle ago:
 *   dH[i-1][j]  = PE i-1's output at cycle t-1 (it handled column j then)
 *   dV[i][j-1]  = this PE's own register from cycle t-1
 * So a PE reads ONE neighbour register and its own. No diagonal bypass, no broadcast,
 * nothing whose fanout grows with NW_PE. That is the whole point.
 *
 * BOUNDARY CONDITIONS, global alignment:
 *   H[0][j] = j*G  =>  dH[0][j] = G   (the array's top input)
 *   H[i][0] = i*G  =>  dV[i][0] = G   (every PE's reset value)
 *
 * UNBOUNDED QUERY. m > NW_PE is handled in bands of NW_PE rows, carrying the bottom
 * row's dH per column between bands -- 3 bits/column, in `bnd`. Only the last band
 * accumulates the score, from H[m][0] = m*G. Identical in structure to the verified
 * Myers banding (NOTES.md N24).
 * =================================================================================*/
#ifndef NW_WAVE_H
#define NW_WAVE_H

#include <stdint.h>

#ifndef NW_PE
#define NW_PE 512                  /* PEs = query rows per band = cells/cycle */
#endif

#define NWW_MATCH     2
#define NWW_MISMATCH (-1)
#define NWW_GAP      (-2)

/* Deltas live in [-2, 4]; stored biased by +2 into [0, 6] so 3 bits suffice. */
#define NWW_BIAS   2

/* Traceback directions, 2 bits per cell. Public API: the host decodes the matrix with
 * these, so they live here rather than in the generated table.
 *   DIAG  H[i][j] = H[i-1][j-1] + s   consume one query AND one reference symbol
 *   UP    H[i][j] = H[i-1][j]   + G   consume query, gap in the reference
 *   LEFT  H[i][j] = H[i][j-1]   + G   consume reference, gap in the query */
#define NWW_DIAG 0
#define NWW_UP   1
#define NWW_LEFT 2
#define NWW_DMIN  (-2)
#define NWW_DMAX   4

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the global alignment SCORE directly (no identity involved).
 * `bnd` must hold at least n int8_t entries; it carries the inter-band boundary and is
 * used in place. Pass NULL only when m <= NW_PE. */
int64_t nw_wave_scalar(const uint8_t *q, int64_t m,
                       const uint8_t *r, int64_t n,
                       int8_t *bnd);

/* Direction-recording variant. `dirs` must hold m*n bytes, row-major, one byte per
 * cell holding NWW_DIAG / NWW_UP / NWW_LEFT.
 *
 * STORAGE IS THE BINDING CONSTRAINT, not compute: 2 bits/cell x m x n means 8 GB of
 * HBM caps this at m*n <= 3.2e10, about 179k x 179k. Two 5 Mbp genomes would need
 * 6.25 TB. Genome-scale traceback needs Hirschberg / Myers-Miller linear space. */
int64_t nw_wave_dirs(const uint8_t *q, int64_t m,
                     const uint8_t *r, int64_t n,
                     int8_t *bnd, uint8_t *dirs);

/* Walks `dirs` from (m,n) to (0,0) and independently scores the path it encodes.
 * Compare against the DP optimum: that verifies the recorded path IS optimal, rather
 * than merely matching one particular tie-breaking convention. */
int64_t nw_wave_traceback(const uint8_t *q, int64_t m,
                          const uint8_t *r, int64_t n,
                          const uint8_t *dirs, int64_t *ops);

int nww_lut_selfcheck(void);

#ifdef __cplusplus
}
#endif
#endif
