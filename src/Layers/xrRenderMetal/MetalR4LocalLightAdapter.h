#pragma once

#include "IR4Backend.h"
#include "R4DdsTexture.h"
#include "R4LocalShadowAtlasFoundation.h"

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// Encodes the exact non-MSAA unshadowed point/spot MSC paths into scene-owned
// accumulator passes. It owns only value snapshots and Metal resources; the
// engine's light/shadow/D3D state never crosses this boundary.
class MetalR4LocalLightAdapter final
{
public:
    struct Options
    {
        std::filesystem::path shaderDirectory;
        xray::metal::R4DdsBytesProvider ddsBytesProvider;
    };

    struct GBufferInputs
    {
        std::shared_ptr<IR4Texture> position;
        std::shared_ptr<IR4Texture> diffuse;
        std::shared_ptr<IR4Buffer> staticGlobals;
        std::shared_ptr<IR4Sampler> noFilterSampler;
        std::shared_ptr<IR4Sampler> linearSampler;
        std::shared_ptr<IR4Texture> shadowAtlas;
        std::shared_ptr<IR4Sampler> shadowSampler;
        const std::vector<xray::metal::R4LocalShadowAtlasRecord>* shadowRecords = nullptr;
    };

    struct ViewState
    {
        std::array<float, 16> view{};
        std::array<float, 16> projection{};
        std::array<float, 3> eye{};
        bool valid = false;
    };

    struct Telemetry
    {
        std::size_t requested = 0;
        std::size_t admitted = 0;
        std::size_t submitted = 0;
        std::size_t culled = 0;
        std::size_t skippedUnavailable = 0;
        std::size_t skippedInactive = 0;
        std::size_t skippedShadowed = 0;
        std::size_t submittedShadowed = 0;
        std::size_t skippedShadowedPoint = 0;
        std::size_t skippedUnsupported = 0;
        std::size_t skippedInvalid = 0;
		std::size_t skippedCookie = 0;
    };

    MetalR4LocalLightAdapter();
    ~MetalR4LocalLightAdapter();
    MetalR4LocalLightAdapter(const MetalR4LocalLightAdapter&) = delete;
    MetalR4LocalLightAdapter& operator=(const MetalR4LocalLightAdapter&) = delete;

    bool Prepare(IR4Backend& backend, const Options& options, std::string& status);
    bool DrawIntoActiveAccumulator(IR4Backend& backend, const GBufferInputs& inputs,
        const ViewState& view, const std::shared_ptr<const R4EngineLocalLightSnapshot>& snapshot,
        std::string& error);

    bool IsReady() const { return m_ready; }
    const std::string& Status() const { return m_status; }
    const Telemetry& LastTelemetry() const { return m_telemetry; }

	// Opaque implementation is public only so its renderer-local cookie cache
	// can be implemented in this translation unit; ownership stays private.
public:
    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
    bool m_ready = false;
    std::string m_status;
    Telemetry m_telemetry;
};

bool RunMetalR4LocalLightAdapterSelfTest(std::string& error);
