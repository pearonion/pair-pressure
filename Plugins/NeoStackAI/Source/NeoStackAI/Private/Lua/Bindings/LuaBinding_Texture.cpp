// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Lua/LuaEnumReflection.h"
#include "Lua/LuaAssetResolver.h"
#include "Lua/LuaStr.h"
#include "Tools/NeoStackToolUtils.h"

#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureCube.h"
#include "Engine/TextureDefines.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/CanvasRenderTarget2D.h"
#include "Engine/VolumeTexture.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// HELPERS
// ============================================================================

static std::string CompressionSettingsToString(TextureCompressionSettings CS)
{
	FString Name = NeoLuaEnum::ToString(CS);
	if (Name.IsEmpty() || Name.IsNumeric() || Name.Equals(TEXT("TC_MAX")))
	{
		return "Unknown";
	}
	Name.RemoveFromStart(TEXT("TC_"));
	return NeoLuaStr::ToStdString(Name);
}


static const char* FilterToString(TextureFilter F)
{
	switch (F)
	{
	case TF_Nearest:  return "Nearest";
	case TF_Bilinear: return "Bilinear";
	case TF_Trilinear:return "Trilinear";
	case TF_Default:  return "Default";
	default:          return "Unknown";
	}
}

static const char* AddressToString(TextureAddress A)
{
	switch (A)
	{
	case TA_Wrap:   return "Wrap";
	case TA_Clamp:  return "Clamp";
	case TA_Mirror: return "Mirror";
	default:        return "Unknown";
	}
}

static std::string RenderTargetFormatToString(ETextureRenderTargetFormat Format)
{
	FString Name = NeoLuaEnum::ToString(Format);
	Name.RemoveFromStart(TEXT("RTF_"));
	return NeoLuaStr::ToStdString(Name);
}

static std::string RenderTargetSampleCountToString(ETextureRenderTargetSampleCount SampleCount)
{
	FString Name = NeoLuaEnum::ToString(SampleCount);
	Name.RemoveFromStart(TEXT("RTSC_"));
	return NeoLuaStr::ToStdString(Name);
}

static const char* MipGenToString(TextureMipGenSettings M)
{
	switch (M)
	{
	case TMGS_FromTextureGroup:  return "FromTextureGroup";
	case TMGS_SimpleAverage:     return "SimpleAverage";
	case TMGS_Sharpen0:          return "Sharpen0";
	case TMGS_Sharpen1:          return "Sharpen1";
	case TMGS_Sharpen2:          return "Sharpen2";
	case TMGS_Sharpen3:          return "Sharpen3";
	case TMGS_Sharpen4:          return "Sharpen4";
	case TMGS_Sharpen5:          return "Sharpen5";
	case TMGS_Sharpen6:          return "Sharpen6";
	case TMGS_Sharpen7:          return "Sharpen7";
	case TMGS_Sharpen8:          return "Sharpen8";
	case TMGS_Sharpen9:          return "Sharpen9";
	case TMGS_Sharpen10:         return "Sharpen10";
	case TMGS_NoMipmaps:         return "NoMipmaps";
	case TMGS_LeaveExistingMips: return "LeaveExistingMips";
	case TMGS_Blur1:             return "Blur1";
	case TMGS_Blur2:             return "Blur2";
	case TMGS_Blur3:             return "Blur3";
	case TMGS_Blur4:             return "Blur4";
	case TMGS_Blur5:             return "Blur5";
	case TMGS_Unfiltered:        return "Unfiltered";
	case TMGS_Angular:           return "Angular";
	default:                     return "Unknown";
	}
}

static const char* LossyCompressionToString(ETextureLossyCompressionAmount A)
{
	switch (A)
	{
	case TLCA_Default: return "Default";
	case TLCA_None:    return "None";
	case TLCA_Lowest:  return "Lowest";
	case TLCA_Low:     return "Low";
	case TLCA_Medium:  return "Medium";
	case TLCA_High:    return "High";
	case TLCA_Highest: return "Highest";
	default:           return "Unknown";
	}
}

