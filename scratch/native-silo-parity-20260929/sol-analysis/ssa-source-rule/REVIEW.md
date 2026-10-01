# Generic main Flora source SSA candidate review

Luna source review: initial named-file eligibility NO-GO; updated exact namespace
inspection conditional PASS. Level MT_TREE_ST visuals start with flags zero and
are loaded directly, while existing optimization-override mutation paths use
separate gameplay/HUD model-pool instances. Non-overridden B_TREE uses a fresh
ShaderElement E[4], whose emissive flag is false; the tree blender does not set it.
Every actual Lua override is excluded instead of compiling its GPU shader.

The opt-in inspector reuses R4 Lua bindings/file order/namespace semantics in a
separate CPU-only VM. Whole namespace loads use lua_cpcall, their boolean/status
is checked, and errors/unknown routes disable all eligibility. Namespace traversal
rejects metatable-dependent/non-table states before source lookup. No compiler
instance, OnDeviceCreate, _lua_Create, or GPU resource compile is invoked. Current
shader scripts are the same trusted inputs as ordinary engine startup; their
top-level code is executed in a separate VM. No mod is rewritten.

Build: all changed translation units compiled. AnomalyDX11 final build exit0.
FloraAdapter.SelfTest PASS. Independent source-decision oracle PASS on all5316
actual frame700 IDs:1199 reject/4117keep. Geometry standalone self-test initially
failed because the existing fixture reserved96 bytes for a tree transform plus
10 floats (requires104); the preserved prestate helper fails identically. Only
that fixture extent is repaired in the draft; standalone self-test now PASS.
The CMake LevelGeometryProbe link failed with unresolved engine Memory/mem_free
symbols; that path is not recorded as PASS and no unrelated CMake repair is made.

Not production accepted. Runtime metadata admission, pixel/motion correctness,
and same-binary performance remain pending. Shared source was reversed exactly
after freezing candidate-ssa-source-01; ordinary output restoration follows the
normal rebuild, with its original resumed-turn app/bare preserved separately.
