# Main Flora pixel-pair draft

This scratch-only draft adds one opt-in main-G-buffer OFF/OFF infrastructure control. It is not a portal culler, fixed-ID filter, shader change, or shipping feature. The active source files are copied under `draft/src/Layers/xrRenderMetal/`; `main-flora-pixel-pair.patch` is the reviewable patch against the pinned original hashes in `draft-source-hashes.json`.

The diagnostic is default-off. It requires both `XRAY_METAL_FLORA_MAIN_PIXEL_PAIR_DIR=<scratch output>` and `XRAY_METAL_FLORA_MAIN_PIXEL_PAIR_FRAME=<exact engine frame>`. On that frame it snapshots all four pre-Flora MRTs plus D32S8 into OFF targets, copies those into ON targets, verifies the original-to-OFF base copy, draws the same adapter twice with one frozen `R4FloraViewState`, and validates OFF/ON control equality plus nonblank position/depth-stencil change. It then resumes the original production G-buffer with Load actions and invokes the preexisting bundled world callback unchanged exactly once.

Color comparisons use the existing raw-byte `ValidateTemporalTextures` pair oracle and write PPMs. The only new backend contract is an exact D32S8 pair readback/comparison; it uses the same depth and stencil blit aspects as the existing visibility probe. Result files are asynchronous completion receipts, so a root-run frame must complete and reports must be inspected. The diagnostic adds two main-Flora replays/uploads on that selected frame.

The ON branch is intentionally identical to OFF for this first control. A future test may supply distinct admission behavior only after root review; no route or ID list is wired here. Production draws remain in original order and still run once after the isolated passes. Adapter state checked in source shows main draw only updates telemetry; production drawing resets it.

No build or game run was done by this agent. Root owns compile/runtime review.
