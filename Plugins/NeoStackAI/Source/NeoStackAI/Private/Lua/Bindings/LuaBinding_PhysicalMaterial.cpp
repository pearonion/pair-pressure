// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Lua/LuaAssetResolver.h"
#include "Lua/LuaStr.h"

#include "PhysicalMaterials/PhysicalMaterial.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// HELPERS
// ============================================================================

static const char* CombineModeToString(EFrictionCombineMode::Type Mode)
{
	switch (Mode)
	{
	case EFrictionCombineMode::Average:  return "Average";
	case EFrictionCombineMode::Min:      return "Min";
	case EFrictionCombineMode::Multiply: return "Multiply";
	case EFrictionCombineMode::Max:      return "Max";
	default:                             return "Average";
	}
}

static FString SoftCollisionModeToString(EPhysicalMaterialSoftCollisionMode Mode)
{
	if (const UEnum* Enum = StaticEnum<EPhysicalMaterialSoftCollisionMode>())
	{
		return Enum->GetNameStringByValue(static_cast<int64>(Mode));
	}

	switch (Mode)
	{
	case EPhysicalMaterialSoftCollisionMode::None:              return TEXT("None");
	case EPhysicalMaterialSoftCollisionMode::RelativeThickness: return TEXT("RelativeThickness");
	case EPhysicalMaterialSoftCollisionMode::AbsoluteThickess:  return TEXT("AbsoluteThickess");
	default:                                                    return TEXT("None");
	}
}
// ============================================================================
// BINDING
// ============================================================================

static TArray<FLuaFunctionDoc> PhysicalMaterialDocs = {};

static void BindPhysicalMaterial(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_physical_material", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = NeoLuaStr::ToFString(PathStr);
		UPhysicalMaterial* PhysMat = NeoLuaAsset::Resolve<UPhysicalMaterial>(FPath);
		if (!PhysMat) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"PhysicalMaterial enrichment methods:\n"
			"\n"
			"info() — structured summary:\n"
			"  friction, static_friction, friction_combine_mode, override_friction_combine_mode,\n"
			"  restitution, restitution_combine_mode, override_restitution_combine_mode,\n"
			"  density, surface_type, raise_mass_to_power,\n"
			"  sleep_linear_velocity_threshold, sleep_angular_velocity_threshold, sleep_counter_threshold,\n"
			"  strength (tensile, compression, shear), damage_threshold_multiplier, debug_color,\n"
			"  soft_collision_mode, soft_collision_thickness, base_friction_impulse (experimental)\n"
			"\n"
			"Property editing uses the generic asset reflection API:\n"
			"  get(\"PropertyName\")\n"
			"  set(\"PropertyName\", \"Value\")\n"
			"  list_properties(filter?, all?)\n"
			"  property_meta(\"PropertyName\")\n"
			"  set respects numeric ClampMin/ClampMax metadata exposed by property_meta().\n"
			"\n"
			"Use raw engine property names, e.g.:\n"
			"  Friction\n"
			"  StaticFriction\n"
			"  FrictionCombineMode\n"
			"  bOverrideFrictionCombineMode\n"
			"  Restitution\n"
			"  RestitutionCombineMode\n"
			"  bOverrideRestitutionCombineMode\n"
			"  Density\n"
			"  SurfaceType\n"
			"  RaiseMassToPower\n"
			"  SleepLinearVelocityThreshold\n"
			"  SleepAngularVelocityThreshold\n"
			"  SleepCounterThreshold\n"
			"  Strength.TensileStrength\n"
			"  Strength.CompressionStrength\n"
			"  Strength.ShearStrength\n"
			"  DamageModifier.DamageThresholdMultiplier\n"
			"  DebugColor\n"
			"  SoftCollisionMode\n"
			"  SoftCollisionThickness\n"
			"  BaseFrictionImpulse\n";

		// ================================================================
		// info()
		// ================================================================
		AssetObj.set_function("info", [PhysMat, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(PhysMat))
			{
				Session.Log(TEXT("[FAIL] info -> asset no longer valid"));
				return sol::lua_nil;
			}

			sol::table Result = Lua.create_table();

			// Surface properties
			Result["friction"] = PhysMat->Friction;
			Result["static_friction"] = PhysMat->StaticFriction;
			Result["friction_combine_mode"] = CombineModeToString(PhysMat->FrictionCombineMode.GetValue());
			Result["override_friction_combine_mode"] = static_cast<bool>(PhysMat->bOverrideFrictionCombineMode);

			Result["restitution"] = PhysMat->Restitution;
			Result["restitution_combine_mode"] = CombineModeToString(PhysMat->RestitutionCombineMode.GetValue());
			Result["override_restitution_combine_mode"] = static_cast<bool>(PhysMat->bOverrideRestitutionCombineMode);

			// Object properties
			Result["density"] = PhysMat->Density;
			Result["raise_mass_to_power"] = PhysMat->RaiseMassToPower;
			Result["sleep_linear_velocity_threshold"] = PhysMat->SleepLinearVelocityThreshold;
			Result["sleep_angular_velocity_threshold"] = PhysMat->SleepAngularVelocityThreshold;
			Result["sleep_counter_threshold"] = PhysMat->SleepCounterThreshold;

			// Surface type
			const UEnum* SurfaceEnum = StaticEnum<EPhysicalSurface>();
			if (SurfaceEnum)
			{
				FString SurfaceName = SurfaceEnum->GetNameStringByValue(static_cast<int64>(PhysMat->SurfaceType.GetValue()));
				Result["surface_type"] = TCHAR_TO_UTF8(*SurfaceName);
			}
			else
			{
				Result["surface_type"] = static_cast<int>(PhysMat->SurfaceType.GetValue());
			}

			// Strength
			sol::table StrengthTable = Lua.create_table();
			StrengthTable["tensile"] = PhysMat->Strength.TensileStrength;
			StrengthTable["compression"] = PhysMat->Strength.CompressionStrength;
			StrengthTable["shear"] = PhysMat->Strength.ShearStrength;
			Result["strength"] = StrengthTable;

			// Damage modifier
			Result["damage_threshold_multiplier"] = PhysMat->DamageModifier.DamageThresholdMultiplier;

			// Debug color
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			sol::table ColorTable = Lua.create_table();
			ColorTable["r"] = PhysMat->DebugColor.R;
			ColorTable["g"] = PhysMat->DebugColor.G;
			ColorTable["b"] = PhysMat->DebugColor.B;
			ColorTable["a"] = PhysMat->DebugColor.A;
			Result["debug_color"] = ColorTable;
#endif

			// Experimental properties
			const FString SoftCollisionMode = SoftCollisionModeToString(PhysMat->SoftCollisionMode);
			Result["soft_collision_mode"] = TCHAR_TO_UTF8(*SoftCollisionMode);
			Result["soft_collision_thickness"] = PhysMat->SoftCollisionThickness;
			Result["base_friction_impulse"] = PhysMat->BaseFrictionImpulse;

			Session.Log(FString::Printf(TEXT("[OK] info() -> PhysicalMaterial, friction=%.2f, restitution=%.2f, density=%.2f"),
				(double)PhysMat->Friction, (double)PhysMat->Restitution, (double)PhysMat->Density));
			return Result;
		});
	});
}

REGISTER_LUA_BINDING(PhysicalMaterial, PhysicalMaterialDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindPhysicalMaterial(Lua, Session);
});
