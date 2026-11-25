/* wv_tb_walk.h -- ON-CARD TRACEBACK for the S1c continuous-band kernel (S1t).
 * ===================================================================================
 * WHY. On the card, the host traceback cost ~1,426 XRT syncs x ~27 us = 32-43 ms at
 * 181 kbp -- as long as the whole alignment (40 ms). The walk itself touches only m+n
 * cells; the cost was the PCIe round trips. Walking on the card removes them: the host
 * reads back one small ops buffer (2 bits per step) instead of ~1,400 direction windows.
 *
 * HOW IT REACHES THE DIRECTIONS WITHOUT A NEW AXI PORT. All 32 HBM AXI connections are
 * taken (31 direction writers + mem). The link connects `mem` to HBM[0:31], so the one
 * control port can address every pseudo-channel through the HBM switch. The host writes
 * each direction buffer's position RELATIVE TO mem (in 64-bit words) into a descriptor
 * at mem[b_off] -- the region S1 freed when the boundary moved on chip -- and the walker
 * reads mem[doff[k] + w]. No kernel argument changes; the host ABI is unchanged.
 *
 * DESCRIPTOR at mem[b_off + x], int64 each:
 *   x = 0 .. WV_NCH-1   word offset of direction buffer k relative to mem   (host)
 *   WV_TBD_EN           1 = walk, 0 = skip (scoring-only timing runs)       (host)
 *   WV_TBD_OPS          word offset of the ops output area                   (host)
 *   WV_TBD_CAP          capacity of that area in steps                       (host)
 *   WV_TBD_NOPS         number of steps written                              (kernel)
 *   WV_TBD_SCORE        score summed along the path                          (kernel)
 *   WV_TBD_STATUS       0 ok, 1 ops area too small, 2 impossible base-3 byte (kernel)
 *   WV_TBD_FILLS        number of cache fills                                (kernel)
 * OPS: step s (s = 0 is the move INTO cell (m,n)) is bits 2(s%32)+1..2(s%32) of word
 * s/32; 0 DIAG, 1 UP, 2 LEFT -- the same codes and order as nw_host_traceback()'s ops.
 *
 * WHERE A CELL'S DIRECTION LIVES (mirror of nwh_dir_at, NWH_CONT, SG=1):
 *   row = i-1, col = j-1;  band = row / WV_PE;  k = (row % WV_PE) / 128;  il = row % 128
 *   c    = band*S + col + il          counted cycle of tile k (S = n + gap)
 *   beat = (c / 5) * 2 + il / 64      byte il % 64 of that beat, digit c % 5 (MSD first)
 *
 * THE STEP LOOP NEVER DIVIDES. Inside one tile run (fixed k, fixed band) every move
 * lowers c by exactly 1 (UP: il-1, LEFT: col-1) or 2 (DIAG: both), so c/5 and c%5 are
 * carried as a (group, digit) pair with a borrow. Divisions happen only when the walk
 * enters a new tile, once per ~128 steps.
 *
 * THE CACHE. The walk inside a tile run descends in beat index, so one window of
 * WV_TB_SPAN beats ENDING at the current beat serves the whole run (host measurement:
 * a run covers ~64 beats). A different tile, or a beat below the window, ends the inner
 * step loop and the outer loop refills. Only the current tile's window is kept: rows only
 * decrease, so a walk never returns to a tile it has left in the same band.
 * ===================================================================================*/
#ifndef WV_TB_WALK_H
#define WV_TB_WALK_H

#ifndef WV_TB_SPAN
#define WV_TB_SPAN 128                      /* beats per cache fill (8 KB) */
#endif
#define WV_TB_WPB   8                       /* 64-bit words per 512-bit beat */

#define WV_TBD_EN     (WV_NCH + 0)
#define WV_TBD_OPS    (WV_NCH + 1)
#define WV_TBD_CAP    (WV_NCH + 2)
#define WV_TBD_NOPS   (WV_NCH + 3)
#define WV_TBD_SCORE  (WV_NCH + 4)
#define WV_TBD_STATUS (WV_NCH + 5)
#define WV_TBD_FILLS  (WV_NCH + 6)
#define WV_TBD_WORDS  (WV_NCH + 7)

