# Borrowed MSC typed-alias hypothesis

This single-file draft changes only local typed aliases inside `BindMscResources`. With `XRAY_METAL_E08_BORROW_MSC_TYPES` unset or anything except exact `1`, each `dynamic_pointer_cast` / `static_pointer_cast` path remains selected and retains the original temporary shared owner. With the flag set to exact `1`, local aliases are raw pointers obtained from the still-live `bindings.resources` or `constantSlices` owners. The first-pass null validation remains in place. The second-pass constant-buffer `static_cast` is only used for the `MetalBuffer` already validated in pass one (or created by `UploadFrameData`).

Resource-owner retains, frame snapshots, indirect-read declarations, descriptor validation, descriptor bytes, TLAB allocation, and GPU addresses are unchanged. The flag is strict default-off. The expected opportunity is limited to avoiding temporary shared-reference increments/decrements; the experiment has no frame-time or GPU-saving claim.

No build, probe, or game run was performed.
