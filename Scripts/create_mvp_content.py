import math
import unreal


MAP_PATH = "/Game/Maps/MVP_ClothingStore"
BP_PATH = "/Game/Blueprints"


def enum_value(enum_type, name):
    return getattr(enum_type, name.upper())


def ensure_directory(path):
    if not unreal.EditorAssetLibrary.does_directory_exist(path):
        unreal.EditorAssetLibrary.make_directory(path)


def create_blueprint(asset_name, parent_class):
    asset_path = f"{BP_PATH}/{asset_name}"
    existing = unreal.EditorAssetLibrary.load_asset(asset_path)
    if existing:
        return existing

    factory = unreal.BlueprintFactory()
    factory.set_editor_property("parent_class", parent_class)
    blueprint = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        asset_name,
        BP_PATH,
        unreal.Blueprint,
        factory,
    )
    unreal.EditorAssetLibrary.save_loaded_asset(blueprint)
    return blueprint


def spawn_actor(actor_class, label, location, rotation=None, scale=None):
    rotation = rotation or unreal.Rotator(0.0, 0.0, 0.0)
    actor = unreal.EditorLevelLibrary.spawn_actor_from_class(actor_class, location, rotation)
    actor.set_actor_label(label)
    if scale:
        actor.set_actor_scale3d(scale)
    return actor


def spawn_cube(label, location, scale):
    cube_mesh = unreal.EditorAssetLibrary.load_asset("/Engine/BasicShapes/Cube.Cube")
    actor = spawn_actor(unreal.StaticMeshActor, label, location, scale=scale)
    actor.static_mesh_component.set_static_mesh(cube_mesh)
    actor.static_mesh_component.set_editor_property("mobility", unreal.ComponentMobility.STATIC)
    return actor


def make_waypoint(bp_class, label, location, context, suggested, held_prop, wait_seconds):
    waypoint = spawn_actor(bp_class, label, location)
    waypoint.set_editor_property("Context", context)
    waypoint.set_editor_property("SuggestedNextActivity", suggested)
    waypoint.set_editor_property("HeldProp", held_prop)
    waypoint.set_editor_property("WaitSeconds", wait_seconds)
    return waypoint


def assign_waypoints(shopper, waypoints):
    routine = shopper.get_component_by_class(unreal.VNHRoutineComponent)
    if routine:
        routine.set_editor_property("RoutineWaypoints", waypoints)


