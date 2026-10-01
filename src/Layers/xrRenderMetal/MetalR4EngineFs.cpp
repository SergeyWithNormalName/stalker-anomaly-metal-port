#include "MetalR4EngineFs.h"
#include "MetalR4DetailGrassAdapter.h"
#include "MetalR4UiVideo.h"

#if defined(STATIC_RENDERER_R4)
#include "../xrRenderPC_R4/stdafx.h"
#include "../xrRender/ETextureParams.h"
#include "../xrRender/DetailManager.h"
#include "../xrRender/cl_intersect.h"
#include "../../xrEngine/gamemtllib.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace
{
bool resolveTextureDdsPath(
    const std::filesystem::path& textureName,
    std::filesystem::path& resolvedPath,
    std::string& error)
{
    std::string original = textureName.generic_string();
    std::replace(original.begin(), original.end(), '/', '\\');
    while (!original.empty() && original.front() == '\\')
        original.erase(original.begin());

    if (original.empty() || original.rfind("xrfs:", 0u) == 0u ||
        original.find("..") != std::string::npos ||
        _stricmp(textureName.extension().string().c_str(), ".dds") != 0)
    {
        error = "terrain texture identity must be a relative .dds filename";
        return false;
    }
    std::filesystem::path baseName = textureName;
    baseName.replace_extension();
    std::string stem = baseName.generic_string();
    std::replace(stem.begin(), stem.end(), '/', '\\');
    while (!stem.empty() && stem.front() == '\\')
        stem.erase(stem.begin());

    if (stem.empty())
    {
        error = "terrain texture identity has no filename stem";
        return false;
    }
    const auto makeGameTexturePath = [](const char* fallback) {
        std::string s = fallback;
        std::replace(s.begin(), s.end(), '\\', '/');
        return std::filesystem::path(std::string("xrfs:$game_textures$@") + s + ".dds");
    };
    // dx10Texture checks bump maps in $game_textures$ first and selects its
    // engine dummy when the requested map is absent. It deliberately does not
    // search level/save aliases for that special case.
    if (stem.find("_bump") != std::string::npos)
    {
        string_path found;
        if (FS.exist(found, "$game_textures$", stem.c_str(), ".dds"))
        {
            std::string posixOriginal = original;
            std::replace(posixOriginal.begin(), posixOriginal.end(), '\\', '/');
            resolvedPath = std::string("xrfs:$game_textures$@") + posixOriginal;
            return true;
        }
        const char* fallback = stem.find("_bump#") != std::string::npos ?
            "ed/ed_dummy_bump#" : "ed/ed_dummy_bump";
        if (FS.exist(found, "$game_textures$", fallback, ".dds"))
        {
            resolvedPath = makeGameTexturePath(fallback);
            return true;
        }
        error = "engine texture fallback is unavailable for " + original;
        return false;
    }
    if (stem.find("_height") != std::string::npos)
    {
        string_path found;
        if (FS.exist(found, "$game_textures$", stem.c_str(), ".dds"))
        {
            std::string posixOriginal = original;
            std::replace(posixOriginal.begin(), posixOriginal.end(), '\\', '/');
            resolvedPath = std::string("xrfs:$game_textures$@") + posixOriginal;
            return true;
        }
        // Neutral synthetic zero height texture (POM samples flat height 0.0)
        resolvedPath = std::string("xrfs:builtin_zero@") + textureName.filename().string();
        return true;
    }
    static constexpr const char* SearchAliases[] = {
        "$level$",
        "$game_saves$",
        "$game_textures$",
    };
    string_path found;
    for (const char* alias : SearchAliases)
    {
        if (FS.exist(found, alias, stem.c_str(), ".dds"))
        {
            std::string posixOriginal = original;
            std::replace(posixOriginal.begin(), posixOriginal.end(), '\\', '/');
            resolvedPath = std::string("xrfs:") + alias + "@" + posixOriginal;
            return true;
        }
    }
    // This is the non-editor D3D fallback after its three normal aliases.
    // Keep it explicit in the resolved URI so the terrain receipt reveals
    // degraded source content rather than silently sampling an unbound SRV.
    if (FS.exist(found, "$game_textures$", "ed\\ed_not_existing_texture", ".dds"))
    {
        resolvedPath = makeGameTexturePath("ed\\ed_not_existing_texture");
        return true;
    }
    error = "engine texture search and ed_not_existing_texture fallback are unavailable for " + original;
    return false;
}

bool resolveLevelTextureDdsPath(
    const std::string& levelName,
    const std::filesystem::path& textureName,
    std::filesystem::path& resolvedPath,
    std::string& error)
{
    const std::string original = textureName.generic_string();
    if (levelName.empty() || levelName.find("..") != std::string::npos ||
        levelName.find('/') != std::string::npos || levelName.find('\\') != std::string::npos ||
        original.empty() || original.rfind("xrfs:", 0u) == 0u ||
        original.find("..") != std::string::npos || textureName.extension() != ".dds")
    {
        error = "level texture identity must contain a safe level name and relative .dds filename";
        return false;
    }
    std::filesystem::path relative = std::filesystem::path("levels") / levelName / textureName;
    std::filesystem::path baseName = relative;
    baseName.replace_extension();
    std::string stem = baseName.generic_string();
    std::replace(stem.begin(), stem.end(), '/', '\\');
    string_path found;
    if (FS.exist(found, "$game_data$", stem.c_str(), ".dds"))
    {
        resolvedPath = std::string("xrfs:$game_data$@") + relative.generic_string();
        error.clear();
        return true;
    }
    if (FS.exist(found, "$game_textures$", stem.c_str(), ".dds"))
    {
        resolvedPath = std::string("xrfs:$game_textures$@") + relative.generic_string();
        error.clear();
        return true;
    }
    if (FS.exist(found, "$level$", textureName.stem().string().c_str(), ".dds"))
    {
        resolvedPath = std::string("xrfs:$level$@") + textureName.filename().string();
        error.clear();
        return true;
    }
    // Fallback: If level-specific lightmap is missing (common in modded/dynamic lighting levels
    // such as Agroprom Underground in MOD 442), fallback to neutral synthetic hemi lightmap
    // keeping the requested texture filename so the material validator accepts the slot match.
    const std::string filename = textureName.filename().string();
    const bool isLightmap = filename.rfind("lmap#", 0u) == 0u ||
                            filename.find("lmap") != std::string::npos ||
                            filename.find("hemi") != std::string::npos ||
                            (filename.size() >= 6u && filename.compare(filename.size() - 6u, 2u, "lm") == 0);
    if (isLightmap)
    {
        resolvedPath = std::string("xrfs:builtin@") + filename;
        error.clear();
        return true;
    }
    error = "level texture " + original + " is not present in level " + levelName;
    return false;
}

bool resolveCurrentLevelTextureDdsPath(
    const std::filesystem::path& textureName,
    std::filesystem::path& resolvedPath,
    std::string& error)
{
    const std::string original = textureName.generic_string();
    if (original.empty() || original.rfind("xrfs:", 0u) == 0u ||
        original.find("..") != std::string::npos || textureName.is_absolute() ||
        textureName.extension() != ".dds")
    {
        error = "current-level texture identity must be a safe relative .dds filename";
        return false;
    }
    std::filesystem::path baseName = textureName;
    baseName.replace_extension();
    std::string stem = baseName.generic_string();
    std::replace(stem.begin(), stem.end(), '/', '\\');
    string_path found;
    if (!FS.exist(found, "$level$", stem.c_str(), ".dds"))
    {
        error = "current level archive does not contain " + original;
        return false;
    }
    resolvedPath = std::string("xrfs:$level$@") + original;
    error.clear();
    return true;
}

bool loadTextureDetailScale(
    const std::string& levelName,
    const std::filesystem::path& textureName,
    float& detailScale,
    std::filesystem::path& resolvedPath,
    std::string& error)
{
    detailScale = 1.0f;
    resolvedPath.clear();
    std::filesystem::path textureStem = textureName;
    textureStem.replace_extension();
    if (levelName.empty() || levelName.find("..") != std::string::npos ||
        levelName.find('/') != std::string::npos || levelName.find('\\') != std::string::npos ||
        textureStem.empty() || textureStem.is_absolute() ||
        textureStem.generic_string().find("..") != std::string::npos)
    {
        error = "texture THM lookup requires a safe level and relative texture name";
        return false;
    }

    std::filesystem::path levelStem = std::filesystem::path("levels") / levelName / textureStem;
    std::string levelLookup = levelStem.generic_string();
    std::string gameLookup = textureStem.generic_string();
    std::replace(levelLookup.begin(), levelLookup.end(), '/', '\\');
    std::replace(gameLookup.begin(), gameLookup.end(), '/', '\\');
    string_path found;
    const char* alias = nullptr;
    if (FS.exist(found, "$game_data$", levelLookup.c_str(), ".thm"))
        alias = "$game_data$";
    else if (FS.exist(found, "$game_textures$", gameLookup.c_str(), ".thm"))
        alias = "$game_textures$";
    if (!alias)
    {
        error = "engine texture THM search did not find " + textureStem.generic_string();
        return false;
    }

    IReader* reader = FS.r_open(found);
    if (!reader)
    {
        error = "engine filesystem cannot open texture THM";
        return false;
    }
    const bool hasTypeChunk = reader->find_chunk(THM_CHUNK_TYPE) != 0;
    if (!hasTypeChunk)
    {
        FS.r_close(reader);
        error = "texture THM has no type chunk";
        return false;
    }
    reader->r_u32();
    STextureParams parameters;
    parameters.Load(*reader);
    FS.r_close(reader);
    if (!(parameters.detail_scale > 0.0f) || !std::isfinite(parameters.detail_scale))
    {
        error = "texture THM detail scale is not finite and positive";
        return false;
    }
    detailScale = parameters.detail_scale;
    resolvedPath = std::string("xrfs:") + alias + "@" +
        (alias == std::string("$game_data$") ? levelStem : textureStem).generic_string() + ".thm";
    error.clear();
    return true;
}

bool classifyTextureBump(
    const std::filesystem::path& baseTextureDds,
    R4EngineTextureBumpKind& kind,
    std::filesystem::path& bumpTextureDds,
    std::filesystem::path& bumpXTextureDds,
    std::string& error)
{
    kind = R4EngineTextureBumpKind::Flat;
    bumpTextureDds.clear();
    bumpXTextureDds.clear();
    std::filesystem::path stem = baseTextureDds;
    stem.replace_extension();
    if (stem.empty() || stem.is_absolute() || stem.generic_string().find("..") != std::string::npos ||
        baseTextureDds.extension() != ".dds")
    {
        error = "THM bump classification requires a safe relative DDS identity";
        return false;
    }
    std::string lookup = stem.generic_string();
    std::replace(lookup.begin(), lookup.end(), '/', '\\');
    string_path found;
    const char* alias = nullptr;
    // Match the texture resolver's level-before-game-textures precedence.
    if (FS.exist(found, "$level$", lookup.c_str(), ".thm"))
        alias = "$level$";
    else if (FS.exist(found, "$game_textures$", lookup.c_str(), ".thm"))
        alias = "$game_textures$";
    if (!alias)
    {
        error.clear();
        return true;
    }
    IReader* reader = FS.r_open(found);
    if (!reader || !reader->find_chunk(THM_CHUNK_TYPE))
    {
        if (reader) FS.r_close(reader);
        error = "winning texture THM has no readable type chunk";
        return false;
    }
    reader->r_u32();
    STextureParams parameters;
    parameters.Load(*reader);
    FS.r_close(reader);
    if (parameters.type != STextureParams::ttImage && parameters.type != STextureParams::ttTerrain &&
        parameters.type != STextureParams::ttNormalMap)
    {
        error.clear();
        return true;
    }
    if (parameters.bump_mode != STextureParams::tbmUse &&
        parameters.bump_mode != STextureParams::tbmUseParallax)
    {
        error.clear();
        return true;
    }
    if (!parameters.bump_name.size())
    {
        error = "THM declares a bump mode without a bump texture identity";
        return false;
    }
    std::filesystem::path bump(parameters.bump_name.c_str());
    if (bump.is_absolute() || bump.generic_string().find("..") != std::string::npos)
    {
        error = "THM bump texture identity is unsafe";
        return false;
    }
    if (bump.extension().empty())
        bump += ".dds";
    if (bump.extension() != ".dds" || !resolveTextureDdsPath(bump, bumpTextureDds, error))
        return false;
    kind = parameters.bump_mode == STextureParams::tbmUseParallax ?
        R4EngineTextureBumpKind::Steep : R4EngineTextureBumpKind::Bump;
    if (kind != R4EngineTextureBumpKind::Steep)
        return true;
    std::filesystem::path bumpX = bump;
    bumpX.replace_extension();
    bumpX += "#.dds";
    if (!resolveTextureDdsPath(bumpX, bumpXTextureDds, error))
        return false;
    return true;
}

bool readXrfsDdsBytes(
    const std::filesystem::path& path,
    const std::size_t maximumBytes,
    std::vector<std::uint8_t>& bytes,
    std::string& error)
{
    constexpr const char* Prefix = "xrfs:";
    const std::string encoded = path.generic_string();
    if (encoded.rfind(Prefix, 0u) != 0u)
    {
        error = "engine DDS provider received a non-xrfs path";
        return false;
    }
    const std::string source = encoded.substr(std::strlen(Prefix));
    if (source.empty())
    {
        error = "xrfs DDS path has no virtual filename";
        return false;
    }
    std::string alias = "$game_textures$";
    std::string relativePath = source;
    const std::size_t separator = source.find('@');
    if (separator != std::string::npos)
    {
        alias = source.substr(0, separator);
        relativePath = source.substr(separator + 1u);
    }
    if (alias.empty() || relativePath.empty() || relativePath.find('@') != std::string::npos)
    {
        error = "xrfs DDS path must be xrfs:relative.dds or xrfs:$alias$@relative.dds";
        return false;
    }
    std::replace(relativePath.begin(), relativePath.end(), '/', '\\');
    if (alias == "builtin" || alias == "$builtin$" ||
        alias == "builtin_zero" || alias == "$builtin_zero$")
    {
        // 4x4 RGBA8 uncompressed DDS (128 bytes header + 64 bytes pixel payload = 192 bytes)
        // With RGBA = (255, 255, 255, 255), ambient hemi factor (A) is 1.0f and light (RGB) is 1.0f.
        // With builtin_zero, RGBA = (0, 0, 0, 0) for flat zero height/displacement.
        bytes.assign(192u, 0u);
        const auto writeU32 = [&bytes](std::size_t offset, std::uint32_t val) {
            bytes[offset] = static_cast<std::uint8_t>(val & 0xffu);
            bytes[offset + 1] = static_cast<std::uint8_t>((val >> 8) & 0xffu);
            bytes[offset + 2] = static_cast<std::uint8_t>((val >> 16) & 0xffu);
            bytes[offset + 3] = static_cast<std::uint8_t>((val >> 24) & 0xffu);
        };
        writeU32(0u, 0x20534444u); // 'DDS '
        writeU32(4u, 124u);        // dwSize
        writeU32(8u, 0x1007u);     // dwFlags: DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT
        writeU32(12u, 4u);         // dwHeight = 4
        writeU32(16u, 4u);         // dwWidth = 4
        writeU32(20u, 16u);        // dwPitchOrLinearSize = 4 * 4
        writeU32(28u, 1u);         // dwMipMapCount = 1
        writeU32(76u, 32u);        // ddspf.dwSize
        writeU32(80u, 0x41u);      // ddspf.dwFlags: DDPF_RGB | DDPF_ALPHAPIXELS
        writeU32(88u, 32u);        // ddspf.dwRGBBitCount = 32
        writeU32(92u, 0x000000ffu); // R mask
        writeU32(96u, 0x0000ff00u); // G mask
        writeU32(100u, 0x00ff0000u);// B mask
        writeU32(104u, 0xff000000u);// A mask
        writeU32(108u, 0x1000u);    // dwCaps: DDSCAPS_TEXTURE
        const bool isZero = alias == "builtin_zero" || alias == "$builtin_zero$";
        if (!isZero)
        {
            for (std::size_t p = 128u; p < 192u; ++p)
                bytes[p] = 0xffu;
        }
        Msg("* METAL_FS_READ_OK alias=\"%s\" path=\"%s\" length=192", alias.c_str(), relativePath.c_str());
        return true;
    }
    IReader* reader = FS.r_open(alias.c_str(), relativePath.c_str());
    if (!reader)
    {
        Msg("! METAL_FS_READ_FAIL alias=\"%s\" path=\"%s\"", alias.c_str(), relativePath.c_str());
        error = "engine filesystem cannot open xrfs DDS: " + encoded;
        return false;
    }
    Msg("* METAL_FS_READ_OK alias=\"%s\" path=\"%s\" length=%d", alias.c_str(), relativePath.c_str(), reader->length());
    const int length = reader->length();
    if (length <= 0 || static_cast<std::size_t>(length) > maximumBytes || !reader->pointer())
    {
        FS.r_close(reader);
        error = length <= 0 ? "engine filesystem returned an empty xrfs DDS" :
            "engine filesystem xrfs DDS exceeds the provider byte limit";
        return false;
    }
    const std::uint8_t* data = static_cast<const std::uint8_t*>(reader->pointer());
    bytes.assign(data, data + static_cast<std::size_t>(length));
    FS.r_close(reader);
    return true;
}

bool readLevelDetailsBytes(const std::filesystem::path& requestedPath,
    const std::size_t maximumBytes, std::vector<std::uint8_t>& bytes,
    std::string& error)
{
    bytes.clear();
    if (requestedPath.generic_string() != "level.details")
    {
        error = "level.details provider only accepts the canonical level.details identity";
        return false;
    }
    IReader* reader = FS.r_open("$level$", "level.details");
    if (!reader)
    {
        error = "engine filesystem cannot open $level$/level.details";
        return false;
    }
    const int length = reader->length();
    if (length <= 0 || static_cast<std::size_t>(length) > maximumBytes || !reader->pointer())
    {
        FS.r_close(reader);
        error = length <= 0 ? "engine filesystem returned an empty level.details" :
            "engine filesystem level.details exceeds the provider byte limit";
        return false;
    }
    const std::uint8_t* data = static_cast<const std::uint8_t*>(reader->pointer());
    bytes.assign(data, data + static_cast<std::size_t>(length));
    FS.r_close(reader);
    error.clear();
    return true;
}
} // namespace

