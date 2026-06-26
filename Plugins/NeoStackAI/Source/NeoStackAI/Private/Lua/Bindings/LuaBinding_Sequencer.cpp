// Copyright 2025-2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Lua/LuaAssetResolver.h"
#include "Lua/LuaStr.h"
#include "Lua/LuaTypeResolver.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneSequence.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSection.h"
#include "MovieSceneTrack.h"
#include "MovieSceneNameableTrack.h"
#include "MovieSceneSpawnable.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Tracks/MovieSceneAudioTrack.h"
#include "Tracks/MovieSceneSkeletalAnimationTrack.h"
#include "Tracks/MovieScenePropertyTrack.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Sections/MovieSceneAudioSection.h"
#include "Sections/MovieSceneSkeletalAnimationSection.h"
#include "Sections/MovieSceneCameraCutSection.h"
#include "Sections/MovieSceneFadeSection.h"
#include "Sections/MovieSceneSubSection.h"
#include "Tracks/MovieSceneSubTrack.h"
#include "Tracks/MovieSceneFadeTrack.h"
#include "Tracks/MovieSceneCinematicShotTrack.h"
#include "Sections/MovieSceneCinematicShotSection.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Channels/MovieSceneIntegerChannel.h"
#include "Channels/MovieSceneBoolChannel.h"
#include "Channels/MovieSceneByteChannel.h"
#include "Channels/MovieSceneStringChannel.h"
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7)
#include "Channels/MovieSceneTextChannel.h"
#else
#include "MovieSceneTextChannel.h"
#endif
#include "Channels/MovieSceneEventChannel.h"
#include "Channels/MovieSceneObjectPathChannel.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneChannelEditorData.h"
#include "Sound/SoundBase.h"
#include "Animation/AnimSequenceBase.h"
#include "MovieSceneObjectBindingID.h"
#include "MovieSceneSequenceID.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Editor.h"
#include "Camera/CameraActor.h"
#include "ScopedTransaction.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "MovieSceneMarkedFrame.h"
#include "MovieSceneFolder.h"
#include "MovieSceneFwd.h"
#include "MovieSceneCommonHelpers.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
#include "MovieSceneClock.h"
#endif
#include "MovieSceneTimeHelpers.h"
#include "Generators/MovieSceneEasingFunction.h"
#include "Evaluation/MovieSceneCompletionMode.h"
#include "Evaluation/Blending/MovieSceneBlendType.h"
#include "Channels/MovieSceneEvent.h"
#include "Engine/Blueprint.h"
#include "MovieSceneSequenceEditor.h"
#include "Tools/NeoStackToolUtils.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// Helpers
// ============================================================================

namespace
{

FGuid FindBindingByName(UMovieScene* MovieScene, const FString& Name)
{
	if (!MovieScene) return FGuid();

	for (int32 i = 0; i < MovieScene->GetPossessableCount(); i++)
	{
		const FMovieScenePossessable& P = MovieScene->GetPossessable(i);
		if (P.GetName().Equals(Name, ESearchCase::IgnoreCase))
			return P.GetGuid();
	}
	for (int32 i = 0; i < MovieScene->GetSpawnableCount(); i++)
	{
		const FMovieSceneSpawnable& S = MovieScene->GetSpawnable(i);
		if (S.GetName().Equals(Name, ESearchCase::IgnoreCase))
			return S.GetGuid();
	}
	return FGuid();
}

AActor* FindActorByName(const FString& ActorName)
{
	if (!GEditor) return nullptr;
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return nullptr;

	// Exact label
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (*It && (*It)->GetActorLabel().Equals(ActorName, ESearchCase::IgnoreCase))
			return *It;
	}
	// Exact name
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (*It && (*It)->GetName().Equals(ActorName, ESearchCase::IgnoreCase))
			return *It;
	}
	// Partial
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (*It && ((*It)->GetActorLabel().Contains(ActorName) || (*It)->GetName().Contains(ActorName)))
			return *It;
	}
	return nullptr;
}

// Resolve a class name like "PointLight", "CameraActor", "APointLight", or a full
// class path ("/Script/Engine.PointLight", "/Game/BP_Camera.BP_Camera_C") to an
// AActor subclass. Delegates to NeoLuaType::ResolveType for path-aware lookup that
// the hand-rolled TObjectIterator scan could not do (BP classes must be loaded,
// not just iterated).
UClass* ResolveActorClassByName(const FString& ClassName)
{
	// 1) Full-path / short-name lookup via the shared resolver. Accepts /Script,
	//    /Game, F-prefix, primitives (we reject non-UClass below). This also
	//    handles LoadObject<UClass> for BP classes that aren't yet in memory.
	FString Canonical = ClassName;
	if (UObject* Obj = NeoLuaType::ResolveType(Canonical))
	{
		if (UClass* C = Cast<UClass>(Obj))
		{
			if (C->IsChildOf(AActor::StaticClass()) && !C->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
				return C;
		}
	}

	// 2) Fall back to the original short-name iteration for "PointLight" style
	//    lookups where the class is loaded but the short name doesn't match the
	//    canonical A-prefix form used by UField searches.
	FString WithPrefix = ClassName.StartsWith(TEXT("A")) ? ClassName : (TEXT("A") + ClassName);
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (!It->IsChildOf(AActor::StaticClass()) || It->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
			continue;

		FString ShortName = It->GetName();
		if (ShortName.Equals(ClassName, ESearchCase::IgnoreCase) ||
			ShortName.Equals(WithPrefix, ESearchCase::IgnoreCase))
			return *It;
	}
	// 3) Partial match as a last resort.
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if (!It->IsChildOf(AActor::StaticClass()) || It->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
			continue;
		if (It->GetName().Contains(ClassName))
			return *It;
	}
	return nullptr;
}

// Find a master track (no binding) by track class type
UMovieSceneTrack* FindMasterTrackByClass(UMovieScene* MovieScene, UClass* TrackClass, int32 TrackIdx = -1)
{
	if (!MovieScene || !TrackClass) return nullptr;
	if (TrackClass->IsChildOf(UMovieSceneCameraCutTrack::StaticClass()))
	{
		UMovieSceneTrack* CameraCutTrack = MovieScene->GetCameraCutTrack();
		if (CameraCutTrack && CameraCutTrack->IsA(TrackClass) && (TrackIdx < 0 || TrackIdx == 0))
		{
			return CameraCutTrack;
		}
		return nullptr;
	}

	int32 MatchCount = 0;
	for (UMovieSceneTrack* Track : MovieScene->GetTracks())
	{
		if (Track && Track->IsA(TrackClass))
		{
			if (TrackIdx < 0 || MatchCount == TrackIdx) return Track;
			MatchCount++;
		}
	}
	return nullptr;
}

FFrameNumber SecondsToFrame(UMovieScene* MovieScene, double Seconds)
{
	const FFrameRate TickRes = MovieScene->GetTickResolution();
	return FFrameNumber(static_cast<int32>(FMath::RoundToDouble(Seconds * TickRes.AsDecimal())));
}

double FrameToSeconds(UMovieScene* MovieScene, FFrameNumber Frame)
{
	const double Tick = MovieScene->GetTickResolution().AsDecimal();
	return FMath::IsNearlyZero(Tick) ? 0.0 : Frame.Value / Tick;
}

UClass* ResolveTrackClass(const FString& TrackType)
{
	if (TrackType.IsEmpty()) return nullptr;

	TArray<UClass*> Derived;
	GetDerivedClasses(UMovieSceneTrack::StaticClass(), Derived);

	auto GetFriendly = [](UClass* C) -> FString
	{
		FString N = C->GetName();
		N.RemoveFromStart(TEXT("MovieScene"));
		N.RemoveFromEnd(TEXT("Track"));
		return N.IsEmpty() ? C->GetName() : N;
	};

	// Pass 1: exact friendly name
	for (UClass* C : Derived)
	{
		if (!C || C->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated)) continue;
		if (GetFriendly(C).Equals(TrackType, ESearchCase::IgnoreCase)) return C;
	}
	// Pass 2: full class name
	for (UClass* C : Derived)
	{
		if (!C || C->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated)) continue;
		if (C->GetName().Equals(TrackType, ESearchCase::IgnoreCase)) return C;
		FString WithPrefix = FString::Printf(TEXT("MovieScene%sTrack"), *TrackType);
		if (C->GetName().Equals(WithPrefix, ESearchCase::IgnoreCase)) return C;
	}
	// Pass 3: partial contains
	for (UClass* C : Derived)
	{
		if (!C || C->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated)) continue;
		if (C->GetName().Contains(TrackType)) return C;
	}
	return nullptr;
}

FString GetFriendlyTrackName(UClass* TrackClass)
{
	if (!TrackClass) return TEXT("Unknown");
	FString N = TrackClass->GetName();
	N.RemoveFromStart(TEXT("MovieScene"));
	N.RemoveFromEnd(TEXT("Track"));
	return N.IsEmpty() ? TrackClass->GetName() : N;
}

ETrackSupport GetSequenceTrackSupport(const UMovieSceneSequence* Sequence, UClass* TrackClass)
{
	if (!Sequence || !TrackClass || !TrackClass->IsChildOf(UMovieSceneTrack::StaticClass()))
	{
		return ETrackSupport::NotSupported;
	}

#if WITH_EDITOR
	return Sequence->IsTrackSupported(TSubclassOf<UMovieSceneTrack>(TrackClass));
#else
	return ETrackSupport::Supported;
#endif
}

bool IsTrackClassSupportedBySequence(const UMovieSceneSequence* Sequence, UClass* TrackClass)
{
	return GetSequenceTrackSupport(Sequence, TrackClass) != ETrackSupport::NotSupported;
}

const char* TrackSupportToString(ETrackSupport Support)
{
	switch (Support)
	{
	case ETrackSupport::Supported:
		return "supported";
	case ETrackSupport::Default:
		return "default";
	default:
		return "not_supported";
	}
}

int32 CountChannels(UMovieSceneSection* Section)
{
	if (!Section) return 0;
	FMovieSceneChannelProxy& Proxy = Section->GetChannelProxy();
	int32 C = 0;
	// Count every channel type that the add/list keyframe paths can actually reach.
	// Must stay in sync with the AppendChannels/AddKey branches in list("channels")
	// and add("keyframe") — if those ever grow a new channel type, add it here too.
	C += Proxy.GetChannels<FMovieSceneDoubleChannel>().Num();
	C += Proxy.GetChannels<FMovieSceneFloatChannel>().Num();
	C += Proxy.GetChannels<FMovieSceneBoolChannel>().Num();
	C += Proxy.GetChannels<FMovieSceneIntegerChannel>().Num();
	C += Proxy.GetChannels<FMovieSceneByteChannel>().Num();
	C += Proxy.GetChannels<FMovieSceneStringChannel>().Num();
	C += Proxy.GetChannels<FMovieSceneTextChannel>().Num();
	C += Proxy.GetChannels<FMovieSceneObjectPathChannel>().Num();
	C += Proxy.GetChannels<FMovieSceneEventChannel>().Num();
	return C;
}

UMovieSceneFolder* FindFolderByName(TArrayView<UMovieSceneFolder* const> Folders, const FString& Name, UMovieSceneFolder** OutParent = nullptr)
{
	for (UMovieSceneFolder* Folder : Folders)
	{
		if (!Folder) continue;
		if (Folder->GetFolderName().ToString().Equals(Name, ESearchCase::IgnoreCase))
		{
			if (OutParent) *OutParent = nullptr;
			return Folder;
		}
		UMovieSceneFolder* Found = FindFolderByName(Folder->GetChildFolders(), Name, OutParent);
		if (Found)
		{
			if (OutParent && *OutParent == nullptr) *OutParent = Folder;
			return Found;
		}
	}
	return nullptr;
}

FColor ParseColorFromLua(sol::table C, FColor Default = FColor(255, 255, 255, 255))
{
	// Support hex string: {hex="#FF0000"}
	std::string HexStr = C.get_or("hex", std::string());
	if (!HexStr.empty())
	{
		FString Hex = NeoLuaStr::ToFString(HexStr);
		Hex.RemoveFromStart(TEXT("#"));
		FColor Result = FColor::FromHex(Hex);
		return Result;
	}
	// Support RGB table: {r=255, g=0, b=0}
	FColor Result;
	Result.R = static_cast<uint8>(C.get_or("r", static_cast<int>(Default.R)));
	Result.G = static_cast<uint8>(C.get_or("g", static_cast<int>(Default.G)));
	Result.B = static_cast<uint8>(C.get_or("b", static_cast<int>(Default.B)));
	Result.A = static_cast<uint8>(C.get_or("a", static_cast<int>(Default.A)));
	return Result;
}

// FLinearColor sibling — used by fields that store float colors natively (e.g. marked frames).
// Accepts: {hex="#RRGGBBAA"} | {r,g,b,a} with either 0-1 floats or 0-255 ints (auto-detected).
//
// Scale detection uses ONLY explicitly-provided channels. A prior version folded the
// Default channels into the max check, so `{r=255}` would divide the float-scale
// Defaults (e.g. G=1.0) by 255 and clobber them to near-zero. Now: if any explicit
// channel > 1, we treat explicit channels as 0-255 ints and normalize them while
// leaving Default channels untouched.
FLinearColor ParseLinearColorFromLua(sol::table C, FLinearColor Default = FLinearColor(0.f, 1.f, 1.f, 0.4f))
{
	std::string HexStr = C.get_or("hex", std::string());
	if (!HexStr.empty())
	{
		FString Hex = NeoLuaStr::ToFString(HexStr);
		Hex.RemoveFromStart(TEXT("#"));
		return FLinearColor(FColor::FromHex(Hex));
	}
	sol::optional<double> RO = C.get<sol::optional<double>>("r");
	sol::optional<double> GO = C.get<sol::optional<double>>("g");
	sol::optional<double> BO = C.get<sol::optional<double>>("b");
	sol::optional<double> AO = C.get<sol::optional<double>>("a");

	// Detect scale from provided channels only.
	bool bIsByteScale = false;
	if (RO.has_value() && RO.value() > 1.0) bIsByteScale = true;
	if (GO.has_value() && GO.value() > 1.0) bIsByteScale = true;
	if (BO.has_value() && BO.value() > 1.0) bIsByteScale = true;
	if (AO.has_value() && AO.value() > 1.0) bIsByteScale = true;
	const double Div = bIsByteScale ? 255.0 : 1.0;

	return FLinearColor(
		static_cast<float>(RO.has_value() ? RO.value() / Div : Default.R),
		static_cast<float>(GO.has_value() ? GO.value() / Div : Default.G),
		static_cast<float>(BO.has_value() ? BO.value() / Div : Default.B),
		static_cast<float>(AO.has_value() ? AO.value() / Div : Default.A));
}

} // namespace

// ============================================================================
// Lua Binding
// ============================================================================

// One-line docs surfaced by the global doc index (in-asset help() still drives
// the full _help_text block set up inside BindSequencer). Entries are keyed by
// the asset-verb namespace so agents can discover them without open_asset first.
static TArray<FLuaFunctionDoc> SequencerDocs = {
	{ TEXT("open_asset(path):add(type, params)"),
	  TEXT("Add Sequencer element: binding, track, section, keyframe, marked_frame, folder, binding_tag, section_group, node_group"),
	  TEXT("table | true | nil") },
	{ TEXT("open_asset(path):remove(type, id)"),
	  TEXT("Remove a Sequencer element by id or descriptor"),
	  TEXT("true | nil") },
	{ TEXT("open_asset(path):configure(type, id_or_params, params?)"),
	  TEXT("Configure sequence | binding | track | folder | marked_frame(s) | node_group"),
	  TEXT("table | true | nil") },
	{ TEXT("open_asset(path):list(type, params?)"),
	  TEXT("List Sequencer state: track_types, bindings, channels, keyframes, properties, marked_frames, folders, binding_tags, section_groups, node_groups"),
	  TEXT("table | nil") },
	{ TEXT("open_asset(path):configure_section(params)"),
	  TEXT("Configure a section: ease/range/row_index/pre-post-roll/active/locked/color_tint + type-specific"),
	  TEXT("true | nil") },
	{ TEXT("open_asset(path):set_transforms(params)"),
	  TEXT("Key location/rotation/scale on a 3DTransform track binding"),
	  TEXT("true | nil") },
	{ TEXT("open_asset(path):set_playback_range(start, end)"),
	  TEXT("Set the playback range in seconds"),
	  TEXT("true | nil") },
	{ TEXT("open_asset(path):add_subsequence(params)"),
	  TEXT("Nest another LevelSequence as a subsequence section"),
	  TEXT("table | nil") },
	{ TEXT("open_asset(path):execute_shot_plan(params)"),
	  TEXT("Apply a batch of camera cuts with optional procedural framing"),
	  TEXT("table | nil") },
	{ TEXT("open_asset(path):analyze_camera_cuts()"),
	  TEXT("Pacing/coverage report for camera cuts"),
	  TEXT("table") },
	{ TEXT("open_asset(path):find_marked_frame(params)"),
	  TEXT("Find a marked frame by {label=..} or {frame=N}"),
	  TEXT("integer | nil") },
	{ TEXT("open_asset(path):find_next_marked_frame(params)"),
	  TEXT("Walk to the adjacent marker from {frame=N, forward?=true}"),
	  TEXT("integer | nil") },
	{ TEXT("open_asset(path):get_director_blueprint()"),
	  TEXT("Path of the assigned director blueprint, or nil"),
	  TEXT("string | nil") },
	{ TEXT("open_asset(path):set_director_blueprint()"),
	  TEXT("Create-or-fetch this sequence's director blueprint (owned by the sequence)"),
	  TEXT("table | nil") },
	{ TEXT("open_asset(path):metadata(verb, params?)"),
	  TEXT("Manage ULevelSequence metadata: list | find | add | copy | remove"),
	  TEXT("table | nil") },
	{ TEXT("open_asset(path):info()"),
	  TEXT("Summary: counts, rates, ranges, director_blueprint, camera_cuts, groups"),
	  TEXT("table") },
};

