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
    // SSFX ground splashes are world-space detail-model meshes, not rain
    // sprite quads. Preserve their independent topology and 32-bit batch
    // indices so multiple copied DM_Drop instances can share one immutable
    // Metal upload without the legacy 16-bit per-lock ceiling.
    struct SplashVertex
    {
        float position[3]{};
        std::uint32_t d3dColor = 0u;
        float texcoord[2]{};
    };
    std::vector<SplashVertex> splashVertices;
    std::vector<std::uint32_t> splashIndices;
    std::array<float, 4> shaderSetup{};
    std::array<float, 4> splashSetup{};
    float density = 0.0f;
    bool ssfx = false;
    std::uint32_t droppedVertices = 0u;
    std::uint32_t droppedSplashVertices = 0u;
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
        result.splashVertices.swap(m_frame.splashVertices);
        result.splashIndices.swap(m_frame.splashIndices);
        result.shaderSetup = m_frame.shaderSetup;
        result.splashSetup = m_frame.splashSetup;
        result.density = m_frame.density;
        result.ssfx = m_frame.ssfx;
        result.droppedVertices = m_frame.droppedVertices;
        result.droppedSplashVertices = m_frame.droppedSplashVertices;
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
