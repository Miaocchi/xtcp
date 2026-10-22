#pragma once

/**
 * @file cc.h
 * @brief Congestion control hooks, kernel tcp_congestion_ops style.
 *
 * The XtcpCongestionOps fields mirror Linux net/ipv4/tcp_cong.c semantics
 * so kernel CC modules port into xtcp with logic and constants unchanged.
 */

#include <xtcp/stdafx.h>
#include <xtcp/core/tcp.h>

namespace xtcp {
    namespace cc {
        /**
         * @brief Per-connection fields the CC hooks operate on (kernel
         *        tcp_sock field equivalents). Defined in core/tcp.h so the
         *        connection owns the state; the alias keeps the cc API name.
         */
        using XtcpConnCc = core::XtcpConnCc;

        /**
         * @brief ACK sample (kernel struct ack_sample equivalent).
         */
        struct AckSample {
            UInt32  rtt_us     = 0;   /**< RTT sample */
            UInt32  acked      = 0;   /**< Bytes acknowledged */
            UInt32  delivered  = 0;   /**< Total delivered count */
            UInt32  inflight   = 0;
        };

        /**
         * @brief Rate sample (kernel struct rate_sample equivalent).
         */
        struct RateSample {
            UInt32  delivered      = 0;  /**< Delivered since last sample */
            UInt32  interval_us    = 0;  /**< Elapsed since last sample */
            UInt32  rtt_us         = 0;  /**< RTT sample (0 = none) */
            UInt32  lost           = 0;  /**< Lost bytes since last sample */
            /**
             * @brief Kernel rs->losses equivalent, PACKETS: segments freshly
             *        marked lost since the last sample (tcp_input_66.c:3936
             *        `lost = tp->lost - lost`). KCC subtracts this from cwnd
             *        directly (tcp_kcc.c:3993 `cwnd -= rs->losses`), so the
             *        byte-domain `lost` above cannot stand in for it: a
             *        2*MSS loss and a 1-byte loss must not look the same.
             */
            UInt32  lost_pkts      = 0;
            UInt32  acked          = 0;
            UInt32  is_app_limited = 0;  /**< Send-time app-limited snapshot (0/1) */
            /**
             * @brief Kernel rs->prior_delivered equivalent, BYTES (xtcp's
             *        delivered domain; the kernel counts packets): the
             *        connection's delivered total when the most recently
             *        sent segment acknowledged by this sample was sent.
             */
            UInt64  prior_delivered = 0;
            /**
             * @brief Kernel tp->delivered equivalent, cumulative BYTES
             *        after this ACK was processed.  Do not confuse with
             *        delivered (per-sample delta) or with packet counts.
             */
            UInt64  delivered_total = 0;
            /** @brief Kernel rs->is_ack_delayed equivalent (0/1). */
            bool    is_ack_delayed = false;
            /**
             * @brief Kernel rs->delivered equivalent, PACKETS: segments
             *        delivered since the last emitted sample (cumulative ACK
             *        and SACK both count, mirroring tcp_count_delivered).
             *        The byte-domain `delivered` above is unchanged for
             *        BBR/CUBIC; KCC reads this packet domain.
             */
            UInt32  delivered_pkts = 0;
            /** @brief Kernel rs->prior_delivered (PACKETS): delivered_pkts
             *         when the newest acknowledged segment was sent. */
            UInt64  prior_delivered_pkts = 0;
            /** @brief Kernel tp->delivered (PACKETS), cumulative after ACK. */
            UInt64  delivered_total_pkts = 0;
            /** @brief Kernel rs->prior_in_flight (BYTES): in-flight bytes
             *         BEFORE this ACK was processed (tcp_input.c:3839).
             *         The PROBE_BW cycle phase test uses it; DRAIN keeps the
             *         post-ACK inflight, as kernel kcc_check_drain runs from
             *         cong_control (after the ACK cleaned the queue). */
            UInt64  prior_in_flight_bytes = 0;
            /** @brief Kernel interval_to_edt (MICROSECONDS): the connection's
             *         pacing-queue depth at this ACK, i.e. how long until the
             *         last queued segment departs. TcpConn::pacing_deadline_
             *         is the tp->tcp_wstamp_ns equivalent; both are us.
             *         Kernel kcc_packets_in_net_at_edt credits
             *         bw*interval_to_edt to the in-flight count. */
            UInt32  pacing_edt_us = 0;
        };

        /**
         * @brief Kernel tcp_rate_skb_delivered delayed-ACK heuristic
         *        (tcp_input.c: pkts_acked==1 && fully_acked && !prior_sacked
         *        && (snd_una-prior_snd_una)<mss && prior_delivered+1==delivered
         *        && !(CA_ALERT|SYN_ACKED)).  State-free so the exact truth
         *        table can be tested without a live connection.
         * @param acked            Cumulative bytes acknowledged by this ACK.
         * @param popped           Number of fully acknowledged segments popped.
         * @param newest_len       Length of the newest popped segment.
         * @param batch_retrans    Any popped segment was a retransmission.
         * @param prior_sacked     Connection had SACKed bytes before the pop.
         * @param ece              The ACK carried ECE (FLAG_CA_ALERT).
         * @param sack_present     The ACK carried at least one SACK block.
         * @param sent_delivered   delivered snapshot of the newest popped segment.
         * @param delivered_after  Cumulative delivered bytes after this ACK.
         * @param mss              Effective MSS (strict '<' test).
         */
        inline bool IsLikelyDelayedAck(UInt32 acked, UInt32 popped, UInt32 newest_len,
                                       bool batch_retrans, bool prior_sacked,
                                       bool ece, bool sack_present,
                                       UInt64 sent_delivered, UInt64 delivered_after,
                                       UInt32 mss) noexcept {
            if (0 == acked || 0 == mss || acked >= mss) {
                return false;
            }
            if (1 != popped || acked != newest_len) {
                return false;
            }
            if (batch_retrans || prior_sacked || ece || sack_present) {
                return false;
            }
            return sent_delivered + acked == delivered_after;
        }

