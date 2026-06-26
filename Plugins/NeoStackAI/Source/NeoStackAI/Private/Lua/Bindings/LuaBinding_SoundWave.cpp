// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Lua/LuaAssetResolver.h"
#include "Lua/LuaStr.h"
#include "ScopedTransaction.h"

#include "Sound/SoundWave.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundGroups.h"
#include "Engine/EngineTypes.h"
#include "EditorFramework/AssetImportData.h"
#include "IWaveformTransformation.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// VERSION COMPAT HELPERS
// ============================================================================

static TArray<FSoundWaveCuePoint> NSAI_GetSoundWaveCuePoints(const USoundWave* Wave)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	return Wave->GetSoundWaveCuePoints();
#else
	return Wave->GetCuePoints();
#endif
}

static void NSAI_SetSoundWaveCuePoints(USoundWave* Wave, const TArray<FSoundWaveCuePoint>& InCuePoints)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	Wave->SetSoundWaveCuePoints(InCuePoints);
#else
	FProperty* Prop = USoundWave::StaticClass()->FindPropertyByName(TEXT("CuePoints"));
	if (Prop)
	{
		TArray<FSoundWaveCuePoint>* CuePointsPtr = Prop->ContainerPtrToValuePtr<TArray<FSoundWaveCuePoint>>(Wave);
		if (CuePointsPtr)
		{
			*CuePointsPtr = InCuePoints;
		}
	}
#endif
}

// ============================================================================
// HELPERS
// ============================================================================

static const char* SoundGroupToString(ESoundGroup Group)
{
	switch (Group)
	{
	case SOUNDGROUP_Default: return "default";
	case SOUNDGROUP_Effects: return "effects";
	case SOUNDGROUP_UI:      return "ui";
	case SOUNDGROUP_Music:   return "music";
	case SOUNDGROUP_Voice:   return "voice";
	default:
		// GameSoundGroup1-20 — return numeric identifier
		{
			static thread_local char Buf[32];
			FCStringAnsi::Snprintf(Buf, sizeof(Buf), "game_sound_group_%d", static_cast<int>(Group) - static_cast<int>(SOUNDGROUP_GameSoundGroup1) + 1);
			return Buf;
		}
	}
}

namespace SoundWaveHelpers {
static const char* LoadingBehaviorToString(ESoundWaveLoadingBehavior Behavior)
{
	switch (Behavior)
	{
	case ESoundWaveLoadingBehavior::Inherited:    return "inherited";
	case ESoundWaveLoadingBehavior::RetainOnLoad: return "retain_on_load";
	case ESoundWaveLoadingBehavior::PrimeOnLoad:  return "prime_on_load";
	case ESoundWaveLoadingBehavior::LoadOnDemand: return "load_on_demand";
	case ESoundWaveLoadingBehavior::ForceInline:  return "force_inline";
	default:                                      return "inherited";
	}
}
} // namespace SoundWaveHelpers

static const char* CompressionTypeToString(ESoundAssetCompressionType Type)
{
	switch (Type)
	{
	case ESoundAssetCompressionType::BinkAudio:        return "bink_audio";
	case ESoundAssetCompressionType::ADPCM:            return "adpcm";
	case ESoundAssetCompressionType::PCM:              return "pcm";
	case ESoundAssetCompressionType::Opus:             return "opus";
	case ESoundAssetCompressionType::PlatformSpecific: return "platform_specific";
	case ESoundAssetCompressionType::ProjectDefined:   return "project_defined";
	case ESoundAssetCompressionType::RADAudio:         return "rad_audio";
	default:                                           return "platform_specific";
	}
}

// ============================================================================
// BINDING
// ============================================================================

static TArray<FLuaFunctionDoc> SoundWaveDocs = {};

