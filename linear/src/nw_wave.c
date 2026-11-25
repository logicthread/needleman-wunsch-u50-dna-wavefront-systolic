/* nw_wave.c — cycle-accurate scalar model of the 512-PE anti-diagonal wavefront.
 *
 * Written in SNAPSHOT-THEN-COMMIT form, like nw2_myers.c, and for the same reason:
 * every PE fires in the same instant, so none may observe another's result from the
 * current cycle. Two earlier attempts at loop-style register modelling in this project
 * silently crossed register boundaries and produced designs that simulated correctly
 * and could never be built. The snapshot discipline makes the register boundary
 * explicit and is why this is the reference the RTL must match.
 *
 * See nw_wave.h for the delta recurrence, the measured [-2,4] ranges and the schedule.
 */
#include "nw_wave.h"
#include "nw_wave_lut.h"

/* Build with -DNWW_CHECK_RANGE to assert that every delta really does stay inside the
 * measured [NWW_DMIN, NWW_DMAX] window. That window is what sizes the PE registers at
 * 3 bits, so it is worth proving rather than trusting -- the textbook bound for this
 * recurrence is [gap, match] = [-2, 2], and the true upper bound here is 4. */
#ifdef NWW_CHECK_RANGE
#include <stdio.h>
#include <stdlib.h>
static void nww_assert_range(const char *what, int v) {
    if (v < NWW_DMIN || v > NWW_DMAX) {
        printf("RANGE VIOLATION: %s = %d, outside [%d, %d] -- 3 bits is NOT enough\n",
               what, v, NWW_DMIN, NWW_DMAX);
        exit(3);
    }
}
#define NWW_CHK(what, v) nww_assert_range((what), (int)(v))
#else
#define NWW_CHK(what, v) ((void)0)
#endif

/* s(q[i], r[j]) — the only place the scoring parameters appear. */
static inline int8_t nww_sub(uint8_t a, uint8_t b) {
    /* An N on either side must never score as a match. Treated as a mismatch, which
     * is the conventional choice for a score-only global aligner. */
    if (a > 3 || b > 3) return (int8_t)NWW_MISMATCH;
    return (a == b) ? (int8_t)NWW_MATCH : (int8_t)NWW_MISMATCH;
}

static inline int8_t nww_max3(int8_t a, int8_t b, int8_t c) {
    int8_t v = a;
    if (b > v) v = b;
    if (c > v) v = c;
    return v;
}

/* Proves the generated ROM equals the arithmetic over every reachable input. Called
 * once from nw_wave_scalar, so any test run exercises it. The kernel uses the table and
 * this model uses the arithmetic -- without this they could disagree silently and the
 * three-way testbench would blame the wrong component. */
int nww_lut_selfcheck(void) {
    for (int s_bit = 0; s_bit < 2; ++s_bit)
        for (int ea = 0; ea <= 6; ++ea)
            for (int eb = 0; eb <= 6; ++eb) {
                const int a = NWW_DEC(ea), b = NWW_DEC(eb);
                const int sc = s_bit ? NWW_MATCH : NWW_MISMATCH;
                int e = sc;
                if (a + NWW_GAP > e) e = a + NWW_GAP;
                if (b + NWW_GAP > e) e = b + NWW_GAP;
                const unsigned v = NWW_LUT[NWW_LUT_IDX(s_bit, ea, eb)];
                if ((int)NWW_LUT_DV(v) != NWW_ENC(e - a)) return 1;
                if ((int)NWW_LUT_DH(v) != NWW_ENC(e - b)) return 2;
                /* direction: diag > up > left, same priority as the generator */
                const int want = (e == sc) ? NWW_DIAG
                               : ((e == a + NWW_GAP) ? NWW_UP : NWW_LEFT);
                if ((int)NWW_LUT_DIR(v) != want) return 3;
            }
    return 0;
}

