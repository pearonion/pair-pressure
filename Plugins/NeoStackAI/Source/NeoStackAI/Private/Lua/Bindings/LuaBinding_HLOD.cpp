// Copyright 2025-2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Lua/LuaAssetResolver.h"
#include "Lua/LuaPropertyHelper.h"
#include "Lua/LuaStr.h"
#include <sol/sol.hpp>
#include "Tools/NeoStackToolUtils.h"

#include "WorldPartition/HLOD/HLODActor.h"
#include "WorldPartition/HLOD/HLODLayer.h"
#include "WorldPartition/HLOD/HLODBuilder.h"
#include "WorldPartition/HLOD/HLODModifier.h"
#include "WorldPartition/HLOD/HLODSourceActors.h"
#include "WorldPartition/HLOD/HLODSourceActorsFromCell.h"
#include "WorldPartition/HLOD/HLODSourceActorsFromLevel.h"
#include "WorldPartition/HLOD/HLODStats.h"
#include "WorldPartition/HLOD/HLODEditorSubsystem.h"
#include "WorldPartition/IWorldPartitionEditorModule.h"
#include "HierarchicalLOD.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "Engine/LODActor.h"
#include "Editor.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/SavePackage.h"
#include "ScopedTransaction.h"

// ─── Documentation ───
//
// hlod_create_layer was removed — use create_asset(path, "hlodlayer", opts?)
// (LuaBinding_CreateBlueprint.cpp:18; alias at BlueprintUtils.cpp:1935). Apply
// post-create configuration with hlod_configure_layer.

static TArray<FLuaFunctionDoc> HLODDocs = {
	{ TEXT("hlod_list_actors()"), TEXT("List all World Partition HLOD actors in the current level"), TEXT("table[]") },
	{ TEXT("hlod_build(actor_label?, force?)"), TEXT("Build HLOD for a specific actor or all — force=true to rebuild unconditionally"), TEXT("int or nil") },
	{ TEXT("hlod_check_hash(actor_label?)"), TEXT("Check if HLOD actor(s) need rebuild by comparing hashes — always returns an array"), TEXT("table[]") },
	{ TEXT("hlod_get_layers()"), TEXT("List all UHLODLayer assets found in the project via asset registry"), TEXT("table[]") },
	{ TEXT("hlod_configure_layer(path, options)"), TEXT("Configure an HLOD layer — options: layer_type, parent_layer, linked_layer, builder_class, cell_size, loading_range, is_spatially_loaded, hlod_actor_class, hlod_modifier_class. For nested HLODBuilderSettings fields use open_asset(path):set('HLODBuilderSettings.<field>', value)"), TEXT("true or nil") },
	{ TEXT("hlod_get_source_actors(actor_label)"), TEXT("Get source actor info for a World Partition HLOD actor"), TEXT("table or nil") },
	{ TEXT("hlod_get_stats(actor_label)"), TEXT("Get full FWorldPartitionHLODStats report (mesh/material/memory/build/input groups) for an HLOD actor"), TEXT("table or nil") },
	{ TEXT("hlod_set_actor_field(actor_label, field, value)"), TEXT("Mutate a runtime HLOD actor — field: require_warmup, hlod_bounds, min_visible_distance, hlod_hash, lod_level, source_cell_guid, is_standalone, set_stat, reset_stats. value shape depends on field"), TEXT("true or nil") },
	{ TEXT("hlod_export(actor_label, options)"), TEXT("Export HLOD assets — options: export_path REQUIRED, mesh_origin ('Actor'/'World'), test_export_only (bool dry-run)"), TEXT("table or nil") },
	{ TEXT("hlod_write_stats(filename, stats_type?)"), TEXT("Write per-world HLOD stats CSV via UWorldPartitionHLODEditorSubsystem — stats_type: 'default' or 'input_details'"), TEXT("true or nil") },
	{ TEXT("hlod_legacy_build(force?)"), TEXT("Trigger a legacy (non-WP) HLOD build for the current level"), TEXT("true or nil") },
	{ TEXT("hlod_legacy_clear()"), TEXT("Clear all legacy HLOD data from the current level"), TEXT("true or nil") },
	{ TEXT("hlod_legacy_needs_build(force?)"), TEXT("Check if the legacy HLOD system needs a rebuild"), TEXT("bool") },
	{ TEXT("hlod_legacy_preview_build()"), TEXT("Build clusters and spawn LODActors WITHOUT creating/merging meshes"), TEXT("true or nil") },
	{ TEXT("hlod_legacy_clear_preview()"), TEXT("Clear only the preview ALODActors"), TEXT("true or nil") },
	{ TEXT("hlod_legacy_build_meshes(force?)"), TEXT("Build LOD meshes for all existing LODActors"), TEXT("true or nil") },
	{ TEXT("hlod_legacy_save_meshes()"), TEXT("Save HLOD meshes for actors in all the world's levels"), TEXT("true or nil") },
	{ TEXT("hlod_legacy_delete_empty_packages(level_name?)"), TEXT("Delete HLOD packages that are empty for the given level (default: persistent level)"), TEXT("true or nil") },
	{ TEXT("hlod_legacy_build_lod_actor(actor_label, lod_level)"), TEXT("Build a single ALODActor's mesh at a specific LOD level"), TEXT("true or nil") },
	{ TEXT("hlod_list_lod_actors()"), TEXT("List all legacy LOD actors (ALODActor) in the current level — includes triangle counts"), TEXT("table[]") },
};

// ─── Helpers ───

static FString HLODLayerTypeToString(EHLODLayerType Type)
{
	switch (Type)
	{
	case EHLODLayerType::Instancing:       return TEXT("Instancing");
	case EHLODLayerType::MeshMerge:        return TEXT("MeshMerge");
	case EHLODLayerType::MeshSimplify:     return TEXT("MeshSimplify");
	case EHLODLayerType::MeshApproximate:  return TEXT("MeshApproximate");
	case EHLODLayerType::Custom:           return TEXT("Custom");
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
	case EHLODLayerType::CustomHLODActor:  return TEXT("CustomHLODActor");
#endif
	default:                               return TEXT("Unknown");
	}
}

static bool StringToHLODLayerType(const FString& Str, EHLODLayerType& OutType)
{
	if (Str.Equals(TEXT("Instancing"), ESearchCase::IgnoreCase))           { OutType = EHLODLayerType::Instancing; return true; }
	if (Str.Equals(TEXT("MeshMerge"), ESearchCase::IgnoreCase))            { OutType = EHLODLayerType::MeshMerge; return true; }
	if (Str.Equals(TEXT("MeshSimplify"), ESearchCase::IgnoreCase))         { OutType = EHLODLayerType::MeshSimplify; return true; }
	if (Str.Equals(TEXT("MeshApproximate"), ESearchCase::IgnoreCase))      { OutType = EHLODLayerType::MeshApproximate; return true; }
	if (Str.Equals(TEXT("Custom"), ESearchCase::IgnoreCase))               { OutType = EHLODLayerType::Custom; return true; }
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
	if (Str.Equals(TEXT("CustomHLODActor"), ESearchCase::IgnoreCase))      { OutType = EHLODLayerType::CustomHLODActor; return true; }
#endif
	return false;
}

static UHLODLayer* LoadHLODLayerByPath(const FString& Path)
{
	return NeoLuaAsset::ResolveWithRegistry<UHLODLayer>(Path);
}

// Property writes on UHLODLayer MUST go through PreEditChange/PostEditChangeProperty so that
// the engine's property-name branches in UHLODLayer::PostEditChangeProperty fire — the LayerType
// branch (HLODLayer.cpp:175) recreates HLODBuilderSettings via WorldPartitionHLODUtilities, and the
// ParentLayer branch (HLODLayer.cpp:185) runs circular-parent + invalid-source-material validation
// and clears the bad parent. Bare PostEditChange() supplies a null Property and skips both.
static bool WriteLayerProperty(UHLODLayer* Layer, const FString& PropertyName, const NeoLuaProperty::FPropertyValueInput& Input, FString& OutError)
{
	return NeoLuaProperty::SetNamedPropertyValueWithEditChange(Layer, PropertyName, Input, OutError);
}