static void BindSequencer(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_sequencer", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = NeoLuaStr::ToFString(PathStr);
		ULevelSequence* Sequence = NeoLuaAsset::Resolve<ULevelSequence>(FPath);
		if (!Sequence) return;
		UMovieScene* MovieScene = Sequence->GetMovieScene();
		if (!MovieScene) return;

		AssetObj["_help_text"] =
			"Element types for add/remove/list:\n"
			"  binding     — actor/component possessable or spawnable\n"
			"  track       — track on a binding or master track\n"
			"  section     — append/remove a section on an existing track\n"
			"  keyframe     — keyframe by channel index on a track\n"
			"  marked_frame — timeline marker (add/remove/list/configure)\n"
			"  folder       — organizational folder (add/remove/list/configure)\n"
			"  binding_tag  — tag on a binding for organizing/referencing (add/remove/list)\n"
			"  section_group — group sections together (add/remove/list)\n"
			"  node_group   — group tracks/nodes in the sequencer tree (add/remove/list/configure)\n"
			"\n"
			"Discovery (list only):\n"
			"  track_types  — all available track types\n"
			"  bindings     — full binding/track/section/timing dump (includes class, parent, template_class,\n"
			"                 row_index, overlap_priority, pre/post_roll_frames, ease_in/out_frames per section)\n"
			"  channels     — channel indices for a specific track ({binding, track_type, track_index?, section_index?})\n"
			"  keyframes    — all keyframes on a channel ({binding, track_type, channel, track_index?, section?})\n"
			"  properties    — animatable properties on an actor or binding ({actor_name=..} or {binding=..})\n"
			"  marked_frames — all marked frames on the timeline\n"
			"  folders       — folder tree structure\n"
			"  binding_tags — all binding tag mappings\n"
			"  section_groups — all section groups\n"
			"  node_groups  — all node groups\n"
			"\n"
			"add(type, params):\n"
			"  add(\"binding\", {actor_name=\"CameraActor\", name?=\"Cam\", component_name?=\"CameraComponent\",\n"
			"       spawnable?=false, spawn_ownership?=\"inner_sequence\"|\"root_sequence\"|\"external\",\n"
			"       continuously_respawn?=false, net_addressable_name?=false})\n"
			"    Possessable: actor must exist in level. Spawnable: class name or /Script/Engine.CameraActor path\n"
			"  add(\"track\", {binding?=\"Cam\", track_type=\"3DTransform\", start_time?=0, end_time?=5,\n"
			"       sound_asset?, sound_volume?, sound_pitch?, animation_asset?,\n"
			"       camera_binding?, property_name?, property_path?,\n"
			"       -- CinematicShot: sequence (path), shot_display_name?, row_index?})\n"
			"  add(\"keyframe\", {binding?=\"Cam\", track_type=\"3DTransform\", channel_index=0,\n"
			"       time=1.0, value=100, bool_value?, string_value?, object_path?,\n"
			"       interp?=\"cubic\", track_index?=0, section_index?=0})\n"
			"    Omit binding for master tracks (Fade, CameraCut, etc.)\n"
			"    Channel types: Double/Float/Bool/Integer/Byte (value=N), String (string_value=\"text\"),\n"
			"    ObjectPath (object_path=\"/Game/...\"), Event (event=\"/Game/BP.FnName\" or\n"
			"                {function=\"/Game/BP.FnName\", bound_object?=\"PinName\", payload?={k=v,..}})\n"
			"  add(\"section\", {binding?=\"Cam\", track_type=\"Audio\", track_index?=0,\n"
			"       start=0.0, end=2.0, row_index?=-1})\n"
			"  add(\"marked_frame\", {frame=120, label?=\"name\", comment?=\"...\",\n"
			"       color?={r,g,b,a} (0-1 floats or 0-255 ints) | {hex=\"#RRGGBBAA\"},\n"
			"       is_determinism_fence?=false, is_inclusive_time?=false})\n"
			"  add(\"folder\", {name=\"Cameras\", parent?=\"ParentFolder\", color?={r=255,g=128,b=0} or {hex=\"#FF8000\"}})\n"
			"  add(\"binding_tag\", {tag=\"Hero\", binding?=\"ActorName\"}) — create tag, optionally assign to binding\n"
			"  add(\"section_group\", {sections={...}}) — group sections by {binding, track_type, track_index?, section_index?}\n"
			"  add(\"node_group\", {name=\"GroupName\", nodes?={\"path1\",\"path2\"}, enable_filter?=false})\n"
			"\n"
			"remove(type, id):\n"
			"  remove(\"binding\", \"CameraActor\")\n"
			"  remove(\"track\", {binding?=\"Cam\", track_type=\"3DTransform\", track_index?=0})\n"
			"  remove(\"section\", {binding?=\"Cam\", track_type=\"Audio\", track_index?=0, section_index=0})\n"
			"  remove(\"keyframe\", {binding?=\"Cam\", track_type=\"3DTransform\", channel_index=0, time=1.0, section_index?=0})\n"
			"  remove(\"marked_frame\", index_or_label) — by 0-based index or label string\n"
			"  remove(\"marked_frames\") — remove all marked frames\n"
			"  remove(\"folder\", \"FolderName\") — remove folder by name\n"
			"  remove(\"binding_tag\", {tag=\"Hero\", binding?=\"ActorName\"}) — remove tag entirely, or untag a specific binding\n"
			"  remove(\"section_group\", {binding, track_type, track_index?, section_index?}) — ungroup a section\n"
			"  remove(\"node_group\", \"GroupName\") — remove a node group\n"
			"\n"
			"configure(type, id, params):\n"
			"  configure(\"sequence\", {display_rate?=30, tick_resolution?=24000,\n"
			"       evaluation_type?=\"with_sub_frames\"|\"frame_locked\",\n"
			"       clock_source?=\"tick\"|\"platform\"|\"audio\"|\"relative_timecode\"|\"timecode\"|\"play_every_frame\"|\"custom\",\n"
			"       custom_clock?=\"/Script/Foo.UMyClockClass\", read_only?=false,\n"
			"       playback_range_locked?=false, selection_range?={start,end},\n"
			"       view_range?={start,end}, working_range?={start,end}})\n"
			"  configure(\"binding\", \"BindingName\", {display_name?, sorting_order?, rebind_actor?,\n"
			"       unbind?=true, spawn_ownership?=\"inner_sequence\"|\"root_sequence\"|\"external\",\n"
			"       continuously_respawn?=true/false, net_addressable_name?=true/false})\n"
			"  configure(\"track\", {binding?=\"ActorName\", track_type=\"Transform\", track_index?=0},\n"
			"       {display_name?=\"Custom Name\", color?={r=255,g=128,b=0}, muted?=true,\n"
			"        sorting_order?=N,\n"
			"        row_eval_disabled?={row=N, disabled=true/false},\n"
			"        local_row_eval_disabled?={row=N, disabled=true/false},\n"
			"        fix_row_indices?=true, update_easing?=true, remove_all_animation_data?=true,\n"
			"        section_to_key?=<section_index> | -1 (clear)})\n"
			"  configure(\"folder\", \"FolderName\", {new_name?, color?, sorting_order?, add_binding?, remove_binding?,\n"
			"       add_track?={binding?, track_type, track_index?}, remove_track?={binding?, track_type, track_index?}})\n"
			"  configure(\"marked_frame\", index_or_label, {frame?, label?, comment?,\n"
			"       color?={r,g,b,a} (0-1 floats or 0-255 ints) | {hex=\"#RRGGBBAA\"},\n"
			"       use_custom_color?=true/false, is_determinism_fence?, is_inclusive_time?})\n"
			"  configure(\"marked_frames\", {locked?=true/false, globally_show?=true/false}) — lock/show all\n"
			"  configure(\"node_group\", \"GroupName\", {new_name?, add_node?, remove_node?, enable_filter?})\n"
			"\n"
			"list(type, params?):\n"
			"  list(\"track_types\"), list(\"bindings\"), list(\"channels\", {binding=.., track_type=.., section_index?})\n"
			"  list(\"keyframes\", {binding=.., track_type=.., channel=N, track_index?, section?})\n"
			"  list(\"properties\", {actor_name=.. or binding=..})\n"
			"  list(\"marked_frames\"), list(\"folders\")\n"
			"  list(\"binding_tags\"), list(\"section_groups\"), list(\"node_groups\")\n"
			"\n"
			"Action methods:\n"
			"  set_transforms({binding, time, location?, rotation?, scale?, interp?})\n"
			"    location: {x,y,z} or {X,Y,Z}  rotation: {pitch=N,yaw=N,roll=N} or {P,Y,R}  scale: {x,y,z} or {X,Y,Z}\n"
			"  set_playback_range(start, end) — set playback range in seconds\n"
			"  configure_section({track_type, binding?, track_index?, section_index?,\n"
			"    ease_in?, ease_out?, ease_in_type?=\"/Script/...\", ease_out_type?=\"/Script/...\",\n"
			"    completion_mode?=\"keep_state\"|\"restore_state\"|\"project_default\",\n"
			"    blend_type?=\"absolute\"|\"additive\"|\"relative\"|\"additive_from_base\"|\"override\",\n"
			"    active?, locked?, color_tint?, start?, end?, row_index?, overlap_priority?,\n"
			"    pre_roll_frames?, post_roll_frames?,\n"
			"    -- Fade: color?={r,g,b,a}, fade_audio?\n"
			"    -- Audio: looping?, start_offset?, sound_asset?, suppress_subtitles?, override_attenuation?\n"
			"    -- SkeletalAnimation: start_frame_offset?, end_frame_offset?, slot_name?,\n"
			"    --   play_rate?, reverse?, skip_anim_notifiers?, force_custom_mode?,\n"
			"    --   match_with_previous?, animation? (asset path)\n"
			"    -- CameraCut: camera_binding? (binding name/guid), lock_previous_camera?\n"
			"    -- CinematicShot: shot_display_name?\n"
			"    -- Sub/Subsequence: sub_sequence? (asset path)})\n"
			"  add_subsequence({sequence_path, start_time, end_time?, row_index?})\n"
			"  execute_shot_plan({shots={...}, replace_camera_cuts?, default_target_actor?})\n"
			"  analyze_camera_cuts() — pacing/coverage review\n"
			"  get_director_blueprint() — returns path of the director BP, or nil\n"
			"  set_director_blueprint() — create-or-fetch the director BP (owned by this sequence)\n"
			"  find_marked_frame({label?=\"\" | frame?=N}) — returns marker index or nil\n"
			"  find_next_marked_frame({frame=N, forward?=true}) — returns adjacent marker index or nil\n"
			"  metadata(verb, params?) — verbs: list | find | add | copy | remove. Wraps ULevelSequence meta APIs\n"
			"  info() — summary (includes playback_range_locked, view_range, director_blueprint)\n";

		// ================================================================
		// list(type, params?)
		// ================================================================
		AssetObj.set_function("list", [Sequence, MovieScene, &Session](sol::table self,
			sol::optional<std::string> TypeOpt, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = NeoLuaStr::ToFStringOpt(TypeOpt, TEXT("all"));

			if (FType.Equals(TEXT("all"), ESearchCase::IgnoreCase))
			{
				sol::protected_function InfoFn = self["info"];
				if (InfoFn.valid()) return InfoFn(self);
				return sol::lua_nil;
			}

			// ---- list("track_types") ----
			if (FType.Equals(TEXT("track_types"), ESearchCase::IgnoreCase))
			{
				TArray<UClass*> Derived;
				GetDerivedClasses(UMovieSceneTrack::StaticClass(), Derived);

				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				for (UClass* C : Derived)
				{
					if (!C || C->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated)) continue;
					const ETrackSupport Support = GetSequenceTrackSupport(Sequence, C);
					if (Support == ETrackSupport::NotSupported) continue;
					sol::table Entry = Lua.create_table();
					Entry["name"] = TCHAR_TO_UTF8(*GetFriendlyTrackName(C));
					Entry["class_name"] = TCHAR_TO_UTF8(*C->GetName());
					Entry["support"] = TrackSupportToString(Support);
					Result[Idx++] = Entry;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"track_types\") -> %d"), Idx - 1));
				return Result;
			}

			// ---- list("bindings") ----
			if (FType.Equals(TEXT("bindings"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();

				const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
				const FFrameRate TickRate = MovieScene->GetTickResolution();
				Result["display_rate"] = DisplayRate.AsDecimal();
				Result["tick_resolution"] = TickRate.AsDecimal();

				const TRange<FFrameNumber> PlaybackRange = MovieScene->GetPlaybackRange();
				if (PlaybackRange.HasLowerBound())
					Result["playback_start"] = FrameToSeconds(MovieScene, PlaybackRange.GetLowerBoundValue());
				if (PlaybackRange.HasUpperBound())
					Result["playback_end"] = FrameToSeconds(MovieScene, PlaybackRange.GetUpperBoundValue());

				// Build GUID->name map
				TMap<FGuid, FString> GuidToName;
				for (int32 i = 0; i < MovieScene->GetPossessableCount(); ++i)
				{
					const FMovieScenePossessable& P = MovieScene->GetPossessable(i);
					GuidToName.Add(P.GetGuid(), P.GetName());
				}
				for (int32 i = 0; i < MovieScene->GetSpawnableCount(); ++i)
				{
					const FMovieSceneSpawnable& Sp = MovieScene->GetSpawnable(i);
					GuidToName.Add(Sp.GetGuid(), Sp.GetName());
				}

				auto BuildTrackTable = [&](UMovieSceneTrack* Track, int32 TrackIndex) -> sol::table
				{
					sol::table TT = Lua.create_table();
					TT["index"] = TrackIndex;
					TT["type"] = Track ? TCHAR_TO_UTF8(*GetFriendlyTrackName(Track->GetClass())) : "Unknown";
					if (!Track) return TT;

#if WITH_EDITORONLY_DATA
					TT["display_name"] = TCHAR_TO_UTF8(*Track->GetDisplayName().ToString());
					const FColor& Tint = Track->GetColorTint();
					if (Tint != FColor(0, 0, 0, 0))
					{
						sol::table ColorT = Lua.create_table();
						ColorT["r"] = Tint.R;
						ColorT["g"] = Tint.G;
						ColorT["b"] = Tint.B;
						ColorT["a"] = Tint.A;
						TT["color_tint"] = ColorT;
					}
#endif
					TT["is_muted"] = Track->IsEvalDisabled();

					const TArray<UMovieSceneSection*>& Sections = Track->GetAllSections();
					TT["sections"] = Sections.Num();

					sol::table SectionsTable = Lua.create_table();
					for (int32 si = 0; si < Sections.Num(); ++si)
					{
						UMovieSceneSection* Sec = Sections[si];
						sol::table ST = Lua.create_table();
						ST["index"] = si;
						if (Sec)
						{
							const TRange<FFrameNumber> R = Sec->GetRange();
							if (R.HasLowerBound()) ST["start"] = FrameToSeconds(MovieScene, R.GetLowerBoundValue());
							if (R.HasUpperBound()) ST["end"] = FrameToSeconds(MovieScene, R.GetUpperBoundValue());
							ST["channels"] = CountChannels(Sec);
							ST["row_index"] = Sec->GetRowIndex();
							ST["overlap_priority"] = Sec->GetOverlapPriority();
							if (Sec->GetPreRollFrames() > 0) ST["pre_roll_frames"] = Sec->GetPreRollFrames();
							if (Sec->GetPostRollFrames() > 0) ST["post_roll_frames"] = Sec->GetPostRollFrames();
							if (Sec->Easing.GetEaseInDuration() > 0) ST["ease_in_frames"] = Sec->Easing.GetEaseInDuration();
							if (Sec->Easing.GetEaseOutDuration() > 0) ST["ease_out_frames"] = Sec->Easing.GetEaseOutDuration();

							if (UMovieSceneCameraCutSection* CCS = Cast<UMovieSceneCameraCutSection>(Sec))
							{
								const FMovieSceneObjectBindingID& CID = CCS->GetCameraBindingID();
								if (CID.IsValid())
								{
									const FString* CN = GuidToName.Find(CID.GetGuid());
									ST["camera_binding"] = TCHAR_TO_UTF8(CN ? **CN : *CID.GetGuid().ToString());
								}
							}

							// CinematicShot section details (check before SubSection since it inherits from it)
							if (UMovieSceneCinematicShotSection* CineSec = Cast<UMovieSceneCinematicShotSection>(Sec))
							{
								FString ShotName = CineSec->GetShotDisplayName();
								if (!ShotName.IsEmpty())
								{
									ST["shot_display_name"] = TCHAR_TO_UTF8(*ShotName);
								}
								if (UMovieSceneSequence* SubSeq = CineSec->GetSequence())
								{
									ST["sequence"] = TCHAR_TO_UTF8(*SubSeq->GetPathName());
								}
							}
							// Sub/Subsequence section details
							else if (UMovieSceneSubSection* SubSec = Cast<UMovieSceneSubSection>(Sec))
							{
								if (UMovieSceneSequence* SubSeq = SubSec->GetSequence())
								{
									ST["sequence"] = TCHAR_TO_UTF8(*SubSeq->GetPathName());
								}
							}
						}
						SectionsTable[si + 1] = ST;
					}
					TT["section_details"] = SectionsTable;
					return TT;
				};

				auto BuildBindingTable = [&](const FGuid& Guid, const FString& Name, const char* Kind) -> sol::table
				{
					sol::table BT = Lua.create_table();
					BT["name"] = TCHAR_TO_UTF8(*Name);
					BT["kind"] = Kind;
					BT["guid"] = TCHAR_TO_UTF8(*Guid.ToString());

					const FMovieSceneBinding* Binding = MovieScene->FindBinding(Guid);
					if (Binding)
					{
						const TArray<UMovieSceneTrack*>& Tracks = Binding->GetTracks();
						sol::table TracksTable = Lua.create_table();
						for (int32 ti = 0; ti < Tracks.Num(); ++ti)
							TracksTable[ti + 1] = BuildTrackTable(Tracks[ti], ti);
						BT["tracks"] = TracksTable;
					}
					return BT;
				};

				// Flat list (integer-indexed) so ipairs(result) iterates all bindings
				int32 FlatIdx = 1;

				// Possessables
				sol::table Poss = Lua.create_table();
				for (int32 i = 0; i < MovieScene->GetPossessableCount(); ++i)
				{
					const FMovieScenePossessable& P = MovieScene->GetPossessable(i);
					sol::table BT = BuildBindingTable(P.GetGuid(), P.GetName(), "possessable");
					if (const UClass* PClass = P.GetPossessedObjectClass())
						BT["class"] = TCHAR_TO_UTF8(*PClass->GetName());
					if (P.GetParent().IsValid())
					{
						const FString* ParentName = GuidToName.Find(P.GetParent());
						BT["parent"] = TCHAR_TO_UTF8(ParentName ? **ParentName : *P.GetParent().ToString());
					}
					Poss[i + 1] = BT;
					Result[FlatIdx++] = BT;
				}
				Result["possessables"] = Poss;

				// Spawnables
				sol::table Spawn = Lua.create_table();
				for (int32 i = 0; i < MovieScene->GetSpawnableCount(); ++i)
				{
					const FMovieSceneSpawnable& Sp = MovieScene->GetSpawnable(i);
					sol::table BT = BuildBindingTable(Sp.GetGuid(), Sp.GetName(), "spawnable");
					if (const UObject* Template = Sp.GetObjectTemplate())
						BT["template_class"] = TCHAR_TO_UTF8(*Template->GetClass()->GetName());
					// Spawnable config — mirrors configure("binding") accepted keys.
					switch (Sp.GetSpawnOwnership())
					{
						case ESpawnOwnership::InnerSequence: BT["spawn_ownership"] = "inner_sequence"; break;
						case ESpawnOwnership::RootSequence:  BT["spawn_ownership"] = "root_sequence";  break;
						case ESpawnOwnership::External:      BT["spawn_ownership"] = "external";       break;
					}
					BT["continuously_respawn"] = Sp.bContinuouslyRespawn;
					BT["net_addressable_name"] = Sp.bNetAddressableName;
					Spawn[i + 1] = BT;
					Result[FlatIdx++] = BT;
				}
				Result["spawnables"] = Spawn;

				// Master tracks
				TArray<UMovieSceneTrack*> MasterTracks = MovieScene->GetTracks();
				if (UMovieSceneTrack* CCT = MovieScene->GetCameraCutTrack())
					MasterTracks.Add(CCT);

				sol::table MasterTable = Lua.create_table();
				for (int32 i = 0; i < MasterTracks.Num(); ++i)
					MasterTable[i + 1] = BuildTrackTable(MasterTracks[i], i);
				Result["master_tracks"] = MasterTable;

				Session.Log(FString::Printf(TEXT("[OK] list(\"bindings\") -> %d possessables, %d spawnables, %d master tracks"),
					MovieScene->GetPossessableCount(), MovieScene->GetSpawnableCount(), MasterTracks.Num()));
				return Result;
			}

			// ---- list("channels", {binding?, track_type, track_index?, section_index?}) ----
			// Omit binding for master tracks (Fade, CameraCut, CinematicShot, Sub, etc.).
			if (FType.Equals(TEXT("channels"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value())
				{
					Session.Log(TEXT("[FAIL] list(\"channels\") -> {track_type=..} required (binding optional for master tracks)"));
					return sol::lua_nil;
				}
				sol::table P = Params.value();
				std::string BindingStr = P.get_or("binding", std::string());
				std::string TrackTypeStr = P.get_or("track_type", std::string());
				int32 TrackIdx = P.get_or("track_index", 0);

				if (TrackTypeStr.empty())
				{
					Session.Log(TEXT("[FAIL] list(\"channels\") -> track_type required"));
					return sol::lua_nil;
				}

				FString FBinding = NeoLuaStr::ToFString(BindingStr);
				FString FTrackType = NeoLuaStr::ToFString(TrackTypeStr);

				UClass* TrackClass = ResolveTrackClass(FTrackType);
				if (!TrackClass)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] list(\"channels\") -> unknown track type '%s'. Call list(\"track_types\") for valid names."), *FTrackType));
					return sol::lua_nil;
				}

				UMovieSceneTrack* TargetTrack = nullptr;
				if (BindingStr.empty())
				{
					// Master track path — same lookup as add("keyframe") at the master branch.
					TargetTrack = FindMasterTrackByClass(MovieScene, TrackClass, TrackIdx);
					if (!TargetTrack && TrackClass->IsChildOf(UMovieSceneCameraCutTrack::StaticClass()))
						TargetTrack = MovieScene->GetCameraCutTrack();
					if (!TargetTrack)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] list(\"channels\") -> no master %s track (index %d). Call list(\"bindings\") to see master_tracks."), *FTrackType, TrackIdx));
						return sol::lua_nil;
					}
				}
				else
				{
					FGuid BindingGuid = FindBindingByName(MovieScene, FBinding);
					if (!BindingGuid.IsValid())
					{
						Session.Log(FString::Printf(TEXT("[FAIL] list(\"channels\") -> binding '%s' not found. Call list(\"bindings\") to confirm exact binding names."), *FBinding));
						return sol::lua_nil;
					}

					const FMovieSceneBinding* Binding = MovieScene->FindBinding(BindingGuid);
					if (!Binding)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] list(\"channels\") -> binding '%s' has no tracks. Call list(\"bindings\") to inspect, or add(\"track\", {...}) first."), *FBinding));
						return sol::lua_nil;
					}

					int32 MatchCount = 0;
					for (UMovieSceneTrack* Track : Binding->GetTracks())
					{
						if (Track && Track->IsA(TrackClass))
						{
							if (MatchCount == TrackIdx) { TargetTrack = Track; break; }
							MatchCount++;
						}
					}
					if (!TargetTrack)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] list(\"channels\") -> no %s track (index %d) on '%s'"), *FTrackType, TrackIdx, *FBinding));
						return sol::lua_nil;
					}
				}

				const TArray<UMovieSceneSection*>& Sections = TargetTrack->GetAllSections();
				int32 SectionIdx = P.get_or("section_index", 0);
				if (SectionIdx < 0 || SectionIdx >= Sections.Num() || !Sections[SectionIdx])
				{
					Session.Log(FString::Printf(TEXT("[FAIL] list(\"channels\") -> section %d out of range (total %d)"), SectionIdx, Sections.Num()));
					return sol::lua_nil;
				}

				UMovieSceneSection* Section = Sections[SectionIdx];
				FMovieSceneChannelProxy& Proxy = Section->GetChannelProxy();
				sol::table Result = Lua.create_table();
				int32 GlobalIdx = 0;
				int32 LuaIdx = 1;

				auto AppendChannels = [&](auto Channels, auto MetaDataView, const char* TypeName)
				{
					for (int32 ci = 0; ci < Channels.Num(); ci++)
					{
						sol::table CE = Lua.create_table();
						CE["index"] = GlobalIdx;
						CE["type"] = TypeName;
						CE["keys"] = Channels[ci] ? Channels[ci]->GetNumKeys() : 0;
						if (ci < MetaDataView.Num())
						{
							FString ChName = MetaDataView[ci].Name.ToString();
							if (!ChName.IsEmpty())
								CE["name"] = TCHAR_TO_UTF8(*ChName);
							FString ChDisplay = MetaDataView[ci].DisplayText.ToString();
							if (!ChDisplay.IsEmpty())
								CE["display_name"] = TCHAR_TO_UTF8(*ChDisplay);
						}
						Result[LuaIdx++] = CE;
						GlobalIdx++;
					}
				};

				AppendChannels(Proxy.GetChannels<FMovieSceneDoubleChannel>(), Proxy.GetMetaData<FMovieSceneDoubleChannel>(), "Double");
				AppendChannels(Proxy.GetChannels<FMovieSceneFloatChannel>(), Proxy.GetMetaData<FMovieSceneFloatChannel>(), "Float");
				AppendChannels(Proxy.GetChannels<FMovieSceneBoolChannel>(), Proxy.GetMetaData<FMovieSceneBoolChannel>(), "Bool");
				AppendChannels(Proxy.GetChannels<FMovieSceneIntegerChannel>(), Proxy.GetMetaData<FMovieSceneIntegerChannel>(), "Integer");
				AppendChannels(Proxy.GetChannels<FMovieSceneByteChannel>(), Proxy.GetMetaData<FMovieSceneByteChannel>(), "Byte");
				AppendChannels(Proxy.GetChannels<FMovieSceneStringChannel>(), Proxy.GetMetaData<FMovieSceneStringChannel>(), "String");
				AppendChannels(Proxy.GetChannels<FMovieSceneTextChannel>(), Proxy.GetMetaData<FMovieSceneTextChannel>(), "Text");
				AppendChannels(Proxy.GetChannels<FMovieSceneObjectPathChannel>(), Proxy.GetMetaData<FMovieSceneObjectPathChannel>(), "ObjectPath");
				AppendChannels(Proxy.GetChannels<FMovieSceneEventChannel>(), Proxy.GetMetaData<FMovieSceneEventChannel>(), "Event");

				Session.Log(FString::Printf(TEXT("[OK] list(\"channels\") -> %d channels on %s:'%s'"), GlobalIdx, *FTrackType,
					FBinding.IsEmpty() ? TEXT("(master)") : *FBinding));
				return Result;
			}

			// ---- list("keyframes", {binding?, track_type, channel, track_index?, section?}) ----
			// Omit binding for master tracks.
			if (FType.Equals(TEXT("keyframes"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value())
				{
					Session.Log(TEXT("[FAIL] list(\"keyframes\") -> {track_type=.., channel=N} required (binding optional for master tracks)"));
					return sol::lua_nil;
				}
				sol::table P = Params.value();
				std::string BindingStr = P.get_or("binding", std::string());
				std::string TrackTypeStr = P.get_or("track_type", std::string());
				int32 ChannelIndex = P.get_or("channel", -1);
				int32 TrackIdx = P.get_or("track_index", 0);
				int32 SectionIdx = P.get_or("section", 0);

				if (TrackTypeStr.empty() || ChannelIndex < 0)
				{
					Session.Log(TEXT("[FAIL] list(\"keyframes\") -> track_type and channel required"));
					return sol::lua_nil;
				}

				FString FBinding = NeoLuaStr::ToFString(BindingStr);
				FString FTrackType = NeoLuaStr::ToFString(TrackTypeStr);

				UClass* TrackClass = ResolveTrackClass(FTrackType);
				if (!TrackClass)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] list(\"keyframes\") -> unknown track type '%s'. Call list(\"track_types\") for valid names."), *FTrackType));
					return sol::lua_nil;
				}

				UMovieSceneTrack* TargetTrack = nullptr;
				if (BindingStr.empty())
				{
					TargetTrack = FindMasterTrackByClass(MovieScene, TrackClass, TrackIdx);
					if (!TargetTrack && TrackClass->IsChildOf(UMovieSceneCameraCutTrack::StaticClass()))
						TargetTrack = MovieScene->GetCameraCutTrack();
					if (!TargetTrack)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] list(\"keyframes\") -> no master %s track (index %d)"), *FTrackType, TrackIdx));
						return sol::lua_nil;
					}
				}
				else
				{
					FGuid BindingGuid = FindBindingByName(MovieScene, FBinding);
					if (!BindingGuid.IsValid())
					{
						Session.Log(FString::Printf(TEXT("[FAIL] list(\"keyframes\") -> binding '%s' not found. Call list(\"bindings\") to confirm exact names."), *FBinding));
						return sol::lua_nil;
					}

					const FMovieSceneBinding* Binding = MovieScene->FindBinding(BindingGuid);
					if (!Binding)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] list(\"keyframes\") -> binding '%s' has no tracks. Call list(\"bindings\") to inspect, or add(\"track\", {...}) + add(\"keyframe\", {...}) first."), *FBinding));
						return sol::lua_nil;
					}

					int32 MatchCount = 0;
					for (UMovieSceneTrack* Track : Binding->GetTracks())
					{
						if (Track && Track->IsA(TrackClass))
						{
							if (MatchCount == TrackIdx) { TargetTrack = Track; break; }
							MatchCount++;
						}
					}
					if (!TargetTrack)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] list(\"keyframes\") -> no %s track (index %d) on '%s'"), *FTrackType, TrackIdx, *FBinding));
						return sol::lua_nil;
					}
				}

				const TArray<UMovieSceneSection*>& Sections = TargetTrack->GetAllSections();
				if (SectionIdx < 0 || SectionIdx >= Sections.Num() || !Sections[SectionIdx])
				{
					Session.Log(FString::Printf(TEXT("[FAIL] list(\"keyframes\") -> section %d out of range (total %d)"), SectionIdx, Sections.Num()));
					return sol::lua_nil;
				}

				UMovieSceneSection* Section = Sections[SectionIdx];
				FMovieSceneChannelProxy& Proxy = Section->GetChannelProxy();

				// Interp mode to string helper
				auto InterpToStr = [](ERichCurveInterpMode Mode) -> const char*
				{
					switch (Mode)
					{
					case RCIM_Linear:   return "linear";
					case RCIM_Constant: return "constant";
					case RCIM_Cubic:    return "cubic";
					case RCIM_None:     return "none";
					default:            return "unknown";
					}
				};

				// Tangent mode to string helper
				auto TangentToStr = [](ERichCurveTangentMode Mode) -> const char*
				{
					switch (Mode)
					{
					case RCTM_Auto:          return "auto";
					case RCTM_User:          return "user";
					case RCTM_Break:         return "break";
					case RCTM_None:          return "none";
					case RCTM_SmartAuto:     return "smart_auto";
					default:                 return "unknown";
					}
				};

				sol::table Result = Lua.create_table();
				int32 GlobalIdx = 0;
				bool bFound = false;

				// Double channels
				TArrayView<FMovieSceneDoubleChannel*> DCs = Proxy.GetChannels<FMovieSceneDoubleChannel>();
				if (!bFound && ChannelIndex >= GlobalIdx && ChannelIndex < GlobalIdx + DCs.Num())
				{
					FMovieSceneDoubleChannel* Ch = DCs[ChannelIndex - GlobalIdx];
					if (!Ch) { Session.Log(TEXT("[FAIL] list(\"keyframes\") -> null double channel")); return sol::lua_nil; }
					auto ChData = Ch->GetData();
					TArrayView<const FFrameNumber> Times = ChData.GetTimes();
					TArrayView<const FMovieSceneDoubleValue> Values = ChData.GetValues();
					for (int32 i = 0; i < Times.Num(); ++i)
					{
						sol::table KE = Lua.create_table();
						KE["time"] = FrameToSeconds(MovieScene, Times[i]);
						KE["value"] = Values[i].Value;
						KE["interp"] = InterpToStr(Values[i].InterpMode);
						KE["tangent_mode"] = TangentToStr(Values[i].TangentMode);
						Result[i + 1] = KE;
					}
					bFound = true;
				}
				GlobalIdx += DCs.Num();

				// Float channels
				TArrayView<FMovieSceneFloatChannel*> FCs = Proxy.GetChannels<FMovieSceneFloatChannel>();
				if (!bFound && ChannelIndex >= GlobalIdx && ChannelIndex < GlobalIdx + FCs.Num())
				{
					FMovieSceneFloatChannel* Ch = FCs[ChannelIndex - GlobalIdx];
					if (!Ch) { Session.Log(TEXT("[FAIL] list(\"keyframes\") -> null float channel")); return sol::lua_nil; }
					auto ChData = Ch->GetData();
					TArrayView<const FFrameNumber> Times = ChData.GetTimes();
					TArrayView<const FMovieSceneFloatValue> Values = ChData.GetValues();
					for (int32 i = 0; i < Times.Num(); ++i)
					{
						sol::table KE = Lua.create_table();
						KE["time"] = FrameToSeconds(MovieScene, Times[i]);
						KE["value"] = static_cast<double>(Values[i].Value);
						KE["interp"] = InterpToStr(Values[i].InterpMode);
						KE["tangent_mode"] = TangentToStr(Values[i].TangentMode);
						Result[i + 1] = KE;
					}
					bFound = true;
				}
				GlobalIdx += FCs.Num();

				// Bool channels
				TArrayView<FMovieSceneBoolChannel*> BCs = Proxy.GetChannels<FMovieSceneBoolChannel>();
				if (!bFound && ChannelIndex >= GlobalIdx && ChannelIndex < GlobalIdx + BCs.Num())
				{
					FMovieSceneBoolChannel* Ch = BCs[ChannelIndex - GlobalIdx];
					if (!Ch) { Session.Log(TEXT("[FAIL] list(\"keyframes\") -> null bool channel")); return sol::lua_nil; }
					auto ChData = Ch->GetData();
					TArrayView<const FFrameNumber> Times = ChData.GetTimes();
					TArrayView<const bool> Values = ChData.GetValues();
					for (int32 i = 0; i < Times.Num(); ++i)
					{
						sol::table KE = Lua.create_table();
						KE["time"] = FrameToSeconds(MovieScene, Times[i]);
						KE["value"] = Values[i];
						Result[i + 1] = KE;
					}
					bFound = true;
				}
				GlobalIdx += BCs.Num();

				// Integer channels
				TArrayView<FMovieSceneIntegerChannel*> ICs = Proxy.GetChannels<FMovieSceneIntegerChannel>();
				if (!bFound && ChannelIndex >= GlobalIdx && ChannelIndex < GlobalIdx + ICs.Num())
				{
					FMovieSceneIntegerChannel* Ch = ICs[ChannelIndex - GlobalIdx];
					if (!Ch) { Session.Log(TEXT("[FAIL] list(\"keyframes\") -> null integer channel")); return sol::lua_nil; }
					auto ChData = Ch->GetData();
					TArrayView<const FFrameNumber> Times = ChData.GetTimes();
					TArrayView<const int32> Values = ChData.GetValues();
					for (int32 i = 0; i < Times.Num(); ++i)
					{
						sol::table KE = Lua.create_table();
						KE["time"] = FrameToSeconds(MovieScene, Times[i]);
						KE["value"] = Values[i];
						Result[i + 1] = KE;
					}
					bFound = true;
				}
				GlobalIdx += ICs.Num();

				// Byte channels
				TArrayView<FMovieSceneByteChannel*> YCs = Proxy.GetChannels<FMovieSceneByteChannel>();
				if (!bFound && ChannelIndex >= GlobalIdx && ChannelIndex < GlobalIdx + YCs.Num())
				{
					FMovieSceneByteChannel* Ch = YCs[ChannelIndex - GlobalIdx];
					if (!Ch) { Session.Log(TEXT("[FAIL] list(\"keyframes\") -> null byte channel")); return sol::lua_nil; }
					auto ChData = Ch->GetData();
					TArrayView<const FFrameNumber> Times = ChData.GetTimes();
					TArrayView<const uint8> Values = ChData.GetValues();
					for (int32 i = 0; i < Times.Num(); ++i)
					{
						sol::table KE = Lua.create_table();
						KE["time"] = FrameToSeconds(MovieScene, Times[i]);
						KE["value"] = static_cast<int>(Values[i]);
						Result[i + 1] = KE;
					}
					bFound = true;
				}
				GlobalIdx += YCs.Num();

				// String channels
				TArrayView<FMovieSceneStringChannel*> SCs = Proxy.GetChannels<FMovieSceneStringChannel>();
				if (!bFound && ChannelIndex >= GlobalIdx && ChannelIndex < GlobalIdx + SCs.Num())
				{
					FMovieSceneStringChannel* Ch = SCs[ChannelIndex - GlobalIdx];
					if (!Ch) { Session.Log(TEXT("[FAIL] list(\"keyframes\") -> null string channel")); return sol::lua_nil; }
					auto ChData = Ch->GetData();
					TArrayView<const FFrameNumber> Times = ChData.GetTimes();
					TArrayView<const FString> Values = ChData.GetValues();
					for (int32 i = 0; i < Times.Num(); ++i)
					{
						sol::table KE = Lua.create_table();
						KE["time"] = FrameToSeconds(MovieScene, Times[i]);
						KE["value"] = std::string(TCHAR_TO_UTF8(*Values[i]));
						Result[i + 1] = KE;
					}
					bFound = true;
				}
				GlobalIdx += SCs.Num();

				// Text channels
				TArrayView<FMovieSceneTextChannel*> TCs = Proxy.GetChannels<FMovieSceneTextChannel>();
				if (!bFound && ChannelIndex >= GlobalIdx && ChannelIndex < GlobalIdx + TCs.Num())
				{
					FMovieSceneTextChannel* Ch = TCs[ChannelIndex - GlobalIdx];
					if (!Ch) { Session.Log(TEXT("[FAIL] list(\"keyframes\") -> null text channel")); return sol::lua_nil; }
					auto ChData = Ch->GetData();
					TArrayView<const FFrameNumber> Times = ChData.GetTimes();
					TArrayView<const FText> Values = ChData.GetValues();
					for (int32 i = 0; i < Times.Num(); ++i)
					{
						sol::table KE = Lua.create_table();
						KE["time"] = FrameToSeconds(MovieScene, Times[i]);
						KE["value"] = std::string(TCHAR_TO_UTF8(*Values[i].ToString()));
						Result[i + 1] = KE;
					}
					bFound = true;
				}
				GlobalIdx += TCs.Num();

				// ObjectPath channels
				TArrayView<FMovieSceneObjectPathChannel*> OPCs = Proxy.GetChannels<FMovieSceneObjectPathChannel>();
				if (!bFound && ChannelIndex >= GlobalIdx && ChannelIndex < GlobalIdx + OPCs.Num())
				{
					FMovieSceneObjectPathChannel* Ch = OPCs[ChannelIndex - GlobalIdx];
					if (!Ch) { Session.Log(TEXT("[FAIL] list(\"keyframes\") -> null object path channel")); return sol::lua_nil; }
					auto ChData = Ch->GetData();
					TArrayView<const FFrameNumber> Times = ChData.GetTimes();
					TArrayView<const FMovieSceneObjectPathChannelKeyValue> Values = ChData.GetValues();
					for (int32 i = 0; i < Times.Num(); ++i)
					{
						sol::table KE = Lua.create_table();
						KE["time"] = FrameToSeconds(MovieScene, Times[i]);
						UObject* Obj = Values[i].Get();
						KE["value"] = Obj ? std::string(TCHAR_TO_UTF8(*Obj->GetPathName())) : std::string("");
						Result[i + 1] = KE;
					}
					bFound = true;
				}
				GlobalIdx += OPCs.Num();

				// Event channels (list-only, values are BP function references)
				TArrayView<FMovieSceneEventChannel*> EVCs = Proxy.GetChannels<FMovieSceneEventChannel>();
				if (!bFound && ChannelIndex >= GlobalIdx && ChannelIndex < GlobalIdx + EVCs.Num())
				{
					FMovieSceneEventChannel* Ch = EVCs[ChannelIndex - GlobalIdx];
					if (!Ch) { Session.Log(TEXT("[FAIL] list(\"keyframes\") -> null event channel")); return sol::lua_nil; }
					auto ChData = Ch->GetData();
					TArrayView<const FFrameNumber> Times = ChData.GetTimes();
					TArrayView<const FMovieSceneEvent> Values = ChData.GetValues();
					for (int32 i = 0; i < Times.Num(); ++i)
					{
						sol::table KE = Lua.create_table();
						KE["time"] = FrameToSeconds(MovieScene, Times[i]);
						UFunction* Func = Values[i].Ptrs.Function;
						KE["value"] = Func ? std::string(TCHAR_TO_UTF8(*Func->GetName())) : std::string("(unbound)");
						Result[i + 1] = KE;
					}
					bFound = true;
				}
				GlobalIdx += EVCs.Num();

				if (!bFound)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] list(\"keyframes\") -> channel %d out of range (total %d)"), ChannelIndex, GlobalIdx));
					return sol::lua_nil;
				}

				int32 KeyCount = Result.size();
				Session.Log(FString::Printf(TEXT("[OK] list(\"keyframes\") -> %d keys on channel %d of %s:'%s'"), KeyCount, ChannelIndex, *FTrackType,
					FBinding.IsEmpty() ? TEXT("(master)") : *FBinding));
				return Result;
			}

			// ---- list("properties", {actor_name or binding}) ----
			if (FType.Equals(TEXT("properties"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value())
				{
					Session.Log(TEXT("[FAIL] list(\"properties\") -> {actor_name=.. or binding=..} required"));
					return sol::lua_nil;
				}
				std::string ActorStr = Params.value().get_or("actor_name", std::string());
				std::string BindingStr = Params.value().get_or("binding", std::string());
				if (ActorStr.empty() && BindingStr.empty())
				{
					Session.Log(TEXT("[FAIL] list(\"properties\") -> actor_name or binding required"));
					return sol::lua_nil;
				}

				// Resolve the target class: either from level actor or from binding template/class
				UClass* TargetClass = nullptr;
				FString DisplayName;
				TArray<UActorComponent*> TargetComponents;

				// Try level actor first (works for possessables)
				FString LookupName = ActorStr.empty() ? NeoLuaStr::ToFString(BindingStr) : NeoLuaStr::ToFString(ActorStr);
				AActor* Actor = FindActorByName(LookupName);
				if (Actor)
				{
					TargetClass = Actor->GetClass();
					DisplayName = Actor->GetActorLabel();
					Actor->GetComponents(TargetComponents);
				}
				else
				{
					// Not in level — look up binding in MovieScene (spawnables/possessables)
					FString FBindName = BindingStr.empty() ? NeoLuaStr::ToFString(ActorStr) : NeoLuaStr::ToFString(BindingStr);

					// Check spawnables (have template objects)
					for (int32 i = 0; i < MovieScene->GetSpawnableCount(); ++i)
					{
						const FMovieSceneSpawnable& Sp = MovieScene->GetSpawnable(i);
						if (Sp.GetName().Equals(FBindName, ESearchCase::IgnoreCase))
						{
							if (const UObject* Template = Sp.GetObjectTemplate())
							{
								TargetClass = Template->GetClass();
								DisplayName = Sp.GetName();
								// Get components from template actor
								if (const AActor* TemplateActor = Cast<AActor>(Template))
								{
									const_cast<AActor*>(TemplateActor)->GetComponents(TargetComponents);
								}
							}
							break;
						}
					}
					// Check possessables (have class info)
					if (!TargetClass)
					{
						for (int32 i = 0; i < MovieScene->GetPossessableCount(); ++i)
						{
							const FMovieScenePossessable& P = MovieScene->GetPossessable(i);
							if (P.GetName().Equals(FBindName, ESearchCase::IgnoreCase))
							{
								TargetClass = const_cast<UClass*>(P.GetPossessedObjectClass());
								DisplayName = P.GetName();
								break;
							}
						}
					}
				}

				if (!TargetClass)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] list(\"properties\") -> '%s' not found as level actor or binding"), *LookupName));
					return sol::lua_nil;
				}

				auto IsAnimatable = [](FProperty* Prop) -> bool
				{
					if (CastField<FFloatProperty>(Prop) || CastField<FDoubleProperty>(Prop)) return true;
					if (CastField<FBoolProperty>(Prop)) return true;
					if (CastField<FIntProperty>(Prop) || CastField<FByteProperty>(Prop)) return true;
					if (FStructProperty* SP = CastField<FStructProperty>(Prop))
					{
						FName SN = SP->Struct->GetFName();
						if (SN == NAME_Vector || SN == NAME_Rotator || SN == NAME_Color ||
							SN == NAME_LinearColor || SN == NAME_Transform)
							return true;
					}
					return false;
				};

				sol::table Result = Lua.create_table();

				// Actor/class properties
				sol::table ActorProps = Lua.create_table();
				int32 AI = 1;
				for (TFieldIterator<FProperty> It(TargetClass); It; ++It)
				{
					FProperty* Prop = *It;
					if (Prop && IsAnimatable(Prop) && Prop->HasAnyPropertyFlags(CPF_Edit))
					{
						sol::table PE = Lua.create_table();
						PE["name"] = TCHAR_TO_UTF8(*Prop->GetName());
						PE["type"] = TCHAR_TO_UTF8(*NeoStackToolUtils::GetPropertyTypeName(Prop));
						ActorProps[AI++] = PE;
					}
				}
				Result["actor_properties"] = ActorProps;

				// Component properties (from instance or template)
				sol::table Components = Lua.create_table();
				int32 CI = 1;
				for (UActorComponent* Comp : TargetComponents)
				{
					if (!Comp) continue;
					sol::table CompProps = Lua.create_table();
					int32 CPI = 1;
					for (TFieldIterator<FProperty> It(Comp->GetClass()); It; ++It)
					{
						FProperty* Prop = *It;
						if (Prop && IsAnimatable(Prop) && Prop->HasAnyPropertyFlags(CPF_Edit))
						{
							sol::table PE = Lua.create_table();
							PE["name"] = TCHAR_TO_UTF8(*Prop->GetName());
							PE["type"] = TCHAR_TO_UTF8(*NeoStackToolUtils::GetPropertyTypeName(Prop));
							CompProps[CPI++] = PE;
						}
					}
					if (CPI > 1)
					{
						sol::table CompEntry = Lua.create_table();
						CompEntry["name"] = TCHAR_TO_UTF8(*Comp->GetName());
						CompEntry["class"] = TCHAR_TO_UTF8(*Comp->GetClass()->GetName());
						CompEntry["properties"] = CompProps;
						Components[CI++] = CompEntry;
					}
				}
				Result["components"] = Components;

				Session.Log(FString::Printf(TEXT("[OK] list(\"properties\") -> '%s' (%s)"), *DisplayName, *TargetClass->GetName()));
				return Result;
			}

			// ---- list("marked_frames") ----
			if (FType.Equals(TEXT("marked_frames"), ESearchCase::IgnoreCase))
			{
				const TArray<FMovieSceneMarkedFrame>& Markers = MovieScene->GetMarkedFrames();
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Markers.Num(); ++i)
				{
					const FMovieSceneMarkedFrame& MF = Markers[i];
					sol::table Entry = Lua.create_table();
					Entry["index"] = i;
					Entry["frame"] = MF.FrameNumber.Value;
					Entry["time"] = FrameToSeconds(MovieScene, MF.FrameNumber);
					Entry["label"] = TCHAR_TO_UTF8(*MF.Label);
#if WITH_EDITORONLY_DATA
					Entry["comment"] = TCHAR_TO_UTF8(*MF.Comment);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
					Entry["use_custom_color"] = MF.bUseCustomColor;
					sol::table Color = Lua.create_table();
					Color["r"] = MF.CustomColor.R;
					Color["g"] = MF.CustomColor.G;
					Color["b"] = MF.CustomColor.B;
					Color["a"] = MF.CustomColor.A;
					Entry["color"] = Color;
#endif
#endif
					Entry["is_determinism_fence"] = MF.bIsDeterminismFence;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
					Entry["is_inclusive_time"] = MF.bIsInclusiveTime;
#endif
					Result[i + 1] = Entry;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"marked_frames\") -> %d markers, locked=%s"),
					Markers.Num(), MovieScene->AreMarkedFramesLocked() ? TEXT("true") : TEXT("false")));
				return Result;
			}

			// ---- list("folders") ----
			if (FType.Equals(TEXT("folders"), ESearchCase::IgnoreCase))
			{
				TArrayView<UMovieSceneFolder* const> RootFolders = MovieScene->GetRootFolders();

				// Build GUID->name map for binding lookups
				TMap<FGuid, FString> GuidToName;
				for (int32 i = 0; i < MovieScene->GetPossessableCount(); ++i)
				{
					const FMovieScenePossessable& P = MovieScene->GetPossessable(i);
					GuidToName.Add(P.GetGuid(), P.GetName());
				}
				for (int32 i = 0; i < MovieScene->GetSpawnableCount(); ++i)
				{
					const FMovieSceneSpawnable& Sp = MovieScene->GetSpawnable(i);
					GuidToName.Add(Sp.GetGuid(), Sp.GetName());
				}

				std::function<sol::table(UMovieSceneFolder*)> BuildFolderTable;
				BuildFolderTable = [&](UMovieSceneFolder* Folder) -> sol::table
				{
					sol::table FT = Lua.create_table();
					FT["name"] = TCHAR_TO_UTF8(*Folder->GetFolderName().ToString());
#if WITH_EDITORONLY_DATA
					sol::table Color = Lua.create_table();
					Color["r"] = Folder->GetFolderColor().R;
					Color["g"] = Folder->GetFolderColor().G;
					Color["b"] = Folder->GetFolderColor().B;
					Color["a"] = Folder->GetFolderColor().A;
					FT["color"] = Color;
					FT["sorting_order"] = Folder->GetSortingOrder();
#endif
					// Child folders
					TArrayView<UMovieSceneFolder* const> Children = Folder->GetChildFolders();
					if (Children.Num() > 0)
					{
						sol::table ChildTable = Lua.create_table();
						for (int32 ci = 0; ci < Children.Num(); ++ci)
						{
							if (Children[ci])
								ChildTable[ci + 1] = BuildFolderTable(Children[ci]);
						}
						FT["child_folders"] = ChildTable;
					}

					// Child bindings
					const TArray<FGuid>& Bindings = Folder->GetChildObjectBindings();
					if (Bindings.Num() > 0)
					{
						sol::table BindTable = Lua.create_table();
						for (int32 bi = 0; bi < Bindings.Num(); ++bi)
						{
							const FString* Name = GuidToName.Find(Bindings[bi]);
							BindTable[bi + 1] = TCHAR_TO_UTF8(Name ? **Name : *Bindings[bi].ToString());
						}
						FT["bindings"] = BindTable;
					}

					// Child tracks
					const TArray<UMovieSceneTrack*>& Tracks = Folder->GetChildTracks();
					if (Tracks.Num() > 0)
					{
						sol::table TrackTable = Lua.create_table();
						for (int32 ti = 0; ti < Tracks.Num(); ++ti)
						{
							if (Tracks[ti])
								TrackTable[ti + 1] = TCHAR_TO_UTF8(*GetFriendlyTrackName(Tracks[ti]->GetClass()));
						}
						FT["tracks"] = TrackTable;
					}

					FT["child_folder_count"] = Children.Num();
					FT["binding_count"] = Bindings.Num();
					FT["track_count"] = Folder->GetChildTracks().Num();
					return FT;
				};

				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < RootFolders.Num(); ++i)
				{
					if (RootFolders[i])
						Result[i + 1] = BuildFolderTable(RootFolders[i]);
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"folders\") -> %d root folders"), RootFolders.Num()));
				return Result;
			}

			// ---- list("binding_tags") ----
			if (FType.Equals(TEXT("binding_tags"), ESearchCase::IgnoreCase))
			{
				// Build GUID->name map for readable output
				TMap<FGuid, FString> GuidToName;
				for (int32 i = 0; i < MovieScene->GetPossessableCount(); ++i)
				{
					const FMovieScenePossessable& P = MovieScene->GetPossessable(i);
					GuidToName.Add(P.GetGuid(), P.GetName());
				}
				for (int32 i = 0; i < MovieScene->GetSpawnableCount(); ++i)
				{
					const FMovieSceneSpawnable& Sp = MovieScene->GetSpawnable(i);
					GuidToName.Add(Sp.GetGuid(), Sp.GetName());
				}

				const TMap<FName, FMovieSceneObjectBindingIDs>& TagMap = MovieScene->AllTaggedBindings();
				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				for (const auto& Pair : TagMap)
				{
					sol::table Entry = Lua.create_table();
					Entry["tag"] = TCHAR_TO_UTF8(*Pair.Key.ToString());
					sol::table Bindings = Lua.create_table();
					int32 BIdx = 1;
					for (const FMovieSceneObjectBindingID& BID : Pair.Value.IDs)
					{
						UE::MovieScene::FFixedObjectBindingID Fixed = BID.ReinterpretAsFixed();
						const FString* BindName = GuidToName.Find(Fixed.Guid);
						sol::table BEntry = Lua.create_table();
						BEntry["name"] = BindName ? TCHAR_TO_UTF8(**BindName) : TCHAR_TO_UTF8(*Fixed.Guid.ToString());
						BEntry["guid"] = TCHAR_TO_UTF8(*Fixed.Guid.ToString());
						Bindings[BIdx++] = BEntry;
					}
					Entry["bindings"] = Bindings;
					Result[Idx++] = Entry;
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"binding_tags\") -> %d tags"), Idx - 1));
				return Result;
			}

			// ---- list("section_groups") ----
			if (FType.Equals(TEXT("section_groups"), ESearchCase::IgnoreCase))
			{
				// Access SectionGroups via reflection (protected member)
				FProperty* SGProp = UMovieScene::StaticClass()->FindPropertyByName(TEXT("SectionGroups"));
				if (!SGProp)
				{
					Session.Log(TEXT("[FAIL] list(\"section_groups\") -> SectionGroups property not found"));
					return sol::lua_nil;
				}
				FArrayProperty* ArrayProp = CastField<FArrayProperty>(SGProp);
				if (!ArrayProp)
				{
					Session.Log(TEXT("[FAIL] list(\"section_groups\") -> SectionGroups is not an array property"));
					return sol::lua_nil;
				}

				FScriptArrayHelper ArrayHelper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(MovieScene));
				sol::table Result = Lua.create_table();
				int32 GroupIdx = 1;
				for (int32 i = 0; i < ArrayHelper.Num(); ++i)
				{
					const FMovieSceneSectionGroup* Group = reinterpret_cast<const FMovieSceneSectionGroup*>(ArrayHelper.GetRawPtr(i));
					if (!Group) continue;

					sol::table GEntry = Lua.create_table();
					GEntry["index"] = i;
					sol::table Sections = Lua.create_table();
					int32 SIdx = 1;
					for (auto It = Group->begin(); It != Group->end(); ++It)
					{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
						UMovieSceneSection* Sec = It->Get();
#else
						UMovieSceneSection* Sec = (*It).Get();
#endif
						if (!Sec) continue;
						sol::table SEntry = Lua.create_table();
						UMovieSceneTrack* Track = Cast<UMovieSceneTrack>(Sec->GetOuter());
						if (Track)
						{
							SEntry["track_type"] = TCHAR_TO_UTF8(*GetFriendlyTrackName(Track->GetClass()));
#if WITH_EDITORONLY_DATA
							SEntry["track_display_name"] = TCHAR_TO_UTF8(*Track->GetDisplayName().ToString());
#endif
						}
						SEntry["section_index"] = Track ? Track->GetAllSections().IndexOfByKey(Sec) : -1;
						Sections[SIdx++] = SEntry;
					}
					GEntry["sections"] = Sections;
					GEntry["section_count"] = SIdx - 1;
					Result[GroupIdx++] = GEntry;
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"section_groups\") -> %d groups"), GroupIdx - 1));
				return Result;
			}

