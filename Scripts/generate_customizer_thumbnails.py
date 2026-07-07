import json
import math
import os
import re
import tempfile

import unreal


DATA_TABLE_PATH = "/Game/UI/Customizer/DT_VNHCustomizationItems.DT_VNHCustomizationItems"
THUMBNAIL_ROOT = "/Game/UI/CharacterCustomizer/Thumbnails"
RAPID_THUMBNAIL_BLUEPRINT = "/GmRapidThumbnailCreator/Blueprints/EUB_ThumbnailCreator"
RAPID_THUMBNAIL_PNG_TARGET = "/GmRapidThumbnailCreator/Materials/RT/RT_ThumbnailCreatorPNG"
RAPID_THUMBNAIL_FRAMING_PADDING = 1.20
REPORT_DIR = os.path.join(unreal.Paths.project_saved_dir(), "CodexReports")
REPORT_PATH = os.path.join(REPORT_DIR, "customizer_thumbnail_generation.json")
UPDATE_TSV_PATH = os.path.join(REPORT_DIR, "customizer_thumbnail_updates.tsv")


def log(message):
    unreal.log("[VNH Thumbnail Generator] " + str(message))


def ensure_directory(path):
    if not unreal.EditorAssetLibrary.does_directory_exist(path):
        unreal.EditorAssetLibrary.make_directory(path)


def sanitize_asset_name(name):
    name = re.sub(r"[^A-Za-z0-9_]+", "_", str(name))
    name = re.sub(r"_+", "_", name).strip("_")
    return name or "Unnamed"


def soft_object_path_to_asset_path(value):
    text = str(value or "")
    if not text or text == "None":
        return ""
    if "'" in text:
        # Handles formats like SkeletalMesh'/Game/Path.Asset'
        text = text.split("'", 1)[1].rsplit("'", 1)[0]
    return text


def icon_is_set(value):
    text = str(value or "")
    return bool(text and text != "None")


def configure_icon_texture(texture):
    texture.set_editor_property("mip_gen_settings", unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)
    texture.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_EDITOR_ICON)
    texture.set_editor_property("lod_group", unreal.TextureGroup.TEXTUREGROUP_UI)
    texture.set_editor_property("address_x", unreal.TextureAddress.TA_CLAMP)
    texture.set_editor_property("address_y", unreal.TextureAddress.TA_CLAMP)
    texture.set_editor_property("never_stream", True)
    texture.set_editor_property("srgb", True)


def find_enum_value(enum_type_name, name_fragment):
    enum_type = getattr(unreal, enum_type_name, None)
    if not enum_type:
        raise RuntimeError(f"Unreal enum {enum_type_name} is unavailable")

    normalized_fragment = name_fragment.replace("_", "").lower()
    for attribute_name in dir(enum_type):
        if normalized_fragment in attribute_name.replace("_", "").lower():
            return getattr(enum_type, attribute_name)
    raise RuntimeError(f"Could not find {name_fragment} in Unreal enum {enum_type_name}")


