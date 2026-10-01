#!/usr/bin/env python3
"""Record a compact, inspectable E00 provenance manifest for one fixture run."""
import argparse, hashlib, json, os, pathlib, platform, subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--root", required=True, type=pathlib.Path)
parser.add_argument("--run-dir", required=True, type=pathlib.Path)
parser.add_argument("--backend", required=True)
parser.add_argument("--fixture", required=True)
parser.add_argument("--binary", required=True, type=pathlib.Path)
parser.add_argument("--profile-manifest", required=True, type=pathlib.Path)
parser.add_argument("--user-ltx", required=True, type=pathlib.Path)
args = parser.parse_args()

def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""): h.update(block)
    return h.hexdigest()

def command(*parts):
    try:
        return subprocess.check_output(parts, cwd=args.root, text=True, stderr=subprocess.STDOUT).strip()
    except (OSError, subprocess.CalledProcessError) as error:
        return f"UNAVAILABLE: {error}"

def shader_receipts():
    receipt = {}
    for key, value in sorted(os.environ.items()):
        if not key.startswith("XRAY_METAL_MSC_"): continue
        path = pathlib.Path(value)
        files = [path] if path.is_file() else sorted(path.rglob("*.metallib")) if path.is_dir() else []
        receipt[key] = {"path": str(path.resolve()) if path.exists() else str(path), "files": [{"path": str(file.resolve()), "sha256": digest(file)} for file in files]}
    encoded = json.dumps(receipt, sort_keys=True, separators=(",", ":")).encode()
    return receipt, hashlib.sha256(encoded).hexdigest()

def e00_inputs():
    relative_paths = (
        "docs/direct-metal-release/FIXTURES.json",
        "scratch/axr_main_parity_overlay.patch",
        "scratch/compute_parity_metrics.py",
        "scratch/e00_fixture_scenarios.script",
        "scratch/parity_manifest_contract.py",
        "scratch/zzzz_visual_parity_harness.script",
        "script/annotate_fixture_telemetry.py",
        "script/assemble_release_parity_manifest.py",
        "script/capture_metal_release_provenance.py",
        "script/e00_compile_scope_dxbc.sh",
        "script/e00_fixture_contract.py",
        "script/run_metal_release_fixture.sh",
        "script/summarize_metal_admission.py",
        "script/xray_metal_shader_env.sh",
        "tools/e00_dxbc/e00_scope_dxbc.c",
        "tools/e00_dxbc/verify_cache.py",
        "tools/e00_dxbc/out-scope/provenance-model_scope_lense.txt",
        "tools/e00_dxbc/out-reflex/provenance-models_reflex_reticle_simple_3db.txt",
        "tools/e00_dxbc/out-laserbeam/provenance-models_laserbeam.txt",
        "tools/e00_dxbc/out-scope-reticle-vs/provenance-models_scope_reticle.txt",
    )
    values = {}
    for relative in relative_paths:
        path = args.root / relative
        if not path.is_file():
            raise SystemExit(f"missing E00 provenance input: {path}")
        values[relative] = digest(path)
    required_cache_records = (
        ("model_scope_lense.ps", "20481111001100000000000001001011000100100801216200621000000"),
        ("models_reflex_reticle_simple_3db.ps", "20481111001100000000000100001011000100100801216200621000000"),
        ("models_laserbeam.ps", "20481111001100000000000100001011000100100801216200621000000"),
        ("models_scope_reticle.vs", "20481111001100000000000100001011000100100801216200621000000"),
    )
    for shader, key in required_cache_records:
        cache = args.root / "runtime/macos-gamma/appdata/shaders_cache/r4" / shader / key
        if not cache.is_file():
            raise SystemExit(f"missing E00 shader cache: {cache}")
        values[f"shader_cache:{shader}/{key}"] = digest(cache)
    return values

args.run_dir.mkdir(parents=True, exist_ok=True)
stat = args.binary.stat()
receipts, receipt_hash = shader_receipts()
manifest = {
    "schema": "direct-metal-release-e00/v1", "backend": args.backend, "fixture": args.fixture,
    "binary": {"path": str(args.binary.resolve()), "sha256": digest(args.binary), "mtime_ns": stat.st_mtime_ns, "size": stat.st_size},
    "git_head": command("git", "rev-parse", "HEAD"),
    "dirty_patch_sha256": digest(args.run_dir / "dirty.patch") if (args.run_dir / "dirty.patch").exists() else "",
    "untracked_list_sha256": digest(args.run_dir / "untracked.txt") if (args.run_dir / "untracked.txt").exists() else "",
    "build_options": command("cmake", "-LA", "-N", "build"),
    "tool_versions": {"xcode": command("xcodebuild", "-version"), "msc": command(str(args.root / "build/direct-metal-toolchain/metal-shaderconverter-4.0.1/bin/metal-shaderconverter"), "--version"), "dxc": command("dxc", "--version")},
    "profile": {"manifest": str(args.profile_manifest.resolve()), "manifest_sha256": digest(args.profile_manifest), "user_ltx_sha256": digest(args.user_ltx)},
    "shader_receipts": receipts, "shader_receipt_set_sha256": receipt_hash,
    "e00_inputs": e00_inputs(),
    "effective_env": {key: value for key, value in sorted(os.environ.items()) if key.startswith(("XRAY_", "MTL_", "MVK_", "VK_"))},
    "platform": platform.platform(),
}
(args.run_dir / "provenance.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
