#include "Lua/LuaBindingRegistry.h"
#include "Lua/LuaPropertyHelper.h"
#include "Lua/LuaCurveHelper.h"
#include "Lua/LuaAssetResolver.h"
#include "Lua/LuaStr.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimData/AttributeIdentifier.h"
#include "Animation/BuiltInAttributeTypes.h"
#include "Animation/AnimCurveTypes.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimTypes.h"
#include "Animation/AnimBoneCompressionSettings.h"
#include "Animation/AnimCurveCompressionSettings.h"
#include "Curves/RichCurve.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

static UClass* FindNotifyClassForAnimSeq(const FString& TypeName)
{
	if (TypeName.IsEmpty()) return nullptr;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		if ((*It)->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated)) continue;
		if (!(*It)->IsChildOf(UAnimNotify::StaticClass()) && !(*It)->IsChildOf(UAnimNotifyState::StaticClass())) continue;
		FString Name = (*It)->GetName();
		if (Name.Equals(TypeName, ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("AnimNotify_") + TypeName, ESearchCase::IgnoreCase) ||
			Name.Equals(TEXT("AnimNotifyState_") + TypeName, ESearchCase::IgnoreCase))
			return *It;
	}
	return nullptr;
}

static void InitializeAnimSequenceNotifyForEditor(FAnimNotifyEvent& Event, UAnimNotify* Notify)
{
	if (Notify)
	{
		Notify->OnAnimNotifyCreatedInEditor(Event);
	}
}

static void InitializeAnimSequenceNotifyForEditor(FAnimNotifyEvent& Event, UAnimNotifyState* NotifyState)
{
	if (NotifyState)
	{
		NotifyState->OnAnimNotifyCreatedInEditor(Event);
	}
}

// Set extra properties on a notify object from a Lua params table.
// Reserved keys (name, time, type, duration, track, branching_point, trigger_chance) are skipped.
static void SetNotifyPropertiesFromParams(UObject* NotifyObj, const sol::table& Params, FLuaSessionData& Session)
{
	if (!NotifyObj) return;

	static const TSet<FString> ReservedKeys = {
		TEXT("name"), TEXT("time"), TEXT("type"), TEXT("duration"),
		TEXT("track"), TEXT("branching_point"), TEXT("trigger_chance")
	};

	for (const auto& KV : Params)
	{
		if (!KV.first.is<std::string>()) continue;
		FString Key = NeoLuaStr::ToFString(KV.first.as<std::string>());
		if (ReservedKeys.Contains(Key.ToLower())) continue;
		if (!KV.second.is<std::string>()) continue;

		FString Value = NeoLuaStr::ToFString(KV.second.as<std::string>());

		// Find property on the notify object (case-insensitive)
		FProperty* Prop = nullptr;
		for (TFieldIterator<FProperty> PropIt(NotifyObj->GetClass()); PropIt; ++PropIt)
		{
			if (PropIt->GetName().Equals(Key, ESearchCase::IgnoreCase))
			{
				Prop = *PropIt;
				break;
			}
		}

		if (!Prop)
		{
			// Try Instanced sub-objects (e.g. RootMotionModifier on MotionWarping notify)
			for (TFieldIterator<FObjectProperty> ObjIt(NotifyObj->GetClass()); ObjIt; ++ObjIt)
			{
				if (ObjIt->GetName().Equals(Key, ESearchCase::IgnoreCase))
				{
					// For instanced object properties, the value is the class name to instantiate
					UClass* SubClass = nullptr;
					for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
					{
						if ((*ClassIt)->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated)) continue;
						if (!(*ClassIt)->IsChildOf(ObjIt->PropertyClass)) continue;
						FString ClassName = (*ClassIt)->GetName();
						if (ClassName.Equals(Value, ESearchCase::IgnoreCase) ||
							ClassName.Equals(TEXT("U") + Value, ESearchCase::IgnoreCase) ||
							ClassName.Contains(Value, ESearchCase::IgnoreCase))
						{
							SubClass = *ClassIt;
							break;
						}
					}

					if (SubClass)
					{
						UObject* SubObj = NewObject<UObject>(NotifyObj, SubClass, NAME_None, RF_Transactional);
						ObjIt->SetObjectPropertyValue(ObjIt->ContainerPtrToValuePtr<void>(NotifyObj), SubObj);
						Session.Log(FString::Printf(TEXT("[OK] notify property \"%s\" = instantiated %s"), *Key, *SubClass->GetName()));
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] notify property \"%s\" -> class \"%s\" not found"), *Key, *Value));
					}
					break;
				}
			}
			continue;
		}

		FString Error;
		if (NeoLuaProperty::SetPropertyValueFromStringSafe(
			Prop,
			Prop->ContainerPtrToValuePtr<void>(NotifyObj),
			NotifyObj,
			Value,
			Error))
		{
			Session.Log(FString::Printf(TEXT("[OK] notify property \"%s\" = \"%s\""), *Key, *Value));
		}
		else
		{
			Session.Log(FString::Printf(TEXT("[WARN] notify property \"%s\" -> %s"), *Key, *Error));
		}
	}
}

static void SetSyncMarkerTrackIndex(FAnimSyncMarker& Marker, int32 TrackIndex)
{
#if WITH_EDITORONLY_DATA
	Marker.TrackIndex = TrackIndex;
#else
	(void)Marker;
	(void)TrackIndex;
#endif
}

static int32 GetSyncMarkerTrackIndex(const FAnimSyncMarker& Marker)
{
#if WITH_EDITORONLY_DATA
	return Marker.TrackIndex;
#else
	(void)Marker;
	return 0;
#endif
}