#if WV_DIR_SG != 1 || WV_DIR_SCYC != 5 || WV_DIR_SBEATS != 2 || WV_TILE_PE != 128
#error "wv_tb_walk.h assumes WV_TILE_PE=128 (one 5-cycle group = 2 beats)"
#endif

/* digit dpos (0 = earliest cycle = most significant) of a base-3 byte, as a ROM */
static ap_uint<2> wv_b3_digit(ap_uint<8> byte, ap_uint<3> dpos)
{
#pragma HLS INLINE
    static const ap_uint<2> tab[256 * 8] = {
#define WV_B3D(v, d) (ap_uint<2>)((d) < 5 && (v) < 243 ? ((v) / (d == 0 ? 81 : d == 1 ? 27 : d == 2 ? 9 : d == 3 ? 3 : 1)) % 3 : 0)
#define WV_B3R(v) WV_B3D(v,0), WV_B3D(v,1), WV_B3D(v,2), WV_B3D(v,3), WV_B3D(v,4), 0, 0, 0
#define WV_B3R4(v) WV_B3R(v), WV_B3R(v + 1), WV_B3R(v + 2), WV_B3R(v + 3)
#define WV_B3R16(v) WV_B3R4(v), WV_B3R4(v + 4), WV_B3R4(v + 8), WV_B3R4(v + 12)
#define WV_B3R64(v) WV_B3R16(v), WV_B3R16(v + 16), WV_B3R16(v + 32), WV_B3R16(v + 48)
        WV_B3R64(0), WV_B3R64(64), WV_B3R64(128), WV_B3R64(192)
#undef WV_B3R64
#undef WV_B3R16
#undef WV_B3R4
#undef WV_B3R
#undef WV_B3D
    };
    return tab[(((unsigned)byte) << 3) | (unsigned)dpos];
}

