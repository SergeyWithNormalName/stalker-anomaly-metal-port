# Compute skin integration — root review decisions

Native API and position guard passed central tests. The opt-in production adapter route was applied after root and independent review; the backend library and three focused CPU tests passed. V4 remains pinned; no new gameplay binary has been linked or promoted. Actual adapter transaction wiring and production output equality remain gates.

## Native jobs

The backend owns unique nonzero acquisition epochs, current exact arena-upload spans, full-batch count/span/alias validation, canonical embedded MSL and resource versions retained through GPU completion. Feature flag is strict `XRAY_METAL_COMPUTE_SKIN=1`; metadata tracking must not add vector appends to the default CPU path. No graphics encoder open; finish all pending private uploads before compute, end compute before consumers. Invalid second job must leave every output unwritten. Kernel arithmetic stays identical to the qualified standalone version. No production CPU wait/readback.

## Position guard

Keep the actual CPU position operation order, original source/world bounds, 2048 radius and max(64,32*expectedRadius) admission. Do NOT repeat normal/tangent/binormal transformations to qualify GPU skin. Bound them conservatively from maximum finite source basis component M, maximum absolute weight sum W and maximum bone linear row sum L, all evaluated in double. Both M*L and M*L*W must remain comfortably below sqrt(FLT_MAX/3); otherwise full CPU fallback. Signed residuals use absolute contributions. Metadata may later be hoisted into the immutable geometry cache. Any non-Ready result falls back, never introduces a new rejection.

## Geometry and current-pose validity

Canonical input is cached per immutable admitted geometry/content key, preserving XRFS winners. Do not permanently cache a failure caused solely by a particular renderable's short current bone palette. Decode independent of current palette length (e.g. maximum representable raw bone index capacity), store maximum referenced bone index, then validate actual current palette each frame. Source-intrinsic nonfinite basis/layout incompatibility can remain CPU fallback. Count memory for canonical CPU data plus GPU buffer; no hidden unbounded per-instance canonical copies. Pose hash and palette upload belong once per renderable, not once per leaf.

## Pending output transaction

Current CPU `hasSkinPose` is promoted during Prewarm because private uploads have replay journals. GPU output MUST NOT copy that shortcut: an abandoned command buffer never executed its skin kernel. Store pending GPU output stamps keyed by stable instance key, output wrapper, current unique Prewarm epoch and a logical output-write serial. On successful Submit only, promote reusable pose. On discard/retry clear pending stamps/jobs, retaining only genuinely committed prior content. New output allocations remain uncommitted until Submit.

The adapter exclusively mutates its skin output. Increment output-write serial on any CPU upload or output replacement; cancel both queued GPU write AND pending stamp on same-key CPU fallback/failure. At commit require expected wrapper, epoch and write serial. Backend still retains the exact native buffer version. If implementation permits any other writer after encoding, add an actual native-version token rather than assuming wrapper identity alone is enough.

Collect/prevalidate jobs before dispatch and dispatch once after all geometry preparation. Publish current-epoch skin readiness only after a successful GPU batch or completed CPU fallback preparation; whole Prewarm completed gate remains authoritative. On native batch failure, perform the original complete CPU path for every queued job before any drawing, or fail the entire frame clearly. Never allow a half-GPU/half-uninitialized frame. Uniform/allocation failures that erase instances must cancel their queued jobs. Preserve source bounds/sanity, source ordering, existing motion history and static shadow behavior.

After native batch rejection, latch the compute route off for the adapter lifetime and execute one complete CPU Prewarm retry. Per-geometry or palette admission fallback remains local. A later duplicate key reusing submitted pose A must cancel an earlier queued pose B. CommitSubmittedSkin is called for every successful scene submit before the temporal-history early return; the old object-motion commit remains behind that return. The supported scene contract is one public Prewarm per acquired command buffer, with the internal CPU retry allowed only after the no-compute-write failure boundary.

## Bounded actual-output oracle

The next diagnostic stage is independently opt-in via XRAY_METAL_COMPUTE_SKIN_ORACLE=1, together with compute enabled. An ordinary job has a null shared request pointer and performs no reference repack/readback. An oracle request owns expected bytes from the actual old CPU repack, a stable instance key, pose hash, output write serial, Prewarm epoch and engine frame. The backend attaches the actual submitted serial and canonical shader hash to its completion receipt.

Select at most one surviving request per acquired frame, at most 4 MiB, with two samples per available influence class in distinct successfully submitted frames. Do not block sampling all later classes on an absent earlier class. Only successful submission of the surviving selected job consumes quota; duplicate-key cancellation, batch rejection and discard do not. Absence or incomplete coverage stays explicit.

Preallocate Shared staging before compute. Encode the output copy after compute on the same command buffer and compare all 36 bytes per vertex exactly after completion. No CPU/GPU tolerance is introduced. Diagnostic failure after dispatch must never return the backend's no-write false result. Missing copies, abandoned work and receipt failures cannot count as PASS. Enforce the one-request/4-MiB bound across the whole frame context, clear abandoned ledgers, and retain immutable expected data and native resources through completion. No normal-frame wait or readback is permitted.

## Required adapter proof

Shared production state-machine tests for successful submit, discard/retry with unchanged pose, acquired-slot wrap, repeated engine frame, replaced output and same-key CPU fallback/cancellation. Runtime opt-in CPU/GPU output equality after completion with bounded readbacks, forced late-submit retry/cold upload and actual moving models. No timing claim from validation runs. Only then same-binary ordinary CPU/GPU A/B on unlocked Mac; locked-display callback measurements are conditional economics only.
