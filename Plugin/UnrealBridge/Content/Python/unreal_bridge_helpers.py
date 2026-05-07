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


# ─── G1 / G2 — Bulk Input bindings JSON import/export ───────────────

def export_input_bindings_to_json(content_path_filter="/Game", include_imcs=True,
                                   include_ias=True):
    """Serialize the project's IA + IMC stack into a single JSON-friendly dict.

    Round-trips with `import_input_bindings_from_json`. Useful for diffing
    between branches / templating common input setups / cross-project copy.

    Returns:
        {
            "input_actions": [{path, value_type, description, consume_input,
                               trigger_when_paused, triggers:[{class,params}],
                               modifiers:[{class,params}]}, ...],
            "mapping_contexts": [{path, description, mappings:[
                {action_path, key, triggers:[{class,params}], modifiers:[{class,params}]
                }, ...]}, ...]
        }
    """
    gp = unreal.UnrealBridgeGameplayLibrary
    asset_lib = unreal.EditorAssetLibrary
    out = {"input_actions": [], "mapping_contexts": []}

    if include_ias:
        for path in gp.list_input_actions(content_path_filter, 0):
            ia = unreal.load_asset(path)
            if not ia:
                continue
            entry = {
                "path": path,
                "value_type": ia.value_type.name.lower().capitalize().replace("Axis1d", "Axis1D")
                              .replace("Axis2d", "Axis2D").replace("Axis3d", "Axis3D"),
                "description": str(ia.action_description),
                "consume_input": bool(ia.consume_input),
                "trigger_when_paused": bool(ia.trigger_when_paused),
                "triggers":  [{"class": t.class_name, "params": t.params_json}
                              for t in gp.get_input_action_triggers_full(path)],
                "modifiers": [{"class": m.class_name, "params": m.params_json}
                              for m in gp.get_input_action_modifiers_full(path)],
            }
            out["input_actions"].append(entry)

    if include_imcs:
        for path in gp.list_input_mapping_contexts(content_path_filter, 0):
            imc = unreal.load_asset(path)
            if not imc:
                continue
            entry = {
                "path": path,
                "description": str(imc.context_description),
                "mappings": [],
            }
            for m in gp.get_input_mapping_context_mappings(path):
                entry["mappings"].append({
                    "action_path": m.action_path,
                    "key": m.key_name,
                    # The read API only gives class names per-mapping, not
                    # individual JSON params. JSON params on per-mapping
                    # triggers/modifiers can't be round-tripped today; this
                    # is a known limitation flagged in the roadmap doc.
                    "triggers":  [{"class": c} for c in list(m.trigger_classes)],
                    "modifiers": [{"class": c} for c in list(m.modifier_classes)],
                })
            out["mapping_contexts"].append(entry)
    return out


