#include "Lua/LuaBindingRegistry.h"
#include "Lua/LuaPropertyHelper.h"
#include "Lua/LuaAssetResolver.h"
#include "Lua/LuaStr.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimTypes.h"
#include "Animation/Skeleton.h"
#include "Animation/AnimMetaData.h"
#include "Animation/BlendProfile.h"
#include "AlphaBlend.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

static UClass* FindNotifyClassForMontage(const FString& TypeName)
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

static void InitializeMontageNotifyForEditor(FAnimNotifyEvent& Event, UAnimNotify* Notify)
{
	if (Notify)
	{
		Notify->OnAnimNotifyCreatedInEditor(Event);
	}
}

static void InitializeMontageNotifyForEditor(FAnimNotifyEvent& Event, UAnimNotifyState* NotifyState)
{
	if (NotifyState)
	{
		NotifyState->OnAnimNotifyCreatedInEditor(Event);
	}
}

static bool CanPlaceMontageNotify(UAnimSequenceBase* Animation, UAnimNotify* Notify, FLuaSessionData& Session)
{
#if WITH_EDITOR
	if (Notify && !Notify->CanBePlaced(Animation))
	{
		Session.Log(FString::Printf(TEXT("[FAIL] add(\"notify\") -> notify class '%s' cannot be placed on '%s'"),
			*Notify->GetClass()->GetName(), *GetNameSafe(Animation)));
		return false;
	}
#endif
	return true;
}

static bool CanPlaceMontageNotify(UAnimSequenceBase* Animation, UAnimNotifyState* NotifyState, FLuaSessionData& Session)
{
#if WITH_EDITOR
	if (NotifyState && !NotifyState->CanBePlaced(Animation))
	{
		Session.Log(FString::Printf(TEXT("[FAIL] add(\"notify\") -> notify state class '%s' cannot be placed on '%s'"),
			*NotifyState->GetClass()->GetName(), *GetNameSafe(Animation)));
		return false;
	}
#endif
	return true;
}

static void ForceNativeBranchingPointTickType(FAnimNotifyEvent& Event)
{
	if ((Event.Notify && Event.Notify->bIsNativeBranchingPoint) ||
		(Event.NotifyStateClass && Event.NotifyStateClass->bIsNativeBranchingPoint))
	{
		Event.MontageTickType = EMontageNotifyTickType::BranchingPoint;
	}
}

// Set extra properties on a notify object from a Lua params table.
// Reserved keys are skipped; everything else gets set as a UPROPERTY via ImportText.
// For instanced sub-objects, pass the class name as the value to auto-instantiate.
static void SetMontageNotifyPropertiesFromParams(UObject* NotifyObj, const sol::table& Params, FLuaSessionData& Session)
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

// ---- Post-mutation helper ----
// Mirrors SAnimMontagePanel::SortAndUpdateMontage + SAnimNotifyPanel notify-change flow + OnMontageModified.
// Engine flow: SortAnimSegments → CollapseAnimSegments → UpdateLinkableElements → InvalidateRecursiveAsset →
//              CalculateSequenceLength/SetCompositeLength → sort CompositeSections → RefreshNotifyTriggerOffsets →
//              RefreshCacheData (sorts notifies + rebuilds branching point markers) → PostEditChange → MarkPackageDirty
static void MontagePostMutationUpdate(UAnimMontage* Montage)
{
	if (!Montage) return;

	// 1. Sort + collapse segments in all slot tracks (SAnimMontagePanel::SortAnimSegments + CollapseMontage)
	for (FSlotAnimationTrack& Slot : Montage->SlotAnimTracks)
	{
		Slot.AnimTrack.SortAnimSegments();
		Slot.AnimTrack.CollapseAnimSegments();
	}

	// 2. Update linkable element references
	Montage->UpdateLinkableElements();

	// 3. Recalculate sequence length (mirrors FAnimModel_AnimMontage::RecalculateSequenceLength)
	Montage->InvalidateRecursiveAsset();
	const float NewLength = Montage->CalculateSequenceLength();
	if (!FMath::IsNearlyEqual(NewLength, Montage->GetPlayLength(), UE_KINDA_SMALL_NUMBER))
	{
		Montage->SetCompositeLength(NewLength);
	}

	// 4. Sort composite sections by time
	Montage->CompositeSections.Sort([](const FCompositeSection& A, const FCompositeSection& B)
	{
		return A.GetTime() < B.GetTime();
	});

	// 5. Refresh notify trigger offsets (matches FAnimModel_AnimMontage::RefreshNotifyTriggerOffsets)
	for (FAnimNotifyEvent& Notify : Montage->Notifies)
	{
		EAnimEventTriggerOffsets::Type PredictedOffset = Montage->CalculateOffsetForNotify(Notify.GetTime());
		Notify.RefreshTriggerOffset(PredictedOffset);
		if (Notify.GetDuration() > 0.0f)
		{
			PredictedOffset = Montage->CalculateOffsetForNotify(Notify.GetTime() + Notify.GetDuration());
			Notify.RefreshEndTriggerOffset(PredictedOffset);
		}
		else
		{
			Notify.EndTriggerTimeOffset = 0.0f;
		}
	}

	// 6. Refresh cache — sorts notifies by time, rebuilds branching point markers, propagates to children
	//    (mirrors SAnimNotifyPanel notify-change flow: Sequence->RefreshCacheData())
	Montage->RefreshCacheData();

	// 7. Final callbacks (mirrors SAnimMontagePanel::OnMontageModified)
	Montage->PostEditChange();
	Montage->MarkPackageDirty();
}

