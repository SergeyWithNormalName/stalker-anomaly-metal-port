#include "MetalR4EngineScene.h"

#include "IR4Backend.h"
#include "R4WholeLightVisibilitySelection.h"
#include "R4StaticConstantGroups.h"
#include "MetalR4EngineFs.h"
#include "MetalR4LightHomDiagnostic.h"
#include "MetalR4FogScatteringAdapter.h"
#include "MetalR4LensFlareAdapter.h"
#include "R4LocalShadowAtlasFoundation.h"
#include "R4LocalShadowBatchPlan.h"
#include "R4LocalLightFaceVisibility.h"
#include "R4StaticShadowIndex.h"
#include "R4StaticShadowMerge.h"
#include "MetalR4VolumetricCapture.h"
#include "R4NightVisionFoundation.h"
#include "R4SubmittedViewHistory.h"
#include "R4ActiveOcclusionSceneBridge.h"
#include "R4ActiveOcclusionCatalogSelection.h"
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <new>
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

bool qualifyActiveOcclusionLocalVolumeShader(const std::filesystem::path& directory,
    const char*& reason)
{
    try
    {
        constexpr const char* expectedMetallib =
            "8c570a04d307eea059a5000eb693ed2d1e6a31f7ba1a0cf212ad961c607afc93";
        constexpr const char* expectedReflection =
            "2717f0d1347d142df19048da76fd04852f9a67cbb415581cfa21e80ba6b07a1c";
        const auto metallibPath = directory / "accum_volume.vs.metallib";
        const auto reflectionPath = directory / "accum_volume.vs.json";
        const auto receiptPath = directory / "local-lights-msc-receipt.txt";
        std::error_code ec;
        const std::uintmax_t metallibSize = std::filesystem::file_size(metallibPath, ec);
        if (ec || metallibSize == 0u || metallibSize > 1024u * 1024u)
        { reason = "local-volume-metallib-unreadable-or-oversized"; return false; }
        ec.clear();
        const std::uintmax_t reflectionSize = std::filesystem::file_size(reflectionPath, ec);
        if (ec || reflectionSize == 0u || reflectionSize > 64u * 1024u)
        { reason = "local-volume-reflection-unreadable-or-oversized"; return false; }
        ec.clear();
        const std::uintmax_t receiptSize = std::filesystem::file_size(receiptPath, ec);
        if (ec || receiptSize == 0u || receiptSize > 64u * 1024u)
        { reason = "local-volume-receipt-unreadable-or-oversized"; return false; }

        std::string metallibHash, reflectionHash;
        if (!fileSha256(metallibPath, metallibHash) || metallibHash != expectedMetallib ||
            !fileSha256(reflectionPath, reflectionHash) || reflectionHash != expectedReflection)
        { reason = "local-volume-artifact-identity-mismatch"; return false; }

        std::string receipt;
        if (!readActiveOcclusionTextFileBounded(receiptPath, 64u * 1024u, receipt))
        { reason = "local-volume-receipt-unreadable-or-oversized"; return false; }
        if (receipt.find("source_tree_sha256=bc502abe6276487148978a7423c102079d7120a12ed8359c87eff7f1047b8494") == std::string::npos ||
            receipt.find("msc_version=metal-irconverter version: 4.0.1") == std::string::npos ||
            receipt.find("entry_point=main") == std::string::npos ||
            receipt.find("common_define_args=-D SMAP_size=2048 -D FP16_FILTER=1 -D FP16_BLEND=1 -D USE_HWSMAP=1 -D USE_HWSMAP_PCF=1 -D USE_BRANCHING=1 -D USE_VTF=1 -D SM_5=1 -D SSFX_INT_GRASS=8 -D SSFX_SSR_QUALITY=0 -D SSFX_WATER_QUALITY=1 -D SSFX_WATER_PARALLAX=2 -D SSFX_IL_QUALITY=16 -D SSFX_AO_QUALITY=2 -D SSFX_POM_REFINE=0 -D SSFX_TERRA_POM_REFINE=0 -D SSFX_SSS_DIR_QUALITY=6 -D SSFX_SSS_OMNI_QUALITY=2 -D SSFX_MODEXE=1") == std::string::npos ||
            receipt.find("output=accum_volume.vs\tdxil_sha256=1a6b903e514652d9229d84c343bd08a2841f90e1400304244f3fb70bf6bd28b8\tmetallib_sha256=8c570a04d307eea059a5000eb693ed2d1e6a31f7ba1a0cf212ad961c607afc93\treflection_sha256=2717f0d1347d142df19048da76fd04852f9a67cbb415581cfa21e80ba6b07a1c") == std::string::npos)
        { reason = "local-volume-source-or-receipt-identity-mismatch"; return false; }
    }
    catch (...)
    {
        reason = "local-volume-qualification-failed";
        return false;
    }
    reason = nullptr;
    return true;
}

// E07 CPU encoding attribution. These intervals include nested caster work;
// they are not GPU durations and must not be added to scene_ms.
struct E07ShadowCpuScope
{
    MetalR4DynamicWorldAdapter* adapter = nullptr;
    bool local = false;
    std::chrono::steady_clock::time_point start{};
    E07ShadowCpuScope(MetalR4DynamicWorldAdapter* owner, bool localShadow)
        : local(localShadow)
    {
        const char* path = std::getenv("XRAY_METAL_E07_FRAMES");
        if (owner && path && *path)
        {
            adapter = owner;
            start = std::chrono::steady_clock::now();
        }
    }
    ~E07ShadowCpuScope()
    {
        if (adapter)
        {
            const double ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
            adapter->AddE07ShadowCpu(local ? ms : 0.0, local ? 0.0 : ms);
        }
    }
};

// E06 bundles must admit the artifact bytes as well as the reflected layout.
// Input-tree and define hashes remain in the receipted cook evidence; runtime
// checks the exact scope/compiler plus every artifact it will load.
static bool validateE06Receipt(const std::filesystem::path& directory,
    const char* receiptName, const char* scope,
    std::initializer_list<const char*> shaders, std::string& error)
{
    const auto path = (directory / receiptName).string();
    NSData* data = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:path.c_str()]];
    id decoded = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nullptr] : nil;
    if (![decoded isKindOfClass:[NSDictionary class]])
    { error = "E06 shader receipt missing or invalid: " + path; return false; }
    NSDictionary* receipt = decoded;
    NSString* expectedScope = [NSString stringWithUTF8String:scope];
    if (![receipt[@"scope"] isEqual:expectedScope] ||
        ![receipt[@"msc_version"] isKindOfClass:[NSString class]] ||
        ![receipt[@"msc_version"] hasPrefix:@"metal-irconverter version: 4.0.1"] ||
        ![receipt[@"dxc_version"] isKindOfClass:[NSString class]] ||
        ![receipt[@"dxc_version"] hasPrefix:@"libdxcompiler.dylib: 1.9"] ||
        ![receipt[@"inputs"] isKindOfClass:[NSDictionary class]] ||
        ![receipt[@"defines"] isKindOfClass:[NSArray class]] ||
        ![receipt[@"outputs"] isKindOfClass:[NSDictionary class]])
    { error = "E06 shader receipt scope/compiler contract mismatch: " + path; return false; }
    for (id define in receipt[@"defines"])
        if (![define isKindOfClass:[NSString class]] || [define hasPrefix:@"USE_MSAA"])
        { error = "E06 bundle must preserve the active non-MSAA defines"; return false; }
    for (const char* shader : shaders)
    {
        NSDictionary* artifacts = receipt[@"outputs"][[NSString stringWithUTF8String:shader]];
        if (![artifacts isKindOfClass:[NSDictionary class]])
        { error = std::string("E06 receipt omits shader: ") + shader; return false; }
        for (const char* extension : {".dxil", ".metallib", ".reflection.json"})
        {
            NSString* expected = artifacts[[NSString stringWithUTF8String:extension]];
            std::string actual;
            if (![expected isKindOfClass:[NSString class]] ||
                !fileSha256(directory / (std::string(shader) + extension), actual) ||
                actual != [expected UTF8String])
            { error = std::string("E06 artifact hash mismatch: ") + shader + extension; return false; }
        }
    }
    return true;
}

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
    struct FloraPixelOracle
    {
        std::array<std::shared_ptr<IR4Texture>, 2> maps;
        std::shared_ptr<IR4Texture> depth;
        std::size_t pairs = 0u;
        std::size_t pairsThisFrame = 0u;
        std::size_t viewsThisFrame = 0u;
        std::size_t firstViewThisFrame = 0u;
        std::uint32_t lastFrame = UINT32_MAX;
        std::unordered_set<std::string> sampledViews;
    } floraLocalOracle, floraSunOracle;
    std::shared_ptr<IR4Texture> atlas;
    std::shared_ptr<IR4Texture> depth;
    std::vector<xray::metal::R4LocalShadowAtlasRecord> records;
    xray::metal::R4StaticShadowIndex staticIndex;
    bool staticLinear = std::getenv("XRAY_METAL_E08_STATIC_LINEAR") != nullptr;
    bool sunLinear = std::getenv("XRAY_METAL_E08_SUN_LINEAR") != nullptr;
    bool sunOracle = std::getenv("XRAY_METAL_E08_SUN_ORACLE") != nullptr;
    std::vector<std::size_t> sunCasters;
    std::array<std::shared_ptr<IR4Texture>, 3> sunPixelMaps;
    std::size_t sunPixelPairs = 0;
    struct StaticMergeEntry
    {
        std::vector<std::size_t> materials;
        xray::metal::R4StaticShadowMerge::Result plan;
        std::shared_ptr<IR4Buffer> indices;
    };
    bool mergeStatic = std::getenv("XRAY_METAL_E08_MERGE_STATIC_SHADOWS_OFF") == nullptr;
    std::vector<std::vector<std::uint16_t>> mergeSourceIndices;
    std::unordered_map<std::uint64_t, std::vector<std::shared_ptr<StaticMergeEntry>>> mergeCache;
    std::size_t mergeCacheBytes = 0;
    std::size_t mergedEncodedDraws = 0;
    bool mergeCacheSaturated = false;
    std::shared_ptr<IR4Texture> mergePixelMap;
    std::size_t mergePixelPairs = 0;
    bool staticOracle = std::getenv("XRAY_METAL_E08_STATIC_ORACLE") != nullptr;
    std::size_t oracleViews = 0;
    std::size_t candidates = 0;
    std::size_t renderedLights = 0;
    std::size_t submittedCasters = 0;
    std::size_t culledCasters = 0;
    std::size_t dynamicCandidates = 0;
    std::size_t submittedDynamicCasters = 0;
    std::size_t culledDynamicCasters = 0;
    std::size_t rejectedDynamicCasters = 0;
    std::size_t omittedDynamicCasters = 0;
    std::size_t batches = 0;
    std::size_t transformUploadBytes = 0;
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

bool wholeLightVisibilityEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("XRAY_METAL_WHOLE_LIGHT_VISIBILITY");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

std::uint32_t wholeLightVisibilityFirstFrame()
{
    static const std::uint32_t firstFrame = [] {
        std::uint32_t parsedFrame = 300u;
        const char* value = std::getenv("XRAY_METAL_WHOLE_LIGHT_VISIBILITY_START_FRAME");
        if (value && *value && !parseUnsigned(value, parsedFrame))
        {
            std::fprintf(stderr, "METAL_WHOLE_LIGHT_VIS status=disabled reason=invalid_start_frame\n");
            return UINT32_MAX;
        }
        return parsedFrame;
    }();
    return firstFrame;
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
    bool pendingUploaded = false;
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
    // SSR still needs the authored sky cubemaps for its gloss pass when the
    // optional sky2 draw bundle itself is not configured.
    sky.ddsBytesProvider = ddsBytesProvider;
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
        const Matrix4 previousWorldViewProjection = multiply(scene.previousViewProjection, draw.modelTransform);
        std::memcpy(vertexGlobals.data(), &previousWorldViewProjection, sizeof(Matrix4));
        std::memcpy(vertexGlobals.data() + 64u, &scene.previousViewProjection,
            sizeof(Matrix4));
        std::memcpy(vertexTransforms.data(), &worldViewProjection, sizeof(Matrix4));
        std::memcpy(vertexTransforms.data() + 64u, &worldView, sizeof(Matrix4));
        std::memcpy(vertexTransforms.data() + 128u, &draw.modelTransform, sizeof(Matrix4));
        const float materialClass[4] = {0.0f, 0.0f, 0.0f, 0.25f};
        std::memcpy(vertexTransforms.data() + 192u, materialClass, sizeof(materialClass));

        std::memcpy(fragmentGlobals.data(), &previousWorldViewProjection, sizeof(Matrix4));
        std::memcpy(fragmentGlobals.data() + 64u, &scene.previousViewProjection,
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
        const float fallbackVerticalTan = -std::tan(55.0f * 3.14159265358979323846f / 360.0f);
        const float fallbackHorizontalTan = -fallbackVerticalTan * (16.0f / 9.0f);
        const float projectionX = scene.projection.elements[0];
        const float projectionY = scene.projection.elements[5];
        const float horizontalTan = std::isfinite(projectionX) && std::fabs(projectionX) > 1.0e-6f ?
            1.0f / projectionX : fallbackHorizontalTan;
        const float verticalTan = std::isfinite(projectionY) && std::fabs(projectionY) > 1.0e-6f ?
            -1.0f / projectionY : fallbackVerticalTan;
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
    if (!terrain.ready || terrain.uploaded || terrain.pendingUploaded)
        return true;
    terrain.uploadedBytes = 0u;
    for (TerrainTextureAsset& asset : terrain.textures)
        asset.uploadedBytes = 0u;
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
    }
    terrain.pendingUploaded = true;
    std::fprintf(stderr, "METAL_TERRAIN_UPLOAD_READY draws=%zu textures=%zu bytes=%llu\n",
        terrain.draws.size(), terrain.textures.size(),
        static_cast<unsigned long long>(terrain.uploadedBytes));
    return true;
}

