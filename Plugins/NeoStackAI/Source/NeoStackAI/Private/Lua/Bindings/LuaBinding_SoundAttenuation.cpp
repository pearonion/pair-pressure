// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Lua/LuaAssetResolver.h"
#include "Lua/LuaStr.h"
#include "Tools/NeoStackToolUtils.h"
#include "ScopedTransaction.h"

#include "Sound/SoundAttenuation.h"
#include "IAudioExtensionPlugin.h"
#include "Engine/CollisionProfile.h"
#include "Sound/SoundSubmix.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// HELPERS
// ============================================================================

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

static const char* DistanceModelToString(EAttenuationDistanceModel Model)
{
	switch (Model)
	{
	case EAttenuationDistanceModel::Linear:       return "linear";
	case EAttenuationDistanceModel::Logarithmic:  return "logarithmic";
	case EAttenuationDistanceModel::Inverse:      return "inverse";
	case EAttenuationDistanceModel::LogReverse:    return "log_reverse";
	case EAttenuationDistanceModel::NaturalSound:  return "natural_sound";
	case EAttenuationDistanceModel::Custom:        return "custom";
	default:                                       return "linear";
	}
}

static const char* FalloffModeToString(ENaturalSoundFalloffMode Mode)
{
	switch (Mode)
	{
	case ENaturalSoundFalloffMode::Continues: return "continues";
	case ENaturalSoundFalloffMode::Silent:    return "silent";
	case ENaturalSoundFalloffMode::Hold:      return "hold";
	default:                                  return "continues";
	}
}

static const char* SpatializationAlgorithmToString(ESoundSpatializationAlgorithm Algo)
{
	switch (Algo)
	{
	case SPATIALIZATION_Default: return "panning";
	case SPATIALIZATION_HRTF:    return "hrtf";
	default:                     return "panning";
	}
}

static const char* AbsorptionMethodToString(EAirAbsorptionMethod Method)
{
	switch (Method)
	{
	case EAirAbsorptionMethod::Linear:      return "linear";
	case EAirAbsorptionMethod::CustomCurve: return "custom_curve";
	default:                                return "linear";
	}
}

static const char* ReverbSendMethodToString(EReverbSendMethod Method)
{
	switch (Method)
	{
	case EReverbSendMethod::Linear:      return "linear";
	case EReverbSendMethod::CustomCurve: return "custom_curve";
	case EReverbSendMethod::Manual:      return "manual";
	default:                             return "linear";
	}
}

static const char* PriorityAttenuationMethodToString(EPriorityAttenuationMethod Method)
{
	switch (Method)
	{
	case EPriorityAttenuationMethod::Linear:      return "linear";
	case EPriorityAttenuationMethod::CustomCurve: return "custom_curve";
	case EPriorityAttenuationMethod::Manual:      return "manual";
	default:                                      return "linear";
	}
}

static const char* NonSpatRadiusModeToString(ENonSpatializedRadiusSpeakerMapMode Mode)
{
	switch (Mode)
	{
	case ENonSpatializedRadiusSpeakerMapMode::OmniDirectional: return "omni_directional";
	case ENonSpatializedRadiusSpeakerMapMode::Direct2D:        return "direct_2d";
	case ENonSpatializedRadiusSpeakerMapMode::Surround2D:      return "surround_2d";
	default:                                                    return "omni_directional";
	}
}

static const char* SendLevelControlMethodToString(ESendLevelControlMethod Method)
{
	switch (Method)
	{
	case ESendLevelControlMethod::Linear:      return "linear";
	case ESendLevelControlMethod::CustomCurve: return "custom_curve";
	case ESendLevelControlMethod::Manual:      return "manual";
	default:                                   return "linear";
	}
}

