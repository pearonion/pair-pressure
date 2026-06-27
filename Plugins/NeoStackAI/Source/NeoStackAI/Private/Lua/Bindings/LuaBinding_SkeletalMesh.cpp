// Copyright 2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Lua/LuaPropertyHelper.h"
#include "Lua/LuaPropertyTable.h"
#include "Lua/LuaAssetResolver.h"
#include "Lua/LuaStr.h"
#include "Tools/NeoStackToolUtils.h"

#include "Editor.h"
#include "SkeletalMeshEditorSubsystem.h"
#include "AssetCompilingManager.h"

#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Engine/Texture2D.h"
#include "Components/SkinnedMeshComponent.h"
#include "Animation/AnimInstance.h"
#include "Animation/Skeleton.h"
#include "Animation/BoneReference.h"
#include "Animation/MorphTarget.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
#include "Rendering/SkinWeightProfile.h"
#else
#include "Animation/SkinWeightProfile.h"
#endif
#include "PhysicsEngine/PhysicsAsset.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "ClothingAssetBase.h"
#include "Animation/MeshDeformer.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
#include "Animation/MeshDeformerCollection.h"
#endif
#include "UObject/UnrealType.h"
#include "UObject/PropertyAccessUtil.h"
#include "UObject/UObjectIterator.h"
#include "ScopedTransaction.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ============================================================================
// BINDING
// ============================================================================

// Accessor for the editor-only subsystem. Returns nullptr under -run or commandlet
// launches where GEditor is null; callers must handle that.
static USkeletalMeshEditorSubsystem* GetSkelMeshEditorSubsystem()
{
	return GEditor ? GEditor->GetEditorSubsystem<USkeletalMeshEditorSubsystem>() : nullptr;
}

static const TArray<UClothingAssetBase*>& GetMeshClothingAssetsReadOnly(const USkeletalMesh* Mesh)
{
	return Mesh->GetMeshClothingAssets();
}

static const TCHAR* SkeletalMeshForwardAxisToString(EAxis::Type Axis)
{
	switch (Axis)
	{
	case EAxis::X: return TEXT("X");
	case EAxis::Y: return TEXT("Y");
	case EAxis::Z: return TEXT("Z");
	default: return TEXT("None");
	}
}

static bool ParseSkeletalMeshForwardAxis(const FString& AxisString, EAxis::Type& OutAxis)
{
	if (AxisString.Equals(TEXT("X"), ESearchCase::IgnoreCase))
	{
		OutAxis = EAxis::X;
		return true;
	}
	if (AxisString.Equals(TEXT("Y"), ESearchCase::IgnoreCase))
	{
		OutAxis = EAxis::Y;
		return true;
	}
	if (AxisString.Equals(TEXT("Z"), ESearchCase::IgnoreCase))
	{
		OutAxis = EAxis::Z;
		return true;
	}
	if (AxisString.Equals(TEXT("None"), ESearchCase::IgnoreCase))
	{
		OutAxis = EAxis::None;
		return true;
	}
	return false;
}

static TArray<FLuaFunctionDoc> SkeletalMeshDocs = {
	{ TEXT("open_asset(path):info()"),
	  TEXT("Summary: bones/LODs/materials/morph-targets/sockets counts, skeleton, physics, bounds, nanite, overlay material, mesh deformer, clothing."),
	  TEXT("table") },
	{ TEXT("open_asset(path):list(type, opts?)"),
	  TEXT("type ∈ bones|materials|sockets|morph_targets|lods|clothing|generated_morph_targets|sections|clothing_in_use|section_clothing|clothing_active_for_lod|skin_weight_profiles|source_models|vertex_colors. bones supports {tree=true}|{transforms=true}; sections/section_clothing take {lod[,section]}; skin_weight_profiles supports {details=true}; vertex_colors takes {lod?=0}. list(lods)/list(sections) resolve material_index through LODInfo->LODMaterialMap."),
	  TEXT("table") },
	{ TEXT("open_asset(path):add(type, params)"),
	  TEXT("type ∈ lod|socket|clothing_asset|skin_weight_profile. Socket: {name, bone, location?, rotation?, scale?, force_always_animated?}. Clothing: {path=\"/Game/.../ClothingAsset\"} (asset must be outered to this mesh). Skin weight profile: {name, default_profile?, default_profile_from_lod?}."),
	  TEXT("table|nil") },
	{ TEXT("open_asset(path):remove(type, id)"),
	  TEXT("type ∈ lod|socket|morph_target|clothing_asset|section. id is LOD index, socket/morph-target name, or {lod, section} table for clothing_asset / section. remove(morph_target) goes through RemoveMorphTargets (cleans mesh-description + imported-source filename). remove(section) rejects if clothing is bound — call remove(\"clothing_asset\") first."),
	  TEXT("table|nil") },
	{ TEXT("open_asset(path):configure(type, id, params)"),
	  TEXT("type ∈ lod|material|socket|section|physics|nanite|skeleton|lod_settings|bounds|post_process_anim_bp|animating_rig|ray_tracing|overlay_material|mesh_deformer|floor_offset|morph_target|target_mesh_deformers|forward_axis. Physics uses AssignPhysicsAsset compatibility check; section edits go through USkeletalMeshEditorSubsystem::SetSection*. forward_axis only accepts no-op confirmation because UE stores it as MeshDescription orientation metadata."),
	  TEXT("table|nil") },
	{ TEXT("open_asset(path):build(opts?)"),
	  TEXT("Rebuild the mesh after mutations. opts: {wait?=true}. Default blocks on async build via FAssetCompilingManager; pass {wait=false} to return immediately with queued=true."),
	  TEXT("table|nil") },
	{ TEXT("open_asset(path):rename_socket(old_name, new_name)"),
	  TEXT("Rename socket via USkeletalMeshEditorSubsystem::RenameSocket — updates preview attachments."),
	  TEXT("table|nil") },
	{ TEXT("open_asset(path):rename_morph_target(old_name, new_name)"),
	  TEXT("Rename a morph target via USkeletalMesh::RenameMorphTarget — preserves mesh-description morph attributes and imported source filenames."),
	  TEXT("table|nil") },
	{ TEXT("open_asset(path):import_lod(lod_index, source_filename)"),
	  TEXT("Import or re-import FBX as an LOD. Empty filename re-imports from the LODInfo.SourceImportFilename."),
	  TEXT("table|nil") },
	{ TEXT("open_asset(path):reimport_all_custom_lods()"),
	  TEXT("Re-import every custom LOD from its recorded source file."),
	  TEXT("table|nil") },
	{ TEXT("open_asset(path):strip_lod_geometry(lod_index, texture_mask_path, threshold)"),
	  TEXT("Strip triangles whose UV0 samples non-black pixels on TextureMask. threshold = compare-with-zero tolerance."),
	  TEXT("table|nil") },
	{ TEXT("open_asset(path):create_physics_asset(params?)"),
	  TEXT("Create a new PhysicsAsset for this mesh. params: {set_to_mesh?=true, lod_index?=0}. Returns {path}."),
	  TEXT("table|nil") },
	{ TEXT("open_asset(path):regenerate_lods(params?)"),
	  TEXT("Regenerate LODs. params: {new_lod_count?=0 keeps current, regenerate_imported?=false, generate_base?=false}."),
	  TEXT("table|nil") },
	{ TEXT("open_asset(path):remove_legacy_clothing_sections()"),
	  TEXT("Restore duplicated clothing render sections back into the original mesh (legacy clothing migration helper)."),
	  TEXT("table|nil") },
	{ TEXT("open_asset(path):release_skin_weight_profile_resources(opts?)"),
	  TEXT("Release allocated skin-weight-profile GPU resources only with {unsafe_confirm_no_active_users=true}; rejects compiling meshes and live component users."),
	  TEXT("table|nil") },
	{ TEXT("open_asset(path):has_mesh_description(lod)"),
	  TEXT("Cheap check (no unpack): does this LOD have an editable FMeshDescription?"),
	  TEXT("table|nil") },
	{ TEXT("open_asset(path):modify_mesh_description(lod, always_dirty?=true)"),
	  TEXT("Store the LOD's mesh description bulk data in the current transaction for undo/redo before an external edit."),
	  TEXT("table|nil") },
	{ TEXT("open_asset(path):commit_mesh_description(lod, opts?)"),
	  TEXT("Commit the mesh description for this LOD to bulk storage. opts control morph_targets/skin_weight_profiles/vertex_attributes/vertex_colors/mark_package_dirty/force_update flags on FCommitMeshDescriptionParams."),
	  TEXT("table|nil") },
	{ TEXT("open_asset(path):clear_mesh_description(lod?)"),
	  TEXT("Clear stored mesh description(s). Pass a lod index or omit to clear all LODs."),
	  TEXT("table|nil") },
	{ TEXT("open_asset(path):set_num_source_models(count)"),
	  TEXT("Resize the LOD source-model array (>=1). Fewer than current discards upper LODs; more appends empty LODs."),
	  TEXT("table|nil") },
	{ TEXT("open_asset(path):get_clothing_asset(index_or_name)"),
	  TEXT("Returns cloth sub-object with get(prop), set(prop, value_any), list_properties(filter?). set() accepts typed Lua values (tables → FVector/FLinearColor via registered struct handlers)."),
	  TEXT("table|nil") },
};

// ============================================================================
// Helpers local to this binding
// ============================================================================

// Resolve a weak mesh pointer and early-return with a [FAIL] log when the
// asset has been GC'd/unloaded. Mirrors the pattern from LuaBinding_OpenAsset.cpp.
#define NS_RESOLVE_MESH_OR_FAIL(OpName) \
	USkeletalMesh* Mesh = WeakMesh.Get(); \
	if (!Mesh) \
	{ \
		Session.Log(TEXT("[FAIL] " OpName " -> asset no longer valid. Reopen it with open_asset(path).")); \
		return sol::lua_nil; \
	}

