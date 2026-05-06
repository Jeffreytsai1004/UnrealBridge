#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "UnrealBridgePerfLibrary.generated.h"

/**
 * Frame timing snapshot in milliseconds. Values come from the raw
 * GGameThreadTime / GRenderThreadTime / RHIGetGPUFrameCycles globals (updated
 * every frame by FViewport::Draw). When `stat unit` is enabled on the active
 * level viewport, the smoothed FStatUnitData values override the raw ones.
 * Fps is GAverageFPS.
 */
USTRUCT(BlueprintType)
struct FBridgeFrameTiming
{
	GENERATED_BODY()

	/** Engine's running-average FPS (GAverageFPS). 0 before the first full frame. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	float Fps = 0.f;

	/** Frame time in ms. FStatUnitData::FrameTime when smoothed; else 1000/Fps. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	float FrameMs = 0.f;

	/** Game-thread time, ms. Raw GGameThreadTime, or smoothed FStatUnitData value. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	float GameThreadMs = 0.f;

	/** Render-thread time, ms. Raw GRenderThreadTime, or smoothed FStatUnitData value. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	float RenderThreadMs = 0.f;

	/** GPU frame time, ms. Summed across MAX_NUM_GPUS via RHIGetGPUFrameCycles. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	float GpuMs = 0.f;

	/** RHI translation time, ms. 0 on the raw path — only set when bSmoothed=true. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	float RhiMs = 0.f;

	/** Delta seconds reported by FApp for the most recent frame. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	float DeltaSeconds = 0.f;

	/** GFrameCounter value at capture time. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 FrameNumber = 0;

	/** True when `stat unit` is active and values came from FStatUnitData (smoothed). */
	/** False means raw per-frame globals (no running average). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bSmoothed = false;
};

/**
 * Per-frame draw call and primitive counters sampled from the RHI globals.
 * These values are the counts for the most recently submitted frame — pulling
 * them once per second is fine; pulling several times per frame returns
 * near-identical numbers.
 */
USTRUCT(BlueprintType)
struct FBridgeRenderCounters
{
	GENERATED_BODY()

	/** GNumDrawCallsRHI summed across MAX_NUM_GPUS. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 DrawCalls = 0;

	/** GNumPrimitivesDrawnRHI summed across MAX_NUM_GPUS. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 PrimitivesDrawn = 0;

	/** GNumExplicitGPUsForRendering — usually 1 on desktop builds. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 NumGpus = 1;
};

/** Process + platform memory snapshot, in megabytes (MiB). */
USTRUCT(BlueprintType)
struct FBridgeMemoryStats
{
	GENERATED_BODY()

	/** Process working set (FPlatformMemoryStats::UsedPhysical), MiB. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 UsedPhysicalMb = 0;

	/** Process virtual commit (UsedVirtual), MiB. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 UsedVirtualMb = 0;

	/** Peak working set observed this session, MiB. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 PeakUsedPhysicalMb = 0;

	/** Peak virtual commit observed this session, MiB. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 PeakUsedVirtualMb = 0;

	/** System-wide available physical memory at capture time, MiB. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 AvailablePhysicalMb = 0;

	/** System-wide available virtual memory at capture time, MiB. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 AvailableVirtualMb = 0;

	/** Total physical RAM on the machine, MiB. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 TotalPhysicalMb = 0;
};

/** Histogram entry: "this class has N live UObjects". */
USTRUCT(BlueprintType)
struct FBridgeUObjectClassCount
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString ClassName;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 Count = 0;
};

/**
 * UObject population snapshot. Iterates TObjectIterator<UObject> and aggregates
 * by UClass, returning the top-N classes by count. O(N) in the number of live
 * UObjects (typically 100k-2M in an editor session) — don't call on a hot path.
 */
USTRUCT(BlueprintType)
struct FBridgeUObjectStats
{
	GENERATED_BODY()

