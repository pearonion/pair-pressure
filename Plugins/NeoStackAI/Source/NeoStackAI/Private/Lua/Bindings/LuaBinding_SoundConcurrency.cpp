// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Lua/LuaAssetResolver.h"
#include "Lua/LuaStr.h"

#include "Sound/SoundConcurrency.h"
#include "UObject/UnrealType.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// HELPERS
// ============================================================================

static const char* ResolutionRuleToString(EMaxConcurrentResolutionRule::Type Rule)
{
	switch (Rule)
	{
	case EMaxConcurrentResolutionRule::PreventNew:                     return "prevent_new";
	case EMaxConcurrentResolutionRule::StopOldest:                     return "stop_oldest";
	case EMaxConcurrentResolutionRule::StopFarthestThenPreventNew:     return "stop_farthest_then_prevent_new";
	case EMaxConcurrentResolutionRule::StopFarthestThenOldest:         return "stop_farthest_then_oldest";
	case EMaxConcurrentResolutionRule::StopLowestPriority:             return "stop_lowest_priority";
	case EMaxConcurrentResolutionRule::StopQuietest:                   return "stop_quietest";
	case EMaxConcurrentResolutionRule::StopLowestPriorityThenPreventNew: return "stop_lowest_priority_then_prevent_new";
	default:                                                           return "unknown";
	}
}

static const char* VolumeScaleModeToString(EConcurrencyVolumeScaleMode Mode)
{
	switch (Mode)
	{
	case EConcurrencyVolumeScaleMode::Default:  return "default";
	case EConcurrencyVolumeScaleMode::Distance: return "distance";
	case EConcurrencyVolumeScaleMode::Priority: return "priority";
	default:                                    return "default";
	}
}

static const FPerPlatformInt* GetPlatformMaxCount(const FSoundConcurrencySettings& Settings)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7 && WITH_EDITOR
	const FProperty* Property = FSoundConcurrencySettings::StaticStruct()->FindPropertyByName(TEXT("PlatformMaxCount"));
	const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
	if (!StructProperty || StructProperty->Struct != FPerPlatformInt::StaticStruct())
	{
		return nullptr;
	}
	return StructProperty->ContainerPtrToValuePtr<FPerPlatformInt>(&Settings);
#else
	return nullptr;
#endif
}

static bool ReadPositiveInt(sol::table Params, const char* Key, int32& OutValue, FString& OutError)
{
	sol::optional<int> Value = Params.get<sol::optional<int>>(Key);
	if (!Value.has_value())
	{
		return false;
	}
	if (Value.value() <= 0)
	{
		OutError = FString::Printf(TEXT("'%s' must be greater than 0"), UTF8_TO_TCHAR(Key));
		return false;
	}
	OutValue = Value.value();
	return true;
}

static bool BuildPlatformMaxCount(sol::table Params, FPerPlatformInt& OutValue, FString& OutError)
{
	int32 DefaultValue = 0;
	if (!ReadPositiveInt(Params, "value", DefaultValue, OutError) &&
		!ReadPositiveInt(Params, "default", DefaultValue, OutError) &&
		!ReadPositiveInt(Params, "max_count", DefaultValue, OutError))
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("specify positive 'value', 'default', or 'max_count'");
		}
		return false;
	}

	OutValue = FPerPlatformInt(DefaultValue);
	sol::optional<sol::table> Platforms = Params.get<sol::optional<sol::table>>("platforms");
	if (Platforms.has_value())
	{
#if WITH_EDITORONLY_DATA
		for (const auto& Pair : Platforms.value())
		{
			if (!Pair.first.is<std::string>() || !Pair.second.is<int>())
			{
				OutError = TEXT("'platforms' must map platform names to positive integer max counts");
				return false;
			}

			const int32 PlatformValue = Pair.second.as<int>();
			if (PlatformValue <= 0)
			{
				OutError = TEXT("'platforms' values must be greater than 0");
				return false;
			}

			const std::string PlatformName = Pair.first.as<std::string>();
			OutValue.PerPlatform.Add(FName(UTF8_TO_TCHAR(PlatformName.c_str())), PlatformValue);
		}
#else
		OutError = TEXT("per-platform overrides require WITH_EDITORONLY_DATA");
		return false;
#endif
	}

	return true;
}
// ============================================================================
// BINDING
// ============================================================================

static TArray<FLuaFunctionDoc> SoundConcurrencyDocs = {};

