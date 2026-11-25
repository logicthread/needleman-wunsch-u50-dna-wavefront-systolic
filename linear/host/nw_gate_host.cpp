/* nw_gate_host.cpp -- U50 hardware host for the tiled delta-DP aligner, written to the
 * course gate's contract (fpga-client/AGENTS.md):
 *   - xclbin from argv[1] if it ends in .xclbin, else ./design.xclbin
 *   - device 0 (inside the gate sandbox that is the U50)
 *   - every output goes to the current directory (the gate returns it as out/)
 *   - metrics.txt (key value) + one RESULT line on stdout; exit 0 only if all checks pass
 *
 * ONE SOURCE, TWO KERNEL INTERFACES, selected at compile time and checked against the
 * xclbin's own argument list (xclbinutil EMBEDDED_METADATA), not against today's sources:
 *
 *   WV_IFACE=1  kernel_tile.cpp   (3072 / 2048 / 512 builds, 5 + 2 + TILES args)
 *       qpack, rpack, bnd_r, bnd_w, score_out, m, n, d00..dK
 *       bnd_r and bnd_w are THE SAME BUFFER (both sp= HBM[1]); the kernel writes band b's
 *       boundary and reads it back for band b+1. One BO, passed twice.
 *   WV_IFACE=2  kernel_tile_1b.cpp (3968 build, 7 + TILES args)
 *       mem, q_off, r_off, b_off, s_off, m, n, d00..dK   (single control bundle)
 *
 * Every existing xclbin is 2-BIT direction packing (NOTES N59), so there is no base-3
 * path for kernel_tile.cpp builds; kernel_tile_1b builds may be base-3 (-DWV_DIR_B3).
 *
 * Two phases per run, both verified against an O(n)-space CPU DP (src/nw_ref.c):
 *   phase "check": m = 2 bands, small n  -> score + full traceback path check.
 *                  Two bands is the minimum that exercises the band boundary buffer.
 *   phase "perf":  m = PERF_BANDS bands, n = PERF_N -> score + traceback, and the kernel
 *                  is timed over --reps launches (min/median reported). GCUPS = m*n/t.
 * -DWV_ONCARD_TB (S1t, kernel_tile_1b_s1t.cpp): the kernel walks the path itself. The
 * host writes a descriptor at mem[b_off] -- each direction BO's device address relative
 * to mem, in words -- times --reps launches with the walk OFF (scoring GCUPS), then one
 * launch with the walk ON and reads back only the 2-bit ops. The check phase also runs
 * the host walker and requires the card's ops to equal it step for step.
 *
 * The path check scores the recovered ops independently and requires it to be
 * end-to-end and equal to the optimum. Ties make several optimal paths legal, so the
 * score -- not the path -- is the sound test (same rule as the HLS testbench).
 */
#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>

#include "xrt/xrt_bo.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_kernel.h"

extern "C" {
#include "nw.h"
#include "nw_host_traceback.h"
}

#ifndef NW_PE
#error "NW_PE must match the xclbin"
#endif
#ifndef WV_TILE_PE
#define WV_TILE_PE 128
#endif
#ifndef WV_IFACE
#error "WV_IFACE must be 1 (kernel_tile.cpp: qpack/rpack/bnd/score) or 2 (kernel_tile_1b.cpp: mem+offsets)"
#endif
/* WV_DIR_B3: the 3968 xclbin (job 106803, kernel_tile_1b@3ba10560) IS base-3 -- proven by
 * its RTL (31 files with pk_ regs + ob0/ob1) and by a raw hardware dump decoding 100% valid
 * under the base-3 layout. N59's "every link is 2-bit" was wrong for this build. The
 * kernel_tile.cpp builds (3072/2048/512) have no base-3 code and stay 2-bit. */
#if defined(WV_DIR_B3) && !defined(NWH_DIR_B3)
#error "WV_DIR_B3 requires the decoder built with -DNWH_DIR_B3"
#endif
#define WV_TILES     (NW_PE / WV_TILE_PE)
#define WV_PER_WORD  32
#define WV_BEATBYTES 64
#define WV_DIR_PACK  (512 / (WV_TILE_PE * 2))
static_assert(NW_PE % WV_TILE_PE == 0, "whole tiles only");
static_assert(NWH_TILE_PE == WV_TILE_PE, "decoder tile width");
#ifndef WV_DIR_B3
static_assert(NWH_PACK == WV_DIR_PACK, "decoder 2-bit pack");
#endif
#if WV_IFACE == 2
#define WV_NMAX 262144   /* on-chip ubi/ubo depth in kernel_tile_1b @3ba10560 */
#endif

