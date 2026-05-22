# UnrealBridge Struct Library API

Module: `unreal.UnrealBridgeStructLibrary`

Create and edit `UUserDefinedStruct` (UDS) assets — the user-defined struct surface in `/Game/`, the rowtype for `UDataTable`, the variable type a Blueprint can hold. Wraps `FStructureEditorUtils` so every write goes through the same code path the native struct editor uses; transactional, recompiles dependents, fires `OnStructureChanged`.

## When to use

| Goal | This library? |
|---|---|
| Create a new UDS asset programmatically | Yes — `create_user_defined_struct(path)` |
| Add / remove / rename / retype / reorder fields | Yes — there's no native Python entry point (`unreal.UserDefinedStructureFactory` and `unreal.StructureEditorUtils` are not exposed; `unreal.UserDefinedStruct` is the class but you can't edit it from Python alone) |
| Read existing UDS field list | Yes — but `unreal.load_asset(path)` + struct reflection also works for read-only |
| Use a UDS as a `UDataTable` rowtype | Combine with `UnrealBridgeDataTableLibrary.create_data_table_from_json(path, struct_path, "[]")` |
| Reference a UDS in a Blueprint variable | Combine with `UnrealBridgeBlueprintLibrary.add_blueprint_variable(bp, name, "<UDS_Name>")` — the same `type_string` parser is shared |

## Type strings (shared with `add_blueprint_variable`)

The `type_string` argument accepted by `add_struct_variable` / `change_struct_variable_type` uses the same parser as `UnrealBridgeBlueprintLibrary.add_blueprint_variable`. Canonical forms (case-insensitive):

| Form | Maps to |
|---|---|
| `"Bool"` / `"Boolean"` | `PC_Boolean` |
| `"Byte"` | `PC_Byte` |
| `"Int"` / `"Int64"` | `PC_Int` / `PC_Int64` |
| `"Float"` / `"Double"` | `PC_Real` with float/double subcategory |
| `"String"` / `"Name"` / `"Text"` | `PC_String` / `PC_Name` / `PC_Text` |
| `"Vector"` / `"Rotator"` / `"Transform"` / `"LinearColor"` / `"GameplayTag"` | `PC_Struct` with the canonical engine struct |
| any `UScriptStruct` simple name (e.g. `"MyDataStruct"`, `"BridgeAssetInfo"`) | `PC_Struct` after lookup in `/Script/CoreUObject.` / `/Script/Engine.` |
| any `UClass` simple name (e.g. `"Actor"`, `"StaticMesh"`) | `PC_Object` reference |
| `"Array of <type>"` prefix | wraps the above as a `TArray` |

Not currently supported in `type_string`: Set / Map containers, enum types, soft-object refs. Extending the parser also extends `add_blueprint_variable`.

## FBridgeStructVariableInfo

Returned by `get_struct_variables`.

| Field | Type | Notes |
|---|---|---|
| `name` | str | Friendly (UI-displayed) name. Use this in subsequent ops. |
| `type_string` | str | Canonical form — round-trips through the same `type_string` parser. |
| `default_value` | str | Serialized FProperty value. Empty when never set. |
| `guid` | str | Internal stable GUID (read-only — exposed for power-user precision). |
| `tooltip` | str | |
| `b_edit_on_instance` | bool | False = `bDontEditOnInstance` is set (variable is greyed out on owning BPs). |

## FBridgeStructInfo

Returned by `get_struct_info`.

| Field | Type | Notes |
|---|---|---|
| `path` | str | Fully-qualified asset path. |
| `tooltip` | str | Struct-level tooltip. |
| `variable_count` | int | |
| `status` | str | `"UpToDate"` / `"NotCompiled"` / `"Error_Recursion"` / `"Error_FallbackStruct"` / `"Error_NotBlueprintType"` / `"Error_NotSupportedType"` / `"Error_EmptyStructure"` / `"Unknown"` |

## FBridgeStructCreateResult

Returned by `create_user_defined_struct`. Same shape as `FBridgeCreateAssetResult` (`b_success`, `path`, `error`).

---

## Asset lifecycle

### create_user_defined_struct(asset_path) -> FBridgeStructCreateResult

Create a new UDS at `asset_path` (e.g. `"/Game/Data/UDS_HitInfo"`). Internally goes through `IAssetTools::CreateAsset` with the engine's `StructureFactory`, so the new asset is dirtied + saved through standard paths and shows up in the Content Browser immediately.

**UE forces a placeholder field on every fresh UDS** (`MemberVar_0` with type `Bool`). Add real fields first, then `remove_struct_variable` the placeholder — UE rejects 0-field UDS.

```python
r = unreal.UnrealBridgeStructLibrary.create_user_defined_struct('/Game/Data/UDS_HitInfo')
assert r.b_success, r.error
print(r.path)  # → "/Game/Data/UDS_HitInfo.UDS_HitInfo"
```

