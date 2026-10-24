# 性能

> 语言：[English](PERFORMANCE.md) | **中文**

本文档中的每一个数字都由文中展示的命令、在下列硬件与软件版本上、于同一
工作时段内产生。原始输出已留存，并在结果表中逐字引用。没有任何数字是
推算的、跨机器平均的、或沿用旧运行的。英文主版本：
[PERFORMANCE.md](PERFORMANCE.md)。

## 1. 测试环境

### 环境 A —— Windows 主机（全部 `bench_*` 数字）

| 项目 | 值 |
|---|---|
| CPU | AMD Ryzen 9 7945HX，16 核 / 32 线程（Zen 4），基础频率 2.5 GHz |
| 内存 | 63.2 GB |
| 操作系统 | Windows 11 企业版，build 26200 |
| 电源计划 | 厂商性能档（"野兽模式"） |
| 编译器 | MSVC 14.39.33519（Visual Studio 2022 Enterprise） |
| 构建系统 | CMake 3.22.1，构建类型 `Release` |
| 相关开关 | 默认（吞吐基准关闭 `XTCP_CHECKSUM_VALIDATE`） |

### 环境 B —— WSL2 客户机（全部 TUN `interop_*` 数字）

| 项目 | 值 |
|---|---|
| 宿主 | 与环境 A 同一台物理机 |
| 客户机系统 | Ubuntu 24.04.3 LTS |
| 内核 | 6.18.33.2-microsoft-standard-WSL2 |
| 客户机可见 CPU/内存 | 32 vCPU / 30 GB（WSL2 VM 分配） |
| 编译器 | GCC 13.3.0（Ubuntu 13.3.0-6ubuntu2~24.04.1），`-O2` Release |
| TUN 设备 | `/dev/net/tun`，样例以 root 执行 |

### 环境 C —— QEMU 仿真（仅用于功能验证）

GCC 13（`aarch64-linux-gnu-g++`）aarch64 交叉构建、全静态链接，在同一
主机上以 `qemu-aarch64` 用户态仿真执行。QEMU 单核仿真会扭曲墙钟节奏，
因此**本环境不产生任何性能数字**；它的作用是证明代码在 ARM64 上正确
（237/237 测试通过）。

## 2. 每个基准测什么

| 基准 | 测量对象 | 不测量 |
|---|---|---|
| `bench_throughput` | 一对 `XtcpStack` 经进程内内存后端背靠背相连：纯 TCP 处理成本 | 网卡、内核网络栈、真实网络 |
| `bench_multi_thread` | N 线程向一个栈的 N 条不同流注入段：合成负载下的 RX 侧并发行为 | 端到端应用吞吐 |
| `bench_syn_rate` | 连接建立速率（SYN → ESTABLISHED → 拆除），单进程 | 有丢包时的握手 |
| `bench_conns` | 扫描空闲表与近期活跃表的成本 | 每连接内存占用 |
| `interop_perf` | 内核与 XTCP 经真实 TUN 设备的单向批量传输 | 多流聚合 |
| `interop_latency` | 经 TUN 的 2000 次顺序 1 KB echo 往返延迟分布 | 首包延迟、建连延迟 |
| `interop_fair` | `tbf 20mbit` + `netem delay 40ms` 整形下 4 并发流的带宽占比 | 无整形时的公平性 |

## 3. 结果

### 3.1 环回吞吐（`bench_throughput`，环境 A）

命令形态：`bench_throughput <total_bytes> <chunk_bytes> <qdisc> <quickack> [cc]`。
每配置三轮；所有轮次均列出。

| 配置 | 第 1 轮 | 第 2 轮 | 第 3 轮 | 最优 | 最优 kpps |
|---|---|---|---|---|---|
| 64 MB，1460 B 块，KCC | 24 624.86 | 24 216.44 | 24 503.81 | **24 624.86** | 3 162.6 |
| 256 MB，1460 B 块，KCC | 25 532.07 | 22 212.09 | 25 127.18 | **25 532.07** | 3 279.0 |
| 64 MB，1460 B 块，Reno | 23 972.17 | 23 057.91 | 23 055.83 | **23 972.17** | 3 078.7 |
| 128 MB，65 536 B 块，KCC | 23 741.69 | 12 096.58 | 19 627.57 | **23 741.69** | 3 124.6 |
| 64 MB，64 B 块，KCC | 1 257.63 | 1 129.59 | 1 274.92 | **1 274.92** | 3 731.6 |

如何读这些行：

