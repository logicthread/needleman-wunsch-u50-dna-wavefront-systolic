/* tile_tbk_tb.cpp -- direction round-trip for the TILED softcore.
 *
 * The directions themselves are proven elsewhere (tb_tb.c, 178 cases, against the
 * scalar model). What THIS tests is the anti-diagonal, beat-packed, per-tile layout and
 * the host's inverse of it: if the mapping is off by one in band, tile, counted column,
 * port, slot or bit, every direction is individually perfect and the alignment is still
 * nonsense. The mapping was verified standalone over 22,126,416 cells first; this
 * checks the KERNEL agrees with that mapping.
 *
 * Re-scoring the recovered path, rather than diffing against a golden traceback, is
 * deliberate: ties are legal and a golden's tie-break is not the spec.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <cstdint>
#include <ap_int.h>
extern "C" {
#include "nw_wave.h"
#ifdef WV_ONCARD_TB
/* descriptor slots -- MUST mirror src/wv_tb_walk.h (WV_NCH == WV_DIR_PORTS here) */
#define WV_TBD_EN     (WV_DIR_PORTS + 0)
#define WV_TBD_OPS    (WV_DIR_PORTS + 1)
#define WV_TBD_CAP    (WV_DIR_PORTS + 2)
#define WV_TBD_NOPS   (WV_DIR_PORTS + 3)
#define WV_TBD_SCORE  (WV_DIR_PORTS + 4)
#define WV_TBD_STATUS (WV_DIR_PORTS + 5)
#define WV_TBD_FILLS  (WV_DIR_PORTS + 6)
#endif
}
extern "C" int32_t nw_align_ref(const uint8_t *, int32_t, const uint8_t *, int32_t);

#ifndef WV_QDEPTH
#define WV_QDEPTH 4096
#endif
#ifndef WV_RDEPTH
#define WV_RDEPTH 1048576
#endif
#ifndef WV_BDEPTH
#define WV_BDEPTH 1048576
#endif
#ifndef WV_DIRDEPTH
#define WV_DIRDEPTH 4096
#endif
#ifndef WV_TILE_PE
#define WV_TILE_PE 128
#endif
#define WV_PER_WORD       32
#define WV_TILES          (NW_PE / WV_TILE_PE)
/* ---- WV_SHARE must MIRROR the kernel exactly (kernel_tile_1b.cpp) ---------
 * WV_SHARE tiles share one HBM channel, group-interleaved. Tile k lives on port
 * k/WV_SHARE at sub-slot k%WV_SHARE, and the channel's band stride is
 * bpb*WV_SHARE. If this mirror drifts from the kernel the decode is silently
 * wrong, which is exactly what the DIRDIFF and >242 group checks below catch. */
#ifndef WV_SHARE
#define WV_SHARE 1
#endif
#if (WV_TILES % WV_SHARE) != 0
#error "WV_SHARE must divide WV_TILES exactly -- see kernel_tile_1b.cpp"
#endif
#if WV_SHARE > 1 && !defined(WV_DIR_B3)
#error "WV_SHARE > 1 requires WV_DIR_B3: the shared writer interleaves at BASE-3 GROUP granularity, which the non-B3 layout has no concept of."
#endif
#define WV_NCH            (WV_TILES / WV_SHARE)
/* ONE port per tile: 128 PEs x 2 bits = 256 bits per cycle, so 2 counted cycles fill a
 * 512-bit beat = 256 cell values, 16 per 32-bit word. Verified standalone over
 * 25,867,509 cells, 0 wrong and 0 never-written. */
#define WV_DIR_WBITS      (WV_TILE_PE * 2)
#define WV_DIR_BEATBITS   512
#define WV_DIR_PACK       (WV_DIR_BEATBITS / WV_DIR_WBITS)
#ifdef WV_DIR_B3
/* must mirror kernel_tile.cpp exactly */
#define WV_DIR_GROUP      5
#define WV_DIR_BPG        ((WV_TILE_PE * 8) / WV_DIR_BEATBITS)
#define WV_DIR_BYTES      (WV_DIR_BEATBITS / 8)
/* SUPERGROUP -- must mirror kernel_tile_1b.cpp exactly. A group yields WV_TILE_PE
 * bytes, which only fill whole 512-bit beats when WV_TILE_PE % 64 == 0. At 160 it
 * does not (160*8/512 = 2.5), so emission is keyed to WV_DIR_SG groups:
 *   128 -> SG 1, 128 B, 2 beats, 5 cycles   (the shipped layout, unchanged)
 *   160 -> SG 2, 320 B, 5 beats, 10 cycles  */
