#pragma once

#include "IR4Backend.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace xray::metal
{
constexpr std::size_t R4WholeLightVisibilityMaxSlots = 48u;
constexpr std::size_t R4WholeLightVisibilityMaxGroups = 8u;

struct R4WholeLightVisibilityRecord
{
    R4WholeLightVisibilitySample sample;
    std::uint32_t slot = 0u;
    bool encoded = false;
};

struct R4WholeLightVisibilityGroupResult
{
    std::uint32_t engineFrame = 0u;
    std::uint64_t cpuObservationScopeId = 0u;
    std::int32_t batchId = -1;
    std::uint64_t lightId = 0u;
    std::uint64_t groupId = 0u;
    R4WholeLightVisibilityGroupKind groupKind = R4WholeLightVisibilityGroupKind::SingleRecord;
    std::uint8_t expectedFaceCount = 0u;
    bool complete = false;
    std::uint64_t passedSamples = 0u;
};

inline bool R4WholeLightVisibilitySampleValid(const R4WholeLightVisibilitySample& s)
{
    if (!s.lightId || s.batchId < 0)
        return false;
    if (s.groupKind == R4WholeLightVisibilityGroupKind::SingleRecord)
        return s.face == 0u && s.expectedFaceCount == 1u;
    return s.groupKind == R4WholeLightVisibilityGroupKind::OmniParent &&
        s.parentLightId != 0u && s.expectedFaceCount == 6u && s.face < 6u;
}

inline std::vector<R4WholeLightVisibilityGroupResult> R4ClassifyWholeLightVisibility(
    const std::vector<R4WholeLightVisibilityRecord>& records,
    const std::uint64_t* values, const std::size_t valueCount,
    const bool commandCompleted)
{
    std::vector<R4WholeLightVisibilityGroupResult> groups;
    for (const auto& record : records)
    {
        const auto& sample = record.sample;
        const std::uint64_t groupKey = sample.groupKind == R4WholeLightVisibilityGroupKind::OmniParent ?
            sample.parentLightId : sample.lightId;
        auto found = std::find_if(groups.begin(), groups.end(), [&](const auto& group) {
            return group.groupId == groupKey && group.groupKind == sample.groupKind;
        });
        if (found == groups.end())
        {
            groups.push_back({sample.engineFrame, sample.cpuObservationScopeId,
                sample.batchId, sample.lightId, groupKey,
                sample.groupKind, sample.expectedFaceCount, false, 0u});
        }
    }
    bool slotsValid = true;
    std::uint64_t slotMask = 0u;
    bool lightIdsValid = true;
    for (std::size_t i = 0; i < records.size(); ++i)
    {
        const auto& record = records[i];
        if (record.slot >= R4WholeLightVisibilityMaxSlots || record.slot >= valueCount ||
            record.slot >= 64u)
        {
            slotsValid = false;
            continue;
        }
        const std::uint64_t bit = 1ull << record.slot;
        slotsValid = slotsValid && (slotMask & bit) == 0u;
        slotMask |= bit;
        lightIdsValid = lightIdsValid && record.sample.lightId != 0u;
        for (std::size_t prior = 0; prior < i; ++prior)
            lightIdsValid = lightIdsValid &&
                records[prior].sample.lightId != record.sample.lightId;
    }
    bool globalValid = slotsValid && lightIdsValid &&
        records.size() <= R4WholeLightVisibilityMaxSlots &&
        groups.size() <= R4WholeLightVisibilityMaxGroups && values != nullptr &&
        valueCount <= R4WholeLightVisibilityMaxSlots;
    for (auto& group : groups)
    {
        std::uint32_t faceMask = 0u;
        std::size_t rowCount = 0u;
        bool valid = commandCompleted && globalValid && group.expectedFaceCount > 0u &&
            group.expectedFaceCount <= 6u;
        std::uint64_t total = 0u;
        for (const auto& record : records)
        {
            const auto& sample = record.sample;
            const std::uint64_t key = sample.groupKind == R4WholeLightVisibilityGroupKind::OmniParent ?
                sample.parentLightId : sample.lightId;
            if (key != group.groupId || sample.groupKind != group.groupKind)
                continue;
            if (rowCount > 0u && sample.batchId != group.batchId)
                group.batchId = -1;
            ++rowCount;
            const bool faceInMaskRange = sample.face < 6u;
            valid = valid && sample.engineFrame == group.engineFrame &&
                sample.cpuObservationScopeId == group.cpuObservationScopeId &&
                sample.expectedFaceCount == group.expectedFaceCount && record.encoded &&
                R4WholeLightVisibilitySampleValid(sample) && faceInMaskRange &&
                record.slot < valueCount && record.slot < R4WholeLightVisibilityMaxSlots;
            if (!faceInMaskRange)
                continue;
            const std::uint32_t bit = 1u << sample.face;
            valid = valid && (faceMask & bit) == 0u;
            faceMask |= bit;
            if (record.encoded && values && record.slot < valueCount &&
                record.slot < R4WholeLightVisibilityMaxSlots)
            {
                const std::uint64_t value = values[record.slot];
                total = value > std::numeric_limits<std::uint64_t>::max() - total ?
                    std::numeric_limits<std::uint64_t>::max() : total + value;
            }
        }
        const std::uint32_t expectedMask = group.expectedFaceCount > 0u && group.expectedFaceCount <= 6u ?
            ((1u << group.expectedFaceCount) - 1u) : 0u;
        group.complete = valid && rowCount == group.expectedFaceCount && faceMask == expectedMask;
        if (group.complete)
            group.passedSamples = total;
    }
    return groups;
}
} // namespace xray::metal
