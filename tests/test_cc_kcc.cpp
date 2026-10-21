/**
 * @file test_cc_kcc.cpp
 * @brief KCC fidelity differential vs the reference core
 *        (plugins/ref/kcc_core.c) plus white-box tests of the c8bead4
 *        estimator/min_rtt/cycle/full-bw fixes through the read-only
 *        KccSnapshot probe.
 *
 * Goldens marked "kcc_model" come from F:\kcc\tests\kcc_model.py
 * (read-only reference model at commit c8bead4).
 */

#include <xtcp/cc/cc.h>

#include "cc_kcc_probe.h"

#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
#include "ref/kcc_core.c"
}

static int g_failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

namespace {
    using xtcp::cc::KccSnapshot;
    using xtcp::cc::RateSample;
    using xtcp::cc::XtcpConnCc;
    using xtcp::cc::XtcpCongestionOps;

    constexpr UInt32 kMss = 1460;
    constexpr UInt32 kDelivered = 29200;
    // Packet-domain pins (kernel bw = delivered_pkts << 24 / interval_us).
    // Driver input 29200 B / 20000 us = 20 segments -> bw = (20<<24)/20000
    // = 16777; rate = Haircut(MulShr8(bw*mss, gain)).
    constexpr UInt64 kStartupRate = 4172409;     /* gain 739 (2.887x) */
    constexpr UInt64 kProbeRate = 1806726;       /* gain 320 (1.25x) */
    constexpr UInt64 kDrainRate = 1084036;       /* 0.75x PROBE phase */
    constexpr UInt64 kDrainModeRate = 496849;    /* DRAIN mode (~0.347x) */
    constexpr UInt64 kCruiseRate = 1445381;      /* gain 256 (1.0x) */
    constexpr UInt64 kPacingExtreme = 1068150599751ull;

    /**
     * @brief Drives the port and the reference core with one shared input.
     *
     * Round cadence is DELIVERED-based now: Round() passes the pre-ACK
     * delivered total as prior_delivered, which crosses the next round
     * boundary exactly once. InRound() passes prior_delivered = 0, so it
     * stays inside the same burst (only the very first sample can open a
     * round). Tests that need an explicit boundary call Ack() with the
     * desired prior value.
     */
    struct Driver {
        kcc_core ref;
        XtcpConnCc sk;
        const XtcpCongestionOps* ops = NULLPTR;
        UInt64 now_us = 0;
        UInt32 last_rtt = 0;
        bool started = false;
        UInt32 flight = 20 * kMss;
        UInt64 total = 0;          /**< cc_.delivered equivalent (bytes) */
        UInt64 total_pkts = 0;     /**< cc_.delivered_pkts equivalent (segments) */
        UInt32 rand_seed = 2;      /**< must match KccTestSetRandomSeed(seed) */
        UInt32 edt_us = 0;         /**< kernel interval_to_edt fed to both sides */

        explicit Driver(UInt32 seed_min = 0, UInt32 srtt = 0, UInt32 rand_seed_ = 2)
            : rand_seed(rand_seed_) {
            std::memset(&ref, 0, sizeof(ref));
            kcc_core_init(&ref);
            ref.mss = kMss;
            ref.srtt_us = srtt;
            if (0 < seed_min) {
                ref.min_rtt_us = seed_min;
            } else if (0 < srtt) {
                ref.min_rtt_us = srtt;
            }
            kcc_core_set_random_seed(&ref, rand_seed);

            sk.mss = kMss;
            sk.rtt_min_us = seed_min;
            sk.srtt_us = srtt;
            sk.inflight = flight;
            ops = xtcp::cc::FindCongestionControl("kcc");
            CHECK(NULLPTR != ops);
            if (NULLPTR != ops) {
                ops->init(&sk);
            }
        }

        ~Driver() {
            if (NULLPTR != ops) {
                ops->release(&sk);
            }
        }

        KccSnapshot Snap() const {
            KccSnapshot snapshot;
            const bool ok = xtcp::cc::KccGetSnapshot(&sk, &snapshot);
            CHECK(ok);
            return snapshot;
        }

        void Ack(UInt32 rtt_us, UInt32 interval_us, UInt32 delivered = kDelivered,
                 UInt32 lost = 0, UInt64 prior_delivered = 0,
                 bool is_ack_delayed = false, bool is_app_limited = false) {
            // Packet domain (kernel tp->delivered counts tcp_skb_pcount).
            // The driver's public inputs are BYTES, so the delivered count
            // maps to ceil(bytes/mss) segments and the prior watermark is
            // translated through the packet total accumulated so far (a
            // plain ceil(prior_bytes/mss) would be wrong for short
            // segments, which are one packet each).
            const UInt64 total_before = total;
            const UInt64 total_pkts_before = total_pkts;
            const UInt64 delivered_pkts = (0 == delivered) ? 0
                : ((static_cast<UInt64>(delivered) + kMss - 1) / kMss);
            UInt64 prior_pkts;
            if (0 == prior_delivered) {
                prior_pkts = 0;
            } else if (prior_delivered >= total_before) {
                prior_pkts = total_pkts_before;
            } else {
                prior_pkts = (0 == total_before) ? 0
                    : (prior_delivered * total_pkts_before) / total_before;
            }
            now_us += interval_us;
            total += delivered;
            total_pkts += delivered_pkts;
            ref.srtt_us = sk.srtt_us;
            kcc_core_on_ack_pkts(&ref, static_cast<s64>(now_us),
                                 static_cast<s64>(delivered_pkts), static_cast<s64>(rtt_us),
                                 static_cast<s64>(flight), static_cast<s64>(flight),
                                 static_cast<s64>(lost),
                                 static_cast<s64>(prior_pkts),
                                 static_cast<s64>(total_pkts),
                                 is_ack_delayed ? 1 : 0, is_app_limited ? 1 : 0,
                                 edt_us);
            RateSample rs;
            rs.delivered = delivered;
            rs.interval_us = interval_us;
            rs.rtt_us = rtt_us;
            rs.acked = delivered;
            rs.lost = lost;
            rs.is_app_limited = is_app_limited ? 1u : 0u;
            rs.prior_delivered = prior_delivered;
            rs.delivered_total = total;
            rs.is_ack_delayed = is_ack_delayed;
            rs.delivered_pkts = static_cast<UInt32>(delivered_pkts);
            rs.prior_delivered_pkts = prior_pkts;
            rs.delivered_total_pkts = total_pkts;
            rs.prior_in_flight_bytes = flight;
            rs.pacing_edt_us = edt_us;
            sk.inflight = flight;
            if (NULLPTR != ops) {
                ops->cong_control(&sk, &rs);
            }
        }

        void Round(UInt32 rtt_us, UInt32 delivered = kDelivered, UInt32 lost = 0,
                   bool delayed = false, bool app_limited = false) {
            const UInt32 interval = started ? last_rtt : ((0 < rtt_us) ? rtt_us : 1);
            const UInt64 prior = total;
            Ack(rtt_us, interval, delivered, lost, prior, delayed, app_limited);
            last_rtt = rtt_us;
            started = true;
        }

        /** @brief Kernel icsk_ca_state on BOTH sides (recovery differential). */
        void SetCaState(Byte state) {
            sk.ca_state = state;
            ref.ca_state = state;
        }
        /** @brief Kernel tp->prior_cwnd on BOTH sides. */
        void SetPriorCwnd(UInt32 cwnd) {
            sk.prior_cwnd = cwnd;
            ref.prior_cwnd = cwnd;
        }

        void InRound(UInt32 rtt_us, UInt32 interval_us = 1,
                     UInt32 delivered = kDelivered, UInt32 lost = 0,
                     bool delayed = false, bool app_limited = false) {
            Ack(rtt_us, interval_us, delivered, lost, 0, delayed, app_limited);
        }
    };

    void CheckFullSnapshot(Driver& d) {
        const KccSnapshot p = d.Snap();
        kcc_snapshot r;
        kcc_core_get_snapshot(&d.ref, &r);
        CHECK(p.x_est == r.x_est);
        CHECK(p.max_bw == r.max_bw);
        CHECK(p.full_bw == r.full_bw);
        CHECK(p.p_est == r.p_est);
        CHECK(p.qdelay_avg == r.qdelay_avg);
        CHECK(p.jitter_ewma == r.jitter_ewma);
        CHECK(p.sample_cnt == r.sample_cnt);
        CHECK(p.min_rtt_us == r.min_rtt_us);
        CHECK(p.rtt_cnt == r.rtt_cnt);
        CHECK(p.confirm_cnt == r.confirm_cnt);
        CHECK(p.confirm_slow_cnt == r.confirm_slow_cnt);
        CHECK(p.locked == r.locked);
        CHECK(p.locked_high_cnt == r.locked_high_cnt);
        CHECK(p.min_rtt_fast_fall_cnt == r.min_rtt_fast_fall_cnt);
        CHECK(p.x_est_fall_cnt == r.x_est_fall_cnt);
        CHECK(p.srtt_guard_cnt == r.srtt_guard_cnt);
        CHECK(p.srtt_suppress_cnt == r.srtt_suppress_cnt);
        CHECK(p.round_rtt_min == r.round_rtt_min);
        CHECK(p.prev_round_rtt_min == r.prev_round_rtt_min);
        CHECK(p.mode == r.mode);
        CHECK(p.pacing_gain == r.pacing_gain);
        CHECK(p.cwnd_gain == r.cwnd_gain);
        CHECK(p.cycle_idx == r.cycle_idx);
        CHECK(p.full_bw_reached == r.full_bw_reached);
        CHECK(p.full_bw_cnt == r.full_bw_cnt);
        CHECK(p.has_seen_rtt == r.has_seen_rtt);
        CHECK(p.idle_restart == r.idle_restart);
        CHECK(p.next_rtt_delivered == r.next_rtt_delivered);
        CHECK(p.total_delivered == r.total_delivered);
        CHECK(p.cycle_mstamp_us == r.cycle_mstamp_us);
        CHECK(p.last_ack_us == r.last_ack_us);
        CHECK(p.drain_enter_stamp_us == r.drain_enter_stamp_us);
        CHECK(p.rand_state == r.rand_state);
        // Recovery interaction (kcc_set_cwnd_to_recover_or_restore).
        CHECK(p.ca_state == r.ca_state);
        CHECK(p.prev_ca_state == r.prev_ca_state);
        CHECK(p.prior_cwnd == r.prior_cwnd);
        CHECK(p.packet_conservation == r.packet_conservation);
    }

    void CheckRefCommon(Driver& d) {
        const KccSnapshot s = d.Snap();
        CHECK(s.x_est == kcc_core_x_est(&d.ref));
        CHECK(s.p_est == kcc_core_p_est(&d.ref));
        CHECK(s.qdelay_avg == kcc_core_qdelay(&d.ref));
        CHECK(s.min_rtt_us == kcc_core_min_rtt(&d.ref));
        CHECK(s.mode == kcc_core_mode(&d.ref));
        CHECK(static_cast<s64>(s.max_bw) == d.ref.max_bw);
    }

    void TestKccG3PathIncreasePerRound() {
        Driver d;
        for (UInt32 i = 0; i < 10; ++i) {
            d.Round(20000);
        }
        CHECK(20000 == d.Snap().min_rtt_us);

        UInt32 detected = 0;
        KccSnapshot s = d.Snap();
        for (UInt32 r = 1; r <= 8; ++r) {
            d.Round(200000);
            s = d.Snap();
            CHECK(s.min_rtt_us == d.ref.min_rtt_us);
            if (190000 <= s.min_rtt_us) {
                detected = r;
                break;
            }
        }
        CHECK(0 < detected);
        CHECK(detected <= 7);
        CHECK(200000 == s.min_rtt_us);
        CHECK(200000 == d.ref.min_rtt_us);
        CheckRefCommon(d);
    }

