#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace xray::metal
{
namespace screenshot
{
constexpr std::uint32_t ddsMagic = 0x20534444u; // "DDS "
constexpr std::uint32_t ddsHeaderFlagsTexture = 0x00001007u;
constexpr std::uint32_t ddsHeaderFlagsLinearSize = 0x00080000u;
constexpr std::uint32_t ddsPixelFormatFourCC = 0x00000004u;
constexpr std::uint32_t ddsCapsTexture = 0x00001000u;
constexpr std::uint32_t dxt1FourCC = 0x31545844u; // "DXT1"

struct DdsPixelFormat
{
    std::uint32_t size;
    std::uint32_t flags;
    std::uint32_t fourCC;
    std::uint32_t rgbBitCount;
    std::uint32_t redBitMask;
    std::uint32_t greenBitMask;
    std::uint32_t blueBitMask;
    std::uint32_t alphaBitMask;
};

struct DdsHeader
{
    std::uint32_t size;
    std::uint32_t flags;
    std::uint32_t height;
    std::uint32_t width;
    std::uint32_t pitchOrLinearSize;
    std::uint32_t depth;
    std::uint32_t mipMapCount;
    std::uint32_t reserved1[11];
    DdsPixelFormat pixelFormat;
    std::uint32_t caps;
    std::uint32_t caps2;
    std::uint32_t caps3;
    std::uint32_t caps4;
    std::uint32_t reserved2;
};

static_assert(sizeof(DdsPixelFormat) == 32, "DDS pixel format layout changed");
static_assert(sizeof(DdsHeader) == 124, "DDS header layout changed");

inline std::vector<std::uint8_t> BuildDxt1Placeholder(std::uint32_t width, std::uint32_t height)
{
    width = width == 0 ? 1u : width;
    height = height == 0 ? 1u : height;

    const std::uint64_t blockWidth = (static_cast<std::uint64_t>(width) + 3u) / 4u;
    const std::uint64_t blockHeight = (static_cast<std::uint64_t>(height) + 3u) / 4u;
    const std::uint64_t dataSize = blockWidth * blockHeight * 8u;
    if (dataSize > std::numeric_limits<std::uint32_t>::max() ||
        dataSize > std::numeric_limits<std::size_t>::max() - sizeof(std::uint32_t) - sizeof(DdsHeader))
        return {};

    std::vector<std::uint8_t> image(sizeof(std::uint32_t) + sizeof(DdsHeader) + static_cast<std::size_t>(dataSize));
    DdsHeader header{};
    header.size = sizeof(DdsHeader);
    header.flags = ddsHeaderFlagsTexture | ddsHeaderFlagsLinearSize;
    header.height = height;
    header.width = width;
    header.pitchOrLinearSize = static_cast<std::uint32_t>(dataSize);
    header.pixelFormat = {sizeof(DdsPixelFormat), ddsPixelFormatFourCC, dxt1FourCC, 0, 0, 0, 0, 0};
    header.caps = ddsCapsTexture;
    std::memcpy(image.data(), &ddsMagic, sizeof(ddsMagic));
    std::memcpy(image.data() + sizeof(std::uint32_t), &header, sizeof(header));

    // A single opaque dark-grey DXT1 block. Every block chooses endpoint zero.
    constexpr std::uint16_t grey565 = 0x39e7u;
    std::uint8_t* block = image.data() + sizeof(std::uint32_t) + sizeof(DdsHeader);
    for (std::size_t offset = 0; offset < static_cast<std::size_t>(dataSize); offset += 8u)
    {
        block[offset] = static_cast<std::uint8_t>(grey565 & 0xffu);
        block[offset + 1u] = static_cast<std::uint8_t>(grey565 >> 8u);
    }
    return image;
}

inline std::vector<std::uint8_t> BuildTgaPlaceholder(std::uint16_t width, std::uint16_t height)
{
    width = width == 0 ? 1u : width;
    height = height == 0 ? 1u : height;
    const std::size_t pixelCount = static_cast<std::size_t>(width) * height;
    if (pixelCount > (std::numeric_limits<std::size_t>::max() - 18u) / 3u)
        return {};

    std::vector<std::uint8_t> image(18u + pixelCount * 3u, 0x20u);
    image[2] = 2u; // Uncompressed true-color image.
    image[12] = static_cast<std::uint8_t>(width & 0xffu);
    image[13] = static_cast<std::uint8_t>(width >> 8u);
    image[14] = static_cast<std::uint8_t>(height & 0xffu);
    image[15] = static_cast<std::uint8_t>(height >> 8u);
    image[16] = 24u;
    image[17] = 0x20u; // Top-left origin.
    return image;
}
} // namespace screenshot
} // namespace xray::metal