void commitTerrainSceneUploads(TerrainScene& terrain)
{
    if (!terrain.pendingUploaded)
        return;
    terrain.uploaded = true;
    for (TerrainTextureAsset& asset : terrain.textures)
    {
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
    terrain.pendingUploaded = false;
}

void discardTerrainSceneUploads(TerrainScene& terrain)
{
    terrain.pendingUploaded = false;
    terrain.uploadedBytes = 0u;
    for (TerrainTextureAsset& asset : terrain.textures)
        asset.uploadedBytes = 0u;
}

bool drawTerrainScene(IR4Backend& backend, const LevelScene& scene, TerrainScene& terrain, std::string& error)
{
    if (!terrain.ready || (!terrain.uploaded && !terrain.pendingUploaded))
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
    scene.previousViewProjection = scene.uniformData.viewProjection;
    if (state.previousValid && !state.cameraCut)
    {
        Matrix4 previousView, previousProjection;
        std::memcpy(previousView.elements, state.previousView.data(), sizeof(previousView.elements));
        std::memcpy(previousProjection.elements, state.previousProjection.data(), sizeof(previousProjection.elements));
        scene.previousViewProjection = multiply(previousProjection, previousView);
    }

    IR4Resources& resources = backend.Resources();
    if (!scene.productionMsc)
        return resources.UpdateBuffer(
            scene.uniforms, 0, &scene.uniformData, sizeof(scene.uniformData), error);

    const bool captureActiveOcclusionConstants = scene.activeOcclusionEnabled &&
        state.valid && activeOcclusionFrameInWindow(state.engineFrame) &&
        scene.activeOcclusionObservation.scopeId != 0u &&
        scene.activeOcclusionObservation.engineFrame == state.engineFrame &&
        !scene.activeOcclusionObservation.complete &&
        !scene.activeOcclusionObservation.failed;
    if (!scene.staticConstantGroupsEnabled)
    {
        for (LevelScene::TextureMaterial& material : scene.textureMaterials)
        {
            if (captureActiveOcclusionConstants && material.activeOcclusionTriangleCount != 0u)
            {
                material.activeOcclusionConstantsValid = false;
                material.activeOcclusionConstantsScopeId = 0u;
                material.activeOcclusionConstantsFrame = UINT32_MAX;
            }
            std::array<std::uint8_t, 160> vertexGlobals{};
            std::array<std::uint8_t, 256> transforms{};
            const Matrix4 worldViewProjection = multiply(
                scene.uniformData.viewProjection, material.modelTransform);
            const Matrix4 worldView = multiply(scene.view, material.modelTransform);
            const Matrix4 previousWorldViewProjection = multiply(scene.previousViewProjection, material.modelTransform);
            std::memcpy(vertexGlobals.data(), &previousWorldViewProjection, sizeof(Matrix4));
            std::memcpy(vertexGlobals.data() + 64,
                &scene.previousViewProjection, sizeof(Matrix4));
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
            if (captureActiveOcclusionConstants && material.activeOcclusionTriangleCount != 0u)
            {
                // Keep the exact bytes submitted to the two shader constant buffers.
                std::memcpy(material.activeOcclusionUploadedWvp.data(), transforms.data(),
                    sizeof(material.activeOcclusionUploadedWvp));
                std::memcpy(material.activeOcclusionUploadedJitter.data(), vertexGlobals.data() + 128u,
                    sizeof(material.activeOcclusionUploadedJitter));
                material.activeOcclusionConstantsScopeId =
                    scene.activeOcclusionObservation.scopeId;
                material.activeOcclusionConstantsFrame = state.engineFrame;
                material.activeOcclusionConstantsValid = true;
            }
        }
    }
    else
    {
        const auto invalidateActiveConstants = [&scene, captureActiveOcclusionConstants]() {
            if (!captureActiveOcclusionConstants)
                return;
            for (LevelScene::TextureMaterial& material : scene.textureMaterials)
            {
                if (material.activeOcclusionTriangleCount == 0u)
                    continue;
                xray::metal::InvalidateR4StaticConstantOcclusionStamp(
                    material.activeOcclusionConstantsScopeId,
                    material.activeOcclusionConstantsFrame,
                    material.activeOcclusionConstantsValid);
            }
        };
        const bool groupedUpdateSucceeded = xray::metal::ApplyR4StaticConstantGroupUpdates(
            scene.staticConstantGroupPlan,
            captureActiveOcclusionConstants,
            [&scene](const std::size_t, const std::size_t representative,
                xray::metal::R4StaticConstantPayload& payload) {
                const LevelScene::TextureMaterial& material = scene.textureMaterials[representative];
                payload = xray::metal::BuildR4StaticConstantFramePayload(
                    material.modelTransform, scene.uniformData.viewProjection, scene.view,
                    scene.previousViewProjection, multiply);
                return true;
            },
            [&scene, &resources, &error](const std::size_t groupIndex,
                const xray::metal::R4StaticConstantPayload& payload) {
                const LevelScene::StaticConstantBufferGroup& group =
                    scene.staticConstantBufferGroups[groupIndex];
                return resources.UpdateBuffer(group.mscVertexGlobals, 0,
                           payload.vertexGlobals.data(), payload.vertexGlobals.size(), error) &&
                    resources.UpdateBuffer(group.mscVertexTransforms, 0,
                           payload.vertexTransforms.data(), payload.vertexTransforms.size(), error) &&
                    resources.UpdateBuffer(group.mscFragmentTransforms, 0,
                           payload.fragmentTransforms.data(), payload.fragmentTransforms.size(), error);
            },
            [&scene, &state, captureActiveOcclusionConstants](const std::size_t materialIndex,
                const std::size_t, const xray::metal::R4StaticConstantPayload& payload) {
                LevelScene::TextureMaterial& material = scene.textureMaterials[materialIndex];
                if (!captureActiveOcclusionConstants || material.activeOcclusionTriangleCount == 0u)
                    return;
                xray::metal::PublishR4StaticConstantOcclusionStamp(
                    material.activeOcclusionUploadedWvp,
                    material.activeOcclusionUploadedJitter,
                    material.activeOcclusionConstantsScopeId,
                    material.activeOcclusionConstantsFrame,
                    material.activeOcclusionConstantsValid,
                    payload, scene.activeOcclusionObservation.scopeId, state.engineFrame);
            },
            invalidateActiveConstants);
        if (!groupedUpdateSucceeded)
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
    // The index has exactly the level-scene lifetime; Prepare recreates both
    // materials and this owner. Animation and camera visibility are not keys.
    for (std::size_t i = 0; i < scene.textureMaterials.size(); ++i)
    {
        const auto& m = scene.textureMaterials[i];
        shadows.staticIndex.Add(m.localPositionBounds.available, m.localPositionBounds.minimum,
            m.localPositionBounds.maximum, m.modelTransform.elements, i);
    }
    shadows.staticIndex.Finish();
    if (shadows.mergeStatic || std::getenv("XRAY_METAL_E08_STATIC_MERGE_PIXEL_ORACLE_DIR"))
    {
        shadows.mergeSourceIndices.resize(scene.textureMaterials.size());
        for (std::size_t i = 0; i < scene.textureMaterials.size(); ++i)
        {
            const auto& m = scene.textureMaterials[i];
            if (m.indexBufferId >= scene.geometry.indexBuffers.size())
            { error = "E08 static merge source index buffer unavailable"; return false; }
            const auto& source = scene.geometry.indexBuffers[m.indexBufferId];
            const std::size_t offset = source.dataOffset + std::size_t(m.indexBase) * 2u;
            const std::size_t bytes = std::size_t(m.indexCount) * 2u;
            if (m.indexBase > source.indexCount || m.indexCount > source.indexCount - m.indexBase ||
                offset > scene.geometryBytes.size() || bytes > scene.geometryBytes.size() - offset)
            { error = "E08 static merge source index span exceeds geometry"; return false; }
            auto& indices = shadows.mergeSourceIndices[i];
            indices.resize(m.indexCount);
            if (bytes) std::memcpy(indices.data(), scene.geometryBytes.data() + offset, bytes);
        }
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
    shadows.ready = shadows.atlas && shadows.depth;
    if (!shadows.ready && error.empty())
        error = "local shadow atlas resource allocation failed";
    return shadows.ready;
}

R4FloraViewState makeFloraView(const MetalR4EngineScene::ViewState& view,
    const R4EnvironmentSnapshot& environment, const R4DetailGrassMotionSnapshot& motion,
    const std::shared_ptr<IR4Buffer>& staticGlobals)
{
    R4FloraViewState result;
    result.view=view.view; result.projection=view.projection;
    result.previousView=view.previousView; result.previousProjection=view.previousProjection;
    result.eye=view.eye; result.staticGlobals=staticGlobals;
    result.timeSeconds=environment.globalTime; result.treeScaleBiasCorrection=environment.treeScaleBiasCorrection;
    result.previousBenders=motion.previousBenders; result.currentBenders=motion.currentBenders;
    result.bendersSetup=motion.bendersSetup; result.windParams=motion.windParams;
    result.windSetupGrass=motion.windSetupGrass; result.windSetupTrees=motion.windSetupTrees;
    result.windAnimation=motion.windAnimation; result.windAnimationPrevious=motion.windAnimationPrevious;
    result.windValid=motion.valid; result.valid=view.valid; result.previousValid=view.previousValid;
    return result;
}
R4DetailGrassViewState makeDetailView(const MetalR4EngineScene::ViewState& view,
    const R4EnvironmentSnapshot& environment)
{
    R4DetailGrassViewState result;
    result.view=view.view; result.projection=view.projection;
    result.previousView=view.previousView; result.previousProjection=view.previousProjection;
    result.eye=view.eye; result.timeSeconds=environment.globalTime;
    result.ambient=environment.ambient; result.hemiColor=environment.hemiColor;
    result.sunColor=environment.adaptedSunColor; result.sunDirection=environment.adaptedSunDirection;
    result.fogColor=environment.fogColor; result.fogDensity=environment.fogDensity;
    result.fogNear=environment.fogNear; result.fogFar=environment.fogFar;
    result.valid=view.valid&&environment.valid; result.previousValid=view.previousValid;
    return result;
}
bool sphereOutsideMainView(const std::array<float,3>& center, float radius, const Matrix4& vp)
{
    for (unsigned plane=0;plane<5;++plane)
    {
        const unsigned row=plane<4 ? plane/2 : 2;
        const float sign=plane<4 && plane%2==0 ? 1.0f : -1.0f;
        float normal[3];
        for (unsigned i=0;i<3;++i) normal[i]=vp.elements[3+4*i]+sign*vp.elements[row+4*i];
        const float d=vp.elements[15]+sign*vp.elements[row+12];
        const float distance=normal[0]*center[0]+normal[1]*center[1]+normal[2]*center[2]+d;
        if (distance < -radius*std::sqrt(normal[0]*normal[0]+normal[1]*normal[1]+normal[2]*normal[2])) return true;
    }
    return false;
}

bool planLocalShadowBatches(const MetalR4EngineScene::ViewState& view,
    const std::shared_ptr<const R4EngineLocalLightSnapshot>& snapshot,
    const R4EnvironmentSnapshot& environment,
    std::vector<std::vector<std::size_t>>& batches, std::vector<std::size_t>& unshadowed,
    std::string& error)
{
    batches.clear(); unshadowed.clear();
    if (!view.valid || !snapshot || snapshot->engineFrame == UINT32_MAX) return true;
    Matrix4 cameraView{}, cameraProjection{};
    std::memcpy(cameraView.elements, view.view.data(), sizeof(cameraView));
    std::memcpy(cameraProjection.elements, view.projection.data(), sizeof(cameraProjection));
    const Matrix4 viewProjection=multiply(cameraProjection,cameraView);
    std::vector<xray::metal::R4LocalShadowBatchCandidate> candidates;
    std::unordered_set<std::uint64_t> visibleParents;
    for (std::size_t index=0;index<snapshot->lights.size();++index)
    {
        const auto& light=snapshot->lights[index];
        if (!light.shadowed) { unshadowed.push_back(index); continue; }
        if (!light.active) continue;
        const float length2=light.direction[0]*light.direction[0]+light.direction[1]*light.direction[1]+light.direction[2]*light.direction[2];
        if (light.type==R4EngineLocalLightType::Point || !std::isfinite(light.range) ||
            !std::isfinite(light.cone) || !std::isfinite(light.virtualSize) ||
            light.range<=light.virtualSize || light.virtualSize<=0 || light.cone<=0 ||
            light.cone>=3.0805f || !std::isfinite(length2) || length2<=1.e-8f)
        {
            std::fprintf(stderr, "METAL_E08_INVALID_SHADOW_LIGHT frame=%u id=%llu parent=%llu type=%u range=%.9g cone=%.9g virtual=%.9g dir=%.9g,%.9g,%.9g active=%d\n",
                snapshot->engineFrame, (unsigned long long)light.lightId, (unsigned long long)light.parentLightId,
                static_cast<unsigned>(light.type), light.range, light.cone, light.virtualSize,
                light.direction[0], light.direction[1], light.direction[2], light.active);
            error="invalid captured shadowed local light"; return false;
        }
        const bool omni=light.type==R4EngineLocalLightType::OmniFace;
        candidates.push_back({index,light.lightId,light.parentLightId,light.omniFace,omni});
        xray::metal::R4PositionBounds bounds;
        bounds.available=true;
        bounds.minimum=omni?std::array<float,3>{-1,-1,-1}:std::array<float,3>{-.5f,-.5f,0};
        bounds.maximum=omni?std::array<float,3>{1,1,1}:std::array<float,3>{.5f,.5f,1};
        const Matrix4 volume=makeSpotVolumeModel(light);
        if (!finiteMatrix(volume)) { error="nonfinite local shadow volume"; return false; }
        if (!conservativelyOutsideMetalFrustum(bounds,volume,viewProjection,true))
            visibleParents.insert(omni?light.parentLightId:light.lightId);
    }
    // Validate the complete source snapshot before visibility filtering. A
    // parent enters with all six faces when any of its volumes can contribute.
    std::vector<std::vector<std::size_t>> validated;
    if (!xray::metal::PlanR4LocalShadowBatches(candidates,validated,error)) return false;
    candidates.erase(std::remove_if(candidates.begin(),candidates.end(),[&](const auto& c) {
        return !visibleParents.count(c.omni?c.parentId:c.lightId);
    }),candidates.end());
    if (!xray::metal::PlanR4LocalShadowBatches(candidates,batches,error)) return false;
    static const bool faceCull=[] {
        const char* value=std::getenv("XRAY_METAL_LOCAL_LIGHT_FACE_CULL");
        return value && !std::strcmp(value,"1");
    }();
    // Preserve the old complete-six-face witness contract when that diagnostic
    // is requested. Pixel parity for face culling uses the color-phase capture.
    const char* batchProbe=std::getenv("XRAY_METAL_LOCAL_SHADOW_BATCH_PROBE_DIR");
    if (faceCull && batchProbe && *batchProbe)
    {
        static bool logged=false;
        if (!logged) std::fprintf(stderr,"METAL_LOCAL_LIGHT_FACE_CULL status=bypassed reason=complete_face_batch_witness\n");
        logged=true;
        return true;
    }
    if (!faceCull || !environment.valid ||
        environment.engineFrame!=view.engineFrame || snapshot->engineFrame!=view.engineFrame ||
        view.engineFrame==UINT32_MAX || !finiteMatrix(viewProjection) ||
        !std::isfinite(snapshot->shadowDepthScale) || !std::isfinite(snapshot->shadowDepthBias)) return true;
    std::unordered_set<std::uint64_t> exemptParents;
    for (const auto& light:snapshot->lights)
        if (light.type==R4EngineLocalLightType::OmniFace &&
            !xray::metal::R4LocalLightFaceMayBeCulled(light,environment))
            exemptParents.insert(light.parentLightId);
    std::array<float,16> vp{};
    std::memcpy(vp.data(),viewProjection.elements,sizeof(viewProjection));
    const Matrix4 inverseView=inverseRigidTransform(cameraView);
    if (!finiteMatrix(inverseView)) return true;
    std::size_t before=0, eligible=0, exempt=0, unknown=0, removed=0;
    for (auto& batch:batches)
    {
        before+=batch.size();
        batch.erase(std::remove_if(batch.begin(),batch.end(),[&](std::size_t index) {
            const auto& light=snapshot->lights[index];
            if (light.type!=R4EngineLocalLightType::OmniFace) return false;
            if (exemptParents.count(light.parentLightId)) { ++exempt; return false; }
            ++eligible;
            std::array<float,3> minimum{},maximum{};
            if (!BuildMetalR4LocalLightVolumeBounds(light,minimum,maximum)) { ++unknown; return false; }
            if (!xray::metal::R4LocalLightFaceBoundsOutsideView(minimum,maximum,vp)) return false;
            // Culling must not hide a malformed shadow source. Check the same
            // projection/derived-matrix admission used by its tile; uncertainty
            // retains the record so the normal renderer reports the error.
            const Matrix4 lightView=makeR4SpotLightView(light);
            const Matrix4 lightProjection=makeR4SpotLightProjection(light);
            if (!light.lightId || !finiteMatrix(lightView) || !finiteMatrix(lightProjection) ||
                !finiteMatrix(multiply(lightProjection,lightView))) { ++unknown; return false; }
            xray::metal::R4LocalShadowAtlasRecord proof;
            proof.atlasSize=LocalShadowTileSize; proof.atlasExtent=LocalShadowAtlasExtent;
            std::memcpy(proof.lightView.data(),lightView.elements,sizeof(lightView));
            std::memcpy(proof.lightProjection.data(),lightProjection.elements,sizeof(lightProjection));
            std::memcpy(proof.deviceInverseView.data(),inverseView.elements,sizeof(inverseView));
            std::string proofError;
            if (!xray::metal::DeriveR4LocalShadowMatrix(proof,snapshot->shadowDepthScale,
                    snapshot->shadowDepthBias,proof.shadowMatrix,proofError) ||
                !xray::metal::DeriveR4LocalLightMapMatrix(proof,snapshot->shadowDepthScale,
                    snapshot->shadowDepthBias,proof.lightMapMatrix,proofError)) { ++unknown; return false; }
            ++removed;
            return true;
        }),batch.end());
    }
    batches.erase(std::remove_if(batches.begin(),batches.end(),[](const auto& batch) {
        return batch.empty();
    }),batches.end());
    // A removed record cannot produce accumulator fragments. Its complete
    // shadow tile and all caster families are consequently unnecessary; caster
    // admission for the remaining records still uses each light's own frustum.
    if (view.engineFrame%120u==0u)
        std::fprintf(stderr,"METAL_LOCAL_LIGHT_FACE_CULL frame=%u before=%zu eligible=%zu exempt=%zu unknown=%zu removed=%zu remaining=%zu batches=%zu\n",
            view.engineFrame,before,eligible,exempt,unknown,removed,before-removed,batches.size());
    return true;
}

struct ActiveOcclusionGridStats
{
    R4ActiveOcclusion::BuildStats build{};
    std::uint64_t scopeId = 0u;
    std::size_t drawsUsed = 0u;
    std::size_t drawsSkipped = 0u;
    std::size_t receiptsSeen = 0u;
    std::size_t selectedDraws = 0u;
    std::uint64_t receiptTriangles = 0u;
    std::uint64_t selectedTriangles = 0u;
    std::uint64_t omittedReceiptTriangles = 0u;
    std::size_t selectorInvalidReceipts = 0u;
    std::size_t selectorDuplicateReceipts = 0u;
    std::size_t selectorUnknownProjection = 0u;
    std::size_t selectorOutsideViewport = 0u;
    std::size_t selectorBudgetOmissions = 0u;
    std::uint32_t selectionBudget = 0u;
    std::size_t sourcePairs = 0u;
    bool ready = false;
    bool sourceTriangleCap = false;
    const char* reason = "not-attempted";
    double buildMs = 0.0;
    double queryMs = 0.0;
    std::uint64_t queryCellTests = 0u;
    std::size_t inputTriangles = 0u;
    bool queryCellCap = false;
    bool queryInvalid = false;
};

bool buildActiveOcclusionGrid(LevelScene& scene,
    const std::shared_ptr<const R4EngineLocalLightSnapshot>& snapshot,
    const std::vector<R4WholeLightVisibilitySample>& samples,
    R4ActiveOcclusion::CoverageGrid& grid, ActiveOcclusionGridStats& result)
{
    const auto buildStart = std::chrono::steady_clock::now();
    const auto& observation = scene.activeOcclusionObservation;
    if (!snapshot || samples.empty() || !observation.scopeId || !observation.complete ||
        observation.failed || observation.engineFrame != snapshot->engineFrame ||
        observation.viewportWidth == 0u || observation.viewportHeight == 0u)
    {
        result.reason = "scope-or-selection-unavailable";
        result.buildMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - buildStart).count();
        return false;
    }

    result.scopeId = observation.scopeId;
    try
    {
        using namespace xray::metal::active_occlusion_catalog;
        SelectionResult selection;
        if (!SelectCurrentView(scene.activeOcclusionSourceCatalog,
                observation.admittedDraws.data(), observation.admittedDraws.size(),
                observation.scopeId, observation.engineFrame,
                observation.viewportWidth, observation.viewportHeight,
                ViewportYDirection::ClipPositiveYMapsToLowerPixelY, selection,
                kStartingTriangleBudget, kMaximumDrawReceipts))
        {
            result.reason = "catalog-current-view-selection-failed";
            result.buildMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - buildStart).count();
            return false;
        }
        result.receiptsSeen = observation.admittedDraws.size();
        result.selectedDraws = selection.stats.receiptsSelected;
        result.selectedTriangles = selection.stats.selectedTriangles;
        result.selectionBudget = selection.stats.effectiveTriangleBudget;
        result.selectorInvalidReceipts = selection.stats.invalidReceipt;
        result.selectorDuplicateReceipts = selection.stats.duplicateReceipt;
        result.selectorUnknownProjection = selection.stats.unknownProjection;
        result.selectorOutsideViewport = selection.stats.outsideViewport;
        result.selectorBudgetOmissions = selection.stats.omittedByTriangleBudget;
        for (const auto& receipt : observation.admittedDraws)
            result.receiptTriangles += receipt.triangleCount;

        std::vector<R4ActiveOcclusionBridge::SourceTriangle> source;
        source.reserve(static_cast<std::size_t>(selection.stats.selectedTriangles));
        struct PreparedDraw
        {
            const DrawReceipt* receipt = nullptr;
            std::size_t firstSourceTriangle = 0u;
            std::uint32_t triangleCount = 0u;
        };
        std::vector<PreparedDraw> prepared;
        prepared.reserve(selection.draws.size());
        for (const SelectedDraw& chosen : selection.draws)
        {
            if (chosen.receiptIndex >= observation.admittedDraws.size())
            {
                ++result.drawsSkipped;
                continue;
            }
            const DrawReceipt& receipt = observation.admittedDraws[chosen.receiptIndex];
            const VisualRange* range = scene.activeOcclusionSourceCatalog.Find(chosen.visualId);
            if (receipt.drawId != chosen.drawId || receipt.visualId != chosen.visualId ||
                !range || range->firstTriangle != chosen.firstTriangle ||
                range->triangleCount != chosen.triangleCount ||
                source.size() > selection.stats.selectedTriangles ||
                chosen.triangleCount > selection.stats.selectedTriangles - source.size())
            {
                ++result.drawsSkipped;
                continue;
            }
            const std::size_t firstSourceTriangle = source.size();
            const auto& catalogTriangles = scene.activeOcclusionSourceCatalog.Triangles();
            for (std::uint32_t i = 0u; i != chosen.triangleCount; ++i)
            {
                const SourceTriangle& input = catalogTriangles[range->firstTriangle + i];
                R4ActiveOcclusionBridge::SourceTriangle output;
                output.sourceTriangleId = input.triangleId;
                output.mateTriangleId = input.mateTriangleId;
                for (unsigned corner = 0u; corner != 3u; ++corner)
                {
                    output.sourceVertexIds[corner] = input.sourceVertexIds[corner];
                    for (unsigned axis = 0u; axis != 3u; ++axis)
                        output.positions[corner][axis] = input.positions[corner][axis];
                }
                source.emplace_back(output);
            }
            prepared.push_back({&receipt, firstSourceTriangle, chosen.triangleCount});
        }
        std::vector<R4ActiveOcclusion::Triangle> triangles;
        triangles.reserve(source.size());
        for (const PreparedDraw& item : prepared)
        {
            const DrawReceipt& receipt = *item.receipt;
            if (receipt.scopeId != observation.scopeId ||
                receipt.engineFrame != observation.engineFrame ||
                item.triangleCount == 0u ||
                item.firstSourceTriangle > source.size() ||
                item.triangleCount > source.size() - item.firstSourceTriangle)
            { ++result.drawsSkipped; continue; }
            R4ActiveOcclusionBridge::DrawTransform draw;
            draw.drawId = receipt.drawId;
            draw.scopeId = receipt.scopeId;
            draw.engineFrame = receipt.engineFrame;
            draw.viewportWidth = receipt.viewportWidth;
            draw.viewportHeight = receipt.viewportHeight;
            draw.clockwiseFrontFace = receipt.clockwiseFrontFace;
            draw.backFaceCull = receipt.backFaceCull;
            draw.depthLess = receipt.depthLess;
            draw.opaque = receipt.opaque;
            draw.writesDepth = receipt.writesDepth;
            draw.stencilPassGuaranteed = receipt.stencilPassGuaranteed;
            std::memcpy(draw.uploadedWvp, receipt.uploadedWvp.data(), sizeof(draw.uploadedWvp));
            std::memcpy(draw.uploadedJitter, receipt.uploadedJitter.data(), sizeof(draw.uploadedJitter));
            const char* drawReason = nullptr;
            if (!R4ActiveOcclusionBridge::AppendDrawTriangles(
                    source.data() + item.firstSourceTriangle, item.triangleCount, draw,
                    observation.scopeId, observation.engineFrame, triangles, drawReason))
            { ++result.drawsSkipped; continue; }
            ++result.drawsUsed;
        }
        result.drawsSkipped += observation.admittedDraws.size() -
            std::min(observation.admittedDraws.size(), result.drawsUsed + result.drawsSkipped);
        result.omittedReceiptTriangles = result.receiptTriangles > triangles.size() ?
            result.receiptTriangles - triangles.size() : 0u;
        for (const auto& triangle : triangles)
            if (triangle.optionalMateTriangle != R4ActiveOcclusion::kNoMate)
                ++result.sourcePairs;
        result.sourcePairs /= 2u;

        R4ActiveOcclusion::BuildInput input;
        input.observationScopeId = observation.scopeId;
        input.triangles = triangles.data();
        input.triangleCount = triangles.size();
        input.viewportWidth = observation.viewportWidth;
        input.viewportHeight = observation.viewportHeight;
        input.viewportY = R4ActiveOcclusion::ViewportYDirection::ClipPositiveYMapsToLowerPixelY;
        input.pixelAreaWinding = R4ActiveOcclusion::PixelAreaWinding::PositiveIsClockwise;
        input.rasterEdgeGuardPixels = 1.0f;
        result.inputTriangles = triangles.size();
        if (!grid.Build(input, &result.build))
        {
            result.reason = "coverage-grid-build-invalid";
            result.buildMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - buildStart).count();
            return false;
        }
        result.ready = true;
        result.reason = nullptr;
    }
    catch (...)
    {
        grid.Reset();
        result.ready = false;
        result.reason = "observer-grid-allocation-or-construction-failure";
    }
    result.buildMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - buildStart).count();
    return result.ready;
}

void queryActiveOcclusionSamples(const LevelScene& scene,
    const std::shared_ptr<const R4EngineLocalLightSnapshot>& snapshot,
    const MetalR4LocalLightAdapter::ViewState& view,
    const std::vector<std::vector<std::size_t>>& batches,
    std::vector<R4WholeLightVisibilitySample>& samples,
    R4ActiveOcclusion::CoverageGrid& grid, ActiveOcclusionGridStats& stats)
{
    if (!snapshot || samples.empty())
        return;
    const auto queryStart = std::chrono::steady_clock::now();
    std::array<float, 16> cameraVp{};
    BuildMetalR4LocalLightCameraViewProjection(view.view, view.projection, cameraVp);
    const auto& observation = scene.activeOcclusionObservation;
    for (auto& sample : samples)
    {
        sample.cpuObservationScopeId = stats.ready ? stats.scopeId : 0u;
        const char* reason = nullptr;
        const char* prediction = "unknown";
        std::uint32_t queryCells = 0u;
        if (!stats.ready)
            reason = stats.reason ? stats.reason : "coverage-grid-unavailable";
        else
        {
            auto found = snapshot->lights.end();
            std::size_t foundIndex = 0u;
            for (std::size_t index = 0; index != snapshot->lights.size(); ++index)
                if (snapshot->lights[index].lightId == sample.lightId)
                { found = snapshot->lights.begin() + static_cast<std::ptrdiff_t>(index); foundIndex = index; break; }
            const bool batchMatches = sample.batchId >= 0 &&
                static_cast<std::size_t>(sample.batchId) < batches.size() &&
                std::find(batches[static_cast<std::size_t>(sample.batchId)].begin(),
                    batches[static_cast<std::size_t>(sample.batchId)].end(), foundIndex) !=
                    batches[static_cast<std::size_t>(sample.batchId)].end();
            const bool identityMatches = found != snapshot->lights.end() && batchMatches &&
                sample.engineFrame == snapshot->engineFrame &&
                sample.cpuObservationScopeId == stats.scopeId &&
                found->active && found->shadowed &&
                ((found->type == R4EngineLocalLightType::OmniFace &&
                    sample.groupKind == R4WholeLightVisibilityGroupKind::OmniParent &&
                    sample.expectedFaceCount == 6u && sample.face < 6u &&
                    found->parentLightId == sample.parentLightId && found->omniFace == sample.face) ||
                 (found->type == R4EngineLocalLightType::Spot &&
                    sample.groupKind == R4WholeLightVisibilityGroupKind::SingleRecord &&
                    sample.expectedFaceCount == 1u && sample.face == 0u));
            if (!identityMatches)
                reason = "selected-face-snapshot-identity-mismatch";
            else
            {
                std::array<float, 3> minimum{}, maximum{};
                R4ActiveOcclusion::ClipVertex corners[8]{};
                const char* boundsReason = nullptr;
                std::array<float, 16> model{}, wvp{};
                std::array<float, 4> localPositionAbsMax{};
                std::array<float, 4> associationError{};
                BuildMetalR4LocalLightModel(*found, model, &localPositionAbsMax);
                BuildMetalR4LocalLightWorldViewProjection(cameraVp, model, wvp);
                if (!BuildMetalR4LocalLightVolumeBounds(*found, minimum, maximum))
                    reason = "local-light-volume-bounds-invalid";
                else if (!R4ActiveOcclusionBridge::BuildLocalVolumeAssociationError(
                    cameraVp.data(), model.data(), wvp.data(), localPositionAbsMax.data(),
                    associationError.data(), boundsReason))
                    reason = boundsReason ? boundsReason : "local-volume-association-error-unknown";
                else if (!R4ActiveOcclusionBridge::BuildPaddedWorldAabbClipCorners(
                    minimum.data(), maximum.data(), cameraVp.data(), associationError.data(),
                    corners, boundsReason))
                    reason = boundsReason ? boundsReason : "local-light-query-corners-invalid";
                else
                {
                    R4ActiveOcclusion::QueryStats query{};
                    const auto result = grid.QueryPaddedAabb(corners,
                        observation.viewportWidth, observation.viewportHeight,
                        R4ActiveOcclusion::ViewportYDirection::ClipPositiveYMapsToLowerPixelY,
                        1.0f, stats.scopeId, &query);
                    queryCells = query.cellsVisited;
                    stats.queryCellTests += query.cellsVisited;
                    if (query.invalidInput || query.cellTestCapReached)
                    {
                        stats.queryInvalid = stats.queryInvalid || query.invalidInput;
                        stats.queryCellCap = stats.queryCellCap || query.cellTestCapReached;
                        reason = query.cellTestCapReached ? "query-cell-cap" : "query-invalid-input";
                    }
                    else if (result == R4ActiveOcclusion::QueryResult::ConservativelyBehindCoverage)
                    { prediction = "behind"; reason = "all-query-cells-behind-proven-coverage"; }
                    else
                    { prediction = "visible_or_unknown"; reason = "not-proven-behind-coverage"; }
                }
            }
        }
        std::fprintf(stderr,
            "METAL_ACTIVE_OCCLUSION_FACE cpu_observation_scope=%llu engine_frame=%u "
            "batch_id=%d light_id=%llu parent_light_id=%llu face=%u group_kind=%s "
            "expected_faces=%u prediction=%s query_cells=%u reason=%s\n",
            static_cast<unsigned long long>(sample.cpuObservationScopeId), sample.engineFrame,
            sample.batchId, static_cast<unsigned long long>(sample.lightId),
            static_cast<unsigned long long>(sample.parentLightId), sample.face,
            sample.groupKind == R4WholeLightVisibilityGroupKind::OmniParent ? "omni-parent" : "single",
            sample.expectedFaceCount,
            prediction, queryCells, reason ? reason : "none");
    }
    stats.queryMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - queryStart).count();
}

// Exact ordered source IDs are the key. Geometry/model/materials are immutable
// for this scene owner; Prepare resets the cache. Light matrices are fresh CBs,
// never cached. Only the index stream is reused, under a 64-MiB cache budget.
std::shared_ptr<LocalShadowScene::StaticMergeEntry> getStaticShadowMerge(
    IR4Backend& backend, const LevelScene& scene, LocalShadowScene& shadows,
    const std::vector<std::pair<std::size_t, std::size_t>>& entries,
    const std::vector<std::uint8_t>& transforms, std::string& error)
{
    if (entries.empty()) return {};
    std::vector<std::size_t> ids;
    ids.reserve(entries.size());
    std::uint64_t hash = 1469598103934665603ull;
    for (const auto& entry : entries)
    { ids.push_back(entry.first); hash ^= entry.first; hash *= 1099511628211ull; }
    const auto found = shadows.mergeCache.find(hash);
    if (found != shadows.mergeCache.end())
        for (const auto& cached : found->second)
            if (cached->materials == ids) return cached;
    if (shadows.mergeCacheSaturated) return {}; // Existing keys remain reusable.
    auto cached = std::make_shared<LocalShadowScene::StaticMergeEntry>();
    cached->materials = std::move(ids);
    std::vector<xray::metal::R4StaticShadowMerge::Row> rows;
    rows.reserve(entries.size());
    for (const auto& entry : entries)
    {
        if (entry.first >= scene.textureMaterials.size() || entry.first >= shadows.mergeSourceIndices.size() ||
            entry.second > transforms.size() || sizeof(Matrix4) > transforms.size() - entry.second)
        { error = "E08 static merge immutable source unavailable"; return {}; }
        const auto& m = scene.textureMaterials[entry.first];
        if (m.vertexBufferId >= scene.geometry.vertexBuffers.size())
        { error = "E08 static merge source vertex buffer unavailable"; return {}; }
        const auto& indices = shadows.mergeSourceIndices[entry.first];
        xray::metal::R4StaticShadowMerge::Row row;
        row.vertexBufferId = m.vertexBufferId; row.indices = indices.data(); row.indexCount = indices.size();
        row.vertexBase = m.vertexBase; row.vertexCount = m.vertexCount;
        row.vertexBufferVertexCount = scene.geometry.vertexBuffers[m.vertexBufferId].vertexCount;
        row.modelBytes = m.modelTransform.elements; row.modelByteCount = sizeof(Matrix4);
        row.wvpBytes = transforms.data() + entry.second; row.wvpByteCount = sizeof(Matrix4);
        row.opaque = m.shaderName != "def_shaders\\def_aref";
        row.supported = m.indexCount % 3u == 0u;
        rows.push_back(row);
    }
    const char* planError = nullptr;
    if (!xray::metal::R4StaticShadowMerge::Build(rows.data(), rows.size(), cached->plan, planError))
    { error = planError ? planError : "E08 static merge plan failed"; return {}; }
    bool hasMerged = false;
    for (const auto& run : cached->plan.runs)
    {
        if (run.indexCount > UINT32_MAX || run.baseVertex > INT32_MAX)
            return {}; // Conservative legacy fallback for an oversized run.
        hasMerged |= run.merged;
    }
    constexpr std::size_t CacheBudget = 64u * 1024u * 1024u;
    const std::size_t indexBytes = cached->plan.indices.size() * sizeof(std::uint16_t);
    const std::size_t payload = (hasMerged ? indexBytes : 0u) +
        cached->materials.capacity() * sizeof(std::size_t) +
        cached->plan.runs.capacity() * sizeof(xray::metal::R4StaticShadowMerge::Run) + 256u;
    if (payload > CacheBudget || shadows.mergeCacheBytes > CacheBudget - payload ||
        shadows.mergeCache.size() >= 2048u)
    {
        // Never reset accounting while current-frame plans/GPU work still pin
        // old entries. Keep existing keys, and use original draws for new keys.
        shadows.mergeCacheSaturated = true;
        return {};
    }
    if (hasMerged && indexBytes)
    {
        R4BufferDesc desc;
        desc.size = indexBytes; desc.storage = R4ResourceStorage::Private;
        desc.usage = R4BufferUsage::Index; desc.label = "R4 E08 immutable merged static shadow indices";
        cached->indices = backend.Resources().CreateBuffer(desc, cached->plan.indices.data(), indexBytes, error);
        if (!cached->indices) return {};
    }
    // CreateBuffer owns its staging/recovery copy. Runs retain offsets/counts;
    // keeping another CPU index stream in every cached plan adds no value.
    std::vector<std::uint16_t>().swap(cached->plan.indices);
    shadows.mergeCacheBytes += payload;
    shadows.mergeCache[hash].push_back(cached);
    return cached;
}

