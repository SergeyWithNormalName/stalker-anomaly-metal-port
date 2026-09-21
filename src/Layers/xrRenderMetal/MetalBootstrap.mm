#import <Cocoa/Cocoa.h>

#include <CommonCrypto/CommonDigest.h>

#include <SDL.h>

#include "IR4Backend.h"
#include "IR4Resources.h"
#include "R4DdsTexture.h"
#include "R4LevelGeometry.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <errno.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unordered_map>
#include <vector>

namespace
{
const char* pixelFormatName(R4PixelFormat format)
{
    switch (format)
    {
    case R4PixelFormat::BC1Rgba: return "BC1_RGBA_UNORM";
    case R4PixelFormat::BC1RgbaSrgb: return "BC1_RGBA_SRGB";
    case R4PixelFormat::BC2Rgba: return "BC2_RGBA_UNORM";
    case R4PixelFormat::BC2RgbaSrgb: return "BC2_RGBA_SRGB";
    case R4PixelFormat::BC3Rgba: return "BC3_RGBA_UNORM";
    case R4PixelFormat::BC3RgbaSrgb: return "BC3_RGBA_SRGB";
    case R4PixelFormat::BC7RgbaUnorm: return "BC7_RGBA_UNORM";
    case R4PixelFormat::BC7RgbaUnormSrgb: return "BC7_RGBA_SRGB";
    case R4PixelFormat::RGBA8Unorm: return "RGBA8_UNORM";
    case R4PixelFormat::BGRA8Unorm: return "BGRA8_UNORM";
    default: return "OTHER";
    }
}

struct Vec3
{
    float x;
    float y;
    float z;
};

struct Vec4
{
    float x;
    float y;
    float z;
    float w;
};

Vec3 operator-(const Vec3& left, const Vec3& right)
{
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

float dot(const Vec3& left, const Vec3& right)
{
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

Vec3 cross(const Vec3& left, const Vec3& right)
{
    return {
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x,
    };
}

Vec3 normalize(const Vec3& value)
{
    const float length = std::sqrt(dot(value, value));
    return {value.x / length, value.y / length, value.z / length};
}

struct alignas(16) Matrix4
{
    float elements[16];
};

Matrix4 multiply(const Matrix4& left, const Matrix4& right)
{
    Matrix4 result{};
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            for (int k = 0; k < 4; ++k)
                result.elements[column * 4 + row] +=
                    left.elements[k * 4 + row] * right.elements[column * 4 + k];
    return result;
}

bool finiteMatrix(const Matrix4& matrix)
{
    for (const float value : matrix.elements)
    {
        if (!std::isfinite(value))
            return false;
    }
    return true;
}

Vec4 transformPointToClip(const Matrix4& matrix, const Vec3& point)
{
    return {
        point.x * matrix.elements[0] + point.y * matrix.elements[4] +
            point.z * matrix.elements[8] + matrix.elements[12],
        point.x * matrix.elements[1] + point.y * matrix.elements[5] +
            point.z * matrix.elements[9] + matrix.elements[13],
        point.x * matrix.elements[2] + point.y * matrix.elements[6] +
            point.z * matrix.elements[10] + matrix.elements[14],
        point.x * matrix.elements[3] + point.y * matrix.elements[7] +
            point.z * matrix.elements[11] + matrix.elements[15],
    };
}

// This deliberately only rejects an AABB when all eight corners are outside
// the same Metal clip plane (x/y in [-w,w], z in [0,w]). Any malformed input,
// invalid camera, or non-finite clip coordinate fails open.
bool conservativelyOutsideMetalFrustum(const xray::metal::R4PositionBounds& bounds,
    const Matrix4& modelTransform, const Matrix4& viewProjection, const bool liveView)
{
    if (!liveView || !bounds.available || !finiteMatrix(modelTransform) || !finiteMatrix(viewProjection) ||
        !std::isfinite(bounds.minimum[0]) || !std::isfinite(bounds.minimum[1]) ||
        !std::isfinite(bounds.minimum[2]) || !std::isfinite(bounds.maximum[0]) ||
        !std::isfinite(bounds.maximum[1]) || !std::isfinite(bounds.maximum[2]) ||
        bounds.minimum[0] > bounds.maximum[0] || bounds.minimum[1] > bounds.maximum[1] ||
        bounds.minimum[2] > bounds.maximum[2])
        return false;

    const Matrix4 worldViewProjection = multiply(viewProjection, modelTransform);
    bool outsideLeft = true;
    bool outsideRight = true;
    bool outsideBottom = true;
    bool outsideTop = true;
    bool outsideNear = true;
    bool outsideFar = true;
    for (const float x : {bounds.minimum[0], bounds.maximum[0]})
    {
        for (const float y : {bounds.minimum[1], bounds.maximum[1]})
        {
            for (const float z : {bounds.minimum[2], bounds.maximum[2]})
            {
                const Vec4 clip = transformPointToClip(worldViewProjection, {x, y, z});
                if (!std::isfinite(clip.x) || !std::isfinite(clip.y) ||
                    !std::isfinite(clip.z) || !std::isfinite(clip.w))
                    return false;
                outsideLeft = outsideLeft && clip.x < -clip.w;
                outsideRight = outsideRight && clip.x > clip.w;
                outsideBottom = outsideBottom && clip.y < -clip.w;
                outsideTop = outsideTop && clip.y > clip.w;
                outsideNear = outsideNear && clip.z < 0.0f;
                outsideFar = outsideFar && clip.z > clip.w;
            }
        }
    }
    return outsideLeft || outsideRight || outsideBottom || outsideTop || outsideNear || outsideFar;
}

Vec3 transformPoint(const Matrix4& matrix, const Vec3& point)
{
    return {
        point.x * matrix.elements[0] + point.y * matrix.elements[4] +
            point.z * matrix.elements[8] + matrix.elements[12],
        point.x * matrix.elements[1] + point.y * matrix.elements[5] +
            point.z * matrix.elements[9] + matrix.elements[13],
        point.x * matrix.elements[2] + point.y * matrix.elements[6] +
            point.z * matrix.elements[10] + matrix.elements[14],
    };
}

Vec3 transformDirection(const Matrix4& matrix, const Vec3& direction)
{
    return {
        direction.x * matrix.elements[0] + direction.y * matrix.elements[4] +
            direction.z * matrix.elements[8],
        direction.x * matrix.elements[1] + direction.y * matrix.elements[5] +
            direction.z * matrix.elements[9],
        direction.x * matrix.elements[2] + direction.y * matrix.elements[6] +
            direction.z * matrix.elements[10],
    };
}

Matrix4 makeLookAt(const Vec3& eye, const Vec3& target)
{
    const Vec3 backward = normalize(eye - target);
    const Vec3 right = normalize(cross({0.0f, 1.0f, 0.0f}, backward));
    const Vec3 up = cross(backward, right);
    return {{
        right.x, up.x, backward.x, 0.0f,
        right.y, up.y, backward.y, 0.0f,
        right.z, up.z, backward.z, 0.0f,
        -dot(right, eye), -dot(up, eye), -dot(backward, eye), 1.0f,
    }};
}

Matrix4 inverseRigidTransform(const Matrix4& matrix)
{
    Matrix4 result{{
        matrix.elements[0], matrix.elements[4], matrix.elements[8], 0.0f,
        matrix.elements[1], matrix.elements[5], matrix.elements[9], 0.0f,
        matrix.elements[2], matrix.elements[6], matrix.elements[10], 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    }};
    const Vec3 translation{
        matrix.elements[12], matrix.elements[13], matrix.elements[14]};
    result.elements[12] = -(
        result.elements[0] * translation.x + result.elements[4] * translation.y +
        result.elements[8] * translation.z);
    result.elements[13] = -(
        result.elements[1] * translation.x + result.elements[5] * translation.y +
        result.elements[9] * translation.z);
    result.elements[14] = -(
        result.elements[2] * translation.x + result.elements[6] * translation.y +
        result.elements[10] * translation.z);
    return result;
}

Matrix4 makePerspective(float verticalFovRadians, float aspect, float nearPlane, float farPlane)
{
    const float scale = 1.0f / std::tan(verticalFovRadians * 0.5f);
    Matrix4 result{};
    result.elements[0] = scale / aspect;
    result.elements[5] = scale;
    result.elements[10] = farPlane / (nearPlane - farPlane);
    result.elements[11] = -1.0f;
    result.elements[14] = (farPlane * nearPlane) / (nearPlane - farPlane);
    return result;
}

Matrix4 makeOrthographic(float halfWidth, float halfHeight, float nearPlane, float farPlane)
{
    Matrix4 result{};
    result.elements[0] = 1.0f / halfWidth;
    result.elements[5] = 1.0f / halfHeight;
    result.elements[10] = 1.0f / (nearPlane - farPlane);
    result.elements[14] = nearPlane / (nearPlane - farPlane);
    result.elements[15] = 1.0f;
    return result;
}

// Quantize the light-space origin to the shadow-map texel grid.  Directional
// light orientation stays continuous, while camera translation cannot slide
// the projected caster/receiver pair between texels every frame.
void stabilizeOrthographicShadowView(Matrix4& view, float halfWidth, float halfHeight,
    std::uint32_t resolution)
{
    if (resolution == 0u || !std::isfinite(halfWidth) || !std::isfinite(halfHeight) ||
        halfWidth <= 0.0f || halfHeight <= 0.0f)
        return;
    const float texelX = (2.0f * halfWidth) / static_cast<float>(resolution);
    const float texelY = (2.0f * halfHeight) / static_cast<float>(resolution);
    view.elements[12] = std::round(view.elements[12] / texelX) * texelX;
    view.elements[13] = std::round(view.elements[13] / texelY) * texelY;
}

struct alignas(16) LevelUniforms
{
    Matrix4 viewProjection;
    float minimumHeight;
    float inverseHeightRange;
    float padding[2];
};

constexpr const char* LevelDiagnosticMsl = R"metal(
#include <metal_stdlib>
using namespace metal;

struct LevelVertexIn
{
    float3 position [[attribute(0)]];
};

struct LevelVertexOut
{
    float4 position [[position]];
    float height;
};

struct LevelUniforms
{
    float4x4 viewProjection;
    float minimumHeight;
    float inverseHeightRange;
    float2 padding;
};

vertex LevelVertexOut r4_level_flat_vs(
    LevelVertexIn input [[stage_in]],
    constant LevelUniforms& uniforms [[buffer(1)]])
{
    LevelVertexOut output;
    output.position = uniforms.viewProjection * float4(input.position, 1.0);
    output.height = saturate((input.position.y - uniforms.minimumHeight) * uniforms.inverseHeightRange);
    return output;
}

fragment float4 r4_level_flat_ps(LevelVertexOut input [[stage_in]])
{
    const float3 low = float3(0.16, 0.20, 0.12);
    const float3 high = float3(0.58, 0.62, 0.45);
    return float4(mix(low, high, input.height), 1.0);
}
)metal";

constexpr const char* LevelTextureDiagnosticMsl = R"metal(
#include <metal_stdlib>
using namespace metal;

struct LevelVertexIn
{
    float3 position [[attribute(0)]];
    short2 baseTexcoord [[attribute(1)]];
};

struct LevelVertexInShort4
{
    float3 position [[attribute(0)]];
    short4 baseTexcoord [[attribute(1)]];
};

struct LevelVertexInHemi
{
    float3 position [[attribute(0)]];
    short2 baseTexcoord [[attribute(1)]];
    short2 hemiTexcoord [[attribute(2)]];
    float4 tangent [[attribute(3)]];
    float4 binormal [[attribute(4)]];
};

struct LevelVertexInExactBase
{
    float3 position [[attribute(0)]];
    short2 baseTexcoord [[attribute(1)]];
    float4 tangent [[attribute(3)]];
    float4 binormal [[attribute(4)]];
};

struct LevelVertexOut
{
    float4 position [[position]];
    float2 baseTexcoord;
    float2 hemiTexcoord;
};

struct LevelUniforms
{
    float4x4 viewProjection;
    float minimumHeight;
    float inverseHeightRange;
    float2 padding;
};

vertex LevelVertexOut r4_level_texture_vs(
    LevelVertexIn input [[stage_in]],
    constant LevelUniforms& uniforms [[buffer(1)]])
{
    LevelVertexOut output;
    output.position = uniforms.viewProjection * float4(input.position, 1.0);
    output.baseTexcoord = float2(input.baseTexcoord) * (1.0 / 1024.0);
    output.hemiTexcoord = float2(0.0);
    return output;
}

vertex LevelVertexOut r4_level_texture_short4_vs(
    LevelVertexInShort4 input [[stage_in]],
    constant LevelUniforms& uniforms [[buffer(1)]])
{
    LevelVertexOut output;
    output.position = uniforms.viewProjection * float4(input.position, 1.0);
    output.baseTexcoord = float2(input.baseTexcoord.xy) * (1.0 / 1024.0);
    output.hemiTexcoord = float2(0.0);
    return output;
}

vertex LevelVertexOut r4_level_texture_exact_vs(
    LevelVertexInExactBase input [[stage_in]],
    constant LevelUniforms& uniforms [[buffer(1)]])
{
    LevelVertexOut output;
    output.position = uniforms.viewProjection * float4(input.position, 1.0);
    output.baseTexcoord =
        (float2(input.baseTexcoord) + float2(input.tangent.a, input.binormal.a)) *
        (1.0 / 1024.0);
    output.hemiTexcoord = float2(0.0);
    return output;
}

vertex LevelVertexOut r4_level_hemi_vs(
    LevelVertexInHemi input [[stage_in]],
    constant LevelUniforms& uniforms [[buffer(1)]])
{
    LevelVertexOut output;
    output.position = uniforms.viewProjection * float4(input.position, 1.0);
    output.baseTexcoord =
        (float2(input.baseTexcoord) + float2(input.tangent.a, input.binormal.a)) *
        (1.0 / 1024.0);
    output.hemiTexcoord = float2(input.hemiTexcoord) * (1.0 / 32768.0);
    return output;
}

fragment float4 r4_level_texture_ps(
    LevelVertexOut input [[stage_in]],
    texture2d<float> baseTexture [[texture(0)]],
    sampler baseSampler [[sampler(0)]])
{
    return float4(baseTexture.sample(baseSampler, input.baseTexcoord).rgb, 1.0);
}

fragment float4 r4_level_hemi_ps(
    LevelVertexOut input [[stage_in]],
    texture2d<float> baseTexture [[texture(0)]],
    texture2d<float> hemiTexture [[texture(1)]],
    sampler baseSampler [[sampler(0)]],
    sampler hemiSampler [[sampler(1)]])
{
    const float3 base = baseTexture.sample(baseSampler, input.baseTexcoord).rgb;
    const float hemi = hemiTexture.sample(hemiSampler, input.hemiTexcoord).a;
    return float4(base * hemi, 1.0);
}
)metal";

constexpr const char* GBufferDisplayMsl = R"metal(
#include <metal_stdlib>
using namespace metal;

struct DisplayVertexOut
{
    float4 position [[position]];
    float2 uv;
};

vertex DisplayVertexOut r4_gbuffer_display_vs(uint vertexId [[vertex_id]])
{
    const float2 positions[3] = {
        float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)
    };
    DisplayVertexOut output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.uv = float2(output.position.x * 0.5 + 0.5, 0.5 - output.position.y * 0.5);
    return output;
}

fragment float4 r4_gbuffer_display_ps(
    DisplayVertexOut input [[stage_in]],
    texture2d<float> inspectedColor [[texture(0)]],
    texture2d<float> packedPosition [[texture(1)]],
    sampler linearSampler [[sampler(0)]])
{
    const float3 presented = inspectedColor.sample(linearSampler, input.uv).rgb;
    const float depth = packedPosition.sample(linearSampler, input.uv).z;
    if (abs(depth) < 1.0e-6)
    {
        // The converted sky2 pass writes the lighting target only for the
        // stencil-zero background. Preserve the established diagnostic clear
        // when that target is empty, but do not hide a real converted sky.
        if (dot(abs(presented), float3(1.0)) < 1.0e-6)
            return float4(0.025, 0.055, 0.105, 1.0);
    }
    return float4(presented, 1.0);
}
)metal";

struct CombineVertex
{
    float position[4];
    std::uint32_t color;
    float jitter[2];
};
static_assert(sizeof(CombineVertex) == 28, "R4 FVF::TL layout must remain 28 bytes");

struct SunVolumeVertex
{
    float position[4];
};
static_assert(sizeof(SunVolumeVertex) == 16, "R4 sun cuboid layout must remain 16 bytes");

struct LuminanceBuildVertex
{
    float position[4];
    float texcoord[4][2];
};
static_assert(sizeof(LuminanceBuildVertex) == 48,
    "R4 v_build layout must remain 48 bytes");

struct LuminanceFilterVertex
{
    float position[4];
    float texcoord[8][4];
};
static_assert(sizeof(LuminanceFilterVertex) == 144,
    "R4 v_filter layout must remain 144 bytes");

// A material admission unit is intentionally one static visual plus its two
// authoritative texture assets.  It avoids silently collecting all descendants
// of a level root while still allowing a controlled multi-material scene.
struct LevelMaterialInput
{
    std::uint32_t visualId = 0;
    std::filesystem::path baseDdsPath;
    std::filesystem::path hemiDdsPath;
};

struct LevelScene
{
    struct TextureAsset
    {
        xray::metal::R4DdsTexture ddsTexture;
        std::shared_ptr<IR4Texture> texture;
        std::uint64_t uploadedBytes = 0;
        std::filesystem::path ddsPath;
    };

    struct TextureMaterial
    {
        std::shared_ptr<IR4Buffer> uniforms;
        std::shared_ptr<IR4Buffer> mscVertexGlobals;
        std::shared_ptr<IR4Buffer> mscVertexTransforms;
        std::shared_ptr<IR4Buffer> mscFragmentGlobals;
        std::shared_ptr<IR4Buffer> mscFragmentTransforms;
        std::shared_ptr<IR4Buffer> sunNearShadowTransforms;
        std::shared_ptr<IR4Buffer> sunShadowTransforms;
        Matrix4 modelTransform{};
        xray::metal::R4PositionBounds localPositionBounds;
        std::size_t textureAssetIndex = 0;
        std::size_t hemiTextureAssetIndex = std::numeric_limits<std::size_t>::max();
        std::uint32_t visualId = 0;
        std::uint16_t shaderId = 0;
        std::uint32_t vertexBufferId = 0;
        std::uint32_t vertexBase = 0;
        std::uint32_t vertexCount = 0;
        std::uint32_t indexBufferId = 0;
        std::uint32_t indexBase = 0;
        std::uint32_t indexCount = 0;
        std::uint32_t uvOffset = 0;
        std::uint32_t hemiUvOffset = 0;
        std::uint32_t normalOffset = 0;
        std::uint32_t tangentOffset = 0;
        std::uint32_t binormalOffset = 0;
        bool exactBaseUv = false;
        R4VertexFormat uvFormat = R4VertexFormat::Short2;
        std::string shaderName;
        std::string textureList;
        std::string virtualName;
    };

    xray::metal::R4LevelGeometry geometry;
    std::vector<std::uint8_t> geometryBytes;
    std::vector<std::shared_ptr<IR4Buffer>> vertexBuffers;
    std::vector<std::shared_ptr<IR4Buffer>> indexBuffers;
    std::vector<std::shared_ptr<IR4GraphicsPipeline>> pipelines;
    std::vector<std::shared_ptr<IR4GraphicsPipeline>> arefPipelines;
    std::shared_ptr<IR4Buffer> uniforms;
    std::shared_ptr<IR4Texture> depthTarget;
    std::shared_ptr<IR4Texture> scopeDepthSnapshot;
    std::vector<std::shared_ptr<IR4Texture>> gBufferTargets;
    std::shared_ptr<IR4Texture> lightingAccumulator;
    std::vector<std::shared_ptr<IR4Texture>> lightingTargets;
    std::shared_ptr<IR4Texture> scopeSceneColorSnapshot;
    std::shared_ptr<IR4Texture> lightingTonemap;
    std::shared_ptr<IR4Texture> lightingTonemapScratch;
    std::shared_ptr<IR4Texture> luminance64;
    std::shared_ptr<IR4Texture> luminance8;
    std::shared_ptr<IR4Texture> lightingWhite;
    std::shared_ptr<IR4Texture> lightingBlack;
    // Set only after the exact SSFX AO producer completed this frame.  IL is
    // intentionally still neutral until its distinct screen-space family is
    // admitted, so combine_1 never receives a plausible but wrong substitute.
    std::shared_ptr<IR4Texture> ssfxAoOutput;
    std::shared_ptr<IR4Texture> ssfxIlOutput;
    std::shared_ptr<IR4Texture> lutAtlas;
    xray::metal::R4DdsTexture lutAtlasDds;
    bool lutAtlasUploaded = false;
    std::shared_ptr<IR4Texture> lightingEnvironment;
    std::shared_ptr<IR4Texture> lightingEnvironment0;
    std::shared_ptr<IR4Texture> lightingEnvironment1;
    std::vector<TextureAsset> textureAssets;
    std::vector<TextureMaterial> textureMaterials;
    std::unordered_map<std::uint32_t, std::size_t> textureMaterialByVisual;
    std::shared_ptr<IR4Sampler> baseSampler;
    std::shared_ptr<IR4Sampler> hemiSampler;
    std::shared_ptr<IR4GraphicsPipeline> gBufferDisplayPipeline;
    std::shared_ptr<IR4Buffer> gBufferDisplayIndices;
    std::shared_ptr<IR4GraphicsPipeline> baseLightingPipeline;
    std::shared_ptr<IR4Buffer> baseLightingVertices;
    std::shared_ptr<IR4Buffer> baseLightingIndices;
    std::shared_ptr<IR4Buffer> baseLightingGlobals;
    std::shared_ptr<IR4Buffer> baseLightingStaticGlobals;
    std::shared_ptr<IR4Buffer> baseLightingDynamicLight;
    std::shared_ptr<IR4Sampler> lightingNoFilterSampler;
    std::shared_ptr<IR4Sampler> lightingLinearSampler;
    std::shared_ptr<IR4GraphicsPipeline> luminanceBuildPipeline;
    std::shared_ptr<IR4GraphicsPipeline> luminanceFilterPipeline;
    std::shared_ptr<IR4GraphicsPipeline> luminanceAdaptPipeline;
    std::shared_ptr<IR4Buffer> luminanceBuildVertices;
    std::shared_ptr<IR4Buffer> luminanceFilter64Vertices;
    std::shared_ptr<IR4Buffer> luminanceFilter8Vertices;
    std::shared_ptr<IR4Buffer> luminanceIndices;
    std::shared_ptr<IR4Buffer> luminanceBuildVertexGlobals;
    std::shared_ptr<IR4Buffer> luminanceBuildFragmentGlobals;
    std::shared_ptr<IR4Buffer> luminanceFilter64VertexGlobals;
    std::shared_ptr<IR4Buffer> luminanceFilter64FragmentGlobals;
    std::shared_ptr<IR4Buffer> luminanceFilter8VertexGlobals;
    std::shared_ptr<IR4Buffer> luminanceFilter8FragmentGlobals;
    std::shared_ptr<IR4Buffer> luminanceAdaptGlobals;
    std::shared_ptr<IR4Sampler> sunShadowSampler;
    std::shared_ptr<IR4GraphicsPipeline> sunMaskPipeline;
    std::shared_ptr<IR4GraphicsPipeline> sunNearPipeline;
    std::shared_ptr<IR4GraphicsPipeline> sunFarPipeline;
    std::shared_ptr<IR4GraphicsPipeline> sunShadowPipeline;
    std::shared_ptr<IR4GraphicsPipeline> sunShadowArefPipeline;
    std::shared_ptr<IR4Buffer> sunMaskVertices;
    std::shared_ptr<IR4Buffer> sunMaskIndices;
    std::shared_ptr<IR4Buffer> sunVolumeVertices;
    std::shared_ptr<IR4Buffer> sunVolumeIndices;
    std::shared_ptr<IR4Buffer> sunVertexGlobals;
    std::shared_ptr<IR4Buffer> sunVertexTransforms;
    std::shared_ptr<IR4Buffer> sunNearFragmentGlobals;
    std::shared_ptr<IR4Buffer> sunFarFragmentGlobals;
    std::shared_ptr<IR4Buffer> sunMaskDynamicLight;
    std::shared_ptr<IR4Buffer> sunNearShadowTransforms;
    std::shared_ptr<IR4Buffer> sunShadowTransforms;
    std::shared_ptr<IR4Texture> sunMaskTexture;
    std::shared_ptr<IR4Texture> sunNearShadowTexture;
    std::shared_ptr<IR4Texture> sunShadowTexture;
    std::shared_ptr<IR4Texture> sunShadowDepthTarget;
    std::shared_ptr<IR4Texture> sunSssTexture;
    LevelUniforms uniformData{};
    Matrix4 view{};
    Matrix4 projection{};
    Matrix4 sunNearShadowViewProjection{};
    Matrix4 sunShadowViewProjection{};
    Vec3 eye{};
    Vec3 center{};
    float radius = 0.0f;
    std::uint64_t drawableVisuals = 0;
    std::uint64_t drawableIndices = 0;
    std::uint32_t textureRootId = std::numeric_limits<std::uint32_t>::max();
    bool hemiDiagnostic = false;
    bool productionMsc = false;
    bool productionAref = false;
    bool productionBaseLighting = false;
    bool productionLuminance = false;
    bool productionSun = false;
    bool automaticLmhFrustumCulling = false;
    bool liveViewProjectionValid = false;
    bool gameplayViewValid = false;
    bool automaticLmhLiveFrustumReported = false;
    bool automaticLmhGameplayFrustumReported = false;
    std::uint64_t automaticLmhFrustumFrames = 0;
    std::uint64_t automaticLmhFrustumSubmitted = 0;
    std::uint64_t automaticLmhFrustumCulled = 0;
    bool lightingInputsUploaded = false;
    float luminanceAdaptation = 0.5f;
    std::uint32_t luminanceLastTelemetryFrame = UINT32_MAX;
    bool uploaded = false;
};

bool readTextFile(const std::filesystem::path& path, std::string& text)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return false;
    text.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return stream.good() || stream.eof();
}

bool fileSha256(const std::filesystem::path& path, std::string& digest)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return false;
    CC_SHA256_CTX context;
    CC_SHA256_Init(&context);
    std::array<char, 64 * 1024> block{};
    while (stream)
    {
        stream.read(block.data(), block.size());
        const std::streamsize count = stream.gcount();
        if (count > 0)
            CC_SHA256_Update(&context, block.data(), static_cast<CC_LONG>(count));
    }
    if (!stream.eof())
        return false;
    std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> bytes{};
    CC_SHA256_Final(bytes.data(), &context);
    static constexpr char Hex[] = "0123456789abcdef";
    digest.resize(bytes.size() * 2);
    for (std::size_t index = 0; index < bytes.size(); ++index)
    {
        digest[index * 2] = Hex[bytes[index] >> 4];
        digest[index * 2 + 1] = Hex[bytes[index] & 0x0f];
    }
    return true;
}

bool receiptAdmitsArtifact(
    const std::string& receipt,
    const char* stage,
    const std::string& metallibSha256,
    const std::string& reflectionSha256)
{
    const std::string prefix = std::string("stage=") + stage + "\t";
    std::size_t lineStart = receipt.find(prefix);
    while (lineStart != std::string::npos)
    {
        const std::size_t lineEnd = receipt.find('\n', lineStart);
        const std::string line = receipt.substr(lineStart, lineEnd - lineStart);
        if (line.find("\tmetallib_sha256=" + metallibSha256) != std::string::npos &&
            line.find("\treflection_sha256=" + reflectionSha256) != std::string::npos)
            return true;
        if (lineEnd == std::string::npos)
            break;
        lineStart = receipt.find(prefix, lineEnd + 1);
    }
    return false;
}