#if WITH_EDITORONLY_DATA
			// ---- list("node_groups") ----
			if (FType.Equals(TEXT("node_groups"), ESearchCase::IgnoreCase))
			{
				UMovieSceneNodeGroupCollection& NGC = MovieScene->GetNodeGroups();
				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				for (UMovieSceneNodeGroup* NG : NGC)
				{
					if (!NG) continue;
					sol::table Entry = Lua.create_table();
					Entry["name"] = TCHAR_TO_UTF8(*NG->GetName().ToString());
					Entry["enable_filter"] = NG->GetEnableFilter();

					sol::table Nodes = Lua.create_table();
					int32 NIdx = 1;
					for (const FString& Path : NG->GetNodes())
					{
						Nodes[NIdx++] = TCHAR_TO_UTF8(*Path);
					}
					Entry["nodes"] = Nodes;
					Entry["node_count"] = NIdx - 1;
					Result[Idx++] = Entry;
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"node_groups\") -> %d groups"), Idx - 1));
				return Result;
			}
#endif

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: track_types, bindings, channels, keyframes, properties, marked_frames, folders, binding_tags, section_groups, node_groups"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// add(type, params)
		// ================================================================
		AssetObj.set_function("add", [Sequence, MovieScene, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = NeoLuaStr::ToFString(Type);

			// ---- add("binding", {actor_name, name?, component_name?}) ----
			if (FType.Equals(TEXT("binding"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"binding\") -> {actor_name=..} required")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string ActorStr = P.get_or("actor_name", std::string());
				if (ActorStr.empty()) { Session.Log(TEXT("[FAIL] add(\"binding\") -> actor_name required")); return sol::lua_nil; }

				FString FActorName = NeoLuaStr::ToFString(ActorStr);
				std::string NameStr = P.get_or("name", std::string());
				std::string CompStr = P.get_or("component_name", std::string());

				FString FName = NameStr.empty()
					? (CompStr.empty() ? FActorName : NeoLuaStr::ToFString(CompStr))
					: NeoLuaStr::ToFString(NameStr);

				// Check duplicate
				if (FindBindingByName(MovieScene, FName).IsValid())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"binding\") -> '%s' already exists"), *FName));
					return sol::lua_nil;
				}

				AActor* Actor = FindActorByName(FActorName);
				bool bSpawnable = P.get_or("spawnable", false);

				// For possessables, the actor must exist in the level
				if (!Actor && !bSpawnable)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"binding\") -> actor '%s' not found in level (use spawnable=true to create from class name)"), *FActorName));
					return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "SeqAddBinding", "Add Sequencer Binding"));
				MovieScene->Modify();

				// Component binding (requires actor in level)
				if (!CompStr.empty())
				{
					if (!Actor)
					{
						Session.Log(TEXT("[FAIL] add(\"binding\") -> component bindings require an actor in the level"));
						return sol::lua_nil;
					}

					FString FComp = NeoLuaStr::ToFString(CompStr);
					UActorComponent* Component = nullptr;

					// Exact name match
					for (UActorComponent* Comp : Actor->GetComponents())
					{
						if (Comp && Comp->GetName().Equals(FComp, ESearchCase::IgnoreCase))
						{ Component = Comp; break; }
					}
					// Partial match
					if (!Component)
					{
						for (UActorComponent* Comp : Actor->GetComponents())
						{
							if (Comp && Comp->GetName().Contains(FComp))
							{ Component = Comp; break; }
						}
					}
					// Class name match
					if (!Component)
					{
						for (UActorComponent* Comp : Actor->GetComponents())
						{
							if (Comp && Comp->GetClass()->GetName().Contains(FComp))
							{ Component = Comp; break; }
						}
					}

					if (!Component)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] add(\"binding\") -> component '%s' not found on '%s'"), *FComp, *Actor->GetActorLabel()));
						return sol::lua_nil;
					}

					// Find or create parent actor binding
					FString ActorLabel = Actor->GetActorLabel();
					FGuid ActorGuid = FindBindingByName(MovieScene, ActorLabel);
					if (!ActorGuid.IsValid())
						ActorGuid = FindBindingByName(MovieScene, Actor->GetName());
					if (!ActorGuid.IsValid())
					{
						ActorGuid = MovieScene->AddPossessable(ActorLabel, Actor->GetClass());
						if (ActorGuid.IsValid())
							Sequence->BindPossessableObject(ActorGuid, *Actor, Actor->GetWorld());
					}
					if (!ActorGuid.IsValid())
					{
						Session.Log(FString::Printf(TEXT("[FAIL] add(\"binding\") -> failed to create parent actor binding for '%s'. Verify the actor exists in the current editor world and retry with its exact label from the World Outliner."), *Actor->GetActorLabel()));
						return sol::lua_nil;
					}

					FGuid CompGuid = MovieScene->AddPossessable(FName, Component->GetClass());
					if (!CompGuid.IsValid())
					{
						Session.Log(FString::Printf(TEXT("[FAIL] add(\"binding\") -> failed to create component binding for '%s' on '%s'. Call list(\"properties\", {actor_name=\"%s\"}) and retry with the exact component name."),
							*Component->GetName(), *Actor->GetActorLabel(), *Actor->GetActorLabel()));
						return sol::lua_nil;
					}
					FMovieScenePossessable* Poss = MovieScene->FindPossessable(CompGuid);
					if (Poss) Poss->SetParent(ActorGuid, MovieScene);
					Sequence->BindPossessableObject(CompGuid, *Component, Actor);
					Sequence->MarkPackageDirty();

					Session.Log(FString::Printf(TEXT("[OK] add(\"binding\", name=\"%s\", component=\"%s\" on \"%s\")"),
						*FName, *Component->GetName(), *Actor->GetActorLabel()));

					sol::table R = Lua.create_table();
					R["name"] = TCHAR_TO_UTF8(*FName);
					R["guid"] = TCHAR_TO_UTF8(*CompGuid.ToString());
					return R;
				}

				// Actor binding — spawnable
				if (bSpawnable)
				{
					UObject* Template = nullptr;

					if (Actor)
					{
						// Actor exists in level — duplicate it as spawnable template
						Template = Sequence->MakeSpawnableTemplateFromInstance(*Actor, ::FName(*FName));
					}
					else
					{
						// Actor not in level — resolve as class name and create template from CDO
						UClass* ActorClass = ResolveActorClassByName(FActorName);
						if (!ActorClass)
						{
							Session.Log(FString::Printf(TEXT("[FAIL] add(\"binding\") -> '%s' not found as level actor or actor class"), *FActorName));
							return sol::lua_nil;
						}
						// Create a fresh template object owned by MovieScene from the class defaults
						Template = NewObject<UObject>(MovieScene, ActorClass, ::FName(*FName));
					}

					if (!Template)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] add(\"binding\") -> failed to create spawnable template for '%s'. Pass an existing level actor label, a short class name, or a full /Script or /Game class path."), *FActorName));
						return sol::lua_nil;
					}

					FGuid NewGuid = MovieScene->AddSpawnable(FName, *Template);
					if (!NewGuid.IsValid())
					{
						Session.Log(FString::Printf(TEXT("[FAIL] add(\"binding\") -> failed to create spawnable for class '%s'. Verify the class is an AActor subclass."), *FActorName));
						return sol::lua_nil;
					}

					// Apply optional spawnable config at create time. These can also be set
					// later via configure("binding", name, {...}); duplicated here for ergonomics
					// so callers can do a single add() call.
					if (FMovieSceneSpawnable* NewSpawnable = MovieScene->FindSpawnable(NewGuid))
					{
						std::string OwnStr = P.get_or<std::string>("spawn_ownership", "");
						if (!OwnStr.empty())
						{
							FString Own = NeoLuaStr::ToFString(OwnStr);
							if (Own.Equals(TEXT("inner_sequence"), ESearchCase::IgnoreCase) || Own.Equals(TEXT("InnerSequence"), ESearchCase::IgnoreCase))
								NewSpawnable->SetSpawnOwnership(ESpawnOwnership::InnerSequence);
							else if (Own.Equals(TEXT("root_sequence"), ESearchCase::IgnoreCase) || Own.Equals(TEXT("RootSequence"), ESearchCase::IgnoreCase) ||
									 Own.Equals(TEXT("master_sequence"), ESearchCase::IgnoreCase) || Own.Equals(TEXT("MasterSequence"), ESearchCase::IgnoreCase))
								NewSpawnable->SetSpawnOwnership(ESpawnOwnership::RootSequence);
							else if (Own.Equals(TEXT("external"), ESearchCase::IgnoreCase) || Own.Equals(TEXT("External"), ESearchCase::IgnoreCase))
								NewSpawnable->SetSpawnOwnership(ESpawnOwnership::External);
						}
						sol::optional<bool> ContRespawnOpt = P.get<sol::optional<bool>>("continuously_respawn");
						if (ContRespawnOpt.has_value()) NewSpawnable->bContinuouslyRespawn = ContRespawnOpt.value();
						sol::optional<bool> NetOpt = P.get<sol::optional<bool>>("net_addressable_name");
						if (NetOpt.has_value()) NewSpawnable->bNetAddressableName = NetOpt.value();
					}
					Sequence->MarkPackageDirty();

					Session.Log(FString::Printf(TEXT("[OK] add(\"binding\", name=\"%s\", actor=\"%s\", spawnable=true)"),
						*FName, *FActorName));

					sol::table R = Lua.create_table();
					R["name"] = TCHAR_TO_UTF8(*FName);
					R["guid"] = TCHAR_TO_UTF8(*NewGuid.ToString());
					R["type"] = "spawnable";
					return R;
				}

				// Possessable (default)
				FGuid NewGuid = MovieScene->AddPossessable(FName, Actor->GetClass());
				if (!NewGuid.IsValid())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"binding\") -> failed to create possessable for '%s'. Verify the actor is in the current editor world and retry with its exact World Outliner label."),
						*Actor->GetActorLabel()));
					return sol::lua_nil;
				}
				Sequence->BindPossessableObject(NewGuid, *Actor, Actor->GetWorld());
				Sequence->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] add(\"binding\", name=\"%s\", actor=\"%s\")"),
					*FName, *Actor->GetActorLabel()));

				sol::table R = Lua.create_table();
				R["name"] = TCHAR_TO_UTF8(*FName);
				R["guid"] = TCHAR_TO_UTF8(*NewGuid.ToString());
				R["type"] = "possessable";
				return R;
			}

			// ---- add("track", {...}) ----
			if (FType.Equals(TEXT("track"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"track\") -> {track_type=..} required")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string TrackTypeStr = P.get_or("track_type", std::string());
				if (TrackTypeStr.empty()) { Session.Log(TEXT("[FAIL] add(\"track\") -> track_type required")); return sol::lua_nil; }

				FString FTrackType = NeoLuaStr::ToFString(TrackTypeStr);
				UClass* TrackClass = ResolveTrackClass(FTrackType);
				if (!TrackClass)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"track\") -> unknown track type '%s'. Call list(\"track_types\") for valid names."), *FTrackType));
					return sol::lua_nil;
				}
				if (!IsTrackClassSupportedBySequence(Sequence, TrackClass))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"track\") -> track type '%s' is not supported by this sequence. Call list(\"track_types\") for valid names."), *FTrackType));
					return sol::lua_nil;
				}

				std::string BindingStr = P.get_or("binding", std::string());
				double StartTime = P.get_or("start_time", 0.0);
				double EndTime = P.get_or("end_time", -1.0);
				const bool bHasEndTime = EndTime >= 0.0;
				const bool bIsMaster = BindingStr.empty();
				const bool bIsCameraCut = TrackClass->IsChildOf(UMovieSceneCameraCutTrack::StaticClass());
				const bool bIsCinematicShot = TrackClass->IsChildOf(UMovieSceneCinematicShotTrack::StaticClass());

				// Pre-validate the frame range BEFORE any MovieScene mutation. Previously
				// end_time was checked inside each per-type branch after AddTrack / AddNewCameraCut
				// / AddSequenceOnRow had already run, which left stray tracks/sections behind
				// when the call reported failure.
				const FFrameNumber PreStartFrame = SecondsToFrame(MovieScene, StartTime);
				if (bHasEndTime)
				{
					const FFrameNumber PreEndFrame = SecondsToFrame(MovieScene, EndTime);
					if (PreEndFrame <= PreStartFrame)
					{
						Session.Log(TEXT("[FAIL] add(\"track\") -> end_time must be > start_time"));
						return sol::lua_nil;
					}
				}

				if (bIsCameraCut && !bIsMaster)
				{
					Session.Log(TEXT("[FAIL] add(\"track\") -> CameraCut is a master track; omit binding and set camera_binding"));
					return sol::lua_nil;
				}
				if (bIsCinematicShot && !bIsMaster)
				{
					Session.Log(TEXT("[FAIL] add(\"track\") -> CinematicShot is a master track; omit binding"));
					return sol::lua_nil;
				}

				// ----------------------------------------------------------------
				// Validate required inputs BEFORE mutating the MovieScene.
				// Previously the flow was: AddTrack → validate → fail-return, which
				// left a stray empty master track behind on validation failure
				// (UMovieScene::AddTrack mutates and broadcasts OnTrackAdded at
				// MovieScene.cpp:1283/1342). We now pre-resolve CameraCut camera_binding
				// and CinematicShot sequence+circular-ref checks first.
				// ----------------------------------------------------------------
				FGuid PreResolvedCamGuid; // CameraCut only
				ULevelSequence* PreResolvedShotSeq = nullptr; // CinematicShot only

				if (bIsCameraCut)
				{
					std::string CamBindStr = P.get_or("camera_binding", std::string());
					if (CamBindStr.empty())
					{
						Session.Log(TEXT("[FAIL] add(\"track\") -> CameraCut requires camera_binding"));
						return sol::lua_nil;
					}
					FString FCamBind = NeoLuaStr::ToFString(CamBindStr);
					PreResolvedCamGuid = FindBindingByName(MovieScene, FCamBind);
					if (!PreResolvedCamGuid.IsValid())
					{
						Session.Log(FString::Printf(TEXT("[FAIL] add(\"track\") -> camera_binding '%s' not found. Call list(\"bindings\") to confirm exact binding names."), *FCamBind));
						return sol::lua_nil;
					}
				}
				else if (bIsCinematicShot)
				{
					std::string SeqPathStr = P.get_or("sequence", std::string());
					if (SeqPathStr.empty())
					{
						Session.Log(TEXT("[FAIL] add(\"track\") -> CinematicShot requires sequence (path to LevelSequence asset)"));
						return sol::lua_nil;
					}
					PreResolvedShotSeq = NeoLuaAsset::Resolve<ULevelSequence>(NeoLuaStr::ToFString(SeqPathStr));
					if (!PreResolvedShotSeq)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] add(\"track\") -> failed to load sequence '%s'. Pass a LevelSequence asset path like /Game/Cinematics/Shot01, or create one with create_asset(path, \"LevelSequence\")."),
							UTF8_TO_TCHAR(SeqPathStr.c_str())));
						return sol::lua_nil;
					}
					if (PreResolvedShotSeq == Sequence)
					{
						Session.Log(TEXT("[FAIL] add(\"track\") -> cannot add a sequence as its own cinematic shot"));
						return sol::lua_nil;
					}
					UMovieScene* ShotMS = PreResolvedShotSeq->GetMovieScene();
					if (ShotMS)
					{
						UMovieSceneSubTrack* ShotSubTrack = ShotMS->FindTrack<UMovieSceneSubTrack>();
						if (ShotSubTrack && ShotSubTrack->ContainsSequence(*Sequence, true))
						{
							Session.Log(TEXT("[FAIL] add(\"track\") -> would create circular reference"));
							return sol::lua_nil;
						}
						UMovieSceneCinematicShotTrack* ShotCinTrack = ShotMS->FindTrack<UMovieSceneCinematicShotTrack>();
						if (ShotCinTrack && ShotCinTrack->ContainsSequence(*Sequence, true))
						{
							Session.Log(TEXT("[FAIL] add(\"track\") -> would create circular reference"));
							return sol::lua_nil;
						}
					}
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "SeqAddTrack", "Add Sequencer Track"));

				UMovieSceneTrack* NewTrack = nullptr;
				FGuid BindingGuid;

				if (bIsMaster)
				{
					if (bIsCameraCut)
					{
						NewTrack = MovieScene->GetCameraCutTrack();
						if (!NewTrack)
							NewTrack = MovieScene->AddCameraCutTrack(TrackClass);
					}
					else
					{
						NewTrack = MovieScene->AddTrack(TrackClass);
					}
				}
				else
				{
					FString FBinding = NeoLuaStr::ToFString(BindingStr);
					BindingGuid = FindBindingByName(MovieScene, FBinding);
					if (!BindingGuid.IsValid())
					{
						Session.Log(FString::Printf(TEXT("[FAIL] add(\"track\") -> binding '%s' not found. Call list(\"bindings\") to confirm exact binding names."), *FBinding));
						return sol::lua_nil;
					}
					NewTrack = MovieScene->AddTrack(TrackClass, BindingGuid);
				}

				if (!NewTrack)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"track\") -> failed to create %s track. Call list(\"track_types\") for valid names."), *FTrackType));
					return sol::lua_nil;
				}

				FFrameNumber StartFrame = SecondsToFrame(MovieScene, StartTime);
				FFrameNumber EndFrame = StartFrame;
				UMovieSceneSection* Section = nullptr;

				if (bIsCameraCut)
				{
					// Pre-validated: PreResolvedCamGuid is valid.
					UMovieSceneCameraCutTrack* CCTrack = Cast<UMovieSceneCameraCutTrack>(NewTrack);
					if (!CCTrack)
					{
						Session.Log(TEXT("[FAIL] add(\"track\") -> CameraCut track class did not produce UMovieSceneCameraCutTrack. Retry with track_type=\"CameraCut\" from list(\"track_types\")."));
						return sol::lua_nil;
					}

					UE::MovieScene::FRelativeObjectBindingID RelID(PreResolvedCamGuid);
					FMovieSceneObjectBindingID CamBindID(RelID);
					UMovieSceneCameraCutSection* CCS = CCTrack->AddNewCameraCut(CamBindID, StartFrame);
					if (!CCS)
					{
						Session.Log(TEXT("[FAIL] add(\"track\") -> failed to add CameraCut section"));
						return sol::lua_nil;
					}
					Section = CCS;

					if (bHasEndTime)
					{
						EndFrame = SecondsToFrame(MovieScene, EndTime);
						if (EndFrame <= StartFrame)
						{
							Session.Log(TEXT("[FAIL] add(\"track\") -> end_time must be > start_time"));
							return sol::lua_nil;
						}
						CCS->SetRange(TRange<FFrameNumber>(StartFrame, EndFrame));
						CCTrack->RearrangeAllSections();
					}
				}
				else if (bIsCinematicShot)
				{
					// Pre-validated: PreResolvedShotSeq is valid and non-circular.
					ULevelSequence* ShotSeq = PreResolvedShotSeq;
					UMovieSceneCinematicShotTrack* ShotTrack = Cast<UMovieSceneCinematicShotTrack>(NewTrack);
					if (!ShotTrack)
					{
						Session.Log(TEXT("[FAIL] add(\"track\") -> internal CinematicShot cast failed"));
						return sol::lua_nil;
					}

					UMovieScene* ShotMS = ShotSeq->GetMovieScene();
					// Circular-reference checks already ran pre-mutation — no-op here.

					// Calculate duration from shot sequence's playback range
					int32 Duration = 0;
					if (ShotMS)
					{
						const TRange<FFrameNumber> ShotRange = ShotMS->GetPlaybackRange();
						if (ShotRange.HasLowerBound() && ShotRange.HasUpperBound())
						{
							double ShotDurationSec = FrameToSeconds(ShotMS, ShotRange.GetUpperBoundValue()) -
								FrameToSeconds(ShotMS, ShotRange.GetLowerBoundValue());
							Duration = SecondsToFrame(MovieScene, ShotDurationSec).Value;
						}
					}
					if (Duration <= 0)
					{
						Duration = SecondsToFrame(MovieScene, 5.0).Value; // Default 5s
					}

					if (bHasEndTime)
					{
						EndFrame = SecondsToFrame(MovieScene, EndTime);
						if (EndFrame <= StartFrame)
						{
							Session.Log(TEXT("[FAIL] add(\"track\") -> end_time must be > start_time"));
							return sol::lua_nil;
						}
						Duration = (EndFrame - StartFrame).Value;
					}

					int32 RowIndex = P.get_or("row_index", -1);
					UMovieSceneSubSection* ShotSection = ShotTrack->AddSequenceOnRow(ShotSeq, StartFrame, Duration, RowIndex);
					if (!ShotSection)
					{
						Session.Log(TEXT("[FAIL] add(\"track\") -> failed to create cinematic shot section"));
						return sol::lua_nil;
					}
					Section = ShotSection;

					// Set shot display name if provided
					if (UMovieSceneCinematicShotSection* CineSec = Cast<UMovieSceneCinematicShotSection>(ShotSection))
					{
						std::string ShotName = P.get_or("shot_display_name", std::string());
						if (!ShotName.empty())
						{
							CineSec->SetShotDisplayName(NeoLuaStr::ToFString(ShotName));
						}
					}

					ShotTrack->SortSections();
				}
				else
				{
					if (bHasEndTime)
						EndFrame = SecondsToFrame(MovieScene, EndTime);
					else
					{
						const TRange<FFrameNumber> PR = MovieScene->GetPlaybackRange();
						if (PR.HasUpperBound())
							EndFrame = PR.GetUpperBoundValue();
						else
						{
							const int32 OneSec = FMath::Max(1, static_cast<int32>(FMath::FloorToDouble(MovieScene->GetTickResolution().AsDecimal())));
							EndFrame = StartFrame + FFrameNumber(OneSec);
						}
					}
					if (EndFrame <= StartFrame)
					{
						Session.Log(TEXT("[FAIL] add(\"track\") -> end_time must be > start_time"));
						return sol::lua_nil;
					}

					Section = NewTrack->CreateNewSection();
					if (Section)
					{
						Section->SetRange(TRange<FFrameNumber>(StartFrame, EndFrame));
						NewTrack->AddSection(*Section);

						// Audio configuration
						if (UMovieSceneAudioSection* AudioSec = Cast<UMovieSceneAudioSection>(Section))
						{
							std::string SoundStr = P.get_or("sound_asset", std::string());
							if (!SoundStr.empty())
							{
								USoundBase* Sound = NeoLuaAsset::Resolve<USoundBase>(NeoLuaStr::ToFString(SoundStr));
								if (Sound) AudioSec->SetSound(Sound);
							}
							FMovieSceneChannelProxy& CP = AudioSec->GetChannelProxy();
							TArrayView<FMovieSceneFloatChannel*> FC = CP.GetChannels<FMovieSceneFloatChannel>();
							double Vol = P.get_or("sound_volume", 1.0);
							double Pitch = P.get_or("sound_pitch", 1.0);
							if (FC.Num() > 0 && Vol != 1.0) FC[0]->SetDefault(static_cast<float>(Vol));
							if (FC.Num() > 1 && Pitch != 1.0) FC[1]->SetDefault(static_cast<float>(Pitch));
						}

						// Skeletal animation configuration
						if (UMovieSceneSkeletalAnimationSection* AnimSec = Cast<UMovieSceneSkeletalAnimationSection>(Section))
						{
							std::string AnimStr = P.get_or("animation_asset", std::string());
							if (!AnimStr.empty())
							{
								UAnimSequenceBase* Anim = NeoLuaAsset::Resolve<UAnimSequenceBase>(NeoLuaStr::ToFString(AnimStr));
								if (Anim) AnimSec->Params.Animation = Anim;
							}
						}

						// Property track configuration
						if (UMovieScenePropertyTrack* PropTrack = Cast<UMovieScenePropertyTrack>(NewTrack))
						{
							std::string PropName = P.get_or("property_name", std::string());
							std::string PropPath = P.get_or("property_path", std::string());
							if (PropPath.empty()) PropPath = PropName;
							if (!PropName.empty())
								PropTrack->SetPropertyNameAndPath(FName(NeoLuaStr::ToFString(PropName)), NeoLuaStr::ToFString(PropPath));
						}
					}
				}

				Sequence->MarkPackageDirty();
				FString Friendly = GetFriendlyTrackName(TrackClass);
				Session.Log(FString::Printf(TEXT("[OK] add(\"track\", type=\"%s\", binding=\"%s\")"),
					*Friendly, BindingStr.empty() ? TEXT("(master)") : UTF8_TO_TCHAR(BindingStr.c_str())));
				return sol::make_object(Lua, true);
			}

			// ---- add("keyframe", {...}) ----
			if (FType.Equals(TEXT("keyframe"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"keyframe\") -> {track_type, channel_index, time, value} required")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string BindStr = P.get_or("binding", std::string());
				std::string TrackTypeStr = P.get_or("track_type", std::string());
				if (TrackTypeStr.empty())
				{
					Session.Log(TEXT("[FAIL] add(\"keyframe\") -> track_type required"));
					return sol::lua_nil;
				}

				int32 ChannelIndex = P.get_or("channel_index", 0);
				double Time = P.get_or("time", 0.0);
				double Value = P.get_or("value", 0.0);
				// For bool channels: prefer explicit bool_value, fall back to value
				// (value=true in Lua is coerced to 1.0 by sol2, so check it)
				bool BoolValue = false;
				sol::optional<bool> ExplicitBool = P.get<sol::optional<bool>>("bool_value");
				if (ExplicitBool.has_value())
				{
					BoolValue = ExplicitBool.value();
				}
				else
				{
					// Interpret value param as bool: true, or non-zero number
					sol::object ValObj = P["value"];
					if (ValObj.is<bool>())
						BoolValue = ValObj.as<bool>();
					else if (ValObj.is<double>())
						BoolValue = (ValObj.as<double>() != 0.0);
				}
				std::string StringValue = P.get_or("string_value", std::string());
				std::string ObjectPath = P.get_or("object_path", std::string());
				std::string InterpStr = P.get_or("interp", std::string("cubic"));
				int32 TrackIdx = P.get_or("track_index", -1);

				FString FBinding = NeoLuaStr::ToFString(BindStr);
				FString FTrackType = NeoLuaStr::ToFString(TrackTypeStr);
				FString FInterp = NeoLuaStr::ToFString(InterpStr);

				UClass* TrackClass = ResolveTrackClass(FTrackType);
				if (!TrackClass)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"keyframe\") -> unknown track type '%s'. Call list(\"track_types\") for valid names."), *FTrackType));
					return sol::lua_nil;
				}

				UMovieSceneTrack* TargetTrack = nullptr;

				if (BindStr.empty())
				{
					// No binding — search master tracks (Fade, CameraCut, Sub, etc.)
					TargetTrack = FindMasterTrackByClass(MovieScene, TrackClass, TrackIdx);
					if (!TargetTrack)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] add(\"keyframe\") -> no master %s track found"), *FTrackType));
						return sol::lua_nil;
					}
				}
				else
				{
					// Binding specified — search binding tracks
					FGuid BindingGuid = FindBindingByName(MovieScene, FBinding);
					if (!BindingGuid.IsValid())
					{
						Session.Log(FString::Printf(TEXT("[FAIL] add(\"keyframe\") -> binding '%s' not found. Call list(\"bindings\") to confirm the exact binding name."), *FBinding));
						return sol::lua_nil;
					}

					const FMovieSceneBinding* Binding = MovieScene->FindBinding(BindingGuid);
					if (!Binding)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] add(\"keyframe\") -> no tracks for '%s'"), *FBinding));
						return sol::lua_nil;
					}

					int32 MatchCount = 0;
					for (UMovieSceneTrack* Track : Binding->GetTracks())
					{
						if (Track && Track->IsA(TrackClass))
						{
							if (TrackIdx < 0 || MatchCount == TrackIdx) { TargetTrack = Track; break; }
							MatchCount++;
						}
					}
					if (!TargetTrack)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] add(\"keyframe\") -> no %s track on '%s'"), *FTrackType, *FBinding));
						return sol::lua_nil;
					}
				}

				int32 SectionIdx = P.get_or("section_index", 0);
				const TArray<UMovieSceneSection*>& Sections = TargetTrack->GetAllSections();
				if (SectionIdx < 0 || SectionIdx >= Sections.Num() || !Sections[SectionIdx])
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"keyframe\") -> section %d out of range (total %d)"), SectionIdx, Sections.Num()));
					return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "SeqAddKey", "Add Sequencer Keyframe"));

				UMovieSceneSection* Section = Sections[SectionIdx];
				Section->Modify();
				FFrameNumber Frame = SecondsToFrame(MovieScene, Time);
				FMovieSceneChannelProxy& Proxy = Section->GetChannelProxy();
				int32 GlobalIdx = 0;

				// Double channels
				TArrayView<FMovieSceneDoubleChannel*> DCs = Proxy.GetChannels<FMovieSceneDoubleChannel>();
				if (ChannelIndex >= GlobalIdx && ChannelIndex < GlobalIdx + DCs.Num())
				{
					FMovieSceneDoubleChannel* Ch = DCs[ChannelIndex - GlobalIdx];
					if (!Ch) { Session.Log(TEXT("[FAIL] add(\"keyframe\") -> null double channel")); return sol::lua_nil; }
					if (FInterp == TEXT("linear")) Ch->AddLinearKey(Frame, Value);
					else if (FInterp == TEXT("constant")) Ch->AddConstantKey(Frame, Value);
					else Ch->AddCubicKey(Frame, Value, RCTM_Auto);
					Sequence->MarkPackageDirty();
					Session.Log(FString::Printf(TEXT("[OK] add(\"keyframe\", ch=%d, t=%.2f, v=%.4f, %s)"), ChannelIndex, Time, Value, *FInterp));
					return sol::make_object(Lua, true);
				}
				GlobalIdx += DCs.Num();

				// Float channels
				TArrayView<FMovieSceneFloatChannel*> FCs = Proxy.GetChannels<FMovieSceneFloatChannel>();
				if (ChannelIndex >= GlobalIdx && ChannelIndex < GlobalIdx + FCs.Num())
				{
					FMovieSceneFloatChannel* Ch = FCs[ChannelIndex - GlobalIdx];
					if (!Ch) { Session.Log(TEXT("[FAIL] add(\"keyframe\") -> null float channel")); return sol::lua_nil; }
					FMovieSceneFloatValue FV(static_cast<float>(Value));
					if (FInterp == TEXT("linear")) FV.InterpMode = RCIM_Linear;
					else if (FInterp == TEXT("constant")) FV.InterpMode = RCIM_Constant;
					else FV.InterpMode = RCIM_Cubic;
					Ch->GetData().AddKey(Frame, FV);
					Sequence->MarkPackageDirty();
					Session.Log(FString::Printf(TEXT("[OK] add(\"keyframe\", ch=%d, t=%.2f, v=%.4f, %s)"), ChannelIndex, Time, Value, *FInterp));
					return sol::make_object(Lua, true);
				}
				GlobalIdx += FCs.Num();

				// Bool channels
				TArrayView<FMovieSceneBoolChannel*> BCs = Proxy.GetChannels<FMovieSceneBoolChannel>();
				if (ChannelIndex >= GlobalIdx && ChannelIndex < GlobalIdx + BCs.Num())
				{
					FMovieSceneBoolChannel* Ch = BCs[ChannelIndex - GlobalIdx];
					if (!Ch) { Session.Log(TEXT("[FAIL] add(\"keyframe\") -> null bool channel")); return sol::lua_nil; }
					Ch->GetData().AddKey(Frame, BoolValue);
					Sequence->MarkPackageDirty();
					Session.Log(FString::Printf(TEXT("[OK] add(\"keyframe\", ch=%d, t=%.2f, bool=%s)"), ChannelIndex, Time, BoolValue ? TEXT("true") : TEXT("false")));
					return sol::make_object(Lua, true);
				}
				GlobalIdx += BCs.Num();

				// Integer channels
				TArrayView<FMovieSceneIntegerChannel*> ICs = Proxy.GetChannels<FMovieSceneIntegerChannel>();
				if (ChannelIndex >= GlobalIdx && ChannelIndex < GlobalIdx + ICs.Num())
				{
					FMovieSceneIntegerChannel* Ch = ICs[ChannelIndex - GlobalIdx];
					if (!Ch) { Session.Log(TEXT("[FAIL] add(\"keyframe\") -> null integer channel")); return sol::lua_nil; }
					Ch->GetData().AddKey(Frame, static_cast<int32>(Value));
					Sequence->MarkPackageDirty();
					Session.Log(FString::Printf(TEXT("[OK] add(\"keyframe\", ch=%d, t=%.2f, v=%d)"), ChannelIndex, Time, static_cast<int32>(Value)));
					return sol::make_object(Lua, true);
				}
				GlobalIdx += ICs.Num();

				// Byte channels
				TArrayView<FMovieSceneByteChannel*> YCs = Proxy.GetChannels<FMovieSceneByteChannel>();
				if (ChannelIndex >= GlobalIdx && ChannelIndex < GlobalIdx + YCs.Num())
				{
					FMovieSceneByteChannel* Ch = YCs[ChannelIndex - GlobalIdx];
					if (!Ch) { Session.Log(TEXT("[FAIL] add(\"keyframe\") -> null byte channel")); return sol::lua_nil; }
					Ch->GetData().AddKey(Frame, static_cast<uint8>(Value));
					Sequence->MarkPackageDirty();
					Session.Log(FString::Printf(TEXT("[OK] add(\"keyframe\", ch=%d, t=%.2f, v=%d)"), ChannelIndex, Time, static_cast<uint8>(Value)));
					return sol::make_object(Lua, true);
				}
				GlobalIdx += YCs.Num();

				// String channels
				TArrayView<FMovieSceneStringChannel*> SCs = Proxy.GetChannels<FMovieSceneStringChannel>();
				if (ChannelIndex >= GlobalIdx && ChannelIndex < GlobalIdx + SCs.Num())
				{
					FMovieSceneStringChannel* Ch = SCs[ChannelIndex - GlobalIdx];
					if (!Ch) { Session.Log(TEXT("[FAIL] add(\"keyframe\") -> null string channel")); return sol::lua_nil; }
					FString FVal = NeoLuaStr::ToFString(StringValue);
					Ch->GetData().AddKey(Frame, FVal);
					Sequence->MarkPackageDirty();
					Session.Log(FString::Printf(TEXT("[OK] add(\"keyframe\", ch=%d, t=%.2f, string=\"%s\")"), ChannelIndex, Time, *FVal));
					return sol::make_object(Lua, true);
				}
				GlobalIdx += SCs.Num();

				// Text channels
				TArrayView<FMovieSceneTextChannel*> TCs = Proxy.GetChannels<FMovieSceneTextChannel>();
				if (ChannelIndex >= GlobalIdx && ChannelIndex < GlobalIdx + TCs.Num())
				{
					FMovieSceneTextChannel* Ch = TCs[ChannelIndex - GlobalIdx];
					if (!Ch) { Session.Log(TEXT("[FAIL] add(\"keyframe\") -> null text channel")); return sol::lua_nil; }
					const std::string TextValue = P.get_or("text_value", StringValue);
					FText FVal = FText::FromString(NeoLuaStr::ToFString(TextValue));
					Ch->GetData().AddKey(Frame, FVal);
					Sequence->MarkPackageDirty();
					Session.Log(FString::Printf(TEXT("[OK] add(\"keyframe\", ch=%d, t=%.2f, text=\"%s\")"), ChannelIndex, Time, *FVal.ToString()));
					return sol::make_object(Lua, true);
				}
				GlobalIdx += TCs.Num();

				// ObjectPath channels
				TArrayView<FMovieSceneObjectPathChannel*> OPCs = Proxy.GetChannels<FMovieSceneObjectPathChannel>();
				if (ChannelIndex >= GlobalIdx && ChannelIndex < GlobalIdx + OPCs.Num())
				{
					FMovieSceneObjectPathChannel* Ch = OPCs[ChannelIndex - GlobalIdx];
					if (!Ch) { Session.Log(TEXT("[FAIL] add(\"keyframe\") -> null object path channel")); return sol::lua_nil; }
					FString FPath = NeoLuaStr::ToFString(ObjectPath);
					// Route through the shared resolver so Lua-short paths (matching open_asset's
					// acceptance surface) work here too; falls back to Asset Registry.
					UObject* Obj = NeoLuaAsset::ResolveWithRegistry(FPath);
					if (!Obj)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] add(\"keyframe\") -> object_path '%s' did not resolve. Use a /Game asset path or confirm it with open_asset() first."), *FPath));
						return sol::lua_nil;
					}
					FMovieSceneObjectPathChannelKeyValue KV(Obj);
					Ch->GetData().AddKey(Frame, KV);
					Sequence->MarkPackageDirty();
					Session.Log(FString::Printf(TEXT("[OK] add(\"keyframe\", ch=%d, t=%.2f, object=\"%s\")"), ChannelIndex, Time, *FPath));
					return sol::make_object(Lua, true);
				}
				GlobalIdx += OPCs.Num();

				// Event channels — bind a UFunction from a Blueprint and optionally set payload.
				// Accepts:
				//   event="/Game/BP.FunctionName"                                   (simple string form)
				// or
				//   event={function="/Game/BP.FnName", bound_object?="PinName",
				//          payload?={key=value, ...}}                               (full form)
				//
				// NOTE: we populate Ptrs.Function directly so playback fires at runtime. The editor-side
				// WeakEndpoint (K2 graph node) is NOT set — meaning the "edit endpoint" button in
				// Sequencer won't open a graph. A future BP compile may also regenerate Ptrs from
				// WeakEndpoint, which would clear our assignment. This is a Lua scripting limitation
				// consistent with the other non-editor-graph-aware bindings in this file.
				TArrayView<FMovieSceneEventChannel*> EVCs = Proxy.GetChannels<FMovieSceneEventChannel>();
				if (ChannelIndex >= GlobalIdx && ChannelIndex < GlobalIdx + EVCs.Num())
				{
					FMovieSceneEventChannel* Ch = EVCs[ChannelIndex - GlobalIdx];
					if (!Ch) { Session.Log(TEXT("[FAIL] add(\"keyframe\") -> null event channel")); return sol::lua_nil; }

					sol::object EventObj = P["event"];
					if (!EventObj.valid() || EventObj.get_type() == sol::type::lua_nil)
					{
						Session.Log(TEXT("[FAIL] add(\"keyframe\") -> event channel requires event=\"/Game/BP.FnName\" or event={function=..,bound_object?=..,payload?=..}"));
						return sol::lua_nil;
					}

					FString FunctionSpec;
					FString BoundPinName;
					sol::optional<sol::table> PayloadOpt;

					if (EventObj.is<std::string>())
					{
						FunctionSpec = NeoLuaStr::ToFString(EventObj.as<std::string>());
					}
					else if (EventObj.is<sol::table>())
					{
						sol::table ET = EventObj.as<sol::table>();
						FunctionSpec = NeoLuaStr::ToFStringOpt(ET.get<sol::optional<std::string>>("function"));
						BoundPinName = NeoLuaStr::ToFStringOpt(ET.get<sol::optional<std::string>>("bound_object"));
						PayloadOpt = ET.get<sol::optional<sol::table>>("payload");
					}
					else
					{
						Session.Log(TEXT("[FAIL] add(\"keyframe\") -> event must be a string path or a table"));
						return sol::lua_nil;
					}

					if (FunctionSpec.IsEmpty())
					{
						Session.Log(TEXT("[FAIL] add(\"keyframe\") -> event.function missing"));
						return sol::lua_nil;
					}

					// Split into blueprint path and function name.
					// Accepts "/Game/BP.FunctionName" — UE's asset path convention.
					FString BPPath, FnName;
					{
						const int32 DotIdx = FunctionSpec.Find(TEXT("."), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
						if (DotIdx == INDEX_NONE)
						{
							Session.Log(FString::Printf(TEXT("[FAIL] add(\"keyframe\") -> event.function '%s' must be \"/Game/BP.FnName\""), *FunctionSpec));
							return sol::lua_nil;
						}
						BPPath = FunctionSpec.Left(DotIdx);
						FnName = FunctionSpec.Mid(DotIdx + 1);
					}

					UBlueprint* BP = NeoLuaAsset::Resolve<UBlueprint>(BPPath);
					if (!BP || !BP->GeneratedClass)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] add(\"keyframe\") -> blueprint '%s' not found or not compiled"), *BPPath));
						return sol::lua_nil;
					}

					UFunction* EventFn = BP->GeneratedClass->FindFunctionByName(FName(*FnName));
					if (!EventFn)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] add(\"keyframe\") -> function '%s' not found on %s. Compile the blueprint and verify the function name via class_methods(\"%s\") before retrying."),
							*FnName, *BP->GetName(), *BP->GeneratedClass->GetPathName()));
						return sol::lua_nil;
					}

					FMovieSceneEvent Event;
					Event.Ptrs.Function = EventFn;

					// Bound-object pin: find the named FProperty on the UFunction (if provided).
					if (!BoundPinName.IsEmpty())
					{
						FProperty* BoundProp = nullptr;
						for (TFieldIterator<FProperty> It(EventFn); It; ++It)
						{
							if (It->GetName().Equals(BoundPinName, ESearchCase::IgnoreCase))
							{
								BoundProp = *It;
								break;
							}
						}
						if (!BoundProp)
						{
							Session.Log(FString::Printf(TEXT("[WARN] add(\"keyframe\") -> bound_object pin '%s' not found on %s"), *BoundPinName, *FnName));
						}
						else
						{
							Event.Ptrs.BoundObjectProperty = BoundProp;
#if WITH_EDITORONLY_DATA
							Event.BoundObjectPinName = FName(*BoundPinName);
#endif
						}
					}