	/** Total number of live UObjects walked. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 TotalObjects = 0;

	/** Number of distinct UClass types seen. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 UniqueClasses = 0;

	/** Top classes by live-object count, descending. Capped at the caller's TopN. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	TArray<FBridgeUObjectClassCount> TopClasses;
};

/**
 * Aggregated row for memory / asset / actor breakdown queries on
 * `UUnrealBridgePerfLibrary`. `Key` is the group identifier (folder path,
 * class name, level name, compression format, etc. — depends on which
 * UFUNCTION returned the row). `LevelName` is optional and only populated by
 * actor-breakdown variants that need a per-level disambiguator on top of the
 * primary key (group_by="level_class" → Key=class, LevelName=level).
 */
USTRUCT(BlueprintType)
struct FBridgePerfBreakdownRow
{
	GENERATED_BODY()

	/** Primary group key. Interpretation is per-UFUNCTION (see callers). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString Key;

	/** Number of assets / actors / objects falling into this group. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 Count = 0;

	/** Total bytes attributed to this group (disk size or runtime size, see caller). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 TotalBytes = 0;

	/** Up to 3 representative paths for "show me what's in here" UI affordance. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	TArray<FString> SamplePaths;

	/**
	 * Optional secondary key. Empty for asset-only breakdowns; populated by
	 * level-aware actor breakdowns when the primary key isn't already a level
	 * (e.g. group_by="level_class" returns Key=class, LevelName=level).
	 */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString LevelName;
};

/**
 * One bucket from the always-on frame-time histogram. The histogram is
 * accumulated by `BridgePerfFrameHook` (registered at module startup, hooked
 * onto FCoreDelegates::OnEndFrame). `GetFrameTimeHistogram` re-aggregates the
 * fine internal buckets (0.5 ms each) into the caller's coarser buckets.
 */
USTRUCT(BlueprintType)
struct FBridgeHistogramBucket
{
	GENERATED_BODY()

	/** Bucket lower edge in milliseconds, inclusive. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	float LowerMs = 0.f;

	/** Bucket upper edge in milliseconds, exclusive. FLT_MAX for the overflow bucket. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	float UpperMs = 0.f;

	/** Count of frames whose total time fell into [LowerMs, UpperMs). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 Count = 0;

	/** Pre-computed fraction of total observed frames in this bucket, [0, 1]. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	float Percent = 0.f;
};

/**
 * One captured hitch (frame whose total time exceeded the caller's threshold).
 * Logged by `BridgePerfFrameHook` into a small ring buffer; `GetHitchLog`
 * filters by threshold and returns the most recent entries.
 */
USTRUCT(BlueprintType)
struct FBridgeHitchEntry
{
	GENERATED_BODY()

	/** GFrameCounter value at hitch capture time. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 FrameNumber = 0;

	/** FApp::GetCurrentTime() at hitch capture time, in seconds since launch. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	double TimestampSeconds = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	float GameThreadMs = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	float RenderThreadMs = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	float GpuMs = 0.f;

	/** Total wall-clock frame time, ms. (Sourced from FApp::GetCurrentTime() delta.) */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	float TotalMs = 0.f;
};

/**
 * Status of the opt-in periodic perf sampler. Returned by
 * GetPerfSamplingState. When `bActive` is false the other fields describe the
 * most recently completed run (cleared on next StartPerfSampling).
 */
USTRUCT(BlueprintType)
struct FBridgePerfSamplingState
{
	GENERATED_BODY()

	/** True between StartPerfSampling and StopPerfSampling. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bActive = false;

	/** ISO-8601 UTC timestamp of the last StartPerfSampling call (empty if never started). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString StartedAtUtc;

	/** Currently buffered sample count (resets on each StartPerfSampling). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 SamplesCollected = 0;

	/** Sampler period in milliseconds (as configured at the last StartPerfSampling). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 PeriodMs = 0;

	/** Configured ring buffer cap for the active run. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 MaxSamples = 0;

	/** True when the ticker is also recording UObject stats per sample. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bIncludeUObjectStats = false;
};

/** Bundled perf snapshot returned by GetPerfSnapshot. */
USTRUCT(BlueprintType)
struct FBridgePerfSnapshot
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FBridgeFrameTiming Timing;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FBridgeRenderCounters Render;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FBridgeMemoryStats Memory;

