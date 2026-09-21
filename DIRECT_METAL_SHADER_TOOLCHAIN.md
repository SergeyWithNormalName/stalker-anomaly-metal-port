# Direct Metal shader toolchain

Status: Phase 1 is reproducible and the bounded Phase 2 representative gate is
complete. The evidence is deliberately split into DXC compilation, MSC
conversion/reflection, and Metal runtime/PSO receipts. None of these receipts
claim a rendered frame or production resource binding.

## Frozen versions

### DirectX Shader Compiler

- Source: official `microsoft/DirectXShaderCompiler` repository.
- Release: `v1.9.2607`.
- Commit: `0d3ee6b551b8fa768fbf825300ebab81047ef6a8`.
- Build method: `script/build_dxc_macos.sh`.
- Default output: `build/direct-metal-toolchain/dxc-v1.9.2607/bin/dxc`.
- Proven ARM64 CLI SHA-256:
  `f739222c4896b4144b8349e35139ccd4fdc455fca7ba6e7c8673f85e4833e462`.
- Proven `libdxcompiler.dylib` SHA-256:
  `87dd08de453fe27618c6b766525dc726d5ba988f62236d032363ec9eee5cab44`.

The build script verifies the source revision before configuring and keeps all
source and products in the ignored developer-local `build/` tree. Tests and
the SPIR-V backend are disabled because this path only needs HLSL-to-DXIL.

### Apple Metal Shader Converter

- Product page artifact: `Metal Shader Converter 4.0 beta 2`.
- Payload CLI version: `metal-irconverter 4.0.1`.
- Authenticated Apple Developer Downloads URL captured by macOS:
  `https://download.developer.apple.com/Developer_Tools/Metal_Shader_Converter_3.1_beta_1/Metal_Shader_Converter_4.0_beta_2.pkg`.
- Package and payload hashes are frozen in
  `src/Layers/xrRenderMetal/shaders/toolchain/metal-shaderconverter-4.0.1.lock`.
- Default isolated output:
  `build/direct-metal-toolchain/metal-shaderconverter-4.0.1`.

On the 2026-09-09 test host, `pkgutil --check-signature` reports `Status:
invalid signature` for the outer package and strict payload signature
validation does not establish a trusted authority. This artifact is therefore
not described as signature-verified. It is accepted only by exact package and
payload hashes tied to the authenticated Apple download URL.

The repository does not install the package system-wide or bypass Gatekeeper.
After reviewing the lock, provision a new isolated copy explicitly:

```sh
./script/provision_metal_shaderconverter.sh \
  --package /path/to/Metal_Shader_Converter_4.0_beta_2.pkg \
  --accept-unverified-package-signature
```

The script refuses an existing destination, verifies the pinned package, CLI,
and library hashes, expands only into a temporary directory, copies the CLI,
headers, library, man pages, and manual under `build/`, and writes a local
`provision-receipt.txt` containing the unverified-signature status.

Apple's bundled manual specifies macOS 13 and Xcode 15 as minimum host
requirements. Converted libraries use Argument Buffers Tier 2 and the gate
targets macOS 14, Metal 3.

### Xcode Metal Toolchain component

- Xcode: 27.0 beta, build `27A5209h`.
- Observed component/compiler: `27A5209h`, Apple metal `32023.917`.

The optional component is not a stable dependency of this gate. On this beta,
`xcodebuild -showComponent MetalToolchain` and a direct cryptex invocation once
reported the component as usable, while ordinary `xcrun metal` and later
non-escalated checks still reported it missing. Production discovery does not
hardcode the ephemeral cryptex path. DXC-to-MSC conversion does not require the
standalone `metal` compiler.

## Discovery contract

Run:

```sh
./script/probe_metal_shader_toolchain.sh
```

The probe searches normal system locations plus the pinned project-local build
directories. Developer-local overrides do not embed personal absolute paths
in production CMake:

```sh
./script/probe_metal_shader_toolchain.sh \
  -DXRAY_DXC_EXECUTABLE=/path/to/dxc \
  -DXRAY_METAL_SHADER_CONVERTER_EXECUTABLE=/path/to/metal-shaderconverter
```

Environment alternatives are `DXC_ROOT` and
`METAL_SHADER_CONVERTER_ROOT`. R4 targets must call
`xray_require_metal_shader_toolchain()` only inside an Apple-only, explicitly
enabled conversion path so the existing DXVK build remains unaffected.

