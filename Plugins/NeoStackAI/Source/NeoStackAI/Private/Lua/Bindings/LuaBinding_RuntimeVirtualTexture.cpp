// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Lua/LuaAssetResolver.h"
#include "Lua/LuaStr.h"

#include "VT/RuntimeVirtualTexture.h"
#include "VT/RuntimeVirtualTextureEnum.h"
#include "PixelFormat.h"
#include "UObject/UnrealType.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// HELPERS
// ============================================================================

static const char* MaterialTypeToString(ERuntimeVirtualTextureMaterialType Type)
{
	switch (Type)
	{
	case ERuntimeVirtualTextureMaterialType::BaseColor:                            return "base_color";
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
	case ERuntimeVirtualTextureMaterialType::Mask4:                                return "mask4";
#endif
	case ERuntimeVirtualTextureMaterialType::BaseColor_Normal_Roughness:           return "base_color_normal_roughness";
	case ERuntimeVirtualTextureMaterialType::BaseColor_Normal_Specular:            return "base_color_normal_specular";
	case ERuntimeVirtualTextureMaterialType::BaseColor_Normal_Specular_YCoCg:      return "base_color_normal_specular_ycocg";
	case ERuntimeVirtualTextureMaterialType::BaseColor_Normal_Specular_Mask_YCoCg: return "base_color_normal_specular_mask_ycocg";
	case ERuntimeVirtualTextureMaterialType::WorldHeight:                          return "world_height";
	case ERuntimeVirtualTextureMaterialType::Displacement:                         return "displacement";
	default:                                                                       return "unknown";
	}
}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
static const char* PriorityToString(EVTProducerPriority P)
{
	switch (P)
	{
	case EVTProducerPriority::Lowest:      return "lowest";
	case EVTProducerPriority::Lower:       return "lower";
	case EVTProducerPriority::Low:         return "low";
	case EVTProducerPriority::BelowNormal: return "below_normal";
	case EVTProducerPriority::Normal:      return "normal";
	case EVTProducerPriority::AboveNormal: return "above_normal";
	case EVTProducerPriority::High:        return "high";
	case EVTProducerPriority::Highest:     return "highest";
	default:                               return "normal";
	}
}

static EVTProducerPriority GetStoredCustomPriority(const URuntimeVirtualTexture* RVT)
{
	if (const FEnumProperty* Prop = FindFProperty<FEnumProperty>(URuntimeVirtualTexture::StaticClass(), TEXT("CustomPriority")))
	{
		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(RVT);
		return static_cast<EVTProducerPriority>(Prop->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr));
	}
	return RVT->GetPriority();
}
#endif

static FString TextureGroupToString(TEnumAsByte<TextureGroup> Group)
{
	UEnum* Enum = StaticEnum<TextureGroup>();
	if (!Enum) return TEXT("World");
	FString Name = Enum->GetNameStringByValue((int64)Group.GetValue());
	// Strip "TEXTUREGROUP_" prefix for cleaner Lua interface
	Name.RemoveFromStart(TEXT("TEXTUREGROUP_"));
	return Name;
}

// ============================================================================
// BINDING
// ============================================================================

static TArray<FLuaFunctionDoc> RuntimeVirtualTextureDocs = {};

