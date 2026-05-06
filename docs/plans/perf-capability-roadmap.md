# Perf 能力扩展路线图

目标：把 perf 接口从"现在多少"扩到 4 个新维度——**WHAT**（什么占用了内存/磁盘）、**WHEN**（什么时候出问题）、**WHERE**（cost 花在哪个 actor/material/pass）、**WHY**（UE 内部 trace 视角）。典型 agent 任务形态：

- **诊断**："这个项目 cook 出来 8GB，哪些 texture 占大头？" / "PIE 跑 60s 掉了几次帧？" / "改完渲染管线哪里多了 5ms？"
- **回归**：改完一批 material / BP 后比对之前快照，自动判 "draw call 多了 30%" / "memory 涨了 200MB"
- **优化**：知道 hottest 的 material / actor / asset，agent 决定先动谁

硬约束：所有读操作 **零副作用**（不触发渲染、不强制加载未加载资产、不开新线程）；时间序列采样 GameThread 只占 < 1% 帧时间；trace 集成只在用户显式 start/stop 时启用；跨版本支持 5.3-5.7（个别 RHI/Lumen/Trace API 只能 5.7+ 时整段 gate）。

最后更新：2026-05-06（v0.1 — 路线图建立。M1-M4 全部待实现；现状只覆盖 point-in-time 5 个 UFUNCTION）。

---

## 现状基线（2026-05-06）

`UnrealBridgePerfLibrary`（2026-04-20 交付）已覆盖**单点采样**：

| UFUNCTION | 输出 USTRUCT | 来源 |
|---|---|---|
| `get_frame_timing` | `FBridgeFrameTiming` (FPS / FrameMs / GT / RT / GPU / RHI / DeltaSeconds / FrameNumber / bSmoothed) | `FStatUnitData` 优先；fallback 到 `GGameThreadTime` / `GRenderThreadTime` / `RHIGetGPUFrameCycles` |
| `get_render_counters` | `FBridgeRenderCounters` (DrawCalls / PrimitivesDrawn / NumGpus) | `GNumDrawCallsRHI` / `GNumPrimitivesDrawnRHI` 跨 `MAX_NUM_GPUS` 求和 |
| `get_memory_stats` | `FBridgeMemoryStats` (UsedPhysical/Virtual + Peak + Available + Total，MiB) | `FPlatformMemory::GetStats` |
| `get_u_object_stats` | `FBridgeUObjectStats` (TotalObjects / UniqueClasses / TopClasses) | `TObjectIterator<UObject>` 全量遍历，按 `GetClass` 计数 |
| `get_perf_snapshot` | `FBridgePerfSnapshot` (上 4 项聚合 + ISO-8601 时间戳 + 引擎版本 + bWasInPie) | 复合 |

**未覆盖**的能力空白：

- **资产/内存细分**：哪个文件夹下的 texture 占了 2GB？哪类 mesh 总内存最大？哪个 level 装了多少 actor？
- **时间序列**：过去 60 秒 FPS 怎么变？hitch 多少次？60 秒内内存涨了多少？
- **空间细分**：当前帧每个 material 贡献多少 draw call / primitive？哪个 actor 是渲染大头？
- **UE Insights / Trace**：能不能让 agent 触发一次 trace 然后读结果摘要？

下面 M1-M4 按风险/收益排序。

---

## 设计原则

