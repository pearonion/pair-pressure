// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Lua/LuaAssetResolver.h"
#include "Lua/LuaStr.h"
#include "ScopedTransaction.h"

#include "Sound/SoundClass.h"
#include "Sound/SoundMix.h"
#include "Sound/SoundSubmix.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// HELPERS
// ============================================================================

static const TCHAR* LoadingBehaviorToString(ESoundWaveLoadingBehavior B)
{
	switch (B)
	{
	case ESoundWaveLoadingBehavior::Inherited:    return TEXT("inherited");
	case ESoundWaveLoadingBehavior::RetainOnLoad: return TEXT("retain_on_load");
	case ESoundWaveLoadingBehavior::PrimeOnLoad:  return TEXT("prime_on_load");
	case ESoundWaveLoadingBehavior::LoadOnDemand: return TEXT("load_on_demand");
	case ESoundWaveLoadingBehavior::ForceInline:  return TEXT("force_inline");
	case ESoundWaveLoadingBehavior::Uninitialized:return TEXT("uninitialized");
	default:                                      return TEXT("inherited");
	}
}

static const TCHAR* OutputTargetToString(EAudioOutputTarget::Type T)
{
	switch (T)
	{
	case EAudioOutputTarget::Speaker:                      return TEXT("speaker");
	case EAudioOutputTarget::Controller:                   return TEXT("controller");
	case EAudioOutputTarget::ControllerFallbackToSpeaker:  return TEXT("controller_fallback_to_speaker");
	default:                                               return TEXT("speaker");
	}
}

static void PostEditAndDirtySoundClass(USoundClass* SoundClass)
{
	if (!IsValid(SoundClass))
	{
		return;
	}

	SoundClass->PostEditChange();
	SoundClass->MarkPackageDirty();
}

static int32 CountValidSoundClassChildren(const USoundClass* SoundClass)
{
	int32 Count = 0;
	if (!SoundClass)
	{
		return Count;
	}

	for (const TObjectPtr<USoundClass>& Child : SoundClass->ChildClasses)
	{
		if (Child)
		{
			++Count;
		}
	}
	return Count;
}

// ============================================================================
// BINDING
// ============================================================================

static TArray<FLuaFunctionDoc> SoundClassDocs = {};