static void BindSkeletalMesh(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_skeletal_mesh", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = NeoLuaStr::ToFString(PathStr);
		USkeletalMesh* InitialMesh = NeoLuaAsset::ResolveWithRegistry<USkeletalMesh>(FPath);
		if (!InitialMesh) return;

		// GC-safe weak reference — all lambdas capture this instead of the raw pointer.
		// See LuaBinding_OpenAsset.cpp for the matching pattern.
		TWeakObjectPtr<USkeletalMesh> WeakMesh(InitialMesh);

		// ---- help text ----
		AssetObj["_help_text"] =
			"SkeletalMesh enrichment:\n"
			"\n"
			"info() — comprehensive summary\n"
			"\n"
			"list(type, opts?):\n"
			"  list(\"bones\")                        — flat array: {index, name, parent_index, parent_name}\n"
			"  list(\"bones\", {tree=true})            — hierarchical tree with nested children\n"
			"  list(\"bones\", {transforms=true})      — flat list with reference pose transform\n"
			"  list(\"materials\")                     — {index, slot_name, material_path, material_name, overlay_material_path}\n"
			"  list(\"sockets\")                       — {name, bone_name, location, rotation, scale, source}\n"
			"  list(\"morph_targets\")                  — {index, name}\n"
			"  list(\"generated_morph_targets\")        — names flagged as generated-by-engine (via subsystem)\n"
			"  list(\"lods\")                          — per-LOD stats including sections\n"
			"  list(\"sections\", {lod=N})              — per-section detail for one LOD (cast_shadow, recompute_tangent, visible_in_ray_tracing via subsystem)\n"
			"  list(\"clothing\")                      — clothing assets\n"
			"  list(\"vertex_colors\", {lod?=0})        — painted vertex color rows {x,y,z,r,g,b,a} from GetVertexColorData\n"
			"\n"
			"add(type, params):\n"
			"  add(\"lod\")                            — add auto-generated LOD level\n"
			"  add(\"socket\", {name, bone, location?, rotation?, scale?, force_always_animated?})\n"
			"  add(\"clothing_asset\", {path=\"/Game/.../Cloth\"})\n"
			"  add(\"skin_weight_profile\", {name, default_profile?, default_profile_from_lod?})\n"
			"\n"
			"remove(type, id):\n"
			"  remove(\"lod\", index)                  — goes through USkeletalMeshEditorSubsystem::RemoveLODs\n"
			"  remove(\"socket\", name)                — mesh-only; edit Skeleton to remove skeleton sockets\n"
			"  remove(\"morph_target\", name)           — calls RemoveMorphTargets (cleans mesh description + imported source filename + transient rename)\n"
			"  remove(\"clothing_asset\", {lod, section}) — unbinds cloth at the given LOD section; rejects if nothing is bound\n"
			"  remove(\"section\", {lod, section})       — destructive: deletes an imported LOD section; reject if clothing is bound (unbind first)\n"
			"\n"
			"configure(type, id, params):\n"
			"  configure(\"lod\", index, {screen_size?, lod_hysteresis?, reduction={...}, build_settings={...}, bones_to_remove?, bones_to_prioritize?, weight_of_prioritization?})\n"
			"  configure(\"material\", index, {material=\"/Path\", slot_name?, overlay_material?=\"/Path\"|\"None\"})\n"
			"  configure(\"socket\", name, {bone?, location?, rotation?, scale?}) — mesh-owned only\n"
			"  configure(\"section\", {lod, section, cast_shadow?, recompute_tangent?, recompute_tangents_vertex_mask_channel?, visible_in_ray_tracing?})\n"
			"  configure(\"physics\", {physics_asset?, shadow_physics_asset?, enable_per_poly_collision?}) — physics_asset uses AssignPhysicsAsset compatibility check\n"
			"  configure(\"nanite\", nil, {enabled=true, explicit_tangents?, lerp_uvs?, separable?, voxel_ndf?})\n"
			"  configure(\"skeleton\", nil, {skeleton=\"/Game/Path/MySkeleton\"})\n"
			"  configure(\"lod_settings\", nil, {min_lod=2, disable_below_min_lod_stripping=true})\n"
			"  configure(\"bounds\", nil, {positive_extension={x,y,z}, negative_extension={x,y,z}})\n"
			"  configure(\"post_process_anim_bp\", nil, {blueprint=\"/Game/Path/AnimBP\"|\"None\", lod_threshold?})\n"
			"  configure(\"animating_rig\", nil, {rig=\"/Game/Path/ControlRig\"|\"None\"})\n"
			"  configure(\"ray_tracing\", nil, {enabled=true, min_lod?})\n"
			"  configure(\"overlay_material\", nil, {material=\"/Path\"|\"None\", max_draw_distance?})\n"
			"  configure(\"mesh_deformer\", nil, {deformer=\"/Path\"|\"None\"})\n"
			"  configure(\"floor_offset\", nil, {offset=0.0})\n"
			"  configure(\"morph_target\", nil, {generated_by_engine=true, names={...}?})\n"
			"\n"
			"build() — rebuild mesh after changes\n"
			"\n"
			"Editor-subsystem operations:\n"
			"  rename_socket(old_name, new_name)\n"
			"  rename_morph_target(old_name, new_name)\n"
			"  import_lod(lod_index, source_filename) — empty string re-imports from LODInfo.SourceImportFilename\n"
			"  reimport_all_custom_lods()\n"
			"  strip_lod_geometry(lod_index, texture_mask_path, threshold)\n"
			"  create_physics_asset({set_to_mesh?=true, lod_index?=0})\n"
			"  regenerate_lods({new_lod_count?=0, regenerate_imported?=false, generate_base?=false})\n"
			"  remove_legacy_clothing_sections()\n"
			"  release_skin_weight_profile_resources({unsafe_confirm_no_active_users=true})\n"
			"\n"
			"Source-model / MeshDescription ops:\n"
			"  list(\"source_models\")                  — {index, is_valid, has_mesh_description, is_simplified, screen_size}\n"
			"  has_mesh_description(lod)               — bool check (cheap, no unpack)\n"
			"  modify_mesh_description(lod, always_dirty?=true) — store for undo/redo before external edit\n"
			"  commit_mesh_description(lod, {update_morph_targets?, update_skin_weight_profiles?, update_vertex_attributes?, update_vertex_colors?, mark_package_dirty?, force_update?}?) — commit changes to bulk data\n"
			"  clear_mesh_description(lod?)            — clear one LOD or all LODs\n"
			"  set_num_source_models(count)            — resize LOD array (>=1)\n"
			"\n"
			"These wrappers are the canonical high-level API — they validate inputs, resolve\n"
			"asset paths, and reshape returns for Lua callers. The generic reflection globals\n"
			"invoke(subsystem(\"SkeletalMeshEditorSubsystem\"), \"Func\", {...}) / invoke(mesh, ...)\n"
			"still reach the same BlueprintCallable UFUNCTIONs but skip the local validation,\n"
			"path-resolution and compat-check layers. Prefer the named verbs above.\n";

		// ================================================================
		// add(type, params)
		// ================================================================
		AssetObj.set_function("add", [WeakMesh, &Session](sol::table /*Self*/,
			std::string TypeStr, sol::optional<sol::table> ParamsOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			NS_RESOLVE_MESH_OR_FAIL("add");
			FString FType = NeoLuaStr::ToFString(TypeStr);

			// ---- add("lod") ----
			if (FType.Equals(TEXT("lod"), ESearchCase::IgnoreCase))
			{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
				Mesh->Modify();
				int32 NewIdx = Mesh->GetLODNum();
				Mesh->AddSourceModel(true);
				FSkeletalMeshLODInfo* LODInfo = Mesh->GetLODInfo(NewIdx);
				if (LODInfo)
				{
					FSkeletalMeshLODInfo* PrevInfo = (NewIdx > 0) ? Mesh->GetLODInfo(NewIdx - 1) : nullptr;
					if (PrevInfo)
					{
						LODInfo->ScreenSize = FPerPlatformFloat(FMath::Max(0.01f, PrevInfo->ScreenSize.GetDefault() * 0.5f));
						LODInfo->ReductionSettings = PrevInfo->ReductionSettings;
						LODInfo->ReductionSettings.NumOfTrianglesPercentage = FMath::Max(0.1, PrevInfo->ReductionSettings.NumOfTrianglesPercentage * 0.5);
					}
				}
				Mesh->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"lod\") -> LOD %d added (total: %d)"), NewIdx, Mesh->GetLODNum()));
				sol::table R = Lua.create_table();
				R["index"] = NewIdx;
				R["total_lods"] = Mesh->GetLODNum();
				return R;
#else
				sol::table R = Lua.create_table();
				R["error"] = "AddSourceModel requires UE 5.7+";
				return R;
#endif
			}

			// ---- add("socket", {name, bone, ...}) ----
			if (FType.Equals(TEXT("socket"), ESearchCase::IgnoreCase))
			{
				if (!ParamsOpt.has_value()) { Session.Log(TEXT("[FAIL] add(\"socket\") -> params required: {name, bone}")); return sol::lua_nil; }
				sol::table P = ParamsOpt.value();
				std::string Name = P.get_or<std::string>("name", "");
				std::string BoneName = P.get_or<std::string>("bone", "");
				if (Name.empty()) { Session.Log(TEXT("[FAIL] add(\"socket\") -> 'name' is required")); return sol::lua_nil; }
				if (BoneName.empty()) { Session.Log(TEXT("[FAIL] add(\"socket\") -> 'bone' is required")); return sol::lua_nil; }

				FName BoneFName = FName(NeoLuaStr::ToFString(BoneName));
				const FReferenceSkeleton& RefSkel = Mesh->GetRefSkeleton();
				const bool bBoneExists = (RefSkel.FindBoneIndex(BoneFName) != INDEX_NONE);

				// Pre-check for duplicate. USkeletalMesh::AddSocket only guards against mesh-owned
				// duplicates (not skeleton sockets — overriding a skeleton socket on the mesh is
				// a legitimate use-case), so we match that rule here.
				FName WantedSocket = FName(NeoLuaStr::ToFString(Name));
				bool bDuplicateOnMesh = false;
				for (USkeletalMeshSocket* Existing : Mesh->GetMeshOnlySocketList())
				{
					if (Existing && Existing->SocketName == WantedSocket) { bDuplicateOnMesh = true; break; }
				}

				if (!bBoneExists)
				{
					Session.Log(FString::Printf(
						TEXT("[FAIL] add(\"socket\", \"%s\") -> bone \"%s\" not in reference skeleton. Call list(\"bones\") for valid bone names."),
						*NeoLuaStr::ToFString(Name), *BoneFName.ToString()));
					return sol::lua_nil;
				}
				if (bDuplicateOnMesh)
				{
					Session.Log(FString::Printf(
						TEXT("[FAIL] add(\"socket\", \"%s\") -> socket with this name already exists. Call list(\"sockets\") or remove(\"socket\", \"%s\") first."),
						*NeoLuaStr::ToFString(Name), *NeoLuaStr::ToFString(Name)));
					return sol::lua_nil;
				}

				USkeletalMeshSocket* Socket = NewObject<USkeletalMeshSocket>(Mesh);
				Socket->SocketName = WantedSocket;
				Socket->BoneName = BoneFName;

				sol::optional<sol::table> LocOpt = P.get<sol::optional<sol::table>>("location");
				if (LocOpt.has_value())
				{
					Socket->RelativeLocation.X = LocOpt.value().get_or("x", 0.0);
					Socket->RelativeLocation.Y = LocOpt.value().get_or("y", 0.0);
					Socket->RelativeLocation.Z = LocOpt.value().get_or("z", 0.0);
				}
				sol::optional<sol::table> RotOpt = P.get<sol::optional<sol::table>>("rotation");
				if (RotOpt.has_value())
				{
					Socket->RelativeRotation.Pitch = RotOpt.value().get_or("pitch", 0.0);
					Socket->RelativeRotation.Yaw = RotOpt.value().get_or("yaw", 0.0);
					Socket->RelativeRotation.Roll = RotOpt.value().get_or("roll", 0.0);
				}
				sol::optional<sol::table> ScaleOpt = P.get<sol::optional<sol::table>>("scale");
				if (ScaleOpt.has_value())
				{
					Socket->RelativeScale.X = ScaleOpt.value().get_or("x", 1.0);
					Socket->RelativeScale.Y = ScaleOpt.value().get_or("y", 1.0);
					Socket->RelativeScale.Z = ScaleOpt.value().get_or("z", 1.0);
				}
				if (P.get<sol::optional<bool>>("force_always_animated").has_value())
					Socket->bForceAlwaysAnimated = P.get<bool>("force_always_animated");

				// USkeletalMesh::AddSocket opens its own scoped transaction + Modify() internally.
				const int32 BeforeCount = Mesh->GetMeshOnlySocketList().Num();
				Mesh->AddSocket(Socket);
				const int32 AfterCount = Mesh->GetMeshOnlySocketList().Num();
				if (AfterCount != BeforeCount + 1 || Mesh->FindSocket(WantedSocket) != Socket)
				{
					Session.Log(FString::Printf(
						TEXT("[FAIL] add(\"socket\", \"%s\") -> engine rejected the socket (check log for USkeletalMesh error). Typically: duplicate name or bone \"%s\" missing after validation."),
						*NeoLuaStr::ToFString(Name), *BoneFName.ToString()));
					return sol::lua_nil;
				}
				Session.Log(FString::Printf(TEXT("[OK] add(\"socket\") -> \"%s\" on bone \"%s\""),
					*Socket->SocketName.ToString(), *Socket->BoneName.ToString()));
				sol::table R = Lua.create_table();
				R["name"] = Name;
				R["bone"] = BoneName;
				return R;
			}

			// ---- add("clothing_asset", {path}) — Finding 5 ----
			if (FType.Equals(TEXT("clothing_asset"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("clothing"), ESearchCase::IgnoreCase))
			{
				if (!ParamsOpt.has_value())
				{
					Session.Log(TEXT("[FAIL] add(\"clothing_asset\") -> params required: {path=\"/Game/.../ClothingAsset\"}"));
					return sol::lua_nil;
				}
				sol::table P = ParamsOpt.value();
				std::string PathStr = P.get_or<std::string>("path", "");
				if (PathStr.empty())
				{
					Session.Log(TEXT("[FAIL] add(\"clothing_asset\") -> 'path' is required"));
					return sol::lua_nil;
				}
				UClothingAssetBase* NewAsset = NeoLuaAsset::ResolveWithRegistry<UClothingAssetBase>(NeoLuaStr::ToFString(PathStr));
				if (!NewAsset)
				{
					Session.Log(FString::Printf(
						TEXT("[FAIL] add(\"clothing_asset\", \"%s\") -> asset not found or not a UClothingAssetBase. Use find_assets(\"/Game\", {class=\"ClothingAssetCommon\"}) to discover valid paths."),
						*NeoLuaStr::ToFString(PathStr)));
					return sol::lua_nil;
				}
				const int32 BeforeCount = Mesh->GetMeshClothingAssets().Num();
				Mesh->Modify();
				Mesh->AddClothingAsset(NewAsset);
				// USkeletalMesh::AddClothingAsset silently no-ops when NewAsset->GetOuter() != Mesh.
				// Regular content assets resolved by path have a package outer and get rejected.
				const bool bAdded = Mesh->GetMeshClothingAssets().Contains(NewAsset)
				                 && Mesh->GetMeshClothingAssets().Num() > BeforeCount;
				if (!bAdded)
				{
					Session.Log(FString::Printf(
						TEXT("[FAIL] add(\"clothing_asset\", \"%s\") -> engine rejected the asset because clothing assets must be outered to this SkeletalMesh. Use the Skeletal Mesh Editor's Clothing panel to import or duplicate the cloth into this mesh's outer."),
						*NeoLuaStr::ToFString(PathStr)));
					return sol::lua_nil;
				}
				Mesh->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"clothing_asset\") -> \"%s\""), *NewAsset->GetName()));
				sol::table R = Lua.create_table();
				R["name"] = TCHAR_TO_UTF8(*NewAsset->GetName());
				R["guid"] = TCHAR_TO_UTF8(*NewAsset->GetAssetGuid().ToString());
				R["total_clothing_assets"] = Mesh->GetMeshClothingAssets().Num();
				return R;
			}

			// ---- add("skin_weight_profile", {name, default_profile?, default_profile_from_lod?}) — round-3 Finding 4 ----
			if (FType.Equals(TEXT("skin_weight_profile"), ESearchCase::IgnoreCase))
			{
				if (!ParamsOpt.has_value())
				{
					Session.Log(TEXT("[FAIL] add(\"skin_weight_profile\") -> params required: {name, default_profile?, default_profile_from_lod?}"));
					return sol::lua_nil;
				}
				sol::table P = ParamsOpt.value();
				std::string NameStr = P.get_or<std::string>("name", "");
				if (NameStr.empty())
				{
					Session.Log(TEXT("[FAIL] add(\"skin_weight_profile\") -> 'name' is required"));
					return sol::lua_nil;
				}
				FName ProfileName(*NeoLuaStr::ToFString(NameStr));
				// Duplicate guard: AddSkinWeightProfile has no built-in rejection for dup names.
				for (const FSkinWeightProfileInfo& Existing : Mesh->GetSkinWeightProfiles())
				{
					if (Existing.Name == ProfileName)
					{
						Session.Log(FString::Printf(
							TEXT("[FAIL] add(\"skin_weight_profile\", \"%s\") -> a profile with this name already exists. Call list(\"skin_weight_profiles\") to see existing names."),
							*ProfileName.ToString()));
						return sol::lua_nil;
					}
				}

				FSkinWeightProfileInfo NewInfo;
				NewInfo.Name = ProfileName;
				if (P.get<sol::optional<bool>>("default_profile").has_value())
					NewInfo.DefaultProfile = FPerPlatformBool(P.get<bool>("default_profile"));
				if (P.get<sol::optional<int>>("default_profile_from_lod").has_value())
					NewInfo.DefaultProfileFromLODIndex = FPerPlatformInt(P.get<int>("default_profile_from_lod"));

				Mesh->Modify();
				Mesh->AddSkinWeightProfile(NewInfo);
				Mesh->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] add(\"skin_weight_profile\") -> \"%s\""), *ProfileName.ToString()));
				sol::table R = Lua.create_table();
				R["name"] = TCHAR_TO_UTF8(*ProfileName.ToString());
				R["total_profiles"] = Mesh->GetSkinWeightProfiles().Num();
				return R;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: lod, socket, clothing_asset, skin_weight_profile"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// remove(type, id)
		// ================================================================
		AssetObj.set_function("remove", [WeakMesh, &Session](sol::table /*Self*/,
			std::string TypeStr, sol::object IdObj, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			NS_RESOLVE_MESH_OR_FAIL("remove");
			FString FType = NeoLuaStr::ToFString(TypeStr);

			// ---- remove("lod", index) ----
			if (FType.Equals(TEXT("lod"), ESearchCase::IgnoreCase))
			{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
				int32 Index = IdObj.as<int32>();
				if (Index <= 0)
				{
					Session.Log(TEXT("[FAIL] remove(\"lod\") -> cannot remove LOD 0. Call list(\"lods\") to see available indices."));
					return sol::lua_nil;
				}
				const int32 LodCount = Mesh->GetLODNum();
				if (Index >= LodCount)
				{
					Session.Log(FString::Printf(
						TEXT("[FAIL] remove(\"lod\", %d) -> index out of range (lod_count=%d). Call list(\"lods\") and retry with a valid nonzero LOD index."),
						Index, LodCount));
					return sol::lua_nil;
				}
				USkeletalMeshEditorSubsystem* Subsys = GetSkelMeshEditorSubsystem();
				if (!Subsys)
				{
					Session.Log(TEXT("[FAIL] remove(\"lod\") -> editor subsystem unavailable. Run inside the Unreal editor (not commandlet/cook) and retry."));
					return sol::lua_nil;
				}
				FSkeletalMeshModel* ImportedModel = Mesh->GetImportedModel();
				const int32 ImportedLodCount = ImportedModel ? ImportedModel->LODModels.Num() : 0;
				if (Index >= ImportedLodCount)
				{
					Mesh->Modify();
					Mesh->RemoveSourceModel(Index);
					Mesh->MarkPackageDirty();

					Session.Log(FString::Printf(TEXT("[OK] remove(\"lod\", %d) -> removed source-model-only LOD (remaining: %d)"), Index, Mesh->GetLODNum()));
					sol::table R = Lua.create_table();
					R["removed"] = Index;
					R["total_lods"] = Mesh->GetLODNum();
					return R;
				}
				TArray<int32> ToRemove; ToRemove.Add(Index);
				if (!USkeletalMeshEditorSubsystem::RemoveLODs(Mesh, ToRemove))
				{
					Session.Log(FString::Printf(
						TEXT("[FAIL] remove(\"lod\", %d) -> UE RemoveLODs failed. Verify the LOD exists, the mesh is not compiling, and no readonly source control lock is held."), Index));
					return sol::lua_nil;
				}
				Session.Log(FString::Printf(TEXT("[OK] remove(\"lod\", %d) -> remaining: %d"), Index, Mesh->GetLODNum()));
				sol::table R = Lua.create_table();
				R["removed"] = Index;
				R["total_lods"] = Mesh->GetLODNum();
				return R;
#else
				sol::table R = Lua.create_table();
				R["error"] = "RemoveLODs requires UE 5.7+";
				return R;
#endif
			}

			// ---- remove("socket", name) ----
			if (FType.Equals(TEXT("socket"), ESearchCase::IgnoreCase))
			{
				FString SocketName = NeoLuaStr::ToFString(IdObj.as<std::string>());
				USkeletalMeshSocket* Socket = Mesh->FindSocket(FName(*SocketName));
				if (!Socket)
				{
					Session.Log(FString::Printf(
						TEXT("[FAIL] remove(\"socket\", \"%s\") -> not found. Call list(\"sockets\") to see existing names."), *SocketName));
					return sol::lua_nil;
				}

				TArray<TObjectPtr<USkeletalMeshSocket>>& MeshSockets = Mesh->GetMeshOnlySocketList();
				bool bFound = false;
				for (int32 i = 0; i < MeshSockets.Num(); ++i)
				{
					if (MeshSockets[i] == Socket)
					{
						Mesh->Modify();
						MeshSockets.RemoveAt(i);
						bFound = true;
						break;
					}
				}
				if (!bFound)
				{
					Session.Log(FString::Printf(
						TEXT("[FAIL] remove(\"socket\", \"%s\") -> socket is on the Skeleton, not the mesh. Open and edit the Skeleton asset to remove it."), *SocketName));
					return sol::lua_nil;
				}
				Mesh->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove(\"socket\") -> \"%s\""), *SocketName));
				sol::table R = Lua.create_table();
				R["removed"] = TCHAR_TO_UTF8(*SocketName);
				return R;
			}

			// ---- remove("morph_target", name) ----
			// Use USkeletalMesh::RemoveMorphTargets (the editor-complete removal path) rather than
			// UnregisterMorphTarget. RemoveMorphTargets also unregisters the mesh-description
			// morph attribute, clears LODInfo.ImportedMorphTargetSourceFilename entries, transient-
			// renames the morph target, and marks it garbage. UnregisterMorphTarget alone can let
			// stale mesh-description data resurrect the morph target on the next build.
			if (FType.Equals(TEXT("morph_target"), ESearchCase::IgnoreCase))
			{
				FString MorphName = NeoLuaStr::ToFString(IdObj.as<std::string>());
				if (!Mesh->FindMorphTarget(FName(*MorphName)))
				{
					Session.Log(FString::Printf(
						TEXT("[FAIL] remove(\"morph_target\", \"%s\") -> not found. Call list(\"morph_targets\") to see existing names."), *MorphName));
					return sol::lua_nil;
				}

				Mesh->Modify();
				TArray<FName> NamesToRemove; NamesToRemove.Add(FName(*MorphName));
				if (!Mesh->RemoveMorphTargets(NamesToRemove))
				{
					Session.Log(FString::Printf(
						TEXT("[FAIL] remove(\"morph_target\", \"%s\") -> engine removal failed. Call list(\"morph_targets\") and retry with an existing morph target name."),
						*MorphName));
					return sol::lua_nil;
				}
				Mesh->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove(\"morph_target\") -> \"%s\""), *MorphName));
				sol::table R = Lua.create_table();
				R["removed"] = TCHAR_TO_UTF8(*MorphName);
				return R;
			}

			// ---- remove("clothing_asset", {lod, section}) — Finding 5 ----
			// Unbinds cloth data from the specified LOD section and removes the asset from the mesh.
			if (FType.Equals(TEXT("clothing_asset"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("clothing"), ESearchCase::IgnoreCase))
			{
				if (!IdObj.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] remove(\"clothing_asset\") -> params required: {lod=N, section=N}. Call list(\"clothing_in_use\") to find bound sections."));
					return sol::lua_nil;
				}
				sol::table P = IdObj.as<sol::table>();
				sol::optional<int> LodOpt = P.get<sol::optional<int>>("lod");
				sol::optional<int> SecOpt = P.get<sol::optional<int>>("section");
				if (!LodOpt.has_value() || !SecOpt.has_value())
				{
					Session.Log(TEXT("[FAIL] remove(\"clothing_asset\") -> {lod, section} are required."));
					return sol::lua_nil;
				}
				// USkeletalMesh::RemoveClothingAsset silently no-ops if nothing is bound at (lod, section).
				// Pre-check so the caller gets a truthful success signal.
				UClothingAssetBase* Bound = Mesh->GetSectionClothingAsset(LodOpt.value(), SecOpt.value());
				if (!Bound)
				{
					Session.Log(FString::Printf(
						TEXT("[FAIL] remove(\"clothing_asset\", {lod=%d, section=%d}) -> no clothing asset is bound there. Call list(\"clothing_in_use\") or list(\"section_clothing\", {lod=N, section=M}) first."),
						LodOpt.value(), SecOpt.value()));
					return sol::lua_nil;
				}
				Mesh->Modify();
				Mesh->RemoveClothingAsset(LodOpt.value(), SecOpt.value());
				Mesh->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove(\"clothing_asset\", {lod=%d, section=%d}) -> \"%s\" unbound"),
					LodOpt.value(), SecOpt.value(), *Bound->GetName()));
				sol::table R = Lua.create_table();
				R["lod"] = LodOpt.value();
				R["section"] = SecOpt.value();
				R["unbound_name"] = TCHAR_TO_UTF8(*Bound->GetName());
				return R;
			}

			// ---- remove("section", {lod, section}) — round-4 Finding 5: RemoveMeshSection ----
			// Destructive: deletes an imported LOD section. Engine rejects when clothing is bound —
			// caller must unbind cloth first (remove("clothing_asset", {lod, section})).
			if (FType.Equals(TEXT("section"), ESearchCase::IgnoreCase))
			{
				if (!IdObj.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] remove(\"section\") -> params required: {lod=N, section=N}. Call list(\"sections\", {lod=N}) to discover valid indices."));
					return sol::lua_nil;
				}
				sol::table P = IdObj.as<sol::table>();
				sol::optional<int> LodOpt = P.get<sol::optional<int>>("lod");
				sol::optional<int> SecOpt = P.get<sol::optional<int>>("section");
				if (!LodOpt.has_value() || !SecOpt.has_value())
				{
					Session.Log(TEXT("[FAIL] remove(\"section\") -> {lod, section} are required."));
					return sol::lua_nil;
				}
				// USkeletalMesh::GetImportedModel() returns FSkeletalMeshModel*, defined in
				// Rendering/SkeletalMeshModel.h (WITH_EDITOR only). This binding lives in an
				// editor-only module so the type is always visible here.
				FSkeletalMeshModel* Imported = Mesh->GetImportedModel();
				if (!Imported || !Imported->LODModels.IsValidIndex(LodOpt.value()))
				{
					Session.Log(FString::Printf(
						TEXT("[FAIL] remove(\"section\", {lod=%d}) -> LOD index out of range (imported_lods=%d)."),
						LodOpt.value(), Imported ? Imported->LODModels.Num() : 0));
					return sol::lua_nil;
				}
				const FSkeletalMeshLODModel& LodModel = Imported->LODModels[LodOpt.value()];
				if (!LodModel.Sections.IsValidIndex(SecOpt.value()))
				{
					Session.Log(FString::Printf(
						TEXT("[FAIL] remove(\"section\", {lod=%d, section=%d}) -> section index out of range (lod_sections=%d). Call list(\"sections\", {lod=%d})."),
						LodOpt.value(), SecOpt.value(), LodModel.Sections.Num(), LodOpt.value()));
					return sol::lua_nil;
				}
				// Guard against cloth-bound sections (engine silently refuses these).
				if (Mesh->GetSectionClothingAsset(LodOpt.value(), SecOpt.value()))
				{
					Session.Log(FString::Printf(
						TEXT("[FAIL] remove(\"section\", {lod=%d, section=%d}) -> clothing is bound to this section. Call remove(\"clothing_asset\", {lod=%d, section=%d}) first, then retry."),
						LodOpt.value(), SecOpt.value(), LodOpt.value(), SecOpt.value()));
					return sol::lua_nil;
				}
				Mesh->Modify();
				Mesh->RemoveMeshSection(LodOpt.value(), SecOpt.value());
				Mesh->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] remove(\"section\", {lod=%d, section=%d})"), LodOpt.value(), SecOpt.value()));
				sol::table R = Lua.create_table();
				R["lod"] = LodOpt.value();
				R["section"] = SecOpt.value();
				return R;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type. Valid: lod, socket, morph_target, clothing_asset, section"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// configure(type, id, params)
		// ================================================================
		AssetObj.set_function("configure", [WeakMesh, &Session](sol::table /*Self*/,
			std::string TypeStr, sol::object Arg2, sol::optional<sol::table> Arg3, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			NS_RESOLVE_MESH_OR_FAIL("configure");
			FString FType = NeoLuaStr::ToFString(TypeStr);

			// ---- configure("lod", index, {...}) ----
			if (FType.Equals(TEXT("lod"), ESearchCase::IgnoreCase))
			{
				int32 Index = Arg2.as<int32>();
				if (!Arg3.has_value())
				{
					Session.Log(TEXT("[FAIL] configure(\"lod\") -> params required: {screen_size?, lod_hysteresis?, reduction?, build_settings?, bones_to_remove?, bones_to_prioritize?, weight_of_prioritization?, allow_cpu_access?, morph_target_position_error_tolerance?}. Call list(\"lods\") to find valid LOD indices first."));
					return sol::lua_nil;
				}
				sol::table P = Arg3.value();

				FSkeletalMeshLODInfo* LODInfo = Mesh->GetLODInfo(Index);
				if (!LODInfo)
				{
					Session.Log(FString::Printf(
						TEXT("[FAIL] configure(\"lod\", %d) -> index out of range (lod_count=%d). Call list(\"lods\")."),
						Index, Mesh->GetLODNum()));
					return sol::lua_nil;
				}

				Mesh->Modify();

				if (P.get<sol::optional<double>>("screen_size").has_value())
					LODInfo->ScreenSize = FPerPlatformFloat(static_cast<float>(P.get<double>("screen_size")));
				if (P.get<sol::optional<double>>("lod_hysteresis").has_value())
					LODInfo->LODHysteresis = static_cast<float>(P.get<double>("lod_hysteresis"));
				if (P.get<sol::optional<bool>>("allow_cpu_access").has_value())
					LODInfo->bAllowCPUAccess = P.get<bool>("allow_cpu_access");
				if (P.get<sol::optional<double>>("morph_target_position_error_tolerance").has_value())
					LODInfo->MorphTargetPositionErrorTolerance = static_cast<float>(P.get<double>("morph_target_position_error_tolerance"));

				// BonesToRemove
				sol::optional<sol::table> BonesToRemoveOpt = P.get<sol::optional<sol::table>>("bones_to_remove");
				if (BonesToRemoveOpt.has_value())
				{
					LODInfo->BonesToRemove.Empty();
					for (auto& Pair : BonesToRemoveOpt.value())
					{
						if (Pair.second.is<std::string>())
						{
							FBoneReference Ref;
							Ref.BoneName = FName(NeoLuaStr::ToFString(Pair.second.as<std::string>()));
							LODInfo->BonesToRemove.Add(Ref);
						}
					}
				}

				// BonesToPrioritize
				sol::optional<sol::table> BonesToPrioritizeOpt = P.get<sol::optional<sol::table>>("bones_to_prioritize");
				if (BonesToPrioritizeOpt.has_value())
				{
					LODInfo->BonesToPrioritize.Empty();
					for (auto& Pair : BonesToPrioritizeOpt.value())
					{
						if (Pair.second.is<std::string>())
						{
							FBoneReference Ref;
							Ref.BoneName = FName(NeoLuaStr::ToFString(Pair.second.as<std::string>()));
							LODInfo->BonesToPrioritize.Add(Ref);
						}
					}
				}

				if (P.get<sol::optional<double>>("weight_of_prioritization").has_value())
					LODInfo->WeightOfPrioritization = static_cast<float>(P.get<double>("weight_of_prioritization"));

				// Reduction settings (inline — matches existing direct-write pattern).
				sol::optional<sol::table> RedOpt = P.get<sol::optional<sol::table>>("reduction");
				if (RedOpt.has_value())
				{
					sol::table R = RedOpt.value();
					FSkeletalMeshOptimizationSettings& RS = LODInfo->ReductionSettings;
					if (R.get<sol::optional<double>>("num_of_triangles_percentage").has_value()) RS.NumOfTrianglesPercentage = R.get<double>("num_of_triangles_percentage");
					if (R.get<sol::optional<double>>("num_of_vert_percentage").has_value()) RS.NumOfVertPercentage = R.get<double>("num_of_vert_percentage");
					if (R.get<sol::optional<double>>("max_deviation").has_value()) RS.MaxDeviationPercentage = R.get<double>("max_deviation");
					if (R.get<sol::optional<int>>("max_num_of_triangles").has_value()) RS.MaxNumOfTriangles = static_cast<uint32>(FMath::Max(0, R.get<int>("max_num_of_triangles")));
					if (R.get<sol::optional<int>>("max_num_of_verts").has_value()) RS.MaxNumOfVerts = static_cast<uint32>(FMath::Max(0, R.get<int>("max_num_of_verts")));
					if (R.get<sol::optional<int>>("base_lod").has_value()) RS.BaseLOD = R.get<int>("base_lod");
					if (R.get<sol::optional<double>>("welding_threshold").has_value()) RS.WeldingThreshold = R.get<double>("welding_threshold");
					if (R.get<sol::optional<bool>>("recalculate_normals").has_value()) RS.bRecalcNormals = R.get<bool>("recalculate_normals");
					if (R.get<sol::optional<double>>("normals_threshold").has_value()) RS.NormalsThreshold = R.get<double>("normals_threshold");
					if (R.get<sol::optional<bool>>("lock_edges").has_value()) RS.bLockEdges = R.get<bool>("lock_edges");
					if (R.get<sol::optional<bool>>("lock_colorBounaries").has_value()) RS.bLockColorBounaries = R.get<bool>("lock_colorBounaries");
				}

				// Build settings (Finding 7 — goes through subsystem so the editor closes/reopens correctly)
				sol::optional<sol::table> BuildOpt = P.get<sol::optional<sol::table>>("build_settings");
				bool bBuildApplied = false;
				if (BuildOpt.has_value())
				{
					if (USkeletalMeshEditorSubsystem* Subsys = GetSkelMeshEditorSubsystem())
					{
						FSkeletalMeshBuildSettings BS;
						USkeletalMeshEditorSubsystem::GetLodBuildSettings(Mesh, Index, BS);
						sol::table B = BuildOpt.value();
						if (B.get<sol::optional<bool>>("recompute_normals").has_value()) BS.bRecomputeNormals = B.get<bool>("recompute_normals");
						if (B.get<sol::optional<bool>>("recompute_tangents").has_value()) BS.bRecomputeTangents = B.get<bool>("recompute_tangents");
						if (B.get<sol::optional<bool>>("use_mikk_tspace").has_value()) BS.bUseMikkTSpace = B.get<bool>("use_mikk_tspace");
						if (B.get<sol::optional<bool>>("compute_weighted_normals").has_value()) BS.bComputeWeightedNormals = B.get<bool>("compute_weighted_normals");
						if (B.get<sol::optional<bool>>("remove_degenerates").has_value()) BS.bRemoveDegenerates = B.get<bool>("remove_degenerates");
						if (B.get<sol::optional<bool>>("use_high_precision_tangent_basis").has_value()) BS.bUseHighPrecisionTangentBasis = B.get<bool>("use_high_precision_tangent_basis");
						if (B.get<sol::optional<bool>>("use_high_precision_skin_weights").has_value()) BS.bUseHighPrecisionSkinWeights = B.get<bool>("use_high_precision_skin_weights");
						if (B.get<sol::optional<bool>>("use_full_precision_uvs").has_value()) BS.bUseFullPrecisionUVs = B.get<bool>("use_full_precision_uvs");
						if (B.get<sol::optional<bool>>("use_backwards_compatible_f16_trunc_uvs").has_value()) BS.bUseBackwardsCompatibleF16TruncUVs = B.get<bool>("use_backwards_compatible_f16_trunc_uvs");
						if (B.get<sol::optional<double>>("threshold_position").has_value()) BS.ThresholdPosition = static_cast<float>(B.get<double>("threshold_position"));
						if (B.get<sol::optional<double>>("threshold_tangent_normal").has_value()) BS.ThresholdTangentNormal = static_cast<float>(B.get<double>("threshold_tangent_normal"));
						if (B.get<sol::optional<double>>("threshold_uv").has_value()) BS.ThresholdUV = static_cast<float>(B.get<double>("threshold_uv"));
						if (B.get<sol::optional<double>>("morph_threshold_position").has_value()) BS.MorphThresholdPosition = static_cast<float>(B.get<double>("morph_threshold_position"));
						if (B.get<sol::optional<int>>("bone_influence_limit").has_value()) BS.BoneInfluenceLimit = B.get<int>("bone_influence_limit");
						USkeletalMeshEditorSubsystem::SetLodBuildSettings(Mesh, Index, BS);
						bBuildApplied = true;
					}
					else
					{
						Session.Log(TEXT("[WARN] configure(\"lod\", .., {build_settings=..}) -> editor subsystem unavailable; skipped."));
					}
				}

				Mesh->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"lod\", %d) -> updated%s"), Index, bBuildApplied ? TEXT(" (build_settings applied)") : TEXT("")));
				sol::table Res = Lua.create_table();
				Res["index"] = Index;
				Res["screen_size"] = LODInfo->ScreenSize.GetDefault();
				return Res;
			}

			// ---- configure("material", index, {material, slot_name?, overlay_material?}) ----
			if (FType.Equals(TEXT("material"), ESearchCase::IgnoreCase))
			{
				int32 Index = Arg2.as<int32>();
				if (!Arg3.has_value())
				{
					Session.Log(TEXT("[FAIL] configure(\"material\") -> params required: {material?=\"/Game/...\", slot_name?, overlay_material?=\"/Game/...\"|\"None\"}. Call list(\"materials\") for valid zero-based slot indices."));
					return sol::lua_nil;
				}
				sol::table P = Arg3.value();

				TArray<FSkeletalMaterial> Materials = Mesh->GetMaterials();
				if (Index < 0 || Index >= Materials.Num())
				{
					Session.Log(FString::Printf(
						TEXT("[FAIL] configure(\"material\", %d) -> index out of range (material_count=%d). Call list(\"materials\") and retry with a valid zero-based index."),
						Index, Materials.Num()));
					return sol::lua_nil;
				}
				Mesh->Modify();

				sol::optional<std::string> MatPathOpt = P.get<sol::optional<std::string>>("material");
				if (MatPathOpt.has_value())
				{
					UMaterialInterface* MatIface = NeoLuaAsset::ResolveWithRegistry<UMaterialInterface>(NeoLuaStr::ToFString(MatPathOpt.value()));
					if (MatIface) Materials[Index].MaterialInterface = MatIface;
					else Session.Log(FString::Printf(TEXT("[WARN] configure(\"material\", %d) -> material \"%s\" not found"), Index, *NeoLuaStr::ToFString(MatPathOpt.value())));
				}
				sol::optional<std::string> SlotOpt = P.get<sol::optional<std::string>>("slot_name");
				if (SlotOpt.has_value())
					Materials[Index].MaterialSlotName = FName(NeoLuaStr::ToFString(SlotOpt.value()));

				// Finding 7 — per-slot overlay material (subsystem equivalent: SetMaterialSlotOverlayMaterial).
				sol::optional<std::string> OverlayOpt = P.get<sol::optional<std::string>>("overlay_material");
				if (OverlayOpt.has_value())
				{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
					FString OverlayPath = NeoLuaStr::ToFString(OverlayOpt.value());
					if (OverlayPath.Equals(TEXT("None"), ESearchCase::IgnoreCase) || OverlayPath.IsEmpty())
					{
						Materials[Index].OverlayMaterialInterface = nullptr;
					}
					else
					{
						UMaterialInterface* OverlayMat = NeoLuaAsset::ResolveWithRegistry<UMaterialInterface>(OverlayPath);
						if (OverlayMat) Materials[Index].OverlayMaterialInterface = OverlayMat;
						else Session.Log(FString::Printf(TEXT("[WARN] configure(\"material\", %d) -> overlay_material \"%s\" not found"), Index, *OverlayPath));
					}
#else
					Session.Log(FString::Printf(TEXT("[WARN] configure(\"material\", %d) -> overlay_material requires UE 5.7+; ignored."), Index));
#endif
				}

				Mesh->SetMaterials(Materials);
				// SetMaterials only assigns the array — the render-data rebuild and component
				// re-register live in PostEditChangeProperty for the Materials fname.
				if (FProperty* MatProp = FindFProperty<FProperty>(USkeletalMesh::StaticClass(), USkeletalMesh::GetMaterialsMemberName()))
				{
					FPropertyChangedEvent Evt(MatProp, EPropertyChangeType::ValueSet);
					Mesh->PostEditChangeProperty(Evt);
				}
				Mesh->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"material\", %d)"), Index));
				sol::table Res = Lua.create_table();
				Res["index"] = Index;
				return Res;
			}

			// ---- configure("socket", name, {bone?, location?, rotation?, scale?}) ----
			if (FType.Equals(TEXT("socket"), ESearchCase::IgnoreCase))
			{
				FString SocketName = NeoLuaStr::ToFString(Arg2.as<std::string>());
				USkeletalMeshSocket* Socket = Mesh->FindSocket(FName(*SocketName));
				if (!Socket)
				{
					Session.Log(FString::Printf(
						TEXT("[FAIL] configure(\"socket\", \"%s\") -> not found. Call list(\"sockets\") for existing names."), *SocketName));
					return sol::lua_nil;
				}
				if (!Arg3.has_value()) { Session.Log(TEXT("[FAIL] configure(\"socket\") -> params required: {bone?, location?, rotation?, scale?}. Call list(\"sockets\") to discover existing socket names first.")); return sol::lua_nil; }

				// Finding 3 — FindSocket falls back to the Skeleton; reject skeleton-owned sockets
				// to match existing remove("socket") behaviour and avoid dirtying only the mesh package.
				TArray<TObjectPtr<USkeletalMeshSocket>>& MeshSockets = Mesh->GetMeshOnlySocketList();
				if (!MeshSockets.Contains(Socket))
				{
					Session.Log(FString::Printf(
						TEXT("[FAIL] configure(\"socket\", \"%s\") -> socket belongs to the Skeleton, not this mesh. Open the Skeleton asset to edit it, or call add(\"socket\", {name=\"%s\", ...}) to override on this mesh."),
						*SocketName, *SocketName));
					return sol::lua_nil;
				}

				sol::table P = Arg3.value();

				Socket->Modify();

				sol::optional<std::string> BoneOpt = P.get<sol::optional<std::string>>("bone");
				if (BoneOpt.has_value())
				{
					FName NewBone = FName(NeoLuaStr::ToFString(BoneOpt.value()));
					if (Mesh->GetRefSkeleton().FindBoneIndex(NewBone) == INDEX_NONE)
					{
						Session.Log(FString::Printf(
							TEXT("[WARN] configure(\"socket\", \"%s\") -> bone \"%s\" not in reference skeleton. Applied anyway; socket may not function correctly."),
							*SocketName, *NewBone.ToString()));
					}
					Socket->BoneName = NewBone;
				}

				sol::optional<sol::table> LocOpt = P.get<sol::optional<sol::table>>("location");
				if (LocOpt.has_value()) { Socket->RelativeLocation.X = LocOpt.value().get_or("x", Socket->RelativeLocation.X); Socket->RelativeLocation.Y = LocOpt.value().get_or("y", Socket->RelativeLocation.Y); Socket->RelativeLocation.Z = LocOpt.value().get_or("z", Socket->RelativeLocation.Z); }
				sol::optional<sol::table> RotOpt = P.get<sol::optional<sol::table>>("rotation");
				if (RotOpt.has_value()) { Socket->RelativeRotation.Pitch = RotOpt.value().get_or("pitch", Socket->RelativeRotation.Pitch); Socket->RelativeRotation.Yaw = RotOpt.value().get_or("yaw", Socket->RelativeRotation.Yaw); Socket->RelativeRotation.Roll = RotOpt.value().get_or("roll", Socket->RelativeRotation.Roll); }
				sol::optional<sol::table> ScaleOpt = P.get<sol::optional<sol::table>>("scale");
				if (ScaleOpt.has_value()) { Socket->RelativeScale.X = ScaleOpt.value().get_or("x", Socket->RelativeScale.X); Socket->RelativeScale.Y = ScaleOpt.value().get_or("y", Socket->RelativeScale.Y); Socket->RelativeScale.Z = ScaleOpt.value().get_or("z", Socket->RelativeScale.Z); }

				Mesh->MarkPackageDirty();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"socket\") -> \"%s\""), *SocketName));
				sol::table R = Lua.create_table();
				R["name"] = TCHAR_TO_UTF8(*SocketName);
				return R;
			}

			// ---- configure("section", {lod, section, cast_shadow?, recompute_tangent?, ...}) ----
			// Finding 7 coverage: wraps SetSectionCastShadow / SetSectionRecomputeTangent /
			// SetSectionRecomputeTangentsVertexMaskChannel / SetSectionVisibleInRayTracing.
			if (FType.Equals(TEXT("section"), ESearchCase::IgnoreCase))
			{
				if (!Arg2.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"section\") -> params required: {lod, section, cast_shadow?, recompute_tangent?, recompute_tangents_vertex_mask_channel?, visible_in_ray_tracing?}. Call list(\"sections\", {lod=N}) to discover valid (lod, section) pairs."));
					return sol::lua_nil;
				}
				USkeletalMeshEditorSubsystem* Subsys = GetSkelMeshEditorSubsystem();
				if (!Subsys)
				{
					Session.Log(TEXT("[FAIL] configure(\"section\") -> editor subsystem unavailable. Run inside the Unreal editor (not commandlet/cook) and retry."));
					return sol::lua_nil;
				}

				sol::table P = Arg2.as<sol::table>();
				sol::optional<int> LodOpt = P.get<sol::optional<int>>("lod");
				sol::optional<int> SecOpt = P.get<sol::optional<int>>("section");
				if (!LodOpt.has_value() || !SecOpt.has_value())
				{
					Session.Log(TEXT("[FAIL] configure(\"section\") -> {lod, section} are required. Call list(\"lods\") / list(\"sections\", {lod=N}) to discover valid indices."));
					return sol::lua_nil;
				}
				int32 LodIndex = LodOpt.value();
				int32 SecIndex = SecOpt.value();

				sol::table R = Lua.create_table();
				R["lod"] = LodIndex;
				R["section"] = SecIndex;
				bool bAnyApplied = false;

				if (P.get<sol::optional<bool>>("cast_shadow").has_value())
				{
					bool bVal = P.get<bool>("cast_shadow");
					if (!Subsys->SetSectionCastShadow(Mesh, LodIndex, SecIndex, bVal))
					{
						Session.Log(FString::Printf(TEXT("[FAIL] configure(\"section\") -> SetSectionCastShadow rejected LOD=%d section=%d (check LOD/section indices)."), LodIndex, SecIndex));
						return sol::lua_nil;
					}
					R["cast_shadow"] = bVal;
					bAnyApplied = true;
				}
				if (P.get<sol::optional<bool>>("recompute_tangent").has_value())
				{
					bool bVal = P.get<bool>("recompute_tangent");
					if (!Subsys->SetSectionRecomputeTangent(Mesh, LodIndex, SecIndex, bVal))
					{
						Session.Log(FString::Printf(
							TEXT("[FAIL] configure(\"section\") -> SetSectionRecomputeTangent rejected LOD=%d section=%d. Call list(\"sections\", {lod=%d}) to discover valid section indices."),
							LodIndex, SecIndex, LodIndex));
						return sol::lua_nil;
					}
					R["recompute_tangent"] = bVal;
					bAnyApplied = true;
				}
				if (P.get<sol::optional<int>>("recompute_tangents_vertex_mask_channel").has_value())
				{
					int32 Val = P.get<int>("recompute_tangents_vertex_mask_channel");
					if (Val < 0 || Val > 255)
					{
						Session.Log(FString::Printf(TEXT("[FAIL] configure(\"section\") -> recompute_tangents_vertex_mask_channel=%d out of uint8 range [0,255]."), Val));
						return sol::lua_nil;
					}
					if (!Subsys->SetSectionRecomputeTangentsVertexMaskChannel(Mesh, LodIndex, SecIndex, static_cast<uint8>(Val)))
					{
						Session.Log(FString::Printf(
							TEXT("[FAIL] configure(\"section\") -> SetSectionRecomputeTangentsVertexMaskChannel rejected LOD=%d section=%d. Call list(\"sections\", {lod=%d}) and retry with a valid section."),
							LodIndex, SecIndex, LodIndex));
						return sol::lua_nil;
					}
					R["recompute_tangents_vertex_mask_channel"] = Val;
					bAnyApplied = true;
				}
				if (P.get<sol::optional<bool>>("visible_in_ray_tracing").has_value())
				{
					bool bVal = P.get<bool>("visible_in_ray_tracing");
					if (!Subsys->SetSectionVisibleInRayTracing(Mesh, LodIndex, SecIndex, bVal))
					{
						Session.Log(FString::Printf(
							TEXT("[FAIL] configure(\"section\") -> SetSectionVisibleInRayTracing rejected LOD=%d section=%d. Call list(\"sections\", {lod=%d}) and retry with a valid section."),
							LodIndex, SecIndex, LodIndex));
						return sol::lua_nil;
					}
					R["visible_in_ray_tracing"] = bVal;
					bAnyApplied = true;
				}

				if (!bAnyApplied)
				{
					Session.Log(TEXT("[WARN] configure(\"section\") -> no recognised keys; nothing applied. Accepted: cast_shadow, recompute_tangent, recompute_tangents_vertex_mask_channel, visible_in_ray_tracing."));
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[OK] configure(\"section\", {lod=%d, section=%d})"), LodIndex, SecIndex));
				}
				return R;
			}

			// ---- configure("physics", {physics_asset?, shadow_physics_asset?, enable_per_poly_collision?}) ----
			if (FType.Equals(TEXT("physics"), ESearchCase::IgnoreCase))
			{
				if (!Arg2.is<sol::table>())
				{
					Session.Log(TEXT("[FAIL] configure(\"physics\") -> params table required: {physics_asset?, shadow_physics_asset?, enable_per_poly_collision?}"));
					return sol::lua_nil;
				}
				sol::table P = Arg2.as<sol::table>();

				sol::table R = Lua.create_table();
				bool bAny = false;

				// physics_asset — routed through AssignPhysicsAsset (compat check + component refresh).
				sol::optional<std::string> PAOpt = P.get<sol::optional<std::string>>("physics_asset");
				if (PAOpt.has_value())
				{
					FString PAStr = NeoLuaStr::ToFString(PAOpt.value());
					UPhysicsAsset* PA = nullptr;
					if (!PAStr.Equals(TEXT("None"), ESearchCase::IgnoreCase) && !PAStr.IsEmpty())
					{
						PA = NeoLuaAsset::ResolveWithRegistry<UPhysicsAsset>(PAStr);
						if (!PA)
						{
							Session.Log(FString::Printf(
								TEXT("[FAIL] configure(\"physics\") -> physics_asset \"%s\" not found or not a UPhysicsAsset. Use find_assets(\"/Game\", {class=\"PhysicsAsset\"}) or pass \"None\" to clear."),
								*PAStr));
							return sol::lua_nil;
						}
					}
					if (!USkeletalMeshEditorSubsystem::AssignPhysicsAsset(Mesh, PA))
					{
						Session.Log(FString::Printf(
							TEXT("[FAIL] configure(\"physics\") -> physics_asset \"%s\" is incompatible with this mesh. One or more body setup / constraint bones are missing from the reference skeleton. Use a PhysicsAsset built for the same skeleton, or pass \"None\" to clear."),
							*PAStr));
						return sol::lua_nil;
					}
					R["physics_asset"] = Mesh->GetPhysicsAsset() ? TCHAR_TO_UTF8(*Mesh->GetPhysicsAsset()->GetPathName()) : "None";
					bAny = true;
				}

				// shadow_physics_asset — no subsystem helper; write directly, then notify via property-changed event.
				sol::optional<std::string> SPAOpt = P.get<sol::optional<std::string>>("shadow_physics_asset");
				if (SPAOpt.has_value())
				{
					FString SPAStr = NeoLuaStr::ToFString(SPAOpt.value());
					UPhysicsAsset* SPA = nullptr;
					bool bResolved = true;
					if (!SPAStr.Equals(TEXT("None"), ESearchCase::IgnoreCase) && !SPAStr.IsEmpty())
					{
						SPA = NeoLuaAsset::ResolveWithRegistry<UPhysicsAsset>(SPAStr);
						if (!SPA)
						{
							Session.Log(FString::Printf(TEXT("[WARN] configure(\"physics\") -> shadow_physics_asset \"%s\" not found; skipped."), *SPAStr));
							bResolved = false;
						}
					}
					if (bResolved)
					{
						Mesh->Modify();
						Mesh->SetShadowPhysicsAsset(SPA);
						if (FProperty* Prop = FindFProperty<FProperty>(USkeletalMesh::StaticClass(), TEXT("ShadowPhysicsAsset")))
						{
							FPropertyChangedEvent Evt(Prop, EPropertyChangeType::ValueSet);
							Mesh->PostEditChangeProperty(Evt);
						}
						R["shadow_physics_asset"] = Mesh->GetShadowPhysicsAsset() ? TCHAR_TO_UTF8(*Mesh->GetShadowPhysicsAsset()->GetPathName()) : "None";
						bAny = true;
					}
				}

				if (P.get<sol::optional<bool>>("enable_per_poly_collision").has_value())
				{
					Mesh->Modify();
					Mesh->SetEnablePerPolyCollision(P.get<bool>("enable_per_poly_collision"));
					// BuildPhysicsData() only runs via PostEditChangeProperty when the fname matches.
					if (FProperty* PerPolyProp = FindFProperty<FProperty>(USkeletalMesh::StaticClass(), USkeletalMesh::GetEnablePerPolyCollisionMemberName()))
					{
						FPropertyChangedEvent Evt(PerPolyProp, EPropertyChangeType::ValueSet);
						Mesh->PostEditChangeProperty(Evt);
					}
					R["enable_per_poly_collision"] = Mesh->GetEnablePerPolyCollision();
					bAny = true;
				}

				if (!bAny)
				{
					Session.Log(TEXT("[WARN] configure(\"physics\") -> no recognised keys; nothing applied."));
				}
				else
				{
					Mesh->MarkPackageDirty();
					Session.Log(TEXT("[OK] configure(\"physics\") -> updated"));
				}
				return R;
			}

			// ---- configure("nanite", nil, {...}) ----
			if (FType.Equals(TEXT("nanite"), ESearchCase::IgnoreCase))
			{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
				sol::table P = Arg3.has_value() ? Arg3.value() : Arg2.as<sol::table>();
				FScopedTransaction Tx(FText::FromString(TEXT("Configure Nanite Settings")));
				Mesh->Modify();

				FMeshNaniteSettings NaniteSettings = Mesh->GetNaniteSettings();
				if (P.get<sol::optional<bool>>("enabled").has_value())
					NaniteSettings.bEnabled = P.get<bool>("enabled");
				if (P.get<sol::optional<bool>>("explicit_tangents").has_value())
					NaniteSettings.bExplicitTangents = P.get<bool>("explicit_tangents");
				if (P.get<sol::optional<bool>>("lerp_uvs").has_value())
					NaniteSettings.bLerpUVs = P.get<bool>("lerp_uvs");
				if (P.get<sol::optional<bool>>("separable").has_value())
					NaniteSettings.bSeparable = P.get<bool>("separable");
				if (P.get<sol::optional<bool>>("voxel_ndf").has_value())
					NaniteSettings.bVoxelNDF = P.get<bool>("voxel_ndf");

				Mesh->SetNaniteSettings(NaniteSettings);
				Mesh->NotifyNaniteSettingsChanged();
				Mesh->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"nanite\") -> enabled=%s"), NaniteSettings.bEnabled ? TEXT("true") : TEXT("false")));
				sol::table R = Lua.create_table();
				R["enabled"] = static_cast<bool>(NaniteSettings.bEnabled);
				R["explicit_tangents"] = static_cast<bool>(NaniteSettings.bExplicitTangents);
				R["lerp_uvs"] = static_cast<bool>(NaniteSettings.bLerpUVs);
				R["separable"] = static_cast<bool>(NaniteSettings.bSeparable);
				R["voxel_ndf"] = static_cast<bool>(NaniteSettings.bVoxelNDF);
				return R;
