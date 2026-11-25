/* nw_wave_lut.h — GENERATED, do not edit by hand.
 *
 * The whole PE as a truth table: next state AND traceback direction. With the delta
 * encoding dH and dV each take only the 7 values [-2,4] and the substitution score only
 * 2, so the PE's entire behaviour is 7 bits in, 8 bits out -- 98 reachable entries.
 *
 * WHY A TABLE: expressed as arithmetic the loop-carried path
 *     dv -> (dv+G) -> max3 -> (e-dv) -> dv
 * measured 3.213 ns in HLS (warning 200-887), because ap_int widens at every step. The
 * recurrence is genuinely loop-carried and cannot be pipelined away, so the only way to
 * shorten it is to stop computing it. One distributed-ROM lookup is a single LUT level.
 *
 * THE DIRECTION IS FREE. It is a function of the SAME 7 index bits, so emitting it
 * widens the ROM output from 6 bits to 8 and adds NOTHING to the critical path.
 *
 *   index = (s_is_match << 6) | (enc(dH_up) << 3) | enc(dV_old)
 *   value = (dir << 6) | (enc(dH_new) << 3) | enc(dV_new)
 *   enc(d) = d + 2, mapping [-2,4] to [0,6]. Unreachable slots hold 0.
 *
 * DIRECTION SEMANTICS, with e = H[i][j] - H[i-1][j-1]:
 *   DIAG (0)  e == s              H[i][j] = H[i-1][j-1] + s   consume both
 *   UP   (1)  e == dH_up + G      H[i][j] = H[i-1][j]   + G   consume query, gap in ref
 *   LEFT (2)  e == dV_old + G     H[i][j] = H[i][j-1]   + G   consume ref, gap in query
 * Ties are broken diag > up > left. Every candidate attaining e is an optimal
 * predecessor, so the priority selects WHICH optimal alignment is reported, never a
 * suboptimal one -- which is why the test checks that the reconstructed path scores
 * exactly the DP optimum rather than matching one particular golden's choices.
 *
 * The deltas' [-2,4] range is MEASURED over 1.56M cells of full DP, not assumed: the
 * textbook bound for this recurrence is [gap, match] = [-2,2] and is wrong here.
 *
 * Generated from the SAME recurrence as nw_wave.c. nww_lut_selfcheck() asserts the
 * table equals the arithmetic over all 98 reachable inputs, so the two cannot drift.
 */
#ifndef NW_WAVE_LUT_H
#define NW_WAVE_LUT_H

#define NWW_LUT_SIZE 128
#define NWW_ENC(d)   ((d) + 2)
#define NWW_DEC(e)   ((e) - 2)

/* Defined in nw_wave.h, which is the public header; guarded so this file stays
 * standalone if included on its own. */
#ifndef NWW_DIAG
#define NWW_DIAG 0
#define NWW_UP   1
#define NWW_LEFT 2
#endif

#define NWW_LUT_IDX(s_match, enc_dh_up, enc_dv) \
    ((((s_match) ? 1 : 0) << 6) | ((enc_dh_up) << 3) | (enc_dv))
#define NWW_LUT_DV(v)  ((v) & 0x7)
#define NWW_LUT_DH(v)  (((v) >> 3) & 0x7)
#define NWW_LUT_DIR(v) (((v) >> 6) & 0x3)

static const unsigned char NWW_LUT[NWW_LUT_SIZE] = {
    0x1b, 0x13, 0x0b, 0x03, 0x84, 0x85, 0x86, 0x00,
    0x1a, 0x12, 0x0a, 0x02, 0x83, 0x84, 0x85, 0x00,
    0x19, 0x11, 0x09, 0x01, 0x82, 0x83, 0x84, 0x00,
    0x18, 0x10, 0x08, 0x00, 0x81, 0x82, 0x83, 0x00,
    0x60, 0x58, 0x50, 0x48, 0x40, 0x81, 0x82, 0x00,
    0x68, 0x60, 0x58, 0x50, 0x48, 0x40, 0x81, 0x00,
    0x70, 0x68, 0x60, 0x58, 0x50, 0x48, 0x40, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x36, 0x2e, 0x26, 0x1e, 0x16, 0x0e, 0x06, 0x00,
    0x35, 0x2d, 0x25, 0x1d, 0x15, 0x0d, 0x05, 0x00,
    0x34, 0x2c, 0x24, 0x1c, 0x14, 0x0c, 0x04, 0x00,
    0x33, 0x2b, 0x23, 0x1b, 0x13, 0x0b, 0x03, 0x00,
    0x32, 0x2a, 0x22, 0x1a, 0x12, 0x0a, 0x02, 0x00,
    0x31, 0x29, 0x21, 0x19, 0x11, 0x09, 0x01, 0x00,
    0x30, 0x28, 0x20, 0x18, 0x10, 0x08, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

#endif
