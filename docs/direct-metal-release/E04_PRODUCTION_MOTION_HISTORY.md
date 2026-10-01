# Production static motion and AO/IL history (2026-09-27)

Packet 2 of `E02_E06_AUDIT_2026-09-27.md`. Evidence root:
`scratch/release_runs/20260927-static-motion-history/`.

## Production repair

LMH and terrain now bind `previous VP * static model` at `m_wvp_prev` and
`previous VP` at `m_vp_prev`. DefVertex and DefObjectsLod receive the same
scene-owned previous camera. Current transforms remain current. This matches
the converted flat/AREF/bump/steep shader layouts; no HLSL or MSC bundle was
changed. AO/IL consume the existing motion MRT; their commented matrix
reprojection is not used as a substitute for a correct producer.

`R4SubmittedViewHistory` selects the camera from the last accepted primary-world
submission. Staging, commit and discard are separate. Acquisition retries and
aborted encodes do not advance the camera, object transforms or AO/IL metadata.
A second primary world render in the same submission is rejected before any
encoding. First frame, explicit cut, >50 m displacement, >60 degree direction
change, projection change, engine-frame rewind, scene reload and extent changes
start with current-as-previous. API reset and asset changes invalidate history. The native-retina resize path
now obtains backing pixels from the NSView rather than feeding the configured
SDL layer drawable size back into the next resize.
Asynchronous GPU failure still follows the existing device-lost shutdown path.

Private resource uploads also follow the submission transaction. Level, terrain,
DefVertex and DefObjectsLod keep their CPU sources until submission; the backend
retains immutable staging and replays private uploads from a discarded command
buffer before the next render. Render passes and history copies are not replayed.
`XRAY_METAL_TEMPORAL_COLD_UPLOAD_PROBE=1` aborts the first scene encode to exercise
cold assets, rather than only already populated targets.

G-buffer/depth targets are resized before AO/IL targets. AO/IL reuse requires
that their own submitted source frame match the selected previous camera frame;
changes to either viewport scale clear both histories and previous position.
A failed partial scene encode is discarded instead of being submitted as a
successful temporal producer. Shutdown also discards an unfinished frame.

## GPU evidence method

`XRAY_METAL_MOTION_PROBE_DIR` enables isolated duplicate G-buffer passes for
LMH, terrain, DefVertex and DefObjectsLod. They use actual level geometry,
materials, production draw functions and existing converted shader pipelines.
The production MRTs and temporal histories are not substituted by these passes.
Native readback copies D32 raster depth and RGBA16F velocity after GPU completion.
An independent double-precision inverse VP reconstructs world positions at pixel
centres, projects them through the previous VP, and compares UV velocity in
pixels. Gates: <=0.5 px for motion and <=0.01 px after reset. Empty family
coverage is `NO_COVERAGE`, never an acceptance pass. Each family requires both
moving and reset samples with actual rasterized pixels.

Actual AO/IL readbacks witness zero RGBA immediately after history clearing,
exact current-position -> previous-position copies after the chain, and finite
AO/IL outputs, including nonzero AO occlusion in alpha and IL color in RGB. These checks establish data ownership and motion input
correctness; they do not establish same-fixture Windows image parity.

`XRAY_METAL_TEMPORAL_LIFECYCLE_PROBE=1` is a diagnostic-only sequence: skip one
acquisition, unload/reload real scene producers, discard an encoded frame,
request a second world view, halve the render extent and restore it. It restores
the inherited native-retina setting. These switches are opt-in and alter the
fixture workload, so runs are diagnostic, not E00/performance admission.

## NPC boundary

World object transforms are staged until submission and matched against the
actual previous producer frame. Motion identity includes asset content/name and
object ID; anonymous `0xffff` IDs do not borrow history by snapshot ordinal.
A missing object history still retains camera motion using the current object
transform. The value ABI does not carry a spawn generation, so immediate
same-ID/same-model reuse remains a separate identity limitation. A same-name
asset replacement with a missing/zero content hash also cannot prove a new
identity. These cases are not reported as verified NPC identity parity.

Original SKIN_1–4 shaders use previous bone matrices. The current world CPU
skinning path emits SKIN_0 from current bones; SKIN_0 uses `P_prev = P`.
Animated-bone motion therefore remains open. This repair does not claim full
NPC motion parity or completion of all E04/E06 visual gates.

## Verification results

- CPU `SubmittedViewHistory.SelfTest`: **1/1 PASS**.
- Final main build: **PASS** (`build-final.log`), binary
  `9c2bd697b4d8fc8e3cf608c389b61c22337e54b9b2548464f0001a1b92613d24`.
  CPU gate includes accepted/discarded non-finite camera recovery (`final-cpu.log`).
- Standalone Bootstrap source compiled, but its executable link is **BLOCKED**
  by unresolved engine filesystem/memory/Theora dependencies
  (`build-bootstrap-final.log`). The added local commit/discard hooks introduce
  no external symbol dependency. This auxiliary target is not the production
  main executable used for GPU verification.
