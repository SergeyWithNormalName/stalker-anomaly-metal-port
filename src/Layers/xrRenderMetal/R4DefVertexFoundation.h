#pragma once

#include "R4LevelGeometry.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace xray::metal
{
// The exact packed static input used by l05_bar's def_shaders\def_vertex and
// def_shaders\def_aref_v
// records and the reflected deffer_base_flat USE_R2_STATIC_SUN vertex shader.
struct R4DefVertexPackedVertex
{
    float position[3];
    std::uint8_t normal[4];
    std::uint8_t tangent[4];
    std::uint8_t binormal[4];
    std::uint8_t color[4];
    std::int16_t texcoord[2];
};
static_assert(sizeof(R4DefVertexPackedVertex) == 32u,
    "def_vertex's P3/C4/C4/C4/C4/S2 source ABI must remain tightly packed");

struct R4DefVertexCoverage
{
    std::uint64_t visualCount = 0;
    std::uint64_t vertexCount = 0;
    std::uint64_t indexCount = 0;
    std::uint64_t rejectedVertexAbi = 0;
};

bool IsR4DefVertexShaderFamily(const std::string& shaderName);
bool ValidateR4DefVertexPackedVertexAbi(const R4VertexBufferSummary& vertexBuffer,
    std::string& error);
bool CollectR4DefVertexCoverage(const R4LevelGeometry& geometry,
    R4DefVertexCoverage& coverage, std::string& error);

// Structural gate only: validates the production converted flat/static-color
// and alpha-tested bundle file pairing, reflection, and exact TLAB/vertex contract.
bool ValidateR4DefVertexFlatMscBundle(const std::filesystem::path& shaderDirectory,
    std::string& error);

bool RunR4DefVertexFoundationSelfTest(std::string& error);
} // namespace xray::metal
