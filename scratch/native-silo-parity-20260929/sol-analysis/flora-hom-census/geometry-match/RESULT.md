# Cordon HOM to active level triangle screen

## Result

This CPU-only screen found **zero** raw float32 triangle identities between
the extracted Cordon HOM and valid active `MT_NORMAL` indexed geometry across
the whole active `level.geom`. It also found zero HOM `VPack`-equivalent
triangles near baseline camera `(-221.082565, -18.284866, -168.182037)`.
That local screen reproduces `CHOM::Load`'s `Fvector::similar` component rule
(`abs(delta) < 0.01`) with a 150 m HOM-centroid radius and 150.02 m active
triangle coverage radius. The screen touched 266,226 local active triangles;
it used exact indexed POSITION0 vertices and each visual's model transform.

The active level contains 8,303,157 indexed triangles, of which 1,242,810
belong to valid `MT_NORMAL` visuals. The HOM input contains 1,414 triangles;
all 1,414 records have flags `1`. The match CSV has only its header. Every
potential match would have retained HOM triangle/flags, visual ID/type,
shader ID/name/base texture, source triangle index, match kind, and winding.
No alpha/depth/material qualification or suppression is produced; shader
names alone are not an opacity proof. The earlier observer's 3,377 hidden
rest AABBs (3,240 after a 2 m sensitivity pad) therefore still have no
triangle-level source geometry qualification. Keep `geometry_verified=false`.

The input manifest pins the active `level` and `level.geom`, extracted
`level.hom`, frozen parser, producer, and source receipts. `run.sh` compiles
only the standalone CPU screen and validates the zero-row output; it does not
build the game or touch production sources.

## Source contract review

There are no matched shader families to audit for occluder eligibility. For
context only, the current R4 configured terrain G-buffer descriptor in
`src/Layers/xrRenderMetal/MetalR4EngineScene.mm:1521-1544` uses depth compare
`Less`, depth writes enabled, back-face culling, and clockwise front faces.
The paired `deffer_terrain_high_flat_d.ps` fragment source samples material
textures and writes the G-buffer; it contains no `clip`/`discard` operation.
This is a source-level contract for that configured terrain path, not proof
that any HOM triangle matches it. Other static material routes remain
unqualified.

The HOM loader preserves the per-triangle flags through `add_face_packed_D`
and into `occTri::flags` (`src/Layers/xrRender/HOM.cpp:136-169`). In the HOM
selection rule, a nonzero flag bypasses the camera-side plane test
(`HOM.cpp:298-301`), so a one-sided render match would still need a separate
winding/two-sided proof before it could justify suppression. No winding union,
approximate bounds, or physics `cform` evidence was used here.

## Reproduction and artifacts

- `screen.cpp`: frozen-parser indexed triangle matcher.
- `run.sh`: standalone CPU build/run and output validation.
- `matches.csv`: empty match set with schema header.
- `manifest.json`: input/output sizes and SHA-256 values plus counts and limits.

The result is geometric candidate triage only. It provides no FPS/GPU-saving
claim and does not change HOM's runtime behavior.
