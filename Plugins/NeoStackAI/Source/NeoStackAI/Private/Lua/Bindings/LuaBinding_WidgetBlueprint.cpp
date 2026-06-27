// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Lua/LuaPropertyHelper.h"
#include "Lua/LuaAssetResolver.h"
#include "Lua/LuaStr.h"
#include "Tools/NeoStackToolUtils.h"
#include "Utils/NeoTypeResolver.h"

#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/PanelSlot.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/BorderSlot.h"
#include "Components/ButtonSlot.h"
#include "Components/GridSlot.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/OverlaySlot.h"
#include "Components/ScrollBoxSlot.h"
#include "Components/SizeBoxSlot.h"
#include "Components/UniformGridSlot.h"
#include "Components/TextBlock.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "ScopedTransaction.h"
#include "Factories.h"
#include "Animation/WidgetAnimation.h"
#include "Animation/WidgetAnimationBinding.h"
#include "Components/NamedSlot.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "WidgetBlueprintEditorUtils.h"
#include "MovieScene.h"
#include "MovieSceneTrack.h"
#include "MovieSceneSection.h"
#include "Blueprint/WidgetNavigation.h"
#include "BaseWidgetBlueprint.h"
#include "Engine/Font.h"
#include "Engine/Texture2D.h"
#include "Fonts/SlateFontInfo.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Guid.h"
#include "UObject/UnrealType.h"

#include <functional>
#include <initializer_list>

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// Helpers
// ============================================================================

namespace WidgetHelper
{

class FWidgetImportPreflightFactory : public FCustomizableTextObjectFactory
{
public:
	FWidgetImportPreflightFactory()
		: FCustomizableTextObjectFactory(GWarn)
	{
	}

	virtual bool CanCreateClass(UClass* ObjectClass, bool& bOmitSubObjs) const override
	{
		bOmitSubObjs = false;
		return ObjectClass
			&& (ObjectClass->IsChildOf(UWidget::StaticClass())
				|| ObjectClass->IsChildOf(UPanelSlot::StaticClass()));
	}

	virtual void ProcessConstructedObject(UObject* NewObject) override
	{
		check(NewObject);

		if (UWidget* Widget = Cast<UWidget>(NewObject))
		{
			NewWidgetMap.Add(Widget->GetFName(), Widget);
		}
	}

	TMap<FName, UWidget*> NewWidgetMap;
};

// Build a Lua table describing one widget
static sol::table WidgetToTable(sol::state_view& Lua, UWidget* Widget, UWidgetBlueprint* WidgetBP)
{
	sol::table W = Lua.create_table();
	W["name"] = TCHAR_TO_UTF8(*Widget->GetName());
	W["type"] = TCHAR_TO_UTF8(*Widget->GetClass()->GetName());
	W["is_variable"] = (bool)Widget->bIsVariable;
	W["visible"] = Widget->IsVisible();
	W["is_panel"] = Widget->IsA<UPanelWidget>();

	// Visibility enum
	switch (Widget->GetVisibility())
	{
	case ESlateVisibility::Visible:           W["visibility"] = "Visible"; break;
	case ESlateVisibility::Collapsed:         W["visibility"] = "Collapsed"; break;
	case ESlateVisibility::Hidden:            W["visibility"] = "Hidden"; break;
	case ESlateVisibility::HitTestInvisible:  W["visibility"] = "HitTestInvisible"; break;
	case ESlateVisibility::SelfHitTestInvisible: W["visibility"] = "SelfHitTestInvisible"; break;
	}

	// Parent info
	int32 ChildIndex = INDEX_NONE;
	UPanelWidget* Parent = UWidgetTree::FindWidgetParent(Widget, ChildIndex);
	if (Parent)
	{
		W["parent"] = TCHAR_TO_UTF8(*Parent->GetName());
		W["child_index"] = ChildIndex;
	}
	else if (WidgetBP && WidgetBP->WidgetTree && Widget == WidgetBP->WidgetTree->RootWidget)
	{
		W["parent"] = "ROOT";
		W["child_index"] = 0;
	}

	// Slot info
	if (Widget->Slot)
	{
		W["slot_type"] = TCHAR_TO_UTF8(*Widget->Slot->GetClass()->GetName());
	}

	// Children count for panels
	if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
	{
		W["children_count"] = Panel->GetChildrenCount();
	}

	return W;
}

static FDelegateProperty* FindBindableDelegateProperty(UWidget* Widget, const FName& PropertyName, bool& bOutIsAttributeProperty)
{
	bOutIsAttributeProperty = false;
	if (!Widget || PropertyName.IsNone())
	{
		return nullptr;
	}

	// Matches FDelegateEditorBinding::IsBindingValid in UMGEditor/Private/WidgetBlueprint.cpp:
	// property bindings target "<PropertyName>Delegate"; event bindings target PropertyName directly.
	const FName AttributeDelegateName(*(PropertyName.ToString() + TEXT("Delegate")));
	if (FDelegateProperty* DelegateProperty = FindFProperty<FDelegateProperty>(Widget->GetClass(), AttributeDelegateName))
	{
		bOutIsAttributeProperty = true;
		return DelegateProperty;
	}

	return FindFProperty<FDelegateProperty>(Widget->GetClass(), PropertyName);
}

// Recursive hierarchy builder -> returns array of widget tables with depth info
static void BuildHierarchy(sol::state_view& Lua, sol::table& Result, int32& Idx,
	UWidget* Widget, UWidgetBlueprint* WidgetBP, int32 Depth)
{
	if (!Widget) return;

	sol::table W = WidgetToTable(Lua, Widget, WidgetBP);
	W["depth"] = Depth;
	Result[Idx++] = W;

	if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
	{
		for (int32 i = 0; i < Panel->GetChildrenCount(); ++i)
		{
			BuildHierarchy(Lua, Result, Idx, Panel->GetChildAt(i), WidgetBP, Depth + 1);
		}
	}
}

// Get editable properties for a widget
static sol::table GetWidgetProperties(sol::state_view& Lua, UWidget* Widget)
{
	sol::table Props = Lua.create_table();
	int32 Idx = 1;

	for (TFieldIterator<FProperty> PropIt(Widget->GetClass()); PropIt; ++PropIt)
	{
		FProperty* Prop = *PropIt;
		if (!Prop->HasAnyPropertyFlags(CPF_Edit)) continue;
		if (Prop->HasAnyPropertyFlags(CPF_Deprecated)) continue;
		if (CastField<FMulticastDelegateProperty>(Prop)) continue;
		if (CastField<FDelegateProperty>(Prop)) continue;

		FString Value;
		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Widget);
		Prop->ExportTextItem_Direct(Value, ValuePtr, nullptr, nullptr, PPF_None);
		if (Value.Len() > 120) Value = Value.Left(117) + TEXT("...");

		sol::table E = Lua.create_table();
		E["name"] = TCHAR_TO_UTF8(*Prop->GetName());
		E["type"] = TCHAR_TO_UTF8(*NeoStackToolUtils::GetPropertyTypeName(Prop));
		E["value"] = TCHAR_TO_UTF8(*Value);
		FString Category = Prop->GetMetaData(TEXT("Category"));
		E["category"] = Category.IsEmpty() ? "Default" : TCHAR_TO_UTF8(*Category);
		Props[Idx++] = E;
	}

	return Props;
}

// Get slot properties
static sol::table GetSlotProperties(sol::state_view& Lua, UPanelSlot* Slot)
{
	sol::table Props = Lua.create_table();
	if (!Slot) return Props;

	int32 Idx = 1;
	for (TFieldIterator<FProperty> PropIt(Slot->GetClass()); PropIt; ++PropIt)
	{
		FProperty* Prop = *PropIt;
		if (!Prop->HasAnyPropertyFlags(CPF_Edit)) continue;

		FString Value;
		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Slot);
		Prop->ExportTextItem_Direct(Value, ValuePtr, nullptr, nullptr, PPF_None);
		if (Value.Len() > 120) Value = Value.Left(117) + TEXT("...");

		sol::table E = Lua.create_table();
		E["name"] = TCHAR_TO_UTF8(*Prop->GetName());
		E["type"] = TCHAR_TO_UTF8(*NeoStackToolUtils::GetPropertyTypeName(Prop));
		E["value"] = TCHAR_TO_UTF8(*Value);
		Props[Idx++] = E;
	}

	return Props;
}

static bool RegisterWidgetVariableGuidIfMissing(UWidgetBlueprint* WBP, const FName& VariableName, FString* OutReason = nullptr)
{
	if (!WBP || VariableName.IsNone())
	{
		if (OutReason)
		{
			*OutReason = TEXT("invalid widget blueprint or variable name");
		}
		return false;
	}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	if (WBP->WidgetVariableNameToGuidMap.Contains(VariableName))
	{
		return true;
	}

	WBP->OnVariableAdded(VariableName);
#endif
	return true;
}

static void DiscardImportedWidgets(TSet<UWidget*>& ImportedWidgetSet)
{
	for (UWidget* Widget : ImportedWidgetSet)
	{
		if (!Widget)
		{
			continue;
		}

		Widget->RemoveFromParent();

		if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
		{
			Panel->ClearChildren();
		}

		Widget->Rename(nullptr, GetTransientPackage());
	}

	ImportedWidgetSet.Reset();
}

static void DiscardWidgetSubtree(UWidget* RootWidget)
{
	if (!RootWidget)
	{
		return;
	}

	TArray<UWidget*> WidgetsToDiscard;
	WidgetsToDiscard.Add(RootWidget);
	UWidgetTree::GetChildWidgets(RootWidget, WidgetsToDiscard);

	for (UWidget* Widget : WidgetsToDiscard)
	{
		if (!Widget)
		{
			continue;
		}

		Widget->RemoveFromParent();

		if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
		{
			Panel->ClearChildren();
		}

		Widget->Rename(nullptr, GetTransientPackage());
	}
}