#if WITH_EDITORONLY_DATA
					// Payload variables: each entry becomes an FMovieSceneEventPayloadVariable
					// whose Value field is the value exported as text (the same format BP graph
					// uses for default pin values).
					if (PayloadOpt.has_value())
					{
						sol::table PT = PayloadOpt.value();
						for (auto& Pair : PT)
						{
							if (!Pair.first.is<std::string>()) continue;
							const FString VarName = NeoLuaStr::ToFString(Pair.first.as<std::string>());
							FMovieSceneEventPayloadVariable Var;

							if (Pair.second.is<std::string>())
							{
								Var.Value = NeoLuaStr::ToFString(Pair.second.as<std::string>());
							}
							else if (Pair.second.is<bool>())
							{
								Var.Value = Pair.second.as<bool>() ? TEXT("true") : TEXT("false");
							}
							else if (Pair.second.is<double>())
							{
								Var.Value = FString::SanitizeFloat(Pair.second.as<double>());
							}
							else if (Pair.second.is<int>())
							{
								Var.Value = FString::FromInt(Pair.second.as<int>());
							}
							else
							{
								// Unsupported type — skip with a warn instead of failing the whole key.
								Session.Log(FString::Printf(TEXT("[WARN] add(\"keyframe\") -> payload var '%s' has unsupported type; skipped"), *VarName));
								continue;
							}
							Event.PayloadVariables.Add(FName(*VarName), Var);
						}
					}
#endif

					Ch->GetData().AddKey(Frame, Event);
					Sequence->MarkPackageDirty();
					Session.Log(FString::Printf(TEXT("[OK] add(\"keyframe\", ch=%d, t=%.2f, event=%s.%s)"),
						ChannelIndex, Time, *BP->GetName(), *FnName));
					return sol::make_object(Lua, true);
				}
				GlobalIdx += EVCs.Num();

				Session.Log(FString::Printf(TEXT("[FAIL] add(\"keyframe\") -> channel_index %d out of range (total %d). Call list(\"channels\", {binding?=.., track_type=..}) to pick a valid index."), ChannelIndex, GlobalIdx));
				return sol::lua_nil;
			}

			// ---- add("section", {binding?, track_type, track_index?, start, end, row_index?}) ----
			// Appends a new section to an EXISTING track. Omit binding for master tracks.
			// add("track") already creates a track-with-one-initial-section; this verb is for
			// appending additional sections (e.g. a second audio clip on one track, sliced anim
			// segments, multi-shot fades). Uses Track->CreateNewSection() + Track->AddSection().
			if (FType.Equals(TEXT("section"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"section\") -> {track_type, start, end} required")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string TrackTypeStr = P.get_or("track_type", std::string());
				if (TrackTypeStr.empty()) { Session.Log(TEXT("[FAIL] add(\"section\") -> track_type required")); return sol::lua_nil; }

				FString FTrackType = NeoLuaStr::ToFString(TrackTypeStr);
				UClass* TrackClass = ResolveTrackClass(FTrackType);
				if (!TrackClass)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"section\") -> unknown track type '%s'. Call list(\"track_types\") for valid names."), *FTrackType));
					return sol::lua_nil;
				}

				std::string BindStr = P.get_or("binding", std::string());
				int32 TrackIdx = P.get_or("track_index", 0);
				sol::optional<double> StartOpt = P.get<sol::optional<double>>("start");
				sol::optional<double> EndOpt = P.get<sol::optional<double>>("end");
				if (!StartOpt.has_value() || !EndOpt.has_value())
				{
					Session.Log(TEXT("[FAIL] add(\"section\") -> start and end (seconds) required"));
					return sol::lua_nil;
				}
				const double StartTime = StartOpt.value();
				const double EndTime = EndOpt.value();
				if (EndTime <= StartTime)
				{
					Session.Log(TEXT("[FAIL] add(\"section\") -> end must be > start"));
					return sol::lua_nil;
				}

				// Resolve track (same pattern as configure_section and F4 master-track paths above).
				UMovieSceneTrack* TargetTrack = nullptr;
				if (BindStr.empty())
				{
					TargetTrack = FindMasterTrackByClass(MovieScene, TrackClass, TrackIdx);
					if (!TargetTrack && TrackClass->IsChildOf(UMovieSceneCameraCutTrack::StaticClass()))
						TargetTrack = MovieScene->GetCameraCutTrack();
				}
				else
				{
					FGuid BindingGuid = FindBindingByName(MovieScene, NeoLuaStr::ToFString(BindStr));
					if (!BindingGuid.IsValid())
					{
						Session.Log(FString::Printf(TEXT("[FAIL] add(\"section\") -> binding '%s' not found"), UTF8_TO_TCHAR(BindStr.c_str())));
						return sol::lua_nil;
					}
					const FMovieSceneBinding* Binding = MovieScene->FindBinding(BindingGuid);
					if (Binding)
					{
						int32 MatchCount = 0;
						for (UMovieSceneTrack* Track : Binding->GetTracks())
						{
							if (Track && Track->IsA(TrackClass))
							{
								if (MatchCount == TrackIdx) { TargetTrack = Track; break; }
								MatchCount++;
							}
						}
					}
				}
				if (!TargetTrack)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"section\") -> no %s track (index %d) %s"),
						*FTrackType, TrackIdx, BindStr.empty() ? TEXT("(master)") : *FString::Printf(TEXT("on '%s'"), UTF8_TO_TCHAR(BindStr.c_str()))));
					return sol::lua_nil;
				}

				// CameraCut sections MUST be created via UMovieSceneCameraCutTrack::AddNewCameraCut
				// so they carry a valid camera binding. The generic CreateNewSection path would
				// produce a section with an empty FMovieSceneObjectBindingID — evaluation would
				// render black. Require a camera_binding param and route through the typed API.
				if (TrackClass->IsChildOf(UMovieSceneCameraCutTrack::StaticClass()))
				{
					std::string CamBindStr = P.get_or("camera_binding", std::string());
					if (CamBindStr.empty())
					{
						Session.Log(TEXT("[FAIL] add(\"section\") -> CameraCut sections require camera_binding"));
						return sol::lua_nil;
					}
					FString FCamBind = NeoLuaStr::ToFString(CamBindStr);
					FGuid CamGuid = FindBindingByName(MovieScene, FCamBind);
					if (!CamGuid.IsValid())
					{
						Session.Log(FString::Printf(TEXT("[FAIL] add(\"section\") -> camera_binding '%s' not found. Call list(\"bindings\") to confirm."), *FCamBind));
						return sol::lua_nil;
					}

					const FScopedTransaction Tx(NSLOCTEXT("AIK", "SeqAddSection", "Add Sequencer Section"));
					TargetTrack->Modify();

					UMovieSceneCameraCutTrack* CCTrack = Cast<UMovieSceneCameraCutTrack>(TargetTrack);
					const FFrameNumber StartFrame = SecondsToFrame(MovieScene, StartTime);
					const FFrameNumber EndFrame = SecondsToFrame(MovieScene, EndTime);
					UE::MovieScene::FRelativeObjectBindingID RelID(CamGuid);
					FMovieSceneObjectBindingID CamBindID(RelID);
					UMovieSceneCameraCutSection* CCS = CCTrack->AddNewCameraCut(CamBindID, StartFrame);
					if (!CCS)
					{
						Session.Log(TEXT("[FAIL] add(\"section\") -> AddNewCameraCut returned null"));
						return sol::lua_nil;
					}
					CCS->SetRange(TRange<FFrameNumber>(StartFrame, EndFrame));
					sol::optional<int> RowOpt = P.get<sol::optional<int>>("row_index");
					if (RowOpt.has_value()) CCS->SetRowIndex(RowOpt.value());
					CCTrack->RearrangeAllSections();
					Sequence->MarkPackageDirty();

					const int32 NewIdx = CCTrack->GetAllSections().IndexOfByKey(CCS);
					Session.Log(FString::Printf(TEXT("[OK] add(\"section\", type=\"CameraCut\", start=%.2f, end=%.2f) -> index %d"),
						StartTime, EndTime, NewIdx));
					sol::table R = Lua.create_table();
					R["section_index"] = NewIdx;
					R["start"] = StartTime;
					R["end"] = EndTime;
					return R;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "SeqAddSection", "Add Sequencer Section"));
				TargetTrack->Modify();

				UMovieSceneSection* NewSection = TargetTrack->CreateNewSection();
				if (!NewSection)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"section\") -> CreateNewSection returned null for %s"), *FTrackType));
					return sol::lua_nil;
				}

				const FFrameNumber StartFrame = SecondsToFrame(MovieScene, StartTime);
				const FFrameNumber EndFrame = SecondsToFrame(MovieScene, EndTime);
				NewSection->SetRange(TRange<FFrameNumber>(StartFrame, EndFrame));

				sol::optional<int> RowOpt = P.get<sol::optional<int>>("row_index");
				if (RowOpt.has_value())
					NewSection->SetRowIndex(RowOpt.value());

				TargetTrack->AddSection(*NewSection);
				Sequence->MarkPackageDirty();

				const int32 NewIdx = TargetTrack->GetAllSections().IndexOfByKey(NewSection);
				Session.Log(FString::Printf(TEXT("[OK] add(\"section\", type=\"%s\", start=%.2f, end=%.2f) -> index %d"),
					*FTrackType, StartTime, EndTime, NewIdx));

				sol::table R = Lua.create_table();
				R["section_index"] = NewIdx;
				R["start"] = StartTime;
				R["end"] = EndTime;
				return R;
			}

			// ---- add("marked_frame", {frame=120, label?, comment?, color?, is_determinism_fence?, is_inclusive_time?}) ----
			if (FType.Equals(TEXT("marked_frame"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"marked_frame\") -> {frame=N} required")); return sol::lua_nil; }
				sol::table P = Params.value();

				sol::optional<int> FrameOpt = P["frame"];
				if (!FrameOpt.has_value())
				{
					Session.Log(TEXT("[FAIL] add(\"marked_frame\") -> frame (frame number) required"));
					return sol::lua_nil;
				}

				FMovieSceneMarkedFrame MF;
				MF.FrameNumber = FFrameNumber(FrameOpt.value());
				std::string LabelStr = P.get_or<std::string>("label", "");
				if (!LabelStr.empty()) MF.Label = NeoLuaStr::ToFString(LabelStr);
#if WITH_EDITORONLY_DATA
				std::string CommentStr = P.get_or<std::string>("comment", "");
				if (!CommentStr.empty()) MF.Comment = NeoLuaStr::ToFString(CommentStr);

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
				sol::optional<sol::table> ColorOpt = P["color"];
				if (ColorOpt.has_value())
				{
					// CustomColor is FLinearColor — ParseLinearColorFromLua accepts {hex="#…"},
					// 0-1 floats, or 0-255 ints (auto-detected by magnitude).
					MF.CustomColor = ParseLinearColorFromLua(ColorOpt.value(), MF.CustomColor);
					MF.bUseCustomColor = true;
				}
#endif
#endif
				MF.bIsDeterminismFence = P.get_or("is_determinism_fence", false);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
				MF.bIsInclusiveTime = P.get_or("is_inclusive_time", false);
#endif

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "SeqAddMarker", "Add Sequencer Marked Frame"));
				MovieScene->Modify();
				int32 Idx = MovieScene->AddMarkedFrame(MF);
				MovieScene->SortMarkedFrames();
				Sequence->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] add(\"marked_frame\", frame=%d, label=\"%s\") -> index %d"),
					MF.FrameNumber.Value, *MF.Label, Idx));
				sol::table Result = Lua.create_table();
				Result["index"] = Idx;
				return Result;
			}

			// ---- add("folder", {name, parent?, color?}) ----
			if (FType.Equals(TEXT("folder"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"folder\") -> {name=..} required")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string NameStr = P.get_or("name", std::string());
				if (NameStr.empty()) { Session.Log(TEXT("[FAIL] add(\"folder\") -> name required")); return sol::lua_nil; }

				FString FolderName = NeoLuaStr::ToFString(NameStr);

				// Check for duplicate name
				TArrayView<UMovieSceneFolder* const> RootFolders = MovieScene->GetRootFolders();
				if (FindFolderByName(RootFolders, FolderName))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"folder\") -> folder '%s' already exists"), *FolderName));
					return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "SeqAddFolder", "Add Sequencer Folder"));

				UMovieSceneFolder* NewFolder = NewObject<UMovieSceneFolder>(MovieScene, NAME_None, RF_Transactional);
				NewFolder->SetFolderName(FName(*FolderName));

#if WITH_EDITORONLY_DATA
				sol::optional<sol::table> ColorOpt = P["color"];
				if (ColorOpt.has_value())
				{
					NewFolder->SetFolderColor(ParseColorFromLua(ColorOpt.value()));
				}
#endif

				std::string ParentStr = P.get_or("parent", std::string());
				if (!ParentStr.empty())
				{
					FString ParentName = NeoLuaStr::ToFString(ParentStr);
					UMovieSceneFolder* ParentFolder = FindFolderByName(RootFolders, ParentName);
					if (!ParentFolder)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] add(\"folder\") -> parent folder '%s' not found"), *ParentName));
						return sol::lua_nil;
					}
					ParentFolder->AddChildFolder(NewFolder);
				}
				else
				{
					MovieScene->AddRootFolder(NewFolder);
				}

				Sequence->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"folder\", name=\"%s\", parent=\"%s\")"),
					*FolderName, ParentStr.empty() ? TEXT("(root)") : UTF8_TO_TCHAR(ParentStr.c_str())));

				sol::table R = Lua.create_table();
				R["name"] = NameStr;
				return R;
			}

			// ---- add("binding_tag", {tag, binding?}) ----
			if (FType.Equals(TEXT("binding_tag"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"binding_tag\") -> {tag=..} required")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string TagStr = P.get_or("tag", std::string());
				if (TagStr.empty()) { Session.Log(TEXT("[FAIL] add(\"binding_tag\") -> tag required")); return sol::lua_nil; }

				FName Tag = FName(NeoLuaStr::ToFString(TagStr));
				const FScopedTransaction Tx(NSLOCTEXT("AIK", "SeqAddBindingTag", "Add Binding Tag"));
				MovieScene->Modify();

				std::string BindingStr = P.get_or("binding", std::string());
				if (BindingStr.empty())
				{
					// Just create the tag (no binding assignment)
					MovieScene->AddNewBindingTag(Tag);
					Sequence->MarkPackageDirty();
					Session.Log(FString::Printf(TEXT("[OK] add(\"binding_tag\", tag=\"%s\") — tag created"), *Tag.ToString()));

					sol::table R = Lua.create_table();
					R["tag"] = TagStr;
					return R;
				}

				// Assign tag to a specific binding
				FString FBinding = NeoLuaStr::ToFString(BindingStr);
				FGuid BindingGuid = FindBindingByName(MovieScene, FBinding);
				if (!BindingGuid.IsValid())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"binding_tag\") -> binding '%s' not found"), *FBinding));
					return sol::lua_nil;
				}

				MovieScene->AddNewBindingTag(Tag);
				UE::MovieScene::FFixedObjectBindingID FixedID(BindingGuid, MovieSceneSequenceID::Root);
				MovieScene->TagBinding(Tag, FixedID);
				Sequence->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] add(\"binding_tag\", tag=\"%s\", binding=\"%s\")"), *Tag.ToString(), *FBinding));
				sol::table R = Lua.create_table();
				R["tag"] = TagStr;
				R["binding"] = BindingStr;
				return R;
			}

#if WITH_EDITORONLY_DATA
			// ---- add("section_group", {sections={...}}) ----
			if (FType.Equals(TEXT("section_group"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"section_group\") -> {sections={...}} required")); return sol::lua_nil; }
				sol::table P = Params.value();
				sol::optional<sol::table> SectionsOpt = P.get<sol::optional<sol::table>>("sections");
				if (!SectionsOpt.has_value()) { Session.Log(TEXT("[FAIL] add(\"section_group\") -> sections array required")); return sol::lua_nil; }
				sol::table SectionsArr = SectionsOpt.value();

				TArray<UMovieSceneSection*> SectionsToGroup;
				int32 Count = static_cast<int32>(SectionsArr.size());
				if (Count < 2)
				{
					Session.Log(TEXT("[FAIL] add(\"section_group\") -> at least 2 sections required"));
					return sol::lua_nil;
				}

				for (int32 i = 1; i <= Count; ++i)
				{
					sol::optional<sol::table> SecOpt = SectionsArr.get<sol::optional<sol::table>>(i);
					if (!SecOpt.has_value()) continue;
					sol::table SecT = SecOpt.value();

					std::string BindingStr = SecT.get_or("binding", std::string());
					std::string TrackTypeStr = SecT.get_or("track_type", std::string());
					if (TrackTypeStr.empty()) { Session.Log(TEXT("[FAIL] add(\"section_group\") -> each section needs track_type")); return sol::lua_nil; }

					FString FTrackType = NeoLuaStr::ToFString(TrackTypeStr);
					UClass* TrackClass = ResolveTrackClass(FTrackType);
					if (!TrackClass) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"section_group\") -> unknown track type '%s'. Call list(\"track_types\") for valid names."), *FTrackType)); return sol::lua_nil; }

					int32 TrackIdx = SecT.get_or("track_index", 0);
					int32 SectionIdx = SecT.get_or("section_index", 0);

					UMovieSceneTrack* Track = nullptr;
					if (BindingStr.empty())
					{
						Track = FindMasterTrackByClass(MovieScene, TrackClass, TrackIdx);
					}
					else
					{
						FGuid Guid = FindBindingByName(MovieScene, NeoLuaStr::ToFString(BindingStr));
						if (!Guid.IsValid()) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"section_group\") -> binding '%s' not found"), UTF8_TO_TCHAR(BindingStr.c_str()))); return sol::lua_nil; }
						const FMovieSceneBinding* Binding = MovieScene->FindBinding(Guid);
						if (!Binding) { Session.Log(TEXT("[FAIL] add(\"section_group\") -> binding not found")); return sol::lua_nil; }
						int32 Found = 0;
						for (UMovieSceneTrack* T : Binding->GetTracks())
						{
							if (T && T->GetClass()->IsChildOf(TrackClass))
							{
								if (Found == TrackIdx) { Track = T; break; }
								++Found;
							}
						}
					}

					if (!Track)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] add(\"section_group\") -> track '%s' index %d not found"), *FTrackType, TrackIdx));
						return sol::lua_nil;
					}

					const TArray<UMovieSceneSection*>& AllSections = Track->GetAllSections();
					if (!AllSections.IsValidIndex(SectionIdx))
					{
						Session.Log(FString::Printf(TEXT("[FAIL] add(\"section_group\") -> section index %d out of range (track has %d)"), SectionIdx, AllSections.Num()));
						return sol::lua_nil;
					}
					SectionsToGroup.Add(AllSections[SectionIdx]);
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "SeqGroupSections", "Group Sections"));
				MovieScene->Modify();
				MovieScene->GroupSections(SectionsToGroup);
				Sequence->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] add(\"section_group\") — grouped %d sections"), SectionsToGroup.Num()));
				sol::table R = Lua.create_table();
				R["section_count"] = SectionsToGroup.Num();
				return R;
			}

			// ---- add("node_group", {name, nodes?, enable_filter?}) ----
			if (FType.Equals(TEXT("node_group"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"node_group\") -> {name=..} required")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string NameStr = P.get_or("name", std::string());
				if (NameStr.empty()) { Session.Log(TEXT("[FAIL] add(\"node_group\") -> name required")); return sol::lua_nil; }

				FName GroupName = FName(NeoLuaStr::ToFString(NameStr));

				// Check for duplicate
				UMovieSceneNodeGroupCollection& NGC = MovieScene->GetNodeGroups();
				for (UMovieSceneNodeGroup* Existing : NGC)
				{
					if (Existing && Existing->GetName() == GroupName)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] add(\"node_group\") -> '%s' already exists"), *GroupName.ToString()));
						return sol::lua_nil;
					}
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "SeqAddNodeGroup", "Add Node Group"));
				MovieScene->Modify();

				UMovieSceneNodeGroup* NewGroup = NewObject<UMovieSceneNodeGroup>(MovieScene);
				NewGroup->SetName(GroupName);

				bool bEnableFilter = P.get_or("enable_filter", false);
				NewGroup->SetEnableFilter(bEnableFilter);

				// Add nodes if provided
				sol::optional<sol::table> NodesOpt = P.get<sol::optional<sol::table>>("nodes");
				if (NodesOpt.has_value())
				{
					sol::table NodesArr = NodesOpt.value();
					for (auto& Pair : NodesArr)
					{
						if (Pair.second.is<std::string>())
						{
							NewGroup->AddNode(NeoLuaStr::ToFString(Pair.second.as<std::string>()));
						}
					}
				}

				NGC.AddNodeGroup(NewGroup);
				Sequence->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] add(\"node_group\", name=\"%s\")"), *GroupName.ToString()));
				sol::table R = Lua.create_table();
				R["name"] = NameStr;
				R["enable_filter"] = bEnableFilter;
				return R;
			}
