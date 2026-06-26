// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Lua/LuaAssetResolver.h"
#include "Lua/LuaStr.h"
#include "Tools/NeoStackToolUtils.h"
#include "ScopedTransaction.h"

#include "Sound/SoundMix.h"
#include "Sound/SoundClass.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// BINDING
// ============================================================================

static TArray<FLuaFunctionDoc> SoundMixDocs = {};

namespace
{
#if WITH_EDITOR
// USoundMix::CausesPassiveDependencyLoop / CheckForDependencyLoop are declared but not
// DLL-exported by the Engine module, so a plugin cannot link against them. Replicate their
// logic against the public UPROPERTY fields (SoundClassEffects, PassiveSoundMixModifiers,
// ChildClasses), mirroring the engine impl in SoundMix.cpp.
static bool SoundMixCheckForDependencyLoop(const USoundMix* Mix, USoundClass* SoundClass, TArray<USoundClass*>& ProblemClasses, bool bCheckChildren)
{
	if (!SoundClass) return false;
	bool bFoundProblemClass = false;

	// circular references to passive sound mixes that could deactivate the mix
	for (const FPassiveSoundMixModifier& Modifier : SoundClass->PassiveSoundMixModifiers)
	{
		if (Modifier.SoundMix == Mix && Modifier.MinVolumeThreshold > 0.f && Modifier.MaxVolumeThreshold < 10.f)
		{
			ProblemClasses.AddUnique(SoundClass);
			bFoundProblemClass = true;
		}
	}

	if (bCheckChildren)
	{
		for (USoundClass* Child : SoundClass->ChildClasses)
		{
			if (Child && SoundMixCheckForDependencyLoop(Mix, Child, ProblemClasses, bCheckChildren))
			{
				bFoundProblemClass = true;
			}
		}
	}
	return bFoundProblemClass;
}
#endif // WITH_EDITOR

static bool GetSoundMixPassiveDependencyLoop(USoundMix* Mix, TArray<USoundClass*>& OutProblemClasses)
{
	OutProblemClasses.Reset();
#if WITH_EDITOR
	if (!Mix) return false;
	for (const FSoundClassAdjuster& Adjuster : Mix->SoundClassEffects)
	{
		// dependency loops only matter when volume is reduced (can deactivate the mix)
		if (Adjuster.SoundClassObject && Adjuster.VolumeAdjuster < 1.0f)
		{
			return SoundMixCheckForDependencyLoop(Mix, Adjuster.SoundClassObject, OutProblemClasses, Adjuster.bApplyToChildren);
		}
	}
	return false;
#else
	(void)Mix;
	return false;
#endif
}
} // namespace

