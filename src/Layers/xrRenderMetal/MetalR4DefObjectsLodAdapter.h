#pragma once

#include "IR4Backend.h"
#include "R4DdsTexture.h"
#include "R4LevelGeometry.h"

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct R4DefObjectsLodViewState
{
    std::array<float, 16> view{};
    std::array<float, 16> projection{};
    // The scene-owned b2 static_globals buffer is populated from the live R4
    // environment before G-buffer encoding. Steep parallax binds it directly.
    std::shared_ptr<IR4Buffer> staticGlobals;
    bool valid = false;
};

// Isolated B_VERT adapter. It never owns a render pass or engine scene state;
// callers provide the level's already-uploaded raw buffers and call Draw only
// inside the scene-owned production G-buffer encoder.
class MetalR4DefObjectsLodAdapter final
{
public:
    struct Options
    {
        std::filesystem::path shaderDirectory;
        xray::metal::R4DdsBytesProvider ddsBytesProvider;
    };
    struct Telemetry
    {
        std::size_t requestedVisuals = 0;
        std::size_t flatVisuals = 0;
        std::size_t bumpVisuals = 0;
        std::size_t steepVisuals = 0;
        std::size_t rejectedVisuals = 0;
        std::size_t textureAssets = 0;
        std::uint64_t uploadedBytes = 0;
        std::size_t submittedVisuals = 0;
        std::size_t culledVisuals = 0;
    };

    MetalR4DefObjectsLodAdapter();
    ~MetalR4DefObjectsLodAdapter();
    MetalR4DefObjectsLodAdapter(const MetalR4DefObjectsLodAdapter&) = delete;
    MetalR4DefObjectsLodAdapter& operator=(const MetalR4DefObjectsLodAdapter&) = delete;

    bool Prepare(IR4Backend& backend, const xray::metal::R4LevelGeometry& geometry,
        const std::vector<std::shared_ptr<IR4Buffer>>& vertexBuffers,
        const std::vector<std::shared_ptr<IR4Buffer>>& indexBuffers,
        const Options& options, std::string& status);
    bool Upload(IR4Backend& backend, std::string& error);
    bool DrawIntoActiveGBuffer(IR4Backend& backend, const R4DefObjectsLodViewState& view,
        std::string& error);
    bool IsReady() const { return m_ready; }
    const Telemetry& LastTelemetry() const { return m_telemetry; }
    const std::string& Status() const { return m_status; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    bool m_ready = false;
    std::string m_status;
    Telemetry m_telemetry;
};

bool RunMetalR4DefObjectsLodAdapterSelfTest(std::string& error);
