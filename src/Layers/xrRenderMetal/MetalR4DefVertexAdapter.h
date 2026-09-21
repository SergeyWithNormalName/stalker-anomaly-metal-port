#pragma once

#include "IR4Backend.h"
#include "R4DdsTexture.h"
#include "R4LevelGeometry.h"

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// Owns the isolated static-color R4 `def_shaders\\def_vertex` admission.
// It never opens or closes a render pass; MetalR4EngineScene calls Draw only
// while its production four-target G-buffer encoder is active.
struct R4DefVertexViewState
{
    std::array<float, 16> view{};
    std::array<float, 16> projection{};
    bool valid = false;
};

class MetalR4DefVertexAdapter final
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
        std::size_t admittedVisuals = 0;
        std::size_t alphaTestedVisuals = 0;
        std::size_t rejectedVisuals = 0;
        std::size_t textureAssets = 0;
        std::uint64_t textureBytes = 0;
        std::size_t submittedVisuals = 0;
        std::size_t alphaTestedSubmittedVisuals = 0;
        std::size_t culledVisuals = 0;
        std::size_t draws = 0;
    };

    MetalR4DefVertexAdapter();
    ~MetalR4DefVertexAdapter();
    MetalR4DefVertexAdapter(const MetalR4DefVertexAdapter&) = delete;
    MetalR4DefVertexAdapter& operator=(const MetalR4DefVertexAdapter&) = delete;

    // Reuses the already-owned, raw R4 level vertex/index buffers only after
    // the exact 32-byte P3/C4/C4/C4/C4/S2 source ABI is proven.
    bool Prepare(IR4Backend& backend, const xray::metal::R4LevelGeometry& geometry,
        const std::vector<std::shared_ptr<IR4Buffer>>& vertexBuffers,
        const std::vector<std::shared_ptr<IR4Buffer>>& indexBuffers,
        const Options& options, std::string& status);

    // Must run before the G-buffer pass: uploads private texture resources
    // once and releases retained CPU DDS bytes after success.
    bool Upload(IR4Backend& backend, std::string& error);

    bool DrawIntoActiveGBuffer(IR4Backend& backend, const R4DefVertexViewState& view,
        std::string& error);

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

bool RunMetalR4DefVertexAdapterSelfTest(std::string& error);
