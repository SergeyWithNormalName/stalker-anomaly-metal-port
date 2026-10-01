#pragma once

#include "IR4Backend.h"
#include <array>
#include <cstdint>
#include <vector>

// No result from this diagnostic may change admission or atlas planning.
// Raster lifetime is the engine's HOM owner; run synchronously on the render
// thread with no legacy HOM scheduler. Each call rebuilds the raster for its
// exact camera, including second-viewport rendering in the same engine frame.
struct R4LightHomDiagnosticFrame
{
    bool enabled = false;
    bool ready = false;
    std::uint32_t frame = UINT32_MAX;
    std::uint64_t sample = 0;
    std::vector<std::uint8_t> hidden;
    std::vector<std::uint8_t> eligible;
    std::size_t CountHidden(const std::vector<std::size_t>& indices, bool eligibleOnly) const;
};

bool MetalR4LightHomDiagnosticEnabled();
R4LightHomDiagnosticFrame BeginMetalR4LightHomDiagnostic(
    const std::array<float,16>& view, const std::array<float,16>& projection,
    const std::array<float,3>& eye, bool cameraValid, std::uint32_t frame,
    const R4EngineLocalLightSnapshot* snapshot, const R4EnvironmentSnapshot& environment,
    const std::vector<std::vector<std::size_t>>& batches, const std::vector<std::size_t>& unshadowed);
