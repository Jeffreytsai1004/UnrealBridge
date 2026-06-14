#include "UnrealBridgeGameplayTagLibrary.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "GameplayTagContainer.h"
#include "GameplayTagRedirectors.h"
#if WITH_GAMEPLAYTAGSEDITOR
#include "GameplayTagsEditorModule.h"
#endif
#include "GameplayTagsManager.h"
#include "GameplayTagsSettings.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"

namespace BridgeGameplayTagOps
{
	/** EGameplayTagSourceType → short string. Centralised so both
	 *  GetTagSourceInfo and any future reverse lookup share names. */
	const TCHAR* SourceTypeToString(EGameplayTagSourceType Type)
	{
		switch (Type)
		{
			case EGameplayTagSourceType::Native:             return TEXT("Native");
			case EGameplayTagSourceType::DefaultTagList:     return TEXT("DefaultTagList");
			case EGameplayTagSourceType::TagList:            return TEXT("TagList");
			case EGameplayTagSourceType::RestrictedTagList:  return TEXT("RestrictedTagList");
			case EGameplayTagSourceType::DataTable:          return TEXT("DataTable");
			default:                                          return TEXT("Invalid");
		}
	}

	/** Resolve a TagSource record into a human-readable file location.
	 *  For ini-backed sources we pull the actual config file path; for
	 *  Native and DataTable we fall back to the source's FName. */
	FString ResolveSourceLocation(const FGameplayTagSource* Source)
	{
		if (!Source) return FString();

		switch (Source->SourceType)
		{
			case EGameplayTagSourceType::DefaultTagList:
			case EGameplayTagSourceType::TagList:
			case EGameplayTagSourceType::RestrictedTagList:
			{
				const FString ConfigPath = Source->GetConfigFileName();
				return ConfigPath.IsEmpty() ? Source->SourceName.ToString() : ConfigPath;
			}
			default:
				return Source->SourceName.ToString();
		}
	}

	/**
	 * UE 5.7 has a quirk where some Add/Rename/Remove mutations re-serialise
	 * `UGameplayTagsSettings` and silently drop redirects whose `OldTagName`
	 * has no live in-memory tag node — which is exactly the shape every
	 * just-renamed redirect has (the OldTag is gone, only the redirect
	 * preserves lookups). The in-memory `UGameplayTagsList::GameplayTagRedirects`
	 * array still has the redirect for the rest of the session, so the bug
	 * only surfaces on the *next* editor restart, when on-disk state wins.
	 *
	 * This helper closes the gap by reading the on-disk ini and re-appending
	 * any redirects from the in-memory list that aren't textually present.
	 * Called from Add/Rename/Remove after the underlying editor module call.
	 *
	 * Returns the number of redirect lines that were re-appended (0 = ini
	 * already matches in-memory state).
	 */
	int32 EnsureSourceRedirectsPersisted(FName SourceName)
	{
		if (SourceName.IsNone()) return 0;

		UGameplayTagsManager& TagsMgr = UGameplayTagsManager::Get();
		const FGameplayTagSource* Source = TagsMgr.FindTagSource(SourceName);
#if !UE_VERSION_OLDER_THAN(5, 6, 0)
		if (!Source || !Source->SourceTagList) return 0;

		const FString IniPath = Source->GetConfigFileName();
		if (IniPath.IsEmpty()) return 0;

		FString IniText;
		if (!FFileHelper::LoadFileToString(IniText, *IniPath)) return 0;

		// Build the set of redirect lines that should be present.
		TArray<FString> MissingLines;
		for (const FGameplayTagRedirect& R : Source->SourceTagList->GameplayTagRedirects)
		{
			if (R.OldTagName.IsNone() || R.NewTagName.IsNone()) continue;
			const FString Line = FString::Printf(
				TEXT("+GameplayTagRedirects=(OldTagName=\"%s\",NewTagName=\"%s\")"),
				*R.OldTagName.ToString(), *R.NewTagName.ToString());
			if (!IniText.Contains(Line, ESearchCase::CaseSensitive))
			{
				MissingLines.Add(Line);
			}
		}

		if (MissingLines.Num() == 0) return 0;

		// Insert before the first +GameplayTagList= line so the redirects stay
		// clustered with their existing siblings (UE convention). Fall back to
		// EOF append if no GameplayTagList line exists.
		const FString TagListMarker = TEXT("+GameplayTagList=");
		const FString JoinedNew = FString::Join(MissingLines, TEXT("\r\n")) + TEXT("\r\n");

		const int32 InsertIdx = IniText.Find(TagListMarker, ESearchCase::CaseSensitive, ESearchDir::FromStart);
		if (InsertIdx != INDEX_NONE)
		{
			IniText.InsertAt(InsertIdx, JoinedNew);
		}
		else
		{
			if (!IniText.EndsWith(TEXT("\n"))) IniText += TEXT("\r\n");
			IniText += JoinedNew;
		}

		if (!FFileHelper::SaveStringToFile(IniText, *IniPath))
		{
			UE_LOG(LogTemp, Warning,
				TEXT("EnsureSourceRedirectsPersisted: failed to write %s"), *IniPath);
			return 0;
		}

		UE_LOG(LogTemp, Log,
			TEXT("EnsureSourceRedirectsPersisted: re-appended %d redirect(s) to %s"),
			MissingLines.Num(), *IniPath);
		return MissingLines.Num();
#else
		// 5.5: UGameplayTagsSettings::GameplayTagRedirects is a global singleton
		// with no per-source attribution — there is no way to reconstruct
		// "what redirects should be in source X" from in-memory state, so the
		// disk-vs-memory reconciliation this helper performs on 5.6+ has
		// nothing to bind to. The 5.7 serialise-drop quirk also doesn't
		// manifest on 5.5 (only the post-5.6 SourceTagList serializer hits it),
		// so a no-op is correct on legacy.
		return 0;
#endif
	}

