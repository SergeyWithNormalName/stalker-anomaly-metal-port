# Compute-skin backend probe

## Outcome

The focused build completed and all three authorized CTest cases passed. The offscreen Metal contract probe passed its CPU/API acceptance checks and GPU output readback. FrameUploadGpu self-test and PrivateUploadRecovery also passed. This is API-level validation only; it does not validate rendered presentation or actual frame-submit behavior.

The probe creates an internal backend context, acquires an MTL command buffer manually, calls the production `EncodeComputeSkinJobs` API, and validates the shared acquisition epoch helper. It checks output against canonical CPU expectations and confirms rejected batches leave output unchanged. Its no-drawable retry and slot-wrap coverage resets arenas and exercises `beginAcquiredContext`; it does not force a real `BeginFrame` drawable retry or actual submit path. Do not interpret this as a native integration or gameplay claim.

## Commands and receipts

Exact build and CTest commands are in `commands.txt`. Build output is `build-retry-04.log`; the three-test result is `ctest-retry-04.log`. Frozen source hashes and pre/post app/executable hashes are in `pre-retry-04-sha256.txt` and `post-ctest-sha256.txt`.

The original app executable remains SHA-256 `b0bcf586c65fdb8bad8034202b3df96f0786d7fefaf4f8d9e1b411c29fc323cc`; V4 candidate remains `2bb3dc6b4b35a805a991792864c98f40fb7df128f8431da306d731a88d517f71`. Only the focused probe targets and their required backend library were built. No game was launched, no profile was modified, and no production app was linked or copied.
