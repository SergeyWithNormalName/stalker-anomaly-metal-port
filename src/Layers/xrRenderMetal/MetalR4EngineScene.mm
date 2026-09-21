#include "MetalR4EngineScene.h"

#include "IR4Backend.h"
#include "MetalR4EngineFs.h"
#include "R4LocalShadowAtlasFoundation.h"
#include "R4NightVisionFoundation.h"
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

// Keep the production scene in precisely one source of truth while the
// standalone executable remains a useful validation harness.  Defining this
// macro excludes only its main() function; every parser, MSC admission check,
// resource contract, and draw helper is compiled into this private bridge TU.
#define XRAY_METAL_ENGINE_SCENE_IMPLEMENTATION 1
#include "MetalBootstrap.mm"
#undef XRAY_METAL_ENGINE_SCENE_IMPLEMENTATION

namespace
{
constexpr R4ClearColor EngineSceneClearColor{0.025, 0.055, 0.105, 1.0};

static bool decodePngImage(const std::vector<std::uint8_t>& bytes,
    xray::metal::R4DdsTexture& result, std::string& error)
{
    if (bytes.size() < 8u)
    {
        error = "image payload too small for PNG header";
        return false;
    }
    static const std::uint8_t pngMagic[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (std::memcmp(bytes.data(), pngMagic, 8) != 0)
    {
        error = "not a PNG image";
        return false;
    }
    CFDataRef encoded = CFDataCreate(kCFAllocatorDefault, bytes.data(), static_cast<CFIndex>(bytes.size()));
    if (!encoded)
    {
        error = "failed to create CFData from PNG bytes";
        return false;
    }
    CGImageSourceRef source = CGImageSourceCreateWithData(encoded, nullptr);
    CFRelease(encoded);
    if (!source)
    {
        error = "failed to create CGImageSource from PNG data";
        return false;
    }
    CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
    CFRelease(source);
    if (!image)
    {
        error = "failed to decode CGImage from PNG source";
        return false;
    }
    const size_t width = CGImageGetWidth(image);
    const size_t height = CGImageGetHeight(image);
    if (width == 0 || height == 0 || width > 16384 || height > 16384)
    {
        CGImageRelease(image);
        error = "invalid image dimensions";
        return false;
    }
    const size_t rowBytes = width * 4u;
    result.bytes.resize(rowBytes * height);
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGContextRef context = colorSpace ?
        CGBitmapContextCreate(result.bytes.data(), width, height, 8, rowBytes, colorSpace,
            kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big) : nullptr;
    if (colorSpace)
        CGColorSpaceRelease(colorSpace);
    if (!context)
    {
        CGImageRelease(image);
        result.bytes.clear();
        error = "failed to create CGBitmapContext";
        return false;
    }
    CGContextSetBlendMode(context, kCGBlendModeCopy);
    CGContextDrawImage(context, CGRectMake(0, 0, width, height), image);
    CGContextRelease(context);
    CGImageRelease(image);

    result.descriptor.width = static_cast<std::uint32_t>(width);
    result.descriptor.height = static_cast<std::uint32_t>(height);
    result.descriptor.format = R4PixelFormat::RGBA8Unorm;
    result.descriptor.type = R4TextureType::Texture2D;
    result.descriptor.arrayLength = 1u;
    result.descriptor.mipLevels = 1u;
    result.descriptor.storage = R4ResourceStorage::Private;
    result.descriptor.usage = R4TextureUsage::ShaderRead;
    result.descriptor.label = "R4 PNG-decoded texture";
    result.mipLevels.clear();
    result.mipLevels.push_back({
        static_cast<std::uint32_t>(width),
        static_cast<std::uint32_t>(height),
        0u, 0u, 0u, rowBytes, rowBytes * height
    });
    return true;
}
constexpr std::uint32_t LocalShadowAtlasExtent = 2048u;
constexpr std::uint32_t LocalShadowTileSize = 512u;
constexpr std::size_t LocalShadowTileCapacity = 16u;
constexpr std::size_t GBufferHeatTargetIndex = 2u;

// The converted HUD G-buffer shaders write L_hotness to SV_Target2.  Treat
// that attachment as a scope input only while it remains the normalized,
// shader-readable 2D heat carrier established by the four-target R4 ABI.
// Falling back to an unrelated render target would turn a binding mistake into
// a plausible-looking but semantically wrong thermal image.
bool hasReadableGBufferHeatTarget(const std::vector<std::shared_ptr<IR4Texture>>& targets,
    const R4Texture2DDesc& sceneColor)
{
    if (targets.size() != 4u || !targets[GBufferHeatTargetIndex])
        return false;
    const R4Texture2DDesc& heat = targets[GBufferHeatTargetIndex]->Descriptor();
    return heat.type == R4TextureType::Texture2D &&
        heat.format == R4PixelFormat::RGBA8Unorm &&
        heat.width == sceneColor.width && heat.height == sceneColor.height &&
        HasR4TextureUsage(heat.usage, R4TextureUsage::ShaderRead);
}

struct LocalShadowScene
{
    std::shared_ptr<IR4Texture> atlas;
    std::shared_ptr<IR4Texture> depth;
    std::shared_ptr<IR4Buffer> transforms;
    std::vector<xray::metal::R4LocalShadowAtlasRecord> records;
    std::size_t candidates = 0;
    std::size_t renderedLights = 0;
    std::size_t submittedCasters = 0;
    std::size_t culledCasters = 0;
    std::size_t dynamicCandidates = 0;
    std::size_t submittedDynamicCasters = 0;
    std::size_t culledDynamicCasters = 0;
    std::size_t rejectedDynamicCasters = 0;
    std::size_t omittedDynamicCasters = 0;
    bool ready = false;
};

struct NightVisionScene
{
    std::array<std::shared_ptr<IR4GraphicsPipeline>, 3> pipelines;
    std::shared_ptr<IR4GraphicsPipeline> blurPipeline;
    std::vector<std::shared_ptr<IR4Buffer>> vertices;
    std::vector<std::shared_ptr<IR4Buffer>> globals;
    std::vector<std::shared_ptr<IR4Buffer>> staticGlobals;
    std::vector<std::array<std::shared_ptr<IR4Buffer>, 6>> blurGlobals;
    std::shared_ptr<IR4Buffer> indices;
    std::shared_ptr<IR4Sampler> sampler;
    std::shared_ptr<IR4Texture> sceneSnapshot;
    std::array<std::shared_ptr<IR4Texture>, 3> blurHorizontal;
    std::array<std::shared_ptr<IR4Texture>, 3> blurFinal;
    std::filesystem::path shaderDirectory;
    std::uint32_t lastLoggedFrame = UINT32_MAX;
    std::int32_t lastLoggedGeneration = 0;
    bool ready = false;
};

Matrix4 makeR4SpotLightView(const R4EngineLocalLight& light)
{
    Vec3 direction = normalize({light.direction[0], light.direction[1], light.direction[2]});
    Vec3 right{light.right[0], light.right[1], light.right[2]};
    const float rightLength2 = dot(right, right);
    Vec3 up;
    if (rightLength2 > 1.0e-8f && std::isfinite(rightLength2))
    {
        right = normalize(right);
        up = normalize(cross(direction, right));
        right = normalize(cross(up, direction));
    }
    else
    {
        up = std::abs(direction.y) > .99f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
        right = normalize(cross(up, direction));
        up = normalize(cross(direction, right));
    }
    const Vec3 position{light.position[0], light.position[1], light.position[2]};
    return {{
        right.x, up.x, direction.x, 0,
        right.y, up.y, direction.y, 0,
        right.z, up.z, direction.z, 0,
        -dot(position, right), -dot(position, up), -dot(position, direction), 1,
    }};
}

Matrix4 makeR4SpotLightProjection(const R4EngineLocalLight& light)
{
    const float nearPlane = light.virtualSize;
    const float farPlane = light.range + 1.0e-5f;
	// R4 deliberately gives OmniFace a much wider shadow frustum.  Treating it
	// as an ordinary spot clips point-light faces and makes campfires/lamps look
	// unshadowed or disappear at their seams.
	const float tanShift = light.type == R4EngineLocalLightType::OmniFace ? 0.3f : 0.0610865f;
    const float cotangent = 1.0f / std::tan((light.cone + tanShift) * .5f);
    const float q = farPlane / (farPlane - nearPlane);
    Matrix4 result{};
    result.elements[0] = cotangent;
    result.elements[5] = cotangent;
    result.elements[10] = q;
    result.elements[11] = 1.0f;
    result.elements[14] = -q * nearPlane;
    return result;
}

Matrix4 makeSpotVolumeModel(const R4EngineLocalLight& light)
{
    Vec3 direction = normalize({light.direction[0], light.direction[1], light.direction[2]});
    Vec3 right{light.right[0], light.right[1], light.right[2]};
    const float rightLength2 = dot(right, right);
    Vec3 up;
    if (rightLength2 > 1.0e-8f && std::isfinite(rightLength2))
    {
        right = normalize(right);
        up = normalize(cross(direction, right));
        right = normalize(cross(up, direction));
    }
    else
    {
        up = std::abs(direction.y) > .99f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
        right = normalize(cross(up, direction));
        up = normalize(cross(direction, right));
    }
	const float diameter = light.type == R4EngineLocalLightType::OmniFace ?
		2.0f * light.range : 2.0f * light.range * std::tan(light.cone * .5f);
    Matrix4 model{};
    model.elements[0]=right.x*diameter; model.elements[1]=right.y*diameter; model.elements[2]=right.z*diameter;
    model.elements[4]=up.x*diameter; model.elements[5]=up.y*diameter; model.elements[6]=up.z*diameter;
	const float depth = light.type == R4EngineLocalLightType::OmniFace ? 2.0f * light.range : light.range;
    model.elements[8]=direction.x*depth; model.elements[9]=direction.y*depth; model.elements[10]=direction.z*depth;
    model.elements[12]=light.position[0]; model.elements[13]=light.position[1]; model.elements[14]=light.position[2];
    model.elements[15]=1.0f;
    return model;
}

bool isXrfsPath(const std::filesystem::path& path)
{
    return path.generic_string().rfind("xrfs:", 0u) == 0u;
}

bool parseUnsigned(const char* value, std::uint32_t& result)
{
    if (!value || !*value)
        return false;
    char* end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed > UINT32_MAX)
        return false;
    result = static_cast<std::uint32_t>(parsed);
    return true;
}

bool parseControlledMaterials(
    const char* value,
    std::vector<xray::metal::R4ControlledMaterial>& materials,
    std::string& error)
{
    if (!value || !*value)
        return false;
    const std::string list(value);
    std::size_t entryStart = 0;
    while (entryStart <= list.size())
    {
        const std::size_t entryEnd = list.find(';', entryStart);
        const std::string entry = list.substr(entryStart, entryEnd - entryStart);
        const std::size_t first = entry.find('|');
        const std::size_t second = first == std::string::npos ?
            std::string::npos : entry.find('|', first + 1);
        if (entry.empty() || first == std::string::npos || second == std::string::npos ||
            entry.find('|', second + 1) != std::string::npos)
        {
            error = "XRAY_METAL_MATERIALS entries must be visualId|base.dds|hemi.dds";
            return false;
        }
        xray::metal::R4ControlledMaterial material;
        if (!parseUnsigned(entry.substr(0, first).c_str(), material.visualId))
        {
            error = "XRAY_METAL_MATERIALS visual id must be an unsigned 32-bit integer";
            return false;
        }
        material.baseDdsPath = entry.substr(first + 1, second - first - 1);
        material.hemiDdsPath = entry.substr(second + 1);
        materials.emplace_back(std::move(material));
        if (entryEnd == std::string::npos)
            break;
        entryStart = entryEnd + 1;
    }
    return true;
}

struct AutomaticLmhSelection
{
    std::vector<LevelMaterialInput> materials;
    std::size_t requested = 0;
    std::size_t admitted = 0;
    std::size_t skippedInvalid = 0;
    std::size_t skippedDuplicate = 0;
    std::size_t skippedShader = 0;
    std::size_t skippedTextureSlots = 0;
    std::size_t skippedVertexAbi = 0;
    std::size_t skippedBaseTexture = 0;
    std::size_t skippedHemiTexture = 0;
};

bool hasExactLmhVertexAbi(const xray::metal::R4VertexBufferSummary& vertexBuffer)
{
    const auto hasElement = [&vertexBuffer](std::uint8_t type, std::uint8_t usage,
        std::uint8_t usageIndex) {
        return std::count_if(vertexBuffer.declaration.begin(), vertexBuffer.declaration.end(),
            [type, usage, usageIndex](const xray::metal::R4VertexElement& element) {
                return element.stream == 0u && element.type == type && element.method == 0u &&
                    element.usage == usage && element.usageIndex == usageIndex;
            }) == 1;
    };
    const auto normal = std::find_if(vertexBuffer.declaration.begin(), vertexBuffer.declaration.end(),
        [](const xray::metal::R4VertexElement& element) {
            return element.stream == 0u && element.type == 4u && element.method == 0u &&
                element.usage == 3u && element.usageIndex == 0u;
        });
    return vertexBuffer.hasPositionFloat3 &&
        hasElement(2u, 0u, 0u) && // POSITION0/FLOAT3
        hasElement(4u, 3u, 0u) && // NORMAL0/D3DCOLOR
        hasElement(4u, 6u, 0u) && // TANGENT0/D3DCOLOR
        hasElement(4u, 7u, 0u) && // BINORMAL0/D3DCOLOR
        hasElement(6u, 5u, 0u) && // TEXCOORD0/SHORT2
        hasElement(6u, 5u, 1u) && // TEXCOORD1/SHORT2
        normal != vertexBuffer.declaration.end() && normal->offset != 0u;
}

bool selectAutomaticLmhMaterials(
    const std::filesystem::path& levelDirectory,
    const bool includeAref,
    AutomaticLmhSelection& selection,
    std::string& error)
{
    selection = {};
    xray::metal::R4LevelGeometry geometry;
    if (!xray::metal::LoadR4LevelGeometry(levelDirectory, geometry, error))
        return false;
    const std::string levelName = levelDirectory.lexically_normal().filename().string();
    if (levelName.empty())
    {
        error = "automatic LMH admission cannot derive a level name";
        return false;
    }
    std::unordered_set<std::uint32_t> seenVisuals;
    selection.materials.reserve(geometry.visuals.size());
    for (const xray::metal::R4StaticVisual& visual : geometry.visuals)
    {
        if (!seenVisuals.emplace(visual.visualId).second)
        {
            ++selection.skippedDuplicate;
            continue;
        }
        ++selection.requested;
        if (!visual.valid || !visual.referencedPositionBounds.available ||
            visual.shaderId >= geometry.shaders.size() || visual.vertexBufferId >= geometry.vertexBuffers.size())
        {
            ++selection.skippedInvalid;
            continue;
        }
        const xray::metal::R4LevelShader& shader = geometry.shaders[visual.shaderId];
        if (shader.shaderName != "default" &&
            (!includeAref || shader.shaderName != "def_shaders\\def_aref"))
        {
            ++selection.skippedShader;
            continue;
        }
        if (shader.textures.size() != 3u || shader.baseTexture.empty() ||
            shader.textures[0] != shader.baseTexture ||
            shader.textures[1].rfind("lmap#", 0u) != 0u ||
            shader.textures[2].rfind("lmap#", 0u) != 0u ||
            shader.textures[1].size() <= 6u || shader.textures[2].size() <= 6u ||
            shader.textures[1].substr(shader.textures[1].size() - 2u) != "_1" ||
            shader.textures[2].substr(shader.textures[2].size() - 2u) != "_2")
        {
            ++selection.skippedTextureSlots;
            continue;
        }
        if (!hasExactLmhVertexAbi(geometry.vertexBuffers[visual.vertexBufferId]))
        {
            ++selection.skippedVertexAbi;
            continue;
        }
        std::filesystem::path baseDds;
        std::string localError;
        std::string normalizedBaseIdentity = shader.baseTexture;
        std::replace(normalizedBaseIdentity.begin(), normalizedBaseIdentity.end(), '\\', '/');
        const std::filesystem::path baseIdentity = normalizedBaseIdentity + ".dds";
        const auto leafFilename = [](const std::filesystem::path& p) {
            std::string s = p.generic_string();
            std::replace(s.begin(), s.end(), '\\', '/');
            const std::size_t slash = s.rfind('/');
            return slash == std::string::npos ? s : s.substr(slash + 1u);
        };
        if (!ResolveMetalR4EngineTextureDdsPath(baseIdentity, baseDds, localError) ||
            leafFilename(baseDds) != leafFilename(baseIdentity))
        {
            ++selection.skippedBaseTexture;
            continue;
        }
        std::filesystem::path hemiDds;
        const std::filesystem::path hemiIdentity = shader.textures[2] + ".dds";
        if (!ResolveMetalR4EngineLevelTextureDdsPath(
                levelName, hemiIdentity, hemiDds, localError))
        {
            ++selection.skippedHemiTexture;
            continue;
        }
        selection.materials.push_back({visual.visualId, std::move(baseDds), std::move(hemiDds)});
        ++selection.admitted;
    }
    if (selection.materials.empty())
    {
        std::fprintf(stderr,
            "* [Metal] Automatic LMH admission: level '%s' has no default+LMH lightmapped terrain (indoor/underground level)\n",
            levelName.c_str());
        error.clear();
        return true;
    }
    error.clear();
    return true;
}

bool parseTerrainVisualIds(
    const char* value,
    std::vector<std::uint32_t>& visualIds,
    std::string& error)
{
    visualIds.clear();
    if (!value || !*value)
        return true;
    const std::string list(value);
    std::unordered_set<std::uint32_t> unique;
    std::size_t begin = 0;
    while (begin <= list.size())
    {
        const std::size_t end = list.find(',', begin);
        std::string token = list.substr(begin, end - begin);
        const auto first = std::find_if_not(token.begin(), token.end(), [](unsigned char c) {
            return std::isspace(c) != 0;
        });
        const auto last = std::find_if_not(token.rbegin(), token.rend(), [](unsigned char c) {
            return std::isspace(c) != 0;
        }).base();
        token = first < last ? std::string(first, last) : std::string();
        std::uint32_t visualId = 0;
        if (!parseUnsigned(token.c_str(), visualId) || !unique.emplace(visualId).second)
        {
            error = "XRAY_METAL_TERRAIN_VISUALS must be a comma-separated unique uint32 list";
            return false;
        }
        visualIds.push_back(visualId);
        if (end == std::string::npos)
            break;
        begin = end + 1u;
    }
    return !visualIds.empty();
}

struct TerrainVertex
{
    float normal[4];
    float tangent[4];
    float binormal[4];
    std::int32_t texcoord[2];
    float position[4];
};
static_assert(sizeof(TerrainVertex) == 72u,
    "Terrain MSC vertex ABI must remain 3x float4 + int2 + float4");

struct TerrainTextureAsset
{
    std::string bindingName;
    std::filesystem::path path;
    xray::metal::R4DdsTexture dds;
    std::shared_ptr<IR4Texture> texture;
    std::uint64_t uploadedBytes = 0;
};

struct TerrainDraw
{
    std::uint32_t visualId = 0;
    std::uint32_t sourceVertexBuffer = 0;
    std::uint32_t vertexCount = 0;
    std::uint32_t indexCount = 0;
    std::string shaderName;
    std::array<std::string, 17> textureIdentities;
    std::array<std::shared_ptr<IR4Texture>, 17> textures;
    float detailScale = 1.0f;
    Matrix4 modelTransform{};
    xray::metal::R4PositionBounds localPositionBounds;
    std::vector<TerrainVertex> vertices;
    std::vector<std::uint16_t> indices;
    std::shared_ptr<IR4Buffer> vertexBuffer;
    std::shared_ptr<IR4Buffer> indexBuffer;
    std::shared_ptr<IR4Buffer> vertexGlobals;
    std::shared_ptr<IR4Buffer> vertexTransforms;
    std::shared_ptr<IR4Buffer> fragmentGlobals;
    std::shared_ptr<IR4Buffer> fragmentStaticGlobals;
};

struct TerrainScene
{
    bool requested = false;
    bool ready = false;
    bool uploaded = false;
    bool disableLogged = false;
    bool liveFrustumReported = false;
    bool gameplayFrustumReported = false;
    std::uint64_t renderCount = 0;
    std::size_t lastSubmitted = 0;
    std::size_t lastCulled = 0;
    std::uint64_t uploadedBytes = 0;
    std::filesystem::path shaderDirectory;
    std::vector<TerrainTextureAsset> textures;
    std::vector<TerrainDraw> draws;
    std::shared_ptr<IR4GraphicsPipeline> pipeline;
    std::shared_ptr<IR4Sampler> linearSampler;
    std::shared_ptr<IR4Sampler> baseSampler;
};

#pragma pack(push, 1)
struct SkyVertex
{
    float position[3];
    std::uint32_t color;
    float texcoord0[3];
    float texcoord1[3];
};
#pragma pack(pop)
static_assert(sizeof(SkyVertex) == 40u,
    "sky2 must retain the R4 FVF::XYZ/D3DCOLOR/TEX3/TEX3 source stride");

struct CloudVertex
{
    float position[3];
    std::uint32_t windDirections;
    std::uint32_t color;
};
static_assert(sizeof(CloudVertex) == 20u,
    "clouds must retain the R4 FVF::XYZ/D3DCOLOR/D3DCOLOR source stride");

struct SkyTextureAsset
{
    std::filesystem::path resolvedPath;
    xray::metal::R4DdsTexture dds;
    std::shared_ptr<IR4Texture> texture;
};

struct SkyScene
{
    bool configured = false;
    bool ready = false;
    bool unavailableLogged = false;
    bool combineUnavailableLogged = false;
    bool resourceUnavailableLogged = false;
    std::filesystem::path shaderDirectory;
    xray::metal::R4DdsBytesProvider ddsBytesProvider;
    std::shared_ptr<IR4GraphicsPipeline> pipeline;
    std::shared_ptr<IR4Buffer> vertices;
    std::shared_ptr<IR4Buffer> indices;
    std::shared_ptr<IR4Buffer> vertexTransforms;
    std::shared_ptr<IR4Buffer> fragmentGlobals;
    std::shared_ptr<IR4Buffer> fragmentStaticGlobals;
    std::shared_ptr<IR4Texture> tonemap;
    std::shared_ptr<IR4Sampler> sampler;
    std::shared_ptr<IR4GraphicsPipeline> cloudsPipeline;
    std::shared_ptr<IR4Buffer> cloudsVertices;
    std::shared_ptr<IR4Buffer> cloudsIndices;
    std::shared_ptr<IR4Buffer> cloudsTransforms;
    std::shared_ptr<IR4Sampler> cloudsSampler;
    std::unordered_map<std::string, SkyTextureAsset> cloudTextures;
    std::filesystem::path cloudsShaderDirectory;
    std::size_t cloudsVertexCapacity = 0;
    std::size_t cloudsIndexCapacity = 0;
    std::uint64_t cloudsDrawCount = 0;
    std::uint32_t lastCloudLoggedFrame = UINT32_MAX;
    bool tonemapUploaded = false;
    std::unordered_map<std::string, SkyTextureAsset> textures;
    std::string lastCombineEnvironmentSignature;
    std::uint64_t drawCount = 0;
    std::uint32_t lastLoggedFrame = UINT32_MAX;
    std::uint32_t lastCombineUnavailableFrame = UINT32_MAX;
    std::uint32_t lastResourceUnavailableFrame = UINT32_MAX;
};

constexpr std::array<const char*, 17> TerrainTextureBindingNames = {{
    "s_base", "s_mask", "s_dt_r", "s_dt_g", "s_dt_b", "s_dt_a",
    "s_dn_r", "s_dn_g", "s_dn_b", "s_dn_a", "s_puddles_normal",
    "s_puddles_mask", "s_rainsplash", "s_height_r", "s_height_g",
    "s_height_b", "s_height_a",
}};

using TerrainMaterialMap =
    std::unordered_map<std::string, std::array<std::string, 4>>;

bool receiptContains(const std::string& receipt, const std::string& key,
    const std::string& value)
{
    return receipt.find(key + "=" + value) != std::string::npos;
}

bool validateSkyMscReflection(
    const std::filesystem::path& shaderDirectory,
    std::filesystem::path& vertexMetallib,
    std::filesystem::path& fragmentMetallib,
    std::string& error)
{
    vertexMetallib = shaderDirectory / "sky2.vs.metallib";
    fragmentMetallib = shaderDirectory / "sky2.ps.metallib";
    const std::filesystem::path vertexJsonPath = shaderDirectory / "sky2.vs.json";
    const std::filesystem::path fragmentJsonPath = shaderDirectory / "sky2.ps.json";
    const std::filesystem::path receiptPath = shaderDirectory / "sky2-msc-receipt.txt";
    std::string vertexJson;
    std::string fragmentJson;
    std::string receipt;
    std::string vertexMetallibHash;
    std::string fragmentMetallibHash;
    std::string vertexReflectionHash;
    std::string fragmentReflectionHash;
    if (!readTextFile(vertexJsonPath, vertexJson) ||
        !readTextFile(fragmentJsonPath, fragmentJson) ||
        !readTextFile(receiptPath, receipt) ||
        !fileSha256(vertexMetallib, vertexMetallibHash) ||
        !fileSha256(fragmentMetallib, fragmentMetallibHash) ||
        !fileSha256(vertexJsonPath, vertexReflectionHash) ||
        !fileSha256(fragmentJsonPath, fragmentReflectionHash) ||
        !receiptContains(receipt, "evidence_scope",
            "exact_active_r4_gamma_sky2_non_msaa_dxc_msc_artifacts") ||
        !receiptContains(receipt, "metallib_sha256", vertexMetallibHash) ||
        !receiptContains(receipt, "metallib_sha256", fragmentMetallibHash) ||
        !receiptContains(receipt, "reflection_sha256", vertexReflectionHash) ||
        !receiptContains(receipt, "reflection_sha256", fragmentReflectionHash))
    {
        error = "sky2 MSC artifacts do not match sky2-msc-receipt.txt";
        return false;
    }
    const std::string vertexTlab = automaticLinearTlab({{0, "SRV"}, {0, "CBV"}});
    const std::string fragmentTlab = automaticLinearTlab({
        {0, "SRV"}, {1, "SRV"}, {0, "CBV"}, {1, "CBV"}, {0, "Sampler"},
    });
    static constexpr const char* VertexInputs =
        "\"vertex_inputs\":[{\"columnCount\":4,\"elementType\":\"Float\",\"index\":0,"
        "\"name\":\"position0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":1,"
        "\"name\":\"color0\"},{\"columnCount\":3,\"elementType\":\"Float\",\"index\":2,"
        "\"name\":\"texcoord0\"},{\"columnCount\":3,\"elementType\":\"Float\",\"index\":3,"
        "\"name\":\"texcoord1\"}]";
    if (vertexJson.find("\"ShaderType\":\"Vertex\"") == std::string::npos ||
        vertexJson.find("\"ResourceCount\":2") == std::string::npos ||
        vertexJson.find(vertexTlab) == std::string::npos ||
        vertexJson.find(VertexInputs) == std::string::npos ||
        fragmentJson.find("\"ShaderType\":\"Fragment\"") == std::string::npos ||
        fragmentJson.find("\"ResourceCount\":5") == std::string::npos ||
        fragmentJson.find(fragmentTlab) == std::string::npos ||
        fragmentJson.find("\"num_render_targets\":2") == std::string::npos)
    {
        error = "sky2 MSC reflection does not match the exact R4/GAMMA TLAB and vertex ABI";
        return false;
    }
    return true;
}

bool prepareSkyScene(
    IR4Backend& backend,
    const std::filesystem::path& shaderDirectory,
    const xray::metal::R4DdsBytesProvider& ddsBytesProvider,
    SkyScene& sky,
    std::string& error)
{
    sky = {};
    if (shaderDirectory.empty())
        return true;
    sky.configured = true;
    if (!ddsBytesProvider)
    {
        error = "sky2 MSC requires the engine XRFS DDS provider";
        return false;
    }
    std::filesystem::path vertexMetallib;
    std::filesystem::path fragmentMetallib;
    if (!validateSkyMscReflection(shaderDirectory, vertexMetallib, fragmentMetallib, error))
        return false;

    const std::string vertexPath = vertexMetallib.string();
    const std::string fragmentPath = fragmentMetallib.string();
    R4GraphicsPipelineDesc pipeline;
    pipeline.label = "R4 converted sky2 background";
    pipeline.shaderKey = "r4-gamma-sky2-nomsaa-msc-v1";
    pipeline.vertexMetallibPath = vertexPath.c_str();
    pipeline.fragmentMetallibPath = fragmentPath.c_str();
    pipeline.vertexAttributes = {
        {11u, 6u, 0u, R4VertexFormat::Float3},
        {12u, 6u, 12u, R4VertexFormat::UChar4Normalized},
        {13u, 6u, 16u, R4VertexFormat::Float3},
        {14u, 6u, 28u, R4VertexFormat::Float3},
    };
    pipeline.vertexLayouts = {{6u, sizeof(SkyVertex)}};
    pipeline.colorFormats = {R4PixelFormat::RGBA8Unorm, R4PixelFormat::RGBA8Unorm};
    pipeline.depthFormat = R4PixelFormat::Depth32FloatStencil8;
    pipeline.depthCompare = R4CompareFunction::Always;
    pipeline.depthWrite = false;
    pipeline.cullMode = R4CullMode::None;
    // The G-buffer writes 1, then the direct-sun mask may promote geometry to
    // 5. Both are non-zero. Sampling only stencil zero after combine preserves
    // every geometric pixel while sky2 fills the exact cleared background.
    pipeline.stencil.enabled = true;
    pipeline.stencil.frontFace.compare = R4CompareFunction::Equal;
    pipeline.stencil.frontFace.readMask = 0xffu;
    pipeline.stencil.frontFace.writeMask = 0u;
    pipeline.stencil.backFace = pipeline.stencil.frontFace;
    sky.pipeline = backend.CreateGraphicsPipeline(pipeline, error);
    if (!sky.pipeline)
        return false;

    static constexpr std::uint16_t Indices[60] = {
        0,2,3, 3,1,0, 4,5,7, 7,6,4, 0,1,9, 9,8,0,
        8,9,5, 5,4,8, 1,3,10, 10,9,1, 9,10,7, 7,5,9,
        3,2,11, 11,10,3, 10,11,6, 6,7,10, 2,0,8, 8,11,2, 11,8,4, 4,6,11,
    };
    IR4Resources& resources = backend.Resources();
    R4BufferDesc vertexDesc;
    vertexDesc.size = sizeof(SkyVertex) * 12u;
    vertexDesc.storage = R4ResourceStorage::Shared;
    vertexDesc.usage = R4BufferUsage::Vertex;
    vertexDesc.label = "R4 sky2 FVF source vertices";
    sky.vertices = resources.CreateBuffer(vertexDesc, nullptr, 0u, error);
    R4BufferDesc indexDesc;
    indexDesc.size = sizeof(Indices);
    indexDesc.storage = R4ResourceStorage::Shared;
    indexDesc.usage = R4BufferUsage::Index;
    indexDesc.label = "R4 sky2 FVF source indices";
    sky.indices = resources.CreateBuffer(indexDesc, Indices, sizeof(Indices), error);
    const auto createConstants = [&resources, &error](std::size_t size, const char* label) {
        R4BufferDesc descriptor;
        descriptor.size = size;
        descriptor.storage = R4ResourceStorage::Shared;
        descriptor.usage = R4BufferUsage::Uniform;
        descriptor.label = label;
        return resources.CreateBuffer(descriptor, nullptr, 0u, error);
    };
    sky.vertexTransforms = createConstants(256u, "R4 sky2 dynamic_transforms b0");
    sky.fragmentGlobals = createConstants(192u, "R4 sky2 globals b0");
    sky.fragmentStaticGlobals = createConstants(528u, "R4 sky2 static_globals b1");
    R4Texture2DDesc tonemap;
    tonemap.width = 1u;
    tonemap.height = 1u;
    tonemap.format = R4PixelFormat::R32Float;
    tonemap.storage = R4ResourceStorage::Private;
    tonemap.usage = R4TextureUsage::ShaderRead;
    tonemap.label = "R4 sky2 controlled tonemap";
    sky.tonemap = resources.CreateTexture2D(tonemap, error);
    R4SamplerDesc sampler;
    sampler.minFilter = R4SamplerFilter::Linear;
    sampler.magFilter = R4SamplerFilter::Linear;
    sampler.mipFilter = R4SamplerFilter::Linear;
    sampler.addressU = R4SamplerAddressMode::ClampToEdge;
    sampler.addressV = R4SamplerAddressMode::ClampToEdge;
    sampler.addressW = R4SamplerAddressMode::ClampToEdge;
    sampler.label = "R4 sky2 smp_rtlinear";
    sky.sampler = resources.CreateSampler(sampler, error);
    if (!sky.vertices || !sky.indices || !sky.vertexTransforms || !sky.fragmentGlobals ||
        !sky.fragmentStaticGlobals || !sky.tonemap || !sky.sampler)
        return false;
    sky.shaderDirectory = shaderDirectory;
    sky.ddsBytesProvider = ddsBytesProvider;
    sky.ready = true;
    return true;
}

bool validateCloudsMscReflection(const std::filesystem::path& shaderDirectory,
    std::filesystem::path& vertexMetallib, std::filesystem::path& fragmentMetallib,
    std::string& error)
{
    vertexMetallib = shaderDirectory / "clouds.vs.metallib";
    fragmentMetallib = shaderDirectory / "clouds.ps.metallib";
    std::string vertexJson, fragmentJson, receipt;
    std::string vertexHash, fragmentHash, vertexJsonHash, fragmentJsonHash;
    if (!readTextFile(shaderDirectory / "clouds.vs.json", vertexJson) ||
        !readTextFile(shaderDirectory / "clouds.ps.json", fragmentJson) ||
        !readTextFile(shaderDirectory / "clouds-msc-receipt.txt", receipt) ||
        !fileSha256(shaderDirectory / "clouds.vs.metallib", vertexHash) ||
        !fileSha256(shaderDirectory / "clouds.ps.metallib", fragmentHash) ||
        !fileSha256(shaderDirectory / "clouds.vs.json", vertexJsonHash) ||
        !fileSha256(shaderDirectory / "clouds.ps.json", fragmentJsonHash) ||
        !receiptContains(receipt, "evidence_scope",
            "exact_active_r4_gamma_clouds_non_msaa_dxc_msc_artifacts") ||
        !receiptContains(receipt, "permutation", "clouds_non_msaa_r4_gamma") ||
        !receiptContains(receipt, "metallib_sha256", vertexHash) ||
        !receiptContains(receipt, "metallib_sha256", fragmentHash) ||
        !receiptContains(receipt, "reflection_sha256", vertexJsonHash) ||
        !receiptContains(receipt, "reflection_sha256", fragmentJsonHash) ||
        vertexJson.find("\"ShaderType\":\"Vertex\"") == std::string::npos ||
        vertexJson.find("\"ResourceCount\":3") == std::string::npos ||
        vertexJson.find("\"EltOffset\":48") == std::string::npos ||
        vertexJson.find("\"name\":\"color1\"") == std::string::npos ||
        fragmentJson.find("\"ShaderType\":\"Fragment\"") == std::string::npos ||
        fragmentJson.find("\"ResourceCount\":4") == std::string::npos ||
        fragmentJson.find("\"EltOffset\":72") == std::string::npos ||
        fragmentJson.find("\"num_render_targets\":1") == std::string::npos)
    {
        error = "clouds MSC artifacts, receipt, or reflection ABI mismatch";
        return false;
    }
    return true;
}

bool prepareCloudsScene(IR4Backend& backend, const std::filesystem::path& shaderDirectory,
    SkyScene& sky, std::string& error)
{
    if (shaderDirectory.empty())
        return true;
    if (!sky.ddsBytesProvider)
    {
        error = "clouds MSC requires the engine XRFS DDS provider";
        return false;
    }
    std::filesystem::path vertexMetallib, fragmentMetallib;
    if (!validateCloudsMscReflection(shaderDirectory, vertexMetallib, fragmentMetallib, error))
        return false;
    const std::string vertexPath = vertexMetallib.string();
    const std::string fragmentPath = fragmentMetallib.string();
    R4GraphicsPipelineDesc pipeline;
    pipeline.label = "R4 converted clouds hemisphere";
    pipeline.shaderKey = "r4-gamma-clouds-nomsaa-msc-v1";
    pipeline.vertexMetallibPath = vertexPath.c_str();
    pipeline.fragmentMetallibPath = fragmentPath.c_str();
    pipeline.vertexAttributes = {
        {11u, 6u, offsetof(CloudVertex, position), R4VertexFormat::Float3},
        {12u, 6u, offsetof(CloudVertex, windDirections), R4VertexFormat::UChar4Normalized},
        {13u, 6u, offsetof(CloudVertex, color), R4VertexFormat::UChar4Normalized},
    };
    pipeline.vertexLayouts = {{6u, sizeof(CloudVertex)}};
    pipeline.colorFormats = {R4PixelFormat::RGBA8Unorm};
    pipeline.depthFormat = R4PixelFormat::Depth32FloatStencil8;
    pipeline.depthCompare = R4CompareFunction::Always;
    pipeline.depthWrite = false;
    pipeline.cullMode = R4CullMode::None;
    pipeline.blend.enabled = true;
    pipeline.blend.sourceRgb = pipeline.blend.sourceAlpha = R4BlendFactor::SourceAlpha;
    pipeline.blend.destinationRgb = pipeline.blend.destinationAlpha = R4BlendFactor::OneMinusSourceAlpha;
    sky.cloudsPipeline = backend.CreateGraphicsPipeline(pipeline, error);
    if (!sky.cloudsPipeline)
        return false;
    R4BufferDesc transforms;
    transforms.size = 256u;
    transforms.storage = R4ResourceStorage::Shared;
    transforms.usage = R4BufferUsage::Uniform;
    transforms.label = "R4 clouds dynamic_transforms b0";
    sky.cloudsTransforms = backend.Resources().CreateBuffer(transforms, nullptr, 0u, error);
    R4SamplerDesc sampler;
    sampler.minFilter = sampler.magFilter = sampler.mipFilter = R4SamplerFilter::Linear;
    sampler.addressU = sampler.addressV = sampler.addressW = R4SamplerAddressMode::Repeat;
    sampler.maxAnisotropy = 8u;
    sampler.label = "R4 clouds smp_base";
    sky.cloudsSampler = backend.Resources().CreateSampler(sampler, error);
    if (!sky.cloudsTransforms || !sky.cloudsSampler)
        return false;
    sky.cloudsShaderDirectory = shaderDirectory;
    return true;
}

bool validateTerrainMscReflection(
    const std::filesystem::path& shaderDirectory,
    std::filesystem::path& vertexMetallib,
    std::filesystem::path& fragmentMetallib,
    std::string& error)
{
    vertexMetallib = shaderDirectory / "deffer_terrain_flat_d.vs.metallib";
    fragmentMetallib = shaderDirectory / "deffer_terrain_high_flat_d.ps.metallib";
    const auto vertexJsonPath = shaderDirectory / "deffer_terrain_flat_d.vs.json";
    const auto fragmentJsonPath = shaderDirectory / "deffer_terrain_high_flat_d.ps.json";
    const auto receiptPath = shaderDirectory / "terrain-l05-bar-msc-receipt.txt";
    std::string vertexJson;
    std::string fragmentJson;
    std::string receipt;
    std::string vertexMetallibHash;
    std::string fragmentMetallibHash;
    std::string vertexReflectionHash;
    std::string fragmentReflectionHash;
    if (!readTextFile(vertexJsonPath, vertexJson) ||
        !readTextFile(fragmentJsonPath, fragmentJson) ||
        !readTextFile(receiptPath, receipt) ||
        !fileSha256(vertexMetallib, vertexMetallibHash) ||
        !fileSha256(fragmentMetallib, fragmentMetallibHash) ||
        !fileSha256(vertexJsonPath, vertexReflectionHash) ||
        !fileSha256(fragmentJsonPath, fragmentReflectionHash) ||
        !receiptContains(receipt, "vertex_metallib_sha256", vertexMetallibHash) ||
        !receiptContains(receipt, "fragment_metallib_sha256", fragmentMetallibHash) ||
        !receiptContains(receipt, "vertex_reflection_sha256", vertexReflectionHash) ||
        !receiptContains(receipt, "fragment_reflection_sha256", fragmentReflectionHash))
    {
        error = "terrain MSC artifacts do not match terrain-l05-bar-msc-receipt.txt";
        return false;
    }
    static constexpr const char* VertexInputs =
        "\"vertex_inputs\":[{\"columnCount\":4,\"elementType\":\"Float\",\"index\":0,"
        "\"name\":\"normal0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":1,"
        "\"name\":\"tangent0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":2,"
        "\"name\":\"binormal0\"},{\"columnCount\":2,\"elementType\":\"Int32\",\"index\":3,"
        "\"name\":\"texcoord0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":4,"
        "\"name\":\"position0\"}]";
    const std::string vertexTlab = automaticLinearTlab({{0, "CBV"}, {1, "CBV"}});
    const std::string fragmentTlab = automaticLinearTlab({
        {0, "SRV"}, {1, "SRV"}, {2, "SRV"}, {3, "SRV"}, {4, "SRV"},
        {5, "SRV"}, {6, "SRV"}, {7, "SRV"}, {8, "SRV"}, {9, "SRV"},
        {10, "SRV"}, {11, "SRV"}, {12, "SRV"}, {13, "SRV"}, {14, "SRV"},
        {15, "SRV"}, {16, "SRV"}, {0, "CBV"}, {1, "CBV"},
        {0, "Sampler"}, {1, "Sampler"},
    });
    if (vertexJson.find("\"ShaderType\":\"Vertex\"") == std::string::npos ||
        vertexJson.find("\"ResourceCount\":2") == std::string::npos ||
        vertexJson.find(vertexTlab) == std::string::npos ||
        vertexJson.find(VertexInputs) == std::string::npos ||
        fragmentJson.find("\"ShaderType\":\"Fragment\"") == std::string::npos ||
        fragmentJson.find("\"ResourceCount\":21") == std::string::npos ||
        fragmentJson.find(fragmentTlab) == std::string::npos ||
        fragmentJson.find("\"num_render_targets\":4") == std::string::npos)
    {
        error = "terrain MSC reflection does not match the exact HQ terrain ABI";
        return false;
    }
    return true;
}

bool buildTerrainMaterialMap(
    const std::vector<MetalR4EngineScene::TerrainShaderMaterial>& source,
    TerrainMaterialMap& materials,
    std::string& error)
{
    materials.clear();
    for (const auto& material : source)
    {
        if (material.shaderName.rfind("levels\\", 0u) != 0u ||
            std::any_of(material.detailTextures.begin(), material.detailTextures.end(),
                [](const std::string& identity) {
                    if (identity.empty() || identity == "$null" ||
                        identity.find("..") != std::string::npos)
                        return true;
                    std::string normalized = identity;
                    std::replace(normalized.begin(), normalized.end(), '\\', '/');
                    return std::filesystem::path(normalized).is_absolute();
                }) ||
            !materials.emplace(material.shaderName, material.detailTextures).second)
        {
            error = "terrain shader material list contains an unsafe or duplicate record";
            return false;
        }
    }
    if (materials.empty())
    {
        // Preserve the standalone l05_bar artifact proof. Engine runs always
        // replace this bounded fallback with archive-authored shaders.xr data.
        const std::array<std::string, 4> details = {
            "detail\\ground_grass_01", "detail\\ground_asphalt_01",
            "detail\\ground_earth_01", "detail\\ground_mud_01"};
        materials.emplace("levels\\bar_grass", details);
        materials.emplace("levels\\bar_asfalt", details);
    }
    return true;
}

std::string terrainDdsIdentity(const std::string& identity, const char* suffix = "")
{
    std::string result = identity;
    if (result.size() >= 4u)
    {
        std::string extension = result.substr(result.size() - 4u);
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        if (extension == ".dds")
            result.resize(result.size() - 4u);
    }
    result += suffix;
    result += ".dds";
    return result;
}

bool makeTerrainDraw(
    const LevelScene& scene,
    const xray::metal::R4StaticVisual& visual,
    const TerrainMaterialMap& materials,
    TerrainDraw& draw,
    std::string& error)
{
    if (!visual.valid || visual.vertexBufferId >= scene.geometry.vertexBuffers.size() ||
        visual.indexBufferId >= scene.geometry.indexBuffers.size())
    {
        error = "terrain visual is invalid or has an out-of-range source buffer";
        return false;
    }
    if (visual.shaderId >= scene.geometry.shaders.size())
    {
        error = "terrain visual does not resolve to a configured-level terrain record";
        return false;
    }
    const auto& shader = scene.geometry.shaders[visual.shaderId];
    const auto material = materials.find(shader.shaderName);
    if (material == materials.end() || shader.textures.size() < 2u ||
        shader.textures[0].empty() || shader.textures[1].empty())
    {
        error = "terrain visual does not match an archive-authored BmmD terrain record";
        return false;
    }
    const auto& vertexBuffer = scene.geometry.vertexBuffers[visual.vertexBufferId];
    const auto& indexBuffer = scene.geometry.indexBuffers[visual.indexBufferId];
    if (vertexBuffer.stride != 32u || vertexBuffer.positionOffset != 0u ||
        !vertexBuffer.hasPositionFloat3 || visual.vertexCount == 0u || visual.indexCount == 0u)
    {
        error = "terrain visual does not use the admitted packed static P3/C4/C4/C4/S2/S2 layout";
        return false;
    }
    const auto hasElement = [&vertexBuffer](std::uint16_t offset, std::uint8_t type,
        std::uint8_t usage, std::uint8_t usageIndex) {
        return std::any_of(vertexBuffer.declaration.begin(), vertexBuffer.declaration.end(),
            [offset, type, usage, usageIndex](const auto& element) {
                return element.stream == 0u && element.offset == offset &&
                    element.type == type && element.method == 0u &&
                    element.usage == usage && element.usageIndex == usageIndex;
            });
    };
    if (!hasElement(12u, 4u, 3u, 0u) || !hasElement(16u, 4u, 6u, 0u) ||
        !hasElement(20u, 4u, 7u, 0u) || !hasElement(24u, 6u, 5u, 0u))
    {
        error = "terrain visual declaration does not expose NORMAL/TANGENT/BINORMAL/TEXCOORD0";
        return false;
    }
    const std::size_t vertexOffset = vertexBuffer.dataOffset +
        static_cast<std::size_t>(visual.vertexBase) * vertexBuffer.stride;
    const std::size_t vertexBytes = static_cast<std::size_t>(visual.vertexCount) * vertexBuffer.stride;
    const std::size_t indexOffset = indexBuffer.dataOffset +
        static_cast<std::size_t>(visual.indexBase) * sizeof(std::uint16_t);
    const std::size_t indexBytes = static_cast<std::size_t>(visual.indexCount) * sizeof(std::uint16_t);
    if (vertexOffset > scene.geometryBytes.size() || vertexBytes > scene.geometryBytes.size() - vertexOffset ||
        indexOffset > scene.geometryBytes.size() || indexBytes > scene.geometryBytes.size() - indexOffset)
    {
        error = "terrain visual source range exceeds the retained level geometry bytes";
        return false;
    }
    draw = {};
    draw.visualId = visual.visualId;
    draw.sourceVertexBuffer = visual.vertexBufferId;
    draw.vertexCount = visual.vertexCount;
    draw.indexCount = visual.indexCount;
    draw.shaderName = shader.shaderName;
    const std::string& base = shader.textures[0];
    draw.textureIdentities = {
        terrainDdsIdentity(base), terrainDdsIdentity(base, "_mask"),
        terrainDdsIdentity(material->second[0]), terrainDdsIdentity(material->second[1]),
        terrainDdsIdentity(material->second[2]), terrainDdsIdentity(material->second[3]),
        terrainDdsIdentity(material->second[0], "_bump"),
        terrainDdsIdentity(material->second[1], "_bump"),
        terrainDdsIdentity(material->second[2], "_bump"),
        terrainDdsIdentity(material->second[3], "_bump"),
        "fx\\water_normal.dds", terrainDdsIdentity(base, "_puddles_mask"),
        "fx\\water_sbumpvolume.dds",
        terrainDdsIdentity(material->second[0], "_height"),
        terrainDdsIdentity(material->second[1], "_height"),
        terrainDdsIdentity(material->second[2], "_height"),
        terrainDdsIdentity(material->second[3], "_height")};
    draw.localPositionBounds = visual.referencedPositionBounds;
    draw.vertices.resize(visual.vertexCount);
    draw.indices.resize(visual.indexCount);
    const std::uint8_t* sourceVertices = scene.geometryBytes.data() + vertexOffset;
    for (std::size_t index = 0; index < draw.vertices.size(); ++index)
    {
        const std::uint8_t* source = sourceVertices + index * vertexBuffer.stride;
        TerrainVertex& target = draw.vertices[index];
        for (std::size_t component = 0; component < 4; ++component)
        {
            target.normal[component] = static_cast<float>(source[12u + component]) / 255.0f;
            target.tangent[component] = static_cast<float>(source[16u + component]) / 255.0f;
            target.binormal[component] = static_cast<float>(source[20u + component]) / 255.0f;
        }
        std::int16_t tc[2]{};
        std::memcpy(tc, source + 24u, sizeof(tc));
        target.texcoord[0] = tc[0];
        target.texcoord[1] = tc[1];
        std::memcpy(target.position, source, sizeof(float) * 3u);
        target.position[3] = 1.0f;
    }
    std::memcpy(draw.indices.data(), scene.geometryBytes.data() + indexOffset, indexBytes);
    for (const std::uint16_t index : draw.indices)
    {
        if (index >= draw.vertexCount)
        {
            error = "terrain visual indices are not local to its OGF_GCONTAINER vertex range";
            return false;
        }
    }
    if (visual.hasModelTransform)
        std::memcpy(draw.modelTransform.elements, visual.modelTransform.data(),
            sizeof(draw.modelTransform.elements));
    else
    {
        draw.modelTransform = {{
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
        }};
    }
    return true;
}

struct TerrainAutoSelection
{
    std::vector<std::uint32_t> visualIds;
    std::size_t requested = 0;
    std::size_t admitted = 0;
    std::size_t skipped = 0;
};

TerrainAutoSelection selectAllAdmittedTerrainVisuals(
    const LevelScene& level, const TerrainMaterialMap& materials)
{
    TerrainAutoSelection selection;
    std::unordered_set<std::uint32_t> unique;
    for (const xray::metal::R4StaticVisual& visual : level.geometry.visuals)
    {
        if (visual.shaderId >= level.geometry.shaders.size())
            continue;
        const std::string& shaderName = level.geometry.shaders[visual.shaderId].shaderName;
        if (materials.find(shaderName) == materials.end())
            continue;
        ++selection.requested;
        // Reuse the exact draw admission, including material spelling,
        // declaration, byte-range, local-index, and tree-transform checks.
        TerrainDraw proof;
        std::string error;
        if (!makeTerrainDraw(level, visual, materials, proof, error) ||
            !unique.emplace(visual.visualId).second)
        {
            ++selection.skipped;
            std::fprintf(stderr, "METAL_TERRAIN_AUTO_SKIP visual=%u reason=\"%s\"\n",
                visual.visualId, error.empty() ? "duplicate static visual" : error.c_str());
            continue;
        }
        selection.visualIds.push_back(visual.visualId);
        ++selection.admitted;
    }
    return selection;
}

bool prepareTerrainScene(
    IR4Backend& backend,
    const LevelScene& level,
    const std::filesystem::path& levelDirectory,
    const std::vector<std::uint32_t>& visualIds,
    const TerrainMaterialMap& materials,
    const std::filesystem::path& shaderDirectory,
    const xray::metal::R4DdsBytesProvider& ddsBytesProvider,
    TerrainScene& terrain,
    std::string& error)
{
    terrain = {};
    terrain.requested = !visualIds.empty();
    if (!terrain.requested)
        return true;
    if (!ddsBytesProvider)
    {
        error = "terrain HQ admission requires the engine XRFS DDS provider";
        return false;
    }
    std::filesystem::path vertexMetallib;
    std::filesystem::path fragmentMetallib;
    if (shaderDirectory.empty() || !validateTerrainMscReflection(
            shaderDirectory, vertexMetallib, fragmentMetallib, error))
        return false;

    terrain.shaderDirectory = shaderDirectory;
    terrain.draws.reserve(visualIds.size());
    std::unordered_set<std::uint32_t> admittedRoots;
    std::unordered_set<std::uint32_t> admittedLeaves;
    for (const std::uint32_t visualId : visualIds)
    {
        if (!admittedRoots.emplace(visualId).second)
        {
            error = "terrain visual selection contains a duplicate id";
            return false;
        }
        std::vector<std::uint32_t> leaves;
        if (!xray::metal::CollectR4StaticVisualDescendants(
                level.geometry, visualId, leaves, error))
            return false;
        for (const std::uint32_t leafId : leaves)
        {
            // Roots may overlap in a hierarchy; encode each static leaf once.
            if (!admittedLeaves.emplace(leafId).second)
                continue;
            const auto visual = std::find_if(level.geometry.visuals.begin(), level.geometry.visuals.end(),
                [leafId](const auto& candidate) { return candidate.visualId == leafId; });
            if (visual == level.geometry.visuals.end())
            {
                error = "terrain root resolved a static visual missing from level geometry";
                return false;
            }
            TerrainDraw draw;
            if (!makeTerrainDraw(level, *visual, materials, draw, error))
                return false;
            terrain.draws.emplace_back(std::move(draw));
        }
    }
    if (terrain.draws.empty())
    {
        error = "terrain selection resolved no drawable static leaves";
        return false;
    }

    IR4Resources& resources = backend.Resources();
    const std::string levelName = levelDirectory.filename().string();
    if (levelName.empty())
    {
        error = "terrain HQ admission requires a configured level directory";
        return false;
    }
    std::unordered_map<std::string, std::shared_ptr<IR4Texture>> textureCache;
    std::unordered_map<std::string, float> detailScaleCache;
    for (TerrainDraw& draw : terrain.draws)
    {
        const auto knownScale = detailScaleCache.find(draw.textureIdentities[0]);
        if (knownScale != detailScaleCache.end())
            draw.detailScale = knownScale->second;
        else
        {
            std::filesystem::path detailScalePath;
            std::string detailScaleError;
            if (!LoadMetalR4EngineTextureDetailScale(levelName,
                    draw.textureIdentities[0], draw.detailScale,
                    detailScalePath, detailScaleError))
            {
                draw.detailScale = 1.0f;
                std::fprintf(stderr,
                    "METAL_TERRAIN_DETAIL_SCALE shader=%s value=1.000000 "
                    "fallback=default reason=\"%s\"\n",
                    draw.shaderName.c_str(), detailScaleError.c_str());
            }
            else
            {
                std::fprintf(stderr,
                    "METAL_TERRAIN_DETAIL_SCALE shader=%s value=%.6f path=%s\n",
                    draw.shaderName.c_str(), draw.detailScale,
                    detailScalePath.generic_string().c_str());
            }
            detailScaleCache.emplace(draw.textureIdentities[0], draw.detailScale);
        }
        for (std::size_t index = 0u; index < draw.textureIdentities.size(); ++index)
        {
            const std::string& relativePath = draw.textureIdentities[index];
            const auto cached = textureCache.find(relativePath);
            if (cached != textureCache.end())
            {
                draw.textures[index] = cached->second;
                continue;
            }
            TerrainTextureAsset asset;
            asset.bindingName = TerrainTextureBindingNames[index];
            std::string levelError;
            if (!ResolveMetalR4EngineLevelTextureDdsPath(
                    levelName, relativePath, asset.path, levelError) &&
                !ResolveMetalR4EngineTextureDdsPath(relativePath, asset.path, error))
            {
                error = "terrain texture " + asset.bindingName + " for " +
                    draw.shaderName + " is unavailable: " + error;
                return false;
            }
            if (!xray::metal::LoadR4DdsTexture(
                    asset.path, ddsBytesProvider, asset.dds, error))
            {
                error = "terrain texture " + asset.bindingName + " for " +
                    draw.shaderName + " is unavailable: " + error;
                return false;
            }
            std::fprintf(stderr,
                "METAL_TERRAIN_TEXTURE_RESOLVED shader=%s binding=%s path=%s\n",
                draw.shaderName.c_str(), asset.bindingName.c_str(),
                asset.path.generic_string().c_str());
            asset.dds.descriptor.label = "R4 terrain HQ XRFS texture";
            asset.texture = resources.CreateTexture2D(asset.dds.descriptor, error);
            if (!asset.texture)
            {
                error = "terrain texture " + asset.bindingName + " creation failed: " + error;
                return false;
            }
            draw.textures[index] = asset.texture;
            textureCache.emplace(relativePath, asset.texture);
            terrain.textures.emplace_back(std::move(asset));
        }
    }

    R4SamplerDesc linearSampler;
    linearSampler.minFilter = R4SamplerFilter::Linear;
    linearSampler.magFilter = R4SamplerFilter::Linear;
    linearSampler.mipFilter = R4SamplerFilter::Linear;
    linearSampler.addressU = R4SamplerAddressMode::Repeat;
    linearSampler.addressV = R4SamplerAddressMode::Repeat;
    linearSampler.label = "R4 terrain HQ smp_linear";
    terrain.linearSampler = resources.CreateSampler(linearSampler, error);
    linearSampler.maxAnisotropy = 16;
    linearSampler.label = "R4 terrain HQ smp_base";
    terrain.baseSampler = resources.CreateSampler(linearSampler, error);
    if (!terrain.linearSampler || !terrain.baseSampler)
        return false;

    const std::string vertexPath = vertexMetallib.string();
    const std::string fragmentPath = fragmentMetallib.string();
    R4GraphicsPipelineDesc pipeline;
    pipeline.label = "R4 converted terrain HQ MSC G-buffer";
    pipeline.shaderKey = "r4-terrain-configured-level-hq-msc-v1";
    pipeline.vertexMetallibPath = vertexPath.c_str();
    pipeline.fragmentMetallibPath = fragmentPath.c_str();
    pipeline.vertexAttributes = {
        {11, 6, 0, R4VertexFormat::Float4},
        {12, 6, 16, R4VertexFormat::Float4},
        {13, 6, 32, R4VertexFormat::Float4},
        {14, 6, 48, R4VertexFormat::Int2},
        {15, 6, 56, R4VertexFormat::Float4},
    };
    pipeline.vertexLayouts = {{6, sizeof(TerrainVertex)}};
    pipeline.colorFormats = {
        R4PixelFormat::RGBA16Float,
        R4PixelFormat::RGBA8Unorm,
        R4PixelFormat::RGBA8Unorm,
        R4PixelFormat::RGBA16Float,
    };
    pipeline.depthFormat = R4PixelFormat::Depth32FloatStencil8;
    pipeline.depthCompare = R4CompareFunction::Less;
    pipeline.depthWrite = true;
    pipeline.cullMode = R4CullMode::Back;
    pipeline.frontFace = R4Winding::Clockwise;
    pipeline.stencil.enabled = true;
    pipeline.stencil.frontFace.compare = R4CompareFunction::Always;
    pipeline.stencil.frontFace.depthStencilPass = R4StencilOperation::Replace;
    pipeline.stencil.frontFace.readMask = 0xff;
    pipeline.stencil.frontFace.writeMask = 0x7f;
    pipeline.stencil.backFace = pipeline.stencil.frontFace;
    terrain.pipeline = backend.CreateGraphicsPipeline(pipeline, error);
    if (!terrain.pipeline)
        return false;

    const auto createBuffer = [&resources, &error](std::size_t size, R4BufferUsage usage,
        const char* label) {
        R4BufferDesc descriptor;
        descriptor.size = size;
        descriptor.storage = R4ResourceStorage::Private;
        descriptor.usage = usage;
        descriptor.label = label;
        return resources.CreateBuffer(descriptor, nullptr, 0, error);
    };
    const auto createConstantBuffer = [&resources, &error](std::size_t size, const char* label) {
        R4BufferDesc descriptor;
        descriptor.size = size;
        descriptor.storage = R4ResourceStorage::Shared;
        descriptor.usage = R4BufferUsage::Uniform;
        descriptor.label = label;
        return resources.CreateBuffer(descriptor, nullptr, 0, error);
    };
    for (TerrainDraw& draw : terrain.draws)
    {
        draw.vertexBuffer = createBuffer(draw.vertices.size() * sizeof(TerrainVertex),
            R4BufferUsage::Vertex, "R4 terrain HQ repacked vertices");
        draw.indexBuffer = createBuffer(draw.indices.size() * sizeof(std::uint16_t),
            R4BufferUsage::Index, "R4 terrain HQ copied indices");
        draw.vertexGlobals = createConstantBuffer(160u, "R4 terrain HQ VS globals b0");
        draw.vertexTransforms = createConstantBuffer(256u, "R4 terrain HQ VS transforms b1");
        draw.fragmentGlobals = createConstantBuffer(192u, "R4 terrain HQ PS globals b0");
        draw.fragmentStaticGlobals = createConstantBuffer(528u, "R4 terrain HQ PS globals b1");
        if (!draw.vertexBuffer || !draw.indexBuffer || !draw.vertexGlobals ||
            !draw.vertexTransforms || !draw.fragmentGlobals || !draw.fragmentStaticGlobals)
            return false;
    }
    terrain.ready = true;
    return true;
}

bool updateTerrainConstants(
    IR4Backend& backend,
    const LevelScene& scene,
    TerrainScene& terrain,
    std::string& error)
{
    if (!terrain.ready)
        return true;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    backend.DrawableSize(width, height);
    if (width == 0u || height == 0u)
    {
        error = "terrain HQ constants require a non-zero drawable";
        return false;
    }
    const auto putFloat4 = [](auto& bytes, std::size_t offset,
        float x, float y, float z, float w) {
        const float value[4] = {x, y, z, w};
        std::memcpy(bytes.data() + offset, value, sizeof(value));
    };
    const Matrix4 inverseView = inverseRigidTransform(scene.view);
    for (TerrainDraw& draw : terrain.draws)
    {
        std::array<std::uint8_t, 160> vertexGlobals{};
        std::array<std::uint8_t, 256> vertexTransforms{};
        std::array<std::uint8_t, 192> fragmentGlobals{};
        std::array<std::uint8_t, 528> fragmentStaticGlobals{};
        const Matrix4 worldViewProjection = multiply(
            scene.uniformData.viewProjection, draw.modelTransform);
        const Matrix4 worldView = multiply(scene.view, draw.modelTransform);
        std::memcpy(vertexGlobals.data(), &worldViewProjection, sizeof(Matrix4));
        std::memcpy(vertexGlobals.data() + 64u, &scene.uniformData.viewProjection,
            sizeof(Matrix4));
        std::memcpy(vertexTransforms.data(), &worldViewProjection, sizeof(Matrix4));
        std::memcpy(vertexTransforms.data() + 64u, &worldView, sizeof(Matrix4));
        std::memcpy(vertexTransforms.data() + 128u, &draw.modelTransform, sizeof(Matrix4));
        const float materialClass[4] = {0.0f, 0.0f, 0.0f, 0.25f};
        std::memcpy(vertexTransforms.data() + 192u, materialClass, sizeof(materialClass));

        std::memcpy(fragmentGlobals.data(), &worldViewProjection, sizeof(Matrix4));
        std::memcpy(fragmentGlobals.data() + 64u, &scene.uniformData.viewProjection,
            sizeof(Matrix4));
        putFloat4(fragmentGlobals, 160u, 12.0f, 20.0f, 0.04f, 1.0f);
        putFloat4(fragmentGlobals, 176u, -0.07f, -0.05f, -0.13f, 0.0f);

        std::memcpy(fragmentStaticGlobals.data(), &scene.view, sizeof(Matrix4));
        std::memcpy(fragmentStaticGlobals.data() + 64u, &inverseView, sizeof(Matrix4));
        std::memcpy(fragmentStaticGlobals.data() + 128u, &scene.projection, sizeof(Matrix4));
        std::memcpy(fragmentStaticGlobals.data() + 192u,
            &scene.uniformData.viewProjection, sizeof(Matrix4));
        putFloat4(fragmentStaticGlobals, 304u, 0.025f, 0.055f, 0.105f, 0.0f);
        putFloat4(fragmentStaticGlobals, 320u, 0.50f, 0.52f, 0.56f, 0.0f);
        putFloat4(fragmentStaticGlobals, 336u, 1.0f, 0.96f, 0.86f, 0.0f);
        putFloat4(fragmentStaticGlobals, 352u, -0.35f, -0.80f, 0.48f, 0.0f);
        putFloat4(fragmentStaticGlobals, 368u, 0.45f, 0.50f, 0.58f, 0.0f);
        putFloat4(fragmentStaticGlobals, 384u, scene.eye.x, scene.eye.y, scene.eye.z, 0.0f);
        const float verticalTan = -std::tan(55.0f * 3.14159265358979323846f / 360.0f);
        const float horizontalTan = -verticalTan * (16.0f / 9.0f);
        putFloat4(fragmentStaticGlobals, 400u, horizontalTan, verticalTan,
            (2.0f * horizontalTan) / static_cast<float>(width),
            (2.0f * verticalTan) / static_cast<float>(height));
        putFloat4(fragmentStaticGlobals, 416u, static_cast<float>(width),
            static_cast<float>(height), 1.0f / static_cast<float>(width),
            1.0f / static_cast<float>(height));
        putFloat4(fragmentStaticGlobals, 432u, -0.35f, -0.80f, 0.48f, 0.0f);
        // The HQ terrain VS consumes dynamic_transforms.dt_params at b1+240.
        // Match the native binder using the archive-winning base THM scale
        // for this material and the native default detail range (50).
        putFloat4(vertexTransforms, 240u, draw.detailScale, draw.detailScale,
            draw.detailScale, 1.0f / 50.0f);

        IR4Resources& resources = backend.Resources();
        if (!resources.UpdateBuffer(draw.vertexGlobals, 0, vertexGlobals.data(),
                vertexGlobals.size(), error) ||
            !resources.UpdateBuffer(draw.vertexTransforms, 0, vertexTransforms.data(),
                vertexTransforms.size(), error) ||
            !resources.UpdateBuffer(draw.fragmentGlobals, 0, fragmentGlobals.data(),
                fragmentGlobals.size(), error) ||
            !resources.UpdateBuffer(draw.fragmentStaticGlobals, 0, fragmentStaticGlobals.data(),
                fragmentStaticGlobals.size(), error))
            return false;
    }
    return true;
}

bool uploadTerrainScene(IR4Backend& backend, TerrainScene& terrain, std::string& error)
{
    if (!terrain.ready || terrain.uploaded)
        return true;
    IR4Resources& resources = backend.Resources();
    for (TerrainDraw& draw : terrain.draws)
    {
        if (!resources.UpdateBuffer(draw.vertexBuffer, 0, draw.vertices.data(),
                draw.vertices.size() * sizeof(TerrainVertex), error) ||
            !resources.UpdateBuffer(draw.indexBuffer, 0, draw.indices.data(),
                draw.indices.size() * sizeof(std::uint16_t), error))
            return false;
    }
    for (TerrainTextureAsset& asset : terrain.textures)
    {
        for (std::size_t level = 0; level < asset.dds.mipLevels.size(); ++level)
        {
            const xray::metal::R4DdsMipLevel& mip = asset.dds.mipLevels[level];
            if (!resources.UploadTexture2D(asset.texture, static_cast<std::uint32_t>(level), 0,
                    asset.dds.bytes.data() + mip.dataOffset, mip.bytesPerImage,
                    mip.bytesPerRow, mip.bytesPerImage, error))
                return false;
            asset.uploadedBytes += mip.bytesPerImage;
            terrain.uploadedBytes += mip.bytesPerImage;
        }
        asset.dds.bytes.clear();
        asset.dds.bytes.shrink_to_fit();
    }
    for (TerrainDraw& draw : terrain.draws)
    {
        draw.vertices.clear();
        draw.vertices.shrink_to_fit();
        draw.indices.clear();
        draw.indices.shrink_to_fit();
    }
    terrain.uploaded = true;
    std::fprintf(stderr, "METAL_TERRAIN_UPLOAD_READY draws=%zu textures=%zu bytes=%llu\n",
        terrain.draws.size(), terrain.textures.size(),
        static_cast<unsigned long long>(terrain.uploadedBytes));
    return true;
}

bool drawTerrainScene(IR4Backend& backend, const LevelScene& scene, TerrainScene& terrain, std::string& error)
{
    if (!terrain.ready || !terrain.uploaded)
        return true;
    R4MscStageBindings vertexBindings;
    vertexBindings.stage = R4ShaderStage::Vertex;
    vertexBindings.tlabSize = 48u;
    R4MscStageBindings fragmentBindings;
    fragmentBindings.stage = R4ShaderStage::Fragment;
    fragmentBindings.tlabSize = 504u;
    if (!backend.BindGraphicsPipeline(terrain.pipeline, error))
        return false;
    std::size_t submitted = 0;
    std::size_t culled = 0;
    for (const TerrainDraw& draw : terrain.draws)
    {
        if (conservativelyOutsideMetalFrustum(draw.localPositionBounds, draw.modelTransform,
                scene.uniformData.viewProjection, scene.liveViewProjectionValid))
        {
            ++culled;
            continue;
        }
        vertexBindings.resources = {
            {R4MscResourceType::ConstantBuffer, 0u,
                draw.vertexGlobals, 0, 160u},
            {R4MscResourceType::ConstantBuffer, 24u,
                draw.vertexTransforms, 0, 256u},
        };
        std::vector<R4MscResourceBinding> resources;
        resources.reserve(21u);
        for (std::size_t index = 0; index < draw.textures.size(); ++index)
        {
            R4MscResourceBinding texture;
            texture.type = R4MscResourceType::Texture;
            texture.tlabOffset = index * 24u;
            texture.texture = draw.textures[index];
            resources.emplace_back(std::move(texture));
        }
        resources.push_back({R4MscResourceType::ConstantBuffer, 408u,
            draw.fragmentGlobals, 0, 192u});
        resources.push_back({R4MscResourceType::ConstantBuffer, 432u,
            draw.fragmentStaticGlobals, 0, 528u});
        resources.push_back({R4MscResourceType::Sampler, 456u,
            nullptr, 0, 0, nullptr, terrain.linearSampler});
        resources.push_back({R4MscResourceType::Sampler, 480u,
            nullptr, 0, 0, nullptr, terrain.baseSampler});
        fragmentBindings.resources = std::move(resources);
        if (!backend.BindVertexBuffer(6u, draw.vertexBuffer, 0, error) ||
            !backend.BindMscResources(vertexBindings, error) ||
            !backend.BindMscResources(fragmentBindings, error))
            return false;
        R4IndexedDrawDesc indexed;
        indexed.indexBuffer = draw.indexBuffer;
        indexed.indexCount = draw.indexCount;
        indexed.indexType = R4IndexType::UInt16;
        if (!backend.DrawIndexed(indexed, error))
            return false;
        ++submitted;
    }
    ++terrain.renderCount;
    terrain.lastSubmitted = submitted;
    terrain.lastCulled = culled;
    const bool firstLiveReport = scene.liveViewProjectionValid && !terrain.liveFrustumReported;
    const bool firstGameplayReport = scene.gameplayViewValid && !terrain.gameplayFrustumReported;
    if (terrain.renderCount == 1u || firstLiveReport || firstGameplayReport ||
        terrain.renderCount % 300u == 0u)
    {
        std::fprintf(stderr,
            "METAL_TERRAIN_DRAW frame=%llu submitted=%zu culled=%zu live_view=%s textures=%zu shader=terrain_hq_msc "
            "fail_open=nonfinite_or_uncertain\n",
            static_cast<unsigned long long>(terrain.renderCount), terrain.lastSubmitted,
            terrain.lastCulled, scene.liveViewProjectionValid ? "true" : "false",
            terrain.textures.size());
        if (firstLiveReport)
            terrain.liveFrustumReported = true;
        if (firstGameplayReport)
            terrain.gameplayFrustumReported = true;
    }
    return true;
}

bool resolveSkyTexture(
    IR4Backend& backend,
    SkyScene& sky,
    const std::string& identity,
    std::shared_ptr<IR4Texture>& texture,
    std::string& error)
{
    texture.reset();
    if (identity.empty())
    {
        error = "sky2 environment snapshot has an empty cube texture identity";
        return false;
    }
    const auto existing = sky.textures.find(identity);
    if (existing != sky.textures.end())
    {
        texture = existing->second.texture;
        return static_cast<bool>(texture);
    }
    constexpr std::size_t MaximumSkyTextureCache = 8u;
    if (sky.textures.size() >= MaximumSkyTextureCache)
    {
        const auto unused = std::find_if(sky.textures.begin(), sky.textures.end(),
            [](const auto& entry) { return entry.second.texture.use_count() == 1u; });
        if (unused == sky.textures.end())
        {
            error = "sky2 cube texture cache capacity reached with all entries in flight";
            return false;
        }
        sky.textures.erase(unused);
    }
    std::filesystem::path requested(identity);
    if (requested.extension().empty())
        requested += ".dds";
    std::filesystem::path resolved;
    if (!ResolveMetalR4EngineTextureDdsPath(requested, resolved, error))
    {
        error = "sky2 engine texture resolution failed for " + identity + ": " + error;
        return false;
    }
    SkyTextureAsset candidate;
    candidate.resolvedPath = resolved;
    if (!xray::metal::LoadR4DdsTexture(resolved, sky.ddsBytesProvider, candidate.dds, error))
    {
        error = "sky2 DDS load failed for " + resolved.generic_string() + ": " + error;
        return false;
    }
    if (candidate.dds.descriptor.type != R4TextureType::TextureCube ||
        candidate.dds.descriptor.arrayLength != 1u)
    {
        error = "sky2 texture is not a single six-face DDS cubemap: " +
            resolved.generic_string();
        return false;
    }
    candidate.dds.descriptor.label = "R4 sky2 environment cubemap";
    candidate.texture = backend.Resources().CreateTexture2D(candidate.dds.descriptor, error);
    if (!candidate.texture)
    {
        error = "sky2 Metal cubemap allocation failed: " + error;
        return false;
    }
    for (const xray::metal::R4DdsMipLevel& subresource : candidate.dds.mipLevels)
    {
        if (subresource.dataOffset > candidate.dds.bytes.size() ||
            subresource.bytesPerImage > candidate.dds.bytes.size() - subresource.dataOffset ||
            !backend.Resources().UploadTexture2D(candidate.texture, subresource.mipLevel,
                subresource.arraySlice, candidate.dds.bytes.data() + subresource.dataOffset,
                subresource.bytesPerImage, subresource.bytesPerRow,
                subresource.bytesPerImage, error))
        {
            error = "sky2 Metal cubemap upload failed: " + error;
            return false;
        }
    }
    candidate.dds.bytes.clear();
    candidate.dds.bytes.shrink_to_fit();
    texture = candidate.texture;
    std::fprintf(stderr, "METAL_SKY_TEXTURE_READY identity=%s path=%s mips=%u faces=6\n",
        identity.c_str(), resolved.generic_string().c_str(), candidate.dds.descriptor.mipLevels);
    sky.textures.emplace(identity, std::move(candidate));
    return true;
}

bool prepareSkyResources(
    IR4Backend& backend,
    SkyScene& sky,
    const R4EnvironmentSnapshot& environment,
    std::shared_ptr<IR4Texture>& sky0,
    std::shared_ptr<IR4Texture>& sky1,
    std::string& error)
{
    sky0.reset();
    sky1.reset();
    if (!environment.valid)
    {
        error = "sky2 environment snapshot is unavailable";
        return false;
    }
    // Private texture uploads encode a blit pass. Do all resolves and uploads
    // before opening the sky render encoder; Metal command buffers cannot
    // begin a blit encoder while this pass owns an active render encoder.
    if (!resolveSkyTexture(backend, sky, environment.sky0TextureName, sky0, error) ||
        !resolveSkyTexture(backend, sky, environment.sky1TextureName, sky1, error))
        return false;
    if (!sky.tonemapUploaded)
    {
        const float tonemap = 1.0f;
        if (!backend.Resources().UploadTexture2D(
                sky.tonemap, 0u, 0u, &tonemap, sizeof(tonemap), sizeof(tonemap),
                sizeof(tonemap), error))
            return false;
        sky.tonemapUploaded = true;
    }
    return true;
}

bool resolveCloudTexture(IR4Backend& backend, SkyScene& sky, const std::string& identity,
    std::shared_ptr<IR4Texture>& texture, std::string& error)
{
    texture.reset();
    if (identity.empty())
    {
        error = "cloud snapshot has an empty 2D texture identity";
        return false;
    }
    if (const auto found = sky.cloudTextures.find(identity); found != sky.cloudTextures.end())
    {
        texture = found->second.texture;
        return static_cast<bool>(texture);
    }
    std::filesystem::path requested(identity);
    if (requested.extension().empty())
        requested += ".dds";
    std::filesystem::path resolved;
    if (!ResolveMetalR4EngineTextureDdsPath(requested, resolved, error))
    {
        error = "cloud texture resolution failed for " + identity + ": " + error;
        return false;
    }
    SkyTextureAsset candidate;
    candidate.resolvedPath = resolved;
    if (!xray::metal::LoadR4DdsTexture(resolved, sky.ddsBytesProvider, candidate.dds, error) ||
        candidate.dds.descriptor.type != R4TextureType::Texture2D ||
        candidate.dds.descriptor.arrayLength != 1u)
    {
        if (error.empty()) error = "cloud texture is not a single 2D DDS";
        return false;
    }
    candidate.dds.descriptor.label = "R4 clouds texture";
    candidate.texture = backend.Resources().CreateTexture2D(candidate.dds.descriptor, error);
    if (!candidate.texture)
        return false;
    for (const xray::metal::R4DdsMipLevel& mip : candidate.dds.mipLevels)
    {
        if (mip.arraySlice != 0u || mip.dataOffset > candidate.dds.bytes.size() ||
            mip.bytesPerImage > candidate.dds.bytes.size() - mip.dataOffset ||
            !backend.Resources().UploadTexture2D(candidate.texture, mip.mipLevel, 0u,
                candidate.dds.bytes.data() + mip.dataOffset, mip.bytesPerImage,
                mip.bytesPerRow, mip.bytesPerImage, error))
        {
            error = "cloud texture upload failed: " + error;
            return false;
        }
    }
    candidate.dds.bytes.clear();
    candidate.dds.bytes.shrink_to_fit();
    texture = candidate.texture;
    sky.cloudTextures.emplace(identity, std::move(candidate));
    return true;
}

bool prepareCloudResources(IR4Backend& backend, SkyScene& sky,
    const R4EnvironmentSnapshot& environment, std::shared_ptr<IR4Texture>& cloud0,
    std::shared_ptr<IR4Texture>& cloud1, std::string& error)
{
    cloud0.reset();
    cloud1.reset();
    if (!sky.cloudsPipeline || !environment.cloudsValid ||
        environment.cloudsVertices.empty() || environment.cloudsIndices.empty() ||
        environment.cloudsIndices.size() % 3u != 0u || environment.cloudsColor[3] <= 0.0f)
        return true;
    if (!resolveCloudTexture(backend, sky, environment.clouds0TextureName, cloud0, error) ||
        !resolveCloudTexture(backend, sky, environment.clouds1TextureName, cloud1, error))
        return false;
    if (environment.cloudsVertices.size() > std::numeric_limits<std::uint16_t>::max())
    {
        error = "cloud hemisphere exceeds the uint16 R4 index ABI";
        return false;
    }
    for (const std::uint16_t index : environment.cloudsIndices)
    {
        if (index >= environment.cloudsVertices.size())
        {
            error = "cloud hemisphere index exceeds its captured vertices";
            return false;
        }
    }
    IR4Resources& resources = backend.Resources();
    if (sky.cloudsVertexCapacity != environment.cloudsVertices.size() || !sky.cloudsVertices)
    {
        R4BufferDesc descriptor;
        descriptor.size = environment.cloudsVertices.size() * sizeof(CloudVertex);
        descriptor.storage = R4ResourceStorage::Shared;
        descriptor.usage = R4BufferUsage::Vertex;
        descriptor.label = "R4 clouds FVF source vertices";
        sky.cloudsVertices = resources.CreateBuffer(descriptor, nullptr, 0u, error);
        sky.cloudsVertexCapacity = sky.cloudsVertices ? environment.cloudsVertices.size() : 0u;
    }
    if (sky.cloudsIndexCapacity != environment.cloudsIndices.size() || !sky.cloudsIndices)
    {
        R4BufferDesc descriptor;
        descriptor.size = environment.cloudsIndices.size() * sizeof(std::uint16_t);
        descriptor.storage = R4ResourceStorage::Shared;
        descriptor.usage = R4BufferUsage::Index;
        descriptor.label = "R4 clouds R4 source indices";
        sky.cloudsIndices = resources.CreateBuffer(descriptor, nullptr, 0u, error);
        sky.cloudsIndexCapacity = sky.cloudsIndices ? environment.cloudsIndices.size() : 0u;
    }
    return static_cast<bool>(sky.cloudsVertices) && static_cast<bool>(sky.cloudsIndices);
}

bool prepareCombineEnvironmentResources(
    IR4Backend& backend,
    SkyScene& sky,
    LevelScene& scene,
    const R4EnvironmentSnapshot& environment,
    std::string& error)
{
    scene.lightingEnvironment0.reset();
    scene.lightingEnvironment1.reset();
    if (!environment.valid || environment.environment0TextureName.empty() ||
        environment.environment1TextureName.empty())
    {
        error = "combine_1 environment snapshot has no authored cubemap identities";
        return false;
    }
    if (!resolveSkyTexture(backend, sky, environment.environment0TextureName,
            scene.lightingEnvironment0, error) ||
        !resolveSkyTexture(backend, sky, environment.environment1TextureName,
            scene.lightingEnvironment1, error))
        return false;
    const std::string signature = environment.environment0TextureName + "|" +
        environment.environment1TextureName + "|" + environment.sky0TextureName + "|" +
        environment.sky1TextureName;
    if (signature != sky.lastCombineEnvironmentSignature)
    {
        std::fprintf(stderr,
            "METAL_COMBINE_ENV_READY frame=%u env=%s,%s sky=%s,%s blend=%.3f "
            "ambient=%.4f,%.4f,%.4f environment=%.4f,%.4f,%.4f\n",
            environment.engineFrame, environment.environment0TextureName.c_str(),
            environment.environment1TextureName.c_str(), environment.sky0TextureName.c_str(),
            environment.sky1TextureName.c_str(), environment.skyBlend,
            environment.combineAmbient[0], environment.combineAmbient[1],
            environment.combineAmbient[2], environment.combineEnvironment[0],
            environment.combineEnvironment[1], environment.combineEnvironment[2]);
        sky.lastCombineEnvironmentSignature = signature;
    }
    return true;
}

bool drawSkyScene(
    IR4Backend& backend,
    SkyScene& sky,
    const LevelScene& scene,
    const MetalR4EngineScene::ViewState& view,
    const R4EnvironmentSnapshot& environment,
    const R4GameplayVisionSnapshot& vision,
    const std::shared_ptr<IR4Texture>& sky0,
    const std::shared_ptr<IR4Texture>& sky1,
    std::string& error)
{
    if (!sky.ready)
        return true;
    if (!environment.valid || !view.valid)
    {
        if (!sky.unavailableLogged || environment.engineFrame - sky.lastLoggedFrame >= 300u)
        {
            std::fprintf(stderr, "METAL_SKY_SKIP reason=%s frame=%u\n",
                environment.valid ? "camera_unavailable" : "environment_snapshot_unavailable",
                environment.engineFrame);
            sky.unavailableLogged = true;
            sky.lastLoggedFrame = environment.engineFrame;
        }
        return true;
    }
    const auto byte = [](float value) {
        const float scaled = std::floor(value * 255.0f);
        return static_cast<std::uint32_t>(std::clamp(scaled, 0.0f, 255.0f));
    };
    const std::uint32_t color = (byte(environment.skyBlend) << 24u) |
        (byte(environment.skyColor[0]) << 16u) |
        (byte(environment.skyColor[1]) << 8u) |
        byte(environment.skyColor[2]);
    static constexpr float Positions[12][3] = {
        {-1.f, -1.f, -1.f}, { 1.f, -1.f, -1.f}, {-1.f, -1.f,  1.f}, { 1.f, -1.f,  1.f},
        {-1.f,  1.f, -1.f}, { 1.f,  1.f, -1.f}, {-1.f,  1.f,  1.f}, { 1.f,  1.f,  1.f},
        {-1.f,  0.f, -1.f}, { 1.f,  0.f, -1.f}, { 1.f,  0.f,  1.f}, {-1.f,  0.f,  1.f},
    };
    static constexpr float Directions[12][3] = {
        {-1.f, -1.01f, -1.f}, { 1.f, -1.01f, -1.f}, {-1.f, -1.01f,  1.f}, { 1.f, -1.01f,  1.f},
        {-1.f,  1.f, -1.f}, { 1.f,  1.f, -1.f}, {-1.f,  1.f,  1.f}, { 1.f,  1.f,  1.f},
        {-1.f, -1.f, -1.f}, { 1.f, -1.f, -1.f}, { 1.f, -1.f,  1.f}, {-1.f, -1.f,  1.f},
    };
    std::array<SkyVertex, 12> vertices{};
    for (std::size_t index = 0; index < vertices.size(); ++index)
    {
        std::memcpy(vertices[index].position, Positions[index], sizeof(Positions[index]));
        vertices[index].color = color;
        std::memcpy(vertices[index].texcoord0, Directions[index], sizeof(Directions[index]));
        std::memcpy(vertices[index].texcoord1, Directions[index], sizeof(Directions[index]));
    }
    const float cosine = std::cos(environment.skyRotation);
    const float sine = std::sin(environment.skyRotation);
    const Matrix4 skyWorld{{
        cosine, 0.0f, -sine, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        sine, 0.0f, cosine, 0.0f,
        view.eye[0], view.eye[1], view.eye[2], 1.0f,
    }};
    const Matrix4 viewProjection = multiply(scene.projection, scene.view);
    const Matrix4 worldViewProjection = multiply(viewProjection, skyWorld);
    const Matrix4 inverseView = inverseRigidTransform(scene.view);
    std::array<std::uint8_t, 256> vertexTransforms{};
    std::array<std::uint8_t, 192> fragmentGlobals{};
    std::array<std::uint8_t, 528> fragmentStaticGlobals{};
    auto putFloat4 = [](auto& bytes, std::size_t offset, float x, float y, float z, float w) {
        const float value[4] = {x, y, z, w};
        std::memcpy(bytes.data() + offset, value, sizeof(value));
    };
    std::memcpy(vertexTransforms.data(), &worldViewProjection, sizeof(Matrix4));
    std::memcpy(fragmentGlobals.data() + 160u,
        vision.shaderParam8.data(), sizeof(float) * vision.shaderParam8.size());
    std::memcpy(fragmentGlobals.data() + 176u,
        vision.shaderParam7.data(), sizeof(float) * vision.shaderParam7.size());
    std::memcpy(fragmentStaticGlobals.data(), &scene.view, sizeof(Matrix4));
    std::memcpy(fragmentStaticGlobals.data() + 64u, &inverseView, sizeof(Matrix4));
    std::memcpy(fragmentStaticGlobals.data() + 128u, &scene.projection, sizeof(Matrix4));
    std::memcpy(fragmentStaticGlobals.data() + 192u, &viewProjection, sizeof(Matrix4));
    putFloat4(fragmentStaticGlobals, 304u, environment.fogColor[0], environment.fogColor[1],
        environment.fogColor[2], 0.0f);
    putFloat4(fragmentStaticGlobals, 320u, environment.ambient[0], environment.ambient[1],
        environment.ambient[2], environment.skyBlend);
    putFloat4(fragmentStaticGlobals, 336u, environment.sunColor[0], environment.sunColor[1],
        environment.sunColor[2], 0.0f);
    putFloat4(fragmentStaticGlobals, 352u, environment.sunDirection[0], environment.sunDirection[1],
        environment.sunDirection[2], 0.0f);
    putFloat4(fragmentStaticGlobals, 368u, environment.hemiColor[0], environment.hemiColor[1],
        environment.hemiColor[2], environment.hemiColor[3]);
    putFloat4(fragmentStaticGlobals, 384u, view.eye[0], view.eye[1], view.eye[2], 0.0f);
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;
    backend.DrawableSize(width, height);
    if (width == 0u || height == 0u)
    {
        error = "sky2 cannot bind static globals for a zero-sized drawable";
        return false;
    }
    putFloat4(fragmentStaticGlobals, 416u, static_cast<float>(width), static_cast<float>(height),
        1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height));
    if (!backend.Resources().UpdateBuffer(sky.vertices, 0u, vertices.data(), sizeof(vertices), error) ||
        !backend.Resources().UpdateBuffer(sky.vertexTransforms, 0u, vertexTransforms.data(),
            vertexTransforms.size(), error) ||
        !backend.Resources().UpdateBuffer(sky.fragmentGlobals, 0u, fragmentGlobals.data(),
            fragmentGlobals.size(), error) ||
        !backend.Resources().UpdateBuffer(sky.fragmentStaticGlobals, 0u, fragmentStaticGlobals.data(),
            fragmentStaticGlobals.size(), error))
        return false;

