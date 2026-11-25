/* nw_host_traceback.c -- host-side path recovery from the kernel's direction beats.
 * ===================================================================================
 * The kernel writes 2 bits per cell, anti-diagonal-major, one 512-bit beat per 2 counted
 * cycles per tile. This walks the optimal path back from (m,n) to (0,0), fetching only
 * the beats it needs.
 *
 * WHY NOT JUST READ IT ALL BACK. At the capacity limit the direction array is 8.19 GB.
 * Over PCIe Gen3 x16 at ~12 GB/s practical that is ~680 ms, against ~56 ms of compute at
 * 2048 PE / 300 MHz -- the readback would be twelve times the alignment. A traceback
 * touches only m+n cells of the m*n array, so this fetches per beat, on demand, and
 * caches the last one. Consecutive steps usually land in the same beat or its neighbour,
 * so the fetch count is far below one per cell.
 *
 * THE INVERSE MAPPING, verified standalone over 25,867,509 cells at 4 tiles and
 * 61,916,973 at 16, with a written-at-least-once check in both:
 *
 *   cell (row, j), 0-based:
 *     band = row / PE                 k = (row % PE) / TILE_PE      (which port)
 *     i    = (row % PE) % TILE_PE     (local PE within the tile)
 *     c    = j + i                    COUNTED from the tile's own first active cycle,
 *                                     NOT a cycle index -- so FIFO depth, writer
 *                                     latency and pipeline registers are all irrelevant
 *     bpb  = ceil((n + TILE_PE) / PACK)          beats per band per port
 *     beat = band * bpb + c / PACK
 *     bit  = (TILE_PE * 2) * (c % PACK) + 2 * i
 *
 * Deriving position from a cycle index instead is what broke the kernel's own tail
 * (wrong by 7, and csim disagreed with RTL).
 * ===================================================================================*/
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "nw_host_traceback.h"

#define NWH_DIAG 0
#define NWH_UP   1
#define NWH_LEFT 2

static inline int64_t nwh_ceil_div(int64_t a, int64_t b) { return (a + b - 1) / b; }
static inline int64_t nwh_min(int64_t a, int64_t b) { return a < b ? a : b; }
static inline int64_t nwh_max(int64_t a, int64_t b) { return a > b ? a : b; }

/* Per-port span cache. The walk descends in beat index within a band, so a span fetched
 * backwards from the current beat serves many subsequent steps. */
typedef struct {
    int64_t  lo, hi;                      /* cached beats [lo, hi], hi<lo = empty */
    uint8_t *buf;                         /* NWH_SPAN * NWH_BEAT_BYTES            */
} nwh_span;

/* ---- BASE-3 UNPACKER ---------------------------------------------------------------
 * One byte holds five directions as d0*81 + d1*27 + d2*9 + d3*3 + d4, d0 earliest.
 *
 * A 243-entry table is built once instead of dividing per cell. The traceback walks
 * only m+n cells, so dividing would be cheap too -- the table exists because the
 * decode is then a single indexed load, which also makes a BULK decode (whole beats at
 * a time, for debugging or for a host-side full-matrix dump) trivial. */
static const unsigned NWH_P3[NWH_DIR_GROUP] = { 81u, 27u, 9u, 3u, 1u };

static unsigned char nwh_b3tab[NWH_B3_MAX + 1][NWH_DIR_GROUP];
static int           nwh_b3tab_ready = 0;

static void nwh_b3_init(void)
{
    unsigned v;
    if (nwh_b3tab_ready) return;
    for (v = 0; v <= (unsigned)NWH_B3_MAX; ++v) {
        int d;
        for (d = 0; d < NWH_DIR_GROUP; ++d)
            nwh_b3tab[v][d] = (unsigned char)((v / NWH_P3[d]) % 3u);
    }
    nwh_b3tab_ready = 1;
}

int nwh_unpack5(unsigned byte, unsigned char out[NWH_DIR_GROUP])
{
    int d;
    /* > 242 is not a representable group. It cannot come from a correct kernel write,
     * so it means a mis-addressed beat or a stale buffer -- report it instead of
     * returning a plausible-looking wrong direction. */
    if (byte > (unsigned)NWH_B3_MAX) return -1;
    nwh_b3_init();
    for (d = 0; d < NWH_DIR_GROUP; ++d) out[d] = nwh_b3tab[byte][d];
    return 0;
}

