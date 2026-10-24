# Performance

> Language: **English** | [中文](PERFORMANCE_CN.md)

Every number in this document was produced by a command shown here, on the
hardware and software versions listed below, during one session. Raw
outputs were captured and are quoted verbatim in the result tables. Nothing
is projected, averaged across machines, or carried over from older runs.
Chinese version: [PERFORMANCE_CN.md](PERFORMANCE_CN.md).

## 1. Test environments

### Environment A — Windows host (all `bench_*` numbers)

| Item | Value |
|---|---|
| CPU | AMD Ryzen 9 7945HX, 16 cores / 32 threads (Zen 4), base clock 2.5 GHz |
| RAM | 63.2 GB |
| OS | Windows 11 Enterprise, build 26200 |
| Power plan | Vendor performance profile ("Beast mode") |
| Compiler | MSVC 14.39.33519 (Visual Studio 2022 Enterprise) |
| Build system | CMake 3.22.1, build type `Release` |
| Relevant flags | Default (`XTCP_CHECKSUM_VALIDATE` off for throughput benches) |

### Environment B — WSL2 guest (all TUN `interop_*` numbers)

| Item | Value |
|---|---|
| Host | Same physical machine as Environment A |
| Guest OS | Ubuntu 24.04.3 LTS |
| Kernel | 6.18.33.2-microsoft-standard-WSL2 |
| CPU/memory visible to guest | 32 vCPUs / 30 GB (WSL2 VM allocation) |
| Compiler | GCC 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1), `-O2` Release |
| TUN device | `/dev/net/tun`, samples executed as root |

### Environment C — QEMU emulation (functional verification only)

aarch64 cross-build with GCC 13 (`aarch64-linux-gnu-g++`), dynamically
linked against the aarch64 sysroot and executed under `qemu-aarch64`
user-mode emulation (`qemu-aarch64 -L /usr/aarch64-linux-gnu`) on the same
host. A fully static link is deliberately not used: the plugin loader calls
`dlopen`, and a static link then warns that the glibc shared objects are
still required at run time. QEMU single-core emulation distorts wall-clock
pacing, so **no performance numbers are taken from this environment**; it
exists to prove the code is correct on ARM64 (240/240 tests pass there,
zero warnings).

## 2. What each benchmark measures

| Benchmark | Measures | Does NOT measure |
|---|---|---|
| `bench_throughput` | One `XtcpStack` pair wired back-to-back through in-memory backends inside one process: TCP processing cost only | NIC, kernel network stack, real network |
| `bench_multi_thread` | N threads injecting segments into N distinct flows of one stack: RX-side concurrency behavior under synthetic load | End-to-end application throughput |
| `bench_syn_rate` | Connection establishment rate (SYN → ESTABLISHED → teardown), single process | Handshake under packet loss |
| `bench_conns` | Cost of sweeping an idle vs recently-active connection table | Memory footprint per connection |
| `interop_perf` | One-way bulk transfer between the kernel and XTCP through a real TUN device | Multi-flow aggregate |
| `interop_latency` | RTT distribution of 2000 sequential 1 KB echo round trips through TUN | First-packet latency, connection setup latency |
| `interop_fair` | Bandwidth share of 4 concurrent flows shaped by `tbf 20mbit` + `netem delay 40ms` on the TUN path | Fairness without shaping |

## 3. Results

### 3.1 Loopback throughput (`bench_throughput`, Environment A)

Command shape: `bench_throughput <total_bytes> <chunk_bytes> <qdisc> <quickack> [cc]`.
Three rounds per configuration; all rounds are shown.

| Configuration | Round 1 | Round 2 | Round 3 | Best | Best kpps |
|---|---|---|---|---|---|
| 64 MB, 1460 B chunks, KCC | 24 624.86 | 24 216.44 | 24 503.81 | **24 624.86** | 3 162.6 |
| 256 MB, 1460 B chunks, KCC | 25 532.07 | 22 212.09 | 25 127.18 | **25 532.07** | 3 279.0 |
| 64 MB, 1460 B chunks, Reno | 23 972.17 | 23 057.91 | 23 055.83 | **23 972.17** | 3 078.7 |
| 128 MB, 65 536 B chunks, KCC | 23 741.69 | 12 096.58 | 19 627.57 | **23 741.69** | 3 124.6 |
| 64 MB, 64 B chunks, KCC | 1 257.63 | 1 129.59 | 1 274.92 | **1 274.92** | 3 731.6 |