    R4MscStageBindings vertexBindings;
    vertexBindings.stage = R4ShaderStage::Vertex;
    vertexBindings.tlabSize = 48u;
    vertexBindings.resources = {
        {R4MscResourceType::Texture, 0u, nullptr, 0u, 0u, sky.tonemap, nullptr},
        {R4MscResourceType::ConstantBuffer, 24u, sky.vertexTransforms, 0u, 256u, nullptr, nullptr},
    };
    R4MscStageBindings fragmentBindings;
    fragmentBindings.stage = R4ShaderStage::Fragment;
    fragmentBindings.tlabSize = 120u;
    fragmentBindings.resources = {
        {R4MscResourceType::Texture, 0u, nullptr, 0u, 0u, sky0, nullptr},
        {R4MscResourceType::Texture, 24u, nullptr, 0u, 0u, sky1, nullptr},
        {R4MscResourceType::ConstantBuffer, 48u, sky.fragmentGlobals, 0u, 192u, nullptr, nullptr},
        {R4MscResourceType::ConstantBuffer, 72u, sky.fragmentStaticGlobals, 0u, 528u, nullptr, nullptr},
        {R4MscResourceType::Sampler, 96u, nullptr, 0u, 0u, nullptr, sky.sampler},
    };
    if (!backend.BindGraphicsPipeline(sky.pipeline, error) ||
        !backend.SetStencilReference(0u, error) ||
        !backend.BindVertexBuffer(6u, sky.vertices, 0u, error) ||
        !backend.BindMscResources(vertexBindings, error) ||
        !backend.BindMscResources(fragmentBindings, error))
        return false;
    R4IndexedDrawDesc draw;
    draw.indexBuffer = sky.indices;
    draw.indexCount = 60u;
    draw.indexType = R4IndexType::UInt16;
    if (!backend.DrawIndexed(draw, error))
        return false;
    ++sky.drawCount;
    if (sky.drawCount == 1u || sky.drawCount % 300u == 0u)
    {
        std::fprintf(stderr,
            "METAL_SKY_DRAW frame=%u shader=sky2_msc textures=%s,%s blend=%.3f rotation=%.3f "
            "stencil=background_zero targets=2\n",
            environment.engineFrame, environment.sky0TextureName.c_str(),
            environment.sky1TextureName.c_str(), environment.skyBlend, environment.skyRotation);
    }
    sky.unavailableLogged = false;
    return true;
}

