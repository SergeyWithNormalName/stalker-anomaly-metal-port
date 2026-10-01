# Offline start-sector screen

## Finding

For saved frame 700 at eye bits `[3277657379, 3247589224, 3274190490]` (`(-221.0825653, -18.2848663, -168.1820374)`), a standalone CPU parser of the active GAMMA `l01_escape/level.cform` found exactly one nearest downward CFORM hit within 1000 m: triangle 77,364, sector tag 25, at `y=-19.93986` / distance `1.65499`. The source's upward retry also has one nearest hit in sector 25 (`y=-17.32386`, distance `0.96101`). The CFORM record layout is verified exactly: v4 header, 1,664,581 vertices, 2,949,353 16-byte triangles, and total size 67,164,656 bytes. The triangle sector tag is the upper 16 bits of the final packed word. `CObjectSpace::Load` passes triangles through `CLevel::Load_GameSpecific_CFORM`; that callback remaps material and shadow/wallmark flags but does not rewrite sector tags.

The tool also parsed all 128 source portals and checked both source `detectLastSector` vertical rays against portal triangles. There are zero portal intersections at this eye, so the portal-first query cannot supersede the nearest CFORM result. Since the source starts downward and only retries upward if no sector was found, the source-selected start sector is **25**, assuming its normal CDB nearest-hit semantics; this case has a unique nearest triangle, not a tie.

This is an indoor/small-room start sector, not outdoor sector 30. Sector 25's OGF root is 22,078 (root-box volume 229.8, and the camera lies inside that box). The engine's `LoadSectors` source heuristic selects the largest root-box volume as `pOutdoorSector`; active level data makes that sector 30/root 21,910 (volume 422,726,262). The start-sector decision above comes from a sector-tagged CFORM ray, not from assuming a root box determines sector ownership.

Sector 25 is directly adjacent to sector 30 through five source portal polygons (IDs 58–62). Therefore portal-frustum traversal could plausibly constrain the 5,308 frame-700 Flora visual IDs owned by sector 30. That is an opportunity hypothesis only: portal adjacency does not establish which polygons pass the camera frustum, which outdoor roots/Flora bounds pass the accumulated frusta, or a count of eligible draws.

## Scope and limits

The script is a CPU-only, read-only binary screen; it does not call engine CDB/OPCODE. It parses the exact CFORM and level chunk layouts and computes vertical intersections directly. The unique nearest triangle plus absence of portal-ray hits makes start-sector 25 robust for this frame, but it is not a general replacement for CDB tie/edge behavior. It does not replay `CSector::traverse`, `CFrustum::ClipPoly`, source SSA/fade, HOM, or root-visual draw tests, and must not be used for suppression or an FPS claim.

Exact source portal-frustum membership is feasible offline from the current assets: 32 sector records, 128 portals, their sector portal lists/root visual indices, the saved frame-700 view/projection bits, and the captured `r_ssaDISCARD` value are available. A small follow-up would need to mirror the source portal sphere/neighbor selection, polygon clipping/frustum construction, and SSA/fade test. Source `GMBase` uses options 7 (`VQ_HOM | VQ_SSA | VQ_FADE`); because this task excludes HOM culling, a no-HOM replay would be a clearly labeled portal-only superset, not an exact source visible-sector result. Even an exact sector-frustum set would still need root/descendant frustum and source draw-gate checks before estimating Flora IDs.

## Inputs and reproducibility

- Active `level`: SHA-256 `1800a451d0bba9af1da69fae8683b36b6e6346709c0d6b9bf14385f4a48d19f7`.
- Active `level.cform`: SHA-256 `6a8e56d483d60da0f461fb2e20ca64ba193b94ed883013c337c7d293a2410e31`.
- Captured frame camera/matrix and runtime fields: `scratch/native-silo-parity-20260929/runs/source-topology-wind-census-01/native/output/source-topology.json`.
- Run: `python3 vertical_sector_screen.py` (requires local NumPy).
- Source contracts: `src/Layers/xrRender/r__sector_detect.cpp` (`detectLastSector`), `src/Layers/xrRenderPC_R4/r4_loader.cpp` (`LoadSectors`, largest-volume `pOutdoorSector`), `src/Layers/xrRender/r__sector.cpp` (`CPortal::Setup`, `CSector::traverse`), `src/xrCDB/xrCDB.h` (`TRI::sector`), and `src/xrGame/Level_load.cpp` (`Load_GameSpecific_CFORM`).
