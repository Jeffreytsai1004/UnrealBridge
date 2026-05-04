# UnrealBridge GameplayTag Library API

`unreal.UnrealBridgeGameplayTagLibrary` — GameplayTag-specific helpers built on top of the AssetRegistry's SearchableName index and `UGameplayTagsManager`.

For generic SearchableName queries (PrimaryAssetId, GameplayCueTag, project-defined named-value structs), use the lower-level `UnrealBridgeAssetLibrary` functions instead — see `bridge-asset-api.md` "SearchableName Index". GameplayTag values flow through the same index; this library just adds tag-specific niceties:

- child-tag expansion (queries the tag tree via `UGameplayTagsManager`, then unions multi-tag results)
- access to *registered* tags including unused ones (SearchableName only sees referenced tags)
- source-of-definition lookup (which DataTable / .ini / native module declares a tag)

For runtime ASC tag queries (live actor tags, active GE tags), see `bridge-gameplayability-api.md`.

For tag hierarchy navigation (`find_child_tags` / `get_tag_parents`), also see `bridge-gameplayability-api.md` — those existed before this library.

---

## find_assets_referencing_tag(tag_string, b_include_children, package_path_filter, max_results) -> list[str]

Find every asset that references a GameplayTag — the programmatic equivalent of the editor's right-click → "Search for References" on a tag.

```python
gtl = unreal.UnrealBridgeGameplayTagLibrary

# Every asset referencing this exact tag
refs = gtl.find_assets_referencing_tag('Combat.Hit', False, '', 0)

# Include child tags too (Combat.Hit + Combat.Hit.Critical + Combat.Hit.Glance ...)
refs_recursive = gtl.find_assets_referencing_tag('Combat.Hit', True, '', 0)

# /Game-only, capped at 50
project_refs = gtl.find_assets_referencing_tag('Combat.Hit', True, '/Game', 50)
```

Parameters:

| name | meaning |
|---|---|
| `tag_string` | Full tag, e.g. `'Combat.Hit'`. |
| `b_include_children` | When `True`, walks the tag tree via `UGameplayTagsManager.RequestGameplayTagChildren` and unions results. When `False`, queries only the exact tag. |
| `package_path_filter` | Empty = no filter. Otherwise only return packages whose path starts with this — e.g. `'/Game'` to exclude engine + plugins. |
| `max_results` | `0` = unlimited. |

Returns sorted, de-duplicated package paths.