static bool ValidateImportPayload(UWidgetBlueprint* WBP, const FString& PayloadText, FString& OutReason)
{
	if (!WBP || !WBP->WidgetTree)
	{
		OutReason = TEXT("no widget tree");
		return false;
	}

	if (!PayloadText.Contains(TEXT("Begin Object")) || !PayloadText.Contains(TEXT("End Object")))
	{
		OutReason = TEXT("payload is not UE object text from export_widgets()");
		return false;
	}

	const FString TempPackageName = FString::Printf(TEXT("/Engine/UMG/Editor/NeoStackImportPreflight_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	UPackage* TempPackage = NewObject<UPackage>(nullptr, *TempPackageName, RF_Transient);
	if (!TempPackage)
	{
		OutReason = TEXT("could not allocate a transient package for import validation");
		return false;
	}

	FWidgetImportPreflightFactory Factory;
	Factory.ProcessBuffer(TempPackage, RF_Transactional, PayloadText);

	if (Factory.NewWidgetMap.Num() == 0)
	{
		OutReason = TEXT("payload did not deserialize any widgets");
		return false;
	}

	for (const TPair<FName, UWidget*>& Entry : Factory.NewWidgetMap)
	{
		UPanelWidget* PanelWidget = Cast<UPanelWidget>(Entry.Value);
		if (!PanelWidget)
		{
			continue;
		}

		const TArray<UPanelSlot*> PanelSlots = PanelWidget->GetSlots();
		const int32 ChildCount = PanelWidget->GetChildrenCount();
		if (PanelSlots.Num() < ChildCount)
		{
			OutReason = FString::Printf(
				TEXT("panel '%s' has %d child widget(s) but only %d slot object(s) in imported text"),
				*PanelWidget->GetName(), ChildCount, PanelSlots.Num());
			return false;
		}

		for (int32 ChildIndex = 0; ChildIndex < ChildCount; ++ChildIndex)
		{
			UWidget* PanelChild = PanelWidget->GetChildAt(ChildIndex);
			UPanelSlot* PanelSlot = PanelSlots.IsValidIndex(ChildIndex) ? PanelSlots[ChildIndex] : nullptr;
			if (!PanelChild || !PanelSlot)
			{
				OutReason = FString::Printf(
					TEXT("panel '%s' contains an invalid child/slot pair at index %d"),
					*PanelWidget->GetName(), ChildIndex);
				return false;
			}
		}
	}

	return true;
}

// ----------------------------------------------------------------------------
// IsNameSafeForWidget — pre-flight validation that mirrors the engine's
// FWidgetBlueprintEditorUtils::VerifyWidgetRename (WidgetBlueprintEditorUtils.cpp:136),
// which is what the Designer's inline rename calls via OnVerifyTextChanged.
//
// Why this exists: FKismetNameValidator (used by both the engine's RenameWidget
// backend and our binding) does NOT enumerate widgets in the WidgetTree. Its name
// table is built from BP variables/functions/graphs/SCS only, and its
// StaticFindObject fallback uses Blueprint as the outer — but widgets live under
// Blueprint->WidgetTree, so the lookup misses them. When a widget has
// bIsVariable=false, it doesn't appear in NewVariables either, so the validator
// returns Ok for a name that already belongs to another widget. The engine's
// public RenameWidget then trips two failures in sequence:
//   1. UWidgetBlueprint::OnVariableRenamed ensure() at WidgetBlueprint.cpp:1038
//      ("WidgetVariableNameToGuidMap should not already contain NewName"). Even
//      though it's a non-fatal ensure, the very next line overwrites the existing
//      GUID — corrupting external references that resolved through the GuidMap.
//   2. UObject::Rename UE_LOG(Fatal) at Obj.cpp:349 ("Renaming on top of an
//      existing object is not allowed") — this one *aborts the process* and only
//      our SEH guard in NeoLuaState saves the editor.
//
// Three checks, in order of cost:
//   (1) WidgetTree->FindWidget — the common case. Catches the user-visible
//       "two widgets with the same name" collision.
//   (2) WidgetVariableNameToGuidMap (5.6+) — every widget gets a GuidMap entry
//       regardless of bIsVariable (WidgetBlueprintCompiler.cpp:794 convention).
//       A stale entry from a malformed prior compile/import will trip the engine
//       ensure even when WidgetTree is clean — catch it ourselves with a
//       sharper, non-corrupting error.
//   (3) UObject::Rename(REN_Test) — the engine's last word on naming. If this
//       dry-run says no, the real rename will fatal. Only meaningful when we
//       have an existing widget being renamed; for add/duplicate the construct
//       site has its own naming guarantees (MakeUniqueObjectName / explicit FName
//       at construction time fails cleanly).
//
// WidgetBeingRenamed: pass the widget that's about to take NewName, or nullptr
// for add/duplicate paths where no widget exists yet.
//
// Returns true if NewName is safe; false with OutReason populated otherwise.
static bool IsNameSafeForWidget(UWidgetBlueprint* WBP, FName NewName,
	UWidget* WidgetBeingRenamed, FString& OutReason)
{
	if (!WBP || !WBP->WidgetTree)
	{
		OutReason = TEXT("no widget tree");
		return false;
	}

	// (1) WidgetTree collision — the case FKismetNameValidator misses.
	if (UWidget* Existing = WBP->WidgetTree->FindWidget(NewName))
	{
		if (Existing != WidgetBeingRenamed)
		{
			OutReason = FString::Printf(
				TEXT("name '%s' is already in use by widget '%s' in this WidgetTree (call list_widgets() to inspect)"),
				*NewName.ToString(), *Existing->GetClass()->GetName());
			return false;
		}
	}

	// (2) GuidMap collision — catches stale entries the WidgetTree check can't see.
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	if (WBP->WidgetVariableNameToGuidMap.Contains(NewName))
	{
		// Allowed only if the existing entry already belongs to the widget being
		// renamed. This covers two benign cases:
		//   (a) Same-name "rename" (no-op) — GuidMap[NewName] == GuidMap[OldName].
		//   (b) Rename round-trip where OldName's GUID was already the one under
		//       NewName (shouldn't happen from a clean state, but harmless).
		// For add/duplicate (WidgetBeingRenamed=nullptr) and for any rename where
		// the GUIDs differ, a Contains() hit means the engine's OnVariableRenamed
		// would silently overwrite an unrelated entry — reject loudly instead.
		const bool bMapEntryIsForUs = WidgetBeingRenamed
			&& WBP->WidgetVariableNameToGuidMap.FindRef(NewName)
			   == WBP->WidgetVariableNameToGuidMap.FindRef(WidgetBeingRenamed->GetFName());
		if (!bMapEntryIsForUs)
		{
			OutReason = FString::Printf(
				TEXT("name '%s' is already registered in WidgetVariableNameToGuidMap (likely a stale entry from a prior compile or import)"),
				*NewName.ToString());
			return false;
		}
	}
#endif

	// (3) UObject-truth dry-run. Only meaningful when we have a widget already.
	if (WidgetBeingRenamed
		&& WidgetBeingRenamed->GetFName() != NewName
		&& !WidgetBeingRenamed->Rename(*NewName.ToString(), nullptr, REN_Test))
	{
		OutReason = FString::Printf(
			TEXT("UObject::Rename(REN_Test) refused '%s' — another object already exists at this name under WidgetTree"),
			*NewName.ToString());
		return false;
	}

	return true;
}

static FName MakeUniqueSafeWidgetName(UWidgetBlueprint* WBP, UClass* WidgetClass, FString& OutReason)
{
	if (!WBP || !WBP->WidgetTree || !WidgetClass)
	{
		OutReason = TEXT("invalid widget blueprint, widget tree, or widget class");
		return NAME_None;
	}

	for (int32 Attempt = 0; Attempt < 1024; ++Attempt)
	{
		const FName Candidate = MakeUniqueObjectName(WBP->WidgetTree, WidgetClass);
		FString CandidateReason;
		if (IsNameSafeForWidget(WBP, Candidate, /*WidgetBeingRenamed=*/nullptr, CandidateReason))
		{
			return Candidate;
		}
	}

	OutReason = TEXT("could not generate a widget name that is free in both the WidgetTree and WidgetVariableNameToGuidMap");
	return NAME_None;
}

} // namespace WidgetHelper

// ============================================================================
// Widget Blueprint Enrichment
// ============================================================================

// Widget Blueprint methods registered on the table returned by open_asset() for any
// UWidgetBlueprint. The underscore-prefixed `_enrich_widget_blueprint` is the internal
// dispatch entry point called from open_asset and is surfaced here for discoverability.
static TArray<FLuaFunctionDoc> WidgetBlueprintDocs = {
	{ TEXT("_enrich_widget_blueprint(bp_table)"), TEXT("Internal — called by open_asset for UWidgetBlueprint assets to attach widget-tree methods"), TEXT("void") },

	// Introspection
	{ TEXT("bp:widget_info()"), TEXT("Summary: widget/panel counts, animations, bindings, named slots, root widget name"), TEXT("table or nil") },
	{ TEXT("bp:list_widgets(filter?)"), TEXT("Full widget tree hierarchy with depth, slot, parent; optional name/type substring filter"), TEXT("array<widget table>") },
	{ TEXT("bp:get_widget(name)"), TEXT("Widget info plus all editable properties; canonical maps are props and slot_props, verbose arrays are properties and slot_properties"), TEXT("table or nil") },
	{ TEXT("bp:find_widgets(query)"), TEXT("Search by name (string) or by table {type=, name=, is_variable=, is_panel=}"), TEXT("array<widget table>") },

	// Mutation verbs — widget tree
	{ TEXT("bp:add_widget(type, opts?)"), TEXT("Construct and parent a UWidget; opts = {name=, parent=, index=, is_variable=}. Omitting parent is only valid when creating the root widget."), TEXT("widget table or nil") },
	{ TEXT("bp:configure_widget(name, opts)"), TEXT("Set editable UProperties on widget or slot. Returns {ok=true, changes=N, warnings=N} when any change applies; nil when no requested change applied."), TEXT("table or nil") },
	{ TEXT("bp:rename_widget(old_name, new_name)"), TEXT("Rename widget, update bindings, animation/navigation references, and variable GUID map; result includes requested_name/final_name/sanitized_name"), TEXT("widget table or nil") },
	{ TEXT("bp:reparent_widget(name, new_parent, index?)"), TEXT("Move widget under a new panel; preflighted against CanAddMoreChildren"), TEXT("true or nil") },
	{ TEXT("bp:reorder_widget(name, new_index)"), TEXT("Shift widget within its current parent to the given child index"), TEXT("true or nil") },
	{ TEXT("bp:duplicate_widget(name, new_name?)"), TEXT("Clone a widget subtree next to the source; auto-generates name if not provided"), TEXT("widget table or nil") },

	// Animations
	{ TEXT("bp:list_animations()"), TEXT("Widget animations with name, display_name, start/end time, track_count"), TEXT("array<animation table>") },
	{ TEXT("bp:get_animation(name)"), TEXT("Detailed animation info with bindings, tracks, sections, master tracks"), TEXT("table or nil") },
	{ TEXT("bp:add_animation(name)"), TEXT("Create a new UWidgetAnimation with a 5-second 30fps MovieScene"), TEXT("animation table or nil") },
	{ TEXT("bp:remove_animation(name)"), TEXT("Delete a widget animation (also calls OnVariableRemoved)"), TEXT("true or nil") },
	{ TEXT("bp:configure_animation(name, opts)"), TEXT("Set display_name, duration, start_time. NOTE: display_name only changes the label — for a full rename use rename_animation(old, new)."), TEXT("true or nil") },
	{ TEXT("bp:rename_animation(old_name, new_name)"), TEXT("Full rename: animation object, MovieScene, WidgetVariableNameToGuidMap key, and graph variable refs. Required for BP graph references to follow."), TEXT("true or nil") },

	// Bindings
	{ TEXT("bp:list_bindings()"), TEXT("All blueprint-level property/function bindings"), TEXT("array<binding table>") },
	{ TEXT("bp:add_binding(opts)"), TEXT("Add or replace a binding: {object_name=, property_name=, function_name?, source_property?, kind?=\"Property\"|\"Function\"}"), TEXT("true or nil") },
	{ TEXT("bp:remove_binding(opts)"), TEXT("Remove bindings matching {object_name?, property_name?} (at least one required)"), TEXT("int count or nil") },

	// Named slots
	{ TEXT("bp:list_named_slots()"), TEXT("UNamedSlot widgets in the local tree with parent and content/content_name/content_type"), TEXT("array<slot table>") },
	{ TEXT("bp:list_inherited_named_slots()"), TEXT("Named slots exposed by parent widget classes; {name, has_inherited_content}"), TEXT("array<slot table>") },
	{ TEXT("bp:add_named_slot(name, opts?)"), TEXT("Create a UNamedSlot widget; opts = {parent=, expose_on_instance_only=}"), TEXT("slot table or nil") },
	{ TEXT("bp:remove_named_slot(name)"), TEXT("Delete a named slot widget (renames to transient, calls OnVariableRemoved)"), TEXT("true or nil") },
	{ TEXT("bp:set_named_slot_content(slot, widget_or_nil)"), TEXT("Assign a widget to a named slot; nil clears. Rejects self, root, and ancestor cycles"), TEXT("true or nil") },

	// Editor-surface wrappers (Finding 7)
	{ TEXT("bp:validate_widget_tree()"), TEXT("Circular-reference check via UWidgetBlueprint::HasCircularReferences"), TEXT("{ok=bool, offending_widget?, offending_type?}") },
	{ TEXT("bp:get_desired_focus()"), TEXT("Startup keyboard-focus widget name from the UserWidget CDO"), TEXT("string or nil") },
	{ TEXT("bp:set_desired_focus(name_or_nil)"), TEXT("Set the UUserWidget::DesiredFocusWidget; nil/empty clears"), TEXT("true or nil") },
	{ TEXT("bp:configure_thumbnail(texture_path)"), TEXT("Set the asset thumbnail to a UTexture2D via FWidgetBlueprintEditorUtils::SetTextureAsAssetThumbnail"), TEXT("true or nil") },
	{ TEXT("bp:export_widgets(names?)"), TEXT("Serialize widgets to text (nil = root plus descendants, string = one widget, array = selected widgets)"), TEXT("string or nil") },
	{ TEXT("bp:import_widgets(text, opts?)"), TEXT("Paste serialized widgets; opts={parent=\"panel_name\"}. Required when the tree has a root. Registers each widget in the compiler GUID map and parents the root widgets. Returns array of {name,type,is_root}"), TEXT("array or nil") },
};

static void BindWidgetBlueprint(sol::state& Lua, FLuaSessionData& Session)
{
	// ==================================================================
	// _enrich_widget_blueprint (called from open_asset for WidgetBlueprints)
	// ==================================================================
	Lua.set_function("_enrich_widget_blueprint", [&Session](sol::table BPObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = BPObj.get<std::string>("path");
		FString FPath = NeoLuaStr::ToFString(PathStr);
		UWidgetBlueprint* WidgetBP = NeoLuaAsset::Resolve<UWidgetBlueprint>(FPath);
		if (!WidgetBP) return;

		// Store widget-specific help
		sol::optional<std::string> ExistingHelp = BPObj.get<sol::optional<std::string>>("_help_text");
		std::string BaseHelp = ExistingHelp.has_value() ? ExistingHelp.value() : "";

		BPObj["_help_text"] = BaseHelp +
			"\n=== Widget Blueprint methods ===\n"
			"list_widgets(filter?)         -> full widget tree hierarchy\n"
			"get_widget(name)              -> widget info + props map + slot_props map + verbose properties arrays\n"
			"find_widgets(query)           -> search by type or name pattern\n"
			"add_widget(type, opts?)       -> create widget; omit parent only when creating the root\n"
			"remove_widget(name)           -> remove widget from tree (with cleanup)\n"
			"configure_widget(name, opts)  -> set widget/slot properties; returns {ok=true, changes=N, warnings=N}; nil if 0 changes apply\n"
			"rename_widget(old_name, new_name) -> rename widget + update all refs; result includes final_name when sanitized\n"
			"reparent_widget(name, new_parent, index?) -> move widget to new parent\n"
			"duplicate_widget(name, new_name?) -> clone widget subtree\n"
			"reorder_widget(name, new_index)   -> change position within parent\n"
			"list_animations()             -> widget animations\n"
			"get_animation(name)           -> animation details with tracks/bindings\n"
			"add_animation(name)           -> create new widget animation with a 5-second default playback range\n"
			"remove_animation(name)        -> delete widget animation\n"
			"configure_animation(name, opts) -> set timing; display_name only changes the label\n"
			"rename_animation(old_name, new_name) -> full rename (object, MovieScene, variable, graph refs)\n"
			"list_bindings()               -> all property bindings (blueprint-level, NOT per-widget)\n"
			"add_binding({object_name=, property_name=, function_name=, source_property=}) -> add validated property binding\n"
			"remove_binding({object_name=, property_name=}) -> remove property binding (pass table, not string)\n"
			"list_named_slots()            -> local named slots with content/content_name/content_type\n"
			"list_inherited_named_slots()  -> named slots exposed by parent widget classes\n"
			"add_named_slot(name, opts?)   -> create named slot widget\n"
			"remove_named_slot(name)       -> remove named slot\n"
			"set_named_slot_content(slot, widget_or_nil) -> assign widget to named slot (nil clears)\n"
			"validate_widget_tree()        -> {ok=bool, offending_widget?} circular-reference check\n"
			"get_desired_focus()           -> current startup focus widget name or nil\n"
			"set_desired_focus(name_or_nil) -> set or clear startup keyboard focus widget\n"
			"configure_thumbnail(texture_path) -> set asset thumbnail from a UTexture2D\n"
			"export_widgets(names?)        -> serialized text (nil = root plus descendants, string = one, array = many)\n"
			"import_widgets(text, {parent=name}?) -> paste serialized widgets under parent; returns array\n"
			"widget_info()                 -> widget tree summary\n"
			"\n"
			"=== configure_widget property examples ===\n"
			"ALL editable UProperties are settable. Use get_widget(name).props to discover widget property names.\n"
			"Values can be Lua scalars, tables (auto-converted to UE struct format), or UE ImportText strings.\n"
			"\n"
			"-- Text color (FSlateColor wraps FLinearColor):\n"
			"configure_widget('MyText', {ColorAndOpacity='(SpecifiedColor=(R=1,G=0,B=0,A=1))'})\n"
			"\n"
			"-- Font size (FSlateFontInfo - use ImportText string or table):\n"
			"configure_widget('MyText', {Font='(Size=24)'})\n"
			"-- Font family/typeface; FontObject paths are resolved explicitly:\n"
			"configure_widget('MyText', {Font={FontObject='/Engine/EngineFonts/RobotoMono.RobotoMono', TypefaceFontName='Bold', Size=18, LetterSpacing=80}})\n"
			"configure_widget('MyText', {Font=\"(FontObject=Font'/Engine/EngineFonts/RobotoMono.RobotoMono',TypefaceFontName=\\\"Bold\\\",Size=18)\"})\n"
			"\n"
			"-- Shadow:\n"
			"configure_widget('MyText', {ShadowOffset='(X=2,Y=2)', ShadowColorAndOpacity='(R=0,G=0,B=0,A=0.5)'})\n"
			"\n"
			"-- Slot padding/margins (common panel slots expose this alias):\n"
			"configure_widget('MyText', {slot={Padding={left=10, top=5, right=10, bottom=5}}})\n"
			"-- Slot alignment:\n"
			"configure_widget('MyText', {slot={HorizontalAlignment='HAlign_Center', VerticalAlignment='VAlign_Center'}})\n"
			"-- Grid slots:\n"
			"configure_widget('MyCell', {slot={Row=1, Column=2, RowSpan=1, ColumnSpan=2, Layer=0, Nudge={x=0,y=0}}})\n"
			"-- CanvasPanelSlot accepts agent-friendly aliases and the raw LayoutData struct; inspect get_widget(name).slot_props first:\n"
			"configure_widget('MyText', {slot={Position={x=10,y=20}, Size={x=200,y=40}, Anchors={minimum={x=0,y=0}, maximum={x=0,y=0}}, Alignment={x=0,y=0}, ZOrder=1, AutoSize=false}})\n"
			"configure_widget('MyText', {slot={LayoutData='(Offsets=(Left=10,Top=20,Right=200,Bottom=40),Anchors=(Minimum=(X=0,Y=0),Maximum=(X=0,Y=0)),Alignment=(X=0,Y=0))'}})\n"
			"\n"
			"-- Image tint:\n"
			"configure_widget('MyImage', {ColorAndOpacity='(R=1,G=0.5,B=0,A=1)'})\n"
			"\n"
			"-- Button background (FLinearColor, not FSlateColor):\n"
			"configure_widget('MyButton', {BackgroundColor='(R=0.2,G=0.6,B=1,A=1)'})\n"
			"\n"
			"-- Any UProperty by name (use get_widget to see all available):\n"
			"configure_widget('MyText', {Justification='ETextJustify::Center', AutoWrapText=true})\n"
			"\n"
			"TIP: get_widget(name).slot_props is the flat slot map; slot_properties is the verbose array.\n"
			"TIP: For complex structs, pass the full UE ImportText string (get_widget shows current format).\n"
			"TIP: .graphs/.variables/.components refresh after structural Blueprint mutations; call refresh() if external code changed the asset.\n"
			"TIP: list_events() with no source includes widget-tree delegates and components; bind_event(source,event) validates the event name.\n"
			"TIP: add_event_dispatcher accepts either {{name=..., type=...}} or {params={{name=..., type=...}}}.\n"
			"\n"
			"(Also available: list_events, bind_event, unbind_event from Blueprint base)\n";

		// ================================================================
		// widget_info() -> structured summary
		// ================================================================
		BPObj.set_function("widget_info", [FPath, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UWidgetBlueprint* WBP = NeoLuaAsset::Resolve<UWidgetBlueprint>(FPath);
			if (!WBP)
			{
				Session.Log(TEXT("[FAIL] widget_info -> blueprint not found. Call open_asset(path) to reload, or find_assets(\"WidgetBlueprint\") to discover paths."));
				return sol::lua_nil;
			}

			sol::table Result = Lua.create_table();
			Result["path"] = TCHAR_TO_UTF8(*FPath);
			Result["type"] = "WidgetBlueprint";

			if (WBP->ParentClass)
				Result["parent_class"] = TCHAR_TO_UTF8(*WBP->ParentClass->GetName());

			// Widget count
			int32 WidgetCount = 0;
			int32 PanelCount = 0;
			if (WBP->WidgetTree)
			{
				TArray<UWidget*> AllWidgets;
				WBP->WidgetTree->GetAllWidgets(AllWidgets);
				WidgetCount = AllWidgets.Num();
				for (UWidget* W : AllWidgets)
				{
					if (W->IsA<UPanelWidget>()) PanelCount++;
				}

				if (WBP->WidgetTree->RootWidget)
					Result["root_widget"] = TCHAR_TO_UTF8(*WBP->WidgetTree->RootWidget->GetName());
			}
			Result["widget_count"] = WidgetCount;
			Result["panel_count"] = PanelCount;
			Result["animation_count"] = WBP->Animations.Num();
			Result["binding_count"] = WBP->Bindings.Num();

			// Named slots
			sol::table Slots = Lua.create_table();
			int32 SlotIdx = 1;
			if (WBP->WidgetTree)
			{
				TArray<FName> SlotNames;
				WBP->WidgetTree->GetSlotNames(SlotNames);
				for (const FName& Name : SlotNames)
				{
					Slots[SlotIdx++] = TCHAR_TO_UTF8(*Name.ToString());
				}
			}
			Result["named_slots"] = Slots;

			Session.Log(FString::Printf(TEXT("[OK] widget_info() -> %d widgets, %d animations"),
				WidgetCount, WBP->Animations.Num()));
			return Result;
		});

		// ================================================================
		// list_widgets(filter?) -> full widget tree hierarchy
		// ================================================================
		BPObj.set_function("list_widgets", [FPath, &Session](sol::table /*self*/,
			sol::optional<std::string> FilterOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			UWidgetBlueprint* WBP = NeoLuaAsset::Resolve<UWidgetBlueprint>(FPath);
			if (!WBP || !WBP->WidgetTree)
			{
				Session.Log(TEXT("[FAIL] list_widgets -> no widget tree. The blueprint has no UWidgetTree — open_asset(path) with a valid WidgetBlueprint, or call add_widget(type) to seed a root widget."));
				return sol::lua_nil;
			}

			FString Filter = NeoLuaStr::ToFStringOpt(FilterOpt);

			sol::table Result = Lua.create_table();

			if (Filter.IsEmpty())
			{
				// Return full hierarchy from root
				int32 Idx = 1;
				WidgetHelper::BuildHierarchy(Lua, Result, Idx, WBP->WidgetTree->RootWidget, WBP, 0);

				Session.Log(FString::Printf(TEXT("[OK] list_widgets() -> %d widgets"), Idx - 1));
			}
			else
			{
				// Filtered: match name or type
				TArray<UWidget*> AllWidgets;
				WBP->WidgetTree->GetAllWidgets(AllWidgets);
				int32 Idx = 1;

				for (UWidget* Widget : AllWidgets)
				{
					FString Name = Widget->GetName();
					FString Type = Widget->GetClass()->GetName();
					if (Name.Contains(Filter, ESearchCase::IgnoreCase) ||
						Type.Contains(Filter, ESearchCase::IgnoreCase))
					{
						Result[Idx++] = WidgetHelper::WidgetToTable(Lua, Widget, WBP);
					}
				}

				Session.Log(FString::Printf(TEXT("[OK] list_widgets(\"%s\") -> %d matches"), *Filter, Idx - 1));
			}

			return Result;
		});

		// ================================================================
		// get_widget(name) -> detailed widget info
		// ================================================================
		BPObj.set_function("get_widget", [FPath, &Session](sol::table /*self*/,
			const std::string& WidgetName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FName = NeoLuaStr::ToFString(WidgetName);

			UWidgetBlueprint* WBP = NeoLuaAsset::Resolve<UWidgetBlueprint>(FPath);
			if (!WBP || !WBP->WidgetTree)
			{
				Session.Log(TEXT("[FAIL] get_widget -> no widget tree. The blueprint has no UWidgetTree — open_asset(path) with a valid WidgetBlueprint, or call add_widget(type) to seed a root widget."));
				return sol::lua_nil;
			}

			UWidget* Widget = WBP->WidgetTree->FindWidget(::FName(*FName));
			if (!Widget)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] get_widget(\"%s\") -> not found. Call list_widgets() or find_widgets(query) to discover valid widget names."), *FName));
				return sol::lua_nil;
			}

			sol::table Result = WidgetHelper::WidgetToTable(Lua, Widget, WBP);

			// Add detailed properties (array of {name, type, value, category} tables)
			Result["properties"] = WidgetHelper::GetWidgetProperties(Lua, Widget);

			// Add flat props map for easy access: props["PropertyName"] = "value"
			sol::table FlatProps = Lua.create_table();
			for (TFieldIterator<FProperty> PropIt(Widget->GetClass()); PropIt; ++PropIt)
			{
				FProperty* Prop = *PropIt;
				if (!Prop->HasAnyPropertyFlags(CPF_Edit)) continue;
				if (Prop->HasAnyPropertyFlags(CPF_Deprecated)) continue;
				if (CastField<FMulticastDelegateProperty>(Prop)) continue;
				if (CastField<FDelegateProperty>(Prop)) continue;

				FString Value;
				const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Widget);
				Prop->ExportTextItem_Direct(Value, ValuePtr, nullptr, nullptr, PPF_None);
				if (Value.Len() > 500) Value = Value.Left(497) + TEXT("...");
				FlatProps[TCHAR_TO_UTF8(*Prop->GetName())] = TCHAR_TO_UTF8(*Value);
			}
			Result["props"] = FlatProps;

			// Add slot properties
			if (Widget->Slot)
			{
				Result["slot_properties"] = WidgetHelper::GetSlotProperties(Lua, Widget->Slot);

				// Flat slot props too
				sol::table FlatSlot = Lua.create_table();
				for (TFieldIterator<FProperty> PropIt(Widget->Slot->GetClass()); PropIt; ++PropIt)
				{
					FProperty* Prop = *PropIt;
					if (!Prop->HasAnyPropertyFlags(CPF_Edit)) continue;
					FString Value;
					const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Widget->Slot);
					Prop->ExportTextItem_Direct(Value, ValuePtr, nullptr, nullptr, PPF_None);
					if (Value.Len() > 500) Value = Value.Left(497) + TEXT("...");
					FlatSlot[TCHAR_TO_UTF8(*Prop->GetName())] = TCHAR_TO_UTF8(*Value);
				}
				Result["slot_props"] = FlatSlot;
			}

			// Add children list for panels
			if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
			{
				sol::table Children = Lua.create_table();
				for (int32 i = 0; i < Panel->GetChildrenCount(); ++i)
				{
					UWidget* Child = Panel->GetChildAt(i);
					if (Child)
					{
						sol::table C = Lua.create_table();
						C["name"] = TCHAR_TO_UTF8(*Child->GetName());
						C["type"] = TCHAR_TO_UTF8(*Child->GetClass()->GetName());
						C["index"] = i;
						Children[i + 1] = C;
					}
				}
				Result["children"] = Children;
			}

			Session.Log(FString::Printf(TEXT("[OK] get_widget(\"%s\") -> %s"),
				*FName, *Widget->GetClass()->GetName()));
			return Result;
		});

		// ================================================================
		// find_widgets(query) -> search by type, name pattern, or property
		// ================================================================
		BPObj.set_function("find_widgets", [FPath, &Session](sol::table /*self*/,
			sol::object QueryObj, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			UWidgetBlueprint* WBP = NeoLuaAsset::Resolve<UWidgetBlueprint>(FPath);
			if (!WBP || !WBP->WidgetTree)
			{
				Session.Log(TEXT("[FAIL] find_widgets -> no widget tree. The blueprint has no UWidgetTree — open_asset(path) with a valid WidgetBlueprint, or call add_widget(type) to seed a root widget."));
				return sol::lua_nil;
			}

			TArray<UWidget*> AllWidgets;
			WBP->WidgetTree->GetAllWidgets(AllWidgets);

			sol::table Result = Lua.create_table();
			int32 Idx = 1;

			if (QueryObj.is<std::string>())
			{
				// String query: match name or class name
				FString Query = NeoLuaStr::ToFString(QueryObj.as<std::string>());

				for (UWidget* Widget : AllWidgets)
				{
					FString Name = Widget->GetName();
					FString ClassName = Widget->GetClass()->GetName();

					if (Name.Contains(Query, ESearchCase::IgnoreCase) ||
						ClassName.Equals(Query, ESearchCase::IgnoreCase) ||
						ClassName.Equals(Query + TEXT("Widget"), ESearchCase::IgnoreCase))
					{
						Result[Idx++] = WidgetHelper::WidgetToTable(Lua, Widget, WBP);
					}
				}

				Session.Log(FString::Printf(TEXT("[OK] find_widgets(\"%s\") -> %d matches"), *Query, Idx - 1));
			}
			else if (QueryObj.is<sol::table>())
			{
				// Table query: {type="Button", name="*Submit*", is_variable=true}
				sol::table Q = QueryObj.as<sol::table>();
				FString TypeFilter = NeoLuaStr::ToFString(Q.get_or("type", std::string()));
				FString NameFilter = NeoLuaStr::ToFString(Q.get_or("name", std::string()));
				sol::optional<bool> VarFilter = Q.get<sol::optional<bool>>("is_variable");
				sol::optional<bool> PanelFilter = Q.get<sol::optional<bool>>("is_panel");

				for (UWidget* Widget : AllWidgets)
				{
					FString Name = Widget->GetName();
					FString ClassName = Widget->GetClass()->GetName();

					if (!TypeFilter.IsEmpty() &&
						!ClassName.Equals(TypeFilter, ESearchCase::IgnoreCase) &&
						!ClassName.Equals(TypeFilter + TEXT("Widget"), ESearchCase::IgnoreCase) &&
						!Widget->GetClass()->IsChildOf(NeoTypeResolver::FindClassRobust(TypeFilter)))
					{
						continue;
					}
					if (!NameFilter.IsEmpty() && !Name.Contains(NameFilter, ESearchCase::IgnoreCase))
						continue;
					if (VarFilter.has_value() && Widget->bIsVariable != VarFilter.value())
						continue;
					if (PanelFilter.has_value() && Widget->IsA<UPanelWidget>() != PanelFilter.value())
						continue;

					Result[Idx++] = WidgetHelper::WidgetToTable(Lua, Widget, WBP);
				}

				Session.Log(FString::Printf(TEXT("[OK] find_widgets({...}) -> %d matches"), Idx - 1));
			}

			return Result;
		});

		// ================================================================
		// reparent_widget(name, new_parent, index?)
		// ================================================================
		BPObj.set_function("reparent_widget", [FPath, &Session](sol::table /*self*/,
			const std::string& WidgetName, const std::string& NewParentName,
			sol::optional<int> IndexOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FWidgetName = NeoLuaStr::ToFString(WidgetName);
			FString FNewParent = NeoLuaStr::ToFString(NewParentName);

			UWidgetBlueprint* WBP = NeoLuaAsset::Resolve<UWidgetBlueprint>(FPath);
			if (!WBP || !WBP->WidgetTree)
			{
				Session.Log(TEXT("[FAIL] reparent_widget -> no widget tree. The blueprint has no UWidgetTree — open_asset(path) with a valid WidgetBlueprint, or call add_widget(type) to seed a root widget."));
				return sol::lua_nil;
			}

			UWidget* Widget = WBP->WidgetTree->FindWidget(::FName(*FWidgetName));
			if (!Widget)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] reparent_widget(\"%s\") -> widget not found. Call list_widgets() to discover valid widget names."), *FWidgetName));
				return sol::lua_nil;
			}

			UPanelWidget* NewParent = Cast<UPanelWidget>(WBP->WidgetTree->FindWidget(::FName(*FNewParent)));
			if (!NewParent)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] reparent_widget -> parent '%s' not found or not a panel. Call find_widgets({is_panel=true}) to choose a valid parent."), *FNewParent));
				return sol::lua_nil;
			}

			// Cannot parent to self or descendant
			if (Widget == NewParent)
			{
				Session.Log(TEXT("[FAIL] reparent_widget -> cannot parent widget to itself"));
				return sol::lua_nil;
			}
			// Walk up from NewParent to check if Widget is an ancestor (prevents circular parenting)
			{
				UWidget* Walk = NewParent;
				while (Walk)
				{
					if (Walk == Widget)
					{
						Session.Log(TEXT("[FAIL] reparent_widget -> cannot parent to own descendant"));
						return sol::lua_nil;
					}
					int32 CI;
					Walk = UWidgetTree::FindWidgetParent(Walk, CI);
				}
			}

			// Preflight the new parent BEFORE unhooking the widget from its current parent.
			// UPanelWidget::AddChild (PanelWidget.cpp:139) returns nullptr when
			// !bCanHaveMultipleChildren && GetChildrenCount() > 0 (UBorder/UScaleBox/
			// URetainerBox/USizeBox/UNamedSlot/…). Without this check we'd orphan Widget.
			if (!NewParent->CanAddMoreChildren())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] reparent_widget -> parent '%s' cannot accept more children (single-child panel is full)"), *FNewParent));
				return sol::lua_nil;
			}

			// Also reject reparenting the root widget (it has no panel parent to detach from).
			int32 RootProbeIdx;
			if (!UWidgetTree::FindWidgetParent(Widget, RootProbeIdx) && Widget == WBP->WidgetTree->RootWidget)
			{
				Session.Log(TEXT("[FAIL] reparent_widget -> cannot reparent root widget (remove it first or set a new root)"));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaReparentWidget", "Reparent Widget"));
			WBP->WidgetTree->SetFlags(RF_Transactional);
			WBP->WidgetTree->Modify();

			// Remove from current parent
			int32 OldIndex;
			UPanelWidget* OldParent = UWidgetTree::FindWidgetParent(Widget, OldIndex);
			if (OldParent)
			{
				OldParent->Modify();
				OldParent->RemoveChild(Widget);
			}

			// Add to new parent — verify the insert actually produced a UPanelSlot.
			NewParent->Modify();
			UPanelSlot* NewSlot = nullptr;
			if (IndexOpt.has_value() && IndexOpt.value() >= 0)
			{
				int32 Idx = FMath::Min(IndexOpt.value(), NewParent->GetChildrenCount());
				NewSlot = NewParent->InsertChildAt(Idx, Widget);
			}
			else
			{
				NewSlot = NewParent->AddChild(Widget);
			}

			if (!NewSlot)
			{
				// Defensive: re-attach to the old parent so we don't leave Widget orphaned.
				if (OldParent)
				{
					OldParent->AddChild(Widget);
				}
				Session.Log(FString::Printf(TEXT("[FAIL] reparent_widget(\"%s\" -> \"%s\") -> InsertChildAt/AddChild returned null; widget restored to previous parent"),
					*FWidgetName, *FNewParent));
				return sol::lua_nil;
			}

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

			Session.Log(FString::Printf(TEXT("[OK] reparent_widget(\"%s\" -> \"%s\")"), *FWidgetName, *FNewParent));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// reorder_widget(name, new_index)
		// ================================================================
		BPObj.set_function("reorder_widget", [FPath, &Session](sol::table /*self*/,
			const std::string& WidgetName, int NewIndex,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FWidgetName = NeoLuaStr::ToFString(WidgetName);

			UWidgetBlueprint* WBP = NeoLuaAsset::Resolve<UWidgetBlueprint>(FPath);
			if (!WBP || !WBP->WidgetTree)
			{
				Session.Log(TEXT("[FAIL] reorder_widget -> no widget tree. The blueprint has no UWidgetTree — open_asset(path) with a valid WidgetBlueprint, or call add_widget(type) to seed a root widget."));
				return sol::lua_nil;
			}

			UWidget* Widget = WBP->WidgetTree->FindWidget(::FName(*FWidgetName));
			if (!Widget)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] reorder_widget(\"%s\") -> widget not found. Call list_widgets() to discover valid widget names."), *FWidgetName));
				return sol::lua_nil;
			}

			int32 OldIndex;
			UPanelWidget* Parent = UWidgetTree::FindWidgetParent(Widget, OldIndex);
			if (!Parent)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] reorder_widget(\"%s\") -> no parent (root widget?)"), *FWidgetName));
				return sol::lua_nil;
			}

			int32 ClampedIndex = FMath::Clamp(NewIndex, 0, Parent->GetChildrenCount() - 1);

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaReorderWidget", "Reorder Widget"));
			WBP->WidgetTree->Modify();
			Parent->Modify();

			Parent->ShiftChild(ClampedIndex, Widget);

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

			Session.Log(FString::Printf(TEXT("[OK] reorder_widget(\"%s\", %d) (was %d)"),
				*FWidgetName, ClampedIndex, OldIndex));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// duplicate_widget(name, new_name?)
		// ================================================================
		BPObj.set_function("duplicate_widget", [FPath, &Session](sol::table /*self*/,
			const std::string& WidgetName, sol::optional<std::string> NewNameOpt,
			sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FWidgetName = NeoLuaStr::ToFString(WidgetName);

			UWidgetBlueprint* WBP = NeoLuaAsset::Resolve<UWidgetBlueprint>(FPath);
			if (!WBP || !WBP->WidgetTree)
			{
				Session.Log(TEXT("[FAIL] duplicate_widget -> no widget tree. The blueprint has no UWidgetTree — open_asset(path) with a valid WidgetBlueprint, or call add_widget(type) to seed a root widget."));
				return sol::lua_nil;
			}

			UWidget* SourceWidget = WBP->WidgetTree->FindWidget(::FName(*FWidgetName));
			if (!SourceWidget)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] duplicate_widget(\"%s\") -> not found"), *FWidgetName));
				return sol::lua_nil;
			}

			int32 OldIndex;
			UPanelWidget* Parent = UWidgetTree::FindWidgetParent(SourceWidget, OldIndex);
			if (!Parent)
			{
				Session.Log(TEXT("[FAIL] duplicate_widget -> cannot duplicate root widget"));
				return sol::lua_nil;
			}

			// Determine new name
			FString FNewName;
			if (NewNameOpt.has_value())
			{
				FNewName = NeoLuaStr::ToFStringOpt(NewNameOpt);
			}
			else
			{
				// Auto-generate: WidgetName_Copy, WidgetName_Copy2, etc.
				FNewName = FWidgetName + TEXT("_Copy");
				int32 Suffix = 2;
				while (WBP->WidgetTree->FindWidget(::FName(*FNewName)))
				{
					FNewName = FString::Printf(TEXT("%s_Copy%d"), *FWidgetName, Suffix++);
				}
			}

			// Pre-flight uniqueness — same rationale as add_widget. The auto-generated
			// branch above already cycles past WidgetTree collisions, but a stale GuidMap
			// entry would still trip OnVariableAdded post-construction.
			{
				FString PreflightReason;
				if (!WidgetHelper::IsNameSafeForWidget(WBP, ::FName(*FNewName), /*WidgetBeingRenamed=*/nullptr, PreflightReason))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] duplicate_widget -> %s"), *PreflightReason));
					return sol::lua_nil;
				}
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaDuplicateWidget", "Duplicate Widget"));
			WBP->WidgetTree->SetFlags(RF_Transactional);
			WBP->WidgetTree->Modify();

			// Create duplicate via DuplicateObject
			UWidget* NewWidget = DuplicateObject<UWidget>(SourceWidget, WBP->WidgetTree, ::FName(*FNewName));
			if (!NewWidget)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] duplicate_widget(\"%s\") -> duplication failed"), *FWidgetName));
				return sol::lua_nil;
			}

			// Ensure child widgets from the duplication have unique names (mirrors engine FindNextValidName pattern)
			if (UPanelWidget* NewPanel = Cast<UPanelWidget>(NewWidget))
			{
				TArray<UWidget*> ChildWidgets;
				UWidgetTree::GetChildWidgets(NewWidget, ChildWidgets);
				for (UWidget* Child : ChildWidgets)
				{
					if (Child && Child != NewWidget)
					{
						FString ChildName = Child->GetName();
						// Check if a widget with the same name already exists in the tree
						if (UWidget* Existing = WBP->WidgetTree->FindWidget(Child->GetFName()))
						{
							if (Existing != Child)
							{
								// Generate unique name
								FString BaseName = ChildName;
								int32 Suffix = 1;
								FString UniqueName = FString::Printf(TEXT("%s_%d"), *BaseName, Suffix);
								while (WBP->WidgetTree->FindWidget(::FName(*UniqueName)))
								{
									Suffix++;
									UniqueName = FString::Printf(TEXT("%s_%d"), *BaseName, Suffix);
								}
								Child->Rename(*UniqueName, WBP->WidgetTree);
							}
						}
					}
				}
			}

			// Preflight: InsertChildAt will fail on single-child panels already holding content.
			// Do NOT register the variable in WidgetVariableNameToGuidMap until we know the insert
			// succeeded — otherwise OnVariableAdded leaves a dangling GUID entry for a widget
			// that was never actually parented.
			if (!Parent->CanAddMoreChildren())
			{
				WidgetHelper::DiscardWidgetSubtree(NewWidget);
				Session.Log(FString::Printf(TEXT("[FAIL] duplicate_widget(\"%s\") -> parent '%s' cannot accept more children (single-child panel is full)"),
					*FWidgetName, *Parent->GetName()));
				return sol::lua_nil;
			}

			Parent->Modify();
			UPanelSlot* Slot = Parent->InsertChildAt(OldIndex + 1, NewWidget);
			if (!Slot)
			{
				// Discard the outered duplicate so the widget tree doesn't retain a stray UObject.
				WidgetHelper::DiscardWidgetSubtree(NewWidget);
				Session.Log(FString::Printf(TEXT("[FAIL] duplicate_widget(\"%s\") -> InsertChildAt returned null; duplicate discarded"), *FWidgetName));
				return sol::lua_nil;
			}

			// Only now — after the insert succeeded — register the widget variables. Engine paste
			// (WidgetBlueprintEditorUtils.cpp:1966) calls OnVariableAdded for EVERY pasted widget,
			// not just the root, because the compiler needs a WidgetVariableNameToGuidMap entry
			// for any duplicated descendant that is bIsVariable, a UNamedSlot, or referenced by a
			// binding (WidgetBlueprintCompiler.cpp:1455/1483). A missing entry triggers
			// ensureAlwaysMsgf on compile and regenerates a fresh GUID, breaking external refs.
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			WidgetHelper::RegisterWidgetVariableGuidIfMissing(WBP, NewWidget->GetFName());
			{
				TArray<UWidget*> DuplicatedDescendants;
				UWidgetTree::GetChildWidgets(NewWidget, DuplicatedDescendants);
				for (UWidget* Descendant : DuplicatedDescendants)
				{
					if (Descendant && Descendant != NewWidget)
					{
						WidgetHelper::RegisterWidgetVariableGuidIfMissing(WBP, Descendant->GetFName());
					}
				}
			}
