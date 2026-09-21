# Direct Metal baseline gate

Date: 2026-09-09

Machine: MacBook Pro 14-inch (2021), Apple M1 Pro 10-core CPU / 16-core GPU, 16 GB unified memory

Scene: `gamma-world` save, `l05_bar`, fixed initial camera

Output: benchmark-only 960x540 logical SDL window, verified by the DXVK presenter as a 1920x1080 physical swapchain

## Preservation point

The playable fallback remains the existing R4 D3D11 path:

```text
native ARM64 X-Ray / GAMMA
  -> R4 D3D11
  -> DXVK Native
  -> Vulkan
  -> project-local custom MoltenVK
  -> Metal
```

- Source branch before this work: `macos-port`
- Source commit: `9a75fefc0608bde0b796beb5bd938a3f95300172`
- Working-port state at the branch point: 458 modified tracked paths and 106 untracked paths. This uncommitted layer is part of the currently playable port and was not rewritten or bulk-committed.
- Direct Metal work branch: `feature/native-metal-r4`
- ARM64 executable SHA-256: `0c5cb4ea336119c37db816c6cf33228227c2152f3ed7e90626b0d21ba432eff4`
- Safe custom MoltenVK SHA-256: `79fae6ee35c2ba0562f201b8570b01f4636cfa63d2905a41cbb43f74e4258785`
- Required stability setting: `MVK_CONFIG_LIVE_CHECK_ALL_RESOURCES=1`

The DXVK/MoltenVK code and normal launcher remain the default/reference path. The Metal bootstrap is an independent opt-in target.

## Measurement method

The measured process was launched with:

```sh
./script/launch_moltenvk_benchmark_variant.sh safe --load gamma-world
```

For the GPU run, only Metal Performance HUD logging and encoder timing were enabled. `MTL_CAPTURE_ENABLED`, `MTL_DEBUG_LAYER`, `MTL_SHADER_VALIDATION`, Vulkan validation layers, and DXVK debug logging were all disabled/unset by the SAFE wrapper. No GPU capture was used.

Evidence:

- `profile-safe-1080p-cpu-sample.txt`: 5-second, 1 ms requested-interval `sample(1)` report from the warmed process, fully symbolicated for the engine, DXVK, and custom MoltenVK.
- `profile-safe-1080p-5s.trace`: completed 5-second attach-mode Metal System Trace (89 MB). Its CPU tables are usable. Because Instruments attached after the Metal device was created, application-level encoder mapping is absent; no GPU number below is inferred from its system-wide GPU events.
- `profile-safe-1080p-metal-hud-sample.csv`: the Metal HUD batch beginning at frame 1709, transcribed as 33 consecutive `(present interval, GPU time)` pairs. Apple documents the console format as frame number, graphics memory, process memory, followed by per-frame present-interval/GPU-time pairs.

The HUD batch was parsed without dropping any values:

| Metric | Median | p95 | p99 | Min | Max |
|---|---:|---:|---:|---:|---:|
| Present interval | 30.97 ms | 36.61 ms | 37.50 ms | 18.17 ms | 39.54 ms |
| GPU execution | 30.43 ms | 32.38 ms | 32.63 ms | 27.96 ms | 32.91 ms |

The mean present interval for that steady one-second batch corresponds to 32.73 FPS. Other observed HUD batches contain GPU excursions around 56-65 ms and present intervals above 100 ms, matching the reported unstable frame pacing. Those excursions are evidence of spikes, not a statistically sufficient long-run p99 benchmark.

## Approximate frame-time decomposition

The CPU report contains 855 samples for each long-lived thread. Multiplying a thread's sample share by the 30.97 ms steady median frame interval provides an approximate per-frame occupancy. It is a sampling estimate, not nested instrumentation, so parallel rows do not add to one serial frame.

