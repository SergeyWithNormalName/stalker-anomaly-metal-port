#pragma once

#include "MetalR4StaticForwardAdapter.h"
#include "R4DdsTexture.h"

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// Owns only the exact SSFX water phase. It deliberately does not participate
// in MetalR4StaticForwardAdapter's general alpha-forward pass.
class MetalR4WaterAdapter final
{
public:
    struct Options
    {
        std::filesystem::path shaderDirectory;
        xray::metal::R4DdsBytesProvider ddsBytesProvider;
    };
    struct Telemetry
    {
        std::size_t capturedRecords = 0;
        std::size_t rejectedRecords = 0;
        std::size_t ssrPipelines = 0;
        std::size_t finalPipelines = 0;
        std::size_t fullscreenPipelines = 0;
        std::size_t submittedWaterDraws = 0;
    };
    // All inputs are explicitly owned by the production scene.  This keeps
    // the water phase independent from the legacy D3D dsgraph and makes the
    // read/write boundaries of its temporal targets inspectable.
    struct FrameInputs
    {
        std::shared_ptr<IR4Texture> sceneColor;
        std::shared_ptr<IR4Texture> position;
        std::shared_ptr<IR4Texture> accumulator;
        std::shared_ptr<IR4Texture> depth;
        std::shared_ptr<IR4Texture> environment0;
        std::shared_ptr<IR4Texture> environment1;
        std::shared_ptr<IR4Buffer> staticGlobals;
        std::array<float, 16> view{};
        std::array<float, 16> projection{};
        std::array<float, 16> previousView{};
        std::array<float, 16> previousProjection{};
        R4EnvironmentSnapshot environment{};
        bool cameraCut = false;
        bool previousValid = false;
    };

    MetalR4WaterAdapter();
    ~MetalR4WaterAdapter();
    MetalR4WaterAdapter(const MetalR4WaterAdapter&) = delete;
    MetalR4WaterAdapter& operator=(const MetalR4WaterAdapter&) = delete;

    bool Prepare(IR4Backend& backend, const std::vector<R4StaticWaterDrawRecord>& records,
        const Options& options, std::string& status);
    // Recreates all SSFX intermediates when the combined scene target changes.
    // Scene snapshot follows the combined target format; the four authored
    // SSFX water targets retain reference RGBA8 regardless of HDR scene format.
    bool PrepareRenderTargets(IR4Backend& backend, std::uint32_t width, std::uint32_t height,
        R4PixelFormat sceneFormat, std::string& error);
    bool Draw(IR4Backend& backend, const FrameInputs& inputs, std::string& error);
    void DiscardUnsubmittedFrame();
    void CommitSubmittedFrame();

    bool IsReady() const { return m_ready; }
    const std::string& Status() const { return m_status; }
    const Telemetry& LastTelemetry() const { return m_telemetry; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    bool m_ready = false;
    std::string m_status;
    Telemetry m_telemetry;
};