#endif

			Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: binding, track, section, keyframe, marked_frame, folder, binding_tag, section_group, node_group"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// remove(type, id)
		// ================================================================
		AssetObj.set_function("remove", [Sequence, MovieScene, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = NeoLuaStr::ToFString(Type);

			// ---- remove("binding", name) ----
			if (FType.Equals(TEXT("binding"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<std::string>()) { Session.Log(TEXT("[FAIL] remove(\"binding\") -> name required")); return sol::lua_nil; }
				FString FName = NeoLuaStr::ToFString(Id.as<std::string>());
				FGuid Guid = FindBindingByName(MovieScene, FName);
				if (!Guid.IsValid())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"binding\") -> '%s' not found"), *FName));
					return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "SeqRemBinding", "Remove Sequencer Binding"));

				FMovieScenePossessable* Poss = MovieScene->FindPossessable(Guid);
				if (Poss && MovieScene->RemovePossessable(Guid))
				{
					Sequence->MarkPackageDirty();
					Session.Log(FString::Printf(TEXT("[OK] remove(\"binding\", \"%s\") possessable"), *FName));
					return sol::make_object(Lua, true);
				}

				FMovieSceneSpawnable* Spawn = MovieScene->FindSpawnable(Guid);
				if (Spawn && MovieScene->RemoveSpawnable(Guid))
				{
					Sequence->MarkPackageDirty();
					Session.Log(FString::Printf(TEXT("[OK] remove(\"binding\", \"%s\") spawnable"), *FName));
					return sol::make_object(Lua, true);
				}

				Session.Log(FString::Printf(TEXT("[FAIL] remove(\"binding\") -> failed to remove '%s'"), *FName));
				return sol::lua_nil;
			}

			// ---- remove("track", {binding?, track_type, track_index?}) ----
			if (FType.Equals(TEXT("track"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<sol::table>()) { Session.Log(TEXT("[FAIL] remove(\"track\") -> {track_type=..} required")); return sol::lua_nil; }
				sol::table P = Id.as<sol::table>();
				std::string TrackTypeStr = P.get_or("track_type", std::string());
				if (TrackTypeStr.empty()) { Session.Log(TEXT("[FAIL] remove(\"track\") -> track_type required")); return sol::lua_nil; }

				FString FTrackType = NeoLuaStr::ToFString(TrackTypeStr);
				UClass* TrackClass = ResolveTrackClass(FTrackType);
				if (!TrackClass)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"track\") -> unknown track type '%s'. Call list(\"track_types\") for valid names."), *FTrackType));
					return sol::lua_nil;
				}

				std::string BindStr = P.get_or("binding", std::string());
				int32 TrackIdx = P.get_or("track_index", 0);
				const bool bIsCameraCut = TrackClass->IsChildOf(UMovieSceneCameraCutTrack::StaticClass());

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "SeqRemTrack", "Remove Sequencer Track"));

				if (BindStr.empty())
				{
					// Master track removal
					if (bIsCameraCut)
					{
						UMovieSceneTrack* CCT = MovieScene->GetCameraCutTrack();
						if (CCT)
						{
							MovieScene->RemoveCameraCutTrack();
							Sequence->MarkPackageDirty();
							Session.Log(FString::Printf(TEXT("[OK] remove(\"track\", type=\"%s\") master"), *FTrackType));
							return sol::make_object(Lua, true);
						}
						Session.Log(FString::Printf(TEXT("[FAIL] remove(\"track\") -> no %s master track"), *FTrackType));
						return sol::lua_nil;
					}

					int32 MatchCount = 0;
					for (UMovieSceneTrack* Track : MovieScene->GetTracks())
					{
						if (Track && Track->IsA(TrackClass))
						{
							if (MatchCount == TrackIdx)
							{
								if (MovieScene->RemoveTrack(*Track))
								{
									Sequence->MarkPackageDirty();
									Session.Log(FString::Printf(TEXT("[OK] remove(\"track\", type=\"%s\") master"), *FTrackType));
									return sol::make_object(Lua, true);
								}
								Session.Log(FString::Printf(TEXT("[FAIL] remove(\"track\") -> failed to remove %s master track"), *FTrackType));
								return sol::lua_nil;
							}
							MatchCount++;
						}
					}
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"track\") -> no %s master track"), *FTrackType));
					return sol::lua_nil;
				}

				// Binding track removal
				if (bIsCameraCut)
				{
					Session.Log(TEXT("[FAIL] remove(\"track\") -> CameraCut is master; omit binding"));
					return sol::lua_nil;
				}

				FString FBinding = NeoLuaStr::ToFString(BindStr);
				FGuid Guid = FindBindingByName(MovieScene, FBinding);
				if (!Guid.IsValid())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"track\") -> binding '%s' not found"), *FBinding));
					return sol::lua_nil;
				}

				const FMovieSceneBinding* Binding = MovieScene->FindBinding(Guid);
				if (!Binding)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"track\") -> no tracks for '%s'"), *FBinding));
					return sol::lua_nil;
				}

				int32 MatchCount = 0;
				for (UMovieSceneTrack* Track : Binding->GetTracks())
				{
					if (Track && Track->IsA(TrackClass))
					{
						if (MatchCount == TrackIdx)
						{
							if (MovieScene->RemoveTrack(*Track))
							{
								Sequence->MarkPackageDirty();
								Session.Log(FString::Printf(TEXT("[OK] remove(\"track\", type=\"%s\" from \"%s\")"), *FTrackType, *FBinding));
								return sol::make_object(Lua, true);
							}
							Session.Log(FString::Printf(TEXT("[FAIL] remove(\"track\") -> failed to remove %s from '%s'"), *FTrackType, *FBinding));
							return sol::lua_nil;
						}
						MatchCount++;
					}
				}

				Session.Log(FString::Printf(TEXT("[FAIL] remove(\"track\") -> no %s track (index %d) on '%s'"), *FTrackType, TrackIdx, *FBinding));
				return sol::lua_nil;
			}

			// ---- remove("keyframe", {binding?, track_type, channel_index, time, track_index?}) ----
			// Omit binding for master tracks (mirrors add("keyframe") semantics).
			if (FType.Equals(TEXT("keyframe"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<sol::table>()) { Session.Log(TEXT("[FAIL] remove(\"keyframe\") -> {track_type, channel_index, time} required (binding optional for master tracks)")); return sol::lua_nil; }
				sol::table P = Id.as<sol::table>();
				std::string BindStr = P.get_or("binding", std::string());
				std::string TrackTypeStr = P.get_or("track_type", std::string());
				if (TrackTypeStr.empty())
				{
					Session.Log(TEXT("[FAIL] remove(\"keyframe\") -> track_type required"));
					return sol::lua_nil;
				}

				int32 ChannelIndex = P.get_or("channel_index", 0);
				double Time = P.get_or("time", 0.0);
				int32 TrackIdx = P.get_or("track_index", -1);

				FString FBinding = NeoLuaStr::ToFString(BindStr);
				FString FTrackType = NeoLuaStr::ToFString(TrackTypeStr);

				UClass* TrackClass = ResolveTrackClass(FTrackType);
				if (!TrackClass)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"keyframe\") -> unknown track type '%s'. Call list(\"track_types\") for valid names."), *FTrackType));
					return sol::lua_nil;
				}

				UMovieSceneTrack* TargetTrack = nullptr;
				if (BindStr.empty())
				{
					TargetTrack = FindMasterTrackByClass(MovieScene, TrackClass, TrackIdx < 0 ? 0 : TrackIdx);
					if (!TargetTrack && TrackClass->IsChildOf(UMovieSceneCameraCutTrack::StaticClass()))
						TargetTrack = MovieScene->GetCameraCutTrack();
					if (!TargetTrack)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] remove(\"keyframe\") -> no master %s track"), *FTrackType));
						return sol::lua_nil;
					}
				}
				else
				{
					FGuid BindingGuid = FindBindingByName(MovieScene, FBinding);
					if (!BindingGuid.IsValid())
					{
						Session.Log(FString::Printf(TEXT("[FAIL] remove(\"keyframe\") -> binding '%s' not found"), *FBinding));
						return sol::lua_nil;
					}

					const FMovieSceneBinding* Binding = MovieScene->FindBinding(BindingGuid);
					if (!Binding)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] remove(\"keyframe\") -> no tracks for '%s'"), *FBinding));
						return sol::lua_nil;
					}

					int32 MatchCount = 0;
					for (UMovieSceneTrack* Track : Binding->GetTracks())
					{
						if (Track && Track->IsA(TrackClass))
						{
							if (TrackIdx < 0 || MatchCount == TrackIdx) { TargetTrack = Track; break; }
							MatchCount++;
						}
					}
					if (!TargetTrack)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] remove(\"keyframe\") -> no %s track on '%s'"), *FTrackType, *FBinding));
						return sol::lua_nil;
					}
				}

				int32 SectionIdx = P.get_or("section_index", 0);
				const TArray<UMovieSceneSection*>& Sections = TargetTrack->GetAllSections();
				if (SectionIdx < 0 || SectionIdx >= Sections.Num() || !Sections[SectionIdx])
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"keyframe\") -> section %d out of range (total %d)"), SectionIdx, Sections.Num()));
					return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "SeqRemKey", "Remove Sequencer Keyframe"));
				UMovieSceneSection* Section = Sections[SectionIdx];
				Section->Modify();
				FFrameNumber Frame = SecondsToFrame(MovieScene, Time);
				FMovieSceneChannelProxy& Proxy = Section->GetChannelProxy();
				int32 GlobalIdx = 0;

				// Helper: find and remove key at or near Frame
				auto RemoveKeyAtTime = [&](auto* Channel) -> bool
				{
					if (!Channel) return false;
					auto Data = Channel->GetData();
					const int32 NumKeys = Data.GetTimes().Num();
					for (int32 k = 0; k < NumKeys; ++k)
					{
						if (Data.GetTimes()[k] == Frame)
						{
							Channel->GetData().RemoveKey(k);
							return true;
						}
					}
					// Nearest within 1 tick
					for (int32 k = 0; k < NumKeys; ++k)
					{
						if (FMath::Abs(Data.GetTimes()[k].Value - Frame.Value) <= 1)
						{
							Channel->GetData().RemoveKey(k);
							return true;
						}
					}
					return false;
				};

				bool bRemoved = false;

				// Double channels
				TArrayView<FMovieSceneDoubleChannel*> DCs = Proxy.GetChannels<FMovieSceneDoubleChannel>();
				if (ChannelIndex >= GlobalIdx && ChannelIndex < GlobalIdx + DCs.Num())
				{
					bRemoved = RemoveKeyAtTime(DCs[ChannelIndex - GlobalIdx]);
				}
				GlobalIdx += DCs.Num();

				if (!bRemoved)
				{
					// Float channels
					TArrayView<FMovieSceneFloatChannel*> FCs = Proxy.GetChannels<FMovieSceneFloatChannel>();
					if (ChannelIndex >= GlobalIdx && ChannelIndex < GlobalIdx + FCs.Num())
					{
						bRemoved = RemoveKeyAtTime(FCs[ChannelIndex - GlobalIdx]);
					}
					GlobalIdx += FCs.Num();
				}

				if (!bRemoved)
				{
					// Bool channels
					TArrayView<FMovieSceneBoolChannel*> BCs = Proxy.GetChannels<FMovieSceneBoolChannel>();
					if (ChannelIndex >= GlobalIdx && ChannelIndex < GlobalIdx + BCs.Num())
					{
						bRemoved = RemoveKeyAtTime(BCs[ChannelIndex - GlobalIdx]);
					}
					GlobalIdx += BCs.Num();
				}

				if (!bRemoved)
				{
					// Integer channels
					TArrayView<FMovieSceneIntegerChannel*> ICs = Proxy.GetChannels<FMovieSceneIntegerChannel>();
					if (ChannelIndex >= GlobalIdx && ChannelIndex < GlobalIdx + ICs.Num())
					{
						bRemoved = RemoveKeyAtTime(ICs[ChannelIndex - GlobalIdx]);
					}
					GlobalIdx += ICs.Num();
				}

				if (!bRemoved)
				{
					// Byte channels
					TArrayView<FMovieSceneByteChannel*> YCs = Proxy.GetChannels<FMovieSceneByteChannel>();
					if (ChannelIndex >= GlobalIdx && ChannelIndex < GlobalIdx + YCs.Num())
					{
						bRemoved = RemoveKeyAtTime(YCs[ChannelIndex - GlobalIdx]);
					}
					GlobalIdx += YCs.Num();
				}

				if (!bRemoved)
				{
					// String channels
					TArrayView<FMovieSceneStringChannel*> SCs = Proxy.GetChannels<FMovieSceneStringChannel>();
					if (ChannelIndex >= GlobalIdx && ChannelIndex < GlobalIdx + SCs.Num())
					{
						bRemoved = RemoveKeyAtTime(SCs[ChannelIndex - GlobalIdx]);
					}
					GlobalIdx += SCs.Num();
				}

				if (!bRemoved)
				{
					// Text channels
					TArrayView<FMovieSceneTextChannel*> TCs = Proxy.GetChannels<FMovieSceneTextChannel>();
					if (ChannelIndex >= GlobalIdx && ChannelIndex < GlobalIdx + TCs.Num())
					{
						bRemoved = RemoveKeyAtTime(TCs[ChannelIndex - GlobalIdx]);
					}
					GlobalIdx += TCs.Num();
				}

				if (!bRemoved)
				{
					// ObjectPath channels
					TArrayView<FMovieSceneObjectPathChannel*> OPCs = Proxy.GetChannels<FMovieSceneObjectPathChannel>();
					if (ChannelIndex >= GlobalIdx && ChannelIndex < GlobalIdx + OPCs.Num())
					{
						bRemoved = RemoveKeyAtTime(OPCs[ChannelIndex - GlobalIdx]);
					}
					GlobalIdx += OPCs.Num();
				}

				if (!bRemoved)
				{
					// Event channels
					TArrayView<FMovieSceneEventChannel*> EVCs = Proxy.GetChannels<FMovieSceneEventChannel>();
					if (ChannelIndex >= GlobalIdx && ChannelIndex < GlobalIdx + EVCs.Num())
					{
						bRemoved = RemoveKeyAtTime(EVCs[ChannelIndex - GlobalIdx]);
					}
					GlobalIdx += EVCs.Num();
				}

				if (bRemoved)
				{
					Sequence->MarkPackageDirty();
					Session.Log(FString::Printf(TEXT("[OK] remove(\"keyframe\", ch=%d, t=%.2f)"), ChannelIndex, Time));
					return sol::make_object(Lua, true);
				}

				Session.Log(FString::Printf(TEXT("[FAIL] remove(\"keyframe\") -> no key at t=%.2f on channel %d. Call list(\"keyframes\", {...}) to inspect the exact keyed frame times."), Time, ChannelIndex));
				return sol::lua_nil;
			}

			// ---- remove("section", {binding?, track_type, track_index?, section_index}) ----
			// Removes a specific section from an existing track. Uses UMovieSceneTrack::RemoveSectionAt.
			if (FType.Equals(TEXT("section"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<sol::table>()) { Session.Log(TEXT("[FAIL] remove(\"section\") -> {track_type, section_index} required")); return sol::lua_nil; }
				sol::table P = Id.as<sol::table>();
				std::string TrackTypeStr = P.get_or("track_type", std::string());
				if (TrackTypeStr.empty()) { Session.Log(TEXT("[FAIL] remove(\"section\") -> track_type required")); return sol::lua_nil; }

				FString FTrackType = NeoLuaStr::ToFString(TrackTypeStr);
				UClass* TrackClass = ResolveTrackClass(FTrackType);
				if (!TrackClass)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"section\") -> unknown track type '%s'. Call list(\"track_types\") for valid names."), *FTrackType));
					return sol::lua_nil;
				}

				std::string BindStr = P.get_or("binding", std::string());
				int32 TrackIdx = P.get_or("track_index", 0);
				int32 SectionIdx = P.get_or("section_index", -1);
				if (SectionIdx < 0)
				{
					Session.Log(TEXT("[FAIL] remove(\"section\") -> section_index required"));
					return sol::lua_nil;
				}

				UMovieSceneTrack* TargetTrack = nullptr;
				if (BindStr.empty())
				{
					TargetTrack = FindMasterTrackByClass(MovieScene, TrackClass, TrackIdx);
					if (!TargetTrack && TrackClass->IsChildOf(UMovieSceneCameraCutTrack::StaticClass()))
						TargetTrack = MovieScene->GetCameraCutTrack();
				}
				else
				{
					FGuid BindingGuid = FindBindingByName(MovieScene, NeoLuaStr::ToFString(BindStr));
					if (!BindingGuid.IsValid())
					{
						Session.Log(FString::Printf(TEXT("[FAIL] remove(\"section\") -> binding '%s' not found"), UTF8_TO_TCHAR(BindStr.c_str())));
						return sol::lua_nil;
					}
					const FMovieSceneBinding* Binding = MovieScene->FindBinding(BindingGuid);
					if (Binding)
					{
						int32 MatchCount = 0;
						for (UMovieSceneTrack* Track : Binding->GetTracks())
						{
							if (Track && Track->IsA(TrackClass))
							{
								if (MatchCount == TrackIdx) { TargetTrack = Track; break; }
								MatchCount++;
							}
						}
					}
				}
				if (!TargetTrack)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"section\") -> no %s track (index %d)"), *FTrackType, TrackIdx));
					return sol::lua_nil;
				}

				const TArray<UMovieSceneSection*>& Sections = TargetTrack->GetAllSections();
				if (SectionIdx >= Sections.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"section\") -> section_index %d out of range (track has %d)"), SectionIdx, Sections.Num()));
					return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "SeqRemSection", "Remove Sequencer Section"));
				TargetTrack->Modify();
				TargetTrack->RemoveSectionAt(SectionIdx);
				Sequence->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] remove(\"section\", type=\"%s\", section_index=%d)"), *FTrackType, SectionIdx));
				return sol::make_object(Lua, true);
			}

			// ---- remove("marked_frame", index_or_label) ----
			if (FType.Equals(TEXT("marked_frame"), ESearchCase::IgnoreCase))
			{
				if (MovieScene->AreMarkedFramesLocked())
				{
					Session.Log(TEXT("[FAIL] remove(\"marked_frame\") -> marked frames are locked. Call configure(\"marked_frames\", {locked=false}) before removing markers."));
					return sol::lua_nil;
				}

				int32 DeleteIdx = -1;
				if (Id.is<std::string>())
				{
					FString Label = NeoLuaStr::ToFString(Id.as<std::string>());
					DeleteIdx = MovieScene->FindMarkedFrameByLabel(Label);
					if (DeleteIdx < 0)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] remove(\"marked_frame\") -> label '%s' not found"), *Label));
						return sol::lua_nil;
					}
				}
				else if (Id.is<int>() || Id.is<double>())
				{
					DeleteIdx = Id.is<int>() ? Id.as<int>() : static_cast<int32>(Id.as<double>());
				}
				else
				{
					Session.Log(TEXT("[FAIL] remove(\"marked_frame\") -> index (number) or label (string) required"));
					return sol::lua_nil;
				}

				const TArray<FMovieSceneMarkedFrame>& Markers = MovieScene->GetMarkedFrames();
				if (DeleteIdx < 0 || DeleteIdx >= Markers.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"marked_frame\") -> index %d out of range (total %d)"), DeleteIdx, Markers.Num()));
					return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "SeqRemMarker", "Remove Sequencer Marked Frame"));
				MovieScene->Modify();
				MovieScene->DeleteMarkedFrame(DeleteIdx);
				Sequence->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] remove(\"marked_frame\", %d)"), DeleteIdx));
				return sol::make_object(Lua, true);
			}

			// ---- remove("marked_frames") — delete all ----
			if (FType.Equals(TEXT("marked_frames"), ESearchCase::IgnoreCase))
			{
				if (MovieScene->AreMarkedFramesLocked())
				{
					Session.Log(TEXT("[FAIL] remove(\"marked_frames\") -> marked frames are locked. Call configure(\"marked_frames\", {locked=false}) before removing markers."));
					return sol::lua_nil;
				}

				const int32 Count = MovieScene->GetMarkedFrames().Num();
				if (Count == 0)
				{
					Session.Log(TEXT("[OK] remove(\"marked_frames\") -> already empty"));
					return sol::make_object(Lua, true);
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "SeqRemAllMarkers", "Remove All Sequencer Marked Frames"));
				MovieScene->Modify();
				MovieScene->DeleteMarkedFrames();
				Sequence->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] remove(\"marked_frames\") -> removed %d"), Count));
				return sol::make_object(Lua, true);
			}

			// ---- remove("folder", name) ----
			if (FType.Equals(TEXT("folder"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<std::string>()) { Session.Log(TEXT("[FAIL] remove(\"folder\") -> folder name (string) required")); return sol::lua_nil; }
				FString FolderName = NeoLuaStr::ToFString(Id.as<std::string>());

				TArrayView<UMovieSceneFolder* const> RootFolders = MovieScene->GetRootFolders();
				UMovieSceneFolder* ParentFolder = nullptr;
				UMovieSceneFolder* Target = FindFolderByName(RootFolders, FolderName, &ParentFolder);
				if (!Target)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"folder\") -> folder '%s' not found"), *FolderName));
					return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "SeqRemFolder", "Remove Sequencer Folder"));
				MovieScene->Modify();

				if (ParentFolder)
				{
					ParentFolder->RemoveChildFolder(Target);
				}
				else
				{
					MovieScene->RemoveRootFolder(Target);
				}

				Sequence->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove(\"folder\", \"%s\")"), *FolderName));
				return sol::make_object(Lua, true);
			}

			// ---- remove("binding_tag", {tag, binding?}) or remove("binding_tag", "TagName") ----
			if (FType.Equals(TEXT("binding_tag"), ESearchCase::IgnoreCase))
			{
				FName Tag;
				std::string BindingStr;

				if (Id.is<std::string>())
				{
					// Simple form: remove("binding_tag", "TagName") — removes tag entirely
					Tag = FName(NeoLuaStr::ToFString(Id.as<std::string>()));
				}
				else if (Id.is<sol::table>())
				{
					sol::table P = Id.as<sol::table>();
					std::string TagStr = P.get_or("tag", std::string());
					if (TagStr.empty()) { Session.Log(TEXT("[FAIL] remove(\"binding_tag\") -> tag required")); return sol::lua_nil; }
					Tag = FName(NeoLuaStr::ToFString(TagStr));
					BindingStr = P.get_or("binding", std::string());
				}
				else
				{
					Session.Log(TEXT("[FAIL] remove(\"binding_tag\") -> tag name (string) or {tag=.., binding?=..} required"));
					return sol::lua_nil;
				}

				// Verify tag exists
				const TMap<FName, FMovieSceneObjectBindingIDs>& TagMap = MovieScene->AllTaggedBindings();
				if (!TagMap.Contains(Tag))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"binding_tag\") -> tag '%s' not found"), *Tag.ToString()));
					return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "SeqRemoveBindingTag", "Remove Binding Tag"));
				MovieScene->Modify();

				if (BindingStr.empty())
				{
					// Remove tag entirely
					MovieScene->RemoveTag(Tag);
					Sequence->MarkPackageDirty();
					Session.Log(FString::Printf(TEXT("[OK] remove(\"binding_tag\", \"%s\") — tag removed entirely"), *Tag.ToString()));
					return sol::make_object(Lua, true);
				}

				// Untag a specific binding
				FString FBinding = NeoLuaStr::ToFString(BindingStr);
				FGuid BindingGuid = FindBindingByName(MovieScene, FBinding);
				if (!BindingGuid.IsValid())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"binding_tag\") -> binding '%s' not found"), *FBinding));
					return sol::lua_nil;
				}

				UE::MovieScene::FFixedObjectBindingID FixedID(BindingGuid, MovieSceneSequenceID::Root);
				MovieScene->UntagBinding(Tag, FixedID);
				Sequence->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove(\"binding_tag\", tag=\"%s\", binding=\"%s\")"), *Tag.ToString(), *FBinding));
				return sol::make_object(Lua, true);
			}

#if WITH_EDITORONLY_DATA
			// ---- remove("section_group", {binding, track_type, track_index?, section_index?}) ----
			if (FType.Equals(TEXT("section_group"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<sol::table>()) { Session.Log(TEXT("[FAIL] remove(\"section_group\") -> {binding?, track_type, track_index?, section_index?} required")); return sol::lua_nil; }
				sol::table P = Id.as<sol::table>();
				std::string BindingStr = P.get_or("binding", std::string());
				std::string TrackTypeStr = P.get_or("track_type", std::string());
				if (TrackTypeStr.empty()) { Session.Log(TEXT("[FAIL] remove(\"section_group\") -> track_type required")); return sol::lua_nil; }

				FString FTrackType = NeoLuaStr::ToFString(TrackTypeStr);
				UClass* TrackClass = ResolveTrackClass(FTrackType);
				if (!TrackClass) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"section_group\") -> unknown track type '%s'. Call list(\"track_types\") for valid names."), *FTrackType)); return sol::lua_nil; }

				int32 TrackIdx = P.get_or("track_index", 0);
				int32 SectionIdx = P.get_or("section_index", 0);

				UMovieSceneTrack* Track = nullptr;
				if (BindingStr.empty())
				{
					Track = FindMasterTrackByClass(MovieScene, TrackClass, TrackIdx);
				}
				else
				{
					FGuid Guid = FindBindingByName(MovieScene, NeoLuaStr::ToFString(BindingStr));
					if (!Guid.IsValid()) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"section_group\") -> binding '%s' not found"), UTF8_TO_TCHAR(BindingStr.c_str()))); return sol::lua_nil; }
					const FMovieSceneBinding* Binding = MovieScene->FindBinding(Guid);
					if (!Binding) { Session.Log(TEXT("[FAIL] remove(\"section_group\") -> binding not found")); return sol::lua_nil; }
					int32 Found = 0;
					for (UMovieSceneTrack* T : Binding->GetTracks())
					{
						if (T && T->GetClass()->IsChildOf(TrackClass))
						{
							if (Found == TrackIdx) { Track = T; break; }
							++Found;
						}
					}
				}

				if (!Track) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"section_group\") -> track '%s' index %d not found"), *FTrackType, TrackIdx)); return sol::lua_nil; }

				const TArray<UMovieSceneSection*>& AllSections = Track->GetAllSections();
				if (!AllSections.IsValidIndex(SectionIdx))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"section_group\") -> section index %d out of range"), SectionIdx));
					return sol::lua_nil;
				}

				UMovieSceneSection* Section = AllSections[SectionIdx];
				const FScopedTransaction Tx(NSLOCTEXT("AIK", "SeqUngroupSection", "Ungroup Section"));
				MovieScene->Modify();
				MovieScene->UngroupSection(*Section);
				MovieScene->CleanSectionGroups();
				Sequence->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] remove(\"section_group\") — ungrouped section %d of '%s'"), SectionIdx, *FTrackType));
				return sol::make_object(Lua, true);
			}

			// ---- remove("node_group", "GroupName") ----
			if (FType.Equals(TEXT("node_group"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<std::string>()) { Session.Log(TEXT("[FAIL] remove(\"node_group\") -> group name (string) required")); return sol::lua_nil; }
				FName GroupName = FName(NeoLuaStr::ToFString(Id.as<std::string>()));

				UMovieSceneNodeGroupCollection& NGC = MovieScene->GetNodeGroups();
				UMovieSceneNodeGroup* Target = nullptr;
				for (UMovieSceneNodeGroup* NG : NGC)
				{
					if (NG && NG->GetName() == GroupName)
					{
						Target = NG;
						break;
					}
				}
				if (!Target)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"node_group\") -> group '%s' not found"), *GroupName.ToString()));
					return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "SeqRemoveNodeGroup", "Remove Node Group"));
				MovieScene->Modify();
				NGC.RemoveNodeGroup(Target);
				Sequence->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] remove(\"node_group\", \"%s\")"), *GroupName.ToString()));
				return sol::make_object(Lua, true);
			}
#endif

			Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type. Valid: binding, track, section, keyframe, marked_frame, marked_frames, folder, binding_tag, section_group, node_group"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// configure(type, id_or_params, params?)
		// ================================================================
		AssetObj.set_function("configure", [Sequence, MovieScene, &Session](sol::table /*self*/,
			const std::string& Type, sol::object IdOrParams, sol::optional<sol::table> ExtraParams, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = NeoLuaStr::ToFString(Type);

			// ---- configure("sequence", {display_rate?, tick_resolution?, evaluation_type?, clock_source?}) ----
			if (FType.Equals(TEXT("sequence"), ESearchCase::IgnoreCase))
			{
				sol::table P = IdOrParams.is<sol::table>() ? IdOrParams.as<sol::table>() : sol::table();
				if (!P.valid())
				{
					Session.Log(TEXT("[FAIL] configure(\"sequence\") -> params table required"));
					return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "SeqCfgSequence", "Configure Sequencer Settings"));
				MovieScene->Modify();
				int32 Changes = 0;

				// display_rate: number (fps) or table {numerator, denominator}
				sol::object DisplayRateObj = P["display_rate"];
				if (DisplayRateObj.valid() && DisplayRateObj.get_type() != sol::type::lua_nil)
				{
					FFrameRate NewRate;
					if (DisplayRateObj.is<sol::table>())
					{
						sol::table RT = DisplayRateObj.as<sol::table>();
						NewRate.Numerator = RT.get_or("numerator", 30);
						NewRate.Denominator = RT.get_or("denominator", 1);
					}
					else
					{
						int32 Fps = DisplayRateObj.is<int>() ? DisplayRateObj.as<int>() : static_cast<int32>(DisplayRateObj.as<double>());
						NewRate = FFrameRate(Fps, 1);
					}
					MovieScene->SetDisplayRate(NewRate);
					Changes++;
				}

				// tick_resolution: number or table {numerator, denominator}
				// Uses the same retiming path as Sequencer's UI (SSequencerTimePanel): MigrateFrameTimes
				// retimes playback range, selection range, every track/section, camera cuts, marked
				// frames, and easing — and recurses into sub-sequences. SetTickResolutionDirectly
				// would reinterpret existing frame values instead, silently breaking content.
				sol::object TickResObj = P["tick_resolution"];
				if (TickResObj.valid() && TickResObj.get_type() != sol::type::lua_nil)
				{
					FFrameRate NewRate;
					if (TickResObj.is<sol::table>())
					{
						sol::table RT = TickResObj.as<sol::table>();
						NewRate.Numerator = RT.get_or("numerator", 24000);
						NewRate.Denominator = RT.get_or("denominator", 1);
					}
					else
					{
						int32 Ticks = TickResObj.is<int>() ? TickResObj.as<int>() : static_cast<int32>(TickResObj.as<double>());
						NewRate = FFrameRate(Ticks, 1);
					}
					const FFrameRate OldRate = MovieScene->GetTickResolution();
					if (NewRate == OldRate)
					{
						MovieScene->SetTickResolutionDirectly(NewRate);
					}
					else
					{
						UE::MovieScene::TimeHelpers::MigrateFrameTimes(OldRate, NewRate, MovieScene, /*bApplyRecursively*/ true);
					}
					Changes++;
				}

				// evaluation_type: "frame_locked" or "with_sub_frames"
				std::string EvalStr = P.get_or<std::string>("evaluation_type", "");
				if (!EvalStr.empty())
				{
					FString EvalType = NeoLuaStr::ToFString(EvalStr);
					if (EvalType.Equals(TEXT("frame_locked"), ESearchCase::IgnoreCase) || EvalType.Equals(TEXT("FrameLocked"), ESearchCase::IgnoreCase))
					{
						MovieScene->SetEvaluationType(EMovieSceneEvaluationType::FrameLocked);
						Changes++;
					}
					else if (EvalType.Equals(TEXT("with_sub_frames"), ESearchCase::IgnoreCase) || EvalType.Equals(TEXT("WithSubFrames"), ESearchCase::IgnoreCase))
					{
						MovieScene->SetEvaluationType(EMovieSceneEvaluationType::WithSubFrames);
						Changes++;
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[FAIL] configure(\"sequence\") -> unknown evaluation_type '%s'. Valid: frame_locked, with_sub_frames"), *EvalType));
						return sol::lua_nil;
					}
				}

				// clock_source: "tick", "platform", "audio", "relative_timecode", "timecode", "play_every_frame", "custom"
				std::string ClockStr = P.get_or<std::string>("clock_source", "");
				if (!ClockStr.empty())
				{
					FString ClockSrc = NeoLuaStr::ToFString(ClockStr);
					EUpdateClockSource NewClock = EUpdateClockSource::Tick;

					if (ClockSrc.Equals(TEXT("tick"), ESearchCase::IgnoreCase))
						NewClock = EUpdateClockSource::Tick;
					else if (ClockSrc.Equals(TEXT("platform"), ESearchCase::IgnoreCase))
						NewClock = EUpdateClockSource::Platform;
					else if (ClockSrc.Equals(TEXT("audio"), ESearchCase::IgnoreCase))
						NewClock = EUpdateClockSource::Audio;
					else if (ClockSrc.Equals(TEXT("relative_timecode"), ESearchCase::IgnoreCase) || ClockSrc.Equals(TEXT("RelativeTimecode"), ESearchCase::IgnoreCase))
						NewClock = EUpdateClockSource::RelativeTimecode;
					else if (ClockSrc.Equals(TEXT("timecode"), ESearchCase::IgnoreCase))
						NewClock = EUpdateClockSource::Timecode;
					else if (ClockSrc.Equals(TEXT("play_every_frame"), ESearchCase::IgnoreCase) || ClockSrc.Equals(TEXT("PlayEveryFrame"), ESearchCase::IgnoreCase))
						NewClock = EUpdateClockSource::PlayEveryFrame;
					else if (ClockSrc.Equals(TEXT("custom"), ESearchCase::IgnoreCase))
						NewClock = EUpdateClockSource::Custom;
					else
					{
						Session.Log(FString::Printf(TEXT("[FAIL] configure(\"sequence\") -> unknown clock_source '%s'. Valid: tick, platform, audio, relative_timecode, timecode, play_every_frame, custom"), *ClockSrc));
						return sol::lua_nil;
					}

					MovieScene->SetClockSource(NewClock);
					Changes++;
				}

				// playback_range_locked
				sol::optional<bool> PBLockedOpt = P.get<sol::optional<bool>>("playback_range_locked");
				if (PBLockedOpt.has_value())
				{
					MovieScene->SetPlaybackRangeLocked(PBLockedOpt.value());
					Changes++;
				}

				// view_range: {start, end} in seconds
				sol::optional<sol::table> ViewRangeOpt = P.get<sol::optional<sol::table>>("view_range");
				if (ViewRangeOpt.has_value())
				{
					sol::table VR = ViewRangeOpt.value();
					double VStart = VR.get_or("start", VR.get_or(1, 0.0));
					double VEnd = VR.get_or("end", VR.get_or(2, 10.0));
					if (VEnd <= VStart)
					{
						Session.Log(TEXT("[FAIL] configure(\"sequence\") -> view_range end must be > start"));
						return sol::lua_nil;
					}
					MovieScene->SetViewRange(VStart, VEnd);
					Changes++;
				}

				// working_range: {start, end} in seconds
				sol::optional<sol::table> WorkRangeOpt = P.get<sol::optional<sol::table>>("working_range");
				if (WorkRangeOpt.has_value())
				{
					sol::table WR = WorkRangeOpt.value();
					double WStart = WR.get_or("start", WR.get_or(1, 0.0));
					double WEnd = WR.get_or("end", WR.get_or(2, 10.0));
					if (WEnd <= WStart)
					{
						Session.Log(TEXT("[FAIL] configure(\"sequence\") -> working_range end must be > start"));
						return sol::lua_nil;
					}
					MovieScene->SetWorkingRange(WStart, WEnd);
					Changes++;
				}

#if WITH_EDITORONLY_DATA
				// read_only — editor-only flag that locks the sequence from mutation.
				sol::optional<bool> ReadOnlyOpt = P.get<sol::optional<bool>>("read_only");
				if (ReadOnlyOpt.has_value())
				{
					MovieScene->SetReadOnly(ReadOnlyOpt.value());
					Changes++;
				}
#endif

				// selection_range: {start, end} in seconds. Mirrors playback_range.
				sol::optional<sol::table> SelRangeOpt = P.get<sol::optional<sol::table>>("selection_range");
				if (SelRangeOpt.has_value())
				{
					sol::table SR = SelRangeOpt.value();
					double SStart = SR.get_or("start", SR.get_or(1, 0.0));
					double SEnd = SR.get_or("end", SR.get_or(2, 0.0));
					if (SEnd < SStart)
					{
						Session.Log(TEXT("[FAIL] configure(\"sequence\") -> selection_range end must be >= start"));
						return sol::lua_nil;
					}
					FFrameNumber SF = SecondsToFrame(MovieScene, SStart);
					FFrameNumber EF = SecondsToFrame(MovieScene, SEnd);
					MovieScene->SetSelectionRange(TRange<FFrameNumber>(SF, EF));
					Changes++;
				}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
				// custom_clock: assign a UMovieSceneClock instance via class path. When
				// clock_source="custom" is also set, evaluation uses this clock.
				// We must verify IsChildOf(UMovieSceneClock) explicitly — NewObject<T>
				// asserts under DO_CHECK when the provided UClass is not a T-subclass,
				// so without this guard "/Script/Engine.Actor" would crash instead of
				// returning a clean Lua fail.
				std::string CustomClockPath = P.get_or<std::string>("custom_clock", "");
				if (!CustomClockPath.empty())
				{
					FString FCC = NeoLuaStr::ToFString(CustomClockPath);
					FString Canon = FCC;
					UObject* Obj = NeoLuaType::ResolveType(Canon);
					UClass* ClockClass = Cast<UClass>(Obj);
					if (!ClockClass)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] configure(\"sequence\") -> custom_clock '%s' did not resolve to a UClass. Pass a /Script or /Game class path that derives from UMovieSceneClock."), *FCC));
						return sol::lua_nil;
					}
					if (!ClockClass->IsChildOf(UMovieSceneClock::StaticClass()))
					{
						Session.Log(FString::Printf(TEXT("[FAIL] configure(\"sequence\") -> custom_clock '%s' must derive from UMovieSceneClock."), *FCC));
						return sol::lua_nil;
					}
					if (ClockClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
					{
						Session.Log(FString::Printf(TEXT("[FAIL] configure(\"sequence\") -> custom_clock '%s' is abstract or deprecated."), *FCC));
						return sol::lua_nil;
					}
					UMovieSceneClock* NewClock = NewObject<UMovieSceneClock>(MovieScene, ClockClass);
					MovieScene->SetCustomClock(NewClock);
					Changes++;
				}
#endif

				if (Changes == 0)
				{
					Session.Log(TEXT("[FAIL] configure(\"sequence\") -> no valid properties specified. Valid: display_rate, tick_resolution, evaluation_type, clock_source, playback_range_locked, view_range, working_range, read_only, selection_range, custom_clock"));
					return sol::lua_nil;
				}

				Sequence->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"sequence\") -> %d changes"), Changes));

				sol::table Result = Lua.create_table();
				Result["changes"] = Changes;
				Result["display_rate"] = MovieScene->GetDisplayRate().AsDecimal();
				Result["tick_resolution"] = MovieScene->GetTickResolution().AsDecimal();
				Result["evaluation_type"] = MovieScene->GetEvaluationType() == EMovieSceneEvaluationType::FrameLocked
					? "frame_locked" : "with_sub_frames";
				const EUpdateClockSource CS = MovieScene->GetClockSource();
				const char* CSStr = "tick";
				if (CS == EUpdateClockSource::Platform) CSStr = "platform";
				else if (CS == EUpdateClockSource::Audio) CSStr = "audio";
				else if (CS == EUpdateClockSource::RelativeTimecode) CSStr = "relative_timecode";
				else if (CS == EUpdateClockSource::Timecode) CSStr = "timecode";
				else if (CS == EUpdateClockSource::PlayEveryFrame) CSStr = "play_every_frame";
				else if (CS == EUpdateClockSource::Custom) CSStr = "custom";
				Result["clock_source"] = CSStr;
				Result["playback_range_locked"] = MovieScene->IsPlaybackRangeLocked();
				{
					TRange<double> VR = MovieScene->GetEditorData().GetViewRange();
					sol::table VRT = Lua.create_table();
					VRT["start"] = VR.GetLowerBoundValue(); VRT["end"] = VR.GetUpperBoundValue();
					Result["view_range"] = VRT;
				}
				{
					TRange<double> WR = MovieScene->GetEditorData().GetWorkingRange();
					sol::table WRT = Lua.create_table();
					WRT["start"] = WR.GetLowerBoundValue(); WRT["end"] = WR.GetUpperBoundValue();
					Result["working_range"] = WRT;
				}
				return Result;
			}

			// ---- configure("marked_frames", {locked=?, globally_show=?}) ----
			if (FType.Equals(TEXT("marked_frames"), ESearchCase::IgnoreCase))
			{
				sol::table P = IdOrParams.is<sol::table>() ? IdOrParams.as<sol::table>() : sol::table();
				if (!P.valid())
				{
					Session.Log(TEXT("[FAIL] configure(\"marked_frames\") -> {locked=?, globally_show=?} required"));
					return sol::lua_nil;
				}

				sol::optional<bool> LockedOpt = P["locked"];
				sol::optional<bool> GloballyShowOpt = P["globally_show"];

				if (!LockedOpt.has_value() && !GloballyShowOpt.has_value())
				{
					// Return current state
					sol::table Result = Lua.create_table();
					Result["locked"] = MovieScene->AreMarkedFramesLocked();
#if WITH_EDITORONLY_DATA
					Result["globally_show"] = MovieScene->GetGloballyShowMarkedFrames();
#endif
					Session.Log(FString::Printf(TEXT("[OK] configure(\"marked_frames\") -> current state (locked=%s)"),
						MovieScene->AreMarkedFramesLocked() ? TEXT("true") : TEXT("false")));
					return Result;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "SeqCfgMarkers", "Configure Sequencer Marked Frames"));
				MovieScene->Modify();
				int32 Changes = 0;
				if (LockedOpt.has_value()) { MovieScene->SetMarkedFramesLocked(LockedOpt.value()); Changes++; }
#if WITH_EDITORONLY_DATA
				if (GloballyShowOpt.has_value()) { MovieScene->SetGloballyShowMarkedFrames(GloballyShowOpt.value()); Changes++; }
