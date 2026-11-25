#ifndef NW_HOST_TRACEBACK_H
#define NW_HOST_TRACEBACK_H
#include <stdint.h>

/* Must match the kernel: WV_DIR_BEATBITS and WV_TILE_PE */
#define NWH_BEAT_BITS  512
#define NWH_BEAT_BYTES (NWH_BEAT_BITS / 8)
#ifndef NWH_TILE_PE
#define NWH_TILE_PE    128
#endif
/* NWH_SHARE must MIRROR the kernel's WV_SHARE. Tiles sharing one HBM channel are
 * group-interleaved, so tile k reads from PORT k/NWH_SHARE at sub-slot
 * k%NWH_SHARE, and the per-channel band stride is bpb*NWH_SHARE. A drift between
 * this and the kernel decodes silently wrong, so the host program static_asserts
 * it against the kernel's value. */
#ifndef NWH_SHARE
#define NWH_SHARE 1
#endif
#define NWH_PACK       (NWH_BEAT_BITS / (NWH_TILE_PE * 2))   /* 2 */

/* ---- BASE-3 DIRECTION PACKING (-DNWH_DIR_B3) --------------------------------------
 * Mirrors the kernel's -DWV_DIR_B3. A direction has three values (DIAG 0, UP 1,
 * LEFT 2), so 3^5 = 243 <= 256 lets five cells share one BYTE: 1.600 bits/cell against
 * a log2(3) = 1.585 floor. The kernel groups along TIME, not the PE axis, because
 * NWH_TILE_PE is not divisible by 5:
 *
 *   five COUNTED CYCLES -> one byte per lane -> NWH_TILE_PE bytes
 *                       -> NWH_DIR_BPG beats of NWH_BEAT_BITS
 *
 * Lanes 0..NWH_DIR_BYTES-1 land in the FIRST beat of the group and the rest in the
 * second, because the kernel writes ob0 before ob1.
 *
 * DIGIT ORDER: the kernel applies acc = acc*3 + d once per counted cycle, so the FIRST
 * cycle of a group is the MOST significant digit and the weights are {81,27,9,3,1}.
 * Verified exhaustively over all 243 groups / 1,215 extractions. */
#define NWH_DIR_GROUP  5
#define NWH_DIR_BPG    ((NWH_TILE_PE * 8) / NWH_BEAT_BITS)   /* beats per group = 2 */
#define NWH_DIR_BYTES  (NWH_BEAT_BITS / 8)                   /* lanes per beat  = 64 */
/* SUPERGROUP -- must mirror the kernel's WV_DIR_* exactly. A base-3 group yields
 * NWH_TILE_PE bytes, which fill whole 512-bit beats only when NWH_TILE_PE % 64 == 0.
 * At 160 it does not (160*8/512 = 2.5), so the kernel emits on a supergroup of
 * NWH_DIR_SG groups:  128 -> SG 1, 2 beats, 5 cycles;  160 -> SG 2, 5 beats, 10. */
#define NWH_P2OF64(x)  ( ((x) % 64 == 0) ? 64 : ((x) % 32 == 0) ? 32 \
                       : ((x) % 16 == 0) ? 16 : ((x) %  8 == 0) ?  8 : 1 )
#define NWH_DIR_SG     (64 / NWH_P2OF64(NWH_TILE_PE))
#define NWH_DIR_SBYTES (NWH_DIR_SG * NWH_TILE_PE)
#define NWH_DIR_SBEATS (NWH_DIR_SBYTES / NWH_DIR_BYTES)
#define NWH_DIR_SCYC   (NWH_DIR_SG * NWH_DIR_GROUP)
#define NWH_B3_MAX     242                                   /* 3^5 - 1 */

/* Unpack one byte into its five directions, out[0] = earliest counted cycle.
 * Returns 0, or -1 if the byte is not a valid base-3 group (> 242), which can only
 * mean the beat was mis-addressed or the buffer is stale -- worth catching loudly
 * rather than silently returning a wrong direction. */
int nwh_unpack5(unsigned byte, unsigned char out[NWH_DIR_GROUP]);

/* Beats cached per port.
 *
 * 256, NOT 4096, AND THE REASON IS MEASURED. The call count is set by the GEOMETRY, not
 * by the span: the path changes tile every WV_TILE_PE rows, so each port is visited in
 * runs of ~128 steps covering ~64 beats, and a span of that order already covers a run.
 * At m=n=60,000 the call count was 469 at EVERY span from 128 to 16384, while the bytes
 * fetched scaled linearly -- 3.8 MB at 128, 123 MB at 4096, 492 MB at 16384. Larger
 * spans buy nothing and cost everything. 256 is twice the measured run length, for
 * margin against path shapes with longer runs in one tile, and still only ~7.7 MB at
 * m=n=60,000. Too small merely costs extra calls; it is never wrong. */
#ifndef NWH_SPAN
#define NWH_SPAN 256
#endif

typedef struct {
    int64_t m, n;
    int64_t pe;            /* total PEs (NW_PE)        */
    int64_t tile_pe;       /* PEs per tile = per port  */
    int     match, mismatch, gap;
} nwh_geom;

/* Fetch `count` consecutive beats starting at `first` from port `port` into `dst`
 * (count * NWH_BEAT_BYTES). Return 0 on success. One clEnqueueReadBuffer.
 *
 * SPAN, NOT SINGLE BEAT, and that is the whole point. Per-beat fetching needs 325,834
 * calls at the capacity limit -- ~6.5 s of call overhead, worse than streaming the
 * entire 8.19 GB array. Spans cut it to ~1,400 calls. */
typedef int (*nwh_fetch_span_fn)(void *ctx, int port, int64_t first, int64_t count,
                                 uint8_t *dst);

#ifdef NWH_CONT
/* S1c continuous-band layout: direction beats per port for the whole alignment. */
int64_t nwh_cont_beats(const nwh_geom *g);
#endif

int64_t nw_host_traceback(const nwh_geom *g,
                          const uint8_t *q, const uint8_t *r,
                          nwh_fetch_span_fn fetch, void *ctx,
                          uint8_t *ops, int64_t ops_cap,
                          int64_t *score_out, int64_t *calls_out,
                          int64_t *beats_out);
#endif