static bool WriteLayerObjectProperty(UHLODLayer* Layer, const FString& PropertyName, UObject* Value, FString& OutError)
{
	FProperty* Property = NeoLuaProperty::FindPropertyByName(Layer->GetClass(), PropertyName);
	if (!Property)
	{
		OutError = FString::Printf(TEXT("property '%s' not found"), *PropertyName);
		return false;
	}

	FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Property);
	if (!ObjProp)
	{
		OutError = FString::Printf(TEXT("property '%s' is not an object/class property"), *PropertyName);
		return false;
	}

	Layer->Modify();
	Layer->PreEditChange(Property);
	ObjProp->SetObjectPropertyValue(ObjProp->ContainerPtrToValuePtr<void>(Layer), Value);
	FPropertyChangedEvent ChangeEvent(Property, EPropertyChangeType::ValueSet);
	// UHLODLayer::PostEditChangeProperty is declared `private` in UE 5.7
	// (HLODLayer.h:89, inside a private: section). Route through UObject* so we
	// call the public base entry point; virtual dispatch still lands on the
	// layer's override.
	static_cast<UObject*>(Layer)->PostEditChangeProperty(ChangeEvent);
	return true;
}

static bool ApplyHLODLayerOptions(UHLODLayer* Layer, const sol::table& Options, FString& OutError)
{
	// --- Phase 1: Validate all options that can fail BEFORE modifying anything ---
	EHLODLayerType NewType = Layer->GetLayerType();
	bool bSetType = false;
	auto TypeOpt = Options.get<sol::optional<std::string>>("layer_type");
	if (TypeOpt.has_value())
	{
		FString TypeStr = NeoLuaStr::ToFStringOpt(TypeOpt);
		if (!StringToHLODLayerType(TypeStr, NewType))
		{
			OutError = FString::Printf(TEXT("Unknown layer_type '%s' (valid: Instancing, MeshMerge, MeshSimplify, MeshApproximate, Custom, CustomHLODActor)"), *TypeStr);
			return false;
		}
		bSetType = true;
	}

	UHLODLayer* ResolvedParent = nullptr;
	bool bSetParent = false;
	bool bClearParent = false;
	auto ParentOpt = Options.get<sol::optional<std::string>>("parent_layer");
	if (ParentOpt.has_value())
	{
		FString ParentPath = NeoLuaStr::ToFStringOpt(ParentOpt);
		if (ParentPath.IsEmpty())
		{
			bClearParent = true;
		}
		else
		{
			ResolvedParent = LoadHLODLayerByPath(ParentPath);
			if (!ResolvedParent)
			{
				OutError = FString::Printf(TEXT("Could not load parent_layer '%s' — pass a /Game/... path or use hlod_get_layers() to list valid layers"), *ParentPath);
				return false;
			}
			bSetParent = true;
		}
	}

	UHLODLayer* ResolvedLinked = nullptr;
	bool bSetLinked = false;
	bool bClearLinked = false;
	auto LinkedOpt = Options.get<sol::optional<std::string>>("linked_layer");
	if (LinkedOpt.has_value())
	{
		FString LinkedPath = NeoLuaStr::ToFStringOpt(LinkedOpt);
		if (LinkedPath.IsEmpty())
		{
			bClearLinked = true;
		}
		else
		{
			ResolvedLinked = LoadHLODLayerByPath(LinkedPath);
			if (!ResolvedLinked)
			{
				OutError = FString::Printf(TEXT("Could not load linked_layer '%s' — pass a /Game/... path or use hlod_get_layers() to list valid layers"), *LinkedPath);
				return false;
			}
			bSetLinked = true;
		}
	}

	UClass* ResolvedActorClass = nullptr;
	bool bSetActorClass = false;
	auto ActorClassOpt = Options.get<sol::optional<std::string>>("hlod_actor_class");
	if (ActorClassOpt.has_value())
	{
		FString ClassPath = NeoLuaStr::ToFStringOpt(ActorClassOpt);
		if (ClassPath.IsEmpty())
		{
			bSetActorClass = true; // will clear
		}
		else
		{
			ResolvedActorClass = LoadClass<AWorldPartitionHLOD>(nullptr, *ClassPath);
			if (!ResolvedActorClass)
			{
				OutError = FString::Printf(TEXT("Could not load hlod_actor_class '%s' — pass a full /Script/... or /Game/... class path"), *ClassPath);
				return false;
			}
			bSetActorClass = true;
		}
	}

	UClass* ResolvedModifierClass = nullptr;
	bool bSetModifierClass = false;
	auto ModifierClassOpt = Options.get<sol::optional<std::string>>("hlod_modifier_class");
	if (ModifierClassOpt.has_value())
	{
		FString ClassPath = NeoLuaStr::ToFStringOpt(ModifierClassOpt);
		if (ClassPath.IsEmpty())
		{
			bSetModifierClass = true; // will clear
		}
		else
		{
			ResolvedModifierClass = LoadClass<UWorldPartitionHLODModifier>(nullptr, *ClassPath);
			if (!ResolvedModifierClass)
			{
				OutError = FString::Printf(TEXT("Could not load hlod_modifier_class '%s' — pass a full /Script/... or /Game/... class path"), *ClassPath);
				return false;
			}
			bSetModifierClass = true;
		}
	}

	// HLODBuilderClass — only meaningful when LayerType == Custom (engine HLODLayer.h:101 EditCondition).
	UClass* ResolvedBuilderClass = nullptr;
	bool bSetBuilderClass = false;
	auto BuilderClassOpt = Options.get<sol::optional<std::string>>("builder_class");
	if (BuilderClassOpt.has_value())
	{
		FString ClassPath = NeoLuaStr::ToFStringOpt(BuilderClassOpt);
		if (ClassPath.IsEmpty())
		{
			bSetBuilderClass = true; // will clear
		}
		else
		{
			ResolvedBuilderClass = LoadClass<UHLODBuilder>(nullptr, *ClassPath);
			if (!ResolvedBuilderClass)
			{
				OutError = FString::Printf(TEXT("Could not load builder_class '%s' — pass a full /Script/... or /Game/... UHLODBuilder subclass path"), *ClassPath);
				return false;
			}
			bSetBuilderClass = true;
		}
	}

	// --- Phase 2: All validation passed — now apply changes (each write brackets PreEditChange/PostEditChangeProperty) ---
	FScopedTransaction Transaction(FText::FromString(TEXT("Configure HLOD Layer")));

	if (bSetType)
	{
		// Write LayerType through the property system so HLODLayer::PostEditChangeProperty fires the
		// LayerType branch and recreates HLODBuilderSettings via WorldPartitionHLODUtilities.
		NeoLuaProperty::FPropertyValueInput Input;
		Input.bIsString = true;
		Input.StringValue = HLODLayerTypeToString(NewType);
		FString Err;
		if (!WriteLayerProperty(Layer, TEXT("LayerType"), Input, Err))
		{
			OutError = FString::Printf(TEXT("LayerType write failed: %s"), *Err);
			return false;
		}
	}

	if (bClearParent || bSetParent)
	{
		// ParentLayer write triggers HLODLayer::PostEditChangeProperty's circular-parent +
		// invalid-source-material validation; an invalid parent is cleared inline by the engine.
		FString Err;
		if (!WriteLayerObjectProperty(Layer, TEXT("ParentLayer"), bSetParent ? ResolvedParent : nullptr, Err))
		{
			OutError = FString::Printf(TEXT("ParentLayer write failed: %s"), *Err);
			return false;
		}
	}

	if (bClearLinked || bSetLinked)
	{
		FString Err;
		if (!WriteLayerObjectProperty(Layer, TEXT("LinkedLayer"), bSetLinked ? ResolvedLinked : nullptr, Err))
		{
			OutError = FString::Printf(TEXT("LinkedLayer write failed: %s"), *Err);
			return false;
		}
	}

	if (bSetActorClass)
	{
		FString Err;
		if (!WriteLayerObjectProperty(Layer, TEXT("HLODActorClass"), ResolvedActorClass, Err))
		{
			OutError = FString::Printf(TEXT("HLODActorClass write failed: %s"), *Err);
			return false;
		}
	}

	if (bSetModifierClass)
	{
		FString Err;
		if (!WriteLayerObjectProperty(Layer, TEXT("HLODModifierClass"), ResolvedModifierClass, Err))
		{
			OutError = FString::Printf(TEXT("HLODModifierClass write failed: %s"), *Err);
			return false;
		}
	}

	if (bSetBuilderClass)
	{
		// HLODBuilderClass write fires the same engine branch as LayerType (HLODLayer.cpp:175-184),
		// so HLODBuilderSettings is recreated to match the new builder class automatically.
		FString Err;
		if (!WriteLayerObjectProperty(Layer, TEXT("HLODBuilderClass"), ResolvedBuilderClass, Err))
		{
			OutError = FString::Printf(TEXT("HLODBuilderClass write failed: %s"), *Err);
			return false;
		}
	}

	// cell_size and loading_range are deprecated in 5.7 but the properties still exist.
	sol::optional<double> CellSizeOpt = Options.get<sol::optional<double>>("cell_size");
	if (CellSizeOpt.has_value())
	{
		NeoLuaProperty::FPropertyValueInput Input;
		Input.bIsNumber = true;
		Input.NumberValue = static_cast<double>(static_cast<int32>(CellSizeOpt.value()));
		FString Err;
		WriteLayerProperty(Layer, TEXT("CellSize"), Input, Err);
	}

	sol::optional<double> LoadingRangeOpt = Options.get<sol::optional<double>>("loading_range");
	if (LoadingRangeOpt.has_value())
	{
		NeoLuaProperty::FPropertyValueInput Input;
		Input.bIsNumber = true;
		Input.NumberValue = LoadingRangeOpt.value();
		FString Err;
		WriteLayerProperty(Layer, TEXT("LoadingRange"), Input, Err);
	}

	auto SpatialOpt = Options.get<sol::optional<bool>>("is_spatially_loaded");
	if (SpatialOpt.has_value())
	{
		NeoLuaProperty::FPropertyValueInput Input;
		Input.bIsBool = true;
		Input.BoolValue = SpatialOpt.value();
		FString Err;
		WriteLayerProperty(Layer, TEXT("bIsSpatiallyLoaded"), Input, Err);
	}

	Layer->MarkPackageDirty();
	return true;
}

