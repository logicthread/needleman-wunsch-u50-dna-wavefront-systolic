/* tb_walk_local.cpp -- unit test of the on-card traceback walker (src/wv_tb_walk.h).
 *
 * Builds the S1c continuous-band direction beats from the proven scalar model
 * (nw_wave_dirs), lays them out at offsets inside one arena exactly as the host will on
 * the card (mem + 31 direction buffers, addressed relative to mem), runs wv_tb_walk, and
 * requires:
 *   ops  == nw_host_traceback() ops, step for step   (the host walker proven on silicon)
 *   path score == nw_align_ref() (the DP optimum), status 0, nops equal.
 * Plain C++ with the open-source ap_int headers; runs in seconds on a laptop. Vitis csim
 * of the whole kernel (tile_1b_tb_s1c.cpp -DWV_ONCARD_TB) is the integration test.
 *
 *   build: see scripts/walk_local.sh
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include <ap_int.h>

#ifndef NW_PE
#define NW_PE 512
#endif
#define WV_PE        NW_PE
#define WV_TILE_PE   128
#define WV_TILES     (WV_PE / WV_TILE_PE)
#define WV_NCH       WV_TILES
#define WV_DIR_SG    1
#define WV_DIR_GROUP 5
#define WV_DIR_SCYC  5
#define WV_DIR_SBEATS 2
#define WV_NMIN      (WV_PE + 8 * WV_TILE_PE)
#define WV_BWIN      640
#define WV_GAP_OF(n) (((n) + WV_BWIN < WV_NMIN) ? (WV_NMIN - (n)) : WV_BWIN)

extern "C" {
#include "nw_wave.h"
#include "nw.h"
#include "nw_host_traceback.h"
}
#include "wv_tb_walk.h"

static uint32_t rs = 88172645u;
static uint32_t rnd() { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

struct port_ctx { std::vector<std::vector<uint8_t>> *ports; };
static int fetch(void *ctx, int port, int64_t first, int64_t count, uint8_t *dst) {
    std::vector<uint8_t> &p = (*((port_ctx *)ctx)->ports)[port];
    memcpy(dst, p.data() + first * 64, (size_t)count * 64);
    return 0;
}

static int bad = 0, ran = 0;

static void run(const char *tag, int64_t m, int64_t n, bool similar) {
    std::vector<uint8_t> q(m), r(n);
    for (auto &x : q) x = rnd() % 4;
    for (int64_t t = 0; t < n; ++t) r[t] = (similar && t < m && rnd() % 10) ? q[t] : rnd() % 4;

    /* scalar model directions, row-major */
    std::vector<int8_t>  bnd(n > 0 ? n : 1);
    std::vector<uint8_t> dirs((size_t)m * n);
    nw_wave_dirs(q.data(), m, r.data(), n, bnd.data(), dirs.data());

    /* S1c layout */
    const int64_t gap = WV_GAP_OF(n), S = n + gap;
    const int64_t nb = (m + WV_PE - 1) / WV_PE;
    const int64_t clast = (nb - 1) * S + n + WV_TILE_PE;
    const int64_t cmax = (clast + 4) / 5 * 5;
    const int64_t nbeats = cmax / 5 * 2;
    static const unsigned P3[5] = {81, 27, 9, 3, 1};
    std::vector<std::vector<uint8_t>> ports(WV_NCH, std::vector<uint8_t>((size_t)nbeats * 64, 0));
    for (int64_t row = 0; row < m; ++row) {
        const int64_t band = row / WV_PE, rib = row % WV_PE;
        const int k = (int)(rib / 128), il = (int)(rib % 128);
        for (int64_t col = 0; col < n; ++col) {
            const int64_t c = band * S + col + il;
            const int64_t beat = (c / 5) * 2 + il / 64;
            ports[k][(size_t)beat * 64 + (il % 64)] += (uint8_t)(dirs[(size_t)row * n + col] * P3[c % 5]);
        }
    }

    /* host walker (reference) */
    nwh_geom g = { m, n, WV_PE, WV_TILE_PE, 2, -1, -2 };
    port_ctx pc = { &ports };
    std::vector<uint8_t> hops((size_t)(m + n + 8));
    int64_t hscore = 0;
    const int64_t hn = nw_host_traceback(&g, q.data(), r.data(), fetch, &pc,
                                         hops.data(), (int64_t)hops.size(), &hscore, 0, 0);

    /* arena: [mem: descriptor | ops] [dir 0] [dir 1] ... -- offsets relative to mem */
    const int64_t DESC = 0, OPS = 64, opsw = (m + n + 31) / 32 + 1;
    const int64_t dir0 = OPS + opsw + 8;
    const int64_t total = dir0 + (int64_t)WV_NCH * nbeats * 8;
    std::vector<uint64_t> arena((size_t)total, 0);
    for (int k = 0; k < WV_NCH; ++k) {
        arena[DESC + k] = (uint64_t)(dir0 + (int64_t)k * nbeats * 8);
        memcpy(&arena[(size_t)(dir0 + (int64_t)k * nbeats * 8)], ports[k].data(), (size_t)nbeats * 64);
    }
    arena[DESC + WV_TBD_EN]  = 1;
    arena[DESC + WV_TBD_OPS] = OPS;
    arena[DESC + WV_TBD_CAP] = m + n + 8;

    /* on-chip q/r as the kernel holds them */
    std::vector<ap_uint<256>> ubq4((m + 127) / 128 + 1, 0);
    for (int64_t t = 0; t < m; ++t) ubq4[t >> 7].range(2 * (t & 127) + 1, 2 * (t & 127)) = q[t];
    std::vector<uint64_t> ubr((n + 31) / 32 + 1, 0);
    for (int64_t t = 0; t < n; ++t) ubr[t >> 5] |= (uint64_t)r[t] << (2 * (t & 31));

    wv_tb_walk(arena.data(), DESC, ubq4.data(), ubr.data(), m, n, (uint32_t)S, (uint32_t)nbeats);

    const int64_t nops = (int64_t)arena[DESC + WV_TBD_NOPS];
    const int64_t sc   = (int64_t)arena[DESC + WV_TBD_SCORE];
    const int64_t st   = (int64_t)arena[DESC + WV_TBD_STATUS];
    const int64_t fl   = (int64_t)arena[DESC + WV_TBD_FILLS];
    const int64_t gold = nw_align_ref(q.data(), (int32_t)m, r.data(), (int32_t)n);
    int diff = -1;
    if (nops == hn)
        for (int64_t s = 0; s < nops; ++s)
            if (((arena[OPS + s / 32] >> (2 * (s % 32))) & 3) != hops[s]) { diff = (int)s; break; }
    ++ran;
    const bool ok = st == 0 && nops == hn && diff < 0 && sc == gold && hscore == gold;
    if (!ok) ++bad;
    printf("  %-5s %-18s m=%-6lld n=%-6lld bands=%lld nops=%lld fills=%lld score=%lld gold=%lld st=%lld%s\n",
           ok ? "ok" : "FAIL", tag, (long long)m, (long long)n, (long long)nb, (long long)nops,
           (long long)fl, (long long)sc, (long long)gold, (long long)st,
           diff >= 0 ? " OPSDIFF" : (nops != hn ? " NOPS!=HOST" : ""));
}

int main() {
    printf("wv_tb_walk local test, NW_PE=%d (%d tiles)\n", NW_PE, WV_TILES);
    run("single band", 200, 150, false);
    run("tile boundary", 3 * 128 + 1, 300, true);
    run("band edge", WV_PE, 500, true);
    run("band edge+1", WV_PE + 1, 500, true);
    run("3 bands g>BWIN", 3 * WV_PE + 17, 90, false);
    run("2 bands n>NMIN", 2 * WV_PE - 5, WV_NMIN + 33, true);
    run("tall thin", 4 * WV_PE, 7, false);
    run("wide flat", 9, 3000, false);
    run("n=1", 2 * WV_PE, 1, false);
    run("m=1", 1, 700, false);
    run("identical", 2 * WV_PE, 2 * WV_PE, true);
    run("long similar", 5 * WV_PE + 3, 4000, true);
    run("long random", 3 * WV_PE, 3500, false);
    printf("%d cases, %d failures\n%s\n", ran, bad, bad ? "*** WALK FAILED ***" : "WALK PASSED");
    return bad != 0;
}
