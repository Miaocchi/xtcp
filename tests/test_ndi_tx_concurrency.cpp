/**
 * @file test_ndi_tx_concurrency.cpp
 * @brief The stack must not call a backend's Tx() concurrently.
 *
 * Backend::Tx() carries no thread-safety requirement in the interface
 * contract, and every shipped backend that is not the test ManualBackend
 * (TunBackend in samples/tun2socks, and any DMA ring in a real deployment)
 * appends to a plain container without a lock. The stack shards its
 * connection table, and each shard emits under its OWN lock, so two
 * connections placed on different shards reach Tx() from two threads at
 * once. That corrupted the heap in the real TUN path:
 *
 *   free(): corrupted unsorted chunks
 *   malloc(): unaligned tcache chunk detected
 *
 * This test installs a backend that ASSERTS single-threaded entry (it flags
 * a violation instead of corrupting memory, so the failure is deterministic
 * and does not depend on allocator timing), drives several connections from
 * separate threads through the real shard placement, and requires zero
 * concurrent Tx() calls.
 *
 * Red baseline: with per-shard emission straight into Tx(), the violation
 * counter is nonzero. Green: the stack serializes Tx() (or the contract is
 * changed so callers cannot hit it) and the counter stays zero.
 */

#include <xtcp/core/stack.h>
#include <xtcp/ndi.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

namespace {

    /**
     * @brief Backend that records concurrent Tx() entry instead of racing.
     *
     * It performs the same plain (unlocked) container append a real backend
     * does, but detects a second simultaneous entrant first and counts it.
     * No lock protects the queue on purpose: that is the contract the stack
     * is expected to honour on the backend's behalf.
     */
    class SingleThreadedBackend final : public xtcp::ndi::Backend {
    public:
        std::atomic<UInt32> concurrent_entries{0};
        std::atomic<UInt32> tx_calls{0};
        std::atomic<UInt32> inside{0};
        std::atomic<bool> rx_enabled{false};
        xtcp::ndi::RxHandler rx;

        virtual bool Tx(xtcp::ndi::Packet&& packet) noexcept override {
            if (1 < inside.fetch_add(1, std::memory_order_acq_rel) + 1) {
                concurrent_entries.fetch_add(1, std::memory_order_relaxed);
            }
            tx_calls.fetch_add(1, std::memory_order_relaxed);
            // Simulate the real work (a device write / queue append) so the
            // window in which a second thread can enter is realistic.
            for (UInt32 i = 0; i < 64; ++i) {
                std::atomic_signal_fence(std::memory_order_seq_cst);
            }
            // The queue is protected so a contract violation is COUNTED and
            // the assertion still prints; the real TunBackend has no lock and
            // corrupts the heap instead. The detection above is the signal.
            if (!packet.owned.IsEmpty()) {
                std::lock_guard<std::mutex> scope(queue_mutex_);
                queue_.push_back(std::move(packet.owned));
            }
            inside.fetch_sub(1, std::memory_order_acq_rel);
            return true;
        }

        virtual void SetRxHandler(xtcp::ndi::RxHandler handler) noexcept override {
            rx = std::move(handler);
        }

        virtual xtcp::ndi::BackendCaps Caps() const noexcept override {
            return xtcp::ndi::kCapNone;
        }

        /** @brief Hands the queued frames to the peer (single-threaded). */
        UInt32 DrainInto(SingleThreadedBackend& peer) {
            UInt32 moved = 0;
            std::vector<xtcp::buf::BufRef> take;
            {
                std::lock_guard<std::mutex> scope(queue_mutex_);
                take.swap(queue_);
            }
            for (xtcp::buf::BufRef& ref : take) {
                if (ref.IsEmpty()) {
                    continue;
                }
                xtcp::ndi::Packet p;
                p.data = ref.Data();
                p.len = ref.Len();
                p.eth_type = 0x0800;
                if (peer.rx) {
                    peer.rx(std::move(p));
                }
                ++moved;
            }
            return moved;
        }

    private:
        std::mutex queue_mutex_;
        std::vector<xtcp::buf::BufRef> queue_;
    };

    constexpr UInt32 kFlows = 8;
    constexpr UInt32 kBytesPerFlow = 256 * 1024;
    constexpr UInt32 kChunk = 1460;
}