	/** Look up a tag's primary source name (the FName the manager indexes
	 *  it by, e.g. "DefaultGameplayTags.ini"). Used by EnsureRedirectsPersisted
	 *  callers that only have a tag string, not a source. Returns NAME_None if
	 *  the tag isn't registered or has no source. */
	FName ResolveTagSourceName(const FString& TagString)
	{
#if WITH_EDITORONLY_DATA
		UGameplayTagsManager& TagsMgr = UGameplayTagsManager::Get();
		const TSharedPtr<FGameplayTagNode> Node = TagsMgr.FindTagNode(FName(*TagString));
		if (!Node.IsValid()) return NAME_None;
		const TArray<FName>& Sources = Node->GetAllSourceNames();
		return Sources.Num() > 0 ? Sources[0] : NAME_None;
#else
		return NAME_None;
#endif
	}
}

TArray<FString> UUnrealBridgeGameplayTagLibrary::FindAssetsReferencingTag(
	const FString& TagString, bool bIncludeChildren,
	const FString& PackagePathFilter, int32 MaxResults)
{
	TArray<FString> Result;
	if (TagString.IsEmpty()) return Result;

	UGameplayTagsManager& TagsMgr = UGameplayTagsManager::Get();
	const FGameplayTag RootTag = TagsMgr.RequestGameplayTag(FName(*TagString), /*ErrorIfNotFound=*/false);
	if (!RootTag.IsValid())
	{
		// Fall through and try the literal name anyway — useful for
		// finding stale references to a tag that has been deleted but
		// is still indexed in old assets.
		UE_LOG(LogTemp, Verbose,
			TEXT("FindAssetsReferencingTag: tag '%s' not registered; querying SearchableName by raw name only."),
			*TagString);
	}

	TArray<FName> TagsToQuery;
	TagsToQuery.Add(FName(*TagString));

	if (bIncludeChildren && RootTag.IsValid())
	{
		const FGameplayTagContainer ChildContainer = TagsMgr.RequestGameplayTagChildren(RootTag);
		for (const FGameplayTag& Child : ChildContainer)
		{
			TagsToQuery.AddUnique(Child.GetTagName());
		}
	}

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	UScriptStruct* TagStruct = FGameplayTag::StaticStruct();

	TSet<FString> Unique;
	for (const FName& TagName : TagsToQuery)
	{
		const FAssetIdentifier TagId(TagStruct, TagName);
		TArray<FAssetIdentifier> Refs;
		AssetRegistry.GetReferencers(TagId, Refs,
			UE::AssetRegistry::EDependencyCategory::SearchableName);

		for (const FAssetIdentifier& Ref : Refs)
		{
			if (Ref.PackageName.IsNone()) continue;
			const FString PackageName = Ref.PackageName.ToString();
			if (!PackagePathFilter.IsEmpty() && !PackageName.StartsWith(PackagePathFilter)) continue;
			Unique.Add(PackageName);
			if (MaxResults > 0 && Unique.Num() >= MaxResults) break;
		}
		if (MaxResults > 0 && Unique.Num() >= MaxResults) break;
	}

	Result = Unique.Array();
	Result.Sort();
	if (MaxResults > 0 && Result.Num() > MaxResults) Result.SetNum(MaxResults);
	return Result;
}