bool drawCloudScene(IR4Backend& backend, const LevelScene& scene, SkyScene& sky,
    const MetalR4EngineScene::ViewState& view, const R4EnvironmentSnapshot& environment,
    const std::shared_ptr<IR4Texture>& cloud0, const std::shared_ptr<IR4Texture>& cloud1,
    std::string& error)
{
    if (!sky.cloudsPipeline || !environment.cloudsValid || !cloud0 || !cloud1 ||
        !sky.cloudsVertices || !sky.cloudsIndices || !scene.lightingTonemap ||
        !scene.baseLightingStaticGlobals || !view.valid || environment.cloudsColor[3] <= 0.0f)
        return true;
    const auto byte = [](const float value) {
        return static_cast<std::uint32_t>(std::clamp(std::floor(value * 255.0f), 0.0f, 255.0f));
    };
    const auto rgba = [](const std::uint32_t red, const std::uint32_t green,
        const std::uint32_t blue, const std::uint32_t alpha) {
        return (alpha << 24u) | (red << 16u) | (green << 8u) | blue;
    };
    constexpr float Pi = 3.14159265358979323846f;
    const float h0 = Pi * 0.25f;
    const float h1 = Pi * 0.375f;
    const std::array<float, 4> wind = {{
        (-std::sin(h0) * 0.5f + 0.5f), (std::cos(h0) * 0.5f + 0.5f),
        (-std::sin(h1) * 0.5f + 0.5f), (std::cos(h1) * 0.5f + 0.5f),
    }};
    const std::uint32_t windDirections = rgba(byte(wind[0]), byte(wind[1]),
        byte(wind[3]), byte(wind[2]));
    const std::uint32_t color = rgba(byte(environment.cloudsColor[0]),
        byte(environment.cloudsColor[1]), byte(environment.cloudsColor[2]),
        byte(environment.cloudsColor[3]));
    std::vector<CloudVertex> vertices(environment.cloudsVertices.size());
    for (std::size_t index = 0; index < vertices.size(); ++index)
    {
        std::memcpy(vertices[index].position, environment.cloudsVertices[index].data(),
            sizeof(vertices[index].position));
        vertices[index].windDirections = windDirections;
        vertices[index].color = color;
    }
    const float cosine = std::cos(environment.skyRotation);
    const float sine = std::sin(environment.skyRotation);
    const Matrix4 cloudWorld{{
        cosine * 10.0f, 0.0f, -sine * 10.0f, 0.0f,
        0.0f, 0.4f, 0.0f, 0.0f,
        sine * 10.0f, 0.0f, cosine * 10.0f, 0.0f,
        view.eye[0], view.eye[1], view.eye[2], 1.0f,
    }};
    const Matrix4 worldViewProjection = multiply(multiply(scene.projection, scene.view), cloudWorld);
    std::array<std::uint8_t, 256> transforms{};
    std::memcpy(transforms.data(), &worldViewProjection, sizeof(worldViewProjection));
    if (!backend.Resources().UpdateBuffer(sky.cloudsVertices, 0u, vertices.data(),
            vertices.size() * sizeof(CloudVertex), error) ||
        !backend.Resources().UpdateBuffer(sky.cloudsIndices, 0u, environment.cloudsIndices.data(),
            environment.cloudsIndices.size() * sizeof(std::uint16_t), error) ||
        !backend.Resources().UpdateBuffer(sky.cloudsTransforms, 0u, transforms.data(),
            transforms.size(), error))
        return false;
    R4MscStageBindings vs;
    vs.stage = R4ShaderStage::Vertex;
    vs.tlabSize = 72u;
    vs.resources = {
        {R4MscResourceType::Texture, 0u, nullptr, 0u, 0u, scene.lightingTonemap, nullptr},
        {R4MscResourceType::ConstantBuffer, 24u, sky.cloudsTransforms, 0u, 256u, nullptr, nullptr},
        {R4MscResourceType::ConstantBuffer, 48u, scene.baseLightingStaticGlobals, 0u, 528u, nullptr, nullptr},
    };
    R4MscStageBindings ps;
    ps.stage = R4ShaderStage::Fragment;
    ps.tlabSize = 96u;
    ps.resources = {
        {R4MscResourceType::Texture, 0u, nullptr, 0u, 0u, scene.lightingTonemap, nullptr},
        {R4MscResourceType::Texture, 24u, nullptr, 0u, 0u, cloud0, nullptr},
        {R4MscResourceType::Texture, 48u, nullptr, 0u, 0u, cloud1, nullptr},
        {R4MscResourceType::Sampler, 72u, nullptr, 0u, 0u, nullptr, sky.cloudsSampler},
    };
    if (!backend.BindGraphicsPipeline(sky.cloudsPipeline, error) ||
        !backend.BindVertexBuffer(6u, sky.cloudsVertices, 0u, error) ||
        !backend.BindMscResources(vs, error) || !backend.BindMscResources(ps, error))
        return false;
    R4IndexedDrawDesc draw;
    draw.indexBuffer = sky.cloudsIndices;
    draw.indexCount = static_cast<std::uint32_t>(environment.cloudsIndices.size());
    draw.indexType = R4IndexType::UInt16;
    if (!backend.DrawIndexed(draw, error))
        return false;
    ++sky.cloudsDrawCount;
    if (sky.lastCloudLoggedFrame == UINT32_MAX ||
        environment.engineFrame - sky.lastCloudLoggedFrame >= 300u)
    {
        std::fprintf(stderr,
            "METAL_CLOUD_FRAME frame=%u draws=1 vertices=%zu indices=%zu textures=%s,%s "
            "blend=srcalpha_invsrcalpha scale=10,0.4,10 rotation=%.3f\n",
            environment.engineFrame, environment.cloudsVertices.size(), environment.cloudsIndices.size(),
            environment.clouds0TextureName.c_str(), environment.clouds1TextureName.c_str(),
            environment.skyRotation);
        sky.lastCloudLoggedFrame = environment.engineFrame;
    }
    return true;
}

