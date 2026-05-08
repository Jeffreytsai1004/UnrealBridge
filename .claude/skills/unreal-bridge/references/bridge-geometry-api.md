# bridge-geometry-api

`unreal.UnrealBridgeGeometryLibrary` — Lane 2 of the procedural-content roadmap. Wraps the runtime + editor Geometry Script libraries to give agents a `UDynamicMesh`-based authoring pipeline (load static mesh → operate → save). See `docs/plans/procedural-content-roadmap.md` §4 for the full design contract.

Hard contract:
- All ops require **UE 5.7+**. On 5.4-5.6 every UFUNCTION returns its zero value + logs a warning (compiled-in stub bodies; see `feedback_uht_no_if_around_reflection`).
- Asset-save ops (`save_mesh_to_new_static_mesh` / `save_mesh_to_existing_static_mesh`) are **editor-world only** — invoking from PIE / cmdlet returns "" / false.
- Mesh handles are **process-global integers** mapped to `UDynamicMesh*` via `TStrongObjectPtr` (GC-safe). Caller must `release_dynamic_mesh(handle)` when done; handles are never reused (monotonic counter), so stale handles fail loudly rather than aliasing fresh meshes.
- Big mesh ops (boolean / voxel-merge on dense meshes) block the GameThread. Per `feedback_bridge_exec_holds_gamethread`, split heavy chains across exec calls.
- Per `feedback_split_asset_ops`, **never** chain `create_dynamic_mesh` + `load_*` + `save_*` in a single bridge exec — `CreateNewStaticMeshAssetFromMesh` opens an asset-reference completing modal that deadlocks the bridge if other ops are queued behind it. Pattern:
  - exec1: `create → load → ops → get_info`
  - exec2: `save_to_new_static_mesh → release`

## What's shipped

**Phase 2 — M4 (this batch, 8 ops + 1 USTRUCT)**:
- handle pool: `create_dynamic_mesh`, `release_dynamic_mesh`, `list_dynamic_mesh_handles`
- ingest: `load_mesh_from_static_mesh`, `load_mesh_from_component`
- save: `save_mesh_to_new_static_mesh`, `save_mesh_to_existing_static_mesh`
- query: `get_mesh_info` → `FBridgeMeshInfo`

Phase 3 (boolean / smooth / decimate / recompute_normals) and the rest of M5 / M6 land in subsequent commits — see roadmap §4 M5 / M6.

---

## FBridgeMeshInfo

Static facts about a `UDynamicMesh`. Field names follow the standard UE Python projection (`bHasNormals` → `.has_normals`).

| Python field | C++ source | Notes |
|---|---|---|
| `num_triangles` | `UDynamicMesh::GetTriangleCount()` | Triangle count after compaction; not `NumTriangleIDs`. |
| `num_vertices` | `MeshQueryFunctions::GetVertexCount` | Vertex count after compaction. |
| `num_uv_layers` | `MeshQueryFunctions::GetNumUVSets` | 0 when the mesh has no UV attribute set. |
| `has_normals` | `MeshQueryFunctions::GetHasTriangleNormals` | The Normals attribute (split-normals capable). |
| `has_vertex_colors` | `MeshQueryFunctions::GetHasVertexColors` | |
| `bounds` | `MeshQueryFunctions::GetMeshBoundingBox` | Local-space `unreal.Box`. Empty when handle invalid. |

---

## create_dynamic_mesh() -> int

Allocate a fresh, empty `UDynamicMesh` and register it under a new int handle.

Returns: positive int handle (≥1); 0 indicates allocation failure.

**Cost** — O(1).

**Example**
```python
import unreal as u
G = u.UnrealBridgeGeometryLibrary
h = G.create_dynamic_mesh()
print('handle:', h)
```

**Pitfalls**
- Handles are never reused — every call increments a process-global counter. After 2³¹ calls (~2.1B) the int wraps; not realistic in practice.
- Mesh is empty until populated by `load_mesh_from_*` or (Phase 3+) primitive `append_*` ops.

---

## release_dynamic_mesh(handle) -> bool

Drop a handle from the registry. The underlying `UDynamicMesh` becomes eligible for GC. Idempotent.

Returns: true if the handle was registered (and is now released), false if it was unknown.

**Pitfalls**
- Releasing a stale handle returns false — that's a no-op, not an error. Use it before reusing handle slots in batch jobs.

---

## list_dynamic_mesh_handles() -> list[int]

All currently-registered handles. Sorted ascending. Useful for leak detection ("did I forget to release?") and end-of-session cleanup.

---

## load_mesh_from_static_mesh(handle, asset_path, lod=0) -> bool

Replace the handle's mesh with the geometry of a `UStaticMesh` asset. Wraps `CopyMeshFromStaticMeshV2` with default options (apply build settings, request tangents, use section materials).

| Param | Type | Notes |
|---|---|---|
| `handle` | int | Live handle. |
| `asset_path` | str | Content path, e.g. `/Engine/BasicShapes/Cube`. |
| `lod` | int (default 0) | Source-model LOD index. Silently clamped to available count. |

Returns: true on Geometry Script `Success` outcome.

**Pitfalls**
- Source-model LODs are **not** available at runtime — the call works in editor but in cooked builds you'd need `EGeometryScriptLODType::RenderData` (not exposed yet — file an issue if you need cooked-build support).
- Failure cases that return false: asset path unloadable, handle invalid, requested LOD genuinely missing.

