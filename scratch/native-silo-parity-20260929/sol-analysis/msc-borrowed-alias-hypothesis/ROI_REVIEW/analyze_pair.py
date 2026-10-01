#!/usr/bin/env python3
"""Read-only matched ROI review for the borrowed MSC typed-alias flag pair."""
from __future__ import annotations
import csv, hashlib, importlib.util, json, math, statistics, sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[5]
STAGE = ROOT / "scratch/native-silo-parity-20260929"
HYP = STAGE / "sol-analysis/msc-borrowed-alias-hypothesis"
ANALYSIS = Path(__file__).resolve().parent
GENERAL = STAGE / "sol-analysis/source-topology-wind-census/portal-architecture/general-context"
RUNS = {"off": STAGE / "runs/msc-borrowed-alias-off-01/native/output",
        "on": STAGE / "runs/msc-borrowed-alias-on-01/native/output",
        "dedup": STAGE / "runs/msc-dedup-clean-on-01/native/output"}
EXPECTED = {
    "candidate_exe_sha256": "56f9848724ffeed6ea5ea05b7864e23c27c60b6958025d234a5171cea0a163ae",
    "manifest_sha256": "cb6f0b62b3d83615c78156cfa31d97cc90fa208dcd1287e7d50722a02bae8613",
    "profile_sha256": "41312db9a6b06ef5876bc34fe3753e8cc6480711672618dd345331773f1601fb",
    "user_ltx_sha256": "b82f466fb766cc140256ca8953f4f80768ce5f8d6ff18deb126e3abfd51b3334",
    "save_sha256": "4558915180d6fdc11d22006b4f9dff850c581fc15c57514b1f8cec39252b9225",
    "ordinary_exe_sha256": "d3d93d5bb7c5883c8a453f1f5e24d6a690da4ed769d5ab201203a8db95484fa0",
    "ordinary_probe_sha256": "a43001af4780ac809a4a93ec5e4042cb9c4a3c911c0861a550e490a325d01274",
    "source_sha256": "365115ea87703d4038a45b95e9a8decc4d0769f2b091159e253930c36892bebc",
}
BASE_PATH = GENERAL / "PERFRAME_ROI_REVIEW/analyze_perframe_roi.py"
spec = importlib.util.spec_from_file_location("perframe_roi_base", BASE_PATH)
if spec is None or spec.loader is None: raise RuntimeError(f"cannot import metric helper {BASE_PATH}")
BASE = importlib.util.module_from_spec(spec); spec.loader.exec_module(BASE)

def sha(p: Path) -> str: return hashlib.sha256(p.read_bytes()).hexdigest()
def read_env(p: Path) -> dict[str,str]:
    return {k:v for line in p.read_text(errors="strict").splitlines() if "=" in line and not line.lstrip().startswith("#") for k,v in [line.split("=",1)]}
def read_shared(p: Path, suffix: str) -> str:
    for line in p.read_text().splitlines():
        parts=line.split()
        if len(parts)>=2 and parts[-1].endswith(suffix): return parts[0]
    raise ValueError(f"missing shared-state receipt {suffix}")