#endif
				Sequence->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"marked_frames\") -> %d changes"), Changes));
				return sol::make_object(Lua, true);
			}

			// ---- configure("marked_frame", index_or_label, {frame?, label?, comment?, color?, is_determinism_fence?, is_inclusive_time?}) ----
			if (FType.Equals(TEXT("marked_frame"), ESearchCase::IgnoreCase))
			{
				if (MovieScene->AreMarkedFramesLocked())
				{
					Session.Log(TEXT("[FAIL] configure(\"marked_frame\") -> marked frames are locked. Call configure(\"marked_frames\", {locked=false}) first."));
					return sol::lua_nil;
				}

				// Resolve index
				int32 MarkIdx = -1;
				if (IdOrParams.is<std::string>())
				{
					FString Label = NeoLuaStr::ToFString(IdOrParams.as<std::string>());
					MarkIdx = MovieScene->FindMarkedFrameByLabel(Label);
					if (MarkIdx < 0)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] configure(\"marked_frame\") -> label '%s' not found"), *Label));
						return sol::lua_nil;
					}
				}
				else if (IdOrParams.is<int>() || IdOrParams.is<double>())
				{
					MarkIdx = IdOrParams.is<int>() ? IdOrParams.as<int>() : static_cast<int32>(IdOrParams.as<double>());
				}
				else
				{
					Session.Log(TEXT("[FAIL] configure(\"marked_frame\") -> index (number) or label (string) required as second arg"));
					return sol::lua_nil;
				}

				const TArray<FMovieSceneMarkedFrame>& Markers = MovieScene->GetMarkedFrames();
				if (MarkIdx < 0 || MarkIdx >= Markers.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"marked_frame\") -> index %d out of range (total %d)"), MarkIdx, Markers.Num()));
					return sol::lua_nil;
				}

				if (!ExtraParams.has_value())
				{
					Session.Log(TEXT("[FAIL] configure(\"marked_frame\") -> params table required as third arg"));
					return sol::lua_nil;
				}
				sol::table P = ExtraParams.value();

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "SeqCfgMarker", "Configure Sequencer Marked Frame"));
				MovieScene->Modify();

				// We need mutable access — get a copy, modify, then set back
				FMovieSceneMarkedFrame MF = Markers[MarkIdx];
				int32 Changes = 0;

				sol::optional<int> FrameOpt = P["frame"];
				if (FrameOpt.has_value())
				{
					MF.FrameNumber = FFrameNumber(FrameOpt.value());
					Changes++;
				}

				sol::optional<std::string> LabelOpt = P["label"];
				if (LabelOpt.has_value())
				{
					MF.Label = NeoLuaStr::ToFString(LabelOpt.value());
					Changes++;
				}

#if WITH_EDITORONLY_DATA
				sol::optional<std::string> CommentOpt = P["comment"];
				if (CommentOpt.has_value())
				{
					MF.Comment = NeoLuaStr::ToFString(CommentOpt.value());
					Changes++;
				}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
				sol::optional<sol::table> ColorOpt = P["color"];
				if (ColorOpt.has_value())
				{
					MF.CustomColor = ParseLinearColorFromLua(ColorOpt.value(), MF.CustomColor);
					MF.bUseCustomColor = true;
					Changes++;
				}

				sol::optional<bool> UseColorOpt = P["use_custom_color"];
				if (UseColorOpt.has_value())
				{
					MF.bUseCustomColor = UseColorOpt.value();
					Changes++;
				}
#endif
#endif

				sol::optional<bool> FenceOpt = P["is_determinism_fence"];
				if (FenceOpt.has_value())
				{
					MF.bIsDeterminismFence = FenceOpt.value();
					Changes++;
				}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
				sol::optional<bool> InclusiveOpt = P["is_inclusive_time"];
				if (InclusiveOpt.has_value())
				{
					MF.bIsInclusiveTime = InclusiveOpt.value();
					Changes++;
				}
#endif

				if (Changes == 0)
				{
					Session.Log(TEXT("[FAIL] configure(\"marked_frame\") -> no valid properties specified"));
					return sol::lua_nil;
				}

				// Delete old and re-add modified frame (API doesn't expose mutable ref)
				MovieScene->DeleteMarkedFrame(MarkIdx);
				int32 NewIdx = MovieScene->AddMarkedFrame(MF);
				MovieScene->SortMarkedFrames();
				Sequence->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"marked_frame\", %d) -> %d changes, new index %d"),
					MarkIdx, Changes, NewIdx));
				sol::table Result = Lua.create_table();
				Result["index"] = NewIdx;
				Result["changes"] = Changes;
				return Result;
			}

			// ---- configure("binding", name, {display_name=...}) ----
			if (FType.Equals(TEXT("binding"), ESearchCase::IgnoreCase))
			{
				if (!IdOrParams.is<std::string>())
				{
					Session.Log(TEXT("[FAIL] configure(\"binding\") -> binding name (string) required as second arg"));
					return sol::lua_nil;
				}
				FString BindingName = NeoLuaStr::ToFString(IdOrParams.as<std::string>());

				if (!ExtraParams.has_value())
				{
					Session.Log(TEXT("[FAIL] configure(\"binding\") -> params table required as third arg"));
					return sol::lua_nil;
				}
				sol::table P = ExtraParams.value();

				FGuid BindingGuid = FindBindingByName(MovieScene, BindingName);
				if (!BindingGuid.IsValid())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"binding\") -> binding '%s' not found"), *BindingName));
					return sol::lua_nil;
				}

				int32 Changes = 0;
				const FScopedTransaction Tx(NSLOCTEXT("AIK", "SeqCfgBinding", "Configure Sequencer Binding"));
				MovieScene->Modify();

				sol::optional<std::string> DisplayNameOpt = P["display_name"];
				if (DisplayNameOpt.has_value())
				{
					FString DisplayName = NeoLuaStr::ToFString(DisplayNameOpt.value());
					MovieScene->SetObjectDisplayName(BindingGuid, FText::FromString(DisplayName));
					Changes++;
				}

				// sorting_order — FMovieSceneBinding::SetSortingOrder controls Outliner order.
				sol::optional<int> SortOpt = P.get<sol::optional<int>>("sorting_order");
				if (SortOpt.has_value())
				{
					if (FMovieSceneBinding* Binding = MovieScene->FindBinding(BindingGuid))
					{
						Binding->SetSortingOrder(SortOpt.value());
						Changes++;
					}
				}

				// rebind_actor — retarget a POSSESSABLE onto a different level actor while
				// preserving its tracks/sections. Spawnables don't have stored bindings
				// (their actors are instantiated at spawn time from the template), so we
				// reject the call with a clear message if applied to a spawnable.
				sol::optional<std::string> RebindOpt = P["rebind_actor"];
				if (RebindOpt.has_value())
				{
					if (!MovieScene->FindPossessable(BindingGuid))
					{
						Session.Log(TEXT("[FAIL] configure(\"binding\") -> rebind_actor only applies to possessables. For spawnables, create a new binding with the desired class."));
						return sol::lua_nil;
					}
					FString NewActorName = NeoLuaStr::ToFString(RebindOpt.value());
					AActor* NewActor = FindActorByName(NewActorName);
					if (!NewActor)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] configure(\"binding\") -> rebind_actor '%s' not found in the current editor world."), *NewActorName));
						return sol::lua_nil;
					}
					Sequence->UnbindPossessableObjects(BindingGuid);
					Sequence->BindPossessableObject(BindingGuid, *NewActor, NewActor->GetWorld());
					Changes++;
				}

				// unbind — drop all object bindings for this possessable GUID without removing
				// the FMovieSceneBinding itself (tracks survive). No-op on spawnables since
				// their bindings are computed, not stored.
				sol::optional<bool> UnbindOpt = P.get<sol::optional<bool>>("unbind");
				if (UnbindOpt.has_value() && UnbindOpt.value())
				{
					if (!MovieScene->FindPossessable(BindingGuid))
					{
						Session.Log(TEXT("[FAIL] configure(\"binding\") -> unbind only applies to possessables. Use remove(\"binding\", name) to delete a spawnable."));
						return sol::lua_nil;
					}
					Sequence->UnbindPossessableObjects(BindingGuid);
					Changes++;
				}

				// Spawnable-specific: spawn_ownership, continuously_respawn, net_addressable_name.
				if (FMovieSceneSpawnable* Spawnable = MovieScene->FindSpawnable(BindingGuid))
				{
					std::string OwnStr = P.get_or<std::string>("spawn_ownership", "");
					if (!OwnStr.empty())
					{
						FString Own = NeoLuaStr::ToFString(OwnStr);
						ESpawnOwnership NewOwn = ESpawnOwnership::InnerSequence;
						if (Own.Equals(TEXT("inner_sequence"), ESearchCase::IgnoreCase) || Own.Equals(TEXT("InnerSequence"), ESearchCase::IgnoreCase))
							NewOwn = ESpawnOwnership::InnerSequence;
						else if (Own.Equals(TEXT("master_sequence"), ESearchCase::IgnoreCase) || Own.Equals(TEXT("MasterSequence"), ESearchCase::IgnoreCase) ||
								 Own.Equals(TEXT("root_sequence"), ESearchCase::IgnoreCase) || Own.Equals(TEXT("RootSequence"), ESearchCase::IgnoreCase))
							NewOwn = ESpawnOwnership::RootSequence;
						else if (Own.Equals(TEXT("external"), ESearchCase::IgnoreCase) || Own.Equals(TEXT("External"), ESearchCase::IgnoreCase))
							NewOwn = ESpawnOwnership::External;
						else
						{
							Session.Log(FString::Printf(TEXT("[FAIL] configure(\"binding\") -> unknown spawn_ownership '%s'. Valid: inner_sequence, root_sequence, external"), *Own));
							return sol::lua_nil;
						}
						Spawnable->SetSpawnOwnership(NewOwn);
						Changes++;
					}
					sol::optional<bool> ContRespawnOpt = P.get<sol::optional<bool>>("continuously_respawn");
					if (ContRespawnOpt.has_value())
					{
						Spawnable->bContinuouslyRespawn = ContRespawnOpt.value();
						Changes++;
					}
					sol::optional<bool> NetOpt = P.get<sol::optional<bool>>("net_addressable_name");
					if (NetOpt.has_value())
					{
						Spawnable->bNetAddressableName = NetOpt.value();
						Changes++;
					}
				}

				if (Changes == 0)
				{
					Session.Log(TEXT("[FAIL] configure(\"binding\") -> no valid properties specified. Valid: display_name, sorting_order, rebind_actor, unbind, spawn_ownership, continuously_respawn, net_addressable_name"));
					return sol::lua_nil;
				}

				Sequence->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"binding\", \"%s\") -> %d changes"), *BindingName, Changes));
				sol::table Result = Lua.create_table();
				Result["changes"] = Changes;
				return Result;
			}

			// ---- configure("folder", name, {new_name?, color?, add_binding?, remove_binding?, add_track?, remove_track?}) ----
			if (FType.Equals(TEXT("folder"), ESearchCase::IgnoreCase))
			{
				if (!IdOrParams.is<std::string>())
				{
					Session.Log(TEXT("[FAIL] configure(\"folder\") -> folder name (string) required as second arg"));
					return sol::lua_nil;
				}
				FString FolderName = NeoLuaStr::ToFString(IdOrParams.as<std::string>());

				if (!ExtraParams.has_value())
				{
					Session.Log(TEXT("[FAIL] configure(\"folder\") -> params table required as third arg"));
					return sol::lua_nil;
				}
				sol::table P = ExtraParams.value();

				TArrayView<UMovieSceneFolder* const> RootFolders = MovieScene->GetRootFolders();
				UMovieSceneFolder* Folder = FindFolderByName(RootFolders, FolderName);
				if (!Folder)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"folder\") -> folder '%s' not found"), *FolderName));
					return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "SeqCfgFolder", "Configure Sequencer Folder"));
				Folder->SetFlags(RF_Transactional);
				Folder->Modify();
				int32 Changes = 0;

				// Rename
				sol::optional<std::string> NewNameOpt = P["new_name"];
				if (NewNameOpt.has_value())
				{
					FString NewName = NeoLuaStr::ToFString(NewNameOpt.value());
					Folder->SetFolderName(FName(*NewName));
					Changes++;
				}

#if WITH_EDITORONLY_DATA
				// Color
				sol::optional<sol::table> ColorOpt = P["color"];
				if (ColorOpt.has_value())
				{
					Folder->SetFolderColor(ParseColorFromLua(ColorOpt.value()));
					Changes++;
				}

				// Sorting order (lower = higher in the Outliner)
				sol::optional<int> SortOpt = P.get<sol::optional<int>>("sorting_order");
				if (SortOpt.has_value())
				{
					Folder->SetSortingOrder(SortOpt.value());
					Changes++;
				}
#endif

				// Add binding to folder
				sol::optional<std::string> AddBindOpt = P["add_binding"];
				if (AddBindOpt.has_value())
				{
					FString BindName = NeoLuaStr::ToFString(AddBindOpt.value());
					FGuid BindGuid = FindBindingByName(MovieScene, BindName);
					if (BindGuid.IsValid())
					{
						Folder->AddChildObjectBinding(BindGuid);
						Changes++;
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[FAIL] configure(\"folder\") -> binding '%s' not found for add_binding"), *BindName));
						return sol::lua_nil;
					}
				}

				// Remove binding from folder
				sol::optional<std::string> RemBindOpt = P["remove_binding"];
				if (RemBindOpt.has_value())
				{
					FString BindName = NeoLuaStr::ToFString(RemBindOpt.value());
					FGuid BindGuid = FindBindingByName(MovieScene, BindName);
					if (BindGuid.IsValid())
					{
						Folder->RemoveChildObjectBinding(BindGuid);
						Changes++;
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[FAIL] configure(\"folder\") -> binding '%s' not found for remove_binding"), *BindName));
						return sol::lua_nil;
					}
				}

				// Add track to folder: {binding?, track_type, track_index?}
				sol::optional<sol::table> AddTrackOpt = P["add_track"];
				if (AddTrackOpt.has_value())
				{
					sol::table AT = AddTrackOpt.value();
					std::string TrackTypeStr = AT.get_or("track_type", std::string());
					if (TrackTypeStr.empty())
					{
						Session.Log(TEXT("[FAIL] configure(\"folder\") -> add_track requires track_type"));
						return sol::lua_nil;
					}
					UClass* TrackClass = ResolveTrackClass(NeoLuaStr::ToFString(TrackTypeStr));
					if (!TrackClass)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] configure(\"folder\") -> unknown track type '%s'. Call list(\"track_types\") for valid names."), UTF8_TO_TCHAR(TrackTypeStr.c_str())));
						return sol::lua_nil;
					}

					std::string BindStr = AT.get_or("binding", std::string());
					int32 TrackIdx = AT.get_or("track_index", 0);
					UMovieSceneTrack* TargetTrack = nullptr;

					if (BindStr.empty())
					{
						// Master track
						int32 MatchCount = 0;
						for (UMovieSceneTrack* Track : MovieScene->GetTracks())
						{
							if (Track && Track->IsA(TrackClass))
							{
								if (MatchCount == TrackIdx) { TargetTrack = Track; break; }
								MatchCount++;
							}
						}
						if (!TargetTrack && TrackClass->IsChildOf(UMovieSceneCameraCutTrack::StaticClass()))
							TargetTrack = MovieScene->GetCameraCutTrack();
					}
					else
					{
						FGuid BindGuid = FindBindingByName(MovieScene, NeoLuaStr::ToFString(BindStr));
						if (BindGuid.IsValid())
						{
							const FMovieSceneBinding* Binding = MovieScene->FindBinding(BindGuid);
							if (Binding)
							{
								int32 MatchCount = 0;
								for (UMovieSceneTrack* Track : Binding->GetTracks())
								{
									if (Track && Track->IsA(TrackClass))
									{
										if (MatchCount == TrackIdx) { TargetTrack = Track; break; }
										MatchCount++;
									}
								}
							}
						}
					}

					if (TargetTrack)
					{
						Folder->AddChildTrack(TargetTrack);
						Changes++;
					}
					else
					{
						Session.Log(FString::Printf(
							TEXT("[FAIL] configure(\"folder\") -> add_track could not resolve {binding='%s', track_type='%s', track_index=%d}. Call list(\"bindings\") to confirm the exact tuple."),
							BindStr.empty() ? TEXT("(master)") : UTF8_TO_TCHAR(BindStr.c_str()),
							UTF8_TO_TCHAR(TrackTypeStr.c_str()),
							TrackIdx));
						return sol::lua_nil;
					}
				}

				// Remove track from folder: {binding?, track_type, track_index?}
				sol::optional<sol::table> RemTrackOpt = P["remove_track"];
				if (RemTrackOpt.has_value())
				{
					sol::table RT = RemTrackOpt.value();
					std::string TrackTypeStr = RT.get_or("track_type", std::string());
					if (TrackTypeStr.empty())
					{
						Session.Log(TEXT("[FAIL] configure(\"folder\") -> remove_track requires track_type"));
						return sol::lua_nil;
					}
					UClass* TrackClass = ResolveTrackClass(NeoLuaStr::ToFString(TrackTypeStr));
					if (!TrackClass)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] configure(\"folder\") -> unknown track type '%s'. Call list(\"track_types\") for valid names."), UTF8_TO_TCHAR(TrackTypeStr.c_str())));
						return sol::lua_nil;
					}

					std::string BindStr = RT.get_or("binding", std::string());
					int32 TrackIdx = RT.get_or("track_index", 0);
					UMovieSceneTrack* TargetTrack = nullptr;

					if (BindStr.empty())
					{
						int32 MatchCount = 0;
						for (UMovieSceneTrack* Track : MovieScene->GetTracks())
						{
							if (Track && Track->IsA(TrackClass))
							{
								if (MatchCount == TrackIdx) { TargetTrack = Track; break; }
								MatchCount++;
							}
						}
						if (!TargetTrack && TrackClass->IsChildOf(UMovieSceneCameraCutTrack::StaticClass()))
							TargetTrack = MovieScene->GetCameraCutTrack();
					}
					else
					{
						FGuid BindGuid = FindBindingByName(MovieScene, NeoLuaStr::ToFString(BindStr));
						if (BindGuid.IsValid())
						{
							const FMovieSceneBinding* Binding = MovieScene->FindBinding(BindGuid);
							if (Binding)
							{
								int32 MatchCount = 0;
								for (UMovieSceneTrack* Track : Binding->GetTracks())
								{
									if (Track && Track->IsA(TrackClass))
									{
										if (MatchCount == TrackIdx) { TargetTrack = Track; break; }
										MatchCount++;
									}
								}
							}
						}
					}

					if (TargetTrack)
					{
						Folder->RemoveChildTrack(TargetTrack);
						Changes++;
					}
					else
					{
						Session.Log(FString::Printf(
							TEXT("[FAIL] configure(\"folder\") -> remove_track could not resolve {binding='%s', track_type='%s', track_index=%d}. Call list(\"bindings\") to confirm the exact tuple."),
							BindStr.empty() ? TEXT("(master)") : UTF8_TO_TCHAR(BindStr.c_str()),
							UTF8_TO_TCHAR(TrackTypeStr.c_str()),
							TrackIdx));
						return sol::lua_nil;
					}
				}

				if (Changes == 0)
				{
					Session.Log(TEXT("[FAIL] configure(\"folder\") -> no valid properties specified. Valid: new_name, color, add_binding, remove_binding, add_track, remove_track"));
					return sol::lua_nil;
				}

				Sequence->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"folder\", \"%s\") -> %d changes"), *FolderName, Changes));
				sol::table Result = Lua.create_table();
				Result["changes"] = Changes;
				return Result;
			}

			// ---- configure("track", {binding?, track_type, track_index?}, {display_name?, color?, muted?}) ----
			if (FType.Equals(TEXT("track"), ESearchCase::IgnoreCase))
			{
				if (!IdOrParams.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"track\") -> identifier table {binding?, track_type, track_index?} required as second arg"));
					return sol::lua_nil;
				}
				sol::table Id = IdOrParams.as<sol::table>();

				if (!ExtraParams.has_value())
				{
					Session.Log(TEXT("[FAIL] configure(\"track\") -> params table {display_name?, color?, muted?} required as third arg"));
					return sol::lua_nil;
				}
				sol::table P = ExtraParams.value();

				std::string TrackTypeStr = Id.get_or("track_type", std::string());
				if (TrackTypeStr.empty())
				{
					Session.Log(TEXT("[FAIL] configure(\"track\") -> track_type required in identifier table"));
					return sol::lua_nil;
				}

				UClass* TrackClass = ResolveTrackClass(NeoLuaStr::ToFString(TrackTypeStr));
				if (!TrackClass)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"track\") -> unknown track type '%s'. Call list(\"track_types\") for valid names."), UTF8_TO_TCHAR(TrackTypeStr.c_str())));
					return sol::lua_nil;
				}

				std::string BindStr = Id.get_or("binding", std::string());
				int32 TrackIdx = Id.get_or("track_index", 0);
				UMovieSceneTrack* TargetTrack = nullptr;

				if (BindStr.empty())
				{
					// Master track
					int32 MatchCount = 0;
					for (UMovieSceneTrack* Track : MovieScene->GetTracks())
					{
						if (Track && Track->IsA(TrackClass))
						{
							if (MatchCount == TrackIdx) { TargetTrack = Track; break; }
							MatchCount++;
						}
					}
					if (!TargetTrack && TrackClass->IsChildOf(UMovieSceneCameraCutTrack::StaticClass()))
						TargetTrack = MovieScene->GetCameraCutTrack();
				}
				else
				{
					FGuid BindGuid = FindBindingByName(MovieScene, NeoLuaStr::ToFString(BindStr));
					if (!BindGuid.IsValid())
					{
						Session.Log(FString::Printf(TEXT("[FAIL] configure(\"track\") -> binding '%s' not found"), UTF8_TO_TCHAR(BindStr.c_str())));
						return sol::lua_nil;
					}
					const FMovieSceneBinding* Binding = MovieScene->FindBinding(BindGuid);
					if (Binding)
					{
						int32 MatchCount = 0;
						for (UMovieSceneTrack* Track : Binding->GetTracks())
						{
							if (Track && Track->IsA(TrackClass))
							{
								if (MatchCount == TrackIdx) { TargetTrack = Track; break; }
								MatchCount++;
							}
						}
					}
				}

				if (!TargetTrack)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"track\") -> track not found (type='%s', binding='%s', index=%d)"),
						UTF8_TO_TCHAR(TrackTypeStr.c_str()), UTF8_TO_TCHAR(BindStr.c_str()), TrackIdx));
					return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "SeqCfgTrack", "Configure Sequencer Track"));
				TargetTrack->SetFlags(RF_Transactional);
				TargetTrack->Modify();
				int32 Changes = 0;

#if WITH_EDITORONLY_DATA
				// display_name — only works on UMovieSceneNameableTrack subclasses
				sol::optional<std::string> DisplayNameOpt = P["display_name"];
				if (DisplayNameOpt.has_value())
				{
					UMovieSceneNameableTrack* NameableTrack = Cast<UMovieSceneNameableTrack>(TargetTrack);
					if (NameableTrack)
					{
						NameableTrack->SetDisplayName(FText::FromString(NeoLuaStr::ToFString(DisplayNameOpt.value())));
						Changes++;
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure(\"track\") -> track type '%s' does not support display_name (not a nameable track)"),
							UTF8_TO_TCHAR(TrackTypeStr.c_str())));
					}
				}

				// color — SetColorTint
				sol::optional<sol::table> ColorOpt = P["color"];
				if (ColorOpt.has_value())
				{
					TargetTrack->SetColorTint(ParseColorFromLua(ColorOpt.value()));
					Changes++;
				}
#endif

				// muted — SetEvalDisabled
				sol::optional<bool> MutedOpt = P["muted"];
				if (MutedOpt.has_value())
				{
					TargetTrack->SetEvalDisabled(MutedOpt.value());
					Changes++;
				}

				// Row-level eval disable: {row_eval_disabled={row=N, disabled=true/false}}
				// Mirrors UMovieSceneTrack::SetRowEvalDisabled (serialized state).
				sol::optional<sol::table> RowEvalOpt = P.get<sol::optional<sol::table>>("row_eval_disabled");
				if (RowEvalOpt.has_value())
				{
					sol::table RE = RowEvalOpt.value();
					const int32 Row = RE.get_or("row", 0);
					const bool bDisabled = RE.get_or("disabled", true);
					TargetTrack->SetRowEvalDisabled(bDisabled, Row);
					Changes++;
				}

#if WITH_EDITOR && ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
				// Local (non-serialized) row eval toggle: {local_row_eval_disabled={row, disabled}}
				sol::optional<sol::table> LocalRowEvalOpt = P.get<sol::optional<sol::table>>("local_row_eval_disabled");
				if (LocalRowEvalOpt.has_value())
				{
					sol::table LRE = LocalRowEvalOpt.value();
					const int32 Row = LRE.get_or("row", 0);
					const bool bDisabled = LRE.get_or("disabled", true);
					TargetTrack->SetLocalRowEvalDisabled(bDisabled, Row);
					Changes++;
				}
#endif

				// Boolean actions (perform when explicitly true).
				sol::optional<bool> FixRowsOpt = P.get<sol::optional<bool>>("fix_row_indices");
				if (FixRowsOpt.has_value() && FixRowsOpt.value())
				{
					TargetTrack->FixRowIndices();
					Changes++;
				}
				sol::optional<bool> UpdateEasingOpt = P.get<sol::optional<bool>>("update_easing");
				if (UpdateEasingOpt.has_value() && UpdateEasingOpt.value())
				{
					TargetTrack->UpdateEasing();
					Changes++;
				}
				sol::optional<bool> RemoveAllAnimOpt = P.get<sol::optional<bool>>("remove_all_animation_data");
				if (RemoveAllAnimOpt.has_value() && RemoveAllAnimOpt.value())
				{
					TargetTrack->RemoveAllAnimationData();
					Changes++;
				}

				// sorting_order — UMovieSceneTrack::SetSortingOrder controls Outliner order
				// among master/binding tracks. Distinct from row_index (per-section).
				sol::optional<int> TrackSortOpt = P.get<sol::optional<int>>("sorting_order");
				if (TrackSortOpt.has_value())
				{
					TargetTrack->SetSortingOrder(TrackSortOpt.value());
					Changes++;
				}

				// Choose which section this track keys into when multiple sections overlap.
				// {section_to_key = <section_index>} or {section_to_key = nil} to clear.
				sol::optional<int> SectionToKeyOpt = P.get<sol::optional<int>>("section_to_key");
				if (SectionToKeyOpt.has_value())
				{
					const int32 SK = SectionToKeyOpt.value();
					const TArray<UMovieSceneSection*>& Secs = TargetTrack->GetAllSections();
					if (SK < 0)
					{
						TargetTrack->SetSectionToKey(nullptr);
						Changes++;
					}
					else if (SK < Secs.Num() && Secs[SK])
					{
						TargetTrack->SetSectionToKey(Secs[SK]);
						Changes++;
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[FAIL] configure(\"track\") -> section_to_key %d out of range (track has %d sections)"), SK, Secs.Num()));
						return sol::lua_nil;
					}
				}

				if (Changes == 0)
				{
					Session.Log(TEXT("[FAIL] configure(\"track\") -> no valid properties specified. Valid: display_name, color, muted, sorting_order, row_eval_disabled, local_row_eval_disabled, fix_row_indices, update_easing, remove_all_animation_data, section_to_key"));
					return sol::lua_nil;
				}

				Sequence->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"track\", type='%s') -> %d changes"),
					UTF8_TO_TCHAR(TrackTypeStr.c_str()), Changes));

				sol::table Result = Lua.create_table();
				Result["changes"] = Changes;
#if WITH_EDITORONLY_DATA
				Result["display_name"] = TCHAR_TO_UTF8(*TargetTrack->GetDisplayName().ToString());
				const FColor& Tint = TargetTrack->GetColorTint();
				sol::table ColorResult = Lua.create_table();
				ColorResult["r"] = Tint.R;
				ColorResult["g"] = Tint.G;
				ColorResult["b"] = Tint.B;
				ColorResult["a"] = Tint.A;
				Result["color"] = ColorResult;
#endif
				Result["muted"] = TargetTrack->IsEvalDisabled();
				return Result;
			}

#if WITH_EDITORONLY_DATA
			// ---- configure("node_group", "GroupName", {new_name?, add_node?, remove_node?, enable_filter?}) ----
			if (FType.Equals(TEXT("node_group"), ESearchCase::IgnoreCase))
			{
				if (!IdOrParams.is<std::string>()) { Session.Log(TEXT("[FAIL] configure(\"node_group\") -> group name (string) required as second arg")); return sol::lua_nil; }
				if (!ExtraParams.has_value()) { Session.Log(TEXT("[FAIL] configure(\"node_group\") -> params table required as third arg")); return sol::lua_nil; }

				FName GroupName = FName(NeoLuaStr::ToFString(IdOrParams.as<std::string>()));
				sol::table P = ExtraParams.value();

				UMovieSceneNodeGroupCollection& NGC = MovieScene->GetNodeGroups();
				UMovieSceneNodeGroup* Target = nullptr;
				for (UMovieSceneNodeGroup* NG : NGC)
				{
					if (NG && NG->GetName() == GroupName)
					{
						Target = NG;
						break;
					}
				}
				if (!Target)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"node_group\") -> group '%s' not found"), *GroupName.ToString()));
					return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "SeqConfigureNodeGroup", "Configure Node Group"));
				MovieScene->Modify();

				TArray<FString> Changes;

				// Rename
				std::string NewNameStr = P.get_or("new_name", std::string());
				if (!NewNameStr.empty())
				{
					FName NewName = FName(NeoLuaStr::ToFString(NewNameStr));
					Target->SetName(NewName);
					Changes.Add(FString::Printf(TEXT("name->%s"), *NewName.ToString()));
				}

				// Enable filter
				sol::optional<bool> EnableFilterOpt = P.get<sol::optional<bool>>("enable_filter");
				if (EnableFilterOpt.has_value())
				{
					Target->SetEnableFilter(EnableFilterOpt.value());
					Changes.Add(FString::Printf(TEXT("enable_filter=%s"), EnableFilterOpt.value() ? TEXT("true") : TEXT("false")));
				}

				// Add node
				std::string AddNodeStr = P.get_or("add_node", std::string());
				if (!AddNodeStr.empty())
				{
					FString NodePath = NeoLuaStr::ToFString(AddNodeStr);
					Target->AddNode(NodePath);
					Changes.Add(FString::Printf(TEXT("+node=%s"), *NodePath));
				}

				// Remove node
				std::string RemoveNodeStr = P.get_or("remove_node", std::string());
				if (!RemoveNodeStr.empty())
				{
					FString NodePath = NeoLuaStr::ToFString(RemoveNodeStr);
					Target->RemoveNode(NodePath);
					Changes.Add(FString::Printf(TEXT("-node=%s"), *NodePath));
				}

				Sequence->MarkPackageDirty();

				FString ChangesSummary = FString::Join(Changes, TEXT(", "));
				Session.Log(FString::Printf(TEXT("[OK] configure(\"node_group\", \"%s\") -> %s"),
					*GroupName.ToString(), Changes.Num() > 0 ? *ChangesSummary : TEXT("no changes")));

				sol::table Result = Lua.create_table();
				Result["name"] = TCHAR_TO_UTF8(*Target->GetName().ToString());
				Result["enable_filter"] = Target->GetEnableFilter();
				return Result;
			}