int64_t nw_wave_scalar(const uint8_t *q, int64_t m,
                       const uint8_t *r, int64_t n,
                       int8_t *bnd) {
    {   /* fail loudly rather than let a stale table pass as a kernel bug */
        static int checked = 0;
        if (!checked) {
            const int rc = nww_lut_selfcheck();
            if (rc) { NWW_CHK("LUT selfcheck failed", 99); }
            checked = 1;
        }
    }
    if (m < 0 || n < 0) return 0;
    if (m == 0) return (int64_t)NWW_GAP * n;      /* all gaps */
    if (n == 0) return (int64_t)NWW_GAP * m;

    const int64_t P      = NW_PE;
    const int64_t nbands = (m + P - 1) / P;

    /* H[m][0] = m*G, then the score walks along the bottom row via dH. */
    int64_t acc = (int64_t)NWW_GAP * m;

    for (int64_t band = 0; band < nbands; ++band) {
        const int64_t row0 = band * P;
        const int64_t rows = (m - row0 < P) ? (m - row0) : P;   /* live PEs */
        const int     last = (band == nbands - 1);

        /* ---- per-PE registers. dv[i] holds dV[i][j-1]; dh[i] holds this PE's dH
         * output for the column it processed last cycle, which is exactly what PE i+1
         * needs as dH[i][j] this cycle. */
        int8_t  dv[NW_PE], dh[NW_PE], s_dh[NW_PE];
        uint8_t qc[NW_PE];
        for (int64_t i = 0; i < rows; ++i) {
            dv[i] = (int8_t)NWW_GAP;        /* dV[i][0] = G, from H[i][0] = i*G */
            dh[i] = (int8_t)NWW_GAP;        /* no column processed yet */
            qc[i] = q[row0 + i];            /* PE i holds one query character */
        }

        /* The array drains PE by PE, so the last PE finishes rows-1 cycles after the
         * reference ends. */
        const int64_t tmax = n + rows;
        for (int64_t t = 0; t < tmax; ++t) {
            /* ================= snapshot =================
             * THIS IS LOAD-BEARING. PE i reads dH[i-1][j], the value PE i-1 produced
             * one cycle ago. Walking i upwards and reading dh[i-1] directly would read
             * the value PE i-1 produced THIS cycle, for column j+1 -- one column too
             * new. In hardware every PE fires simultaneously off the registers as they
             * stood at the clock edge, so the model must copy them first. */
            for (int64_t i = 0; i < rows; ++i) s_dh[i] = dh[i];

            for (int64_t i = 0; i < rows; ++i) {
                const int64_t j = t - i;               /* the anti-diagonal */
                if (j < 0 || j >= n) {
                    /* Not on the wavefront yet, or already drained. Registers hold:
                     * a PE that kept updating while inactive would corrupt the
                     * neighbour still consuming its output. */
                    continue;
                }

                /* dH[i-1][j]: for PE 0 the band's top boundary (G in band 0, the
                 * carried bottom row otherwise); else PE i-1's SNAPSHOT register. */
                const int8_t dh_up = (i == 0)
                    ? ((band == 0) ? (int8_t)NWW_GAP : bnd[j])
                    : s_dh[i - 1];

                const int8_t s = nww_sub(qc[i], r[j]);
                const int8_t e = nww_max3(s,
                                          (int8_t)(dh_up  + NWW_GAP),
                                          (int8_t)(dv[i]  + NWW_GAP));
                const int8_t dv_new = (int8_t)(e - dh_up);
                const int8_t dh_new = (int8_t)(e - dv[i]);

                NWW_CHK("dV", dv_new);
                NWW_CHK("dH", dh_new);
                dv[i] = dv_new;
                dh[i] = dh_new;

                /* The bottom live PE's dH is the score contribution on the last band,
                 * or the boundary handed to the next band. Written at column j and
                 * read by PE 0 at column j of the NEXT band, so `bnd` is used in place
                 * with no aliasing inside a band. */
                if (i == rows - 1) {
                    if (last) acc += dh_new;
                    else      bnd[j] = dh_new;
                }
            }
        }
    }
    return acc;
}


