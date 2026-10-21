/**
 * @file test_ecn_cc.cpp
 * @brief ECN-CE -> CC dispatch (CC audit M5): with a congestion-control
 *        plugin installed, a CE-marked round must reduce cwnd through the
 *        plugin's contract (cwnd_event(kCaEventEcnCe) + ssthresh hook),
 *        not the inline Reno half-cut. CUBIC cuts by beta = 0.7; the
 *        pre-fix core halved the window and left CUBIC's state
 *        inconsistent.
 */

#include <xtcp/core/stack.h>
#include <xtcp/cc/cc.h>
#include <xtcp/ndi/manual.h>
#include "harness/raw_pkt.h"

#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

static int g_failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                 \
        }                                                                \
    } while (0)

static void Pump(xtcp::ndi::ManualBackend& a, xtcp::ndi::ManualBackend& b,
                 xtcp::XtcpStack& sa, xtcp::XtcpStack& sb) {
    Byte out[65536];
    for (UInt32 round = 0; round < 2000; ++round) {
        bool moved = false;
        while (0 != a.TxPending()) {
            const UInt32 n = a.PollTx(out);
            if (0 < n) {
                b.Inject(out, n, 0x0800);
                moved = true;
            }
        }
        while (0 != b.TxPending()) {
            const UInt32 n = b.PollTx(out);
            if (0 < n) {
                a.Inject(out, n, 0x0800);
                moved = true;
            }
        }
        sa.PollAckTimers();
        sb.PollAckTimers();
        if (!moved) {
            return;
        }
    }
}

static void ConnCwnd(xtcp::XtcpStack& s, UInt64 id, UInt32& cwnd) {
    UInt32 inflight, ssthresh, snd_wnd, retx, dup_acks, fast_rec;
    UInt64 rto_deadline;
    UInt32 front_seq, snd_una;
    UInt16 lp, rp;
    s.ConnStats(id, inflight, cwnd, ssthresh, snd_wnd, retx, rto_deadline,
                dup_acks, fast_rec, front_seq, snd_una, lp, rp);
}

// ---- FSM-level ECN tests (batch 1: D02/D13) ----
static constexpr UInt32 kMss = 1460;

static void Put32(Byte* p, UInt32 v) {
    p[0] = static_cast<Byte>(v >> 24);
    p[1] = static_cast<Byte>(v >> 16);
    p[2] = static_cast<Byte>(v >> 8);
    p[3] = static_cast<Byte>(v & 0xFF);
}

/** Raw IPv4-less TCP segment (TcpConn::OnSegment takes the TCP payload). */
static std::vector<Byte> Seg(UInt32 seq, UInt32 ack, UInt16 flags, bool sack = false,
                             UInt32 sl = 0, UInt32 sr = 0, bool mss_opt = false) {
    const UInt32 opt = (sack ? 12u : 0u) + (mss_opt ? 4u : 0u);
    std::vector<Byte> seg(20 + opt, 0);
    Byte* t = seg.data();
    t[0] = 0x9C; t[1] = 0x40;   // sport 40000
    t[2] = 0x01; t[3] = 0xBB;   // dport 443
    Put32(t + 4, seq);
    Put32(t + 8, ack);
    t[12] = static_cast<Byte>(((20 + opt) / 4) << 4);
    t[13] = static_cast<Byte>(flags);
    t[14] = 0xFF; t[15] = 0xFF;
    UInt32 o = 20;
    if (mss_opt) {
        t[o] = 2; t[o + 1] = 4;
        t[o + 2] = static_cast<Byte>(kMss >> 8); t[o + 3] = static_cast<Byte>(kMss & 0xFF);
        o += 4;
    }
    if (sack) {
        t[o] = 5; t[o + 1] = 10;
        Put32(t + o + 2, sl);
        Put32(t + o + 6, sr);
        t[o + 10] = 1; t[o + 11] = 1;
    }
    return seg;
}

static xtcp::core::Endpoint EcnLocalEp() {
    xtcp::core::Endpoint e;
    e.family = 4;
    e.addr[0] = 0x0A000001;
    e.port = 443;
    return e;
}

static xtcp::core::Endpoint EcnRemoteEp() {
    xtcp::core::Endpoint e;
    e.family = 4;
    e.addr[0] = 0xC0A80102;
    e.port = 40000;
    return e;
}

/** Established ECN connection (real SYN / SYN+ACK|ECE handshake). */
struct EcnHarness {
    std::vector<xtcp::buf::BufRef> tx;
    xtcp::core::TcpConn conn;
    xtcp::core::TimePoint now = 1000;