#ifdef NWH_CONT
#ifndef NWH_DIR_B3
#error "NWH_CONT (S1c continuous bands) is defined only for the base-3 layout"
#endif
/* Idle tokens after each band: max(NWH_BWIN, NMIN - n), NMIN = pe + 8*tile_pe. Mirrors
 * the kernel's WV_GAP_OF. NWH_BWIN = 640 for the V11 (tile-wide swap) kernel, 0 otherwise. */
#ifndef NWH_BWIN
#define NWH_BWIN 0
#endif
static int64_t nwh_cont_gap(const nwh_geom *g)
{
    const int64_t nmin = g->pe + 8 * g->tile_pe;
    const int64_t a = nmin - g->n;
    return a > NWH_BWIN ? a : (int64_t)NWH_BWIN;
}

/* Beats per port for the whole alignment in the S1c continuous layout -- must mirror the
 * kernel's cmax/nbeats exactly. */
int64_t nwh_cont_beats(const nwh_geom *g)
{
    const int64_t S     = g->n + nwh_cont_gap(g);
    const int64_t nb    = nwh_ceil_div(g->m, g->pe);
    const int64_t clast = (nb - 1) * S + g->n + g->tile_pe;
    const int64_t cmax  = nwh_ceil_div(clast, NWH_DIR_SCYC) * NWH_DIR_SCYC;
    return (cmax / NWH_DIR_SCYC) * NWH_DIR_SBEATS * NWH_SHARE;
}
#endif

/* Fetch the direction for cell (row, j). */
static int nwh_dir_at(const nwh_geom *g, nwh_fetch_span_fn fetch, void *ctx,
                      nwh_span *sp, int64_t nbeats_total,
                      int64_t row, int64_t j, int *out,
                      int64_t *calls, int64_t *beats)
{
    const int64_t rib  = row % g->pe;
    const int     k    = (int)(rib / g->tile_pe);
    const int     i    = (int)(rib % g->tile_pe);
    const int64_t band = row / g->pe;
#ifdef NWH_CONT
    /* S1c CONTINUOUS BANDS (kernel_tile_1b_s1c.cpp): one diagonal stream per tile for the
     * whole alignment, band stride S = n + gap tokens, gap = max(0, NMIN - n) with
     * NMIN = pe + 8*tile_pe. No per-band beat offset. */
    const int64_t nmin = g->pe + 8 * g->tile_pe;
    const int64_t S    = g->n + nwh_cont_gap(g);
    const int64_t c    = band * S + j + i;
#else
    const int64_t c    = j + i;
#endif
#ifdef NWH_DIR_B3
    /* cmax is rounded UP to a whole number of 5-cycle groups, exactly as the kernel
     * does, or the last group's beats would be mis-indexed. */
    const int64_t cmax = nwh_ceil_div(g->n + g->tile_pe, NWH_DIR_SCYC) * NWH_DIR_SCYC;
    const int64_t bpb  = (cmax / NWH_DIR_SCYC) * NWH_DIR_SBEATS;
    /* Supergroup sg; this cycle sits in group gsg of it. The byte index runs across
     * the WHOLE supergroup, so lane i of group gsg owns byte gsg*tile_pe + i. At
     * NWH_TILE_PE=128 (SG=1) this reduces to the original grp*2 + i/64. */
    const int64_t sg   = c / NWH_DIR_SCYC;
    const int     gsg  = (int)((c / NWH_DIR_GROUP) % NWH_DIR_SG);
    const int     dpos = (int)(c % NWH_DIR_GROUP);
    const int     bidx = gsg * NWH_TILE_PE + i;
#ifdef NWH_CONT
    (void)bpb;
    const int64_t beat = (sg * NWH_SHARE + k % NWH_SHARE) * NWH_DIR_SBEATS
                       + (bidx / NWH_DIR_BYTES);
#else
    const int64_t beat = band * bpb * NWH_SHARE
                       + (sg * NWH_SHARE + k % NWH_SHARE) * NWH_DIR_SBEATS
                       + (bidx / NWH_DIR_BYTES);
#endif
    const int     byo  = bidx % NWH_DIR_BYTES;           /* byte within the beat */
#else
    const int64_t bpb  = nwh_ceil_div(g->n + g->tile_pe, NWH_PACK);
    const int64_t beat = band * bpb + c / NWH_PACK;
    const int     bit  = (int)(g->tile_pe * 2) * (int)(c % NWH_PACK) + 2 * i;
#endif

    nwh_span *s = &sp[k];
    if (beat < s->lo || beat > s->hi) {
        /* fetch a span ENDING at this beat: the walk descends, so the useful beats are
         * the ones below it. Clamp to the array. */
        int64_t lo = nwh_max(0, beat - (NWH_SPAN - 1));
        int64_t hi = nwh_min(nbeats_total - 1, lo + NWH_SPAN - 1);
        if (hi < beat) { lo = beat; hi = nwh_min(nbeats_total - 1, beat + NWH_SPAN - 1); }
        /* port, not tile: NWH_SHARE tiles share one channel. The span cache stays
         * per-TILE because each tile walks its own beat range inside that channel. */
        if (fetch(ctx, k / NWH_SHARE, lo, hi - lo + 1, s->buf) != 0) return -1;
        s->lo = lo; s->hi = hi;
        if (calls) ++*calls;
        if (beats) *beats += hi - lo + 1;
    }
    const uint8_t *b = s->buf + (size_t)(beat - s->lo) * NWH_BEAT_BYTES;
#ifdef NWH_DIR_B3
    {
        const unsigned byte = (unsigned)b[byo];
        if (byte > (unsigned)NWH_B3_MAX) return -2;   /* mis-addressed or stale beat */
        nwh_b3_init();
        *out = (int)nwh_b3tab[byte][dpos];
    }
#else
    *out = (b[bit >> 3] >> (bit & 7)) & 0x3;
#endif
    return 0;
}

