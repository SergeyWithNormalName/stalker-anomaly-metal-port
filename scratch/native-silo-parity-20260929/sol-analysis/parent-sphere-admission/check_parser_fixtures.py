#!/usr/bin/env python3
"""Scratch parser data-quality controls; not game/geometry/GPU acceptance."""
from pathlib import Path
import json
import struct
import subprocess
import sys

OUT = Path(__file__).resolve().parent
FIXTURE = OUT / "parser-fixture"
FIXTURE.mkdir(exist_ok=True)
matrix = [1.0 if i % 5 == 0 else 0.0 for i in range(16)]
def as_bits(values):
    return ".".join(f"{struct.unpack('<I', struct.pack('<f', value))[0]:08x}" for value in values)
vp = as_bits(matrix)
eye = as_bits([0, 0, 0])
capture = f"METAL_PARENT_SPHERE_CAPTURE frame=700 device_frame=700 cache_frame=700 source_view_matched=1 allow_r1=0 r1_policy=1 device_vp_bits={vp} view_bits={vp} projection_bits={vp} eye_bits={eye}\n"
view = f"METAL_PARENT_SPHERE_VIEW frame=700 valid=1 snapshot_frame=700 environment_frame=700 environment_valid=1 scope_receivers=0 hud_receivers=1 volume_frame=0 view_bits={vp} projection_bits={vp} eye_bits={eye}\n"
parents = "METAL_PARENT_SPHERE_PARENT frame=700 parent=1 source_static=0 captured_records=6 type=2 shadowed=1 hud=0 volumetric=0 sphere_valid=1 source_outside=1 sphere_x=20 sphere_y=0 sphere_z=1 sphere_radius=1\n"
parents += "METAL_PARENT_SPHERE_PARENT frame=700 parent=2 source_static=0 captured_records=1 type=1 shadowed=1 hud=1 volumetric=0 sphere_valid=1 source_outside=1 sphere_x=20 sphere_y=0 sphere_z=1 sphere_radius=1\n"
faces = "".join(f"METAL_PARENT_SPHERE_FACE frame=700 batch=0 tile={i} parent=1 light={101+i} face={i} type=2 source_static=0 material_rows=2 static_geometry_draws=1 dynamic_geometry_casters=3 flora_shadow_draws=4 transform_bytes=512\n" for i in range(6))
faces += "METAL_PARENT_SPHERE_FACE frame=700 batch=0 tile=6 parent=2 light=201 face=0 type=1 source_static=0 material_rows=1 static_geometry_draws=1 dynamic_geometry_casters=1 flora_shadow_draws=1 transform_bytes=256\n"
base = capture + parents + "METAL_PARENT_SPHERE_CAPTURE_DONE frame=700 parents=2 records=7\n" + view + faces
e01 = "serial,engine_frame,status,indexed_draws,pso_binds,updated_bytes\n690,700,completed,100,90,5000\n"
phase = "engine_frame,phase\n700,1\n"
cases = {
    "valid": (base, e01, phase, None),
    "accepted_projection_mismatch": (base.replace("projection_bits=" + vp, "projection_bits=" + as_bits([2.0]+matrix[1:]), 1), e01, phase, "accepted EngineScene projection_bits differs"),
    "stale_cache_frame": (base.replace("cache_frame=700", "cache_frame=699"), e01, phase, "frame mismatch"),
    "unmatched_device_vp": (base.replace("source_view_matched=1", "source_view_matched=0"), e01, phase, "Device VP did not match"),
    "duplicate_capture_retry": (capture + base, e01, phase, "one capture"),
    "incomplete_omni": (base.replace(faces.splitlines(True)[5], ""), e01, phase, "complete ordinary point/spot packages"),
    "warmup": (base, e01, phase.replace("700,1", "700,0"), "measured phase1"),
    "gpu_error": (base, e01.replace("completed", "error"), phase, "completed acquisition"),
    "r1_gate_absent": (base.replace("r1_policy=1", "r1_policy=0"), e01, phase, "R1 policy enabled"),
}
receipts = []
for name, (log, csv_e01, csv_phase, rejection) in cases.items():
    directory = FIXTURE / name
    directory.mkdir(exist_ok=True)
    (directory / "engine.log").write_text(log)
    (directory / "e01.csv").write_text(csv_e01)
    (directory / "phases.csv").write_text(csv_phase)
    result = subprocess.run([sys.executable, str(OUT / "summarize_observer.py"), str(directory / "engine.log"),
                             "--frame", "700", "--e01", str(directory / "e01.csv"),
                             "--device-phases", str(directory / "phases.csv"), "--require-corrected-r1"],
                            capture_output=True, text=True)
    if rejection is None:
        assert result.returncode == 0, result.stderr
        summary = json.loads(result.stdout)
        assert summary["actual_rendered"]["faces"] == 7
        assert summary["source_outside_actual_rendered"]["faces"] == 7
        assert summary["ordinary_dynamic_source_outside_actual_rendered"]["faces"] == 6
        assert summary["ordinary_dynamic_source_outside_actual_rendered"]["material_rows"] == 12
        assert summary["completed_e01"]["serial"] == 690
        (directory / "summary.json").write_text(result.stdout)
    else:
        assert result.returncode != 0 and rejection in result.stderr, (name, result.returncode, result.stderr)
    receipts.append({"case": name, "result": "PASS", "expected_rejection": rejection})
receipt = {"scope": "synthetic parser provenance fixtures only; no live telemetry, CFrustum, build or GPU acceptance",
           "cases": receipts}
(FIXTURE / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
print(json.dumps(receipt, indent=2))