        /**
         * @brief Kernel tcp_rate_check_app_limited truth table
         *        (tcp_rate_v66.c:194-207). State-free so every row can be
         *        tested without a live connection:
         *          1. write_seq - snd_nxt < mss   -> pending < mss
         *          2. nothing queued in host qdisc/NIC -> tx_idle
         *          3. tcp_packets_in_flight < snd_cwnd -> packet units
         *          4. lost_out <= retrans_out
         * @param pending_bytes Bytes the application has queued but not sent.
         * @param tx_idle       No pending send/emit backlog in the host.
         * @param inflight_pkts Packets in flight (kernel packets_out - ...).
         * @param cwnd_pkts     Congestion window in packets.
         * @param lost_out_pkts Packets marked lost and not yet delivered.
         * @param retrans_out_pkts Packets retransmitted and still in flight.
         * @param mss           Effective MSS.
         */
        inline bool IsAppLimited(UInt32 pending_bytes, bool tx_idle,
                                 UInt32 inflight_pkts, UInt32 cwnd_pkts,
                                 UInt32 lost_out_pkts, UInt32 retrans_out_pkts,
                                 UInt32 mss) noexcept {
            if (0 == mss) {
                return false;
            }
            return pending_bytes < mss && tx_idle &&
                   inflight_pkts < cwnd_pkts &&
                   lost_out_pkts <= retrans_out_pkts;
        }

        /**
         * @brief CA events (kernel enum tcp_ca_event equivalent).
         * @note Values are append-only: new events must not renumber the
         *       existing ones (out-of-tree ops switch on them).
         */
        enum CaEvent : Byte {
            kCaEventLoss       = 0,
            kCaEventEcnCe      = 1,
            kCaEventAck        = 2,
            kCaEventDelayedAck = 3,
            kCaEventNewData    = 4,
            kCaEventTxStart    = 5,  /**< First data after an idle pipe
                                          (kernel CA_EVENT_TX_START) */
            kCaEventCompleteCwr = 6, /**< A cwnd reduction episode ended
                                          (kernel CA_EVENT_COMPLETE_CWR,
                                          tcp_end_cwnd_reduction). Append-only:
                                          out-of-tree ops that switch on the
                                          old values are unaffected. */
        };

        /**
         * @brief CC hook table (kernel tcp_congestion_ops equivalent).
         */
        struct XtcpCongestionOps {
            const char* name = NULLPTR;
            void (*init)(XtcpConnCc* sk) = NULLPTR;
            void (*release)(XtcpConnCc* sk) = NULLPTR;
            UInt32 (*ssthresh)(XtcpConnCc* sk) = NULLPTR;
            void (*cong_avoid)(XtcpConnCc* sk, UInt32 ack, UInt32 acked) = NULLPTR;
            void (*set_state)(XtcpConnCc* sk, Byte new_state) = NULLPTR;
            void (*cwnd_event)(XtcpConnCc* sk, CaEvent ev) = NULLPTR;
            void (*pkts_acked)(XtcpConnCc* sk, const AckSample* sample) = NULLPTR;
            UInt32 (*undo_cwnd)(XtcpConnCc* sk) = NULLPTR;
            void (*cong_control)(XtcpConnCc* sk, const RateSample* rs) = NULLPTR;
            UInt32 (*reinit_ssthresh)(XtcpConnCc* sk) = NULLPTR;
            Byte    flags = 0;
        };

        /**
         * @brief Registers a CC algorithm (immediate availability).
         */
        bool RegisterCongestionControl(const XtcpCongestionOps& ops) noexcept;
        /**
         * @brief Unregisters a CC algorithm by name.
         */
        bool UnregisterCongestionControl(const char* name) noexcept;
        /**
         * @brief Looks up a CC algorithm by name.
         * @return Hook table, or NULLPTR.
         */
        const XtcpCongestionOps* FindCongestionControl(const char* name) noexcept;
        /**
         * @brief Copies a registered (active) algorithm into out atomically.
         * @param name Algorithm name.
         * @param out  Receives a private copy of the hook table.
         * @return True when found and active.
         * @note The returned copy is safe to keep beyond registration changes:
         *       Unregister/Register mutate the registry entry, never the copy.
         */
        bool GetCongestionControl(const char* name, XtcpCongestionOps& out) noexcept;
        /**
         * @brief Applies an ACK/rate sample to a connection's CC.
         * @param sk    Connection CC state.
         * @param ops   Algorithm hooks (may be NULLPTR for no-op).
         * @param rs    Rate sample.
         */
        void ApplyRateSample(XtcpConnCc* sk, const XtcpCongestionOps* ops, const RateSample* rs,
                     UInt32 ack) noexcept;

        /**
         * @brief Registers the BBRv1 port (reference algorithm).
         */
        void RegisterBbrv1() noexcept;
        /**
         * @brief Registers the CUBIC sample (tutorial port).
         */
        void RegisterCubic() noexcept;
        /**
         * @brief Registers the KCC port (built-in algorithm; KCC is the
         *        default CC for new connections, Reno when no ops plug in).
         */
        void RegisterKcc() noexcept;
        /**
         * @brief Registers the built-in algorithms (KCC, BBRv1, CUBIC) once.
         * @note Called automatically by XtcpStack construction.
         */
        void RegisterBuiltinCc() noexcept;
    }
}
