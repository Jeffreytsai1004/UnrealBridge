#include "UnrealBridgeTypeParse.h"

#include "EdGraphSchema_K2.h"
#include "UObject/Class.h"
#include "UObject/Interface.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Package.h"

namespace BridgeTypeParseImpl
{

namespace
{
	/** Lookup a UScriptStruct by simple name, trying common engine packages. */
	UScriptStruct* FindStructByName(const FString& Name)
	{
		UScriptStruct* S = FindObject<UScriptStruct>(nullptr, *Name);
		if (!S) S = FindObject<UScriptStruct>(nullptr, *(FString(TEXT("/Script/CoreUObject.")) + Name));
		if (!S) S = FindObject<UScriptStruct>(nullptr, *(FString(TEXT("/Script/Engine.")) + Name));
		if (!S) S = FindObject<UScriptStruct>(nullptr, *(FString(TEXT("/Script/GameplayTags.")) + Name));
		return S;
	}

	/** Lookup a UClass by simple name. Hot paths: bare name, Engine, CoreUObject.
	 *  Cold path (sweep over loaded UClasses) catches module-scoped classes
	 *  like /Script/GameplayTags.GameplayTagAssetInterface that no fixed
	 *  prefix list would resolve. */
	UClass* FindClassByName(const FString& Name)
	{
		UClass* C = FindObject<UClass>(nullptr, *Name);
		if (!C) C = FindObject<UClass>(nullptr, *(FString(TEXT("/Script/Engine.")) + Name));
		if (!C) C = FindObject<UClass>(nullptr, *(FString(TEXT("/Script/CoreUObject.")) + Name));
		if (!C)
		{
			for (TObjectIterator<UClass> It; It; ++It)
			{
				if (It->GetName() == Name) { return *It; }
			}
		}
		return C;
	}

	/** Lookup a UEnum by simple name. UE's reflected enums live in many modules;
	 *  fall back to a global UEnum sweep if the common-namespace lookups miss. */
	UEnum* FindEnumByName(const FString& Name)
	{
		UEnum* E = FindObject<UEnum>(nullptr, *Name);
		if (!E) E = FindObject<UEnum>(nullptr, *(FString(TEXT("/Script/Engine.")) + Name));
		if (!E) E = FindObject<UEnum>(nullptr, *(FString(TEXT("/Script/CoreUObject.")) + Name));
		if (!E)
		{
			// Sweep: many enums live in module packages we can't enumerate
			// statically. Linear scan over all loaded UEnums is acceptable
			// for the rare authoring path; if the enum is unloaded this
			// won't find it but the caller already exhausted hot paths.
			for (TObjectIterator<UEnum> It; It; ++It)
			{
				if (It->GetName() == Name) { return *It; }
			}
		}
		return E;
	}

	/** "Foo<Bar>"  →  ("Foo", "Bar")  if both brackets present at the ends.
	 *  Returns false if not a single templated form. Inner may contain commas
	 *  (caller responsible for splitting them — used by Map<K, V>). */
	bool TryExtractTemplated(const FString& In, const TCHAR* Tag, FString& OutInner)
	{
		const FString Prefix = FString::Printf(TEXT("%s<"), Tag);
		if (!In.StartsWith(Prefix, ESearchCase::IgnoreCase)) return false;
		if (!In.EndsWith(TEXT(">"))) return false;
		OutInner = In.Mid(Prefix.Len(), In.Len() - Prefix.Len() - 1).TrimStartAndEnd();
		return !OutInner.IsEmpty();
	}

