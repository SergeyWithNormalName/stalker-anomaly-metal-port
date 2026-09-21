#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace xray::metal
{
inline constexpr std::size_t R4ControlledMaterialBatchLimit = 64;

struct R4ControlledMaterial
{
    std::uint32_t visualId = 0;
    std::filesystem::path baseDdsPath;
    std::filesystem::path hemiDdsPath;
};

// Admission is deliberately structural and file-system independent.  The
// level loader subsequently proves that each path matches the chosen visual's
// shader texture slots and vertex declaration.
bool ValidateR4ControlledMaterialSet(
    const std::vector<R4ControlledMaterial>& materials,
    std::size_t maximumMaterials,
    std::string& error);

std::size_t CountUniqueR4ControlledMaterialTexturePaths(
    const std::vector<R4ControlledMaterial>& materials);

bool RunR4ControlledMaterialSetSelfTest(std::string& error);
} // namespace xray::metal
