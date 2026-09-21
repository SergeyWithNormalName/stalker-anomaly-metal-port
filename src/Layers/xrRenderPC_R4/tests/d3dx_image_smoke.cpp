#include <d3dx11tex.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

int main(int argc, char** argv)
{
    if (argc != 5)
    {
        std::fprintf(stderr, "usage: %s <image> <width> <height> <png|bc7>\n", argv[0]);
        return 2;
    }

    std::ifstream file(argv[1], std::ios::binary);
    std::vector<unsigned char> bytes(std::istreambuf_iterator<char>(file), {});
    if (bytes.empty())
    {
        std::fprintf(stderr, "cannot read: %s\n", argv[1]);
        return 1;
    }

    D3DX11_IMAGE_INFO info = {};
    const HRESULT hr = D3DX11GetImageInfoFromMemory(bytes.data(), bytes.size(), nullptr, &info, nullptr);
    const UINT expectedWidth = static_cast<UINT>(std::strtoul(argv[2], nullptr, 10));
    const UINT expectedHeight = static_cast<UINT>(std::strtoul(argv[3], nullptr, 10));
    const bool expectsPng = std::strcmp(argv[4], "png") == 0;
    const bool expectsBc7 = std::strcmp(argv[4], "bc7") == 0;
    const DXGI_FORMAT expectedFormat = expectsPng ? DXGI_FORMAT_R8G8B8A8_UNORM
        : expectsBc7 ? DXGI_FORMAT_BC7_UNORM : DXGI_FORMAT_UNKNOWN;
    const UINT expectedFileFormat = expectsPng ? D3DX11_IFF_PNG
        : expectsBc7 ? D3DX11_IFF_DDS : D3DX11_IFF_FORCE_DWORD;
    if (expectedFormat == DXGI_FORMAT_UNKNOWN || FAILED(hr)
        || info.Width != expectedWidth || info.Height != expectedHeight
        || info.ResourceDimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D
        || info.Format != expectedFormat || info.ImageFileFormat != expectedFileFormat)
    {
        std::fprintf(stderr, "unexpected image info hr=0x%08x %ux%u format=%u dimension=%u\n",
                     unsigned(hr), info.Width, info.Height, unsigned(info.Format),
                     unsigned(info.ResourceDimension));
        return 1;
    }

    std::printf("%s image info: %ux%u, format=%u\n", argv[4], info.Width, info.Height,
                unsigned(info.Format));
    return 0;
}