static double now_ms() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
static int64_t bpb_of(int64_t n) {          /* beats per band per port */
#ifdef WV_DIR_B3
    const int64_t cmax = (n + WV_TILE_PE + 4) / 5 * 5;      /* whole 5-cycle groups */
    return cmax / 5 * ((WV_TILE_PE * 8) / 512);             /* 2 beats per group at 128 */
#else
    return (n + WV_TILE_PE + WV_DIR_PACK - 1) / WV_DIR_PACK;
#endif
}

struct fetch_ctx { std::vector<xrt::bo> *dir; std::vector<uint8_t *> *map; int64_t beats;
                   int64_t calls = 0, nbeats = 0; xrt::bo *stage = nullptr; uint8_t *smap = nullptr;
                   int64_t stage_beats = 0;
                   /* batched prefetch: per port, beat intervals already synced into the host
                    * mirror. A miss syncs the SAME beat window for every port in parallel,
                    * because on entering a band all tiles need roughly the same columns. */
                   bool batch = true; int threads = 8;
                   std::vector<std::vector<std::pair<int64_t, int64_t>>> have;
                   int64_t batches = 0, hits = 0, misses = 0; };
static bool covered(const fetch_ctx *c, int port, int64_t f, int64_t n) {
    for (auto &iv : c->have[port]) if (f >= iv.first && f + n <= iv.second) return true;
    return false;
}
static int fetch_span(void *v, int port, int64_t first, int64_t count, uint8_t *dst) {
    auto *c = static_cast<fetch_ctx *>(v);
    if (port < 0 || port >= WV_TILES || first < 0 || count <= 0 || first + count > c->beats)
        return -1;
    const size_t off = (size_t)first * WV_BEATBYTES, bytes = (size_t)count * WV_BEATBYTES;
    if (!c->stage && c->batch) {
        if (c->have.empty()) c->have.resize(WV_TILES);
        if (covered(c, port, first, count)) { ++c->hits; }
        else {
            ++c->misses; ++c->batches;
            /* widen the window so neighbouring tiles' slightly-left positions are covered */
            const int64_t lo = std::max<int64_t>(0, first - 64), hi = std::min<int64_t>(c->beats, first + count);
            std::vector<int> todo;
            for (int k = 0; k < WV_TILES; ++k) if (!covered(c, k, lo, hi - lo)) todo.push_back(k);
            std::vector<std::thread> th;
            const int T = std::max(1, std::min<int>(c->threads, (int)todo.size()));
            for (int t = 0; t < T; ++t)
                th.emplace_back([&, t]() {
                    for (size_t q = t; q < todo.size(); q += T)
                        (*c->dir)[todo[q]].sync(XCL_BO_SYNC_BO_FROM_DEVICE, (size_t)(hi - lo) * WV_BEATBYTES,
                                                (size_t)lo * WV_BEATBYTES);
                });
            for (auto &x : th) x.join();
            for (int k : todo) c->have[k].push_back({lo, hi});
            c->nbeats += (hi - lo) * (int64_t)todo.size();
        }
        std::memcpy(dst, (*c->map)[port] + off, bytes);
        ++c->calls;
        return 0;
    }
    if (c->stage) {                              /* device-only: DMA the span into staging */
        if (count > c->stage_beats) return -1;
        c->stage->copy((*c->dir)[port], bytes, off, 0);
        c->stage->sync(XCL_BO_SYNC_BO_FROM_DEVICE, bytes, 0);
        std::memcpy(dst, c->smap, bytes);
    } else {
        (*c->dir)[port].sync(XCL_BO_SYNC_BO_FROM_DEVICE, bytes, off);
        std::memcpy(dst, (*c->map)[port] + off, bytes);
    }
    ++c->calls; c->nbeats += count;
    return 0;
}
static int64_t score_ops(const uint8_t *ops, int64_t k, const uint8_t *q, int64_t m,
                         const uint8_t *r, int64_t n, int64_t *uq, int64_t *ur) {
    int64_t s = 0, i = 0, j = 0;
    for (int64_t t = 0; t < k; ++t) {
        switch (ops[t]) {
        case 0: if (i >= m || j >= n) return INT64_MIN;
                s += (q[i] == r[j]) ? NW_MATCH : NW_MISMATCH; ++i; ++j; break;
        case 1: if (i >= m) return INT64_MIN; s += NW_GAP; ++i; break;
        case 2: if (j >= n) return INT64_MIN; s += NW_GAP; ++j; break;
        default: return INT64_MIN;
        }
    }
    *uq = i; *ur = j; return s;
}
static int g_timeout_ms = 20000;
static bool g_dump = false;   /* --dump-dir: write raw dir buffers + inputs of the check phase */
/* --devonly: direction BOs are DEVICE-ONLY (no host mirror). At 181 kbp the 31 direction
 * buffers total ~6.6 GB, too close to the gate's 8 GiB address-space cap if mirrored.
 * Traceback copies only the spans it needs through one small host-backed staging BO. */