bool CreateMetalR4EngineXrfsProvider(
    xray::metal::R4DdsBytesProvider& provider,
    std::string& error)
{
    provider = readXrfsDdsBytes;
    error.clear();
    return true;
}

bool CreateMetalR4EngineLevelDetailsProvider(
    R4BinaryBytesProvider& provider,
    std::string& error)
{
    provider = readLevelDetailsBytes;
    error.clear();
    return true;
}

bool LoadMetalR4EngineLevelDetailsSnapshot(
    R4DetailGrassSnapshot& snapshot,
    std::string& error)
{
    std::vector<std::uint8_t> bytes;
    if (!readLevelDetailsBytes("level.details", 64u * 1024u * 1024u, bytes, error))
        return false;
    return LoadR4DetailGrassSnapshot(bytes, snapshot, error);
}

bool CaptureMetalR4EngineDetailGrassMotion(
    R4DetailGrassMotionSnapshot& snapshot,
    std::string& error)
{
    snapshot = {};
    if (!g_pGamePersistent || !g_pGamePersistent->Environment().CurrentEnv)
    {
        error = "detail grass motion needs a live game-persistent environment";
        return false;
    }
    const CEnvDescriptor& environment = *g_pGamePersistent->Environment().CurrentEnv;
    const IGame_Persistent::grass_data& data = g_pGamePersistent->grass_shader_data;
    const auto copy4 = [](const Fvector4& value) {
        return std::array<float, 4>{value.x, value.y, value.z, value.w};
    };
    const auto finite4 = [](const std::array<float, 4>& value) {
        return std::isfinite(value[0]) && std::isfinite(value[1]) &&
            std::isfinite(value[2]) && std::isfinite(value[3]);
    };
    snapshot.engineFrame = Device.dwFrame;
    snapshot.bendersSetup = copy4(ps_ssfx_int_grass_params_1);
    snapshot.windSetupGrass = copy4(ps_ssfx_wind_grass);
    snapshot.windSetupTrees = copy4(ps_ssfx_wind_trees);
    snapshot.windParams = {environment.wind_direction, environment.wind_velocity, 0.0f, 0.0f};
    snapshot.windAnimation = copy4(g_pGamePersistent->Environment().wind_anim);
    snapshot.windAnimationPrevious = copy4(Device.wind_anim_prev);
    snapshot.grassAlign = ps_ssfx_terrain_grass_align;
    // dx10DetailManager_VS writes a player bender only when the same runtime
    // toggle is enabled, and always writes the sentinel direction at slot 16.
    // Match dx10DetailManager_VS.cpp exactly: all current/previous bender
    // uploads are inside this gate, not merely conditioned on a derived
    // quantity. When disabled, the Metal adapter receives no invented state.
    if (ps_ssfx_grass_interactive.y > 0.0f)
    {
        const int requested = static_cast<int>(ps_ssfx_grass_interactive.y);
        const std::uint32_t quantity = static_cast<std::uint32_t>(std::clamp(requested + 1, 0, 16));
        if (ps_ssfx_grass_interactive.x > 0.0f)
            snapshot.currentBenders[0] = {Device.vCameraPosition.x, Device.vCameraPosition.y,
                Device.vCameraPosition.z, -1.0f};
        snapshot.currentBenders[16] = {0.0f, -99.0f, 0.0f, 1.0f};
        for (std::uint32_t index = 0u; index < quantity; ++index)
        {
            snapshot.previousBenders[index] = copy4(data.prev_pos[index]);
            snapshot.previousBenders[index + 16u] = copy4(data.prev_dir[index]);
            if (index == 0u)
                continue;
            snapshot.currentBenders[index] = {data.pos[index].x, data.pos[index].y,
                data.pos[index].z, data.radius_curr[index]};
            snapshot.currentBenders[index + 16u] = {data.dir[index].x, data.dir[index].y,
                data.dir[index].z, data.str[index]};
            if (data.id[index] != NULL)
                ++snapshot.activeBenders;
        }
    }
    for (const std::array<float, 4>& value : snapshot.previousBenders)
        if (!finite4(value))
        {
            error = "detail grass previous bender state contains a non-finite value";
            return false;
        }
    for (const std::array<float, 4>& value : snapshot.currentBenders)
        if (!finite4(value))
        {
            error = "detail grass current bender state contains a non-finite value";
            return false;
        }
    if (!finite4(snapshot.bendersSetup) || !finite4(snapshot.windParams) ||
        !finite4(snapshot.windSetupGrass) || !finite4(snapshot.windSetupTrees) ||
        !finite4(snapshot.windAnimation) || !finite4(snapshot.windAnimationPrevious))
    {
        error = "detail grass SSFX wind state contains a non-finite value";
        return false;
    }
    snapshot.valid = true;
    error.clear();
    return true;
}