    void TestKccG3PerRoundEvidence() {
        Driver d;
        for (UInt32 i = 0; i < 10; ++i) {
            d.Round(20000);
        }

        // A single 25ms round with 20 ACKs. Counting per ACK used to reach
        // six consecutive exceedances inside this one round.
        d.Round(25000);
        for (UInt32 i = 1; i < 20; ++i) {
            d.InRound(25000);
        }
        KccSnapshot s = d.Snap();
        CHECK(0 == s.confirm_cnt);
        CHECK(s.confirm_cnt <= 1);
        CHECK(25000 == s.round_rtt_min);
        CHECK(20000 == s.min_rtt_us);

        // The next round boundary sees one elevated round => at most one count.
        d.Round(20000);
        s = d.Snap();
        CHECK(s.confirm_cnt <= 1);
        CHECK(s.confirm_slow_cnt <= 1);
        CHECK(20000 == s.min_rtt_us);

        // A clean baseline round breaks the chain.
        d.Round(20000);
        s = d.Snap();
        CHECK(0 == s.confirm_cnt);
        CHECK(0 == s.confirm_slow_cnt);
        CHECK(20000 == s.min_rtt_us);
        CheckRefCommon(d);
    }

    void TestKccLockRelease() {
        Driver d;
        d.Round(4000);
        KccSnapshot s = d.Snap();
        CHECK(1 == s.locked);
        CHECK(4000 == s.min_rtt_us);

        for (UInt32 i = 0; i < 10; ++i) {
            d.Round(40000);
        }
        s = d.Snap();
        CHECK(0 == s.locked);
        CHECK(40000 == s.min_rtt_us);
        CHECK(40000 == d.ref.min_rtt_us);
        CheckRefCommon(d);
    }

    void TestKccLongPathOutlierDoesNotLock() {
        Driver d;
        for (UInt32 i = 0; i < 5; ++i) {
            d.Round(20000);
        }
        d.Round(4000);
        KccSnapshot s = d.Snap();
        CHECK(0 == s.locked);
        CHECK(20000 == s.min_rtt_us);

        // G3 must still work after the outlier.
        for (UInt32 i = 0; i < 7; ++i) {
            d.Round(200000);
        }
        s = d.Snap();
        CHECK(200000 == s.min_rtt_us);
        CHECK(200000 == d.ref.min_rtt_us);
        CheckRefCommon(d);
    }

    void TestKccStickyAndFastFall() {
        // Sticky: 5 consecutive rounds below 0.75 * min_rtt (kcc_model).
        {
            Driver d;
            for (UInt32 i = 0; i < 5; ++i) {
                d.Round(20000);
            }
            for (UInt32 i = 0; i < 4; ++i) {
                d.Round(10000);
                CHECK(20000 == d.Snap().min_rtt_us);
            }
            d.Round(10000);
            KccSnapshot s = d.Snap();
            CHECK(10000 == s.min_rtt_us);
            CHECK(10000 == d.ref.min_rtt_us);
            CheckRefCommon(d);
        }

        // Fast fall: a drop below min_rtt/4 needs two consecutive rounds.
        {
            Driver d;
            for (UInt32 i = 0; i < 5; ++i) {
                d.Round(20000);
            }
            d.Round(4000);
            CHECK(20000 == d.Snap().min_rtt_us);
            CHECK(1 == d.Snap().min_rtt_fast_fall_cnt);
            d.Round(4000);
            KccSnapshot s = d.Snap();
            CHECK(4000 == s.min_rtt_us);
            CHECK(4000 == d.ref.min_rtt_us);
            CheckRefCommon(d);
        }
    }

    void TestKccSrttGuard() {
        // Stable srtt below min_rtt must engage after five rounds.
        {
            Driver d(20000, 10000);
            for (UInt32 i = 0; i < 10; ++i) {
                d.Round(20000);
            }
            KccSnapshot s = d.Snap();
            CHECK(10000 == s.min_rtt_us);
            CHECK(10000 == d.ref.min_rtt_us);
            CheckRefCommon(d);
        }

        // A G3-confirmed path increase must survive a lagging srtt.
        {
            Driver d(20000, 20000);
            for (UInt32 i = 0; i < 10; ++i) {
                d.Round(20000);
            }
            UInt32 detected = 0;
            for (UInt32 r = 1; r <= 8; ++r) {
                d.Round(200000);
                if (190000 <= d.Snap().min_rtt_us) {
                    detected = r;
                    break;
                }
            }
            CHECK(0 < detected && detected <= 7);
            for (UInt32 i = 0; i < 10; ++i) {
                d.Round(200000);
                CHECK(190000 <= d.Snap().min_rtt_us);
            }
            CHECK(200000 == d.ref.min_rtt_us);
            CheckRefCommon(d);
        }
    }

    void TestKccG2PerRound() {
        Driver d;
        d.Round(20000);
        CHECK((UInt64(20000) << 10) == d.Snap().x_est);

        // Two ACKs inside the same round must not grow x_est.
        d.InRound(21000);
        CHECK((UInt64(20000) << 10) == d.Snap().x_est);
        d.InRound(22000);
        KccSnapshot s = d.Snap();
        CHECK((UInt64(20000) << 10) == s.x_est);
        CHECK(1125 == s.qdelay_avg);                     // kcc_model same-round golden

        // Next round applies one 12.2% G2 step, capped at the observation.
        d.Round(30000);
        s = d.Snap();
        CHECK((UInt64(22440) << 10) == s.x_est);         // kcc_model golden
        CHECK(1500 <= s.qdelay_avg);
        CHECK(2234 == s.qdelay_avg);                     // kcc_model golden
        CheckRefCommon(d);
    }

    void TestKccQdelayBaseline() {
        Driver d;
        d.Round(20000);
        // 100 ACKs in one round: a gradual 10ms queue build. The qdelay
        // baseline is the confirmed min_rtt, not x_est (which stays at the
        // observed 20ms because G2 runs once per round).
        for (UInt32 i = 0; i < 100; ++i) {
            d.InRound(20000 + i * 100);
        }
        KccSnapshot s = d.Snap();
        CHECK(9200 == s.qdelay_avg);                     // kcc_model golden
        CHECK((UInt64(20000) << 10) == s.x_est);
        CHECK(20000 == s.min_rtt_us);
        CheckRefCommon(d);
    }

    void TestKccStalenessDownwardOnly() {
        Driver d;
        for (UInt32 i = 0; i < 10; ++i) {
            d.Round(20000);
        }
        for (UInt32 i = 0; i < 7; ++i) {
            d.Round(40000);
        }
        CHECK(40000 == d.Snap().min_rtt_us);
        for (UInt32 i = 0; i < 127; ++i) {
            d.Round(40000);
        }
        // This sample is the 128th round since the last min_rtt update and
        // therefore the first staleness evaluation. Downward-only: x_est
        // must not be raised to 95% of min_rtt.
        d.Round(10000);
        KccSnapshot s = d.Snap();
        CHECK((UInt64(10000) << 10) == s.x_est);
        CHECK(40000 == s.min_rtt_us);
        CheckRefCommon(d);
    }

    void TestKccFullBwThresholdMatchesKernel() {
        Driver d;
        UInt32 delivered = 14600;
        bool reached = false;
        for (UInt32 i = 0; i < 8; ++i) {
            d.Round(10000, delivered);
            KccSnapshot s = d.Snap();
            // Kernel full-bw detection runs on the round_start sample, so the
            // plateau count starts one round earlier than the old time-round
            // port. The detection sample itself enters DRAIN (mode 2).
            if (i == 3) {
                CHECK(1 == s.full_bw_reached);
                CHECK(2 == s.mode);                      // DRAIN on entry
                CHECK(1 == d.ref.full_bw_reached);
                CHECK(2 == d.ref.mode);
                reached = true;
            }
            // 3%/round keeps the three-round growth below 1.25x, which is
            // what the kernel's plateau detector measures (a 10%/round
            // staircase would refresh full_bw on the fourth round instead).
            delivered += delivered * 3 / 100;
        }
        CHECK(reached);
        CHECK(1 == d.Snap().full_bw_reached);
    }

    void TestKccProbeCycleAdvances() {
        Driver d;
        const UInt32 delivered = 29200;
        UInt64 rates[12] = {0};
        UInt32 cycles[12] = {0};
        for (UInt32 i = 0; i < 12; ++i) {
            d.Round(20000, delivered);
            rates[i] = d.sk.pacing_rate;
            cycles[i] = d.Snap().cycle_idx;
            CHECK(20000 == d.Snap().min_rtt_us);
        }
        // PIN (delivered-round + kernel order):
        //  0-2 STARTUP; 3-4 DRAIN (detection sample enters DRAIN; exit at
        //  +2*min_rtt); 5-7 probe-up 1.25x (2*min_rtt timeout bounds it);
        //  8-9 drain phase 0.75x; 10-11 cruise 1.0x.
        for (UInt32 i = 0; i < 3; ++i) {
            CHECK(kStartupRate == rates[i]);
        }
        CHECK(kDrainModeRate == rates[3]);
        CHECK(kDrainModeRate == rates[4]);
        CHECK(kProbeRate == rates[5]);
        CHECK(kProbeRate == rates[6]);
        CHECK(kProbeRate == rates[7]);
        CHECK(kDrainRate == rates[8]);
        CHECK(kDrainRate == rates[9]);
        CHECK(kCruiseRate == rates[10]);
        CHECK(kCruiseRate == rates[11]);
        CHECK(0 == cycles[5]);
        CHECK(0 == cycles[6]);
        CHECK(0 == cycles[7]);
        CHECK(1 == cycles[8]);
        CHECK(1 == cycles[9]);
        CHECK(2 == cycles[10]);
        CHECK(2 == cycles[11]);
        CheckRefCommon(d);
    }

    void TestKccPacingExtremeClamp() {
        Driver d;
        d.Ack(1000, 1, 0xFFFFFFFFu);
        const KccSnapshot s = d.Snap();
        // Kernel clamps the packet-domain estimate to U32_MAX.
        CHECK(0xFFFFFFFFull == s.max_bw);
        CHECK(kPacingExtreme == d.sk.pacing_rate);
        CHECK(static_cast<s64>(kPacingExtreme) == kcc_core_pacing_rate(&d.ref));
        CheckRefCommon(d);
    }

    void TestKccBdpSaturation() {
        // product = max_bw * min_rtt overflows u64 on a long-stall RTT
        // (4s) with the clamped bandwidth. The saturated BDP must stay
        // positive and below the segment clamp, and pacing must stay valid.
        // The cwnd model is additive in STARTUP now (kernel kcc_set_cwnd:
        // cwnd += acked), so the old "one step to the saturated BDP
        // (>700M segments)" pin no longer applies - that was the
        // max(prev,BDP) behaviour being removed. Pin the exact additive
        // growth instead (explicit expectation change, not a regression).
        Driver d;
        d.Ack(4000000u, 1, 0xFFFFFFFFu);
        const KccSnapshot s = d.Snap();
        CHECK(0 != s.max_bw);
        const UInt64 expected = 10ull + (0xFFFFFFFFull + kMss - 1) / kMss;
        CHECK(expected == static_cast<UInt64>(d.sk.snd_cwnd));
        CHECK(d.sk.snd_cwnd <= 0x7FFFFFFFu);
        CHECK(0 < d.sk.pacing_rate);
        CHECK(kcc_core_cwnd_segs(&d.ref) == static_cast<s64>(d.sk.snd_cwnd));
        CheckRefCommon(d);
    }