#else
				sol::table R = Lua.create_table();
				R["error"] = "Nanite settings requires UE 5.7+";
				return R;
#endif
			}

			// ---- configure("skeleton", nil, {skeleton="/Game/..."}) ----
			if (FType.Equals(TEXT("skeleton"), ESearchCase::IgnoreCase))
			{
				sol::table P = Arg3.has_value() ? Arg3.value() : Arg2.as<sol::table>();
				std::string SkelPath = P.get_or<std::string>("skeleton", "");
				if (SkelPath.empty()) { Session.Log(TEXT("[FAIL] configure(\"skeleton\") -> 'skeleton' path required")); return sol::lua_nil; }

				USkeleton* NewSkeleton = NeoLuaAsset::ResolveWithRegistry<USkeleton>(NeoLuaStr::ToFString(SkelPath));
				if (!NewSkeleton)
				{
					Session.Log(FString::Printf(
						TEXT("[FAIL] configure(\"skeleton\") -> skeleton \"%s\" not found or not a USkeleton. Use find_assets(\"/Game\", {class=\"Skeleton\"}) and retry with its path."),
						UTF8_TO_TCHAR(SkelPath.c_str())));
					return sol::lua_nil;
				}

				FScopedTransaction Tx(FText::FromString(TEXT("Set Skeleton")));
				Mesh->Modify();
				Mesh->SetSkeleton(NewSkeleton);
				Mesh->PostEditChange();
				Mesh->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"skeleton\") -> %s"), *NewSkeleton->GetPathName()));
				sol::table R = Lua.create_table();
				R["skeleton"] = TCHAR_TO_UTF8(*NewSkeleton->GetPathName());
				return R;
			}

			// ---- configure("lod_settings", nil, {min_lod?, disable_below_min_lod_stripping?}) ----
			if (FType.Equals(TEXT("lod_settings"), ESearchCase::IgnoreCase))
			{
				sol::table P = Arg3.has_value() ? Arg3.value() : Arg2.as<sol::table>();
				FScopedTransaction Tx(FText::FromString(TEXT("Configure LOD Settings")));
				Mesh->Modify();

				if (P.get<sol::optional<int>>("min_lod").has_value())
				{
					int32 MinLod = P.get<int>("min_lod");
					Mesh->SetMinLodIdx(MinLod);
				}
				if (P.get<sol::optional<bool>>("disable_below_min_lod_stripping").has_value())
				{
					FPerPlatformBool Val;
					Val.Default = P.get<bool>("disable_below_min_lod_stripping");
					Mesh->SetDisableBelowMinLodStripping(Val);
				}

				Mesh->PostEditChange();
				Mesh->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"lod_settings\") -> min_lod=%d"), Mesh->GetMinLodIdx()));
				sol::table R = Lua.create_table();
				R["min_lod"] = Mesh->GetMinLodIdx();
				R["disable_below_min_lod_stripping"] = Mesh->GetDisableBelowMinLodStripping().Default;
				return R;
			}

			// ---- configure("bounds", nil, {positive_extension?, negative_extension?}) ----
			if (FType.Equals(TEXT("bounds"), ESearchCase::IgnoreCase))
			{
				sol::table P = Arg3.has_value() ? Arg3.value() : Arg2.as<sol::table>();
				Mesh->Modify();

				sol::optional<sol::table> PosOpt = P.get<sol::optional<sol::table>>("positive_extension");
				if (PosOpt.has_value())
				{
					sol::table V = PosOpt.value();
					FVector Ext;
					Ext.X = V.get_or("x", 0.0);
					Ext.Y = V.get_or("y", 0.0);
					Ext.Z = V.get_or("z", 0.0);
					Mesh->SetPositiveBoundsExtension(Ext);
				}
				sol::optional<sol::table> NegOpt = P.get<sol::optional<sol::table>>("negative_extension");
				if (NegOpt.has_value())
				{
					sol::table V = NegOpt.value();
					FVector Ext;
					Ext.X = V.get_or("x", 0.0);
					Ext.Y = V.get_or("y", 0.0);
					Ext.Z = V.get_or("z", 0.0);
					Mesh->SetNegativeBoundsExtension(Ext);
				}

				Mesh->PostEditChange();
				Mesh->MarkPackageDirty();

				const FVector& PosExt = Mesh->GetPositiveBoundsExtension();
				const FVector& NegExt = Mesh->GetNegativeBoundsExtension();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"bounds\") -> pos=(%.1f, %.1f, %.1f) neg=(%.1f, %.1f, %.1f)"),
					PosExt.X, PosExt.Y, PosExt.Z, NegExt.X, NegExt.Y, NegExt.Z));
				sol::table R = Lua.create_table();
				sol::table RPosExt = Lua.create_table();
				RPosExt["x"] = PosExt.X; RPosExt["y"] = PosExt.Y; RPosExt["z"] = PosExt.Z;
				R["positive_extension"] = RPosExt;
				sol::table RNegExt = Lua.create_table();
				RNegExt["x"] = NegExt.X; RNegExt["y"] = NegExt.Y; RNegExt["z"] = NegExt.Z;
				R["negative_extension"] = RNegExt;
				return R;
			}

			// ---- configure("post_process_anim_bp", nil, {...}) ----
			if (FType.Equals(TEXT("post_process_anim_bp"), ESearchCase::IgnoreCase))
			{
				sol::table P = Arg3.has_value() ? Arg3.value() : Arg2.as<sol::table>();
				Mesh->Modify();

				sol::optional<std::string> BPOpt = P.get<sol::optional<std::string>>("blueprint");
				if (BPOpt.has_value())
				{
					FString BPPath = NeoLuaStr::ToFString(BPOpt.value());
					if (BPPath.Equals(TEXT("None"), ESearchCase::IgnoreCase) || BPPath.IsEmpty())
					{
						Mesh->SetPostProcessAnimBlueprint(nullptr);
					}
					else
					{
						UClass* BPClass = NeoLuaAsset::ResolveWithRegistry<UClass>(BPPath);
						if (BPClass && BPClass->IsChildOf(UAnimInstance::StaticClass()))
						{
							Mesh->SetPostProcessAnimBlueprint(BPClass);
						}
						else
						{
							Session.Log(FString::Printf(
								TEXT("[FAIL] configure(\"post_process_anim_bp\") -> \"%s\" is not an AnimInstance class. Use an AnimBlueprint generated class path (e.g. \"/Game/MyABP.MyABP_C\") or pass \"None\" to clear."),
								*BPPath));
							return sol::lua_nil;
						}
					}
				}
				if (P.get<sol::optional<int>>("lod_threshold").has_value())
				{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
					Mesh->SetPostProcessAnimGraphLODThreshold(P.get<int>("lod_threshold"));
#else
					Mesh->SetPostProcessAnimBPLODThreshold(P.get<int>("lod_threshold"));
#endif
				}

				Mesh->PostEditChange();
				Mesh->MarkPackageDirty();

				TSubclassOf<UAnimInstance> PostProcBP = Mesh->GetPostProcessAnimBlueprint();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"post_process_anim_bp\") -> %s"),
					PostProcBP.Get() ? *PostProcBP.Get()->GetPathName() : TEXT("None")));
				sol::table R = Lua.create_table();
				R["blueprint"] = PostProcBP.Get() ? TCHAR_TO_UTF8(*PostProcBP.Get()->GetPathName()) : "None";
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
				R["lod_threshold"] = Mesh->GetPostProcessAnimGraphLODThreshold();