/* ===================================================================================
 * TRACEBACK
 *
 * nw_wave_dirs() is nw_wave_scalar() with one addition: every cell's direction is
 * recorded. The direction costs nothing to produce -- it comes out of the same ROM
 * lookup as the next state, indexed by the same 7 bits -- but it costs a great deal to
 * STORE, and that is the real constraint:
 *
 *     2 bits per cell x m x n.
 *
 * At 8 GB of HBM that is m*n <= 3.2e10 cells, i.e. about 179k x 179k. Two 5 Mbp
 * genomes would need 6.25 TB and a chromosome pair ~15 PB. **Full-matrix traceback does
 * not scale to the two-genome workload** and never will; genome-scale traceback needs
 * Hirschberg / Myers-Miller divide-and-conquer, which is O(min(m,n)) space for 2x the
 * compute. This path is for inputs that fit.
 *
 * `dirs` here is plain row-major, one byte per cell, because the host reads it and
 * clarity matters more than density in the model. The KERNEL writes anti-diagonal
 * order instead -- at cycle t the 512 PEs hold cells (i, t-i), which are contiguous in
 * that order and scattered in row-major, so anti-diagonal order is one coalesced burst
 * per cycle and row-major order would be 512 scattered writes.
 * =================================================================================*/
int64_t nw_wave_dirs(const uint8_t *q, int64_t m,
                     const uint8_t *r, int64_t n,
                     int8_t *bnd, uint8_t *dirs) {
    if (m <= 0 || n <= 0) return nw_wave_scalar(q, m, r, n, bnd);

    const int64_t P      = NW_PE;
    const int64_t nbands = (m + P - 1) / P;
    int64_t acc = (int64_t)NWW_GAP * m;

    for (int64_t band = 0; band < nbands; ++band) {
        const int64_t row0 = band * P;
        const int64_t rows = (m - row0 < P) ? (m - row0) : P;
        const int     last = (band == nbands - 1);

        int8_t  dv[NW_PE], dh[NW_PE], s_dh[NW_PE];
        uint8_t qc[NW_PE];
        for (int64_t i = 0; i < rows; ++i) {
            dv[i] = (int8_t)NWW_GAP;
            dh[i] = (int8_t)NWW_GAP;
            qc[i] = q[row0 + i];
        }

        const int64_t tmax = n + rows;
        for (int64_t t = 0; t < tmax; ++t) {
            for (int64_t i = 0; i < rows; ++i) s_dh[i] = dh[i];

            for (int64_t i = 0; i < rows; ++i) {
                const int64_t j = t - i;
                if (j < 0 || j >= n) continue;

                const int8_t dh_up = (i == 0)
                    ? ((band == 0) ? (int8_t)NWW_GAP : bnd[j])
                    : s_dh[i - 1];

                const int smatch = (qc[i] < 4 && r[j] < 4 && qc[i] == r[j]) ? 1 : 0;
                const unsigned v = NWW_LUT[
                    NWW_LUT_IDX(smatch, NWW_ENC((int)dh_up), NWW_ENC((int)dv[i]))];

                dv[i] = (int8_t)NWW_DEC((int)NWW_LUT_DV(v));
                dh[i] = (int8_t)NWW_DEC((int)NWW_LUT_DH(v));
                dirs[(row0 + i) * n + j] = (uint8_t)NWW_LUT_DIR(v);

                if (i == rows - 1) {
                    if (last) acc += dh[i];
                    else      bnd[j] = dh[i];
                }
            }
        }
    }
    return acc;
}

/* Walk the direction matrix from (m,n) back to (0,0) and score the path independently.
 * Returns the path's score; *ops receives its length. The caller compares this against
 * the DP optimum -- that is a far stronger check than matching some golden's choice of
 * tie-break, because it verifies the recorded path is itself optimal. */
int64_t nw_wave_traceback(const uint8_t *q, int64_t m,
                          const uint8_t *r, int64_t n,
                          const uint8_t *dirs, int64_t *ops) {
    int64_t i = m, j = n, sc = 0, k = 0;
    while (i > 0 && j > 0) {
        const uint8_t d = dirs[(i - 1) * n + (j - 1)];
        if (d == NWW_DIAG) {
            const int match = (q[i-1] < 4 && r[j-1] < 4 && q[i-1] == r[j-1]);
            sc += match ? NWW_MATCH : NWW_MISMATCH;
            --i; --j;
        } else if (d == NWW_UP) {       /* consume query, gap in reference */
            sc += NWW_GAP; --i;
        } else {                         /* LEFT: consume reference, gap in query */
            sc += NWW_GAP; --j;
        }
        ++k;
    }
    /* whatever remains is a run of gaps against the leading edge */
    sc += (int64_t)NWW_GAP * (i + j);
    k  += i + j;
    if (ops) *ops = k;
    return sc;
}
