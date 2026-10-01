# Independent review

## Result

No remaining concrete correctness defect found in the observer after the six-file frame/SSA bridge was included. The initial one-file integration attempt did fail: its staged adapter used `R4FloraViewState.engineFrame/ssaObserver` without staging the header that defines them. `root-build-02/result.json` records a successful candidate build (`c55afacf…`) with those bridge files staged, followed by a successful normal rebuild and restoration of the pre-run ordinary app/bare binaries (`d3d93d…`). This is build proof only; no observer receipt/runtime evidence is established by that build.

## Contract checks

- Default-off activation requires the explicit observer flag and absolute receipt path. Capture is limited to engine frame 700, the exact G-buffer pass label, and exact leaf/trunk pipeline key+label. Shadow PSOs and other passes are excluded.
- `CreateGraphicsPipeline` validates duplicate/range constraints, writes each accepted `R4GraphicsPipelineDesc` attribute/layout directly into the native `MTLVertexDescriptor`, and creates the PSO from it. The observer preserves those accepted source descriptor values and compares the selected native PSO's label. Describe this as **factory-validated descriptor input**, not a descriptor queried back from native PSO introspection.
- Vertex/index bindings snapshot the same native version used by the existing encoder/draw call and retain its owner/version for frame lifetime. Index bounds include the submitted range; decoded indices include signed `baseVertex`; P4 W reads use the bound offset and 72-byte stride. CPU reads are restricted to Shared, non-frame-upload, correctly used buffers.
- Flora ID rows are appended in the same adapter loop immediately after each successful `DrawIndexed`; backend fetch rows are appended during that same call after its native draw encoding. The CSV's row order therefore matches the receipt's filtered main leaf/trunk draw order, provided both observer outputs are enabled and `pass_complete=1`. IDs remain an external ordered join; the backend API itself has no visual ID.
- Frame/pass abort resets diagnostic references. A pass split marks the receipt incomplete and consumes the one attempt; write/open/rename failure is diagnostic-only and cannot change draw state.

## Evidence limits

The receipt's native PSO label/key and recorded vertex-metallib path identify the selected backend object, but the receipt does not itself hash the loaded vertex/fragment metallib bytes or reflection files. Pin those four artifact hashes in the external launch manifest before describing the observation as tied to a particular compiled shader package. The CPU-visible Shared-buffer hash and W scan establish what bytes were CPU-readable in the exact bound buffer versions and what index/stride math yields; they do not prove GPU execution, shader semantics, or GPU-fetched values. No color/pixel, temporal, culling, or performance conclusion follows.

The draft does not alter TLAB, descriptor tables, samplers, resource residency, pipeline, or draw arguments, so the shader-converter binding-model requirements are outside this observer's modification surface. Reviewed against `integrating-metal-shaderconverter-shaders/references/binding-model.md` and the backend's direct pipeline construction path.