1. **扩展现有 `UnrealBridgePerfLibrary`，不另起新库**。该库现在 5 个 UFUNCTION，扩到 ~25 个仍在合理规模内（对比 `UnrealBridgeBlueprintLibrary` 200+ 个）。perf 主题集中查找体验更好。
2. **disk vs runtime 内存分开**：内存类查询都带 `mode` 参数 ∈ `"runtime"`（已加载对象）/ `"disk"`（AssetRegistry 元数据，无需 load）。AAA 项目通常想要 disk 视角。默认 `"disk"`。
3. **采样不引入新线程**。Phase 2 的周期采样走 `FTSTicker::GetCoreTicker()`（GT），帧时间统计走 `FCoreDelegates::OnEndFrame`（GT）。所有数据落 ring buffer，复用 `FBridgeCallLog`（2026-04-20 已落地）的成熟模式。
4. **RT 同步只在显式渲染细分时使用**。Phase 3 的 `ENQUEUE_RENDER_COMMAND` 会卡 GameThread 几毫秒，必须显式调用、不能塞到周期采样里。
5. **Lumen / Nanite / Trace 这些 5.7-only API 走整库块 gate**。沿用 `Chooser` / `PoseSearch` 的 `#if !UE_VERSION_OLDER_THAN(5, 7, 0)` 整段 wrap 模式，5.3-5.6 路径上对应 UFUNCTION 直接消失（Stub 版返回 success=false + "not supported on this engine version"）。
6. **每个 UFUNCTION 自带 cap**：`top_n` / `max_results` 必填且 clamp，避免被 agent 不小心拉爆 token。

---

## 里程碑拆分

### M1 — 内存与资产分解（WHAT 占了空间）

最先做的原因：**纯 introspection，零渲染管线 hook，零 PIE 依赖**。AssetRegistry + `GetResourceSizeBytes` 两个底层 API 就能交付，跨版本风险最低。一次 commit 落地一个子项很自然。

| # | 能力 | 工程量 | 备注 |
|---|---|---|---|
| M1-1 | `get_texture_memory_breakdown(group_by, mode='disk', max_groups=50)` → `TArray<FBridgePerfBreakdownRow>` | 小-中 | `group_by` ∈ `"folder"` (按 PackagePath) / `"lod_group"` (TC_Default / TC_Masks / TC_Normalmap...) / `"compression_format"` (PF_BC1 / PF_DXT5...) / `"sampler_type"`. disk 模式只读 `FAssetData::TagsAndValues`（cooked tags 包含 `Format` / `LODGroup` / `Width` / `Height` / `MaxInGameSize`），无 LoadObject |
| M1-2 | `get_mesh_memory_breakdown(group_by, mesh_type='all', mode='disk')` | 小-中 | mesh_type ∈ `"static"` / `"skeletal"` / `"all"`. group_by ∈ `"folder"` / `"lod_count"` / `"vertex_count_bucket"`. runtime 模式走 `UStaticMesh::GetResourceSizeBytes(EResourceSizeMode::EstimatedTotal)` |
| M1-3 | `get_world_actor_breakdown(level_filter='', group_by='class')` | 小 | group_by ∈ `"class"` / `"level"` / `"level_class"`. 走 `World->GetLevels()` 含 streaming sublevels；World Partition 项目额外走 `UWorldPartition::GetActorDescContainer()` 列出未加载 actor descs |
| M1-4 | `get_uobject_memory_breakdown(top_n=20, mode='runtime')` | 小 | 在现有 `get_u_object_stats` 之上**增加** `total_bytes` 字段（按类累加 `GetResourceSizeBytes(Exclusive)`）。runtime-only（disk 模式无意义） |
| M1-5 | `get_audio_memory_breakdown(group_by='compression_format', mode='disk')` | 小 | SoundWave 的 disk size 通常 cook 时压缩成 OGG/Opus，AssetRegistry tag 里有 `NumChannels` / `SampleRate` / `Duration` / `CompressionName` |
| M1-6 | `get_asset_size_top_n(class_filter='', top_n=50)` | 小 | 跨任意 UClass 的"最大 N 个 asset"。走 `AssetRegistry.GetAssetsByClass` + `GetAssetSizeOnDisk`（5.4+；5.3 用 `IFileManager::FileSize` on `FPackageName::LongPackageNameToFilename`）|

新增 USTRUCT：

```cpp
USTRUCT(BlueprintType)
struct FBridgePerfBreakdownRow
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf") FString Key;          // e.g. "/Game/Characters/Hero" or "TC_Normalmap" or "PF_BC5"
    UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf") int32   Count = 0;    // assets/objects in this group
    UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf") int64   TotalBytes = 0;
    UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf") TArray<FString> SamplePaths;  // first 3 paths for "show me what's in here"
};
```

