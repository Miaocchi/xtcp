#pragma once

/**
 * @file cc_kcc_probe.h
 * @brief Read-only KCC state snapshot for tests (no control-path effect).
 *
 * Mirrors the private KccState fields needed to observe the geodesic
 * estimator, G2/G3 evidence counters, min_rtt confirmation, PROBE_BW cycle
 * phase and full-bw detection. Not part of the public CC API.
 */

#include <xtcp/cc/cc.h>

namespace xtcp {
    namespace cc {
        /**
         * @brief Copy of the per-connection KCC state (diagnostics only).
         */
        struct KccSnapshot {
            UInt64 x_est = 0;
            UInt64 max_bw = 0;
            UInt64 full_bw = 0;
            UInt32 p_est = 0;
            UInt32 qdelay_avg = 0;
            UInt32 jitter_ewma = 0;
            UInt32 sample_cnt = 0;
            UInt32 min_rtt_us = 0;
            UInt32 rtt_cnt = 0;
            UInt32 confirm_cnt = 0;
            UInt32 confirm_slow_cnt = 0;
            UInt32 locked = 0;
            UInt32 locked_high_cnt = 0;
            UInt32 min_rtt_fast_fall_cnt = 0;
            UInt32 x_est_fall_cnt = 0;
            UInt32 srtt_guard_cnt = 0;
            UInt32 srtt_suppress_cnt = 0;
            UInt32 round_rtt_min = 0;
            UInt32 prev_round_rtt_min = 0;
            UInt32 mode = 0;
            UInt32 pacing_gain = 0;
            UInt32 cwnd_gain = 0;
            UInt32 cycle_idx = 0;
            UInt32 full_bw_reached = 0;
            UInt32 full_bw_cnt = 0;
            UInt32 has_seen_rtt = 0;
            // Delivered-round / idle / DRAIN state (residual-limits wave).
            UInt64 next_rtt_delivered = 0;
            UInt64 total_delivered = 0;
            UInt64 cycle_mstamp_us = 0;
            UInt64 last_ack_us = 0;
            UInt64 drain_enter_stamp_us = 0;
            UInt32 idle_restart = 0;
            UInt32 rand_state = 0;
            // Recovery interaction (mirror of sk->ca_state / prior_cwnd).
            UInt32 ca_state = 0;
            UInt32 prev_ca_state = 0;
            UInt32 prior_cwnd = 0;
            UInt32 packet_conservation = 0;
        };

        /**
         * @brief Copies the current KCC state.
         * @return False when sk/out is NULL or KCC is not initialised.
         */
        bool KccGetSnapshot(const XtcpConnCc* sk, KccSnapshot* out) noexcept;

        /**
         * @brief Test-only: every connection created afterwards starts from
         *        this exact SplitMix32 state (deterministic, reproducible).
         */
        void KccTestSetRandomSeed(UInt32 seed) noexcept;
        /**
         * @brief Test-only: subsequent connections derive their seed from an
         *        incrementing counter (production-style, predictable).
         */
        void KccTestSetRandomCounter(UInt32 counter) noexcept;
        /**
         * @brief Test-only: restore production seeding (global counter).
         */
        void KccTestResetRandom() noexcept;
        /**
         * @brief Test-only: one draw from the production RandBelow sampler
         *        (SplitMix32 + rejection sampling), so the uniformity test
         *        exercises the real sampler without a live connection.
         * @param state SplitMix32 state (updated in place).
         * @param n     Exclusive upper bound.
         */
        UInt32 KccTestRandBelow(UInt32* state, UInt32 n) noexcept;
        /**
         * @brief Test-only: the rejection limit for n (exclusive upper bound
         *        of accepted draws, computed in 64 bits as 2^32 - 2^32 % n).
         *        Exposed so the boundary rows (accept limit-1, reject limit)
         *        can be tested without waiting for a 1-in-6e8 draw.
         */
        UInt64 KccTestRandLimit(UInt32 n) noexcept;
        /**
         * @brief Test-only: one draw through the production sampler with an
         *        INJECTED raw value (bypasses SplitMix32). Returns the
         *        accepted bucket, or false when the raw value is rejected.
         */
        bool KccTestRandBelowFromRaw(UInt64 raw, UInt32 n, UInt32* out) noexcept;
        /**
         * @brief Test-only: kernel kcc_packets_in_net_at_edt in the byte
         *        domain (packets scaled by mss), so the EDT credit term can
         *        be pinned exactly without a live connection.
         * @param inflight_bytes Raw in-flight bytes.
         * @param gain           Pacing gain in BBR_UNIT scale (256 = 1.0x).
         * @param mss            Segment size.
         * @param bw_pkts        Packet-domain bw estimate (segments/us << 24).
         * @param edt_us         interval_to_edt in microseconds.
         */
        /**
         * @brief Test-only: drives kernel kcc_is_next_cycle_phase for the
         *        connection's current PROBE_BW phase (read-only: the phase
         *        is not advanced). Lets a test show that the decision reads
         *        rs->prior_in_flight_bytes (the pre-ACK snapshot) and not
         *        the post-ACK sk->inflight.
         * @return 1 when the phase should end, 0 otherwise.
         */
        UInt32 KccTestIsNextCyclePhase(XtcpConnCc* sk, const RateSample* rs,
                                       UInt64 now_us) noexcept;
        UInt64 KccTestPacketsInNetAtEdt(UInt64 inflight_bytes, UInt32 gain, UInt32 mss,
                                        UInt64 bw_pkts, UInt32 edt_us) noexcept;
    }
}
