#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace xray::metal
{
inline std::uint64_t AdvanceR4ComputeSkinWriteSerial(std::uint64_t& serial)
{
    if (++serial == 0u) serial = 1u;
    return serial;
}

inline bool R4ComputeSkinShouldDispatch(const bool optedIn, const bool backendDisabled,
    const bool currentAttemptIsCpuRetry)
{
    return optedIn && !backendDisabled && !currentAttemptIsCpuRetry;
}

template<class Buffer, class Job>
class R4ComputeSkinFrameJournal
{
public:
    struct Stamp
    {
        std::string key;
        std::shared_ptr<Buffer> output;
        std::uint64_t epoch = 0u;
        std::uint64_t writeSerial = 0u;
        std::uint64_t poseHash = 0u;
    };

    void Begin(const std::uint64_t epoch)
    {
        Discard();
        epoch_ = epoch;
    }

    bool Queue(const std::string& key, Job job, std::shared_ptr<Buffer> output,
        const std::uint64_t epoch, const std::uint64_t writeSerial,
        const std::uint64_t poseHash)
    {
        if (key.empty() || !output || epoch == 0u || epoch != epoch_ || writeSerial == 0u)
            return false;
        const auto jobIt = std::find_if(jobs_.begin(), jobs_.end(),
            [&key](const QueuedJob& entry) { return entry.key == key; });
        if (jobIt == jobs_.end())
            jobs_.push_back({key, std::move(job)});
        else
            jobIt->job = std::move(job);

        const auto stampIt = std::find_if(stamps_.begin(), stamps_.end(),
            [&key](const Stamp& stamp) { return stamp.key == key; });
        Stamp stamp{key, std::move(output), epoch, writeSerial, poseHash};
        if (stampIt == stamps_.end())
            stamps_.push_back(std::move(stamp));
        else
            *stampIt = std::move(stamp);
        return true;
    }

    // Same-key CPU fallback, output replacement or instance erasure cancels
    // both the queued write and its provisional submit stamp.
    void Cancel(const std::string& key)
    {
        jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(),
            [&key](const QueuedJob& entry) { return entry.key == key; }), jobs_.end());
        stamps_.erase(std::remove_if(stamps_.begin(), stamps_.end(),
            [&key](const Stamp& stamp) { return stamp.key == key; }), stamps_.end());
    }

    std::vector<Job> TakeJobs()
    {
        std::vector<Job> result;
        result.reserve(jobs_.size());
        for (QueuedJob& entry : jobs_)
            result.push_back(std::move(entry.job));
        jobs_.clear();
        return result;
    }

    template<class Resolver>
    void Promote(const std::uint64_t submittedEpoch, Resolver&& resolve)
    {
        if (submittedEpoch != 0u && submittedEpoch == epoch_)
        {
            for (const Stamp& stamp : stamps_)
            {
                if (stamp.epoch != submittedEpoch) continue;
                auto* state = resolve(stamp.key);
                if (!state || state->skinnedVertices != stamp.output ||
                    state->outputWriteSerial != stamp.writeSerial)
                    continue;
                state->skinPoseHash = stamp.poseHash;
                state->skinPoseOutput = stamp.output;
                state->skinPoseWriteSerial = stamp.writeSerial;
                state->hasSkinPose = true;
            }
        }
        Discard();
    }

    void Discard()
    {
        jobs_.clear();
        stamps_.clear();
        epoch_ = 0u;
    }

    std::uint64_t Epoch() const { return epoch_; }
    std::size_t QueuedJobCount() const { return jobs_.size(); }
    std::size_t PendingStampCount() const { return stamps_.size(); }
    const std::vector<Stamp>& Stamps() const { return stamps_; }

private:
    struct QueuedJob
    {
        std::string key;
        Job job;
    };
    std::uint64_t epoch_ = 0u;
    std::vector<QueuedJob> jobs_;
    std::vector<Stamp> stamps_;
};

template<class Buffer>
bool R4ComputeSkinPoseMatches(const bool hasSkinPose, const std::uint64_t committedPoseHash,
    const std::shared_ptr<Buffer>& committedOutput, const std::uint64_t committedWriteSerial,
    const std::uint64_t currentPoseHash, const std::shared_ptr<Buffer>& currentOutput,
    const std::uint64_t currentWriteSerial)
{
    return hasSkinPose && committedOutput && committedOutput == currentOutput &&
        committedWriteSerial != 0u && committedWriteSerial == currentWriteSerial &&
        committedPoseHash == currentPoseHash;
}
} // namespace xray::metal