#define WV_P2OF64(x)      ( ((x) % 64 == 0) ? 64 : ((x) % 32 == 0) ? 32 \
                          : ((x) % 16 == 0) ? 16 : ((x) %  8 == 0) ?  8 : 1 )
#define WV_DIR_SG         (64 / WV_P2OF64(WV_TILE_PE))
#define WV_DIR_SBYTES     (WV_DIR_SG * WV_TILE_PE)
#define WV_DIR_SBEATS     (WV_DIR_SBYTES / WV_DIR_BYTES)
#define WV_DIR_SCYC       (WV_DIR_SG * WV_DIR_GROUP)
static const unsigned WV_P3[5] = {81u, 27u, 9u, 3u, 1u};  /* d0 is MOST significant */
#endif
#define WV_DIR_PORTS      WV_NCH
/* S1c: continuous bands. Band stride S = n + g, g = max(0, WV_NMIN - n); must mirror
 * kernel_tile_1b_s1c.cpp. Tile k's counted cycle for (band, j, lane i) is band*S + j + i. */
#ifndef WV_NMIN
#define WV_NMIN (NW_PE + 8 * WV_TILE_PE)
#endif
#ifndef __SYNTHESIS__
extern int wv_qstarve;
#endif
typedef ap_uint<WV_DIR_BEATBITS> dirbeat;

#if WV_DIR_PORTS == 4
extern "C" void nw_tile_u50(uint64_t *, int64_t, int64_t, int64_t, int64_t,
                            int64_t, int64_t,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *);
#define WV_DIRARGS \
                dbuf[0].data(), dbuf[1].data(), dbuf[2].data(), \
                dbuf[3].data()
#elif WV_DIR_PORTS == 8
extern "C" void nw_tile_u50(uint64_t *, int64_t, int64_t, int64_t, int64_t,
                            int64_t, int64_t,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *);
#define WV_DIRARGS \
                dbuf[0].data(), dbuf[1].data(), dbuf[2].data(), \
                dbuf[3].data(), dbuf[4].data(), dbuf[5].data(), \
                dbuf[6].data(), dbuf[7].data()
#elif WV_DIR_PORTS == 32
extern "C" void nw_tile_u50(uint64_t *, int64_t, int64_t, int64_t, int64_t,
                            int64_t, int64_t,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *);
#define WV_DIRARGS \
                dbuf[0].data(), dbuf[1].data(), dbuf[2].data(), \
                dbuf[3].data(), dbuf[4].data(), dbuf[5].data(), \
                dbuf[6].data(), dbuf[7].data(), dbuf[8].data(), \
                dbuf[9].data(), dbuf[10].data(), dbuf[11].data(), \
                dbuf[12].data(), dbuf[13].data(), dbuf[14].data(), \
                dbuf[15].data(), dbuf[16].data(), dbuf[17].data(), \
                dbuf[18].data(), dbuf[19].data(), dbuf[20].data(), \
                dbuf[21].data(), dbuf[22].data(), dbuf[23].data(), \
                dbuf[24].data(), dbuf[25].data(), dbuf[26].data(), \
                dbuf[27].data(), dbuf[28].data(), dbuf[29].data(), \
                dbuf[30].data(), dbuf[31].data()
#elif WV_DIR_PORTS == 31
extern "C" void nw_tile_u50(uint64_t *, int64_t, int64_t, int64_t, int64_t,
                            int64_t, int64_t,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *);