	/** Populated only when bIncludeUObjectStats was true. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FBridgeUObjectStats UObjects;

	/** ISO-8601 UTC timestamp for delta-comparison across snapshots. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString CaptureTimeUtc;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString EngineVersion;

	/** True when PIE was active at capture time. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bWasInPie = false;
};

/**
 * One material → set of primitives using it (M3-1). Returned by
 * `GetVisiblePrimitivesByMaterial`. The implementation is GT-only — it reads
 * `UPrimitiveComponent::GetUsedMaterials` for every component in the editor
 * world. This means the row reflects "what materials are referenced in this
 * world" rather than "what materials are submitted in the current frame's
 * culled view" — the latter would require RT-side `FScene` traversal which is
 * intentionally avoided here to dodge the cross-version shim cost.
 */
USTRUCT(BlueprintType)
struct FBridgeMaterialRenderRow
{
	GENERATED_BODY()

	/** Asset path of the material / material instance. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString MaterialPath;

	/** Number of distinct UPrimitiveComponents that reference this material. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 PrimitiveCount = 0;

	/** Total estimated triangle count contributed across those primitives.
	 *  Static meshes use LOD0 triangle count from the asset RenderData; skeletal
	 *  meshes use LOD0 triangle count from FSkeletalMeshRenderData. 0 when the
	 *  asset is unavailable (e.g. unloaded). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 TotalTriangles = 0;

	/** First 3 actor paths owning a primitive that uses this material. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	TArray<FString> SampleActorPaths;
};

/**
 * Per-actor render cost summary (M3-2). Returned by `GetActorRenderCost`
 * and `GetShadowCasterBreakdown`. GameThread-only — reads cached
 * UPrimitiveComponent properties; does not reflect culling state.
 */
USTRUCT(BlueprintType)
struct FBridgeActorRenderCost
{
	GENERATED_BODY()

	/** Actor path (e.g. "/Game/Maps/Forest.Forest:PersistentLevel.SM_Tree_42"). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString ActorPath;

	/** Number of UPrimitiveComponents on the actor (visible or hidden). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 PrimitiveComponentCount = 0;

	/** Sum of `GetNumMaterials()` across primitive components. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 MaterialSlotCount = 0;

	/** Sum of LOD0 triangle counts across static + skeletal mesh components.
	 *  Returns 0 for primitive types we don't recognize (particle systems, etc.). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 EstimatedTriangleCount = 0;

	/** True when ANY primitive component on the actor has bCastDynamicShadow=true. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bCastsDynamicShadow = false;

	/** Distinct material asset paths referenced across all primitives. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	TArray<FString> Materials;
};

/**
 * Lumen runtime diagnostics (M3-4). Populated only on UE 5.7+. On older
 * versions every field stays at its default-zero value and the UFUNCTION's
 * stub logs a one-time warning. Public Lumen state in 5.7 is mostly
 * visualization metadata — the surface-cache / probe counters live in the
 * Renderer module's private `FLumenSceneData` and aren't reachable from a
 * non-Renderer module. We expose what the engine API surfaces publicly:
 * available visualization modes (proxy for "Lumen feature is wired up") and
 * the active mode name (resolved via the `r.Lumen.Visualize.ViewMode` CVar).
 */
USTRUCT(BlueprintType)
struct FBridgeLumenDiagnostics
{
	GENERATED_BODY()

	/** True when GetLumenVisualizationData() returned an initialized instance.
	 *  False on unsupported engine versions or when the visualization data
	 *  module hasn't been loaded yet. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bAvailable = false;

	/** Engine identifier returned by FEngineVersion::Current() — handy for
	 *  attributing data when comparing snapshots across versions. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString EngineVersion;

	/** All visualization mode names registered with FLumenVisualizationData
	 *  (e.g. "Overview", "FinalLightingScene"). Empty when bAvailable=false. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	TArray<FString> VisualizationModes;

	/** Currently active visualization mode according to r.Lumen.Visualize.ViewMode.
	 *  Empty string when no mode is active or the CVar isn't registered. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString ActiveVisualizationMode;

	/** True when r.DynamicGlobalIlluminationMethod is set to a value that
	 *  enables Lumen GI. Determined at call time from the CVar. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bLumenGiEnabled = false;

	/** True when r.ReflectionMethod is set to a value that enables Lumen
	 *  reflections. Determined at call time from the CVar. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bLumenReflectionsEnabled = false;
};

/**
 * Nanite runtime stats (M3-5). Populated only on UE 5.7+. Reads the public
 * surface of `Nanite::GStreamingManager` (an ENGINE_API global) — capacity
 * configuration the streaming manager exposes via its public getters. Nanite's
 * per-frame cluster / visible-set counters live in private members and aren't
 * accessible without Renderer module access; we record them as 0 with a note
 * in the field comments. Non-zero default values indicate Nanite is at least
 * configured even without per-frame telemetry.
 */
USTRUCT(BlueprintType)
struct FBridgeNaniteStats
{
	GENERATED_BODY()