#else
				R["lod_threshold"] = Mesh->GetPostProcessAnimBPLODThreshold();
#endif
				return R;
			}

			// ---- configure("animating_rig", nil, {rig=...}) ----
			if (FType.Equals(TEXT("animating_rig"), ESearchCase::IgnoreCase))
			{
				sol::table P = Arg3.has_value() ? Arg3.value() : Arg2.as<sol::table>();
				std::string RigPath = P.get_or<std::string>("rig", "");
				Mesh->Modify();

				if (RigPath.empty() || FString(NeoLuaStr::ToFString(RigPath)).Equals(TEXT("None"), ESearchCase::IgnoreCase))
				{
					Mesh->SetDefaultAnimatingRig(nullptr);
				}
				else
				{
					TSoftObjectPtr<UObject> RigRef(FSoftObjectPath(NeoLuaStr::ToFString(RigPath)));
					Mesh->SetDefaultAnimatingRig(RigRef);
				}

				Mesh->PostEditChange();
				Mesh->MarkPackageDirty();

				TSoftObjectPtr<UObject> CurrentRig = Mesh->GetDefaultAnimatingRig();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"animating_rig\") -> %s"),
					CurrentRig.IsNull() ? TEXT("None") : *CurrentRig.ToString()));
				sol::table R = Lua.create_table();
				R["rig"] = CurrentRig.IsNull() ? "None" : TCHAR_TO_UTF8(*CurrentRig.ToString());
				return R;
			}

			// ---- configure("ray_tracing", nil, {enabled?, min_lod?}) ----
			if (FType.Equals(TEXT("ray_tracing"), ESearchCase::IgnoreCase))
			{
				sol::table P = Arg3.has_value() ? Arg3.value() : Arg2.as<sol::table>();
				Mesh->Modify();

				if (P.get<sol::optional<bool>>("enabled").has_value())
					Mesh->SetSupportRayTracing(P.get<bool>("enabled"));
				if (P.get<sol::optional<int>>("min_lod").has_value())
					Mesh->SetRayTracingMinLOD(P.get<int>("min_lod"));

				Mesh->PostEditChange();
				Mesh->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"ray_tracing\") -> enabled=%s, min_lod=%d"),
					Mesh->GetSupportRayTracing() ? TEXT("true") : TEXT("false"), Mesh->GetRayTracingMinLOD()));
				sol::table R = Lua.create_table();
				R["enabled"] = Mesh->GetSupportRayTracing();
				R["min_lod"] = Mesh->GetRayTracingMinLOD();
				return R;
			}

			// ---- configure("overlay_material", nil, {material?, max_draw_distance?}) ----
			if (FType.Equals(TEXT("overlay_material"), ESearchCase::IgnoreCase))
			{
				sol::table P = Arg3.has_value() ? Arg3.value() : Arg2.as<sol::table>();
				Mesh->Modify();

				sol::optional<std::string> MatOpt = P.get<sol::optional<std::string>>("material");
				if (MatOpt.has_value())
				{
					FString MatPath = NeoLuaStr::ToFString(MatOpt.value());
					if (MatPath.Equals(TEXT("None"), ESearchCase::IgnoreCase) || MatPath.IsEmpty())
					{
						Mesh->SetOverlayMaterial(nullptr);
					}
					else
					{
						UMaterialInterface* Mat = NeoLuaAsset::ResolveWithRegistry<UMaterialInterface>(MatPath);
						if (Mat) Mesh->SetOverlayMaterial(Mat);
						else Session.Log(FString::Printf(TEXT("[WARN] configure(\"overlay_material\") -> material \"%s\" not found"), *MatPath));
					}
				}
				if (P.get<sol::optional<double>>("max_draw_distance").has_value())
					Mesh->SetOverlayMaterialMaxDrawDistance(static_cast<float>(P.get<double>("max_draw_distance")));

				// PostEditChange triggers Build() + component reregister for non-interactive changes
				// (SkeletalMesh.cpp:1422-1469). Raw setters only assign the member.
				Mesh->PostEditChange();
				Mesh->MarkPackageDirty();

				UMaterialInterface* OverlayMat = Mesh->GetOverlayMaterial();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"overlay_material\") -> %s"),
					OverlayMat ? *OverlayMat->GetPathName() : TEXT("None")));
				sol::table R = Lua.create_table();
				R["material"] = OverlayMat ? TCHAR_TO_UTF8(*OverlayMat->GetPathName()) : "None";
				R["max_draw_distance"] = Mesh->GetOverlayMaterialMaxDrawDistance();
				return R;
			}

			// ---- configure("mesh_deformer", nil, {deformer=...}) ----
			if (FType.Equals(TEXT("mesh_deformer"), ESearchCase::IgnoreCase))
			{
				sol::table P = Arg3.has_value() ? Arg3.value() : Arg2.as<sol::table>();
				std::string DeformerPath = P.get_or<std::string>("deformer", "");
				Mesh->Modify();

				if (DeformerPath.empty() || FString(NeoLuaStr::ToFString(DeformerPath)).Equals(TEXT("None"), ESearchCase::IgnoreCase))
				{
					Mesh->SetDefaultMeshDeformer(nullptr);
				}
				else
				{
					UMeshDeformer* Deformer = NeoLuaAsset::ResolveWithRegistry<UMeshDeformer>(NeoLuaStr::ToFString(DeformerPath));
					if (Deformer) Mesh->SetDefaultMeshDeformer(Deformer);
					else Session.Log(FString::Printf(TEXT("[WARN] configure(\"mesh_deformer\") -> deformer \"%s\" not found"), *NeoLuaStr::ToFString(DeformerPath)));
				}

				Mesh->PostEditChange();
				Mesh->MarkPackageDirty();

				UMeshDeformer* CurrentDeformer = Mesh->GetDefaultMeshDeformer();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"mesh_deformer\") -> %s"),
					CurrentDeformer ? *CurrentDeformer->GetPathName() : TEXT("None")));
				sol::table R = Lua.create_table();
				R["deformer"] = CurrentDeformer ? TCHAR_TO_UTF8(*CurrentDeformer->GetPathName()) : "None";
				return R;
			}

			// ---- configure("floor_offset", nil, {offset=0.0}) ----
			if (FType.Equals(TEXT("floor_offset"), ESearchCase::IgnoreCase))
			{
				sol::table P = Arg3.has_value() ? Arg3.value() : Arg2.as<sol::table>();
				Mesh->Modify();

				if (P.get<sol::optional<double>>("offset").has_value())
					Mesh->SetFloorOffset(static_cast<float>(P.get<double>("offset")));

				Mesh->PostEditChange();
				Mesh->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] configure(\"floor_offset\") -> %.2f"), Mesh->GetFloorOffset()));
				sol::table R = Lua.create_table();
				R["offset"] = Mesh->GetFloorOffset();
				return R;
			}

			// ---- configure("morph_target", nil, {generated_by_engine=true, names?}) ----
			// Finding 7 coverage: SetMorphTargetsToGeneratedByEngine.
			if (FType.Equals(TEXT("morph_target"), ESearchCase::IgnoreCase))
			{
				sol::table P = Arg3.has_value() ? Arg3.value() : (Arg2.is<sol::table>() ? Arg2.as<sol::table>() : sol::table());
				if (!P.valid())
				{
					Session.Log(TEXT("[FAIL] configure(\"morph_target\") -> params required: {generated_by_engine=true, names?={...}}"));
					return sol::lua_nil;
				}
				if (P.get<sol::optional<bool>>("generated_by_engine").has_value())
				{
					if (!P.get<bool>("generated_by_engine"))
					{
						Session.Log(TEXT("[WARN] configure(\"morph_target\", {generated_by_engine=false}) -> no subsystem API to unset generated-by-engine flag; skipped. Use the mesh editor UI."));
						return sol::lua_nil;
					}
					TArray<FString> Names;
					sol::optional<sol::table> NamesOpt = P.get<sol::optional<sol::table>>("names");
					if (NamesOpt.has_value())
					{
						for (auto& Pair : NamesOpt.value())
						{
							if (Pair.second.is<std::string>())
								Names.Add(NeoLuaStr::ToFString(Pair.second.as<std::string>()));
						}
					}
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
					const bool bChanged = USkeletalMeshEditorSubsystem::SetMorphTargetsToGeneratedByEngine(Mesh, Names);
					Session.Log(FString::Printf(TEXT("[OK] configure(\"morph_target\", {generated_by_engine=true}) -> %s (names=%d specified, %d total morph targets on mesh)"),
						bChanged ? TEXT("applied") : TEXT("no change"),
						Names.Num(),
						Mesh->GetMorphTargets().Num()));
					sol::table R = Lua.create_table();
					R["changed"] = bChanged;
					return R;
#else
					Session.Log(TEXT("[FAIL] configure(\"morph_target\", {generated_by_engine=true}) requires UE 5.7+"));
					return sol::lua_nil;
#endif
				}
				Session.Log(TEXT("[FAIL] configure(\"morph_target\") -> only 'generated_by_engine=true' is supported. Names subarray filters which morph targets to flag."));
				return sol::lua_nil;
			}

			// ---- configure("target_mesh_deformers", nil, {collection=...}) — Finding 5 ----
			if (FType.Equals(TEXT("target_mesh_deformers"), ESearchCase::IgnoreCase))
			{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
				sol::table P = Arg3.has_value() ? Arg3.value() : Arg2.as<sol::table>();
				std::string PathStr = P.get_or<std::string>("collection", "");
				Mesh->Modify();

				if (PathStr.empty() || FString(NeoLuaStr::ToFString(PathStr)).Equals(TEXT("None"), ESearchCase::IgnoreCase))
				{
					Mesh->SetTargetMeshDeformers(nullptr);
				}
				else
				{
					UMeshDeformerCollection* Coll = NeoLuaAsset::ResolveWithRegistry<UMeshDeformerCollection>(NeoLuaStr::ToFString(PathStr));
					if (Coll) Mesh->SetTargetMeshDeformers(Coll);
					else
					{
						Session.Log(FString::Printf(
							TEXT("[WARN] configure(\"target_mesh_deformers\") -> collection \"%s\" not found or not a UMeshDeformerCollection"),
							*NeoLuaStr::ToFString(PathStr)));
					}
				}
				Mesh->PostEditChange();
				Mesh->MarkPackageDirty();
				UMeshDeformerCollection* Current = Mesh->GetTargetMeshDeformers();
				Session.Log(FString::Printf(TEXT("[OK] configure(\"target_mesh_deformers\") -> %s"),
					Current ? *Current->GetPathName() : TEXT("None")));
				sol::table R = Lua.create_table();
				R["collection"] = Current ? TCHAR_TO_UTF8(*Current->GetPathName()) : "None";
				return R;
#else
				Session.Log(TEXT("[FAIL] configure(\"target_mesh_deformers\") requires UE 5.7+"));
				return sol::lua_nil;
#endif
			}

			// ---- configure("forward_axis", nil, {axis=...}) — Finding 5 ----
			if (FType.Equals(TEXT("forward_axis"), ESearchCase::IgnoreCase))
			{
				sol::table P = Arg3.has_value() ? Arg3.value() : Arg2.as<sol::table>();
				std::string AxisStr = P.get_or<std::string>("axis", "");
				if (AxisStr.empty())
				{
					Session.Log(TEXT("[FAIL] configure(\"forward_axis\") -> 'axis' required: one of \"X\", \"Y\", \"Z\", \"None\"."));
					return sol::lua_nil;
				}
				const FString FAxis = NeoLuaStr::ToFString(AxisStr);
				EAxis::Type Axis = EAxis::None;
				if (!ParseSkeletalMeshForwardAxis(FAxis, Axis))
				{
					Session.Log(FString::Printf(
						TEXT("[FAIL] configure(\"forward_axis\") -> invalid axis \"%s\". Expected X|Y|Z|None."),
						*FAxis));
					return sol::lua_nil;
				}
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
				const EAxis::Type CurrentAxis = Mesh->GetForwardAxis().GetValue();
				if (Axis != CurrentAxis)
				{
					Session.Log(FString::Printf(
						TEXT("[FAIL] configure(\"forward_axis\") -> refusing metadata-only change from %s to %s; UE stores this as MeshDescription orientation metadata, so reimport or use a transform-backed mesh edit."),
						SkeletalMeshForwardAxisToString(CurrentAxis),
						SkeletalMeshForwardAxisToString(Axis)));
					return sol::lua_nil;
				}
#else
				Session.Log(TEXT("[FAIL] configure(\"forward_axis\") requires UE 5.7+"));
				return sol::lua_nil;
#endif
				const TCHAR* AxisName = SkeletalMeshForwardAxisToString(Axis);
				Session.Log(FString::Printf(
					TEXT("[OK] configure(\"forward_axis\") -> %s (unchanged)"),
					AxisName));
				sol::table R = Lua.create_table();
				R["axis"] = TCHAR_TO_UTF8(AxisName);
				R["changed"] = false;
				return R;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown type. Valid: lod, material, socket, section, physics, nanite, skeleton, lod_settings, bounds, post_process_anim_bp, animating_rig, ray_tracing, overlay_material, mesh_deformer, floor_offset, morph_target, target_mesh_deformers, forward_axis"), *FType));
			return sol::lua_nil;
		});

		// ================================================================
		// build()
		// ================================================================
		AssetObj.set_function("build", [WeakMesh, &Session](sol::table /*Self*/, sol::optional<sol::table> OptsOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			NS_RESOLVE_MESH_OR_FAIL("build");
			// USkeletalMesh::Build() may queue an async build when async compilation is allowed
			// (see FSkinnedAssetCompilingManager::IsAsyncCompilationAllowed). Default to blocking
			// until completion so the returned rebuilt flag is truthful; callers that want the
			// async behaviour can pass {wait=false} and check 'queued' on the returned table.
			const bool bWait = OptsOpt.has_value() ? OptsOpt.value().get_or("wait", true) : true;
			Mesh->Build();
			if (bWait)
			{
				FAssetCompilingManager::Get().FinishCompilationForObjects({ Mesh });
			}
			Mesh->MarkPackageDirty();
			const bool bStillCompiling = Mesh->IsCompiling();
			const bool bRebuilt = !bStillCompiling;
			Session.Log(FString::Printf(TEXT("[OK] build() -> %s %s"), *Mesh->GetName(),
				bRebuilt ? TEXT("rebuilt") : TEXT("queued (async; call build({wait=true}) to block)")));
			sol::table R = Lua.create_table();
			R["rebuilt"] = bRebuilt;
			R["queued"] = bStillCompiling;
			R["lod_count"] = Mesh->GetLODNum();
			return R;
		});

		// ================================================================
		// rename_socket(old_name, new_name) — Finding 7
		// ================================================================
		AssetObj.set_function("rename_socket", [WeakMesh, &Session](sol::table /*Self*/,
			std::string OldNameStr, std::string NewNameStr, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			NS_RESOLVE_MESH_OR_FAIL("rename_socket");
			FString OldName = NeoLuaStr::ToFString(OldNameStr);
			FString NewName = NeoLuaStr::ToFString(NewNameStr);
			if (OldName.IsEmpty() || NewName.IsEmpty())
			{
				Session.Log(TEXT("[FAIL] rename_socket -> old_name and new_name are required"));
				return sol::lua_nil;
			}
			// UE 5.7's RenameSocket internally null-checks GetSkeleton() (SkeletalMeshEditorSubsystem.cpp:615)
			// but older 5.x engines crashed here. Check locally so we can emit a targeted message
			// instead of the generic "subsystem rejected" one.
			if (!Mesh->GetSkeleton())
			{
				Session.Log(FString::Printf(
					TEXT("[FAIL] rename_socket(\"%s\" -> \"%s\") -> mesh has no Skeleton assigned. Assign one via configure(\"skeleton\", nil, {skeleton=\"/Path/MySkeleton\"}), or edit mesh-only sockets with add/configure/remove(\"socket\")."),
					*OldName, *NewName));
				return sol::lua_nil;
			}
			if (!USkeletalMeshEditorSubsystem::RenameSocket(Mesh, FName(*OldName), FName(*NewName)))
			{
				Session.Log(FString::Printf(
					TEXT("[FAIL] rename_socket(\"%s\" -> \"%s\") -> subsystem rejected. Likely: old socket not found, new name already exists, or socket is on the Skeleton."),
					*OldName, *NewName));
				return sol::lua_nil;
			}
			Session.Log(FString::Printf(TEXT("[OK] rename_socket(\"%s\" -> \"%s\")"), *OldName, *NewName));
			sol::table R = Lua.create_table();
			R["old_name"] = TCHAR_TO_UTF8(*OldName);
			R["new_name"] = TCHAR_TO_UTF8(*NewName);
			return R;
		});

		// ================================================================
		// rename_morph_target(old_name, new_name) — round-4 Finding 4
		// Preserves mesh-description morph attributes and LODInfo.ImportedMorphTargetSourceFilename
		// entries (USkeletalMesh::RenameMorphTarget at SkeletalMesh.cpp:4804).
		// ================================================================
		AssetObj.set_function("rename_morph_target", [WeakMesh, &Session](sol::table /*Self*/,
			std::string OldNameStr, std::string NewNameStr, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			NS_RESOLVE_MESH_OR_FAIL("rename_morph_target");
			FString OldName = NeoLuaStr::ToFString(OldNameStr);
			FString NewName = NeoLuaStr::ToFString(NewNameStr);
			if (OldName.IsEmpty() || NewName.IsEmpty())
			{
				Session.Log(TEXT("[FAIL] rename_morph_target -> old_name and new_name are required"));
				return sol::lua_nil;
			}
			if (!Mesh->RenameMorphTarget(FName(*OldName), FName(*NewName)))
			{
				Session.Log(FString::Printf(
					TEXT("[FAIL] rename_morph_target(\"%s\" -> \"%s\") -> engine rejected. Likely: old name not found, new name already exists as a morph target, or new name is an invalid object name. Call list(\"morph_targets\") to discover valid names."),
					*OldName, *NewName));
				return sol::lua_nil;
			}
			Mesh->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] rename_morph_target(\"%s\" -> \"%s\")"), *OldName, *NewName));
			sol::table R = Lua.create_table();
			R["old_name"] = TCHAR_TO_UTF8(*OldName);
			R["new_name"] = TCHAR_TO_UTF8(*NewName);
			return R;
		});

		// ================================================================
		// import_lod(lod_index, source_filename) — Finding 7
		// ================================================================
		AssetObj.set_function("import_lod", [WeakMesh, &Session](sol::table /*Self*/,
			int32 LODIndex, std::string SourceFilenameStr, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			NS_RESOLVE_MESH_OR_FAIL("import_lod");
			USkeletalMeshEditorSubsystem* Subsys = GetSkelMeshEditorSubsystem();
			if (!Subsys)
			{
				Session.Log(TEXT("[FAIL] import_lod -> editor subsystem unavailable. Run inside the Unreal editor (not commandlet/cook) and retry."));
				return sol::lua_nil;
			}
			const int32 Result = Subsys->ImportLOD(Mesh, LODIndex, NeoLuaStr::ToFString(SourceFilenameStr));
			if (Result == INDEX_NONE)
			{
				Session.Log(FString::Printf(
					TEXT("[FAIL] import_lod(%d, \"%s\") -> failed. Check: filename exists on disk (or LODInfo has a SourceImportFilename), LODIndex is in range, editor not in PIE."),
					LODIndex, *NeoLuaStr::ToFString(SourceFilenameStr)));
				return sol::lua_nil;
			}
			Session.Log(FString::Printf(TEXT("[OK] import_lod(%d) -> imported"), Result));
			sol::table R = Lua.create_table();
			R["lod_index"] = Result;
			R["total_lods"] = Mesh->GetLODNum();
			return R;
		});

		// ================================================================
		// reimport_all_custom_lods() — Finding 7
		// ================================================================
		AssetObj.set_function("reimport_all_custom_lods", [WeakMesh, &Session](sol::table /*Self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			NS_RESOLVE_MESH_OR_FAIL("reimport_all_custom_lods");
			if (!USkeletalMeshEditorSubsystem::ReimportAllCustomLODs(Mesh))
			{
				Session.Log(TEXT("[FAIL] reimport_all_custom_lods() -> failed. Check that each custom LOD has a recorded SourceImportFilename on disk."));
				return sol::lua_nil;
			}
			Session.Log(TEXT("[OK] reimport_all_custom_lods()"));
			sol::table R = Lua.create_table();
			R["lod_count"] = Mesh->GetLODNum();
			return R;
		});

		// ================================================================
		// strip_lod_geometry(lod_index, texture_mask_path, threshold) — Finding 7
		// ================================================================
		AssetObj.set_function("strip_lod_geometry", [WeakMesh, &Session](sol::table /*Self*/,
			int32 LODIndex, std::string TextureMaskPath, double Threshold, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			NS_RESOLVE_MESH_OR_FAIL("strip_lod_geometry");
			USkeletalMeshEditorSubsystem* Subsys = GetSkelMeshEditorSubsystem();
			if (!Subsys)
			{
				Session.Log(TEXT("[FAIL] strip_lod_geometry -> editor subsystem unavailable. Run inside the Unreal editor (not commandlet/cook) and retry."));
				return sol::lua_nil;
			}
			UTexture2D* Mask = NeoLuaAsset::ResolveWithRegistry<UTexture2D>(NeoLuaStr::ToFString(TextureMaskPath));
			if (!Mask)
			{
				Session.Log(FString::Printf(
					TEXT("[FAIL] strip_lod_geometry -> texture_mask \"%s\" not found or not a Texture2D."),
					*NeoLuaStr::ToFString(TextureMaskPath)));
				return sol::lua_nil;
			}
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			if (!Subsys->StripLODGeometry(Mesh, LODIndex, Mask, static_cast<float>(Threshold)))
			{
				Session.Log(FString::Printf(
					TEXT("[FAIL] strip_lod_geometry(%d, \"%s\", %.4f) -> failed. Check LOD index and that the texture has UV0 coverage."),
					LODIndex, *NeoLuaStr::ToFString(TextureMaskPath), Threshold));
				return sol::lua_nil;
			}
			Session.Log(FString::Printf(TEXT("[OK] strip_lod_geometry(%d) -> done"), LODIndex));
			sol::table R = Lua.create_table();
			R["lod_index"] = LODIndex;
			return R;
#else
			Session.Log(TEXT("[FAIL] strip_lod_geometry requires UE 5.7+"));
			return sol::lua_nil;
#endif
		});

		// ================================================================
		// create_physics_asset({set_to_mesh?, lod_index?}) — Finding 7
		// ================================================================
		AssetObj.set_function("create_physics_asset", [WeakMesh, &Session](sol::table /*Self*/,
			sol::optional<sol::table> ParamsOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			NS_RESOLVE_MESH_OR_FAIL("create_physics_asset");
			bool bSetToMesh = true;
			int32 LodIndex = 0;
			if (ParamsOpt.has_value())
			{
				sol::table P = ParamsOpt.value();
				if (P.get<sol::optional<bool>>("set_to_mesh").has_value()) bSetToMesh = P.get<bool>("set_to_mesh");
				if (P.get<sol::optional<int>>("lod_index").has_value()) LodIndex = P.get<int>("lod_index");
			}
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			UPhysicsAsset* NewPA = USkeletalMeshEditorSubsystem::CreatePhysicsAsset(Mesh, bSetToMesh, LodIndex);
			if (!NewPA)
			{
				Session.Log(FString::Printf(
					TEXT("[FAIL] create_physics_asset -> CreatePhysicsAsset returned null. Check LOD %d exists and has geometry."), LodIndex));
				return sol::lua_nil;
			}
			Session.Log(FString::Printf(
				TEXT("[OK] create_physics_asset -> \"%s\"%s"),
				*NewPA->GetPathName(), bSetToMesh ? TEXT(" (assigned to mesh)") : TEXT("")));
			sol::table R = Lua.create_table();
			R["path"] = TCHAR_TO_UTF8(*NewPA->GetPathName());
			R["name"] = TCHAR_TO_UTF8(*NewPA->GetName());
			R["set_to_mesh"] = bSetToMesh;
			return R;
#else
			// Pre-5.7: subsystem CreatePhysicsAsset signature differs and isn't a clean fallback.
			(void)bSetToMesh; (void)LodIndex;
			Session.Log(TEXT("[FAIL] create_physics_asset requires UE 5.7+"));
			return sol::lua_nil;
#endif
		});

		// ================================================================
		// regenerate_lods({new_lod_count?, regenerate_imported?, generate_base?}) — Finding 7
		// ================================================================
		// ================================================================
		// remove_legacy_clothing_sections() — Finding 5
		// ================================================================
		AssetObj.set_function("remove_legacy_clothing_sections", [WeakMesh, &Session](sol::table /*Self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			NS_RESOLVE_MESH_OR_FAIL("remove_legacy_clothing_sections");
			Mesh->Modify();
			Mesh->RemoveLegacyClothingSections();
			Mesh->MarkPackageDirty();
			Session.Log(TEXT("[OK] remove_legacy_clothing_sections()"));
			sol::table R = Lua.create_table();
			R["done"] = true;
			return R;
		});

		// ================================================================
		// release_skin_weight_profile_resources({unsafe_confirm_no_active_users=true}) — Finding 5
		// ================================================================
		AssetObj.set_function("release_skin_weight_profile_resources", [WeakMesh, &Session](sol::table /*Self*/,
			sol::optional<sol::table> ParamsOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			NS_RESOLVE_MESH_OR_FAIL("release_skin_weight_profile_resources");

			const bool bUnsafeConfirmNoActiveUsers = ParamsOpt.has_value()
				? ParamsOpt.value().get_or("unsafe_confirm_no_active_users", false)
				: false;
			if (!bUnsafeConfirmNoActiveUsers)
			{
				Session.Log(TEXT("[FAIL] release_skin_weight_profile_resources() -> requires {unsafe_confirm_no_active_users=true}; this low-level UE call assumes skin-weight-profile buffers are not in use anywhere."));
				return sol::lua_nil;
			}

			FAssetCompilingManager::Get().FinishCompilationForObjects({ Mesh });
			if (Mesh->IsCompiling())
			{
				Session.Log(TEXT("[FAIL] release_skin_weight_profile_resources() -> skeletal mesh is still compiling"));
				return sol::lua_nil;
			}

			int32 RegisteredComponentUsers = 0;
			int32 ActiveProfileUsers = 0;
			int32 PendingProfileUsers = 0;
			for (TObjectIterator<USkinnedMeshComponent> It; It; ++It)
			{
				USkinnedMeshComponent* Component = *It;
				if (!Component || Component->IsUnreachable() || Component->GetSkinnedAsset() != Mesh)
				{
					continue;
				}

				if (Component->IsRegistered())
				{
					++RegisteredComponentUsers;
				}
				if (Component->IsUsingSkinWeightProfile())
				{
					++ActiveProfileUsers;
				}
				if (Component->IsSkinWeightProfilePending())
				{
					++PendingProfileUsers;
				}
			}

			if (RegisteredComponentUsers > 0 || ActiveProfileUsers > 0 || PendingProfileUsers > 0)
			{
				Session.Log(FString::Printf(
					TEXT("[FAIL] release_skin_weight_profile_resources() -> mesh has %d registered component user(s), %d active skin-weight-profile user(s), and %d pending skin-weight-profile user(s); cannot prove resources are unused"),
					RegisteredComponentUsers,
					ActiveProfileUsers,
					PendingProfileUsers));
				return sol::lua_nil;
			}

			Mesh->ReleaseSkinWeightProfileResources();
			Session.Log(TEXT("[OK] release_skin_weight_profile_resources()"));
			sol::table R = Lua.create_table();
			R["done"] = true;
			R["registered_component_users"] = RegisteredComponentUsers;
			R["active_profile_users"] = ActiveProfileUsers;
			R["pending_profile_users"] = PendingProfileUsers;
			return R;
		});

		AssetObj.set_function("regenerate_lods", [WeakMesh, &Session](sol::table /*Self*/,
			sol::optional<sol::table> ParamsOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			NS_RESOLVE_MESH_OR_FAIL("regenerate_lods");
			int32 NewLODCount = 0;
			bool bRegenEvenIfImported = false;
			bool bGenerateBaseLOD = false;
			if (ParamsOpt.has_value())
			{
				sol::table P = ParamsOpt.value();
				if (P.get<sol::optional<int>>("new_lod_count").has_value()) NewLODCount = P.get<int>("new_lod_count");
				if (P.get<sol::optional<bool>>("regenerate_imported").has_value()) bRegenEvenIfImported = P.get<bool>("regenerate_imported");
				if (P.get<sol::optional<bool>>("generate_base").has_value()) bGenerateBaseLOD = P.get<bool>("generate_base");
			}
			if (!USkeletalMeshEditorSubsystem::RegenerateLOD(Mesh, NewLODCount, bRegenEvenIfImported, bGenerateBaseLOD))
			{
				Session.Log(TEXT("[FAIL] regenerate_lods() -> RegenerateLOD failed. Typically: mesh reduction module not available."));
				return sol::lua_nil;
			}
			Session.Log(FString::Printf(TEXT("[OK] regenerate_lods() -> lod_count=%d"), Mesh->GetLODNum()));
			sol::table R = Lua.create_table();
			R["lod_count"] = Mesh->GetLODNum();
			return R;
		});

		// ================================================================
		// has_mesh_description(lod_index) — round-3 Finding 3
		// ================================================================
		AssetObj.set_function("has_mesh_description", [WeakMesh, &Session](sol::table /*Self*/,
			int32 LODIndex, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			NS_RESOLVE_MESH_OR_FAIL("has_mesh_description");
#if WITH_EDITORONLY_DATA
			const bool bHas = Mesh->HasMeshDescription(LODIndex);
			Session.Log(FString::Printf(TEXT("[OK] has_mesh_description(%d) -> %s"), LODIndex, bHas ? TEXT("true") : TEXT("false")));
			sol::table R = Lua.create_table();
			R["lod_index"] = LODIndex;
			R["has_mesh_description"] = bHas;
			return R;
#else
			Session.Log(TEXT("[FAIL] has_mesh_description -> requires WITH_EDITORONLY_DATA."));
			return sol::lua_nil;
#endif
		});

		// ================================================================
		// modify_mesh_description(lod_index, always_dirty?) — round-3 Finding 3
		// Stores the LOD's mesh description for undo/redo. Call before mutating
		// externally, then commit_mesh_description() to finalise.
		// ================================================================
		AssetObj.set_function("modify_mesh_description", [WeakMesh, &Session](sol::table /*Self*/,
			int32 LODIndex, sol::optional<bool> AlwaysDirtyOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			NS_RESOLVE_MESH_OR_FAIL("modify_mesh_description");
#if WITH_EDITORONLY_DATA
			const bool bAlways = AlwaysDirtyOpt.value_or(true);
			const bool bStored = Mesh->ModifyMeshDescription(LODIndex, bAlways);
			Session.Log(FString::Printf(TEXT("[OK] modify_mesh_description(%d) -> stored=%s"), LODIndex, bStored ? TEXT("true") : TEXT("false")));
			sol::table R = Lua.create_table();
			R["lod_index"] = LODIndex;
			R["stored_in_transaction"] = bStored;
			return R;
#else
			Session.Log(TEXT("[FAIL] modify_mesh_description -> requires WITH_EDITORONLY_DATA."));
			return sol::lua_nil;
#endif
		});

		// ================================================================
		// commit_mesh_description(lod_index, opts?) — round-3 Finding 3
		// Forwards to USkeletalMesh::CommitMeshDescription(LOD, FCommitMeshDescriptionParams).
		// opts keys: update_morph_targets?, update_skin_weight_profiles?, update_vertex_attributes?,
		// update_vertex_colors?, mark_package_dirty?, force_update?.
		// ================================================================
		AssetObj.set_function("commit_mesh_description", [WeakMesh, &Session](sol::table /*Self*/,
			int32 LODIndex, sol::optional<sol::table> OptsOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			NS_RESOLVE_MESH_OR_FAIL("commit_mesh_description");
#if WITH_EDITORONLY_DATA
			USkeletalMesh::FCommitMeshDescriptionParams Params;
			if (OptsOpt.has_value())
			{
				sol::table O = OptsOpt.value();
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
				if (O.get<sol::optional<bool>>("update_morph_targets").has_value()) Params.bUpdateMorphTargets = O.get<bool>("update_morph_targets");
				if (O.get<sol::optional<bool>>("update_skin_weight_profiles").has_value()) Params.bUpdateSkinWeightProfiles = O.get<bool>("update_skin_weight_profiles");
				if (O.get<sol::optional<bool>>("update_vertex_attributes").has_value()) Params.bUpdateVertexAttributes = O.get<bool>("update_vertex_attributes");
				if (O.get<sol::optional<bool>>("update_vertex_colors").has_value()) Params.bUpdateVertexColors = O.get<bool>("update_vertex_colors");
#endif
				if (O.get<sol::optional<bool>>("mark_package_dirty").has_value()) Params.bMarkPackageDirty = O.get<bool>("mark_package_dirty");
				if (O.get<sol::optional<bool>>("force_update").has_value()) Params.bForceUpdate = O.get<bool>("force_update");
			}
			if (!Mesh->CommitMeshDescription(LODIndex, Params))
			{
				Session.Log(FString::Printf(
					TEXT("[FAIL] commit_mesh_description(%d) -> engine rejected. Check the LOD index and mesh edit/build state."),
					LODIndex));
				return sol::lua_nil;
			}
			Session.Log(FString::Printf(TEXT("[OK] commit_mesh_description(%d)"), LODIndex));
			sol::table R = Lua.create_table();
			R["lod_index"] = LODIndex;
			R["committed"] = true;
			return R;
#else
			Session.Log(TEXT("[FAIL] commit_mesh_description -> requires WITH_EDITORONLY_DATA."));
			return sol::lua_nil;
#endif
		});

		// ================================================================
		// clear_mesh_description(lod_index?) — round-3 Finding 3
		// Clears the stored mesh description for one LOD. Omit the index to clear all.
		// ================================================================
		AssetObj.set_function("clear_mesh_description", [WeakMesh, &Session](sol::table /*Self*/,
			sol::optional<int> LODIndexOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			NS_RESOLVE_MESH_OR_FAIL("clear_mesh_description");
#if WITH_EDITORONLY_DATA
			Mesh->Modify();
			if (LODIndexOpt.has_value())
			{
				Mesh->ClearMeshDescription(LODIndexOpt.value());
				Session.Log(FString::Printf(TEXT("[OK] clear_mesh_description(%d)"), LODIndexOpt.value()));
			}
			else
			{
				Mesh->ClearAllMeshDescriptions();
				Session.Log(TEXT("[OK] clear_mesh_description() -> cleared all LODs"));
			}
			Mesh->MarkPackageDirty();
			sol::table R = Lua.create_table();
			R["cleared_lod"] = LODIndexOpt.value_or(-1);
			return R;
#else
			Session.Log(TEXT("[FAIL] clear_mesh_description -> requires WITH_EDITORONLY_DATA."));
			return sol::lua_nil;
#endif
		});

		// ================================================================
		// set_num_source_models(count) — round-3 Finding 3
		// Resize the source-model array (LOD count). Fewer = discard upper LODs;
		// more = append empty LODs.
		// ================================================================
		AssetObj.set_function("set_num_source_models", [WeakMesh, &Session](sol::table /*Self*/,
			int32 Count, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			NS_RESOLVE_MESH_OR_FAIL("set_num_source_models");
			if (Count < 1)
			{
				Session.Log(FString::Printf(
					TEXT("[FAIL] set_num_source_models(%d) -> count must be >= 1 (LOD 0 is required)."), Count));
				return sol::lua_nil;
			}
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			Mesh->Modify();
			Mesh->SetNumSourceModels(Count);
			Mesh->MarkPackageDirty();
			Session.Log(FString::Printf(TEXT("[OK] set_num_source_models(%d) -> now %d"), Count, Mesh->GetNumSourceModels()));
			sol::table R = Lua.create_table();
			R["count"] = Mesh->GetNumSourceModels();
			return R;
#else
			Session.Log(TEXT("[FAIL] set_num_source_models requires UE 5.7+ (SetNumSourceModels is private pre-5.7)"));
			return sol::lua_nil;
#endif
		});

		// ================================================================
		// info()
		// ================================================================
		AssetObj.set_function("info", [WeakMesh, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			NS_RESOLVE_MESH_OR_FAIL("info");
			sol::table Result = Lua.create_table();

			const FReferenceSkeleton& RefSkel = Mesh->GetRefSkeleton();
			Result["bone_count"] = RefSkel.GetNum();
			Result["lod_count"] = Mesh->GetLODNum();

			const TArray<FSkeletalMaterial>& Materials = Mesh->GetMaterials();
			Result["material_count"] = Materials.Num();

			const TArray<TObjectPtr<UMorphTarget>>& MorphTargets = Mesh->GetMorphTargets();
			Result["morph_target_count"] = MorphTargets.Num();

			// Sockets: mesh-only + skeleton sockets
			const auto& MeshSockets = Mesh->GetMeshOnlySocketList();
			int32 SkeletonSocketCount = 0;
			USkeleton* Skeleton = Mesh->GetSkeleton();
			if (Skeleton)
			{
				SkeletonSocketCount = Skeleton->Sockets.Num();
			}
			Result["socket_count"] = MeshSockets.Num() + SkeletonSocketCount;
			Result["mesh_socket_count"] = MeshSockets.Num();
			Result["skeleton_socket_count"] = SkeletonSocketCount;

			Result["skeleton"] = Skeleton ? TCHAR_TO_UTF8(*Skeleton->GetPathName()) : "None";

			UPhysicsAsset* PhysAsset = Mesh->GetPhysicsAsset();
			Result["physics_asset"] = PhysAsset ? TCHAR_TO_UTF8(*PhysAsset->GetPathName()) : "None";

			UPhysicsAsset* ShadowPhysAsset = Mesh->GetShadowPhysicsAsset();
			Result["shadow_physics_asset"] = ShadowPhysAsset ? TCHAR_TO_UTF8(*ShadowPhysAsset->GetPathName()) : "None";

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			const FMeshNaniteSettings& NaniteSettings = Mesh->GetNaniteSettings();
			Result["nanite_enabled"] = static_cast<bool>(NaniteSettings.bEnabled);
#else
			Result["nanite_enabled"] = false;
#endif

			Result["min_lod"] = Mesh->GetMinLodIdx();
			Result["disable_below_min_lod_stripping"] = Mesh->GetDisableBelowMinLodStripping().Default;
			Result["has_vertex_colors"] = Mesh->GetHasVertexColors();

			TSubclassOf<UAnimInstance> PostProcBP = Mesh->GetPostProcessAnimBlueprint();
			if (PostProcBP)
			{
				UClass* BPClass = PostProcBP.Get();
				Result["post_process_anim_bp"] = BPClass ? TCHAR_TO_UTF8(*BPClass->GetPathName()) : "None";
			}
			else
			{
				Result["post_process_anim_bp"] = "None";
			}

			TSoftObjectPtr<UObject> DefaultRig = Mesh->GetDefaultAnimatingRig();
			Result["default_animating_rig"] = DefaultRig.IsNull() ? "None" : TCHAR_TO_UTF8(*DefaultRig.ToString());

			const FVector& PosExt = Mesh->GetPositiveBoundsExtension();
			const FVector& NegExt = Mesh->GetNegativeBoundsExtension();
			sol::table BoundsInfo = Lua.create_table();
			sol::table PosExtT = Lua.create_table();
			PosExtT["x"] = PosExt.X; PosExtT["y"] = PosExt.Y; PosExtT["z"] = PosExt.Z;
			BoundsInfo["positive_extension"] = PosExtT;
			sol::table NegExtT = Lua.create_table();
			NegExtT["x"] = NegExt.X; NegExtT["y"] = NegExt.Y; NegExtT["z"] = NegExt.Z;
			BoundsInfo["negative_extension"] = NegExtT;
			FBoxSphereBounds Bounds = Mesh->GetBounds();
			sol::table BoundsOrigin = Lua.create_table();
			BoundsOrigin["x"] = Bounds.Origin.X; BoundsOrigin["y"] = Bounds.Origin.Y; BoundsOrigin["z"] = Bounds.Origin.Z;
			BoundsInfo["origin"] = BoundsOrigin;
			sol::table BoundsExtent = Lua.create_table();
			BoundsExtent["x"] = Bounds.BoxExtent.X; BoundsExtent["y"] = Bounds.BoxExtent.Y; BoundsExtent["z"] = Bounds.BoxExtent.Z;
			BoundsInfo["box_extent"] = BoundsExtent;
			BoundsInfo["sphere_radius"] = Bounds.SphereRadius;
			Result["bounds"] = BoundsInfo;

			Result["support_ray_tracing"] = Mesh->GetSupportRayTracing();
			Result["ray_tracing_min_lod"] = Mesh->GetRayTracingMinLOD();

			UMaterialInterface* OverlayMat = Mesh->GetOverlayMaterial();
			Result["overlay_material"] = OverlayMat ? TCHAR_TO_UTF8(*OverlayMat->GetPathName()) : "None";
			Result["overlay_material_max_draw_distance"] = Mesh->GetOverlayMaterialMaxDrawDistance();

			UMeshDeformer* MeshDeformer = Mesh->GetDefaultMeshDeformer();
			Result["default_mesh_deformer"] = MeshDeformer ? TCHAR_TO_UTF8(*MeshDeformer->GetPathName()) : "None";

			Result["floor_offset"] = Mesh->GetFloorOffset();

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
			Result["forward_axis"] = TCHAR_TO_UTF8(SkeletalMeshForwardAxisToString(Mesh->GetForwardAxis().GetValue()));
#else
			Result["forward_axis"] = "Unknown";
#endif

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			Result["post_process_anim_graph_lod_threshold"] = Mesh->GetPostProcessAnimGraphLODThreshold();
#else
			Result["post_process_anim_graph_lod_threshold"] = Mesh->GetPostProcessAnimBPLODThreshold();
#endif

			Result["has_active_clothing"] = Mesh->HasActiveClothingAssets();
			const TArray<UClothingAssetBase*>& ClothingAssets = GetMeshClothingAssetsReadOnly(Mesh);
			Result["clothing_asset_count"] = ClothingAssets.Num();

			FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering();
			if (RenderData)
			{
				sol::table LODs = Lua.create_table();
				for (int32 LODIdx = 0; LODIdx < RenderData->LODRenderData.Num(); ++LODIdx)
				{
					const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[LODIdx];
					sol::table LODEntry = Lua.create_table();
					LODEntry["index"] = LODIdx;
					LODEntry["vertices"] = static_cast<int>(LODData.GetNumVertices());

					uint32 TotalTriangles = 0;
					bool bHasCloth = false;
					for (const FSkelMeshRenderSection& Section : LODData.RenderSections)
					{
						TotalTriangles += Section.NumTriangles;
						if (Section.HasClothingData()) bHasCloth = true;
					}
					LODEntry["triangles"] = static_cast<int>(TotalTriangles);
					LODEntry["sections"] = LODData.RenderSections.Num();
					LODEntry["has_cloth"] = bHasCloth;

					const FSkeletalMeshLODInfo* LODInfo = Mesh->GetLODInfo(LODIdx);
					if (LODInfo)
					{
						LODEntry["screen_size"] = LODInfo->ScreenSize.GetDefault();
						LODEntry["lod_hysteresis"] = LODInfo->LODHysteresis;
					}
					LODs[LODIdx + 1] = LODEntry;
				}
				Result["lods"] = LODs;
			}

			Session.Log(FString::Printf(TEXT("[OK] info() -> %s: %d bones, %d LODs, %d materials, %d morph targets"),
				*Mesh->GetName(), RefSkel.GetNum(), Mesh->GetLODNum(), Materials.Num(), MorphTargets.Num()));
			return Result;
		});

		// ================================================================
		// list(type, opts?)
		// ================================================================
		AssetObj.set_function("list", [WeakMesh, &Session](sol::table Self,
			sol::optional<std::string> TypeOpt, sol::optional<sol::table> OptsOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			NS_RESOLVE_MESH_OR_FAIL("list");
			FString FType = NeoLuaStr::ToFStringOpt(TypeOpt, TEXT("all"));

			// ---- list() / list("all") -> info() ----
			if (FType.Equals(TEXT("all"), ESearchCase::IgnoreCase))
			{
				sol::protected_function InfoFn = Self["info"];
				if (InfoFn.valid()) return InfoFn(Self);
				return sol::lua_nil;
			}

			// ---- list("bones") / list("bones", {tree=true}) / list("bones", {transforms=true}) ----
			if (FType.Equals(TEXT("bones"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("bone"), ESearchCase::IgnoreCase))
			{
				const FReferenceSkeleton& RefSkel = Mesh->GetRefSkeleton();
				const TArray<FMeshBoneInfo>& BoneInfos = RefSkel.GetRefBoneInfo();
				const TArray<FTransform>& RefBonePose = RefSkel.GetRefBonePose();
				int32 NumBones = RefSkel.GetNum();

				bool bTree = false;
				bool bTransforms = false;
				if (OptsOpt.has_value())
				{
					bTree = OptsOpt.value().get_or("tree", false);
					bTransforms = OptsOpt.value().get_or("transforms", false);
				}

				if (bTree)
				{
					TMap<int32, TArray<int32>> ChildrenMap;
					TArray<int32> RootBones;
					for (int32 i = 0; i < NumBones; ++i)
					{
						int32 ParentIdx = BoneInfos[i].ParentIndex;
						if (ParentIdx == INDEX_NONE) RootBones.Add(i);
						else ChildrenMap.FindOrAdd(ParentIdx).Add(i);
					}

					TFunction<sol::table(int32)> BuildBoneTree = [&](int32 BoneIndex) -> sol::table
					{
						sol::table BoneEntry = Lua.create_table();
						BoneEntry["index"] = BoneIndex;
						BoneEntry["name"] = TCHAR_TO_UTF8(*BoneInfos[BoneIndex].Name.ToString());
						BoneEntry["parent_index"] = BoneInfos[BoneIndex].ParentIndex;
						if (BoneInfos[BoneIndex].ParentIndex >= 0 && BoneInfos[BoneIndex].ParentIndex < NumBones)
							BoneEntry["parent_name"] = TCHAR_TO_UTF8(*BoneInfos[BoneInfos[BoneIndex].ParentIndex].Name.ToString());
						else
							BoneEntry["parent_name"] = "None";

						const TArray<int32>* ChildIndices = ChildrenMap.Find(BoneIndex);
						if (ChildIndices && ChildIndices->Num() > 0)
						{
							sol::table Children = Lua.create_table();
							int32 ChildIdx = 1;
							for (int32 ChildBoneIdx : *ChildIndices)
								Children[ChildIdx++] = BuildBoneTree(ChildBoneIdx);
							BoneEntry["children"] = Children;
						}
						return BoneEntry;
					};

					sol::table Result = Lua.create_table();
					int32 RootIdx = 1;
					for (int32 RootBoneIdx : RootBones)
						Result[RootIdx++] = BuildBoneTree(RootBoneIdx);

					Session.Log(FString::Printf(TEXT("[OK] list(\"bones\", {tree=true}) -> %d bones"), NumBones));
					return Result;
				}
				else
				{
					sol::table Result = Lua.create_table();
					for (int32 i = 0; i < NumBones; ++i)
					{
						sol::table E = Lua.create_table();
						E["index"] = i;
						E["name"] = TCHAR_TO_UTF8(*BoneInfos[i].Name.ToString());
						E["parent_index"] = BoneInfos[i].ParentIndex;
						if (BoneInfos[i].ParentIndex >= 0 && BoneInfos[i].ParentIndex < NumBones)
							E["parent_name"] = TCHAR_TO_UTF8(*BoneInfos[BoneInfos[i].ParentIndex].Name.ToString());
						else
							E["parent_name"] = "None";

						if (bTransforms && i < RefBonePose.Num())
						{
							const FTransform& BoneTransform = RefBonePose[i];
							sol::table Loc = Lua.create_table();
							FVector Translation = BoneTransform.GetTranslation();
							Loc["x"] = Translation.X; Loc["y"] = Translation.Y; Loc["z"] = Translation.Z;
							E["location"] = Loc;
							sol::table Rot = Lua.create_table();
							FRotator Rotation = BoneTransform.Rotator();
							Rot["pitch"] = Rotation.Pitch; Rot["yaw"] = Rotation.Yaw; Rot["roll"] = Rotation.Roll;
							E["rotation"] = Rot;
							sol::table Scl = Lua.create_table();
							FVector Scale = BoneTransform.GetScale3D();
							Scl["x"] = Scale.X; Scl["y"] = Scale.Y; Scl["z"] = Scale.Z;
							E["scale"] = Scl;
						}
						Result[i + 1] = E;
					}

					Session.Log(FString::Printf(TEXT("[OK] list(\"bones\"%s) -> %d"), bTransforms ? TEXT(", {transforms=true}") : TEXT(""), NumBones));
					return Result;
				}
			}

			// ---- list("materials") ----
			if (FType.Equals(TEXT("materials"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("material"), ESearchCase::IgnoreCase))
			{
				const TArray<FSkeletalMaterial>& Materials = Mesh->GetMaterials();
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Materials.Num(); ++i)
				{
					sol::table E = Lua.create_table();
					E["index"] = i;
					E["slot_name"] = TCHAR_TO_UTF8(*Materials[i].MaterialSlotName.ToString());
					if (Materials[i].MaterialInterface)
					{
						E["material_path"] = TCHAR_TO_UTF8(*Materials[i].MaterialInterface->GetPathName());
						E["material_name"] = TCHAR_TO_UTF8(*Materials[i].MaterialInterface->GetName());
					}
					else
					{
						E["material_path"] = "None";
						E["material_name"] = "None";
					}
					// Overlay material (Finding 7 coverage). 5.7+ only.
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
					if (UMaterialInterface* Overlay = Materials[i].OverlayMaterialInterface)
					{
						E["overlay_material_path"] = TCHAR_TO_UTF8(*Overlay->GetPathName());
						E["overlay_material_name"] = TCHAR_TO_UTF8(*Overlay->GetName());
					}
					else
					{
						E["overlay_material_path"] = "None";
					}
#else
					E["overlay_material_path"] = "None";
#endif
					Result[i + 1] = E;
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"materials\") -> %d"), Materials.Num()));
				return Result;
			}

			// ---- list("sockets") ----
			if (FType.Equals(TEXT("sockets"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("socket"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				int32 Idx = 1;

				auto EmitSocket = [&Result, &Idx, &Lua](USkeletalMeshSocket* Socket, const TCHAR* Source)
				{
					if (!Socket) return;
					sol::table E = Lua.create_table();
					E["name"] = TCHAR_TO_UTF8(*Socket->SocketName.ToString());
					E["bone_name"] = TCHAR_TO_UTF8(*Socket->BoneName.ToString());
					E["source"] = TCHAR_TO_UTF8(Source);
					sol::table Loc = Lua.create_table();
					Loc["x"] = Socket->RelativeLocation.X; Loc["y"] = Socket->RelativeLocation.Y; Loc["z"] = Socket->RelativeLocation.Z;
					E["location"] = Loc;
					sol::table Rot = Lua.create_table();
					Rot["pitch"] = Socket->RelativeRotation.Pitch; Rot["yaw"] = Socket->RelativeRotation.Yaw; Rot["roll"] = Socket->RelativeRotation.Roll;
					E["rotation"] = Rot;
					sol::table Scale = Lua.create_table();
					Scale["x"] = Socket->RelativeScale.X; Scale["y"] = Socket->RelativeScale.Y; Scale["z"] = Socket->RelativeScale.Z;
					E["scale"] = Scale;
					E["force_always_animated"] = Socket->bForceAlwaysAnimated;
					Result[Idx++] = E;
				};

				for (USkeletalMeshSocket* Socket : Mesh->GetMeshOnlySocketList()) EmitSocket(Socket, TEXT("mesh"));
				if (USkeleton* Skeleton = Mesh->GetSkeleton())
					for (USkeletalMeshSocket* Socket : Skeleton->Sockets) EmitSocket(Socket, TEXT("skeleton"));

				Session.Log(FString::Printf(TEXT("[OK] list(\"sockets\") -> %d"), Idx - 1));
				return Result;
			}

			// ---- list("morph_targets") ----
			if (FType.Equals(TEXT("morph_targets"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("morph_target"), ESearchCase::IgnoreCase))
			{
				const TArray<TObjectPtr<UMorphTarget>>& MorphTargets = Mesh->GetMorphTargets();
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < MorphTargets.Num(); ++i)
				{
					sol::table E = Lua.create_table();
					E["index"] = i;
					E["name"] = MorphTargets[i] ? TCHAR_TO_UTF8(*MorphTargets[i]->GetName()) : "(null)";
					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"morph_targets\") -> %d"), MorphTargets.Num()));
				return Result;
			}

			// ---- list("generated_morph_targets") — Finding 7 ----
			if (FType.Equals(TEXT("generated_morph_targets"), ESearchCase::IgnoreCase))
			{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
				TArray<FName> OutNames;
				USkeletalMeshEditorSubsystem::GetMorphTargetsGeneratedByEngine(Mesh, OutNames);
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < OutNames.Num(); ++i)
				{
					sol::table E = Lua.create_table();
					E["name"] = TCHAR_TO_UTF8(*OutNames[i].ToString());
					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"generated_morph_targets\") -> %d"), OutNames.Num()));
				return Result;