std::string automaticLinearTlab(
    const std::vector<std::pair<std::uint32_t, const char*>>& entries)
{
    std::string result = "\"TopLevelArgumentBuffer\":[";
    for (std::size_t index = 0; index < entries.size(); ++index)
    {
        if (index != 0)
            result += ',';
        result += "{\"EltOffset\":" + std::to_string(index * 24) +
            ",\"Name\":\"\",\"Size\":24,\"Slot\":" +
            std::to_string(entries[index].first) +
            ",\"Space\":0,\"Type\":\"" + entries[index].second + "\"}";
    }
    result += ']';
    return result;
}

bool validateLmhMscReflection(
    const std::filesystem::path& vertexMetallibPath,
    const std::filesystem::path& fragmentMetallibPath,
    std::string& error)
{
    std::filesystem::path vertexReflection = vertexMetallibPath;
    std::filesystem::path fragmentReflection = fragmentMetallibPath;
    vertexReflection.replace_extension(".json");
    fragmentReflection.replace_extension(".json");
    if (vertexMetallibPath.filename() != "deffer_base_lmh_flat.vs.metallib" ||
        fragmentMetallibPath.filename() != "deffer_base_lmh_flat.ps.metallib" ||
        vertexMetallibPath.parent_path() != fragmentMetallibPath.parent_path())
    {
        error = "production MSC LMH mode requires the exact named shader pair";
        return false;
    }
    std::string vertexJson;
    std::string fragmentJson;
    if (!readTextFile(vertexReflection, vertexJson) ||
        !readTextFile(fragmentReflection, fragmentJson))
    {
        error = "production MSC LMH metallibs require adjacent reflection JSON files";
        return false;
    }
    std::string receipt;
    std::string vertexMetallibHash;
    std::string fragmentMetallibHash;
    std::string vertexReflectionHash;
    std::string fragmentReflectionHash;
    const std::filesystem::path receiptPath =
        vertexMetallibPath.parent_path() / "lmh-msc-receipt.txt";
    if (!readTextFile(receiptPath, receipt) ||
        !fileSha256(vertexMetallibPath, vertexMetallibHash) ||
        !fileSha256(fragmentMetallibPath, fragmentMetallibHash) ||
        !fileSha256(vertexReflection, vertexReflectionHash) ||
        !fileSha256(fragmentReflection, fragmentReflectionHash) ||
        !receiptAdmitsArtifact(
            receipt, "vs", vertexMetallibHash, vertexReflectionHash) ||
        !receiptAdmitsArtifact(
            receipt, "ps", fragmentMetallibHash, fragmentReflectionHash))
    {
        error = "production MSC LMH artifacts do not match their conversion receipt";
        return false;
    }
    static constexpr const char* VertexTlab =
        "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,"
        "\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":24,\"Name\":\"\","
        "\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"}]";
    static constexpr const char* VertexInputs =
        "\"vertex_inputs\":[{\"columnCount\":4,\"elementType\":\"Float\",\"index\":0,"
        "\"name\":\"normal0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":1,"
        "\"name\":\"tangent0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":2,"
        "\"name\":\"binormal0\"},{\"columnCount\":2,\"elementType\":\"Int32\",\"index\":3,"
        "\"name\":\"texcoord0\"},{\"columnCount\":2,\"elementType\":\"Int32\",\"index\":4,"
        "\"name\":\"texcoord1\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":5,"
        "\"name\":\"position0\"}]";
    static constexpr const char* FragmentTlab =
        "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,"
        "\"Slot\":0,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":24,\"Name\":\"\","
        "\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":48,"
        "\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},"
        "{\"EltOffset\":72,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,"
        "\"Type\":\"CBV\"},{\"EltOffset\":96,\"Name\":\"\",\"Size\":24,\"Slot\":0,"
        "\"Space\":0,\"Type\":\"Sampler\"},{\"EltOffset\":120,\"Name\":\"\","
        "\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"Sampler\"}]";
    if (vertexJson.find("\"ShaderType\":\"Vertex\"") == std::string::npos ||
        vertexJson.find("\"ResourceCount\":2") == std::string::npos ||
        vertexJson.find(VertexTlab) == std::string::npos ||
        vertexJson.find(VertexInputs) == std::string::npos ||
        fragmentJson.find("\"ShaderType\":\"Fragment\"") == std::string::npos ||
        fragmentJson.find("\"ResourceCount\":6") == std::string::npos ||
        fragmentJson.find(FragmentTlab) == std::string::npos ||
        fragmentJson.find("\"num_render_targets\":4") == std::string::npos)
    {
        error = "production MSC shader reflection does not match deffer_base_lmh_flat ABI";
        return false;
    }
    return true;
}

bool validateLmhArefMscReflection(
    const std::filesystem::path& fragmentMetallibPath,
    std::string& error)
{
    if (fragmentMetallibPath.empty())
        return true;
    if (fragmentMetallibPath.filename() != "deffer_base_lmh_aref_flat.ps.metallib")
    {
        error = "production MSC LMH alpha-test mode requires the exact aref fragment shader";
        return false;
    }
    std::filesystem::path reflection = fragmentMetallibPath;
    reflection.replace_extension(".json");
    std::string json;
    std::string receipt;
    std::string metallibHash;
    std::string reflectionHash;
    if (!readTextFile(reflection, json) ||
        !readTextFile(fragmentMetallibPath.parent_path() / "lmh-aref-msc-receipt.txt", receipt) ||
        !fileSha256(fragmentMetallibPath, metallibHash) ||
        !fileSha256(reflection, reflectionHash) ||
        !receiptAdmitsArtifact(receipt, "ps", metallibHash, reflectionHash))
    {
        error = "production MSC LMH alpha-test artifact does not match its conversion receipt";
        return false;
    }
    static constexpr const char* FragmentTlab =
        "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,"
        "\"Slot\":0,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":24,\"Name\":\"\","
        "\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":48,"
        "\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},"
        "{\"EltOffset\":72,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,"
        "\"Type\":\"CBV\"},{\"EltOffset\":96,\"Name\":\"\",\"Size\":24,"
        "\"Slot\":0,\"Space\":0,\"Type\":\"Sampler\"},{\"EltOffset\":120,"
        "\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"Sampler\"}]";
    if (json.find("\"ShaderType\":\"Fragment\"") == std::string::npos ||
        json.find("\"ResourceCount\":6") == std::string::npos ||
        json.find(FragmentTlab) == std::string::npos ||
        json.find("\"num_render_targets\":4") == std::string::npos ||
        json.find("\"discards\":true") == std::string::npos)
    {
        error = "production MSC LMH alpha-test reflection does not match the flat aref ABI";
        return false;
    }
    return true;
}

bool validateBaseLightingMscReflection(
    const std::filesystem::path& vertexMetallibPath,
    const std::filesystem::path& fragmentMetallibPath,
    std::string& error)
{
    std::filesystem::path vertexReflection = vertexMetallibPath;
    std::filesystem::path fragmentReflection = fragmentMetallibPath;
    vertexReflection.replace_extension(".json");
    fragmentReflection.replace_extension(".json");
    if (vertexMetallibPath.filename() != "combine_1.vs.metallib" ||
        fragmentMetallibPath.filename() != "combine_1_nomsaa.ps.metallib" ||
        vertexMetallibPath.parent_path() != fragmentMetallibPath.parent_path())
    {
        error = "production MSC base lighting requires the exact combine_1 shader pair";
        return false;
    }
    std::string vertexJson;
    std::string fragmentJson;
    std::string receipt;
    std::string vertexMetallibHash;
    std::string fragmentMetallibHash;
    std::string vertexReflectionHash;
    std::string fragmentReflectionHash;
    const std::filesystem::path receiptPath =
        vertexMetallibPath.parent_path() / "base-lighting-msc-receipt.txt";
    if (!readTextFile(vertexReflection, vertexJson) ||
        !readTextFile(fragmentReflection, fragmentJson) ||
        !readTextFile(receiptPath, receipt) ||
        !fileSha256(vertexMetallibPath, vertexMetallibHash) ||
        !fileSha256(fragmentMetallibPath, fragmentMetallibHash) ||
        !fileSha256(vertexReflection, vertexReflectionHash) ||
        !fileSha256(fragmentReflection, fragmentReflectionHash) ||
        !receiptAdmitsArtifact(receipt, "vs", vertexMetallibHash, vertexReflectionHash) ||
        !receiptAdmitsArtifact(receipt, "ps", fragmentMetallibHash, fragmentReflectionHash))
    {
        error = "production MSC base-lighting artifacts do not match their conversion receipt";
        return false;
    }
    const std::string vertexTlab = automaticLinearTlab({{0, "SRV"}});
    const std::string fragmentTlab = automaticLinearTlab({
        {0, "SRV"}, {1, "SRV"}, {2, "SRV"}, {3, "SRV"},
        {4, "SRV"}, {5, "SRV"}, {6, "SRV"}, {7, "SRV"},
        {0, "CBV"}, {1, "CBV"}, {2, "CBV"},
        {0, "Sampler"}, {1, "Sampler"}, {2, "Sampler"},
    });
    static constexpr const char* VertexInputs =
        "\"vertex_inputs\":[{\"columnCount\":4,\"elementType\":\"Float\",\"index\":0,"
        "\"name\":\"positiont0\"},{\"columnCount\":2,\"elementType\":\"Float\","
        "\"index\":1,\"name\":\"texcoord0\"}]";
    if (vertexJson.find("\"ShaderType\":\"Vertex\"") == std::string::npos ||
        vertexJson.find("\"ResourceCount\":1") == std::string::npos ||
        vertexJson.find(vertexTlab) == std::string::npos ||
        vertexJson.find(VertexInputs) == std::string::npos ||
        fragmentJson.find("\"ShaderType\":\"Fragment\"") == std::string::npos ||
        fragmentJson.find("\"ResourceCount\":14") == std::string::npos ||
        fragmentJson.find(fragmentTlab) == std::string::npos ||
        fragmentJson.find("\"num_render_targets\":2") == std::string::npos)
    {
        error = "production MSC shader reflection does not match combine_1 ABI";
        return false;
    }
    return true;
}

bool validateSunMscReflection(
    const std::filesystem::path& shaderDirectory,
    std::string& error)
{
    struct ShaderContract
    {
        const char* basename;
        const char* stage;
        std::size_t resourceCount;
        std::string tlab;
        const char* vertexInputs;
    };
    const std::array<ShaderContract, 9> contracts = {{
        {"stub_notransform_t.vs", "vs", 1,
            automaticLinearTlab({{0, "CBV"}}),
            "\"vertex_inputs\":[{\"columnCount\":4,\"elementType\":\"Float\","
            "\"index\":0,\"name\":\"positiont0\"},{\"columnCount\":2,"
            "\"elementType\":\"Float\",\"index\":1,\"name\":\"texcoord0\"},"
            "{\"columnCount\":4,\"elementType\":\"Float\",\"index\":2,"
            "\"name\":\"color0\"}]"},
        {"accum_sun_mask_nomsaa.ps", "ps", 3,
            automaticLinearTlab({{0, "SRV"}, {0, "CBV"}, {0, "Sampler"}}), nullptr},
        {"accum_sun.vs", "vs", 2,
            automaticLinearTlab({{0, "CBV"}, {1, "CBV"}}),
            "\"vertex_inputs\":[{\"columnCount\":4,\"elementType\":\"Float\","
            "\"index\":0,\"name\":\"position0\"}]"},
        {"accum_sun_near_nomsaa_nominmax.ps", "ps", 11,
            automaticLinearTlab({{1, "SRV"}, {2, "SRV"}, {3, "SRV"},
                {0, "SRV"}, {4, "SRV"}, {0, "CBV"}, {1, "CBV"},
                {2, "CBV"}, {0, "Sampler"}, {1, "Sampler"}, {2, "Sampler"}}),
            nullptr},
        {"accum_sun_far_nomsaa.ps", "ps", 11,
            automaticLinearTlab({{1, "SRV"}, {2, "SRV"}, {3, "SRV"},
                {4, "SRV"}, {0, "SRV"}, {0, "CBV"}, {1, "CBV"},
                {2, "CBV"}, {0, "Sampler"}, {1, "Sampler"}, {2, "Sampler"}}),
            nullptr},
        {"shadow_direct_base.vs", "vs", 1,
            automaticLinearTlab({{0, "CBV"}}),
            "\"vertex_inputs\":[{\"columnCount\":4,\"elementType\":\"Float\","
            "\"index\":0,\"name\":\"position0\"}]"},
        {"shadow_direct_base_color.ps", "ps", 0,
            automaticLinearTlab({}), nullptr},
        {"shadow_direct_base_aref.vs", "vs", 1,
            automaticLinearTlab({{0, "CBV"}}),
            "\"vertex_inputs\":[{\"columnCount\":4,\"elementType\":\"Float\",\"index\":0,\"name\":\"normal0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":1,\"name\":\"tangent0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":2,\"name\":\"binormal0\"},{\"columnCount\":2,\"elementType\":\"Int32\",\"index\":3,\"name\":\"texcoord0\"},{\"columnCount\":4,\"elementType\":\"Float\",\"index\":4,\"name\":\"position0\"}]"},
        {"shadow_direct_base_aref_color.ps", "ps", 2,
            automaticLinearTlab({{0, "SRV"}, {0, "Sampler"}}), nullptr},
    }};

    std::string receipt;
    if (!readTextFile(shaderDirectory / "sun-msc-receipt.txt", receipt))
    {
        error = "production MSC direct-sun receipt is unavailable";
        return false;
    }
    for (const ShaderContract& contract : contracts)
    {
        const std::filesystem::path metallib =
            shaderDirectory / (std::string(contract.basename) + ".metallib");
        const std::filesystem::path reflection =
            shaderDirectory / (std::string(contract.basename) + ".json");
        std::string json;
        std::string metallibHash;
        std::string reflectionHash;
        if (!readTextFile(reflection, json) ||
            !fileSha256(metallib, metallibHash) ||
            !fileSha256(reflection, reflectionHash) ||
            !receiptAdmitsArtifact(
                receipt, contract.stage, metallibHash, reflectionHash) ||
            json.find(contract.stage[0] == 'v' ?
                "\"ShaderType\":\"Vertex\"" : "\"ShaderType\":\"Fragment\"") ==
                std::string::npos ||
            json.find("\"ResourceCount\":" +
                std::to_string(contract.resourceCount)) == std::string::npos ||
            json.find(contract.tlab) == std::string::npos ||
            (contract.vertexInputs && json.find(contract.vertexInputs) == std::string::npos) ||
            (contract.stage[0] == 'p' &&
             json.find("\"num_render_targets\":1") == std::string::npos))
        {
            error = "production MSC direct-sun artifact/reflection mismatch: " +
                std::string(contract.basename);
            return false;
        }
    }
    return true;
}

bool validateLuminanceMscReflection(
    const std::filesystem::path& shaderDirectory,
    std::string& error)
{
    struct ShaderContract
    {
        const char* basename;
        const char* stage;
        std::size_t resourceCount;
        const char* tlab;
        const char* vertexInputs;
    };
    static const std::array<ShaderContract, 5> Contracts = {{
        {"stub_notransform_build.vs", "vs", 1,
            "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"}]",
            "\"name\":\"texcoord3\""},
        {"stub_notransform_filter.vs", "vs", 1,
            "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"}]",
            "\"name\":\"texcoord7\""},
        {"bloom_luminance_1.ps", "ps", 3,
            "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":48,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"Sampler\"}]",
            nullptr},
        {"bloom_luminance_2.ps", "ps", 3,
            "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":48,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"Sampler\"}]",
            nullptr},
        {"bloom_luminance_3.ps", "ps", 6,
            "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":48,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":72,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":96,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"Sampler\"},{\"EltOffset\":120,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"Sampler\"}]",
            nullptr},
    }};
    std::string receipt;
    if (!readTextFile(shaderDirectory / "luminance-msc-receipt.txt", receipt))
    {
        error = "production MSC luminance receipt is unavailable";
        return false;
    }
    for (const ShaderContract& contract : Contracts)
    {
        const std::filesystem::path metallib =
            shaderDirectory / (std::string(contract.basename) + ".metallib");
        const std::filesystem::path reflection =
            shaderDirectory / (std::string(contract.basename) + ".json");
        std::string json;
        std::string metallibHash;
        std::string reflectionHash;
        if (!readTextFile(reflection, json) ||
            !fileSha256(metallib, metallibHash) ||
            !fileSha256(reflection, reflectionHash) ||
            !receiptAdmitsArtifact(receipt, contract.stage, metallibHash, reflectionHash) ||
            json.find(contract.stage[0] == 'v' ?
                "\"ShaderType\":\"Vertex\"" : "\"ShaderType\":\"Fragment\"") ==
                std::string::npos ||
            json.find("\"ResourceCount\":" + std::to_string(contract.resourceCount)) ==
                std::string::npos ||
            json.find(contract.tlab) == std::string::npos ||
            (contract.vertexInputs && json.find(contract.vertexInputs) == std::string::npos) ||
            (contract.stage[0] == 'p' &&
             json.find("\"num_render_targets\":1") == std::string::npos))
        {
            error = "production MSC luminance artifact/reflection mismatch: " +
                std::string(contract.basename);
            return false;
        }
    }
    return true;
}

