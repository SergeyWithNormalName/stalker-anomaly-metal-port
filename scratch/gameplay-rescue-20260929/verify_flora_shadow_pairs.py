#!/usr/bin/env python3
"""Verify completed production Flora R32 exports; equal clear maps are no coverage."""

import argparse
import array
import json
import math
from pathlib import Path
import sys


def read_map(directory, report):
    if not report.get("command_completed") or report.get("command_error"):
        raise ValueError("GPU command/readback did not complete successfully")
    name = report.get("shadow_map_raw_file", "")
    if not name or Path(name).name != name:
        raise ValueError("Missing or unsafe completed raw map filename")
    data = (directory / name).read_bytes()
    expected = report["shadow_map_width"] * report["shadow_map_height"] * 4
    if len(data) != expected:
        raise ValueError("Raw map byte size does not match its reported extent")
    values = array.array("f")
    values.frombytes(data)
    if values.itemsize != 4:
        raise ValueError("R32 reader requires four-byte floats")
    if sys.byteorder != "little":
        values.byteswap()
    nonfinite = sum(not math.isfinite(value) for value in values)
    covered = sum(math.isfinite(value) and value < 1.0 for value in values)
    return data, values, nonfinite, covered


def verify(directory):
    provenance = {}
    manifest = directory / "flora-pairs.jsonl"
    if manifest.exists():
        for line in manifest.read_text().splitlines():
            entry = json.loads(line)
            identity = (entry["family"], entry["engine_frame"])
            if identity in provenance:
                raise ValueError("Duplicate production pair provenance")
            provenance[identity] = entry
    groups = {}
    for path in sorted(directory.glob("*.json")):
        report = json.loads(path.read_text())
        family = report.get("family", "")
        if not family.startswith(("gameplay-flora-local-", "gameplay-flora-sun-")):
            continue
        for mode in ("legacy", "instanced"):
            if family.endswith("-" + mode):
                identity = (family[: -len(mode) - 1], report["engine_frame"])
                modes = groups.setdefault(identity, {})
                if mode in modes:
                    raise ValueError("Duplicate report for the same production pair/mode")
                modes[mode] = report

    pairs = []
    positive = {"local": 0, "sun": 0}
    failures = []
    for (family, frame), modes in sorted(groups.items()):
        pair = {"family": family, "engine_frame": frame}
        try:
            if set(modes) != {"legacy", "instanced"}:
                raise ValueError("Pair has a missing legacy or instanced completed report")
            baseline, candidate = modes["legacy"], modes["instanced"]
            state = provenance.get((family, frame))
            if not state:
                raise ValueError("Missing same-state production pair provenance")
            if not math.isfinite(state["time_seconds"]):
                raise ValueError("Nonfinite Flora time in pair provenance")
            for key in ("light_view_projection", "wind_params", "wind_animation", "wind_setup_trees"):
                if any(not math.isfinite(value) for value in state[key]):
                    raise ValueError("Nonfinite immutable Flora state: " + key)
            if state["extent"] != baseline["shadow_map_width"] or state["extent"] != baseline["shadow_map_height"]:
                raise ValueError("Map extent does not match live pair provenance")
            for key in ("shadow_map_width", "shadow_map_height",
                        "stage_volume_view_projection", "current_view_projection", "inverse_view"):
                if key not in baseline or baseline[key] != candidate.get(key):
                    raise ValueError("Paired frame/view/extent metadata differs: " + key)
            a, av, anf, ac = read_map(directory, baseline)
            b, bv, bnf, bc = read_map(directory, candidate)
            mismatch_pixels = sum(a[i:i + 4] != b[i:i + 4] for i in range(0, len(a), 4))
            pair.update(mismatch_pixels=mismatch_pixels, nonfinite_pixels=[anf, bnf],
                        covered_pixels=[ac, bc], provenance=state)
            if anf or bnf or a != b:
                raise ValueError("R32 pixel equality or finiteness failed")
            pair["result"] = "PASS" if ac and bc else "NO_COVERAGE"
            if ac and bc:
                if not state["instances"] or not state["legacy_draws"] or not state["instanced_draws"]:
                    raise ValueError("Positive shadow map without admitted live caster draws")
                positive["local" if family.startswith("gameplay-flora-local-") else "sun"] += 1
        except (KeyError, OSError, ValueError) as error:
            pair.update(result="FAIL", error=str(error))
            failures.append(str(error))
        pairs.append(pair)
    if not groups:
        failures.append("No completed production Flora pair reports")
    for kind, count in positive.items():
        if count == 0:
            failures.append("No positive completed " + kind + " Flora shadow pair")
    return {"schema": "gameplay-flora-shadow-pair-comparison/v1",
            "result": "PASS" if not failures else "FAIL", "positive_pairs": positive,
            "pairs": pairs, "failures": failures}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = verify(args.directory)
    text = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.write_text(text)
    print(text, end="")
    return 0 if result["result"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