static FString OcclusionTraceChannelToString(ECollisionChannel Channel)
{
	switch (Channel)
	{
	case ECC_Visibility:    return TEXT("visibility");
	case ECC_Camera:        return TEXT("camera");
	case ECC_WorldStatic:   return TEXT("world_static");
	case ECC_WorldDynamic:  return TEXT("world_dynamic");
	case ECC_Pawn:          return TEXT("pawn");
	case ECC_PhysicsBody:   return TEXT("physics_body");
	case ECC_Vehicle:       return TEXT("vehicle");
	case ECC_Destructible:  return TEXT("destructible");
	default:
		if (const UCollisionProfile* CollisionProfile = UCollisionProfile::Get())
		{
			const FName ChannelName = CollisionProfile->ReturnChannelNameFromContainerIndex(static_cast<int32>(Channel));
			if (!ChannelName.IsNone())
			{
				return ChannelName.ToString();
			}
		}
		if (const UEnum* ChannelEnum = StaticEnum<ECollisionChannel>())
		{
			const FString EnumName = ChannelEnum->GetNameStringByValue(static_cast<int64>(Channel));
			if (!EnumName.IsEmpty())
			{
				return EnumName;
			}
		}
		return FString::Printf(TEXT("ECollisionChannel(%d)"), static_cast<int32>(Channel));
	}
}

static sol::table ObjectRefToTable(sol::state_view Lua, const UObject* Object)
{
	sol::table T = Lua.create_table();
	T["is_set"] = Object != nullptr;
	T["path"] = Object ? TCHAR_TO_UTF8(*Object->GetPathName()) : "";
	T["class_name"] = Object ? TCHAR_TO_UTF8(*Object->GetClass()->GetName()) : "";
	return T;
}

template <typename UObjectType>
static sol::table ObjectArrayToTable(sol::state_view Lua, const TArray<TObjectPtr<UObjectType>>& Objects)
{
	sol::table T = Lua.create_table();
	int32 Index = 1;
	for (const TObjectPtr<UObjectType>& Object : Objects)
	{
		T[Index++] = ObjectRefToTable(Lua, Object.Get());
	}
	return T;
}

static sol::table RuntimeFloatCurveToTable(sol::state_view Lua, const FRuntimeFloatCurve& Curve)
{
	sol::table T = Lua.create_table();
	const UCurveFloat* ExternalCurve = Curve.ExternalCurve.Get();
	T["has_external_curve"] = ExternalCurve != nullptr;
	T["external_curve_path"] = ExternalCurve ? TCHAR_TO_UTF8(*ExternalCurve->GetPathName()) : "";
	T["external_curve_class_name"] = ExternalCurve ? TCHAR_TO_UTF8(*ExternalCurve->GetClass()->GetName()) : "";
	T["external_curve"] = ObjectRefToTable(Lua, ExternalCurve);

	sol::table Keys = Lua.create_table();
	if (const FRichCurve* RichCurve = Curve.GetRichCurveConst())
	{
		const TArray<FRichCurveKey>& RichKeys = RichCurve->GetConstRefOfKeys();
		T["key_count"] = RichKeys.Num();
		for (int32 Index = 0; Index < RichKeys.Num(); ++Index)
		{
			const FRichCurveKey& Key = RichKeys[Index];
			sol::table KeyTable = Lua.create_table();
			KeyTable["time"] = Key.Time;
			KeyTable["value"] = Key.Value;
			Keys[Index + 1] = KeyTable;
		}
	}
	else
	{
		T["key_count"] = 0;
	}
	T["keys"] = Keys;
	return T;
}

// ============================================================================
// BINDING
// ============================================================================

static TArray<FLuaFunctionDoc> SoundAttenuationDocs = {};

