#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xray::metal::screenshot
{
// Savegame thumbnails are deliberately fixed at the legacy X-Ray size.  The
// codec does not resample: its caller must provide exactly this extent.
inline constexpr std::uint32_t SaveThumbnailWidth = 128u;
inline constexpr std::uint32_t SaveThumbnailHeight = 128u;

enum class SourcePixelFormat
{
    // Byte order is B, G, R, A. This is the CPU layout of a readback from a
    // MTLPixelFormatBGRA8Unorm final-color target.
    BGRA8,
    // Byte order is R, G, B, A. This is useful for CPU-only callers/tests.
    RGBA8,
};

// Builds one legacy "DDS " + DXT1/BC1 image from a top-left-origin 128x128
// source. `pixels` has `rowBytes` bytes between row starts and each source
// pixel consumes exactly four bytes in `format` order. Alpha is deliberately
// discarded: the output is always opaque four-colour BC1, matching the legacy
// gamesave-thumbnail contract. On failure `dds` is cleared and `error` is set.
bool BuildOpaqueDxt1SaveThumbnail(
    const std::uint8_t* pixels,
    std::size_t rowBytes,
    SourcePixelFormat format,
    std::vector<std::uint8_t>& dds,
    std::string& error);
} // namespace xray::metal::screenshot
