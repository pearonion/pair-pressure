// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Lua/LuaAssetResolver.h"
#include "Lua/LuaStr.h"
#include "Tools/NeoStackToolUtils.h"
#include "ScopedTransaction.h"

#include "Sound/SoundCue.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundNode.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundConcurrency.h"
#include "Sound/SoundSubmix.h"
#include "Sound/SoundNodeWavePlayer.h"
#include "Sound/SoundEffectSource.h"
#include "Sound/SoundSourceBusSend.h"
#include "Sound/SoundSubmixSend.h"
#include "Sound/SoundSourceBus.h"
#include "Sound/AudioBus.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// HELPERS
// ============================================================================

static int32 GetSoundCueNodeCount(const USoundCue* Cue)
{
#if WITH_EDITORONLY_DATA
	return Cue ? Cue->AllNodes.Num() : 0;
#else
	(void)Cue;
	return 0;
#endif
}

static bool HasSoundCueGraph(const USoundCue* Cue)
{
#if WITH_EDITORONLY_DATA
	return Cue && Cue->SoundCueGraph != nullptr;
#else
	(void)Cue;
	return false;
#endif
}

static const char* VirtualizationModeToString(EVirtualizationMode Mode)
{
	switch (Mode)
	{
	case EVirtualizationMode::Disabled:        return "disabled";
	case EVirtualizationMode::PlayWhenSilent:  return "play_when_silent";
	case EVirtualizationMode::Restart:         return "restart";
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
PRAGMA_DISABLE_DEPRECATION_WARNINGS
	case EVirtualizationMode::SeekRestart:     return "seek_restart";
PRAGMA_ENABLE_DEPRECATION_WARNINGS
#endif
	default:                                   return "disabled";
	}
}

namespace SoundCueHelpers {
static const char* AttenuationShapeToString(EAttenuationShape::Type Shape)
{
	switch (Shape)
	{
	case EAttenuationShape::Sphere:  return "sphere";
	case EAttenuationShape::Capsule: return "capsule";
	case EAttenuationShape::Box:     return "box";
	case EAttenuationShape::Cone:    return "cone";
	default:                         return "sphere";
	}
}
} // namespace SoundCueHelpers

static bool TrySetSeekRestartMode(USoundCue* Cue, const FString& ModeStr)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	if (ModeStr.Equals(TEXT("seek_restart"), ESearchCase::IgnoreCase) || ModeStr.Equals(TEXT("SeekRestart"), ESearchCase::IgnoreCase))
	{
PRAGMA_DISABLE_DEPRECATION_WARNINGS
		Cue->VirtualizationMode = EVirtualizationMode::SeekRestart;
PRAGMA_ENABLE_DEPRECATION_WARNINGS
		return true;
	}
#endif
	return false;
}

static const char* DistanceAlgorithmToString(EAttenuationDistanceModel Algo)
{
	switch (Algo)
	{
	case EAttenuationDistanceModel::Linear:       return "linear";
	case EAttenuationDistanceModel::Logarithmic:  return "logarithmic";
	case EAttenuationDistanceModel::Inverse:      return "inverse";
	case EAttenuationDistanceModel::LogReverse:   return "log_reverse";
	case EAttenuationDistanceModel::NaturalSound: return "natural_sound";
	case EAttenuationDistanceModel::Custom:       return "custom";
	default:                                      return "linear";
	}
}

static const char* SpatializationToString(ESoundSpatializationAlgorithm Algo)
{
	switch (Algo)
	{
	case SPATIALIZATION_Default: return "default";
	case SPATIALIZATION_HRTF:    return "hrtf";
	default:                     return "default";
	}
}