bool prepareLevelScene(
    const std::filesystem::path& levelDirectory,
    const std::vector<std::filesystem::path>& textureDdsPaths,
    const std::filesystem::path& hemiDdsPath,
    const std::vector<LevelMaterialInput>& materialInputs,
    const xray::metal::R4DdsBytesProvider* ddsBytesProvider,
    const std::filesystem::path& vertexMetallibPath,
    const std::filesystem::path& fragmentMetallibPath,
    const std::filesystem::path& arefFragmentMetallibPath,
    const std::filesystem::path& lightingVertexMetallibPath,
    const std::filesystem::path& lightingFragmentMetallibPath,
    const std::filesystem::path& sunShaderDirectory,
    const std::filesystem::path& luminanceShaderDirectory,
    std::uint32_t textureVisualId,
    std::uint32_t textureRootId,
    IR4Backend& backend,
    LevelScene& scene,
    std::string& error)
{
    if (!xray::metal::LoadR4LevelGeometryWithBytes(
            levelDirectory, scene.geometry, scene.geometryBytes, error))
        return false;

    scene.productionMsc = !vertexMetallibPath.empty() && !fragmentMetallibPath.empty();
    if (scene.productionMsc &&
        !validateLmhMscReflection(vertexMetallibPath, fragmentMetallibPath, error))
        return false;
    scene.productionAref = !arefFragmentMetallibPath.empty();
    if (scene.productionAref &&
        (!scene.productionMsc || !validateLmhArefMscReflection(
            arefFragmentMetallibPath, error)))
        return false;
    scene.productionBaseLighting = !lightingVertexMetallibPath.empty() &&
        !lightingFragmentMetallibPath.empty();
    if (scene.productionBaseLighting &&
        (!scene.productionMsc || !validateBaseLightingMscReflection(
            lightingVertexMetallibPath, lightingFragmentMetallibPath, error)))
        return false;
    scene.productionSun = !sunShaderDirectory.empty();
    if (scene.productionSun &&
        (!scene.productionBaseLighting ||
         !validateSunMscReflection(sunShaderDirectory, error)))
        return false;
    scene.productionLuminance = !luminanceShaderDirectory.empty();
    if (scene.productionLuminance &&
        (!scene.productionBaseLighting ||
         !validateLuminanceMscReflection(luminanceShaderDirectory, error)))
        return false;

    const bool explicitMaterialSet = !materialInputs.empty();
    const bool textureDiagnostic = explicitMaterialSet || !textureDdsPaths.empty();
    std::vector<LevelMaterialInput> resolvedMaterials;
    if (explicitMaterialSet)
    {
        if (textureRootId != std::numeric_limits<std::uint32_t>::max() ||
            !textureDdsPaths.empty() || !hemiDdsPath.empty())
        {
            error = "explicit material inputs cannot be combined with legacy root/DDS selection";
            return false;
        }
        resolvedMaterials = materialInputs;
    }
    else if (textureDiagnostic)
    {
        std::vector<std::uint32_t> selectedVisualIds;
        if (textureRootId != std::numeric_limits<std::uint32_t>::max())
        {
            if (!xray::metal::CollectR4StaticVisualDescendants(
                    scene.geometry, textureRootId, selectedVisualIds, error))
                return false;
            scene.textureRootId = textureRootId;
        }
        else
            selectedVisualIds.push_back(textureVisualId);

        std::unordered_map<std::string, std::filesystem::path> baseByFilename;
        for (const std::filesystem::path& ddsPath : textureDdsPaths)
        {
            const std::string filename = ddsPath.filename().string();
            if (filename.empty() || !baseByFilename.emplace(filename, ddsPath).second)
            {
                error = "texture diagnostic DDS filenames must be unique";
                return false;
            }
        }
        for (const std::uint32_t visualId : selectedVisualIds)
        {
            const auto found = std::find_if(scene.geometry.visuals.begin(), scene.geometry.visuals.end(),
                [visualId](const auto& visual) { return visual.visualId == visualId; });
            if (found == scene.geometry.visuals.end() || found->shaderId >= scene.geometry.shaders.size())
            {
                error = "legacy texture selection references a missing visual";
                return false;
            }
            std::string normalizedBase = scene.geometry.shaders[found->shaderId].baseTexture;
            std::replace(normalizedBase.begin(), normalizedBase.end(), '\\', '/');
            const std::string expected = std::filesystem::path(normalizedBase).filename().string() + ".dds";
            const auto base = baseByFilename.find(expected);
            if (base == baseByFilename.end())
            {
                error = "no supplied DDS matches base texture " + expected;
                return false;
            }
            resolvedMaterials.push_back({visualId, base->second, hemiDdsPath});
        }
    }

    if (textureDiagnostic)
    {
        std::unordered_map<std::uint32_t, std::size_t> materialInputByVisual;
        std::unordered_map<std::string, std::size_t> textureAssetByPath;
        scene.textureMaterials.reserve(scene.textureMaterials.size() + resolvedMaterials.size());
        scene.textureMaterialByVisual.reserve(
            scene.textureMaterialByVisual.size() + resolvedMaterials.size());
        if (resolvedMaterials.size() <= std::numeric_limits<std::size_t>::max() / 2u)
        {
            scene.textureAssets.reserve(
                scene.textureAssets.size() + resolvedMaterials.size() * 2u);
            textureAssetByPath.reserve(resolvedMaterials.size() * 2u);
        }
        materialInputByVisual.reserve(resolvedMaterials.size());
        const auto addTextureAsset = [&](const std::filesystem::path& path,
                                         std::size_t& assetIndex) -> bool {
            const std::string key = path.lexically_normal().generic_string();
            if (key.empty())
            {
                error = "controlled material set contains an empty DDS path";
                return false;
            }
            const auto existing = textureAssetByPath.find(key);
            if (existing != textureAssetByPath.end())
            {
                assetIndex = existing->second;
                return true;
            }
            LevelScene::TextureAsset asset;
            const bool isVirtualPath = key.rfind("xrfs:", 0u) == 0u;
            if ((isVirtualPath && !ddsBytesProvider) ||
                (isVirtualPath && !xray::metal::LoadR4DdsTexture(
                    path, *ddsBytesProvider, asset.ddsTexture, error)) ||
                (!isVirtualPath && !xray::metal::LoadR4DdsTexture(
                    path, asset.ddsTexture, error)))
            {
                if (isVirtualPath && !ddsBytesProvider)
                    error = "xrfs DDS path requires an in-memory byte provider";
                return false;
            }
            asset.ddsPath = path;
            assetIndex = scene.textureAssets.size();
            textureAssetByPath.emplace(key, assetIndex);
            scene.textureAssets.emplace_back(std::move(asset));
            return true;
        };

        bool anyHemi = false;
        bool everyMaterialHasHemi = !resolvedMaterials.empty();
        for (std::size_t inputIndex = 0; inputIndex < resolvedMaterials.size(); ++inputIndex)
        {
            const LevelMaterialInput& input = resolvedMaterials[inputIndex];
            if (input.visualId == std::numeric_limits<std::uint32_t>::max() ||
                input.baseDdsPath.empty() ||
                !materialInputByVisual.emplace(input.visualId, inputIndex).second)
            {
                error = "controlled material set requires unique visual ids and base DDS paths";
                return false;
            }
            anyHemi = anyHemi || !input.hemiDdsPath.empty();
            everyMaterialHasHemi = everyMaterialHasHemi && !input.hemiDdsPath.empty();
        }
        if (resolvedMaterials.empty() || (anyHemi && !everyMaterialHasHemi))
        {
            error = "controlled material set must be non-empty and cannot mix LMH and non-LMH visuals";
            return false;
        }

        const auto leafFilename = [](const std::filesystem::path& p) {
            std::string s = p.generic_string();
            std::replace(s.begin(), s.end(), '\\', '/');
            const std::size_t slash = s.rfind('/');
            return slash == std::string::npos ? s : s.substr(slash + 1u);
        };
        for (const LevelMaterialInput& input : resolvedMaterials)
        {
            const auto found = std::find_if(scene.geometry.visuals.begin(), scene.geometry.visuals.end(),
                [&input](const auto& visual) { return visual.visualId == input.visualId; });
            if (found == scene.geometry.visuals.end() || !found->valid ||
                !found->referencedPositionBounds.available ||
                found->shaderId >= scene.geometry.shaders.size())
            {
                error = "controlled material visual is missing, invalid, or has no resolved material";
                return false;
            }
            const xray::metal::R4LevelShader& shader = scene.geometry.shaders[found->shaderId];
            std::string normalizedBase = shader.baseTexture;
            std::replace(normalizedBase.begin(), normalizedBase.end(), '\\', '/');
            const std::string expectedBase =
                std::filesystem::path(normalizedBase).filename().string() + ".dds";
            if (leafFilename(input.baseDdsPath) != expectedBase)
            {
                error = "controlled material base DDS does not match visual " +
                    std::to_string(input.visualId) + " texture " + expectedBase;
                return false;
            }

            const xray::metal::R4VertexBufferSummary& vertexBuffer =
                scene.geometry.vertexBuffers[found->vertexBufferId];
            std::uint32_t baseTexcoordOffset = 0;
            bool hasBaseTexcoord = false;
            R4VertexFormat baseTexcoordFormat = R4VertexFormat::Short2;
            for (const xray::metal::R4VertexElement& element : vertexBuffer.declaration)
            {
                if (element.stream == 0u && (element.type == 6u || element.type == 7u) &&
                    element.method == 0u && element.usage == 5u && element.usageIndex == 0u)
                {
                    baseTexcoordOffset = element.offset;
                    baseTexcoordFormat = element.type == 6u ?
                        R4VertexFormat::Short2 : R4VertexFormat::Short4;
                    hasBaseTexcoord = true;
                    break;
                }
            }
            if (!hasBaseTexcoord)
            {
                error = "controlled material requires stream-0 TEXCOORD0 SHORT2 or SHORT4";
                return false;
            }
            const auto findElement = [&vertexBuffer](std::uint8_t usage, std::uint8_t usageIndex) {
                return std::find_if(vertexBuffer.declaration.begin(), vertexBuffer.declaration.end(),
                    [usage, usageIndex](const xray::metal::R4VertexElement& element) {
                        return element.stream == 0u && element.type == 4u && element.method == 0u &&
                            element.usage == usage && element.usageIndex == usageIndex;
                    });
            };
            const auto tangentElement = findElement(6u, 0u);
            const auto normalElement = findElement(3u, 0u);
            const auto binormalElement = findElement(7u, 0u);

            LevelScene::TextureMaterial material;
            if (!addTextureAsset(input.baseDdsPath, material.textureAssetIndex))
                return false;
            material.visualId = found->visualId;
            material.shaderId = found->shaderId;
            material.vertexBufferId = found->vertexBufferId;
            material.vertexBase = found->vertexBase;
            material.vertexCount = found->vertexCount;
            material.indexBufferId = found->indexBufferId;
            material.indexBase = found->indexBase;
            material.indexCount = found->indexCount;
            material.uvOffset = baseTexcoordOffset;
            material.uvFormat = baseTexcoordFormat;
            if (normalElement != vertexBuffer.declaration.end())
                material.normalOffset = normalElement->offset;
            if (baseTexcoordFormat == R4VertexFormat::Short2 &&
                tangentElement != vertexBuffer.declaration.end() &&
                binormalElement != vertexBuffer.declaration.end())
            {
                material.tangentOffset = tangentElement->offset;
                material.binormalOffset = binormalElement->offset;
                material.exactBaseUv = true;
            }
            if (everyMaterialHasHemi)
            {
                if (shader.textures.size() < 3u || shader.textures[2].rfind("lmap#", 0u) != 0u)
                {
                    error = "controlled LMH visual has no R4 hemi lightmap in texture slot 2";
                    return false;
                }
                std::string normalizedHemi = shader.textures[2];
                std::replace(normalizedHemi.begin(), normalizedHemi.end(), '\\', '/');
                const std::string expectedHemi =
                    std::filesystem::path(normalizedHemi).filename().string() + ".dds";
                if (leafFilename(input.hemiDdsPath) != expectedHemi)
                {
                    error = "controlled material hemi DDS does not match visual " +
                        std::to_string(input.visualId) + " texture " + expectedHemi;
                    return false;
                }
                const auto hemiElement = std::find_if(
                    vertexBuffer.declaration.begin(), vertexBuffer.declaration.end(),
                    [](const xray::metal::R4VertexElement& element) {
                        return element.stream == 0u && element.type == 6u && element.method == 0u &&
                            element.usage == 5u && element.usageIndex == 1u;
                    });
                if (hemiElement == vertexBuffer.declaration.end() || !material.exactBaseUv)
                {
                    error = "controlled LMH material requires TEXCOORD1 SHORT2 and packed tangent/binormal";
                    return false;
                }
                if (!addTextureAsset(input.hemiDdsPath, material.hemiTextureAssetIndex))
                    return false;
                material.hemiUvOffset = hemiElement->offset;
            }
            material.shaderName = shader.shaderName;
            material.textureList = shader.textureList;
            material.virtualName = shader.baseTexture;
            material.localPositionBounds = found->referencedPositionBounds;
            std::copy(found->modelTransform.begin(), found->modelTransform.end(),
                material.modelTransform.elements);
            scene.textureMaterialByVisual.emplace(material.visualId, scene.textureMaterials.size());
            scene.textureMaterials.emplace_back(std::move(material));
        }
        scene.hemiDiagnostic = everyMaterialHasHemi;
    }

    if (scene.productionMsc)
    {
        if ((!scene.hemiDiagnostic && !scene.textureMaterials.empty()) ||
            !std::filesystem::is_regular_file(vertexMetallibPath) ||
            !std::filesystem::is_regular_file(fragmentMetallibPath))
        {
            error = "production MSC LMH mode requires controlled hemi visuals and two existing metallibs";
            return false;
        }
        for (const LevelScene::TextureMaterial& material : scene.textureMaterials)
        {
            if (material.normalOffset == 0 || material.uvFormat != R4VertexFormat::Short2)
            {
                error = "production MSC LMH visual does not match the reflected static vertex ABI";
                return false;
            }
        }
    }

    Vec3 minimum{INFINITY, INFINITY, INFINITY};
    Vec3 maximum{-INFINITY, -INFINITY, -INFINITY};
    for (const xray::metal::R4StaticVisual& visual : scene.geometry.visuals)
    {
        if (!visual.valid || !visual.referencedPositionBounds.available)
            continue;
        if (textureDiagnostic && !scene.textureMaterialByVisual.count(visual.visualId))
            continue;
        const auto material = scene.textureMaterialByVisual.find(visual.visualId);
        if (textureDiagnostic && material != scene.textureMaterialByVisual.end())
        {
            const auto& bounds = visual.referencedPositionBounds;
            const Matrix4& model = scene.textureMaterials[material->second].modelTransform;
            for (std::uint32_t corner = 0; corner < 8u; ++corner)
            {
                const Vec3 local{
                    corner & 1u ? bounds.maximum[0] : bounds.minimum[0],
                    corner & 2u ? bounds.maximum[1] : bounds.minimum[1],
                    corner & 4u ? bounds.maximum[2] : bounds.minimum[2],
                };
                const Vec3 world = transformPoint(model, local);
                minimum.x = std::min(minimum.x, world.x);
                minimum.y = std::min(minimum.y, world.y);
                minimum.z = std::min(minimum.z, world.z);
                maximum.x = std::max(maximum.x, world.x);
                maximum.y = std::max(maximum.y, world.y);
                maximum.z = std::max(maximum.z, world.z);
            }
        }
        else
        {
            minimum.x = std::min(minimum.x, visual.referencedPositionBounds.minimum[0]);
            minimum.y = std::min(minimum.y, visual.referencedPositionBounds.minimum[1]);
            minimum.z = std::min(minimum.z, visual.referencedPositionBounds.minimum[2]);
            maximum.x = std::max(maximum.x, visual.referencedPositionBounds.maximum[0]);
            maximum.y = std::max(maximum.y, visual.referencedPositionBounds.maximum[1]);
            maximum.z = std::max(maximum.z, visual.referencedPositionBounds.maximum[2]);
        }
        ++scene.drawableVisuals;
        scene.drawableIndices += visual.indexCount;
    }
    if (scene.drawableVisuals == 0)
    {
        error = "real level has no validated POSITION0/FLOAT3 static visuals";
        return false;
    }

    const Vec3 center{
        (minimum.x + maximum.x) * 0.5f,
        (minimum.y + maximum.y) * 0.5f,
        (minimum.z + maximum.z) * 0.5f,
    };
    const Vec3 extent{maximum.x - minimum.x, maximum.y - minimum.y, maximum.z - minimum.z};
    const float radius = std::max({extent.x, extent.y, extent.z}) * 0.5f;
    if (!(radius > 0.0f) || !std::isfinite(radius))
    {
        error = "real level position bounds are degenerate";
        return false;
    }
    scene.center = center;
    scene.radius = radius;
    scene.eye = {center.x + radius * 1.15f, center.y + radius * 0.85f,
        center.z + radius * 1.15f};
    scene.view = makeLookAt(scene.eye, center);
    scene.projection = makePerspective(
        55.0f * 3.14159265358979323846f / 180.0f, 16.0f / 9.0f,
        std::max(0.1f, radius * 0.001f), radius * 6.0f);
    scene.uniformData.viewProjection = multiply(scene.projection, scene.view);
    scene.uniformData.minimumHeight = minimum.y;
    scene.uniformData.inverseHeightRange = 1.0f / std::max(1.0f, extent.y);

    IR4Resources& resources = backend.Resources();
    scene.vertexBuffers.reserve(scene.geometry.vertexBuffers.size());
    scene.pipelines.reserve(scene.geometry.vertexBuffers.size());
    scene.arefPipelines.reserve(scene.geometry.vertexBuffers.size());
    for (std::size_t i = 0; i < scene.geometry.vertexBuffers.size(); ++i)
    {
        const xray::metal::R4VertexBufferSummary& source = scene.geometry.vertexBuffers[i];
        R4BufferDesc bufferDesc;
        bufferDesc.size = source.dataSize;
        bufferDesc.storage = R4ResourceStorage::Private;
        bufferDesc.usage = R4BufferUsage::Vertex;
        bufferDesc.label = "R4 l05_bar static vertex buffer";
        std::shared_ptr<IR4Buffer> buffer = resources.CreateBuffer(bufferDesc, nullptr, 0, error);
        if (!buffer)
            return false;
        scene.vertexBuffers.emplace_back(std::move(buffer));

        const bool hasTextureMaterial = std::any_of(
            scene.textureMaterials.begin(), scene.textureMaterials.end(),
            [i](const auto& material) { return material.vertexBufferId == i; });
        const bool selectedTextureVertexBuffer = scene.productionMsc ?
            hasTextureMaterial : (!textureDiagnostic || hasTextureMaterial);
        if (!source.hasPositionFloat3 || !selectedTextureVertexBuffer)
        {
            scene.pipelines.emplace_back();
            scene.arefPipelines.emplace_back();
            continue;
        }
        std::uint32_t baseTexcoordOffset = 0;
        std::uint32_t hemiTexcoordOffset = 0;
        std::uint32_t tangentOffset = 0;
        std::uint32_t binormalOffset = 0;
        bool exactBaseUv = false;
        R4VertexFormat baseTexcoordFormat = R4VertexFormat::Short2;
        if (textureDiagnostic)
        {
            const auto selected = std::find_if(scene.textureMaterials.begin(), scene.textureMaterials.end(),
                [i](const auto& material) { return material.vertexBufferId == i; });
            baseTexcoordOffset = selected->uvOffset;
            baseTexcoordFormat = selected->uvFormat;
            hemiTexcoordOffset = selected->hemiUvOffset;
            tangentOffset = selected->tangentOffset;
            binormalOffset = selected->binormalOffset;
            exactBaseUv = selected->exactBaseUv;
            if (std::any_of(scene.textureMaterials.begin(), scene.textureMaterials.end(),
                [&scene, selected, i, baseTexcoordOffset, baseTexcoordFormat, exactBaseUv,
                    tangentOffset, binormalOffset, hemiTexcoordOffset](const auto& material) {
                        return material.vertexBufferId == i &&
                            (material.uvOffset != baseTexcoordOffset ||
                             material.uvFormat != baseTexcoordFormat ||
                             material.exactBaseUv != exactBaseUv ||
                             (scene.hemiDiagnostic &&
                                (material.hemiUvOffset != hemiTexcoordOffset ||
                                 material.normalOffset != selected->normalOffset)) ||
                             (exactBaseUv &&
                                (material.tangentOffset != tangentOffset ||
                                 material.binormalOffset != binormalOffset)));
                    }))
            {
                error = "selected visuals sharing a vertex buffer disagree on TEXCOORD0 layout";
                return false;
            }
            if (scene.hemiDiagnostic && baseTexcoordFormat != R4VertexFormat::Short2)
            {
                error = "hemi lightmap diagnostic currently requires TEXCOORD0 SHORT2";
                return false;
            }
        }
        const std::string shaderKey = std::string(scene.productionMsc ?
            "r4-deffer-base-lmh-flat-msc-v1/" : (textureDiagnostic ?
            "r4-level-texture-v1/" : "r4-level-flat-v1/")) +
            "stride=" + std::to_string(source.stride) +
            "/position=" + std::to_string(source.positionOffset) +
            (textureDiagnostic ? "/texcoord=" + std::to_string(baseTexcoordOffset) +
                "/type=" + std::to_string(static_cast<unsigned>(baseTexcoordFormat)) +
                (exactBaseUv ? "/exact-base=" + std::to_string(tangentOffset) + "," +
                    std::to_string(binormalOffset) : "") +
                (scene.hemiDiagnostic ? "/hemi=" + std::to_string(hemiTexcoordOffset) +
                    "/tangent=" + std::to_string(tangentOffset) +
                    "/binormal=" + std::to_string(binormalOffset) : "") : "");
        R4GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.label = scene.productionMsc ?
            "R4 deffer_base_lmh_flat production MSC G-buffer" : (textureDiagnostic ?
            "R4 l05_bar texture diagnostic pipeline" : "R4 l05_bar flat diagnostic pipeline");
        pipelineDesc.shaderKey = shaderKey.c_str();
        if (scene.productionMsc)
        {
            const auto selected = std::find_if(scene.textureMaterials.begin(),
                scene.textureMaterials.end(),
                [i](const auto& material) { return material.vertexBufferId == i; });
            constexpr std::uint32_t AttributeBase = 11;
            constexpr std::uint32_t VertexBufferIndex = 6;
            pipelineDesc.vertexMetallibPath = vertexMetallibPath.c_str();
            pipelineDesc.fragmentMetallibPath = fragmentMetallibPath.c_str();
            pipelineDesc.vertexAttributes = {
                {AttributeBase + 0, VertexBufferIndex, selected->normalOffset,
                    R4VertexFormat::UChar4Normalized},
                {AttributeBase + 1, VertexBufferIndex, selected->tangentOffset,
                    R4VertexFormat::UChar4Normalized},
                {AttributeBase + 2, VertexBufferIndex, selected->binormalOffset,
                    R4VertexFormat::UChar4Normalized},
                {AttributeBase + 3, VertexBufferIndex, selected->uvOffset,
                    R4VertexFormat::Short2},
                {AttributeBase + 4, VertexBufferIndex, selected->hemiUvOffset,
                    R4VertexFormat::Short2},
                {AttributeBase + 5, VertexBufferIndex, source.positionOffset,
                    R4VertexFormat::Float3},
            };
            pipelineDesc.vertexLayouts.push_back({VertexBufferIndex, source.stride});
            pipelineDesc.colorFormats = {
                R4PixelFormat::RGBA16Float,
                R4PixelFormat::RGBA8Unorm,
                R4PixelFormat::RGBA8Unorm,
                R4PixelFormat::RGBA16Float,
            };
        }
        else
        {
            pipelineDesc.mslSource = textureDiagnostic ? LevelTextureDiagnosticMsl : LevelDiagnosticMsl;
            pipelineDesc.vertexFunction = scene.hemiDiagnostic ? "r4_level_hemi_vs" :
                (textureDiagnostic ?
                    (baseTexcoordFormat == R4VertexFormat::Short4 ?
                        "r4_level_texture_short4_vs" :
                        (exactBaseUv ? "r4_level_texture_exact_vs" : "r4_level_texture_vs")) :
                    "r4_level_flat_vs");
            pipelineDesc.fragmentFunction = scene.hemiDiagnostic ? "r4_level_hemi_ps" :
                (textureDiagnostic ? "r4_level_texture_ps" : "r4_level_flat_ps");
            pipelineDesc.vertexAttributes.push_back(
                {0, 0, source.positionOffset, R4VertexFormat::Float3});
            if (textureDiagnostic)
                pipelineDesc.vertexAttributes.push_back(
                    {1, 0, baseTexcoordOffset, baseTexcoordFormat});
            if (scene.hemiDiagnostic)
                pipelineDesc.vertexAttributes.push_back(
                    {2, 0, hemiTexcoordOffset, R4VertexFormat::Short2});
            if (exactBaseUv)
            {
                pipelineDesc.vertexAttributes.push_back(
                    {3, 0, tangentOffset, R4VertexFormat::UChar4Normalized});
                pipelineDesc.vertexAttributes.push_back(
                    {4, 0, binormalOffset, R4VertexFormat::UChar4Normalized});
            }
            pipelineDesc.vertexLayouts.push_back({0, source.stride});
            pipelineDesc.colorFormat = R4PixelFormat::BGRA8Unorm;
        }
        pipelineDesc.depthFormat = scene.productionMsc ?
            R4PixelFormat::Depth32FloatStencil8 : R4PixelFormat::Depth32Float;
        pipelineDesc.depthCompare = R4CompareFunction::Less;
        pipelineDesc.depthWrite = true;
        if (scene.productionMsc)
        {
            pipelineDesc.stencil.enabled = true;
            pipelineDesc.stencil.frontFace.compare = R4CompareFunction::Always;
            pipelineDesc.stencil.frontFace.depthStencilPass = R4StencilOperation::Replace;
            pipelineDesc.stencil.frontFace.readMask = 0xff;
            pipelineDesc.stencil.frontFace.writeMask = 0x7f;
            pipelineDesc.stencil.backFace = pipelineDesc.stencil.frontFace;
        }
        pipelineDesc.cullMode = textureDiagnostic ? R4CullMode::Back : R4CullMode::None;
        // R4's normal scene phase uses D3DCULL_CCW, so authored clockwise
        // triangles are the visible front faces. Preserve that convention
        // explicitly instead of inheriting Metal's counter-clockwise default.
        if (textureDiagnostic)
            pipelineDesc.frontFace = R4Winding::Clockwise;
        std::shared_ptr<IR4GraphicsPipeline> pipeline =
            backend.CreateGraphicsPipeline(pipelineDesc, error);
        if (!pipeline)
            return false;
        scene.pipelines.emplace_back(std::move(pipeline));
        const bool needsArefPipeline = scene.productionAref &&
            std::any_of(scene.textureMaterials.begin(), scene.textureMaterials.end(),
                [i](const auto& material) {
                    return material.vertexBufferId == i &&
                        material.shaderName == "def_shaders\\def_aref";
                });
        if (needsArefPipeline)
        {
            const std::string arefShaderKey = shaderKey + "/alpha-test";
            const std::string arefPath = arefFragmentMetallibPath.string();
            pipelineDesc.label = "R4 deffer_base_lmh_aref_flat production MSC G-buffer";
            pipelineDesc.shaderKey = arefShaderKey.c_str();
            pipelineDesc.fragmentMetallibPath = arefPath.c_str();
            std::shared_ptr<IR4GraphicsPipeline> arefPipeline =
                backend.CreateGraphicsPipeline(pipelineDesc, error);
            if (!arefPipeline)
                return false;
            scene.arefPipelines.emplace_back(std::move(arefPipeline));
        }
        else
            scene.arefPipelines.emplace_back();
    }

    scene.indexBuffers.reserve(scene.geometry.indexBuffers.size());
    for (const xray::metal::R4IndexBufferSummary& source : scene.geometry.indexBuffers)
    {
        R4BufferDesc bufferDesc;
        bufferDesc.size = source.dataSize;
        bufferDesc.storage = R4ResourceStorage::Private;
        bufferDesc.usage = R4BufferUsage::Index;
        bufferDesc.label = "R4 l05_bar static index buffer";
        std::shared_ptr<IR4Buffer> buffer = resources.CreateBuffer(bufferDesc, nullptr, 0, error);
        if (!buffer)
            return false;
        scene.indexBuffers.emplace_back(std::move(buffer));
    }

    R4BufferDesc uniformDesc;
    uniformDesc.size = sizeof(scene.uniformData);
    uniformDesc.storage = R4ResourceStorage::Shared;
    uniformDesc.usage = R4BufferUsage::Uniform;
    uniformDesc.label = "R4 l05_bar flat camera uniforms";
    scene.uniforms = resources.CreateBuffer(
        uniformDesc, &scene.uniformData, sizeof(scene.uniformData), error);
    if (!scene.uniforms)
        return false;

    if (textureDiagnostic)
    {
        for (LevelScene::TextureMaterial& material : scene.textureMaterials)
        {
            LevelUniforms materialUniforms = scene.uniformData;
            materialUniforms.viewProjection = multiply(
                scene.uniformData.viewProjection, material.modelTransform);
            material.uniforms = resources.CreateBuffer(
                uniformDesc, &materialUniforms, sizeof(materialUniforms), error);
            if (!material.uniforms)
                return false;
        }
        for (LevelScene::TextureAsset& asset : scene.textureAssets)
        {
            asset.ddsTexture.descriptor.label = "R4 l05_bar real base texture";
            asset.texture = resources.CreateTexture2D(asset.ddsTexture.descriptor, error);
            if (!asset.texture)
                return false;
        }
        R4SamplerDesc samplerDesc;
        samplerDesc.minFilter = R4SamplerFilter::Linear;
        samplerDesc.magFilter = R4SamplerFilter::Linear;
        samplerDesc.mipFilter = R4SamplerFilter::Linear;
        samplerDesc.addressU = R4SamplerAddressMode::Repeat;
        samplerDesc.addressV = R4SamplerAddressMode::Repeat;
        samplerDesc.label = "R4 l05_bar real base sampler";
        scene.baseSampler = resources.CreateSampler(samplerDesc, error);
        if (!scene.baseSampler)
            return false;
        if (scene.hemiDiagnostic)
        {
            samplerDesc.addressU = R4SamplerAddressMode::ClampToEdge;
            samplerDesc.addressV = R4SamplerAddressMode::ClampToEdge;
            samplerDesc.label = "R4 l05_bar hemi lightmap sampler";
            scene.hemiSampler = resources.CreateSampler(samplerDesc, error);
            if (!scene.hemiSampler)
                return false;
        }

        if (scene.productionMsc)
        {
            auto createConstantBuffer = [&resources, &error](
                const void* bytes, std::size_t size, const char* label) {
                R4BufferDesc descriptor;
                descriptor.size = size;
                descriptor.storage = R4ResourceStorage::Shared;
                descriptor.usage = R4BufferUsage::Uniform;
                descriptor.label = label;
                return resources.CreateBuffer(descriptor, bytes, size, error);
            };
            for (LevelScene::TextureMaterial& material : scene.textureMaterials)
            {
                std::array<std::uint8_t, 160> vertexGlobals{};
                std::array<std::uint8_t, 176> fragmentGlobals{};
                std::array<std::uint8_t, 256> transforms{};
                const Matrix4 worldViewProjection = multiply(
                    scene.uniformData.viewProjection, material.modelTransform);
                const Matrix4 worldView = multiply(scene.view, material.modelTransform);
                std::memcpy(vertexGlobals.data() + 0, &worldViewProjection, sizeof(Matrix4));
                std::memcpy(vertexGlobals.data() + 64,
                    &scene.uniformData.viewProjection, sizeof(Matrix4));
                std::memcpy(transforms.data() + 0, &worldViewProjection, sizeof(Matrix4));
                std::memcpy(transforms.data() + 64, &worldView, sizeof(Matrix4));
                std::memcpy(transforms.data() + 128, &material.modelTransform, sizeof(Matrix4));
                const float materialClass[4] = {0.0f, 0.0f, 0.0f, 0.25f};
                std::memcpy(transforms.data() + 192, materialClass, sizeof(materialClass));

                material.mscVertexGlobals = createConstantBuffer(
                    vertexGlobals.data(), vertexGlobals.size(), "R4 MSC VS globals b0");
                material.mscVertexTransforms = createConstantBuffer(
                    transforms.data(), transforms.size(), "R4 MSC VS transforms b1");
                material.mscFragmentGlobals = createConstantBuffer(
                    fragmentGlobals.data(), fragmentGlobals.size(), "R4 MSC PS globals b0");
                material.mscFragmentTransforms = createConstantBuffer(
                    transforms.data(), transforms.size(), "R4 MSC PS transforms b1");
                if (!material.mscVertexGlobals || !material.mscVertexTransforms ||
                    !material.mscFragmentGlobals || !material.mscFragmentTransforms)
                    return false;
            }

            R4GraphicsPipelineDesc displayPipeline;
            displayPipeline.label = "R4 production G-buffer albedo inspection";
            displayPipeline.shaderKey = "r4-gbuffer-albedo-display-v1";
            displayPipeline.mslSource = GBufferDisplayMsl;
            displayPipeline.vertexFunction = "r4_gbuffer_display_vs";
            displayPipeline.fragmentFunction = "r4_gbuffer_display_ps";
            displayPipeline.colorFormat = R4PixelFormat::BGRA8Unorm;
            displayPipeline.cullMode = R4CullMode::None;
            scene.gBufferDisplayPipeline =
                backend.CreateGraphicsPipeline(displayPipeline, error);
            const std::uint16_t displayIndices[3] = {0, 1, 2};
            R4BufferDesc displayIndexDesc;
            displayIndexDesc.size = sizeof(displayIndices);
            displayIndexDesc.storage = R4ResourceStorage::Shared;
            displayIndexDesc.usage = R4BufferUsage::Index;
            displayIndexDesc.label = "R4 G-buffer display indices";
            scene.gBufferDisplayIndices = resources.CreateBuffer(
                displayIndexDesc, displayIndices, sizeof(displayIndices), error);
            if (!scene.gBufferDisplayPipeline || !scene.gBufferDisplayIndices)
                return false;

            if (scene.productionBaseLighting)
            {
                R4GraphicsPipelineDesc lightingPipeline;
                lightingPipeline.label = "R4 combine_1 production base lighting";
                lightingPipeline.shaderKey = "r4-combine-1-nomsaa-msc-v1";
                lightingPipeline.vertexMetallibPath = lightingVertexMetallibPath.c_str();
                lightingPipeline.fragmentMetallibPath = lightingFragmentMetallibPath.c_str();
                lightingPipeline.vertexAttributes = {
                    {11, 6, 0, R4VertexFormat::Float4},
                    {12, 6, 20, R4VertexFormat::Float2},
                };
                lightingPipeline.vertexLayouts.push_back({6, sizeof(CombineVertex)});
                lightingPipeline.colorFormats = {
                    R4PixelFormat::RGBA8Unorm,
                    R4PixelFormat::RGBA8Unorm,
                };
                lightingPipeline.blend.enabled = true;
                lightingPipeline.blend.sourceRgb = R4BlendFactor::OneMinusSourceAlpha;
                lightingPipeline.blend.destinationRgb = R4BlendFactor::SourceAlpha;
                lightingPipeline.blend.sourceAlpha = R4BlendFactor::OneMinusSourceAlpha;
                lightingPipeline.blend.destinationAlpha = R4BlendFactor::SourceAlpha;
                lightingPipeline.cullMode = R4CullMode::None;
                scene.baseLightingPipeline =
                    backend.CreateGraphicsPipeline(lightingPipeline, error);

                std::array<CombineVertex, 4> vertices{};
                R4BufferDesc vertexDesc;
                vertexDesc.size = sizeof(vertices);
                vertexDesc.storage = R4ResourceStorage::Shared;
                vertexDesc.usage = R4BufferUsage::Vertex;
                vertexDesc.label = "R4 combine_1 FVF::TL quad";
                scene.baseLightingVertices = resources.CreateBuffer(
                    vertexDesc, vertices.data(), sizeof(vertices), error);
                const std::uint16_t indices[6] = {0, 1, 2, 2, 1, 3};
                R4BufferDesc indexDesc;
                indexDesc.size = sizeof(indices);
                indexDesc.storage = R4ResourceStorage::Shared;
                indexDesc.usage = R4BufferUsage::Index;
                indexDesc.label = "R4 combine_1 quad indices";
                scene.baseLightingIndices = resources.CreateBuffer(
                    indexDesc, indices, sizeof(indices), error);

                std::array<std::uint8_t, 384> lightingGlobals{};
                std::array<std::uint8_t, 528> lightingStaticGlobals{};
                std::array<std::uint8_t, 48> lightingDynamic{};
                scene.baseLightingGlobals = createConstantBuffer(
                    lightingGlobals.data(), lightingGlobals.size(),
                    "R4 combine_1 globals b0");
                scene.baseLightingStaticGlobals = createConstantBuffer(
                    lightingStaticGlobals.data(), lightingStaticGlobals.size(),
                    "R4 combine_1 static globals b1");
                scene.baseLightingDynamicLight = createConstantBuffer(
                    lightingDynamic.data(), lightingDynamic.size(),
                    "R4 combine_1 dynamic light b2");

                auto createLightingTexture = [&resources, &error](
                    R4PixelFormat format, R4TextureType type, const char* label,
                    R4TextureUsage usage = R4TextureUsage::ShaderRead) {
                    R4Texture2DDesc descriptor;
                    descriptor.width = 1;
                    descriptor.height = 1;
                    descriptor.format = format;
                    descriptor.type = type;
                    descriptor.storage = R4ResourceStorage::Private;
                    descriptor.usage = usage;
                    descriptor.label = label;
                    return resources.CreateTexture2D(descriptor, error);
                };
                scene.lightingTonemap = createLightingTexture(
                    R4PixelFormat::R32Float, R4TextureType::Texture2D,
                    "R4 combine_1 previous-frame tonemap",
                    R4TextureUsage::ShaderRead | R4TextureUsage::RenderTarget);
                scene.lightingTonemapScratch = createLightingTexture(
                    R4PixelFormat::R32Float, R4TextureType::Texture2D,
                    "R4 luminance current-frame tonemap",
                    R4TextureUsage::ShaderRead | R4TextureUsage::RenderTarget);
                scene.lightingWhite = createLightingTexture(
                    R4PixelFormat::RGBA8Unorm, R4TextureType::Texture2D,
                    "R4 combine_1 white AO");
                scene.lightingBlack = createLightingTexture(
                    R4PixelFormat::RGBA8Unorm, R4TextureType::Texture2D,
                    "R4 combine_1 black IL");
                R4Texture2DDesc environmentDesc;
                environmentDesc.width = 64;
                environmentDesc.height = 64;
                environmentDesc.mipLevels = 7;
                environmentDesc.format = R4PixelFormat::RGBA32Float;
                environmentDesc.type = R4TextureType::TextureCube;
                environmentDesc.storage = R4ResourceStorage::Private;
                environmentDesc.usage = R4TextureUsage::ShaderRead;
                environmentDesc.label = "R4 combine_1 neutral environment cube";
                scene.lightingEnvironment =
                    resources.CreateTexture2D(environmentDesc, error);

                R4SamplerDesc noFilter;
                noFilter.minFilter = R4SamplerFilter::Nearest;
                noFilter.magFilter = R4SamplerFilter::Nearest;
                noFilter.mipFilter = R4SamplerFilter::Nearest;
                noFilter.addressU = R4SamplerAddressMode::ClampToEdge;
                noFilter.addressV = R4SamplerAddressMode::ClampToEdge;
                noFilter.addressW = R4SamplerAddressMode::ClampToEdge;
                noFilter.label = "R4 combine_1 smp_nofilter";
                scene.lightingNoFilterSampler = resources.CreateSampler(noFilter, error);
                R4SamplerDesc linear;
                linear.minFilter = R4SamplerFilter::Linear;
                linear.magFilter = R4SamplerFilter::Linear;
                linear.mipFilter = R4SamplerFilter::Linear;
                linear.addressU = R4SamplerAddressMode::Repeat;
                linear.addressV = R4SamplerAddressMode::Repeat;
                linear.addressW = R4SamplerAddressMode::Repeat;
                linear.label = "R4 combine_1 smp_linear";
                scene.lightingLinearSampler = resources.CreateSampler(linear, error);

                if (scene.productionLuminance)
                {
                    const auto luminancePath = [&luminanceShaderDirectory](const char* basename) {
                        return (luminanceShaderDirectory /
                            (std::string(basename) + ".metallib")).string();
                    };
                    const std::string buildVertexPath =
                        luminancePath("stub_notransform_build.vs");
                    const std::string filterVertexPath =
                        luminancePath("stub_notransform_filter.vs");
                    const std::string buildFragmentPath =
                        luminancePath("bloom_luminance_1.ps");
                    const std::string filterFragmentPath =
                        luminancePath("bloom_luminance_2.ps");
                    const std::string adaptFragmentPath =
                        luminancePath("bloom_luminance_3.ps");

                    R4GraphicsPipelineDesc buildPipeline;
                    buildPipeline.label = "R4 converted luminance 64x64 build";
                    buildPipeline.shaderKey = "r4-luminance-build-msc-v1";
                    buildPipeline.vertexMetallibPath = buildVertexPath.c_str();
                    buildPipeline.fragmentMetallibPath = buildFragmentPath.c_str();
                    buildPipeline.vertexAttributes = {
                        {11, 6, offsetof(LuminanceBuildVertex, position), R4VertexFormat::Float4},
                        {12, 6, offsetof(LuminanceBuildVertex, texcoord[0]), R4VertexFormat::Float2},
                        {13, 6, offsetof(LuminanceBuildVertex, texcoord[1]), R4VertexFormat::Float2},
                        {14, 6, offsetof(LuminanceBuildVertex, texcoord[2]), R4VertexFormat::Float2},
                        {15, 6, offsetof(LuminanceBuildVertex, texcoord[3]), R4VertexFormat::Float2},
                    };
                    buildPipeline.vertexLayouts.push_back({6, sizeof(LuminanceBuildVertex)});
                    buildPipeline.colorFormats = {R4PixelFormat::RGBA16Float};
                    buildPipeline.cullMode = R4CullMode::None;
                    scene.luminanceBuildPipeline =
                        backend.CreateGraphicsPipeline(buildPipeline, error);

                    auto makeFilterPipeline = [&](const char* label, const char* key,
                                                   const std::string& fragmentPath,
                                                   R4PixelFormat format) {
                        R4GraphicsPipelineDesc pipeline;
                        pipeline.label = label;
                        pipeline.shaderKey = key;
                        pipeline.vertexMetallibPath = filterVertexPath.c_str();
                        pipeline.fragmentMetallibPath = fragmentPath.c_str();
                        for (std::uint32_t index = 0; index < 8; ++index)
                        {
                            pipeline.vertexAttributes.push_back({12u + index, 6,
                                offsetof(LuminanceFilterVertex, texcoord) + index * sizeof(float) * 4,
                                R4VertexFormat::Float4});
                        }
                        pipeline.vertexAttributes.insert(pipeline.vertexAttributes.begin(),
                            {11, 6, offsetof(LuminanceFilterVertex, position), R4VertexFormat::Float4});
                        pipeline.vertexLayouts.push_back({6, sizeof(LuminanceFilterVertex)});
                        pipeline.colorFormats = {format};
                        pipeline.cullMode = R4CullMode::None;
                        return backend.CreateGraphicsPipeline(pipeline, error);
                    };
                    scene.luminanceFilterPipeline = makeFilterPipeline(
                        "R4 converted luminance 8x8 filter",
                        "r4-luminance-filter-msc-v1", filterFragmentPath,
                        R4PixelFormat::RGBA16Float);
                    scene.luminanceAdaptPipeline = makeFilterPipeline(
                        "R4 converted luminance 1x1 adaptation",
                        "r4-luminance-adapt-msc-v1", adaptFragmentPath,
                        R4PixelFormat::R32Float);

                    auto makeLuminanceTarget = [&resources, &error](
                        std::uint32_t extent, const char* label) {
                        R4Texture2DDesc descriptor;
                        descriptor.width = extent;
                        descriptor.height = extent;
                        descriptor.format = R4PixelFormat::RGBA16Float;
                        descriptor.storage = R4ResourceStorage::Private;
                        descriptor.usage =
                            R4TextureUsage::RenderTarget | R4TextureUsage::ShaderRead;
                        descriptor.label = label;
                        return resources.CreateTexture2D(descriptor, error);
                    };
                    scene.luminance64 = makeLuminanceTarget(64, "R4 luminance 64x64");
                    scene.luminance8 = makeLuminanceTarget(8, "R4 luminance 8x8");

                    std::array<LuminanceBuildVertex, 4> buildVertices{};
                    std::array<LuminanceFilterVertex, 4> filter64Vertices{};
                    std::array<LuminanceFilterVertex, 4> filter8Vertices{};
                    R4BufferDesc luminanceVertexDesc;
                    luminanceVertexDesc.storage = R4ResourceStorage::Shared;
                    luminanceVertexDesc.usage = R4BufferUsage::Vertex;
                    luminanceVertexDesc.size = sizeof(buildVertices);
                    luminanceVertexDesc.label = "R4 luminance build quad";
                    scene.luminanceBuildVertices = resources.CreateBuffer(luminanceVertexDesc,
                        buildVertices.data(), sizeof(buildVertices), error);
                    luminanceVertexDesc.size = sizeof(filter64Vertices);
                    luminanceVertexDesc.label = "R4 luminance 64-to-8 filter quad";
                    scene.luminanceFilter64Vertices = resources.CreateBuffer(luminanceVertexDesc,
                        filter64Vertices.data(), sizeof(filter64Vertices), error);
                    luminanceVertexDesc.label = "R4 luminance 8-to-1 filter quad";
                    scene.luminanceFilter8Vertices = resources.CreateBuffer(luminanceVertexDesc,
                        filter8Vertices.data(), sizeof(filter8Vertices), error);
                    const std::uint16_t luminanceIndices[6] = {0, 1, 2, 2, 1, 3};
                    R4BufferDesc luminanceIndexDesc;
                    luminanceIndexDesc.size = sizeof(luminanceIndices);
                    luminanceIndexDesc.storage = R4ResourceStorage::Shared;
                    luminanceIndexDesc.usage = R4BufferUsage::Index;
                    luminanceIndexDesc.label = "R4 luminance quad indices";
                    scene.luminanceIndices = resources.CreateBuffer(luminanceIndexDesc,
                        luminanceIndices, sizeof(luminanceIndices), error);

                    std::array<std::uint8_t, 528> staticGlobals{};
                    std::array<std::uint8_t, 176> adaptGlobals{};
                    scene.luminanceBuildVertexGlobals = createConstantBuffer(staticGlobals.data(),
                        staticGlobals.size(), "R4 luminance build VS static globals");
                    scene.luminanceBuildFragmentGlobals = createConstantBuffer(staticGlobals.data(),
                        staticGlobals.size(), "R4 luminance build PS static globals");
                    scene.luminanceFilter64VertexGlobals = createConstantBuffer(staticGlobals.data(),
                        staticGlobals.size(), "R4 luminance 64-to-8 VS static globals");
                    scene.luminanceFilter64FragmentGlobals = createConstantBuffer(staticGlobals.data(),
                        staticGlobals.size(), "R4 luminance 64-to-8 PS static globals");
                    scene.luminanceFilter8VertexGlobals = createConstantBuffer(staticGlobals.data(),
                        staticGlobals.size(), "R4 luminance 8-to-1 VS static globals");
                    scene.luminanceFilter8FragmentGlobals = createConstantBuffer(staticGlobals.data(),
                        staticGlobals.size(), "R4 luminance 8-to-1 PS static globals");
                    scene.luminanceAdaptGlobals = createConstantBuffer(adaptGlobals.data(),
                        adaptGlobals.size(), "R4 luminance MiddleGray globals");
                }

                if (scene.productionSun)
                {
                    const auto shaderPath = [&sunShaderDirectory](const char* basename) {
                        return (sunShaderDirectory /
                            (std::string(basename) + ".metallib")).string();
                    };
                    const std::string maskVertexPath = shaderPath("stub_notransform_t.vs");
                    const std::string maskFragmentPath =
                        shaderPath("accum_sun_mask_nomsaa.ps");
                    const std::string sunVertexPath = shaderPath("accum_sun.vs");
                    const std::string sunNearPath =
                        shaderPath("accum_sun_near_nomsaa_nominmax.ps");
                    const std::string sunFarPath =
                        shaderPath("accum_sun_far_nomsaa.ps");
                    const std::string shadowVertexPath =
                        shaderPath("shadow_direct_base.vs");
                    const std::string shadowFragmentPath =
                        shaderPath("shadow_direct_base_color.ps");
                    const std::string shadowArefVertexPath =
                        shaderPath("shadow_direct_base_aref.vs");
                    const std::string shadowArefFragmentPath =
                        shaderPath("shadow_direct_base_aref_color.ps");

                    R4GraphicsPipelineDesc maskPipeline;
                    maskPipeline.label = "R4 converted direct-sun stencil mask";
                    maskPipeline.shaderKey = "r4-sun-mask-nomsaa-msc-v1";
                    maskPipeline.vertexMetallibPath = maskVertexPath.c_str();
                    maskPipeline.fragmentMetallibPath = maskFragmentPath.c_str();
                    maskPipeline.vertexAttributes = {
                        {11, 6, 0, R4VertexFormat::Float4},
                        {12, 6, 20, R4VertexFormat::Float2},
                        {13, 6, 16, R4VertexFormat::UChar4Normalized},
                    };
                    maskPipeline.vertexLayouts.push_back({6, sizeof(CombineVertex)});
                    maskPipeline.colorFormats = {R4PixelFormat::RGBA16Float};
                    maskPipeline.colorWriteMasks = {0};
                    maskPipeline.depthFormat = R4PixelFormat::Depth32FloatStencil8;
                    maskPipeline.depthCompare = R4CompareFunction::Always;
                    maskPipeline.cullMode = R4CullMode::None;
                    maskPipeline.stencil.enabled = true;
                    maskPipeline.stencil.frontFace.compare = R4CompareFunction::LessEqual;
                    maskPipeline.stencil.frontFace.depthStencilPass =
                        R4StencilOperation::Replace;
                    maskPipeline.stencil.frontFace.readMask = 0x01;
                    maskPipeline.stencil.frontFace.writeMask = 0xff;
                    maskPipeline.stencil.backFace = maskPipeline.stencil.frontFace;
                    scene.sunMaskPipeline =
                        backend.CreateGraphicsPipeline(maskPipeline, error);

                    auto createSunPipeline = [&](const char* label, const char* key,
                                                 const std::string& fragmentPath,
                                                 R4CompareFunction depthCompare,
                                                 std::uint32_t stencilWriteMask,
                                                 R4StencilOperation stencilPass) {
                        R4GraphicsPipelineDesc pipeline;
                        pipeline.label = label;
                        pipeline.shaderKey = key;
                        pipeline.vertexMetallibPath = sunVertexPath.c_str();
                        pipeline.fragmentMetallibPath = fragmentPath.c_str();
                        pipeline.vertexAttributes = {
                            {11, 6, 0, R4VertexFormat::Float4},
                        };
                        pipeline.vertexLayouts.push_back({6, sizeof(SunVolumeVertex)});
                        pipeline.colorFormats = {R4PixelFormat::RGBA16Float};
                        pipeline.depthFormat = R4PixelFormat::Depth32FloatStencil8;
                        pipeline.depthCompare = depthCompare;
                        pipeline.cullMode = R4CullMode::None;
                        pipeline.stencil.enabled = true;
                        pipeline.stencil.frontFace.compare = R4CompareFunction::LessEqual;
                        pipeline.stencil.frontFace.depthStencilPass = stencilPass;
                        pipeline.stencil.frontFace.readMask = 0xff;
                        pipeline.stencil.frontFace.writeMask = stencilWriteMask;
                        pipeline.stencil.backFace = pipeline.stencil.frontFace;
                        return backend.CreateGraphicsPipeline(pipeline, error);
                    };
                    scene.sunNearPipeline = createSunPipeline(
                        "R4 converted direct-sun near cascade",
                        "r4-sun-near-nomsaa-nominmax-msc-v1", sunNearPath,
                        R4CompareFunction::GreaterEqual, 0xfe, R4StencilOperation::Zero);
                    scene.sunFarPipeline = createSunPipeline(
                        "R4 converted direct-sun far cascade",
                        "r4-sun-far-nomsaa-msc-v1", sunFarPath,
                        R4CompareFunction::Always, 0x00, R4StencilOperation::Keep);

                    const xray::metal::R4VertexBufferSummary& shadowVertexBuffer =
                        !scene.textureMaterials.empty() ?
                            scene.geometry.vertexBuffers[scene.textureMaterials.front().vertexBufferId] :
                            (!scene.geometry.vertexBuffers.empty() ? scene.geometry.vertexBuffers.front() :
                                xray::metal::R4VertexBufferSummary{});
                    if (!scene.textureMaterials.empty() &&
                        std::any_of(scene.textureMaterials.begin(), scene.textureMaterials.end(),
                            [&scene, &shadowVertexBuffer](const auto& material) {
                                const auto& buffer = scene.geometry.vertexBuffers[material.vertexBufferId];
                                return buffer.stride != shadowVertexBuffer.stride ||
                                    buffer.positionOffset != shadowVertexBuffer.positionOffset;
                            }))
                    {
                        error = "controlled MSC material set has incompatible shadow POSITION0 layouts";
                        return false;
                    }
                    R4GraphicsPipelineDesc shadowPipeline;
                    shadowPipeline.label = "R4 converted direct-sun shadow map";
                    shadowPipeline.shaderKey = "r4-shadow-direct-base-r32-msc-v1";
                    shadowPipeline.vertexMetallibPath = shadowVertexPath.c_str();
                    shadowPipeline.fragmentMetallibPath = shadowFragmentPath.c_str();
                    shadowPipeline.vertexAttributes = {{
                        11, 6, shadowVertexBuffer.positionOffset, R4VertexFormat::Float3,
                    }};
                    shadowPipeline.vertexLayouts.push_back(
                        {6, shadowVertexBuffer.stride});
                    shadowPipeline.colorFormats = {R4PixelFormat::R32Float};
                    shadowPipeline.depthFormat = R4PixelFormat::Depth32Float;
                    shadowPipeline.depthCompare = R4CompareFunction::Less;
                    shadowPipeline.depthWrite = true;
                    shadowPipeline.cullMode = R4CullMode::Back;
                    shadowPipeline.frontFace = R4Winding::Clockwise;
                    scene.sunShadowPipeline =
                        backend.CreateGraphicsPipeline(shadowPipeline, error);

                    const auto arefMaterial = std::find_if(scene.textureMaterials.begin(),
                        scene.textureMaterials.end(), [](const auto& material) {
                            return material.shaderName == "def_shaders\\def_aref";
                        });
                    if (arefMaterial != scene.textureMaterials.end())
                    {
                        const xray::metal::R4VertexBufferSummary& arefVertexBuffer =
                            scene.geometry.vertexBuffers[arefMaterial->vertexBufferId];
                        if (!arefMaterial->exactBaseUv ||
                            arefMaterial->uvFormat != R4VertexFormat::Short2 ||
                            arefMaterial->normalOffset == 0u ||
                            arefMaterial->tangentOffset == 0u ||
                            arefMaterial->binormalOffset == 0u ||
                            std::any_of(scene.textureMaterials.begin(), scene.textureMaterials.end(),
                                [&arefVertexBuffer, &arefMaterial, &scene](const auto& material) {
                                    if (material.shaderName != "def_shaders\\def_aref") return false;
                                    const auto& vertex = scene.geometry.vertexBuffers[material.vertexBufferId];
                                    return vertex.stride != arefVertexBuffer.stride ||
                                        vertex.positionOffset != arefVertexBuffer.positionOffset ||
                                        material.uvOffset != arefMaterial->uvOffset ||
                                        material.normalOffset != arefMaterial->normalOffset ||
                                        material.tangentOffset != arefMaterial->tangentOffset ||
                                        material.binormalOffset != arefMaterial->binormalOffset ||
                                        !material.exactBaseUv || material.uvFormat != R4VertexFormat::Short2;
                                }))
                        {
                            error = "alpha-tested shadow casters do not match the R4 packed static vertex ABI";
                            return false;
                        }
                        R4GraphicsPipelineDesc arefShadowPipeline = shadowPipeline;
                        arefShadowPipeline.label = "R4 converted direct-sun alpha-tested shadow map";
                        arefShadowPipeline.shaderKey = "r4-shadow-direct-base-aref-r32-msc-v1";
                        arefShadowPipeline.vertexMetallibPath = shadowArefVertexPath.c_str();
                        arefShadowPipeline.fragmentMetallibPath = shadowArefFragmentPath.c_str();
                        arefShadowPipeline.vertexAttributes = {{
                            {11, 6, arefMaterial->normalOffset, R4VertexFormat::UChar4Normalized},
                            {12, 6, arefMaterial->tangentOffset, R4VertexFormat::UChar4Normalized},
                            {13, 6, arefMaterial->binormalOffset, R4VertexFormat::UChar4Normalized},
                            {14, 6, arefMaterial->uvOffset, R4VertexFormat::Short2},
                            {15, 6, arefVertexBuffer.positionOffset, R4VertexFormat::Float3},
                        }};
                        arefShadowPipeline.vertexLayouts = {{6, arefVertexBuffer.stride}};
                        scene.sunShadowArefPipeline = backend.CreateGraphicsPipeline(arefShadowPipeline, error);
                    }

                    std::array<CombineVertex, 4> maskVertices{};
                    R4BufferDesc maskVertexDesc;
                    maskVertexDesc.size = sizeof(maskVertices);
                    maskVertexDesc.storage = R4ResourceStorage::Shared;
                    maskVertexDesc.usage = R4BufferUsage::Vertex;
                    maskVertexDesc.label = "R4 direct-sun FVF::TL mask quad";
                    scene.sunMaskVertices = resources.CreateBuffer(maskVertexDesc,
                        maskVertices.data(), sizeof(maskVertices), error);
                    const std::uint16_t quadIndices[6] = {0, 1, 2, 2, 1, 3};
                    R4BufferDesc maskIndexDesc;
                    maskIndexDesc.size = sizeof(quadIndices);
                    maskIndexDesc.storage = R4ResourceStorage::Shared;
                    maskIndexDesc.usage = R4BufferUsage::Index;
                    maskIndexDesc.label = "R4 direct-sun mask indices";
                    scene.sunMaskIndices = resources.CreateBuffer(maskIndexDesc,
                        quadIndices, sizeof(quadIndices), error);

                    static constexpr SunVolumeVertex VolumeVertices[8] = {
                        {{-1.0f, -1.0f, 0.70f, 1.0f}},
                        {{-1.0f, -1.0f, 1.00f, 1.0f}},
                        {{-1.0f,  1.0f, 1.00f, 1.0f}},
                        {{-1.0f,  1.0f, 0.70f, 1.0f}},
                        {{ 1.0f,  1.0f, 1.00f, 1.0f}},
                        {{ 1.0f,  1.0f, 0.70f, 1.0f}},
                        {{ 1.0f, -1.0f, 1.00f, 1.0f}},
                        {{ 1.0f, -1.0f, 0.70f, 1.0f}},
                    };
                    static constexpr std::uint16_t VolumeIndices[48] = {
                        3,2,1, 3,1,0, 7,6,5, 5,6,4,
                        3,5,2, 4,2,5, 1,6,7, 7,0,1,
                        5,3,0, 7,5,0, 1,4,6, 2,4,1,
                    };
                    R4BufferDesc volumeVertexDesc;
                    volumeVertexDesc.size = sizeof(VolumeVertices);
                    volumeVertexDesc.storage = R4ResourceStorage::Shared;
                    volumeVertexDesc.usage = R4BufferUsage::Vertex;
                    volumeVertexDesc.label = "R4 direct-sun cascade cuboid";
                    scene.sunVolumeVertices = resources.CreateBuffer(volumeVertexDesc,
                        VolumeVertices, sizeof(VolumeVertices), error);
                    R4BufferDesc volumeIndexDesc;
                    volumeIndexDesc.size = sizeof(VolumeIndices);
                    volumeIndexDesc.storage = R4ResourceStorage::Shared;
                    volumeIndexDesc.usage = R4BufferUsage::Index;
                    volumeIndexDesc.label = "R4 direct-sun cascade cuboid indices";
                    scene.sunVolumeIndices = resources.CreateBuffer(volumeIndexDesc,
                        VolumeIndices, sizeof(VolumeIndices), error);

                    std::array<std::uint8_t, 224> sunVertexGlobals{};
                    std::array<std::uint8_t, 256> sunVertexTransforms{};
                    std::array<std::uint8_t, 432> sunNearFragmentGlobals{};
                    std::array<std::uint8_t, 444> sunFarFragmentGlobals{};
                    std::array<std::uint8_t, 48> sunMaskDynamic{};
                    scene.sunVertexGlobals = createConstantBuffer(
                        sunVertexGlobals.data(), sunVertexGlobals.size(),
                        "R4 direct-sun VS globals b0");
                    scene.sunVertexTransforms = createConstantBuffer(
                        sunVertexTransforms.data(), sunVertexTransforms.size(),
                        "R4 direct-sun VS transforms b1");
                    scene.sunNearFragmentGlobals = createConstantBuffer(
                        sunNearFragmentGlobals.data(), sunNearFragmentGlobals.size(),
                        "R4 direct-sun near PS globals b0");
                    scene.sunFarFragmentGlobals = createConstantBuffer(
                        sunFarFragmentGlobals.data(), sunFarFragmentGlobals.size(),
                        "R4 direct-sun far PS globals b0");
                    scene.sunMaskDynamicLight = createConstantBuffer(
                        sunMaskDynamic.data(), sunMaskDynamic.size(),
                        "R4 direct-sun mask dynamic light b0");
                    std::array<std::uint8_t, 256> shadowTransforms{};
                    scene.sunNearShadowTransforms = createConstantBuffer(
                        shadowTransforms.data(), shadowTransforms.size(),
                        "R4 direct-sun near shadow transforms b0");
                    scene.sunShadowTransforms = createConstantBuffer(
                        shadowTransforms.data(), shadowTransforms.size(),
                        "R4 direct-sun shadow transforms b0");
                    for (LevelScene::TextureMaterial& material : scene.textureMaterials)
                    {
                        material.sunNearShadowTransforms = createConstantBuffer(
                            shadowTransforms.data(), shadowTransforms.size(),
                            "R4 direct-sun material near shadow transforms b0");
                        material.sunShadowTransforms = createConstantBuffer(
                            shadowTransforms.data(), shadowTransforms.size(),
                            "R4 direct-sun material shadow transforms b0");
                    }

                    scene.sunMaskTexture = createLightingTexture(
                        R4PixelFormat::RGBA8Unorm, R4TextureType::Texture2D,
                        "R4 controlled white sun mask");
                    scene.sunSssTexture = createLightingTexture(
                        R4PixelFormat::RGBA8Unorm, R4TextureType::Texture2D,
                        "R4 controlled black screen-space shadow");
                    R4Texture2DDesc shadowColorDesc;
                    shadowColorDesc.width = 2048;
                    shadowColorDesc.height = 2048;
                    shadowColorDesc.format = R4PixelFormat::R32Float;
                    shadowColorDesc.storage = R4ResourceStorage::Private;
                    shadowColorDesc.usage =
                        R4TextureUsage::RenderTarget | R4TextureUsage::ShaderRead;
                    shadowColorDesc.label = "R4 direct-sun near R32 shadow map";
                    scene.sunNearShadowTexture =
                        resources.CreateTexture2D(shadowColorDesc, error);
                    shadowColorDesc.label = "R4 direct-sun far R32 shadow map";
                    scene.sunShadowTexture =
                        resources.CreateTexture2D(shadowColorDesc, error);
                    R4Texture2DDesc shadowDepthDesc;
                    shadowDepthDesc.width = 2048;
                    shadowDepthDesc.height = 2048;
                    shadowDepthDesc.format = R4PixelFormat::Depth32Float;
                    shadowDepthDesc.storage = R4ResourceStorage::Private;
                    shadowDepthDesc.usage = R4TextureUsage::RenderTarget;
                    shadowDepthDesc.label = "R4 direct-sun shadow depth target";
                    scene.sunShadowDepthTarget =
                        resources.CreateTexture2D(shadowDepthDesc, error);

                    R4SamplerDesc shadowSampler;
                    shadowSampler.minFilter = R4SamplerFilter::Linear;
                    shadowSampler.magFilter = R4SamplerFilter::Linear;
                    shadowSampler.mipFilter = R4SamplerFilter::Nearest;
                    shadowSampler.addressU = R4SamplerAddressMode::ClampToBorder;
                    shadowSampler.addressV = R4SamplerAddressMode::ClampToBorder;
                    shadowSampler.addressW = R4SamplerAddressMode::ClampToBorder;
                    shadowSampler.compareFunction = R4SamplerCompareFunction::LessEqual;
                    shadowSampler.borderColor = R4SamplerBorderColor::OpaqueWhite;
                    shadowSampler.label = "R4 direct-sun comparison sampler";
                    scene.sunShadowSampler = resources.CreateSampler(shadowSampler, error);
                }

                if (!scene.baseLightingPipeline || !scene.baseLightingVertices ||
                    !scene.baseLightingIndices || !scene.baseLightingGlobals ||
                    !scene.baseLightingStaticGlobals || !scene.baseLightingDynamicLight ||
                    !scene.lightingTonemap || !scene.lightingTonemapScratch ||
                    !scene.lightingWhite || !scene.lightingBlack ||
                    !scene.lightingEnvironment || !scene.lightingNoFilterSampler ||
                    !scene.lightingLinearSampler)
                    return false;
                if (scene.productionLuminance &&
                    (!scene.luminanceBuildPipeline || !scene.luminanceFilterPipeline ||
                     !scene.luminanceAdaptPipeline || !scene.luminanceBuildVertices ||
                     !scene.luminanceFilter64Vertices || !scene.luminanceFilter8Vertices ||
                     !scene.luminanceIndices || !scene.luminance64 || !scene.luminance8 ||
                     !scene.luminanceBuildVertexGlobals ||
                     !scene.luminanceBuildFragmentGlobals ||
                     !scene.luminanceFilter64VertexGlobals ||
                     !scene.luminanceFilter64FragmentGlobals ||
                     !scene.luminanceFilter8VertexGlobals ||
                     !scene.luminanceFilter8FragmentGlobals ||
                     !scene.luminanceAdaptGlobals))
                    return false;
                if (scene.productionSun &&
                    (!scene.sunMaskPipeline || !scene.sunNearPipeline ||
                     !scene.sunFarPipeline || !scene.sunShadowPipeline ||
                     !scene.sunMaskVertices ||
                     !scene.sunMaskIndices || !scene.sunVolumeVertices ||
                     !scene.sunVolumeIndices || !scene.sunVertexGlobals ||
                     !scene.sunVertexTransforms || !scene.sunNearFragmentGlobals ||
                     !scene.sunFarFragmentGlobals ||
                     !scene.sunMaskDynamicLight || !scene.sunNearShadowTransforms ||
                     !scene.sunShadowTransforms ||
                     !scene.sunMaskTexture || !scene.sunNearShadowTexture || !scene.sunShadowTexture ||
                     !scene.sunShadowDepthTarget || !scene.sunSssTexture ||
                     !scene.sunShadowSampler))
                    return false;
                if (scene.productionSun && std::any_of(
                        scene.textureMaterials.begin(), scene.textureMaterials.end(),
                        [](const auto& material) {
                            return !material.sunNearShadowTransforms || !material.sunShadowTransforms;
                        }))
                    return false;
                if (scene.productionSun && std::any_of(
                        scene.textureMaterials.begin(), scene.textureMaterials.end(),
                        [](const auto& material) {
                            return material.shaderName == "def_shaders\\def_aref";
                        }) && !scene.sunShadowArefPipeline)
                    return false;
            }
        }
    }
    return true;
}