class RapidThumbnailPluginStage:
    """Headless adapter around GmRapidThumbnailCreator's capture actor."""

    @staticmethod
    def _set_first_property(target, names, value):
        last_error = None
        for name in names:
            try:
                target.set_editor_property(name, value)
                return
            except Exception as exc:
                last_error = exc
        raise RuntimeError(f"Could not set any of {names}: {last_error}")

    def __init__(self):
        actor_class = unreal.EditorAssetLibrary.load_blueprint_class(RAPID_THUMBNAIL_BLUEPRINT)
        if not actor_class:
            raise RuntimeError(f"Could not load {RAPID_THUMBNAIL_BLUEPRINT}")

        self.actor = unreal.EditorLevelLibrary.spawn_actor_from_class(
            actor_class,
            # The plugin's Blueprint auto-fit logic uses world-space bounds as
            # relative offsets, so its capture actor must remain at the origin.
            unreal.Vector(0.0, 0.0, 0.0),
            unreal.Rotator(0.0, 0.0, 0.0),
        )
        if not self.actor:
            raise RuntimeError("Could not spawn GmRapidThumbnailCreator capture actor")

        self.actor.set_actor_label("VNH_TempRapidThumbnailCreator")
        self.actor.set_editor_property("display_obj_type", find_enum_value("GmDisplayObjectType", "ESKM"))
        self.actor.set_editor_property("export_type", find_enum_value("GmExportType", "PNG"))
        self.actor.set_editor_property("texture_res_width", 512)
        self.actor.set_editor_property("texture_res_height", 512)
        self._set_first_property(
            self.actor,
            ("bExportLocalDesktop", "b_export_local_desktop", "export_local_desktop"),
            False,
        )
        self._set_first_property(
            self.actor,
            ("bSuppressSaveConfirmation", "b_suppress_save_confirmation", "suppress_save_confirmation"),
            True,
        )
        self._set_first_property(
            self.actor,
            ("bEnableBackground", "b_enable_background", "enable_background"),
            True,
        )
        self.actor.set_editor_property("background_color", unreal.LinearColor(0.004, 0.012, 0.016, 1.0))
        self.actor.set_editor_property("object_transparency", 0.0)

        self.capture_component = self.actor.get_editor_property("scene_capture2d_component")
        self.render_target = self.actor.get_editor_property("render_target2d_png")
        if not self.render_target:
            self.render_target = unreal.EditorAssetLibrary.load_asset(RAPID_THUMBNAIL_PNG_TARGET)
            if self.render_target:
                self.actor.set_editor_property("render_target2d_png", self.render_target)
        if not self.capture_component or not self.render_target:
            raise RuntimeError("GmRapidThumbnailCreator capture components are unavailable")

        self.capture_component.set_editor_property("texture_target", self.render_target)
        self._configure_lighting()
        self._configure_post_process()

    def _configure_lighting(self):
        light_settings = (
            ("center_point_light", 1800.0),
            ("near_by_point_light", 1100.0),
            ("distant_point_light", 700.0),
        )
        for property_name, intensity in light_settings:
            light = self.actor.get_editor_property(property_name)
            if light:
                light.set_visibility(True)
                light.set_intensity(intensity)

    def _configure_post_process(self):
        settings = self.capture_component.get_editor_property("post_process_settings")
        overrides = {
            "override_auto_exposure_method": True,
            "auto_exposure_method": unreal.AutoExposureMethod.AEM_MANUAL,
            "override_auto_exposure_bias": True,
            "auto_exposure_bias": 0.0,
            "override_auto_exposure_apply_physical_camera_exposure": True,
            "auto_exposure_apply_physical_camera_exposure": False,
            "override_bloom_intensity": True,
            "bloom_intensity": 0.0,
            "override_lens_flare_intensity": True,
            "lens_flare_intensity": 0.0,
            "override_motion_blur_amount": True,
            "motion_blur_amount": 0.0,
            "override_vignette_intensity": True,
            "vignette_intensity": 0.0,
        }
        for property_name, value in overrides.items():
            settings.set_editor_property(property_name, value)
        self.capture_component.set_editor_property("post_process_settings", settings)
        self.capture_component.set_editor_property("post_process_blend_weight", 1.0)

    def capture_asset_to_texture(self, category, asset_path, mesh_asset_name):
        mesh = unreal.EditorAssetLibrary.load_asset(asset_path)
        if not mesh:
            return "", "mesh asset failed to load"

        category_folder = f"{THUMBNAIL_ROOT}/{sanitize_asset_name(category)}"
        ensure_directory(category_folder)
        asset_name = "T_" + sanitize_asset_name(mesh_asset_name)
        result_path = f"{category_folder}/{asset_name}.{asset_name}"
        if unreal.EditorAssetLibrary.does_asset_exist(result_path):
            return result_path, "existing"

        try:
            self.actor.set_editor_property("skeletal_mesh_asset", mesh)
            self.actor.give_life()

            # GiveLife auto-fits the plugin stage and may derive its own output
            # name, so restore our deterministic path after the fit pass.
            skeletal_component = self.actor.get_editor_property("skeletal_mesh_comp")
            meshes_location = self.actor.get_editor_property("meshes_loc")
            if skeletal_component and meshes_location:
                bounds_origin, _, _ = unreal.SystemLibrary.get_component_bounds(skeletal_component)
                relative_transform = meshes_location.get_relative_transform()
                relative_location = relative_transform.translation
                meshes_location.set_relative_location(
                    unreal.Vector(
                        relative_location.x,
                        relative_location.y,
                        relative_location.z - bounds_origin.z,
                    ),
                    False,
                    False,
                )
            spring_arm = self.actor.get_editor_property("spring_arm_comp")
            if spring_arm:
                fitted_distance = spring_arm.get_editor_property("target_arm_length")
                spring_arm.set_editor_property(
                    "target_arm_length",
                    fitted_distance * RAPID_THUMBNAIL_FRAMING_PADDING,
                )
            self.actor.set_editor_property("export_path", category_folder + "/")
            self.actor.set_editor_property("output_file_name", asset_name)
            self.capture_component.set_editor_property("texture_target", self.render_target)
            self.actor.update_camera_capture_render()
            self.capture_component.capture_scene()
            self.capture_component.capture_scene()
            self.actor.save_static_texture()

            if not unreal.EditorAssetLibrary.does_asset_exist(result_path):
                return "", "plugin did not create the expected texture asset"

            texture = unreal.EditorAssetLibrary.load_asset(result_path)
            if texture:
                configure_icon_texture(texture)
                unreal.EditorAssetLibrary.save_loaded_asset(texture)
            unreal.EditorAssetLibrary.save_asset(result_path, only_if_is_dirty=False)
            return result_path, "GmRapidThumbnailCreator"
        except Exception as exc:
            return "", str(exc)

    def destroy(self):
        if self.actor:
            self.actor.destroy_actor()
            self.actor = None


