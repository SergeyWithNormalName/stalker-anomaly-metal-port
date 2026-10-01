# Compute-skin adapter wiring probe

This is a scratch-only CPU harness for the real `MetalR4DynamicWorldAdapter` API. It does not edit the production adapter and does not create a GPU device. `Backend::EncodeComputeSkinJobs` only queues job descriptions; `Backend::SubmitFrame` materializes deterministic tagged output bytes. `Backend::DiscardFrame` drops pending jobs. The adapter itself performs the real `Configure`, `Prewarm`, `Draw`, `CommitSubmittedSkin`, and `DiscardUnsubmittedFrame` calls.

The harness uses the installed-content snapshots already qualified as small real OGF formats:

- `qualification/installed-skin-fixtures/model-02-root-1.r4skin`: 1W, 123 vertices, SHA-256 `e2300cb62105651710239d172164e51cadec365207efa10299570a31e438bd95`.
- `qualification/installed-skin-fixtures/model-00-root-2.r4skin`: 2W, 167 vertices, SHA-256 `406371bb30d88aa657d9d5f5a34b7845587359a0a3d78620cf085b7ccfdde110`.

Configure uses the existing `build/metal-runtime-shaders/dynamic-world` MSC bundle and `dynamic-world-msc-receipt.txt`. Its receipt records the actual base-bump VS/PS metallib hashes as `36de2f888980629c62d9e8240923d33e671b14ef03567a0b67cf16b7dd186ac4` and `22d5ff4ef98e9c7b249ea479eaec2218550ecd546028bfb771edc9aa410a493d`, and the model-bump VS as `bac1c8c5550fc6894ad6b89c27c5dbc72d2e804789c8bd82f0aa7e4df4c14b73`. A minimal generated DXT1 DDS is supplied through the real provider callback solely to exercise texture admission/upload calls.

Coverage per weight format:

- initial compute queue, no output changes before submit, fake submit, then adapter commit and Draw binding the submitted output;
- frame-context slots 0/1/2 and wrap to 0 while engine frame repeats, proving a committed matching slot pose is a cache hit;
- changed pose B discarded before fake submit, followed by a fresh B dispatch;
- same-key B followed by committed A in one snapshot cancels the pending B batch;
- backend `false` pre-dispatch result causes full CPU fallback, Draw matches `RepackR4DynamicWorldSkinnedVertices`, and the adapter's compute opt-in stays latched off for later changed poses.
- with the adapter oracle opt-in set separately, an owned exact-CPU-byte request is selected for each weight class only while each class has fewer than two committed samples and the output is at most 4 MiB; a discarded selection and a B request canceled by same-key committed A do not spend quota.

The tagged bytes stand in only for a successful backend write and validate command/journal/cache/draw-buffer wiring. They are not a shader arithmetic oracle or GPU result. No performance, app integration, or gameplay claim follows from this harness.