bool updateBaseLightingConstants(
    IR4Backend& backend,
    LevelScene& scene,
    std::uint32_t width,
    std::uint32_t height,
    const R4EnvironmentSnapshot* environment,
    std::string& error)
{
    std::array<std::uint8_t, 384> globals{};
    std::array<std::uint8_t, 528> staticGlobals{};
    std::array<std::uint8_t, 48> dynamicLight{};
    auto putFloat4 = [](auto& bytes, std::size_t offset,
                         float x, float y, float z, float w) {
        const float value[4] = {x, y, z, w};
        std::memcpy(bytes.data() + offset, value, sizeof(value));
    };

    const auto finite3 = [](const std::array<float, 3>& value) {
        return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
    };
    const auto finite4 = [](const std::array<float, 4>& value) {
        return std::isfinite(value[0]) && std::isfinite(value[1]) &&
            std::isfinite(value[2]) && std::isfinite(value[3]);
    };
    const auto normalizeOr = [](const Vec3& value, const Vec3& fallback) {
        const float length2 = dot(value, value);
        return std::isfinite(length2) && length2 > 1.0e-12f ?
            normalize(value) : fallback;
    };
    const bool liveEnvironment = environment && environment->valid &&
        finite3(environment->sunColor) && finite3(environment->sunDirection) &&
        finite3(environment->adaptedSunColor) && finite3(environment->adaptedSunDirection) &&
        finite3(environment->fogColor) && finite3(environment->skyColor) &&
        finite4(environment->hemiColor) && finite4(environment->combineAmbient) &&
        finite4(environment->combineEnvironment) && finite4(environment->hmodelStuff) &&
        finite4(environment->imageCorrections) && finite4(environment->colorGrading) &&
        std::isfinite(environment->globalTime) && std::isfinite(environment->fogNear) &&
        std::isfinite(environment->fogFar) && std::isfinite(environment->fogDensity) &&
        std::isfinite(environment->rainDensity) && std::isfinite(environment->wetnessFactor) &&
        std::isfinite(environment->skyRotation) && environment->fogFar > environment->fogNear;
    const std::array<float, 4> combineAmbient = liveEnvironment ? environment->combineAmbient :
        std::array<float, 4>{0.50f, 0.52f, 0.56f, 0.0f};
    const std::array<float, 4> combineEnvironment = liveEnvironment ? environment->combineEnvironment :
        std::array<float, 4>{0.70f, 0.76f, 0.82f, 0.0f};
    const std::array<float, 4> hmodelStuff = liveEnvironment ? environment->hmodelStuff :
        std::array<float, 4>{1.0f, 1.0f, 0.0f, 0.0f};
    const std::array<float, 3> rawSunColor = liveEnvironment ? environment->sunColor :
        std::array<float, 3>{1.0f, 0.96f, 0.86f};
    const std::array<float, 3> rawSunDirection = liveEnvironment ? environment->sunDirection :
        std::array<float, 3>{-0.35f, -0.80f, 0.48f};
    const std::array<float, 3> adaptedSunColor = liveEnvironment ? environment->adaptedSunColor :
        std::array<float, 3>{1.0f, 0.96f, 0.86f};
    const std::array<float, 3> adaptedSunDirection = liveEnvironment ? environment->adaptedSunDirection :
        std::array<float, 3>{-0.35f, -0.80f, 0.48f};
    const Vec3 rawSunDirectionView = normalizeOr(transformDirection(scene.view,
        {rawSunDirection[0], rawSunDirection[1], rawSunDirection[2]}), {-0.35f, -0.80f, 0.48f});
    const Vec3 adaptedSunDirectionWorld = normalizeOr(
        {adaptedSunDirection[0], adaptedSunDirection[1], adaptedSunDirection[2]},
        {-0.35f, -0.80f, 0.48f});
    const Vec3 adaptedSunDirectionView = normalizeOr(transformDirection(scene.view,
        adaptedSunDirectionWorld), {-0.35f, -0.80f, 0.48f});

    putFloat4(globals, 208, combineEnvironment[0], combineEnvironment[1],
        combineEnvironment[2], combineEnvironment[3]);
    putFloat4(globals, 224, hmodelStuff[0], hmodelStuff[1], hmodelStuff[2], hmodelStuff[3]);
    putFloat4(globals, 288, 1.0f, 0.0f, 0.0f, 0.0f);
    putFloat4(globals, 320, 1.0f, 0.0f, 0.0f, 0.0f);
    if (liveEnvironment)
        putFloat4(globals, 368, environment->skyColor[0], environment->skyColor[1],
            environment->skyColor[2], environment->skyRotation);
    else
        putFloat4(globals, 368, 0.025f, 0.055f, 0.105f, 0.0f);

    const Matrix4 inverseView = inverseRigidTransform(scene.view);
    std::memcpy(staticGlobals.data() + 0, &scene.view, sizeof(Matrix4));
    std::memcpy(staticGlobals.data() + 64, &inverseView, sizeof(Matrix4));
    std::memcpy(staticGlobals.data() + 128, &scene.projection, sizeof(Matrix4));
    std::memcpy(staticGlobals.data() + 192,
        &scene.uniformData.viewProjection, sizeof(Matrix4));
    if (liveEnvironment)
    {
        const Matrix4& fullTransform = scene.uniformData.viewProjection;
        Vec3 fogPlane{
            -(fullTransform.elements[3] + fullTransform.elements[2]),
            -(fullTransform.elements[7] + fullTransform.elements[6]),
            -(fullTransform.elements[11] + fullTransform.elements[10]),
        };
        const float fogPlaneLength = std::sqrt(dot(fogPlane, fogPlane));
        if (fogPlaneLength > 1.0e-6f && std::isfinite(fogPlaneLength))
        {
            const float inverseLength = -1.0f / fogPlaneLength;
            fogPlane = {fogPlane.x * inverseLength, fogPlane.y * inverseLength,
                fogPlane.z * inverseLength};
            const float fogPlaneW = -(fullTransform.elements[15] + fullTransform.elements[14]) * inverseLength;
            const float fogRangeInverse = 1.0f / (environment->fogFar - environment->fogNear);
            putFloat4(staticGlobals, 256, environment->globalTime, environment->globalTime * 10.0f,
                environment->globalTime / 10.0f, std::sin(environment->globalTime));
            putFloat4(staticGlobals, 272, -fogPlane.x * fogRangeInverse,
                -fogPlane.y * fogRangeInverse, -fogPlane.z * fogRangeInverse,
                1.0f - (fogPlaneW - environment->fogNear) * fogRangeInverse);
            putFloat4(staticGlobals, 288, -environment->fogNear * fogRangeInverse,
                environment->fogNear, environment->fogFar, fogRangeInverse);
        }
        putFloat4(staticGlobals, 304, environment->fogColor[0], environment->fogColor[1],
            environment->fogColor[2], environment->fogDensity);
    }
    else
        putFloat4(staticGlobals, 304, 0.025f, 0.055f, 0.105f, 0.0f);
    putFloat4(staticGlobals, 320, combineAmbient[0], combineAmbient[1],
        combineAmbient[2], combineAmbient[3]);
    putFloat4(staticGlobals, 336, rawSunColor[0], rawSunColor[1], rawSunColor[2], 0.0f);
    putFloat4(staticGlobals, 352, rawSunDirection[0], rawSunDirection[1], rawSunDirection[2], 0.0f);
    putFloat4(staticGlobals, 368, liveEnvironment ? environment->hemiColor[0] : 0.45f,
        liveEnvironment ? environment->hemiColor[1] : 0.50f,
        liveEnvironment ? environment->hemiColor[2] : 0.58f,
        liveEnvironment ? environment->hemiColor[3] : 0.0f);
    putFloat4(staticGlobals, 384, scene.eye.x, scene.eye.y, scene.eye.z, 0.0f);
    const float verticalTan = -std::tan(55.0f * 3.14159265358979323846f / 360.0f);
    const float horizontalTan = -verticalTan * (16.0f / 9.0f);
    putFloat4(staticGlobals, 400,
        horizontalTan, verticalTan,
        (2.0f * horizontalTan) / static_cast<float>(width),
        (2.0f * verticalTan) / static_cast<float>(height));
    putFloat4(staticGlobals, 416,
        static_cast<float>(width), static_cast<float>(height),
        1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height));
    putFloat4(staticGlobals, 432, rawSunDirectionView.x, rawSunDirectionView.y,
        rawSunDirectionView.z, 0.0f);
    // shared/common.h: rain_params is after parallax.  Preserve the live
    // mixer values verbatim so puddle/terrain permutations decide selectively
    // where wetness appears; no generic postprocess gloss is introduced.
    putFloat4(staticGlobals, 464,
        liveEnvironment ? environment->rainDensity : 0.0f,
        liveEnvironment ? environment->wetnessFactor : 0.0f,
        0.0f, 0.0f);
    const std::array<float, 4> imageCorrections = liveEnvironment ?
        environment->imageCorrections : std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f};
    const std::array<float, 4> colorGrading = liveEnvironment ?
        environment->colorGrading : std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f};
    putFloat4(staticGlobals, 480, imageCorrections[0], imageCorrections[1],
        imageCorrections[2], imageCorrections[3]);
    putFloat4(staticGlobals, 496, colorGrading[0], colorGrading[1],
        colorGrading[2], colorGrading[3]);

    const float adaptedLuminance = std::max(0.0f, 0.30f * adaptedSunColor[0] +
        0.48f * adaptedSunColor[1] + 0.22f * adaptedSunColor[2]);
    putFloat4(dynamicLight, 0, adaptedSunColor[0], adaptedSunColor[1], adaptedSunColor[2],
        adaptedLuminance);
    putFloat4(dynamicLight, 32, adaptedSunDirectionView.x, adaptedSunDirectionView.y,
        adaptedSunDirectionView.z, 0.0f);

    constexpr float JitterTextureSize = 64.0f;
    const float scaleX = static_cast<float>(width) / JitterTextureSize;
    const float scaleY = static_cast<float>(height) / JitterTextureSize;
    const std::array<CombineVertex, 4> vertices = {{
        {{-1.0f, 1.0f, 0.0f, 1.0f}, 0u, {0.0f, scaleY}},
        {{-1.0f, -1.0f, 0.0f, 0.0f}, 0u, {0.0f, 0.0f}},
        {{1.0f, 1.0f, 1.0f, 1.0f}, 0u, {scaleX, scaleY}},
        {{1.0f, -1.0f, 1.0f, 0.0f}, 0u, {scaleX, 0.0f}},
    }};

    IR4Resources& resources = backend.Resources();
    if (!resources.UpdateBuffer(
            scene.baseLightingGlobals, 0, globals.data(), globals.size(), error) ||
        !resources.UpdateBuffer(scene.baseLightingStaticGlobals, 0,
            staticGlobals.data(), staticGlobals.size(), error) ||
        !resources.UpdateBuffer(scene.baseLightingDynamicLight, 0,
            dynamicLight.data(), dynamicLight.size(), error) ||
        !resources.UpdateBuffer(scene.baseLightingVertices, 0,
            vertices.data(), sizeof(vertices), error))
        return false;
    if (!scene.productionSun)
        return true;

    const float maskDepth = 1.0e-4f;
    const std::array<CombineVertex, 4> maskVertices = {{
        {{0.0f, static_cast<float>(height), maskDepth, 1.0f},
            0xffffffffu, {0.0f, 1.0f}},
        {{0.0f, 0.0f, maskDepth, 1.0f}, 0xffffffffu, {0.0f, 0.0f}},
        {{static_cast<float>(width), static_cast<float>(height), maskDepth, 1.0f},
            0xffffffffu, {1.0f, 1.0f}},
        {{static_cast<float>(width), 0.0f, maskDepth, 1.0f},
            0xffffffffu, {1.0f, 0.0f}},
    }};
    const Matrix4 identity{{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    }};
    const Matrix4 screenTexgen{{
        0.5f, 0.0f, 0.0f, 0.0f,
        0.0f, -0.5f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.5f, 0.5f, 0.0f, 1.0f,
    }};
    Matrix4 constantSunMask{};
    constantSunMask.elements[12] = 0.5f;
    constantSunMask.elements[13] = 0.5f;
    constantSunMask.elements[15] = 1.0f;
    const Vec3 sunDirection = adaptedSunDirectionWorld;
    const float lightDistance = scene.radius * 3.0f;
    const Vec3 farLightEye{
        scene.center.x - sunDirection.x * lightDistance,
        scene.center.y - sunDirection.y * lightDistance,
        scene.center.z - sunDirection.z * lightDistance,
    };
    Matrix4 farLightView = makeLookAt(farLightEye, scene.center);
    stabilizeOrthographicShadowView(farLightView, scene.radius * 1.75f,
        scene.radius * 1.75f, 2048u);
    const Matrix4 farLightProjection = makeOrthographic(
        scene.radius * 1.75f, scene.radius * 1.75f,
        std::max(0.1f, scene.radius * 0.01f), scene.radius * 6.0f);
    const Matrix4 farLightViewProjection = multiply(farLightProjection, farLightView);
    scene.sunShadowViewProjection = farLightViewProjection;

    // R4 applies a separate, tightly fitted near cascade before the far map.
    // The old Metal path rendered only the far map over the whole frame, which
    // made close receivers use the coarse level-sized projection.  Keep the
    // split at the original default near distance (20 m) and cover the full
    // near receiver disk plus its shadow casters.
    constexpr float SunNearDistance = 20.0f;
    const float nearRadius = std::min(scene.radius * 1.75f, SunNearDistance * 1.5f);
    const Vec3 nearCenter = scene.gameplayViewValid ? scene.eye : scene.center;
    const Vec3 nearLightEye{
        nearCenter.x - sunDirection.x * nearRadius * 3.0f,
        nearCenter.y - sunDirection.y * nearRadius * 3.0f,
        nearCenter.z - sunDirection.z * nearRadius * 3.0f,
    };
    Matrix4 nearLightView = makeLookAt(nearLightEye, nearCenter);
    stabilizeOrthographicShadowView(nearLightView, nearRadius, nearRadius, 2048u);
    const Matrix4 nearLightProjection = makeOrthographic(
        nearRadius, nearRadius, std::max(0.1f, nearRadius * 0.01f), nearRadius * 6.0f);
    const Matrix4 nearLightViewProjection = multiply(nearLightProjection, nearLightView);
    scene.sunNearShadowViewProjection = nearLightViewProjection;
    Matrix4 nearShadowTexgen = screenTexgen;
    nearShadowTexgen.elements[10] = 1.0f;
    nearShadowTexgen.elements[14] = -0.00001f;
    Matrix4 farShadowTexgen = screenTexgen;
    farShadowTexgen.elements[10] = 1.0f;
    farShadowTexgen.elements[14] = -0.00002f;
    const Matrix4 nearShadowLookup = multiply(
        nearShadowTexgen, multiply(nearLightViewProjection, inverseView));
    Matrix4 farShadowLookup = multiply(
        farShadowTexgen, multiply(farLightViewProjection, inverseView));
    // R4's far path applies the default TSM receiver translation after its
    // projection.  Retain that source default while the native backend does
    // not yet receive the live r2_sun_tsm_bias console value.
    Matrix4 farTsmBias{};
    farTsmBias.elements[0] = farTsmBias.elements[5] = farTsmBias.elements[10] =
        farTsmBias.elements[15] = 1.0f;
    farTsmBias.elements[12] = sunDirection.x * -0.01f;
    farTsmBias.elements[13] = sunDirection.y * -0.01f;
    farTsmBias.elements[14] = sunDirection.z * -0.01f;
    farShadowLookup = multiply(farShadowLookup, farTsmBias);

    std::array<std::uint8_t, 224> sunVertexGlobals{};
    std::array<std::uint8_t, 256> sunVertexTransforms{};
    std::array<std::uint8_t, 432> sunNearFragmentGlobals{};
    std::array<std::uint8_t, 444> sunFarFragmentGlobals{};
    std::array<std::uint8_t, 48> maskDynamicLight{};
    std::memcpy(sunVertexGlobals.data() + 160, &screenTexgen, sizeof(Matrix4));
    std::memcpy(sunVertexTransforms.data() + 0, &identity, sizeof(Matrix4));
    std::memcpy(sunNearFragmentGlobals.data() + 224,
        &constantSunMask, sizeof(Matrix4));
    std::memcpy(sunNearFragmentGlobals.data() + 288,
        &nearShadowLookup, sizeof(Matrix4));
    putFloat4(sunNearFragmentGlobals, 416, 1.0f, 0.0f, 0.0f, 0.0f);
    putFloat4(sunFarFragmentGlobals, 272, 1.0f, 0.0f, 0.0f, 0.0f);
    std::memcpy(sunFarFragmentGlobals.data() + 304,
        &constantSunMask, sizeof(Matrix4));
    std::memcpy(sunFarFragmentGlobals.data() + 368,
        &farShadowLookup, sizeof(Matrix4));
    const float viewShadowProjection[3] = {0.0f, 0.0f, 0.0f};
    std::memcpy(sunFarFragmentGlobals.data() + 432,
        viewShadowProjection, sizeof(viewShadowProjection));
    const float maskDirectionScale = -std::sqrt(adaptedLuminance + 1.0e-6f);
    putFloat4(maskDynamicLight, 32, adaptedSunDirectionView.x * maskDirectionScale,
        adaptedSunDirectionView.y * maskDirectionScale,
        adaptedSunDirectionView.z * maskDirectionScale, 0.0f);
    std::array<std::uint8_t, 256> nearShadowTransforms{};
    std::array<std::uint8_t, 256> farShadowTransforms{};
    const Matrix4 firstNearShadowWorldViewProjection = multiply(
        nearLightViewProjection, !scene.textureMaterials.empty() ?
            scene.textureMaterials.front().modelTransform : identity);
    const Matrix4 firstFarShadowWorldViewProjection = multiply(
        farLightViewProjection, !scene.textureMaterials.empty() ?
            scene.textureMaterials.front().modelTransform : identity);
    std::memcpy(nearShadowTransforms.data(),
        &firstNearShadowWorldViewProjection, sizeof(Matrix4));
    std::memcpy(farShadowTransforms.data(),
        &firstFarShadowWorldViewProjection, sizeof(Matrix4));

    if (!resources.UpdateBuffer(scene.sunMaskVertices, 0,
               maskVertices.data(), sizeof(maskVertices), error))
        return false;
    if (!resources.UpdateBuffer(scene.sunVertexGlobals, 0,
            sunVertexGlobals.data(), sunVertexGlobals.size(), error))
        return false;
    if (!resources.UpdateBuffer(scene.sunVertexTransforms, 0,
            sunVertexTransforms.data(), sunVertexTransforms.size(), error))
        return false;
    // The source renderer moves the cuboid split to r2_sun_near in projected
    // depth each frame.  Its static native replacement was fixed at 0.70,
    // so the near stencil range varied with FOV and camera projection.  Keep
    // the same 20 m source default and derive its Metal depth from this frame.
    float cascadeDepth = 0.70f;
    if (scene.gameplayViewValid)
    {
        const Vec3 forward = normalize(transformDirection(inverseView, {0.0f, 0.0f, -1.0f}));
        const Vec3 splitPoint{
            scene.eye.x + forward.x * SunNearDistance,
            scene.eye.y + forward.y * SunNearDistance,
            scene.eye.z + forward.z * SunNearDistance,
        };
        const Vec4 splitClip = transformPointToClip(scene.uniformData.viewProjection, splitPoint);
        if (std::isfinite(splitClip.z) && std::isfinite(splitClip.w) &&
            std::fabs(splitClip.w) > 1.0e-6f)
            cascadeDepth = std::clamp(splitClip.z / splitClip.w, 0.0f, 1.0f);
    }
    const std::array<SunVolumeVertex, 8> volumeVertices = {{
        {{-1.0f, -1.0f, cascadeDepth, 1.0f}},
        {{-1.0f, -1.0f, 1.00f, 1.0f}},
        {{-1.0f,  1.0f, 1.00f, 1.0f}},
        {{-1.0f,  1.0f, cascadeDepth, 1.0f}},
        {{ 1.0f,  1.0f, 1.00f, 1.0f}},
        {{ 1.0f,  1.0f, cascadeDepth, 1.0f}},
        {{ 1.0f, -1.0f, 1.00f, 1.0f}},
        {{ 1.0f, -1.0f, cascadeDepth, 1.0f}},
    }};
    if (!resources.UpdateBuffer(scene.sunVolumeVertices, 0, volumeVertices.data(),
            sizeof(volumeVertices), error))
        return false;
    if (!resources.UpdateBuffer(scene.sunNearFragmentGlobals, 0,
            sunNearFragmentGlobals.data(), sunNearFragmentGlobals.size(), error))
        return false;
    if (!resources.UpdateBuffer(scene.sunFarFragmentGlobals, 0,
            sunFarFragmentGlobals.data(), sunFarFragmentGlobals.size(), error))
        return false;
    if (!resources.UpdateBuffer(scene.sunMaskDynamicLight, 0,
            maskDynamicLight.data(), maskDynamicLight.size(), error))
        return false;
    for (LevelScene::TextureMaterial& material : scene.textureMaterials)
    {
        const Matrix4 nearShadowWorldViewProjection = multiply(
            nearLightViewProjection, material.modelTransform);
        const Matrix4 farShadowWorldViewProjection = multiply(
            farLightViewProjection, material.modelTransform);
        std::memcpy(nearShadowTransforms.data(), &nearShadowWorldViewProjection, sizeof(Matrix4));
        std::memcpy(farShadowTransforms.data(), &farShadowWorldViewProjection, sizeof(Matrix4));
        if (!resources.UpdateBuffer(material.sunNearShadowTransforms, 0,
                nearShadowTransforms.data(), nearShadowTransforms.size(), error))
            return false;
        if (!resources.UpdateBuffer(material.sunShadowTransforms, 0,
                farShadowTransforms.data(), farShadowTransforms.size(), error))
            return false;
    }
    if (!resources.UpdateBuffer(scene.sunNearShadowTransforms, 0,
            nearShadowTransforms.data(), nearShadowTransforms.size(), error))
        return false;
    return resources.UpdateBuffer(scene.sunShadowTransforms, 0,
        farShadowTransforms.data(), farShadowTransforms.size(), error);
}