	/** Strip a "Foo of " prefix if present. Returns true and writes the rest. */
	bool TryExtractPrefix(const FString& In, const TCHAR* Tag, FString& OutInner)
	{
		const FString Prefix = FString::Printf(TEXT("%s of "), Tag);
		if (!In.StartsWith(Prefix, ESearchCase::IgnoreCase)) return false;
		OutInner = In.Mid(Prefix.Len()).TrimStartAndEnd();
		return !OutInner.IsEmpty();
	}
}

bool ParseTypeString(const FString& TypeStr, FEdGraphPinType& OutPinType)
{
	OutPinType = FEdGraphPinType();

	FString Type = TypeStr.TrimStartAndEnd();
	if (Type.IsEmpty()) return false;

	// ── Container prefixes — strip first, then re-enter parser on inner type ──
	// Recognised forms:
	//   "Array of X" / "Array<X>"
	//   "Set of X"   / "Set<X>"
	//   "Map of K to V" / "Map<K, V>"
	{
		FString Inner;
		EPinContainerType DetectedContainer = EPinContainerType::None;

		if (TryExtractPrefix(Type, TEXT("Array"), Inner) ||
		    TryExtractTemplated(Type, TEXT("Array"), Inner))
		{
			DetectedContainer = EPinContainerType::Array;
		}
		else if (TryExtractPrefix(Type, TEXT("Set"), Inner) ||
		         TryExtractTemplated(Type, TEXT("Set"), Inner))
		{
			DetectedContainer = EPinContainerType::Set;
		}

		if (DetectedContainer != EPinContainerType::None)
		{
			FEdGraphPinType InnerType;
			if (!ParseTypeString(Inner, InnerType)) return false;
			// Containers don't nest in UE's editor pin system (no TArray<TSet<>>).
			if (InnerType.ContainerType != EPinContainerType::None) return false;

			OutPinType = InnerType;
			OutPinType.ContainerType = DetectedContainer;
			return true;
		}
	}

	// Map "Map<K, V>" or "Map of K to V" — splits two halves and re-enters.
	{
		FString MapInner;
		FString KeyStr, ValueStr;
		bool bMap = false;

		if (TryExtractTemplated(Type, TEXT("Map"), MapInner))
		{
			// Split on the topmost comma (one level deep in <>).
			int32 Depth = 0;
			int32 SplitAt = INDEX_NONE;
			for (int32 i = 0; i < MapInner.Len(); ++i)
			{
				const TCHAR Ch = MapInner[i];
				if (Ch == TEXT('<')) ++Depth;
				else if (Ch == TEXT('>')) --Depth;
				else if (Ch == TEXT(',') && Depth == 0) { SplitAt = i; break; }
			}
			if (SplitAt == INDEX_NONE) return false;
			KeyStr = MapInner.Left(SplitAt).TrimStartAndEnd();
			ValueStr = MapInner.Mid(SplitAt + 1).TrimStartAndEnd();
			bMap = true;
		}
		else if (Type.StartsWith(TEXT("Map of "), ESearchCase::IgnoreCase))
		{
			const FString InnerStr = Type.Mid(7).TrimStartAndEnd();
			const int32 ToIdx = InnerStr.Find(TEXT(" to "), ESearchCase::IgnoreCase);
			if (ToIdx == INDEX_NONE) return false;
			KeyStr = InnerStr.Left(ToIdx).TrimStartAndEnd();
			ValueStr = InnerStr.Mid(ToIdx + 4).TrimStartAndEnd();
			bMap = true;
		}

		if (bMap)
		{
			FEdGraphPinType KeyType, ValueType;
			if (!ParseTypeString(KeyStr, KeyType)) return false;
			if (!ParseTypeString(ValueStr, ValueType)) return false;
			if (KeyType.ContainerType   != EPinContainerType::None) return false;
			if (ValueType.ContainerType != EPinContainerType::None) return false;

			OutPinType = KeyType;
			OutPinType.ContainerType = EPinContainerType::Map;
			OutPinType.PinValueType.TerminalCategory          = ValueType.PinCategory;
			OutPinType.PinValueType.TerminalSubCategory       = ValueType.PinSubCategory;
			OutPinType.PinValueType.TerminalSubCategoryObject = ValueType.PinSubCategoryObject;
			return true;
		}
	}

	// ── Templated reference types — Object<T> / Class<T> / SoftObject<T> /
	//    SoftClass<T> / Interface<T> / Enum<T> ─────────────────────────────
	{
		FString Inner;
		if (TryExtractTemplated(Type, TEXT("Object"), Inner))
		{
			UClass* C = FindClassByName(Inner);
			if (!C) return false;
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Object;
			OutPinType.PinSubCategoryObject = C;
			return true;
		}
		if (TryExtractTemplated(Type, TEXT("Class"), Inner))
		{
			UClass* C = FindClassByName(Inner);
			if (!C) return false;
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Class;
			OutPinType.PinSubCategoryObject = C;
			return true;
		}
		if (TryExtractTemplated(Type, TEXT("SoftObject"), Inner))
		{
			UClass* C = FindClassByName(Inner);
			if (!C) return false;
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_SoftObject;
			OutPinType.PinSubCategoryObject = C;
			return true;
		}
		if (TryExtractTemplated(Type, TEXT("SoftClass"), Inner))
		{
			UClass* C = FindClassByName(Inner);
			if (!C) return false;
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_SoftClass;
			OutPinType.PinSubCategoryObject = C;
			return true;
		}
		if (TryExtractTemplated(Type, TEXT("Interface"), Inner))
		{
			UClass* C = FindClassByName(Inner);
			if (!C || !C->HasAnyClassFlags(CLASS_Interface)) return false;
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Interface;
			OutPinType.PinSubCategoryObject = C;
			return true;
		}
		if (TryExtractTemplated(Type, TEXT("Enum"), Inner))
		{
			UEnum* E = FindEnumByName(Inner);
			if (!E) return false;
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
			OutPinType.PinSubCategoryObject = E;
			return true;
		}
	}

	// ── Atomic primitive / common-struct names ─────────────────────────────
	if (Type.Equals(TEXT("Bool"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("Boolean"), ESearchCase::IgnoreCase))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	}
	else if (Type.Equals(TEXT("Byte"), ESearchCase::IgnoreCase))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
	}
	else if (Type.Equals(TEXT("Int"), ESearchCase::IgnoreCase))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int;
	}
	else if (Type.Equals(TEXT("Int64"), ESearchCase::IgnoreCase))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
	}
	else if (Type.Equals(TEXT("Float"), ESearchCase::IgnoreCase))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
		OutPinType.PinSubCategory = TEXT("float");
	}
	else if (Type.Equals(TEXT("Double"), ESearchCase::IgnoreCase))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
		OutPinType.PinSubCategory = TEXT("double");
	}
	else if (Type.Equals(TEXT("String"), ESearchCase::IgnoreCase))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_String;
	}
	else if (Type.Equals(TEXT("Name"), ESearchCase::IgnoreCase))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Name;
	}
	else if (Type.Equals(TEXT("Text"), ESearchCase::IgnoreCase))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Text;
	}
	else if (Type.Equals(TEXT("Vector"), ESearchCase::IgnoreCase))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = TBaseStructure<FVector>::Get();
	}
	else if (Type.Equals(TEXT("Vector2D"), ESearchCase::IgnoreCase))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = TBaseStructure<FVector2D>::Get();
	}
	else if (Type.Equals(TEXT("Vector4"), ESearchCase::IgnoreCase))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = TBaseStructure<FVector4>::Get();
	}
	else if (Type.Equals(TEXT("IntPoint"), ESearchCase::IgnoreCase))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = TBaseStructure<FIntPoint>::Get();
	}
	else if (Type.Equals(TEXT("IntVector"), ESearchCase::IgnoreCase))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = TBaseStructure<FIntVector>::Get();
	}
	else if (Type.Equals(TEXT("Rotator"), ESearchCase::IgnoreCase))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
	}
	else if (Type.Equals(TEXT("Transform"), ESearchCase::IgnoreCase))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = TBaseStructure<FTransform>::Get();
	}
	else if (Type.Equals(TEXT("Quat"), ESearchCase::IgnoreCase))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = TBaseStructure<FQuat>::Get();
	}
	else if (Type.Equals(TEXT("LinearColor"), ESearchCase::IgnoreCase))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = TBaseStructure<FLinearColor>::Get();
	}
	else if (Type.Equals(TEXT("Color"), ESearchCase::IgnoreCase))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = TBaseStructure<FColor>::Get();
	}
	else if (Type.Equals(TEXT("DateTime"), ESearchCase::IgnoreCase))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = TBaseStructure<FDateTime>::Get();
	}
	else if (Type.Equals(TEXT("Guid"), ESearchCase::IgnoreCase))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = TBaseStructure<FGuid>::Get();
	}
	else if (Type.Equals(TEXT("GameplayTag"), ESearchCase::IgnoreCase))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = FindObject<UScriptStruct>(nullptr, TEXT("/Script/GameplayTags.GameplayTag"));
	}
	else if (Type.Equals(TEXT("GameplayTagContainer"), ESearchCase::IgnoreCase))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = FindObject<UScriptStruct>(nullptr, TEXT("/Script/GameplayTags.GameplayTagContainer"));
	}
	else
	{
		// Bare-name fallback: try UScriptStruct, then UEnum, then UClass.
		// Enum lookup placed before class so that an enum named like a class
		// (rare) resolves to enum semantics first.
		if (UScriptStruct* FoundStruct = FindStructByName(Type))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutPinType.PinSubCategoryObject = FoundStruct;
			return true;
		}
		if (UEnum* FoundEnum = FindEnumByName(Type))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
			OutPinType.PinSubCategoryObject = FoundEnum;
			return true;
		}
		if (UClass* FoundClass = FindClassByName(Type))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Object;
			OutPinType.PinSubCategoryObject = FoundClass;
			return true;
		}
		return false;
	}

	return true;
}


