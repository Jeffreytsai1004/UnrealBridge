// Copyright UnrealBridge. Shared type-string ↔ FEdGraphPinType helpers.
//
// Used by UnrealBridgeBlueprintLibrary (variable / parameter / member type
// CRUD) and UnrealBridgeStructLibrary (UserDefinedStruct field CRUD). The
// "type_string" surface is the same on both — keeping the parser in one
// place means adding a new type form (e.g. Set / Map / Enum) automatically
// extends every bridge surface that accepts type_string.
#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphPin.h"

namespace BridgeTypeParseImpl
{
	/** Parse a human-friendly type description (case-insensitive) into an
	 *  FEdGraphPinType. Supported forms:
	 *      Bool / Boolean / Byte / Int / Int64 / Float / Double
	 *      String / Name / Text
	 *      Vector / Rotator / Transform / LinearColor / GameplayTag
	 *      <any UScriptStruct simple name in CoreUObject or Engine>
	 *      <any UClass simple name in CoreUObject or Engine>
	 *      "Array of <type>" — wraps the above as a TArray pin
	 *
	 *  Returns false if no form matched. */
	bool ParseTypeString(const FString& TypeStr, FEdGraphPinType& OutPinType);

	/** Inverse: serialize an FEdGraphPinType back to a string that
	 *  ParseTypeString can round-trip. For the canonical type set above
	 *  the round-trip is exact; for unrecognised pin categories the raw
	 *  category name is returned. */
	FString PinTypeToString(const FEdGraphPinType& PinType);
}
