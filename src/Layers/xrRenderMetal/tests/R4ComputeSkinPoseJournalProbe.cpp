#include "R4ComputeSkinPoseJournal.h"

#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
struct Output { int id = 0; };
struct Job { int id = 0; };
struct Instance
{
    std::shared_ptr<Output> skinnedVertices;
    std::shared_ptr<Output> skinPoseOutput;
    std::uint64_t outputWriteSerial = 0u;
    std::uint64_t skinPoseWriteSerial = 0u;
    std::uint64_t skinPoseHash = 0u;
    bool hasSkinPose = false;
};
using Journal = xray::metal::R4ComputeSkinFrameJournal<Output, Job>;

bool require(const bool value, const char* label)
{
    if (!value) std::fprintf(stderr, "COMPUTE_SKIN_JOURNAL FAIL %s\n", label);
    return value;
}

bool submittedPromotionAndSlotWrap()
{
    Journal journal;
    std::unordered_map<std::string, Instance> slots;
    for (int slot = 0; slot < 3; ++slot)
    {
        const std::string key = "geometry:object:7:slot:" + std::to_string(slot);
        Instance& instance = slots[key];
        instance.skinnedVertices = std::make_shared<Output>(Output{slot});
        instance.outputWriteSerial = xray::metal::AdvanceR4ComputeSkinWriteSerial(instance.outputWriteSerial);
        journal.Begin(std::uint64_t(slot + 1));
        if (!require(journal.Queue(key, Job{slot}, instance.skinnedVertices,
                std::uint64_t(slot + 1), instance.outputWriteSerial, 0xabcdu),
                "stage slot output")) return false;
        const std::vector<Job> jobs = journal.TakeJobs();
        if (!require(jobs.size() == 1u && jobs[0].id == slot && journal.PendingStampCount() == 1u,
                "batch extraction retains pending stamp")) return false;
        journal.Promote(std::uint64_t(slot + 1), [&slots](const std::string& name) -> Instance* {
            const auto it = slots.find(name);
            return it == slots.end() ? nullptr : &it->second;
        });
        if (!require(instance.hasSkinPose && instance.skinPoseHash == 0xabcdu,
                "successful submit promotes matching wrapper and serial")) return false;
    }
    const std::string wrapped = "geometry:object:7:slot:0";
    const Instance& instance = slots.at(wrapped);
    if (!require(xray::metal::R4ComputeSkinPoseMatches(instance.hasSkinPose, instance.skinPoseHash,
            instance.skinPoseOutput, instance.skinPoseWriteSerial, 0xabcdu,
            instance.skinnedVertices, instance.outputWriteSerial), "acquired slot wrap reuses committed pose"))
        return false;
    return true;
}

bool retryAndRepeatedEngineFrame()
{
    Journal journal;
    std::unordered_map<std::string, Instance> states;
    Instance& instance = states["repeat-frame"];
    instance.skinnedVertices = std::make_shared<Output>(Output{4});
    instance.outputWriteSerial = 1u;
    instance.skinPoseWriteSerial = 1u;
    instance.skinPoseHash = 10u;
    instance.skinPoseOutput = instance.skinnedVertices;
    instance.hasSkinPose = true;

    const std::uint32_t engineFrame = 77u;
    (void)engineFrame;
    journal.Begin(8u);
    if (!require(journal.Queue("repeat-frame", Job{1}, instance.skinnedVertices, 8u,
            instance.outputWriteSerial, 11u), "stage retry pose")) return false;
    const auto firstAttempt = journal.TakeJobs();
    if (!require(firstAttempt.size() == 1u, "first same-frame attempt queued")) return false;
    journal.Discard(); // late drawable retry: compute was not submitted
    if (!require(instance.hasSkinPose && instance.skinPoseHash == 10u &&
            !xray::metal::R4ComputeSkinPoseMatches(instance.hasSkinPose, instance.skinPoseHash,
                instance.skinPoseOutput, instance.skinPoseWriteSerial, 11u,
                instance.skinnedVertices, instance.outputWriteSerial),
            "discard preserves only prior committed pose")) return false;

    journal.Begin(9u); // same engine frame, fresh backend acquisition epoch
    if (!require(journal.Queue("repeat-frame", Job{2}, instance.skinnedVertices, 9u,
            instance.outputWriteSerial, 11u), "same frame retry requeues unchanged pose")) return false;
    const auto retryJobs = journal.TakeJobs();
    if (!require(retryJobs.size() == 1u && retryJobs[0].id == 2,
            "retry has a new job")) return false;
    journal.Promote(9u, [&states](const std::string& name) -> Instance* {
        const auto it = states.find(name);
        return it == states.end() ? nullptr : &it->second;
    });
    return require(instance.skinPoseHash == 11u && instance.hasSkinPose,
        "retry submit commits current pose");
}