- MSS 大小路径在本主机进程内维持约 24–25 Gbps。这就是栈自身在此处的
  上限：不涉及网卡、不涉及内核网络。
- KCC 与 Reno 在此规模相差约 2.7 %；两者打满同一条环回路径。
- 65 536 字节大块一行波动最大（12.1–23.7 Gbps）。大写入放大了笔记本
  CPU 的调度噪声（见 §5）。
- 64 字节写入受包速率限制：无论带宽多少约 3.7 Mpps，即每段固定成本
  占主导。

### 3.2 RX worker 扩展性（`bench_multi_thread`，环境 A）

每个 worker 向自己的流注入 200 000 段；worker 并发作用于同一个栈。

| Workers | 总段数 | 墙钟时间 | 聚合 | kpps |
|---|---|---|---|---|
| 1 | 200 000 | 0.017 s | 135.18 Gbps | 11 573.6 |
| 8 | 1 600 000 | 0.189 s | 98.92 Gbps | 8 469.3 |

八个 worker 的聚合*低于*单 worker。这是当前实现在合成注入下的实测性质，
不是定律：各 worker 的流哈希到不同分片，该运行同时锻炼跨分片并行与共享
入口路径。引用多核结论时请据此措辞（§5）。

### 3.3 连接处理（`bench_syn_rate`、`bench_conns`，环境 A）

```
bench_syn_rate: 200000 conns in 0.353s = 566149 conns/s (completed=200000)
bench_conns:    10000 conns, 2000 rounds, 0.02 us/round, 0.0 ns/conn/round (idle)
bench_conns:    10000 conns, 500 rounds, 3481.35 us/round, 348.1 ns/conn/round (dirty)
```

空闲扫描每轮 O(1)（定时器轮快速路径）；dirty 扫描按活跃状态支付每连接
成本。

### 3.4 TUN 往返延迟（`interop_latency`，环境 B）

2000 次顺序 echo 往返，1 KB 载荷，内核客户端 ↔ XTCP 服务端经
`/dev/net/tun`。两次独立执行：

| 轮次 | mean | p50 | p95 | p99 | 抖动 (σ) | 失败 |
|---|---|---|---|---|---|---|
| 1 | 205.8 µs | 204.0 µs | 233.5 µs | 266.9 µs | 21.1 µs | 0 |
| 2 | 195.6 µs | 193.6 µs | 219.6 µs | 259.8 µs | 17.2 µs | 0 |

其中包含每轮两次 TUN 设备穿越与内核客户端自身的 socket 路径；这不是纯
栈内延迟数字。

### 3.5 TUN 批量吞吐（`interop_perf`，环境 B）

16 MB 单向传输，内核发送端 ↔ XTCP 接收端经 TUN：

| 轮次 | XTCP over TUN | 内核环回基线（同命令） |
|---|---|---|
| 1 | 1 806.42 Mbps | 30 932.36 Mbps |
| 2 | 1 791.84 Mbps | 21 148.88 Mbps |

基线行是同一样例打印的内核自身环回吞吐，用于标定。XTCP-over-TUN 达到
约 1.8 Gbps，瓶颈在 TUN 字符设备拷贝路径而非 TCP 处理：同一台机器的
进程内数字约为 25 Gbps（§3.1）。

### 3.6 公平性（`interop_fair`，环境 B）

TUN 路径上 `tbf 20mbit` + `netem delay 40ms` 整形下的四条并发流，各传
2 097 152 字节：

```
[fairness] flows=4 total_mbps=18.9
flow[0] 4.78 Mbps (25.3%)   flow[1] 4.75 Mbps (25.1%)
flow[2] 4.71 Mbps (24.9%)   flow[3] 4.67 Mbps (24.7%)
Jain index = 0.9999 ; all flows completed
```

## 4. 验证背景

正确性是有条件的时性能毫无意义，因此列出产生这些数字时成立的质量门：

- 全量确定性套件：Windows MSVC Release 注册项全部通过（`ctest`；含可选
  lwIP 差分目标 237/237，不含 236/236）。
- 同一套件 aarch64 静态交叉构建：`qemu-aarch64` 下 237/237 通过
  （80/80/77 三批顺序执行，零失败）。
- 精确源码版本的 CI：Linux（clang++ 与 g++）ASan+UBSan 与 Windows MSVC
  全绿。

## 5. 有效性威胁

在你引用这些数字之前先读这里：

1. **笔记本硬件。** 环境 A 是厂商性能档下的移动版 Zen 4。热状态会让单轮
   波动数十个百分点（§3.1 可见）；正因如此才报告"三轮最优"，且每一轮都
   公布。