static void BindRuntimeVirtualTexture(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_runtime_virtual_texture", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = NeoLuaStr::ToFString(PathStr);
		URuntimeVirtualTexture* RVT = NeoLuaAsset::Resolve<URuntimeVirtualTexture>(FPath);
		if (!RVT) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"RuntimeVirtualTexture enrichment methods:\n"
			"\n"
			"info() — structured summary:\n"
			"  tile_count, tile_size, tile_border_size, size (derived),\n"
			"  page_table_size, material_type, compress_textures,\n"
			"  clear_textures, adaptive, continuous_update,\n"
			"  single_physical_space, private_space, remove_low_mips,\n"
			"  low_quality_compression, layer_count, lod_group,\n"
			"  custom_material_data {x,y,z,w},\n"
			"  use_custom_priority, custom_priority, effective_priority,\n"
			"  layers [{index, format, srgb, ycocg}, ...]\n"
			"\n"
			"Property editing uses the generic asset reflection API:\n"
			"  get(\"PropertyName\")\n"
			"  set(\"PropertyName\", \"Value\")\n"
			"  list_properties(filter?, all?)\n"
			"\n"
			"Use raw engine property names and stored values, e.g.:\n"
			"  TileCount, TileSize, TileBorderSize, MaterialType,\n"
			"  bCompressTextures, bClearTextures, bAdaptive,\n"
			"  bContinuousUpdate, bSinglePhysicalSpace, bPrivateSpace,\n"
			"  RemoveLowMips, bUseLowQualityCompression, LODGroup,\n"
			"  CustomMaterialData, bUseCustomPriority, CustomPriority\n";

		// ================================================================
		// info()
		// ================================================================
		AssetObj.set_function("info", [RVT, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			if (!IsValid(RVT))
			{
				Session.Log(TEXT("[FAIL] info -> asset no longer valid"));
				return sol::lua_nil;
			}

			sol::table R = Lua.create_table();

			R["tile_count"] = RVT->GetTileCount();
			R["tile_size"] = RVT->GetTileSize();
			R["tile_border_size"] = RVT->GetTileBorderSize();
			R["size"] = RVT->GetSize();
			R["page_table_size"] = RVT->GetPageTableSize();
			R["material_type"] = MaterialTypeToString(RVT->GetMaterialType());
			R["compress_textures"] = RVT->GetCompressTextures();
			R["clear_textures"] = RVT->GetClearTextures();
			R["adaptive"] = RVT->GetAdaptivePageTable();
			R["continuous_update"] = RVT->GetContinuousUpdate();
			R["single_physical_space"] = RVT->GetSinglePhysicalSpace();
			R["private_space"] = RVT->GetPrivateSpace();
			R["remove_low_mips"] = RVT->GetRemoveLowMips();
			R["low_quality_compression"] = RVT->GetLQCompression();
			R["layer_count"] = RVT->GetLayerCount();
			R["lod_group"] = std::string(TCHAR_TO_UTF8(*TextureGroupToString(RVT->GetLODGroup())));
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			R["use_custom_priority"] = RVT->GetUseCustomPriority();
			R["custom_priority"] = PriorityToString(GetStoredCustomPriority(RVT));
			R["default_priority"] = PriorityToString(RVT->GetDefaultPriority());
			R["effective_priority"] = PriorityToString(RVT->GetPriority());

			// Custom material data
			FVector4f CMD = RVT->GetCustomMaterialData();
			sol::table CmdT = Lua.create_table();
			CmdT["x"] = CMD.X;
			CmdT["y"] = CMD.Y;
			CmdT["z"] = CMD.Z;
			CmdT["w"] = CMD.W;
			R["custom_material_data"] = CmdT;
#endif

			// Per-layer info
			int32 LayerCount = RVT->GetLayerCount();
			sol::table Layers = Lua.create_table();
			for (int32 i = 0; i < LayerCount; ++i)
			{
				sol::table Layer = Lua.create_table();
				Layer["index"] = i;
				EPixelFormat Fmt = RVT->GetLayerFormat(i);
				Layer["format"] = std::string(TCHAR_TO_UTF8(GPixelFormats[Fmt].Name));
				Layer["srgb"] = RVT->IsLayerSRGB(i);
				Layer["ycocg"] = RVT->IsLayerYCoCg(i);
				Layers[i + 1] = Layer;
			}
			R["layers"] = Layers;

			Session.Log(FString::Printf(TEXT("[OK] info() -> RuntimeVirtualTexture, size=%d, material=%s, layers=%d"),
				RVT->GetSize(), UTF8_TO_TCHAR(MaterialTypeToString(RVT->GetMaterialType())),
				RVT->GetLayerCount()));
			return R;
		});
	});
}

REGISTER_LUA_BINDING(RuntimeVirtualTexture, RuntimeVirtualTextureDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindRuntimeVirtualTexture(Lua, Session);
});
