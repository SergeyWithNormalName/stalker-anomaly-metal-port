#!/usr/bin/env python3
"""Materialize the selected GAMMA Mod Organizer 2 profile for native macOS.

MO2 normally exposes a virtual Windows filesystem.  The native Anomaly port
cannot load that VFS, so this tool creates a project-local tree of absolute
symlinks. It never writes to the GAMMA/MO2 installation. The installed GAMMA
USVFS view was verified against a native preflight: base Anomaly is lowest,
then the CRLF modlist is applied bottom to top (top entries win), then MO2's
overwrite directory is highest.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


VERSION = 4
TREE_NAMES = ("gamedata", "db")


@dataclass(frozen=True)
class ModEntry:
    name: str
    enabled: bool
    line_number: int
    separator: bool


@dataclass(frozen=True)
class Candidate:
    relative_path: str
    source: str
    layer: str
    priority: int


class MaterializationError(RuntimeError):
    pass


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def selected_profile_name(mo2_ini: Path) -> str:
    text = mo2_ini.read_text(encoding="utf-8", errors="replace")
    match = re.search(r"^selected_profile=@ByteArray\(([^)]*)\)\s*$", text, re.MULTILINE)
    if not match:
        raise MaterializationError(f"MO2 selected_profile is missing from {mo2_ini}")

    # Current GAMMA's value is plain ASCII (MyTree).  Reject an escaped Qt
    # byte-array rather than guessing a profile name and materializing the
    # wrong one.
    profile = match.group(1)
    if not profile or "\\" in profile or "/" in profile:
        raise MaterializationError(f"Unsupported MO2 selected_profile encoding: {profile!r}")
    return profile


def parse_modlist(path: Path) -> list[ModEntry]:
    entries: list[ModEntry] = []
    # splitlines() intentionally removes CR from MO2's CRLF modlist.txt.
    for line_number, line in enumerate(path.read_text(encoding="utf-8-sig", errors="replace").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        if line[0] not in "+-":
            raise MaterializationError(f"Unexpected modlist entry at {path}:{line_number}: {line!r}")
        name = line[1:]
        if not name:
            raise MaterializationError(f"Empty mod name at {path}:{line_number}")
        entries.append(ModEntry(name, line[0] == "+", line_number, "separator" in name.casefold()))
    return entries


def active_layers(mo2_root: Path, entries: Iterable[ModEntry]) -> tuple[list[tuple[str, Path]], list[str], list[str]]:
    layers: list[tuple[str, Path]] = []
    missing: list[str] = []
    root_only: list[str] = []
    for entry in entries:
        if not entry.enabled or entry.separator:
            continue
        root = mo2_root / "mods" / entry.name
        if not root.is_dir():
            missing.append(entry.name)
            continue
        layers.append((f"MO2 mod: {entry.name}", root))
        if not (root / "gamedata").is_dir() and not (root / "db").is_dir():
            root_only.append(entry.name)
    return layers, missing, root_only


def add_tree(
    winners: dict[str, Candidate],
    conflicts: dict[str, list[Candidate]],
    case_variants: dict[str, list[Candidate]],
    hidden_entries: list[Candidate],
    root: Path,
    layer: str,
    priority: int,
    progress: dict[str, int],
) -> None:
    if not root.is_dir():
        return

    for directory, directories, files in os.walk(root, followlinks=False):
        directories.sort(key=str.casefold)
        files.sort(key=str.casefold)
        symlinked_directories = [name for name in directories if (Path(directory) / name).is_symlink()]
        if symlinked_directories:
            rendered = ", ".join(symlinked_directories[:4])
            raise MaterializationError(
                f"Directory symlinks are not safe to merge without an explicit MO2 rule: {Path(directory)} ({rendered})"
            )

        directory_path = Path(directory)
        for filename in files:
            source = directory_path / filename
            relative = source.relative_to(root).as_posix()
            # USVFS explicitly registers .mohidden as a skipped suffix.  The
            # suffix is an MO2 control marker, not a file visible to Anomaly;
            # skipping it lets a lower-priority real file remain visible.
            if source.name.casefold().endswith(".mohidden"):
                hidden_entries.append(Candidate(relative, str(source.absolute()), layer, priority))
                continue
            key = relative.casefold()
            candidate = Candidate(relative, str(source.absolute()), layer, priority)
            previous = winners.get(key)
            if previous is not None:
                history = conflicts.setdefault(key, [previous])
                history.append(candidate)
                if previous.relative_path != candidate.relative_path:
                    variants = case_variants.setdefault(key, [previous])
                    variants.append(candidate)
            winners[key] = candidate
            progress["scanned_files"] += 1
            if progress["scanned_files"] % 50000 == 0:
                print(f"  scanned {progress['scanned_files']:,} source files…", file=sys.stderr, flush=True)


def add_compatibility_pda_overlay(
    winners: dict[str, Candidate],
    conflicts: dict[str, list[Candidate]],
    case_variants: dict[str, list[Candidate]],
    compatibility_script: Path | None,
    priority: int,
) -> bool:
    relative = "scripts/pda_dynamic_tabs.script"
    key = relative.casefold()
    if compatibility_script is None or not compatibility_script.is_file() or key in winners:
        return False
    candidate = Candidate(
        relative,
        str(compatibility_script.absolute()),
        "macOS/xray-monolith compatibility overlay: pda_dynamic_tabs.script",
        priority,
    )
    winners[key] = candidate
    return True


def add_compatibility_fix_relations_overlay(
    winners: dict[str, Candidate],
    priority: int,
) -> bool:
    compat_script = Path(__file__).resolve().parent.parent / "gamedata" / "scripts" / "zzz_fix_relations.script"
    if not compat_script.is_file():
        return False
    relative = "scripts/zzz_fix_relations.script"
    key = relative.casefold()
    winners[key] = Candidate(
        relative,
        str(compat_script.absolute()),
        "macOS/xray-monolith compatibility overlay: zzz_fix_relations.script",
        priority,
    )
    return True


def add_compatibility_gamepad_overlay(
    winners: dict[str, Candidate],
    priority: int,
) -> bool:
    compat_script = Path(__file__).resolve().parent.parent / "gamedata" / "scripts" / "gamepad_ui_compat.script"
    if not compat_script.is_file():
        return False
    relative = "scripts/gamepad_ui_compat.script"
    key = relative.casefold()
    winners[key] = Candidate(
        relative,
        str(compat_script.absolute()),
        "macOS/xray-monolith compatibility overlay: gamepad_ui_compat.script",
        priority,
    )
    return True


def write_symlink_tree(destination: Path, winners: dict[str, Candidate]) -> None:
    for index, candidate in enumerate(sorted(winners.values(), key=lambda item: item.relative_path.casefold()), 1):
        target = destination / candidate.relative_path
        target.parent.mkdir(parents=True, exist_ok=True)
        os.symlink(candidate.source, target)
        if index % 50000 == 0:
            print(f"  created {index:,} profile links…", file=sys.stderr, flush=True)


def category_matchers() -> list[tuple[str, tuple[tuple[str, ...], ...]]]:
    return [
        ("main menu / UI", (("textures/gammamainmenulogo.dds",), ("textures/ui/", "main_menu"), ("textures/ui/",))),
        ("inventory / item UI", (("textures/ui/", "ui_actor_menu"), ("textures/ui/", "icon"))),
        ("starting-equipment UI", (("textures/ui/ui_icon_k98.dds",), ("configs/items/settings/new_game_loadouts.ltx",))),
        ("gameplay scripts", (("scripts/",),)),
        ("weapon configs", (("configs/items/weapons/",),)),
        ("actor / loadout configs", (("configs/creatures/actor.ltx",), ("configs/loadouts/",))),
        ("PDA UI", (("configs/ui/", "pda"), ("textures/ui/", "pda"), ("scripts/", "pda"))),
        ("shaders", (("shaders/",),)),
        ("textures", (("textures/",),)),
    ]


def category_hit(
    patterns: tuple[tuple[str, ...], ...], candidates: dict[str, Candidate], conflicts: dict[str, list[Candidate]]
) -> tuple[Candidate, list[Candidate]] | None:
    # Prefer an actual conflict so the report proves priority semantics.  Fall
    # back to a single-source winner when a category has no conflict.
    ordered_sets = list(conflicts.values())
    for terms in patterns:
        for values in ordered_sets:
            if all(term in values[-1].relative_path.casefold() for term in terms):
                return values[-1], values
    for terms in patterns:
        for candidate in sorted(candidates.values(), key=lambda item: item.relative_path.casefold()):
            if all(term in candidate.relative_path.casefold() for term in terms):
                return candidate, [candidate]
    return None


def render_candidates(candidates: list[Candidate]) -> str:
    return " → ".join(item.layer for item in candidates)


def report(
    stage: Path,
    mo2_root: Path,
    game_root: Path,
    profile: str,
    modlist: Path,
    mo2_ini: Path,
    entries: list[ModEntry],
    layers: list[tuple[str, Path]],
    root_only: list[str],
    winners_by_tree: dict[str, dict[str, Candidate]],
    conflicts_by_tree: dict[str, dict[str, list[Candidate]]],
    case_variants_by_tree: dict[str, dict[str, list[Candidate]]],
    hidden_entries_by_tree: dict[str, list[Candidate]],
    compatibility_overlay_applied: bool,
    gamepad_compatibility_overlay_applied: bool,
) -> dict:
    enabled = [entry for entry in entries if entry.enabled]
    disabled = [entry for entry in entries if not entry.enabled]
    enabled_separators = [entry for entry in enabled if entry.separator]
    enabled_mods = [entry for entry in enabled if not entry.separator]
    rows: list[str] = [
        "# Native macOS GAMMA MO2 materialization",
        "",
        "This directory is a symlinked, read-only view of the actual selected MO2 profile. "
        "No GAMMA/MO2 source files were copied or modified.",
        "",
        "## Source profile",
        "",
        f"- MO2 root: `{mo2_root}`",
        f"- Base Anomaly root: `{game_root}`",
        f"- Selected profile from `ModOrganizer.ini`: `{profile}`",
        f"- `modlist.txt` SHA-256: `{sha256(modlist)}`",
        f"- `ModOrganizer.ini` SHA-256: `{sha256(mo2_ini)}`",
        f"- Enabled entries: **{len(enabled)}** ({len(enabled_mods)} mods, {len(enabled_separators)} separators)",
        f"- Disabled entries: **{len(disabled)}**",
        "",
        "## Resolution order",
        "",
        "The installed GAMMA USVFS view resolves this `modlist.txt` bottom to top: entries nearer the top "
        "have higher virtual-file priority. This was verified against the real Windows/MO2 launch by the "
        "conflicting 3DSS/BAS weapon configuration and the resulting filesystem inventory. This "
        "materialization applies base Anomaly first, then enabled non-separator entries bottom to top, then "
        "`overwrite` last.",
        "",
        "- Base Anomaly: lowest priority",
        f"- Enabled MO2 mods materialized: {len(layers)}",
        "- MO2 `overwrite`: highest priority",
        "",
        "## Materialized trees",
        "",
    ]
    for name in TREE_NAMES:
        winners = winners_by_tree[name]
        conflicts = conflicts_by_tree[name]
        variants = case_variants_by_tree[name]
        rows.append(
            f"- `{name}/`: {len(winners):,} winning files; {len(conflicts):,} paths had MO2 priority conflicts; "
            f"{len(variants):,} case-only spelling conflicts were resolved by that same priority rule; "
            f"{len(hidden_entries_by_tree[name]):,} `.mohidden` MO2 control files were skipped."
        )
    rows.extend([
        "",
        "## Compatibility overlay",
        "",
        "The active GAMMA profile does not provide `gamedata/scripts/pda_dynamic_tabs.script`, while the native "
        "xray-monolith UI calls `pda_dynamic_tabs.build_extra_tabs`. "
        + ("The PDA compatibility script was layered after MO2. " if compatibility_overlay_applied else "")
        + (
            "The gamepad UI compatibility script was layered after MO2."
            if gamepad_compatibility_overlay_applied
            else "The gamepad UI compatibility source file was absent."
        ),
        "The gamepad compatibility layer uses a separate script and leaves MO2-provided GAMMA scripts intact.",
        "",
        "## Representative winning paths",
        "",
        "| category | virtual path | candidate layers, low → high | winning materialized source |",
        "| --- | --- | --- | --- |",
    ])
    for category, terms in category_matchers():
        hit = category_hit(terms, winners_by_tree["gamedata"], conflicts_by_tree["gamedata"])
        if hit is None:
            rows.append(f"| {category} | _not found_ | — | — |")
            continue
        winner, candidates = hit
        rows.append(
            f"| {category} | `{winner.relative_path}` | `{render_candidates(candidates)}` | `{winner.source}` |"
        )

    db_hit = category_hit((("highresloadingscreens",),), winners_by_tree["db"], conflicts_by_tree["db"])
    if db_hit is not None:
        winner, candidates = db_hit
        rows.append(
            f"| loading-screen archive | `db/{winner.relative_path}` | `{render_candidates(candidates)}` | `{winner.source}` |"
        )

    rows.extend([
        "",
        "## Entries without gamedata/db",
        "",
        "These enabled MO2 entries contain neither `gamedata` nor `db`; they do not contribute files to the native "
        "engine's data roots:",
    ])
    if root_only:
        rows.extend(f"- `{name}`" for name in root_only)
    else:
        rows.append("- None")

    rows.extend(["", "## MO2 hidden-file semantics", ""])
    if any(hidden_entries_by_tree.values()):
        rows.append("USVFS logs show `.mohidden` as a skipped suffix. These entries were not exposed:")
        for tree_name in TREE_NAMES:
            for entry in hidden_entries_by_tree[tree_name]:
                rows.append(f"- `{tree_name}/{entry.relative_path}` from `{entry.layer}`")
    else:
        rows.append("No `.mohidden` entries were present in the selected profile.")

    report_path = stage / "MO2_PROFILE_REPORT.md"
    report_path.write_text("\n".join(rows) + "\n", encoding="utf-8")

    manifest = {
        "format_version": VERSION,
        "generated_at_epoch": int(time.time()),
        "mo2_root": str(mo2_root),
        "base_anomaly_root": str(game_root),
        "selected_profile": profile,
        "modlist_path": str(modlist),
        "modlist_sha256": sha256(modlist),
        "modorganizer_ini_path": str(mo2_ini),
        "modorganizer_ini_sha256": sha256(mo2_ini),
        "priority_direction": "base Anomaly -> modlist bottom-to-top (earlier/top entries win) -> overwrite",
        "entries": {
            "enabled": len(enabled),
            "disabled": len(disabled),
            "enabled_mods": len(enabled_mods),
            "enabled_separators": len(enabled_separators),
        },
        "enabled_mods_in_low_to_high_priority_order": [entry.name for entry in reversed(enabled_mods)],
        "enabled_mods_without_gamedata_or_db": root_only,
        "trees": {
            name: {
                "winning_files": len(winners_by_tree[name]),
                "conflicted_virtual_paths": len(conflicts_by_tree[name]),
                "case_only_conflicted_paths": len(case_variants_by_tree[name]),
                "mohidden_entries_skipped": len(hidden_entries_by_tree[name]),
            }
            for name in TREE_NAMES
        },
        "compatibility_overlay": {
            "pda_dynamic_tabs_script": compatibility_overlay_applied,
            "gamepad_ui_compat_script": gamepad_compatibility_overlay_applied,
            "pda_reason": "Native UI invokes it and the active GAMMA profile does not provide it.",
            "gamepad_reason": "Native gamepad navigation needs semantic callbacks for GAMMA's Lua cell widgets.",
        },
    }
    (stage / "profile-manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mo2-root", type=Path, required=True)
    parser.add_argument("--game-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--compat-pda-script", type=Path)
    parser.add_argument(
        "--check-current",
        action="store_true",
        help="exit 0 only when the existing materialization matches the active MO2 profile",
    )
    args = parser.parse_args()

    mo2_root = args.mo2_root.resolve()
    game_root = args.game_root.resolve()
    output = args.output.resolve()
    mo2_ini = mo2_root / "ModOrganizer.ini"
    if not mo2_ini.is_file():
        raise MaterializationError(f"Missing MO2 configuration: {mo2_ini}")
    if not game_root.is_dir():
        raise MaterializationError(f"Missing base Anomaly root: {game_root}")

    profile = selected_profile_name(mo2_ini)
    modlist = mo2_root / "profiles" / profile / "modlist.txt"
    if not modlist.is_file():
        raise MaterializationError(f"Selected MO2 profile has no modlist: {modlist}")
    entries = parse_modlist(modlist)
    layers, missing, root_only = active_layers(mo2_root, entries)
    if missing:
        joined = "\n  - ".join(missing)
        raise MaterializationError(f"Enabled MO2 mods missing from mods/:\n  - {joined}")

    if args.check_current:
        manifest_path = output / "profile-manifest.json"
        if not manifest_path.is_file() or not (output / "gamedata").is_dir() or not (output / "db").is_dir():
            print("MO2 materialization is missing.", file=sys.stderr)
            return 1
        try:
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            print(f"MO2 materialization manifest is unreadable: {error}", file=sys.stderr)
            return 1
        expected = {
            "format_version": VERSION,
            "selected_profile": profile,
            "modlist_sha256": sha256(modlist),
            "modorganizer_ini_sha256": sha256(mo2_ini),
            "base_anomaly_root": str(game_root),
            "mo2_root": str(mo2_root),
        }
        stale_keys = [key for key, value in expected.items() if manifest.get(key) != value]
        if stale_keys:
            print("MO2 materialization is stale: " + ", ".join(stale_keys), file=sys.stderr)
            return 1
        print(f"MO2 materialization is current for profile {profile}.")
        return 0

    stage = output.with_name(output.name + f".staging-{os.getpid()}")
    if stage.exists():
        raise MaterializationError(f"Refusing to reuse pre-existing staging directory: {stage}")
    stage.mkdir(parents=True)

    try:
        winners_by_tree: dict[str, dict[str, Candidate]] = {name: {} for name in TREE_NAMES}
        conflicts_by_tree: dict[str, dict[str, list[Candidate]]] = {name: {} for name in TREE_NAMES}
        case_variants_by_tree: dict[str, dict[str, list[Candidate]]] = {name: {} for name in TREE_NAMES}
        hidden_entries_by_tree: dict[str, list[Candidate]] = {name: [] for name in TREE_NAMES}
        progress = {"scanned_files": 0}

        # The modlist is serialized in reverse of the effective GAMMA VFS
        # priority: lower file lines are applied first and top lines win.
        applied_layers = list(reversed(layers))
        print(f"Selected MO2 profile: {profile}", flush=True)
        print(f"Enabled mods: {len(layers)}; materializing symlinked gamedata and db trees…", flush=True)
        for name in TREE_NAMES:
            priority = 0
            add_tree(
                winners_by_tree[name],
                conflicts_by_tree[name],
                case_variants_by_tree[name],
                hidden_entries_by_tree[name],
                game_root / name,
                "Base Anomaly",
                priority,
                progress,
            )
            priority += 1
            for layer_name, mod_root in applied_layers:
                add_tree(
                    winners_by_tree[name],
                    conflicts_by_tree[name],
                    case_variants_by_tree[name],
                    hidden_entries_by_tree[name],
                    mod_root / name,
                    layer_name,
                    priority,
                    progress,
                )
                priority += 1
            add_tree(
                winners_by_tree[name],
                conflicts_by_tree[name],
                case_variants_by_tree[name],
                hidden_entries_by_tree[name],
                mo2_root / "overwrite" / name,
                "MO2 overwrite",
                priority,
                progress,
            )

        compatibility_overlay_applied = add_compatibility_pda_overlay(
            winners_by_tree["gamedata"],
            conflicts_by_tree["gamedata"],
            case_variants_by_tree["gamedata"],
            args.compat_pda_script.resolve() if args.compat_pda_script else None,
            len(layers) + 2,
        )
        add_compatibility_fix_relations_overlay(
            winners_by_tree["gamedata"],
            len(layers) + 2,
        )
        gamepad_compatibility_overlay_applied = add_compatibility_gamepad_overlay(
            winners_by_tree["gamedata"],
            len(layers) + 3,
        )

        for name in TREE_NAMES:
            print(f"Writing {name}: {len(winners_by_tree[name]):,} symlinks…", flush=True)
            write_symlink_tree(stage / name, winners_by_tree[name])

        manifest = report(
            stage,
            mo2_root,
            game_root,
            profile,
            modlist,
            mo2_ini,
            entries,
            layers,
            root_only,
            winners_by_tree,
            conflicts_by_tree,
            case_variants_by_tree,
            hidden_entries_by_tree,
            compatibility_overlay_applied,
            gamepad_compatibility_overlay_applied,
        )

        previous = output.with_name(output.name + ".previous")
        if previous.exists():
            shutil.rmtree(previous)
        if output.exists():
            output.rename(previous)
        stage.rename(output)
        print(
            "Materialization complete: "
            f"{manifest['trees']['gamedata']['winning_files']:,} gamedata files, "
            f"{manifest['trees']['db']['winning_files']:,} db files.",
            flush=True,
        )
        return 0
    except Exception:
        shutil.rmtree(stage, ignore_errors=True)
        raise


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except MaterializationError as error:
        print(f"MO2 materialization failed: {error}", file=sys.stderr)
        raise SystemExit(2)
