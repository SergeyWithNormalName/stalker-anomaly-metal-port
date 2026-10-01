#!/usr/bin/env python3
import argparse, json, pathlib

parser = argparse.ArgumentParser()
parser.add_argument("telemetry", type=pathlib.Path)
parser.add_argument("--drawable", required=True)
parser.add_argument("--source", required=True)
args = parser.parse_args()
width, height = (int(value) for value in args.drawable.split("x", 1))
data = json.loads(args.telemetry.read_text())
data.update({"actual_drawable_width": width, "actual_drawable_height": height, "actual_drawable_source": args.source})
args.telemetry.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
