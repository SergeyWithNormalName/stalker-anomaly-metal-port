#pragma once

#include "IR4Backend.h"
#include "R4DdsTexture.h"
#include "R4LocalShadowAtlasFoundation.h"
#include "R4LightMarker.h"

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
        // Explicit resource for standalone GPU fixtures without engine XRFS.
        // The engine leaves this empty and loads the archive-winning cookie.
        std::shared_ptr<IR4Texture> defaultSpotCookie;
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
        std::size_t markerResets = 0;
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

    // One cursor and telemetry epoch spans every atlas batch in a GPU frame.
    struct DrawState
    {
        xray::metal::R4LightMarker marker;
        std::vector<R4WholeLightVisibilitySample> visibilitySamples;
        std::int64_t activeVisibilityBatch = -1;
    };
    void BeginFrame() { m_telemetry = {}; }
    bool DrawBatchIntoActiveAccumulator(IR4Backend& backend, const GBufferInputs& inputs,
        const ViewState& view, const std::shared_ptr<const R4EngineLocalLightSnapshot>& snapshot,
        const std::vector<std::size_t>& lightIndices, DrawState& state, std::string& error);

    bool ResolveProjectorTexture(IR4Backend& backend, const R4EngineLocalLight& light,
        std::shared_ptr<IR4Texture>& texture, std::string& error);

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
    bool m_activeOcclusionTransformsRequested = false;
    std::string m_status;
    Telemetry m_telemetry;
};

// Pure helper for the camera VP used by the local-light adapter. It shares the
// adapter's historical `multiply(projection, view)` implementation so the
// diagnostic query cannot silently substitute a separately ordered multiply.
void BuildMetalR4LocalLightCameraViewProjection(
    const std::array<float, 16>& view,
    const std::array<float, 16>& projection,
    std::array<float, 16>& viewProjection);

// These two helpers share the exact model construction and multiply used by
// the ordinary adapter upload path. They expose numeric provenance only.
void BuildMetalR4LocalLightModel(const R4EngineLocalLight& light,
    std::array<float, 16>& model, std::array<float, 4>* localPositionAbsMax = nullptr);
void BuildMetalR4LocalLightWorldViewProjection(
    const std::array<float, 16>& cameraViewProjection,
    const std::array<float, 16>& model,
    std::array<float, 16>& worldViewProjection);

bool RunMetalR4LocalLightAdapterSelfTest(std::string& error);

// Conservative world AABB of the exact accumulator mesh/model, not the lamp
// position or shadow frustum. Diagnostic only; invalid bases fail open.
bool BuildMetalR4LocalLightVolumeBounds(const R4EngineLocalLight& light,
    std::array<float, 3>& minimum, std::array<float, 3>& maximum);
