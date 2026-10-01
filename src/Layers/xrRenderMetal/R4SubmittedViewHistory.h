#pragma once

#include <array>
#include <cmath>
#include <cstdint>

// Primary-world history follows accepted producer submissions, not engine
// ticks or frame-context slots. A skipped/aborted frame never advances it.
class R4SubmittedViewHistory
{
public:
    template<class View> View Select(const View& current, std::uint32_t width,
        std::uint32_t height) const
    {
        View selected = current;
        bool reset = !valid || !current.valid || current.cameraCut ||
            width != extent[0] || height != extent[1] || current.engineFrame <= frame;
        float distanceSquared = 0.0f;
        float directionDot = 0.0f;
        for (std::size_t i = 0; i < 3u; ++i)
        {
            reset = reset || !std::isfinite(current.eye[i]);
            const float difference = current.eye[i] - eye[i];
            distanceSquared += difference * difference;
            directionDot += current.view[2u + 4u * i] * view[2u + 4u * i];
        }
        reset = reset || distanceSquared > 2500.0f || directionDot < 0.5f;
        for (std::size_t i = 0; i < 16u; ++i)
            reset = reset || !std::isfinite(current.view[i]) ||
                !std::isfinite(current.projection[i]) ||
                std::abs(current.projection[i] - projection[i]) > 1.0e-5f;
        selected.previousView = reset ? current.view : view;
        selected.previousProjection = reset ? current.projection : projection;
        selected.previousFrame = reset ? current.engineFrame : frame;
        selected.previousValid = !reset;
        selected.cameraCut = reset;
        return selected;
    }

    template<class View> void Stage(const View& current, std::uint32_t width,
        std::uint32_t height, std::uint64_t submissionSerial)
    {
        pendingView = current.view;
        pendingProjection = current.projection;
        pendingEye = current.eye;
        pendingFrame = current.engineFrame;
        pendingExtent = {width, height};
        pendingSerial = submissionSerial;
        pending = current.valid;
        // Keep a primary-view token even for malformed camera data so the
        // submission still rejects a second view. If accepted, however, that
        // malformed producer must invalidate history instead of storing NaNs
        // as the source for a later finite camera.
        pendingFinite = current.valid && finite(current.view) && finite(current.projection) &&
            finite(current.eye);
    }
    void Commit()
    {
        if (!pending) return;
        view = pendingView; projection = pendingProjection; eye = pendingEye;
        frame = pendingFrame; extent = pendingExtent; serial = pendingSerial;
        valid = pendingFinite;
        pending = false;
        pendingFinite = false;
    }
    void Discard() { pending = false; pendingFinite = false; }
    void Invalidate() { valid = false; pending = false; pendingFinite = false; }
    bool HasPending() const { return pending; }
    std::uint64_t Serial() const { return valid ? serial : 0u; }
private:
    template<std::size_t N> static bool finite(const std::array<float, N>& values)
    {
        for (const float value : values)
            if (!std::isfinite(value)) return false;
        return true;
    }

    std::array<float, 16> view{}, projection{}, pendingView{}, pendingProjection{};
    std::array<float, 3> eye{}, pendingEye{};
    std::array<std::uint32_t, 2> extent{}, pendingExtent{};
    std::uint32_t frame = 0, pendingFrame = 0;
    std::uint64_t serial = 0, pendingSerial = 0;
    bool valid = false, pending = false, pendingFinite = false;
};
