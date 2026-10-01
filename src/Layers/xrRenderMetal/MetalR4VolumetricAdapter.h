#pragma once

#include "IR4Backend.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

// The active R4 volumetric chain, built from the original MSC-converted
// shaders.  Engine-owned values cross this boundary as immutable snapshots;
// no light, frustum, or shader object is retained here.
class MetalR4VolumetricAdapter final
{
public:
    struct Options
    {
        std::filesystem::path shaderDirectory;
    };

    struct FrameInputs
    {
        std::uint32_t engineFrame = 0;
        std::shared_ptr<IR4Texture> position;
        std::shared_ptr<IR4Buffer> staticGlobals;
    };

    struct SunPass
    {
        std::shared_ptr<IR4Texture> shadowMap;
        std::shared_ptr<IR4Buffer> dynamicLight;
        std::shared_ptr<IR4Buffer> vertexGlobals;
        std::shared_ptr<IR4Buffer> vertexTransforms;
        std::shared_ptr<IR4Buffer> volumeVertices;
        std::shared_ptr<IR4Buffer> volumeIndices;
        std::array<float, 16> shadowMatrix{};
        float nearRange = 0.0f;
        float farRange = 0.0f;
        float shaftsIntensity = 0.0f;
        std::uint32_t indexCount = 48u;
    };

    struct SpotPass
    {
        // These are the atlas and projector for the current light only.  The
        // caller must invoke DrawSpot before the atlas tile is reused.
        std::shared_ptr<IR4Texture> shadowAtlas;
        std::shared_ptr<IR4Texture> lightMap;
        std::array<float, 16> shadowMatrix{};
        // Full captured record matrix. The shader's two float4 m_lmap vectors
        // consume the first eight floats exactly as the existing local-light
        // adapter's reflected MSC ABI does.
        std::array<float, 16> lightMapMatrix{};

        // Source accum_volumetric.vs inputs, already captured in view space.
        std::array<float, 4> boundsMin{};
        std::array<float, 4> boundsMax{};
        std::array<std::array<float, 4>, 6> frustumClipPlanes{};
        // Source dynamic_light cbuffer: Ldynamic_color then Ldynamic_pos.
        // Position.w is the source 1 / (m_volumetric_distance * range * .95)^2.
        std::array<float, 4> dynamicColor{};
        std::array<float, 4> dynamicPositionAttenuation{};
        std::uint32_t sliceCount = 0;
    };

    struct CompositeInputs
    {
        std::shared_ptr<IR4Texture> blueNoise;
        std::shared_ptr<IR4Texture> lightingLow;
        std::shared_ptr<IR4Texture> lightingHigh;
    };

    struct Telemetry
    {
        std::size_t sunDraws = 0;
        std::size_t spotDraws = 0;
        std::size_t blurDraws = 0;
        bool spotMerged = false;
        bool finalComposited = false;
    };

    MetalR4VolumetricAdapter();
    ~MetalR4VolumetricAdapter();
    MetalR4VolumetricAdapter(const MetalR4VolumetricAdapter&) = delete;
    MetalR4VolumetricAdapter& operator=(const MetalR4VolumetricAdapter&) = delete;

    bool Configure(IR4Backend& backend, const Options& options, std::string& error);
    bool PrepareRenderTargets(IR4Backend& backend, std::uint32_t width,
        std::uint32_t height, std::uint32_t volsize, std::string& error);
    bool BeginFrame(IR4Backend& backend, const FrameInputs& inputs, std::string& error);
    bool DrawSun(IR4Backend& backend, const SunPass& pass, std::string& error);
    bool DrawSpot(IR4Backend& backend, const SpotPass& pass, std::string& error);
    bool BlurAndMerge(IR4Backend& backend, std::string& error);
    bool DrawFinalComposite(IR4Backend& backend, const CompositeInputs& inputs,
        std::string& error);

    bool IsReady() const { return m_ready; }
    const std::shared_ptr<IR4Texture>& OutputTexture() const;
    const Telemetry& LastTelemetry() const { return m_telemetry; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    bool m_ready = false;
    Telemetry m_telemetry;
};
