#!/usr/bin/env python3
"""Generate a scratch-only observer against the pinned R1 candidate source."""
from pathlib import Path
import difflib
import hashlib
import json

ROOT = Path(__file__).resolve().parents[4]
OUT = Path(__file__).resolve().parent
BASE_HASHES = {
    "src/Layers/xrRenderMetal/MetalRenderDeviceRender.mm": "12b16a921963c60f1b512abafddd7b4c7a37bf7f2f5c77b002136f43be085a0c",
    "src/Layers/xrRenderMetal/MetalR4EngineScene.mm": "3cf3d743d40aece9ffd1aa9fd61a1726840bcb8f31fd04e3fab70e7c1269322f",
}

HELPERS = r'''
// One bounded source-policy census. No rendering or live-light state is changed.
bool isR4ParentSphereObservationFrame(const std::uint32_t engineFrame)
{
    static const bool enabled = [] {
        const char* value = std::getenv("XRAY_METAL_PARENT_SPHERE_OBSERVE");
        return value && std::strcmp(value, "1") == 0;
    }();
    static const std::uint32_t frame = [] {
        const char* value = std::getenv("XRAY_METAL_PARENT_SPHERE_OBSERVE_FRAME");
        if (!value) return std::uint32_t(700u);
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        return end && *end == '\0' && parsed > 0u && parsed < UINT32_MAX ?
            static_cast<std::uint32_t>(parsed) : UINT32_MAX;
    }();
    return enabled && engineFrame == frame;
}

void printR4ParentSphereBits(const char* key, const void* values, const std::size_t count)
{
    std::fprintf(stderr, " %s=", key);
    for (std::size_t index = 0; index < count; ++index)
    {
        std::uint32_t bits = 0u;
        std::memcpy(&bits, static_cast<const unsigned char*>(values) + index * sizeof(float), sizeof(bits));
        std::fprintf(stderr, "%s%08x", index ? "." : "", bits);
    }
}
'''

DEVICE_OBSERVE = r'''
        if (isR4ParentSphereObservationFrame(lightingFrame.engineFrame))
        {
            Fmatrix capturedVP; capturedVP.set(Device.mFullTransform);
            Fmatrix cachedView, cachedProjection, cachedVP;
            std::memcpy(&cachedView, viewState.view.data(), sizeof(cachedView));
            std::memcpy(&cachedProjection, viewState.projection.data(), sizeof(cachedProjection));
            const bool finiteInput = _valid(capturedVP) && _valid(cachedView) && _valid(cachedProjection);
            bool sourceViewMatched = finiteInput && viewState.valid &&
                viewState.engineFrame == lightingFrame.engineFrame && Device.dwFrame == lightingFrame.engineFrame &&
                viewState.eye[0] == Device.vCameraPosition.x &&
                viewState.eye[1] == Device.vCameraPosition.y && viewState.eye[2] == Device.vCameraPosition.z;
            if (sourceViewMatched)
            {
                cachedVP.mul(cachedProjection, cachedView);
                sourceViewMatched = std::memcmp(&cachedVP, &capturedVP, sizeof(cachedVP)) == 0;
            }
            CFrustum sourceFrustum;
            if (sourceViewMatched)
                sourceFrustum.CreateFromMatrix(capturedVP, FRUSTUM_P_LRTB | FRUSTUM_P_FAR);
            std::fprintf(stderr,
                "METAL_PARENT_SPHERE_CAPTURE frame=%u device_frame=%u cache_frame=%u source_view_matched=%u "
                "allow_r1=%u r1_policy=%u",
                lightingFrame.engineFrame, Device.dwFrame, viewState.engineFrame, unsigned(sourceViewMatched),
                unsigned(ps_r2_ls_flags.test(R2FLAG_R1LIGHTS)), unsigned(r1StaticPolicy));
            printR4ParentSphereBits("device_vp_bits", &capturedVP, 16u);
            printR4ParentSphereBits("view_bits", viewState.view.data(), 16u);
            printR4ParentSphereBits("projection_bits", viewState.projection.data(), 16u);
            printR4ParentSphereBits("eye_bits", viewState.eye.data(), 3u);
            std::fprintf(stderr, "\n");
            std::unordered_map<std::uint64_t, std::size_t> recordsPerParent;
            for (const auto& light : lightingFrame.lights) ++recordsPerParent[light.parentLightId];
            std::unordered_set<std::uint64_t> reported;
            for (const auto& light : lightingFrame.lights)
            {
                if (!reported.emplace(light.parentLightId).second) continue;
                const auto owner = lightByParentIdentity.find(light.parentLightId);
                const bool hasSphere = owner != lightByParentIdentity.end() && owner->second &&
                    owner->second->SpatialComponent;
                Fvector center{}; float radius = 0.f;
                if (hasSphere)
                {
                    center.set(owner->second->SpatialComponent->spatial.sphere.P);
                    radius = owner->second->SpatialComponent->spatial.sphere.R;
                }
                const bool sphereValid = hasSphere && _valid(center) && std::isfinite(radius) && radius > 0.f;
                const int outside = sourceViewMatched && sphereValid ?
                    int(!sourceFrustum.testSphere_dirty(center, radius)) : -1;
                std::fprintf(stderr,
                    "METAL_PARENT_SPHERE_PARENT frame=%u parent=%llu source_static=%u captured_records=%zu "
                    "type=%u shadowed=%u hud=%u volumetric=%u sphere_valid=%u source_outside=%d "
                    "sphere_x=%.9g sphere_y=%.9g sphere_z=%.9g sphere_radius=%.9g\n",
                    lightingFrame.engineFrame, static_cast<unsigned long long>(light.parentLightId),
                    unsigned(light.staticLight), recordsPerParent.at(light.parentLightId), unsigned(light.type),
                    unsigned(light.shadowed), unsigned(light.hudMode), unsigned(light.volumetricEnabled),
                    unsigned(sphereValid), outside, center.x, center.y, center.z, radius);
            }
            std::fprintf(stderr, "METAL_PARENT_SPHERE_CAPTURE_DONE frame=%u parents=%zu records=%zu\n",
                lightingFrame.engineFrame, recordsPerParent.size(), lightingFrame.lights.size());
        }
'''

