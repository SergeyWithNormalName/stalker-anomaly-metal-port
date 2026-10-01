#pragma once

#include "IR4Backend.h"
#include "MetalR4VolumetricAdapter.h"
#include "R4LocalShadowAtlasFoundation.h"

#include <array>
#include <string>

// Reconstruct the value-only OMNIPART fields that R4 light::export_() normally
// copies from a shadowed point-light parent. Direct Metal skips that D3D light
// export path. Seed parent enabled/quality/raw color in face before calling;
// this computes the source face sphere and LOD without changing
// or querying the engine-owned child object.
void CaptureMetalR4OmniFaceVolumetricValues(R4EngineLocalLight& face,
    const std::array<float, 3>& cameraPosition,
    const std::array<float, 4>& ssfxVolumetric,
    float slightFade, float lodStart, float lodEnd);

// Reproduces the engine-space setup in CRenderTarget::accum_volumetric from
// immutable light/shadow/view snapshots. A supported but inactive light is
// returned as a successful pass with sliceCount == 0.
bool BuildMetalR4VolumetricSpotPass(const R4EngineLocalLight& light,
    const R4EnvironmentSnapshot& environment,
    const xray::metal::R4LocalShadowAtlasRecord& shadowRecord,
    const std::array<float, 16>& acceptedView,
    const std::array<float, 16>& acceptedProjection,
    MetalR4VolumetricAdapter::SpotPass& output, std::string& error);
