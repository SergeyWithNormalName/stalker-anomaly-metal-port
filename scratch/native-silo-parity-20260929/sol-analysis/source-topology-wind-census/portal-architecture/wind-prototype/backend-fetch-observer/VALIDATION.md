# Draft review and receipt contract

## Source contract

- Backend prestate SHA-256: `365115ea87703d4038a45b95e9a8decc4d0769f2b091159e253930c36892bebc`; the saved backup matches byte-for-byte. The working production source was rechecked and still has this hash.
- Flora adapter source SHA-256: `3cd57e6f84069698619358337d660bd6e159357ed455c3769ee5b584c47d58dc`.
- The main G-buffer adapter declares the exact leaf/trunk labels and shader keys, attribute 11 as Float4 from buffer 6 at offset 0, and a 72-byte per-vertex layout. Repacking writes position W as exact float `1.0f`.
- The MSC vertex-pipeline reference specifies the reserved stage-in attribute range beginning at 11 and the first vertex-buffer binding at 6. The draft records the exact backend descriptor and actual selected native pipeline label/identity.
- `SetE01FrameTiming` normally stores the engine frame only while E01 timing is enabled. The draft stores that frame number for this observer only when the observer environment flag is active, so its filter can remain at frame 700 without enabling E01 timing.
- `MetalBuffer` exposes native version handles, storage mode, usage, and frame-upload state. `BindVertexBuffer` already retains the buffer version through the frame. The draft snapshots the native handle and version at `BindVertexBuffer` and binds that exact pointer; this remains the descriptor actually selected even if a later Shared update performs copy-on-write. It takes the index native handle/version before and uses those exact values in the draw call. It keeps only the diagnostic references in frame state and clears them at receipt/frame abort. Main Flora vertex/index buffers are Shared and static; any buffer that does not prove Shared, non-frame-upload, Vertex/Index usage, and CPU-readable contents is unknown.

## Capture boundary

Observer activation requires all of:

1. `XRAY_METAL_FLORA_FETCH_OBSERVER=1` and a nonempty `XRAY_METAL_FLORA_FETCH_FILE` path.
2. Engine frame 700 and pass label `R4 engine scene G-buffer`.
3. A selected MSC PSO whose label and shader key exactly match the main leaf or trunk pipeline.
4. The selected backend vertex descriptor contains position attribute 11 / Float4 / buffer 6 / offset 0 and a buffer-6 stride of 72 bytes.

For each matching indexed draw, the receipt records PSO uid, actual native PSO label, pipeline key, vertex metallib path, backend descriptor match, vertex native/version/resource identities and SHA-256 of the full logical `IR4Buffer::Size()` range (not native allocation padding), bound offset/size, index native/version/resource identities, actual index-range SHA-256 from the exact native index buffer/version passed to the draw, index offset/count/type, base vertex, instance count, checked index count, and W-bit counts. W is checked after adding signed `baseVertex` and applying the same 72-byte stride and P4 byte-12 offset represented by the descriptor.

The draw contract does not expose visual IDs. Join rows to the captured visual inventory by their per-run buffer IDs, content hashes, and index tuple. If multiple visual rows match the same content, or a row has no match, report that join as ambiguous/unknown. Process-local pointer values are not stable across runs; the content hashes bind the receipt to the bytes observed in this process.

If a diagnostic pass split interrupts G-buffer capture, the emitted file is marked `pass_complete=0`; it must not be used as a complete frame inventory. If the observer cannot open/write the receipt, it logs an unavailable reason and does not fail or alter the draw. It consumes at most one attempt for the process after a receipt attempt.

## Review limits

This draft was source-reviewed only. It has not been compiled or run. Reading Shared `MTLBuffer.contents` provides a CPU-side observation of the bound bytes; this does not prove GPU execution, MSC semantics beyond the captured descriptor/reflection linkage, transform rounding, visual identity join, culling correctness, temporal stability, or FPS. No primary source, executable, game state, or shader output was changed.

The draft follows the `integrating-metal-shaderconverter-shaders` skill reference `references/vertex-pipelines.md` for the stage-in attribute and vertex-buffer index contract.