bool updateSceneView(
    IR4Backend& backend,
    LevelScene& scene,
    const MetalR4EngineScene::ViewState& state,
    const R4EnvironmentSnapshot& environment,
    std::string& error)
{
    scene.liveViewProjectionValid = state.valid;
    scene.gameplayViewValid = state.valid && environment.valid;
    if (state.valid)
    {
        std::memcpy(scene.view.elements, state.view.data(), sizeof(scene.view.elements));
        std::memcpy(scene.projection.elements, state.projection.data(), sizeof(scene.projection.elements));
        scene.eye = {state.eye[0], state.eye[1], state.eye[2]};
        scene.uniformData.viewProjection = multiply(scene.projection, scene.view);
    }

    IR4Resources& resources = backend.Resources();
    if (!scene.productionMsc)
        return resources.UpdateBuffer(
            scene.uniforms, 0, &scene.uniformData, sizeof(scene.uniformData), error);

    for (LevelScene::TextureMaterial& material : scene.textureMaterials)
    {
        std::array<std::uint8_t, 160> vertexGlobals{};
        std::array<std::uint8_t, 256> transforms{};
        const Matrix4 worldViewProjection = multiply(
            scene.uniformData.viewProjection, material.modelTransform);
        const Matrix4 worldView = multiply(scene.view, material.modelTransform);
        std::memcpy(vertexGlobals.data(), &worldViewProjection, sizeof(Matrix4));
        std::memcpy(vertexGlobals.data() + 64,
            &scene.uniformData.viewProjection, sizeof(Matrix4));
        std::memcpy(transforms.data(), &worldViewProjection, sizeof(Matrix4));
        std::memcpy(transforms.data() + 64, &worldView, sizeof(Matrix4));
        std::memcpy(transforms.data() + 128, &material.modelTransform, sizeof(Matrix4));
        const float materialClass[4] = {0.0f, 0.0f, 0.0f, 0.25f};
        std::memcpy(transforms.data() + 192, materialClass, sizeof(materialClass));
        if (!resources.UpdateBuffer(material.mscVertexGlobals, 0,
                vertexGlobals.data(), vertexGlobals.size(), error) ||
            !resources.UpdateBuffer(material.mscVertexTransforms, 0,
                transforms.data(), transforms.size(), error) ||
            !resources.UpdateBuffer(material.mscFragmentTransforms, 0,
                transforms.data(), transforms.size(), error))
            return false;
    }
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    backend.DrawableSize(width, height);
    if (width == 0 || height == 0)
    {
        error = "engine scene cannot update lighting constants for a zero-sized drawable";
        return false;
    }
    return updateBaseLightingConstants(backend, scene, width, height, &environment, error);
}

bool prepareLocalShadowScene(IR4Backend& backend, const LevelScene& scene,
    LocalShadowScene& shadows, std::string& error)
{
    shadows = {};
    if (!scene.sunShadowPipeline || !scene.sunShadowSampler || scene.textureMaterials.empty())
    {
        error = "local shadow atlas requires the converted opaque caster pipeline";
        return false;
    }
    R4Texture2DDesc atlasDesc;
    atlasDesc.width = LocalShadowAtlasExtent;
    atlasDesc.height = LocalShadowAtlasExtent;
    atlasDesc.format = R4PixelFormat::R32Float;
    atlasDesc.storage = R4ResourceStorage::Private;
    atlasDesc.usage = R4TextureUsage::RenderTarget | R4TextureUsage::ShaderRead;
    atlasDesc.label = "R4 local-light R32 shadow atlas";
    shadows.atlas = backend.Resources().CreateTexture2D(atlasDesc, error);
    R4Texture2DDesc depthDesc = atlasDesc;
    depthDesc.format = R4PixelFormat::Depth32Float;
    depthDesc.usage = R4TextureUsage::RenderTarget;
    depthDesc.label = "R4 local-light shadow atlas depth";
    shadows.depth = backend.Resources().CreateTexture2D(depthDesc, error);
    const std::size_t transformCount = LocalShadowTileCapacity * scene.textureMaterials.size();
    if (transformCount > std::numeric_limits<std::size_t>::max() / 256u)
    {
        error = "local shadow transform arena size overflow";
        return false;
    }
    R4BufferDesc transformDesc;
    transformDesc.size = transformCount * 256u;
    transformDesc.storage = R4ResourceStorage::Shared;
    transformDesc.usage = R4BufferUsage::Uniform;
    transformDesc.label = "R4 local-light shadow caster transforms";
    shadows.transforms = backend.Resources().CreateBuffer(transformDesc, nullptr, 0u, error);
    shadows.ready = shadows.atlas && shadows.depth && shadows.transforms;
    if (!shadows.ready && error.empty())
        error = "local shadow atlas resource allocation failed";
    return shadows.ready;
}

bool renderLocalShadowAtlas(IR4Backend& backend, LevelScene& scene,
    LocalShadowScene& shadows, const MetalR4EngineScene::ViewState& view,
    const std::shared_ptr<const R4EngineLocalLightSnapshot>& snapshot,
    MetalR4DynamicWorldAdapter* dynamicWorld,
    std::string& error)
{
    shadows.records.clear();
    shadows.candidates = shadows.renderedLights = shadows.submittedCasters =
        shadows.culledCasters = shadows.omittedDynamicCasters = 0u;
    shadows.dynamicCandidates = shadows.submittedDynamicCasters =
        shadows.culledDynamicCasters = shadows.rejectedDynamicCasters = 0u;
    if (!shadows.ready || !view.valid || !snapshot ||
        snapshot->engineFrame == std::numeric_limits<std::uint32_t>::max())
        return true;
    struct Candidate { std::size_t lightIndex; float score; };
    std::vector<Candidate> candidates;
    Matrix4 cameraView{};
    Matrix4 cameraProjection{};
    std::memcpy(cameraView.elements, view.view.data(), sizeof(cameraView));
    std::memcpy(cameraProjection.elements, view.projection.data(), sizeof(cameraProjection));
    const Matrix4 viewProjection = multiply(cameraProjection, cameraView);
    xray::metal::R4PositionBounds spotBounds;
    spotBounds.available = true;
    spotBounds.minimum = {-0.5f, -0.5f, 0.0f};
    spotBounds.maximum = {0.5f, 0.5f, 1.0f};
	xray::metal::R4PositionBounds omniBounds;
	omniBounds.available = true;
	omniBounds.minimum = {-1.0f, -1.0f, -1.0f};
	omniBounds.maximum = {1.0f, 1.0f, 1.0f};
    for (std::size_t index = 0; index < snapshot->lights.size(); ++index)
    {
        const R4EngineLocalLight& light = snapshot->lights[index];
        const float directionLength2 = light.direction[0]*light.direction[0] +
            light.direction[1]*light.direction[1] + light.direction[2]*light.direction[2];
        if (!light.active || !light.shadowed || light.type == R4EngineLocalLightType::Point ||
            !std::isfinite(light.range) || !std::isfinite(light.cone) ||
            !std::isfinite(light.virtualSize) || light.range <= light.virtualSize ||
            light.virtualSize <= 0.0f || light.cone <= 0.0f || light.cone >= 3.0805f ||
            !std::isfinite(directionLength2) || directionLength2 <= 1.0e-8f)
            continue;
        const Matrix4 volume = makeSpotVolumeModel(light);
		const xray::metal::R4PositionBounds& volumeBounds =
			light.type == R4EngineLocalLightType::OmniFace ? omniBounds : spotBounds;
        if (!finiteMatrix(volume) || conservativelyOutsideMetalFrustum(
                volumeBounds, volume, viewProjection, true))
            continue;
        const float dx = light.position[0] - view.eye[0];
        const float dy = light.position[1] - view.eye[1];
        const float dz = light.position[2] - view.eye[2];
        candidates.push_back({index, dx*dx + dy*dy + dz*dz});
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
        return left.score < right.score;
    });
    shadows.candidates = candidates.size();
    if (candidates.size() > LocalShadowTileCapacity)
        candidates.resize(LocalShadowTileCapacity);
    if (candidates.empty())
        return true;

    const Matrix4 inverseView = inverseRigidTransform(cameraView);
    std::vector<std::uint8_t> transformBytes(
        LocalShadowTileCapacity * scene.textureMaterials.size() * 256u, 0u);
    std::vector<Matrix4> lightViewProjections;
    lightViewProjections.reserve(candidates.size());
    shadows.records.reserve(candidates.size());
    for (std::size_t tile = 0; tile < candidates.size(); ++tile)
    {
        const R4EngineLocalLight& light = snapshot->lights[candidates[tile].lightIndex];
        const Matrix4 lightView = makeR4SpotLightView(light);
        const Matrix4 lightProjection = makeR4SpotLightProjection(light);
        const Matrix4 lightViewProjection = multiply(lightProjection, lightView);
        if (!finiteMatrix(lightView) || !finiteMatrix(lightProjection) ||
            !finiteMatrix(lightViewProjection))
        {
            error = "local shadow light transform is non-finite";
            return false;
        }
        xray::metal::R4LocalShadowAtlasRecord record;
		record.lightIdentity = light.lightId;
        record.engineFrame = snapshot->engineFrame;
        record.smapId = 0u;
		record.face = light.type == R4EngineLocalLightType::OmniFace ? light.omniFace : 0u;
        record.atlasX = static_cast<std::uint32_t>(tile % 4u) * LocalShadowTileSize;
        record.atlasY = static_cast<std::uint32_t>(tile / 4u) * LocalShadowTileSize;
        record.atlasSize = LocalShadowTileSize;
        record.atlasExtent = LocalShadowAtlasExtent;
        std::memcpy(record.lightView.data(), lightView.elements, sizeof(lightView));
        std::memcpy(record.lightProjection.data(), lightProjection.elements, sizeof(lightProjection));
        std::memcpy(record.deviceInverseView.data(), inverseView.elements, sizeof(inverseView));
        if (!xray::metal::DeriveR4LocalShadowMatrix(record, snapshot->shadowDepthScale,
                snapshot->shadowDepthBias, record.shadowMatrix, error) ||
            !xray::metal::DeriveR4LocalLightMapMatrix(record, snapshot->shadowDepthScale,
                snapshot->shadowDepthBias, record.lightMapMatrix, error))
            return false;
        for (std::size_t materialIndex = 0; materialIndex < scene.textureMaterials.size(); ++materialIndex)
        {
            const Matrix4 wvp = multiply(lightViewProjection,
                scene.textureMaterials[materialIndex].modelTransform);
            const std::size_t offset = (tile * scene.textureMaterials.size() + materialIndex) * 256u;
            std::memcpy(transformBytes.data() + offset, wvp.elements, sizeof(wvp));
        }
        lightViewProjections.push_back(lightViewProjection);
        shadows.records.push_back(std::move(record));
    }
    if (shadows.records.empty())
        return true;
    if (!backend.Resources().UpdateBuffer(shadows.transforms, 0u,
            transformBytes.data(), transformBytes.size(), error))
        return false;

    for (std::size_t tile = 0; tile < shadows.records.size(); ++tile)
    {
        xray::metal::R4LocalShadowAtlasRecord& record = shadows.records[tile];
        R4RenderPassDesc pass;
        pass.label = "R4 converted local-light static shadow atlas tile";
        pass.useDrawable = false;
        R4ColorAttachmentDesc color;
        color.texture = shadows.atlas;
        color.loadAction = tile == 0u ? R4LoadAction::Clear : R4LoadAction::Load;
        color.storeAction = R4StoreAction::Store;
        color.clearColor = {1.0, 1.0, 1.0, 1.0};
        pass.colorAttachments = {color};
        pass.depthStencilAttachment.texture = shadows.depth;
        // Each tile reuses the same full-size depth attachment. Clearing it for
        // every light is intentional: previous tile color stays in the R32 atlas,
        // while stale depth must not reject this light's independent casters.
        pass.depthStencilAttachment.depthLoadAction = R4LoadAction::Clear;
        pass.depthStencilAttachment.depthStoreAction = R4StoreAction::Store;
        pass.hasViewport = true;
        pass.viewport = {double(record.atlasX), double(record.atlasY),
            double(record.atlasSize), double(record.atlasSize), 0.0, 1.0};
        pass.hasScissor = true;
        pass.scissor = {record.atlasX, record.atlasY, record.atlasSize, record.atlasSize};
        if (!backend.BeginRenderPass(pass, error))
            return false;
        std::size_t lastVertexBuffer = std::numeric_limits<std::size_t>::max();
        for (std::size_t materialIndex = 0; materialIndex < scene.textureMaterials.size(); ++materialIndex)
        {
            const LevelScene::TextureMaterial& material = scene.textureMaterials[materialIndex];
            const bool alphaTested = material.shaderName == "def_shaders\\def_aref";
            const std::shared_ptr<IR4GraphicsPipeline>& pipeline = alphaTested ?
                scene.sunShadowArefPipeline : scene.sunShadowPipeline;
            if (!pipeline)
            {
                error = "alpha-tested R4 local-shadow caster pipeline is unavailable";
                return false;
            }
            if (conservativelyOutsideMetalFrustum(material.localPositionBounds,
                    material.modelTransform, lightViewProjections[tile], true))
            { ++shadows.culledCasters; continue; }
            const std::size_t offset = (tile * scene.textureMaterials.size() + materialIndex) * 256u;
            R4MscStageBindings vertexBindings;
            vertexBindings.stage = R4ShaderStage::Vertex;
            vertexBindings.tlabSize = 24u;
            vertexBindings.resources = {{R4MscResourceType::ConstantBuffer, 0u,
                shadows.transforms, offset, 256u, nullptr, nullptr}};
            if (!backend.BindGraphicsPipeline(pipeline, error) ||
                !backend.BindMscResources(vertexBindings, error))
                return false;
            if (alphaTested)
            {
                if (material.textureAssetIndex >= scene.textureAssets.size() ||
                    !scene.textureAssets[material.textureAssetIndex].texture ||
                    !scene.lightingLinearSampler)
                {
                    error = "alpha-tested R4 local-shadow caster base texture or sampler is unavailable";
                    return false;
                }
                R4MscStageBindings fragmentBindings;
                fragmentBindings.stage = R4ShaderStage::Fragment;
                fragmentBindings.tlabSize = 48u;
                fragmentBindings.resources = {
                    {R4MscResourceType::Texture, 0u, nullptr, 0u, 0u,
                        scene.textureAssets[material.textureAssetIndex].texture, nullptr},
                    {R4MscResourceType::Sampler, 24u, nullptr, 0u, 0u,
                        nullptr, scene.lightingLinearSampler},
                };
                if (!backend.BindMscResources(fragmentBindings, error))
                    return false;
            }
            if (lastVertexBuffer != material.vertexBufferId)
            {
                if (!backend.BindVertexBuffer(6u,
                        scene.vertexBuffers[material.vertexBufferId], 0u, error))
                    return false;
                lastVertexBuffer = material.vertexBufferId;
            }
            R4IndexedDrawDesc draw;
            draw.indexBuffer = scene.indexBuffers[material.indexBufferId];
            draw.indexBufferOffset = std::size_t(material.indexBase) * sizeof(std::uint16_t);
            draw.indexCount = material.indexCount;
            draw.indexType = R4IndexType::UInt16;
            draw.baseVertex = static_cast<std::int32_t>(material.vertexBase);
            if (!backend.DrawIndexed(draw, error))
                return false;
            ++shadows.submittedCasters;
        }
        if (dynamicWorld && dynamicWorld->IsEnabled())
        {
            std::array<float, 16> tileLvp{};
            std::memcpy(tileLvp.data(), lightViewProjections[tile].elements, sizeof(tileLvp));
            if (!dynamicWorld->DrawLocalShadowTile(backend, tileLvp,
                    backend.EngineWorldSnapshot(),
                    shadows.dynamicCandidates,
                    shadows.submittedDynamicCasters,
                    shadows.culledDynamicCasters,
                    shadows.rejectedDynamicCasters,
                    error))
                return false;
        }
        if (!backend.EndRenderPass(error))
            return false;
        record.renderedDepth = true;
        ++shadows.renderedLights;
    }
    if (const std::shared_ptr<const R4EngineWorldSnapshot> world = backend.EngineWorldSnapshot())
    {
        if (dynamicWorld && dynamicWorld->IsEnabled())
            shadows.omittedDynamicCasters = 0u;
        else
            shadows.omittedDynamicCasters = world->renderables.size();
    }
    return true;
}

bool prepareNightVisionScene(IR4Backend& backend,
    const std::filesystem::path& shaderDirectory,
    NightVisionScene& nightVision,
    std::string& error)
{
    if (shaderDirectory.empty())
        return true;
    if (!xray::metal::ValidateR4NightVisionMscBundle(shaderDirectory, error))
        return false;

    std::string receipt;
    const std::filesystem::path receiptPath =
        shaderDirectory / "nightvision-msc-receipt.txt";
    if (!readTextFile(receiptPath, receipt) ||
        !receiptContains(receipt, "evidence_scope",
            "exact_r4_global_nightvision_gen1_gen2_gen3_and_blur_pyramid_dxc_dxil_msc_artifacts") ||
        !receiptContains(receipt, "blur_contract",
            "pp_blur.ps:resources=5,tlab=SRV:t0@0,t1@24;CBV:b0@48,b1@72;Sampler:s0@96,size=120,rt=1,globals=304:shader_param_8@256,shader_param_7@272,blur_params@288"))
    {
        error = "night vision MSC receipt is unavailable or has the wrong production scope";
        return false;
    }
    struct ReceiptArtifact
    {
        const char* source;
        const char* metallib;
        const char* reflection;
    };
    static constexpr ReceiptArtifact ReceiptArtifacts[] = {
        {"stub_screen_space.vs", "stub_screen_space.vs.metallib",
            "stub_screen_space.vs.reflection.json"},
        {"pp_blur.ps", "pp_blur.ps.metallib", "pp_blur.ps.reflection.json"},
        {"nightvision_gen_1.ps", "nightvision_gen_1.ps.metallib",
            "nightvision_gen_1.ps.reflection.json"},
        {"nightvision_gen_2.ps", "nightvision_gen_2.ps.metallib",
            "nightvision_gen_2.ps.reflection.json"},
        {"nightvision_gen_3.ps", "nightvision_gen_3.ps.metallib",
            "nightvision_gen_3.ps.reflection.json"},
    };
    for (const ReceiptArtifact& artifact : ReceiptArtifacts)
    {
        std::string metallibHash;
        std::string reflectionHash;
        if (!fileSha256(shaderDirectory / artifact.metallib, metallibHash) ||
            !fileSha256(shaderDirectory / artifact.reflection, reflectionHash))
        {
            error = std::string("night vision MSC artifact cannot be hashed: ") +
                artifact.source;
            return false;
        }
        const std::string sourceMarker = std::string("\tsource=") + artifact.source + "\t";
        const std::size_t sourcePosition = receipt.find(sourceMarker);
        const std::size_t lineStart = sourcePosition == std::string::npos ?
            std::string::npos : receipt.rfind('\n', sourcePosition);
        const std::size_t lineEnd = sourcePosition == std::string::npos ?
            std::string::npos : receipt.find('\n', sourcePosition);
        const std::string line = sourcePosition == std::string::npos ? std::string{} :
            receipt.substr(lineStart == std::string::npos ? 0u : lineStart + 1u,
                lineEnd == std::string::npos ? std::string::npos :
                    lineEnd - (lineStart == std::string::npos ? 0u : lineStart + 1u));
        if (line.find("\tmetallib_sha256=" + metallibHash) == std::string::npos ||
            line.find("\treflection_sha256=" + reflectionHash) == std::string::npos)
        {
            error = std::string("night vision MSC artifact does not match receipt: ") +
                artifact.source;
            return false;
        }
    }

    static constexpr const char* PipelineKeys[3] = {
        "r4-nightvision-gen1-msc-v1", "r4-nightvision-gen2-msc-v1",
        "r4-nightvision-gen3-msc-v1"};
    for (std::size_t index = 0; index < nightVision.pipelines.size(); ++index)
    {
        const auto generation = static_cast<xray::metal::R4NightVisionGeneration>(index);
        const auto contract = xray::metal::GetR4NightVisionPipelineContract(generation);
        const std::string vertexPath =
            (shaderDirectory / contract.vertexMetallib).generic_string();
        const std::string fragmentPath =
            (shaderDirectory / contract.fragmentMetallib).generic_string();
        R4GraphicsPipelineDesc pipeline;
        pipeline.label = "R4 converted global night vision";
        pipeline.shaderKey = PipelineKeys[index];
        pipeline.vertexMetallibPath = vertexPath.c_str();
        pipeline.fragmentMetallibPath = fragmentPath.c_str();
        pipeline.vertexAttributes = contract.vertexAttributes;
        pipeline.vertexLayouts = contract.vertexLayouts;
        pipeline.colorFormat = R4PixelFormat::RGBA8Unorm;
        pipeline.blend = contract.state.blend;
        pipeline.depthCompare = R4CompareFunction::Always;
        pipeline.depthWrite = false;
        pipeline.cullMode = contract.state.cullMode;
        nightVision.pipelines[index] = backend.CreateGraphicsPipeline(pipeline, error);
        if (!nightVision.pipelines[index])
            return false;
    }
    {
        const auto contract = xray::metal::GetR4NightVisionBlurPipelineContract();
        const std::string vertexPath =
            (shaderDirectory / contract.vertexMetallib).generic_string();
        const std::string fragmentPath =
            (shaderDirectory / contract.fragmentMetallib).generic_string();
        R4GraphicsPipelineDesc pipeline;
        pipeline.label = "R4 converted night vision blur pyramid";
        pipeline.shaderKey = "r4-nightvision-pp-blur-msc-v1";
        pipeline.vertexMetallibPath = vertexPath.c_str();
        pipeline.fragmentMetallibPath = fragmentPath.c_str();
        pipeline.vertexAttributes = contract.vertexAttributes;
        pipeline.vertexLayouts = contract.vertexLayouts;
        pipeline.colorFormat = R4PixelFormat::RGBA8Unorm;
        pipeline.cullMode = contract.state.cullMode;
        nightVision.blurPipeline = backend.CreateGraphicsPipeline(pipeline, error);
        if (!nightVision.blurPipeline)
            return false;
    }

    IR4Resources& resources = backend.Resources();
    const std::uint16_t indices[6] = {0u, 1u, 2u, 2u, 1u, 3u};
    R4BufferDesc indexDesc;
    indexDesc.size = sizeof(indices);
    indexDesc.storage = R4ResourceStorage::Shared;
    indexDesc.usage = R4BufferUsage::Index;
    indexDesc.label = "R4 night vision fullscreen indices";
    nightVision.indices = resources.CreateBuffer(indexDesc, indices, sizeof(indices), error);
    if (!nightVision.indices)
        return false;

    const auto createBuffer = [&resources, &error](std::size_t size,
                                  R4BufferUsage usage, const char* label) {
        R4BufferDesc descriptor;
        descriptor.size = size;
        descriptor.storage = R4ResourceStorage::Shared;
        descriptor.usage = usage;
        descriptor.label = label;
        return resources.CreateBuffer(descriptor, nullptr, 0u, error);
    };
    const std::uint32_t frameContexts = std::max(1u, backend.FrameContextCount());
    for (std::uint32_t index = 0; index < frameContexts; ++index)
    {
        nightVision.vertices.emplace_back(createBuffer(
            sizeof(xray::metal::R4NightVisionFullscreenVertex) * 4u,
            R4BufferUsage::Vertex, "R4 night vision fullscreen vertices"));
        nightVision.globals.emplace_back(createBuffer(
            192u, R4BufferUsage::Uniform, "R4 night vision globals b0"));
        nightVision.staticGlobals.emplace_back(createBuffer(
            528u, R4BufferUsage::Uniform, "R4 night vision static globals b1"));
        std::array<std::shared_ptr<IR4Buffer>, 6> blurPassGlobals;
        for (std::shared_ptr<IR4Buffer>& buffer : blurPassGlobals)
            buffer = createBuffer(304u, R4BufferUsage::Uniform,
                "R4 night vision pp_blur globals b0");
        nightVision.blurGlobals.emplace_back(std::move(blurPassGlobals));
        if (!nightVision.vertices.back() || !nightVision.globals.back() ||
            !nightVision.staticGlobals.back() ||
            std::any_of(nightVision.blurGlobals.back().begin(),
                nightVision.blurGlobals.back().end(),
                [](const auto& buffer) { return !buffer; }))
            return false;
    }
    R4SamplerDesc sampler;
    sampler.addressU = R4SamplerAddressMode::ClampToEdge;
    sampler.addressV = R4SamplerAddressMode::ClampToEdge;
    sampler.addressW = R4SamplerAddressMode::ClampToEdge;
    sampler.label = "R4 night vision smp_rtlinear";
    nightVision.sampler = resources.CreateSampler(sampler, error);
    if (!nightVision.sampler)
        return false;
    nightVision.shaderDirectory = shaderDirectory;
    nightVision.ready = true;
    return true;
}

