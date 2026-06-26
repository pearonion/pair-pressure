// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Lua/LuaPropertyHelper.h"
#include "Lua/LuaPropertyTable.h"
#include "Lua/LuaAssetResolver.h"
#include "Lua/LuaStr.h"
#include "Tools/NeoStackToolUtils.h"
#include "ScopedTransaction.h"

#include "FoliageType.h"
#include "FoliageType_InstancedStaticMesh.h"
#include "FoliageType_Actor.h"
#include "LandscapeGrassType.h"
#include "Engine/StaticMesh.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// HELPERS
// ============================================================================

static const char* ScalingToString(EFoliageScaling S)
{
	switch (S)
	{
	case EFoliageScaling::Uniform: return "uniform";
	case EFoliageScaling::Free:    return "free";
	case EFoliageScaling::LockXY:  return "lock_xy";
	case EFoliageScaling::LockXZ:  return "lock_xz";
	case EFoliageScaling::LockYZ:  return "lock_yz";
	default:                       return "uniform";
	}
}

static const char* GrassScalingToString(EGrassScaling S)
{
	switch (S)
	{
	case EGrassScaling::Uniform: return "uniform";
	case EGrassScaling::Free:    return "free";
	case EGrassScaling::LockXY:  return "lock_xy";
	default:                     return "uniform";
	}
}

static const char* MobilityToString(EComponentMobility::Type M)
{
	switch (M)
	{
	case EComponentMobility::Static:    return "static";
	case EComponentMobility::Stationary: return "stationary";
	case EComponentMobility::Movable:   return "movable";
	default:                            return "static";
	}
}

// Preserve legacy snake_case enum aliases ("lock_xy", "movable") used by existing Lua scripts.
// UPROPERTY enum names are PascalCase (LockXY, Movable); NeoLuaEnum::ParseRuntime matches
// case-insensitively but "lock_xy" ≠ "LockXY" because the underscore shifts character indices.
// We normalize the value *inside* the Params table before it reaches ApplyTable so the
// downstream FEnumProperty import receives the name it expects.
static void NormalizeScalingKey(sol::table Params, const char* Key)
{
	sol::optional<std::string> Val = Params.get<sol::optional<std::string>>(Key);
	if (!Val.has_value()) return;
	const std::string& S = Val.value();
	if (S == "lock_xy")      Params[Key] = "LockXY";
	else if (S == "lock_xz") Params[Key] = "LockXZ";
	else if (S == "lock_yz") Params[Key] = "LockYZ";
	// "uniform" / "free" already match (case-insensitive).
}

static void NormalizeMobilityKey(sol::table Params, const char* Key)
{
	// "static"/"stationary"/"movable" → case-insensitive match already works.
	// No-op kept for symmetry + future UMETA display names.
	(void)Params; (void)Key;
}

// Generic "warn-if-out-of-range" helper — matches the clamping the old code did
// (engine meta-data ClampMin/ClampMax is only enforced in the editor details panel).
template<typename T>
static void ClampKey(sol::table Params, const char* Key, T Lo, T Hi)
{
	sol::optional<T> Val = Params.get<sol::optional<T>>(Key);
	if (!Val.has_value()) return;
	Params[Key] = FMath::Clamp(Val.value(), Lo, Hi);
}

static bool ApplyFoliageSourceChange(UFoliageType* Foliage, FProperty* SourceProperty, UObject* NewSource)
{
	if (!Foliage || !SourceProperty)
	{
		return false;
	}

	Foliage->PreEditChange(SourceProperty);
	Foliage->SetSource(NewSource);
	FPropertyChangedEvent ChangeEvent(SourceProperty, EPropertyChangeType::ValueSet);
	Foliage->PostEditChangeProperty(ChangeEvent);
	Foliage->MarkPackageDirty();
	return true;
}

// ============================================================================
// UFoliageType ENRICHMENT
// ============================================================================

static TArray<FLuaFunctionDoc> FoliageTypeDocs = {};

