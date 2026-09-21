#include "../MetalScreenshotFallback.h"

#include <cstdint>
#include <cstring>
#include <iostream>

namespace
{
std::uint32_t readU32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}
}

int main()
{
    const auto saveThumbnail = xray::metal::screenshot::BuildDxt1Placeholder(128, 128);
    const std::size_t savePayloadSize = 32u * 32u * 8u;
    if (saveThumbnail.size() != 4u + 124u + savePayloadSize ||
        readU32(saveThumbnail, 0) != xray::metal::screenshot::ddsMagic ||
        readU32(saveThumbnail, 4) != 124u ||
        readU32(saveThumbnail, 12) != 128u ||
        readU32(saveThumbnail, 16) != 128u ||
        readU32(saveThumbnail, 20) != savePayloadSize ||
        readU32(saveThumbnail, sizeof(std::uint32_t) +
            offsetof(xray::metal::screenshot::DdsHeader, pixelFormat) +
            offsetof(xray::metal::screenshot::DdsPixelFormat, fourCC)) != xray::metal::screenshot::dxt1FourCC)
    {
        std::cerr << "METAL_SCREENSHOT_FALLBACK_SELF_TEST result=FAIL artifact=dds\n";
        return 1;
    }

    const auto levelMap = xray::metal::screenshot::BuildTgaPlaceholder(128, 64);
    if (levelMap.size() != 18u + 128u * 64u * 3u || levelMap[2] != 2u ||
        levelMap[12] != 128u || levelMap[14] != 64u || levelMap[16] != 24u || levelMap[17] != 0x20u)
    {
        std::cerr << "METAL_SCREENSHOT_FALLBACK_SELF_TEST result=FAIL artifact=tga\n";
        return 1;
    }

    std::cout << "METAL_SCREENSHOT_FALLBACK_SELF_TEST result=PASS save_thumbnail=128x128 format=DXT1 levelmap=128x64 format=TGA\n";
    return 0;
}