// Diagnostic pairs draw the same live Flora and wind twice in one acquired
// frame. Independent attachments and completed readbacks preserve production.
bool runFloraShadowPixelOracle(IR4Backend& backend, LevelScene& scene,
    LocalShadowScene::FloraPixelOracle& oracle, MetalR4FloraAdapter* flora,
    const R4FloraViewState& floraView, const MetalR4EngineScene::ViewState& view,
    const std::array<float, 16>& lightViewProjection, std::uint32_t extent,
    const std::string& family, std::size_t pairLimit, std::size_t framePairLimit,
    std::string& error)
{
    const char* directory = std::getenv("XRAY_METAL_FLORA_SHADOW_PIXEL_ORACLE_DIR");
    if (!directory || !*directory || !flora || !flora->IsReady() || !view.valid ||
        oracle.pairs >= pairLimit)
        return true;
    const char* everyText = std::getenv("XRAY_METAL_FLORA_SHADOW_PIXEL_ORACLE_EVERY");
    const unsigned long every = everyText ?
        std::max(1ul, std::strtoul(everyText, nullptr, 10)) : 240ul;
    if (view.engineFrame % every != 0u)
        return true;
    if (oracle.lastFrame != view.engineFrame)
    {
        oracle.lastFrame = view.engineFrame;
        oracle.pairsThisFrame = 0u;
        oracle.viewsThisFrame = 0u;
        oracle.firstViewThisFrame = family.rfind("gameplay-flora-local-", 0u) == 0u ?
            oracle.pairs : 0u;
        oracle.sampledViews.clear();
    }
    if (!oracle.sampledViews.emplace(family).second)
        return true;
    // Rotate local-light cases across sampled frames instead of consuming the
    // cap on the first atlas batch/time. All three sun cascades sample together.
    if (oracle.viewsThisFrame++ < oracle.firstViewThisFrame ||
        oracle.pairsThisFrame >= framePairLimit)
        return true;
    if (!oracle.maps[0])
    {
        R4Texture2DDesc descriptor;
        descriptor.width = descriptor.height = extent;
        descriptor.format = R4PixelFormat::R32Float;
        descriptor.storage = R4ResourceStorage::Private;
        descriptor.usage = R4TextureUsage::RenderTarget | R4TextureUsage::ShaderRead;
        descriptor.label = "R4 isolated live Flora legacy shadow oracle";
        oracle.maps[0] = backend.Resources().CreateTexture2D(descriptor, error);
        descriptor.label = "R4 isolated live Flora instanced shadow oracle";
        oracle.maps[1] = backend.Resources().CreateTexture2D(descriptor, error);
        descriptor.format = R4PixelFormat::Depth32Float;
        descriptor.usage = R4TextureUsage::RenderTarget;
        descriptor.label = "R4 isolated live Flora shadow oracle depth";
        oracle.depth = backend.Resources().CreateTexture2D(descriptor, error);
        if (!oracle.maps[0] || !oracle.maps[1] || !oracle.depth)
            return false;
    }
    if (oracle.maps[0]->Descriptor().width != extent)
    {
        error = "Flora pixel oracle extent changed within its prepared scene";
        return false;
    }
    std::array<MetalR4FloraAdapter::Telemetry, 2> sampled;
    for (std::size_t mode = 0; mode < 2u; ++mode)
    {
        R4RenderPassDesc pass;
        pass.label = "R4 isolated live Flora legacy/instanced shadow pair";
        pass.useDrawable = false;
        R4ColorAttachmentDesc color;
        color.texture = oracle.maps[mode];
        color.clearColor = {1.0, 1.0, 1.0, 1.0};
        pass.colorAttachments = {color};
        pass.depthStencilAttachment.texture = oracle.depth;
        pass.depthStencilAttachment.depthLoadAction = R4LoadAction::Clear;
        pass.depthStencilAttachment.depthStoreAction = R4StoreAction::DontCare;
        pass.hasViewport = true;
        pass.viewport = {0.0, 0.0, double(extent), double(extent), 0.0, 1.0};
        pass.hasScissor = true;
        pass.scissor = {0u, 0u, extent, extent};
        if (!backend.BeginRenderPass(pass, error) ||
            !flora->DrawShadowCastersForValidation(backend, lightViewProjection,
                floraView, mode == 1u, sampled[mode], error) ||
            !backend.EndRenderPass(error))
            return false;
        R4SunShadowValidationDesc snapshot;
        snapshot.shadowR32 = oracle.maps[mode];
        snapshot.positionRGBA16F = scene.gBufferTargets[0];
        snapshot.sceneDepthStencil = scene.depthTarget;
        snapshot.accumulationRGBA16F = scene.lightingAccumulator;
        snapshot.positionDecompressionParams = scene.positionDecompressionParams;
        const Matrix4 inverse = inverseRigidTransform(scene.view);
        std::memcpy(snapshot.inverseView.data(), inverse.elements, sizeof(inverse));
        std::memcpy(snapshot.currentViewProjection.data(),
            scene.uniformData.viewProjection.elements, sizeof(Matrix4));
        snapshot.stageVolumeViewProjection = lightViewProjection;
        snapshot.family = family + (mode == 0u ? "-legacy" : "-instanced");
        snapshot.outputDirectory = directory;
        snapshot.engineFrame = view.engineFrame;
        snapshot.phase = R4SunShadowValidationPhase::OffCameraCasters;
        snapshot.requireShadowedReceivers = false;
        snapshot.exportShadowMap = true;
        if (!backend.ValidateSunShadowCascade(snapshot, error))
            return false;
    }
    if (sampled[0].shadowSubmittedVisuals != sampled[1].shadowSubmittedVisuals ||
        sampled[0].shadowCulledVisuals != sampled[1].shadowCulledVisuals)
    {
        error = "Flora pixel oracle implementations selected different live casters";
        return false;
    }
    std::error_code filesystemError;
    std::filesystem::create_directories(directory, filesystemError);
    std::ofstream manifest(std::filesystem::path(directory) / "flora-pairs.jsonl", std::ios::app);
    if (filesystemError || !manifest)
    {
        error = "Flora pixel oracle could not record pair provenance";
        return false;
    }
    manifest << std::setprecision(std::numeric_limits<float>::max_digits10)
        << "{\"family\":\"" << family << "\",\"engine_frame\":" << view.engineFrame
        << ",\"extent\":" << extent << ",\"time_seconds\":" << floraView.timeSeconds
        << ",\"wind_valid\":" << (floraView.windValid ? "true" : "false")
        << ",\"instances\":" << sampled[0].shadowSubmittedVisuals
        << ",\"legacy_draws\":" << sampled[0].shadowDraws
        << ",\"instanced_draws\":" << sampled[1].shadowDraws;
    const auto writeValues = [&manifest](const char* key, const auto& values) {
        manifest << ",\"" << key << "\":[";
        for (std::size_t index = 0; index < values.size(); ++index)
            manifest << (index ? "," : "") << values[index];
        manifest << "]";
    };
    writeValues("light_view_projection", lightViewProjection);
    writeValues("wind_params", floraView.windParams);
    writeValues("wind_animation", floraView.windAnimation);
    writeValues("wind_setup_trees", floraView.windSetupTrees);
    manifest << "}\n";
    manifest.close();
    if (!manifest)
    {
        error = "Flora pixel oracle failed to persist pair provenance";
        return false;
    }
    ++oracle.pairs;
    ++oracle.pairsThisFrame;
    std::fprintf(stderr,
        "METAL_FLORA_PIXEL_PAIR frame=%u family=%s instances=%zu legacy_draws=%zu instanced_draws=%zu extent=%u time=%.9g\n",
        view.engineFrame, family.c_str(), sampled[0].shadowSubmittedVisuals,
        sampled[0].shadowDraws, sampled[1].shadowDraws, extent, floraView.timeSeconds);
    return true;
}

bool renderLocalShadowBatch(IR4Backend& backend, LevelScene& scene,
    LocalShadowScene& shadows, const MetalR4EngineScene::ViewState& view,
    const std::shared_ptr<const R4EngineLocalLightSnapshot>& snapshot,
    const std::vector<std::size_t>& indices, std::uint32_t batchId,
    MetalR4DynamicWorldAdapter* dynamicWorld,
    MetalR4DefVertexAdapter* defVertex,
    MetalR4DefObjectsLodAdapter* defObjectsLod,
    MetalR4FloraAdapter* flora, const R4FloraViewState& floraView,
    MetalR4DetailGrassAdapter* detail, const R4DetailGrassViewState& detailView,
    const R4EnvironmentSnapshot& environment,
    const std::vector<R4WholeLightVisibilitySample>* visibilitySamples,
    const R4LightHomDiagnosticFrame* homDiagnostic,
    std::string& error)
{
    E07ShadowCpuScope e07ShadowCpu(dynamicWorld, true);
    const auto homBatchStart=homDiagnostic?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
    shadows.records.clear();
    if (!shadows.ready || !snapshot || indices.size()>LocalShadowTileCapacity)
    { error="local shadow batch has no atlas or exceeds its capacity"; return false; }
    struct Candidate { std::size_t lightIndex; };
    std::vector<Candidate> candidates;
    for (const auto index:indices) candidates.push_back({index});
    Matrix4 cameraView{};
    std::memcpy(cameraView.elements,view.view.data(),sizeof(cameraView));
    Matrix4 cameraProjection{};
    std::memcpy(cameraProjection.elements,view.projection.data(),sizeof(cameraProjection));
    const Matrix4 viewProjection=multiply(cameraProjection,cameraView);
    const Matrix4 inverseView = inverseRigidTransform(cameraView);
    // Allocate constants only for casters admitted by each light frustum.
    // Dense level-sized tables for every batch can exhaust the frame arena.
    std::vector<std::uint8_t> transformBytes;
    using MaterialEntry = std::pair<std::size_t, std::size_t>; // source index, immutable CB offset
    std::vector<std::vector<MaterialEntry>> materialEntries(candidates.size());
    std::vector<double> homPrepMs(homDiagnostic ? candidates.size() : 0u,0.0);
    std::vector<std::size_t> allMaterials;
    if (shadows.staticLinear)
        for (std::size_t i = 0; i < scene.textureMaterials.size(); ++i) allMaterials.push_back(i);
    std::vector<Matrix4> lightViewProjections;
    lightViewProjections.reserve(candidates.size());
    shadows.records.reserve(candidates.size());
    for (std::size_t c = 0; c < candidates.size(); ++c)
    {
        const auto homPrepStart=homDiagnostic?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
        const std::uint32_t tile = static_cast<std::uint32_t>(c);
        const R4EngineLocalLight& light = snapshot->lights[candidates[c].lightIndex];
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
        record.smapId = batchId;
		record.face = light.type == R4EngineLocalLightType::OmniFace ? light.omniFace : 0u;
        record.atlasX = (tile % 4u) * LocalShadowTileSize;
        record.atlasY = (tile / 4u) * LocalShadowTileSize;
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
        const auto& selectedMaterials = shadows.staticLinear ? allMaterials :
            shadows.staticIndex.Query(lightViewProjection.elements);
        if (shadows.staticOracle)
        {
            for (std::size_t i = 0; i < scene.textureMaterials.size(); ++i)
            {
                const auto& m = scene.textureMaterials[i];
                if (!conservativelyOutsideMetalFrustum(m.localPositionBounds, m.modelTransform,
                        lightViewProjection, true) &&
                    !std::binary_search(selectedMaterials.begin(), selectedMaterials.end(), i))
                { error = "E08 static shadow index omitted an oracle caster"; return false; }
            }
            if (++shadows.oracleViews % 1000 == 0)
                std::fprintf(stderr, "METAL_E08_STATIC_ORACLE views=%zu draws=%zu misses=0\n",
                    shadows.oracleViews, scene.textureMaterials.size());
        }
        for (const std::size_t materialIndex : selectedMaterials)
        {
            const auto& material=scene.textureMaterials[materialIndex];
            if (conservativelyOutsideMetalFrustum(material.localPositionBounds,
                    material.modelTransform, lightViewProjection, true)) continue;
            const Matrix4 wvp = multiply(lightViewProjection,material.modelTransform);
            const std::size_t offset=transformBytes.size();
            materialEntries[tile].emplace_back(materialIndex, offset);
            transformBytes.resize(offset+256u,0u);
            std::memcpy(transformBytes.data()+offset,wvp.elements,sizeof(wvp));
        }
        lightViewProjections.push_back(lightViewProjection);
        shadows.records.push_back(std::move(record));
        if (homDiagnostic) homPrepMs[c]=std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-homPrepStart).count();
    }
    if (shadows.records.empty())
        return true;
    R4FrameBufferSlice batchTransforms;
    if (!transformBytes.empty() && !backend.UploadFrameData(transformBytes.data(), transformBytes.size(),
            256u, batchTransforms, error))
        return false;

    shadows.transformUploadBytes+=transformBytes.size();
    const char* mergePixelDirectory = std::getenv("XRAY_METAL_E08_STATIC_MERGE_PIXEL_ORACLE_DIR");
    std::vector<std::shared_ptr<LocalShadowScene::StaticMergeEntry>> mergePlans(candidates.size());
    if (shadows.mergeStatic || (mergePixelDirectory && *mergePixelDirectory))
        for (std::size_t tile = 0; tile < candidates.size(); ++tile)
        {
            const auto homMergeStart=homDiagnostic?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
            std::string mergeError;
            mergePlans[tile] = getStaticShadowMerge(backend, scene, shadows, materialEntries[tile], transformBytes, mergeError);
            if (!mergeError.empty()) { error = mergeError; return false; }
            if (homDiagnostic) homPrepMs[tile]+=std::chrono::duration<double,std::milli>(
                std::chrono::steady_clock::now()-homMergeStart).count();
        }
    const bool separateTiles = std::getenv("XRAY_METAL_E08_SHADOW_SEPARATE_TILES") != nullptr;
    const auto drawStaticTile = [&](std::size_t tile, bool useMerged, bool account) {
        std::size_t lastVertexBuffer = std::numeric_limits<std::size_t>::max();
        const auto& merge = useMerged ? mergePlans[tile] : nullptr;
        const std::size_t drawCount = merge ? merge->plan.runs.size() : materialEntries[tile].size();
        for (std::size_t call = 0; call < drawCount; ++call)
        {
            const auto* run = merge ? &merge->plan.runs[call] : nullptr;
            const auto& entry = materialEntries[tile][run ? run->firstRow : call];
            const std::size_t materialIndex = entry.first;
            const LevelScene::TextureMaterial& material = scene.textureMaterials[materialIndex];
            const bool alphaTested = material.shaderName == "def_shaders\\def_aref";
            const std::shared_ptr<IR4GraphicsPipeline>& pipeline = alphaTested ?
                scene.sunShadowArefPipeline : scene.sunShadowPipeline;
            if (!pipeline)
            {
                error = "alpha-tested R4 local-shadow caster pipeline is unavailable";
                return false;
            }
            const std::size_t offset = entry.second;
            R4MscStageBindings vertexBindings;
            vertexBindings.stage = R4ShaderStage::Vertex;
            vertexBindings.tlabSize = 24u;
            vertexBindings.resources = {{R4MscResourceType::ConstantBuffer, 0u,
                batchTransforms.buffer, batchTransforms.offset + offset, 256u, nullptr, nullptr}};
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
            if (run && run->merged)
            {
                draw.indexBuffer = merge->indices;
                draw.indexBufferOffset = run->indexOffset * sizeof(std::uint16_t);
                draw.indexCount = static_cast<std::uint32_t>(run->indexCount);
                draw.baseVertex = static_cast<std::int32_t>(run->baseVertex);
            }
            if (!backend.DrawIndexed(draw, error))
                return false;
            if (account)
            { shadows.submittedCasters += run ? run->rowCount : 1u; ++shadows.mergedEncodedDraws; }
        }
        return true;
    };
    // Bounded completed-GPU oracle of real source materials. These isolated
    // atlases contain static casters only; production clears/restores its own
    // atlas/depth afterwards. No receiver/off-camera PASS is inferred.
    if (mergePixelDirectory && *mergePixelDirectory && view.engineFrame % 120u == 0u &&
        shadows.mergePixelPairs < 6u)
    {
        if (!shadows.mergePixelMap)
        {
            auto desc = shadows.atlas->Descriptor(); desc.label = "R4 E08 static merge pixel oracle";
            shadows.mergePixelMap = backend.Resources().CreateTexture2D(desc, error);
            if (!shadows.mergePixelMap) return false;
        }
        for (const bool merged : {false, true})
        {
            R4RenderPassDesc pass;
            pass.label = "R4 E08 isolated static shadow index oracle"; pass.useDrawable = false;
            R4ColorAttachmentDesc color;
            color.texture = shadows.mergePixelMap; color.clearColor = {1,1,1,1};
            pass.colorAttachments = {color};
            pass.depthStencilAttachment.texture = shadows.depth;
            pass.depthStencilAttachment.depthLoadAction = R4LoadAction::Clear;
            pass.depthStencilAttachment.depthStoreAction = R4StoreAction::DontCare;
            if (!backend.BeginRenderPass(pass, error)) return false;
            for (std::size_t tile = 0; tile < shadows.records.size(); ++tile)
            {
                const auto& r = shadows.records[tile];
                R4Viewport viewport{double(r.atlasX), double(r.atlasY), double(r.atlasSize), double(r.atlasSize), 0, 1};
                R4ScissorRect scissor{r.atlasX, r.atlasY, r.atlasSize, r.atlasSize};
                if (!backend.SetActiveRenderRegion(viewport, scissor, error) ||
                    !drawStaticTile(tile, merged, false)) return false;
            }
            if (!backend.EndRenderPass(error)) return false;
            R4SunShadowValidationDesc snapshot;
            snapshot.shadowR32 = shadows.mergePixelMap; snapshot.positionRGBA16F = scene.gBufferTargets[0];
            snapshot.sceneDepthStencil = scene.depthTarget; snapshot.accumulationRGBA16F = scene.lightingAccumulator;
            snapshot.positionDecompressionParams = scene.positionDecompressionParams;
            const auto inverse = inverseRigidTransform(scene.view);
            std::memcpy(snapshot.inverseView.data(), &inverse, sizeof(inverse));
            std::memcpy(snapshot.currentViewProjection.data(), &scene.uniformData.viewProjection, sizeof(Matrix4));
            std::memcpy(snapshot.stageVolumeViewProjection.data(), &lightViewProjections.front(), sizeof(Matrix4));
            snapshot.family = "e08-static-merge-b" + std::to_string(batchId) + (merged ? "-merged" : "-legacy");
            snapshot.outputDirectory = mergePixelDirectory; snapshot.engineFrame = view.engineFrame;
            snapshot.phase = R4SunShadowValidationPhase::OffCameraCasters;
            snapshot.requireShadowedReceivers = false; snapshot.exportShadowMap = true;
            if (!backend.ValidateSunShadowCascade(snapshot, error)) return false;
        }
        ++shadows.mergePixelPairs;
    }
    for (std::size_t tile = 0; tile < shadows.records.size(); ++tile)
    {
        const auto homTileStart=homDiagnostic?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
        const std::size_t homStaticBefore=shadows.mergedEncodedDraws, homDynamicBefore=shadows.submittedDynamicCasters;
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
        // Tiles have disjoint scissor rectangles. Clear depth once at batch
        // start; no pixel in another tile can reject this light's casters.
        // One encoder retains the atlas on chip across all tiles in the batch.
        pass.depthStencilAttachment.depthLoadAction = R4LoadAction::Clear;
        pass.depthStencilAttachment.depthStoreAction = separateTiles ? R4StoreAction::Store : R4StoreAction::DontCare;
        pass.hasViewport = true;
        pass.viewport = {double(record.atlasX), double(record.atlasY),
            double(record.atlasSize), double(record.atlasSize), 0.0, 1.0};
        pass.hasScissor = true;
        pass.scissor = {record.atlasX, record.atlasY, record.atlasSize, record.atlasSize};
        if (tile == 0u || separateTiles)
        {
            pass.label = separateTiles ? "R4 local shadow separate tile" : "R4 local shadow atlas batch";
            if (!backend.BeginRenderPass(pass, error)) return false;
        }
        else if (!backend.SetActiveRenderRegion(pass.viewport, pass.scissor, error))
            return false;
        shadows.culledCasters += scene.textureMaterials.size() - materialEntries[tile].size();
        if (!drawStaticTile(tile, shadows.mergeStatic, true)) return false;
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
        if (defVertex && defVertex->IsReady())
        {
            std::array<float, 16> tileLvp{};
            std::memcpy(tileLvp.data(), lightViewProjections[tile].elements, sizeof(tileLvp));
            if (!defVertex->DrawShadowCasters(backend, tileLvp,
                    scene.sunShadowPipeline, scene.sunShadowArefPipeline,
                    scene.lightingLinearSampler ? scene.lightingLinearSampler : scene.sunShadowSampler,
                    error))
                return false;
        }
        if (defObjectsLod && defObjectsLod->IsReady())
        {
            std::array<float, 16> tileLvp{};
            std::memcpy(tileLvp.data(), lightViewProjections[tile].elements, sizeof(tileLvp));
            if (!defObjectsLod->DrawShadowCasters(backend, tileLvp,
                    scene.sunShadowPipeline, error))
                return false;
        }
        std::array<float,16> tileMatrix{};
        std::memcpy(tileMatrix.data(),lightViewProjections[tile].elements,sizeof(Matrix4));
        if (flora && flora->IsReady() && !flora->DrawShadowCasters(backend,tileMatrix,floraView,error))
            return false;
        const auto sourceLight=std::find_if(snapshot->lights.begin(),snapshot->lights.end(),
            [&](const auto& light) { return light.lightId==record.lightIdentity; });
        if (detail && detail->IsReady() && environment.grassShadows &&
            environment.ssfxGrassShadows[0]>=3 && sourceLight!=snapshot->lights.end() &&
            !sourceLight->decorative && sourceLight->cameraDistance<=environment.ssfxGrassShadows[2] &&
            !sphereOutsideMainView(sourceLight->position,sourceLight->range*0.6f,viewProjection))
        {
            R4DetailGrassShadowState shadow;
            shadow.lightView=record.lightView; shadow.lightProjection=record.lightProjection;
            shadow.lightViewProjection=tileMatrix; shadow.lightPosition=sourceLight->position;
            shadow.fadeFromLight=true; shadow.fadeDistance=-1; shadow.lightRange=sourceLight->range;
            if (!detail->DrawShadowCasters(backend,detailView,shadow,error)) return false;
        }
        if ((separateTiles || tile + 1 == shadows.records.size()) && !backend.EndRenderPass(error))
            return false;
        record.renderedDepth = true;
        ++shadows.renderedLights;
        if (homDiagnostic)
        {
            const auto index=indices[tile]; const auto& light=snapshot->lights[index];
            const bool hidden=index<homDiagnostic->hidden.size() && homDiagnostic->hidden[index];
            const bool eligible=index<homDiagnostic->eligible.size() && homDiagnostic->eligible[index];
            const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-homTileStart).count();
            std::fprintf(stderr,"METAL_LIGHT_HOM_TILE frame=%u sample=%llu batch=%u tile=%zu parent=%llu hidden=%u eligible=%u prep_cpu_ms=%.6f shadow_cpu_ms=%.6f static_rows=%zu static_draws=%zu dynamic_draws=%zu transform_bytes=%zu\n",
                view.engineFrame,(unsigned long long)homDiagnostic->sample,batchId,tile,
                (unsigned long long)(light.parentLightId?light.parentLightId:light.lightId),unsigned(hidden),unsigned(eligible),
                homPrepMs[tile],ms,materialEntries[tile].size(),shadows.mergedEncodedDraws-homStaticBefore,
                shadows.submittedDynamicCasters-homDynamicBefore,materialEntries[tile].size()*256u);
        }
        if (visibilitySamples && indices[tile] < snapshot->lights.size())
        {
            const auto& visibilityLight = snapshot->lights[indices[tile]];
            const auto selected = std::find_if(visibilitySamples->begin(), visibilitySamples->end(),
                [&](const auto& sample) {
                    return sample.engineFrame == view.engineFrame && sample.batchId == static_cast<std::int32_t>(batchId) &&
                        sample.lightId == visibilityLight.lightId && sample.parentLightId == visibilityLight.parentLightId &&
                        sample.face == (visibilityLight.type == R4EngineLocalLightType::OmniFace ? visibilityLight.omniFace : 0u);
                });
            if (selected != visibilitySamples->end())
                std::fprintf(stderr, "METAL_WHOLE_LIGHT_VIS_TILE frame=%u batch=%u light=%llu parent=%llu face=%u kind=%u static_rows=%zu static_encoded_draws=%zu dynamic_submitted_draws=%zu static_transform_bytes=%zu\n",
                    view.engineFrame, batchId, (unsigned long long)visibilityLight.lightId,
                    (unsigned long long)visibilityLight.parentLightId, selected->face,
                    static_cast<unsigned>(selected->groupKind), materialEntries[tile].size(),
                    shadows.mergedEncodedDraws - homStaticBefore,
                    shadows.submittedDynamicCasters - homDynamicBefore,
                    materialEntries[tile].size() * 256u);
        }
    }
    if (std::getenv("XRAY_METAL_FLORA_SHADOW_PIXEL_ORACLE_DIR"))
        for (std::size_t tile = 0; tile < shadows.records.size(); ++tile)
        {
            std::array<float, 16> matrix{};
            std::memcpy(matrix.data(), lightViewProjections[tile].elements, sizeof(Matrix4));
            const auto& record = shadows.records[tile];
            const std::string family = "gameplay-flora-local-b" + std::to_string(batchId) +
                "-t" + std::to_string(tile) + "-light" + std::to_string(record.lightIdentity);
            if (!runFloraShadowPixelOracle(backend, scene, shadows.floraLocalOracle,
                    flora, floraView, view, matrix, record.atlasSize, family, 12u, 4u, error))
                return false;
        }
    if (const std::shared_ptr<const R4EngineWorldSnapshot> world = backend.EngineWorldSnapshot())
    {
        if (dynamicWorld && dynamicWorld->IsEnabled())
            shadows.omittedDynamicCasters = 0u;
        else
            shadows.omittedDynamicCasters = world->renderables.size();
    }
    if (homDiagnostic)
        std::fprintf(stderr,"METAL_LIGHT_HOM_BATCH frame=%u sample=%llu batch=%u records=%zu hidden_records=%zu eligible_hidden_records=%zu total_cpu_ms=%.6f\n",
            view.engineFrame,(unsigned long long)homDiagnostic->sample,batchId,indices.size(),
            homDiagnostic->CountHidden(indices,false),homDiagnostic->CountHidden(indices,true),
            std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-homBatchStart).count());
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