里程碑产出验收：能一次调用回答"这个项目里 _所有 normalmap_ 总共占多少 disk，最大的 5 张是哪些"、"`/Game/Levels/Forest` 下 actor 数量按 class 排"。

工程量估计：~3 天（每个子项 ~30-80 行 cpp，含跨版本 GetAssetSizeOnDisk shim）。

### M2 — 时间序列与采样（WHEN 出问题）

依赖 M1 的 baseline 数据 shape，但实现完全独立。可以与 M1 并行做。

| # | 能力 | 工程量 | 备注 |
|---|---|---|---|
| M2-1 | `start_perf_sampling(period_ms=100, max_samples=600, include_uobject_stats=false)` | 中 | `FTSTicker::GetCoreTicker().AddTicker` 周期采 `get_perf_snapshot`；环形缓冲到 `max_samples`（默认 600 = 60s @ 100ms）；include_uobject 默认 false（避免 100ms 周期内做 200ms 的 TObjectIterator 全量扫）|
| M2-2 | `stop_perf_sampling()` → `TArray<FBridgePerfSnapshot>` | 小 | 释放 Ticker handle，返回缓冲全量 |
| M2-3 | `is_perf_sampling_active()` → `{active, started_at_utc, samples_collected, period_ms}` | 小 | 状态查询；agent 检查"我之前是不是已经开了一个采样" |
| M2-4 | `get_frame_time_histogram(bucket_ms=5, max_bucket=100)` → `TArray<FBridgeHistogramBucket>` | 中 | 自上次 reset 起累积；hook `FCoreDelegates::OnEndFrame`，每帧把 GFrameTime 投到桶里。固定桶数组（max_bucket/bucket_ms + 1 个 overflow），无运行时分配 |
| M2-5 | `get_hitch_log(threshold_ms=50, max_entries=50)` → `TArray<FBridgeHitchEntry>` | 中 | 同 hook，但只记录超阈值帧；ring buffer 默认 50 条 |
| M2-6 | `reset_frame_time_histogram()` / `clear_hitch_log()` | 小 | 测试新 baseline 前清掉旧数据 |
| M2-7 | `export_perf_samples_to_csv(output_path)` | 小 | 把当前采样缓冲序列化成 CSV，便于外部 Excel/Plotly 看趋势 |

新增 USTRUCT：

```cpp
USTRUCT(BlueprintType)
struct FBridgeHistogramBucket
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf") float LowerMs = 0.f;     // bucket lower edge inclusive
    UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf") float UpperMs = 0.f;     // bucket upper edge exclusive (FLT_MAX for overflow)
    UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf") int32 Count = 0;
    UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf") float Percent = 0.f;     // pre-computed; fraction of total frames in this bucket
};

USTRUCT(BlueprintType)
struct FBridgeHitchEntry
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf") int64  FrameNumber = 0;  // GFrameCounter
    UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf") double TimestampSeconds = 0.0;
    UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf") float  GameThreadMs = 0.f;
    UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf") float  RenderThreadMs = 0.f;
    UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf") float  GpuMs = 0.f;
    UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf") float  TotalMs = 0.f;
};
```

**生命周期分两类**：

- **opt-in 采样**（M2-1..3, 7）：用户显式 start/stop 才工作；`stop_perf_sampling` 自动释放 ticker，重复调用幂等
- **always-on 累积**（M2-4..6）：hook `OnEndFrame`，从 module startup 起一直统计；提供 reset 给"我现在要新 baseline" 用例

里程碑产出验收：用户跑 PIE 60 秒后，一次调用拿到「frame time 分布直方图 + 5 次 hitch 的具体时刻 + 60 个 100ms 节点的完整 perf snapshot」。

工程量估计：~4 天（FTSTicker 周期 hook + ring buffer + CSV 导出 + hitch detector）。

### M3 — 渲染细分（WHERE cost 花掉）

风险最高的一步。`FScene` / `FPrimitiveSceneProxy` 在 RT 上活，要走 `ENQUEUE_RENDER_COMMAND` + 同步等结果，跨版本访问器有变化。