def build_gate() -> dict[str,Any]:
    result=json.loads((HYP/"root-build-01/result.json").read_text())
    candidate=STAGE/"candidate-msc-borrowed-alias-01/Anomaly.app/Contents/MacOS/AnomalyDX11"
    manifest=STAGE/"candidate-msc-borrowed-alias-01/source-manifest.json"
    if result.get("build_exit")!=0 or result.get("normal_build_exit")!=0: raise ValueError("candidate or restore build failed")
    if result.get("candidate_executable_sha256")!=EXPECTED["candidate_exe_sha256"] or result.get("manifest_sha256")!=EXPECTED["manifest_sha256"]: raise ValueError("candidate build pins mismatch")
    if result.get("ordinary_exe_before")!=EXPECTED["ordinary_exe_sha256"]: raise ValueError("ordinary executable prestate mismatch")
    restored=result.get("ordinary_outputs_restored",{})
    if restored.get("app")!=EXPECTED["ordinary_exe_sha256"] or restored.get("bare")!=EXPECTED["ordinary_exe_sha256"] or restored.get("probe")!=EXPECTED["ordinary_probe_sha256"] or result.get("ordinary_probe_before")!=EXPECTED["ordinary_probe_sha256"]: raise ValueError("ordinary R1 outputs/probe not restored")
    pre=result.get("production_prestate",{}); post=result.get("restored_source_hashes",{})
    if pre!={"src/Layers/xrRenderMetal/MetalR4Backend.mm":EXPECTED["source_sha256"]} or pre!=post: raise ValueError("production source restoration mismatch")
    if sha(candidate)!=EXPECTED["candidate_exe_sha256"] or sha(manifest)!=EXPECTED["manifest_sha256"]: raise ValueError("candidate current files mismatch")
    # This probe separately exercised fixture draws with the candidate alias flag enabled.
    probe=HYP/"root-build-01/msc-binding-gpu-probe.log"
    build_script=HYP/"build_candidate.py"
    log=probe.read_text(errors="strict")
    if result.get("msc_binding_gpu_probe_exit")!=0 or "E08_FLORA_INSTANCING_GPU PASS" not in log: raise ValueError("GPU fixture probe did not pass")
    if "XRAY_METAL_E08_BORROW_MSC_TYPES='1'" not in build_script.read_text(): raise ValueError("GPU fixture probe was not configured with borrowed alias flag 1")
    return {"candidate_exe_sha256":sha(candidate),"manifest_sha256":sha(manifest),"ordinary_R1_restored":True,
            "production_source_restored":True,"probe_exit":result["msc_binding_gpu_probe_exit"],
            "probe_flag":"XRAY_METAL_E08_BORROW_MSC_TYPES=1","probe_pass_marker":"E08_FLORA_INSTANCING_GPU PASS"}

def env_gate() -> dict[str,Any]:
    env={arm:read_env(RUNS[arm]/"environment.txt") for arm in RUNS}
    def norm(arm: str, val: str) -> str:
        return val.replace(str(STAGE/f"runs/msc-borrowed-alias-{arm}-01"), "<RUN>")
    a={k:norm("off",v) for k,v in env["off"].items()}; b={k:norm("on",v) for k,v in env["on"].items()}
    if set(a)!=set(b): raise ValueError(f"environment key mismatch: {sorted(set(a)^set(b))}")
    delta={k:(a[k],b[k]) for k in sorted(a) if a[k]!=b[k]}
    expected={"XRAY_METAL_E08_BORROW_MSC_TYPES":("0","1")}
    if delta!=expected: raise ValueError(f"environment differences exceed the candidate flag: {delta}")
    return {"passed":True,"only_setting_difference":delta}

def dedup_env_gate() -> dict[str,Any]:
    off=read_env(RUNS["off"]/"environment.txt")
    dedup=read_env(RUNS["dedup"]/"environment.txt")
    def norm(label: str, env: dict[str,str]) -> dict[str,str]:
        return {k:v.replace(str(STAGE/f"runs/msc-borrowed-alias-{label}-01"),"<RUN>")
            .replace(str(STAGE/"runs/msc-dedup-clean-on-01"),"<RUN>") for k,v in env.items()}
    a,b=norm("off",off),norm("dedup",dedup)
    # The off build predates this explicit default-off variable; absence is the documented default.
    a.setdefault("XRAY_METAL_E08_DEDUP_RESOURCES","0")
    delta={k:(a.get(k),b.get(k)) for k in sorted(set(a)|set(b)) if a.get(k)!=b.get(k)}
    expected={"XRAY_METAL_E08_DEDUP_RESOURCES":("0","1")}
    if delta!=expected: raise ValueError(f"dedup diagnostic differs beyond its one setting: {delta}")
    if b.get("XRAY_METAL_E08_BORROW_MSC_TYPES")!="0": raise ValueError("dedup run unexpectedly enables borrowed aliases")
    return {"passed":True,"only_setting_difference":delta}

def runtime_manifest_gate() -> dict[str,Any]:
    docs={arm:json.loads((out/"runtime-manifest.json").read_text()) for arm,out in RUNS.items()}
    baseline=docs["off"]
    for arm,doc in docs.items():
        if doc.get("file_hashes")!=baseline.get("file_hashes") or doc.get("asset_symlinks")!=baseline.get("asset_symlinks"):
            raise ValueError(f"{arm} runtime file/symlink manifest differs from baseline")
    return {"passed":True,"runtime_file_hash_rows":len(baseline["file_hashes"]),"runtime_symlink_rows":len(baseline["asset_symlinks"]),
        "same_user_ltx_save_and_vfs_assets":True}