    void TestKccPacingUnitGrid() {
        // Intervals are powers of two <= BW_SCALE, so the segment-domain
        // floor and the byte-domain floor are identical and the check can be
        // exact (the audit's <=1 B/s bound).
        const UInt32 segs[] = {1, 2, 10, 100};
        const UInt32 intervals[] = {1, 2, 4, 8, 16, 64, 256, 1024};
        const UInt32 kMinRtt = 1000000000u;   // seeds min_rtt; boot rate is 0
        for (UInt32 si = 0; si < 4; ++si) {
            for (UInt32 ii = 0; ii < 8; ++ii) {
                Driver d(kMinRtt);
                const UInt32 delivered = segs[si] * kMss;
                d.Ack(kMinRtt, intervals[ii], delivered);
                // Independent segment-domain kernel formula:
                // bw = segs << BW_SCALE / interval; rate = bw*mss*gain;
                // 1% converted by the BW_SCALE shift; USEC_PER_SEC fold.
                const UInt64 bw_seg = (static_cast<UInt64>(delivered / kMss) << 24) / intervals[ii];
                const UInt64 raw = (bw_seg * kMss * 739) >> 8;
                const UInt64 expected = (raw * 990000ull) >> 24;
                const UInt64 actual = d.sk.pacing_rate;
                const UInt64 diff = (actual > expected) ? (actual - expected)
                                                        : (expected - actual);
                CHECK(diff <= 1);
                if (diff > 1) {
                    std::fprintf(stderr, "grid seg=%u interval=%u actual=%llu expected=%llu\n",
                                 segs[si], intervals[ii],
                                 static_cast<unsigned long long>(actual),
                                 static_cast<unsigned long long>(expected));
                }
            }
        }
    }

    void TestKccBdpConfirmedBaseline() {
        Driver d;
        for (UInt32 i = 0; i < 10; ++i) {
            d.Round(20000);
        }
        const UInt32 baseline_cwnd = d.sk.snd_cwnd;
        CHECK(0 < baseline_cwnd);

        d.Round(5000);
        KccSnapshot s = d.Snap();
        CHECK(20000 == s.min_rtt_us);
        CHECK(static_cast<UInt64>(d.sk.snd_cwnd) * 10 >=
              static_cast<UInt64>(baseline_cwnd) * 6);

        for (UInt32 i = 0; i < 3; ++i) {
            d.Round(20000);
        }
        s = d.Snap();
        CHECK(20000 == s.min_rtt_us);
        CHECK(static_cast<UInt64>(d.sk.snd_cwnd) * 10 >=
              static_cast<UInt64>(baseline_cwnd) * 6);
        CHECK(20000 == d.ref.min_rtt_us);
        CheckRefCommon(d);
    }

    void TestKccColdStart() {
        {
            Driver d;
            CHECK(1000 == d.Snap().min_rtt_us);
            d.Round(20000);
            KccSnapshot s = d.Snap();
            CHECK(20000 == s.min_rtt_us);
            CHECK((UInt64(20000) << 10) == s.x_est);
            CHECK(0 < d.sk.pacing_rate);
            CHECK(20000 == d.ref.min_rtt_us);
            CheckRefCommon(d);
        }
        {
            // A real init seed must be preserved (not replaced as placeholder).
            Driver d(8000);
            CHECK(8000 == d.Snap().min_rtt_us);
            d.Round(20000);
            CHECK(8000 == d.Snap().min_rtt_us);
            CHECK(8000 == d.ref.min_rtt_us);
        }
    }

    void TestKccDifferential() {
        Driver d;
        // Deterministic RTT sequence: starts at 30ms, dips to 20ms (new min),
        // then a slow upward drift with noise.
        UInt32 seed = 0xABCD1234;
        const auto next = [&seed]() -> UInt32 {
            seed = seed * 1664525u + 1013904223u;
            return seed;
        };

        const UInt32 kSteps = 3000;
        for (UInt32 i = 0; i < kSteps; ++i) {
            UInt32 rtt_us;
            if (i < 200) {
                rtt_us = 30000 - i * 5;              // 30ms -> 29ms
            } else if (i < 400) {
                rtt_us = 20000 + (next() % 500);     // new min ~20ms
            } else {
                rtt_us = 20000 + (i % 1000) * 3 + (next() % 2000);
            }
            const UInt32 acked = 1 + (next() % 4);
            const UInt32 delivered = acked * kMss;
            const UInt32 lost = (0 == (i % 700)) ? delivered : 0;
            const UInt32 interval_us = 10000 + (next() % 5000);
            d.Ack(rtt_us, interval_us, delivered, lost);

            CheckFullSnapshot(d);
            CHECK(kcc_core_cwnd_segs(&d.ref) == static_cast<s64>(d.sk.snd_cwnd));
            CHECK(kcc_core_pacing_rate(&d.ref) == static_cast<s64>(d.sk.pacing_rate));
            if (0 < g_failures) {
                std::fprintf(stderr, "differential mismatch at step %u\n", i);
                break;
            }
        }
    }

    void TestKccRegistry() {
        xtcp::cc::RegisterKcc();
        const XtcpCongestionOps* ops = xtcp::cc::FindCongestionControl("kcc");
        CHECK(NULLPTR != ops);
        CHECK(0 == std::strcmp("kcc", ops->name));

        XtcpConnCc sk;
        ops->init(&sk);
        sk.mss = kMss;
        CHECK(NULLPTR != sk.ca_priv);

        // Initial state: STARTUP, cwnd = 10.
        CHECK(10 == sk.snd_cwnd);
        ops->release(&sk);
        CHECK(NULLPTR == sk.ca_priv);
    }

    void TestKccRoundBoundaryTracksFlight() {
        // Delivered-round semantics: a burst of ACK-clock samples whose
        // segments were all sent before the same delivered watermark opens
        // exactly ONE round regardless of how many ACKs arrive.
        Driver d;
        constexpr UInt32 kFirstDelivered = 10 * kMss;
        constexpr UInt32 kInterval = 100;

        for (UInt32 i = 0; i < 10; ++i) {
            const UInt32 delivered = (0 == i) ? kFirstDelivered : kMss;
            d.InRound(1000, kInterval, delivered);
            CHECK(1 == d.ref.rtt_cnt);
            CHECK(1 == d.Snap().rtt_cnt);
            CHECK(0 == d.ref.full_bw_cnt);
            CHECK(static_cast<UInt32>(kcc_core_cwnd_segs(&d.ref)) == d.sk.snd_cwnd);
        }
        // The boundary was captured at the first sample (10*mss); the ACK
        // clock alone does not move it.
        // Packet domain: the watermark is a segment count (10 MSS = 10).\n        CHECK(static_cast<UInt64>(kFirstDelivered / kMss) == d.Snap().next_rtt_delivered);

        // A segment sent after the round watermark advances it exactly once.
        d.Round(1000, kMss);
        CHECK(2 == d.ref.rtt_cnt);
        CHECK(2 == d.Snap().rtt_cnt);
        const KccSnapshot s = d.Snap();
        CHECK(d.total_pkts == s.next_rtt_delivered);  // boundary tracks delivered_total_pkts
        CHECK(static_cast<UInt32>(kcc_core_cwnd_segs(&d.ref)) == d.sk.snd_cwnd);
    }

    void TestKccRoundBoundaryTracksCurrentRtt() {
        // RTT changes must NOT drive round boundaries: only the delivered
        // watermark does. Same burst + changing RTT stays in one round.
        Driver d;
        const UInt32 kFastDelivered = 10 * kMss;
        const auto sample = [&](UInt32 delivered, UInt32 interval_us, UInt32 rtt_us) {
            d.InRound(rtt_us, interval_us, delivered);
            CHECK(static_cast<UInt32>(kcc_core_cwnd_segs(&d.ref)) == d.sk.snd_cwnd);
        };

        sample(kFastDelivered, 100, 1000);
        CHECK(1 == d.ref.rtt_cnt);
        const s64 max_bw = (static_cast<s64>(kFastDelivered / kMss) << 24) / 100;
        CHECK(max_bw == d.ref.max_bw);
        for (UInt32 i = 0; i < 20; ++i) {
            sample(kMss, 400, 4000);
            CHECK(1 == d.ref.rtt_cnt);   // still the same delivered round
        }
        const KccSnapshot s = d.Snap();
        CHECK(static_cast<UInt64>(kFastDelivered / kMss) == s.next_rtt_delivered);
    }

    void TestKccFullBwTransition() {
        // PIN: full-bw detection must fire on 3 consecutive PLATEAU rounds
        // (growth < 1.25x/round) and drive STARTUP -> DRAIN -> PROBE_BW.
        // Delivered-round detection fires on sample 4; the detection sample
        // enters DRAIN, and the 2*min_rtt timeout bounds probe-up at samples
        // 5-7. Sample 8 advances to the 0.75x phase, sample 10 to cruise.
        Driver d;
        const UInt32 kDeliveredSample = 29200;
        UInt64 rates[12] = {0};
        for (UInt32 i = 0; i < 12; ++i) {
            d.Round(20000, kDeliveredSample);
            rates[i] = d.sk.pacing_rate;
        }
        for (UInt32 i = 0; i < 3; ++i) {
            CHECK(kStartupRate == rates[i]);
        }
        CHECK(kDrainModeRate == rates[3]);   // DRAIN on the detection sample
        CHECK(kDrainModeRate == rates[4]);
        CHECK(kProbeRate == rates[5]);
        CHECK(kProbeRate == rates[6]);
        CHECK(kProbeRate == rates[7]);       // probe-up timeout at 2*min_rtt
        CHECK(kDrainRate == rates[8]);
        CHECK(kDrainRate == rates[9]);
        CHECK(kCruiseRate == rates[10]);
        CHECK(kCruiseRate == rates[11]);
    }

    // ------------------------------------------------------------------
    // Residual-limits wave (delivered round / delayed ACK / cycle / DRAIN /
    // cwnd) plus FSM collection tests.
    // ------------------------------------------------------------------

    /* Feed rounds until STARTUP detects the pipe is full. The detection
     * sample itself enters DRAIN (kernel kcc_check_drain order). */
    KccSnapshot FeedFullBw(Driver& d, UInt32 rtt_us = 10000) {
        UInt32 delivered = 14600;
        KccSnapshot detection;
        for (UInt32 i = 0; i < 5; ++i) {
            d.Round(rtt_us, delivered);
            if (3 == i) {
                detection = d.Snap();
            }
            // Grow slower than 1.25x over three rounds: the kernel only
            // refreshes full_bw when a round is >= 1.25x, so a 10%/round
            // staircase never plateaus against the stale reference.
            delivered += delivered * 3 / 100;
        }
        CHECK(2 == d.Snap().mode);
        CHECK(2 == detection.mode);
        return detection;
    }

    /* Drive STARTUP -> DRAIN -> PROBE_BW and return the first PROBE sample. */
    KccSnapshot ForceProbe(Driver& d, UInt32 rtt_us = 10000) {
        FeedFullBw(d, rtt_us);
        d.flight = 0;
        d.Round(rtt_us, 14600);
        const KccSnapshot s = d.Snap();
        CHECK(1 == s.mode);
        return s;
    }

    void TestKccDeliveredRound() {
        {
            // Same-burst: 1 first sample + 5 ACKs for segments that were all
            // sent before the same delivered watermark -> exactly ONE round.
            Driver d;
            d.Ack(20000, 20000, 10 * kMss, 0, 0);
            for (UInt32 i = 0; i < 5; ++i) {
                d.Ack(20000, 1, kMss, 0, 0);
            }
            const KccSnapshot s = d.Snap();
            CHECK(1 == s.rtt_cnt);
            CHECK(10ull == s.next_rtt_delivered);   // 10 MSS = 10 packets
            CHECK(d.total_pkts == s.next_rtt_delivered + 5ull);
        }
        {
            // ACK-clocked: every segment was sent after the previous round's
            // watermark -> one round per sample.
            Driver d;
            for (UInt32 i = 0; i < 10; ++i) {
                const UInt64 prior = d.total;
                d.Ack(1000, 100, 100 * kMss, 0, prior);
                CHECK(i + 1 == d.Snap().rtt_cnt);
                CHECK(d.total_pkts == d.Snap().next_rtt_delivered);
            }
        }
    }