#endif

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

			Session.Log(FString::Printf(TEXT("[OK] duplicate_widget(\"%s\") -> \"%s\""), *FWidgetName, *FNewName));

			sol::table Result = WidgetHelper::WidgetToTable(Lua, NewWidget, WBP);
			return Result;
		});

		// ================================================================
		// list_animations() -> widget animations
		// ================================================================
		BPObj.set_function("list_animations", [FPath, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			UWidgetBlueprint* WBP = NeoLuaAsset::Resolve<UWidgetBlueprint>(FPath);
			if (!WBP)
			{
				Session.Log(TEXT("[FAIL] list_animations -> blueprint not found. Call open_asset(path) to reload, or find_assets(\"WidgetBlueprint\") to discover paths."));
				return sol::lua_nil;
			}

			sol::table Result = Lua.create_table();
			int32 Idx = 1;

			for (UWidgetAnimation* Anim : WBP->Animations)
			{
				if (!Anim) continue;

				sol::table A = Lua.create_table();
				A["name"] = TCHAR_TO_UTF8(*Anim->GetName());
				A["display_name"] = TCHAR_TO_UTF8(*Anim->GetDisplayName().ToString());

				// UWidgetAnimation::GetStartTime/GetEndTime (WidgetAnimation.cpp:170/175) dereference
				// MovieScene without null-guards. A malformed animation entry would crash the editor.
				UMovieScene* MovieScene = Anim->GetMovieScene();
				if (MovieScene)
				{
					A["start_time"] = Anim->GetStartTime();
					A["end_time"] = Anim->GetEndTime();
					A["duration"] = Anim->GetEndTime() - Anim->GetStartTime();
					const TArray<FMovieSceneBinding>& Bindings = const_cast<const UMovieScene*>(MovieScene)->GetBindings();
					A["track_count"] = Bindings.Num();
					A["has_movie_scene"] = true;
				}
				else
				{
					A["has_movie_scene"] = false;
				}

				Result[Idx++] = A;
			}

			Session.Log(FString::Printf(TEXT("[OK] list_animations() -> %d animations"), Idx - 1));
			return Result;
		});

		// ================================================================
		// get_animation(name) -> detailed animation info with tracks
		// ================================================================
		BPObj.set_function("get_animation", [FPath, &Session](sol::table /*self*/,
			const std::string& AnimName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FAnimName = NeoLuaStr::ToFString(AnimName);

			UWidgetBlueprint* WBP = NeoLuaAsset::Resolve<UWidgetBlueprint>(FPath);
			if (!WBP) { Session.Log(TEXT("[FAIL] get_animation -> blueprint not found. Call open_asset(path) to reload, or find_assets(\"WidgetBlueprint\") to discover paths.")); return sol::lua_nil; }

			UWidgetAnimation* FoundAnim = nullptr;
			for (UWidgetAnimation* Anim : WBP->Animations)
			{
				if (Anim && (Anim->GetName().Equals(FAnimName, ESearchCase::IgnoreCase) ||
					Anim->GetDisplayName().ToString().Equals(FAnimName, ESearchCase::IgnoreCase)))
				{
					FoundAnim = Anim;
					break;
				}
			}
			if (!FoundAnim)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] get_animation(\"%s\") -> animation not found. Call list_animations() to discover valid animation names."), *FAnimName));
				return sol::lua_nil;
			}

			sol::table Result = Lua.create_table();
			Result["name"] = TCHAR_TO_UTF8(*FoundAnim->GetName());
			Result["display_name"] = TCHAR_TO_UTF8(*FoundAnim->GetDisplayName().ToString());

			// UWidgetAnimation::GetStartTime/GetEndTime dereference MovieScene without a null
			// guard (WidgetAnimation.cpp:170/175) — null guard here before touching the timeline.
			if (FoundAnim->GetMovieScene())
			{
				Result["start_time"] = FoundAnim->GetStartTime();
				Result["end_time"] = FoundAnim->GetEndTime();
				Result["duration"] = FoundAnim->GetEndTime() - FoundAnim->GetStartTime();
				Result["has_movie_scene"] = true;
			}
			else
			{
				Result["has_movie_scene"] = false;
			}

			// Animation bindings (which widgets are animated)
			sol::table BindingsTable = Lua.create_table();
			int32 BIdx = 1;
			const TArray<FWidgetAnimationBinding>& AnimBindings = FoundAnim->GetBindings();
			for (const FWidgetAnimationBinding& Binding : AnimBindings)
			{
				sol::table B = Lua.create_table();
				B["widget_name"] = TCHAR_TO_UTF8(*Binding.WidgetName.ToString());
				B["is_root_widget"] = Binding.bIsRootWidget;
				if (!Binding.SlotWidgetName.IsNone())
					B["slot_widget_name"] = TCHAR_TO_UTF8(*Binding.SlotWidgetName.ToString());
				BindingsTable[BIdx++] = B;
			}
			Result["bindings"] = BindingsTable;

			// Tracks from MovieScene
			UMovieScene* MovieScene = FoundAnim->GetMovieScene();
			if (MovieScene)
			{
				sol::table TracksTable = Lua.create_table();
				int32 TIdx = 1;
				const auto& SceneBindings = const_cast<const UMovieScene*>(MovieScene)->GetBindings();
				for (const FMovieSceneBinding& SceneBinding : SceneBindings)
				{
					for (UMovieSceneTrack* Track : SceneBinding.GetTracks())
					{
						if (!Track) continue;
						sol::table T = Lua.create_table();
						T["type"] = TCHAR_TO_UTF8(*Track->GetClass()->GetName());
						T["display_name"] = TCHAR_TO_UTF8(*Track->GetDisplayName().ToString());

						// Section count and ranges
						const TArray<UMovieSceneSection*>& Sections = Track->GetAllSections();
						T["section_count"] = Sections.Num();

						sol::table SectionsTable = Lua.create_table();
						int32 SIdx = 1;
						for (UMovieSceneSection* Section : Sections)
						{
							if (!Section) continue;
							sol::table Sec = Lua.create_table();
							Sec["active"] = Section->IsActive();
							Sec["locked"] = Section->IsLocked();
							Sec["row_index"] = Section->GetRowIndex();
							if (Section->HasStartFrame())
								Sec["start_frame"] = Section->GetInclusiveStartFrame().Value;
							if (Section->HasEndFrame())
								Sec["end_frame"] = Section->GetExclusiveEndFrame().Value;
							SectionsTable[SIdx++] = Sec;
						}
						T["sections"] = SectionsTable;
						TracksTable[TIdx++] = T;
					}
				}

				// Also include master tracks
				for (UMovieSceneTrack* Track : MovieScene->GetTracks())
				{
					if (!Track) continue;
					sol::table T = Lua.create_table();
					T["type"] = TCHAR_TO_UTF8(*Track->GetClass()->GetName());
					T["display_name"] = TCHAR_TO_UTF8(*Track->GetDisplayName().ToString());
					T["is_master"] = true;
					const TArray<UMovieSceneSection*>& Sections = Track->GetAllSections();
					T["section_count"] = Sections.Num();
					TracksTable[TIdx++] = T;
				}

				Result["tracks"] = TracksTable;
			}

			Session.Log(FString::Printf(TEXT("[OK] get_animation(\"%s\")"), *FAnimName));
			return Result;
		});

		// ================================================================
		// add_animation(name) -> create new widget animation
		// ================================================================
		BPObj.set_function("add_animation", [FPath, &Session](sol::table /*self*/,
			const std::string& AnimName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FAnimName = NeoLuaStr::ToFString(AnimName);

			UWidgetBlueprint* WBP = NeoLuaAsset::Resolve<UWidgetBlueprint>(FPath);
			if (!WBP) { Session.Log(TEXT("[FAIL] add_animation -> blueprint not found. Call open_asset(path) to reload, or find_assets(\"WidgetBlueprint\") to discover paths.")); return sol::lua_nil; }

			// Check if animation already exists
			for (UWidgetAnimation* Anim : WBP->Animations)
			{
				if (Anim && Anim->GetName().Equals(FAnimName, ESearchCase::IgnoreCase))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add_animation(\"%s\") -> already exists"), *FAnimName));
					return sol::lua_nil;
				}
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaAddWidgetAnim", "Add Widget Animation"));
			WBP->Modify();

			// Create the animation object
			UWidgetAnimation* NewAnim = NewObject<UWidgetAnimation>(WBP, ::FName(*FAnimName), RF_Transactional);
			if (!NewAnim)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_animation(\"%s\") -> NewObject<UWidgetAnimation> failed. Retry with a unique valid object name (alphanumeric, no leading digits)."), *FAnimName));
				return sol::lua_nil;
			}

			// Create the MovieScene for keyframe storage
			UMovieScene* MovieScene = NewObject<UMovieScene>(NewAnim, ::FName("MovieScene"), RF_Transactional);
			NewAnim->MovieScene = MovieScene;

			NewAnim->SetDisplayLabel(FAnimName);

			// Set default playback range (0 to 5 seconds). Playback ranges are stored
			// in tick-resolution frames, not display-rate frames.
			FFrameRate FrameRate(30, 1);
			FFrameRate TickResolution(24000, 1);
			MovieScene->SetDisplayRate(FrameRate);
			MovieScene->SetTickResolutionDirectly(TickResolution);
			FFrameNumber StartFrame(0);
			FFrameNumber EndFrame = FFrameNumber(static_cast<int32>(5.0 * TickResolution.AsDecimal()));
			MovieScene->SetPlaybackRange(TRange<FFrameNumber>(StartFrame, EndFrame));

			// Matches engine flow: AnimationTabSummoner.cpp lines 306-311
			WBP->Animations.Add(NewAnim);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			WidgetHelper::RegisterWidgetVariableGuidIfMissing(WBP, NewAnim->GetFName());
#endif
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

			sol::table Result = Lua.create_table();
			Result["name"] = TCHAR_TO_UTF8(*NewAnim->GetName());
			Result["display_name"] = TCHAR_TO_UTF8(*NewAnim->GetDisplayName().ToString());
			Result["duration"] = 5.0;

			Session.Log(FString::Printf(TEXT("[OK] add_animation(\"%s\")"), *FAnimName));
			return Result;
		});

		// ================================================================
		// remove_animation(name)
		// ================================================================
		BPObj.set_function("remove_animation", [FPath, &Session](sol::table /*self*/,
			const std::string& AnimName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FAnimName = NeoLuaStr::ToFString(AnimName);

			UWidgetBlueprint* WBP = NeoLuaAsset::Resolve<UWidgetBlueprint>(FPath);
			if (!WBP) { Session.Log(TEXT("[FAIL] remove_animation -> blueprint not found. Call open_asset(path) to reload, or find_assets(\"WidgetBlueprint\") to discover paths.")); return sol::lua_nil; }

			int32 FoundIdx = INDEX_NONE;
			for (int32 i = 0; i < WBP->Animations.Num(); ++i)
			{
				if (WBP->Animations[i] && (WBP->Animations[i]->GetName().Equals(FAnimName, ESearchCase::IgnoreCase) ||
					WBP->Animations[i]->GetDisplayName().ToString().Equals(FAnimName, ESearchCase::IgnoreCase)))
				{
					FoundIdx = i;
					break;
				}
			}
			if (FoundIdx == INDEX_NONE)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove_animation(\"%s\") -> not found"), *FAnimName));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaRemoveWidgetAnim", "Remove Widget Animation"));
			WBP->Modify();

			// Matches engine flow: AnimationTabSummoner.cpp lines 889-898
			UWidgetAnimation* AnimToRemove = WBP->Animations[FoundIdx];
			const FName RemovedAnimName = AnimToRemove->GetFName();
			AnimToRemove->Rename(nullptr, GetTransientPackage());
			WBP->Animations.RemoveAt(FoundIdx);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			WBP->OnVariableRemoved(RemovedAnimName);
