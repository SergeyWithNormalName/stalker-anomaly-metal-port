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
#include "MetalR4UiVideo.h"
#include "MetalUIOverlayCommon.h"
#include "R4DdsTexture.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <dispatch/dispatch.h>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdio.h>
#include <unordered_map>
#include <utility>
#include <vector>

extern void Msg(const char* format, ...);

namespace
{
constexpr std::size_t MaxFramesInFlight = 3;
constexpr std::size_t DefaultTlabBlockSize = 16 * 1024 * 1024; // 16 MB per frame
constexpr std::size_t TlabAlignment = 256;

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
    MetalBuffer(id<MTLBuffer> buffer, const R4BufferDesc& descriptor, std::size_t allocationSize)
        : m_buffer(buffer), m_descriptor(descriptor), m_allocationSize(allocationSize)
    {
        m_descriptor.label = nullptr;
    }

    std::size_t Size() const override { return m_descriptor.size; }
    std::size_t AllocationSize() const override { return m_allocationSize; }
    R4ResourceStorage Storage() const override { return m_descriptor.storage; }
    id<MTLBuffer> Native() const { return m_buffer; }

private:
    id<MTLBuffer> m_buffer;
    R4BufferDesc m_descriptor;
    std::size_t m_allocationSize;
};

class MetalTexture final : public IR4Texture
{
public:
    MetalTexture(id<MTLTexture> texture, const R4Texture2DDesc& descriptor)
        : m_texture(texture), m_descriptor(descriptor)
    {
        m_descriptor.label = nullptr;
    }

    const R4Texture2DDesc& Descriptor() const override { return m_descriptor; }
    id<MTLTexture> Native() const { return m_texture; }

private:
    id<MTLTexture> m_texture;
    R4Texture2DDesc m_descriptor;
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

