# Source topology census draft review

## Important lifecycle finding

The live Metal loader does **not** populate the legacy R4 `CRender::Sectors` or `CRender::Visuals` arrays. In `src/Layers/xrRenderPC_R4/r4_loader.cpp:35-59`, the Metal branch loads lights, optionally loads the diagnostic HOM asset, calls `MetalRenderDeviceRender::LoadLevelScene`, sets `b_loaded`, and returns. The ordinary `LoadVisuals` and `LoadSectors` calls are later in the non-Metal path at lines 115-131. Bounded search found no other `xrRenderMetal` code that adds to those R4 arrays; `LoadLevelScene` prepares `MetalR4EngineScene`'s independent CPU scene.

Therefore this census is expected to emit `status:"unknown"` with `source_sector_visual_graph_unavailable`, zero sector/visual counts, no start-sector ordinal, and no visible-sector rows on the current Metal path. It does not bootstrap metadata or reinterpret the independent Metal scene as a source graph. A future CPU-only topology loader would be a separate change/hypothesis and is deliberately out of this draft.

## Draft behavior and compile limitation

- Starts from the exact prestate copy `prestate-R_calculate.cpp` (SHA-256 `07d5190518b1401c337b342a22ed130560b87f12a25a969619514e3d950b9d46`). The separate draft file differs only by the observer helper and its call immediately after the existing `GMBase.traverse`.
- Strictly opt-in: `XRAY_METAL_SOURCE_TOPOLOGY_CENSUS=1`; one attempt only, on `Device.dwFrame == 700`. Output requires an absolute `XRAY_METAL_SOURCE_TOPOLOGY_FILE` path. Missing path or write failure logs `status=unknown`.
- Read-only. It takes `GMBase.S_LC`'s shared lock while copying the existing map, then serializes JSON after releasing it. It records frame, extent, FOV, eye, view/projection/full matrices, detected start-sector ordinal, `i_options` and HOM bit, requested renderer values, visible sector/root indices, frustum counts, masks, and the current CFrustum plane coefficients. Floats use `max_digits10` round-trip formatting; nonfinite inputs become JSON `null` and force unknown status.
- Known status is further gated on the exact target level name/counts, fixed eye bits, FOV bits, 1920×1200 extent, and bitwise agreement between the current Device eye/full transform and `GMBase`'s last traversal inputs. Expected archive SHA-256 values are included as provenance references, while `runtime_file_hashes_verified` remains explicitly false. A downstream consumer must compare actual active file receipts and reject mismatches before joining this record to frame-700 IDs.
- No output is consumed by draw ordering, culling, admission, or suppression. The JSON/log explicitly keeps `suppression=0`.

This remains an **unbuilt prototype**, not a compile-ready patch: `WriteMetalSourceTopologyCensus` is a free namespace helper but currently refers to `Sectors`, `Visuals`, `GMBase`, and `pLastSector` as if it were a `CRender` member. Those references need explicit owner access or relocation into the member function. Given the confirmed empty legacy graph on current Metal, the full per-frustum serializer is also unnecessary before any separate CPU topology bootstrap; prefer the smaller one-frame state observer owned by the root effort.

## Source-only validation

Reviewed the generated unified patch and verified the production working-tree paths were not edited by this draft. No build, game run, or shader compilation was performed, as requested. The artifact is a candidate scratch patch only; its C++ compilation and runtime behavior remain unverified.

Artifacts: `candidate.patch`, `draft/src/Layers/xrRender/R_calculate.cpp`, and `prestate-R_calculate.cpp`.
