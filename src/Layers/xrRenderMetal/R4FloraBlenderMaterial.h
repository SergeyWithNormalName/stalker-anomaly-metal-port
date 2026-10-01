#pragma once

#include <string>

// Value-only subset of a serialized B_TREE blender used by the Metal Flora
// adapter. The engine reader stays out of adapter/probe headers.
struct R4FloraBlenderMaterial
{
    std::string shaderName;
    bool alphaBlend = false;
    bool objectLod = false;
};