struct StaticMotionProbeTargets
{
    std::array<std::shared_ptr<IR4Texture>, 4> gBuffer;
    std::shared_ptr<IR4Texture> depth;
    std::uint32_t lastProbeFrame = UINT32_MAX;
    bool previousSampleWasReset = false;
};

bool sameTextureDescriptor(const R4Texture2DDesc& left, const R4Texture2DDesc& right)
{
    return left.width == right.width && left.height == right.height &&
        left.mipLevels == right.mipLevels && left.arrayLength == right.arrayLength &&
        left.type == right.type && left.format == right.format &&
        left.storage == right.storage && left.usage == right.usage;
}

bool ensureStaticMotionProbeTargets(IR4Backend& backend, LevelScene& scene,
    StaticMotionProbeTargets& targets, std::string& error)
{
    if (scene.gBufferTargets.size() != targets.gBuffer.size() || !scene.depthTarget)
    {
        error = "static motion probe requires the production four-target G-buffer and depth target";
        return false;
    }
    for (const auto& target : scene.gBufferTargets)
    {
        if (!target)
        {
            error = "static motion probe found a missing production G-buffer attachment";
            return false;
        }
    }

    bool reusable = static_cast<bool>(targets.depth);
    for (std::size_t index = 0; reusable && index < targets.gBuffer.size(); ++index)
        reusable = targets.gBuffer[index] && sameTextureDescriptor(
            targets.gBuffer[index]->Descriptor(), scene.gBufferTargets[index]->Descriptor());
    reusable = reusable && sameTextureDescriptor(
        targets.depth->Descriptor(), scene.depthTarget->Descriptor());
    if (reusable)
        return true;

    StaticMotionProbeTargets replacement;
    const std::array<const char*, 4> labels = {
        "R4 isolated motion probe G-buffer 0",
        "R4 isolated motion probe G-buffer 1",
        "R4 isolated motion probe G-buffer 2",
        "R4 isolated motion probe G-buffer 3 motion",
    };
    for (std::size_t index = 0; index < replacement.gBuffer.size(); ++index)
    {
        R4Texture2DDesc descriptor = scene.gBufferTargets[index]->Descriptor();
        descriptor.label = labels[index];
        replacement.gBuffer[index] = backend.Resources().CreateTexture2D(descriptor, error);
        if (!replacement.gBuffer[index])
            return false;
    }
    R4Texture2DDesc depthDescriptor = scene.depthTarget->Descriptor();
    depthDescriptor.label = "R4 isolated motion probe depth";
    replacement.depth = backend.Resources().CreateTexture2D(depthDescriptor, error);
    if (!replacement.depth)
        return false;
    replacement.lastProbeFrame = targets.lastProbeFrame;
    replacement.previousSampleWasReset = targets.previousSampleWasReset;
    targets = std::move(replacement);
    return true;
}

std::size_t countLmhProbeDraws(const LevelScene& scene)
{
    std::size_t count = 0;
    for (const xray::metal::R4StaticVisual& visual : scene.geometry.visuals)
    {
        if (!visual.valid || !visual.referencedPositionBounds.available ||
            visual.vertexBufferId >= scene.pipelines.size() ||
            !scene.pipelines[visual.vertexBufferId])
            continue;
        if (!scene.textureMaterials.empty() &&
            scene.textureMaterialByVisual.find(visual.visualId) ==
                scene.textureMaterialByVisual.end())
            continue;
        if (scene.automaticLmhFrustumCulling && !scene.textureMaterials.empty())
        {
            const auto material = scene.textureMaterialByVisual.find(visual.visualId);
            const LevelScene::TextureMaterial& binding =
                scene.textureMaterials[material->second];
            if (conservativelyOutsideMetalFrustum(binding.localPositionBounds,
                    binding.modelTransform, scene.uniformData.viewProjection,
                    scene.liveViewProjectionValid))
                continue;
        }
        ++count;
    }
    return count;
}

struct ColorProbeFrameState
{
    std::array<float,4> ao{}, il{};
    bool aoAvailable=false, ilAvailable=false, initialized=false;
    std::uint32_t lastFrame=UINT32_MAX, forcedFrame=UINT32_MAX;
};
std::unordered_map<IR4Backend*, ColorProbeFrameState>& colorProbeFrames()
{
    static std::unordered_map<IR4Backend*, ColorProbeFrameState> states;
    return states;
}
void selectColorProbeFrame(IR4Backend& backend, const MetalR4EngineScene::ViewState& view,
    const R4EnvironmentSnapshot& environment)
{
    if (!view.valid || !environment.valid) return;
    auto& state=colorProbeFrames()[&backend];
    if (!state.initialized || state.lastFrame > view.engineFrame ||
        (view.previousValid && state.lastFrame != view.previousFrame) ||
        state.ao != environment.ssfxAo || state.il != environment.ssfxIl ||
        state.aoAvailable != environment.ssfxAoAvailable || state.ilAvailable != environment.ssfxIlAvailable)
        state.forcedFrame=view.engineFrame;
    state.ao=environment.ssfxAo; state.il=environment.ssfxIl;
    state.aoAvailable=environment.ssfxAoAvailable; state.ilAvailable=environment.ssfxIlAvailable;
    state.lastFrame=view.engineFrame; state.initialized=true;
}
bool shouldCaptureColorFrame(IR4Backend& backend, const MetalR4EngineScene::ViewState& view)
{
    const char* directory=std::getenv("XRAY_METAL_COLOR_PROBE_DIR");
    if (!directory || !*directory || !view.valid) return false;
    const char* intervalText=std::getenv("XRAY_METAL_COLOR_PROBE_EVERY");
    const auto interval=intervalText ? std::max(1ul,std::strtoul(intervalText,nullptr,10)) : 20ul;
    return view.cameraCut || view.engineFrame % interval == 0u ||
        colorProbeFrames()[&backend].forcedFrame == view.engineFrame;
}

// Readbacks are sampled only when requested, and preserve the production graph.
bool captureColorPhase(IR4Backend& backend, const MetalR4EngineScene::ViewState& view,
    const std::string& phase, const std::vector<std::shared_ptr<IR4Texture>>& textures,
    const std::vector<std::string>& labels, std::string& error,
    bool rgbZero = false, bool alphaOne = false, bool equalPair = false)
{
    if (!shouldCaptureColorFrame(backend, view)) return true;
    const char* directory = std::getenv("XRAY_METAL_COLOR_PROBE_DIR");
    R4TemporalTextureValidationDesc descriptor;
    descriptor.textures = textures; descriptor.labels = labels;
    descriptor.engineFrame = view.engineFrame; descriptor.previousFrame = view.previousFrame;
    descriptor.phase = phase; descriptor.outputDirectory = directory;
    descriptor.expectFinite = true; descriptor.writeImages = true; descriptor.expectRgbZero = rgbZero;
    descriptor.expectAlphaOne = alphaOne; descriptor.expectEqualPair = equalPair;
    descriptor.expectDifferentPair = phase.rfind("il_stale_positive_control_", 0) == 0;
    return backend.ValidateTemporalTextures(descriptor, error);
}

