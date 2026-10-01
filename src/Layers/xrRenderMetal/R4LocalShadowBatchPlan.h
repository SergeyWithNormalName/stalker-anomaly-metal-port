#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xray::metal
{
inline constexpr std::size_t kR4LocalShadowBatchCapacity = 16u;

// An adapter-independent reference to one captured local light. `lightIndex`
// addresses the original snapshot. Omni faces share `parentId`; `lightId` is
// an opaque, unique light identity and is never decoded by the planner.
struct R4LocalShadowBatchCandidate
{
    std::size_t lightIndex = 0u;
    std::uint64_t lightId = 0u;
    std::uint64_t parentId = 0u;
    std::uint8_t face = 0xffu;
    bool omni = false;
};

// Groups all six faces of every point light atomically, then packs whole light
// groups into deterministic batches. On failure, `batches` is empty.
bool PlanR4LocalShadowBatches(const std::vector<R4LocalShadowBatchCandidate>& candidates,
    std::vector<std::vector<std::size_t>>& batches, std::string& error,
    std::size_t capacity = kR4LocalShadowBatchCapacity);

bool RunR4LocalShadowBatchPlanSelfTest(std::string& error);
} // namespace xray::metal