bool BuildMetalR4EngineGroundedDetailGrassFrame(
    const R4DetailGrassSnapshot& snapshot,
    const std::array<float, 3>& eye,
    const std::uint64_t generation,
    R4DetailGrassFrame& frame,
    std::string& error)
{
    frame = {};
    frame.generation = generation;
    if (!g_pGameLevel || !g_pGameLevel->bReady ||
        !g_pGameLevel->ObjectSpace.GetStaticModel() || snapshot.sizeX == 0u ||
        snapshot.sizeZ == 0u || snapshot.slots.size() !=
            static_cast<std::size_t>(snapshot.sizeX) * snapshot.sizeZ)
    {
        error = "detail grass needs a live static CDB and validated level.details grid";
        return false;
    }
    if (!std::isfinite(eye[0]) || !std::isfinite(eye[1]) || !std::isfinite(eye[2]))
    {
        error = "detail grass camera position is non-finite";
        return false;
    }
    int dither[16][16]{};
    static constexpr int magic[4][4] = {{0, 14, 3, 13}, {11, 5, 8, 6},
        {12, 2, 15, 1}, {7, 9, 4, 10}};
    const float magicFactor = (255.0f - 1.0f) / 16.0f;
    for (int i = 0; i != 4; ++i)
        for (int j = 0; j != 4; ++j)
            for (int k = 0; k != 4; ++k)
                for (int l = 0; l != 4; ++l)
                    dither[4 * k + i][4 * l + j] = static_cast<int>(0.5f +
                        magic[i][j] * magicFactor + (magic[k][l] / 16.0f) * magicFactor);
    const auto interpolate = [](const std::array<std::uint8_t, 4>& values,
        const std::uint32_t x, const std::uint32_t y, const std::uint32_t size) {
        const float fx = static_cast<float>(x) / static_cast<float>(size);
        const float fy = static_cast<float>(y) / static_cast<float>(size);
        const float ifx = 1.0f - fx;
        const float ify = 1.0f - fy;
        const float c01 = (values[0] * ifx + values[1] * fx) * (255.0f / 15.0f);
        const float c23 = (values[2] * ifx + values[3] * fx) * (255.0f / 15.0f);
        const float c02 = (values[0] * ify + values[2] * fy) * (255.0f / 15.0f);
        const float c13 = (values[1] * ify + values[3] * fy) * (255.0f / 15.0f);
        return (ify * c01 + fy * c23 + ifx * c02 + fx * c13) * 0.5f;
    };
    const auto hash2 = [](const std::uint32_t x, const std::uint32_t y) {
        const std::uint64_t value = (static_cast<std::uint64_t>(x) << 32u) | y;
        return static_cast<std::uint32_t>((9199940308585234877ull * value +
            9199940308585234877ull) >> 32u);
    };
    // Key: (modelId, chunkZ, chunkX)
    // Grouping instances into spatial chunks (4x4 slots = 8x8m) provides tight bounding boxes
    // and eliminates cross-batch reshuffling when the camera crosses slot boundaries.
    std::map<std::tuple<std::uint16_t, int, int>, std::vector<R4DetailGrassInstance>> chunkInstances;
    const int eyeX = iFloor(eye[0] / dm_slot_size + .5f);
    const int eyeZ = iFloor(eye[2] / dm_slot_size + .5f);
    // Keep the exact active CDetailManager cache footprint: cg2w_X/Z maps
    // [0, dm_current_cache_line) to [camera-dm_current_size,
    // camera+dm_current_size]. Console detail-radius changes update all of
    // these variables atomically before the legacy manager is reloaded.
    if (dm_current_cache_line != dm_current_size * 2u + 1u || dm_current_fade <= 0.0f ||
        !std::isfinite(dm_current_fade))
    {
        error = "active detail cache dimensions or fade distance are invalid";
        return false;
    }
    const int Radius = static_cast<int>(dm_current_size);
    frame.cacheSlots = static_cast<std::size_t>(dm_current_cache_line) * dm_current_cache_line;
    frame.cacheFadeDistance = dm_current_fade;
    xrXRC xrc;
    CDB::TRI* triangles = g_pGameLevel->ObjectSpace.GetStaticTris();
    Fvector* vertices = g_pGameLevel->ObjectSpace.GetStaticVerts();
    for (int sz = eyeZ - Radius; sz <= eyeZ + Radius; ++sz)
    {
        for (int sx = eyeX - Radius; sx <= eyeX + Radius; ++sx)
        {
            const int localX = sx + snapshot.offsetX;
            const int localZ = sz + snapshot.offsetZ;
            if (localX < 0 || localZ < 0 || localX >= static_cast<int>(snapshot.sizeX) ||
                localZ >= static_cast<int>(snapshot.sizeZ))
                continue;
            const R4DetailGrassSlot& slot = snapshot.slots[
                static_cast<std::size_t>(localZ) * snapshot.sizeX + static_cast<std::size_t>(localX)];
            Fvector center; center.set((sx + 0.5f) * dm_slot_size,
                slot.yBase + slot.yHeight * 0.5f, (sz + 0.5f) * dm_slot_size);
            Fvector half; half.set(dm_slot_size * 0.5f + EPS_L,
                slot.yHeight * 0.5f + EPS_L, dm_slot_size * 0.5f + EPS_L);
            // UpdateVisibleM ultimately tests the decompressed slot sphere
            // against dm_fade.  We only reject when this conservative raw
            // slot sphere is wholly outside the same active distance, so an
            // uncertain slot survives rather than disappearing at the edge.
            const float sphereRadius = std::sqrt(half.x * half.x + half.y * half.y + half.z * half.z);
            const float dx = center.x - eye[0];
            const float dy = center.y - eye[1];
            const float dz = center.z - eye[2];
            const float centerDistance = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (centerDistance - sphereRadius > dm_current_fade)
            {
                ++frame.distanceRejectedSlots;
                continue;
            }
            xrc.box_options(CDB::OPT_FULL_TEST);
            xrc.box_query(g_pGameLevel->ObjectSpace.GetStaticModel(), center, half);
            if (xrc.r_count() == 0u)
                continue;
            const float density = std::max(0.05f, ps_current_detail_density);
            const std::uint32_t size = static_cast<std::uint32_t>(iCeil(dm_slot_size / density));
            const float jitter = density / 1.7f;
            const std::uint32_t seed = hash2(static_cast<std::uint32_t>(sx), static_cast<std::uint32_t>(sz));
            CRandom selection(0x12071980u ^ seed), randomJitter(0x12071980u ^ seed),
                yaw(0x12071980u ^ seed), scale(0x12071980u ^ seed);
            struct PendingSlotInstance
            {
                std::uint16_t modelId = 0;
                int chunkX = 0;
                int chunkZ = 0;
                R4DetailGrassInstance instance;
            };
            std::vector<PendingSlotInstance> pendingSlotInstances;
            Fbox slotBounds;
            slotBounds.invalidate();
            bool hasSlotBounds = false;
            for (std::uint32_t z = 0; z <= size; ++z)
            {
                for (std::uint32_t x = 0; x <= size; ++x)
                {
                    ++frame.requestedInstances;
                    const std::uint32_t shiftX = randomJitter.randI(16u);
                    const std::uint32_t shiftZ = randomJitter.randI(16u);
                    std::uint16_t selected[4]{};
                    std::size_t selectedCount = 0u;
                    for (std::size_t palette = 0; palette != 4u; ++palette)
                    {
                        const std::uint16_t modelId = slot.modelIds[palette];
                        if (modelId == 0x3fffu || modelId >= snapshot.models.size())
                            continue;
                        const float alpha = interpolate(slot.palette[palette],
                            std::min(x, size - 1u), std::min(z, size - 1u), size);
                        const int threshold = static_cast<int>(std::floor(alpha + .5f));
                        if (threshold > dither[(x + shiftX) % 16u][(z + shiftZ) % 16u])
                            selected[selectedCount++] = modelId;
                    }
                    if (selectedCount == 0u)
                        continue;
                    const std::uint16_t modelId = selectedCount == 1u ? selected[0] :
                        selected[selection.randI(static_cast<u32>(selectedCount))];
                    const R4DetailGrassModel& model = snapshot.models[modelId];
                    Fvector position; position.set((static_cast<float>(x) / size) * dm_slot_size +
                        sx * dm_slot_size - EPS_L + randomJitter.randFs(jitter),
                        slot.yBase + slot.yHeight + EPS_L,
                        (static_cast<float>(z) / size) * dm_slot_size +
                        sz * dm_slot_size - EPS_L + randomJitter.randFs(jitter));
                    Fvector direction; direction.set(0.0f, -1.0f, 0.0f);
                    float groundY = slot.yBase - 5.0f;
                    Fvector normal; normal.set(0.0f, 0.0f, 0.0f);
                    bool noPush = false;
                    for (u32 triangleIndex = 0u; triangleIndex < xrc.r_count(); ++triangleIndex)
                    {
                        CDB::TRI& triangle = triangles[xrc.r_begin()[triangleIndex].id];
                        SGameMtl* material = GMLib.GetMaterialByIdx(triangle.material);
                        if (material && material->Flags.test(SGameMtl::flPassable))
                            continue;
                        if (RImplementation.pOutdoorSector)
                        {
                            CSector* sector = static_cast<CSector*>(RImplementation.getSector(triangle.sector));
                            if (sector != RImplementation.pOutdoorSector)
                            {
                                noPush = true;
                                break;
                            }
                        }
                        Fvector triangleVertices[3] = {vertices[triangle.verts[0]], vertices[triangle.verts[1]],
                            vertices[triangle.verts[2]]};
                        float u = 0.0f, v = 0.0f, range = 0.0f;
                        if (CDB::TestRayTri(position, direction, triangleVertices, u, v, range, TRUE) && range >= 0.0f)
                        {
                            const float candidateY = position.y - range;
                            if (candidateY > groundY) groundY = candidateY;
                            normal.mknormal(triangleVertices[0], triangleVertices[1], triangleVertices[2]);
                            // cache_Decompress consumes the first static-CDB
                            // hit in query order; preserving that ordering is
                            // more important than opportunistically finding a
                            // different surface behind it.
                            break;
                        }
                    }
                    const float slope = scale.randF(ps_ssfx_terrain_grass_slope * .8f,
                        ps_ssfx_terrain_grass_slope);
                    if (noPush || groundY < slot.yBase || normal.dotproduct(direction) > -(1.0f - slope))
                    {
                        ++frame.rejectedInstances;
                        continue;
                    }
                    position.y = groundY;
                    const float minimumScale = model.minimumScale * .5f;
                    const float maximumScale = model.maximumScale * .9f;
                    if (!std::isfinite(minimumScale) || !std::isfinite(maximumScale) ||
                        minimumScale <= 0.0f || maximumScale < minimumScale)
                    {
                        error = "detail grass model produces an invalid legacy runtime scale interval";
                        return false;
                    }
                    const float itemScale = scale.randF(minimumScale, maximumScale) *
                        ps_current_detail_height;
                    Fmatrix rotation, scaleMatrix, transform;
                    rotation.rotateY(yaw.randF(0.0f, PI_MUL_2));
                    if (ps_ssfx_terrain_grass_align > 0)
                    {
                        const Fmatrix preserved = rotation;
                        rotation.j.set(normal);
                        Fvector::generate_orthonormal_basis(rotation.j, rotation.i, rotation.k);
                        rotation.mulB_43(preserved);
                    }
                    rotation.translate_over(position);
                    scaleMatrix.scale(itemScale, itemScale, itemScale);
                    transform.mul_43(rotation, scaleMatrix);
                    R4DetailGrassInstance instance;
                    std::memcpy(instance.transform.data(), &transform, sizeof(transform));
                    instance.terrainNormal = {normal.x, normal.y, normal.z};
                    instance.sun = slot.sun;
                    instance.hemi = slot.hemi;
                    // cache_Decompress merges each accepted transformed detail
                    // model AABB into D.vis.box, then getsphere stores this
                    // center in Item.position. Reconstruct that same source
                    // center from the accepted grounded instances before
                    // splitting them into Metal batches.
                    Fbox modelBounds;
                    modelBounds.min.set(model.boundsMinimum[0], model.boundsMinimum[1], model.boundsMinimum[2]);
                    modelBounds.max.set(model.boundsMaximum[0], model.boundsMaximum[1], model.boundsMaximum[2]);
                    Fbox transformedModelBounds;
                    transformedModelBounds.xform(modelBounds, transform);
                    if (!hasSlotBounds)
                    {
                        slotBounds = transformedModelBounds;
                        hasSlotBounds = true;
                    }
                    else
                        slotBounds.merge(transformedModelBounds);
                    if ((model.flags & DO_NO_WAVING) != 0u)
                        ++frame.stillInstances;
                    const int chunkX = sx >= 0 ? (sx / 4) : ((sx - 3) / 4);
                    const int chunkZ = sz >= 0 ? (sz / 4) : ((sz - 3) / 4);
                    pendingSlotInstances.push_back({modelId, chunkX, chunkZ, instance});
                    ++frame.groundedInstances;
                }
            }
            if (hasSlotBounds)
            {
                Fvector shadowFadePosition;
                slotBounds.getcenter(shadowFadePosition);
                const float fadeDx = shadowFadePosition.x - eye[0];
                const float fadeDy = shadowFadePosition.y - eye[1];
                const float fadeDz = shadowFadePosition.z - eye[2];
                const float shadowDistanceSquared = fadeDx * fadeDx + fadeDy * fadeDy + fadeDz * fadeDz;
                for (PendingSlotInstance& pending : pendingSlotInstances)
                {
                    pending.instance.shadowFadePosition = {shadowFadePosition.x,
                        shadowFadePosition.y, shadowFadePosition.z};
                    pending.instance.shadowDistanceSquared = shadowDistanceSquared;
                    pending.instance.hasShadowFadePosition = true;
                    chunkInstances[std::make_tuple(pending.modelId, pending.chunkZ, pending.chunkX)]
                        .push_back(pending.instance);
                }
            }
        }
    }
    for (auto& entry : chunkInstances)
    {
        std::vector<R4DetailGrassInstance>& source = entry.second;
        const std::uint16_t modelId = std::get<0>(entry.first);
        for (std::size_t first = 0; first < source.size(); first += kR4DetailGrassBatchSize)
        {
            const std::size_t count = std::min(kR4DetailGrassBatchSize, source.size() - first);
            R4DetailGrassBatch batch;
            batch.modelId = modelId;
            batch.instances.assign(source.begin() + first, source.begin() + first + count);
            const R4DetailGrassModel& model = snapshot.models[modelId];
            batch.boundsMinimum = {std::numeric_limits<float>::infinity(),
                std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()};
            batch.boundsMaximum = {-std::numeric_limits<float>::infinity(),
                -std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()};
            for (const R4DetailGrassInstance& instance : batch.instances)
                for (std::uint32_t mask = 0u; mask != 8u; ++mask)
                {
                    const float x = (mask & 1u) ? model.boundsMaximum[0] : model.boundsMinimum[0];
                    const float y = (mask & 2u) ? model.boundsMaximum[1] : model.boundsMinimum[1];
                    const float z = (mask & 4u) ? model.boundsMaximum[2] : model.boundsMinimum[2];
                    const float worldX = x * instance.transform[0] + y * instance.transform[4] +
                        z * instance.transform[8] + instance.transform[12];
                    const float worldY = x * instance.transform[1] + y * instance.transform[5] +
                        z * instance.transform[9] + instance.transform[13];
                    const float worldZ = x * instance.transform[2] + y * instance.transform[6] +
                        z * instance.transform[10] + instance.transform[14];
                    if (!std::isfinite(worldX) || !std::isfinite(worldY) || !std::isfinite(worldZ))
                        continue;
                    batch.boundsMinimum[0] = std::min(batch.boundsMinimum[0], worldX);
                    batch.boundsMinimum[1] = std::min(batch.boundsMinimum[1], worldY);
                    batch.boundsMinimum[2] = std::min(batch.boundsMinimum[2], worldZ);
                    batch.boundsMaximum[0] = std::max(batch.boundsMaximum[0], worldX);
                    batch.boundsMaximum[1] = std::max(batch.boundsMaximum[1], worldY);
                    batch.boundsMaximum[2] = std::max(batch.boundsMaximum[2], worldZ);
                }
            batch.hasBounds = std::isfinite(batch.boundsMinimum[0]) && std::isfinite(batch.boundsMinimum[1]) &&
                std::isfinite(batch.boundsMinimum[2]) && std::isfinite(batch.boundsMaximum[0]) &&
                std::isfinite(batch.boundsMaximum[1]) && std::isfinite(batch.boundsMaximum[2]);
            frame.batches.emplace_back(std::move(batch));
        }
    }
    error.clear();
    return true;
}

