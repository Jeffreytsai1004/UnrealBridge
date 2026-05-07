"""
UnrealBridge helper functions for common editor operations.

These are pre-loaded in the UE Python environment and can be called
directly from scripts sent via the bridge.

Usage from bridge:
    python bridge.py exec "from unreal_bridge_helpers import *; print(list_assets('/Game/Props'))"
"""

import unreal


def list_assets(path, class_filter=None, recursive=True):
    """List all assets under a content path.

    Args:
        path: Content path (e.g. '/Game/Props')
        class_filter: Optional class name to filter by (e.g. 'StaticMesh')
        recursive: Whether to search subdirectories

    Returns:
        List of asset path strings.
    """
    asset_registry = unreal.AssetRegistryHelpers.get_asset_registry()

    if recursive:
        assets = asset_registry.get_assets_by_path(path, recursive=True)
    else:
        assets = asset_registry.get_assets_by_path(path, recursive=False)

    paths = []
    for asset_data in assets:
        if class_filter and str(asset_data.asset_class_path.asset_name) != class_filter:
            continue
        paths.append(str(asset_data.get_full_name()))

    return paths


def get_selected_actors():
    """Get all currently selected actors in the level editor.

    Returns:
        List of dicts with actor info (name, class, location, rotation).
    """
    subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = subsystem.get_selected_level_actors()

    result = []
    for actor in actors:
        loc = actor.get_actor_location()
        rot = actor.get_actor_rotation()
        result.append({
            "name": actor.get_name(),
            "label": actor.get_actor_label(),
            "class": actor.get_class().get_name(),
            "location": {"x": loc.x, "y": loc.y, "z": loc.z},
            "rotation": {"pitch": rot.pitch, "yaw": rot.yaw, "roll": rot.roll},
        })

    return result


def find_actors_by_class(class_name):
    """Find all actors of a given class in the current level.

    Args:
        class_name: Unreal class name (e.g. 'StaticMeshActor', 'PointLight')

    Returns:
        List of actor names.
    """
    subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = subsystem.get_all_level_actors()
    return [
        a.get_actor_label()
        for a in actors
        if a.get_class().get_name() == class_name
    ]


def get_actor_properties(actor_label):
    """Get properties of an actor by its label.

    Args:
        actor_label: The actor's display label in the editor.

    Returns:
        Dict of property names to values.
    """
    subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = subsystem.get_all_level_actors()

    for actor in actors:
        if actor.get_actor_label() == actor_label:
            loc = actor.get_actor_location()
            rot = actor.get_actor_rotation()
            scale = actor.get_actor_scale3d()
            return {
                "name": actor.get_name(),
                "label": actor.get_actor_label(),
                "class": actor.get_class().get_name(),
                "location": {"x": loc.x, "y": loc.y, "z": loc.z},
                "rotation": {"pitch": rot.pitch, "yaw": rot.yaw, "roll": rot.roll},
                "scale": {"x": scale.x, "y": scale.y, "z": scale.z},
                "is_hidden": actor.is_hidden_ed(),
                "folder_path": str(actor.get_folder_path()),
            }

    return None


def set_actor_transform(actor_label, location=None, rotation=None, scale=None):
    """Set the transform of an actor by its label.

    Args:
        actor_label: The actor's display label.
        location: Optional dict {"x":, "y":, "z":} or None to keep current.
        rotation: Optional dict {"pitch":, "yaw":, "roll":} or None.
        scale: Optional dict {"x":, "y":, "z":} or None.

    Returns:
        True if the actor was found and updated.
    """
    subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = subsystem.get_all_level_actors()

    for actor in actors:
        if actor.get_actor_label() == actor_label:
            if location:
                actor.set_actor_location(
                    unreal.Vector(location["x"], location["y"], location["z"]),
                    False, False
                )
            if rotation:
                actor.set_actor_rotation(
                    unreal.Rotator(rotation["pitch"], rotation["yaw"], rotation["roll"]),
                    False
                )
            if scale:
                actor.set_actor_scale3d(
                    unreal.Vector(scale["x"], scale["y"], scale["z"])
                )
            return True

    return False


