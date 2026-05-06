# UnrealBridge — engine version compatibility

The plugin claims **Unreal Engine 5.3+**. The build matrix in
`tools/build_matrix.py` has verified clean BuildPlugin against 5.3 / 5.4 /
5.5 / 5.6 / 5.7 with the gating below; 5.2 is configured but disabled
(would need additional pre-5.3 shims). Some features are gated to **5.7+**
because they depend on engine APIs that don't exist or behave differently
on the older 5.x's.

This document lists what's gated. The build matrix in `tools/build_matrix.py`
verifies the gates by compiling the plugin against each engine version.

## Whole-library gates (disappear entirely on 5.4)

Each library below is wrapped in `#if !UE_VERSION_OLDER_THAN(5, 7, 0)`. On 5.4
its `.h` and `.cpp` compile to empty translation units, no `UCLASS` is
registered, and calling any of its UFUNCTIONs from Python on a 5.4 build will
fail with "no such function on UnrealBridgeXxxLibrary".

| Library | Reason |
|---|---|
| `UnrealBridgeChooserLibrary` | `OutputObjectColumn.h` doesn't exist in 5.4 (added with the Chooser plugin's output-column rewrite); other Chooser internals shifted heavily 5.4 → 5.7 |
| `UnrealBridgePoseSearchLibrary` | Core API rewritten: `UPoseSearchSchema::GetRoledSkeletons`, `UPoseSearchDatabase::GetNumAnimationAssets` / `GetDatabaseAnimationAsset`, and `FPoseSearchDatabaseAnimationAsset` are all 5.5+ additions |
| `UnrealBridgeMaterialLibrary` | `EMaterialDomain::MD_*` enum values differ in scope; `MATUSAGE_Voxels` / `MATUSAGE_StaticMesh` don't exist in 5.4 |
| `UnrealBridgeNavigationLibrary` | `ARecastNavMesh::GetDebugGeometryForTile` 2nd arg type changed (`int32` → `FNavTileRef`) and the "default tile = aggregate all" sentinel doesn't exist on 5.4 |

## Single-UFUNCTION gates (library still works, one function unavailable on 5.4)

| UFUNCTION | Reason |
|---|---|
| `UnrealBridgeDataTableLibrary::CopyDataTableRows` | `UDataTable::AddRow(FName, const uint8*, UScriptStruct*)` 3-arg overload is 5.7+ |
| `UnrealBridgeBlueprintLibrary::AddAsyncActionNode` | `UK2Node_AsyncAction::InitializeProxyFromFunction` doesn't exist on 5.4 |
| `UnrealBridgeGameplayAbilityLibrary::AddAbilityTaskNode` | `UK2Node_LatentAbilityCall` UCLASS in `GameplayAbilitiesEditor` is non-API in 5.4; `NewObject<>` of it fails to link from external modules |

## Inline shims (function works on both, different code paths)

These remain callable on 5.4 — the macro picks the right code path internally.

