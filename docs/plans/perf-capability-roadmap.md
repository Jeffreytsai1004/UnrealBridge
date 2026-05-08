# Perf 能力扩展路线图（v2）

> **v1 状态：闭环删除（2026-05-08）**。v1 的 24 行 + 5 baseline = 29 ops 全部 shipped；
> v1 路线图文件按"shipped 即删"的工程惯例移除（同 enhanced-input / procedural-content
> 路线图的处置方式）。本文件 v2 启动新一轮 — 针对 AAA 量级项目的端到端诊断 + 自动归因。
>
> 目标：把 perf 工具从"point-in-time snapshot + 资产分解"提升到"agent 看一眼 trace
> 文件就能定位 hitch 根因、5GB 内存增量来源、加载 30s 卡哪里、replication 字节超
> 预算谁的责任"。
>
> 基于反思（2026-05-08，[与用户对话](#9-相关历史)）：现行 ops 覆盖单人/中型独立项目
> 良好，但 AAA 项目盲区集中在五处 ——**CPU trace 钻取深度不够、GPU timeline 完全没
> 走、loadtime/net/cook/alloc 四类专项 trace 0 覆盖、per-pass GPU 缺、hitch 现场无
> 自动归因**。
>
> 写于 2026-05-08。已交付 v1 + procedural-content + agent-capability-gaps Tier A
> 大部分项之后的代码状态。
>
> 与 `agent-capability-gaps.md` 的对接 —— A1（感知类）和 A11（自动化测试）部分项
> 与本路线图相关；不重复实现，本路线图只覆盖 Perf 维度。

---

## 1. 已交付基线（v1 closeout 2026-05-08）

`UnrealBridgePerfLibrary` 当前共 **29 ops**：

| 子模块 | ops | 函数 |
|---|---|---|
| Baseline (point-in-time) | 5 | `get_frame_timing` / `get_render_counters` / `get_memory_stats` / `get_u_object_stats` / `get_perf_snapshot` |
| M1 内存 + 资产分解 | 6 | `get_texture_memory_breakdown` / `get_mesh_memory_breakdown` / `get_world_actor_breakdown` / `get_uobject_memory_breakdown` / `get_audio_memory_breakdown` / `get_asset_size_top_n` |
| M2 时间序列 + 采样 | 7 | `start_perf_sampling` / `stop_perf_sampling` / `get_perf_sampling_state` / `get_frame_time_histogram` / `get_hitch_log` / `reset_frame_time_histogram` / `clear_hitch_log` / `export_perf_samples_to_csv` |
| M3 渲染细分 | 6 | `get_visible_primitives_by_material` / `get_actor_render_cost` / `get_lod_distribution` / `get_lumen_diagnostics` / `get_nanite_stats` / `get_shadow_caster_breakdown` |
| M4 UE Trace 集成 | 5 | `start_trace_capture` / `stop_trace_capture` / `get_trace_state` / `list_trace_channels` / `parse_trace_to_summary` |

**reference**：`.claude/skills/unreal-bridge/references/bridge-perf-api.md` (manifest 一致)。

**模块依赖**：`TraceLog`（runtime channel enum）+ `TraceServices`（offline parse）。

---

## 2. 大项目覆盖度评估（按子领域）

| 子领域 | 覆盖 | 关键缺口（agent 在 AAA 项目上做不了的事） |
|---|---|---|
| 内存快照 + 资产分解 | **80%** | 缺 texture streaming pool 驻留量、Render Target 内存、VRAM vs 系统内存区分、HLOD 大小、RVT 占用 |
| 帧时间 + hitch | **70%** | 缺 percentile（p50/p90/p95/p99 — 60Hz 目标看 max 没意义，看 p99 才真）；hitch 触发时无自动 trace 抓取 |
| 渲染细分 | **60%** | 缺 per-pass GPU 时间（BasePass / ShadowDepths / Lumen / Translucency 各几 ms）、material instruction count 全库扫、texture sampler 用尽检查 |
| CPU trace 钻取 | **50%** | `parse_trace_to_summary` 只 walk CPU timeline、只 inclusive、无 per-thread 拆分（GT / RT / RHI / WorkerThread）、无 callstack drill-down、无 exclusive time |
| GPU trace | **0%** | TimingProfiler `GetGpuQueueTimeline` 数据完全没读；agent 看 trace 等于盲一只眼 |
| 加载时间 | **0%** | `LoadTimeProfilerProvider` ready in TraceServices；30s cold-load 无法定位 |
| 网络复制 | **0%** | `NetProfilerProvider` ready；多人项目 replication 预算盲区 |
| 内存分配 trace | **0%** | `AllocationsProvider` ready；泄漏 + 短期峰值无法定位栈 |
| Cook / 打包 | **0%** | `CookProfilerProvider` ready；4h+ cook 优化盲区 |
| Hitch 自动归因 | **30%** | `hitch_log` 列时刻；缺 hitch 现场自动 trace + screenshot + memory delta 联合归因 |
| 跨快照 diff | **20%** | 有 `get_perf_snapshot` 但无 `compare_perf_snapshots(a, b)` util；改完手动比对 |

**单结论**：中型项目能用，AAA 量级缺关键诊断器。本路线图补 5 个里程碑（M5-M9）。

---

## 3. 设计原则

1. **复用 ParseTraceToSummary 已建的 TraceServices 入口**。M5-M6 的 trace 钻取全部
   通过同一个 `IAnalysisService::Analyze` + `FAnalysisSessionReadScope` 模式扩展，避免
   重新设计 trace 解析栈。
2. **不引入新 .uplugin 依赖**。所有 TraceServices provider 已经在 v1 link 进来；net
   / alloc / cook trace 由同一份 .utrace 解出，trace channel 是 captureargs。
3. **每个新 op 自带 per-call cost cap**：trace 解析 op 默认 timeout 60s + cap on
   trace 文件大小 1 GB（再大要么走异步、要么走外部 Insights）。
4. **超长 trace 走 worker thread parse**。`Analyze` 同步会卡 GT；M5 之后所有 trace
   解析 op 加 `bAsync=false` 默认 + `bAsync=true` opt-in（用 `FFunctionGraphTask` +
   `FEvent` wait）。
5. **5.7-only**。所有 v2 op 走 `#if !UE_VERSION_OLDER_THAN(5, 7, 0)` 整函数 gate，
   pre-5.7 路径由 `gen_version_stubs.py` 自动生成 stub（同 v1 M4-5 模式）。
6. **向 v1 USTRUCT 兼容性追加字段，不改字段语义**。下游 Python 脚本依赖现有结构。

---

## 4. 里程碑拆分

### M5 — Trace summary 深化（Tier 1：最高 ROI）

复用 `parse_trace_to_summary` 入口，扩字段 + 加新函数。每个项 ~100-300 行 cpp，0 新
USTRUCT-类型，单 commit 落地。

| # | 能力 | 工程量 | 备注 |
|---|---|---|---|
| M5-1 | `parse_trace_to_summary` 增 `GpuHotScopes: TArray<FBridgePerfHotScope>` 字段 | 小 | walk `ITimingProfilerProvider::GetGpuQueueCount` + `GetGpuQueueTimeline(QueueId)`；同 CPU 路径 aggregate |
| M5-2 | `parse_trace_to_summary` 增 `PerThreadHotScopes: TArray<FBridgePerThreadHotScopes>` 字段 | 中 | 用 `IThreadProvider::EnumerateThreads` 拿线程名 → 对每条 timeline 单独 aggregate top-N；GT / RT / RHI / WorkerThread 单独看 |
| M5-3 | `parse_trace_to_summary` 增 `Counters: TArray<FBridgePerfCounter>` 字段 | 小-中 | `ICounterProvider::EnumerateCounters` + `GetCounterMonotonicTimeline`；每个 counter 输出 min/max/avg/last |
| M5-4 | `get_frame_time_percentiles(p_list: TArray<float>)` → `TArray<float>` | 小 | 复用现有 always-on histogram；O(buckets) 累计 + 线性插值；p50/p90/p95/p99 一次返回 |
| M5-5 | `parse_trace_to_summary` 增 `LoadTimeBreakdown: TArray<FBridgePerfLoadTimeRow>`（top-N 慢加载包） | 中 | `ILoadTimeProfilerProvider::ReadEvents`；按 PackageName 聚合 LoadingTime；用户问"为啥 cold-load 30s" 直答 |

**M5 联合验收**：把现有 `parse_trace_to_summary` 输出的字段从 ~14 个扩到 ~22 个，agent
看一份 .utrace 就能回答"GT vs RT 哪个吃满"、"哪 5 个 package 拖累 cold-load"、
"60s 内 player.health counter 值变化"、"p99 frame time 多少 ms"。

工程量估计：~3 天（5 个子项；M5-2 略复杂因为要二次 aggregate）。

---

### M6 — 专项 trace 流派（Tier 2：单点价值高）

每个新增一个 ParseXxx 函数 + 对应 USTRUCT。trace 必须用对应 channel 抓（`memalloc`
/ `net` / `cook`），所以这些函数适合 hitch / leak / cook-debug 专项工作流。

| # | 能力 | 工程量 | 备注 |
|---|---|---|---|
| M6-1 | `parse_alloc_trace_to_summary(utrace_path, top_n=50)` → `FBridgePerfAllocSummary` | 中-大 | `IAllocationsProvider`；输出未释放分配 top-N（按 size、按 callstack）+ peak total；trace 必须含 `memalloc` channel |
| M6-2 | `parse_net_trace_to_summary(utrace_path, top_n=50)` → `FBridgePerfNetSummary` | 中 | `INetProfilerProvider`；输出 per-actor 复制字节、最贵 RPC、replication frequency；trace 必须含 `net` channel |
| M6-3 | `parse_cook_trace_to_summary(utrace_path, top_n=50)` → `FBridgePerfCookSummary` | 中 | `ICookProfilerProvider`；输出 per-asset cook duration top-N、shader compilation 阶段时间；trace 必须含 `cook` channel |

**M6 联合验收**：分别从专项 trace 输出"哪个内存分配栈在泄漏"、"哪个 actor replication
吃 30% 带宽"、"4h cook 里 2.5h 是 shader compile"。

工程量估计：~1 周（每子项 2-3 天，主要在 USTRUCT 设计 + provider walk + 测试 trace 准备）。

---

### M7 — 流送 + Per-pass GPU（Tier 2：runtime perf 关键）

不依赖 trace 文件，是 live editor / PIE 状态查询。

| # | 能力 | 工程量 | 备注 |
|---|---|---|---|
| M7-1 | `get_texture_streaming_residency(top_n=30)` → `TArray<FBridgeTextureStreamingRow>` | 中 | `IStreamingManager` + `IStreamingTexture` 遍历；每个 texture 输出 (resident_mip, wanted_mip, resident_kb, wanted_kb, last_visible_at)；找"流送压力下哪些贴图掉档" |
| M7-2 | `get_render_target_memory()` → `FBridgeRenderTargetMemory` | 小-中 | `FRenderResource` 全局表 + RT 资源分类；输出 GBuffer / shadow atlas / Lumen surface cache / VT / lighting cache 各自 MiB |
| M7-3 | `get_per_pass_gpu_timings(viewport_index=0)` → `TArray<FBridgeGpuPassTiming>` | 中-大 | RDG profiler reading；输出 BasePass / ShadowDepths / Lumen / Translucency / PostProcess 等 ~15 个标准 pass 的最近一帧 GPU ms；需 RT 同步（`ENQUEUE_RENDER_COMMAND` + `FEvent`） |
| M7-4 | `analyze_all_materials(top_n_by_instructions=30)` → `TArray<FBridgeMaterialPerfRow>` | 中 | 跨整库扫描 master material；走现有 `MaterialEditingLibrary` 的 `GetRepresentativeInstructionCounts` + sampler count；输出 worst-N |

**M7 联合验收**：agent 不开 Insights 直接答"GBuffer 占 250 MB"、"BasePass 4.2 ms"、
"M_Foliage_Master 是 instruction 大户（612 条）"。

工程量估计：~5 天（M7-3 RT sync 是大头）。

---

### M8 — 自动归因 + 跨快照 diff（Tier 3：流程性 high-leverage）

| # | 能力 | 工程量 | 备注 |
|---|---|---|---|
| M8-1 | `begin_auto_hitch_capture(threshold_ms=50, ring_seconds=10, output_dir)` / `end_auto_hitch_capture()` | 大 | hook `OnEndFrame`，frame > threshold 时把过去 `ring_seconds` 的 trace events flush 到 .utrace 文件 + 同步 screenshot；需 ring-buffer trace channel |
| M8-2 | `compare_perf_snapshots(a: FBridgePerfSnapshot, b: FBridgePerfSnapshot)` → `FBridgePerfSnapshotDelta` | 小-中 | per-field diff + flag 显著回归（>10% 阈值）；与 M2 的 perf_sampling 配合，PR 自动 baseline 比较 |
| M8-3 | `begin_insights_for_trace(utrace_path)` → `bool` | 小 | shell out `UnrealInsights.exe -OpenTraceFile=<path>`；agent 走不下去时让人接手 |

**M8 联合验收**：长期 PIE 跑测里 hitch 一发生立刻有"现场 trace + 当帧 screenshot"
对照 PR；改完代码自动 perf 回归比对。

工程量估计：M8-1 ~1 周（ring-buffer 是 UE Trace 不直接支持的特性，要么用短 trace
循环重写，要么 link `FTraceAuxiliary::Pause/Resume`）；M8-2 / M8-3 各 1 天。

---

### M9 — 端到端 PIE 性能预算闭环（推后 / 占位）

构思：`run_perf_budget(level, profile, duration_seconds, budget: FPerfBudget)` —
启动 PIE → 自动驾驶（已有 reactive handlers + IA 注入） → N 秒后采集 perf snapshot
→ 与 budget 比较 → 输出 pass/fail + delta 表 + screenshot。

工程量大（需要 PIE 自动化驱动），暂不立项；先看 M5-M8 落地后是否真有 demo / studio
驱动场景。

---

## 5. 跨里程碑非功能要求

### 性能预算

| 操作类 | 期望成本 | 频次约束 |
|---|---|---|
| M5 trace summary 增量字段 | +50-200 ms（在原 `parse_trace_to_summary` 基础上）| 同原函数；不限 |
| M5-4 frame_time_percentiles | < 0.5 ms | 不限 |
| M6 专项 trace parse | 1-30 s（取决于 trace 大小 + channel）| 用户显式调，不要塞到周期采样 |
| M7-1 streaming residency | 5-50 ms | < 1 Hz |
| M7-3 per-pass GPU | 1-5 ms（RT 同步开销）| < 1 Hz；不要塞到 M2 周期采样 |
| M8-1 auto-hitch capture | hook 加每帧 < 0.05 ms；触发时 50-200 ms 写盘 | 一直开；触发受 threshold 控制 |
| M8-2 compare_perf_snapshots | < 1 ms | 不限 |

### Threading 模型

| 操作 | 调用线程 | 内部访问 | 同步原语 |
|---|---|---|---|
| M5 trace summary | bridge worker → GT | TraceServices session 自带锁 | `FAnalysisSessionReadScope` (RAII) |
| M6 专项 parse | bridge worker → GT | 同上 | 同上 |
| M7-1 streaming | GT only | `IStreamingManager` 单线程 | 无 |
| M7-2 RT memory | GT (cached values) | 已 publish 的 stats | 无 |
| M7-3 per-pass GPU | GT → ENQUEUE → RT | `FScene` proxies | `FEvent` |
| M8-1 hook | OnEndFrame (GT) | trace ring buffer | 内部 lock-free queue |

### 跨版本支持

5.7-only。pre-5.7 走 `gen_version_stubs.py` 自动 stub。

---

## 6. 已知坑（每子项独立）

1. **M5-2 per-thread aggregation memory 大**：5GB trace 文件下，每条 timeline aggregate
   top-N 容器要 cap，避免 RAM 炸；预计每线程 ~256 KB top-N buffer 上限。
2. **M5-5 LoadTime 数据要 trace 含 `loadtime` channel**。这点跟 M6 的专项 trace 一样
   ——抓 trace 时通道选错就拿不到。文档明示。
3. **M6-1 AllocationsProvider 数据量极大**（每个分配一个事件）。1 GB 内存活动 = 数
   GB trace。Cap on trace 大小 5 GB；超出就 fail-fast 让用户用 cap 模式抓 trace。
4. **M7-3 per-pass GPU 跨 5.7 minor 不稳定**。RDG profiler API 在 5.7 是预览状态，5.8
   可能改 sig。文档写明"5.7.x only" + 跟随 stable 升级。
5. **M8-1 ring-buffer trace** UE 5.7 没有原生支持。要么 hack `FTraceAuxiliary::Pause/
   Resume` + 短文件 rolling，要么 hook trace channel 输出 stream 自己 buffer。后者复
   杂；前者 trace 起点不准（粒度 = 一次 Pause 周期）。先做粒度差版本，文档明示。
6. **M5-3 counters 每秒 100 Hz × 10k counters → trace 体积爆炸**。读 trace 时按
   `ICounterProvider::ReadCounter(idx)` 单步读，避免一次性加载全 timeline。
7. **`ParseTraceToSummary` 自身的 `Analyze` 是同步阻塞 bridge exec**（已知约束 in v1
   doc）。M5-M6 的所有新 parse 函数继承这个限制，文档统一交叉引用
   `feedback_bridge_exec_holds_gamethread`。

---

## 7. 优先级 / 实施顺序

### P0 — 解锁"AAA agent 自服务诊断"

| 排名 | 项 | 估时 | 解锁 |
|---|---|---|---|
| 1 | **M5-1 GPU timeline + M5-2 per-thread**（同 commit）| 1.5 d | trace summary 完整度 50% → 80%；agent 能区分 CPU vs GPU 瓶颈 |
| 2 | **M5-4 frame percentiles** | 0.5 d | AAA 真实帧分布有数；hitch 调查从"看 max"升到"看 p99" |
| 3 | **M5-5 LoadTime 集成** | 1 d | 大世界 cold-load 30s+ 的逐 package 归因 |

**P0 验收**：agent 拿一份 .utrace（cpu+gpu+frame+loadtime+counter channels），单
`parse_trace_to_summary` 调用回答 90% 常见 perf 问题。

### P1 — 闭环 AAA 多人 + 内存

M5-3 counters + M6-1 alloc trace + M6-2 net trace。聚焦多人项目 + 长期内存稳定性。

### P2 — 渲染深入 + cook

M7-3 per-pass GPU + M7-4 material analyzer + M6-3 cook trace。给程序员/TA 用。

### P3 — 流程闭环

M8-1 auto-hitch + M8-2 snapshot diff。需要 PR 流程 / CI 接入才有价值，先看用户工作
流。

### 后续观察项（推后）

- M9 端到端 PIE 性能预算闭环
- runtime virtual texture (RVT) 内存分解
- HLOD layer breakdown
- Audio mixer load
- Niagara/VFX budget query

---

## 8. 排除项（不做）

明确不做的，避免重复立项 / scope creep：

- **重复 v1 的功能**（已 ship 29 ops 都不动）
- **第三方 profiler 集成**（PIX / RenderDoc / Aftermath）—— 那是用户用 Insights / DX
  GUI 的领域，bridge 不复制
- **Live shader profiler / shader complexity heatmap**（A1-#2 GBuffer 已部分覆盖
  shader_complexity view mode；本路线图不重做）
- **Crash dump 解析**（agent-capability-gaps.md A3-#13 单独立项，不在本路线图）
- **cooked package 大小回归 alarm**（属于 packaging / DevOps，非 perf）
- **UE 5.6 / 5.5 backport** —— 整 v2 锁 5.7-only 跟随 v1 模式

---

## 9. 相关历史

- 2026-05-08 v1 闭环（24/24 ops 全交付，删除 v1 路线图）
- 2026-05-08 反思讨论（这文档的母对话）—— 用户问"还能拓展什么 / 大项目能用吗"，
  识别五处 AAA 盲区
- `agent-capability-gaps.md` A1（感知类）/ A6（自动化测试）/ A11（Webhook 通知）—
  本路线图与之正交，仅 Perf 维度
- `feedback_bridge_exec_holds_gamethread.md` —— M5-M6 trace parse 函数的同步约束
- `feedback_property_importtext_silent_zero.md` —— v1 sub-agent 测试发现的 cross-cut
  feedback；M8-2 snapshot diff 实现时复用相同 snapshot/restore 模式

---

## 10. 验证 checklist（每个里程碑落地时跑）

- [ ] `python tools/build_matrix.py` 5.3-5.7 全过 -Rocket
- [ ] `python tools/gen_version_stubs.py` 同步（M5-M8 所有新 op 加进 PerfLibrary
      `scope=functions` 列表）
- [ ] `python tools/gen_manifest.py` 同步（manifest reflect 新函数）
- [ ] `python tools/audit_tech_debt.py` 看新增代码无 LOG_TEMP / TODO 残留
- [ ] `bridge.py exec --stdin` 跑里程碑端到端 smoke
- [ ] `bridge-perf-api.md` 与新签名对齐
