#pragma once

#include "IR4Backend.h"
#include "R4DdsTexture.h"
#include "R4LevelGeometry.h"

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct R4StaticForwardViewState
{
    std::array<float, 16> view{};
    std::array<float, 16> projection{};
    std::array<float, 3> eye{};
    bool valid = false;
};

// Water is a distinct R4 phase, not an ordinary alpha-forward material.  The
// static-forward loader owns the immutable level buffers, so it publishes a
// narrow Metal-only record for the dedicated SSFX water subsystem rather than
// making that subsystem rediscover or borrow legacy dsgraph state.
struct R4StaticWaterDrawRecord
{
    struct VertexLayout
    {
        std::uint32_t stride = 0;
        std::size_t positionOffset = 0;
        std::size_t normalOffset = 0;
        std::size_t tangentOffset = 0;
        std::size_t binormalOffset = 0;
        std::size_t colorOffset = 0;
        std::size_t texcoordOffset = 0;
    } vertexLayout;
    std::uint32_t vertexBase = 0;
    std::uint32_t indexBase = 0;
    std::uint32_t indexCount = 0;
    std::array<float, 16> model{};
    xray::metal::R4PositionBounds bounds;
    std::string baseTextureIdentity;
    std::string normalTextureIdentity;
    std::shared_ptr<IR4Buffer> vertices;
    std::shared_ptr<IR4Buffer> indices;
    std::shared_ptr<IR4Texture> base;
    std::shared_ptr<IR4Texture> normalMap;
};

class MetalR4StaticForwardAdapter final
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
        std::size_t lightmappedVisuals = 0;
        std::size_t vertexColorVisuals = 0;
        std::size_t wallmarkVisuals = 0;
        std::size_t lightplaneVisuals = 0;
        std::size_t waterVisuals = 0;
        std::size_t transformedLightmappedVisuals = 0;
        std::size_t transformedVertexColorVisuals = 0;
        std::size_t transformedWaterVisuals = 0;
        std::size_t rejectedVisuals = 0;
        std::size_t textureAssets = 0;
        std::uint64_t uploadedBytes = 0;
        std::size_t submittedVisuals = 0;
        std::size_t culledVisuals = 0;
    };

    MetalR4StaticForwardAdapter();
    ~MetalR4StaticForwardAdapter();
    MetalR4StaticForwardAdapter(const MetalR4StaticForwardAdapter&) = delete;
    MetalR4StaticForwardAdapter& operator=(const MetalR4StaticForwardAdapter&) = delete;

    bool Prepare(IR4Backend& backend, const std::filesystem::path& levelDirectory,
        const xray::metal::R4LevelGeometry& geometry,
        const std::vector<std::shared_ptr<IR4Buffer>>& vertexBuffers,
        const std::vector<std::shared_ptr<IR4Buffer>>& indexBuffers,
        const std::shared_ptr<IR4Texture>& environmentCube,
        const Options& options, std::string& status);
    bool Upload(IR4Backend& backend, std::string& error);
    bool DrawIntoActiveForward(IR4Backend& backend, const R4StaticForwardViewState& view,
        const std::shared_ptr<IR4Buffer>& staticGlobals, std::string& error,
        bool includeWater = true);

    bool IsReady() const { return m_ready; }
    const std::string& Status() const { return m_status; }
    const Telemetry& LastTelemetry() const { return m_telemetry; }
    std::vector<R4StaticWaterDrawRecord> WaterDrawRecords() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    bool m_ready = false;
    std::string m_status;
    Telemetry m_telemetry;
};

bool RunMetalR4StaticForwardAdapterSelfTest(std::string& error);