| # | 能力 | 工程量 | 备注 |
|---|---|---|---|
| M3-1 | `get_visible_primitives_by_material(viewport_index=0, top_n=50)` | 大 | RT 遍历 `FScene->Primitives`，每个 proxy 查 `GetMeshDescription` 拿 material，按 material 聚合 primitive_count + total_triangles + 3 个 sample actor paths |
| M3-2 | `get_actor_render_cost(actor_path)` → `FBridgeActorRenderCost` | 中 | GT 调用：从 `AActor` 取所有 `UPrimitiveComponent`，每个查 `GetNumMaterials` + `GetStaticLODs` + `bCastsDynamicShadow`。无 RT 依赖（用组件缓存的 desc，不是 RT live state），但拿不到当前帧 culled 状态 |
| M3-3 | `get_lod_distribution(class_filter='', actor_filter='')` | 小-中 | 遍历 PIE world 里的 static/skel mesh actors，每个读当前 LOD index 累计；输出每 mesh asset 的 LOD 分布直方图 |
| M3-4 | `get_lumen_diagnostics()` （5.7+ gate）| 中 | `FLumenSceneData` / `FLumenViewState` 内部状态：surface cache 大小（MiB）、screen probe 数量、final gather quality。需 RT 同步 |
| M3-5 | `get_nanite_stats()` （5.7+ gate）| 中 | `FNaniteScene` 内部：cluster 数量、加载的 streamable pages、virtual texture 占用。需 RT 同步 |
| M3-6 | `get_shadow_caster_breakdown(top_n=30)` | 中 | 按"投射动态阴影的 primitive"聚合，输出 actor + shadow cost 估算（基于 cascade splits / VSM page count）|

新增 USTRUCT：

```cpp
USTRUCT(BlueprintType)
struct FBridgeMaterialRenderRow
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf") FString MaterialPath;
    UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf") int32   PrimitiveCount = 0;
    UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf") int64   TotalTriangles = 0;
    UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf") TArray<FString> SampleActorPaths;  // first 3 actors using this material
};

USTRUCT(BlueprintType)
struct FBridgeActorRenderCost
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf") FString ActorPath;
    UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf") int32   PrimitiveComponentCount = 0;
    UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf") int32   MaterialSlotCount = 0;
    UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf") int64   EstimatedTriangleCount = 0;
    UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf") bool    bCastsDynamicShadow = false;
    UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf") TArray<FString> Materials;
};
```

里程碑产出验收：当前帧画面里"哪个 material 是 draw call 大头"、"哪个 actor 总三角面数最高"，agent 能一次性拉到。

工程量估计：~1 周（RT 同步 helper + 跨版本 FScene 访问器适配 + Lumen/Nanite gating + 6 个 UFUNCTION）。

**重要提醒**：`ENQUEUE_RENDER_COMMAND` 同步等结果会卡 GT 1-5ms（取决于 scene 复杂度），**不能塞到 M2 周期采样里**——只在 agent 显式调 M3 函数时执行。

### M4 — UE Trace / Insights 集成（WHY — 火焰图证据）

最重的一步。Trace 本身集成不难（FTraceAuxiliary 5.4+ 直接暴露），难点在 utrace 文件离线 parse。

| # | 能力 | 工程量 | 备注 |
|---|---|---|---|
| M4-1 | `start_trace_capture(channels, output_dir='', max_size_mb=500)` (5.4+ gate) | 中 | `FTraceAuxiliary::Start(EConnectionType::File, *FilePath, ...)`. channels ∈ `"cpu"` / `"gpu"` / `"frame"` / `"rdg"` / `"loadtime"` / `"memalloc"` / `"gameplay"` / `"slate"`. 5.3 fallback 走 `GEngine->Exec(World, TEXT("Trace.Start File=..."))` |
| M4-2 | `stop_trace_capture()` → `{path, size_bytes, duration_seconds, channels[]}` | 小 | `FTraceAuxiliary::Stop()`. 返回真实写出的 .utrace 路径供下一步 parse |
| M4-3 | `is_trace_active()` → `{active, started_at_utc, current_size_bytes, channels[]}` | 小 | 状态查询 |
| M4-4 | `list_trace_channels()` → `TArray<FString>` (5.4+ gate) | 小 | 走 `Trace::EnumerateChannels` 列出本机 UE 知道的所有 channel 名+ 当前是否 enabled |
| M4-5 | `parse_trace_to_summary(utrace_path, top_n=20)` | 大 | shell-out 到 `UnrealInsights.exe -OpenTraceFile=<path> -Quit -ExportTo=...`（5.7+ 才稳）；fallback：直接 link `TraceServices` 模块走原生 reader 抽 frame timing + top-N hot scopes |