int main() {
    xtcp::buf::InitPools();
    {
        SingleThreadedBackend a;
        SingleThreadedBackend b;
        xtcp::XtcpStack sa(&a);
        xtcp::XtcpStack sb(&b);

        a.SetRxHandler([&sa](xtcp::ndi::Packet&& p) {
            xtcp::buf::BufRef buf = xtcp::buf::BufRef::Acquire(p.len);
            if (buf.IsEmpty()) {
                return;
            }
            std::memcpy(buf.Data(), p.data, p.len);
            buf.SetLen(p.len);
            sa.OnPacket(std::move(buf));
        });
        b.SetRxHandler([&sb](xtcp::ndi::Packet&& p) {
            xtcp::buf::BufRef buf = xtcp::buf::BufRef::Acquire(p.len);
            if (buf.IsEmpty()) {
                return;
            }
            std::memcpy(buf.Data(), p.data, p.len);
            buf.SetLen(p.len);
            sb.OnPacket(std::move(buf));
        });

        std::atomic<UInt64> received{0};
        sb.SetRecvHandler([&received](UInt64, const Byte*, UInt32 n) {
            received.fetch_add(n, std::memory_order_relaxed);
        });

        // One listener per flow so each connection has a distinct 4-tuple and
        // therefore its own shard placement.
        std::vector<UInt64> conns;
        for (UInt32 f = 0; f < kFlows; ++f) {
            xtcp::core::Endpoint server;
            server.family = 4;
            server.addr[0] = 0x0A000002;
            server.port = static_cast<UInt16>(9000 + f);
            sb.Listen(server);

            xtcp::core::Endpoint local;
            local.family = 4;
            local.addr[0] = 0x0A000001;
            local.port = static_cast<UInt16>(30000 + f);
            const UInt64 conn = sa.Connect(local, server);
            CHECK(0 != conn);
            conns.push_back(conn);
        }

        // Handshake pump (single threaded, like a real event loop).
        for (UInt32 round = 0; round < 2000; ++round) {
            bool moved = false;
            if (0 != a.DrainInto(b)) {
                moved = true;
            }
            if (0 != b.DrainInto(a)) {
                moved = true;
            }
            sa.PollAckTimers();
            sb.PollAckTimers();
            if (!moved) {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }
        for (UInt32 f = 0; f < kFlows; ++f) {
            CHECK(xtcp::core::TcpState::kEstablished == sa.ConnectionState(conns[f]));
        }

        // Drive every connection from its own thread: this is what a real
        // tun2socks / iperf-parallel deployment does, and what the shard
        // design claims to support.
        std::atomic<bool> stop{false};
        std::thread pump([&]() {
            while (!stop.load(std::memory_order_relaxed)) {
                a.DrainInto(b);
                b.DrainInto(a);
                sa.PollAckTimers();
                sb.PollAckTimers();
                std::this_thread::sleep_for(std::chrono::microseconds(20));
            }
        });

        std::vector<std::thread> workers;
        for (UInt32 f = 0; f < kFlows; ++f) {
            workers.emplace_back([&sa, &conns, f]() {
                std::vector<Byte> data(kChunk, static_cast<Byte>(0x40 + f));
                UInt64 sent = 0;
                while (sent < kBytesPerFlow) {
                    if (sa.Send(conns[f], data.data(), kChunk)) {
                        sent += kChunk;
                    } else {
                        std::this_thread::yield();
                    }
                }
            });
        }
        for (auto& w : workers) {
            w.join();
        }
        for (UInt32 round = 0; round < 4000; ++round) {
            if (received.load() >= static_cast<UInt64>(kFlows) * kBytesPerFlow) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        stop.store(true, std::memory_order_relaxed);
        pump.join();

        CHECK(0 < a.tx_calls.load());
        CHECK(0 < b.tx_calls.load());
        if (0 != a.concurrent_entries.load() || 0 != b.concurrent_entries.load()) {
            std::fprintf(stderr,
                         "CONCURRENT Tx(): side A=%u side B=%u (backend contract violated)\n",
                         a.concurrent_entries.load(), b.concurrent_entries.load());
        }
        CHECK(0 == a.concurrent_entries.load());
        CHECK(0 == b.concurrent_entries.load());
    }
    xtcp::buf::ShutdownPools();

    if (0 < g_failures) {
        std::fprintf(stderr, "test_ndi_tx_concurrency: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("test_ndi_tx_concurrency: all passed\n");
    return 0;
}
