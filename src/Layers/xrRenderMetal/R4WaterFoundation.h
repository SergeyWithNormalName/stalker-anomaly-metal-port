#pragma once

#include "IR4Resources.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace xray::metal
{
// Exact non-MSAA R4 SSFX water intermediates. The reference allocates the
// first three at full scene extent and restricts the SSR/blur viewports; do
// not silently shrink the allocation itself. Waves are always 512².
struct R4WaterTargetContract
{
    R4PixelFormat format = R4PixelFormat::RGBA8Unorm;
    std::uint32_t wavesWidth = 512u;
    std::uint32_t wavesHeight = 512u;
    R4TextureUsage usage = R4TextureUsage::RenderTarget | R4TextureUsage::ShaderRead;
};

struct R4WaterPipelineContract
{
    const char* vertexMetallib = nullptr;
    const char* fragmentMetallib = nullptr;
    std::uint32_t vertexResourceCount = 0;
    std::uint32_t fragmentResourceCount = 0;
    std::size_t vertexTlabSize = 0;
    std::size_t fragmentTlabSize = 0;
};

R4WaterTargetContract GetR4WaterTargetContract();
R4WaterPipelineContract GetR4WaterSsrPipelineContract();
R4WaterPipelineContract GetR4WaterFinalPipelineContract();
R4WaterPipelineContract GetR4WaterBlurPipelineContract();
R4WaterPipelineContract GetR4WaterNoBlurPipelineContract();
R4WaterPipelineContract GetR4WaterWavesPipelineContract();

// Validates the exact seven artifacts and TLAB layouts produced by
// compile_r4_water_msc.sh. It does not create a PSO or mutate a render pass.
bool ValidateR4WaterMscBundle(const std::filesystem::path& shaderDirectory,
    std::string& error);
bool RunR4WaterFoundationSelfTest(std::string& error);
} // namespace xray::metal