static void BindSoundWave(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_sound_wave", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = NeoLuaStr::ToFString(PathStr);
		USoundWave* Wave = NeoLuaAsset::Resolve<USoundWave>(FPath);
		if (!Wave) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"SoundWave enrichment methods:\n"
			"\n"
			"info() — structured summary:\n"
			"  duration, sample_rate, num_channels, volume, pitch, is_looping, priority,\n"
			"  sound_group, compression_quality, compression_type, loading_behavior,\n"
			"  is_ambisonics, is_mature, subtitle_priority, import_path, sound_class,\n"
			"  lufs, sample_peak_db, manual_word_wrap, single_line, cue_point_count, loop_region_count\n"
			"\n"
			"Property editing uses the generic asset reflection API:\n"
			"  get(\"PropertyName\")\n"
			"  set(\"PropertyName\", \"Value\")\n"
			"  list_properties(filter?, all?)\n"
			"\n"
			"Use raw engine property names, e.g.:\n"
			"  Volume\n"
			"  Pitch\n"
			"  bLooping\n"
			"  Priority\n"
			"  SoundGroup\n"
			"  CompressionQuality\n"
			"  SoundAssetCompressionType\n"
			"  LoadingBehavior\n"
			"  bMature\n"
			"  SubtitlePriority\n"
			"  bIsAmbisonics\n"
			"  SoundClassObject\n"
			"  bManualWordWrap\n"
			"  bSingleLine\n"
			"\n"
			"list(type) — list sub-items:\n"
			"  'subtitles' — subtitle cues (text, time)\n"
			"  'cue_points' — cue points (label, frame_position, frame_length, is_loop_region)\n"
			"  'loop_regions' — loop region cue points only\n"
			"\n"
			"add(type, params) — add sub-items:\n"
			"  'subtitle', {text='...', time=0.0}\n"
			"  'cue_point', {label='...', frame_position=0, frame_length=0, is_loop_region=false}\n"
			"\n"
			"remove(type, params) — remove sub-items:\n"
			"  'subtitle', {index=N} — remove subtitle at 0-based index\n"
			"  'cue_point', {index=N} — remove cue point at 0-based index\n"
			"  'all_subtitles' — clear all subtitles\n"
			"  'all_cue_points' — clear all cue points\n";

		// ================================================================
		// info()
		// ================================================================
		AssetObj.set_function("info", [Wave, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(Wave))
			{
				Session.Log(TEXT("[FAIL] info -> asset no longer valid"));
				return sol::lua_nil;
			}

			sol::table R = Lua.create_table();

			R["duration"] = Wave->Duration;
			R["sample_rate"] = Wave->GetSampleRateForCurrentPlatform();
			R["num_channels"] = Wave->NumChannels;
			R["volume"] = Wave->Volume;
			R["pitch"] = Wave->Pitch;
			R["is_looping"] = static_cast<bool>(Wave->bLooping);
			R["priority"] = Wave->Priority;
			R["sound_group"] = SoundGroupToString(Wave->SoundGroup.GetValue());
			R["compression_quality"] = Wave->GetCompressionQuality();
			R["compression_type"] = CompressionTypeToString(Wave->GetSoundAssetCompressionTypeEnum());
			R["loading_behavior"] = SoundWaveHelpers::LoadingBehaviorToString(Wave->LoadingBehavior);
			R["is_ambisonics"] = static_cast<bool>(Wave->bIsAmbisonics);
			R["is_mature"] = static_cast<bool>(Wave->bMature);
			R["subtitle_priority"] = Wave->SubtitlePriority;
			R["subtitle_count"] = Wave->Subtitles.Num();
			R["total_samples"] = Wave->TotalSamples;
			R["manual_word_wrap"] = static_cast<bool>(Wave->bManualWordWrap);
			R["single_line"] = static_cast<bool>(Wave->bSingleLine);

			// Cue point / loop region counts
			TArray<FSoundWaveCuePoint> AllCuePoints = Wave->GetCuePoints();
			R["cue_point_count"] = AllCuePoints.Num();
			TArray<FSoundWaveCuePoint> LoopRegions = Wave->GetLoopRegions();
			R["loop_region_count"] = LoopRegions.Num();

			// Sound class reference
			if (Wave->SoundClassObject)
			{
				R["sound_class"] = std::string(TCHAR_TO_UTF8(*Wave->SoundClassObject->GetPathName()));
			}

			// Import path & loudness (editor-only)
#if WITH_EDITORONLY_DATA
			if (Wave->AssetImportData)
			{
				FString ImportPath = Wave->AssetImportData->GetFirstFilename();
				if (!ImportPath.IsEmpty())
				{
					R["import_path"] = std::string(TCHAR_TO_UTF8(*ImportPath));
				}
			}
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			R["lufs"] = Wave->LUFS;
			R["sample_peak_db"] = Wave->SamplePeakDB;
#endif
#endif

			Session.Log(FString::Printf(TEXT("[OK] info() -> SoundWave, duration=%.2fs, channels=%d, sample_rate=%d"),
				Wave->Duration, Wave->NumChannels, (int32)Wave->GetSampleRateForCurrentPlatform()));
			return R;
		});

		// ================================================================
		// list(type)
		// ================================================================
		AssetObj.set_function("list", [Wave, &Session](sol::table /*self*/,
			const std::string& ListType, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(Wave))
			{
				Session.Log(TEXT("[FAIL] list -> asset no longer valid"));
				return sol::lua_nil;
			}

			FString TypeStr = NeoLuaStr::ToFString(ListType);

			// ----- subtitles -----
			if (TypeStr.Equals(TEXT("subtitles"), ESearchCase::IgnoreCase))
			{
				sol::table Arr = Lua.create_table();
				for (int32 i = 0; i < Wave->Subtitles.Num(); i++)
				{
					const FSubtitleCue& Cue = Wave->Subtitles[i];
					sol::table Entry = Lua.create_table();
					Entry["index"] = i;
					Entry["text"] = std::string(TCHAR_TO_UTF8(*Cue.Text.ToString()));
					Entry["time"] = Cue.Time;
					Arr[i + 1] = Entry;
				}
				Session.Log(FString::Printf(TEXT("[OK] list('subtitles') -> %d entries"), Wave->Subtitles.Num()));
				return Arr;
			}

			// ----- cue_points -----
			if (TypeStr.Equals(TEXT("cue_points"), ESearchCase::IgnoreCase))
			{
				TArray<FSoundWaveCuePoint> CuePoints = Wave->GetCuePoints();
				sol::table Arr = Lua.create_table();
				for (int32 i = 0; i < CuePoints.Num(); i++)
				{
					const FSoundWaveCuePoint& CP = CuePoints[i];
					sol::table Entry = Lua.create_table();
					Entry["index"] = i;
					Entry["cue_point_id"] = CP.CuePointID;
					Entry["label"] = std::string(TCHAR_TO_UTF8(*CP.Label));
					Entry["frame_position"] = static_cast<double>(CP.FramePosition);
					Entry["frame_length"] = static_cast<double>(CP.FrameLength);
					Entry["is_loop_region"] = CP.IsLoopRegion();
					Arr[i + 1] = Entry;
				}
				Session.Log(FString::Printf(TEXT("[OK] list('cue_points') -> %d entries"), CuePoints.Num()));
				return Arr;
			}

			// ----- loop_regions -----
			if (TypeStr.Equals(TEXT("loop_regions"), ESearchCase::IgnoreCase))
			{
				TArray<FSoundWaveCuePoint> Regions = Wave->GetLoopRegions();
				sol::table Arr = Lua.create_table();
				for (int32 i = 0; i < Regions.Num(); i++)
				{
					const FSoundWaveCuePoint& CP = Regions[i];
					sol::table Entry = Lua.create_table();
					Entry["index"] = i;
					Entry["cue_point_id"] = CP.CuePointID;
					Entry["label"] = std::string(TCHAR_TO_UTF8(*CP.Label));
					Entry["frame_position"] = static_cast<double>(CP.FramePosition);
					Entry["frame_length"] = static_cast<double>(CP.FrameLength);
					Entry["is_loop_region"] = true;
					Arr[i + 1] = Entry;
				}
				Session.Log(FString::Printf(TEXT("[OK] list('loop_regions') -> %d entries"), Regions.Num()));
				return Arr;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list('%s') -> unknown type. Valid: subtitles, cue_points, loop_regions"), *TypeStr));
			return sol::lua_nil;
		});

		// ================================================================
		// add(type, params)
		// ================================================================
		AssetObj.set_function("add", [Wave, &Session](sol::table /*self*/,
			const std::string& AddType, sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(Wave))
			{
				Session.Log(TEXT("[FAIL] add -> asset no longer valid"));
				return sol::lua_nil;
			}

			FString TypeStr = NeoLuaStr::ToFString(AddType);

			// ----- subtitle -----
			if (TypeStr.Equals(TEXT("subtitle"), ESearchCase::IgnoreCase))
			{
				const FScopedTransaction Transaction(FText::FromString(TEXT("SoundWave: Add Subtitle")));
				Wave->Modify();

				FSubtitleCue NewCue;
				sol::optional<std::string> Text = Params.get<sol::optional<std::string>>("text");
				if (Text.has_value())
				{
					NewCue.Text = FText::FromString(NeoLuaStr::ToFStringOpt(Text));
				}
				NewCue.Time = static_cast<float>(Params.get_or("time", 0.0));

				Wave->Subtitles.Add(NewCue);

				FPropertyChangedEvent Event(nullptr, EPropertyChangeType::ValueSet);
				Wave->PostEditChangeProperty(Event);
				Wave->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] add('subtitle') -> index %d, text='%s', time=%.3f"),
					Wave->Subtitles.Num() - 1, *NewCue.Text.ToString(), NewCue.Time));
				return sol::make_object(Lua, Wave->Subtitles.Num() - 1);
			}

			// ----- cue_point -----
			if (TypeStr.Equals(TEXT("cue_point"), ESearchCase::IgnoreCase))
			{
#if WITH_EDITOR
				const FScopedTransaction Transaction(FText::FromString(TEXT("SoundWave: Add Cue Point")));
				Wave->Modify();

				// Get current cue points, add new one, set back
				TArray<FSoundWaveCuePoint> CuePoints = NSAI_GetSoundWaveCuePoints(Wave);

				FSoundWaveCuePoint NewCP;
				sol::optional<std::string> Label = Params.get<sol::optional<std::string>>("label");
				if (Label.has_value())
				{
					NewCP.Label = NeoLuaStr::ToFStringOpt(Label);
				}
				NewCP.FramePosition = static_cast<int64>(Params.get_or("frame_position", 0.0));
				NewCP.FrameLength = static_cast<int64>(Params.get_or("frame_length", 0.0));
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
				NewCP.SetLoopRegion(Params.get_or("is_loop_region", false));
#else
				// FSoundWaveCuePoint::SetLoopRegion was added in UE 5.6; older engines kept the
				// flag protected/private, so we leave the default (false) on 5.4/5.5.
#endif
				// CuePointID: assign next available
				int32 MaxID = -1;
				for (const FSoundWaveCuePoint& CP : CuePoints) { MaxID = FMath::Max(MaxID, CP.CuePointID); }
				NewCP.CuePointID = MaxID + 1;

				CuePoints.Add(NewCP);
				NSAI_SetSoundWaveCuePoints(Wave,CuePoints);

				Wave->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] add('cue_point') -> id=%d, label='%s', frame_pos=%lld"),
					NewCP.CuePointID, *NewCP.Label, NewCP.FramePosition));
				return sol::make_object(Lua, NewCP.CuePointID);
