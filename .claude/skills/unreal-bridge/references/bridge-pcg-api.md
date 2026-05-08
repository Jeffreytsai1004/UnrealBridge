# bridge-pcg-api

`unreal.UnrealBridgePCGLibrary` — Lane 3 of the procedural-content roadmap.
**Read-only graph access + override edit + Generate/Cleanup trigger** for
existing PCG content. **No graph editing** — bridge does not duplicate the
PCG visual editor (roadmap §5/§8).

Hard contract:
- **UE 5.7+ only.** Older engines compile to logged-warning stubs.
- **Editor-world only.** PIE/runtime PCG is the PCG plugin's own concern.
- `wait_for_pcg_generate` polls `IsGenerating()` on the GameThread with
  50ms sleep — caller must understand it blocks the bridge for the
  duration. Same pattern as `hot_reload.py`.
- `set_pcg_component_override` uses `Property->ImportText` — value strings
  must be in UE's standard exported form for the property's type.

## What's shipped (8 ops)

- `list_pcg_graph_assets(filter, max=200) -> list[str]` (L3-1)
- `list_pcg_components_in_level(level_filter, max=200) -> list[FBridgePCGComponentEntry]` (L3-2)
- `get_pcg_component_state(actor_label, component_name) -> FBridgePCGComponentState` (L3-3)
- `get_pcg_component_overrides(actor_label, component_name) -> list[FBridgePCGOverrideEntry]` (L3-4)
- `set_pcg_component_override(actor_label, component_name, name, exported_value) -> bool` (L3-5)
- `trigger_pcg_generate(actor_label, component_name, b_force=False) -> bool` (L3-6)
- `wait_for_pcg_generate(actor_label, component_name, timeout_sec=60.0) -> FBridgePCGWaitResult` (L3-7)
- `cleanup_pcg_component(actor_label, component_name, b_remove_components=False) -> bool` (L3-8)

## USTRUCTs

### FBridgePCGComponentEntry
| Field | Type | Source |
|---|---|---|
| `actor_label` | str | `AActor::GetActorLabel()` |
| `component_name` | str | `UPCGComponent::GetName()` |
| `graph_path` | str | `GetGraph()->GetPathName()` |
| `b_generated` | bool | `bGenerated` |
| `b_generating` | bool | `IsGenerating()` |

### FBridgePCGComponentState
| Field | Type | Source |
|---|---|---|
| `graph_path` | str | as above |
| `b_generated` | bool | as above |
| `b_dirty` | bool | (always False — `bDirtyGenerated` is private; expose if PCG ships a getter) |
| `b_generating` | bool | as above |
| `generated_bounds` | Box | `GetLastGeneratedBounds()` |
| `last_generation_iso` | str | wall-clock at the moment of query (ISO-8601) — a **call timestamp**, not the actual last gen time (PCG doesn't expose that yet) |

### FBridgePCGOverrideEntry
| Field | Type | Source |
|---|---|---|
| `name` | str | property bag entry name |
| `type_str` | str | `Property->GetCPPType()` |
| `value_str` | str | `Property->ExportText_Direct(...)` |

### FBridgePCGWaitResult
| Field | Type | Notes |
|---|---|---|
| `b_success` | bool | True iff `IsGenerating()` reported false before `timeout_sec` |
| `elapsed_ms` | float | total wall-time spent polling |
| `note` | str | `"generated"` / `"not generated (no work to do?)"` / `"timeout"` / `"component not found"` |

---

## End-to-end pattern

```python
import unreal as u
P = u.UnrealBridgePCGLibrary

# Discover PCG content
print(P.list_pcg_graph_assets(filter='', max=20))
comps = P.list_pcg_components_in_level(level_filter='', max=20)
for c in comps:
    print(c.actor_label, c.component_name, c.graph_path, c.b_generated)

# Inspect overrides on the first component
if comps:
    e = comps[0]
    overrides = P.get_pcg_component_overrides(e.actor_label, e.component_name)
    for o in overrides:
        print(o.name, '=', o.value_str, '(', o.type_str, ')')

    # Set one override + regenerate
    P.set_pcg_component_override(e.actor_label, e.component_name, 'Density', '0.5')
    P.trigger_pcg_generate(e.actor_label, e.component_name, b_force=True)
    result = P.wait_for_pcg_generate(e.actor_label, e.component_name, timeout_sec=60.0)
    print(result.b_success, result.elapsed_ms, result.note)

    # Tear down generated content
    P.cleanup_pcg_component(e.actor_label, e.component_name, b_remove_components=True)
```

---

## Pitfalls

- **Property-bag values are strict.** `set_pcg_component_override` wraps
  `Property->ImportText`. For `int` pass `"42"`; for `FVector` pass
  `"(X=1.0, Y=2.0, Z=3.0)"`; for `FString` pass `"\"hello\""`. Wrong
  format → ImportText silently consumes nothing and returns false.
- **`b_dirty` is always false in the current build.** `bDirtyGenerated`
  is a private field on `UPCGComponent` with no public getter; we'd need
  a friend class or a PR upstream. Not blocking for the read+trigger
  use case.
- **Big PCG generations block the bridge for as long as
  `wait_for_pcg_generate` is held.** Fire-and-forget is `trigger` only;
  use a poll loop in Python if you don't want to block.
- **`cleanup_pcg_component(b_remove_components=True)`** purges any
  component the PCG generation registered as managed — this can include
  things outside the immediate output (foliage components, ISMs spawned
  by sub-graphs). Use False unless you know you want a hard reset.
