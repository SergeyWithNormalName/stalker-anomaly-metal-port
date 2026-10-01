#!/usr/bin/env python3
"""Cook original E06 bloom-build/luminance chain and legacy admission metadata."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

SHADERS = ["stub_notransform_build.vs", "stub_notransform_filter.vs", "bloom_build.ps",
           "bloom_luminance_1.ps", "bloom_luminance_2.ps", "bloom_luminance_3.ps"]


def write_admission_metadata(output):
    receipt = json.loads((output / "effects-msc-receipt.json").read_text())
    lines = ["scope=" + receipt["scope"], "source_root=" + receipt["source_root"],
             "dxc_version=" + receipt["dxc_version"], "msc_version=" + receipt["msc_version"]]
    for name in SHADERS:
        artifact = receipt["outputs"][name]
        # Existing stand-alone luminance admission expects .json; retain the
        # canonical MSC .reflection.json and duplicate identical metadata only.
        (output / (name + ".json")).write_bytes((output / (name + ".reflection.json")).read_bytes())
        lines.append("stage=" + name[-2:] + "\tsource=" + name +
                     "\tsource_sha256=" + receipt["inputs"][name] +
                     "\tdxil_sha256=" + artifact[".dxil"] +
                     "\tmetallib_sha256=" + artifact[".metallib"] +
                     "\treflection_sha256=" + artifact[".reflection.json"])
    (output / "luminance-msc-receipt.txt").write_text("\n".join(lines) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    cook = Path(__file__).with_name("compile_r4_e06_effects_msc.py")
    command = [sys.executable, str(cook), "--source-root", str(args.source_root), "--output", str(args.output)]
    for name in SHADERS:
        command += ["--shader", name]
    subprocess.run(command, check=True)
    write_admission_metadata(args.output)


if __name__ == "__main__":
    main()
