#include "R4LocalShadowBatchPlan.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <tuple>

namespace xray::metal
{
namespace
{
struct LightGroup
{
    std::uint64_t stableId = 0u;
    std::uint64_t firstLightId = 0u;
    bool omni = false;
    std::vector<std::size_t> lightIndices;
};

void setError(std::string& error, const char* message)
{
    error = message;
}

bool failPlan(std::vector<std::vector<std::size_t>>& batches,
    std::string& error, const char* message)
{
    batches.clear();
    setError(error, message);
    return false;
}
} // namespace

bool PlanR4LocalShadowBatches(const std::vector<R4LocalShadowBatchCandidate>& candidates,
    std::vector<std::vector<std::size_t>>& batches, std::string& error,
    const std::size_t capacity)
{
    batches.clear();
    error.clear();
    if (capacity == 0u)
        return failPlan(batches, error, "local shadow batch capacity is zero");

    std::set<std::size_t> uniqueIndices;
    std::set<std::uint64_t> uniqueLightIds;
    std::map<std::uint64_t, std::array<const R4LocalShadowBatchCandidate*, 6>> omniFaces;
    std::map<std::uint64_t, std::uint64_t> omniFirstLightIds;
    std::vector<LightGroup> groups;
    groups.reserve(candidates.size());

    for (const R4LocalShadowBatchCandidate& candidate : candidates)
    {
        if (!uniqueIndices.insert(candidate.lightIndex).second)
            return failPlan(batches, error, "duplicate local light snapshot index");
        if (!uniqueLightIds.insert(candidate.lightId).second)
            return failPlan(batches, error, "duplicate local light identity");

        if (!candidate.omni)
        {
            LightGroup group;
            group.stableId = candidate.lightId;
            group.firstLightId = candidate.lightId;
            group.lightIndices.push_back(candidate.lightIndex);
            groups.push_back(std::move(group));
            continue;
        }

        if (candidate.face >= 6u)
            return failPlan(batches, error, "OmniFace index is outside [0, 5]");
        auto [facesIt, inserted] = omniFaces.try_emplace(candidate.parentId);
        if (inserted)
        {
            facesIt->second.fill(nullptr);
            omniFirstLightIds.emplace(candidate.parentId, candidate.lightId);
        }
        if (facesIt->second[candidate.face])
            return failPlan(batches, error, "duplicate OmniFace for one parent light");
        facesIt->second[candidate.face] = &candidate;
    }

    for (const auto& [parentId, faces] : omniFaces)
    {
        LightGroup group;
        group.stableId = parentId;
        group.firstLightId = omniFirstLightIds.at(parentId);
        group.omni = true;
        group.lightIndices.reserve(6u);
        for (std::size_t face = 0u; face < faces.size(); ++face)
        {
            if (!faces[face])
                return failPlan(batches, error, "point-light OmniFace group is incomplete");
            group.lightIndices.push_back(faces[face]->lightIndex);
        }
        groups.push_back(std::move(group));
    }

    for (const LightGroup& group : groups)
        if (group.lightIndices.size() > capacity)
            return failPlan(batches, error, "atomic point-light group exceeds batch capacity");

    std::sort(groups.begin(), groups.end(), [](const LightGroup& left, const LightGroup& right)
    {
        if (left.lightIndices.size() != right.lightIndices.size())
            return left.lightIndices.size() > right.lightIndices.size();
        return std::tie(left.stableId, left.omni, left.firstLightId) <
            std::tie(right.stableId, right.omni, right.firstLightId);
    });

    std::vector<std::size_t> batchCounts;
    std::vector<std::vector<std::vector<std::size_t>>> packedGroups;
    for (const LightGroup& group : groups)
    {
        std::size_t batchIndex = 0u;
        for (; batchIndex < batchCounts.size(); ++batchIndex)
            if (batchCounts[batchIndex] + group.lightIndices.size() <= capacity)
                break;
        if (batchIndex == batchCounts.size())
        {
            batchCounts.push_back(0u);
            packedGroups.emplace_back();
        }
        batchCounts[batchIndex] += group.lightIndices.size();
        packedGroups[batchIndex].push_back(group.lightIndices);
    }

    batches.reserve(packedGroups.size());
    for (const auto& batchGroups : packedGroups)
    {
        std::vector<std::size_t> batch;
        for (const auto& groupIndices : batchGroups)
            batch.insert(batch.end(), groupIndices.begin(), groupIndices.end());
        batches.push_back(std::move(batch));
    }
    return true;
}

bool RunR4LocalShadowBatchPlanSelfTest(std::string& error)
{
    const auto candidateForFace = [](const std::uint64_t parent, const std::uint8_t face,
        const std::size_t index)
    {
        // Deliberately unrelated to the production bit-packed lightId scheme:
        // grouping must depend only on the explicit parentId and face values.
        const std::uint64_t opaqueId = 0xf000000000000000ull |
            (parent << 8u) | std::uint64_t(0x80u + face);
        return R4LocalShadowBatchCandidate{index, opaqueId, parent, face, true};
    };
    std::vector<R4LocalShadowBatchCandidate> candidates;
    for (std::uint8_t face = 0u; face != 6u; ++face)
        candidates.push_back(candidateForFace(21u, face, std::size_t(face)));
    for (std::uint8_t face = 0u; face != 6u; ++face)
        candidates.push_back(candidateForFace(7u, face, 6u + std::size_t(face)));
    for (std::size_t index = 0u; index != 17u; ++index)
    {
        const std::uint64_t lightId = 1000u + index;
        candidates.push_back({12u + index, lightId, lightId, 0xffu, false});
    }

    std::vector<std::vector<std::size_t>> planned;
    if (!PlanR4LocalShadowBatches(candidates, planned, error))
        return false;
    if (planned.size() < 2u)
    {
        error = "planner dropped the capacity overflow instead of starting another batch";
        return false;
    }
    std::map<std::size_t, const R4LocalShadowBatchCandidate*> byIndex;
    for (const auto& candidate : candidates)
        byIndex.emplace(candidate.lightIndex, &candidate);
    std::set<std::size_t> seen;
    std::map<std::uint64_t, std::size_t> omniBatch;
    for (std::size_t batchIndex = 0u; batchIndex < planned.size(); ++batchIndex)
    {
        if (planned[batchIndex].empty() || planned[batchIndex].size() > kR4LocalShadowBatchCapacity)
        {
            error = "planner produced an empty or over-capacity batch";
            return false;
        }
        for (const std::size_t lightIndex : planned[batchIndex])
        {
            const auto found = byIndex.find(lightIndex);
            if (found == byIndex.end() || !seen.insert(lightIndex).second)
            {
                error = "planned snapshot index is missing or appears more than once";
                return false;
            }
            const auto& candidate = *found->second;
            if (candidate.omni)
            {
                const auto [position, inserted] = omniBatch.emplace(candidate.parentId, batchIndex);
                if (!inserted && position->second != batchIndex)
                {
                    error = "point-light faces were split across batches";
                    return false;
                }
            }
        }
    }
    if (seen.size() != candidates.size())
    {
        error = "planner did not preserve every candidate";
        return false;
    }

    // Shuffle the input sequence while preserving each snapshot index. Planning
    // must not change batch boundaries or the stable face order.
    std::vector<R4LocalShadowBatchCandidate> reordered(candidates.rbegin(), candidates.rend());
    std::vector<std::vector<std::size_t>> reorderedPlan;
    if (!PlanR4LocalShadowBatches(reordered, reorderedPlan, error) || reorderedPlan != planned)
    {
        error = "batch plan depends on capture input order";
        return false;
    }

    std::vector<R4LocalShadowBatchCandidate> incomplete(candidates.begin(), candidates.begin() + 5);
    std::vector<std::vector<std::size_t>> invalidOutput{{999u}};
    if (PlanR4LocalShadowBatches(incomplete, invalidOutput, error) || !invalidOutput.empty())
    {
        error = "incomplete OmniFace group was accepted or left partial output";
        return false;
    }

    std::vector<R4LocalShadowBatchCandidate> duplicateFace;
    for (std::uint8_t face = 0u; face != 6u; ++face)
        duplicateFace.push_back(candidateForFace(31u, face, std::size_t(face)));
    duplicateFace.back().face = 4u;
    if (PlanR4LocalShadowBatches(duplicateFace, invalidOutput, error) || !invalidOutput.empty())
    {
        error = "duplicate OmniFace group member was accepted";
        return false;
    }

    std::vector<R4LocalShadowBatchCandidate> tooSmall;
    for (std::uint8_t face = 0u; face != 6u; ++face)
        tooSmall.push_back(candidateForFace(41u, face, std::size_t(face)));
    if (PlanR4LocalShadowBatches(tooSmall, invalidOutput, error, 5u) || !invalidOutput.empty())
    {
        error = "planner split an atomic point-light group to fit capacity";
        return false;
    }

    error.clear();
    return true;
}
} // namespace xray::metal