/** Internal helper: serialize a leaf type (no container wrap) used by both
 *  PinTypeToString and the Map value-half. SubCategory is FName because
 *  both FEdGraphPinType::PinSubCategory and FEdGraphTerminalType::TerminalSubCategory
 *  are FName, not FString. */
static FString PinLeafToString(const FName Category, const FName SubCategory, UObject* SubObj)
{
	if (Category == UEdGraphSchema_K2::PC_Boolean)  return TEXT("Bool");
	if (Category == UEdGraphSchema_K2::PC_Byte)
	{
		if (UEnum* E = Cast<UEnum>(SubObj))
		{
			return FString::Printf(TEXT("Enum<%s>"), *E->GetName());
		}
		return TEXT("Byte");
	}
	if (Category == UEdGraphSchema_K2::PC_Int)      return TEXT("Int");
	if (Category == UEdGraphSchema_K2::PC_Int64)    return TEXT("Int64");
	if (Category == UEdGraphSchema_K2::PC_Real)     return (SubCategory == TEXT("float")) ? TEXT("Float") : TEXT("Double");
	if (Category == UEdGraphSchema_K2::PC_String)   return TEXT("String");
	if (Category == UEdGraphSchema_K2::PC_Name)     return TEXT("Name");
	if (Category == UEdGraphSchema_K2::PC_Text)     return TEXT("Text");

	if (Category == UEdGraphSchema_K2::PC_Struct)
	{
		UScriptStruct* SS = Cast<UScriptStruct>(SubObj);
		if (SS == TBaseStructure<FVector>::Get())       return TEXT("Vector");
		if (SS == TBaseStructure<FVector2D>::Get())     return TEXT("Vector2D");
		if (SS == TBaseStructure<FVector4>::Get())      return TEXT("Vector4");
		if (SS == TBaseStructure<FIntPoint>::Get())     return TEXT("IntPoint");
		if (SS == TBaseStructure<FIntVector>::Get())    return TEXT("IntVector");
		if (SS == TBaseStructure<FRotator>::Get())      return TEXT("Rotator");
		if (SS == TBaseStructure<FTransform>::Get())    return TEXT("Transform");
		if (SS == TBaseStructure<FQuat>::Get())         return TEXT("Quat");
		if (SS == TBaseStructure<FLinearColor>::Get())  return TEXT("LinearColor");
		if (SS == TBaseStructure<FColor>::Get())        return TEXT("Color");
		if (SS == TBaseStructure<FDateTime>::Get())     return TEXT("DateTime");
		if (SS == TBaseStructure<FGuid>::Get())         return TEXT("Guid");
		if (SS && SS->GetFName() == FName(TEXT("GameplayTag")))           return TEXT("GameplayTag");
		if (SS && SS->GetFName() == FName(TEXT("GameplayTagContainer")))  return TEXT("GameplayTagContainer");
		return SS ? SS->GetName() : TEXT("Struct");
	}

	auto ClassName = [&]() -> FString {
		if (UClass* C = Cast<UClass>(SubObj)) return C->GetName();
		return TEXT("Object");
	};

	if (Category == UEdGraphSchema_K2::PC_Object)     return FString::Printf(TEXT("Object<%s>"),     *ClassName());
	if (Category == UEdGraphSchema_K2::PC_Class)      return FString::Printf(TEXT("Class<%s>"),      *ClassName());
	if (Category == UEdGraphSchema_K2::PC_SoftObject) return FString::Printf(TEXT("SoftObject<%s>"), *ClassName());
	if (Category == UEdGraphSchema_K2::PC_SoftClass)  return FString::Printf(TEXT("SoftClass<%s>"),  *ClassName());
	if (Category == UEdGraphSchema_K2::PC_Interface)  return FString::Printf(TEXT("Interface<%s>"),  *ClassName());

	return Category.ToString();
}

FString PinTypeToString(const FEdGraphPinType& PinType)
{
	const FString Core = PinLeafToString(
		PinType.PinCategory,
		PinType.PinSubCategory,
		PinType.PinSubCategoryObject.Get());

	switch (PinType.ContainerType)
	{
		case EPinContainerType::Array:
			return FString::Printf(TEXT("Array of %s"), *Core);
		case EPinContainerType::Set:
			return FString::Printf(TEXT("Set of %s"), *Core);
		case EPinContainerType::Map:
		{
			const FString ValueCore = PinLeafToString(
				PinType.PinValueType.TerminalCategory,
				PinType.PinValueType.TerminalSubCategory,
				PinType.PinValueType.TerminalSubCategoryObject.Get());
			return FString::Printf(TEXT("Map<%s, %s>"), *Core, *ValueCore);
		}
		case EPinContainerType::None:
		default:
			return Core;
	}
}

} // namespace BridgeTypeParseImpl