def scaffold_enhanced_input_pawn(bp_path, ia_action_map, parent_class=None,
                                 imc_path=None, save=True):
    """Scaffold a Pawn-derived Blueprint with Enhanced Input bindings.

    For each entry in `ia_action_map`, ensures a function graph exists and
    wires the IA's chosen trigger event to a CallFunction node for that
    function — i.e. the BP equivalent of `BindAction(IA, Triggered, this,
    &Pawn::Move)` in C++. Caller is responsible for filling in the actual
    function bodies afterwards.

    Args:
        bp_path: Content path of the Pawn BP, e.g. "/Game/MyPawn/BP_Hero".
                 Created (as `parent_class` or APawn) if missing.
        ia_action_map: Dict {ia_path: (trigger_event, function_name)}.
                       trigger_event ∈ {"Triggered", "Started", "Ongoing",
                       "Canceled", "Completed"}.
        parent_class: UClass for newly-created BPs (default unreal.Pawn).
        imc_path: Optional. Recorded in the result dict; IMC application
                  is currently the caller's responsibility (use
                  AddMappingContext at runtime, or set
                  DefaultPawnInputMappingContext on the PlayerController).
                  A future revision may generate the BeginPlay graph that
                  pushes the IMC onto the local-player subsystem.
        save: Save the BP package after authoring. Default True.

    Returns:
        Dict { "blueprint": <path>,
               "imc": <path or None>,
               "wired": [{"ia": str, "function": str, "wired": bool,
                          "reason": str, "event_node": guid, "call_node": guid}] }

    Example:
        scaffold_enhanced_input_pawn(
            "/Game/Pawns/BP_Hero",
            {
                "/Game/Input/IA_Move.IA_Move": ("Triggered", "HandleMove"),
                "/Game/Input/IA_Jump.IA_Jump": ("Started",   "HandleJump"),
            })
    """
    bp_lib    = unreal.UnrealBridgeBlueprintLibrary
    bp_ed     = unreal.BlueprintEditorLibrary
    asset_lib = unreal.EditorAssetLibrary

    # 1. Ensure BP exists.
    if not asset_lib.does_asset_exist(bp_path):
        slash = bp_path.rfind("/")
        folder, name = bp_path[:slash], bp_path[slash + 1:]
        factory = unreal.BlueprintFactory()
        factory.set_editor_property("parent_class", parent_class or unreal.Pawn)
        unreal.AssetToolsHelpers.get_asset_tools().create_asset(
            name, folder, unreal.Blueprint, factory)
        asset_lib.save_asset(bp_path)

    bp = unreal.load_asset(bp_path)
    if not bp:
        raise RuntimeError(f"failed to load Blueprint at {bp_path}")

    # 2. Pre-create any missing target function graphs, then compile so they're
    #    real on the GeneratedClass before we add CallFunction nodes for them.
    #    Bridge.get_blueprint_functions is the read API; FunctionGraphs UPROPERTY
    #    is protected and not directly Python-accessible.
    existing = {f.get_editor_property("name")
                for f in bp_lib.get_blueprint_functions(bp_path)}
    for _, (_, fn_name) in ia_action_map.items():
        if fn_name not in existing:
            bp_ed.add_function_graph(bp, fn_name)
            existing.add(fn_name)
    bp_ed.compile_blueprint(bp)

    # 3. Wire each IA → function.
    results = []
    y = 100
    for ia_path, (trigger_event, fn_name) in ia_action_map.items():
        r = bp_lib.wire_enhanced_input_action_to_function(
            bp_path, "EventGraph", ia_path, trigger_event,
            "", fn_name,
            300, y, 700, y)
        results.append({
            "ia": ia_path, "function": fn_name,
            "wired": r.wired, "reason": r.failure_reason,
            "event_node": r.event_node_guid, "call_node": r.call_node_guid,
        })
        y += 250

    bp_ed.compile_blueprint(bp)
    if save:
        asset_lib.save_asset(bp_path)

    return {"blueprint": bp_path, "imc": imc_path, "wired": results}


def get_world_info():
    """Get information about the current world/level.

    Returns:
        Dict with level name, actor count, etc.
    """
    world = unreal.EditorLevelLibrary.get_editor_world()
    subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = subsystem.get_all_level_actors()

    return {
        "level_name": world.get_name() if world else "None",
        "actor_count": len(actors),
        "map_name": unreal.EditorLevelLibrary.get_editor_world().get_path_name() if world else "None",
    }