static void BindSoundMix(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_sound_mix", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = NeoLuaStr::ToFString(PathStr);
		USoundMix* Mix = NeoLuaAsset::Resolve<USoundMix>(FPath);
		if (!Mix) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"SoundMix enrichment methods:\n"
			"\n"
			"info() — structured summary:\n"
			"  initial_delay, fade_in_time, duration, fade_out_time,\n"
			"  apply_eq, eq_priority, eq_settings (bands 0-3),\n"
			"  effect_count, effects (array), has_dependency_loop,\n"
			"  dependency_loop_problem_class_count, dependency_loop_problem_classes\n"
			"\n"
			"list('effect') — list all sound class adjusters:\n"
			"  returns array of {sound_class, volume, pitch, low_pass_filter_frequency,\n"
			"    apply_to_children, voice_center_channel_volume}\n"
			"\n"
			"add('effect', {sound_class='/path/to/SoundClass', volume=1.0, pitch=1.0,\n"
			"  low_pass_filter_frequency=20000, apply_to_children=false,\n"
			"  voice_center_channel_volume=1.0})\n"
			"\n"
			"remove('effect', {sound_class='/path/to/SoundClass'}) — remove by sound class\n"
			"remove('effect', {index=1}) — remove by 1-based index\n"
			"\n"
			"Mix-level properties (InitialDelay, FadeInTime, Duration, FadeOutTime, bApplyEQ,\n"
			"EQPriority, EQSettings.FrequencyCenter0..3, EQSettings.Gain0..3,\n"
			"EQSettings.Bandwidth0..3) go through the generic asset:set(\"PropName\", value)\n"
			"path. Call list_properties() to discover authored names.\n"
			"\n"
			"configure('effect', {index=1, volume=0.5, pitch=1.0,\n"
			"  low_pass_filter_frequency=20000, apply_to_children=false,\n"
			"  voice_center_channel_volume=1.0, sound_class='/path'}) — modify existing adjuster\n";

		// ================================================================
		// info()
		// ================================================================
		AssetObj.set_function("info", [Mix, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(Mix))
			{
				Session.Log(TEXT("[FAIL] info -> asset no longer valid"));
				return sol::lua_nil;
			}

			sol::table R = Lua.create_table();

			R["initial_delay"] = Mix->InitialDelay;
			R["fade_in_time"] = Mix->FadeInTime;
			R["duration"] = Mix->Duration;
			R["fade_out_time"] = Mix->FadeOutTime;
			R["apply_eq"] = static_cast<bool>(Mix->bApplyEQ);
			R["eq_priority"] = Mix->EQPriority;

			// EQ settings summary
			sol::table EQ = Lua.create_table();
			const FAudioEQEffect& E = Mix->EQSettings;
			EQ["frequency_center_0"] = E.FrequencyCenter0; EQ["gain_0"] = E.Gain0; EQ["bandwidth_0"] = E.Bandwidth0;
			EQ["frequency_center_1"] = E.FrequencyCenter1; EQ["gain_1"] = E.Gain1; EQ["bandwidth_1"] = E.Bandwidth1;
			EQ["frequency_center_2"] = E.FrequencyCenter2; EQ["gain_2"] = E.Gain2; EQ["bandwidth_2"] = E.Bandwidth2;
			EQ["frequency_center_3"] = E.FrequencyCenter3; EQ["gain_3"] = E.Gain3; EQ["bandwidth_3"] = E.Bandwidth3;
			R["eq_settings"] = EQ;

			R["effect_count"] = Mix->SoundClassEffects.Num();
			TArray<USoundClass*> ProblemClasses;
			R["has_dependency_loop"] = GetSoundMixPassiveDependencyLoop(Mix, ProblemClasses);
			R["dependency_loop_problem_class_count"] = ProblemClasses.Num();
			sol::table ProblemClassTable = Lua.create_table();
			int32 ProblemIndex = 1;
			for (int32 i = 0; i < ProblemClasses.Num(); i++)
			{
				if (!ProblemClasses[i])
				{
					continue;
				}
				sol::table Entry = Lua.create_table();
				Entry["index"] = i + 1;
				Entry["name"] = std::string(TCHAR_TO_UTF8(*ProblemClasses[i]->GetName()));
				Entry["path"] = std::string(TCHAR_TO_UTF8(*ProblemClasses[i]->GetPathName()));
				ProblemClassTable[ProblemIndex++] = Entry;
			}
			R["dependency_loop_problem_classes"] = ProblemClassTable;

			// Brief effects list
			sol::table Effects = Lua.create_table();
			for (int32 i = 0; i < Mix->SoundClassEffects.Num(); i++)
			{
				const FSoundClassAdjuster& Adj = Mix->SoundClassEffects[i];
				sol::table Entry = Lua.create_table();
				Entry["index"] = i + 1;
				Entry["sound_class"] = Adj.SoundClassObject
					? std::string(TCHAR_TO_UTF8(*Adj.SoundClassObject->GetPathName()))
					: std::string("(none)");
				Entry["volume"] = Adj.VolumeAdjuster;
				Entry["pitch"] = Adj.PitchAdjuster;
				Entry["low_pass_filter_frequency"] = Adj.LowPassFilterFrequency;
				Entry["apply_to_children"] = static_cast<bool>(Adj.bApplyToChildren);
				Entry["voice_center_channel_volume"] = Adj.VoiceCenterChannelVolumeAdjuster;
				Effects[i + 1] = Entry;
			}
			R["effects"] = Effects;

			Session.Log(FString::Printf(TEXT("[OK] info() -> SoundMix, effects=%d, fade_in=%.2f, fade_out=%.2f"),
				Mix->SoundClassEffects.Num(), Mix->FadeInTime, Mix->FadeOutTime));
			return R;
		});

		// ================================================================
		// list("effect")
		// ================================================================
		AssetObj.set_function("list", [Mix, &Session](sol::table /*self*/,
			const std::string& What, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(Mix))
			{
				Session.Log(TEXT("[FAIL] list -> asset no longer valid"));
				return sol::lua_nil;
			}

			if (What != "effect" && What != "effects")
			{
				Session.Log(FString::Printf(TEXT("[FAIL] list('%s') -> unknown. Valid: 'effect'"),
					UTF8_TO_TCHAR(What.c_str())));
				return sol::lua_nil;
			}

			sol::table R = Lua.create_table();
			for (int32 i = 0; i < Mix->SoundClassEffects.Num(); i++)
			{
				const FSoundClassAdjuster& Adj = Mix->SoundClassEffects[i];
				sol::table Entry = Lua.create_table();
				Entry["index"] = i + 1;
				Entry["sound_class"] = Adj.SoundClassObject
					? std::string(TCHAR_TO_UTF8(*Adj.SoundClassObject->GetPathName()))
					: std::string("(none)");
				Entry["volume"] = Adj.VolumeAdjuster;
				Entry["pitch"] = Adj.PitchAdjuster;
				Entry["low_pass_filter_frequency"] = Adj.LowPassFilterFrequency;
				Entry["apply_to_children"] = static_cast<bool>(Adj.bApplyToChildren);
				Entry["voice_center_channel_volume"] = Adj.VoiceCenterChannelVolumeAdjuster;
				R[i + 1] = Entry;
			}

			Session.Log(FString::Printf(TEXT("[OK] list('effect') -> %d effects"), Mix->SoundClassEffects.Num()));
			return R;
		});

		// ================================================================
		// add("effect", opts)
		// ================================================================
		AssetObj.set_function("add", [Mix, &Session](sol::table /*self*/,
			const std::string& What, sol::table Opts, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(Mix))
			{
				Session.Log(TEXT("[FAIL] add -> asset no longer valid"));
				return sol::lua_nil;
			}

			if (What != "effect")
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add('%s') -> unknown. Valid: 'effect'"),
					UTF8_TO_TCHAR(What.c_str())));
				return sol::lua_nil;
			}

			// sound_class is required
			std::string ClassPath = Opts.get_or<std::string>("sound_class", "");
			if (ClassPath.empty())
			{
				Session.Log(TEXT("[FAIL] add('effect') -> 'sound_class' path is required"));
				return sol::lua_nil;
			}

			USoundClass* SC = NeoLuaAsset::Resolve<USoundClass>(NeoLuaStr::ToFString(ClassPath));
			if (!SC)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add('effect') -> could not load SoundClass '%s'"),
					UTF8_TO_TCHAR(ClassPath.c_str())));
				return sol::lua_nil;
			}

			// Warn about duplicates
			for (int32 i = 0; i < Mix->SoundClassEffects.Num(); i++)
			{
				if (Mix->SoundClassEffects[i].SoundClassObject == SC)
				{
					Session.Log(FString::Printf(TEXT("[WARN] add('effect') -> SoundClass '%s' already exists at index %d, adding duplicate"),
						*SC->GetName(), i + 1));
					break;
				}
			}

			const FScopedTransaction Transaction(FText::FromString(TEXT("SoundMix: Add Effect")));
			Mix->Modify();

			FSoundClassAdjuster NewAdj;
			NewAdj.SoundClassObject = SC;
			NewAdj.VolumeAdjuster = static_cast<float>(Opts.get_or("volume", 1.0));
			NewAdj.PitchAdjuster = static_cast<float>(Opts.get_or("pitch", 1.0));
			NewAdj.LowPassFilterFrequency = static_cast<float>(Opts.get_or("low_pass_filter_frequency", 20000.0));
			NewAdj.bApplyToChildren = Opts.get_or("apply_to_children", false);
			NewAdj.VoiceCenterChannelVolumeAdjuster = static_cast<float>(Opts.get_or("voice_center_channel_volume", 1.0));

			Mix->SoundClassEffects.Add(NewAdj);

			Mix->PostEditChange();
			Mix->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] add('effect') -> added SoundClass '%s' at index %d"),
				*SC->GetName(), Mix->SoundClassEffects.Num()));

			sol::table R = Lua.create_table();
			R["index"] = Mix->SoundClassEffects.Num();
			return R;
		});

		// ================================================================
		// remove("effect", opts)
		// ================================================================
		AssetObj.set_function("remove", [Mix, &Session](sol::table /*self*/,
			const std::string& What, sol::table Opts, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(Mix))
			{
				Session.Log(TEXT("[FAIL] remove -> asset no longer valid"));
				return sol::lua_nil;
			}

			if (What != "effect")
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove('%s') -> unknown. Valid: 'effect'"),
					UTF8_TO_TCHAR(What.c_str())));
				return sol::lua_nil;
			}

			int32 RemoveIdx = INDEX_NONE;

			// By index (1-based)
			sol::optional<int> Idx = Opts.get<sol::optional<int>>("index");
			if (Idx.has_value())
			{
				RemoveIdx = Idx.value() - 1; // convert to 0-based
			}
			else
			{
				// By sound_class path
				std::string ClassPath = Opts.get_or<std::string>("sound_class", "");
				if (!ClassPath.empty())
				{
					FString FClassPath = NeoLuaStr::ToFString(ClassPath);
					USoundClass* TargetClass = NeoLuaAsset::Resolve<USoundClass>(FClassPath);
					for (int32 i = 0; i < Mix->SoundClassEffects.Num(); i++)
					{
						USoundClass* ExistingClass = Mix->SoundClassEffects[i].SoundClassObject;
						if (ExistingClass &&
							(ExistingClass == TargetClass ||
							 ExistingClass->GetPathName() == FClassPath ||
							 ExistingClass->GetPathName().StartsWith(FClassPath + TEXT("."))))
						{
							RemoveIdx = i;
							break;
						}
					}
				}
			}

			if (RemoveIdx < 0 || RemoveIdx >= Mix->SoundClassEffects.Num())
			{
				Session.Log(TEXT("[FAIL] remove('effect') -> effect not found or index out of range"));
				return sol::lua_nil;
			}

			const FScopedTransaction Transaction(FText::FromString(TEXT("SoundMix: Remove Effect")));
			Mix->Modify();

			FString RemovedName = Mix->SoundClassEffects[RemoveIdx].SoundClassObject
				? Mix->SoundClassEffects[RemoveIdx].SoundClassObject->GetName()
				: TEXT("(none)");

			Mix->SoundClassEffects.RemoveAt(RemoveIdx);

			Mix->PostEditChange();
			Mix->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] remove('effect') -> removed '%s', %d effects remain"),
				*RemovedName, Mix->SoundClassEffects.Num()));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// configure(params) — mix-level settings
		// configure('effect', {index=N, ...}) — modify existing adjuster
		// ================================================================
		AssetObj.set_function("configure", [Mix, &Session](sol::table /*self*/,
			sol::object FirstArg, sol::optional<sol::table> SecondArg, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(Mix))
			{
				Session.Log(TEXT("[FAIL] configure -> asset no longer valid"));
				return sol::lua_nil;
			}

			// ---- configure('effect', {index=N, ...}) ----
			if (FirstArg.is<std::string>())
			{
				std::string What = FirstArg.as<std::string>();
				if (What != "effect")
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure('%s') -> unknown. Valid: 'effect'"),
						UTF8_TO_TCHAR(What.c_str())));
					return sol::lua_nil;
				}

				if (!SecondArg.has_value())
				{
					Session.Log(TEXT("[FAIL] configure('effect') -> options table with 'index' is required"));
					return sol::lua_nil;
				}

				sol::table Opts = SecondArg.value();
				sol::optional<int> Idx = Opts.get<sol::optional<int>>("index");
				if (!Idx.has_value())
				{
					Session.Log(TEXT("[FAIL] configure('effect') -> 'index' (1-based) is required"));
					return sol::lua_nil;
				}

				int32 AdjIdx = Idx.value() - 1;
				if (AdjIdx < 0 || AdjIdx >= Mix->SoundClassEffects.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure('effect') -> index %d out of range (1..%d)"),
						Idx.value(), Mix->SoundClassEffects.Num()));
					return sol::lua_nil;
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("SoundMix: Configure Effect")));
				Mix->Modify();

				FSoundClassAdjuster& Adj = Mix->SoundClassEffects[AdjIdx];
				bool bModified = false;
				FString Changes;

				// sound_class (reassign)
				sol::optional<std::string> NewClassPath = Opts.get<sol::optional<std::string>>("sound_class");
				if (NewClassPath.has_value() && !NewClassPath.value().empty())
				{
					USoundClass* NewSC = NeoLuaAsset::Resolve<USoundClass>(NeoLuaStr::ToFStringOpt(NewClassPath));
					if (NewSC)
					{
						Adj.SoundClassObject = NewSC;
						Changes += FString::Printf(TEXT(" sound_class=%s"), *NewSC->GetName());
						bModified = true;
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure('effect') -> could not load SoundClass '%s', skipping"),
							UTF8_TO_TCHAR(NewClassPath.value().c_str())));
					}
				}

				sol::optional<double> Vol = Opts.get<sol::optional<double>>("volume");
				if (Vol.has_value())
				{
					Adj.VolumeAdjuster = FMath::Max(0.0f, static_cast<float>(Vol.value()));
					Changes += FString::Printf(TEXT(" volume=%.2f"), Adj.VolumeAdjuster);
					bModified = true;
				}

				sol::optional<double> Pitch = Opts.get<sol::optional<double>>("pitch");
				if (Pitch.has_value())
				{
					Adj.PitchAdjuster = FMath::Clamp(static_cast<float>(Pitch.value()), 0.0f, 8.0f);
					Changes += FString::Printf(TEXT(" pitch=%.2f"), Adj.PitchAdjuster);
					bModified = true;
				}

				sol::optional<double> LPF = Opts.get<sol::optional<double>>("low_pass_filter_frequency");
				if (LPF.has_value())
				{
					Adj.LowPassFilterFrequency = FMath::Clamp(static_cast<float>(LPF.value()), 0.0f, 20000.0f);
					Changes += FString::Printf(TEXT(" lpf=%.0f"), Adj.LowPassFilterFrequency);
					bModified = true;
				}

				sol::optional<bool> ApplyChildren = Opts.get<sol::optional<bool>>("apply_to_children");
				if (ApplyChildren.has_value())
				{
					Adj.bApplyToChildren = ApplyChildren.value();
					Changes += FString::Printf(TEXT(" apply_to_children=%s"), ApplyChildren.value() ? TEXT("true") : TEXT("false"));
					bModified = true;
				}

				sol::optional<double> VoiceCenter = Opts.get<sol::optional<double>>("voice_center_channel_volume");
				if (VoiceCenter.has_value())
				{
					Adj.VoiceCenterChannelVolumeAdjuster = FMath::Max(0.0f, static_cast<float>(VoiceCenter.value()));
					Changes += FString::Printf(TEXT(" voice_center=%.2f"), Adj.VoiceCenterChannelVolumeAdjuster);
					bModified = true;
				}

				if (bModified)
				{
					Mix->PostEditChange();
					Mix->MarkPackageDirty();
					Session.Log(FString::Printf(TEXT("[OK] configure('effect', index=%d) ->%s"), Idx.value(), *Changes));
					return sol::make_object(Lua, true);
				}

				Session.Log(FString::Printf(TEXT("[OK] configure('effect', index=%d) -> nothing changed"), Idx.value()));
				return sol::make_object(Lua, true);
			}

			// Mix-level bulk property setter removed 2026-04-20 — agents should use
			// asset:set("PropertyName", value) for InitialDelay, FadeInTime, Duration,
			// FadeOutTime, bApplyEQ, EQPriority, and asset:set("EQSettings.FrequencyCenter0",
			// value) etc. for the EQ band fields. The out-of-range Clamp bounds that used
			// to live here are defensive; engine ImportText accepts any float.
			Session.Log(TEXT("[FAIL] configure() -> only configure('effect', {index=N, ...}) is supported. Use asset:set for mix-level properties (InitialDelay, FadeInTime, Duration, FadeOutTime, bApplyEQ, EQPriority, EQSettings.*)"));
			return sol::lua_nil;
		});
	});
}

REGISTER_LUA_BINDING(SoundMix, SoundMixDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindSoundMix(Lua, Session);
});
