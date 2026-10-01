# Independent review: fixed-ID portal ROI candidate

## Verdict

**Conditional pass for the isolated, root-harness-pinned fixed-camera upper-bound experiment. Not a production admission or temporally qualified cull.** The candidate is default-off, has several fail-open gates, checks the intended ID/material inventory, and skips only in the main Flora G-buffer draw path after the existing Metal clip test and before per-draw upload/binding work. Shadow draw code does not consult the fixture. Root's exact fixture SHA and active `level`/`level.geom` launch gates are required for the ID-set claim.

## Base and fixture evidence

- `MetalR4FloraAdapter.base.cpp` and the current production `src/Layers/xrRenderMetal/MetalR4FloraAdapter.cpp` both hash to `3cd57e6f84069698619358337d660bd6e159357ed455c3769ee5b584c47d58dc`. The candidate scratch copy hashes to `7f0544f479a8bc4cdad06845aba3c19097c3a17a0b39be8a4e30826aa6409fb4`; the production source remains the base, not the candidate.
- I independently parsed `portal-upper-bound-fixed-camera-4086.csv`: SHA-256 `35bce89dfa74883f8615d5cdf129327fbff515ede1af464bb6159b4630693c1d`, 4,086 unique IDs, 1,765 leaves and 2,321 trunks. Its IDs and material labels exactly equal the per-ID rows marked `excluded_by_all_nohom_owner_frusta=1` in the corrected frame-700 qualification CSV, and every ID/material pair exists in the 5,316-ID captured main-Flora inventory.
- The fixture's view, projection and eye bit rows match the saved frame-700 capture bit-for-bit. Camera admission in the candidate compares those same three arrays exactly, including signed zero.
- The adapter does not authenticate the ID file itself. For this experiment the root harness must enforce the exact fixture SHA above and the exact active `level`/`level.geom` hashes before every ON launch. Without those external pins, schema/count/camera/inventory checks alone do not prove that the file is the 4,086-row geometry candidate set.

## Runtime gates and skip placement

The fixture is considered only when `XRAY_METAL_FLORA_PORTAL_ROI` is exactly `1` and `XRAY_METAL_FLORA_PORTAL_ID_FILE` is an absolute path. Invalid path, bad header/count, malformed/non-finite camera bits, invalid/duplicate IDs, unknown materials, or read failure leave `portalRoiCandidate` false. On valid parse, `Prepare` joins each fixture ID to `m_impl->draws`; missing IDs, duplicate admitted IDs or a Leaf/Trunk material mismatch reject the entire fixture. `Prepare` creates a fresh `Impl`, so stale candidate state does not survive re-prepare.

At draw time the gate also requires a valid view, `view.windValid`, and bit-exact fixture view/projection/eye equality. Any mismatch disables the fixture for the whole draw call. In the loop, the existing `isOutsideMetalClipVolume` test executes first. Only a matched fixture ID that passed that test is skipped; the `continue` occurs before per-visual constant staging, `UploadFrameData`, resource binding, pipeline binding, vertex binding, or `DrawIndexed`. Other retained visuals continue in their original vector order and execute the same binding/upload path.

The shadow path rebuilds its candidate set from `m_impl->draws` and light matrices. It never reads `portalRoiFixture`, `portalRoiCandidate`, or the main-view camera gate. The patch makes no changes to shadow grouping, shadow selection/order, shader choice, resource bindings or shadow telemetry.

## Telemetry and qualification boundary

On the first matching draw call, the dedicated line reports the number of fixture IDs that passed the ordinary Metal clip test and were actually skipped, the other main Flora visuals that passed the clip test, fixture IDs already rejected by the clip test, and configured ID count. It labels the scope `fixed-frame-main-gbuffer-potential-upper-bound` and sets `wind_temporal_qualification=0`. These are separate from the existing renderer counters and make the fixed-ID effect observable; the log appears once per `Impl`, not once per matching frame. A partial draw that returns an upload/backend error before the loop completes emits no activation summary and must not be used as a completed count.

The runtime gate checks only the current camera bits and the `windValid` boolean. It does not compare frame number, previous view, time, current/previous bender arrays, or wind parameters against the captured frame. Thus an opt-in run on a later frame with the same camera can still skip these IDs after their wind state changes. That is intentionally outside this upper-bound qualification; the label is accurate, but the root harness must hold the intended frame/state for the experiment. No claim of safe repeated or shipping culling follows.

No independent build or game run was performed in this review; root owns the already-started isolated validation. Source review scope is the scratch candidate diff only. The production adapter was not edited.
