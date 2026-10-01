#pragma once

#include "IR4Resources.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct R4TemporalTextureValidationDesc
{
    std::vector<std::shared_ptr<IR4Texture>> textures;
    std::vector<std::string> labels;
    std::uint32_t engineFrame = 0;
    std::uint32_t previousFrame = 0;
    std::string phase;
    std::string outputDirectory;
    bool expectZero = false;
    bool expectRgbZero = false;
    bool expectAlphaOne = false;
    bool expectFinite = false;
    bool writeImages = false;
    bool expectEqualPair = false;
    bool comparePair = false;
    bool expectDifferentPair = false;
};

bool EncodeMetalR4TemporalTextureValidation(void* commandBuffer,
    const std::vector<void*>& nativeTextures,
    const R4TemporalTextureValidationDesc& descriptor, std::string& error);