TArray<FString> UUnrealBridgeGameplayTagLibrary::ListAllRegisteredTags(
	const FString& FilterPrefix, int32 MaxResults)
{
	TArray<FString> Result;

	UGameplayTagsManager& TagsMgr = UGameplayTagsManager::Get();
	FGameplayTagContainer AllTags;
	TagsMgr.RequestAllGameplayTags(AllTags, /*OnlyIncludeDictionaryTags=*/false);

	for (const FGameplayTag& Tag : AllTags)
	{
		FString TagStr = Tag.ToString();
		if (!FilterPrefix.IsEmpty() && !TagStr.StartsWith(FilterPrefix)) continue;
		Result.Add(MoveTemp(TagStr));
		if (MaxResults > 0 && Result.Num() >= MaxResults) break;
	}

	Result.Sort();
	return Result;
}

FBridgeTagSourceInfo UUnrealBridgeGameplayTagLibrary::GetTagSourceInfo(const FString& TagString)
{
	FBridgeTagSourceInfo Info;
	Info.TagString = TagString;
	Info.SourceType = TEXT("NotFound");

	if (TagString.IsEmpty()) return Info;

	UGameplayTagsManager& TagsMgr = UGameplayTagsManager::Get();
	const TSharedPtr<FGameplayTagNode> Node = TagsMgr.FindTagNode(FName(*TagString));
	if (!Node.IsValid()) return Info;

	Info.bFound = true;
	Info.bIsRestricted = Node->IsRestrictedGameplayTag();

#if WITH_EDITORONLY_DATA
	Info.bIsExplicit = Node->IsExplicitTag();

	const TArray<FName>& SourceNames = Node->GetAllSourceNames();
	if (SourceNames.Num() == 0)
	{
		Info.SourceType = TEXT("Unknown");
		return Info;
	}

	// Primary = first source.
	const FName PrimarySourceName = SourceNames[0];
	if (const FGameplayTagSource* PrimarySource = TagsMgr.FindTagSource(PrimarySourceName))
	{
		Info.SourceType = BridgeGameplayTagOps::SourceTypeToString(PrimarySource->SourceType);
		Info.SourceLocation = BridgeGameplayTagOps::ResolveSourceLocation(PrimarySource);
	}
	else
	{
		Info.SourceType = TEXT("Unknown");
		Info.SourceLocation = PrimarySourceName.ToString();
	}

	// Additional sources (rare — when the same tag is registered from
	// multiple .ini / native locations).
	for (int32 i = 1; i < SourceNames.Num(); ++i)
	{
		Info.AdditionalSources.Add(SourceNames[i].ToString());
	}
#else
	// Non-editor build can't introspect source names. Should not happen
	// for this editor-only plugin, but guard for safety.
	Info.bIsExplicit = true;
	Info.SourceType = TEXT("Unknown");
#endif

	return Info;
}

// ── Tag source enumeration ──────────────────────────────────────