namespace
{
struct UiSequenceDef
{
    std::uint32_t mspf = 50;
    bool cycled = false;
    std::vector<std::string> frames;
};

static std::mutex s_uiSeqMutex;
static std::unordered_map<std::string, UiSequenceDef> s_uiSeqCache;
static std::unordered_set<std::string> s_uiNonSeqCache;

static std::string normalizeUiTextureStem(const std::string& name)
{
    std::string s = name;
    std::replace(s.begin(), s.end(), '/', '\\');
    while (!s.empty() && s.front() == '\\')
        s.erase(s.begin());
    const std::size_t ext = s.rfind('.');
    if (ext != std::string::npos)
    {
        std::string extStr = s.substr(ext);
        for (char& c : extStr)
            c = static_cast<char>(tolower(c));
        if (extStr == ".dds" || extStr == ".seq")
            s = s.substr(0, ext);
    }
    return s;
}

static bool resolveUiSequence(
    const std::string& textureName,
    UiSequenceDef& seq,
    std::string& error)
{
    const std::string stem = normalizeUiTextureStem(textureName);
    if (stem.empty())
    {
        error = "UI sequence name is empty";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(s_uiSeqMutex);
        if (s_uiNonSeqCache.find(stem) != s_uiNonSeqCache.end())
        {
            error = "not a sequence";
            return false;
        }
        const auto it = s_uiSeqCache.find(stem);
        if (it != s_uiSeqCache.end())
        {
            seq = it->second;
            return true;
        }
    }

    // Check for explicit .seq file in textures search aliases
    static constexpr const char* SeqAliases[] = {
        "$game_textures$",
        "$level$",
        "$game_saves$",
    };
    string_path foundSeq;
    bool foundSeqFile = false;
    for (const char* alias : SeqAliases)
    {
        if (FS.exist(foundSeq, alias, stem.c_str(), ".seq"))
        {
            foundSeqFile = true;
            break;
        }
    }

    if (foundSeqFile)
    {
        IReader* fsReader = FS.r_open(foundSeq);
        if (fsReader)
        {
            UiSequenceDef parsed;
            parsed.cycled = false;
            string256 buffer;
            fsReader->r_string(buffer, sizeof(buffer));
            _Trim(buffer);
            if (0 == _stricmp(buffer, "cycled"))
            {
                parsed.cycled = true;
                fsReader->r_string(buffer, sizeof(buffer));
                _Trim(buffer);
            }
            int fps = atoi(buffer);
            if (fps <= 0) fps = 20;
            parsed.mspf = 1000 / static_cast<std::uint32_t>(fps);
            if (parsed.mspf == 0) parsed.mspf = 1;

            while (!fsReader->eof())
            {
                fsReader->r_string(buffer, sizeof(buffer));
                _Trim(buffer);
                if (buffer[0])
                {
                    std::string frame = normalizeUiTextureStem(buffer);
                    if (!frame.empty())
                        parsed.frames.push_back(std::move(frame));
                }
            }
            FS.r_close(fsReader);

            if (!parsed.frames.empty())
            {
                std::lock_guard<std::mutex> lock(s_uiSeqMutex);
                s_uiSeqCache[stem] = parsed;
                seq = std::move(parsed);
                error.clear();
                return true;
            }
        }
    }

    // Fallback: Check for numbered sequence frames (_01.dds, _02.dds, ...)
    string_path testFn;
    if (FS.exist(testFn, "$game_textures$", (stem + "_01").c_str(), ".dds"))
    {
        UiSequenceDef autoSeq;
        autoSeq.cycled = false;
        autoSeq.mspf = 50; // 20 fps default
        for (int i = 1; i <= 99; ++i)
        {
            char suffix[16];
            xr_sprintf(suffix, "_%02d", i);
            std::string candidate = stem + suffix;
            if (FS.exist(testFn, "$game_textures$", candidate.c_str(), ".dds"))
            {
                autoSeq.frames.push_back(std::move(candidate));
            }
            else
            {
                break;
            }
        }
        if (autoSeq.frames.size() >= 2)
        {
            std::lock_guard<std::mutex> lock(s_uiSeqMutex);
            s_uiSeqCache[stem] = autoSeq;
            seq = std::move(autoSeq);
            error.clear();
            return true;
        }
    }

    // Not a sequence
    {
        std::lock_guard<std::mutex> lock(s_uiSeqMutex);
        s_uiNonSeqCache.insert(stem);
    }
    error = "not a sequence";
    return false;
}
} // namespace

