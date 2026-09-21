#pragma once

#include "IR4Resources.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

enum class R4VertexFormat
{
    Float,
    Float2,
    Float3,
    Float4,
    Int2,
    Int4,
    UChar4,
    UChar4Normalized,
    Short2,
    Short2Normalized,
    Short4,
    Short4Normalized,
    UShort2Normalized,
    UShort4Normalized,
    Half2,
    Half4,
};

struct R4VertexAttributeDesc
{
    std::uint32_t attributeIndex = 0;
    std::uint32_t bufferIndex = 0;
    std::size_t offset = 0;
    R4VertexFormat format = R4VertexFormat::Float3;
};

struct R4VertexBufferLayoutDesc
{
    std::uint32_t bufferIndex = 0;
    std::size_t stride = 0;
};

enum class R4CompareFunction
{
    Never,
    Less,
    LessEqual,
    Equal,
    GreaterEqual,
    Greater,
    Always,
};

enum class R4CullMode
{
    None,
    Front,
    Back,
};

enum class R4Winding
{
    Clockwise,
    CounterClockwise,
};

enum class R4BlendFactor
{
    Zero,
    One,
    SourceAlpha,
    OneMinusSourceAlpha,
};

enum class R4StencilOperation
{
    Keep,
    Zero,
    Replace,
    IncrementClamp,
    DecrementClamp,
    Invert,
    IncrementWrap,
    DecrementWrap,
};

struct R4StencilFaceDesc
{
    R4CompareFunction compare = R4CompareFunction::Always;
    R4StencilOperation stencilFailure = R4StencilOperation::Keep;
    R4StencilOperation depthFailure = R4StencilOperation::Keep;
    R4StencilOperation depthStencilPass = R4StencilOperation::Keep;
    std::uint32_t readMask = 0xff;
    std::uint32_t writeMask = 0xff;
};

struct R4StencilDesc
{
    bool enabled = false;
    R4StencilFaceDesc frontFace;
    R4StencilFaceDesc backFace;
};

struct R4BlendDesc
{
    bool enabled = false;
    R4BlendFactor sourceRgb = R4BlendFactor::One;
    R4BlendFactor destinationRgb = R4BlendFactor::Zero;
    R4BlendFactor sourceAlpha = R4BlendFactor::One;
    R4BlendFactor destinationAlpha = R4BlendFactor::Zero;
};

struct R4GraphicsPipelineDesc
{
    const char* label = nullptr;
    const char* shaderKey = nullptr;
    const char* mslSource = nullptr;
    const char* vertexFunction = nullptr;
    const char* fragmentFunction = nullptr;
    // Offline Metal Shader Converter output. When both paths are set the
    // backend loads the single exported function from each metallib instead
    // of compiling the diagnostic MSL source above.
    const char* vertexMetallibPath = nullptr;
    const char* fragmentMetallibPath = nullptr;
    std::vector<R4VertexAttributeDesc> vertexAttributes;
    std::vector<R4VertexBufferLayoutDesc> vertexLayouts;
    R4PixelFormat colorFormat = R4PixelFormat::BGRA8Unorm;
    std::vector<R4PixelFormat> colorFormats;
    R4PixelFormat depthFormat = R4PixelFormat::Invalid;
    std::uint32_t sampleCount = 1;
    R4BlendDesc blend;
    // Empty means RGBA for every attachment. Otherwise one mask per color
    // format, using Metal's R/G/B/A bit values (0x1/0x2/0x4/0x8).
    std::vector<std::uint8_t> colorWriteMasks;
    R4CompareFunction depthCompare = R4CompareFunction::Always;
    bool depthWrite = false;
    R4StencilDesc stencil;
    R4CullMode cullMode = R4CullMode::Back;
    R4Winding frontFace = R4Winding::CounterClockwise;
};

class IR4GraphicsPipeline
{
public:
    virtual ~IR4GraphicsPipeline() = default;
};

enum class R4IndexType
{
    UInt16,
    UInt32,
};

struct R4IndexedDrawDesc
{
    std::shared_ptr<IR4Buffer> indexBuffer;
    std::size_t indexBufferOffset = 0;
    std::uint32_t indexCount = 0;
    R4IndexType indexType = R4IndexType::UInt16;
    std::int32_t baseVertex = 0;
    std::uint32_t instanceCount = 1;
};
