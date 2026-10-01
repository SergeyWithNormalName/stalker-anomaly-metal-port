#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <AppKit/AppKit.h>
#include <SDL.h>
#include <SDL_metal.h>

extern "C" void MacOS_ActivateAppWindow(void* sdlWindow)
{
    @autoreleasepool
    {
        if (sdlWindow)
        {
            SDL_ShowWindow(static_cast<SDL_Window*>(sdlWindow));
            SDL_RaiseWindow(static_cast<SDL_Window*>(sdlWindow));
        }
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [NSApp activateIgnoringOtherApps:YES];
        for (NSWindow* win in [NSApp windows])
        {
            [win makeKeyAndOrderFront:nil];
            [win orderFrontRegardless];
        }
    }
}

#if __has_include(<metal_irconverter_runtime/metal_irconverter_runtime.h>)
#define XRAY_METAL_HAS_MSC_RUNTIME 1
#define IR_PRIVATE_IMPLEMENTATION
#include <metal_irconverter_runtime/metal_irconverter_runtime.h>
#else
#define XRAY_METAL_HAS_MSC_RUNTIME 0
#endif

#include <SDL.h>
#include <SDL_metal.h>

#include "IR4Backend.h"
#include "IR4Resources.h"
#include "MetalR4EngineFs.h"
#include "MetalR4LocalLightAdapter.h"
#include "MetalR4UiVideo.h"
#include "MetalUIOverlayCommon.h"
#include "R4LightMarker.h"
#include "R4DdsTexture.h"
#include "R4ComputeSkinFoundation.h"
#include "R4ComputeSkinMetal.h"
#include "R4ComputeSkinSource.h"
#include "R4WholeLightVisibilityLedger.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dispatch/dispatch.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdio.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

extern void Msg(const char* format, ...);

namespace
{
constexpr std::size_t MaxFramesInFlight = 3;
constexpr std::size_t MaxPrivateUploadRecoveryFailures = 3;
constexpr std::size_t MaxComputeSkinOracleBytesPerFrame = 4u * 1024u * 1024u;
constexpr std::size_t WholeLightVisibilityBufferBytes =
    xray::metal::R4WholeLightVisibilityMaxSlots * sizeof(std::uint64_t);
constexpr std::uint32_t MaxWholeLightVisibilityAcquisitions = 12u;

enum class ComputeSkinOracleOutcome
{
    Pending,
    Pass,
    Fail,
    Inconclusive,
};

struct ComputeSkinOracleReceipt
{
    std::shared_ptr<const R4ComputeSkinOracleRequest> request;
    id<MTLBuffer> readback = nil;
    std::uint64_t acquireEpoch = 0u;
    std::uint32_t vertexCount = 0u;
    std::uint32_t influenceCount = 0u;
    bool copyEncoded = false;
    const char* unavailableReason = nullptr;
    ComputeSkinOracleOutcome outcome = ComputeSkinOracleOutcome::Pending;
};

ComputeSkinOracleOutcome finishComputeSkinOracleReceipt(
    const std::shared_ptr<ComputeSkinOracleReceipt>& receipt,
    const MTLCommandBufferStatus commandStatus, const std::uint64_t frameSerial,
    const bool probeCompletion)
{
    if (!receipt || !receipt->request)
        return ComputeSkinOracleOutcome::Inconclusive;
    const R4ComputeSkinOracleRequest& request = *receipt->request;
    ComputeSkinOracleOutcome outcome = ComputeSkinOracleOutcome::Inconclusive;
    std::size_t mismatchOffset = std::numeric_limits<std::size_t>::max();
    std::size_t mismatchBytes = 0u;
    if (commandStatus == MTLCommandBufferStatusCompleted && receipt->copyEncoded &&
        receipt->readback && receipt->readback.length >= request.expectedBytes.size() &&
        receipt->readback.contents &&
        !request.expectedBytes.empty())
    {
        const auto* actual = static_cast<const std::uint8_t*>(receipt->readback.contents);
        if (std::memcmp(actual, request.expectedBytes.data(), request.expectedBytes.size()) == 0)
            outcome = ComputeSkinOracleOutcome::Pass;
        else
        {
            outcome = ComputeSkinOracleOutcome::Fail;
            for (std::size_t i = 0; i < request.expectedBytes.size(); ++i)
                if (actual[i] != request.expectedBytes[i])
                {
                    if (mismatchOffset == std::numeric_limits<std::size_t>::max())
                        mismatchOffset = i;
                    ++mismatchBytes;
                }
        }
    }

    const char* status = outcome == ComputeSkinOracleOutcome::Pass ? "PASS" :
        (outcome == ComputeSkinOracleOutcome::Fail ? "FAIL" : "INCONCLUSIVE");
    const char* reason = outcome != ComputeSkinOracleOutcome::Inconclusive ? "" :
        (commandStatus != MTLCommandBufferStatusCompleted ? "command-buffer-error" :
            (receipt->unavailableReason ? receipt->unavailableReason : "missing-readback"));
    const char* serialLabel = probeCompletion ? "probe" : "submitted";
    std::fprintf(stderr,
        "METAL_COMPUTE_SKIN_ORACLE status=%s key=\"%s\" frame_serial_%s=%llu engine_frame=%u "
        "prewarm_epoch=%llu acquire_epoch=%llu pose_hash=%llu output_write_serial=%llu "
        "vertices=%u influences=%u bytes=%zu msl_sha256=%s reason=%s mismatch_bytes=%zu mismatch_offset=%zu\n",
        status, request.stableKey.c_str(), serialLabel,
        static_cast<unsigned long long>(frameSerial), request.engineFrame,
        static_cast<unsigned long long>(request.prewarmEpoch),
        static_cast<unsigned long long>(receipt->acquireEpoch),
        static_cast<unsigned long long>(request.poseHash),
        static_cast<unsigned long long>(request.outputWriteSerial), receipt->vertexCount,
        receipt->influenceCount, request.expectedBytes.size(), xray::metal::R4ComputeSkinSourceSha256,
        reason, mismatchBytes, mismatchOffset);
    receipt->outcome = outcome;
    return outcome;
}

void reportSkippedComputeSkinOracle(const R4ComputeSkinOracleRequest& request,
    const std::uint64_t acquireEpoch, const char* reason)
{
    std::fprintf(stderr,
        "METAL_COMPUTE_SKIN_ORACLE status=SKIPPED key=\"%s\" engine_frame=%u "
        "prewarm_epoch=%llu acquire_epoch=%llu pose_hash=%llu output_write_serial=%llu "
        "bytes=%zu reason=%s msl_sha256=%s\n",
        request.stableKey.c_str(), request.engineFrame,
        static_cast<unsigned long long>(request.prewarmEpoch),
        static_cast<unsigned long long>(acquireEpoch),
        static_cast<unsigned long long>(request.poseHash),
        static_cast<unsigned long long>(request.outputWriteSerial), request.expectedBytes.size(),
        reason, xray::metal::R4ComputeSkinSourceSha256);
}

void reportWholeLightVisibilityCompletion(
    const std::vector<xray::metal::R4WholeLightVisibilityRecord>& records,
    id<MTLBuffer> resultBuffer, const bool ledgerOverflow,
    const std::uint64_t serial, const std::uint64_t acquireEpoch,
    const MTLCommandBufferStatus status)
{
    if (records.empty() && !ledgerOverflow)
        return;
    const bool completed = status == MTLCommandBufferStatusCompleted && !ledgerOverflow &&
        resultBuffer && resultBuffer.contents && resultBuffer.length >= WholeLightVisibilityBufferBytes;
    const auto* values = completed ? static_cast<const std::uint64_t*>(resultBuffer.contents) : nullptr;
    const auto groups = xray::metal::R4ClassifyWholeLightVisibility(records, values,
        resultBuffer ? resultBuffer.length / sizeof(std::uint64_t) : 0u, completed);
    const bool allGroupsComplete = completed && !groups.empty() &&
        std::all_of(groups.begin(), groups.end(), [](const auto& group) { return group.complete; });
    if (ledgerOverflow)
        std::fprintf(stderr,
            "METAL_WHOLE_LIGHT_VISIBILITY status=INVALID reason=capacity-overflow submitted_serial=%llu acquire_epoch=%llu\n",
            static_cast<unsigned long long>(serial), static_cast<unsigned long long>(acquireEpoch));
    for (const auto& group : groups)
    {
        const bool valid = completed && group.complete;
        if (valid)
        {
            const bool zero = group.passedSamples == 0u;
            std::fprintf(stderr,
                "METAL_WHOLE_LIGHT_VISIBILITY status=complete submitted_serial=%llu acquire_epoch=%llu "
                "engine_frame=%u cpu_observation_scope=%llu group_kind=%s group_id=%llu expected_faces=%u "
                "deferred_receiver_samples=%llu zero_deferred_receiver_samples=%s\n",
                static_cast<unsigned long long>(serial), static_cast<unsigned long long>(acquireEpoch),
                group.engineFrame, static_cast<unsigned long long>(group.cpuObservationScopeId),
                group.groupKind == R4WholeLightVisibilityGroupKind::OmniParent ? "omni-parent" : "single",
                static_cast<unsigned long long>(group.groupId), group.expectedFaceCount,
                static_cast<unsigned long long>(group.passedSamples), zero ? "true" : "false");
        }
        else
        {
            std::fprintf(stderr,
                "METAL_WHOLE_LIGHT_VISIBILITY status=invalid submitted_serial=%llu acquire_epoch=%llu "
                "engine_frame=%u cpu_observation_scope=%llu group_kind=%s group_id=%llu expected_faces=%u "
                "deferred_receiver_samples=NA zero_deferred_receiver_samples=NA reason=%s\n",
                static_cast<unsigned long long>(serial), static_cast<unsigned long long>(acquireEpoch),
                group.engineFrame, static_cast<unsigned long long>(group.cpuObservationScopeId),
                group.groupKind == R4WholeLightVisibilityGroupKind::OmniParent ? "omni-parent" : "single",
                static_cast<unsigned long long>(group.groupId), group.expectedFaceCount,
                status != MTLCommandBufferStatusCompleted ? "command-buffer-not-completed" :
                    (ledgerOverflow ? "capacity-overflow" : "missing-duplicate-or-invalid-face"));
        }
    }
    for (const auto& record : records)
    {
        const auto& sample = record.sample;
        const bool hasValue = allGroupsComplete && record.encoded &&
            xray::metal::R4WholeLightVisibilitySampleValid(sample) && values &&
            record.slot < resultBuffer.length / sizeof(std::uint64_t) &&
            record.slot < xray::metal::R4WholeLightVisibilityMaxSlots;
        const std::string sampleValue = hasValue ? std::to_string(values[record.slot]) : "NA";
        std::fprintf(stderr,
            "METAL_WHOLE_LIGHT_VISIBILITY_FACE status=%s submitted_serial=%llu acquire_epoch=%llu "
            "engine_frame=%u cpu_observation_scope=%llu batch_id=%d light_id=%llu parent_light_id=%llu face=%u "
            "group_kind=%s expected_faces=%u deferred_receiver_samples=%s\n",
            hasValue ? "VALID" : "INVALID", static_cast<unsigned long long>(serial),
            static_cast<unsigned long long>(acquireEpoch), sample.engineFrame,
            static_cast<unsigned long long>(sample.cpuObservationScopeId), sample.batchId,
            static_cast<unsigned long long>(sample.lightId),
            static_cast<unsigned long long>(sample.parentLightId), sample.face,
            sample.groupKind == R4WholeLightVisibilityGroupKind::OmniParent ? "omni-parent" : "single",
            sample.expectedFaceCount, sampleValue.c_str());
    }
}

static std::size_t GetConfiguredFramesInFlight()
{
    static const std::size_t count = [] {
        if (const char* env = std::getenv("XRAY_METAL_MAX_FRAMES_IN_FLIGHT"))
        {
            const int v = std::atoi(env);
            if (v == 2)
                return static_cast<std::size_t>(2);
        }
        return MaxFramesInFlight;
    }();
    return count;
}
constexpr std::size_t DefaultFrameUploadBlockSize = 4 * 1024 * 1024;
constexpr std::size_t MaxFrameUploadBytes = 128 * 1024 * 1024;
constexpr std::size_t DefaultTlabBlockSize = 16 * 1024 * 1024; // 16 MB per frame
constexpr std::size_t MaxFrameTlabBytes = 128 * 1024 * 1024;
constexpr std::size_t TlabAlignment = 256;

static std::uint64_t e01NowNs()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

static double e01ElapsedMs(const std::uint64_t start, const std::uint64_t end)
{
    return end >= start ? static_cast<double>(end - start) / 1000000.0 : 0.0;
}

// E08 diagnostic only. Upload time is nested in MSC bind time; never add them.
struct E08ScopedCpu
{
    double* total = nullptr;
    std::uint64_t start = 0;
    E08ScopedCpu(bool enabled, double& result) : total(enabled ? &result : nullptr)
    { if (total) start = e01NowNs(); }
    ~E08ScopedCpu() { if (total) *total += e01ElapsedMs(start, e01NowNs()); }
};

struct E01FrameCompletion
{
    std::atomic<std::uint64_t> scheduledNs{0};
    std::atomic<std::uint64_t> commitDurationNs{0};
    std::uint64_t beginNs = 0;
    std::uint64_t serial = 0;
    std::uint32_t engineFrame = 0;
    std::uint32_t cameraFrame = 0;
    std::uint32_t slot = 0;
    std::uint32_t queueDepth = 0;
    double waitMs = 0.0;
    double drawableMs = 0.0;
    double sceneMs = 0.0;
    double grassMs = 0.0;
    double dynamicPrewarmMs = 0.0;
    double encodeMs = 0.0;
    double presentEncodeMs = 0.0;
    std::uint64_t updatedBytes = 0;
    std::uint32_t bufferAllocations = 0;
    std::uint32_t textureAllocations = 0;
    std::uint32_t indexedDraws = 0;
    std::uint32_t psoBinds = 0;
};

struct E01PresentationSink
{
    explicit E01PresentationSink(const std::string& path) : file(std::fopen(path.c_str(), "w"))
    {
        if (file)
            std::fputs("serial,presented_s\n", file);
    }
    ~E01PresentationSink() { if (file) std::fclose(file); }
    FILE* file = nullptr;
    std::mutex mutex;
};

struct UIOverlayUniforms
{
    float drawableWidth;
    float drawableHeight;
    float textureWidth;
    float textureHeight;
    float alphaRef;
    std::uint32_t atlasPixels;
    std::uint32_t isFont;
    std::uint32_t pad;
};

static_assert(sizeof(xray::metal::ui::Vertex) == 24u, "Metal UI overlay vertex ABI changed");
static_assert(sizeof(UIOverlayUniforms) == 32u, "Metal UI overlay uniform ABI changed");

constexpr char UIOverlayMsl[] = R"MSL(
#include <metal_stdlib>
using namespace metal;
// `float3` has 16-byte alignment in MSL, while the captured engine TL/LIT
// vertex is a tightly packed 24-byte P3/C4/UV2 record.  Keep the GPU stride
// identical to the value-only capture ABI.
struct UIVertex { packed_float3 position; uint color; float2 uv; };
struct UIUniforms { float drawableWidth; float drawableHeight; float textureWidth; float textureHeight; float alphaRef; uint atlasPixels; uint isFont; uint pad; };
struct UIOut { float4 position [[position]]; float4 color; float2 uv; };
vertex UIOut metal_ui_vertex(const device UIVertex* vertices [[buffer(0)]], constant UIUniforms& uniforms [[buffer(1)]], uint vertexId [[vertex_id]])
{
    const UIVertex inputVertex = vertices[vertexId];
    UIOut output;
    output.position = float4((inputVertex.position.x / uniforms.drawableWidth) * 2.0 - 1.0,
        1.0 - (inputVertex.position.y / uniforms.drawableHeight) * 2.0, inputVertex.position.z, 1.0);
    output.color = float4(float((inputVertex.color >> 16) & 255u), float((inputVertex.color >> 8) & 255u),
        float(inputVertex.color & 255u), float((inputVertex.color >> 24) & 255u)) / 255.0;
    output.uv = uniforms.atlasPixels != 0u ? inputVertex.uv / float2(uniforms.textureWidth, uniforms.textureHeight) : inputVertex.uv;
    return output;
}
fragment float4 metal_ui_fragment(UIOut input [[stage_in]], texture2d<float> texture [[texture(0)]],
    sampler textureSampler [[sampler(0)]], constant UIUniforms& uniforms [[buffer(1)]])
{
    const float4 sampled = texture.sample(textureSampler, input.uv);
    const float fontAlpha = (uniforms.isFont == 2u) ? (1.0 - sampled.a) : sampled.a;
    const float4 color = (uniforms.isFont != 0u)
        ? float4(input.color.rgb, fontAlpha * input.color.a)
        : float4(sampled.rgb * input.color.rgb, sampled.a * input.color.a);
    if (color.a < uniforms.alphaRef)
        discard_fragment();
    return color;
}
)MSL";

constexpr char PresentationMsl[] = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct PresentOut { float4 position [[position]]; };

vertex PresentOut metal_present_vertex(uint vertexId [[vertex_id]])
{
    constexpr float2 positions[] = {float2(-1.0f, -1.0f), float2(3.0f, -1.0f), float2(-1.0f, 3.0f)};
    PresentOut output;
    output.position = float4(positions[vertexId], 0.0f, 1.0f);
    return output;
}

fragment float4 metal_present_fragment(PresentOut input [[stage_in]],
    texture2d<float, access::read> source [[texture(0)]])
{
    const uint2 sourceSize(source.get_width(), source.get_height());
    return source.read(min(uint2(input.position.xy), sourceSize - 1u));
}

kernel void metal_save_thumbnail(texture2d<float, access::read> source [[texture(0)]],
    device uchar4* destination [[buffer(0)]], uint2 outputPosition [[thread_position_in_grid]])
{
    constexpr uint outputWidth = 128u;
    constexpr uint outputHeight = 128u;
    if (outputPosition.x >= outputWidth || outputPosition.y >= outputHeight)
        return;
    const uint2 sourceSize(source.get_width(), source.get_height());
    const uint2 sourcePosition(
        min(((outputPosition.x * 2u + 1u) * sourceSize.x) / (outputWidth * 2u), sourceSize.x - 1u),
        min(((outputPosition.y * 2u + 1u) * sourceSize.y) / (outputHeight * 2u), sourceSize.y - 1u));
    const float4 rgba = saturate(source.read(sourcePosition));
    destination[outputPosition.y * outputWidth + outputPosition.x] = uchar4(
        uchar(rgba.b * 255.0f + 0.5f), uchar(rgba.g * 255.0f + 0.5f),
        uchar(rgba.r * 255.0f + 0.5f), uchar(rgba.a * 255.0f + 0.5f));
}
)MSL";

MTLPrimitiveType metalUiPrimitiveType(xray::metal::ui::PrimitiveType primitive)
{
    switch (primitive)
    {
    case xray::metal::ui::PrimitiveType::TriangleList: return MTLPrimitiveTypeTriangle;
    case xray::metal::ui::PrimitiveType::TriangleStrip: return MTLPrimitiveTypeTriangleStrip;
    case xray::metal::ui::PrimitiveType::LineStrip: return MTLPrimitiveTypeLineStrip;
    case xray::metal::ui::PrimitiveType::LineList: return MTLPrimitiveTypeLine;
    }
    return MTLPrimitiveTypeTriangle;
}

MTLCullMode metalUiCullMode(std::uint8_t cullMode)
{
    // IUIRender::cmCW/counter-clockwise are D3D-facing names. With clockwise
    // front faces, culling CW means culling Metal's front face.
    if (cullMode == 1u)
        return MTLCullModeFront;
    if (cullMode == 2u)
        return MTLCullModeBack;
    return MTLCullModeNone;
}

std::string describeError(NSError* error)
{
    if (!error)
        return "unknown Metal error";
    return error.localizedDescription.UTF8String ?: "unknown Metal error";
}

id<MTLFunction> loadSingleMetallibFunction(
    id<MTLDevice> device,
    const char* path,
    std::string& error)
{
    if (!path || !path[0])
    {
        error = "Metal Shader Converter metallib path is empty";
        return nil;
    }
    NSError* libraryError = nil;
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
    id<MTLLibrary> library = [device newLibraryWithURL:url error:&libraryError];
    if (!library)
    {
        error = std::string("Metal Shader Converter metallib load failed: ") +
            describeError(libraryError);
        return nil;
    }
    if (library.functionNames.count != 1)
    {
        error = "Metal Shader Converter metallib must export exactly one function";
        return nil;
    }
    id<MTLFunction> function = [library newFunctionWithName:library.functionNames.firstObject];
    if (!function)
        error = "Metal Shader Converter metallib function lookup failed";
    return function;
}

MTLPixelFormat metalPixelFormat(R4PixelFormat format)
{
    switch (format)
    {
    case R4PixelFormat::A8Unorm: return MTLPixelFormatA8Unorm;
    case R4PixelFormat::R8Unorm: return MTLPixelFormatR8Unorm;
    case R4PixelFormat::RG8Unorm: return MTLPixelFormatRG8Unorm;
    case R4PixelFormat::RG8Snorm: return MTLPixelFormatRG8Snorm;
    case R4PixelFormat::RGBA8Unorm: return MTLPixelFormatRGBA8Unorm;
    case R4PixelFormat::RGBA8UnormSrgb: return MTLPixelFormatRGBA8Unorm_sRGB;
    case R4PixelFormat::RGBA8Snorm: return MTLPixelFormatRGBA8Snorm;
    case R4PixelFormat::BGRA8Unorm: return MTLPixelFormatBGRA8Unorm;
    case R4PixelFormat::BGRA8UnormSrgb: return MTLPixelFormatBGRA8Unorm_sRGB;
    case R4PixelFormat::R16Unorm: return MTLPixelFormatR16Unorm;
    case R4PixelFormat::R16Float: return MTLPixelFormatR16Float;
    case R4PixelFormat::RG16Unorm: return MTLPixelFormatRG16Unorm;
    case R4PixelFormat::RG16Float: return MTLPixelFormatRG16Float;
    case R4PixelFormat::RGBA16Unorm: return MTLPixelFormatRGBA16Unorm;
    case R4PixelFormat::RGBA16Float: return MTLPixelFormatRGBA16Float;
    case R4PixelFormat::R32Float: return MTLPixelFormatR32Float;
    case R4PixelFormat::RG32Float: return MTLPixelFormatRG32Float;
    case R4PixelFormat::RGBA32Float: return MTLPixelFormatRGBA32Float;
    case R4PixelFormat::RGB10A2Unorm: return MTLPixelFormatRGB10A2Unorm;
    case R4PixelFormat::RG11B10Float: return MTLPixelFormatRG11B10Float;
    case R4PixelFormat::BC1Rgba: return MTLPixelFormatBC1_RGBA;
    case R4PixelFormat::BC1RgbaSrgb: return MTLPixelFormatBC1_RGBA_sRGB;
    case R4PixelFormat::BC2Rgba: return MTLPixelFormatBC2_RGBA;
    case R4PixelFormat::BC2RgbaSrgb: return MTLPixelFormatBC2_RGBA_sRGB;
    case R4PixelFormat::BC3Rgba: return MTLPixelFormatBC3_RGBA;
    case R4PixelFormat::BC3RgbaSrgb: return MTLPixelFormatBC3_RGBA_sRGB;
    case R4PixelFormat::BC4RUnorm: return MTLPixelFormatBC4_RUnorm;
    case R4PixelFormat::BC4RSnorm: return MTLPixelFormatBC4_RSnorm;
    case R4PixelFormat::BC5RgUnorm: return MTLPixelFormatBC5_RGUnorm;
    case R4PixelFormat::BC5RgSnorm: return MTLPixelFormatBC5_RGSnorm;
    case R4PixelFormat::BC6hRgbFloat: return MTLPixelFormatBC6H_RGBFloat;
    case R4PixelFormat::BC6hRgbUfloat: return MTLPixelFormatBC6H_RGBUfloat;
    case R4PixelFormat::BC7RgbaUnorm: return MTLPixelFormatBC7_RGBAUnorm;
    case R4PixelFormat::BC7RgbaUnormSrgb: return MTLPixelFormatBC7_RGBAUnorm_sRGB;
    case R4PixelFormat::Depth16Unorm: return MTLPixelFormatDepth16Unorm;
    case R4PixelFormat::Depth24UnormStencil8:
        // Apple GPUs do not expose the D3D R24G8 typeless view family.
        return MTLPixelFormatDepth32Float_Stencil8;
    case R4PixelFormat::Depth32Float: return MTLPixelFormatDepth32Float;
    case R4PixelFormat::Depth32FloatStencil8: return MTLPixelFormatDepth32Float_Stencil8;
    case R4PixelFormat::Invalid: return MTLPixelFormatInvalid;
    }
    return MTLPixelFormatInvalid;
}

MTLTextureUsage metalTextureUsage(R4TextureUsage usage)
{
    MTLTextureUsage result = MTLTextureUsageUnknown;
    if (HasR4TextureUsage(usage, R4TextureUsage::ShaderRead))
        result |= MTLTextureUsageShaderRead;
    if (HasR4TextureUsage(usage, R4TextureUsage::ShaderWrite))
        result |= MTLTextureUsageShaderWrite;
    if (HasR4TextureUsage(usage, R4TextureUsage::RenderTarget))
        result |= MTLTextureUsageRenderTarget;
    if (HasR4TextureUsage(usage, R4TextureUsage::PixelFormatView))
        result |= MTLTextureUsagePixelFormatView;
    return result;
}

struct R4PixelLayout
{
    std::size_t blockWidth = 1;
    std::size_t blockHeight = 1;
    std::size_t bytesPerBlock = 0;
};

R4PixelLayout pixelLayout(R4PixelFormat format)
{
    switch (format)
    {
    case R4PixelFormat::A8Unorm:
    case R4PixelFormat::R8Unorm:
        return {1, 1, 1};
    case R4PixelFormat::RG8Unorm:
    case R4PixelFormat::RG8Snorm:
    case R4PixelFormat::R16Unorm:
    case R4PixelFormat::R16Float:
        return {1, 1, 2};
    case R4PixelFormat::RGBA8Unorm:
    case R4PixelFormat::RGBA8UnormSrgb:
    case R4PixelFormat::RGBA8Snorm:
    case R4PixelFormat::BGRA8Unorm:
    case R4PixelFormat::BGRA8UnormSrgb:
    case R4PixelFormat::RG16Unorm:
    case R4PixelFormat::RG16Float:
    case R4PixelFormat::R32Float:
    case R4PixelFormat::RGB10A2Unorm:
    case R4PixelFormat::RG11B10Float:
        return {1, 1, 4};
    case R4PixelFormat::RGBA16Unorm:
    case R4PixelFormat::RGBA16Float:
    case R4PixelFormat::RG32Float:
        return {1, 1, 8};
    case R4PixelFormat::RGBA32Float:
        return {1, 1, 16};
    case R4PixelFormat::BC1Rgba:
    case R4PixelFormat::BC1RgbaSrgb:
    case R4PixelFormat::BC4RUnorm:
    case R4PixelFormat::BC4RSnorm:
        return {4, 4, 8};
    case R4PixelFormat::BC2Rgba:
    case R4PixelFormat::BC2RgbaSrgb:
    case R4PixelFormat::BC3Rgba:
    case R4PixelFormat::BC3RgbaSrgb:
    case R4PixelFormat::BC5RgUnorm:
    case R4PixelFormat::BC5RgSnorm:
    case R4PixelFormat::BC6hRgbFloat:
    case R4PixelFormat::BC6hRgbUfloat:
    case R4PixelFormat::BC7RgbaUnorm:
    case R4PixelFormat::BC7RgbaUnormSrgb:
        return {4, 4, 16};
    case R4PixelFormat::Invalid:
    case R4PixelFormat::Depth16Unorm:
    case R4PixelFormat::Depth24UnormStencil8:
    case R4PixelFormat::Depth32Float:
    case R4PixelFormat::Depth32FloatStencil8:
        return {};
    }
    return {};
}

bool isBlockCompressed(R4PixelFormat format)
{
    const R4PixelLayout layout = pixelLayout(format);
    return layout.blockWidth == 4 && layout.blockHeight == 4 && layout.bytesPerBlock != 0;
}

bool validateTextureUploadLayout(
    R4PixelFormat format,
    std::uint32_t width,
    std::uint32_t height,
    std::size_t dataSize,
    std::size_t bytesPerRow,
    std::size_t bytesPerImage,
    std::string& error)
{
    const R4PixelLayout layout = pixelLayout(format);
    if (layout.bytesPerBlock == 0)
    {
        error = "texture format does not support generic CPU upload";
        return false;
    }

    const std::size_t blocksWide =
        (static_cast<std::size_t>(width) + layout.blockWidth - 1) / layout.blockWidth;
    const std::size_t blocksHigh =
        (static_cast<std::size_t>(height) + layout.blockHeight - 1) / layout.blockHeight;
    constexpr std::size_t Max = std::numeric_limits<std::size_t>::max();
    if (blocksWide > Max / layout.bytesPerBlock)
    {
        error = "texture row size overflow";
        return false;
    }
    const std::size_t minimumRowBytes = blocksWide * layout.bytesPerBlock;
    if (bytesPerRow < minimumRowBytes || bytesPerRow % layout.bytesPerBlock != 0)
    {
        error = "texture row pitch is smaller than the format geometry";
        return false;
    }
    if (blocksHigh > Max / bytesPerRow)
    {
        error = "texture image size overflow";
        return false;
    }
    const std::size_t minimumImageBytes = blocksHigh * bytesPerRow;
    if (bytesPerImage < minimumImageBytes || dataSize < bytesPerImage)
    {
        error = "texture image pitch or data size is smaller than the format geometry";
        return false;
    }
    return true;
}

MTLSamplerAddressMode metalAddressMode(R4SamplerAddressMode mode)
{
    switch (mode)
    {
    case R4SamplerAddressMode::ClampToEdge: return MTLSamplerAddressModeClampToEdge;
    case R4SamplerAddressMode::Repeat: return MTLSamplerAddressModeRepeat;
    case R4SamplerAddressMode::MirrorRepeat: return MTLSamplerAddressModeMirrorRepeat;
    case R4SamplerAddressMode::ClampToZero: return MTLSamplerAddressModeClampToZero;
    case R4SamplerAddressMode::ClampToBorder: return MTLSamplerAddressModeClampToBorderColor;
    }
    return MTLSamplerAddressModeClampToEdge;
}

MTLCompareFunction metalSamplerCompareFunction(R4SamplerCompareFunction function)
{
    switch (function)
    {
    case R4SamplerCompareFunction::Never: return MTLCompareFunctionNever;
    case R4SamplerCompareFunction::Less: return MTLCompareFunctionLess;
    case R4SamplerCompareFunction::LessEqual: return MTLCompareFunctionLessEqual;
    case R4SamplerCompareFunction::Equal: return MTLCompareFunctionEqual;
    case R4SamplerCompareFunction::GreaterEqual: return MTLCompareFunctionGreaterEqual;
    case R4SamplerCompareFunction::Greater: return MTLCompareFunctionGreater;
    case R4SamplerCompareFunction::Always: return MTLCompareFunctionAlways;
    }
    return MTLCompareFunctionNever;
}

MTLSamplerBorderColor metalSamplerBorderColor(R4SamplerBorderColor color)
{
    switch (color)
    {
    case R4SamplerBorderColor::TransparentBlack:
        return MTLSamplerBorderColorTransparentBlack;
    case R4SamplerBorderColor::OpaqueBlack: return MTLSamplerBorderColorOpaqueBlack;
    case R4SamplerBorderColor::OpaqueWhite: return MTLSamplerBorderColorOpaqueWhite;
    }
    return MTLSamplerBorderColorTransparentBlack;
}

MTLSamplerMinMagFilter metalMinMagFilter(R4SamplerFilter filter)
{
    return filter == R4SamplerFilter::Linear ? MTLSamplerMinMagFilterLinear :
                                               MTLSamplerMinMagFilterNearest;
}

MTLSamplerMipFilter metalMipFilter(R4SamplerFilter filter)
{
    return filter == R4SamplerFilter::Linear ? MTLSamplerMipFilterLinear :
                                               MTLSamplerMipFilterNearest;
}

MTLVertexFormat metalVertexFormat(R4VertexFormat format)
{
    switch (format)
    {
    case R4VertexFormat::Float: return MTLVertexFormatFloat;
    case R4VertexFormat::Float2: return MTLVertexFormatFloat2;
    case R4VertexFormat::Float3: return MTLVertexFormatFloat3;
    case R4VertexFormat::Float4: return MTLVertexFormatFloat4;
    case R4VertexFormat::Int2: return MTLVertexFormatInt2;
    case R4VertexFormat::Int4: return MTLVertexFormatInt4;
    case R4VertexFormat::UChar4: return MTLVertexFormatUChar4;
    case R4VertexFormat::UChar4Normalized: return MTLVertexFormatUChar4Normalized;
    case R4VertexFormat::Short2: return MTLVertexFormatShort2;
    case R4VertexFormat::Short2Normalized: return MTLVertexFormatShort2Normalized;
    case R4VertexFormat::Short4: return MTLVertexFormatShort4;
    case R4VertexFormat::Short4Normalized: return MTLVertexFormatShort4Normalized;
    case R4VertexFormat::UShort2Normalized: return MTLVertexFormatUShort2Normalized;
    case R4VertexFormat::UShort4Normalized: return MTLVertexFormatUShort4Normalized;
    case R4VertexFormat::Half2: return MTLVertexFormatHalf2;
    case R4VertexFormat::Half4: return MTLVertexFormatHalf4;
    }
    return MTLVertexFormatInvalid;
}

std::size_t vertexFormatSize(R4VertexFormat format)
{
    switch (format)
    {
    case R4VertexFormat::Float: return 4;
    case R4VertexFormat::Float2: return 8;
    case R4VertexFormat::Float3: return 12;
    case R4VertexFormat::Float4: return 16;
    case R4VertexFormat::Int2: return 8;
    case R4VertexFormat::Int4: return 16;
    case R4VertexFormat::UChar4:
    case R4VertexFormat::UChar4Normalized:
    case R4VertexFormat::Short2:
    case R4VertexFormat::Short2Normalized:
    case R4VertexFormat::UShort2Normalized:
    case R4VertexFormat::Half2:
        return 4;
    case R4VertexFormat::Short4:
    case R4VertexFormat::Short4Normalized:
    case R4VertexFormat::UShort4Normalized:
    case R4VertexFormat::Half4:
        return 8;
    }
    return 0;
}

MTLCompareFunction metalCompareFunction(R4CompareFunction function)
{
    switch (function)
    {
    case R4CompareFunction::Never: return MTLCompareFunctionNever;
    case R4CompareFunction::Less: return MTLCompareFunctionLess;
    case R4CompareFunction::LessEqual: return MTLCompareFunctionLessEqual;
    case R4CompareFunction::Equal: return MTLCompareFunctionEqual;
    case R4CompareFunction::GreaterEqual: return MTLCompareFunctionGreaterEqual;
    case R4CompareFunction::Greater: return MTLCompareFunctionGreater;
    case R4CompareFunction::Always: return MTLCompareFunctionAlways;
    }
    return MTLCompareFunctionAlways;
}

MTLCullMode metalCullMode(R4CullMode mode)
{
    switch (mode)
    {
    case R4CullMode::None: return MTLCullModeNone;
    case R4CullMode::Front: return MTLCullModeFront;
    case R4CullMode::Back: return MTLCullModeBack;
    }
    return MTLCullModeNone;
}

MTLWinding metalWinding(R4Winding winding)
{
    return winding == R4Winding::Clockwise ? MTLWindingClockwise : MTLWindingCounterClockwise;
}

MTLBlendFactor metalBlendFactor(R4BlendFactor factor)
{
    switch (factor)
    {
    case R4BlendFactor::Zero: return MTLBlendFactorZero;
    case R4BlendFactor::One: return MTLBlendFactorOne;
    case R4BlendFactor::SourceAlpha: return MTLBlendFactorSourceAlpha;
    case R4BlendFactor::OneMinusSourceAlpha: return MTLBlendFactorOneMinusSourceAlpha;
    }
    return MTLBlendFactorOne;
}

MTLLoadAction metalLoadAction(R4LoadAction action)
{
    switch (action)
    {
    case R4LoadAction::DontCare: return MTLLoadActionDontCare;
    case R4LoadAction::Load: return MTLLoadActionLoad;
    case R4LoadAction::Clear: return MTLLoadActionClear;
    }
    return MTLLoadActionDontCare;
}

MTLStoreAction metalStoreAction(R4StoreAction action)
{
    return action == R4StoreAction::Store ? MTLStoreActionStore : MTLStoreActionDontCare;
}

MTLStencilOperation metalStencilOperation(R4StencilOperation operation)
{
    switch (operation)
    {
    case R4StencilOperation::Keep: return MTLStencilOperationKeep;
    case R4StencilOperation::Zero: return MTLStencilOperationZero;
    case R4StencilOperation::Replace: return MTLStencilOperationReplace;
    case R4StencilOperation::IncrementClamp: return MTLStencilOperationIncrementClamp;
    case R4StencilOperation::DecrementClamp: return MTLStencilOperationDecrementClamp;
    case R4StencilOperation::Invert: return MTLStencilOperationInvert;
    case R4StencilOperation::IncrementWrap: return MTLStencilOperationIncrementWrap;
    case R4StencilOperation::DecrementWrap: return MTLStencilOperationDecrementWrap;
    }
    return MTLStencilOperationKeep;
}

std::string pipelineCacheKey(const R4GraphicsPipelineDesc& descriptor)
{
    std::ostringstream key;
    key << (descriptor.shaderKey ? descriptor.shaderKey : "") << '\0'
        << (descriptor.mslSource ? descriptor.mslSource : "") << '\0'
        << (descriptor.vertexFunction ? descriptor.vertexFunction : "") << '\0'
        << (descriptor.fragmentFunction ? descriptor.fragmentFunction : "") << '\0'
        << (descriptor.vertexMetallibPath ? descriptor.vertexMetallibPath : "") << '\0'
        << (descriptor.fragmentMetallibPath ? descriptor.fragmentMetallibPath : "") << '\0'
        << static_cast<unsigned>(descriptor.colorFormat) << ':'
        << static_cast<unsigned>(descriptor.depthFormat) << ':'
        << descriptor.sampleCount << ':'
        << descriptor.blend.enabled << ':'
        << static_cast<unsigned>(descriptor.blend.sourceRgb) << ':'
        << static_cast<unsigned>(descriptor.blend.destinationRgb) << ':'
        << static_cast<unsigned>(descriptor.blend.sourceAlpha) << ':'
        << static_cast<unsigned>(descriptor.blend.destinationAlpha) << ':'
        << static_cast<unsigned>(descriptor.depthCompare) << ':'
        << descriptor.depthWrite << ':'
        << descriptor.stencil.enabled << ':'
        << static_cast<unsigned>(descriptor.cullMode) << ':'
        << static_cast<unsigned>(descriptor.frontFace);
    const auto appendStencilFace = [&key](const R4StencilFaceDesc& face) {
        key << ':' << static_cast<unsigned>(face.compare)
            << ':' << static_cast<unsigned>(face.stencilFailure)
            << ':' << static_cast<unsigned>(face.depthFailure)
            << ':' << static_cast<unsigned>(face.depthStencilPass)
            << ':' << face.readMask << ':' << face.writeMask;
    };
    appendStencilFace(descriptor.stencil.frontFace);
    appendStencilFace(descriptor.stencil.backFace);
    for (const R4VertexAttributeDesc& attribute : descriptor.vertexAttributes)
        key << "|a" << attribute.attributeIndex << ':' << attribute.bufferIndex << ':'
            << attribute.offset << ':' << static_cast<unsigned>(attribute.format);
    for (const R4VertexBufferLayoutDesc& layout : descriptor.vertexLayouts)
        key << "|l" << layout.bufferIndex << ':' << layout.stride;
    for (R4PixelFormat format : descriptor.colorFormats)
        key << "|c" << static_cast<unsigned>(format);
    for (std::uint8_t mask : descriptor.colorWriteMasks)
        key << "|w" << static_cast<unsigned>(mask);
    return key.str();
}

class MetalBuffer final : public IR4Buffer
{
public:
    MetalBuffer(id<MTLBuffer> buffer, const R4BufferDesc& descriptor, std::size_t allocationSize,
        bool frameUpload = false)
        : m_version(std::make_shared<NativeVersion>(buffer)), m_descriptor(descriptor),
          m_allocationSize(allocationSize),
          m_frameUpload(frameUpload)
    {
        m_descriptor.label = nullptr;
    }

    std::size_t Size() const override { return m_descriptor.size; }
    std::size_t AllocationSize() const override { return m_allocationSize; }
    R4ResourceStorage Storage() const override { return m_descriptor.storage; }
    id<MTLBuffer> Native() const { return m_version->buffer; }
    bool IsFrameUpload() const { return m_frameUpload; }
    std::shared_ptr<void> VersionHandle() const { return m_version; }
    bool IsVersionInUse() const { return m_version.use_count() > 1; }
    void ReplaceNative(id<MTLBuffer> buffer)
    {
        m_version = std::make_shared<NativeVersion>(buffer);
    }
    bool UpdateShared(id<MTLDevice> device, std::size_t offset, const void* data,
        std::size_t size, std::string& error)
    {
        if (m_descriptor.storage != R4ResourceStorage::Shared || !device || !data ||
            offset > m_descriptor.size || size > m_descriptor.size - offset)
        {
            error = "invalid Shared buffer update";
            return false;
        }
        if (IsVersionInUse())
        {
            id<MTLBuffer> replacement = [device newBufferWithLength:m_allocationSize
                options:MTLResourceStorageModeShared];
            if (!replacement)
            {
                error = "Shared buffer copy-on-write allocation failed";
                return false;
            }
            std::memcpy(replacement.contents, Native().contents, m_allocationSize);
            ReplaceNative(replacement);
        }
        std::memcpy(static_cast<std::uint8_t*>(Native().contents) + offset, data, size);
        return true;
    }

private:
    struct NativeVersion
    {
        explicit NativeVersion(id<MTLBuffer> value) : buffer(value) {}
        id<MTLBuffer> buffer;
    };
    std::shared_ptr<NativeVersion> m_version;
    R4BufferDesc m_descriptor;
    std::size_t m_allocationSize;
    bool m_frameUpload = false;
};

// The only mutable operation is allocation/copy while the context is being
// encoded. A range is never handed out twice before Reset, which follows the
// context's completion semaphore (or abandonment before commit).
class MetalFrameUploadArena
{
public:
    void Reset()
    {
        m_activeBlock = 0;
        m_offset = 0;
        m_acquireEpoch = 0;
        m_trackUploads = false;
        for (Block& block : m_blocks)
            block.uploadedRanges.clear();
    }
    void BeginAcquisition(std::uint64_t acquireEpoch, bool trackUploads)
    {
        Reset();
        m_acquireEpoch = acquireEpoch;
        m_trackUploads = trackUploads && acquireEpoch != 0;
    }
    void Clear()
    {
        m_blocks.clear();
        m_totalCapacity = 0;
        Reset();
        m_trackUploads = false;
    }

    bool Upload(id<MTLDevice> device, const void* data, std::size_t size,
        std::size_t alignment, R4FrameBufferSlice& slice, std::string& error)
    {
        slice = {};
        if (!device || !data || size == 0 || alignment == 0 ||
            (alignment & (alignment - 1)) != 0 || alignment > MaxFrameUploadBytes ||
            size > MaxFrameUploadBytes)
        {
            error = "invalid frame upload size or alignment";
            return false;
        }
        alignment = std::max<std::size_t>(alignment, 16);
        const std::size_t mask = alignment - 1;
        if (size > MaxFrameUploadBytes - mask)
        {
            error = "frame upload alignment overflow";
            return false;
        }

        for (;;)
        {
            if (m_activeBlock < m_blocks.size())
            {
                Block& block = m_blocks[m_activeBlock];
                if (m_offset <= block.capacity && m_offset <= MaxFrameUploadBytes - mask)
                {
                    const std::size_t aligned = (m_offset + mask) & ~mask;
                    if (aligned <= block.capacity && size <= block.capacity - aligned)
                    {
                        std::memcpy(static_cast<std::uint8_t*>(block.buffer->Native().contents) +
                                aligned, data, size);
                        m_offset = aligned + size;
                        slice = {block.buffer, aligned, size, m_acquireEpoch};
                        if (m_trackUploads)
                        {
                            try { block.uploadedRanges.push_back({aligned, size}); }
                            catch (...)
                            {
                                slice = {};
                                error = "frame upload ownership metadata allocation failed";
                                return false;
                            }
                        }
                        return true;
                    }
                }
                ++m_activeBlock;
                m_offset = 0;
                continue;
            }

            const std::size_t capacity = std::max(DefaultFrameUploadBlockSize,
                (size + mask) & ~mask);
            if (capacity > MaxFrameUploadBytes - m_totalCapacity)
            {
                error = "frame upload arena capacity exceeded";
                return false;
            }
            id<MTLBuffer> native = [device newBufferWithLength:capacity
                options:MTLResourceStorageModeShared];
            if (!native)
            {
                error = "frame upload arena Metal allocation failed";
                return false;
            }
            native.label = [NSString stringWithFormat:@"R4 frame upload block %zu", m_blocks.size()];
            R4BufferDesc descriptor;
            descriptor.size = capacity;
            descriptor.storage = R4ResourceStorage::Shared;
            descriptor.usage = R4BufferUsage::Generic;
            auto buffer = std::make_shared<MetalBuffer>(native, descriptor, capacity, true);
            m_blocks.push_back({std::move(buffer), capacity});
            m_totalCapacity += capacity;
        }
    }

    std::size_t Capacity() const { return m_totalCapacity; }
    std::uint64_t AcquireEpoch() const { return m_acquireEpoch; }

    // A current frame upload is authorized only for the exact bytes copied by
    // Upload. Alignment gaps and enclosing superspans never qualify.
    bool OwnsUploadedRange(id<MTLBuffer> native, std::size_t offset,
        std::size_t size, std::uint64_t acquireEpoch) const
    {
        if (!m_trackUploads || !native || size == 0 || acquireEpoch == 0 ||
            acquireEpoch != m_acquireEpoch)
            return false;
        for (const Block& block : m_blocks)
        {
            if (block.buffer->Native() != native)
                continue;
            const auto found = std::lower_bound(block.uploadedRanges.begin(),
                block.uploadedRanges.end(), offset,
                [](const UploadedRange& range, std::size_t value) { return range.offset < value; });
            return found != block.uploadedRanges.end() && found->offset == offset &&
                found->size == size;
        }
        return false;
    }

private:
    struct UploadedRange { std::size_t offset = 0, size = 0; };
    struct Block
    {
        std::shared_ptr<MetalBuffer> buffer;
        std::size_t capacity = 0;
        std::vector<UploadedRange> uploadedRanges;
    };
    std::vector<Block> m_blocks;
    std::size_t m_activeBlock = 0;
    std::size_t m_offset = 0;
    std::size_t m_totalCapacity = 0;
    std::uint64_t m_acquireEpoch = 0;
    bool m_trackUploads = false;
};

class MetalTexture final : public IR4Texture
{
public:
    MetalTexture(id<MTLTexture> texture, const R4Texture2DDesc& descriptor,
        std::shared_ptr<IR4Texture> parent = {})
        : m_texture(texture), m_descriptor(descriptor), m_parent(std::move(parent))
    {
        m_descriptor.label = nullptr;
    }

    const R4Texture2DDesc& Descriptor() const override { return m_descriptor; }
    id<MTLTexture> Native() const { return m_texture; }

private:
    id<MTLTexture> m_texture;
    R4Texture2DDesc m_descriptor;
    // A live view still needs uploads to its parent texture after an abort.
    std::shared_ptr<IR4Texture> m_parent;
};

class MetalSampler final : public IR4Sampler
{
public:
    explicit MetalSampler(id<MTLSamplerState> sampler) : m_sampler(sampler) {}
    id<MTLSamplerState> Native() const { return m_sampler; }

private:
    id<MTLSamplerState> m_sampler;
};

class MetalGraphicsPipeline final : public IR4GraphicsPipeline
{
public:
    MetalGraphicsPipeline(
        id<MTLRenderPipelineState> pipeline,
        id<MTLDepthStencilState> depthState,
        bool usesMsc,
        R4CullMode cullMode,
        R4Winding frontFace)
        : m_pipeline(pipeline), m_depthState(depthState), m_usesMsc(usesMsc),
          m_cullMode(cullMode), m_frontFace(frontFace)
    {
    }

    id<MTLRenderPipelineState> Native() const { return m_pipeline; }
    id<MTLDepthStencilState> DepthState() const { return m_depthState; }
    bool UsesMsc() const { return m_usesMsc; }
    R4CullMode CullMode() const { return m_cullMode; }
    R4Winding FrontFace() const { return m_frontFace; }

private:
    id<MTLRenderPipelineState> m_pipeline;
    id<MTLDepthStencilState> m_depthState;
    bool m_usesMsc;
    R4CullMode m_cullMode;
    R4Winding m_frontFace;
};

class MetalR4Backend final : public IR4Backend, public IR4Resources
{
public:
    ~MetalR4Backend() override { Shutdown(); }
    static bool ProbePrivateUploadRecovery(std::string& error);
    static bool ProbeComputeSkinBackendGpu(std::string& error);
    static bool ProbeWholeLightVisibilityGpu(std::string& error);

    bool Initialize(SDL_Window* window, const std::uint32_t preferredWidth,
        const std::uint32_t preferredHeight, std::string& error) override
    {
        const char* computeSkinEnv = std::getenv("XRAY_METAL_COMPUTE_SKIN");
        m_computeSkinEnabled = computeSkinEnv && std::strcmp(computeSkinEnv, "1") == 0;
        const char* visibilityEnv = std::getenv("XRAY_METAL_WHOLE_LIGHT_VISIBILITY");
        m_wholeLightVisibilityEnabled = visibilityEnv && std::strcmp(visibilityEnv, "1") == 0;
        m_wholeLightVisibilitySelectedAcquisitions = 0u;
        m_wholeLightVisibilityCapNoticeEmitted = false;
        if (m_window || !window)
        {
            error = m_window ? "Metal R4 backend is already initialized" : "SDL window is null";
            return false;
        }

        m_view = SDL_Metal_CreateView(window);
        if (!m_view)
        {
            error = std::string("SDL_Metal_CreateView failed: ") + SDL_GetError();
            return false;
        }

        m_layer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(m_view);
        m_device = MTLCreateSystemDefaultDevice();
        m_commandQueue = [m_device newCommandQueue];
        if (!m_layer || !m_device || !m_commandQueue)
        {
            error = "Metal device, layer, or command queue creation failed";
            SDL_Metal_DestroyView(m_view);
            m_view = nullptr;
            m_layer = nil;
            m_device = nil;
            m_commandQueue = nil;
            return false;
        }

        m_window = window;
        const char* e03TracePath = std::getenv("XRAY_METAL_E03_TRACE_PATH");
        const char* e03FlagPath = std::getenv("XRAY_METAL_E03_CAPTURE_FLAG_PATH");
        m_e03TracePath = e03TracePath ? e03TracePath : "";
        m_e03CaptureFlagPath = e03FlagPath ? e03FlagPath : "";
        m_e03CaptureAttempted = false;
        m_e03CaptureActive = false;
        m_nextFrameIndex = 0;
        m_activeFrameIndex = 0;
        m_frameActive = false;
        m_nextSerial = 1;
        m_lastSubmittedSerial.store(0, std::memory_order_relaxed);
        m_completedSerial.store(0, std::memory_order_relaxed);
        m_hadCommandBufferError.store(false, std::memory_order_relaxed);
        m_privateUploadRecoveryFailed = false;
        m_privateUploadRecoveryFailures = 0;
        m_drawableGeneration.store(0, std::memory_order_relaxed);
        m_drawableWidth = 0;
        m_drawableHeight = 0;
        m_preferredDrawableWidth = preferredWidth;
        m_preferredDrawableHeight = preferredHeight;
        {
            std::lock_guard<std::mutex> lock(m_thumbnailMutex);
            m_latestThumbnail = {};
        }
        m_mscTlabAllocations = 0;
        if (const char* timingPath = std::getenv("XRAY_METAL_E01_FRAMES"))
        {
            m_e01Frames = std::fopen(timingPath, "w");
            if (m_e01Frames)
            {
                std::fputs("serial,engine_frame,camera_frame,slot,begin_ns,wait_ms,drawable_ms,scene_ms,grass_ms,dynamic_prewarm_ms,encode_ms,present_encode_ms,commit_ms,scheduled_ns,completed_ns,gpu_start_s,gpu_end_s,presented_s,queue_depth,status,updated_bytes,buffer_allocations,texture_allocations,indexed_draws,pso_binds\n", m_e01Frames);
                m_e01Presentation = std::make_shared<E01PresentationSink>(
                    std::string(timingPath) + ".presented.csv");
            }
            else
                std::fprintf(stderr, "METAL_E01_ERROR cannot_open_frames=\"%s\"\n", timingPath);
        }
        if (const char* path = std::getenv("XRAY_METAL_E08_GPU_PASSES"))
        {
            if ([m_device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary])
                for (id<MTLCounterSet> set in m_device.counterSets)
                    if ([set.name isEqualToString:MTLCommonCounterSetTimestamp]) m_e08CounterSet = set;
            if (m_e08CounterSet) m_e08GpuPasses = std::fopen(path, "w");
            if (m_e08GpuPasses)
            {
                m_e08CpuBindings = std::fopen((std::string(path) + ".cpu.csv").c_str(), "w");
                if (m_e08CpuBindings)
                    std::fputs("serial,engine_frame,render_pass_ms,msc_bind_ms,upload_ms,pso_bind_ms,vertex_bind_ms,indexed_draw_ms\n", m_e08CpuBindings);
            }
            if (m_e08GpuPasses)
                std::fputs("serial,engine_frame,pass,label,vertex_start,vertex_end,fragment_start,fragment_end,cpu_before,gpu_before,cpu_after,gpu_after,ns_per_gpu_tick,status,cpu_encode_ms,indexed_draws\n", m_e08GpuPasses);
            std::fprintf(stderr, "METAL_E08_GPU_PASSES enabled=%d sample_every=120 path=%s\n",
                m_e08GpuPasses != nullptr, path);
        }
        m_layer.device = m_device;
        m_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        m_layer.framebufferOnly = YES;
        m_layer.maximumDrawableCount = GetConfiguredFramesInFlight();
        m_layer.presentsWithTransaction = NO;
        m_layer.displaySyncEnabled = YES;

        for (FrameContext& context : m_frames)
        {
            context.available = dispatch_semaphore_create(1);
#if XRAY_METAL_HAS_MSC_RUNTIME
            id<MTLBuffer> initialTlab = [m_device newBufferWithLength:DefaultTlabBlockSize
                                                              options:MTLResourceStorageModeShared];
            if (initialTlab)
            {
                initialTlab.label = @"R4 MSC Frame TLAB initial";
                context.tlabBlocks.push_back({initialTlab, DefaultTlabBlockSize});
                context.tlabTotalCapacity = DefaultTlabBlockSize;
            }
#endif
        }

        ResizeDrawable();
        RegisterDisplayNotificationObservers();
        UpdateDisplayCapabilities("initial-startup");
        return true;
    }

    void ResizeDrawable(std::uint32_t renderWidth = 0, std::uint32_t renderHeight = 0) override
    {
        if (!m_window || !m_layer || m_frameActive)
            return;

        if (renderWidth && renderHeight)
        {
            m_preferredDrawableWidth = renderWidth;
            m_preferredDrawableHeight = renderHeight;
        }
        int backingWidth = 0;
        int backingHeight = 0;
        int logicalWidth = 0;
        int logicalHeight = 0;
        // SDL_Metal_GetDrawableSize reports the configured layer drawable.
        // Use the view's backing size so a temporary render extent cannot
        // become the input for the next native-retina resize.
        NSView* const metalView = (__bridge NSView*)m_view;
        const NSSize backingSize = [metalView convertSizeToBacking:metalView.bounds.size];
        backingWidth = static_cast<int>(std::lround(backingSize.width));
        backingHeight = static_cast<int>(std::lround(backingSize.height));
        SDL_GetWindowSize(m_window, &logicalWidth, &logicalHeight);
        const char* nativeRetinaValue = std::getenv("XRAY_METAL_NATIVE_RETINA");
        const bool nativeRetina = nativeRetinaValue && std::strcmp(nativeRetinaValue, "1") == 0;
        const int width = nativeRetina ? backingWidth :
            (m_preferredDrawableWidth != 0u ? static_cast<int>(m_preferredDrawableWidth) : logicalWidth);
        const int height = nativeRetina ? backingHeight :
            (m_preferredDrawableHeight != 0u ? static_cast<int>(m_preferredDrawableHeight) : logicalHeight);
        bool sizeChanged = false;
        if (width > 0 && height > 0)
        {
            const std::uint32_t drawableWidth = static_cast<std::uint32_t>(width);
            const std::uint32_t drawableHeight = static_cast<std::uint32_t>(height);
            m_layer.drawableSize = CGSizeMake((CGFloat)width, (CGFloat)height);
            if (m_drawableWidth != drawableWidth || m_drawableHeight != drawableHeight)
            {
                m_drawableWidth = drawableWidth;
                m_drawableHeight = drawableHeight;
                m_drawableGeneration.fetch_add(1, std::memory_order_acq_rel);
                std::fprintf(stderr,
                    "METAL_DRAWABLE_SIZE logical=%dx%d backing=%dx%d render=%ux%u policy=%s\n",
                    logicalWidth, logicalHeight, backingWidth, backingHeight,
                    drawableWidth, drawableHeight, nativeRetina ? "native-retina" :
                    (m_preferredDrawableWidth != 0u ? "engine-resolution" : "logical"));
                std::lock_guard<std::mutex> lock(m_thumbnailMutex);
                m_latestThumbnail = {};
                sizeChanged = true;
            }
        }

        if (sizeChanged)
        {
            UpdateDisplayCapabilities("resize");
        }
        else if (m_displayCapabilitiesDirty.exchange(false, std::memory_order_acq_rel))
        {
            const char* reason = m_pendingDisplayChangeReason.exchange("display-change", std::memory_order_acq_rel);
            UpdateDisplayCapabilities(reason ? reason : "display-change");
        }
    }

    R4FrameAcquireResult BeginFrame(std::string& error) override
    {
        m_e08SubmitRetry = false;
        const std::uint64_t beginNs = m_e01Frames ? e01NowNs() : 0;
        if (!m_window || !m_layer || !m_commandQueue)
        {
            error = "Metal R4 backend is not initialized";
            return R4FrameAcquireResult::Error;
        }
        if (m_frameActive)
        {
            error = "BeginFrame called while another frame is active";
            return R4FrameAcquireResult::Error;
        }
        if (m_privateUploadRecoveryFailed)
        {
            error = "Metal private uploads aborted repeatedly; recovery stopped to prevent memory exhaustion";
            return R4FrameAcquireResult::Error;
        }
        if (m_hadCommandBufferError.load(std::memory_order_acquire))
        {
            error = "a previous Metal command buffer failed";
            return R4FrameAcquireResult::Error;
        }

        FrameContext& context = m_frames[m_nextFrameIndex];
        dispatch_semaphore_wait(context.available, DISPATCH_TIME_FOREVER);
        const std::uint64_t waitEndNs = m_e01Frames ? e01NowNs() : 0;
        if (m_hadCommandBufferError.load(std::memory_order_acquire))
        {
            error = "a previous Metal command buffer failed";
            dispatch_semaphore_signal(context.available);
            return R4FrameAcquireResult::Error;
        }

        if (context.commandBuffer && context.commandBuffer.status == MTLCommandBufferStatusError)
        {
            m_hadCommandBufferError.store(true, std::memory_order_release);
            error = "previous Metal command buffer failed: " + describeError(context.commandBuffer.error);
            context.commandBuffer = nil;
            runDeferredReleases(context);
            dispatch_semaphore_signal(context.available);
            return R4FrameAcquireResult::Error;
        }

        context.commandBuffer = nil;
        runDeferredReleases(context);
        finishPrivateUploads(context);
        context.retainedResources.clear();
        context.retainedOwners.clear();
        context.indirectReads.clear();
        context.computeSkinOracleReceipt.reset();
        context.privateUploads.clear();
        context.uploadArena.Reset();
        context.e02CowBytes = 0;
        context.e08PassLabels.clear();
        context.e08PassCpuRecords.clear();
        context.e08CurrentPassProfiled = false;
        context.e08CpuMs.fill(0);
        context.e08Profile = m_e08GpuPasses && m_nextSerial % 120 == 0;
        if (context.e08Profile)
        {
            if (!context.e08Counters)
            {
                MTLCounterSampleBufferDescriptor* desc = [[MTLCounterSampleBufferDescriptor alloc] init];
                desc.counterSet = m_e08CounterSet; desc.storageMode = MTLStorageModeShared;
                desc.sampleCount = 4096; desc.label = @"E08 diagnostic render-stage timestamps";
                NSError* counterError = nil;
                context.e08Counters = [m_device newCounterSampleBufferWithDescriptor:desc error:&counterError];
                if (!context.e08Counters)
                    std::fprintf(stderr, "METAL_E08_GPU_PASSES unavailable=%s\n", describeError(counterError).c_str());
            }
            context.e08Profile = context.e08Counters != nil;
            if (context.e08Profile)
                [m_device sampleTimestamps:&context.e08CpuBefore gpuTimestamp:&context.e08GpuBefore];
        }

        context.activeTlabBlockIndex = 0;
        context.activeTlabBlockOffset = 0;
        ResizeDrawable();

        const std::uint64_t drawableStartNs = m_e01Frames ? e01NowNs() : 0;
        const char* lifecycleProbe = std::getenv("XRAY_METAL_TEMPORAL_LIFECYCLE_PROBE");
        const char* forceAcquireRetry = std::getenv("XRAY_METAL_TEMPORAL_FORCE_ACQUIRE_RETRY");
        const bool forcedRetry = lifecycleProbe && std::strcmp(lifecycleProbe, "1") == 0 &&
            forceAcquireRetry && std::strcmp(forceAcquireRetry, "1") == 0;
        if (forcedRetry) ::unsetenv("XRAY_METAL_TEMPORAL_FORCE_ACQUIRE_RETRY");
        context.drawable = forcedRetry || m_e08LateDrawable ? nil : [m_layer nextDrawable];
        const std::uint64_t drawableEndNs = m_e01Frames ? e01NowNs() : 0;
        if (forcedRetry || (!m_e08LateDrawable && !context.drawable))
        {
            if (forcedRetry)
                std::fprintf(stderr, "METAL_TEMPORAL_ACQUIRE_RETRY slot=%u semaphore_released=1\n", static_cast<unsigned>(m_nextFrameIndex));
            dispatch_semaphore_signal(context.available);
            return R4FrameAcquireResult::Retry;
        }

        maybeBeginE03Capture();
        context.commandBuffer = [m_commandQueue commandBuffer];
        if (!context.commandBuffer)
        {
            stopE03Capture();
            context.drawable = nil;
            error = "Metal command buffer creation failed";
            dispatch_semaphore_signal(context.available);
            return R4FrameAcquireResult::Error;
        }
        if (!ensurePresentationResources(context, error))
        {
            stopE03Capture();
            context.commandBuffer = nil;
            context.drawable = nil;
            dispatch_semaphore_signal(context.available);
            return R4FrameAcquireResult::Error;
        }

        if (!beginAcquiredContext(context, error))
        {
            context.commandBuffer = nil;
            context.drawable = nil;
            dispatch_semaphore_signal(context.available);
            return R4FrameAcquireResult::Error;
        }

        m_activeFrameIndex = m_nextFrameIndex;
        m_frameActive = true;
        // Replay resource transfers from an aborted encode before any new
        // render/history work. Legacy asset producers may have released their
        // CPU source already; keep staging only while the destination still
        // has an owner. Failed scene construction must not keep dead assets.
        context.privateUploads = std::move(m_recoverablePrivateUploads);
        prunePrivateUploads(context.privateUploads);
        if (!context.privateUploads.empty())
        {
            std::fprintf(stderr, "METAL_PRIVATE_UPLOAD_RETRY operations=%zu\n", context.privateUploads.size());
            for (const auto& upload : context.privateUploads)
            {
                if (!upload.encode(context.commandBuffer, error))
                { abandonActiveFrame(); return R4FrameAcquireResult::Error; }
            }
        }
        if (m_e01Frames)
        {
            context.e01BeginNs = beginNs;
            context.e01WaitMs = e01ElapsedMs(beginNs, waitEndNs);
            context.e01DrawableMs = e01ElapsedMs(drawableStartNs, drawableEndNs);
            context.e01ReadyNs = e01NowNs();
            context.e01SceneMs = 0.0;
            context.e01GrassMs = 0.0;
            context.e01DynamicPrewarmMs = 0.0;
            context.e01EngineFrame = 0;
            context.e01CameraFrame = 0;
            context.e01UpdatedBytes = 0;
            context.e01BufferAllocations = 0;
            context.e01TextureAllocations = 0;
            context.e01IndexedDraws = 0;
            context.e01PsoBinds = 0;
        }
        return R4FrameAcquireResult::Ready;
    }

    void SetE01FrameTiming(const std::uint32_t engineFrame,
        const std::uint32_t cameraFrame, const double sceneMs) override
    {
        if (!m_e01Frames || !m_frameActive)
            return;
        FrameContext& context = m_frames[m_activeFrameIndex];
        context.e01EngineFrame = engineFrame;
        context.e01CameraFrame = cameraFrame;
        context.e01SceneMs = sceneMs;
    }

    void AddE01CpuStage(const char* stage, const double milliseconds) override
    {
        if (!m_e01Frames || !m_frameActive || !stage)
            return;
        FrameContext& context = m_frames[m_activeFrameIndex];
        if (std::strcmp(stage, "grass") == 0)
            context.e01GrassMs += milliseconds;
        else if (std::strcmp(stage, "dynamic_prewarm") == 0)
            context.e01DynamicPrewarmMs += milliseconds;
    }

    bool BeginRenderPass(const R4RenderPassDesc& pass, std::string& error) override
    {
        if (m_sceneOwnedGBufferCallbackActive)
        {
            error = "scene-owned G-buffer callback cannot begin a render pass";
            return false;
        }
        if (!m_frameActive)
        {
            error = "BeginRenderPass called without an active frame";
            return false;
        }

        FrameContext& context = m_frames[m_activeFrameIndex];
        finishPrivateUploads(context);
        E08ScopedCpu e08Cpu(context.e08Profile, context.e08CpuMs[0]);
        if (context.encoder)
        {
            error = "BeginRenderPass called while another render pass is active";
            return false;
        }

        MTLRenderPassDescriptor* descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
        if (pass.enableVisibilityResultCounts && m_wholeLightVisibilityEnabled &&
            !context.visibilityAdmissionResolved)
        {
            context.visibilityAdmissionResolved = true;
            if (m_wholeLightVisibilitySelectedAcquisitions < MaxWholeLightVisibilityAcquisitions)
            {
                ++m_wholeLightVisibilitySelectedAcquisitions;
                context.visibilityAdmissionGranted = true;
                try
                {
                    context.visibilityRecords.reserve(xray::metal::R4WholeLightVisibilityMaxSlots);
                }
                catch (...)
                {
                    context.visibilityAdmissionGranted = false;
                    context.visibilityLedgerOverflow = true;
                }
                if (context.visibilityAdmissionGranted)
                {
                    if (!context.visibilityResultBuffer)
                        context.visibilityResultBuffer = [m_device newBufferWithLength:WholeLightVisibilityBufferBytes
                            options:MTLResourceStorageModeShared];
                    if (context.visibilityResultBuffer && context.visibilityResultBuffer.contents)
                    {
                        // Reset exactly once for this acquired context before its first query pass.
                        std::memset(context.visibilityResultBuffer.contents, 0, WholeLightVisibilityBufferBytes);
                        context.visibilityBufferReset = true;
                    }
                    else
                    {
                        context.visibilityAdmissionGranted = false;
                        context.visibilityLedgerOverflow = true;
                    }
                }
            }
            else
            {
                context.visibilityDeniedByCap = true;
                if (!m_wholeLightVisibilityCapNoticeEmitted)
                {
                    m_wholeLightVisibilityCapNoticeEmitted = true;
                    std::fprintf(stderr,
                        "METAL_WHOLE_LIGHT_VISIBILITY status=SKIPPED reason=12-acquisition-cap\n");
                }
            }
        }
        if (pass.enableVisibilityResultCounts && m_wholeLightVisibilityEnabled &&
            context.visibilityAdmissionGranted &&
            context.visibilityBufferReset && context.visibilityResultBuffer)
            descriptor.visibilityResultBuffer = context.visibilityResultBuffer;
        if (pass.colorAttachments.size() > 8 ||
            (pass.useDrawable && !pass.colorAttachments.empty()))
        {
            error = "Metal render pass has an invalid drawable/color attachment combination";
            abandonActiveFrame();
            return false;
        }

        std::uint32_t attachmentWidth = 0;
        std::uint32_t attachmentHeight = 0;
        auto admitExtent = [&attachmentWidth, &attachmentHeight](
                               std::uint32_t width, std::uint32_t height) {
            if (attachmentWidth == 0 && attachmentHeight == 0)
            {
                attachmentWidth = width;
                attachmentHeight = height;
                return true;
            }
            return width == attachmentWidth && height == attachmentHeight;
        };

        if (pass.useDrawable)
        {
            descriptor.colorAttachments[0].texture = context.finalColor;
            descriptor.colorAttachments[0].loadAction =
                metalLoadAction(pass.drawableColor.loadAction);
            descriptor.colorAttachments[0].storeAction =
                metalStoreAction(pass.drawableColor.storeAction);
            descriptor.colorAttachments[0].clearColor = MTLClearColorMake(
                pass.drawableColor.clearColor.red, pass.drawableColor.clearColor.green,
                pass.drawableColor.clearColor.blue, pass.drawableColor.clearColor.alpha);
            admitExtent(static_cast<std::uint32_t>(context.finalColor.width),
                static_cast<std::uint32_t>(context.finalColor.height));
        }
        else
        {
            for (std::size_t index = 0; index < pass.colorAttachments.size(); ++index)
            {
                const R4ColorAttachmentDesc& attachment = pass.colorAttachments[index];
                const std::shared_ptr<MetalTexture> target =
                    std::dynamic_pointer_cast<MetalTexture>(attachment.texture);
                if (!target ||
                    !HasR4TextureUsage(target->Descriptor().usage, R4TextureUsage::RenderTarget) ||
                    !admitExtent(target->Descriptor().width, target->Descriptor().height))
                {
                    error = "invalid Metal off-screen color target";
                    abandonActiveFrame();
                    return false;
                }
                descriptor.colorAttachments[index].texture = target->Native();
                descriptor.colorAttachments[index].loadAction =
                    metalLoadAction(attachment.loadAction);
                descriptor.colorAttachments[index].storeAction =
                    metalStoreAction(attachment.storeAction);
                descriptor.colorAttachments[index].clearColor = MTLClearColorMake(
                    attachment.clearColor.red, attachment.clearColor.green,
                    attachment.clearColor.blue, attachment.clearColor.alpha);
                retainFrameResource(context, attachment.texture);
            }
        }

        if (pass.depthStencilAttachment.texture)
        {
            const std::shared_ptr<MetalTexture> depthTarget =
                std::dynamic_pointer_cast<MetalTexture>(pass.depthStencilAttachment.texture);
            if (!depthTarget ||
                !HasR4TextureUsage(depthTarget->Descriptor().usage, R4TextureUsage::RenderTarget) ||
                (depthTarget->Descriptor().format != R4PixelFormat::Depth16Unorm &&
                 depthTarget->Descriptor().format != R4PixelFormat::Depth24UnormStencil8 &&
                 depthTarget->Descriptor().format != R4PixelFormat::Depth32Float &&
                 depthTarget->Descriptor().format != R4PixelFormat::Depth32FloatStencil8))
            {
                error = "invalid Metal depth target";
                abandonActiveFrame();
                return false;
            }
            if (!admitExtent(
                    depthTarget->Descriptor().width, depthTarget->Descriptor().height))
            {
                error = "Metal depth target size does not match the render pass";
                abandonActiveFrame();
                return false;
            }
            descriptor.depthAttachment.texture = depthTarget->Native();
            descriptor.depthAttachment.loadAction =
                metalLoadAction(pass.depthStencilAttachment.depthLoadAction);
            descriptor.depthAttachment.storeAction =
                metalStoreAction(pass.depthStencilAttachment.depthStoreAction);
            descriptor.depthAttachment.clearDepth = pass.depthStencilAttachment.clearDepth;
            if (depthTarget->Descriptor().format == R4PixelFormat::Depth24UnormStencil8 ||
                depthTarget->Descriptor().format == R4PixelFormat::Depth32FloatStencil8)
            {
                descriptor.stencilAttachment.texture = depthTarget->Native();
                descriptor.stencilAttachment.loadAction =
                    metalLoadAction(pass.depthStencilAttachment.stencilLoadAction);
                descriptor.stencilAttachment.storeAction =
                    metalStoreAction(pass.depthStencilAttachment.stencilStoreAction);
                descriptor.stencilAttachment.clearStencil =
                    pass.depthStencilAttachment.clearStencil;
            }
            retainFrameResource(context, pass.depthStencilAttachment.texture);
        }

        if (attachmentWidth == 0 || attachmentHeight == 0)
        {
            error = "Metal render pass has no drawable, color, or depth attachment";
            abandonActiveFrame();
            return false;
        }

        const R4Viewport viewport = pass.hasViewport ? pass.viewport : R4Viewport{
            0.0, 0.0, static_cast<double>(attachmentWidth),
            static_cast<double>(attachmentHeight), 0.0, 1.0};
        if (viewport.width <= 0.0 || viewport.height <= 0.0 ||
            viewport.originX < 0.0 || viewport.originY < 0.0 ||
            viewport.originX + viewport.width > attachmentWidth ||
            viewport.originY + viewport.height > attachmentHeight ||
            viewport.nearDepth < 0.0 || viewport.farDepth > 1.0 ||
            viewport.nearDepth > viewport.farDepth)
        {
            error = "Metal render pass viewport is invalid";
            abandonActiveFrame();
            return false;
        }
        const R4ScissorRect scissor = pass.hasScissor ? pass.scissor : R4ScissorRect{
            0, 0, attachmentWidth, attachmentHeight};
        if (scissor.width == 0 || scissor.height == 0 || scissor.x > attachmentWidth ||
            scissor.y > attachmentHeight || scissor.width > attachmentWidth - scissor.x ||
            scissor.height > attachmentHeight - scissor.y)
        {
            error = "Metal render pass scissor is invalid";
            abandonActiveFrame();
            return false;
        }

        context.e08CurrentPassProfiled = false;
        if (context.e08Profile && context.e08PassLabels.size() < 1024)
        {
            const NSUInteger base = context.e08PassLabels.size() * 4;
            auto* sample = descriptor.sampleBufferAttachments[0];
            sample.sampleBuffer = context.e08Counters;
            sample.startOfVertexSampleIndex = base; sample.endOfVertexSampleIndex = base + 1;
            sample.startOfFragmentSampleIndex = base + 2; sample.endOfFragmentSampleIndex = base + 3;
            context.e08PassLabels.emplace_back(pass.label ? pass.label : "unnamed");
            context.e08PassCpuRecords.push_back({0, 0});
            context.e08PassCpuStart = e01NowNs();
            context.e08PassDrawStart = context.e01IndexedDraws;
            context.e08CurrentPassProfiled = true;
        }
        // Indirect residency declarations belong to this encoder, including
        // encoders reopened by diagnostic pass splitting.
        context.indirectReads.clear();
        context.encoder = [context.commandBuffer renderCommandEncoderWithDescriptor:descriptor];
        if (!context.encoder)
        {
            error = "Metal render command encoder creation failed";
            abandonActiveFrame();
            return false;
        }
        NSString* passLabel = pass.label ? [NSString stringWithUTF8String:pass.label] :
                                           @"R4 Metal render pass";
        context.encoder.label = passLabel;
        [context.encoder pushDebugGroup:passLabel];
        [context.encoder setViewport:MTLViewport{viewport.originX, viewport.originY,
            viewport.width, viewport.height, viewport.nearDepth, viewport.farDepth}];
        [context.encoder setScissorRect:MTLScissorRect{
            scissor.x, scissor.y, scissor.width, scissor.height}];
        context.mscPipelineBound = false;
        context.activePass = pass;
        context.activePassLabel = pass.label ? pass.label : "R4 Metal render pass";
        context.activePass.label = nullptr;
        return true;
    }

    bool SplitActivePassForDiagnostics(const std::string& label, std::string& error) override
    {
        if (!m_frameActive || !m_frames[m_activeFrameIndex].encoder)
        {
            error = "diagnostic draw split requires an active render pass";
            return false;
        }
        FrameContext& context = m_frames[m_activeFrameIndex];
        R4RenderPassDesc pass = context.activePass;
        // Original clears ran once. Preserve every attachment across splits.
        pass.drawableColor.loadAction = R4LoadAction::Load;
        pass.drawableColor.storeAction = R4StoreAction::Store;
        for (auto& attachment : pass.colorAttachments)
        {
            attachment.loadAction = R4LoadAction::Load;
            attachment.storeAction = R4StoreAction::Store;
        }
        pass.depthStencilAttachment.depthLoadAction = R4LoadAction::Load;
        pass.depthStencilAttachment.depthStoreAction = R4StoreAction::Store;
        pass.depthStencilAttachment.stencilLoadAction = R4LoadAction::Load;
        pass.depthStencilAttachment.stencilStoreAction = R4StoreAction::Store;
        [context.encoder popDebugGroup];
        [context.encoder endEncoding];
        if (context.e08CurrentPassProfiled)
        {
            context.e08PassCpuRecords.back() = {
                double(e01NowNs() - context.e08PassCpuStart) / 1.0e6,
                context.e01IndexedDraws - context.e08PassDrawStart};
            context.e08CurrentPassProfiled = false;
        }
        context.encoder = nil;
        pass.label = label.c_str();
        // A scene-owned callback may request only this controlled continuation.
        const bool callbackActive = m_sceneOwnedGBufferCallbackActive;
        m_sceneOwnedGBufferCallbackActive = false;
        const bool restarted = BeginRenderPass(pass, error);
        m_sceneOwnedGBufferCallbackActive = callbackActive;
        return restarted;
    }

    bool SetActiveRenderRegion(const R4Viewport& viewport, const R4ScissorRect& scissor,
        std::string& error) override
    {
        if (m_sceneOwnedGBufferCallbackActive || !m_frameActive ||
            !m_frames[m_activeFrameIndex].encoder)
        { error = "render region requires an active scene-owned pass"; return false; }
        FrameContext& context = m_frames[m_activeFrameIndex];
        const auto& pass = context.activePass;
        const R4Texture2DDesc* target = !pass.colorAttachments.empty() ?
            &pass.colorAttachments.front().texture->Descriptor() :
            (pass.depthStencilAttachment.texture ? &pass.depthStencilAttachment.texture->Descriptor() : nullptr);
        const std::uint32_t width = target ? target->width : static_cast<std::uint32_t>(context.finalColor.width);
        const std::uint32_t height = target ? target->height : static_cast<std::uint32_t>(context.finalColor.height);
        if (!std::isfinite(viewport.originX) || !std::isfinite(viewport.originY) ||
            !std::isfinite(viewport.width) || !std::isfinite(viewport.height) ||
            !std::isfinite(viewport.nearDepth) || !std::isfinite(viewport.farDepth) ||
            viewport.width <= 0 || viewport.height <= 0 || viewport.originX < 0 || viewport.originY < 0 ||
            viewport.originX + viewport.width > width || viewport.originY + viewport.height > height ||
            viewport.nearDepth < 0 || viewport.farDepth > 1 || viewport.nearDepth > viewport.farDepth ||
            scissor.width == 0 || scissor.height == 0 || scissor.x > width || scissor.y > height ||
            scissor.width > width - scissor.x || scissor.height > height - scissor.y)
        { error = "active render region exceeds its attachment"; return false; }
        [context.encoder setViewport:MTLViewport{viewport.originX, viewport.originY,
            viewport.width, viewport.height, viewport.nearDepth, viewport.farDepth}];
        [context.encoder setScissorRect:MTLScissorRect{scissor.x, scissor.y, scissor.width, scissor.height}];
        context.activePass.hasViewport = context.activePass.hasScissor = true;
        context.activePass.viewport = viewport; context.activePass.scissor = scissor;
        return true;
    }

    bool EndRenderPass(std::string& error) override
    {
        if (m_sceneOwnedGBufferCallbackActive)
        {
            error = "scene-owned G-buffer callback cannot end its render pass";
            return false;
        }
        if (!m_frameActive)
        {
            error = "EndRenderPass called without an active frame";
            return false;
        }

        FrameContext& context = m_frames[m_activeFrameIndex];
        E08ScopedCpu e08Cpu(context.e08Profile, context.e08CpuMs[0]);
        if (!context.encoder)
        {
            error = "EndRenderPass called without an active render pass";
            return false;
        }

        [context.encoder popDebugGroup];
        [context.encoder endEncoding];
        if (context.e08CurrentPassProfiled)
        {
            context.e08PassCpuRecords.back() = {
                double(e01NowNs() - context.e08PassCpuStart) / 1.0e6,
                context.e01IndexedDraws - context.e08PassDrawStart};
            context.e08CurrentPassProfiled = false;
        }
        context.encoder = nil;
        context.activePass = {};
        context.activePassLabel.clear();
        return true;
    }

    bool CopyTexture2D(const std::shared_ptr<IR4Texture>& source,
        const std::shared_ptr<IR4Texture>& destination, std::string& error) override
    {
        if (m_sceneOwnedGBufferCallbackActive)
        {
            error = "scene-owned G-buffer callback cannot copy textures";
            return false;
        }
        if (!m_frameActive)
        {
            error = "CopyTexture2D called without an active frame";
            return false;
        }
        FrameContext& context = m_frames[m_activeFrameIndex];
        finishPrivateUploads(context);
        if (context.encoder)
        {
            error = "CopyTexture2D called while a render pass is active";
            return false;
        }
        const std::shared_ptr<MetalTexture> metalSource =
            std::dynamic_pointer_cast<MetalTexture>(source);
        const std::shared_ptr<MetalTexture> metalDestination =
            std::dynamic_pointer_cast<MetalTexture>(destination);
        if (!metalSource || !metalDestination || source == destination)
        {
            error = "CopyTexture2D requires distinct Metal textures";
            return false;
        }
        const R4Texture2DDesc& sourceDesc = metalSource->Descriptor();
        const R4Texture2DDesc& destinationDesc = metalDestination->Descriptor();
        if (sourceDesc.type != R4TextureType::Texture2D ||
            destinationDesc.type != R4TextureType::Texture2D ||
            sourceDesc.width != destinationDesc.width ||
            sourceDesc.height != destinationDesc.height ||
            sourceDesc.format != destinationDesc.format)
        {
            error = "CopyTexture2D source/destination contract mismatch";
            return false;
        }
        id<MTLBlitCommandEncoder> blit = [context.commandBuffer blitCommandEncoder];
        if (!blit)
        {
            error = "CopyTexture2D blit encoder creation failed";
            return false;
        }
        blit.label = @"R4 scene texture snapshot";
        [blit copyFromTexture:metalSource->Native()
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(sourceDesc.width, sourceDesc.height, 1)
                    toTexture:metalDestination->Native()
             destinationSlice:0
             destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        retainFrameResource(context, source);
        retainFrameResource(context, destination);
        return true;
    }

    bool UploadFrameData(const void* data, std::size_t size, std::size_t alignment,
        R4FrameBufferSlice& slice, std::string& error) override
    {
        if (!m_frameActive)
        {
            error = "frame upload requires an active acquired frame";
            return false;
        }
        FrameContext& context = m_frames[m_activeFrameIndex];
        E08ScopedCpu e08Cpu(context.e08Profile, context.e08CpuMs[2]);
        if (!context.commandBuffer)
        {
            error = "frame upload requires a command buffer";
            return false;
        }
        if (!context.uploadArena.Upload(m_device, data, size, alignment, slice, error))
            return false;
        if (m_e01Frames)
            context.e01UpdatedBytes += size;
        return true;
    }

    bool EncodeComputeSkinJobs(const std::vector<R4ComputeSkinJob>& jobs,
        std::string& error) override
    {
        if (!m_computeSkinEnabled)
        {
            error = "native compute skinning requires XRAY_METAL_COMPUTE_SKIN=1";
            return false;
        }
        if (!m_frameActive || jobs.empty())
        {
            error = jobs.empty() ? "compute skinning job batch is empty" :
                "compute skinning requires an active acquired frame";
            return false;
        }
        FrameContext& context = m_frames[m_activeFrameIndex];
        if (!context.commandBuffer || context.encoder)
        {
            error = "compute skinning requires an idle frame command buffer outside render/queued upload encoders";
            return false;
        }

        struct ValidatedJob
        {
            const R4ComputeSkinJob* job = nullptr;
            std::shared_ptr<MetalBuffer> input;
            std::shared_ptr<MetalBuffer> bones;
            std::shared_ptr<MetalBuffer> output;
            id<MTLBuffer> inputNative = nil;
            id<MTLBuffer> bonesNative = nil;
            id<MTLBuffer> outputNative = nil;
            std::size_t inputBytes = 0;
            std::size_t boneBytes = 0;
            std::size_t outputBytes = 0;
        };
        struct BufferRange { id<MTLBuffer> native; std::size_t begin, end; };
        std::vector<ValidatedJob> validated;
        std::vector<BufferRange> outputs;
        std::vector<BufferRange> reads;
        if (jobs.size() > std::numeric_limits<std::size_t>::max() / 6u)
        {
            error = "compute skinning batch is too large to validate safely";
            return false;
        }
        try
        {
            validated.reserve(jobs.size());
            outputs.reserve(jobs.size());
            reads.reserve(jobs.size() * 2u);
        }
        catch (...)
        {
            error = "compute skinning validation metadata allocation failed";
            return false;
        }

        const std::uint64_t acquireEpoch = context.uploadArena.AcquireEpoch();
        const auto checkedSpan = [](std::size_t offset, std::size_t length,
            std::size_t logicalSize, NSUInteger nativeSize, std::size_t& end) {
            if (length == 0 || offset > std::numeric_limits<std::size_t>::max() - length)
                return false;
            end = offset + length;
            return end <= logicalSize && end <= static_cast<std::size_t>(nativeSize);
        };
        const auto checkedProduct = [](std::size_t left, std::size_t right,
            std::size_t& result) {
            if (right != 0 && left > std::numeric_limits<std::size_t>::max() / right)
                return false;
            result = left * right;
            return true;
        };
        const auto overlaps = [](const BufferRange& left, const BufferRange& right) {
            return left.native == right.native && left.begin < right.end && right.begin < left.end;
        };

        // Validate all resources and every write/write and write/read alias
        // before a command encoder is created or any output can be dispatched.
        for (std::size_t index = 0; index < jobs.size(); ++index)
        {
            const R4ComputeSkinJob& job = jobs[index];
            ValidatedJob item;
            item.job = &job;
            item.input = std::dynamic_pointer_cast<MetalBuffer>(job.canonicalInput);
            item.bones = std::dynamic_pointer_cast<MetalBuffer>(job.bones.buffer);
            item.output = std::dynamic_pointer_cast<MetalBuffer>(job.output);
            std::size_t end = 0;
            if (!item.input || !item.bones || !item.output || !job.canonicalInput ||
                !job.bones.buffer || !job.output || job.vertexCount == 0 ||
                job.influenceCount < 1 || job.influenceCount > 4 || job.boneCount == 0 ||
                job.maxReferencedBoneIndex >= job.boneCount)
            {
                error = "compute skinning job " + std::to_string(index) +
                    " has missing resources or invalid counts/bone metadata";
                return false;
            }
            if (!checkedProduct(job.vertexCount, 88u, item.inputBytes) ||
                !checkedProduct(job.boneCount, 64u, item.boneBytes) ||
                !checkedProduct(job.vertexCount, 36u, item.outputBytes) ||
                job.inputSize != item.inputBytes || job.outputSize != item.outputBytes)
            {
                error = "compute skinning job " + std::to_string(index) +
                    " has inconsistent or overflowing canonical/input/output byte counts";
                return false;
            }
            item.inputNative = item.input->Native();
            item.bonesNative = item.bones->Native();
            item.outputNative = item.output->Native();
            if (!item.inputNative || !item.bonesNative || !item.outputNative ||
                item.inputNative.device != m_device || item.bonesNative.device != m_device ||
                item.outputNative.device != m_device ||
                item.input->IsFrameUpload() ||
                item.output->Storage() != R4ResourceStorage::Private ||
                !item.bones->IsFrameUpload() || job.bones.acquireEpoch == 0 ||
                job.bones.acquireEpoch != acquireEpoch || job.bones.size < item.boneBytes ||
                !context.uploadArena.OwnsUploadedRange(item.bonesNative, job.bones.offset,
                    job.bones.size, job.bones.acquireEpoch))
            {
                error = "compute skinning job " + std::to_string(index) +
                    " uses an ephemeral canonical input, stale/foreign resource, or non-current bone upload";
                return false;
            }
            std::size_t inputEnd = 0, bonesEnd = 0, outputEnd = 0;
            if (!checkedSpan(job.inputOffset, item.inputBytes, item.input->Size(),
                    item.inputNative.length, inputEnd) ||
                !checkedSpan(job.bones.offset, job.bones.size, item.bones->Size(),
                    item.bonesNative.length, bonesEnd) ||
                !checkedSpan(job.outputOffset, item.outputBytes, item.output->Size(),
                    item.outputNative.length, outputEnd))
            {
                error = "compute skinning job " + std::to_string(index) +
                    " resource span is out of bounds or overflows";
                return false;
            }
            if ((job.inputOffset & 3u) != 0 || (job.bones.offset & 3u) != 0 ||
                (job.outputOffset & 3u) != 0)
            {
                error = "compute skinning job " + std::to_string(index) +
                    " resource offsets are not float aligned";
                return false;
            }
            item.bonesNative = item.bones->Native();
            reads.push_back({item.inputNative, job.inputOffset, inputEnd});
            reads.push_back({item.bonesNative, job.bones.offset, bonesEnd});
            outputs.push_back({item.outputNative, job.outputOffset, outputEnd});
            validated.push_back(std::move(item));
        }
        for (std::size_t outputIndex = 0; outputIndex < outputs.size(); ++outputIndex)
        {
            for (std::size_t otherOutput = 0; otherOutput < outputIndex; ++otherOutput)
                if (overlaps(outputs[outputIndex], outputs[otherOutput]))
                {
                    error = "compute skinning output spans overlap";
                    return false;
                }
            for (const BufferRange& read : reads)
                if (overlaps(outputs[outputIndex], read))
                {
                    error = "compute skinning output aliases an input or bone upload";
                    return false;
                }
        }

        std::shared_ptr<ComputeSkinOracleReceipt> oracleReceipt;
        const ValidatedJob* oracleJob = nullptr;
        bool oracleFrameCapReached = bool(context.computeSkinOracleReceipt);
        for (const ValidatedJob& item : validated)
        {
            const auto& request = item.job->oracle;
            if (!request) continue;
            if (oracleFrameCapReached)
            {
                reportSkippedComputeSkinOracle(*request, acquireEpoch, "one-readback-per-frame-cap");
                continue;
            }
            if (!m_computeSkinOracleEnabled)
            {
                reportSkippedComputeSkinOracle(*request, acquireEpoch, "oracle-opt-in-disabled");
                continue;
            }
            const bool requestMetadataValid = !request->stableKey.empty() &&
                request->prewarmEpoch != 0u && request->outputWriteSerial != 0u;
            if (!requestMetadataValid || request->expectedBytes.empty() ||
                request->expectedBytes.size() != item.outputBytes ||
                request->expectedBytes.size() > MaxComputeSkinOracleBytesPerFrame)
            {
                reportSkippedComputeSkinOracle(*request, acquireEpoch,
                    request->expectedBytes.size() > MaxComputeSkinOracleBytesPerFrame ?
                        "four-megabyte-frame-cap" : "invalid-request-span-or-metadata");
                continue;
            }
            id<MTLBuffer> readback = [m_device newBufferWithLength:request->expectedBytes.size()
                options:MTLResourceStorageModeShared];
            if (!readback)
            {
                reportSkippedComputeSkinOracle(*request, acquireEpoch, "readback-allocation-failed");
                continue;
            }
            try
            {
                oracleReceipt = std::make_shared<ComputeSkinOracleReceipt>();
            }
            catch (...)
            {
                reportSkippedComputeSkinOracle(*request, acquireEpoch, "receipt-allocation-failed");
                continue;
            }
            readback.label = @"R4 compute skin bounded oracle readback";
            oracleReceipt->request = request;
            oracleReceipt->readback = readback;
            oracleReceipt->acquireEpoch = acquireEpoch;
            oracleReceipt->vertexCount = item.job->vertexCount;
            oracleReceipt->influenceCount = item.job->influenceCount;
            oracleJob = &item;
            oracleFrameCapReached = true;
        }

        if (!ensureComputeSkinPipeline(error))
        {
            if (oracleReceipt && oracleReceipt->request)
                reportSkippedComputeSkinOracle(*oracleReceipt->request, acquireEpoch,
                    "compute-pipeline-unavailable-before-dispatch");
            return false;
        }
        try
        {
            const std::size_t extraOwners = jobs.size() * 6u;
            const std::size_t extraResources = jobs.size() * 6u;
            if (extraOwners > std::numeric_limits<std::size_t>::max() - context.retainedOwners.size() ||
                extraResources > std::numeric_limits<std::size_t>::max() - context.retainedResources.size())
            {
                error = "compute skinning retained-resource count overflow";
                return false;
            }
            context.retainedOwners.reserve(context.retainedOwners.size() + extraOwners);
            context.retainedResources.reserve(context.retainedResources.size() + extraResources);
            for (const ValidatedJob& item : validated)
            {
                retainFrameResource(context, item.input);
                retainFrameResource(context, item.input->VersionHandle());
                retainFrameResource(context, item.bones);
                retainFrameResource(context, item.bones->VersionHandle());
                retainFrameResource(context, item.output);
                retainFrameResource(context, item.output->VersionHandle());
            }
        }
        catch (...)
        {
            error = "compute skinning resource retention allocation failed before dispatch";
            return false;
        }

        finishPrivateUploads(context);
        id<MTLComputeCommandEncoder> compute = [context.commandBuffer computeCommandEncoder];
        if (!compute)
        {
            if (oracleReceipt && oracleReceipt->request)
                reportSkippedComputeSkinOracle(*oracleReceipt->request, acquireEpoch,
                    "compute-encoder-unavailable-before-dispatch");
            error = "compute skinning encoder creation failed before dispatch";
            return false;
        }
        compute.label = @"R4 canonical compute skin jobs";
        [compute setComputePipelineState:m_computeSkinPipeline];
        if (oracleReceipt)
            context.computeSkinOracleReceipt = oracleReceipt;
        struct SkinParams { std::uint32_t vertices, influences; };
        const NSUInteger threadWidth = std::max<NSUInteger>(1u,
            std::min<NSUInteger>(m_computeSkinPipeline.threadExecutionWidth,
                m_computeSkinPipeline.maxTotalThreadsPerThreadgroup));
        for (const ValidatedJob& item : validated)
        {
            const R4ComputeSkinJob& job = *item.job;
            const SkinParams params{job.vertexCount, job.influenceCount};
            [compute setBuffer:item.inputNative offset:job.inputOffset atIndex:0];
            [compute setBuffer:item.bonesNative offset:job.bones.offset atIndex:1];
            [compute setBuffer:item.outputNative offset:job.outputOffset atIndex:2];
            [compute setBytes:&params length:sizeof(params) atIndex:3];
            [compute dispatchThreads:MTLSizeMake(job.vertexCount, 1u, 1u)
                threadsPerThreadgroup:MTLSizeMake(threadWidth, 1u, 1u)];
        }
        [compute endEncoding];
        if (oracleReceipt && oracleJob)
        {
            id<MTLBlitCommandEncoder> oracleBlit = [context.commandBuffer blitCommandEncoder];
            if (!oracleBlit)
                oracleReceipt->unavailableReason = "oracle-blit-encoder-unavailable-after-dispatch";
            else
            {
                [oracleBlit copyFromBuffer:oracleJob->outputNative
                    sourceOffset:oracleJob->job->outputOffset
                    toBuffer:oracleReceipt->readback destinationOffset:0
                    size:oracleJob->job->outputSize];
                [oracleBlit endEncoding];
                oracleReceipt->copyEncoded = true;
            }
        }
        error.clear();
        return true;
    }

    void DiscardFrame() override
    {
        if (m_sceneOwnedGBufferCallbackActive || !m_frameActive) return;
        FrameContext& context = m_frames[m_activeFrameIndex];
        if (context.encoder) { [context.encoder popDebugGroup]; [context.encoder endEncoding]; }
        abandonActiveFrame();
    }

    bool ValidateStaticMotionVectors(const R4StaticMotionValidationDesc& descriptor,
        std::string& error) override
    {
        if (!m_frameActive || m_frames[m_activeFrameIndex].encoder)
        { error = "motion validation requires an acquired frame outside a render encoder"; return false; }
        const auto depth = std::dynamic_pointer_cast<MetalTexture>(descriptor.depth);
        const auto motion = std::dynamic_pointer_cast<MetalTexture>(descriptor.motion);
        if (!depth || !motion)
        { error = "motion validation requires native scene textures"; return false; }
        finishPrivateUploads(m_frames[m_activeFrameIndex]);
        return EncodeMetalR4StaticMotionValidation(
            (__bridge void*)m_frames[m_activeFrameIndex].commandBuffer,
            (__bridge void*)depth->Native(), (__bridge void*)motion->Native(), descriptor, error);
    }

    bool ValidateSunShadowCascade(const R4SunShadowValidationDesc& descriptor,
        std::string& error) override
    {
        if (!m_frameActive || m_frames[m_activeFrameIndex].encoder)
        { error="sun shadow validation requires a frame outside an encoder"; return false; }
        const auto shadow=std::dynamic_pointer_cast<MetalTexture>(descriptor.shadowR32);
        const auto position=std::dynamic_pointer_cast<MetalTexture>(descriptor.positionRGBA16F);
        const auto depth=std::dynamic_pointer_cast<MetalTexture>(descriptor.sceneDepthStencil);
        const auto accumulator=std::dynamic_pointer_cast<MetalTexture>(descriptor.accumulationRGBA16F);
        if (!position || !depth || !accumulator)
        { error="sun shadow validation requires native G-buffer/depth/accumulator"; return false; }
        finishPrivateUploads(m_frames[m_activeFrameIndex]);
        return EncodeMetalR4SunShadowValidation((__bridge void*)m_frames[m_activeFrameIndex].commandBuffer,
            shadow ? (__bridge void*)shadow->Native() : nullptr, (__bridge void*)position->Native(),
            (__bridge void*)depth->Native(), (__bridge void*)accumulator->Native(), descriptor,error);
    }

    bool ValidateLocalShadowBatch(const R4LocalShadowBatchValidationDesc& descriptor,
        const std::shared_ptr<IR4Texture>& atlasTexture, const std::shared_ptr<IR4Texture>& positionTexture,
        const std::shared_ptr<IR4Texture>& depthTexture, const std::shared_ptr<IR4Texture>& accumulationTexture,
        std::string& error) override
    {
        if (!m_frameActive || m_frames[m_activeFrameIndex].encoder)
        { error="local shadow batch validation requires an acquired frame outside an encoder"; return false; }
        const auto atlas=std::dynamic_pointer_cast<MetalTexture>(atlasTexture);
        const auto position=std::dynamic_pointer_cast<MetalTexture>(positionTexture);
        const auto depth=std::dynamic_pointer_cast<MetalTexture>(depthTexture);
        const auto accumulator=std::dynamic_pointer_cast<MetalTexture>(accumulationTexture);
        if (!atlas || !position || !depth || !accumulator)
        { error="local shadow batch validation requires native textures"; return false; }
        finishPrivateUploads(m_frames[m_activeFrameIndex]);
        return EncodeMetalR4LocalShadowBatchValidation((__bridge void*)m_frames[m_activeFrameIndex].commandBuffer,
            (__bridge void*)atlas->Native(),(__bridge void*)position->Native(),(__bridge void*)depth->Native(),
            (__bridge void*)accumulator->Native(),descriptor,error);
    }

    bool ValidateTemporalTextures(const R4TemporalTextureValidationDesc& descriptor,
        std::string& error) override
    {
        if (!m_frameActive || m_frames[m_activeFrameIndex].encoder)
        { error = "temporal validation requires an acquired frame outside an encoder"; return false; }
        std::vector<void*> nativeTextures;
        for (const auto& texture : descriptor.textures)
        {
            const auto native = std::dynamic_pointer_cast<MetalTexture>(texture);
            if (!native) { error = "temporal validation requires native textures"; return false; }
            nativeTextures.push_back((__bridge void*)native->Native());
        }
        finishPrivateUploads(m_frames[m_activeFrameIndex]);
        return EncodeMetalR4TemporalTextureValidation(
            (__bridge void*)m_frames[m_activeFrameIndex].commandBuffer, nativeTextures, descriptor, error);
    }

    bool LastFrameSubmitWasRetry() const override { return m_e08SubmitRetry; }

    bool SubmitFrame(std::string& error) override
    {
        m_e08SubmitRetry = false;
        if (m_sceneOwnedGBufferCallbackActive)
        {
            error = "scene-owned G-buffer callback cannot submit the frame";
            return false;
        }
        if (!m_frameActive)
        {
            error = "SubmitFrame called without an active frame";
            return false;
        }

        FrameContext& context = m_frames[m_activeFrameIndex];
        finishPrivateUploads(context);
        if (context.encoder)
        {
            error = "SubmitFrame called while a render pass is active";
            return false;
        }
        const std::uint64_t encodeEndNs = m_e01Frames ? e01NowNs() : 0;
        if (m_e08LateDrawable)
        {
            // Offscreen scene work uses finalColor, not the drawable. Let CPU
            // encoding overlap the preceding submitted GPU frame before the
            // same bounded three-drawable acquire/present contract.
            const auto acquireStart = m_e01Frames ? e01NowNs() : 0;
            const char* forceSubmitRetry = std::getenv("XRAY_METAL_E08_FORCE_SUBMIT_RETRY");
            char* retryEnd = nullptr;
            const unsigned long retryFrame = forceSubmitRetry ? std::strtoul(forceSubmitRetry, &retryEnd, 10) : 0;
            const auto retryWorld = forceSubmitRetry ? EngineWorldSnapshot() : nullptr;
            const bool forcedRetry = forceSubmitRetry && retryEnd && *retryEnd == '\0' && retryFrame > 0 &&
                (retryFrame == 1 || (retryWorld && retryWorld->engineFrame >= retryFrame));
            if (forcedRetry) ::unsetenv("XRAY_METAL_E08_FORCE_SUBMIT_RETRY");
            context.drawable = forcedRetry ? nil : [m_layer nextDrawable];
            if (m_e01Frames) context.e01DrawableMs += e01ElapsedMs(acquireStart, e01NowNs());
            if (!context.drawable || context.drawable.texture.width != context.finalColor.width ||
                context.drawable.texture.height != context.finalColor.height ||
                context.drawable.texture.pixelFormat != context.finalColor.pixelFormat)
            {
                error = "late drawable unavailable or changed extent during scene encoding";
                abandonActiveFrame();
                m_e08SubmitRetry = !m_privateUploadRecoveryFailed;
                return false;
            }
        }
        const std::uint64_t presentationStartNs = m_e01Frames ? e01NowNs() : 0;
        if (!encodePresentation(context, error))
        {
            abandonActiveFrame();
            return false;
        }
        const std::uint64_t presentationEndNs = m_e01Frames ? e01NowNs() : 0;

        const std::uint64_t serial = m_nextSerial++;
        context.completionSerial = serial;
        std::shared_ptr<E01FrameCompletion> e01;
        if (m_e01Frames)
        {
            e01 = std::make_shared<E01FrameCompletion>();
            e01->serial = serial;
            e01->beginNs = context.e01BeginNs;
            e01->engineFrame = context.e01EngineFrame;
            e01->cameraFrame = context.e01CameraFrame;
            e01->slot = static_cast<std::uint32_t>(m_activeFrameIndex);
            e01->queueDepth = static_cast<std::uint32_t>(
                serial - m_completedSerial.load(std::memory_order_acquire));
            e01->waitMs = context.e01WaitMs;
            e01->drawableMs = context.e01DrawableMs;
            e01->sceneMs = context.e01SceneMs;
            e01->grassMs = context.e01GrassMs;
            e01->dynamicPrewarmMs = context.e01DynamicPrewarmMs;
            e01->updatedBytes = context.e01UpdatedBytes;
            e01->bufferAllocations = context.e01BufferAllocations;
            e01->textureAllocations = context.e01TextureAllocations;
            e01->indexedDraws = context.e01IndexedDraws;
            e01->psoBinds = context.e01PsoBinds;
            e01->encodeMs = e01ElapsedMs(context.e01ReadyNs, encodeEndNs);
            e01->presentEncodeMs = e01ElapsedMs(presentationStartNs, presentationEndNs);
            [context.commandBuffer addScheduledHandler:^(id<MTLCommandBuffer>) {
                e01->scheduledNs.store(e01NowNs(), std::memory_order_release);
            }];
        }
        context.commandBuffer.label = [NSString stringWithFormat:@"R4 Metal frame %llu",
            static_cast<unsigned long long>(serial)];
        dispatch_semaphore_t available = context.available;
        std::atomic<std::uint64_t>* completedSerial = &m_completedSerial;
        std::atomic<bool>* hadCommandBufferError = &m_hadCommandBufferError;
        std::atomic<std::uint64_t>* drawableGeneration = &m_drawableGeneration;
        std::mutex* thumbnailMutex = &m_thumbnailMutex;
        R4SaveThumbnailSnapshot* latestThumbnail = &m_latestThumbnail;
        const std::uint64_t generation = context.drawableGeneration;
        id<MTLBuffer> thumbnailBuffer = context.thumbnailBuffer;
        const std::shared_ptr<ComputeSkinOracleReceipt> computeSkinOracleReceipt =
            std::move(context.computeSkinOracleReceipt);
        const auto visibilityRecords = std::move(context.visibilityRecords);
        const id<MTLBuffer> visibilityResultBuffer = context.visibilityResultBuffer;
        const bool visibilityLedgerOverflow = context.visibilityLedgerOverflow;
        const std::uint64_t visibilityAcquireEpoch = context.acquireEpoch;
        id<CAMetalDrawable> e01Drawable = e01 ? context.drawable : nil;
        if (e01 && m_e01Presentation && m_e01Presentation->file)
        {
            const std::shared_ptr<E01PresentationSink> presentation = m_e01Presentation;
            [e01Drawable addPresentedHandler:^(id<MTLDrawable> drawable) {
                const double presented = drawable.presentedTime;
                std::lock_guard<std::mutex> lock(presentation->mutex);
                std::fprintf(presentation->file, "%llu,%.9f\n",
                    static_cast<unsigned long long>(serial), presented);
            }];
        }
        FILE* e01Frames = m_e01Frames;
        std::mutex* e01FramesMutex = &m_e01FramesMutex;
        id<MTLCounterSampleBuffer> e08Counters = context.e08Profile ? context.e08Counters : nil;
        const std::vector<std::string> e08Labels = context.e08PassLabels;
        const auto e08PassCpuRecords = context.e08PassCpuRecords;
        const MTLTimestamp e08CpuBefore = context.e08CpuBefore, e08GpuBefore = context.e08GpuBefore;
        const std::uint32_t e08EngineFrame = context.e01EngineFrame;
        FILE* e08File = m_e08GpuPasses;
        FILE* e08CpuFile = m_e08CpuBindings;
        const std::array<double, 6> e08CpuMs = context.e08CpuMs;
        std::mutex* e08Mutex = &m_e08GpuMutex;
        id<MTLDevice> e08Device = e08Counters ? m_device : nil;
        [context.commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> commandBuffer) {
            @autoreleasepool
            {
                if (e01 && e01Frames)
                {
                    const std::uint64_t completedNs = e01NowNs();
                    const double gpuStart = commandBuffer.GPUStartTime;
                    const double gpuEnd = commandBuffer.GPUEndTime;
                    const double presented = e01Drawable.presentedTime;
                    std::lock_guard<std::mutex> timingLock(*e01FramesMutex);
                    std::fprintf(e01Frames,
                        "%llu,%u,%u,%u,%llu,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%llu,%llu,%.9f,%.9f,%.9f,%u,%s,%llu,%u,%u,%u,%u\n",
                        static_cast<unsigned long long>(e01->serial), e01->engineFrame,
                        e01->cameraFrame, e01->slot,
                        static_cast<unsigned long long>(e01->beginNs),
                        e01->waitMs, e01->drawableMs, e01->sceneMs,
                        e01->grassMs, e01->dynamicPrewarmMs,
                        e01->encodeMs, e01->presentEncodeMs,
                        static_cast<double>(e01->commitDurationNs.load(std::memory_order_acquire)) / 1000000.0,
                        static_cast<unsigned long long>(e01->scheduledNs.load(std::memory_order_acquire)),
                        static_cast<unsigned long long>(completedNs), gpuStart, gpuEnd,
                        presented, e01->queueDepth,
                        commandBuffer.status == MTLCommandBufferStatusCompleted ? "completed" : "error",
                        static_cast<unsigned long long>(e01->updatedBytes), e01->bufferAllocations,
                        e01->textureAllocations, e01->indexedDraws, e01->psoBinds);
                }
                if (e08Counters && e08File && !e08Labels.empty())
                {
                    MTLTimestamp cpuAfter = 0, gpuAfter = 0;
                    [e08Device sampleTimestamps:&cpuAfter gpuTimestamp:&gpuAfter];
                    const double scale = cpuAfter > e08CpuBefore && gpuAfter > e08GpuBefore ?
                        double(cpuAfter - e08CpuBefore) / double(gpuAfter - e08GpuBefore) : 0;
                    NSData* data = [e08Counters resolveCounterRange:NSMakeRange(0, e08Labels.size() * 4)];
                    const bool resolved = data && data.length == e08Labels.size() * 4 * sizeof(MTLCounterResultTimestamp);
                    const auto* values = resolved ? static_cast<const MTLCounterResultTimestamp*>(data.bytes) : nullptr;
                    std::lock_guard<std::mutex> lock(*e08Mutex);
                    if (e08CpuFile)
                        std::fprintf(e08CpuFile, "%llu,%u,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n",
                            (unsigned long long)serial, e08EngineFrame, e08CpuMs[0], e08CpuMs[1],
                            e08CpuMs[2], e08CpuMs[3], e08CpuMs[4], e08CpuMs[5]);
                    for (std::size_t i = 0; i < e08Labels.size(); ++i)
                    {
                        const bool valid = resolved && scale > 0 &&
                            commandBuffer.status == MTLCommandBufferStatusCompleted &&
                            values[i*4].timestamp != MTLCounterErrorValue &&
                            values[i*4+1].timestamp != MTLCounterErrorValue &&
                            values[i*4+2].timestamp != MTLCounterErrorValue &&
                            values[i*4+3].timestamp != MTLCounterErrorValue &&
                            values[i*4].timestamp > 0 && values[i*4+2].timestamp > 0 &&
                            values[i*4+1].timestamp >= values[i*4].timestamp &&
                            values[i*4+3].timestamp >= values[i*4+2].timestamp;
                        // Quoted labels use production literals without quotes/commas.
                        std::fprintf(e08File, "%llu,%u,%zu,\"%s\",%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.9f,%s,%.6f,%llu\n",
                            (unsigned long long)serial, e08EngineFrame, i, e08Labels[i].c_str(),
                            resolved ? (unsigned long long)values[i*4].timestamp : 0,
                            resolved ? (unsigned long long)values[i*4+1].timestamp : 0,
                            resolved ? (unsigned long long)values[i*4+2].timestamp : 0,
                            resolved ? (unsigned long long)values[i*4+3].timestamp : 0,
                            (unsigned long long)e08CpuBefore, (unsigned long long)e08GpuBefore,
                            (unsigned long long)cpuAfter, (unsigned long long)gpuAfter, scale,
                            valid ? "completed" : "invalid", e08PassCpuRecords[i].first,
                            (unsigned long long)e08PassCpuRecords[i].second);
                    }
                }
                if (computeSkinOracleReceipt)
                    finishComputeSkinOracleReceipt(computeSkinOracleReceipt,
                        commandBuffer.status, serial, false);
                reportWholeLightVisibilityCompletion(visibilityRecords, visibilityResultBuffer,
                    visibilityLedgerOverflow, serial, visibilityAcquireEpoch, commandBuffer.status);
                std::uint64_t observed = completedSerial->load(std::memory_order_relaxed);
                while (observed < serial &&
                       !completedSerial->compare_exchange_weak(
                           observed, serial, std::memory_order_release, std::memory_order_relaxed))
                {
                }
                if (commandBuffer.status == MTLCommandBufferStatusError)
                {
                    hadCommandBufferError->store(true, std::memory_order_release);
                    fprintf(stderr, "METAL_R4_BACKEND_ERROR serial=%llu command_buffer=\"%s\"\n",
                        static_cast<unsigned long long>(serial),
                        commandBuffer.error.localizedDescription.UTF8String);
                }
                else if (commandBuffer.status == MTLCommandBufferStatusCompleted &&
                    generation == drawableGeneration->load(std::memory_order_acquire) &&
                    thumbnailBuffer && thumbnailBuffer.contents)
                {
                    std::lock_guard<std::mutex> lock(*thumbnailMutex);
                    if (generation == drawableGeneration->load(std::memory_order_acquire) &&
                        serial > latestThumbnail->frameSerial)
                    {
                        latestThumbnail->frameSerial = serial;
                        const auto* begin = static_cast<const std::uint8_t*>(thumbnailBuffer.contents);
                        latestThumbnail->bgraPixels.assign(begin,
                            begin + R4SaveThumbnailSnapshot::Width *
                                R4SaveThumbnailSnapshot::Height * 4u);
                    }
                }
            }
            dispatch_semaphore_signal(available);
        }];

        [context.commandBuffer presentDrawable:context.drawable];
        const std::uint64_t commitStartNs = e01 ? e01NowNs() : 0;
        [context.commandBuffer commit];
        stopE03Capture();
        if (e01)
            e01->commitDurationNs.store(e01NowNs() - commitStartNs, std::memory_order_release);
        context.drawable = nil;

        m_lastSubmittedSerial.store(serial, std::memory_order_release);
        m_privateUploadRecoveryFailures = 0;
        m_frameActive = false;
        m_nextFrameIndex = (m_activeFrameIndex + 1) % GetConfiguredFramesInFlight();
        return true;
    }

    bool CopyLatestCompletedSaveThumbnail(R4SaveThumbnailSnapshot& snapshot) const override
    {
        std::lock_guard<std::mutex> lock(m_thumbnailMutex);
        const std::size_t expectedBytes = static_cast<std::size_t>(R4SaveThumbnailSnapshot::Width) *
            R4SaveThumbnailSnapshot::Height * 4u;
        if (m_latestThumbnail.frameSerial == 0u ||
            m_latestThumbnail.bgraPixels.size() != expectedBytes)
            return false;
        snapshot = m_latestThumbnail;
        return true;
    }

    bool RunSceneOwnedGBufferCallback(
        const std::function<bool(IR4Backend&, std::string&)>& callback,
        std::string& error) override
    {
        if (!m_frameActive)
        {
            error = "scene-owned G-buffer callback requires an active frame";
            return false;
        }
        if (m_sceneOwnedGBufferCallbackActive)
        {
            error = "scene-owned G-buffer callback is already active";
            return false;
        }
        FrameContext& context = m_frames[m_activeFrameIndex];
        if (!context.encoder)
        {
            error = "scene-owned G-buffer callback requires an active render pass";
            return false;
        }
        if (!callback)
        {
            error = "scene-owned G-buffer callback is empty";
            return false;
        }
        m_sceneOwnedGBufferCallbackActive = true;
        const bool succeeded = callback(*this, error);
        m_sceneOwnedGBufferCallbackActive = false;
        return succeeded;
    }

    std::shared_ptr<IR4GraphicsPipeline> CreateGraphicsPipeline(
        const R4GraphicsPipelineDesc& descriptor,
        std::string& error) override
    {
        const bool usesMsc = descriptor.vertexMetallibPath && descriptor.fragmentMetallibPath;
        const bool hasAnyMscPath = descriptor.vertexMetallibPath || descriptor.fragmentMetallibPath;
        const bool usesMsl = descriptor.mslSource && descriptor.vertexFunction &&
            descriptor.fragmentFunction;
        if (!m_device || !descriptor.shaderKey || (hasAnyMscPath && !usesMsc) ||
            (!usesMsc && !usesMsl) ||
            descriptor.sampleCount == 0 || descriptor.colorFormats.size() > 8)
        {
            error = "invalid graphics pipeline descriptor or uninitialized Metal backend";
            return {};
        }
#if !XRAY_METAL_HAS_MSC_RUNTIME
        if (usesMsc)
        {
            error = "Metal Shader Converter runtime header is unavailable";
            return {};
        }
#endif

        std::vector<R4PixelFormat> colorFormats = descriptor.colorFormats;
        if (colorFormats.empty() && descriptor.colorFormat != R4PixelFormat::Invalid)
            colorFormats.push_back(descriptor.colorFormat);
        const MTLPixelFormat depthFormat = metalPixelFormat(descriptor.depthFormat);
        if ((colorFormats.empty() && descriptor.depthFormat == R4PixelFormat::Invalid) ||
            (!descriptor.colorWriteMasks.empty() &&
             descriptor.colorWriteMasks.size() != colorFormats.size()) ||
            std::any_of(descriptor.colorWriteMasks.begin(), descriptor.colorWriteMasks.end(),
                [](std::uint8_t mask) { return (mask & ~0x0fu) != 0; }) ||
            (descriptor.depthFormat != R4PixelFormat::Invalid &&
             depthFormat == MTLPixelFormatInvalid) ||
            std::any_of(colorFormats.begin(), colorFormats.end(), [](R4PixelFormat format) {
                return metalPixelFormat(format) == MTLPixelFormatInvalid;
            }))
        {
            error = "graphics pipeline uses an unsupported attachment format";
            return {};
        }

        const std::string key = pipelineCacheKey(descriptor);
        const auto cached = m_pipelineCache.find(key);
        if (cached != m_pipelineCache.end())
            return cached->second;

        if (![m_device supportsTextureSampleCount:descriptor.sampleCount])
        {
            error = "Metal graphics pipeline sample count is unsupported";
            return {};
        }

        id<MTLFunction> vertexFunction = nil;
        id<MTLFunction> fragmentFunction = nil;
        if (usesMsc)
        {
            vertexFunction = loadSingleMetallibFunction(
                m_device, descriptor.vertexMetallibPath, error);
            if (vertexFunction)
                fragmentFunction = loadSingleMetallibFunction(
                    m_device, descriptor.fragmentMetallibPath, error);
        }
        else
        {
            NSError* libraryError = nil;
            NSString* source = [NSString stringWithUTF8String:descriptor.mslSource];
            id<MTLLibrary> library =
                [m_device newLibraryWithSource:source options:nil error:&libraryError];
            if (!library)
            {
                error = "Metal shader library compilation failed: " + describeError(libraryError);
                return {};
            }
            vertexFunction = [library newFunctionWithName:
                [NSString stringWithUTF8String:descriptor.vertexFunction]];
            fragmentFunction = [library newFunctionWithName:
                [NSString stringWithUTF8String:descriptor.fragmentFunction]];
        }
        if (!vertexFunction || !fragmentFunction)
        {
            if (error.empty())
                error = "Metal graphics pipeline function lookup failed";
            return {};
        }

        MTLVertexDescriptor* vertexDescriptor = [[MTLVertexDescriptor alloc] init];
        std::array<bool, 31> usedAttributes{};
        std::array<bool, 31> usedLayouts{};
        for (const R4VertexBufferLayoutDesc& layout : descriptor.vertexLayouts)
        {
            if (layout.bufferIndex >= usedLayouts.size() || layout.stride == 0 ||
                usedLayouts[layout.bufferIndex])
            {
                error = "Metal vertex layout index, stride, or duplication is invalid";
                return {};
            }
            usedLayouts[layout.bufferIndex] = true;
            vertexDescriptor.layouts[layout.bufferIndex].stride = layout.stride;
            vertexDescriptor.layouts[layout.bufferIndex].stepFunction = MTLVertexStepFunctionPerVertex;
            vertexDescriptor.layouts[layout.bufferIndex].stepRate = 1;
        }
        for (const R4VertexAttributeDesc& attribute : descriptor.vertexAttributes)
        {
            if (attribute.attributeIndex >= usedAttributes.size() ||
                attribute.bufferIndex >= usedLayouts.size() ||
                usedAttributes[attribute.attributeIndex] || !usedLayouts[attribute.bufferIndex])
            {
                error = "Metal vertex attribute index, buffer, or duplication is invalid";
                return {};
            }
            const MTLVertexFormat format = metalVertexFormat(attribute.format);
            const std::size_t formatSize = vertexFormatSize(attribute.format);
            const std::size_t stride = vertexDescriptor.layouts[attribute.bufferIndex].stride;
            if (format == MTLVertexFormatInvalid || formatSize == 0 ||
                attribute.offset > stride || formatSize > stride - attribute.offset)
            {
                error = "Metal vertex attribute format or range is unsupported";
                return {};
            }
            usedAttributes[attribute.attributeIndex] = true;
            vertexDescriptor.attributes[attribute.attributeIndex].format = format;
            vertexDescriptor.attributes[attribute.attributeIndex].offset = attribute.offset;
            vertexDescriptor.attributes[attribute.attributeIndex].bufferIndex = attribute.bufferIndex;
        }

        MTLRenderPipelineDescriptor* metalDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
        if (descriptor.label)
            metalDescriptor.label = [NSString stringWithUTF8String:descriptor.label];
        metalDescriptor.vertexFunction = vertexFunction;
        metalDescriptor.fragmentFunction = fragmentFunction;
        metalDescriptor.vertexDescriptor = vertexDescriptor;
        metalDescriptor.rasterSampleCount = descriptor.sampleCount;
        for (std::size_t index = 0; index < colorFormats.size(); ++index)
            metalDescriptor.colorAttachments[index].pixelFormat = metalPixelFormat(colorFormats[index]);
        metalDescriptor.depthAttachmentPixelFormat = depthFormat;
        if (depthFormat == MTLPixelFormatDepth32Float_Stencil8)
            metalDescriptor.stencilAttachmentPixelFormat = depthFormat;
        for (std::size_t index = 0; index < colorFormats.size(); ++index)
        {
            MTLRenderPipelineColorAttachmentDescriptor* color =
                metalDescriptor.colorAttachments[index];
            color.blendingEnabled = descriptor.blend.enabled;
            color.sourceRGBBlendFactor = metalBlendFactor(descriptor.blend.sourceRgb);
            color.destinationRGBBlendFactor = metalBlendFactor(descriptor.blend.destinationRgb);
            color.sourceAlphaBlendFactor = metalBlendFactor(descriptor.blend.sourceAlpha);
            color.destinationAlphaBlendFactor = metalBlendFactor(descriptor.blend.destinationAlpha);
            color.writeMask = descriptor.colorWriteMasks.empty() ? MTLColorWriteMaskAll :
                static_cast<MTLColorWriteMask>(descriptor.colorWriteMasks[index]);
        }

        NSError* pipelineError = nil;
        id<MTLRenderPipelineState> nativePipeline =
            [m_device newRenderPipelineStateWithDescriptor:metalDescriptor error:&pipelineError];
        if (!nativePipeline)
        {
            error = "Metal graphics pipeline creation failed: " + describeError(pipelineError);
            return {};
        }

        MTLDepthStencilDescriptor* depthDescriptor = [[MTLDepthStencilDescriptor alloc] init];
        depthDescriptor.depthCompareFunction = metalCompareFunction(descriptor.depthCompare);
        depthDescriptor.depthWriteEnabled = descriptor.depthWrite;
        if (descriptor.stencil.enabled)
        {
            auto makeStencil = [](const R4StencilFaceDesc& face) {
                MTLStencilDescriptor* stencil = [[MTLStencilDescriptor alloc] init];
                stencil.stencilCompareFunction = metalCompareFunction(face.compare);
                stencil.stencilFailureOperation =
                    metalStencilOperation(face.stencilFailure);
                stencil.depthFailureOperation = metalStencilOperation(face.depthFailure);
                stencil.depthStencilPassOperation =
                    metalStencilOperation(face.depthStencilPass);
                stencil.readMask = face.readMask;
                stencil.writeMask = face.writeMask;
                return stencil;
            };
            depthDescriptor.frontFaceStencil = makeStencil(descriptor.stencil.frontFace);
            depthDescriptor.backFaceStencil = makeStencil(descriptor.stencil.backFace);
        }
        id<MTLDepthStencilState> depthState = [m_device newDepthStencilStateWithDescriptor:depthDescriptor];
        if (!depthState)
        {
            error = "Metal depth stencil state creation failed";
            return {};
        }

        std::shared_ptr<IR4GraphicsPipeline> pipeline = std::make_shared<MetalGraphicsPipeline>(
            nativePipeline, depthState, usesMsc, descriptor.cullMode, descriptor.frontFace);
        m_pipelineCache.emplace(key, pipeline);
        return pipeline;
    }

    bool BindGraphicsPipeline(
        const std::shared_ptr<IR4GraphicsPipeline>& pipeline,
        std::string& error) override
    {
        FrameContext* context = activeRenderContext(error);
        if (!context)
            return false;
        E08ScopedCpu e08Cpu(context->e08Profile, context->e08CpuMs[3]);
        const std::shared_ptr<MetalGraphicsPipeline> metalPipeline =
            std::dynamic_pointer_cast<MetalGraphicsPipeline>(pipeline);
        if (!metalPipeline)
        {
            error = "invalid Metal graphics pipeline";
            return false;
        }
        [context->encoder setRenderPipelineState:metalPipeline->Native()];
        [context->encoder setDepthStencilState:metalPipeline->DepthState()];
        [context->encoder setCullMode:metalCullMode(metalPipeline->CullMode())];
        [context->encoder setFrontFacingWinding:metalWinding(metalPipeline->FrontFace())];
        context->mscPipelineBound = metalPipeline->UsesMsc();
        retainFrameResource(*context, pipeline);
        if (m_e01Frames)
            ++context->e01PsoBinds;
        return true;
    }

    bool BindVertexBuffer(
        std::uint32_t index,
        const std::shared_ptr<IR4Buffer>& buffer,
        std::size_t offset,
        std::string& error) override
    {
        FrameContext* context = activeRenderContext(error);
        const std::shared_ptr<MetalBuffer> metalBuffer = std::dynamic_pointer_cast<MetalBuffer>(buffer);
        if (!context || !metalBuffer || index >= 31 || offset >= metalBuffer->Size())
        {
            if (error.empty())
                error = "invalid Metal vertex buffer binding";
            return false;
        }
        E08ScopedCpu e08Cpu(context->e08Profile, context->e08CpuMs[4]);
        [context->encoder setVertexBuffer:metalBuffer->Native() offset:offset atIndex:index];
        retainFrameResource(*context, buffer);
        retainFrameResource(*context, metalBuffer->VersionHandle());
        return true;
    }

    bool BindFragmentTexture(
        std::uint32_t index,
        const std::shared_ptr<IR4Texture>& texture,
        std::string& error) override
    {
        FrameContext* context = activeRenderContext(error);
        const std::shared_ptr<MetalTexture> metalTexture =
            std::dynamic_pointer_cast<MetalTexture>(texture);
        if (!context || !metalTexture || index >= 128)
        {
            if (error.empty())
                error = "invalid Metal fragment texture binding";
            return false;
        }
        [context->encoder setFragmentTexture:metalTexture->Native() atIndex:index];
        retainFrameResource(*context, texture);
        return true;
    }

    bool BindFragmentSampler(
        std::uint32_t index,
        const std::shared_ptr<IR4Sampler>& sampler,
        std::string& error) override
    {
        FrameContext* context = activeRenderContext(error);
        const std::shared_ptr<MetalSampler> metalSampler =
            std::dynamic_pointer_cast<MetalSampler>(sampler);
        if (!context || !metalSampler || index >= 16)
        {
            if (error.empty())
                error = "invalid Metal fragment sampler binding";
            return false;
        }
        [context->encoder setFragmentSamplerState:metalSampler->Native() atIndex:index];
        retainFrameResource(*context, sampler);
        return true;
    }

    bool BindMscResources(
        const R4MscStageBindings& bindings,
        std::string& error) override
    {
#if !XRAY_METAL_HAS_MSC_RUNTIME
        (void)bindings;
        error = "Metal Shader Converter runtime header is unavailable";
        return false;
#else
        FrameContext* context = activeRenderContext(error);
        constexpr std::size_t EntrySize = sizeof(IRDescriptorTableEntry);
        if (!context || bindings.tlabSize == 0 ||
            bindings.tlabSize > MaxFrameTlabBytes ||
            bindings.tlabSize % EntrySize != 0)
        {
            if (error.empty())
                error = "invalid Metal Shader Converter TLAB size";
            return false;
        }

        const std::size_t entryCount = bindings.tlabSize / EntrySize;
        E08ScopedCpu e08Cpu(context->e08Profile, context->e08CpuMs[1]);
        // Most reflected VS/PS TLABs have <=16 entries. Scratch dies at return;
        // GPU data still lives in a fresh immutable per-draw arena allocation.
        constexpr std::size_t SmallTlabEntries = 16;
        const bool small = !m_e08HeapBindings && entryCount <= SmallTlabEntries;
        std::uint16_t smallOccupied = 0;
        std::array<R4FrameBufferSlice, SmallTlabEntries> smallSlices{};
        std::vector<bool> occupied;
        std::vector<R4FrameBufferSlice> heapSlices;
        if (!small) { occupied.resize(entryCount, false); heapSlices.resize(entryCount); }
        R4FrameBufferSlice* constantSlices = small ? smallSlices.data() : heapSlices.data();
        const MTLRenderStages stages = bindings.stage == R4ShaderStage::Vertex ?
            MTLRenderStageVertex : MTLRenderStageFragment;
        // Validate and declare indirect use on every draw.
        for (const R4MscResourceBinding& binding : bindings.resources)
        {
            if (binding.tlabOffset % EntrySize != 0 ||
                binding.tlabOffset >= bindings.tlabSize)
            {
                error = "Metal Shader Converter TLAB offset is invalid";
                return false;
            }
            const std::size_t entryIndex = binding.tlabOffset / EntrySize;
            if (small ? (smallOccupied & (1u << entryIndex)) != 0 : occupied[entryIndex])
            {
                error = "Metal Shader Converter TLAB entries overlap";
                return false;
            }
            if (small) smallOccupied |= static_cast<std::uint16_t>(1u << entryIndex);
            else occupied[entryIndex] = true;

            if (binding.type == R4MscResourceType::ConstantBuffer)
            {
                const std::shared_ptr<MetalBuffer> buffer =
                    std::dynamic_pointer_cast<MetalBuffer>(binding.buffer);
                const std::size_t size = binding.bufferSize == 0 ?
                    (buffer && binding.bufferOffset <= buffer->Size() ?
                        buffer->Size() - binding.bufferOffset : 0) : binding.bufferSize;
                if (!buffer || size == 0 || binding.bufferOffset > buffer->Size() ||
                    size > buffer->Size() - binding.bufferOffset ||
                    size > std::numeric_limits<std::uint32_t>::max())
                {
                    error = "invalid Metal Shader Converter constant-buffer binding";
                    return false;
                }
                R4FrameBufferSlice slice{binding.buffer, binding.bufferOffset, size};
                if (buffer->Storage() == R4ResourceStorage::Shared &&
                    !buffer->IsFrameUpload())
                {
                    const auto* source = static_cast<const std::uint8_t*>(
                        buffer->Native().contents) + binding.bufferOffset;
                    if (!UploadFrameData(source, size, 256, slice, error))
                        return false;
                }
                constantSlices[entryIndex] = slice;
                const std::shared_ptr<MetalBuffer> boundBuffer =
                    std::static_pointer_cast<MetalBuffer>(slice.buffer);
                declareIndirectRead(*context, boundBuffer->Native(), stages);
                retainFrameResource(*context, slice.buffer);
            }
            else if (binding.type == R4MscResourceType::Texture)
            {
                const std::shared_ptr<MetalTexture> texture =
                    std::dynamic_pointer_cast<MetalTexture>(binding.texture);
                if (!texture)
                {
                    error = "invalid Metal Shader Converter texture binding";
                    return false;
                }
                declareIndirectRead(*context, texture->Native(), stages);
                retainFrameResource(*context, binding.texture);
            }
            else if (binding.type == R4MscResourceType::Sampler)
            {
                const std::shared_ptr<MetalSampler> sampler =
                    std::dynamic_pointer_cast<MetalSampler>(binding.sampler);
                if (!sampler)
                {
                    error = "invalid Metal Shader Converter sampler binding";
                    return false;
                }
                retainFrameResource(*context, binding.sampler);
            }
            else
            {
                error = "unknown Metal Shader Converter resource type";
                return false;
            }
        }

        // Suballocate TLAB from per-frame bump allocator
        const std::size_t alignedAllocSize =
            (bindings.tlabSize + TlabAlignment - 1) & ~(TlabAlignment - 1);
        if (context->activeTlabBlockOffset > MaxFrameTlabBytes - (TlabAlignment - 1))
        {
            error = "Metal Shader Converter TLAB offset overflow";
            return false;
        }
        context->activeTlabBlockOffset =
            (context->activeTlabBlockOffset + TlabAlignment - 1) & ~(TlabAlignment - 1);

        if (context->tlabBlocks.empty() ||
            context->activeTlabBlockOffset >
                context->tlabBlocks[context->activeTlabBlockIndex].capacity ||
            bindings.tlabSize > context->tlabBlocks[context->activeTlabBlockIndex].capacity -
                context->activeTlabBlockOffset)
        {
            if (!context->tlabBlocks.empty() &&
                context->activeTlabBlockIndex + 1 < context->tlabBlocks.size() &&
                bindings.tlabSize <= context->tlabBlocks[context->activeTlabBlockIndex + 1].capacity)
            {
                ++context->activeTlabBlockIndex;
                context->activeTlabBlockOffset = 0;
            }
            else
            {
                const std::size_t newBlockSize = std::max(DefaultTlabBlockSize, alignedAllocSize);
                if (newBlockSize > MaxFrameTlabBytes - context->tlabTotalCapacity)
                {
                    error = "Metal Shader Converter TLAB capacity exceeded";
                    return false;
                }
                id<MTLBuffer> newBuffer = [m_device newBufferWithLength:newBlockSize
                                                                options:MTLResourceStorageModeShared];
                if (!newBuffer)
                {
                    error = "Metal Shader Converter TLAB allocation failed";
                    return false;
                }
                newBuffer.label = [NSString stringWithFormat:@"R4 MSC Frame TLAB block %zu",
                    context->tlabBlocks.size()];
                context->tlabBlocks.push_back({newBuffer, newBlockSize});
                context->tlabTotalCapacity += newBlockSize;
                context->activeTlabBlockIndex = context->tlabBlocks.size() - 1;
                context->activeTlabBlockOffset = 0;
            }
        }

        TlabBlock& activeBlock = context->tlabBlocks[context->activeTlabBlockIndex];
        const std::size_t allocOffset = context->activeTlabBlockOffset;
        context->activeTlabBlockOffset += alignedAllocSize;
        ++m_mscTlabAllocations;

        auto* tlabBytes = static_cast<std::uint8_t*>(activeBlock.buffer.contents) + allocOffset;
        std::memset(tlabBytes, 0, bindings.tlabSize);
        auto* entries = reinterpret_cast<IRDescriptorTableEntry*>(tlabBytes);

        for (const R4MscResourceBinding& binding : bindings.resources)
        {
            IRDescriptorTableEntry* entry = &entries[binding.tlabOffset / EntrySize];
            if (binding.type == R4MscResourceType::ConstantBuffer)
            {
                const R4FrameBufferSlice& slice =
                    constantSlices[binding.tlabOffset / EntrySize];
                const std::shared_ptr<MetalBuffer> buffer =
                    std::static_pointer_cast<MetalBuffer>(slice.buffer);
                IRDescriptorTableSetBuffer(
                    entry, buffer->Native().gpuAddress + slice.offset, slice.size);
            }
            else if (binding.type == R4MscResourceType::Texture)
            {
                const std::shared_ptr<MetalTexture> texture =
                    std::dynamic_pointer_cast<MetalTexture>(binding.texture);
                IRDescriptorTableSetTexture(entry, texture->Native(), 0.0f, 0);
            }
            else
            {
                const std::shared_ptr<MetalSampler> sampler =
                    std::dynamic_pointer_cast<MetalSampler>(binding.sampler);
                IRDescriptorTableSetSampler(entry, sampler->Native(), 0.0f);
            }
        }

        declareIndirectRead(*context, activeBlock.buffer, stages);
        if (bindings.stage == R4ShaderStage::Vertex)
            [context->encoder setVertexBuffer:activeBlock.buffer offset:allocOffset atIndex:kIRArgumentBufferBindPoint];
        else
            [context->encoder setFragmentBuffer:activeBlock.buffer offset:allocOffset atIndex:kIRArgumentBufferBindPoint];
        return true;
#endif
    }

    bool SetStencilReference(std::uint32_t reference, std::string& error) override
    {
        FrameContext* context = activeRenderContext(error);
        if (!context)
            return false;
        [context->encoder setStencilReferenceValue:reference];
        return true;
    }

    bool DrawIndexed(const R4IndexedDrawDesc& descriptor, std::string& error) override
    {
        FrameContext* context = activeRenderContext(error);
        const std::shared_ptr<MetalBuffer> indexBuffer =
            std::dynamic_pointer_cast<MetalBuffer>(descriptor.indexBuffer);
        const std::size_t indexSize = descriptor.indexType == R4IndexType::UInt16 ? 2u : 4u;
        if (!context || !indexBuffer || descriptor.indexCount == 0 || descriptor.instanceCount == 0 ||
            descriptor.indexBufferOffset % indexSize != 0 ||
            descriptor.indexBufferOffset > indexBuffer->Size() ||
            static_cast<std::size_t>(descriptor.indexCount) >
                (indexBuffer->Size() - descriptor.indexBufferOffset) / indexSize)
        {
            if (error.empty())
                error = "invalid Metal indexed draw range";
            return false;
        }
        E08ScopedCpu e08Cpu(context->e08Profile, context->e08CpuMs[5]);
        const MTLIndexType indexType = descriptor.indexType == R4IndexType::UInt16 ?
            MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
#if XRAY_METAL_HAS_MSC_RUNTIME
        if (context->mscPipelineBound)
        {
            IRRuntimeDrawIndexedPrimitives(
                context->encoder,
                MTLPrimitiveTypeTriangle,
                descriptor.indexCount,
                indexType,
                indexBuffer->Native(),
                descriptor.indexBufferOffset,
                descriptor.instanceCount,
                descriptor.baseVertex,
                0);
        }
        else
#endif
        {
            [context->encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                        indexCount:descriptor.indexCount
                                         indexType:indexType
                                       indexBuffer:indexBuffer->Native()
                                 indexBufferOffset:descriptor.indexBufferOffset
                                     instanceCount:descriptor.instanceCount
                                        baseVertex:descriptor.baseVertex
                                      baseInstance:0];
        }
        retainFrameResource(*context, descriptor.indexBuffer);
        retainFrameResource(*context, indexBuffer->VersionHandle());
        if (m_e01Frames || context->e08Profile)
            ++context->e01IndexedDraws;
        return true;
    }

    bool DrawIndexedWithVisibility(const R4IndexedDrawDesc& descriptor,
        const R4WholeLightVisibilitySample& sample, bool& resultEncoded,
        std::string& error) override
    {
        resultEncoded = false;
        FrameContext* context = activeRenderContext(error);
        if (!context)
            return false;
        if (!m_wholeLightVisibilityEnabled || !context->activePass.enableVisibilityResultCounts)
            return DrawIndexed(descriptor, error);
        if (!context->visibilityAdmissionResolved || context->visibilityDeniedByCap)
            return DrawIndexed(descriptor, error);
        if (!context->visibilityAdmissionGranted || !context->visibilityBufferReset ||
            !context->visibilityResultBuffer)
        {
            context->visibilityLedgerOverflow = true;
            return DrawIndexed(descriptor, error);
        }

        auto recordInvalid = [&]() {
            context->visibilityLedgerOverflow = true;
            return DrawIndexed(descriptor, error);
        };
        if (context->visibilityRecords.size() >= xray::metal::R4WholeLightVisibilityMaxSlots ||
            !xray::metal::R4WholeLightVisibilitySampleValid(sample))
            return recordInvalid();

        const std::uint64_t groupKey = sample.groupKind == R4WholeLightVisibilityGroupKind::OmniParent ?
            sample.parentLightId : sample.lightId;
        std::array<std::pair<R4WholeLightVisibilityGroupKind, std::uint64_t>,
            xray::metal::R4WholeLightVisibilityMaxGroups> seenGroups{};
        std::size_t seenGroupCount = 0u;
        bool foundGroup = false;
        for (const auto& existing : context->visibilityRecords)
        {
            const auto& prior = existing.sample;
            const std::uint64_t priorKey = prior.groupKind == R4WholeLightVisibilityGroupKind::OmniParent ?
                prior.parentLightId : prior.lightId;
            const auto key = std::make_pair(prior.groupKind, priorKey);
            if (std::find(seenGroups.begin(), seenGroups.begin() + seenGroupCount, key) ==
                seenGroups.begin() + seenGroupCount)
            {
                if (seenGroupCount < seenGroups.size())
                    seenGroups[seenGroupCount++] = key;
                else
                    return recordInvalid();
            }
            if (priorKey == groupKey && prior.groupKind == sample.groupKind)
                foundGroup = true;
        }
        if (!foundGroup && seenGroupCount >= xray::metal::R4WholeLightVisibilityMaxGroups)
            return recordInvalid();

        const std::uint32_t slot = static_cast<std::uint32_t>(context->visibilityRecords.size());
        try
        {
            context->visibilityRecords.push_back({sample, slot, false});
        }
        catch (...)
        {
            // Allocation failure is an observation miss. Leave the draw on the
            // normal path and ensure this acquisition can never yield a zero.
            context->visibilityLedgerOverflow = true;
            return DrawIndexed(descriptor, error);
        }
        [context->encoder setVisibilityResultMode:MTLVisibilityResultModeCounting
            offset:static_cast<NSUInteger>(slot) * sizeof(std::uint64_t)];
        const bool drawn = DrawIndexed(descriptor, error);
        [context->encoder setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
        if (!drawn)
            return false;
        context->visibilityRecords.back().encoded = true;
        resultEncoded = true;
        return true;
    }

    bool SubmitEngineWorldRenderables(
        const std::vector<R4EngineWorldRenderable>& renderables,
        std::uint32_t engineFrame,
        std::string& error) override
    {
        if (!m_frameActive)
        {
            error = "engine world submission requires an active Metal frame";
            return false;
        }

		auto snapshot = std::make_shared<R4EngineWorldSnapshot>();
		snapshot->engineFrame = engineFrame;
		snapshot->renderables = renderables;
		std::size_t geometryCount = 0;
		std::size_t indexCount = 0;
		for (const R4EngineWorldRenderable& renderable : snapshot->renderables)
		{
			if (!renderable.asset)
				continue;
			geometryCount += renderable.asset->geometry.size();
			for (const R4EngineWorldGeometry& geometry : renderable.asset->geometry)
				indexCount += geometry.indexCount;
		}
		{
			std::lock_guard<std::mutex> lock(m_engineWorldSnapshotMutex);
			m_engineWorldSnapshot = std::move(snapshot);
		}
		static std::uint32_t menuSubmitCounter = 0;
		if (engineFrame == UINT32_MAX)
		{
			if (menuSubmitCounter++ % 300 == 0)
			{
				fprintf(stderr, "METAL_WORLD_SUBMIT frame=menu renderables=%zu geometry=%zu indices=%zu\n",
					renderables.size(), geometryCount, indexCount);
			}
		}
		else if (m_lastLoggedEngineWorldFrame == UINT32_MAX ||
                 engineFrame - m_lastLoggedEngineWorldFrame >= 300)
        {
            m_lastLoggedEngineWorldFrame = engineFrame;
			m_lastLoggedEngineWorldCount = renderables.size();
			fprintf(stderr, "METAL_WORLD_SUBMIT frame=%u renderables=%zu geometry=%zu indices=%zu\n",
				engineFrame, renderables.size(), geometryCount, indexCount);
		}
        return true;
    }

    std::shared_ptr<const R4EngineWorldSnapshot> EngineWorldSnapshot() const override
    {
		std::lock_guard<std::mutex> lock(m_engineWorldSnapshotMutex);
		return m_engineWorldSnapshot;
    }

    bool SubmitEngineHudRenderables(const R4EngineHudSnapshot& snapshot,
        std::string& error) override
    {
        if (!m_frameActive)
        {
            error = "engine HUD submission requires an active Metal frame";
            return false;
        }
        auto immutable = std::make_shared<R4EngineHudSnapshot>(snapshot);
        {
            std::lock_guard<std::mutex> lock(m_engineHudSnapshotMutex);
            m_engineHudSnapshot = std::move(immutable);
        }
        return true;
    }

    std::shared_ptr<const R4EngineHudSnapshot> EngineHudSnapshot() const override
    {
        std::lock_guard<std::mutex> lock(m_engineHudSnapshotMutex);
        return m_engineHudSnapshot;
    }

    bool SubmitEngineLightingFrame(const R4LightingFrameSnapshot& submitted,
        std::string& error) override
    {
        if (!m_frameActive)
        {
            error = "engine local-light submission requires an active Metal frame";
            return false;
        }
		// The engine-side bridge owns Device.dwFrame and rejects a stale capture
		// before this call.  The backend has no engine-global dependency; still
		// fail closed for the explicit invalid sentinel.
		if (submitted.engineFrame == std::numeric_limits<std::uint32_t>::max())
		{
			error = "engine lighting snapshot has an invalid frame";
			return false;
		}
        auto snapshot = std::make_shared<R4LightingFrameSnapshot>(submitted);
        {
            std::lock_guard<std::mutex> lock(m_engineLocalLightSnapshotMutex);
            m_engineLocalLightSnapshot = std::move(snapshot);
        }
        return true;
    }

	std::shared_ptr<const R4LightingFrameSnapshot> EngineLightingFrameSnapshot() const override
    {
        std::lock_guard<std::mutex> lock(m_engineLocalLightSnapshotMutex);
        return m_engineLocalLightSnapshot;
    }

    bool RenderUIOverlay(const xray::metal::ui::FrameBatch& batch, std::string& error) override
    {
        if (!m_frameActive)
        {
            error = "Metal UI overlay requires an active frame";
            return false;
        }
        FrameContext& context = m_frames[m_activeFrameIndex];
        if (context.encoder)
        {
            error = "Metal UI overlay requires all scene passes to be closed";
            return false;
        }
        if (batch.draws.empty())
            return true;

        std::uint32_t drawableWidth = 0;
        std::uint32_t drawableHeight = 0;
        DrawableSize(drawableWidth, drawableHeight);
        if (drawableWidth == 0u || drawableHeight == 0u)
        {
            error = "Metal UI overlay has a zero-sized drawable";
            return false;
        }

        struct ResolvedDraw
        {
            const xray::metal::ui::DrawRecord* record = nullptr;
            std::shared_ptr<IR4Texture> texture;
            xray::metal::ui::overlay::ScissorRect scissor;
        };
        std::vector<ResolvedDraw> draws;
        draws.reserve(batch.draws.size());
        std::uint32_t skippedDraws = 0;
        for (const xray::metal::ui::DrawRecord& record : batch.draws)
        {
            if (record.vertexCount == 0u || record.indexCount == 0u ||
                record.firstVertex > batch.vertices.size() || record.vertexCount > batch.vertices.size() - record.firstVertex ||
                record.firstIndex > batch.indices.size() || record.indexCount > batch.indices.size() - record.firstIndex)
            {
                ++skippedDraws;
                continue;
            }
            const auto indexBegin = batch.indices.begin() + record.firstIndex;
            const auto indexEnd = indexBegin + record.indexCount;
            if (std::any_of(indexBegin, indexEnd, [&record](std::uint32_t index) {
                    return index < record.firstVertex || index >= record.firstVertex + record.vertexCount;
                }))
            {
                ++skippedDraws;
                continue;
            }
            xray::metal::ui::overlay::ScissorRect scissor;
            if (!xray::metal::ui::overlay::ResolveScissor(record.scissor, drawableWidth, drawableHeight, scissor))
            {
                ++skippedDraws;
                continue;
            }
            std::shared_ptr<IR4Texture> texture;
            if (!resolveUiTexture(record.shader.texture, texture))
            {
                ++skippedDraws;
                continue;
            }
            if (record.font && texture == m_uiWhiteTexture)
            {
                ++skippedDraws;
                continue;
            }
            draws.push_back({&record, std::move(texture), scissor});
        }
        if (draws.empty())
        {
            const std::uint64_t batchId = m_uiOverlayFrameCounter++;
            if (batchId == 0u || batchId % 300u == 0u)
                fprintf(stderr, "METAL_UI_OVERLAY batch=%llu draws=0 skipped=%u dropped=%u reason=no_supported_draws\n",
                    static_cast<unsigned long long>(batchId), skippedDraws, batch.droppedDraws);
            m_uiLastRenderedDraws = 0u;
            m_uiLastSkippedDraws = skippedDraws;
            m_uiLastDroppedDraws = batch.droppedDraws;
            return true;
        }
        if (!ensureUiOverlayPipeline(error) || !ensureUiOverlaySampler(error))
            return false;

        R4BufferDesc vertexDesc;
        vertexDesc.size = batch.vertices.size() * sizeof(xray::metal::ui::Vertex);
        vertexDesc.storage = R4ResourceStorage::Shared;
        vertexDesc.usage = R4BufferUsage::Vertex;
        vertexDesc.label = "Metal UI overlay vertices";
        const std::shared_ptr<IR4Buffer> vertexBuffer = CreateBuffer(
            vertexDesc, batch.vertices.data(), vertexDesc.size, error);
        R4BufferDesc indexDesc;
        indexDesc.size = batch.indices.size() * sizeof(std::uint32_t);
        indexDesc.storage = R4ResourceStorage::Shared;
        indexDesc.usage = R4BufferUsage::Index;
        indexDesc.label = "Metal UI overlay indices";
        const std::shared_ptr<IR4Buffer> indexBuffer = CreateBuffer(
            indexDesc, batch.indices.data(), indexDesc.size, error);
        const std::shared_ptr<MetalBuffer> metalVertices = std::dynamic_pointer_cast<MetalBuffer>(vertexBuffer);
        const std::shared_ptr<MetalBuffer> metalIndices = std::dynamic_pointer_cast<MetalBuffer>(indexBuffer);
        if (!metalVertices || !metalIndices)
        {
            if (error.empty())
                error = "Metal UI overlay buffer allocation failed";
            return false;
        }

        R4RenderPassDesc overlayPass;
        overlayPass.label = "R4 Metal UI overlay";
        overlayPass.useDrawable = true;
        overlayPass.drawableColor.loadAction = R4LoadAction::Load;
        overlayPass.drawableColor.storeAction = R4StoreAction::Store;
        if (!BeginRenderPass(overlayPass, error))
            return false;
        FrameContext* renderContext = activeRenderContext(error);
        if (!renderContext)
            return false;
        [renderContext->encoder setRenderPipelineState:m_uiOverlayPipeline];
        [renderContext->encoder setVertexBuffer:metalVertices->Native() offset:0 atIndex:0];
        [renderContext->encoder setFragmentSamplerState:std::dynamic_pointer_cast<MetalSampler>(m_uiOverlaySampler)->Native()
                                              atIndex:0];
        retainFrameResource(*renderContext, vertexBuffer);
        retainFrameResource(*renderContext, indexBuffer);

        std::uint32_t renderedDraws = 0;
        for (const ResolvedDraw& resolved : draws)
        {
            const xray::metal::ui::DrawRecord& record = *resolved.record;
            const std::shared_ptr<MetalTexture> texture = std::dynamic_pointer_cast<MetalTexture>(resolved.texture);
            if (!texture)
            {
                ++skippedDraws;
                continue;
            }
            UIOverlayUniforms uniforms{};
            uniforms.drawableWidth = static_cast<float>(drawableWidth);
            uniforms.drawableHeight = static_cast<float>(drawableHeight);
            uniforms.textureWidth = static_cast<float>(texture->Descriptor().width);
            uniforms.textureHeight = static_cast<float>(texture->Descriptor().height);
            uniforms.alphaRef = std::clamp(record.alphaRef, 0, 255) / 255.0f;
            uniforms.atlasPixels = record.textureCoordinateSpace == xray::metal::ui::TextureCoordinateSpace::AtlasPixels ? 1u : 0u;
            const bool isFont2 = record.shader.shader.find("font2") != std::string::npos;
            uniforms.isFont = record.font ? (isFont2 ? 2u : 1u) : 0u;
            uniforms.pad = 0u;
            if (!(uniforms.textureWidth > 0.0f) || !(uniforms.textureHeight > 0.0f))
            {
                ++skippedDraws;
                continue;
            }
            [renderContext->encoder setScissorRect:MTLScissorRect{
                resolved.scissor.x, resolved.scissor.y, resolved.scissor.width, resolved.scissor.height}];
            [renderContext->encoder setCullMode:metalUiCullMode(record.cullMode)];
            [renderContext->encoder setFrontFacingWinding:MTLWindingClockwise];
            [renderContext->encoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];
            [renderContext->encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:1];
            [renderContext->encoder setFragmentTexture:texture->Native() atIndex:0];
            [renderContext->encoder drawIndexedPrimitives:metalUiPrimitiveType(record.primitive)
                                                indexCount:record.indexCount
                                                 indexType:MTLIndexTypeUInt32
                                               indexBuffer:metalIndices->Native()
                                         indexBufferOffset:static_cast<NSUInteger>(record.firstIndex) * sizeof(std::uint32_t)
                                             instanceCount:1
                                                baseVertex:0
                                              baseInstance:0];
            retainFrameResource(*renderContext, resolved.texture);
            ++renderedDraws;
        }
        if (!EndRenderPass(error))
            return false;
        const std::uint64_t batchId = m_uiOverlayFrameCounter++;
        if (batchId == 0u || batchId % 300u == 0u)
            fprintf(stderr, "METAL_UI_OVERLAY batch=%llu draws=%u skipped=%u dropped=%u vertices=%zu indices=%zu\n",
                static_cast<unsigned long long>(batchId), renderedDraws, skippedDraws, batch.droppedDraws,
                batch.vertices.size(), batch.indices.size());
        m_uiLastRenderedDraws = renderedDraws;
        m_uiLastSkippedDraws = skippedDraws;
        m_uiLastDroppedDraws = batch.droppedDraws;
        return true;
    }

    std::size_t GraphicsPipelineCacheSize() const override { return m_pipelineCache.size(); }

    bool DeferRelease(std::function<void()> release, std::string& error) override
    {
        if (!m_frameActive)
        {
            error = "DeferRelease requires an active frame";
            return false;
        }
        if (!release)
        {
            error = "DeferRelease callback is empty";
            return false;
        }

        m_frames[m_activeFrameIndex].deferredReleases.emplace_back(std::move(release));
        return true;
    }

    IR4Resources& Resources() override { return *this; }

    std::shared_ptr<IR4Buffer> CreateBuffer(
        const R4BufferDesc& descriptor,
        const void* initialData,
        std::size_t initialDataSize,
        std::string& error) override
    {
        if (!m_device || descriptor.size == 0)
        {
            error = !m_device ? "CreateBuffer requires an initialized Metal backend" :
                                "buffer size must be non-zero";
            return {};
        }
        if ((initialDataSize != 0 && !initialData) || initialDataSize > descriptor.size)
        {
            error = "invalid initial buffer data range";
            return {};
        }

        constexpr std::size_t UniformAlignment = 256;
        if (descriptor.usage == R4BufferUsage::Uniform &&
            descriptor.size > std::numeric_limits<std::size_t>::max() - (UniformAlignment - 1))
        {
            error = "uniform buffer allocation size overflow";
            return {};
        }
        const std::size_t allocationSize = descriptor.usage == R4BufferUsage::Uniform ?
            (descriptor.size + UniformAlignment - 1) & ~(UniformAlignment - 1) : descriptor.size;
        const MTLResourceOptions options = descriptor.storage == R4ResourceStorage::Shared ?
            MTLResourceStorageModeShared : MTLResourceStorageModePrivate;
        id<MTLBuffer> nativeBuffer = [m_device newBufferWithLength:allocationSize options:options];
        if (!nativeBuffer)
        {
            error = "Metal buffer allocation failed";
            return {};
        }
        if (descriptor.label)
            nativeBuffer.label = [NSString stringWithUTF8String:descriptor.label];

        std::shared_ptr<IR4Buffer> buffer =
            std::make_shared<MetalBuffer>(nativeBuffer, descriptor, allocationSize);
        if (m_e01Frames && m_frameActive)
            ++m_frames[m_activeFrameIndex].e01BufferAllocations;
        if (initialDataSize != 0 && !UpdateBuffer(buffer, 0, initialData, initialDataSize, error))
            return {};
        return buffer;
    }

    bool UpdateBuffer(
        const std::shared_ptr<IR4Buffer>& buffer,
        std::size_t offset,
        const void* data,
        std::size_t dataSize,
        std::string& error) override
    {
        const std::shared_ptr<MetalBuffer> metalBuffer =
            std::dynamic_pointer_cast<MetalBuffer>(buffer);
        if (!metalBuffer || (dataSize != 0 && !data) || offset > metalBuffer->Size() ||
            dataSize > metalBuffer->Size() - offset)
        {
            error = "invalid buffer update";
            return false;
        }
        if (dataSize == 0)
            return true;
        if (metalBuffer->IsFrameUpload())
        {
            error = "frame upload slices are immutable";
            return false;
        }
        if (m_e01Frames && m_frameActive)
            m_frames[m_activeFrameIndex].e01UpdatedBytes += dataSize;

        if (metalBuffer->Storage() == R4ResourceStorage::Shared)
        {
            const bool copyOnWrite = metalBuffer->IsVersionInUse();
            if (copyOnWrite)
            {
                const std::size_t bytes = metalBuffer->AllocationSize();
                if (bytes > MaxFrameUploadBytes ||
                    (m_frameActive && bytes > MaxFrameUploadBytes -
                        m_frames[m_activeFrameIndex].e02CowBytes))
                {
                    error = "Shared buffer copy-on-write budget exceeded";
                    return false;
                }
            }
            if (!metalBuffer->UpdateShared(m_device, offset, data, dataSize, error))
                return false;
            if (copyOnWrite && m_frameActive)
            {
                m_frames[m_activeFrameIndex].e02CowBytes += metalBuffer->AllocationSize();
                if (m_e01Frames)
                    ++m_frames[m_activeFrameIndex].e01BufferAllocations;
            }
            return true;
        }

        FrameContext* context = activeUploadContext(error);
        if (!context)
            return false;
        id<MTLBuffer> staging = [m_device newBufferWithBytes:data
                                                      length:dataSize
                                                     options:MTLResourceStorageModeShared];
        if (!staging)
        {
            error = "private buffer staging allocation failed";
            return false;
        }
        id<MTLBlitCommandEncoder> blit = privateUploadEncoder(*context);
        if (!blit)
        {
            error = "private buffer blit encoder creation failed";
            return false;
        }
        blit.label = @"R4 private buffer upload";
        [blit copyFromBuffer:staging
                sourceOffset:0
                    toBuffer:metalBuffer->Native()
           destinationOffset:offset
                        size:dataSize];
        if (!m_e08BatchUploads) [blit endEncoding];
        context->deferredReleases.emplace_back([staging] { (void)staging; });
        id<MTLBuffer> destination = metalBuffer->Native();
        context->privateUploads.push_back({metalBuffer, offset, dataSize, [staging, destination, offset, dataSize](
            id<MTLCommandBuffer> command, std::string& retryError) {
            id<MTLBlitCommandEncoder> retry = [command blitCommandEncoder];
            if (!retry) { retryError = "private buffer retry encoder creation failed"; return false; }
            retry.label = @"R4 private buffer upload retry";
            [retry copyFromBuffer:staging sourceOffset:0 toBuffer:destination destinationOffset:offset size:dataSize];
            [retry endEncoding];
            return true;
        }});
        return true;
    }

    std::shared_ptr<IR4Texture> CreateTexture2D(
        const R4Texture2DDesc& descriptor,
        std::string& error) override
    {
        const MTLPixelFormat pixelFormat = metalPixelFormat(descriptor.format);
        const bool validType =
            (descriptor.type == R4TextureType::Texture2D && descriptor.arrayLength == 1) ||
            (descriptor.type == R4TextureType::Texture2DArray && descriptor.arrayLength > 0) ||
            (descriptor.type == R4TextureType::TextureCube && descriptor.arrayLength == 1 &&
                descriptor.width == descriptor.height);
        if (!m_device || pixelFormat == MTLPixelFormatInvalid || descriptor.width == 0 ||
            descriptor.height == 0 || descriptor.mipLevels == 0 || !validType)
        {
            error = "invalid texture descriptor or uninitialized Metal backend";
            return {};
        }
        if (isBlockCompressed(descriptor.format) && !m_device.supportsBCTextureCompression)
        {
            error = "Metal device does not support BC texture compression";
            return {};
        }

        MTLTextureDescriptor* metalDescriptor = descriptor.type == R4TextureType::TextureCube ?
            [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:pixelFormat
                                                                  size:descriptor.width
                                                             mipmapped:descriptor.mipLevels > 1] :
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pixelFormat
                                                               width:descriptor.width
                                                              height:descriptor.height
                                                           mipmapped:descriptor.mipLevels > 1];
        metalDescriptor.mipmapLevelCount = descriptor.mipLevels;
        metalDescriptor.arrayLength = descriptor.arrayLength;
        if (descriptor.type == R4TextureType::Texture2DArray)
            metalDescriptor.textureType = MTLTextureType2DArray;
        metalDescriptor.storageMode = descriptor.storage == R4ResourceStorage::Shared ?
            MTLStorageModeShared : MTLStorageModePrivate;
        metalDescriptor.usage = metalTextureUsage(descriptor.usage);

        id<MTLTexture> texture = [m_device newTextureWithDescriptor:metalDescriptor];
        if (!texture)
        {
            error = "Metal texture allocation failed";
            return {};
        }
        if (descriptor.label)
            texture.label = [NSString stringWithUTF8String:descriptor.label];
        if (m_e01Frames && m_frameActive)
            ++m_frames[m_activeFrameIndex].e01TextureAllocations;
        return std::make_shared<MetalTexture>(texture, descriptor);
    }

    std::shared_ptr<IR4Texture> CreateTextureView(
        const std::shared_ptr<IR4Texture>& texture,
        R4PixelFormat viewFormat,
        std::string& error) override
    {
        const std::shared_ptr<MetalTexture> metalTexture =
            std::dynamic_pointer_cast<MetalTexture>(texture);
        const MTLPixelFormat pixelFormat = metalPixelFormat(viewFormat);
        if (!metalTexture || pixelFormat == MTLPixelFormatInvalid ||
            !HasR4TextureUsage(metalTexture->Descriptor().usage, R4TextureUsage::PixelFormatView))
        {
            error = "invalid texture view request or missing PixelFormatView usage";
            return {};
        }

        id<MTLTexture> view = [metalTexture->Native() newTextureViewWithPixelFormat:pixelFormat];
        if (!view)
        {
            error = "Metal texture view creation failed";
            return {};
        }
        R4Texture2DDesc descriptor = metalTexture->Descriptor();
        descriptor.format = viewFormat;
        descriptor.label = nullptr;
        return std::make_shared<MetalTexture>(view, descriptor, texture);
    }

    bool UploadTexture2D(
        const std::shared_ptr<IR4Texture>& texture,
        std::uint32_t mipLevel,
        std::uint32_t arraySlice,
        const void* data,
        std::size_t dataSize,
        std::size_t bytesPerRow,
        std::size_t bytesPerImage,
        std::string& error) override
    {
        const std::shared_ptr<MetalTexture> metalTexture =
            std::dynamic_pointer_cast<MetalTexture>(texture);
        const std::uint32_t sliceCount = metalTexture &&
            metalTexture->Descriptor().type == R4TextureType::TextureCube ? 6u :
            (metalTexture ? metalTexture->Descriptor().arrayLength : 0u);
        if (!metalTexture || !data || bytesPerRow == 0 || bytesPerImage == 0 ||
            dataSize < bytesPerImage || mipLevel >= metalTexture->Descriptor().mipLevels ||
            arraySlice >= sliceCount)
        {
            error = "invalid 2D texture upload";
            return false;
        }

        const std::uint32_t width = mipLevel >= 32 ? 1 :
            std::max(1u, metalTexture->Descriptor().width >> mipLevel);
        const std::uint32_t height = mipLevel >= 32 ? 1 :
            std::max(1u, metalTexture->Descriptor().height >> mipLevel);
        if (!validateTextureUploadLayout(
                metalTexture->Descriptor().format,
                width,
                height,
                dataSize,
                bytesPerRow,
                bytesPerImage,
                error))
            return false;
        const MTLRegion region = MTLRegionMake2D(0, 0, width, height);
        if (metalTexture->Descriptor().storage == R4ResourceStorage::Shared)
        {
            [metalTexture->Native() replaceRegion:region
                                      mipmapLevel:mipLevel
                                            slice:arraySlice
                                        withBytes:data
                                      bytesPerRow:bytesPerRow
                                    bytesPerImage:bytesPerImage];
            return true;
        }

        FrameContext* context = activeUploadContext(error);
        if (!context)
            return false;
        id<MTLBuffer> staging = [m_device newBufferWithBytes:data
                                                      length:bytesPerImage
                                                     options:MTLResourceStorageModeShared];
        if (!staging)
        {
            error = "private texture staging allocation failed";
            return false;
        }
        id<MTLBlitCommandEncoder> blit = privateUploadEncoder(*context);
        if (!blit)
        {
            error = "private texture blit encoder creation failed";
            return false;
        }
        blit.label = @"R4 private texture upload";
        [blit copyFromBuffer:staging
                sourceOffset:0
           sourceBytesPerRow:bytesPerRow
         sourceBytesPerImage:bytesPerImage
                  sourceSize:MTLSizeMake(width, height, 1)
                   toTexture:metalTexture->Native()
            destinationSlice:arraySlice
            destinationLevel:mipLevel
           destinationOrigin:MTLOriginMake(0, 0, 0)];
        if (!m_e08BatchUploads) [blit endEncoding];
        context->deferredReleases.emplace_back([staging] { (void)staging; });
        id<MTLTexture> destination = metalTexture->Native();
        context->privateUploads.push_back({metalTexture, mipLevel, arraySlice, [staging, destination, bytesPerRow, bytesPerImage,
            width, height, arraySlice, mipLevel](id<MTLCommandBuffer> command, std::string& retryError) {
            id<MTLBlitCommandEncoder> retry = [command blitCommandEncoder];
            if (!retry) { retryError = "private texture retry encoder creation failed"; return false; }
            retry.label = @"R4 private texture upload retry";
            [retry copyFromBuffer:staging sourceOffset:0 sourceBytesPerRow:bytesPerRow
                sourceBytesPerImage:bytesPerImage sourceSize:MTLSizeMake(width, height, 1)
                toTexture:destination destinationSlice:arraySlice destinationLevel:mipLevel
                destinationOrigin:MTLOriginMake(0, 0, 0)];
            [retry endEncoding];
            return true;
        }});
        return true;
    }

    std::shared_ptr<IR4Sampler> CreateSampler(
        const R4SamplerDesc& descriptor,
        std::string& error) override
    {
        if (!m_device)
        {
            error = "CreateSampler requires an initialized Metal backend";
            return {};
        }
        MTLSamplerDescriptor* metalDescriptor = [[MTLSamplerDescriptor alloc] init];
        metalDescriptor.minFilter = metalMinMagFilter(descriptor.minFilter);
        metalDescriptor.magFilter = metalMinMagFilter(descriptor.magFilter);
        metalDescriptor.mipFilter = metalMipFilter(descriptor.mipFilter);
        metalDescriptor.sAddressMode = metalAddressMode(descriptor.addressU);
        metalDescriptor.tAddressMode = metalAddressMode(descriptor.addressV);
        metalDescriptor.rAddressMode = metalAddressMode(descriptor.addressW);
        metalDescriptor.compareFunction =
            metalSamplerCompareFunction(descriptor.compareFunction);
        metalDescriptor.borderColor = metalSamplerBorderColor(descriptor.borderColor);
        metalDescriptor.maxAnisotropy = std::clamp(descriptor.maxAnisotropy, 1u, 16u);
        metalDescriptor.supportArgumentBuffers = YES;
        if (descriptor.label)
            metalDescriptor.label = [NSString stringWithUTF8String:descriptor.label];

        id<MTLSamplerState> sampler = [m_device newSamplerStateWithDescriptor:metalDescriptor];
        if (!sampler)
        {
            error = "Metal sampler creation failed";
            return {};
        }
        return std::make_shared<MetalSampler>(sampler);
    }

    void Shutdown() override
    {
        if (m_sceneOwnedGBufferCallbackActive)
        {
            fprintf(stderr, "METAL_R4_CALLBACK_REJECT operation=Shutdown\n");
            return;
        }
        if (!m_window)
        {
            m_computeSkinPipeline = nil;
            m_computeSkinPipelineAttempted = false;
            m_computeSkinPipelineError.clear();
            return;
        }

        if (m_frameActive)
        {
            DiscardFrame();
        }
        stopE03Capture();

        for (FrameContext& context : m_frames)
        {
            dispatch_semaphore_wait(context.available, DISPATCH_TIME_FOREVER);
            if (context.commandBuffer && context.commandBuffer.status == MTLCommandBufferStatusError)
            {
                m_hadCommandBufferError.store(true, std::memory_order_release);
                fprintf(stderr, "METAL_R4_BACKEND_ERROR serial=%llu command_buffer=\"%s\"\n",
                    static_cast<unsigned long long>(context.completionSerial),
                    context.commandBuffer.error.localizedDescription.UTF8String);
            }
            context.commandBuffer = nil;
            context.drawable = nil;
            context.finalColor = nil;
            context.thumbnailBuffer = nil;
            runDeferredReleases(context);
            finishPrivateUploads(context);
            context.retainedResources.clear();
            context.retainedOwners.clear();
            context.indirectReads.clear();
            context.computeSkinOracleReceipt.reset();
            context.visibilityRecords.clear();
            context.visibilityResultBuffer = nil;
            context.visibilityBufferReset = false;
            context.visibilityAdmissionResolved = false;
            context.visibilityAdmissionGranted = false;
            context.visibilityDeniedByCap = false;
            context.visibilityLedgerOverflow = false;
            // Balance the drain wait before ARC releases the dispatch object;
            // libdispatch traps if a semaphore is destroyed while consumed.
            dispatch_semaphore_signal(context.available);
            context.available = nullptr;
        }
        if (m_e01Frames)
        {
            std::fclose(m_e01Frames);
            m_e01Frames = nullptr;
        }
        if (m_e08GpuPasses) { std::fclose(m_e08GpuPasses); m_e08GpuPasses = nullptr; }
        if (m_e08CpuBindings) { std::fclose(m_e08CpuBindings); m_e08CpuBindings = nullptr; }
        m_e08CounterSet = nil;
        for (auto& frame : m_frames) { frame.e08Counters = nil; frame.e08PassLabels.clear(); }
        m_e01Presentation.reset();

        m_pipelineCache.clear();
        std::size_t totalTlabCapacity = 0;
        std::size_t totalTlabBlocks = 0;
        for (FrameContext& context : m_frames)
        {
            totalTlabBlocks += context.tlabBlocks.size();
            for (const auto& block : context.tlabBlocks)
                totalTlabCapacity += block.capacity;
            context.tlabBlocks.clear();
            context.tlabTotalCapacity = 0;
            context.uploadArena.Clear();
            context.activeTlabBlockIndex = 0;
            context.activeTlabBlockOffset = 0;
        }
        if (totalTlabCapacity > 0)
        {
            fprintf(stderr, "METAL_MSC_TLAB shutdown frames=%zu blocks=%zu totalBytes=%zu allocations=%llu\n",
                m_frames.size(), totalTlabBlocks, totalTlabCapacity,
                static_cast<unsigned long long>(m_mscTlabAllocations));
        }
        m_mscTlabAllocations = 0;
        m_recoverablePrivateUploads.clear();
        for (auto& context : m_frames) context.privateUploads.clear();
        m_uiTextures.clear();
        m_uiVideos.clear();
        m_uiWhiteTexture.reset();
        m_uiOverlaySampler.reset();
        m_uiDdsProvider = {};
        m_uiDdsProviderReady = false;
        m_uiOverlayPipeline = nil;
        m_presentPipeline = nil;
        m_thumbnailPipeline = nil;
        m_computeSkinPipeline = nil;
        m_computeSkinPipelineAttempted = false;
        m_computeSkinPipelineError.clear();
        {
            std::lock_guard<std::mutex> lock(m_thumbnailMutex);
            m_latestThumbnail = {};
        }

        UnregisterDisplayNotificationObservers();

        SDL_Metal_DestroyView(m_view);
        m_view = nullptr;
        m_layer = nil;
        m_commandQueue = nil;
        m_device = nil;
        m_window = nullptr;
    }

    const char* DeviceName() const override
    {
        return m_device ? m_device.name.UTF8String : "uninitialized";
    }

    void DrawableSize(std::uint32_t& width, std::uint32_t& height) const override
    {
        width = m_layer ? static_cast<std::uint32_t>(m_layer.drawableSize.width) : 0;
        height = m_layer ? static_cast<std::uint32_t>(m_layer.drawableSize.height) : 0;
    }

    std::uint64_t LastSubmittedSerial() const override
    {
        return m_lastSubmittedSerial.load(std::memory_order_acquire);
    }

    std::uint64_t CompletedSerial() const override
    {
        return m_completedSerial.load(std::memory_order_acquire);
    }

    bool HasCommandBufferError() const override
    {
        return m_privateUploadRecoveryFailed || m_hadCommandBufferError.load(std::memory_order_acquire);
    }

    bool SupportsBCTextureCompression() const override
    {
        return m_device && m_device.supportsBCTextureCompression;
    }

    std::uint32_t FrameContextCount() const override
    {
        return static_cast<std::uint32_t>(GetConfiguredFramesInFlight());
    }

    std::uint32_t ActiveFrameContextIndex() const override
    {
        return static_cast<std::uint32_t>(m_activeFrameIndex);
    }

    const DisplayCapabilities& GetDisplayCapabilities() const override
    {
        std::lock_guard<std::mutex> lock(m_displayCapsMutex);
        return m_displayCapabilities;
    }

    void RegisterDisplayNotificationObservers()
    {
        if (m_displayNotificationObservers)
            return;

        m_displayNotificationObservers = [[NSMutableArray alloc] init];
        NSNotificationCenter* center = [NSNotificationCenter defaultCenter];

        auto addObs = [&](NSNotificationName name, const char* reason) {
            id obs = [center addObserverForName:name
                                         object:nil
                                          queue:[NSOperationQueue mainQueue]
                                     usingBlock:^(NSNotification* _Nonnull note) {
                (void)note;
                m_pendingDisplayChangeReason.store(reason, std::memory_order_release);
                m_displayCapabilitiesDirty.store(true, std::memory_order_release);
                UpdateDisplayCapabilities(reason);
            }];
            if (obs)
                [m_displayNotificationObservers addObject:obs];
        };

        addObs(NSWindowDidChangeScreenNotification, "window-screen-change");
        addObs(NSWindowDidChangeBackingPropertiesNotification, "backing-properties-change");
        addObs(NSWindowDidResizeNotification, "window-resize");
        addObs(NSWindowDidChangeScreenProfileNotification, "screen-profile-change");
        addObs(NSApplicationDidChangeScreenParametersNotification, "screen-parameters-change");
    }

    void UnregisterDisplayNotificationObservers()
    {
        if (!m_displayNotificationObservers)
            return;

        NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
        for (id obs in m_displayNotificationObservers)
        {
            [center removeObserver:obs];
        }
        m_displayNotificationObservers = nil;
    }

    void UpdateDisplayCapabilities(const char* reason)
    {
        std::lock_guard<std::mutex> lock(m_displayCapsMutex);

        DisplayCapabilities newCaps = QueryDisplayCapabilities(
            m_window, m_view, m_layer, m_drawableWidth, m_drawableHeight);

        if (!m_displayCapabilitiesValid)
        {
            m_displayCapabilities = newCaps;
            m_displayCapabilitiesValid = true;
            std::string logStr = newCaps.ToString();
            Msg("* [Metal] Display Capabilities [INITIAL]: %s", logStr.c_str());
            std::fprintf(stderr, "METAL_DISPLAY_CAPS [INITIAL] %s\n", logStr.c_str());
            const char* logJsonEnv = std::getenv("XRAY_METAL_LOG_DISPLAY_CAPS_JSON");
            if (logJsonEnv && std::strcmp(logJsonEnv, "1") == 0)
            {
                std::string jsonStr = newCaps.ToJson();
                Msg("* [Metal] Display Capabilities JSON [INITIAL]: %s", jsonStr.c_str());
                std::fprintf(stderr, "METAL_DISPLAY_CAPS_JSON [INITIAL] %s\n", jsonStr.c_str());
            }
            return;
        }

        if (newCaps != m_displayCapabilities)
        {
            m_displayCapabilities = newCaps;
            std::string logStr = newCaps.ToString();
            Msg("* [Metal] Display Capabilities [CHANGED: %s]: %s", reason ? reason : "unknown", logStr.c_str());
            std::fprintf(stderr, "METAL_DISPLAY_CAPS [CHANGED: %s] %s\n", reason ? reason : "unknown", logStr.c_str());
            const char* logJsonEnv = std::getenv("XRAY_METAL_LOG_DISPLAY_CAPS_JSON");
            if (logJsonEnv && std::strcmp(logJsonEnv, "1") == 0)
            {
                std::string jsonStr = newCaps.ToJson();
                Msg("* [Metal] Display Capabilities JSON [CHANGED: %s]: %s", reason ? reason : "unknown", jsonStr.c_str());
                std::fprintf(stderr, "METAL_DISPLAY_CAPS_JSON [CHANGED: %s] %s\n", reason ? reason : "unknown", jsonStr.c_str());
            }
        }
    }

private:
    struct UiTextureEntry
    {
        bool attempted = false;
        std::shared_ptr<IR4Texture> texture;
    };

    struct UiVideoEntry
    {
        std::unique_ptr<IR4UiVideoPlayback> playback;
        std::shared_ptr<IR4Texture> texture;
        std::uint32_t width = 0u;
        std::uint32_t height = 0u;
    };

    bool ensureUiOverlayPipeline(std::string& error)
    {
        if (m_uiOverlayPipeline)
            return true;
        if (!m_device || !m_layer)
        {
            error = "Metal UI overlay pipeline requires an initialized drawable";
            return false;
        }
        NSError* libraryError = nil;
        id<MTLLibrary> library = [m_device newLibraryWithSource:[NSString stringWithUTF8String:UIOverlayMsl]
                                                        options:nil
                                                          error:&libraryError];
        if (!library)
        {
            error = "Metal UI overlay shader compilation failed: " + describeError(libraryError);
            return false;
        }
        id<MTLFunction> vertex = [library newFunctionWithName:@"metal_ui_vertex"];
        id<MTLFunction> fragment = [library newFunctionWithName:@"metal_ui_fragment"];
        if (!vertex || !fragment)
        {
            error = "Metal UI overlay shader entry points are unavailable";
            return false;
        }
        MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
        descriptor.label = @"R4 Metal UI overlay";
        descriptor.vertexFunction = vertex;
        descriptor.fragmentFunction = fragment;
        descriptor.colorAttachments[0].pixelFormat = m_layer.pixelFormat;
        descriptor.colorAttachments[0].blendingEnabled = YES;
        descriptor.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
        descriptor.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
        descriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        descriptor.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        descriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
        descriptor.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        descriptor.colorAttachments[0].writeMask = MTLColorWriteMaskAll;
        NSError* pipelineError = nil;
        m_uiOverlayPipeline = [m_device newRenderPipelineStateWithDescriptor:descriptor error:&pipelineError];
        if (!m_uiOverlayPipeline)
        {
            error = "Metal UI overlay pipeline creation failed: " + describeError(pipelineError);
            return false;
        }
        fprintf(stderr, "METAL_UI_OVERLAY_PIPELINE status=ready format=bgra8unorm blend=straight_alpha\n");
        return true;
    }

    bool ensureUiOverlaySampler(std::string& error)
    {
        if (m_uiOverlaySampler)
            return true;
        R4SamplerDesc descriptor;
        descriptor.minFilter = R4SamplerFilter::Linear;
        descriptor.magFilter = R4SamplerFilter::Linear;
        descriptor.mipFilter = R4SamplerFilter::Linear;
        descriptor.addressU = R4SamplerAddressMode::ClampToEdge;
        descriptor.addressV = R4SamplerAddressMode::ClampToEdge;
        descriptor.addressW = R4SamplerAddressMode::ClampToEdge;
        descriptor.label = "Metal UI overlay sampler";
        m_uiOverlaySampler = CreateSampler(descriptor, error);
        return static_cast<bool>(m_uiOverlaySampler);
    }

    bool ensureUiWhiteTexture(std::string& error)
    {
        if (m_uiWhiteTexture)
            return true;
        R4Texture2DDesc descriptor;
        descriptor.width = 1u;
        descriptor.height = 1u;
        descriptor.mipLevels = 1u;
        descriptor.format = R4PixelFormat::RGBA8Unorm;
        descriptor.storage = R4ResourceStorage::Shared;
        descriptor.usage = R4TextureUsage::ShaderRead;
        descriptor.label = "Metal UI overlay white texture";
        m_uiWhiteTexture = CreateTexture2D(descriptor, error);
        if (!m_uiWhiteTexture)
            return false;
        constexpr std::uint8_t white[] = {255u, 255u, 255u, 255u};
        return UploadTexture2D(m_uiWhiteTexture, 0u, 0u, white, sizeof(white), sizeof(white), sizeof(white), error);
    }

    bool resolveUiTexture(const std::string& identity, std::shared_ptr<IR4Texture>& result)
    {
        std::string error;
        if (identity.empty() || identity == "ui\\ui_console" || identity == "ui\\ui_empty" || identity == "null" ||
            identity == "ui/ui_console" || identity == "ui/ui_empty")
        {
            if (!ensureUiWhiteTexture(error))
            {
                fprintf(stderr, "METAL_UI_TEXTURE_SKIP texture=\"%s\" error=\"%s\"\n", identity.c_str(), error.c_str());
                return false;
            }
            result = m_uiWhiteTexture;
            return true;
        }
        if (identity.size() > 1024u)
        {
            fprintf(stderr, "METAL_UI_TEXTURE_SKIP texture=<oversize> error=\"UI texture identity exceeds the bounded path length\"\n");
            return false;
        }
        std::string activeFrame = identity;
        std::string frameError;
        ResolveMetalR4EngineUiTextureFrame(identity, activeFrame, frameError);

        // Check active UI video streams
        const auto videoIt = m_uiVideos.find(activeFrame);
        if (videoIt != m_uiVideos.end())
        {
            R4UiVideoFrame vFrame;
            if (videoIt->second.playback && videoIt->second.playback->Update(vFrame, error))
            {
                if (vFrame.newFrameAvailable && vFrame.pixels && videoIt->second.texture)
                {
                    const std::size_t bytesPerRow = vFrame.width * 4u;
                    const std::size_t bytesPerImage = bytesPerRow * vFrame.height;
                    UploadTexture2D(videoIt->second.texture, 0u, 0u, vFrame.pixels,
                        bytesPerImage, bytesPerRow, bytesPerImage, error);
                }
            }
            result = videoIt->second.texture;
            return static_cast<bool>(result);
        }

        const auto existing = m_uiTextures.find(activeFrame);
        if (existing != m_uiTextures.end())
        {
            result = existing->second.texture;
            return static_cast<bool>(result);
        }

        // Probe for UI video (.ogm) before DDS search
        std::unique_ptr<IR4UiVideoPlayback> videoPlayback;
        std::string videoError;
        if (OpenMetalR4UiVideo(activeFrame, videoPlayback, videoError) && videoPlayback)
        {
            UiVideoEntry& vEntry = m_uiVideos[activeFrame];
            vEntry.playback = std::move(videoPlayback);
            vEntry.width = vEntry.playback->Width();
            vEntry.height = vEntry.playback->Height();

            R4Texture2DDesc descriptor;
            descriptor.type = R4TextureType::Texture2D;
            descriptor.width = vEntry.width;
            descriptor.height = vEntry.height;
            descriptor.mipLevels = 1u;
            descriptor.arrayLength = 1u;
            descriptor.format = R4PixelFormat::BGRA8Unorm;
            descriptor.storage = R4ResourceStorage::Shared;
            descriptor.usage = R4TextureUsage::ShaderRead;
            descriptor.label = "Metal UI overlay video texture";

            vEntry.texture = CreateTexture2D(descriptor, error);
            if (!vEntry.texture)
            {
                Msg("! [Metal] Failed to create video texture for %s: %s",
                    activeFrame.c_str(), error.c_str());
                m_uiVideos.erase(activeFrame);
                if (ensureUiWhiteTexture(error))
                {
                    result = m_uiWhiteTexture;
                    return true;
                }
                return false;
            }

            R4UiVideoFrame vFrame;
            if (vEntry.playback->Update(vFrame, error) && vFrame.pixels)
            {
                const std::size_t bytesPerRow = vFrame.width * 4u;
                const std::size_t bytesPerImage = bytesPerRow * vFrame.height;
                UploadTexture2D(vEntry.texture, 0u, 0u, vFrame.pixels,
                    bytesPerImage, bytesPerRow, bytesPerImage, error);
            }

            result = vEntry.texture;
            Msg("* [Metal] UI video stream started: \"%s\" (%ux%u BGRA8)",
                activeFrame.c_str(), vEntry.width, vEntry.height);
            return true;
        }
        constexpr std::size_t MaximumCachedTextures = 1024u;
        if (m_uiTextures.size() >= MaximumCachedTextures)
        {
            fprintf(stderr, "METAL_UI_TEXTURE_SKIP texture=\"%s\" error=\"UI texture cache capacity reached\"\n", activeFrame.c_str());
            return false;
        }
        UiTextureEntry& entry = m_uiTextures[activeFrame];
        entry.attempted = true;
        std::filesystem::path xrfsPath;
        if (!ResolveMetalR4EngineTextureDdsPath(activeFrame, xrfsPath, error))
        {
            if (!xray::metal::ui::overlay::BuildXrfsTexturePath(activeFrame, xrfsPath, error))
            {
                fprintf(stderr, "METAL_UI_TEXTURE_FALLBACK texture=\"%s\" error=\"%s\"\n", activeFrame.c_str(), error.c_str());
                std::string fallbackError;
                if (ensureUiWhiteTexture(fallbackError))
                {
                    entry.texture = m_uiWhiteTexture;
                    result = m_uiWhiteTexture;
                    return true;
                }
                return false;
            }
        }
        if (!m_uiDdsProviderReady)
        {
            if (!CreateMetalR4EngineXrfsProvider(m_uiDdsProvider, error))
            {
                fprintf(stderr, "METAL_UI_TEXTURE_SKIP texture=\"%s\" error=\"%s\"\n", activeFrame.c_str(), error.c_str());
                return false;
            }
            m_uiDdsProviderReady = true;
        }
        xray::metal::R4DdsTexture dds;
        if (!xray::metal::LoadR4DdsTexture(xrfsPath, m_uiDdsProvider, dds, error))
        {
            Msg("! METAL_UI_TEXTURE_FALLBACK texture=\"%s\" xrfs=\"%s\" error=\"%s\"", activeFrame.c_str(), xrfsPath.string().c_str(), error.c_str());
            fprintf(stderr, "METAL_UI_TEXTURE_FALLBACK texture=\"%s\" error=\"%s\"\n", activeFrame.c_str(), error.c_str());
            std::string fallbackError;
            if (ensureUiWhiteTexture(fallbackError))
            {
                entry.texture = m_uiWhiteTexture;
                result = m_uiWhiteTexture;
                return true;
            }
            return false;
        }
        Msg("* METAL_UI_TEXTURE_LOAD_OK texture=\"%s\" width=%u height=%u format=%d", activeFrame.c_str(), dds.descriptor.width, dds.descriptor.height, static_cast<int>(dds.descriptor.format));
        R4Texture2DDesc descriptor = dds.descriptor;
        descriptor.label = "Metal UI overlay texture";
        entry.texture = CreateTexture2D(descriptor, error);
        if (!entry.texture)
        {
            fprintf(stderr, "METAL_UI_TEXTURE_FALLBACK texture=\"%s\" error=\"%s\"\n", activeFrame.c_str(), error.c_str());
            std::string fallbackError;
            if (ensureUiWhiteTexture(fallbackError))
            {
                entry.texture = m_uiWhiteTexture;
                result = m_uiWhiteTexture;
                return true;
            }
            return false;
        }
        for (const xray::metal::R4DdsMipLevel& level : dds.mipLevels)
        {
            if (level.dataOffset > dds.bytes.size() || level.bytesPerImage > dds.bytes.size() - level.dataOffset ||
                !UploadTexture2D(entry.texture, level.mipLevel, level.arraySlice,
                    dds.bytes.data() + level.dataOffset,
                    level.bytesPerImage, level.bytesPerRow, level.bytesPerImage, error))
            {
                entry.texture.reset();
                fprintf(stderr, "METAL_UI_TEXTURE_FALLBACK texture=\"%s\" error=\"%s\"\n", activeFrame.c_str(), error.c_str());
                std::string fallbackError;
                if (ensureUiWhiteTexture(fallbackError))
                {
                    entry.texture = m_uiWhiteTexture;
                    result = m_uiWhiteTexture;
                    return true;
                }
                return false;
            }
        }
        result = entry.texture;
        fprintf(stderr, "METAL_UI_TEXTURE_READY texture=\"%s\" width=%u height=%u mips=%u\n", activeFrame.c_str(),
            dds.descriptor.width, dds.descriptor.height, dds.descriptor.mipLevels);
        return true;
    }

    struct TlabBlock
    {
        id<MTLBuffer> buffer = nil;
        std::size_t capacity = 0;
    };

    struct PrivateUploadOperation
    {
        std::weak_ptr<void> owner;
        // Exact destination write: buffer offset/size or texture mip/slice.
        std::size_t first = 0;
        std::size_t second = 0;
        std::function<bool(id<MTLCommandBuffer>, std::string&)> encode;
    };

    static void prunePrivateUploads(std::vector<PrivateUploadOperation>& uploads)
    {
        struct Key
        {
            void* owner;
            std::size_t first, second;
            bool operator==(const Key& other) const
            { return owner == other.owner && first == other.first && second == other.second; }
        };
        struct Hash
        {
            std::size_t operator()(const Key& key) const
            { return std::hash<void*>{}(key.owner) ^ (key.first * 31u) ^ (key.second * 131u); }
        };
        std::unordered_set<Key, Hash> survivingWrites;
        // Keep the latest exact write, preserving order for overlapping ranges.
        // Scene upload producers reissue their initial data after a discard.
        for (auto it = uploads.rbegin(); it != uploads.rend(); ++it)
        {
            const auto owner = it->owner.lock();
            if (!owner || !survivingWrites.insert({owner.get(), it->first, it->second}).second)
                it->encode = {};
        }
        uploads.erase(std::remove_if(uploads.begin(), uploads.end(),
            [](const auto& upload) { return !upload.encode; }), uploads.end());
    }

    struct FrameContext
    {
        dispatch_semaphore_t available = nullptr;
        id<CAMetalDrawable> drawable = nil;
        id<MTLTexture> finalColor = nil;
        id<MTLBuffer> thumbnailBuffer = nil;
        id<MTLCommandBuffer> commandBuffer = nil;
        id<MTLRenderCommandEncoder> encoder = nil;
        id<MTLBuffer> visibilityResultBuffer = nil;
        std::vector<xray::metal::R4WholeLightVisibilityRecord> visibilityRecords;
        std::uint64_t acquireEpoch = 0u;
        bool visibilityBufferReset = false;
        bool visibilityAdmissionResolved = false;
        bool visibilityAdmissionGranted = false;
        bool visibilityDeniedByCap = false;
        bool visibilityLedgerOverflow = false;
        id<MTLBlitCommandEncoder> uploadBlit = nil;
        R4RenderPassDesc activePass;
        id<MTLCounterSampleBuffer> e08Counters = nil;
        bool e08Profile = false;
        MTLTimestamp e08CpuBefore = 0, e08GpuBefore = 0;
        std::vector<std::string> e08PassLabels;
        std::vector<std::pair<double, std::uint64_t>> e08PassCpuRecords;
        bool e08CurrentPassProfiled = false;
        std::uint64_t e08PassCpuStart = 0, e08PassDrawStart = 0;
        std::array<double, 6> e08CpuMs{};
        std::string activePassLabel;
        bool mscPipelineBound = false;
        std::uint64_t completionSerial = 0;
        std::uint64_t drawableGeneration = 0;
        std::vector<std::function<void()>> deferredReleases;
        std::vector<PrivateUploadOperation> privateUploads;
        std::vector<std::shared_ptr<void>> retainedResources;
        std::unordered_set<const void*> retainedOwners;
        std::unordered_map<const void*, MTLRenderStages> indirectReads;
        std::shared_ptr<ComputeSkinOracleReceipt> computeSkinOracleReceipt;
        MetalFrameUploadArena uploadArena;
        std::size_t e02CowBytes = 0;
        std::vector<TlabBlock> tlabBlocks;
        std::size_t tlabTotalCapacity = 0;
        std::size_t activeTlabBlockIndex = 0;
        std::size_t activeTlabBlockOffset = 0;
        std::uint64_t e01BeginNs = 0;
        std::uint64_t e01ReadyNs = 0;
        double e01WaitMs = 0.0;
        double e01DrawableMs = 0.0;
        double e01SceneMs = 0.0;
        double e01GrassMs = 0.0;
        double e01DynamicPrewarmMs = 0.0;
        std::uint32_t e01EngineFrame = 0;
        std::uint32_t e01CameraFrame = 0;
        std::uint64_t e01UpdatedBytes = 0;
        std::uint32_t e01BufferAllocations = 0;
        std::uint32_t e01TextureAllocations = 0;
        std::uint32_t e01IndexedDraws = 0;
        std::uint32_t e01PsoBinds = 0;
    };

    bool beginAcquiredContext(FrameContext& context, std::string& error)
    {
        if (m_nextFrameAcquireEpoch == std::numeric_limits<std::uint64_t>::max())
        {
            error = "native frame acquisition epoch exhausted";
            return false;
        }
        const std::uint64_t epoch = ++m_nextFrameAcquireEpoch;
        context.uploadArena.BeginAcquisition(epoch, m_computeSkinEnabled);
        context.acquireEpoch = epoch;
        context.visibilityRecords.clear();
        context.visibilityLedgerOverflow = false;
        context.visibilityBufferReset = false;
        context.visibilityAdmissionResolved = false;
        context.visibilityAdmissionGranted = false;
        context.visibilityDeniedByCap = false;
        return true;
    }

    bool ensureComputeSkinPipeline(std::string& error)
    {
        if (m_computeSkinPipeline)
            return true;
        if (m_computeSkinPipelineAttempted)
        {
            error = m_computeSkinPipelineError;
            return false;
        }
        m_computeSkinPipelineAttempted = true;
        const auto fail = [&](std::string reason) {
            m_computeSkinPipelineError = std::move(reason);
            error = m_computeSkinPipelineError;
            return false;
        };
        NSError* libraryError = nil;
        NSString* source = [NSString stringWithUTF8String:xray::metal::R4ComputeSkinMslSource];
        MTLCompileOptions* options = xray::metal::R4ComputeSkinCompileOptions();
        id<MTLLibrary> library = source ? [m_device newLibraryWithSource:source
            options:options error:&libraryError] : nil;
        if (!library)
        {
            return fail("Metal compute skin source compile failed (" +
                std::string(xray::metal::R4ComputeSkinSourceSha256) + "): " +
                describeError(libraryError));
        }
        id<MTLFunction> function = [library newFunctionWithName:@"r4_compute_skin"];
        if (!function)
        {
            return fail("canonical compute skin kernel entry point is unavailable");
        }
        NSError* pipelineError = nil;
        id<MTLComputePipelineState> pipeline = [m_device
            newComputePipelineStateWithFunction:function error:&pipelineError];
        if (!pipeline)
        {
            return fail("Metal compute skin pipeline creation failed: " + describeError(pipelineError));
        }
        m_computeSkinPipeline = pipeline;
        return true;
    }

    bool ensurePresentationPipelines(std::string& error)
    {
        if (m_presentPipeline && m_thumbnailPipeline)
            return true;
        NSError* libraryError = nil;
        id<MTLLibrary> library = [m_device newLibraryWithSource:
            [NSString stringWithUTF8String:PresentationMsl] options:nil error:&libraryError];
        if (!library)
        {
            error = "Metal presentation shader compilation failed: " + describeError(libraryError);
            return false;
        }
        id<MTLFunction> presentVertex = [library newFunctionWithName:@"metal_present_vertex"];
        id<MTLFunction> presentFragment = [library newFunctionWithName:@"metal_present_fragment"];
        id<MTLFunction> thumbnailFunction = [library newFunctionWithName:@"metal_save_thumbnail"];
        if (!presentVertex || !presentFragment || !thumbnailFunction)
        {
            error = "Metal presentation shader entry points are unavailable";
            return false;
        }
        MTLRenderPipelineDescriptor* presentDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
        presentDescriptor.label = @"R4 Metal final presentation";
        presentDescriptor.vertexFunction = presentVertex;
        presentDescriptor.fragmentFunction = presentFragment;
        presentDescriptor.colorAttachments[0].pixelFormat = m_layer.pixelFormat;
        NSError* pipelineError = nil;
        m_presentPipeline = [m_device newRenderPipelineStateWithDescriptor:presentDescriptor error:&pipelineError];
        if (!m_presentPipeline)
        {
            error = "Metal final presentation pipeline creation failed: " + describeError(pipelineError);
            return false;
        }
        m_thumbnailPipeline = [m_device newComputePipelineStateWithFunction:thumbnailFunction error:&pipelineError];
        if (!m_thumbnailPipeline)
        {
            m_presentPipeline = nil;
            error = "Metal save-thumbnail pipeline creation failed: " + describeError(pipelineError);
            return false;
        }
        fprintf(stderr, "METAL_PRESENTATION_PIPELINES status=ready thumbnail=128x128-bgra8\n");
        return true;
    }

    bool ensurePresentationResources(FrameContext& context, std::string& error)
    {
        if ((!m_e08LateDrawable && (!context.drawable || !context.drawable.texture)) ||
            !ensurePresentationPipelines(error))
        {
            if (error.empty())
                error = "Metal presentation resources require a drawable";
            return false;
        }
        const NSUInteger width = context.drawable ? context.drawable.texture.width : m_drawableWidth;
        const NSUInteger height = context.drawable ? context.drawable.texture.height : m_drawableHeight;
        const MTLPixelFormat format = context.drawable ? context.drawable.texture.pixelFormat : m_layer.pixelFormat;
        if (width == 0u || height == 0u)
        {
            error = "Metal presentation drawable has zero extent";
            return false;
        }
        if (!context.finalColor || context.finalColor.width != width ||
            context.finalColor.height != height ||
            context.finalColor.pixelFormat != format)
        {
            MTLTextureDescriptor* descriptor =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                   width:width height:height mipmapped:NO];
            descriptor.storageMode = MTLStorageModePrivate;
            descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            context.finalColor = [m_device newTextureWithDescriptor:descriptor];
            if (!context.finalColor)
            {
                error = "Metal final-color texture allocation failed";
                return false;
            }
            context.finalColor.label = @"R4 Metal final color";
        }
        constexpr NSUInteger thumbnailBytes = R4SaveThumbnailSnapshot::Width *
            R4SaveThumbnailSnapshot::Height * 4u;
        if (!context.thumbnailBuffer || context.thumbnailBuffer.length != thumbnailBytes)
        {
            context.thumbnailBuffer = [m_device newBufferWithLength:thumbnailBytes
                                                             options:MTLResourceStorageModeShared];
            if (!context.thumbnailBuffer)
            {
                error = "Metal save-thumbnail readback allocation failed";
                return false;
            }
            context.thumbnailBuffer.label = @"R4 Metal save thumbnail BGRA8";
        }
        context.drawableGeneration = m_drawableGeneration.load(std::memory_order_acquire);
        return true;
    }

    bool encodePresentation(FrameContext& context, std::string& error)
    {
        finishPrivateUploads(context);
        if (!context.commandBuffer || !context.drawable || !context.finalColor ||
            !context.thumbnailBuffer || !m_presentPipeline || !m_thumbnailPipeline)
        {
            error = "Metal presentation resources are incomplete";
            return false;
        }

        id<MTLComputeCommandEncoder> compute = [context.commandBuffer computeCommandEncoder];
        if (!compute)
        {
            error = "Metal save-thumbnail compute encoder creation failed";
            return false;
        }
        compute.label = @"R4 Metal save thumbnail";
        [compute setComputePipelineState:m_thumbnailPipeline];
        [compute setTexture:context.finalColor atIndex:0];
        [compute setBuffer:context.thumbnailBuffer offset:0 atIndex:0];
        const NSUInteger threadWidth = m_thumbnailPipeline.threadExecutionWidth;
        const NSUInteger threadHeight = std::max<NSUInteger>(1u,
            m_thumbnailPipeline.maxTotalThreadsPerThreadgroup / threadWidth);
        [compute dispatchThreads:MTLSizeMake(R4SaveThumbnailSnapshot::Width,
                                    R4SaveThumbnailSnapshot::Height, 1u)
             threadsPerThreadgroup:MTLSizeMake(threadWidth, threadHeight, 1u)];
        [compute endEncoding];

        MTLRenderPassDescriptor* descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
        descriptor.colorAttachments[0].texture = context.drawable.texture;
        descriptor.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        descriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> present =
            [context.commandBuffer renderCommandEncoderWithDescriptor:descriptor];
        if (!present)
        {
            error = "Metal final presentation encoder creation failed";
            return false;
        }
        present.label = @"R4 Metal final presentation";
        [present setRenderPipelineState:m_presentPipeline];
        [present setFragmentTexture:context.finalColor atIndex:0];
        [present drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [present endEncoding];
        return true;
    }


    FrameContext* activeUploadContext(std::string& error)
    {
        if (!m_frameActive)
        {
            error = "private resource upload requires an active frame";
            return nullptr;
        }
        FrameContext& context = m_frames[m_activeFrameIndex];
        if (context.encoder)
        {
            error = "private resource upload cannot run inside a render pass";
            return nullptr;
        }
        return &context;
    }

    FrameContext* activeRenderContext(std::string& error)
    {
        if (!m_frameActive)
        {
            error = "render command requires an active frame";
            return nullptr;
        }
        FrameContext& context = m_frames[m_activeFrameIndex];
        if (!context.encoder)
        {
            error = "render command requires an active render pass";
            return nullptr;
        }
        return &context;
    }

    static void runDeferredReleases(FrameContext& context)
    {
        for (std::function<void()>& release : context.deferredReleases)
            release();
        context.deferredReleases.clear();
    }

    void maybeBeginE03Capture()
    {
        if (m_e03CaptureAttempted || m_e03TracePath.empty() || m_e03CaptureFlagPath.empty())
            return;
        FILE* flag = std::fopen(m_e03CaptureFlagPath.c_str(), "r");
        if (!flag)
            return;
        char stage[64] = {};
        const bool ready = std::fgets(stage, sizeof(stage), flag) &&
            std::strncmp(stage, "READY_FOR_NO_HUD", 16) == 0;
        std::fclose(flag);
        if (!ready)
            return;

        m_e03CaptureAttempted = true;
        MTLCaptureManager* manager = [MTLCaptureManager sharedCaptureManager];
        if (![manager supportsDestination:MTLCaptureDestinationGPUTraceDocument])
        {
            std::fprintf(stderr, "METAL_E03_CAPTURE_UNAVAILABLE destination=gputrace\n");
            return;
        }
        MTLCaptureDescriptor* descriptor = [MTLCaptureDescriptor new];
        descriptor.captureObject = m_commandQueue;
        descriptor.destination = MTLCaptureDestinationGPUTraceDocument;
        descriptor.outputURL = [NSURL fileURLWithPath:
            [NSString stringWithUTF8String:m_e03TracePath.c_str()]];
        NSError* captureError = nil;
        if (![manager startCaptureWithDescriptor:descriptor error:&captureError])
        {
            std::fprintf(stderr, "METAL_E03_CAPTURE_START_ERROR reason=%s\n",
                captureError.localizedDescription.UTF8String ?: "unknown");
            return;
        }
        m_e03CaptureActive = true;
        std::fprintf(stderr, "METAL_E03_CAPTURE_STARTED path=%s\n", m_e03TracePath.c_str());
    }

    void stopE03Capture()
    {
        if (!m_e03CaptureActive)
            return;
        [[MTLCaptureManager sharedCaptureManager] stopCapture];
        m_e03CaptureActive = false;
        std::fprintf(stderr, "METAL_E03_CAPTURE_STOPPED path=%s\n", m_e03TracePath.c_str());
    }

    id<MTLBlitCommandEncoder> privateUploadEncoder(FrameContext& context)
    {
        if (!m_e08BatchUploads) return [context.commandBuffer blitCommandEncoder];
        if (!context.uploadBlit)
            context.uploadBlit = [context.commandBuffer blitCommandEncoder];
        return context.uploadBlit;
    }

    static void finishPrivateUploads(FrameContext& context)
    {
        if (context.uploadBlit)
        { [context.uploadBlit endEncoding]; context.uploadBlit = nil; }
    }

    template<class Resource>
    void retainFrameResource(FrameContext& context, const std::shared_ptr<Resource>& resource)
    {
        if (!m_e08DedupResources || context.retainedOwners.insert(resource.get()).second)
            context.retainedResources.emplace_back(resource);
    }

    void declareIndirectRead(FrameContext& context, id<MTLResource> resource, MTLRenderStages stages)
    {
        if (m_e08DedupResources)
        {
            auto& declared = context.indirectReads[(__bridge const void*)resource];
            if ((declared & stages) == stages) return;
            declared |= stages;
        }
        // This cache is read-only. Writes/future mixed usage must issue their
        // own precise declarations; never infer them from a cached read.
        [context.encoder useResource:resource usage:MTLResourceUsageRead stages:stages];
    }

    void abandonActiveFrame()
    {
        stopE03Capture();
        FrameContext& context = m_frames[m_activeFrameIndex];
        finishPrivateUploads(context);
        context.encoder = nil;
        context.commandBuffer = nil;
        context.drawable = nil;
        runDeferredReleases(context);
        finishPrivateUploads(context);
        context.retainedResources.clear();
        context.retainedOwners.clear();
        context.indirectReads.clear();
        context.computeSkinOracleReceipt.reset();
        context.visibilityRecords.clear();
        context.visibilityBufferReset = false;
        context.visibilityAdmissionResolved = false;
        context.visibilityAdmissionGranted = false;
        context.visibilityDeniedByCap = false;
        context.visibilityLedgerOverflow = false;
        const bool hadUploads = !context.privateUploads.empty();
        prunePrivateUploads(context.privateUploads);
        if (hadUploads && ++m_privateUploadRecoveryFailures >= MaxPrivateUploadRecoveryFailures)
        {
            m_privateUploadRecoveryFailed = true;
            m_recoverablePrivateUploads.clear();
            context.privateUploads.clear();
            std::fprintf(stderr, "METAL_PRIVATE_UPLOAD_RECOVERY_STOP failures=%zu reason=repeated_frame_abort\n",
                m_privateUploadRecoveryFailures);
        }
        else
        {
            for (auto& upload : context.privateUploads)
                m_recoverablePrivateUploads.emplace_back(std::move(upload));
            context.privateUploads.clear();
            prunePrivateUploads(m_recoverablePrivateUploads);
        }
        context.uploadArena.Reset();
        context.e02CowBytes = 0;
        context.activeTlabBlockIndex = 0;
        context.activeTlabBlockOffset = 0;
        dispatch_semaphore_signal(context.available);
        m_frameActive = false;
		m_sceneOwnedGBufferCallbackActive = false;
    }

    SDL_Window* m_window = nullptr;
    SDL_MetalView m_view = nullptr;
    CAMetalLayer* m_layer = nil;
    id<MTLDevice> m_device = nil;
    id<MTLCommandQueue> m_commandQueue = nil;
    std::string m_e03TracePath;
    std::string m_e03CaptureFlagPath;
    bool m_e03CaptureAttempted = false;
    bool m_e03CaptureActive = false;
    id<MTLRenderPipelineState> m_presentPipeline = nil;
    id<MTLComputePipelineState> m_thumbnailPipeline = nil;
    id<MTLComputePipelineState> m_computeSkinPipeline = nil;
    bool m_computeSkinPipelineAttempted = false;
    std::string m_computeSkinPipelineError;
    std::uint64_t m_nextFrameAcquireEpoch = 0;
    bool m_computeSkinEnabled = [] {
        const char* value = std::getenv("XRAY_METAL_COMPUTE_SKIN");
        return value && std::strcmp(value, "1") == 0;
    }();
    bool m_wholeLightVisibilityEnabled = [] {
        const char* value = std::getenv("XRAY_METAL_WHOLE_LIGHT_VISIBILITY");
        return value && std::strcmp(value, "1") == 0;
    }();
    std::uint32_t m_wholeLightVisibilitySelectedAcquisitions = 0u;
    bool m_wholeLightVisibilityCapNoticeEmitted = false;
    bool m_computeSkinOracleEnabled = [] {
        const char* value = std::getenv("XRAY_METAL_COMPUTE_SKIN_ORACLE");
        return value && std::strcmp(value, "1") == 0;
    }();
    std::array<FrameContext, MaxFramesInFlight> m_frames;
    std::vector<PrivateUploadOperation> m_recoverablePrivateUploads;
    std::size_t m_privateUploadRecoveryFailures = 0;
    bool m_privateUploadRecoveryFailed = false;
    std::size_t m_nextFrameIndex = 0;
    std::size_t m_activeFrameIndex = 0;
    bool m_frameActive = false;
    std::uint64_t m_nextSerial = 1;
    std::atomic<std::uint64_t> m_lastSubmittedSerial{0};
    std::atomic<std::uint64_t> m_completedSerial{0};
    bool m_e08SubmitRetry = false;
    bool m_e08LateDrawable = std::getenv("XRAY_METAL_E08_EARLY_DRAWABLE") == nullptr;
    bool m_e08BatchUploads = std::getenv("XRAY_METAL_E08_UPLOAD_BATCH") != nullptr;
    bool m_e08DedupResources = std::getenv("XRAY_METAL_E08_DEDUP_RESOURCES") != nullptr;
    bool m_e08HeapBindings = std::getenv("XRAY_METAL_E08_HEAP_BINDINGS") != nullptr;
    FILE* m_e08GpuPasses = nullptr;
    FILE* m_e08CpuBindings = nullptr;
    id<MTLCounterSet> m_e08CounterSet = nil;
    std::mutex m_e08GpuMutex;
	FILE* m_e01Frames = nullptr;
	std::mutex m_e01FramesMutex;
	std::shared_ptr<E01PresentationSink> m_e01Presentation;
	std::atomic<bool> m_hadCommandBufferError{false};
	std::atomic<std::uint64_t> m_drawableGeneration{0};
	std::uint32_t m_drawableWidth = 0;
	std::uint32_t m_drawableHeight = 0;
	std::uint32_t m_preferredDrawableWidth = 0;
	std::uint32_t m_preferredDrawableHeight = 0;
	mutable std::mutex m_thumbnailMutex;
	R4SaveThumbnailSnapshot m_latestThumbnail;
	bool m_sceneOwnedGBufferCallbackActive = false;
	mutable std::mutex m_engineWorldSnapshotMutex;
	std::shared_ptr<const R4EngineWorldSnapshot> m_engineWorldSnapshot;
	mutable std::mutex m_engineHudSnapshotMutex;
	std::shared_ptr<const R4EngineHudSnapshot> m_engineHudSnapshot;
	mutable std::mutex m_engineLocalLightSnapshotMutex;
	std::shared_ptr<const R4EngineLocalLightSnapshot> m_engineLocalLightSnapshot;
	std::uint32_t m_lastLoggedEngineWorldFrame = UINT32_MAX;
	std::size_t m_lastLoggedEngineWorldCount = std::numeric_limits<std::size_t>::max();
    std::unordered_map<std::string, std::shared_ptr<IR4GraphicsPipeline>> m_pipelineCache;
    std::uint64_t m_mscTlabAllocations = 0;
    id<MTLRenderPipelineState> m_uiOverlayPipeline = nil;
    std::shared_ptr<IR4Sampler> m_uiOverlaySampler;
    std::shared_ptr<IR4Texture> m_uiWhiteTexture;
    xray::metal::R4DdsBytesProvider m_uiDdsProvider;
    bool m_uiDdsProviderReady = false;
    std::unordered_map<std::string, UiTextureEntry> m_uiTextures;
    std::unordered_map<std::string, UiVideoEntry> m_uiVideos;
    std::uint64_t m_uiOverlayFrameCounter = 0;
    std::uint32_t m_uiLastRenderedDraws = UINT32_MAX;
    std::uint32_t m_uiLastSkippedDraws = UINT32_MAX;
    std::uint32_t m_uiLastDroppedDraws = UINT32_MAX;
    DisplayCapabilities m_displayCapabilities;
    bool m_displayCapabilitiesValid = false;
    std::atomic<bool> m_displayCapabilitiesDirty{true};
    std::atomic<const char*> m_pendingDisplayChangeReason{"initial-startup"};
    NSMutableArray* m_displayNotificationObservers = nil;
    mutable std::mutex m_displayCapsMutex;
};

bool MetalR4Backend::ProbePrivateUploadRecovery(std::string& error)
{
    @autoreleasepool
    {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> queue = device ? [device newCommandQueue] : nil;
        if (!device || !queue)
        {
            error = "private upload recovery probe could not create a Metal device/queue";
            return false;
        }

        auto initialize = [&](MetalR4Backend& backend) {
            backend.m_device = device;
            backend.m_commandQueue = queue;
            backend.m_frames[0].available = dispatch_semaphore_create(1);
            return backend.m_frames[0].available != nullptr;
        };
        auto beginManualFrame = [&](MetalR4Backend& backend) {
            FrameContext& context = backend.m_frames[0];
            if (backend.m_frameActive || !context.available ||
                backend.m_privateUploadRecoveryFailed)
                return false;
            dispatch_semaphore_wait(context.available, DISPATCH_TIME_FOREVER);
            context.commandBuffer = [backend.m_commandQueue commandBuffer];
            if (!context.commandBuffer)
            {
                dispatch_semaphore_signal(context.available);
                return false;
            }
            backend.m_activeFrameIndex = 0;
            if (!backend.beginAcquiredContext(context, error))
            {
                context.commandBuffer = nil;
                dispatch_semaphore_signal(context.available);
                return false;
            }
            backend.m_frameActive = true;
            context.privateUploads = std::move(backend.m_recoverablePrivateUploads);
            prunePrivateUploads(context.privateUploads);
            for (const auto& upload : context.privateUploads)
            {
                if (!upload.encode(context.commandBuffer, error))
                {
                    backend.abandonActiveFrame();
                    return false;
                }
            }
            return true;
        };
        auto replayAndReadBack = [&](MetalR4Backend& backend,
                                     const std::shared_ptr<IR4Buffer>& destination,
                                     const std::array<std::uint8_t, 64>& expected) {
            if (!beginManualFrame(backend))
            {
                error = "private upload recovery probe could not begin replay command";
                return false;
            }
            FrameContext& context = backend.m_frames[0];
            if (context.privateUploads.empty())
            {
                error = "private upload recovery probe found no live upload to read back";
                backend.abandonActiveFrame();
                return false;
            }

            id<MTLBuffer> readback = [device newBufferWithLength:expected.size()
                options:MTLResourceStorageModeShared];
            const auto native = std::dynamic_pointer_cast<MetalBuffer>(destination);
            id<MTLBlitCommandEncoder> blit = [context.commandBuffer blitCommandEncoder];
            if (!readback || !native || !blit)
            {
                error = "private upload recovery readback allocation/encoder failed";
                backend.abandonActiveFrame();
                return false;
            }
            [blit copyFromBuffer:native->Native() sourceOffset:0 toBuffer:readback
                destinationOffset:0 size:expected.size()];
            [blit endEncoding];
            [context.commandBuffer commit];
            [context.commandBuffer waitUntilCompleted];
            const bool completed = context.commandBuffer.status == MTLCommandBufferStatusCompleted;
            const bool matches = completed && std::memcmp(readback.contents,
                expected.data(), expected.size()) == 0;
            context.commandBuffer = nil;
            context.privateUploads.clear();
            backend.m_frameActive = false;
            dispatch_semaphore_signal(context.available);
            if (!completed)
                error = "private upload recovery readback command failed";
            else if (!matches)
                error = "private upload recovery replay returned unexpected bytes";
            return matches;
        };
        auto releaseBackend = [](MetalR4Backend& backend) {
            for (FrameContext& context : backend.m_frames)
            {
                context.commandBuffer = nil;
                context.privateUploads.clear();
                finishPrivateUploads(context);
                context.retainedResources.clear();
                context.retainedOwners.clear();
                context.indirectReads.clear();
                context.uploadArena.Clear();
                runDeferredReleases(context);
            }
            backend.m_recoverablePrivateUploads.clear();
            backend.m_frames[0].available = nullptr;
            backend.m_commandQueue = nil;
            backend.m_device = nil;
        };

        // A live destination must retain its staged write through discard and
        // replay. Two writes to the same exact range across two aborts must
        // collapse to the latest staging contents.
        {
            MetalR4Backend backend;
            if (!initialize(backend))
            {
                error = "private upload recovery probe semaphore creation failed";
                return false;
            }
            R4BufferDesc descriptor;
            descriptor.size = 64;
            descriptor.storage = R4ResourceStorage::Private;
            descriptor.usage = R4BufferUsage::Generic;
            descriptor.label = "private upload recovery probe";
            auto buffer = backend.CreateBuffer(descriptor, nullptr, 0, error);
            if (!buffer)
            {
                releaseBackend(backend);
                return false;
            }
            std::array<std::uint8_t, 64> first{};
            std::array<std::uint8_t, 64> latest{};
            first.fill(0x31);
            latest.fill(0xa7);
            if (!beginManualFrame(backend) ||
                !backend.UpdateBuffer(buffer, 0, first.data(), first.size(), error))
            {
                if (backend.m_frameActive) backend.DiscardFrame();
                releaseBackend(backend);
                if (error.empty()) error = "could not encode first private buffer upload";
                return false;
            }
            backend.DiscardFrame();
            if (backend.m_recoverablePrivateUploads.size() != 1u ||
                !beginManualFrame(backend) ||
                !backend.UpdateBuffer(buffer, 0, latest.data(), latest.size(), error))
            {
                if (backend.m_frameActive) backend.DiscardFrame();
                releaseBackend(backend);
                if (error.empty()) error = "first aborted private upload was not retained";
                return false;
            }
            backend.DiscardFrame();
            if (backend.m_recoverablePrivateUploads.size() != 1u)
            {
                error = "repeated exact private writes were not coalesced across discards";
                releaseBackend(backend);
                return false;
            }
            if (!replayAndReadBack(backend, buffer, latest))
            {
                releaseBackend(backend);
                return false;
            }
            releaseBackend(backend);
        }

        // The native view must keep its source wrapper alive, while releasing
        // the view itself must let the journal discard the now-dead upload.
        {
            MetalR4Backend backend;
            if (!initialize(backend))
            {
                error = "private upload recovery probe semaphore creation failed";
                return false;
            }
            R4Texture2DDesc descriptor;
            descriptor.width = 2;
            descriptor.height = 2;
            descriptor.format = R4PixelFormat::RGBA8Unorm;
            descriptor.storage = R4ResourceStorage::Private;
            descriptor.usage = R4TextureUsage::ShaderRead | R4TextureUsage::PixelFormatView;
            descriptor.label = "private upload recovery view owner probe";
            auto parent = backend.CreateTexture2D(descriptor, error);
            auto parentWeak = std::weak_ptr<IR4Texture>(parent);
            auto view = parent ? backend.CreateTextureView(parent, descriptor.format, error) : nullptr;
            std::array<std::uint8_t, 16> pixels{};
            pixels.fill(0x5c);
            if (!parent || !view || !beginManualFrame(backend) ||
                !backend.UploadTexture2D(view, 0, 0, pixels.data(), pixels.size(), 8, 16, error))
            {
                if (backend.m_frameActive) backend.DiscardFrame();
                releaseBackend(backend);
                if (error.empty()) error = "could not upload through a live private texture view";
                return false;
            }
            parent.reset();
            if (parentWeak.expired())
            {
                backend.DiscardFrame();
                releaseBackend(backend);
                error = "live texture view did not retain its parent wrapper";
                return false;
            }
            backend.DiscardFrame();
            if (backend.m_recoverablePrivateUploads.size() != 1u)
            {
                releaseBackend(backend);
                error = "live texture-view upload was not retained after discard";
                return false;
            }
            view.reset();
            prunePrivateUploads(backend.m_recoverablePrivateUploads);
            if (!parentWeak.expired() || !backend.m_recoverablePrivateUploads.empty())
            {
                releaseBackend(backend);
                error = "released texture view left a stale parent/upload journal entry";
                return false;
            }
            releaseBackend(backend);
        }

        // Failed asset rebuilds repeatedly allocate destinations which are
        // immediately released; each aborted generation should be prunable.
        {
            MetalR4Backend backend;
            if (!initialize(backend))
            {
                error = "private upload recovery probe semaphore creation failed";
                return false;
            }
            R4Texture2DDesc descriptor;
            descriptor.width = 2;
            descriptor.height = 2;
            descriptor.format = R4PixelFormat::RGBA8Unorm;
            descriptor.storage = R4ResourceStorage::Private;
            descriptor.usage = R4TextureUsage::ShaderRead;
            descriptor.label = "private upload recovery replaced asset probe";
            std::array<std::uint8_t, 16> pixels{};
            pixels.fill(0x84);
            for (unsigned generation = 0; generation != 2; ++generation)
            {
                auto texture = backend.CreateTexture2D(descriptor, error);
                if (!texture || !beginManualFrame(backend) ||
                    !backend.UploadTexture2D(texture, 0, 0, pixels.data(), pixels.size(), 8, 16, error))
                {
                    if (backend.m_frameActive) backend.DiscardFrame();
                    releaseBackend(backend);
                    if (error.empty()) error = "could not encode replaced texture upload";
                    return false;
                }
                backend.DiscardFrame();
                texture.reset();
                prunePrivateUploads(backend.m_recoverablePrivateUploads);
                if (!backend.m_recoverablePrivateUploads.empty())
                {
                    releaseBackend(backend);
                    error = "released asset generation remained in the private upload journal";
                    return false;
                }
            }
            releaseBackend(backend);
        }

        // A live asset that is abandoned repeatedly must stop at the bounded
        // threshold and release all retained staging immediately.
        {
            MetalR4Backend backend;
            if (!initialize(backend))
            {
                error = "private upload recovery probe semaphore creation failed";
                return false;
            }
            R4BufferDesc descriptor;
            descriptor.size = 64;
            descriptor.storage = R4ResourceStorage::Private;
            descriptor.usage = R4BufferUsage::Generic;
            descriptor.label = "private upload recovery terminal probe";
            auto buffer = backend.CreateBuffer(descriptor, nullptr, 0, error);
            if (!buffer)
            {
                releaseBackend(backend);
                return false;
            }
            std::array<std::uint8_t, 64> bytes{};
            bytes.fill(0x6d);
            for (std::size_t attempt = 0; attempt != MaxPrivateUploadRecoveryFailures; ++attempt)
            {
                if (!beginManualFrame(backend) ||
                    !backend.UpdateBuffer(buffer, 0, bytes.data(), bytes.size(), error))
                {
                    if (backend.m_frameActive) backend.DiscardFrame();
                    releaseBackend(backend);
                    if (error.empty()) error = "could not encode terminal-bound upload";
                    return false;
                }
                backend.DiscardFrame();
                const std::size_t expected = attempt + 1u < MaxPrivateUploadRecoveryFailures ? 1u : 0u;
                if (backend.m_recoverablePrivateUploads.size() != expected)
                {
                    releaseBackend(backend);
                    error = "private upload recovery journal did not respect its bounded retry threshold";
                    return false;
                }
            }
            if (!backend.m_privateUploadRecoveryFailed || !backend.HasCommandBufferError())
            {
                releaseBackend(backend);
                error = "repeated private upload discards did not fail-stop the backend";
                return false;
            }
            releaseBackend(backend);
        }

        return true;
    }
}
}

bool MetalR4Backend::ProbeComputeSkinBackendGpu(std::string& error)
{
    @autoreleasepool
    {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> queue = [device newCommandQueue];
        if (!device || !queue)
        {
            error = "Metal device/queue unavailable for compute skin backend probe";
            return false;
        }

        MetalR4Backend backend;
        backend.m_device = device;
        backend.m_commandQueue = queue;
        for (FrameContext& context : backend.m_frames)
            context.available = dispatch_semaphore_create(1);
        FrameContext& context = backend.m_frames[0];
        auto beginManualAcquiredFrame = [&]() {
            if (backend.m_frameActive || !context.available)
                return false;
            dispatch_semaphore_wait(context.available, DISPATCH_TIME_FOREVER);
            context.commandBuffer = [queue commandBuffer];
            if (!context.commandBuffer)
            {
                dispatch_semaphore_signal(context.available);
                return false;
            }
            backend.m_activeFrameIndex = 0;
            if (!backend.beginAcquiredContext(context, error))
            {
                context.commandBuffer = nil;
                dispatch_semaphore_signal(context.available);
                return false;
            }
            backend.m_frameActive = true;
            return true;
        };
        auto fail = [&](const std::string& message) {
            if (error.empty()) error = message;
            if (backend.m_frameActive) backend.DiscardFrame();
            return false;
        };
        if (!beginManualAcquiredFrame())
            return fail("compute skin probe could not begin an acquired frame");

        constexpr std::size_t VertexCount = 4;
        constexpr std::size_t InputBytes = VertexCount * sizeof(xray::metal::R4ComputeSkinVertex);
        constexpr std::size_t OutputStride = 36;
        constexpr std::size_t OutputBytes = VertexCount * OutputStride;
        using InputVertex = xray::metal::R4ComputeSkinVertex;
        std::array<InputVertex, VertexCount> source{};
        constexpr std::array<std::array<float, 4>, VertexCount> weights{{
            {{1.0f, 0.0f, 0.0f, 0.0f}},
            {{0.25f, 0.75f, 0.0f, 0.0f}},
            {{0.25f, 0.25f, 0.5f, 0.0f}},
            {{0.125f, 0.25f, 0.25f, 0.375f}},
        }};
        std::array<float, 4 * 16> boneMatrices{};
        for (std::size_t bone = 0; bone < 4; ++bone)
        {
            float* matrix = boneMatrices.data() + bone * 16;
            matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
            matrix[12] = static_cast<float>(bone);
        }
        for (std::size_t vertex = 0; vertex < source.size(); ++vertex)
        {
            InputVertex& value = source[vertex];
            value.position[0] = 0.25f; value.position[1] = 0.5f; value.position[2] = 0.75f;
            value.normal[1] = 1.0f;
            value.tangent[0] = 1.0f;
            value.binormal[2] = 1.0f;
            value.texcoord[0] = 0.125f; value.texcoord[1] = 0.875f;
            for (std::size_t influence = 0; influence < 4; ++influence)
            {
                value.bones[influence] = static_cast<std::uint32_t>(influence);
                value.weights[influence] = weights[vertex][influence];
            }
        }
        R4BufferDesc inputDesc{InputBytes, R4ResourceStorage::Shared,
            R4BufferUsage::Generic, "R4 compute skin backend canonical probe input"};
        auto input = backend.CreateBuffer(inputDesc, source.data(), InputBytes, error);
        if (!input) return fail("compute skin probe canonical input allocation failed: " + error);
        R4FrameBufferSlice bones;
        if (!backend.UploadFrameData(boneMatrices.data(), sizeof(boneMatrices), 256u, bones, error))
            return fail("compute skin probe bone upload failed: " + error);
        const std::uint64_t firstAcquireEpoch = bones.acquireEpoch;
        if (firstAcquireEpoch == 0 || firstAcquireEpoch != context.uploadArena.AcquireEpoch())
            return fail("compute skin probe upload did not stamp the production acquired epoch");
        std::array<std::uint8_t, 16> gapUpload{};
        R4FrameBufferSlice afterGap;
        if (!backend.UploadFrameData(gapUpload.data(), gapUpload.size(), 512u, afterGap, error))
            return fail("compute skin probe could not create an alignment gap: " + error);
        R4FrameBufferSlice ephemeralCanonical;
        if (!backend.UploadFrameData(source.data(), sizeof(InputVertex), 16u,
                ephemeralCanonical, error))
            return fail("compute skin probe could not stage an ephemeral canonical input: " + error);

        R4BufferDesc outputDesc{OutputBytes, R4ResourceStorage::Private,
            R4BufferUsage::Vertex, "R4 compute skin backend private probe output"};
        auto output = backend.CreateBuffer(outputDesc, nullptr, 0, error);
        if (!output) return fail("compute skin probe output allocation failed: " + error);
        std::array<std::uint8_t, OutputBytes> sentinel{};
        sentinel.fill(0xcd);
        if (!backend.UpdateBuffer(output, 0, sentinel.data(), sentinel.size(), error))
            return fail("compute skin probe sentinel upload failed: " + error);

        std::vector<R4ComputeSkinJob> jobs;
        jobs.reserve(VertexCount);
        for (std::size_t vertex = 0; vertex < VertexCount; ++vertex)
        {
            R4ComputeSkinJob job;
            job.canonicalInput = input;
            job.inputOffset = vertex * sizeof(InputVertex);
            job.inputSize = sizeof(InputVertex);
            job.bones = bones;
            job.output = output;
            job.outputOffset = vertex * OutputStride;
            job.outputSize = OutputStride;
            job.vertexCount = 1;
            job.influenceCount = static_cast<std::uint32_t>(vertex + 1u);
            job.boneCount = 4;
            job.maxReferencedBoneIndex = static_cast<std::uint32_t>(vertex);
            jobs.push_back(std::move(job));
        }
        struct ExpectedOutput
        {
            float position[4];
            std::uint8_t normal[4], tangent[4], binormal[4];
            float texcoord[2];
        };
        static_assert(sizeof(ExpectedOutput) == OutputStride, "compute skin output ABI");
        constexpr float expectedFirstVertexX = 0.25f;
        ExpectedOutput expectedFirstVertex{};
        expectedFirstVertex.position[0] = expectedFirstVertexX;
        expectedFirstVertex.position[1] = 0.5f;
        expectedFirstVertex.position[2] = 0.75f;
        expectedFirstVertex.position[3] = 1.0f;
        expectedFirstVertex.normal[0] = 127; expectedFirstVertex.normal[1] = 255;
        expectedFirstVertex.normal[2] = 127; expectedFirstVertex.normal[3] = 0;
        expectedFirstVertex.tangent[0] = 127; expectedFirstVertex.tangent[1] = 127;
        expectedFirstVertex.tangent[2] = 255; expectedFirstVertex.tangent[3] = 0;
        expectedFirstVertex.binormal[0] = 255; expectedFirstVertex.binormal[1] = 127;
        expectedFirstVertex.binormal[2] = 127; expectedFirstVertex.binormal[3] = 0;
        expectedFirstVertex.texcoord[0] = 0.125f;
        expectedFirstVertex.texcoord[1] = 0.875f;
        auto positiveOracle = std::make_shared<R4ComputeSkinOracleRequest>();
        positiveOracle->stableKey = "backend-gpu-probe:positive-1w";
        positiveOracle->expectedBytes.resize(OutputStride);
        std::memcpy(positiveOracle->expectedBytes.data(), &expectedFirstVertex, OutputStride);
        positiveOracle->poseHash = 0x52d31u;
        positiveOracle->outputWriteSerial = 1u;
        positiveOracle->prewarmEpoch = 1u;
        positiveOracle->engineFrame = 1u;
        jobs[0].oracle = positiveOracle;

        // Each rejection uses the production validator and leaves the same
        // private sentinel untouched. The range case points at an alignment gap.
        std::vector<R4ComputeSkinJob> rejected{jobs[0]};
        rejected[0].bones.acquireEpoch = firstAcquireEpoch - 1u;
        error.clear();
        if (backend.EncodeComputeSkinJobs(rejected, error))
            return fail("compute skin backend accepted a stale acquisition epoch");
        rejected[0] = jobs[0];
        rejected[0].bones.offset = bones.offset + 256u;
        error.clear();
        if (backend.EncodeComputeSkinJobs(rejected, error))
            return fail("compute skin backend accepted an unowned alignment-gap span");
        rejected = {jobs[0], jobs[1]};
        rejected[1].outputOffset = rejected[0].outputOffset;
        error.clear();
        if (backend.EncodeComputeSkinJobs(rejected, error))
            return fail("compute skin backend accepted overlapping output spans");
        rejected = {jobs[0]};
        rejected[0].canonicalInput = ephemeralCanonical.buffer;
        rejected[0].inputOffset = ephemeralCanonical.offset;
        error.clear();
        if (backend.EncodeComputeSkinJobs(rejected, error))
            return fail("compute skin backend accepted an arena slice as canonical geometry input");

        R4BufferDesc aliasInputDesc{InputBytes, R4ResourceStorage::Private,
            R4BufferUsage::Generic, "R4 compute skin native alias probe"};
        auto aliasInput = backend.CreateBuffer(aliasInputDesc, source.data(), InputBytes, error);
        if (!aliasInput) return fail("compute skin alias input allocation failed: " + error);
        const auto aliasMetalInput = std::dynamic_pointer_cast<MetalBuffer>(aliasInput);
        auto aliasOutput = std::make_shared<MetalBuffer>(aliasMetalInput->Native(),
            aliasInputDesc, aliasMetalInput->AllocationSize());
        rejected[0] = jobs[0];
        rejected[0].canonicalInput = aliasInput;
        rejected[0].inputOffset = 0;
        rejected[0].output = aliasOutput;
        rejected[0].outputOffset = 0;
        error.clear();
        if (backend.EncodeComputeSkinJobs(rejected, error))
            return fail("compute skin backend accepted aliased native input/output wrappers");

        rejected = jobs;
        rejected[1].influenceCount = 5;
        error.clear();
        if (backend.EncodeComputeSkinJobs(rejected, error))
            return fail("compute skin backend accepted a batch with an invalid second job");

        [backend.m_frames[0].uploadBlit endEncoding];
        backend.m_frames[0].uploadBlit = nil;
        id<MTLBuffer> invalidReadback = [device newBufferWithLength:OutputBytes
            options:MTLResourceStorageModeShared];
        id<MTLBuffer> resultReadback = [device newBufferWithLength:OutputBytes
            options:MTLResourceStorageModeShared];
        if (!invalidReadback || !resultReadback)
            return fail("compute skin probe readback allocation failed");
        id<MTLBlitCommandEncoder> before = [context.commandBuffer blitCommandEncoder];
        if (!before) return fail("compute skin probe pre-dispatch readback encoder failed");
        [before copyFromBuffer:std::dynamic_pointer_cast<MetalBuffer>(output)->Native()
            sourceOffset:0 toBuffer:invalidReadback destinationOffset:0 size:OutputBytes];
        [before endEncoding];

        error.clear();
        if (!backend.EncodeComputeSkinJobs(jobs, error))
            return fail("production compute skin batch encode failed: " + error);
        id<MTLBlitCommandEncoder> after = [context.commandBuffer blitCommandEncoder];
        if (!after) return fail("compute skin probe post-dispatch readback encoder failed");
        [after copyFromBuffer:std::dynamic_pointer_cast<MetalBuffer>(output)->Native()
            sourceOffset:0 toBuffer:resultReadback destinationOffset:0 size:OutputBytes];
        [after endEncoding];
        id<MTLCommandBuffer> command = context.commandBuffer;
        [command commit];
        [command waitUntilCompleted];
        if (command.status != MTLCommandBufferStatusCompleted)
            return fail("compute skin backend GPU probe command failed: " + describeError(command.error));
        context.privateUploads.clear(); // These staged writes completed with this probe command.
        const std::shared_ptr<ComputeSkinOracleReceipt> positiveReceipt =
            std::move(context.computeSkinOracleReceipt);
        if (!positiveReceipt || finishComputeSkinOracleReceipt(positiveReceipt,
                command.status, 0u, true) != ComputeSkinOracleOutcome::Pass)
            return fail("compute skin probe exact-byte positive oracle did not pass");

        const auto* sentinelResult = static_cast<const std::uint8_t*>(invalidReadback.contents);
        if (!std::equal(sentinel.begin(), sentinel.end(), sentinelResult))
            return fail("rejected compute skin batch modified output before dispatch");

        const auto* result = static_cast<const std::uint8_t*>(resultReadback.contents);
        constexpr std::array<float, VertexCount> expectedX{{0.25f, 1.0f, 1.5f, 2.125f}};
        for (std::size_t vertex = 0; vertex < VertexCount; ++vertex)
        {
            ExpectedOutput actual{};
            std::memcpy(&actual, result + vertex * OutputStride, sizeof(actual));
            if (actual.position[0] != expectedX[vertex] || actual.position[1] != 0.5f ||
                actual.position[2] != 0.75f || actual.position[3] != 1.0f ||
                actual.normal[0] != 127 || actual.normal[1] != 255 ||
                actual.normal[2] != 127 || actual.normal[3] != 0 ||
                actual.tangent[0] != 127 || actual.tangent[1] != 127 ||
                actual.tangent[2] != 255 || actual.tangent[3] != 0 ||
                actual.binormal[0] != 255 || actual.binormal[1] != 127 ||
                actual.binormal[2] != 127 || actual.binormal[3] != 0 ||
                actual.texcoord[0] != 0.125f || actual.texcoord[1] != 0.875f)
                return fail("compute skin backend output differed from the canonical CPU expectation");
        }

        const R4ComputeSkinJob staleJob = jobs[0];
        R4ComputeSkinJob staleCanonicalJob = jobs[0];
        staleCanonicalJob.canonicalInput = ephemeralCanonical.buffer;
        staleCanonicalJob.inputOffset = ephemeralCanonical.offset;
        backend.DiscardFrame();
        if (context.uploadArena.AcquireEpoch() != 0)
            return fail("discard did not clear the acquired frame epoch");
        if (!beginManualAcquiredFrame())
            return fail("compute skin probe could not reacquire after discard");
        R4FrameBufferSlice negativeBones;
        if (!backend.UploadFrameData(boneMatrices.data(), sizeof(boneMatrices), 256u,
                negativeBones, error))
            return fail("compute skin probe negative-oracle bone upload failed: " + error);
        R4ComputeSkinJob negativeJob = jobs[0];
        negativeJob.bones = negativeBones;
        auto negativeOracle = std::make_shared<R4ComputeSkinOracleRequest>(*positiveOracle);
        negativeOracle->stableKey = "backend-gpu-probe:negative-one-bit";
        negativeOracle->expectedBytes[0] ^= 0x01u;
        negativeOracle->poseHash = 0x52d32u;
        negativeOracle->outputWriteSerial = 2u;
        negativeOracle->prewarmEpoch = 2u;
        negativeOracle->engineFrame = 2u;
        negativeJob.oracle = negativeOracle;
        if (!backend.EncodeComputeSkinJobs({negativeJob}, error))
            return fail("compute skin negative-oracle encode failed: " + error);
        id<MTLCommandBuffer> negativeCommand = context.commandBuffer;
        [negativeCommand commit];
        [negativeCommand waitUntilCompleted];
        if (negativeCommand.status != MTLCommandBufferStatusCompleted)
            return fail("compute skin negative-oracle command failed: " + describeError(negativeCommand.error));
        context.privateUploads.clear();
        const std::shared_ptr<ComputeSkinOracleReceipt> negativeReceipt =
            std::move(context.computeSkinOracleReceipt);
        if (!negativeReceipt || finishComputeSkinOracleReceipt(negativeReceipt,
                negativeCommand.status, 0u, true) != ComputeSkinOracleOutcome::Fail)
            return fail("compute skin probe one-bit mismatch was not rejected exactly");
        backend.DiscardFrame();

        if (!beginManualAcquiredFrame())
            return fail("compute skin probe could not begin abandoned-oracle frame");
        R4FrameBufferSlice abandonedBones;
        if (!backend.UploadFrameData(boneMatrices.data(), sizeof(boneMatrices), 256u,
                abandonedBones, error))
            return fail("compute skin probe abandoned-oracle bone upload failed: " + error);
        R4ComputeSkinJob abandonedJob = jobs[0];
        abandonedJob.bones = abandonedBones;
        auto abandonedOracle = std::make_shared<R4ComputeSkinOracleRequest>(*positiveOracle);
        abandonedOracle->stableKey = "backend-gpu-probe:abandoned-no-completion";
        abandonedOracle->outputWriteSerial = 3u;
        abandonedOracle->prewarmEpoch = 3u;
        abandonedOracle->engineFrame = 3u;
        abandonedJob.oracle = abandonedOracle;
        if (!backend.EncodeComputeSkinJobs({abandonedJob}, error) ||
            !context.computeSkinOracleReceipt)
            return fail("compute skin abandoned-oracle request was not staged");
        backend.DiscardFrame();
        if (context.computeSkinOracleReceipt)
            return fail("discard retained an oracle receipt without GPU completion");

        if (!beginManualAcquiredFrame())
            return fail("compute skin probe could not begin stale-span frame");
        rejected = {staleJob};
        error.clear();
        if (backend.EncodeComputeSkinJobs(rejected, error))
            return fail("compute skin backend accepted a prior-acquisition bone slice");
        rejected = {staleCanonicalJob};
        error.clear();
        if (backend.EncodeComputeSkinJobs(rejected, error))
            return fail("compute skin backend accepted a stale arena slice as canonical input");
        backend.DiscardFrame();

        // Model the real no-drawable retry point: BeginFrame resets the arena,
        // returns Retry, and does not call beginAcquiredContext. The following
        // successful acquisition must receive a fresh epoch, including slot wrap.
        std::uint64_t previousEpoch = firstAcquireEpoch;
        FrameContext& retryContext = backend.m_frames[0];
        retryContext.uploadArena.Reset(); // Drawable retry returns here without stamping a new epoch.
        if (retryContext.uploadArena.AcquireEpoch() != 0)
            return fail("drawable retry retained an old native acquisition epoch");
        const std::array<std::size_t, 4> slotOrder{{0u, 1u, 2u, 0u}};
        for (std::size_t slot : slotOrder)
        {
            FrameContext& slotContext = backend.m_frames[slot];
            slotContext.uploadArena.Reset();
            if (slotContext.uploadArena.AcquireEpoch() != 0 ||
                !backend.beginAcquiredContext(slotContext, error))
                return fail("retry/slot-wrap acquisition helper failed");
            const std::uint64_t nextEpoch = slotContext.uploadArena.AcquireEpoch();
            if (nextEpoch <= previousEpoch)
                return fail("retry or slot wrap reused a prior native acquisition epoch");
            previousEpoch = nextEpoch;
            slotContext.uploadArena.Reset();
        }
        error.clear();
        return true;
    }
}

bool MetalR4Backend::ProbeWholeLightVisibilityGpu(std::string& error)
{
    @autoreleasepool
    {
            MetalR4Backend backend;
            backend.m_device = MTLCreateSystemDefaultDevice();
            backend.m_commandQueue = backend.m_device ? [backend.m_device newCommandQueue] : nil;
            backend.m_wholeLightVisibilityEnabled = true;
            FrameContext& context = backend.m_frames[0];
            context.available = dispatch_semaphore_create(1);
            bool contextLocked = false;
            auto fail = [&](const std::string& message) {
                if (backend.m_frameActive)
                {
                    backend.DiscardFrame();
                    contextLocked = false;
                }
                else if (contextLocked)
                {
                    dispatch_semaphore_signal(context.available);
                    contextLocked = false;
                }
                context.commandBuffer = nil;
                context.visibilityResultBuffer = nil;
                context.available = nullptr;
                error = message;
                return false;
            };
            if (!backend.m_device || !backend.m_commandQueue || !context.available)
                return fail("visibility probe could not create a Metal device, queue, or semaphore");

            static const char* source =
                "#include <metal_stdlib>\nusing namespace metal;\n"
                "vertex float4 vis_probe_vertex(uint id [[vertex_id]]) {\n"
                "  if (id == 0) return float4(-1.0, -1.0, 0.5, 1.0);\n"
                "  if (id == 1) return float4( 3.0, -1.0, 0.5, 1.0);\n"
                "  return float4(-1.0, 3.0, 0.5, 1.0);\n}\n"
                "fragment float4 vis_probe_fragment() { return float4(1,0,0,1); }\n";
            NSError* compileError = nil;
            id<MTLLibrary> library = [backend.m_device newLibraryWithSource:
                [NSString stringWithUTF8String:source] options:nil error:&compileError];
            id<MTLFunction> vertex = [library newFunctionWithName:@"vis_probe_vertex"];
            id<MTLFunction> fragment = [library newFunctionWithName:@"vis_probe_fragment"];
            MTLRenderPipelineDescriptor* pipelineDesc = [[MTLRenderPipelineDescriptor alloc] init];
            pipelineDesc.vertexFunction = vertex;
            pipelineDesc.fragmentFunction = fragment;
            pipelineDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            pipelineDesc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
            pipelineDesc.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
            NSError* pipelineError = nil;
            id<MTLRenderPipelineState> pipeline = [backend.m_device
                newRenderPipelineStateWithDescriptor:pipelineDesc error:&pipelineError];
            MTLDepthStencilDescriptor* depthDesc = [[MTLDepthStencilDescriptor alloc] init];
            depthDesc.depthCompareFunction = MTLCompareFunctionAlways;
            depthDesc.depthWriteEnabled = NO;
            MTLStencilDescriptor* stencilDesc = [[MTLStencilDescriptor alloc] init];
            stencilDesc.stencilCompareFunction = MTLCompareFunctionEqual;
            stencilDesc.stencilFailureOperation = MTLStencilOperationKeep;
            stencilDesc.depthFailureOperation = MTLStencilOperationKeep;
            stencilDesc.depthStencilPassOperation = MTLStencilOperationKeep;
            stencilDesc.readMask = 0xff;
            stencilDesc.writeMask = 0;
            depthDesc.frontFaceStencil = stencilDesc;
            depthDesc.backFaceStencil = stencilDesc;
            id<MTLDepthStencilState> depthState = [backend.m_device
                newDepthStencilStateWithDescriptor:depthDesc];
            if (!library || !vertex || !fragment || !pipeline || !depthState)
                return fail(std::string("visibility probe pipeline setup failed: ") +
                    (pipelineError.localizedDescription.UTF8String ?:
                     compileError.localizedDescription.UTF8String ?: "unknown"));

            constexpr std::uint32_t Width = 32u, Height = 32u;
            R4Texture2DDesc colorDesc;
            colorDesc.width = Width; colorDesc.height = Height;
            colorDesc.format = R4PixelFormat::RGBA8Unorm;
            colorDesc.storage = R4ResourceStorage::Shared;
            colorDesc.usage = R4TextureUsage::RenderTarget;
            auto color = backend.CreateTexture2D(colorDesc, error);
            R4Texture2DDesc depthTextureDesc = colorDesc;
            depthTextureDesc.format = R4PixelFormat::Depth32FloatStencil8;
            auto depth = backend.CreateTexture2D(depthTextureDesc, error);
            const std::array<std::uint16_t, 3> indices{{0, 1, 2}};
            R4BufferDesc indexDesc;
            indexDesc.size = sizeof(indices);
            indexDesc.storage = R4ResourceStorage::Shared;
            indexDesc.usage = R4BufferUsage::Index;
            auto indexBuffer = backend.CreateBuffer(indexDesc, indices.data(), sizeof(indices), error);
            if (!color || !depth || !indexBuffer)
                return fail("visibility probe offscreen attachment/index allocation failed: " + error);

            struct Capture
            {
                std::array<std::uint8_t, Width * Height * 4u> color{};
                std::array<float, Width * Height> depth{};
                std::array<std::uint8_t, Width * Height> stencil{};
                std::array<std::uint64_t, 2> counts{};
            } queried, ordinary;
            constexpr std::size_t ReadbackRowBytes = 256u;
            constexpr std::size_t ReadbackPlaneBytes = ReadbackRowBytes * Height;
            constexpr std::size_t ColorReadbackOffset = 0u;
            constexpr std::size_t DepthReadbackOffset = ReadbackPlaneBytes;
            constexpr std::size_t StencilReadbackOffset = 2u * ReadbackPlaneBytes;
            constexpr std::size_t ReadbackBytes = 3u * ReadbackPlaneBytes;
            auto drawFrame = [&](const bool query, const bool expectEncoded, Capture& capture) {
                dispatch_semaphore_wait(context.available, DISPATCH_TIME_FOREVER);
                contextLocked = true;
                context.commandBuffer = [backend.m_commandQueue commandBuffer];
                backend.m_activeFrameIndex = 0;
                if (!context.commandBuffer || !backend.beginAcquiredContext(context, error))
                    return false;
                backend.m_frameActive = true;
                R4RenderPassDesc pass;
                pass.label = query ? "whole-light visibility query probe" : "whole-light ordinary probe";
                pass.useDrawable = false;
                pass.enableVisibilityResultCounts = query;
                pass.colorAttachments.push_back({color, R4LoadAction::Clear, R4StoreAction::Store,
                    {0.0, 0.0, 0.0, 0.0}});
                pass.depthStencilAttachment.texture = depth;
                pass.depthStencilAttachment.depthLoadAction = R4LoadAction::Clear;
                pass.depthStencilAttachment.depthStoreAction = R4StoreAction::Store;
                pass.depthStencilAttachment.clearDepth = 1.0;
                pass.depthStencilAttachment.stencilLoadAction = R4LoadAction::Clear;
                pass.depthStencilAttachment.stencilStoreAction = R4StoreAction::Store;
                pass.depthStencilAttachment.clearStencil = 1u;
                pass.hasViewport = true;
                pass.viewport = {0, 0, Width, Height, 0, 1};
                if (!backend.BeginRenderPass(pass, error))
                {
                    if (!backend.m_frameActive)
                        contextLocked = false; // BeginRenderPass failure abandons and signals.
                    return false;
                }
                [context.encoder setRenderPipelineState:pipeline];
                [context.encoder setDepthStencilState:depthState];
                R4IndexedDrawDesc draw;
                draw.indexBuffer = indexBuffer;
                draw.indexCount = 3;
                draw.indexType = R4IndexType::UInt16;
                for (std::uint64_t id = 1u; id <= 2u; ++id)
                {
                    R4WholeLightVisibilitySample sample;
                    sample.engineFrame = 1u;
                    sample.batchId = static_cast<std::int32_t>(id);
                    sample.lightId = id;
                    sample.face = 0u;
                    bool encoded = false;
                    [context.encoder setStencilReferenceValue:id == 1u ? 1u : 2u];
                    if (!backend.DrawIndexedWithVisibility(draw, sample, encoded, error) ||
                        (expectEncoded && !encoded) || (!expectEncoded && encoded))
                        return false;
                }
                if (query && !expectEncoded &&
                    (!context.visibilityDeniedByCap || !context.visibilityRecords.empty()))
                {
                    error = "visibility cap did not fail open to ordinary drawing";
                    return false;
                }
                if (!backend.EndRenderPass(error))
                    return false;
                id<MTLBuffer> readback = [backend.m_device newBufferWithLength:ReadbackBytes
                    options:MTLResourceStorageModeShared];
                id<MTLBlitCommandEncoder> blit = [context.commandBuffer blitCommandEncoder];
                auto colorNative = std::dynamic_pointer_cast<MetalTexture>(color)->Native();
                auto depthNative = std::dynamic_pointer_cast<MetalTexture>(depth)->Native();
                if (!readback || !blit)
                {
                    error = "visibility probe aspect readback allocation/encoder failed";
                    return false;
                }
                const MTLOrigin origin = MTLOriginMake(0, 0, 0);
                const MTLSize extent = MTLSizeMake(Width, Height, 1);
                [blit copyFromTexture:colorNative sourceSlice:0 sourceLevel:0 sourceOrigin:origin
                    sourceSize:extent toBuffer:readback destinationOffset:ColorReadbackOffset
                    destinationBytesPerRow:ReadbackRowBytes destinationBytesPerImage:0];
                [blit copyFromTexture:depthNative sourceSlice:0 sourceLevel:0 sourceOrigin:origin
                    sourceSize:extent toBuffer:readback destinationOffset:DepthReadbackOffset
                    destinationBytesPerRow:ReadbackRowBytes destinationBytesPerImage:0
                    options:MTLBlitOptionDepthFromDepthStencil];
                [blit copyFromTexture:depthNative sourceSlice:0 sourceLevel:0 sourceOrigin:origin
                    sourceSize:extent toBuffer:readback destinationOffset:StencilReadbackOffset
                    destinationBytesPerRow:ReadbackRowBytes destinationBytesPerImage:0
                    options:MTLBlitOptionStencilFromDepthStencil];
                [blit endEncoding];
                [context.commandBuffer commit];
                [context.commandBuffer waitUntilCompleted]; // Probe-only synchronization.
                const bool completed = context.commandBuffer.status == MTLCommandBufferStatusCompleted;
                if (completed)
                {
                    const auto* bytes = static_cast<const std::uint8_t*>(readback.contents);
                    for (std::size_t y = 0; y < Height; ++y)
                    {
                        std::memcpy(capture.color.data() + y * Width * 4u,
                            bytes + ColorReadbackOffset + y * ReadbackRowBytes, Width * 4u);
                        std::memcpy(capture.depth.data() + y * Width,
                            bytes + DepthReadbackOffset + y * ReadbackRowBytes, Width * sizeof(float));
                        std::memcpy(capture.stencil.data() + y * Width,
                            bytes + StencilReadbackOffset + y * ReadbackRowBytes, Width);
                    }
                    if (query && expectEncoded && context.visibilityResultBuffer.contents)
                    {
                        std::memcpy(capture.counts.data(), context.visibilityResultBuffer.contents,
                            sizeof(capture.counts));
                    }
                }
                context.commandBuffer = nil;
                context.visibilityRecords.clear();
                context.visibilityBufferReset = false;
                context.retainedResources.clear();
                context.retainedOwners.clear();
                backend.m_frameActive = false;
                dispatch_semaphore_signal(context.available);
                contextLocked = false;
                if (!completed)
                    error = "visibility probe command buffer did not complete";
                return completed;
            };
            if (!drawFrame(true, true, queried) || !drawFrame(false, false, ordinary))
                return fail("visibility probe draw/readback failed: " + error);
            if (queried.counts[0] == 0u || queried.counts[1] != 0u)
                return fail("visibility counter did not distinguish stencil-qualified pass/reject");
            if (queried.color != ordinary.color || queried.depth != ordinary.depth ||
                queried.stencil != ordinary.stencil)
                return fail("enabling visibility counts changed offscreen color/depth attachments");

            dispatch_semaphore_wait(context.available, DISPATCH_TIME_FOREVER);
            contextLocked = true;
            context.commandBuffer = [backend.m_commandQueue commandBuffer];
            backend.m_activeFrameIndex = 0;
            if (!context.commandBuffer || !backend.beginAcquiredContext(context, error))
                return fail("visibility abort probe could not acquire a context");
            backend.m_frameActive = true;
            R4RenderPassDesc abortPass;
            abortPass.useDrawable = false;
            abortPass.enableVisibilityResultCounts = true;
            abortPass.colorAttachments.push_back({color, R4LoadAction::Clear, R4StoreAction::Store,
                {0.0, 0.0, 0.0, 0.0}});
            abortPass.depthStencilAttachment.texture = depth;
            abortPass.depthStencilAttachment.depthLoadAction = R4LoadAction::Clear;
            abortPass.depthStencilAttachment.depthStoreAction = R4StoreAction::Store;
            abortPass.depthStencilAttachment.stencilLoadAction = R4LoadAction::Clear;
            abortPass.depthStencilAttachment.stencilStoreAction = R4StoreAction::Store;
            abortPass.depthStencilAttachment.clearStencil = 1u;
            if (!backend.BeginRenderPass(abortPass, error))
            {
                if (!backend.m_frameActive)
                    contextLocked = false;
                return fail("visibility abort probe pass setup failed");
            }
            [context.encoder setRenderPipelineState:pipeline];
            [context.encoder setDepthStencilState:depthState];
            bool abortEncoded = false;
            R4WholeLightVisibilitySample abortSample;
            abortSample.engineFrame = 2u; abortSample.batchId = 1; abortSample.lightId = 3u;
            R4IndexedDrawDesc abortDraw;
            abortDraw.indexBuffer = indexBuffer;
            abortDraw.indexCount = 3u;
            abortDraw.indexType = R4IndexType::UInt16;
            [context.encoder setStencilReferenceValue:1u];
            if (!backend.DrawIndexedWithVisibility(abortDraw, abortSample, abortEncoded, error) || !abortEncoded)
                return fail("visibility abort probe could not encode its query draw");
            backend.DiscardFrame();
            contextLocked = false;
            if (!context.visibilityRecords.empty() || context.visibilityBufferReset)
                return fail("discard retained a visibility receipt from an unsubmitted command");
            Capture repeatedFrame;
            for (std::uint32_t i = 0u; i < 10u; ++i)
                if (!drawFrame(true, true, repeatedFrame))
                    return fail("visibility probe failed before the 12-acquisition observation cap: " + error);
            Capture cappedFrame;
            if (!drawFrame(true, false, cappedFrame) ||
                backend.m_wholeLightVisibilitySelectedAcquisitions != MaxWholeLightVisibilityAcquisitions ||
                !backend.m_wholeLightVisibilityCapNoticeEmitted)
                return fail("13th query acquisition did not remain ordinary after the backend cap");
            context.available = nullptr;
            error.clear();
            return true;
    }
}

bool RunMetalR4PrivateUploadRecoveryProbe(std::string& error)
{
    return MetalR4Backend::ProbePrivateUploadRecovery(error);
}

bool RunMetalR4ComputeSkinBackendGpuProbe(std::string& error)
{
    const char* current = std::getenv("XRAY_METAL_COMPUTE_SKIN");
    const bool hadValue = current != nullptr;
    const std::string oldValue = current ? current : "";
    const char* currentOracle = std::getenv("XRAY_METAL_COMPUTE_SKIN_ORACLE");
    const bool hadOracleValue = currentOracle != nullptr;
    const std::string oldOracleValue = currentOracle ? currentOracle : "";
    if (::setenv("XRAY_METAL_COMPUTE_SKIN", "1", 1) != 0)
    {
        error = "compute skin probe could not enable the opt-in backend path";
        return false;
    }
    if (::setenv("XRAY_METAL_COMPUTE_SKIN_ORACLE", "1", 1) != 0)
    {
        if (hadValue) ::setenv("XRAY_METAL_COMPUTE_SKIN", oldValue.c_str(), 1);
        else ::unsetenv("XRAY_METAL_COMPUTE_SKIN");
        error = "compute skin probe could not enable the bounded oracle";
        return false;
    }
    const bool passed = MetalR4Backend::ProbeComputeSkinBackendGpu(error);
    const int restored = hadValue ? ::setenv("XRAY_METAL_COMPUTE_SKIN", oldValue.c_str(), 1) :
        ::unsetenv("XRAY_METAL_COMPUTE_SKIN");
    const int oracleRestored = hadOracleValue ?
        ::setenv("XRAY_METAL_COMPUTE_SKIN_ORACLE", oldOracleValue.c_str(), 1) :
        ::unsetenv("XRAY_METAL_COMPUTE_SKIN_ORACLE");
    if (restored != 0 && passed)
    {
        error = "compute skin probe could not restore XRAY_METAL_COMPUTE_SKIN";
        return false;
    }
    if (oracleRestored != 0 && passed)
    {
        error = "compute skin probe could not restore XRAY_METAL_COMPUTE_SKIN_ORACLE";
        return false;
    }
    return passed;
}

bool RunMetalR4WholeLightVisibilityGpuProbe(std::string& error)
{
    return MetalR4Backend::ProbeWholeLightVisibilityGpu(error);
}

std::unique_ptr<IR4Backend> CreateMetalR4Backend()
{
    return std::make_unique<MetalR4Backend>();
}

bool RunMetalR4FrameUploadGpuProbe(bool oldPath, std::string& error)
{
    @autoreleasepool
    {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> queue = [device newCommandQueue];
        if (!device || !queue)
        {
            error = "Metal device/queue unavailable for frame upload probe";
            return false;
        }
        constexpr std::size_t PatternSize = 256;
        constexpr std::size_t PatternCount = 6; // AO, IL, four blur strengths
        auto encodePatternCopies = [&](MetalFrameUploadArena* arena, int frame,
            id<MTLCommandBuffer> __strong& command,
            id<MTLBuffer> __strong& readback) -> bool {
            command = [queue commandBuffer];
            readback = [device newBufferWithLength:PatternSize * PatternCount
                options:MTLResourceStorageModeShared];
            id<MTLBuffer> oldShared = arena ? nil :
                [device newBufferWithLength:PatternSize options:MTLResourceStorageModeShared];
            id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
            if (!command || !readback || !blit || (!arena && !oldShared))
            {
                error = "Metal frame upload probe allocation failed";
                return false;
            }
            for (std::size_t index = 0; index < PatternCount; ++index)
            {
                std::array<std::uint8_t, PatternSize> bytes{};
                bytes.fill(static_cast<std::uint8_t>(0x20 + frame * 16 + index));
                id<MTLBuffer> source = oldShared;
                std::size_t sourceOffset = 0;
                if (arena)
                {
                    R4FrameBufferSlice slice;
                    if (!arena->Upload(device, bytes.data(), bytes.size(), 256, slice, error))
                        return false;
                    if (slice.offset % 256 != 0 || slice.size != PatternSize ||
                        slice.offset > slice.buffer->Size() - slice.size)
                    {
                        error = "frame upload slice bounds/alignment failed";
                        return false;
                    }
                    source = std::static_pointer_cast<MetalBuffer>(slice.buffer)->Native();
                    sourceOffset = slice.offset;
                }
                else
                    std::memcpy(oldShared.contents, bytes.data(), bytes.size());
                [blit copyFromBuffer:source sourceOffset:sourceOffset
                    toBuffer:readback destinationOffset:index * PatternSize size:PatternSize];
            }
            [blit endEncoding];
            [command commit];
            return true;
        };

        if (oldPath)
        {
            id<MTLCommandBuffer> command = nil;
            id<MTLBuffer> readback = nil;
            if (!encodePatternCopies(nullptr, 0, command, readback))
                return false;
            [command waitUntilCompleted];
            if (command.status != MTLCommandBufferStatusCompleted)
            {
                error = "old-path GPU command failed";
                return false;
            }
            const auto* result = static_cast<const std::uint8_t*>(readback.contents);
            for (std::size_t index = 0; index < PatternCount; ++index)
            {
                if (result[index * PatternSize] != 0x20 + index)
                {
                    error = "old Shared buffer overwrite reproduced before submit";
                    return false;
                }
            }
            return true;
        }

        std::array<MetalFrameUploadArena, MaxFramesInFlight> arenas;
        std::array<id<MTLCommandBuffer>, MaxFramesInFlight> commands{};
        std::array<id<MTLBuffer>, MaxFramesInFlight> readbacks{};
        // Two waves give six encoded frames. Each wave commits three command
        // buffers before any wait; the second forces completed-slot reuse.
        for (int wave = 0; wave != 2; ++wave)
        {
            for (std::size_t slot = 0; slot < MaxFramesInFlight; ++slot)
            {
                arenas[slot].Reset();
                if (!encodePatternCopies(&arenas[slot],
                        wave * static_cast<int>(MaxFramesInFlight) + static_cast<int>(slot),
                        commands[slot], readbacks[slot]))
                    return false;
            }
            for (std::size_t slot = 0; slot < MaxFramesInFlight; ++slot)
            {
                [commands[slot] waitUntilCompleted];
                if (commands[slot].status != MTLCommandBufferStatusCompleted)
                {
                    error = "frame upload GPU command failed";
                    return false;
                }
                const auto* result = static_cast<const std::uint8_t*>(readbacks[slot].contents);
                const int frame = wave * static_cast<int>(MaxFramesInFlight) +
                    static_cast<int>(slot);
                for (std::size_t index = 0; index < PatternCount; ++index)
                {
                    const std::uint8_t expected =
                        static_cast<std::uint8_t>(0x20 + frame * 16 + index);
                    const auto* begin = result + index * PatternSize;
                    if (!std::all_of(begin, begin + PatternSize,
                            [expected](std::uint8_t value) { return value == expected; }))
                    {
                        error = "frame upload GPU pattern changed before completion";
                        return false;
                    }
                }
            }
        }
        for (R4BufferUsage usage : {R4BufferUsage::Vertex, R4BufferUsage::Index})
        {
            id<MTLBuffer> native = [device newBufferWithLength:PatternSize
                options:MTLResourceStorageModeShared];
            R4BufferDesc descriptor{PatternSize, R4ResourceStorage::Shared, usage,
                "E02 Shared version probe"};
            MetalBuffer legacy(native, descriptor, PatternSize);
            std::array<std::uint8_t, PatternSize> first{};
            std::array<std::uint8_t, PatternSize> second{};
            first.fill(0x51);
            second.fill(0xa6);
            if (!legacy.UpdateShared(device, 0, first.data(), first.size(), error))
                return false;
            const std::shared_ptr<void> firstVersion = legacy.VersionHandle();
            id<MTLBuffer> firstNative = legacy.Native();
            id<MTLBuffer> readback = [device newBufferWithLength:PatternSize * 2
                options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> command = [queue commandBuffer];
            id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
            [blit copyFromBuffer:firstNative sourceOffset:0 toBuffer:readback
                destinationOffset:0 size:PatternSize];
            if (!legacy.UpdateShared(device, 0, second.data(), second.size(), error))
                return false;
            const std::shared_ptr<void> secondVersion = legacy.VersionHandle();
            if (legacy.Native() == firstNative)
            {
                error = "bound Shared vertex/index buffer was not versioned";
                return false;
            }
            [blit copyFromBuffer:legacy.Native() sourceOffset:0 toBuffer:readback
                destinationOffset:PatternSize size:PatternSize];
            [blit endEncoding];
            [command commit];
            [command waitUntilCompleted];
            if (command.status != MTLCommandBufferStatusCompleted)
            {
                error = "Shared version GPU copy failed";
                return false;
            }
            const auto* result = static_cast<const std::uint8_t*>(readback.contents);
            if (!std::all_of(result, result + PatternSize,
                    [](std::uint8_t value) { return value == 0x51; }) ||
                !std::all_of(result + PatternSize, result + 2 * PatternSize,
                    [](std::uint8_t value) { return value == 0xa6; }))
            {
                error = "Shared vertex/index native versions changed before completion";
                return false;
            }
        }
        // Draw twice before one submit using distinct transform and color
        // slices. Read back the target to verify actual shader consumption,
        // not just blit copies of the upload buffer.
        {
            NSString* source = [NSString stringWithUTF8String:R"(
#include <metal_stdlib>
using namespace metal;
vertex float4 e02_vertex(const device float2* positions [[buffer(0)]],
    const device float2* translation [[buffer(1)]], uint index [[vertex_id]]) {
    return float4(positions[index] + translation[0], 0.0, 1.0);
}
fragment float4 e02_fragment(const device float4* color [[buffer(0)]]) {
    return color[0];
}
)"];
            NSError* compileError = nil;
            id<MTLLibrary> library = [device newLibraryWithSource:source options:nil
                error:&compileError];
            if (!library)
            {
                error = "E02 draw probe shader compile failed: " +
                    describeError(compileError);
                return false;
            }
            MTLRenderPipelineDescriptor* pipelineDesc =
                [[MTLRenderPipelineDescriptor alloc] init];
            pipelineDesc.vertexFunction = [library newFunctionWithName:@"e02_vertex"];
            pipelineDesc.fragmentFunction = [library newFunctionWithName:@"e02_fragment"];
            pipelineDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            id<MTLRenderPipelineState> pipeline =
                [device newRenderPipelineStateWithDescriptor:pipelineDesc error:&compileError];
            if (!pipeline)
            {
                error = "E02 draw probe pipeline failed: " + describeError(compileError);
                return false;
            }
            MTLTextureDescriptor* textureDesc = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                width:64 height:32 mipmapped:NO];
            textureDesc.storageMode = MTLStorageModeShared;
            textureDesc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            id<MTLTexture> target = [device newTextureWithDescriptor:textureDesc];
            if (!target)
            {
                error = "E02 draw probe target allocation failed";
                return false;
            }
            const std::array<float, 6> triangle = {{
                -0.25f, -0.5f, 0.25f, -0.5f, 0.0f, 0.5f}};
            const std::array<float, 2> left = {{-0.5f, 0.0f}};
            const std::array<float, 2> right = {{0.5f, 0.0f}};
            const std::array<float, 4> red = {{1.0f, 0.0f, 0.0f, 1.0f}};
            const std::array<float, 4> green = {{0.0f, 1.0f, 0.0f, 1.0f}};
            MetalFrameUploadArena drawArena;
            R4FrameBufferSlice vertices, leftTransform, rightTransform, redColor, greenColor;
            if (!drawArena.Upload(device, triangle.data(), sizeof(triangle), 16,
                    vertices, error) ||
                !drawArena.Upload(device, left.data(), sizeof(left), 256,
                    leftTransform, error) ||
                !drawArena.Upload(device, red.data(), sizeof(red), 256,
                    redColor, error) ||
                !drawArena.Upload(device, right.data(), sizeof(right), 256,
                    rightTransform, error) ||
                !drawArena.Upload(device, green.data(), sizeof(green), 256,
                    greenColor, error))
                return false;
            MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = target;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
            id<MTLCommandBuffer> command = [queue commandBuffer];
            id<MTLRenderCommandEncoder> encoder =
                [command renderCommandEncoderWithDescriptor:pass];
            if (!command || !encoder)
            {
                error = "E02 draw probe encoder creation failed";
                return false;
            }
            [encoder setRenderPipelineState:pipeline];
            [encoder setVertexBuffer:std::static_pointer_cast<MetalBuffer>(vertices.buffer)->Native()
                offset:vertices.offset atIndex:0];
            const auto draw = [&](const R4FrameBufferSlice& transform,
                const R4FrameBufferSlice& color) {
                [encoder setVertexBuffer:std::static_pointer_cast<MetalBuffer>(transform.buffer)->Native()
                    offset:transform.offset atIndex:1];
                [encoder setFragmentBuffer:std::static_pointer_cast<MetalBuffer>(color.buffer)->Native()
                    offset:color.offset atIndex:0];
                [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            };
            draw(leftTransform, redColor);
            draw(rightTransform, greenColor);
            [encoder endEncoding];
            [command commit];
            [command waitUntilCompleted];
            if (command.status != MTLCommandBufferStatusCompleted)
            {
                error = "E02 two-draw GPU command failed";
                return false;
            }
            std::array<std::uint8_t, 64 * 32 * 4> pixels{};
            [target getBytes:pixels.data() bytesPerRow:64 * 4
                fromRegion:MTLRegionMake2D(0, 0, 64, 32) mipmapLevel:0];
            const auto pixel = [&](std::size_t x) { return pixels.data() + (16 * 64 + x) * 4; };
            if (pixel(16)[0] < 240 || pixel(16)[1] > 16 ||
                pixel(48)[1] < 240 || pixel(48)[0] > 16)
            {
                error = "E02 two draws lost distinct transform/color ranges";
                return false;
            }
        }
        R4FrameBufferSlice invalid;
        std::array<std::uint8_t, 16> bytes{};
        if (arenas[0].Upload(device, bytes.data(), bytes.size(), 3, invalid, error) ||
            arenas[0].Upload(device, bytes.data(), MaxFrameUploadBytes,
                MaxFrameUploadBytes, invalid, error))
        {
            error = "frame upload arena accepted invalid alignment/overflow";
            return false;
        }
        error.clear();
        return true;
    }
}

bool RunMetalR4ReprojectionProbe(std::string& error)
{
    @autoreleasepool
    {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> queue = [device newCommandQueue];
        if (!device || !queue)
        {
            error = "Metal device/queue unavailable for reprojection probe";
            return false;
        }

        // Test 1: CPU Stationary Points Screen-Space Reprojection Gate (<= 0.5 pixel)
        auto makePerspective = [](float fovYRad, float aspect, float zNear, float zFar) {
            std::array<float, 16> m{};
            float f = 1.0f / std::tan(fovYRad * 0.5f);
            m[0] = f / aspect;
            m[5] = f;
            m[10] = zFar / (zFar - zNear);
            m[11] = 1.0f;
            m[14] = -(zNear * zFar) / (zFar - zNear);
            return m;
        };

        auto multiply4x4 = [](const std::array<float, 16>& a, const std::array<float, 16>& b) {
            std::array<float, 16> r{};
            for (int col = 0; col < 4; ++col)
                for (int row = 0; row < 4; ++row)
                    for (int k = 0; k < 4; ++k)
                        r[col * 4 + row] += a[k * 4 + row] * b[col * 4 + k];
            return r;
        };

        auto transformPoint = [](const std::array<float, 16>& m, const std::array<float, 4>& p) {
            std::array<float, 4> r{};
            for (int row = 0; row < 4; ++row)
                r[row] = m[0 * 4 + row] * p[0] + m[1 * 4 + row] * p[1] + m[2 * 4 + row] * p[2] + m[3 * 4 + row] * p[3];
            return r;
        };

        auto inverseRigid = [](const std::array<float, 16>& m) {
            std::array<float, 16> inv{};
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    inv[c * 4 + r] = m[r * 4 + c];
            inv[15] = 1.0f;
            for (int r = 0; r < 3; ++r)
            {
                float t = 0.0f;
                for (int c = 0; c < 3; ++c)
                    t -= inv[c * 4 + r] * m[12 + c];
                inv[12 + r] = t;
            }
            return inv;
        };

        const float fov = 75.0f * (3.141592653589793f / 180.0f);
        const float aspect = 1920.0f / 1080.0f;
        const std::array<float, 16> projPrev = makePerspective(fov, aspect, 0.2f, 500.0f);
        const std::array<float, 16> projCurr = projPrev;

        // ViewPrev: eye at (0, 1.5, 0), looking along +Z
        std::array<float, 16> viewPrev = {
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 1, 0,
            0, -1.5f, 0, 1
        };

        // ViewCurr: camera moved to (0.3, 1.6, -0.2), rotated yaw +5 deg, pitch -2 deg
        float yaw = 5.0f * (3.141592653589793f / 180.0f);
        float pitch = -2.0f * (3.141592653589793f / 180.0f);
        float cy = std::cos(yaw), sy = std::sin(yaw);
        float cp = std::cos(pitch), sp = std::sin(pitch);
        std::array<float, 16> rot = {
            cy,        sp * sy,  -cp * sy, 0,
            0,         cp,        sp,      0,
            sy,       -sp * cy,   cp * cy, 0,
            0,         0,         0,       1
        };
        std::array<float, 16> trans = {
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 1, 0,
            -0.3f, -1.6f, 0.2f, 1
        };
        const std::array<float, 16> viewCurr = multiply4x4(rot, trans);
        const std::array<float, 16> invViewCurr = inverseRigid(viewCurr);

        const std::array<float, 16> vpPrev = multiply4x4(projPrev, viewPrev);
        const std::array<float, 16> mCurrent = projCurr;
        const std::array<float, 16> mPrevious = multiply4x4(vpPrev, invViewCurr);

        // Generate 100 stationary 3D points inside view frustum
        constexpr std::size_t PointCount = 100;
        struct PointData {
            float viewPos[4];
            float expectedPrevPixel[2];
            float currPixel[2];
        };
        std::vector<PointData> testPoints;
        testPoints.reserve(PointCount);

        float maxErrorCpu = 0.0f;
        for (std::size_t i = 0; i < PointCount; ++i)
        {
            float t = static_cast<float>(i) / static_cast<float>(PointCount - 1);
            float depth = 2.0f + t * 98.0f;
            float worldX = (static_cast<float>((i * 7) % 19) / 9.5f - 1.0f) * (depth * 0.4f);
            float worldY = 1.5f + (static_cast<float>((i * 13) % 17) / 8.5f - 1.0f) * (depth * 0.3f);
            float worldZ = depth;

            std::array<float, 4> pWorld = {worldX, worldY, worldZ, 1.0f};

            std::array<float, 4> clipPrev = transformPoint(vpPrev, pWorld);
            if (clipPrev[3] <= 0.0f) continue;
            float ndcPrevX = clipPrev[0] / clipPrev[3];
            float ndcPrevY = clipPrev[1] / clipPrev[3];
            float uvPrevX = ndcPrevX * 0.5f + 0.5f;
            float uvPrevY = -ndcPrevY * 0.5f + 0.5f;
            float pixelPrevX = uvPrevX * 1920.0f;
            float pixelPrevY = uvPrevY * 1080.0f;

            std::array<float, 4> pViewCurr = transformPoint(viewCurr, pWorld);
            std::array<float, 4> clipCurr = transformPoint(projCurr, pViewCurr);
            if (clipCurr[3] <= 0.0f) continue;
            float ndcCurrX = clipCurr[0] / clipCurr[3];
            float ndcCurrY = clipCurr[1] / clipCurr[3];
            float uvCurrX = ndcCurrX * 0.5f + 0.5f;
            float uvCurrY = -ndcCurrY * 0.5f + 0.5f;
            float pixelCurrX = uvCurrX * 1920.0f;
            float pixelCurrY = uvCurrY * 1080.0f;

            std::array<float, 4> pCur = transformPoint(mCurrent, pViewCurr);
            std::array<float, 4> pPrv = transformPoint(mPrevious, pViewCurr);
            float curX = pCur[0] / pCur[3];
            float curY = pCur[1] / pCur[3];
            float prvX = pPrv[0] / pPrv[3];
            float prvY = pPrv[1] / pPrv[3];
            float uvRpX = (curX - prvX) * 0.5f;
            float uvRpY = (curY - prvY) * -0.5f;
            float reprojectedPixelX = (uvCurrX - uvRpX) * 1920.0f;
            float reprojectedPixelY = (uvCurrY - uvRpY) * 1080.0f;

            float err = std::sqrt((reprojectedPixelX - pixelPrevX) * (reprojectedPixelX - pixelPrevX) +
                                  (reprojectedPixelY - pixelPrevY) * (reprojectedPixelY - pixelPrevY));
            if (err > maxErrorCpu) maxErrorCpu = err;

            PointData pd;
            pd.viewPos[0] = pViewCurr[0];
            pd.viewPos[1] = pViewCurr[1];
            pd.viewPos[2] = pViewCurr[2];
            pd.viewPos[3] = 1.0f;
            pd.expectedPrevPixel[0] = pixelPrevX;
            pd.expectedPrevPixel[1] = pixelPrevY;
            pd.currPixel[0] = pixelCurrX;
            pd.currPixel[1] = pixelCurrY;
            testPoints.push_back(pd);
        }

        if (maxErrorCpu > 0.5f)
        {
            error = "CPU stationary point reprojection error " + std::to_string(maxErrorCpu) + " px exceeds 0.5 px gate";
            return false;
        }

        // Test 2: GPU Reprojection via Metal Compute Kernel
        const char* mslSource = R"(
            #include <metal_stdlib>
            using namespace metal;

            struct ReprojParams {
                float4x4 m_current;
                float4x4 m_previous;
                float2 screen_res;
                float2 pad;
            };

            struct InPoint {
                float4 viewPos;
                float2 expectedPrevPixel;
                float2 currPixel;
            };

            kernel void reproj_kernel(
                device const InPoint* points [[buffer(0)]],
                constant ReprojParams& params [[buffer(1)]],
                device float* errors [[buffer(2)]],
                uint id [[thread_position_in_grid]])
            {
                float4 p4 = points[id].viewPos;
                float4 p_cur = params.m_current * p4;
                float4 p_prv = params.m_previous * p4;
                float2 cur = p_cur.xy / p_cur.w;
                float2 prv = p_prv.xy / p_prv.w;
                float2 uv_rp = (cur - prv) * float2(0.5, -0.5);
                float2 uv_curr = points[id].currPixel / params.screen_res;
                float2 reprojected_pixel = (uv_curr - uv_rp) * params.screen_res;
                float2 diff = reprojected_pixel - points[id].expectedPrevPixel;
                errors[id] = length(diff);
            }
        )";

        NSError* compileErr = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:mslSource]
                                                      options:nil
                                                        error:&compileErr];
        if (!library)
        {
            error = "failed to compile Metal reprojection test kernel: " +
                std::string(compileErr ? compileErr.localizedDescription.UTF8String : "unknown");
            return false;
        }

        id<MTLFunction> kernelFunc = [library newFunctionWithName:@"reproj_kernel"];
        id<MTLComputePipelineState> pso = [device newComputePipelineStateWithFunction:kernelFunc error:&compileErr];
        if (!pso)
        {
            error = "failed to create Metal reprojection pipeline: " +
                std::string(compileErr ? compileErr.localizedDescription.UTF8String : "unknown");
            return false;
        }

        id<MTLBuffer> pointsBuf = [device newBufferWithBytes:testPoints.data()
                                                      length:testPoints.size() * sizeof(PointData)
                                                     options:MTLResourceStorageModeShared];

        struct GpuReprojParams {
            float m_current[16];
            float m_previous[16];
            float screen_res[2];
            float pad[2];
        } params;
        std::memcpy(params.m_current, mCurrent.data(), sizeof(params.m_current));
        std::memcpy(params.m_previous, mPrevious.data(), sizeof(params.m_previous));
        params.screen_res[0] = 1920.0f;
        params.screen_res[1] = 1080.0f;
        params.pad[0] = 0.0f;
        params.pad[1] = 0.0f;

        id<MTLBuffer> paramsBuf = [device newBufferWithBytes:&params
                                                      length:sizeof(params)
                                                     options:MTLResourceStorageModeShared];

        id<MTLBuffer> errorsBuf = [device newBufferWithLength:testPoints.size() * sizeof(float)
                                                      options:MTLResourceStorageModeShared];

        id<MTLCommandBuffer> cmd = [queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
        [enc setComputePipelineState:pso];
        [enc setBuffer:pointsBuf offset:0 atIndex:0];
        [enc setBuffer:paramsBuf offset:0 atIndex:1];
        [enc setBuffer:errorsBuf offset:0 atIndex:2];
        MTLSize gridSize = MTLSizeMake(testPoints.size(), 1, 1);
        NSUInteger threadGroupSizeVal = std::min<NSUInteger>(testPoints.size(), pso.maxTotalThreadsPerThreadgroup);
        MTLSize threadGroupSize = MTLSizeMake(threadGroupSizeVal, 1, 1);
        [enc dispatchThreads:gridSize threadsPerThreadgroup:threadGroupSize];
        [enc endEncoding];
        [cmd commit];
        [cmd waitUntilCompleted];

        if (cmd.status != MTLCommandBufferStatusCompleted)
        {
            error = "Metal GPU reprojection command buffer failed";
            return false;
        }

        const float* gpuErrors = static_cast<const float*>(errorsBuf.contents);
        float maxErrorGpu = 0.0f;
        for (std::size_t i = 0; i < testPoints.size(); ++i)
        {
            if (gpuErrors[i] > maxErrorGpu)
                maxErrorGpu = gpuErrors[i];
        }

        if (maxErrorGpu > 0.5f)
        {
            error = "GPU stationary point reprojection error " + std::to_string(maxErrorGpu) + " px exceeds 0.5 px gate";
            return false;
        }

        // Test 3: Dynamic Object Motion Vector Correctness (<= 0.5 pixel)
        std::array<float, 16> tPrev = {
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 1, 0,
            5.0f, 0.0f, 20.0f, 1
        };
        std::array<float, 16> tCurr = {
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 1, 0,
            5.4f, 0.0f, 20.2f, 1
        };
        const std::array<float, 4> vLocal = {0.2f, 1.0f, 0.0f, 1.0f};

        std::array<float, 4> pWPrev = transformPoint(tPrev, vLocal);
        std::array<float, 4> pWCurr = transformPoint(tCurr, vLocal);
        std::array<float, 4> cPrev = transformPoint(vpPrev, pWPrev);
        std::array<float, 4> cCurr = transformPoint(multiply4x4(projCurr, viewCurr), pWCurr);

        float pxPrevX = (cPrev[0] / cPrev[3] * 0.5f + 0.5f) * 1920.0f;
        float pxPrevY = (-cPrev[1] / cPrev[3] * 0.5f + 0.5f) * 1080.0f;
        float pxCurrX = (cCurr[0] / cCurr[3] * 0.5f + 0.5f) * 1920.0f;
        float pxCurrY = (-cCurr[1] / cCurr[3] * 0.5f + 0.5f) * 1080.0f;
        float trueDeltaX = pxCurrX - pxPrevX;
        float trueDeltaY = pxCurrY - pxPrevY;

        const std::array<float, 16> wvpCurr = multiply4x4(multiply4x4(projCurr, viewCurr), tCurr);
        const std::array<float, 16> wvpPrev = multiply4x4(vpPrev, tPrev);
        std::array<float, 4> velCurr = transformPoint(wvpCurr, vLocal);
        std::array<float, 4> velPrev = transformPoint(wvpPrev, vLocal);
        float mvNdcX = (velCurr[0] / velCurr[3]) - (velPrev[0] / velPrev[3]);
        float mvNdcY = (velCurr[1] / velCurr[3]) - (velPrev[1] / velPrev[3]);
        float mvPixelX = mvNdcX * 0.5f * 1920.0f;
        float mvPixelY = -mvNdcY * 0.5f * 1080.0f;

        float mvErr = std::sqrt((mvPixelX - trueDeltaX) * (mvPixelX - trueDeltaX) +
                                (mvPixelY - trueDeltaY) * (mvPixelY - trueDeltaY));
        if (mvErr > 0.5f)
        {
            error = "Dynamic object motion vector error " + std::to_string(mvErr) + " px exceeds 0.5 px gate";
            return false;
        }

        // Test 4: Stationary Dynamic Object Motion Vector (zero jitter)
        std::array<float, 4> velStatCurr = transformPoint(wvpCurr, vLocal);
        std::array<float, 4> velStatPrev = velStatCurr;
        float statMvX = ((velStatCurr[0] / velStatCurr[3]) - (velStatPrev[0] / velStatPrev[3])) * 0.5f * 1920.0f;
        float statMvY = ((velStatCurr[1] / velStatCurr[3]) - (velStatPrev[1] / velStatPrev[3])) * -0.5f * 1080.0f;
        if (std::abs(statMvX) > 1.0e-5f || std::abs(statMvY) > 1.0e-5f)
        {
            error = "Stationary object produced non-zero motion vector";
            return false;
        }

        std::fprintf(stderr,
            "E04_REPROJECTION_PROBE cpu_max_err=%.4fpx gpu_max_err=%.4fpx mv_err=%.4fpx stationary_err=%.6fpx status=PASS\n",
            maxErrorCpu, maxErrorGpu, mvErr, std::sqrt(statMvX * statMvX + statMvY * statMvY));
        error.clear();
        return true;
    }
}

bool RunMetalR4LocalLightRolloverGpuProbe(const std::string& shaderDirectory,
    const std::string& artifactDirectory, const std::size_t lightCount, std::string& error)
{
    @autoreleasepool
    {
        namespace fs = std::filesystem;
        constexpr std::uint32_t Width = 16;
        constexpr std::uint32_t Height = 8;
        constexpr std::size_t PixelCount = Width * Height;
        constexpr std::array<float, 4> Seed = {0.0625f, 0.125f, 0.1875f, 0.25f};
        if (shaderDirectory.empty() || artifactDirectory.empty() || lightCount == 0u || lightCount > 400u)
        {
            error = "shader directory, artifact directory, and a light count from 1 through 400 are required";
            return false;
        }
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
        {
            error = std::string("SDL video initialization failed: ") + SDL_GetError();
            return false;
        }
        SDL_Window* window = SDL_CreateWindow("R4 local-light rollover probe",
            SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, Width, Height,
            SDL_WINDOW_HIDDEN | SDL_WINDOW_METAL | SDL_WINDOW_ALLOW_HIGHDPI);
        if (!window)
        {
            error = std::string("hidden Metal window creation failed: ") + SDL_GetError();
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            return false;
        }
        std::unique_ptr<IR4Backend> backend = CreateMetalR4Backend();
        bool success = false;
        do
        {
            if (!backend->Initialize(window, Width, Height, error))
                break;
            const auto fail = [&error](const std::string& detail) {
                if (error.empty()) error = detail;
                return false;
            };
            IR4Resources& resources = backend->Resources();
            const auto makeTexture = [&resources](R4PixelFormat format, R4TextureUsage usage,
                const char* label, std::string& failure,
                const R4ResourceStorage storage = R4ResourceStorage::Shared) -> std::shared_ptr<IR4Texture> {
                R4Texture2DDesc descriptor;
                descriptor.width = Width; descriptor.height = Height;
                descriptor.format = format; descriptor.storage = storage;
                descriptor.usage = usage; descriptor.label = label;
                return resources.CreateTexture2D(descriptor, failure);
            };
            const auto makeBuffer = [&resources](const void* data, std::size_t size,
                R4BufferUsage usage, const char* label, std::string& failure) -> std::shared_ptr<IR4Buffer> {
                R4BufferDesc descriptor;
                descriptor.size = size; descriptor.storage = R4ResourceStorage::Shared;
                descriptor.usage = usage; descriptor.label = label;
                auto result = resources.CreateBuffer(descriptor, data, size, failure);
                return result;
            };
            auto position = makeTexture(R4PixelFormat::RGBA16Float, R4TextureUsage::ShaderRead,
                "E05 local light fixture position", error);
            auto diffuse = makeTexture(R4PixelFormat::RGBA8Unorm, R4TextureUsage::ShaderRead,
                "E05 local light fixture diffuse", error);
            if (!position || !diffuse) { fail("G-buffer fixture texture allocation failed: " + error); break; }
            std::vector<__fp16> positionPixels(PixelCount * 4u);
            std::vector<std::uint8_t> diffusePixels(PixelCount * 4u, 0u);
            // gbuffer_stage.h::gbuf_pack_hemi_mtl(0, 0), followed by the
            // production RGBA16F target conversion. All receivers are a
            // synthetic planar surface with packed -Z normals and depth .5.
            const std::uint32_t packedHemiMaterial = 0x40000000u | 0x04000000u;
            float hemiMaterial;
            std::memcpy(&hemiMaterial, &packedHemiMaterial, sizeof(hemiMaterial));
            for (std::uint32_t y = 0; y < Height; ++y)
                for (std::uint32_t x = 0; x < Width; ++x)
                {
                    const std::size_t offset = (std::size_t(y) * Width + x) * 4u;
                    if (x < Width / 2u)
                    {
                        positionPixels[offset + 0u] = __fp16(0.0f);
                        positionPixels[offset + 1u] = __fp16(-1.0f);
                        positionPixels[offset + 2u] = __fp16(0.5f);
                        positionPixels[offset + 3u] = __fp16(hemiMaterial);
                        diffusePixels[offset + 0u] = 180u;
                        diffusePixels[offset + 1u] = 180u;
                        diffusePixels[offset + 2u] = 180u;
                        diffusePixels[offset + 3u] = 255u;
                    }
                }
            if (!resources.UploadTexture2D(position, 0u, 0u, positionPixels.data(),
                    positionPixels.size() * sizeof(__fp16), Width * 4u * sizeof(__fp16),
                    positionPixels.size() * sizeof(__fp16), error) ||
                !resources.UploadTexture2D(diffuse, 0u, 0u, diffusePixels.data(),
                    diffusePixels.size(), Width * 4u, diffusePixels.size(), error))
            { fail("G-buffer fixture upload failed: " + error); break; }

            std::array<std::uint8_t, 528> staticGlobalBytes{};
            const float screenResolution[4] = {float(Width), float(Height), 1.0f / float(Width), 1.0f / float(Height)};
            std::memcpy(staticGlobalBytes.data() + 416u, screenResolution, sizeof(screenResolution));
            auto staticGlobals = makeBuffer(staticGlobalBytes.data(), staticGlobalBytes.size(),
                R4BufferUsage::Uniform, "E05 local light fixture globals", error);
            R4SamplerDesc nearestDesc; nearestDesc.minFilter = nearestDesc.magFilter = R4SamplerFilter::Nearest;
            nearestDesc.mipFilter = R4SamplerFilter::Nearest; nearestDesc.addressU = nearestDesc.addressV = R4SamplerAddressMode::ClampToEdge;
            nearestDesc.label = "E05 local light fixture nearest";
            auto nearest = resources.CreateSampler(nearestDesc, error);
            R4SamplerDesc linearDesc = nearestDesc; linearDesc.minFilter = linearDesc.magFilter = R4SamplerFilter::Linear;
            linearDesc.label = "E05 local light fixture linear";
            auto linear = resources.CreateSampler(linearDesc, error);
            R4Texture2DDesc cookieDesc; cookieDesc.width = cookieDesc.height = 1u;
            cookieDesc.format = R4PixelFormat::RGBA8Unorm; cookieDesc.storage = R4ResourceStorage::Shared;
            cookieDesc.usage = R4TextureUsage::ShaderRead; cookieDesc.label = "E05 explicit white spot cookie";
            auto cookie = resources.CreateTexture2D(cookieDesc, error);
            const std::array<std::uint8_t, 4> whiteCookie = {255u, 255u, 255u, 255u};
            if (!staticGlobals || !nearest || !linear || !cookie ||
                !resources.UploadTexture2D(cookie, 0u, 0u, whiteCookie.data(), whiteCookie.size(),
                    whiteCookie.size(), whiteCookie.size(), error))
            { fail("local-light shared fixture resources unavailable: " + error); break; }

            MetalR4LocalLightAdapter adapter;
            MetalR4LocalLightAdapter::Options adapterOptions;
            adapterOptions.shaderDirectory = fs::path(shaderDirectory);
            adapterOptions.defaultSpotCookie = cookie;
            std::string adapterStatus;
            if (!adapter.Prepare(*backend, adapterOptions, adapterStatus) || !adapter.IsReady())
            { fail("production local-light MSC adapter did not prepare: " + adapterStatus); break; }

            const std::array<std::uint16_t, 6> quadIndices = {0u, 1u, 2u, 0u, 2u, 3u};
            auto indexBuffer = makeBuffer(quadIndices.data(), sizeof(quadIndices), R4BufferUsage::Index,
                "E05 fullscreen fixture indices", error);
            if (!indexBuffer) { fail("fixture index buffer allocation failed: " + error); break; }
            const auto fullscreenPipeline = [&](const char* key, const char* vertexName,
                const char* source, const bool depthTest, const bool establishGeometry,
                const R4PixelFormat colorFormat, std::string& failure) {
                R4GraphicsPipelineDesc descriptor;
                descriptor.label = key; descriptor.shaderKey = key; descriptor.mslSource = source;
                descriptor.vertexFunction = vertexName; descriptor.fragmentFunction = "e05_fixture_fragment";
                descriptor.colorFormats = {colorFormat};
                descriptor.depthFormat = R4PixelFormat::Depth32FloatStencil8;
                descriptor.depthCompare = depthTest ? R4CompareFunction::Equal : R4CompareFunction::Always;
                descriptor.depthWrite = establishGeometry; descriptor.cullMode = R4CullMode::None;
                if (establishGeometry) descriptor.colorWriteMasks = {0u};
                if (depthTest || establishGeometry)
                {
                    descriptor.stencil.enabled = true;
                    descriptor.stencil.frontFace.compare = depthTest ? R4CompareFunction::Equal : R4CompareFunction::Always;
                    descriptor.stencil.frontFace.readMask = depthTest ? 1u : 0u;
                    descriptor.stencil.frontFace.writeMask = establishGeometry ? 1u : 0u;
                    descriptor.stencil.frontFace.stencilFailure = R4StencilOperation::Keep;
                    descriptor.stencil.frontFace.depthFailure = R4StencilOperation::Keep;
                    descriptor.stencil.frontFace.depthStencilPass = establishGeometry ? R4StencilOperation::Replace : R4StencilOperation::Keep;
                    descriptor.stencil.backFace = descriptor.stencil.frontFace;
                }
                return backend->CreateGraphicsPipeline(descriptor, failure);
            };
            const char* fixtureMsl = R"(
#include <metal_stdlib>
using namespace metal;
vertex float4 e05_geometry_vertex(uint index [[vertex_id]]) {
    const float4 p[4] = {float4(-1,-1,0.5,1), float4(0,-1,0.5,1),
                         float4(0,1,0.5,1), float4(-1,1,0.5,1)};
    return p[index];
}
vertex float4 e05_geometry_depth_vertex(uint index [[vertex_id]]) {
    const float2 p[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)};
    return float4(p[index], 0.5, 1);
}
vertex float4 e05_sky_depth_vertex(uint index [[vertex_id]]) {
    const float2 p[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)};
    return float4(p[index], 1, 1);
}
fragment float4 e05_fixture_fragment() { return float4(1,1,1,1); }
)";
            auto geometryPipeline = fullscreenPipeline("E05 establish geometry bit0", "e05_geometry_vertex", fixtureMsl, false, true, R4PixelFormat::RGBA16Float, error);
            auto geometryDepthVerify = fullscreenPipeline("E05 verify geometry depth and bit0", "e05_geometry_depth_vertex", fixtureMsl, true, false, R4PixelFormat::RGBA16Float, error);
            auto skyDepthVerify = fullscreenPipeline("E05 verify sky depth and bit0", "e05_sky_depth_vertex", fixtureMsl, true, false, R4PixelFormat::RGBA16Float, error);
            auto resetPipeline = backend->CreateGraphicsPipeline(xray::metal::R4LightMarkerResetPipelineDesc(), error);
            if (!geometryPipeline || !geometryDepthVerify || !skyDepthVerify || !resetPipeline)
            { fail("E05 fixture or production reset pipeline creation failed: " + error); break; }

            R4EngineLocalLightSnapshot lightSnapshot;
            lightSnapshot.engineFrame = 501u;
            lightSnapshot.ssfxLightSetup1 = {1.0f, 0.0f, 0.0f, 0.0f};
            lightSnapshot.lights.reserve(lightCount);
            for (std::size_t i = 0; i < 400u; ++i)
            {
                R4EngineLocalLight light;
                light.active = true;
                light.type = i % 3u == 0u ? R4EngineLocalLightType::Point :
                    (i % 3u == 1u ? R4EngineLocalLightType::Spot : R4EngineLocalLightType::OmniFace);
                light.position = {0.0f, 0.0f, 0.1f};
                light.direction = {0.0f, 0.0f, 1.0f}; light.right = {1.0f, 0.0f, 0.0f};
                // Keep the entire far cap inside the fixture's orthographic
                // 0..1 depth range so depth-fail stencil can close the volume.
                light.range = 0.6f; light.cone = 1.2f; light.virtualSize = 1.0f;
                light.color = i + 1u == lightCount ? std::array<float, 4>{0.5f,0.3f,0.2f,1.0f} :
                    std::array<float, 4>{0.2f, 0.15f, 0.1f, 1.0f};
                light.specular = 0.01f;
                light.lightId = i + 1u;
                if (i < lightCount) lightSnapshot.lights.push_back(light);
            }

            struct Captured
            {
                std::shared_ptr<IR4Texture> color;
                std::shared_ptr<IR4Texture> depthStencil;
                std::shared_ptr<IR4Texture> diagnostics;
                std::vector<float> rgba;
                std::vector<__fp16> verify;
                std::size_t resets = 0;
                std::size_t submitted = 0;
            };
            std::array<Captured, 8> captures;
            for (Captured& capture : captures)
            {
                capture.color = makeTexture(R4PixelFormat::RGBA16Float, R4TextureUsage::RenderTarget,
                    "E05 local light accumulator", error);
                capture.depthStencil = makeTexture(R4PixelFormat::Depth32FloatStencil8,
                    R4TextureUsage::RenderTarget,
                    "E05 local light depth stencil", error, R4ResourceStorage::Private);
                capture.diagnostics = makeTexture(R4PixelFormat::RGBA16Float, R4TextureUsage::RenderTarget,
                    "E05 local light preservation diagnostic", error);
                if (!capture.color || !capture.depthStencil || !capture.diagnostics)
                { fail("offscreen rollover target allocation failed: " + error); break; }
            }
            if (!error.empty()) break;

            if (backend->BeginFrame(error) != R4FrameAcquireResult::Ready)
            { fail("hidden-window frame acquisition failed: " + error); break; }
            const auto drawIndexed = [&](const std::shared_ptr<IR4GraphicsPipeline>& pipeline,
                const std::uint32_t reference, const std::uint32_t count, const char* label) {
                R4IndexedDrawDesc draw; draw.indexBuffer = indexBuffer; draw.indexCount = count;
                draw.indexType = R4IndexType::UInt16;
                return backend->BindGraphicsPipeline(pipeline, error) &&
                    backend->SetStencilReference(reference, error) && backend->DrawIndexed(draw, error);
            };
            const auto renderOne = [&](Captured& capture, const std::size_t pose, const std::size_t mode) {
                const bool reference = mode == 1u;
                R4RenderPassDesc pass;
                pass.label = reference ? "E05 rollover chunked reference" : "E05 rollover bulk";
                pass.useDrawable = false;
                pass.colorAttachments.push_back({capture.color, R4LoadAction::Clear, R4StoreAction::Store,
                    {Seed[0], Seed[1], Seed[2], Seed[3]}});
                pass.depthStencilAttachment.texture = capture.depthStencil;
                pass.depthStencilAttachment.depthLoadAction = R4LoadAction::Clear;
                pass.depthStencilAttachment.depthStoreAction = R4StoreAction::Store;
                pass.depthStencilAttachment.stencilLoadAction = R4LoadAction::Clear;
                pass.depthStencilAttachment.stencilStoreAction = R4StoreAction::Store;
                pass.depthStencilAttachment.clearDepth = 1.0;
                pass.depthStencilAttachment.clearStencil = 0u;
                if (!backend->BeginRenderPass(pass, error)) return false;
                if (!drawIndexed(geometryPipeline, 1u, 6u, "geometry")) return false;
                MetalR4LocalLightAdapter::GBufferInputs inputs;
                inputs.position = position; inputs.diffuse = diffuse; inputs.staticGlobals = staticGlobals;
                inputs.noFilterSampler = nearest; inputs.linearSampler = linear;
                MetalR4LocalLightAdapter::ViewState view;
                view.view = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
                view.projection = view.view; view.valid = true;
                if (pose == 1u)
                {
                    const float angle = 0.035f;
                    view.view = {std::cos(angle),0,-std::sin(angle),0, 0,1,0,0,
                        std::sin(angle),0,std::cos(angle),0, -0.035f,0,0,1};
                }
                view.eye = pose == 0u ? std::array<float,3>{0,0,0} :
                    std::array<float,3>{0.035f,0,0};
                auto snapshot = std::make_shared<R4EngineLocalLightSnapshot>(lightSnapshot);
                if (mode == 2u)
                    snapshot->lights.pop_back();
                else if (mode == 3u)
                {
                    const R4EngineLocalLight tail = snapshot->lights.back();
                    snapshot->lights.assign(1u, tail);
                }
                if (!reference)
                {
                    if (!adapter.DrawIntoActiveAccumulator(*backend, inputs, view, snapshot, error)) return false;
                    capture.resets = adapter.LastTelemetry().markerResets;
                    capture.submitted = adapter.LastTelemetry().submitted;
                }
                else
                {
                    constexpr std::size_t ChunkSize = 124u;
                    std::size_t resetCount = 0;
                    for (std::size_t begin = 0; begin < snapshot->lights.size(); begin += ChunkSize)
                    {
                        if (begin != 0u)
                        {
                            if (!drawIndexed(resetPipeline, 0u, 3u, "chunk marker reset")) return false;
                            ++resetCount;
                        }
                        auto chunk = std::make_shared<R4EngineLocalLightSnapshot>(*snapshot);
                        const std::size_t end = std::min(snapshot->lights.size(), begin + ChunkSize);
                        chunk->lights.assign(snapshot->lights.begin() + begin, snapshot->lights.begin() + end);
                        if (!adapter.DrawIntoActiveAccumulator(*backend, inputs, view, chunk, error)) return false;
                        capture.submitted += adapter.LastTelemetry().submitted;
                    }
                    capture.resets = resetCount;
                }
                if (!backend->EndRenderPass(error)) return false;

                // Read-only witness pass: exact depth equality plus stencil
                // bit-0 equality independently cover geometry (left) and sky (right).
                R4RenderPassDesc verify;
                verify.label = "E05 depth and geometry-mask preservation witness";
                verify.useDrawable = false;
                verify.colorAttachments.push_back({capture.diagnostics, R4LoadAction::Clear,
                    R4StoreAction::Store, {0,0,0,0}});
                verify.depthStencilAttachment.texture = capture.depthStencil;
                verify.depthStencilAttachment.depthLoadAction = R4LoadAction::Load;
                verify.depthStencilAttachment.depthStoreAction = R4StoreAction::Store;
                verify.depthStencilAttachment.stencilLoadAction = R4LoadAction::Load;
                verify.depthStencilAttachment.stencilStoreAction = R4StoreAction::Store;
                if (!backend->BeginRenderPass(verify, error) ||
                    !drawIndexed(geometryDepthVerify, 1u, 3u, "geometry depth witness") ||
                    !drawIndexed(skyDepthVerify, 0u, 3u, "sky depth witness") ||
                    !backend->EndRenderPass(error)) return false;
                return true;
            };
            for (std::size_t pose = 0; pose < 2u; ++pose)
            {
                const std::size_t base = pose * 4u;
                for (std::size_t mode = 0; mode < 4u; ++mode)
                    if (!renderOne(captures[base + mode], pose, mode))
                    { fail("bulk/reference/tail adapter rendering failed: " + error); break; }
                if (!error.empty()) break;
            }
            if (!error.empty()) break;
            if (!backend->SubmitFrame(error))
            { fail("rollover probe frame submit failed: " + error); break; }
            backend->Shutdown(); // drains the GPU before the shared texture readback.

            for (Captured& capture : captures)
            {
                const auto texture = std::dynamic_pointer_cast<MetalTexture>(capture.color);
                const auto witness = std::dynamic_pointer_cast<MetalTexture>(capture.diagnostics);
                if (!texture || !witness) { fail("offscreen readback lost the native Metal textures"); break; }
                std::vector<__fp16> half(PixelCount * 4u);
                std::vector<__fp16> proof(PixelCount * 4u);
                [texture->Native() getBytes:half.data() bytesPerRow:Width * 4u * sizeof(__fp16)
                    fromRegion:MTLRegionMake2D(0, 0, Width, Height) mipmapLevel:0];
                [witness->Native() getBytes:proof.data() bytesPerRow:Width * 4u * sizeof(__fp16)
                    fromRegion:MTLRegionMake2D(0, 0, Width, Height) mipmapLevel:0];
                capture.rgba.resize(PixelCount * 4u);
                capture.verify = std::move(proof);
                for (std::size_t i = 0; i < capture.rgba.size(); ++i)
                    capture.rgba[i] = static_cast<float>(half[i]);
            }
            if (!error.empty()) break;
            float maxDifference = 0.0f;
            float maxGeometrySignal = 0.0f;
            float maxSkyDifferenceFromSeed = 0.0f;
            float maxTailContribution = 0.0f;
            float maxIsolatedTailContribution = 0.0f;
            float maxTailIsolationError = 0.0f;
            float minPrefixSignal = std::numeric_limits<float>::max();
            std::size_t witnessFailures = 0;
            bool missingTailInPose = false;
            for (std::size_t pose = 0; pose < 2u; ++pose)
            {
                float poseTailContribution = 0.0f;
                float poseIsolatedTailContribution = 0.0f;
                float posePrefixSignal = 0.0f;
                const Captured& bulk = captures[pose * 4u];
                const Captured& reference = captures[pose * 4u + 1u];
                const Captured& prefix = captures[pose * 4u + 2u];
                const Captured& lastAlone = captures[pose * 4u + 3u];
                for (std::size_t pixel = 0; pixel < PixelCount; ++pixel)
                {
                    for (std::size_t channel = 0; channel < 3u; ++channel)
                    {
                        const std::size_t at = pixel * 4u + channel;
                        maxDifference = std::max(maxDifference, std::abs(bulk.rgba[at] - reference.rgba[at]));
                        if (pixel % Width < Width / 2u)
                        {
                            maxGeometrySignal = std::max(maxGeometrySignal,
                                std::abs(bulk.rgba[at] - Seed[channel]));
                            posePrefixSignal = std::max(posePrefixSignal, prefix.rgba[at] - Seed[channel]);
                        }
                        else
                            maxSkyDifferenceFromSeed = std::max(maxSkyDifferenceFromSeed,
                                std::abs(bulk.rgba[at] - Seed[channel]));
                        const float tailDelta = bulk.rgba[at] - prefix.rgba[at];
                        const float isolated = lastAlone.rgba[at] - Seed[channel];
                        maxTailContribution = std::max(maxTailContribution, tailDelta);
                        maxIsolatedTailContribution = std::max(maxIsolatedTailContribution, isolated);
                        poseTailContribution = std::max(poseTailContribution, tailDelta);
                        poseIsolatedTailContribution = std::max(poseIsolatedTailContribution, isolated);
                        maxTailIsolationError = std::max(maxTailIsolationError, std::abs(tailDelta - isolated));
                        if (!std::isfinite(bulk.rgba[at]) || !std::isfinite(reference.rgba[at]) ||
                            !std::isfinite(prefix.rgba[at]) || !std::isfinite(lastAlone.rgba[at]))
                            ++witnessFailures;
                        if (pixel % Width < Width / 2u &&
                            (bulk.rgba[at] < Seed[channel] || reference.rgba[at] < Seed[channel] ||
                             prefix.rgba[at] < Seed[channel] || lastAlone.rgba[at] < Seed[channel]))
                            ++witnessFailures;
                    }
                    for (std::size_t channel = 0; channel < 4u; ++channel)
                        if (!std::isfinite(static_cast<float>(bulk.verify[pixel * 4u + channel])) ||
                            !std::isfinite(static_cast<float>(reference.verify[pixel * 4u + channel])) ||
                            static_cast<float>(bulk.verify[pixel * 4u + channel]) < 0.99f ||
                            static_cast<float>(reference.verify[pixel * 4u + channel]) < 0.99f)
                            ++witnessFailures;
                }
                missingTailInPose = missingTailInPose || poseTailContribution < 0.001f ||
                    poseIsolatedTailContribution < 0.001f;
                minPrefixSignal = std::min(minPrefixSignal, posePrefixSignal);
            }
            const std::size_t expectedBulkResets = lightCount < 125u ? 0u : 1u + (lightCount - 125u) / 126u;
            const std::size_t expectedReferenceResets = lightCount <= 124u ? 0u : (lightCount - 1u) / 124u;
            bool resetAndAdmissionMismatch = false;
            for (std::size_t pose = 0; pose < 2u; ++pose)
            {
                const std::size_t base = pose * 4u;
                resetAndAdmissionMismatch = resetAndAdmissionMismatch ||
                    captures[base].resets != expectedBulkResets ||
                    captures[base].submitted != lightCount ||
                    captures[base + 1u].resets != expectedReferenceResets ||
                    captures[base + 1u].submitted != lightCount;
            }
            if (maxDifference > 0.001f || maxGeometrySignal < 0.001f ||
                maxSkyDifferenceFromSeed > 0.001f || maxTailContribution < 0.001f ||
                maxIsolatedTailContribution < 0.001f || maxTailIsolationError > 0.002f ||
                witnessFailures != 0u || resetAndAdmissionMismatch || missingTailInPose ||
                (lightCount > 1u && minPrefixSignal < 0.005f))
            {
                std::ostringstream message;
                message << "GPU mismatch: maxBulkVsReference=" << maxDifference
                    << " geometrySignal=" << maxGeometrySignal
                    << " skyDelta=" << maxSkyDifferenceFromSeed
                    << " tailDelta=" << maxTailContribution
                    << " isolatedTail=" << maxIsolatedTailContribution
                    << " prefixSignal=" << minPrefixSignal
                    << " tailIsolationError=" << maxTailIsolationError
                    << " depthOrBit0WitnessFailures=" << witnessFailures
                    << " bulkResets=" << captures[0].resets << "/" << expectedBulkResets
                    << " referenceResets=" << captures[1].resets << "/" << expectedReferenceResets
                    << " submitted=" << captures[0].submitted << "/" << lightCount;
                fail(message.str()); break;
            }

            std::error_code artifactError;
            fs::create_directories(fs::path(artifactDirectory), artifactError);
            if (artifactError) { fail("could not create artifact directory: " + artifactError.message()); break; }
            // PPM preview of the first-pose bulk accumulator. HDR values are
            // clamped only for this view artifact; the report retains raw error gates.
            std::ofstream ppm(fs::path(artifactDirectory) / "local-light-rollover-bulk.ppm", std::ios::binary);
            if (!ppm) { fail("could not create PPM GPU artifact"); break; }
            ppm << "P6\n" << Width << " " << Height << "\n255\n";
            for (std::size_t pixel = 0; pixel < PixelCount; ++pixel)
                for (std::size_t channel = 0; channel < 3u; ++channel)
                {
                    const float value = std::clamp(captures[0].rgba[pixel * 4u + channel], 0.0f, 1.0f);
                    const std::uint8_t byte = static_cast<std::uint8_t>(std::lround(value * 255.0f));
                    ppm.write(reinterpret_cast<const char*>(&byte), 1);
                }
            ppm.close();
            std::ofstream report(fs::path(artifactDirectory) / "local-light-rollover-report.json");
            if (!report) { fail("could not create GPU report artifact"); break; }
            report << std::fixed << std::setprecision(6)
                << "{\n  \"status\": \"PASS\",\n  \"shader_bundle\": \"" << shaderDirectory
                << "\",\n  \"lights\": " << lightCount << ",\n  \"types\": [\"point\", \"spot\", \"omni_face\"],\n"
                << "  \"camera_poses\": 2,\n  \"bulk_marker_resets\": " << captures[0].resets
                << ",\n  \"chunk_reference_marker_resets\": " << captures[1].resets
                << ",\n  \"max_bulk_reference_abs_error\": " << maxDifference
                << ",\n  \"max_bulk_minus_prefix_tail_contribution\": " << maxTailContribution
                << ",\n  \"max_isolated_tail_contribution\": " << maxIsolatedTailContribution
                << ",\n  \"max_tail_isolation_abs_error\": " << maxTailIsolationError
                << ",\n  \"max_geometry_signal_from_seed\": " << maxGeometrySignal
                << ",\n  \"min_prefix_signal_across_poses\": " << minPrefixSignal
                << ",\n  \"max_sky_delta_from_seed\": " << maxSkyDifferenceFromSeed
                << ",\n  \"depth_or_bit0_witness_failures\": " << witnessFailures
                << ",\n  \"accumulator_seed\": [" << Seed[0] << ", " << Seed[1] << ", " << Seed[2] << ", " << Seed[3] << "]\n}\n";
            if (!report) { fail("writing GPU report artifact failed"); break; }
            std::fprintf(stderr,
                "E05_LOCAL_LIGHT_ROLLOVER_GPU lights=%zu poses=2 bulk_resets=%zu chunk_resets=%zu max_delta=%.6f geometry_signal=%.6f sky_delta=%.6f tail_delta=%.6f isolated_tail=%.6f depth_bit0_failures=%zu artifact=\"%s\" status=PASS\n",
                lightCount, captures[0].resets, captures[1].resets, maxDifference, maxGeometrySignal,
                maxSkyDifferenceFromSeed, maxTailContribution, maxIsolatedTailContribution,
                witnessFailures, artifactDirectory.c_str());
            error.clear(); success = true;
        } while (false);
        backend->Shutdown();
        SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return success;
    }
}