#endif
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

			Session.Log(FString::Printf(TEXT("[OK] remove_animation(\"%s\")"), *FAnimName));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// configure_animation(name, opts) -> set timing, display name
		// ================================================================
		BPObj.set_function("configure_animation", [FPath, &Session](sol::table /*self*/,
			const std::string& AnimName, sol::table Opts, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FAnimName = NeoLuaStr::ToFString(AnimName);

			UWidgetBlueprint* WBP = NeoLuaAsset::Resolve<UWidgetBlueprint>(FPath);
			if (!WBP) { Session.Log(TEXT("[FAIL] configure_animation -> blueprint not found. Call open_asset(path) to reload, or find_assets(\"WidgetBlueprint\") to discover paths.")); return sol::lua_nil; }

			UWidgetAnimation* FoundAnim = nullptr;
			for (UWidgetAnimation* Anim : WBP->Animations)
			{
				if (Anim && (Anim->GetName().Equals(FAnimName, ESearchCase::IgnoreCase) ||
					Anim->GetDisplayName().ToString().Equals(FAnimName, ESearchCase::IgnoreCase)))
				{
					FoundAnim = Anim;
					break;
				}
			}
			if (!FoundAnim)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] configure_animation(\"%s\") -> not found"), *FAnimName));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaConfigWidgetAnim", "Configure Widget Animation"));
			FoundAnim->Modify();
			int32 ChangedCount = 0;
			TSet<FString> HandledKeys;

			// Display name
			sol::optional<std::string> DisplayNameOpt = Opts.get<sol::optional<std::string>>("display_name");
			if (DisplayNameOpt.has_value())
			{
				FoundAnim->SetDisplayLabel(NeoLuaStr::ToFStringOpt(DisplayNameOpt));
				ChangedCount++;
				HandledKeys.Add(TEXT("display_name"));
			}

			// Duration / playback range (FFrameNumber is in TickResolution space, NOT DisplayRate)
			UMovieScene* MovieScene = FoundAnim->GetMovieScene();
			if (MovieScene)
			{
				sol::optional<double> DurationOpt = Opts.get<sol::optional<double>>("duration");
				sol::optional<double> StartOpt = Opts.get<sol::optional<double>>("start_time");

				if (DurationOpt.has_value()) HandledKeys.Add(TEXT("duration"));
				if (StartOpt.has_value()) HandledKeys.Add(TEXT("start_time"));

				if (DurationOpt.has_value() || StartOpt.has_value())
				{
					MovieScene->Modify();
					FFrameRate TickRate = MovieScene->GetTickResolution();
					double Start = StartOpt.has_value() ? StartOpt.value() : 0.0;
					double Duration = DurationOpt.has_value() ? DurationOpt.value() : (FoundAnim->GetEndTime() - FoundAnim->GetStartTime());

					FFrameNumber StartFrame = FFrameNumber(static_cast<int32>(Start * TickRate.AsDecimal()));
					FFrameNumber EndFrame = FFrameNumber(static_cast<int32>((Start + Duration) * TickRate.AsDecimal()));
					MovieScene->SetPlaybackRange(TRange<FFrameNumber>(StartFrame, EndFrame));
					ChangedCount++;
				}
			}

			// Warn about any keys we didn't handle so the agent can retry with correct ones
			for (auto& Pair : Opts)
			{
				if (!Pair.first.is<std::string>()) continue;
				FString Key = NeoLuaStr::ToFString(Pair.first.as<std::string>());
				if (!HandledKeys.Contains(Key))
				{
					Session.Log(FString::Printf(TEXT("[WARN] configure_animation(\"%s\") -> key '%s' was not applied. Keys handled this call: %s"),
						*FAnimName, *Key, HandledKeys.Num() > 0 ? *FString::Join(HandledKeys.Array(), TEXT(", ")) : TEXT("(none)")));
				}
			}

			FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

			Session.Log(FString::Printf(TEXT("[OK] configure_animation(\"%s\") -> %d changes"), *FAnimName, ChangedCount));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// rename_animation(old_name, new_name) -> full rename (object, MovieScene, variable GUID, graph refs)
		//
		// configure_animation(display_name=) only touches the display label. To actually rename the
		// animation so the compiler generates the expected variable and graph references track, we
		// need the multi-step engine flow from AnimationTabSummoner.cpp:273-302:
		//   1. Modify the animation + its MovieScene
		//   2. SetDisplayLabel(new)
		//   3. Rename the UWidgetAnimation to NewName
		//   4. Rename the MovieScene to NewName
		//   5. OnVariableRenamed(old, new) — updates WidgetVariableNameToGuidMap
		//   6. ReplaceVariableReferences — graph nodes referencing the old variable name
		//   7. MarkBlueprintAsStructurallyModified
		// ================================================================
		BPObj.set_function("rename_animation", [FPath, &Session](sol::table /*self*/,
			const std::string& OldName, const std::string& NewName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FOldName = NeoLuaStr::ToFString(OldName);
			FString FNewName = NeoLuaStr::ToFString(NewName);

			UWidgetBlueprint* WBP = NeoLuaAsset::Resolve<UWidgetBlueprint>(FPath);
			if (!WBP) { Session.Log(TEXT("[FAIL] rename_animation -> blueprint not found. Call open_asset(path) to reload, or find_assets(\"WidgetBlueprint\") to discover paths.")); return sol::lua_nil; }

			if (FNewName.IsEmpty())
			{
				Session.Log(TEXT("[FAIL] rename_animation -> new name is empty"));
				return sol::lua_nil;
			}

			// Resolve target animation (match by object name or display label)
			UWidgetAnimation* FoundAnim = nullptr;
			for (UWidgetAnimation* Anim : WBP->Animations)
			{
				if (Anim && (Anim->GetName().Equals(FOldName, ESearchCase::IgnoreCase) ||
					Anim->GetDisplayName().ToString().Equals(FOldName, ESearchCase::IgnoreCase)))
				{
					FoundAnim = Anim;
					break;
				}
			}
			if (!FoundAnim)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] rename_animation(\"%s\") -> animation not found. Call list_animations() to discover valid animation names."), *FOldName));
				return sol::lua_nil;
			}

			// Sanitize new name (widget animations share the blueprint namespace)
			FString SanitizedName = SlugStringForValidName(FNewName);
			if (SanitizedName.IsEmpty()) SanitizedName = FNewName;
			const FName NewFName(*SanitizedName);
			const FName OldFName = FoundAnim->GetFName();

			if (NewFName == OldFName)
			{
				// Display label can still change even if object name is unchanged.
				const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaRenameAnimSameName", "Rename Widget Animation (label)"));
				FoundAnim->Modify();
				FoundAnim->SetDisplayLabel(FNewName);
				FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
				Session.Log(FString::Printf(TEXT("[OK] rename_animation(\"%s\") -> display label only (object name unchanged)"), *FOldName));
				return sol::make_object(Lua, true);
			}

			// Uniqueness check against widgets, animations, and blueprint variables via FKismetNameValidator.
			FKismetNameValidator Validator(WBP, OldFName);
			const EValidatorResult VR = Validator.IsValid(NewFName);
			if (VR != EValidatorResult::Ok)
			{
				const TCHAR* Reason =
					VR == EValidatorResult::AlreadyInUse           ? TEXT("name already in use") :
					VR == EValidatorResult::ExistingName           ? TEXT("same name as current") :
					VR == EValidatorResult::LocallyInUse           ? TEXT("name locally in use") :
					VR == EValidatorResult::ContainsInvalidCharacters ? TEXT("contains invalid characters") :
					VR == EValidatorResult::TooLong                ? TEXT("name too long") :
					VR == EValidatorResult::EmptyName              ? TEXT("empty name") :
					TEXT("invalid");
				Session.Log(FString::Printf(
					TEXT("[FAIL] rename_animation(\"%s\", \"%s\") -> sanitized name '%s' is %s. Retry with a unique alphanumeric name; call list_animations() to inspect existing names."),
					*FOldName, *FNewName, *NewFName.ToString(), Reason));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaRenameAnim", "Rename Widget Animation"));
			FoundAnim->Modify();
			if (UMovieScene* MovieScene = FoundAnim->GetMovieScene())
			{
				MovieScene->Modify();
			}
			WBP->Modify();

			FoundAnim->SetDisplayLabel(FNewName);
			FoundAnim->Rename(*NewFName.ToString());
			if (UMovieScene* MovieScene = FoundAnim->GetMovieScene())
			{
				MovieScene->Rename(*NewFName.ToString());
			}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			WBP->OnVariableRenamed(OldFName, NewFName);
#endif

			FBlueprintEditorUtils::ReplaceVariableReferences(WBP, OldFName, NewFName);
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

			Session.Log(FString::Printf(TEXT("[OK] rename_animation(\"%s\" -> \"%s\")"), *FOldName, *NewFName.ToString()));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// list_bindings() -> property bindings
		// ================================================================
		BPObj.set_function("list_bindings", [FPath, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			UWidgetBlueprint* WBP = NeoLuaAsset::Resolve<UWidgetBlueprint>(FPath);
			if (!WBP) { Session.Log(TEXT("[FAIL] list_bindings -> blueprint not found. Call open_asset(path) to reload, or find_assets(\"WidgetBlueprint\") to discover paths.")); return sol::lua_nil; }

			sol::table Result = Lua.create_table();
			int32 Idx = 1;

			for (const FDelegateEditorBinding& Binding : WBP->Bindings)
			{
				sol::table B = Lua.create_table();
				B["object_name"] = TCHAR_TO_UTF8(*Binding.ObjectName);
				B["property_name"] = TCHAR_TO_UTF8(*Binding.PropertyName.ToString());
				B["function_name"] = TCHAR_TO_UTF8(*Binding.FunctionName.ToString());
				B["source_property"] = TCHAR_TO_UTF8(*Binding.SourceProperty.ToString());
				B["kind"] = (Binding.Kind == EBindingKind::Function) ? "Function" : "Property";
				Result[Idx++] = B;
			}

			Session.Log(FString::Printf(TEXT("[OK] list_bindings() -> %d bindings"), Idx - 1));
			return Result;
		});

		// ================================================================
		// add_binding({object_name, property_name, function_name?, source_property?, kind?})
		// ================================================================
		BPObj.set_function("add_binding", [FPath, &Session](sol::table /*self*/,
			sol::table Opts, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			UWidgetBlueprint* WBP = NeoLuaAsset::Resolve<UWidgetBlueprint>(FPath);
			if (!WBP) { Session.Log(TEXT("[FAIL] add_binding -> blueprint not found. Call open_asset(path) to reload, or find_assets(\"WidgetBlueprint\") to discover paths.")); return sol::lua_nil; }

			std::string ObjName = Opts.get_or<std::string>("object_name", "");
			std::string PropName = Opts.get_or<std::string>("property_name", "");
			if (ObjName.empty() || PropName.empty())
			{
				Session.Log(TEXT("[FAIL] add_binding -> 'object_name' and 'property_name' required"));
				return sol::lua_nil;
			}

			UWidget* TargetWidget = nullptr;

			// Check widget exists and the requested target property is actually bindable.
			if (WBP->WidgetTree)
			{
				UWidget* W = WBP->WidgetTree->FindWidget(::FName(NeoLuaStr::ToFString(ObjName)));
				if (!W)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add_binding -> widget '%s' not found"),
						UTF8_TO_TCHAR(ObjName.c_str())));
					return sol::lua_nil;
				}
				TargetWidget = W;
			}
			if (!TargetWidget)
			{
				Session.Log(TEXT("[FAIL] add_binding -> widget tree missing or target widget could not be resolved"));
				return sol::lua_nil;
			}

			bool bIsAttributeProperty = false;
			FDelegateProperty* TargetDelegateProperty = WidgetHelper::FindBindableDelegateProperty(
				TargetWidget, ::FName(NeoLuaStr::ToFString(PropName)), bIsAttributeProperty);
			if (!TargetDelegateProperty)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_binding(%s.%s) -> property is not bindable on %s. Use get_widget(name).props to inspect properties with binding delegates."),
					UTF8_TO_TCHAR(ObjName.c_str()), UTF8_TO_TCHAR(PropName.c_str()), *TargetWidget->GetClass()->GetName()));
				return sol::lua_nil;
			}

			const std::string KindStr = Opts.get_or<std::string>("kind", "Property");
			const bool bFunctionKind = (KindStr == "Function");
			const FString FunctionNameStr = NeoLuaStr::ToFString(Opts.get_or<std::string>("function_name", ""));
			const FString SourcePropertyStr = NeoLuaStr::ToFString(Opts.get_or<std::string>("source_property", ""));

			// Engine runtime binding (WidgetBlueprint.cpp:547-563) reads SourcePath + MemberGuid, NOT
			// SourceProperty. Resolve them now so the binding actually delivers values at runtime.
			UClass* SearchClass = WBP->SkeletonGeneratedClass ? WBP->SkeletonGeneratedClass : WBP->GeneratedClass;
			if (!SearchClass)
			{
				Session.Log(TEXT("[FAIL] add_binding -> blueprint has no SkeletonGeneratedClass; compile the Widget Blueprint first and retry."));
				return sol::lua_nil;
			}

			FDelegateEditorBinding NewBinding;
			NewBinding.ObjectName = NeoLuaStr::ToFString(ObjName);
			NewBinding.PropertyName = ::FName(NeoLuaStr::ToFString(PropName));
			NewBinding.FunctionName = ::FName(*FunctionNameStr);
			NewBinding.SourceProperty = ::FName(*SourcePropertyStr);
			NewBinding.Kind = bFunctionKind ? EBindingKind::Function : EBindingKind::Property;

			if (bFunctionKind)
			{
				// For function bindings, rename-safe lookup uses MemberGuid (ToRuntimeBinding falls
				// back to FunctionName string when the GUID isn't set). Resolve via the blueprint's
				// property-GUID provider chain.
				if (!FunctionNameStr.IsEmpty())
				{
					FGuid FoundGuid;
					if (UBlueprint::GetGuidFromClassByFieldName<UFunction>(SearchClass, NewBinding.FunctionName, FoundGuid))
					{
						NewBinding.MemberGuid = FoundGuid;
					}
					// Also verify the function exists — bind to a missing function silently produces a no-op.
					if (!SearchClass->FindFunctionByName(NewBinding.FunctionName))
					{
						Session.Log(FString::Printf(TEXT("[WARN] add_binding -> function '%s' not found on %s; binding recorded but may not resolve at compile time."),
							*FunctionNameStr, *SearchClass->GetName()));
					}
				}
				else
				{
					Session.Log(TEXT("[FAIL] add_binding -> kind=Function requires 'function_name'"));
					return sol::lua_nil;
				}
			}
			else
			{
				// For property bindings, runtime uses SourcePath (FEditorPropertyPath). Build a one-segment
				// path from the requested source property on the blueprint's skeleton class. Without this,
				// ToRuntimeBinding produces an empty path and the binding never fires.
				if (!SourcePropertyStr.IsEmpty())
				{
					FProperty* SrcProp = FindFProperty<FProperty>(SearchClass, ::FName(*SourcePropertyStr));
					if (SrcProp)
					{
						TArray<FFieldVariant> Chain;
						Chain.Add(FFieldVariant(SrcProp));
						NewBinding.SourcePath = FEditorPropertyPath(Chain);
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] add_binding -> source_property '%s' not found on %s; SourcePath will be empty and the runtime binding will not deliver a value."),
							*SourcePropertyStr, *SearchClass->GetName()));
					}
				}
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaAddBinding", "Add Widget Binding"));
			WBP->Modify();

			// Remove existing binding for the same object+property if any
			WBP->Bindings.RemoveAll([&](const FDelegateEditorBinding& Existing)
			{
				return Existing.ObjectName == NewBinding.ObjectName &&
					Existing.PropertyName == NewBinding.PropertyName;
			});

			WBP->Bindings.Add(NewBinding);
			FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

			Session.Log(FString::Printf(TEXT("[OK] add_binding(%s.%s) kind=%s%s"),
				UTF8_TO_TCHAR(ObjName.c_str()), *NewBinding.PropertyName.ToString(),
				bFunctionKind ? TEXT("Function") : TEXT("Property"),
				bFunctionKind
					? (NewBinding.MemberGuid.IsValid() ? TEXT(" [guid-tracked]") : TEXT(" [name-only]"))
					: (NewBinding.SourcePath.IsEmpty() ? TEXT(" [no SourcePath]") : TEXT(" [SourcePath set]"))));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// remove_binding({object_name, property_name})
		// ================================================================
		BPObj.set_function("remove_binding", [FPath, &Session](sol::table /*self*/,
			sol::table Opts, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			UWidgetBlueprint* WBP = NeoLuaAsset::Resolve<UWidgetBlueprint>(FPath);
			if (!WBP) { Session.Log(TEXT("[FAIL] remove_binding -> blueprint not found. Call open_asset(path) to reload, or find_assets(\"WidgetBlueprint\") to discover paths.")); return sol::lua_nil; }

			std::string ObjName = Opts.get_or<std::string>("object_name", "");
			std::string PropName = Opts.get_or<std::string>("property_name", "");

			if (ObjName.empty() && PropName.empty())
			{
				Session.Log(TEXT("[FAIL] remove_binding -> at least 'object_name' or 'property_name' required"));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaRemoveBinding", "Remove Widget Binding"));
			WBP->Modify();

			FString FObjName = NeoLuaStr::ToFString(ObjName);
			FName FPropName = ::FName(NeoLuaStr::ToFString(PropName));

			int32 Removed = WBP->Bindings.RemoveAll([&](const FDelegateEditorBinding& Existing)
			{
				if (!ObjName.empty() && Existing.ObjectName != FObjName) return false;
				if (!PropName.empty() && Existing.PropertyName != FPropName) return false;
				return true;
			});

			if (Removed == 0)
			{
				Session.Log(TEXT("[FAIL] remove_binding -> no matching binding found"));
				return sol::lua_nil;
			}

			FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

			Session.Log(FString::Printf(TEXT("[OK] remove_binding -> removed %d"), Removed));
			return sol::make_object(Lua, Removed);
		});

		// ================================================================
		// list_named_slots() -> named slots with content
		// ================================================================
		BPObj.set_function("list_named_slots", [FPath, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			UWidgetBlueprint* WBP = NeoLuaAsset::Resolve<UWidgetBlueprint>(FPath);
			if (!WBP || !WBP->WidgetTree)
			{
				Session.Log(TEXT("[FAIL] list_named_slots -> no widget tree. The blueprint has no UWidgetTree — open_asset(path) with a valid WidgetBlueprint, or call add_widget(type) to seed a root widget."));
				return sol::lua_nil;
			}

			sol::table Result = Lua.create_table();
			int32 Idx = 1;

			// Find all UNamedSlot widgets in the tree
			TArray<UWidget*> AllWidgets;
			WBP->WidgetTree->GetAllWidgets(AllWidgets);

			for (UWidget* Widget : AllWidgets)
			{
				UNamedSlot* NamedSlot = Cast<UNamedSlot>(Widget);
				if (!NamedSlot) continue;

				sol::table Slot = Lua.create_table();
				Slot["name"] = TCHAR_TO_UTF8(*NamedSlot->GetName());
#if WITH_EDITORONLY_DATA
				Slot["expose_on_instance_only"] = NamedSlot->bExposeOnInstanceOnly;
#endif

				// Check if slot has content
				if (NamedSlot->GetChildrenCount() > 0)
				{
					UWidget* Content = NamedSlot->GetChildAt(0);
					if (Content)
					{
						Slot["content"] = TCHAR_TO_UTF8(*Content->GetName());
						Slot["content_name"] = TCHAR_TO_UTF8(*Content->GetName());
						Slot["content_type"] = TCHAR_TO_UTF8(*Content->GetClass()->GetName());
					}
				}
				else
				{
					Slot["content"] = sol::lua_nil;
					Slot["content_name"] = sol::lua_nil;
				}

				// Parent info
				int32 ChildIndex;
				UPanelWidget* Parent = UWidgetTree::FindWidgetParent(NamedSlot, ChildIndex);
				if (Parent)
				{
					Slot["parent"] = TCHAR_TO_UTF8(*Parent->GetName());
				}

				Result[Idx++] = Slot;
			}

			Session.Log(FString::Printf(TEXT("[OK] list_named_slots() -> %d slots"), Idx - 1));
			return Result;
		});

		// ================================================================
		// add_named_slot(name, opts?) -> create NamedSlot widget
		// ================================================================
		BPObj.set_function("add_named_slot", [FPath, &Session](sol::table /*self*/,
			const std::string& SlotName, sol::optional<sol::table> OptsOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FSlotName = NeoLuaStr::ToFString(SlotName);

			UWidgetBlueprint* WBP = NeoLuaAsset::Resolve<UWidgetBlueprint>(FPath);
			if (!WBP || !WBP->WidgetTree)
			{
				Session.Log(TEXT("[FAIL] add_named_slot -> no widget tree. The blueprint has no UWidgetTree — open_asset(path) with a valid WidgetBlueprint, or call add_widget(type) to seed a root widget."));
				return sol::lua_nil;
			}

			// Pre-flight uniqueness — see IsNameSafeForWidget for why the WidgetTree check
			// alone isn't enough (GuidMap can hold stale entries that trip OnVariableAdded
			// at line 1747 AFTER construction).
			{
				FString PreflightReason;
				if (!WidgetHelper::IsNameSafeForWidget(WBP, ::FName(*FSlotName), /*WidgetBeingRenamed=*/nullptr, PreflightReason))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add_named_slot(\"%s\") -> %s"), *FSlotName, *PreflightReason));
					return sol::lua_nil;
				}
			}

			// Parse opts
			FString ParentName;
			bool bExposeOnInstanceOnly = false;
			bool bHasExposeFlag = false;
			if (OptsOpt.has_value())
			{
				sol::table Opts = OptsOpt.value();
				sol::optional<std::string> ParentOpt = Opts.get<sol::optional<std::string>>("parent");
				if (ParentOpt.has_value())
					ParentName = NeoLuaStr::ToFStringOpt(ParentOpt);
				sol::optional<bool> ExposeOpt = Opts.get<sol::optional<bool>>("expose_on_instance_only");
				if (ExposeOpt.has_value())
				{
					bExposeOnInstanceOnly = ExposeOpt.value();
					bHasExposeFlag = true;
				}
			}

			// ── Resolve + preflight target BEFORE ConstructWidget ────────────────
			// Prevents a stray NewSlot outered to WidgetTree on validation failure.
			enum class EAttachMode { IntoPanel, AsRoot };
			EAttachMode AttachMode;
			UPanelWidget* TargetPanel = nullptr;

			if (!ParentName.IsEmpty())
			{
				TargetPanel = Cast<UPanelWidget>(WBP->WidgetTree->FindWidget(::FName(*ParentName)));
				if (!TargetPanel)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add_named_slot -> parent '%s' not found or not a panel"), *ParentName));
					return sol::lua_nil;
				}
				if (!TargetPanel->CanAddMoreChildren())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add_named_slot -> parent '%s' cannot accept more children"), *ParentName));
					return sol::lua_nil;
				}
				AttachMode = EAttachMode::IntoPanel;
			}
			else if (WBP->WidgetTree->RootWidget)
			{
				TargetPanel = Cast<UPanelWidget>(WBP->WidgetTree->RootWidget);
				if (!TargetPanel)
				{
					Session.Log(TEXT("[FAIL] add_named_slot -> root is not a panel and no parent specified. Pass opts={parent=\"<panel_name>\"}."));
					return sol::lua_nil;
				}
				if (!TargetPanel->CanAddMoreChildren())
				{
					Session.Log(TEXT("[FAIL] add_named_slot -> root panel cannot accept more children. Pass opts={parent=\"<panel_name>\"} to target a different panel."));
					return sol::lua_nil;
				}
				AttachMode = EAttachMode::IntoPanel;
			}
			else
			{
				AttachMode = EAttachMode::AsRoot;
			}

			// ── Now construct and attach ─────────────────────────────────────────
			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaAddNamedSlot", "Add Named Slot"));
			WBP->WidgetTree->SetFlags(RF_Transactional);
			WBP->WidgetTree->Modify();

			UNamedSlot* NewSlot = WBP->WidgetTree->ConstructWidget<UNamedSlot>(UNamedSlot::StaticClass(), ::FName(*FSlotName));
			if (!NewSlot)
			{
				Session.Log(TEXT("[FAIL] add_named_slot -> construction failed"));
				return sol::lua_nil;
			}

