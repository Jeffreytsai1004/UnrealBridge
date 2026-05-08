#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "UnrealBridgePCGLibrary.generated.h"

USTRUCT(BlueprintType)
struct FBridgePCGComponentEntry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|PCG") FString ActorLabel;
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|PCG") FString ComponentName;
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|PCG") FString GraphPath;
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|PCG") bool bGenerated = false;
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|PCG") bool bGenerating = false;
};

USTRUCT(BlueprintType)
struct FBridgePCGComponentState
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|PCG") FString GraphPath;
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|PCG") bool bGenerated = false;
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|PCG") bool bDirty = false;
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|PCG") bool bGenerating = false;
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|PCG") FBox GeneratedBounds = FBox(ForceInit);
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|PCG") FString LastGenerationIso;
};

USTRUCT(BlueprintType)
struct FBridgePCGOverrideEntry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|PCG") FString Name;
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|PCG") FString TypeStr;
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|PCG") FString ValueStr;
};

USTRUCT(BlueprintType)
struct FBridgePCGWaitResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|PCG") bool bSuccess = false;
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|PCG") float ElapsedMs = 0.0f;
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|PCG") FString Note;
};

/**
 * PCG read + trigger — Lane 3 of the procedural-content roadmap.
 *
 * Hard contract (roadmap §5, §8):
 *  - Read-only graph access + override edit + Generate/Cleanup trigger ONLY.
 *    NO graph editing (graph topology is PCG's territory; agents write
 *    code, not visual graphs).
 *  - Whole library is gated to UE 5.7+. On 5.4-5.6 the UFUNCTIONs exist
 *    (UHT requires unconditional decls) but bodies live in the
 *    UnrealBridgePCGLibrary_Stubs.cpp and log a warning + return T{}.
 *  - Editor-world only — runtime/PIE PCG generation is PCG's own concern;
 *    bridge does not duplicate it.
 *  - WaitForPCGGenerate polls on the GameThread (FPlatformProcess::Sleep
 *    in 50ms steps) — caller must know this blocks. Same pattern as
 *    hot_reload.py.
 */
UCLASS()
class UNREALBRIDGE_API UUnrealBridgePCGLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:

	/** L3-1 — list UPCGGraph assets in the project, optionally filtered by short-name substring. */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|PCG")
	static TArray<FString> ListPCGGraphAssets(const FString& Filter, int32 Max = 200);

	/** L3-2 — list UPCGComponent on actors in the editor world. */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|PCG")
	static TArray<FBridgePCGComponentEntry> ListPCGComponentsInLevel(const FString& LevelFilter, int32 Max = 200);

	/** L3-3 — full state for one component on a level actor. */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|PCG")
	static FBridgePCGComponentState GetPCGComponentState(const FString& ActorLabel, const FString& ComponentName);

	/** L3-4 — list user-parameter values exposed by the component's graph instance. */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|PCG")
	static TArray<FBridgePCGOverrideEntry> GetPCGComponentOverrides(const FString& ActorLabel, const FString& ComponentName);

	/**
	 * L3-5 — set a single user-parameter override via Property->ImportText.
	 * `ExportedValue` must be in UE's standard exported form for the property's
	 * type (e.g. `42` for int, `(X=1, Y=2, Z=3)` for FVector, `"hello"` for FString).
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|PCG")
	static bool SetPCGComponentOverride(const FString& ActorLabel, const FString& ComponentName, const FString& Name, const FString& ExportedValue);

	/**
	 * L3-6 — request asynchronous generation. Returns immediately after the
	 * request is queued; use `WaitForPCGGenerate` to block until done.
	 *
	 * @param bForce true → run even if not dirty.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|PCG")
	static bool TriggerPCGGenerate(const FString& ActorLabel, const FString& ComponentName, bool bForce = false);

	/**
	 * L3-7 — block (with 50ms polling) until the component finishes generating
	 * or `TimeoutSec` elapses. Returns elapsed_ms + success flag.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|PCG")
	static FBridgePCGWaitResult WaitForPCGGenerate(const FString& ActorLabel, const FString& ComponentName, float TimeoutSec = 60.0f);

	/** L3-8 — clean up generated content (purge actors / components tagged by this PCG generation). */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|PCG")
	static bool CleanupPCGComponent(const FString& ActorLabel, const FString& ComponentName, bool bRemoveComponents = false);
};