    EcnHarness()
        : conn(xtcp::core::TcpState::kSynSent, EcnLocalEp(), EcnRemoteEp(), 100, 200,
               [this](xtcp::buf::BufRef&& p) { tx.push_back(std::move(p)); }) {
        conn.SetEcnRequested(true);
        conn.SetSackOk(true);
        conn.SendSyn();
        const std::vector<Byte> synack = Seg(500, 101,
            static_cast<UInt16>(xtcp::core::kFlagSyn | xtcp::core::kFlagAck |
                                xtcp::core::kFlagEce), false, 0, 0, true);
        conn.OnSegment(synack.data(), static_cast<UInt32>(synack.size()), now);
        conn.SetSndBuf(4u << 20);
    }

    void Send(UInt32 n) {
        std::vector<Byte> buf(n, 0x41);
        CHECK(conn.SendData(buf.data(), n, now));
    }
    void Deliver(UInt32 ack, UInt16 flags = xtcp::core::kFlagAck, bool sack = false,
                 UInt32 sl = 0, UInt32 sr = 0) {
        const std::vector<Byte> seg = Seg(201, ack, flags, sack, sl, sr);
        conn.OnSegment(seg.data(), static_cast<UInt32>(seg.size()), now);
    }
    void Warmup(UInt32 rounds) {
        for (UInt32 r = 0; r < rounds; ++r) {
            const UInt32 w = conn.Cwnd();
            Send(w * kMss);
            now += 20000;
            Deliver(conn.SndNxt());
        }
    }
};

static Byte g_ecnState = 0xFF;
static UInt32 g_ecnCalls = 0;

static void EcnProbeSsthreshInit(xtcp::cc::XtcpConnCc*) {}
static void EcnProbeSsthreshRelease(xtcp::cc::XtcpConnCc*) {}
static UInt32 EcnProbeSsthresh(xtcp::cc::XtcpConnCc* sk) { return sk->snd_ssthresh; }
static void EcnProbeControl(xtcp::cc::XtcpConnCc* sk, const xtcp::cc::RateSample*) {
    g_ecnState = sk->ca_state;
    ++g_ecnCalls;
}

static void EnsureEcnProbeCc() {
    static bool registered = false;
    if (registered) {
        return;
    }
    xtcp::cc::XtcpCongestionOps ops;
    ops.name = "ecn_cc_probe";
    ops.init = EcnProbeSsthreshInit;
    ops.release = EcnProbeSsthreshRelease;
    ops.ssthresh = EcnProbeSsthresh;
    ops.cong_control = EcnProbeControl;
    CHECK(xtcp::cc::RegisterCongestionControl(ops));
    registered = true;
}

/**
 * @brief D02: an ECE must enter CA_CWR (kernel tcp_enter_cwr,
 *        tcp_input.c:2686-2697) BEFORE the CC hook runs.
 *
 * Kernel tcp_fastretrans_alert's ECE path (tcp_try_to_open -> tcp_enter_cwr)
 * sets high_seq/prior_cwnd/prr_*, clears undo_marker and moves the state to
 * TCP_CA_CWR; tcp_cong_control runs afterwards, so a state-keying CC sees
 * state 2. The port never called tcp_enter_cwr at all: the ECE cut happened
 * inline, the state stayed Open and high_seq_ was never anchored (audit D02).
 */
static void TestEcnEntersCwr() {
    EnsureEcnProbeCc();
    EcnHarness h;
    CHECK(h.conn.EcnActive());
    CHECK(h.conn.SetCongestionControl("ecn_cc_probe"));
    h.Warmup(8);
    h.Send(kMss);
    g_ecnState = 0xFF;
    g_ecnCalls = 0;
    h.now += 20000;
    h.Deliver(h.conn.SndNxt(),
              static_cast<UInt16>(xtcp::core::kFlagAck | xtcp::core::kFlagEce));
    CHECK(1 <= g_ecnCalls);
    CHECK(2 == g_ecnState);   // red: 0 (never entered CWR)
}

/**
 * @brief D02: the ECE reduction must survive the same ACK.
 *
 * KCC's ssthresh hook passes snd_ssthresh through (cc_kcc.cpp:1042) and
 * cong_control owns the window, so the FSM's clamp to the hook's return
 * value is the only reduction. Before the fix the ECE block ran with the
 * sentinel threshold and cwnd never moved (audit D02, a4probe30: 92 -> 92).
 */
