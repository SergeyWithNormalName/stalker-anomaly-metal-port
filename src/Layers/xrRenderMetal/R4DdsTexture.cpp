#include "R4DdsTexture.h"

#include <fstream>
#include <limits>
#include <utility>

namespace xray::metal
{
namespace
{
constexpr std::size_t kMagicSize = 4u;
constexpr std::size_t kHeaderSize = 124u;
constexpr std::size_t kBaseDataOffset = kMagicSize + kHeaderSize;
constexpr std::size_t kDx10HeaderSize = 20u;
constexpr std::uint32_t kDdsMagic = 0x20534444u; // "DDS " in little endian.
constexpr std::uint32_t kDdsHeaderSize = 124u;
constexpr std::uint32_t kPixelFormatSize = 32u;
constexpr std::uint32_t kDdsPfAlphaPixels = 0x1u;
constexpr std::uint32_t kDdsPfAlpha = 0x2u;
constexpr std::uint32_t kDdsPfFourCc = 0x4u;
constexpr std::uint32_t kDdsPfRgb = 0x40u;
constexpr std::uint32_t kDdsPfLuminance = 0x20000u;
constexpr std::uint32_t kDdsCaps2CubeMap = 0x200u;
constexpr std::uint32_t kDdsCaps2CubeMapPositiveX = 0x400u;
constexpr std::uint32_t kDdsCaps2CubeMapNegativeX = 0x800u;
constexpr std::uint32_t kDdsCaps2CubeMapPositiveY = 0x1000u;
constexpr std::uint32_t kDdsCaps2CubeMapNegativeY = 0x2000u;
constexpr std::uint32_t kDdsCaps2CubeMapPositiveZ = 0x4000u;
constexpr std::uint32_t kDdsCaps2CubeMapNegativeZ = 0x8000u;
constexpr std::uint32_t kDdsCaps2CubeMapFaces =
    kDdsCaps2CubeMapPositiveX | kDdsCaps2CubeMapNegativeX |
    kDdsCaps2CubeMapPositiveY | kDdsCaps2CubeMapNegativeY |
    kDdsCaps2CubeMapPositiveZ | kDdsCaps2CubeMapNegativeZ;
constexpr std::uint32_t kDdsCaps2CubeMapComplete = kDdsCaps2CubeMap | kDdsCaps2CubeMapFaces;
constexpr std::uint32_t kDdsCaps2Volume = 0x200000u;
constexpr std::uint32_t kDdsResourceDimensionTexture2D = 3u;
constexpr std::uint32_t kDdsResourceMiscTextureCube = 0x4u;

constexpr std::uint32_t fourCc(const char a, const char b, const char c, const char d)
{
	return static_cast<std::uint32_t>(static_cast<unsigned char>(a))
		| (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8u)
		| (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16u)
		| (static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24u);
}

bool checkedAdd(const std::size_t left, const std::size_t right, std::size_t& result)
{
	if (right > std::numeric_limits<std::size_t>::max() - left)
		return false;
	result = left + right;
	return true;
}

bool checkedMultiply(const std::size_t left, const std::size_t right, std::size_t& result)
{
	if (left != 0u && right > std::numeric_limits<std::size_t>::max() / left)
		return false;
	result = left * right;
	return true;
}

bool readU32(const std::vector<std::uint8_t>& bytes, const std::size_t offset, std::uint32_t& value)
{
	std::size_t end = 0;
	if (!checkedAdd(offset, 4u, end) || end > bytes.size())
		return false;
	value = static_cast<std::uint32_t>(bytes[offset])
		| (static_cast<std::uint32_t>(bytes[offset + 1u]) << 8u)
		| (static_cast<std::uint32_t>(bytes[offset + 2u]) << 16u)
		| (static_cast<std::uint32_t>(bytes[offset + 3u]) << 24u);
	return true;
}

bool readHeaderU32(const std::vector<std::uint8_t>& bytes, const std::size_t fieldOffset, std::uint32_t& value)
{
	return readU32(bytes, kMagicSize + fieldOffset, value);
}

bool legacyFormat(const std::uint32_t flags, const std::uint32_t fourCC, const std::uint32_t bits,
	const std::uint32_t redMask, const std::uint32_t greenMask, const std::uint32_t blueMask,
	const std::uint32_t alphaMask, R4PixelFormat& format, std::size_t& bytesPerBlock,
	bool& compressed, bool& expandBgr24, std::string& error)
{
	compressed = false;
	expandBgr24 = false;
	if ((flags & kDdsPfFourCc) != 0u)
	{
		compressed = true;
		switch (fourCC)
		{
		case fourCc('D', 'X', 'T', '1'): format = R4PixelFormat::BC1Rgba; bytesPerBlock = 8u; return true;
		case fourCc('D', 'X', 'T', '3'): format = R4PixelFormat::BC2Rgba; bytesPerBlock = 16u; return true;
		case fourCc('D', 'X', 'T', '5'): format = R4PixelFormat::BC3Rgba; bytesPerBlock = 16u; return true;
		case fourCc('A', 'T', 'I', '1'): format = R4PixelFormat::BC4RUnorm; bytesPerBlock = 8u; return true;
		case fourCc('B', 'C', '4', 'U'): format = R4PixelFormat::BC4RUnorm; bytesPerBlock = 8u; return true;
		case fourCc('A', 'T', 'I', '2'): format = R4PixelFormat::BC5RgUnorm; bytesPerBlock = 16u; return true;
		case fourCc('B', 'C', '5', 'U'): format = R4PixelFormat::BC5RgUnorm; bytesPerBlock = 16u; return true;
		// D3DFMT_A8 (28) and D3DFMT_L8 (50) are 8-bit uncompressed formats mapped to A8Unorm.
		case 28u: compressed = false; format = R4PixelFormat::A8Unorm; bytesPerBlock = 1u; return true;
		case 50u: compressed = false; format = R4PixelFormat::A8Unorm; bytesPerBlock = 1u; return true;
		// D3DFMT_A8R8G8B8 (21) and D3DFMT_X8R8G8B8 (22) are 32-bit BGRA/BGRX formats.
		case 21u: compressed = false; format = R4PixelFormat::BGRA8Unorm; bytesPerBlock = 4u; return true;
		case 22u: compressed = false; format = R4PixelFormat::BGRA8Unorm; bytesPerBlock = 4u; return true;
		// D3DFMT_A16B16G16R16F is stored as its numeric D3DFORMAT value in
		// legacy DDS FOURCC. X-Ray weather environment cubes use this HDR
		// layout; little-endian component order is directly Metal RGBA16Float.
		case 113u: compressed = false; format = R4PixelFormat::RGBA16Float; bytesPerBlock = 8u; return true;
		default:
			error = "unsupported legacy DDS FOURCC=" + std::to_string(fourCC);
			return false;
		}
	}

	if (((flags & kDdsPfAlpha) != 0u || (flags & kDdsPfAlphaPixels) != 0u) &&
		bits == 8u && (alphaMask == 0xffu || alphaMask == 0u))
	{
		format = R4PixelFormat::A8Unorm;
		bytesPerBlock = 1u;
		return true;
	}
	if ((flags & kDdsPfLuminance) != 0u && bits == 8u && (redMask == 0xffu || redMask == 0u))
	{
		format = R4PixelFormat::A8Unorm;
		bytesPerBlock = 1u;
		return true;
	}
	if ((flags & kDdsPfRgb) != 0u && bits == 24u && redMask == 0x00ff0000u &&
		greenMask == 0x0000ff00u && blueMask == 0x000000ffu && alphaMask == 0u)
	{
		// Metal has no three-channel 8-bit texture format. Preserve the legacy
		// DDS' BGR color meaning by expanding it once to tightly packed RGBA8.
		format = R4PixelFormat::RGBA8Unorm;
		bytesPerBlock = 3u;
		expandBgr24 = true;
		return true;
	}
	if ((flags & kDdsPfRgb) == 0u || bits != 32u)
	{
		error = "unsupported legacy DDS pixel format flags=" + std::to_string(flags) +
			" bits=" + std::to_string(bits) + " rmask=" + std::to_string(redMask) +
			" gmask=" + std::to_string(greenMask) + " bmask=" + std::to_string(blueMask) +
			" amask=" + std::to_string(alphaMask);
		return false;
	}
	bytesPerBlock = 4u;
	if (redMask == 0x000000ffu && greenMask == 0x0000ff00u && blueMask == 0x00ff0000u &&
		(alphaMask == 0xff000000u || alphaMask == 0u))
	{
		format = R4PixelFormat::RGBA8Unorm;
		return true;
	}
	if (redMask == 0x00ff0000u && greenMask == 0x0000ff00u && blueMask == 0x000000ffu &&
		(alphaMask == 0xff000000u || alphaMask == 0u))
	{
		format = R4PixelFormat::BGRA8Unorm;
		return true;
	}
	error = "unsupported legacy DDS RGBA8 channel masks";
	return false;
}

bool dx10Format(const std::uint32_t dxgiFormat, R4PixelFormat& format, std::size_t& bytesPerBlock, std::string& error)
{
	switch (dxgiFormat)
	{
	case 71u: format = R4PixelFormat::BC1Rgba; bytesPerBlock = 8u; return true;
	case 72u: format = R4PixelFormat::BC1RgbaSrgb; bytesPerBlock = 8u; return true;
	case 74u: format = R4PixelFormat::BC2Rgba; bytesPerBlock = 16u; return true;
	case 75u: format = R4PixelFormat::BC2RgbaSrgb; bytesPerBlock = 16u; return true;
	case 77u: format = R4PixelFormat::BC3Rgba; bytesPerBlock = 16u; return true;
	case 78u: format = R4PixelFormat::BC3RgbaSrgb; bytesPerBlock = 16u; return true;
	case 80u: format = R4PixelFormat::BC4RUnorm; bytesPerBlock = 8u; return true;
	case 83u: format = R4PixelFormat::BC5RgUnorm; bytesPerBlock = 16u; return true;
	case 98u: format = R4PixelFormat::BC7RgbaUnorm; bytesPerBlock = 16u; return true;
	case 99u: format = R4PixelFormat::BC7RgbaUnormSrgb; bytesPerBlock = 16u; return true;
	default:
		error = "unsupported DX10 DDS DXGI format";
		return false;
	}
}

bool appendMipLevels(const std::uint32_t width, const std::uint32_t height, const std::uint32_t count,
    const bool compressed, const std::size_t bytesPerBlock, const std::size_t dataOffset,
    const std::uint32_t arraySlice, std::vector<R4DdsMipLevel>& mipLevels,
    std::size_t& expectedEnd, std::string& error)
{
    std::size_t cursor = dataOffset;
	for (std::uint32_t level = 0; level < count; ++level)
	{
		const std::uint32_t mipWidth = level >= 32u ? 1u : (width >> level == 0u ? 1u : width >> level);
		const std::uint32_t mipHeight = level >= 32u ? 1u : (height >> level == 0u ? 1u : height >> level);
		std::size_t unitsWide = mipWidth;
		std::size_t unitsHigh = mipHeight;
		if (compressed)
		{
			unitsWide = (unitsWide + 3u) / 4u;
			unitsHigh = (unitsHigh + 3u) / 4u;
		}
		std::size_t bytesPerRow = 0;
		std::size_t bytesPerImage = 0;
		std::size_t next = 0;
		if (!checkedMultiply(unitsWide, bytesPerBlock, bytesPerRow)
			|| !checkedMultiply(bytesPerRow, unitsHigh, bytesPerImage)
			|| !checkedAdd(cursor, bytesPerImage, next))
		{
			error = "DDS mip geometry overflows host size_t";
			return false;
		}
        mipLevels.push_back({mipWidth, mipHeight, level, arraySlice, cursor, bytesPerRow, bytesPerImage});
        cursor = next;
	}
	expectedEnd = cursor;
	return true;
}

void writeU32(std::vector<std::uint8_t>& bytes, const std::size_t offset, const std::uint32_t value)
{
	bytes[offset] = static_cast<std::uint8_t>(value & 0xffu);
	bytes[offset + 1u] = static_cast<std::uint8_t>((value >> 8u) & 0xffu);
	bytes[offset + 2u] = static_cast<std::uint8_t>((value >> 16u) & 0xffu);
	bytes[offset + 3u] = static_cast<std::uint8_t>((value >> 24u) & 0xffu);
}

std::vector<std::uint8_t> makeTestDds(const std::uint32_t width, const std::uint32_t height,
    const std::uint32_t mipLevels, const std::uint32_t fourCC, const bool dx10, const std::uint32_t dxgiFormat,
    const std::size_t payloadBytes, const std::uint32_t caps2 = 0u,
    const std::uint32_t miscFlag = 0u, const std::uint32_t arraySize = 1u)
{
	const std::size_t headerBytes = kBaseDataOffset + (dx10 ? kDx10HeaderSize : 0u);
	std::vector<std::uint8_t> bytes(headerBytes + payloadBytes, 0u);
	writeU32(bytes, 0u, kDdsMagic);
	writeU32(bytes, 4u, kDdsHeaderSize);
	writeU32(bytes, 4u + 8u, height);
	writeU32(bytes, 4u + 12u, width);
	writeU32(bytes, 4u + 24u, mipLevels);
	writeU32(bytes, 4u + 72u, kPixelFormatSize);
	writeU32(bytes, 4u + 76u, kDdsPfFourCc);
    writeU32(bytes, 4u + 80u, fourCC);
    writeU32(bytes, 4u + 108u, caps2);
    if (dx10)
    {
        writeU32(bytes, kBaseDataOffset, dxgiFormat);
        writeU32(bytes, kBaseDataOffset + 4u, kDdsResourceDimensionTexture2D);
        writeU32(bytes, kBaseDataOffset + 8u, miscFlag);
        writeU32(bytes, kBaseDataOffset + 12u, arraySize);
	}
	return bytes;
}
} // namespace

bool ParseR4DdsTexture(const std::vector<std::uint8_t>& bytes, R4DdsTexture& result, std::string& error)
{
	result = {};
	error.clear();
	if (bytes.size() < kBaseDataOffset)
	{
		error = "DDS file is shorter than its magic and 124-byte header";
		return false;
	}

	std::uint32_t magic = 0;
	std::uint32_t headerSize = 0;
	std::uint32_t height = 0;
	std::uint32_t width = 0;
	std::uint32_t mipCount = 0;
	std::uint32_t pixelFormatSize = 0;
	std::uint32_t pixelFormatFlags = 0;
	std::uint32_t pixelFourCC = 0;
	std::uint32_t rgbBits = 0;
	std::uint32_t redMask = 0;
	std::uint32_t greenMask = 0;
	std::uint32_t blueMask = 0;
	std::uint32_t alphaMask = 0;
	std::uint32_t caps2 = 0;
	if (!readU32(bytes, 0u, magic) || !readHeaderU32(bytes, 0u, headerSize)
		|| !readHeaderU32(bytes, 8u, height) || !readHeaderU32(bytes, 12u, width)
		|| !readHeaderU32(bytes, 24u, mipCount) || !readHeaderU32(bytes, 72u, pixelFormatSize)
		|| !readHeaderU32(bytes, 76u, pixelFormatFlags) || !readHeaderU32(bytes, 80u, pixelFourCC)
		|| !readHeaderU32(bytes, 84u, rgbBits) || !readHeaderU32(bytes, 88u, redMask)
		|| !readHeaderU32(bytes, 92u, greenMask) || !readHeaderU32(bytes, 96u, blueMask)
		|| !readHeaderU32(bytes, 100u, alphaMask) || !readHeaderU32(bytes, 108u, caps2))
	{
		error = "DDS header fields are truncated";
		return false;
	}
	if (magic != kDdsMagic || headerSize != kDdsHeaderSize || pixelFormatSize != kPixelFormatSize)
	{
		error = "DDS magic or fixed header size is invalid";
		return false;
	}
	if (width == 0u || height == 0u)
	{
		error = "DDS dimensions must be non-zero";
		return false;
	}
    if ((caps2 & kDdsCaps2Volume) != 0u)
    {
        error = "DDS volume textures are not supported";
        return false;
    }
    const bool legacyCube = (caps2 & kDdsCaps2CubeMap) != 0u;
    const std::uint32_t legacyCubeFaces = caps2 & kDdsCaps2CubeMapFaces;
    if ((legacyCube && caps2 != kDdsCaps2CubeMapComplete) ||
        (!legacyCube && legacyCubeFaces != 0u))
    {
        error = "DDS legacy cubemap must contain exactly six faces";
        return false;
    }
	if (mipCount == 0u)
		mipCount = 1u;
	std::uint32_t maximumMipCount = 1u;
	for (std::uint32_t largestDimension = width > height ? width : height; largestDimension > 1u; largestDimension >>= 1u)
		++maximumMipCount;
	if (mipCount > maximumMipCount)
	{
		error = "DDS mip count exceeds the complete 2D mip chain";
		return false;
	}

    R4PixelFormat format = R4PixelFormat::Invalid;
    std::size_t bytesPerBlock = 0;
    bool compressed = false;
    bool expandBgr24 = false;
    bool cube = legacyCube;
    std::size_t dataOffset = kBaseDataOffset;
	if ((pixelFormatFlags & kDdsPfFourCc) != 0u && pixelFourCC == fourCc('D', 'X', '1', '0'))
	{
		std::size_t dx10End = 0;
		if (!checkedAdd(dataOffset, kDx10HeaderSize, dx10End) || dx10End > bytes.size())
		{
			error = "DDS DX10 extension header is truncated";
			return false;
		}
		std::uint32_t dxgiFormat = 0;
		std::uint32_t resourceDimension = 0;
		std::uint32_t miscFlag = 0;
		std::uint32_t arraySize = 0;
		if (!readU32(bytes, dataOffset, dxgiFormat) || !readU32(bytes, dataOffset + 4u, resourceDimension)
			|| !readU32(bytes, dataOffset + 8u, miscFlag) || !readU32(bytes, dataOffset + 12u, arraySize))
		{
			error = "DDS DX10 extension fields are truncated";
			return false;
		}
        const bool dx10Cube = (miscFlag & kDdsResourceMiscTextureCube) != 0u;
        if (resourceDimension != kDdsResourceDimensionTexture2D || arraySize != 1u ||
            (miscFlag & ~kDdsResourceMiscTextureCube) != 0u ||
            (legacyCube && !dx10Cube) || (!legacyCube && legacyCubeFaces != 0u) ||
            (legacyCube && caps2 != kDdsCaps2CubeMapComplete))
        {
            error = "DDS DX10 texture is not one 2D image or one complete cubemap";
            return false;
        }
        cube = dx10Cube;
        if (!dx10Format(dxgiFormat, format, bytesPerBlock, error))
			return false;
		compressed = true;
		dataOffset = dx10End;
	}
	else if (!legacyFormat(pixelFormatFlags, pixelFourCC, rgbBits, redMask, greenMask, blueMask, alphaMask,
		format, bytesPerBlock, compressed, expandBgr24, error))
	{
		return false;
	}

    if (cube && width != height)
    {
        error = "DDS cubemap faces must be square";
        return false;
    }

    std::size_t expectedEnd = dataOffset;
    std::vector<R4DdsMipLevel> mipLevels;
    const std::uint32_t sliceCount = cube ? 6u : 1u;
    mipLevels.reserve(static_cast<std::size_t>(mipCount) * sliceCount);
    for (std::uint32_t arraySlice = 0; arraySlice < sliceCount; ++arraySlice)
    {
        if (!appendMipLevels(width, height, mipCount, compressed, bytesPerBlock, expectedEnd,
                arraySlice, mipLevels, expectedEnd, error))
            return false;
    }
	if (expectedEnd > bytes.size())
	{
		error = "DDS mip payload is truncated";
		return false;
	}
	// The engine's D3DX11 compatibility loader walks the declared subresources
	// with a `remaining >= sliceBytes` guard and intentionally does not require
	// `remaining == 0` afterwards. Archive-winning GAMMA assets therefore may
	// carry an opaque tail after a complete declared mip chain. Keep every mip
	// range exact, then discard that non-image tail before it can reach Metal.

	result.descriptor.width = width;
	result.descriptor.height = height;
	result.descriptor.mipLevels = mipCount;
    result.descriptor.arrayLength = 1u;
    result.descriptor.type = cube ? R4TextureType::TextureCube : R4TextureType::Texture2D;
	result.descriptor.format = format;
	result.descriptor.storage = R4ResourceStorage::Private;
	result.descriptor.usage = R4TextureUsage::ShaderRead;
	if (expandBgr24)
	{
		std::vector<std::uint8_t> expanded;
		std::vector<R4DdsMipLevel> expandedMipLevels;
		expandedMipLevels.reserve(mipLevels.size());
		for (const R4DdsMipLevel& source : mipLevels)
		{
			R4DdsMipLevel destination = source;
			destination.dataOffset = expanded.size();
			destination.bytesPerRow = static_cast<std::size_t>(source.width) * 4u;
			destination.bytesPerImage = destination.bytesPerRow * source.height;
			expanded.reserve(expanded.size() + destination.bytesPerImage);
			for (std::uint32_t y = 0u; y < source.height; ++y)
			{
				const std::uint8_t* row = bytes.data() + source.dataOffset +
					static_cast<std::size_t>(y) * source.bytesPerRow;
				for (std::uint32_t x = 0u; x < source.width; ++x)
				{
					const std::uint8_t* bgr = row + static_cast<std::size_t>(x) * 3u;
					expanded.push_back(bgr[2]);
					expanded.push_back(bgr[1]);
					expanded.push_back(bgr[0]);
					expanded.push_back(255u);
				}
			}
			expandedMipLevels.push_back(destination);
		}
		result.mipLevels = std::move(expandedMipLevels);
		result.bytes = std::move(expanded);
	}
	else
	{
		result.mipLevels = std::move(mipLevels);
		result.bytes.assign(bytes.begin(), bytes.begin() + expectedEnd);
	}
	return true;
}

bool LoadR4DdsTexture(const std::filesystem::path& path, R4DdsTexture& result, std::string& error)
{
	std::ifstream stream(path, std::ios::binary | std::ios::ate);
	if (!stream)
	{
		error = "cannot open DDS file: " + path.generic_string();
		return false;
	}
	const std::streamoff length = stream.tellg();
	if (length < 0 || static_cast<std::uintmax_t>(length) > std::numeric_limits<std::size_t>::max())
	{
		error = "DDS file length is not representable";
		return false;
	}
	const std::size_t byteCount = static_cast<std::size_t>(length);
	if (byteCount > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()))
	{
		error = "DDS file length is not readable by the stream API";
		return false;
	}
	std::vector<std::uint8_t> bytes(byteCount);
	stream.seekg(0);
	if (!bytes.empty() && !stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
	{
		error = "cannot read DDS file: " + path.generic_string();
		return false;
	}
	return ParseR4DdsTexture(bytes, result, error);
}

bool LoadR4DdsTexture(
	const std::filesystem::path& path,
	const R4DdsBytesProvider& provider,
	R4DdsTexture& result,
	std::string& error)
{
	result = {};
	if (!provider)
	{
		error = "DDS byte provider is empty";
		return false;
	}
	if (path.empty())
	{
		error = "DDS virtual path is empty";
		return false;
	}
	std::vector<std::uint8_t> bytes;
	if (!provider(path, R4DdsProviderMaximumBytes, bytes, error))
	{
		if (error.empty())
			error = "DDS byte provider rejected virtual path";
		return false;
	}
	if (bytes.size() > R4DdsProviderMaximumBytes)
	{
		error = "DDS byte provider exceeded its maximum byte limit";
		return false;
	}
	return ParseR4DdsTexture(bytes, result, error);
}

bool RunR4DdsTextureSelfTest(std::string& error)
{
	auto expectSuccess = [&error](const std::vector<std::uint8_t>& bytes, const R4PixelFormat format,
		const std::uint32_t width, const std::uint32_t height, const std::uint32_t mips) {
		R4DdsTexture texture;
		if (!ParseR4DdsTexture(bytes, texture, error))
			return false;
		return texture.descriptor.format == format && texture.descriptor.width == width
			&& texture.descriptor.height == height && texture.descriptor.mipLevels == mips
			&& texture.descriptor.type == R4TextureType::Texture2D
			&& texture.descriptor.arrayLength == 1u && texture.mipLevels.size() == mips
			&& texture.bytes == bytes;
	};
	auto expectFailure = [&error](const std::vector<std::uint8_t>& bytes) {
		R4DdsTexture texture;
		std::string localError;
		if (ParseR4DdsTexture(bytes, texture, localError))
		{
			error = "malformed DDS unexpectedly parsed";
			return false;
		}
		return true;
	};
	const std::vector<std::uint8_t> providerBytes =
		makeTestDds(4u, 4u, 1u, fourCc('D', 'X', 'T', '1'), false, 0u, 8u);
	bool providerObservedLimit = false;
	const R4DdsBytesProvider provider = [&providerBytes, &providerObservedLimit](
		const std::filesystem::path& path, const std::size_t maximumBytes,
		std::vector<std::uint8_t>& output, std::string& providerError) {
		providerObservedLimit = maximumBytes == R4DdsProviderMaximumBytes;
		if (path.generic_string() != "xrfs:mock.dds")
		{
			providerError = "mock virtual DDS path is unavailable";
			return false;
		}
		output = providerBytes;
		return true;
	};
	R4DdsTexture providerTexture;
	if (!LoadR4DdsTexture("xrfs:mock.dds", provider, providerTexture, error) ||
		!providerObservedLimit || providerTexture.bytes != providerBytes)
	{
		error = "DDS byte-provider self-test did not parse its exact mock payload";
		return false;
	}
	if (LoadR4DdsTexture("xrfs:missing.dds", provider, providerTexture, error))
	{
		error = "DDS byte-provider missing-path self-test was accepted";
		return false;
	}
	if (LoadR4DdsTexture(std::filesystem::path{}, provider, providerTexture, error))
	{
		error = "DDS byte-provider empty-path self-test was accepted";
		return false;
	}

	// DXT5, 8x4 with a complete 8x4 -> 4x2 -> 2x1 chain: 32 + 16 + 16 bytes.
	const auto dxt5 = makeTestDds(8u, 4u, 3u, fourCc('D', 'X', 'T', '5'), false, 0u, 64u);
	if (!expectSuccess(dxt5, R4PixelFormat::BC3Rgba, 8u, 4u, 3u))
	{
		if (error.empty()) error = "legacy DXT5 descriptor mismatch";
		return false;
	}
	// The D3DX11 compatibility path accepts bytes after the complete declared
	// chain. Parse the same valid mip ranges, but retain only image bytes so
	// Metal uploads can never consume that opaque tail.
	std::vector<std::uint8_t> dxt5WithTail = dxt5;
	dxt5WithTail.insert(dxt5WithTail.end(), {0xdeu, 0xadu, 0xbeu, 0xefu});
	R4DdsTexture trailingTexture;
	if (!ParseR4DdsTexture(dxt5WithTail, trailingTexture, error) ||
		trailingTexture.bytes != dxt5 || trailingTexture.mipLevels.size() != 3u ||
		trailingTexture.mipLevels.back().dataOffset + trailingTexture.mipLevels.back().bytesPerImage != dxt5.size())
	{
		if (error.empty()) error = "DDS trailing payload was not isolated from declared mips";
		return false;
	}
	// DX10 BC7 is always a 16-byte 4x4 block.
	const auto bc7 = makeTestDds(4u, 4u, 1u, fourCc('D', 'X', '1', '0'), true, 98u, 16u);
	if (!expectSuccess(bc7, R4PixelFormat::BC7RgbaUnorm, 4u, 4u, 1u))
	{
		if (error.empty()) error = "DX10 BC7 descriptor mismatch";
		return false;
	}
	// Legacy X-Ray HDR environment maps store D3DFMT_A16B16G16R16F as the
	// numeric FOURCC 113. It is an uncompressed 8-byte RGBA texel.
	const auto rgba16f = makeTestDds(2u, 2u, 1u, 113u, false, 0u, 32u);
	if (!expectSuccess(rgba16f, R4PixelFormat::RGBA16Float, 2u, 2u, 1u))
	{
		if (error.empty()) error = "legacy A16B16G16R16F descriptor mismatch";
		return false;
	}
	// Legacy 24-bit BGR has no native Metal pixel format; it expands exactly
	// once to RGBA8 while retaining subresource geometry.
	auto bgr24 = makeTestDds(2u, 1u, 1u, 0u, false, 0u, 6u);
	writeU32(bgr24, 4u + 76u, kDdsPfRgb);
	writeU32(bgr24, 4u + 80u, 0u);
	writeU32(bgr24, 4u + 84u, 24u);
	writeU32(bgr24, 4u + 88u, 0x00ff0000u);
	writeU32(bgr24, 4u + 92u, 0x0000ff00u);
	writeU32(bgr24, 4u + 96u, 0x000000ffu);
	writeU32(bgr24, 4u + 100u, 0u);
	bgr24[kBaseDataOffset + 0u] = 1u; bgr24[kBaseDataOffset + 1u] = 2u;
	bgr24[kBaseDataOffset + 2u] = 3u; bgr24[kBaseDataOffset + 3u] = 4u;
	bgr24[kBaseDataOffset + 4u] = 5u; bgr24[kBaseDataOffset + 5u] = 6u;
	R4DdsTexture bgr24Texture;
	if (!ParseR4DdsTexture(bgr24, bgr24Texture, error) ||
		bgr24Texture.descriptor.format != R4PixelFormat::RGBA8Unorm ||
		bgr24Texture.mipLevels.size() != 1u || bgr24Texture.mipLevels[0].dataOffset != 0u ||
		bgr24Texture.mipLevels[0].bytesPerRow != 8u || bgr24Texture.bytes.size() != 8u ||
		bgr24Texture.bytes != std::vector<std::uint8_t>({3u, 2u, 1u, 255u, 6u, 5u, 4u, 255u}))
	{
		if (error.empty()) error = "legacy BGR24 expansion mismatch";
		return false;
	}
	// Legacy 8-bit uncompressed Alpha font texture (DDPF_ALPHA, bits = 8).
	auto alpha8 = makeTestDds(4u, 4u, 1u, 0u, false, 0u, 16u);
	writeU32(alpha8, 4u + 76u, kDdsPfAlpha);
	writeU32(alpha8, 4u + 80u, 0u);
	writeU32(alpha8, 4u + 84u, 8u);
	writeU32(alpha8, 4u + 88u, 0u);
	writeU32(alpha8, 4u + 92u, 0u);
	writeU32(alpha8, 4u + 96u, 0u);
	writeU32(alpha8, 4u + 100u, 0xffu);
	if (!expectSuccess(alpha8, R4PixelFormat::A8Unorm, 4u, 4u, 1u))
	{
		if (error.empty()) error = "legacy DDPF_ALPHA 8-bit descriptor mismatch";
		return false;
	}
	// Legacy FOURCC 28 (D3DFMT_A8) uncompressed 8-bit alpha texture.
	const auto fourCcA8 = makeTestDds(4u, 4u, 1u, 28u, false, 0u, 16u);
	if (!expectSuccess(fourCcA8, R4PixelFormat::A8Unorm, 4u, 4u, 1u))
	{
		if (error.empty()) error = "legacy FOURCC 28 D3DFMT_A8 descriptor mismatch";
		return false;
	}
	// Legacy 32-bit uncompressed BGRX (no alpha channel).
	auto bgrx32 = makeTestDds(2u, 2u, 1u, 0u, false, 0u, 16u);
	writeU32(bgrx32, 4u + 76u, kDdsPfRgb);
	writeU32(bgrx32, 4u + 80u, 0u);
	writeU32(bgrx32, 4u + 84u, 32u);
	writeU32(bgrx32, 4u + 88u, 0x00ff0000u);
	writeU32(bgrx32, 4u + 92u, 0x0000ff00u);
	writeU32(bgrx32, 4u + 96u, 0x000000ffu);
	writeU32(bgrx32, 4u + 100u, 0u);
	if (!expectSuccess(bgrx32, R4PixelFormat::BGRA8Unorm, 2u, 2u, 1u))
	{
		if (error.empty()) error = "legacy 32-bit BGRX descriptor mismatch";
		return false;
	}
	// Legacy cube storage is face-major: +X/-X/+Y/-Y/+Z/-Z, then mip 0..N.
	// A DXT1 4x4 and 2x2 chain consumes two 8-byte blocks per face.
	const auto legacyCube = makeTestDds(4u, 4u, 2u, fourCc('D', 'X', 'T', '1'), false, 0u,
		6u * 16u, kDdsCaps2CubeMapComplete);
	R4DdsTexture legacyCubeTexture;
	if (!ParseR4DdsTexture(legacyCube, legacyCubeTexture, error) ||
		legacyCubeTexture.descriptor.type != R4TextureType::TextureCube ||
		legacyCubeTexture.descriptor.arrayLength != 1u || legacyCubeTexture.mipLevels.size() != 12u)
	{
		if (error.empty()) error = "legacy cubemap descriptor mismatch";
		return false;
	}
	for (std::uint32_t face = 0; face < 6u; ++face)
	{
		for (std::uint32_t mip = 0; mip < 2u; ++mip)
		{
			const R4DdsMipLevel& level = legacyCubeTexture.mipLevels[face * 2u + mip];
			const std::size_t expectedOffset = kBaseDataOffset + static_cast<std::size_t>(face) * 16u + mip * 8u;
			if (level.arraySlice != face || level.mipLevel != mip || level.dataOffset != expectedOffset ||
				level.bytesPerRow != 8u || level.bytesPerImage != 8u)
			{
				error = "legacy cubemap subresource offsets are not face-major";
				return false;
			}
		}
	}
	// DX10 has no mandatory legacy caps2 face bits. The resource misc flag is
	// authoritative and its single array element represents all six faces.
	const auto dx10Cube = makeTestDds(4u, 4u, 1u, fourCc('D', 'X', '1', '0'), true, 98u,
		6u * 16u, 0u, kDdsResourceMiscTextureCube);
	R4DdsTexture dx10CubeTexture;
	if (!ParseR4DdsTexture(dx10Cube, dx10CubeTexture, error) ||
		dx10CubeTexture.descriptor.type != R4TextureType::TextureCube ||
		dx10CubeTexture.mipLevels.size() != 6u)
	{
		if (error.empty()) error = "DX10 cubemap descriptor mismatch";
		return false;
	}
	for (std::uint32_t face = 0; face < 6u; ++face)
	{
		const R4DdsMipLevel& level = dx10CubeTexture.mipLevels[face];
		if (level.arraySlice != face || level.mipLevel != 0u ||
			level.dataOffset != kBaseDataOffset + kDx10HeaderSize + static_cast<std::size_t>(face) * 16u)
		{
			error = "DX10 cubemap face offsets are not exact";
			return false;
		}
	}
	std::vector<std::uint8_t> truncated(dxt5.begin(), dxt5.end() - 1);
	R4DdsTexture truncatedTexture;
	std::string truncatedError;
	if (ParseR4DdsTexture(truncated, truncatedTexture, truncatedError) ||
		truncatedError != "DDS mip payload is truncated")
	{
		error = "DDS truncated declared mip chain was not rejected exactly";
		return false;
	}
	std::vector<std::uint8_t> incompleteCube = legacyCube;
	writeU32(incompleteCube, 4u + 108u, kDdsCaps2CubeMap | kDdsCaps2CubeMapPositiveX);
	if (!expectFailure(incompleteCube)) return false;
	std::vector<std::uint8_t> cubeArray = dx10Cube;
	writeU32(cubeArray, kBaseDataOffset + 12u, 2u);
	if (!expectFailure(cubeArray)) return false;
	std::vector<std::uint8_t> volume = dxt5;
	writeU32(volume, 4u + 108u, kDdsCaps2Volume);
	if (!expectFailure(volume)) return false;
	std::vector<std::uint8_t> unknown = dxt5;
	writeU32(unknown, 4u + 80u, fourCc('B', 'A', 'D', '!'));
	if (!expectFailure(unknown)) return false;
	// Sum of full-resolution 32-bit mip images cannot fit in size_t on 64-bit;
	// it also remains rejected safely on narrower hosts.
	const auto overflowing = makeTestDds(0xffffffffu, 0xffffffffu, 32u, fourCc('D', 'X', 'T', '1'), false, 0u, 0u);
	if (!expectFailure(overflowing)) return false;
	error.clear();
	return true;
}
} // namespace xray::metal