static void BindSoundClass(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_sound_class", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = NeoLuaStr::ToFString(PathStr);
		USoundClass* SoundClass = NeoLuaAsset::Resolve<USoundClass>(FPath);
		if (!SoundClass) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"SoundClass enrichment methods:\n"
			"\n"
			"info() — structured summary:\n"
			"  volume, pitch, low_pass_filter_frequency, attenuation_distance_scale,\n"
			"  lfe_bleed, voice_center_channel_volume, always_play, is_ui_sound,\n"
			"  is_music, center_channel_only, apply_ambient_volumes, apply_reverb,\n"
			"  default_2d_reverb_send_amount, apply_effects, loading_behavior,\n"
			"  output_target, default_submix, modulation,\n"
			"  radio_filter_volume, radio_filter_volume_threshold,\n"
			"  parent, child_count, children, passive_sound_mix_count,\n"
			"  passive_sound_mixes\n"
			"\n"
			"Property editing uses the generic asset reflection API:\n"
			"  get(\"PropertyName\")\n"
			"  set(\"PropertyName\", \"Value\")\n"
			"  list_properties(filter?, all?)\n"
			"\n"
			"Use raw engine property names on the Properties struct, e.g.:\n"
			"  Properties.Volume\n"
			"  Properties.Pitch\n"
			"  Properties.LowPassFilterFrequency\n"
			"  Properties.AttenuationDistanceScale\n"
			"  Properties.LFEBleed\n"
			"  Properties.VoiceCenterChannelVolume\n"
			"  Properties.bAlwaysPlay\n"
			"  Properties.bIsUISound\n"
			"  Properties.bIsMusic\n"
			"  Properties.bCenterChannelOnly\n"
			"  Properties.bApplyAmbientVolumes\n"
			"  Properties.bReverb\n"
			"  Properties.Default2DReverbSendAmount\n"
			"  Properties.bApplyEffects\n"
			"  Properties.RadioFilterVolume\n"
			"  Properties.RadioFilterVolumeThreshold\n"
			"  Properties.LoadingBehavior\n"
			"  Properties.OutputTarget\n"
			"  Properties.DefaultSubmix\n"
			"\n"
			"list(type) — type = 'passive_sound_mixes' | 'children'\n"
			"\n"
			"add(type, params) — add items:\n"
			"  type='passive_sound_mix': {sound_mix='/path', min_volume=0, max_volume=10}\n"
			"  type='child': {path='/Game/MyChildClass'}\n"
			"\n"
			"remove(type, params) — remove items:\n"
			"  type='passive_sound_mix': {index=N} or {sound_mix='/path'}\n"
			"  type='child': {index=N} or {path='/path'}\n";

		// ================================================================
		// info()
		// ================================================================
		AssetObj.set_function("info", [SoundClass, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(SoundClass))
			{
				Session.Log(TEXT("[FAIL] info -> asset no longer valid"));
				return sol::lua_nil;
			}

			const FSoundClassProperties& P = SoundClass->Properties;
			sol::table R = Lua.create_table();

			// Core properties
			R["volume"] = P.Volume;
			R["pitch"] = P.Pitch;
			R["low_pass_filter_frequency"] = P.LowPassFilterFrequency;
			R["attenuation_distance_scale"] = P.AttenuationDistanceScale;
			R["lfe_bleed"] = P.LFEBleed;
			R["voice_center_channel_volume"] = P.VoiceCenterChannelVolume;

			// Flags
			R["always_play"] = static_cast<bool>(P.bAlwaysPlay);
			R["is_ui_sound"] = static_cast<bool>(P.bIsUISound);
			R["is_music"] = static_cast<bool>(P.bIsMusic);
			R["center_channel_only"] = static_cast<bool>(P.bCenterChannelOnly);
			R["apply_ambient_volumes"] = static_cast<bool>(P.bApplyAmbientVolumes);
			R["apply_reverb"] = static_cast<bool>(P.bReverb);
			R["apply_effects"] = static_cast<bool>(P.bApplyEffects);
			R["default_2d_reverb_send_amount"] = P.Default2DReverbSendAmount;

			// Loading behavior
			R["loading_behavior"] = std::string(TCHAR_TO_UTF8(LoadingBehaviorToString(P.LoadingBehavior)));

			// Output target
			R["output_target"] = std::string(TCHAR_TO_UTF8(OutputTargetToString(P.OutputTarget)));

			// Default submix
			if (P.DefaultSubmix)
			{
				R["default_submix"] = std::string(TCHAR_TO_UTF8(*P.DefaultSubmix->GetPathName()));
			}

			// Modulation settings
			{
				sol::table Mod = Lua.create_table();
				sol::table Vol = Lua.create_table();
				Vol["value"] = P.ModulationSettings.VolumeModulationDestination.Value;
				Mod["volume"] = Vol;

				sol::table Pit = Lua.create_table();
				Pit["value"] = P.ModulationSettings.PitchModulationDestination.Value;
				Mod["pitch"] = Pit;

				sol::table HP = Lua.create_table();
				HP["value"] = P.ModulationSettings.HighpassModulationDestination.Value;
				Mod["highpass"] = HP;

				sol::table LP = Lua.create_table();
				LP["value"] = P.ModulationSettings.LowpassModulationDestination.Value;
				Mod["lowpass"] = LP;

				R["modulation"] = Mod;
			}

			// Legacy
			R["radio_filter_volume"] = P.RadioFilterVolume;
			R["radio_filter_volume_threshold"] = P.RadioFilterVolumeThreshold;

			// Hierarchy
			if (SoundClass->ParentClass)
			{
				R["parent"] = std::string(TCHAR_TO_UTF8(*SoundClass->ParentClass->GetPathName()));
			}

			const int32 ValidChildCount = CountValidSoundClassChildren(SoundClass);
			R["child_count"] = ValidChildCount;
			R["child_slot_count"] = SoundClass->ChildClasses.Num();
			if (SoundClass->ChildClasses.Num() > 0)
			{
				sol::table Children = Lua.create_table();
				int32 OutIndex = 1;
				for (int32 i = 0; i < SoundClass->ChildClasses.Num(); i++)
				{
					if (SoundClass->ChildClasses[i])
					{
						Children[OutIndex++] = std::string(TCHAR_TO_UTF8(*SoundClass->ChildClasses[i]->GetPathName()));
					}
				}
				R["children"] = Children;
			}

			// Passive sound mix modifiers
			R["passive_sound_mix_count"] = SoundClass->PassiveSoundMixModifiers.Num();
			if (SoundClass->PassiveSoundMixModifiers.Num() > 0)
			{
				sol::table Mixes = Lua.create_table();
				for (int32 i = 0; i < SoundClass->PassiveSoundMixModifiers.Num(); i++)
				{
					const FPassiveSoundMixModifier& Mod = SoundClass->PassiveSoundMixModifiers[i];
					sol::table Entry = Lua.create_table();
					Entry["index"] = i + 1;
					if (Mod.SoundMix)
					{
						Entry["sound_mix"] = std::string(TCHAR_TO_UTF8(*Mod.SoundMix->GetPathName()));
					}
					Entry["min_volume_threshold"] = Mod.MinVolumeThreshold;
					Entry["max_volume_threshold"] = Mod.MaxVolumeThreshold;
					Mixes[i + 1] = Entry;
				}
				R["passive_sound_mixes"] = Mixes;
			}

			Session.Log(FString::Printf(TEXT("[OK] info() -> SoundClass, volume=%.2f, pitch=%.2f, children=%d"),
				P.Volume, P.Pitch, ValidChildCount));
			return R;
		});

		// ================================================================
		// list(type)
		// ================================================================
		AssetObj.set_function("list", [SoundClass, &Session](sol::table /*self*/,
			std::string Type, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(SoundClass))
			{
				Session.Log(TEXT("[FAIL] list -> asset no longer valid"));
				return sol::lua_nil;
			}

			if (Type == "passive_sound_mixes")
			{
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < SoundClass->PassiveSoundMixModifiers.Num(); i++)
				{
					const FPassiveSoundMixModifier& Mod = SoundClass->PassiveSoundMixModifiers[i];
					sol::table Entry = Lua.create_table();
					Entry["index"] = i + 1;
					if (Mod.SoundMix)
					{
						Entry["sound_mix"] = std::string(TCHAR_TO_UTF8(*Mod.SoundMix->GetPathName()));
						Entry["name"] = std::string(TCHAR_TO_UTF8(*Mod.SoundMix->GetName()));
					}
					Entry["min_volume_threshold"] = Mod.MinVolumeThreshold;
					Entry["max_volume_threshold"] = Mod.MaxVolumeThreshold;
					Result[i + 1] = Entry;
				}
				Session.Log(FString::Printf(TEXT("[OK] list('passive_sound_mixes') -> %d entries"),
					SoundClass->PassiveSoundMixModifiers.Num()));
				return Result;
			}

			if (Type == "children")
			{
				sol::table Result = Lua.create_table();
				int32 OutIndex = 1;
				for (int32 i = 0; i < SoundClass->ChildClasses.Num(); i++)
				{
					if (SoundClass->ChildClasses[i])
					{
						sol::table Entry = Lua.create_table();
						Entry["index"] = i + 1;
						Entry["slot_index"] = i + 1;
						Entry["path"] = std::string(TCHAR_TO_UTF8(*SoundClass->ChildClasses[i]->GetPathName()));
						Entry["name"] = std::string(TCHAR_TO_UTF8(*SoundClass->ChildClasses[i]->GetName()));
						Result[OutIndex++] = Entry;
					}
				}
				Session.Log(FString::Printf(TEXT("[OK] list('children') -> %d entries"),
					OutIndex - 1));
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list('%s') -> unknown type. Use: passive_sound_mixes, children"),
				UTF8_TO_TCHAR(Type.c_str())));
			return sol::lua_nil;
		});

		// ================================================================
		// add(type, params)
		// ================================================================
		AssetObj.set_function("add", [SoundClass, &Session](sol::table /*self*/,
			std::string Type, sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(SoundClass))
			{
				Session.Log(TEXT("[FAIL] add -> asset no longer valid"));
				return sol::lua_nil;
			}

			if (Type == "passive_sound_mix")
			{
				sol::optional<std::string> MixPath = Params.get<sol::optional<std::string>>("sound_mix");
				if (!MixPath.has_value() || MixPath.value().empty())
				{
					Session.Log(TEXT("[FAIL] add('passive_sound_mix') -> 'sound_mix' path required"));
					return sol::lua_nil;
				}

				FString Path = NeoLuaStr::ToFString(MixPath.value());
				USoundMix* Mix = NeoLuaAsset::Resolve<USoundMix>(Path);
				if (!Mix)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add('passive_sound_mix') -> could not load SoundMix '%s'"), *Path));
					return sol::lua_nil;
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("SoundClass: Add Passive Sound Mix")));
				SoundClass->Modify();

				FPassiveSoundMixModifier NewMod;
				NewMod.SoundMix = Mix;
				NewMod.MinVolumeThreshold = static_cast<float>(Params.get_or("min_volume", 0.0));
				NewMod.MaxVolumeThreshold = static_cast<float>(Params.get_or("max_volume", 10.0));
				SoundClass->PassiveSoundMixModifiers.Add(NewMod);

				SoundClass->PostEditChange();
				SoundClass->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add('passive_sound_mix') -> added '%s' (min=%.1f, max=%.1f)"),
					*Mix->GetName(), NewMod.MinVolumeThreshold, NewMod.MaxVolumeThreshold));
				return sol::make_object(Lua, SoundClass->PassiveSoundMixModifiers.Num());
			}

			if (Type == "child")
			{
				sol::optional<std::string> ChildPath = Params.get<sol::optional<std::string>>("path");
				if (!ChildPath.has_value() || ChildPath.value().empty())
				{
					Session.Log(TEXT("[FAIL] add('child') -> 'path' required"));
					return sol::lua_nil;
				}

				FString Path = NeoLuaStr::ToFString(ChildPath.value());
				USoundClass* Child = NeoLuaAsset::Resolve<USoundClass>(Path);
				if (!Child)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add('child') -> could not load SoundClass '%s'"), *Path));
					return sol::lua_nil;
				}

				if (Child == SoundClass)
				{
					Session.Log(TEXT("[FAIL] add('child') -> cannot add self as child"));
					return sol::lua_nil;
				}

				// Cycle detection: check if Child is an ancestor of SoundClass
				if (Child->RecurseCheckChild(SoundClass))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add('child') -> '%s' is an ancestor, would create cycle"), *Child->GetName()));
					return sol::lua_nil;
				}

				// Check for duplicates
				if (SoundClass->ChildClasses.Contains(Child))
				{
					Session.Log(FString::Printf(TEXT("[WARN] add('child') -> '%s' is already a child"), *Child->GetName()));
					return sol::make_object(Lua, SoundClass->ChildClasses.Num());
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("SoundClass: Add Child")));
				SoundClass->Modify();
				USoundClass* OldParent = Child->ParentClass;

				// SetParentClass removes from old parent and sets ParentClass,
				// but does NOT add to new parent's ChildClasses array
				Child->SetParentClass(SoundClass);
				SoundClass->ChildClasses.AddUnique(Child);

				PostEditAndDirtySoundClass(SoundClass);
				PostEditAndDirtySoundClass(Child);
				if (OldParent && OldParent != SoundClass)
				{
					PostEditAndDirtySoundClass(OldParent);
				}
				SoundClass->RefreshAllGraphs(false);
				Session.Log(FString::Printf(TEXT("[OK] add('child') -> added '%s'"), *Child->GetName()));
				return sol::make_object(Lua, SoundClass->ChildClasses.Num());
			}

			Session.Log(FString::Printf(TEXT("[FAIL] add('%s') -> unknown type. Use: passive_sound_mix, child"),
				UTF8_TO_TCHAR(Type.c_str())));
			return sol::lua_nil;
		});

		// ================================================================
		// remove(type, params)
		// ================================================================
		AssetObj.set_function("remove", [SoundClass, &Session](sol::table /*self*/,
			std::string Type, sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(SoundClass))
			{
				Session.Log(TEXT("[FAIL] remove -> asset no longer valid"));
				return sol::lua_nil;
			}

			if (Type == "passive_sound_mix")
			{
				const FScopedTransaction Transaction(FText::FromString(TEXT("SoundClass: Remove Passive Sound Mix")));
				SoundClass->Modify();

				// By index (1-based)
				sol::optional<int> Idx = Params.get<sol::optional<int>>("index");
				if (Idx.has_value())
				{
					int32 ZeroIdx = Idx.value() - 1;
					if (ZeroIdx < 0 || ZeroIdx >= SoundClass->PassiveSoundMixModifiers.Num())
					{
						Session.Log(FString::Printf(TEXT("[FAIL] remove('passive_sound_mix') -> index %d out of range (1-%d)"),
							Idx.value(), SoundClass->PassiveSoundMixModifiers.Num()));
						return sol::lua_nil;
					}
					SoundClass->PassiveSoundMixModifiers.RemoveAt(ZeroIdx);
					SoundClass->PostEditChange();
					SoundClass->MarkPackageDirty();
					Session.Log(FString::Printf(TEXT("[OK] remove('passive_sound_mix') -> removed index %d"), Idx.value()));
					return sol::make_object(Lua, true);
				}

				// By sound mix path
				sol::optional<std::string> MixPath = Params.get<sol::optional<std::string>>("sound_mix");
				if (MixPath.has_value())
				{
					FString Path = NeoLuaStr::ToFString(MixPath.value());
					for (int32 i = SoundClass->PassiveSoundMixModifiers.Num() - 1; i >= 0; i--)
					{
						if (SoundClass->PassiveSoundMixModifiers[i].SoundMix &&
							SoundClass->PassiveSoundMixModifiers[i].SoundMix->GetPathName() == Path)
						{
							SoundClass->PassiveSoundMixModifiers.RemoveAt(i);
							SoundClass->PostEditChange();
							SoundClass->MarkPackageDirty();
							Session.Log(FString::Printf(TEXT("[OK] remove('passive_sound_mix') -> removed '%s'"), *Path));
							return sol::make_object(Lua, true);
						}
					}
					Session.Log(FString::Printf(TEXT("[FAIL] remove('passive_sound_mix') -> '%s' not found"), *Path));
					return sol::lua_nil;
				}

				Session.Log(TEXT("[FAIL] remove('passive_sound_mix') -> specify 'index' or 'sound_mix'"));
				return sol::lua_nil;
			}

			if (Type == "child")
			{
				const FScopedTransaction Transaction(FText::FromString(TEXT("SoundClass: Remove Child")));
				SoundClass->Modify();

				// By index (1-based)
				sol::optional<int> Idx = Params.get<sol::optional<int>>("index");
				if (Idx.has_value())
				{
					int32 ZeroIdx = Idx.value() - 1;
					if (ZeroIdx < 0 || ZeroIdx >= SoundClass->ChildClasses.Num())
					{
						Session.Log(FString::Printf(TEXT("[FAIL] remove('child') -> index %d out of range (1-%d)"),
							Idx.value(), SoundClass->ChildClasses.Num()));
						return sol::lua_nil;
					}
					USoundClass* Child = SoundClass->ChildClasses[ZeroIdx];
					USoundClass* OldParent = Child ? Child->ParentClass : nullptr;
					if (Child && OldParent == SoundClass)
					{
						Child->SetParentClass(nullptr);
					}
					if (SoundClass->ChildClasses.IsValidIndex(ZeroIdx) && SoundClass->ChildClasses[ZeroIdx] == Child)
					{
						SoundClass->ChildClasses.RemoveAt(ZeroIdx);
					}
					else if (Child)
					{
						SoundClass->ChildClasses.Remove(Child);
					}
					else
					{
						SoundClass->ChildClasses.RemoveAt(ZeroIdx);
					}
					PostEditAndDirtySoundClass(SoundClass);
					PostEditAndDirtySoundClass(Child);
					if (OldParent && OldParent != SoundClass)
					{
						PostEditAndDirtySoundClass(OldParent);
					}
					SoundClass->RefreshAllGraphs(false);
					Session.Log(FString::Printf(TEXT("[OK] remove('child') -> removed index %d"), Idx.value()));
					return sol::make_object(Lua, true);
				}

				// By path
				sol::optional<std::string> ChildPath = Params.get<sol::optional<std::string>>("path");
				if (ChildPath.has_value())
				{
					FString Path = NeoLuaStr::ToFString(ChildPath.value());
					for (int32 i = SoundClass->ChildClasses.Num() - 1; i >= 0; i--)
					{
						if (SoundClass->ChildClasses[i] && SoundClass->ChildClasses[i]->GetPathName() == Path)
						{
							USoundClass* Child = SoundClass->ChildClasses[i];
							USoundClass* OldParent = Child->ParentClass;
							if (OldParent == SoundClass)
							{
								Child->SetParentClass(nullptr);
							}
							if (SoundClass->ChildClasses.IsValidIndex(i) && SoundClass->ChildClasses[i] == Child)
							{
								SoundClass->ChildClasses.RemoveAt(i);
							}
							else
							{
								SoundClass->ChildClasses.Remove(Child);
							}
							PostEditAndDirtySoundClass(SoundClass);
							PostEditAndDirtySoundClass(Child);
							if (OldParent && OldParent != SoundClass)
							{
								PostEditAndDirtySoundClass(OldParent);
							}
							SoundClass->RefreshAllGraphs(false);
							Session.Log(FString::Printf(TEXT("[OK] remove('child') -> removed '%s'"), *Path));
							return sol::make_object(Lua, true);
						}
					}
					Session.Log(FString::Printf(TEXT("[FAIL] remove('child') -> '%s' not found"), *Path));
					return sol::lua_nil;
				}

				Session.Log(TEXT("[FAIL] remove('child') -> specify 'index' or 'path'"));
				return sol::lua_nil;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] remove('%s') -> unknown type. Use: passive_sound_mix, child"),
				UTF8_TO_TCHAR(Type.c_str())));
			return sol::lua_nil;
		});
	});
}

REGISTER_LUA_BINDING(SoundClass, SoundClassDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindSoundClass(Lua, Session);
});