bool ResolveMetalR4EngineUiTextureFrame(
    const std::string& textureName,
    std::string& resolvedTextureName,
    std::string& error)
{
    UiSequenceDef seq;
    if (resolveUiSequence(textureName, seq, error))
    {
        if (seq.frames.empty())
        {
            resolvedTextureName = textureName;
            return true;
        }
        const std::uint32_t totalFrames = static_cast<std::uint32_t>(seq.frames.size());
        const std::uint32_t mspf = seq.mspf > 0u ? seq.mspf : 50u;
        const std::uint32_t frame = Device.dwTimeContinual / mspf;
        std::uint32_t frameId = 0u;
        if (seq.cycled)
        {
            frameId = frame % (totalFrames * 2u);
            if (frameId >= totalFrames)
                frameId = (totalFrames - 1u) - (frameId % totalFrames);
        }
        else
        {
            frameId = frame % totalFrames;
        }
        resolvedTextureName = seq.frames[frameId];
        error.clear();
        return true;
    }
    resolvedTextureName = textureName;
    error.clear();
    return true;
}

bool ResolveMetalR4EngineTextureDdsPath(
    const std::filesystem::path& textureName,
    std::filesystem::path& resolvedPath,
    std::string& error)
{
    std::filesystem::path ddsName = textureName;
    if (_stricmp(ddsName.extension().string().c_str(), ".dds") != 0)
        ddsName += ".dds";
    return resolveTextureDdsPath(ddsName, resolvedPath, error);
}

