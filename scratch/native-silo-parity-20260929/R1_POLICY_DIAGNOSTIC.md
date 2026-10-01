# R1 static-light source policy: initial diagnostic

Status: strong candidate, qualification and ordinary measurement pending. Not Windows-superiority acceptance.

Matched user setting `r2_allow_r1_lights off` disables entire static source packages before export in Windows `CLight_DB::add_light`. Native live registry capture omitted that source policy. The opt-in candidate adds the same predicate before any parent/face capture. Fog, frustum, HOM, distance/LOD, RGB and caster selection are unchanged.

One immutable binary `d86d011e5826c43596e91f4cafaec1300b6c6e119ae0b57d81bc9cffbb396b18`, manifest `e99f06e904f0edbed946c0ddf3eb1c993065e84146c2aa934bbd743de14f5aa1`, measured with `XRAY_METAL_R1_STATIC_POLICY=0/1`. E01/E07 and one-frame observer enabled; no CUA/CPU sample/build during either measurement. Same pinned save/user/shaders, 1920x1200, fixed camera, clear2 weather, AI active. These are short scene diagnostics; live simulation can vary between runs.

| Metric | Old source policy | Matching source policy |
|---|---:|---:|
| On-Glass FPS (valid contiguous trace) | 19.1339 | 24.9140 |
| Callback FPS | 19.1263 | 24.9139 |
| CPU encode p50 ms | 47.793 | 35.595 |
| Indexed draws p50 | 21320 | 14840 |
| Command-buffer GPU elapsed p50 ms | 37.892 | 31.950 |

Initial displayed gain: 30.21%. Nested CPU timings must not be summed; GPU elapsed is not hardware occupancy and overlaps CPU.

OFF observer completed measured frame360: 59 static parents expand to 354 captured records. Of 285 rendered shadow faces, 222 (37 parents) are static packages rejected by the Windows setting. Their observed work includes 856 static-geometry draws, 2161 flora-shadow draws and 964 dynamic caster proxies. Proxy categories are not a full draw accounting. Exact within-frame parent/face joins and completion are stored in `runs/r1-policy-off-01/native/output/r1-observer-summary.json`.

ON frame360 happens during warmup because it renders faster: 30 captured parents, 75 records and 63 rendered faces, all dynamic. This establishes exclusion at that frame only; it is NOT a measured same-frame A/B count comparison. Parent IDs are pointers and must not be compared directly across processes/ASLR.

Pending: game-switch ON positive control; visible full-window and toggle proof; ordinary same-binary reverse pair without E01/E07/observer; Windows playable comparable reference. Expected image changes with the switch OFF correct the old mismatch; equality to the old bug's output is not the acceptance criterion. Main/baked-lightmap production paths are untouched structurally, but visual acceptance remains separate.

Production app and bare executable remain the exact original `b0bcf586...`; primary user/mod data are unchanged. All candidates, runtime clones and reports are task scratch artifacts. The current source contains default-off policy and observer in two renderer files; no installation changes.