里程碑产出验收：agent 能调 `start_trace_capture(["cpu","gpu","frame"])` → 等 PIE 跑 30 秒 → `stop_trace_capture()` → `parse_trace_to_summary` 返回"top 10 hot CPU scopes by total time"。

工程量估计：~1.5 周（M4-1..4 共 4 天；M4-5 单独 1 周——utrace 二进制 parse 是大头，可能要 link `TraceServices` 模块）。

如果 M4-5 太重可以**先只做 M4-1..4**——agent 至少能采集 trace 文件，由用户自己拿 Insights 看。

---

## 跨里程碑非功能要求

### 跨版本支持矩阵

| API | 5.3 | 5.4 | 5.5 | 5.6 | 5.7 | 处理 |
|---|---|---|---|---|---|---|
| `UTexture::GetResourceSizeBytes` | ✓ | ✓ | ✓ | ✓ | ✓ | 全版本 OK |
| `IAssetRegistry::GetAssetSizeOnDisk` | — | ✓ | ✓ | ✓ | ✓ | 5.3 用 `IFileManager::FileSize` shim |
| `FAssetData::TagsAndValues["LODGroup"]` | ✓ | ✓ | ✓ | ✓ | ✓ | 全版本 OK（部分 tag 名 5.5 起转 FName 化，访问代码同） |
| `FTSTicker` / `FCoreDelegates::OnEndFrame` | ✓ | ✓ | ✓ | ✓ | ✓ | 全版本 OK |
| `FScene` 主结构 + `Primitives` 数组 | ✓ | ✓ | ✓ | ✓ | ✓ | 概念稳定，访问器细节 5.4 起略变（需要 inline shim 不超过 2 处） |
| `FLumenSceneData` 公共状态 | partial | partial | partial | ✓ | ✓ | 5.6+ 才是 stable public；M3-4 整段 5.7+ gate |
| `FNaniteScene` 公共状态 | partial | ✓ | ✓ | ✓ | ✓ | API 5.4+；为统一安全 M3-5 也走 5.7+ gate |
| `FTraceAuxiliary::Start` | — | ✓ | ✓ | ✓ | ✓ | 5.4+；5.3 fallback `GEngine->Exec("Trace.Start File=...")` |
| `UnrealInsights -ExportTo=` | — | — | — | — | partial | 5.7 partial；可靠 export 只在 5.8（未来）。M4-5 这条 fallback 要做（直接 link TraceServices）|

### 性能预算

| 操作 | 单次成本 | 频次上限建议 |
|---|---|---|
| M1 资产分解（typical 5k texture）| ~50-200 ms（AssetRegistry 主导）| 不限，但建议每次手动调 |
| M2 周期采样 tick（不含 uobject）| < 0.5 ms | 100 ms 周期 = 0.5% 帧时间，可常开 |
| M2 + include_uobject | 50-200 ms | **不要** 100 ms 周期；建议 ≥5s 周期 |
| M2 帧时间 histogram + hitch（OnEndFrame）| < 0.05 ms | 每帧免费 |
| M3 RT 同步遍历 | 1-5 ms | 不超过 1Hz；不要塞到周期采样 |
| M4 trace（cpu+gpu+frame channels）| 5-15% 帧时长 overhead | 用户显式 start，自动定上限尺寸 |

### 内存预算

