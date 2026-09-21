#include "../MetalScreenshotCodec.h"
#include "../R4DdsTexture.h"

#include <array>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace
{
constexpr std::uint32_t Dxt1FourCC = 0x31545844u;

std::uint32_t readU32(const std::vector<std::uint8_t>& bytes, const std::size_t offset)
{
    std::uint32_t value = 0u;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

void setBgraPixel(std::vector<std::uint8_t>& image, const std::uint32_t x, const std::uint32_t y,
    const std::array<std::uint8_t, 3u>& rgb)
{
    const std::size_t offset = (static_cast<std::size_t>(y) *
        xray::metal::screenshot::SaveThumbnailWidth + x) * 4u;
    image[offset] = rgb[2];
    image[offset + 1u] = rgb[1];
    image[offset + 2u] = rgb[0];
    image[offset + 3u] = 7u; // Must be ignored by the opaque encoder.
}

bool expect(const bool condition, const char* message)
{
    if (!condition)
        std::cerr << "METAL_SCREENSHOT_CODEC_SELF_TEST detail=" << message << '\n';
    return condition;
}

std::array<std::uint8_t, 3u> decodeDxt1BlockFirstPixel(const std::uint8_t* block)
{
    const std::uint16_t endpoint0 = static_cast<std::uint16_t>(block[0]) |
        static_cast<std::uint16_t>(block[1]) << 8u;
    const std::uint16_t endpoint1 = static_cast<std::uint16_t>(block[2]) |
        static_cast<std::uint16_t>(block[3]) << 8u;
    const auto unpack = [](const std::uint16_t color) {
        const std::uint8_t red = static_cast<std::uint8_t>((color >> 11u) & 0x1fu);
        const std::uint8_t green = static_cast<std::uint8_t>((color >> 5u) & 0x3fu);
        const std::uint8_t blue = static_cast<std::uint8_t>(color & 0x1fu);
        return std::array<std::uint8_t, 3u>{
            static_cast<std::uint8_t>((red << 3u) | (red >> 2u)),
            static_cast<std::uint8_t>((green << 2u) | (green >> 4u)),
            static_cast<std::uint8_t>((blue << 3u) | (blue >> 2u)),
        };
    };
    const auto color0 = unpack(endpoint0);
    const auto color1 = unpack(endpoint1);
    const std::array<std::array<std::uint8_t, 3u>, 4u> palette = {
        color0,
        color1,
        std::array<std::uint8_t, 3u>{
            static_cast<std::uint8_t>((2u * color0[0] + color1[0]) / 3u),
            static_cast<std::uint8_t>((2u * color0[1] + color1[1]) / 3u),
            static_cast<std::uint8_t>((2u * color0[2] + color1[2]) / 3u),
        },
        std::array<std::uint8_t, 3u>{
            static_cast<std::uint8_t>((color0[0] + 2u * color1[0]) / 3u),
            static_cast<std::uint8_t>((color0[1] + 2u * color1[1]) / 3u),
            static_cast<std::uint8_t>((color0[2] + 2u * color1[2]) / 3u),
        },
    };
    return palette[block[4] & 0x3u];
}

bool approximately(const std::array<std::uint8_t, 3u>& color, const std::uint8_t red,
    const std::uint8_t green, const std::uint8_t blue)
{
    constexpr int tolerance = 8;
    return std::abs(static_cast<int>(color[0]) - red) <= tolerance &&
        std::abs(static_cast<int>(color[1]) - green) <= tolerance &&
        std::abs(static_cast<int>(color[2]) - blue) <= tolerance;
}
} // namespace

int main()
{
    using namespace xray::metal;
    constexpr std::uint32_t width = screenshot::SaveThumbnailWidth;
    constexpr std::uint32_t height = screenshot::SaveThumbnailHeight;
    std::vector<std::uint8_t> bgra(static_cast<std::size_t>(width) * height * 4u, 0u);
    for (std::uint32_t y = 0u; y < height; ++y)
    {
        for (std::uint32_t x = 0u; x < width; ++x)
        {
            const bool right = x >= width / 2u;
            const bool bottom = y >= height / 2u;
            const std::array<std::uint8_t, 3u> color = !right && !bottom ?
                std::array<std::uint8_t, 3u>{255u, 0u, 0u} :
                right && !bottom ? std::array<std::uint8_t, 3u>{0u, 255u, 0u} :
                !right && bottom ? std::array<std::uint8_t, 3u>{0u, 0u, 255u} :
                std::array<std::uint8_t, 3u>{255u, 255u, 255u};
            setBgraPixel(bgra, x, y, color);
        }
    }

    std::vector<std::uint8_t> dds;
    std::string error;
    if (!screenshot::BuildOpaqueDxt1SaveThumbnail(bgra.data(), width * 4u,
            screenshot::SourcePixelFormat::BGRA8, dds, error))
    {
        std::cerr << "METAL_SCREENSHOT_CODEC_SELF_TEST result=FAIL encode=" << error << '\n';
        return 1;
    }
    const std::size_t payloadBytes = 32u * 32u * 8u;
    if (!expect(dds.size() == 4u + 124u + payloadBytes, "unexpected DDS size") ||
        !expect(readU32(dds, 0u) == 0x20534444u, "missing DDS magic") ||
        !expect(readU32(dds, 12u) == height && readU32(dds, 16u) == width,
            "wrong DDS extent") ||
        !expect(readU32(dds, 20u) == payloadBytes, "wrong linear size") ||
        !expect(readU32(dds, 84u) == Dxt1FourCC, "wrong DXT1 fourCC"))
        return 1;

    R4DdsTexture parsed;
    if (!ParseR4DdsTexture(dds, parsed, error) || parsed.descriptor.width != width ||
        parsed.descriptor.height != height || parsed.descriptor.format != R4PixelFormat::BC1Rgba ||
        parsed.mipLevels.size() != 1u || parsed.mipLevels[0].bytesPerImage != payloadBytes)
    {
        std::cerr << "METAL_SCREENSHOT_CODEC_SELF_TEST result=FAIL parse=" << error << '\n';
        return 1;
    }

    std::vector<std::uint8_t> repeated;
    if (!screenshot::BuildOpaqueDxt1SaveThumbnail(bgra.data(), width * 4u,
            screenshot::SourcePixelFormat::BGRA8, repeated, error) ||
        !expect(repeated == dds, "encoder is not deterministic"))
        return 1;
    const std::uint8_t* payload = dds.data() + 128u;
    if (!expect(std::memcmp(payload, payload + 8u, payloadBytes - 8u) != 0,
            "quadrants collapsed to one BC1 block"))
        return 1;
    const std::size_t blocksWide = width / 4u;
    if (!expect(approximately(decodeDxt1BlockFirstPixel(payload), 255u, 0u, 0u),
            "top-left is not red") ||
        !expect(approximately(decodeDxt1BlockFirstPixel(payload + 16u * 8u), 0u, 255u, 0u),
            "top-right is not green") ||
        !expect(approximately(decodeDxt1BlockFirstPixel(payload + 16u * blocksWide * 8u), 0u, 0u, 255u),
            "bottom-left is not blue") ||
        !expect(approximately(decodeDxt1BlockFirstPixel(payload +
                (16u * blocksWide + 16u) * 8u), 255u, 255u, 255u),
            "bottom-right is not white"))
        return 1;

    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4u, 0u);
    for (std::uint32_t pixel = 0u; pixel < width * height; ++pixel)
    {
        rgba[pixel * 4u] = bgra[pixel * 4u + 2u];
        rgba[pixel * 4u + 1u] = bgra[pixel * 4u + 1u];
        rgba[pixel * 4u + 2u] = bgra[pixel * 4u];
        rgba[pixel * 4u + 3u] = 255u - bgra[pixel * 4u + 3u];
    }
    if (!screenshot::BuildOpaqueDxt1SaveThumbnail(rgba.data(), width * 4u,
            screenshot::SourcePixelFormat::RGBA8, repeated, error) ||
        !expect(repeated == dds, "RGBA8 and BGRA8 input disagree"))
        return 1;
    if (screenshot::BuildOpaqueDxt1SaveThumbnail(nullptr, width * 4u,
            screenshot::SourcePixelFormat::BGRA8, repeated, error) ||
        !expect(repeated.empty(), "invalid input retained output"))
        return 1;

    std::cout << "METAL_SCREENSHOT_CODEC_SELF_TEST result=PASS input=BGRA8 output=DXT1 "
              << "thumbnail=128x128 quadrants=non_uniform parser=accepted\n";
    return 0;
}