#define WV_DIRARGS \
                dbuf[0].data(), dbuf[1].data(), dbuf[2].data(), \
                dbuf[3].data(), dbuf[4].data(), dbuf[5].data(), \
                dbuf[6].data(), dbuf[7].data(), dbuf[8].data(), \
                dbuf[9].data(), dbuf[10].data(), dbuf[11].data(), \
                dbuf[12].data(), dbuf[13].data(), dbuf[14].data(), \
                dbuf[15].data(), dbuf[16].data(), dbuf[17].data(), \
                dbuf[18].data(), dbuf[19].data(), dbuf[20].data(), \
                dbuf[21].data(), dbuf[22].data(), dbuf[23].data(), \
                dbuf[24].data(), dbuf[25].data(), dbuf[26].data(), \
                dbuf[27].data(), dbuf[28].data(), dbuf[29].data(), \
                dbuf[30].data()
#elif WV_DIR_PORTS == 24
extern "C" void nw_tile_u50(uint64_t *, int64_t, int64_t, int64_t, int64_t,
                            int64_t, int64_t,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *);
#define WV_DIRARGS \
                dbuf[0].data(), dbuf[1].data(), dbuf[2].data(), \
                dbuf[3].data(), dbuf[4].data(), dbuf[5].data(), \
                dbuf[6].data(), dbuf[7].data(), dbuf[8].data(), \
                dbuf[9].data(), dbuf[10].data(), dbuf[11].data(), \
                dbuf[12].data(), dbuf[13].data(), dbuf[14].data(), \
                dbuf[15].data(), dbuf[16].data(), dbuf[17].data(), \
                dbuf[18].data(), dbuf[19].data(), dbuf[20].data(), \
                dbuf[21].data(), dbuf[22].data(), dbuf[23].data()
#elif WV_DIR_PORTS == 16
extern "C" void nw_tile_u50(uint64_t *, int64_t, int64_t, int64_t, int64_t,
                            int64_t, int64_t,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *);
#define WV_DIRARGS \
                dbuf[0].data(), dbuf[1].data(), dbuf[2].data(), \
                dbuf[3].data(), dbuf[4].data(), dbuf[5].data(), \
                dbuf[6].data(), dbuf[7].data(), dbuf[8].data(), \
                dbuf[9].data(), dbuf[10].data(), dbuf[11].data(), \
                dbuf[12].data(), dbuf[13].data(), dbuf[14].data(), \
                dbuf[15].data()
#elif WV_DIR_PORTS == 1
extern "C" void nw_tile_u50(uint64_t *, int64_t, int64_t, int64_t, int64_t,
                            int64_t, int64_t,
                            dirbeat *);
#define WV_DIRARGS \
                dbuf[0].data()
#elif WV_DIR_PORTS == 30
extern "C" void nw_tile_u50(uint64_t *, int64_t, int64_t, int64_t, int64_t,
                            int64_t, int64_t,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *,
                            dirbeat *);
#define WV_DIRARGS \
                dbuf[0].data(), dbuf[1].data(), dbuf[2].data(), \
                dbuf[3].data(), dbuf[4].data(), dbuf[5].data(), \
                dbuf[6].data(), dbuf[7].data(), dbuf[8].data(), \
                dbuf[9].data(), dbuf[10].data(), dbuf[11].data(), \
                dbuf[12].data(), dbuf[13].data(), dbuf[14].data(), \
                dbuf[15].data(), dbuf[16].data(), dbuf[17].data(), \
                dbuf[18].data(), dbuf[19].data(), dbuf[20].data(), \
                dbuf[21].data(), dbuf[22].data(), dbuf[23].data(), \
                dbuf[24].data(), dbuf[25].data(), dbuf[26].data(), \
                dbuf[27].data(), dbuf[28].data(), dbuf[29].data()
#else
#error "testbench supports 1, 4, 8, 16, 24, 30, 31 or 32 dir ports -- at WV_TILE_PE=128 that is 512/1024/2048/3072/3968/4096 PE; at 160 it is 160/640/2560/4800 PE"
#endif

