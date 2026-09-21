#include "../R4DdsTexture.h"

#include <filesystem>
#include <iostream>
#include <locale>
#include <string>

namespace
{
const char* formatName(const R4PixelFormat format)
{
	switch (format)
	{
	case R4PixelFormat::A8Unorm: return "A8_UNORM";
	case R4PixelFormat::RGBA8Unorm: return "RGBA8_UNORM";
	case R4PixelFormat::BGRA8Unorm: return "BGRA8_UNORM";
	case R4PixelFormat::BC1Rgba: return "BC1_RGBA_UNORM";
	case R4PixelFormat::BC1RgbaSrgb: return "BC1_RGBA_SRGB";
	case R4PixelFormat::BC2Rgba: return "BC2_RGBA_UNORM";
	case R4PixelFormat::BC2RgbaSrgb: return "BC2_RGBA_SRGB";
	case R4PixelFormat::BC3Rgba: return "BC3_RGBA_UNORM";
	case R4PixelFormat::BC3RgbaSrgb: return "BC3_RGBA_SRGB";
	case R4PixelFormat::BC4RUnorm: return "BC4_R_UNORM";
	case R4PixelFormat::BC5RgUnorm: return "BC5_RG_UNORM";
	case R4PixelFormat::BC7RgbaUnorm: return "BC7_RGBA_UNORM";
	case R4PixelFormat::BC7RgbaUnormSrgb: return "BC7_RGBA_SRGB";
	default: return "INVALID";
	}
}
} // namespace

int main(int argc, char** argv)
{
	std::cout.imbue(std::locale::classic());
	if (argc == 2 && std::string(argv[1]) == "--self-test")
	{
		std::string error;
		if (!xray::metal::RunR4DdsTextureSelfTest(error))
		{
			std::cerr << "METAL_DDS_SELF_TEST result=FAIL error=" << error << '\n';
			return 1;
		}
		std::cout << "METAL_DDS_SELF_TEST result=PASS\n";
		return 0;
	}
	if (argc != 3 || std::string(argv[1]) != "--dds")
	{
		std::cerr << "usage: " << argv[0] << " --dds <file> | --self-test\n";
		return 2;
	}

	const std::filesystem::path path(argv[2]);
	xray::metal::R4DdsTexture texture;
	std::string error;
	if (!xray::metal::LoadR4DdsTexture(path, texture, error))
	{
		std::cerr << "METAL_DDS_COMPLETE result=FAIL error=" << error << '\n';
		return 1;
	}
	std::cout << "METAL_DDS_BEGIN file=" << path.generic_string() << '\n';
	std::cout << "METAL_DDS_TEXTURE width=" << texture.descriptor.width
		<< " height=" << texture.descriptor.height
		<< " mips=" << texture.descriptor.mipLevels
		<< " array_length=" << texture.descriptor.arrayLength
		<< " type=" << (texture.descriptor.type == R4TextureType::TextureCube ? "cube" : "2d")
		<< " format=" << formatName(texture.descriptor.format)
		<< " retained_bytes=" << texture.bytes.size() << '\n';
	for (std::size_t level = 0; level < texture.mipLevels.size(); ++level)
	{
		const auto& mip = texture.mipLevels[level];
		std::cout << "METAL_DDS_MIP level=" << level
			<< " mip=" << mip.mipLevel << " slice=" << mip.arraySlice
			<< " width=" << mip.width << " height=" << mip.height
			<< " offset=" << mip.dataOffset << " row_bytes=" << mip.bytesPerRow
			<< " image_bytes=" << mip.bytesPerImage << '\n';
	}
	std::cout << "METAL_DDS_COMPLETE result=PASS\n";
	return 0;
}