// Full FWorldPartitionHLODStats coverage — engine defines 21 stat names in HLODStats.h.
// Each entry maps an FName accessor to (group, lua_key) for grouped report output.
struct FHLODStatEntry
{
	const FName& Name;
	const TCHAR* Group;
	const TCHAR* Key;
};

static const FHLODStatEntry& GetHLODStatEntries(int32 Index)
{
	static const FHLODStatEntry Entries[] = {
		// Input
		{ FWorldPartitionHLODStats::InputActorCount,             TEXT("input"),    TEXT("actor_count") },
		{ FWorldPartitionHLODStats::InputTriangleCount,          TEXT("input"),    TEXT("triangle_count") },
		{ FWorldPartitionHLODStats::InputVertexCount,            TEXT("input"),    TEXT("vertex_count") },
		// Mesh
		{ FWorldPartitionHLODStats::MeshInstanceCount,           TEXT("mesh"),     TEXT("instance_count") },
		{ FWorldPartitionHLODStats::MeshNaniteTriangleCount,     TEXT("mesh"),     TEXT("nanite_triangle_count") },
		{ FWorldPartitionHLODStats::MeshNaniteVertexCount,       TEXT("mesh"),     TEXT("nanite_vertex_count") },
		{ FWorldPartitionHLODStats::MeshTriangleCount,           TEXT("mesh"),     TEXT("triangle_count") },
		{ FWorldPartitionHLODStats::MeshVertexCount,             TEXT("mesh"),     TEXT("vertex_count") },
		{ FWorldPartitionHLODStats::MeshUVChannelCount,          TEXT("mesh"),     TEXT("uv_channel_count") },
		// Material
		{ FWorldPartitionHLODStats::MaterialBaseColorTextureSize, TEXT("material"), TEXT("base_color_texture_size") },
		{ FWorldPartitionHLODStats::MaterialNormalTextureSize,    TEXT("material"), TEXT("normal_texture_size") },
		{ FWorldPartitionHLODStats::MaterialEmissiveTextureSize,  TEXT("material"), TEXT("emissive_texture_size") },
		{ FWorldPartitionHLODStats::MaterialMetallicTextureSize,  TEXT("material"), TEXT("metallic_texture_size") },
		{ FWorldPartitionHLODStats::MaterialRoughnessTextureSize, TEXT("material"), TEXT("roughness_texture_size") },
		{ FWorldPartitionHLODStats::MaterialSpecularTextureSize,  TEXT("material"), TEXT("specular_texture_size") },
		// Memory
		{ FWorldPartitionHLODStats::MemoryMeshResourceSizeBytes,     TEXT("memory"), TEXT("mesh_resource_bytes") },
		{ FWorldPartitionHLODStats::MemoryTexturesResourceSizeBytes, TEXT("memory"), TEXT("textures_resource_bytes") },
		{ FWorldPartitionHLODStats::MemoryDiskSizeBytes,             TEXT("memory"), TEXT("disk_bytes") },
		// Build
		{ FWorldPartitionHLODStats::BuildTimeLoadMilliseconds,  TEXT("build"), TEXT("load_ms") },
		{ FWorldPartitionHLODStats::BuildTimeBuildMilliseconds, TEXT("build"), TEXT("build_ms") },
		{ FWorldPartitionHLODStats::BuildTimeTotalMilliseconds, TEXT("build"), TEXT("total_ms") },
	};
	return Entries[Index];
}

static constexpr int32 NumHLODStatEntries = 21;

// Look up an HLOD actor by label — shared by every per-actor verb.
static AWorldPartitionHLOD* FindHLODActorByLabel(UWorld* World, const FString& Label)
{
	for (TActorIterator<AWorldPartitionHLOD> It(World); It; ++It)
	{
		AWorldPartitionHLOD* HLODActor = *It;
		if (HLODActor && HLODActor->GetActorLabel() == Label)
		{
			return HLODActor;
		}
	}
	return nullptr;
}

static ALODActor* FindLegacyLODActorByLabel(UWorld* World, const FString& Label)
{
	for (TActorIterator<ALODActor> It(World); It; ++It)
	{
		ALODActor* LODActor = *It;
		if (LODActor && LODActor->GetActorLabel() == Label)
		{
			return LODActor;
		}
	}
	return nullptr;
}

// ─── Binding ───