static UScriptStruct* ResolveAttributeType(const FString& TypeStr)
{
	if (TypeStr.Equals(TEXT("float"), ESearchCase::IgnoreCase))
		return FFloatAnimationAttribute::StaticStruct();
	if (TypeStr.Equals(TEXT("int"), ESearchCase::IgnoreCase) || TypeStr.Equals(TEXT("integer"), ESearchCase::IgnoreCase))
		return FIntegerAnimationAttribute::StaticStruct();
	if (TypeStr.Equals(TEXT("string"), ESearchCase::IgnoreCase))
		return FStringAnimationAttribute::StaticStruct();
	if (TypeStr.Equals(TEXT("transform"), ESearchCase::IgnoreCase))
		return FTransformAnimationAttribute::StaticStruct();
	if (TypeStr.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
		return FVectorAnimationAttribute::StaticStruct();
	if (TypeStr.Equals(TEXT("quaternion"), ESearchCase::IgnoreCase) || TypeStr.Equals(TEXT("quat"), ESearchCase::IgnoreCase))
		return FQuaternionAnimationAttribute::StaticStruct();
	return nullptr;
}

static FString AttributeTypeToString(const UScriptStruct* Struct)
{
	if (!Struct) return TEXT("unknown");
	if (Struct == FFloatAnimationAttribute::StaticStruct()) return TEXT("float");
	if (Struct == FIntegerAnimationAttribute::StaticStruct()) return TEXT("int");
	if (Struct == FStringAnimationAttribute::StaticStruct()) return TEXT("string");
	if (Struct == FTransformAnimationAttribute::StaticStruct()) return TEXT("transform");
	if (Struct == FVectorAnimationAttribute::StaticStruct()) return TEXT("vector");
	if (Struct == FQuaternionAnimationAttribute::StaticStruct()) return TEXT("quaternion");
	return Struct->GetName();
}

static FVector LuaVectorFromTable(const sol::table& T, const FVector& Default)
{
	return FVector(
		T.get_or(1, Default.X),
		T.get_or(2, Default.Y),
		T.get_or(3, Default.Z));
}

static bool ParseTransformCurveKey(const sol::table& Key, float& OutTime, FTransform& OutTransform)
{
	sol::optional<double> Time = Key.get<sol::optional<double>>("time");
	if (!Time.has_value())
	{
		return false;
	}

	sol::optional<sol::table> LocationOpt = Key.get<sol::optional<sol::table>>("location");
	sol::optional<sol::table> RotationOpt = Key.get<sol::optional<sol::table>>("rotation");
	sol::optional<sol::table> ScaleOpt = Key.get<sol::optional<sol::table>>("scale");
	if (!LocationOpt.has_value() && !RotationOpt.has_value() && !ScaleOpt.has_value())
	{
		return false;
	}

	FVector Location = FVector::ZeroVector;
	if (LocationOpt.has_value())
	{
		Location = LuaVectorFromTable(LocationOpt.value(), Location);
	}

	FQuat Rotation = FQuat::Identity;
	if (RotationOpt.has_value())
	{
		FVector RotatorValues = LuaVectorFromTable(RotationOpt.value(), FVector::ZeroVector);
		Rotation = FRotator(RotatorValues.X, RotatorValues.Y, RotatorValues.Z).Quaternion();
	}

	FVector Scale = FVector::OneVector;
	if (ScaleOpt.has_value())
	{
		Scale = LuaVectorFromTable(ScaleOpt.value(), Scale);
	}

	OutTime = static_cast<float>(Time.value());
	OutTransform = FTransform(Rotation, Location, Scale);
	return true;
}

static TArray<FLuaFunctionDoc> AnimSequenceDocs = {};

static void BindAnimSequence(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_anim_sequence", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = NeoLuaStr::ToFString(PathStr);
		UAnimSequence* Seq = NeoLuaAsset::Resolve<UAnimSequence>(FPath);
		if (!Seq) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"Element types for add/remove/list/configure:\n"
			"  notify      — anim notify event at a time\n"
			"  sync_marker — authored sync marker\n"
			"  bone_track  — bone transform track\n"
			"  curve       — float or transform curve\n"
			"  attribute   — custom bone attribute\n"
			"\n"
			"add(type, params):\n"
			"  add(\"notify\", {name=\"FootStep\", time=0.5, type=\"AnimNotify_PlaySound\", track=0})\n"
			"  add(\"notify\", {name=\"Trail\", time=0.2, type=\"AnimNotifyState_Trail\", duration=0.8})\n"
			"  add(\"sync_marker\", {name=\"Foot\", time=0.3, track=0})\n"
			"  add(\"bone_track\", {bone=\"hand_r\"})\n"
			"  add(\"curve\", {name=\"Speed\", type=\"float\"})  — type: float or transform\n"
			"  add(\"curve\", {name=\"SpeedCopy\", source=\"Speed\"})  — duplicate existing curve\n"
			"  add(\"curve_key\", {curve=\"Speed\", time=0.5, value=1.0, interp=\"cubic\", tangent_mode=\"auto\"})\n"
			"  add(\"attribute\", {bone=\"hand_r\", name=\"ImpactWeight\", type=\"float\"})\n"
			"    — type: float, int, string, transform, vector, quaternion\n"
			"\n"
			"remove(type, id):\n"
			"  remove(\"notify\", \"FootStep\") or remove(\"notify\", 1)  — by name or 1-based index\n"
			"  remove(\"sync_marker\", \"Foot\") or remove(\"sync_marker\", 1)\n"
			"  remove(\"bone_track\", \"hand_r\") — by bone name\n"
			"  remove(\"curve\", \"Speed\") or remove(\"curve\", {name=\"Speed\", type=\"transform\"})\n"
			"  remove(\"curve_key\", {curve=\"Speed\", time=0.5})\n"
			"  remove(\"attribute\", {bone=\"hand_r\", name=\"ImpactWeight\"})\n"
			"  remove(\"all_attributes\") — remove all bone attributes\n"
			"  remove(\"bone_attributes\", \"hand_r\") — remove all attributes for bone\n"
			"\n"
			"list(type):\n"
			"  list(\"notifies\"), list(\"sync_markers\"), list(\"bone_tracks\"), list(\"curves\")\n"
			"  list(\"attributes\") — all custom bone attributes\n"
			"\n"
			"configure(type, id, params):\n"
			"  configure(\"notify\", 1, {time=0.3, duration=0.5, track=1, trigger_chance=0.8})\n"
			"  configure(\"notify\", 1, {filter_type=\"LOD\", filter_lod=2, dedicated_server=false})\n"
			"  configure(\"sync_marker\", 1, {name=\"NewName\", time=0.5, track=0})\n"
			"  configure(\"curve\", \"Speed\", {editable=true, disabled=false, metadata=false})\n"
			"  (for plain sequence properties — RateScale, bLoop, Interpolation, AdditiveAnimType,\n"
			"   RefPoseType/RefPoseSeq/RefFrameIndex, bEnableRootMotion, bForceRootLock,\n"
			"   RootMotionRootLock, bUseNormalizedRootMotionScale, RetargetSource,\n"
			"   BoneCompressionSettings, CurveCompressionSettings, CompressionErrorThresholdScale,\n"
			"   bAllowFrameStripping — use asset:set(\"PropertyName\", value); list_properties() to discover)\n"
			"\n"
			"Action methods:\n"
			"  set_bone_keys(bone, {positions={{x,y,z},...}, rotations={{p,y,r},...}, scales=...})\n"
			"  update_bone_keys(bone, start_frame, {positions=..., rotations=..., scales=...})\n"
			"  set_curve_keys(name, {{time=0, value=1, interp=\"cubic\"},...}, type?)\n"
			"  set_transform_curve_keys(name, {{time=0, location={x,y,z}, rotation={p,y,r}, scale={x,y,z}},...})\n"
			"  duplicate_curve(source_name, new_name, type?) — duplicate a curve\n"
			"  rename_curve(old_name, new_name, type?) — rename a curve\n"
			"  scale_curve(name, factor, origin?, type?) — scale values by factor around origin\n"
			"  set_curve_color(name, {r,g,b,a?}) — set editor display color for float curves only\n"
			"  set_frame_rate(fps) — set sampling frame rate\n"
			"  set_num_frames(count) — set total frame count\n"
			"  resize(new_frame_count, t0, t1) — resize with frame remapping\n"
			"  cleanup_bone_tracks() — remove bone tracks not in skeleton\n"
			"  info() — summary of length, frame rate, notifies, sync markers, curves, bone tracks\n";

		// ---- add(type, params) ----
		AssetObj.set_function("add", [Seq, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = NeoLuaStr::ToFString(Type);

			if (FType.Equals(TEXT("notify"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"notify\") -> params required: {name=.., time=..}")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string Name = P.get_or<std::string>("name", "");
				double Time = P.get_or("time", 0.0);
				if (Name.empty()) { Session.Log(TEXT("[FAIL] add(\"notify\") -> name required")); return sol::lua_nil; }

				FString FName = NeoLuaStr::ToFString(Name);
				float PlayLength = Seq->GetPlayLength();
				if (Time < 0.0 || Time > PlayLength)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"notify\") -> time %.2f out of range (0-%.2f)"), Time, PlayLength));
					return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "AddNotify", "Add Notify"));
				Seq->Modify();

				FAnimNotifyEvent NewEvent;
				NewEvent.NotifyName = ::FName(*FName);
				NewEvent.Link(Seq, static_cast<float>(Time));
				NewEvent.TriggerTimeOffset = GetTriggerTimeOffsetForType(Seq->CalculateOffsetForNotify(static_cast<float>(Time)));
				NewEvent.TrackIndex = static_cast<int32>(P.get_or("track", 0.0));

				std::string TypeStr = P.get_or<std::string>("type", "");
				if (!TypeStr.empty())
				{
					FString FTypeStr = NeoLuaStr::ToFString(TypeStr);
					UClass* NotifyClass = FindNotifyClassForAnimSeq(FTypeStr);
					if (NotifyClass)
					{
						if (NotifyClass->IsChildOf(UAnimNotifyState::StaticClass()))
						{
							UAnimNotifyState* NS = NewObject<UAnimNotifyState>(Seq, NotifyClass, NAME_None, RF_Transactional);
							InitializeAnimSequenceNotifyForEditor(NewEvent, NS);
							NewEvent.NotifyStateClass = NS;
							double Dur = P.get_or("duration", 0.5);
							NewEvent.Duration = static_cast<float>(Dur);
							NewEvent.EndLink.Link(Seq, static_cast<float>(Time + Dur));
						}
						else
						{
							NewEvent.Notify = NewObject<UAnimNotify>(Seq, NotifyClass, NAME_None, RF_Transactional);
							InitializeAnimSequenceNotifyForEditor(NewEvent, NewEvent.Notify);
						}
					}
				}

				// Set extra properties on the notify object from params table
				// Any key that isn't a reserved param (name, time, type, duration, track, etc.)
				// gets set as a UPROPERTY on the created notify via ImportText.
				// For instanced sub-objects (e.g. RootMotionModifier), pass the class name as value.
				UObject* CreatedNotify = NewEvent.NotifyStateClass
					? static_cast<UObject*>(NewEvent.NotifyStateClass)
					: static_cast<UObject*>(NewEvent.Notify);
				if (CreatedNotify)
				{
					SetNotifyPropertiesFromParams(CreatedNotify, P, Session);
				}

#if WITH_EDITORONLY_DATA
				NewEvent.Guid = FGuid::NewGuid();
#endif
				NewEvent.TriggerWeightThreshold = ZERO_ANIMWEIGHT_THRESH;

				if (P.get_or("branching_point", false))
					NewEvent.MontageTickType = EMontageNotifyTickType::BranchingPoint;

				Seq->Notifies.Add(NewEvent);
				Seq->RefreshCacheData();
				Seq->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"notify\", name=\"%s\", time=%.2f)"), *FName, Time));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("bone_track"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"bone_track\") -> {bone=..} required")); return sol::lua_nil; }
				std::string BoneName = Params.value().get_or<std::string>("bone", "");
				if (BoneName.empty()) { Session.Log(TEXT("[FAIL] add(\"bone_track\") -> bone name required")); return sol::lua_nil; }

				FString BoneStr = NeoLuaStr::ToFString(BoneName);
				IAnimationDataController& Ctrl = Seq->GetController();
				IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "AddBoneTrack", "Add Bone Track"));
				bool bOk = Ctrl.AddBoneCurve(::FName(BoneStr));
				Session.Log(FString::Printf(TEXT("[%s] add(\"bone_track\", bone=\"%s\")"), bOk ? TEXT("OK") : TEXT("FAIL"), *BoneStr));
				return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
			}
			else if (FType.Equals(TEXT("curve"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"curve\") -> {name=..} required")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string CurveName = P.get_or<std::string>("name", "");
				if (CurveName.empty()) { Session.Log(TEXT("[FAIL] add(\"curve\") -> name required")); return sol::lua_nil; }

				FString FCurveName = NeoLuaStr::ToFString(CurveName);
				std::string TypeStr = P.get_or<std::string>("type", "float");
				ERawCurveTrackTypes CurveType = FString(UTF8_TO_TCHAR(TypeStr.c_str())).Equals(TEXT("transform"), ESearchCase::IgnoreCase)
					? ERawCurveTrackTypes::RCT_Transform : ERawCurveTrackTypes::RCT_Float;

				// If source is specified, duplicate an existing curve
				std::string SourceName = P.get_or<std::string>("source", "");
				if (!SourceName.empty())
				{
					FString FSourceName = NeoLuaStr::ToFString(SourceName);
					FAnimationCurveIdentifier SourceId(::FName(*FSourceName), CurveType);
					FAnimationCurveIdentifier NewId(::FName(*FCurveName), CurveType);
					IAnimationDataController& Ctrl = Seq->GetController();
					IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "DupCurve", "Duplicate Curve"));
					bool bOk = Ctrl.DuplicateCurve(SourceId, NewId);
					Session.Log(FString::Printf(TEXT("[%s] add(\"curve\", name=\"%s\", source=\"%s\")"), bOk ? TEXT("OK") : TEXT("FAIL"), *FCurveName, *FSourceName));
					return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
				}

				FAnimationCurveIdentifier CurveId(::FName(*FCurveName), CurveType);
				IAnimationDataController& Ctrl = Seq->GetController();
				IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "AddCurve", "Add Curve"));
				bool bOk = Ctrl.AddCurve(CurveId);
				Session.Log(FString::Printf(TEXT("[%s] add(\"curve\", name=\"%s\")"), bOk ? TEXT("OK") : TEXT("FAIL"), *FCurveName));
				return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
			}
			else if (FType.Equals(TEXT("curve_key"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"curve_key\") -> {curve=.., time=.., value=..} required")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string CrvName = P.get_or<std::string>("curve", "");
				if (CrvName.empty()) { Session.Log(TEXT("[FAIL] add(\"curve_key\") -> curve name required")); return sol::lua_nil; }

				FString FCrvName = NeoLuaStr::ToFString(CrvName);
				std::string CrvTypeStr = P.get_or<std::string>("type", "float");
				ERawCurveTrackTypes CrvType = FString(UTF8_TO_TCHAR(CrvTypeStr.c_str())).Equals(TEXT("transform"), ESearchCase::IgnoreCase)
					? ERawCurveTrackTypes::RCT_Transform : ERawCurveTrackTypes::RCT_Float;

				FAnimationCurveIdentifier CurveId(::FName(*FCrvName), CrvType);
				IAnimationDataController& Ctrl = Seq->GetController();
				if (CrvType == ERawCurveTrackTypes::RCT_Transform)
				{
					float Time = 0.0f;
					FTransform TransformValue;
					if (!ParseTransformCurveKey(P, Time, TransformValue))
					{
						Session.Log(TEXT("[FAIL] add(\"curve_key\") -> transform keys require time and at least one of location, rotation, or scale"));
						return sol::lua_nil;
					}

					IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "SetTransformCurveKey", "Set Transform Curve Key"));
					bool bOk = Ctrl.SetTransformCurveKey(CurveId, Time, TransformValue);
					Session.Log(FString::Printf(TEXT("[%s] add(\"curve_key\", curve=\"%s\", type=\"transform\", time=%.3f)"), bOk ? TEXT("OK") : TEXT("FAIL"), *FCrvName, Time));
					return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
				}

				// Parse via shared curve helper — accepts both `interp`/`interp_mode` and
				// `tangent`/`tangent_mode` aliases, case-insensitive enum strings, and weights.
				FRichCurveKey Key;
				if (!NeoLuaCurve::ParseKey(P, Key))
				{
					Session.Log(TEXT("[FAIL] add(\"curve_key\") -> time and value required"));
					return sol::lua_nil;
				}
				// Historical default: if no interp was specified, AnimSequence curves default to Cubic
				// (float-curve assets default to Linear instead). Preserve that behavior.
				sol::object InterpObj = P["interp"];
				if (!InterpObj.valid() || !InterpObj.is<std::string>())
				{
					sol::object InterpModeObj = P["interp_mode"];
					if (!InterpModeObj.valid() || !InterpModeObj.is<std::string>())
					{
						Key.InterpMode = RCIM_Cubic;
					}
				}

				IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "SetCurveKey", "Set Curve Key"));
				bool bOk = Ctrl.SetCurveKey(CurveId, Key);
				Session.Log(FString::Printf(TEXT("[%s] add(\"curve_key\", curve=\"%s\", time=%.3f, value=%.3f)"), bOk ? TEXT("OK") : TEXT("FAIL"), *FCrvName, Key.Time, Key.Value));
				return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
			}

			else if (FType.Equals(TEXT("sync_marker"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"sync_marker\") -> params required: {name=.., time=..}")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string Name = P.get_or<std::string>("name", "");
				if (Name.empty()) { Session.Log(TEXT("[FAIL] add(\"sync_marker\") -> name required")); return sol::lua_nil; }

				double Time = P.get<sol::optional<double>>("time").value_or(0.0);
				int32 Track = static_cast<int32>(P.get<sol::optional<double>>("track").value_or(0.0));
				float PlayLength = Seq->GetPlayLength();
				if (Time < 0.0 || Time > PlayLength)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"sync_marker\") -> time %.2f out of range (0-%.2f)"), Time, PlayLength));
					return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "AddSyncMarker", "Add Sync Marker"));
				Seq->Modify();

				FAnimSyncMarker NewMarker;
				NewMarker.MarkerName = ::FName(NeoLuaStr::ToFString(Name));
				NewMarker.Time = static_cast<float>(Time);