static const char* PowerOfTwoToString(ETexturePowerOfTwoSetting::Type P)
{
	switch (P)
	{
	case ETexturePowerOfTwoSetting::None:                       return "None";
	case ETexturePowerOfTwoSetting::PadToPowerOfTwo:            return "PadToPowerOfTwo";
	case ETexturePowerOfTwoSetting::PadToSquarePowerOfTwo:      return "PadToSquarePowerOfTwo";
	case ETexturePowerOfTwoSetting::StretchToPowerOfTwo:        return "StretchToPowerOfTwo";
	case ETexturePowerOfTwoSetting::StretchToSquarePowerOfTwo:  return "StretchToSquarePowerOfTwo";
	case ETexturePowerOfTwoSetting::ResizeToSpecificResolution: return "ResizeToSpecificResolution";
	default:                                                    return "Unknown";
	}
}

static const char* CompositeTextureToString(ECompositeTextureMode M)
{
	switch (M)
	{
	case CTM_Disabled:                 return "Disabled";
	case CTM_NormalRoughnessToRed:     return "NormalRoughnessToRed";
	case CTM_NormalRoughnessToGreen:   return "NormalRoughnessToGreen";
	case CTM_NormalRoughnessToBlue:    return "NormalRoughnessToBlue";
	case CTM_NormalRoughnessToAlpha:   return "NormalRoughnessToAlpha";
	default:                           return "Unknown";
	}
}

static const char* CompressionQualityToString(ETextureCompressionQuality Q)
{
	switch (Q)
	{
	case TCQ_Default: return "Default";
	case TCQ_Lowest:  return "Lowest";
	case TCQ_Low:     return "Low";
	case TCQ_Medium:  return "Medium";
	case TCQ_High:    return "High";
	case TCQ_Highest: return "Highest";
	default:          return "Unknown";
	}
}

static const char* AvailabilityToString(ETextureAvailability A)
{
	switch (A)
	{
	case ETextureAvailability::GPU: return "GPU";
	case ETextureAvailability::CPU: return "CPU";
	default:                        return "Unknown";
	}
}

static const char* MipLoadOptionsToString(ETextureMipLoadOptions O)
{
	switch (O)
	{
	case ETextureMipLoadOptions::Default:      return "Default";
	case ETextureMipLoadOptions::AllMips:       return "AllMips";
	case ETextureMipLoadOptions::OnlyFirstMip:  return "OnlyFirstMip";
	default:                                    return "Unknown";
	}
}

static const char* CookTilingToString(TextureCookPlatformTilingSettings T)
{
	switch (T)
	{
	case TCPTS_FromTextureGroup: return "FromTextureGroup";
	case TCPTS_Tile:             return "Tile";
	case TCPTS_DoNotTile:        return "DoNotTile";
	default:                     return "Unknown";
	}
}

static const char* DownscaleOptionsToString(ETextureDownscaleOptions D)
{
	switch (D)
	{
	case ETextureDownscaleOptions::Default:       return "Default";
	case ETextureDownscaleOptions::Unfiltered:    return "Unfiltered";
	case ETextureDownscaleOptions::SimpleAverage: return "SimpleAverage";
	case ETextureDownscaleOptions::Sharpen0:      return "Sharpen0";
	case ETextureDownscaleOptions::Sharpen1:      return "Sharpen1";
	case ETextureDownscaleOptions::Sharpen2:      return "Sharpen2";
	case ETextureDownscaleOptions::Sharpen3:      return "Sharpen3";
	case ETextureDownscaleOptions::Sharpen4:      return "Sharpen4";
	case ETextureDownscaleOptions::Sharpen5:      return "Sharpen5";
	case ETextureDownscaleOptions::Sharpen6:      return "Sharpen6";
	case ETextureDownscaleOptions::Sharpen7:      return "Sharpen7";
	case ETextureDownscaleOptions::Sharpen8:      return "Sharpen8";
	case ETextureDownscaleOptions::Sharpen9:      return "Sharpen9";
	case ETextureDownscaleOptions::Sharpen10:     return "Sharpen10";
	default:                                      return "Unknown";
	}
}

// Match LOD group by exact name after stripping TEXTUREGROUP_ prefix
// ============================================================================
// BINDING
// ============================================================================

static TArray<FLuaFunctionDoc> TextureDocs = {};