bool LoadMetalR4EngineTextureDimensions(
    const std::filesystem::path& textureName,
    std::uint32_t& width,
    std::uint32_t& height,
    std::string& error)
{
    width = 0u;
    height = 0u;
    std::string candidateName = textureName.generic_string();
    UiSequenceDef seq;
    std::string seqError;
    if (resolveUiSequence(candidateName, seq, seqError) && !seq.frames.empty())
    {
        candidateName = seq.frames.front();
    }
    std::string videoError;
    if (GetMetalR4UiVideoDimensions(candidateName, width, height, videoError))
    {
        error.clear();
        return true;
    }
    std::filesystem::path ddsName = candidateName;
    if (_stricmp(ddsName.extension().string().c_str(), ".dds") != 0)
        ddsName += ".dds";
    std::filesystem::path resolvedPath;
    if (!resolveTextureDdsPath(ddsName, resolvedPath, error))
        return false;
    xray::metal::R4DdsBytesProvider provider;
    if (!CreateMetalR4EngineXrfsProvider(provider, error))
        return false;
    xray::metal::R4DdsTexture dds;
    if (!xray::metal::LoadR4DdsTexture(resolvedPath, provider, dds, error) ||
        dds.descriptor.type != R4TextureType::Texture2D || dds.descriptor.arrayLength != 1u ||
        dds.descriptor.width == 0u || dds.descriptor.height == 0u)
    {
        if (error.empty())
            error = "UI texture must be a non-empty 2D DDS";
        return false;
    }
    width = dds.descriptor.width;
    height = dds.descriptor.height;
    error.clear();
    return true;
}