int64_t nw_host_traceback(const nwh_geom *g,
                          const uint8_t *q, const uint8_t *r,
                          nwh_fetch_span_fn fetch, void *ctx,
                          uint8_t *ops, int64_t ops_cap,
                          int64_t *score_out, int64_t *calls_out,
                          int64_t *beats_out)
{
    const int64_t nports = g->pe / g->tile_pe;
#ifdef NWH_DIR_B3
    const int64_t cmaxt  = nwh_ceil_div(g->n + g->tile_pe, NWH_DIR_GROUP) * NWH_DIR_GROUP;
    const int64_t bpb    = (cmaxt / NWH_DIR_GROUP) * NWH_DIR_BPG;
#else
    const int64_t bpb    = nwh_ceil_div(g->n + g->tile_pe, NWH_PACK);
#endif
    const int64_t nbands = nwh_ceil_div(g->m, g->pe);
#ifdef NWH_CONT
    const int64_t ntot   = nwh_cont_beats(g);
    (void)bpb;
#else
    const int64_t ntot   = nbands * bpb;
#endif

    nwh_span *sp = (nwh_span *)calloc((size_t)nports, sizeof(nwh_span));
    if (!sp) return -3;
    for (int64_t k = 0; k < nports; ++k) {
        sp[k].lo = 1; sp[k].hi = 0;                    /* empty */
        sp[k].buf = (uint8_t *)malloc((size_t)NWH_SPAN * NWH_BEAT_BYTES);
        if (!sp[k].buf) { for (int64_t t = 0; t < k; ++t) free(sp[t].buf);
                          free(sp); return -3; }
    }
    int64_t i = g->m, j = g->n, nops = 0, score = 0, calls = 0, beats = 0;
    int64_t rc = 0;

    while (i > 0 || j > 0) {
        if (nops >= ops_cap) { rc = -1; goto done; }        /* caller's buffer too small */

        /* AT AN EDGE THERE IS ONLY ONE LEGAL MOVE, so no lookup is needed -- and the
         * lookup would be out of bounds. Reading dirs[(i-1)*n + (j-1)] before testing
         * i>0 && j>0 is exactly the bug that made a testbench report 17/18 failures
         * against a correct kernel: with j==0 the index is (i-1)*n - 1, in range but
         * the previous row's last column. */
        if (i > 0 && j > 0) {
            int d;
            if (nwh_dir_at(g, fetch, ctx, sp, ntot, i - 1, j - 1, &d,
                           &calls, &beats) != 0) { rc = -2; goto done; }
            if (d == NWH_DIAG) {
                score += (q[i - 1] == r[j - 1]) ? g->match : g->mismatch;
                ops[nops++] = NWH_DIAG; --i; --j;
            } else if (d == NWH_UP) {
                score += g->gap; ops[nops++] = NWH_UP; --i;
            } else {
                score += g->gap; ops[nops++] = NWH_LEFT; --j;
            }
        } else if (i > 0) {
            score += g->gap; ops[nops++] = NWH_UP;   --i;
        } else {
            score += g->gap; ops[nops++] = NWH_LEFT; --j;
        }
    }
    rc = nops;
done:
    for (int64_t k = 0; k < nports; ++k) free(sp[k].buf);
    free(sp);
    if (rc > 0) {
        if (score_out) *score_out = score;
        if (calls_out) *calls_out = calls;
        if (beats_out) *beats_out = beats;
    }
    return rc;
}
