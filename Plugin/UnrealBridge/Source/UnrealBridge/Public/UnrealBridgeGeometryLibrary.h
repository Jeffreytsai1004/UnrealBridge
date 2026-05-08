#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "UnrealBridgeGeometryLibrary.generated.h"

/**
 * Geometry Script wrapper — Lane 2 of the procedural-content roadmap.
 *
 * Design contract (see docs/plans/procedural-content-roadmap.md §4):
 *  - UDynamicMesh handles are int32 keys into a process-global map. The
 *    map holds TStrongObjectPtr<UDynamicMesh> so handles survive GC for
 *    as long as the bridge keeps them registered. Caller must Release()
 *    when done.
 *  - All ops run on GameThread (bridge worker dispatches there); large
 *    sweeps (M5-6 voxel merge, M5-2 boolean on dense meshes) can block
 *    the GT for seconds. Split them across exec calls per
 *    feedback_bridge_exec_holds_gamethread.
 *  - The whole library is gated to UE 5.7+. On 5.4-5.6 the UFUNCTIONs
 *    still exist (UHT requires unconditional declarations) but their
 *    bodies live in UnrealBridgeGeometryLibrary_Stubs.cpp and log a
 *    warning + return T{}. See feedback_uht_no_if_around_reflection.
 *  - Asset save UFUNCTIONs (M4-4 / M4-5) must run in editor world only;
 *    invoking them from PIE / cmdlet silently no-ops.
 *
 * UFUNCTIONs land in three batches:
 *   Phase 2 — M4: handle pool + asset I/O (8 ops)
 *   Phase 3 — M5/M6 P2 priority: boolean / smooth / decimate / recompute_normals (4 ops)
 *   Future  — M5 rest + M6-1/2 bake (12 ops)
 */
UCLASS()
class UNREALBRIDGE_API UUnrealBridgeGeometryLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	// UFUNCTIONs land in subsequent phases — see class docblock above.
};