def e01(path: Path, first: int, last: int) -> dict[str,Any]:
    rows=[r for r in BASE.read_csv(path) if first<=int(r["engine_frame"])<=last and r.get("status")=="completed"]
    if not rows: raise ValueError("no completed E01 rows in measured frame window")
    def d(key: str) -> dict[str,Any]:
        vals=[float(r[key]) for r in rows]
        return {"count":len(vals),"mean":statistics.mean(vals),"p50":BASE.nearest(vals,.50),"p99":BASE.nearest(vals,.99)}
    gpu=[(float(r["gpu_end_s"])-float(r["gpu_start_s"]))*1000 for r in rows if float(r["gpu_end_s"])>float(r["gpu_start_s"])]
    return {"rows":len(rows),"cpu_ms":{k:d(k) for k in ("scene_ms","encode_ms","drawable_ms","commit_ms")},
            "work":{k:d(k) for k in ("indexed_draws","updated_bytes","buffer_allocations","texture_allocations","pso_binds")},
            "gpu_elapsed_ms":{"count":len(gpu),"mean":statistics.mean(gpu),"p50":BASE.nearest(gpu,.50),"p99":BASE.nearest(gpu,.99)} if gpu else None}
def run(arm: str) -> dict[str,Any]:
    out=RUNS[arm]
    required=("exit-status.txt","argv.txt","environment.txt","shared-state-sha256.txt","matched_profile.lua","phases.csv","phases.csv.device.csv","phases.csv.weather.csv","metal-timeline.json","e01-frames.csv","runtime-manifest.json","thermal-before.txt","thermal-after.txt","power-before.txt","power-after.txt","swap-before.txt","swap-after.txt")
    missing=[name for name in required if not (out/name).is_file()]
    if missing: raise ValueError(f"{arm} missing receipts {missing}")
    if (out/"exit-status.txt").read_text().strip()!="0": raise ValueError(f"{arm} game did not exit 0")
    if sha(out/"matched_profile.lua")!=EXPECTED["profile_sha256"]: raise ValueError(f"{arm} profile mismatch")
    if read_shared(out/"shared-state-sha256.txt","/appdata/user.ltx")!=EXPECTED["user_ltx_sha256"] or read_shared(out/"shared-state-sha256.txt","/savedgames/rescue-cordon-control.scop")!=EXPECTED["save_sha256"]: raise ValueError(f"{arm} user/save mismatch")
    argv=(out/"argv.txt").read_text().splitlines()[0]
    exe=STAGE/"candidate-msc-borrowed-alias-01/Anomaly.app/Contents/MacOS/AnomalyDX11"
    if Path(argv).resolve()!=exe.resolve(): raise ValueError(f"{arm} executable path mismatch")
    ev=read_env(out/"environment.txt")
    want="1" if arm=="on" else "0"
    if ev.get("XRAY_METAL_E08_BORROW_MSC_TYPES")!=want: raise ValueError(f"{arm} flag mismatch")
    if ev.get("XRAY_METAL_R1_STATIC_POLICY")!="0" or ev.get("METAL_PERFORMANCE_PER_FRAME_SIGNPOSTS")!="1": raise ValueError(f"{arm} common renderer/E01 settings mismatch")
    marks=BASE.read_markers(out/"phases.csv")
    for name in ("measure_start","measure_end","weather_committed"):
        if name not in marks: raise ValueError(f"{arm} missing marker {name}")
    start,end=int(marks["measure_start"]["epoch_s"]),int(marks["measure_end"]["epoch_s"])
    first,last=int(marks["measure_start"]["frame"]),int(marks["measure_end"]["frame"])
    if end-start!=20: raise ValueError(f"{arm} is not a 20-second marker window")
    if marks["weather_committed"].get("weather")!="w_clear2" or marks["weather_committed"].get("drawable")!="1920x1200": raise ValueError(f"{arm} commit marker weather/extent mismatch")
    allrows=[r for r in BASE.read_csv(out/"phases.csv.device.csv") if r.get("phase")=="1"]
    gate=BASE.state_gate(allrows)
    if not gate["passed"]: raise ValueError(f"{arm} device rows failed state gate: {gate['errors'][:8]}")
    measured=[r for r in allrows if first<=int(r["engine_frame"])<=last]
    timeline=BASE.timeline_stats(out/"metal-timeline.json",start,end)
    callback=BASE.callback_stats(measured)
    work=e01(out/"e01-frames.csv",first,last)
    receipt={name:hashlib.sha256((out/name).read_bytes()).hexdigest() for name in required}
    health={name:(out/name).read_text(errors="strict").strip() for name in ("thermal-before.txt","thermal-after.txt","power-before.txt","power-after.txt","swap-before.txt","swap-after.txt")}
    health_receipt={name:sha(out/name) for name in health}
    if any("No thermal warning level has been recorded" not in health[k] or "No performance warning level has been recorded" not in health[k] for k in ("thermal-before.txt","thermal-after.txt")):
        raise ValueError(f"{arm} thermal/performance warning was recorded")
    if any("AC Power" not in health[k] for k in ("power-before.txt","power-after.txt")): raise ValueError(f"{arm} was not on AC throughout")
    weather=[r for r in BASE.read_csv(out/"phases.csv.weather.csv") if r.get("marker")=="weather_committed"]
    if len(weather)!=1: raise ValueError(f"{arm} expected one committed weather row")
    return {"arm":arm,"window_epoch_s":[start,end],"frame_markers":[first,last],"state_rows":gate["rows"],"measured_callback_rows":len(measured),
            "timeline":timeline,"callback":callback,"e01":work,"weather_commit":{k:v for k,v in weather[0].items() if k not in ("marker","epoch_s")},"health":health,"health_sha256":health_receipt,"input_sha256":receipt}
