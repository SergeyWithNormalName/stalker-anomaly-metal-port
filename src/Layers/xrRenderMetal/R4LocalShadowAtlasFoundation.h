#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace xray::metal
{
// Value-only handoff after R4 LP_smap_pool allocation and phase_smap_S depth
// rendering. `atlasSize` is L->X.S.size; its one-texel border is part of the
// sampling contract, not a Metal-specific convention.
struct R4LocalShadowAtlasRecord
{
    std::uint64_t lightIdentity = 0;
    std::uint32_t engineFrame = 0;
    std::uint32_t smapId = 0;
    std::uint32_t atlasX = 0, atlasY = 0, atlasSize = 0, atlasExtent = 0;
    std::uint32_t face = 0; // spot: 0; point: exact cube face 0..5
    bool renderedDepth = false;
    bool translucent = false;
    std::array<float, 16> lightView{};
    std::array<float, 16> lightProjection{};
    std::array<float, 16> deviceInverseView{};
    std::array<float, 16> shadowMatrix{};
    std::array<float, 16> lightMapMatrix{};
};

bool DeriveR4LocalShadowMatrix(const R4LocalShadowAtlasRecord& record,
    float depthScale, float depthBias, std::array<float, 16>& matrix, std::string& error);
bool DeriveR4LocalLightMapMatrix(const R4LocalShadowAtlasRecord& record,
    float depthScale, float depthBias, std::array<float, 16>& matrix, std::string& error);
bool ValidateR4LocalShadowAtlasRecord(const R4LocalShadowAtlasRecord& record,
    std::uint32_t expectedFrame, std::string& error);
bool RunR4LocalShadowAtlasFoundationSelfTest(std::string& error);
} // namespace xray::metal
