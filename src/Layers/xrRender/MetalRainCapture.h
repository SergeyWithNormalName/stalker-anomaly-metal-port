#pragma once

#include "MetalParticleCapture.h"

#include <array>
#include <mutex>
#include <utility>
#include <vector>

// Direct Metal receives the already simulated CEffect_Rain quads here, before
// the legacy RCache/D3D path would consume them.  It is deliberately a
// value-only handoff: the producer retains no shader, texture, or render
// target objects from the legacy renderer.
namespace xray::metal::rain
{
struct FrameBatch
{
    std::vector<particles::SpriteVertex> vertices;
    std::array<float, 4> shaderSetup{};
    float density = 0.0f;
    bool ssfx = false;
    std::uint32_t droppedVertices = 0u;
};

class FrameCapture
{
public:
    static constexpr std::uint32_t MaxVertices = 10000u;

    void submit(FrameBatch frame)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_frame = std::move(frame);
    }

    FrameBatch drain()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        FrameBatch result;
        result.vertices.swap(m_frame.vertices);
        result.shaderSetup = m_frame.shaderSetup;
        result.density = m_frame.density;
        result.ssfx = m_frame.ssfx;
        result.droppedVertices = m_frame.droppedVertices;
        m_frame = {};
        return result;
    }

private:
    std::mutex m_mutex;
    FrameBatch m_frame;
};

inline FrameCapture& capture()
{
    static FrameCapture instance;
    return instance;
}

inline void SubmitFrame(FrameBatch frame)
{
    capture().submit(std::move(frame));
}

inline FrameBatch DrainFrame()
{
    return capture().drain();
}
} // namespace xray::metal::rain