// Ensure montage has a starting section at time 0 (mirrors UAnimMontageFactory::EnsureStartingSection)
static bool MontageEnsureStartingSection(UAnimMontage* Montage)
{
	bool bModified = false;
	if (Montage->CompositeSections.Num() <= 0)
	{
		FCompositeSection NewSection;
		NewSection.SetTime(0.0f);
		NewSection.SectionName = FName(TEXT("Default"));
		Montage->CompositeSections.Add(NewSection);
		bModified = true;
	}
	if (Montage->CompositeSections[0].GetTime() > 0.0f)
	{
		Montage->CompositeSections[0].SetTime(0.0f);
		bModified = true;
	}
	return bModified;
}

static void MontageEnsureNotifyTrack(UAnimMontage* Montage, int32& TrackIndex)
{
	if (!Montage)
	{
		TrackIndex = 0;
		return;
	}
	if (TrackIndex < 0 || TrackIndex > 20)
	{
		TrackIndex = 0;
	}
	while (!Montage->AnimNotifyTracks.IsValidIndex(TrackIndex))
	{
		const int32 NewIndex = Montage->AnimNotifyTracks.Num();
		Montage->AnimNotifyTracks.Add(FAnimNotifyTrack(*FString::FromInt(NewIndex + 1), FLinearColor::White));
	}
}

static FString FormatAlphaBlendOption(EAlphaBlendOption Opt)
{
	switch (Opt)
	{
	case EAlphaBlendOption::Linear: return TEXT("linear");
	case EAlphaBlendOption::Cubic: return TEXT("cubic");
	case EAlphaBlendOption::HermiteCubic: return TEXT("hermite_cubic");
	case EAlphaBlendOption::Sinusoidal: return TEXT("sinusoidal");
	case EAlphaBlendOption::QuadraticInOut: return TEXT("quadratic");
	case EAlphaBlendOption::CubicInOut: return TEXT("cubic_in_out");
	case EAlphaBlendOption::QuarticInOut: return TEXT("quartic");
	case EAlphaBlendOption::QuinticInOut: return TEXT("quintic");
	case EAlphaBlendOption::CircularIn: return TEXT("circular_in");
	case EAlphaBlendOption::CircularOut: return TEXT("circular_out");
	case EAlphaBlendOption::CircularInOut: return TEXT("circular");
	case EAlphaBlendOption::ExpIn: return TEXT("exp_in");
	case EAlphaBlendOption::ExpOut: return TEXT("exp_out");
	case EAlphaBlendOption::ExpInOut: return TEXT("exponential");
	case EAlphaBlendOption::Custom: return TEXT("custom");
	default: return TEXT("linear");
	}
}

static TArray<FLuaFunctionDoc> AnimMontageDocs = {};