**Edge case**: when the tag has been deleted from the manager but stale assets still reference it, the query is best-effort — child expansion is skipped (the manager doesn't know the tree), but the literal-name query still finds the orphaned references.

---

## list_all_registered_tags(filter_prefix, max_results) -> list[str]

Every tag the `UGameplayTagsManager` knows about — including ones that have never been referenced by any asset (which the SearchableName index would miss). Backed by `RequestAllGameplayTags`.

```python
all_tags = gtl.list_all_registered_tags('', 0)
combat_branch = gtl.list_all_registered_tags('Combat.', 0)
```

**Difference from `list_searchable_name_values('GameplayTag', ...)`** (in `UnrealBridgeAssetLibrary`):

| function | source | what it returns |
|---|---|---|
| `list_all_registered_tags` | `UGameplayTagsManager` | all *registered* tags (config + native + DataTable + ini) |
| `list_searchable_name_values('GameplayTag', ...)` | AssetRegistry SearchableName | all tags *at least one asset has referenced* |

**Dead-tag detection**: set difference `registered − used` reveals tags that are defined but never referenced — candidates for cleanup. Combine the two queries client-side.

---

## get_tag_source_info(tag_string) -> BridgeTagSourceInfo

Where is this tag defined? Use this when planning to rename / delete a tag — answers "which .ini or DataTable do I edit?".

```python
info = gtl.get_tag_source_info('Foley.Event.Jump')
print(info.source_type)      # "DefaultTagList"
print(info.source_location)  # "C:/.../Config/DefaultGameplayTags.ini"
print(info.is_explicit)      # True
print(info.found)            # True
```

`BridgeTagSourceInfo` fields:

| field | type | meaning |
|---|---|---|
| `tag_string` | str | The tag the lookup was performed for (echoes input). |
| `found` | bool | False when the tag isn't in the manager (typo, plugin not loaded, deleted tag). All other fields are unreliable when False. |
| `source_type` | str | `"Native"`, `"DefaultTagList"`, `"TagList"`, `"RestrictedTagList"`, `"DataTable"`, `"NotFound"`, `"Unknown"`. |
| `source_location` | str | For ini sources: the actual config file path. For Native / DataTable: the source FName (usually module / asset name). |
| `is_explicit` | bool | True if the tag was explicitly added; False if it only exists as an implicit parent of a more specific child tag. |
| `is_restricted` | bool | True if this is a restricted gameplay tag. |
| `additional_sources` | list[str] | Rare: when the same tag is registered from multiple places (e.g. native + ini), the secondary source FNames are listed here. The primary one is in `source_type` / `source_location`. |

Source type semantics:

| source_type | meaning |
|---|---|
| `Native` | Defined via `UE_DEFINE_GAMEPLAY_TAG` / `FNativeGameplayTag` C++ macros |
| `DefaultTagList` | `Config/DefaultGameplayTags.ini` (Project Settings → GameplayTags) |
| `TagList` | Other `.ini` file under `Config/Tags/` |
| `RestrictedTagList` | Restricted-tag `.ini` |
| `DataTable` | Declared in a UDataTable asset |
| `NotFound` | Tag isn't in the manager at all |
| `Unknown` | Found in manager but couldn't resolve source (rare) |

---

---

## Tag source enumeration

### list_tag_source_inis(filter_type) -> list[BridgeTagSourceListing]

Every place this project's tags can come from — ini files, native code modules, DataTables. Pass the returned `source_name` as the `source_ini` argument to `add_gameplay_tag(...)` to control where a new tag lands.

```python
sources = gtl.list_tag_source_inis('')           # all sources
inis    = gtl.list_tag_source_inis('DefaultTagList')  # just the project's primary ini
for s in sources:
    print(s.source_name, s.source_type, 'writable' if s.is_writable else '(read-only)')
```

`filter_type` accepts: `''`, `'Native'`, `'DefaultTagList'`, `'TagList'`, `'RestrictedTagList'`, `'DataTable'`.

`BridgeTagSourceListing` fields:

| field | meaning |
|---|---|
| `source_name` | FName-as-string. Pass to `add_gameplay_tag(source_ini=...)`. |
| `source_type` | One of the EGameplayTagSourceType strings above. |
| `config_file_path` | Resolved on-disk path for ini-backed sources; empty for Native/DataTable. |
| `is_writable` | True when the bridge's mutation APIs can target this source (DefaultTagList / TagList / RestrictedTagList only). Native = C++ source edit required; DataTable = use the DataTable library. |

---

## Mutations

These three write to disk via `IGameplayTagsEditorModule`. Native-source tags can't be written from Python — they live in C++ source. Restricted tags use a separate flow not yet exposed.

### add_gameplay_tag(new_tag, source_ini='', comment='', is_restricted=False) -> bool

Add a new GameplayTag. `source_ini=''` writes to the project's default (`Config/DefaultGameplayTags.ini`); pass a `source_name` from `list_tag_source_inis(...)` to target a specific ini.

```python
ok = gtl.add_gameplay_tag('Combat.Stun', '', 'Stuns the target for N seconds')
# Or target a specific source:
ok = gtl.add_gameplay_tag('UI.Modal.Inventory', 'MyMod.ini', '')
```

Returns False if the tag already exists, the source isn't found, or the editor module rejected the write.

### rename_gameplay_tag(old_tag, new_tag, rename_children=True) -> bool

Rename a tag in-place. UE auto-inserts a `+GameplayTagRedirects=(OldTagName=...,NewTagName=...)` line so existing asset references continue to resolve to the new name — **don't** also write the redirect manually.

```python
ok = gtl.rename_gameplay_tag('Combat.Hit', 'Combat.HitImpact', rename_children=True)
# rename_children=True also renames Combat.Hit.Critical → Combat.HitImpact.Critical
```

This wrapper validates `old_tag` is registered before delegating to UE — without that guard, `IGameplayTagsEditorModule::RenameTagInINI` would happily write a redirect for a non-existent tag, leaving useless `+GameplayTagRedirects=` litter in the ini. Returns False if `old_tag` doesn't exist.

### remove_gameplay_tag(tag) -> bool

Delete a tag from its source ini.

```python
# ALWAYS check references first — UE doesn't insert a redirect on delete,
# so dangling references will surface as warnings on next load.
refs = gtl.find_assets_referencing_tag('Combat.OldTag', True, '/Game', 0)
if not refs:
    gtl.remove_gameplay_tag('Combat.OldTag')
```

Returns False if the tag doesn't exist or is native-defined.

**Caveats common to all three mutations:**
- Run from the editor (not cooked builds — these APIs need `IGameplayTagsEditorModule`).
- Changes write to disk **immediately** but the manager's in-memory tree refresh is what makes subsequent calls see the new state — which happens automatically.
- The editor periodically saves its in-memory tag state back to source inis. Direct manual edits to a tag ini while the editor is running risk being overwritten on shutdown — prefer these APIs over hand-editing the ini in a running session.
- After `rename_*` followed by `remove_*` of the new name, the redirect line for `old → new` is also cleaned up by UE. After a manual delete of the source-ini line in a stopped editor, dangling redirects can accumulate — clean them by hand.

---

## Common workflows

**"What references this tag I'm about to rename?"**

```python
refs = gtl.find_assets_referencing_tag('Old.Tag.Name', True, '/Game', 0)
for r in refs: print(r)
src = gtl.get_tag_source_info('Old.Tag.Name')
print(f'Defined in: {src.source_location}')
```

**"Which tags are defined but never used?" (dead tags)**

```python
asl = unreal.UnrealBridgeAssetLibrary
registered = set(gtl.list_all_registered_tags('', 0))
used       = set(asl.list_searchable_name_values('GameplayTag', '', 0))
dead = sorted(registered - used)
print(f'{len(dead)} dead tags:'); [print(f'  {t}') for t in dead]
```

**"Which assets in /Game reference any tag under Combat.*?"**

```python
refs = gtl.find_assets_referencing_tag('Combat', True, '/Game', 0)
```

**"What tags does this Blueprint actually use?"**

```python
asl = unreal.UnrealBridgeAssetLibrary
tags = asl.get_searchable_names_used_by_asset('/Game/BP/MyAbility', 'GameplayTag', 0)
for t in tags: print(t.value_name)
```