static void BindSoundConcurrency(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_sound_concurrency", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = NeoLuaStr::ToFString(PathStr);
		USoundConcurrency* Asset = NeoLuaAsset::Resolve<USoundConcurrency>(FPath);
		if (!Asset) return;

		// ---- help text ----
		AssetObj["_help_text"] =
				"SoundConcurrency enrichment methods:\n"
				"\n"
				"info() — structured summary:\n"
				"  max_count, dynamic_max_count, enable_platform_scaling,\n"
				"  platform_max_count, platform_max_count_default,\n"
				"  platform_max_count_override_count, platform_max_counts,\n"
				"  resolution_rule, retrigger_time,\n"
				"  limit_to_owner, volume_scale, volume_scale_mode, volume_scale_attack_time,\n"
				"  volume_scale_can_release, volume_scale_release_time,\n"
				"  voice_steal_release_time, is_eviction_supported\n"
				"\n"
				"configure(type, params):\n"
				"  configure(\"max_count\", {value=8, enable_platform_scaling=false})\n"
				"  configure(\"max_count\", {value=8, enable_platform_scaling=true, platforms={Windows=8}})\n"
				"  configure(\"enable_platform_scaling\", {enabled=true})\n"
				"  configure(\"platform_max_count\", {default=8, platforms={Windows=8}})\n"
				"\n"
				"Property editing uses the generic asset reflection API:\n"
				"  get(\"PropertyName\")\n"
				"  set(\"PropertyName\", \"Value\")\n"
			"  list_properties(filter?, all?)\n"
				"\n"
				"Use raw engine property names on the Concurrency struct, e.g.:\n"
				"  Concurrency.MaxCount\n"
				"  Concurrency.ResolutionRule\n"
				"  Concurrency.RetriggerTime\n"
			"  Concurrency.bLimitToOwner\n"
			"  Concurrency.VolumeScale\n"
			"  Concurrency.VolumeScaleMode\n"
			"  Concurrency.VolumeScaleAttackTime\n"
			"  Concurrency.bVolumeScaleCanRelease\n"
			"  Concurrency.VolumeScaleReleaseTime\n"
			"  Concurrency.VoiceStealReleaseTime\n";

		// ================================================================
		// info()
		// ================================================================
		AssetObj.set_function("info", [Asset, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(Asset))
			{
				Session.Log(TEXT("[FAIL] info -> asset no longer valid"));
				return sol::lua_nil;
			}

			const FSoundConcurrencySettings& C = Asset->Concurrency;
			sol::table R = Lua.create_table();

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7 && WITH_EDITOR
				R["max_count"] = C.GetMaxCount();
				R["dynamic_max_count"] = C.MaxCount;
				R["enable_platform_scaling"] = C.IsMaxCountPlatformScalingEnabled();
				if (const FPerPlatformInt* PlatformMaxCount = GetPlatformMaxCount(C))
				{
					R["platform_max_count"] = PlatformMaxCount->GetValue();
					R["platform_max_count_default"] = PlatformMaxCount->Default;
					sol::table PlatformCounts = Lua.create_table();
#if WITH_EDITORONLY_DATA
					R["platform_max_count_override_count"] = PlatformMaxCount->PerPlatform.Num();
					int32 OutIndex = 1;
					for (const TPair<FName, int32>& Pair : PlatformMaxCount->PerPlatform)
					{
						sol::table Entry = Lua.create_table();
						Entry["platform"] = std::string(TCHAR_TO_UTF8(*Pair.Key.ToString()));
						Entry["max_count"] = Pair.Value;
						PlatformCounts[OutIndex++] = Entry;
					}
#else
					R["platform_max_count_override_count"] = 0;
#endif
					R["platform_max_counts"] = PlatformCounts;
				}
				else
				{
					R["platform_max_count"] = C.MaxCount;
					R["platform_max_count_default"] = C.MaxCount;
					R["platform_max_count_override_count"] = 0;
					R["platform_max_counts"] = Lua.create_table();
				}
#else
				R["max_count"] = C.MaxCount;
				R["dynamic_max_count"] = C.MaxCount;
				R["enable_platform_scaling"] = false;
				R["platform_max_count"] = C.MaxCount;
				R["platform_max_count_default"] = C.MaxCount;
				R["platform_max_count_override_count"] = 0;
				R["platform_max_counts"] = Lua.create_table();
#endif
			R["resolution_rule"] = ResolutionRuleToString(C.ResolutionRule);
			R["retrigger_time"] = C.RetriggerTime;
			R["limit_to_owner"] = static_cast<bool>(C.bLimitToOwner);
			R["volume_scale"] = C.GetVolumeScale();
			R["volume_scale_mode"] = VolumeScaleModeToString(C.VolumeScaleMode);
			R["volume_scale_attack_time"] = C.VolumeScaleAttackTime;
			R["volume_scale_can_release"] = static_cast<bool>(C.bVolumeScaleCanRelease);
			R["volume_scale_release_time"] = C.VolumeScaleReleaseTime;
			R["voice_steal_release_time"] = C.VoiceStealReleaseTime;
			R["is_eviction_supported"] = C.IsEvictionSupported();

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			Session.Log(FString::Printf(TEXT("[OK] info() -> SoundConcurrency, max_count=%d, rule=%s"),
				C.GetMaxCount(),
				UTF8_TO_TCHAR(ResolutionRuleToString(C.ResolutionRule))));
#else
			Session.Log(FString::Printf(TEXT("[OK] info() -> SoundConcurrency, max_count=%d, rule=%s"),
				C.MaxCount,
				UTF8_TO_TCHAR(ResolutionRuleToString(C.ResolutionRule))));
#endif
				return R;
			});

			// ================================================================
			// configure(type, params)
			// ================================================================
			AssetObj.set_function("configure", [Asset, &Session](sol::table /*self*/,
				std::string Type, sol::table Params, sol::this_state S) -> sol::object
			{
				sol::state_view Lua(S);

				if (!IsValid(Asset))
				{
					Session.Log(TEXT("[FAIL] configure -> asset no longer valid"));
					return sol::lua_nil;
				}

				const FString FType = NeoLuaStr::ToFString(Type);

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7 && WITH_EDITOR
				if (Type == "max_count")
				{
					sol::optional<bool> EnableScaling = Params.get<sol::optional<bool>>("enable_platform_scaling");
					if (EnableScaling.has_value() && EnableScaling.value())
					{
						FString Error;
						FPerPlatformInt PlatformValue;
						if (!BuildPlatformMaxCount(Params, PlatformValue, Error))
						{
							Session.Log(FString::Printf(TEXT("[FAIL] configure(\"max_count\") -> %s"), *Error));
							return sol::lua_nil;
						}

						Asset->Modify();
						if (!Asset->Concurrency.SetPlatformMaxCount(PlatformValue))
						{
							Session.Log(TEXT("[FAIL] configure(\"max_count\") -> SetPlatformMaxCount rejected value"));
							return sol::lua_nil;
						}
						Asset->Concurrency.SetEnableMaxCountPlatformScaling(true);
						Asset->PostEditChange();
						Asset->MarkPackageDirty();
						Session.Log(FString::Printf(TEXT("[OK] configure(\"max_count\") -> platform scaled max_count=%d"),
							Asset->Concurrency.GetMaxCount()));
						return sol::make_object(Lua, true);
					}

					FString Error;
					int32 MaxCount = 0;
					if (!ReadPositiveInt(Params, "value", MaxCount, Error) &&
						!ReadPositiveInt(Params, "max_count", MaxCount, Error))
					{
						if (Error.IsEmpty())
						{
							Error = TEXT("specify positive 'value' or 'max_count'");
						}
						Session.Log(FString::Printf(TEXT("[FAIL] configure(\"max_count\") -> %s"), *Error));
						return sol::lua_nil;
					}

					Asset->Modify();
					Asset->Concurrency.SetEnableMaxCountPlatformScaling(false);
					if (!Asset->Concurrency.SetMaxCount(MaxCount))
					{
						Session.Log(TEXT("[FAIL] configure(\"max_count\") -> SetMaxCount rejected value"));
						return sol::lua_nil;
					}
					Asset->PostEditChange();
					Asset->MarkPackageDirty();
					Session.Log(FString::Printf(TEXT("[OK] configure(\"max_count\") -> dynamic max_count=%d"), MaxCount));
					return sol::make_object(Lua, true);
				}

				if (Type == "enable_platform_scaling")
				{
					sol::optional<bool> Enabled = Params.get<sol::optional<bool>>("enabled");
					if (!Enabled.has_value())
					{
						Session.Log(TEXT("[FAIL] configure(\"enable_platform_scaling\") -> {enabled=true|false} required"));
						return sol::lua_nil;
					}

					Asset->Modify();
					Asset->Concurrency.SetEnableMaxCountPlatformScaling(Enabled.value());
					Asset->PostEditChange();
					Asset->MarkPackageDirty();
					Session.Log(FString::Printf(TEXT("[OK] configure(\"enable_platform_scaling\") -> %s"),
						Enabled.value() ? TEXT("true") : TEXT("false")));
					return sol::make_object(Lua, true);
				}

				if (Type == "platform_max_count")
				{
					FString Error;
					FPerPlatformInt PlatformValue;
					if (!BuildPlatformMaxCount(Params, PlatformValue, Error))
					{
						Session.Log(FString::Printf(TEXT("[FAIL] configure(\"platform_max_count\") -> %s"), *Error));
						return sol::lua_nil;
					}

					Asset->Modify();
					if (!Asset->Concurrency.SetPlatformMaxCount(PlatformValue))
					{
						Session.Log(TEXT("[FAIL] configure(\"platform_max_count\") -> SetPlatformMaxCount rejected value"));
						return sol::lua_nil;
					}
					Asset->PostEditChange();
					Asset->MarkPackageDirty();
					Session.Log(TEXT("[OK] configure(\"platform_max_count\")"));
					return sol::make_object(Lua, true);
				}
#else
				if (Type == "max_count" || Type == "enable_platform_scaling" || Type == "platform_max_count")
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> UE 5.7 editor-only SoundConcurrency max-count APIs are unavailable"), *FType));
					return sol::lua_nil;
				}
#endif

				Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown type. Valid: max_count, enable_platform_scaling, platform_max_count"), *FType));
				return sol::lua_nil;
			});
		});
	}

REGISTER_LUA_BINDING(SoundConcurrency, SoundConcurrencyDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindSoundConcurrency(Lua, Session);
});
