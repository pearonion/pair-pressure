// Copyright 2026 Betide Studio. All Rights Reserved.
//
// Shared UE type-name parser. Turns human/agent-authored type strings like
// "Float", "Actor", "EFoo", "FDataTableRowHandle", "Array<Int>",
// "Map<String,Struct:FGameplayTag>", "/Game/Types/MyEnum.MyEnum" into a
// fully-populated FEdGraphPinType.
//
// This is the single source of truth for type resolution across the plugin —
// Blueprint variables, UserDefinedStruct fields, Blackboard keys, function
// params/returns. Every binding that takes a "type" argument should route
// through here instead of rolling its own mini-parser (those invariably miss
// struct resolution, F/U/A/E-prefix fallbacks, and path-based user types).

#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphPin.h"

namespace NeoTypeResolver
{
	// Class / enum / struct reflection lookups with prefix-dance fallback
	// (U/A/E/F) and loaded-object scan. Returns nullptr when the name can't
	// be resolved to a compiled UE type.
	UScriptStruct* FindStructRobust(const FString& StructName);
	UClass*        FindClassRobust(const FString& ClassName);
	UEnum*         FindEnumRobust(const FString& EnumName);

	// Full type-string parser. Returns false on unknown types (so callers
	// can report a real error instead of silently coercing to String).
	// Supports:
	//   - primitives: bool/int/int64/float/double/string/name/text/byte
	//   - containers: "T[]", "array:T", "set:T", "map:K:V"
	//   - explicit prefixes: "class:X", "soft:X", "softclass:X", "interface:X",
	//                        "enum:X", "struct:X"
	//   - struct aliases: vector, rotator, transform, gameplaytag, datatable,
	//                     datatablerowhandle, hitresult, etc.
	//   - reflection fallback for any discoverable UClass / UScriptStruct / UEnum
	//   - asset path fallback: "/Game/X/MyEnum.MyEnum" for user-defined types
	bool ResolveTypeString(const FString& VarType, FEdGraphPinType& OutPinType);
}
