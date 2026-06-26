// Copyright 2026 Betide Studio. All Rights Reserved.
//
// Templated parse/format helpers for any UENUM, backed by StaticEnum<T>() reflection.
//
// Replaces hand-rolled `if (S == TEXT("X")) return EFoo::X; ...` ladders that go stale
// every time the engine adds an enum value. With these helpers, adding a new UENUM
// value in the engine is automatically picked up by every binding that uses Parse<T>
// — no binding changes required.
//
// Behaviour:
//   - Case-insensitive matching against authored names + UMETA(DisplayName) aliases.
//   - Optional caller-supplied alias map for binding-side aliases not in the engine
//     (e.g. "space" -> ERigElementType::Null, "int" -> ERigControlType::Integer).
//   - ToString() returns the short name (without the "EFoo::" qualifier).
//   - GetValueNames() / ForEachValue() iterate non-hidden values for help text and
//     error messages.
//
// Usage:
//   const TMap<FString, ERigElementType> Aliases = { { TEXT("space"), ERigElementType::Null } };
//   if (TOptional<ERigElementType> T = NeoLuaEnum::Parse<ERigElementType>(InStr, &Aliases))
//   {
//       UseType(*T);
//   }
//
//   FString Display = NeoLuaEnum::ToString(ERigElementType::Bone); // "Bone"
//   TArray<FString> Names = NeoLuaEnum::GetValueNames<ERigMetadataType>(); // for error messages

#pragma once

#include "CoreMinimal.h"
#include "UObject/Class.h"
#include "Misc/Optional.h"

namespace NeoLuaEnum
{

namespace Detail
{
	// Strip the "EFoo::" prefix from a fully-qualified UENUM name.
	// UEnum::GetNameStringByValue returns the unqualified name in 5.x for `enum class`
	// types, but historically has returned the qualified form for legacy enums; this
	// guard covers both.
	inline FString ShortName(const FString& In)
	{
		int32 Idx;
		if (In.FindLastChar(TEXT(':'), Idx)) return In.Mid(Idx + 1);
		return In;
	}

