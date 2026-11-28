/* dut_driver.cpp -- runs every repository model of the design on a file of test cases and
 * prints their raw outputs. It JUDGES NOTHING: check_cases.py compares these outputs with
 * independent references (pure Python DP, Biopython) and re-scores the traceback ops itself.
 *
 * Models run per case (all compiled from the repo sources, NW_PE chosen at build time):
 *   gold   nw_align_ref        C row-major DP, score domain        (linear/src/nw_ref.c)
 *   wave   nw_wave_scalar      cycle model of the PE array, banded (linear/src/nw_wave.c)
 *   dirs   nw_wave_dirs + nw_wave_traceback   per-cell directions, path re-scored in C
 *   host   nw_host_traceback   host walker over the S1c base-3 layout (host/)
 *   card   wv_tb_walk          the on-card walker (src/wv_tb_walk.h), same source the kernel includes
 * The base-3 direction layout is built exactly as in test/tb_walk_local.cpp.
 *
 * Case file, one per line:  id class m n query reference      (query/reference over ACGTN; "-" = empty)
 * Classes starting with N or Z are model-only (N has no 2-bit code in the walker; Z has m or n = 0).
 *
 * Output, per case:
 *   CASE id class m n gold wave dirs_path host_score host_nops card_status card_nops card_score card_fills ops_first_diff
 *   OPS  <card ops, one digit per step: 0 DIAG, 1 UP, 2 LEFT, step 0 = move into cell (m,n)>
 *   DIRS <row-major direction digits, only when m*n <= DIRS_MAX>
 * Fields of models that were not run are -1000000000000 (NOTRUN).
 *
 *   usage: dut_driver cases.txt          (build: run_all.sh)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
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

#define DIRS_MAX 60000
#define NOTRUN (-1000000000000LL)   /* "this model was not run" -- must not collide with a real score */
/* above this many cells the per-cell direction matrix is not built (33 GB at 181 kbp): score-only */
#define WALK_MAX 3000000000LL

struct port_ctx { std::vector<std::vector<uint8_t>> *ports; };
static int fetch(void *ctx, int port, int64_t first, int64_t count, uint8_t *dst) {
    std::vector<uint8_t> &p = (*((port_ctx *)ctx)->ports)[port];
    memcpy(dst, p.data() + first * 64, (size_t)count * 64);
    return 0;
}

static uint8_t code(char c) {
    switch (c) { case 'A': return 0; case 'C': return 1; case 'G': return 2; case 'T': return 3; default: return 4; }
}