#if WITH_EDITORONLY_DATA
				NewMarker.Guid = FGuid::NewGuid();
#endif
				SetSyncMarkerTrackIndex(NewMarker, Track);
				Seq->AuthoredSyncMarkers.Add(NewMarker);
				Seq->SortSyncMarkers();
				Seq->RefreshSyncMarkerDataFromAuthored();
				Seq->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"sync_marker\", name=\"%s\", time=%.2f)"), UTF8_TO_TCHAR(Name.c_str()), Time));
				return sol::make_object(Lua, true);
			}

			else if (FType.Equals(TEXT("attribute"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"attribute\") -> {bone=.., name=.., type=..} required")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string BoneName = P.get_or<std::string>("bone", "");
				std::string AttrName = P.get_or<std::string>("name", "");
				std::string TypeStr = P.get_or<std::string>("type", "float");
				if (BoneName.empty() || AttrName.empty()) { Session.Log(TEXT("[FAIL] add(\"attribute\") -> bone and name required")); return sol::lua_nil; }

				FString FBoneName = NeoLuaStr::ToFString(BoneName);
				FString FAttrName = NeoLuaStr::ToFString(AttrName);
				FString FTypeStr = NeoLuaStr::ToFString(TypeStr);
				UScriptStruct* AttrType = ResolveAttributeType(FTypeStr);
				if (!AttrType) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"attribute\") -> unknown type \"%s\". Valid: float, int, string, transform, vector, quaternion"), *FTypeStr)); return sol::lua_nil; }

				FAnimationAttributeIdentifier AttrId = UAnimationAttributeIdentifierExtensions::CreateAttributeIdentifier(Seq, ::FName(*FAttrName), ::FName(*FBoneName), AttrType);
				if (!AttrId.IsValid()) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"attribute\") -> invalid identifier (bone \"%s\" may not exist in skeleton)"), *FBoneName)); return sol::lua_nil; }

				IAnimationDataController& Ctrl = Seq->GetController();
				IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "AddAttr", "Add Attribute"));
				bool bOk = Ctrl.AddAttribute(AttrId);
				Session.Log(FString::Printf(TEXT("[%s] add(\"attribute\", bone=\"%s\", name=\"%s\", type=\"%s\")"), bOk ? TEXT("OK") : TEXT("FAIL"), *FBoneName, *FAttrName, *FTypeStr));
				return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: notify, sync_marker, bone_track, curve, curve_key, attribute"), *FType));
			return sol::lua_nil;
		});

		// ---- remove(type, id) ----
		AssetObj.set_function("remove", [Seq, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = NeoLuaStr::ToFString(Type);

			if (FType.Equals(TEXT("notify"), ESearchCase::IgnoreCase))
			{
				if (!IsValid(Seq)) return sol::lua_nil;
				int32 IdxToRemove = INDEX_NONE;
				if (Id.is<int>())
				{
					IdxToRemove = Id.as<int>() - 1;
				}
				else if (Id.is<std::string>())
				{
					FString Target = NeoLuaStr::ToFString(Id.as<std::string>());
					for (int32 i = 0; i < Seq->Notifies.Num(); i++)
						if (Seq->Notifies[i].NotifyName.ToString().Equals(Target, ESearchCase::IgnoreCase)) { IdxToRemove = i; break; }
				}
				if (IdxToRemove < 0 || IdxToRemove >= Seq->Notifies.Num())
				{
					Session.Log(TEXT("[FAIL] remove(\"notify\") -> not found")); return sol::lua_nil;
				}
				FString Removed = Seq->Notifies[IdxToRemove].NotifyName.ToString();
				const FScopedTransaction Tx(NSLOCTEXT("AIK", "RemNotify", "Remove Notify"));
				Seq->Modify();
				Seq->Notifies.RemoveAt(IdxToRemove);
				Seq->RefreshCacheData();
				Seq->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove(\"notify\", \"%s\")"), *Removed));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("bone_track"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<std::string>()) { Session.Log(TEXT("[FAIL] remove(\"bone_track\") -> bone name required")); return sol::lua_nil; }
				FString BoneStr = NeoLuaStr::ToFString(Id.as<std::string>());
				IAnimationDataController& Ctrl = Seq->GetController();
				IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "RemBoneTrack", "Remove Bone Track"));
				bool bOk = Ctrl.RemoveBoneTrack(::FName(BoneStr));
				Session.Log(FString::Printf(TEXT("[%s] remove(\"bone_track\", \"%s\")"), bOk ? TEXT("OK") : TEXT("FAIL"), *BoneStr));
				return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
			}
			else if (FType.Equals(TEXT("curve"), ESearchCase::IgnoreCase))
			{
				FString FCurveName;
				ERawCurveTrackTypes CurveType = ERawCurveTrackTypes::RCT_Float;
				if (Id.is<std::string>())
				{
					FCurveName = NeoLuaStr::ToFString(Id.as<std::string>());
				}
				else if (Id.is<sol::table>())
				{
					sol::table T = Id.as<sol::table>();
					FCurveName = NeoLuaStr::ToFString(T.get_or<std::string>("name", ""));
					std::string TypeStr = T.get_or<std::string>("type", "float");
					if (FString(UTF8_TO_TCHAR(TypeStr.c_str())).Equals(TEXT("transform"), ESearchCase::IgnoreCase))
						CurveType = ERawCurveTrackTypes::RCT_Transform;
				}
				if (FCurveName.IsEmpty()) { Session.Log(TEXT("[FAIL] remove(\"curve\") -> curve name required")); return sol::lua_nil; }
				FAnimationCurveIdentifier CurveId(::FName(*FCurveName), CurveType);
				IAnimationDataController& Ctrl = Seq->GetController();
				IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "RemCurve", "Remove Curve"));
				bool bOk = Ctrl.RemoveCurve(CurveId);
				Session.Log(FString::Printf(TEXT("[%s] remove(\"curve\", \"%s\")"), bOk ? TEXT("OK") : TEXT("FAIL"), *FCurveName));
				return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
			}
			else if (FType.Equals(TEXT("curve_key"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<sol::table>()) { Session.Log(TEXT("[FAIL] remove(\"curve_key\") -> {curve=.., time=..} required")); return sol::lua_nil; }
				sol::table T = Id.as<sol::table>();
				std::string CrvName = T.get_or<std::string>("curve", "");
				if (CrvName.empty()) { Session.Log(TEXT("[FAIL] remove(\"curve_key\") -> curve name required")); return sol::lua_nil; }

				FString FCrvName = NeoLuaStr::ToFString(CrvName);
				std::string CrvTypeStr = T.get_or<std::string>("type", "float");
				ERawCurveTrackTypes CrvType = FString(UTF8_TO_TCHAR(CrvTypeStr.c_str())).Equals(TEXT("transform"), ESearchCase::IgnoreCase)
					? ERawCurveTrackTypes::RCT_Transform : ERawCurveTrackTypes::RCT_Float;

				float Time = static_cast<float>(T.get_or("time", 0.0));
				FAnimationCurveIdentifier CurveId(::FName(*FCrvName), CrvType);
				IAnimationDataController& Ctrl = Seq->GetController();
				IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "RemCurveKey", "Remove Curve Key"));
				bool bOk = (CrvType == ERawCurveTrackTypes::RCT_Transform)
					? Ctrl.RemoveTransformCurveKey(CurveId, Time)
					: Ctrl.RemoveCurveKey(CurveId, Time);
				Session.Log(FString::Printf(TEXT("[%s] remove(\"curve_key\", curve=\"%s\", time=%.3f)"), bOk ? TEXT("OK") : TEXT("FAIL"), *FCrvName, Time));
				return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
			}

			else if (FType.Equals(TEXT("sync_marker"), ESearchCase::IgnoreCase))
			{
				if (!IsValid(Seq)) return sol::lua_nil;
				const FScopedTransaction Tx(NSLOCTEXT("AIK", "RemSyncMarker", "Remove Sync Marker"));
				Seq->Modify();

				if (Id.is<int>())
				{
					int32 Idx = Id.as<int>() - 1;
					if (Idx < 0 || Idx >= Seq->AuthoredSyncMarkers.Num())
					{
						Session.Log(TEXT("[FAIL] remove(\"sync_marker\") -> index out of range")); return sol::lua_nil;
					}
					FString Removed = Seq->AuthoredSyncMarkers[Idx].MarkerName.ToString();
					Seq->AuthoredSyncMarkers.RemoveAt(Idx);
					Seq->SortSyncMarkers();
					Seq->RefreshSyncMarkerDataFromAuthored();
					Seq->MarkPackageDirty();
					Session.Log(FString::Printf(TEXT("[OK] remove(\"sync_marker\", %d) -> \"%s\""), Idx + 1, *Removed));
					return sol::make_object(Lua, true);
				}
				else if (Id.is<std::string>())
					{
						FName Target(NeoLuaStr::ToFString(Id.as<std::string>()));
						TArray<FName> Names = { Target };
						if (!Seq->RemoveSyncMarkers(Names))
						{
							Session.Log(FString::Printf(TEXT("[FAIL] remove(\"sync_marker\", \"%s\") -> not found"), *Target.ToString()));
							return sol::lua_nil;
						}
						Seq->SortSyncMarkers();
						Seq->RefreshSyncMarkerDataFromAuthored();
						Seq->MarkPackageDirty();
					Session.Log(FString::Printf(TEXT("[OK] remove(\"sync_marker\", \"%s\")"), *Target.ToString()));
					return sol::make_object(Lua, true);
				}
				Session.Log(TEXT("[FAIL] remove(\"sync_marker\") -> provide name or 1-based index")); return sol::lua_nil;
			}

			else if (FType.Equals(TEXT("attribute"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<sol::table>()) { Session.Log(TEXT("[FAIL] remove(\"attribute\") -> {bone=.., name=..} required")); return sol::lua_nil; }
				sol::table T = Id.as<sol::table>();
				std::string BoneName = T.get_or<std::string>("bone", "");
				std::string AttrName = T.get_or<std::string>("name", "");
				if (BoneName.empty() || AttrName.empty()) { Session.Log(TEXT("[FAIL] remove(\"attribute\") -> bone and name required")); return sol::lua_nil; }

				FString FBoneName = NeoLuaStr::ToFString(BoneName);
				FString FAttrName = NeoLuaStr::ToFString(AttrName);

				// Find the attribute to get its full identifier (with type info)
				IAnimationDataController& AttrCtrl = Seq->GetController();
				FAnimationAttributeIdentifier FoundId;
				bool bFound = false;
				for (const FAnimatedBoneAttribute& Attr : AttrCtrl.GetModel()->GetAttributes())
				{
					if (Attr.Identifier.GetBoneName() == ::FName(*FBoneName) && Attr.Identifier.GetName() == ::FName(*FAttrName))
					{
						FoundId = Attr.Identifier;
						bFound = true;
						break;
					}
				}
				if (!bFound) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"attribute\") -> attribute \"%s\" on bone \"%s\" not found"), *FAttrName, *FBoneName)); return sol::lua_nil; }

				IAnimationDataController::FScopedBracket Bracket(&AttrCtrl, NSLOCTEXT("AIK", "RemAttr", "Remove Attribute"));
				bool bOk = AttrCtrl.RemoveAttribute(FoundId);
				Session.Log(FString::Printf(TEXT("[%s] remove(\"attribute\", bone=\"%s\", name=\"%s\")"), bOk ? TEXT("OK") : TEXT("FAIL"), *FBoneName, *FAttrName));
				return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
			}

			else if (FType.Equals(TEXT("all_attributes"), ESearchCase::IgnoreCase))
			{
				IAnimationDataController& Ctrl = Seq->GetController();
				IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "RemAllAttr", "Remove All Attributes"));
				int32 Count = Ctrl.RemoveAllAttributes();
				Session.Log(FString::Printf(TEXT("[OK] remove(\"all_attributes\") -> %d removed"), Count));
				return sol::make_object(Lua, Count);
			}

			else if (FType.Equals(TEXT("bone_attributes"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<std::string>()) { Session.Log(TEXT("[FAIL] remove(\"bone_attributes\") -> bone name required")); return sol::lua_nil; }
				FString FBoneName = NeoLuaStr::ToFString(Id.as<std::string>());
				IAnimationDataController& Ctrl = Seq->GetController();
				IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "RemBoneAttr", "Remove Bone Attributes"));
				int32 Count = Ctrl.RemoveAllAttributesForBone(::FName(*FBoneName));
				Session.Log(FString::Printf(TEXT("[OK] remove(\"bone_attributes\", \"%s\") -> %d removed"), *FBoneName, Count));
				return sol::make_object(Lua, Count);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type. Valid: notify, sync_marker, bone_track, curve, curve_key, attribute, all_attributes, bone_attributes"), *FType));
			return sol::lua_nil;
		});

		// ---- list(type?) ----
		AssetObj.set_function("list", [Seq, &Session](sol::table self,
			sol::optional<std::string> TypeOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = NeoLuaStr::ToFStringOpt(TypeOpt, TEXT("all"));

			if (FType.Equals(TEXT("all"), ESearchCase::IgnoreCase))
			{
				sol::protected_function InfoFn = self["info"];
				if (InfoFn.valid()) return InfoFn(self);
				return sol::lua_nil;
			}

			if (FType.Contains(TEXT("notif"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Seq->Notifies.Num(); i++)
				{
					const FAnimNotifyEvent& E = Seq->Notifies[i];
					sol::table Entry = Lua.create_table();
					Entry["index"] = i + 1;
					Entry["name"] = TCHAR_TO_UTF8(*E.NotifyName.ToString());
					Entry["time"] = E.GetTriggerTime();
					Entry["duration"] = E.GetDuration();
					Entry["track"] = E.TrackIndex;
					Result[i + 1] = Entry;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"notifies\") -> %d"), Seq->Notifies.Num()));
				return Result;
			}

			// For bone_tracks and curves, return summary
			if (FType.Contains(TEXT("bone"), ESearchCase::IgnoreCase) || FType.Contains(TEXT("track"), ESearchCase::IgnoreCase))
			{
				IAnimationDataController& Ctrl = Seq->GetController();
				TArray<FName> TrackNames;
				Ctrl.GetModel()->GetBoneTrackNames(TrackNames);
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < TrackNames.Num(); i++)
				{
					sol::table E = Lua.create_table();
					E["index"] = i + 1;
					E["bone"] = TCHAR_TO_UTF8(*TrackNames[i].ToString());
					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"bone_tracks\") -> %d"), TrackNames.Num()));
				return Result;
			}

			if (FType.Contains(TEXT("curve"), ESearchCase::IgnoreCase))
			{
				IAnimationDataController& Ctrl = Seq->GetController();
				const TArray<FFloatCurve>& FloatCurves = Ctrl.GetModel()->GetFloatCurves();
				const TArray<FTransformCurve>& TransformCurves = Ctrl.GetModel()->GetTransformCurves();
				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				for (int32 i = 0; i < FloatCurves.Num(); i++)
				{
					sol::table E = Lua.create_table();
					E["index"] = Idx;
					E["name"] = TCHAR_TO_UTF8(*FloatCurves[i].GetName().ToString());
					E["type"] = "float";
					E["keys"] = FloatCurves[i].FloatCurve.GetNumKeys();
					E["editable"] = FloatCurves[i].GetCurveTypeFlag(AACF_Editable);
					E["disabled"] = FloatCurves[i].GetCurveTypeFlag(AACF_Disabled);
					E["metadata"] = FloatCurves[i].GetCurveTypeFlag(AACF_Metadata);
					Result[Idx++] = E;
				}
				for (int32 i = 0; i < TransformCurves.Num(); i++)
				{
					sol::table E = Lua.create_table();
					E["index"] = Idx;
					E["name"] = TCHAR_TO_UTF8(*TransformCurves[i].GetName().ToString());
					E["type"] = "transform";
					E["keys"] = TransformCurves[i].TranslationCurve.GetNumKeys();
					E["editable"] = TransformCurves[i].GetCurveTypeFlag(AACF_Editable);
					E["disabled"] = TransformCurves[i].GetCurveTypeFlag(AACF_Disabled);
					E["metadata"] = TransformCurves[i].GetCurveTypeFlag(AACF_Metadata);
					Result[Idx++] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"curves\") -> %d float, %d transform"), FloatCurves.Num(), TransformCurves.Num()));
				return Result;
			}

			if (FType.Contains(TEXT("sync"), ESearchCase::IgnoreCase) || FType.Contains(TEXT("marker"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Seq->AuthoredSyncMarkers.Num(); i++)
				{
					const FAnimSyncMarker& M = Seq->AuthoredSyncMarkers[i];
					sol::table Entry = Lua.create_table();
					Entry["index"] = i + 1;
					Entry["name"] = TCHAR_TO_UTF8(*M.MarkerName.ToString());
					Entry["time"] = M.Time;
					Entry["track_index"] = GetSyncMarkerTrackIndex(M);
					Result[i + 1] = Entry;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"sync_markers\") -> %d"), Seq->AuthoredSyncMarkers.Num()));
				return Result;
			}

			if (FType.Contains(TEXT("attrib"), ESearchCase::IgnoreCase))
			{
				IAnimationDataController& AttrCtrl = Seq->GetController();
				TArrayView<const FAnimatedBoneAttribute> Attributes = AttrCtrl.GetModel()->GetAttributes();
				sol::table Result = Lua.create_table();
				int32 Idx = 1;
				for (const FAnimatedBoneAttribute& Attr : Attributes)
				{
					sol::table Entry = Lua.create_table();
					Entry["index"] = Idx;
					Entry["bone"] = TCHAR_TO_UTF8(*Attr.Identifier.GetBoneName().ToString());
					Entry["name"] = TCHAR_TO_UTF8(*Attr.Identifier.GetName().ToString());
					Entry["type"] = TCHAR_TO_UTF8(*AttributeTypeToString(Attr.Identifier.GetType()));
					Entry["keys"] = Attr.Curve.GetNumKeys();
					Result[Idx++] = Entry;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"attributes\") -> %d"), Attributes.Num()));
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: notifies, sync_markers, bone_tracks, curves, attributes"), *FType));
			return sol::lua_nil;
		});

		// ---- Action: set_bone_keys(bone, data) ----
		AssetObj.set_function("set_bone_keys", [Seq, &Session](sol::table /*self*/,
			const std::string& BoneName, sol::table Data, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Seq)) return sol::lua_nil;

			FString BoneStr = NeoLuaStr::ToFString(BoneName);
			::FName Bone(BoneStr);

			sol::optional<sol::table> PosOpt = Data.get<sol::optional<sol::table>>("positions");
			sol::optional<sol::table> RotOpt = Data.get<sol::optional<sol::table>>("rotations");
			if (!PosOpt.has_value() || !RotOpt.has_value())
			{
				Session.Log(TEXT("[FAIL] set_bone_keys -> \"positions\" and \"rotations\" tables required"));
				return sol::lua_nil;
			}
			sol::table Positions = PosOpt.value();
			sol::table Rotations = RotOpt.value();
			sol::optional<sol::table> ScalesOpt = Data.get<sol::optional<sol::table>>("scales");

			int32 NumKeys = static_cast<int32>(Positions.size());
			TArray<FVector3f> PosKeys;
			TArray<FQuat4f> RotKeys;
			TArray<FVector3f> ScaleKeys;
			PosKeys.Reserve(NumKeys);
			RotKeys.Reserve(NumKeys);
			ScaleKeys.Reserve(NumKeys);

			for (int32 i = 1; i <= NumKeys; i++)
			{
				sol::table P = Positions[i];
				PosKeys.Add(FVector3f(
					static_cast<float>(P.get_or(1, 0.0)),
					static_cast<float>(P.get_or(2, 0.0)),
					static_cast<float>(P.get_or(3, 0.0))));

				sol::table R = Rotations[i];
				if (R.size() >= 4)
					RotKeys.Add(FQuat4f(
						static_cast<float>(R.get_or(1, 0.0)),
						static_cast<float>(R.get_or(2, 0.0)),
						static_cast<float>(R.get_or(3, 0.0)),
						static_cast<float>(R.get_or(4, 1.0))));
				else
				{
					FRotator3f Rot(
						static_cast<float>(R.get_or(1, 0.0)),
						static_cast<float>(R.get_or(2, 0.0)),
						static_cast<float>(R.get_or(3, 0.0)));
					RotKeys.Add(Rot.Quaternion());
				}

				if (ScalesOpt.has_value())
				{
					sol::table Sc = ScalesOpt.value()[i];
					ScaleKeys.Add(FVector3f(
						static_cast<float>(Sc.get_or(1, 1.0)),
						static_cast<float>(Sc.get_or(2, 1.0)),
						static_cast<float>(Sc.get_or(3, 1.0))));
				}
				else
					ScaleKeys.Add(FVector3f(1, 1, 1));
			}

			IAnimationDataController& Ctrl = Seq->GetController();
			IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "SetBoneKeys", "Set Bone Keys"));
			bool bOk = Ctrl.SetBoneTrackKeys(Bone, PosKeys, RotKeys, ScaleKeys);
			Session.Log(FString::Printf(TEXT("[%s] set_bone_keys(\"%s\", %d keys)"), bOk ? TEXT("OK") : TEXT("FAIL"), *Bone.ToString(), NumKeys));
			return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
		});

		// ---- Action: set_curve_keys(name, keys, type?) ----
		AssetObj.set_function("set_curve_keys", [Seq, &Session](sol::table /*self*/,
			const std::string& CurveName, sol::table Keys, sol::optional<std::string> TypeOpt,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Seq)) return sol::lua_nil;

			FString FCurveName = NeoLuaStr::ToFString(CurveName);
			ERawCurveTrackTypes CurveType = (TypeOpt.has_value() && FString(UTF8_TO_TCHAR(TypeOpt.value().c_str())).Equals(TEXT("transform"), ESearchCase::IgnoreCase))
				? ERawCurveTrackTypes::RCT_Transform : ERawCurveTrackTypes::RCT_Float;

			FAnimationCurveIdentifier CurveId(::FName(*FCurveName), CurveType);
			if (CurveType == ERawCurveTrackTypes::RCT_Transform)
			{
				TArray<FTransform> Transforms;
				TArray<float> Times;
				int32 NumKeys = static_cast<int32>(Keys.size());
				Transforms.Reserve(NumKeys);
				Times.Reserve(NumKeys);

				for (int32 i = 1; i <= NumKeys; i++)
				{
					sol::object Entry = Keys[i];
					if (!Entry.valid() || !Entry.is<sol::table>())
					{
						Session.Log(FString::Printf(TEXT("[FAIL] set_curve_keys(\"%s\", transform) -> key %d must be a table"), *FCurveName, i));
						return sol::lua_nil;
					}

					float Time = 0.0f;
					FTransform TransformValue;
					if (!ParseTransformCurveKey(Entry.as<sol::table>(), Time, TransformValue))
					{
						Session.Log(FString::Printf(TEXT("[FAIL] set_curve_keys(\"%s\", transform) -> key %d requires time and at least one transform component"), *FCurveName, i));
						return sol::lua_nil;
					}
					Times.Add(Time);
					Transforms.Add(TransformValue);
				}

				IAnimationDataController& Ctrl = Seq->GetController();
				IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "SetTransformCurveKeys", "Set Transform Curve Keys"));
				bool bOk = Ctrl.SetTransformCurveKeys(CurveId, Transforms, Times);
				Session.Log(FString::Printf(TEXT("[%s] set_curve_keys(\"%s\", transform, %d keys)"), bOk ? TEXT("OK") : TEXT("FAIL"), *FCurveName, NumKeys));
				return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
			}

			TArray<FRichCurveKey> CurveKeys;
			for (auto& [_, val] : Keys)
			{
				if (!val.is<sol::table>()) continue;
				sol::table K = val.as<sol::table>();
				FRichCurveKey Key;
				if (!NeoLuaCurve::ParseKey(K, Key)) continue;
				// Historical default: AnimSequence curves prefer Cubic interp when none specified.
				sol::object InterpObj = K["interp"];
				bool bHasInterp = (InterpObj.valid() && InterpObj.is<std::string>());
				if (!bHasInterp)
				{
					sol::object InterpModeObj = K["interp_mode"];
					bHasInterp = (InterpModeObj.valid() && InterpModeObj.is<std::string>());
				}
				if (!bHasInterp) Key.InterpMode = RCIM_Cubic;
				CurveKeys.Add(Key);
			}

			IAnimationDataController& Ctrl = Seq->GetController();
			IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "SetCurveKeys", "Set Curve Keys"));
			bool bOk = Ctrl.SetCurveKeys(CurveId, CurveKeys);
			Session.Log(FString::Printf(TEXT("[%s] set_curve_keys(\"%s\", %d keys)"), bOk ? TEXT("OK") : TEXT("FAIL"), *FCurveName, CurveKeys.Num()));
			return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
		});

		// ---- Action: set_frame_rate / set_num_frames ----
		AssetObj.set_function("set_frame_rate", [Seq, &Session](sol::table /*self*/, int Fps, sol::this_state S) -> sol::object
			{
				sol::state_view Lua(S);
				if (!IsValid(Seq)) return sol::lua_nil;
				if (Fps <= 0)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] set_frame_rate(%d) -> fps must be positive and non-zero"), Fps));
					return sol::lua_nil;
				}
				IAnimationDataController& Ctrl = Seq->GetController();
				IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "SetFR", "Set Frame Rate"));
				Ctrl.SetFrameRate(FFrameRate(Fps, 1));
			Session.Log(FString::Printf(TEXT("[OK] set_frame_rate(%d)"), Fps));
			return sol::make_object(Lua, true);
		});

		AssetObj.set_function("set_num_frames", [Seq, &Session](sol::table /*self*/, int Count, sol::this_state S) -> sol::object
			{
				sol::state_view Lua(S);
				if (!IsValid(Seq)) return sol::lua_nil;
				if (Count <= 0)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] set_num_frames(%d) -> count must be positive and non-zero"), Count));
					return sol::lua_nil;
				}
				IAnimationDataController& Ctrl = Seq->GetController();
				IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "SetFrames", "Set Num Frames"));
				Ctrl.SetNumberOfFrames(FFrameNumber(Count));
			Session.Log(FString::Printf(TEXT("[OK] set_num_frames(%d)"), Count));
			return sol::make_object(Lua, true);
		});

		// ---- Action: update_bone_keys(bone, start_frame, data) ----
		AssetObj.set_function("update_bone_keys", [Seq, &Session](sol::table /*self*/,
			const std::string& BoneName, int StartFrame, sol::table Data, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Seq)) return sol::lua_nil;

			FString BoneStr = NeoLuaStr::ToFString(BoneName);
			::FName Bone(BoneStr);

			sol::optional<sol::table> PosOpt = Data.get<sol::optional<sol::table>>("positions");
			sol::optional<sol::table> RotOpt = Data.get<sol::optional<sol::table>>("rotations");
			if (!PosOpt.has_value() || !RotOpt.has_value())
			{
				Session.Log(TEXT("[FAIL] update_bone_keys -> \"positions\" and \"rotations\" tables required"));
				return sol::lua_nil;
			}
			sol::table Positions = PosOpt.value();
			sol::table Rotations = RotOpt.value();
			sol::optional<sol::table> ScalesOpt = Data.get<sol::optional<sol::table>>("scales");

			int32 NumKeys = static_cast<int32>(Positions.size());
			TArray<FVector3f> PosKeys;
			TArray<FQuat4f> RotKeys;
			TArray<FVector3f> ScaleKeys;
			PosKeys.Reserve(NumKeys);
			RotKeys.Reserve(NumKeys);
			ScaleKeys.Reserve(NumKeys);

			for (int32 i = 1; i <= NumKeys; i++)
			{
				sol::table P = Positions[i];
				PosKeys.Add(FVector3f(
					static_cast<float>(P.get_or(1, 0.0)),
					static_cast<float>(P.get_or(2, 0.0)),
					static_cast<float>(P.get_or(3, 0.0))));

				sol::table R = Rotations[i];
				if (R.size() >= 4)
					RotKeys.Add(FQuat4f(
						static_cast<float>(R.get_or(1, 0.0)),
						static_cast<float>(R.get_or(2, 0.0)),
						static_cast<float>(R.get_or(3, 0.0)),
						static_cast<float>(R.get_or(4, 1.0))));
				else
				{
					FRotator3f Rot(
						static_cast<float>(R.get_or(1, 0.0)),
						static_cast<float>(R.get_or(2, 0.0)),
						static_cast<float>(R.get_or(3, 0.0)));
					RotKeys.Add(Rot.Quaternion());
				}

				if (ScalesOpt.has_value())
				{
					sol::table Sc = ScalesOpt.value()[i];
					ScaleKeys.Add(FVector3f(
						static_cast<float>(Sc.get_or(1, 1.0)),
						static_cast<float>(Sc.get_or(2, 1.0)),
						static_cast<float>(Sc.get_or(3, 1.0))));
				}
				else
					ScaleKeys.Add(FVector3f(1, 1, 1));
			}

			FInt32Range KeyRange(StartFrame, StartFrame + NumKeys);
			IAnimationDataController& Ctrl = Seq->GetController();
			IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "UpdateBoneKeys", "Update Bone Keys"));
			bool bOk = Ctrl.UpdateBoneTrackKeys(Bone, KeyRange, PosKeys, RotKeys, ScaleKeys);
			Session.Log(FString::Printf(TEXT("[%s] update_bone_keys(\"%s\", start=%d, %d keys)"), bOk ? TEXT("OK") : TEXT("FAIL"), *Bone.ToString(), StartFrame, NumKeys));
			return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
		});

		// ---- Action: set_transform_curve_keys(name, keys) ----
		AssetObj.set_function("set_transform_curve_keys", [Seq, &Session](sol::table /*self*/,
			const std::string& CurveName, sol::table Keys, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Seq)) return sol::lua_nil;

			FString FCurveName = NeoLuaStr::ToFString(CurveName);
			FAnimationCurveIdentifier CurveId(::FName(*FCurveName), ERawCurveTrackTypes::RCT_Transform);

			TArray<FTransform> Transforms;
			TArray<float> Times;
			int32 NumKeys = static_cast<int32>(Keys.size());
			Transforms.Reserve(NumKeys);
			Times.Reserve(NumKeys);

				for (int32 i = 1; i <= NumKeys; i++)
				{
					sol::object Entry = Keys[i];
					if (!Entry.valid() || !Entry.is<sol::table>())
					{
						Session.Log(FString::Printf(TEXT("[FAIL] set_transform_curve_keys(\"%s\") -> key %d must be a table"), *FCurveName, i));
						return sol::lua_nil;
					}

					float Time = 0.0f;
					FTransform TransformValue;
					if (!ParseTransformCurveKey(Entry.as<sol::table>(), Time, TransformValue))
					{
						Session.Log(FString::Printf(TEXT("[FAIL] set_transform_curve_keys(\"%s\") -> key %d requires time and at least one transform component"), *FCurveName, i));
						return sol::lua_nil;
					}
					Times.Add(Time);
					Transforms.Add(TransformValue);
				}

			IAnimationDataController& Ctrl = Seq->GetController();
			IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "SetTCKeys", "Set Transform Curve Keys"));
			bool bOk = Ctrl.SetTransformCurveKeys(CurveId, Transforms, Times);
			Session.Log(FString::Printf(TEXT("[%s] set_transform_curve_keys(\"%s\", %d keys)"), bOk ? TEXT("OK") : TEXT("FAIL"), *FCurveName, NumKeys));
			return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
		});

		// ---- Action: duplicate_curve(source_name, new_name, type?) ----
		AssetObj.set_function("duplicate_curve", [Seq, &Session](sol::table /*self*/,
			const std::string& SourceName, const std::string& NewName,
			sol::optional<std::string> TypeOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Seq)) return sol::lua_nil;

			ERawCurveTrackTypes CurveType = (TypeOpt.has_value() && FString(UTF8_TO_TCHAR(TypeOpt.value().c_str())).Equals(TEXT("transform"), ESearchCase::IgnoreCase))
				? ERawCurveTrackTypes::RCT_Transform : ERawCurveTrackTypes::RCT_Float;

			FString FSourceName = NeoLuaStr::ToFString(SourceName);
			FString FNewName = NeoLuaStr::ToFString(NewName);
			FAnimationCurveIdentifier SourceId(::FName(*FSourceName), CurveType);
			FAnimationCurveIdentifier NewId(::FName(*FNewName), CurveType);

			IAnimationDataController& Ctrl = Seq->GetController();
			IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "DuplicateCurve", "Duplicate Curve"));
			bool bOk = Ctrl.DuplicateCurve(SourceId, NewId);
			Session.Log(FString::Printf(TEXT("[%s] duplicate_curve(\"%s\" -> \"%s\")"), bOk ? TEXT("OK") : TEXT("FAIL"), *FSourceName, *FNewName));
			return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
		});

		// ---- Action: rename_curve(old_name, new_name, type?) ----
		AssetObj.set_function("rename_curve", [Seq, &Session](sol::table /*self*/,
			const std::string& OldName, const std::string& NewName,
			sol::optional<std::string> TypeOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Seq)) return sol::lua_nil;

			ERawCurveTrackTypes CurveType = (TypeOpt.has_value() && FString(UTF8_TO_TCHAR(TypeOpt.value().c_str())).Equals(TEXT("transform"), ESearchCase::IgnoreCase))
				? ERawCurveTrackTypes::RCT_Transform : ERawCurveTrackTypes::RCT_Float;

			FString FOldName = NeoLuaStr::ToFString(OldName);
			FString FNewName = NeoLuaStr::ToFString(NewName);
			FAnimationCurveIdentifier OldId(::FName(*FOldName), CurveType);
			FAnimationCurveIdentifier NewId(::FName(*FNewName), CurveType);

			IAnimationDataController& Ctrl = Seq->GetController();
			IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "RenameCurve", "Rename Curve"));
			bool bOk = Ctrl.RenameCurve(OldId, NewId);
			Session.Log(FString::Printf(TEXT("[%s] rename_curve(\"%s\" -> \"%s\")"), bOk ? TEXT("OK") : TEXT("FAIL"), *FOldName, *FNewName));
			return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
		});

		// ---- Action: scale_curve(name, factor, origin?, type?) ----
		AssetObj.set_function("scale_curve", [Seq, &Session](sol::table /*self*/,
			const std::string& CurveName, double Factor,
			sol::optional<double> OriginOpt, sol::optional<std::string> TypeOpt,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Seq)) return sol::lua_nil;

			ERawCurveTrackTypes CurveType = (TypeOpt.has_value() && FString(UTF8_TO_TCHAR(TypeOpt.value().c_str())).Equals(TEXT("transform"), ESearchCase::IgnoreCase))
				? ERawCurveTrackTypes::RCT_Transform : ERawCurveTrackTypes::RCT_Float;

			FString FCurveName = NeoLuaStr::ToFString(CurveName);
			FAnimationCurveIdentifier CurveId(::FName(*FCurveName), CurveType);
			float Origin = static_cast<float>(OriginOpt.value_or(0.0));

			IAnimationDataController& Ctrl = Seq->GetController();
			IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "ScaleCurve", "Scale Curve"));
			bool bOk = Ctrl.ScaleCurve(CurveId, Origin, static_cast<float>(Factor));
			Session.Log(FString::Printf(TEXT("[%s] scale_curve(\"%s\", factor=%.2f, origin=%.2f)"), bOk ? TEXT("OK") : TEXT("FAIL"), *FCurveName, Factor, Origin));
			return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
		});

		// ---- Action: set_curve_color(name, color) ----
		AssetObj.set_function("set_curve_color", [Seq, &Session](sol::table /*self*/,
			const std::string& CurveName, sol::table Color,
			sol::optional<std::string> TypeOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Seq)) return sol::lua_nil;

			if (Color.size() < 3) { Session.Log(TEXT("[FAIL] set_curve_color -> color needs at least {r, g, b}")); return sol::lua_nil; }

			if (TypeOpt.has_value())
			{
				const FString FType = NeoLuaStr::ToFStringOpt(TypeOpt);
				if (!FType.IsEmpty() && !FType.Equals(TEXT("float"), ESearchCase::IgnoreCase))
				{
					Session.Log(TEXT("[FAIL] set_curve_color -> UE 5.7 only supports editor colors for float curves"));
					return sol::lua_nil;
				}
			}

			FString FCurveName = NeoLuaStr::ToFString(CurveName);
			FAnimationCurveIdentifier CurveId(::FName(*FCurveName), ERawCurveTrackTypes::RCT_Float);
			FLinearColor C(
				static_cast<float>(Color.get_or(1, 0.0)),
				static_cast<float>(Color.get_or(2, 0.0)),
				static_cast<float>(Color.get_or(3, 0.0)),
				static_cast<float>(Color.get_or(4, 1.0)));

			IAnimationDataController& Ctrl = Seq->GetController();
			IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "SetCurveColor", "Set Curve Color"));
			bool bOk = Ctrl.SetCurveColor(CurveId, C);
			Session.Log(FString::Printf(TEXT("[%s] set_curve_color(\"%s\", {%.2f,%.2f,%.2f,%.2f})"), bOk ? TEXT("OK") : TEXT("FAIL"), *FCurveName, C.R, C.G, C.B, C.A));
			return bOk ? sol::make_object(Lua, true) : sol::lua_nil;
		});

		// ---- configure(type, id, params) ----
		AssetObj.set_function("configure", [Seq, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Seq)) return sol::lua_nil;
			FString FType = NeoLuaStr::ToFString(Type);

			if (FType.Equals(TEXT("notify"), ESearchCase::IgnoreCase))
			{
				int32 Idx = INDEX_NONE;
				if (Id.is<int>())
				{
					Idx = Id.as<int>() - 1;
				}
				else if (Id.is<std::string>())
				{
					FString Target = NeoLuaStr::ToFString(Id.as<std::string>());
					for (int32 i = 0; i < Seq->Notifies.Num(); i++)
						if (Seq->Notifies[i].NotifyName.ToString().Equals(Target, ESearchCase::IgnoreCase)) { Idx = i; break; }
				}
				if (Idx < 0 || Idx >= Seq->Notifies.Num())
				{
					Session.Log(TEXT("[FAIL] configure(\"notify\") -> not found")); return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "ConfigNotify", "Configure Notify"));
				Seq->Modify();
				FAnimNotifyEvent& Evt = Seq->Notifies[Idx];

				sol::optional<double> NewTime = Params.get<sol::optional<double>>("time");
				if (NewTime.has_value())
				{
					float T = static_cast<float>(NewTime.value());
					Evt.Link(Seq, T);
					Evt.TriggerTimeOffset = GetTriggerTimeOffsetForType(Seq->CalculateOffsetForNotify(T));
				}

				sol::optional<double> NewDuration = Params.get<sol::optional<double>>("duration");
				if (NewDuration.has_value() && Evt.NotifyStateClass)
				{
					Evt.Duration = static_cast<float>(NewDuration.value());
					Evt.EndLink.Link(Seq, Evt.GetTriggerTime() + Evt.Duration);
				}

				sol::optional<double> NewTrack = Params.get<sol::optional<double>>("track");
				if (NewTrack.has_value())
				{
					Evt.TrackIndex = static_cast<int32>(NewTrack.value());
				}

				sol::optional<double> TriggerChance = Params.get<sol::optional<double>>("trigger_chance");
				if (TriggerChance.has_value())
				{
					Evt.NotifyTriggerChance = FMath::Clamp(static_cast<float>(TriggerChance.value()), 0.0f, 1.0f);
				}

				sol::optional<bool> BranchingPoint = Params.get<sol::optional<bool>>("branching_point");
				if (BranchingPoint.has_value())
				{
					Evt.MontageTickType = BranchingPoint.value() ? EMontageNotifyTickType::BranchingPoint : EMontageNotifyTickType::Queued;
				}

				sol::optional<double> WeightThreshold = Params.get<sol::optional<double>>("trigger_weight_threshold");
				if (WeightThreshold.has_value())
				{
					Evt.TriggerWeightThreshold = static_cast<float>(WeightThreshold.value());
				}

				sol::optional<std::string> FilterType = Params.get<sol::optional<std::string>>("filter_type");
				if (FilterType.has_value())
				{
					FString FFilter = NeoLuaStr::ToFStringOpt(FilterType);
					if (FFilter.Equals(TEXT("LOD"), ESearchCase::IgnoreCase))
						Evt.NotifyFilterType = ENotifyFilterType::LOD;
					else
						Evt.NotifyFilterType = ENotifyFilterType::NoFiltering;
				}

				sol::optional<double> FilterLOD = Params.get<sol::optional<double>>("filter_lod");
				if (FilterLOD.has_value())
				{
					Evt.NotifyFilterLOD = static_cast<int32>(FilterLOD.value());
				}

				sol::optional<bool> DedicatedServer = Params.get<sol::optional<bool>>("dedicated_server");
				if (DedicatedServer.has_value())
				{
					Evt.bTriggerOnDedicatedServer = DedicatedServer.value();
				}

				sol::optional<bool> TriggerFollower = Params.get<sol::optional<bool>>("trigger_on_follower");
				if (TriggerFollower.has_value())
				{
					Evt.bTriggerOnFollower = TriggerFollower.value();
				}

				Seq->SortNotifies();
				Seq->RefreshCacheData();
				Seq->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"notify\", %d)"), Idx + 1));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("sync_marker"), ESearchCase::IgnoreCase))
			{
				int32 Idx = INDEX_NONE;
				if (Id.is<int>())
				{
					Idx = Id.as<int>() - 1;
				}
				else if (Id.is<std::string>())
				{
					FName Target(NeoLuaStr::ToFString(Id.as<std::string>()));
					for (int32 i = 0; i < Seq->AuthoredSyncMarkers.Num(); i++)
						if (Seq->AuthoredSyncMarkers[i].MarkerName == Target) { Idx = i; break; }
				}
				if (Idx < 0 || Idx >= Seq->AuthoredSyncMarkers.Num())
				{
					Session.Log(TEXT("[FAIL] configure(\"sync_marker\") -> not found")); return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "ConfigSyncMarker", "Configure Sync Marker"));
				Seq->Modify();
				FAnimSyncMarker& Marker = Seq->AuthoredSyncMarkers[Idx];

				sol::optional<std::string> NewName = Params.get<sol::optional<std::string>>("name");
				if (NewName.has_value())
				{
					FName OldName = Marker.MarkerName;
					FName NewFName(NeoLuaStr::ToFStringOpt(NewName));
					if (OldName != NewFName)
					{
						Seq->RenameSyncMarkers(OldName, NewFName);
					}
				}

				sol::optional<double> NewTime = Params.get<sol::optional<double>>("time");
				if (NewTime.has_value())
				{
					Marker.Time = static_cast<float>(NewTime.value());
				}

				sol::optional<double> NewTrack = Params.get<sol::optional<double>>("track");
				if (NewTrack.has_value())
				{
					SetSyncMarkerTrackIndex(Marker, static_cast<int32>(NewTrack.value()));
				}

				Seq->SortSyncMarkers();
				Seq->RefreshSyncMarkerDataFromAuthored();
				Seq->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"sync_marker\", %d)"), Idx + 1));
				return sol::make_object(Lua, true);
			}

			else if (FType.Equals(TEXT("curve"), ESearchCase::IgnoreCase))
			{
				FString FCurveName;
				ERawCurveTrackTypes CurveType = ERawCurveTrackTypes::RCT_Float;
				if (Id.is<std::string>())
				{
					FCurveName = NeoLuaStr::ToFString(Id.as<std::string>());
				}
				else if (Id.is<sol::table>())
				{
					sol::table T = Id.as<sol::table>();
					FCurveName = NeoLuaStr::ToFString(T.get_or<std::string>("name", ""));
					std::string TypeStr = T.get_or<std::string>("type", "float");
					if (FString(UTF8_TO_TCHAR(TypeStr.c_str())).Equals(TEXT("transform"), ESearchCase::IgnoreCase))
						CurveType = ERawCurveTrackTypes::RCT_Transform;
				}
				if (FCurveName.IsEmpty()) { Session.Log(TEXT("[FAIL] configure(\"curve\") -> curve name required")); return sol::lua_nil; }

				FAnimationCurveIdentifier CurveId(::FName(*FCurveName), CurveType);
				IAnimationDataController& Ctrl = Seq->GetController();
					IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "CfgCurveFlags", "Configure Curve Flags"));

					bool bAnySet = false;
					bool bAllSucceeded = true;
					FString FailedFlags;
					auto ApplyCurveFlag = [&](const TCHAR* FlagName, EAnimAssetCurveFlags Flag, bool bState)
					{
						bAnySet = true;
						if (!Ctrl.SetCurveFlag(CurveId, Flag, bState))
						{
							bAllSucceeded = false;
							if (!FailedFlags.IsEmpty())
							{
								FailedFlags += TEXT(", ");
							}
							FailedFlags += FlagName;
						}
					};

					sol::optional<bool> Editable = Params.get<sol::optional<bool>>("editable");
					if (Editable.has_value())
					{
						ApplyCurveFlag(TEXT("editable"), AACF_Editable, Editable.value());
					}
					sol::optional<bool> Disabled = Params.get<sol::optional<bool>>("disabled");
					if (Disabled.has_value())
					{
						ApplyCurveFlag(TEXT("disabled"), AACF_Disabled, Disabled.value());
					}
					sol::optional<bool> Metadata = Params.get<sol::optional<bool>>("metadata");
					if (Metadata.has_value())
					{
						ApplyCurveFlag(TEXT("metadata"), AACF_Metadata, Metadata.value());
					}

					if (!bAnySet) { Session.Log(TEXT("[FAIL] configure(\"curve\") -> specify editable, disabled, or metadata")); return sol::lua_nil; }
					if (!bAllSucceeded)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] configure(\"curve\", \"%s\") -> failed to set flag(s): %s"), *FCurveName, *FailedFlags));
						return sol::lua_nil;
					}
					Session.Log(FString::Printf(TEXT("[OK] configure(\"curve\", \"%s\")"), *FCurveName));
					return sol::make_object(Lua, true);
				}

			Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown type. Valid: notify, sync_marker, sequence, curve"), *FType));
			return sol::lua_nil;
		});

		// ---- resize(new_frame_count, t0, t1) ----
		AssetObj.set_function("resize", [Seq, &Session](sol::table /*self*/, int NewFrameCount, int T0, int T1, sol::this_state S) -> sol::object
			{
				sol::state_view Lua(S);
				if (!IsValid(Seq)) return sol::lua_nil;
				if (NewFrameCount <= 0)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] resize(%d, t0=%d, t1=%d) -> new frame count must be positive and non-zero"), NewFrameCount, T0, T1));
					return sol::lua_nil;
				}
				if (T0 < 0 || T1 < T0 || T1 > NewFrameCount)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] resize(%d, t0=%d, t1=%d) -> require 0 <= t0 <= t1 <= new_frame_count"), NewFrameCount, T0, T1));
					return sol::lua_nil;
				}
				IAnimationDataController& Ctrl = Seq->GetController();
				IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "Resize", "Resize Animation"));
				Ctrl.ResizeInFrames(FFrameNumber(NewFrameCount), FFrameNumber(T0), FFrameNumber(T1));
			Session.Log(FString::Printf(TEXT("[OK] resize(%d, t0=%d, t1=%d)"), NewFrameCount, T0, T1));
			return sol::make_object(Lua, true);
		});

		// ---- cleanup_bone_tracks() ----
		AssetObj.set_function("cleanup_bone_tracks", [Seq, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Seq)) return sol::lua_nil;
			USkeleton* Skeleton = Seq->GetSkeleton();
			if (!Skeleton) { Session.Log(TEXT("[FAIL] cleanup_bone_tracks() -> no skeleton assigned")); return sol::lua_nil; }

			IAnimationDataController& Ctrl = Seq->GetController();
			int32 TracksBefore = Ctrl.GetModel()->GetNumBoneTracks();
			IAnimationDataController::FScopedBracket Bracket(&Ctrl, NSLOCTEXT("AIK", "CleanupBoneTracks", "Cleanup Bone Tracks"));
			bool bOk = Ctrl.RemoveBoneTracksMissingFromSkeleton(Skeleton);
			int32 TracksAfter = Ctrl.GetModel()->GetNumBoneTracks();
			int32 Removed = TracksBefore - TracksAfter;
			Session.Log(FString::Printf(TEXT("[%s] cleanup_bone_tracks() -> %d tracks removed (%d remaining)"), bOk ? TEXT("OK") : TEXT("FAIL"), Removed, TracksAfter));
			return sol::make_object(Lua, Removed);
		});

		// ---- info() — override default ----
		AssetObj.set_function("info", [Seq, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Seq)) return sol::lua_nil;

			sol::table Result = Lua.create_table();
			Result["length"] = Seq->GetPlayLength();
			Result["frame_rate"] = Seq->GetSamplingFrameRate().AsDecimal();
			Result["num_frames"] = Seq->GetNumberOfSampledKeys();
			Result["notifies"] = Seq->Notifies.Num();
			Result["sync_markers"] = Seq->AuthoredSyncMarkers.Num();

			IAnimationDataController& Ctrl = Seq->GetController();
			const IAnimationDataModel* Model = Ctrl.GetModel();
			Result["bone_tracks"] = Model->GetNumBoneTracks();
			Result["float_curves"] = static_cast<int>(Model->GetFloatCurves().Num());
			Result["transform_curves"] = static_cast<int>(Model->GetTransformCurves().Num());
			Result["attributes"] = static_cast<int>(Model->GetAttributes().Num());

			Result["rate_scale"] = Seq->RateScale;
			Result["loop"] = Seq->bLoop;
			Result["root_motion"] = Seq->bEnableRootMotion;
			Result["force_root_lock"] = Seq->bForceRootLock;

			switch (Seq->Interpolation)
			{
			case EAnimInterpolationType::Step: Result["interpolation"] = "step"; break;
			default: Result["interpolation"] = "linear"; break;
			}

			switch (Seq->AdditiveAnimType)
			{
			case AAT_LocalSpaceBase: Result["additive_type"] = "local_space"; break;
			case AAT_RotationOffsetMeshSpace: Result["additive_type"] = "mesh_space"; break;
			default: Result["additive_type"] = "none"; break;
			}

			Session.Log(FString::Printf(TEXT("[OK] info() -> %.2fs, %d notifies, %d sync markers, %d bone tracks, %d curves, %d attributes"),
				Seq->GetPlayLength(), Seq->Notifies.Num(), Seq->AuthoredSyncMarkers.Num(),
				Model->GetNumBoneTracks(),
				Model->GetFloatCurves().Num(),
				Model->GetAttributes().Num()));
			return Result;
		});
	});
}

REGISTER_LUA_BINDING(AnimSequence, AnimSequenceDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindAnimSequence(Lua, Session);
});
