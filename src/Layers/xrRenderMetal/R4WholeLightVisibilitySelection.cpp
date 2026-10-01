#include "R4WholeLightVisibilitySelection.h"
#include "R4LocalLightFaceVisibility.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace xray::metal
{
namespace
{
struct GroupKey
{
    R4WholeLightVisibilityGroupKind kind = R4WholeLightVisibilityGroupKind::SingleRecord;
    std::uint64_t parent = 0;
    bool operator<(const GroupKey& other) const
    {
        return std::tie(kind, parent) < std::tie(other.kind, other.parent);
    }
};

struct Group
{
    GroupKey key{};
    std::vector<std::size_t> indices;
};

enum class ExemptReason { None, Hud, Volumetric, Invalid };

ExemptReason exemptReason(const R4EngineLocalLight& light, const R4EnvironmentSnapshot& environment)
{
    // This observer only measures the deferred accumulation consumer. HUD and
    // volumetric routes are separate consumers, so their parents are excluded.
    for (const float value : environment.ssfxVolumetric)
        if (!std::isfinite(value)) return ExemptReason::Invalid;
    for (const float value : light.position) if (!std::isfinite(value)) return ExemptReason::Invalid;
    for (const float value : light.direction) if (!std::isfinite(value)) return ExemptReason::Invalid;
    for (const float value : light.right) if (!std::isfinite(value)) return ExemptReason::Invalid;
    for (const float value : light.color) if (!std::isfinite(value)) return ExemptReason::Invalid;
    if (!std::isfinite(light.range) || !std::isfinite(light.cone)) return ExemptReason::Invalid;
    if (!light.active || !light.shadowed) return ExemptReason::Invalid;
    if (light.hudMode) return ExemptReason::Hud;
    if (light.type == R4EngineLocalLightType::OmniFace)
        return R4LocalLightFaceMayBeCulled(light, environment) ? ExemptReason::None :
            ((environment.localVolumetricEnabled && environment.volumetricMode != 1) ? ExemptReason::Volumetric : ExemptReason::Invalid);
    // Match BuildMetalR4VolumetricSpotPass: a spotlight has a volume consumer
    // only when the global pass is enabled and either SSFX or the captured
    // per-light flag admits it.
    return environment.localVolumetricEnabled && environment.volumetricMode != 1 &&
        (environment.ssfxVolumetric[0] > 0.0f || light.volumetricEnabled) ?
            ExemptReason::Volumetric : ExemptReason::None;
}
}

bool SelectR4WholeLightVisibilitySamples(const R4EngineLocalLightSnapshot& snapshot,
    const R4EnvironmentSnapshot& environment,
    const std::vector<std::vector<std::size_t>>& finalBatches,
    std::uint32_t firstFrame, std::uint32_t engineFrame,
    std::vector<R4WholeLightVisibilitySample>& samples,
    R4WholeLightVisibilitySelectionStats& stats, std::string& error)
{
    samples.clear();
    stats = {};
    error.clear();
    if (engineFrame < firstFrame || engineFrame - firstFrame >= 12u ||
        snapshot.engineFrame != engineFrame || environment.engineFrame != engineFrame ||
        !environment.valid)
        return true;

    std::map<GroupKey, Group> groups;
    std::map<std::uint64_t, std::size_t> idCounts;
    for (std::size_t i = 0; i < snapshot.lights.size(); ++i)
    {
        const auto& light = snapshot.lights[i];
        if (light.lightId != 0u) ++idCounts[light.lightId];
        if (!light.shadowed || !light.active) continue;
        if (light.type == R4EngineLocalLightType::Spot)
        {
            const GroupKey key{R4WholeLightVisibilityGroupKind::SingleRecord, light.lightId};
            auto [it, inserted] = groups.emplace(key, Group{key, {}});
            (void)inserted;
            it->second.indices.push_back(i);
        }
        else if (light.type == R4EngineLocalLightType::OmniFace)
        {
            const GroupKey key{R4WholeLightVisibilityGroupKind::OmniParent, light.parentLightId};
            auto [it, inserted] = groups.emplace(key, Group{key, {}});
            (void)inserted;
            it->second.indices.push_back(i);
        }
    }

    std::map<std::size_t, std::size_t> batchForIndex;
    for (std::size_t batch = 0; batch < finalBatches.size(); ++batch)
        for (const std::size_t index : finalBatches[batch])
        {
            if (index >= snapshot.lights.size())
            {
                error = "visibility selection saw an out-of-range planned light index";
                return true;
            }
            if (!batchForIndex.emplace(index, batch).second)
            {
                error = "visibility selection saw a light repeated in planned batches";
                return true;
            }
        }

    struct Eligible
    {
        GroupKey key{};
        std::vector<std::pair<std::size_t, std::size_t>> indexBatch;
    };
    std::vector<Eligible> eligible;
    for (const auto& [key, group] : groups)
    {
        const std::size_t plannedMembers = static_cast<std::size_t>(std::count_if(
            group.indices.begin(), group.indices.end(), [&](std::size_t index) {
                return batchForIndex.find(index) != batchForIndex.end();
            }));
        if (plannedMembers == 0u) continue; // existing frustum planning removed the whole group
        ++stats.plannedGroups;
        const std::size_t expected = key.kind == R4WholeLightVisibilityGroupKind::OmniParent ? 6u : 1u;
        if ((key.kind == R4WholeLightVisibilityGroupKind::OmniParent && key.parent == 0u) ||
            group.indices.size() != expected || plannedMembers != group.indices.size())
        { ++stats.incompleteOrCollision; continue; }
        std::array<bool, 6> faces{};
        std::set<std::uint64_t> localIds;
        Eligible candidate; candidate.key = key;
        bool valid = true;
        std::vector<ExemptReason> reasons;
        reasons.reserve(group.indices.size());
        for (const std::size_t index : group.indices)
        {
            const ExemptReason reason = exemptReason(snapshot.lights[index], environment);
            reasons.push_back(reason);
            if (reason == ExemptReason::Hud) ++stats.hudExemptRecords;
            else if (reason == ExemptReason::Volumetric) ++stats.volumetricExemptRecords;
            else if (reason == ExemptReason::Invalid) ++stats.invalidExemptRecords;
        }
        std::size_t reasonIndex = 0u;
        for (const std::size_t index : group.indices)
        {
            const auto& light = snapshot.lights[index];
            const bool isOmni = key.kind == R4WholeLightVisibilityGroupKind::OmniParent;
            const ExemptReason reason = reasons[reasonIndex++];
            if (light.lightId == 0u ||
                (isOmni && light.parentLightId != key.parent) ||
                (!isOmni && light.lightId != key.parent) ||
                idCounts[light.lightId] != 1u || !localIds.insert(light.lightId).second ||
                light.type != (isOmni ? R4EngineLocalLightType::OmniFace : R4EngineLocalLightType::Spot) ||
                reason != ExemptReason::None)
            { valid = false; break; }
            if (isOmni)
            {
                if (light.omniFace >= faces.size() || faces[light.omniFace])
                { valid = false; break; }
                faces[light.omniFace] = true;
            }
            else if (light.omniFace != 0xffu && light.omniFace != 0u)
            { valid = false; break; }
            const auto batch = batchForIndex.find(index);
            if (batch == batchForIndex.end() ||
                batch->second > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
            { valid = false; break; }
            candidate.indexBatch.emplace_back(index, batch->second);
        }
        if (valid && key.kind == R4WholeLightVisibilityGroupKind::OmniParent &&
            std::find(faces.begin(), faces.end(), false) != faces.end())
            valid = false;
        if (valid) eligible.push_back(std::move(candidate));
        else ++stats.incompleteOrCollision;
    }

    stats.eligibleGroups = eligible.size();
    if (eligible.empty()) return true;
    const std::size_t offset = (static_cast<std::size_t>(engineFrame - firstFrame) * 8u) % eligible.size();
    const std::size_t selectedCount = std::min<std::size_t>(8u, eligible.size());
    stats.selectedGroups = selectedCount;
    stats.omittedByQuota = eligible.size() - selectedCount;
    for (std::size_t n = 0; n < selectedCount; ++n)
    {
        const auto& group = eligible[(offset + n) % eligible.size()];
        for (const auto [index, batch] : group.indexBatch)
        {
            const auto& light = snapshot.lights[index];
            R4WholeLightVisibilitySample sample;
            sample.engineFrame = engineFrame;
            sample.batchId = static_cast<std::int32_t>(batch);
            sample.lightId = light.lightId;
            sample.parentLightId = light.parentLightId;
            sample.face = group.key.kind == R4WholeLightVisibilityGroupKind::OmniParent ? light.omniFace : 0u;
            sample.groupKind = group.key.kind;
            sample.expectedFaceCount = group.key.kind == R4WholeLightVisibilityGroupKind::OmniParent ? 6u : 1u;
            samples.push_back(sample);
            ++stats.selectedRecords;
        }
    }
    return true;
}

bool RunR4WholeLightVisibilitySelectionSelfTest(std::string& error)
{
    const auto fail = [&error](const char* why) { error = why; return false; };
    R4EngineLocalLightSnapshot snapshot; snapshot.engineFrame = 300u;
    R4EnvironmentSnapshot environment; environment.engineFrame = 300u; environment.valid = true;
    std::vector<std::vector<std::size_t>> batches(2);
    const auto addOmni = [&](std::uint64_t parent, std::size_t split) {
        for (std::uint8_t face = 0; face < 6; ++face)
        {
            R4EngineLocalLight light; light.lightId = parent * 10u + face + 1u;
            light.parentLightId = parent; light.type = R4EngineLocalLightType::OmniFace;
            light.active = true; light.shadowed = true; light.omniFace = face;
            const std::size_t index = snapshot.lights.size(); snapshot.lights.push_back(light);
            batches[face < split ? 0u : 1u].push_back(index);
        }
    };
    addOmni(1u, 3u); // one logical group crosses planned batches
    R4EngineLocalLight spot; spot.lightId = 100u; spot.parentLightId = 0u;
    spot.type = R4EngineLocalLightType::Spot; spot.active = true; spot.shadowed = true;
    const std::size_t spotIndex = snapshot.lights.size(); snapshot.lights.push_back(spot);
    batches[1].push_back(spotIndex);

    std::vector<R4WholeLightVisibilitySample> samples;
    R4WholeLightVisibilitySelectionStats stats;
    if (!SelectR4WholeLightVisibilitySamples(snapshot, environment, batches, 300u, 300u, samples, stats, error) ||
        samples.size() != 7u)
        return fail("complete six-face parent and spot were not selected");
    std::size_t omni = 0u, spotCount = 0u;
    for (const auto& sample : samples)
    {
        if (sample.groupKind == R4WholeLightVisibilityGroupKind::OmniParent)
        { ++omni; if (sample.expectedFaceCount != 6u || sample.face >= 6u) return fail("bad omni tag"); }
        else
        { ++spotCount; if (sample.expectedFaceCount != 1u || sample.face != 0u) return fail("bad spot tag"); }
    }
    if (omni != 6u || spotCount != 1u) return fail("group metadata mismatch");
    for (const auto& sample : samples)
        if (sample.groupKind == R4WholeLightVisibilityGroupKind::OmniParent &&
            sample.batchId != (sample.face < 3u ? 0 : 1))
            return fail("cross-batch face lost its planned batch ID");

    // Missing/duplicate faces, identity collisions, and excluded consumers must
    // omit the whole group. These cases are selected at the only frame in range.
    batches[1].erase(std::remove(batches[1].begin(), batches[1].end(), std::size_t{5}), batches[1].end());
    if (!SelectR4WholeLightVisibilitySamples(snapshot, environment, batches, 300u, 300u, samples, stats, error) ||
        samples.size() != 1u || samples.front().groupKind != R4WholeLightVisibilityGroupKind::SingleRecord)
        return fail("missing planned face did not exempt the parent");
    batches[1].push_back(5u);
    snapshot.lights[5].omniFace = 4u;
    if (!SelectR4WholeLightVisibilitySamples(snapshot, environment, batches, 300u, 300u, samples, stats, error) ||
        samples.size() != 1u || samples.front().groupKind != R4WholeLightVisibilityGroupKind::SingleRecord)
        return fail("duplicate omni face did not exempt the parent");
    snapshot.lights[5].omniFace = 5u;
    snapshot.lights[2].lightId = snapshot.lights[0].lightId;
    if (!SelectR4WholeLightVisibilitySamples(snapshot, environment, batches, 300u, 300u, samples, stats, error) ||
        samples.size() != 1u)
        return fail("identity collision did not exempt the parent");
    snapshot.lights[2].lightId = 13u;
    snapshot.lights[1].hudMode = true;
    if (!SelectR4WholeLightVisibilitySamples(snapshot, environment, batches, 300u, 300u, samples, stats, error) ||
        samples.size() != 1u)
        return fail("HUD face did not exempt the parent");
    snapshot.lights[1].hudMode = false;
    snapshot.lights[1].volumetricEnabled = true;
    // Exercise the per-light route specifically; the default SSFX x control is
    // 1 and would correctly classify every admitted shadow light as volumetric.
    environment.ssfxVolumetric[0] = 0.0f;
    environment.localVolumetricEnabled = true;
    if (!SelectR4WholeLightVisibilitySamples(snapshot, environment, batches, 300u, 300u, samples, stats, error) ||
        samples.size() != 1u)
        return fail("volumetric face did not exempt the parent");
    snapshot.lights[1].volumetricEnabled = false;
    environment.ssfxVolumetric[0] = 1.0f;
    if (!SelectR4WholeLightVisibilitySamples(snapshot, environment, batches, 300u, 300u, samples, stats, error) ||
        !samples.empty() || stats.volumetricExemptRecords != 7u)
        return fail("SSFX volumetric face did not exempt the parent");
    environment.localVolumetricEnabled = false;

    // At most eight complete groups per frame; rotations cover later groups.
    snapshot.lights.clear(); batches.assign(1u, {});
    for (std::uint64_t parent = 1u; parent <= 10u; ++parent)
    {
        R4EngineLocalLight l; l.lightId = parent; l.parentLightId = parent;
        l.type = R4EngineLocalLightType::Spot; l.active = true; l.shadowed = true;
        batches[0].push_back(snapshot.lights.size()); snapshot.lights.push_back(l);
    }
    if (!SelectR4WholeLightVisibilitySamples(snapshot, environment, batches, 300u, 300u, samples, stats, error) || samples.size() != 8u)
        return fail("per-frame group cap was not enforced");
    snapshot.engineFrame = environment.engineFrame = 301u;
    if (!SelectR4WholeLightVisibilitySamples(snapshot, environment, batches, 300u, 301u, samples, stats, error) || samples.size() != 8u)
        return fail("next-frame group cap failed");
    std::set<std::uint64_t> frameZero, frameOne;
    for (const auto& sample : samples) frameOne.insert(sample.lightId);
    snapshot.engineFrame = environment.engineFrame = 300u;
    SelectR4WholeLightVisibilitySamples(snapshot, environment, batches, 300u, 300u, samples, stats, error);
    for (const auto& sample : samples) frameZero.insert(sample.lightId);
    bool discoveredLateGroups = false;
    for (auto id : frameOne) if (!frameZero.count(id)) discoveredLateGroups = true;
    if (!discoveredLateGroups) return fail("group rotation did not advance the sample window");
    for (std::uint64_t id = 1u; id <= 10u; ++id)
        if (!frameZero.count(id) && !frameOne.count(id)) return fail("two-frame rotation missed a candidate group");
    snapshot.engineFrame = environment.engineFrame = 312u;
    if (!SelectR4WholeLightVisibilitySamples(snapshot, environment, batches, 300u, 312u, samples, stats, error) || !samples.empty())
        return fail("selection exceeded the twelve-frame window");

    // Parent IDs are irrelevant for a singleton spotlight, while each selected
    // Omni parent contributes exactly six records and the per-frame cap is 48.
    snapshot.engineFrame = environment.engineFrame = 300u;
    snapshot.lights.clear(); batches.assign(1u, {});
    for (std::uint64_t parent = 1u; parent <= 10u; ++parent)
        for (std::uint8_t face = 0; face < 6u; ++face)
        {
            R4EngineLocalLight light; light.lightId = parent * 10u + face + 1u;
            light.parentLightId = parent; light.type = R4EngineLocalLightType::OmniFace;
            light.active = true; light.shadowed = true; light.omniFace = face;
            batches[0].push_back(snapshot.lights.size()); snapshot.lights.push_back(light);
        }
    if (!SelectR4WholeLightVisibilitySamples(snapshot, environment, batches, 300u, 300u, samples, stats, error) ||
        samples.size() != 48u || stats.selectedGroups != 8u)
        return fail("eight-parent / 48-face cap was not enforced");
    error.clear();
    return true;
}
} // namespace xray::metal