- M2 周期 ring buffer：600 sample × ~250 byte = ~150 KB（不含 uobject stats）；启用 uobject 时单 sample 多 ~5 KB（top_20 classes），600 sample = ~3 MB
- M2 hitch log：50 entry × ~64 byte = ~3 KB
- M2 frame time histogram：21 bucket × ~16 byte = ~340 byte
- M4 utrace：CPU+GPU+frame 三 channel 大约 30 MB/分钟。`max_size_mb` cap 默认 500（~16 分钟）

### Threading 模型

| 函数 | 调用线程 | 访问的状态 | 同步方式 |
|---|---|---|---|
| M1 全部 | GT | AssetRegistry / 已加载 UObject | 无（AR 内部线程安全）|
| M2 `start/stop_sampling` | GT | Ticker handle + ring buffer | atomic ops |
| M2 `OnEndFrame` hook | GT | histogram + hitch ring | 该 delegate 本来就 GT |
| M3-1, M3-4..6 | GT 调用 → ENQUEUE → RT 执行 → GT 阻塞等 | RT-side `FScene` | `FEvent` + `FPlatformProcess::GetSynchEventFromPool` |
| M3-2, M3-3 | GT only | UPrimitiveComponent 缓存属性 | 无 |
| M4-1, M4-2 | GT | `FTraceAuxiliary` 全局态 | UE 内部线程安全 |

---

## 已知 lock

- **disk size 在 5.3 没有 `IAssetRegistry::GetAssetSizeOnDisk`**——shim 走 `IFileManager::Get().FileSize(*FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension()))`，慢但准。M1-1 / M1-6 的 disk 模式在 5.3 上单次扫 5k asset 大约比 5.4+ 慢 3 倍。
- **`UTexture::GetResourceSizeBytes(EResourceSizeMode::EstimatedTotal)` 只反映 _已加载_ 的对象**。runtime 模式下未加载的 texture 报 0；agent 看到"内存好像不大"是因为没 load。要么显式 LoadObject + 加载完成等（贵），要么改用 disk 模式（推荐）。
- **`FAssetData::TagsAndValues` 在未保存的 asset 上为空**。新建但没 save 的 texture/mesh 在 disk 模式下被跳过（disk size = 0）。注释里写明：disk 模式只统计**已 save 到磁盘**的资产。
- **World Partition 的 actor 枚举**和传统 sublevel 完全不同。M1-3 的 group_by="level" 在 WP 项目上要走 `UWorldPartition::GetActorDescContainer()`，`Container->ForEachActorDescContainerInstance()`；普通 `World->GetLevels()` 只返回当前 streamed-in 的 actor，会少计。需要分支处理。
- **M2 sampling 启动后崩溃 / 编辑器关闭**：Ticker handle 必须在 module shutdown 时释放（注册到 `FCoreDelegates::OnPreExit` 或 module's `ShutdownModule`），否则 hot-reload 后 stale handle 还在跑。
- **M3 `FScene` 访问 5.4 起加了 const-only 入口**：原 `World->Scene` 返回 `FSceneInterface*`，要 downcast 到 `FScene*` 拿内部状态。`static_cast<FScene*>` 在 5.4 起需要 friend / forward decl 处理，5.7 上 `FScene` 头文件位置还变过一次（`Renderer/Private/SceneRendering.h` → `RenderCore/Public/Scene.h`）。需要做 inline shim namespace。
- **M3 RT 同步等结果 + `bSync = true`** 在编辑器主循环里**有死锁风险**——如果 GT 持有 task graph lock 等 RT 而 RT 在等回调，就 deadlock。规避：`FPlatformProcess::GetSynchEventFromPool(false)` + `Event->Wait(timeout_ms = 100)`，超时直接返回部分数据。
- **M4 trace 文件路径在 5.4 vs 5.5 默认目录不同**：5.4 写 `<Project>/Saved/Profiling/`；5.5+ 改成 `<Project>/Saved/UnrealTrace/`。`stop_trace_capture` 返回的 path 必须 query 实际写出位置而不是预设。
- **M4-5 的 `UnrealInsights.exe -ExportTo=` 不稳**：5.7 上格式不固定，5.6 及之前根本没这个 flag。fallback 必须做——link `TraceServices`（已经 ship 在 Engine plugin 里）走原生 reader API，约 200-300 行代码 + 跨版本 schema 适配。