static void BindHLOD(sol::state& Lua, FLuaSessionData& Session)
{
	// ---- hlod_list_actors() ----
	Lua.set_function("hlod_list_actors", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Session.Log(TEXT("[FAIL] hlod_list_actors -> no editor world. Open a level in the editor and retry."));
			return sol::lua_nil;
		}

		sol::table Result = Lua.create_table();
		int32 Idx = 1;
		for (TActorIterator<AWorldPartitionHLOD> It(World); It; ++It)
		{
			AWorldPartitionHLOD* HLODActor = *It;
			if (!HLODActor) continue;

			sol::table Entry = Lua.create_table();
			Entry["label"] = std::string(TCHAR_TO_UTF8(*HLODActor->GetActorLabel()));
			Entry["name"] = std::string(TCHAR_TO_UTF8(*HLODActor->GetName()));
			Entry["lod_level"] = (int)HLODActor->GetLODLevel();
			Entry["min_visible_distance"] = HLODActor->GetMinVisibleDistance();
			Entry["hash"] = (double)HLODActor->GetHLODHash();

			FBox Bounds = HLODActor->GetHLODBounds();
			if (Bounds.IsValid)
			{
				sol::table BEntry = Lua.create_table();
				BEntry["min_x"] = Bounds.Min.X;
				BEntry["min_y"] = Bounds.Min.Y;
				BEntry["min_z"] = Bounds.Min.Z;
				BEntry["max_x"] = Bounds.Max.X;
				BEntry["max_y"] = Bounds.Max.Y;
				BEntry["max_z"] = Bounds.Max.Z;
				Entry["bounds"] = BEntry;
			}

			Result[Idx++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] hlod_list_actors -> %d HLOD actors"), Idx - 1));
		return sol::make_object(Lua, Result);
	});

	// ---- hlod_build(actor_label?, force?) ----
	Lua.set_function("hlod_build", [&Session](sol::optional<std::string> ActorLabel, sol::optional<bool> Force, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Session.Log(TEXT("[FAIL] hlod_build -> no editor world. Open a level in the editor and retry."));
			return sol::lua_nil;
		}

		bool bForce = Force.value_or(false);
		FString FilterLabel = NeoLuaStr::ToFStringOpt(ActorLabel);
		int32 BuiltCount = 0;

		for (TActorIterator<AWorldPartitionHLOD> It(World); It; ++It)
		{
			AWorldPartitionHLOD* HLODActor = *It;
			if (!HLODActor) continue;

			if (!FilterLabel.IsEmpty() && HLODActor->GetActorLabel() != FilterLabel)
				continue;

			HLODActor->BuildHLOD(bForce);
			BuiltCount++;
		}

		if (BuiltCount == 0 && !FilterLabel.IsEmpty())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] hlod_build -> no HLOD actor found with label '%s'. Run hlod_list_actors() and retry with an exact label."), *FilterLabel));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] hlod_build -> built %d HLOD actor(s)%s"), BuiltCount, bForce ? TEXT(" (forced)") : TEXT("")));
		return sol::make_object(Lua, BuiltCount);
	});

	// ---- hlod_check_hash(actor_label?) ----
	Lua.set_function("hlod_check_hash", [&Session](sol::optional<std::string> ActorLabel, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Session.Log(TEXT("[FAIL] hlod_check_hash -> no editor world. Open a level in the editor and retry."));
			return sol::lua_nil;
		}

		FString FilterLabel = NeoLuaStr::ToFStringOpt(ActorLabel);
		sol::table Result = Lua.create_table();
		int32 Idx = 1;

		for (TActorIterator<AWorldPartitionHLOD> It(World); It; ++It)
		{
			AWorldPartitionHLOD* HLODActor = *It;
			if (!HLODActor) continue;

			if (!FilterLabel.IsEmpty() && HLODActor->GetActorLabel() != FilterLabel)
				continue;

			uint32 StoredHash = HLODActor->GetHLODHash();
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			uint32 CurrentHash = HLODActor->ComputeHLODHash();
#else
			uint32 CurrentHash = StoredHash; // ComputeHLODHash not available pre-5.7
#endif

			sol::table Entry = Lua.create_table();
			Entry["label"] = std::string(TCHAR_TO_UTF8(*HLODActor->GetActorLabel()));
			Entry["stored_hash"] = (double)StoredHash;
			Entry["current_hash"] = (double)CurrentHash;
			Entry["needs_rebuild"] = (StoredHash != CurrentHash);
			Result[Idx++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] hlod_check_hash -> checked %d HLOD actor(s)"), Idx - 1));
		return sol::make_object(Lua, Result);
	});

	// ---- hlod_get_layers() ----
	Lua.set_function("hlod_get_layers", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);

		// Use asset registry to find persisted UHLODLayer assets (not transient/PIE objects)
		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		TArray<FAssetData> AssetDataList;
		AssetRegistry.GetAssetsByClass(UHLODLayer::StaticClass()->GetClassPathName(), AssetDataList);

		sol::table Result = Lua.create_table();
		int32 Idx = 1;

		for (const FAssetData& AssetData : AssetDataList)
		{
			UHLODLayer* Layer = Cast<UHLODLayer>(AssetData.GetAsset());
			if (!Layer) continue;

			sol::table Entry = Lua.create_table();
			const FString ObjectPath = Layer->GetPathName();
			Entry["name"] = std::string(TCHAR_TO_UTF8(*Layer->GetName()));
			Entry["path"] = std::string(TCHAR_TO_UTF8(*NeoLuaAsset::ToPackagePath(ObjectPath)));
			Entry["object_path"] = std::string(TCHAR_TO_UTF8(*ObjectPath));
			Entry["layer_type"] = std::string(TCHAR_TO_UTF8(*HLODLayerTypeToString(Layer->GetLayerType())));
			Entry["requires_warmup"] = Layer->DoesRequireWarmup();

			UHLODLayer* Parent = Layer->GetParentLayer();
			if (Parent)
			{
				Entry["parent_layer"] = std::string(TCHAR_TO_UTF8(*NeoLuaAsset::ToPackagePath(Parent->GetPathName())));
				Entry["parent_layer_object_path"] = std::string(TCHAR_TO_UTF8(*Parent->GetPathName()));
			}

	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			UHLODLayer* Linked = Layer->GetLinkedLayer();
			if (Linked)
			{
				Entry["linked_layer"] = std::string(TCHAR_TO_UTF8(*NeoLuaAsset::ToPackagePath(Linked->GetPathName())));
				Entry["linked_layer_object_path"] = std::string(TCHAR_TO_UTF8(*Linked->GetPathName()));
			}
#endif

			const TSubclassOf<UHLODBuilder> BuilderClass = Layer->GetHLODBuilderClass();
			if (BuilderClass)
			{
				Entry["builder_class"] = std::string(TCHAR_TO_UTF8(*BuilderClass->GetPathName()));
			}

			const TSubclassOf<AWorldPartitionHLOD> ActorClass = Layer->GetHLODActorClass();
			if (ActorClass && ActorClass != AWorldPartitionHLOD::StaticClass())
			{
				Entry["hlod_actor_class"] = std::string(TCHAR_TO_UTF8(*ActorClass->GetPathName()));
			}

			const TSubclassOf<UWorldPartitionHLODModifier> ModifierClass = Layer->GetHLODModifierClass();
			if (ModifierClass)
			{
				Entry["hlod_modifier_class"] = std::string(TCHAR_TO_UTF8(*ModifierClass->GetPathName()));
			}

			// cell_size/loading_range/is_spatially_loaded — deprecated in 5.7 but still accessible via reflection
			FProperty* CellSizeProp = Layer->GetClass()->FindPropertyByName(TEXT("CellSize"));
			if (CellSizeProp)
			{
				int32 CellSize = 0;
				CellSizeProp->GetValue_InContainer(Layer, &CellSize);
				Entry["cell_size"] = CellSize;
			}

			FProperty* LoadingRangeProp = Layer->GetClass()->FindPropertyByName(TEXT("LoadingRange"));
			if (LoadingRangeProp)
			{
				double LR = 0.0;
				LoadingRangeProp->GetValue_InContainer(Layer, &LR);
				Entry["loading_range"] = LR;
			}

			// bIsSpatiallyLoaded is a bitfield (uint32:1) — must use FBoolProperty typed accessor
			FBoolProperty* SpatialProp = CastField<FBoolProperty>(Layer->GetClass()->FindPropertyByName(TEXT("bIsSpatiallyLoaded")));
			if (SpatialProp)
			{
				Entry["is_spatially_loaded"] = SpatialProp->GetPropertyValue_InContainer(Layer);
			}

			Result[Idx++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] hlod_get_layers -> %d HLOD layers"), Idx - 1));
		return sol::make_object(Lua, Result);
	});

	// ---- hlod_configure_layer(path, options) ----
	Lua.set_function("hlod_configure_layer", [&Session](const std::string& Path, sol::table Options, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		FString FPath = NeoLuaStr::ToFString(Path);

		UHLODLayer* Layer = LoadHLODLayerByPath(FPath);
		if (!Layer)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] hlod_configure_layer -> could not load HLODLayer at '%s'. Use hlod_get_layers() to list valid paths or pass a full /Game/... path."), *FPath));
			return sol::lua_nil;
		}

		FString Error;
		if (!ApplyHLODLayerOptions(Layer, Options, Error))
		{
			Session.Log(FString::Printf(TEXT("[FAIL] hlod_configure_layer -> %s"), *Error));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] hlod_configure_layer -> configured '%s'"), *Layer->GetName()));
		return sol::make_object(Lua, true);
	});

	// ---- hlod_get_source_actors(actor_label) ----
	Lua.set_function("hlod_get_source_actors", [&Session](const std::string& ActorLabel, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Session.Log(TEXT("[FAIL] hlod_get_source_actors -> no editor world. Open a level in the editor and retry."));
			return sol::lua_nil;
		}

		FString FilterLabel = NeoLuaStr::ToFString(ActorLabel);

		AWorldPartitionHLOD* FoundActor = FindHLODActorByLabel(World, FilterLabel);
		if (!FoundActor)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] hlod_get_source_actors -> no HLOD actor with label '%s'. Run hlod_list_actors() and retry with an exact label."), *FilterLabel));
			return sol::lua_nil;
		}

		UWorldPartitionHLODSourceActors* SourceActors = FoundActor->GetSourceActors();
		if (!SourceActors)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] hlod_get_source_actors -> HLOD actor '%s' has no source actors data. The actor may not be built — try hlod_build('%s')."), *FilterLabel, *FilterLabel));
			return sol::lua_nil;
		}

		sol::table Result = Lua.create_table();

		const UHLODLayer* HLODLayer = SourceActors->GetHLODLayer();
		if (HLODLayer)
		{
			Result["hlod_layer"] = std::string(TCHAR_TO_UTF8(*HLODLayer->GetPathName()));
		}

		if (UWorldPartitionHLODSourceActorsFromCell* CellSource = Cast<UWorldPartitionHLODSourceActorsFromCell>(SourceActors))
		{
			Result["source_type"] = std::string("cell");
			const TArray<FWorldPartitionRuntimeCellObjectMapping>& Actors = CellSource->GetActors();
			sol::table ActorsTable = Lua.create_table();
			int32 Idx = 1;
			for (const FWorldPartitionRuntimeCellObjectMapping& Mapping : Actors)
			{
				sol::table Entry = Lua.create_table();
				Entry["path"] = std::string(TCHAR_TO_UTF8(*Mapping.Path.ToString()));
				Entry["package"] = std::string(TCHAR_TO_UTF8(*Mapping.Package.ToString()));
				Entry["base_class"] = std::string(TCHAR_TO_UTF8(*Mapping.BaseClass.ToString()));
				Entry["native_class"] = std::string(TCHAR_TO_UTF8(*Mapping.NativeClass.ToString()));
				ActorsTable[Idx++] = Entry;
			}
			Result["actors"] = ActorsTable;
			Result["actor_count"] = Actors.Num();

			Session.Log(FString::Printf(TEXT("[OK] hlod_get_source_actors -> '%s' has %d cell-based source actors"), *FilterLabel, Actors.Num()));
		}
		else if (UWorldPartitionHLODSourceActorsFromLevel* LevelSource = Cast<UWorldPartitionHLODSourceActorsFromLevel>(SourceActors))
		{
			Result["source_type"] = std::string("level");
			const TSoftObjectPtr<UWorld>& SourceLevel = LevelSource->GetSourceLevel();
			Result["source_level"] = std::string(TCHAR_TO_UTF8(*SourceLevel.ToString()));

			Session.Log(FString::Printf(TEXT("[OK] hlod_get_source_actors -> '%s' has level-based source: %s"), *FilterLabel, *SourceLevel.ToString()));
		}
		else
		{
			Result["source_type"] = std::string(TCHAR_TO_UTF8(*SourceActors->GetClass()->GetName()));
			Session.Log(FString::Printf(TEXT("[OK] hlod_get_source_actors -> '%s' has %s source actors"), *FilterLabel, *SourceActors->GetClass()->GetName()));
		}

		return sol::make_object(Lua, Result);
	});

	// ---- hlod_get_stats(actor_label) ----
	Lua.set_function("hlod_get_stats", [&Session](const std::string& ActorLabel, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Session.Log(TEXT("[FAIL] hlod_get_stats -> no editor world. Open a level in the editor and retry."));
			return sol::lua_nil;
		}

		FString FilterLabel = NeoLuaStr::ToFString(ActorLabel);
		AWorldPartitionHLOD* FoundActor = FindHLODActorByLabel(World, FilterLabel);
		if (!FoundActor)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] hlod_get_stats -> no HLOD actor with label '%s'. Run hlod_list_actors() and retry with an exact label."), *FilterLabel));
			return sol::lua_nil;
		}

		sol::table Result = Lua.create_table();
		Result["label"] = std::string(TCHAR_TO_UTF8(*FoundActor->GetActorLabel()));
		Result["lod_level"] = (int)FoundActor->GetLODLevel();
		Result["hash"] = (double)FoundActor->GetHLODHash();
		Result["min_visible_distance"] = FoundActor->GetMinVisibleDistance();

		// Group stats by FWorldPartitionHLODStats category. Drop zero-valued entries to keep
		// the report compact, but include the empty group sub-tables so callers can rely on
		// `result.mesh`, `result.material`, etc. always existing.
		sol::table Groups = Lua.create_table();
		Groups["input"]    = Lua.create_table();
		Groups["mesh"]     = Lua.create_table();
		Groups["material"] = Lua.create_table();
		Groups["memory"]   = Lua.create_table();
		Groups["build"]    = Lua.create_table();

		int32 NonZeroCount = 0;
		for (int32 i = 0; i < NumHLODStatEntries; ++i)
		{
			const FHLODStatEntry& Entry = GetHLODStatEntries(i);
			int64 Value = FoundActor->GetStat(Entry.Name);
			if (Value != 0)
			{
				sol::table Group = Groups[std::string(TCHAR_TO_UTF8(Entry.Group))];
				Group[std::string(TCHAR_TO_UTF8(Entry.Key))] = (double)Value;
				NonZeroCount++;
			}
		}
		Result["stats"] = Groups;

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
		// Input stats — referenced assets per builder
		const FHLODBuildInputStats& InputStats = FoundActor->GetInputStats();
		if (InputStats.BuildersReferencedAssets.Num() > 0)
		{
			sol::table InputTable = Lua.create_table();
			for (const auto& Pair : InputStats.BuildersReferencedAssets)
			{
				sol::table BuilderEntry = Lua.create_table();
				sol::table MeshesTable = Lua.create_table();
				int32 MeshIdx = 1;
				for (const auto& MeshPair : Pair.Value.StaticMeshes)
				{
					sol::table MeshEntry = Lua.create_table();
					MeshEntry["asset"] = std::string(TCHAR_TO_UTF8(*MeshPair.Key.ToString()));
					MeshEntry["count"] = (int)MeshPair.Value;
					MeshesTable[MeshIdx++] = MeshEntry;
				}
				BuilderEntry["static_meshes"] = MeshesTable;
				InputTable[std::string(TCHAR_TO_UTF8(*Pair.Key.ToString()))] = BuilderEntry;
			}
			Result["input_stats"] = InputTable;
		}
