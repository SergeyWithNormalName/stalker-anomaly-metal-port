#pragma once

#include "IR4Backend.h"
#include "R4DdsTexture.h"

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// Converts the bounded CPU-only OGF payload emitted by the engine bridge into
// the reflected deffer_base_bump vertex ABI. It is deliberately separate from
// MetalR4EngineScene so bundle admission and raw-data tests remain possible
// without a live SDL/Metal frame.
struct R4DynamicWorldViewState
{
    std::array<float, 16> view{};
    std::array<float, 16> projection{};
    bool valid = false;
};

struct R4DynamicWorldBumpVertex
{
    float normal[4];
    float tangent[4];
    float binormal[4];
    std::int16_t texcoord[2];
    float position[4];
};
static_assert(sizeof(R4DynamicWorldBumpVertex) == 68,
    "deffer_base_bump reflected FLOAT4/FLOAT4/FLOAT4/SHORT2/FLOAT4 ABI must be tightly packed");

bool RepackR4DynamicWorldBumpVertices(
    const R4EngineWorldGeometry& source,
    std::vector<R4DynamicWorldBumpVertex>& vertices,
    std::string& error);

// Hardware-style SKIN_0 input after CPU skinning of a raw vertBoned OGF.
// It matches the live deffer_model_bump SKIN_0 reflection exactly.
struct R4DynamicWorldSkinnedVertex
{
    float position[4];
    std::uint8_t normal[4];
    std::uint8_t tangent[4];
    std::uint8_t binormal[4];
    float texcoord[2];
};
static_assert(sizeof(R4DynamicWorldSkinnedVertex) == 36,
    "deffer_model_bump SKIN_0 ABI must match the 36-byte hardware vertex layout");

bool RepackR4DynamicWorldSkinned1WVertices(
    const R4EngineWorldGeometry& source,
    const std::shared_ptr<const std::vector<std::array<float, 16>>>& boneTransforms,
    std::vector<R4DynamicWorldSkinnedVertex>& vertices,
    std::string& error);

// Admits raw vertBoned1W/2W/3W/4W payloads after strictly validating their
// packed ABI, bone indices, and finite blend weights. Signed implicit residuals
// retain the exact xrSkin3W/xrSkin4W semantics. All variants
// are CPU-skinned into the proven SKIN_0 vertex stream; no previous-bone GPU
// state is required.
bool RepackR4DynamicWorldSkinnedVertices(
    const R4EngineWorldGeometry& source,
    const std::shared_ptr<const std::vector<std::array<float, 16>>>& boneTransforms,
    std::vector<R4DynamicWorldSkinnedVertex>& vertices,
    std::string& error);

bool ResolveR4DynamicWorldBumpTextures(
    const std::string& textureName,
    std::filesystem::path& basePath,
    std::filesystem::path& bumpPath,
    std::string& error);

bool RunR4DynamicWorldAdapterSelfTest(std::string& error);

class MetalR4DynamicWorldAdapter final
{
public:
    MetalR4DynamicWorldAdapter();
    ~MetalR4DynamicWorldAdapter();
    MetalR4DynamicWorldAdapter(const MetalR4DynamicWorldAdapter&) = delete;
    MetalR4DynamicWorldAdapter& operator=(const MetalR4DynamicWorldAdapter&) = delete;

    struct Options
    {
        std::filesystem::path vertexMetallibPath;
        std::filesystem::path fragmentMetallibPath;
        std::filesystem::path modelVertexMetallibPath;
        // Optional exact flat siblings. When omitted, Configure probes the
        // bump bundle directory for deffer_base_flat and deffer_model_flat.
        std::filesystem::path flatVertexMetallibPath;
        std::filesystem::path flatFragmentMetallibPath;
        std::filesystem::path modelFlatVertexMetallibPath;
        std::filesystem::path shadowVertexMetallibPath;
        std::filesystem::path shadowFragmentMetallibPath;
        xray::metal::R4DdsBytesProvider ddsBytesProvider;
    };

    // Missing bundles leave the adapter disabled rather than compromising the
    // static scene. A present but mismatched reflection is also fail-closed.
    bool Configure(IR4Backend& backend, const Options& options, std::string& status);

    // Must run before the scene opens its G-buffer pass: private texture
    // uploads are intentionally prohibited while a render encoder is active.
    bool Prewarm(IR4Backend& backend,
        const R4DynamicWorldViewState& view,
        const std::shared_ptr<const R4EngineWorldSnapshot>& world,
        std::string& error);

    // Called only by the scene-owned in-pass callback. It never creates,
    // ends, or submits a pass; it binds cached resources and draws only assets
    // admitted during Prewarm.
    bool Draw(IR4Backend& backend,
        const R4DynamicWorldViewState& view,
        const std::shared_ptr<const R4EngineWorldSnapshot>& world,
        std::string& error);

    // Renders dynamic world casters into the currently active local shadow atlas tile pass.
    // The viewport, scissor, and shadow render pass are already active on backend.
    bool DrawLocalShadowTile(IR4Backend& backend,
        const std::array<float, 16>& lightViewProjection,
        const std::shared_ptr<const R4EngineWorldSnapshot>& world,
        std::size_t& candidates,
        std::size_t& submitted,
        std::size_t& culled,
        std::size_t& rejected,
        std::string& error);

    bool IsEnabled() const { return m_enabled; }
    const std::string& Status() const { return m_status; }
    struct Telemetry
    {
        std::uint32_t engineFrame = 0;
        std::size_t renderables = 0;
        std::size_t cachedGeometry = 0;
        std::size_t rejectedGeometry = 0;
        std::size_t skinnedCandidates = 0;
        std::size_t nonSkinnedCandidates = 0;
        std::size_t skinned1WCandidates = 0;
        std::size_t skinned2WCandidates = 0;
        std::size_t skinned3WCandidates = 0;
        std::size_t skinned4WCandidates = 0;
        std::size_t skinned1WRejected = 0;
        std::size_t skinned2WRejected = 0;
        std::size_t skinned3WRejected = 0;
        std::size_t skinned4WRejected = 0;
        std::size_t skinnedBoundsRejected = 0;
        std::size_t frustumRejected = 0;
        std::size_t bumpMaterialGeometry = 0;
        std::size_t flatMaterialGeometry = 0;
        std::size_t flatMaterialUnavailable = 0;
        std::size_t skinnedDraws = 0;
        std::size_t skinned1WDraws = 0;
        std::size_t skinned2WDraws = 0;
        std::size_t skinned3WDraws = 0;
        std::size_t skinned4WDraws = 0;
        float largestSkinnedWorldRadius = 0.0f;
        float largestSkinnedExpectedRadius = 0.0f;
        std::size_t instanceCacheHits = 0;
        std::size_t instanceCacheMisses = 0;
        std::size_t instanceCachePruned = 0;
        std::size_t instanceCacheSaturated = 0;
        std::size_t skinnedVerticesReused = 0;
		// HeatVision values which passed the legacy active-texture gate at
		// capture time, and the submitted dynamic G-buffer draws that received
		// their non-zero L_hotness b0 payload.
		std::size_t hotRenderables = 0;
		std::size_t hotDraws = 0;
        std::size_t shadowDraws = 0;
        std::size_t shadowSkinnedDraws = 0;
        std::size_t draws = 0;
    };
    const Telemetry& LastTelemetry() const { return m_telemetry; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    bool m_enabled = false;
    std::string m_status;
    Telemetry m_telemetry;
};
