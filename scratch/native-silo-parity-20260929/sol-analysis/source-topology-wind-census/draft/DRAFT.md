# Scratch-only flora wind census draft

This draft copies `src/Layers/xrRenderMetal/MetalR4EngineScene.mm` at its assigned starting SHA-256 `c7c3b5c77e11e41b78095c867e25fe7568b886aa70c2d2fc3eb6ee1af3c1af53`. It adds one private helper header and two minimal scene-source changes: an include and a call immediately after `makeFloraView(callbackView, ...)`, before the existing `DrawIntoActiveGBuffer` call.

The observer is strictly default-off. It requires `XRAY_METAL_SOURCE_TOPOLOGY_CENSUS=1`, the main flora callback's `callbackView.engineFrame == 700`, and an absolute `XRAY_METAL_FLORA_WIND_FILE`. A static one-attempt guard prevents repeat output. It writes the exact `R4FloraViewState` float arrays and scalar inputs, plus duplicated callback view/frame provenance and source environment/motion frame numbers. It checks all serialized float inputs for finiteness and writes a compact `available:false` record on invalid state. It does not modify the view or rendering result.

The JSON records `wind_verified:false`, `suppression:0`, an explicitly unverified/null texture receipt hash, and render size as unavailable because this `ViewState` has no dimensions. The observer reads no GPU resources and invents no shader data. No static visual IDs are included; later correlation can use the same-frame view/projection/eye and recorded frame numbers.

Files:

- `src/Layers/xrRenderMetal/MetalR4EngineScene.mm` — exact starting copy plus include/call only.
- `src/Layers/xrRenderMetal/MetalFloraWindCensus.h` — self-contained one-shot JSON observer.
- `source-changes.patch` — the two-file scratch patch.

No app build, shader cook, or game launch was performed. Root owns validation, build, freeze, and restore decisions.