class ThumbnailCaptureStage:
    def __init__(self):
        self.preview_origin = unreal.Vector(60000.0, 60000.0, 60000.0)
        self.spawned = []
        self.world = unreal.EditorLevelLibrary.get_editor_world()
        self.render_target = unreal.RenderingLibrary.create_render_target2d(
            self.world,
            512,
            512,
            unreal.TextureRenderTargetFormat.RTF_RGBA8,
        )
        self.render_target.set_editor_property("clear_color", unreal.LinearColor(0.004, 0.012, 0.016, 1.0))

        self.mesh_actor = unreal.EditorLevelLibrary.spawn_actor_from_class(
            unreal.SkeletalMeshActor,
            self.preview_origin,
            unreal.Rotator(0.0, -90.0, 0.0),
        )
        self.spawned.append(self.mesh_actor)
        self.mesh_actor.set_actor_label("VNH_TempCustomizerThumbnailMesh")
        self.mesh_actor.skeletal_mesh_component.set_editor_property("forced_lod_model", 1)
        self.mesh_actor.skeletal_mesh_component.set_editor_property("cast_shadow", True)

        self.backdrop_actor = unreal.EditorLevelLibrary.spawn_actor_from_class(
            unreal.StaticMeshActor,
            self.preview_origin,
            unreal.Rotator(0.0, 0.0, 0.0),
        )
        self.spawned.append(self.backdrop_actor)
        self.backdrop_actor.set_actor_label("VNH_TempCustomizerThumbnailBackdrop")
        backdrop_mesh = unreal.EditorAssetLibrary.load_asset("/Engine/BasicShapes/Cube.Cube")
        self.backdrop_actor.static_mesh_component.set_static_mesh(backdrop_mesh)
        backdrop_parent = unreal.EditorAssetLibrary.load_asset("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")
        if backdrop_parent:
            backdrop_material = self.backdrop_actor.static_mesh_component.create_dynamic_material_instance(
                0,
                backdrop_parent,
                "VNH_ThumbnailBackdrop",
            )
            backdrop_material.set_vector_parameter_value(
                "Color",
                unreal.LinearColor(0.003, 0.010, 0.014, 1.0),
            )
            self.backdrop_actor.static_mesh_component.set_material(0, backdrop_material)
        self.backdrop_actor.static_mesh_component.set_editor_property("cast_shadow", False)

        self.capture_actor = unreal.EditorLevelLibrary.spawn_actor_from_class(
            unreal.SceneCapture2D,
            unreal.Vector(self.preview_origin.x + 300.0, self.preview_origin.y, self.preview_origin.z),
            unreal.Rotator(0.0, 180.0, 0.0),
        )
        self.spawned.append(self.capture_actor)
        self.capture_actor.set_actor_label("VNH_TempCustomizerThumbnailCapture")
        self.capture_component = self.capture_actor.capture_component2d
        self.capture_component.set_editor_property("fov_angle", 30.0)
        self.capture_component.set_editor_property("capture_every_frame", False)
        self.capture_component.set_editor_property("capture_on_movement", False)
        self.capture_component.set_editor_property("texture_target", self.render_target)
        if hasattr(unreal, "SceneCaptureSource"):
            self.capture_component.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
        self.capture_component.set_editor_property(
            "primitive_render_mode",
            unreal.SceneCapturePrimitiveRenderMode.PRM_USE_SHOW_ONLY_LIST,
        )
        self.capture_component.show_only_actor_components(self.mesh_actor, True)
        self.capture_component.show_only_actor_components(self.backdrop_actor, True)
        self._configure_post_process()

        self.key_light = unreal.EditorLevelLibrary.spawn_actor_from_class(
            unreal.PointLight,
            unreal.Vector(self.preview_origin.x + 150.0, self.preview_origin.y - 150.0, self.preview_origin.z + 150.0),
            unreal.Rotator(0.0, 0.0, 0.0),
        )
        self.spawned.append(self.key_light)
        self.key_light.set_actor_label("VNH_TempCustomizerThumbnailKeyLight")

        self.fill_light = unreal.EditorLevelLibrary.spawn_actor_from_class(
            unreal.PointLight,
            unreal.Vector(self.preview_origin.x + 120.0, self.preview_origin.y + 150.0, self.preview_origin.z + 100.0),
            unreal.Rotator(0.0, 0.0, 0.0),
        )
        self.spawned.append(self.fill_light)
        self.fill_light.set_actor_label("VNH_TempCustomizerThumbnailFillLight")

    def _configure_post_process(self):
        settings = self.capture_component.get_editor_property("post_process_settings")
        overrides = {
            "override_auto_exposure_method": True,
            "auto_exposure_method": unreal.AutoExposureMethod.AEM_MANUAL,
            "override_auto_exposure_bias": True,
            "auto_exposure_bias": 0.0,
            "override_auto_exposure_apply_physical_camera_exposure": True,
            "auto_exposure_apply_physical_camera_exposure": False,
            "override_bloom_intensity": True,
            "bloom_intensity": 0.0,
            "override_lens_flare_intensity": True,
            "lens_flare_intensity": 0.0,
            "override_motion_blur_amount": True,
            "motion_blur_amount": 0.0,
            "override_vignette_intensity": True,
            "vignette_intensity": 0.0,
        }
        for property_name, value in overrides.items():
            settings.set_editor_property(property_name, value)
        self.capture_component.set_editor_property("post_process_settings", settings)
        self.capture_component.set_editor_property("post_process_blend_weight", 1.0)

    def capture_asset_to_png(self, asset_path, png_path):
        mesh = unreal.EditorAssetLibrary.load_asset(asset_path)
        if not mesh:
            return False, "mesh asset failed to load"

        try:
            self.mesh_actor.skeletal_mesh_component.set_skeletal_mesh(mesh)
            self.mesh_actor.set_actor_location(self.preview_origin, False, False)
            self.mesh_actor.set_actor_rotation(unreal.Rotator(0.0, -90.0, 0.0), False)

            origin, extent = self.mesh_actor.get_actor_bounds(False)
            fov = 30.0
            max_extent = max(extent.x, extent.y, extent.z, 10.0)
            distance = max_extent / math.tan(math.radians(fov * 0.5)) * 1.28
            camera_location = unreal.Vector(origin.x + distance, origin.y, origin.z)

            self.capture_actor.set_actor_location(camera_location, False, False)
            self.capture_actor.set_actor_rotation(unreal.Rotator(0.0, 180.0, 0.0), False)
            self.capture_component.set_editor_property("fov_angle", fov)

            self.backdrop_actor.set_actor_location(
                unreal.Vector(origin.x - max_extent * 2.0, origin.y, origin.z),
                False,
                False,
            )
            backdrop_span = max(max_extent * 8.0, 400.0)
            self.backdrop_actor.set_actor_scale3d(
                unreal.Vector(0.02, backdrop_span / 100.0, backdrop_span / 100.0)
            )

            self.key_light.set_actor_location(
                unreal.Vector(origin.x + distance * 0.45, origin.y - max_extent * 1.2, origin.z + max_extent * 1.1),
                False,
                False,
            )
            self.key_light.point_light_component.set_editor_property("intensity", 25.0)
            self.key_light.point_light_component.set_editor_property("attenuation_radius", max_extent * 8.0)

            self.fill_light.set_actor_location(
                unreal.Vector(origin.x + distance * 0.25, origin.y + max_extent * 1.3, origin.z + max_extent * 0.55),
                False,
                False,
            )
            self.fill_light.point_light_component.set_editor_property("intensity", 8.0)
            self.fill_light.point_light_component.set_editor_property("attenuation_radius", max_extent * 8.0)

            # Rebuild the show-only list after swapping meshes so the capture
            # cannot include the loaded level's sky, floor, or nearby actors.
            self.capture_component.clear_show_only_components()
            self.capture_component.show_only_actor_components(self.mesh_actor, True)
            self.capture_component.show_only_actor_components(self.backdrop_actor, True)

            # The first capture updates render state after the mesh swap. The
            # second is the deterministic frame exported to disk.
            self.capture_component.capture_scene()
            self.capture_component.capture_scene()
            unreal.RenderingLibrary.export_render_target(self.world, self.render_target, os.path.dirname(png_path), os.path.basename(png_path))
            return os.path.exists(png_path), "scene_capture_export_render_target"
        except Exception as exc:
            return False, str(exc)

    def destroy(self):
        self.mesh_actor.skeletal_mesh_component.set_skeletal_mesh(None)
        for actor in reversed(self.spawned):
            if actor:
                actor.destroy_actor()
        self.spawned = []