bool ResolveMetalR4EngineCurrentLevelTextureDdsPath(
    const std::filesystem::path& textureName,
    std::filesystem::path& resolvedPath,
    std::string& error)
{
    return resolveCurrentLevelTextureDdsPath(textureName, resolvedPath, error);
}

bool ResolveMetalR4EngineLevelTextureDdsPath(
    const std::string& levelName,
    const std::filesystem::path& textureName,
    std::filesystem::path& resolvedPath,
    std::string& error)
{
    return resolveLevelTextureDdsPath(levelName, textureName, resolvedPath, error);
}


bool LoadMetalR4EngineTextureDetailScale(
    const std::string& levelName,
    const std::filesystem::path& textureName,
    float& detailScale,
    std::filesystem::path& resolvedPath,
    std::string& error)
{
    return loadTextureDetailScale(levelName, textureName, detailScale, resolvedPath, error);
}

bool ClassifyMetalR4EngineTextureBump(
    const std::filesystem::path& baseTextureDds,
    R4EngineTextureBumpKind& kind,
    std::filesystem::path& bumpTextureDds,
    std::filesystem::path& bumpXTextureDds,
    std::string& error)
{
    return classifyTextureBump(baseTextureDds, kind, bumpTextureDds, bumpXTextureDds, error);
}
#else
bool CreateMetalR4EngineXrfsProvider(
    xray::metal::R4DdsBytesProvider& provider,
    std::string& error)
{
    provider = {};
    error = "xrfs DDS paths require an engine-linked Metal backend";
    return false;
}

