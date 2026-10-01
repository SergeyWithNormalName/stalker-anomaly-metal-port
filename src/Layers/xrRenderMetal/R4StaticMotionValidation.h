#pragma once

#include "IR4Resources.h"
#include <array>
#include <cstdint>
#include <memory>
#include <string>

struct R4StaticMotionValidationDesc
{
    std::shared_ptr<IR4Texture> depth;
    std::shared_ptr<IR4Texture> motion;
    std::array<float, 16> currentViewProjection{};
    std::array<float, 16> previousViewProjection{};
    std::string family;
    std::string outputDirectory;
    std::uint32_t engineFrame = 0;
    std::uint32_t previousFrame = 0;
    bool reset = false;
};

// Objective-C++ implementation accepts opaque native Metal handles, keeping
// Metal types outside the renderer's value-only public interface.
bool EncodeMetalR4StaticMotionValidation(void* commandBuffer, void* depth,
    void* motion, const R4StaticMotionValidationDesc& descriptor, std::string& error);
