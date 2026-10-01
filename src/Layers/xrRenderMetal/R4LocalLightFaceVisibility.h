#pragma once

#include "IR4Backend.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace xray::metal
{
// Only an accumulator-only OmniFace may use the accumulator mesh as its
// contribution bound. A volumetric cone can extend beyond that mesh, and HUD
// sources follow a separate projection contract. Unknown input stays visible.
inline bool R4LocalLightFaceMayBeCulled(const R4EngineLocalLight& light,
    const R4EnvironmentSnapshot& environment)
{
    if (light.type != R4EngineLocalLightType::OmniFace || !light.active ||
        !light.shadowed || light.hudMode || !environment.valid)
        return false;
    for (const float value : environment.ssfxVolumetric)
        if (!std::isfinite(value)) return false;
    return !environment.localVolumetricEnabled || environment.volumetricMode == 1 ||
        (!light.volumetricEnabled && !(environment.ssfxVolumetric[0] > 0.0f));
}

// The caller supplies the padded world AABB of the real accumulator mesh.
// All eight corners must lie strictly outside one homogeneous clip plane.
// Homogeneous halfspaces remain valid for negative W and near-plane crossing:
// any one separating plane proves no contained triangle can rasterize.
inline bool R4LocalLightFaceBoundsOutsideView(const std::array<float, 3>& minimum,
    const std::array<float, 3>& maximum, const std::array<float, 16>& viewProjection)
{
    for (unsigned axis = 0; axis < 3; ++axis)
        if (!std::isfinite(minimum[axis]) || !std::isfinite(maximum[axis]) ||
            minimum[axis] > maximum[axis]) return false;
    for (const float value : viewProjection)
        if (!std::isfinite(value)) return false;
    std::array<bool, 6> outside{true, true, true, true, true, true};
    for (unsigned corner = 0; corner < 8; ++corner)
    {
        const std::array<double, 4> position{
            corner & 1u ? maximum[0] : minimum[0],
            corner & 2u ? maximum[1] : minimum[1],
            corner & 4u ? maximum[2] : minimum[2], 1.0};
        std::array<double, 4> clip{}, magnitude{};
        for (unsigned row = 0; row < 4; ++row)
            for (unsigned column = 0; column < 4; ++column)
            {
                const double term = position[column] * viewProjection[column * 4u + row];
                clip[row] += term;
                magnitude[row] += std::abs(term);
            }
        // GPU float matrix products may cancel large translation terms. Bound
        // roundoff from those terms, not only the small resulting clip W. The
        // factor also covers CPU model/WVP multiplication and its reassociation.
        std::array<double, 4> roundoff{};
        for (unsigned row = 0; row < 4; ++row)
        {
            if (!std::isfinite(clip[row]) || !std::isfinite(magnitude[row]) ||
                magnitude[row] > std::numeric_limits<float>::max() / 128.0) return false;
            roundoff[row] = 64.0 * std::numeric_limits<float>::epsilon() * magnitude[row];
        }
        const double base = 1.e-5 * std::max(1.0, std::abs(clip[3]));
        const double xMargin = base + roundoff[0] + roundoff[3];
        const double yMargin = base + roundoff[1] + roundoff[3];
        const double zMargin = base + roundoff[2] + roundoff[3];
        outside[0] = outside[0] && clip[0] < -clip[3] - xMargin;
        outside[1] = outside[1] && clip[0] > clip[3] + xMargin;
        outside[2] = outside[2] && clip[1] < -clip[3] - yMargin;
        outside[3] = outside[3] && clip[1] > clip[3] + yMargin;
        outside[4] = outside[4] && clip[2] < -base - roundoff[2];
        outside[5] = outside[5] && clip[2] > clip[3] + zMargin;
    }
    return std::any_of(outside.begin(), outside.end(), [](bool value) { return value; });
}
} // namespace xray::metal