**Example**
```python
ok = G.load_mesh_from_static_mesh(h, '/Engine/BasicShapes/Cube', 0)
assert ok
```

---

## load_mesh_from_component(actor_label, component_name, handle) -> bool

Pull mesh geometry from a primitive component on a level actor (editor world). Useful when the actor's component has runtime overrides — instance materials, dynamic mesh components, deformation modifiers — that don't exist in the underlying asset. Wraps `SceneUtilityFunctions::CopyMeshFromComponent` with normals + tangents + UVs + colors enabled.

| Param | Type | Notes |
|---|---|---|
| `actor_label` | str | User-visible label or FName (matches `UnrealBridgeLevelLibrary` resolver). |
| `component_name` | str | Component FName / GetName. Empty string falls back to root component. |
| `handle` | int | Target bridge handle. |

Returns: true on Success.

**Pitfalls**
- Editor world only — invoking from PIE returns false. For PIE-side mesh capture, use the runtime trace family from `UnrealBridgeLevelLibrary`.
- The Local-to-World transform is consumed internally (we always use the local-space mesh for further authoring); pass through `mesh_transform` (Phase 3+) if you need world-space placement.

---

## save_mesh_to_new_static_mesh(handle, new_asset_path, material_list) -> str

Save the handle's mesh as a brand-new `UStaticMesh` asset on disk via `CreateNewStaticMeshAssetFromMesh`.

| Param | Type | Notes |
|---|---|---|
| `handle` | int | Source handle. |
| `new_asset_path` | str | Content path, e.g. `/Game/_BridgeTest/SM_Out`. |
| `material_list` | `list[UMaterialInterface]` | Optional. Maps 1:1 to MaterialIDs in the dynamic mesh — slot N ← list[N]. Empty list → engine default material in every slot. |

Returns: the saved asset's package path on Success; empty string on Failure.

**Editor-world only** (`WITH_EDITOR` gate). Returns "" outside the editor.

**Pitfalls**
- Per `feedback_split_asset_ops` — **must** be the only asset-write op in its bridge exec. The function opens an asset-reference completing modal that deadlocks the bridge if other ops are queued.
- Default options are conservative: Nanite off, no normal/tangent recompute, collision on, default trace flag. Run `recompute_normals_and_tangents` (Phase 3+) before saving if your edits invalidated normals.
- The function creates the package; caller doesn't need to `editor_asset_lib.save_loaded_asset(...)` separately.

**Example (split exec form)**
```python
# exec1: build the mesh
import unreal as u; G = u.UnrealBridgeGeometryLibrary
h = G.create_dynamic_mesh()
G.load_mesh_from_static_mesh(h, '/Engine/BasicShapes/Cube', 0)
print(G.get_mesh_info(h))
```
```python
# exec2: save + release
import unreal as u; G = u.UnrealBridgeGeometryLibrary
hs = list(G.list_dynamic_mesh_handles()); h = hs[-1]
print(G.save_mesh_to_new_static_mesh(h, '/Game/_BridgeTest/SM_CubeCopy', []))
G.release_dynamic_mesh(h)
```

---

## save_mesh_to_existing_static_mesh(handle, existing_asset_path, b_replace_materials) -> bool

Push the handle's mesh INTO an existing `UStaticMesh` asset, replacing its source-model LOD0 geometry. Wraps `CopyMeshToStaticMesh`.

| Param | Type | Notes |
|---|---|---|
| `handle` | int | Source handle. |
| `existing_asset_path` | str | Content path to an EXISTING StaticMesh. |
| `b_replace_materials` | bool | True → dynamic mesh's MaterialIDs replace existing slot list. False → preserve slot mapping by section index (most common). |

Returns: true on Success. Calls `Modify()` + `MarkPackageDirty()` so undo + Save All pick up the change.

**Editor-world only**.

**Pitfalls**
- After return, the asset is dirty in memory but not yet saved to disk. Caller still needs `editor_asset_lib.save_loaded_asset(asset_path)` for persistence.
- If the existing asset's slot count is smaller than the dynamic mesh's MaterialIDs and `b_replace_materials=False`, extra material IDs map to the highest-index slot.

---

## get_mesh_info(handle) -> FBridgeMeshInfo

Static facts about the mesh held by `handle`. See FBridgeMeshInfo above.

Returns the all-zero/empty struct when handle invalid (use `list_dynamic_mesh_handles()` to verify before calling).

**Cost** — O(NumVertices + NumTriangles) for the queries that aren't cached. ~1ms per 100k tris.

---

## End-to-end smoke pattern (split exec, per pit #15)

```python
# exec1
import unreal as u; G = u.UnrealBridgeGeometryLibrary
h = G.create_dynamic_mesh()
G.load_mesh_from_static_mesh(h, '/Engine/BasicShapes/Cube', 0)
info = G.get_mesh_info(h)
print('cube:', info.num_triangles, 'tri /', info.num_vertices, 'vert')
print('handles:', list(G.list_dynamic_mesh_handles()))
```
```python
# exec2
import unreal as u; G = u.UnrealBridgeGeometryLibrary
hs = list(G.list_dynamic_mesh_handles()); h = hs[-1]
out_path = G.save_mesh_to_new_static_mesh(h, '/Game/_BridgeTest/SM_CubeCopy', [])
print('saved:', out_path)
print('released:', G.release_dynamic_mesh(h))
```
