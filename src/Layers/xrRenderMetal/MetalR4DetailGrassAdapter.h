#pragma once

#include "IR4Backend.h"
#include "R4DdsTexture.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Owns only decoded level.details data.  It deliberately contains no D3D
// resource and can therefore be built by the Direct Metal lifecycle before a
// scene adapter is wired into an active G-buffer encoder.
struct R4DetailGrassModel
{
    struct Vertex
    {
        std::array<float, 3> position{};
        float u = 0.0f;
        float v = 0.0f;
    };

    std::string shaderName;
    std::string textureName;
    std::uint32_t flags = 0;
    float minimumScale = 0.0f;
    float maximumScale = 0.0f;
    std::vector<Vertex> vertices;
    std::vector<std::uint16_t> indices;
    std::array<float, 3> boundsMinimum{};
    std::array<float, 3> boundsMaximum{};
};

struct R4DetailGrassSlot
{
    std::array<std::uint16_t, 4> modelIds{};
    std::array<std::array<std::uint8_t, 4>, 4> palette{};
    float yBase = 0.0f;
    float yHeight = 0.0f;
    float sun = 0.0f;
    float hemi = 0.0f;
};

struct R4DetailGrassSnapshot
{
    std::uint32_t version = 0;
    std::int32_t offsetX = 0;
    std::int32_t offsetZ = 0;
    std::uint32_t sizeX = 0;
    std::uint32_t sizeZ = 0;
    std::vector<R4DetailGrassModel> models;
    std::vector<R4DetailGrassSlot> slots;
};

// Parses the byte-identical level.details chunk structure exposed by XRFS.
// Only v3/v4 are accepted; malformed chunks and all out-of-range model indices
// fail closed rather than falling back to undefined placement data.
bool LoadR4DetailGrassSnapshot(const std::vector<std::uint8_t>& bytes,
    R4DetailGrassSnapshot& snapshot, std::string& error);

// MSC deffer_grass uses float4 POSITION plus int4 TEXCOORD0.  The historical
// D3D detail stream was float3 + SHORT4; this explicit representation prevents
// Metal vertex fetch from silently applying a different short conversion.
struct R4DetailGrassVertex
{
    float position[4]{};
    std::int32_t misc[4]{};
};
static_assert(sizeof(R4DetailGrassVertex) == 32u,
    "deffer_grass MSC vertex ABI must be float4 plus int4");

struct R4DetailGrassInstance
{
    std::array<float, 16> transform{};
    // Exact CDetailManager Item.position (the decompressed slot visibility
    // sphere center), not the random grounded blade transform translation.
    std::array<float, 3> shadowFadePosition{};
    float shadowDistanceSquared = 0.0f;
    bool hasShadowFadePosition = false;
    std::array<float, 3> terrainNormal{};
    float sun = 0.0f;
    float hemi = 0.0f;
    float alpha = 1.0f;
};

// Value-only copy of the exact inputs set by dx10DetailManager_VS.cpp for
// the SSFX grass variant. No D3D constant buffer or DetailManager object is
// retained by the Metal path.
struct R4DetailGrassMotionSnapshot
{
    std::uint32_t engineFrame = 0;
    std::array<std::array<float, 4>, 32> previousBenders{};
    std::array<std::array<float, 4>, 32> currentBenders{};
    std::array<float, 4> bendersSetup{};
    std::array<float, 4> windParams{};
    std::array<float, 4> windSetupGrass{};
    std::array<float, 4> windSetupTrees{};
    std::array<float, 4> windAnimation{};
    std::array<float, 4> windAnimationPrevious{};
    std::int32_t grassAlign = 0;
    std::uint32_t activeBenders = 0;
    bool valid = false;
};

// A frame is intentionally expressed in the decoded model-ID domain retained
// by SetSnapshot().  The engine-side expander may therefore discard a bad
// ground query before it ever reaches the Metal resource bridge.
struct R4DetailGrassBatch
{
    std::uint16_t modelId = 0;
    std::vector<R4DetailGrassInstance> instances;
    std::array<float, 3> boundsMinimum{};
    std::array<float, 3> boundsMaximum{};
    bool hasBounds = false;
};

struct R4DetailGrassFrame
{
    std::uint64_t generation = 0;
    std::vector<R4DetailGrassBatch> batches;
    std::size_t requestedInstances = 0;
    std::size_t groundedInstances = 0;
    std::size_t rejectedInstances = 0;
    std::size_t cacheSlots = 0;
    std::size_t distanceRejectedSlots = 0;
    // Informational only: models marked DO_NO_WAVING. The converted SSFX_WIND
    // permutation does not consume legacy Wave0/Wave1 constants, so this is
    // deliberately not a draw-bin selector.
    std::size_t stillInstances = 0;
    float cacheFadeDistance = 0.0f;
};