static sol::table BuildAttenuationTable(sol::state_view& Lua, const FSoundAttenuationSettings& Atten)
{
	sol::table AttenT = Lua.create_table();

	AttenT["attenuate"] = static_cast<bool>(Atten.bAttenuate);
	AttenT["spatialize"] = static_cast<bool>(Atten.bSpatialize);
	AttenT["shape"] = SoundCueHelpers::AttenuationShapeToString(Atten.AttenuationShape);

	sol::table Extents = Lua.create_table();
	Extents["x"] = Atten.AttenuationShapeExtents.X;
	Extents["y"] = Atten.AttenuationShapeExtents.Y;
	Extents["z"] = Atten.AttenuationShapeExtents.Z;
	AttenT["shape_extents"] = Extents;

	AttenT["falloff_distance"] = Atten.FalloffDistance;
	AttenT["db_attenuation_at_max"] = Atten.dBAttenuationAtMax;
	AttenT["distance_algorithm"] = DistanceAlgorithmToString(Atten.DistanceAlgorithm);

	AttenT["lpf_enabled"] = static_cast<bool>(Atten.bAttenuateWithLPF);
	AttenT["lpf_radius_min"] = Atten.LPFRadiusMin;
	AttenT["lpf_radius_max"] = Atten.LPFRadiusMax;
	AttenT["lpf_freq_min"] = Atten.LPFFrequencyAtMin;
	AttenT["lpf_freq_max"] = Atten.LPFFrequencyAtMax;
	AttenT["hpf_freq_min"] = Atten.HPFFrequencyAtMin;
	AttenT["hpf_freq_max"] = Atten.HPFFrequencyAtMax;

	AttenT["occlusion"] = static_cast<bool>(Atten.bEnableOcclusion);
	AttenT["reverb_send"] = static_cast<bool>(Atten.bEnableReverbSend);
	AttenT["spatialization_algorithm"] = SpatializationToString(Atten.SpatializationAlgorithm);

	return AttenT;
}

static bool BuildSoundCueNodeList(const USoundCue* Cue, sol::state_view& Lua, sol::table& Result, int32& OutCount)
{
#if WITH_EDITORONLY_DATA
	OutCount = Cue ? Cue->AllNodes.Num() : 0;
	if (!Cue)
	{
		return false;
	}

	for (int32 i = 0; i < Cue->AllNodes.Num(); i++)
	{
		USoundNode* Node = Cue->AllNodes[i];
		if (!Node) continue;

		sol::table E = Lua.create_table();
		E["index"] = i + 1;
		E["class"] = TCHAR_TO_UTF8(*Node->GetClass()->GetName());
		E["name"] = TCHAR_TO_UTF8(*Node->GetName());
		E["child_count"] = static_cast<int>(Node->ChildNodes.Num());

		if (UEdGraphNode* GraphNode = Node->GetGraphNode())
		{
			E["node_pos_x"] = GraphNode->NodePosX;
			E["node_pos_y"] = GraphNode->NodePosY;

			sol::table InputPins = Lua.create_table();
			int32 InputIndex = 1;
			for (UEdGraphPin* Pin : GraphNode->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Input)
				{
					continue;
				}

				sol::table PinInfo = Lua.create_table();
				PinInfo["index"] = InputIndex;
				PinInfo["name"] = TCHAR_TO_UTF8(*Pin->PinName.ToString());
				PinInfo["friendly_name"] = TCHAR_TO_UTF8(*Pin->PinFriendlyName.ToString());
				PinInfo["linked_to_count"] = Pin->LinkedTo.Num();
				InputPins[InputIndex++] = PinInfo;
			}
			E["input_pins"] = InputPins;
			E["input_pin_count"] = InputIndex - 1;
		}

		if (USoundNodeWavePlayer* WavePlayer = Cast<USoundNodeWavePlayer>(Node))
		{
			USoundWave* Wave = WavePlayer->GetSoundWave();
			if (Wave)
			{
				E["sound_wave"] = TCHAR_TO_UTF8(*Wave->GetPathName());
				E["sound_wave_name"] = TCHAR_TO_UTF8(*Wave->GetName());
			}
			E["is_looping"] = static_cast<bool>(WavePlayer->bLooping);
		}

		if (Node->ChildNodes.Num() > 0)
		{
			sol::table ChildIndices = Lua.create_table();
			int32 ChildIdx = 1;
			for (USoundNode* ChildNode : Node->ChildNodes)
			{
				if (!ChildNode) continue;
				const int32 ChildAllNodesIndex = Cue->AllNodes.IndexOfByKey(ChildNode);
				if (ChildAllNodesIndex != INDEX_NONE)
				{
					ChildIndices[ChildIdx++] = ChildAllNodesIndex + 1;
				}
			}
			E["children"] = ChildIndices;
		}

		Result[i + 1] = E;
	}

	return true;
#else
	(void)Cue;
	(void)Lua;
	(void)Result;
	OutCount = 0;
	return false;
#endif
}

static const char* SubmixSendStageToString(ESubmixSendStage Stage)
{
	switch (Stage)
	{
	case ESubmixSendStage::PostDistanceAttenuation: return "post_distance_attenuation";
	case ESubmixSendStage::PreDistanceAttenuation:  return "pre_distance_attenuation";
	default:                                         return "post_distance_attenuation";
	}
}