bool replacementAndCpuFallbackCancel()
{
    Journal journal;
    std::unordered_map<std::string, Instance> states;
    Instance& replaced = states["replaced"];
    replaced.skinnedVertices = std::make_shared<Output>(Output{1});
    replaced.outputWriteSerial = 1u;
    replaced.skinPoseOutput = replaced.skinnedVertices;
    replaced.skinPoseWriteSerial = 1u;
    replaced.skinPoseHash = 41u;
    replaced.hasSkinPose = true;
    journal.Begin(12u);
    if (!require(journal.Queue("replaced", Job{3}, replaced.skinnedVertices,
            12u, replaced.outputWriteSerial, 42u), "stage replaceable output")) return false;
    const auto jobs = journal.TakeJobs();
    if (!require(jobs.size() == 1u, "replaceable job extracted")) return false;
    replaced.skinnedVertices = std::make_shared<Output>(Output{2});
    xray::metal::AdvanceR4ComputeSkinWriteSerial(replaced.outputWriteSerial);
    journal.Promote(12u, [&states](const std::string& name) -> Instance* {
        const auto it = states.find(name);
        return it == states.end() ? nullptr : &it->second;
    });
    if (!require(replaced.hasSkinPose && replaced.skinPoseHash == 41u &&
            !xray::metal::R4ComputeSkinPoseMatches(replaced.hasSkinPose, replaced.skinPoseHash,
                replaced.skinPoseOutput, replaced.skinPoseWriteSerial, 42u,
                replaced.skinnedVertices, replaced.outputWriteSerial),
            "replacement and serial change reject stale promotion and reuse")) return false;

    Instance& fallback = states["same-key-fallback"];
    fallback.skinnedVertices = std::make_shared<Output>(Output{3});
    fallback.outputWriteSerial = 1u;
    journal.Begin(13u);
    if (!require(journal.Queue("same-key-fallback", Job{4}, fallback.skinnedVertices,
            13u, fallback.outputWriteSerial, 99u), "stage job before CPU fallback")) return false;
    journal.Cancel("same-key-fallback"); // legacy CPU update owns the output now
    if (!require(journal.QueuedJobCount() == 0u && journal.PendingStampCount() == 0u &&
            journal.TakeJobs().empty(), "CPU fallback cancels write and promotion")) return false;
    journal.Promote(13u, [&states](const std::string& name) -> Instance* {
        const auto it = states.find(name);
        return it == states.end() ? nullptr : &it->second;
    });
    if (!require(!fallback.hasSkinPose, "cancelled GPU pose cannot commit")) return false;

    Instance& duplicate = states["duplicate-snapshot-key"];
    duplicate.skinnedVertices = std::make_shared<Output>(Output{5});
    duplicate.outputWriteSerial = 2u;
    duplicate.skinPoseWriteSerial = 2u;
    duplicate.skinPoseHash = 100u;
    duplicate.skinPoseOutput = duplicate.skinnedVertices;
    duplicate.hasSkinPose = true;
    journal.Begin(14u);
    if (!require(journal.Queue("duplicate-snapshot-key", Job{5}, duplicate.skinnedVertices,
            14u, duplicate.outputWriteSerial, 200u),
            "earlier duplicate queues changed provisional pose")) return false;
    if (xray::metal::R4ComputeSkinPoseMatches(duplicate.hasSkinPose, duplicate.skinPoseHash,
            duplicate.skinPoseOutput, duplicate.skinPoseWriteSerial, 100u,
            duplicate.skinnedVertices, duplicate.outputWriteSerial))
        journal.Cancel("duplicate-snapshot-key"); // later duplicate draws committed pose A
    if (!require(journal.QueuedJobCount() == 0u && journal.PendingStampCount() == 0u,
            "later committed-pose duplicate cancels earlier GPU pose B")) return false;
    journal.Promote(14u, [&states](const std::string& name) -> Instance* {
        const auto it = states.find(name);
        return it == states.end() ? nullptr : &it->second;
    });
    if (!require(duplicate.skinPoseHash == 100u && duplicate.hasSkinPose,
            "cancelled duplicate leaves committed pose A intact")) return false;

    std::uint64_t serial = std::numeric_limits<std::uint64_t>::max();
    return require(xray::metal::AdvanceR4ComputeSkinWriteSerial(serial) == 1u,
        "logical write serial skips zero on rollover");
}

bool backendDisableLatch()
{
    bool optedIn = true;
    bool backendDisabled = false;
    bool cpuRetry = false;
    if (!require(xray::metal::R4ComputeSkinShouldDispatch(optedIn, backendDisabled, cpuRetry),
            "opt-in starts enabled")) return false;
    cpuRetry = true;
    if (!require(!xray::metal::R4ComputeSkinShouldDispatch(optedIn, backendDisabled, cpuRetry),
            "same-frame CPU retry skips compute")) return false;
    cpuRetry = false;
    backendDisabled = true;
    if (!require(!xray::metal::R4ComputeSkinShouldDispatch(optedIn, backendDisabled, cpuRetry),
            "backend rejection disables later frames")) return false;
    optedIn = false;
    return require(!xray::metal::R4ComputeSkinShouldDispatch(optedIn, false, false),
        "default opt-out remains CPU-only");
}
} // namespace

int main()
{
    if (!submittedPromotionAndSlotWrap() || !retryAndRepeatedEngineFrame() ||
        !replacementAndCpuFallbackCancel() || !backendDisableLatch()) return 1;
    std::puts("COMPUTE_SKIN_JOURNAL PASS submit discard retry slot-wrap replacement CPU-cancel serial-wrap backend-latch");
    return 0;
}
