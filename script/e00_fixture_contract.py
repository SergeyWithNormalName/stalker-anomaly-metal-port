#!/usr/bin/env python3
"""Fail-closed reader for the small E00 fixture registry.

The runner deliberately consumes only values that have a fixture contract in
FIXTURES.json.  Keeping this validation out of shell avoids accepting a
partially specified fixture because an optional expansion happened to be
empty.
"""
import argparse
import hashlib
import json
import pathlib
import re
import sys


READY_FIELDS = {
    "level": str,
    "camera": str,
    "direction": str,
    "weather": str,
    "weather_weight": (int, float),
    "time": str,
    "save_name": str,
    "save_sha256": str,
    "scope_cache_required": bool,
    "warmup_seconds": (int, float),
    "scenario": str,
    "evidence_sources": dict,
}
CAPTURE_FIELDS = {
    "camera_position_tolerance_m": (int, float),
    "camera_angle_tolerance_rad": (int, float),
    "matrix_tolerance": (int, float),
    "motion_duration_seconds": (int, float),
    "no_hud_settle_seconds": (int, float),
    "timeline_unit": str,
}
ALLOWED_STATUSES = {"ready", "pending"}


def fail(message: str) -> None:
    raise ValueError(message)


def load_registry(path: pathlib.Path) -> dict:
    try:
        registry = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        fail(f"cannot read fixture registry: {error}")

    if registry.get("schema") != "direct-metal-release-fixtures/v1":
        fail("unsupported fixture registry schema")
    fixtures = registry.get("fixtures")
    if not isinstance(fixtures, dict) or set(fixtures) != {f"S{i}" for i in range(8)}:
        fail("fixture registry must define exactly S0 through S7")
    capture = registry.get("capture_contract")
    if not isinstance(capture, dict):
        fail("fixture registry lacks capture_contract")
    for name, expected_type in CAPTURE_FIELDS.items():
        value = capture.get(name)
        if not isinstance(value, expected_type) or isinstance(value, bool) or value == "":
            fail(f"capture_contract.{name} is missing or invalid")
        if name != "timeline_unit" and value <= 0:
            fail(f"capture_contract.{name} must be positive")
    if capture["timeline_unit"] != "seconds":
        fail("capture_contract.timeline_unit must be seconds")

    for name, fixture in fixtures.items():
        if not isinstance(fixture, dict):
            fail(f"{name} must be an object")
        if fixture.get("status") not in ALLOWED_STATUSES:
            fail(f"{name}.status is missing or invalid")
        if not isinstance(fixture.get("purpose"), str) or not fixture["purpose"]:
            fail(f"{name}.purpose is missing")
        if fixture["status"] != "ready":
            continue
        for field, expected_type in READY_FIELDS.items():
            value = fixture.get(field)
            is_boolean_field = expected_type is bool
            if not isinstance(value, expected_type) or (not is_boolean_field and isinstance(value, bool)) or value == "":
                fail(f"ready fixture {name}.{field} is missing or invalid")
        if not re.fullmatch(r"[0-9a-f]{64}", fixture["save_sha256"]):
            fail(f"ready fixture {name}.save_sha256 must be lowercase SHA-256")
        if fixture["warmup_seconds"] <= 0:
            fail(f"ready fixture {name}.warmup_seconds must be positive")
        if not 0 <= fixture["weather_weight"] <= 1:
            fail(f"ready fixture {name}.weather_weight must be between 0 and 1")
        if not re.fullmatch(r"\d\d:\d\d:\d\d", fixture["time"]):
            fail(f"ready fixture {name}.time must be HH:MM:SS")
        sources = fixture["evidence_sources"]
        if set(sources) != {"A_windows_d3d11", "B_crossover_gptk", "C_dxvk_moltenvk", "D_direct_metal"}:
            fail(f"ready fixture {name}.evidence_sources must define A/B/C/D")
        if any(not isinstance(value, str) or not value for value in sources.values()):
            fail(f"ready fixture {name}.evidence_sources values must be nonempty strings")
    return registry


parser = argparse.ArgumentParser()
parser.add_argument("--registry", required=True, type=pathlib.Path)
parser.add_argument("--check", action="store_true")
parser.add_argument("--fixture")
parser.add_argument("--field")
parser.add_argument("--registry-sha256", action="store_true")
args = parser.parse_args()

try:
    registry = load_registry(args.registry)
    if args.check:
        if args.fixture or args.field or args.registry_sha256:
            fail("--check cannot be combined with query arguments")
        sys.exit(0)
    if args.registry_sha256:
        if args.fixture or args.field:
            fail("--registry-sha256 cannot be combined with fixture query arguments")
        print(hashlib.sha256(args.registry.read_bytes()).hexdigest())
        sys.exit(0)
    if not args.fixture or not args.field:
        fail("--fixture and --field are required for a fixture query")
    fixture = registry["fixtures"].get(args.fixture)
    if fixture is None:
        fail(f"unknown fixture {args.fixture}")
    if args.field.startswith("capture_contract."):
        value = registry["capture_contract"].get(args.field.split(".", 1)[1])
    else:
        value = fixture.get(args.field)
    if value is None:
        fail(f"fixture {args.fixture} has no {args.field}")
    if isinstance(value, bool):
        print("true" if value else "false")
    elif isinstance(value, (str, int, float)):
        print(value)
    else:
        fail(f"fixture {args.fixture}.{args.field} is not scalar")
except ValueError as error:
    print(f"E00 fixture registry error: {error}", file=sys.stderr)
    sys.exit(2)