    void TestKccDelayedAckFiltered() {
        Driver d;
        for (UInt32 i = 0; i < 20; ++i) {
            d.Round(20000);
        }
        CHECK(20000 == d.Snap().min_rtt_us);
        for (UInt32 i = 0; i < 8; ++i) {
            d.Round(45000, kDelivered, 0, true);
            // A delayed sample must not become the round's minimum.
            CHECK(0xFFFFFFFFu == d.Snap().round_rtt_min);
        }
        const KccSnapshot s = d.Snap();
        CHECK(20000 == s.min_rtt_us);     // G3 chain was not polluted
        CHECK(20000 == d.ref.min_rtt_us);
        // A real sample still updates the round statistic.
        d.Round(20000);
        CHECK(20000 == d.Snap().round_rtt_min);
        CheckRefCommon(d);
    }

    void TestKccDelayedAckTruthTable() {
        using xtcp::cc::IsLikelyDelayedAck;
        // acked, popped, len, retrans, prior_sacked, ece, sack, sent, after, mss
        CHECK(IsLikelyDelayedAck(100, 1, 100, false, false, false, false, 500, 600, kMss));
        CHECK(!IsLikelyDelayedAck(kMss, 1, kMss, false, false, false, false, 0, kMss, kMss));
        CHECK(!IsLikelyDelayedAck(200, 2, 100, false, false, false, false, 400, 600, kMss));
        CHECK(!IsLikelyDelayedAck(100, 1, 100, true, false, false, false, 500, 600, kMss));
        CHECK(!IsLikelyDelayedAck(100, 1, 100, false, true, false, false, 500, 600, kMss));
        CHECK(!IsLikelyDelayedAck(100, 1, 100, false, false, true, false, 500, 600, kMss));
        CHECK(!IsLikelyDelayedAck(100, 1, 100, false, false, false, true, 500, 600, kMss));
        CHECK(!IsLikelyDelayedAck(100, 1, 100, false, false, false, false, 450, 600, kMss));
        CHECK(!IsLikelyDelayedAck(100, 1, 150, false, false, false, false, 500, 600, kMss));
        CHECK(!IsLikelyDelayedAck(0, 1, 100, false, false, false, false, 500, 600, kMss));
    }

    void TestKccDrainExit() {
        {
            // Not drained: a huge pipe only leaves via the 4*min_rtt timeout.
            Driver d;
            const KccSnapshot entry = FeedFullBw(d);
            const UInt64 bdp = kcc_bdp_packets(entry.max_bw, entry.min_rtt_us, 256);
            const UInt64 expected_ssthresh = bdp;
            CHECK(expected_ssthresh == d.sk.snd_ssthresh);
            CHECK(entry.drain_enter_stamp_us == entry.last_ack_us);
            d.flight = 300000;  // ~10x BDP, never drained
            UInt32 delivered = 14600;
            // Detection happened at round 4; FeedFullBw's fifth sample put
            // elapsed at 10ms. These three samples reach 20/30/40ms, which
            // never exceeds the 4*min_rtt (40ms) timeout.
            for (UInt32 i = 0; i < 3; ++i) {
                d.Round(10000, delivered);
                CHECK(2 == d.Snap().mode);
            }
            d.Round(10000, delivered);       // elapsed 50ms > 4*min_rtt
            CHECK(1 == d.Snap().mode);
        }
        {
            // Drained: FeedFullBw's fifth sample already moved elapsed to
            // 10ms (== min_rtt, strict '>' false); the next sample reaches
            // 20ms with the pipe drained and exits.
            Driver d;
            FeedFullBw(d);
            d.flight = 0;
            d.Round(10000, 14600);
            CHECK(1 == d.Snap().mode);
        }
    }

    /**
     * @brief Kernel kcc_packets_in_net_at_edt EDT credit (tcp_kcc.c:5016-5037).
     *
     *   interval_delivered = (bw * interval_us) >> BW_SCALE
     *   inflight_at_edt    = ceil(inflight/mss) + (gain > 256 ? 1 : 0)
     *   credit >= inflight_at_edt -> 0
     *   else (inflight_at_edt - credit) * mss
     *
     * Row 1 pins backward compatibility: edt_us == 0 must reproduce the old
     * value exactly (no credit, only the gain headroom).
     */
    void TestKccDrainEdtCredit() {
        using xtcp::cc::KccTestPacketsInNetAtEdt;
        const UInt32 mss = kMss;               // 1460
        const UInt64 bw = 1ull << 24;          // 1 segment per microsecond

        // Row 1: edt_us == 0 -> identical to the old (credit-free) formula.
        CHECK(2 * mss == KccTestPacketsInNetAtEdt(2 * mss, 256, mss, bw, 0));
        CHECK(3 * mss == KccTestPacketsInNetAtEdt(2 * mss, 288, mss, bw, 0));  // +1 seg
        // Sub-MSS remainder still counts as one whole packet (kernel ceil).
        CHECK(mss == KccTestPacketsInNetAtEdt(1, 256, mss, bw, 0));

        // Row 2: credit >= inflight -> 0. 3 us at 1 seg/us credits 3 packets.
        CHECK(0 == KccTestPacketsInNetAtEdt(2 * mss, 256, mss, bw, 3));
        CHECK(0 == KccTestPacketsInNetAtEdt(2 * mss, 288, mss, bw, 4));

        // Row 3: partial credit -> exactly (pkts - credit) * mss.
        // 2 in flight, 1 us credit -> 1 packet left.
        CHECK(mss == KccTestPacketsInNetAtEdt(2 * mss, 256, mss, bw, 1));
        // 5 in flight + gain headroom, 2 us credit -> 4 packets.
        CHECK(4 * mss == KccTestPacketsInNetAtEdt(5 * mss, 288, mss, bw, 2));
        // Half-rate bw: 1 us credits 0.5 packet -> floor(credit) == 0.
        CHECK(5 * mss == KccTestPacketsInNetAtEdt(5 * mss, 256, mss, bw / 2, 1));
        // 2 us at half rate credits 1 packet -> 4 left.
        CHECK(4 * mss == KccTestPacketsInNetAtEdt(5 * mss, 256, mss, bw / 2, 2));

        // mss == 0 falls back to 1460 (never divides by zero).
        CHECK(2 * 1460 == KccTestPacketsInNetAtEdt(2 * 1460, 256, 0, bw, 0));
    }

    UInt32 ProbePhaseForSeed(UInt32 seed) {
        xtcp::cc::KccTestSetRandomSeed(seed);
        Driver d(0, 0, seed);
        const KccSnapshot s = ForceProbe(d);
        return s.cycle_idx;
    }

    void TestKccCycleRandomized() {
        // Fixed seed 2 reproduces the historical phase-0 sequence.
        CHECK(0 == ProbePhaseForSeed(2));
        CHECK(0 == ProbePhaseForSeed(2));
        // The kernel mapping never starts at phase 1 (drain) and is
        // reproducible per seed.
        for (UInt32 seed = 0; seed < 32; ++seed) {
            const UInt32 phase = ProbePhaseForSeed(seed);
            CHECK(1 != phase);
            CHECK(phase < 8);
            CHECK(phase == ProbePhaseForSeed(seed));
        }
        // Production-style counter seeding decorrelates connections.
        xtcp::cc::KccTestSetRandomCounter(1);
        UInt32 phases[8] = {0};
        for (UInt32 i = 0; i < 8; ++i) {
            Driver d(0, 0, 1);
            phases[i] = ForceProbe(d).cycle_idx;
        }
        bool differ = false;
        for (UInt32 i = 1; i < 8; ++i) {
            if (phases[i] != phases[0]) {
                differ = true;
            }
        }
        CHECK(differ);
        xtcp::cc::KccTestSetRandomSeed(2);
    }

    UInt64 ExpectedPacing(UInt64 bw) {
        // Haircut(MulShr8(bw, 1.0x)) with BBR_UNIT=256.
        const UInt64 v = bw;
        const UInt64 hi = v >> 8;
        UInt64 scaled = (0 != hi && hi > (~UInt64(0)) / 256) ? ~UInt64(0)
                                                            : hi * 256 + (((v & 0xFFu) * 256) >> 8);
        const UInt64 q = scaled >> 24;
        const UInt64 r = scaled & ((UInt64(1) << 24) - 1);
        return q * 990000ull + ((r * 990000ull) >> 24);
    }

    void TestKccIdleRestart() {
        xtcp::cc::KccTestSetRandomSeed(2);
        Driver d(0, 0, 2);
        const KccSnapshot probe = ForceProbe(d);
        CHECK(0 == probe.cycle_idx);
        const UInt64 rate_before = d.sk.pacing_rate;
        d.sk.app_limited = 1;
        d.ops->cwnd_event(&d.sk, xtcp::cc::kCaEventTxStart);
        KccSnapshot s = d.Snap();
        CHECK(1 == s.idle_restart);
        CHECK(s.cycle_mstamp_us == s.last_ack_us);
        CHECK(ExpectedPacing(s.max_bw * kMss) == d.sk.pacing_rate);
        (void)rate_before;

        // No app-limited marker: the event has no effect.
        d.Round(20000, kDelivered);       // delivered>0 clears idle_restart
        CHECK(0 == d.Snap().idle_restart);
        const UInt64 stamp = d.Snap().cycle_mstamp_us;
        d.sk.app_limited = 0;
        d.ops->cwnd_event(&d.sk, xtcp::cc::kCaEventTxStart);
        s = d.Snap();
        CHECK(0 == s.idle_restart);
        CHECK(stamp == s.cycle_mstamp_us);
    }

    void TestKccCwndStartup() {
        // Same-burst ACK clock (no further round boundaries): STARTUP grows
        // additively while delivered_total < 10*mss or cwnd < target.
        Driver d(20000);
        CHECK(10 == d.sk.snd_cwnd);
        for (UInt32 i = 0; i < 10; ++i) {
            d.Ack(20000, 20000, kMss, 0, 0);
        }
        CHECK(19 == d.sk.snd_cwnd);
        CHECK(static_cast<s64>(d.sk.snd_cwnd) == kcc_core_cwnd_segs(&d.ref));
    }

    void TestKccCwndLossAndCap() {
        Driver d;
        // Kernel kcc_ssthresh is a PASS-THROUGH: it saves prior_cwnd and
        // returns the current snd_ssthresh without touching the window (the
        // cut itself is the FSM's cwnd = inflight + 1).
        const UInt32 prior = d.sk.snd_cwnd;
        const UInt32 hook_ssthresh = d.ops->ssthresh(&d.sk);
        CHECK(0x7FFFFFFFu == hook_ssthresh);
        CHECK(prior == d.sk.snd_cwnd);
        CHECK(prior == d.sk.prior_cwnd);
        // The FSM-side cut leaves the window at the reduced value; the growth
        // assertions below are unchanged.
        d.sk.snd_cwnd = 4;
        d.ref.cwnd_segs = 4;
        d.Ack(20000, 20000, kMss, 0, 0);
        CHECK(5 == d.sk.snd_cwnd);
        d.Ack(20000, 20000, kMss, 0, 0);
        CHECK(6 == d.sk.snd_cwnd);
        CHECK(static_cast<s64>(d.sk.snd_cwnd) == kcc_core_cwnd_segs(&d.ref));

        // Loss is deducted from the base before growth (kernel order).
        Driver e;
        e.Ack(20000, 20000, kMss, 2 * kMss, 0);
        CHECK(9 == e.sk.snd_cwnd);   // (10*MSS - 2920 + 1460) / MSS
        CHECK(static_cast<s64>(e.sk.snd_cwnd) == kcc_core_cwnd_segs(&e.ref));
    }