	// Resolve TEnum -> UEnum* once per process per type.
	template <typename TEnum>
	UEnum* GetUEnum()
	{
		static UEnum* Cached = StaticEnum<TEnum>();
		return Cached;
	}
}

// Try to parse a UENUM value from a string.
// - Case-insensitive.
// - Accepts authored names AND UMETA(DisplayName) values (via EGetByNameFlags::CheckAuthoredName).
// - Optional Aliases map applied first (binding-side shortcuts).
// - Returns Unset if no match.
template <typename TEnum>
TOptional<TEnum> Parse(const FString& In, const TMap<FString, TEnum>* Aliases = nullptr)
{
	if (In.IsEmpty()) return TOptional<TEnum>();

	if (Aliases)
	{
		for (const TPair<FString, TEnum>& P : *Aliases)
		{
			if (P.Key.Equals(In, ESearchCase::IgnoreCase))
			{
				return P.Value;
			}
		}
	}

	UEnum* E = Detail::GetUEnum<TEnum>();
	if (!E) return TOptional<TEnum>();

	// Engine fast path: exact name (handles authored DisplayName aliases).
	const int64 Direct = E->GetValueByNameString(In, EGetByNameFlags::CheckAuthoredName);
	if (Direct != INDEX_NONE)
	{
		return static_cast<TEnum>(Direct);
	}

	// Case-insensitive scan over authored names + DisplayName metadata.
	const int32 Num = E->NumEnums();
	for (int32 i = 0; i < Num - 1; ++i) // last entry is _MAX
	{
		const FString AuthoredShort = Detail::ShortName(E->GetNameStringByIndex(i));
		if (AuthoredShort.Equals(In, ESearchCase::IgnoreCase))
		{
			return static_cast<TEnum>(E->GetValueByIndex(i));
		}
		const FString Display = E->GetMetaData(TEXT("DisplayName"), i);
		if (!Display.IsEmpty() && Display.Equals(In, ESearchCase::IgnoreCase))
		{
			return static_cast<TEnum>(E->GetValueByIndex(i));
		}
	}

	return TOptional<TEnum>();
}

// Same as Parse, but returns Fallback when the input is empty or doesn't match.
template <typename TEnum>
TEnum ParseOr(const FString& In, TEnum Fallback, const TMap<FString, TEnum>* Aliases = nullptr)
{
	TOptional<TEnum> Result = Parse<TEnum>(In, Aliases);
	return Result.IsSet() ? *Result : Fallback;
}

// Format a UENUM value as its short authored name (e.g. "Bone").
// Falls back to the integer string if the value is unknown to the registry.
template <typename TEnum>
FString ToString(TEnum Value)
{
	UEnum* E = Detail::GetUEnum<TEnum>();
	if (!E) return FString::FromInt(static_cast<int64>(Value));
	const FString Full = E->GetNameStringByValue(static_cast<int64>(Value));
	if (Full.IsEmpty()) return FString::FromInt(static_cast<int64>(Value));
	return Detail::ShortName(Full);
}

// Returns the non-hidden, non-_MAX value names of TEnum.
// Useful for "valid values: …" error messages and `list("element_types")` style help.
template <typename TEnum>
TArray<FString> GetValueNames()
{
	TArray<FString> Out;
	UEnum* E = Detail::GetUEnum<TEnum>();
	if (!E) return Out;

	const int32 Num = E->NumEnums();
	for (int32 i = 0; i < Num - 1; ++i)
	{
		if (E->HasMetaData(TEXT("Hidden"), i)) continue;
		Out.Add(Detail::ShortName(E->GetNameStringByIndex(i)));
	}
	return Out;
}

// Iterate the non-hidden values of TEnum.
// Visit signature: void(TEnum Value, const FString& ShortName).
template <typename TEnum, typename Fn>
void ForEachValue(Fn&& Visit)
{
	UEnum* E = Detail::GetUEnum<TEnum>();
	if (!E) return;

	const int32 Num = E->NumEnums();
	for (int32 i = 0; i < Num - 1; ++i)
	{
		if (E->HasMetaData(TEXT("Hidden"), i)) continue;
		const TEnum V = static_cast<TEnum>(E->GetValueByIndex(i));
		Visit(V, Detail::ShortName(E->GetNameStringByIndex(i)));
	}
}

// Convenience: comma-joined list of valid value names (for one-line error reporting).
template <typename TEnum>
FString JoinValueNames(const TCHAR* Separator = TEXT(", "))
{
	const TArray<FString> Names = GetValueNames<TEnum>();
	return FString::Join(Names, Separator);
}

// Runtime UEnum* variant of Parse<T>, for callers that only have a UEnum* at runtime
// (FEnumProperty::GetEnum(), FByteProperty::GetIntPropertyEnum(), arbitrary reflection).
//   - Passes EGetByNameFlags::CheckAuthoredName so UMETA(DisplayName) aliases hit the
//     engine's fast path.
//   - Case-insensitive linear-scan fallback also catches DisplayName aliases AND
//     correctly resolves enumerators declared with value == -1 (which the naive
//     `Value != INDEX_NONE` check falsely rejects).
//   - Accepts authored short name ("X"), qualified form ("EFoo::X"), and DisplayName.
//   - Skips UMETA(Hidden) enumerators.
// Returns false if the input is empty or doesn't match any value.
inline bool ParseRuntime(UEnum* Enum, const FString& In, int64& OutValue)
{
	if (!Enum || In.IsEmpty()) return false;

	// Fast path: name-based lookup with authored-name flag so DisplayName aliases resolve.
	const int64 Direct = Enum->GetValueByNameString(In, EGetByNameFlags::CheckAuthoredName);
	if (Direct != INDEX_NONE)
	{
		OutValue = Direct;
		return true;
	}

	// Qualified form: "EFoo::X"
	const int64 Qualified = Enum->GetValueByNameString(Enum->GetName() + TEXT("::") + In, EGetByNameFlags::CheckAuthoredName);
	if (Qualified != INDEX_NONE)
	{
		OutValue = Qualified;
		return true;
	}

	// Case-insensitive linear scan. Index-based so a legitimate value of -1 resolves
	// correctly (the INDEX_NONE sentinel above can't distinguish "not found" from "found
	// a value == -1"). Also applies the UMETA(Hidden) filter.
	const int32 Num = Enum->NumEnums();
	for (int32 Index = 0; Index < Num; ++Index)
	{
		if (Enum->HasMetaData(TEXT("Hidden"), Index)) continue;

		const FString AuthoredShort = Detail::ShortName(Enum->GetNameStringByIndex(Index));
		if (AuthoredShort.Equals(In, ESearchCase::IgnoreCase))
		{
			OutValue = Enum->GetValueByIndex(Index);
			return true;
		}
		if (Enum->GetAuthoredNameStringByIndex(Index).Equals(In, ESearchCase::IgnoreCase))
		{
			OutValue = Enum->GetValueByIndex(Index);
			return true;
		}
		const FString Display = Enum->GetMetaData(TEXT("DisplayName"), Index);
		if (!Display.IsEmpty() && Display.Equals(In, ESearchCase::IgnoreCase))
		{
			OutValue = Enum->GetValueByIndex(Index);
			return true;
		}
	}

	return false;
}

} // namespace NeoLuaEnum