static void wv_tb_walk(uint64_t *mem, int64_t b_off,
                       const ap_uint<256> *ubq4, const uint64_t *ubr,
                       int64_t m, int64_t n, uint32_t S, uint32_t nbeats)
{
#pragma HLS INLINE off
    if (mem[b_off + WV_TBD_EN] != 1) return;

    int64_t doff[WV_NCH];
tb_desc:
    for (int k = 0; k < WV_NCH; ++k) {
#pragma HLS PIPELINE II = 1
        doff[k] = (int64_t)mem[b_off + k];
    }
    const int64_t ops_off = (int64_t)mem[b_off + WV_TBD_OPS];
    const int64_t cap     = (int64_t)mem[b_off + WV_TBD_CAP];

    /* cache[w][b] = word w of cached beat clo+b. Split by word so a fill can write all
     * eight words of a beat in one cycle if the port delivers them. */
    static uint64_t cache[WV_TB_WPB][WV_TB_SPAN];
#pragma HLS ARRAY_PARTITION variable = cache dim = 1 complete
    int     ck  = -1;                       /* tile whose window is cached */
    int64_t clo = 1, chi = 0;               /* cached beats [clo, chi]     */

    int32_t i = (int32_t)m, j = (int32_t)n;
    /* row = i-1 decomposed; meaningful only while i > 0 */
    int32_t band = 0, k = 0, il = 0;
    if (i > 0) {
        const int32_t row = i - 1, rib = row % WV_PE;
        band = row / WV_PE; k = rib / WV_TILE_PE; il = rib % WV_TILE_PE;
    }
    int64_t nops = 0, score = 0, fills = 0, status = 0;
    uint64_t opw = 0;

tb_outer:
    while (i > 0 || j > 0) {
#pragma HLS LOOP_TRIPCOUNT min = 1 max = 8192
        /* (re)enter a tile: exact position by division, once per tile run */
        uint32_t grp = 0, dig = 0;          /* c / 5, c % 5 */
        if (i > 0 && j > 0) {
            const uint32_t c = (uint32_t)band * S + (uint32_t)(j - 1) + (uint32_t)il;
            grp = c / WV_DIR_SCYC; dig = c % WV_DIR_SCYC;
            const int64_t beat = (int64_t)grp * WV_DIR_SBEATS + (il >> 6);
            if (k != ck || beat < clo || beat > chi) {
                int64_t lo = beat - (WV_TB_SPAN - 1);
                if (lo < 0) lo = 0;
                int64_t hi = lo + WV_TB_SPAN - 1;
                if (hi > (int64_t)nbeats - 1) hi = (int64_t)nbeats - 1;
                const uint64_t *src = mem + doff[k] + lo * WV_TB_WPB;
                const int32_t nfill = (int32_t)(hi - lo + 1);
tb_fill:
                for (int32_t b = 0; b < nfill; ++b) {
#pragma HLS PIPELINE II = 1
#pragma HLS LOOP_TRIPCOUNT min = 1 max = WV_TB_SPAN
                    for (int w = 0; w < WV_TB_WPB; ++w) {
#pragma HLS UNROLL
                        cache[w][b] = src[(int64_t)b * WV_TB_WPB + w];
                    }
                }
                ck = k; clo = lo; chi = hi; ++fills;
            }
        }

        /* step loop: stays inside the cached window of tile k, band `band` */
        bool leave = false;
tb_step:
        while (!leave) {
#pragma HLS PIPELINE
#pragma HLS LOOP_TRIPCOUNT min = 1 max = 1024
            if (i == 0 && j == 0) break;
            if (nops >= cap) { status = 1; break; }
            ap_uint<2> d;
            if (i > 0 && j > 0) {
                const int64_t beat = (int64_t)grp * WV_DIR_SBEATS + (il >> 6);
                if (beat < clo) break;                 /* below the window: refill */
                const int      byo  = il & 63;
                const uint64_t word = cache[byo >> 3][beat - clo];
                const ap_uint<8> byte = (ap_uint<8>)(word >> (8 * (byo & 7)));
                if (byte > 242) { status = 2; break; }
                d = wv_b3_digit(byte, (ap_uint<3>)dig);
            } else {
                d = (i > 0) ? (ap_uint<2>)NWW_UP : (ap_uint<2>)NWW_LEFT;
            }
            /* path score: verification against the DP score, not needed for the path */
            if (d == NWW_DIAG) {
                const ap_uint<256> qb = ubq4[(i - 1) >> 7];
                const unsigned qv = (unsigned)qb.range(2 * ((i - 1) & 127) + 1, 2 * ((i - 1) & 127));
                const unsigned rv = (unsigned)((ubr[(j - 1) >> 5] >> (2 * ((j - 1) & 31))) & 3);
                score += (qv == rv) ? NWW_MATCH : NWW_MISMATCH;
            } else {
                score += NWW_GAP;
            }
            opw |= (uint64_t)d << (2 * (nops & 31));
            if ((nops & 31) == 31) { mem[ops_off + (nops >> 5)] = opw; opw = 0; }
            ++nops;

            /* move, then keep (grp, dig) = c/5, c%5 for the new cell */
            const int dc = (d == NWW_DIAG) ? 2 : 1;
            if (d != NWW_LEFT) {                       /* row decreases */
                --i;
                if (il == 0) {                         /* leaves the tile: re-enter */
                    il = WV_TILE_PE - 1;
                    if (k == 0) { k = WV_TILES - 1; --band; } else { --k; }
                    leave = true;
                } else {
                    --il;
                }
            }
            if (d != NWW_UP) --j;
            if (dig >= (uint32_t)dc) dig -= dc; else { dig = dig + WV_DIR_SCYC - dc; --grp; }
        }
        if (status != 0) break;
    }
    if ((nops & 31) != 0) mem[ops_off + (nops >> 5)] = opw;
    mem[b_off + WV_TBD_NOPS]   = (uint64_t)nops;
    mem[b_off + WV_TBD_SCORE]  = (uint64_t)score;
    mem[b_off + WV_TBD_STATUS] = (uint64_t)status;
    mem[b_off + WV_TBD_FILLS]  = (uint64_t)fills;
}

#endif /* WV_TB_WALK_H */