2. **进程内环回不是网络。** `bench_throughput` 完全排除了网卡、DMA、内核
   栈与中断。它界定的是库自身的成本；不能外推为部署吞吐。
3. **WSL2 是虚拟化的。** TUN 数字包含 hypervisor 的设备模型。裸机 Linux
   上的绝对延迟（§3.4）会不同；有意义的是同一客户机上与内核环回基线的
   *对比*。
4. **合成注入 ≠ 应用负载。** §3.2 把预构造的段灌入 RX 路径。真实负载会
   交织系统调用、定时器与用户回调。
5. **单次公平性/延迟样本。** §3.4–3.6 中标注处报告了两轮，其余为单轮。
   它们证明的是数量级层面的行为与可复现性，不是统计严格性。
6. **尚无 DPDK/EFVI 后端**，因此 TUN 路径之外的线速声明根本无从谈起。

## 6. 复现

```bash
# 环境 A（Windows，MSVC Release）
cmake -B build -G "Visual Studio 17 2022" -A x64 \
      -DXTCP_BUILD_TESTS=ON -DXTCP_BUILD_BENCH=ON
cmake --build build --config Release
build\Release\bench_throughput.exe 67108864 1460 0 1        # §3.1 第 1 行
build\Release\bench_multi_thread.exe                        # §3.2
build\Release\bench_syn_rate.exe 200000                     # §3.3
build\Release\bench_conns.exe 10000 2000                    # §3.3 idle
build\Release\bench_conns.exe 10000 500 dirty               # §3.3 dirty

# 环境 B（WSL2，root 以使用 /dev/net/tun）
cmake -B build -DCMAKE_BUILD_TYPE=Release -DXTCP_BUILD_SAMPLES=ON
cmake --build build -j$(nproc)
sudo ./build/interop_latency     # §3.4
sudo ./build/interop_perf        # §3.5
sudo ./build/interop_fair        # §3.6
```

上文每张表对应的原始输出都来自本文所报告的运行；在相近硬件上重跑应落在
已公布的波动范围内。

## 7. 拥塞控制审计：在飞查询的热路径代价

对照 Linux v6.6 审计恢复/拥塞 episode 机制时（见
`docs/ARCHITECTURE_CN.md` 的「CA 状态机」一节），需要在多处读取
`tcp_packets_in_flight()`：损失降窗、速率采样、`CA_EVENT_TX_START` 门、
以及 RFC 6675 的 pipe 门。内核从 O(1) 计数器得到该值；最初实现改为遍历
`retrans_queue_`，即每次调用 O(N)，而它位于每 ACK 的路径上。

### 7.1 为什么 100 Gbps 下必须处理

100 Gbps、1 ms RTT 的路径在飞约 1.25 MB，即约 850 个 MSS 段留在重传队列。
在环境 B 硬件上实测一次队列遍历迭代的成本：**每段 1.56 ns**（探针
`deep_pipe2`，队列深度 10 下迭代 200 000 次，`taskset` 绑核）。

| 队列深度 | 队列遍历 | O(1) 计数器 | 倍数 |
|---|---|---|---|
| 10 | 15.58 ns | 2.04 ns | 7.7x |

外推到 100 Gbps BDP（约 850 段），一次遍历约 1.3 μs，而 ACK 路径每个 ACK
调用一至两次。三个量现已改为增量维护（`pipe_sacked_bytes_`、
`pipe_lost_bytes_`、`pipe_retrans_bytes_`），在队列或
`seg.sacked` / `seg.lost` / `seg.retries` 的每次变更处同步，查询因此为 O(1)。

### 7.2 端到端结果（无性能回退）

`bench_throughput 67108864 1460 0 1 kcc`（§3.1 第 1 行的配置），两个二进制
都 `taskset` 绑到同一核心、交替运行、取 6 轮最优：

| 版本 | 最优 | 比值 |
|---|---|---|
| 审计前（`7da6f59`） | 14 507.56 Mbps | 1.0000 |
| 审计后（本版本） | **15 423.28 Mbps** | **1.0631** |

在文档所用配置下，审计后的版本比审计前**更快**。本机单轮波动可达数十个百分点
（§5.1），因此只有绑核交替对比才有意义；未绑核的单次运行不作数。

### 7.3 增量计数器的正确性门

增量维护的求和只有在每次变更都重新记账时才安全。两个测试对此把关：