#endif

		Session.Log(FString::Printf(TEXT("[OK] hlod_get_stats -> '%s' has %d non-zero stats"), *FilterLabel, NonZeroCount));
		return sol::make_object(Lua, Result);
	});

	// ---- hlod_set_actor_field(actor_label, field, value) ----
	// Covers the AWorldPartitionHLOD setters that open_asset cannot reach: HLODActor::CanEditChange
	// returns false (HLODActor.h:149) so generic UPROPERTY writes are refused.
	Lua.set_function("hlod_set_actor_field", [&Session](const std::string& ActorLabel, const std::string& Field, sol::object Value, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Session.Log(TEXT("[FAIL] hlod_set_actor_field -> no editor world. Open a level in the editor and retry."));
			return sol::lua_nil;
		}

		FString FilterLabel = NeoLuaStr::ToFString(ActorLabel);
		AWorldPartitionHLOD* FoundActor = FindHLODActorByLabel(World, FilterLabel);
		if (!FoundActor)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] hlod_set_actor_field -> no HLOD actor with label '%s'. Run hlod_list_actors() and retry with an exact label."), *FilterLabel));
			return sol::lua_nil;
		}

		FString FField = NeoLuaStr::ToFString(Field);

		if (FField.Equals(TEXT("require_warmup"), ESearchCase::IgnoreCase))
		{
			if (!Value.is<bool>())
			{
				Session.Log(TEXT("[FAIL] hlod_set_actor_field('require_warmup') -> value must be bool. Pass true or false."));
				return sol::lua_nil;
			}
			FoundActor->Modify();
			FoundActor->SetRequireWarmup(Value.as<bool>());
		}
		else if (FField.Equals(TEXT("min_visible_distance"), ESearchCase::IgnoreCase))
		{
			if (!Value.is<double>())
			{
				Session.Log(TEXT("[FAIL] hlod_set_actor_field('min_visible_distance') -> value must be number (centimeters)."));
				return sol::lua_nil;
			}
			FoundActor->Modify();
			FoundActor->SetMinVisibleDistance(Value.as<double>());
		}
		else if (FField.Equals(TEXT("lod_level"), ESearchCase::IgnoreCase))
		{
			if (!Value.is<double>())
			{
				Session.Log(TEXT("[FAIL] hlod_set_actor_field('lod_level') -> value must be a non-negative integer."));
				return sol::lua_nil;
			}
			const double LODLevelValue = Value.as<double>();
			if (!FMath::IsFinite(LODLevelValue)
				|| FMath::FloorToDouble(LODLevelValue) != LODLevelValue
				|| LODLevelValue < 0.0
				|| LODLevelValue > static_cast<double>(TNumericLimits<uint32>::Max()))
			{
				Session.Log(TEXT("[FAIL] hlod_set_actor_field('lod_level') -> value must be a finite integer in the uint32 range."));
				return sol::lua_nil;
			}
			FoundActor->Modify();
			FoundActor->SetLODLevel(static_cast<uint32>(LODLevelValue));
		}
		else if (FField.Equals(TEXT("is_standalone"), ESearchCase::IgnoreCase))
		{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			if (!Value.is<bool>())
			{
				Session.Log(TEXT("[FAIL] hlod_set_actor_field('is_standalone') -> value must be bool. Pass true or false."));
				return sol::lua_nil;
			}
			FoundActor->Modify();
			FoundActor->SetIsStandalone(Value.as<bool>());
#else
			Session.Log(TEXT("[FAIL] hlod_set_actor_field('is_standalone') requires UE 5.7+"));
			return sol::lua_nil;
#endif
		}
		else if (FField.Equals(TEXT("hlod_bounds"), ESearchCase::IgnoreCase))
		{
			if (!Value.is<sol::table>())
			{
				Session.Log(TEXT("[FAIL] hlod_set_actor_field('hlod_bounds') -> value must be a table {min_x,min_y,min_z,max_x,max_y,max_z}."));
				return sol::lua_nil;
			}
			sol::table BoundsTable = Value.as<sol::table>();
			FBox NewBounds(
				FVector(
					BoundsTable.get_or("min_x", 0.0),
					BoundsTable.get_or("min_y", 0.0),
					BoundsTable.get_or("min_z", 0.0)),
				FVector(
					BoundsTable.get_or("max_x", 0.0),
					BoundsTable.get_or("max_y", 0.0),
					BoundsTable.get_or("max_z", 0.0)));
			FoundActor->Modify();
			FoundActor->SetHLODBounds(NewBounds);
		}
		else if (FField.Equals(TEXT("hlod_hash"), ESearchCase::IgnoreCase))
		{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			// SetHLODHash takes hash + report content. Accept either {hash=N, report=""} or a bare number.
			uint32 NewHash = 0;
			FString Report;
			if (Value.is<sol::table>())
			{
				sol::table HashTable = Value.as<sol::table>();
				NewHash = static_cast<uint32>(HashTable.get_or("hash", 0.0));
				Report = NeoLuaStr::ToFString(HashTable.get_or<std::string>("report", ""));
			}
			else if (Value.is<double>())
			{
				NewHash = static_cast<uint32>(Value.as<double>());
			}
			else
			{
				Session.Log(TEXT("[FAIL] hlod_set_actor_field('hlod_hash') -> pass a number or a table {hash=N, report=\"...\"}."));
				return sol::lua_nil;
			}
			FoundActor->Modify();
			FoundActor->SetHLODHash(NewHash, Report);
#else
			Session.Log(TEXT("[FAIL] hlod_set_actor_field('hlod_hash') requires UE 5.7+"));
			return sol::lua_nil;
#endif
		}
		else if (FField.Equals(TEXT("source_cell_guid"), ESearchCase::IgnoreCase))
		{
			if (!Value.is<std::string>())
			{
				Session.Log(TEXT("[FAIL] hlod_set_actor_field('source_cell_guid') -> value must be a GUID string."));
				return sol::lua_nil;
			}
			FGuid NewGuid;
			if (!FGuid::Parse(NeoLuaStr::ToFString(Value.as<std::string>()), NewGuid))
			{
				Session.Log(TEXT("[FAIL] hlod_set_actor_field('source_cell_guid') -> value is not a valid GUID. Use the form 00000000-0000-0000-0000-000000000000."));
				return sol::lua_nil;
			}
			FoundActor->Modify();
			FoundActor->SetSourceCellGuid(NewGuid);
		}
		else if (FField.Equals(TEXT("set_stat"), ESearchCase::IgnoreCase))
		{
			if (!Value.is<sol::table>())
			{
				Session.Log(TEXT("[FAIL] hlod_set_actor_field('set_stat') -> value must be {name=\"...\", value=N}."));
				return sol::lua_nil;
			}
			sol::table StatTable = Value.as<sol::table>();
			FString StatName = NeoLuaStr::ToFString(StatTable.get_or<std::string>("name", ""));
			if (StatName.IsEmpty())
			{
				Session.Log(TEXT("[FAIL] hlod_set_actor_field('set_stat') -> 'name' is required (e.g. \"MeshTriangleCount\")."));
				return sol::lua_nil;
			}
			int64 StatValue = static_cast<int64>(StatTable.get_or("value", 0.0));
			FoundActor->Modify();
			FoundActor->SetStat(FName(*StatName), StatValue);
		}
		else if (FField.Equals(TEXT("reset_stats"), ESearchCase::IgnoreCase))
		{
			FoundActor->Modify();
			FoundActor->ResetStats();
		}
		else
		{
			Session.Log(FString::Printf(TEXT("[FAIL] hlod_set_actor_field -> unknown field '%s'. Valid: require_warmup, hlod_bounds, min_visible_distance, hlod_hash, lod_level, source_cell_guid, is_standalone, set_stat, reset_stats."), *FField));
			return sol::lua_nil;
		}

		FoundActor->MarkPackageDirty();
		Session.Log(FString::Printf(TEXT("[OK] hlod_set_actor_field('%s', '%s')"), *FilterLabel, *FField));
		return sol::make_object(Lua, true);
	});

	// ---- hlod_export(actor_label, options) ----
	Lua.set_function("hlod_export", [&Session](const std::string& ActorLabel, sol::optional<sol::table> Options, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Session.Log(TEXT("[FAIL] hlod_export -> no editor world. Open a level in the editor and retry."));
			return sol::lua_nil;
		}

		FString FilterLabel = NeoLuaStr::ToFString(ActorLabel);
		AWorldPartitionHLOD* FoundActor = FindHLODActorByLabel(World, FilterLabel);
		if (!FoundActor)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] hlod_export -> no HLOD actor with label '%s'. Run hlod_list_actors() and retry with an exact label."), *FilterLabel));
			return sol::lua_nil;
		}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
		FExportHLODAssetsParams Params;
		Params.MeshOrigin = EExportHLODMeshOrigin::World;

		if (Options.has_value())
		{
			auto OriginOpt = Options.value().get<sol::optional<std::string>>("mesh_origin");
			if (OriginOpt.has_value())
			{
				FString OriginStr = NeoLuaStr::ToFStringOpt(OriginOpt);
				if (OriginStr.Equals(TEXT("Actor"), ESearchCase::IgnoreCase))
					Params.MeshOrigin = EExportHLODMeshOrigin::Actor;
			}

			auto PathOpt = Options.value().get<sol::optional<std::string>>("export_path");
			if (PathOpt.has_value())
			{
				Params.ExportRootPath.Path = NeoLuaStr::ToFStringOpt(PathOpt);
			}

			auto TestOnlyOpt = Options.value().get<sol::optional<bool>>("test_export_only");
			if (TestOnlyOpt.has_value())
			{
				Params.bTestExportOnly = TestOnlyOpt.value();
			}
		}

		if (Params.ExportRootPath.Path.IsEmpty())
		{
			Session.Log(TEXT("[FAIL] hlod_export -> export_path is required. Pass options={export_path='/Game/HLOD/Exported'}."));
			return sol::lua_nil;
		}

		FString ErrorMessage;
		TArray<UObject*> ExportedAssets = FoundActor->ExportHLODAssets(Params, ErrorMessage);

		if (!ErrorMessage.IsEmpty())
		{
			Session.Log(FString::Printf(TEXT("[FAIL] hlod_export -> %s. Use export_path='/Game/...' and ensure the destination assets do not already exist (or pass test_export_only=true to dry-run)."), *ErrorMessage));
			return sol::lua_nil;
		}

		sol::table Result = Lua.create_table();
		sol::table AssetsTable = Lua.create_table();
		int32 Idx = 1;
		for (UObject* Asset : ExportedAssets)
		{
			if (Asset)
			{
				AssetsTable[Idx++] = std::string(TCHAR_TO_UTF8(*Asset->GetPathName()));
			}
		}
		Result["assets"] = AssetsTable;
		Result["count"] = ExportedAssets.Num();
		Result["test_export_only"] = Params.bTestExportOnly;

		Session.Log(FString::Printf(TEXT("[OK] hlod_export -> %s %d asset(s) from '%s'"), Params.bTestExportOnly ? TEXT("validated") : TEXT("exported"), ExportedAssets.Num(), *FilterLabel));
		return sol::make_object(Lua, Result);