def report(data: dict[str,Any]) -> str:
    off,on=data["runs"]["off"],data["runs"]["on"]
    lines=["# Borrowed MSC typed-alias pair: ROI review","",
      "Both arms ran the same candidate executable and controlled scene. The sole environment difference was `XRAY_METAL_E08_BORROW_MSC_TYPES=0` versus `1`. The candidate changes local typed aliases in `BindMscResources`; it keeps owners, descriptors, arena snapshots, GPU addresses, and indirect-read declarations unchanged.","",
      "| Metric | Flag off | Flag on |","|---|---:|---:|"]
    def add(name, f): lines.append(f"| {name} | {f(off)} | {f(on)} |")
    add("On-Glass throughput (N / summed drawable intervals)",lambda r:f"{r['timeline']['interval_throughput_fps']:.2f} FPS")
    add("On-Glass 1% low",lambda r:f"{r['timeline']['one_percent_low_fps']:.2f} FPS")
    add("Callback throughput",lambda r:f"{r['callback']['callback_throughput_fps']:.2f} FPS")
    add("Callback 1% low",lambda r:f"{r['callback']['one_percent_low_fps']:.2f} FPS")
    add("Callback interval p50 / p99",lambda r:f"{r['callback']['frame_interval_ms']['p50_nearest_rank']:.2f} / {r['callback']['frame_interval_ms']['p99_nearest_rank']:.2f} ms")
    add("E01 scene CPU p50 / encode p50",lambda r:f"{r['e01']['cpu_ms']['scene_ms']['p50']:.3f} / {r['e01']['cpu_ms']['encode_ms']['p50']:.3f} ms")
    add("E01 draws p50 / updated bytes p50",lambda r:f"{r['e01']['work']['indexed_draws']['p50']:.0f} / {r['e01']['work']['updated_bytes']['p50']:.0f}")
    add("GPU elapsed p50 / p99",lambda r:f"{r['timeline']['on_gpu_elapsed_ms']['p50_nearest_rank']:.3f} / {r['timeline']['on_gpu_elapsed_ms']['p99_nearest_rank']:.3f} ms")
    add("In-flight CPU p50 / p99",lambda r:f"{r['timeline']['inflight_cpu_end_to_end_ms']['p50_nearest_rank']:.3f} / {r['timeline']['inflight_cpu_end_to_end_ms']['p99_nearest_rank']:.3f} ms")
    lines += ["",f"The pair collected {off['timeline']['event_count']} versus {on['timeline']['event_count']} On-Glass intervals. GPU elapsed and in-flight CPU overlap pipeline work and are not additive; E01 phase values are separate diagnostics.","",
      "The result is a small, single 20-second pair: a roughly 1% On-Glass difference does not establish a repeatable gain. AI-driven draw counts differ slightly, so the actual work was not identical. The build's GPU fixture probe did run with the candidate flag set to 1 and passed the flora-instancing fixture; that does not measure or prove the lifetime behavior of the live game scene. No shipping/FPS acceptance follows from this pair.",""]
    dedup=data["runs"]["dedup"]
    lines += ["", "## Separate resource-dedup diagnostic", "",
      "This is an independent comparison against the alias-off baseline above. Borrowed aliases remain off in both arms; only `XRAY_METAL_E08_DEDUP_RESOURCES` changes from its default-off absence to `1`. Do not combine these values with the alias-on pair or attribute the alias-on results to dedup.", "",
      "| Metric | Dedup default off | Dedup enabled |", "|---|---:|---:|"]
    def pairline(name: str, metric: str, path: tuple[str,...], fmt: str):
        a,b=data["runs"]["off"],dedup
        for part in path: a=a[part]; b=b[part]
        lines.append(f"| {name} | {fmt(a)} | {fmt(b)} |")
    pairline("On-Glass throughput", "", ("timeline","interval_throughput_fps"), lambda x:f"{x:.2f} FPS")
    pairline("On-Glass 1% low", "", ("timeline","one_percent_low_fps"), lambda x:f"{x:.2f} FPS")
    pairline("Callback throughput", "", ("callback","callback_throughput_fps"), lambda x:f"{x:.2f} FPS")
    pairline("Callback 1% low", "", ("callback","one_percent_low_fps"), lambda x:f"{x:.2f} FPS")
    pairline("Scene CPU p50 / encode CPU p50", "", ("e01","cpu_ms"), lambda x:f"{x['scene_ms']['p50']:.3f} / {x['encode_ms']['p50']:.3f} ms")
    pairline("Indexed draws p50 / updated bytes p50", "", ("e01","work"), lambda x:f"{x['indexed_draws']['p50']:.0f} / {x['updated_bytes']['p50']:.0f}")
    pairline("GPU elapsed p50", "", ("timeline","on_gpu_elapsed_ms"), lambda x:f"{x['p50_nearest_rank']:.3f} ms")
    lines += ["", "This second single 20-second screen shows a larger throughput difference, but its measured draw count is substantially lower with dedup enabled. That scene-work difference prevents attributing the result to resource deduplication; it is a hypothesis signal only, not an ROI or FPS acceptance.", ""]
    return "\n".join(lines)