bool ensureLevelDepthTarget(IR4Backend& backend, LevelScene& scene, std::string& error)
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    backend.DrawableSize(width, height);
    const bool depthReady = scene.depthTarget && scene.depthTarget->Descriptor().width == width &&
        scene.depthTarget->Descriptor().height == height;
    const bool gBufferReady = !scene.productionMsc ||
        (scene.gBufferTargets.size() == 4 &&
         scene.scopeDepthSnapshot &&
         scene.scopeDepthSnapshot->Descriptor().width == width &&
         scene.scopeDepthSnapshot->Descriptor().height == height &&
         std::all_of(scene.gBufferTargets.begin(), scene.gBufferTargets.end(),
             [width, height](const auto& target) {
                 return target && target->Descriptor().width == width &&
                     target->Descriptor().height == height;
             }));
    const bool lightingReady = !scene.productionBaseLighting ||
        (scene.lightingAccumulator &&
         scene.lightingAccumulator->Descriptor().width == width &&
         scene.lightingAccumulator->Descriptor().height == height &&
         scene.lightingTargets.size() == 2 &&
         scene.scopeSceneColorSnapshot &&
         scene.scopeSceneColorSnapshot->Descriptor().width == width &&
         scene.scopeSceneColorSnapshot->Descriptor().height == height &&
         std::all_of(scene.lightingTargets.begin(), scene.lightingTargets.end(),
             [width, height](const auto& target) {
                 return target && target->Descriptor().width == width &&
                     target->Descriptor().height == height;
             }));
    if (depthReady && gBufferReady && lightingReady)
        return true;
    if (width == 0 || height == 0)
    {
        error = "drawable has no size for the level depth target";
        return false;
    }
    R4Texture2DDesc depthDesc;
    depthDesc.width = width;
    depthDesc.height = height;
    depthDesc.format = scene.productionMsc ?
        R4PixelFormat::Depth32FloatStencil8 : R4PixelFormat::Depth32Float;
    depthDesc.storage = R4ResourceStorage::Private;
    depthDesc.usage = scene.productionMsc ?
        (R4TextureUsage::RenderTarget | R4TextureUsage::ShaderRead) :
        R4TextureUsage::RenderTarget;
    depthDesc.label = "R4 l05_bar depth";
    scene.depthTarget = backend.Resources().CreateTexture2D(depthDesc, error);
    if (!scene.depthTarget)
        return false;

    if (scene.productionMsc)
    {
        R4Texture2DDesc scopeDepthDesc = depthDesc;
        scopeDepthDesc.usage = R4TextureUsage::ShaderRead;
        scopeDepthDesc.label = "R4 3DSS world depth snapshot";
        scene.scopeDepthSnapshot =
            backend.Resources().CreateTexture2D(scopeDepthDesc, error);
        if (!scene.scopeDepthSnapshot)
            return false;
        static constexpr R4PixelFormat Formats[4] = {
            R4PixelFormat::RGBA16Float,
            R4PixelFormat::RGBA8Unorm,
            R4PixelFormat::RGBA8Unorm,
            R4PixelFormat::RGBA16Float,
        };
        scene.gBufferTargets.clear();
        for (std::size_t index = 0; index < 4; ++index)
        {
            R4Texture2DDesc targetDesc;
            targetDesc.width = width;
            targetDesc.height = height;
            targetDesc.format = Formats[index];
            targetDesc.storage = R4ResourceStorage::Private;
            targetDesc.usage = R4TextureUsage::RenderTarget | R4TextureUsage::ShaderRead;
            targetDesc.label = "R4 production G-buffer target";
            std::shared_ptr<IR4Texture> target =
                backend.Resources().CreateTexture2D(targetDesc, error);
            if (!target)
                return false;
            scene.gBufferTargets.emplace_back(std::move(target));
        }
    }
    if (scene.productionBaseLighting)
    {
        R4Texture2DDesc accumulatorDesc;
        accumulatorDesc.width = width;
        accumulatorDesc.height = height;
        accumulatorDesc.format = R4PixelFormat::RGBA16Float;
        accumulatorDesc.storage = R4ResourceStorage::Private;
        accumulatorDesc.usage = R4TextureUsage::RenderTarget | R4TextureUsage::ShaderRead;
        accumulatorDesc.label = "R4 production light accumulator";
        scene.lightingAccumulator =
            backend.Resources().CreateTexture2D(accumulatorDesc, error);
        if (!scene.lightingAccumulator)
            return false;
        scene.lightingTargets.clear();
        for (std::size_t index = 0; index < 2; ++index)
        {
            R4Texture2DDesc lightingDesc;
            lightingDesc.width = width;
            lightingDesc.height = height;
            lightingDesc.format = R4PixelFormat::RGBA8Unorm;
            lightingDesc.storage = R4ResourceStorage::Private;
            lightingDesc.usage = R4TextureUsage::RenderTarget | R4TextureUsage::ShaderRead;
            lightingDesc.label = index == 0 ?
                "R4 combine_1 low target" : "R4 combine_1 high target";
            std::shared_ptr<IR4Texture> target =
                backend.Resources().CreateTexture2D(lightingDesc, error);
            if (!target)
                return false;
            scene.lightingTargets.emplace_back(std::move(target));
        }
        R4Texture2DDesc scopeColorDesc;
        scopeColorDesc.width = width;
        scopeColorDesc.height = height;
        scopeColorDesc.format = R4PixelFormat::RGBA8Unorm;
        scopeColorDesc.storage = R4ResourceStorage::Private;
        scopeColorDesc.usage = R4TextureUsage::ShaderRead;
        scopeColorDesc.label = "R4 3DSS pre-reticle scene snapshot";
        scene.scopeSceneColorSnapshot =
            backend.Resources().CreateTexture2D(scopeColorDesc, error);
        if (!scene.scopeSceneColorSnapshot)
            return false;
        if (!updateBaseLightingConstants(backend, scene, width, height, nullptr, error))
            return false;
    }
    return true;
}