def capture_asset_to_png(asset_path, png_path, stage=None):
    if stage:
        return stage.capture_asset_to_png(asset_path, png_path)

    stage = ThumbnailCaptureStage()
    try:
        return stage.capture_asset_to_png(asset_path, png_path)
    finally:
        stage.destroy()


def import_png_as_texture(category, source_png, mesh_asset_name):
    ensure_directory(THUMBNAIL_ROOT)
    category_folder = f"{THUMBNAIL_ROOT}/{sanitize_asset_name(category)}"
    ensure_directory(category_folder)

    asset_name = "T_" + sanitize_asset_name(mesh_asset_name)
    asset_path = f"{category_folder}/{asset_name}.{asset_name}"
    if unreal.EditorAssetLibrary.does_asset_exist(asset_path):
        return asset_path

    task = unreal.AssetImportTask()
    task.filename = source_png
    task.destination_path = category_folder
    task.destination_name = asset_name
    task.automated = True
    task.save = True
    task.replace_existing = False

    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    imported = task.imported_object_paths
    if imported:
        texture_path = imported[0]
        texture = unreal.EditorAssetLibrary.load_asset(texture_path)
        if texture:
            configure_icon_texture(texture)
            unreal.EditorAssetLibrary.save_loaded_asset(texture)
        return texture_path

    return ""