#else
		Session.Log(TEXT("[FAIL] hlod_export requires UE 5.7+"));
		return sol::lua_nil;
#endif
	});

	// ---- hlod_write_stats(filename, stats_type?) ----
	// Wraps UWorldPartitionHLODEditorSubsystem::WriteHLODStats — the editor subsystem path that
	// persists per-world HLOD stats to a CSV (default) or input-details report (input_details).
	Lua.set_function("hlod_write_stats", [&Session](const std::string& Filename, sol::optional<std::string> StatsType, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Session.Log(TEXT("[FAIL] hlod_write_stats -> no editor world. Open a level in the editor and retry."));
			return sol::lua_nil;
		}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
		// UWorldPartitionHLODEditorSubsystem became a UEditorSubsystem in UE 5.8
		// (was a UWorldSubsystem); fetch it from GEditor rather than the world.
		UWorldPartitionHLODEditorSubsystem* HLODSubsystem = GEditor ? GEditor->GetEditorSubsystem<UWorldPartitionHLODEditorSubsystem>() : nullptr;
#else
		UWorldPartitionHLODEditorSubsystem* HLODSubsystem = World->GetSubsystem<UWorldPartitionHLODEditorSubsystem>();
#endif
		if (!HLODSubsystem)
		{
			Session.Log(TEXT("[FAIL] hlod_write_stats -> UWorldPartitionHLODEditorSubsystem unavailable. Open a World Partition level (the subsystem only initializes for WP worlds)."));
			return sol::lua_nil;
		}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
		IWorldPartitionEditorModule::FWriteHLODStatsParams Params;
		Params.World = World;
		Params.Filename = NeoLuaStr::ToFString(Filename);
		Params.StatsType = IWorldPartitionEditorModule::FWriteHLODStatsParams::EStatsType::Default;

		FString TypeStr = NeoLuaStr::ToFStringOpt(StatsType);
		if (!TypeStr.IsEmpty())
		{
			if (TypeStr.Equals(TEXT("input_details"), ESearchCase::IgnoreCase) ||
				TypeStr.Equals(TEXT("input"), ESearchCase::IgnoreCase))
			{
				Params.StatsType = IWorldPartitionEditorModule::FWriteHLODStatsParams::EStatsType::InputDetails;
			}
			else if (!TypeStr.Equals(TEXT("default"), ESearchCase::IgnoreCase))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] hlod_write_stats -> unknown stats_type '%s'. Use 'default' or 'input_details'."), *TypeStr));
				return sol::lua_nil;
			}
		}

		const bool bOk = HLODSubsystem->WriteHLODStats(Params);
		if (!bOk)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] hlod_write_stats -> WriteHLODStats returned false (filename '%s'). Check that the destination directory is writable."), *Params.Filename));
			return sol::lua_nil;
		}

		Session.Log(FString::Printf(TEXT("[OK] hlod_write_stats -> wrote %s stats to '%s'"),
			Params.StatsType == IWorldPartitionEditorModule::FWriteHLODStatsParams::EStatsType::InputDetails ? TEXT("input_details") : TEXT("default"),
			*Params.Filename));
		return sol::make_object(Lua, true);