	/** True when GStreamingManager has at least one resource entry (HasResourceEntries()).
	 *  False on unsupported engine versions or when no Nanite mesh has loaded. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bAvailable = false;

	/** Nanite::GStreamingManager.GetMaxStreamingPages() — total streaming-page
	 *  budget the manager was initialized with. Independent of how many are
	 *  resident this frame. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 MaxStreamingPages = 0;

	/** Nanite::GStreamingManager.GetMaxHierarchyLevels(). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 MaxHierarchyLevels = 0;

	/** True when the streaming manager reported IsSafeForRendering() at call
	 *  time. Useful as a "Nanite system actually initialized" check. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bIsSafeForRendering = false;

	/** Estimated count of UStaticMesh assets in the editor world whose
	 *  IsNaniteEnabled() / HasValidNaniteData() returns true. Aggregated on
	 *  the GameThread by walking UStaticMeshComponents — does NOT require
	 *  RT sync. 0 on unsupported engine versions. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 NaniteStaticMeshComponents = 0;

	/** Engine version string for snapshot attribution. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString EngineVersion;
};

/**
 * Structured performance snapshots for UnrealBridge. Replaces parsing
 * `stat unit` text output. All values are read from engine globals + platform
 * APIs on the GameThread.
 */
UCLASS()
class UNREALBRIDGE_API UUnrealBridgePerfLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:

	/**
	 * Return smoothed frame timing (FPS, game/render/GPU ms). Prefers
	 * FStatUnitData from the active editor viewport (smoothed running
	 * average). Falls back to raw RenderCore / RHI globals when no viewport
	 * client is reachable (e.g. headless commandlet). Timings are stale when
	 * the editor hasn't rendered recently — check `frame_number` to detect
	 * this.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static FBridgeFrameTiming GetFrameTiming();

	/**
	 * Return draw call and primitive counts for the most recent rendered
	 * frame, summed across MAX_NUM_GPUS. 0 on headless builds or before the
	 * first frame.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static FBridgeRenderCounters GetRenderCounters();

	/** Return process + system memory stats in MiB. */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static FBridgeMemoryStats GetMemoryStats();