static uint32_t rs = 2463534242u;
static uint32_t rnd() { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
/* ONE buffer. The kernel takes word offsets into it, so the layout is the host's
 * choice -- here: query, reference, boundary, score, each word-aligned. The m_axi depth
 * must EQUAL this allocation (N11/N38): a mismatch is undefined behaviour that no level
 * of the tool reports. */
static std::vector<uint64_t> mem;
static int64_t Q_OFF, R_OFF, B_OFF, S_OFF;
static int64_t OPS_OFF = 0, OPS_CAP = 0;   /* S1t on-card ops area */
static std::vector<dirbeat>  dbuf[WV_DIR_PORTS];
static int bad = 0, ran = 0;
static int b3bad = 0;   /* impossible base-3 bytes seen */

static void pack(std::vector<uint64_t> &dst, const uint8_t *src, int64_t len) {
    std::fill(dst.begin(), dst.end(), 0ull);
    for (int64_t i = 0; i < len; ++i)
        dst[i / WV_PER_WORD] |= (uint64_t)(src[i] & 3) << (2 * (i % WV_PER_WORD));
}

static void chk(const char *tag, const std::vector<uint8_t> &q,
                const std::vector<uint8_t> &r) {
    const int64_t m = (int64_t)q.size(), n = (int64_t)r.size();
    const int64_t nbands = (m + NW_PE - 1) / NW_PE;
    /* S1c continuous layout, exactly as the kernel computes it */
#if !defined(S1C_VAR) || S1C_VAR == 11 || S1C_VAR == 12
#ifndef WV_BWIN
#define WV_BWIN 640
#endif
    const int64_t gap   = (n + WV_BWIN < WV_NMIN) ? (int64_t)WV_NMIN - n : (int64_t)WV_BWIN;
#else
    const int64_t gap   = (n < WV_NMIN) ? (int64_t)WV_NMIN - n : 0;
#endif
    const int64_t S     = n + gap;
    const int64_t clast = (nbands - 1) * S + n + WV_TILE_PE;
    const int64_t cmax  = (clast + WV_DIR_SCYC - 1) / WV_DIR_SCYC * WV_DIR_SCYC;
    const int64_t ntot  = (cmax / WV_DIR_SCYC) * WV_DIR_SBEATS;
    if (ntot > WV_DIRDEPTH) { printf("  SKIP %s (dir depth)\n", tag); return; }
    const int starve0 = wv_qstarve;


    /* CAPACITY GUARD. This is the fault that cost job 105271 (NOTES N38): the cosim
     * config carried WV_QDEPTH=64 (64 words x 32 = 2048 bases) while the cosim case
     * uses m = NW_PE = 3072, so pack() wrote past qbuf and wv_qloader read past the
     * m_axi depth. Neither is an error at any level of the tool -- it is undefined
     * behaviour that surfaced as a wrong score, and csim passed because ITS config
     * said 256. At NW_PE=2048 the same 64 fit with EXACTLY zero margin, which is the
     * only reason 2048 ever verified.
     *
     * A hard failure, deliberately NOT the `SKIP` used for dir depth above: a skipped
     * case at cosim would leave ran==0 and still print PASSED, which is the worse bug. */
    if (m > (int64_t)WV_QDEPTH * WV_PER_WORD || n > (int64_t)WV_RDEPTH * WV_PER_WORD) {
        ++ran; ++bad;
        printf("  CONFIG ERROR %-14s m=%lld n=%lld exceeds buffer: "
               "query cap %lld bases (WV_QDEPTH=%d), ref cap %lld bases (WV_RDEPTH=%d)\n",
               tag, (long long)m, (long long)n,
               (long long)WV_QDEPTH * WV_PER_WORD, (int)WV_QDEPTH,
               (long long)WV_RDEPTH * WV_PER_WORD, (int)WV_RDEPTH);
        return;
    }

    /* lay the inputs out at their offsets; zero everything else so a stray read is
     * visible as a wrong answer rather than as stale data from the previous case */
    std::fill(mem.begin(), mem.end(), 0ull);
    for (int64_t i = 0; i < m; ++i)
        mem[Q_OFF + i / WV_PER_WORD] |= (uint64_t)(q[i] & 3) << (2 * (i % WV_PER_WORD));
    for (int64_t j = 0; j < n; ++j)
        mem[R_OFF + j / WV_PER_WORD] |= (uint64_t)(r[j] & 3) << (2 * (j % WV_PER_WORD));
    for (int p = 0; p < WV_DIR_PORTS; ++p) std::fill(dbuf[p].begin(), dbuf[p].end(), 0);
    mem[S_OFF] = 0;
#ifdef WV_ONCARD_TB
    /* S1t descriptor: direction buffers addressed RELATIVE TO mem, as the host does with
     * xrt::bo::address() on the card. In csim the buffers are ordinary host arrays, so
     * the word difference of their addresses plays that role. */
    for (int p = 0; p < WV_DIR_PORTS; ++p)
        mem[B_OFF + p] = (uint64_t)((const uint64_t *)dbuf[p].data() - mem.data());
    mem[B_OFF + WV_TBD_EN]  = 1;
    mem[B_OFF + WV_TBD_OPS] = OPS_OFF;
    mem[B_OFF + WV_TBD_CAP] = OPS_CAP;
#endif

    nw_tile_u50(mem.data(), Q_OFF, R_OFF, B_OFF, S_OFF, m, n,
                WV_DIRARGS);

    /* ---- INVERSE MAPPING (verified standalone over 22.1M cells) ----
     * cell (row, j): band = row/NW_PE, tile k = (row%NW_PE)/WV_TILE_PE,
     * local PE i = (row%NW_PE)%WV_TILE_PE. Tile k's COUNTED column is c = j + i -- a
     * count from its own first active cycle, not a cycle index, so FIFO latency
     * anywhere upstream is irrelevant. */
    std::vector<uint8_t> dirs((size_t)m * n, 0xff);
    for (int64_t row = 0; row < m; ++row) {
        const int64_t band = row / NW_PE, rib = row % NW_PE;
        const int     k = (int)(rib / WV_TILE_PE), i = (int)(rib % WV_TILE_PE);
        /* WV_SHARE tiles per channel: port is k/WV_SHARE, and sub is this tile's
         * slot inside each interleaved group. WV_SHARE==1 gives p==k, sub==0. */
        const int     p   = k / WV_SHARE;
        const int     sub = k % WV_SHARE;
        for (int64_t j = 0; j < n; ++j) {
            const int64_t c    = band * S + j + i;
#ifdef WV_DIR_B3
            /* BASE-3, 5 CELLS PER BYTE. Group g = c/5 produces WV_DIR_BPG beats; lanes
             * 0..63 land in the first, 64..127 in the second (the kernel writes ob0
             * before ob1, so ob0 is the lower beat index). Inside a beat, lane i owns
             * byte i%64. Digit dpos within the group carries weight 3^(4-dpos) because
             * the kernel applies acc = acc*3 + d once per counted cycle, making the
             * FIRST cycle the most significant digit. */
            /* Supergroup sg of WV_DIR_SG groups; this cycle sits in group gsg of it.
             * The byte index runs across the WHOLE supergroup, so lane i of group
             * gsg owns byte gsg*WV_TILE_PE + i, and that byte's beat is index/64.
             * At WV_TILE_PE=128 (SG=1) this reduces to the original g*2 + i/64. */
            const int64_t sg    = c / WV_DIR_SCYC;
            const int     gsg   = (int)((c / WV_DIR_GROUP) % WV_DIR_SG);
            const int     dpos  = (int)(c % WV_DIR_GROUP);
            const int     bidx  = gsg * WV_TILE_PE + i;
            const int64_t beat  = (sg * WV_SHARE + sub) * WV_DIR_SBEATS
                                + (bidx / WV_DIR_BYTES);
            const int     boff  = 8 * (bidx % WV_DIR_BYTES);
            const unsigned byte = (unsigned)dbuf[p][beat].range(boff + 7, boff);
            /* 3^5 - 1 = 242 is the largest representable group. A larger value cannot
             * come from exactly five `acc = acc*3 + d` steps, so it means a lane was
             * multiplied the wrong number of times (a misaligned group) or the beat was
             * mis-addressed. The per-cell DIRDIFF below would catch it too, but this
             * names the cause directly instead of reporting a wrong direction. */
            if (byte > 242u && b3bad < 4) {
                ++b3bad;
                printf("    B3RANGE row=%lld j=%lld tile=%d i=%d c=%lld beat=%lld "
                       "byte=%u > 242 (impossible for 5 base-3 digits)\n",
                       (long long)row, (long long)j, k, i, (long long)c,
                       (long long)beat, byte);
            }
            dirs[(size_t)row * n + j] = (uint8_t)((byte / WV_P3[dpos]) % 3u);
#else
            (void)sub;  /* non-B3 layout: WV_SHARE>1 is rejected at the top */
            const int64_t beat = c / WV_DIR_PACK;   /* unreachable: S1c is B3-only */
            const int     lo   = WV_DIR_WBITS * (int)(c % WV_DIR_PACK) + 2 * i;
            dirs[(size_t)row * n + j] =
                (uint8_t)(unsigned)dbuf[p][beat].range(lo + 1, lo);
#endif
        }
    }
    /* ---- DIFF AGAINST THE SCALAR MODEL, which emits row-major directions directly.
     * Localises a layout error to a specific (row, j) and therefore to a specific
     * (tile, local PE, counted column, port, slot) instead of just a bad path score. */
    {
        std::vector<int8_t>  mb((size_t)(n > 0 ? n : 1), 0);
        std::vector<uint8_t> md((size_t)m * n, 0);
        nw_wave_dirs(q.data(), m, r.data(), n, mb.data(), md.data());
        int shown = 0;
        for (int64_t row = 0; row < m && shown < 4; ++row)
            for (int64_t j = 0; j < n && shown < 4; ++j) {
                const size_t t = (size_t)row * n + j;
                if (dirs[t] != md[t]) {
                    const int64_t rib = row % NW_PE;
                    const int k = (int)(rib / WV_TILE_PE), i = (int)(rib % WV_TILE_PE);
                    printf("    DIRDIFF row=%lld j=%lld tile=%d i=%d c=%lld slot=%lld "
                           "port=%d kernel=%u model=%u\n",
                           (long long)row, (long long)j, k, i, (long long)(j + i),
                           #ifdef WV_DIR_B3
                           (long long)((j + i) % WV_DIR_GROUP),
#else
                           (long long)((j + i) % WV_DIR_PACK),
#endif
                           k, dirs[t], md[t]);
                    ++shown;
                }
            }
    }

    for (size_t t = 0; t < dirs.size(); ++t)
        if (dirs[t] == 0xff) {
            if (++bad <= 6) printf("  FAIL %-16s m=%lld n=%lld cell %zu never written\n",
                                   tag, (long long)m, (long long)n, t);
            return;
        }

    /* ---- RE-SCORE via nw_wave_traceback(), which is already proven (tb_tb.c, 178
     * cases, against the scalar model). Re-scoring rather than diffing a golden's path
     * is deliberate: ties are legal and a golden's tie-break is not the spec.
     *
     * My first version hand-rolled the walk and read dirs[(i2-1)*n + (j2-1)] BEFORE
     * testing i2>0 && j2>0. Once j2 hits 0 -- early whenever m >> n, e.g. 336 UP moves
     * at m=356, n=20 -- that index is (i2-1)*n - 1: in range, but the previous row's
     * last column. No crash, wrong cell, and a bogus DIAG there sends the score
     * negative. That was the entire 17/18 failure, and the kernel was never wrong: a
     * plain-C model of the tiled chain matched the row-major golden on all 15 cases
     * including the failing ones, which is what pointed at the harness. */
    int64_t ops = 0;
    const int64_t sc = nw_wave_traceback(q.data(), m, r.data(), n, dirs.data(), &ops);
    const int64_t gold = (int64_t)nw_align_ref(q.data(), (int32_t)m, r.data(), (int32_t)n);
    ++ran;
    if (wv_qstarve != starve0) {
        ++bad;
        printf("  FAIL %-16s m=%lld n=%lld QUERY STARVED (%d lane-0 band starts before the block)\n",
               tag, (long long)m, (long long)n, wv_qstarve - starve0);
    }
    if (sc != gold || mem[S_OFF] != gold) {
        if (++bad <= 8)
            printf("  FAIL %-16s m=%lld n=%lld golden=%lld path=%lld score=%lld\n",
                   tag, (long long)m, (long long)n, (long long)gold,
                   (long long)sc, (long long)mem[S_OFF]);
    }
#ifdef WV_ONCARD_TB
    {   /* the walk the card must reproduce, step for step, from the decoded dirs */
        std::vector<uint8_t> want;
        int64_t i2 = m, j2 = n;
        while (i2 > 0 || j2 > 0) {
            int d = (i2 > 0 && j2 > 0) ? dirs[(size_t)(i2 - 1) * n + (j2 - 1)] : (i2 > 0 ? 1 : 2);
            want.push_back((uint8_t)d);
            if (d != 2) --i2;
            if (d != 1) --j2;
        }
        const int64_t nops = (int64_t)mem[B_OFF + WV_TBD_NOPS];
        const int64_t psc  = (int64_t)mem[B_OFF + WV_TBD_SCORE];
        const int64_t st   = (int64_t)mem[B_OFF + WV_TBD_STATUS];
        int64_t diff = -1;
        if (nops == (int64_t)want.size())
            for (int64_t s2 = 0; s2 < nops; ++s2)
                if ((int)((mem[OPS_OFF + s2 / 32] >> (2 * (s2 % 32))) & 3) != want[s2]) { diff = s2; break; }
        if (st != 0 || nops != (int64_t)want.size() || diff >= 0 || psc != gold) {
            if (++bad <= 8)
                printf("  FAIL %-16s m=%lld n=%lld ONCARD TB status=%lld nops=%lld want=%zu "
                       "first diff=%lld path score=%lld gold=%lld\n",
                       tag, (long long)m, (long long)n, (long long)st, (long long)nops,
                       want.size(), (long long)diff, (long long)psc, (long long)gold);
        } else {
            printf("  ok   %-16s on-card traceback %lld steps, %lld fills\n", tag,
                   (long long)nops, (long long)mem[B_OFF + WV_TBD_FILLS]);
        }
    }
#endif
}

static void gen(std::vector<uint8_t> &v, int64_t len) {
    v.resize((size_t)len);
    for (int64_t i = 0; i < len; ++i) v[(size_t)i] = (uint8_t)(rnd() % 4);
}

int main() {
    /* Layout, in 64-bit words. Must match WV_MEMDEPTH in the kernel exactly. */
    Q_OFF = 0;
    R_OFF = Q_OFF + WV_QDEPTH;
    B_OFF = R_OFF + WV_RDEPTH;
    S_OFF = B_OFF + (WV_BDEPTH + 7) / 8;
#ifdef WV_ONCARD_TB
    /* ops area after the score: 2 bits per step, at most m + n steps */
    OPS_OFF = S_OFF + 1;
    OPS_CAP = (int64_t)(WV_QDEPTH + WV_RDEPTH) * WV_PER_WORD;
    mem.assign((size_t)(OPS_OFF + OPS_CAP / 32 + 1), 0ull);
    {   /* the walker reads beats as 8 little-endian 64-bit words: prove the csim type
         * stores them that way, or every offset above is meaningless */
        dirbeat t = 0; t.range(7, 0) = 0x11; t.range(71, 64) = 0x22; t.range(511, 504) = 0x33;
        const uint8_t *b = (const uint8_t *)&t;
        if (sizeof(dirbeat) != 64 || b[0] != 0x11 || b[8] != 0x22 || b[63] != 0x33) {
            printf("CONFIG ERROR: ap_uint<512> is not 64 little-endian bytes in csim\n");
            return 1;
        }
    }
#else
    mem.assign((size_t)(S_OFF + 1), 0ull);
#endif
    printf("single-bundle layout: q@%lld r@%lld b@%lld s@%lld  total %lld words\n",
           (long long)Q_OFF,(long long)R_OFF,(long long)B_OFF,(long long)S_OFF,
           (long long)(S_OFF + 1));
    for (int p = 0; p < WV_DIR_PORTS; ++p) dbuf[p].assign(WV_DIRDEPTH, 0);
    printf("NW_PE=%d  %d tiles x %d PE  %d dir ports x %d bit/cycle  PACK=%d "
           "(%d cells per 512-bit beat, 16 per 32-bit word)\n",
           NW_PE, WV_TILES, WV_TILE_PE, WV_DIR_PORTS, WV_DIR_WBITS, WV_DIR_PACK,
           WV_DIR_BEATBITS / 2);

    std::vector<uint8_t> q, r;
#ifdef NW_TB_SMALL
    /* COSIM PATH. Two single-band cases, n differing by 600, so cycles/column comes from
     * the DIFFERENCE and the fill/drain/qload/AXI overhead cancels. Single band only:
     * bnd_r/bnd_w alias one buffer and cosim's wrapper copies each port separately,
     * which de-aliases them (see tile_tb.cpp). What this measures is whether the four
     * dir writers sustain the array at II=1 -- the question the FIFO exists to answer. */
    { gen(q, NW_PE); gen(r, 600);
      printf("  cosim A: m=%d n=600 single band\n", NW_PE);  chk("A", q, r); }
    { gen(q, NW_PE); gen(r, 1200);
      printf("  cosim B: m=%d n=1200 single band\n", NW_PE); chk("B", q, r); }
    /* S1: MULTI-BAND in cosim. Valid for kernel_tile_1b because the boundary is on chip
     * (no bnd_r/bnd_w aliasing for the cosim wrapper to de-alias). Multi-band was never
     * checked at RTL before, and kernel_tile.cpp's multi-band then failed on silicon. */
    { gen(q, 3 * NW_PE); gen(r, 300);
      printf("  cosim C: m=%d n=300 THREE bands (gap %d)\n", 3 * NW_PE, WV_NMIN - 300); chk("C multi-band", q, r); }
    /* S1c: n >= WV_NMIN, so g = 0 -- bands run back to back. D and E differ only in band
     * count, so latency(E) - latency(D) is the cost of ONE extra band: ideal = n. */
    { gen(q, 2 * NW_PE); gen(r, WV_NMIN + 40);
      printf("  cosim D: m=%d n=%d TWO bands, no gap\n", 2 * NW_PE, WV_NMIN + 40); chk("D 2 bands g0", q, r); }
    { gen(q, 3 * NW_PE); gen(r, WV_NMIN + 40);
      printf("  cosim E: m=%d n=%d THREE bands, no gap\n", 3 * NW_PE, WV_NMIN + 40); chk("E 3 bands g0", q, r); }
    printf("%d cases, %d failures\n", ran, bad);
    printf("%s\n", bad ? "*** TILE TRACEBACK FAILED ***" : "TILE TRACEBACK PASSED");
    return bad ? 1 : 0;
#endif
    for (int i = 0; i < 6; ++i) {                       /* K1 single band */
        const int64_t m = 1 + (int64_t)(rnd() % NW_PE);
        gen(q, m); gen(r, 1 + (int64_t)(rnd() % 120)); chk("K1 single band", q, r);
    }
    for (int dm = -1; dm <= 1; ++dm) {                  /* K2 band boundary */
        const int64_t m = NW_PE + dm; if (m <= 0) continue;
        gen(q, m); gen(r, 90); chk("K2 band boundary", q, r);
    }
    for (int dk = -1; dk <= 1; ++dk) {                  /* K3 TILE boundary */
        const int64_t m = WV_TILE_PE * 3 + dk; if (m <= 0) continue;
        gen(q, m); gen(r, 70); chk("K3 tile boundary", q, r);
    }
    for (int64_t n = 1; n <= 5; ++n) {                  /* K4 array never fills */
        gen(q, NW_PE + 5); gen(r, n); chk("K4 tiny n", q, r);
    }
    { gen(q, WV_TILE_PE); r = q; chk("K5 identical", q, r); }
    /* S1c continuous-band edges */
    { gen(q, 3 * NW_PE + 7); gen(r, WV_NMIN);     chk("K6 n=NMIN g=0", q, r); }
    { gen(q, 2 * NW_PE);     gen(r, WV_NMIN - 1); chk("K6 n=NMIN-1 g=1", q, r); }
    { gen(q, 2 * NW_PE - 3); gen(r, WV_NMIN + 33); chk("K6 n=NMIN+33", q, r); }
    { gen(q, 8 * NW_PE);     gen(r, 37);          chk("K7 8 bands", q, r); }
    { gen(q, 2 * NW_PE);     gen(r, 1);           chk("K7 n=1 2 bands", q, r); }

    printf("%d cases, %d failures\n", ran, bad);
    printf("%s\n", bad ? "*** TILE TRACEBACK FAILED ***" : "TILE TRACEBACK PASSED");
    return bad ? 1 : 0;
}