def set_icon_on_row(data_table_path, row_name, icon_path):
    # DataTableFunctionLibrary cannot write rows from Python in all UE versions.
    # Use the same editor CSV import pathway Unreal uses for DataTables by
    # modifying only through the row struct object when available.
    dt = unreal.EditorAssetLibrary.load_asset(data_table_path)
    if hasattr(unreal.DataTableFunctionLibrary, "fill_data_table_from_json_string"):
        # Not used here because it replaces the whole table and is too risky for
        # an incremental thumbnail fill operation.
        pass
    return False


def main(max_rows=20, dry_run=False):
    os.makedirs(REPORT_DIR, exist_ok=True)
    open(UPDATE_TSV_PATH, "w", encoding="utf-8").write("")

    data_table = unreal.EditorAssetLibrary.load_asset(DATA_TABLE_PATH)
    if not data_table:
        raise RuntimeError(f"Could not load DataTable {DATA_TABLE_PATH}")

    row_names = list(unreal.DataTableFunctionLibrary.get_data_table_row_names(data_table))
    result = {
        "data_table": DATA_TABLE_PATH,
        "thumbnail_root": THUMBNAIL_ROOT,
        "capture_backend": "dry_run" if dry_run else "",
        "dry_run": dry_run,
        "max_rows": max_rows,
        "rows_seen": len(row_names),
        "generated": [],
        "skipped_existing_icon": 0,
        "skipped_no_mesh": 0,
        "failed": [],
        "needs_icon_write": [],
        "note": "Generator captures/imports thumbnails incrementally. Row Icon writes are handled by the Lua MCP DataTable API after import.",
    }

    # DataTable rows are easiest and safest to inspect through exported JSON.
    export_json = unreal.DataTableFunctionLibrary.export_data_table_to_json_string(data_table)
    if not export_json:
        result["failed"].append({"row": "", "reason": "DataTable export_as_json unavailable"})
        with open(REPORT_PATH, "w", encoding="utf-8") as f:
            json.dump(result, f, indent=2)
        return result

    parsed = json.loads(export_json)
    if isinstance(parsed, list):
        parsed_rows = parsed
    elif isinstance(parsed, dict):
        parsed_rows = []
        for name, values in parsed.items():
            row = dict(values)
            row.setdefault("Name", name)
            parsed_rows.append(row)
    else:
        raise RuntimeError("Unexpected DataTable JSON shape")

    processed = 0
    temp_dir = tempfile.mkdtemp(prefix="vnh_customizer_thumbs_")
    pending_updates = []
    capture_stage = None
    if not dry_run:
        try:
            capture_stage = RapidThumbnailPluginStage()
            result["capture_backend"] = "GmRapidThumbnailCreator"
        except Exception as exc:
            log(f"Rapid Thumbnail Creator unavailable; using fallback capture stage: {exc}")
            capture_stage = ThumbnailCaptureStage()
            result["capture_backend"] = "VNH SceneCapture fallback"

    try:
        for row in parsed_rows:
            row_name = str(row.get("Name", ""))
            if not row_name:
                continue

            configured_icon_path = soft_object_path_to_asset_path(row.get("Icon"))
            if configured_icon_path and unreal.EditorAssetLibrary.does_asset_exist(configured_icon_path):
                result["skipped_existing_icon"] += 1
                continue

            mesh_path = soft_object_path_to_asset_path(row.get("Mesh"))
            if not mesh_path:
                result["skipped_no_mesh"] += 1
                continue

            category = row.get("Category", "Misc")
            mesh_asset_name = mesh_path.split(".")[-1]
            expected_asset_name = "T_" + sanitize_asset_name(mesh_asset_name)
            expected_asset_path = f"{THUMBNAIL_ROOT}/{sanitize_asset_name(category)}/{expected_asset_name}.{expected_asset_name}"
            if configured_icon_path and configured_icon_path != expected_asset_path:
                result["failed"].append(
                    {
                        "row": row_name,
                        "mesh": mesh_path,
                        "reason": (
                            "configured Icon path does not match the deterministic generator path: "
                            f"{configured_icon_path} != {expected_asset_path}"
                        ),
                    }
                )
                continue

            if unreal.EditorAssetLibrary.does_asset_exist(expected_asset_path):
                result["needs_icon_write"].append({"row": str(row_name), "icon": expected_asset_path})
                pending_updates.append((str(row_name), expected_asset_path))
                processed += 1
                if max_rows and processed >= max_rows:
                    break
                continue

            if dry_run:
                result["generated"].append({"row": str(row_name), "mesh": mesh_path, "icon": expected_asset_path, "dry_run": True})
                processed += 1
                if max_rows and processed >= max_rows:
                    break
                continue

            capture_reason = ""
            if isinstance(capture_stage, RapidThumbnailPluginStage):
                icon_path, capture_reason = capture_stage.capture_asset_to_texture(
                    category,
                    mesh_path,
                    mesh_asset_name,
                )
            else:
                png_path = os.path.join(temp_dir, expected_asset_name + ".png")
                ok, capture_reason = capture_asset_to_png(mesh_path, png_path, capture_stage)
                icon_path = import_png_as_texture(category, png_path, mesh_asset_name) if ok else ""

            if icon_path:
                result["generated"].append(
                    {
                        "row": str(row_name),
                        "mesh": mesh_path,
                        "icon": icon_path,
                        "capture_backend": capture_reason,
                    }
                )
                result["needs_icon_write"].append({"row": str(row_name), "icon": icon_path})
                pending_updates.append((str(row_name), icon_path))
            else:
                result["failed"].append(
                    {
                        "row": str(row_name),
                        "mesh": mesh_path,
                        "reason": capture_reason or "capture/import produced no texture",
                    }
                )

            processed += 1
            if max_rows and processed >= max_rows:
                break
    finally:
        if capture_stage:
            capture_stage.destroy()

    with open(UPDATE_TSV_PATH, "w", encoding="utf-8") as f:
        for row_name, icon_path in pending_updates:
            f.write(f"{row_name}\t{icon_path}\n")

    with open(REPORT_PATH, "w", encoding="utf-8") as f:
        json.dump(result, f, indent=2)
    log(f"Wrote report: {REPORT_PATH}")
    return result


if __name__ == "__main__":
    main()
