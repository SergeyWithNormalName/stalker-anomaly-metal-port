# Main Flora SSA numerical upper-bound screen — scratch draft

This is a deliberately **non-shipping performance oracle** for the fixed
frame-700 camera. It skips the 1,199 actual main Flora visual IDs that the
offline OGF SSA join classified as numerical hard/fade rejects. It does not
prove the Windows visual/shader exception contract: `eIgnoreOptimization` and
selected shader E[4] `bEmissive` remain unverified. A faster ON arm would show
only the maximum practical ROI worth investigating; it would not establish
quality parity, safe culling, GPU time saved, or a deployable feature.

## Frozen inputs

- Source file: `src/Layers/xrRenderMetal/MetalR4FloraAdapter.cpp`, prestate
  SHA-256 `3cd57e6f84069698619358337d660bd6e159357ed455c3769ee5b584c47d58dc`.
- Patch: `main-flora-ssa-upper-bound.patch`, SHA-256
  `035c4479580a601d91556b9421ff403b7010752d7a0015e3fd0ef52a07f3e769`.
- ID file: `../ssa-main-ids/frame-700-upper-bound-ids.csv`, SHA-256
  `fcdc309f52b6594502c6dcbe8be74ed997f5b608dd79f24442ed2102c610ca72`.
  It has 1,199 sorted unique ID/material rows: 885 trunk, 314 leaf. The
  generator is the optional `--upper-bound-file` output of
  `../ssa-main-ids/join_main_flora_ssa.py` and pins the winning level SHA,
  source observer CSV SHA, and frame 700 in its header.

The patch changes one CPP file only. Production source was **not** edited by
this draft, and no build or game run was performed here. `git apply --check
--whitespace=error` passed against the pinned source.

## Runtime boundary

OFF is the candidate binary with `XRAY_METAL_MAIN_FLORA_SSA_UPPER_BOUND` unset
or anything other than exactly `1`. It never opens the ID file; every
previously admitted Flora draw reaches the same upload/bind/DrawIndexed path.
The sole added per-draw branch evaluates false.

ON requires `XRAY_METAL_MAIN_FLORA_SSA_UPPER_BOUND=1` and
`XRAY_METAL_MAIN_FLORA_SSA_IDS_FILE=<absolute path to pinned CSV>`. The
launcher must verify the full ID-file SHA-256 above before launch; the runtime
parser is intentionally small and does **not** compute SHA-256 itself. At
`Prepare`, it reads at most 64 KiB once, checks exact schema/provenance/count,
1,199 strictly ascending unique numeric IDs and leaf/trunk labels, the
11,663-draw source census, and every ID/material against actual prepared
draws. Any missing, malformed, duplicated or mismatched input disables all
skips and logs `admitted=0`; rendering continues unchanged. No partially
parsed list is applied.

Each main draw first passes the existing Metal clip test. Only then can the
fixed-camera candidate branch skip it, before per-draw upload/bind/draw.
The accepted view, projection and eye must match frame-700 float32 bits
exactly; moving, zooming, changing resolution/FOV, or any different view makes
`view_match=0` and disables all skips. Shadow-caster rendering never reads
the skip bit. An ON sample logs `view_match`, `skipped`, `submitted`, and
`clip_culled` every 300 Flora calls; there is no per-draw logging.

## Bounded screen owned by root

Use a same-binary OFF/ON fixed-camera pair after checking the ID SHA and source
hash. First require `admitted=1`, `view_match=1` in the measured window, and
`skipped=1199`; otherwise the ON arm is invalid. With the same scene, main
Flora should move from 5,316 to 4,117 draws and whole-frame indexed draws
should fall by about 1,199, subject to independent game workload changes.
Compare callback and displayed intervals and E01 work counters under the
matched save/weather/camera/profile, with timeline-gap validity. Do not use
the instrumented observer run as a performance baseline. Reject the direction
if the screen is neutral or worse. Even a positive direction requires a
separate fully qualified source SSA implementation with exemption and visual
proof; this patch must be removed after the ROI screen.