	/**
	 * Return the top-N UClass types by live UObject count. Iterates every
	 * live UObject (~100k-2M typical) — expect 10-200 ms. TopN clamped to
	 * [1, 200].
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static FBridgeUObjectStats GetUObjectStats(int32 TopN = 20);

	/**
	 * Aggregate snapshot. bIncludeUObjectStats defaults off because the
	 * UObject iteration is the slow part; pass true when you want the
	 * histogram, false for a cheap per-second sampler.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static FBridgePerfSnapshot GetPerfSnapshot(bool bIncludeUObjectStats = false, int32 UObjectTopN = 20);

	/**
	 * Aggregate the editor world's actors by class / level / level_class. Only
	 * walks `World->GetLevels()` — the persistent level plus loaded streaming
	 * sublevels. World Partition projects will report only currently-loaded
	 * actors (the partition unloaded-desc enumeration is a TODO; it requires a
	 * separate API path that has churned across 5.3-5.7). `LevelFilter` is a
	 * substring matched against each level's package short name; empty means
	 * "all levels". `GroupBy` ∈ {"class", "level", "level_class"}. `MaxGroups`
	 * clamped to [1, 1000]; rows sorted by Count descending, ties broken by Key.
	 *
	 * Per-row schema:
	 *   - GroupBy="class":         Key=actor class FName,    LevelName=""
	 *   - GroupBy="level":         Key=level package short,   LevelName=""
	 *   - GroupBy="level_class":   Key=actor class FName,     LevelName=level package short
	 * TotalBytes is always 0 here (actors are runtime-only, no on-disk size).
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static TArray<FBridgePerfBreakdownRow> GetWorldActorBreakdown(
		const FString& LevelFilter,
		const FString& GroupBy = TEXT("class"),
		int32 MaxGroups = 200);

	/**
	 * Aggregate UTexture assets by group with disk or runtime byte totals.
	 *
	 * `GroupBy` ∈ {"folder", "lod_group", "compression_format", "sampler_type"}:
	 *   - "folder" → Key = leading content path (e.g. "/Game/Characters")
	 *   - "lod_group" → Key = TextureGroup enum (e.g. "TEXTUREGROUP_World")
	 *   - "compression_format" → Key = TextureCompressionSettings enum (e.g. "TC_Default")
	 *   - "sampler_type" → Key = derived sampler type (e.g. "Color", "Normal", "Masks")
	 *
	 * `Mode` ∈ {"disk", "runtime"} (default "disk"):
	 *   - "disk" walks AssetRegistry without loading textures; reads the package
	 *     file size on disk via FPackageName::DoesPackageExist + IFileManager.
	 *     Never-saved assets have empty TagsAndValues and are skipped.
	 *   - "runtime" iterates loaded UTexture objects via TObjectIterator and
	 *     calls GetResourceSizeBytes(EstimatedTotal). Reflects only currently
	 *     loaded textures — large parts of the project will be invisible.
	 *
	 * `MaxGroups` clamped to [1, 1000]; rows sorted by TotalBytes descending,
	 * ties broken by Count then Key.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static TArray<FBridgePerfBreakdownRow> GetTextureMemoryBreakdown(
		const FString& GroupBy,
		const FString& Mode = TEXT("disk"),
		int32 MaxGroups = 50);

	/**
	 * Aggregate static + skeletal mesh assets by group with disk or runtime
	 * byte totals.
	 *
	 * `GroupBy` ∈ {"folder", "lod_count", "vertex_count_bucket"}:
	 *   - "folder" → Key = leading content path (one level deep)
	 *   - "lod_count" → Key = number of LODs (e.g. "3 LODs")
	 *   - "vertex_count_bucket" → Key = log-scale bucket
	 *     ("<1k", "1k-10k", "10k-100k", "100k-1M", ">=1M")
	 *
	 * `MeshType` ∈ {"static", "skeletal", "all"} (default "all"):
	 *   - "static" walks UStaticMesh only
	 *   - "skeletal" walks USkeletalMesh only
	 *   - "all" walks both, summed into a single bucket per Key
	 *
	 * `Mode` ∈ {"disk", "runtime"} (default "disk"):
	 *   - "disk" reads AssetRegistry tags (LODs, Vertices) without LoadObject;
	 *     bytes come from the package's on-disk file size.
	 *   - "runtime" iterates loaded UStaticMesh / USkeletalMesh objects and
	 *     calls GetResourceSizeBytes(EstimatedTotal); LOD/vertex counts come
	 *     from RenderData. Misses unloaded meshes by design.
	 *
	 * `MaxGroups` clamped to [1, 1000]; rows sorted by TotalBytes descending,
	 * ties broken by Count then Key. Never-saved assets (empty TagsAndValues
	 * + missing on-disk file) are skipped in disk mode.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static TArray<FBridgePerfBreakdownRow> GetMeshMemoryBreakdown(
		const FString& GroupBy,
		const FString& MeshType = TEXT("all"),
		const FString& Mode = TEXT("disk"),
		int32 MaxGroups = 50);

	/**
	 * Top-N UClass histogram with byte totals — runtime-only. Per-class:
	 * Key=class FName, Count=live instance count, TotalBytes=sum of
	 * `GetResourceSizeBytes(EResourceSizeMode::Exclusive)` across instances.
	 * SamplePaths holds up to 3 representative live object paths.
	 *
	 * Iterates every live UObject (`TObjectIterator<UObject>`); typical
	 * editor session is 100k-2M objects so expect 50-300 ms. `TopN` clamped
	 * to [1, 200]; rows sorted by TotalBytes descending. Disk mode is not
	 * meaningful for this query (UObjects are runtime-side) — there is no
	 * `mode` parameter.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static TArray<FBridgePerfBreakdownRow> GetUObjectMemoryBreakdown(int32 TopN = 20);

	/**
	 * Aggregate USoundWave assets by group with disk or runtime byte totals.
	 *
	 * `GroupBy` ∈ {"compression_format", "folder", "sample_rate_bucket",
	 * "channel_count"}:
	 *   - "compression_format" → Key = ESoundAssetCompressionType enum
	 *     (e.g. "PlatformSpecific", "BinkAudio", "PCM")
	 *   - "folder" → Key = leading content path (one level deep)
	 *   - "sample_rate_bucket" → Key = "<8k", "8k-16k", "16k-22k", "22k-44k",
	 *     "44k-48k", "48k-96k", ">=96k", "(unknown)"
	 *   - "channel_count" → Key = "Mono" / "Stereo" / "5.1" / "7.1" / "Other"
	 *
	 * `Mode` ∈ {"disk", "runtime"} (default "disk"):
	 *   - "disk" walks AssetRegistry without LoadObject; bytes from package
	 *     file size; group keys come from TagsAndValues.
	 *   - "runtime" iterates loaded USoundWave objects.
	 *
	 * `MaxGroups` clamped to [1, 1000]; rows sorted by TotalBytes descending.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static TArray<FBridgePerfBreakdownRow> GetAudioMemoryBreakdown(
		const FString& GroupBy = TEXT("compression_format"),
		const FString& Mode = TEXT("disk"),
		int32 MaxGroups = 50);

	/**
	 * Top-N largest assets on disk, optionally filtered by UClass. Each row
	 * is one asset: Key=full asset path (e.g. "/Game/Foo/Bar.Bar"), Count=1,
	 * TotalBytes=on-disk package size, LevelName=class path of the asset
	 * (e.g. "/Script/Engine.Texture2D"), SamplePaths empty.
	 *
	 * `ClassFilter`:
	 *   - empty → all assets
	 *   - "/Script/Engine.Texture2D" → exact class match (top-level path)
	 *   - "Texture2D" → matched against the asset class short name
	 * Subclasses are included by default — passing UTexture sweeps every
	 * Texture2D / TextureCube / etc.
	 *
	 * `TopN` clamped to [1, 1000]. Walks AssetRegistry only — no LoadObject;
	 * never-saved assets are skipped (their disk contribution is 0).
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static TArray<FBridgePerfBreakdownRow> GetAssetSizeTopN(
		const FString& ClassFilter,
		int32 TopN = 50);

	// ─── M2: time series & sampling ────────────────────────────

	/**
	 * Return a histogram of per-frame total time observed since module load
	 * (or since the last `ResetFrameTimeHistogram` call). Frames are recorded
	 * in fine 0.5 ms internal buckets by an OnEndFrame hook that started at
	 * StartupModule; this UFUNCTION re-aggregates them into caller-supplied
	 * `BucketMs`-wide buckets up to `MaxBucketMs`. Frames above `MaxBucketMs`
	 * land in a single overflow bucket whose UpperMs is FLT_MAX.
	 *
	 * `BucketMs` clamped to [0.5, 50.0]; values below 0.5 round up to 0.5
	 * (the internal resolution). `MaxBucketMs` clamped to [BucketMs, 200.0].
	 *
	 * Cost is O(internal_buckets) per call (~400 reads) — safe to call
	 * at any rate. Bucket Percent fields sum to 1.0 across all buckets.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static TArray<FBridgeHistogramBucket> GetFrameTimeHistogram(
		float BucketMs = 5.f,
		float MaxBucketMs = 100.f);

	/**
	 * Return up to `MaxEntries` most-recent hitch entries (frames whose total
	 * time exceeded the OnEndFrame hitch detector threshold) where each
	 * entry's TotalMs is also at or above `ThresholdMs`. Entries are returned
	 * newest-last (chronological order).
	 *
	 * The hitch detector logs every frame above an internal min threshold
	 * (33 ms — i.e. anything below 30 fps); `ThresholdMs` re-filters that
	 * captured set per call, so values below 33 will simply return whatever
	 * was already captured. `MaxEntries` clamped to [1, 200].
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static TArray<FBridgeHitchEntry> GetHitchLog(
		float ThresholdMs = 50.f,
		int32 MaxEntries = 50);

	/**
	 * Zero out all internal frame-time histogram buckets so the next
	 * `GetFrameTimeHistogram` returns only frames captured after this call.
	 * Useful to baseline before/after a measured change.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static void ResetFrameTimeHistogram();

	/** Drop every captured hitch entry. */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static void ClearHitchLog();