Reading these rows:

- The MSS-sized path sustains ~24–25 Gbps in-process on this host. This is
  the stack's own ceiling here: no NIC, no kernel networking involved.
- KCC versus Reno differs by ~2.7 % at this scale; both saturate the same
  loopback path.
- The 65 536-byte-chunk row shows the largest spread (12.1–23.7 Gbps).
  Large writes amplify scheduler noise on a laptop CPU (see §5).
- 64-byte writes are packet-rate bound: ~3.7 Mpps regardless of bandwidth,
  i.e. per-segment fixed cost dominates.

**Measurement note.** The rows above were taken *unpinned* on an otherwise
idle host. Thread migration alone moves the same binary by tens of percent,
and the host cannot reproduce them while any sustained load is running.
§7.7 repeats the comparison core-pinned against the pre-audit tree with
five-run medians; use those numbers when comparing revisions.

### 3.2 RX worker scaling (`bench_multi_thread`, Environment A)

Each worker injects 200 000 segments into its own flow; workers run
concurrently against one stack.

| Workers | Segments total | Wall time | Aggregate | kpps |
|---|---|---|---|---|
| 1 | 200 000 | 0.017 s | 135.18 Gbps | 11 573.6 |
| 8 | 1 600 000 | 0.189 s | 98.92 Gbps | 8 469.3 |

Eight workers aggregate *below* one worker. This is a measured property of
the current implementation under synthetic injection, not a law: per-worker
flows hash to different shards, so the run exercises cross-shard parallelism
plus shared ingress paths. Treat multi-core claims accordingly (§5).

**Correction (§7.7):** these numbers were measured *unpinned*, so the worker
threads migrated across the 32 logical CPUs. Pinned to eight dedicated cores
the eight-worker aggregate is 157-163 Gbps (above 100 Gbps) and the
single-worker figure is 94-104 Gbps; the "eight below one" reading was a
scheduling artefact, not the sharded ceiling.

### 3.3 Connection handling (`bench_syn_rate`, `bench_conns`, Environment A)

```
bench_syn_rate: 200000 conns in 0.353s = 566149 conns/s (completed=200000)
bench_conns:    10000 conns, 2000 rounds, 0.02 us/round, 0.0 ns/conn/round (idle)
bench_conns:    10000 conns, 500 rounds, 3481.35 us/round, 348.1 ns/conn/round (dirty)
```

Idle sweep is O(1) per round (timer-wheel fast path); the dirty sweep pays
per-connection work proportional to active state.

### 3.4 RTT latency over TUN (`interop_latency`, Environment B)

2000 sequential echo round trips, 1 KB payload, kernel client ↔ XTCP server
through `/dev/net/tun`. Two independent executions:

| Run | mean | p50 | p95 | p99 | jitter (σ) | failures |
|---|---|---|---|---|---|---|
| 1 | 205.8 µs | 204.0 µs | 233.5 µs | 266.9 µs | 21.1 µs | 0 |
| 2 | 195.6 µs | 193.6 µs | 219.6 µs | 259.8 µs | 17.2 µs | 0 |

This includes two TUN device crossings and the kernel client's own socket
path per round; it is not a pure stack-latency figure.

### 3.5 Bulk throughput over TUN (`interop_perf`, Environment B)

16 MB one-way transfer, kernel sender ↔ XTCP receiver via TUN:

| Run | XTCP over TUN | Kernel loopback baseline (same command) |
|---|---|---|
| 1 | 1 806.42 Mbps | 30 932.36 Mbps |
| 2 | 1 791.84 Mbps | 21 148.88 Mbps |

The baseline row is the kernel's own loopback throughput printed by the
same sample for calibration. XTCP-over-TUN reaches ~1.8 Gbps and is bound
by the TUN character-device copy path, not by TCP processing: the in-process
number for the same machine is ~25 Gbps (§3.1).