bool ensureNightVisionSnapshot(IR4Backend& backend, NightVisionScene& nightVision,
    std::uint32_t width, std::uint32_t height, std::string& error)
{
    const bool snapshotReady = nightVision.sceneSnapshot &&
        nightVision.sceneSnapshot->Descriptor().width == width &&
        nightVision.sceneSnapshot->Descriptor().height == height;
    bool exactBlurSizes = true;
    for (std::size_t index = 0; exactBlurSizes && index < 3u; ++index)
    {
        const std::uint32_t divisor = 2u << index;
        const std::uint32_t expectedWidth = std::max(1u, width / divisor);
        const std::uint32_t expectedHeight = std::max(1u, height / divisor);
        exactBlurSizes = nightVision.blurHorizontal[index] && nightVision.blurFinal[index] &&
            nightVision.blurHorizontal[index]->Descriptor().width == expectedWidth &&
            nightVision.blurHorizontal[index]->Descriptor().height == expectedHeight &&
            nightVision.blurFinal[index]->Descriptor().width == expectedWidth &&
            nightVision.blurFinal[index]->Descriptor().height == expectedHeight;
    }
    if (snapshotReady && exactBlurSizes)
        return true;
    R4Texture2DDesc descriptor;
    descriptor.width = width;
    descriptor.height = height;
    descriptor.format = R4PixelFormat::RGBA8Unorm;
    descriptor.storage = R4ResourceStorage::Private;
    descriptor.usage = R4TextureUsage::ShaderRead;
    descriptor.label = "R4 night vision pre-effect scene snapshot";
    nightVision.sceneSnapshot = backend.Resources().CreateTexture2D(descriptor, error);
    if (!nightVision.sceneSnapshot)
        return false;
    descriptor.usage = R4TextureUsage::RenderTarget | R4TextureUsage::ShaderRead;
    for (std::size_t index = 0; index < 3u; ++index)
    {
        const std::uint32_t divisor = 2u << index;
        descriptor.width = std::max(1u, width / divisor);
        descriptor.height = std::max(1u, height / divisor);
        descriptor.label = "R4 night vision horizontal blur target";
        nightVision.blurHorizontal[index] =
            backend.Resources().CreateTexture2D(descriptor, error);
        descriptor.label = "R4 night vision final blur target";
        nightVision.blurFinal[index] = backend.Resources().CreateTexture2D(descriptor, error);
        if (!nightVision.blurHorizontal[index] || !nightVision.blurFinal[index])
            return false;
    }
    return true;
}

bool drawNightVision(IR4Backend& backend, LevelScene& scene,
    NightVisionScene& nightVision, const R4EnvironmentSnapshot& environment,
    const R4GameplayVisionSnapshot& vision, std::string& error)
{
    if (!vision.nightVisionActive)
        return true;
    if (!nightVision.ready || scene.gBufferTargets.empty() ||
        scene.lightingTargets.empty() || !scene.lightingTargets[0])
    {
        error = "gameplay requested night vision but the production MSC pass is unavailable";
        return false;
    }
    const std::int32_t generation = std::clamp(vision.nightVisionPhase, 1, 3);
    const R4Texture2DDesc& target = scene.lightingTargets[0]->Descriptor();
    if (!ensureNightVisionSnapshot(backend, nightVision, target.width, target.height, error) ||
        !backend.CopyTexture2D(scene.lightingTargets[0], nightVision.sceneSnapshot, error))
        return false;

    const std::uint32_t frameSlot = backend.ActiveFrameContextIndex();
    if (frameSlot >= nightVision.vertices.size() || frameSlot >= nightVision.globals.size() ||
        frameSlot >= nightVision.staticGlobals.size() ||
        frameSlot >= nightVision.blurGlobals.size())
    {
        error = "night vision active frame context exceeds prepared resources";
        return false;
    }
    constexpr float Depth = 1.0e-5f;
    using Vertex = xray::metal::R4NightVisionFullscreenVertex;
    const std::array<Vertex, 4> vertices = {{
        {{0.0f, 1.0f}, {0.0f, static_cast<float>(target.height), Depth, 1.0f}},
        {{0.0f, 0.0f}, {0.0f, 0.0f, Depth, 1.0f}},
        {{1.0f, 1.0f}, {static_cast<float>(target.width), static_cast<float>(target.height), Depth, 1.0f}},
        {{1.0f, 0.0f}, {static_cast<float>(target.width), 0.0f, Depth, 1.0f}},
    }};
    std::array<std::uint8_t, 192> globals{};
    std::array<std::uint8_t, 528> staticGlobals{};
    const auto copyFinite4 = [](std::uint8_t* destination,
                                const std::array<float, 4>& source) {
        float values[4]{};
        for (std::size_t index = 0; index < 4; ++index)
            values[index] = std::isfinite(source[index]) ? source[index] : 0.0f;
        std::memcpy(destination, values, sizeof(values));
    };
    copyFinite4(globals.data() + 160u, vision.shaderParam8);
    copyFinite4(globals.data() + 176u, vision.shaderParam7);
    const float time = environment.valid && std::isfinite(environment.globalTime) ?
        environment.globalTime : 0.0f;
    const float timers[4] = {time, time * 10.0f, time / 10.0f, std::sin(time)};
    const float screenRes[4] = {static_cast<float>(target.width),
        static_cast<float>(target.height), 1.0f / static_cast<float>(target.width),
        1.0f / static_cast<float>(target.height)};
    std::memcpy(staticGlobals.data() + 256u, timers, sizeof(timers));
    std::memcpy(staticGlobals.data() + 416u, screenRes, sizeof(screenRes));
    IR4Resources& resources = backend.Resources();
    if (!resources.UpdateBuffer(nightVision.vertices[frameSlot], 0u,
            vertices.data(), sizeof(vertices), error) ||
        !resources.UpdateBuffer(nightVision.globals[frameSlot], 0u,
            globals.data(), globals.size(), error) ||
        !resources.UpdateBuffer(nightVision.staticGlobals[frameSlot], 0u,
            staticGlobals.data(), staticGlobals.size(), error))
        return false;
    for (std::size_t level = 0; level < 3u; ++level)
    {
        const R4Texture2DDesc& blurTarget = nightVision.blurFinal[level]->Descriptor();
        for (std::size_t pass = 0; pass < 2u; ++pass)
        {
            std::array<std::uint8_t, 304> blurGlobals{};
            copyFinite4(blurGlobals.data() + 256u, vision.shaderParam8);
            copyFinite4(blurGlobals.data() + 272u, vision.shaderParam7);
            const float divisor = static_cast<float>(2u << level);
            const float blurParams[4] = {pass == 0u ? 1.0f : 0.0f,
                pass == 0u ? 0.0f : 1.0f,
                static_cast<float>(target.width) / divisor,
                static_cast<float>(target.height) / divisor};
            std::memcpy(blurGlobals.data() + 288u, blurParams, sizeof(blurParams));
            if (!resources.UpdateBuffer(nightVision.blurGlobals[frameSlot][level * 2u + pass],
                    0u, blurGlobals.data(), blurGlobals.size(), error))
                return false;
        }
    }

    const auto drawBlurPass = [&](const std::size_t level, const std::size_t passIndex,
                                  const std::shared_ptr<IR4Texture>& source,
                                  const std::shared_ptr<IR4Texture>& destination) {
        R4RenderPassDesc blurPass;
        blurPass.label = passIndex == 0u ?
            "R4 converted night vision horizontal blur" :
            "R4 converted night vision vertical blur";
        blurPass.useDrawable = false;
        R4ColorAttachmentDesc blurColor;
        blurColor.texture = destination;
        blurColor.loadAction = R4LoadAction::DontCare;
        blurColor.storeAction = R4StoreAction::Store;
        blurPass.colorAttachments = {blurColor};
        const R4Texture2DDesc& descriptor = destination->Descriptor();
        blurPass.hasViewport = true;
        blurPass.viewport = {0.0, 0.0, double(descriptor.width),
            double(descriptor.height), 0.0, 1.0};
        R4MscStageBindings blurVertexBindings;
        blurVertexBindings.stage = R4ShaderStage::Vertex;
        blurVertexBindings.tlabSize = 24u;
        blurVertexBindings.resources = {{R4MscResourceType::ConstantBuffer, 0u,
            nightVision.staticGlobals[frameSlot], 0u, 528u, nullptr, nullptr}};
        R4MscStageBindings blurFragmentBindings;
        blurFragmentBindings.stage = R4ShaderStage::Fragment;
        blurFragmentBindings.tlabSize = 120u;
        blurFragmentBindings.resources = {
            {R4MscResourceType::Texture, 0u, nullptr, 0u, 0u, scene.gBufferTargets[0], nullptr},
            {R4MscResourceType::Texture, 24u, nullptr, 0u, 0u, source, nullptr},
            {R4MscResourceType::ConstantBuffer, 48u,
                nightVision.blurGlobals[frameSlot][level * 2u + passIndex],
                0u, 304u, nullptr, nullptr},
            {R4MscResourceType::ConstantBuffer, 72u,
                nightVision.staticGlobals[frameSlot], 0u, 528u, nullptr, nullptr},
            {R4MscResourceType::Sampler, 96u, nullptr, 0u, 0u, nullptr, nightVision.sampler},
        };
        R4IndexedDrawDesc blurDraw;
        blurDraw.indexBuffer = nightVision.indices;
        blurDraw.indexCount = 6u;
        blurDraw.indexType = R4IndexType::UInt16;
        return backend.BeginRenderPass(blurPass, error) &&
            backend.BindGraphicsPipeline(nightVision.blurPipeline, error) &&
            backend.BindVertexBuffer(6u, nightVision.vertices[frameSlot], 0u, error) &&
            backend.BindMscResources(blurVertexBindings, error) &&
            backend.BindMscResources(blurFragmentBindings, error) &&
            backend.DrawIndexed(blurDraw, error) && backend.EndRenderPass(error);
    };
    for (std::size_t level = 0; level < 3u; ++level)
    {
        if (!drawBlurPass(level, 0u, nightVision.sceneSnapshot,
                nightVision.blurHorizontal[level]) ||
            !drawBlurPass(level, 1u, nightVision.blurHorizontal[level],
                nightVision.blurFinal[level]))
            return false;
    }

    R4RenderPassDesc pass;
    pass.label = "R4 converted global night vision";
    pass.useDrawable = false;
    R4ColorAttachmentDesc color;
    color.texture = scene.lightingTargets[0];
    color.loadAction = R4LoadAction::DontCare;
    color.storeAction = R4StoreAction::Store;
    pass.colorAttachments = {color};
    pass.hasViewport = true;
    pass.viewport = {0.0, 0.0, double(target.width), double(target.height), 0.0, 1.0};

    R4MscStageBindings vertexBindings;
    vertexBindings.stage = R4ShaderStage::Vertex;
    vertexBindings.tlabSize = 24u;
    vertexBindings.resources = {{R4MscResourceType::ConstantBuffer, 0u,
        nightVision.staticGlobals[frameSlot], 0u, 528u, nullptr, nullptr}};
    R4MscStageBindings fragmentBindings;
    fragmentBindings.stage = R4ShaderStage::Fragment;
    fragmentBindings.tlabSize = 192u;
    fragmentBindings.resources = {
        {R4MscResourceType::Texture, 0u, nullptr, 0u, 0u, scene.gBufferTargets[0], nullptr},
        {R4MscResourceType::Texture, 24u, nullptr, 0u, 0u, nightVision.sceneSnapshot, nullptr},
        {R4MscResourceType::Texture, 48u, nullptr, 0u, 0u, nightVision.blurFinal[0], nullptr},
        {R4MscResourceType::Texture, 72u, nullptr, 0u, 0u, nightVision.blurFinal[1], nullptr},
        {R4MscResourceType::Texture, 96u, nullptr, 0u, 0u, nightVision.blurFinal[2], nullptr},
        {R4MscResourceType::ConstantBuffer, 120u,
            nightVision.globals[frameSlot], 0u, 192u, nullptr, nullptr},
        {R4MscResourceType::ConstantBuffer, 144u,
            nightVision.staticGlobals[frameSlot], 0u, 528u, nullptr, nullptr},
        {R4MscResourceType::Sampler, 168u, nullptr, 0u, 0u, nullptr, nightVision.sampler},
    };
    R4IndexedDrawDesc draw;
    draw.indexBuffer = nightVision.indices;
    draw.indexCount = 6u;
    draw.indexType = R4IndexType::UInt16;
    if (!backend.BeginRenderPass(pass, error) ||
        !backend.BindGraphicsPipeline(nightVision.pipelines[generation - 1], error) ||
        !backend.BindVertexBuffer(6u, nightVision.vertices[frameSlot], 0u, error) ||
        !backend.BindMscResources(vertexBindings, error) ||
        !backend.BindMscResources(fragmentBindings, error) ||
        !backend.DrawIndexed(draw, error) || !backend.EndRenderPass(error))
        return false;
    if (nightVision.lastLoggedGeneration != generation ||
        nightVision.lastLoggedFrame == UINT32_MAX ||
        environment.engineFrame - nightVision.lastLoggedFrame >= 300u)
    {
        std::fprintf(stderr,
            "METAL_NIGHTVISION_FRAME frame=%u generation=%d active=true submitted=1 "
            "resources=position+scene+blur_2_4_8 passes=6+1 params=gameplay_authoritative\n",
            environment.engineFrame, generation);
        nightVision.lastLoggedGeneration = generation;
        nightVision.lastLoggedFrame = environment.engineFrame;
    }
    return true;
}