## Real GAMMA shader source materialization

R4 reads source from the logical `r3/` tree; `r4/` is the compiled DXBC cache
namespace. The winning GAMMA overlay lacks some base includes, so the gate
materializes the canonical `shaders.db0` archive and overlays the active loose
files into a new temporary tree:

```sh
./script/materialize_r4_shader_sources.sh \
  --crossover-bottle NAME \
  --output /private/tmp/gamma-r4-shaders
```

The script refuses the live `runtime/` tree and symlink outputs. The proven
materialization contained 660 `r3` files and used archive SHA-256
`f18413b82890ea43bc55ec50df2c6477d451c8dabfe09cc4b5c040f0ee8de669`.

One narrow compatibility patch is applied only to the temporary tree. DXC
rejects the FXC expression form `[unroll(q_steps[...].x)]`; under
`__hlsl_dx_compiler` the patch uses plain `[unroll]`, while preserving the FXC
path and live GAMMA files.

## Representative shader gate

The manifest
`src/Layers/xrRenderMetal/shaders/representative-gate.tsv` contains eleven real
GAMMA/R4 entries: static world VS/PS, `SKIN_0` model VS, the exact non-MSAA sun
wrapper, shadow VS, grass VS, water PS, TAA PS, HUD PS, and the admitted L05 Bar
HQ terrain VS/PS pair. The macro set is derived from the proven non-MSAA R4
configuration and active SSFX values.

### 1. DXC receipt

```sh
./script/probe_r4_shader_dxc.sh \
  --source-root /private/tmp/gamma-r4-shaders/base/shaders/r3 \
  --output /private/tmp/gamma-r4-dxil
```

`dxc-gate-receipt.txt` records the exact compiler/library, source tree,
materialization, manifest, compatibility patch set, arguments, source hashes,
DXIL hashes, and warnings. Its scope is HLSL-to-DXIL only.

### 2. MSC conversion and structural reflection receipt

```sh
./script/convert_r4_shader_msc.sh \
  --dxil-dir /private/tmp/gamma-r4-dxil \
  --output /private/tmp/gamma-r4-metal
```

Before conversion, `validate_dxc_gate_receipt.py` recomputes the current
manifest and every DXIL hash. The converter emits a metallib and reflection
JSON for every entry. `validate_msc_reflection.py` checks entry point, stage,
resource counts and ranges, vertex inputs, and fragment target counts. This is
structural metadata evidence; it does not prove argument-buffer writes,
resource residency, or a draw.

### 3. Runtime load and PSO constructibility receipt

After reading `man MetalValidation`, run on a real Apple GPU:

```sh
MTL_DEBUG_LAYER=1 \
MTL_SHADER_VALIDATION=1 \
MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 \
./script/launch_metal_shader_gate.sh \
  --receipt /private/tmp/gamma-r4-metal-runtime-receipt.txt \
  /private/tmp/gamma-r4-metal/*.metallib
```

The harness queries each library's exported function name rather than assuming
one, and uses Metal Shader Converter's
`kIRStageInAttributeStartIndex`/`kIRVertexBufferBindPoint` constants. The proven
Apple M1 Pro run loaded all nine functions under API and GPU validation and
created:

- a depth-only `shadow_direct_tree` PSO;
- a `SKIN_0 deffer_model_bump.vs + deffer_base_bump.ps` G-buffer PSO using the
  exact 36-byte `FSkinned.cpp::dwDecl_01W` layout and four real R4 scene MRT
  format classes.

Before launch, `validate_msc_gate_receipt.py` recomputes every metallib and
reflection hash against the MSC receipt. The runtime receipt hashes that
receipt, the validator, manifest, harness, executable, runtime binding header,
and every metallib, and records validation state and complete program output.

The successful 2026-09-09 receipt and its complete hash chain are preserved in
`DIRECT_METAL_SHADER_GATE_EVIDENCE.md`.

## Evidence boundary and next work

The representative compile/convert/load gate is complete. It proves toolchain
viability across every required shader class and central PSO constructibility.
It does not yet prove actual R4 argument-buffer population, resource residency,
draw submission, textures, or visual parity. `SKIN_0` is the requested live
macro permutation but was not found as an exact installed DXBC cache pairing,
so the old cache remains reference evidence rather than a byte-paired oracle.

Phase 3 can now introduce an explicit Metal backend selector while keeping
DXVK as the default and preserving the standalone bootstrap and all fallback
paths.