TArray<FBridgeTagSourceListing> UUnrealBridgeGameplayTagLibrary::ListTagSourceInis(
	const FString& FilterType)
{
	TArray<FBridgeTagSourceListing> Result;
	UGameplayTagsManager& TagsMgr = UGameplayTagsManager::Get();

	// FindTagSourcesWithType is the only public window onto the otherwise-private
	// TagSources map. Iterate every category and union the results.
	static const TArray<EGameplayTagSourceType> AllTypes = {
		EGameplayTagSourceType::Native,
		EGameplayTagSourceType::DefaultTagList,
		EGameplayTagSourceType::TagList,
		EGameplayTagSourceType::RestrictedTagList,
		EGameplayTagSourceType::DataTable,
	};

	const FName FilterFName = FilterType.IsEmpty() ? NAME_None : FName(*FilterType);

	for (EGameplayTagSourceType Type : AllTypes)
	{
		const FString TypeName = BridgeGameplayTagOps::SourceTypeToString(Type);
		if (!FilterFName.IsNone() && TypeName != FilterType) continue;

		TArray<const FGameplayTagSource*> Sources;
		TagsMgr.FindTagSourcesWithType(Type, Sources);

		for (const FGameplayTagSource* Source : Sources)
		{
			if (!Source) continue;
			FBridgeTagSourceListing Entry;
			Entry.SourceName = Source->SourceName.ToString();
			Entry.SourceType = TypeName;
			Entry.ConfigFilePath = BridgeGameplayTagOps::ResolveSourceLocation(Source);
			// Native sources require C++ edits — not writable from here.
			// DataTable would need DataTable-row mutation, also not yet wired.
			// Anything ini-backed (DefaultTagList / TagList / RestrictedTagList)
			// IS writable via IGameplayTagsEditorModule.
			switch (Type)
			{
				case EGameplayTagSourceType::DefaultTagList:
				case EGameplayTagSourceType::TagList:
				case EGameplayTagSourceType::RestrictedTagList:
					Entry.bIsWritable = true;
					break;
				default:
					Entry.bIsWritable = false;
					break;
			}
			Result.Add(MoveTemp(Entry));
		}
	}

	return Result;
}

// ── Mutations ──

bool UUnrealBridgeGameplayTagLibrary::AddGameplayTag(
	const FString& NewTag, const FString& SourceIni, const FString& Comment, bool bIsRestricted
)
{
#if WITH_GAMEPLAYTAGSEDITOR
	if (!IGameplayTagsEditorModule::IsAvailable())
	{
		UE_LOG(LogTemp, Warning, TEXT("AddGameplayTag: IGameplayTagsEditorModule unavailable in UE 5.7"));
		return false;
	}
	if (NewTag.IsEmpty()) return false;
	IGameplayTagsEditorModule& EditorModule = IGameplayTagsEditorModule::Get();
	bool bAdded = EditorModule.AddNewGameplayTagToINI(NewTag, Comment, FName(*SourceIni));
	if (bAdded)
	{
		UE_LOG(LogTemp, Log, TEXT("AddGameplayTag: added %s"), *NewTag);
	}
	return bAdded;
#else
	return false;
#endif
}

bool UUnrealBridgeGameplayTagLibrary::RenameGameplayTag(
	const FString& OldTag, const FString& NewTag, bool bRenameChildren
)
{
#if WITH_GAMEPLAYTAGSEDITOR
	if (!IGameplayTagsEditorModule::IsAvailable())
	{
		UE_LOG(LogTemp, Warning, TEXT("RenameGameplayTag: IGameplayTagsEditorModule unavailable in UE 5.7"));
		return false;
	}
	if (OldTag.IsEmpty() || NewTag.IsEmpty()) return false;
	bool bRenamed = IGameplayTagsEditorModule::Get().RenameTagInINI(OldTag, NewTag, bRenameChildren);
	if (bRenamed)
	{
		UE_LOG(LogTemp, Log, TEXT("RenameGameplayTag: %s -> %s"), *OldTag, *NewTag);
	}
	return bRenamed;
#else
	return false;
#endif
}

bool UUnrealBridgeGameplayTagLibrary::RemoveGameplayTag(
	const FString& TagString
)
{
#if WITH_GAMEPLAYTAGSEDITOR
	if (!IGameplayTagsEditorModule::IsAvailable())
	{
		UE_LOG(LogTemp, Warning, TEXT("RemoveGameplayTag: IGameplayTagsEditorModule unavailable in UE 5.7"));
		return false;
	}
	if (TagString.IsEmpty()) return false;
	UGameplayTagsManager& TagsMgr = UGameplayTagsManager::Get();
	const FGameplayTag Tag = TagsMgr.RequestGameplayTag(FName(*TagString), false);
	if (!Tag.IsValid()) return false;
	TSharedPtr<FGameplayTagNode> Node = TagsMgr.FindTagNode(Tag);
	if (!Node.IsValid()) return false;
	bool bRemoved = IGameplayTagsEditorModule::Get().DeleteTagFromINI(Node);
	if (bRemoved)
	{
		UE_LOG(LogTemp, Log, TEXT("RemoveGameplayTag: removed %s"), *TagString);
	}
	return bRemoved;
#else
	return false;
#endif
}

