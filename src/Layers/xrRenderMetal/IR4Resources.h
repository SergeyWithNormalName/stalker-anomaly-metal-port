#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

enum class R4ResourceStorage
{
    Shared,
    Private,
};

enum class R4BufferUsage
{
    Vertex,
    Index,
    Uniform,
    Generic,
};

struct R4BufferDesc
{
    std::size_t size = 0;
    R4ResourceStorage storage = R4ResourceStorage::Private;
    R4BufferUsage usage = R4BufferUsage::Generic;
    const char* label = nullptr;
};

class IR4Buffer
{
public:
    virtual ~IR4Buffer() = default;
    virtual std::size_t Size() const = 0;
    virtual std::size_t AllocationSize() const = 0;
    virtual R4ResourceStorage Storage() const = 0;
};

enum class R4PixelFormat
{
    Invalid,
    A8Unorm,
    R8Unorm,
    RG8Unorm,
    RG8Snorm,
    RGBA8Unorm,
    RGBA8UnormSrgb,
    RGBA8Snorm,
    BGRA8Unorm,
    BGRA8UnormSrgb,
    R16Unorm,
    R16Float,
    RG16Unorm,
    RG16Float,
    RGBA16Unorm,
    RGBA16Float,
    R32Float,
    RG32Float,
    RGBA32Float,
    RGB10A2Unorm,
    RG11B10Float,
    BC1Rgba,
    BC1RgbaSrgb,
    BC2Rgba,
    BC2RgbaSrgb,
    BC3Rgba,
    BC3RgbaSrgb,
    BC4RUnorm,
    BC4RSnorm,
    BC5RgUnorm,
    BC5RgSnorm,
    BC6hRgbFloat,
    BC6hRgbUfloat,
    BC7RgbaUnorm,
    BC7RgbaUnormSrgb,
    Depth16Unorm,
    Depth24UnormStencil8,
    Depth32Float,
    Depth32FloatStencil8,
};

enum class R4TextureUsage : std::uint32_t
{
    None = 0,
    ShaderRead = 1u << 0u,
    ShaderWrite = 1u << 1u,
    RenderTarget = 1u << 2u,
    PixelFormatView = 1u << 3u,
};

enum class R4TextureType
{
    Texture2D,
    Texture2DArray,
    TextureCube,
};

inline R4TextureUsage operator|(R4TextureUsage left, R4TextureUsage right)
{
    return static_cast<R4TextureUsage>(
        static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
}

inline bool HasR4TextureUsage(R4TextureUsage value, R4TextureUsage bit)
{
    return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(bit)) != 0;
}

struct R4Texture2DDesc
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mipLevels = 1;
    // TextureCube uses one cube here; its six faces are addressed through
    // UploadTexture2D's arraySlice 0..5 rather than as an array of six 2D
    // textures. Texture2DArray uses its ordinary slice count.
    std::uint32_t arrayLength = 1;
    R4TextureType type = R4TextureType::Texture2D;
    R4PixelFormat format = R4PixelFormat::Invalid;
    R4ResourceStorage storage = R4ResourceStorage::Private;
    R4TextureUsage usage = R4TextureUsage::ShaderRead;
    const char* label = nullptr;
};

class IR4Texture
{
public:
    virtual ~IR4Texture() = default;
    virtual const R4Texture2DDesc& Descriptor() const = 0;
};

enum class R4SamplerFilter
{
    Nearest,
    Linear,
};

enum class R4SamplerAddressMode
{
    ClampToEdge,
    Repeat,
    MirrorRepeat,
    ClampToZero,
    ClampToBorder,
};

enum class R4SamplerCompareFunction
{
    Never,
    Less,
    LessEqual,
    Equal,
    GreaterEqual,
    Greater,
    Always,
};

enum class R4SamplerBorderColor
{
    TransparentBlack,
    OpaqueBlack,
    OpaqueWhite,
};

struct R4SamplerDesc
{
    R4SamplerFilter minFilter = R4SamplerFilter::Linear;
    R4SamplerFilter magFilter = R4SamplerFilter::Linear;
    R4SamplerFilter mipFilter = R4SamplerFilter::Linear;
    R4SamplerAddressMode addressU = R4SamplerAddressMode::Repeat;
    R4SamplerAddressMode addressV = R4SamplerAddressMode::Repeat;
    R4SamplerAddressMode addressW = R4SamplerAddressMode::Repeat;
    R4SamplerCompareFunction compareFunction = R4SamplerCompareFunction::Never;
    R4SamplerBorderColor borderColor = R4SamplerBorderColor::TransparentBlack;
    std::uint32_t maxAnisotropy = 1;
    const char* label = nullptr;
};

class IR4Sampler
{
public:
    virtual ~IR4Sampler() = default;
};

class IR4Resources
{
public:
    virtual ~IR4Resources() = default;

    virtual std::shared_ptr<IR4Buffer> CreateBuffer(
        const R4BufferDesc& descriptor,
        const void* initialData,
        std::size_t initialDataSize,
        std::string& error) = 0;
    virtual bool UpdateBuffer(
        const std::shared_ptr<IR4Buffer>& buffer,
        std::size_t offset,
        const void* data,
        std::size_t dataSize,
        std::string& error) = 0;

    virtual std::shared_ptr<IR4Texture> CreateTexture2D(
        const R4Texture2DDesc& descriptor,
        std::string& error) = 0;
    virtual std::shared_ptr<IR4Texture> CreateTextureView(
        const std::shared_ptr<IR4Texture>& texture,
        R4PixelFormat viewFormat,
        std::string& error) = 0;
    virtual bool UploadTexture2D(
        const std::shared_ptr<IR4Texture>& texture,
        std::uint32_t mipLevel,
        // TextureCube accepts its six canonical face slices 0..5.
        std::uint32_t arraySlice,
        const void* data,
        std::size_t dataSize,
        std::size_t bytesPerRow,
        std::size_t bytesPerImage,
        std::string& error) = 0;

    virtual std::shared_ptr<IR4Sampler> CreateSampler(
        const R4SamplerDesc& descriptor,
        std::string& error) = 0;
};
