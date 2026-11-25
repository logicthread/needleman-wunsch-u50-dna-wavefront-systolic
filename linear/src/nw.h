/* nw.h — Needleman-Wunsch global alignment, LINEAR gap, score only.
 *
 * Scoring fixed by the assignment:   match +2,  mismatch -1,  gap -2.
 *
 * THE IDENTITY THAT DRIVES THIS WHOLE DESIGN.
 *
 * In GLOBAL alignment the two lengths m, n are fixed, so the column counts are
 * constrained. Let a = matches, b = mismatches, g = total gap columns. Every column
 * consumes one character from each sequence (match/mismatch) or one from a single
 * sequence (gap), hence
 *
 *     m + n = 2a + 2b + g        =>   a = (m + n - g)/2 - b
 *
 * Substituting into the score with match=+2, mismatch=-1, gap=-2:
 *
 *     score = 2a - b - 2g
 *           = (m + n - g) - 2b - b - 2g
 *           = (m + n) - 3(b + g)
 *           = (m + n) - 3 * editdist(q, r)
 *
 * The relation is per-alignment, not merely at the optimum, so MAXIMISING THE SCORE
 * IS EXACTLY MINIMISING UNIT-COST EDIT DISTANCE. Verified over randomised ACGT pairs
 * in tb_nw.c against both golden implementations.
 *
 * That is what licenses Myers' bit-parallel bit-vector algorithm here: it computes
 * Levenshtein distance 64 query positions per cycle in pure bitwise logic, with no
 * score datapath at all. Cells per LUT is ~34x a score-matrix PE array, which is the
 * whole reason 500+ GCUPS is comfortable on a U50 rather than marginal.
 *
 * SCOPE OF THE SHORTCUT — it is narrow, and silently wrong outside it:
 *
 *   - GLOBAL ONLY. In local alignment (Smith-Waterman) the aligned sub-lengths vary,
 *     m + n is not constant, and the substitution collapses. The sibling kernel in
 *     ../dna_wavefront CANNOT use this.
 *   - THIS PARAMETER TRIPLE ONLY. The collapse needs a mismatch and a gap column to
 *     cost the same in the (m+n) - k*d form. Change the gap cost to -3 and the
 *     identity dies with it.
 *
 * nw_score_from_dist() is therefore the ONLY place the scoring parameters appear in
 * this entire codebase. Everything else computes a distance.
 *
 * SCORE ONLY, deliberately: traceback needs the whole O(m*n) matrix. Production
 * aligners score in hardware and re-align one small window on the host (Hirschberg /
 * Myers-Miller linear space) to recover the CIGAR.
 */
#ifndef NW_H_
#define NW_H_

#include <stdint.h>

/* 2-bit DNA plus one ambiguity code. NW_N matches nothing — in the bit-vector route
 * that is free: it is simply a character whose bit is set in none of the four Peq
 * masks, so it mismatches everything by construction. */
enum { NW_A = 0, NW_C = 1, NW_G = 2, NW_T = 3, NW_N = 4 };

#define NW_MATCH     ( 2)
#define NW_MISMATCH  (-1)
#define NW_GAP       (-2)

/* Architecture constants. These are the DP-F design point from
 * docs/nw-u50-dse.html and must match kernel_nw.cpp exactly. */
#define NW_WORD   64                      /* bits per Myers block = query rows per block */
#define NW_NBLK    4                      /* blocks chained per lane, one cycle apart */
#define NW_QMAX   (NW_WORD * NW_NBLK)     /* 256 — max query length */
#define NW_LANES   8                      /* independent alignments in flight */
/* cells retired per cycle = NW_LANES * NW_NBLK * NW_WORD = 2048 */

/* The one place the scoring parameters live. d = edit distance. */
static inline int32_t nw_score_from_dist(int32_t m, int32_t n, int32_t d) {
    return (m + n) - 3 * d;
}

/* ---- golden implementations, for cross-checking ---------------------------- */

/* Row-major O(m*n) Needleman-Wunsch in the SCORE domain, linear gap. The golden.
 * Returns H[m][n]. Global reports exactly one cell, so unlike the Smith-Waterman
 * work there is no tie-break to specify and no class of bug around it. */
int32_t nw_align_ref(const uint8_t *q, int32_t m,
                     const uint8_t *r, int32_t n);

/* Row-major O(m*n) Levenshtein in the DISTANCE domain. The second golden, and the
 * one that makes the identity testable independently of the score recurrence. */
int32_t nw_lev_ref(const uint8_t *q, int32_t m,
                   const uint8_t *r, int32_t n);

/* ---- the design under test -------------------------------------------------- */

/* Scalar Myers bit-vector, NW_NBLK blocks chained with the SAME one-cycle skew as
 * the hardware. Bit-exact mirror of kernel_nw.cpp's inner loop, so a disagreement
 * between the two is a port bug and not an algorithm bug. Returns edit distance.
 * Requires m <= NW_QMAX; returns -1 otherwise. */
int32_t nw_myers_skewed(const uint8_t *q, int32_t m,
                        const uint8_t *r, int32_t n);

/* 'ACGTN...' -> packed codes, into out[]. Returns length written. */
int32_t nw_encode(const char *s, uint8_t *out, int32_t max);

#endif /* NW_H_ */
