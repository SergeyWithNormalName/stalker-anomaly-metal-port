#include "R4MaterialSet.h"

#include <limits>
#include <unordered_set>

namespace xray::metal
{
bool ValidateR4ControlledMaterialSet(
    const std::vector<R4ControlledMaterial>& materials,
    std::size_t maximumMaterials,
    std::string& error)
{
    if (maximumMaterials == 0 || materials.empty() || materials.size() > maximumMaterials)
    {
        error = "controlled material set size is outside its explicit admission limit";
        return false;
    }
    std::unordered_set<std::uint32_t> visualIds;
    for (const R4ControlledMaterial& material : materials)
    {
        if (material.visualId == std::numeric_limits<std::uint32_t>::max() ||
            material.baseDdsPath.empty() || material.hemiDdsPath.empty())
        {
            error = "controlled material requires a visual id, base DDS, and hemi DDS";
            return false;
        }
        if (!visualIds.emplace(material.visualId).second)
        {
            error = "controlled material set contains a duplicate visual id";
            return false;
        }
    }
    return true;
}

std::size_t CountUniqueR4ControlledMaterialTexturePaths(
    const std::vector<R4ControlledMaterial>& materials)
{
    std::unordered_set<std::string> paths;
    for (const R4ControlledMaterial& material : materials)
    {
        paths.emplace(material.baseDdsPath.lexically_normal().generic_string());
        paths.emplace(material.hemiDdsPath.lexically_normal().generic_string());
    }
    return paths.size();
}

bool RunR4ControlledMaterialSetSelfTest(std::string& error)
{
    const std::vector<R4ControlledMaterial> valid = {
        {7364, "base_a.dds", "hemi_a.dds"},
        {7365, "base_b.dds", "hemi_b.dds"},
    };
    if (!ValidateR4ControlledMaterialSet(valid, R4ControlledMaterialBatchLimit, error))
        return false;
    std::vector<R4ControlledMaterial> duplicate = valid;
    duplicate[1].visualId = duplicate[0].visualId;
    if (ValidateR4ControlledMaterialSet(duplicate, R4ControlledMaterialBatchLimit, error))
    {
        error = "duplicate visual self-test was accepted";
        return false;
    }
    std::vector<R4ControlledMaterial> missingHemi = valid;
    missingHemi[1].hemiDdsPath.clear();
    if (ValidateR4ControlledMaterialSet(missingHemi, R4ControlledMaterialBatchLimit, error))
    {
        error = "missing hemi self-test was accepted";
        return false;
    }
    if (ValidateR4ControlledMaterialSet(valid, 1, error))
    {
        error = "oversized material set self-test was accepted";
        return false;
    }
    std::vector<R4ControlledMaterial> batch;
    batch.reserve(R4ControlledMaterialBatchLimit);
    for (std::uint32_t index = 0; index < R4ControlledMaterialBatchLimit; ++index)
    {
        batch.push_back({10000u + index,
            "shared_base_" + std::to_string(index % 4u) + ".dds",
            "shared_hemi_" + std::to_string(index % 4u) + ".dds"});
    }
    if (!ValidateR4ControlledMaterialSet(batch, R4ControlledMaterialBatchLimit, error) ||
        CountUniqueR4ControlledMaterialTexturePaths(batch) != 8u)
    {
        error = "64-material shared-texture self-test failed";
        return false;
    }
    batch.push_back({10000u + R4ControlledMaterialBatchLimit,
        "overflow_base.dds", "overflow_hemi.dds"});
    if (ValidateR4ControlledMaterialSet(batch, R4ControlledMaterialBatchLimit, error))
    {
        error = "65-material overflow self-test was accepted";
        return false;
    }
    error.clear();
    return true;
}
} // namespace xray::metal