static void BindFoliageType(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_foliage_type", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = NeoLuaStr::ToFString(PathStr);
		UFoliageType* Foliage = NeoLuaAsset::Resolve<UFoliageType>(FPath);
		if (!Foliage) return;

		AssetObj["_help_text"] =
			"FoliageType enrichment methods:\n"
			"\n"
			"info() — structured summary of foliage settings (type, mesh, painting,\n"
			"  placement, landscape layers, collision, instance settings, lightmap,\n"
			"  lighting channels).\n"
			"\n"
			"configure(params) — set foliage properties. All editable UPROPERTYs on\n"
			"UFoliageType / UFoliageType_InstancedStaticMesh / UFoliageType_Actor are\n"
			"accepted via reflection (snake_case keys auto-map to PascalCase/b-prefix).\n"
			"Special-cased:\n"
			"  mesh (string path, ISM only)        — triggers SetStaticMesh + UpdateBounds\n"
			"  actor_class (string path, actor)    — triggers UpdateBounds\n"
			"  scaling/mobility (string)           — snake_case enum aliases (lock_xy etc.)\n"
			"  overridden_lightmap_res (int)       — auto-rounded to factor of 4\n"
			"  align_max_angle / random_pitch_angle — clamped to [0, 359]\n"
			"  minimum_layer_weight / minimum_exclusion_layer_weight — clamped to [0, 1]\n"
			"  custom_depth_stencil_value          — clamped to [0, 255]\n"
			"\n"
			"Example keys: density, density_adjustment_factor, radius, scaling,\n"
			"  scale_x={min,max}, scale_y, scale_z, z_offset, align_to_normal,\n"
			"  align_max_angle, random_yaw, random_pitch_angle, average_normal,\n"
			"  ground_slope_angle={min,max}, height={min,max}, landscape_layers[],\n"
			"  exclusion_landscape_layers[], minimum_layer_weight, collision_with_world,\n"
			"  collision_scale={x,y,z}, mobility, cull_distance={min,max}, cast_shadow,\n"
			"  cast_dynamic_shadow, cast_static_shadow, cast_contact_shadow,\n"
			"  cast_shadow_as_two_sided, receives_decals, affect_dynamic_indirect_lighting,\n"
			"  affect_distance_field_lighting, use_as_occluder, enable_density_scaling,\n"
			"  enable_discard_on_load, enable_cull_distance_scaling, visible_in_ray_tracing,\n"
			"  evaluate_world_position_offset, world_position_offset_disable_distance,\n"
			"  override_lightmap_res, overridden_lightmap_res, render_custom_depth,\n"
			"  custom_depth_stencil_value, translucency_sort_priority,\n"
			"  lighting_channels={channel_0,channel_1,channel_2}\n";

		// ================================================================
		// info() — curated summary (type-specific top-level + reflection dump
		// of the remaining fields would drown the AI, so hand-picked).
		// ================================================================
		AssetObj.set_function("info", [Foliage, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(Foliage))
			{
				Session.Log(TEXT("[FAIL] info -> asset no longer valid"));
				return sol::lua_nil;
			}

			sol::table R = Lua.create_table();

			if (UFoliageType_InstancedStaticMesh* ISM = Cast<UFoliageType_InstancedStaticMesh>(Foliage))
			{
				R["type"] = "instanced_static_mesh";
				if (ISM->Mesh)
					R["mesh"] = std::string(TCHAR_TO_UTF8(*ISM->Mesh->GetPathName()));

				if (ISM->OverrideMaterials.Num() > 0)
				{
					sol::table Mats = Lua.create_table();
					for (int32 i = 0; i < ISM->OverrideMaterials.Num(); i++)
						if (ISM->OverrideMaterials[i])
							Mats[i + 1] = std::string(TCHAR_TO_UTF8(*ISM->OverrideMaterials[i]->GetPathName()));
					R["override_materials"] = Mats;
				}

				if (ISM->NaniteOverrideMaterials.Num() > 0)
				{
					sol::table Mats = Lua.create_table();
					for (int32 i = 0; i < ISM->NaniteOverrideMaterials.Num(); i++)
						if (ISM->NaniteOverrideMaterials[i])
							Mats[i + 1] = std::string(TCHAR_TO_UTF8(*ISM->NaniteOverrideMaterials[i]->GetPathName()));
					R["nanite_override_materials"] = Mats;
				}
			}
			else if (UFoliageType_Actor* ActorFoliage = Cast<UFoliageType_Actor>(Foliage))
			{
				R["type"] = "actor";
				if (ActorFoliage->ActorClass)
					R["actor_class"] = std::string(TCHAR_TO_UTF8(*ActorFoliage->ActorClass->GetPathName()));
				R["attach_to_base_component"] = ActorFoliage->bShouldAttachToBaseComponent;
				R["static_mesh_only"] = ActorFoliage->bStaticMeshOnly;
			}
			else
			{
				R["type"] = "unknown";
			}

			auto Interval = [&Lua](const FFloatInterval& I) { sol::table T = Lua.create_table(); T["min"] = I.Min; T["max"] = I.Max; return T; };
			auto IntInterval = [&Lua](const FInt32Interval& I) { sol::table T = Lua.create_table(); T["min"] = I.Min; T["max"] = I.Max; return T; };

			R["density"] = Foliage->Density;
			R["density_adjustment_factor"] = Foliage->DensityAdjustmentFactor;
			R["radius"] = Foliage->Radius;
			R["scaling"] = ScalingToString(Foliage->Scaling);
			R["scale_x"] = Interval(Foliage->ScaleX);
			R["scale_y"] = Interval(Foliage->ScaleY);
			R["scale_z"] = Interval(Foliage->ScaleZ);

			R["z_offset"] = Interval(Foliage->ZOffset);
			R["align_to_normal"] = static_cast<bool>(Foliage->AlignToNormal);
			R["average_normal"] = static_cast<bool>(Foliage->AverageNormal);
			R["align_max_angle"] = Foliage->AlignMaxAngle;
			R["random_yaw"] = static_cast<bool>(Foliage->RandomYaw);
			R["random_pitch_angle"] = Foliage->RandomPitchAngle;
			R["ground_slope_angle"] = Interval(Foliage->GroundSlopeAngle);
			R["height"] = Interval(Foliage->Height);
			R["collision_with_world"] = static_cast<bool>(Foliage->CollisionWithWorld);
			{
				sol::table CS = Lua.create_table();
				CS["x"] = Foliage->CollisionScale.X;
				CS["y"] = Foliage->CollisionScale.Y;
				CS["z"] = Foliage->CollisionScale.Z;
				R["collision_scale"] = CS;
			}

			if (Foliage->LandscapeLayers.Num() > 0)
			{
				sol::table Layers = Lua.create_table();
				for (int32 i = 0; i < Foliage->LandscapeLayers.Num(); i++)
					Layers[i + 1] = std::string(TCHAR_TO_UTF8(*Foliage->LandscapeLayers[i].ToString()));
				R["landscape_layers"] = Layers;
			}
			if (Foliage->ExclusionLandscapeLayers.Num() > 0)
			{
				sol::table Layers = Lua.create_table();
				for (int32 i = 0; i < Foliage->ExclusionLandscapeLayers.Num(); i++)
					Layers[i + 1] = std::string(TCHAR_TO_UTF8(*Foliage->ExclusionLandscapeLayers[i].ToString()));
				R["exclusion_landscape_layers"] = Layers;
			}

			R["mobility"] = MobilityToString(Foliage->Mobility.GetValue());
			R["cull_distance"] = IntInterval(Foliage->CullDistance);
			R["cast_shadow"] = static_cast<bool>(Foliage->CastShadow);
			R["cast_dynamic_shadow"] = static_cast<bool>(Foliage->bCastDynamicShadow);
			R["cast_static_shadow"] = static_cast<bool>(Foliage->bCastStaticShadow);
			R["cast_contact_shadow"] = static_cast<bool>(Foliage->bCastContactShadow);
			R["cast_shadow_as_two_sided"] = static_cast<bool>(Foliage->bCastShadowAsTwoSided);
			R["receives_decals"] = static_cast<bool>(Foliage->bReceivesDecals);
			R["affect_dynamic_indirect_lighting"] = static_cast<bool>(Foliage->bAffectDynamicIndirectLighting);
			R["affect_distance_field_lighting"] = static_cast<bool>(Foliage->bAffectDistanceFieldLighting);
			R["use_as_occluder"] = static_cast<bool>(Foliage->bUseAsOccluder);
			R["render_custom_depth"] = static_cast<bool>(Foliage->bRenderCustomDepth);
			R["custom_depth_stencil_value"] = Foliage->CustomDepthStencilValue;
			R["translucency_sort_priority"] = Foliage->TranslucencySortPriority;

			R["enable_density_scaling"] = static_cast<bool>(Foliage->bEnableDensityScaling);
			R["enable_discard_on_load"] = static_cast<bool>(Foliage->bEnableDiscardOnLoad);
			R["enable_cull_distance_scaling"] = static_cast<bool>(Foliage->bEnableCullDistanceScaling);

			R["visible_in_ray_tracing"] = static_cast<bool>(Foliage->bVisibleInRayTracing);
			R["evaluate_world_position_offset"] = static_cast<bool>(Foliage->bEvaluateWorldPositionOffset);
			R["world_position_offset_disable_distance"] = Foliage->WorldPositionOffsetDisableDistance;

			R["average_normal_single_component"] = static_cast<bool>(Foliage->AverageNormalSingleComponent);
			R["average_normal_sample_count"] = Foliage->AverageNormalSampleCount;
			R["minimum_layer_weight"] = Foliage->MinimumLayerWeight;
			R["minimum_exclusion_layer_weight"] = Foliage->MinimumExclusionLayerWeight;

			R["override_lightmap_res"] = static_cast<bool>(Foliage->bOverrideLightMapRes);
			R["overridden_lightmap_res"] = Foliage->OverriddenLightMapRes;

			{
				sol::table LC = Lua.create_table();
				LC["channel_0"] = static_cast<bool>(Foliage->LightingChannels.bChannel0);
				LC["channel_1"] = static_cast<bool>(Foliage->LightingChannels.bChannel1);
				LC["channel_2"] = static_cast<bool>(Foliage->LightingChannels.bChannel2);
				R["lighting_channels"] = LC;
			}

			Session.Log(FString::Printf(TEXT("[OK] info() -> FoliageType, density=%.1f, radius=%.1f"),
				Foliage->Density, Foliage->Radius));
			return R;
		});

		// ================================================================
		// configure(params) — special cases + reflection walker
		//
		// The old implementation had ~400 lines of sol::optional<T> ladders,
		// one per UFoliageType UPROPERTY. ApplyTable handles every editable
		// field uniformly via PropertyAccessUtil (snake→Pascal + b-prefix
		// variants, bit-fields, FFloatInterval/FInt32Interval/FVector struct
		// handlers, TArray<FName>).
		//
		// Four classes of keys still need explicit handling:
		//   1. `mesh` (ISM)          — asset resolve + SetStaticMesh() triggers UpdateBounds
		//   2. `actor_class` (Actor) — asset resolve as UClass + UpdateBounds
		//   3. snake_case enum values (scaling=lock_xy, …) — normalized to PascalCase
		//   4. clamped fields        — engine ClampMin/ClampMax meta isn't enforced on
		//                              direct writes, so we preserve the old clamps
		// ================================================================
		AssetObj.set_function("configure", [Foliage, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(Foliage))
			{
				Session.Log(TEXT("[FAIL] configure -> asset no longer valid"));
				return sol::lua_nil;
			}

			const FScopedTransaction Transaction(FText::FromString(TEXT("FoliageType: Configure")));
			Foliage->Modify();
			bool bSourceChanged = false;

			// ── 1. Mesh (ISM subtype only) — SetStaticMesh() handles UpdateBounds ──
			sol::optional<std::string> MeshPath = Params.get<sol::optional<std::string>>("mesh");
			if (MeshPath.has_value())
			{
				Params["mesh"] = sol::lua_nil;  // consume so ApplyTable skips it
				if (UFoliageType_InstancedStaticMesh* ISM = Cast<UFoliageType_InstancedStaticMesh>(Foliage))
				{
					FString MPath = NeoLuaStr::ToFStringOpt(MeshPath);
					if (UStaticMesh* NewMesh = NeoLuaAsset::Resolve<UStaticMesh>(MPath))
					{
						FProperty* MeshProperty = NeoLuaProperty::FindPropertyByName(ISM->GetClass(),
							GET_MEMBER_NAME_STRING_CHECKED(UFoliageType_InstancedStaticMesh, Mesh));
						bSourceChanged |= ApplyFoliageSourceChange(Foliage, MeshProperty, NewMesh);
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure: mesh '%s' not found"), *MPath));
					}
				}
			}

			// ── 2. Actor class (Actor subtype only) + attach_to_base_component ──
			// `attach_to_base_component` needs explicit handling: the backing field is
			// `bShouldAttachToBaseComponent`, whose authored name ("ShouldAttachToBase…")
			// doesn't match any of the snake→Pascal→bPrefix variants of the exposed key.
			if (UFoliageType_Actor* ActorFoliage = Cast<UFoliageType_Actor>(Foliage))
			{
				sol::optional<std::string> ActorClassPath = Params.get<sol::optional<std::string>>("actor_class");
				if (ActorClassPath.has_value())
				{
					Params["actor_class"] = sol::lua_nil;
					FString APath = NeoLuaStr::ToFStringOpt(ActorClassPath);
					UClass* NewClass = LoadObject<UClass>(nullptr, *APath);
					if (NewClass && NewClass->IsChildOf(AActor::StaticClass()))
					{
						FProperty* ActorClassProperty = NeoLuaProperty::FindPropertyByName(ActorFoliage->GetClass(),
							GET_MEMBER_NAME_STRING_CHECKED(UFoliageType_Actor, ActorClass));
						bSourceChanged |= ApplyFoliageSourceChange(Foliage, ActorClassProperty, NewClass);
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure: actor_class '%s' not found or not an Actor"), *APath));
					}
				}

				sol::optional<bool> AttachBase = Params.get<sol::optional<bool>>("attach_to_base_component");
				if (AttachBase.has_value())
				{
					ActorFoliage->bShouldAttachToBaseComponent = AttachBase.value();
					Params["attach_to_base_component"] = sol::lua_nil;
				}
			}

			// ── 3. Snake-case enum aliases (legacy API compatibility) ──
			NormalizeScalingKey(Params, "scaling");
			NormalizeMobilityKey(Params, "mobility");

			// ── 4. Range clamps matching the engine's UIMin/UIMax meta ──
			ClampKey<double>(Params, "align_max_angle", 0.0, 359.0);
			ClampKey<double>(Params, "random_pitch_angle", 0.0, 359.0);
			ClampKey<double>(Params, "density", 0.0, 10000.0);
			ClampKey<double>(Params, "density_adjustment_factor", 0.0, 1000.0);
			ClampKey<double>(Params, "radius", 0.0, 100000.0);
			ClampKey<double>(Params, "minimum_layer_weight", 0.0, 1.0);
			ClampKey<double>(Params, "minimum_exclusion_layer_weight", 0.0, 1.0);
			ClampKey<int>(Params, "average_normal_sample_count", 0, INT32_MAX);
			ClampKey<int>(Params, "world_position_offset_disable_distance", 0, INT32_MAX);
			ClampKey<int>(Params, "custom_depth_stencil_value", 0, 255);

			// OverriddenLightMapRes must be a multiple of 4 (engine enforces this on the
			// lightmap atlas). We round up to the nearest factor-of-4 ≥ 4.
			sol::optional<int> LightmapRes = Params.get<sol::optional<int>>("overridden_lightmap_res");
			if (LightmapRes.has_value())
			{
				Params["overridden_lightmap_res"] = FMath::Max(4, (LightmapRes.value() + 3) & ~3);
			}

			// collision_scale needs partial-update semantics: the registered FVector
			// handler defaults missing components to 0, which would zero out untouched
			// axes. Preserve the old "merge only provided components" behavior here.
			sol::optional<sol::table> CollScale = Params.get<sol::optional<sol::table>>("collision_scale");
			if (CollScale.has_value())
			{
				sol::table CS = CollScale.value();
				Params["collision_scale"] = sol::lua_nil;
				sol::optional<double> CX = CS.get<sol::optional<double>>("x");
				sol::optional<double> CY = CS.get<sol::optional<double>>("y");
				sol::optional<double> CZ = CS.get<sol::optional<double>>("z");
				if (CX.has_value()) Foliage->CollisionScale.X = CX.value();
				if (CY.has_value()) Foliage->CollisionScale.Y = CY.value();
				if (CZ.has_value()) Foliage->CollisionScale.Z = CZ.value();
			}

			// ── 5. Reflection walker handles the remaining ~50 UPROPERTYs ──
			FString ApplyErr;
			TArray<FString> Warnings;
			const bool bApplied = NeoLuaProperty::ApplyTable(Foliage, Params, ApplyErr, &Warnings);
			for (const FString& W : Warnings)
			{
				Session.Log(FString::Printf(TEXT("[WARN] configure %s"), *W));
			}

			if (bApplied || bSourceChanged || !ApplyErr.IsEmpty())
			{
				Foliage->MarkPackageDirty();
			}

			Session.Log((bApplied || bSourceChanged)
				? FString(TEXT("[OK] configure()"))
				: FString::Printf(TEXT("[OK] configure() -> nothing changed%s%s"),
					ApplyErr.IsEmpty() ? TEXT("") : TEXT(" — "), *ApplyErr));
			return sol::make_object(Lua, true);
		});
	});
}