bool uploadLevelScene(IR4Backend& backend, LevelScene& scene, std::string& error)
{
    if (scene.uploaded)
        return true;
    IR4Resources& resources = backend.Resources();
    for (std::size_t i = 0; i < scene.geometry.vertexBuffers.size(); ++i)
    {
        const auto& source = scene.geometry.vertexBuffers[i];
        if (!resources.UpdateBuffer(scene.vertexBuffers[i], 0,
                scene.geometryBytes.data() + source.dataOffset, source.dataSize, error))
            return false;
    }
    for (std::size_t i = 0; i < scene.geometry.indexBuffers.size(); ++i)
    {
        const auto& source = scene.geometry.indexBuffers[i];
        if (!resources.UpdateBuffer(scene.indexBuffers[i], 0,
                scene.geometryBytes.data() + source.dataOffset, source.dataSize, error))
            return false;
    }
    for (LevelScene::TextureAsset& asset : scene.textureAssets)
    {
        for (std::size_t level = 0; level < asset.ddsTexture.mipLevels.size(); ++level)
        {
            const xray::metal::R4DdsMipLevel& mip = asset.ddsTexture.mipLevels[level];
            if (!resources.UploadTexture2D(
                    asset.texture,
                    static_cast<std::uint32_t>(level),
                    0,
                    asset.ddsTexture.bytes.data() + mip.dataOffset,
                    mip.bytesPerImage,
                    mip.bytesPerRow,
                    mip.bytesPerImage,
                    error))
                return false;
            asset.uploadedBytes += mip.bytesPerImage;
        }
        asset.ddsTexture.bytes.clear();
        asset.ddsTexture.bytes.shrink_to_fit();
    }
    if (scene.lutAtlas && !scene.lutAtlasUploaded && !scene.lutAtlasDds.bytes.empty())
    {
        for (std::size_t level = 0; level < scene.lutAtlasDds.mipLevels.size(); ++level)
        {
            const xray::metal::R4DdsMipLevel& mip = scene.lutAtlasDds.mipLevels[level];
            if (!resources.UploadTexture2D(
                    scene.lutAtlas,
                    static_cast<std::uint32_t>(level),
                    0,
                    scene.lutAtlasDds.bytes.data() + mip.dataOffset,
                    mip.bytesPerImage,
                    mip.bytesPerRow,
                    mip.bytesPerImage,
                    error))
                return false;
        }
        scene.lutAtlasDds.bytes.clear();
        scene.lutAtlasDds.bytes.shrink_to_fit();
        scene.lutAtlasUploaded = true;
        std::fprintf(stderr, "METAL_LUT_ATLAS_UPLOADED width=%u height=%u mips=%u format=%u\n",
            scene.lutAtlas->Descriptor().width, scene.lutAtlas->Descriptor().height,
            scene.lutAtlas->Descriptor().mipLevels, static_cast<unsigned>(scene.lutAtlas->Descriptor().format));
    }
    if (scene.productionBaseLighting && !scene.lightingInputsUploaded)
    {
        const float tonemap = 1.0f;
        const std::uint8_t white[4] = {255, 255, 255, 255};
        const std::uint8_t black[4] = {0, 0, 0, 0};
        if (!resources.UploadTexture2D(
                scene.lightingTonemap, 0, 0, &tonemap, sizeof(tonemap), 4, 4, error) ||
            !resources.UploadTexture2D(
                scene.lightingTonemapScratch, 0, 0, &tonemap, sizeof(tonemap), 4, 4, error) ||
            !resources.UploadTexture2D(
                scene.lightingWhite, 0, 0, white, sizeof(white), 4, 4, error) ||
            !resources.UploadTexture2D(
                scene.lightingBlack, 0, 0, black, sizeof(black), 4, 4, error))
            return false;
        for (std::uint32_t level = 0; level < 7; ++level)
        {
            const std::uint32_t dimension = std::max(1u, 64u >> level);
            std::vector<float> environment(
                static_cast<std::size_t>(dimension) * dimension * 4u);
            for (std::size_t texel = 0; texel < environment.size(); texel += 4)
            {
                environment[texel + 0] = 1.25f;
                environment[texel + 1] = 1.35f;
                environment[texel + 2] = 1.50f;
                environment[texel + 3] = 1.0f;
            }
            const std::size_t bytesPerRow = static_cast<std::size_t>(dimension) * 16u;
            const std::size_t bytesPerImage = bytesPerRow * dimension;
            for (std::uint32_t face = 0; face < 6; ++face)
            {
                if (!resources.UploadTexture2D(
                        scene.lightingEnvironment, level, face,
                        environment.data(), bytesPerImage,
                        bytesPerRow, bytesPerImage, error))
                    return false;
            }
        }
        if (scene.productionSun)
        {
            const std::uint8_t whiteSunMask[4] = {255, 255, 255, 255};
            const std::uint8_t blackSss[4] = {0, 0, 0, 0};
            if (!resources.UploadTexture2D(scene.sunMaskTexture, 0, 0,
                    whiteSunMask, sizeof(whiteSunMask), 4, 4, error) ||
                !resources.UploadTexture2D(scene.sunSssTexture, 0, 0,
                    blackSss, sizeof(blackSss), 4, 4, error))
                return false;
        }
        scene.lightingInputsUploaded = true;
    }
    scene.geometryBytes.clear();
    scene.geometryBytes.shrink_to_fit();
    scene.uploaded = true;
    return true;
}

bool updateLuminanceConstants(
    IR4Backend& backend,
    LevelScene& scene,
    std::uint32_t sourceWidth,
    std::uint32_t sourceHeight,
    const R4EnvironmentSnapshot& environment,
    std::string& error)
{
    if (!scene.productionLuminance)
        return true;
    const auto screenGlobals = [](std::uint32_t width, std::uint32_t height) {
        std::array<std::uint8_t, 528> bytes{};
        const float screen[4] = {
            static_cast<float>(width), static_cast<float>(height),
            1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height),
        };
        std::memcpy(bytes.data() + 416, screen, sizeof(screen));
        return bytes;
    };
    const std::array<std::uint8_t, 528> buildVertexGlobals = screenGlobals(64, 64);
    const std::array<std::uint8_t, 528> buildFragmentGlobals =
        screenGlobals(sourceWidth, sourceHeight);
    const std::array<std::uint8_t, 528> filter64VertexGlobals = screenGlobals(8, 8);
    const std::array<std::uint8_t, 528> filter64FragmentGlobals = screenGlobals(64, 64);
    const std::array<std::uint8_t, 528> filter8VertexGlobals = screenGlobals(1, 1);
    const std::array<std::uint8_t, 528> filter8FragmentGlobals = screenGlobals(8, 8);

    std::array<LuminanceBuildVertex, 4> buildVertices{};
    const float halfX = 1.0f / static_cast<float>(sourceWidth);
    const float halfY = 1.0f / static_cast<float>(sourceHeight);
    const float oneX = 2.0f * halfX;
    const float oneY = 2.0f * halfY;
    const float ax[4] = {halfX, halfX + oneX, halfX, halfX + oneX};
    const float ay[4] = {halfY, halfY, halfY + oneY, halfY + oneY};
    const float bx[4] = {1.0f + ax[0], 1.0f + ax[1], 1.0f + ax[2], 1.0f + ax[3]};
    const float by[4] = {1.0f + ay[0], 1.0f + ay[1], 1.0f + ay[2], 1.0f + ay[3]};
    const auto setBuildVertex = [&](std::size_t vertex, float x, float y,
                                    bool right, bool down) {
        buildVertices[vertex].position[0] = x;
        buildVertices[vertex].position[1] = y;
        buildVertices[vertex].position[3] = 1.0f;
        for (std::size_t texcoord = 0; texcoord < 4; ++texcoord)
        {
            buildVertices[vertex].texcoord[texcoord][0] = right ? bx[texcoord] : ax[texcoord];
            buildVertices[vertex].texcoord[texcoord][1] = down ? by[texcoord] : ay[texcoord];
        }
    };
    setBuildVertex(0, 0.0f, 64.0f, false, true);
    setBuildVertex(1, 0.0f, 0.0f, false, false);
    setBuildVertex(2, 64.0f, 64.0f, true, true);
    setBuildVertex(3, 64.0f, 0.0f, true, false);

    const auto makeFilterVertices = [](float targetExtent, float sourceExtent) {
        std::array<LuminanceFilterVertex, 4> vertices{};
        float ax[16]{};
        float ay[16]{};
        float bx[16]{};
        float by[16]{};
        for (std::size_t kernel = 0; kernel < 16; ++kernel)
        {
            ax[kernel] = static_cast<float>((kernel * 2 + 1) % 8) / sourceExtent;
            ay[kernel] = static_cast<float>((kernel / 4) * 2 + 1) / sourceExtent;
            bx[kernel] = ax[kernel] + 1.0f;
            by[kernel] = ay[kernel] + 1.0f;
        }
        const auto setVertex = [&](std::size_t vertex, float x, float y,
                                   bool right, bool down) {
            vertices[vertex].position[0] = x;
            vertices[vertex].position[1] = y;
            vertices[vertex].position[3] = 1.0f;
            for (std::size_t texcoord = 0; texcoord < 8; ++texcoord)
            {
                float* value = vertices[vertex].texcoord[texcoord];
                value[0] = right ? bx[texcoord] : ax[texcoord];
                value[1] = down ? by[texcoord] : ay[texcoord];
                value[2] = down ? by[texcoord + 8] : ay[texcoord + 8];
                value[3] = right ? bx[texcoord + 8] : ax[texcoord + 8];
            }
        };
        setVertex(0, 0.0f, targetExtent, false, true);
        setVertex(1, 0.0f, 0.0f, false, false);
        setVertex(2, targetExtent, targetExtent, true, true);
        setVertex(3, targetExtent, 0.0f, true, false);
        return vertices;
    };
    const std::array<LuminanceFilterVertex, 4> filter64Vertices =
        makeFilterVertices(8.0f, 64.0f);
    const std::array<LuminanceFilterVertex, 4> filter8Vertices =
        makeFilterVertices(1.0f, 8.0f);

    const float frameDelta = std::isfinite(environment.frameDeltaSeconds) ?
        std::max(0.0f, environment.frameDeltaSeconds) : 0.0f;
    const float adaptationSpeed = std::isfinite(environment.tonemapParameters[3]) ?
        std::max(0.0f, environment.tonemapParameters[3]) : 1.0f;
    scene.luminanceAdaptation = 0.9f * scene.luminanceAdaptation +
        0.1f * frameDelta * adaptationSpeed;
    std::array<std::uint8_t, 176> adaptGlobals{};
    const float middleGray[4] = {
        std::isfinite(environment.tonemapParameters[0]) ? environment.tonemapParameters[0] : 1.0f,
        std::isfinite(environment.tonemapParameters[1]) ? environment.tonemapParameters[1] : 0.0f,
        std::isfinite(environment.tonemapParameters[2]) ? environment.tonemapParameters[2] : 1.0f,
        scene.luminanceAdaptation,
    };
    std::memcpy(adaptGlobals.data() + 160, middleGray, sizeof(middleGray));

    IR4Resources& resources = backend.Resources();
    return resources.UpdateBuffer(scene.luminanceBuildVertices, 0,
               buildVertices.data(), sizeof(buildVertices), error) &&
        resources.UpdateBuffer(scene.luminanceFilter64Vertices, 0,
               filter64Vertices.data(), sizeof(filter64Vertices), error) &&
        resources.UpdateBuffer(scene.luminanceFilter8Vertices, 0,
               filter8Vertices.data(), sizeof(filter8Vertices), error) &&
        resources.UpdateBuffer(scene.luminanceBuildVertexGlobals, 0,
               buildVertexGlobals.data(), buildVertexGlobals.size(), error) &&
        resources.UpdateBuffer(scene.luminanceBuildFragmentGlobals, 0,
               buildFragmentGlobals.data(), buildFragmentGlobals.size(), error) &&
        resources.UpdateBuffer(scene.luminanceFilter64VertexGlobals, 0,
               filter64VertexGlobals.data(), filter64VertexGlobals.size(), error) &&
        resources.UpdateBuffer(scene.luminanceFilter64FragmentGlobals, 0,
               filter64FragmentGlobals.data(), filter64FragmentGlobals.size(), error) &&
        resources.UpdateBuffer(scene.luminanceFilter8VertexGlobals, 0,
               filter8VertexGlobals.data(), filter8VertexGlobals.size(), error) &&
        resources.UpdateBuffer(scene.luminanceFilter8FragmentGlobals, 0,
               filter8FragmentGlobals.data(), filter8FragmentGlobals.size(), error) &&
        resources.UpdateBuffer(scene.luminanceAdaptGlobals, 0,
               adaptGlobals.data(), adaptGlobals.size(), error);
}

bool drawLuminancePass(
    IR4Backend& backend,
    const std::shared_ptr<IR4GraphicsPipeline>& pipeline,
    const std::shared_ptr<IR4Buffer>& vertices,
    const std::shared_ptr<IR4Buffer>& vertexGlobals,
    const std::shared_ptr<IR4Texture>& source,
    const std::shared_ptr<IR4Buffer>& fragmentGlobals,
    const std::shared_ptr<IR4Sampler>& sampler,
    const std::shared_ptr<IR4Buffer>& indices,
    std::string& error)
{
    R4MscStageBindings vertexBindings;
    vertexBindings.stage = R4ShaderStage::Vertex;
    vertexBindings.tlabSize = 24;
    vertexBindings.resources = {{R4MscResourceType::ConstantBuffer, 0,
        vertexGlobals, 0, 528, nullptr, nullptr}};
    R4MscStageBindings fragmentBindings;
    fragmentBindings.stage = R4ShaderStage::Fragment;
    fragmentBindings.tlabSize = 72;
    fragmentBindings.resources = {
        {R4MscResourceType::Texture, 0, nullptr, 0, 0, source, nullptr},
        {R4MscResourceType::ConstantBuffer, 24, fragmentGlobals, 0, 528, nullptr, nullptr},
        {R4MscResourceType::Sampler, 48, nullptr, 0, 0, nullptr, sampler},
    };
    if (!backend.BindGraphicsPipeline(pipeline, error) ||
        !backend.BindVertexBuffer(6, vertices, 0, error) ||
        !backend.BindMscResources(vertexBindings, error) ||
        !backend.BindMscResources(fragmentBindings, error))
        return false;
    R4IndexedDrawDesc draw;
    draw.indexBuffer = indices;
    draw.indexCount = 6;
    draw.indexType = R4IndexType::UInt16;
    return backend.DrawIndexed(draw, error);
}

bool drawLuminanceAdaptation(
    IR4Backend& backend,
    LevelScene& scene,
    const R4EnvironmentSnapshot& environment,
    std::string& error)
{
    if (!scene.productionLuminance)
        return true;
    const auto renderPass = [&](const char* label, std::uint32_t extent,
                                const std::shared_ptr<IR4Texture>& target,
                                const std::function<bool()>& draw) {
        R4RenderPassDesc pass;
        pass.label = label;
        pass.useDrawable = false;
        R4ColorAttachmentDesc attachment;
        attachment.texture = target;
        pass.colorAttachments = {attachment};
        pass.hasViewport = true;
        pass.viewport.width = extent;
        pass.viewport.height = extent;
        return backend.BeginRenderPass(pass, error) && draw() && backend.EndRenderPass(error);
    };
    if (!renderPass("R4 converted luminance build 64x64", 64, scene.luminance64, [&] {
            return drawLuminancePass(backend, scene.luminanceBuildPipeline,
                scene.luminanceBuildVertices, scene.luminanceBuildVertexGlobals,
                scene.lightingTargets[1], scene.luminanceBuildFragmentGlobals,
                scene.lightingLinearSampler, scene.luminanceIndices, error);
        }) ||
        !renderPass("R4 converted luminance filter 8x8", 8, scene.luminance8, [&] {
            return drawLuminancePass(backend, scene.luminanceFilterPipeline,
                scene.luminanceFilter64Vertices, scene.luminanceFilter64VertexGlobals,
                scene.luminance64, scene.luminanceFilter64FragmentGlobals,
                scene.lightingLinearSampler, scene.luminanceIndices, error);
        }))
        return false;

    const auto drawAdapt = [&] {
        R4MscStageBindings vertexBindings;
        vertexBindings.stage = R4ShaderStage::Vertex;
        vertexBindings.tlabSize = 24;
        vertexBindings.resources = {{R4MscResourceType::ConstantBuffer, 0,
            scene.luminanceFilter8VertexGlobals, 0, 528, nullptr, nullptr}};
        R4MscStageBindings fragmentBindings;
        fragmentBindings.stage = R4ShaderStage::Fragment;
        fragmentBindings.tlabSize = 144;
        fragmentBindings.resources = {
            {R4MscResourceType::Texture, 0, nullptr, 0, 0, scene.luminance8, nullptr},
            {R4MscResourceType::Texture, 24, nullptr, 0, 0, scene.lightingTonemap, nullptr},
            {R4MscResourceType::ConstantBuffer, 48,
                scene.luminanceAdaptGlobals, 0, 176, nullptr, nullptr},
            {R4MscResourceType::ConstantBuffer, 72,
                scene.luminanceFilter8FragmentGlobals, 0, 528, nullptr, nullptr},
            {R4MscResourceType::Sampler, 96, nullptr, 0, 0,
                nullptr, scene.lightingNoFilterSampler},
            {R4MscResourceType::Sampler, 120, nullptr, 0, 0,
                nullptr, scene.lightingLinearSampler},
        };
        if (!backend.BindGraphicsPipeline(scene.luminanceAdaptPipeline, error) ||
            !backend.BindVertexBuffer(6, scene.luminanceFilter8Vertices, 0, error) ||
            !backend.BindMscResources(vertexBindings, error) ||
            !backend.BindMscResources(fragmentBindings, error))
            return false;
        R4IndexedDrawDesc draw;
        draw.indexBuffer = scene.luminanceIndices;
        draw.indexCount = 6;
        draw.indexType = R4IndexType::UInt16;
        return backend.DrawIndexed(draw, error);
    };
    if (!renderPass("R4 converted temporal luminance adaptation", 1,
            scene.lightingTonemapScratch, drawAdapt))
        return false;
    std::swap(scene.lightingTonemap, scene.lightingTonemapScratch);
    if (scene.luminanceLastTelemetryFrame == UINT32_MAX ||
        environment.engineFrame - scene.luminanceLastTelemetryFrame >= 300u)
    {
        std::fprintf(stderr,
            "METAL_LUMINANCE_FRAME frame=%u source=combine_high chain=64x64,8x8,1x1 "
            "middle_gray=%.4f amount=%.4f low_lum=%.4f adaptation=%.6f ping_pong=true\n",
            environment.engineFrame, environment.tonemapParameters[0],
            environment.tonemapParameters[1], environment.tonemapParameters[2],
            scene.luminanceAdaptation);
        scene.luminanceLastTelemetryFrame = environment.engineFrame;
    }
    return true;
}

bool drawLevelScene(IR4Backend& backend, LevelScene& scene, std::string& error)
{
    if (scene.textureMaterials.empty() &&
        !backend.BindVertexBuffer(1, scene.uniforms, 0, error))
        return false;
    if (!scene.productionMsc && !scene.textureMaterials.empty() &&
        !backend.BindFragmentSampler(0, scene.baseSampler, error))
        return false;
    if (!scene.productionMsc && scene.hemiDiagnostic &&
        !backend.BindFragmentSampler(1, scene.hemiSampler, error))
        return false;
    std::size_t lastVertexBuffer = std::numeric_limits<std::size_t>::max();
    std::shared_ptr<IR4GraphicsPipeline> lastPipeline;
    std::size_t automaticLmhSubmitted = 0;
    std::size_t automaticLmhCulled = 0;
    std::size_t automaticLmhArefSubmitted = 0;
    std::size_t automaticLmhArefCulled = 0;
    for (const xray::metal::R4StaticVisual& visual : scene.geometry.visuals)
    {
        if (!visual.valid || !visual.referencedPositionBounds.available ||
            !scene.pipelines[visual.vertexBufferId])
            continue;
        std::shared_ptr<IR4GraphicsPipeline> selectedPipeline =
            scene.pipelines[visual.vertexBufferId];
        bool alphaTested = false;
        if (!scene.textureMaterials.empty())
        {
            const auto material = scene.textureMaterialByVisual.find(visual.visualId);
            if (material == scene.textureMaterialByVisual.end())
                continue;
            const LevelScene::TextureMaterial& binding =
                scene.textureMaterials[material->second];
            if (scene.productionAref && binding.shaderName == "def_shaders\\def_aref")
            {
                alphaTested = true;
                selectedPipeline = scene.arefPipelines[visual.vertexBufferId];
                if (!selectedPipeline)
                {
                    error = "admitted LMH alpha-test visual has no matching production pipeline";
                    return false;
                }
            }
            if (scene.automaticLmhFrustumCulling && conservativelyOutsideMetalFrustum(
                    binding.localPositionBounds, binding.modelTransform,
                    scene.uniformData.viewProjection, scene.liveViewProjectionValid))
            {
                ++automaticLmhCulled;
                if (alphaTested)
                    ++automaticLmhArefCulled;
                continue;
            }
            if (scene.productionMsc)
            {
                R4MscStageBindings vertexBindings;
                vertexBindings.stage = R4ShaderStage::Vertex;
                vertexBindings.tlabSize = 48;
                vertexBindings.resources = {
                    {R4MscResourceType::ConstantBuffer, 0,
                        binding.mscVertexGlobals, 0, 160},
                    {R4MscResourceType::ConstantBuffer, 24,
                        binding.mscVertexTransforms, 0, 256},
                };
                R4MscStageBindings fragmentBindings;
                fragmentBindings.stage = R4ShaderStage::Fragment;
                fragmentBindings.tlabSize = 144;
                R4MscResourceBinding baseTexture;
                baseTexture.type = R4MscResourceType::Texture;
                baseTexture.tlabOffset = 0;
                baseTexture.texture = scene.textureAssets[binding.textureAssetIndex].texture;
                R4MscResourceBinding hemiTexture;
                hemiTexture.type = R4MscResourceType::Texture;
                hemiTexture.tlabOffset = 24;
                hemiTexture.texture = scene.textureAssets[binding.hemiTextureAssetIndex].texture;
                R4MscResourceBinding fragmentGlobals;
                fragmentGlobals.type = R4MscResourceType::ConstantBuffer;
                fragmentGlobals.tlabOffset = 48;
                fragmentGlobals.buffer = binding.mscFragmentGlobals;
                fragmentGlobals.bufferSize = 176;
                R4MscResourceBinding fragmentTransforms;
                fragmentTransforms.type = R4MscResourceType::ConstantBuffer;
                fragmentTransforms.tlabOffset = 72;
                fragmentTransforms.buffer = binding.mscFragmentTransforms;
                fragmentTransforms.bufferSize = 256;
                R4MscResourceBinding rtLinearSampler;
                rtLinearSampler.type = R4MscResourceType::Sampler;
                rtLinearSampler.tlabOffset = 96;
                rtLinearSampler.sampler = scene.hemiSampler;
                R4MscResourceBinding baseSampler;
                baseSampler.type = R4MscResourceType::Sampler;
                baseSampler.tlabOffset = 120;
                baseSampler.sampler = scene.baseSampler;
                fragmentBindings.resources = {
                    baseTexture, hemiTexture, fragmentGlobals, fragmentTransforms,
                    rtLinearSampler, baseSampler,
                };
                if (!backend.BindMscResources(vertexBindings, error) ||
                    !backend.BindMscResources(fragmentBindings, error))
                    return false;
            }
            else
            {
                if (!backend.BindVertexBuffer(
                        1, binding.uniforms, 0, error) ||
                    !backend.BindFragmentTexture(
                        0, scene.textureAssets[binding.textureAssetIndex].texture, error))
                    return false;
                if (scene.hemiDiagnostic &&
                    !backend.BindFragmentTexture(
                        1, scene.textureAssets[binding.hemiTextureAssetIndex].texture, error))
                    return false;
            }
        }
        if (lastPipeline != selectedPipeline)
        {
            if (!backend.BindGraphicsPipeline(selectedPipeline, error))
                return false;
            lastPipeline = selectedPipeline;
        }
        if (lastVertexBuffer != visual.vertexBufferId)
        {
            if (!backend.BindVertexBuffer(scene.productionMsc ? 6 : 0,
                    scene.vertexBuffers[visual.vertexBufferId], 0, error))
                return false;
            lastVertexBuffer = visual.vertexBufferId;
        }
        R4IndexedDrawDesc draw;
        draw.indexBuffer = scene.indexBuffers[visual.indexBufferId];
        draw.indexBufferOffset = static_cast<std::size_t>(visual.indexBase) * sizeof(std::uint16_t);
        draw.indexCount = visual.indexCount;
        draw.indexType = R4IndexType::UInt16;
        draw.baseVertex = static_cast<std::int32_t>(visual.vertexBase);
        if (!backend.DrawIndexed(draw, error))
            return false;
        if (scene.automaticLmhFrustumCulling)
        {
            ++automaticLmhSubmitted;
            if (alphaTested)
                ++automaticLmhArefSubmitted;
        }
    }
    if (scene.automaticLmhFrustumCulling)
    {
        ++scene.automaticLmhFrustumFrames;
        scene.automaticLmhFrustumSubmitted = automaticLmhSubmitted;
        scene.automaticLmhFrustumCulled = automaticLmhCulled;
        const bool firstLiveReport = scene.liveViewProjectionValid &&
            !scene.automaticLmhLiveFrustumReported;
        const bool firstGameplayReport = scene.gameplayViewValid &&
            !scene.automaticLmhGameplayFrustumReported;
        if (scene.automaticLmhFrustumFrames == 1u || firstLiveReport || firstGameplayReport ||
            scene.automaticLmhFrustumFrames % 300u == 0u)
        {
            std::fprintf(stderr,
                "METAL_LMH_FRUSTUM frame=%llu submitted=%zu culled=%zu "
                "aref_submitted=%zu aref_culled=%zu live_view=%s "
                "mode=automatic_all fail_open=nonfinite_or_uncertain\n",
                static_cast<unsigned long long>(scene.automaticLmhFrustumFrames),
                automaticLmhSubmitted, automaticLmhCulled,
                automaticLmhArefSubmitted, automaticLmhArefCulled,
                scene.liveViewProjectionValid ? "true" : "false");
            if (firstLiveReport)
                scene.automaticLmhLiveFrustumReported = true;
            if (firstGameplayReport)
                scene.automaticLmhGameplayFrustumReported = true;
        }
    }
    return true;
}

