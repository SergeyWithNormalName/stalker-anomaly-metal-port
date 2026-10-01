# Adjacent static-shadow pipeline reuse: reject after first screen

Same immutable candidate `fc48ab71424b7ae6f01822a1ab275d3d49ffc4aab0e37e6ee3fd4189c939a015`, pinned save/config/shaders, combined V7 features, 1920x1200, fixed Cordon camera, weather paused at w_clear2, device factor 1, game factor 6 and live AI. E01/E07 diagnostics and the frame360 count observer were enabled in both arms. No CUA, CPU sampler or build ran during either measurement window.

| First short screen | Off | On |
|---|---:|---:|
| Valid on-glass mean FPS | 18.7030 | 18.9288 |
| Callback mean FPS | 18.6998 | 18.9269 |
| CPU encode median, ms | 48.0219 | 48.2560 |
| Indexed draws median | 21097 | 21375 |
| GPU command-buffer elapsed median, ms | 36.6448 | 36.5810 |

The 1.21% displayed change is small and confounded by different live workloads. CPU encode did not improve. This is not proof of a reliable speedup or regression. There is no useful first-screen result warranting an expanded cache or qualification matrix.

The completed measured frame360 census explains the small opportunity: local static shadows make only 1270/1237 merged draws, and the cache removes 566/546 PSO requests (roughly 3.7% of complete-frame PSO requests). It preserves all static draw order and resource/MSC/VB/indexed arguments, but does not reduce draws or uploads. No exact pixel/motion acceptance was attempted for this rejected candidate.

The single source file was restored exactly to prestate `c7c3b5c77e11e41b78095c867e25fe7568b886aa70c2d2fc3eb6ee1af3c1af53`; all original 47 handoff source hashes match again. Production bundle remains the saved original. The tiny frozen runtime/observer patches, candidate, raw A/B runs and restore receipt remain archived.

Normal logs expose a larger target: the main visible-Flora pass submits 5316 draws, while the sampled main-thread stack spends 437/2264 inclusive samples in `DrawIntoActiveGBuffer`. Profiling is perturbed, not an elapsed-time or FPS claim. Source feasibility and a minimal same-frame output hypothesis are required before extending already-qualified shadow instancing to this pass.