    /**
     * @brief Kernel kcc_ssthresh pass-through + kcc_undo_cwnd contract.
     */
    void TestKccSsthreshPassthroughAndUndo() {
        Driver d;
        d.sk.snd_cwnd = 20;
        d.sk.snd_ssthresh = 18;
        const UInt32 ret = d.ops->ssthresh(&d.sk);
        CHECK(18 == ret);            // returns the CURRENT ssthresh
        CHECK(20 == d.sk.snd_cwnd);  // never writes the window
        CHECK(20 == d.sk.prior_cwnd);
        CHECK(18 == d.sk.snd_ssthresh);  // unchanged by the hook

        // kcc_undo_cwnd returns the current window and clears full_bw.
        const KccSnapshot before = d.Snap();
        const UInt32 undo = d.ops->undo_cwnd(&d.sk);
        CHECK(20 == undo);
        CHECK(20 == d.sk.snd_cwnd);
        const KccSnapshot after = d.Snap();
        CHECK(0ull == after.full_bw);
        CHECK(0u == after.full_bw_cnt);
        (void)before;

        // set_state(CA_Loss) also clears full_bw.
        d.ops->set_state(&d.sk, 4);
        CHECK(0ull == d.Snap().full_bw);
    }


    /**
     * @brief RandBelow must be uniform (no modulo bias).
     *
     * 2^20 draws over 7 buckets through the production sampler: rejection
     * sampling gives every bucket the same probability, so each count must
     * land within 1% of 2^20/7. The old `SplitMix32 % 7` (a plain modulo over
     * a 2^32 range that is not a multiple of 7) skews the distribution and is
     * the red baseline this test pins.
     */
    void TestKccRandUnbiased() {
        constexpr UInt32 kDraws = 1u << 20;
        constexpr UInt32 kBuckets = 7;
        UInt32 counts[kBuckets] = {0};
        UInt32 state = 0x5EED1234u;
        for (UInt32 i = 0; i < kDraws; ++i) {
            const UInt32 r = xtcp::cc::KccTestRandBelow(&state, kBuckets);
            CHECK(r < kBuckets);
            ++counts[r];
        }
        const UInt32 expected = kDraws / kBuckets;
        UInt32 max_dev = 0;
        for (UInt32 b = 0; b < kBuckets; ++b) {
            const UInt32 dev = (counts[b] > expected) ? (counts[b] - expected)
                                                      : (expected - counts[b]);
            if (dev > max_dev) {
                max_dev = dev;
            }
        }
        CHECK(static_cast<UInt64>(max_dev) * 100 < static_cast<UInt64>(expected));
        std::fprintf(stderr, "rand buckets: %u %u %u %u %u %u %u (expected %u)\n",
                     counts[0], counts[1], counts[2], counts[3],
                     counts[4], counts[5], counts[6], expected);
        // n == 0 is a no-op that must not advance the state into garbage.
        CHECK(0u == xtcp::cc::KccTestRandBelow(&state, 0));
    }

    /**
     * @brief Production seeding must not be a predictable arithmetic
     *        progression (the old `seq * 2654435761 ^ 0x9E3779B9`).
     */
    void TestKccSeedUnpredictable() {
        xtcp::cc::KccTestResetRandom();   // production seeding
        Driver a;
        Driver b;
        Driver c;
        const UInt32 sa = a.Snap().rand_state;
        const UInt32 sb = b.Snap().rand_state;
        const UInt32 sc = c.Snap().rand_state;
        CHECK(sa != sb);
        CHECK(sb != sc);
        CHECK(sa != sc);
        // Not an arithmetic progression: the two deltas differ.
        const UInt32 d1 = sb - sa;
        const UInt32 d2 = sc - sb;
        CHECK(d1 != d2);
        // The counter component still decorrelates: seeds are not equal even
        // when the entropy repeats.
        CHECK(0 != sa || 0 != sb);
        xtcp::cc::KccTestSetRandomSeed(2);
    }
    /**
     * @brief Rejection BOUNDARY rows (a statistical test cannot see a
     *        1-in-6e8 off-by-one; these rows pin it exactly).
     *
     * For n = 7 the unbiased block is [0, 2^32 - 4) = [0, 4294967292).
     *   raw = limit - 1  -> accepted, bucket (limit-1) % 7
     *   raw = limit      -> REJECTED
     *   raw = 2^32 - 1   -> REJECTED
     * The old `0xFFFFFFFF - (0xFFFFFFFF % 7)` = 4294967292 with `r <= limit`
     * accepted limit itself, i.e. 4294967293 values - not a multiple of 7.
     */
    void TestKccRandRejectionBoundary() {
        constexpr UInt32 kN = 7;
        const UInt64 limit = xtcp::cc::KccTestRandLimit(kN);
        const UInt64 two32 = (UInt64(1) << 32);
        CHECK(limit == two32 - (two32 % kN));   // exact: 2^32 - 2^32 % n
        CHECK(0 == (limit % kN));               // a multiple of n
        CHECK(limit < two32);
        CHECK(two32 - limit == 4);              // 2^32 mod 7 == 4

        UInt32 bucket = 0xDEADBEEFu;
        CHECK(xtcp::cc::KccTestRandBelowFromRaw(limit - 1, kN, &bucket));
        CHECK(bucket == static_cast<UInt32>((limit - 1) % kN));
        // The exact boundary value must be REJECTED (the old code accepted it).
        CHECK(!xtcp::cc::KccTestRandBelowFromRaw(limit, kN, &bucket));
        CHECK(!xtcp::cc::KccTestRandBelowFromRaw(two32 - 1, kN, &bucket));
        // Accepted raws always map into range.
        for (UInt64 raw = 0; raw < limit; raw += 613566756ull) {
            CHECK(xtcp::cc::KccTestRandBelowFromRaw(raw, kN, &bucket));
            CHECK(bucket < kN);
        }
        // Degenerate bounds must terminate and be in range.
        CHECK(xtcp::cc::KccTestRandBelowFromRaw(0, 1, &bucket));
        CHECK(0u == bucket);
        CHECK(xtcp::cc::KccTestRandBelowFromRaw(0, 0, &bucket));
    }

    // ---- FSM-level tests: recorder CC + a real TcpConn ----
    RateSample g_recLast;
    UInt32 g_recCalls = 0;
    UInt32 g_recTxStart = 0;
    UInt32 g_recMarker = 0;
    Byte g_recCaState = 0;

    void RecInit(XtcpConnCc*) noexcept {}
    void RecRelease(XtcpConnCc*) noexcept {}
    void RecControl(XtcpConnCc* sk, const RateSample* rs) noexcept {
        g_recLast = *rs;
        g_recMarker = sk->app_limited;
        g_recCaState = sk->ca_state;
        ++g_recCalls;
    }
    void RecEvent(XtcpConnCc*, xtcp::cc::CaEvent ev) noexcept {
        if (xtcp::cc::kCaEventTxStart == ev) {
            ++g_recTxStart;
        }
    }

    void EnsureRecorderCc() {
        static bool registered = false;
        if (registered) {
            return;
        }
        XtcpCongestionOps ops;
        ops.name = "kcc_test_rec";
        ops.init = RecInit;
        ops.release = RecRelease;
        ops.cong_control = RecControl;
        ops.cwnd_event = RecEvent;
        CHECK(xtcp::cc::RegisterCongestionControl(ops));
        registered = true;
    }

    xtcp::core::Endpoint LocalEp() {
        xtcp::core::Endpoint e;
        e.family = 4;
        e.addr[0] = 0x0A000001;
        e.port = 443;
        return e;
    }
    xtcp::core::Endpoint RemoteEp() {
        xtcp::core::Endpoint e;
        e.family = 4;
        e.addr[0] = 0xC0A80102;
        e.port = 40000;
        return e;
    }

    std::vector<Byte> BuildSeg(UInt16 sport, UInt16 dport, UInt32 seq, UInt32 ack,
                               UInt16 flags, UInt16 window = 65535,
                               bool sack = false, UInt32 sl = 0, UInt32 sr = 0) {
        std::vector<Byte> seg(sack ? 32 : 20, 0);
        Byte* t = seg.data();
        t[0] = static_cast<Byte>(sport >> 8); t[1] = static_cast<Byte>(sport & 0xFF);
        t[2] = static_cast<Byte>(dport >> 8); t[3] = static_cast<Byte>(dport & 0xFF);
        t[4] = static_cast<Byte>(seq >> 24); t[5] = static_cast<Byte>(seq >> 16);
        t[6] = static_cast<Byte>(seq >> 8); t[7] = static_cast<Byte>(seq & 0xFF);
        t[8] = static_cast<Byte>(ack >> 24); t[9] = static_cast<Byte>(ack >> 16);
        t[10] = static_cast<Byte>(ack >> 8); t[11] = static_cast<Byte>(ack & 0xFF);
        t[12] = static_cast<Byte>(sack ? 0x80 : 0x50);
        t[13] = static_cast<Byte>(((flags & xtcp::core::kFlagFin) ? 0x01 : 0x00) |
                                  ((flags & xtcp::core::kFlagSyn) ? 0x02 : 0x00) |
                                  ((flags & xtcp::core::kFlagRst) ? 0x04 : 0x00) |
                                  ((flags & xtcp::core::kFlagPsh) ? 0x08 : 0x00) |
                                  ((flags & xtcp::core::kFlagAck) ? 0x10 : 0x00) |
                                  ((flags & xtcp::core::kFlagEce) ? 0x40 : 0x00));
        t[14] = static_cast<Byte>(window >> 8);
        t[15] = static_cast<Byte>(window & 0xFF);
        if (sack) {
            t[20] = 5;   // SACK kind
            t[21] = 10;  // length
            t[22] = static_cast<Byte>(sl >> 24); t[23] = static_cast<Byte>(sl >> 16);
            t[24] = static_cast<Byte>(sl >> 8);  t[25] = static_cast<Byte>(sl & 0xFF);
            t[26] = static_cast<Byte>(sr >> 24); t[27] = static_cast<Byte>(sr >> 16);
            t[28] = static_cast<Byte>(sr >> 8);  t[29] = static_cast<Byte>(sr & 0xFF);
            t[30] = 1; t[31] = 1;  // NOP padding
        }
        return seg;
    }

    struct FsmHarness {
        std::vector<xtcp::buf::BufRef> tx;
        xtcp::core::TcpConn conn;
        xtcp::core::TimePoint now = 1000;

        explicit FsmHarness(xtcp::core::TcpState state = xtcp::core::TcpState::kEstablished)
            : conn(state, LocalEp(), RemoteEp(), 100, 200,
                   [this](xtcp::buf::BufRef&& p) { tx.push_back(std::move(p)); }) {
            EnsureRecorderCc();
            conn.SetPeerMss(kMss);
            conn.SetCongestionControl("kcc_test_rec");
            g_recCalls = 0;
            g_recTxStart = 0;
            g_recMarker = 0;
        }

        void Send(UInt32 n) {
            // Size the payload buffer to the request: callers pass
            // multiples of the MSS (e.g. 4 * 1460 = 5840), which a fixed
            // 4096-byte stack array would overflow (ASan caught it).
            std::vector<Byte> buf(n, 0);
            CHECK(conn.SendData(buf.data(), n, now));
        }

        void Deliver(UInt32 ack, UInt16 flags = xtcp::core::kFlagAck,
                     UInt16 window = 65535, bool sack = false,
                     UInt32 sl = 0, UInt32 sr = 0, UInt32 seq = 201) {
            const std::vector<Byte> seg =
                BuildSeg(40000, 443, seq, ack, flags, window, sack, sl, sr);
            conn.OnSegment(seg.data(), static_cast<UInt32>(seg.size()), now);
        }

        void Warm() {
            Send(1460);
            now += 20000;
            Deliver(conn.SndNxt());
            CHECK(1 == g_recCalls);   // degenerate first sample
        }
    };

