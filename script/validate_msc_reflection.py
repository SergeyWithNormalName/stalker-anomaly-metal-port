#!/usr/bin/env python3
import argparse
import csv
import json
from pathlib import Path


def manifest_rows(path: Path):
    with path.open(newline="", encoding="utf-8") as stream:
        for row in csv.reader(stream, delimiter="\t"):
            if not row or row[0].startswith("#"):
                continue
            if len(row) != 4:
                raise ValueError(f"invalid manifest row: {row!r}")
            yield row


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--reflection-dir", required=True, type=Path)
    args = parser.parse_args()

    count = 0
    for shader_class, profile, source, permutation in manifest_rows(args.manifest):
        reflection_path = args.reflection_dir / f"{source}.json"
        data = json.loads(reflection_path.read_text(encoding="utf-8"))

        expected_type = "Vertex" if profile.startswith("vs_") else "Fragment"
        if data.get("EntryPoint") != "main" or data.get("ShaderType") != expected_type:
            raise ValueError(f"unexpected entry point/type in {reflection_path}")

        arguments = data.get("TopLevelArgumentBuffer")
        resource_count = data.get("ResourceCount")
        used_resources = data.get("UsedResources")
        if not isinstance(arguments, list) or resource_count != len(arguments):
            raise ValueError(f"resource count mismatch in {reflection_path}")
        if not isinstance(used_resources, list) or len(used_resources) != resource_count:
            raise ValueError(f"used-resource count mismatch in {reflection_path}")

        previous_end = 0
        for binding_index, argument in enumerate(arguments):
            offset = argument.get("EltOffset")
            size = argument.get("Size")
            if not isinstance(offset, int) or not isinstance(size, int) or size <= 0:
                raise ValueError(f"invalid argument layout in {reflection_path}")
            if offset < previous_end:
                raise ValueError(f"overlapping argument layout in {reflection_path}")
            if argument.get("Type") not in {"SRV", "UAV", "CBV", "Sampler"}:
                raise ValueError(f"unknown argument type in {reflection_path}")
            if used_resources[binding_index].get("bindingIndex") != binding_index:
                raise ValueError(f"binding index mismatch in {reflection_path}")
            previous_end = offset + size

        state = data.get("state")
        if not isinstance(state, dict):
            raise ValueError(f"missing stage state in {reflection_path}")
        if expected_type == "Vertex" and not isinstance(state.get("vertex_inputs"), list):
            raise ValueError(f"missing vertex inputs in {reflection_path}")
        if expected_type == "Fragment" and not isinstance(state.get("num_render_targets"), int):
            raise ValueError(f"missing fragment target count in {reflection_path}")

        print(
            "MSC_REFLECTION_READY"
            f" class={shader_class} source={source} type={expected_type}"
            f" resources={resource_count} permutation={permutation}"
        )
        count += 1

    if count == 0:
        raise ValueError("manifest contains no shader entries")
    print(f"MSC_REFLECTION_GATE_COMPLETE shaders={count}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
