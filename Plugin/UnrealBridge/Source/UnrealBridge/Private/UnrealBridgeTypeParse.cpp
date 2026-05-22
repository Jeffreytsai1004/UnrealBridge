#include "UnrealBridgeTypeParse.h"

#include "EdGraphSchema_K2.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/Package.h"

namespace BridgeTypeParseImpl
{

bool ParseTypeString(const FString& TypeStr, FEdGraphPinType& OutPinType)
{
	OutPinType = FEdGraphPinType();

	FString Type = TypeStr.TrimStartAndEnd();

	// Array prefix
	static const FString ArrayPrefix = TEXT("Array of ");
	if (Type.StartsWith(ArrayPrefix))
	{
		Type = Type.Mid(ArrayPrefix.Len());
		OutPinType.ContainerType = EPinContainerType::Array;
	}

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
	else if (Type.Equals(TEXT("LinearColor"), ESearchCase::IgnoreCase))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = TBaseStructure<FLinearColor>::Get();
	}
	else if (Type.Equals(TEXT("GameplayTag"), ESearchCase::IgnoreCase))
	{
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
		OutPinType.PinSubCategoryObject = FindObject<UScriptStruct>(nullptr, TEXT("/Script/GameplayTags.GameplayTag"));
	}
	else
	{
		// Try as struct
		UScriptStruct* FoundStruct = FindObject<UScriptStruct>(nullptr, *Type);
		if (!FoundStruct)
			FoundStruct = FindObject<UScriptStruct>(nullptr, *(FString(TEXT("/Script/CoreUObject.")) + Type));
		if (!FoundStruct)
			FoundStruct = FindObject<UScriptStruct>(nullptr, *(FString(TEXT("/Script/Engine.")) + Type));

		if (FoundStruct)
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutPinType.PinSubCategoryObject = FoundStruct;
			return true;
		}

		// Try as class (object reference)
		UClass* FoundClass = FindObject<UClass>(nullptr, *Type);
		if (!FoundClass)
			FoundClass = FindObject<UClass>(nullptr, *(FString(TEXT("/Script/Engine.")) + Type));
		if (!FoundClass)
			FoundClass = FindObject<UClass>(nullptr, *(FString(TEXT("/Script/CoreUObject.")) + Type));

		if (FoundClass)
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Object;
			OutPinType.PinSubCategoryObject = FoundClass;
			return true;
		}

		return false;
	}

	return true;
}

FString PinTypeToString(const FEdGraphPinType& PinType)
{
	FString Core;

	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
	{
		Core = TEXT("Bool");
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Byte)
	{
		Core = TEXT("Byte");
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Int)
	{
		Core = TEXT("Int");
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Int64)
	{
		Core = TEXT("Int64");
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Real)
	{
		Core = (PinType.PinSubCategory == TEXT("float")) ? TEXT("Float") : TEXT("Double");
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_String)
	{
		Core = TEXT("String");
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Name)
	{
		Core = TEXT("Name");
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Text)
	{
		Core = TEXT("Text");
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Struct && PinType.PinSubCategoryObject.IsValid())
	{
		// Prefer the friendly aliases the parser already understands; fall back
		// to the bare struct name otherwise. Reflexive: PinTypeToString returns
		// "Vector" for FVector → ParseTypeString accepts "Vector" → produces the
		// same FEdGraphPinType.
		UScriptStruct* SS = Cast<UScriptStruct>(PinType.PinSubCategoryObject.Get());
		if (SS == TBaseStructure<FVector>::Get())            Core = TEXT("Vector");
		else if (SS == TBaseStructure<FRotator>::Get())      Core = TEXT("Rotator");
		else if (SS == TBaseStructure<FTransform>::Get())    Core = TEXT("Transform");
		else if (SS == TBaseStructure<FLinearColor>::Get())  Core = TEXT("LinearColor");
		else if (SS && SS->GetFName() == FName(TEXT("GameplayTag")))
		{
			Core = TEXT("GameplayTag");
		}
		else if (SS)
		{
			Core = SS->GetName();
		}
		else
		{
			Core = TEXT("Struct");
		}
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Object && PinType.PinSubCategoryObject.IsValid())
	{
		if (UClass* C = Cast<UClass>(PinType.PinSubCategoryObject.Get()))
		{
			Core = C->GetName();
		}
		else
		{
			Core = TEXT("Object");
		}
	}
	else
	{
		// Unrecognised category — return the raw FName.
		Core = PinType.PinCategory.ToString();
	}

	if (PinType.ContainerType == EPinContainerType::Array)
	{
		return FString::Printf(TEXT("Array of %s"), *Core);
	}
	return Core;
}

} // namespace BridgeTypeParseImpl