---

## 下次上手清单（handoff）

按依赖与风险排序。每个子项独立可 commit，build_matrix 5.3-5.7 全过再下一个。

1. **M1-3 `get_world_actor_breakdown`** — 起步首选。无版本风险（`World->GetLevels()` + actor iter 全版本一样），World Partition 分支可以先 TODO（标注），常规项目直接 work。**~1 commit**。
2. **M1-1 `get_texture_memory_breakdown`（disk 模式）** — `GetAssetsByClass(UTexture)` + `FAssetData::TagsAndValues["LODGroup"]` 聚合，5.3 走 `IFileManager` size shim。**~1 commit + build_matrix verify**。
3. **M1-2 `get_mesh_memory_breakdown`** — 复用 M1-1 的聚合 helper。**~1 commit**。
4. **M1-4 `get_uobject_memory_breakdown`** — 在现有 `get_u_object_stats` 基础上扩展，几十行；同 commit 也加 `total_bytes` 到现有结构（前向兼容，旧字段不变）。
5. **M1-5 / M1-6** — 收尾 M1。
6. **M2-4..6（always-on hook）** — 先做"零成本"的 frame time histogram + hitch log，验证 OnEndFrame hook 模式工作 + ring buffer 运转。**~1 commit**。
7. **M2-1..3 + M2-7（opt-in 周期采样）** — 建立在 M2-4 验证通过的 hook 之上，加 FTSTicker 启停 + CSV 导出。**~2 commit（采样核心 + CSV）**。
8. **决策点**：M1+M2 完成后评估 M3 / M4 是否仍需要做。如果用户实际项目的"内存太大 + 帧不稳"两类问题已被覆盖到 80%，M3/M4 可推迟。
9. **M3-2 `get_actor_render_cost`** — 如果继续，从这个最简单的（GT only，无 RT 同步）开始；建立 actor 渲染成本的测量基线。**~1 commit**。
10. **M3-1 `get_visible_primitives_by_material`** — RT 同步 helper 先封装独立辅助函数（`BridgePerfImpl::RunOnRenderThreadSync`），单元化跨版本风险。**~2 commit（helper + 函数）**。
11. **M3-4 / M3-5（Lumen / Nanite）** — 整段 `#if !UE_VERSION_OLDER_THAN(5, 7, 0)` 包住 + `_Stubs.cpp` 补对应版本。**~1 commit / 子项**。
12. **M4-1..4** — 先做 trace 启停 + 状态查询，**不**做 parse-to-summary。给用户"agent 能采，自己开 Insights 看"的体验。**~2 commit**。
13. **M4-5 parse-to-summary** — 单独大 commit。先做最常用的"top hot CPU scopes" 一种 query，其他 query 类型（GPU pass cost / memory growth / RDG events）按需补。**~1 周工程量**。

第一周交付节奏：M1 全过 + M2-4..7。第二周：M2-1..3 + M3 评估。第三周：M3 / M4 视需要选做。

---

## 与其他路线图的关系

- `agent-capability-gaps.md` — A1-#3（Perf 快照）✅ 已交付（即现状基线 5 个函数）；本文件 = A1-#3 的扩展子路线图
- `agent-capability-gaps-extended.md` — B11 性能/资源分解类 4 项（B11-#83 / #84 / #85 / #86）落到 M1 + M3：B11-#84 = M1-1，B11-#85 = M1-3，B11-#83 = M3-1，B11-#86 后续 M5（未列入本文，shader permutation 热度独立做）
- `material-capability-roadmap.md` M1-3 `get_material_stats`（instruction count / sampler count）是材质内部的 perf 视角；本文件 M3-1 是材质对场景渲染的 perf 贡献——两者互补
- `blueprint-capability-roadmap.md` BP-#6 ✅ 运行时 BP 变量快照（2026-05-06 交付）覆盖 _BP 调试_，本文件覆盖 _引擎/渲染/资产 perf_，互不重叠
