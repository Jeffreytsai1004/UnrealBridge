#!/usr/bin/env python3
"""Generate a Markdown changelog by diffing two snapshots of bridge_manifest.json.

The manifest at `.claude/skills/unreal-bridge/scripts/bridge_manifest.json` is
the source of truth for "what UFUNCTIONs / enums / structs the bridge exposes"
— it's regenerated via `tools/gen_manifest.py` and committed alongside feature
PRs. This tool reads two git refs of that file, computes the diff, and emits
markdown grouped by library.

Usage:
    python tools/gen_changelog.py                 # diff origin/main..HEAD
    python tools/gen_changelog.py --from v1.0.0   # since a tag
    python tools/gen_changelog.py --from HEAD~10  # last 10 commits
    python tools/gen_changelog.py --to v1.1.0     # up to a tag (default HEAD)
    python tools/gen_changelog.py --output CHANGELOG.md
    python tools/gen_changelog.py --json          # machine-readable diff

Exit codes: 0 = changelog produced (even if empty), 1 = git/parse error.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path
from typing import Optional

REPO_ROOT = Path(__file__).resolve().parent.parent
MANIFEST_PATH = ".claude/skills/unreal-bridge/scripts/bridge_manifest.json"

# Windows consoles in non-en locales (e.g. cp936) can't encode emojis used in
# section headers — reconfigure stdout to UTF-8 with replace fallback.
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")


def git_show(ref: str, path: str) -> Optional[dict]:
    """Read a JSON file at a given git ref. Returns None if the file didn't
    exist at that ref (e.g. ref pre-dates the file's introduction)."""
    try:
        proc = subprocess.run(
            ["git", "show", f"{ref}:{path}"],
            capture_output=True, text=True, encoding="utf-8",
            cwd=REPO_ROOT,
        )
    except FileNotFoundError:
        sys.exit("error: git not found on PATH")
    if proc.returncode != 0:
        # `git show` returns non-zero when path doesn't exist at ref;
        # treat as "no manifest yet" rather than fatal.
        return None
    return json.loads(proc.stdout)


def fn_signature(fn: dict) -> str:
    """Render a function's params/returns as a comparable signature string."""
    parts = []
    for p in fn.get("params", []):
        s = f"{p.get('name', '?')}: {p.get('type', '?')}"
        if p.get("has_default"):
            s += f" = {p.get('default')!r}"
        parts.append(s)
    return f"({', '.join(parts)}) -> {fn.get('returns', '?')}"


def diff_libraries(old: dict, new: dict) -> dict:
    """Compute the diff structure between two manifests."""
    old_libs = old.get("libraries", {}) if old else {}
    new_libs = new.get("libraries", {}) if new else {}

    diff = {
        "added_libraries": [],
        "removed_libraries": [],
        "library_changes": {},  # libname -> {added: [...], removed: [...], signature_changed: [...]}
    }

    for lib_name in sorted(set(old_libs) | set(new_libs)):
        old_lib = old_libs.get(lib_name)
        new_lib = new_libs.get(lib_name)

        if old_lib is None and new_lib is not None:
            fns = sorted(new_lib.get("functions", {}).keys())
            diff["added_libraries"].append({"name": lib_name, "functions": fns})
            continue
        if new_lib is None and old_lib is not None:
            fns = sorted(old_lib.get("functions", {}).keys())
            diff["removed_libraries"].append({"name": lib_name, "functions": fns})
            continue

        old_fns = old_lib.get("functions", {})
        new_fns = new_lib.get("functions", {})
        added, removed, sig_changed = [], [], []

        for fn_name in sorted(set(old_fns) | set(new_fns)):
            old_fn = old_fns.get(fn_name)
            new_fn = new_fns.get(fn_name)
            if old_fn is None:
                added.append({"name": fn_name, "signature": fn_signature(new_fn)})
            elif new_fn is None:
                removed.append({"name": fn_name, "signature": fn_signature(old_fn)})
            else:
                old_sig = fn_signature(old_fn)
                new_sig = fn_signature(new_fn)
                if old_sig != new_sig:
                    sig_changed.append({
                        "name": fn_name,
                        "old": old_sig,
                        "new": new_sig,
                    })

        if added or removed or sig_changed:
            diff["library_changes"][lib_name] = {
                "added": added,
                "removed": removed,
                "signature_changed": sig_changed,
            }

    # Enums & structs (lightweight — added/removed only, not full field diff)
    diff["added_enums"]    = sorted(set(new.get("enums", {})) - set(old.get("enums", {})))    if old and new else []
    diff["removed_enums"]  = sorted(set(old.get("enums", {})) - set(new.get("enums", {})))    if old and new else []
    diff["added_structs"]  = sorted(set(new.get("structs", {})) - set(old.get("structs", {}))) if old and new else []
    diff["removed_structs"] = sorted(set(old.get("structs", {})) - set(new.get("structs", {}))) if old and new else []

    return diff


def render_markdown(diff: dict, from_ref: str, to_ref: str,
                    old_meta: dict, new_meta: dict) -> str:
    out = []
    out.append(f"# Bridge API Changelog")
    out.append("")
    out.append(f"`{from_ref}` → `{to_ref}`")
    out.append("")
    if old_meta:
        out.append(f"- **From**: {old_meta.get('generated_at', '?')} (UE {old_meta.get('ue_version', '?')})")
    if new_meta:
        out.append(f"- **To**:   {new_meta.get('generated_at', '?')} (UE {new_meta.get('ue_version', '?')})")
    out.append("")

    has_content = False

    if diff["added_libraries"]:
        has_content = True
        out.append("## ✨ New libraries")
        out.append("")
        for lib in diff["added_libraries"]:
            out.append(f"### `{lib['name']}`")
            out.append("")
            for fn in lib["functions"]:
                out.append(f"- `{fn}`")
            out.append("")

    if diff["removed_libraries"]:
        has_content = True
        out.append("## 🗑️ Removed libraries")
        out.append("")
        for lib in diff["removed_libraries"]:
            out.append(f"- **`{lib['name']}`** ({len(lib['functions'])} functions)")
        out.append("")

    if diff["library_changes"]:
        has_content = True
        out.append("## Library changes")
        out.append("")
        for lib_name, changes in sorted(diff["library_changes"].items()):
            n_add = len(changes["added"])
            n_rm = len(changes["removed"])
            n_chg = len(changes["signature_changed"])
            summary = []
            if n_add: summary.append(f"+{n_add}")
            if n_rm:  summary.append(f"-{n_rm}")
            if n_chg: summary.append(f"~{n_chg}")
            out.append(f"### `{lib_name}` ({', '.join(summary)})")
            out.append("")
            if changes["added"]:
                out.append("**Added**")
                out.append("")
                for fn in changes["added"]:
                    out.append(f"- `{fn['name']}{fn['signature']}`")
                out.append("")
            if changes["removed"]:
                out.append("**Removed**")
                out.append("")
                for fn in changes["removed"]:
                    out.append(f"- `{fn['name']}{fn['signature']}`")
                out.append("")
            if changes["signature_changed"]:
                out.append("**Signature changed**")
                out.append("")
                for fn in changes["signature_changed"]:
                    out.append(f"- `{fn['name']}`")
                    out.append(f"  - old: `{fn['old']}`")
                    out.append(f"  - new: `{fn['new']}`")
                out.append("")

    if diff["added_enums"] or diff["removed_enums"]:
        has_content = True
        out.append("## Enums")
        out.append("")
        if diff["added_enums"]:
            out.append("**Added**: " + ", ".join(f"`{e}`" for e in diff["added_enums"]))
            out.append("")
        if diff["removed_enums"]:
            out.append("**Removed**: " + ", ".join(f"`{e}`" for e in diff["removed_enums"]))
            out.append("")

    if diff["added_structs"] or diff["removed_structs"]:
        has_content = True
        out.append("## Structs")
        out.append("")
        if diff["added_structs"]:
            out.append("**Added**: " + ", ".join(f"`{s}`" for s in diff["added_structs"]))
            out.append("")
        if diff["removed_structs"]:
            out.append("**Removed**: " + ", ".join(f"`{s}`" for s in diff["removed_structs"]))
            out.append("")

    if not has_content:
        out.append("*No bridge API changes between these refs.*")
        out.append("")

    return "\n".join(out)


def main() -> int:
    ap = argparse.ArgumentParser(description="Diff bridge_manifest.json across git refs.")
    ap.add_argument("--from", dest="from_ref", default="origin/main",
                    help="Old git ref (default: origin/main)")
    ap.add_argument("--to", dest="to_ref", default="HEAD",
                    help="New git ref (default: HEAD)")
    ap.add_argument("--output", help="Write markdown to this file (default: stdout)")
    ap.add_argument("--json", action="store_true",
                    help="Emit machine-readable diff instead of markdown")
    args = ap.parse_args()

    old = git_show(args.from_ref, MANIFEST_PATH)
    new = git_show(args.to_ref, MANIFEST_PATH)

    if old is None and new is None:
        sys.exit(f"error: manifest not present at either {args.from_ref} or {args.to_ref}")

    diff = diff_libraries(old or {}, new or {})

    if args.json:
        text = json.dumps(diff, indent=2, ensure_ascii=False)
    else:
        text = render_markdown(diff, args.from_ref, args.to_ref, old or {}, new or {})

    if args.output:
        Path(args.output).write_text(text, encoding="utf-8")
        print(f"wrote {args.output}", file=sys.stderr)
    else:
        print(text)

    return 0


if __name__ == "__main__":
    sys.exit(main())