### 3.6 Fairness (`interop_fair`, Environment B)

Four concurrent flows shaped by `tbf 20mbit` + `netem delay 40ms` on the
TUN path, each transferring 2 097 152 bytes:

```
[fairness] flows=4 total_mbps=18.9
flow[0] 4.78 Mbps (25.3%)   flow[1] 4.75 Mbps (25.1%)
flow[2] 4.71 Mbps (24.9%)   flow[3] 4.67 Mbps (24.7%)
Jain index = 0.9999 ; all flows completed
```

## 4. Verification context

Performance means nothing if correctness is conditional, so the gates that
held while these numbers were taken:

- Full deterministic suite, four local configurations: Linux g++ Release
  240/240, Linux clang++ Release 240/240, Linux g++ ASan/UBSan 240/240,
  Linux clang++ ASan/UBSan 240/240; Windows MSVC Release 239/239 and MSVC
  Debug 239/239.
- aarch64 cross-build under `qemu-aarch64`: 240/240, zero warnings.
- CI on the exact source revision: ASan+UBSan suites green on Linux
  (clang++ and g++) and Windows MSVC (Release and Debug).
- Zero compiler warnings in every configuration, including the builds that
  enable `samples/`, `bench/` and the lwIP differential target.

## 5. Threats to validity

Stated before you cite these numbers anywhere:

1. **Laptop hardware.** Environment A is a mobile Zen 4 part under a vendor
   performance profile. Thermal states move individual rounds by tens of
   percent (visible in §3.1); "best of 3" is reported precisely because of
   that, and every round is published.
2. **In-process loopback is not a network.** `bench_throughput` excludes
   NIC, DMA, kernel stack, and interrupts entirely. It bounds what the
   library itself costs; it does not predict deployed throughput.
3. **WSL2 is virtualized.** TUN numbers include the hypervisor's device
   model. Absolute latencies (§3.4) would differ on bare-metal Linux; the
   *comparison* against the kernel-loopback baseline on the same guest is
   the meaningful part.
4. **Synthetic injection ≠ application load.** §3.2 drives pre-built
   segments at the RX path. Real workloads interleave syscalls, timers, and
   user callbacks.
5. **Single-run fairness/latency samples.** §3.4–3.6 report two runs where
   marked, single runs otherwise. They demonstrate behavior and reproducibility
   at order-of-magnitude level, not statistical rigor.
6. **No DPDK/EFVI backend exists yet**, so line-rate claims beyond the TUN
   path cannot be made at all.

## 6. Reproducing

```bash
# Environment A (Windows, MSVC Release)
cmake -B build -G "Visual Studio 17 2022" -A x64 \
      -DXTCP_BUILD_TESTS=ON -DXTCP_BUILD_BENCH=ON
cmake --build build --config Release
build\Release\bench_throughput.exe 67108864 1460 0 1        # §3.1 row 1
build\Release\bench_multi_thread.exe                        # §3.2
build\Release\bench_syn_rate.exe 200000                     # §3.3
build\Release\bench_conns.exe 10000 2000                    # §3.3 idle
build\Release\bench_conns.exe 10000 500 dirty               # §3.3 dirty

# Environment B (WSL2, root for /dev/net/tun)
cmake -B build -DCMAKE_BUILD_TYPE=Release -DXTCP_BUILD_SAMPLES=ON
cmake --build build -j$(nproc)
sudo ./build/interop_latency     # §3.4
sudo ./build/interop_perf        # §3.5
sudo ./build/interop_fair        # §3.6
```

Core-pinned runs (§7.7) - pinning is what makes cross-revision numbers
comparable, because an unpinned run moves by tens of percent with scheduler
migration:

```bash
# Linux
taskset -c 3           ./build/bench_throughput 67108864 1460 0 1 kcc 0   # single core
taskset -c 3           ./build/bench_throughput 268435456 1460 0 1 kcc 0  # 256 MB
taskset -c 0-7         ./build/bench_multi_thread                         # eight cores
```

```powershell
# Windows: set ProcessorAffinity after start (0x8 = core 3, 0xFF = cores 0-7)
$p = Start-Process build\Release\bench_throughput.exe `
     -ArgumentList '67108864','1460','0','1','kcc','0' -NoNewWindow -PassThru