#endif

			Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown type. Valid: sequence, binding, track, folder, marked_frame, marked_frames, node_group"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// set_transforms(params | array of params)
		// ================================================================
		AssetObj.set_function("set_transforms", [Sequence, MovieScene, &Session](sol::table /*self*/,
			sol::table Input, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			// Determine if single table or array of tables
			TArray<sol::table> Entries;
			if (Input[1].valid())
			{
				// Array of tables
				for (auto& kv : Input)
				{
					if (kv.second.is<sol::table>())
						Entries.Add(kv.second.as<sol::table>());
				}
			}
			else
			{
				// Single table
				Entries.Add(Input);
			}

			if (Entries.Num() == 0)
			{
				Session.Log(TEXT("[FAIL] set_transforms -> at least one transform entry required"));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "SeqSetXform", "Set Sequencer Transforms"));
			int32 TotalKeys = 0;
			int32 SuccessCount = 0;

			for (const sol::table& P : Entries)
			{
				std::string BindStr = P.get_or("binding", std::string());
				if (BindStr.empty())
				{
					Session.Log(TEXT("[FAIL] set_transforms -> binding required"));
					continue;
				}
				FString FBinding = NeoLuaStr::ToFString(BindStr);
				double Time = P.get_or("time", 0.0);
				std::string InterpStr = P.get_or("interp", std::string("cubic"));
				FString FInterp = NeoLuaStr::ToFString(InterpStr);

				bool bHasLoc = false, bHasRot = false, bHasScale = false;
				FVector Loc(0); FRotator Rot(0); FVector Scale(1);

				sol::optional<sol::table> LocOpt = P["location"];
				if (LocOpt.has_value())
				{
					sol::table LT = LocOpt.value();
					if (LT["x"].valid())
					{
						// Named fields: {x=N, y=N, z=N}
						Loc.X = LT.get_or("x", 0.0); Loc.Y = LT.get_or("y", 0.0); Loc.Z = LT.get_or("z", 0.0);
						bHasLoc = true;
					}
					else if (LT[1].valid() && LT[2].valid() && LT[3].valid())
					{
						// Array: {X, Y, Z}
						Loc.X = LT[1].get<double>(); Loc.Y = LT[2].get<double>(); Loc.Z = LT[3].get<double>();
						bHasLoc = true;
					}
				}
				sol::optional<sol::table> RotOpt = P["rotation"];
				if (RotOpt.has_value())
				{
					sol::table RT = RotOpt.value();
					if (RT["pitch"].valid() || RT["yaw"].valid() || RT["roll"].valid())
					{
						// Named fields: {pitch=N, yaw=N, roll=N}
						Rot.Pitch = RT.get_or("pitch", 0.0); Rot.Yaw = RT.get_or("yaw", 0.0); Rot.Roll = RT.get_or("roll", 0.0);
						bHasRot = true;
					}
					else if (RT[1].valid() && RT[2].valid() && RT[3].valid())
					{
						// Array: {Pitch, Yaw, Roll}
						Rot.Pitch = RT[1].get<double>(); Rot.Yaw = RT[2].get<double>(); Rot.Roll = RT[3].get<double>();
						bHasRot = true;
					}
				}
				sol::optional<sol::table> ScaleOpt = P["scale"];
				if (ScaleOpt.has_value())
				{
					sol::table ST = ScaleOpt.value();
					if (ST["x"].valid())
					{
						// Named fields: {x=N, y=N, z=N}
						Scale.X = ST.get_or("x", 1.0); Scale.Y = ST.get_or("y", 1.0); Scale.Z = ST.get_or("z", 1.0);
						bHasScale = true;
					}
					else if (ST[1].valid() && ST[2].valid() && ST[3].valid())
					{
						// Array: {X, Y, Z}
						Scale.X = ST[1].get<double>(); Scale.Y = ST[2].get<double>(); Scale.Z = ST[3].get<double>();
						bHasScale = true;
					}
				}

				if (!bHasLoc && !bHasRot && !bHasScale)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] set_transforms -> '%s' at %.2fs: need location, rotation, or scale"), *FBinding, Time));
					continue;
				}

				FGuid BindingGuid = FindBindingByName(MovieScene, FBinding);
				if (!BindingGuid.IsValid())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] set_transforms -> binding '%s' not found"), *FBinding));
					continue;
				}
				const FMovieSceneBinding* Binding = MovieScene->FindBinding(BindingGuid);
				if (!Binding)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] set_transforms -> no tracks for '%s'"), *FBinding));
					continue;
				}

				UMovieScene3DTransformTrack* XTrack = nullptr;
				for (UMovieSceneTrack* Track : Binding->GetTracks())
				{
					XTrack = Cast<UMovieScene3DTransformTrack>(Track);
					if (XTrack) break;
				}
				if (!XTrack)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] set_transforms -> '%s' has no 3DTransform track"), *FBinding));
					continue;
				}

				const TArray<UMovieSceneSection*>& Secs = XTrack->GetAllSections();
				if (Secs.Num() == 0)
				{
					Session.Log(TEXT("[FAIL] set_transforms -> transform track has no sections"));
					continue;
				}

				UMovieScene3DTransformSection* XSec = Cast<UMovieScene3DTransformSection>(Secs[0]);
				if (!XSec)
				{
					Session.Log(TEXT("[FAIL] set_transforms -> could not get transform section"));
					continue;
				}

				XSec->Modify();
				FMovieSceneChannelProxy& CP = XSec->GetChannelProxy();
				TArrayView<FMovieSceneDoubleChannel*> DCs = CP.GetChannels<FMovieSceneDoubleChannel>();
				if (DCs.Num() < 9)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] set_transforms -> section has %d channels (need 9)"), DCs.Num()));
					continue;
				}

				FFrameNumber Frame = SecondsToFrame(MovieScene, Time);
				int32 Keys = 0;

				auto AddKey = [&](int32 Idx, double Val)
				{
					FMovieSceneDoubleChannel* Ch = DCs[Idx];
					if (!Ch) return;
					if (FInterp == TEXT("linear")) Ch->AddLinearKey(Frame, Val);
					else if (FInterp == TEXT("constant")) Ch->AddConstantKey(Frame, Val);
					else Ch->AddCubicKey(Frame, Val, RCTM_Auto);
					Keys++;
				};

				if (bHasLoc) { AddKey(0, Loc.X); AddKey(1, Loc.Y); AddKey(2, Loc.Z); }
				// Rotation channels: [3]=X(Roll), [4]=Y(Pitch), [5]=Z(Yaw) — matches UE Rotation[3] array order
				if (bHasRot) { AddKey(3, Rot.Roll); AddKey(4, Rot.Pitch); AddKey(5, Rot.Yaw); }
				if (bHasScale) { AddKey(6, Scale.X); AddKey(7, Scale.Y); AddKey(8, Scale.Z); }

				TotalKeys += Keys;
				SuccessCount++;
			}

			if (SuccessCount == 0)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_transforms -> no entries applied (0/%d). Fix the per-entry errors above and retry; wrote 0 keys."), Entries.Num()));
				return sol::lua_nil;
			}

			Sequence->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] set_transforms -> %d entries, %d keys"), SuccessCount, TotalKeys));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// analyze_camera_cuts()
		// ================================================================
		AssetObj.set_function("analyze_camera_cuts", [Sequence, MovieScene, &Session](sol::table /*self*/,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			// Build GUID->name map
			TMap<FGuid, FString> GuidToName;
			for (int32 i = 0; i < MovieScene->GetPossessableCount(); ++i)
			{
				const FMovieScenePossessable& P = MovieScene->GetPossessable(i);
				GuidToName.Add(P.GetGuid(), P.GetName());
			}
			for (int32 i = 0; i < MovieScene->GetSpawnableCount(); ++i)
			{
				const FMovieSceneSpawnable& Sp = MovieScene->GetSpawnable(i);
				GuidToName.Add(Sp.GetGuid(), Sp.GetName());
			}

			UMovieSceneCameraCutTrack* CCTrack = Cast<UMovieSceneCameraCutTrack>(MovieScene->GetCameraCutTrack());
			if (!CCTrack)
			{
				for (UMovieSceneTrack* T : MovieScene->GetTracks())
				{
					CCTrack = Cast<UMovieSceneCameraCutTrack>(T);
					if (CCTrack) break;
				}
			}

			sol::table Result = Lua.create_table();

			if (!CCTrack)
			{
				Result["shots"] = 0;
				Result["issues"] = 0;
				Result["message"] = "No CameraCut track found";
				Session.Log(TEXT("[OK] analyze_camera_cuts() -> no CameraCut track"));
				return Result;
			}

			TArray<UMovieSceneCameraCutSection*> Sections;
			for (UMovieSceneSection* Sec : CCTrack->GetAllSections())
			{
				if (UMovieSceneCameraCutSection* CCS = Cast<UMovieSceneCameraCutSection>(Sec))
					Sections.Add(CCS);
			}

			Sections.Sort([](const UMovieSceneCameraCutSection& A, const UMovieSceneCameraCutSection& B)
			{
				int32 SA = A.GetRange().HasLowerBound() ? A.GetRange().GetLowerBoundValue().Value : MIN_int32;
				int32 SB = B.GetRange().HasLowerBound() ? B.GetRange().GetLowerBoundValue().Value : MIN_int32;
				return SA < SB;
			});

			const double TickDecimal = MovieScene->GetTickResolution().AsDecimal();
			int32 IssueCount = 0;
			int32 BoundedCount = 0;
			double TotalDuration = 0.0;
			TSet<FGuid> UniqueGuid;

			sol::table ShotsTable = Lua.create_table();
			sol::table IssuesTable = Lua.create_table();
			sol::table ReviewTable = Lua.create_table();
			int32 IssueIdx = 1;
			int32 ReviewIdx = 1;

			bool bPrevEnd = false;
			FFrameNumber PrevEndFrame(0);
			FGuid PrevCamGuid;
			bool bPrevCam = false;

			for (int32 si = 0; si < Sections.Num(); ++si)
			{
				UMovieSceneCameraCutSection* Sec = Sections[si];
				if (!Sec) continue;

				const TRange<FFrameNumber> Range = Sec->GetRange();
				const bool bHasS = Range.HasLowerBound(), bHasE = Range.HasUpperBound();
				const FFrameNumber SF = bHasS ? Range.GetLowerBoundValue() : FFrameNumber(0);
				const FFrameNumber EF = bHasE ? Range.GetUpperBoundValue() : FFrameNumber(0);

				sol::table ShotEntry = Lua.create_table();
				ShotEntry["index"] = si;
				if (bHasS) ShotEntry["start"] = FrameToSeconds(MovieScene, SF);
				if (bHasE) ShotEntry["end"] = FrameToSeconds(MovieScene, EF);

				double Duration = 0.0;
				if (bHasS && bHasE)
				{
					Duration = FMath::Max(0.0, FrameToSeconds(MovieScene, EF) - FrameToSeconds(MovieScene, SF));
					ShotEntry["duration"] = Duration;
					TotalDuration += Duration;
					BoundedCount++;
				}

				const FMovieSceneObjectBindingID& CID = Sec->GetCameraBindingID();
				FGuid CamGuid = CID.GetGuid();
				if (CamGuid.IsValid())
				{
					const FString* CN = GuidToName.Find(CamGuid);
					ShotEntry["camera"] = TCHAR_TO_UTF8(CN ? **CN : *CamGuid.ToString());
					UniqueGuid.Add(CamGuid);
				}
				else
				{
					IssuesTable[IssueIdx++] = FString::Printf(TEXT("Shot %d: missing camera_binding"), si + 1);
					IssueCount++;
				}

				// Duration issues
				if (bHasS && bHasE)
				{
					if (Duration < 0.75) { IssuesTable[IssueIdx++] = TCHAR_TO_UTF8(*FString::Printf(TEXT("Shot %d: very short (%.2fs < 0.75s)"), si + 1, Duration)); IssueCount++; }
					else if (Duration > 8.0) { IssuesTable[IssueIdx++] = TCHAR_TO_UTF8(*FString::Printf(TEXT("Shot %d: very long (%.2fs > 8s)"), si + 1, Duration)); IssueCount++; }

					double StartSec = FrameToSeconds(MovieScene, SF);
					double EndSec = FrameToSeconds(MovieScene, EF);
					double EdgePad = FMath::Min(0.30, Duration * 0.2);
					ReviewTable[ReviewIdx++] = TCHAR_TO_UTF8(*FString::Printf(TEXT("Shot %d start: %.2f"), si + 1, StartSec + EdgePad));
					ReviewTable[ReviewIdx++] = TCHAR_TO_UTF8(*FString::Printf(TEXT("Shot %d middle: %.2f"), si + 1, (StartSec + EndSec) * 0.5));
					if (EndSec - EdgePad > StartSec + EdgePad + 0.05)
						ReviewTable[ReviewIdx++] = TCHAR_TO_UTF8(*FString::Printf(TEXT("Shot %d end: %.2f"), si + 1, EndSec - EdgePad));
				}

				// Gap/overlap
				if (bPrevEnd && bHasS)
				{
					int32 Delta = SF.Value - PrevEndFrame.Value;
					if (Delta > 0 && TickDecimal > 0.0)
					{
						IssuesTable[IssueIdx++] = TCHAR_TO_UTF8(*FString::Printf(TEXT("Shot %d: gap from previous (%.2fs)"), si + 1, Delta / TickDecimal));
						IssueCount++;
					}
					else if (Delta < 0 && TickDecimal > 0.0)
					{
						IssuesTable[IssueIdx++] = TCHAR_TO_UTF8(*FString::Printf(TEXT("Shot %d: overlap with previous (%.2fs)"), si + 1, -Delta / TickDecimal));
						IssueCount++;
					}
				}

				// Same camera consecutive
				if (bPrevCam && CamGuid.IsValid() && CamGuid == PrevCamGuid)
				{
					IssuesTable[IssueIdx++] = TCHAR_TO_UTF8(*FString::Printf(TEXT("Shot %d: same camera as previous"), si + 1));
					IssueCount++;
				}

				bPrevEnd = bHasE; PrevEndFrame = EF;
				bPrevCam = CamGuid.IsValid(); PrevCamGuid = CamGuid;

				ShotsTable[si + 1] = ShotEntry;
			}

			Result["shots"] = ShotsTable;
			Result["shot_count"] = Sections.Num();
			Result["issues"] = IssuesTable;
			Result["issue_count"] = IssueCount;
			Result["unique_cameras"] = UniqueGuid.Num();
			Result["average_duration"] = BoundedCount > 0 ? TotalDuration / BoundedCount : 0.0;
			Result["review_timestamps"] = ReviewTable;

			Session.Log(FString::Printf(TEXT("[OK] analyze_camera_cuts() -> %d shots, %d issues"), Sections.Num(), IssueCount));
			return Result;
		});

		// ================================================================
		// execute_shot_plan({shots=[...], replace_camera_cuts?, default_target_actor?})
		// ================================================================
		AssetObj.set_function("execute_shot_plan", [Sequence, MovieScene, &Session](sol::table Self,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			sol::optional<sol::table> ShotsOpt = Params["shots"];
			if (!ShotsOpt.has_value())
			{
				Session.Log(TEXT("[FAIL] execute_shot_plan -> shots array required"));
				return sol::lua_nil;
			}
			sol::table ShotsArr = ShotsOpt.value();
			bool bReplace = Params.get_or("replace_camera_cuts", false);
			std::string DefaultTargetStr = Params.get_or("default_target_actor", std::string());
			FString DefaultTarget = NeoLuaStr::ToFString(DefaultTargetStr);

			// Parse shots
			struct FShotDef
			{
				FString Name;
				double StartTime = 0.0, EndTime = 0.0;
				FString CameraBinding, CameraActor, TargetActor;
				FString ShotSize = TEXT("medium"), Side = TEXT("front"), Angle = TEXT("eye_level"), Movement = TEXT("static");
				double DistanceScale = 1.0, HeightOffset = 0.0;
			};

			TArray<FShotDef> Shots;
			TArray<FString> ParseErrors;
			int32 ShotNum = 0;

			for (auto& kv : ShotsArr)
			{
				ShotNum++;
				if (!kv.second.is<sol::table>()) { ParseErrors.Add(FString::Printf(TEXT("Shot %d: not a table"), ShotNum)); continue; }
				sol::table ST = kv.second.as<sol::table>();

				FShotDef D;
				std::string NameStr = ST.get_or("name", std::string());
				D.Name = NameStr.empty() ? FString::Printf(TEXT("Shot %d"), ShotNum) : NeoLuaStr::ToFString(NameStr);

				sol::optional<double> SOpt = ST["start_time"];
				sol::optional<double> EOpt = ST["end_time"];
				if (!SOpt.has_value() || !EOpt.has_value()) { ParseErrors.Add(FString::Printf(TEXT("%s: missing start_time/end_time"), *D.Name)); continue; }
				D.StartTime = SOpt.value(); D.EndTime = EOpt.value();
				if (D.EndTime <= D.StartTime) { ParseErrors.Add(FString::Printf(TEXT("%s: end_time must be > start_time"), *D.Name)); continue; }

				auto GetStr = [&](const char* Key) -> FString
				{
					std::string V = ST.get_or(Key, std::string());
					return V.empty() ? FString() : NeoLuaStr::ToFString(V);
				};

				D.CameraBinding = GetStr("camera_binding");
				D.CameraActor = GetStr("camera_actor");
				D.TargetActor = GetStr("target_actor");
				D.ShotSize = GetStr("shot_size"); if (D.ShotSize.IsEmpty()) D.ShotSize = TEXT("medium"); D.ShotSize = D.ShotSize.ToLower();
				D.Side = GetStr("side"); if (D.Side.IsEmpty()) D.Side = TEXT("front"); D.Side = D.Side.ToLower();
				D.Angle = GetStr("angle"); if (D.Angle.IsEmpty()) D.Angle = TEXT("eye_level"); D.Angle = D.Angle.ToLower();
				D.Movement = GetStr("movement"); if (D.Movement.IsEmpty()) D.Movement = TEXT("static"); D.Movement = D.Movement.ToLower();
				D.DistanceScale = FMath::Max(0.1, ST.get_or("distance_scale", 1.0));
				D.HeightOffset = ST.get_or("height_offset", 0.0);

				Shots.Add(MoveTemp(D));
			}

			if (Shots.Num() == 0)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] execute_shot_plan -> no valid shots. %s"), *FString::Join(ParseErrors, TEXT("; "))));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "SeqShotPlan", "Execute Shot Plan"));

			// Collect camera bindings for auto-rotate
			auto IsLikelyCamera = [](const UClass* C) -> bool
			{
				if (!C) return false;
				if (C->IsChildOf(ACameraActor::StaticClass())) return true;
				return C->GetName().Contains(TEXT("Camera"));
			};

			auto CollectCameraBindings = [&]() -> TArray<FString>
			{
				TArray<FString> R; TSet<FString> Seen;
				for (int32 i = 0; i < MovieScene->GetPossessableCount(); ++i)
				{
					const FMovieScenePossessable& P = MovieScene->GetPossessable(i);
					if (IsLikelyCamera(P.GetPossessedObjectClass()) && !Seen.Contains(P.GetName()))
					{ Seen.Add(P.GetName()); R.Add(P.GetName()); }
				}
				for (int32 i = 0; i < MovieScene->GetSpawnableCount(); ++i)
				{
					const FMovieSceneSpawnable& Sp = MovieScene->GetSpawnable(i);
					const UClass* SC = Sp.GetObjectTemplate() ? Sp.GetObjectTemplate()->GetClass() : nullptr;
					if (IsLikelyCamera(SC) && !Seen.Contains(Sp.GetName()))
					{ Seen.Add(Sp.GetName()); R.Add(Sp.GetName()); }
				}
				R.Sort(); return R;
			};

			TArray<FString> AutoCams = CollectCameraBindings();
			int32 AutoCamIdx = 0;

			if (bReplace && MovieScene->GetCameraCutTrack())
				MovieScene->RemoveCameraCutTrack();

			// Shot size distances
			auto GetShotDist = [](const FString& Size) -> double
			{
				if (Size == TEXT("wide")) return 1200.0;
				if (Size == TEXT("closeup")) return 260.0;
				if (Size == TEXT("extreme_closeup")) return 140.0;
				return 700.0;
			};

			auto GetSideDir = [](const FString& Side, const FVector& Fwd, const FVector& Rt) -> FVector
			{
				if (Side == TEXT("back")) return (-Fwd).GetSafeNormal();
				if (Side == TEXT("left")) return (-Rt).GetSafeNormal();
				if (Side == TEXT("right")) return Rt.GetSafeNormal();
				if (Side == TEXT("front_left")) return (Fwd - Rt).GetSafeNormal();
				if (Side == TEXT("front_right")) return (Fwd + Rt).GetSafeNormal();
				if (Side == TEXT("back_left")) return ((-Fwd) - Rt).GetSafeNormal();
				if (Side == TEXT("back_right")) return ((-Fwd) + Rt).GetSafeNormal();
				return Fwd.GetSafeNormal();
			};

			auto GetAngleOff = [](const FString& Angle, double& OutH, double& OutR)
			{
				OutH = 0.0; OutR = 0.0;
				if (Angle == TEXT("high_angle")) OutH = 140.0;
				else if (Angle == TEXT("low_angle")) OutH = -90.0;
				else if (Angle == TEXT("bird_eye")) OutH = 450.0;
				else if (Angle == TEXT("worm_eye")) OutH = -220.0;
				else if (Angle == TEXT("dutch_left")) OutR = -12.0;
				else if (Angle == TEXT("dutch_right")) OutR = 12.0;
			};

			int32 CutsApplied = 0, XformsApplied = 0;
			TArray<FString> Warnings;

			for (const FShotDef& Shot : Shots)
			{
				FString CamBindName = Shot.CameraBinding;

				// Resolve camera binding
				if (!CamBindName.IsEmpty())
				{
					if (!FindBindingByName(MovieScene, CamBindName).IsValid())
					{
						Warnings.Add(FString::Printf(TEXT("%s: camera_binding '%s' not found"), *Shot.Name, *CamBindName));
						continue;
					}
				}
				else if (!Shot.CameraActor.IsEmpty())
				{
					AActor* CamActor = FindActorByName(Shot.CameraActor);
					if (!CamActor)
					{
						Warnings.Add(FString::Printf(TEXT("%s: camera_actor '%s' not found"), *Shot.Name, *Shot.CameraActor));
						continue;
					}
					// Ensure binding
					FString Label = CamActor->GetActorLabel();
					FGuid Existing = FindBindingByName(MovieScene, Label);
					if (!Existing.IsValid()) Existing = FindBindingByName(MovieScene, CamActor->GetName());
					if (!Existing.IsValid())
					{
						FGuid NewG = MovieScene->AddPossessable(!Label.IsEmpty() ? Label : CamActor->GetName(), CamActor->GetClass());
						if (NewG.IsValid())
						{
							Sequence->BindPossessableObject(NewG, *CamActor, CamActor->GetWorld());
							CamBindName = !Label.IsEmpty() ? Label : CamActor->GetName();
							AutoCams = CollectCameraBindings();
						}
						else
						{
							Warnings.Add(FString::Printf(TEXT("%s: failed to bind camera actor"), *Shot.Name));
							continue;
						}
					}
					else
					{
						CamBindName = !Label.IsEmpty() ? Label : CamActor->GetName();
					}
				}
				else if (AutoCams.Num() > 0)
				{
					CamBindName = AutoCams[AutoCamIdx % AutoCams.Num()];
					AutoCamIdx++;
				}
				else
				{
					// Find first camera in level
					ACameraActor* Fallback = nullptr;
					if (GEditor)
					{
						UWorld* W = GEditor->GetEditorWorldContext().World();
						if (W)
						{
							TActorIterator<ACameraActor> It(W);
							if (It)
							{
								Fallback = *It;
							}
						}
					}
					if (!Fallback)
					{
						Warnings.Add(FString::Printf(TEXT("%s: no camera found"), *Shot.Name));
						continue;
					}
					FString Label = Fallback->GetActorLabel();
					FGuid NewG = MovieScene->AddPossessable(!Label.IsEmpty() ? Label : Fallback->GetName(), Fallback->GetClass());
					if (NewG.IsValid())
					{
						Sequence->BindPossessableObject(NewG, *Fallback, Fallback->GetWorld());
						CamBindName = !Label.IsEmpty() ? Label : Fallback->GetName();
						AutoCams = CollectCameraBindings();
					}
					else
					{
						Warnings.Add(FString::Printf(TEXT("%s: failed to bind fallback camera"), *Shot.Name));
						continue;
					}
				}

				// Add camera cut
				FGuid CamGuid = FindBindingByName(MovieScene, CamBindName);
				if (!CamGuid.IsValid())
				{
					Warnings.Add(FString::Printf(TEXT("%s: camera '%s' binding lost"), *Shot.Name, *CamBindName));
					continue;
				}

				UMovieSceneCameraCutTrack* CCTrack = Cast<UMovieSceneCameraCutTrack>(MovieScene->GetCameraCutTrack());
				if (!CCTrack)
					CCTrack = Cast<UMovieSceneCameraCutTrack>(MovieScene->AddCameraCutTrack(UMovieSceneCameraCutTrack::StaticClass()));
				if (!CCTrack)
				{
					Warnings.Add(FString::Printf(TEXT("%s: failed to get/create CameraCut track"), *Shot.Name));
					continue;
				}

				FFrameNumber StartFrame = SecondsToFrame(MovieScene, Shot.StartTime);
				FFrameNumber EndFrame = SecondsToFrame(MovieScene, Shot.EndTime);

				// Check for overlap with existing sections
				const TRange<FFrameNumber> NewRange(StartFrame, EndFrame);
				for (UMovieSceneSection* ExistingSec : CCTrack->GetAllSections())
				{
					if (ExistingSec && ExistingSec->GetRange().Overlaps(NewRange))
					{
						Warnings.Add(FString::Printf(TEXT("%s: overlaps existing camera cut at %.2f-%.2fs"),
							*Shot.Name, FrameToSeconds(MovieScene, ExistingSec->GetRange().GetLowerBoundValue()),
							FrameToSeconds(MovieScene, ExistingSec->GetRange().GetUpperBoundValue())));
						break;
					}
				}

				UE::MovieScene::FRelativeObjectBindingID RelID(CamGuid);
				FMovieSceneObjectBindingID CamBindID(RelID);
				UMovieSceneCameraCutSection* CCS = CCTrack->AddNewCameraCut(CamBindID, StartFrame);
				if (!CCS)
				{
					Warnings.Add(FString::Printf(TEXT("%s: failed to add CameraCut section"), *Shot.Name));
					continue;
				}
				CCS->SetRange(TRange<FFrameNumber>(StartFrame, EndFrame));
				CCTrack->RearrangeAllSections();
				CutsApplied++;

				// Compute camera transforms if target actor specified
				FString TargetName = Shot.TargetActor.IsEmpty() ? DefaultTarget : Shot.TargetActor;
				if (TargetName.IsEmpty())
				{
					Warnings.Add(FString::Printf(TEXT("%s: no target_actor; only camera cut applied"), *Shot.Name));
					continue;
				}

				AActor* TargetActor = FindActorByName(TargetName);
				if (!TargetActor)
				{
					Warnings.Add(FString::Printf(TEXT("%s: target '%s' not found; only camera cut applied"), *Shot.Name, *TargetName));
					continue;
				}

				// Ensure transform track exists on camera binding
				const FMovieSceneBinding* CamBinding = MovieScene->FindBinding(CamGuid);
				UMovieScene3DTransformTrack* XTrack = nullptr;
				if (CamBinding)
				{
					for (UMovieSceneTrack* T : CamBinding->GetTracks())
					{
						XTrack = Cast<UMovieScene3DTransformTrack>(T);
						if (XTrack) break;
					}
				}
				if (!XTrack)
				{
					XTrack = Cast<UMovieScene3DTransformTrack>(MovieScene->AddTrack(UMovieScene3DTransformTrack::StaticClass(), CamGuid));
					if (XTrack)
					{
						UMovieSceneSection* NewSec = XTrack->CreateNewSection();
						if (NewSec)
						{
							NewSec->SetRange(TRange<FFrameNumber>(StartFrame, EndFrame));
							XTrack->AddSection(*NewSec);
						}
					}
				}
				if (!XTrack)
				{
					Warnings.Add(FString::Printf(TEXT("%s: failed to create transform track"), *Shot.Name));
					continue;
				}

				// Extend section range if needed
				const TArray<UMovieSceneSection*>& XSecs = XTrack->GetAllSections();
				if (XSecs.Num() > 0 && XSecs[0])
				{
					UMovieSceneSection* XSec = XSecs[0];
					TRange<FFrameNumber> ER = XSec->GetRange();
					FFrameNumber NS = StartFrame, NE = EndFrame;
					if (ER.HasLowerBound()) NS = FMath::Min(NS, ER.GetLowerBoundValue());
					if (ER.HasUpperBound()) NE = FMath::Max(NE, ER.GetUpperBoundValue());
					if (NE <= NS) NE = NS + FFrameNumber(1);
					XSec->SetRange(TRange<FFrameNumber>(NS, NE));
				}

				UMovieScene3DTransformSection* XSec = XSecs.Num() > 0 ? Cast<UMovieScene3DTransformSection>(XSecs[0]) : nullptr;
				if (!XSec)
				{
					Warnings.Add(FString::Printf(TEXT("%s: no transform section"), *Shot.Name));
					continue;
				}

				XSec->Modify();
				FMovieSceneChannelProxy& CP = XSec->GetChannelProxy();
				TArrayView<FMovieSceneDoubleChannel*> DCs = CP.GetChannels<FMovieSceneDoubleChannel>();
				if (DCs.Num() < 9)
				{
					Warnings.Add(FString::Printf(TEXT("%s: transform section has %d channels (need 9)"), *Shot.Name, DCs.Num()));
					continue;
				}

				const FVector TgtLoc = TargetActor->GetActorLocation();
				const FVector TgtFwd = TargetActor->GetActorForwardVector().GetSafeNormal();
				const FVector TgtRt = TargetActor->GetActorRightVector().GetSafeNormal();
				const FVector SideDir = GetSideDir(Shot.Side, TgtFwd, TgtRt);

				double AngleH = 0.0, AngleR = 0.0;
				GetAngleOff(Shot.Angle, AngleH, AngleR);

				const double BaseDist = GetShotDist(Shot.ShotSize) * Shot.DistanceScale;
				const double HeightOff = AngleH + Shot.HeightOffset;
				FVector StartLoc = TgtLoc + (SideDir * BaseDist) + FVector(0, 0, HeightOff);

				FVector EndDir = SideDir;
				double EndDist = BaseDist;
				if (Shot.Movement == TEXT("push_in")) EndDist = BaseDist * 0.8;
				else if (Shot.Movement == TEXT("pull_out")) EndDist = BaseDist * 1.2;
				else if (Shot.Movement == TEXT("orbit_left")) EndDir = FRotator(0, -20, 0).RotateVector(SideDir).GetSafeNormal();
				else if (Shot.Movement == TEXT("orbit_right")) EndDir = FRotator(0, 20, 0).RotateVector(SideDir).GetSafeNormal();

				FVector EndLoc = TgtLoc + (EndDir * EndDist) + FVector(0, 0, HeightOff);
				FRotator StartRot = (TgtLoc - StartLoc).Rotation(); StartRot.Roll += AngleR;
				FRotator EndRot = (TgtLoc - EndLoc).Rotation(); EndRot.Roll += AngleR;

				auto SetXformKey = [&](FFrameNumber Frame, const FVector& Loc, const FRotator& Rot)
				{
					DCs[0]->AddCubicKey(Frame, Loc.X, RCTM_Auto);
					DCs[1]->AddCubicKey(Frame, Loc.Y, RCTM_Auto);
					DCs[2]->AddCubicKey(Frame, Loc.Z, RCTM_Auto);
					DCs[3]->AddCubicKey(Frame, Rot.Roll, RCTM_Auto);
					DCs[4]->AddCubicKey(Frame, Rot.Pitch, RCTM_Auto);
					DCs[5]->AddCubicKey(Frame, Rot.Yaw, RCTM_Auto);
				};

				SetXformKey(StartFrame, StartLoc, StartRot);
				SetXformKey(EndFrame, EndLoc, EndRot);
				XformsApplied++;
			}

			Sequence->Modify();
			Sequence->MarkPackageDirty();

			sol::table Result = Lua.create_table();
			Result["cuts_applied"] = CutsApplied;
			Result["transforms_applied"] = XformsApplied;
			Result["requested"] = Shots.Num();

			if (Warnings.Num() > 0)
			{
				sol::table WT = Lua.create_table();
				for (int32 wi = 0; wi < Warnings.Num(); ++wi)
					WT[wi + 1] = TCHAR_TO_UTF8(*Warnings[wi]);
				Result["warnings"] = WT;
			}
			if (ParseErrors.Num() > 0)
			{
				sol::table ET = Lua.create_table();
				for (int32 ei = 0; ei < ParseErrors.Num(); ++ei)
					ET[ei + 1] = TCHAR_TO_UTF8(*ParseErrors[ei]);
				Result["errors"] = ET;
			}

			Session.Log(FString::Printf(TEXT("[OK] execute_shot_plan -> %d cuts, %d transforms, %d warnings"),
				CutsApplied, XformsApplied, Warnings.Num()));
			return Result;
		});

		// ================================================================
		// find_marked_frame({label?, frame?}) -> index or nil
		// ================================================================
		// Wraps UMovieScene::FindMarkedFrameByLabel / FindMarkedFrameByFrameNumber.
		// Both are plain C++ (no UFUNCTION) so they cannot be reached via `invoke`.
		AssetObj.set_function("find_marked_frame", [MovieScene, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			sol::optional<std::string> LabelOpt = Params["label"];
			sol::optional<int> FrameOpt = Params["frame"];
			int32 Idx = -1;
			if (LabelOpt.has_value())
			{
				Idx = MovieScene->FindMarkedFrameByLabel(NeoLuaStr::ToFString(LabelOpt.value()));
			}
			else if (FrameOpt.has_value())
			{
				Idx = MovieScene->FindMarkedFrameByFrameNumber(FFrameNumber(FrameOpt.value()));
			}
			else
			{
				Session.Log(TEXT("[FAIL] find_marked_frame -> pass {label=..} or {frame=..}"));
				return sol::lua_nil;
			}
			if (Idx < 0)
			{
				Session.Log(TEXT("[OK] find_marked_frame -> (not found)"));
				return sol::lua_nil;
			}
			Session.Log(FString::Printf(TEXT("[OK] find_marked_frame -> index %d"), Idx));
			return sol::make_object(Lua, Idx);
		});

		// ================================================================
		// find_next_marked_frame({frame, forward?=true}) -> index or nil
		// ================================================================
		AssetObj.set_function("find_next_marked_frame", [MovieScene, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			sol::optional<int> FrameOpt = Params["frame"];
			if (!FrameOpt.has_value())
			{
				Session.Log(TEXT("[FAIL] find_next_marked_frame -> {frame=N, forward?=true} required"));
				return sol::lua_nil;
			}
			const bool bForward = Params.get_or("forward", true);
			const int32 Idx = MovieScene->FindNextMarkedFrame(FFrameNumber(FrameOpt.value()), bForward);
			if (Idx < 0)
			{
				Session.Log(TEXT("[OK] find_next_marked_frame -> (none)"));
				return sol::lua_nil;
			}
			Session.Log(FString::Printf(TEXT("[OK] find_next_marked_frame -> index %d"), Idx));
			return sol::make_object(Lua, Idx);
		});

		// ================================================================
		// set_playback_range(start, end)
		// ================================================================
		AssetObj.set_function("set_playback_range", [Sequence, MovieScene, &Session](sol::table /*self*/,
			double Start, double End, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (End <= Start)
			{
				Session.Log(TEXT("[FAIL] set_playback_range -> end must be > start"));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "SeqSetPBRange", "Set Playback Range"));
			FFrameNumber StartFrame = SecondsToFrame(MovieScene, Start);
			FFrameNumber EndFrame = SecondsToFrame(MovieScene, End);
			MovieScene->SetPlaybackRange(TRange<FFrameNumber>(StartFrame, EndFrame));
			Sequence->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] set_playback_range(%.2f, %.2f)"), Start, End));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// configure_section({binding?, track_type, track_index?, section_index?,
		//   ease_in?, ease_out?, start?, end?})
		// ================================================================
		AssetObj.set_function("configure_section", [Sequence, MovieScene, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			std::string TrackTypeStr = Params.get_or("track_type", std::string());
			if (TrackTypeStr.empty())
			{
				Session.Log(TEXT("[FAIL] configure_section -> track_type required"));
				return sol::lua_nil;
			}

			FString FTrackType = NeoLuaStr::ToFString(TrackTypeStr);
			UClass* TrackClass = ResolveTrackClass(FTrackType);
			if (!TrackClass)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] configure_section -> unknown track type '%s'. Call list(\"track_types\") for valid names."), *FTrackType));
				return sol::lua_nil;
			}

			std::string BindStr = Params.get_or("binding", std::string());
			int32 TrackIdx = Params.get_or("track_index", 0);
			int32 SectionIdx = Params.get_or("section_index", 0);

			// Find the track
			UMovieSceneTrack* TargetTrack = nullptr;
			if (BindStr.empty())
			{
				// Master track
				if (TrackClass->IsChildOf(UMovieSceneCameraCutTrack::StaticClass()))
				{
					TargetTrack = MovieScene->GetCameraCutTrack();
				}
				else
				{
					int32 MatchCount = 0;
					for (UMovieSceneTrack* Track : MovieScene->GetTracks())
					{
						if (Track && Track->IsA(TrackClass))
						{
							if (MatchCount == TrackIdx) { TargetTrack = Track; break; }
							MatchCount++;
						}
					}
				}
			}
			else
			{
				FString FBinding = NeoLuaStr::ToFString(BindStr);
				FGuid BindingGuid = FindBindingByName(MovieScene, FBinding);
				if (!BindingGuid.IsValid())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure_section -> binding '%s' not found"), *FBinding));
					return sol::lua_nil;
				}
				const FMovieSceneBinding* Binding = MovieScene->FindBinding(BindingGuid);
				if (Binding)
				{
					int32 MatchCount = 0;
					for (UMovieSceneTrack* Track : Binding->GetTracks())
					{
						if (Track && Track->IsA(TrackClass))
						{
							if (MatchCount == TrackIdx) { TargetTrack = Track; break; }
							MatchCount++;
						}
					}
				}
			}

			if (!TargetTrack)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] configure_section -> no %s track found"), *FTrackType));
				return sol::lua_nil;
			}

			const TArray<UMovieSceneSection*>& Sections = TargetTrack->GetAllSections();
			if (SectionIdx < 0 || SectionIdx >= Sections.Num() || !Sections[SectionIdx])
			{
				Session.Log(FString::Printf(TEXT("[FAIL] configure_section -> section index %d out of range (has %d)"), SectionIdx, Sections.Num()));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "SeqConfigSec", "Configure Section"));
			UMovieSceneSection* Section = Sections[SectionIdx];
			Section->Modify();
			int32 Changes = 0;

			// Easing
			sol::optional<double> EaseInOpt = Params.get<sol::optional<double>>("ease_in");
			if (EaseInOpt.has_value())
			{
				FFrameNumber EaseFrames = SecondsToFrame(MovieScene, EaseInOpt.value());
				Section->Easing.bManualEaseIn = true;
				Section->Easing.ManualEaseInDuration = EaseFrames.Value;
				Changes++;
			}

			sol::optional<double> EaseOutOpt = Params.get<sol::optional<double>>("ease_out");
			if (EaseOutOpt.has_value())
			{
				FFrameNumber EaseFrames = SecondsToFrame(MovieScene, EaseOutOpt.value());
				Section->Easing.bManualEaseOut = true;
				Section->Easing.ManualEaseOutDuration = EaseFrames.Value;
				Changes++;
			}

			// Range adjustment. Guard against open bounds — TRangeBound::GetValue check-fails
			// when the bound is Open, and migrated all-range sections (TRange::All()) do
			// have open bounds. If only one side is provided and the opposite side is open,
			// we fail with a helpful message instead of asserting.
			// Also validate NewEnd >= NewStart: UMovieSceneSection::SetRange silently no-ops
			// when the invariant fails (ensure check + conditional assign), so without this
			// guard the binding would report success while the range didn't change.
			sol::optional<double> StartOpt = Params.get<sol::optional<double>>("start");
			sol::optional<double> EndOpt = Params.get<sol::optional<double>>("end");
			if (StartOpt.has_value() || EndOpt.has_value())
			{
				TRange<FFrameNumber> Range = Section->GetRange();
				const bool bNeedLower = !StartOpt.has_value() && !Range.HasLowerBound();
				const bool bNeedUpper = !EndOpt.has_value() && !Range.HasUpperBound();
				if (bNeedLower || bNeedUpper)
				{
					Session.Log(TEXT("[FAIL] configure_section -> section has an open bound; provide both start and end to close it"));
					return sol::lua_nil;
				}
				FFrameNumber NewStart = StartOpt.has_value() ? SecondsToFrame(MovieScene, StartOpt.value()) : Range.GetLowerBoundValue();
				FFrameNumber NewEnd = EndOpt.has_value() ? SecondsToFrame(MovieScene, EndOpt.value()) : Range.GetUpperBoundValue();
				if (NewEnd < NewStart)
				{
					Session.Log(TEXT("[FAIL] configure_section -> end must be >= start (engine SetRange would silently reject)"));
					return sol::lua_nil;
				}
				Section->SetRange(TRange<FFrameNumber>(NewStart, NewEnd));
				Changes++;
			}

			// Row index
			sol::optional<int> RowIndexOpt = Params.get<sol::optional<int>>("row_index");
			if (RowIndexOpt.has_value())
			{
				Section->SetRowIndex(RowIndexOpt.value());
				Changes++;
			}

			// Overlap priority
			sol::optional<int> OverlapOpt = Params.get<sol::optional<int>>("overlap_priority");
			if (OverlapOpt.has_value())
			{
				Section->SetOverlapPriority(OverlapOpt.value());
				Changes++;
			}

			// Pre/Post roll
			sol::optional<int> PreRollOpt = Params.get<sol::optional<int>>("pre_roll_frames");
			if (PreRollOpt.has_value())
			{
				Section->SetPreRollFrames(PreRollOpt.value());
				Changes++;
			}
			sol::optional<int> PostRollOpt = Params.get<sol::optional<int>>("post_roll_frames");
			if (PostRollOpt.has_value())
			{
				Section->SetPostRollFrames(PostRollOpt.value());
				Changes++;
			}

			// Generic UMovieSceneSection controls (apply to every section type)
			// completion_mode — how the section behaves when done. Enum: keep_state |
			// restore_state | project_default. Also sets bCanEditCompletionMode=true so
			// Sequencer UI exposes the dropdown.
			std::string CompletionModeStr = Params.get_or<std::string>("completion_mode", "");
			if (!CompletionModeStr.empty())
			{
				FString CM = NeoLuaStr::ToFString(CompletionModeStr);
				EMovieSceneCompletionMode NewMode = EMovieSceneCompletionMode::KeepState;
				if (CM.Equals(TEXT("keep_state"), ESearchCase::IgnoreCase) || CM.Equals(TEXT("KeepState"), ESearchCase::IgnoreCase))
					NewMode = EMovieSceneCompletionMode::KeepState;
				else if (CM.Equals(TEXT("restore_state"), ESearchCase::IgnoreCase) || CM.Equals(TEXT("RestoreState"), ESearchCase::IgnoreCase))
					NewMode = EMovieSceneCompletionMode::RestoreState;
				else if (CM.Equals(TEXT("project_default"), ESearchCase::IgnoreCase) || CM.Equals(TEXT("ProjectDefault"), ESearchCase::IgnoreCase))
					NewMode = EMovieSceneCompletionMode::ProjectDefault;
				else
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure_section -> unknown completion_mode '%s'. Valid: keep_state, restore_state, project_default"), *CM));
					return sol::lua_nil;
				}
				Section->EvalOptions.EnableAndSetCompletionMode(NewMode);
				Changes++;
			}

			// ease_in_type / ease_out_type — class path that implements IMovieSceneEasingFunction.
			// Providing an empty string clears the curve. We verify that the resolved class
			// implements the interface before NewObject to avoid DO_CHECK asserts.
			auto ApplyEasingClass = [&](const char* Key, TScriptInterface<IMovieSceneEasingFunction>& Target, const TCHAR* Label) -> bool
			{
				sol::optional<std::string> Opt = Params.get<sol::optional<std::string>>(Key);
				if (!Opt.has_value()) return true; // not provided — leave alone, no-op
				FString ClassPath = NeoLuaStr::ToFString(Opt.value());
				if (ClassPath.IsEmpty())
				{
					Target = nullptr;
					Changes++;
					return true;
				}
				FString Canon = ClassPath;
				UObject* Obj = NeoLuaType::ResolveType(Canon);
				UClass* CurveClass = Cast<UClass>(Obj);
				if (!CurveClass)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure_section -> %s '%s' did not resolve to a UClass."), Label, *ClassPath));
					return false;
				}
				if (!CurveClass->ImplementsInterface(UMovieSceneEasingFunction::StaticClass()))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure_section -> %s '%s' must implement IMovieSceneEasingFunction."), Label, *ClassPath));
					return false;
				}
				if (CurveClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure_section -> %s '%s' is abstract or deprecated."), Label, *ClassPath));
					return false;
				}
				// TScriptInterface's operator= handles both SetObject and the interface
				// vtable lookup internally (see ScriptInterface.h). Assigning via = is
				// the engine-idiomatic path and works for native + BP implementations.
				UObject* NewCurve = NewObject<UObject>(Section, CurveClass);
				Target = NewCurve;
				Changes++;
				return true;
			};
			if (!ApplyEasingClass("ease_in_type", Section->Easing.EaseIn, TEXT("ease_in_type"))) return sol::lua_nil;
			if (!ApplyEasingClass("ease_out_type", Section->Easing.EaseOut, TEXT("ease_out_type"))) return sol::lua_nil;

			// blend_type — EMovieSceneBlendType. Supported values depend on the section's
			// track; we validate against GetSupportedBlendTypes before calling SetBlendType
			// so unsupported values return a clean fail instead of silent engine rejection.
			std::string BlendStr = Params.get_or<std::string>("blend_type", "");
			if (!BlendStr.empty())
			{
				FString BS = NeoLuaStr::ToFString(BlendStr);
				EMovieSceneBlendType NewBlend = EMovieSceneBlendType::Invalid;
				if (BS.Equals(TEXT("absolute"), ESearchCase::IgnoreCase))               NewBlend = EMovieSceneBlendType::Absolute;
				else if (BS.Equals(TEXT("additive"), ESearchCase::IgnoreCase))          NewBlend = EMovieSceneBlendType::Additive;
				else if (BS.Equals(TEXT("relative"), ESearchCase::IgnoreCase))          NewBlend = EMovieSceneBlendType::Relative;
				else if (BS.Equals(TEXT("additive_from_base"), ESearchCase::IgnoreCase)
					  || BS.Equals(TEXT("AdditiveFromBase"), ESearchCase::IgnoreCase))  NewBlend = EMovieSceneBlendType::AdditiveFromBase;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
				else if (BS.Equals(TEXT("override"), ESearchCase::IgnoreCase))          NewBlend = EMovieSceneBlendType::Override;
#endif
				else
				{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
					Session.Log(FString::Printf(TEXT("[FAIL] configure_section -> unknown blend_type '%s'. Valid: absolute, additive, relative, additive_from_base, override"), *BS));
#else
					Session.Log(FString::Printf(TEXT("[FAIL] configure_section -> unknown blend_type '%s'. Valid: absolute, additive, relative, additive_from_base (override requires UE 5.7+)"), *BS));
#endif
					return sol::lua_nil;
				}
				FMovieSceneBlendTypeField Supported = Section->GetSupportedBlendTypes();
				if (!Supported.Contains(NewBlend))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure_section -> blend_type '%s' is not supported by %s. This track's section type only accepts a subset."), *BS, *FTrackType));
					return sol::lua_nil;
				}
				Section->SetBlendType(NewBlend);
				Changes++;
			}

			sol::optional<bool> ActiveOpt = Params.get<sol::optional<bool>>("active");
			if (ActiveOpt.has_value())
			{
				Section->SetIsActive(ActiveOpt.value());
				Changes++;
			}
			sol::optional<bool> LockedOpt = Params.get<sol::optional<bool>>("locked");
			if (LockedOpt.has_value())
			{
				Section->SetIsLocked(LockedOpt.value());
				Changes++;
			}
			sol::optional<sol::table> TintOpt = Params.get<sol::optional<sol::table>>("color_tint");
			if (TintOpt.has_value())
			{
				Section->SetColorTint(ParseColorFromLua(TintOpt.value(), Section->GetColorTint()));
				Changes++;
			}

			// Fade section properties
			if (UMovieSceneFadeSection* FadeSec = Cast<UMovieSceneFadeSection>(Section))
			{
				sol::optional<sol::table> ColorOpt = Params.get<sol::optional<sol::table>>("color");
				if (ColorOpt.has_value())
				{
					sol::table CT = ColorOpt.value();
					FadeSec->FadeColor.R = CT.get_or("r", CT.get_or(1, FadeSec->FadeColor.R));
					FadeSec->FadeColor.G = CT.get_or("g", CT.get_or(2, FadeSec->FadeColor.G));
					FadeSec->FadeColor.B = CT.get_or("b", CT.get_or(3, FadeSec->FadeColor.B));
					FadeSec->FadeColor.A = CT.get_or("a", CT.get_or(4, FadeSec->FadeColor.A));
					Changes++;
				}
				sol::optional<bool> FadeAudioOpt = Params.get<sol::optional<bool>>("fade_audio");
				if (FadeAudioOpt.has_value())
				{
					FadeSec->bFadeAudio = FadeAudioOpt.value() ? 1 : 0;
					Changes++;
				}
			}

			// Audio section properties
			if (UMovieSceneAudioSection* AudioSec = Cast<UMovieSceneAudioSection>(Section))
			{
				sol::optional<bool> LoopOpt = Params.get<sol::optional<bool>>("looping");
				if (LoopOpt.has_value())
				{
					AudioSec->SetLooping(LoopOpt.value());
					Changes++;
				}
				sol::optional<double> StartOffsetOpt = Params.get<sol::optional<double>>("start_offset");
				if (StartOffsetOpt.has_value())
				{
					AudioSec->SetStartOffset(SecondsToFrame(MovieScene, StartOffsetOpt.value()));
					Changes++;
				}
				sol::optional<bool> SuppressSubsOpt = Params.get<sol::optional<bool>>("suppress_subtitles");
				if (SuppressSubsOpt.has_value())
				{
					AudioSec->SetSuppressSubtitles(SuppressSubsOpt.value());
					Changes++;
				}
				sol::optional<bool> OverrideAttenOpt = Params.get<sol::optional<bool>>("override_attenuation");
				if (OverrideAttenOpt.has_value())
				{
					AudioSec->SetOverrideAttenuation(OverrideAttenOpt.value());
					Changes++;
				}
				std::string SoundStr = Params.get_or("sound_asset", std::string());
				if (!SoundStr.empty())
				{
					USoundBase* Sound = NeoLuaAsset::Resolve<USoundBase>(NeoLuaStr::ToFString(SoundStr));
					if (Sound) { AudioSec->SetSound(Sound); Changes++; }
				}
			}

			// SkeletalAnimation section properties
			if (UMovieSceneSkeletalAnimationSection* AnimSec = Cast<UMovieSceneSkeletalAnimationSection>(Section))
			{
				sol::optional<int> StartFrameOffsetOpt = Params.get<sol::optional<int>>("start_frame_offset");
				if (StartFrameOffsetOpt.has_value())
				{
					AnimSec->Params.StartFrameOffset = FFrameNumber(StartFrameOffsetOpt.value());
					Changes++;
				}
				sol::optional<int> EndFrameOffsetOpt = Params.get<sol::optional<int>>("end_frame_offset");
				if (EndFrameOffsetOpt.has_value())
				{
					AnimSec->Params.EndFrameOffset = FFrameNumber(EndFrameOffsetOpt.value());
					Changes++;
				}
				std::string SlotNameStr = Params.get_or("slot_name", std::string());
				if (!SlotNameStr.empty())
				{
					AnimSec->Params.SlotName = FName(NeoLuaStr::ToFString(SlotNameStr));
					Changes++;
				}
				sol::optional<double> PlayRateOpt = Params.get<sol::optional<double>>("play_rate");
				if (PlayRateOpt.has_value())
				{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
					AnimSec->Params.PlayRate.Set(PlayRateOpt.value());
#else
					AnimSec->Params.PlayRate = static_cast<float>(PlayRateOpt.value());
#endif
					Changes++;
				}
				sol::optional<bool> ReverseOpt = Params.get<sol::optional<bool>>("reverse");
				if (ReverseOpt.has_value())
				{
					AnimSec->Params.bReverse = ReverseOpt.value() ? 1 : 0;
					Changes++;
				}
				sol::optional<bool> SkipNotifOpt = Params.get<sol::optional<bool>>("skip_anim_notifiers");
				if (SkipNotifOpt.has_value())
				{
					AnimSec->Params.bSkipAnimNotifiers = SkipNotifOpt.value();
					Changes++;
				}
				sol::optional<bool> ForceCustomOpt = Params.get<sol::optional<bool>>("force_custom_mode");
				if (ForceCustomOpt.has_value())
				{
					AnimSec->Params.bForceCustomMode = ForceCustomOpt.value();
					Changes++;
				}
				sol::optional<bool> MatchPrevOpt = Params.get<sol::optional<bool>>("match_with_previous");
				if (MatchPrevOpt.has_value())
				{
					AnimSec->bMatchWithPrevious = MatchPrevOpt.value() ? 1 : 0;
					Changes++;
				}
				std::string AnimPathStr = Params.get_or("animation", std::string());
				if (!AnimPathStr.empty())
				{
					UAnimSequenceBase* Anim = NeoLuaAsset::Resolve<UAnimSequenceBase>(NeoLuaStr::ToFString(AnimPathStr));
					if (Anim)
					{
						AnimSec->Params.Animation = Anim;
						Changes++;
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure_section -> animation '%s' not found"), UTF8_TO_TCHAR(AnimPathStr.c_str())));
					}
				}
			}

			// CameraCut section properties
			if (UMovieSceneCameraCutSection* CCSec = Cast<UMovieSceneCameraCutSection>(Section))
			{
				std::string CamBindStr = Params.get_or("camera_binding", std::string());
				if (!CamBindStr.empty())
				{
					FString FCamBind = NeoLuaStr::ToFString(CamBindStr);
					// Try as binding name first, then as raw GUID
					FGuid CamGuid = FindBindingByName(MovieScene, FCamBind);
					if (!CamGuid.IsValid())
					{
						FGuid::Parse(FCamBind, CamGuid);
					}
					if (CamGuid.IsValid())
					{
						CCSec->SetCameraGuid(CamGuid);
						Changes++;
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure_section -> camera_binding '%s' not found"), *FCamBind));
					}
				}
				sol::optional<bool> LockPrevOpt = Params.get<sol::optional<bool>>("lock_previous_camera");
				if (LockPrevOpt.has_value())
				{
					CCSec->bLockPreviousCamera = LockPrevOpt.value();
					Changes++;
				}
			}

			// CinematicShot section properties
			if (UMovieSceneCinematicShotSection* CineSec = Cast<UMovieSceneCinematicShotSection>(Section))
			{
				std::string ShotNameStr = Params.get_or("shot_display_name", std::string());
				if (!ShotNameStr.empty())
				{
					CineSec->SetShotDisplayName(NeoLuaStr::ToFString(ShotNameStr));
					Changes++;
				}
			}

			// Sub/Subsequence section properties
			if (UMovieSceneSubSection* SubSec = Cast<UMovieSceneSubSection>(Section))
			{
				std::string SubSeqStr = Params.get_or("sub_sequence", std::string());
				if (!SubSeqStr.empty())
				{
					UMovieSceneSequence* SubSeq = NeoLuaAsset::Resolve<UMovieSceneSequence>(NeoLuaStr::ToFString(SubSeqStr));
					if (SubSeq)
					{
						SubSec->SetSequence(SubSeq);
						Changes++;
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure_section -> sub_sequence '%s' not found"), UTF8_TO_TCHAR(SubSeqStr.c_str())));
					}
				}
			}

			if (Changes == 0)
			{
				Session.Log(TEXT("[FAIL] configure_section -> no supported properties were provided. Valid keys: ease_in, ease_out, ease_in_type, ease_out_type, completion_mode, blend_type, start, end, row_index, overlap_priority, pre_roll_frames, post_roll_frames, active, locked, color_tint, plus section-type-specific keys (see help())"));
				return sol::lua_nil;
			}

			Sequence->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] configure_section(%s, section=%d) -> %d changes"),
				*FTrackType, SectionIdx, Changes));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// add_subsequence({sequence_path, start_time, end_time?, row_index?})
		// ================================================================
		AssetObj.set_function("add_subsequence", [Sequence, MovieScene, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			std::string SeqPathStr = Params.get_or("sequence_path", std::string());
			if (SeqPathStr.empty())
			{
				Session.Log(TEXT("[FAIL] add_subsequence -> sequence_path required. Pass a LevelSequence asset path like /Game/Cinematics/Shot01, or create one via create_asset(path, \"LevelSequence\")."));
				return sol::lua_nil;
			}

			ULevelSequence* SubSeq = NeoLuaAsset::Resolve<ULevelSequence>(NeoLuaStr::ToFString(SeqPathStr));
			if (!SubSeq)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_subsequence -> failed to load '%s'"),
					UTF8_TO_TCHAR(SeqPathStr.c_str())));
				return sol::lua_nil;
			}

			// Prevent circular references
			if (SubSeq == Sequence)
			{
				Session.Log(TEXT("[FAIL] add_subsequence -> cannot add a sequence as its own subsequence"));
				return sol::lua_nil;
			}

			double StartTime = Params.get_or("start_time", 0.0);
			int32 RowIndex = Params.get_or("row_index", 0);

			// Pre-validate end_time > start_time BEFORE any MovieScene mutation.
			// AddSequenceOnRow sets the section range directly from (StartTime, Duration)
			// without validation, so a bad duration produces a broken sub-section.
			const FFrameNumber PreStartFrame = SecondsToFrame(MovieScene, StartTime);
			sol::optional<double> PreEndOpt = Params.get<sol::optional<double>>("end_time");
			if (PreEndOpt.has_value())
			{
				const FFrameNumber PreEndFrame = SecondsToFrame(MovieScene, PreEndOpt.value());
				if (PreEndFrame <= PreStartFrame)
				{
					Session.Log(TEXT("[FAIL] add_subsequence -> end_time must be > start_time"));
					return sol::lua_nil;
				}
			}

			// Circular-reference check runs BEFORE AddTrack — otherwise a rejected
			// call would leave a stray SubTrack behind. Engine pattern from
			// SubTrackEditorBase.cpp: check the SUB-sequence's tracks for the parent,
			// not the parent's tracks for itself.
			UMovieScene* SubMS_Check = SubSeq->GetMovieScene();
			if (SubMS_Check)
			{
				UMovieSceneSubTrack* SubSubTrack = SubMS_Check->FindTrack<UMovieSceneSubTrack>();
				if (SubSubTrack && SubSubTrack->ContainsSequence(*Sequence, true))
				{
					Session.Log(TEXT("[FAIL] add_subsequence -> would create circular reference"));
					return sol::lua_nil;
				}
				UMovieSceneCinematicShotTrack* SubShotTrack = SubMS_Check->FindTrack<UMovieSceneCinematicShotTrack>();
				if (SubShotTrack && SubShotTrack->ContainsSequence(*Sequence, true))
				{
					Session.Log(TEXT("[FAIL] add_subsequence -> would create circular reference"));
					return sol::lua_nil;
				}
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "SeqAddSub", "Add Subsequence"));

			// Get or create sub track (only after all validation passes).
			UMovieSceneSubTrack* SubTrack = MovieScene->FindTrack<UMovieSceneSubTrack>();
			if (!SubTrack)
			{
				SubTrack = Cast<UMovieSceneSubTrack>(MovieScene->AddTrack(UMovieSceneSubTrack::StaticClass()));
			}
			if (!SubTrack)
			{
				Session.Log(TEXT("[FAIL] add_subsequence -> failed to create SubTrack"));
				return sol::lua_nil;
			}

			FFrameNumber StartFrame = SecondsToFrame(MovieScene, StartTime);

			// Calculate duration from sub-sequence's playback range
			UMovieScene* SubMS = SubSeq->GetMovieScene();
			int32 Duration = 0;
			if (SubMS)
			{
				const TRange<FFrameNumber> SubRange = SubMS->GetPlaybackRange();
				if (SubRange.HasLowerBound() && SubRange.HasUpperBound())
				{
					// Convert from sub-sequence tick resolution to this sequence's tick resolution
					double SubDurationSec = FrameToSeconds(SubMS, SubRange.GetUpperBoundValue()) -
						FrameToSeconds(SubMS, SubRange.GetLowerBoundValue());
					Duration = SecondsToFrame(MovieScene, SubDurationSec).Value;
				}
			}
			if (Duration <= 0)
			{
				Duration = SecondsToFrame(MovieScene, 5.0).Value; // Default 5s
			}

			sol::optional<double> EndOpt = Params.get<sol::optional<double>>("end_time");
			if (EndOpt.has_value())
			{
				Duration = (SecondsToFrame(MovieScene, EndOpt.value()) - StartFrame).Value;
			}

			UMovieSceneSubSection* SubSection = SubTrack->AddSequenceOnRow(SubSeq, StartFrame, Duration, RowIndex);
			if (!SubSection)
			{
				Session.Log(TEXT("[FAIL] add_subsequence -> failed to create sub-section"));
				return sol::lua_nil;
			}

			Sequence->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] add_subsequence(\"%s\", start=%.2f, row=%d)"),
				UTF8_TO_TCHAR(SeqPathStr.c_str()), StartTime, RowIndex));

			sol::table Result = Lua.create_table();
			Result["sequence"] = SeqPathStr;
			Result["start"] = StartTime;
			Result["duration_seconds"] = FrameToSeconds(MovieScene, FFrameNumber(Duration));
			return Result;
		});

		// ================================================================
		// metadata(verb, params?)
		// ================================================================
		// Wraps ULevelSequence::Find/FindOrAdd/Copy/RemoveMetaDataByClass.
		// These are BlueprintCallable so they're ALSO reachable via the global
		// invoke(seq_handle, "FindOrAddMetaDataByClass", {InClass=...}). Two paths
		// exist on purpose:
		//   • metadata(...)  — typed verb; class-path resolution, handle returns,
		//                      scoped transaction, editor-only reflection walk for list.
		//   • invoke(...)    — generic UFunction marshaller; returns native conversion
		//                      of the UObject* (typically a table with path/class).
		// Prefer metadata() for authoring workflows; invoke() for ad-hoc reflection.
		// If you edit one side, update the other so behaviour stays aligned.
		//
		// Verbs: "list" | "find" | "add" | "copy" | "remove"
		//   list()                                 → {path, class_name}[] for every metadata obj
		//   find({class_path})                      → handle {path, class} or nil
		//   add({class_path})                       → handle (FindOrAdd semantics)
		//   copy({source="/Game/.../MetaObj"})      → handle (DuplicateObject into this sequence)
		//   remove({class_path})                    → true
		AssetObj.set_function("metadata", [Sequence, &Session](sol::table /*self*/,
			const std::string& Verb, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FVerb = NeoLuaStr::ToFString(Verb);

#if WITH_EDITORONLY_DATA
			auto ResolveMetaClass = [&](const FString& Spec) -> UClass*
			{
				FString Canon = Spec;
				UObject* Obj = NeoLuaType::ResolveType(Canon);
				return Cast<UClass>(Obj);
			};

			auto MakeHandle = [&](UObject* MetaObj) -> sol::object
			{
				if (!MetaObj) return sol::lua_nil;
				sol::table H = Lua.create_table();
				H["path"] = TCHAR_TO_UTF8(*MetaObj->GetPathName());
				H["class"] = TCHAR_TO_UTF8(*MetaObj->GetClass()->GetPathName());
				H["class_name"] = TCHAR_TO_UTF8(*MetaObj->GetClass()->GetName());
				return H;
			};

			if (FVerb.Equals(TEXT("list"), ESearchCase::IgnoreCase))
			{
				// No UFUNCTION exposes the MetaDataObjects array directly, but we can
				// iterate via reflection on the ULevelSequence.
				sol::table R = Lua.create_table();
				int32 Idx = 1;
				FProperty* MetaProp = ULevelSequence::StaticClass()->FindPropertyByName(TEXT("MetaDataObjects"));
				if (FArrayProperty* ArrP = CastField<FArrayProperty>(MetaProp))
				{
					FScriptArrayHelper Helper(ArrP, ArrP->ContainerPtrToValuePtr<void>(Sequence));
					if (FObjectPropertyBase* InnerObjProp = CastField<FObjectPropertyBase>(ArrP->Inner))
					{
						for (int32 i = 0; i < Helper.Num(); ++i)
						{
							UObject* M = InnerObjProp->GetObjectPropertyValue(Helper.GetRawPtr(i));
							if (M) R[Idx++] = MakeHandle(M);
						}
					}
				}
				Session.Log(FString::Printf(TEXT("[OK] metadata(\"list\") -> %d entries"), Idx - 1));
				return R;
			}

			if (!Params.has_value())
			{
				Session.Log(TEXT("[FAIL] metadata(verb, params) -> params table required for find/add/copy/remove"));
				return sol::lua_nil;
			}
			sol::table P = Params.value();

			if (FVerb.Equals(TEXT("find"), ESearchCase::IgnoreCase))
			{
				FString ClassPath = NeoLuaStr::ToFStringOpt(P.get<sol::optional<std::string>>("class_path"));
				UClass* C = ResolveMetaClass(ClassPath);
				if (!C) { Session.Log(FString::Printf(TEXT("[FAIL] metadata(\"find\") -> class '%s' not found"), *ClassPath)); return sol::lua_nil; }
				UObject* M = Sequence->FindMetaDataByClass(C);
				Session.Log(FString::Printf(TEXT("[OK] metadata(\"find\", %s) -> %s"), *C->GetName(), M ? TEXT("found") : TEXT("(none)")));
				return MakeHandle(M);
			}

			if (FVerb.Equals(TEXT("add"), ESearchCase::IgnoreCase))
			{
				FString ClassPath = NeoLuaStr::ToFStringOpt(P.get<sol::optional<std::string>>("class_path"));
				UClass* C = ResolveMetaClass(ClassPath);
				if (!C) { Session.Log(FString::Printf(TEXT("[FAIL] metadata(\"add\") -> class '%s' not found"), *ClassPath)); return sol::lua_nil; }
				const FScopedTransaction Tx(NSLOCTEXT("AIK", "SeqAddMeta", "Add Level Sequence Metadata"));
				UObject* M = Sequence->FindOrAddMetaDataByClass(C);
				Sequence->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] metadata(\"add\", %s)"), *C->GetName()));
				return MakeHandle(M);
			}

			if (FVerb.Equals(TEXT("copy"), ESearchCase::IgnoreCase))
			{
				FString SourcePath = NeoLuaStr::ToFStringOpt(P.get<sol::optional<std::string>>("source"));
				UObject* Src = NeoLuaAsset::ResolveWithRegistry(SourcePath);
				if (!Src) { Session.Log(FString::Printf(TEXT("[FAIL] metadata(\"copy\") -> source '%s' not found"), *SourcePath)); return sol::lua_nil; }
				const FScopedTransaction Tx(NSLOCTEXT("AIK", "SeqCopyMeta", "Copy Level Sequence Metadata"));
				UObject* M = Sequence->CopyMetaData(Src);
				Sequence->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] metadata(\"copy\", %s)"), *SourcePath));
				return MakeHandle(M);
			}

			if (FVerb.Equals(TEXT("remove"), ESearchCase::IgnoreCase))
			{
				FString ClassPath = NeoLuaStr::ToFStringOpt(P.get<sol::optional<std::string>>("class_path"));
				UClass* C = ResolveMetaClass(ClassPath);
				if (!C) { Session.Log(FString::Printf(TEXT("[FAIL] metadata(\"remove\") -> class '%s' not found"), *ClassPath)); return sol::lua_nil; }
				const FScopedTransaction Tx(NSLOCTEXT("AIK", "SeqRemoveMeta", "Remove Level Sequence Metadata"));
				Sequence->RemoveMetaDataByClass(C);
				Sequence->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] metadata(\"remove\", %s)"), *C->GetName()));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] metadata(\"%s\") -> unknown verb. Valid: list, find, add, copy, remove"), *FVerb));
			return sol::lua_nil;