#else
				Session.Log(TEXT("[FAIL] add('cue_point') -> only available in editor builds"));
				return sol::lua_nil;
#endif
			}

			Session.Log(FString::Printf(TEXT("[FAIL] add('%s') -> unknown type. Valid: subtitle, cue_point"), *TypeStr));
			return sol::lua_nil;
		});

		// ================================================================
		// remove(type, params)
		// ================================================================
		AssetObj.set_function("remove", [Wave, &Session](sol::table /*self*/,
			const std::string& RemoveType, sol::optional<sol::table> OptParams, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(Wave))
			{
				Session.Log(TEXT("[FAIL] remove -> asset no longer valid"));
				return sol::lua_nil;
			}

			FString TypeStr = NeoLuaStr::ToFString(RemoveType);

			// ----- all_subtitles -----
			if (TypeStr.Equals(TEXT("all_subtitles"), ESearchCase::IgnoreCase))
			{
				const FScopedTransaction Transaction(FText::FromString(TEXT("SoundWave: Clear Subtitles")));
				Wave->Modify();
				int32 Count = Wave->Subtitles.Num();
				Wave->Subtitles.Empty();
				FPropertyChangedEvent Event(nullptr, EPropertyChangeType::ValueSet);
				Wave->PostEditChangeProperty(Event);
				Wave->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove('all_subtitles') -> cleared %d entries"), Count));
				return sol::make_object(Lua, true);
			}

			// ----- all_cue_points -----
			if (TypeStr.Equals(TEXT("all_cue_points"), ESearchCase::IgnoreCase))
			{
#if WITH_EDITOR
				const FScopedTransaction Transaction(FText::FromString(TEXT("SoundWave: Clear Cue Points")));
				Wave->Modify();
				TArray<FSoundWaveCuePoint> Empty;
				NSAI_SetSoundWaveCuePoints(Wave,Empty);
				Wave->MarkPackageDirty();
				Session.Log(TEXT("[OK] remove('all_cue_points') -> cleared"));
				return sol::make_object(Lua, true);
#else
				Session.Log(TEXT("[FAIL] remove('all_cue_points') -> only available in editor builds"));
				return sol::lua_nil;
#endif
			}

			// For indexed removals, params table is required
			if (!OptParams.has_value())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove('%s') -> params table required with 'index' key"), *TypeStr));
				return sol::lua_nil;
			}
			sol::table Params = OptParams.value();

			// ----- subtitle -----
			if (TypeStr.Equals(TEXT("subtitle"), ESearchCase::IgnoreCase))
			{
				sol::optional<int> Idx = Params.get<sol::optional<int>>("index");
				if (!Idx.has_value())
				{
					Session.Log(TEXT("[FAIL] remove('subtitle') -> 'index' param required"));
					return sol::lua_nil;
				}
				int32 Index = Idx.value();
				if (Index < 0 || Index >= Wave->Subtitles.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove('subtitle') -> index %d out of range [0..%d)"),
						Index, Wave->Subtitles.Num()));
					return sol::lua_nil;
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("SoundWave: Remove Subtitle")));
				Wave->Modify();
				Wave->Subtitles.RemoveAt(Index);
				FPropertyChangedEvent Event(nullptr, EPropertyChangeType::ValueSet);
				Wave->PostEditChangeProperty(Event);
				Wave->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] remove('subtitle', index=%d)"), Index));
				return sol::make_object(Lua, true);
			}

			// ----- cue_point -----
			if (TypeStr.Equals(TEXT("cue_point"), ESearchCase::IgnoreCase))
			{
#if WITH_EDITOR
				sol::optional<int> Idx = Params.get<sol::optional<int>>("index");
				if (!Idx.has_value())
				{
					Session.Log(TEXT("[FAIL] remove('cue_point') -> 'index' param required"));
					return sol::lua_nil;
				}

				int32 Index = Idx.value();
				TArray<FSoundWaveCuePoint> ListedCuePoints = Wave->GetCuePoints();
				if (Index < 0 || Index >= ListedCuePoints.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove('cue_point') -> index %d out of range [0..%d)"),
						Index, ListedCuePoints.Num()));
					return sol::lua_nil;
				}
				const int32 CuePointID = ListedCuePoints[Index].CuePointID;

				TArray<FSoundWaveCuePoint> CuePoints = NSAI_GetSoundWaveCuePoints(Wave);
				const int32 FullIndex = CuePoints.IndexOfByPredicate([CuePointID](const FSoundWaveCuePoint& CP)
				{
					return !CP.IsLoopRegion() && CP.CuePointID == CuePointID;
				});
				if (FullIndex == INDEX_NONE)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove('cue_point') -> listed cue point id %d not found in full cue-point storage"), CuePointID));
					return sol::lua_nil;
				}

				const FScopedTransaction Transaction(FText::FromString(TEXT("SoundWave: Remove Cue Point")));
				Wave->Modify();
				CuePoints.RemoveAt(FullIndex);
				NSAI_SetSoundWaveCuePoints(Wave,CuePoints);
				Wave->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] remove('cue_point', index=%d, cue_point_id=%d)"), Index, CuePointID));
				return sol::make_object(Lua, true);
#else
				Session.Log(TEXT("[FAIL] remove('cue_point') -> only available in editor builds"));
				return sol::lua_nil;
#endif
			}

			Session.Log(FString::Printf(TEXT("[FAIL] remove('%s') -> unknown type. Valid: subtitle, cue_point, all_subtitles, all_cue_points"), *TypeStr));
			return sol::lua_nil;
		});
	});
}

REGISTER_LUA_BINDING(SoundWave, SoundWaveDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindSoundWave(Lua, Session);
});