bool drawBaseLighting(IR4Backend& backend, LevelScene& scene, std::string& error)
{
    if (!scene.productionBaseLighting || scene.gBufferTargets.size() != 4 ||
        scene.lightingTargets.size() != 2 || !scene.lightingAccumulator ||
        !scene.baseLightingPipeline || !scene.baseLightingVertices ||
        !scene.baseLightingIndices)
    {
        error = "production base-lighting resources are incomplete";
        return false;
    }
    auto textureBinding = [](std::size_t offset, const std::shared_ptr<IR4Texture>& texture) {
        R4MscResourceBinding binding;
        binding.type = R4MscResourceType::Texture;
        binding.tlabOffset = offset;
        binding.texture = texture;
        return binding;
    };
    auto bufferBinding = [](std::size_t offset, const std::shared_ptr<IR4Buffer>& buffer,
                             std::size_t size) {
        R4MscResourceBinding binding;
        binding.type = R4MscResourceType::ConstantBuffer;
        binding.tlabOffset = offset;
        binding.buffer = buffer;
        binding.bufferSize = size;
        return binding;
    };
    auto samplerBinding = [](std::size_t offset, const std::shared_ptr<IR4Sampler>& sampler) {
        R4MscResourceBinding binding;
        binding.type = R4MscResourceType::Sampler;
        binding.tlabOffset = offset;
        binding.sampler = sampler;
        return binding;
    };

    R4MscStageBindings vertexBindings;
    vertexBindings.stage = R4ShaderStage::Vertex;
    vertexBindings.tlabSize = 24;
    vertexBindings.resources.push_back(textureBinding(0, scene.lightingTonemap));

    R4MscStageBindings fragmentBindings;
    fragmentBindings.stage = R4ShaderStage::Fragment;
    fragmentBindings.tlabSize = 336;
    fragmentBindings.resources = {
        textureBinding(0, scene.gBufferTargets[0]),
        textureBinding(24, scene.gBufferTargets[1]),
        textureBinding(48, scene.lightingAccumulator),
        textureBinding(72, scene.lightingEnvironment0 ?
            scene.lightingEnvironment0 : scene.lightingEnvironment),
        textureBinding(96, scene.lightingEnvironment1 ?
            scene.lightingEnvironment1 : scene.lightingEnvironment),
        textureBinding(120, scene.ssfxAoOutput ? scene.ssfxAoOutput : scene.lightingWhite),
        textureBinding(144, scene.ssfxIlOutput ? scene.ssfxIlOutput : scene.lightingBlack),
        textureBinding(168, scene.gBufferTargets[3]),
        bufferBinding(192, scene.baseLightingGlobals, 384),
        bufferBinding(216, scene.baseLightingStaticGlobals, 528),
        bufferBinding(240, scene.baseLightingDynamicLight, 48),
        samplerBinding(264, scene.lightingNoFilterSampler),
        samplerBinding(288, scene.lightingLinearSampler),
        samplerBinding(312, scene.baseSampler),
    };
    if (!backend.BindGraphicsPipeline(scene.baseLightingPipeline, error) ||
        !backend.BindVertexBuffer(6, scene.baseLightingVertices, 0, error) ||
        !backend.BindMscResources(vertexBindings, error) ||
        !backend.BindMscResources(fragmentBindings, error))
        return false;
    R4IndexedDrawDesc draw;
    draw.indexBuffer = scene.baseLightingIndices;
    draw.indexCount = 6;
    draw.indexType = R4IndexType::UInt16;
    return backend.DrawIndexed(draw, error);
}

bool drawSunShadowMap(IR4Backend& backend, LevelScene& scene, bool nearCascade,
    std::string& error)
{
    if (!scene.productionSun || !scene.sunShadowPipeline ||
        !scene.sunShadowTransforms)
    {
        error = "production direct-sun shadow resources are incomplete";
        return false;
    }
    if (scene.textureMaterials.empty())
        return true;
    R4MscStageBindings vertexBindings;
    vertexBindings.stage = R4ShaderStage::Vertex;
    vertexBindings.tlabSize = 24;
    std::size_t lastVertexBuffer = std::numeric_limits<std::size_t>::max();
    for (const LevelScene::TextureMaterial& material : scene.textureMaterials)
    {
        const bool alphaTested = material.shaderName == "def_shaders\\def_aref";
        const std::shared_ptr<IR4GraphicsPipeline>& pipeline = alphaTested ?
            scene.sunShadowArefPipeline : scene.sunShadowPipeline;
        if (!pipeline)
        {
            error = "alpha-tested R4 shadow caster pipeline is unavailable";
            return false;
        }
        if (!backend.BindGraphicsPipeline(pipeline, error))
            return false;
        vertexBindings.resources = {{
            R4MscResourceType::ConstantBuffer, 0,
            nearCascade ? material.sunNearShadowTransforms : material.sunShadowTransforms,
            0, 256, nullptr, nullptr,
        }};
        if (!backend.BindMscResources(vertexBindings, error))
            return false;
        if (alphaTested)
        {
            if (material.textureAssetIndex >= scene.textureAssets.size() ||
                !scene.textureAssets[material.textureAssetIndex].texture || !scene.lightingLinearSampler)
            {
                error = "alpha-tested R4 shadow caster base texture or sampler is unavailable";
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
            if (!backend.BindVertexBuffer(
                    6, scene.vertexBuffers[material.vertexBufferId], 0, error))
                return false;
            lastVertexBuffer = material.vertexBufferId;
        }
        R4IndexedDrawDesc draw;
        draw.indexBuffer = scene.indexBuffers[material.indexBufferId];
        draw.indexBufferOffset =
            static_cast<std::size_t>(material.indexBase) * sizeof(std::uint16_t);
        draw.indexCount = material.indexCount;
        draw.indexType = R4IndexType::UInt16;
        draw.baseVertex = static_cast<std::int32_t>(material.vertexBase);
        if (!backend.DrawIndexed(draw, error))
            return false;
    }
    return true;
}

bool drawSunMask(IR4Backend& backend, LevelScene& scene, std::string& error)
{
    if (!scene.productionSun || !scene.sunMaskPipeline ||
        !scene.sunMaskVertices || !scene.sunMaskIndices)
    {
        error = "production direct-sun mask resources are incomplete";
        return false;
    }
    R4MscStageBindings vertexBindings;
    vertexBindings.stage = R4ShaderStage::Vertex;
    vertexBindings.tlabSize = 24;
    vertexBindings.resources = {{
        R4MscResourceType::ConstantBuffer, 0,
        scene.baseLightingStaticGlobals, 0, 528, nullptr, nullptr,
    }};

    R4MscStageBindings fragmentBindings;
    fragmentBindings.stage = R4ShaderStage::Fragment;
    fragmentBindings.tlabSize = 72;
    R4MscResourceBinding position;
    position.type = R4MscResourceType::Texture;
    position.tlabOffset = 0;
    position.texture = scene.gBufferTargets[0];
    R4MscResourceBinding light;
    light.type = R4MscResourceType::ConstantBuffer;
    light.tlabOffset = 24;
    light.buffer = scene.sunMaskDynamicLight;
    light.bufferSize = 48;
    R4MscResourceBinding sampler;
    sampler.type = R4MscResourceType::Sampler;
    sampler.tlabOffset = 48;
    sampler.sampler = scene.lightingNoFilterSampler;
    fragmentBindings.resources = {position, light, sampler};

    if (!backend.BindGraphicsPipeline(scene.sunMaskPipeline, error) ||
        !backend.SetStencilReference(5, error) ||
        !backend.BindVertexBuffer(6, scene.sunMaskVertices, 0, error) ||
        !backend.BindMscResources(vertexBindings, error) ||
        !backend.BindMscResources(fragmentBindings, error))
        return false;
    R4IndexedDrawDesc draw;
    draw.indexBuffer = scene.sunMaskIndices;
    draw.indexCount = 6;
    draw.indexType = R4IndexType::UInt16;
    return backend.DrawIndexed(draw, error);
}

bool drawSunCascade(
    IR4Backend& backend,
    LevelScene& scene,
    bool farCascade,
    std::string& error)
{
    const std::shared_ptr<IR4GraphicsPipeline>& pipeline = farCascade ?
        scene.sunFarPipeline : scene.sunNearPipeline;
    if (!scene.productionSun || !pipeline || !scene.sunVolumeVertices ||
        !scene.sunVolumeIndices)
    {
        error = "production direct-sun cascade resources are incomplete";
        return false;
    }
    auto textureBinding = [](std::size_t offset, const std::shared_ptr<IR4Texture>& texture) {
        R4MscResourceBinding binding;
        binding.type = R4MscResourceType::Texture;
        binding.tlabOffset = offset;
        binding.texture = texture;
        return binding;
    };
    auto bufferBinding = [](std::size_t offset, const std::shared_ptr<IR4Buffer>& buffer,
                             std::size_t size) {
        R4MscResourceBinding binding;
        binding.type = R4MscResourceType::ConstantBuffer;
        binding.tlabOffset = offset;
        binding.buffer = buffer;
        binding.bufferSize = size;
        return binding;
    };
    auto samplerBinding = [](std::size_t offset, const std::shared_ptr<IR4Sampler>& sampler) {
        R4MscResourceBinding binding;
        binding.type = R4MscResourceType::Sampler;
        binding.tlabOffset = offset;
        binding.sampler = sampler;
        return binding;
    };

    R4MscStageBindings vertexBindings;
    vertexBindings.stage = R4ShaderStage::Vertex;
    vertexBindings.tlabSize = 48;
    vertexBindings.resources = {
        bufferBinding(0, scene.sunVertexGlobals, 224),
        bufferBinding(24, scene.sunVertexTransforms, 256),
    };

    R4MscStageBindings fragmentBindings;
    fragmentBindings.stage = R4ShaderStage::Fragment;
    fragmentBindings.tlabSize = 264;
    fragmentBindings.resources = {
        textureBinding(0, scene.gBufferTargets[0]),
        textureBinding(24, scene.gBufferTargets[1]),
        textureBinding(48, scene.sunMaskTexture),
        textureBinding(72, farCascade ? scene.sunSssTexture : scene.sunNearShadowTexture),
        textureBinding(96, farCascade ? scene.sunShadowTexture : scene.sunSssTexture),
        bufferBinding(120,
            farCascade ? scene.sunFarFragmentGlobals : scene.sunNearFragmentGlobals,
            farCascade ? 444 : 432),
        bufferBinding(144, scene.baseLightingStaticGlobals, 528),
        bufferBinding(168, scene.baseLightingDynamicLight, 48),
        samplerBinding(192, scene.lightingNoFilterSampler),
        samplerBinding(216, scene.lightingLinearSampler),
        samplerBinding(240, scene.sunShadowSampler),
    };

    if (!backend.BindGraphicsPipeline(pipeline, error) ||
        !backend.SetStencilReference(5, error) ||
        !backend.BindVertexBuffer(6, scene.sunVolumeVertices, 0, error) ||
        !backend.BindMscResources(vertexBindings, error) ||
        !backend.BindMscResources(fragmentBindings, error))
        return false;
    R4IndexedDrawDesc draw;
    draw.indexBuffer = scene.sunVolumeIndices;
    draw.indexCount = 48;
    draw.indexType = R4IndexType::UInt16;
    return backend.DrawIndexed(draw, error);
}

bool drawGBufferDisplay(IR4Backend& backend, LevelScene& scene, std::string& error)
{
    if (!scene.productionMsc || scene.gBufferTargets.size() != 4 ||
        !scene.gBufferDisplayPipeline || !scene.gBufferDisplayIndices)
    {
        error = "production G-buffer display resources are incomplete";
        return false;
    }
    const std::shared_ptr<IR4Texture> displayTexture = scene.productionBaseLighting ?
        (scene.lightingTargets.size() == 2 ? scene.lightingTargets[0] : nullptr) :
        scene.gBufferTargets[1];
    if (!displayTexture ||
        !backend.BindGraphicsPipeline(scene.gBufferDisplayPipeline, error) ||
        !backend.BindFragmentTexture(0, displayTexture, error) ||
        !backend.BindFragmentTexture(1, scene.gBufferTargets[0], error) ||
        !backend.BindFragmentSampler(0, scene.baseSampler, error))
        return false;
    R4IndexedDrawDesc draw;
    draw.indexBuffer = scene.gBufferDisplayIndices;
    draw.indexCount = 3;
    draw.indexType = R4IndexType::UInt16;
    return backend.DrawIndexed(draw, error);
}
} // namespace

static int parseFrameLimit(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--frames") != 0)
            continue;

        if (++i >= argc)
        {
            fprintf(stderr, "--frames requires a non-negative integer\n");
            return -1;
        }

        errno = 0;
        char* end = nullptr;
        const long value = strtol(argv[i], &end, 10);
        if (errno != 0 || end == argv[i] || *end != '\0' || value < 0 || value > INT_MAX)
        {
            fprintf(stderr, "invalid --frames value: %s\n", argv[i]);
            return -1;
        }
        return static_cast<int>(value);
    }
    return 0;
}

static bool parseLevelDirectory(int argc, char** argv, const char*& levelDirectory)
{
    levelDirectory = nullptr;
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--level-dir") != 0)
            continue;
        if (++i >= argc)
        {
            fprintf(stderr, "--level-dir requires a directory\n");
            return false;
        }
        levelDirectory = argv[i];
        return true;
    }
    return true;
}

struct TextureDiagnosticOptions
{
    std::vector<std::filesystem::path> ddsPaths;
    std::filesystem::path hemiDdsPath;
    std::filesystem::path mscVertexMetallibPath;
    std::filesystem::path mscFragmentMetallibPath;
    std::filesystem::path lightingVertexMetallibPath;
    std::filesystem::path lightingFragmentMetallibPath;
    std::filesystem::path sunShaderDirectory;
    std::uint32_t visualId = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t rootId = std::numeric_limits<std::uint32_t>::max();
};

static bool parseTextureDiagnostic(int argc, char** argv, TextureDiagnosticOptions& options)
{
    options = {};
    bool sawTextureVisual = false;
    bool sawTextureRoot = false;
    bool sawHemiDds = false;
    bool sawMscVertex = false;
    bool sawMscFragment = false;
    bool sawLightingVertex = false;
    bool sawLightingFragment = false;
    bool sawSunShaderDirectory = false;
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--texture-dds") == 0)
        {
            if (++i >= argc)
            {
                fprintf(stderr, "--texture-dds requires a file\n");
                return false;
            }
            options.ddsPaths.emplace_back(argv[i]);
        }
        else if (strcmp(argv[i], "--hemi-dds") == 0)
        {
            if (++i >= argc || sawHemiDds)
            {
                fprintf(stderr, "--hemi-dds requires one unique file\n");
                return false;
            }
            sawHemiDds = true;
            options.hemiDdsPath = argv[i];
        }
        else if (strcmp(argv[i], "--msc-vs-metallib") == 0 ||
            strcmp(argv[i], "--msc-ps-metallib") == 0)
        {
            const bool isVertex = strcmp(argv[i], "--msc-vs-metallib") == 0;
            bool& seen = isVertex ? sawMscVertex : sawMscFragment;
            if (++i >= argc || seen)
            {
                fprintf(stderr, "%s requires one unique file\n",
                    isVertex ? "--msc-vs-metallib" : "--msc-ps-metallib");
                return false;
            }
            seen = true;
            (isVertex ? options.mscVertexMetallibPath :
                options.mscFragmentMetallibPath) = argv[i];
        }
        else if (strcmp(argv[i], "--msc-lighting-vs-metallib") == 0 ||
            strcmp(argv[i], "--msc-lighting-ps-metallib") == 0)
        {
            const bool isVertex = strcmp(argv[i], "--msc-lighting-vs-metallib") == 0;
            bool& seen = isVertex ? sawLightingVertex : sawLightingFragment;
            if (++i >= argc || seen)
            {
                fprintf(stderr, "%s requires one unique file\n",
                    isVertex ? "--msc-lighting-vs-metallib" :
                        "--msc-lighting-ps-metallib");
                return false;
            }
            seen = true;
            (isVertex ? options.lightingVertexMetallibPath :
                options.lightingFragmentMetallibPath) = argv[i];
        }
        else if (strcmp(argv[i], "--msc-sun-dir") == 0)
        {
            if (++i >= argc || sawSunShaderDirectory)
            {
                fprintf(stderr, "--msc-sun-dir requires one unique directory\n");
                return false;
            }
            sawSunShaderDirectory = true;
            options.sunShaderDirectory = argv[i];
        }
        else if (strcmp(argv[i], "--texture-visual") == 0 ||
            strcmp(argv[i], "--texture-root") == 0)
        {
            const bool isRoot = strcmp(argv[i], "--texture-root") == 0;
            if (++i >= argc || (isRoot ? sawTextureRoot : sawTextureVisual))
            {
                fprintf(stderr, "%s requires one unique visual id\n",
                    isRoot ? "--texture-root" : "--texture-visual");
                return false;
            }
            errno = 0;
            char* end = nullptr;
            const unsigned long value = strtoul(argv[i], &end, 10);
            if (errno != 0 || end == argv[i] || *end != '\0' || value > UINT32_MAX)
            {
                fprintf(stderr, "invalid %s value: %s\n",
                    isRoot ? "--texture-root" : "--texture-visual", argv[i]);
                return false;
            }
            if (isRoot)
            {
                sawTextureRoot = true;
                options.rootId = static_cast<std::uint32_t>(value);
            }
            else
            {
                sawTextureVisual = true;
                options.visualId = static_cast<std::uint32_t>(value);
            }
        }
    }
    if (sawTextureVisual && sawTextureRoot)
    {
        fprintf(stderr, "--texture-visual and --texture-root are mutually exclusive\n");
        return false;
    }
    if (options.ddsPaths.empty() != (!sawTextureVisual && !sawTextureRoot))
    {
        fprintf(stderr, "--texture-dds requires --texture-visual or --texture-root, and vice versa\n");
        return false;
    }
    if (sawTextureVisual && options.ddsPaths.size() != 1u)
    {
        fprintf(stderr, "--texture-visual requires exactly one --texture-dds\n");
        return false;
    }
    if (sawHemiDds && (!sawTextureVisual || sawTextureRoot))
    {
        fprintf(stderr, "--hemi-dds requires single --texture-visual mode\n");
        return false;
    }
    if (sawMscVertex != sawMscFragment ||
        (sawMscVertex && (!sawHemiDds || !sawTextureVisual)))
    {
        fprintf(stderr, "production MSC mode requires both metallibs and the single hemi visual\n");
        return false;
    }
    if (sawLightingVertex != sawLightingFragment ||
        (sawLightingVertex && !sawMscVertex))
    {
        fprintf(stderr, "production base lighting requires both combine metallibs and MSC G-buffer mode\n");
        return false;
    }
    if (sawSunShaderDirectory && !sawLightingVertex)
    {
        fprintf(stderr, "production direct sun requires MSC base lighting mode\n");
        return false;
    }
    return true;
}