SCENE_VIEW = r'''
        if (isR4ParentSphereObservationFrame(view.engineFrame))
        {
            std::fprintf(stderr,
                "METAL_PARENT_SPHERE_VIEW frame=%u valid=%u snapshot_frame=%u environment_frame=%u "
                "environment_valid=%u scope_receivers=%u hud_receivers=%u volume_frame=%u",
                view.engineFrame, unsigned(view.valid), snapshot ? snapshot->engineFrame : UINT32_MAX,
                environment.engineFrame, unsigned(environment.valid), unsigned(hasScope),
                unsigned(hasOpaqueHud), unsigned(volumeFrame));
            printR4ParentSphereBits("view_bits", view.view.data(), 16u);
            printR4ParentSphereBits("projection_bits", view.projection.data(), 16u);
            printR4ParentSphereBits("eye_bits", view.eye.data(), 3u);
            std::fprintf(stderr, "\n");
        }
'''

SCENE_FACE = r'''
        if (isR4ParentSphereObservationFrame(view.engineFrame))
        {
            const auto& light = snapshot->lights[candidates[tile].lightIndex];
            std::fprintf(stderr,
                "METAL_PARENT_SPHERE_FACE frame=%u batch=%u tile=%zu parent=%llu light=%llu "
                "face=%u type=%u source_static=%u material_rows=%zu static_geometry_draws=%zu "
                "dynamic_geometry_casters=%zu flora_shadow_draws=%zu transform_bytes=%zu\n",
                view.engineFrame, batchId, tile, static_cast<unsigned long long>(light.parentLightId),
                static_cast<unsigned long long>(record.lightIdentity), record.face, unsigned(light.type),
                unsigned(light.staticLight), materialEntries[tile].size(),
                shadows.mergedEncodedDraws - homStaticBefore, shadows.submittedDynamicCasters - homDynamicBefore,
                flora && flora->IsReady() ? flora->LastTelemetry().shadowDraws : 0u,
                materialEntries[tile].size() * 256u);
        }
'''

def replace_once(text, old, new):
    assert text.count(old) == 1, (old[:100], text.count(old))
    return text.replace(old, new, 1)

patch_parts = []
manifest = {"schema": "parent-sphere-admission-observer/v1",
            "scope": "scratch-only-no-render-change",
            "baseline": "current R1 policy gate plus existing R1 observer; frozen R1 patches unchanged",
            "files": {}}
for relative, expected in BASE_HASHES.items():
    source = (ROOT / relative).read_bytes()
    assert hashlib.sha256(source).hexdigest() == expected, relative
    before = source.decode()
    after = before
    if relative.endswith("MetalRenderDeviceRender.mm"):
        after = replace_once(after, "namespace\n{\n// shared_str", "namespace\n{\n" + HELPERS + "\n// shared_str")
        after = replace_once(after, "\t\tif (!backend->SubmitEngineLightingFrame(lightingFrame, error))", DEVICE_OBSERVE + "\t\tif (!backend->SubmitEngineLightingFrame(lightingFrame, error))")
    else:
        after = replace_once(after, "constexpr R4ClearColor EngineSceneClearColor{0.025, 0.055, 0.105, 1.0};", "constexpr R4ClearColor EngineSceneClearColor{0.025, 0.055, 0.105, 1.0};\n" + HELPERS)
        after = replace_once(after, "        if (!planLocalShadowBatches(view,snapshot,environment,batches,unshadowed,error)) return false;", "        if (!planLocalShadowBatches(view,snapshot,environment,batches,unshadowed,error)) return false;\n" + SCENE_VIEW)
        after = replace_once(after, "        if (homDiagnostic)\n        {\n            const auto index=indices[tile];", SCENE_FACE + "        if (homDiagnostic)\n        {\n            const auto index=indices[tile];")
    original_path = OUT / "baseline" / relative
    draft_path = OUT / "draft" / relative
    original_path.parent.mkdir(parents=True, exist_ok=True)
    draft_path.parent.mkdir(parents=True, exist_ok=True)
    original_path.write_bytes(source)
    draft_path.write_text(after)
    patch_parts.extend(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                                           fromfile="a/" + relative, tofile="b/" + relative))
    manifest["files"][relative] = {"baseline_sha256": expected,
                                    "draft_sha256": hashlib.sha256(after.encode()).hexdigest()}
patch = "".join(patch_parts)
(OUT / "parent-sphere-observer.patch").write_text(patch)
manifest["patch_sha256"] = hashlib.sha256(patch.encode()).hexdigest()
(OUT / "source-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
print(json.dumps(manifest, indent=2))