	/**
	 * Begin (or restart) periodic perf sampling. A FTSTicker on the GameThread
	 * fires every `PeriodMs` and captures a `FBridgePerfSnapshot` into a
	 * ring buffer of size `MaxSamples`. When `bIncludeUObjectStats` is true,
	 * the UObject-iteration step (50-300 ms typical) runs every tick — only
	 * enable it when paired with periods >= 5000 ms.
	 *
	 * Idempotent: calling while already active discards the prior run's
	 * buffer and restarts with the new parameters.
	 *
	 * `PeriodMs` clamped to [10, 60000]; `MaxSamples` clamped to [1, 10000].
	 * Returns true on success, false only if the FTSTicker registration fails
	 * (extremely unusual — would mean the engine's core ticker is unavailable).
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static bool StartPerfSampling(
		int32 PeriodMs = 100,
		int32 MaxSamples = 600,
		bool bIncludeUObjectStats = false);

	/**
	 * Stop sampling and return the buffered snapshots (empty when nothing was
	 * captured or sampling was never started). The internal buffer is
	 * cleared after this call. Calling while inactive is safe and returns
	 * whatever was previously buffered (typically empty after the prior
	 * StopPerfSampling already drained it).
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static TArray<FBridgePerfSnapshot> StopPerfSampling();

	/** Return the active / inactive state of the periodic sampler. */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static FBridgePerfSamplingState GetPerfSamplingState();