bool runStaticMotionProbe(IR4Backend& backend, LevelScene& scene,
    TerrainScene& terrain, MetalR4DefVertexAdapter* defVertex,
    MetalR4DefObjectsLodAdapter* defObjectsLod,
    const MetalR4EngineScene::ViewState& view,
    const R4RenderPassDesc& productionGBufferPass, std::string& error)
{
    const char* outputDirectory = std::getenv("XRAY_METAL_MOTION_PROBE_DIR");
    if (!outputDirectory || !*outputDirectory)
        return true;
    if (!view.valid || !scene.gameplayViewValid)
        return true;

    static std::mutex targetsMutex;
    static std::unordered_map<IR4Backend*, StaticMotionProbeTargets> targetsByBackend;
    std::lock_guard<std::mutex> lock(targetsMutex);
    StaticMotionProbeTargets& targets = targetsByBackend[&backend];
    const bool reset = view.cameraCut || !view.previousValid;
    const bool gap = view.previousValid && view.engineFrame > view.previousFrame &&
        view.engineFrame - view.previousFrame > 1u;
    const bool firstSample = targets.lastProbeFrame == UINT32_MAX;
    const bool intervalSample = view.engineFrame % 20u == 0u;
    const bool resetTransition = reset && !targets.previousSampleWasReset;
    const bool frameRewound = targets.lastProbeFrame != UINT32_MAX &&
        view.engineFrame < targets.lastProbeFrame;
    targets.previousSampleWasReset = reset;
    if (!firstSample && !intervalSample && !resetTransition && !gap && !frameRewound)
        return true;
    if (!ensureStaticMotionProbeTargets(backend, scene, targets, error))
        return false;

    Matrix4 currentView, currentProjection;
    std::memcpy(currentView.elements, view.view.data(), sizeof(currentView.elements));
    std::memcpy(currentProjection.elements, view.projection.data(), sizeof(currentProjection.elements));
    const Matrix4 currentViewProjection = multiply(currentProjection, currentView);

    const auto validateFamily = [&](const char* family, const std::function<bool(std::string&)>& draw,
                                    const std::function<std::size_t()>& submittedCount,
                                    const std::size_t candidates) {
        R4RenderPassDesc pass = productionGBufferPass;
        pass.label = family;
        for (std::size_t index = 0; index < pass.colorAttachments.size(); ++index)
        {
            pass.colorAttachments[index].texture = targets.gBuffer[index];
            pass.colorAttachments[index].loadAction = R4LoadAction::Clear;
            pass.colorAttachments[index].storeAction = R4StoreAction::Store;
            pass.colorAttachments[index].clearColor = {0.0, 0.0, 0.0, 0.0};
        }
        pass.depthStencilAttachment.texture = targets.depth;
        pass.depthStencilAttachment.depthLoadAction = R4LoadAction::Clear;
        pass.depthStencilAttachment.depthStoreAction = R4StoreAction::Store;
        pass.depthStencilAttachment.stencilLoadAction = R4LoadAction::Clear;
        pass.depthStencilAttachment.stencilStoreAction = R4StoreAction::Store;
        if (!backend.BeginRenderPass(pass, error) ||
            !backend.SetStencilReference(1u, error) || !draw(error) ||
            !backend.EndRenderPass(error))
            return false;

        R4StaticMotionValidationDesc descriptor;
        descriptor.depth = targets.depth;
        descriptor.motion = targets.gBuffer[3];
        std::memcpy(descriptor.currentViewProjection.data(), currentViewProjection.elements,
            sizeof(currentViewProjection.elements));
        std::memcpy(descriptor.previousViewProjection.data(), scene.previousViewProjection.elements,
            sizeof(scene.previousViewProjection.elements));
        descriptor.family = family;
        descriptor.outputDirectory = outputDirectory;
        descriptor.engineFrame = view.engineFrame;
        descriptor.previousFrame = view.previousFrame;
        descriptor.reset = reset;
        if (!backend.ValidateStaticMotionVectors(descriptor, error))
            return false;
        const std::size_t submitted = submittedCount();
        std::fprintf(stderr,
            "METAL_STATIC_MOTION_PROBE frame=%u previous_frame=%u family=%s reset=%d candidates=%zu submitted=%zu output=%s\n",
            view.engineFrame, view.previousFrame, family, reset ? 1 : 0,
            candidates, submitted, outputDirectory);
        return true;
    };

    const std::size_t lmhDraws = countLmhProbeDraws(scene);
    const std::uint64_t lmhFrames = scene.automaticLmhFrustumFrames;
    const std::uint64_t lmhSubmitted = scene.automaticLmhFrustumSubmitted;
    const std::uint64_t lmhCulled = scene.automaticLmhFrustumCulled;
    const bool lmhLiveReported = scene.automaticLmhLiveFrustumReported;
    const bool lmhGameplayReported = scene.automaticLmhGameplayFrustumReported;
    std::size_t lmhSubmittedForProbe = 0u;
    if (!validateFamily("LMH", [&scene, &backend, &lmhSubmittedForProbe](std::string& drawError) {
            const bool drawn = drawLevelScene(backend, scene, drawError);
            lmhSubmittedForProbe = scene.automaticLmhFrustumCulling ?
                static_cast<std::size_t>(scene.automaticLmhFrustumSubmitted) :
                countLmhProbeDraws(scene);
            return drawn;
        }, [&lmhSubmittedForProbe]() { return lmhSubmittedForProbe; }, lmhDraws))
        return false;
    scene.automaticLmhFrustumFrames = lmhFrames;
    scene.automaticLmhFrustumSubmitted = lmhSubmitted;
    scene.automaticLmhFrustumCulled = lmhCulled;
    scene.automaticLmhLiveFrustumReported = lmhLiveReported;
    scene.automaticLmhGameplayFrustumReported = lmhGameplayReported;

    const std::uint64_t terrainFrames = terrain.renderCount;
    const std::size_t terrainSubmitted = terrain.lastSubmitted;
    const std::size_t terrainCulled = terrain.lastCulled;
    const bool terrainLiveReported = terrain.liveFrustumReported;
    const bool terrainGameplayReported = terrain.gameplayFrustumReported;
    const std::size_t terrainCandidates = terrain.ready &&
        (terrain.uploaded || terrain.pendingUploaded) ? terrain.draws.size() : 0u;
    std::size_t terrainSubmittedForProbe = 0u;
    if (!validateFamily("terrain", [&backend, &scene, &terrain, &terrainSubmittedForProbe](std::string& drawError) {
            const bool drawn = drawTerrainScene(backend, scene, terrain, drawError);
            terrainSubmittedForProbe = terrain.ready &&
                (terrain.uploaded || terrain.pendingUploaded) ? terrain.lastSubmitted : 0u;
            return drawn;
        }, [&terrainSubmittedForProbe]() { return terrainSubmittedForProbe; }, terrainCandidates))
        return false;
    terrain.renderCount = terrainFrames;
    terrain.lastSubmitted = terrainSubmitted;
    terrain.lastCulled = terrainCulled;
    terrain.liveFrustumReported = terrainLiveReported;
    terrain.gameplayFrustumReported = terrainGameplayReported;

    const std::size_t defVertexCandidates = defVertex && defVertex->IsReady() ?
        defVertex->LastTelemetry().admittedVisuals : 0u;
    std::size_t defVertexSubmittedForProbe = 0u;
    if (!validateFamily("DefVertex", [&, defVertex](std::string& drawError) {
            if (!defVertex)
                return true;
            R4DefVertexViewState familyView;
            familyView.view = view.view;
            familyView.projection = view.projection;
            familyView.previousView = view.previousView;
            familyView.previousProjection = view.previousProjection;
            familyView.previousValid = view.previousValid;
            familyView.cameraCut = view.cameraCut;
            familyView.valid = view.valid;
            const bool drawn = defVertex->DrawIntoActiveGBuffer(backend, familyView, drawError);
            defVertexSubmittedForProbe = defVertex->IsReady() ?
                defVertex->LastTelemetry().submittedVisuals : 0u;
            return drawn;
        }, [&defVertexSubmittedForProbe]() { return defVertexSubmittedForProbe; },
        defVertexCandidates))
        return false;

    const std::size_t defObjectsCandidates = defObjectsLod && defObjectsLod->IsReady() ?
        defObjectsLod->LastTelemetry().requestedVisuals : 0u;
    std::size_t defObjectsSubmittedForProbe = 0u;
    if (!validateFamily("DefObjectsLod", [&, defObjectsLod](std::string& drawError) {
            if (!defObjectsLod)
                return true;
            R4DefObjectsLodViewState familyView;
            familyView.view = view.view;
            familyView.projection = view.projection;
            familyView.previousView = view.previousView;
            familyView.previousProjection = view.previousProjection;
            familyView.previousValid = view.previousValid;
            familyView.cameraCut = view.cameraCut;
            familyView.staticGlobals = scene.baseLightingStaticGlobals;
            familyView.valid = view.valid;
            const bool drawn = defObjectsLod->DrawIntoActiveGBuffer(backend, familyView, drawError);
            defObjectsSubmittedForProbe = defObjectsLod->IsReady() ?
                defObjectsLod->LastTelemetry().submittedVisuals : 0u;
            return drawn;
        }, [&defObjectsSubmittedForProbe]() { return defObjectsSubmittedForProbe; },
        defObjectsCandidates))
        return false;

    targets.lastProbeFrame = view.engineFrame;
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
    MetalR4SsrAdapter* ssfxSsr,
    std::shared_ptr<IR4Texture>& ssrSceneColorSnapshot,
    MetalR4VolumetricAdapter* volumetric,
    MetalR4RainAdapter* rain,
    std::uint64_t& rainTelemetryFrame,
    MetalR4WaterAdapter* water,
    MetalR4LensFlareAdapter* lensFlare,
    MetalR4LensFlareAdapter* lensOverlay,
    std::uint64_t& lensFlareTelemetryFrame,
    MetalR4LocalLightAdapter* localLights,
    LocalShadowScene* localShadows,
    MetalR4DynamicWorldAdapter* dynamicWorld,
    const MetalR4DynamicWorldAdapter::Options& dynamicProbeOptions,
    MetalR4FloraAdapter* flora, const R4FloraViewState& floraView,
    MetalR4DetailGrassAdapter* detail, const R4DetailGrassViewState& detailView,
    MetalR4HudAdapter* hud,
    MetalR4ScopeAdapter* scope,
    MetalR4ParticleAdapter* particles,
    MetalR4BloomAdapter* bloom,
    std::shared_ptr<IR4Texture>& emissiveTarget,
    float pdaDisplayFactor,
    NightVisionScene* nightVision,
    const MetalR4EngineScene::ViewState& view,
    const R4EnvironmentSnapshot& environment,
    const R4GameplayVisionSnapshot& vision,
    const std::shared_ptr<const MetalR4EngineScene::GBufferWorldDrawCallback>& worldDraw,
    MetalR4FogScatteringAdapter* fogScattering,
    std::string& error)
{
    selectColorProbeFrame(backend, view, environment);
    if (!ensureLevelDepthTarget(backend, scene, error) ||
        !uploadLevelScene(backend, scene, error) ||
        (defVertex && !defVertex->Upload(backend, error)) ||
        (defObjectsLod && !defObjectsLod->Upload(backend, error)) ||
        (staticForward && !staticForward->Upload(backend, error)))
        return false;
    const char* probeIntervalText=std::getenv("XRAY_METAL_SHADOW_PROBE_EVERY");
    const unsigned requestedProbeInterval=probeIntervalText?static_cast<unsigned>(std::strtoul(probeIntervalText,nullptr,10)):20u;
    const unsigned shadowProbeInterval=requestedProbeInterval>=1u && requestedProbeInterval<=3000u?requestedProbeInterval:20u;
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
        E07ShadowCpuScope e07ShadowCpu(dynamicWorld, false);
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
        const auto drawDefVertexSunCasters = [&backend, defVertex, &scene](
            const Matrix4& lightViewProjection, std::string& drawError) {
            if (!defVertex || !defVertex->IsReady())
                return true;
            std::array<float, 16> matrix{};
            std::memcpy(matrix.data(), lightViewProjection.elements, sizeof(matrix));
            return defVertex->DrawShadowCasters(backend, matrix,
                scene.sunShadowPipeline, scene.sunShadowArefPipeline,
                scene.sunShadowSampler, drawError);
        };
        const auto drawDefObjectsLodSunCasters = [&backend, defObjectsLod, &scene](
            const Matrix4& lightViewProjection, std::string& drawError) {
            if (!defObjectsLod || !defObjectsLod->IsReady())
                return true;
            std::array<float, 16> matrix{};
            std::memcpy(matrix.data(), lightViewProjection.elements, sizeof(matrix));
            return defObjectsLod->DrawShadowCasters(backend, matrix,
                scene.sunShadowPipeline, drawError);
        };
        if (scene.sunCascadesValid)
        for (unsigned cascade=0;cascade<3;++cascade)
        {
            R4RenderPassDesc shadowPass;
            const std::string shadowLabel="R4 engine converted direct-sun shadow cascade " + std::to_string(cascade);
            shadowPass.label = shadowLabel.c_str();
            shadowPass.useDrawable=false;
            R4ColorAttachmentDesc shadowColor;
            shadowColor.texture=scene.sunCascades[cascade].shadow;
            shadowColor.clearColor={1,1,1,1};
            shadowPass.colorAttachments={shadowColor};
            shadowPass.depthStencilAttachment.texture=scene.sunShadowDepthTarget;
            shadowPass.depthStencilAttachment.depthStoreAction=R4StoreAction::DontCare;
            const Matrix4& matrix=scene.sunCascades[cascade].lightViewProjection;
            std::vector<std::size_t> fallbackCasters;
            auto& sunCasters = localShadows ? localShadows->sunCasters : fallbackCasters;
            const bool sunLinear = localShadows ? localShadows->sunLinear :
                std::getenv("XRAY_METAL_E08_SUN_LINEAR") != nullptr;
            const bool sunOracle = localShadows ? localShadows->sunOracle :
                std::getenv("XRAY_METAL_E08_SUN_ORACLE") != nullptr;
            sunCasters.clear();
            std::size_t sunCandidates = scene.textureMaterials.size();
            if (!sunLinear)
            {
                const auto select = [&](const std::size_t i) {
                    const auto& m = scene.textureMaterials[i];
                    if (!conservativelyOutsideMetalFrustum(m.localPositionBounds,
                            m.modelTransform, matrix, true)) sunCasters.push_back(i);
                };
                if (localShadows && localShadows->ready)
                {
                    const auto& broad = localShadows->staticIndex.Query(matrix.elements);
                    sunCandidates = broad.size();
                    for (const auto i : broad) select(i);
                }
                else
                    for (std::size_t i = 0; i < scene.textureMaterials.size(); ++i) select(i);
                if (sunOracle)
                    for (std::size_t i = 0; i < scene.textureMaterials.size(); ++i)
                    {
                        const auto& m = scene.textureMaterials[i];
                        if (!conservativelyOutsideMetalFrustum(m.localPositionBounds, m.modelTransform,
                                matrix, true) && !std::binary_search(sunCasters.begin(), sunCasters.end(), i))
                        { error = "E08 static sun broad phase omitted an oracle caster"; return false; }
                    }
            }
            // Bounded, opt-in pixel oracle: real static materials/shaders,
            // complete legacy list vs selected list in isolated GPU maps.
            // The existing validator is used solely to export completed R32
            // snapshots; no off-camera receiver claim is made (count remains 0).
            const char* pixelDirectory = std::getenv("XRAY_METAL_E08_SUN_PIXEL_ORACLE_DIR");
            if (pixelDirectory && *pixelDirectory && localShadows && !sunLinear &&
                view.engineFrame % 240u == 0 && localShadows->sunPixelPairs < 6)
            {
                auto& map = localShadows->sunPixelMaps[cascade];
                if (!map)
                {
                    auto desc = scene.sunCascades[cascade].shadow->Descriptor();
                    desc.label = "R4 E08 static sun GPU oracle";
                    map = backend.Resources().CreateTexture2D(desc, error);
                    if (!map) return false;
                }
                for (const bool culled : {false, true})
                {
                    auto pass = shadowPass;
                    pass.label = "R4 E08 static sun GPU oracle";
                    pass.colorAttachments[0].texture = map;
                    if (!backend.BeginRenderPass(pass, error) ||
                        !drawSunShadowMap(backend, scene, cascade, error, false, false,
                            nullptr, nullptr, culled ? &sunCasters : nullptr) ||
                        !backend.EndRenderPass(error)) return false;
                    R4SunShadowValidationDesc snapshot;
                    snapshot.shadowR32 = map; snapshot.positionRGBA16F = scene.gBufferTargets[0];
                    snapshot.sceneDepthStencil = scene.depthTarget;
                    snapshot.accumulationRGBA16F = scene.lightingAccumulator;
                    snapshot.positionDecompressionParams = scene.positionDecompressionParams;
                    const auto inverse = inverseRigidTransform(scene.view);
                    std::memcpy(snapshot.inverseView.data(), &inverse, sizeof(inverse));
                    std::memcpy(snapshot.currentViewProjection.data(), &scene.uniformData.viewProjection, sizeof(Matrix4));
                    std::memcpy(snapshot.stageVolumeViewProjection.data(), &matrix, sizeof(matrix));
                    snapshot.family = "e08-static-sun-c" + std::to_string(cascade) + (culled ? "-culled" : "-full");
                    snapshot.outputDirectory = pixelDirectory; snapshot.engineFrame = view.engineFrame;
                    snapshot.phase = R4SunShadowValidationPhase::OffCameraCasters;
                    snapshot.requireShadowedReceivers = false; snapshot.exportShadowMap = true;
                    if (!backend.ValidateSunShadowCascade(snapshot, error)) return false;
                }
                ++localShadows->sunPixelPairs;
            }
            std::size_t staticSunDraws = 0;
            if (!backend.BeginRenderPass(shadowPass,error) ||
                !drawSunShadowMap(backend,scene,cascade,error, false, false, &staticSunDraws,
                    nullptr, sunLinear ? nullptr : &sunCasters) ||
                !drawDynamicSunCasters(matrix,error) || !drawDefVertexSunCasters(matrix,error) ||
                !drawDefObjectsLodSunCasters(matrix,error)) return false;
            std::array<float,16> lightVP{};
            std::memcpy(lightVP.data(),matrix.elements,sizeof(Matrix4));
            if (flora && flora->IsReady() && !flora->DrawShadowCasters(backend,lightVP,floraView,error))
                return false;
            if (detail && detail->IsReady() && environment.grassShadows &&
                cascade<=environment.ssfxGrassShadows[0])
            {
                R4DetailGrassShadowState shadow;
                shadow.lightView={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
                shadow.lightProjection=lightVP; shadow.lightViewProjection=lightVP;
                shadow.fadeDistance=environment.detailFadeDistance*environment.detailFadeDistance*environment.ssfxGrassShadows[1];
                if (!detail->DrawShadowCasters(backend,detailView,shadow,error)) return false;
            }
            if (!backend.EndRenderPass(error))
                return false;
            if (localShadows && std::getenv("XRAY_METAL_FLORA_SHADOW_PIXEL_ORACLE_DIR") &&
                !runFloraShadowPixelOracle(backend, scene, localShadows->floraSunOracle,
                    flora, floraView, view, lightVP,
                    scene.sunCascades[cascade].shadow->Descriptor().width,
                    "gameplay-flora-sun-c" + std::to_string(cascade), 6u, 3u, error))
                return false;
            if (view.engineFrame % 120u == 0u)
                std::fprintf(stderr, "METAL_E08_STATIC_SUN frame=%u cascade=%u total=%zu candidates=%zu draws=%zu oracle=%u\n",
                    view.engineFrame, cascade, scene.textureMaterials.size(), sunCandidates,
                    staticSunDraws, unsigned(sunOracle));
            if (view.engineFrame%20u==0u)
                std::fprintf(stderr,"METAL_SUN_CASCADE_FRAME frame=%u cascade=%u size=%.3f bias=%.8f reset=%u flora_draws=%zu grass_enabled=%u grass_draws=%zu planes=%zu\n",
                    view.engineFrame,cascade,scene.sunCascadeContract.cascades[cascade].size,
                    scene.sunCascadeContract.cascades[cascade].bias,
                    scene.sunCascadeContract.cascades[cascade].resetChain,
                    flora?flora->LastTelemetry().shadowDraws:0u,environment.grassShadows,
                    detail && environment.grassShadows && cascade<=environment.ssfxGrassShadows[0]?detail->LastTelemetry().shadowDraws:0u,
                    scene.sunCascadeContract.cascades[cascade].casterPlanes.size());
        }
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
    LevelScene::ActiveOcclusionObservation* activeOcclusionObservation = nullptr;
    if (scene.activeOcclusionEnabled && activeOcclusionFrameInWindow(view.engineFrame))
        activeOcclusionObservation = currentActiveOcclusionObservation(scene, view.engineFrame);
    if (!backend.BeginRenderPass(gBufferPass, error) ||
        !backend.SetStencilReference(1, error) ||
        !drawLevelScene(backend, scene, error, activeOcclusionObservation))
        return false;
    if (defVertex)
    {
        R4DefVertexViewState defVertexView;
        defVertexView.view = view.view;
        defVertexView.projection = view.projection;
        defVertexView.previousView = view.previousView;
        defVertexView.previousProjection = view.previousProjection;
        defVertexView.previousValid = view.previousValid;
        defVertexView.cameraCut = view.cameraCut;
        defVertexView.valid = view.valid;
        if (!defVertex->DrawIntoActiveGBuffer(backend, defVertexView, error))
            return false;
    }
    if (defObjectsLod)
    {
        R4DefObjectsLodViewState defObjectsLodView;
        defObjectsLodView.view = view.view;
        defObjectsLodView.projection = view.projection;
        defObjectsLodView.previousView = view.previousView;
        defObjectsLodView.previousProjection = view.previousProjection;
        defObjectsLodView.previousValid = view.previousValid;
        defObjectsLodView.cameraCut = view.cameraCut;
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
    completeActiveOcclusionObservation(scene, activeOcclusionObservation);
    if (!captureColorPhase(backend, view, "gbuffer_world", scene.gBufferTargets,
            {"position_packed_normal", "diffuse", "auxiliary", "motion"}, error)) return false;
    if (!runStaticMotionProbe(backend, scene, terrain, defVertex, defObjectsLod,
            view, gBufferPass, error))
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
    const bool hasEmissiveHud = hudSnapshot && hudSnapshot->valid && hud && hud->IsReady() &&
        std::any_of(hudSnapshot->renderables.begin(), hudSnapshot->renderables.end(),
            [](const R4EngineHudRenderable& renderable) { return !renderable.emissiveGeometryIndices.empty(); });
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

    const bool volumeFrame = volumetric && volumetric->IsReady() && view.valid && environment.valid &&
        environment.ssfxVolumetricAvailable;
    if (volumeFrame)
    {
        const auto& size = scene.gBufferTargets[0]->Descriptor();
        if (!volumetric->PrepareRenderTargets(backend, size.width, size.height,
                environment.volumetricResolutionDivisor, error)) return false;
        MetalR4VolumetricAdapter::FrameInputs inputs;
        inputs.engineFrame = view.engineFrame; inputs.position = scene.gBufferTargets[0];
        inputs.staticGlobals = scene.baseLightingStaticGlobals;
        if (!volumetric->BeginFrame(backend, inputs, error)) return false;
    }

    if (bloom)
    {
        const auto& size = scene.gBufferTargets[0]->Descriptor();
        if (!emissiveTarget || emissiveTarget->Descriptor().width != size.width ||
            emissiveTarget->Descriptor().height != size.height)
        {
            R4Texture2DDesc target;
            target.width = size.width; target.height = size.height;
            target.format = R4PixelFormat::BGRA8Unorm;
            target.storage = R4ResourceStorage::Private;
            target.usage = R4TextureUsage::RenderTarget | R4TextureUsage::ShaderRead;
            target.label = "R4 exact rt_ssfx_bloom_emissive";
            emissiveTarget = backend.Resources().CreateTexture2D(target, error);
            if (!emissiveTarget) return false;
        }
        R4RenderPassDesc pass;
        pass.label = "R4 exact admitted selflightl emissive producer";
        pass.useDrawable = false;
        R4ColorAttachmentDesc color;
        color.texture = emissiveTarget;
        color.clearColor = {0.0, 0.0, 0.0, 0.0};
        color.storeAction = R4StoreAction::Store;
        pass.colorAttachments = {color};
        pass.depthStencilAttachment.texture = scene.depthTarget;
        pass.depthStencilAttachment.depthLoadAction = R4LoadAction::Load;
        pass.depthStencilAttachment.depthStoreAction = R4StoreAction::Store;
        pass.depthStencilAttachment.stencilLoadAction = R4LoadAction::Load;
        pass.depthStencilAttachment.stencilStoreAction = R4StoreAction::Store;
        R4DynamicWorldViewState dynamicView;
        dynamicView.view = view.view; dynamicView.projection = view.projection;
        dynamicView.previousView = view.previousView;
        dynamicView.previousProjection = view.previousProjection;
        dynamicView.eye = view.eye; dynamicView.engineFrame = view.engineFrame;
        dynamicView.previousFrame = view.previousFrame;
        dynamicView.valid = view.valid; dynamicView.previousValid = view.previousValid;
        dynamicView.cameraCut = view.cameraCut;
        if (!backend.BeginRenderPass(pass, error)) return false;
        const bool drawn = !dynamicWorld || !dynamicWorld->IsEnabled() ||
            dynamicWorld->DrawEmissive(backend, dynamicView, backend.EngineWorldSnapshot(), pdaDisplayFactor, error);
        std::string endError; const bool ended = backend.EndRenderPass(endError);
        if (!drawn) return false;
        if (!ended) { error = endError; return false; }
        if (hasEmissiveHud)
        {
            pass.label = "R4 exact admitted HUD selflightl emissive producer";
            pass.colorAttachments[0].loadAction = R4LoadAction::Load;
            pass.hasViewport = true;
            pass.viewport = {0.0, 0.0, double(size.width), double(size.height), 0.0, 0.02};
            if (!backend.BeginRenderPass(pass, error)) return false;
            const bool hudDrawn = hud->DrawEmissive(backend, hudSnapshot, pdaDisplayFactor, error);
            const bool hudEnded = backend.EndRenderPass(endError);
            if (!hudDrawn) return false;
            if (!hudEnded) { error = endError; return false; }
        }
        const auto world = dynamicWorld ? dynamicWorld->LastTelemetry() : MetalR4DynamicWorldAdapter::Telemetry{};
        const auto ui = hud ? hud->LastTelemetry() : MetalR4HudAdapter::Telemetry{};
        static std::uint32_t lastEmissiveFrame = UINT32_MAX;
        static std::size_t lastCandidates = SIZE_MAX, lastDraws = SIZE_MAX;
        const auto candidates = world.emissiveCandidates + ui.emissiveCandidates;
        const auto draws = world.emissiveDraws + ui.emissiveDraws;
        if (lastEmissiveFrame == UINT32_MAX || environment.engineFrame < lastEmissiveFrame ||
            environment.engineFrame - lastEmissiveFrame >= 300u ||
            candidates != lastCandidates || draws != lastDraws)
        {
            std::fprintf(stderr, "METAL_EMISSIVE_FRAME frame=%u world=%zu/%zu rejected=%zu hud=%zu/%zu rejected=%zu family=selflightl\n",
                environment.engineFrame, world.emissiveDraws, world.emissiveCandidates, world.emissiveRejected,
                ui.emissiveDraws, ui.emissiveCandidates, ui.emissiveRejected);
            lastEmissiveFrame = environment.engineFrame;
            lastCandidates = candidates; lastDraws = draws;
        }

    }

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

        const auto validateSun = [&](R4SunShadowValidationPhase phase, unsigned cascade) {
            const char* output=std::getenv("XRAY_METAL_SHADOW_PROBE_DIR");
            if (!output || !*output || !view.valid || !scene.sunCascadesValid ||
                view.engineFrame % shadowProbeInterval != 0u) return true;
            R4SunShadowValidationDesc witness;
            witness.shadowR32=scene.sunCascades[cascade].shadow;
            witness.positionRGBA16F=scene.gBufferTargets[0];
            witness.positionDecompressionParams=scene.positionDecompressionParams;
            witness.sceneDepthStencil=scene.depthTarget;
            witness.accumulationRGBA16F=scene.lightingAccumulator;
            witness.family="production"; witness.outputDirectory=output;
            witness.engineFrame=view.engineFrame; witness.phase=phase;
            const Matrix4 inverse=inverseRigidTransform(scene.view);
            std::memcpy(witness.inverseView.data(),&inverse,sizeof(inverse));
            std::memcpy(witness.currentViewProjection.data(),&scene.uniformData.viewProjection,sizeof(Matrix4));
            std::memcpy(witness.stageVolumeViewProjection.data(),
                &scene.sunCascades[cascade==2?1:cascade].lightViewProjection,sizeof(Matrix4));
            witness.receiverVolume=scene.sunCascadeContract.cascades[cascade].receiverVolume;
            witness.hasReceiverVolume=true;
            witness.farZCullEnabled=scene.sunZCulling;
            witness.nearDepthScale=environment.sunDepthScale[0]; witness.nearDepthBias=scene.sunCascadeContract.cascades[cascade].bias;
            witness.farDepthScale=environment.sunDepthScale[1]; witness.farDepthBias=scene.sunCascadeContract.cascades[cascade].bias;
            witness.sourceCascadeSize=scene.sunCascadeContract.cascades[cascade].size;
            witness.sourceCascadeBias=scene.sunCascadeContract.cascades[cascade].bias;
            witness.tsmBias=environment.sunTsmBias;
            witness.sunColorIntensity=environment.adaptedSunColor[0]+environment.adaptedSunColor[1]+environment.adaptedSunColor[2];
            witness.grassShadowsEnabled=environment.grassShadows;
            return backend.ValidateSunShadowCascade(witness,error);
        };
        R4RenderPassDesc maskPass;
        maskPass.label = "R4 engine converted direct-sun stencil mask";
        maskPass.useDrawable = false;
        maskPass.colorAttachments = {accumulatorAttachment};
        maskPass.depthStencilAttachment = sunDepthStencil;
        if (!backend.BeginRenderPass(maskPass, error) ||
            !drawSunMask(backend, scene, error) || !backend.EndRenderPass(error) ||
            !validateSun(R4SunShadowValidationPhase::StencilBaseline,0u))
            return false;

        if (scene.sunCascadesValid)
        for (unsigned cascade=0;cascade<3;++cascade)
        {
            R4RenderPassDesc sunPass;
            const std::string sunLabel="R4 engine converted direct-sun accumulation cascade " + std::to_string(cascade);
            sunPass.label=sunLabel.c_str();
            sunPass.useDrawable=false;
            sunPass.colorAttachments={accumulatorAttachment};
            sunPass.depthStencilAttachment=sunDepthStencil;
            if (!backend.BeginRenderPass(sunPass,error) || !drawSunCascade(backend,scene,cascade,error) ||
                !backend.EndRenderPass(error) || !validateSun(
                    static_cast<R4SunShadowValidationPhase>(cascade+1u),cascade)) return false;
        }
    }

    if (volumeFrame && scene.productionSun && scene.sunCascadesValid && environment.sunShaftsEnabled &&
        (environment.sunShaftsMode == 1u || environment.sunShaftsMode == 3u) &&
        environment.sunShaftsIntensity >= 0.0001f)
    {
        if (environment.sunShaftsQuality != 1u)
        { error = "volumetric sun cook only admits source low quality 1"; return false; }
        MetalR4VolumetricAdapter::SunPass pass;
        pass.shadowMap = scene.sunCascades[2].shadow;
        pass.dynamicLight = scene.baseLightingDynamicLight;
        pass.vertexGlobals = scene.sunVertexGlobals; pass.vertexTransforms = scene.sunVertexTransforms;
        pass.volumeVertices = scene.sunCascades[2].volume; pass.volumeIndices = scene.sunVolumeIndices;
        std::memcpy(pass.shadowMatrix.data(), scene.sunCascades[2].shadowLookup.elements, sizeof(Matrix4));
        pass.nearRange = 0; pass.farRange = environment.sunShaftsFarRange;
        pass.shaftsIntensity = environment.sunShaftsIntensity;
        if (!volumetric->DrawSun(backend, pass, error)) return false;
    }

    if (!captureColorPhase(backend, view, "sun_accumulator", {scene.lightingAccumulator}, {"accumulator"}, error)) return false;

    // Windows first emissive contribution follows sun and precedes local lights.
    // The bloom target is a separate later destination of the same exact shader.
    if (dynamicWorld || hasEmissiveHud)
    {
        R4RenderPassDesc pass;
        pass.label = "R4 exact selflightl first accumulator contribution";
        pass.useDrawable = false;
        auto color = accumulatorAttachment; color.loadAction = R4LoadAction::Load;
        color.storeAction = R4StoreAction::Store; pass.colorAttachments = {color};
        pass.depthStencilAttachment.texture = scene.depthTarget;
        pass.depthStencilAttachment.depthLoadAction = R4LoadAction::Load;
        pass.depthStencilAttachment.depthStoreAction = R4StoreAction::Store;
        pass.depthStencilAttachment.stencilLoadAction = R4LoadAction::Load;
        pass.depthStencilAttachment.stencilStoreAction = R4StoreAction::Store;
        R4DynamicWorldViewState dynamicView;
        dynamicView.view = view.view; dynamicView.projection = view.projection;
        dynamicView.previousView = view.previousView; dynamicView.previousProjection = view.previousProjection;
        dynamicView.eye = view.eye; dynamicView.engineFrame = view.engineFrame;
        dynamicView.previousFrame = view.previousFrame; dynamicView.valid = view.valid;
        dynamicView.previousValid = view.previousValid; dynamicView.cameraCut = view.cameraCut;
        if (!backend.BeginRenderPass(pass, error)) return false;
        const bool drawn = !dynamicWorld || !dynamicWorld->IsEnabled() ||
            dynamicWorld->DrawEmissiveAccumulator(backend, dynamicView, backend.EngineWorldSnapshot(), pdaDisplayFactor, error);
        std::string endError; const bool ended = backend.EndRenderPass(endError);
        if (!drawn) return false;
        if (!ended) { error=endError; return false; }
        if (hasEmissiveHud)
        {
            pass.label = "R4 exact HUD selflightl first accumulator contribution";
            pass.hasViewport = true;
            const auto& size = scene.gBufferTargets[0]->Descriptor();
            pass.viewport = {0, 0, double(size.width), double(size.height), 0, 0.02};
            if (!backend.BeginRenderPass(pass, error)) return false;
            const bool hudDrawn = hud->DrawEmissiveAccumulator(backend, hudSnapshot, pdaDisplayFactor, error);
            const bool hudEnded = backend.EndRenderPass(endError);
            if (!hudDrawn) return false;
            if (!hudEnded) { error=endError; return false; }
        }
        if (!captureColorPhase(backend, view, "emissive_accumulator", {scene.lightingAccumulator}, {"accumulator"}, error)) return false;
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
        const auto snapshot=backend.EngineLightingFrameSnapshot();
        std::vector<std::vector<std::size_t>> batches;
        std::vector<std::size_t> unshadowed;
        const bool e08PlanTiming = std::getenv("XRAY_METAL_E08_GPU_PASSES") && view.engineFrame % 120u == 0u;
        const auto e08PlanStart = e08PlanTiming ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        if (!planLocalShadowBatches(view,snapshot,environment,batches,unshadowed,error)) return false;
        std::vector<R4WholeLightVisibilitySample> visibilitySamples;
        xray::metal::R4WholeLightVisibilitySelectionStats visibilityStats;
        const bool visibilityOptIn = wholeLightVisibilityEnabled();
        const std::uint32_t visibilityFirstFrame = visibilityOptIn ?
            wholeLightVisibilityFirstFrame() : UINT32_MAX;
        const char* visibilityFailureReason = nullptr;
        std::string visibilitySelectionError;
        if (visibilityOptIn && snapshot)
        {
            try
            {
                if (visibilityFirstFrame != UINT32_MAX)
                    xray::metal::SelectR4WholeLightVisibilitySamples(*snapshot, environment, batches,
                        visibilityFirstFrame, view.engineFrame, visibilitySamples, visibilityStats,
                        visibilitySelectionError);
            }
            catch (const std::bad_alloc&)
            {
                visibilityFailureReason = "selection-allocation-failed";
                visibilitySamples.clear();
                visibilityStats = {};
            }
            if (!visibilitySelectionError.empty())
                visibilityFailureReason = "selection-invalid-input";
        }
        const bool activeOcclusionObserveRequested = scene.activeOcclusionRequested &&
            visibilityOptIn && activeOcclusionFrameInWindow(view.engineFrame);
        if (activeOcclusionObserveRequested)
        {
            ActiveOcclusionGridStats activeStats;
            activeStats.scopeId = scene.activeOcclusionObservation.scopeId;
            R4ActiveOcclusion::CoverageGrid activeGrid;
            try
            {
                if (!scene.activeOcclusionEnabled)
                    activeStats.reason = "static-source-not-enabled";
                else if (!scene.activeOcclusionLocalVolumeShaderQualified)
                    activeStats.reason = scene.activeOcclusionLocalVolumeShaderReason ?
                        scene.activeOcclusionLocalVolumeShaderReason : "local-volume-shader-unqualified";
                else if (visibilitySamples.empty())
                    activeStats.reason = "no-selected-complete-groups";
                else
                    buildActiveOcclusionGrid(scene, snapshot, visibilitySamples, activeGrid, activeStats);
                if (!visibilitySamples.empty())
                    queryActiveOcclusionSamples(scene, snapshot, localView, batches,
                        visibilitySamples, activeGrid, activeStats);
            }
            catch (...)
            {
                activeGrid.Reset();
                activeStats.ready = false;
                activeStats.reason = "observer-exception-fail-open";
                for (auto& sample : visibilitySamples)
                    sample.cpuObservationScopeId = 0u;
            }
            {
                const char* gridStatus = activeStats.ready ? "ready" :
                    (activeStats.reason && std::strcmp(activeStats.reason,
                        "no-selected-complete-groups") != 0 &&
                        std::strcmp(activeStats.reason, "static-source-not-enabled") != 0 &&
                        std::strcmp(activeStats.reason, "local-volume-artifact-identity-mismatch") != 0 &&
                        std::strcmp(activeStats.reason, "local-volume-source-or-receipt-identity-mismatch") != 0 &&
                        std::strcmp(activeStats.reason, "local-volume-metallib-unreadable-or-oversized") != 0 &&
                        std::strcmp(activeStats.reason, "local-volume-reflection-unreadable-or-oversized") != 0 &&
                        std::strcmp(activeStats.reason, "local-volume-receipt-unreadable-or-oversized") != 0 &&
                        std::strcmp(activeStats.reason, "local-volume-qualification-failed") != 0 ?
                        "unknown" : "skipped");
                std::fprintf(stderr,
                    "METAL_ACTIVE_OCCLUSION_GRID status=%s cpu_observation_scope=%llu engine_frame=%u "
                    "selected_groups=%zu selected_faces=%zu draw_receipts=%zu selected_draws=%zu "
                    "draws_used=%zu draws_skipped=%zu receipt_triangles=%llu selected_triangles=%llu "
                    "omitted_receipt_triangles=%llu selector_invalid=%zu selector_duplicate=%zu "
                    "selector_unknown_projection=%zu selector_outside_viewport=%zu selector_budget_omissions=%zu "
                    "selection_budget=%u triangles=%zu source_pairs=%zu "
                    "paired_unions_accepted=%u cells_marked=%u primitive_cell_tests=%llu "
                    "query_cell_tests=%llu query_invalid=%u triangle_cap=%u cell_test_cap=%u build_ms=%.6f query_ms=%.6f "
                    "raster_convention=synthetic_only suppression=disabled reason=%s\n",
                    gridStatus, static_cast<unsigned long long>(activeStats.scopeId),
                    view.engineFrame, visibilityStats.selectedGroups, visibilitySamples.size(),
                    activeStats.receiptsSeen, activeStats.selectedDraws,
                    activeStats.drawsUsed, activeStats.drawsSkipped,
                    static_cast<unsigned long long>(activeStats.receiptTriangles),
                    static_cast<unsigned long long>(activeStats.selectedTriangles),
                    static_cast<unsigned long long>(activeStats.omittedReceiptTriangles),
                    activeStats.selectorInvalidReceipts,
                    activeStats.selectorDuplicateReceipts,
                    activeStats.selectorUnknownProjection,
                    activeStats.selectorOutsideViewport,
                    activeStats.selectorBudgetOmissions,
                    activeStats.selectionBudget,
                    activeStats.inputTriangles, activeStats.sourcePairs,
                    activeStats.build.pairedUnionsAccepted, activeStats.build.cellsMarked,
                    static_cast<unsigned long long>(activeStats.build.primitiveCellTests),
                    static_cast<unsigned long long>(activeStats.queryCellTests),
                    unsigned(activeStats.queryInvalid),
                    unsigned(activeStats.sourceTriangleCap || activeStats.build.triangleCapReached),
                    unsigned(activeStats.build.cellTestCapReached || activeStats.queryCellCap),
                    activeStats.buildMs, activeStats.queryMs,
                    activeStats.reason ? activeStats.reason : "none");
            }
        }
        if (e08PlanTiming)
            std::fprintf(stderr, "METAL_E08_LOCAL_PLAN frame=%u cpu_ms=%.6f batches=%zu\n", view.engineFrame,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - e08PlanStart).count(), batches.size());
        const auto homDiagnostic=BeginMetalR4LightHomDiagnostic(view.view,view.projection,view.eye,
            view.valid,view.engineFrame,snapshot.get(),environment,batches,unshadowed);
        localLights->BeginFrame();
        MetalR4LocalLightAdapter::DrawState drawState;
        if (!visibilitySamples.empty())
        {
            try
            {
                drawState.visibilitySamples = visibilitySamples;
            }
            catch (const std::bad_alloc&)
            {
                visibilityFailureReason = "draw-state-allocation-failed";
                visibilitySamples.clear();
                drawState.visibilitySamples.clear();
                visibilityStats = {};
            }
        }
        if (visibilityFailureReason)
        {
            static bool loggedDiagnosticFailure = false;
            if (!loggedDiagnosticFailure)
            {
                std::fprintf(stderr, "METAL_WHOLE_LIGHT_VIS status=skipped reason=%s\n",
                    visibilityFailureReason);
                loggedDiagnosticFailure = true;
            }
        }
        if (visibilityOptIn && visibilityFirstFrame != UINT32_MAX &&
            view.engineFrame >= visibilityFirstFrame && view.engineFrame - visibilityFirstFrame < 12u)
            std::fprintf(stderr, "METAL_WHOLE_LIGHT_VIS_FRAME frame=%u status=%s planned_groups=%zu eligible_groups=%zu selected_groups=%zu selected_records=%zu omitted_quota=%zu incomplete_or_collision=%zu hud_exempt_records=%zu volumetric_exempt_records=%zu invalid_exempt_records=%zu\n",
                view.engineFrame, visibilityStats.eligibleGroups ? "selected" : "inconclusive_no_eligible_groups",
                visibilityStats.plannedGroups, visibilityStats.eligibleGroups,
                visibilityStats.selectedGroups, visibilityStats.selectedRecords,
                visibilityStats.omittedByQuota, visibilityStats.incompleteOrCollision,
                visibilityStats.hudExemptRecords, visibilityStats.volumetricExemptRecords,
                visibilityStats.invalidExemptRecords);
        if (localShadows)
        {
            auto& sh=*localShadows;
            sh.records.clear(); sh.candidates=sh.renderedLights=sh.submittedCasters=sh.culledCasters=0;
            sh.dynamicCandidates=sh.submittedDynamicCasters=sh.culledDynamicCasters=sh.rejectedDynamicCasters=sh.omittedDynamicCasters=0;
            sh.batches=batches.size(); sh.transformUploadBytes=0; sh.mergedEncodedDraws=0;
            for (const auto& batch:batches) sh.candidates+=batch.size();
        }
        const auto accumulate=[&](const std::vector<std::size_t>& indices, std::int64_t batchId) {
            if (indices.empty()) return true;
            const auto homAccumStart=homDiagnostic.enabled?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
            const bool enableCounts = visibilityOptIn && batchId >= 0 &&
                std::any_of(visibilitySamples.begin(), visibilitySamples.end(), [batchId](const auto& sample) {
                    return sample.batchId == batchId;
                });
            drawState.activeVisibilityBatch = enableCounts ? batchId : -1;
            if (enableCounts)
            {
                R4RenderPassDesc pass = localLightPass;
                pass.enableVisibilityResultCounts = true;
                if (!backend.BeginRenderPass(pass,error)) return false;
            }
            else if (!backend.BeginRenderPass(localLightPass,error)) return false;
            const bool drawn=localLights->DrawBatchIntoActiveAccumulator(backend,inputs,localView,snapshot,indices,drawState,error);
            std::string endError;
            const bool ended=backend.EndRenderPass(endError);
            if (!drawn) return false;
            if (!ended) { error=endError; return false; }
            if (homDiagnostic.enabled)
                std::fprintf(stderr,"METAL_LIGHT_HOM_ACCUM frame=%u sample=%llu batch=%lld records=%zu hidden_records=%zu eligible_hidden_records=%zu cpu_ms=%.6f\n",
                    view.engineFrame,(unsigned long long)homDiagnostic.sample,(long long)batchId,indices.size(),
                    homDiagnostic.CountHidden(indices,false),homDiagnostic.CountHidden(indices,true),
                    std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-homAccumStart).count());
            return true;
        };
        if (!accumulate(unshadowed,-1)) return false;
        std::vector<xray::metal::R4LocalShadowAtlasRecord> recordArchive;
        for (std::size_t batchId=0;batchId<batches.size();++batchId)
        {
            if (!localShadows) { error="shadowed local lights have no atlas owner"; return false; }
            // The atlas is consumed in this encoder before the next batch can
            // overwrite it. The accumulator, depth and stencil all load/store.
            if (!renderLocalShadowBatch(backend,scene,*localShadows,view,snapshot,batches[batchId],
                static_cast<std::uint32_t>(batchId),dynamicWorld,defVertex,defObjectsLod,
                flora,floraView,detail,detailView,environment,visibilitySamples.empty()?nullptr:&visibilitySamples,
                homDiagnostic.enabled?&homDiagnostic:nullptr,error) ||
                !accumulate(batches[batchId],static_cast<std::int64_t>(batchId))) return false;
            if (volumeFrame)
            {
                for (const auto& record : localShadows->records)
                {
                    const auto lightIndex = std::find_if(batches[batchId].begin(), batches[batchId].end(),
                        [&](std::size_t index) { return snapshot->lights[index].lightId == record.lightIdentity; });
                    if (lightIndex == batches[batchId].end())
                    { error="volumetric record has no light in its active atlas batch"; return false; }
                    MetalR4VolumetricAdapter::SpotPass pass;
                    if (!BuildMetalR4VolumetricSpotPass(snapshot->lights[*lightIndex], environment,
                            record, view.view, view.projection, pass, error)) return false;
                    if (!pass.sliceCount) continue;
                    pass.shadowAtlas = localShadows->atlas;
                    // Source s_accum_volume is created globally with this cookie;
                    // authored per-light surface projectors are not its inputs.
                    R4EngineLocalLight cookieSource; cookieSource.type = R4EngineLocalLightType::Spot;
                    if (!localLights->ResolveProjectorTexture(backend, cookieSource, pass.lightMap, error) ||
                        !volumetric->DrawSpot(backend, pass, error)) return false;
                }
            }
            const char* probeDirectory=std::getenv("XRAY_METAL_LOCAL_SHADOW_BATCH_PROBE_DIR");
            const char* probeEvery=std::getenv("XRAY_METAL_LOCAL_SHADOW_BATCH_PROBE_EVERY");
            const unsigned interval=probeEvery?std::max(1, std::atoi(probeEvery)):20;
            if (probeDirectory && *probeDirectory && view.engineFrame%interval==0u)
            {
                R4LocalShadowBatchValidationDesc witness;
                witness.records=localShadows->records;
                for (const auto index:batches[batchId]) witness.parentIds.push_back(snapshot->lights[index].parentLightId);
                witness.view=view.view; witness.projection=view.projection;
                const Matrix4 inv=inverseRigidTransform(scene.view);
                std::memcpy(witness.inverseView.data(),&inv,sizeof(inv));
                witness.positionDecompressionParams=scene.positionDecompressionParams;
                witness.engineFrame=view.engineFrame; witness.batchId=static_cast<std::uint32_t>(batchId);
                witness.outputDirectory=probeDirectory;
                if (!backend.ValidateLocalShadowBatch(witness,localShadows->atlas,inputs.position,
                    scene.depthTarget,scene.lightingAccumulator,error)) return false;
            }
            recordArchive.insert(recordArchive.end(),localShadows->records.begin(),localShadows->records.end());
        }
        if (localShadows) localShadows->records=std::move(recordArchive); // metadata only; atlas owns the final batch
        const char* batchProbe=std::getenv("XRAY_METAL_LOCAL_SHADOW_BATCH_PROBE");
        if (batchProbe && *batchProbe && view.valid)
        {
            const auto& t=localLights->LastTelemetry();
            std::fprintf(stderr,"METAL_LOCAL_SHADOW_BATCH_FRAME frame=%u batches=%zu records=%zu rendered=%zu shadowed=%zu submitted=%zu culled=%zu fallback=%zu resets=%zu marker=%u transform_bytes=%zu eye=%.6f,%.6f,%.6f\n",
                view.engineFrame,batches.size(),localShadows?localShadows->candidates:0u,
                localShadows?localShadows->renderedLights:0u,t.submittedShadowed,t.submitted,t.culled,t.skippedShadowed,t.markerResets,drawState.marker.next,localShadows?localShadows->transformUploadBytes:0u,view.eye[0],view.eye[1],view.eye[2]);
            for (std::size_t batchId=0;batchId<batches.size();++batchId)
                for (std::size_t tile=0;tile<batches[batchId].size();++tile)
                {
                    const auto& l=snapshot->lights[batches[batchId][tile]];
                    std::fprintf(stderr,"METAL_LOCAL_SHADOW_BATCH_RECORD frame=%u batch=%zu tile=%zu id=%llu parent=%llu face=%u type=%u\n",view.engineFrame,batchId,tile,
                        static_cast<unsigned long long>(l.lightId),static_cast<unsigned long long>(l.parentLightId),l.omniFace,static_cast<unsigned>(l.type));
                }
        }
    }

    if (volumeFrame && !volumetric->BlurAndMerge(backend, error)) return false;
    if (volumeFrame && !captureColorPhase(backend, view, "volumetric_merged", {volumetric->OutputTexture()},
            {"volumetric"}, error)) return false;

    // Opt-in witness: actual production casters outside the primary frustum
    // render into an isolated map, then project primary G-buffer receivers.
    const char* shadowProbeDirectory=std::getenv("XRAY_METAL_SHADOW_PROBE_DIR");
    if (shadowProbeDirectory && *shadowProbeDirectory && scene.sunCascadesValid &&
        view.valid && view.engineFrame % shadowProbeInterval == 0u)
    {
        if (const auto sourceWorld=backend.EngineWorldSnapshot())
        for (const auto& object:sourceWorld->renderables)
            if (object.asset && object.asset->sourceName.find("metal_shadow_item")!=std::string::npos)
                std::fprintf(stderr,"METAL_STATIC_SHADOW_FIXTURE frame=%u position=%.6f,%.6f,%.6f local_sphere=%.6f,%.6f,%.6f,%.6f casts=%u\n",
                    view.engineFrame,object.transform[12],object.transform[13],object.transform[14],
                    object.bounds[0],object.bounds[1],object.bounds[2],object.bounds[3],object.castsShadow);
        auto outsideWorld=std::make_shared<R4EngineWorldSnapshot>();
        outsideWorld->engineFrame=view.engineFrame;
        if (const auto world=backend.EngineWorldSnapshot())
        for (const auto& renderable:world->renderables)
        {
            if (!renderable.castsShadow || !renderable.asset) continue;
            auto staticAsset=std::make_shared<R4EngineWorldAsset>(*renderable.asset);
            staticAsset->geometry.erase(std::remove_if(staticAsset->geometry.begin(),staticAsset->geometry.end(),
                [](const auto& geometry) { return geometry.skinned; }),staticAsset->geometry.end());
            if (staticAsset->geometry.empty()) continue;
            Matrix4 model{}; std::memcpy(&model,renderable.transform.data(),sizeof(model));
            const Vec3 center=transformPoint(model,{renderable.bounds[0],renderable.bounds[1],renderable.bounds[2]});
            float stretch2=0;
            for (unsigned column=0;column<3;++column)
                for (unsigned row=0;row<3;++row) stretch2+=model.elements[column*4+row]*model.elements[column*4+row];
            const float conservativeRadius=renderable.bounds[3]*std::sqrt(stretch2);
            if (sphereOutsideMainView({center.x,center.y,center.z},conservativeRadius,scene.uniformData.viewProjection))
            {
                auto witnessRenderable=renderable; witnessRenderable.asset=staticAsset;
                outsideWorld->renderables.push_back(std::move(witnessRenderable));
            }
        }
        // This adapter starts with an empty geometry cache, never draws the
        // primary view, and prewarms only genuinely off-camera source objects.
        // It exercises the production non-skinned admission fix independently
        // of anything seen during precache or an earlier camera turn.
        MetalR4DynamicWorldAdapter coldDynamic;
        std::string coldStatus;
        if (!coldDynamic.Configure(backend,dynamicProbeOptions,coldStatus))
        { error="cold shadow witness configure: "+coldStatus; return false; }
        const std::size_t cacheBefore=coldDynamic.LastTelemetry().cachedGeometry;
        R4DynamicWorldViewState coldView;
        coldView.view=view.view; coldView.projection=view.projection; coldView.eye=view.eye;
        coldView.previousView=view.previousView; coldView.previousProjection=view.previousProjection;
        coldView.engineFrame=view.engineFrame; coldView.previousFrame=view.previousFrame;
        coldView.valid=view.valid; coldView.previousValid=view.previousValid; coldView.cameraCut=view.cameraCut;
        if (!coldDynamic.Prewarm(backend,coldView,outsideWorld,error)) return false;
        std::fprintf(stderr,"METAL_COLD_SHADOW_CACHE frame=%u outside_objects=%zu cache_before=%zu cache_after=%zu main_draws=0\n",
            view.engineFrame,outsideWorld->renderables.size(),cacheBefore,coldDynamic.LastTelemetry().cachedGeometry);
        auto outsideNpc=std::make_shared<R4EngineWorldSnapshot>();
        if (const auto world=backend.EngineWorldSnapshot())
        {
            *outsideNpc=*world;
            for (auto& renderable:outsideNpc->renderables)
            {
                bool outside=false;
                if (renderable.asset && std::any_of(renderable.asset->geometry.begin(),renderable.asset->geometry.end(),
                    [](const auto& geometry) { return geometry.skinned; }))
                {
                    Matrix4 model{}; std::memcpy(&model,renderable.transform.data(),sizeof(model));
                    const Vec3 center=transformPoint(model,{renderable.bounds[0],renderable.bounds[1],renderable.bounds[2]});
                    float stretch2=0;
                    for (unsigned column=0;column<3;++column)
                        for (unsigned row=0;row<3;++row) stretch2+=model.elements[column*4+row]*model.elements[column*4+row];
                    outside=sphereOutsideMainView({center.x,center.y,center.z},renderable.bounds[3]*std::sqrt(stretch2),scene.uniformData.viewProjection);
                }
                renderable.castsShadow=renderable.castsShadow && outside;
            }
        }
        R4Texture2DDesc target;
        target.width=target.height=2048; target.format=R4PixelFormat::R32Float;
        target.storage=R4ResourceStorage::Private;
        target.usage=R4TextureUsage::RenderTarget | R4TextureUsage::ShaderRead;
        target.label="R4 isolated off-camera caster witness";
        auto isolatedMap=backend.Resources().CreateTexture2D(target,error);
        if (!isolatedMap) return false;
        for (unsigned cascade=0;cascade<3;++cascade)
        for (unsigned family=0;family<10;++family)
        {
            if (family>=2 && family!=4 && family!=5 && cascade!=2) continue;
            if (family==5 && (!environment.grassShadows || cascade>environment.ssfxGrassShadows[0])) continue;
            R4RenderPassDesc pass;
            pass.label="R4 isolated production off-camera shadow casters"; pass.useDrawable=false;
            R4ColorAttachmentDesc color; color.texture=isolatedMap; color.clearColor={1,1,1,1};
            pass.colorAttachments={color};
            pass.depthStencilAttachment.texture=scene.sunShadowDepthTarget;
            pass.depthStencilAttachment.depthStoreAction=R4StoreAction::DontCare;
            if (!backend.BeginRenderPass(pass,error)) return false;
            std::size_t submitted=0;
            const auto& lightVP=scene.sunCascadeContract.cascades[cascade].lightViewProjection;
            if ((family==0 || family==2 || family==3) && flora && flora->HasShadowCasters())
            {
                auto outsideView=floraView; outsideView.shadowOnlyOffCamera=family==0;
                outsideView.shadowIgnoreAlpha=family==3;
                if (!flora->DrawShadowCasters(backend,lightVP,outsideView,error)) return false;
                submitted=flora->LastTelemetry().shadowSubmittedVisuals;
            }
            if (family==1 && coldDynamic.IsEnabled())
            {
                std::size_t candidates=0,culled=0,rejected=0;
                if (!coldDynamic.DrawLocalShadowTile(backend,lightVP,outsideWorld,
                    candidates,submitted,culled,rejected,error)) return false;
            }
            if (family==4 && dynamicWorld && dynamicWorld->IsEnabled())
            {
                std::size_t candidates=0,culled=0,rejected=0;
                if (!dynamicWorld->DrawLocalShadowTile(backend,lightVP,outsideNpc,candidates,submitted,culled,rejected,error)) return false;
            }
            if ((family==6 || family==7) && !drawSunShadowMap(backend,scene,cascade,error,true,family==7,&submitted)) return false;
            if ((family==8 || family==9) && !drawSunShadowMap(backend,scene,cascade,error,true,family==9,&submitted,"mtl_fence")) return false;
            if (family==5 && detail && detail->IsReady())
            {
                R4DetailGrassShadowState shadow;
                shadow.lightView={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
                shadow.lightProjection=lightVP; shadow.lightViewProjection=lightVP;
                shadow.fadeDistance=environment.detailFadeDistance*environment.detailFadeDistance*environment.ssfxGrassShadows[1];
                if (!detail->DrawShadowCasters(backend,detailView,shadow,error)) return false;
                submitted=detail->LastTelemetry().shadowDraws;
            }
            if (!backend.EndRenderPass(error)) return false;
            R4SunShadowValidationDesc witness;
            witness.phase=R4SunShadowValidationPhase::OffCameraCasters;
            witness.shadowR32=isolatedMap; witness.positionRGBA16F=scene.gBufferTargets[0];
            witness.positionDecompressionParams=scene.positionDecompressionParams;
            witness.sceneDepthStencil=scene.depthTarget; witness.accumulationRGBA16F=scene.lightingAccumulator;
            witness.family=std::string(family==0?"offcamera_flora_":family==1?"offcamera_dynamic_":
                family==2?"alpha_flora_":family==3?"opaque_flora_":family==4?"offcamera_npc_":family==5?"grass_sun_":family==6?"alpha_aref_":family==7?"opaque_aref_":family==8?"alpha_fence_":"opaque_fence_")+std::to_string(cascade);
            witness.exportShadowMap=family==1 || family==2 || family==3 || family>=5; witness.requireShadowedReceivers=family<2 || family==4;
            witness.outputDirectory=shadowProbeDirectory; witness.engineFrame=view.engineFrame;
            const Matrix4 inverse=inverseRigidTransform(scene.view);
            std::memcpy(witness.inverseView.data(),&inverse,sizeof(inverse));
            std::memcpy(witness.currentViewProjection.data(),&scene.uniformData.viewProjection,sizeof(Matrix4));
            witness.stageVolumeViewProjection=lightVP;
            witness.sourceCascadeSize=scene.sunCascadeContract.cascades[cascade].size;
            witness.sourceCascadeBias=scene.sunCascadeContract.cascades[cascade].bias;
            witness.offCameraCasterCount=static_cast<std::uint32_t>(submitted);
            if (!backend.ValidateSunShadowCascade(witness,error)) return false;
        }
        if (std::getenv("XRAY_METAL_SHADOW_LOCAL_PROBE") && localShadows && !localShadows->records.empty())
        {
            const unsigned localSize=localShadows->records.front().atlasSize;
            target.width=target.height=localSize;
            auto localMap=backend.Resources().CreateTexture2D(target,error);
            target.format=R4PixelFormat::Depth32Float; target.usage=R4TextureUsage::RenderTarget;
            auto localDepth=backend.Resources().CreateTexture2D(target,error);
            if (!localMap || !localDepth) return false;
            for (const auto& record:localShadows->records)
            for (unsigned family=0;family<4;++family)
            {
                const auto lights=backend.EngineLightingFrameSnapshot();
                const auto sourceLight=lights?std::find_if(lights->lights.begin(),lights->lights.end(),
                    [&](const auto& light) { return light.lightId==record.lightIdentity; }):std::vector<R4EngineLocalLight>::const_iterator{};
                if (family==3 && (!detail || !detail->IsReady() || !environment.grassShadows ||
                    environment.ssfxGrassShadows[0]<3 || !lights || sourceLight==lights->lights.end() ||
                    sourceLight->decorative || sourceLight->cameraDistance>environment.ssfxGrassShadows[2] ||
                    sphereOutsideMainView(sourceLight->position,sourceLight->range*0.6f,scene.uniformData.viewProjection))) continue;
                if (family==0 && lights && sourceLight!=lights->lights.end())
                    std::fprintf(stderr,"METAL_LOCAL_SHADOW_SOURCE frame=%u id=%llu type=%u face=%u position=%.6f,%.6f,%.6f range=%.6f\n",
                        view.engineFrame,static_cast<unsigned long long>(record.lightIdentity),static_cast<unsigned>(sourceLight->type),
                        record.face,sourceLight->position[0],sourceLight->position[1],sourceLight->position[2],sourceLight->range);
                Matrix4 lightView{},lightProjection{};
                std::memcpy(&lightView,record.lightView.data(),sizeof(lightView));
                std::memcpy(&lightProjection,record.lightProjection.data(),sizeof(lightProjection));
                const Matrix4 lightMatrix=multiply(lightProjection,lightView);
                std::array<float,16> lightVP{}; std::memcpy(lightVP.data(),&lightMatrix,sizeof(lightMatrix));
                R4RenderPassDesc pass; pass.label="R4 isolated local off-camera production casters"; pass.useDrawable=false;
                R4ColorAttachmentDesc color; color.texture=localMap; color.clearColor={1,1,1,1}; pass.colorAttachments={color};
                pass.depthStencilAttachment.texture=localDepth;
                pass.depthStencilAttachment.depthStoreAction=R4StoreAction::DontCare;
                if (!backend.BeginRenderPass(pass,error)) return false;
                std::size_t submitted=0;
                if (family==0 && flora && flora->HasShadowCasters())
                {
                    auto outsideView=floraView; outsideView.shadowOnlyOffCamera=true;
                    if (!flora->DrawShadowCasters(backend,lightVP,outsideView,error)) return false;
                    submitted=flora->LastTelemetry().shadowSubmittedVisuals;
                }
                if (family==1 || family==2)
                {
                    std::size_t candidates=0,culled=0,rejected=0;
                    auto* producer=family==1?&coldDynamic:dynamicWorld;
                    if (producer && producer->IsEnabled() && !producer->DrawLocalShadowTile(backend,lightVP,
                        family==1?outsideWorld:outsideNpc,candidates,submitted,culled,rejected,error)) return false;
                }
                if (family==3)
                {
                    R4DetailGrassShadowState shadow;
                    shadow.lightView=record.lightView; shadow.lightProjection=record.lightProjection;
                    shadow.lightViewProjection=lightVP; shadow.lightPosition=sourceLight->position;
                    shadow.fadeFromLight=true; shadow.fadeDistance=-1; shadow.lightRange=sourceLight->range;
                    if (!detail->DrawShadowCasters(backend,detailView,shadow,error)) return false;
                    submitted=detail->LastTelemetry().shadowDraws;
                }
                if (!backend.EndRenderPass(error)) return false;
                R4SunShadowValidationDesc witness;
                witness.phase=R4SunShadowValidationPhase::OffCameraCasters; witness.shadowR32=localMap;
                witness.positionRGBA16F=scene.gBufferTargets[0]; witness.positionDecompressionParams=scene.positionDecompressionParams;
                witness.sceneDepthStencil=scene.depthTarget; witness.accumulationRGBA16F=scene.lightingAccumulator;
                witness.family=std::string(family==0?"offcamera_local_flora_":family==1?"offcamera_local_dynamic_":family==2?"offcamera_local_npc_":"grass_local_")+std::to_string(record.lightIdentity)+"_face_"+std::to_string(record.face);
                witness.requireShadowedReceivers=family!=3;
                if (lights && sourceLight!=lights->lights.end())
                    witness.family+="_type_"+std::to_string(static_cast<unsigned>(sourceLight->type));
                witness.outputDirectory=shadowProbeDirectory; witness.engineFrame=view.engineFrame;
                const Matrix4 inverse=inverseRigidTransform(scene.view);
                std::memcpy(witness.inverseView.data(),&inverse,sizeof(inverse));
                std::memcpy(witness.currentViewProjection.data(),&scene.uniformData.viewProjection,sizeof(Matrix4));
                witness.stageVolumeViewProjection=lightVP; witness.offCameraCasterCount=static_cast<std::uint32_t>(submitted);
                if (!backend.ValidateSunShadowCascade(witness,error)) return false;
            }
        }
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
        aoInputs.previousView = view.previousView;
        aoInputs.previousProjection = view.previousProjection;
        aoInputs.environment = environment;
        aoInputs.cameraCut = view.cameraCut;
        aoInputs.previousValid = view.previousValid;
        aoInputs.previousFrame = view.previousFrame;
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

    if (!captureColorPhase(backend, view, "accumulator", {scene.lightingAccumulator}, {"accumulator"}, error)) return false;
    if (scene.ssfxAoOutput && !captureColorPhase(backend, view, "ao", {scene.ssfxAoOutput}, {"ao"}, error,
            !environment.ssfxAoAvailable || environment.ssfxAo[1] <= 0.0f,
            !environment.ssfxAoAvailable || environment.ssfxAo[1] <= 0.0f)) return false;
    const bool ilDisabled = !environment.ssfxIlAvailable || environment.ssfxIl[1] <= 0.0f;
    if (scene.ssfxIlOutput && !captureColorPhase(backend, view, "il", {scene.ssfxIlOutput}, {"il"}, error,
            ilDisabled, ilDisabled)) return false;

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

    if (!captureColorPhase(backend, view, "combine_base", scene.lightingTargets, {"low", "high"}, error)) return false;
    // Repeat the exact converted combine against an explicit neutral IL input.
    // Same frame G-buffer/AO/accumulator/constants; only IL texture differs.
    const char* colorProbeDirectory = std::getenv("XRAY_METAL_COLOR_PROBE_DIR");
    if (ilDisabled && scene.ssfxIlOutput && colorProbeDirectory && *colorProbeDirectory && view.valid &&
        shouldCaptureColorFrame(backend, view))
    {
        const auto actualTargets = scene.lightingTargets;
        const auto actualIl = scene.ssfxIlOutput;
        std::vector<std::shared_ptr<IR4Texture>> referenceTargets;
        for (const auto& target : actualTargets)
        {
            auto descriptor = target->Descriptor(); descriptor.label = "R4 IL disabled neutral combine reference";
            auto reference = backend.Resources().CreateTexture2D(descriptor, error);
            if (!reference) return false;
            referenceTargets.push_back(reference);
        }
        scene.lightingTargets = referenceTargets; scene.ssfxIlOutput = scene.lightingBlack;
        auto referencePass = lightingPass;
        referencePass.label = "R4 converted combine_1 IL disabled neutral reference";
        for (std::size_t i=0; i<referenceTargets.size(); ++i) referencePass.colorAttachments[i].texture = referenceTargets[i];
        bool referenceDrawn = backend.BeginRenderPass(referencePass, error);
        if (referenceDrawn)
        {
            referenceDrawn = drawBaseLighting(backend, scene, error);
            std::string endError;
            if (!backend.EndRenderPass(endError)) { if (error.empty()) error=endError; referenceDrawn=false; }
        }
        scene.lightingTargets = actualTargets; scene.ssfxIlOutput = actualIl;
        if (!referenceDrawn) return false;
        for (std::size_t i=0; i<actualTargets.size(); ++i)
            if (!captureColorPhase(backend, view, "il_neutral_combine_"+std::to_string(i),
                {actualTargets[i], referenceTargets[i]}, {"actual", "neutral_reference"}, error, false, false, true)) return false;
        // Sensitivity control: stale nonzero IL at intensity zero must differ.
        // Readbacks above precede these overwrites on the same command buffer.
        scene.lightingTargets = referenceTargets; scene.ssfxIlOutput = scene.lightingWhite;
        referencePass.label = "R4 converted combine_1 stale IL positive defect control";
        bool staleDrawn = backend.BeginRenderPass(referencePass, error);
        if (staleDrawn)
        {
            staleDrawn = drawBaseLighting(backend, scene, error);
            std::string endError;
            if (!backend.EndRenderPass(endError)) { if (error.empty()) error=endError; staleDrawn=false; }
        }
        scene.lightingTargets = actualTargets; scene.ssfxIlOutput = actualIl;
        if (!staleDrawn) return false;
        for (std::size_t i=0; i<actualTargets.size(); ++i)
            if (!captureColorPhase(backend, view, "il_stale_positive_control_"+std::to_string(i),
                {actualTargets[i], referenceTargets[i]}, {"actual", "stale_nonzero_il"}, error)) return false;
    }

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

    if (lensFlare && lensFlare->HasFlares())
    {
        R4RenderPassDesc flarePass;
        flarePass.label = "R4 exact effects_sun forward"; flarePass.useDrawable = false;
        R4ColorAttachmentDesc color;
        color.texture = scene.lightingTargets[0]; color.loadAction = R4LoadAction::Load;
        color.storeAction = R4StoreAction::Store; flarePass.colorAttachments = {color};
        flarePass.depthStencilAttachment.texture = scene.depthTarget;
        flarePass.depthStencilAttachment.depthLoadAction = R4LoadAction::Load;
        flarePass.depthStencilAttachment.depthStoreAction = R4StoreAction::Store;
        flarePass.depthStencilAttachment.stencilLoadAction = R4LoadAction::Load;
        flarePass.depthStencilAttachment.stencilStoreAction = R4StoreAction::Store;
        if (!backend.BeginRenderPass(flarePass, error)) return false;
        const bool drawn = lensFlare->DrawIntoActiveForward(backend, scene.baseLightingStaticGlobals, environment.engineFrame, error);
        std::string endError; const bool ended = backend.EndRenderPass(endError);
        if (!drawn) return false;
        if (!ended) { error = endError; return false; }
        const auto& telemetry = lensFlare->LastTelemetry();
        if (++lensFlareTelemetryFrame == 1 || lensFlareTelemetryFrame % 300 == 0)
            std::fprintf(stderr, "METAL_LENS_FLARE_FRAME frame=%u captured=%zu staged=%zu submitted=%zu rejected=%zu\n",
                environment.engineFrame, telemetry.capturedDraws, telemetry.stagedDraws, telemetry.submittedDraws, telemetry.rejectedDraws);
    }

    // Windows copies rt_Generic_0 to rt_Generic_temp immediately before the
    // general SSR chain. Keep an equivalent non-aliasing source snapshot so
    // trace/combine never sample the color target they are writing.
    if (ssfxSsr && ssfxSsr->IsReady())
    {
        MetalR4SsrAdapter::FrameInputs inputs;
        inputs.engineFrame = environment.engineFrame;
        inputs.previousFrame = view.previousFrame;
        inputs.previousValid = view.previousValid;
        inputs.cameraCut = view.cameraCut;
        inputs.enabled = environment.valid && view.valid &&
            environment.ssfxSsrAvailable && environment.ssfxSsrQuality == 0u;
        if (inputs.enabled)
        {
            std::string resourceError;
            std::shared_ptr<IR4Texture> sky0;
            std::shared_ptr<IR4Texture> sky1;
            if (!resolveSkyTexture(backend, sky, environment.sky0TextureName, sky0, resourceError) ||
                !resolveSkyTexture(backend, sky, environment.sky1TextureName, sky1, resourceError))
            {
                inputs.enabled = false;
                if (ssfxSsr && ssfxSsr->BlueNoiseTexture() &&
                    (view.engineFrame == 0u || view.engineFrame % 300u == 0u))
                    std::fprintf(stderr, "METAL_SSR_SKIP phase=resources frame=%u reason=\"%s\"\n",
                        view.engineFrame, resourceError.c_str());
            }
            else if (scene.lightingTargets.empty() || !scene.lightingTargets[0] ||
                scene.gBufferTargets.size() < 4u || !scene.gBufferTargets[0] ||
                !scene.gBufferTargets[1] || !scene.gBufferTargets[3] || !scene.depthTarget ||
                !scene.baseLightingStaticGlobals || !ssrSceneColorSnapshot ||
                !ssfxSsr->BlueNoiseTexture())
            {
                inputs.enabled = false;
                if (view.engineFrame == 0u || view.engineFrame % 300u == 0u)
                    std::fprintf(stderr, "METAL_SSR_SKIP phase=resources frame=%u reason=production_inputs_unavailable\n",
                        view.engineFrame);
            }
            else
            {
                inputs.combinedSceneColor = scene.lightingTargets[0];
                inputs.sceneColorSnapshot = ssrSceneColorSnapshot;
                inputs.position = scene.gBufferTargets[0];
                inputs.diffuse = scene.gBufferTargets[1];
                inputs.motionVectors = scene.gBufferTargets[3];
                inputs.blueNoise = ssfxSsr->BlueNoiseTexture();
                inputs.sky0 = std::move(sky0);
                inputs.sky1 = std::move(sky1);
                inputs.depth = scene.depthTarget;
                inputs.staticGlobals = scene.baseLightingStaticGlobals;
                inputs.noFilterSampler = scene.lightingNoFilterSampler;
                inputs.linearSampler = scene.lightingLinearSampler;
                inputs.baseSampler = scene.baseSampler;
                inputs.ssrSetup = environment.ssfxSsr;
                inputs.ssfxSsr2 = environment.ssfxSsr2;
                inputs.underground = environment.ssfxIsUnderground;

                Matrix4 currentView{};
                Matrix4 currentProjection{};
                std::memcpy(currentView.elements, view.view.data(), sizeof(currentView.elements));
                std::memcpy(currentProjection.elements, view.projection.data(), sizeof(currentProjection.elements));
                // R4 sets m_current to the current projection and m_previous
                // to previousViewProjection * inverse(currentView), matching
                // Matrix_previous.mul(saved_viewproj, Device.mInvView).
                std::memcpy(inputs.currentMatrix.data(), currentProjection.elements,
                    sizeof(currentProjection.elements));
                inputs.previousMatrix = inputs.currentMatrix;
                if (view.previousValid && !view.cameraCut)
                {
                    Matrix4 previousView{};
                    Matrix4 previousProjection{};
                    std::memcpy(previousView.elements, view.previousView.data(), sizeof(previousView.elements));
                    std::memcpy(previousProjection.elements, view.previousProjection.data(), sizeof(previousProjection.elements));
                    const Matrix4 previousViewProjection = multiply(previousProjection, previousView);
                    const Matrix4 previousRelative = multiply(previousViewProjection,
                        inverseRigidTransform(currentView));
                    std::memcpy(inputs.previousMatrix.data(), previousRelative.elements,
                        sizeof(previousRelative.elements));
                }
                if (!backend.CopyTexture2D(inputs.combinedSceneColor,
                        inputs.sceneColorSnapshot, error))
                    return false;
            }
        }
        if (!ssfxSsr->Draw(backend, inputs, error))
            return false;
        if (inputs.enabled && (view.engineFrame == 0u || view.engineFrame % 300u == 0u))
            std::fprintf(stderr,
                "METAL_SSR_FRAME frame=%u previous=%u quality=%u extent=%ux%u passes=5 history=adapter_owned\n",
                view.engineFrame, view.previousFrame, environment.ssfxSsrQuality,
                scene.lightingTargets[0]->Descriptor().width,
                scene.lightingTargets[0]->Descriptor().height);
    }

    // Reference R4 places SSFX water after combine/sky and before
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
        inputs.previousView = view.previousView;
        inputs.previousProjection = view.previousProjection;
        inputs.environment = environment;
        inputs.cameraCut = view.cameraCut;
        inputs.previousValid = view.previousValid;
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

    if (dynamicWorld && dynamicWorld->HasForwardLightplanes())
    {
        if (scene.lightingTargets.empty() || !scene.lightingTargets[0] || !scene.depthTarget)
        {
            error = "dynamic model lightplanes require combined color and scene depth";
            return false;
        }
        R4RenderPassDesc lightplanePass;
        lightplanePass.label = "R4 dynamic models lightplanes additive forward pass";
        lightplanePass.useDrawable = false;
        R4ColorAttachmentDesc color;
        color.texture = scene.lightingTargets[0];
        color.loadAction = R4LoadAction::Load;
        color.storeAction = R4StoreAction::Store;
        lightplanePass.colorAttachments = {color};
        lightplanePass.depthStencilAttachment.texture = scene.depthTarget;
        lightplanePass.depthStencilAttachment.depthLoadAction = R4LoadAction::Load;
        lightplanePass.depthStencilAttachment.depthStoreAction = R4StoreAction::Store;
        lightplanePass.depthStencilAttachment.stencilLoadAction = R4LoadAction::Load;
        lightplanePass.depthStencilAttachment.stencilStoreAction = R4StoreAction::Store;
        R4DynamicWorldViewState dynamicView;
        dynamicView.view = view.view;
        dynamicView.projection = view.projection;
        dynamicView.previousView = view.previousView;
        dynamicView.previousProjection = view.previousProjection;
        dynamicView.eye = view.eye;
        dynamicView.engineFrame = view.engineFrame;
        dynamicView.previousFrame = view.previousFrame;
        dynamicView.cameraCut = view.cameraCut;
        dynamicView.valid = view.valid;
        dynamicView.previousValid = view.previousValid;
        if (!backend.BeginRenderPass(lightplanePass, error) ||
            !dynamicWorld->DrawForwardLightplanes(
                backend, dynamicView, backend.EngineWorldSnapshot(), error) ||
            !backend.EndRenderPass(error))
            return false;
    }

    if (rain && rain->IsReady() && (rain->HasDrops() || rain->HasSplashes()))
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
            !rain->DrawRefraction(backend, scene.scopeSceneColorSnapshot,
                volumeFrame ? volumetric->OutputTexture() : scene.lightingBlack,
                scene.baseLightingStaticGlobals, error) || !backend.EndRenderPass(error))
            return false;
        ++rainTelemetryFrame;
        if (rain->HasDrops())
        {
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
        const std::uint64_t sample = rainTelemetryFrame;
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
    }

    if (rain && rain->IsReady() && rain->HasSplashes())
    {
        R4RenderPassDesc pass;
        pass.label = "R4 exact SSFX ground rain splashes"; pass.useDrawable = false;
        R4ColorAttachmentDesc color;
        color.texture = scene.lightingTargets[0]; color.loadAction = R4LoadAction::Load;
        color.storeAction = R4StoreAction::Store; pass.colorAttachments = {color};
        pass.depthStencilAttachment.texture = scene.depthTarget;
        pass.depthStencilAttachment.depthLoadAction = R4LoadAction::Load;
        pass.depthStencilAttachment.depthStoreAction = R4StoreAction::Store;
        pass.depthStencilAttachment.stencilLoadAction = R4LoadAction::Load;
        pass.depthStencilAttachment.stencilStoreAction = R4StoreAction::Store;
        std::array<float, 16> viewProjection{};
        std::memcpy(viewProjection.data(), scene.uniformData.viewProjection.elements, sizeof(viewProjection));
        if (!backend.BeginRenderPass(pass, error)) return false;
        const bool drawn = rain->DrawSplashesIntoActiveForward(backend, viewProjection, rain->RefractionTarget(), error);
        std::string endError; const bool ended = backend.EndRenderPass(endError);
        if (!drawn) return false;
        if (!ended) { error = endError; return false; }
        static std::uint64_t splashDrawSamples = 0;
        if (++splashDrawSamples == 1u || splashDrawSamples % 300u == 0u)
        {
            const auto& t = rain->LastTelemetry();
            std::fprintf(stderr, "METAL_RAIN_SPLASH_FRAME frame=%u vertices=%zu triangles=%zu dropped=%zu\n",
                environment.engineFrame, t.capturedSplashVertices, t.submittedSplashTriangles, t.droppedSplashVertices);
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

    if (volumeFrame)
    {
        const auto& telemetry = volumetric->LastTelemetry();
        if (telemetry.sunDraws || telemetry.spotDraws)
        {
            MetalR4VolumetricAdapter::CompositeInputs inputs;
            inputs.blueNoise = ssfxSsr ? ssfxSsr->BlueNoiseTexture() : nullptr;
            inputs.lightingLow = scene.lightingTargets[0]; inputs.lightingHigh = scene.lightingTargets[1];
            if (!volumetric->DrawFinalComposite(backend, inputs, error)) return false;
            if (!captureColorPhase(backend, view, "volumetric_combine", scene.lightingTargets, {"low", "high"}, error)) return false;
        }
        if (view.engineFrame % 20u == 0u || view.cameraCut)
            std::fprintf(stderr, "METAL_VOLUMETRIC_FRAME frame=%u sun=%zu spot=%zu blur=%zu merged=%d composited=%d divisor=%u\n",
                view.engineFrame, telemetry.sunDraws, telemetry.spotDraws, telemetry.blurDraws,
                telemetry.spotMerged, telemetry.finalComposited, environment.volumetricResolutionDivisor);
    }

    // phase_combine calls phase_bloom/phase_luminance after water and
    // alpha-forward/rain/particles, before distortion, scope and SSFX bloom.
    if (!drawLuminanceAdaptation(backend, scene, environment, error))
        return false;

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

    if (environment.valid && environment.ssfxFogScatteringAvailable &&
        !std::isfinite(environment.ssfxFogScattering))
    {
        error = "active R4 SSFX fog-scattering control is non-finite";
        return false;
    }
    const bool fogScatteringRequested = environment.valid && environment.ssfxFogScatteringAvailable &&
        environment.ssfxFogScattering > 0.0f;
    if (fogScatteringRequested)
    {
        if (!fogScattering || !fogScattering->IsReady())
        {
            error = "active R4 SSFX fog scattering has no prepared converted shader bundle";
            return false;
        }
        if (scene.lightingTargets.empty() || !scene.lightingTargets[0] ||
            scene.gBufferTargets.empty() || !scene.gBufferTargets[0] ||
            !scene.baseLightingStaticGlobals)
        {
            error = "active SSFX fog scattering requires combined color, position, and static_globals";
            return false;
        }
        const R4Texture2DDesc& sceneColor = scene.lightingTargets[0]->Descriptor();
        if (!fogScattering->Draw(backend, scene.gBufferTargets[0], scene.lightingTargets[0],
                scene.baseLightingStaticGlobals, environment, error) ||
            !backend.CopyTexture2D(fogScattering->Output(), scene.lightingTargets[0], error))
            return false;
        static std::uint32_t lastFogScatteringLogFrame = UINT32_MAX;
        if (lastFogScatteringLogFrame == UINT32_MAX ||
            environment.engineFrame - lastFogScatteringLogFrame >= 300u)
        {
            lastFogScatteringLogFrame = environment.engineFrame;
            std::fprintf(stderr,
                "METAL_FOG_SCATTERING_FRAME frame=%u scattering=%.4f fog=%.4f,%.4f,%.4f,%.4f "
                "extent=%ux%u order=after_distortion_before_scope_bloom\n",
                environment.engineFrame, environment.ssfxFogScattering,
                environment.ssfxFog[0], environment.ssfxFog[1], environment.ssfxFog[2],
                environment.ssfxFog[3], sceneColor.width, sceneColor.height);
        }
        if (!captureColorPhase(backend, view, "fog_scattering", {scene.lightingTargets[0]},
                {"scene"}, error))
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

    if (bloom)
    {
        MetalR4BloomAdapter::Inputs inputs;
        inputs.position = scene.gBufferTargets[0];
        inputs.scene = scene.lightingTargets[0];
        inputs.emissive = emissiveTarget;
        // HUD mask and unsupported emitter families remain explicit matrix gaps.
        inputs.hudMask = scene.lightingBlack;
        inputs.staticGlobals = scene.baseLightingStaticGlobals;
        inputs.environment = environment;
        inputs.vision = vision;
        if (!bloom->Draw(backend, inputs, error)) return false;
    }
    if (bloom && !captureColorPhase(backend, view, "bloom", {bloom->Output()}, {"bloom"}, error)) return false;
    if (!captureColorPhase(backend, view, "scene_before_postprocess", scene.lightingTargets, {"low", "high"}, error)) return false;
    if (nightVision && !drawNightVision(
            backend, scene, *nightVision, environment, vision, error))
        return false;

    R4RenderPassDesc displayPass;
    displayPass.label = "R4 engine production presentation";
    displayPass.drawableColor.clearColor = EngineSceneClearColor;
    const auto drawPresentation = [&]() {
        if (particles && particles->IsReady())
        {
            const bool hasLut = scene.lutAtlas &&
                (scene.lutAtlasUploaded || scene.pendingLutAtlasUploaded);
            const std::shared_ptr<IR4Texture>& lut = hasLut ? scene.lutAtlas : scene.lightingBlack;
            const std::array<float, 4> ssfxLut = hasLut ?
                std::array<float, 4>{1.0f, 16.0f, 0.0f, 0.0f} :
                std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f};
            return particles->DrawPostprocess(backend, scene.gBufferTargets[0],
                scene.lightingTargets[0], bloom ? bloom->Output() : scene.lightingBlack, lut,
                scene.baseLightingStaticGlobals, ssfxLut, error);
        }
        return drawGBufferDisplay(backend, scene, error);
    };
    if (!backend.BeginRenderPass(displayPass, error) ||
        !drawPresentation() || !backend.EndRenderPass(error))
        return false;
    if (lensOverlay && lensOverlay->HasFlares())
    {
        R4RenderPassDesc overlayPass;
        overlayPass.label = "R4 exact lens flares after combine_2";
        overlayPass.drawableColor.loadAction = R4LoadAction::Load;
        overlayPass.drawableColor.storeAction = R4StoreAction::Store;
        overlayPass.depthStencilAttachment.texture = scene.depthTarget;
        overlayPass.depthStencilAttachment.depthLoadAction = R4LoadAction::Load;
        overlayPass.depthStencilAttachment.depthStoreAction = R4StoreAction::Store;
        overlayPass.depthStencilAttachment.stencilLoadAction = R4LoadAction::Load;
        overlayPass.depthStencilAttachment.stencilStoreAction = R4StoreAction::Store;
        if (!backend.BeginRenderPass(overlayPass, error)) return false;
        const bool drawn = lensOverlay->DrawIntoActiveForward(backend, scene.baseLightingStaticGlobals, environment.engineFrame, error);
        std::string endError; const bool ended = backend.EndRenderPass(endError);
        if (!drawn) return false;
        if (!ended) { error = endError; return false; }
        const auto& telemetry = lensOverlay->LastTelemetry();
        if (lensFlareTelemetryFrame == 0 || lensFlareTelemetryFrame % 300 == 0)
            std::fprintf(stderr, "METAL_LENS_OVERLAY_FRAME frame=%u submitted=%zu rejected=%zu order=after_combine_2\n",
                environment.engineFrame, telemetry.submittedDraws, telemetry.rejectedDraws);
    }
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
    R4SubmittedViewHistory viewHistory;
    R4DetailGrassMotionSnapshot floraMotion;
    TerrainScene terrain;
    SkyScene sky;
    bool prepared = false;
    mutable std::mutex worldDrawMutex;
    std::shared_ptr<const GBufferWorldDrawCallback> worldDraw;
    std::unique_ptr<MetalR4FloraAdapter> flora;
    std::unique_ptr<MetalR4DefVertexAdapter> defVertex;
    std::unique_ptr<MetalR4DefObjectsLodAdapter> defObjectsLod;
    std::unique_ptr<MetalR4DynamicWorldAdapter> dynamicWorld;
    MetalR4DynamicWorldAdapter::Options dynamicProbeOptions;
    std::unique_ptr<MetalR4HudAdapter> hud;
    std::unique_ptr<MetalR4ScopeAdapter> scope;
    std::unique_ptr<MetalR4ParticleAdapter> particles;
    std::unique_ptr<MetalR4StaticForwardAdapter> staticForward;
    std::unique_ptr<MetalR4SsfxAoAdapter> ssfxAo;
    std::unique_ptr<MetalR4FogScatteringAdapter> fogScattering;
    std::unique_ptr<MetalR4SsrAdapter> ssfxSsr;
    std::shared_ptr<IR4Texture> ssrSceneColorSnapshot;
    std::uint32_t ssrSnapshotWidth = 0u;
    std::uint32_t ssrSnapshotHeight = 0u;
    R4PixelFormat ssrSnapshotFormat = R4PixelFormat::Invalid;
    std::uint32_t lastSsrStatusFrame = UINT32_MAX;
    std::unique_ptr<MetalR4VolumetricAdapter> volumetric;
    std::unique_ptr<MetalR4BloomAdapter> bloom;
    std::unique_ptr<MetalR4LensFlareAdapter> lensFlare;
    std::unique_ptr<MetalR4LensFlareAdapter> lensOverlay;
    std::uint64_t lensFlareTelemetryFrame = 0;
    std::uint32_t emissiveFactorFrame = UINT32_MAX;
    float emissiveFactor = 0.0f;
    std::uint32_t luminanceThresholdFrame = UINT32_MAX;
    float luminanceThreshold = 0.0f;
    std::shared_ptr<IR4Texture> emissiveTarget;
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
    if (const char* shadowDirectory=std::getenv("XRAY_METAL_MSC_FLORA_SHADOW_DIR"))
        options.flora.shadowShaderDirectory=shadowDirectory;
    if (!ResolveMetalR4EngineTextureDdsPath(std::filesystem::path("fx")/"wind_wave.dds",
            options.flora.wavesTexturePath,error)) return false;
    if (const char* shadowDirectory=std::getenv("XRAY_METAL_MSC_DETAIL_SHADOW_DIR"))
        options.detailGrass.shadowShaderDirectory=shadowDirectory;
    if (const char* dir = std::getenv("XRAY_METAL_MSC_FLORA_INSTANCED_SHADOW_DIR"))
        options.flora.instancedShadowShaderDirectory = dir;
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
    if (const char* emissiveDirectory = std::getenv("XRAY_METAL_MSC_EMISSIVE_DIR");
        emissiveDirectory && *emissiveDirectory)
    {
        options.dynamicWorld.emissiveShaderDirectory = emissiveDirectory;
        options.hud.emissiveShaderDirectory = emissiveDirectory;
    }
    if (const char* splashDirectory = std::getenv("XRAY_METAL_MSC_RAIN_SPLASH_DIR");
        splashDirectory && *splashDirectory)
        options.rain.splashShaderDirectory = splashDirectory;
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
    if (const char* bloomDirectory = std::getenv("XRAY_METAL_MSC_BLOOM_DIR");
        bloomDirectory && *bloomDirectory)
    {
        options.bloom.shaderDirectory = bloomDirectory;
        if (!options.ddsBytesProvider && !CreateMetalR4EngineXrfsProvider(options.ddsBytesProvider, error))
            return false;
        options.bloom.ddsBytesProvider = options.ddsBytesProvider;
    }
    if (const char* directory = std::getenv("XRAY_METAL_MSC_EMISSIVE_NONE_DIR"))
        options.dynamicWorld.emissiveNoneShaderDirectory = directory;
    if (const char* directory = std::getenv("XRAY_METAL_MSC_EMISSIVE_SKIN4_DIR"))
        options.hud.emissiveSkin4ShaderDirectory = directory;
    if (const char* directory = std::getenv("XRAY_METAL_MSC_VOLUMETRIC_DIR"))
        options.volumetric.shaderDirectory = directory;
    if (const char* waterDirectory = std::getenv("XRAY_METAL_MSC_WATER_DIR");
        waterDirectory && *waterDirectory)
    {
        options.water.shaderDirectory = waterDirectory;
        if (!options.ddsBytesProvider && !CreateMetalR4EngineXrfsProvider(options.ddsBytesProvider, error))
            return false;
        options.water.ddsBytesProvider = options.ddsBytesProvider;
    }
    if (const char* ssrDirectory = std::getenv("XRAY_METAL_MSC_SSR_DIR");
        ssrDirectory && *ssrDirectory)
        options.ssfxSsr.shaderDirectory = ssrDirectory;
    if (!options.ddsBytesProvider &&
        !CreateMetalR4EngineXrfsProvider(options.ddsBytesProvider, error))
        return false;
    options.ssfxSsr.ddsBytesProvider = options.ddsBytesProvider;
    if (!ResolveMetalR4EngineTextureDdsPath(std::filesystem::path("fx") / "blue_noise.dds",
            options.ssfxSsr.blueNoiseTexturePath, error))
        return false;
    if (const char* aoDirectory = std::getenv("XRAY_METAL_MSC_AO_DIR");
        aoDirectory && *aoDirectory)
        options.ssfxAo.shaderDirectory = aoDirectory;
    if (const char* fogDirectory = std::getenv("XRAY_METAL_MSC_FOG_SCATTERING_DIR");
        fogDirectory && *fogDirectory)
        options.fogScattering.shaderDirectory = fogDirectory;
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
        "METAL_LUMINANCE_PREPARED shaders=bloom_build+bloom_luminance_1,2,3 "
        "binding=automatic_linear chain=256x256,64x64,8x8,1x1 source=rt_Bloom1_RGBA8 bundle=%s\n",
        options.luminanceShaderDirectory.generic_string().c_str());
    if (!options.luminanceShaderDirectory.empty() &&
        !validateE06Receipt(options.luminanceShaderDirectory, "effects-msc-receipt.json",
            "exact_active_e06_leaf_non_msaa", {"stub_notransform_build.vs", "stub_notransform_filter.vs",
                "bloom_build.ps", "bloom_luminance_1.ps", "bloom_luminance_2.ps", "bloom_luminance_3.ps"}, error))
    { Reset(); return false; }
    if (!options.dynamicWorld.emissiveShaderDirectory.empty() &&
        !validateE06Receipt(options.dynamicWorld.emissiveShaderDirectory, "effects-msc-receipt.json",
            "exact_active_e06_leaf_non_msaa", {"deffer_model_flat.vs", "accum_emissivel.ps"}, error))
    { Reset(); return false; }
    if (!options.rain.splashShaderDirectory.empty() &&
        !validateE06Receipt(options.rain.splashShaderDirectory, "effects-msc-receipt.json",
            "exact_active_e06_leaf_non_msaa", {"effects_rain.vs", "effects_rain_splash.ps"}, error))
    { Reset(); return false; }
    if (!options.bloom.shaderDirectory.empty())
    {
        impl->bloom = std::make_unique<MetalR4BloomAdapter>();
        if (!validateE06Receipt(options.bloom.shaderDirectory, "bloom-msc-receipt.json",
                "exact_active_ssfx_bloom_non_msaa", {"stub_screen_space.vs", "pp_blur.ps",
                "ssfx_bloom.ps", "ssfx_bloom_downsample.ps", "ssfx_bloom_upsample.ps",
                "ssfx_bloom_combine.ps", "ssfx_bloom_flares.ps"}, error) ||
            !impl->bloom->Prepare(backend, options.bloom, error))
        { Reset(); return false; }
        std::fprintf(stderr, "METAL_BLOOM_PREPARED bundle=%s shaders=exact_ssfx_chain emissive_producer=admitted_selflightl\n",
            options.bloom.shaderDirectory.string().c_str());
    }
    if (!options.volumetric.shaderDirectory.empty())
    {
        impl->volumetric = std::make_unique<MetalR4VolumetricAdapter>();
        if (!validateE06Receipt(options.volumetric.shaderDirectory, "effects-msc-receipt.json",
                "exact_active_e06_leaf_non_msaa", {"stub_screen_space.vs", "accum_sun.vs",
                "accum_volumetric_sun_nomsaa.ps", "accum_volumetric_nomsaa.vs", "accum_volumetric_nomsaa.ps",
                "ssfx_volumetric_blur.ps", "ssfx_volumetric_combine.ps", "combine_volumetric.ps"}, error)) return false;
        if (!impl->volumetric->Configure(backend, options.volumetric, error) || !impl->volumetric->IsReady()) return false;
        std::fprintf(stderr, "METAL_VOLUMETRIC_PREPARED bundle=%s source=original_sun_spot_blur_combine\n",
            options.volumetric.shaderDirectory.generic_string().c_str());
    }
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
    if (!options.fogScattering.shaderDirectory.empty())
    {
        impl->fogScattering = std::make_unique<MetalR4FogScatteringAdapter>();
        if (!validateE06Receipt(options.fogScattering.shaderDirectory,
                "effects-msc-receipt.json", "exact_active_e06_leaf_non_msaa",
                {"stub_screen_space.vs", "ssfx_fog_scattering_blur.ps",
                 "ssfx_fog_scattering.ps"}, error) ||
            !impl->fogScattering->Prepare(backend, options.fogScattering, error))
        {
            error = "SSFX fog-scattering MSC adapter preparation failed: " + error;
            Reset();
            return false;
        }
    }
    if (!options.lensFlareShaderDirectory.empty())
    {
        impl->lensFlare = std::make_unique<MetalR4LensFlareAdapter>();
        impl->lensOverlay = std::make_unique<MetalR4LensFlareAdapter>();
        MetalR4LensFlareAdapter::Options flareOptions;
        flareOptions.shaderDirectory = options.lensFlareShaderDirectory;
        flareOptions.ddsBytesProvider = options.ddsBytesProvider;
        if (!impl->lensFlare->Configure(backend, flareOptions, error) || !impl->lensFlare->IsReady())
        { Reset(); return false; }
        flareOptions.colorFormat = R4PixelFormat::BGRA8Unorm;
        if (!impl->lensOverlay->Configure(backend, flareOptions, error) || !impl->lensOverlay->IsReady())
        { Reset(); return false; }
        std::fprintf(stderr, "METAL_LENS_FLARE_PREPARED bundle=%s tlab=24/72\n", options.lensFlareShaderDirectory.string().c_str());
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
    impl->ssfxSsr = std::make_unique<MetalR4SsrAdapter>();
    std::string ssrStatus;
    if (!options.ssfxSsr.shaderDirectory.empty() &&
        !validateE06Receipt(options.ssfxSsr.shaderDirectory, "effects-msc-receipt.json",
            "exact_active_e06_leaf_non_msaa", {"stub_screen_space.vs", "ssfx_ssr_gloss.ps", "ssfx_ssr.ps",
            "ssfx_ssr_blur.ps", "ssfx_ssr_combine.ps"}, error)) return false;
    if (!impl->ssfxSsr->Prepare(backend, options.ssfxSsr, ssrStatus))
    {
        error = "general SSFX SSR adapter preparation failed: " + ssrStatus;
        Reset();
        return false;
    }
    if (impl->ssfxSsr->IsReady())
        std::fprintf(stderr, "METAL_SSR_PREPARED shaders=ssfx_ssr_gloss,trace,blur_x,blur_y,combine quality=0 bundle=%s blue_noise=%s\n",
            options.ssfxSsr.shaderDirectory.generic_string().c_str(),
            impl->ssfxSsr->BlueNoiseTexture() ? "ready" : "missing");
    else
        std::fprintf(stderr, "METAL_SSR_SKIP phase=prepare reason=\"%s\" blue_noise=%s\n",
            ssrStatus.c_str(), impl->ssfxSsr->BlueNoiseTexture() ? "ready" : "missing");
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
    if (!floraOptions.instancedShadowShaderDirectory.empty() &&
        !validateE06Receipt(floraOptions.instancedShadowShaderDirectory,
            "flora-instanced-shadow-receipt.json", "e08-flora-instanced-shadow",
            {"shadow_direct_tree_instanced.vs", "shadow_direct_tree_instanced_aref.vs"}, error))
    { Reset(); return false; }
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
    impl->dynamicProbeOptions=options.dynamicWorld;
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
        if (impl->scene.activeOcclusionRequested && impl->localLights->IsReady())
        {
            const char* qualificationReason = nullptr;
            impl->scene.activeOcclusionLocalVolumeShaderQualified =
                qualifyActiveOcclusionLocalVolumeShader(options.localLights.shaderDirectory,
                    qualificationReason);
            impl->scene.activeOcclusionLocalVolumeShaderReason = qualificationReason ?
                qualificationReason : "qualified-pinned-local-volume-vs";
        }
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
                R4FloraViewState floraView=makeFloraView(callbackView,impl->floraEnvironment,
                    impl->floraMotion,impl->scene.baseLightingStaticGlobals);
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
                R4DetailGrassViewState detailView=makeDetailView(callbackView,impl->detailEnvironment);
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
            dynamicView.previousView = callbackView.previousView;
            dynamicView.previousProjection = callbackView.previousProjection;
            dynamicView.eye = callbackView.eye;
            dynamicView.engineFrame = callbackView.engineFrame;
            dynamicView.previousFrame = callbackView.previousFrame;
            dynamicView.cameraCut = callbackView.cameraCut;
            dynamicView.valid = callbackView.valid;
            dynamicView.previousValid = callbackView.previousValid;
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

void MetalR4EngineScene::SubmitEmissiveFactor(std::uint32_t engineFrame, float factor)
{
    if (!impl) impl = std::make_unique<Impl>();
    impl->emissiveFactorFrame = engineFrame;
    impl->emissiveFactor = std::isfinite(factor) ? factor : 0.0f;
}

void MetalR4EngineScene::SubmitLuminanceBloomThreshold(std::uint32_t engineFrame, float threshold)
{
    if (!impl) impl = std::make_unique<Impl>();
    impl->luminanceThresholdFrame = engineFrame;
    impl->luminanceThreshold = std::isfinite(threshold) ? threshold : 0.0f;
}

bool MetalR4EngineScene::Render(
    IR4Backend& backend,
    const ViewState& incomingView,
    const R4EnvironmentSnapshot& environment,
    const R4GameplayVisionSnapshot& vision,
    std::string& error)
{
    if (!impl || !impl->prepared)
    {
        error = "engine scene is not prepared";
        return false;
    }
    if (impl->viewHistory.HasPending())
    { error = "only one primary world view may produce temporal history per Metal submission"; return false; }
    DiscardUnsubmittedFrame();
    std::uint32_t width = 0, height = 0;
    backend.DrawableSize(width, height);
    const ViewState view = impl->viewHistory.Select(incomingView, width, height);
    // Reallocate producers before preparing temporal consumers. Otherwise AO
    // would bind the old-sized history on the first frame after a resize.
    if (!ensureLevelDepthTarget(backend, impl->scene, error)) return false;
    if (impl->scene.activeOcclusionEnabled && activeOcclusionFrameInWindow(view.engineFrame))
        beginActiveOcclusionObservation(impl->scene, view.engineFrame, view.valid);
    impl->floraEnvironment = environment;
    if (view.valid && environment.valid)
    {
        std::string motionError;
        if (!CaptureMetalR4EngineDetailGrassMotion(impl->floraMotion,motionError))
        { error="tree/grass shadow wind snapshot unavailable: "+motionError; return false; }
    }
    const bool e08Timing = std::getenv("XRAY_METAL_E08_GPU_PASSES") && view.engineFrame % 120 == 0;
    const auto e08SceneUpdateStart = e08Timing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (!updateSceneView(backend, impl->scene, view, environment, error))
        return false;
    if (e08Timing)
        std::fprintf(stderr, "METAL_E08_STATIC_UPDATE frame=%u cpu_ms=%.6f materials=%zu\n",
            view.engineFrame, std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - e08SceneUpdateStart).count(), impl->scene.textureMaterials.size());
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
    const auto e01GrassStart = std::chrono::steady_clock::now();
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
    backend.AddE01CpuStage("grass", std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - e01GrassStart).count());
    if (!updateTerrainConstants(backend, impl->scene, impl->terrain, error))
    {
        std::fprintf(stderr, "METAL_TERRAIN_REJECT phase=constants error=%s\n", error.c_str());
        impl->terrain.ready = false;
        error.clear();
    }
    if (impl->dynamicWorld && impl->dynamicWorld->IsEnabled())
    {
        const auto e01DynamicStart = std::chrono::steady_clock::now();
        R4DynamicWorldViewState dynamicView;
        dynamicView.view = view.view;
        dynamicView.projection = view.projection;
        dynamicView.previousView = view.previousView;
        dynamicView.previousProjection = view.previousProjection;
        dynamicView.eye = view.eye;
        dynamicView.engineFrame = view.engineFrame;
        dynamicView.previousFrame = view.previousFrame;
        dynamicView.cameraCut = view.cameraCut;
        dynamicView.valid = view.valid;
        dynamicView.previousValid = view.previousValid;
        if (!impl->dynamicWorld->Prewarm(
                backend, dynamicView, backend.EngineWorldSnapshot(), error))
            return false;
        backend.AddE01CpuStage("dynamic_prewarm", std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - e01DynamicStart).count());
    }
    const auto e08HudStart = e08Timing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (impl->hud && impl->hud->IsReady() && !impl->hud->Prewarm(backend, backend.EngineHudSnapshot(), error))
        return false;
    if (impl->scope && impl->scope->IsReady() &&
        !impl->scope->Prewarm(backend, backend.EngineHudSnapshot(), error))
        return false;
    if (e08Timing)
        std::fprintf(stderr, "METAL_E08_HUD_STAGE frame=%u cpu_ms=%.6f\n", view.engineFrame,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - e08HudStart).count());
    if (impl->particles && impl->particles->IsReady())
    {
        const auto e08ParticleStart = e08Timing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const bool particleArena = std::getenv("XRAY_METAL_E08_PARTICLE_LEGACY") == nullptr;
        if (!impl->particles->PrepareRenderTargets(backend, error) ||
            !impl->particles->StageFrame(backend.Resources(), impl->particleFrame, error,
                particleArena ? &backend : nullptr))
            return false;
        if (e08Timing)
            std::fprintf(stderr, "METAL_E08_PARTICLE_STAGE frame=%u cpu_ms=%.6f draws=%zu vertices=%zu arena=%u\n",
                view.engineFrame, std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - e08ParticleStart).count(),
                impl->particles->LastTelemetry().preparedDraws,
                impl->particles->LastTelemetry().preparedVertices, unsigned(particleArena));
        impl->particleFrame = {};
    }
    if (impl->lensFlare && impl->lensFlare->IsReady())
    {
        xray::metal::lensflare::Frame source, overlay;
        for (auto& draw : impl->lensFlareFrame.draws)
            (draw.afterPostprocess ? overlay.draws : source.draws).push_back(std::move(draw));
        if (!impl->lensFlare->StageFrame(backend, std::move(source), environment.engineFrame, error) ||
            !impl->lensOverlay->StageFrame(backend, std::move(overlay), environment.engineFrame, error))
            return false;
        impl->lensFlareFrame = {};
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
    if (impl->ssfxSsr && impl->ssfxSsr->IsReady() &&
        !impl->scene.lightingTargets.empty() && impl->scene.lightingTargets[0] &&
        !impl->scene.gBufferTargets.empty() && impl->scene.gBufferTargets[0])
    {
        const R4Texture2DDesc& colorTarget = impl->scene.lightingTargets[0]->Descriptor();
        const R4Texture2DDesc& positionTarget = impl->scene.gBufferTargets[0]->Descriptor();
        if (!impl->ssfxSsr->PrepareRenderTargets(backend, colorTarget.width,
                colorTarget.height, colorTarget.format, positionTarget, error))
            return false;
        if (!impl->ssrSceneColorSnapshot ||
            impl->ssrSnapshotWidth != colorTarget.width ||
            impl->ssrSnapshotHeight != colorTarget.height ||
            impl->ssrSnapshotFormat != colorTarget.format)
        {
            impl->ssrSceneColorSnapshot.reset();
            R4Texture2DDesc snapshotDescriptor = colorTarget;
            snapshotDescriptor.storage = R4ResourceStorage::Private;
            snapshotDescriptor.usage = R4TextureUsage::ShaderRead;
            snapshotDescriptor.label = "R4 general SSR pre-effect scene-color snapshot";
            impl->ssrSceneColorSnapshot = backend.Resources().CreateTexture2D(
                snapshotDescriptor, error);
            if (!impl->ssrSceneColorSnapshot)
                return false;
            impl->ssrSnapshotWidth = colorTarget.width;
            impl->ssrSnapshotHeight = colorTarget.height;
            impl->ssrSnapshotFormat = colorTarget.format;
        }
    }
    if (impl->fogScattering && impl->fogScattering->IsReady() && view.valid &&
        environment.valid && environment.ssfxFogScatteringAvailable &&
        std::isfinite(environment.ssfxFogScattering) && environment.ssfxFogScattering > 0.0f)
    {
        if (impl->scene.lightingTargets.empty() || !impl->scene.lightingTargets[0])
        {
            error = "active SSFX fog scattering has no prepared combined scene color";
            return false;
        }
        const R4Texture2DDesc& target = impl->scene.lightingTargets[0]->Descriptor();
        if (!impl->fogScattering->PrepareRenderTargets(backend, target.width,
                target.height, target.format, error))
            return false;
    }
    std::shared_ptr<const GBufferWorldDrawCallback> worldDraw;
    {
        std::lock_guard<std::mutex> lock(impl->worldDrawMutex);
        worldDraw = impl->worldDraw;
    }
    if (impl->emissiveFactorFrame != view.engineFrame &&
        (view.engineFrame == 0u || view.engineFrame % 300u == 0u))
        std::fprintf(stderr, "METAL_EMISSIVE_FACTOR_REJECT frame=%u owner=%u\n",
            view.engineFrame, impl->emissiveFactorFrame);
    impl->scene.luminanceBloomThreshold = impl->luminanceThresholdFrame == view.engineFrame ? impl->luminanceThreshold : 0.0f;
    const bool rendered = renderProductionScene(backend, impl->scene, impl->terrain, impl->sky,
        impl->defVertex.get(), impl->defObjectsLod.get(), impl->staticForward.get(), impl->ssfxAo.get(),
        impl->ssfxAoTelemetryFrame,
        impl->ssfxSsr.get(), impl->ssrSceneColorSnapshot,
        impl->volumetric.get(),
        impl->rain.get(),
        impl->rainTelemetryFrame,
        impl->water.get(),
        impl->lensFlare.get(),
        impl->lensOverlay.get(),
        impl->lensFlareTelemetryFrame,
        impl->localLights.get(),
        &impl->localShadows,
        impl->dynamicWorld.get(), impl->dynamicProbeOptions,
        impl->flora.get(), makeFloraView(view,environment,impl->floraMotion,impl->scene.baseLightingStaticGlobals),
        impl->detailGrass.get(), makeDetailView(view,environment),
        impl->hud.get(),
        impl->scope.get(),
        impl->particles.get(), impl->bloom.get(), impl->emissiveTarget,
        impl->emissiveFactorFrame == view.engineFrame ? impl->emissiveFactor : 0.0f,
        &impl->nightVision, view, environment, vision,
        worldDraw, impl->fogScattering.get(), error);
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
            environment.engineFrame - impl->lastLocalLightTelemetryFrame >= 300u ||
            std::getenv("XRAY_METAL_LOCAL_LIGHT_DIAGNOSTICS") != nullptr;
        if (reportLocalTelemetry)
        {
            std::fprintf(stderr, "METAL_LOCAL_LIGHT_FRAME frame=%u requested=%zu admitted=%zu submitted=%zu shadowed_submitted=%zu culled=%zu unavailable_skipped=%zu shadowed_skipped=%zu shadowed_point_skipped=%zu unsupported_skipped=%zu invalid_skipped=%zu cookie_skipped=%zu marker_resets=%zu\n",
                environment.engineFrame, local.requested, local.admitted, local.submitted,
                local.submittedShadowed, local.culled, local.skippedUnavailable,
                local.skippedShadowed, local.skippedShadowedPoint, local.skippedUnsupported, local.skippedInvalid,
                local.skippedCookie, local.markerResets);
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
                "rejected_dynamic_casters=%zu dynamic_renderables_omitted=%zu tile=%u capacity=%zu batches=%zu\n",
                environment.engineFrame, impl->localShadows.candidates,
                impl->localShadows.renderedLights, impl->localShadows.submittedCasters,
                impl->localShadows.culledCasters,
                impl->localShadows.dynamicCandidates,
                impl->localShadows.submittedDynamicCasters,
                impl->localShadows.culledDynamicCasters,
                impl->localShadows.rejectedDynamicCasters,
                impl->localShadows.omittedDynamicCasters,
                LocalShadowTileSize, LocalShadowTileCapacity, impl->localShadows.batches);
            if (impl->localShadows.mergeStatic)
                std::fprintf(stderr, "METAL_E08_STATIC_MERGE frame=%u source_rows=%zu encoded_draws=%zu cache_payload_bytes=%zu keys=%zu\n",
                    environment.engineFrame, impl->localShadows.submittedCasters, impl->localShadows.mergedEncodedDraws,
                    impl->localShadows.mergeCacheBytes, impl->localShadows.mergeCache.size());
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
    if (rendered && view.valid && environment.valid)
    {
        impl->viewHistory.Stage(view, width, height, backend.LastSubmittedSerial() + 1u);
        if (std::getenv("XRAY_METAL_TEMPORAL_DIAGNOSTICS"))
            std::fprintf(stderr, "METAL_TEMPORAL_STAGE frame=%u previous_frame=%u previous_serial=%llu next_serial=%llu reset=%d extent=%ux%u\n",
                view.engineFrame, view.previousFrame,
                static_cast<unsigned long long>(impl->viewHistory.Serial()),
                static_cast<unsigned long long>(backend.LastSubmittedSerial() + 1u),
                view.cameraCut ? 1 : 0, width, height);
    }
    if (impl->dynamicWorld && impl->dynamicWorld->IsEnabled())
    {
        R4DynamicWorldViewState diagnosticView;
        diagnosticView.view = view.view;
        diagnosticView.projection = view.projection;
        diagnosticView.valid = view.valid;
        diagnosticView.engineFrame = view.engineFrame;
        impl->dynamicWorld->WriteE07Frame(backend, diagnosticView,
            backend.EngineWorldSnapshot(), rendered);
    }
    return rendered;
}