| Area | Evidence | Approximate time / occupancy |
|---|---|---:|
| Main/game update | 63/855 main-thread samples in `CLevel::OnFrame` and adjacent frame update | about 2.3 ms/frame |
| R4 recording on main thread | 326/855 samples in the render section of `CRenderDevice::Run`; 301 of those are below `CLevel::OnRender` | about 11.8 ms/frame |
| Main-thread present/acquire wait | 245/855 samples in `DxgiSwapChain::PresentBase -> Presenter::acquireNextImage -> condition_variable::wait` | about 8.9 ms/frame |
| Main-thread task wait | 202/855 samples in TBB `execute_and_wait` | about 7.3 ms/frame; worker work overlaps other threads |
| DXVK/MoltenVK submission CPU | 735/855 `dxvk-submit` samples actively below `vkQueueSubmit2 -> MVKQueueCommandBufferSubmission::execute`; only 114/855 are idle queue wait | about 26.6 ms/frame of parallel backend-thread occupancy |
| DXVK command-stream CPU | 282/855 `dxvk-cs` samples active, including descriptor updates; 573/855 are idle wait | about 10.2 ms/frame of parallel backend-thread occupancy |
| GPU execution | Metal HUD command-buffer GPU start/end timing | 30.43 ms median, 32.38 ms p95 in the selected steady batch |

The submission stack is specifically backend work rather than gameplay: it is dominated by MoltenVK command encoding, draw-state finalization, resource residency/liveness processing, descriptor binding, and render-pass construction. `MVKUseResourceHelper::bindAndResetGraphics`, `bindMetalResources`, `encodeGPUAddressableBuffers`, and `MVKLiveList::isLive_` are all present in the hot submission call graph.

## Classification and decision

**Classification: mixed, backend-dominated.**

- It is not the CPU/A-Life stop case: main/game work is nowhere near 30 ms by itself.
- GPU execution is essentially the whole steady frame interval (30.43 ms GPU versus 30.97 ms present median).
- In parallel, the translation backend consumes most of one performance core while turning DXVK/Vulkan commands into Metal commands.
- The main thread visibly waits for swapchain availability rather than continuously executing gameplay.

**Decision: GO to the Direct Metal feasibility project.**

The profile does not promise 60 FPS. It establishes that the limiting work is precisely work Direct Metal can plausibly reduce: GPU pass/bandwidth cost and DXVK/MoltenVK command translation/submission. The next continuation gates remain visual parity and a same-scene 1920x1080 benchmark; Direct Metal must be stopped if a complete parity-capable backend shows no measurable benefit.

## Evidence limitations

- The selected HUD batch is short and suitable for the go/no-go attribution, not a final frame-pacing distribution.
- Attach-mode Instruments could not emit process-specific Metal encoder tables because the Metal device predated attachment. Launch-under-Instruments was attempted, but macOS TCC denied Instruments access to scripts under `Documents` with `Operation not permitted`.
- Encoder-level GPU attribution therefore remains a future profiling task once the app is packaged outside the protected Documents path or granted the relevant Instruments access.
- The first Direct Metal milestone below is only clear/present. It has no R4 visual or performance meaning.

## Minimal Metal bootstrap result

The Apple-only `XRay.Metal.Bootstrap` target is intentionally independent of `AnomalyDX11`. It uses the same SDL/Cocoa family of platform APIs with `SDL_WINDOW_METAL`, creates the system `MTLDevice`, a command queue, an SDL Metal view/CAMetalLayer, clears each drawable, and presents it. It does not load R4 or alter the fallback selector.

Runtime acceptance command:

```sh
./script/launch_metal_bootstrap.sh --frames 240
```

Observed output:

```text
METAL_BOOTSTRAP_READY device="Apple M1 Pro" logical=960x540 drawable=1920x1080 pixel_format=BGRA8Unorm
METAL_BOOTSTRAP_COMPLETE frames=240 drawable=1920x1080
```

The process exited with status 0 after all 240 clear/present frames. An earlier run of the same loop also completed 600 frames. The final command buffer completed without a Metal error. This satisfies only the Phase 3 platform bootstrap gate; it is not evidence of R4 rendering, visual parity, or performance.