def import_input_bindings_from_json(spec, default_save=True, overwrite=False):
    """Bulk create/update IA + IMC + mappings from a dict in the format
    returned by `export_input_bindings_to_json`.

    Behavior per asset:
      - If the asset doesn't exist → created.
      - If it exists and `overwrite=False` (default) → IA properties are
        updated in place; existing triggers/modifiers are LEFT (additive).
      - If it exists and `overwrite=True` → all triggers/modifiers cleared
        first, then re-applied from the spec.

    Returns:
        {"created": [...], "updated": [...], "skipped": [...], "errors": [...]}
    """
    gp = unreal.UnrealBridgeGameplayLibrary
    asset_lib = unreal.EditorAssetLibrary
    log = {"created": [], "updated": [], "skipped": [], "errors": []}

    # Pass 1 — IAs (so IMC mappings can reference them)
    for ia_spec in spec.get("input_actions", []):
        path = ia_spec["path"]
        value_type = ia_spec.get("value_type", "Boolean")
        desc = ia_spec.get("description", "")
        triggers = ia_spec.get("triggers", [])
        modifiers = ia_spec.get("modifiers", [])
        consume = ia_spec.get("consume_input")
        paused  = ia_spec.get("trigger_when_paused")

        existed = asset_lib.does_asset_exist(path)
        if not existed:
            created = gp.create_input_action(path, value_type, desc, default_save)
            if not created:
                log["errors"].append({"path": path, "reason": "create failed"})
                continue
            log["created"].append(path)
        else:
            log["updated"].append(path)
            if overwrite:
                # Clear triggers/modifiers
                ia = unreal.load_asset(path)
                while ia.triggers: gp.remove_trigger_from_ia(path, 0, False)
                while ia.modifiers: gp.remove_modifier_from_ia(path, 0, False)
            # Update value type + description even if existed
            gp.set_input_action_property(path, "ValueType", value_type, False)
            if desc:
                gp.set_input_action_property(path, "ActionDescription", f'"{desc}"', False)

        if consume is not None:
            gp.set_input_action_property(path, "bConsumeInput", "true" if consume else "false", False)
        if paused is not None:
            gp.set_input_action_property(path, "bTriggerWhenPaused", "true" if paused else "false", False)

        for t in triggers:
            gp.add_trigger_to_ia(path, t["class"], t.get("params", "{}"), False)
        for m in modifiers:
            gp.add_modifier_to_ia(path, m["class"], m.get("params", "{}"), False)

        if default_save:
            asset_lib.save_asset(path)

    # Pass 2 — IMCs + mappings
    for imc_spec in spec.get("mapping_contexts", []):
        path = imc_spec["path"]
        desc = imc_spec.get("description", "")

        if not asset_lib.does_asset_exist(path):
            out = gp.create_input_mapping_context(path, desc, default_save)
            if not out:
                log["errors"].append({"path": path, "reason": "create failed"})
                continue
            log["created"].append(path)
        else:
            log["updated"].append(path)

        for m in imc_spec.get("mappings", []):
            ia = m["action_path"]
            key = m["key"]
            gp.add_ia_mapping_to_imc(path, ia, key, False)
            for t in m.get("triggers", []):
                gp.add_trigger_to_imc_mapping(path, ia, key, t["class"], t.get("params", "{}"), False)
            for mod in m.get("modifiers", []):
                gp.add_modifier_to_imc_mapping(path, ia, key, mod["class"], mod.get("params", "{}"), False)

        if default_save:
            asset_lib.save_asset(path)

    return log


# ─── D5 — rule-based trigger conflict detection ─────────────────────

def detect_trigger_conflicts(input_action_path):
    """Heuristic lint for an IA's trigger stack. Returns a list of warnings.

    Rules:
      - Hold + Tap on same IA: timed-hold and tap can both fire, but typically
        you want only one. Flag.
      - Multiple Pressed/Released on same IA: rare and usually wrong.
      - Pulse + Hold: Pulse will retrigger inside the Hold window, often
        unintended. Flag.
      - Down + any other: Down fires every tick the key is held, which
        usually conflicts with timed/edge triggers.

    Args:
        input_action_path: e.g. "/Game/Input/IA_Foo.IA_Foo"
    Returns:
        list[dict] — each dict has {rule, classes, message}
    """
    triggers = unreal.UnrealBridgeGameplayLibrary.get_input_action_triggers(input_action_path)
    classes  = list(triggers[0]) if triggers else []  # First array is class-name list
    issues = []

    def has(name):
        return any(name in c for c in classes)

    if has("Hold") and has("Tap"):
        issues.append({"rule": "hold_and_tap", "classes": ["Hold","Tap"],
                       "message": "Hold + Tap on same IA: both can fire on the same key event"})
    if sum(1 for c in classes if "Pressed" in c) > 1:
        issues.append({"rule": "multiple_pressed", "classes": [c for c in classes if "Pressed" in c],
                       "message": "Multiple Pressed triggers — typically wrong"})
    if sum(1 for c in classes if "Released" in c) > 1:
        issues.append({"rule": "multiple_released", "classes": [c for c in classes if "Released" in c],
                       "message": "Multiple Released triggers — typically wrong"})
    if has("Pulse") and has("Hold"):
        issues.append({"rule": "pulse_and_hold", "classes": ["Pulse","Hold"],
                       "message": "Pulse will retrigger during Hold window — often unintended"})
    if has("Down") and len(classes) > 1:
        issues.append({"rule": "down_with_others", "classes": classes,
                       "message": "Down fires every tick the key is held; conflicts with timed/edge triggers"})

    return issues


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