$p.ProcessorAffinity = [IntPtr]0x8; $p.WaitForExit()
```

Raw outputs corresponding to every table above were captured from the runs
this document reports; re-running should land within the published spreads
on comparable hardware.

## 7. Hot-path cost: the in-flight query and the optimisation series

An audit of the recovery/episode machinery against Linux v6.6 (see
`docs/ARCHITECTURE.md` §"CA state machine") needed
`tcp_packets_in_flight()` in several new places: the loss cut, the rate
sample, the `CA_EVENT_TX_START` gate and the RFC 6675 pipe gate. The kernel
gets that value from O(1) counters; the first implementation walked
`retrans_queue_` instead, which is O(N) per call and sits on the per-ACK path.

The hot path has been tightened further since that audit: §7.4-§7.6 record
each step with its deterministic instruction-count effect, and §7.7 the
core-pinned end-to-end result. Cumulative against the scalar baseline
(`ca519b7`): 30 398 091 -> 24 730 947 instructions for the same 1 MiB
workload (-18.6%), and +26% median throughput on the documented
configuration.

### 7.1 Why it matters at 100 Gbps

A 100 Gbps path at 1 ms RTT holds ~1.25 MB, i.e. ~850 MSS segments in the
retransmission queue. Measured cost of one queue-walk iteration on
Environment B hardware: **1.56 ns per queued segment** (probe
`deep_pipe2`, 200 000 iterations at queue depth 10, `taskset`-pinned).

| Queue depth | Queue walk | O(1) counters | Ratio |
|---|---|---|---|
| 10 | 15.58 ns | 2.04 ns | 7.7x |

Extrapolated to a 100 Gbps BDP (~850 segments) one walk costs ~1.3 us, and
the ACK path called it once or twice per ACK. The three terms are now
maintained incrementally (`pipe_sacked_bytes_`, `pipe_lost_bytes_`,
`pipe_retrans_bytes_`) at every mutation of the queue or of
`seg.sacked` / `seg.lost` / `seg.retries`, so the query is O(1).

### 7.2 End-to-end effect (no regression)

`bench_throughput 67108864 1460 0 1 kcc` (the §3.1 row-1 configuration),
both binaries `taskset`-pinned to the same core, alternating, best of 6:

| Revision | Best | Ratio |
|---|---|---|
| Pre-audit tree (`7da6f59`) | 14 507.56 Mbps | 1.0000 |
| Audited tree (this revision) | **15 423.28 Mbps** | **1.0631** |

The audited tree is *faster* than the pre-audit one at the documented
configuration. Individual rounds on this laptop spread by tens of percent
(§5.1), so only the pinned alternating comparison is meaningful; unpinned
single runs are not.

**Host-state caveat.** These two numbers and §3.1's 24.6 Gbps come from the
same command measured in different host states: the pre-audit tree today
measures anywhere from 14.5 to 21.8 Gbps depending on background load. The
ratio above is therefore only valid within its own session. §7.7 is the
reference comparison - pinned alternating A/B, five-run medians, current
tree.

### 7.3 Correctness gate for the incremental counters

An incrementally maintained sum is only safe if every mutation re-accounts
the segment. Two tests enforce that:

- `TestPipeCountersMatchWalk` drives the named transitions (partial SACK,
  narrower SACK, RTO with LOST+retransmit, cumulative-ACK pop, full drain)
  and asserts the O(1) value equals `PipeWalkReference()` - the original walk,
  retained as a diagnostics-only oracle - after each one.
- `TestPipeCountersRandomized` runs 400 deterministic pseudo-random
  operations (sends, bare and SACK-bearing dup-ACK bursts, RTOs, partial and
  full ACKs) and checks the same invariant after every single step.

Removing any one of the ~20 re-account sites makes
`TestPipeCountersRandomized` fail on the first step that reaches it (verified
by deleting the RTO `PipeReaccountSeg` call: `diverged at step 0`).


### 7.4 SSSE3 copy+checksum on GCC/Clang

The fused payload copy + RFC 1071 accumulation had its SSSE3 path guarded by
`__SSSE3__` / `_MSC_VER`. GCC and Clang do not define `__SSSE3__` at the
default `-march` level, so on Linux the whole vector path was compiled out and
every data segment fell back to the scalar byte loop - `BuildSegmentPacket`
absorbed it as 28.05% of all instructions (callgrind,
`bench_throughput 1048576 1024 0 1 kcc 0`). MSVC was unaffected (`_MSC_VER`
satisfied the guard), which is why Windows measured ~60% higher than WSL for
the same source.

The guard is now `XTCP_HAS_SSSE3_CODE` and the two vector routines carry a
function-scoped `__attribute__((target("ssse3")))` (`XTCP_TARGET_SSSE3`). The
translation unit's global ISA baseline is unchanged and `CpuHasSsse3()` still
selects at run time, so CPUs without SSSE3 keep the scalar fallback.
`Checksum()` also routes header-sized inputs (< 64 B: IPv4 pseudo 12 B, IPv4
20 B, TCP 20 B) to the scalar loop, where vector setup/drain costs more than
the word adds.

Deterministic effect (callgrind, same 1 MiB workload, instructions):

| Build | Total Ir | vs scalar |
|---|---|---|
| Scalar (`ca519b7`) | 30 398 091 | 1.0000 |
| SSSE3 enabled | **26 204 645** | **0.8620** |

`CopyAndChecksumSimd` is now 10.46% (2.74 M Ir) as a separate symbol instead of
28.05% folded into `BuildSegmentPacket`, and `ChecksumSimd` leaves the profile
entirely. Alternating wall-clock runs stay inside the §5.1 spread and average
+7% (8 rounds, mean ratio 1.0698); the instruction count is the noise-free
metric.

Correctness: `test_checksum_simd` (SIMD vs scalar differential) passes and the
full matrix is green (WSL Release 240/240, WSL ASan 240/240, MSVC Release
239/239, MSVC Debug 239/239).


### 7.5 One fewer clock read per send

`MaybeMarkAppLimited` re-read the clock for its pacing-deadline comparison
although `SendData` already holds the entry timestamp. With KCC pacing active
that was an extra vDSO `clock_gettime` per application send; the PMU profile
(`perf record -F 5000 -e cycles`, 128 MiB) put `__vdso_clock_gettime` at
9.80% self, the largest single entry. The timestamp is now passed through
(same instant, so semantics are unchanged).

| Build | Total Ir (callgrind, 1 MiB) |
|---|---|
| `74b27ed` | 26 326 396 |
| with the timestamp reuse | **26 122 982** (-0.77%) |

Combined with §7.4 the total is 30 398 091 -> 26 122 982, i.e. -14.1% against
the scalar build. After the change `__vdso_clock_gettime` is 8.17% of cycles
and IPC is 1.76. The two remaining per-segment clock reads (one in
`XtcpStack::Send`, one in `OnPacket`) are required for pacing and for the
`sent_at` RTT/RACK anchor.

The rest of the profile is flat - buffer refcounting ~14% (one `lock xadd`
plus cache-line traffic per reference drop, inherent to the pooled
refcounted design), harness `Pump`/`Inject`/`memmove` ~16% (ManualBackend is
test infrastructure), `BuildSegmentPacket` 3.3%, `SendData` 3.5%,
`OnAckReceived` 3.2%, `KccMain` 2.4%, mutexes 2.8%. No single waste item
remains; further gains would need concurrency or buffer-lifetime changes and
are treated as out of scope.


### 7.6 AVX2 fused copy + checksum

§7.4 restored the 16-byte SSSE3 kernel on GCC/Clang. The payload copy and the
RFC 1071 accumulation now also have a 256-bit AVX2 kernel
(`CopyAndChecksumAvx2`, 64 bytes per iteration, two independent accumulator
chains, the same 16 KiB drain, and the same bit-identical accumulation rule:
`vpshufb` reverses the bytes inside each 128-bit lane and `vpunpck*` zero-extends
the words into 32-bit lanes). Selection is runtime, `CpuHasAvx2()` then
`CpuHasSsse3()` then scalar; the function-scoped `target("avx2")` attribute
keeps the translation unit's global ISA baseline unchanged.

| Build | Total Ir (callgrind, 1 MiB) | Copy+checksum symbol |
|---|---|---|
| SSSE3 only (`fb0e51b`) | 26 122 982 | 2 740 233 (10.5%) |
| AVX2 | **24 730 947** | **1 453 056 (5.9%)** |

Cumulative against the scalar build (`ca519b7`, 30 398 091 Ir): **-18.6%**.

Pinned alternating wall-clock, `bench_throughput 67108864 1460 0 1 kcc`,
`taskset -c 3`, 12 rounds, scalar build vs this build:

| | Scalar (`ca519b7`) | This build | Ratio |
|---|---|---|---|
| Median | 13 258.4 Mbps | **16 740.0 Mbps** | **1.2626** |
| Mean | 13 282.5 Mbps | 16 189.1 Mbps | 1.2188 |
| Best | 15 636.1 Mbps | **19 301.3 Mbps** | 1.2344 |

On the Windows host (Environment A) the same command reached 22 674 Mbps
(best of 5). Absolute numbers on this laptop move by tens of percent with
background load (§5.1); only the pinned alternating comparison is meaningful.

Correctness: `test_checksum_simd` (SIMD vs scalar differential) passes, the
transfer tests' rolling-CRC checks pass, and the full matrix is green (WSL
Release 240/240, WSL ASan 240/240, MSVC Release 239/239, MSVC Debug 239/239)
with zero warnings on clean builds.

### 7.7 Pinned-core measurements (single core vs eight cores)

Every number below is core-pinned (`taskset -c` on Linux,
`ProcessorAffinity` on Windows), five runs per configuration, median and
best reported, measured in one session on the same host against the
pre-audit tree (`7da6f59`) as the control. The host was **not** idle (the agent session itself held ~700% CPU) and
ran on the Windows **Balanced** power plan, not the vendor profile recorded
in §1 for the §3 numbers - two more reasons the absolute values sit below an
idle-host run. Only the paired comparison is meaningful.

Single core (core 3), full-stack loopback:

| Config | `7da6f59` median | `7da6f59` best | this build median | this build best | median delta |
|---|---|---|---|---|---|
| Windows 64 MB / 1460 B | 15 431.77 | 19 671.74 | 21 434.21 | 23 041.09 | +38.9% |
| Windows 256 MB / 1460 B | 16 809.32 | 21 266.21 | 20 969.03 | 23 178.04 | +24.7% |
| WSL2 64 MB / 1460 B | 15 497.33 | 16 382.10 | 19 248.16 | 20 447.56 | +24.2% |
| WSL2 256 MB / 1460 B | 16 026.39 | 16 725.19 | 16 603.70 | 20 047.44 | +3.6% |
| WSL2 RX injection, 1 worker | 54.10 Gbps | 55.80 | 64.55 | 65.34 | +19.3% |

Eight cores (cores 0-7), RX injection (`bench_multi_thread`, Windows host):

| Metric | `7da6f59` | this build |
|---|---|---|
| 1 worker median | 89.79 Gbps | 94.30 Gbps |
| 1 worker best | 90.45 Gbps | 104.41 Gbps |
| 8 worker aggregate median | 154.21 Gbps | **157.12 Gbps** |
| 8 worker aggregate best | 159.65 Gbps | **162.68 Gbps** |

The 8-worker runs were 149.5-162.7 Gbps across five runs (all above 100 Gbps).

Against the immediate pre-optimisation tree (`ca519b7`, the scalar
copy+checksum build), the same 64 MB pinned configuration moved from a
13 258.4 Mbps median to 16 740.0 Mbps (x1.263) over 12 alternating rounds -
the "+26%" quoted in §7.

Single-flow ceiling: a 1460 B flow at 23 Gbps carries ~1.6 M segments/s, i.e.
~2700 cycles/segment on one core. A single-flow 100 Gbps would need 8.56 M
segments/s (~520 cycles/segment), which is unreachable in-process without
TSO/zero-copy/NIC offload. 100 Gbps is reached by sharding flows across cores:
eight pinned cores give 157-163 Gbps.
