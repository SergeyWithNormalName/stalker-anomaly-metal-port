#include "../R4WholeLightVisibilityLedger.h"

#include <array>
#include <cstdio>
#include <vector>

namespace
{
using namespace xray::metal;

R4WholeLightVisibilityRecord face(std::uint32_t index, std::int32_t batch,
    std::uint32_t slot, std::uint64_t cpuScope = 0u)
{
    R4WholeLightVisibilitySample sample;
    sample.engineFrame = 7u;
    sample.cpuObservationScopeId = cpuScope;
    sample.batchId = batch;
    sample.lightId = 900u + index;
    sample.parentLightId = 42u;
    sample.face = index;
    sample.groupKind = R4WholeLightVisibilityGroupKind::OmniParent;
    sample.expectedFaceCount = 6u;
    return {sample, slot, true};
}

bool omniAcrossBatches()
{
    std::vector<R4WholeLightVisibilityRecord> records;
    for (std::uint32_t i = 0; i < 6u; ++i)
        records.push_back(face(i, i < 3u ? 11 : 12, i));
    const std::array<std::uint64_t, 6> values{{1, 0, 4, 0, 2, 3}};
    const auto groups = R4ClassifyWholeLightVisibility(records, values.data(), values.size(), true);
    return groups.size() == 1u && groups[0].complete && groups[0].passedSamples == 10u &&
        groups[0].batchId == -1;
}

bool cpuObservationScopeCorrelation()
{
    constexpr std::uint64_t scope = 0x4350554f42530017ull;
    std::vector<R4WholeLightVisibilityRecord> consistent;
    for (std::uint32_t i = 0; i < 6u; ++i)
        consistent.push_back(face(i, i < 3u ? 21 : 22, i, scope));
    const std::array<std::uint64_t, 6> values{{1, 2, 3, 4, 5, 6}};
    auto groups = R4ClassifyWholeLightVisibility(consistent, values.data(), values.size(), true);
    if (groups.size() != 1u || !groups[0].complete ||
        groups[0].cpuObservationScopeId != scope)
        return false;

    consistent[4].sample.cpuObservationScopeId = scope + 1u;
    groups = R4ClassifyWholeLightVisibility(consistent, values.data(), values.size(), true);
    return groups.size() == 1u && !groups[0].complete &&
        groups[0].cpuObservationScopeId == scope && groups[0].passedSamples == 0u;
}

bool invalidCases()
{
    std::vector<R4WholeLightVisibilityRecord> first, second;
    for (std::uint32_t i = 0; i < 3u; ++i)
    {
        first.push_back(face(i, 1, i));
        second.push_back(face(i + 3u, 2, i));
    }
    const std::array<std::uint64_t, 6> values{{1, 1, 1, 1, 1, 1}};
    const auto firstHalf = R4ClassifyWholeLightVisibility(first, values.data(), values.size(), true);
    const auto secondHalf = R4ClassifyWholeLightVisibility(second, values.data(), values.size(), true);
    if (firstHalf.size() != 1u || firstHalf[0].complete ||
        secondHalf.size() != 1u || secondHalf[0].complete)
        return false;

    std::vector<R4WholeLightVisibilityRecord> duplicate;
    for (std::uint32_t i = 0; i < 5u; ++i)
        duplicate.push_back(face(i == 4u ? 3u : i, static_cast<int>(i & 1u), i));
    auto result = R4ClassifyWholeLightVisibility(duplicate, values.data(), values.size(), true);
    if (result.size() != 1u || result[0].complete)
        return false;
    result = R4ClassifyWholeLightVisibility(duplicate, values.data(), values.size(), false);
    if (result.size() != 1u || result[0].complete)
        return false;
    duplicate.pop_back();
    duplicate.pop_back();
    result = R4ClassifyWholeLightVisibility(duplicate, values.data(), values.size(), true);
    if (result.size() != 1u || result[0].complete)
        return false;
    std::vector<R4WholeLightVisibilityRecord> aliasedSlots;
    for (std::uint32_t i = 0; i < 6u; ++i)
        aliasedSlots.push_back(face(i, static_cast<int>(i), i == 5u ? 4u : i));
    result = R4ClassifyWholeLightVisibility(aliasedSlots, values.data(), values.size(), true);
    if (result.size() != 1u || result[0].complete)
        return false;
    std::vector<R4WholeLightVisibilityRecord> collidingIds;
    const std::array<std::uint64_t, 12> collisionValues{};
    for (std::uint32_t group = 0u; group < 2u; ++group)
        for (std::uint32_t i = 0u; i < 6u; ++i)
        {
            auto record = face(i, static_cast<int>(group), group * 6u + i);
            record.sample.parentLightId = 42u + group;
            record.sample.lightId = 100u + group * 10u + i;
            if (group == 1u && i == 5u)
                record.sample.lightId = 100u; // duplicate ID across two otherwise complete parents
            collidingIds.push_back(record);
        }
    const auto colliding = R4ClassifyWholeLightVisibility(collidingIds,
        collisionValues.data(), collisionValues.size(), true);
    if (colliding.size() != 2u || colliding[0].complete || colliding[1].complete)
        return false;

    auto invalid = std::vector<R4WholeLightVisibilityRecord>{face(255u, 1, 0u)};
    result = R4ClassifyWholeLightVisibility(invalid, values.data(), values.size(), true);
    if (result.size() != 1u || result[0].complete)
        return false;
    auto negativeBatch = face(0u, -1, 0u);
    invalid = {negativeBatch};
    result = R4ClassifyWholeLightVisibility(invalid, values.data(), values.size(), true);
    if (result.size() != 1u || result[0].complete)
        return false;
    auto tooManyFaces = face(0u, 1, 0u);
    tooManyFaces.sample.expectedFaceCount = 255u;
    invalid = {tooManyFaces};
    result = R4ClassifyWholeLightVisibility(invalid, values.data(), values.size(), true);
    if (result.size() != 1u || result[0].complete)
        return false;
    auto outOfRange = face(0u, 1, 48u);
    invalid = {outOfRange};
    result = R4ClassifyWholeLightVisibility(invalid, values.data(), values.size(), true);
    if (result.size() != 1u || result[0].complete)
        return false;
    result = R4ClassifyWholeLightVisibility(invalid, nullptr, 0u, true);
    return result.size() == 1u && !result[0].complete;
}

bool singleSpot()
{
    R4WholeLightVisibilitySample sample;
    sample.engineFrame = 9;
    sample.batchId = 21;
    sample.lightId = 88;
    sample.face = 0;
    sample.expectedFaceCount = 1;
    std::vector<R4WholeLightVisibilityRecord> records{{sample, 0u, true}};
    const std::array<std::uint64_t, 1> values{{0u}};
    const auto groups = R4ClassifyWholeLightVisibility(records, values.data(), values.size(), true);
    if (groups.size() != 1u || !groups[0].complete || groups[0].passedSamples != 0u)
        return false;

    std::array<std::uint64_t, 9> manyValues{};
    std::vector<R4WholeLightVisibilityRecord> tooManyGroups;
    for (std::uint32_t i = 0u; i < 9u; ++i)
    {
        auto many = sample;
        many.lightId += i;
        tooManyGroups.push_back({many, i, true});
    }
    const auto rejected = R4ClassifyWholeLightVisibility(tooManyGroups,
        manyValues.data(), manyValues.size(), true);
    return rejected.size() == 9u && std::none_of(rejected.begin(), rejected.end(),
        [](const auto& group) { return group.complete; });
}
}

int main()
{
    if (!omniAcrossBatches() || !cpuObservationScopeCorrelation() ||
        !invalidCases() || !singleSpot())
    {
        std::fprintf(stderr, "WHOLE_LIGHT_VISIBILITY_LEDGER status=FAIL\n");
        return 1;
    }
    std::fprintf(stderr, "WHOLE_LIGHT_VISIBILITY_LEDGER status=PASS\n");
    return 0;
}