#else
		Session.Log(TEXT("[FAIL] hlod_write_stats requires UE 5.7+"));
		return sol::lua_nil;
#endif
	});

	// ---- hlod_legacy_build(force?) ----
	Lua.set_function("hlod_legacy_build", [&Session](sol::optional<bool> Force, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Session.Log(TEXT("[FAIL] hlod_legacy_build -> no editor world. Open a level in the editor and retry."));
			return sol::lua_nil;
		}

		FHierarchicalLODBuilder Builder(World);

		bool bForce = Force.value_or(false);
		if (!bForce && !Builder.NeedsBuild())
		{
			Session.Log(TEXT("[OK] hlod_legacy_build -> already up to date (use force=true to rebuild)"));
			return sol::make_object(Lua, true);
		}

		Builder.Build();
		Session.Log(TEXT("[OK] hlod_legacy_build -> build complete"));
		return sol::make_object(Lua, true);
	});

	// ---- hlod_legacy_clear() ----
	Lua.set_function("hlod_legacy_clear", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Session.Log(TEXT("[FAIL] hlod_legacy_clear -> no editor world. Open a level in the editor and retry."));
			return sol::lua_nil;
		}

		FHierarchicalLODBuilder Builder(World);
		Builder.ClearHLODs();

		Session.Log(TEXT("[OK] hlod_legacy_clear -> cleared all legacy HLODs"));
		return sol::make_object(Lua, true);
	});

	// ---- hlod_legacy_needs_build(force?) ----
	Lua.set_function("hlod_legacy_needs_build", [&Session](sol::optional<bool> Force, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Session.Log(TEXT("[FAIL] hlod_legacy_needs_build -> no editor world. Open a level in the editor and retry."));
			return sol::lua_nil;
		}

		FHierarchicalLODBuilder Builder(World);
		bool bNeedsBuild = Builder.NeedsBuild(Force.value_or(false));

		Session.Log(FString::Printf(TEXT("[OK] hlod_legacy_needs_build -> %s"), bNeedsBuild ? TEXT("true") : TEXT("false")));
		return sol::make_object(Lua, bNeedsBuild);
	});

	// ---- hlod_legacy_preview_build() ----
	Lua.set_function("hlod_legacy_preview_build", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Session.Log(TEXT("[FAIL] hlod_legacy_preview_build -> no editor world. Open a level in the editor and retry."));
			return sol::lua_nil;
		}

		FHierarchicalLODBuilder Builder(World);
		Builder.PreviewBuild();
		Session.Log(TEXT("[OK] hlod_legacy_preview_build -> preview build complete"));
		return sol::make_object(Lua, true);
	});

	// ---- hlod_legacy_clear_preview() ----
	Lua.set_function("hlod_legacy_clear_preview", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Session.Log(TEXT("[FAIL] hlod_legacy_clear_preview -> no editor world. Open a level in the editor and retry."));
			return sol::lua_nil;
		}

		FHierarchicalLODBuilder Builder(World);
		Builder.ClearPreviewBuild();
		Session.Log(TEXT("[OK] hlod_legacy_clear_preview -> cleared preview LODActors"));
		return sol::make_object(Lua, true);
	});

	// ---- hlod_legacy_build_meshes(force?) ----
	Lua.set_function("hlod_legacy_build_meshes", [&Session](sol::optional<bool> Force, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Session.Log(TEXT("[FAIL] hlod_legacy_build_meshes -> no editor world. Open a level in the editor and retry."));
			return sol::lua_nil;
		}

		FHierarchicalLODBuilder Builder(World);
		Builder.BuildMeshesForLODActors(Force.value_or(false));
		Session.Log(TEXT("[OK] hlod_legacy_build_meshes -> built meshes for all LODActors"));
		return sol::make_object(Lua, true);
	});

	// ---- hlod_legacy_save_meshes() ----
	Lua.set_function("hlod_legacy_save_meshes", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Session.Log(TEXT("[FAIL] hlod_legacy_save_meshes -> no editor world. Open a level in the editor and retry."));
			return sol::lua_nil;
		}

		FHierarchicalLODBuilder Builder(World);
		Builder.SaveMeshesForActors();
		Session.Log(TEXT("[OK] hlod_legacy_save_meshes -> saved HLOD meshes"));
		return sol::make_object(Lua, true);
	});

	// ---- hlod_legacy_delete_empty_packages(level_name?) ----
	Lua.set_function("hlod_legacy_delete_empty_packages", [&Session](sol::optional<std::string> LevelName, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Session.Log(TEXT("[FAIL] hlod_legacy_delete_empty_packages -> no editor world. Open a level in the editor and retry."));
			return sol::lua_nil;
		}

		ULevel* TargetLevel = World->PersistentLevel;
		FString FLevelName = NeoLuaStr::ToFStringOpt(LevelName);
		if (!FLevelName.IsEmpty())
		{
			TargetLevel = nullptr;
			for (ULevel* Level : World->GetLevels())
			{
				if (Level && Level->GetOutermost()->GetName().EndsWith(FLevelName))
				{
					TargetLevel = Level;
					break;
				}
			}
			if (!TargetLevel)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] hlod_legacy_delete_empty_packages -> no level matching '%s'. Pass a streaming level name or omit for the persistent level."), *FLevelName));
				return sol::lua_nil;
			}
		}

		FHierarchicalLODBuilder Builder(World);
		Builder.DeleteEmptyHLODPackages(TargetLevel);
		Session.Log(FString::Printf(TEXT("[OK] hlod_legacy_delete_empty_packages -> processed level '%s'"), *TargetLevel->GetOutermost()->GetName()));
		return sol::make_object(Lua, true);
	});

	// ---- hlod_legacy_build_lod_actor(actor_label, lod_level) ----
	Lua.set_function("hlod_legacy_build_lod_actor", [&Session](const std::string& ActorLabel, int LODLevel, sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Session.Log(TEXT("[FAIL] hlod_legacy_build_lod_actor -> no editor world. Open a level in the editor and retry."));
			return sol::lua_nil;
		}

		FString FilterLabel = NeoLuaStr::ToFString(ActorLabel);
		ALODActor* LODActor = FindLegacyLODActorByLabel(World, FilterLabel);
		if (!LODActor)
		{
			Session.Log(FString::Printf(TEXT("[FAIL] hlod_legacy_build_lod_actor -> no ALODActor with label '%s'. Run hlod_list_lod_actors() and retry with an exact label."), *FilterLabel));
			return sol::lua_nil;
		}

		if (LODLevel < 0)
		{
			Session.Log(TEXT("[FAIL] hlod_legacy_build_lod_actor -> lod_level must be >= 0."));
			return sol::lua_nil;
		}

		FHierarchicalLODBuilder Builder(World);
		Builder.BuildMeshForLODActor(LODActor, static_cast<uint32>(LODLevel));
		Session.Log(FString::Printf(TEXT("[OK] hlod_legacy_build_lod_actor -> built '%s' at LOD %d"), *FilterLabel, LODLevel));
		return sol::make_object(Lua, true);
	});

	// ---- hlod_list_lod_actors() ----
	Lua.set_function("hlod_list_lod_actors", [&Session](sol::this_state S) -> sol::object
	{
		sol::state_view Lua(S);
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Session.Log(TEXT("[FAIL] hlod_list_lod_actors -> no editor world. Open a level in the editor and retry."));
			return sol::lua_nil;
		}

		sol::table Result = Lua.create_table();
		int32 Idx = 1;

		for (TActorIterator<ALODActor> It(World); It; ++It)
		{
			ALODActor* LODActor = *It;
			if (!LODActor) continue;

			sol::table Entry = Lua.create_table();
			Entry["label"] = std::string(TCHAR_TO_UTF8(*LODActor->GetActorLabel()));
			Entry["name"] = std::string(TCHAR_TO_UTF8(*LODActor->GetName()));
			Entry["lod_level"] = (int)LODActor->LODLevel;
			Entry["sub_actor_count"] = LODActor->SubActors.Num();
			Entry["is_dirty"] = !LODActor->IsBuilt();
			Entry["draw_distance"] = (double)LODActor->GetDrawDistance();
			Entry["triangles_in_sub_actors"] = (double)LODActor->GetNumTrianglesInSubActors();
			Entry["triangles_in_merged_mesh"] = (double)LODActor->GetNumTrianglesInMergedMesh();

			Result[Idx++] = Entry;
		}

		Session.Log(FString::Printf(TEXT("[OK] hlod_list_lod_actors -> %d legacy LOD actors"), Idx - 1));
		return sol::make_object(Lua, Result);
	});
}

REGISTER_LUA_BINDING(HLOD, HLODDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindHLOD(Lua, Session);
});
