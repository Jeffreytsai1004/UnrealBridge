#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "UnrealBridgeGameplayTagLibrary.generated.h"

/**
 * Where a GameplayTag is registered. Returned by GetTagSourceInfo so an
 * agent can answer "if I delete this tag, which file do I edit?".
 */
USTRUCT(BlueprintType)
struct FBridgeTagSourceInfo
{
	GENERATED_BODY()

	/** The tag string the lookup was performed for. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|GameplayTag")
	FString TagString;

	/** True if the tag was found in the manager. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|GameplayTag")
	bool bFound = false;

	/**
	 * EGameplayTagSourceType enum name:
	 *   "Native"             — defined via UE_DEFINE_GAMEPLAY_TAG / native macro
	 *   "DefaultTagList"     — DefaultGameplayTags.ini (Project Settings → GameplayTags)
	 *   "TagList"            — another .ini file under Config/Tags/
	 *   "RestrictedTagList"  — restricted-tag .ini
	 *   "DataTable"          — declared in a UDataTable asset
	 *   "Invalid"/"NotFound" — fallback when lookup fails
	 *
	 * Tags can have multiple sources (e.g. native + ini). All are listed in
	 * AdditionalSources; this field reflects the first/primary one.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|GameplayTag")
	FString SourceType;

	/**
	 * Human-readable location:
	 *   - For Native: the source name (FName from the manager)
	 *   - For DefaultTagList / TagList / RestrictedTagList: the .ini config file path
	 *   - For DataTable: the source name (FName) — usually the DataTable asset name
	 */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|GameplayTag")
	FString SourceLocation;

	/** True if this tag was explicitly added; false if it only exists as an
	 *  implicit parent of a more specific child tag. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|GameplayTag")
	bool bIsExplicit = false;

	/** True if this is a restricted gameplay tag. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|GameplayTag")
	bool bIsRestricted = false;

	/** All source names that contributed this tag (rare — most tags have one). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|GameplayTag")
	TArray<FString> AdditionalSources;
};

/**
 * GameplayTag-specific lookup helpers built on top of the AssetRegistry's
 * SearchableName index and UGameplayTagsManager.
 *
 * For generic SearchableName queries (PrimaryAssetId, GameplayCueTag,
 * project-defined named values) use UnrealBridgeAssetLibrary directly:
 * find_assets_referencing_searchable_name / get_searchable_names_used_by_asset.
 */
UCLASS()
class UNREALBRIDGE_API UUnrealBridgeGameplayTagLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:

	/**
	 * Find every asset that references a given GameplayTag — equivalent to
	 * the editor's right-click "Search for References" on a tag.
	 *
	 * @param TagString          Full tag, e.g. "Combat.Hit".
	 * @param bIncludeChildren   When true, also find assets referencing any
	 *                           child tag (Combat → Combat.Hit, Combat.Block,
	 *                           Combat.Hit.Critical, ...). Walks the tag tree
	 *                           via UGameplayTagsManager and unions results.
	 * @param PackagePathFilter  Optional, e.g. "/Game" to exclude engine and
	 *                           plugins. Empty = no filter.
	 * @param MaxResults         Cap. 0 = unlimited.
	 * @return Sorted, de-duplicated package paths.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|GameplayTag")
	static TArray<FString> FindAssetsReferencingTag(
		const FString& TagString,
		bool bIncludeChildren,
		const FString& PackagePathFilter,
		int32 MaxResults);

	/**
	 * Every tag the GameplayTagsManager knows about — including ones that
	 * have never been referenced by any asset (which the SearchableName
	 * index would miss). Backed by RequestAllGameplayTags.
	 *
	 * @param FilterPrefix  Optional, e.g. "Ability.Combat" to only return
	 *                      that branch (and below). Empty = all tags.
	 * @param MaxResults    Cap. 0 = unlimited.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|GameplayTag")
	static TArray<FString> ListAllRegisteredTags(
		const FString& FilterPrefix,
		int32 MaxResults);

	/**
	 * Where is this tag defined? Use this when planning to rename / delete a
	 * tag — answers "which .ini or DataTable do I edit?".
	 *
	 * Returned struct's bFound == false when the tag is not in the manager
	 * (typo, plugin not loaded, etc.).
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|GameplayTag")
	static FBridgeTagSourceInfo GetTagSourceInfo(const FString& TagString);
};