struct R4DetailGrassViewState
{
    std::array<float, 16> view{};
    std::array<float, 16> projection{};
    std::array<float, 16> previousView{};
    std::array<float, 16> previousProjection{};
    std::array<float, 3> eye{};
    float timeSeconds = 0.0f;
    // The values below are copied from the live R4 environment snapshot.  Do
    // not manufacture lighting in this adapter: zero is a valid controlled
    // fallback only while the engine snapshot itself is unavailable.
    std::array<float, 3> ambient{};
    std::array<float, 4> hemiColor{};
    std::array<float, 3> sunColor{};
    std::array<float, 3> sunDirection{};
    std::array<float, 3> fogColor{};
    float fogDensity = 0.0f;
    float fogNear = 0.0f;
    float fogFar = 0.0f;
    bool valid = false;
    bool previousValid = false;
};

struct R4DetailGrassShadowState
{
    std::array<float, 16> lightView{};
    std::array<float, 16> lightProjection{};
    std::array<float, 16> lightViewProjection{};
    // Windows uses Instance.distance for sun fade and squared XZ distance to
    // light_position for local-light fades. Its local shadow path also rejects
    // each Item.position at full 3D distance >= lightRange.
    float fadeDistance = 0.0f;
    std::array<float, 3> lightPosition{};
    float lightRange = 0.0f;
    bool fadeFromLight = false;
};

constexpr std::size_t kR4DetailGrassBatchSize = 61u;

// Repeats a source detail mesh for at most 61 matrices, mirroring the
// deffer_grass array[61 * 4] / exdata[61] contract.  The returned index stream
// is local to the returned vertex stream and fit for a uint16 draw.
bool RepackR4DetailGrassBatch(const R4DetailGrassModel& model,
    const std::vector<R4DetailGrassInstance>& instances,
    std::vector<R4DetailGrassVertex>& vertices,
    std::vector<std::uint16_t>& indices,
    std::string& error);

class MetalR4DetailGrassAdapter final
{
public:
    struct Options
    {
        std::filesystem::path vertexMetallibPath;
        std::filesystem::path fragmentMetallibPath;
        // Optional dedicated depth-output bundle. The vertex shader is the
        // production deffer_grass permutation; its wrapper PS preserves the
        // original alpha/dither discard behavior.
        std::filesystem::path shadowShaderDirectory;
        xray::metal::R4DdsBytesProvider ddsBytesProvider;
        std::filesystem::path wavesTexturePath;
        // Resolves details\blend's s_base and exact level-local s_bump.
        // Returning false rejects only the affected model/batch.
        std::function<bool(const std::string&, std::filesystem::path&,
            std::filesystem::path&, std::string&)> resolveMaterialTextures;
    };

    struct Telemetry
    {
        std::size_t models = 0;
        std::size_t slots = 0;
        std::size_t requestedInstances = 0;
        std::size_t admittedInstances = 0;
        std::size_t rejectedBatches = 0;
        std::size_t draws = 0;
        std::size_t textureRejectedBatches = 0;
        std::size_t frustumRejectedBatches = 0;
        std::size_t distanceRejectedSlots = 0;
        std::size_t stillInstances = 0;
        std::size_t activeBenders = 0;
        std::size_t motionCaptureFailures = 0;
        std::size_t shadowDraws = 0;
        std::size_t shadowCulledBatches = 0;
        std::size_t shadowRejected = 0;
        float cacheFadeDistance = 0.0f;
    };

    MetalR4DetailGrassAdapter();
    ~MetalR4DetailGrassAdapter();
    MetalR4DetailGrassAdapter(const MetalR4DetailGrassAdapter&) = delete;
    MetalR4DetailGrassAdapter& operator=(const MetalR4DetailGrassAdapter&) = delete;

    // Configures only the proven deffer_grass pipeline and wind-wave texture.
    // It does not install a scene callback or issue a draw.
    bool Configure(IR4Backend& backend, const Options& options, std::string& status);

    // Retains authoritative decoded data for a later scene-owned draw bridge.
    // It accepts only details\\blend models; all other level.details materials
    // remain outside this narrowly proven adapter.
    bool SetSnapshot(const R4DetailGrassSnapshot& snapshot, std::string& error);

    // Called before the scene opens its G-buffer pass.  It uploads only
    // strictly validated <=61-instance batches; malformed models and failed
    // texture resolution are isolated and counted instead of poisoning the
    // rest of the frame.
    bool StageFrame(IR4Backend& backend, const R4DetailGrassFrame& frame,
        std::string& error);

    // Requires an already-open scene-owned G-buffer pass.
    bool DrawIntoActiveGBuffer(IR4Backend& backend,
        const R4DetailGrassViewState& view, std::string& error);

    // Requires an already-open scene-owned sun/local shadow pass. The state
    // carries only the Windows pass matrix/fade inputs; all wind, materials,
    // placements, and lighting remain the same as the visible adapter draw.
    bool DrawShadowCasters(IR4Backend& backend,
        const R4DetailGrassViewState& view,
        const R4DetailGrassShadowState& shadow, std::string& error);

    bool HasShadowCasters() const;

    bool IsReady() const { return m_ready; }
    const R4DetailGrassSnapshot* Snapshot() const;
    const std::string& Status() const { return m_status; }
    const Telemetry& LastTelemetry() const { return m_telemetry; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    bool m_ready = false;
    std::string m_status;
    Telemetry m_telemetry;
};

bool RunR4DetailGrassAdapterSelfTest(std::string& error);
