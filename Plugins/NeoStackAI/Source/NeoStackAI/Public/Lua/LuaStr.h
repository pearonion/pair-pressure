// Copyright 2025-2026 Betide Studio. All Rights Reserved.
//
// Sol2 ↔ FString conversion helpers. Replaces the open-coded
//   FString X = UTF8_TO_TCHAR(StdStr.c_str());
//   FString Y = Opt.has_value() ? UTF8_TO_TCHAR(Opt.value().c_str()) : TEXT("");
// pattern that appears 250+ times across bindings.
//
// Usage:
//   const FString Name        = NeoLuaStr::ToFString(NameStdStr);
//   const FString FilterText  = NeoLuaStr::ToFStringOpt(FilterOpt);
//   const FString Mode        = NeoLuaStr::ToFStringOpt(ModeOpt, TEXT("default"));
//   const std::string PathOut = NeoLuaStr::ToStdString(SomeFString);

#pragma once

#include "CoreMinimal.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

namespace NeoLuaStr
{

// std::string → FString (UTF-8 decoded)
inline FString ToFString(const std::string& S)
{
	return FString(UTF8_TO_TCHAR(S.c_str()));
}

// FString → std::string (UTF-8 encoded). Useful for sol::make_object(L, std::string(...))
inline std::string ToStdString(const FString& S)
{
	return std::string(TCHAR_TO_UTF8(*S));
}

// sol::optional<std::string> → FString, with default for missing/empty
inline FString ToFStringOpt(const sol::optional<std::string>& Opt, const FString& Default = FString())
{
	return Opt.has_value() ? FString(UTF8_TO_TCHAR(Opt.value().c_str())) : Default;
}

// Direct char* → FString (rare but happens with sol::table.get<const char*>)
inline FString ToFString(const char* S)
{
	return S ? FString(UTF8_TO_TCHAR(S)) : FString();
}

}