static void TestEcnCutSurvivesSameAck() {
    xtcp::cc::RegisterKcc();
    EcnHarness h;
    CHECK(h.conn.SetCongestionControl("kcc"));
    h.Warmup(20);
    const UInt32 cwnd_before = h.conn.Cwnd();
    CHECK(20 < cwnd_before);
    h.Send(kMss);
    h.now += 20000;
    h.Deliver(h.conn.SndNxt(),
              static_cast<UInt16>(xtcp::core::kFlagAck | xtcp::core::kFlagEce));
    std::fprintf(stderr, "[ecn-cc] kcc ECE: before=%u after=%u\n",
                 cwnd_before, h.conn.Cwnd());
    CHECK(h.conn.Cwnd() < cwnd_before);   // red: 92 -> 92
}

/**
 * @brief D02: an ECE ACK forbids undoing the cwnd reduction.
 *
 * Kernel tcp_fastretrans_alert part A (tcp_input.c:2986-2987) clears
 * prior_ssthresh on EVERY ECE ACK ("congestion is proven"), and
 * tcp_enter_cwr clears undo_marker. The port kept the no-SACK undo marker,
 * so the ECE that completed a recovery restored the pre-loss window and
 * ssthresh (audit D02, a4probe2/P2: cwnd 5 -> 14, ssthresh 2 -> 0x7FFFFFFF).
 */
static void TestEceBlocksRecoveryUndo() {
    EcnHarness h;   // built-in Reno (no CC plugin)
    h.Warmup(0);
    for (UInt32 i = 0; i < 4; ++i) {
        h.Send(kMss);
    }
    const UInt32 una = h.conn.SndUna();
    const UInt32 end = h.conn.SndNxt();
    for (UInt32 i = 0; i < 3; ++i) {
        h.now += 1000;
        h.Deliver(una);   // raw dup-ACK: no-SACK recovery entry
    }
    CHECK(h.conn.FastRecovery());
    const UInt32 cwnd_cut = h.conn.Cwnd();
    const UInt32 ssth_cut = h.conn.Ssthresh();
    h.now += 20000;
    h.Deliver(end, static_cast<UInt16>(xtcp::core::kFlagAck | xtcp::core::kFlagEce));
    std::fprintf(stderr, "[ecn-cc] undo gate: cut=%u/%u after=%u/%u\n",
                 cwnd_cut, ssth_cut, h.conn.Cwnd(), h.conn.Ssthresh());
    CHECK(h.conn.Cwnd() <= cwnd_cut);        // red: 14
    CHECK(h.conn.Ssthresh() <= ssth_cut);    // red: 0x7FFFFFFF
}

/**
 * @brief D13: a CWR episode ends when an ACK above high_seq arrives.
 *
 * Kernel tcp_fastretrans_alert part D, case TCP_CA_CWR (tcp_input.c:3003-3010):
 * CWR is held at exactly high_seq (the receiver has not seen the CWR bit yet)
 * and ends as soon as something ABOVE high_seq is ACKed. The port's exit
 * block only handled Recovery/Loss, so CA_CWR could never end (audit D13).
 */
static void TestCwrEpisodeEnds() {
    EnsureEcnProbeCc();
    EcnHarness h;
    CHECK(h.conn.EcnActive());
    CHECK(h.conn.SetCongestionControl("ecn_cc_probe"));
    h.Warmup(8);
    h.Send(kMss);
    const UInt32 ece_ack = h.conn.SndNxt();
    g_ecnState = 0xFF;
    h.now += 20000;
    h.Deliver(ece_ack,
              static_cast<UInt16>(xtcp::core::kFlagAck | xtcp::core::kFlagEce));
    CHECK(2 == g_ecnState);   // CWR entered (T1-3)
    // An ACK above high_seq: the peer has seen our CWR bit.
    h.Send(kMss);
    g_ecnState = 0xFF;
    h.now += 20000;
    h.Deliver(h.conn.SndNxt());
    CHECK(0 == g_ecnState);   // CWR ended (red: stays 2/0 pre-fix, unreachable)
}

/**
 * @brief Batch 5 addendum (B): a RACK verdict must not recut the window
 *        during CA_CWR.
 *
 * Kernel tcp_enter_recovery is guarded by `!tcp_in_cwnd_reduction(sk)`
 * (tcp_input_66.c:2834-2839) and tcp_in_cwnd_reduction is true for BOTH
 * TCP_CA_CWR and TCP_CA_Recovery (tcp_66.h:1245-1249): during CWR the
 * reduction snapshot (prior_cwnd_/prior_ssthresh_/high_seq_) already exists,
 * so tcp_init_cwnd_reduction must not run again. The port's RACK gate only
 * excluded CA_Loss (4), letting state 2 through and halving ssthresh a
 * second time (audit B).
 *
 * Red baseline: fr=1 and ssthresh 5 -> 2.
 */