    void TestKccFsmDelayedFields() {
        {
            // Single sub-MSS segment, clean ACK: delayed.
            FsmHarness h;
            h.Warm();
            g_recCalls = 0;
            h.Send(100);
            h.now += 20000;
            h.Deliver(h.conn.SndNxt());
            CHECK(1 == g_recCalls);
            CHECK(g_recLast.is_ack_delayed);
            CHECK(1460ull == g_recLast.prior_delivered);
            CHECK(1560ull == g_recLast.delivered_total);
            CHECK(1u == g_recLast.is_app_limited);
            CHECK(0u == g_recMarker);   // cleared before the sample
        }
        {
            // Full MSS ACK: strict '< mss' fails.
            FsmHarness h;
            h.Warm();
            h.Send(1460);
            h.now += 20000;
            h.Deliver(h.conn.SndNxt());
            CHECK(!g_recLast.is_ack_delayed);
        }
        {
            // Two popped segments: pkts_acked != 1.
            FsmHarness h;
            h.Warm();
            h.Send(100);
            h.Send(100);
            h.now += 20000;
            h.Deliver(h.conn.SndNxt());
            CHECK(!g_recLast.is_ack_delayed);
        }
        {
            // RTO retransmission: TCPCB_RETRANS equivalent excludes it.
            FsmHarness h;
            h.Warm();
            h.Send(100);
            h.now += 2000000;
            h.conn.OnRetransmitTimer(h.now);
            h.now += 20000;
            h.Deliver(h.conn.SndNxt());
            CHECK(!g_recLast.is_ack_delayed);
        }
        {
            // ECE (FLAG_CA_ALERT).
            FsmHarness h;
            h.Warm();
            h.Send(100);
            h.now += 20000;
            h.Deliver(h.conn.SndNxt(),
                      static_cast<UInt16>(xtcp::core::kFlagAck | xtcp::core::kFlagEce));
            CHECK(!g_recLast.is_ack_delayed);
        }
        {
            // Any SACK block on the ACK.
            FsmHarness h;
            h.Warm();
            h.Send(100);
            h.now += 20000;
            const UInt32 end = h.conn.SndNxt();
            h.Deliver(end, xtcp::core::kFlagAck, 65535, true, end, end + 100);
            CHECK(!g_recLast.is_ack_delayed);
        }
        {
            // Entry-level prior_sacked: the segment that was SACKed earlier
            // is cumulatively ACKed by a SACK-less ACK.
            FsmHarness h;
            h.Warm();
            h.Send(100);
            const UInt32 mid = h.conn.SndNxt();
            h.Send(100);
            h.now += 20000;
            h.Deliver(mid, xtcp::core::kFlagAck, 65535, true, mid, mid + 100);
            h.now += 20000;
            h.Deliver(h.conn.SndNxt());
            CHECK(!g_recLast.is_ack_delayed);
        }
        {
            // TLP re-sends the tail without touching the retry budget.
            FsmHarness h;
            h.Warm();
            h.Send(100);
            h.now += 50000;
            h.conn.OnPoll(h.now);
            h.now += 20000;
            h.Deliver(h.conn.SndNxt());
            CHECK(!g_recLast.is_ack_delayed);
        }
        {
            // Persist re-probe of unacked data is a retransmission.
            FsmHarness h;
            h.Warm();
            h.Send(100);
            h.Deliver(h.conn.SndUna(), xtcp::core::kFlagAck, 0);  // zero window
            h.Send(100);                                          // arms persist
            h.now += 1100000;
            h.conn.OnPoll(h.now);
            h.now += 20000;
            h.Deliver(h.conn.SndNxt());
            CHECK(!g_recLast.is_ack_delayed);
        }
        {
            // Persist NEW byte (no unacked data): ordinary single-byte ACK.
            FsmHarness h;
            h.Warm();
            h.Deliver(h.conn.SndUna(), xtcp::core::kFlagAck, 0);  // zero window
            h.Send(100);                                          // buffered + persist armed
            h.now += 1100000;
            h.conn.OnPoll(h.now);                                 // consumes 1 new byte
            h.now += 20000;
            h.Deliver(h.conn.SndNxt());
            CHECK(g_recLast.is_ack_delayed);
        }
    }

    void TestKccFsmTxStartAndAppLimited() {
        FsmHarness h;
        CHECK(0 == g_recTxStart);
        h.Send(100);
        CHECK(1 == g_recTxStart);       // idle pipe -> TX_START once
        h.now += 20000;
        h.Deliver(h.conn.SndNxt());
        CHECK(g_recLast.is_app_limited);  // send-time snapshot of the marker
        // Kernel tcp_rate_gen clears with after(tp->delivered, app_limited).
        // The marker latched at the send entry is (0 + 0) encoded as 1, so a
        // single delivered packet (delivered == 1) does NOT clear it yet -
        // kernel semantics. The SECOND delivery passes it.
        CHECK(1u == g_recMarker);
        h.Send(100);
        h.now += 20000;
        h.Deliver(h.conn.SndNxt());
        CHECK(0u == g_recMarker);         // cleared once delivered passes it
        // The second send happened on an empty pipe (the first ACK emptied
        // it), so it is a fresh TX_START.
        CHECK(2 == g_recTxStart);
    }

    /**
     * @brief Every CC path must leave snd_cwnd a LEGAL window.
     *
     * Regression for the KCC fast-recovery entry: KccSsthresh passes the
     * current ssthresh through (0x7FFFFFFF, the "unlimited" sentinel), so
     * the RFC 5681 `cwnd = ssthresh + 3` inflation - which belongs to the
     * classic Reno path only - produced 0x80000002 (2^31+2). No kernel value
     * is legal there (tcp_snd_cwnd_clamp is <= 0x7FFFFFFF, and the kernel's
     * plugin path enters recovery through tcp_cwnd_reduction instead).
     * Observed red on test_reorder_gate: "cwnd 16->2147483650".
     */
    void TestKccFsmCwndStaysLegal() {
        xtcp::cc::RegisterKcc();
        std::vector<xtcp::buf::BufRef> tx;
        xtcp::core::TcpConn conn(
            xtcp::core::TcpState::kEstablished, LocalEp(), RemoteEp(), 100, 200,
            [&tx](xtcp::buf::BufRef&& p) { tx.push_back(std::move(p)); });
        conn.SetPeerMss(kMss);
        CHECK(conn.SetCongestionControl("kcc"));
        xtcp::core::TimePoint now = 1000;
        Byte data[8192];
        std::memset(data, 0x33, sizeof(data));
        CHECK(conn.SendData(data, sizeof(data), now));
        CHECK(conn.Cwnd() <= 0x7FFFFFFFu);

        // Three dup-ACKs whose SACK block reveals a REAL gap: fast recovery
        // fires (the reordering gate does not apply to confirmed gaps).
        const UInt32 una = conn.SndUna();
        for (UInt32 i = 0; i < 3; ++i) {
            const std::vector<Byte> ack =
                BuildSeg(40000, 443, 201, una, xtcp::core::kFlagAck, 65535, true,
                         una + 1460, una + 2920);
            conn.OnSegment(ack.data(), static_cast<UInt32>(ack.size()), now);
            CHECK(0 < conn.Cwnd());
            CHECK(conn.Cwnd() <= 0x7FFFFFFFu);   // red: 0x80000002 before the fix
        }

        // RTO path: the plugin branch keeps CutCwnd's packets_in_flight + 1
        // (the kernel tcp_enter_loss tail); it must never collapse to an
        // illegal or zero window either.
        now += 3000000;
        conn.OnRetransmitTimer(now);
        CHECK(0 < conn.Cwnd());
        CHECK(conn.Cwnd() <= 0x7FFFFFFFu);
    }

    /**
     * @brief The CA state must return to Open when a recovery episode ends.
     *
     * Kernel tcp_fastretrans_alert ends the episode once snd_una reaches
     * high_seq (tp->snd_nxt at the entry) and calls tcp_set_ca_state(
     * TCP_CA_Open) BEFORE tcp_cong_control (the CC hook). The port kept
     * ca_state at Recovery/Loss forever, so a state-keying CC (KCC's
     * kcc_set_cwnd_to_recover_or_restore) saw "Recovery entered" on every
     * later ACK and re-cut the window to inflight+acked - the window could
     * never recover. Red baseline: cwnd stayed 4 and ca_state stayed 3
     * (observed as test_fastrec_undo "cwnd_before <= cwnd_after" on CI).
     */
    void TestKccFsmCaStateReturnsToOpen() {
        FsmHarness h;
        h.Send(kMss);
        h.Send(kMss);
        h.Send(kMss);
        h.Send(kMss);
        const UInt32 una = h.conn.SndUna();
        const UInt32 end = h.conn.SndNxt();
        const UInt32 cwnd_before = h.conn.Cwnd();
        // Three RAW dup-ACKs (no SACK blocks): no-SACK fast-recovery entry.
        for (UInt32 i = 0; i < 3; ++i) {
            h.now += 1000;
            h.Deliver(una, xtcp::core::kFlagAck);
        }
        CHECK(h.conn.FastRecovery());          // recovery active
        // The cumulative ACK covers everything up to high_seq: the episode
        // ends and the state must be Open when the CC hook runs.
        g_recCalls = 0;
        g_recCaState = 0xFF;
        h.now += 20000;
        h.Deliver(end);
        CHECK(1 == g_recCalls);
        CHECK(0u == g_recCaState);             // red: 3 (Recovery) before the fix
        CHECK(!h.conn.FastRecovery());
        CHECK(cwnd_before <= h.conn.Cwnd());   // window not re-cut by the CC
    }

    /**
     * @brief PipeInFlightBytes must subtract LOST bytes too.
     *
     * Kernel tcp_packets_in_flight() = packets_out - (sacked_out + lost_out)
     * + retrans_out (tcp_h_v66.h:1198-1220). The earlier port subtracted only
     * sacked, so a segment declared lost but not yet retransmitted kept
     * occupying the pipe: a fully-lost flight never looked empty and
     * CA_EVENT_TX_START was suppressed on the retransmit that restarted it.
     *
     * Red baseline: with one 1460-byte segment marked lost (retries == 0,
     * no SACK), the old formula returned 1460 (not 0) and no TX_START fired.
     */
    void TestKccPipeExcludesLost() {
        // Fully-lost flight, retransmitted: the TX_START gate must see the
        // pre-retransmission pipe (kernel tcp_event_data_sent runs INSIDE
        // __tcp_transmit_skb, i.e. before retrans_out += pcount), which is 0
        // once lost_out is subtracted.
        FsmHarness h;
        h.Send(1460);
        CHECK(1 == g_recTxStart);          // first data on an idle pipe
        // RTO marks the head lost (seg.lost) and then retransmits it. With
        // the old formula (sacked only) the pipe still read 1460 here, so
        // TX_START was suppressed and the retransmit could not restart the
        // congestion state.
        h.now += 3000000;
        h.conn.OnRetransmitTimer(h.now);
        CHECK(2 == g_recTxStart);          // lost excluded -> pipe was empty

        // The retransmitted segment is in flight again (retrans_out), so a
        // following send on the same pipe must NOT be another TX_START.
        h.Send(1460);
        CHECK(2 == g_recTxStart);
    }

    /**
     * @brief TLP retransmission must dispatch CA_EVENT_TX_START when the
     *        pipe it leaves is EMPTY, and must not when it is not.
     *
     * Kernel tcp_event_data_sent runs inside __tcp_transmit_skb for EVERY
     * data skb, the TLP probe included (tcp_output_v66.c:1391-1395), and
     * dispatches CA_EVENT_TX_START only when tcp_packets_in_flight()==0
     * (:167-168). With lost_out subtracted (tcp_h_v66.h:1198-1220), a tail
     * that is fully SACKed or lost leaves the pipe at 0 - so the probe is a
     * TX_START. A live tail (still in flight) is not.
     */
    void TestKccTxStartOnTlp() {
        {
            // Empty pipe at the probe: the only outstanding segment is fully
            // SACKed by a dup-ACK, so PipeInFlightBytes() == 0.
            FsmHarness h;
            h.Warm();                       // srtt_ > 0 -> TLP arms on send
            h.Send(kMss);
            const UInt32 before = g_recTxStart;   // after this send's own TX_START
            const UInt32 una = h.conn.SndUna();
            // Duplicate ACK (frontier unchanged) fully SACKing that segment.
            h.now += 1000;
            h.Deliver(una, xtcp::core::kFlagAck, 65535, true, una, una + kMss);
            // TLP deadline is now + 2*srtt (>= 10ms); the RTO floor is 200ms.
            h.now += 60000;
            h.conn.OnPoll(h.now);
            CHECK(before + 1 == g_recTxStart);   // probe on an empty pipe
        }
        {
            // Non-empty pipe: the same probe must NOT dispatch TX_START.
            FsmHarness h;
            h.Warm();
            h.Send(kMss);                        // still in flight, nothing SACKed
            const UInt32 before = g_recTxStart;   // after this send's own TX_START
            h.now += 60000;                      // past 2*srtt, before the RTO
            h.conn.OnPoll(h.now);
            CHECK(before == g_recTxStart);       // pipe non-empty: no TX_START
        }
    }