void MetalR4EngineScene::OnFrameSubmitted()
{
    if (!impl) return;
    commitLevelSceneUploads(impl->scene);
    commitTerrainSceneUploads(impl->terrain);
    if (impl->defVertex) impl->defVertex->CommitSubmittedFrame();
    if (impl->defObjectsLod) impl->defObjectsLod->CommitSubmittedFrame();
    if (impl->dynamicWorld) impl->dynamicWorld->CommitSubmittedSkin();
    if (!impl->viewHistory.HasPending()) return;
    impl->viewHistory.Commit();
    if (impl->dynamicWorld) impl->dynamicWorld->CommitSubmittedFrame();
    if (impl && impl->ssfxAo)
        impl->ssfxAo->CommitSubmittedFrame();
    if (impl && impl->ssfxSsr)
        impl->ssfxSsr->CommitSubmittedFrame();
    if (impl && impl->water)
        impl->water->CommitSubmittedFrame();
    if (impl && impl->bloom) impl->bloom->CommitSubmittedFrame();
    if (impl && impl->lensFlare) impl->lensFlare->CommitSubmittedFrame(impl->floraEnvironment.engineFrame);
    if (impl && impl->lensOverlay) impl->lensOverlay->CommitSubmittedFrame(impl->floraEnvironment.engineFrame);
}