static void BindSoundAttenuation(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_sound_attenuation", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = NeoLuaStr::ToFString(PathStr);
		USoundAttenuation* Asset = NeoLuaAsset::Resolve<USoundAttenuation>(FPath);
		if (!Asset) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"SoundAttenuation enrichment methods:\n"
			"\n"
			"info() — structured summary of all attenuation settings (snake_case keys)\n"
			"\n"
			"All mutations go through the generic asset:set(\"Attenuation.PropName\", value)\n"
			"path (UPROPERTY names, not snake_case). ImportText handles enums by authored\n"
			"name. Call list_properties(\"Attenuation\") to discover field names, or use\n"
			"info() to see current values (with snake_case aliases from ToString helpers).\n"
			"\n"
			"Examples:\n"
			"  asset:set(\"Attenuation.AttenuationShape\", \"Box\")\n"
			"  asset:set(\"Attenuation.FalloffDistance\", 2000.0)\n"
			"  asset:set(\"Attenuation.DistanceAlgorithm\", \"ATTENUATION_Inverse\")\n"
			"  asset:set(\"Attenuation.bSpatialize\", true)\n"
			"  asset:set(\"Attenuation.AttenuationShapeExtents\", {x=400, y=400, z=400})\n";

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

			const FSoundAttenuationSettings& A = Asset->Attenuation;
			sol::table R = Lua.create_table();

			// Shape & distance
			R["shape"] = AttenuationShapeToString(A.AttenuationShape);
			sol::table Extents = Lua.create_table();
			Extents["x"] = A.AttenuationShapeExtents.X;
			Extents["y"] = A.AttenuationShapeExtents.Y;
			Extents["z"] = A.AttenuationShapeExtents.Z;
			R["shape_extents"] = Extents;
			R["falloff_distance"] = A.FalloffDistance;
			R["distance_algorithm"] = DistanceModelToString(A.DistanceAlgorithm);
			R["falloff_mode"] = FalloffModeToString(A.FalloffMode);
			R["db_attenuation_at_max"] = A.dBAttenuationAtMax;
			R["cone_offset"] = A.ConeOffset;
			R["cone_sphere_radius"] = A.ConeSphereRadius;
			R["cone_sphere_falloff_distance"] = A.ConeSphereFalloffDistance;
			R["custom_attenuation_curve"] = RuntimeFloatCurveToTable(Lua, A.CustomAttenuationCurve);

			// Attenuation & spatialization
			R["attenuate"] = static_cast<bool>(A.bAttenuate);
			R["spatialize"] = static_cast<bool>(A.bSpatialize);
			R["spatialization_algorithm"] = SpatializationAlgorithmToString(A.SpatializationAlgorithm);
			R["binaural_radius"] = A.BinauralRadius;
			R["stereo_spread"] = A.StereoSpread;
			R["non_spatialized_radius_start"] = A.NonSpatializedRadiusStart;
			R["non_spatialized_radius_end"] = A.NonSpatializedRadiusEnd;
			R["normalize_stereo"] = static_cast<bool>(A.bApplyNormalizationToStereoSounds);
			R["non_spatialized_radius_mode"] = NonSpatRadiusModeToString(A.NonSpatializedRadiusMode);

			// Listener focus
			R["enable_listener_focus"] = static_cast<bool>(A.bEnableListenerFocus);
			R["focus_azimuth"] = A.FocusAzimuth;
			R["non_focus_azimuth"] = A.NonFocusAzimuth;
			R["focus_distance_scale"] = A.FocusDistanceScale;
			R["non_focus_distance_scale"] = A.NonFocusDistanceScale;
			R["focus_priority_scale"] = A.FocusPriorityScale;
			R["non_focus_priority_scale"] = A.NonFocusPriorityScale;
			R["focus_volume_attenuation"] = A.FocusVolumeAttenuation;
			R["non_focus_volume_attenuation"] = A.NonFocusVolumeAttenuation;
			R["enable_focus_interpolation"] = static_cast<bool>(A.bEnableFocusInterpolation);
			R["focus_attack_interp_speed"] = A.FocusAttackInterpSpeed;
			R["focus_release_interp_speed"] = A.FocusReleaseInterpSpeed;

			// Air absorption / LPF / HPF
			R["air_absorption"] = static_cast<bool>(A.bAttenuateWithLPF);
			R["absorption_method"] = AbsorptionMethodToString(A.AbsorptionMethod);
			R["lpf_radius_min"] = A.LPFRadiusMin;
			R["lpf_radius_max"] = A.LPFRadiusMax;
			R["lpf_frequency_at_min"] = A.LPFFrequencyAtMin;
			R["lpf_frequency_at_max"] = A.LPFFrequencyAtMax;
			R["hpf_frequency_at_min"] = A.HPFFrequencyAtMin;
			R["hpf_frequency_at_max"] = A.HPFFrequencyAtMax;
			R["enable_log_frequency_scaling"] = static_cast<bool>(A.bEnableLogFrequencyScaling);
			R["custom_lowpass_air_absorption_curve"] = RuntimeFloatCurveToTable(Lua, A.CustomLowpassAirAbsorptionCurve);
			R["custom_highpass_air_absorption_curve"] = RuntimeFloatCurveToTable(Lua, A.CustomHighpassAirAbsorptionCurve);

			// Occlusion
			R["enable_occlusion"] = static_cast<bool>(A.bEnableOcclusion);
			R["use_complex_collision_for_occlusion"] = static_cast<bool>(A.bUseComplexCollisionForOcclusion);
			const ECollisionChannel OcclusionChannel = static_cast<ECollisionChannel>(A.OcclusionTraceChannel.GetValue());
			const FString OcclusionChannelName = OcclusionTraceChannelToString(OcclusionChannel);
			R["occlusion_trace_channel"] = TCHAR_TO_UTF8(*OcclusionChannelName);
			R["occlusion_trace_channel_value"] = static_cast<int>(OcclusionChannel);
			R["occlusion_lpf_frequency"] = A.OcclusionLowPassFilterFrequency;
			R["occlusion_volume_attenuation"] = A.OcclusionVolumeAttenuation;
			R["occlusion_interpolation_time"] = A.OcclusionInterpolationTime;

			// Reverb send
			R["enable_reverb_send"] = static_cast<bool>(A.bEnableReverbSend);
			R["reverb_send_method"] = ReverbSendMethodToString(A.ReverbSendMethod);
			R["reverb_wet_level_min"] = A.ReverbWetLevelMin;
			R["reverb_wet_level_max"] = A.ReverbWetLevelMax;
			R["reverb_distance_min"] = A.ReverbDistanceMin;
			R["reverb_distance_max"] = A.ReverbDistanceMax;
			R["manual_reverb_send_level"] = A.ManualReverbSendLevel;
			R["custom_reverb_send_curve"] = RuntimeFloatCurveToTable(Lua, A.CustomReverbSendCurve);

			// Submix send
			R["enable_submix_sends"] = static_cast<bool>(A.bEnableSubmixSends);
			R["submix_send_settings_count"] = A.SubmixSendSettings.Num();
			sol::table SubmixSends = Lua.create_table();
			for (int32 Index = 0; Index < A.SubmixSendSettings.Num(); ++Index)
			{
				const FAttenuationSubmixSendSettings& Send = A.SubmixSendSettings[Index];
				sol::table SendTable = Lua.create_table();
				SendTable["send_level_control_method"] = SendLevelControlMethodToString(Send.SendLevelControlMethod);
				SendTable["sound_submix"] = ObjectRefToTable(Lua, Send.SoundSubmix.Get());
				SendTable["send_level"] = Send.SendLevel;
				SendTable["disable_manual_send_clamp"] = Send.DisableManualSendClamp;
				SendTable["min_send_level"] = Send.MinSendLevel;
				SendTable["max_send_level"] = Send.MaxSendLevel;
				SendTable["min_send_distance"] = Send.MinSendDistance;
				SendTable["max_send_distance"] = Send.MaxSendDistance;
				SendTable["custom_send_level_curve"] = RuntimeFloatCurveToTable(Lua, Send.CustomSendLevelCurve);
				SubmixSends[Index + 1] = SendTable;
			}
			R["submix_send_settings"] = SubmixSends;

			// Priority attenuation
			R["enable_priority_attenuation"] = static_cast<bool>(A.bEnablePriorityAttenuation);
			R["priority_attenuation_method"] = PriorityAttenuationMethodToString(A.PriorityAttenuationMethod);
			R["priority_attenuation_min"] = A.PriorityAttenuationMin;
			R["priority_attenuation_max"] = A.PriorityAttenuationMax;
			R["priority_attenuation_distance_min"] = A.PriorityAttenuationDistanceMin;
			R["priority_attenuation_distance_max"] = A.PriorityAttenuationDistanceMax;
			R["manual_priority_attenuation"] = A.ManualPriorityAttenuation;
			R["custom_priority_attenuation_curve"] = RuntimeFloatCurveToTable(Lua, A.CustomPriorityAttenuationCurve);

			// Plugin, source-data, and AudioLink settings
			R["enable_source_data_override"] = static_cast<bool>(A.bEnableSourceDataOverride);
			R["enable_send_to_audio_link"] = static_cast<bool>(A.bEnableSendToAudioLink);
			R["audio_link_settings_override"] = ObjectRefToTable(Lua, A.AudioLinkSettingsOverride.Get());

			const FSoundAttenuationPluginSettings& PluginSettings = A.PluginSettings;
			sol::table SpatializationPluginSettings = ObjectArrayToTable(Lua, PluginSettings.SpatializationPluginSettingsArray);
			sol::table OcclusionPluginSettings = ObjectArrayToTable(Lua, PluginSettings.OcclusionPluginSettingsArray);
			sol::table ReverbPluginSettings = ObjectArrayToTable(Lua, PluginSettings.ReverbPluginSettingsArray);
			sol::table SourceDataOverridePluginSettings = ObjectArrayToTable(Lua, PluginSettings.SourceDataOverridePluginSettingsArray);
			R["spatialization_plugin_settings_count"] = PluginSettings.SpatializationPluginSettingsArray.Num();
			R["spatialization_plugin_settings"] = SpatializationPluginSettings;
			R["occlusion_plugin_settings_count"] = PluginSettings.OcclusionPluginSettingsArray.Num();
			R["occlusion_plugin_settings"] = OcclusionPluginSettings;
			R["reverb_plugin_settings_count"] = PluginSettings.ReverbPluginSettingsArray.Num();
			R["reverb_plugin_settings"] = ReverbPluginSettings;
			R["source_data_override_plugin_settings_count"] = PluginSettings.SourceDataOverridePluginSettingsArray.Num();
			R["source_data_override_plugin_settings"] = SourceDataOverridePluginSettings;

			sol::table PluginSettingsTable = Lua.create_table();
			PluginSettingsTable["spatialization_plugins_count"] = PluginSettings.SpatializationPluginSettingsArray.Num();
			PluginSettingsTable["spatialization_plugins"] = SpatializationPluginSettings;
			PluginSettingsTable["occlusion_plugins_count"] = PluginSettings.OcclusionPluginSettingsArray.Num();
			PluginSettingsTable["occlusion_plugins"] = OcclusionPluginSettings;
			PluginSettingsTable["reverb_plugins_count"] = PluginSettings.ReverbPluginSettingsArray.Num();
			PluginSettingsTable["reverb_plugins"] = ReverbPluginSettings;
			PluginSettingsTable["source_data_override_plugins_count"] = PluginSettings.SourceDataOverridePluginSettingsArray.Num();
			PluginSettingsTable["source_data_override_plugins"] = SourceDataOverridePluginSettings;
			R["plugin_settings"] = PluginSettingsTable;

			Session.Log(FString::Printf(TEXT("[OK] info() -> SoundAttenuation, shape=%hs, falloff=%.0f, spatialize=%s"),
				AttenuationShapeToString(A.AttenuationShape),
				(double)A.FalloffDistance,
				A.bSpatialize ? TEXT("true") : TEXT("false")));
			return R;
		});

		// configure(params) removed 2026-04-20 — every field on
		// FSoundAttenuationSettings is a plain UPROPERTY, so agents should use
		// asset:set("Attenuation.PropName", value) through the generic path in
		// LuaBinding_OpenAsset.cpp. ImportText handles enum-by-authored-name,
		// structs, and float fields out of the box.
	});
}

REGISTER_LUA_BINDING(SoundAttenuation, SoundAttenuationDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindSoundAttenuation(Lua, Session);
});
