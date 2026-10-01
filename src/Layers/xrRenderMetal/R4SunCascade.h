#pragma once
#include <array>
#include <cstdint>
#include <vector>

struct R4SunCascade
{
    float size = 0.0f;
    float bias = 0.0f;
    bool resetChain = false;
    std::array<float, 16> lightViewProjection{};
    // Original R4 accumulation cuboid, world coordinates. Far uses the middle
    // cascade inverse; these are light-map footprints, not camera-Z splits.
    std::array<std::array<float, 4>, 8> receiverVolume{};
    std::vector<std::array<float, 4>> casterPlanes;
    // Rays actually selected as this cascade's inputs, before caster clipping
    // advances their origins for the next cascade.
    std::array<std::array<float, 3>, 4> initialRayOrigins{};
    std::array<std::array<float, 3>, 4> nextRayOrigins{};
};
struct R4SunCascadeSet
{
    std::array<R4SunCascade, 3> cascades;
    std::uint32_t engineFrame = 0;
    std::uint32_t resolution = 2048;
    bool valid = false;
};