#else
				Session.Log(TEXT("[FAIL] list(\"generated_morph_targets\") requires UE 5.7+"));
				return sol::lua_nil;
#endif
			}

			// ---- list("lods") ----
			if (FType.Equals(TEXT("lods"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("lod"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering();

				if (RenderData)
				{
					for (int32 LODIdx = 0; LODIdx < RenderData->LODRenderData.Num(); ++LODIdx)
					{
						const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[LODIdx];
						// Per-LOD LODMaterialMap remaps section material slots (see FSkeletalMeshLODInfo).
						const FSkeletalMeshLODInfo* LODInfoForMap = Mesh->GetLODInfo(LODIdx);
						sol::table E = Lua.create_table();
						E["index"] = LODIdx;
						E["vertices"] = static_cast<int>(LODData.GetNumVertices());

						uint32 TotalTriangles = 0;
						bool bHasCloth = false;
						sol::table Sections = Lua.create_table();
						for (int32 SecIdx = 0; SecIdx < LODData.RenderSections.Num(); ++SecIdx)
						{
							const FSkelMeshRenderSection& Section = LODData.RenderSections[SecIdx];
							TotalTriangles += Section.NumTriangles;
							if (Section.HasClothingData()) bHasCloth = true;

							int32 ResolvedMaterialIndex = Section.MaterialIndex;
							if (LODInfoForMap && LODInfoForMap->LODMaterialMap.IsValidIndex(SecIdx))
							{
								ResolvedMaterialIndex = LODInfoForMap->LODMaterialMap[SecIdx];
							}

							sol::table SecEntry = Lua.create_table();
							SecEntry["index"] = SecIdx;
							SecEntry["material_index"] = ResolvedMaterialIndex;
							SecEntry["raw_material_index"] = static_cast<int>(Section.MaterialIndex);
							SecEntry["vertices"] = static_cast<int>(Section.NumVertices);
							SecEntry["triangles"] = static_cast<int>(Section.NumTriangles);
							SecEntry["max_bone_influences"] = Section.MaxBoneInfluences;
							SecEntry["has_cloth"] = Section.HasClothingData();
							SecEntry["disabled"] = Section.bDisabled;
							Sections[SecIdx + 1] = SecEntry;
						}

						E["triangles"] = static_cast<int>(TotalTriangles);
						E["section_count"] = LODData.RenderSections.Num();
						E["sections"] = Sections;
						E["has_cloth"] = bHasCloth;

						const FSkeletalMeshLODInfo* LODInfo = Mesh->GetLODInfo(LODIdx);
						if (LODInfo)
						{
							E["screen_size"] = LODInfo->ScreenSize.GetDefault();
							E["lod_hysteresis"] = LODInfo->LODHysteresis;
							E["has_been_simplified"] = static_cast<bool>(LODInfo->bHasBeenSimplified);
							E["weight_of_prioritization"] = LODInfo->WeightOfPrioritization;
							E["morph_target_position_error_tolerance"] = LODInfo->MorphTargetPositionErrorTolerance;

							sol::table BRemove = Lua.create_table();
							for (int32 bi = 0; bi < LODInfo->BonesToRemove.Num(); ++bi)
								BRemove[bi + 1] = TCHAR_TO_UTF8(*LODInfo->BonesToRemove[bi].BoneName.ToString());
							E["bones_to_remove"] = BRemove;
							E["bones_to_remove_count"] = LODInfo->BonesToRemove.Num();

							sol::table BPrioritize = Lua.create_table();
							for (int32 bi = 0; bi < LODInfo->BonesToPrioritize.Num(); ++bi)
								BPrioritize[bi + 1] = TCHAR_TO_UTF8(*LODInfo->BonesToPrioritize[bi].BoneName.ToString());
							E["bones_to_prioritize"] = BPrioritize;
							E["bones_to_prioritize_count"] = LODInfo->BonesToPrioritize.Num();
						}
						Result[LODIdx + 1] = E;
					}
				}
				else
				{
					for (int32 LODIdx = 0; LODIdx < Mesh->GetLODNum(); ++LODIdx)
					{
						sol::table E = Lua.create_table();
						E["index"] = LODIdx;
						const FSkeletalMeshLODInfo* LODInfo = Mesh->GetLODInfo(LODIdx);
						if (LODInfo)
						{
							E["screen_size"] = LODInfo->ScreenSize.GetDefault();
							E["lod_hysteresis"] = LODInfo->LODHysteresis;
							E["has_been_simplified"] = static_cast<bool>(LODInfo->bHasBeenSimplified);
						}
						Result[LODIdx + 1] = E;
					}
				}

				int32 Count = RenderData ? RenderData->LODRenderData.Num() : Mesh->GetLODNum();
				Session.Log(FString::Printf(TEXT("[OK] list(\"lods\") -> %d"), Count));
				return Result;
			}

			// ---- list("sections", {lod}) — Finding 7 ----
			if (FType.Equals(TEXT("sections"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("section"), ESearchCase::IgnoreCase))
			{
				if (!OptsOpt.has_value() || !OptsOpt.value().get<sol::optional<int>>("lod").has_value())
				{
					Session.Log(TEXT("[FAIL] list(\"sections\") -> opts {lod=N} required. Call list(\"lods\") for valid LOD indices."));
					return sol::lua_nil;
				}
				int32 LodIndex = OptsOpt.value().get<int>("lod");
				FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering();
				if (!RenderData || !RenderData->LODRenderData.IsValidIndex(LodIndex))
				{
					Session.Log(FString::Printf(
						TEXT("[FAIL] list(\"sections\", {lod=%d}) -> LOD render data unavailable or index out of range (render_lods=%d). Call build({wait=true}) if the mesh was just mutated, then retry; call list(\"lods\") for valid LOD indices."),
						LodIndex, RenderData ? RenderData->LODRenderData.Num() : 0));
					return sol::lua_nil;
				}
				USkeletalMeshEditorSubsystem* Subsys = GetSkelMeshEditorSubsystem();
				const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[LodIndex];
				const FSkeletalMeshLODInfo* LODInfoForMap = Mesh->GetLODInfo(LodIndex);
				sol::table Result = Lua.create_table();
				for (int32 SecIdx = 0; SecIdx < LODData.RenderSections.Num(); ++SecIdx)
				{
					const FSkelMeshRenderSection& Section = LODData.RenderSections[SecIdx];
					int32 ResolvedMaterialIndex = Section.MaterialIndex;
					if (LODInfoForMap && LODInfoForMap->LODMaterialMap.IsValidIndex(SecIdx))
					{
						ResolvedMaterialIndex = LODInfoForMap->LODMaterialMap[SecIdx];
					}
					sol::table E = Lua.create_table();
					E["index"] = SecIdx;
					E["material_index"] = ResolvedMaterialIndex;
					E["raw_material_index"] = static_cast<int>(Section.MaterialIndex);
					E["vertices"] = static_cast<int>(Section.NumVertices);
					E["triangles"] = static_cast<int>(Section.NumTriangles);
					E["max_bone_influences"] = Section.MaxBoneInfluences;
					E["has_cloth"] = Section.HasClothingData();
					E["disabled"] = Section.bDisabled;
					if (Subsys)
					{
						bool bCastShadow = false;
						bool bRecomputeTangent = false;
						bool bVisibleInRT = false;
						uint8 MaskChannel = 0;
						if (Subsys->GetSectionCastShadow(Mesh, LodIndex, SecIdx, bCastShadow)) E["cast_shadow"] = bCastShadow;
						if (Subsys->GetSectionRecomputeTangent(Mesh, LodIndex, SecIdx, bRecomputeTangent)) E["recompute_tangent"] = bRecomputeTangent;
						if (Subsys->GetSectionVisibleInRayTracing(Mesh, LodIndex, SecIdx, bVisibleInRT)) E["visible_in_ray_tracing"] = bVisibleInRT;
						if (Subsys->GetSectionRecomputeTangentsVertexMaskChannel(Mesh, LodIndex, SecIdx, MaskChannel)) E["recompute_tangents_vertex_mask_channel"] = MaskChannel;
					}
					Result[SecIdx + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"sections\", {lod=%d}) -> %d sections"), LodIndex, LODData.RenderSections.Num()));
				return Result;
			}

			// ---- list("clothing") ----
			if (FType.Equals(TEXT("clothing"), ESearchCase::IgnoreCase))
			{
				const TArray<UClothingAssetBase*>& ClothingAssets = GetMeshClothingAssetsReadOnly(Mesh);
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < ClothingAssets.Num(); ++i)
				{
					sol::table E = Lua.create_table();
					E["index"] = i;
					if (ClothingAssets[i])
					{
						E["name"] = TCHAR_TO_UTF8(*ClothingAssets[i]->GetName());
						E["guid"] = TCHAR_TO_UTF8(*ClothingAssets[i]->GetAssetGuid().ToString());
					}
					else
					{
						E["name"] = "(null)";
						E["guid"] = "";
					}
					Result[i + 1] = E;
				}

				Session.Log(FString::Printf(TEXT("[OK] list(\"clothing\") -> %d"), ClothingAssets.Num()));
				return Result;
			}

			// ---- list("clothing_in_use") — Finding 5: GetClothingAssetsInUse ----
			if (FType.Equals(TEXT("clothing_in_use"), ESearchCase::IgnoreCase))
			{
				TArray<UClothingAssetBase*> InUse;
				Mesh->GetClothingAssetsInUse(InUse);
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < InUse.Num(); ++i)
				{
					sol::table E = Lua.create_table();
					E["index"] = i;
					if (InUse[i])
					{
						E["name"] = TCHAR_TO_UTF8(*InUse[i]->GetName());
						E["guid"] = TCHAR_TO_UTF8(*InUse[i]->GetAssetGuid().ToString());
					}
					else
					{
						E["name"] = "(null)";
						E["guid"] = "";
					}
					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"clothing_in_use\") -> %d"), InUse.Num()));
				return Result;
			}

			// ---- list("section_clothing", {lod, section}) — Finding 5: GetSectionClothingAsset ----
			if (FType.Equals(TEXT("section_clothing"), ESearchCase::IgnoreCase))
			{
				if (!OptsOpt.has_value()
					|| !OptsOpt.value().get<sol::optional<int>>("lod").has_value()
					|| !OptsOpt.value().get<sol::optional<int>>("section").has_value())
				{
					Session.Log(TEXT("[FAIL] list(\"section_clothing\") -> opts {lod, section} required. Call list(\"sections\", {lod=N}) for valid section indices."));
					return sol::lua_nil;
				}
				int32 LodIdx = OptsOpt.value().get<int>("lod");
				int32 SecIdx = OptsOpt.value().get<int>("section");
				UClothingAssetBase* Bound = Mesh->GetSectionClothingAsset(LodIdx, SecIdx);
				sol::table Result = Lua.create_table();
				Result["lod"] = LodIdx;
				Result["section"] = SecIdx;
				if (Bound)
				{
					Result["name"] = TCHAR_TO_UTF8(*Bound->GetName());
					Result["guid"] = TCHAR_TO_UTF8(*Bound->GetAssetGuid().ToString());
					Result["bound"] = true;
				}
				else
				{
					Result["bound"] = false;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"section_clothing\", {lod=%d, section=%d}) -> %s"),
					LodIdx, SecIdx, Bound ? *Bound->GetName() : TEXT("(none)")));
				return Result;
			}

			// ---- list("clothing_active_for_lod", {lod}) — Finding 5: HasActiveClothingAssetsForLOD ----
			if (FType.Equals(TEXT("clothing_active_for_lod"), ESearchCase::IgnoreCase))
			{
				if (!OptsOpt.has_value() || !OptsOpt.value().get<sol::optional<int>>("lod").has_value())
				{
					Session.Log(TEXT("[FAIL] list(\"clothing_active_for_lod\") -> opts {lod=N} required."));
					return sol::lua_nil;
				}
				int32 LodIdx = OptsOpt.value().get<int>("lod");
				const bool bActive = Mesh->HasActiveClothingAssetsForLOD(LodIdx);
				sol::table Result = Lua.create_table();
				Result["lod"] = LodIdx;
				Result["active"] = bActive;
				Session.Log(FString::Printf(TEXT("[OK] list(\"clothing_active_for_lod\", {lod=%d}) -> %s"),
					LodIdx, bActive ? TEXT("true") : TEXT("false")));
				return Result;
			}

			// ---- list("skin_weight_profiles", {details=true}) — Finding 5 + round-3 Finding 4 ----
			if (FType.Equals(TEXT("skin_weight_profiles"), ESearchCase::IgnoreCase))
			{
				const bool bDetails = OptsOpt.has_value() ? OptsOpt.value().get_or("details", false) : false;
				sol::table Result = Lua.create_table();
				if (bDetails)
				{
					// GetSkinWeightProfiles exposes the full FSkinWeightProfileInfo array
					// (name, DefaultProfile, DefaultProfileFromLODIndex).
					const TArray<FSkinWeightProfileInfo>& Profiles = Mesh->GetSkinWeightProfiles();
					for (int32 i = 0; i < Profiles.Num(); ++i)
					{
						sol::table E = Lua.create_table();
						E["name"] = TCHAR_TO_UTF8(*Profiles[i].Name.ToString());
						E["default_profile"] = Profiles[i].DefaultProfile.Default;
						E["default_profile_from_lod"] = Profiles[i].DefaultProfileFromLODIndex.Default;
						Result[i + 1] = E;
					}
					Session.Log(FString::Printf(TEXT("[OK] list(\"skin_weight_profiles\", {details=true}) -> %d"), Profiles.Num()));
				}
				else
				{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
					TArray<FString> Names = Mesh->K2_GetAllSkinWeightProfileNames();
#else
					// Pre-5.7: derive names from FSkinWeightProfileInfo array instead.
					TArray<FString> Names;
					for (const FSkinWeightProfileInfo& Info : Mesh->GetSkinWeightProfiles())
					{
						Names.Add(Info.Name.ToString());
					}
#endif
					for (int32 i = 0; i < Names.Num(); ++i)
					{
						sol::table E = Lua.create_table();
						E["name"] = TCHAR_TO_UTF8(*Names[i]);
						Result[i + 1] = E;
					}
					Session.Log(FString::Printf(TEXT("[OK] list(\"skin_weight_profiles\") -> %d"), Names.Num()));
				}
				return Result;
			}

			// ---- list("vertex_colors", {lod?=0}) — round-4 Finding 6: GetVertexColorData ----
			// Returns {index, x, y, z, r, g, b, a} rows of painted vertex colors.
			if (FType.Equals(TEXT("vertex_colors"), ESearchCase::IgnoreCase))
			{
				int32 Lod = 0;
				if (OptsOpt.has_value() && OptsOpt.value().get<sol::optional<int>>("lod").has_value())
					Lod = OptsOpt.value().get<int>("lod");
				TMap<FVector3f, FColor> ColorData = Mesh->GetVertexColorData(static_cast<uint32>(Lod));
				sol::table Result = Lua.create_table();
				int32 i = 1;
				for (const TPair<FVector3f, FColor>& Pair : ColorData)
				{
					sol::table E = Lua.create_table();
					E["x"] = Pair.Key.X;
					E["y"] = Pair.Key.Y;
					E["z"] = Pair.Key.Z;
					E["r"] = static_cast<int>(Pair.Value.R);
					E["g"] = static_cast<int>(Pair.Value.G);
					E["b"] = static_cast<int>(Pair.Value.B);
					E["a"] = static_cast<int>(Pair.Value.A);
					Result[i++] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"vertex_colors\", {lod=%d}) -> %d unique positions"), Lod, ColorData.Num()));
				return Result;
			}

			// ---- list("source_models") — round-3 Finding 3 observability for FSkeletalMeshSourceModel ----
			if (FType.Equals(TEXT("source_models"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("source_model"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				const int32 NumSrc = Mesh->GetNumSourceModels();
				for (int32 i = 0; i < NumSrc; ++i)
				{
					sol::table E = Lua.create_table();
					E["index"] = i;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
					E["is_valid"] = Mesh->IsSourceModelValid(i);
#else
					// Pre-5.7: no IsSourceModelValid; treat any in-range index as valid (NumSrc bounds the loop).
					E["is_valid"] = true;
#endif
#if WITH_EDITORONLY_DATA
					E["has_mesh_description"] = Mesh->HasMeshDescription(i);
#else
					E["has_mesh_description"] = false;
#endif
					if (const FSkeletalMeshLODInfo* Info = Mesh->GetLODInfo(i))
					{
						E["is_simplified"] = static_cast<bool>(Info->bHasBeenSimplified);
						E["screen_size"] = Info->ScreenSize.GetDefault();
					}
					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"source_models\") -> %d"), NumSrc));
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: bones, materials, sockets, morph_targets, generated_morph_targets, lods, sections, clothing, clothing_in_use, section_clothing, clothing_active_for_lod, skin_weight_profiles, source_models, vertex_colors"), *FType));
			return sol::lua_nil;
		});

		// ----------------------------------------------------------------
		// get_clothing_asset(index_or_name) -> sub-object with get/set/list_properties
		// Typed sol::object dispatch via NeoLuaProperty::ApplySolValueToProperty (Finding 9).
		// Weak pointers for GC safety (Finding 1).
		// ----------------------------------------------------------------
		AssetObj.set_function("get_clothing_asset", [WeakMesh, &Session](sol::table /*self*/,
			sol::object Identifier, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			NS_RESOLVE_MESH_OR_FAIL("get_clothing_asset");

			const TArray<UClothingAssetBase*>& ClothingAssets = GetMeshClothingAssetsReadOnly(Mesh);
			if (ClothingAssets.Num() == 0)
			{
				Session.Log(TEXT("[FAIL] get_clothing_asset -> no clothing assets attached. Attach via Skeletal Mesh Editor > Clothing panel first."));
				return sol::lua_nil;
			}

			UClothingAssetBase* FoundCloth = nullptr;

			if (Identifier.is<int>())
			{
				int32 ZeroIndex = Identifier.as<int>() - 1;
				if (ZeroIndex >= 0 && ZeroIndex < ClothingAssets.Num())
				{
					FoundCloth = ClothingAssets[ZeroIndex];
				}
				else
				{
					Session.Log(FString::Printf(
						TEXT("[FAIL] get_clothing_asset(%d) -> index out of range (count=%d). Call list(\"clothing\")."),
						ZeroIndex + 1, ClothingAssets.Num()));
					return sol::lua_nil;
				}
			}
			else if (Identifier.is<std::string>())
			{
				FString SearchName = NeoLuaStr::ToFString(Identifier.as<std::string>());
				for (UClothingAssetBase* CA : ClothingAssets)
				{
					if (CA && CA->GetName().Contains(SearchName, ESearchCase::IgnoreCase))
					{
						FoundCloth = CA;
						break;
					}
				}
				if (!FoundCloth)
				{
					Session.Log(FString::Printf(
						TEXT("[FAIL] get_clothing_asset(\"%s\") -> not found. Call list(\"clothing\") to see available names."), *SearchName));
					return sol::lua_nil;
				}
			}
			else
			{
				Session.Log(TEXT("[FAIL] get_clothing_asset -> pass name (string) or index (number, 1-based)"));
				return sol::lua_nil;
			}

			if (!FoundCloth)
			{
				Session.Log(TEXT("[FAIL] get_clothing_asset -> clothing asset entry is null/stale. Reopen the mesh and call list(\"clothing\") to choose a valid entry."));
				return sol::lua_nil;
			}

			TWeakObjectPtr<UClothingAssetBase> WeakCloth(FoundCloth);

			sol::table ClothObj = Lua.create_table();
			ClothObj["name"] = TCHAR_TO_UTF8(*FoundCloth->GetName());
			ClothObj["class_name"] = TCHAR_TO_UTF8(*FoundCloth->GetClass()->GetName());

			// get(property) — typed read via NeoLuaProperty::ReadPropertyAsSol
			ClothObj.set_function("get", [WeakCloth, &Session](sol::table /*self*/,
				const std::string& PropertyName, sol::this_state S) -> sol::object
			{
				sol::state_view Lua(S);
				UClothingAssetBase* Cloth = WeakCloth.Get();
				if (!Cloth)
				{
					Session.Log(TEXT("[FAIL] clothing:get -> clothing asset no longer valid. Reacquire with mesh:get_clothing_asset(name_or_index)."));
					return sol::lua_nil;
				}
				FString FProp = NeoLuaStr::ToFString(PropertyName);
				FProperty* Prop = NeoLuaProperty::FindPropertyByName(Cloth->GetClass(), FProp);
				if (!Prop)
				{
					Session.Log(FString::Printf(
						TEXT("[FAIL] clothing:get(\"%s\") -> property not found. Call clothing:list_properties(filter?) to discover valid names."), *FProp));
					return sol::lua_nil;
				}
				return NeoLuaProperty::ReadPropertyAsSol(Prop, Prop->ContainerPtrToValuePtr<void>(Cloth), Lua);
			});

			// set(property, value_any) — typed sol::object dispatch (Finding 9).
			ClothObj.set_function("set", [WeakCloth, WeakMesh, &Session](sol::table /*self*/,
				const std::string& PropertyName, sol::object Value, sol::this_state S) -> sol::object
			{
				sol::state_view Lua(S);
				UClothingAssetBase* Cloth = WeakCloth.Get();
				USkeletalMesh* Mesh = WeakMesh.Get();
				if (!Cloth)
				{
					Session.Log(TEXT("[FAIL] clothing:set -> clothing asset no longer valid. Reacquire with mesh:get_clothing_asset(name_or_index)."));
					return sol::lua_nil;
				}
				FString FProp = NeoLuaStr::ToFString(PropertyName);
				FProperty* Prop = NeoLuaProperty::FindPropertyByName(Cloth->GetClass(), FProp);
				if (!Prop)
				{
					Session.Log(FString::Printf(
						TEXT("[FAIL] clothing:set(\"%s\") -> property not found. Call clothing:list_properties(filter?) to discover valid names."), *FProp));
					return sol::lua_nil;
				}

				Cloth->Modify();
				Cloth->PreEditChange(Prop);

				FString Error;
				bool bOk = false;
				FString ValueDesc;

				if (Value.is<std::string>())
				{
					// String path → existing helper (ImportText-based).
					FString FValue = NeoLuaStr::ToFString(Value.as<std::string>());
					ValueDesc = FValue;
					bOk = NeoLuaProperty::SetPropertyValueFromStringSafe(
						Prop,
						Prop->ContainerPtrToValuePtr<void>(Cloth),
						Cloth,
						FValue,
						Error);
				}
				else
				{
					// Typed Lua value (table/number/bool) → registered struct handler dispatch.
					ValueDesc = TEXT("<table|number|bool>");
					bOk = NeoLuaProperty::ApplySolValueToProperty(
						Prop,
						Prop->ContainerPtrToValuePtr<void>(Cloth),
						Cloth,
						Value,
						Error);
				}

				if (!bOk)
				{
					FPropertyChangedEvent FailEvent(Prop, EPropertyChangeType::ValueSet);
					Cloth->PostEditChangeProperty(FailEvent);
					Session.Log(FString::Printf(TEXT("[FAIL] clothing:set(\"%s\", %s) -> %s"), *FProp, *ValueDesc, *Error));
					return sol::lua_nil;
				}

				if (Mesh) Mesh->MarkPackageDirty();
				FPropertyChangedEvent SuccessEvent(Prop, EPropertyChangeType::ValueSet);
				Cloth->PostEditChangeProperty(SuccessEvent);

				Session.Log(FString::Printf(TEXT("[OK] clothing:set(\"%s\") = %s"), *FProp, *ValueDesc));
				return sol::make_object(Lua, true);
			});

			// list_properties(filter?)
			ClothObj.set_function("list_properties", [WeakCloth, &Session](sol::table /*self*/,
				sol::optional<std::string> Filter, sol::this_state S) -> sol::object
			{
				sol::state_view Lua(S);
				UClothingAssetBase* Cloth = WeakCloth.Get();
				if (!Cloth)
				{
					Session.Log(TEXT("[FAIL] clothing:list_properties -> clothing asset no longer valid. Reacquire with mesh:get_clothing_asset(name_or_index)."));
					return sol::lua_nil;
				}

				FString FFilter = NeoLuaStr::ToFStringOpt(Filter);
				sol::table Result = Lua.create_table();
				int32 Index = 1;

				for (TFieldIterator<FProperty> PropIt(Cloth->GetClass()); PropIt; ++PropIt)
				{
					FProperty* Property = *PropIt;
					if (Property->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient)) continue;

					FString Name = Property->GetName();
					if (!FFilter.IsEmpty() && !Name.Contains(FFilter, ESearchCase::IgnoreCase)) continue;

					FString Type = NeoStackToolUtils::GetPropertyTypeName(Property);
					FString Value = NeoStackToolUtils::GetPropertyValueAsString(Cloth, Property, Cloth);
					FString Category = Property->GetMetaData(TEXT("Category"));
					if (Category.IsEmpty()) Category = TEXT("Default");
					if (Value.Len() > 120) Value = Value.Left(117) + TEXT("...");

					sol::table Entry = Lua.create_table();
					Entry["name"] = TCHAR_TO_UTF8(*Name);
					Entry["type"] = TCHAR_TO_UTF8(*Type);
					Entry["value"] = TCHAR_TO_UTF8(*Value);
					Entry["category"] = TCHAR_TO_UTF8(*Category);
					Result[Index++] = Entry;
				}

				Session.Log(FString::Printf(TEXT("[OK] clothing:list_properties(%s) -> %d properties"),
					FFilter.IsEmpty() ? TEXT("*") : *FFilter, Index - 1));
				return Result;
			});

			Session.Log(FString::Printf(TEXT("[OK] get_clothing_asset(\"%s\") -> sub-object with get/set/list_properties"),
				*FoundCloth->GetName()));
			return ClothObj;
		});
	});
}

REGISTER_LUA_BINDING(SkeletalMesh, SkeletalMeshDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindSkeletalMesh(Lua, Session);
});
