#pragma once

#include "IR4Backend.h"
#include "R4DdsTexture.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

// Owns the general SSFX SSR fullscreen phase. Scene ordering and live engine
// policy remain at the production scene boundary; this leaf owns only its
// five RGBA8 intermediates, previous-position copy, and submit-committed state.
class MetalR4SsrAdapter final
{
public:
    struct Options
    {
        std::filesystem::path shaderDirectory;
        std::filesystem::path blueNoiseTexturePath;
        xray::metal::R4DdsBytesProvider ddsBytesProvider;
        // Only quality 0 has a reviewed DXC annotation fold and matching cook.
        std::uint32_t quality = 0u;
    };

    struct FrameInputs
    {
        // `sceneColorSnapshot` is the pre-SSR copy of the current combined
        // color target (Windows `rt_Generic_temp`). The adapter reads it in
        // trace/combine and writes only `combinedSceneColor` in pass 5.
        std::shared_ptr<IR4Texture> combinedSceneColor;
        std::shared_ptr<IR4Texture> sceneColorSnapshot;
        std::shared_ptr<IR4Texture> position;
        std::shared_ptr<IR4Texture> diffuse;
        std::shared_ptr<IR4Texture> motionVectors;
        std::shared_ptr<IR4Texture> blueNoise;
        // Present in the Windows blender input record. Exact non-MSAA DXIL
        // reflection prunes both environment cubes from the gloss PSO, while
        // sky0/sky1 remain active bindings below.
        std::shared_ptr<IR4Texture> environment0;
        std::shared_ptr<IR4Texture> environment1;
        std::shared_ptr<IR4Texture> sky0;
        std::shared_ptr<IR4Texture> sky1;
        std::shared_ptr<IR4Texture> depth;
        std::shared_ptr<IR4Buffer> staticGlobals;
        std::shared_ptr<IR4Sampler> noFilterSampler;
        std::shared_ptr<IR4Sampler> linearSampler;
        std::shared_ptr<IR4Sampler> baseSampler;

        // Matrices and vectors below are the original `$Globals` b0 values;
        // callers pass the accepted primary view state and live R4 controls.
        std::array<float, 16> currentMatrix{};
        std::array<float, 16> previousMatrix{};
        std::array<float, 4> ssrSetup{1.0f, 0.2f, 0.0f, 0.0f};
        std::array<float, 4> ssfxSsr2{};
        std::array<float, 4> underground{};
        std::uint32_t engineFrame = 0u;
        std::uint32_t previousFrame = 0u;
        bool enabled = false;
        bool previousValid = false;
        bool cameraCut = false;
    };

    MetalR4SsrAdapter();
    ~MetalR4SsrAdapter();
    MetalR4SsrAdapter(const MetalR4SsrAdapter&) = delete;
    MetalR4SsrAdapter& operator=(const MetalR4SsrAdapter&) = delete;

    // A missing or invalid bundle leaves this leaf disabled and reports a
    // status string, matching the other optional R4 adapters.
    bool Prepare(IR4Backend& backend, const Options& options, std::string& status);
    bool PrepareRenderTargets(IR4Backend& backend, std::uint32_t width,
        std::uint32_t height, R4PixelFormat sceneColorFormat,
        const R4Texture2DDesc& positionDescriptor, std::string& error);
    bool Draw(IR4Backend& backend, const FrameInputs& inputs, std::string& error);
    void DiscardUnsubmittedFrame();
    void CommitSubmittedFrame();
    void InvalidateHistory();
    std::shared_ptr<IR4Texture> BlueNoiseTexture() const;

    bool IsReady() const { return m_ready; }
    const std::string& Status() const { return m_status; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    bool m_ready = false;
    std::string m_status;
};