	/**
	 * Serialize the current periodic-sampling buffer to CSV. The header is:
	 *   timestamp_utc, frame_number, fps, frame_ms, gt_ms, rt_ms, gpu_ms,
	 *   rhi_ms, delta_seconds, used_physical_mb, used_virtual_mb,
	 *   available_physical_mb, draw_calls, primitives_drawn, was_in_pie,
	 *   engine_version
	 *
	 * UObject top-class stats are intentionally omitted (would balloon the row
	 * width). One row per sample. Works whether sampling is active or stopped:
	 * the buffer is read in-place without clearing.
	 *
	 * `OutputPath` resolution:
	 *   - empty → <Project>/Saved/UnrealBridge/perf_samples_<unix>.csv
	 *   - directory → that directory + the auto-named file
	 *   - any other path → used as-is (parent dir created on demand)
	 *
	 * Returns true on successful write, false on I/O error or empty buffer.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static bool ExportPerfSamplesToCsv(const FString& OutputPath);

	// ─── M3: render breakdown ──────────────────────────────────

	/**
	 * Aggregate primitive components by material, returning the top-N rows
	 * by primitive count (ties broken by triangle total). Walks every
	 * UPrimitiveComponent in the editor world on the GameThread; per
	 * component reads `GetUsedMaterials()` (public API, stable 5.3-5.7) and
	 * accumulates the asset path → primitive count + triangle total +
	 * sample-actor list mapping.
	 *
	 * Limitations to advertise to callers:
	 *   - GameThread-only — no RT-side scene traversal. The result reflects
	 *     "what materials are referenced" not "what materials drew this
	 *     frame after culling". For most diagnostic uses this is what you
	 *     want; for true visible-set accounting hook UE Insights instead.
	 *   - `ViewportIndex` is accepted but currently ignored — the editor
	 *     world has one canonical material set regardless of which viewport
	 *     is looking at it. Reserved for a future RT-side enrichment.
	 *   - Triangle counts use LOD0; we don't try to guess current LOD per
	 *     component (would need RT-side screen-size resolution).
	 *
	 * `TopN` clamped to [1, 1000].
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static TArray<FBridgeMaterialRenderRow> GetVisiblePrimitivesByMaterial(
		int32 ViewportIndex = 0,
		int32 TopN = 50);

	/**
	 * Per-actor render cost summary. Resolves `ActorPath` via
	 * `FindObject<AActor>` against the editor world (must be a fully
	 * qualified path like "/Game/Maps/Forest.Forest:PersistentLevel.SM_Tree_42").
	 *
	 * Returns an empty struct (ActorPath="" indicating not-found) when the
	 * actor cannot be resolved. Otherwise iterates UPrimitiveComponents on
	 * the actor and aggregates:
	 *   - PrimitiveComponentCount: total primitive components
	 *   - MaterialSlotCount: sum of GetNumMaterials() per primitive
	 *   - EstimatedTriangleCount: sum of LOD0 triangles for static / skel
	 *     mesh components (other primitive types contribute 0)
	 *   - bCastsDynamicShadow: OR of bCastDynamicShadow across primitives
	 *   - Materials: distinct material asset paths referenced
	 *
	 * GameThread-only; no RT sync. Cost is O(num_components_on_actor).
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static FBridgeActorRenderCost GetActorRenderCost(const FString& ActorPath);

	/**
	 * Aggregate static + skeletal mesh components in the editor world by
	 * `<asset_path>:LOD<n>`, returning a breakdown row per (mesh, lod) pair.
	 *
	 * The "current LOD" used here is the component's
	 * `GetForcedLODRequested()` / equivalent best-effort accessor; when the
	 * component has no forced LOD we fall back to LOD0. We intentionally do
	 * NOT compute the screen-size-driven dynamic LOD because that requires
	 * the rendered view (RT-side) and would introduce a sync. Callers who
	 * need true per-frame LOD should use `stat lodgroup` or trace tools.
	 *
	 * Filters:
	 *   - `ClassFilter`: substring matched against the component's class FName
	 *     (case-insensitive). Typical values: "StaticMeshComponent",
	 *     "SkeletalMeshComponent". Empty = both.
	 *   - `ActorFilter`: substring matched against the owning actor's FName.
	 *     Empty = all actors.
	 *
	 * Schema:
	 *   - Key = "<mesh_asset_path>:LOD<n>"
	 *   - Count = number of components reporting that (mesh, lod) pair
	 *   - TotalBytes = 0 (LOD distribution is component-count, not size)
	 *   - SamplePaths = up to 3 owning actor paths
	 *   - LevelName = empty
	 *
	 * Rows sorted by Count descending, ties by Key. Returns at most 1000 rows.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static TArray<FBridgePerfBreakdownRow> GetLodDistribution(
		const FString& ClassFilter,
		const FString& ActorFilter);

	/**
	 * Top-N actors ranked by dynamic-shadow caster cost estimate. Walks
	 * the editor world on the GameThread, picks every actor with at least
	 * one primitive component whose `bCastDynamicShadow=true`, computes the
	 * actor's full render cost (same logic as `GetActorRenderCost`), and
	 * returns them sorted by EstimatedTriangleCount descending.
	 *
	 * "Cost estimate" here = LOD0 triangle total of the shadow-casting
	 * primitive components on the actor. Cascade / VSM page costs are
	 * not modeled — those require RT state which we deliberately avoid.
	 *
	 * `TopN` clamped to [1, 1000].
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static TArray<FBridgeActorRenderCost> GetShadowCasterBreakdown(int32 TopN = 30);

	/**
	 * Lumen runtime diagnostics. Returns default-zero on UE 5.6 and below;
	 * on 5.7+ returns visualization mode list + Lumen-enabled CVar state.
	 * See FBridgeLumenDiagnostics for what's surfaced and why surface-cache
	 * / probe-count internals stay unimplemented.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static FBridgeLumenDiagnostics GetLumenDiagnostics();

	/**
	 * Nanite runtime stats. Returns default-zero on UE 5.6 and below;
	 * on 5.7+ pulls capacity getters from `Nanite::GStreamingManager` plus
	 * a GT-side scan for static-mesh components whose StaticMesh has Nanite
	 * data enabled. Per-frame visible-cluster counters are not exposed
	 * publicly and remain unreported.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static FBridgeNaniteStats GetNaniteStats();
};