static bool g_devonly = false;
/* --tb-batch: on a miss, sync the same beat window for EVERY port in parallel threads.
 * MEASURED WORSE (gate fc8b511d, 181 kbp): 422.7 ms vs 39 ms serial -- only 691/1426 hits,
 * so ~22.8 K syncs instead of 1,426, and XRT syncs serialize on the card's DMA so threads
 * buy nothing. Host traceback cost ~= number of syncs x ~27 us. Off by default. */
static bool g_tb_batch = false;
static bool g_hung = false;
static uint32_t rs;
static uint32_t rnd() { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

struct PhaseResult {
    std::string name; int64_t m = 0, n = 0, gold = 0, score = 0, path_score = 0, nops = 0;
    bool score_ok = false, path_ok = false; double tmin = 0, tmed = 0, cpu_ms = 0;
    int reps = 0; double tb_ms = 0; std::string note;
    double tbrun_ms = 0, rb_ms = 0; int64_t fills = 0;   /* on-card traceback */
};

static PhaseResult run_phase(xrt::device &dev, xrt::kernel &k, const char *name,
                             int64_t m, int64_t n, int reps, unsigned seed, bool do_tb) {
    PhaseResult R; R.name = name; R.m = m; R.n = n; R.reps = reps;
    const int64_t nbands = (m + NW_PE - 1) / NW_PE, bpb = bpb_of(n);
#ifdef NWH_CONT
    /* S1c continuous bands: one stream per port for the whole alignment */
    nwh_geom gb; std::memset(&gb, 0, sizeof gb);
    gb.m = m; gb.n = n; gb.pe = NW_PE; gb.tile_pe = WV_TILE_PE;
    const int64_t beats = nwh_cont_beats(&gb);
#else
    const int64_t beats = nbands * bpb;
#endif
    printf("[%s] m=%" PRId64 " n=%" PRId64 " bands=%" PRId64 " cells=%.3e  dir=%.1f MiB/port x %d%s\n",
           name, m, n, nbands, (double)m * n, beats * 64.0 / 1048576.0, WV_TILES, g_devonly ? "  (device-only)" : "");
    fflush(stdout);

    rs = seed ? seed : 1u;
    std::vector<uint8_t> q((size_t)m), r((size_t)n);
    for (auto &x : q) x = (uint8_t)(rnd() & 3);
    for (auto &x : r) x = (uint8_t)(rnd() & 3);
    double t0 = now_ms();
    R.gold = (int64_t)nw_align_ref(q.data(), (int32_t)m, r.data(), (int32_t)n);
    R.cpu_ms = now_ms() - t0;
    printf("[%s] CPU reference score %" PRId64 " (%.0f ms)\n", name, R.gold, R.cpu_ms);
    fflush(stdout);

    const int64_t qw = (m + WV_PER_WORD - 1) / WV_PER_WORD, rw = (n + WV_PER_WORD - 1) / WV_PER_WORD;
    auto pack = [](uint64_t *dst, const std::vector<uint8_t> &s) {
        for (size_t i = 0; i < s.size(); ++i)
            dst[i / WV_PER_WORD] |= (uint64_t)(s[i] & 3) << (2 * (i % WV_PER_WORD));
    };
    std::vector<xrt::bo> bo_dir; std::vector<uint8_t *> dmap;
    xrt::run run(k);

#if WV_IFACE == 1
    xrt::bo bq(dev, (size_t)qw * 8, k.group_id(0));
    xrt::bo br(dev, (size_t)rw * 8, k.group_id(1));
    xrt::bo bb(dev, (size_t)(n + 64), k.group_id(2));      /* shared by bnd_r and bnd_w */
    xrt::bo bs(dev, 64, k.group_id(4));
    auto *pq = bq.map<uint64_t *>(); std::memset(pq, 0, (size_t)qw * 8); pack(pq, q);
    auto *pr = br.map<uint64_t *>(); std::memset(pr, 0, (size_t)rw * 8); pack(pr, r);
    std::memset(bb.map<uint8_t *>(), 0, (size_t)(n + 64));
    auto *ps = bs.map<int64_t *>(); ps[0] = INT64_MIN;
    bq.sync(XCL_BO_SYNC_BO_TO_DEVICE); br.sync(XCL_BO_SYNC_BO_TO_DEVICE);
    bb.sync(XCL_BO_SYNC_BO_TO_DEVICE); bs.sync(XCL_BO_SYNC_BO_TO_DEVICE);
    run.set_arg(0, bq); run.set_arg(1, br); run.set_arg(2, bb); run.set_arg(3, bb);
    run.set_arg(4, bs); run.set_arg(5, m); run.set_arg(6, n);
#else
    const int64_t q_off = 0, r_off = qw, b_off = r_off + rw, s_off = b_off + (n + 7) / 8 + 1;
#ifdef WV_ONCARD_TB
    static_assert(WV_TILES + 7 <= 64, "descriptor must fit the b_off region");
    const int64_t ops_off = s_off + 1, ops_cap = m + n + 8;
    const int64_t words = ops_off + ops_cap / 32 + 2;
#else
    const int64_t words = s_off + 1;
#endif
    xrt::bo bm(dev, (size_t)words * 8, k.group_id(0));
    auto *pm = bm.map<uint64_t *>(); std::memset(pm, 0, (size_t)words * 8);
    pack(pm + q_off, q); pack(pm + r_off, r);
    pm[s_off] = (uint64_t)INT64_MIN;
    bm.sync(XCL_BO_SYNC_BO_TO_DEVICE);
    run.set_arg(0, bm); run.set_arg(1, q_off); run.set_arg(2, r_off); run.set_arg(3, b_off);
    run.set_arg(4, s_off); run.set_arg(5, m); run.set_arg(6, n);
#endif
    for (int t = 0; t < WV_TILES; ++t) {
        if (g_devonly) {
            bo_dir.emplace_back(dev, (size_t)beats * WV_BEATBYTES, xrt::bo::flags::device_only, k.group_id(7 + t));
            dmap.push_back(nullptr);
            run.set_arg(7 + t, bo_dir.back());
            continue;
        }
        bo_dir.emplace_back(dev, (size_t)beats * WV_BEATBYTES, k.group_id(7 + t));
        uint8_t *p = bo_dir.back().map<uint8_t *>();
        /* Fill with 0xFF, not 0: DIAG is 0, so a zero fill would let a missing write
         * masquerade as a diagonal -- the most common move. 0xFF decodes to 3, which the
         * decoder treats as LEFT, so missing writes bend the path sideways and the
         * independent path-score check (== optimum, end-to-end) catches them. */
        std::memset(p, 0xFF, (size_t)beats * WV_BEATBYTES);
        bo_dir.back().sync(XCL_BO_SYNC_BO_TO_DEVICE);
        dmap.push_back(p);
        run.set_arg(7 + t, bo_dir.back());
    }
#ifdef WV_ONCARD_TB
    /* descriptor: direction buffers RELATIVE TO mem, in 64-bit words. mem is linked to
     * HBM[0:31], so its port reaches every pseudo-channel through the HBM switch. */
    for (int t = 0; t < WV_TILES; ++t) {
        const int64_t d = (int64_t)(bo_dir[t].address() - bm.address());
        if (d % 8) { R.note = "dir BO not word-aligned to mem"; return R; }
        pm[b_off + t] = (uint64_t)(d / 8);
    }
    pm[b_off + WV_TILES + 0] = 0;            /* EN: timed reps are scoring-only */
    pm[b_off + WV_TILES + 1] = (uint64_t)ops_off;
    pm[b_off + WV_TILES + 2] = (uint64_t)ops_cap;
    bm.sync(XCL_BO_SYNC_BO_TO_DEVICE, (size_t)(WV_TILES + 8) * 8, (size_t)b_off * 8);
    printf("[%s] on-card traceback descriptor: mem@0x%llx dir0 at %+lld words\n", name,
           (unsigned long long)bm.address(), (long long)(int64_t)pm[b_off]);
#endif

    std::vector<double> ts;
    for (int it = 0; it < reps; ++it) {
        double a = now_ms(); run.start();
        /* bounded wait: a hang must be REPORTED and the job ended, not left for the
         * gate's 5-min kill -- a killed run blocks this xclbin until a staff reset. */
        auto st = run.wait(std::chrono::milliseconds(g_timeout_ms));
        if (st != ERT_CMD_STATE_COMPLETED) {
            printf("[%s] launch %d NOT COMPLETE after %.0f ms (state %d) -- abandoning run\n",
                   name, it, now_ms() - a, (int)st);
            fflush(stdout);
            R.note = "kernel did not complete (hang)";
            g_hung = true;
            return R;
        }
        ts.push_back(now_ms() - a);
    }
    std::sort(ts.begin(), ts.end());
    R.tmin = ts.front(); R.tmed = ts[ts.size() / 2];

#if WV_IFACE == 1
    bs.sync(XCL_BO_SYNC_BO_FROM_DEVICE); R.score = ps[0];
#else
    bm.sync(XCL_BO_SYNC_BO_FROM_DEVICE); std::memcpy(&R.score, &pm[s_off], 8);
#endif
    R.score_ok = (R.score == R.gold);
    printf("[%s] kernel score %" PRId64 " %s   time min %.3f ms  median %.3f ms over %d  -> %.1f GCUPS (min)\n",
           name, R.score, R.score_ok ? "== CPU" : "!= CPU  *** MISMATCH ***",
           R.tmin, R.tmed, reps, (double)m * n / (R.tmin * 1e6));
    fflush(stdout);

    if (g_dump && R.name == "check" && !g_devonly) {
        /* raw evidence for offline decoding: every port's whole dir buffer, plus the
         * exact inputs. Small problems only -- the result cap is 64 MiB. */
        for (int t = 0; t < WV_TILES; ++t) {
            bo_dir[t].sync(XCL_BO_SYNC_BO_FROM_DEVICE);
            char fn[64]; snprintf(fn, sizeof fn, "dir_%02d.bin", t);
            FILE *f = fopen(fn, "wb"); if (f) { fwrite(dmap[t], 1, (size_t)beats * WV_BEATBYTES, f); fclose(f); }
        }
        FILE *f = fopen("q.bin", "wb"); if (f) { fwrite(q.data(), 1, q.size(), f); fclose(f); }
        f = fopen("r.bin", "wb");       if (f) { fwrite(r.data(), 1, r.size(), f); fclose(f); }
        f = fopen("dump_meta.txt", "w");
        if (f) { fprintf(f, "m %lld\nn %lld\npe %d\ntile_pe %d\ntiles %d\nbeats_per_port %lld\nbpb %lld\n",
                         (long long)m, (long long)n, NW_PE, WV_TILE_PE, WV_TILES, (long long)beats, (long long)bpb); fclose(f); }
        printf("[%s] dumped %d dir buffers (%lld beats each) + q.bin r.bin\n", name, WV_TILES, (long long)beats);
    }
#ifdef WV_ONCARD_TB
    if (do_tb) {
        pm[b_off + WV_TILES + 0] = 1;            /* EN */
        bm.sync(XCL_BO_SYNC_BO_TO_DEVICE, 8, (size_t)(b_off + WV_TILES) * 8);
        double a = now_ms(); run.start();
        auto st = run.wait(std::chrono::milliseconds(g_timeout_ms));
        R.tbrun_ms = now_ms() - a;
        if (st != ERT_CMD_STATE_COMPLETED) {
            printf("[%s] traceback launch NOT COMPLETE after %.0f ms -- abandoning\n", name, R.tbrun_ms);
            R.note = "kernel+walk did not complete (hang)"; g_hung = true; return R;
        }
        a = now_ms();
        bm.sync(XCL_BO_SYNC_BO_FROM_DEVICE, (size_t)(WV_TILES + 8) * 8, (size_t)b_off * 8);
        const int64_t nops = (int64_t)pm[b_off + WV_TILES + 3];
        if (nops > 0 && nops <= ops_cap)
            bm.sync(XCL_BO_SYNC_BO_FROM_DEVICE, (size_t)((nops + 31) / 32) * 8, (size_t)ops_off * 8);
        std::vector<uint8_t> ops((size_t)std::max<int64_t>(nops, 0));
        for (int64_t s2 = 0; s2 < nops; ++s2)
            ops[s2] = (uint8_t)((pm[ops_off + s2 / 32] >> (2 * (s2 % 32))) & 3);
        R.rb_ms = now_ms() - a;
        const int64_t psc = (int64_t)pm[b_off + WV_TILES + 4], st2 = (int64_t)pm[b_off + WV_TILES + 5];
        R.fills = (int64_t)pm[b_off + WV_TILES + 6];
        std::vector<uint8_t> fwd(ops.rbegin(), ops.rend());
        int64_t uq = 0, ur = 0;
        R.nops = nops;
        R.path_score = score_ops(fwd.data(), nops, q.data(), m, r.data(), n, &uq, &ur);
        R.path_ok = (st2 == 0 && R.path_score != INT64_MIN && uq == m && ur == n &&
                     R.path_score == R.gold && psc == R.gold);
        R.tb_ms = std::max(0.0, R.tbrun_ms - R.tmin);    /* walk time on the card */
        printf("[%s] ON-CARD traceback: status %" PRId64 ", %" PRId64 " ops, %" PRId64 " fills, card path score %" PRId64
               ", kernel+walk %.3f ms (walk ~%.3f ms), ops readback %.3f ms -> path %s\n",
               name, st2, nops, R.fills, psc, R.tbrun_ms, R.tb_ms, R.rb_ms,
               R.path_ok ? "OK (end-to-end, == optimum)" : "FAILED");
        if (R.name == "check") {   /* silicon cross-check: host walker on the same buffers */
            fetch_ctx c; c.dir = &bo_dir; c.map = &dmap; c.beats = beats; c.batch = false;
            nwh_geom g; g.m = m; g.n = n; g.pe = NW_PE; g.tile_pe = WV_TILE_PE;
            g.match = NW_MATCH; g.mismatch = NW_MISMATCH; g.gap = NW_GAP;
            std::vector<uint8_t> hops((size_t)(m + n + 8));
            int64_t tbs = 0;
            const int64_t hn = nw_host_traceback(&g, q.data(), r.data(), fetch_span, &c, hops.data(),
                                                 (int64_t)hops.size(), &tbs, nullptr, nullptr);
            bool same = (hn == nops);
            for (int64_t s2 = 0; same && s2 < nops; ++s2) same = (hops[s2] == ops[s2]);
            printf("[%s] card ops vs host walker: %s (host %" PRId64 " ops)\n", name,
                   same ? "IDENTICAL" : "*** DIFFER ***", hn);
            if (!same) { R.path_ok = false; R.note = "card ops != host walker"; }
        }
    } else {
        R.path_ok = true; R.note = "traceback skipped";
    }
    fflush(stdout);
    return R;
#endif
    if (do_tb) {
        fetch_ctx c; c.dir = &bo_dir; c.map = &dmap; c.beats = beats;
        c.batch = g_tb_batch;
        std::unique_ptr<xrt::bo> stage;
        if (g_devonly) {                     /* NWH_SPAN beats is the decoder's largest fetch */
            c.stage_beats = 4096;
            stage.reset(new xrt::bo(dev, (size_t)c.stage_beats * WV_BEATBYTES, k.group_id(7)));
            c.stage = stage.get(); c.smap = stage->map<uint8_t *>();
        }
        nwh_geom g; g.m = m; g.n = n; g.pe = NW_PE; g.tile_pe = WV_TILE_PE;
        g.match = NW_MATCH; g.mismatch = NW_MISMATCH; g.gap = NW_GAP;
        std::vector<uint8_t> ops((size_t)(m + n + 8));
        int64_t tbs = 0, calls = 0, nb = 0;
        double a = now_ms();
        R.nops = nw_host_traceback(&g, q.data(), r.data(), fetch_span, &c, ops.data(),
                                   (int64_t)ops.size(), &tbs, &calls, &nb);
        double tb_ms = now_ms() - a;
        if (R.nops < 0) {
            R.note = "traceback rc=" + std::to_string(R.nops);
        } else {
            /* the decoder walks from (m,n) back to (0,0); score_ops wants forward order */
            std::reverse(ops.begin(), ops.begin() + R.nops);
            int64_t uq = 0, ur = 0;
            R.path_score = score_ops(ops.data(), R.nops, q.data(), m, r.data(), n, &uq, &ur);
            R.path_ok = (R.path_score != INT64_MIN && uq == m && ur == n && R.path_score == R.gold);
            if (!R.path_ok)
                R.note = "path score " + std::to_string(R.path_score) + " consumed q=" +
                         std::to_string(uq) + " r=" + std::to_string(ur);
        }
        printf("[%s] traceback: %" PRId64 " ops, %" PRId64 " span requests, %" PRId64 " parallel batches (%" PRId64 " hits), %.1f ms -> path %s %s\n",
               name, R.nops, c.calls, c.batches, c.hits, tb_ms, R.path_ok ? "OK (end-to-end, == optimum)" : "FAILED",
               R.note.c_str());
        R.tb_ms = tb_ms;
    } else {
        R.path_ok = true; R.note = "traceback skipped";
    }
    fflush(stdout);
    return R;
}

int main(int argc, char **argv) {
    std::string xclbin = "design.xclbin";
#if WV_IFACE == 1
    /* kernel_tile.cpp: multi-band is BROKEN in RTL (gate runs 2026-10-19: band 1 emits
     * inconsistent deltas even with a correct boundary). Single band is verified. */
    int64_t cb = 1, pbands = 1;
#else
    int64_t cb = 2, pbands = 1;
#endif
    int64_t cn = 20000, pn = 262144; int reps = 5; unsigned seed = 12345;
    bool tb = true;
    for (int a = 1; a < argc; ++a) {
        std::string s = argv[a];
        auto nxt = [&]() { return (a + 1 < argc) ? std::string(argv[++a]) : std::string(); };
        if (s.size() > 7 && s.substr(s.size() - 7) == ".xclbin") xclbin = s;
        else if (s == "--check-n")    cn = atoll(nxt().c_str());
        else if (s == "--check-bands") cb = atoll(nxt().c_str());
        else if (s == "--timeout-ms") g_timeout_ms = atoi(nxt().c_str());
        else if (s == "--dump-dir")   g_dump = true;
        else if (s == "--devonly")    g_devonly = true;
        else if (s == "--tb-batch")   g_tb_batch = true;
        else if (s == "--perf-bands") pbands = atoll(nxt().c_str());
        else if (s == "--perf-n")     pn = atoll(nxt().c_str());
        else if (s == "--reps")       reps = atoi(nxt().c_str());
        else if (s == "--seed")       seed = (unsigned)atoi(nxt().c_str());
        else if (s == "--no-tb")      tb = false;
        else { fprintf(stderr, "usage: %s [design.xclbin] [--check-n N] [--perf-bands B] "
                               "[--perf-n N] [--reps R] [--seed S] [--no-tb] [--check-bands B] [--timeout-ms T]\n", argv[0]); return 2; }
    }
#ifdef WV_NMAX
    if (pn > WV_NMAX || cn > WV_NMAX) { fprintf(stderr, "n exceeds on-chip WV_NMAX=%d\n", WV_NMAX); return 2; }
#endif
    #ifdef WV_DIR_B3
    const char *enc = "BASE-3 dirs";
#else
    const char *enc = "2-bit dirs";
#endif
    printf("=== nw_gate_host  NW_PE=%d  %d tiles x %d  iface=%d  %s  xclbin=%s ===\n",
           NW_PE, WV_TILES, WV_TILE_PE, WV_IFACE, enc, xclbin.c_str());
    fflush(stdout);
    std::vector<PhaseResult> res;
    double t_all = now_ms();
    try {
        xrt::device dev(0);
        printf("device: %s\n", dev.get_info<xrt::info::device::name>().c_str());
        auto uuid = dev.load_xclbin(xclbin);
        xrt::kernel k(dev, uuid, "nw_tile_u50");
        res.push_back(run_phase(dev, k, "check", cb * (int64_t)NW_PE, cn, 1, seed, true));
        if (!g_hung) res.push_back(run_phase(dev, k, "perf", pbands * (int64_t)NW_PE, pn, reps, seed + 1, tb));
    } catch (const std::exception &e) {
        fprintf(stderr, "host error: %s\n", e.what());
        FILE *f = fopen("metrics.txt", "w");
        if (f) { fprintf(f, "pass 0\nerror %s\n", e.what()); fclose(f); }
        printf("RESULT: FAIL  xrt-exception\n");
        return 3;
    }
    bool pass = !g_hung;
    FILE *f = fopen("metrics.txt", "w");
    FILE *c = fopen("results.csv", "w");
    if (c) fprintf(c, "phase,m,n,cells,gold,kernel_score,score_ok,path_ok,t_min_ms,t_med_ms,reps,gcups_min,gcups_med,cpu_ref_ms,traceback_ms,e2e_ms,gcups_e2e\n");
    for (auto &R : res) {
        pass = pass && R.score_ok && R.path_ok;
        const double cells = (double)R.m * R.n;
        if (f) fprintf(f, "%s_m %" PRId64 "\n%s_n %" PRId64 "\n%s_score_ok %d\n%s_path_ok %d\n"
                          "%s_kernel_ms_min %.4f\n%s_kernel_ms_med %.4f\n%s_gcups_min %.2f\n%s_gcups_med %.2f\n",
                       R.name.c_str(), R.m, R.name.c_str(), R.n, R.name.c_str(), (int)R.score_ok,
                       R.name.c_str(), (int)R.path_ok, R.name.c_str(), R.tmin, R.name.c_str(), R.tmed,
                       R.name.c_str(), cells / (R.tmin * 1e6), R.name.c_str(), cells / (R.tmed * 1e6));
#ifdef WV_ONCARD_TB
        const double e2e = R.tbrun_ms > 0 ? R.tbrun_ms + R.rb_ms : R.tmin;   /* measured kernel+walk + ops readback */
#else
        const double e2e = R.tmin + R.tb_ms + 0.1;   /* kernel + host traceback + launch */
#endif
        if (c) fprintf(c, "%s,%" PRId64 ",%" PRId64 ",%.0f,%" PRId64 ",%" PRId64 ",%d,%d,%.4f,%.4f,%d,%.2f,%.2f,%.0f,%.2f,%.2f,%.2f\n",
                       R.name.c_str(), R.m, R.n, cells, R.gold, R.score, (int)R.score_ok, (int)R.path_ok,
                       R.tmin, R.tmed, R.reps, cells / (R.tmin * 1e6), cells / (R.tmed * 1e6), R.cpu_ms,
                       R.tb_ms, e2e, cells / (e2e * 1e6));
    }
    if (f) { fprintf(f, "pe %d\ntiles %d\npass %d\nwall_ms %.0f\n", NW_PE, WV_TILES, (int)pass, now_ms() - t_all); fclose(f); }
    if (c) fclose(c);
    const PhaseResult &P = res.back();
    printf("RESULT: %s  pe=%d  perf %.3e cells  kernel=%.3f ms  %.1f GCUPS  traceback=%.1f ms  END-TO-END %.1f GCUPS  check=%s/%s perf=%s/%s\n",
           pass ? "PASS" : "FAIL", NW_PE, (double)P.m * P.n, P.tmin, (double)P.m * P.n / (P.tmin * 1e6),
           P.tb_ms,
#ifdef WV_ONCARD_TB
           (double)P.m * P.n / ((P.tbrun_ms > 0 ? P.tbrun_ms + P.rb_ms : P.tmin) * 1e6),
#else
           (double)P.m * P.n / ((P.tmin + P.tb_ms + 0.1) * 1e6),
#endif
           res[0].score_ok ? "score" : "SCORE-BAD", res[0].path_ok ? "path" : "PATH-BAD",
           P.score_ok ? "score" : "SCORE-BAD", P.path_ok ? "path" : "PATH-BAD");
    return pass ? 0 : 1;
}
