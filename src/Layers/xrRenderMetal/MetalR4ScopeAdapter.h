#pragma once

#include "IR4Backend.h"
#include "R4DdsTexture.h"

#include <filesystem>
#include <memory>
#include <string>

class MetalR4ScopeAdapter final
{
public:
    struct Options
    {
        std::filesystem::path shaderDirectory;
        xray::metal::R4DdsBytesProvider ddsBytesProvider;
    };

    struct PassInputs
    {
        std::shared_ptr<IR4Texture> depthSnapshot;
        std::shared_ptr<IR4Texture> sceneColorSnapshot;
        std::shared_ptr<IR4Texture> position;
        std::shared_ptr<IR4Texture> tonemap;
        std::shared_ptr<IR4Texture> heat;
        std::shared_ptr<IR4Buffer> staticGlobals;
    };

    struct Telemetry
    {
        std::uint32_t frame = 0;
        std::size_t triplets = 0;
        std::size_t prepared = 0;
        std::size_t backDraws = 0;
        std::size_t zwriteDraws = 0;
        std::size_t reticleDraws = 0;
        std::size_t rejected = 0;
    };

    MetalR4ScopeAdapter();
    ~MetalR4ScopeAdapter();

    bool Configure(IR4Backend& backend, const Options& options, std::string& status);
    bool Prewarm(IR4Backend& backend,
        const std::shared_ptr<const R4EngineHudSnapshot>& snapshot,
        std::string& error);
    bool DrawBack(IR4Backend& backend,
        const std::shared_ptr<const R4EngineHudSnapshot>& snapshot,
        std::string& error);
    bool DrawZwrite(IR4Backend& backend,
        const std::shared_ptr<const R4EngineHudSnapshot>& snapshot,
        const PassInputs& inputs, std::string& error);
    bool DrawReticle(IR4Backend& backend,
        const std::shared_ptr<const R4EngineHudSnapshot>& snapshot,
        const PassInputs& inputs, std::string& error);

    bool IsReady() const { return m_ready; }
    const std::string& Status() const { return m_status; }
    const Telemetry& LastTelemetry() const { return m_telemetry; }

private:
    bool DrawRole(IR4Backend& backend,
        const std::shared_ptr<const R4EngineHudSnapshot>& snapshot,
        R4EngineHudRenderable::ScopeRole role, const PassInputs* inputs,
        std::string& error);
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    bool m_ready = false;
    std::string m_status;
    Telemetry m_telemetry;
};

bool RunMetalR4ScopeAdapterSelfTest(std::string& error);