static const char* SendLevelControlToString(ESendLevelControlMethod Method)
{
	switch (Method)
	{
	case ESendLevelControlMethod::Linear:      return "linear";
	case ESendLevelControlMethod::CustomCurve: return "custom_curve";
	case ESendLevelControlMethod::Manual:      return "manual";
	default:                                    return "manual";
	}
}

static const char* BusSendControlToString(ESourceBusSendLevelControlMethod Method)
{
	switch (Method)
	{
	case ESourceBusSendLevelControlMethod::Linear:      return "linear";
	case ESourceBusSendLevelControlMethod::CustomCurve: return "custom_curve";
	case ESourceBusSendLevelControlMethod::Manual:      return "manual";
	default:                                             return "manual";
	}
}

// ============================================================================
// BINDING
// ============================================================================

static TArray<FLuaFunctionDoc> SoundCueDocs = {};

static void BindSoundCue(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_sound_cue", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = NeoLuaStr::ToFString(PathStr);
		USoundCue* Cue = NeoLuaAsset::Resolve<USoundCue>(FPath);
		if (!Cue) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"SoundCue enrichment methods:\n"
			"\n"
			"info() — structured summary:\n"
			"  volume, pitch, priority, virtualization_mode, sound_submix,\n"
			"  is_looping, duration, max_distance, sound_class,\n"
			"  node_count, first_node_class, override_attenuation, subtitle_priority, has_graph,\n"
			"  attenuation_settings, source_effect_chain, enable_bus_sends, enable_base_submix,\n"
			"  enable_submix_sends, bypass_volume_scale_for_priority, concurrency_count,\n"
			"  override_concurrency, max_concurrent_count\n"
			"  When override_attenuation=true, includes attenuation_overrides table:\n"
			"    attenuate, spatialize, shape, shape_extents, falloff_distance, db_attenuation_at_max,\n"
			"    distance_algorithm, lpf_enabled, lpf_radius_min/max, lpf_freq_min/max,\n"
			"    hpf_freq_min/max, occlusion, reverb_send, spatialization_algorithm\n"
			"\n"
			"list(type):\n"
			"  list(\"nodes\") — all sound nodes: {index, class, name, child_count, children, sound_wave, node_pos_x, node_pos_y, input_pins}\n"
			"  list(\"concurrency\") — concurrency set: {name, path}\n"
			"  list(\"submix_sends\") — submix sends: {submix, send_level, control_method, stage, ...}\n"
			"  list(\"bus_sends\") — post-effect bus sends: {source_bus, audio_bus, send_level, control_method, ...}\n"
			"  list(\"pre_effect_bus_sends\") — pre-effect bus sends: {source_bus, audio_bus, send_level, control_method, ...}\n"
			"\n"
			"Set reflected properties with generic set():\n"
			"  set(\"VolumeMultiplier\", 1.5); set(\"PitchMultiplier\", 0.8)\n"
			"  set(\"bOverrideAttenuation\", true); set(\"bPrimeOnLoad\", true)\n"
			"  set(\"bExcludeFromRandomNodeBranchCulling\", true)\n"
			"  set(\"SoundClassObject\", \"/Path/To/SoundClass\")\n"
			"  set(\"VirtualizationMode\", \"PlayWhenSilent|Restart|Disabled|SeekRestart\")\n"
			"  set(\"AttenuationSettings\", \"/Path/To/SoundAttenuation\")\n"
			"  set(\"SourceEffectChain\", \"/Path/To/Chain\")\n"
			"  set(\"bEnableBusSends\", true); set(\"bEnableBaseSubmix\", true)\n"
			"  set(\"bEnableSubmixSends\", true); set(\"bBypassVolumeScaleForPriority\", false)\n"
			"  set(\"bOverrideConcurrency\", true); set(\"ConcurrencyOverrides.MaxCount\", 4)\n"
			"  set(\"ConcurrencySet\", {\"/Path/To/ConcurrencyAsset.Asset\"})\n"
			"  set_add(\"ConcurrencySet\", \"/Path/To/ConcurrencyAsset.Asset\")\n"
			"  set_remove(\"ConcurrencySet\", \"/Path/To/ConcurrencyAsset.Asset\")\n"
			"  set(\"AttenuationOverrides.bSpatialize\", true)\n"
			"  set(\"AttenuationOverrides.bAttenuate\", true)\n"
			"  set(\"AttenuationOverrides.FalloffDistance\", 2000)\n"
			"\n"
			"Graph editing:\n"
			"  Use read_graph() + graph editing tools (add_node, connect, etc.)\n"
			"  SoundCue graphs are now supported by the graph resolver.\n";

		// ================================================================
		// info()
		// ================================================================
		AssetObj.set_function("info", [Cue, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(Cue))
			{
				Session.Log(TEXT("[FAIL] info -> asset no longer valid"));
				return sol::lua_nil;
			}

			sol::table Result = Lua.create_table();

			Result["volume"] = Cue->VolumeMultiplier;
			Result["pitch"] = Cue->PitchMultiplier;
			Result["is_looping"] = Cue->IsLooping();
			Result["duration"] = Cue->GetDuration();
			Result["max_distance"] = Cue->GetMaxDistance();

			// Sound class (on USoundBase)
			USoundClass* SoundClass = Cue->SoundClassObject;
			Result["sound_class"] = SoundClass ? TCHAR_TO_UTF8(*SoundClass->GetPathName()) : "None";

			// Node info (editor-only)
			Result["node_count"] = GetSoundCueNodeCount(Cue);
			Result["has_graph"] = HasSoundCueGraph(Cue);

			// FirstNode (not editor-only)
			USoundNode* First = Cue->FirstNode;
			Result["first_node_class"] = First ? TCHAR_TO_UTF8(*First->GetClass()->GetName()) : "None";

			Result["override_attenuation"] = static_cast<bool>(Cue->bOverrideAttenuation);

			// Attenuation overrides detail (only when override is enabled)
			if (Cue->bOverrideAttenuation)
			{
				Result["attenuation_overrides"] = BuildAttenuationTable(Lua, Cue->AttenuationOverrides);
			}

			// Attenuation settings asset (when NOT overriding)
			if (Cue->AttenuationSettings)
			{
				Result["attenuation_settings"] = TCHAR_TO_UTF8(*Cue->AttenuationSettings->GetPathName());
			}

			// SubtitlePriority is protected — use the public getter
			Result["subtitle_priority"] = Cue->GetSubtitlePriority();

			Result["prime_on_load"] = static_cast<bool>(Cue->bPrimeOnLoad);

			// Priority (from USoundBase, 0-100)
			Result["priority"] = Cue->Priority;

			// VirtualizationMode (includes SeekRestart for 5.7)
			Result["virtualization_mode"] = VirtualizationModeToString(Cue->VirtualizationMode);

			// Sound submix
			if (Cue->SoundSubmixObject)
				Result["sound_submix"] = TCHAR_TO_UTF8(*Cue->SoundSubmixObject->GetPathName());

			// Source effect chain
			if (Cue->SourceEffectChain)
				Result["source_effect_chain"] = TCHAR_TO_UTF8(*Cue->SourceEffectChain->GetPathName());

			// Enable flags (from USoundBase)
			Result["enable_bus_sends"] = static_cast<bool>(Cue->bEnableBusSends);
			Result["enable_base_submix"] = static_cast<bool>(Cue->bEnableBaseSubmix);
			Result["enable_submix_sends"] = static_cast<bool>(Cue->bEnableSubmixSends);
			Result["bypass_volume_scale_for_priority"] = static_cast<bool>(Cue->bBypassVolumeScaleForPriority);

			// Concurrency
			Result["override_concurrency"] = static_cast<bool>(Cue->bOverrideConcurrency);
			if (Cue->bOverrideConcurrency)
			{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
				Result["max_concurrent_count"] = Cue->ConcurrencyOverrides.GetMaxCount();
				Result["raw_max_concurrent_count"] = Cue->ConcurrencyOverrides.MaxCount;
#else
				Result["max_concurrent_count"] = Cue->ConcurrencyOverrides.MaxCount;
#endif
			}
			Result["concurrency_count"] = Cue->ConcurrencySet.Num();

			// Send counts
			Result["submix_send_count"] = Cue->SoundSubmixSends.Num();
			Result["bus_send_count"] = Cue->BusSends.Num();
			Result["pre_effect_bus_send_count"] = Cue->PreEffectBusSends.Num();

			const int32 NodeCount = GetSoundCueNodeCount(Cue);
			Session.Log(FString::Printf(TEXT("[OK] info() -> SoundCue, %d nodes, vol=%.2f, pitch=%.2f"),
				NodeCount, (double)Cue->VolumeMultiplier, (double)Cue->PitchMultiplier));
			return Result;
		});

		// ================================================================
		// list(type?)
		// ================================================================
		AssetObj.set_function("list", [Cue, &Session](sol::table Self,
			sol::optional<std::string> TypeOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = NeoLuaStr::ToFStringOpt(TypeOpt, TEXT("all"));

			if (FType.Equals(TEXT("all"), ESearchCase::IgnoreCase))
			{
				sol::protected_function InfoFn = Self["info"];
				if (InfoFn.valid()) return InfoFn(Self);
				return sol::lua_nil;
			}

			if (!IsValid(Cue))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> asset no longer valid"), *FType));
				return sol::lua_nil;
			}

			// ---- nodes ----
			if (FType.Equals(TEXT("nodes"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("node"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				int32 NodeCount = 0;
				if (!BuildSoundCueNodeList(Cue, Lua, Result, NodeCount))
				{
					Session.Log(TEXT("[FAIL] list(\"nodes\") -> not available in non-editor builds"));
					return sol::lua_nil;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"nodes\") -> %d"), NodeCount));
				return Result;
			}

			// ---- concurrency ----
			if (FType.Equals(TEXT("concurrency"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				for (const TObjectPtr<USoundConcurrency>& Conc : Cue->ConcurrencySet)
				{
					if (!Conc) continue;
					sol::table E = Lua.create_table();
					E["name"] = TCHAR_TO_UTF8(*Conc->GetName());
					E["path"] = TCHAR_TO_UTF8(*Conc->GetPathName());
	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
				E["max_count"] = Conc->Concurrency.GetMaxCount();
#else
				E["max_count"] = Conc->Concurrency.MaxCount;
#endif
					Result[Idx++] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"concurrency\") -> %d"), Idx - 1));
				return Result;
			}

			// ---- submix_sends ----
			if (FType.Equals(TEXT("submix_sends"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Cue->SoundSubmixSends.Num(); i++)
				{
					const FSoundSubmixSendInfo& Send = Cue->SoundSubmixSends[i];
					sol::table E = Lua.create_table();
					E["index"] = i + 1;
					E["submix"] = Send.SoundSubmix ? TCHAR_TO_UTF8(*Send.SoundSubmix->GetPathName()) : "None";
					E["send_level"] = Send.SendLevel;
					E["control_method"] = SendLevelControlToString(Send.SendLevelControlMethod);
					E["stage"] = SubmixSendStageToString(Send.SendStage);
					E["min_send_level"] = Send.MinSendLevel;
					E["max_send_level"] = Send.MaxSendLevel;
					E["min_send_distance"] = Send.MinSendDistance;
					E["max_send_distance"] = Send.MaxSendDistance;
					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"submix_sends\") -> %d"), Cue->SoundSubmixSends.Num()));
				return Result;
			}

			// ---- bus_sends / pre_effect_bus_sends ----
			if (FType.Equals(TEXT("bus_sends"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("pre_effect_bus_sends"), ESearchCase::IgnoreCase))
			{
				const bool bPreEffect = FType.Equals(TEXT("pre_effect_bus_sends"), ESearchCase::IgnoreCase);
				const TArray<FSoundSourceBusSendInfo>& Sends = bPreEffect ? Cue->PreEffectBusSends : Cue->BusSends;

				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Sends.Num(); i++)
				{
					const FSoundSourceBusSendInfo& Send = Sends[i];
					sol::table E = Lua.create_table();
					E["index"] = i + 1;
					if (Send.SoundSourceBus)
						E["source_bus"] = TCHAR_TO_UTF8(*Send.SoundSourceBus->GetPathName());
					if (Send.AudioBus)
						E["audio_bus"] = TCHAR_TO_UTF8(*Send.AudioBus->GetPathName());
					E["send_level"] = Send.SendLevel;
					E["control_method"] = BusSendControlToString(Send.SourceBusSendLevelControlMethod);
					E["min_send_level"] = Send.MinSendLevel;
					E["max_send_level"] = Send.MaxSendLevel;
					E["min_send_distance"] = Send.MinSendDistance;
					E["max_send_distance"] = Send.MaxSendDistance;
					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"%s\") -> %d"), *FType, Sends.Num()));
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: nodes, concurrency, submix_sends, bus_sends, pre_effect_bus_sends"), *FType));
			return sol::lua_nil;
		});

		// configure(params) removed 2026-04-20 — every cue-level property
		// (VolumeMultiplier, PitchMultiplier, bOverrideAttenuation, bPrimeOnLoad,
		// bExcludeFromRandomNodeBranchCulling, SoundClassObject, SoundSubmixObject,
		// AttenuationOverrides.*, ConcurrencyOverrides.*, etc.) is a plain UPROPERTY.
		// Agents use the generic asset:set("PropName", value) path. Engine
		// USoundCue::PostEditChangeProperty handles any required cache invalidation.
	});
}

REGISTER_LUA_BINDING(SoundCue, SoundCueDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindSoundCue(Lua, Session);
});