| Function | What 5.4 lacks | Shim |
|---|---|---|
| `UnrealBridgeAnimLibrary::SetAnimStateDefault` | `UAnimStateEntryNode::GetOutputPin()` | walk `Entry->Pins[]` for the first `EGPD_Output` pin |
| `UnrealBridgeGameplayAbilityLibrary::GetGameplayAbilityBlueprintInfo` and `ListGameplayAbilitiesByTag` | `UGameplayAbility::GetAssetTags()` | read legacy `CDO->AbilityTags` field |
| `UnrealBridgeBlueprintLibrary::GetPIENodeCoverage` | `FKismetDebugUtilities::FindSourceNodeForCodeLocation` const-correctness | `const_cast<UFunction*>(Func)` |
| `UnrealBridgeGameplayTagLibrary` — `EnsureSourceRedirectsPersisted`, `RenameGameplayTag`, `RemoveGameplayTagRedirect`, `ListGameplayTagRedirects` | 5.5 lacks `UGameplayTagsList::GameplayTagRedirects` (on `UGameplayTagsSettings` only); `RenameTagInINI` 3-arg overload added in 5.7 | `!UE_VERSION_OLDER_THAN(5, 6, 0)`: use `SourceTagList->GameplayTagRedirects`; legacy: parse per-source `+GameplayTagRedirects=` lines from disk ini. `RenameTagInINI` gated at `!UE_VERSION_OLDER_THAN(5, 7, 0)` for the `bRenameChildren` parameter |
| `UnrealBridgeAnimLibrary` — `GetAnimGraphNodes`, `ListAnimSlotsInABP`, `DumpAnimNodeProperties` | `UAnimGraphNode_Base::GetFNode` / `GetFNodeProperty` / `GetFNodeType` were `protected` before 5.4 (made `public` in 5.4) | `BridgeAnimNodeAccess::*` shim — gated at `UE_VERSION_OLDER_THAN(5, 4, 0)`, exposes the protected getters via `using`-declaration in a derived helper struct (compile-time access bypass, never instantiated) |
| All `TArray::Pop` / `RemoveAt` call sites with explicit shrinking flag | `EAllowShrinking` enum added in 5.4 (replaced `bool bAllowShrinking`) | `Plugin/.../Private/UnrealBridgeCompat.h` — defines `namespace EAllowShrinking { static constexpr bool No, Yes; }` on pre-5.4 so `Array.Pop(EAllowShrinking::No)` resolves to the legacy `bool` overload |
| `UnrealBridgeBlueprintLibrary` — Blueprint exception/debug paths | `Blueprint/BlueprintExceptionInfo.h` is 5.4+ only (split out of `UObject/Script.h`) | `#if !UE_VERSION_OLDER_THAN(5, 4, 0)` around the include; on 5.3 `FBlueprintExceptionInfo` is reachable via the already-included `UObject/Script.h` |
| `UnrealBridgeGameplayAbilityLibrary::ScanProperty` map/set iteration | 5.4 added `FScriptMapHelper::GetKeyPtr(FIterator)` / `GetValuePtr(FIterator)` / `FScriptSetHelper::GetElementPtr(FIterator)` overloads; 5.3 only has the `int32` overloads | Pass `*It` (dereferenced iterator → `int32`) to the legacy overload that exists in both 5.3 and 5.4+ — no version macro needed at the call site |

## How the gate macro works

```cpp
#include "Misc/EngineVersionComparison.h"

#if !UE_VERSION_OLDER_THAN(5, 7, 0)
// only compiled on 5.7+
#endif
```

The 5.7 threshold for the whole-library and single-UFUNCTION gates above
is conservative — those APIs may work on 5.5 / 5.6 too, but until the
matrix proves a lowered threshold passes BuildPlugin (the gated-IN code
isn't compiled on 5.5 / 5.6 yet, only the gated-OUT empty stubs are), we
keep the cutoff at 5.7. Inline shims, by contrast, *are* compiled on
every version in the matrix — so each `UE_VERSION_OLDER_THAN(M, m, 0)`
gate listed in the table above is verified across all Last verified
versions below.

## Verifying

```
python tools/build_matrix.py             # build against all configured engines
python tools/build_matrix.py --only 5.4  # only 5.4
python tools/build_matrix.py --only 5.7  # only 5.7
```

Last verified versions:
- UE 5.3 (point release: 5.3.2)
- UE 5.4 (point release: 5.4.4)
- UE 5.5 (point release: 5.5.4)
- UE 5.6 (point release: 5.6.1)
- UE 5.7 (point release: 5.7.1)

5.2 is configured in `tools/engines.local.json` with `enabled: false` — to
flip it on, expect additional pre-5.3 shims (the matrix has not yet been
exercised against it).

## Lowering the threshold

When you install another 5.x and add it to `tools/engines.local.json`,
re-running the matrix will reveal which of the 5.7-gated items also work on
the new version. To lower a gate, e.g. from 5.7 to 5.5:

1. Replace `UE_VERSION_OLDER_THAN(5, 7, 0)` with `UE_VERSION_OLDER_THAN(5, 5, 0)`
   on the affected sites (in both the main `.cpp` body gate and the
   corresponding `_Stubs.cpp` inverse gate).
2. Re-run the matrix; verify the lowered version passes.
3. Update the tables above.