    /**
     * @brief RFC 2883 DSACK must count dup_segs into delivered_pkts EXACTLY
     *        once (kernel tcp_dsack_seen -> state->sack_delivered).
     *
     * Sequence: 3 MSS segments; an RTO retransmits the head (so the sender
     * HAS retransmitted - the kernel gate at tcp_input.c:1027-1032 rejects a
     * DSACK when dsack_dups > total_retrans, and a DSACK is by definition
     * the receiver reporting a SPURIOUS retransmission); a cumulative ACK
     * covering that first segment; then a duplicate ACK at the SAME frontier
     * whose DSACK block covers the just-consumed segment (already popped, so
     * only the dup_segs term can credit it); then a SACK block over the
     * remaining two; then a cumulative ACK covering everything.
     *
     * Expected delivered_total_pkts = 1 (cum-ACK) + 1 (DSACK dup_segs)
     * + 2 (SACK) = 4. Five would mean the DSACKed range was counted twice
     * (dup_segs AND the SACK walk); three would mean the DSACK term was
     * dropped. The dup-ACK path must also EMIT a rate sample (kernel
     * tcp_rate_gen runs on every ACK, tcp_input.c:3939), so g_recCalls
     * advances on each of the four ACKs.
     */
    void TestKccDsackCountsDelivered() {
        FsmHarness h;
        h.Send(kMss);
        h.Send(kMss);
        h.Send(kMss);
        const UInt32 una = h.conn.SndUna();
        const UInt32 end = h.conn.SndNxt();
        CHECK(end == una + 3 * kMss);

        // RTO: mark the head lost and retransmit it. total_retrans becomes 1,
        // which is what makes the later DSACK credible to the kernel gate.
        h.now += 3000000;
        h.conn.OnRetransmitTimer(h.now);

        // Cumulative ACK covering the first (retransmitted) segment. This is
        // the connection's first rate sample, so it is degenerate
        // (delivered_pkts == 0) while delivered_total_pkts is already 1.
        g_recCalls = 0;
        h.now += 20000;
        h.Deliver(una + kMss);
        CHECK(1 == g_recCalls);
        CHECK(1ull == g_recLast.delivered_total_pkts);
        CHECK(0u == g_recLast.delivered_pkts);

        // Duplicate ACK at the same frontier with a DSACK block covering the
        // segment that was just cumulatively ACKed. The kernel emits a rate
        // sample for this ACK too (tcp_rate_gen on every ACK).
        h.now += 20000;
        h.Deliver(una + kMss, xtcp::core::kFlagAck, 65535, true,
                  una, una + kMss);
        CHECK(2 == g_recCalls);
        CHECK(2ull == g_recLast.delivered_total_pkts);   // +1 dup_segs
        CHECK(1u == g_recLast.delivered_pkts);           // exactly one

        // SACK the two remaining segments (+2).
        h.now += 20000;
        h.Deliver(una + kMss, xtcp::core::kFlagAck, 65535, true,
                  una + kMss, end);
        CHECK(3 == g_recCalls);
        CHECK(4ull == g_recLast.delivered_total_pkts);   // +2 (SACK)
        CHECK(2u == g_recLast.delivered_pkts);

        // Cumulative ACK covering everything: the two SACKed segments are
        // already counted, so nothing is added (not 5).
        h.now += 20000;
        h.Deliver(end);
        CHECK(4 == g_recCalls);
        CHECK(4ull == g_recLast.delivered_total_pkts);   // not 5
        CHECK(0u == g_recLast.delivered_pkts);
    }

    /**
     * @brief The PROBE_BW cycle-phase decision must read rs->prior_in_flight_bytes
     *        (the pre-ACK snapshot), never the post-ACK sk->inflight.
     *
     * Kernel kcc_is_next_cycle_phase calls
     * kcc_packets_in_net_at_edt(sk, rs->prior_in_flight)
     * (tcp_kcc_c8bead4.c:5049-5056). The two values differ by exactly the
     * bytes this ACK cleaned, so a BDP target placed between them flips the
     * decision. Both directions are asserted: with the pre-ACK value above
     * the target the phase must end, and with the post-ACK value above the
     * target (pre-ACK below) it must NOT end.
     */
    void TestKccCycleUsesPriorInflight() {
        Driver d(0, 0, 2);            // seed 2 -> first PROBE_BW phase is 0 (gain 1.25x)
        const KccSnapshot probe = ForceProbe(d);
        CHECK(1 == probe.mode);       // PROBE_BW
        CHECK(0 == probe.cycle_idx);  // probe-up phase (gain 320)

        // Target BDP for the probe-up phase, in bytes (same helper the
        // implementation uses).
        const UInt64 target = kcc_bdp_packets(probe.max_bw, probe.min_rtt_us, 320) * kMss;
        CHECK(0 < target);

        RateSample rs;
        rs.pacing_edt_us = 0;
        const UInt64 after_full_length =
            probe.cycle_mstamp_us + probe.min_rtt_us + 1000;

        // (a) pre-ACK in flight ABOVE the target, post-ACK below it:
        // the phase must end (decision follows prior_in_flight).
        rs.prior_in_flight_bytes = target + kMss;
        d.sk.inflight = 0;
        CHECK(1u == xtcp::cc::KccTestIsNextCyclePhase(&d.sk, &rs, after_full_length));

        // (b) mirrored: pre-ACK BELOW the target, post-ACK above it.
        // The phase must NOT end - if the implementation read sk->inflight
        // this row would flip to 1 and catch the regression.
        rs.prior_in_flight_bytes = 0;
        d.sk.inflight = target + kMss;
        CHECK(0u == xtcp::cc::KccTestIsNextCyclePhase(&d.sk, &rs, after_full_length));

        // (c) both above the target: end (sanity that the target is real).
        rs.prior_in_flight_bytes = target + kMss;
        d.sk.inflight = target + kMss;
        CHECK(1u == xtcp::cc::KccTestIsNextCyclePhase(&d.sk, &rs, after_full_length));
    }