bool renderProductionScene(
    IR4Backend& backend,
    LevelScene& scene,
    TerrainScene& terrain,
    SkyScene& sky,
    MetalR4DefVertexAdapter* defVertex,
    MetalR4DefObjectsLodAdapter* defObjectsLod,
    MetalR4StaticForwardAdapter* staticForward,
    MetalR4SsfxAoAdapter* ssfxAo,
    std::uint64_t& ssfxAoTelemetryFrame,
    MetalR4RainAdapter* rain,
    std::uint64_t& rainTelemetryFrame,
    MetalR4WaterAdapter* water,
    MetalR4LocalLightAdapter* localLights,
    LocalShadowScene* localShadows,
    MetalR4DynamicWorldAdapter* dynamicWorld,
    MetalR4HudAdapter* hud,
    MetalR4ScopeAdapter* scope,
    MetalR4ParticleAdapter* particles,
    NightVisionScene* nightVision,
    const MetalR4EngineScene::ViewState& view,
    const R4EnvironmentSnapshot& environment,
    const R4GameplayVisionSnapshot& vision,
    const std::shared_ptr<const MetalR4EngineScene::GBufferWorldDrawCallback>& worldDraw,
    std::string& error)
{
    if (!ensureLevelDepthTarget(backend, scene, error) ||
        !uploadLevelScene(backend, scene, error) ||
        (defVertex && !defVertex->Upload(backend, error)) ||
        (defObjectsLod && !defObjectsLod->Upload(backend, error)) ||
        (staticForward && !staticForward->Upload(backend, error)))
        return false;
    if (scene.productionLuminance)
    {
        const R4Texture2DDesc& source = scene.lightingTargets[1]->Descriptor();
        if (!updateLuminanceConstants(
                backend, scene, source.width, source.height, environment, error))
            return false;
    }
    if (!uploadTerrainScene(backend, terrain, error))
    {
        std::fprintf(stderr, "METAL_TERRAIN_REJECT phase=upload error=%s\n", error.c_str());
        terrain.ready = false;
        error.clear();
    }

    if (scene.productionSun)
    {
        const auto drawDynamicSunCasters = [&backend, dynamicWorld](const Matrix4& lightViewProjection,
            std::string& drawError) {
            if (!dynamicWorld || !dynamicWorld->IsEnabled())
                return true;
            std::array<float, 16> matrix{};
            std::memcpy(matrix.data(), lightViewProjection.elements, sizeof(matrix));
            std::size_t candidates = 0u, submitted = 0u, culled = 0u, rejected = 0u;
            return dynamicWorld->DrawLocalShadowTile(backend, matrix,
                backend.EngineWorldSnapshot(), candidates, submitted, culled, rejected, drawError);
        };
        R4RenderPassDesc shadowPass;
        shadowPass.label = "R4 engine converted direct-sun near shadow map";
        shadowPass.useDrawable = false;
        R4ColorAttachmentDesc shadowColor;
        shadowColor.texture = scene.sunNearShadowTexture;
        shadowColor.clearColor = {1.0, 1.0, 1.0, 1.0};
        shadowPass.colorAttachments = {shadowColor};
        shadowPass.depthStencilAttachment.texture = scene.sunShadowDepthTarget;
        shadowPass.depthStencilAttachment.depthStoreAction = R4StoreAction::DontCare;
        if (!backend.BeginRenderPass(shadowPass, error) ||
            !drawSunShadowMap(backend, scene, true, error) ||
            !drawDynamicSunCasters(scene.sunNearShadowViewProjection, error) ||
            !backend.EndRenderPass(error))
            return false;

        shadowPass.label = "R4 engine converted direct-sun far shadow map";
        shadowPass.colorAttachments.front().texture = scene.sunShadowTexture;
        if (!backend.BeginRenderPass(shadowPass, error) ||
            !drawSunShadowMap(backend, scene, false, error) ||
            !drawDynamicSunCasters(scene.sunShadowViewProjection, error) ||
            !backend.EndRenderPass(error))
            return false;
    }

    R4RenderPassDesc gBufferPass;
    gBufferPass.label = "R4 engine scene G-buffer";
    gBufferPass.useDrawable = false;
    gBufferPass.depthStencilAttachment.texture = scene.depthTarget;
    gBufferPass.depthStencilAttachment.depthStoreAction = R4StoreAction::Store;
    for (const std::shared_ptr<IR4Texture>& target : scene.gBufferTargets)
    {
        R4ColorAttachmentDesc attachment;
        attachment.texture = target;
        attachment.clearColor = {0.0, 0.0, 0.0, 0.0};
        gBufferPass.colorAttachments.emplace_back(std::move(attachment));
    }
    if (!backend.BeginRenderPass(gBufferPass, error) ||
        !backend.SetStencilReference(1, error) ||
        !drawLevelScene(backend, scene, error))
        return false;
    if (defVertex)
    {
        R4DefVertexViewState defVertexView;
        defVertexView.view = view.view;
        defVertexView.projection = view.projection;
        defVertexView.valid = view.valid;
        if (!defVertex->DrawIntoActiveGBuffer(backend, defVertexView, error))
            return false;
    }
    if (defObjectsLod)
    {
        R4DefObjectsLodViewState defObjectsLodView;
        defObjectsLodView.view = view.view;
        defObjectsLodView.projection = view.projection;
        defObjectsLodView.staticGlobals = scene.baseLightingStaticGlobals;
        defObjectsLodView.valid = view.valid;
        if (!defObjectsLod->DrawIntoActiveGBuffer(backend, defObjectsLodView, error))
            return false;
    }
    if (!drawTerrainScene(backend, scene, terrain, error))
    {
        std::fprintf(stderr, "METAL_TERRAIN_REJECT phase=draw error=%s\n", error.c_str());
        terrain.ready = false;
        error.clear();
    }
    if (worldDraw)
    {
        const std::shared_ptr<const R4EngineWorldSnapshot> world =
            backend.EngineWorldSnapshot();
        const bool callbackSucceeded = backend.RunSceneOwnedGBufferCallback(
            [&worldDraw, &view, &world](IR4Backend& callbackBackend,
                std::string& callbackError) {
                return (*worldDraw)(callbackBackend, view, world, callbackError);
            },
            error);
        if (!callbackSucceeded)
        {
            if (error.empty())
                error = "scene-owned G-buffer world callback failed";
            // The callback is prevented from ending the pass, so the scene
            // can reliably close its own encoder before failure recovery.
            std::string ignored;
            backend.EndRenderPass(ignored);
            return false;
        }
    }
    if (!backend.EndRenderPass(error))
        return false;

    const std::shared_ptr<const R4EngineHudSnapshot> hudSnapshot =
        ((hud && hud->IsReady()) || (scope && scope->IsReady())) ?
            backend.EngineHudSnapshot() : nullptr;
    const bool hasScope = hudSnapshot && hudSnapshot->valid &&
        hudSnapshot->scope3DFakeEnabled && scope && scope->IsReady() &&
        std::any_of(hudSnapshot->renderables.begin(), hudSnapshot->renderables.end(),
            [](const R4EngineHudRenderable& renderable) {
                return !renderable.scopeLeaves.empty();
            });
    const bool hasOpaqueHud = hudSnapshot && hudSnapshot->valid && hud && hud->IsReady() &&
        std::any_of(hudSnapshot->renderables.begin(), hudSnapshot->renderables.end(),
            [](const R4EngineHudRenderable& renderable) {
                return !renderable.eligibleGeometryIndices.empty();
            });
    if (hasScope && !backend.CopyTexture2D(
            scene.depthTarget, scene.scopeDepthSnapshot, error))
        return false;
    if (hasOpaqueHud || hasScope)
    {
        R4RenderPassDesc hudPass;
        hudPass.label = "R4 engine near-depth HUD G-buffer";
        hudPass.useDrawable = false;
        hudPass.depthStencilAttachment.texture = scene.depthTarget;
        hudPass.depthStencilAttachment.depthLoadAction = R4LoadAction::Load;
        hudPass.depthStencilAttachment.depthStoreAction = R4StoreAction::Store;
        hudPass.depthStencilAttachment.stencilLoadAction = R4LoadAction::Load;
        hudPass.depthStencilAttachment.stencilStoreAction = R4StoreAction::Store;
        for (const std::shared_ptr<IR4Texture>& target : scene.gBufferTargets)
        {
            R4ColorAttachmentDesc attachment;
            attachment.texture = target;
            attachment.loadAction = R4LoadAction::Load;
            attachment.storeAction = R4StoreAction::Store;
            hudPass.colorAttachments.emplace_back(std::move(attachment));
        }
        const R4Texture2DDesc& hudTarget = scene.gBufferTargets.front()->Descriptor();
        hudPass.hasViewport = true;
        // Legacy R4's rmNear() maps HUD depth into the nearest two percent of
        // the world depth range. Keep that contract in a separate load/store
        // pass so first-person geometry cannot be rejected by nearby walls.
        hudPass.viewport = {0.0, 0.0, double(hudTarget.width), double(hudTarget.height), 0.0, 0.02};
        if (!backend.BeginRenderPass(hudPass, error))
            return false;
        if ((hasOpaqueHud && !hud->Draw(backend, hudSnapshot, error)) ||
            (hasScope && !scope->DrawBack(backend, hudSnapshot, error)))
        {
            std::string ignored;
            backend.EndRenderPass(ignored);
            return false;
        }
        if (!backend.EndRenderPass(error))
            return false;
    }
    if (hasScope)
    {
        R4RenderPassDesc zwritePass;
        zwritePass.label = "R4 3DSS stencil depth restore";
        zwritePass.useDrawable = false;
        zwritePass.depthStencilAttachment.texture = scene.depthTarget;
        zwritePass.depthStencilAttachment.depthLoadAction = R4LoadAction::Load;
        zwritePass.depthStencilAttachment.depthStoreAction = R4StoreAction::Store;
        zwritePass.depthStencilAttachment.stencilLoadAction = R4LoadAction::Load;
        zwritePass.depthStencilAttachment.stencilStoreAction = R4StoreAction::Store;
        for (const std::shared_ptr<IR4Texture>& target : scene.gBufferTargets)
        {
            R4ColorAttachmentDesc attachment;
            attachment.texture = target;
            attachment.loadAction = R4LoadAction::Load;
            attachment.storeAction = R4StoreAction::Store;
            zwritePass.colorAttachments.emplace_back(std::move(attachment));
        }
        const R4Texture2DDesc& target = scene.gBufferTargets.front()->Descriptor();
        zwritePass.hasViewport = true;
        zwritePass.viewport = {
            0.0, 0.0, double(target.width), double(target.height), 0.0, 1.0};
        MetalR4ScopeAdapter::PassInputs inputs;
        inputs.depthSnapshot = scene.scopeDepthSnapshot;
        if (!backend.BeginRenderPass(zwritePass, error) ||
            !scope->DrawZwrite(backend, hudSnapshot, inputs, error) ||
            !backend.EndRenderPass(error))
            return false;
    }

    if (localLights && localLights->IsReady() && localShadows && localShadows->ready &&
        !renderLocalShadowAtlas(backend, scene, *localShadows, view,
			backend.EngineLightingFrameSnapshot(), dynamicWorld, error))
        return false;

    R4RenderPassDesc accumulatorPass;
    accumulatorPass.label = "R4 engine light accumulator clear";
    accumulatorPass.useDrawable = false;
    R4ColorAttachmentDesc accumulatorAttachment;
    accumulatorAttachment.texture = scene.lightingAccumulator;
    accumulatorAttachment.clearColor = {0.0, 0.0, 0.0, 0.0};
    accumulatorPass.colorAttachments = {accumulatorAttachment};
    if (!backend.BeginRenderPass(accumulatorPass, error) || !backend.EndRenderPass(error))
        return false;

    if (scene.productionSun)
    {
        accumulatorAttachment.loadAction = R4LoadAction::Load;
        accumulatorAttachment.storeAction = R4StoreAction::Store;
        R4DepthStencilAttachmentDesc sunDepthStencil;
        sunDepthStencil.texture = scene.depthTarget;
        sunDepthStencil.depthLoadAction = R4LoadAction::Load;
        sunDepthStencil.depthStoreAction = R4StoreAction::Store;
        sunDepthStencil.stencilLoadAction = R4LoadAction::Load;
        sunDepthStencil.stencilStoreAction = R4StoreAction::Store;

        R4RenderPassDesc maskPass;
        maskPass.label = "R4 engine converted direct-sun stencil mask";
        maskPass.useDrawable = false;
        maskPass.colorAttachments = {accumulatorAttachment};
        maskPass.depthStencilAttachment = sunDepthStencil;
        if (!backend.BeginRenderPass(maskPass, error) ||
            !drawSunMask(backend, scene, error) || !backend.EndRenderPass(error))
            return false;

        R4RenderPassDesc sunPass;
        sunPass.label = "R4 engine converted direct-sun near cascade";
        sunPass.useDrawable = false;
        sunPass.colorAttachments = {accumulatorAttachment};
        sunPass.depthStencilAttachment = sunDepthStencil;
        if (!backend.BeginRenderPass(sunPass, error) ||
            !drawSunCascade(backend, scene, false, error) || !backend.EndRenderPass(error))
            return false;

        sunPass.label = "R4 engine converted direct-sun far cascade";
        if (!backend.BeginRenderPass(sunPass, error) ||
            !drawSunCascade(backend, scene, true, error) || !backend.EndRenderPass(error))
            return false;
    }

    if (localLights && localLights->IsReady())
    {
        accumulatorAttachment.loadAction = R4LoadAction::Load;
        accumulatorAttachment.storeAction = R4StoreAction::Store;
        R4DepthStencilAttachmentDesc localDepthStencil;
        localDepthStencil.texture = scene.depthTarget;
        localDepthStencil.depthLoadAction = R4LoadAction::Load;
        localDepthStencil.depthStoreAction = R4StoreAction::Store;
        localDepthStencil.stencilLoadAction = R4LoadAction::Load;
        localDepthStencil.stencilStoreAction = R4StoreAction::Store;
        R4RenderPassDesc localLightPass;
        localLightPass.label = "R4 converted local point/spot accumulation";
        localLightPass.useDrawable = false;
        localLightPass.colorAttachments = {accumulatorAttachment};
        localLightPass.depthStencilAttachment = localDepthStencil;
        MetalR4LocalLightAdapter::GBufferInputs inputs;
        inputs.position = scene.gBufferTargets.size() > 0u ? scene.gBufferTargets[0] : nullptr;
        inputs.diffuse = scene.gBufferTargets.size() > 1u ? scene.gBufferTargets[1] : nullptr;
        inputs.staticGlobals = scene.baseLightingStaticGlobals;
        inputs.noFilterSampler = scene.lightingNoFilterSampler;
        inputs.linearSampler = scene.lightingLinearSampler;
        if (localShadows && localShadows->ready)
        {
            inputs.shadowAtlas = localShadows->atlas;
            inputs.shadowSampler = scene.sunShadowSampler;
            inputs.shadowRecords = &localShadows->records;
        }
        MetalR4LocalLightAdapter::ViewState localView;
        localView.view = view.view; localView.projection = view.projection;
        localView.eye = view.eye; localView.valid = view.valid;
        if (!backend.BeginRenderPass(localLightPass, error) ||
            !localLights->DrawIntoActiveAccumulator(backend, inputs, localView,
				backend.EngineLightingFrameSnapshot(), error) || !backend.EndRenderPass(error))
            return false;
    }

    if (scene.productionBaseLighting && sky.configured)
    {
        std::string environmentError;
        if (!prepareCombineEnvironmentResources(
                backend, sky, scene, environment, environmentError))
        {
            if (!sky.combineUnavailableLogged || (environment.valid &&
                    environment.engineFrame - sky.lastCombineUnavailableFrame >= 300u))
            {
                std::fprintf(stderr,
                    "METAL_COMBINE_ENV_FALLBACK frame=%u reason=\"%s\"\n",
                    environment.engineFrame, environmentError.c_str());
                sky.combineUnavailableLogged = true;
                sky.lastCombineUnavailableFrame = environment.engineFrame;
            }
        }
        else
            sky.combineUnavailableLogged = false;
    }

    scene.ssfxAoOutput.reset();
    scene.ssfxIlOutput.reset();
    // Menu/loading frames have no gameplay camera or environment snapshot yet.
    // Keep combine inputs neutral there; the temporal AO/IL graph starts only
    // once the production G-buffer belongs to a live gameplay frame.
    if (ssfxAo && ssfxAo->IsReady() && view.valid && environment.valid)
    {
        if (scene.gBufferTargets.size() != 4u || !scene.gBufferTargets[0] ||
            !scene.gBufferTargets[3] || !scene.baseLightingStaticGlobals)
        {
            error = "SSFX AO requires the four-target production G-buffer and static_globals";
            return false;
        }
        MetalR4SsfxAoAdapter::FrameInputs aoInputs;
        aoInputs.position = scene.gBufferTargets[0];
        aoInputs.motionVectors = scene.gBufferTargets[3];
        aoInputs.accumulator = scene.lightingAccumulator;
        aoInputs.staticGlobals = scene.baseLightingStaticGlobals;
        aoInputs.view = view.view;
        aoInputs.projection = view.projection;
        aoInputs.environment = environment;
		if (!ssfxAo->Draw(backend, aoInputs, error))
		{
			// History/position targets can legitimately lag the first production
			// G-buffer after a level transition.  R4 continues with neutral AO/IL;
			// do the same rather than rejecting the entire lit scene.  Other AO
			// failures remain hard errors because they are not temporal startup.
			if (error == "SSFX AO frame has incomplete G-buffer, temporal, or static-global inputs")
			{
				static std::uint32_t lastNeutralAoFrame = std::numeric_limits<std::uint32_t>::max();
				if (lastNeutralAoFrame == std::numeric_limits<std::uint32_t>::max() ||
					environment.engineFrame - lastNeutralAoFrame >= 300u)
				{
					lastNeutralAoFrame = environment.engineFrame;
					std::fprintf(stderr, "METAL_SSFX_AO_IL_NEUTRAL frame=%u reason=startup_inputs_unavailable\n",
						environment.engineFrame);
				}
				error.clear();
			}
			else
				return false;
		}
		else
		{
			scene.ssfxAoOutput = ssfxAo->Output();
			scene.ssfxIlOutput = ssfxAo->IndirectLightOutput();
		}
        const MetalR4SsfxAoAdapter::Telemetry& telemetry = ssfxAo->LastTelemetry();
        const std::uint64_t sample = ++ssfxAoTelemetryFrame;
        if (sample == 1u || sample % 300u == 0u)
            std::fprintf(stderr,
                "METAL_SSFX_AO_IL_FRAME sample=%llu engine_frame=%u extent=%ux%u ao_passes=%u il_passes=%u temporal_history=%s\n",
                static_cast<unsigned long long>(sample), telemetry.frame, telemetry.width, telemetry.height, telemetry.passes,
                telemetry.indirectLightPasses,
                telemetry.temporalHistory ? "true" : "false");
    }

    R4RenderPassDesc lightingPass;
    lightingPass.label = "R4 engine converted combine_1 base lighting";
    lightingPass.useDrawable = false;
    for (const std::shared_ptr<IR4Texture>& target : scene.lightingTargets)
    {
        R4ColorAttachmentDesc attachment;
        attachment.texture = target;
        attachment.clearColor = {0.0, 0.0, 0.0, 0.0};
        lightingPass.colorAttachments.emplace_back(std::move(attachment));
    }
    if (!backend.BeginRenderPass(lightingPass, error) ||
        !drawBaseLighting(backend, scene, error) || !backend.EndRenderPass(error))
        return false;

    if (sky.ready)
    {
        std::shared_ptr<IR4Texture> sky0;
        std::shared_ptr<IR4Texture> sky1;
        std::string skyError;
        if (!prepareSkyResources(backend, sky, environment, sky0, sky1, skyError))
        {
            if (!sky.resourceUnavailableLogged || (environment.valid &&
                    environment.engineFrame - sky.lastResourceUnavailableFrame >= 300u))
            {
                std::fprintf(stderr, "METAL_SKY_SKIP reason=resource_prepare frame=%u error=%s\n",
                    environment.engineFrame, skyError.c_str());
                sky.resourceUnavailableLogged = true;
                sky.lastResourceUnavailableFrame = environment.engineFrame;
            }
        }
        else
        {
        sky.resourceUnavailableLogged = false;
        std::shared_ptr<IR4Texture> cloud0;
        std::shared_ptr<IR4Texture> cloud1;
        bool cloudsPrepared = false;
        if (sky.cloudsPipeline && environment.cloudsValid)
        {
            std::string cloudError;
            cloudsPrepared = prepareCloudResources(backend, sky, environment,
                cloud0, cloud1, cloudError);
            if (!cloudsPrepared)
                std::fprintf(stderr, "METAL_CLOUD_SKIP reason=resource_prepare frame=%u error=%s\n",
                    environment.engineFrame, cloudError.c_str());
        }
        R4RenderPassDesc skyPass;
        skyPass.label = "R4 converted sky2 background";
        skyPass.useDrawable = false;
        for (const std::shared_ptr<IR4Texture>& target : scene.lightingTargets)
        {
            R4ColorAttachmentDesc attachment;
            attachment.texture = target;
            attachment.loadAction = R4LoadAction::Load;
            attachment.storeAction = R4StoreAction::Store;
            skyPass.colorAttachments.emplace_back(std::move(attachment));
        }
        skyPass.depthStencilAttachment.texture = scene.depthTarget;
        skyPass.depthStencilAttachment.depthLoadAction = R4LoadAction::Load;
        skyPass.depthStencilAttachment.depthStoreAction = R4StoreAction::Store;
        skyPass.depthStencilAttachment.stencilLoadAction = R4LoadAction::Load;
        skyPass.depthStencilAttachment.stencilStoreAction = R4StoreAction::Store;
        const bool passStarted = backend.BeginRenderPass(skyPass, skyError);
        const bool skyDrawn = passStarted && drawSkyScene(
            backend, sky, scene, view, environment, vision, sky0, sky1, skyError);
        const bool passEnded = passStarted && backend.EndRenderPass(skyError);
        if (!skyDrawn || !passEnded)
        {
            std::fprintf(stderr, "METAL_SKY_SKIP reason=runtime_error frame=%u error=%s\n",
                environment.engineFrame, skyError.c_str());
            // Sky is optional presentation content. Its resources and pass are
            // isolated from the established G-buffer/base-lighting chain, so
            // a missing cubemap must not discard an otherwise playable frame.
        }
        if (cloudsPrepared)
        {
            R4RenderPassDesc cloudPass;
            cloudPass.label = "R4 converted clouds hemisphere";
            cloudPass.useDrawable = false;
            R4ColorAttachmentDesc color;
            color.texture = scene.lightingTargets[0];
            color.loadAction = R4LoadAction::Load;
            color.storeAction = R4StoreAction::Store;
            cloudPass.colorAttachments = {color};
            cloudPass.depthStencilAttachment.texture = scene.depthTarget;
            cloudPass.depthStencilAttachment.depthLoadAction = R4LoadAction::Load;
            cloudPass.depthStencilAttachment.depthStoreAction = R4StoreAction::Store;
            cloudPass.depthStencilAttachment.stencilLoadAction = R4LoadAction::Load;
            cloudPass.depthStencilAttachment.stencilStoreAction = R4StoreAction::Store;
            const R4Texture2DDesc& target = scene.lightingTargets[0]->Descriptor();
            cloudPass.hasViewport = true;
            cloudPass.viewport = {0.0, 0.0, double(target.width), double(target.height),
                0.99999, 1.0};
            std::string cloudError;
            const bool cloudPassStarted = backend.BeginRenderPass(cloudPass, cloudError);
            const bool cloudsDrawn = cloudPassStarted && drawCloudScene(backend, scene, sky,
                view, environment, cloud0, cloud1, cloudError);
            const bool cloudPassEnded = cloudPassStarted && backend.EndRenderPass(cloudError);
            if (!cloudsDrawn || !cloudPassEnded)
                std::fprintf(stderr, "METAL_CLOUD_SKIP reason=runtime_error frame=%u error=%s\n",
                    environment.engineFrame, cloudError.c_str());
        }
        }
    }

    if (!drawLuminanceAdaptation(backend, scene, environment, error))
        return false;

    // Reference R4 places SSFX water after combine/sky/luminance and before
    // the remaining alpha-forward families.  The dedicated adapter owns the
    // temporal read/write chain; static-forward therefore skips only water
    // when that adapter was admitted successfully.
    const bool dedicatedWater = water && water->IsReady() &&
        scene.gBufferTargets.size() > 0u && scene.gBufferTargets[0] &&
        scene.lightingAccumulator && scene.depthTarget &&
        scene.lightingEnvironment0 && scene.lightingEnvironment1;
    if (dedicatedWater)
    {
        MetalR4WaterAdapter::FrameInputs inputs;
        inputs.sceneColor = scene.lightingTargets[0];
        inputs.position = scene.gBufferTargets[0];
        inputs.accumulator = scene.lightingAccumulator;
        inputs.depth = scene.depthTarget;
        inputs.environment0 = scene.lightingEnvironment0;
        inputs.environment1 = scene.lightingEnvironment1;
        inputs.staticGlobals = scene.baseLightingStaticGlobals;
        inputs.view = view.view;
        inputs.projection = view.projection;
        inputs.environment = environment;
        if (!water->Draw(backend, inputs, error))
            return false;
    }

    if (staticForward && staticForward->IsReady())
    {
        if (scene.lightingTargets.empty() || !scene.lightingTargets[0] || !scene.depthTarget)
        {
            error = "static-forward pass requires the combined lighting and depth targets";
            return false;
        }
        R4RenderPassDesc forwardPass;
        forwardPass.label = "R4 converted static alpha-blended forward pass";
        forwardPass.useDrawable = false;
        R4ColorAttachmentDesc color;
        color.texture = scene.lightingTargets[0];
        color.loadAction = R4LoadAction::Load;
        color.storeAction = R4StoreAction::Store;
        forwardPass.colorAttachments = {color};
        forwardPass.depthStencilAttachment.texture = scene.depthTarget;
        forwardPass.depthStencilAttachment.depthLoadAction = R4LoadAction::Load;
        forwardPass.depthStencilAttachment.depthStoreAction = R4StoreAction::Store;
        forwardPass.depthStencilAttachment.stencilLoadAction = R4LoadAction::Load;
        forwardPass.depthStencilAttachment.stencilStoreAction = R4StoreAction::Store;
        R4StaticForwardViewState forwardView;
        forwardView.view = view.view;
        forwardView.projection = view.projection;
        forwardView.eye = view.eye;
        forwardView.valid = view.valid;
        if (!backend.BeginRenderPass(forwardPass, error) ||
            !staticForward->DrawIntoActiveForward(
                backend, forwardView, scene.baseLightingStaticGlobals, error,
                !dedicatedWater) ||
            !backend.EndRenderPass(error))
            return false;
    }

    if (rain && rain->IsReady() && rain->HasDrops())
    {
        if (scene.lightingTargets.size() != 2u || !scene.lightingTargets[0] ||
            !scene.scopeSceneColorSnapshot || !scene.depthTarget ||
            scene.gBufferTargets.empty() || !scene.gBufferTargets[0] || !scene.lightingAccumulator ||
            !scene.baseLightingStaticGlobals || !scene.lightingBlack || !rain->UnusedMrtTarget() ||
            !rain->MotionTarget() || !rain->RefractionTarget())
        {
            error = "effects_rain requires production combined, G-buffer, accumulator, and MRT targets";
            return false;
        }
        // effects_rain samples the pre-rain combined image; copying first is
        // the explicit Metal hazard boundary corresponding to generic0.
        if (!backend.CopyTexture2D(scene.lightingTargets[0], scene.scopeSceneColorSnapshot, error))
            return false;
        R4RenderPassDesc rainPrepass;
        rainPrepass.label = "R4 exact ssfx_rain 1/8 prepass";
        rainPrepass.useDrawable = false;
        R4ColorAttachmentDesc refraction;
        refraction.texture = rain->RefractionTarget();
        rainPrepass.colorAttachments = {refraction};
        if (!backend.BeginRenderPass(rainPrepass, error) ||
            !rain->DrawRefraction(backend, scene.scopeSceneColorSnapshot, scene.lightingBlack,
                scene.baseLightingStaticGlobals, error) || !backend.EndRenderPass(error))
            return false;
        R4RenderPassDesc rainPass;
        rainPass.label = "R4 exact effects_rain forward pass";
        rainPass.useDrawable = false;
        R4ColorAttachmentDesc low;
        low.texture = scene.lightingTargets[0]; low.loadAction = R4LoadAction::Load; low.storeAction = R4StoreAction::Store;
        R4ColorAttachmentDesc unused;
        unused.texture = rain->UnusedMrtTarget(); unused.loadAction = R4LoadAction::Clear; unused.storeAction = R4StoreAction::DontCare;
        R4ColorAttachmentDesc motion;
        motion.texture = rain->MotionTarget(); motion.loadAction = R4LoadAction::Clear; motion.storeAction = R4StoreAction::Store;
        rainPass.colorAttachments = {low, unused, motion};
        rainPass.depthStencilAttachment.texture = scene.depthTarget;
        rainPass.depthStencilAttachment.depthLoadAction = R4LoadAction::Load;
        rainPass.depthStencilAttachment.depthStoreAction = R4StoreAction::Store;
        rainPass.depthStencilAttachment.stencilLoadAction = R4LoadAction::Load;
        rainPass.depthStencilAttachment.stencilStoreAction = R4StoreAction::Store;
        std::array<float, 16> viewProjection{};
        std::memcpy(viewProjection.data(), scene.uniformData.viewProjection.elements, sizeof(viewProjection));
        if (!backend.BeginRenderPass(rainPass, error) ||
            !rain->DrawIntoActiveForward(backend, viewProjection, scene.gBufferTargets[0],
                rain->RefractionTarget(), scene.lightingAccumulator,
                scene.baseLightingStaticGlobals, error) || !backend.EndRenderPass(error))
            return false;
        const std::uint64_t sample = ++rainTelemetryFrame;
        if (sample == 1u || sample % 300u == 0u)
        {
            const MetalR4RainAdapter::Telemetry& telemetry = rain->LastTelemetry();
            std::fprintf(stderr,
                "METAL_RAIN_FRAME sample=%llu drops=%zu vertices=%zu density=%.3f ssfx=%s dropped_vertices=%zu\n",
                static_cast<unsigned long long>(sample), telemetry.submittedDrops,
                telemetry.capturedVertices, telemetry.density, telemetry.ssfx ? "true" : "false",
                telemetry.droppedVertices);
        }
    }

    if (particles && particles->IsReady() && !particles->PreparedDraws().empty())
    {
        if (scene.lightingTargets.empty() || !scene.lightingTargets[0] || !scene.depthTarget)
        {
            error = "particle forward pass requires the combined lighting and depth targets";
            return false;
        }
        R4RenderPassDesc particlePass;
        particlePass.label = "R4 converted soft/hard particle forward pass";
        particlePass.useDrawable = false;
        R4ColorAttachmentDesc color;
        color.texture = scene.lightingTargets[0];
        color.loadAction = R4LoadAction::Load;
        color.storeAction = R4StoreAction::Store;
        particlePass.colorAttachments = {color};
        particlePass.depthStencilAttachment.texture = scene.depthTarget;
        particlePass.depthStencilAttachment.depthLoadAction = R4LoadAction::Load;
        particlePass.depthStencilAttachment.depthStoreAction = R4StoreAction::Store;
        particlePass.depthStencilAttachment.stencilLoadAction = R4LoadAction::Load;
        particlePass.depthStencilAttachment.stencilStoreAction = R4StoreAction::Store;
        std::array<float, 16> viewProjection{};
        std::memcpy(viewProjection.data(), scene.uniformData.viewProjection.elements,
            sizeof(viewProjection));
        if (!backend.BeginRenderPass(particlePass, error) ||
            !particles->DrawIntoActiveForward(backend, viewProjection,
                scene.gBufferTargets[0],
                scene.baseLightingStaticGlobals, error) ||
            !backend.EndRenderPass(error))
            return false;
    }

    if (particles && particles->IsReady())
    {
        if (!particles->DistortionTarget() || !scene.depthTarget)
        {
            error = "particle distortion pass requires its mask and scene depth targets";
            return false;
        }
        R4RenderPassDesc distortionPass;
        distortionPass.label = "R4 converted particle distortion mask";
        distortionPass.useDrawable = false;
        R4ColorAttachmentDesc color;
        color.texture = particles->DistortionTarget();
        color.clearColor = {127.0 / 255.0, 127.0 / 255.0, 0.0, 127.0 / 255.0};
        distortionPass.colorAttachments = {color};
        distortionPass.depthStencilAttachment.texture = scene.depthTarget;
        distortionPass.depthStencilAttachment.depthLoadAction = R4LoadAction::Load;
        distortionPass.depthStencilAttachment.depthStoreAction = R4StoreAction::Store;
        distortionPass.depthStencilAttachment.stencilLoadAction = R4LoadAction::Load;
        distortionPass.depthStencilAttachment.stencilStoreAction = R4StoreAction::Store;
        std::array<float, 16> viewProjection{};
        std::memcpy(viewProjection.data(), scene.uniformData.viewProjection.elements,
            sizeof(viewProjection));
        if (!backend.BeginRenderPass(distortionPass, error) ||
            !particles->DrawIntoActiveDistortion(backend, viewProjection,
                scene.baseLightingStaticGlobals, error) ||
            !backend.EndRenderPass(error))
            return false;
    }

    if (hasScope)
    {
        if (!backend.CopyTexture2D(scene.lightingTargets[0],
                scene.scopeSceneColorSnapshot, error))
            return false;
        R4RenderPassDesc reticlePass;
        reticlePass.label = "R4 3DSS converted precise reticle";
        reticlePass.useDrawable = false;
        R4ColorAttachmentDesc color;
        color.texture = scene.lightingTargets[0];
        color.loadAction = R4LoadAction::Load;
        color.storeAction = R4StoreAction::Store;
        reticlePass.colorAttachments = {color};
        reticlePass.depthStencilAttachment.texture = scene.depthTarget;
        reticlePass.depthStencilAttachment.depthLoadAction = R4LoadAction::Load;
        reticlePass.depthStencilAttachment.depthStoreAction = R4StoreAction::Store;
        reticlePass.depthStencilAttachment.stencilLoadAction = R4LoadAction::Load;
        reticlePass.depthStencilAttachment.stencilStoreAction = R4StoreAction::Store;
        const R4Texture2DDesc& target = scene.lightingTargets[0]->Descriptor();
        reticlePass.hasViewport = true;
        reticlePass.viewport = {
            0.0, 0.0, double(target.width), double(target.height), 0.0, 0.02};
        if (!hasReadableGBufferHeatTarget(scene.gBufferTargets, target))
        {
            error = "3DSS precise reticle requires the shader-readable RGBA8 G-buffer heat target";
            return false;
        }
        MetalR4ScopeAdapter::PassInputs inputs;
        inputs.sceneColorSnapshot = scene.scopeSceneColorSnapshot;
        inputs.position = scene.gBufferTargets[0];
        inputs.tonemap = scene.lightingTonemap;
        inputs.heat = scene.gBufferTargets[GBufferHeatTargetIndex];
        inputs.staticGlobals = scene.baseLightingStaticGlobals;
        if (!backend.BeginRenderPass(reticlePass, error) ||
            !scope->DrawReticle(backend, hudSnapshot, inputs, error) ||
            !backend.EndRenderPass(error))
            return false;
    }

    if (nightVision && !drawNightVision(
            backend, scene, *nightVision, environment, vision, error))
        return false;

    R4RenderPassDesc displayPass;
    displayPass.label = "R4 engine production presentation";
    displayPass.drawableColor.clearColor = EngineSceneClearColor;
    const auto drawPresentation = [&]() {
        if (particles && particles->IsReady())
        {
            const bool hasLut = scene.lutAtlas && scene.lutAtlasUploaded;
            const std::shared_ptr<IR4Texture>& lut = hasLut ? scene.lutAtlas : scene.lightingBlack;
            const std::array<float, 4> ssfxLut = hasLut ?
                std::array<float, 4>{1.0f, 16.0f, 0.0f, 0.0f} :
                std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f};
            return particles->DrawPostprocess(backend, scene.gBufferTargets[0],
                scene.lightingTargets[0], scene.lightingBlack, lut,
                scene.baseLightingStaticGlobals, ssfxLut, error);
        }
        return drawGBufferDisplay(backend, scene, error);
    };
    if (!backend.BeginRenderPass(displayPass, error) ||
        !drawPresentation() || !backend.EndRenderPass(error))
        return false;
    return true;
}
} // namespace

bool RunMetalR4EngineSceneFrustumCullingSelfTest(std::string& error)
{
    xray::metal::R4PositionBounds bounds;
    bounds.available = true;
    bounds.minimum = {-1.0f, -1.0f, -1.0f};
    bounds.maximum = {1.0f, 1.0f, 1.0f};
    const Matrix4 identity{{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    }};
    if (conservativelyOutsideMetalFrustum(bounds, identity, identity, true))
    {
        error = "identity AABB was incorrectly culled";
        return false;
    }
    Matrix4 rightOutside = identity;
    rightOutside.elements[12] = 3.0f;
    if (!conservativelyOutsideMetalFrustum(bounds, rightOutside, identity, true))
    {
        error = "AABB fully outside the Metal right plane was not culled";
        return false;
    }
    Matrix4 touchingRightPlane = identity;
    touchingRightPlane.elements[12] = 2.0f;
    if (conservativelyOutsideMetalFrustum(bounds, touchingRightPlane, identity, true))
    {
        error = "AABB touching the clip boundary was incorrectly culled";
        return false;
    }
    Matrix4 nonFiniteTransform = identity;
    nonFiniteTransform.elements[12] = std::numeric_limits<float>::quiet_NaN();
    if (conservativelyOutsideMetalFrustum(bounds, nonFiniteTransform, identity, true) ||
        conservativelyOutsideMetalFrustum(bounds, rightOutside, identity, false))
    {
        error = "frustum culling did not fail open for uncertain input";
        return false;
    }
    std::vector<MetalR4EngineScene::TerrainShaderMaterial> terrainSource = {{
        "levels\\test_dotted",
        {"detail\\ground.v2", "detail\\mud", "detail\\earth", "detail\\stone"},
    }};
    TerrainMaterialMap terrainMaterials;
    if (!buildTerrainMaterialMap(terrainSource, terrainMaterials, error) ||
        terrainMaterials.size() != 1u ||
        terrainMaterials.at("levels\\test_dotted")[0] != "detail\\ground.v2" ||
        terrainDdsIdentity("detail\\ground.v2") != "detail\\ground.v2.dds" ||
        terrainDdsIdentity("detail\\ground.v2", "_bump") !=
            "detail\\ground.v2_bump.dds" ||
        terrainDdsIdentity("detail\\already.DDS") != "detail\\already.dds")
    {
        if (error.empty()) error = "terrain material identity mapping is not stable";
        return false;
    }
    terrainSource.push_back(terrainSource.front());
    if (buildTerrainMaterialMap(terrainSource, terrainMaterials, error))
    {
        error = "terrain material admission accepted a duplicate shader record";
        return false;
    }
    error.clear();
    return true;
}

struct MetalR4EngineScene::Impl
{
    LevelScene scene;
    TerrainScene terrain;
    SkyScene sky;
    bool prepared = false;
    mutable std::mutex worldDrawMutex;
    std::shared_ptr<const GBufferWorldDrawCallback> worldDraw;
    std::unique_ptr<MetalR4FloraAdapter> flora;
    std::unique_ptr<MetalR4DefVertexAdapter> defVertex;
    std::unique_ptr<MetalR4DefObjectsLodAdapter> defObjectsLod;
    std::unique_ptr<MetalR4DynamicWorldAdapter> dynamicWorld;
    std::unique_ptr<MetalR4HudAdapter> hud;
    std::unique_ptr<MetalR4ScopeAdapter> scope;
    std::unique_ptr<MetalR4ParticleAdapter> particles;
    std::unique_ptr<MetalR4StaticForwardAdapter> staticForward;
    std::unique_ptr<MetalR4SsfxAoAdapter> ssfxAo;
    std::uint64_t ssfxAoTelemetryFrame = 0;
    std::unique_ptr<MetalR4RainAdapter> rain;
    std::uint64_t rainTelemetryFrame = 0;
    std::unique_ptr<MetalR4WaterAdapter> water;
    std::unique_ptr<MetalR4DetailGrassAdapter> detailGrass;
    std::unique_ptr<MetalR4LocalLightAdapter> localLights;
    LocalShadowScene localShadows;
    NightVisionScene nightVision;
    bool detailSnapshotLoaded = false;
    std::uint64_t detailFrameGeneration = UINT64_MAX;
    std::uint32_t detailLastLoggedFrame = UINT32_MAX;
    std::uint32_t detailDrawLogFrame = UINT32_MAX;
    R4EnvironmentSnapshot detailEnvironment;
    R4EnvironmentSnapshot floraEnvironment;
    std::uint64_t floraTelemetryFrame = 0;
    bool floraLiveFrustumReported = false;
    bool floraGameplayFrustumReported = false;
    std::uint64_t defVertexTelemetryFrame = 0;
    bool defVertexLiveFrustumReported = false;
    bool defVertexGameplayFrustumReported = false;
    std::uint64_t defObjectsLodTelemetryFrame = 0;
    bool defObjectsLodLiveFrustumReported = false;
    bool defObjectsLodGameplayFrustumReported = false;
    std::uint32_t lastDynamicTelemetryFrame = UINT32_MAX;
    std::size_t lastDynamicTelemetryDraws = std::numeric_limits<std::size_t>::max();
    std::size_t lastDynamicTelemetryRejected = std::numeric_limits<std::size_t>::max();
    std::size_t lastDynamicTelemetryHotDraws = std::numeric_limits<std::size_t>::max();
	std::uint32_t lastHudTelemetryFrame = UINT32_MAX;
	std::uint64_t lastHudTelemetryCoverage = std::numeric_limits<std::uint64_t>::max();
    std::uint32_t lastScopeTelemetryFrame = UINT32_MAX;
    std::uint64_t lastScopeTelemetryCoverage = std::numeric_limits<std::uint64_t>::max();
    std::uint32_t lastLocalLightTelemetryFrame = UINT32_MAX;
    std::size_t lastLocalLightRequested = std::numeric_limits<std::size_t>::max();
    std::size_t lastLocalLightAdmitted = std::numeric_limits<std::size_t>::max();
    std::size_t lastLocalLightCulled = std::numeric_limits<std::size_t>::max();
    std::size_t lastLocalLightShadowed = std::numeric_limits<std::size_t>::max();
    std::size_t lastLocalShadowRendered = std::numeric_limits<std::size_t>::max();
    xray::metal::particles::FrameBatch particleFrame;
    xray::metal::rain::FrameBatch rainFrame;
    xray::metal::lensflare::Frame lensFlareFrame;
    std::uint32_t lastParticleTelemetryFrame = UINT32_MAX;
    std::size_t lastParticleSubmitted = std::numeric_limits<std::size_t>::max();
    std::uint64_t staticForwardTelemetryFrame = 0;
    bool staticForwardLiveFrustumReported = false;
    bool staticForwardGameplayFrustumReported = false;
};

MetalR4EngineScene::MetalR4EngineScene() : impl(std::make_unique<Impl>()) {}
MetalR4EngineScene::~MetalR4EngineScene() = default;

