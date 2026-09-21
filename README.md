# Unofficial STALKER Anomaly Native Metal Port

Experimental source-only work on an Apple-platform / Metal backend for the
X-Ray Monolith engine used by S.T.A.L.K.E.R. Anomaly and compatible Anomaly
modpacks. The repository is intended for engine research and collaboration;
it is not a game distribution or a GAMMA repack.

## Scope

This repository contains the engine source changes, CMake build files, Metal
backend code, shader-conversion tooling, and engineering notes that are useful
to reproduce the current development state.

It deliberately does **not** contain Anomaly or GAMMA runtime files, game
assets, saves, shader databases, compiled binaries, profiling traces, or
modpack archives. Obtain any required game/mod files from their respective
official sources and do not upload them here.

## Status

This is experimental work in progress. Buildability, renderer bring-up, and
individual feature gates do not by themselves establish full gameplay parity,
visual parity, performance parity, or permission from the relevant rights
holders. See the `DIRECT_METAL_*.md` notes for the current evidence and open
gates.

## Legal notice

This is an unofficial fan project. It is not affiliated with or endorsed by
GSC Game World, the Anomaly developers, the GAMMA team, or any individual
mod/add-on author. The names S.T.A.L.K.E.R., Anomaly, and GAMMA are used only
to identify compatibility and the project context.

The repository is a composite work. The existing `License.txt` governs the
X-Ray/GSC-derived source under its stated non-commercial terms. Third-party
components retain their own copyright notices and licenses. `LICENSE.md`
explains why this repository does not apply a blanket MIT/GPL/Apache license.

Do not sell, paywall, bundle, or redistribute copyrighted game/modpack data
with this source tree. If a rights holder or a component author identifies
material that may not be published here, please open an issue or contact the
maintainer so it can be reviewed or removed.

## Upstream and references

- [X-Ray Monolith upstream](https://github.com/themrdemonized/xray-monolith)
- [S.T.A.L.K.E.R. Anomaly project page](https://www.moddb.com/mods/stalker-anomaly)
- [S.T.A.L.K.E.R. G.A.M.M.A.](https://github.com/Grokitach/Stalker_GAMMA)
- [GSC fan-content guidelines](https://www.gsc-game.com/guidelines/)

