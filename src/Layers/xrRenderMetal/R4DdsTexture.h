#pragma once

#include "IR4Resources.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace xray::metal
{
// The offsets address bytes in R4DdsTexture::bytes.  DDS subresources are
// emitted in file order: for a 2D texture this is mip 0..N; for a cube it is
// +X/-X/+Y/-Y/+Z/-Z, each face with mip 0..N. bytesPerImage is tightly packed,
// not DDS' advisory dwPitchOrLinearSize field.
struct R4DdsMipLevel
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mipLevel = 0;
    std::uint32_t arraySlice = 0;
    std::size_t dataOffset = 0;
    std::size_t bytesPerRow = 0;
    std::size_t bytesPerImage = 0;
};

struct R4DdsTexture
{
	R4Texture2DDesc descriptor;
	std::vector<R4DdsMipLevel> mipLevels;
	std::vector<std::uint8_t> bytes;
};

// Parser ownership retains a second copy until upload, so keep one virtual
// asset below 128 MiB and fail before an archive reader performs that copy.
inline constexpr std::size_t R4DdsProviderMaximumBytes = 128u * 1024u * 1024u;

// The provider owns only the read; parsing and texture upload stay in the
// shared backend path. It receives the hard byte ceiling before allocation.
using R4DdsBytesProvider = std::function<bool(
    const std::filesystem::path& path,
    std::size_t maximumBytes,
    std::vector<std::uint8_t>& bytes,
    std::string& error)>;

// Parses one tightly packed 2D texture or one complete six-face cube DDS. The
// parser deliberately accepts only formats represented by R4PixelFormat and
// requires the payload to end exactly after the final subresource. Array and
// volume textures remain rejected.
bool ParseR4DdsTexture(
	const std::vector<std::uint8_t>& bytes,
	R4DdsTexture& result,
	std::string& error);

bool LoadR4DdsTexture(
	const std::filesystem::path& path,
	R4DdsTexture& result,
    std::string& error);

bool LoadR4DdsTexture(
    const std::filesystem::path& path,
    const R4DdsBytesProvider& provider,
    R4DdsTexture& result,
    std::string& error);

bool RunR4DdsTextureSelfTest(std::string& error);
} // namespace xray::metal