def main():
    ensure_directory("/Game/Maps")
    ensure_directory(BP_PATH)

    game_mode_bp = create_blueprint(
        "BP_VNHGameMode",
        unreal.load_class(None, "/Script/VNHSimulator.VNHGameMode"),
    )
    shopper_bp = create_blueprint(
        "BP_VNHShopperCharacter",
        unreal.load_class(None, "/Script/VNHSimulator.VNHShopperCharacter"),
    )
    waypoint_bp = create_blueprint(
        "BP_VNHShopperWaypoint",
        unreal.load_class(None, "/Script/VNHSimulator.VNHShopperWaypoint"),
    )

    if unreal.EditorAssetLibrary.does_asset_exist(MAP_PATH):
        unreal.EditorLevelLibrary.load_level(MAP_PATH)
    else:
        unreal.EditorLevelLibrary.new_level(MAP_PATH)

    world = unreal.EditorLevelLibrary.get_editor_world()
    world_settings = world.get_world_settings()
    world_settings.set_editor_property("default_game_mode", game_mode_bp.generated_class)

    # Clothing store greybox: floor, walls, checkout, racks, mirrors, and a clear entrance lane.
    spawn_cube("SM_Floor_Shop", unreal.Vector(0.0, 0.0, -55.0), unreal.Vector(12.0, 8.0, 0.1))
    spawn_cube("SM_Wall_Back", unreal.Vector(0.0, -400.0, 100.0), unreal.Vector(12.0, 0.15, 2.0))
    spawn_cube("SM_Wall_Left", unreal.Vector(-600.0, 0.0, 100.0), unreal.Vector(0.15, 8.0, 2.0))
    spawn_cube("SM_Wall_Right", unreal.Vector(600.0, 0.0, 100.0), unreal.Vector(0.15, 8.0, 2.0))
    spawn_cube("SM_Checkout_Counter", unreal.Vector(350.0, -280.0, 15.0), unreal.Vector(2.2, 0.55, 0.7))
    spawn_cube("SM_Mirror_Left", unreal.Vector(-560.0, -230.0, 80.0), unreal.Vector(0.05, 1.4, 1.6))
    spawn_cube("SM_Mirror_Right", unreal.Vector(560.0, -230.0, 80.0), unreal.Vector(0.05, 1.4, 1.6))

    for index, location in enumerate(
        [
            unreal.Vector(-300.0, -120.0, 0.0),
            unreal.Vector(0.0, -120.0, 0.0),
            unreal.Vector(300.0, -120.0, 0.0),
            unreal.Vector(-300.0, 160.0, 0.0),
            unreal.Vector(0.0, 160.0, 0.0),
            unreal.Vector(300.0, 160.0, 0.0),
        ],
        start=1,
    ):
        spawn_cube(f"SM_ClothingRack_{index:02d}", location, unreal.Vector(0.9, 0.2, 0.75))

    nav = spawn_actor(unreal.NavMeshBoundsVolume, "NAV_MVPShop", unreal.Vector(0.0, 0.0, 100.0))
    nav.set_actor_scale3d(unreal.Vector(14.0, 10.0, 3.0))

    spawn_actor(unreal.PlayerStart, "PS_HunterEntrance", unreal.Vector(0.0, 480.0, 20.0), unreal.Rotator(0.0, 180.0, 0.0))
    spawn_actor(unreal.PointLight, "L_KeyShopLight", unreal.Vector(0.0, 0.0, 450.0))

    context = unreal.EVNHShopperContext
    waypoint_class = waypoint_bp.generated_class
    loops = [
        [
            make_waypoint(waypoint_class, "WP_A_Browse", unreal.Vector(-330.0, -70.0, 0.0), enum_value(context, "Browsing"), "Mirror", "BlueJacket", 2.0),
            make_waypoint(waypoint_class, "WP_A_Mirror", unreal.Vector(-500.0, -210.0, 0.0), enum_value(context, "Mirror"), "Browsing", "BlueJacket", 2.5),
            make_waypoint(waypoint_class, "WP_A_Idle", unreal.Vector(-190.0, 230.0, 0.0), enum_value(context, "Idle"), "Browsing", "BlueJacket", 1.5),
        ],
        [
            make_waypoint(waypoint_class, "WP_B_Browse", unreal.Vector(20.0, -70.0, 0.0), enum_value(context, "Browsing"), "Checkout", "RedShirt", 2.0),
            make_waypoint(waypoint_class, "WP_B_Checkout", unreal.Vector(250.0, -300.0, 0.0), enum_value(context, "Checkout"), "Browsing", "RedShirt", 3.0),
            make_waypoint(waypoint_class, "WP_B_Walk", unreal.Vector(-50.0, 250.0, 0.0), enum_value(context, "Walking"), "Browsing", "RedShirt", 1.0),
        ],
        [
            make_waypoint(waypoint_class, "WP_C_Browse", unreal.Vector(340.0, -70.0, 0.0), enum_value(context, "Browsing"), "Mirror", "Phone", 1.8),
            make_waypoint(waypoint_class, "WP_C_Mirror", unreal.Vector(500.0, -210.0, 0.0), enum_value(context, "Mirror"), "Checkout", "Phone", 2.2),
            make_waypoint(waypoint_class, "WP_C_Idle", unreal.Vector(230.0, 250.0, 0.0), enum_value(context, "Idle"), "Browsing", "Phone", 1.5),
        ],
        [
            make_waypoint(waypoint_class, "WP_D_Browse", unreal.Vector(-330.0, 210.0, 0.0), enum_value(context, "Browsing"), "Checkout", "Drink", 2.0),
            make_waypoint(waypoint_class, "WP_D_Checkout", unreal.Vector(430.0, -300.0, 0.0), enum_value(context, "Checkout"), "Browsing", "Drink", 2.5),
            make_waypoint(waypoint_class, "WP_D_Walk", unreal.Vector(50.0, 300.0, 0.0), enum_value(context, "Walking"), "Browsing", "Drink", 1.0),
        ],
    ]

    shopper_class = shopper_bp.generated_class
    shopper_locations = [
        unreal.Vector(-320.0, -20.0, 90.0),
        unreal.Vector(20.0, -20.0, 90.0),
        unreal.Vector(320.0, -20.0, 90.0),
        unreal.Vector(-320.0, 260.0, 90.0),
        unreal.Vector(60.0, 260.0, 90.0),
        unreal.Vector(340.0, 220.0, 90.0),
        unreal.Vector(-120.0, -260.0, 90.0),
        unreal.Vector(180.0, -260.0, 90.0),
    ]

    for index, location in enumerate(shopper_locations):
        shopper = spawn_actor(shopper_class, f"BP_Shopper_{index + 1:02d}", location, unreal.Rotator(0.0, math.fmod(index * 47.0, 360.0), 0.0))
        assign_waypoints(shopper, loops[index % len(loops)])

    unreal.EditorLoadingAndSavingUtils.save_dirty_packages(True, True)
    unreal.log("VNH MVP content generation complete.")


if __name__ == "__main__":
    main()
