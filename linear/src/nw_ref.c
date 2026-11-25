/* nw_ref.c — golden Needleman-Wunsch, row-major, score domain, linear gap.
 *
 * Simple on purpose: this is what everything else is measured against. O(m*n) time
 * and O(n) space (two rows), which is all the golden needs since it is score-only.
 *
 * Linear gap means ONE recurrence. There is no "am I already inside a gap" state to
 * carry, so no E and F matrices exist here:
 *
 *     H[i][j] = max( H[i-1][j-1] + sub,  H[i-1][j] + gap,  H[i][j-1] + gap )
 *
 * No max(0, ...) either — that clamp is what makes an alignment LOCAL. Global lets
 * cells go negative and reports the corner H[m][n], not the matrix maximum.
 */
#include <stdlib.h>
#include "nw.h"

static inline int32_t max3(int32_t a, int32_t b, int32_t c) {
    int32_t t = a > b ? a : b;
    return t > c ? t : c;
}

static inline int32_t sub_score(uint8_t a, uint8_t b) {
    /* N matches nothing, including another N. */
    if (a == NW_N || b == NW_N) return NW_MISMATCH;
    return (a == b) ? NW_MATCH : NW_MISMATCH;
}

int32_t nw_align_ref(const uint8_t *q, int32_t m,
                     const uint8_t *r, int32_t n) {
    if (m < 0 || n < 0) return 0;
    if (m == 0) return n * NW_GAP;        /* all of r deleted */
    if (n == 0) return m * NW_GAP;        /* all of q deleted */

    int32_t *prev = malloc((size_t)(n + 1) * sizeof(int32_t));
    int32_t *cur  = malloc((size_t)(n + 1) * sizeof(int32_t));
    if (!prev || !cur) { free(prev); free(cur); return 0; }

    /* Row 0 is the all-gap boundary: H[0][j] = j * gap. */
    for (int32_t j = 0; j <= n; ++j) prev[j] = j * NW_GAP;

    for (int32_t i = 1; i <= m; ++i) {
        cur[0] = i * NW_GAP;              /* column 0 boundary */
        for (int32_t j = 1; j <= n; ++j) {
            cur[j] = max3(prev[j - 1] + sub_score(q[i - 1], r[j - 1]),
                          prev[j]     + NW_GAP,
                          cur[j - 1]  + NW_GAP);
        }
        int32_t *t = prev; prev = cur; cur = t;
    }

    const int32_t out = prev[n];          /* H[m][n] after the final swap */
    free(prev); free(cur);
    return out;
}