PIE-guarded — refuses while `GEditor->PlayWorld != nullptr` because UE leaves UDS in a half-baked state when fields are edited during PIE.

---

## Field CRUD

All writes propagate through `FStructureEditorUtils::OnStructureChanged`, so any Blueprint / DataTable referencing this UDS is recompiled / refreshed automatically.

### add_struct_variable(struct_path, name, type_string, default_value="") -> bool

Add a new field. `type_string` per the table above. `default_value` is optional; if non-empty, written via `ChangeVariableDefaultValue` (uses `FProperty::ImportText_Direct`, so `(X=1,Y=2,Z=3)` for FVector, `True`/`False` for bool, etc.).

```python
unreal.UnrealBridgeStructLibrary.add_struct_variable(
    '/Game/Data/UDS_HitInfo', 'DamageAmount', 'Float', '12.5')
unreal.UnrealBridgeStructLibrary.add_struct_variable(
    '/Game/Data/UDS_HitInfo', 'HitLocation', 'Vector', '(X=0,Y=0,Z=0)')
unreal.UnrealBridgeStructLibrary.add_struct_variable(
    '/Game/Data/UDS_HitInfo', 'Tags', 'Array of GameplayTag', '')
```

### remove_struct_variable(struct_path, name) -> bool

Remove a field by friendly name. Returns False if the field isn't found OR if removing would leave 0 fields (UE forbids it).

### rename_struct_variable(struct_path, old_name, new_name) -> bool

Rename. GUID is preserved — references in DataTable rows / BP variables that hold this struct survive the rename.

### change_struct_variable_type(struct_path, name, new_type_string) -> bool

Change a field's type.

**UE clears the field's default value** as part of `ChangeVariableType` (because the old serialized value may not be valid for the new type). Call `set_struct_variable_default` again if you want a non-default value after the type change.

### set_struct_variable_default(struct_path, name, default_value) -> bool

Set the default value (FProperty export-text format). The struct is recompiled, so dependent BP variable defaults propagate immediately.

### move_struct_variable(struct_path, name, new_index) -> bool

Move a field to position `new_index` (0-based, 0 = top). `FStructureEditorUtils` only exposes single-step Up/Down primitives — this UFUNCTION wraps the loop. `new_index` is clamped to `[0, field_count - 1]`.

---

## Reads

### get_struct_variables(struct_path) -> list[FBridgeStructVariableInfo]

All fields in declaration order. Cheap (no asset compile).

```python
for v in unreal.UnrealBridgeStructLibrary.get_struct_variables('/Game/Data/UDS_HitInfo'):
    print(v.name, v.type_string, v.default_value)
```

### get_struct_info(struct_path) -> FBridgeStructInfo

Top-level metadata + compile status.

---

## Metadata

### set_struct_variable_tooltip(struct_path, name, tooltip) -> bool

Per-field tooltip (`FStructVariableDescription::ToolTip`).

### set_struct_variable_edit_on_instance(struct_path, name, edit_on_instance) -> bool

Toggle the per-field "Edit Anywhere" flag (`bDontEditOnInstance` inverted). When False, BPs holding this struct can't override the field on their CDO — it stays at the struct-level default.

### set_struct_tooltip(struct_path, tooltip) -> bool

Struct-level tooltip (`UUserDefinedStructEditorData::ToolTip`).

---

## Cross-library use

A UDS authored here is immediately usable as:

- **DataTable rowtype.** `UnrealBridgeDataTableLibrary.create_data_table_from_json(dt_path, uds_path, "[...]")`.
- **Blueprint variable.** `UnrealBridgeBlueprintLibrary.add_blueprint_variable(bp_path, name, "<UDS_Name>")` — pass the struct's simple name as the `type_string`.
- **Function parameter / return.** Same `type_string` mechanism on `add_blueprint_function_parameter`.

Recompile of dependent BPs after a struct edit is automatic; the bridge does not need to issue `recompile_blueprint` manually.

---

## Locks / gotchas

- **Placeholder field on create.** UE adds one when the UDS has zero fields. Add real fields first, then remove the placeholder.
- **change_type resets default.** Per UE behavior — re-write the default after a type change if needed.
- **No PIE writes.** Every write op short-circuits to `False` if `GEditor->PlayWorld` is non-null. Stop PIE first.
- **Case-insensitive name lookup.** `"Damage"` and `"damage"` resolve to the same field; create with the case you want.
- **Field GUIDs are stable across renames** but generated fresh by `add_struct_variable`. If you snapshot a GUID and then delete + re-add a field with the same name, the new GUID will differ — re-query.
- **No Set / Map / Enum / SoftObject types yet.** Extending `BridgeTypeParseImpl::ParseTypeString` lifts this limit for both struct and BP variable surfaces simultaneously.