bool MetalR4EngineScene::OptionsFromEnvironment(Options& options, std::string& error)
{
    options = {};
    const auto requiredPath = [&error](const char* name, std::filesystem::path& path) {
        const char* value = std::getenv(name);
        if (!value || !*value)
        {
            error = std::string("missing required environment variable ") + name;
            return false;
        }
        path = value;
        return true;
    };
    if (const char* levelDir = std::getenv("XRAY_METAL_LEVEL_DIR"); levelDir && *levelDir)
        options.levelDirectory = levelDir;

    if (!requiredPath("XRAY_METAL_MSC_VS_METALLIB", options.mscVertexMetallibPath) ||
        !requiredPath("XRAY_METAL_MSC_PS_METALLIB", options.mscFragmentMetallibPath) ||
        !requiredPath("XRAY_METAL_MSC_LIGHTING_VS_METALLIB", options.lightingVertexMetallibPath) ||
        !requiredPath("XRAY_METAL_MSC_LIGHTING_PS_METALLIB", options.lightingFragmentMetallibPath) ||
        !requiredPath("XRAY_METAL_MSC_SUN_DIR", options.sunShaderDirectory) ||
        !requiredPath("XRAY_METAL_MSC_LUMINANCE_DIR", options.luminanceShaderDirectory))
        return false;
    if (const char* skyDirectory = std::getenv("XRAY_METAL_MSC_SKY_DIR");
        skyDirectory && *skyDirectory)
        options.skyShaderDirectory = skyDirectory;
    if (const char* cloudsDirectory = std::getenv("XRAY_METAL_MSC_CLOUDS_DIR");
        cloudsDirectory && *cloudsDirectory)
        options.cloudsShaderDirectory = cloudsDirectory;
    if (const char* nightVisionDirectory = std::getenv("XRAY_METAL_MSC_NIGHTVISION_DIR");
        nightVisionDirectory && *nightVisionDirectory)
        options.nightVisionShaderDirectory = nightVisionDirectory;
    if (const char* arefDirectory = std::getenv("XRAY_METAL_MSC_AREF_DIR");
        arefDirectory && *arefDirectory)
        options.mscArefFragmentMetallibPath =
            std::filesystem::path(arefDirectory) / "deffer_base_lmh_aref_flat.ps.metallib";
    if (const char* defVertexDirectory = std::getenv("XRAY_METAL_MSC_DEF_VERTEX_DIR");
        defVertexDirectory && *defVertexDirectory)
        options.defVertex.shaderDirectory = defVertexDirectory;
    if (const char* defObjectsLodDirectory = std::getenv("XRAY_METAL_MSC_DEF_OBJECTS_LOD_DIR");
        defObjectsLodDirectory && *defObjectsLodDirectory)
        options.defObjectsLod.shaderDirectory = defObjectsLodDirectory;
    if (const char* localLightDirectory = std::getenv("XRAY_METAL_MSC_LOCAL_LIGHT_DIR");
        localLightDirectory && *localLightDirectory)
        options.localLights.shaderDirectory = localLightDirectory;
    const char* controlledMaterials = std::getenv("XRAY_METAL_MATERIALS");
    if (controlledMaterials && *controlledMaterials)
    {
        if (std::strcmp(controlledMaterials, "all") == 0)
            options.autoLmhMaterials = true;
        else if (!parseControlledMaterials(controlledMaterials, options.materials, error))
            return false;
    }
    else
    {
        std::uint32_t visualId = 7364;
        const char* visualIdEnv = std::getenv("XRAY_METAL_VISUAL_ID");
        if (visualIdEnv && !parseUnsigned(visualIdEnv, visualId))
        {
            error = "XRAY_METAL_VISUAL_ID must be an unsigned 32-bit integer";
            return false;
        }
        std::filesystem::path baseDds;
        std::filesystem::path hemiDds;
        if (!requiredPath("XRAY_METAL_TEXTURE_DDS", baseDds) ||
            !requiredPath("XRAY_METAL_HEMI_DDS", hemiDds))
            return false;
        options.materials.push_back({visualId, std::move(baseDds), std::move(hemiDds)});
    }
    const char* terrainVisuals = std::getenv("XRAY_METAL_TERRAIN_VISUALS");
    if (terrainVisuals && *terrainVisuals)
    {
        if (std::strcmp(terrainVisuals, "all") == 0)
            options.terrainAutoSelect = true;
        else if (!parseTerrainVisualIds(terrainVisuals, options.terrainVisualIds, error))
            return false;
    }
    if (options.terrainAutoSelect || !options.terrainVisualIds.empty())
    {
        const char* terrainDirectory = std::getenv("XRAY_METAL_MSC_TERRAIN_DIR");
        options.terrainShaderDirectory = terrainDirectory && *terrainDirectory ?
            std::filesystem::path(terrainDirectory) : options.mscVertexMetallibPath.parent_path();
    }
    const char* floraDirectory = std::getenv("XRAY_METAL_MSC_FLORA_DIR");
    const std::filesystem::path floraShaderDirectory = floraDirectory && *floraDirectory ?
        std::filesystem::path(floraDirectory) : options.mscVertexMetallibPath.parent_path();
    options.flora.leafVertexMetallibPath = floraShaderDirectory / "leaf_wave_hq.vs.metallib";
    options.flora.leafFragmentMetallibPath = floraShaderDirectory / "leaf_wave_hq.ps.metallib";
    options.flora.trunkVertexMetallibPath = floraShaderDirectory / "trunk_wave_hq.vs.metallib";
    options.flora.trunkFragmentMetallibPath = floraShaderDirectory / "trunk_wave_hq.ps.metallib";
    // Flora always samples game textures through XRFS, including when the
    // controlled LMH material and terrain paths happen to be disk-backed.
    if (!options.ddsBytesProvider &&
        !CreateMetalR4EngineXrfsProvider(options.ddsBytesProvider, error))
        return false;
    options.flora.ddsBytesProvider = options.ddsBytesProvider;
    if (!options.defVertex.shaderDirectory.empty())
        options.defVertex.ddsBytesProvider = options.ddsBytesProvider;
    if (!options.defObjectsLod.shaderDirectory.empty())
        options.defObjectsLod.ddsBytesProvider = options.ddsBytesProvider;
    const char* dynamicDir = std::getenv("XRAY_METAL_MSC_DYNAMIC_DIR");
    const char* dynamicVs = std::getenv("XRAY_METAL_DYNAMIC_BUMP_VS_METALLIB");
    const char* dynamicPs = std::getenv("XRAY_METAL_DYNAMIC_BUMP_PS_METALLIB");
    const char* dynamicModelVs = std::getenv("XRAY_METAL_DYNAMIC_MODEL_BUMP_VS_METALLIB");
    if (dynamicDir && *dynamicDir && ((dynamicVs && *dynamicVs) || (dynamicPs && *dynamicPs) ||
            (dynamicModelVs && *dynamicModelVs)))
    {
        error = "XRAY_METAL_MSC_DYNAMIC_DIR cannot be combined with explicit dynamic bump metallib paths";
        return false;
    }
    if (dynamicDir && *dynamicDir)
    {
        options.dynamicWorld.vertexMetallibPath =
            std::filesystem::path(dynamicDir) / "deffer_base_bump.vs.metallib";
        options.dynamicWorld.fragmentMetallibPath =
            std::filesystem::path(dynamicDir) / "deffer_base_bump.ps.metallib";
        options.dynamicWorld.modelVertexMetallibPath =
            std::filesystem::path(dynamicDir) / "deffer_model_bump.vs.metallib";
    }
    if ((dynamicVs && *dynamicVs) != (dynamicPs && *dynamicPs))
    {
        error = "XRAY_METAL_DYNAMIC_BUMP_VS_METALLIB and XRAY_METAL_DYNAMIC_BUMP_PS_METALLIB must be set together";
        return false;
    }
    if (dynamicVs && *dynamicVs)
    {
        options.dynamicWorld.vertexMetallibPath = dynamicVs;
        options.dynamicWorld.fragmentMetallibPath = dynamicPs;
    }
    if (dynamicModelVs && *dynamicModelVs)
        options.dynamicWorld.modelVertexMetallibPath = dynamicModelVs;
    if (!options.dynamicWorld.vertexMetallibPath.empty())
    {
        if (!options.ddsBytesProvider &&
            !CreateMetalR4EngineXrfsProvider(options.ddsBytesProvider, error))
            return false;
        options.dynamicWorld.ddsBytesProvider = options.ddsBytesProvider;
    }
    const char* dynamicShadowVs = std::getenv("XRAY_METAL_DYNAMIC_SHADOW_VS_METALLIB");
    const char* dynamicShadowPs = std::getenv("XRAY_METAL_DYNAMIC_SHADOW_PS_METALLIB");
    if (dynamicShadowVs && *dynamicShadowVs && dynamicShadowPs && *dynamicShadowPs)
    {
        options.dynamicWorld.shadowVertexMetallibPath = dynamicShadowVs;
        options.dynamicWorld.shadowFragmentMetallibPath = dynamicShadowPs;
    }
    else if (!options.sunShaderDirectory.empty())
    {
        options.dynamicWorld.shadowVertexMetallibPath =
            options.sunShaderDirectory / "shadow_direct_base.vs.metallib";
        options.dynamicWorld.shadowFragmentMetallibPath =
            options.sunShaderDirectory / "shadow_direct_base_color.ps.metallib";
    }
    if (const char* hudDirectory = std::getenv("XRAY_METAL_MSC_HUD_DIR"); hudDirectory && *hudDirectory)
    {
        options.hud.shaderDirectory = hudDirectory;
        if (!options.ddsBytesProvider && !CreateMetalR4EngineXrfsProvider(options.ddsBytesProvider, error))
            return false;
        options.hud.ddsBytesProvider = options.ddsBytesProvider;
    }
    if (const char* particleDirectory = std::getenv("XRAY_METAL_MSC_PARTICLE_DIR");
        particleDirectory && *particleDirectory)
    {
        options.particles.shaderDirectory = particleDirectory;
        if (!options.ddsBytesProvider && !CreateMetalR4EngineXrfsProvider(options.ddsBytesProvider, error))
            return false;
        options.particles.ddsBytesProvider = options.ddsBytesProvider;
    }
    if (const char* forwardDirectory = std::getenv("XRAY_METAL_MSC_STATIC_FORWARD_DIR");
        forwardDirectory && *forwardDirectory)
    {
        options.staticForward.shaderDirectory = forwardDirectory;
        if (!options.ddsBytesProvider &&
            !CreateMetalR4EngineXrfsProvider(options.ddsBytesProvider, error))
            return false;
        options.staticForward.ddsBytesProvider = options.ddsBytesProvider;
    }
    if (const char* waterDirectory = std::getenv("XRAY_METAL_MSC_WATER_DIR");
        waterDirectory && *waterDirectory)
    {
        options.water.shaderDirectory = waterDirectory;
        if (!options.ddsBytesProvider && !CreateMetalR4EngineXrfsProvider(options.ddsBytesProvider, error))
            return false;
        options.water.ddsBytesProvider = options.ddsBytesProvider;
    }
    if (const char* aoDirectory = std::getenv("XRAY_METAL_MSC_AO_DIR");
        aoDirectory && *aoDirectory)
        options.ssfxAo.shaderDirectory = aoDirectory;
    if (const char* rainDirectory = std::getenv("XRAY_METAL_MSC_RAIN_DIR");
        rainDirectory && *rainDirectory)
    {
        options.rain.shaderDirectory = rainDirectory;
        options.rain.ddsBytesProvider = options.ddsBytesProvider;
    }
    if (const char* flareDirectory = std::getenv("XRAY_METAL_MSC_LENS_FLARE_DIR");
        flareDirectory && *flareDirectory)
        options.lensFlareShaderDirectory = flareDirectory;
    if (!options.localLights.shaderDirectory.empty())
        options.localLights.ddsBytesProvider = options.ddsBytesProvider;
    const char* detailDirectory = std::getenv("XRAY_METAL_MSC_DETAIL_DIR");
    if (detailDirectory && *detailDirectory)
    {
        const std::filesystem::path directory(detailDirectory);
        options.detailGrass.vertexMetallibPath = directory / "deffer_grass.vs.metallib";
        options.detailGrass.fragmentMetallibPath = directory / "deffer_grass.ps.metallib";
        if (!options.ddsBytesProvider &&
            !CreateMetalR4EngineXrfsProvider(options.ddsBytesProvider, error))
            return false;
        options.detailGrass.ddsBytesProvider = options.ddsBytesProvider;
        if (!ResolveMetalR4EngineTextureDdsPath(std::filesystem::path("fx") / "wind_wave.dds",
                options.detailGrass.wavesTexturePath, error))
            return false;
    }
    return options.autoLmhMaterials || xray::metal::ValidateR4ControlledMaterialSet(
        options.materials, xray::metal::R4ControlledMaterialBatchLimit, error);
}