bool CreateMetalR4EngineLevelDetailsProvider(
    R4BinaryBytesProvider& provider,
    std::string& error)
{
    provider = {};
    error = "level.details requires an engine-linked Metal backend";
    return false;
}

bool CaptureMetalR4EngineDetailGrassMotion(
    R4DetailGrassMotionSnapshot& snapshot,
    std::string& error)
{
    snapshot = {};
    error = "detail grass motion requires an engine-linked Metal backend";
    return false;
}

bool BuildMetalR4EngineGroundedDetailGrassFrame(
    const R4DetailGrassSnapshot& snapshot,
    const std::array<float, 3>& eye,
    const std::uint64_t generation,
    R4DetailGrassFrame& frame,
    std::string& error)
{
    (void)snapshot;
    (void)eye;
    (void)generation;
    frame = {};
    error = "grounded detail grass requires an engine-linked Metal backend";
    return false;
}

bool LoadMetalR4EngineLevelDetailsSnapshot(
    R4DetailGrassSnapshot& snapshot,
    std::string& error)
{
    (void)snapshot;
    error = "level.details requires an engine-linked Metal backend";
    return false;
}

bool ResolveMetalR4EngineUiTextureFrame(
    const std::string& textureName,
    std::string& resolvedTextureName,
    std::string& error)
{
    resolvedTextureName = textureName;
    error.clear();
    return true;
}

bool ResolveMetalR4EngineTextureDdsPath(
    const std::filesystem::path& textureName,
    std::filesystem::path& resolvedPath,
    std::string& error)
{
    (void)textureName;
    resolvedPath.clear();
    error = "xrfs texture search requires an engine-linked Metal backend";
    return false;
}

bool LoadMetalR4EngineTextureDimensions(
    const std::filesystem::path& textureName,
    std::uint32_t& width,
    std::uint32_t& height,
    std::string& error)
{
    (void)textureName;
    width = 0u;
    height = 0u;
    error = "UI texture dimensions require an engine-linked Metal backend";
    return false;
}

bool ResolveMetalR4EngineCurrentLevelTextureDdsPath(
    const std::filesystem::path& textureName,
    std::filesystem::path& resolvedPath,
    std::string& error)
{
    (void)textureName;
    resolvedPath.clear();
    error = "current-level texture search requires an engine-linked Metal backend";
    return false;
}


bool ResolveMetalR4EngineLevelTextureDdsPath(
    const std::string& levelName,
    const std::filesystem::path& textureName,
    std::filesystem::path& resolvedPath,
    std::string& error)
{
    (void)levelName;
    (void)textureName;
    resolvedPath.clear();
    error = "xrfs level texture search requires an engine-linked Metal backend";
    return false;
}


bool LoadMetalR4EngineTextureDetailScale(
    const std::string& levelName,
    const std::filesystem::path& textureName,
    float& detailScale,
    std::filesystem::path& resolvedPath,
    std::string& error)
{
    (void)levelName;
    (void)textureName;
    detailScale = 1.0f;
    resolvedPath.clear();
    error = "texture THM lookup requires an engine-linked Metal backend";
    return false;
}

bool ClassifyMetalR4EngineTextureBump(
    const std::filesystem::path& baseTextureDds,
    R4EngineTextureBumpKind& kind,
    std::filesystem::path& bumpTextureDds,
    std::filesystem::path& bumpXTextureDds,
    std::string& error)
{
    (void)baseTextureDds;
    kind = R4EngineTextureBumpKind::Flat;
    bumpTextureDds.clear();
    bumpXTextureDds.clear();
    error = "texture THM bump classification requires an engine-linked Metal backend";
    return false;
}
#endif