bool UUnrealBridgeGameplayTagLibrary::RemoveGameplayTagRedirect(
	const FString& OldTag, const FString& NewTag, bool bRemoveChildren
)
{
#if WITH_GAMEPLAYTAGSEDITOR
	if (!IGameplayTagsEditorModule::IsAvailable())
	{
		UE_LOG(LogTemp, Warning, TEXT("RemoveGameplayTagRedirect: IGameplayTagsEditorModule unavailable in UE 5.7"));
		return false;
	}
	if (OldTag.IsEmpty() || NewTag.IsEmpty()) return false;
	bool bRemoved = IGameplayTagsEditorModule::Get().RemoveTagRedirect(OldTag, NewTag);
	if (bRemoved)
	{
		UE_LOG(LogTemp, Log, TEXT("RemoveGameplayTagRedirect: %s -> %s"), *OldTag, *NewTag);
	}
	return bRemoved;
#else
	return false;
#endif
}

TArray<FBridgeTagRedirectEntry> UUnrealBridgeGameplayTagLibrary::ListGameplayTagRedirects(
	const FString& SourceIniFilter, const FString& OldTagPrefixFilter)
{
	TArray<FBridgeTagRedirectEntry> Result;
	UGameplayTagsManager& TagsMgr = UGameplayTagsManager::Get();

	static const TArray<EGameplayTagSourceType> WritableTypes = {
		EGameplayTagSourceType::DefaultTagList,
		EGameplayTagSourceType::TagList,
		EGameplayTagSourceType::RestrictedTagList,
	};

	TArray<const FGameplayTagSource*> AllSources;
	TagsMgr.FindTagSourcesWithType(EGameplayTagSourceType::DefaultTagList, AllSources);
	TagsMgr.FindTagSourcesWithType(EGameplayTagSourceType::TagList, AllSources);
	TagsMgr.FindTagSourcesWithType(EGameplayTagSourceType::RestrictedTagList, AllSources);

	TSet<FString> Seen;
	for (const FGameplayTagSource* Source : AllSources)
	{
		if (!Source) continue;
		const FString SourceName = Source->SourceName.ToString();

		// SourceIniFilter: match by FName string if provided.
		if (!SourceIniFilter.IsEmpty() && SourceName != SourceIniFilter) continue;

		const FString IniPath = Source->GetConfigFileName();
		if (IniPath.IsEmpty()) continue;

		FString IniText;
		if (!FFileHelper::LoadFileToString(IniText, *IniPath)) continue;

		// Scan for +GameplayTagRedirects= lines.
		const FString Marker = TEXT("+GameplayTagRedirects=");
		int32 Pos = 0;
		while ((Pos = IniText.Find(Marker, ESearchCase::CaseSensitive, ESearchDir::FromStart, Pos)) != INDEX_NONE)
		{
			Pos += Marker.Len();
			// Collect the parenthesised value.
			if (Pos >= IniText.Len() || IniText[Pos] != TEXT('(')) continue;
			int32 Close = IniText.Find(TEXT(")"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Pos + 1);
			if (Close == INDEX_NONE) break;

			FString RedirectBlock = IniText.Mid(Pos + 1, Close - Pos - 1);
			FString OldTag, NewTag;
			if (!FParse::Value(*RedirectBlock, TEXT("OldTagName=\""), OldTag, false)) continue;
			if (!FParse::Value(*RedirectBlock, TEXT("NewTagName=\""), NewTag, false)) continue;

			// Strip trailing quote.
			if (OldTag.EndsWith(TEXT("\""))) OldTag.LeftChopInline(1);
			if (NewTag.EndsWith(TEXT("\""))) NewTag.LeftChopInline(1);

			// Deduplicate across sources.
			if (Seen.Contains(OldTag)) continue;
			Seen.Add(OldTag);

			// OldTagPrefixFilter.
			if (!OldTagPrefixFilter.IsEmpty() && !OldTag.StartsWith(OldTagPrefixFilter)) continue;

			FBridgeTagRedirectEntry Entry;
			Entry.OldTag = OldTag;
			Entry.NewTag = NewTag;
			Entry.SourceName = SourceName;
			Result.Add(MoveTemp(Entry));
		}
	}

	Result.Sort([](const FBridgeTagRedirectEntry& A, const FBridgeTagRedirectEntry& B) {
		return A.OldTag < B.OldTag;
	});
	return Result;
}