#if WITH_EDITORONLY_DATA
			if (bHasExposeFlag)
			{
				NewSlot->bExposeOnInstanceOnly = bExposeOnInstanceOnly;
			}
#endif

			if (AttachMode == EAttachMode::IntoPanel)
			{
				TargetPanel->Modify();
				UPanelSlot* Slot = TargetPanel->AddChild(NewSlot);
				if (!Slot)
				{
					NewSlot->Rename(nullptr, GetTransientPackage());
					Session.Log(FString::Printf(TEXT("[FAIL] add_named_slot(\"%s\") -> AddChild returned null; widget discarded"), *FSlotName));
					return sol::lua_nil;
				}
			}
			else
			{
				WBP->WidgetTree->RootWidget = NewSlot;
			}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			WidgetHelper::RegisterWidgetVariableGuidIfMissing(WBP, NewSlot->GetFName());
#endif
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

			sol::table Result = Lua.create_table();
			Result["name"] = TCHAR_TO_UTF8(*NewSlot->GetName());
			Result["type"] = "NamedSlot";

			Session.Log(FString::Printf(TEXT("[OK] add_named_slot(\"%s\")"), *FSlotName));
			return Result;
		});

		// ================================================================
		// remove_named_slot(name)
		// ================================================================
		BPObj.set_function("remove_named_slot", [FPath, &Session](sol::table /*self*/,
			const std::string& SlotName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FSlotName = NeoLuaStr::ToFString(SlotName);

			UWidgetBlueprint* WBP = NeoLuaAsset::Resolve<UWidgetBlueprint>(FPath);
			if (!WBP || !WBP->WidgetTree)
			{
				Session.Log(TEXT("[FAIL] remove_named_slot -> no widget tree. The blueprint has no UWidgetTree — open_asset(path) with a valid WidgetBlueprint, or call add_widget(type) to seed a root widget."));
				return sol::lua_nil;
			}

			UWidget* Widget = WBP->WidgetTree->FindWidget(::FName(*FSlotName));
			UNamedSlot* NamedSlot = Cast<UNamedSlot>(Widget);
			if (!NamedSlot)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove_named_slot(\"%s\") -> not found or not a NamedSlot"), *FSlotName));
				return sol::lua_nil;
			}

			// Delegate to the engine helper so child content (if any) also gets transient-renamed,
			// variable-node-cleaned, and GUID-map-cleared. Our previous manual path removed only the
			// slot itself and left child widgets orphaned with stale WidgetVariableNameToGuidMap
			// entries. DeleteWidgets (WidgetBlueprintEditorUtils.cpp:572) recurses children via
			// UWidgetTree::GetChildWidgets and cleans each one (lines 650-672).
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaRemoveNamedSlot", "Remove Named Slot"));
			TSet<UWidget*> ToDelete;
			ToDelete.Add(NamedSlot);
			FWidgetBlueprintEditorUtils::DeleteWidgets(
				WBP, ToDelete,
				FWidgetBlueprintEditorUtils::EDeleteWidgetWarningType::DeleteSilently);
#else
			// Pre-5.6: DeleteWidgets(BP, TSet<UWidget*>, EDeleteWidgetWarningType) did not exist;
			// the only public variant required an editor session. Fall back to the manual flow
			// (which leaks child cleanup, same limitation as the pre-5.6 base remove_widget).
			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaRemoveNamedSlot", "Remove Named Slot"));
			WBP->WidgetTree->SetFlags(RF_Transactional);
			WBP->WidgetTree->Modify();
			int32 ChildIdx;
			UPanelWidget* Parent = UWidgetTree::FindWidgetParent(NamedSlot, ChildIdx);
			if (Parent) { Parent->Modify(); Parent->RemoveChild(NamedSlot); }
			else if (NamedSlot == WBP->WidgetTree->RootWidget) { WBP->WidgetTree->RootWidget = nullptr; }
			const FName SlotFName = NamedSlot->GetFName();
			NamedSlot->Rename(nullptr, GetTransientPackage());
			WBP->WidgetTree->RemoveWidget(NamedSlot);
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
#endif

			Session.Log(FString::Printf(TEXT("[OK] remove_named_slot(\"%s\")"), *FSlotName));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// set_named_slot_content(slot_name, widget_name)
		// ================================================================
		BPObj.set_function("set_named_slot_content", [FPath, &Session](sol::table /*self*/,
			const std::string& SlotName, sol::object WidgetObj, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FSlotName = NeoLuaStr::ToFString(SlotName);

			UWidgetBlueprint* WBP = NeoLuaAsset::Resolve<UWidgetBlueprint>(FPath);
			if (!WBP || !WBP->WidgetTree)
			{
				Session.Log(TEXT("[FAIL] set_named_slot_content -> no widget tree. The blueprint has no UWidgetTree — open_asset(path) with a valid WidgetBlueprint, or call add_widget(type) to seed a root widget."));
				return sol::lua_nil;
			}

			UWidget* SlotWidget = WBP->WidgetTree->FindWidget(::FName(*FSlotName));
			UNamedSlot* NamedSlot = Cast<UNamedSlot>(SlotWidget);
			if (!NamedSlot)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] set_named_slot_content(\"%s\") -> not found or not a NamedSlot"), *FSlotName));
				return sol::lua_nil;
			}

			// ── Validate arguments + cycle-safety BEFORE opening a transaction ────
			// so rejected calls don't leave empty undo entries in the user's history.
			const bool bClearRequested = (WidgetObj.get_type() == sol::type::lua_nil);
			UWidget* ContentWidget = nullptr;
			FString FWidgetName;
			if (!bClearRequested)
			{
				if (!WidgetObj.is<std::string>())
				{
					Session.Log(TEXT("[FAIL] set_named_slot_content -> second arg must be widget name string or nil"));
					return sol::lua_nil;
				}
				FWidgetName = NeoLuaStr::ToFString(WidgetObj.as<std::string>());
				ContentWidget = WBP->WidgetTree->FindWidget(::FName(*FWidgetName));
				if (!ContentWidget)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] set_named_slot_content -> widget '%s' not found. Call list_widgets() to discover valid names."), *FWidgetName));
					return sol::lua_nil;
				}

				// Cycle-safety: refuse any assignment that would form a loop in the widget tree.
				// UWidgetTree::ForWidgetAndChildren (Runtime/UMG/Private/WidgetTree.cpp:234) recurses
				// through panel children and named-slot content with no visited-set guard, so a cycle
				// hangs every subsequent list_widgets / remove_widget call.
				if (ContentWidget == NamedSlot)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] set_named_slot_content(\"%s\", \"%s\") -> slot cannot contain itself"),
						*FSlotName, *FWidgetName));
					return sol::lua_nil;
				}
				if (ContentWidget == WBP->WidgetTree->RootWidget)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] set_named_slot_content(\"%s\", \"%s\") -> cannot place the root widget inside a named slot (would orphan the tree)"),
						*FSlotName, *FWidgetName));
					return sol::lua_nil;
				}
				// Walk up from NamedSlot; if ContentWidget is an ancestor, reject.
				// UWidgetTree::FindWidgetParent only returns UPanelWidget parents — UNamedSlot is
				// itself a UPanelWidget (UContentWidget base), so the walk correctly catches
				// cycles formed through nested named slots inside the same widget tree.
				{
					UWidget* Walk = NamedSlot;
					while (Walk)
					{
						if (Walk == ContentWidget)
						{
							Session.Log(FString::Printf(TEXT("[FAIL] set_named_slot_content(\"%s\", \"%s\") -> content widget is an ancestor of the slot (would create a cycle)"),
								*FSlotName, *FWidgetName));
							return sol::lua_nil;
						}
						int32 WalkIdx;
						Walk = UWidgetTree::FindWidgetParent(Walk, WalkIdx);
					}
				}
			}

			// ── All validation passed — now open the transaction and mutate ──────
			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaSetSlotContent", "Set Named Slot Content"));
			WBP->WidgetTree->Modify();
			NamedSlot->Modify();

			if (bClearRequested)
			{
				if (NamedSlot->GetChildrenCount() > 0)
				{
					NamedSlot->ClearChildren();
				}
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
				Session.Log(FString::Printf(TEXT("[OK] set_named_slot_content(\"%s\") -> cleared"), *FSlotName));
				return sol::make_object(Lua, true);
			}

			// Remove from current parent first
			int32 OldIdx;
			UPanelWidget* OldParent = UWidgetTree::FindWidgetParent(ContentWidget, OldIdx);
			if (OldParent)
			{
				OldParent->Modify();
				OldParent->RemoveChild(ContentWidget);
			}

			// Clear existing content and add new. UNamedSlot is single-child (UContentWidget),
			// so ClearChildren guarantees AddChild below succeeds.
			NamedSlot->ClearChildren();
			NamedSlot->AddChild(ContentWidget);

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

			Session.Log(FString::Printf(TEXT("[OK] set_named_slot_content(\"%s\", \"%s\")"), *FSlotName, *FWidgetName));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// add_widget(type, opts?) -> create and add widget to tree
		// ================================================================
		BPObj.set_function("add_widget", [FPath, &Session](sol::table /*self*/,
			const std::string& TypeName, sol::optional<sol::table> OptsOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FTypeName = NeoLuaStr::ToFString(TypeName);

			UWidgetBlueprint* WBP = NeoLuaAsset::Resolve<UWidgetBlueprint>(FPath);
			if (!WBP || !WBP->WidgetTree)
			{
				Session.Log(TEXT("[FAIL] add_widget -> no widget tree. The blueprint has no UWidgetTree — open_asset(path) with a valid WidgetBlueprint, or call add_widget(type) to seed a root widget."));
				return sol::lua_nil;
			}

			// Resolve widget class — FindClassRobust covers bare name + U prefix;
			// +"Widget" suffix remains a separate fallback for legacy widget naming.
			UClass* WidgetClass = NeoTypeResolver::FindClassRobust(FTypeName);
			if (!WidgetClass)
				WidgetClass = NeoTypeResolver::FindClassRobust(FTypeName + TEXT("Widget"));

			if (!WidgetClass || !WidgetClass->IsChildOf(UWidget::StaticClass()))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_widget(\"%s\") -> widget class not found. Try a UWidget subclass name such as Button, TextBlock, Image, CanvasPanel, VerticalBox, or pass a full class path like /Script/UMG.Button."), *FTypeName));
				return sol::lua_nil;
			}

			// Parse options
			FString WidgetName;
			FString ParentName;
			int32 InsertIndex = INDEX_NONE;
			bool bIsVariable = false;

			if (OptsOpt.has_value())
			{
				sol::table Opts = OptsOpt.value();
				sol::optional<std::string> NameOpt = Opts.get<sol::optional<std::string>>("name");
				if (NameOpt.has_value())
					WidgetName = NeoLuaStr::ToFStringOpt(NameOpt);

				sol::optional<std::string> ParentOpt = Opts.get<sol::optional<std::string>>("parent");
				if (ParentOpt.has_value())
					ParentName = NeoLuaStr::ToFStringOpt(ParentOpt);

				sol::optional<int> IndexOpt = Opts.get<sol::optional<int>>("index");
				if (IndexOpt.has_value())
					InsertIndex = IndexOpt.value();

				sol::optional<bool> VarOpt = Opts.get<sol::optional<bool>>("is_variable");
				if (VarOpt.has_value())
					bIsVariable = VarOpt.value();
			}

			FName FWidgetName;
			if (WidgetName.IsEmpty())
			{
				FString PreflightReason;
				FWidgetName = WidgetHelper::MakeUniqueSafeWidgetName(WBP, WidgetClass, PreflightReason);
				if (FWidgetName.IsNone())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add_widget -> %s"), *PreflightReason));
					return sol::lua_nil;
				}
			}
			else
			{
				FWidgetName = ::FName(*WidgetName);
				FString PreflightReason;
				if (!WidgetHelper::IsNameSafeForWidget(WBP, FWidgetName, /*WidgetBeingRenamed=*/nullptr, PreflightReason))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add_widget -> %s"), *PreflightReason));
					return sol::lua_nil;
				}
			}

			// ── Resolve + preflight the target parent BEFORE ConstructWidget ─────
			// so a failed preflight doesn't leave a stray UObject outered to WidgetTree.
			enum class EAttachMode { IntoPanel, AsRoot };
			EAttachMode AttachMode;
			UPanelWidget* TargetPanel = nullptr;

			if (!ParentName.IsEmpty())
			{
				TargetPanel = Cast<UPanelWidget>(WBP->WidgetTree->FindWidget(::FName(*ParentName)));
				if (!TargetPanel)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add_widget -> parent '%s' not found or not a panel"), *ParentName));
					return sol::lua_nil;
				}
				if (!TargetPanel->CanAddMoreChildren())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add_widget -> parent '%s' cannot accept more children"), *ParentName));
					return sol::lua_nil;
				}
				AttachMode = EAttachMode::IntoPanel;
			}
			else if (WBP->WidgetTree->RootWidget)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_widget(\"%s\") -> root widget '%s' already exists. Pass opts={parent=\"<panel_name>\"} to choose the parent explicitly."),
					*FTypeName, *WBP->WidgetTree->RootWidget->GetName()));
				return sol::lua_nil;
			}
			else
			{
				AttachMode = EAttachMode::AsRoot;
			}

			// ── Now it's safe to open the transaction and construct ──────────────
			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaAddWidget", "Add Widget"));
			WBP->WidgetTree->SetFlags(RF_Transactional);
			WBP->WidgetTree->Modify();

			UWidget* NewWidget = WBP->WidgetTree->ConstructWidget<UWidget>(WidgetClass, FWidgetName);
			if (!NewWidget)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add_widget(\"%s\") -> construction failed"), *FTypeName));
				return sol::lua_nil;
			}

			NewWidget->CreatedFromPalette();
			NewWidget->bIsVariable = bIsVariable;

			if (AttachMode == EAttachMode::IntoPanel)
			{
				TargetPanel->Modify();
				UPanelSlot* Slot = (InsertIndex >= 0)
					? TargetPanel->InsertChildAt(FMath::Min(InsertIndex, TargetPanel->GetChildrenCount()), NewWidget)
					: TargetPanel->AddChild(NewWidget);
				if (!Slot)
				{
					// Preflight should have prevented this; keep a defensive cleanup anyway.
					NewWidget->Rename(nullptr, GetTransientPackage());
					Session.Log(FString::Printf(TEXT("[FAIL] add_widget(\"%s\") -> InsertChildAt/AddChild returned null; widget discarded"), *FTypeName));
					return sol::lua_nil;
				}
			}
			else // AsRoot
			{
				WBP->WidgetTree->RootWidget = NewWidget;
			}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			WidgetHelper::RegisterWidgetVariableGuidIfMissing(WBP, NewWidget->GetFName());