#else
			Session.Log(TEXT("[FAIL] metadata() -> requires editor (WITH_EDITORONLY_DATA=0 build)"));
			return sol::lua_nil;
#endif
		});

		// ================================================================
		// get_director_blueprint() -> path_string | nil
		// ================================================================
		// Reads ULevelSequence::GetDirectorBlueprint(). Cannot be reached via global
		// asset:get — the `DirectorBlueprint` UPROPERTY has no CPF_Edit, so asset:set
		// would reject it; and even if it didn't, a raw property write would bypass
		// the OnCompiled delegate rewire and MarkAsChanged() that SetDirectorBlueprint
		// does (ULevelSequence.cpp:818-838).
		AssetObj.set_function("get_director_blueprint", [Sequence, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UBlueprint* BP = Sequence->GetDirectorBlueprint();
			if (!BP)
			{
				Session.Log(TEXT("[OK] get_director_blueprint() -> (none)"));
				return sol::lua_nil;
			}
			FString Path = BP->GetPathName();
			Session.Log(FString::Printf(TEXT("[OK] get_director_blueprint() -> '%s'"), *Path));
			return sol::make_object(Lua, std::string(TCHAR_TO_UTF8(*Path)));
		});

		// ================================================================
		// set_director_blueprint() — ensures the sequence owns a director BP
		// ================================================================
		// With no args: if the sequence has no director BP yet, creates one via the
		// registered FMovieSceneSequenceEditor (same path Sequencer UI uses).
		// Returns the director BP's path. The BP is always owned by the sequence —
		// we do not support pointing at an external BP because ULevelSequence's
		// contract (LevelSequence.h:115) is "must be contained within this object".
		AssetObj.set_function("set_director_blueprint", [Sequence, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

#if WITH_EDITOR
			FMovieSceneSequenceEditor* Editor = FMovieSceneSequenceEditor::Find(Sequence);
			if (!Editor)
			{
				Session.Log(TEXT("[FAIL] set_director_blueprint() -> no FMovieSceneSequenceEditor registered for this sequence type"));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "SeqSetDirectorBP", "Set Director Blueprint"));
			UBlueprint* BP = Editor->GetOrCreateDirectorBlueprint(Sequence);
			if (!BP)
			{
				Session.Log(TEXT("[FAIL] set_director_blueprint() -> GetOrCreateDirectorBlueprint returned null (sequence type may not support it)"));
				return sol::lua_nil;
			}

			Sequence->MarkPackageDirty();
			FString Path = BP->GetPathName();
			Session.Log(FString::Printf(TEXT("[OK] set_director_blueprint() -> '%s'"), *Path));

			sol::table R = Lua.create_table();
			R["path"] = TCHAR_TO_UTF8(*Path);
			return R;
#else
			Session.Log(TEXT("[FAIL] set_director_blueprint() -> requires editor (WITH_EDITOR=0 build)"));
			return sol::lua_nil;
#endif
		});

		// ================================================================
		// info()
		// ================================================================
		AssetObj.set_function("info", [Sequence, MovieScene, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			sol::table Result = Lua.create_table();

			Result["possessables"] = MovieScene->GetPossessableCount();
			Result["spawnables"] = MovieScene->GetSpawnableCount();

			int32 TrackCount = 0;
			for (int32 i = 0; i < MovieScene->GetPossessableCount(); ++i)
			{
				const FMovieSceneBinding* B = MovieScene->FindBinding(MovieScene->GetPossessable(i).GetGuid());
				if (B) TrackCount += B->GetTracks().Num();
			}
			for (int32 i = 0; i < MovieScene->GetSpawnableCount(); ++i)
			{
				const FMovieSceneBinding* B = MovieScene->FindBinding(MovieScene->GetSpawnable(i).GetGuid());
				if (B) TrackCount += B->GetTracks().Num();
			}
			TrackCount += MovieScene->GetTracks().Num();
			if (MovieScene->GetCameraCutTrack()) TrackCount++;

			Result["total_tracks"] = TrackCount;

			const FFrameRate DR = MovieScene->GetDisplayRate();
			Result["display_rate"] = DR.AsDecimal();

			const FFrameRate TR = MovieScene->GetTickResolution();
			Result["tick_resolution"] = TR.AsDecimal();

			Result["evaluation_type"] = MovieScene->GetEvaluationType() == EMovieSceneEvaluationType::FrameLocked
				? "frame_locked" : "with_sub_frames";

			{
				const EUpdateClockSource CS = MovieScene->GetClockSource();
				const char* CSStr = "tick";
				if (CS == EUpdateClockSource::Platform) CSStr = "platform";
				else if (CS == EUpdateClockSource::Audio) CSStr = "audio";
				else if (CS == EUpdateClockSource::RelativeTimecode) CSStr = "relative_timecode";
				else if (CS == EUpdateClockSource::Timecode) CSStr = "timecode";
				else if (CS == EUpdateClockSource::PlayEveryFrame) CSStr = "play_every_frame";
				else if (CS == EUpdateClockSource::Custom) CSStr = "custom";
				Result["clock_source"] = CSStr;
			}

			const TRange<FFrameNumber> PR = MovieScene->GetPlaybackRange();
			if (PR.HasLowerBound()) Result["playback_start"] = FrameToSeconds(MovieScene, PR.GetLowerBoundValue());
			if (PR.HasUpperBound()) Result["playback_end"] = FrameToSeconds(MovieScene, PR.GetUpperBoundValue());
			Result["playback_range_locked"] = MovieScene->IsPlaybackRangeLocked();
			{
				TRange<double> VR = MovieScene->GetEditorData().GetViewRange();
				sol::table VRT = Lua.create_table();
				VRT["start"] = VR.GetLowerBoundValue(); VRT["end"] = VR.GetUpperBoundValue();
				Result["view_range"] = VRT;
			}

			int32 CameraCuts = 0;
			UMovieSceneTrack* CCT = MovieScene->GetCameraCutTrack();
			if (CCT) CameraCuts = CCT->GetAllSections().Num();
			Result["camera_cuts"] = CameraCuts;

			Result["binding_tags"] = MovieScene->AllTaggedBindings().Num();

#if WITH_EDITORONLY_DATA
			Result["node_groups"] = MovieScene->GetNodeGroups().Num();
#endif

			// Section groups count via reflection
			{
				FProperty* SGProp = UMovieScene::StaticClass()->FindPropertyByName(TEXT("SectionGroups"));
				if (SGProp)
				{
					FArrayProperty* ArrayProp = CastField<FArrayProperty>(SGProp);
					if (ArrayProp)
					{
						FScriptArrayHelper ArrayHelper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(MovieScene));
						Result["section_groups"] = ArrayHelper.Num();
					}
				}
			}

			// Director blueprint (nil if none assigned)
			if (UBlueprint* DirBP = Sequence->GetDirectorBlueprint())
			{
				Result["director_blueprint"] = TCHAR_TO_UTF8(*DirBP->GetPathName());
			}

			Session.Log(FString::Printf(TEXT("[OK] info() -> %d bindings, %d tracks, %d camera cuts"),
				MovieScene->GetPossessableCount() + MovieScene->GetSpawnableCount(), TrackCount, CameraCuts));
			return Result;
		});

		// ================================================================
		// help()
		// ================================================================
		// help() is handled by OpenAsset's help() which reads _help_text
		// and includes generic methods (get/set/list_properties/save/etc.)
	});
}

REGISTER_LUA_BINDING(Sequencer, SequencerDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindSequencer(Lua, Session);
});