bool MetalR4EngineScene::Prepare(IR4Backend& backend, const Options& options, std::string& error)
{
    Reset();
    if (options.levelDirectory.empty() || options.mscVertexMetallibPath.empty() ||
        options.mscFragmentMetallibPath.empty() || options.lightingVertexMetallibPath.empty() ||
        options.lightingFragmentMetallibPath.empty() || options.sunShaderDirectory.empty() ||
        options.luminanceShaderDirectory.empty() ||
        (!options.autoLmhMaterials && !xray::metal::ValidateR4ControlledMaterialSet(
            options.materials, xray::metal::R4ControlledMaterialBatchLimit, error)))
    {
        if (error.empty())
            error = "engine scene requires controlled production LMH, combine, and sun materials";
        return false;
    }
    std::vector<LevelMaterialInput> materialInputs;
    AutomaticLmhSelection automaticLmh;
    if (options.autoLmhMaterials)
    {
        if (!selectAutomaticLmhMaterials(options.levelDirectory,
                !options.mscArefFragmentMetallibPath.empty(), automaticLmh, error))
            return false;
        materialInputs = std::move(automaticLmh.materials);
        std::fprintf(stderr,
            "METAL_LMH_AUTO_SELECT mode=all requested=%zu admitted=%zu skipped_invalid=%zu "
            "skipped_duplicate=%zu skipped_shader=%zu skipped_slots=%zu skipped_vertex_abi=%zu "
            "skipped_base=%zu skipped_hemi=%zu textures=%zu\n",
            automaticLmh.requested, automaticLmh.admitted, automaticLmh.skippedInvalid,
            automaticLmh.skippedDuplicate, automaticLmh.skippedShader,
            automaticLmh.skippedTextureSlots, automaticLmh.skippedVertexAbi,
            automaticLmh.skippedBaseTexture, automaticLmh.skippedHemiTexture,
            xray::metal::CountUniqueR4ControlledMaterialTexturePaths(
                [&materialInputs]() {
                    std::vector<xray::metal::R4ControlledMaterial> result;
                    result.reserve(materialInputs.size());
                    for (const LevelMaterialInput& material : materialInputs)
                        result.push_back({material.visualId, material.baseDdsPath, material.hemiDdsPath});
                    return result;
                }()));
    }
    else
    {
        materialInputs.reserve(options.materials.size());
        for (const xray::metal::R4ControlledMaterial& material : options.materials)
            materialInputs.push_back({material.visualId, material.baseDdsPath, material.hemiDdsPath});
    }
    const xray::metal::R4DdsBytesProvider* provider = options.ddsBytesProvider ?
        &options.ddsBytesProvider : nullptr;
    if (!prepareLevelScene(options.levelDirectory, {}, {}, materialInputs, provider,
            options.mscVertexMetallibPath, options.mscFragmentMetallibPath,
            options.mscArefFragmentMetallibPath,
            options.lightingVertexMetallibPath, options.lightingFragmentMetallibPath,
            options.sunShaderDirectory, options.luminanceShaderDirectory,
            std::numeric_limits<std::uint32_t>::max(),
            std::numeric_limits<std::uint32_t>::max(), backend,
            impl->scene, error))
        return false;
    if (!impl->scene.productionMsc || !impl->scene.productionBaseLighting ||
        !impl->scene.productionSun || !impl->scene.productionLuminance ||
        impl->scene.textureMaterials.size() != materialInputs.size())
    {
        error = "engine scene did not admit the complete production MSC R4 path";
        Reset();
        return false;
    }
    if (!prepareNightVisionScene(
            backend, options.nightVisionShaderDirectory, impl->nightVision, error))
    {
        error = "night vision MSC preparation failed: " + error;
        Reset();
        return false;
    }
    if (impl->nightVision.ready)
        std::fprintf(stderr,
            "METAL_NIGHTVISION_PREPARED generations=1,2,3 tlab=24/192 "
            "constants=192+528 bundle=%s\n",
            options.nightVisionShaderDirectory.generic_string().c_str());
    std::fprintf(stderr,
        "METAL_LUMINANCE_PREPARED shaders=bloom_luminance_1,2,3 "
        "binding=automatic_linear chain=64x64,8x8,1x1 source=combine_high bundle=%s\n",
        options.luminanceShaderDirectory.generic_string().c_str());
    if (!options.ssfxAo.shaderDirectory.empty())
    {
        impl->ssfxAo = std::make_unique<MetalR4SsfxAoAdapter>();
        std::string aoStatus;
        if (!impl->ssfxAo->Prepare(backend, options.ssfxAo, aoStatus) || !impl->ssfxAo->IsReady())
        {
            error = "SSFX AO MSC adapter preparation failed: " +
                (aoStatus.empty() ? impl->ssfxAo->Status() : aoStatus);
            Reset();
            return false;
        }
        std::fprintf(stderr,
            "METAL_SSFX_AO_IL_PREPARED shaders=ssfx_ao+blurx4+ssfx_il+blurx4 bundle=%s tlab=24/240/120/264/72\n",
            options.ssfxAo.shaderDirectory.generic_string().c_str());
    }
    if (!options.rain.shaderDirectory.empty())
    {
        impl->rain = std::make_unique<MetalR4RainAdapter>();
        std::string rainStatus;
        if (!impl->rain->Configure(backend, options.rain, rainStatus) || !impl->rain->IsReady())
        {
            error = "effects_rain MSC adapter preparation failed: " +
                (rainStatus.empty() ? impl->rain->Status() : rainStatus);
            Reset();
            return false;
        }
        std::fprintf(stderr, "METAL_RAIN_PREPARED shader=effects_rain bundle=%s tlab=48/192 mrt=0,2\n",
            options.rain.shaderDirectory.generic_string().c_str());
    }
    if (!options.defVertex.shaderDirectory.empty())
    {
        impl->defVertex = std::make_unique<MetalR4DefVertexAdapter>();
        std::string defVertexStatus;
        if (!impl->defVertex->Prepare(backend, impl->scene.geometry, impl->scene.vertexBuffers,
                impl->scene.indexBuffers, options.defVertex, defVertexStatus) ||
            !impl->defVertex->IsReady())
        {
            error = "def_vertex MSC adapter preparation failed: " +
                (defVertexStatus.empty() ? impl->defVertex->Status() : defVertexStatus);
            Reset();
            return false;
        }
        const MetalR4DefVertexAdapter::Telemetry& telemetry = impl->defVertex->LastTelemetry();
        if (telemetry.requestedVisuals > 0u &&
            (telemetry.admittedVisuals != telemetry.requestedVisuals ||
             telemetry.alphaTestedVisuals > telemetry.admittedVisuals ||
             telemetry.rejectedVisuals != 0u))
        {
            error = "def_vertex MSC adapter did not admit every configured-level family visual";
            Reset();
            return false;
        }
        std::fprintf(stderr,
            "METAL_DEF_VERTEX_PREPARED shaders=def_shaders\\def_vertex,def_shaders\\def_aref_v "
            "requested=%zu admitted=%zu aref=%zu rejected=%zu textures=%zu bundle=%s tlab=48/72\n",
            telemetry.requestedVisuals, telemetry.admittedVisuals,
            telemetry.alphaTestedVisuals, telemetry.rejectedVisuals,
            telemetry.textureAssets, options.defVertex.shaderDirectory.generic_string().c_str());
    }
    if (!options.defObjectsLod.shaderDirectory.empty())
    {
        impl->defObjectsLod = std::make_unique<MetalR4DefObjectsLodAdapter>();
        std::string defObjectsLodStatus;
        if (!impl->defObjectsLod->Prepare(backend, impl->scene.geometry, impl->scene.vertexBuffers,
                impl->scene.indexBuffers, options.defObjectsLod, defObjectsLodStatus) ||
            !impl->defObjectsLod->IsReady())
        {
            error = "def_objects_lod MSC adapter preparation failed: " +
                (defObjectsLodStatus.empty() ? impl->defObjectsLod->Status() : defObjectsLodStatus);
            Reset();
            return false;
        }
        const MetalR4DefObjectsLodAdapter::Telemetry& telemetry =
            impl->defObjectsLod->LastTelemetry();
        if (telemetry.requestedVisuals > 0u &&
            (telemetry.flatVisuals + telemetry.bumpVisuals + telemetry.steepVisuals !=
                telemetry.requestedVisuals ||
             telemetry.rejectedVisuals != 0u))
        {
            error = "def_objects_lod MSC adapter did not admit every configured-level family visual";
            Reset();
            return false;
        }
        std::fprintf(stderr,
            "METAL_DEF_OBJECTS_LOD_PREPARED shader=def_shaders\\def_objects_lod requested=%zu "
            "flat=%zu bump=%zu steep=%zu rejected=%zu textures=%zu bundle=%s tlab=48/96_or_120_or_168\n",
            telemetry.requestedVisuals, telemetry.flatVisuals, telemetry.bumpVisuals, telemetry.steepVisuals,
            telemetry.rejectedVisuals, telemetry.textureAssets,
            options.defObjectsLod.shaderDirectory.generic_string().c_str());
    }
    if (!options.staticForward.shaderDirectory.empty())
    {
        impl->staticForward = std::make_unique<MetalR4StaticForwardAdapter>();
        std::string forwardStatus;
        if (!impl->staticForward->Prepare(backend, options.levelDirectory,
                impl->scene.geometry, impl->scene.vertexBuffers, impl->scene.indexBuffers,
                impl->scene.lightingEnvironment, options.staticForward, forwardStatus) ||
            !impl->staticForward->IsReady())
        {
            error = "static-forward MSC adapter preparation failed: " +
                (forwardStatus.empty() ? impl->staticForward->Status() : forwardStatus);
            Reset();
            return false;
        }
        const auto& telemetry = impl->staticForward->LastTelemetry();
        if (telemetry.requestedVisuals > 0u &&
            (telemetry.lightmappedVisuals + telemetry.vertexColorVisuals +
                    telemetry.wallmarkVisuals + telemetry.lightplaneVisuals +
                    telemetry.waterVisuals !=
                telemetry.requestedVisuals ||
             telemetry.rejectedVisuals != 0u))
        {
            error = "static-forward MSC adapter did not admit every configured-level family visual";
            Reset();
            return false;
        }
        std::fprintf(stderr,
            "METAL_STATIC_FORWARD_PREPARED shaders=def_trans,def_trans_v,wallmarkblend,lightplanes,water "
            "requested=%zu lightmapped=%zu vertex_color=%zu wallmarks=%zu lightplanes=%zu water=%zu "
            "rejected=%zu textures=%zu "
            "transformed=%zu/%zu/%zu bundle=%s order=back_to_front depth=read_only "
            "blend=alpha_or_additive\n",
            telemetry.requestedVisuals, telemetry.lightmappedVisuals,
            telemetry.vertexColorVisuals, telemetry.wallmarkVisuals,
            telemetry.lightplaneVisuals, telemetry.waterVisuals, telemetry.rejectedVisuals, telemetry.textureAssets,
            telemetry.transformedLightmappedVisuals,
            telemetry.transformedVertexColorVisuals,
            telemetry.transformedWaterVisuals,
            options.staticForward.shaderDirectory.generic_string().c_str());
    }
    if (!options.water.shaderDirectory.empty())
    {
        if (!impl->staticForward || !impl->staticForward->IsReady())
        {
            error = "SSFX water requires admitted static-forward water geometry records";
            Reset();
            return false;
        }
        impl->water = std::make_unique<MetalR4WaterAdapter>();
        std::string waterStatus;
        if (!impl->water->Prepare(backend, impl->staticForward->WaterDrawRecords(),
                options.water, waterStatus) || !impl->water->IsReady())
        {
            error = "SSFX water MSC adapter preparation failed: " +
                (waterStatus.empty() ? impl->water->Status() : waterStatus);
            Reset();
            return false;
        }
        const MetalR4WaterAdapter::Telemetry& telemetry = impl->water->LastTelemetry();
        std::fprintf(stderr,
            "METAL_WATER_PREPARED records=%zu rejected=%zu ssr_pipelines=%zu final_pipelines=%zu "
            "fullscreen_pipelines=%zu bundle=%s targets=rgba8_fullres+waves512\n",
            telemetry.capturedRecords, telemetry.rejectedRecords, telemetry.ssrPipelines,
            telemetry.finalPipelines, telemetry.fullscreenPipelines,
            options.water.shaderDirectory.generic_string().c_str());
    }
    // Explicit material lists retain their established bounded submission
    // behavior. Culling is deliberately enabled only for the full structural
    // automatic LMH batch selected by XRAY_METAL_MATERIALS=all.
    impl->scene.automaticLmhFrustumCulling = options.autoLmhMaterials;
    if (!prepareSkyScene(backend, options.skyShaderDirectory, options.ddsBytesProvider,
            impl->sky, error))
    {
        error = "sky2 MSC preparation failed: " + error;
        Reset();
        return false;
    }
    if (impl->sky.ready)
        std::fprintf(stderr, "METAL_SKY_PREPARED shader=sky2_msc bundle=%s tlab=48/120 targets=2\n",
            options.skyShaderDirectory.generic_string().c_str());
    if (!prepareCloudsScene(backend, options.cloudsShaderDirectory, impl->sky, error))
    {
        error = "clouds MSC preparation failed: " + error;
        Reset();
        return false;
    }
    if (impl->sky.cloudsPipeline)
        std::fprintf(stderr, "METAL_CLOUDS_PREPARED shader=clouds_msc bundle=%s tlab=72/96\n",
            options.cloudsShaderDirectory.generic_string().c_str());
    std::vector<std::uint32_t> terrainVisualIds = options.terrainVisualIds;
    TerrainMaterialMap terrainMaterials;
    if ((options.terrainAutoSelect || !terrainVisualIds.empty()) &&
        !buildTerrainMaterialMap(options.terrainShaderMaterials, terrainMaterials, error))
    {
        error = "terrain material admission failed: " + error;
        Reset();
        return false;
    }
    TerrainAutoSelection automaticTerrain;
    if (options.terrainAutoSelect)
    {
        automaticTerrain = selectAllAdmittedTerrainVisuals(impl->scene, terrainMaterials);
        terrainVisualIds = automaticTerrain.visualIds;
        std::fprintf(stderr,
            "METAL_TERRAIN_AUTO_SELECT requested=%zu admitted=%zu skipped=%zu "
            "materials=archive_authored_bmmd\n",
            automaticTerrain.requested, automaticTerrain.admitted, automaticTerrain.skipped);
    }
    if (!terrainVisualIds.empty())
    {
        std::string terrainError;
        if (!prepareTerrainScene(backend, impl->scene, options.levelDirectory,
                terrainVisualIds, terrainMaterials,
                options.terrainShaderDirectory, options.ddsBytesProvider,
                impl->terrain, terrainError))
        {
            std::fprintf(stderr, "METAL_TERRAIN_REJECT phase=prepare error=%s\n",
                terrainError.c_str());
            impl->terrain = {};
        }
        else
        {
            std::uint64_t vertices = 0;
            std::uint64_t indices = 0;
            for (const TerrainDraw& draw : impl->terrain.draws)
            {
                vertices += draw.vertexCount;
                indices += draw.indexCount;
            }
            std::fprintf(stderr,
                "METAL_TERRAIN_PREPARED visuals=%zu vertices=%llu indices=%llu textures=%zu "
                "shader=deffer_terrain_high_flat_d.ps\n",
                impl->terrain.draws.size(), static_cast<unsigned long long>(vertices),
                static_cast<unsigned long long>(indices), impl->terrain.textures.size());
        }
    }
    MetalR4FloraAdapter::Options floraOptions = options.flora;
    if (floraOptions.leafVertexMetallibPath.empty() && floraOptions.leafFragmentMetallibPath.empty() &&
        floraOptions.trunkVertexMetallibPath.empty() && floraOptions.trunkFragmentMetallibPath.empty())
    {
        const std::filesystem::path directory = options.mscVertexMetallibPath.parent_path();
        floraOptions.leafVertexMetallibPath = directory / "leaf_wave_hq.vs.metallib";
        floraOptions.leafFragmentMetallibPath = directory / "leaf_wave_hq.ps.metallib";
        floraOptions.trunkVertexMetallibPath = directory / "trunk_wave_hq.vs.metallib";
        floraOptions.trunkFragmentMetallibPath = directory / "trunk_wave_hq.ps.metallib";
    }
    if (!floraOptions.ddsBytesProvider)
        floraOptions.ddsBytesProvider = options.ddsBytesProvider;
    impl->flora = std::make_unique<MetalR4FloraAdapter>();
    std::string floraStatus;
    if (!impl->flora->Prepare(backend, impl->scene.geometry, impl->scene.geometryBytes,
            floraOptions, floraStatus))
    {
        error = "flora MSC adapter preparation failed: " + floraStatus;
        Reset();
        return false;
    }
    const MetalR4FloraAdapter::Telemetry& floraTelemetry = impl->flora->LastTelemetry();
    std::fprintf(stderr, "METAL_FLORA_PREPARE status=\"%s\" total=%zu admitted=%zu skipped=%zu\n",
        floraStatus.c_str(), floraTelemetry.totalVisuals, floraTelemetry.admittedVisuals,
        floraTelemetry.skippedVisuals);
    impl->prepared = true;
    impl->dynamicWorld = std::make_unique<MetalR4DynamicWorldAdapter>();
    std::string dynamicStatus;
    if (!impl->dynamicWorld->Configure(backend, options.dynamicWorld, dynamicStatus))
    {
        error = "dynamic OGF adapter configuration failed: " + dynamicStatus;
        Reset();
        return false;
    }
    if (!options.hud.shaderDirectory.empty())
    {
        impl->hud = std::make_unique<MetalR4HudAdapter>();
        std::string hudStatus;
        if (!impl->hud->Configure(backend, options.hud, hudStatus))
        { error = "HUD adapter configuration failed: " + hudStatus; Reset(); return false; }
        std::fprintf(stderr, "METAL_HUD_PREPARE status=\"%s\" enabled=%s\n", hudStatus.c_str(), impl->hud->IsReady()?"true":"false");
        impl->scope = std::make_unique<MetalR4ScopeAdapter>();
        std::string scopeStatus;
        MetalR4ScopeAdapter::Options scopeOptions;
        scopeOptions.shaderDirectory = options.hud.shaderDirectory;
        scopeOptions.ddsBytesProvider = options.hud.ddsBytesProvider;
        if (!impl->scope->Configure(backend, scopeOptions, scopeStatus))
        { error = "3DSS scope adapter configuration failed: " + scopeStatus; Reset(); return false; }
        std::fprintf(stderr, "METAL_3DSS_PREPARE status=\"%s\" enabled=%s\n",
            scopeStatus.c_str(), impl->scope->IsReady() ? "true" : "false");
    }
    if (!options.particles.shaderDirectory.empty())
    {
        impl->particles = std::make_unique<MetalR4ParticleAdapter>();
        std::string particleStatus;
        if (!impl->particles->Configure(backend, options.particles, particleStatus))
        {
            error = "particle adapter configuration failed: " + particleStatus;
            Reset();
            return false;
        }
        std::fprintf(stderr, "METAL_PARTICLE_PREPARE status=\"%s\" enabled=%s\n",
            particleStatus.c_str(), impl->particles->IsReady() ? "true" : "false");
    }
    if (options.ddsBytesProvider)
    {
        xray::metal::R4DdsTexture lutDds;
        std::string lutError;
        const std::filesystem::path lutPath = "xrfs:$game_textures$@shaders/lut_atlas.dds";
        std::vector<std::uint8_t> rawBytes;
        bool loaded = false;
        if (options.ddsBytesProvider(lutPath, xray::metal::R4DdsProviderMaximumBytes, rawBytes, lutError))
        {
            static const std::uint8_t pngMagic[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
            if (rawBytes.size() >= 8u && std::memcmp(rawBytes.data(), pngMagic, 8) == 0)
                loaded = decodePngImage(rawBytes, lutDds, lutError);
            else
                loaded = xray::metal::ParseR4DdsTexture(rawBytes, lutDds, lutError);
        }
        if (loaded &&
            lutDds.descriptor.type == R4TextureType::Texture2D &&
            lutDds.descriptor.arrayLength == 1u)
        {
            lutDds.descriptor.label = "R4 SSFX LUT atlas";
            lutDds.descriptor.usage = R4TextureUsage::ShaderRead;
            impl->scene.lutAtlas = backend.Resources().CreateTexture2D(lutDds.descriptor, lutError);
            if (impl->scene.lutAtlas)
            {
                impl->scene.lutAtlasDds = std::move(lutDds);
                impl->scene.lutAtlasUploaded = false;
                std::fprintf(stderr, "METAL_LUT_ATLAS_PREPARED width=%u height=%u mips=%u format=%u\n",
                    impl->scene.lutAtlas->Descriptor().width, impl->scene.lutAtlas->Descriptor().height,
                    impl->scene.lutAtlas->Descriptor().mipLevels, static_cast<unsigned>(impl->scene.lutAtlas->Descriptor().format));
            }
            else
            {
                std::fprintf(stderr, "METAL_LUT_ATLAS_CREATE_FAIL reason=\"%s\"\n", lutError.c_str());
            }
        }
        else
        {
            std::fprintf(stderr, "METAL_LUT_ATLAS_LOAD_FAIL reason=\"%s\"\n", lutError.c_str());
        }
    }
    if (!options.localLights.shaderDirectory.empty())
    {
        impl->localLights = std::make_unique<MetalR4LocalLightAdapter>();
        std::string localLightStatus;
        if (!impl->localLights->Prepare(backend, options.localLights, localLightStatus))
        {
            error = "local-light MSC adapter preparation failed: " + localLightStatus;
            Reset();
            return false;
        }
        std::fprintf(stderr, "METAL_LOCAL_LIGHT_PREPARE status=\"%s\" enabled=%s\n",
            localLightStatus.c_str(), impl->localLights->IsReady() ? "true" : "false");
        if (impl->localLights->IsReady())
        {
            if (!prepareLocalShadowScene(backend, impl->scene, impl->localShadows, error))
            {
                error = "local shadow atlas preparation failed: " + error;
                Reset();
                return false;
            }
            std::fprintf(stderr,
                "METAL_LOCAL_SHADOW_PREPARE atlas=%ux%u tile=%u capacity=%zu casters=static_lmh_only\n",
                LocalShadowAtlasExtent, LocalShadowAtlasExtent, LocalShadowTileSize,
                LocalShadowTileCapacity);
        }
    }
    if (!options.detailGrass.vertexMetallibPath.empty() || !options.detailGrass.fragmentMetallibPath.empty())
    {
        MetalR4DetailGrassAdapter::Options detailOptions = options.detailGrass;
        if (!detailOptions.resolveMaterialTextures)
        {
            const std::string levelName = options.levelDirectory.lexically_normal().filename().string();
            detailOptions.resolveMaterialTextures = [levelName](const std::string& identity,
                std::filesystem::path& base, std::filesystem::path& bump, std::string& localError) {
                if (identity.empty() || identity.find("..") != std::string::npos ||
                    std::filesystem::path(identity).is_absolute())
                {
                    localError = "details blend has an unsafe base texture identity";
                    return false;
                }
                std::filesystem::path baseName(identity);
                baseName.replace_extension(".dds");
                if (!ResolveMetalR4EngineTextureDdsPath(baseName, base, localError))
                    return false;
                const std::filesystem::path bumpName = std::filesystem::path("levels") /
                    levelName / (identity + "_bump.dds");
                return ResolveMetalR4EngineTextureDdsPath(bumpName, bump, localError);
            };
        }
        impl->detailGrass = std::make_unique<MetalR4DetailGrassAdapter>();
        std::string detailStatus;
        if (!impl->detailGrass->Configure(backend, detailOptions, detailStatus))
        {
            error = "detail grass MSC adapter configuration failed: " + detailStatus;
            Reset();
            return false;
        }
        std::fprintf(stderr, "METAL_DETAIL_GRASS_PREPARE status=\"%s\" enabled=%s\n",
            detailStatus.c_str(), impl->detailGrass->IsReady() ? "true" : "false");
    }
    if (impl->flora->IsReady() || impl->dynamicWorld->IsEnabled() ||
        (impl->detailGrass && impl->detailGrass->IsReady()))
    {
        SetGBufferWorldDrawCallback([this](IR4Backend& callbackBackend,
            const ViewState& callbackView,
            const std::shared_ptr<const R4EngineWorldSnapshot>& world,
            std::string& callbackError) {
            if (!impl)
                return true;
            if (impl->flora && impl->flora->IsReady())
            {
                R4FloraViewState floraView;
                floraView.view = callbackView.view;
                floraView.projection = callbackView.projection;
                floraView.previousView = callbackView.previousView;
                floraView.previousProjection = callbackView.previousProjection;
                floraView.eye = callbackView.eye;
                floraView.staticGlobals = impl->scene.baseLightingStaticGlobals;
                floraView.timeSeconds = impl->floraEnvironment.globalTime;
                floraView.treeScaleBiasCorrection =
                    impl->floraEnvironment.treeScaleBiasCorrection;
                floraView.valid = callbackView.valid;
                floraView.previousValid = callbackView.previousValid;
                if (!impl->flora->DrawIntoActiveGBuffer(callbackBackend, floraView, callbackError))
                    return false;
                const MetalR4FloraAdapter::Telemetry& flora = impl->flora->LastTelemetry();
                const std::uint64_t floraFrame = ++impl->floraTelemetryFrame;
                const bool firstLiveReport = callbackView.valid && !impl->floraLiveFrustumReported;
                const bool firstGameplayReport = impl->scene.gameplayViewValid &&
                    !impl->floraGameplayFrustumReported;
                if (floraFrame == 1u || firstLiveReport || firstGameplayReport ||
                    floraFrame % 300u == 0u)
                {
                    std::fprintf(stderr,
                        "METAL_FLORA_FRAME sample=%llu total=%zu admitted=%zu submitted=%zu culled=%zu live_view=%s skipped=%zu\n",
                        static_cast<unsigned long long>(floraFrame), flora.totalVisuals,
                        flora.admittedVisuals, flora.submittedVisuals, flora.culledVisuals,
                        callbackView.valid ? "true" : "false", flora.skippedVisuals);
                    if (firstLiveReport)
                        impl->floraLiveFrustumReported = true;
                    if (firstGameplayReport)
                        impl->floraGameplayFrustumReported = true;
                }
            }
            if (impl->detailGrass && impl->detailGrass->IsReady())
            {
                R4DetailGrassViewState detailView;
                detailView.view = callbackView.view;
                detailView.projection = callbackView.projection;
                detailView.previousView = callbackView.previousView;
                detailView.previousProjection = callbackView.previousProjection;
                detailView.eye = callbackView.eye;
                detailView.timeSeconds = impl->detailEnvironment.globalTime;
                detailView.ambient = impl->detailEnvironment.ambient;
                detailView.hemiColor = impl->detailEnvironment.hemiColor;
                detailView.sunColor = impl->detailEnvironment.adaptedSunColor;
                detailView.sunDirection = impl->detailEnvironment.adaptedSunDirection;
                detailView.fogColor = impl->detailEnvironment.fogColor;
                detailView.fogDensity = impl->detailEnvironment.fogDensity;
                detailView.fogNear = impl->detailEnvironment.fogNear;
                detailView.fogFar = impl->detailEnvironment.fogFar;
                detailView.valid = callbackView.valid && impl->detailEnvironment.valid;
                detailView.previousValid = callbackView.previousValid;
                if (!impl->detailGrass->DrawIntoActiveGBuffer(callbackBackend, detailView, callbackError))
                    return false;
                const MetalR4DetailGrassAdapter::Telemetry& detail = impl->detailGrass->LastTelemetry();
                const std::uint32_t detailFrame = impl->detailEnvironment.engineFrame;
                if (detail.draws != 0u && (impl->detailDrawLogFrame == UINT32_MAX ||
                    detailFrame - impl->detailDrawLogFrame >= 300u))
                {
                    std::fprintf(stderr, "METAL_DETAIL_GRASS_FRAME frame=%u models=%zu slots=%zu admitted=%zu "
                        "draws=%zu rejected_batches=%zu texture_rejected=%zu\n",
                        detailFrame, detail.models, detail.slots,
                        detail.admittedInstances, detail.draws, detail.rejectedBatches,
                        detail.textureRejectedBatches);
                    impl->detailDrawLogFrame = detailFrame;
                }
            }
            if (!impl->dynamicWorld || !impl->dynamicWorld->IsEnabled())
                return true;
            R4DynamicWorldViewState dynamicView;
            dynamicView.view = callbackView.view;
            dynamicView.projection = callbackView.projection;
            dynamicView.valid = callbackView.valid;
            const bool drawn = impl->dynamicWorld->Draw(
                callbackBackend, dynamicView, world, callbackError);
            const MetalR4DynamicWorldAdapter::Telemetry& telemetry =
                impl->dynamicWorld->LastTelemetry();
            if (telemetry.engineFrame != impl->lastDynamicTelemetryFrame &&
                (impl->lastDynamicTelemetryFrame == UINT32_MAX ||
                 telemetry.engineFrame - impl->lastDynamicTelemetryFrame >= 300u))
            {
                std::fprintf(stderr,
                    "METAL_DYNAMIC_OGF_FRAME frame=%u renderables=%zu cached=%zu skipped=%zu "
                    "skinned_candidates=%zu bounds_rejected=%zu frustum_rejected=%zu "
                    "skinned_draws=%zu influences=%zu/%zu/%zu/%zu bump=%zu flat=%zu "
                    "cache_hit=%zu cache_miss=%zu pose_reused=%zu draws=%zu "
                    "hot_renderables=%zu hot_draws=%zu "
                    "max_world_radius=%.3f max_expected_radius=%.3f\n",
                    telemetry.engineFrame, telemetry.renderables, telemetry.cachedGeometry,
                    telemetry.rejectedGeometry, telemetry.skinnedCandidates,
                    telemetry.skinnedBoundsRejected, telemetry.frustumRejected,
                    telemetry.skinnedDraws, telemetry.skinned1WDraws, telemetry.skinned2WDraws,
                    telemetry.skinned3WDraws, telemetry.skinned4WDraws,
                    telemetry.bumpMaterialGeometry, telemetry.flatMaterialGeometry,
                    telemetry.instanceCacheHits, telemetry.instanceCacheMisses,
                    telemetry.skinnedVerticesReused, telemetry.draws,
                    telemetry.hotRenderables, telemetry.hotDraws,
                    telemetry.largestSkinnedWorldRadius, telemetry.largestSkinnedExpectedRadius);
                impl->lastDynamicTelemetryFrame = telemetry.engineFrame;
                impl->lastDynamicTelemetryDraws = telemetry.draws;
                impl->lastDynamicTelemetryRejected = telemetry.rejectedGeometry;
                impl->lastDynamicTelemetryHotDraws = telemetry.hotDraws;
            }
            return drawn;
        });
    }
    return true;
}

void MetalR4EngineScene::Reset()
{
    std::shared_ptr<const GBufferWorldDrawCallback> worldDraw;
    if (impl)
    {
        std::lock_guard<std::mutex> lock(impl->worldDrawMutex);
        worldDraw = impl->worldDraw;
    }
    impl = std::make_unique<Impl>();
    std::lock_guard<std::mutex> lock(impl->worldDrawMutex);
    impl->worldDraw = std::move(worldDraw);
}

void MetalR4EngineScene::SetGBufferWorldDrawCallback(GBufferWorldDrawCallback callback)
{
    if (!impl)
        impl = std::make_unique<Impl>();
    std::shared_ptr<const GBufferWorldDrawCallback> replacement;
    if (callback)
        replacement = std::make_shared<GBufferWorldDrawCallback>(std::move(callback));
    std::lock_guard<std::mutex> lock(impl->worldDrawMutex);
    impl->worldDraw = std::move(replacement);
}

bool MetalR4EngineScene::HasGBufferWorldDrawCallback() const
{
    if (!impl)
        return false;
    std::lock_guard<std::mutex> lock(impl->worldDrawMutex);
    return static_cast<bool>(impl->worldDraw);
}

void MetalR4EngineScene::SubmitParticleFrame(xray::metal::particles::FrameBatch frame)
{
    if (!impl)
        impl = std::make_unique<Impl>();
    impl->particleFrame = std::move(frame);
}

void MetalR4EngineScene::SubmitRainFrame(xray::metal::rain::FrameBatch frame)
{
    if (!impl)
        impl = std::make_unique<Impl>();
    impl->rainFrame = std::move(frame);
}

void MetalR4EngineScene::SubmitLensFlareFrame(xray::metal::lensflare::Frame frame)
{
    if (!impl)
        impl = std::make_unique<Impl>();
    impl->lensFlareFrame = std::move(frame);
}

bool MetalR4EngineScene::Render(
    IR4Backend& backend,
    const ViewState& view,
    const R4EnvironmentSnapshot& environment,
    const R4GameplayVisionSnapshot& vision,
    std::string& error)
{
    if (!impl || !impl->prepared)
    {
        error = "engine scene is not prepared";
        return false;
    }
    impl->floraEnvironment = environment;
    if (!updateSceneView(backend, impl->scene, view, environment, error))
        return false;
    if (impl->scene.productionBaseLighting)
    {
        // combine_1 performs the first authored NV luminance/gain encoding;
        // the later global pass expects that encoded low target. Keep the
        // exact reflected b0 offsets in sync with the postprocess snapshot.
        if (!backend.Resources().UpdateBuffer(impl->scene.baseLightingGlobals, 240u,
                vision.shaderParam8.data(), sizeof(float) * vision.shaderParam8.size(), error) ||
            !backend.Resources().UpdateBuffer(impl->scene.baseLightingGlobals, 256u,
                vision.shaderParam7.data(), sizeof(float) * vision.shaderParam7.size(), error))
            return false;
    }
    if (impl->detailGrass && impl->detailGrass->IsReady())
    {
        impl->detailEnvironment = environment;
        if (!impl->detailSnapshotLoaded)
        {
            R4DetailGrassSnapshot snapshot;
            std::string detailError;
            if (LoadMetalR4EngineLevelDetailsSnapshot(snapshot, detailError) &&
                impl->detailGrass->SetSnapshot(snapshot, detailError))
            {
                impl->detailSnapshotLoaded = true;
                std::fprintf(stderr, "METAL_DETAIL_GRASS_SNAPSHOT models=%zu slots=%zu source=$level$/level.details\n",
                    impl->detailGrass->Snapshot()->models.size(), impl->detailGrass->Snapshot()->slots.size());
            }
            else if (impl->detailLastLoggedFrame == UINT32_MAX ||
                environment.engineFrame - impl->detailLastLoggedFrame >= 300u)
            {
                std::fprintf(stderr, "METAL_DETAIL_GRASS_SKIP phase=snapshot reason=\"%s\"\n", detailError.c_str());
                impl->detailLastLoggedFrame = environment.engineFrame;
            }
        }
        if (impl->detailSnapshotLoaded && view.valid && environment.valid)
        {
            const std::int32_t cellX = static_cast<std::int32_t>(std::floor(view.eye[0] * 0.5f + .5f));
            const std::int32_t cellZ = static_cast<std::int32_t>(std::floor(view.eye[2] * 0.5f + .5f));
            const std::uint64_t generation = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cellX)) << 32u) |
                static_cast<std::uint32_t>(cellZ);
            if (generation != impl->detailFrameGeneration)
            {
                R4DetailGrassFrame detailFrame;
                std::string detailError;
                const R4DetailGrassSnapshot* detailSnapshot = impl->detailGrass->Snapshot();
                const bool grounded = detailSnapshot &&
                    BuildMetalR4EngineGroundedDetailGrassFrame(*detailSnapshot,
                        view.eye, generation, detailFrame, detailError);
                if (!grounded || !impl->detailGrass->StageFrame(backend, detailFrame, detailError))
                {
                    std::fprintf(stderr, "METAL_DETAIL_GRASS_SKIP phase=grounded_frame reason=\"%s\"\n",
                        detailError.c_str());
                    // Do not retain a prior camera cell's instances if its new
                    // static-CDB admission fails. An empty staged frame remains
                    // retryable once level loading ends.
                    R4DetailGrassFrame emptyFrame;
                    emptyFrame.generation = generation;
                    std::string ignored;
                    impl->detailGrass->StageFrame(backend, emptyFrame, ignored);
                }
                else
                    impl->detailFrameGeneration = generation;
            }
        }
    }
    if (!updateTerrainConstants(backend, impl->scene, impl->terrain, error))
    {
        std::fprintf(stderr, "METAL_TERRAIN_REJECT phase=constants error=%s\n", error.c_str());
        impl->terrain.ready = false;
        error.clear();
    }
    if (impl->dynamicWorld && impl->dynamicWorld->IsEnabled())
    {
        R4DynamicWorldViewState dynamicView;
        dynamicView.view = view.view;
        dynamicView.projection = view.projection;
        dynamicView.valid = view.valid;
        if (!impl->dynamicWorld->Prewarm(
                backend, dynamicView, backend.EngineWorldSnapshot(), error))
            return false;
    }
    if (impl->hud && impl->hud->IsReady() && !impl->hud->Prewarm(backend, backend.EngineHudSnapshot(), error))
        return false;
    if (impl->scope && impl->scope->IsReady() &&
        !impl->scope->Prewarm(backend, backend.EngineHudSnapshot(), error))
        return false;
    if (impl->particles && impl->particles->IsReady())
    {
        if (!impl->particles->PrepareRenderTargets(backend, error) ||
            !impl->particles->StageFrame(backend.Resources(), impl->particleFrame, error))
            return false;
        impl->particleFrame = {};
    }
    if (impl->rain && impl->rain->IsReady())
    {
        if (!impl->rain->PrepareRenderTargets(backend, error) ||
            !impl->rain->StageFrame(backend.Resources(), std::move(impl->rainFrame), error))
            return false;
        impl->rainFrame = {};
    }
    if (impl->water && impl->water->IsReady())
    {
        // Menu/loading frames are presented without a loaded gameplay scene.
        // They intentionally have no combined low target, so water simply
        // waits for the first real level frame instead of poisoning startup.
        if (!impl->scene.lightingTargets.empty() && impl->scene.lightingTargets[0])
        {
            const R4Texture2DDesc& target = impl->scene.lightingTargets[0]->Descriptor();
            if (!impl->water->PrepareRenderTargets(backend, target.width, target.height,
                    target.format, error))
                return false;
        }
    }
    if (impl->ssfxAo && impl->ssfxAo->IsReady() &&
        !impl->scene.gBufferTargets.empty() && impl->scene.gBufferTargets[0])
    {
        const R4Texture2DDesc& target = impl->scene.gBufferTargets[0]->Descriptor();
        if (!impl->ssfxAo->PrepareRenderTargets(backend, target.width, target.height, error))
            return false;
    }
    std::shared_ptr<const GBufferWorldDrawCallback> worldDraw;
    {
        std::lock_guard<std::mutex> lock(impl->worldDrawMutex);
        worldDraw = impl->worldDraw;
    }
    const bool rendered = renderProductionScene(backend, impl->scene, impl->terrain, impl->sky,
        impl->defVertex.get(), impl->defObjectsLod.get(), impl->staticForward.get(), impl->ssfxAo.get(),
        impl->ssfxAoTelemetryFrame,
        impl->rain.get(),
        impl->rainTelemetryFrame,
        impl->water.get(),
        impl->localLights.get(),
        &impl->localShadows,
        impl->dynamicWorld.get(),
        impl->hud.get(),
        impl->scope.get(),
        impl->particles.get(), &impl->nightVision, view, environment, vision,
        worldDraw, error);
    if (rendered && impl->hud && impl->hud->IsReady())
    {
        const MetalR4HudAdapter::Telemetry& hud = impl->hud->LastTelemetry();
        const std::uint64_t coverage = (std::uint64_t(hud.requested) << 48u) ^
            (std::uint64_t(hud.admitted) << 32u) ^ (std::uint64_t(hud.submitted) << 16u) ^
            std::uint64_t(hud.rejected) ^ (std::uint64_t(hud.skin4Submitted) << 8u);
        if (hud.frame != impl->lastHudTelemetryFrame &&
            (coverage != impl->lastHudTelemetryCoverage ||
                hud.frame - impl->lastHudTelemetryFrame >= 300u))
        {
            const std::shared_ptr<const R4EngineHudSnapshot> capture = backend.EngineHudSnapshot();
            std::fprintf(stderr,
                "METAL_HUD_FRAME frame=%u capture=requested:%u/admitted:%u/aggregate:%u/strict:%u/scope:%u/non_hq:%u "
                "followup=distort:%u/emissive:%u/wmark:%u requested=%zu admitted=%zu submitted=%zu "
                "bump=%zu flat=%zu skin4=requested:%zu/admitted:%zu/submitted:%zu "
                "reject=no_asset:%zu/unresolved:%zu/non_skinned:%zu/skin:%zu/texture:%zu/upload:%zu/selection:%zu/"
                "skin4_raw:%zu/palette_missing:%zu/palette_mismatch:%zu/palette_capacity:%zu/bone_capacity:%zu/history:%zu\n",
                hud.frame, capture ? capture->captureRequested : 0u,
                capture ? capture->captureAdmitted : 0u,
                capture ? capture->captureAggregateFallback : 0u,
                capture ? capture->captureStrict : 0u,
                capture ? capture->captureScopeForward : 0u, capture ? capture->captureNonHq : 0u,
                capture ? capture->captureDistortFollowup : 0u,
                capture ? capture->captureEmissiveFollowup : 0u,
                capture ? capture->captureWmarkFollowup : 0u, hud.requested, hud.admitted,
                hud.submitted, hud.bumpDraws, hud.flatDraws, hud.skin4Requested,
                hud.skin4Admitted, hud.skin4Submitted, hud.rejectNoAsset,
                hud.rejectUnresolvedLinks, hud.rejectNonSkinned, hud.rejectSkin,
                hud.rejectTexture, hud.rejectUpload, hud.rejectGeometrySelection,
                hud.rejectSkin4RawAbi, hud.rejectSkin4PaletteMissing,
                hud.rejectSkin4PaletteMismatch, hud.rejectSkin4PaletteCapacity,
                hud.rejectSkin4BoneCapacity, hud.rejectSkin4History);
            impl->lastHudTelemetryFrame = hud.frame;
            impl->lastHudTelemetryCoverage = coverage;
        }
    }
    if (rendered && impl->scope && impl->scope->IsReady())
    {
        const MetalR4ScopeAdapter::Telemetry& scope = impl->scope->LastTelemetry();
        const std::uint64_t coverage = (std::uint64_t(scope.triplets) << 48u) ^
            (std::uint64_t(scope.prepared) << 32u) ^
            (std::uint64_t(scope.backDraws) << 24u) ^
            (std::uint64_t(scope.zwriteDraws) << 16u) ^
            (std::uint64_t(scope.reticleDraws) << 8u) ^ std::uint64_t(scope.rejected);
        if (scope.frame != impl->lastScopeTelemetryFrame &&
            (coverage != impl->lastScopeTelemetryCoverage ||
                scope.frame - impl->lastScopeTelemetryFrame >= 300u))
        {
            const std::shared_ptr<const R4EngineHudSnapshot> capture =
                backend.EngineHudSnapshot();
            std::fprintf(stderr,
                "METAL_3DSS_FRAME frame=%u capture=back:%u/zwrite:%u/reticle:%u/triplets:%u/partial_rejected:%u/duplicate_rejected:%u "
                "prepared=%zu submitted=back:%zu/zwrite:%zu/reticle:%zu rejected=%zu\n",
                scope.frame, capture ? capture->captureScopeBack : 0u,
                capture ? capture->captureScopeZwrite : 0u,
                capture ? capture->captureScopeReticlePrecise : 0u,
                capture ? capture->captureScopeTriplets : 0u,
                capture ? capture->captureScopePartialRejected : 0u,
                capture ? capture->captureScopeDuplicateRejected : 0u,
                scope.prepared, scope.backDraws, scope.zwriteDraws,
                scope.reticleDraws, scope.rejected);
            impl->lastScopeTelemetryFrame = scope.frame;
            impl->lastScopeTelemetryCoverage = coverage;
        }
    }
    if (rendered && impl->staticForward && impl->staticForward->IsReady())
    {
        const auto& telemetry = impl->staticForward->LastTelemetry();
        ++impl->staticForwardTelemetryFrame;
        const bool firstLiveReport = view.valid && !impl->staticForwardLiveFrustumReported;
        const bool firstGameplayReport = impl->scene.gameplayViewValid &&
            !impl->staticForwardGameplayFrustumReported;
        if (impl->staticForwardTelemetryFrame == 1u || firstLiveReport || firstGameplayReport ||
            impl->staticForwardTelemetryFrame % 300u == 0u)
        {
            std::fprintf(stderr,
                "METAL_STATIC_FORWARD_DRAW frame=%llu submitted=%zu culled=%zu "
                "lightmapped=%zu vertex_color=%zu wallmarks=%zu lightplanes=%zu "
                "live_view=%s sort=back_to_front\n",
                static_cast<unsigned long long>(impl->staticForwardTelemetryFrame),
                telemetry.submittedVisuals, telemetry.culledVisuals,
                telemetry.lightmappedVisuals, telemetry.vertexColorVisuals,
                telemetry.wallmarkVisuals, telemetry.lightplaneVisuals,
                view.valid ? "true" : "false");
            if (firstLiveReport) impl->staticForwardLiveFrustumReported = true;
            if (firstGameplayReport) impl->staticForwardGameplayFrustumReported = true;
        }
    }
    if (rendered && impl->particles && impl->particles->IsReady())
    {
        const MetalR4ParticleAdapter::Telemetry& particle = impl->particles->LastTelemetry();
        if (impl->lastParticleTelemetryFrame == UINT32_MAX ||
            environment.engineFrame - impl->lastParticleTelemetryFrame >= 300u)
        {
            std::fprintf(stderr,
                "METAL_PARTICLE_FRAME frame=%u captured=%zu prepared=%zu submitted=%zu "
                "soft=%zu hard=%zu additive=%zu alpha_add=%zu distortion=%zu distortion_submitted=%zu "
                "rejected=%zu soft_rejected=%zu "
                "hud_rejected=%zu texture_rejected=%zu dropped=%zu\n",
                environment.engineFrame, particle.capturedDraws, particle.preparedDraws,
                particle.submittedDraws, particle.softDraws, particle.hardDraws,
                particle.additiveDraws, particle.alphaAddDraws,
                particle.distortionDraws, particle.submittedDistortionDraws,
                particle.rejectedDraws, particle.softRejectedDraws,
                particle.hudRejectedDraws, particle.textureRejectedDraws,
                particle.droppedCaptureDraws);
            impl->lastParticleTelemetryFrame = environment.engineFrame;
            impl->lastParticleSubmitted = particle.submittedDraws;
        }
    }
    if (rendered && impl->localLights && impl->localLights->IsReady())
    {
        const MetalR4LocalLightAdapter::Telemetry& local = impl->localLights->LastTelemetry();
        const bool reportLocalTelemetry = impl->lastLocalLightTelemetryFrame == UINT32_MAX ||
            environment.engineFrame - impl->lastLocalLightTelemetryFrame >= 300u;
        if (reportLocalTelemetry)
        {
            std::fprintf(stderr, "METAL_LOCAL_LIGHT_FRAME frame=%u requested=%zu admitted=%zu submitted=%zu shadowed_submitted=%zu culled=%zu unavailable_skipped=%zu shadowed_skipped=%zu shadowed_point_skipped=%zu invalid_skipped=%zu cookie_skipped=%zu\n",
                environment.engineFrame, local.requested, local.admitted, local.submitted,
                local.submittedShadowed, local.culled, local.skippedUnavailable,
                local.skippedShadowed, local.skippedShadowedPoint, local.skippedInvalid,
                local.skippedCookie);
            impl->lastLocalLightTelemetryFrame = environment.engineFrame;
            impl->lastLocalLightRequested = local.requested;
            impl->lastLocalLightAdmitted = local.admitted;
            impl->lastLocalLightCulled = local.culled;
            impl->lastLocalLightShadowed = local.skippedShadowed;
        }
        if (reportLocalTelemetry)
        {
            std::fprintf(stderr,
                "METAL_LOCAL_SHADOW_FRAME frame=%u candidates=%zu rendered=%zu static_casters=%zu "
                "culled_casters=%zu dynamic_candidates=%zu dynamic_casters=%zu culled_dynamic_casters=%zu "
                "rejected_dynamic_casters=%zu dynamic_renderables_omitted=%zu tile=%u capacity=%zu\n",
                environment.engineFrame, impl->localShadows.candidates,
                impl->localShadows.renderedLights, impl->localShadows.submittedCasters,
                impl->localShadows.culledCasters,
                impl->localShadows.dynamicCandidates,
                impl->localShadows.submittedDynamicCasters,
                impl->localShadows.culledDynamicCasters,
                impl->localShadows.rejectedDynamicCasters,
                impl->localShadows.omittedDynamicCasters,
                LocalShadowTileSize, LocalShadowTileCapacity);
            impl->lastLocalShadowRendered = impl->localShadows.renderedLights;
        }
    }
    if (rendered && impl->defVertex && impl->defVertex->IsReady())
    {
        const MetalR4DefVertexAdapter::Telemetry& telemetry = impl->defVertex->LastTelemetry();
        ++impl->defVertexTelemetryFrame;
        const bool firstLiveReport = view.valid && !impl->defVertexLiveFrustumReported;
        const bool firstGameplayReport = impl->scene.gameplayViewValid &&
            !impl->defVertexGameplayFrustumReported;
        if (impl->defVertexTelemetryFrame == 1u || firstLiveReport || firstGameplayReport ||
            impl->defVertexTelemetryFrame % 300u == 0u)
        {
            std::fprintf(stderr,
                "METAL_DEF_VERTEX_DRAW frame=%llu submitted=%zu aref_submitted=%zu "
                "culled=%zu textures=%zu live_view=%s "
                "shaders=deffer_base_flat_msc,deffer_base_aref_flat_msc "
                "fail_open=nonfinite_or_uncertain\n",
                static_cast<unsigned long long>(impl->defVertexTelemetryFrame),
                telemetry.submittedVisuals, telemetry.alphaTestedSubmittedVisuals,
                telemetry.culledVisuals, telemetry.textureAssets,
                view.valid ? "true" : "false");
            if (firstLiveReport) impl->defVertexLiveFrustumReported = true;
            if (firstGameplayReport) impl->defVertexGameplayFrustumReported = true;
        }
    }
    if (rendered && impl->defObjectsLod && impl->defObjectsLod->IsReady())
    {
        const MetalR4DefObjectsLodAdapter::Telemetry& telemetry =
            impl->defObjectsLod->LastTelemetry();
        ++impl->defObjectsLodTelemetryFrame;
        const bool firstLiveReport = view.valid && !impl->defObjectsLodLiveFrustumReported;
        const bool firstGameplayReport = impl->scene.gameplayViewValid &&
            !impl->defObjectsLodGameplayFrustumReported;
        if (impl->defObjectsLodTelemetryFrame == 1u || firstLiveReport || firstGameplayReport ||
            impl->defObjectsLodTelemetryFrame % 300u == 0u)
        {
            std::fprintf(stderr,
                "METAL_DEF_OBJECTS_LOD_DRAW frame=%llu submitted=%zu culled=%zu flat=%zu bump=%zu steep=%zu "
                "textures=%zu live_view=%s shader=deffer_base_flat_or_bump_or_steep_msc "
                "fail_open=nonfinite_or_uncertain\n",
                static_cast<unsigned long long>(impl->defObjectsLodTelemetryFrame),
                telemetry.submittedVisuals, telemetry.culledVisuals, telemetry.flatVisuals,
                telemetry.bumpVisuals, telemetry.steepVisuals, telemetry.textureAssets,
                view.valid ? "true" : "false");
            if (firstLiveReport) impl->defObjectsLodLiveFrustumReported = true;
            if (firstGameplayReport) impl->defObjectsLodGameplayFrustumReported = true;
        }
    }
    return rendered;
}

bool MetalR4EngineScene::IsPrepared() const
{
    return impl && impl->prepared;
}

std::uint64_t MetalR4EngineScene::DrawableVisualCount() const
{
    return impl ? impl->scene.drawableVisuals : 0;
}

std::uint64_t MetalR4EngineScene::DrawableIndexCount() const
{
    return impl ? impl->scene.drawableIndices : 0;
}