// ============================================================================
// ULandscapeGrassType ENRICHMENT
// ============================================================================

static TArray<FLuaFunctionDoc> LandscapeGrassTypeDocs = {};

// Grass variety scaling uses its own enum (EGrassScaling) — only 3 values, no XZ/YZ.
static void NormalizeGrassScalingKey(sol::table Params, const char* Key)
{
	sol::optional<std::string> Val = Params.get<sol::optional<std::string>>(Key);
	if (!Val.has_value()) return;
	if (Val.value() == "lock_xy") Params[Key] = "LockXY";
}

static void BindLandscapeGrassType(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_landscape_grass_type", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = NeoLuaStr::ToFString(PathStr);
		ULandscapeGrassType* GrassType = NeoLuaAsset::Resolve<ULandscapeGrassType>(FPath);
		if (!GrassType) return;

		AssetObj["_help_text"] =
			"LandscapeGrassType enrichment methods:\n"
			"\n"
			"info() — variety_count, enable_density_scaling, and a varieties[] array\n"
			"  containing every editable field per variety.\n"
			"\n"
			"list() — short variety summary (index, mesh name, density)\n"
			"\n"
			"configure(params) — top-level or per-variety settings.\n"
			"  enable_density_scaling (bool)                 — top-level\n"
			"  variety_index (int, 0-based) + any FGrassVariety field — per-variety\n"
			"    (mesh, density, use_grid, placement_jitter, start/end_cull_distance,\n"
			"     min_lod, scaling, scale_x/y/z, random_rotation, align_to_surface,\n"
			"     use_landscape_lightmap, receives_decals, cast_dynamic_shadow,\n"
			"     cast_contact_shadow, affect_distance_field_lighting,\n"
			"     keep_instance_buffer_cpu_copy, wpo_disable_distance,\n"
			"     allowed_density_range={min,max})\n"
			"\n"
			"add(params) — add a new variety (mesh string path required; any other\n"
			"  FGrassVariety field accepted).\n"
			"\n"
			"remove(params) — remove a variety by {index=...}.\n";

		// ================================================================
		// info()
		// ================================================================
		AssetObj.set_function("info", [GrassType, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(GrassType))
			{
				Session.Log(TEXT("[FAIL] info -> asset no longer valid"));
				return sol::lua_nil;
			}

			sol::table R = Lua.create_table();
			R["variety_count"] = GrassType->GrassVarieties.Num();
			R["enable_density_scaling"] = static_cast<bool>(GrassType->bEnableDensityScaling);

			auto Interval = [&Lua](const FFloatInterval& I) { sol::table T = Lua.create_table(); T["min"] = I.Min; T["max"] = I.Max; return T; };

			sol::table Varieties = Lua.create_table();
			for (int32 i = 0; i < GrassType->GrassVarieties.Num(); i++)
			{
				const FGrassVariety& V = GrassType->GrassVarieties[i];
				sol::table VT = Lua.create_table();

				if (V.GrassMesh)
					VT["mesh"] = std::string(TCHAR_TO_UTF8(*V.GrassMesh->GetPathName()));
				VT["density"] = V.GrassDensity.Default;
				VT["use_grid"] = V.bUseGrid;
				VT["placement_jitter"] = V.PlacementJitter;
				VT["start_cull_distance"] = V.StartCullDistance.Default;
				VT["end_cull_distance"] = V.EndCullDistance.Default;
				VT["min_lod"] = V.MinLOD;

				VT["scaling"] = GrassScalingToString(V.Scaling);
				VT["scale_x"] = Interval(V.ScaleX);
				VT["scale_y"] = Interval(V.ScaleY);
				VT["scale_z"] = Interval(V.ScaleZ);

				VT["random_rotation"] = V.RandomRotation;
				VT["align_to_surface"] = V.AlignToSurface;
				VT["use_landscape_lightmap"] = V.bUseLandscapeLightmap;
				VT["receives_decals"] = V.bReceivesDecals;
				VT["cast_dynamic_shadow"] = V.bCastDynamicShadow;
				VT["cast_contact_shadow"] = V.bCastContactShadow;
				VT["affect_distance_field_lighting"] = V.bAffectDistanceFieldLighting;
				VT["keep_instance_buffer_cpu_copy"] = V.bKeepInstanceBufferCPUCopy;
				VT["wpo_disable_distance"] = static_cast<int32>(V.InstanceWorldPositionOffsetDisableDistance);
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
				VT["allowed_density_range"] = Interval(V.AllowedDensityRange);
#endif

				if (V.OverrideMaterials.Num() > 0)
				{
					sol::table Mats = Lua.create_table();
					for (int32 j = 0; j < V.OverrideMaterials.Num(); j++)
						if (V.OverrideMaterials[j])
							Mats[j + 1] = std::string(TCHAR_TO_UTF8(*V.OverrideMaterials[j]->GetPathName()));
					VT["override_materials"] = Mats;
				}

				Varieties[i + 1] = VT;
			}
			R["varieties"] = Varieties;

			Session.Log(FString::Printf(TEXT("[OK] info() -> LandscapeGrassType, %d varieties"),
				GrassType->GrassVarieties.Num()));
			return R;
		});

		// ================================================================
		// list() — abbreviated variety summary
		// ================================================================
		AssetObj.set_function("list", [GrassType, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(GrassType))
			{
				Session.Log(TEXT("[FAIL] list -> asset no longer valid"));
				return sol::lua_nil;
			}

			sol::table R = Lua.create_table();
			for (int32 i = 0; i < GrassType->GrassVarieties.Num(); i++)
			{
				const FGrassVariety& V = GrassType->GrassVarieties[i];
				sol::table VT = Lua.create_table();
				VT["index"] = i;
				VT["mesh"] = V.GrassMesh ? std::string(TCHAR_TO_UTF8(*V.GrassMesh->GetName())) : std::string("(none)");
				VT["density"] = V.GrassDensity.Default;
				R[i + 1] = VT;
			}

			Session.Log(FString::Printf(TEXT("[OK] list() -> %d varieties"), GrassType->GrassVarieties.Num()));
			return R;
		});

		// ================================================================
		// configure(params) — top-level + per-variety
		//
		// Variety fields route through ApplyTableToStruct on FGrassVariety —
		// covers the 20+ per-variety UPROPERTYs. The `mesh` key is special-
		// cased because FGrassVariety::GrassMesh is a UStaticMesh* and we
		// want asset-resolve semantics (find-then-load) rather than
		// FObjectProperty::ImportText's parser (which does work but is
		// stricter about the path format).
		//
		// GrassDensity, StartCullDistance, EndCullDistance are FPerPlatformFloat/
		// FPerPlatformInt — not plain floats. We map `density` / `start_cull_distance` /
		// `end_cull_distance` to their `.Default` sub-field; the full per-platform
		// table isn't exposed here (the old API didn't expose it either).
		// ================================================================
		AssetObj.set_function("configure", [GrassType, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(GrassType))
			{
				Session.Log(TEXT("[FAIL] configure -> asset no longer valid"));
				return sol::lua_nil;
			}

			const FScopedTransaction Transaction(FText::FromString(TEXT("LandscapeGrassType: Configure")));
			GrassType->Modify();

			// Top-level settings write directly to the UObject.
			{
				sol::optional<bool> DensityScaling = Params.get<sol::optional<bool>>("enable_density_scaling");
				if (DensityScaling.has_value())
				{
					GrassType->bEnableDensityScaling = DensityScaling.value();
					Params["enable_density_scaling"] = sol::lua_nil;
				}
			}

			// Per-variety configuration.
			sol::optional<int> VarIdx = Params.get<sol::optional<int>>("variety_index");
			if (VarIdx.has_value())
			{
				int32 Idx = VarIdx.value();
				Params["variety_index"] = sol::lua_nil;

				if (Idx < 0 || Idx >= GrassType->GrassVarieties.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure: variety_index %d out of range [0,%d)"),
						Idx, GrassType->GrassVarieties.Num()));
					return sol::lua_nil;
				}

				FGrassVariety& V = GrassType->GrassVarieties[Idx];

				// 1. Mesh — explicit asset resolve.
				sol::optional<std::string> MeshPath = Params.get<sol::optional<std::string>>("mesh");
				if (MeshPath.has_value())
				{
					Params["mesh"] = sol::lua_nil;
					FString MPath = NeoLuaStr::ToFStringOpt(MeshPath);
					if (UStaticMesh* NewMesh = NeoLuaAsset::Resolve<UStaticMesh>(MPath))
					{
						V.GrassMesh = NewMesh;
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure: mesh '%s' not found"), *MPath));
					}
				}

				// 2. Per-platform scalar "defaults" — map the flat key to the .Default sub-field.
				sol::optional<double> DensityVal = Params.get<sol::optional<double>>("density");
				if (DensityVal.has_value())
				{
					V.GrassDensity.Default = FMath::Max(0.0f, static_cast<float>(DensityVal.value()));
					Params["density"] = sol::lua_nil;
				}

				sol::optional<int> StartCull = Params.get<sol::optional<int>>("start_cull_distance");
				if (StartCull.has_value())
				{
					V.StartCullDistance.Default = FMath::Max(0, StartCull.value());
					Params["start_cull_distance"] = sol::lua_nil;
				}

				sol::optional<int> EndCull = Params.get<sol::optional<int>>("end_cull_distance");
				if (EndCull.has_value())
				{
					V.EndCullDistance.Default = FMath::Max(0, EndCull.value());
					Params["end_cull_distance"] = sol::lua_nil;
				}

				// 3. `wpo_disable_distance` → InstanceWorldPositionOffsetDisableDistance.
				// The full field name isn't reachable from the short snake_case key via
				// any of the FindPropertyByName variants, so write it directly.
				sol::optional<int> WPODist = Params.get<sol::optional<int>>("wpo_disable_distance");
				if (WPODist.has_value())
				{
					V.InstanceWorldPositionOffsetDisableDistance = static_cast<uint32>(FMath::Max(0, WPODist.value()));
					Params["wpo_disable_distance"] = sol::lua_nil;
				}

				// 4. Clamps matching the engine's UIMin/UIMax meta on fields we still
				//    route through the reflection walker.
				ClampKey<double>(Params, "placement_jitter", 0.0, 1.0);
				ClampKey<int>(Params, "min_lod", -1, 8);

				// 5. Snake_case enum normalization.
				NormalizeGrassScalingKey(Params, "scaling");

				// 6. Reflection walker covers the remaining ~20 FGrassVariety UPROPERTYs.
				FString ApplyErr;
				TArray<FString> Warnings;
				NeoLuaProperty::ApplyTableToStruct(FGrassVariety::StaticStruct(), &V, GrassType, Params, ApplyErr, &Warnings);
				for (const FString& W : Warnings)
				{
					Session.Log(FString::Printf(TEXT("[WARN] configure[%d] %s"), Idx, *W));
				}
			}

			FPropertyChangedEvent Event(nullptr, EPropertyChangeType::ValueSet);
			GrassType->PostEditChangeProperty(Event);
			GrassType->MarkPackageDirty();

			Session.Log(TEXT("[OK] configure()"));
			return sol::make_object(Lua, true);
		});

		// ================================================================
		// add(params) — new variety. Mesh is required; remaining FGrassVariety
		// fields flow through ApplyTableToStruct on the newly-constructed variety
		// before it's added to the array.
		// ================================================================
		AssetObj.set_function("add", [GrassType, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(GrassType))
			{
				Session.Log(TEXT("[FAIL] add -> asset no longer valid"));
				return sol::lua_nil;
			}

			sol::optional<std::string> MeshPath = Params.get<sol::optional<std::string>>("mesh");
			if (!MeshPath.has_value())
			{
				Session.Log(TEXT("[FAIL] add: 'mesh' is required (asset path to a StaticMesh)"));
				return sol::lua_nil;
			}

			FString MPath = NeoLuaStr::ToFStringOpt(MeshPath);
			UStaticMesh* Mesh = NeoLuaAsset::Resolve<UStaticMesh>(MPath);
			if (!Mesh)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add: mesh '%s' not found"), *MPath));
				return sol::lua_nil;
			}

			const FScopedTransaction Transaction(FText::FromString(TEXT("LandscapeGrassType: Add Variety")));
			GrassType->Modify();

			FGrassVariety NewVariety;
			NewVariety.GrassMesh = Mesh;
			Params["mesh"] = sol::lua_nil;

			// Per-platform .Default mapping + same clamps as configure().
			sol::optional<double> DensityVal = Params.get<sol::optional<double>>("density");
			if (DensityVal.has_value())
			{
				NewVariety.GrassDensity.Default = FMath::Max(0.0f, static_cast<float>(DensityVal.value()));
				Params["density"] = sol::lua_nil;
			}
			sol::optional<int> StartCull = Params.get<sol::optional<int>>("start_cull_distance");
			if (StartCull.has_value())
			{
				NewVariety.StartCullDistance.Default = FMath::Max(0, StartCull.value());
				Params["start_cull_distance"] = sol::lua_nil;
			}
			sol::optional<int> EndCull = Params.get<sol::optional<int>>("end_cull_distance");
			if (EndCull.has_value())
			{
				NewVariety.EndCullDistance.Default = FMath::Max(0, EndCull.value());
				Params["end_cull_distance"] = sol::lua_nil;
			}

			// wpo_disable_distance → InstanceWorldPositionOffsetDisableDistance (explicit).
			sol::optional<int> WPODist = Params.get<sol::optional<int>>("wpo_disable_distance");
			if (WPODist.has_value())
			{
				NewVariety.InstanceWorldPositionOffsetDisableDistance = static_cast<uint32>(FMath::Max(0, WPODist.value()));
				Params["wpo_disable_distance"] = sol::lua_nil;
			}

			ClampKey<double>(Params, "placement_jitter", 0.0, 1.0);
			NormalizeGrassScalingKey(Params, "scaling");

			FString ApplyErr;
			TArray<FString> Warnings;
			NeoLuaProperty::ApplyTableToStruct(FGrassVariety::StaticStruct(), &NewVariety, GrassType, Params, ApplyErr, &Warnings);
			for (const FString& W : Warnings)
			{
				Session.Log(FString::Printf(TEXT("[WARN] add %s"), *W));
			}

			int32 NewIdx = GrassType->GrassVarieties.Add(MoveTemp(NewVariety));

			FPropertyChangedEvent Event(nullptr, EPropertyChangeType::ValueSet);
			GrassType->PostEditChangeProperty(Event);
			GrassType->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] add(mesh='%s') -> index %d"), *MPath, NewIdx));

			sol::table Result = Lua.create_table();
			Result["index"] = NewIdx;
			return sol::make_object(Lua, Result);
		});

		// ================================================================
		// remove(params) — by {index=...}
		// ================================================================
		AssetObj.set_function("remove", [GrassType, &Session](sol::table /*self*/,
			sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(GrassType))
			{
				Session.Log(TEXT("[FAIL] remove -> asset no longer valid"));
				return sol::lua_nil;
			}

			sol::optional<int> Idx = Params.get<sol::optional<int>>("index");
			if (!Idx.has_value())
			{
				Session.Log(TEXT("[FAIL] remove: 'index' is required (0-based variety index)"));
				return sol::lua_nil;
			}

			int32 Index = Idx.value();
			if (Index < 0 || Index >= GrassType->GrassVarieties.Num())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove: index %d out of range [0,%d)"),
					Index, GrassType->GrassVarieties.Num()));
				return sol::lua_nil;
			}

			const FScopedTransaction Transaction(FText::FromString(TEXT("LandscapeGrassType: Remove Variety")));
			GrassType->Modify();

			FString RemovedName = GrassType->GrassVarieties[Index].GrassMesh
				? GrassType->GrassVarieties[Index].GrassMesh->GetName()
				: TEXT("(none)");

			GrassType->GrassVarieties.RemoveAt(Index);

			FPropertyChangedEvent Event(nullptr, EPropertyChangeType::ValueSet);
			GrassType->PostEditChangeProperty(Event);
			GrassType->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] remove(index=%d) -> removed '%s', %d varieties remain"),
				Index, *RemovedName, GrassType->GrassVarieties.Num()));
			return sol::make_object(Lua, true);
		});
	});
}

// ============================================================================
// REGISTRATION
// ============================================================================

REGISTER_LUA_BINDING(FoliageType, FoliageTypeDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindFoliageType(Lua, Session);
});

REGISTER_LUA_BINDING(LandscapeGrassType, LandscapeGrassTypeDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindLandscapeGrassType(Lua, Session);
});