- `TestPipeCountersMatchWalk` 驱动具名状态迁移（部分 SACK、收窄 SACK、
  RTO 的 LOST+重传、累积 ACK 出队、全排空），每一步后断言 O(1) 值等于
  `PipeWalkReference()`——保留为「仅诊断用」oracle 的原始遍历。
- `TestPipeCountersRandomized` 执行 400 步确定性伪随机操作（发送、裸
  dup-ACK 与带 SACK 的 dup-ACK 突发、RTO、部分与完整 ACK），每一步后校验
  同一不变式。

删除约 20 处记账点中的任意一处，`TestPipeCountersRandomized` 会在到达它的
第一步即失败（已通过删除 RTO 的 `PipeReaccountSeg` 调用验证：
`diverged at step 0`）。


### 7.4 GCC/Clang 上的 SSSE3 拷贝+校验和

融合的「负载拷贝 + RFC 1071 累加」原本用 `__SSSE3__` / `_MSC_VER` 守卫。GCC 与
Clang 在默认 `-march` 下不定义 `__SSSE3__`，所以 Linux 上整条向量路径被编译掉，
每个数据段退回标量逐字节循环——callgrind 中它被折进 `BuildSegmentPacket`，占
全部指令的 28.05%（`bench_throughput 1048576 1024 0 1 kcc 0`）。MSVC 因
`_MSC_VER` 满足守卫不受影响，这正是同一份源码在 Windows 上比 WSL 快约 60% 的
原因。

守卫改为 `XTCP_HAS_SSSE3_CODE`，两个向量函数加函数级
`__attribute__((target("ssse3")))`（`XTCP_TARGET_SSSE3`）。翻译单元的全局 ISA
基线不变，运行时仍由 `CpuHasSsse3()` 选择，无 SSSE3 的 CPU 继续走标量回退。
`Checksum()` 另把「头部尺寸」输入（< 64 B：IPv4 伪首部 12 B、IPv4 头 20 B、
TCP 头 20 B）改走标量循环——该尺寸下向量建立/收尾比字加更贵。

确定性收益（callgrind，同一 1 MiB 负载，指令数）：

| 构建 | 总 Ir | 相对标量 |
|---|---|---|
| 标量（`ca519b7`） | 30 398 091 | 1.0000 |
| 启用 SSSE3 | **26 204 645** | **0.8620** |

`CopyAndChecksumSimd` 现为独立符号、占 10.46%（2.74 M Ir），不再折入
`BuildSegmentPacket` 的 28.05%；`ChecksumSimd` 已完全离开热点表。绑核交替的
墙钟数据落在 §5.1 的波动范围内，均值 +7%（8 轮，均值比值 1.0698）；指令数是
无噪声的判据。

正确性：`test_checksum_simd`（SIMD 与标量差分）通过，全矩阵绿（WSL Release
240/240、WSL ASan 240/240、MSVC Release 239/239、MSVC Debug 239/239）。


### 7.5 每次发送少读一次时钟

`MaybeMarkAppLimited` 为了做 pacing 截止时间比较又读了一次时钟，而
`SendData` 已经持有入口时间戳。KCC pacing 开启时，这等于每次应用发送多一次
vDSO `clock_gettime`；PMU 剖析（`perf record -F 5000 -e cycles`，128 MiB）显示
`__vdso_clock_gettime` 自身占 9.80%，是最大单项。现改为把时间戳透传（同一
时刻，语义不变）。

| 构建 | 总 Ir（callgrind，1 MiB） |
|---|---|
| `74b27ed` | 26 326 396 |
| 复用时间戳 | **26 122 982**（-0.77%） |

叠加 §7.4，总计 30 398 091 -> 26 122 982，相对标量构建 **-14.1%**。改动后
`__vdso_clock_gettime` 占周期 8.17%，IPC 1.76。剩下每段两次时钟读
（`XtcpStack::Send` 与 `OnPacket` 各一次）是 pacing 与 `sent_at` RTT/RACK 锚点
所必需。

剖析其余部分已很平坦：缓冲区引用计数约 14%（每次引用释放一次 `lock xadd` 加
缓存行流量，是池化引用计数设计的固有成本）、测试夹具 `Pump`/`Inject`/`memmove`
约 16%（ManualBackend 属测试基础设施）、`BuildSegmentPacket` 3.3%、
`SendData` 3.5%、`OnAckReceived` 3.2%、`KccMain` 2.4%、互斥锁 2.8%。已无单一
明显浪费项；再进一步需要改动并发或缓冲区生命周期，按超出范围处理。
