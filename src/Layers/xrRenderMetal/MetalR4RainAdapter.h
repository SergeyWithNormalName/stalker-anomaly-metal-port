#pragma once

#include "IR4Pipeline.h"
#include "IR4Resources.h"
#include "R4DdsTexture.h"
#include "../xrRender/MetalRainCapture.h"

#include <array>
#include <filesystem>
#include <memory>
#include <string>

class IR4Backend;

// Owns the native Metal half of the legacy CEffect_Rain sprite pass.  It uses
// the exact effects_rain HLSL pair and consumes CPU-generated quads only;
// weather simulation, ray hits, and density policy remain engine-authoritative.
class MetalR4RainAdapter final
{
public:
    struct Options
    {
        std::filesystem::path shaderDirectory;
        xray::metal::R4DdsBytesProvider ddsBytesProvider;
    };

    struct Telemetry
    {
        std::size_t capturedVertices = 0u;
        std::size_t submittedDrops = 0u;
        std::size_t droppedVertices = 0u;
        float density = 0.0f;
        bool ssfx = false;
    };

    MetalR4RainAdapter();
    ~MetalR4RainAdapter();
    MetalR4RainAdapter(const MetalR4RainAdapter&) = delete;
    MetalR4RainAdapter& operator=(const MetalR4RainAdapter&) = delete;

    bool Configure(IR4Backend& backend, const Options& options, std::string& status);
    bool StageFrame(IR4Resources& resources, xray::metal::rain::FrameBatch frame, std::string& error);
    bool PrepareRenderTargets(IR4Backend& backend, std::string& error);
    bool DrawRefraction(IR4Backend& backend, const std::shared_ptr<IR4Texture>& sceneColor,
        const std::shared_ptr<IR4Texture>& volumetric, const std::shared_ptr<IR4Buffer>& staticGlobals,
        std::string& error);
    bool DrawIntoActiveForward(IR4Backend& backend, const std::array<float, 16>& viewProjection,
        const std::shared_ptr<IR4Texture>& position, const std::shared_ptr<IR4Texture>& sceneColor,
        const std::shared_ptr<IR4Texture>& accumulator, const std::shared_ptr<IR4Buffer>& staticGlobals,
        std::string& error);

    bool IsReady() const { return m_ready; }
    bool HasDrops() const;
    const std::shared_ptr<IR4Texture>& MotionTarget() const;
    const std::shared_ptr<IR4Texture>& UnusedMrtTarget() const;
    const std::shared_ptr<IR4Texture>& RefractionTarget() const;
    const Telemetry& LastTelemetry() const { return m_telemetry; }
    const std::string& Status() const { return m_status; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    bool m_ready = false;
    std::string m_status;
    Telemetry m_telemetry;
};