static void run_case(const char *id, const char *cls, const std::string &qs, const std::string &rs) {
    const int64_t m = (int64_t)qs.size(), n = (int64_t)rs.size();
    std::vector<uint8_t> q(m), r(n);
    for (int64_t i = 0; i < m; ++i) q[i] = code(qs[i]);
    for (int64_t j = 0; j < n; ++j) r[j] = code(rs[j]);
    const bool model_only = (cls[0] == 'N' || cls[0] == 'Z');

    const int64_t gold = nw_align_ref(q.data(), (int32_t)m, r.data(), (int32_t)n);
    std::vector<int8_t> bnd((size_t)(n > 0 ? n : 1) + 8);
    const int64_t wave = nw_wave_scalar(q.data(), m, r.data(), n, bnd.data());

    int64_t dpath = NOTRUN, hscore = NOTRUN, hn = NOTRUN, cst = NOTRUN, cnops = NOTRUN, csc = NOTRUN, cfill = NOTRUN, diff = -1;
    std::string ops_s, dirs_s;
    if (m > 0 && n > 0 && m * n <= WALK_MAX) {
        std::vector<uint8_t> dirs((size_t)m * n);
        std::vector<int8_t> bnd2((size_t)n + 8);
        nw_wave_dirs(q.data(), m, r.data(), n, bnd2.data(), dirs.data());
        int64_t nk = 0;
        dpath = nw_wave_traceback(q.data(), m, r.data(), n, dirs.data(), &nk);
        if (m * n <= DIRS_MAX) { dirs_s.reserve((size_t)m * n); for (auto d : dirs) dirs_s.push_back((char)('0' + d)); }

        if (!model_only) {
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
            nwh_geom g = { m, n, WV_PE, WV_TILE_PE, 2, -1, -2 };
            port_ctx pc = { &ports };
            std::vector<uint8_t> hops((size_t)(m + n + 8));
            hn = nw_host_traceback(&g, q.data(), r.data(), fetch, &pc, hops.data(), (int64_t)hops.size(), &hscore, 0, 0);

            const int64_t DESC = 0, OPS = 64, opsw = (m + n + 31) / 32 + 1;
            const int64_t dir0 = OPS + opsw + 8;
            const int64_t total = dir0 + (int64_t)WV_NCH * nbeats * 8;
            std::vector<uint64_t> arena((size_t)total, 0);
            for (int k = 0; k < WV_NCH; ++k) {
                arena[DESC + k] = (uint64_t)(dir0 + (int64_t)k * nbeats * 8);
                memcpy(&arena[(size_t)(dir0 + (int64_t)k * nbeats * 8)], ports[k].data(), (size_t)nbeats * 64);
            }
            arena[DESC + WV_TBD_EN] = 1; arena[DESC + WV_TBD_OPS] = OPS; arena[DESC + WV_TBD_CAP] = m + n + 8;
            std::vector<ap_uint<256>> ubq4((m + 127) / 128 + 1, 0);
            for (int64_t t = 0; t < m; ++t) ubq4[t >> 7].range(2 * (t & 127) + 1, 2 * (t & 127)) = q[t];
            std::vector<uint64_t> ubr((n + 31) / 32 + 1, 0);
            for (int64_t t = 0; t < n; ++t) ubr[t >> 5] |= (uint64_t)r[t] << (2 * (t & 31));
            wv_tb_walk(arena.data(), DESC, ubq4.data(), ubr.data(), m, n, (uint32_t)S, (uint32_t)nbeats);
            cnops = (int64_t)arena[DESC + WV_TBD_NOPS];
            csc   = (int64_t)arena[DESC + WV_TBD_SCORE];
            cst   = (int64_t)arena[DESC + WV_TBD_STATUS];
            cfill = (int64_t)arena[DESC + WV_TBD_FILLS];
            if (cnops == hn)
                for (int64_t s = 0; s < cnops; ++s)
                    if (((arena[OPS + s / 32] >> (2 * (s % 32))) & 3) != hops[s]) { diff = s; break; }
            ops_s.reserve((size_t)cnops);
            for (int64_t s = 0; s < cnops; ++s) ops_s.push_back((char)('0' + ((arena[OPS + s / 32] >> (2 * (s % 32))) & 3)));
        }
    }
    printf("CASE %s %s %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld\n", id, cls,
           (long long)m, (long long)n, (long long)gold, (long long)wave, (long long)dpath, (long long)hscore,
           (long long)hn, (long long)cst, (long long)cnops, (long long)csc, (long long)cfill, (long long)diff);
    if (!ops_s.empty()) printf("OPS %s\n", ops_s.c_str());
    if (!dirs_s.empty()) printf("DIRS %s\n", dirs_s.c_str());
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s cases.txt\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "r");
    if (!f) { perror(argv[1]); return 2; }
    printf("DRIVER NW_PE=%d tiles=%d NMIN=%d BWIN=%d\n", NW_PE, WV_TILES, WV_NMIN, WV_BWIN);
    static char *line = nullptr; size_t cap = 0; ssize_t len;
    while ((len = getline(&line, &cap, f)) > 0) {
        char id[64], cls[64]; long long m, n; int off = 0;
        if (line[0] == '#' || sscanf(line, "%63s %63s %lld %lld %n", id, cls, &m, &n, &off) < 4) continue;
        std::string q, r; const char *p = line + off;
        const char *sp = strchr(p, ' ');
        if (!sp) continue;
        q.assign(p, sp - p);
        const char *e = sp + 1; size_t rl = strcspn(e, "\r\n");
        r.assign(e, rl);
        if (q == "-") q.clear();
        if (r == "-") r.clear();
        if ((long long)q.size() != m || (long long)r.size() != n) { printf("BADCASE %s\n", id); continue; }
        run_case(id, cls, q, r);
    }
    printf("END\n");
    return 0;
}
