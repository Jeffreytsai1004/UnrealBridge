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
 * One entry from the registered tag-source list (an ini file, native
 * declaration, or DataTable that contributes tags to UGameplayTagsManager).
 */
USTRUCT(BlueprintType)
struct FBridgeTagSourceListing
{
	GENERATED_BODY()

	/** Source FName as the manager knows it (e.g. "DefaultGameplayTags.ini",
	 *  "MyMod.ini", "Native", or a DataTable's package name). Pass this back
	 *  as the SourceIni argument to AddGameplayTag when you want the tag
	 *  written to a specific source. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|GameplayTag")
	FString SourceName;

	/** EGameplayTagSourceType: "Native" | "DefaultTagList" | "TagList" |
	 *  "RestrictedTagList" | "DataTable". */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|GameplayTag")
	FString SourceType;

	/** Resolved on-disk path for ini-backed sources; empty for Native /
	 *  DataTable sources where the SourceName itself is the locator. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|GameplayTag")
	FString ConfigFilePath;

	/** True if the bridge mutation APIs (AddGameplayTag / RenameGameplayTag /
	 *  RemoveGameplayTag) can write to this source. False for Native — those
	 *  tags require C++ source edits — and (for the moment) DataTable, which
	 *  needs row edits via the DataTable library. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|GameplayTag")
	bool bIsWritable = false;
};

/**
 * One `+GameplayTagRedirects=(OldTagName="X",NewTagName="Y")` entry as
 * stored in some source ini. Returned by ListGameplayTagRedirects so an
 * agent can enumerate-then-clean without having to remember every (old,
 * new) pair it created.
 */
USTRUCT(BlueprintType)
struct FBridgeTagRedirectEntry
{
	GENERATED_BODY()

	/** OldTagName in the redirect — the string that gets remapped on lookup. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|GameplayTag")
	FString OldTag;

	/** NewTagName — the resolved tag. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|GameplayTag")
	FString NewTag;

	/** Source FName (e.g. "DefaultGameplayTags.ini") this redirect lives in.
	 *  Pass back to remove_gameplay_tag_redirect via the (old, new) pair. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|GameplayTag")
	FString SourceName;
};

/**
 * GameplayTag-specific helpers built on top of the AssetRegistry's
 * SearchableName index, UGameplayTagsManager, and IGameplayTagsEditorModule.
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

	// ── Tag source enumeration ──────────────────────────────────────

	/**
	 * Every place tags can come from in this project — ini files, native
	 * code modules, DataTables. Pass the returned `source_name` as
	 * `SourceIni` to `AddGameplayTag` to control where a new tag lands.
	 *
	 * @param FilterType  Optional. One of "Native", "DefaultTagList",
	 *                    "TagList", "RestrictedTagList", "DataTable".
	 *                    Empty = all types.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|GameplayTag")
	static TArray<FBridgeTagSourceListing> ListTagSourceInis(const FString& FilterType = TEXT(""));

	// ── Mutations (write to ini / patch redirectors) ────────────────
	//
	// All three write to disk via IGameplayTagsEditorModule. Operations
	// against Native-source tags fail — those live in C++ source.
	// Restricted tags require special handling and are not yet exposed.

	/**
	 * Add a new GameplayTag, optionally choosing which source ini to write
	 * to. Empty `SourceIni` = the project's default
	 * (Config/DefaultGameplayTags.ini).
	 *
	 * @param NewTag        Full tag string, e.g. "Combat.Stun".
	 * @param SourceIni     Source name as returned by ListTagSourceInis,
	 *                      e.g. "DefaultGameplayTags.ini" or
	 *                      "MyMod.ini". Empty = default.
	 * @param Comment       Optional dev comment.
	 * @param bIsRestricted Restricted tag (rarely needed). Defaults to false.
	 * @return true on success; false if the tag already exists, the source
	 *         doesn't exist, or the editor module rejected the write.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|GameplayTag")
	static bool AddGameplayTag(
		const FString& NewTag,
		const FString& SourceIni = TEXT(""),
		const FString& Comment = TEXT(""),
		bool bIsRestricted = false);

	/**
	 * Rename a GameplayTag in-place (in its source ini). UE auto-inserts a
	 * `+GameplayTagRedirects=(OldTagName=...,NewTagName=...)` line so that
	 * existing asset references continue to resolve to the new name —
	 * *don't* also call this manually.
	 *
	 * @param OldTag           Existing full tag string.
	 * @param NewTag           Replacement full tag string.
	 * @param bRenameChildren  Also rename every child tag (Combat → CombatV2
	 *                         renames Combat.Hit → CombatV2.Hit etc.).
	 *                         Defaults to true.
	 * @return true on success; false if OldTag doesn't exist, NewTag already
	 *         exists, or the editor module rejected the rename.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|GameplayTag")
	static bool RenameGameplayTag(
		const FString& OldTag,
		const FString& NewTag,
		bool bRenameChildren = true);

	/**
	 * Delete a GameplayTag from its source ini. Use
	 * `find_assets_referencing_tag(tag, include_children=True)` first to
	 * confirm zero references — UE doesn't insert a redirect on delete, and
	 * stale asset references will surface as warnings on next load.
	 *
	 * Native tags can't be deleted via this API; remove them from C++ source.
	 *
	 * @return true on success; false if the tag doesn't exist or the editor
	 *         module rejected the delete (e.g. tag is native).
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|GameplayTag")
	static bool RemoveGameplayTag(const FString& TagString);

	/**
	 * Remove a `+GameplayTagRedirects=(OldTagName="X",NewTagName="Y")` entry
	 * from the project's tag config. Use when undoing a mistaken
	 * `rename_gameplay_tag` or sweeping a stale orphan redirect that
	 * `remove_gameplay_tag` left behind.
	 *
	 * Searches every writable source ini for a redirect matching BOTH
	 * `OldTag` and `NewTag` (the pair must match exactly — preventing typo
	 * deletes). Removes from `UGameplayTagsList::GameplayTagRedirects`
	 * (in-memory) AND strips the corresponding line from the on-disk ini,
	 * then rebuilds the tag tree so the manager forgets the redirect
	 * immediately.
	 *
	 * @return true if a matching redirect was found and removed; false if
	 *         no redirect with that exact (OldTag, NewTag) pair exists in
	 *         any writable source.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|GameplayTag")
	static bool RemoveGameplayTagRedirect(const FString& OldTag, const FString& NewTag);

	/**
	 * Enumerate every `+GameplayTagRedirects=` entry currently registered in
	 * any writable source. Use to drive enumerate-then-sweep workflows
	 * (e.g. "list every orphan redirect and remove it") without having to
	 * remember the (old, new) pairs you wrote earlier.
	 *
	 * @param SourceIniFilter   Optional source FName to scope to (e.g.
	 *                          "DefaultGameplayTags.ini"). Empty = all
	 *                          writable sources.
	 * @param OldTagPrefixFilter  Optional prefix to filter `OldTag` by
	 *                          (e.g. "Combat." to find every redirect
	 *                          whose OldTag is under Combat).
	 *                          Empty = no filter.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|GameplayTag")
	static TArray<FBridgeTagRedirectEntry> ListGameplayTagRedirects(
		const FString& SourceIniFilter = TEXT(""),
		const FString& OldTagPrefixFilter = TEXT(""));
};
