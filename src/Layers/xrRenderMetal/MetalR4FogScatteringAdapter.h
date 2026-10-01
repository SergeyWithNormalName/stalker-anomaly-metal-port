#pragma once

#include "IR4Backend.h"

#include <filesystem>
#include <memory>
#include <string>

// Exact active-profile SSFX fog-scattering leaf: quarter and half resolution
// blur, followed by the original full-resolution fog-scattering composite.
class MetalR4FogScatteringAdapter final
{
public:
    struct Options
    {
        std::filesystem::path shaderDirectory;
    };

    MetalR4FogScatteringAdapter();
    ~MetalR4FogScatteringAdapter();
    MetalR4FogScatteringAdapter(const MetalR4FogScatteringAdapter&) = delete;
    MetalR4FogScatteringAdapter& operator=(const MetalR4FogScatteringAdapter&) = delete;

    bool Prepare(IR4Backend& backend, const Options& options, std::string& error);
    bool PrepareRenderTargets(IR4Backend& backend, std::uint32_t width, std::uint32_t height,
        R4PixelFormat colorFormat, std::string& error);
    bool Draw(IR4Backend& backend, const std::shared_ptr<IR4Texture>& position,
        const std::shared_ptr<IR4Texture>& sceneColor,
        const std::shared_ptr<IR4Buffer>& staticGlobals,
        const R4EnvironmentSnapshot& environment, std::string& error);

    const std::shared_ptr<IR4Texture>& Output() const;
    bool IsReady() const { return m_ready; }
    const std::string& Status() const { return m_status; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    bool m_ready = false;
    std::string m_status;
};