static void TestRackDoesNotRecutInCwr() {
    EcnHarness h;   // built-in Reno: ssthresh is the observable
    h.Warmup(8);
    // Six segments in flight (one ACK-clock round's worth).
    for (UInt32 i = 0; i < 6; ++i) {
        h.Send(kMss);
    }
    const UInt32 una = h.conn.SndUna();
    // ECE dup-ACK on the frontier: tcp_enter_cwr -> CA_CWR (2).
    h.now += 20000;
    h.Deliver(una, static_cast<UInt16>(xtcp::core::kFlagAck | xtcp::core::kFlagEce));
    const UInt32 ssth_cwr = h.conn.Ssthresh();
    std::fprintf(stderr,
                 "[B] after CWR: una=%u nxt=%u q=%u cwnd=%u ssth=%u\n",
                 una, h.conn.SndNxt(), h.conn.RetransQueueSize(),
                 h.conn.Cwnd(), ssth_cwr);
    CHECK(2 <= h.conn.RetransQueueSize());

    // ONE dup-ACK whose SACK block sets the RACK anchor (segment 2 end) and
    // leaves segment 1 overdue: a pure RACK verdict. dup_acks_ stays 2, below
    // the classic 3-dup threshold, so the classic entry cannot mask it.
    h.now += 500000;
    h.Deliver(una, xtcp::core::kFlagAck, true, una + kMss, una + 2 * kMss);

    std::fprintf(stderr,
                 "[B] cwr ssth=%u -> after RACK ssth=%u fr=%d dup=%u\n",
                 ssth_cwr, h.conn.Ssthresh(), h.conn.FastRecovery() ? 1 : 0,
                 h.conn.DupAcks());
    CHECK(!h.conn.FastRecovery());            // red: RACK entered Recovery
    CHECK(ssth_cwr == h.conn.Ssthresh());     // red: cut again to 2
}
int main() {
    xtcp::buf::InitPools();
    TestEcnEntersCwr();
    TestEcnCutSurvivesSameAck();
    TestEceBlocksRecoveryUndo();
    TestCwrEpisodeEnds();
    TestRackDoesNotRecutInCwr();
    {
        // (1) Plugin level: CUBIC's ECN response cuts by beta 0.7. FSM
        // contract (ECN withdraw): ssthresh hook (updates the CUBIC epoch
        // state and returns the new threshold), cwnd_event (notification
        // only), then clamp the window to the threshold.
        xtcp::cc::RegisterCubic();
        const xtcp::cc::XtcpCongestionOps* ops = xtcp::cc::FindCongestionControl("cubic");
        CHECK(NULLPTR != ops);
        xtcp::cc::XtcpConnCc sk;
        ops->init(&sk);
        sk.mss = 1460;
        sk.snd_cwnd = 100;
        sk.snd_ssthresh = 0x7FFFFFFF;
        const UInt32 ssth = ops->ssthresh(&sk);
        ops->cwnd_event(&sk, xtcp::cc::kCaEventEcnCe);
        if (sk.snd_cwnd > ssth) {
            sk.snd_cwnd = ssth;
        }
        std::fprintf(stderr, "[ecn-cc] cubic after CE: cwnd=%u (expect ~70)\n", sk.snd_cwnd);
        CHECK(70 == sk.snd_cwnd);  // beta 0.7, exactly
        ops->release(&sk);

        // (2) Wire level: A runs CUBIC; a CE-marked segment into B makes B
        // echo ECE; A's cwnd must drop through CUBIC (~0.7x), not the Reno
        // half-cut (0.5x).
        xtcp::ndi::ManualBackend backend_a, backend_b;
        xtcp::XtcpStack stack_a(&backend_a);
        xtcp::XtcpStack stack_b(&backend_b);
        backend_a.SetRxHandler([&stack_a](xtcp::ndi::Packet&& p) {
            xtcp::buf::BufRef buf = xtcp::buf::BufRef::Acquire(p.len);
            std::memcpy(buf.Data(), p.data, p.len);
            buf.SetLen(p.len);
            stack_a.OnPacket(std::move(buf));
        });
        backend_b.SetRxHandler([&stack_b](xtcp::ndi::Packet&& p) {
            xtcp::buf::BufRef buf = xtcp::buf::BufRef::Acquire(p.len);
            std::memcpy(buf.Data(), p.data, p.len);
            buf.SetLen(p.len);
            stack_b.OnPacket(std::move(buf));
        });
        stack_a.SetDefaultCongestionControl("cubic");
        stack_a.SetDefaultEcn(true);
        stack_b.SetDefaultEcn(true);

        xtcp::core::Endpoint a_local, b_local;
        a_local.family = 4;
        a_local.addr[0] = 0x0A000001;
        a_local.port = 40210;
        b_local.family = 4;
        b_local.addr[0] = 0x0A000002;
        b_local.port = 9110;
        CHECK(stack_b.Listen(b_local));
        UInt64 conn_b = 0;
        stack_b.SetAcceptHandler([&conn_b](UInt64 id, const xtcp::core::Endpoint&,
                                           const xtcp::core::Endpoint&) {
            conn_b = id;
            return true;
        });
        const UInt64 conn_a = stack_a.Connect(a_local, b_local);
        CHECK(0 != conn_a);
        Pump(backend_a, backend_b, stack_a, stack_b);
        Pump(backend_a, backend_b, stack_a, stack_b);
        CHECK(xtcp::core::TcpState::kEstablished == stack_a.ConnectionState(conn_a));
        CHECK(0 != conn_b);

        // Grow cwnd: send 64 KB in 16 KB chunks (buffered-path retry so
        // window/send-buffer backpressure never fails the call).
        Byte payload[16384];
        std::memset(payload, 0x5C, sizeof(payload));
        for (UInt32 i = 0; i < 4; ++i) {
            UInt32 guard = 0;
            while (!stack_a.Send(conn_a, payload, sizeof(payload)) && 500 > ++guard) {
                Pump(backend_a, backend_b, stack_a, stack_b);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            Pump(backend_a, backend_b, stack_a, stack_b);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        UInt32 cwnd_before = 0;
        ConnCwnd(stack_a, conn_a, cwnd_before);
        std::fprintf(stderr, "[ecn-cc] cwnd before CE=%u\n", cwnd_before);
        CHECK(20 < cwnd_before);  // the pipe actually filled

        // Inject a CE-marked segment into B: take a REAL A->B data segment
        // off the wire (in-window, correct direction) and set its IP ECN
        // bits to CE (11). The TCP checksum is unaffected (the IP ECN bits
        // are outside the pseudo-header); the IP checksum is recomputed.
        Byte seg[65536];
        UInt32 seg_len = 0;
        for (UInt32 i = 0; i < 50 && 0 == seg_len; ++i) {
            while (0 != backend_a.TxPending()) {
                seg_len = backend_a.PollTx(seg);
                if (0 != seg_len) {
                    break;
                }
            }
            if (0 == seg_len) {
                // Re-open the pipe (the flow may be quiescent between
                // ACK windows) so a fresh segment is in flight.
                stack_a.Send(conn_a, payload, sizeof(payload));
                Pump(backend_a, backend_b, stack_a, stack_b);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        CHECK(0 != seg_len);
        std::vector<Byte> ce(seg, seg + seg_len);
        ce[1] = 0x03;  // CE (RFC 3168 IP ECN bits)
        xtcp::harness::FillIp4Checksum(ce.data());
        backend_b.Inject(ce.data(), static_cast<UInt32>(ce.size()), 0x0800);
        // B's CE -> SetCeSeen -> ECE echo on the next ACK; drive the loop.
        for (UInt32 i = 0; i < 20; ++i) {
            Pump(backend_a, backend_b, stack_a, stack_b);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        UInt32 cwnd_after = 0;
        ConnCwnd(stack_a, conn_a, cwnd_after);
        std::fprintf(stderr, "[ecn-cc] cwnd after CE=%u (before=%u, CUBIC per-round 0.7x)\n",
                     cwnd_after, cwnd_before);
        // B echoes ECE on every ACK until A's CWR arrives, so the
        // once-per-RTT throttle can apply several CUBIC 0.7x rounds. The
        // exact 0.7 semantics are pinned by the plugin-level check above;
        // here: the cut happened through the plugin path and the window
        // did not collapse.
        CHECK(cwnd_after < cwnd_before);
        CHECK(cwnd_after >= cwnd_before / 5);
    }
    xtcp::buf::ShutdownPools();
    std::fprintf(stderr, g_failures ? "ECN_CC: FAILED (%d)\n" : "ECN_CC: ALL PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
