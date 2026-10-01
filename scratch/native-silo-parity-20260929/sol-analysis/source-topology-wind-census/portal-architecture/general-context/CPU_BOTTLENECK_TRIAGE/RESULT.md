# Outdoor pre-oracle CPU sample triage

This is a single 5-second `sample` capture of PID 96654 at frame 207, before the frame-700 GPU oracle, from `portal-pixel-oracle-outdoor-07`. It is a diagnostic stack sample, not an FPS acceptance run, not a GPU-occupancy measurement, and not a frame-time breakdown. Do not add inclusive stack counts or translate these samples into GPU work.

The main thread call graph reports 1,122 sample entries; `MetalR4EngineScene::Render` is the dominant identified application frame with 766 entries. One observed child path reaches `renderProductionScene` (388) and the scene-owned G-buffer callback, where `MetalR4FloraAdapter::DrawIntoActiveGBufferCommon` appears at several instruction locations. In that Flora path the trace includes `MetalR4Backend::BindMscResources`, `declareIndirectRead`, `MetalR4Backend::UploadFrameData`, and `DrawIndexed`; the report includes a `DrawIndexed` child node with 31 samples and multiple binding/declaration nodes (for example 16 and 14 at distinct return PCs). These counts overlap as a call graph and must not be summed into a claimed Flora percentage.

The code supports a concrete CPU-side investigation target: the Flora loop rebuilds three constant-buffer staging vectors, makes three immutable upload calls, then binds VS and PS MSC tables and submits one indexed draw per admitted visual. The backend validates each MSC entry and declares resource use per binding. That is an explicit command-encoding path worth measuring, but this one sample does not prove it is the dominant cost or that reducing it improves FPS. The small node counts at `BindGraphicsPipeline` do not support a pipeline-bind optimization claim. The backend has an existing `XRAY_METAL_E08_DEDUP_RESOURCES` switch, but its launch value was not established from this capture; do not assume whether resource dedup was active.

Background thread call graphs prominently end in `_pthread_cond_wait` or `std::this_thread::sleep_for`/`nanosleep`, consistent with parked worker threads. The main render stack is not shown blocked on those worker waits. These waits describe the sampled worker stacks; they are not a GPU wait measurement and should not be compared by summing counts across threads.

A bounded next step is to read the existing E08 CPU profile output for the same run, if present, especially `e08CpuMs[1]` around `BindMscResources`, and verify the dedup switch from the runner's non-secret setting manifest. Only if that points to a meaningful share should a tightly paired diagnostic compare CPU bind/declaration cost with and without the already-supported switch, while preserving the pixel oracle. No further profiling or code change is justified by this stack alone.

## Source anchors

- `MetalR4FloraAdapter.cpp`: main Flora draw loop / staging vectors at 917-927, three `UploadFrameData` calls at 1003-1008, MSC table binds and indexed draw at 1043-1044.
- `MetalR4Backend.mm`: CPU timing scope around MSC binding at 2893-2895; resource validation/declaration loop at 2907-2964; optional dedup gate in `declareIndirectRead` at 4797-4805 and its environment field at 4898.
- `MetalR4EngineScene.mm`: scene draw is entered from `renderProductionScene` at 4346 and wrapped in `RunSceneOwnedGBufferCallback` at 4627.

## Receipts

- `root-cpu-sample-result.json`: frame 207, PID 96654, bounded five-second capture, exit 0.
- `root-cpu-pre-oracle-sample.txt`: raw Apple sampling call graph.