- `20260927-motion-history-lifecycle2`: **exit 0**, API/shader validation
  enabled, no Metal validation errors found. `lifecycle-summary.json`: **PASS**.
  44 real converted-shader motion readbacks (11 per family), 6 RGBA-zero
  clears, 12 byte-exact position copies, 12 finite AO/IL output pairs.
  Motion error <=0.489 px; reset error <1e-8 px.
- Acquisition Retry at frame 31 released the acquired slot semaphore; producer
  32 uses frame 30. Encoded frame 41 was discarded without completed readbacks;
  producer 42 uses frame 40. Real scene reload at 36 resets history. A second
  primary view at 46 is rejected before encoding. Frames 51 and 56 verify
  1920x1080 -> 960x540 -> 1920x1080 target allocation and current-as-previous.
- Earlier ordinary and slow S1 runs exited 0, but their moving readbacks were
  taken during the engine's 60-frame precache rotation. They establish shader
  projection correctness, not the commanded camera pan/turn. The verifier now
  supports `--moving-after-frame` and rejects their post-precache motion
  coverage (`precache-only-summary.json`: NO_COVERAGE).
- The 90 s warmup S1 run also exited 0 but had zero post-precache motion.
  A subsequent unpaused diagnostic measured `paused=false` while the Device
  position/direction stayed constant. These runs are not camera-pan acceptance.
- `20260927-motion-history-cold-camera`: **exit 0**, API/shader validation,
  `cold-lifecycle-summary.json`: **PASS**. Cold scene encode 9 was discarded;
  the next acquisition replayed 6330 private upload operations. Warm discard
  41 replayed 164 operations; camera/AO source stayed at 40 until producer 42.
  Reload/multiple-view/half-size/restore gates passed on binary
  `b12878e54e9f916f5a2f5a458115779b46ea2686815fe4a5d8a390abdedec83d`.
  48 family readbacks, 6 zero clears, 13 exact position copies and 13 finite
  AO/IL output pairs pass (max 0.489 px). This run does not establish scripted
  post-precache motion.

### Final combined GPU run

`20260927-motion-history-final`: **exit 0 / PASS**, API and shader validation
on, no Metal validation errors found. Final binary is the `9c2bd697...` build
above. `final-summary.json` uses `--moving-after-frame 100`, so precache
rotation cannot satisfy moving coverage. `runtime-final-summary.json` records
actual Device camera poses and the native witnesses.

| Witness | Result |
|---|---|
| Turn, frame 100 (8.824 s) | All four families nonzero; camera position unchanged, direction changed; max error 0.127 px |
| Pure pan, frame 120 (32.162 s) | All four families nonzero; 1.402199 m observed offset at fixed turned direction; max error 0.036 px |
| 120 degree yaw cut, frame 140 | Actual camera direction changes by 99.868 degrees in 3D (pitched camera); all four families current-as-previous, zero velocity; AO/IL cleared |
| Cut return, frame 142 | Base camera restored; zero velocity/reset for all four families; AO/IL cleared |
| Source ownership | Frames 100/120 reuse AO/IL sources 99/119, exactly matching camera history |
| Cold abort | Encode 9 discarded; next acquisition replays 6330 resource uploads |
| Warm abort/acquire gap | Retry 31 and discard 41 preserve sources 30/40; 173 uploads replayed after warm abort |
| Reload / second view / resize | Reload 36 resets; second view 46 rejected before encoding; 51/56 allocate 960x540/1920x1080 |

60 converted-shader family readbacks pass; 9 full RGBA-zero history clears,
16 byte-exact position copies and 16 finite AO/IL output pairs pass. Motion
max error including precache/gaps is 0.489 px, reset <1e-8 px. Native
`final-ao-il.png` and sampled video frames were inspected; they show active
AO/IL at the actual camera poses. This is producer/history acceptance, not
same-fixture Windows visual parity or a performance result.

The original fixture driver froze Device time with `freeze_time 1`.
`CCC_FreezeTime` sets the Device factor to zero; `CObjectList::Update` skips
object/actor camera updates when `Device.fTimeDelta <= EPS_S`. This explains
why successful Lua setter calls produced no camera movement after precache.
The first-eye-only diagnostic was intentionally stopped before motion; its
termination is not an acceptance run or independent renderer failure. A new
diagnostic unfreezes Device time during S1, retains paused weather/game time,
records requested and observed camera poses, and restores the inherited Device
freeze on completion. It also exercises a 120 degree yaw cut and return.
The common S1 scenario now snapshots/restores the Device time factor and
unfreezes it during motion; the harness waits for precache to finish before
starting capture. Lua syntax checks pass for both scripts. Historical S1
Lua-success/video records must not be treated as proof of commanded camera
motion without observing the Device camera. Weather/game time remains frozen
independently. Other scenario drivers were not revalidated here.

Earlier diagnostics are retained: `20260927-motion-history-validation` exited
0 and passed all 28 motion readbacks, but the initial AO witness incorrectly
required nonzero RGB (AO occlusion is alpha), and the long reload meant that
not all injected lifecycle events ran. `20260927-motion-history-lifecycle`
exposed the real native-retina resize feedback bug and failed the capture gate
while stuck at 960x540. Neither is the final acceptance run.
