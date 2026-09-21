#pragma once

#include "R4DdsTexture.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// General binary equivalent of the DDS provider.  The level.details loader
// uses this rather than a host filesystem path so packed GAMMA archives keep
// the same winning-file semantics as the engine.
using R4BinaryBytesProvider = std::function<bool(const std::filesystem::path&,
    std::size_t, std::vector<std::uint8_t>&, std::string&)>;

struct R4DetailGrassSnapshot;
struct R4DetailGrassFrame;
struct R4DetailGrassMotionSnapshot;

// Returns an archive-winning xrfs: provider only in the engine-linked R4
// build. The standalone bootstrap deliberately gets a fail-closed error.
bool CreateMetalR4EngineXrfsProvider(
    xray::metal::R4DdsBytesProvider& provider,
    std::string& error);

// Returns bytes from the currently assigned $level$/level.details archive
// entry.  It intentionally fails outside the engine-linked R4 target and
// never silently substitutes a host-side loose file.
bool CreateMetalR4EngineLevelDetailsProvider(
    R4BinaryBytesProvider& provider,
    std::string& error);

// Reads and decodes the archive-winning level.details file once $level$ is
// live. This is deliberately separate from early scene preparation, where the
// level alias is not assigned yet.
bool LoadMetalR4EngineLevelDetailsSnapshot(
    R4DetailGrassSnapshot& snapshot,
    std::string& error);

// Expands a small, bounded cache neighbourhood using the same palette,
// deterministic jitter, static-CDB ray grounding and slope gate as
// CDetailManager::cache_Decompress. It is deliberately data-only: no D3D
// detail manager resources are created. The bounded neighbourhood is a
// temporary admission limit, not a synthetic placement fallback.
bool BuildMetalR4EngineGroundedDetailGrassFrame(
    const R4DetailGrassSnapshot& snapshot,
    const std::array<float, 3>& eye,
    std::uint64_t generation,
    R4DetailGrassFrame& frame,
    std::string& error);

// Copies the live GrassBenders and SSFX wind inputs after the engine frame has
// updated them. The standalone bootstrap returns a fail-closed error.
bool CaptureMetalR4EngineDetailGrassMotion(
    R4DetailGrassMotionSnapshot& snapshot,
    std::string& error);

// Resolves an animated UI sequence identity to the current active frame name
// using Device.dwTimeContinual. If the texture is not a sequence, returns
// the texture name unchanged.
bool ResolveMetalR4EngineUiTextureFrame(
    const std::string& textureName,
    std::string& resolvedTextureName,
    std::string& error);

// Resolves a texture identity with the same level -> saves -> game-textures
// search order used by dx10Texture.cpp, returning an explicit xrfs: path for
// the archive-winning provider. The caller can pass a DDS name with or without extension.
bool ResolveMetalR4EngineTextureDdsPath(
    const std::filesystem::path& textureName,
    std::filesystem::path& resolvedPath,
    std::string& error);

// Resolves and parses the archive-winning DDS without creating a legacy D3D
// resource. UI geometry needs these dimensions before it can normalize the
// pixel-space rectangles authored in XML texture atlases. Handles both single
// textures and animated sequences.
bool LoadMetalR4EngineTextureDimensions(
    const std::filesystem::path& textureName,
    std::uint32_t& width,
    std::uint32_t& height,
    std::string& error);

// Resolves an explicit texture from the currently assigned $level$ archive.
// This is used after level admission for shader-declared level-local resources
// such as details\\blend's build_details_bump.
bool ResolveMetalR4EngineCurrentLevelTextureDdsPath(
    const std::filesystem::path& textureName,
    std::filesystem::path& resolvedPath,
    std::string& error);

// Resolves a level-local DDS before $level$ has been assigned by the live
// level loader.  The Direct Metal scene is prepared during device creation,
// so it must address the configured level through $game_data$ explicitly.
bool ResolveMetalR4EngineLevelTextureDdsPath(
    const std::string& levelName,
    const std::filesystem::path& textureName,
    std::filesystem::path& resolvedPath,
    std::string& error);

// Loads the same THM detail scale used by cl_dt_scaler for a texture, while
// retaining the early explicit level lookup described above.
bool LoadMetalR4EngineTextureDetailScale(
    const std::string& levelName,
    const std::filesystem::path& textureName,
    float& detailScale,
    std::filesystem::path& resolvedPath,
    std::string& error);

enum class R4EngineTextureBumpKind
{
    Flat,
    Bump,
    Steep,
};

// Reads the same winning THM metadata that CTextureDescrMngr loads through
// STextureParams. The returned bump DDS and `#` companion follow
// dx10Texture's normal bump resolver (including its explicit dummy-map
// fallback); this is a value-only renderer API and does not create a legacy
// D3D texture resource.
bool ClassifyMetalR4EngineTextureBump(
    const std::filesystem::path& baseTextureDds,
    R4EngineTextureBumpKind& kind,
    std::filesystem::path& bumpTextureDds,
    std::filesystem::path& bumpXTextureDds,
    std::string& error);
