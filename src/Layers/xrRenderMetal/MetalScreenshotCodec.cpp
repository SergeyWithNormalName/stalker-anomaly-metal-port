#include "MetalScreenshotCodec.h"

#include <array>
#include <cstring>
#include <limits>

namespace xray::metal::screenshot
{
namespace
{
constexpr std::uint32_t DdsMagic = 0x20534444u; // "DDS " in little endian.
constexpr std::uint32_t DdsHeaderFlagsTexture = 0x00001007u;
constexpr std::uint32_t DdsHeaderFlagsLinearSize = 0x00080000u;
constexpr std::uint32_t DdsPixelFormatFourCC = 0x00000004u;
constexpr std::uint32_t DdsCapsTexture = 0x00001000u;
constexpr std::uint32_t Dxt1FourCC = 0x31545844u; // "DXT1" in little endian.
constexpr std::size_t Dxt1BlockBytes = 8u;

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

static_assert(sizeof(DdsPixelFormat) == 32u, "DDS pixel-format layout changed");
static_assert(sizeof(DdsHeader) == 124u, "DDS header layout changed");

struct Rgb8
{
    std::uint8_t red;
    std::uint8_t green;
    std::uint8_t blue;
};

std::uint16_t packRgb565(const Rgb8 color)
{
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(color.red >> 3u) << 11u) |
        (static_cast<std::uint16_t>(color.green >> 2u) << 5u) |
        static_cast<std::uint16_t>(color.blue >> 3u));
}

Rgb8 unpackRgb565(const std::uint16_t color)
{
    const std::uint8_t red = static_cast<std::uint8_t>((color >> 11u) & 0x1fu);
    const std::uint8_t green = static_cast<std::uint8_t>((color >> 5u) & 0x3fu);
    const std::uint8_t blue = static_cast<std::uint8_t>(color & 0x1fu);
    return {
        static_cast<std::uint8_t>((red << 3u) | (red >> 2u)),
        static_cast<std::uint8_t>((green << 2u) | (green >> 4u)),
        static_cast<std::uint8_t>((blue << 3u) | (blue >> 2u)),
    };
}

Rgb8 interpolate(const Rgb8 first, const Rgb8 second, const unsigned firstWeight,
    const unsigned secondWeight)
{
    return {
        static_cast<std::uint8_t>((first.red * firstWeight + second.red * secondWeight) / 3u),
        static_cast<std::uint8_t>((first.green * firstWeight + second.green * secondWeight) / 3u),
        static_cast<std::uint8_t>((first.blue * firstWeight + second.blue * secondWeight) / 3u),
    };
}

std::uint32_t squaredDistance(const Rgb8 left, const Rgb8 right)
{
    const int red = static_cast<int>(left.red) - static_cast<int>(right.red);
    const int green = static_cast<int>(left.green) - static_cast<int>(right.green);
    const int blue = static_cast<int>(left.blue) - static_cast<int>(right.blue);
    return static_cast<std::uint32_t>(red * red + green * green + blue * blue);
}

Rgb8 readPixel(const std::uint8_t* pixels, const std::size_t rowBytes, const std::uint32_t x,
    const std::uint32_t y, const SourcePixelFormat format)
{
    const std::uint8_t* pixel = pixels + static_cast<std::size_t>(y) * rowBytes +
        static_cast<std::size_t>(x) * 4u;
    if (format == SourcePixelFormat::BGRA8)
        return {pixel[2], pixel[1], pixel[0]};
    return {pixel[0], pixel[1], pixel[2]};
}

