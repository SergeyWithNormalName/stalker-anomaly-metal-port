# Versioned Shared static-globals hypothesis

This scratch-only candidate adds an opt-in `XRAY_METAL_VERSIONED_STATIC_GLOBALS=1` path for the 528-byte `baseLightingStaticGlobals` uniform. Default behavior stays on the existing Shared-buffer snapshot path. When enabled, the MSC binder uses the original Shared buffer range and retains both its owner and `NativeVersion` through frame completion. A later `UpdateShared` therefore clones the current allocation before writing, using the existing COW lifetime contract.

`R4BufferDesc::versionedConstant` is appended to preserve existing aggregate initializers. The backend accessor qualifies the opt-in to Shared Uniform buffers. The local buffer factory only marks `baseLightingStaticGlobals`; all other constant buffers keep the old path. The existing E02 GPU version probe adds Uniform usage with the flag enabled and checks exact old/new readback patterns.

This is a hypothesis package only. No build, GPU probe, game run, or production source edit was performed. Runtime pixel equivalence and frame-time impact remain unverified.