#ifndef XRAY_METAL_ENGINE_SCENE_IMPLEMENTATION
int main(int argc, char** argv)
{
    @autoreleasepool
    {
        const int frameLimit = parseFrameLimit(argc, argv);
        if (frameLimit < 0)
            return 2;
        const char* levelDirectory = nullptr;
        if (!parseLevelDirectory(argc, argv, levelDirectory))
            return 2;
        TextureDiagnosticOptions textureDiagnostic;
        if (!parseTextureDiagnostic(argc, argv, textureDiagnostic))
            return 2;
        if ((!textureDiagnostic.ddsPaths.empty() || !textureDiagnostic.hemiDdsPath.empty()) &&
            !levelDirectory)
        {
            fprintf(stderr, "texture diagnostic requires --level-dir\n");
            return 2;
        }

        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
        {
            fprintf(stderr, "METAL_BOOTSTRAP_ERROR SDL_Init: %s\n", SDL_GetError());
            return 1;
        }

        SDL_Window* window = SDL_CreateWindow(
            "S.T.A.L.K.E.R. Anomaly - Direct Metal Bootstrap",
            SDL_WINDOWPOS_CENTERED,
            SDL_WINDOWPOS_CENTERED,
            960,
            540,
            SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_METAL);
        if (!window)
        {
            fprintf(stderr, "METAL_BOOTSTRAP_ERROR SDL_CreateWindow: %s\n", SDL_GetError());
            SDL_Quit();
            return 1;
        }
        [NSApp activateIgnoringOtherApps:YES];
        [NSApp.windows.firstObject makeKeyAndOrderFront:nil];

        std::unique_ptr<IR4Backend> backend = CreateMetalR4Backend();
        std::string error;
        if (!backend->Initialize(window, 0u, 0u, error))
        {
            fprintf(stderr, "METAL_BOOTSTRAP_ERROR initialize: %s\n", error.c_str());
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }

        int logicalWidth = 0;
        int logicalHeight = 0;
        SDL_GetWindowSize(window, &logicalWidth, &logicalHeight);
        std::uint32_t drawableWidth = 0;
        std::uint32_t drawableHeight = 0;
        backend->DrawableSize(drawableWidth, drawableHeight);
        printf("METAL_BOOTSTRAP_READY device=\"%s\" logical=%dx%d drawable=%ux%u "
               "pixel_format=BGRA8Unorm frame_contexts=%u\n",
            backend->DeviceName(), logicalWidth, logicalHeight,
            drawableWidth, drawableHeight, backend->FrameContextCount());
        fflush(stdout);

        IR4Resources& resources = backend->Resources();
        float uniformData[36] = {};
        R4BufferDesc uniformDesc;
        uniformDesc.size = sizeof(uniformData);
        uniformDesc.storage = R4ResourceStorage::Shared;
        uniformDesc.usage = R4BufferUsage::Uniform;
        uniformDesc.label = "R4 shared uniform smoke";
        std::shared_ptr<IR4Buffer> uniformBuffer =
            resources.CreateBuffer(uniformDesc, uniformData, sizeof(uniformData), error);

        R4BufferDesc vertexDesc;
        vertexDesc.size = 3 * 4 * sizeof(float);
        vertexDesc.storage = R4ResourceStorage::Private;
        vertexDesc.usage = R4BufferUsage::Vertex;
        vertexDesc.label = "R4 private vertex smoke";
        std::shared_ptr<IR4Buffer> vertexBuffer =
            resources.CreateBuffer(vertexDesc, nullptr, 0, error);

        R4BufferDesc indexDesc;
        indexDesc.size = 3 * sizeof(std::uint16_t);
        indexDesc.storage = R4ResourceStorage::Private;
        indexDesc.usage = R4BufferUsage::Index;
        indexDesc.label = "R4 private index smoke";
        std::shared_ptr<IR4Buffer> indexBuffer =
            resources.CreateBuffer(indexDesc, nullptr, 0, error);

        R4Texture2DDesc colorDesc;
        colorDesc.width = 64;
        colorDesc.height = 64;
        colorDesc.format = R4PixelFormat::RGBA8Unorm;
        colorDesc.storage = R4ResourceStorage::Private;
        colorDesc.usage = R4TextureUsage::ShaderRead | R4TextureUsage::RenderTarget |
            R4TextureUsage::PixelFormatView;
        colorDesc.label = "R4 RGBA8 render target smoke";
        std::shared_ptr<IR4Texture> colorTexture = resources.CreateTexture2D(colorDesc, error);
        std::shared_ptr<IR4Texture> colorView =
            resources.CreateTextureView(colorTexture, R4PixelFormat::RGBA8Unorm, error);

        R4Texture2DDesc depthDesc;
        depthDesc.width = 64;
        depthDesc.height = 64;
        depthDesc.format = R4PixelFormat::Depth24UnormStencil8;
        depthDesc.storage = R4ResourceStorage::Private;
        depthDesc.usage = R4TextureUsage::RenderTarget;
        depthDesc.label = "R4 D24S8 logical depth smoke";
        std::shared_ptr<IR4Texture> depthTexture = resources.CreateTexture2D(depthDesc, error);

        R4Texture2DDesc bc1Desc;
        bc1Desc.width = 4;
        bc1Desc.height = 4;
        bc1Desc.format = R4PixelFormat::BC1Rgba;
        bc1Desc.storage = R4ResourceStorage::Private;
        bc1Desc.usage = R4TextureUsage::ShaderRead;
        bc1Desc.label = "R4 BC1 upload smoke";
        std::shared_ptr<IR4Texture> bc1Texture = resources.CreateTexture2D(bc1Desc, error);

        R4SamplerDesc samplerDesc;
        samplerDesc.maxAnisotropy = 8;
        samplerDesc.label = "R4 argument-buffer sampler smoke";
        std::shared_ptr<IR4Sampler> sampler = resources.CreateSampler(samplerDesc, error);

        if (!uniformBuffer || uniformBuffer->AllocationSize() != 256 || !vertexBuffer || !indexBuffer ||
            !colorTexture || !colorView || !depthTexture || !bc1Texture || !sampler)
        {
            fprintf(stderr, "METAL_BOOTSTRAP_ERROR resources: %s\n", error.c_str());
            backend->Shutdown();
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }

        std::unique_ptr<LevelScene> levelScene;
        if (levelDirectory)
        {
            levelScene = std::make_unique<LevelScene>();
            if (!prepareLevelScene(
                    levelDirectory,
                    textureDiagnostic.ddsPaths,
                    textureDiagnostic.hemiDdsPath,
                    {},
                    nullptr,
                    textureDiagnostic.mscVertexMetallibPath,
                    textureDiagnostic.mscFragmentMetallibPath,
                    {},
                    textureDiagnostic.lightingVertexMetallibPath,
                    textureDiagnostic.lightingFragmentMetallibPath,
                    textureDiagnostic.sunShaderDirectory,
                    {},
                    textureDiagnostic.visualId,
                    textureDiagnostic.rootId,
                    *backend,
                    *levelScene,
                    error))
            {
                fprintf(stderr, "METAL_BOOTSTRAP_ERROR level_prepare: %s\n", error.c_str());
                backend->Shutdown();
                SDL_DestroyWindow(window);
                SDL_Quit();
                return 1;
            }
            printf("METAL_LEVEL_READY source=real_xray_static level_dir=\"%s\" "
                   "vertex_buffers=%zu index_buffers=%zu visuals=%llu indices=%llu "
                   "pipeline_cache=%zu shader=%s depth=%s\n",
                levelDirectory,
                levelScene->vertexBuffers.size(), levelScene->indexBuffers.size(),
                static_cast<unsigned long long>(levelScene->drawableVisuals),
                static_cast<unsigned long long>(levelScene->drawableIndices),
                backend->GraphicsPipelineCacheSize(),
                levelScene->productionMsc ? "production_deffer_base_lmh_flat_msc" :
                    (levelScene->hemiDiagnostic ? "diagnostic_base_hemi_msl" :
                    (!textureDiagnostic.ddsPaths.empty() ?
                        "diagnostic_real_texture_msl" : "diagnostic_flat_msl")),
                levelScene->productionMsc ? "Depth32Float_Stencil8" : "Depth32Float");
            fflush(stdout);
            if (!levelScene->textureMaterials.empty())
            {
                const bool rootDiagnostic = levelScene->textureRootId !=
                    std::numeric_limits<std::uint32_t>::max();
                if (rootDiagnostic)
                {
                    printf("METAL_LEVEL_TEXTURE_ROOT_READY root=%u leaves=%zu materials=%zu "
                           "bc_supported=%s\n",
                        levelScene->textureRootId,
                        levelScene->textureMaterials.size(),
                        levelScene->textureAssets.size(),
                        backend->SupportsBCTextureCompression() ? "yes" : "no");
                }
                for (const LevelScene::TextureMaterial& material : levelScene->textureMaterials)
                {
                    const LevelScene::TextureAsset& asset =
                        levelScene->textureAssets[material.textureAssetIndex];
                    printf("%s visual=%u shader_id=%u shader=%s texture_list=\"%s\" "
                           "base_texture=%s dds=\"%s\" format=%s size=%ux%u mips=%u "
                           "vb=%u vbase=%u vcount=%u ib=%u ibase=%u icount=%u "
                           "uv=%s@%u scale=1/1024 base_uv_mode=%s T.a@%u B.a@%u "
                           "xform_translation=%.6f,%.6f,%.6f\n",
                        rootDiagnostic ? "METAL_LEVEL_TEXTURE_MATERIAL_READY" :
                            "METAL_LEVEL_TEXTURE_DIAG_READY",
                        material.visualId,
                        material.shaderId,
                        material.shaderName.c_str(),
                        material.textureList.c_str(),
                        material.virtualName.c_str(),
                        asset.ddsPath.c_str(),
                        pixelFormatName(asset.ddsTexture.descriptor.format),
                        asset.ddsTexture.descriptor.width,
                        asset.ddsTexture.descriptor.height,
                        asset.ddsTexture.descriptor.mipLevels,
                        material.vertexBufferId,
                        material.vertexBase,
                        material.vertexCount,
                        material.indexBufferId,
                        material.indexBase,
                        material.indexCount,
                        material.uvFormat == R4VertexFormat::Short4 ? "SHORT4" : "SHORT2",
                        material.uvOffset,
                        material.exactBaseUv ? "tc_plus_tangent_binormal_alpha" : "raw_tc",
                        material.tangentOffset,
                        material.binormalOffset,
                        material.modelTransform.elements[12],
                        material.modelTransform.elements[13],
                        material.modelTransform.elements[14]);
                }
                if (levelScene->hemiDiagnostic)
                {
                    const LevelScene::TextureMaterial& material = levelScene->textureMaterials.front();
                    const LevelScene::TextureAsset& hemiAsset =
                        levelScene->textureAssets[material.hemiTextureAssetIndex];
                    const auto& shader = levelScene->geometry.shaders[material.shaderId];
                    printf("METAL_LEVEL_HEMI_DIAG_READY visual=%u texture=%s dds=\"%s\" "
                           "format=%s size=%ux%u mips=%u uv=SHORT2@%u scale=1/32768 "
                           "sampler=clamp_linear_mip base_uv_offsets=T.a@%u,B.a@%u\n",
                        material.visualId,
                        shader.textures[2].c_str(),
                        hemiAsset.ddsPath.c_str(),
                        pixelFormatName(hemiAsset.ddsTexture.descriptor.format),
                        hemiAsset.ddsTexture.descriptor.width,
                        hemiAsset.ddsTexture.descriptor.height,
                        hemiAsset.ddsTexture.descriptor.mipLevels,
                        material.hemiUvOffset,
                        material.tangentOffset,
                        material.binormalOffset);
                }
                if (levelScene->productionMsc)
                {
                    printf("METAL_MSC_RUNTIME_READY visual=%u shader=deffer_base_lmh_flat "
                           "binding=automatic_linear reflection=admitted "
                           "vs_tlab=48 ps_tlab=144 "
                           "vertex_buffer=6 attribute_base=11 render_targets=4 "
                           "formats=RGBA16Float,RGBA8Unorm,RGBA8Unorm,RGBA16Float "
                           "depth=Depth32Float_Stencil8 stencil_geometry_ref=1\n",
                        levelScene->textureMaterials.front().visualId);
                }
                if (levelScene->productionBaseLighting)
                {
                    printf("METAL_BASE_LIGHTING_READY visual=%u shader=combine_1_nomsaa "
                           "binding=automatic_linear reflection=admitted "
                           "vs_tlab=24 ps_tlab=336 inputs=position,albedo,accumulator,"
                           "environment,ao,il,motion outputs=2xRGBA8Unorm "
                           "environment=controlled_neutral_cube direct_sun=%s\n",
                        levelScene->textureMaterials.front().visualId,
                        levelScene->productionSun ? "converted_far_cascade" : "pending");
                }
                if (levelScene->productionSun)
                {
                    printf("METAL_DIRECT_SUN_READY visual=%u shaders="
                           "accum_sun_mask_nomsaa,accum_sun_far_nomsaa "
                           "binding=automatic_linear reflection=admitted "
                           "mask_tlab=24/72 cascade_tlab=48/264 "
                           "stencil=geometry_1_light_5 accumulator=RGBA16Float "
                           "shadow=rendered_r32_2048 direct_shadowmap=converted\n",
                        levelScene->textureMaterials.front().visualId);
                }
                fflush(stdout);
            }
        }

        const std::uint8_t invalidUpload[8] = {};
        std::string negativeError;
        const bool invalidLinearAccepted = resources.UploadTexture2D(
            colorTexture, 0, 0, invalidUpload, 1, 1, 1, negativeError);
        negativeError.clear();
        const bool invalidBlockAccepted = resources.UploadTexture2D(
            bc1Texture, 0, 0, invalidUpload, sizeof(invalidUpload), 4, 8, negativeError);
        negativeError.clear();
        const bool depthUploadAccepted = resources.UploadTexture2D(
            depthTexture, 0, 0, invalidUpload, sizeof(invalidUpload), 8, 8, negativeError);
        if (invalidLinearAccepted || invalidBlockAccepted || depthUploadAccepted)
        {
            fprintf(stderr, "METAL_BOOTSTRAP_ERROR invalid resource upload was accepted\n");
            backend->Shutdown();
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }
        error.clear();

        bool running = true;
        bool runtimeFailed = false;
        std::uint64_t renderedFrames = 0;
        std::atomic<std::uint64_t> deferredReleaseCount{0};
        std::atomic<std::uint64_t> resourceReleaseCount{0};
        bool resourceUploadsSubmitted = false;
        bool levelDrawNegativeTested = false;

        while (running && (frameLimit == 0 || renderedFrames < static_cast<std::uint64_t>(frameLimit)))
        {
            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                if (event.type == SDL_QUIT)
                    running = false;
                else if (event.type == SDL_WINDOWEVENT &&
                    (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                     event.window.event == SDL_WINDOWEVENT_RESIZED))
                    backend->ResizeDrawable();
            }
            if (!running)
                break;

            @autoreleasepool
            {
                error.clear();
                const R4FrameAcquireResult acquireResult = backend->BeginFrame(error);
                if (acquireResult == R4FrameAcquireResult::Retry)
                {
                    SDL_Delay(1);
                    continue;
                }
                if (acquireResult == R4FrameAcquireResult::Error)
                {
                    fprintf(stderr, "METAL_BOOTSTRAP_ERROR begin_frame: %s\n", error.c_str());
                    runtimeFailed = true;
                    running = false;
                    break;
                }

                if (!resourceUploadsSubmitted)
                {
                    const float vertexData[12] = {
                        -1.0f, -1.0f, 0.0f, 1.0f,
                         0.0f,  1.0f, 0.0f, 1.0f,
                         1.0f, -1.0f, 0.0f, 1.0f,
                    };
                    const std::uint8_t bc1Block[8] = {0xff, 0xff, 0x00, 0x00, 0, 0, 0, 0};
                    const std::uint16_t indexData[3] = {0, 1, 2};
                    if (!resources.UpdateBuffer(
                            vertexBuffer, 0, vertexData, sizeof(vertexData), error) ||
                        !resources.UpdateBuffer(
                            indexBuffer, 0, indexData, sizeof(indexData), error) ||
                        !resources.UploadTexture2D(
                            bc1Texture, 0, 0, bc1Block, sizeof(bc1Block), 8, 8, error) ||
                        !backend->DeferRelease(
                            [uniformBuffer, vertexBuffer, indexBuffer, colorTexture, colorView, depthTexture,
                                bc1Texture, sampler, &resourceReleaseCount] {
                                resourceReleaseCount.fetch_add(1, std::memory_order_relaxed);
                            },
                            error))
                    {
                        fprintf(stderr, "METAL_BOOTSTRAP_ERROR resource_upload: %s\n", error.c_str());
                        runtimeFailed = true;
                        running = false;
                        break;
                    }

                    uniformBuffer.reset();
                    vertexBuffer.reset();
                    indexBuffer.reset();
                    colorTexture.reset();
                    colorView.reset();
                    depthTexture.reset();
                    bc1Texture.reset();
                    sampler.reset();
                    resourceUploadsSubmitted = true;
                    printf("METAL_RESOURCE_SMOKE_READY shared_uniform=144/256 private_vertex=48 "
                           "private_index=6 "
                           "rgba8_rt_view=ready logical_d24s8=Depth32Float_Stencil8 "
                           "bc1_upload=8 argument_buffer_sampler=ready invalid_layouts=rejected\n");
                }

                if (levelScene &&
                    (!ensureLevelDepthTarget(*backend, *levelScene, error) ||
                     !uploadLevelScene(*backend, *levelScene, error)))
                {
                    fprintf(stderr, "METAL_BOOTSTRAP_ERROR level_upload: %s\n", error.c_str());
                    runtimeFailed = true;
                    running = false;
                    break;
                }

                const double phase = static_cast<double>(renderedFrames % 240) / 239.0;
                const R4ClearColor clearColor = levelScene ?
                    R4ClearColor{0.025, 0.055, 0.105, 1.0} :
                    R4ClearColor{
                        0.025 + 0.035 * phase,
                        0.055 + 0.060 * phase,
                        0.105 + 0.120 * phase,
                        1.0,
                    };

                if (!backend->DeferRelease(
                        [&deferredReleaseCount] {
                            deferredReleaseCount.fetch_add(1, std::memory_order_relaxed);
                        },
                        error))
                {
                    fprintf(stderr, "METAL_BOOTSTRAP_ERROR frame: %s\n", error.c_str());
                    runtimeFailed = true;
                    running = false;
                    break;
                }

                if (levelScene && levelScene->productionSun)
                {
                    R4RenderPassDesc shadowPass;
                    shadowPass.label = "R4 converted direct-sun near shadow map";
                    shadowPass.useDrawable = false;
                    R4ColorAttachmentDesc shadowColor;
                    shadowColor.texture = levelScene->sunNearShadowTexture;
                    shadowColor.clearColor = {1.0, 1.0, 1.0, 1.0};
                    shadowPass.colorAttachments = {shadowColor};
                    shadowPass.depthStencilAttachment.texture =
                        levelScene->sunShadowDepthTarget;
                    shadowPass.depthStencilAttachment.depthStoreAction =
                        R4StoreAction::DontCare;
                    if (!backend->BeginRenderPass(shadowPass, error) ||
                        !drawSunShadowMap(*backend, *levelScene, true, error) ||
                        !backend->EndRenderPass(error))
                    {
                        fprintf(stderr, "METAL_BOOTSTRAP_ERROR direct-sun shadow: %s\n",
                            error.c_str());
                        runtimeFailed = true;
                        running = false;
                        break;
                    }
                    shadowPass.label = "R4 converted direct-sun far shadow map";
                    shadowPass.colorAttachments.front().texture = levelScene->sunShadowTexture;
                    if (!backend->BeginRenderPass(shadowPass, error) ||
                        !drawSunShadowMap(*backend, *levelScene, false, error) ||
                        !backend->EndRenderPass(error))
                    {
                        fprintf(stderr, "METAL_BOOTSTRAP_ERROR direct-sun far shadow: %s\n",
                            error.c_str());
                        runtimeFailed = true;
                        running = false;
                        break;
                    }
                }

                R4RenderPassDesc renderPass;
                renderPass.label = "R4 scene G-buffer";
                renderPass.drawableColor.clearColor = clearColor;
                if (levelScene)
                {
                    renderPass.depthStencilAttachment.texture = levelScene->depthTarget;
                    renderPass.depthStencilAttachment.depthStoreAction =
                        levelScene->productionMsc ? R4StoreAction::Store :
                                                    R4StoreAction::DontCare;
                    if (levelScene->productionMsc)
                    {
                        renderPass.useDrawable = false;
                        for (const std::shared_ptr<IR4Texture>& target :
                             levelScene->gBufferTargets)
                        {
                            R4ColorAttachmentDesc attachment;
                            attachment.texture = target;
                            attachment.clearColor = {0.0, 0.0, 0.0, 0.0};
                            renderPass.colorAttachments.emplace_back(std::move(attachment));
                        }
                    }
                }
                if (!backend->BeginRenderPass(renderPass, error))
                {
                    fprintf(stderr, "METAL_BOOTSTRAP_ERROR frame: %s\n", error.c_str());
                    runtimeFailed = true;
                    running = false;
                    break;
                }
                if (levelScene && !levelDrawNegativeTested)
                {
                    R4IndexedDrawDesc invalidDraw;
                    invalidDraw.indexBuffer = levelScene->indexBuffers.front();
                    invalidDraw.indexBufferOffset = 1;
                    invalidDraw.indexCount = 1;
                    if (backend->DrawIndexed(invalidDraw, error))
                    {
                        fprintf(stderr, "METAL_BOOTSTRAP_ERROR misaligned index offset was accepted\n");
                        runtimeFailed = true;
                        running = false;
                        break;
                    }
                    error.clear();
                    levelDrawNegativeTested = true;
                }
                if ((levelScene && levelScene->productionMsc &&
                        !backend->SetStencilReference(1, error)) ||
                    (levelScene && !drawLevelScene(*backend, *levelScene, error)) ||
                    !backend->EndRenderPass(error))
                {
                    fprintf(stderr, "METAL_BOOTSTRAP_ERROR frame: %s\n", error.c_str());
                    runtimeFailed = true;
                    running = false;
                    break;
                }
                if (levelScene && levelScene->productionBaseLighting)
                {
                    R4RenderPassDesc accumulatorPass;
                    accumulatorPass.label = "R4 light accumulator clear";
                    accumulatorPass.useDrawable = false;
                    R4ColorAttachmentDesc accumulatorAttachment;
                    accumulatorAttachment.texture = levelScene->lightingAccumulator;
                    accumulatorAttachment.clearColor = {0.0, 0.0, 0.0, 0.0};
                    accumulatorPass.colorAttachments = {accumulatorAttachment};
                    if (!backend->BeginRenderPass(accumulatorPass, error) ||
                        !backend->EndRenderPass(error))
                    {
                        fprintf(stderr, "METAL_BOOTSTRAP_ERROR accumulator clear: %s\n",
                            error.c_str());
                        runtimeFailed = true;
                        running = false;
                        break;
                    }

                    if (levelScene->productionSun)
                    {
                        R4ColorAttachmentDesc sunAccumulatorAttachment;
                        sunAccumulatorAttachment.texture = levelScene->lightingAccumulator;
                        sunAccumulatorAttachment.loadAction = R4LoadAction::Load;
                        sunAccumulatorAttachment.storeAction = R4StoreAction::Store;
                        R4DepthStencilAttachmentDesc sunDepthStencil;
                        sunDepthStencil.texture = levelScene->depthTarget;
                        sunDepthStencil.depthLoadAction = R4LoadAction::Load;
                        sunDepthStencil.depthStoreAction = R4StoreAction::Store;
                        sunDepthStencil.stencilLoadAction = R4LoadAction::Load;
                        sunDepthStencil.stencilStoreAction = R4StoreAction::Store;

                        R4RenderPassDesc maskPass;
                        maskPass.label = "R4 converted direct-sun stencil mask";
                        maskPass.useDrawable = false;
                        maskPass.colorAttachments = {sunAccumulatorAttachment};
                        maskPass.depthStencilAttachment = sunDepthStencil;
                        if (!backend->BeginRenderPass(maskPass, error) ||
                            !drawSunMask(*backend, *levelScene, error) ||
                            !backend->EndRenderPass(error))
                        {
                            fprintf(stderr, "METAL_BOOTSTRAP_ERROR direct-sun mask: %s\n",
                                error.c_str());
                            runtimeFailed = true;
                            running = false;
                            break;
                        }

                        R4RenderPassDesc sunPass;
                        sunPass.label = "R4 converted direct-sun near cascade";
                        sunPass.useDrawable = false;
                        sunPass.colorAttachments = {sunAccumulatorAttachment};
                        sunPass.depthStencilAttachment = sunDepthStencil;
                        if (!backend->BeginRenderPass(sunPass, error) ||
                            !drawSunCascade(*backend, *levelScene, false, error) ||
                            !backend->EndRenderPass(error))
                        {
                            fprintf(stderr, "METAL_BOOTSTRAP_ERROR direct-sun near cascade: %s\n",
                                error.c_str());
                            runtimeFailed = true;
                            running = false;
                            break;
                        }
                        sunPass.label = "R4 converted direct-sun far cascade";
                        if (!backend->BeginRenderPass(sunPass, error) ||
                            !drawSunCascade(*backend, *levelScene, true, error) ||
                            !backend->EndRenderPass(error))
                        {
                            fprintf(stderr, "METAL_BOOTSTRAP_ERROR direct-sun cascade: %s\n",
                                error.c_str());
                            runtimeFailed = true;
                            running = false;
                            break;
                        }
                    }

                    R4RenderPassDesc lightingPass;
                    lightingPass.label = "R4 converted combine_1 base lighting";
                    lightingPass.useDrawable = false;
                    for (const std::shared_ptr<IR4Texture>& target :
                         levelScene->lightingTargets)
                    {
                        R4ColorAttachmentDesc attachment;
                        attachment.texture = target;
                        attachment.clearColor = {0.0, 0.0, 0.0, 0.0};
                        lightingPass.colorAttachments.emplace_back(std::move(attachment));
                    }
                    if (!backend->BeginRenderPass(lightingPass, error) ||
                        !drawBaseLighting(*backend, *levelScene, error) ||
                        !backend->EndRenderPass(error))
                    {
                        fprintf(stderr, "METAL_BOOTSTRAP_ERROR base lighting: %s\n",
                            error.c_str());
                        runtimeFailed = true;
                        running = false;
                        break;
                    }
                }
                if (levelScene && levelScene->productionMsc)
                {
                    R4RenderPassDesc displayPass;
                    displayPass.label = "R4 diagnostic presentation";
                    displayPass.drawableColor.clearColor = clearColor;
                    if (!backend->BeginRenderPass(displayPass, error) ||
                        !drawGBufferDisplay(*backend, *levelScene, error) ||
                        !backend->EndRenderPass(error))
                    {
                        fprintf(stderr, "METAL_BOOTSTRAP_ERROR G-buffer display: %s\n",
                            error.c_str());
                        runtimeFailed = true;
                        running = false;
                        break;
                    }
                }
                if (!backend->SubmitFrame(error))
                {
                    fprintf(stderr, "METAL_BOOTSTRAP_ERROR frame: %s\n", error.c_str());
                    runtimeFailed = true;
                    running = false;
                    break;
                }

                ++renderedFrames;
            }
        }

        backend->Shutdown();
        backend->DrawableSize(drawableWidth, drawableHeight);
        const std::uint64_t submittedSerial = backend->LastSubmittedSerial();
        const std::uint64_t completedSerial = backend->CompletedSerial();
        const bool commandBufferError = backend->HasCommandBufferError();
        const std::uint64_t released = deferredReleaseCount.load(std::memory_order_relaxed);
        const std::uint64_t resourceReleased = resourceReleaseCount.load(std::memory_order_relaxed);

        if (runtimeFailed || commandBufferError || completedSerial != submittedSerial ||
            released != renderedFrames || resourceReleased != 1 ||
            (levelScene && !levelDrawNegativeTested))
        {
            fprintf(stderr,
                "METAL_BOOTSTRAP_ERROR drain submitted=%llu completed=%llu deferred=%llu "
                "resource_deferred=%llu frames=%llu\n",
                static_cast<unsigned long long>(submittedSerial),
                static_cast<unsigned long long>(completedSerial),
                static_cast<unsigned long long>(released),
                static_cast<unsigned long long>(resourceReleased),
                static_cast<unsigned long long>(renderedFrames));
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }

        printf("METAL_BOOTSTRAP_COMPLETE frames=%llu submitted_serial=%llu completed_serial=%llu "
               "deferred_releases=%llu resource_deferred_releases=%llu\n",
            static_cast<unsigned long long>(renderedFrames),
            static_cast<unsigned long long>(submittedSerial),
            static_cast<unsigned long long>(completedSerial),
            static_cast<unsigned long long>(released),
            static_cast<unsigned long long>(resourceReleased));
        fflush(stdout);

        if (levelScene)
        {
            printf("METAL_LEVEL_COMPLETE source=real_xray_static frames=%llu visuals_per_frame=%llu "
                   "indices_per_frame=%llu pipeline_cache=%zu depth=%s "
                   "invalid_index_offset=rejected materials=%s\n",
                static_cast<unsigned long long>(renderedFrames),
                static_cast<unsigned long long>(levelScene->drawableVisuals),
                static_cast<unsigned long long>(levelScene->drawableIndices),
                backend->GraphicsPipelineCacheSize(),
                levelScene->productionMsc ? "Depth32Float_Stencil8" : "Depth32Float",
                levelScene->productionMsc ? "production_msc_lmh_gbuffer" :
                    (levelScene->hemiDiagnostic ? "base_plus_hemi_diagnostic" :
                    (!levelScene->textureMaterials.empty() ?
                        "base_texture_diagnostic" : "not_yet_bound")));
            fflush(stdout);
            if (!levelScene->textureMaterials.empty())
            {
                std::size_t uploadedMips = 0;
                std::uint64_t uploadedBytes = 0;
                for (const LevelScene::TextureAsset& asset : levelScene->textureAssets)
                {
                    uploadedMips += asset.ddsTexture.mipLevels.size();
                    uploadedBytes += asset.uploadedBytes;
                }
                if (levelScene->textureRootId != std::numeric_limits<std::uint32_t>::max())
                {
                    printf("METAL_LEVEL_TEXTURE_ROOT_COMPLETE root=%u leaves=%zu materials=%zu "
                           "frames=%llu indices_per_frame=%llu uploaded_mips=%zu "
                           "uploaded_bytes=%llu sampler=repeat_linear_mip result=PASS\n",
                        levelScene->textureRootId,
                        levelScene->textureMaterials.size(),
                        levelScene->textureAssets.size(),
                        static_cast<unsigned long long>(renderedFrames),
                        static_cast<unsigned long long>(levelScene->drawableIndices),
                        uploadedMips,
                        static_cast<unsigned long long>(uploadedBytes));
                }
                else
                {
                    const LevelScene::TextureMaterial& material = levelScene->textureMaterials.front();
                    printf("METAL_LEVEL_TEXTURE_DIAG_COMPLETE visual=%u shader_id=%u "
                           "base_texture=%s frames=%llu indices_per_frame=%llu "
                           "uploaded_mips=%zu uploaded_bytes=%llu sampler=repeat_linear_mip "
                           "result=PASS\n",
                        material.visualId,
                        material.shaderId,
                        material.virtualName.c_str(),
                        static_cast<unsigned long long>(renderedFrames),
                        static_cast<unsigned long long>(levelScene->drawableIndices),
                        uploadedMips,
                        static_cast<unsigned long long>(uploadedBytes));
                    if (levelScene->hemiDiagnostic)
                    {
                        const LevelScene::TextureAsset& hemiAsset =
                            levelScene->textureAssets[material.hemiTextureAssetIndex];
                        printf("METAL_LEVEL_HEMI_DIAG_COMPLETE visual=%u frames=%llu "
                               "hemi_texture=%s hemi_mips=%zu hemi_bytes=%llu "
                               "uv_scale=1/32768 sampler=clamp_linear_mip result=PASS\n",
                            material.visualId,
                            static_cast<unsigned long long>(renderedFrames),
                            levelScene->geometry.shaders[material.shaderId].textures[2].c_str(),
                            hemiAsset.ddsTexture.mipLevels.size(),
                            static_cast<unsigned long long>(hemiAsset.uploadedBytes));
                    }
                    if (levelScene->productionMsc)
                    {
                        printf("METAL_GBUFFER_COMPLETE visual=%u shader=deffer_base_lmh_flat "
                               "binding=msc_automatic_linear render_targets=4 "
                               "formats=RGBA16Float,RGBA8Unorm,RGBA8Unorm,RGBA16Float "
                               "depth=Depth32Float_Stencil8 stencil_geometry_ref=1 "
                               "displayed=%s result=PASS\n",
                            material.visualId,
                            levelScene->productionBaseLighting ?
                                "combine_1_low" : "albedo_gloss");
                    }
                    if (levelScene->productionBaseLighting)
                    {
                        printf("METAL_BASE_LIGHTING_COMPLETE visual=%u frames=%llu "
                               "shader=combine_1_nomsaa gbuffer=position,albedo "
                               "accumulator=RGBA16Float outputs=2xRGBA8Unorm "
                               "displayed=combine_1_low environment=controlled_neutral_cube "
                               "direct_sun=%s result=PASS\n",
                            material.visualId,
                            static_cast<unsigned long long>(renderedFrames),
                            levelScene->productionSun ? "converted_far_cascade" : "pending");
                    }
                    if (levelScene->productionSun)
                    {
                        printf("METAL_DIRECT_SUN_COMPLETE visual=%u frames=%llu "
                               "mask=accum_sun_mask_nomsaa "
                               "cascade=accum_sun_far_nomsaa "
                               "stencil=geometry_1_light_5 accumulator=RGBA16Float "
                               "shadow=rendered_r32_2048 direct_shadowmap=converted "
                               "result=PASS\n",
                            material.visualId,
                            static_cast<unsigned long long>(renderedFrames));
                    }
                }
                fflush(stdout);
            }
        }

        levelScene.reset();
        backend.reset();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
    }
}
#endif // XRAY_METAL_ENGINE_SCENE_IMPLEMENTATION
