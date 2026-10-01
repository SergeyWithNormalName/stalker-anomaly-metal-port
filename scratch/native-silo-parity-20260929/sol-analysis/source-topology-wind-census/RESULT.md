# Source topology and live wind: observer result

The frozen default-off observer `candidate-source-topology-wind-01` (EXE `65f5ac0d...`) completed `source-topology-wind-census-01/native` with exit 0. At frame 700, legacy `Sectors.size()` and `Visuals.size()` are both **0**. The subsequent traversal has no loaded source graph; no visibility mask or draw suppression was added. This confirms the loader source finding.

The actual main-pass `r_dtex_range` is **278.091461**, `ps_r2_df_parallax_range` is **75**, and `g_fSCREEN` is 2,916,000.25. Source and callback eye/view/projection are IEEE-float-bit identical to the older fixed main capture. JSON `-0` tokens must be decoded retaining float signed zero; the default integer decoder loses that sign.

Current and previous wind animations, setups and all 32 current/previous benders are captured in `runs/source-topology-wind-census-01/native/output/flora-wind.json`. These are actual frame inputs, not conservative geometry bounds. `wind_verified=false`, `geometry_verified=false`, suppression 0. This observer run is not an FPS or quality acceptance. Exact artifact hashes and checks are in `result-validation.json`.

Production sources and ordinary app/bare outputs were exactly restored before launch; candidate source snapshots and restore receipts are retained here. The larger unbuilt topology serializer remains only under `full-topology-prototype/` and is not part of this candidate.
