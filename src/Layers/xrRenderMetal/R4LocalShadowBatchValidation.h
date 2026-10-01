#pragma once

#include "R4LocalShadowAtlasFoundation.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct R4LocalShadowBatchValidationDesc
{
    // Records are the current atlas batch in tile order. parentIds is parallel
    // metadata: each OmniFace has its shared point-light parent; spots use
    // their own stable light identity.
    std::vector<xray::metal::R4LocalShadowAtlasRecord> records;
    std::vector<std::uint64_t> parentIds;
    std::array<float, 16> view{};
    std::array<float, 16> projection{};
    std::array<float, 16> inverseView{};
    std::array<float, 4> positionDecompressionParams{};
    std::uint32_t engineFrame = 0u;
    std::uint32_t batchId = 0u;
    std::string outputDirectory;
};

// Appends blits to snapshot the production atlas, primary G-buffer position,
// depth/stencil and local-light accumulator. GPU analysis and artifact writes
// run from the completed command-buffer callback.
bool EncodeMetalR4LocalShadowBatchValidation(void* commandBuffer,
    void* atlasR32, void* positionRGBA16F, void* depthStencil,
    void* accumulatorRGBA16F, const R4LocalShadowBatchValidationDesc& descriptor,
    std::string& error);
