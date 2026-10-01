#pragma once

#include "IR4Backend.h"
#include "R4LocalShadowBatchPlan.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xray::metal
{
struct R4WholeLightVisibilitySelectionStats
{
    std::size_t plannedGroups = 0;
    std::size_t eligibleGroups = 0;
    std::size_t selectedGroups = 0;
    std::size_t selectedRecords = 0;
    std::size_t omittedByQuota = 0;
    std::size_t incompleteOrCollision = 0;
    std::size_t hudExemptRecords = 0;
    std::size_t volumetricExemptRecords = 0;
    std::size_t invalidExemptRecords = 0;
};

// Selects complete, already-planned shadow groups for diagnostic sampling.
// It never changes the input batches and its output is metadata only.
bool SelectR4WholeLightVisibilitySamples(const R4EngineLocalLightSnapshot& snapshot,
    const R4EnvironmentSnapshot& environment,
    const std::vector<std::vector<std::size_t>>& finalBatches,
    std::uint32_t firstFrame, std::uint32_t engineFrame,
    std::vector<R4WholeLightVisibilitySample>& samples,
    R4WholeLightVisibilitySelectionStats& stats, std::string& error);

bool RunR4WholeLightVisibilitySelectionSelfTest(std::string& error);
} // namespace xray::metal