#endif
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

			sol::table Result = WidgetHelper::WidgetToTable(Lua, NewWidget, WBP);
			Session.Log(FString::Printf(TEXT("[OK] add_widget(\"%s\") -> \"%s\""),
				*FTypeName, *NewWidget->GetName()));
			return Result;
		});

		// remove_widget is inherited from the base Blueprint binding, which delegates to
		// FWidgetBlueprintEditorUtils::DeleteWidgets (WidgetBlueprintEditorUtils.cpp:572).
		// The engine helper handles RemoveVariableNodes, FindAndRemoveNamedSlotContent,
		// ReplaceDesiredFocus, transient rename, OnVariableRemoved, and child recursion —
		// all of which the old local override partially reimplemented and diverged on.

		// ================================================================
		// configure_widget(name, opts) -> set properties, visibility, is_variable
		// ================================================================
		BPObj.set_function("configure_widget", [FPath, &Session](sol::table /*self*/,
			const std::string& WidgetName, sol::table Opts, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FWidgetName = NeoLuaStr::ToFString(WidgetName);

			UWidgetBlueprint* WBP = NeoLuaAsset::Resolve<UWidgetBlueprint>(FPath);
			if (!WBP || !WBP->WidgetTree)
			{
				Session.Log(TEXT("[FAIL] configure_widget -> no widget tree. The blueprint has no UWidgetTree — open_asset(path) with a valid WidgetBlueprint, or call add_widget(type) to seed a root widget."));
				return sol::lua_nil;
			}

			UWidget* Widget = WBP->WidgetTree->FindWidget(::FName(*FWidgetName));
			if (!Widget)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] configure_widget(\"%s\") -> not found"), *FWidgetName));
				return sol::lua_nil;
			}

			FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaConfigWidget", "Configure Widget"));
			Widget->Modify();
			int32 ChangedCount = 0;
			int32 WarningCount = 0;
			bool bStructuralChange = false;

			// Special keys handled explicitly
			static const TSet<FString> SpecialKeys = { TEXT("is_variable"), TEXT("visibility"), TEXT("tooltip"), TEXT("properties"), TEXT("slot") };

			// Helper: convert a sol::object to ImportText string (handles scalars + nested tables)
			std::function<TOptional<FString>(const sol::object&)> SolValueToImportText;
			SolValueToImportText = [&SolValueToImportText](const sol::object& Val) -> TOptional<FString>
			{
				if (Val.is<std::string>())
					return NeoLuaStr::ToFString(Val.as<std::string>());
				if (Val.is<bool>())
					return FString(Val.as<bool>() ? TEXT("True") : TEXT("False"));
				if (Val.is<int>())
					return FString::FromInt(Val.as<int>());
				if (Val.is<double>())
					return FString::SanitizeFloat(Val.as<double>());
				if (Val.is<sol::table>())
				{
					// Convert table to UE struct ImportText format: (Key=Val,Key=Val,...)
					// Supports nested tables for nested structs (e.g. FSlateColor.SpecifiedColor)
					sol::table T = Val.as<sol::table>();
					TArray<FString> Parts;
					for (auto& P : T)
					{
						if (!P.first.is<std::string>()) continue;
						FString K = NeoLuaStr::ToFString(P.first.as<std::string>());
						// Capitalize first letter for UE property name convention (left->Left, size->Size)
						if (K.Len() > 0)
						{
							K[0] = FChar::ToUpper(K[0]);
						}
						// Recurse for nested tables
						TOptional<FString> V = SolValueToImportText(P.second);
						if (V.IsSet())
						{
							Parts.Add(FString::Printf(TEXT("%s=%s"), *K, *V.GetValue()));
						}
					}
					if (Parts.Num() > 0)
						return FString::Printf(TEXT("(%s)"), *FString::Join(Parts, TEXT(",")));
					return TOptional<FString>();
				}
				return TOptional<FString>();
			};

			// Helper: set a UProperty on a UObject, using CallSetter when available
			// Supports partial struct updates: copies current value first, then applies ImportText on top,
			// so {Font='(Size=24)'} only changes Size without wiping FontObject/TypefaceFontName/etc.
			auto SetPropertyValue = [&Session](FProperty* Prop, UObject* Obj, const FString& TextValue, const FString& WidgetName) -> bool
			{
				FString Error;
				if (!NeoLuaProperty::PatchPropertyValueFromString(Prop, Obj, TextValue, true, Error))
				{
					Session.Log(FString::Printf(TEXT("[WARN] configure_widget(\"%s\") -> %s for property '%s'"),
						*WidgetName, *Error, *Prop->GetName()));
					return false;
				}

				FPropertyChangedEvent PCE(Prop);
				Obj->PostEditChangeProperty(PCE);
				return true;
			};

			// is_variable — just flip the flag. We do NOT call OnVariableAdded / OnVariableRemoved:
			//   * Every source widget always needs a WidgetVariableNameToGuidMap entry regardless of
			//     bIsVariable (the compiler at WidgetBlueprintCompiler.cpp:794 ensureAlwaysMsgf's it
			//     and re-adds a fresh GUID when missing, breaking any external refs to the old GUID).
			//   * OnVariableAdded has `ensure(!Contains(name))` (WidgetBlueprint.cpp:1025). Since the
			//     widget's name is already in the map from add_widget's OnVariableAdded call, calling
			//     it again would fire the ensure and re-emplace with a new GUID.
			//   * The engine's own UMG details view (SWidgetDetailsView.cpp:641) just assigns
			//     bIsVariable and calls MarkBlueprintAsStructurallyModified — mirror that.
			//   * The `bShouldGenerateVariable` gate at WidgetBlueprintCompiler.cpp:1455 already
			//     decides whether to emit BP-visible property flags based on bIsVariable.
			auto NormalizeKey = [](FString Value) -> FString
			{
				Value.ReplaceInline(TEXT("_"), TEXT(""));
				Value.ReplaceInline(TEXT("-"), TEXT(""));
				return Value.ToLower();
			};

			auto StripObjectPathClassPrefix = [](FString Input) -> FString
			{
				Input = Input.TrimStartAndEnd();
				Input.TrimQuotesInline();

				int32 OpenQuote = INDEX_NONE;
				int32 CloseQuote = INDEX_NONE;
				if (Input.FindChar(TEXT('\''), OpenQuote) &&
					Input.FindLastChar(TEXT('\''), CloseQuote) &&
					CloseQuote > OpenQuote)
				{
					return Input.Mid(OpenQuote + 1, CloseQuote - OpenQuote - 1).TrimStartAndEnd();
				}

				if (Input.Equals(TEXT("None"), ESearchCase::IgnoreCase) ||
					Input.Equals(TEXT("Null"), ESearchCase::IgnoreCase))
				{
					return FString();
				}

				return Input;
			};

			auto ExtractImportTextField = [](const FString& TextValue, const TCHAR* FieldName, FString& OutValue) -> bool
			{
				const FString Needle = FString(FieldName) + TEXT("=");
				const int32 FieldPos = TextValue.Find(Needle, ESearchCase::IgnoreCase);
				if (FieldPos == INDEX_NONE)
				{
					return false;
				}

				int32 Start = FieldPos + Needle.Len();
				while (Start < TextValue.Len() && FChar::IsWhitespace(TextValue[Start]))
				{
					++Start;
				}

				bool bInSingleQuote = false;
				bool bInDoubleQuote = false;
				int32 ParenDepth = 0;
				int32 End = Start;
				for (; End < TextValue.Len(); ++End)
				{
					const TCHAR Ch = TextValue[End];
					if (Ch == TEXT('\'') && !bInDoubleQuote)
					{
						bInSingleQuote = !bInSingleQuote;
						continue;
					}
					if (Ch == TEXT('"') && !bInSingleQuote)
					{
						bInDoubleQuote = !bInDoubleQuote;
						continue;
					}
					if (bInSingleQuote || bInDoubleQuote)
					{
						continue;
					}
					if (Ch == TEXT('('))
					{
						++ParenDepth;
						continue;
					}
					if (Ch == TEXT(')'))
					{
						if (ParenDepth <= 0)
						{
							break;
						}
						--ParenDepth;
						continue;
					}
					if (Ch == TEXT(',') && ParenDepth == 0)
					{
						break;
					}
				}

				OutValue = TextValue.Mid(Start, End - Start).TrimStartAndEnd();
				OutValue.TrimQuotesInline();
				return !OutValue.IsEmpty();
			};

			auto ReadStringField = [&NormalizeKey](const sol::object& Source, std::initializer_list<const TCHAR*> FieldNames, FString& OutValue) -> bool
			{
				if (!Source.valid() || Source.is<sol::lua_nil_t>() || !Source.is<sol::table>())
				{
					return false;
				}

				sol::table Table = Source.as<sol::table>();
				for (auto& Pair : Table)
				{
					if (!Pair.first.is<std::string>() || !Pair.second.is<std::string>())
					{
						continue;
					}

					const FString Key = NormalizeKey(NeoLuaStr::ToFString(Pair.first.as<std::string>()));
					for (const TCHAR* FieldName : FieldNames)
					{
						if (Key.Equals(NormalizeKey(FString(FieldName)), ESearchCase::IgnoreCase))
						{
							OutValue = NeoLuaStr::ToFString(Pair.second.as<std::string>()).TrimStartAndEnd();
							OutValue.TrimQuotesInline();
							return !OutValue.IsEmpty();
						}
					}
				}

				return false;
			};

			auto ResolveObjectPath = [&StripObjectPathClassPrefix](const FString& RawPath, UClass* ExpectedClass) -> UObject*
			{
				if (!ExpectedClass)
				{
					return nullptr;
				}

				const FString Path = StripObjectPathClassPrefix(RawPath);
				if (Path.IsEmpty())
				{
					return nullptr;
				}

				return StaticLoadObject(ExpectedClass, nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
			};

			auto PatchSlateFontInfoObjectRefs = [&](FProperty* Prop, UObject* Obj, const sol::object& Source, const FString& TextValue, const FString& WidgetName) -> bool
			{
				FStructProperty* StructProp = CastField<FStructProperty>(Prop);
				if (!StructProp || StructProp->Struct != FSlateFontInfo::StaticStruct())
				{
					return true;
				}

				FString FontObjectPath;
				const bool bHasFontObject = ReadStringField(Source, { TEXT("FontObject"), TEXT("FontFamily"), TEXT("Font") }, FontObjectPath) ||
					ExtractImportTextField(TextValue, TEXT("FontObject"), FontObjectPath);

				FString FontMaterialPath;
				const bool bHasFontMaterial = ReadStringField(Source, { TEXT("FontMaterial"), TEXT("Material") }, FontMaterialPath) ||
					ExtractImportTextField(TextValue, TEXT("FontMaterial"), FontMaterialPath);

				if (!bHasFontObject && !bHasFontMaterial)
				{
					return true;
				}

				FSlateFontInfo FontInfo;
				if (UTextBlock* TextBlock = Cast<UTextBlock>(Obj))
				{
					FontInfo = TextBlock->GetFont();
				}
				else
				{
					void* ExistingValue = Prop->ContainerPtrToValuePtr<void>(Obj);
					Prop->CopyCompleteValue(&FontInfo, ExistingValue);
				}

				if (bHasFontObject)
				{
					UObject* FontObject = FontObjectPath.IsEmpty() ? nullptr : ResolveObjectPath(FontObjectPath, UFont::StaticClass());
					if (!FontObject && !StripObjectPathClassPrefix(FontObjectPath).IsEmpty())
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure_widget(\"%s\") -> could not load FontObject '%s' for property '%s'"),
							*WidgetName, *FontObjectPath, *Prop->GetName()));
						return false;
					}
					FontInfo.FontObject = FontObject;
				}

				if (bHasFontMaterial)
				{
					UObject* FontMaterial = FontMaterialPath.IsEmpty() ? nullptr : ResolveObjectPath(FontMaterialPath, UMaterialInterface::StaticClass());
					if (!FontMaterial && !StripObjectPathClassPrefix(FontMaterialPath).IsEmpty())
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure_widget(\"%s\") -> could not load FontMaterial '%s' for property '%s'"),
							*WidgetName, *FontMaterialPath, *Prop->GetName()));
						return false;
					}
					FontInfo.FontMaterial = FontMaterial;
				}

				if (UTextBlock* TextBlock = Cast<UTextBlock>(Obj))
				{
					TextBlock->SetFont(FontInfo);
				}
				else
				{
					void* ExistingValue = Prop->ContainerPtrToValuePtr<void>(Obj);
					Prop->CopyCompleteValue(ExistingValue, &FontInfo);
				}

				FPropertyChangedEvent PCE(Prop);
				Obj->PostEditChangeProperty(PCE);
				return true;
			};

			sol::optional<bool> VarOpt = Opts.get<sol::optional<bool>>("is_variable");
			if (VarOpt.has_value())
			{
				const bool bNewValue = VarOpt.value();
				if (bNewValue != (bool)Widget->bIsVariable)
				{
					Widget->bIsVariable = bNewValue;
					bStructuralChange = true;
					ChangedCount++;
				}
			}

			// visibility
			sol::optional<std::string> VisOpt = Opts.get<sol::optional<std::string>>("visibility");
			if (VisOpt.has_value())
			{
				FString VisStr = NeoLuaStr::ToFStringOpt(VisOpt);
				bool bKnownVisibility = true;
				if (VisStr.Equals(TEXT("Visible"), ESearchCase::IgnoreCase))
					Widget->SetVisibility(ESlateVisibility::Visible);
				else if (VisStr.Equals(TEXT("Collapsed"), ESearchCase::IgnoreCase))
					Widget->SetVisibility(ESlateVisibility::Collapsed);
				else if (VisStr.Equals(TEXT("Hidden"), ESearchCase::IgnoreCase))
					Widget->SetVisibility(ESlateVisibility::Hidden);
				else if (VisStr.Equals(TEXT("HitTestInvisible"), ESearchCase::IgnoreCase))
					Widget->SetVisibility(ESlateVisibility::HitTestInvisible);
				else if (VisStr.Equals(TEXT("SelfHitTestInvisible"), ESearchCase::IgnoreCase))
					Widget->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
				else
				{
					Session.Log(FString::Printf(TEXT("[WARN] configure_widget -> unknown visibility '%s'"), *VisStr));
					WarningCount++;
					bKnownVisibility = false;
				}
				if (bKnownVisibility)
				{
					ChangedCount++;
				}
			}

			// tooltip
			sol::optional<std::string> TooltipOpt = Opts.get<sol::optional<std::string>>("tooltip");
			if (TooltipOpt.has_value())
			{
				FProperty* Prop = NeoLuaProperty::FindPropertyByName(Widget->GetClass(), TEXT("ToolTipText"));
				if (Prop)
				{
					FString Value = NeoLuaStr::ToFStringOpt(TooltipOpt);
					if (SetPropertyValue(Prop, Widget, Value, FWidgetName))
					{
						ChangedCount++;
					}
					else
					{
						WarningCount++;
					}
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[WARN] configure_widget(\"%s\") -> ToolTipText property not found on %s"),
						*FWidgetName, *Widget->GetClass()->GetName()));
					WarningCount++;
				}
			}

			// Generic property setting — collect from both top-level keys and "properties" sub-table
			auto ApplyProperties = [&](const sol::table& Props, UObject* Target, const TCHAR* TargetLabel, const TSet<FString>* KeysToSkip = nullptr)
			{
				for (auto& Pair : Props)
				{
					if (!Pair.first.is<std::string>()) continue;
					FString PropName = NeoLuaStr::ToFString(Pair.first.as<std::string>());
					if (KeysToSkip && KeysToSkip->Contains(PropName))
					{
						continue;
					}

					TOptional<FString> PropValue = SolValueToImportText(Pair.second);
					if (!PropValue.IsSet())
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure_widget(\"%s\") -> cannot convert value for '%s' on %s"),
							*FWidgetName, *PropName, TargetLabel));
						WarningCount++;
						continue;
					}

					FProperty* Prop = NeoLuaProperty::FindPropertyByName(Target->GetClass(), PropName);
					if (Prop && Prop->HasAnyPropertyFlags(CPF_Edit))
					{
						if (SetPropertyValue(Prop, Target, PropValue.GetValue(), FWidgetName) &&
							PatchSlateFontInfoObjectRefs(Prop, Target, Pair.second, PropValue.GetValue(), FWidgetName))
						{
							ChangedCount++;
						}
						else
						{
							WarningCount++;
						}
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure_widget(\"%s\") -> property '%s' not found or not editable on %s"),
							*FWidgetName, *PropName, TargetLabel));
						WarningCount++;
					}
				}
			};

			auto ReadNumber = [](sol::table Table, const char* PrimaryKey, const char* AlternateKey, double& OutValue) -> bool
			{
				sol::object Obj = Table[PrimaryKey];
				if (!Obj.valid() || Obj.is<sol::lua_nil_t>())
				{
					Obj = Table[AlternateKey];
				}
				if (!Obj.valid() || Obj.is<sol::lua_nil_t>())
				{
					return false;
				}
				if (Obj.is<double>())
				{
					OutValue = Obj.as<double>();
					return true;
				}
				if (Obj.is<float>())
				{
					OutValue = Obj.as<float>();
					return true;
				}
				if (Obj.is<int>())
				{
					OutValue = Obj.as<int>();
					return true;
				}
				return false;
			};

			auto ReadNumberObject = [](const sol::object& Obj, double& OutValue) -> bool
			{
				if (!Obj.valid() || Obj.is<sol::lua_nil_t>())
				{
					return false;
				}
				if (Obj.is<double>())
				{
					OutValue = Obj.as<double>();
					return true;
				}
				if (Obj.is<float>())
				{
					OutValue = Obj.as<float>();
					return true;
				}
				if (Obj.is<int>())
				{
					OutValue = Obj.as<int>();
					return true;
				}
				return false;
			};

			auto ReadVector2D = [&](sol::table Table, FVector2D& OutValue) -> bool
			{
				double X = 0.0;
				double Y = 0.0;
				if (!ReadNumber(Table, "x", "X", X) || !ReadNumber(Table, "y", "Y", Y))
				{
					return false;
				}
				OutValue = FVector2D(X, Y);
				return true;
			};

			auto ReadMargin = [&](sol::table Table, FMargin& OutValue) -> bool
			{
				double Left = 0.0;
				double Top = 0.0;
				double Right = 0.0;
				double Bottom = 0.0;
				bool bAny = false;

				auto ReadSide = [&](const char* LowerKey, const char* UpperKey, double& OutSide)
				{
					if (ReadNumber(Table, LowerKey, UpperKey, OutSide))
					{
						bAny = true;
						return true;
					}
					return false;
				};

				ReadSide("left", "Left", Left);
				ReadSide("top", "Top", Top);
				ReadSide("right", "Right", Right);
				ReadSide("bottom", "Bottom", Bottom);

				double Horizontal = 0.0;
				if (ReadNumber(Table, "horizontal", "Horizontal", Horizontal))
				{
					Left = Horizontal;
					Right = Horizontal;
					bAny = true;
				}

				double Vertical = 0.0;
				if (ReadNumber(Table, "vertical", "Vertical", Vertical))
				{
					Top = Vertical;
					Bottom = Vertical;
					bAny = true;
				}

				if (!bAny)
				{
					double A = 0.0;
					double B = 0.0;
					double C = 0.0;
					double D = 0.0;
					sol::object V1 = Table[1];
					sol::object V2 = Table[2];
					sol::object V3 = Table[3];
					sol::object V4 = Table[4];
					if (ReadNumberObject(V1, A))
					{
						if (ReadNumberObject(V2, B))
						{
							if (ReadNumberObject(V3, C) && ReadNumberObject(V4, D))
							{
								Left = A;
								Top = B;
								Right = C;
								Bottom = D;
							}
							else
							{
								Left = A;
								Right = A;
								Top = B;
								Bottom = B;
							}
						}
						else
						{
							Left = A;
							Top = A;
							Right = A;
							Bottom = A;
						}
						bAny = true;
					}
				}

				if (!bAny)
				{
					return false;
				}
				OutValue = FMargin(Left, Top, Right, Bottom);
				return true;
			};

			auto ReadMarginImportText = [](const FString& TextValue, FMargin& OutValue) -> bool
			{
				FString Trimmed = TextValue.TrimStartAndEnd();
				if (Trimmed.IsNumeric())
				{
					OutValue = FMargin(FCString::Atod(*Trimmed));
					return true;
				}

				float Left = 0.0f;
				float Top = 0.0f;
				float Right = 0.0f;
				float Bottom = 0.0f;
				bool bAny = false;
				if (FParse::Value(*Trimmed, TEXT("Left="), Left)) bAny = true;
				if (FParse::Value(*Trimmed, TEXT("Top="), Top)) bAny = true;
				if (FParse::Value(*Trimmed, TEXT("Right="), Right)) bAny = true;
				if (FParse::Value(*Trimmed, TEXT("Bottom="), Bottom)) bAny = true;
				if (!bAny)
				{
					return false;
				}
				OutValue = FMargin(Left, Top, Right, Bottom);
				return true;
			};

			auto ReadAnchors = [&](sol::table Table, FAnchors& OutValue) -> bool
			{
				FVector2D Minimum;
				FVector2D Maximum;
				sol::object MinObj = Table["minimum"];
				if (!MinObj.valid() || MinObj.is<sol::lua_nil_t>())
				{
					MinObj = Table["Minimum"];
				}
				sol::object MaxObj = Table["maximum"];
				if (!MaxObj.valid() || MaxObj.is<sol::lua_nil_t>())
				{
					MaxObj = Table["Maximum"];
				}

				if (MinObj.is<sol::table>() && MaxObj.is<sol::table>())
				{
					if (!ReadVector2D(MinObj.as<sol::table>(), Minimum) || !ReadVector2D(MaxObj.as<sol::table>(), Maximum))
					{
						return false;
					}
				}
				else
				{
					double MinX = 0.0;
					double MinY = 0.0;
					double MaxX = 0.0;
					double MaxY = 0.0;
					if (!ReadNumber(Table, "min_x", "MinX", MinX) || !ReadNumber(Table, "min_y", "MinY", MinY) ||
						!ReadNumber(Table, "max_x", "MaxX", MaxX) || !ReadNumber(Table, "max_y", "MaxY", MaxY))
					{
						return false;
					}
					Minimum = FVector2D(MinX, MinY);
					Maximum = FVector2D(MaxX, MaxY);
				}

				OutValue = FAnchors(Minimum.X, Minimum.Y, Maximum.X, Maximum.Y);
				return true;
			};

			auto ReadInt = [](const sol::object& Obj, int32& OutValue) -> bool
			{
				if (Obj.is<int>())
				{
					OutValue = Obj.as<int>();
					return true;
				}
				if (Obj.is<double>())
				{
					OutValue = static_cast<int32>(Obj.as<double>());
					return true;
				}
				return false;
			};

			auto ReadHorizontalAlignment = [](const sol::object& Obj, EHorizontalAlignment& OutValue) -> bool
			{
				if (!Obj.is<std::string>())
				{
					return false;
				}
				FString Value = NeoLuaStr::ToFString(Obj.as<std::string>());
				if (Value.Equals(TEXT("Left"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("HAlign_Left"), ESearchCase::IgnoreCase))
				{
					OutValue = HAlign_Left;
					return true;
				}
				if (Value.Equals(TEXT("Center"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("Centre"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("HAlign_Center"), ESearchCase::IgnoreCase))
				{
					OutValue = HAlign_Center;
					return true;
				}
				if (Value.Equals(TEXT("Right"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("HAlign_Right"), ESearchCase::IgnoreCase))
				{
					OutValue = HAlign_Right;
					return true;
				}
				if (Value.Equals(TEXT("Fill"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("HAlign_Fill"), ESearchCase::IgnoreCase))
				{
					OutValue = HAlign_Fill;
					return true;
				}
				return false;
			};

			auto ReadVerticalAlignment = [](const sol::object& Obj, EVerticalAlignment& OutValue) -> bool
			{
				if (!Obj.is<std::string>())
				{
					return false;
				}
				FString Value = NeoLuaStr::ToFString(Obj.as<std::string>());
				if (Value.Equals(TEXT("Top"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("VAlign_Top"), ESearchCase::IgnoreCase))
				{
					OutValue = VAlign_Top;
					return true;
				}
				if (Value.Equals(TEXT("Center"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("Centre"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("VAlign_Center"), ESearchCase::IgnoreCase))
				{
					OutValue = VAlign_Center;
					return true;
				}
				if (Value.Equals(TEXT("Bottom"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("VAlign_Bottom"), ESearchCase::IgnoreCase))
				{
					OutValue = VAlign_Bottom;
					return true;
				}
				if (Value.Equals(TEXT("Fill"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("VAlign_Fill"), ESearchCase::IgnoreCase))
				{
					OutValue = VAlign_Fill;
					return true;
				}
				return false;
			};

			auto ApplyCommonSlotAliases = [&](const sol::table& Props, UPanelSlot* Slot, TSet<FString>& OutHandledKeys)
			{
				if (!Slot)
				{
					return;
				}

				auto MarkHandled = [&](const FString& Key)
				{
					OutHandledKeys.Add(Key);
					ChangedCount++;
				};
				auto WarnInvalid = [&](const FString& Key)
				{
					OutHandledKeys.Add(Key);
					WarningCount++;
					Session.Log(FString::Printf(TEXT("[WARN] configure_widget(\"%s\") -> invalid %s alias '%s'"),
						*FWidgetName, *Slot->GetClass()->GetName(), *Key));
				};
				auto TryPadding = [&](const FString& Key, const sol::object& Value, auto Setter) -> bool
				{
					if (!Key.Equals(TEXT("Padding"), ESearchCase::IgnoreCase))
					{
						return false;
					}
					FMargin Padding;
					if (Value.is<sol::table>() && ReadMargin(Value.as<sol::table>(), Padding))
					{
						Setter(Padding);
						MarkHandled(Key);
						return true;
					}
					if (Value.is<std::string>() && ReadMarginImportText(NeoLuaStr::ToFString(Value.as<std::string>()), Padding))
					{
						Setter(Padding);
						MarkHandled(Key);
						return true;
					}
					double Uniform = 0.0;
					if (ReadNumberObject(Value, Uniform))
					{
						Setter(FMargin(Uniform));
						MarkHandled(Key);
						return true;
					}

					// Let the generic UPROPERTY path try real Padding import text before warning.
					return false;
				};
				auto TryHorizontalAlignment = [&](const FString& Key, const sol::object& Value, auto Setter) -> bool
				{
					if (!Key.Equals(TEXT("HorizontalAlignment"), ESearchCase::IgnoreCase) && !Key.Equals(TEXT("HAlign"), ESearchCase::IgnoreCase))
					{
						return false;
					}
					EHorizontalAlignment Alignment = HAlign_Fill;
					if (ReadHorizontalAlignment(Value, Alignment))
					{
						Setter(Alignment);
						MarkHandled(Key);
					}
					else
					{
						WarnInvalid(Key);
					}
					return true;
				};
				auto TryVerticalAlignment = [&](const FString& Key, const sol::object& Value, auto Setter) -> bool
				{
					if (!Key.Equals(TEXT("VerticalAlignment"), ESearchCase::IgnoreCase) && !Key.Equals(TEXT("VAlign"), ESearchCase::IgnoreCase))
					{
						return false;
					}
					EVerticalAlignment Alignment = VAlign_Fill;
					if (ReadVerticalAlignment(Value, Alignment))
					{
						Setter(Alignment);
						MarkHandled(Key);
					}
					else
					{
						WarnInvalid(Key);
					}
					return true;
				};

				for (auto& Pair : Props)
				{
					if (!Pair.first.is<std::string>()) continue;
					const FString Key = NeoLuaStr::ToFString(Pair.first.as<std::string>());
					sol::object Value = Pair.second;

					if (UVerticalBoxSlot* VerticalSlot = Cast<UVerticalBoxSlot>(Slot))
					{
						if (TryPadding(Key, Value, [VerticalSlot](FMargin V) { VerticalSlot->SetPadding(V); }) ||
							TryHorizontalAlignment(Key, Value, [VerticalSlot](EHorizontalAlignment V) { VerticalSlot->SetHorizontalAlignment(V); }) ||
							TryVerticalAlignment(Key, Value, [VerticalSlot](EVerticalAlignment V) { VerticalSlot->SetVerticalAlignment(V); }))
						{
							continue;
						}
					}
					else if (UHorizontalBoxSlot* HorizontalSlot = Cast<UHorizontalBoxSlot>(Slot))
					{
						if (TryPadding(Key, Value, [HorizontalSlot](FMargin V) { HorizontalSlot->SetPadding(V); }) ||
							TryHorizontalAlignment(Key, Value, [HorizontalSlot](EHorizontalAlignment V) { HorizontalSlot->SetHorizontalAlignment(V); }) ||
							TryVerticalAlignment(Key, Value, [HorizontalSlot](EVerticalAlignment V) { HorizontalSlot->SetVerticalAlignment(V); }))
						{
							continue;
						}
					}
					else if (UOverlaySlot* OverlaySlot = Cast<UOverlaySlot>(Slot))
					{
						if (TryPadding(Key, Value, [OverlaySlot](FMargin V) { OverlaySlot->SetPadding(V); }) ||
							TryHorizontalAlignment(Key, Value, [OverlaySlot](EHorizontalAlignment V) { OverlaySlot->SetHorizontalAlignment(V); }) ||
							TryVerticalAlignment(Key, Value, [OverlaySlot](EVerticalAlignment V) { OverlaySlot->SetVerticalAlignment(V); }))
						{
							continue;
						}
					}
					else if (USizeBoxSlot* SizeBoxSlot = Cast<USizeBoxSlot>(Slot))
					{
						if (TryPadding(Key, Value, [SizeBoxSlot](FMargin V) { SizeBoxSlot->SetPadding(V); }) ||
							TryHorizontalAlignment(Key, Value, [SizeBoxSlot](EHorizontalAlignment V) { SizeBoxSlot->SetHorizontalAlignment(V); }) ||
							TryVerticalAlignment(Key, Value, [SizeBoxSlot](EVerticalAlignment V) { SizeBoxSlot->SetVerticalAlignment(V); }))
						{
							continue;
						}
					}
					else if (UBorderSlot* BorderSlot = Cast<UBorderSlot>(Slot))
					{
						if (TryPadding(Key, Value, [BorderSlot](FMargin V) { BorderSlot->SetPadding(V); }) ||
							TryHorizontalAlignment(Key, Value, [BorderSlot](EHorizontalAlignment V) { BorderSlot->SetHorizontalAlignment(V); }) ||
							TryVerticalAlignment(Key, Value, [BorderSlot](EVerticalAlignment V) { BorderSlot->SetVerticalAlignment(V); }))
						{
							continue;
						}
					}
					else if (UButtonSlot* ButtonSlot = Cast<UButtonSlot>(Slot))
					{
						if (TryPadding(Key, Value, [ButtonSlot](FMargin V) { ButtonSlot->SetPadding(V); }) ||
							TryHorizontalAlignment(Key, Value, [ButtonSlot](EHorizontalAlignment V) { ButtonSlot->SetHorizontalAlignment(V); }) ||
							TryVerticalAlignment(Key, Value, [ButtonSlot](EVerticalAlignment V) { ButtonSlot->SetVerticalAlignment(V); }))
						{
							continue;
						}
					}
					else if (UScrollBoxSlot* ScrollBoxSlot = Cast<UScrollBoxSlot>(Slot))
					{
						if (TryPadding(Key, Value, [ScrollBoxSlot](FMargin V) { ScrollBoxSlot->SetPadding(V); }) ||
							TryHorizontalAlignment(Key, Value, [ScrollBoxSlot](EHorizontalAlignment V) { ScrollBoxSlot->SetHorizontalAlignment(V); }) ||
							TryVerticalAlignment(Key, Value, [ScrollBoxSlot](EVerticalAlignment V) { ScrollBoxSlot->SetVerticalAlignment(V); }))
						{
							continue;
						}
					}
					else if (UGridSlot* GridSlot = Cast<UGridSlot>(Slot))
					{
						if (TryPadding(Key, Value, [GridSlot](FMargin V) { GridSlot->SetPadding(V); }) ||
							TryHorizontalAlignment(Key, Value, [GridSlot](EHorizontalAlignment V) { GridSlot->SetHorizontalAlignment(V); }) ||
							TryVerticalAlignment(Key, Value, [GridSlot](EVerticalAlignment V) { GridSlot->SetVerticalAlignment(V); }))
						{
							continue;
						}

						int32 IntValue = 0;
						if (Key.Equals(TEXT("Row"), ESearchCase::IgnoreCase))
						{
							if (ReadInt(Value, IntValue)) { GridSlot->SetRow(IntValue); MarkHandled(Key); } else { WarnInvalid(Key); }
						}
						else if (Key.Equals(TEXT("Column"), ESearchCase::IgnoreCase))
						{
							if (ReadInt(Value, IntValue)) { GridSlot->SetColumn(IntValue); MarkHandled(Key); } else { WarnInvalid(Key); }
						}
						else if (Key.Equals(TEXT("RowSpan"), ESearchCase::IgnoreCase))
						{
							if (ReadInt(Value, IntValue)) { GridSlot->SetRowSpan(IntValue); MarkHandled(Key); } else { WarnInvalid(Key); }
						}
						else if (Key.Equals(TEXT("ColumnSpan"), ESearchCase::IgnoreCase))
						{
							if (ReadInt(Value, IntValue)) { GridSlot->SetColumnSpan(IntValue); MarkHandled(Key); } else { WarnInvalid(Key); }
						}
						else if (Key.Equals(TEXT("Layer"), ESearchCase::IgnoreCase))
						{
							if (ReadInt(Value, IntValue)) { GridSlot->SetLayer(IntValue); MarkHandled(Key); } else { WarnInvalid(Key); }
						}
						else if (Key.Equals(TEXT("Nudge"), ESearchCase::IgnoreCase) && Value.is<sol::table>())
						{
							FVector2D Nudge;
							if (ReadVector2D(Value.as<sol::table>(), Nudge)) { GridSlot->SetNudge(Nudge); MarkHandled(Key); } else { WarnInvalid(Key); }
						}
					}
					else if (UUniformGridSlot* UniformGridSlot = Cast<UUniformGridSlot>(Slot))
					{
						if (TryHorizontalAlignment(Key, Value, [UniformGridSlot](EHorizontalAlignment V) { UniformGridSlot->SetHorizontalAlignment(V); }) ||
							TryVerticalAlignment(Key, Value, [UniformGridSlot](EVerticalAlignment V) { UniformGridSlot->SetVerticalAlignment(V); }))
						{
							continue;
						}

						int32 IntValue = 0;
						if (Key.Equals(TEXT("Row"), ESearchCase::IgnoreCase))
						{
							if (ReadInt(Value, IntValue)) { UniformGridSlot->SetRow(IntValue); MarkHandled(Key); } else { WarnInvalid(Key); }
						}
						else if (Key.Equals(TEXT("Column"), ESearchCase::IgnoreCase))
						{
							if (ReadInt(Value, IntValue)) { UniformGridSlot->SetColumn(IntValue); MarkHandled(Key); } else { WarnInvalid(Key); }
						}
					}
				}
			};

			auto ApplyCanvasPanelSlotAliases = [&](const sol::table& Props, UCanvasPanelSlot* CanvasSlot, TSet<FString>& OutHandledKeys)
			{
				if (!CanvasSlot)
				{
					return;
				}

				for (auto& Pair : Props)
				{
					if (!Pair.first.is<std::string>()) continue;
					const FString Key = NeoLuaStr::ToFString(Pair.first.as<std::string>());
					sol::object Value = Pair.second;

					auto MarkHandled = [&]()
					{
						OutHandledKeys.Add(Key);
						ChangedCount++;
					};
					auto WarnInvalid = [&]()
					{
						OutHandledKeys.Add(Key);
						WarningCount++;
						Session.Log(FString::Printf(TEXT("[WARN] configure_widget(\"%s\") -> invalid CanvasPanelSlot alias '%s'"),
							*FWidgetName, *Key));
					};

					if (Key.Equals(TEXT("Position"), ESearchCase::IgnoreCase) && Value.is<sol::table>())
					{
						FVector2D Position;
						if (ReadVector2D(Value.as<sol::table>(), Position))
						{
							CanvasSlot->SetPosition(Position);
							MarkHandled();
						}
						else
						{
							WarnInvalid();
						}
					}
					else if (Key.Equals(TEXT("Size"), ESearchCase::IgnoreCase) && Value.is<sol::table>())
					{
						FVector2D Size;
						if (ReadVector2D(Value.as<sol::table>(), Size))
						{
							CanvasSlot->SetSize(Size);
							MarkHandled();
						}
						else
						{
							WarnInvalid();
						}
					}
					else if (Key.Equals(TEXT("Offsets"), ESearchCase::IgnoreCase) && Value.is<sol::table>())
					{
						FMargin Offsets;
						if (ReadMargin(Value.as<sol::table>(), Offsets))
						{
							CanvasSlot->SetOffsets(Offsets);
							MarkHandled();
						}
						else
						{
							WarnInvalid();
						}
					}
					else if (Key.Equals(TEXT("Anchors"), ESearchCase::IgnoreCase) && Value.is<sol::table>())
					{
						FAnchors Anchors;
						if (ReadAnchors(Value.as<sol::table>(), Anchors))
						{
							CanvasSlot->SetAnchors(Anchors);
							MarkHandled();
						}
						else
						{
							WarnInvalid();
						}
					}
					else if (Key.Equals(TEXT("Alignment"), ESearchCase::IgnoreCase) && Value.is<sol::table>())
					{
						FVector2D Alignment;
						if (ReadVector2D(Value.as<sol::table>(), Alignment))
						{
							CanvasSlot->SetAlignment(Alignment);
							MarkHandled();
						}
						else
						{
							WarnInvalid();
						}
					}
					else if (Key.Equals(TEXT("AutoSize"), ESearchCase::IgnoreCase) || Key.Equals(TEXT("bAutoSize"), ESearchCase::IgnoreCase))
					{
						if (Value.is<bool>())
						{
							CanvasSlot->SetAutoSize(Value.as<bool>());
							MarkHandled();
						}
						else
						{
							WarnInvalid();
						}
					}
					else if (Key.Equals(TEXT("ZOrder"), ESearchCase::IgnoreCase))
					{
						double ZOrder = 0.0;
						if (Value.is<int>())
						{
							CanvasSlot->SetZOrder(Value.as<int>());
							MarkHandled();
						}
						else if (Value.is<double>())
						{
							CanvasSlot->SetZOrder(static_cast<int32>(Value.as<double>()));
							MarkHandled();
						}
						else if (Value.is<sol::table>() && ReadNumber(Value.as<sol::table>(), "value", "Value", ZOrder))
						{
							CanvasSlot->SetZOrder(static_cast<int32>(ZOrder));
							MarkHandled();
						}
						else
						{
							WarnInvalid();
						}
					}
				}
			};

			// Explicit "properties" sub-table
			sol::optional<sol::table> PropsOpt = Opts.get<sol::optional<sol::table>>("properties");
			if (PropsOpt.has_value())
			{
				ApplyProperties(PropsOpt.value(), Widget, TEXT("widget"));
			}

			// Top-level keys that aren't special — treat as property names on the widget
			for (auto& Pair : Opts)
			{
				if (!Pair.first.is<std::string>()) continue;
				FString Key = NeoLuaStr::ToFString(Pair.first.as<std::string>());
				if (SpecialKeys.Contains(Key)) continue;

				TOptional<FString> PropValue = SolValueToImportText(Pair.second);
				if (!PropValue.IsSet())
				{
					Session.Log(FString::Printf(TEXT("[WARN] configure_widget(\"%s\") -> cannot convert value for '%s' on widget"),
						*FWidgetName, *Key));
					WarningCount++;
					continue;
				}

				FProperty* Prop = NeoLuaProperty::FindPropertyByName(Widget->GetClass(), Key);
				if (Prop && Prop->HasAnyPropertyFlags(CPF_Edit))
				{
					if (SetPropertyValue(Prop, Widget, PropValue.GetValue(), FWidgetName) &&
						PatchSlateFontInfoObjectRefs(Prop, Widget, Pair.second, PropValue.GetValue(), FWidgetName))
					{
						ChangedCount++;
					}
					else
					{
						WarningCount++;
					}
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[WARN] configure_widget(\"%s\") -> property '%s' not found or not editable on widget"),
						*FWidgetName, *Key));
					WarningCount++;
				}
			}

			// Slot properties via "slot" sub-table
			sol::optional<sol::table> SlotOpt = Opts.get<sol::optional<sol::table>>("slot");
			if (SlotOpt.has_value() && Widget->Slot)
			{
				Widget->Slot->Modify();
				TSet<FString> HandledSlotKeys;
				ApplyCanvasPanelSlotAliases(SlotOpt.value(), Cast<UCanvasPanelSlot>(Widget->Slot), HandledSlotKeys);
				ApplyCommonSlotAliases(SlotOpt.value(), Widget->Slot, HandledSlotKeys);
				ApplyProperties(SlotOpt.value(), Widget->Slot, TEXT("slot"), &HandledSlotKeys);
				Widget->Slot->SynchronizeProperties();
			}
			else if (SlotOpt.has_value())
			{
				Session.Log(FString::Printf(TEXT("[WARN] configure_widget(\"%s\") -> widget has no slot to configure"),
					*FWidgetName));
				WarningCount++;
			}

			if (ChangedCount > 0)
			{
				// bIsVariable flips change generated-class variable flags → skeleton needs a
				// recompile, which MarkBlueprintAsStructurallyModified triggers. Plain property
				// edits already fired PostEditChangeProperty; the lighter MarkBlueprintAsModified
				// is sufficient for those.
				if (bStructuralChange)
				{
					FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
				}
				else
				{
					FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
				}
			}

			if (ChangedCount <= 0)
			{
				Tx.Cancel();
				Session.Log(FString::Printf(TEXT("[FAIL] configure_widget(\"%s\") -> no changes applied%s"),
					*FWidgetName, WarningCount > 0 ? TEXT(" (see warnings above)") : TEXT("")));
				return sol::lua_nil;
			}

			Session.Log(FString::Printf(TEXT("[OK] configure_widget(\"%s\") -> %d changes, %d warning(s)"), *FWidgetName, ChangedCount, WarningCount));
			sol::table Result = Lua.create_table();
			Result["ok"] = true;
			Result["changes"] = ChangedCount;
			Result["warnings"] = WarningCount;
			Result["warning_count"] = WarningCount;
			return Result;
		});

		// ================================================================
		// rename_widget(old_name, new_name) -> rename widget + update all references
		// ================================================================
		BPObj.set_function("rename_widget", [FPath, &Session](sol::table /*self*/,
			const std::string& OldName, const std::string& NewName, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FOldName = NeoLuaStr::ToFString(OldName);
			FString FNewDisplayName = NeoLuaStr::ToFString(NewName);

			UWidgetBlueprint* WBP = NeoLuaAsset::Resolve<UWidgetBlueprint>(FPath);
			if (!WBP || !WBP->WidgetTree)
			{
				Session.Log(TEXT("[FAIL] rename_widget -> no widget tree. The blueprint has no UWidgetTree — open_asset(path) with a valid WidgetBlueprint, or call add_widget(type) to seed a root widget."));
				return sol::lua_nil;
			}

			FName OldObjectName(*FOldName);
			UWidget* Widget = WBP->WidgetTree->FindWidget(OldObjectName);
			if (!Widget)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] rename_widget(\"%s\") -> not found"), *FOldName));
				return sol::lua_nil;
			}

			UClass* ParentClass = WBP->ParentClass;
			if (!ParentClass)
			{
				Session.Log(TEXT("[FAIL] rename_widget -> no parent class"));
				return sol::lua_nil;
			}

			// Sanitize the new name (mirrors engine SanitizeWidgetName)
			FString GeneratedName = SlugStringForValidName(FNewDisplayName);
			if (GeneratedName.IsEmpty())
			{
				GeneratedName = FOldName;
			}
			FName NewFName(*GeneratedName);

			// Two-layer validation, matching what the Designer does. We CANNOT rely on
			// FKismetNameValidator alone — it doesn't enumerate the WidgetTree, and when
			// bIsVariable=false the widget isn't in NewVariables either, so it returns Ok
			// for a name that already belongs to another widget. Hitting RenameWidget in
			// that state trips OnVariableRenamed's ensure (WidgetBlueprint.cpp:1038),
			// silently overwrites the GuidMap entry, then UE_LOG(Fatal)s in UObject::Rename
			// (Obj.cpp:349). See WidgetHelper::IsNameSafeForWidget for the full rationale.
			//
			// Order: BindWidget property check → WidgetTree/GuidMap/REN_Test → kismet validator.
			//   • BindWidget: reusing an inherited property name is *allowed* and bypasses
			//     the uniqueness check, just like the engine's RenameWidget does at line 326.
			//   • WidgetTree pre-flight: the check FKismetNameValidator misses.
			//   • Kismet validator: still useful for catching collisions with BP-level
			//     names (functions, graphs, member variables that aren't widgets).
			FObjectPropertyBase* ExistingProperty = CastField<FObjectPropertyBase>(ParentClass->FindPropertyByName(NewFName));
			const bool bBindWidget = ExistingProperty
				&& FWidgetBlueprintEditorUtils::IsBindWidgetProperty(ExistingProperty)
				&& Widget->IsA(ExistingProperty->PropertyClass);

			if (!bBindWidget)
			{
				FString PreflightReason;
				if (!WidgetHelper::IsNameSafeForWidget(WBP, NewFName, Widget, PreflightReason))
				{
					Session.Log(FString::Printf(
						TEXT("[FAIL] rename_widget(\"%s\", \"%s\") -> %s"),
						*FOldName, *FNewDisplayName, *PreflightReason));
					return sol::lua_nil;
				}
			}

			TSharedPtr<INameValidatorInterface> NameValidator = MakeShareable(new FKismetNameValidator(WBP, OldObjectName));
			const EValidatorResult ValidatorResult = NameValidator->IsValid(NewFName);
			const bool bUniqueNameForTemplate = (EValidatorResult::Ok == ValidatorResult || bBindWidget);
			if (!bUniqueNameForTemplate)
			{
				// Surface the validator reason (ExistingName / AlreadyInUse / ContainsInvalidCharacters / LocallyInUse)
				// so the agent doesn't have to guess whether to retry with a different name or call list_widgets().
				const TCHAR* Reason =
					ValidatorResult == EValidatorResult::AlreadyInUse           ? TEXT("name already in use") :
					ValidatorResult == EValidatorResult::ExistingName           ? TEXT("same name as the current widget") :
					ValidatorResult == EValidatorResult::LocallyInUse           ? TEXT("name locally in use") :
					ValidatorResult == EValidatorResult::ContainsInvalidCharacters ? TEXT("contains invalid characters") :
					ValidatorResult == EValidatorResult::TooLong                ? TEXT("name too long") :
					ValidatorResult == EValidatorResult::EmptyName              ? TEXT("empty name") :
					TEXT("invalid");
				Session.Log(FString::Printf(
					TEXT("[FAIL] rename_widget(\"%s\", \"%s\") -> sanitized name '%s' is %s. Retry with a unique alphanumeric name; call list_widgets() to inspect existing names."),
					*FOldName, *FNewDisplayName, *NewFName.ToString(), Reason));
				return sol::lua_nil;
			}

			const FString NewNameStr = NewFName.ToString();
			const FString OldNameStr = OldObjectName.ToString();

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaRenameWidget", "Rename Widget"));
			WBP->Modify();
			Widget->Modify();

			// Notify blueprint about variable rename (updates WidgetRenameMap etc.)
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			WBP->OnVariableRenamed(OldObjectName, NewFName);
#endif

			// Rename the template widget
			Widget->SetDisplayLabel(FNewDisplayName);
			Widget->Rename(*NewNameStr);

			// Update desired focus widget reference
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			FWidgetBlueprintEditorUtils::ReplaceDesiredFocus(WBP, OldObjectName, NewFName);
#endif

			// Update delegate/property binding references
			for (FDelegateEditorBinding& Binding : WBP->Bindings)
			{
				if (Binding.ObjectName == OldNameStr)
				{
					Binding.ObjectName = NewNameStr;
				}
			}

			// Update animation binding references
			for (UWidgetAnimation* WidgetAnimation : WBP->Animations)
			{
				if (!WidgetAnimation) continue;
				for (FWidgetAnimationBinding& AnimBinding : WidgetAnimation->AnimationBindings)
				{
					if (AnimBinding.WidgetName == OldObjectName)
					{
						AnimBinding.WidgetName = NewFName;

						if (WidgetAnimation->MovieScene)
						{
							WidgetAnimation->MovieScene->Modify();

							if (AnimBinding.SlotWidgetName == NAME_None)
							{
								FMovieScenePossessable* Possessable = WidgetAnimation->MovieScene->FindPossessable(AnimBinding.AnimationGuid);
								if (Possessable)
								{
									Possessable->SetName(NewFName.ToString());
								}
							}
							else
							{
								break;
							}
						}
					}
				}
			}

			// Update navigation widget references
			WBP->WidgetTree->ForEachWidget([OldObjectName, NewFName](UWidget* W)
			{
				if (W && W->Navigation)
				{
					W->Navigation->SetFlags(RF_Transactional);
					W->Navigation->Modify();
					W->Navigation->TryToRenameBinding(OldObjectName, NewFName);
				}
			});

			// Validate child blueprints and adjust variable names
			FBlueprintEditorUtils::ValidateBlueprintChildVariables(WBP, NewFName);

			// Refresh references and flush editors
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

			// Update variable references and event references
			FBlueprintEditorUtils::ReplaceVariableReferences(WBP, OldObjectName, NewFName);

			const bool bSanitized = !FNewDisplayName.Equals(NewNameStr, ESearchCase::CaseSensitive);
			Session.Log(FString::Printf(TEXT("[OK] rename_widget(\"%s\" -> \"%s\") requested=\"%s\" sanitized=%s"),
				*FOldName, *NewNameStr, *FNewDisplayName, bSanitized ? TEXT("true") : TEXT("false")));

			sol::table Result = WidgetHelper::WidgetToTable(Lua, Widget, WBP);
			Result["requested_name"] = TCHAR_TO_UTF8(*FNewDisplayName);
			Result["final_name"] = TCHAR_TO_UTF8(*NewNameStr);
			Result["sanitized_name"] = bSanitized;
			return Result;
		});

		// ================================================================
		// list_inherited_named_slots() -> named slots exposed by parent widget classes
		// ================================================================
		BPObj.set_function("list_inherited_named_slots", [FPath, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			UWidgetBlueprint* WBP = NeoLuaAsset::Resolve<UWidgetBlueprint>(FPath);
			if (!WBP)
			{
				Session.Log(TEXT("[FAIL] list_inherited_named_slots -> blueprint not found. Call open_asset(path) to reload, or find_assets(\"WidgetBlueprint\") to discover paths."));
				return sol::lua_nil;
			}

			const TArray<FName> Available = WBP->GetInheritedAvailableNamedSlots();
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			const TSet<FName> Filled = WBP->GetInheritedNamedSlotsWithContentInSameTree();
#else
			// Pre-5.7: helper not exported. Treat all inherited slots as unfilled — the
			// callers use this as a UI hint only.
			const TSet<FName> Filled;
#endif

			sol::table Result = Lua.create_table();
			int32 Idx = 1;
			for (const FName& Name : Available)
			{
				sol::table Slot = Lua.create_table();
				Slot["name"] = TCHAR_TO_UTF8(*Name.ToString());
				Slot["has_inherited_content"] = Filled.Contains(Name);
				Result[Idx++] = Slot;
			}

			Session.Log(FString::Printf(TEXT("[OK] list_inherited_named_slots() -> %d inherited slots (%d filled)"),
				Available.Num(), Filled.Num()));
			return Result;
		});

		// ================================================================
		// validate_widget_tree() -> circular-reference check
		// ================================================================
		BPObj.set_function("validate_widget_tree", [FPath, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			UWidgetBlueprint* WBP = NeoLuaAsset::Resolve<UWidgetBlueprint>(FPath);
			if (!WBP)
			{
				Session.Log(TEXT("[FAIL] validate_widget_tree -> blueprint not found. Call open_asset(path) first."));
				return sol::lua_nil;
			}

			// WidgetBlueprint::HasCircularReferences (WidgetBlueprint.cpp:1419) silently returns
			// "no cycle" when GeneratedClass is null (uncompiled) — that would make this function
			// falsely report "ok=true" on a freshly created WBP. Surface the state so the agent
			// can compile first.
			if (!WBP->GeneratedClass)
			{
				Session.Log(TEXT("[FAIL] validate_widget_tree -> blueprint has no GeneratedClass (not compiled). Compile the Widget Blueprint first, then retry."));
				return sol::lua_nil;
			}

			TValueOrError<void, UWidget*> CycleResult = WBP->HasCircularReferences();

			sol::table Result = Lua.create_table();
			if (CycleResult.HasError())
			{
				UWidget* Offender = CycleResult.GetError();
				Result["ok"] = false;
				if (Offender)
				{
					Result["offending_widget"] = TCHAR_TO_UTF8(*Offender->GetName());
					Result["offending_type"] = TCHAR_TO_UTF8(*Offender->GetClass()->GetName());
				}
				Session.Log(FString::Printf(TEXT("[OK] validate_widget_tree() -> CYCLE via '%s'"),
					Offender ? *Offender->GetName() : TEXT("<unknown>")));
			}
			else
			{
				Result["ok"] = true;
				Session.Log(TEXT("[OK] validate_widget_tree() -> no circular references"));
			}
			return Result;
		});

		// ================================================================
		// get_desired_focus() -> current desired-focus widget name
		// ================================================================
		BPObj.set_function("get_desired_focus", [FPath, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			UWidgetBlueprint* WBP = NeoLuaAsset::Resolve<UWidgetBlueprint>(FPath);
			if (!WBP || !WBP->GeneratedClass)
			{
				Session.Log(TEXT("[FAIL] get_desired_focus -> blueprint not compiled yet (no GeneratedClass)"));
				return sol::lua_nil;
			}

			UUserWidget* WidgetCDO = WBP->GeneratedClass->GetDefaultObject<UUserWidget>();
			if (!WidgetCDO)
			{
				Session.Log(TEXT("[FAIL] get_desired_focus -> no UserWidget CDO"));
				return sol::lua_nil;
			}

			const FName FocusName = WidgetCDO->GetDesiredFocusWidgetName();
			if (FocusName.IsNone())
			{
				Session.Log(TEXT("[OK] get_desired_focus() -> (none)"));
				return sol::lua_nil;
			}

			Session.Log(FString::Printf(TEXT("[OK] get_desired_focus() -> '%s'"), *FocusName.ToString()));
			return sol::make_object(Lua, std::string(TCHAR_TO_UTF8(*FocusName.ToString())));
		});

		// ================================================================
		// set_desired_focus(widget_name) -> configure startup keyboard focus
		// ================================================================
		BPObj.set_function("set_desired_focus", [FPath, &Session](sol::table /*self*/,
			sol::object WidgetObj, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			UWidgetBlueprint* WBP = NeoLuaAsset::Resolve<UWidgetBlueprint>(FPath);
			if (!WBP)
			{
				Session.Log(TEXT("[FAIL] set_desired_focus -> blueprint not found. Call open_asset(path) to reload, or find_assets(\"WidgetBlueprint\") to discover paths."));
				return sol::lua_nil;
			}

			// Accept nil / empty-string to clear the desired focus.
			FName FocusName = NAME_None;
			if (WidgetObj.get_type() == sol::type::string)
			{
				FString WidgetName = NeoLuaStr::ToFString(WidgetObj.as<std::string>());
				if (!WidgetName.IsEmpty())
				{
					if (!WBP->WidgetTree || !WBP->WidgetTree->FindWidget(::FName(*WidgetName)))
					{
						Session.Log(FString::Printf(TEXT("[FAIL] set_desired_focus -> widget '%s' not found. Call list_widgets() to discover valid names."), *WidgetName));
						return sol::lua_nil;
					}
					FocusName = ::FName(*WidgetName);
				}
			}
			else if (WidgetObj.get_type() != sol::type::lua_nil)
			{
				Session.Log(TEXT("[FAIL] set_desired_focus -> argument must be widget name string or nil"));
				return sol::lua_nil;
			}

			// The engine helper (WidgetBlueprintEditorUtils.cpp:238) silently no-ops when
			// GeneratedClass is null (uncompiled WBP). Fail loudly so the agent doesn't wonder
			// why their focus setting didn't stick.
			if (!WBP->GeneratedClass)
			{
				Session.Log(TEXT("[FAIL] set_desired_focus -> blueprint has no GeneratedClass (not compiled yet). Compile the Widget Blueprint first, then retry."));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaSetDesiredFocus", "Set Desired Focus"));
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			FWidgetBlueprintEditorUtils::SetDesiredFocus(WBP, FocusName);
#else
			if (UUserWidget* WidgetCDO = WBP->GeneratedClass->GetDefaultObject<UUserWidget>())
			{
				WidgetCDO->SetFlags(RF_Transactional);
				WidgetCDO->Modify();
				WidgetCDO->SetDesiredFocusWidget(FocusName);
			}
#endif
			FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

			Session.Log(FString::Printf(TEXT("[OK] set_desired_focus(\"%s\")"),
				FocusName.IsNone() ? TEXT("(cleared)") : *FocusName.ToString()));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// configure_thumbnail(texture_path) -> set the asset thumbnail texture
		// ================================================================
		BPObj.set_function("configure_thumbnail", [FPath, &Session](sol::table /*self*/,
			const std::string& TexturePath, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FTexPath = NeoLuaStr::ToFString(TexturePath);

			UWidgetBlueprint* WBP = NeoLuaAsset::Resolve<UWidgetBlueprint>(FPath);
			if (!WBP)
			{
				Session.Log(TEXT("[FAIL] configure_thumbnail -> blueprint not found. Call open_asset(path) to reload, or find_assets(\"WidgetBlueprint\") to discover paths."));
				return sol::lua_nil;
			}

			UTexture2D* SourceTexture = NeoLuaAsset::Resolve<UTexture2D>(FTexPath);
			if (!SourceTexture)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] configure_thumbnail -> texture '%s' not found. Pass a valid UTexture2D asset path such as /Game/UI/Thumbnails/MyIcon.MyIcon"), *FTexPath));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaConfigureThumbnail", "Configure Widget Blueprint Thumbnail"));
			WBP->Modify();

			// CRITICAL: FWidgetBlueprintEditorUtils::SetTextureAsAssetThumbnail at
			// WidgetBlueprintEditorUtils.cpp:3130 calls Texture->Rename(ThumbnailName, WidgetBlueprint, …)
			// which *moves* the supplied texture into the WBP as a subobject named "Thumbnail".
			// The engine's own caller (WidgetThumbnailCustomization.cpp:82) passes a transient
			// texture from FImageUtils::ImportFileAsTexture2D, so the destructive rename is safe there.
			// We take a saved asset — if we passed it directly, the source asset would vanish from
			// its original package and every reference to it would break. Duplicate first.
			UTexture2D* ThumbnailCopy = DuplicateObject<UTexture2D>(SourceTexture, GetTransientPackage());
			if (!ThumbnailCopy)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] configure_thumbnail -> failed to duplicate source texture '%s'"), *FTexPath));
				return sol::lua_nil;
			}
			FWidgetBlueprintEditorUtils::SetTextureAsAssetThumbnail(WBP, ThumbnailCopy);

			Session.Log(FString::Printf(TEXT("[OK] configure_thumbnail(\"%s\") -> duplicated to WBP subobject"), *FTexPath));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// export_widgets(names?) -> serialized text for the subtree
		// ================================================================
		BPObj.set_function("export_widgets", [FPath, &Session](sol::table /*self*/,
			sol::optional<sol::object> NamesOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			UWidgetBlueprint* WBP = NeoLuaAsset::Resolve<UWidgetBlueprint>(FPath);
			if (!WBP || !WBP->WidgetTree)
			{
				Session.Log(TEXT("[FAIL] export_widgets -> no widget tree. The blueprint has no UWidgetTree — open_asset(path) with a valid WidgetBlueprint, or call add_widget(type) to seed a root widget."));
				return sol::lua_nil;
			}

			TArray<UWidget*> ToExport;

			// No arg / nil -> export the whole tree from root. Otherwise accept a single name or an array of names.
			const bool bNoArg = !NamesOpt.has_value() || NamesOpt.value().get_type() == sol::type::lua_nil;
			if (bNoArg)
			{
				if (WBP->WidgetTree->RootWidget)
				{
					ToExport.Add(WBP->WidgetTree->RootWidget);
					UWidgetTree::GetChildWidgets(WBP->WidgetTree->RootWidget, ToExport);
				}
			}
			else if (NamesOpt.value().is<std::string>())
			{
				FString WidgetName = NeoLuaStr::ToFString(NamesOpt.value().as<std::string>());
				UWidget* Widget = WBP->WidgetTree->FindWidget(::FName(*WidgetName));
				if (!Widget)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] export_widgets -> widget '%s' not found. Call list_widgets() to discover valid names."), *WidgetName));
					return sol::lua_nil;
				}
				ToExport.Add(Widget);
			}
			else if (NamesOpt.value().is<sol::table>())
			{
				sol::table NamesTbl = NamesOpt.value().as<sol::table>();
				for (auto& Pair : NamesTbl)
				{
					if (!Pair.second.is<std::string>()) continue;
					FString WidgetName = NeoLuaStr::ToFString(Pair.second.as<std::string>());
					UWidget* Widget = WBP->WidgetTree->FindWidget(::FName(*WidgetName));
					if (!Widget)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] export_widgets -> widget '%s' not found. Call list_widgets() to discover valid names."), *WidgetName));
						return sol::lua_nil;
					}
					ToExport.Add(Widget);
				}
			}
			else
			{
				Session.Log(TEXT("[FAIL] export_widgets -> argument must be nil, a widget name string, or an array of widget names"));
				return sol::lua_nil;
			}

			if (ToExport.Num() == 0)
			{
				Session.Log(TEXT("[FAIL] export_widgets -> no widgets to export (tree is empty)"));
				return sol::lua_nil;
			}

			FString ExportedText;
			FWidgetBlueprintEditorUtils::ExportWidgetsToText(ToExport, ExportedText);

			Session.Log(FString::Printf(TEXT("[OK] export_widgets() -> %d widgets, %d chars"),
				ToExport.Num(), ExportedText.Len()));
			return sol::make_object(Lua, std::string(TCHAR_TO_UTF8(*ExportedText)));
		});

		// ================================================================
		// import_widgets(text, opts?) -> paste serialized widgets under a parent
		//
		// opts = { parent = "panel_name" }
		//
		// ImportWidgetsFromText (WidgetBlueprintEditorUtils.cpp:2247) constructs the widgets
		// outered to WBP->WidgetTree but does NOT parent them into the tree or register them
		// in WidgetVariableNameToGuidMap. The engine's paste flow (lines 1960-1993) completes
		// the import by calling OnVariableAdded for each and parenting the root widgets under
		// a target panel. We mirror that here — headless paste is pointless if the widgets
		// end up as stray UObjects and the compiler map is stale.
		// ================================================================
		BPObj.set_function("import_widgets", [FPath, &Session](sol::table /*self*/,
			const std::string& TextToImport, sol::optional<sol::table> OptsOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString PayloadText = NeoLuaStr::ToFString(TextToImport);

			UWidgetBlueprint* WBP = NeoLuaAsset::Resolve<UWidgetBlueprint>(FPath);
			if (!WBP || !WBP->WidgetTree)
			{
				Session.Log(TEXT("[FAIL] import_widgets -> no widget tree. The blueprint has no UWidgetTree — open_asset(path) with a valid WidgetBlueprint, or call add_widget(type) to seed a root widget."));
				return sol::lua_nil;
			}

			if (PayloadText.IsEmpty())
			{
				Session.Log(TEXT("[FAIL] import_widgets -> empty text. Pass the string returned by export_widgets()."));
				return sol::lua_nil;
			}

			{
				FString PreflightReason;
				if (!WidgetHelper::ValidateImportPayload(WBP, PayloadText, PreflightReason))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] import_widgets -> %s"), *PreflightReason));
					return sol::lua_nil;
				}
			}

			// Resolve target parent. Required when the tree already has a root; optional when
			// the tree is empty (imported roots can become the new tree root).
			FString ParentName;
			if (OptsOpt.has_value())
			{
				sol::optional<std::string> POpt = OptsOpt.value().get<sol::optional<std::string>>("parent");
				if (POpt.has_value()) ParentName = NeoLuaStr::ToFStringOpt(POpt);
			}

			UPanelWidget* TargetParent = nullptr;
			if (!ParentName.IsEmpty())
			{
				TargetParent = Cast<UPanelWidget>(WBP->WidgetTree->FindWidget(::FName(*ParentName)));
				if (!TargetParent)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] import_widgets -> parent '%s' not found or not a panel. Call list_widgets() to find a panel widget."), *ParentName));
					return sol::lua_nil;
				}
				// Preflight single-child fullness BEFORE calling ImportWidgetsFromText. Otherwise we'd
				// construct imported widgets + register them in the GUID map, then discover at placement
				// time that we can't attach them — leaving unparented widgets + stale map entries.
				// Engine paste (WidgetBlueprintEditorUtils.cpp:2043) handles this by auto-evicting the
				// existing child; we require the caller to do that explicitly via remove_widget.
				if (!TargetParent->CanAddMoreChildren())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] import_widgets -> parent '%s' cannot accept more children (single-child panel is full). Remove its current child via remove_widget first, or choose a multi-child panel."), *ParentName));
					return sol::lua_nil;
				}
			}
			else if (WBP->WidgetTree->RootWidget)
			{
				Session.Log(TEXT("[FAIL] import_widgets -> tree already has a root; pass {parent=\"panel_name\"} to specify where to paste. Call list_widgets() to find a panel widget."));
				return sol::lua_nil;
			}

			const FScopedTransaction Tx(NSLOCTEXT("AIK", "LuaImportWidgets", "Import Widgets"));
			WBP->Modify();
			WBP->WidgetTree->SetFlags(RF_Transactional);
			WBP->WidgetTree->Modify();

			TSet<UWidget*> ImportedWidgetSet;
			TMap<FName, UWidgetSlotPair*> PastedExtraSlotData;
			FWidgetBlueprintEditorUtils::ImportWidgetsFromText(WBP, PayloadText, ImportedWidgetSet, PastedExtraSlotData);

			if (ImportedWidgetSet.Num() == 0)
			{
				WidgetHelper::DiscardImportedWidgets(ImportedWidgetSet);
				Session.Log(TEXT("[FAIL] import_widgets -> parsed text contained no widgets. The input must be the exact string from export_widgets()."));
				return sol::lua_nil;
			}

			// Collect root-most pasted widgets (no parent AND not content of another paste via
			// INamedSlotInterface) — matches the engine's root detection at lines 1970-1991.
			TArray<UWidget*> RootsToPlace;
			for (UWidget* NewWidget : ImportedWidgetSet)
			{
				if (!NewWidget || NewWidget->GetParent()) continue;

				bool bIsNamedSlotContent = false;
				for (UWidget* ContainerWidget : ImportedWidgetSet)
				{
					if (INamedSlotInterface* NSI = Cast<INamedSlotInterface>(ContainerWidget))
					{
						if (NSI->ContainsContent(NewWidget))
						{
							bIsNamedSlotContent = true;
							break;
						}
					}
				}
				if (!bIsNamedSlotContent)
				{
					RootsToPlace.Add(NewWidget);
				}
			}

			if (RootsToPlace.Num() == 0)
			{
				WidgetHelper::DiscardImportedWidgets(ImportedWidgetSet);
				Session.Log(TEXT("[FAIL] import_widgets -> imported text did not contain a root widget to place."));
				return sol::lua_nil;
			}

			if (TargetParent && !TargetParent->CanHaveMultipleChildren() && RootsToPlace.Num() > 1)
			{
				WidgetHelper::DiscardImportedWidgets(ImportedWidgetSet);
				Session.Log(FString::Printf(TEXT("[FAIL] import_widgets -> parent '%s' accepts only one child, but payload contains %d root widgets."),
					*ParentName, RootsToPlace.Num()));
				return sol::lua_nil;
			}

			// Place the root widgets.
			if (TargetParent)
			{
				TargetParent->SetFlags(RF_Transactional);
				TargetParent->Modify();
				for (UWidget* Root : RootsToPlace)
				{
					if (!TargetParent->CanAddMoreChildren())
					{
						WidgetHelper::DiscardImportedWidgets(ImportedWidgetSet);
						Session.Log(FString::Printf(TEXT("[FAIL] import_widgets -> parent '%s' became full while placing imported widgets; import discarded."),
							*ParentName));
						return sol::lua_nil;
					}
					if (!TargetParent->AddChild(Root))
					{
						WidgetHelper::DiscardImportedWidgets(ImportedWidgetSet);
						Session.Log(FString::Printf(TEXT("[FAIL] import_widgets -> parent '%s' rejected imported root '%s'; import discarded."),
							*ParentName, Root ? *Root->GetName() : TEXT("<null>")));
						return sol::lua_nil;
					}
				}
			}
			else
			{
				// Tree was empty. Assign the first root as the tree root. If multiple roots, wrap in a CanvasPanel.
				if (RootsToPlace.Num() == 1)
				{
					WBP->WidgetTree->RootWidget = RootsToPlace[0];
				}
				else if (RootsToPlace.Num() > 1)
				{
					UCanvasPanel* AutoRoot = WBP->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass());
					WBP->WidgetTree->RootWidget = AutoRoot;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
					WidgetHelper::RegisterWidgetVariableGuidIfMissing(WBP, AutoRoot->GetFName());
#endif
					for (UWidget* Root : RootsToPlace)
					{
						if (!AutoRoot->AddChild(Root))
						{
							WidgetHelper::DiscardImportedWidgets(ImportedWidgetSet);
							if (WBP->WidgetTree->RootWidget == AutoRoot)
							{
								WBP->WidgetTree->RootWidget = nullptr;
							}
							AutoRoot->Rename(nullptr, GetTransientPackage());
							Session.Log(FString::Printf(TEXT("[FAIL] import_widgets -> generated CanvasPanel root rejected imported root '%s'; import discarded."),
								Root ? *Root->GetName() : TEXT("<null>")));
							return sol::lua_nil;
						}
					}
				}
			}

			// Register each imported widget in the compiler's GUID map only after placement
			// succeeds. UWidgetBlueprint::OnVariableAdded ensure()s on duplicates, so use
			// the idempotent wrapper instead of calling the engine hook directly.
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			for (UWidget* NewWidget : ImportedWidgetSet)
			{
				if (NewWidget)
				{
					WidgetHelper::RegisterWidgetVariableGuidIfMissing(WBP, NewWidget->GetFName());
				}
			}
#endif

			sol::table Result = Lua.create_table();
			int32 Idx = 1;
			for (UWidget* W : ImportedWidgetSet)
			{
				if (!W) continue;
				sol::table E = Lua.create_table();
				E["name"] = TCHAR_TO_UTF8(*W->GetName());
				E["type"] = TCHAR_TO_UTF8(*W->GetClass()->GetName());
				E["is_root"] = !W->GetParent();
				Result[Idx++] = E;
			}

			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

			Session.Log(FString::Printf(TEXT("[OK] import_widgets() -> %d widgets (%d root(s) placed under %s)"),
				ImportedWidgetSet.Num(), RootsToPlace.Num(),
				TargetParent ? *TargetParent->GetName() : TEXT("new tree root")));
			return Result;
		});
	});
}

REGISTER_LUA_BINDING(WidgetBlueprint, WidgetBlueprintDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindWidgetBlueprint(Lua, Session);
});
