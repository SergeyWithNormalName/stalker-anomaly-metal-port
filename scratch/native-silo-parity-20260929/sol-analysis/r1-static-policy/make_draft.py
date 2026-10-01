#!/usr/bin/env python3
"""Independent R1 source-policy gate/observer artifacts; never apply a patch."""
from pathlib import Path
import difflib
import hashlib
import json

OUT = Path(__file__).resolve().parent
ROOT = OUT.parents[3]
DEVICE = "src/Layers/xrRenderMetal/MetalRenderDeviceRender.mm"
SCENE = "src/Layers/xrRenderMetal/MetalR4EngineScene.mm"
EXPECTED = {DEVICE: "81188b9f3a6aab3e3d47b606efcd8b62fc0239a05f2b2285b9324bea973a8073",
            SCENE: "c7c3b5c77e11e41b78095c867e25fe7568b886aa70c2d2fc3eb6ee1af3c1af53"}
BASE = {p: (ROOT / p).read_text() for p in EXPECTED}


def sha(text):
    return hashlib.sha256(text.encode()).hexdigest()


for p, expected in EXPECTED.items():
    assert sha(BASE[p]) == expected, (p, sha(BASE[p]), expected)


def once(text, old, new):
    assert text.count(old) == 1, (text.count(old), old[:100])
    return text.replace(old, new, 1)


loop = "\t\tfor (light* source : RImplementation.v_all_lights)\n"
candidate = {DEVICE: once(BASE[DEVICE], loop, """        static const bool r1StaticPolicy = [] {
            const char* flag = std::getenv("XRAY_METAL_R1_STATIC_POLICY");
            return flag && std::strcmp(flag, "1") == 0;
        }();
""" + loop)}
candidate[DEVICE] = once(candidate[DEVICE], """\t\t\tconst u64 parentIdentity = static_cast<u64>(reinterpret_cast<uintptr_t>(source));
""", """            // Same package gate as CLight_DB::add_light; no camera/HOM/LOD policy change.
            if (r1StaticPolicy && source->flags.bStatic && !ps_r2_ls_flags.test(R2FLAG_R1LIGHTS))
                continue;
\t\t\tconst u64 parentIdentity = static_cast<u64>(reinterpret_cast<uintptr_t>(source));
""")

observer = BASE.copy()
observer[DEVICE] = once(observer[DEVICE], """\t\tif (!backend->SubmitEngineLightingFrame(lightingFrame, error))
""", """        static const bool r1PolicyObserve = [] {
            const char* flag = std::getenv("XRAY_METAL_R1_STATIC_POLICY_OBSERVE");
            return flag && std::strcmp(flag, "1") == 0;
        }();
        if (r1PolicyObserve && lightingFrame.engineFrame == 360u)
        {
            const bool allowR1 = ps_r2_ls_flags.test(R2FLAG_R1LIGHTS);
            std::unordered_map<std::uint64_t, std::size_t> capturedRecords;
            for (const auto& light : lightingFrame.lights) ++capturedRecords[light.parentLightId];
            std::unordered_set<std::uint64_t> reported;
            for (const auto& light : lightingFrame.lights)
                if (reported.emplace(light.parentLightId).second)
                    std::fprintf(stderr,
                        "METAL_R1_STATIC_PARENT frame=%u parent=%llu source_static=%u allow_r1=%u "
                        "policy_reject=%u captured_records=%zu type=%u shadowed=%u hud=%u volumetric=%u\\n",
                        lightingFrame.engineFrame, static_cast<unsigned long long>(light.parentLightId),
                        unsigned(light.staticLight), unsigned(allowR1), unsigned(light.staticLight && !allowR1),
                        capturedRecords.at(light.parentLightId), unsigned(light.type), unsigned(light.shadowed),
                        unsigned(light.hudMode), unsigned(light.volumetricEnabled));
            std::fprintf(stderr, "METAL_R1_STATIC_CAPTURE_DONE frame=%u parents=%zu records=%zu allow_r1=%u\\n",
                lightingFrame.engineFrame, capturedRecords.size(), lightingFrame.lights.size(), unsigned(allowR1));
        }
\t\tif (!backend->SubmitEngineLightingFrame(lightingFrame, error))
""")
# This translation unit already uses unordered_set. Device did not previously
# include it, so add an explicit header rather than relying on transitive includes.
if "#include <unordered_set>" not in observer[DEVICE]:
    observer[DEVICE] = once(observer[DEVICE], "#include <unordered_map>\n", "#include <unordered_map>\n#include <unordered_set>\n")
observer[SCENE] = once(observer[SCENE], """    E07ShadowCpuScope e07ShadowCpu(dynamicWorld, true);
""", """    E07ShadowCpuScope e07ShadowCpu(dynamicWorld, true);
    static const bool r1PolicyObserve = [] {
        const char* flag = std::getenv("XRAY_METAL_R1_STATIC_POLICY_OBSERVE");
        return flag && std::strcmp(flag, "1") == 0;
    }();
""")
observer[SCENE] = once(observer[SCENE], """        record.renderedDepth = true;
        ++shadows.renderedLights;
""", """        record.renderedDepth = true;
        ++shadows.renderedLights;
        if (r1PolicyObserve && view.engineFrame == 360u)
        {
            const auto& light = snapshot->lights[candidates[tile].lightIndex];
            // Source-static light policy is distinct from static geometry caster work.
            // Snapshot values and already-produced per-tile counters only; no clock/query/draw.
            std::fprintf(stderr,
                "METAL_R1_STATIC_SHADOW_FACE frame=%u batch=%u tile=%zu parent=%llu light=%llu "
                "face=%u type=%u source_static=%u material_rows=%zu static_geometry_draws=%zu "
                "dynamic_geometry_casters=%zu flora_shadow_draws=%zu transform_bytes=%zu\\n",
                view.engineFrame, batchId, tile, static_cast<unsigned long long>(light.parentLightId),
                static_cast<unsigned long long>(record.lightIdentity), record.face, unsigned(light.type),
                unsigned(light.staticLight), materialEntries[tile].size(),
                shadows.mergedEncodedDraws - homStaticBefore, shadows.submittedDynamicCasters - homDynamicBefore,
                flora && flora->IsReady() ? flora->LastTelemetry().shadowDraws : 0u,
                materialEntries[tile].size() * 256u);
        }
""")


def emit(name, draft):
    patch = "".join("".join(difflib.unified_diff(BASE[p].splitlines(keepends=True),
        draft[p].splitlines(keepends=True), fromfile="a/" + p, tofile="b/" + p)) for p in draft)
    (OUT / (name + ".patch")).write_text(patch)
    files = {}
    for p, content in draft.items():
        dest = OUT / name / p
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(content)
        files[p] = {"prestate_sha256": sha(BASE[p]), "draft_sha256": sha(content)}
    return {"patch_sha256": sha(patch), "files": files}


manifest = {"schema": "r1-static-source-policy-drafts/v1", "scope": "scratch-only-no-source-application",
            "independent_patches": True, "gate": emit("r1-static-policy", candidate),
            "observer": emit("r1-static-policy-observer", observer)}
(OUT / "source-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
for p, content in BASE.items():
    dest = OUT / "prestate" / p
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(content)
print(json.dumps(manifest, indent=2))
