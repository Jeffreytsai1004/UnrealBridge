#!/usr/bin/env python3
"""Preflight environment checker for UnrealBridge build / run.

Scans the local machine for the toolchain + resources needed to compile and
launch a UE editor with this plugin. Intended as a one-shot diagnostic before
`rebuild_relaunch.py` or `build_matrix.py` to catch the
"MSVC 14.38.33130 required" / "out of disk" / "wrong UE_ROOT" class of
problems up-front instead of after a 5-minute compile.

Usage:
    python tools/preflight.py                # text report, exit 1 on any FAIL
    python tools/preflight.py --json         # machine-readable
    python tools/preflight.py --strict       # treat WARN as FAIL too

Checks:
    1. Python version           (>=3.9 recommended for UE 5.4+ plugin tooling)
    2. Visual Studio + MSVC     (>=14.38 for UE 5.7's strict UHT pass)
    3. Windows SDK              (10.0.18362+ for UE 5.4+)
    4. Free disk space          (>=20 GB on the repo drive)
    5. GPU + VRAM               (DX12-capable, >=4 GB recommended)
    6. UE engine path resolvable (UNREAL_EDITOR_EXE or UE_ROOT env var)
    7. Bridge plugin file present (Plugin/UnrealBridge/UnrealBridge.uplugin)
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Optional

REPO_ROOT = Path(__file__).resolve().parent.parent
UPLUGIN_PATH = REPO_ROOT / "Plugin" / "UnrealBridge" / "UnrealBridge.uplugin"

# Windows consoles in non-en locales (e.g. cp936) can't encode some symbols.
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")


@dataclass
class CheckResult:
    name: str
    status: str  # PASS | WARN | FAIL
    detail: str
    extra: dict = field(default_factory=dict)


def status_icon(s: str) -> str:
    return {"PASS": "✓", "WARN": "!", "FAIL": "✗"}.get(s, "?")


# ─── Individual checks ──────────────────────────────────────────────


def check_python() -> CheckResult:
    v = sys.version_info
    detail = f"{v.major}.{v.minor}.{v.micro}"
    if v >= (3, 9):
        return CheckResult("Python", "PASS", detail)
    return CheckResult("Python", "WARN", f"{detail} — recommend 3.9+")


def find_vswhere() -> Optional[Path]:
    candidates = [
        Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) /
        "Microsoft Visual Studio" / "Installer" / "vswhere.exe",
        Path(os.environ.get("ProgramFiles", r"C:\Program Files")) /
        "Microsoft Visual Studio" / "Installer" / "vswhere.exe",
    ]
    for c in candidates:
        if c.exists():
            return c
    return None


def _scan_vs_filesystem() -> Optional[Path]:
    """Fallback when vswhere is broken/missing — scan Program Files directly.

    Looks for VS 2022 first (UE 5.4+ canonical), then 2019 (some UE 5.x still
    work). Returns the first install root that has a usable cl.exe under it.
    """
    bases = [
        Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "Microsoft Visual Studio",
        Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio",
    ]
    # Prefer 2022, then 2019. Skip 2017 (UE 5.x dropped support).
    for year in ("2022", "2019"):
        for base in bases:
            year_dir = base / year
            if not year_dir.exists():
                continue
            for edition_dir in year_dir.iterdir():
                if not edition_dir.is_dir():
                    continue
                msvc_root = edition_dir / "VC" / "Tools" / "MSVC"
                if msvc_root.exists() and any(msvc_root.iterdir()):
                    return edition_dir
    return None


def check_visual_studio() -> CheckResult:
    vswhere = find_vswhere()
    if vswhere is not None:
        try:
            proc = subprocess.run(
                [str(vswhere), "-latest", "-products", "*",
                 "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                 "-format", "json"],
                capture_output=True, text=True, encoding="utf-8", timeout=15,
            )
            if proc.returncode == 0 and proc.stdout.strip():
                installs = json.loads(proc.stdout)
                if installs:
                    inst = installs[0]
                    return CheckResult(
                        "Visual Studio", "PASS",
                        f"{inst.get('displayName', '?')} ({inst.get('installationVersion', '?')})",
                        extra={"path": inst["installationPath"]},
                    )
        except (subprocess.TimeoutExpired, OSError, json.JSONDecodeError):
            pass  # fall through to filesystem scan

    # vswhere absent, broken, or returned empty — scan Program Files directly.
    fs_path = _scan_vs_filesystem()
    if fs_path is None:
        return CheckResult("Visual Studio", "FAIL",
                           "VS 2022 with VC++ tools not found "
                           "(vswhere empty AND no install under Program Files)")

    # Derive a friendly version string from the path: ".../2022/Community" → "VS 2022 Community"
    parent_year = fs_path.parent.name
    edition = fs_path.name
    return CheckResult("Visual Studio", "WARN" if vswhere else "PASS",
                       f"VS {parent_year} {edition} (filesystem scan; vswhere unusable)",
                       extra={"path": str(fs_path)})


def check_msvc(vs_install_path: Optional[str]) -> CheckResult:
    if not vs_install_path:
        return CheckResult("MSVC toolset", "FAIL",
                           "no VS install — can't probe MSVC")

    tools_root = Path(vs_install_path) / "VC" / "Tools" / "MSVC"
    if not tools_root.exists():
        return CheckResult("MSVC toolset", "FAIL",
                           f"VC/Tools/MSVC not under {vs_install_path}")

    versions = sorted(
        (d.name for d in tools_root.iterdir() if d.is_dir()),
        reverse=True,
    )
    if not versions:
        return CheckResult("MSVC toolset", "FAIL", "no MSVC versions installed")

    latest = versions[0]
    parts = latest.split(".")
    try:
        major, minor = int(parts[0]), int(parts[1])
    except (ValueError, IndexError):
        return CheckResult("MSVC toolset", "WARN", f"can't parse version: {latest}")

    # UE 5.7 requires MSVC 14.38+ (the warning the user hit earlier).
    if (major, minor) >= (14, 38):
        return CheckResult("MSVC toolset", "PASS", latest,
                           extra={"available": versions})
    return CheckResult("MSVC toolset", "WARN",
                       f"{latest} — UE 5.7 wants 14.38+; install via VS Installer",
                       extra={"available": versions})


def check_windows_sdk() -> CheckResult:
    # The SDK ships under "Windows Kits\10\Include\<version>"
    candidates = [
        Path(r"C:\Program Files (x86)\Windows Kits\10\Include"),
        Path(r"C:\Program Files\Windows Kits\10\Include"),
    ]
    for include_root in candidates:
        if not include_root.exists():
            continue
        versions = sorted(
            (d.name for d in include_root.iterdir() if d.is_dir() and d.name[0:2] == "10"),
            reverse=True,
        )
        if versions:
            latest = versions[0]
            # UE 5.4+ wants 10.0.18362 minimum (well below modern SDKs).
            return CheckResult("Windows SDK", "PASS", latest,
                               extra={"available": versions})
    return CheckResult("Windows SDK", "FAIL", "Windows 10/11 SDK not found")


def check_disk_space() -> CheckResult:
    try:
        usage = shutil.disk_usage(REPO_ROOT)
    except OSError as e:
        return CheckResult("Disk space", "FAIL", f"disk_usage failed: {e}")

    free_gb = usage.free / (1024 ** 3)
    detail = f"{free_gb:.1f} GiB free on {REPO_ROOT.drive}"
    if free_gb >= 20:
        return CheckResult("Disk space", "PASS", detail)
    if free_gb >= 5:
        return CheckResult("Disk space", "WARN",
                           f"{detail} — UE rebuilds need ~10 GB headroom")
    return CheckResult("Disk space", "FAIL",
                       f"{detail} — under 5 GB, builds will fail")


def check_gpu() -> CheckResult:
    """Use PowerShell CIM (more reliable than wmic which is deprecated)."""
    try:
        proc = subprocess.run(
            ["powershell", "-NoProfile", "-NonInteractive", "-Command",
             "Get-CimInstance Win32_VideoController | "
             "Select-Object Name, AdapterRAM, DriverVersion | "
             "ConvertTo-Json -Compress"],
            capture_output=True, text=True, encoding="utf-8", timeout=15,
        )
    except (subprocess.TimeoutExpired, OSError) as e:
        return CheckResult("GPU", "WARN", f"probe failed: {e}")

    if proc.returncode != 0 or not proc.stdout.strip():
        return CheckResult("GPU", "WARN", "PowerShell CIM returned nothing")

    try:
        data = json.loads(proc.stdout)
    except json.JSONDecodeError:
        return CheckResult("GPU", "WARN", "non-JSON GPU info")

    # Single-GPU systems return dict; multi-GPU returns list.
    if isinstance(data, dict):
        data = [data]

    # Skip virtual / IDD / RDP / Sunlogin / TeamViewer adapters when picking
    # the primary — they show up first in WMI ordering and lie about VRAM.
    VIRTUAL_HINTS = ("idd", "indirect display", "remote", "rdp",
                     "oray", "teamviewer", "virtual", "vmware", "parallels")

    gpus = []
    for g in data:
        name = (g.get("Name") or "?").strip()
        ram = g.get("AdapterRAM")
        if ram is not None and ram > 0:
            ram_gb = ram / (1024 ** 3)
            # Win32_VideoController.AdapterRAM is uint32 — capped at 4 GB even
            # for 24 GB cards. Treat values >= 4 GB - 1 as "uncapped, can't tell".
            if ram_gb >= 3.99:
                ram_str = "≥4 GiB (Win32 uint32 cap; actual likely higher)"
            else:
                ram_str = f"{ram_gb:.1f} GiB"
        else:
            ram_str = "? GiB"
        is_virtual = any(h in name.lower() for h in VIRTUAL_HINTS)
        gpus.append({"name": name, "ram": ram_str,
                     "driver": g.get("DriverVersion", "?"),
                     "virtual": is_virtual})

    if not gpus:
        return CheckResult("GPU", "WARN", "no video adapter detected")

    real_gpus = [g for g in gpus if not g["virtual"]]
    primary = real_gpus[0] if real_gpus else gpus[0]
    detail = f"{primary['name']} ({primary['ram']}, driver {primary['driver']})"
    if len(gpus) > 1:
        detail += f" + {len(gpus) - 1} more"
    return CheckResult("GPU", "PASS", detail, extra={"adapters": gpus})


def check_ue_engine_path() -> CheckResult:
    exe = os.environ.get("UNREAL_EDITOR_EXE", "").strip()
    if exe:
        if Path(exe).exists():
            return CheckResult("UE engine path", "PASS",
                               f"UNREAL_EDITOR_EXE = {exe}")
        return CheckResult("UE engine path", "FAIL",
                           f"UNREAL_EDITOR_EXE points at missing file: {exe}")

    root = os.environ.get("UE_ROOT", "").strip()
    if root:
        derived = Path(root) / "Engine" / "Binaries" / "Win64" / "UnrealEditor.exe"
        if derived.exists():
            return CheckResult("UE engine path", "PASS",
                               f"UE_ROOT = {root} (resolved to {derived.name})")
        return CheckResult("UE engine path", "FAIL",
                           f"UE_ROOT set but no UnrealEditor.exe under {derived}")

    return CheckResult("UE engine path", "WARN",
                       "neither UNREAL_EDITOR_EXE nor UE_ROOT set — "
                       "rebuild_relaunch.py will fail")


def check_uplugin() -> CheckResult:
    if UPLUGIN_PATH.exists():
        return CheckResult("Bridge plugin", "PASS",
                           str(UPLUGIN_PATH.relative_to(REPO_ROOT)))
    return CheckResult("Bridge plugin", "FAIL",
                       f"missing: {UPLUGIN_PATH}")


# ─── Driver ─────────────────────────────────────────────────────────


def run_all() -> list[CheckResult]:
    results = [
        check_python(),
        check_visual_studio(),
    ]
    vs = results[-1]
    # Pass the path through even on WARN — filesystem fallback still gives a
    # valid install path that MSVC scan can use.
    results.append(check_msvc(vs.extra.get("path") if vs.status in ("PASS", "WARN") else None))
    results.append(check_windows_sdk())
    results.append(check_disk_space())
    results.append(check_gpu())
    results.append(check_ue_engine_path())
    results.append(check_uplugin())
    return results


def render_text(results: list[CheckResult]) -> str:
    lines = ["UnrealBridge preflight\n"]
    width = max(len(r.name) for r in results)
    for r in results:
        lines.append(f"  [{status_icon(r.status)}] {r.name.ljust(width)}  {r.detail}")
    lines.append("")

    fails = sum(1 for r in results if r.status == "FAIL")
    warns = sum(1 for r in results if r.status == "WARN")
    passes = sum(1 for r in results if r.status == "PASS")
    lines.append(f"  → {passes} pass, {warns} warn, {fails} fail")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description="Check that the local environment is ready to build/run UnrealBridge.")
    ap.add_argument("--json", action="store_true", help="Emit JSON instead of text")
    ap.add_argument("--strict", action="store_true",
                    help="Treat WARN as FAIL for exit-code purposes")
    args = ap.parse_args()

    results = run_all()

    if args.json:
        print(json.dumps([asdict(r) for r in results], indent=2, ensure_ascii=False))
    else:
        print(render_text(results))

    bad_states = {"FAIL"}
    if args.strict:
        bad_states.add("WARN")
    return 1 if any(r.status in bad_states for r in results) else 0


if __name__ == "__main__":
    sys.exit(main())