void writeDxt1Block(const std::array<Rgb8, 16u>& source, std::uint8_t* destination)
{
    Rgb8 minimum{255u, 255u, 255u};
    Rgb8 maximum{0u, 0u, 0u};
    for (const Rgb8 color : source)
    {
        minimum.red = color.red < minimum.red ? color.red : minimum.red;
        minimum.green = color.green < minimum.green ? color.green : minimum.green;
        minimum.blue = color.blue < minimum.blue ? color.blue : minimum.blue;
        maximum.red = color.red > maximum.red ? color.red : maximum.red;
        maximum.green = color.green > maximum.green ? color.green : maximum.green;
        maximum.blue = color.blue > maximum.blue ? color.blue : maximum.blue;
    }

    std::uint16_t endpoint0 = packRgb565(maximum);
    std::uint16_t endpoint1 = packRgb565(minimum);
    // DXT1 uses endpoint0 > endpoint1 to select its opaque four-colour mode.
    // Force that ordering even for a flat block; alpha is never emitted here.
    if (endpoint0 <= endpoint1)
    {
        if (endpoint0 == 0u)
            endpoint0 = 1u;
        else
            endpoint1 = static_cast<std::uint16_t>(endpoint0 - 1u);
    }

    const std::array<Rgb8, 4u> palette = {
        unpackRgb565(endpoint0),
        unpackRgb565(endpoint1),
        interpolate(unpackRgb565(endpoint0), unpackRgb565(endpoint1), 2u, 1u),
        interpolate(unpackRgb565(endpoint0), unpackRgb565(endpoint1), 1u, 2u),
    };
    std::uint32_t indices = 0u;
    for (std::uint32_t index = 0u; index < source.size(); ++index)
    {
        std::uint32_t bestIndex = 0u;
        std::uint32_t bestDistance = squaredDistance(source[index], palette[0]);
        for (std::uint32_t candidate = 1u; candidate < palette.size(); ++candidate)
        {
            const std::uint32_t distance = squaredDistance(source[index], palette[candidate]);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                bestIndex = candidate;
            }
        }
        indices |= bestIndex << (index * 2u);
    }

    destination[0] = static_cast<std::uint8_t>(endpoint0 & 0xffu);
    destination[1] = static_cast<std::uint8_t>(endpoint0 >> 8u);
    destination[2] = static_cast<std::uint8_t>(endpoint1 & 0xffu);
    destination[3] = static_cast<std::uint8_t>(endpoint1 >> 8u);
    destination[4] = static_cast<std::uint8_t>(indices & 0xffu);
    destination[5] = static_cast<std::uint8_t>((indices >> 8u) & 0xffu);
    destination[6] = static_cast<std::uint8_t>((indices >> 16u) & 0xffu);
    destination[7] = static_cast<std::uint8_t>((indices >> 24u) & 0xffu);
}
} // namespace

bool BuildOpaqueDxt1SaveThumbnail(
    const std::uint8_t* pixels,
    const std::size_t rowBytes,
    const SourcePixelFormat format,
    std::vector<std::uint8_t>& dds,
    std::string& error)
{
    dds.clear();
    error.clear();
    constexpr std::size_t sourceRowBytes = static_cast<std::size_t>(SaveThumbnailWidth) * 4u;
    if (!pixels || rowBytes < sourceRowBytes)
    {
        error = "save-thumbnail source must provide 128 RGBA/BGRA pixels per row";
        return false;
    }
    const std::size_t blocksWide = SaveThumbnailWidth / 4u;
    const std::size_t blocksHigh = SaveThumbnailHeight / 4u;
    const std::size_t payloadBytes = blocksWide * blocksHigh * Dxt1BlockBytes;
    if (payloadBytes > std::numeric_limits<std::uint32_t>::max() ||
        payloadBytes > std::numeric_limits<std::size_t>::max() - sizeof(DdsMagic) - sizeof(DdsHeader))
    {
        error = "save-thumbnail DDS payload overflows host size";
        return false;
    }

    dds.resize(sizeof(DdsMagic) + sizeof(DdsHeader) + payloadBytes);
    DdsHeader header{};
    header.size = sizeof(DdsHeader);
    header.flags = DdsHeaderFlagsTexture | DdsHeaderFlagsLinearSize;
    header.height = SaveThumbnailHeight;
    header.width = SaveThumbnailWidth;
    header.pitchOrLinearSize = static_cast<std::uint32_t>(payloadBytes);
    header.pixelFormat = {sizeof(DdsPixelFormat), DdsPixelFormatFourCC, Dxt1FourCC, 0u, 0u, 0u, 0u, 0u};
    header.caps = DdsCapsTexture;
    std::memcpy(dds.data(), &DdsMagic, sizeof(DdsMagic));
    std::memcpy(dds.data() + sizeof(DdsMagic), &header, sizeof(header));

    std::uint8_t* destination = dds.data() + sizeof(DdsMagic) + sizeof(DdsHeader);
    for (std::uint32_t blockY = 0u; blockY < blocksHigh; ++blockY)
    {
        for (std::uint32_t blockX = 0u; blockX < blocksWide; ++blockX)
        {
            std::array<Rgb8, 16u> block{};
            for (std::uint32_t y = 0u; y < 4u; ++y)
                for (std::uint32_t x = 0u; x < 4u; ++x)
                    block[y * 4u + x] = readPixel(pixels, rowBytes, blockX * 4u + x,
                        blockY * 4u + y, format);
            writeDxt1Block(block, destination);
            destination += Dxt1BlockBytes;
        }
    }
    return true;
}
} // namespace xray::metal::screenshot