    bool Initialize(SDL_Window* window, const std::uint32_t preferredWidth,
        const std::uint32_t preferredHeight, std::string& error) override
    {
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
        m_nextFrameIndex = 0;
        m_activeFrameIndex = 0;
        m_frameActive = false;
        m_nextSerial = 1;
        m_lastSubmittedSerial.store(0, std::memory_order_relaxed);
        m_completedSerial.store(0, std::memory_order_relaxed);
        m_hadCommandBufferError.store(false, std::memory_order_relaxed);
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
        m_layer.device = m_device;
        m_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        m_layer.framebufferOnly = YES;
        m_layer.maximumDrawableCount = MaxFramesInFlight;
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
            }
#endif
        }

        ResizeDrawable();
        return true;
    }

    void ResizeDrawable() override
    {
        if (!m_window || !m_layer || m_frameActive)
            return;

        int backingWidth = 0;
        int backingHeight = 0;
        int logicalWidth = 0;
        int logicalHeight = 0;
        SDL_Metal_GetDrawableSize(m_window, &backingWidth, &backingHeight);
        SDL_GetWindowSize(m_window, &logicalWidth, &logicalHeight);
        const char* nativeRetinaValue = std::getenv("XRAY_METAL_NATIVE_RETINA");
        const bool nativeRetina = nativeRetinaValue && std::strcmp(nativeRetinaValue, "1") == 0;
        const int width = nativeRetina ? backingWidth :
            (m_preferredDrawableWidth != 0u ? static_cast<int>(m_preferredDrawableWidth) : logicalWidth);
        const int height = nativeRetina ? backingHeight :
            (m_preferredDrawableHeight != 0u ? static_cast<int>(m_preferredDrawableHeight) : logicalHeight);
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
            }
        }
    }

    R4FrameAcquireResult BeginFrame(std::string& error) override
    {
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

        FrameContext& context = m_frames[m_nextFrameIndex];
        dispatch_semaphore_wait(context.available, DISPATCH_TIME_FOREVER);

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
        context.retainedResources.clear();
        context.activeTlabBlockIndex = 0;
        context.activeTlabBlockOffset = 0;
        ResizeDrawable();

        context.drawable = [m_layer nextDrawable];
        if (!context.drawable)
        {
            dispatch_semaphore_signal(context.available);
            return R4FrameAcquireResult::Retry;
        }

        context.commandBuffer = [m_commandQueue commandBuffer];
        if (!context.commandBuffer)
        {
            context.drawable = nil;
            error = "Metal command buffer creation failed";
            dispatch_semaphore_signal(context.available);
            return R4FrameAcquireResult::Error;
        }
        if (!ensurePresentationResources(context, error))
        {
            context.commandBuffer = nil;
            context.drawable = nil;
            dispatch_semaphore_signal(context.available);
            return R4FrameAcquireResult::Error;
        }

        m_activeFrameIndex = m_nextFrameIndex;
        m_frameActive = true;
        return R4FrameAcquireResult::Ready;
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
        if (context.encoder)
        {
            error = "BeginRenderPass called while another render pass is active";
            return false;
        }

        MTLRenderPassDescriptor* descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
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
                context.retainedResources.emplace_back(attachment.texture);
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
            context.retainedResources.emplace_back(pass.depthStencilAttachment.texture);
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
        if (!context.encoder)
        {
            error = "EndRenderPass called without an active render pass";
            return false;
        }

        [context.encoder popDebugGroup];
        [context.encoder endEncoding];
        context.encoder = nil;
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
        context.retainedResources.emplace_back(source);
        context.retainedResources.emplace_back(destination);
        return true;
    }

    bool SubmitFrame(std::string& error) override
    {
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
        if (context.encoder)
        {
            error = "SubmitFrame called while a render pass is active";
            return false;
        }
        if (!encodePresentation(context, error))
        {
            abandonActiveFrame();
            return false;
        }

        const std::uint64_t serial = m_nextSerial++;
        context.completionSerial = serial;
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
        [context.commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> commandBuffer) {
            @autoreleasepool
            {
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
        [context.commandBuffer commit];
        context.drawable = nil;

        m_lastSubmittedSerial.store(serial, std::memory_order_release);
        m_frameActive = false;
        m_nextFrameIndex = (m_activeFrameIndex + 1) % MaxFramesInFlight;
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
        context->retainedResources.emplace_back(pipeline);
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
        [context->encoder setVertexBuffer:metalBuffer->Native() offset:offset atIndex:index];
        context->retainedResources.emplace_back(buffer);
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
        context->retainedResources.emplace_back(texture);
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
        context->retainedResources.emplace_back(sampler);
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
        if (!context || bindings.tlabSize == 0 || bindings.tlabSize % EntrySize != 0)
        {
            if (error.empty())
                error = "invalid Metal Shader Converter TLAB size";
            return false;
        }

        const std::size_t entryCount = bindings.tlabSize / EntrySize;
        std::vector<bool> occupied(entryCount, false);
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
            if (occupied[entryIndex])
            {
                error = "Metal Shader Converter TLAB entries overlap";
                return false;
            }
            occupied[entryIndex] = true;

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
                [context->encoder useResource:buffer->Native()
                                         usage:MTLResourceUsageRead
                                        stages:stages];
                context->retainedResources.emplace_back(binding.buffer);
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
                [context->encoder useResource:texture->Native()
                                         usage:MTLResourceUsageRead
                                        stages:stages];
                context->retainedResources.emplace_back(binding.texture);
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
                context->retainedResources.emplace_back(binding.sampler);
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
        context->activeTlabBlockOffset =
            (context->activeTlabBlockOffset + TlabAlignment - 1) & ~(TlabAlignment - 1);

        if (context->tlabBlocks.empty() ||
            context->activeTlabBlockOffset + bindings.tlabSize >
                context->tlabBlocks[context->activeTlabBlockIndex].capacity)
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
                const std::shared_ptr<MetalBuffer> buffer =
                    std::dynamic_pointer_cast<MetalBuffer>(binding.buffer);
                const std::size_t size = binding.bufferSize == 0 ?
                    buffer->Size() - binding.bufferOffset : binding.bufferSize;
                IRDescriptorTableSetBuffer(
                    entry, buffer->Native().gpuAddress + binding.bufferOffset, size);
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

        [context->encoder useResource:activeBlock.buffer usage:MTLResourceUsageRead stages:stages];
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
        context->retainedResources.emplace_back(descriptor.indexBuffer);
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
        renderContext->retainedResources.emplace_back(vertexBuffer);
        renderContext->retainedResources.emplace_back(indexBuffer);

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
            renderContext->retainedResources.emplace_back(resolved.texture);
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

        if (metalBuffer->Storage() == R4ResourceStorage::Shared)
        {
            std::memcpy(static_cast<std::uint8_t*>(metalBuffer->Native().contents) + offset,
                data, dataSize);
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
        id<MTLBlitCommandEncoder> blit = [context->commandBuffer blitCommandEncoder];
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
        [blit endEncoding];
        context->deferredReleases.emplace_back([staging] { (void)staging; });
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
        return std::make_shared<MetalTexture>(view, descriptor);
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
        id<MTLBlitCommandEncoder> blit = [context->commandBuffer blitCommandEncoder];
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
        [blit endEncoding];
        context->deferredReleases.emplace_back([staging] { (void)staging; });
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
            return;

        if (m_frameActive)
        {
            FrameContext& context = m_frames[m_activeFrameIndex];
            if (context.encoder)
            {
                [context.encoder endEncoding];
                context.encoder = nil;
            }
            std::string ignored;
            SubmitFrame(ignored);
        }

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
            context.retainedResources.clear();
            // Balance the drain wait before ARC releases the dispatch object;
            // libdispatch traps if a semaphore is destroyed while consumed.
            dispatch_semaphore_signal(context.available);
            context.available = nullptr;
        }

        m_pipelineCache.clear();
        std::size_t totalTlabCapacity = 0;
        std::size_t totalTlabBlocks = 0;
        for (FrameContext& context : m_frames)
        {
            totalTlabBlocks += context.tlabBlocks.size();
            for (const auto& block : context.tlabBlocks)
                totalTlabCapacity += block.capacity;
            context.tlabBlocks.clear();
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
        m_uiTextures.clear();
        m_uiVideos.clear();
        m_uiWhiteTexture.reset();
        m_uiOverlaySampler.reset();
        m_uiDdsProvider = {};
        m_uiDdsProviderReady = false;
        m_uiOverlayPipeline = nil;
        m_presentPipeline = nil;
        m_thumbnailPipeline = nil;
        {
            std::lock_guard<std::mutex> lock(m_thumbnailMutex);
            m_latestThumbnail = {};
        }

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
        return m_hadCommandBufferError.load(std::memory_order_acquire);
    }

    bool SupportsBCTextureCompression() const override
    {
        return m_device && m_device.supportsBCTextureCompression;
    }

    std::uint32_t FrameContextCount() const override
    {
        return static_cast<std::uint32_t>(MaxFramesInFlight);
    }

    std::uint32_t ActiveFrameContextIndex() const override
    {
        return static_cast<std::uint32_t>(m_activeFrameIndex);
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

    struct FrameContext
    {
        dispatch_semaphore_t available = nullptr;
        id<CAMetalDrawable> drawable = nil;
        id<MTLTexture> finalColor = nil;
        id<MTLBuffer> thumbnailBuffer = nil;
        id<MTLCommandBuffer> commandBuffer = nil;
        id<MTLRenderCommandEncoder> encoder = nil;
        bool mscPipelineBound = false;
        std::uint64_t completionSerial = 0;
        std::uint64_t drawableGeneration = 0;
        std::vector<std::function<void()>> deferredReleases;
        std::vector<std::shared_ptr<void>> retainedResources;
        std::vector<TlabBlock> tlabBlocks;
        std::size_t activeTlabBlockIndex = 0;
        std::size_t activeTlabBlockOffset = 0;
    };

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
        if (!context.drawable || !context.drawable.texture || !ensurePresentationPipelines(error))
        {
            if (error.empty())
                error = "Metal presentation resources require a drawable";
            return false;
        }
        const NSUInteger width = context.drawable.texture.width;
        const NSUInteger height = context.drawable.texture.height;
        if (width == 0u || height == 0u)
        {
            error = "Metal presentation drawable has zero extent";
            return false;
        }
        if (!context.finalColor || context.finalColor.width != width ||
            context.finalColor.height != height ||
            context.finalColor.pixelFormat != context.drawable.texture.pixelFormat)
        {
            MTLTextureDescriptor* descriptor =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:context.drawable.texture.pixelFormat
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

    void abandonActiveFrame()
    {
        FrameContext& context = m_frames[m_activeFrameIndex];
        context.encoder = nil;
        context.commandBuffer = nil;
        context.drawable = nil;
        runDeferredReleases(context);
        context.retainedResources.clear();
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
    id<MTLRenderPipelineState> m_presentPipeline = nil;
    id<MTLComputePipelineState> m_thumbnailPipeline = nil;
    std::array<FrameContext, MaxFramesInFlight> m_frames;
    std::size_t m_nextFrameIndex = 0;
    std::size_t m_activeFrameIndex = 0;
    bool m_frameActive = false;
    std::uint64_t m_nextSerial = 1;
    std::atomic<std::uint64_t> m_lastSubmittedSerial{0};
    std::atomic<std::uint64_t> m_completedSerial{0};
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
};
}

std::unique_ptr<IR4Backend> CreateMetalR4Backend()
{
    return std::make_unique<MetalR4Backend>();
}