void MetalR4EngineScene::DiscardUnsubmittedFrame()
{
    if (!impl) return;
    impl->viewHistory.Discard();
    discardLevelSceneUploads(impl->scene);
    discardTerrainSceneUploads(impl->terrain);
    if (impl->defVertex) impl->defVertex->DiscardUnsubmittedFrame();
    if (impl->defObjectsLod) impl->defObjectsLod->DiscardUnsubmittedFrame();
    if (impl->particles) impl->particles->DiscardFrame();
    if (impl->dynamicWorld) impl->dynamicWorld->DiscardUnsubmittedFrame();
    if (impl->ssfxAo) impl->ssfxAo->DiscardUnsubmittedFrame();
    if (impl->ssfxSsr) impl->ssfxSsr->DiscardUnsubmittedFrame();
    if (impl->water) impl->water->DiscardUnsubmittedFrame();
    if (impl->bloom) impl->bloom->DiscardUnsubmittedFrame();
    if (impl->lensFlare) impl->lensFlare->DiscardUnsubmittedFrame();
    if (impl->lensOverlay) impl->lensOverlay->DiscardUnsubmittedFrame();
}

void MetalR4EngineScene::InvalidateHistory()
{
    if (!impl) return;
    DiscardUnsubmittedFrame();
    impl->viewHistory.Invalidate();
    if (impl->ssfxAo) impl->ssfxAo->InvalidateHistory();
    if (impl->ssfxSsr) impl->ssfxSsr->InvalidateHistory();
    if (impl->dynamicWorld) impl->dynamicWorld->InvalidateHistory();
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