def main() -> None:
    bg=build_gate(); eg=env_gate(); deg=dedup_env_gate(); manifest=runtime_manifest_gate()
    off,on,dedup=run("off"),run("on"),run("dedup")
    if off["weather_commit"]!=on["weather_commit"] or off["weather_commit"]!=dedup["weather_commit"]: raise ValueError("committed weather state differs")
    summary={"schema":"msc-borrowed-alias-roi-review/v2","build_gate":bg,"environment_delta_gate":eg,
             "dedup_environment_delta_gate":deg,"runtime_manifest_gate":manifest,
             "shared_pins":EXPECTED,"scope":"two independent 20-second diagnostics: borrowed alias off/on; resource dedup off/on with borrowed alias off","runs":{"off":off,"on":on,"dedup":dedup},
             "limits":["one short pair per hypothesis; no repeatability or FPS acceptance","AI-driven draw counts vary, especially in the dedup pair","GPU fixture probe is not a live-frame lifetime proof"]}
    sp=ANALYSIS/"summary.json"; rp=ANALYSIS/"RESULT.md"
    sp.write_text(json.dumps(summary,indent=2)+"\n"); rp.write_text(report(summary))
    inputs=[Path(__file__),BASE_PATH,HYP/"root-build-01/result.json",HYP/"root-build-01/msc-binding-gpu-probe.log",HYP/"build_candidate.py",STAGE/"candidate-msc-borrowed-alias-01/source-manifest.json"]
    for out in RUNS.values(): inputs += [out/n for n in ("argv.txt","environment.txt","exit-status.txt","phases.csv","phases.csv.device.csv","phases.csv.weather.csv","metal-timeline.json","e01-frames.csv","matched_profile.lua","shared-state-sha256.txt","runtime-manifest.json","thermal-before.txt","thermal-after.txt","power-before.txt","power-after.txt","swap-before.txt","swap-after.txt")]
    inputs += [sp,rp]
    (ANALYSIS/"sha256.json").write_text(json.dumps({str(p.relative_to(ROOT)):sha(p) for p in inputs},indent=2)+"\n")
    print(json.dumps({"status":"complete","on_glass_off_fps":off["timeline"]["interval_throughput_fps"],"on_glass_on_fps":on["timeline"]["interval_throughput_fps"],"dedup_on_glass_fps":dedup["timeline"]["interval_throughput_fps"],"written":[str(sp),str(rp)]},indent=2))
if __name__=="__main__":
    try: main()
    except (OSError,ValueError,KeyError,json.JSONDecodeError) as e:
        print(f"invalid or incomplete receipts: {e}",file=sys.stderr); raise SystemExit(2)