static void BindTexture(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_texture", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = NeoLuaStr::ToFString(PathStr);
		UTexture* Texture = NeoLuaAsset::Resolve<UTexture>(FPath);
		if (!Texture) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"Texture enrichment.\n"
			"\n"
			"info() — texture summary (size, format, compression, LOD group, mips, source info, color settings, etc.)\n"
			"\n"
			"list(type):\n"
			"  list(\"settings\")    — all current texture settings\n"
			"  list(\"adjustments\") — brightness, vibrance, saturation, hue adjustments\n"
			"  list(\"advanced\")    — advanced compression, padding, availability, downscale settings\n"
			"\n"
			"Set reflected properties with generic set():\n"
			"  set(\"CompressionSettings\", \"TC_Default|TC_Normalmap|TC_BC7|...\")\n"
			"  set(\"SRGB\", true); set(\"Filter\", \"TF_Nearest|TF_Bilinear|TF_Trilinear\")\n"
			"  set(\"LODGroup\", \"TEXTUREGROUP_World\"); set(\"LODBias\", 1)\n"
			"  set(\"MaxTextureSize\", 2048); set(\"MipGenSettings\", \"TMGS_NoMipmaps\")\n"
			"  set(\"VirtualTextureStreaming\", false); set(\"Availability\", \"GPU|CPU\")\n"
			"  set(\"MipLoadOptions\", \"Default|AllMips|OnlyFirstMip\")\n"
			"  set(\"AddressX\", \"TA_Wrap|TA_Clamp|TA_Mirror\")\n"
			"  set(\"AddressY\", \"TA_Wrap|TA_Clamp|TA_Mirror\")\n"
			"  set(\"AdjustBrightness\", 1.0); set(\"AdjustVibrance\", 0.0)\n"
			"  set(\"AdjustSaturation\", 1.0); set(\"AdjustHue\", 0.0)\n"
			"  set(\"AdjustRGBCurve\", 1.0); set(\"AdjustMinAlpha\", 0.0); set(\"AdjustMaxAlpha\", 1.0)\n"
			"  set(\"bChromaKeyTexture\", false); set(\"ChromaKeyThreshold\", 0.0)\n"
			"  set(\"ChromaKeyColor\", {R=0,G=0,B=0,A=255})\n"
			"  set(\"bFlipGreenChannel\", false); set(\"bNormalizeNormals\", true)\n"
			"  set(\"bPreserveBorder\", false); set(\"PowerOfTwoMode\", \"PadToPowerOfTwo\")\n"
			"  set(\"bDoScaleMipsForAlphaCoverage\", false); set(\"AlphaCoverageThresholds\", {X=0,Y=0,Z=0,W=0})\n"
			"  set(\"bOodlePreserveExtremes\", false); set(\"DownscaleOptions\", \"Default\")\n"
			"\n"
			"  compression: Default, Normalmap, Masks, Grayscale, HDR, Alpha, BC7, HalfFloat, etc.\n"
			"  filter: Default, Nearest, Bilinear, Trilinear\n"
			"  address: Wrap, Clamp, Mirror\n"
			"  mip_gen: FromTextureGroup, SimpleAverage, NoMipmaps, Sharpen0-10, Blur1-5, Unfiltered, Angular\n"
			"  lossy_compression_amount: Default, None, Lowest, Low, Medium, High, Highest\n"
			"  compression_quality: Default, Lowest, Low, Medium, High, Highest\n"
			"  power_of_two_mode: None, PadToPowerOfTwo, PadToSquarePowerOfTwo, StretchToPowerOfTwo, StretchToSquarePowerOfTwo, ResizeToSpecificResolution\n"
			"  composite_texture_mode: Disabled, NormalRoughnessToRed/Green/Blue/Alpha\n"
			"  availability: GPU, CPU\n"
			"  mip_load_options: Default, AllMips, OnlyFirstMip\n"
			"  cook_tiling: FromTextureGroup, Tile, DoNotTile\n"
			"  downscale_options: Default, Unfiltered, SimpleAverage, Sharpen0-10\n"
			"  color_space: TCS_None, TCS_sRGB, TCS_Rec2020, TCS_ACESAP0, TCS_ACESAP1, TCS_P3DCI, TCS_P3D65,\n"
			"    TCS_REDWideGamut, TCS_SonySGamut3, TCS_SonySGamut3Cine, TCS_AlexaWideGamut,\n"
			"    TCS_CanonCinemaGamut, TCS_GoProProtuneNative, TCS_PanasonicVGamut, TCS_Custom\n"
			"  encoding_override: TSE_None, TSE_Linear, TSE_sRGB, TSE_ST2084, TSE_Gamma22, TSE_BT1886,\n"
			"    TSE_Gamma26, TSE_Cineon, TSE_REDLog, TSE_REDLog3G10, TSE_SLog1, TSE_SLog2, TSE_SLog3,\n"
			"    TSE_AlexaV3LogC, TSE_CanonLog, TSE_ProTune, TSE_VLog\n";

		// ================================================================
		// info()
		// ================================================================
		AssetObj.set_function("info", [Texture, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Texture)) { Session.Log(TEXT("[FAIL] info -> asset no longer valid")); return sol::lua_nil; }

			sol::table Result = Lua.create_table();
			Result["name"] = TCHAR_TO_UTF8(*Texture->GetName());
			Result["path"] = TCHAR_TO_UTF8(*Texture->GetPathName());
			Result["class"] = TCHAR_TO_UTF8(*Texture->GetClass()->GetName());

			// Type-specific size info
			UTexture2D* Tex2D = Cast<UTexture2D>(Texture);
			UTextureCube* TexCube = Cast<UTextureCube>(Texture);
			UVolumeTexture* TexVol = Cast<UVolumeTexture>(Texture);
			if (UTextureRenderTarget2D* RT2D = Cast<UTextureRenderTarget2D>(Texture))
			{
				Result["width"] = RT2D->SizeX;
				Result["height"] = RT2D->SizeY;
				Result["target_gamma"] = RT2D->TargetGamma;
				Result["address_x"] = AddressToString(static_cast<TextureAddress>(RT2D->AddressX.GetValue()));
				Result["address_y"] = AddressToString(static_cast<TextureAddress>(RT2D->AddressY.GetValue()));
				Result["render_target_format"] = RenderTargetFormatToString(static_cast<ETextureRenderTargetFormat>(RT2D->RenderTargetFormat.GetValue()));
				Result["supports_uav"] = static_cast<bool>(RT2D->bSupportsUAV);
				Result["gpu_shared"] = static_cast<bool>(RT2D->bGPUSharedFlag);
				Result["auto_generate_mips"] = static_cast<bool>(RT2D->bAutoGenerateMips);
				Result["mips_sampler_filter"] = FilterToString(static_cast<TextureFilter>(RT2D->MipsSamplerFilter.GetValue()));
				Result["mips_address_u"] = AddressToString(static_cast<TextureAddress>(RT2D->MipsAddressU.GetValue()));
				Result["mips_address_v"] = AddressToString(static_cast<TextureAddress>(RT2D->MipsAddressV.GetValue()));
				Result["is_srgb"] = static_cast<bool>(RT2D->IsSRGB());
				Result["display_gamma"] = RT2D->GetDisplayGamma();
				Result["pixel_format"] = TCHAR_TO_UTF8(GetPixelFormatString(RT2D->GetFormat()));
				{
					sol::table CC = Lua.create_table();
					CC["r"] = RT2D->ClearColor.R;
					CC["g"] = RT2D->ClearColor.G;
					CC["b"] = RT2D->ClearColor.B;
					CC["a"] = RT2D->ClearColor.A;
					Result["clear_color"] = CC;
				}
				if (UCanvasRenderTarget2D* CanvasRT = Cast<UCanvasRenderTarget2D>(RT2D))
				{
					Result["sample_count"] = RenderTargetSampleCountToString(CanvasRT->GetSampleCount());
					Result["should_clear_on_update"] = CanvasRT->ShouldClearRenderTargetOnReceiveUpdate();
				}
			}
			else if (Tex2D)
			{
				Result["width"] = Tex2D->GetSizeX();
				Result["height"] = Tex2D->GetSizeY();
				Result["num_mips"] = Tex2D->GetNumMips();
				Result["address_x"] = AddressToString(static_cast<TextureAddress>(Tex2D->AddressX.GetValue()));
				Result["address_y"] = AddressToString(static_cast<TextureAddress>(Tex2D->AddressY.GetValue()));
			}
			else if (TexCube)
			{
				Result["width"] = TexCube->GetSizeX();
				Result["height"] = TexCube->GetSizeY();
				Result["num_mips"] = TexCube->GetNumMips();
				Result["num_faces"] = 6;
			}
			else if (TexVol)
			{
				Result["width"] = TexVol->GetSizeX();
				Result["height"] = TexVol->GetSizeY();
				Result["depth"] = TexVol->GetSizeZ();
				Result["num_mips"] = TexVol->GetNumMips();
				Result["address_mode"] = AddressToString(static_cast<TextureAddress>(TexVol->AddressMode.GetValue()));
			}

			// Common texture properties (runtime — always available)
			Result["compression"] = CompressionSettingsToString(static_cast<TextureCompressionSettings>(Texture->CompressionSettings.GetValue()));
			Result["srgb"] = static_cast<bool>(Texture->SRGB);
			Result["filter"] = FilterToString(static_cast<TextureFilter>(Texture->Filter.GetValue()));
			Result["lod_group"] = TCHAR_TO_UTF8(UTexture::GetTextureGroupString(static_cast<TextureGroup>(Texture->LODGroup.GetValue())));
			Result["lod_bias"] = Texture->LODBias;
			Result["virtual_texture_streaming"] = static_cast<bool>(Texture->VirtualTextureStreaming);
			Result["availability"] = AvailabilityToString(Texture->Availability);
			Result["mip_load_options"] = MipLoadOptionsToString(Texture->MipLoadOptions);
			Result["cook_tiling"] = CookTilingToString(static_cast<TextureCookPlatformTilingSettings>(Texture->CookPlatformTilingSettings.GetValue()));
			Result["oodle_preserve_extremes"] = Texture->bOodlePreserveExtremes;
			Result["downscale_options"] = DownscaleOptionsToString(Texture->DownscaleOptions);

#if WITH_EDITORONLY_DATA
			Result["max_texture_size"] = Texture->MaxTextureSize;
			Result["mip_gen"] = MipGenToString(static_cast<TextureMipGenSettings>(Texture->MipGenSettings.GetValue()));
			Result["lossy_compression_amount"] = LossyCompressionToString(static_cast<ETextureLossyCompressionAmount>(Texture->LossyCompressionAmount.GetValue()));
			Result["compression_quality"] = CompressionQualityToString(static_cast<ETextureCompressionQuality>(Texture->CompressionQuality.GetValue()));
			Result["compression_no_alpha"] = static_cast<bool>(Texture->CompressionNoAlpha);
			Result["compression_force_alpha"] = static_cast<bool>(Texture->CompressionForceAlpha);
			Result["compression_none"] = static_cast<bool>(Texture->CompressionNone);
			Result["power_of_two_mode"] = PowerOfTwoToString(static_cast<ETexturePowerOfTwoSetting::Type>(Texture->PowerOfTwoMode.GetValue()));

			// Adjustments
			Result["adjust_brightness"] = Texture->AdjustBrightness;
			Result["adjust_brightness_curve"] = Texture->AdjustBrightnessCurve;
			Result["adjust_vibrance"] = Texture->AdjustVibrance;
			Result["adjust_saturation"] = Texture->AdjustSaturation;
			Result["adjust_rgb_curve"] = Texture->AdjustRGBCurve;
			Result["adjust_hue"] = Texture->AdjustHue;
			Result["adjust_min_alpha"] = Texture->AdjustMinAlpha;
			Result["adjust_max_alpha"] = Texture->AdjustMaxAlpha;

			// Flags
			Result["flip_green_channel"] = static_cast<bool>(Texture->bFlipGreenChannel);
			Result["normalize_normals"] = static_cast<bool>(Texture->bNormalizeNormals);
			Result["preserve_border"] = static_cast<bool>(Texture->bPreserveBorder);
			Result["use_new_mip_filter"] = Texture->bUseNewMipFilter;
			Result["use_legacy_gamma"] = static_cast<bool>(Texture->bUseLegacyGamma);
			Result["alpha_coverage"] = Texture->bDoScaleMipsForAlphaCoverage;

			// Chroma key
			Result["chroma_key"] = Texture->bChromaKeyTexture;
			Result["chroma_key_threshold"] = Texture->ChromaKeyThreshold;
			{
				sol::table CK = Lua.create_table();
				CK["r"] = Texture->ChromaKeyColor.R;
				CK["g"] = Texture->ChromaKeyColor.G;
				CK["b"] = Texture->ChromaKeyColor.B;
				CK["a"] = Texture->ChromaKeyColor.A;
				Result["chroma_key_color"] = CK;
			}

			// Composite texture
			Result["composite_texture_mode"] = CompositeTextureToString(static_cast<ECompositeTextureMode>(Texture->CompositeTextureMode.GetValue()));
			Result["composite_power"] = Texture->CompositePower;
			UTexture* CompTex = Texture->GetCompositeTexture();
			if (CompTex)
			{
				Result["composite_texture"] = TCHAR_TO_UTF8(*CompTex->GetPathName());
			}

			// Source texture info
			if (Texture->Source.IsValid())
			{
				Result["source_width"] = Texture->Source.GetSizeX();
				Result["source_height"] = Texture->Source.GetSizeY();
				Result["source_num_mips"] = Texture->Source.GetNumMips();
				Result["source_num_slices"] = Texture->Source.GetNumSlices();
				Result["source_num_layers"] = Texture->Source.GetNumLayers();
				ETextureSourceFormat Fmt = Texture->Source.GetFormat();
				Result["source_format"] = TCHAR_TO_UTF8(*StaticEnum<ETextureSourceFormat>()->GetNameStringByValue(static_cast<int64>(Fmt)));
			}

			// Color management settings
			{
				sol::table ColorInfo = Lua.create_table();
				ColorInfo["color_space"] = TCHAR_TO_UTF8(*StaticEnum<ETextureColorSpace>()->GetNameStringByValue(static_cast<int64>(Texture->SourceColorSettings.ColorSpace)));
				ColorInfo["encoding_override"] = TCHAR_TO_UTF8(*StaticEnum<ETextureSourceEncoding>()->GetNameStringByValue(static_cast<int64>(Texture->SourceColorSettings.EncodingOverride)));
				ColorInfo["chromatic_adaptation"] = TCHAR_TO_UTF8(*StaticEnum<ETextureChromaticAdaptationMethod>()->GetNameStringByValue(static_cast<int64>(Texture->SourceColorSettings.ChromaticAdaptationMethod)));
				Result["color_settings"] = ColorInfo;
			}
#endif

			Session.Log(FString::Printf(TEXT("[OK] info() -> %s (%s)"),
				*Texture->GetName(), *Texture->GetClass()->GetName()));
			return Result;
		});

		// ================================================================
		// list(type)
		// ================================================================
		AssetObj.set_function("list", [Texture, &Session](sol::table Self,
			sol::optional<std::string> TypeOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			if (!IsValid(Texture)) { Session.Log(TEXT("[FAIL] list -> asset no longer valid")); return sol::lua_nil; }
			FString FType = NeoLuaStr::ToFStringOpt(TypeOpt, TEXT("all"));

			if (FType.Equals(TEXT("all"), ESearchCase::IgnoreCase))
			{
				sol::protected_function InfoFn = Self["info"];
				if (InfoFn.valid()) return InfoFn(Self);
				return sol::lua_nil;
			}

			if (FType.Equals(TEXT("settings"), ESearchCase::IgnoreCase))
			{
				sol::table R = Lua.create_table();
				R["compression"] = CompressionSettingsToString(static_cast<TextureCompressionSettings>(Texture->CompressionSettings.GetValue()));
				R["srgb"] = static_cast<bool>(Texture->SRGB);
				R["filter"] = FilterToString(static_cast<TextureFilter>(Texture->Filter.GetValue()));
				R["lod_group"] = TCHAR_TO_UTF8(UTexture::GetTextureGroupString(static_cast<TextureGroup>(Texture->LODGroup.GetValue())));
				R["lod_bias"] = Texture->LODBias;
				R["virtual_texture_streaming"] = static_cast<bool>(Texture->VirtualTextureStreaming);
				R["availability"] = AvailabilityToString(Texture->Availability);
				R["mip_load_options"] = MipLoadOptionsToString(Texture->MipLoadOptions);
				R["cook_tiling"] = CookTilingToString(static_cast<TextureCookPlatformTilingSettings>(Texture->CookPlatformTilingSettings.GetValue()));

				UTexture2D* Tex2D = Cast<UTexture2D>(Texture);
				if (Tex2D)
				{
					R["address_x"] = AddressToString(static_cast<TextureAddress>(Tex2D->AddressX.GetValue()));
					R["address_y"] = AddressToString(static_cast<TextureAddress>(Tex2D->AddressY.GetValue()));
				}
				UVolumeTexture* TexVol = Cast<UVolumeTexture>(Texture);
				if (TexVol)
				{
					R["address_mode"] = AddressToString(static_cast<TextureAddress>(TexVol->AddressMode.GetValue()));
				}

#if WITH_EDITORONLY_DATA
				R["max_texture_size"] = Texture->MaxTextureSize;
				R["mip_gen"] = MipGenToString(static_cast<TextureMipGenSettings>(Texture->MipGenSettings.GetValue()));
#endif

				Session.Log(TEXT("[OK] list(\"settings\")"));
				return R;
			}

			if (FType.Equals(TEXT("adjustments"), ESearchCase::IgnoreCase))
			{
				sol::table R = Lua.create_table();
#if WITH_EDITORONLY_DATA
				R["brightness"] = Texture->AdjustBrightness;
				R["brightness_curve"] = Texture->AdjustBrightnessCurve;
				R["vibrance"] = Texture->AdjustVibrance;
				R["saturation"] = Texture->AdjustSaturation;
				R["rgb_curve"] = Texture->AdjustRGBCurve;
				R["hue"] = Texture->AdjustHue;
				R["min_alpha"] = Texture->AdjustMinAlpha;
				R["max_alpha"] = Texture->AdjustMaxAlpha;
#endif
				Session.Log(TEXT("[OK] list(\"adjustments\")"));
				return R;
			}

			if (FType.Equals(TEXT("advanced"), ESearchCase::IgnoreCase))
			{
				sol::table R = Lua.create_table();
				R["oodle_preserve_extremes"] = Texture->bOodlePreserveExtremes;
				R["downscale_options"] = DownscaleOptionsToString(Texture->DownscaleOptions);
#if WITH_EDITORONLY_DATA
				R["lossy_compression_amount"] = LossyCompressionToString(static_cast<ETextureLossyCompressionAmount>(Texture->LossyCompressionAmount.GetValue()));
				R["compression_quality"] = CompressionQualityToString(static_cast<ETextureCompressionQuality>(Texture->CompressionQuality.GetValue()));
				R["compression_no_alpha"] = static_cast<bool>(Texture->CompressionNoAlpha);
				R["compression_force_alpha"] = static_cast<bool>(Texture->CompressionForceAlpha);
				R["compression_none"] = static_cast<bool>(Texture->CompressionNone);
				R["power_of_two_mode"] = PowerOfTwoToString(static_cast<ETexturePowerOfTwoSetting::Type>(Texture->PowerOfTwoMode.GetValue()));
				R["use_new_mip_filter"] = Texture->bUseNewMipFilter;
				R["use_legacy_gamma"] = static_cast<bool>(Texture->bUseLegacyGamma);
				R["alpha_coverage"] = Texture->bDoScaleMipsForAlphaCoverage;
				R["composite_texture_mode"] = CompositeTextureToString(static_cast<ECompositeTextureMode>(Texture->CompositeTextureMode.GetValue()));
				R["composite_power"] = Texture->CompositePower;
#endif
				Session.Log(TEXT("[OK] list(\"advanced\")"));
				return R;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: settings, adjustments, advanced"), *FType));
			return sol::lua_nil;
		});

		// NOTE: configure(params) removed 2026-04-20 — UTexture::PostEditChangeProperty
		// already handles UpdateResource, ValidateSettingsAfterImportOrEdit,
		// NotifyMaterials, and LODGroup-driven property resets
		// (Engine/Source/Runtime/Engine/Private/Texture.cpp:822). Use asset:set for
		// every configure() key that was here (CompressionSettings, SRGB, Filter,
		// LODGroup, LODBias, VirtualTextureStreaming, Availability, MipLoadOptions,
		// MaxTextureSize, MipGenSettings, PowerOfTwoMode, NeverStream, CompositeTexture,
		// AdjustBrightness/Saturation/.../Gamma, encoding_override, AddressX/Y/Mode, etc.).
	});
}

REGISTER_LUA_BINDING(Texture, TextureDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindTexture(Lua, Session);
});