    void TestKccFsmTfoRequeue() {
        FsmHarness h(xtcp::core::TcpState::kSynSent);
        const Byte cookie[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        h.conn.SetTfoCookie(cookie);
        Byte data[100];
        std::memset(data, 0, sizeof(data));
        h.now = 1000;
        CHECK(h.conn.SendSynWithData(data, sizeof(data)));
        CHECK(201 == h.conn.SndNxt());  // iss+1 + 100 early bytes

        // SYN+ACK acknowledging only the SYN: TFO requeues the early bytes.
        h.now = 20000;
        const std::vector<Byte> synack =
            BuildSeg(40000, 443, 200, 101,
                     static_cast<UInt16>(xtcp::core::kFlagSyn | xtcp::core::kFlagAck));
        h.conn.OnSegment(synack.data(), static_cast<UInt32>(synack.size()), h.now);
        CHECK(xtcp::core::TcpState::kEstablished == h.conn.State());

        h.now = 40000;
        h.Deliver(201);
        CHECK(100ull == g_recLast.delivered_total);
        CHECK(0ull == g_recLast.prior_delivered);
        CHECK(!g_recLast.is_ack_delayed);  // requeued early data is a retransmit
    }

    void TestKccMigrationEpoch() {
        FsmHarness h1;
        h1.Send(100);
        h1.now += 20000;
        h1.Deliver(h1.conn.SndNxt());
        CHECK(100ull == g_recLast.delivered_total);
        h1.Send(100);   // sent_delivered = 100
        const xtcp::core::TcpConn::ConnCheckpoint ckpt = h1.conn.MakeCheckpoint();
        CHECK(1 == ckpt.retrans_queue.size());
        CHECK(0ull == ckpt.retrans_queue[0].sent_delivered);
        CHECK(0 == ckpt.retrans_queue[0].app_limited);
        CHECK(0 == ckpt.retrans_queue[0].is_retrans);

        std::vector<xtcp::buf::BufRef> tx2;
        xtcp::core::TcpConn conn2(xtcp::core::TcpState::kClosed, LocalEp(), RemoteEp(),
                                  100, 200,
                                  [&tx2](xtcp::buf::BufRef&& p) { tx2.push_back(std::move(p)); });
        EnsureRecorderCc();
        conn2.SetPeerMss(kMss);
        conn2.SetCongestionControl("kcc_test_rec");
        conn2.ApplyCheckpoint(ckpt);

        g_recCalls = 0;
        Byte buf[100];
        std::memset(buf, 0, sizeof(buf));
        const xtcp::core::TimePoint now2 = 50000;
        // The restored segment is in flight; new data rides behind it.
        CHECK(conn2.SendData(buf, sizeof(buf), now2));
        const std::vector<Byte> ack =
            BuildSeg(40000, 443, 201, 301, xtcp::core::kFlagAck);  // ACK old segment only
        conn2.OnSegment(ack.data(), static_cast<UInt32>(ack.size()), now2 + 20000);
        CHECK(1 == g_recCalls);
        // New epoch: delivered_total restarted at 0 (not 100+100) and the
        // segment snapshot was not carried across the migration.
        CHECK(100ull == g_recLast.delivered_total);
        CHECK(0ull == g_recLast.prior_delivered);
    }

    /**
     * @brief Short segments (< MSS) count as ONE kernel packet each.
     *
     * Ten 1024-byte segments, one delivered per 20 ms sample: the kernel's
     * tcp_set_skb_tso_segs assigns pcount = 1 to every sub-MSS skb, so each
     * sample reports bw = (1 << 24) / 20000 = 838. The old byte-domain port
     * reported (1024 << 24) / 20000 = 858993 (i.e. 1024/1460 of the kernel
     * value) and only 1024 delivered bytes per sample.
     */
    void TestKccShortSegPacketDomain() {
        Driver d;
        constexpr UInt32 kShort = 1024;
        for (UInt32 i = 0; i < 10; ++i) {
            d.Ack(20000, 20000, kShort, 0, 0);
        }
        const KccSnapshot s = d.Snap();
        const UInt64 expected = (UInt64(1) << 24) / 20000;   // 838
        CHECK(expected == s.max_bw);
        CHECK(858993ull != s.max_bw);   // the old byte-domain value
        CHECK(10ull == s.total_delivered);   // packets, not bytes
    }

    /**
     * @brief Kernel tcp_rate_check_app_limited truth table
     *        (tcp_rate_v66.c:194-207): pending < mss, tx queue idle,
     *        inflight_pkts < cwnd, lost_out <= retrans_out.
     */
    void TestKccAppLimitedTruthTable() {
        using xtcp::cc::IsAppLimited;
        // Baseline: all four conditions hold -> app-limited.
        CHECK(IsAppLimited(0, true, 3, 10, 0, 0, kMss));
        // 1. Pending bytes >= one MSS -> not app-limited.
        CHECK(!IsAppLimited(kMss, true, 3, 10, 0, 0, kMss));
        CHECK(!IsAppLimited(kMss + 1, true, 3, 10, 0, 0, kMss));
        // 2. Host tx queue non-empty -> not app-limited.
        CHECK(!IsAppLimited(0, false, 3, 10, 0, 0, kMss));
        // 3. Inflight >= cwnd (window-limited) -> not app-limited.
        CHECK(!IsAppLimited(0, true, 10, 10, 0, 0, kMss));
        CHECK(!IsAppLimited(0, true, 11, 10, 0, 0, kMss));
        // 4a. A loss outstanding (lost_out > retrans_out) -> not app-limited.
        CHECK(!IsAppLimited(0, true, 3, 10, 2, 0, kMss));
        CHECK(!IsAppLimited(0, true, 3, 10, 3, 2, kMss));
        // Boundary rows: pending == mss-1 still counts as "less than one
        // segment"; inflight == cwnd-1 still fits the window.
        CHECK(IsAppLimited(kMss - 1, true, 3, 10, 0, 0, kMss));
        CHECK(IsAppLimited(0, true, 9, 10, 0, 0, kMss));
        // 4b. RED ROW: every lost packet has been retransmitted
        //     (lost_out == retrans_out) -> app-limited, unlike the old
        //     "cc_.lost == 0" test which stayed conservative forever.
        CHECK(IsAppLimited(0, true, 3, 10, 2, 2, kMss));
        CHECK(IsAppLimited(0, true, 3, 10, 5, 7, kMss));
        // MSS 0 cannot classify anything.
        CHECK(!IsAppLimited(0, true, 3, 10, 0, 0, 0));
    }

    /**
     * @brief FSM-level: a fully retransmitted loss still marks the send
     *        app-limited (kernel lost_out <= retrans_out), while a lost
     *        packet that is NOT retransmitted does not.
     */
    void TestKccFsmAppLimitedLostRetransmitted() {
        {
            // Zero-window persist re-probe retransmits the lost byte, so
            // lost_out == retrans_out afterwards: a later send on the idle
            // pipe must latch app_limited.
            FsmHarness h;
            h.Warm();
            h.Send(100);
            h.Deliver(h.conn.SndUna(), xtcp::core::kFlagAck, 0);  // zero window
            h.Send(100);                                          // arms persist
            h.now += 1100000;
            h.conn.OnPoll(h.now);                                 // re-probe (retransmit)
            g_recMarker = 0;
            h.now += 20000;
            h.Deliver(h.conn.SndNxt());   // ACK clears the loss bookkeeping
            h.Send(100);
            h.now += 20000;
            h.Deliver(h.conn.SndNxt());
            CHECK(1u == g_recLast.is_app_limited);
        }
        {
            // Fresh idle pipe, nothing lost: app-limited.
            FsmHarness h;
            h.Warm();
            h.Send(100);
            h.now += 20000;
            h.Deliver(h.conn.SndNxt());
            CHECK(1u == g_recLast.is_app_limited);
        }
        {
            // Buffered backlog (pending >= mss): never app-limited.
            FsmHarness h;
            h.Warm();
            h.conn.SetSndBuf(64 * 1024);
            h.Deliver(h.conn.SndUna(), xtcp::core::kFlagAck, 0);  // zero window
            const UInt32 calls_before = g_recCalls;
            h.Send(4 * kMss);   // all buffered behind the closed window
            // The buffered path emits no data, so it produces no rate
            // sample either: nothing is app-limited about a full backlog.
            CHECK(calls_before == g_recCalls);
        }
    }

    /**
     * @brief MSS multiples: the packet-domain estimate is exactly the
     *        byte-domain rate divided by MSS (both floors), so scaling the
     *        packet rate by MSS reproduces the byte rate up to the floor
     *        error of one segment-rate unit (< MSS bytes/us<<24).
     */
    void TestKccMssMultipleEquivalence() {
        for (UInt32 k = 1; k <= 8; ++k) {
            Driver d;
            const UInt32 delivered_bytes = k * kMss;
            d.Ack(20000, 20000, delivered_bytes, 0, 0);
            const KccSnapshot s = d.Snap();
            const UInt64 pkts = (UInt64(k) << 24) / 20000;
            const UInt64 bytes = (UInt64(delivered_bytes) << 24) / 20000;
            CHECK(pkts == s.max_bw);
            // |pkts*mss - bytes| < mss: the only difference is the floor of
            // the packet-domain division (exact when mss divides the product).
            const UInt64 scaled = pkts * kMss;
            const UInt64 diff = (scaled > bytes) ? (scaled - bytes) : (bytes - scaled);
            CHECK(diff < kMss);
        }
    }

    /**
     * @brief Kernel tcp_count_delivered must not charge SACKed bytes twice.
     *
     * Three MSS segments: a duplicate ACK SACKs the second and third
     * (delivered += 2), then the cumulative ACK covers all three. Kernel
     * tcp_clean_rtx_queue (tcp_input.c:3312-3315) skips tcp_count_delivered
     * for skbs tagged SACKED_ACKED, so only the first segment is counted
     * again: total = 1 (warm-up) + 2 (SACK) + 1 (cum-ACK) = 4. Counting the
     * full pkt_count at the pop would report 6.
     */
    void TestKccSackDeliveredNoDoubleCount() {
        FsmHarness h;
        h.Warm();                    // delivered_pkts = 1
        h.Send(kMss);
        h.Send(kMss);
        h.Send(kMss);
        const UInt32 una = h.conn.SndUna();
        const UInt32 end = h.conn.SndNxt();
        h.now += 20000;
        // Duplicate ACK (frontier unchanged) SACKing segments 2 and 3.
        h.Deliver(una, xtcp::core::kFlagAck, 65535, true, una + kMss, end);
        g_recCalls = 0;
        h.now += 20000;
        // Cumulative ACK covering all three (two of them already SACKed).
        h.Deliver(end);
        CHECK(1 == g_recCalls);
        CHECK(4ull == g_recLast.delivered_total_pkts);
        CHECK(6ull != g_recLast.delivered_total_pkts);
    }

    /**
     * @brief Recovery-state differential: ca_state / prev_ca_state /
     *        prior_cwnd / packet_conservation must match the ref core.
     *
     * Before the ref block existed the reference left all four at 0, so a
     * recovery-transition test could not compare anything: the port could
     * diverge (cwnd growth during packet conservation, missing
     * Recovery->Open restore) and the differential stayed green.
     */
    void TestKccRecoveryDifferential() {
        Driver d(0, 0, 2);
        for (UInt32 i = 0; i < 10; ++i) {
            d.Round(20000);
        }
        // Enter Recovery (kernel CA_Recovery = 3).
        d.SetPriorCwnd(d.sk.snd_cwnd);
        d.SetCaState(3);
        for (UInt32 i = 0; i < 20; ++i) {
            d.Round(20000);
            CheckFullSnapshot(d);
            CHECK(1u == d.Snap().packet_conservation);   // conservation latched
            CHECK(kcc_core_cwnd_segs(&d.ref) == static_cast<s64>(d.sk.snd_cwnd));
        }
        // Back to Open: cwnd = max(cwnd, prior_cwnd), conservation cleared.
        d.SetCaState(0);
        for (UInt32 i = 0; i < 20; ++i) {
            d.Round(20000);
            CheckFullSnapshot(d);
            CHECK(0u == d.Snap().packet_conservation);
            CHECK(kcc_core_cwnd_segs(&d.ref) == static_cast<s64>(d.sk.snd_cwnd));
        }
        // CA_Loss (4) behaves like Recovery for the port's conservation gate.
        d.SetCaState(4);
        for (UInt32 i = 0; i < 10; ++i) {
            d.Round(20000);
            CheckFullSnapshot(d);
            CHECK(kcc_core_cwnd_segs(&d.ref) == static_cast<s64>(d.sk.snd_cwnd));
        }
        CHECK(0u == g_failures);
    }

    void TestKccDifferentialNewFields() {
        Driver d(0, 0, 2);
        UInt32 seed = 0x12345678;
        const auto next = [&seed]() -> UInt32 {
            seed = seed * 1664525u + 1013904223u;
            return seed;
        };
        for (UInt32 i = 0; i < 3000; ++i) {
            UInt32 rtt_us = 20000 + (i % 1000) * 3 + (next() % 2000);
            if (i < 200) {
                rtt_us = 30000 - i * 5;
            }
            const UInt32 delivered = (1 + (next() % 4)) * kMss;
            const UInt32 lost = (0 == (i % 700)) ? delivered : 0;
            const UInt32 interval_us = 10000 + (next() % 5000);
            UInt64 prior;
            if (i < 500) {
                prior = d.total;             // ACK-clocked rounds
            } else if (0 == (i % 3)) {
                prior = d.total;             // explicit boundary
            } else if (1 == (i % 3)) {
                prior = 0;                   // same burst
            } else {
                prior = d.total / 2;         // stale snapshot (no round)
            }
            const bool delayed = (0 == (i % 11));
            const bool app = (0 == (i % 17));
            // EDT credit coverage: a third of the samples carry a pacing-queue
            // depth, and it spans the zero-credit / partial / full-credit rows.
            d.edt_us = (0 == (i % 3)) ? (next() % 4000) : 0;
            d.Ack(rtt_us, interval_us, delivered, lost, prior, delayed, app);
            CheckFullSnapshot(d);
            CHECK(kcc_core_cwnd_segs(&d.ref) == static_cast<s64>(d.sk.snd_cwnd));
            CHECK(kcc_core_pacing_rate(&d.ref) == static_cast<s64>(d.sk.pacing_rate));
            if (0 < g_failures) {
                std::fprintf(stderr, "new-field differential mismatch at step %u\n", i);
                break;
            }
        }
    }
}

int main() {
    xtcp::buf::InitPools();
    xtcp::cc::KccTestSetRandomSeed(2);
    TestKccRegistry();
    TestKccDifferential();
    TestKccRoundBoundaryTracksFlight();
    TestKccRoundBoundaryTracksCurrentRtt();
    TestKccFullBwTransition();
    TestKccG3PathIncreasePerRound();
    TestKccG3PerRoundEvidence();
    TestKccLockRelease();
    TestKccLongPathOutlierDoesNotLock();
    TestKccStickyAndFastFall();
    TestKccSrttGuard();
    TestKccG2PerRound();
    TestKccQdelayBaseline();
    TestKccStalenessDownwardOnly();
    TestKccFullBwThresholdMatchesKernel();
    TestKccProbeCycleAdvances();
    TestKccPacingExtremeClamp();
    TestKccBdpSaturation();
    TestKccPacingUnitGrid();
    TestKccBdpConfirmedBaseline();
    TestKccColdStart();
    TestKccDeliveredRound();
    TestKccDelayedAckFiltered();
    TestKccDelayedAckTruthTable();
    TestKccDrainExit();
    TestKccDrainEdtCredit();
    TestKccCycleRandomized();
    TestKccRandUnbiased();
    TestKccRandRejectionBoundary();
    TestKccSeedUnpredictable();
    TestKccIdleRestart();
    TestKccCwndStartup();
    TestKccCwndLossAndCap();
    TestKccSsthreshPassthroughAndUndo();
    TestKccAppLimitedTruthTable();
    TestKccFsmAppLimitedLostRetransmitted();
    TestKccFsmDelayedFields();
    TestKccFsmTxStartAndAppLimited();
    TestKccFsmCwndStaysLegal();
    TestKccFsmCaStateReturnsToOpen();
    TestKccPipeExcludesLost();
    TestKccTxStartOnTlp();
    TestKccDsackCountsDelivered();
    TestKccCycleUsesPriorInflight();
    TestKccFsmTfoRequeue();
    TestKccMigrationEpoch();
    TestKccShortSegPacketDomain();
    TestKccMssMultipleEquivalence();
    TestKccSackDeliveredNoDoubleCount();
    TestKccDifferentialNewFields();
    TestKccRecoveryDifferential();
    xtcp::cc::KccTestResetRandom();
    if (0 < g_failures) {
        std::fprintf(stderr, "test_cc_kcc: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("test_cc_kcc: all passed\n");
    return 0;
}