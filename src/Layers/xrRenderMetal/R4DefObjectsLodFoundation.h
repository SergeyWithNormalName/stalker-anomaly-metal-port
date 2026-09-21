#pragma once

#include "R4LevelGeometry.h"

#include <filesystem>
#include <string>

namespace xray::metal
{
// `def_objects_lod` is B_VERT static geometry, not FLOD's generated 80-byte
// impostor stream: it is the exact P3/C4/C4/C4/SHORT4 level-buffer layout.
bool IsR4DefObjectsLodShaderFamily(const std::string& shaderName);
bool ValidateR4DefObjectsLodVertexAbi(const R4VertexBufferSummary& vertexBuffer,
    std::string& error);

// Structural gate for the non-LMH / non-static-sun deffer_base_flat pair.
// The B_VERT texture metadata still selects flat versus bump per material, so
// this gate must not be used to admit an unclassified bump material.
bool ValidateR4DefObjectsLodFlatMscBundle(const std::filesystem::path& shaderDirectory,
    std::string& error);
bool ValidateR4DefObjectsLodBumpMscBundle(const std::filesystem::path& shaderDirectory,
    std::string& error);
// Exact B_VERT steep-parallax HQ pair: deffer_base_bump-hq.vs plus
// deffer_base_steep-hq.ps. It requires s_bumpX and static_globals b2.
bool ValidateR4DefObjectsLodSteepMscBundle(const std::filesystem::path& shaderDirectory,
    std::string& error);

bool RunR4DefObjectsLodFoundationSelfTest(std::string& error);
} // namespace xray::metal