static void BindAnimMontage(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_montage", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = NeoLuaStr::ToFString(PathStr);
		UAnimMontage* Montage = NeoLuaAsset::Resolve<UAnimMontage>(FPath);
		if (!Montage) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"Element types for add/remove/list/configure:\n"
			"  slot     — animation slot track\n"
			"  segment  — animation segment within a slot\n"
			"  section  — composite section (montage section marker)\n"
			"  notify   — animation notify event\n"
			"\n"
			"add(type, params):\n"
			"  add(\"slot\", {name=\"DefaultSlot\", animation=\"/Game/Anim\"})  — animation is optional\n"
			"  add(\"segment\", {slot=\"DefaultSlot\", animation=\"/Game/Anim\", start_pos=0, play_rate=1})\n"
			"  add(\"section\", {name=\"Attack\", time=0.5})\n"
			"  add(\"notify\", {name=\"Hit\", time=0.3, type=\"PlaySound\", duration=0.5})\n"
			"  add(\"notify\", {name=\"Trail\", time=0.2, type=\"AnimNotifyState_Trail\", duration=0.8, branching_point=true})\n"
			"\n"
			"remove(type, id):\n"
			"  remove(\"slot\", \"DefaultSlot\") — by slot name\n"
			"  remove(\"section\", \"Attack\")   — by section name\n"
			"  remove(\"segment\", {slot=\"DefaultSlot\", index=1})  — 1-based index within slot\n"
			"  remove(\"notify\", \"Hit\") or remove(\"notify\", 1)   — by name or 1-based index\n"
			"\n"
			"list(type):\n"
			"  list(\"slots\"), list(\"sections\"), list(\"notifies\"), list(\"segments\")\n"
			"\n"
			"configure(type, id, params):\n"
			"  configure(\"notify\", 1, {time=0.3, duration=0.5, trigger_chance=0.8})\n"
			"  configure(\"notify\", 1, {filter_type=\"LOD\", filter_lod=2, dedicated_server=false})\n"
			"  configure(\"segment\", {slot=\"DefaultSlot\", index=1}, {start_pos=0.5, play_rate=2})\n"
			"  configure(\"section\", \"Attack\", {time=1.0})\n"
			"  configure(\"montage\", nil, {blend_profile_in=\"MyProfile\", blend_profile_out=\"none\"})\n"
			"    — by-name lookup against owning skeleton; only kept on configure() because\n"
			"      the engine stores UBlendProfile as a sub-object of USkeleton, not as a\n"
			"      standalone asset. For every other montage property (RateScale, bLoop,\n"
			"      BlendIn.BlendTime, BlendIn.BlendOption, BlendModeIn/Out, SyncGroup,\n"
			"      TimeStretchCurveName, BlendOutTriggerTime, bEnableAutoBlendOut, …) use\n"
			"      asset:set(\"Prop\", value); list_properties() to discover.\n"
			"  configure(\"section\", \"Attack\", {add_metadata=\"CurveSourceMetaData\"})\n"
			"  configure(\"section\", \"Attack\", {remove_metadata=1})  — 1-based index\n"
			"\n"
			"Action methods:\n"
			"  link_sections(from_section, to_section) — set next section flow\n"
			"  info() — summary of slots, sections, notifies, length, blend settings\n";

		// ---- add(type, params) ----
		AssetObj.set_function("add", [Montage, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = NeoLuaStr::ToFString(Type);

			if (FType.Equals(TEXT("slot"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"slot\") -> {name=..} required")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string SlotName = P.get_or<std::string>("name", "");
				if (SlotName.empty()) { Session.Log(TEXT("[FAIL] add(\"slot\") -> name required")); return sol::lua_nil; }

				FString SlotStr = NeoLuaStr::ToFString(SlotName);
				FName Slot(SlotStr);
				// Don't use IsValidSlot() — it returns false for empty slots (no AnimSegments).
				// Match engine's CanCreateNewSlot() which checks by name only.
				bool bSlotExists = false;
				for (const FSlotAnimationTrack& Track : Montage->SlotAnimTracks)
				{
					if (Track.SlotName == Slot) { bSlotExists = true; break; }
				}
				if (bSlotExists) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"slot\") -> '%s' already exists"), *Slot.ToString())); return sol::lua_nil; }

				// Register slot on skeleton BEFORE adding to montage (engine flow: SAnimMontagePanel)
				USkeleton* Skeleton = Montage->GetSkeleton();
				if (Skeleton)
				{
					Skeleton->RegisterSlotNode(Slot);
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "AddSlot", "Add Slot"));
				Montage->Modify();
				FSlotAnimationTrack& NewSlot = Montage->AddSlot(Slot);

				std::string AnimPath = P.get_or<std::string>("animation", "");
				if (!AnimPath.empty())
				{
					FString FAnim = NeoLuaStr::ToFString(AnimPath);
					UAnimSequenceBase* Anim = NeoLuaAsset::Resolve<UAnimSequenceBase>(FAnim);
					if (Anim)
					{
						FAnimSegment Seg;
						Seg.SetAnimReference(Anim, true);
						Seg.AnimStartTime = 0.0f;
						Seg.AnimEndTime = Anim->GetPlayLength();
						Seg.AnimPlayRate = 1.0f;
						Seg.LoopingCount = 1;
						Seg.StartPos = 0.0f;
						NewSlot.AnimTrack.AnimSegments.Add(Seg);
					}
				}

				MontagePostMutationUpdate(Montage);
				Session.Log(FString::Printf(TEXT("[OK] add(\"slot\", name=\"%s\")"), *Slot.ToString()));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("segment"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"segment\") -> {slot=.., animation=..} required")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string SlotName = P.get_or<std::string>("slot", "");
				std::string AnimPath = P.get_or<std::string>("animation", "");
				if (SlotName.empty() || AnimPath.empty()) { Session.Log(TEXT("[FAIL] add(\"segment\") -> slot and animation required")); return sol::lua_nil; }

				FString SlotStr = NeoLuaStr::ToFString(SlotName);
				FName Slot(SlotStr);
				FSlotAnimationTrack* Track = nullptr;
				for (FSlotAnimationTrack& T : Montage->SlotAnimTracks)
					if (T.SlotName == Slot) { Track = &T; break; }
				if (!Track) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"segment\") -> slot '%s' not found"), *Slot.ToString())); return sol::lua_nil; }

				FString FAnim = NeoLuaStr::ToFString(AnimPath);
				UAnimSequenceBase* Anim = NeoLuaAsset::Resolve<UAnimSequenceBase>(FAnim);
				if (!Anim) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"segment\") -> animation not found: %s"), *FAnim)); return sol::lua_nil; }

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "AddSegment", "Add Segment"));
				Montage->Modify();
				FAnimSegment Seg;
				Seg.SetAnimReference(Anim, true);
				Seg.AnimStartTime = 0.0f;
				Seg.AnimEndTime = Anim->GetPlayLength();
				Seg.AnimPlayRate = 1.0f;
				Seg.LoopingCount = 1;

				float LastEnd = 0.0f;
				for (const FAnimSegment& Existing : Track->AnimTrack.AnimSegments)
				{
					float End = Existing.StartPos + Existing.GetLength();
					if (End > LastEnd) LastEnd = End;
				}
				Seg.StartPos = LastEnd;

				Seg.StartPos = static_cast<float>(P.get_or("start_pos", static_cast<double>(Seg.StartPos)));
				Seg.AnimPlayRate = static_cast<float>(P.get_or("play_rate", 1.0));
				Seg.LoopingCount = P.get_or("loops", 1);
				Seg.AnimStartTime = static_cast<float>(P.get_or("anim_start", 0.0));
				Seg.AnimEndTime = static_cast<float>(P.get_or("anim_end", static_cast<double>(Seg.AnimEndTime)));

				Track->AnimTrack.AnimSegments.Add(Seg);
				MontagePostMutationUpdate(Montage);
				Session.Log(FString::Printf(TEXT("[OK] add(\"segment\", slot=\"%s\", anim=\"%s\")"), *Slot.ToString(), *Anim->GetName()));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("section"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"section\") -> {name=.., time=..} required")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string SectionName = P.get_or<std::string>("name", "");
				double Time = P.get_or("time", 0.0);
				if (SectionName.empty()) { Session.Log(TEXT("[FAIL] add(\"section\") -> name required")); return sol::lua_nil; }

				FString SectionStr = NeoLuaStr::ToFString(SectionName);
				FName Section(SectionStr);
				Montage->Modify();
				int32 Idx = Montage->AddAnimCompositeSection(Section, static_cast<float>(Time));
				if (Idx == INDEX_NONE) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"section\") -> '%s' already exists"), *Section.ToString())); return sol::lua_nil; }
				MontagePostMutationUpdate(Montage);
				Session.Log(FString::Printf(TEXT("[OK] add(\"section\", name=\"%s\", time=%.2f)"), *Section.ToString(), Time));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("notify"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"notify\") -> {name=.., time=..} required")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string Name = P.get_or<std::string>("name", "");
				double Time = P.get_or("time", 0.0);
				if (Name.empty()) { Session.Log(TEXT("[FAIL] add(\"notify\") -> name required")); return sol::lua_nil; }

				FString NotifyNameStr = NeoLuaStr::ToFString(Name);
				float PlayLength = Montage->GetPlayLength();
				if (Time < 0.0 || Time > PlayLength)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"notify\") -> time %.2f out of range (0-%.2f)"), Time, PlayLength));
					return sol::lua_nil;
				}

				FAnimNotifyEvent NewEvent;
				NewEvent.NotifyName = FName(*NotifyNameStr);
				NewEvent.Link(Montage, static_cast<float>(Time));
				NewEvent.TriggerTimeOffset = GetTriggerTimeOffsetForType(Montage->CalculateOffsetForNotify(static_cast<float>(Time)));
				NewEvent.TrackIndex = static_cast<int32>(P.get_or("track", 0.0));
				NewEvent.Guid = FGuid::NewGuid();
				NewEvent.TriggerWeightThreshold = ZERO_ANIMWEIGHT_THRESH;
				UObject* CreatedNotify = nullptr;

				std::string TypeStr = P.get_or<std::string>("type", "");
				if (!TypeStr.empty())
				{
					FString FTypeStr = NeoLuaStr::ToFString(TypeStr);
					UClass* NotifyClass = FindNotifyClassForMontage(FTypeStr);
					if (NotifyClass)
					{
						if (NotifyClass->IsChildOf(UAnimNotifyState::StaticClass()))
						{
							UAnimNotifyState* NS = NewObject<UAnimNotifyState>(Montage, NotifyClass, NAME_None, RF_Transactional);
							if (!CanPlaceMontageNotify(Montage, NS, Session)) return sol::lua_nil;
							InitializeMontageNotifyForEditor(NewEvent, NS);
							NewEvent.NotifyStateClass = NS;
							CreatedNotify = NS;
							double Dur = P.get_or("duration", 0.5);
							NewEvent.Duration = static_cast<float>(Dur);
							NewEvent.EndLink.Link(Montage, static_cast<float>(Time + Dur));
						}
						else
						{
							UAnimNotify* N = NewObject<UAnimNotify>(Montage, NotifyClass, NAME_None, RF_Transactional);
							if (!CanPlaceMontageNotify(Montage, N, Session)) return sol::lua_nil;
							InitializeMontageNotifyForEditor(NewEvent, N);
							NewEvent.Notify = N;
							CreatedNotify = N;
						}
					}
				}

				if (P.get_or("branching_point", false))
					NewEvent.MontageTickType = EMontageNotifyTickType::BranchingPoint;
				ForceNativeBranchingPointTickType(NewEvent);

				sol::optional<double> TriggerChance = P.get<sol::optional<double>>("trigger_chance");
				if (TriggerChance.has_value())
					NewEvent.NotifyTriggerChance = FMath::Clamp(static_cast<float>(TriggerChance.value()), 0.0f, 1.0f);

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "AddMontageNotify", "Add Montage Notify"));
				Montage->Modify();

				// Set extra properties / instanced sub-objects on the notify
				if (CreatedNotify)
				{
					CreatedNotify->Modify();
					SetMontageNotifyPropertiesFromParams(CreatedNotify, P, Session);
				}

				MontageEnsureNotifyTrack(Montage, NewEvent.TrackIndex);
				Montage->Notifies.Add(NewEvent);
				MontagePostMutationUpdate(Montage);
				Session.Log(FString::Printf(TEXT("[OK] add(\"notify\", name=\"%s\", time=%.2f)"), *NotifyNameStr, Time));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: slot, segment, section, notify"), *FType));
			return sol::lua_nil;
		});

		// ---- remove(type, id) ----
		AssetObj.set_function("remove", [Montage, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = NeoLuaStr::ToFString(Type);

			if (FType.Equals(TEXT("slot"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<std::string>()) { Session.Log(TEXT("[FAIL] remove(\"slot\") -> name required")); return sol::lua_nil; }
				FString SlotStr = NeoLuaStr::ToFString(Id.as<std::string>());
				FName Slot(SlotStr);
				const FScopedTransaction Tx(NSLOCTEXT("AIK", "RemSlot", "Remove Slot"));
				Montage->Modify();
				for (int32 i = 0; i < Montage->SlotAnimTracks.Num(); i++)
				{
					if (Montage->SlotAnimTracks[i].SlotName == Slot)
					{
						Montage->SlotAnimTracks.RemoveAt(i);
						MontagePostMutationUpdate(Montage);
						Session.Log(FString::Printf(TEXT("[OK] remove(\"slot\", \"%s\")"), *Slot.ToString()));
						return sol::make_object(Lua, true);
					}
				}
				Session.Log(FString::Printf(TEXT("[FAIL] remove(\"slot\") -> '%s' not found"), *Slot.ToString()));
				return sol::lua_nil;
			}
			else if (FType.Equals(TEXT("section"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<std::string>()) { Session.Log(TEXT("[FAIL] remove(\"section\") -> name required")); return sol::lua_nil; }
				FString SectionStr = NeoLuaStr::ToFString(Id.as<std::string>());
				FName Section(SectionStr);
				int32 Idx = Montage->GetSectionIndex(Section);
				if (Idx == INDEX_NONE)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"section\") -> '%s' not found"), *Section.ToString()));
					return sol::lua_nil;
				}
				const FScopedTransaction Tx(NSLOCTEXT("AIK", "RemSection", "Remove Section"));
				Montage->Modify();

				// Unlink references pointing to the section being removed (engine: SAnimMontagePanel::RemoveSection)
				const FName SectionNameToRemove = Montage->CompositeSections[Idx].SectionName;
				for (FCompositeSection& CS : Montage->CompositeSections)
				{
					if (CS.NextSectionName == SectionNameToRemove)
					{
						CS.NextSectionName = NAME_None;
					}
				}

				Montage->CompositeSections.RemoveAt(Idx);
				MontageEnsureStartingSection(Montage);
				MontagePostMutationUpdate(Montage);
				Session.Log(FString::Printf(TEXT("[OK] remove(\"section\", \"%s\")"), *Section.ToString()));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("segment"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<sol::table>()) { Session.Log(TEXT("[FAIL] remove(\"segment\") -> {slot=.., index=..} required")); return sol::lua_nil; }
				sol::table P = Id.as<sol::table>();
				std::string SlotName = P.get_or<std::string>("slot", "");
				int32 SegIdx = static_cast<int32>(P.get_or("index", 0.0)) - 1;
				if (SlotName.empty()) { Session.Log(TEXT("[FAIL] remove(\"segment\") -> slot required")); return sol::lua_nil; }

				FString SlotStr = NeoLuaStr::ToFString(SlotName);
				FName Slot(SlotStr);
				FSlotAnimationTrack* Track = nullptr;
				for (FSlotAnimationTrack& T : Montage->SlotAnimTracks)
					if (T.SlotName == Slot) { Track = &T; break; }
				if (!Track) { Session.Log(FString::Printf(TEXT("[FAIL] remove(\"segment\") -> slot '%s' not found"), *Slot.ToString())); return sol::lua_nil; }
				if (SegIdx < 0 || SegIdx >= Track->AnimTrack.AnimSegments.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"segment\") -> index %d out of range (1-%d)"), SegIdx + 1, Track->AnimTrack.AnimSegments.Num()));
					return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "RemSegment", "Remove Segment"));
				Montage->Modify();
				Track->AnimTrack.AnimSegments.RemoveAt(SegIdx);
				MontagePostMutationUpdate(Montage);
				Session.Log(FString::Printf(TEXT("[OK] remove(\"segment\", slot=\"%s\", index=%d)"), *Slot.ToString(), SegIdx + 1));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("notify"), ESearchCase::IgnoreCase))
			{
				if (!IsValid(Montage)) return sol::lua_nil;
				int32 IdxToRemove = INDEX_NONE;
				if (Id.is<int>())
				{
					IdxToRemove = Id.as<int>() - 1;
				}
				else if (Id.is<std::string>())
				{
					FString Target = NeoLuaStr::ToFString(Id.as<std::string>());
					for (int32 i = 0; i < Montage->Notifies.Num(); i++)
					{
						const FAnimNotifyEvent& Evt = Montage->Notifies[i];
						FString EvtName;
						if (Evt.Notify) EvtName = Evt.Notify->GetNotifyName();
						else if (Evt.NotifyStateClass) EvtName = Evt.NotifyStateClass->GetNotifyName();
						else EvtName = Evt.NotifyName.ToString();
						if (EvtName.Equals(Target, ESearchCase::IgnoreCase)) { IdxToRemove = i; break; }
					}
				}
				if (IdxToRemove < 0 || IdxToRemove >= Montage->Notifies.Num())
				{
					Session.Log(TEXT("[FAIL] remove(\"notify\") -> not found")); return sol::lua_nil;
				}
				FString Removed = Montage->Notifies[IdxToRemove].NotifyName.ToString();
				const FScopedTransaction Tx(NSLOCTEXT("AIK", "RemMontageNotify", "Remove Montage Notify"));
				Montage->Modify();
				Montage->Notifies.RemoveAt(IdxToRemove);
				MontagePostMutationUpdate(Montage);
				Session.Log(FString::Printf(TEXT("[OK] remove(\"notify\", \"%s\")"), *Removed));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type. Valid: slot, section, segment, notify"), *FType));
			return sol::lua_nil;
		});

		// ---- list(type?) ----
		AssetObj.set_function("list", [Montage, &Session](sol::table self,
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

			if (FType.Contains(TEXT("slot"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Montage->SlotAnimTracks.Num(); i++)
				{
					const FSlotAnimationTrack& Slot = Montage->SlotAnimTracks[i];
					sol::table Entry = Lua.create_table();
					Entry["name"] = TCHAR_TO_UTF8(*Slot.SlotName.ToString());
					Entry["segments"] = static_cast<int>(Slot.AnimTrack.AnimSegments.Num());
					Entry["length"] = Slot.AnimTrack.GetLength();
					Result[i + 1] = Entry;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"slots\") -> %d"), Montage->SlotAnimTracks.Num()));
				return Result;
			}

			if (FType.Contains(TEXT("section"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Montage->CompositeSections.Num(); i++)
				{
					const FCompositeSection& Sec = Montage->CompositeSections[i];
					sol::table Entry = Lua.create_table();
					Entry["name"] = TCHAR_TO_UTF8(*Sec.SectionName.ToString());
					Entry["next"] = TCHAR_TO_UTF8(*Sec.NextSectionName.ToString());
					Entry["time"] = Sec.GetTime();
					if (Sec.MetaData.Num() > 0)
					{
						sol::table MetaArr = Lua.create_table();
						for (int32 m = 0; m < Sec.MetaData.Num(); m++)
						{
							if (Sec.MetaData[m])
								MetaArr[m + 1] = TCHAR_TO_UTF8(*Sec.MetaData[m]->GetClass()->GetName());
						}
						Entry["metadata"] = MetaArr;
					}
					Result[i + 1] = Entry;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"sections\") -> %d"), Montage->CompositeSections.Num()));
				return Result;
			}

			if (FType.Contains(TEXT("notif"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Montage->Notifies.Num(); i++)
				{
					const FAnimNotifyEvent& E = Montage->Notifies[i];
					sol::table Entry = Lua.create_table();
					Entry["index"] = i + 1;
					Entry["name"] = TCHAR_TO_UTF8(*E.NotifyName.ToString());
					Entry["time"] = E.GetTriggerTime();
					Entry["duration"] = E.GetDuration();
					Entry["track"] = E.TrackIndex;
					Entry["trigger_chance"] = E.NotifyTriggerChance;
					Entry["branching_point"] = (E.MontageTickType == EMontageNotifyTickType::BranchingPoint);
					if (E.Notify)
						Entry["type"] = TCHAR_TO_UTF8(*E.Notify->GetClass()->GetName());
					else if (E.NotifyStateClass)
						Entry["type"] = TCHAR_TO_UTF8(*E.NotifyStateClass->GetClass()->GetName());
					Result[i + 1] = Entry;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"notifies\") -> %d"), Montage->Notifies.Num()));
				return Result;
			}

			if (FType.Contains(TEXT("segment"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				int32 GlobalIdx = 1;
				for (int32 s = 0; s < Montage->SlotAnimTracks.Num(); s++)
				{
					const FSlotAnimationTrack& Slot = Montage->SlotAnimTracks[s];
					for (int32 i = 0; i < Slot.AnimTrack.AnimSegments.Num(); i++)
					{
						const FAnimSegment& Seg = Slot.AnimTrack.AnimSegments[i];
						sol::table Entry = Lua.create_table();
						Entry["slot"] = TCHAR_TO_UTF8(*Slot.SlotName.ToString());
						Entry["index"] = i + 1;
						Entry["start_pos"] = Seg.StartPos;
						Entry["length"] = Seg.GetLength();
						Entry["play_rate"] = Seg.AnimPlayRate;
						Entry["loops"] = Seg.LoopingCount;
						Entry["anim_start"] = Seg.AnimStartTime;
						Entry["anim_end"] = Seg.AnimEndTime;
						UAnimSequenceBase* AnimRef = Seg.GetAnimReference();
						Entry["animation"] = AnimRef ? TCHAR_TO_UTF8(*AnimRef->GetPathName()) : "";
						Result[GlobalIdx++] = Entry;
					}
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"segments\") -> %d"), GlobalIdx - 1));
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: slots, sections, notifies, segments"), *FType));
			return sol::lua_nil;
		});

		// ---- configure(type, id, params) ----
		AssetObj.set_function("configure", [Montage, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Montage)) return sol::lua_nil;
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
					for (int32 i = 0; i < Montage->Notifies.Num(); i++)
					{
						if (Montage->Notifies[i].NotifyName.ToString().Equals(Target, ESearchCase::IgnoreCase)) { Idx = i; break; }
					}
				}
				if (Idx < 0 || Idx >= Montage->Notifies.Num())
				{
					Session.Log(TEXT("[FAIL] configure(\"notify\") -> not found")); return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "ConfigMontageNotify", "Configure Montage Notify"));
				Montage->Modify();
				FAnimNotifyEvent& Evt = Montage->Notifies[Idx];

				sol::optional<double> NewTime = Params.get<sol::optional<double>>("time");
				if (NewTime.has_value())
				{
					float T = static_cast<float>(NewTime.value());
					Evt.Link(Montage, T);
					Evt.TriggerTimeOffset = GetTriggerTimeOffsetForType(Montage->CalculateOffsetForNotify(T));
				}

				sol::optional<double> NewDuration = Params.get<sol::optional<double>>("duration");
				if (NewDuration.has_value() && Evt.NotifyStateClass)
				{
					Evt.Duration = static_cast<float>(NewDuration.value());
					Evt.EndLink.Link(Montage, Evt.GetTriggerTime() + Evt.Duration);
				}

				sol::optional<double> NewTrack = Params.get<sol::optional<double>>("track");
				if (NewTrack.has_value())
				{
					Evt.TrackIndex = static_cast<int32>(NewTrack.value());
					MontageEnsureNotifyTrack(Montage, Evt.TrackIndex);
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
				ForceNativeBranchingPointTickType(Evt);

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

				MontagePostMutationUpdate(Montage);
				Session.Log(FString::Printf(TEXT("[OK] configure(\"notify\", %d)"), Idx + 1));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("segment"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<sol::table>()) { Session.Log(TEXT("[FAIL] configure(\"segment\") -> {slot=.., index=..} required")); return sol::lua_nil; }
				sol::table IdT = Id.as<sol::table>();
				std::string SlotName = IdT.get_or<std::string>("slot", "");
				int32 SegIdx = static_cast<int32>(IdT.get_or("index", 0.0)) - 1;
				if (SlotName.empty()) { Session.Log(TEXT("[FAIL] configure(\"segment\") -> slot required")); return sol::lua_nil; }

				FString SlotStr = NeoLuaStr::ToFString(SlotName);
				FName Slot(SlotStr);
				FSlotAnimationTrack* Track = nullptr;
				for (FSlotAnimationTrack& T : Montage->SlotAnimTracks)
					if (T.SlotName == Slot) { Track = &T; break; }
				if (!Track) { Session.Log(FString::Printf(TEXT("[FAIL] configure(\"segment\") -> slot '%s' not found"), *Slot.ToString())); return sol::lua_nil; }
				if (SegIdx < 0 || SegIdx >= Track->AnimTrack.AnimSegments.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"segment\") -> index %d out of range"), SegIdx + 1)); return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "ConfigSegment", "Configure Segment"));
				Montage->Modify();
				FAnimSegment& Seg = Track->AnimTrack.AnimSegments[SegIdx];

				sol::optional<double> StartPos = Params.get<sol::optional<double>>("start_pos");
				if (StartPos.has_value()) Seg.StartPos = static_cast<float>(StartPos.value());

				sol::optional<double> PlayRate = Params.get<sol::optional<double>>("play_rate");
				if (PlayRate.has_value()) Seg.AnimPlayRate = static_cast<float>(PlayRate.value());

				sol::optional<double> Loops = Params.get<sol::optional<double>>("loops");
				if (Loops.has_value()) Seg.LoopingCount = FMath::Max(1, static_cast<int32>(Loops.value()));

				sol::optional<double> AnimStart = Params.get<sol::optional<double>>("anim_start");
				if (AnimStart.has_value()) Seg.AnimStartTime = static_cast<float>(AnimStart.value());

				sol::optional<double> AnimEnd = Params.get<sol::optional<double>>("anim_end");
				if (AnimEnd.has_value()) Seg.AnimEndTime = static_cast<float>(AnimEnd.value());

				std::string AnimPath = Params.get_or<std::string>("animation", "");
				if (!AnimPath.empty())
				{
					FString FAnim = NeoLuaStr::ToFString(AnimPath);
					UAnimSequenceBase* Anim = NeoLuaAsset::Resolve<UAnimSequenceBase>(FAnim);
					if (Anim) Seg.SetAnimReference(Anim, false);
				}

				MontagePostMutationUpdate(Montage);
				Session.Log(FString::Printf(TEXT("[OK] configure(\"segment\", slot=\"%s\", index=%d)"), *Slot.ToString(), SegIdx + 1));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("section"), ESearchCase::IgnoreCase))
			{
				FString SectionStr;
				if (Id.is<std::string>()) SectionStr = NeoLuaStr::ToFString(Id.as<std::string>());
				else if (Id.is<int>())
				{
					int32 SIdx = Id.as<int>() - 1;
					if (SIdx >= 0 && SIdx < Montage->CompositeSections.Num())
						SectionStr = Montage->CompositeSections[SIdx].SectionName.ToString();
				}
				if (SectionStr.IsEmpty()) { Session.Log(TEXT("[FAIL] configure(\"section\") -> section name or 1-based index required")); return sol::lua_nil; }

				FName SectionName(SectionStr);
				int32 Idx = Montage->GetSectionIndex(SectionName);
				if (Idx == INDEX_NONE) { Session.Log(FString::Printf(TEXT("[FAIL] configure(\"section\") -> '%s' not found"), *SectionStr)); return sol::lua_nil; }

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "ConfigSection", "Configure Section"));
				Montage->Modify();
				FCompositeSection& Sec = Montage->GetAnimCompositeSection(Idx);

				sol::optional<double> NewTime = Params.get<sol::optional<double>>("time");
				if (NewTime.has_value())
				{
					float T = static_cast<float>(NewTime.value());
					Sec.SetTime(T);  // Engine: SetTime THEN Link
					Sec.Link(Montage, T);
				}

				sol::optional<std::string> NextSection = Params.get<sol::optional<std::string>>("next");
				if (NextSection.has_value())
				{
					FString NextStr = NeoLuaStr::ToFStringOpt(NextSection);
					Sec.NextSectionName = NextStr.IsEmpty() ? NAME_None : ::FName(NextStr);
				}

				// Section metadata: add_metadata / remove_metadata
				sol::optional<std::string> AddMeta = Params.get<sol::optional<std::string>>("add_metadata");
				if (AddMeta.has_value())
				{
					FString MetaClassName = NeoLuaStr::ToFStringOpt(AddMeta);
					UClass* MetaClass = nullptr;
					for (TObjectIterator<UClass> It; It; ++It)
					{
						if ((*It)->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated)) continue;
						if (!(*It)->IsChildOf(UAnimMetaData::StaticClass())) continue;
						if ((*It)->GetName().Equals(MetaClassName, ESearchCase::IgnoreCase) ||
							(*It)->GetName().Equals(TEXT("AnimMetaData_") + MetaClassName, ESearchCase::IgnoreCase))
						{
							MetaClass = *It;
							break;
						}
					}
					if (MetaClass)
					{
						UAnimMetaData* NewMeta = NewObject<UAnimMetaData>(Montage, MetaClass, NAME_None, RF_Transactional);
						Sec.MetaData.Add(NewMeta);
						Session.Log(FString::Printf(TEXT("[OK] added metadata %s to section \"%s\""), *MetaClass->GetName(), *SectionStr));
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] metadata class \"%s\" not found"), *MetaClassName));
					}
				}

				sol::optional<double> RemoveMeta = Params.get<sol::optional<double>>("remove_metadata");
				if (RemoveMeta.has_value())
				{
					int32 MetaIdx = static_cast<int32>(RemoveMeta.value()) - 1;
					if (MetaIdx >= 0 && MetaIdx < Sec.MetaData.Num())
					{
						Sec.MetaData.RemoveAt(MetaIdx);
						Session.Log(FString::Printf(TEXT("[OK] removed metadata at index %d from section \"%s\""), MetaIdx + 1, *SectionStr));
					}
				}

				MontagePostMutationUpdate(Montage);
				Session.Log(FString::Printf(TEXT("[OK] configure(\"section\", \"%s\")"), *SectionStr));
				return sol::make_object(Lua, true);
			}
			else if (FType.Equals(TEXT("montage"), ESearchCase::IgnoreCase))
			{
				// Only the blend_profile_in/out convenience lookup survives — BlendProfile is a
				// sub-object of the owning Skeleton, not a separate asset, so name-based lookup
				// is friendlier than constructing the full outer-path string agents would need
				// for asset:set. All plain UPROPERTY fields (RateScale, bLoop, SyncGroup,
				// BlendIn/BlendOut struct fields, BlendModeIn/Out enum, TimeStretchCurveName,
				// etc.) are handled by the generic asset:set(name, value) path.
				const FScopedTransaction Tx(NSLOCTEXT("AIK", "ConfigMontage", "Configure Montage"));
				Montage->Modify();

				USkeleton* Skel = Montage->GetSkeleton();
				int32 Applied = 0;

				sol::optional<std::string> BPIn = Params.get<sol::optional<std::string>>("blend_profile_in");
				if (BPIn.has_value())
				{
					FString ProfName = NeoLuaStr::ToFStringOpt(BPIn);
					if (ProfName.IsEmpty() || ProfName.Equals(TEXT("none"), ESearchCase::IgnoreCase))
					{
						Montage->BlendProfileIn = nullptr; ++Applied;
					}
					else if (Skel)
					{
						if (UBlendProfile* Prof = Skel->GetBlendProfile(FName(ProfName)))
						{
							Montage->BlendProfileIn = Prof; ++Applied;
						}
						else Session.Log(FString::Printf(TEXT("[WARN] blend_profile_in \"%s\" not found on skeleton"), *ProfName));
					}
				}

				sol::optional<std::string> BPOut = Params.get<sol::optional<std::string>>("blend_profile_out");
				if (BPOut.has_value())
				{
					FString ProfName = NeoLuaStr::ToFStringOpt(BPOut);
					if (ProfName.IsEmpty() || ProfName.Equals(TEXT("none"), ESearchCase::IgnoreCase))
					{
						Montage->BlendProfileOut = nullptr; ++Applied;
					}
					else if (Skel)
					{
						if (UBlendProfile* Prof = Skel->GetBlendProfile(FName(ProfName)))
						{
							Montage->BlendProfileOut = Prof; ++Applied;
						}
						else Session.Log(FString::Printf(TEXT("[WARN] blend_profile_out \"%s\" not found on skeleton"), *ProfName));
					}
				}

				if (Applied > 0)
				{
					Montage->RefreshCacheData();
					Montage->PostEditChange();
					Montage->MarkPackageDirty();
				}
				Session.Log(FString::Printf(TEXT("[OK] configure(\"montage\") -> %d field(s) applied"), Applied));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown type. Valid: notify, segment, section, montage"), *FType));
			return sol::lua_nil;
		});

		// ---- Action: link_sections ----
		AssetObj.set_function("link_sections", [Montage, &Session](sol::table /*self*/,
			const std::string& From, const std::string& To, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Montage)) return sol::lua_nil;
			FString FromStr = NeoLuaStr::ToFString(From);
			FName FromName(FromStr);
			FString ToStr = NeoLuaStr::ToFString(To);
			FName ToName = To.empty() ? NAME_None : FName(ToStr);

			int32 Idx = Montage->GetSectionIndex(FromName);
			if (Idx == INDEX_NONE) { Session.Log(TEXT("[FAIL] link_sections -> section not found")); return sol::lua_nil; }
			if (ToName != NAME_None && Montage->GetSectionIndex(ToName) == INDEX_NONE)
			{
				Session.Log(TEXT("[FAIL] link_sections -> next_section not found")); return sol::lua_nil;
			}

			Montage->Modify();
			FCompositeSection& Section = Montage->GetAnimCompositeSection(Idx);
			Section.NextSectionName = ToName;
			Montage->PostEditChange();
			Montage->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] link_sections(\"%s\" -> \"%s\")"), *FromName.ToString(), *ToName.ToString()));
			return sol::make_object(Lua, true);
		});

		// ---- info() — override default ----
		AssetObj.set_function("info", [Montage, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Montage)) return sol::lua_nil;

			sol::table Result = Lua.create_table();
			Result["length"] = Montage->GetPlayLength();
			Result["slots"] = Montage->SlotAnimTracks.Num();
			Result["sections"] = Montage->CompositeSections.Num();
			Result["notifies"] = Montage->Notifies.Num();
			Result["blend_in"] = Montage->BlendIn.GetBlendTime();
			Result["blend_out"] = Montage->BlendOut.GetBlendTime();
			Result["blend_option_in"] = TCHAR_TO_UTF8(*FormatAlphaBlendOption(Montage->BlendIn.GetBlendOption()));
			Result["blend_option_out"] = TCHAR_TO_UTF8(*FormatAlphaBlendOption(Montage->BlendOut.GetBlendOption()));
			Result["blend_out_trigger_time"] = Montage->BlendOutTriggerTime;
			Result["auto_blend_out"] = Montage->bEnableAutoBlendOut;
			Result["rate_scale"] = Montage->RateScale;
			Result["loop"] = Montage->bLoop;

			switch (Montage->BlendModeIn)
			{
			case EMontageBlendMode::Inertialization: Result["blend_mode_in"] = "inertialization"; break;
			default: Result["blend_mode_in"] = "standard"; break;
			}
			switch (Montage->BlendModeOut)
			{
			case EMontageBlendMode::Inertialization: Result["blend_mode_out"] = "inertialization"; break;
			default: Result["blend_mode_out"] = "standard"; break;
			}

			if (Montage->SyncGroup != NAME_None)
				Result["sync_group"] = TCHAR_TO_UTF8(*Montage->SyncGroup.ToString());

			Result["sync_slot_index"] = Montage->SyncSlotIndex;

			FName GroupName = Montage->GetGroupName();
			if (GroupName != NAME_None)
				Result["group_name"] = TCHAR_TO_UTF8(*GroupName.ToString());

			if (Montage->TimeStretchCurveName != NAME_None)
				Result["time_stretch_curve_name"] = TCHAR_TO_UTF8(*Montage->TimeStretchCurveName.ToString());

			if (Montage->BlendProfileIn)
				Result["blend_profile_in"] = TCHAR_TO_UTF8(*Montage->BlendProfileIn->GetName());
			if (Montage->BlendProfileOut)
				Result["blend_profile_out"] = TCHAR_TO_UTF8(*Montage->BlendProfileOut->GetName());

			Session.Log(FString::Printf(TEXT("[OK] info() -> %.2fs, %d slots, %d sections, %d notifies"),
				Montage->GetPlayLength(), Montage->SlotAnimTracks.Num(), Montage->CompositeSections.Num(), Montage->Notifies.Num()));
			return Result;
		});
	});
}

REGISTER_LUA_BINDING(AnimMontage, AnimMontageDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindAnimMontage(Lua, Session);
});
