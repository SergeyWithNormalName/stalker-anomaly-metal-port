/*
 * d3dx_stub.cpp
 *
 * Non-Windows stub / implementation of D3DX9 / D3DX11 math and texture helper
 * functions for macOS (arm64) builds.
 *
 * Matrix functions are real math implementations (not no-ops) because the
 * renderer uses them at runtime.  Texture helpers support the native DDS
 * payloads as well as PNG payloads that GAMMA deliberately stores with a
 * .dds resource name.
 */

#if !defined(_WIN32)

#include <d3dx9math.h>
#include <d3d11.h>
#include <math.h>
#include <string.h>
#include <algorithm>
#include <climits>
#include <vector>

#if defined(__APPLE__)
#pragma push_macro("interface")
#undef interface
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#pragma pop_macro("interface")
#endif

/* ──────────────────────────────────────────────────────────
   D3DX Float16 helpers
   ────────────────────────────────────────────────────────── */
static inline WORD floatToHalf(float f)
{
    unsigned int ui;
    memcpy(&ui, &f, sizeof(ui));
    int s = (ui >> 16) & 0x8000;
    int e = ((ui >> 23) & 0xFF) - 127 + 15;
    int m = (ui >> 13) & 0x3FF;
    if (e <= 0)  return (WORD)s;
    if (e >= 31) return (WORD)(s | 0x7C00);
    return (WORD)(s | (e << 10) | m);
}

static inline float halfToFloat(WORD h)
{
    unsigned int s = (h & 0x8000) << 16;
    unsigned int e = (h >> 10) & 0x1F;
    unsigned int m = h & 0x3FF;
    unsigned int ui;
    if (e == 0)       { ui = s; }
    else if (e == 31) { ui = s | 0x7F800000 | (m << 13); }
    else              { ui = s | ((e + 127 - 15) << 23) | (m << 13); }
    float f;
    memcpy(&f, &ui, sizeof(f));
    return f;
}

extern "C" {

D3DXFLOAT16* WINAPI D3DXFloat32To16Array(D3DXFLOAT16* pout, const FLOAT* pin, UINT n)
{
    for (UINT i = 0; i < n; ++i)
        reinterpret_cast<WORD*>(pout)[i] = floatToHalf(pin[i]);
    return pout;
}

FLOAT* WINAPI D3DXFloat16To32Array(FLOAT* pout, const D3DXFLOAT16* pin, UINT n)
{
    for (UINT i = 0; i < n; ++i)
        pout[i] = halfToFloat(reinterpret_cast<const WORD*>(pin)[i]);
    return pout;
}

/* ──────────────────────────────────────────────────────────
   D3DX Matrix math  (column-major, D3D left-handed convention)
   The D3DXMATRIX is stored row-major in memory: m[row][col].
   ────────────────────────────────────────────────────────── */

D3DXMATRIX* WINAPI D3DXMatrixTranspose(D3DXMATRIX* pout, const D3DXMATRIX* pm)
{
    D3DXMATRIX tmp = *pm;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            pout->m[r][c] = tmp.m[c][r];
    return pout;
}

D3DXMATRIX* WINAPI D3DXMatrixMultiply(D3DXMATRIX* pout,
                                       const D3DXMATRIX* pm1,
                                       const D3DXMATRIX* pm2)
{
    D3DXMATRIX tmp;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            tmp.m[r][c] = 0.0f;
            for (int k = 0; k < 4; ++k)
                tmp.m[r][c] += pm1->m[r][k] * pm2->m[k][c];
        }
    *pout = tmp;
    return pout;
}

D3DXMATRIX* WINAPI D3DXMatrixScaling(D3DXMATRIX* pout, FLOAT sx, FLOAT sy, FLOAT sz)
{
    memset(pout, 0, sizeof(*pout));
    pout->m[3][3] = 1.0f;
    pout->m[0][0] = sx;
    pout->m[1][1] = sy;
    pout->m[2][2] = sz;
    return pout;
}

D3DXMATRIX* WINAPI D3DXMatrixTranslation(D3DXMATRIX* pout, FLOAT x, FLOAT y, FLOAT z)
{
    memset(pout, 0, sizeof(*pout));
    pout->m[0][0] = pout->m[1][1] = pout->m[2][2] = pout->m[3][3] = 1.0f;
    pout->m[3][0] = x;
    pout->m[3][1] = y;
    pout->m[3][2] = z;
    return pout;
}

D3DXMATRIX* WINAPI D3DXMatrixOrthoOffCenterLH(D3DXMATRIX* pout,
                                               FLOAT l, FLOAT r, FLOAT b, FLOAT t,
                                               FLOAT zn, FLOAT zf)
{
    memset(pout, 0, sizeof(*pout));
    pout->m[0][0] =  2.0f / (r - l);
    pout->m[1][1] =  2.0f / (t - b);
    pout->m[2][2] =  1.0f / (zf - zn);
    pout->m[3][0] = -(r + l) / (r - l);
    pout->m[3][1] = -(t + b) / (t - b);
    pout->m[3][2] = -zn / (zf - zn);
    pout->m[3][3] =  1.0f;
    return pout;
}

/* 4×4 cofactor/adjugate inversion */
D3DXMATRIX* WINAPI D3DXMatrixInverse(D3DXMATRIX* pout, FLOAT* pdet, const D3DXMATRIX* pm)
{
    /* Expand to flat float array for clarity */
    const float* m = &pm->m[0][0];
    float inv[16];

    inv[ 0] =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[ 4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[ 8] =  m[4]*m[ 9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[ 9];
    inv[12] = -m[4]*m[ 9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[ 9];
    inv[ 1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[ 5] =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[ 9] = -m[0]*m[ 9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[ 9];
    inv[13] =  m[0]*m[ 9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[ 9];
    inv[ 2] =  m[1]*m[ 6]*m[15] - m[1]*m[ 7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[ 7] - m[13]*m[3]*m[ 6];
    inv[ 6] = -m[0]*m[ 6]*m[15] + m[0]*m[ 7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[ 7] + m[12]*m[3]*m[ 6];
    inv[10] =  m[0]*m[ 5]*m[15] - m[0]*m[ 7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[ 7] - m[12]*m[3]*m[ 5];
    inv[14] = -m[0]*m[ 5]*m[14] + m[0]*m[ 6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[ 6] + m[12]*m[2]*m[ 5];
    inv[ 3] = -m[1]*m[ 6]*m[11] + m[1]*m[ 7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[ 9]*m[2]*m[ 7] + m[ 9]*m[3]*m[ 6];
    inv[ 7] =  m[0]*m[ 6]*m[11] - m[0]*m[ 7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[ 8]*m[2]*m[ 7] - m[ 8]*m[3]*m[ 6];
    inv[11] = -m[0]*m[ 5]*m[11] + m[0]*m[ 7]*m[ 9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[ 9] - m[ 8]*m[1]*m[ 7] + m[ 8]*m[3]*m[ 5];
    inv[15] =  m[0]*m[ 5]*m[10] - m[0]*m[ 6]*m[ 9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[ 9] + m[ 8]*m[1]*m[ 6] - m[ 8]*m[2]*m[ 5];

    float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (pdet) *pdet = det;
    if (det == 0.0f) return nullptr;

    float inv_det = 1.0f / det;
    float* o = &pout->m[0][0];
    for (int i = 0; i < 16; ++i)
        o[i] = inv[i] * inv_det;
    return pout;
}

/* ──────────────────────────────────────────────────────────
   D3DX Plane helpers
   ────────────────────────────────────────────────────────── */

D3DXPLANE* WINAPI D3DXPlaneNormalize(D3DXPLANE* pout, const D3DXPLANE* pp)
{
    float len = sqrtf(pp->a*pp->a + pp->b*pp->b + pp->c*pp->c);
    if (len == 0.0f) { *pout = *pp; return pout; }
    float inv = 1.0f / len;
    pout->a = pp->a * inv;
    pout->b = pp->b * inv;
    pout->c = pp->c * inv;
    pout->d = pp->d * inv;
    return pout;
}

D3DXPLANE* WINAPI D3DXPlaneTransform(D3DXPLANE* pout, const D3DXPLANE* pp, const D3DXMATRIX* pm)
{
    /* Transform a plane: multiply plane vector by inverse-transpose of M */
    D3DXMATRIX inv, invT;
    D3DXMatrixInverse(&inv, nullptr, pm);
    D3DXMatrixTranspose(&invT, &inv);

    const float* m = &invT.m[0][0];
    pout->a = pp->a*m[0] + pp->b*m[4] + pp->c*m[8]  + pp->d*m[12];
    pout->b = pp->a*m[1] + pp->b*m[5] + pp->c*m[9]  + pp->d*m[13];
    pout->c = pp->a*m[2] + pp->b*m[6] + pp->c*m[10] + pp->d*m[14];
    pout->d = pp->a*m[3] + pp->b*m[7] + pp->c*m[11] + pp->d*m[15];
    return pout;
}

/* ──────────────────────────────────────────────────────────
   D3DX Vector helpers
   ────────────────────────────────────────────────────────── */

D3DXVECTOR4* WINAPI D3DXVec3Transform(D3DXVECTOR4* pout,
                                       const D3DXVECTOR3* pv,
                                       const D3DXMATRIX* pm)
{
    const float* m = &pm->m[0][0];
    float x = pv->x, y = pv->y, z = pv->z;
    pout->x = x*m[0] + y*m[4] + z*m[8]  + m[12];
    pout->y = x*m[1] + y*m[5] + z*m[9]  + m[13];
    pout->z = x*m[2] + y*m[6] + z*m[10] + m[14];
    pout->w = x*m[3] + y*m[7] + z*m[11] + m[15];
    return pout;
}

/* ──────────────────────────────────────────────────────────
   D3DX11 DDS texture helpers
   ────────────────────────────────────────────────────────── */

/* D3DX11_IMAGE_INFO and D3DX11_IMAGE_LOAD_INFO are declared in d3dx11tex.h
   which is not shipped in dxvk-native.  Provide minimal compatible structs. */
#ifndef D3DX11_IFF_DDS
#define D3DX11_IFF_DDS 0x0004
#define D3DX11_IFF_BMP 0x0000
#define D3DX11_IFF_PNG 0x0003
#define D3DX11_IFF_JPG 0x0001

typedef struct _D3DX11_IMAGE_INFO {
    UINT Width;
    UINT Height;
    UINT Depth;
    UINT ArraySize;
    UINT MipLevels;
    UINT MiscFlags;
    DXGI_FORMAT Format;
    D3D11_RESOURCE_DIMENSION ResourceDimension;
    UINT ImageFileFormat; /* D3DX11_IMAGE_FILE_FORMAT */
} D3DX11_IMAGE_INFO;

typedef struct _D3DX11_IMAGE_LOAD_INFO {
    UINT Width;
    UINT Height;
    UINT Depth;
    UINT FirstMipLevel;
    UINT MipLevels;
    UINT Usage;
    UINT BindFlags;
    UINT CpuAccessFlags;
    UINT MiscFlags;
    DXGI_FORMAT Format;
    UINT Filter;
    UINT MipFilter;
    D3DX11_IMAGE_INFO* pSrcInfo;
} D3DX11_IMAGE_LOAD_INFO;
#endif

namespace
{
constexpr UINT DdsMagic = 0x20534444; // "DDS "
constexpr UINT DdpfFourCC = 0x00000004;
constexpr UINT DdpfAlpha = 0x00000002;
constexpr UINT DdpfRgb = 0x00000040;
constexpr UINT DdpfLuminance = 0x00020000;
constexpr UINT DdpfBumpDuDv = 0x00080000;
constexpr UINT DdsCaps2Cubemap = 0x00000200;
constexpr UINT DdsFourCCDX10 = 0x30315844; // "DX10"
constexpr UINT DdsFourCCDXT1 = 0x31545844; // "DXT1"
constexpr UINT DdsFourCCDXT2 = 0x32545844; // "DXT2"
constexpr UINT DdsFourCCDXT3 = 0x33545844; // "DXT3"
constexpr UINT DdsFourCCDXT4 = 0x34545844; // "DXT4"
constexpr UINT DdsFourCCDXT5 = 0x35545844; // "DXT5"
constexpr UINT DdsFourCCATI1 = 0x31495441; // "ATI1"
constexpr UINT DdsFourCCATI2 = 0x32495441; // "ATI2"
constexpr UINT DdsFourCCBC4U = 0x55344342; // "BC4U"
constexpr UINT DdsFourCCBC4S = 0x53344342; // "BC4S"
constexpr UINT DdsFourCCBC5U = 0x55354342; // "BC5U"
constexpr UINT DdsFourCCBC5S = 0x53354342; // "BC5S"
constexpr UINT DdsFourCCA16B16G16R16 = 36;
constexpr UINT DdsFourCCV16U16 = 64;
constexpr UINT DdsFourCCQ16W16V16U16 = 110;
constexpr UINT DdsFourCCR16F = 111;
constexpr UINT DdsFourCCG16R16F = 112;
constexpr UINT DdsFourCCA16B16G16R16F = 113;
constexpr UINT DdsFourCCR32F = 114;
constexpr UINT DdsFourCCG32R32F = 115;
constexpr UINT DdsFourCCA32B32G32R32F = 116;
constexpr UINT D3DX11Default = 0xffffffffu;
constexpr DXGI_FORMAT D3DX11FormatFromFile = static_cast<DXGI_FORMAT>(0xfffffffdu);

struct DdsInfo
{
    UINT width = 0;
    UINT height = 0;
    UINT depth = 1;
    UINT mipLevels = 1;
    UINT arraySize = 1;
    UINT miscFlags = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    UINT sourceBytesPerPixel = 0;
    const unsigned char* pixels = nullptr;
    size_t pixelsSize = 0;
};

UINT readU32(const unsigned char* data)
{
    return UINT(data[0]) | (UINT(data[1]) << 8) | (UINT(data[2]) << 16) | (UINT(data[3]) << 24);
}

UINT readBigEndianU32(const unsigned char* data)
{
    return (UINT(data[0]) << 24) | (UINT(data[1]) << 16) | (UINT(data[2]) << 8) | UINT(data[3]);
}

bool isBlockCompressed(DXGI_FORMAT format, UINT& blockBytes)
{
    switch (format)
    {
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
        blockBytes = 8;
        return true;
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        blockBytes = 16;
        return true;
    default:
        blockBytes = 0;
        return false;
    }
}

UINT bytesPerPixel(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_A8_UNORM:
        return 1;
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_B5G6R5_UNORM:
    case DXGI_FORMAT_B5G5R5A1_UNORM:
    case DXGI_FORMAT_B4G4R4A4_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_FLOAT:
        return 2;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
        return 4;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
        return 8;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return 16;
    default:
        return 0;
    }
}

bool mapLegacyDdsFormat(UINT pixelFlags, UINT fourCC, UINT rgbBits,
                        UINT rMask, UINT gMask, UINT bMask, UINT aMask,
                        DXGI_FORMAT& format, UINT& sourceBytesPerPixel)
{
	sourceBytesPerPixel = 0;
    if (pixelFlags & DdpfFourCC)
    {
        switch (fourCC)
        {
        case DdsFourCCDXT1: format = DXGI_FORMAT_BC1_UNORM; return true;
        case DdsFourCCDXT2:
        case DdsFourCCDXT3: format = DXGI_FORMAT_BC2_UNORM; return true;
        case DdsFourCCDXT4:
        case DdsFourCCDXT5: format = DXGI_FORMAT_BC3_UNORM; return true;
        case DdsFourCCATI1:
        case DdsFourCCBC4U: format = DXGI_FORMAT_BC4_UNORM; return true;
        case DdsFourCCBC4S: format = DXGI_FORMAT_BC4_SNORM; return true;
        case DdsFourCCATI2:
        case DdsFourCCBC5U: format = DXGI_FORMAT_BC5_UNORM; return true;
        case DdsFourCCBC5S: format = DXGI_FORMAT_BC5_SNORM; return true;
        case DdsFourCCA16B16G16R16: format = DXGI_FORMAT_R16G16B16A16_UNORM; return true;
        case DdsFourCCV16U16: format = DXGI_FORMAT_R16G16_SNORM; return true;
        case DdsFourCCQ16W16V16U16: format = DXGI_FORMAT_R16G16B16A16_SNORM; return true;
        case DdsFourCCR16F: format = DXGI_FORMAT_R16_FLOAT; return true;
        case DdsFourCCG16R16F: format = DXGI_FORMAT_R16G16_FLOAT; return true;
        case DdsFourCCA16B16G16R16F: format = DXGI_FORMAT_R16G16B16A16_FLOAT; return true;
        case DdsFourCCR32F: format = DXGI_FORMAT_R32_FLOAT; return true;
        case DdsFourCCG32R32F: format = DXGI_FORMAT_R32G32_FLOAT; return true;
        case DdsFourCCA32B32G32R32F: format = DXGI_FORMAT_R32G32B32A32_FLOAT; return true;
        default: return false;
        }
    }

    if (pixelFlags & DdpfLuminance)
    {
        if (rgbBits == 8 && rMask == 0x000000ff)
        {
            format = DXGI_FORMAT_R8_UNORM;
            return true;
        }
        if (rgbBits == 16 && rMask == 0x0000ffff)
        {
            format = DXGI_FORMAT_R16_UNORM;
            return true;
        }
        if (rgbBits == 16 && rMask == 0x000000ff && aMask == 0x0000ff00)
        {
            format = DXGI_FORMAT_R8G8_UNORM;
            return true;
        }
    }
    if ((pixelFlags & DdpfAlpha) && rgbBits == 8 && aMask == 0x000000ff)
    {
        format = DXGI_FORMAT_A8_UNORM;
        return true;
    }
    if (pixelFlags & DdpfBumpDuDv)
    {
        if (rgbBits == 16 && rMask == 0x000000ff && gMask == 0x0000ff00)
        {
            format = DXGI_FORMAT_R8G8_SNORM;
            return true;
        }
        if (rgbBits == 32 && rMask == 0x000000ff && gMask == 0x0000ff00
            && bMask == 0x00ff0000 && aMask == 0xff000000)
        {
            format = DXGI_FORMAT_R8G8B8A8_SNORM;
            return true;
        }
        if (rgbBits == 32 && rMask == 0x0000ffff && gMask == 0xffff0000)
        {
            format = DXGI_FORMAT_R16G16_SNORM;
            return true;
        }
    }
    if (!(pixelFlags & DdpfRgb))
        return false;
    if (rgbBits == 24 && rMask == 0x00ff0000 && gMask == 0x0000ff00
        && bMask == 0x000000ff && aMask == 0)
    {
        // D3D11 has no 24-bit BGR texture format. D3DX expands this legacy
        // encoding to B8G8R8A8 while preserving the source channel order.
        format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sourceBytesPerPixel = 3;
        return true;
    }
    if (rgbBits == 32 && rMask == 0x00ff0000 && gMask == 0x0000ff00
        && bMask == 0x000000ff && aMask == 0xff000000)
    {
        format = DXGI_FORMAT_B8G8R8A8_UNORM;
        return true;
    }
    if (rgbBits == 32 && rMask == 0x000000ff && gMask == 0x0000ff00
        && bMask == 0x00ff0000 && aMask == 0xff000000)
    {
        format = DXGI_FORMAT_R8G8B8A8_UNORM;
        return true;
    }
    if (rgbBits == 32 && rMask == 0x00ff0000 && gMask == 0x0000ff00
        && bMask == 0x000000ff && aMask == 0)
    {
        format = DXGI_FORMAT_B8G8R8X8_UNORM;
        return true;
    }
    if (rgbBits == 32 && rMask == 0x0000ffff && gMask == 0xffff0000)
    {
        format = DXGI_FORMAT_R16G16_UNORM;
        return true;
    }
    if (rgbBits == 16 && rMask == 0x0000f800 && gMask == 0x000007e0 && bMask == 0x0000001f)
    {
        format = DXGI_FORMAT_B5G6R5_UNORM;
        return true;
    }
    if (rgbBits == 16 && rMask == 0x00007c00 && gMask == 0x000003e0
        && bMask == 0x0000001f && aMask == 0x00008000)
    {
        format = DXGI_FORMAT_B5G5R5A1_UNORM;
        return true;
    }
    if (rgbBits == 16 && rMask == 0x00000f00 && gMask == 0x000000f0
        && bMask == 0x0000000f && aMask == 0x0000f000)
    {
        format = DXGI_FORMAT_B4G4R4A4_UNORM;
        return true;
    }
    return false;
}

bool parseDds(const void* data, SIZE_T size, DdsInfo& info)
{
    if (!data || size < 128)
        return false;

    const auto* bytes = static_cast<const unsigned char*>(data);
    if (readU32(bytes) != DdsMagic || readU32(bytes + 4) != 124 || readU32(bytes + 76) != 32)
        return false;

    info.width = readU32(bytes + 16);
    info.height = readU32(bytes + 12);
    info.depth = readU32(bytes + 24) ? readU32(bytes + 24) : 1;
    info.mipLevels = readU32(bytes + 28) ? readU32(bytes + 28) : 1;
    const UINT pixelFlags = readU32(bytes + 80);
    const UINT fourCC = readU32(bytes + 84);
    const UINT caps2 = readU32(bytes + 112);
    size_t dataOffset = 128;

    if (!info.width || !info.height || !info.mipLevels)
        return false;

    if ((pixelFlags & DdpfFourCC) && fourCC == DdsFourCCDX10)
    {
        if (size < dataOffset + 20)
            return false;
        info.format = static_cast<DXGI_FORMAT>(readU32(bytes + dataOffset));
        const UINT dimension = readU32(bytes + dataOffset + 4);
        info.miscFlags = readU32(bytes + dataOffset + 8);
        info.arraySize = readU32(bytes + dataOffset + 12);
        dataOffset += 20;
        if (!info.arraySize || dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
            return false;
        info.dimension = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
        if (info.miscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE)
            info.arraySize *= 6;
    }
    else
    {
        if (!mapLegacyDdsFormat(pixelFlags, fourCC, readU32(bytes + 88),
                                readU32(bytes + 92), readU32(bytes + 96),
                                readU32(bytes + 100), readU32(bytes + 104), info.format,
                                info.sourceBytesPerPixel))
            return false;
        info.dimension = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
        if (caps2 & DdsCaps2Cubemap)
        {
            info.miscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
            info.arraySize = 6;
        }
    }

    if (dataOffset >= size || !bytesPerPixel(info.format))
    {
        UINT blockBytes = 0;
        if (!isBlockCompressed(info.format, blockBytes))
            return false;
    }

    info.pixels = bytes + dataOffset;
    info.pixelsSize = size - dataOffset;
    return true;
}

bool parsePngInfo(const void* data, SIZE_T size, DdsInfo& info)
{
    static constexpr unsigned char pngSignature[] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    if (!data || size < 24)
        return false;

    const auto* bytes = static_cast<const unsigned char*>(data);
    if (memcmp(bytes, pngSignature, sizeof(pngSignature)) != 0
        || readBigEndianU32(bytes + 8) != 13
        || memcmp(bytes + 12, "IHDR", 4) != 0)
        return false;

    info.width = readBigEndianU32(bytes + 16);
    info.height = readBigEndianU32(bytes + 20);
    if (!info.width || !info.height)
        return false;

    info.depth = 1;
    info.arraySize = 1;
    info.mipLevels = 1;
    info.miscFlags = 0;
    info.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    info.dimension = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
    return true;
}

#if defined(__APPLE__)
bool decodePng(const void* data, SIZE_T size, DdsInfo& info, std::vector<unsigned char>& pixels)
{
    if (!parsePngInfo(data, size, info))
        return false;

    const size_t rowBytes = size_t(info.width) * 4;
    if (info.height > SIZE_MAX / rowBytes)
        return false;

    CFDataRef encoded = CFDataCreate(kCFAllocatorDefault,
                                     static_cast<const UInt8*>(data),
                                     static_cast<CFIndex>(size));
    if (!encoded)
        return false;

    CGImageSourceRef source = CGImageSourceCreateWithData(encoded, nullptr);
    CFRelease(encoded);
    if (!source)
        return false;

    CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
    CFRelease(source);
    if (!image)
        return false;

    const size_t width = CGImageGetWidth(image);
    const size_t height = CGImageGetHeight(image);
    if (width != info.width || height != info.height || width > UINT_MAX || height > UINT_MAX)
    {
        CGImageRelease(image);
        return false;
    }

    pixels.resize(rowBytes * height);
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGContextRef context = colorSpace
        ? CGBitmapContextCreate(pixels.data(), width, height, 8, rowBytes, colorSpace,
                                kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big)
        : nullptr;
    if (colorSpace)
        CGColorSpaceRelease(colorSpace);
    if (!context)
    {
        CGImageRelease(image);
        pixels.clear();
        return false;
    }

    CGContextSetBlendMode(context, kCGBlendModeCopy);
    CGContextDrawImage(context, CGRectMake(0, 0, width, height), image);
    CGContextRelease(context);
    CGImageRelease(image);

    info.sourceBytesPerPixel = 4;
    info.pixels = pixels.data();
    info.pixelsSize = pixels.size();
    return true;
}
#endif

void fillImageInfo(const DdsInfo& info, UINT fileFormat, D3DX11_IMAGE_INFO& destination)
{
    destination.Width = info.width;
    destination.Height = info.height;
    destination.Depth = info.depth;
    destination.ArraySize = info.arraySize;
    destination.MipLevels = info.mipLevels;
    destination.MiscFlags = info.miscFlags;
    destination.Format = info.format;
    destination.ResourceDimension = info.dimension;
    destination.ImageFileFormat = fileFormat;
}

void logDdsParseFailure(const void* data, SIZE_T size)
{
    if (!data || size < 128)
    {
        fprintf(stderr, "! D3DX11: invalid DDS payload (%zu bytes)\n", size_t(size));
        return;
    }
    const auto* bytes = static_cast<const unsigned char*>(data);
    fprintf(stderr,
            "! D3DX11: unsupported DDS flags=0x%08x fourcc=0x%08x bpp=%u masks=%08x/%08x/%08x/%08x\n",
            readU32(bytes + 80), readU32(bytes + 84), readU32(bytes + 88),
            readU32(bytes + 92), readU32(bytes + 96), readU32(bytes + 100), readU32(bytes + 104));
}

bool getSubresourceLayout(DXGI_FORMAT format, UINT width, UINT height, UINT sourceBytesPerPixel,
                          size_t& rowBytes, size_t& rowCount, size_t& sliceBytes)
{
    UINT blockBytes = 0;
    if (isBlockCompressed(format, blockBytes))
    {
        rowBytes = size_t((width + 3) / 4) * blockBytes;
        rowCount = (height + 3) / 4;
    }
    else
    {
        const UINT bytes = sourceBytesPerPixel ? sourceBytesPerPixel : bytesPerPixel(format);
        if (!bytes)
            return false;
        rowBytes = size_t(width) * bytes;
        rowCount = height;
    }
    sliceBytes = rowBytes * rowCount;
    return true;
}
}

HRESULT WINAPI D3DX11GetImageInfoFromMemory(
    LPCVOID pSrcData, SIZE_T SrcDataSize,
    void* pPump, D3DX11_IMAGE_INFO* pSrcInfo, HRESULT* pHResult)
{
    DdsInfo info;
    UINT fileFormat = 0;
    const bool parsed = !pPump && pSrcInfo
        && ((parseDds(pSrcData, SrcDataSize, info) && (fileFormat = D3DX11_IFF_DDS))
            || (parsePngInfo(pSrcData, SrcDataSize, info) && (fileFormat = D3DX11_IFF_PNG)));
    if (!parsed && !pPump)
        logDdsParseFailure(pSrcData, SrcDataSize);
    const HRESULT hr = parsed ? S_OK : E_FAIL;
    if (SUCCEEDED(hr))
        fillImageInfo(info, fileFormat, *pSrcInfo);
    if (pHResult)
        *pHResult = hr;
    return hr;
}

HRESULT WINAPI D3DX11CreateTextureFromMemory(
    ID3D11Device* pDevice, LPCVOID pSrcData, SIZE_T SrcDataSize,
    D3DX11_IMAGE_LOAD_INFO* pLoadInfo,
    void* pPump, ID3D11Resource** ppTexture, HRESULT* pHResult)
{
    if (ppTexture)
        *ppTexture = nullptr;

    DdsInfo info;
    std::vector<unsigned char> decodedPng;
    UINT fileFormat = 0;
    const bool parsed = parseDds(pSrcData, SrcDataSize, info)
        ? ((fileFormat = D3DX11_IFF_DDS), true)
#if defined(__APPLE__)
        : (decodePng(pSrcData, SrcDataSize, info, decodedPng)
            ? ((fileFormat = D3DX11_IFF_PNG), true)
            : false);
#else
        : false;
#endif
    if (!pDevice || !ppTexture || pPump || !parsed)
    {
        if (!pPump)
            logDdsParseFailure(pSrcData, SrcDataSize);
        if (pHResult) *pHResult = E_FAIL;
        return E_FAIL;
    }

    if (pLoadInfo && pLoadInfo->pSrcInfo)
        fillImageInfo(info, fileFormat, *pLoadInfo->pSrcInfo);

    const UINT firstMip = pLoadInfo && pLoadInfo->FirstMipLevel != D3DX11Default
        ? pLoadInfo->FirstMipLevel : 0;
    if (firstMip >= info.mipLevels)
    {
        if (pHResult) *pHResult = E_INVALIDARG;
        return E_INVALIDARG;
    }
    const UINT availableMips = info.mipLevels - firstMip;
    // D3D11 uses zero MipLevels to request the full available chain. The
    // renderer's missing-bump fallback reaches this path without an image
    // header, leaving its copied MipLevels field at zero.
    const UINT requestedMips = pLoadInfo && pLoadInfo->MipLevels != D3DX11Default
        && pLoadInfo->MipLevels != 0
        ? pLoadInfo->MipLevels : availableMips;
    const UINT mipLevels = requestedMips < availableMips ? requestedMips : availableMips;
    if (!mipLevels)
    {
        if (pHResult) *pHResult = E_INVALIDARG;
        return E_INVALIDARG;
    }

    std::vector<D3D11_SUBRESOURCE_DATA> initialData;
    initialData.reserve(size_t(info.arraySize) * mipLevels);
    std::vector<std::vector<unsigned char>> convertedSubresources;
    convertedSubresources.reserve(size_t(info.arraySize) * mipLevels);
    const unsigned char* cursor = info.pixels;
    size_t remaining = info.pixelsSize;
    for (UINT arrayIndex = 0; arrayIndex < info.arraySize; ++arrayIndex)
    {
        for (UINT mip = 0; mip < info.mipLevels; ++mip)
        {
            const UINT width = std::max(1u, info.width >> mip);
            const UINT height = std::max(1u, info.height >> mip);
            size_t rowBytes = 0, rowCount = 0, sliceBytes = 0;
            if (!getSubresourceLayout(info.format, width, height, info.sourceBytesPerPixel,
                                      rowBytes, rowCount, sliceBytes)
                || sliceBytes > remaining || rowBytes > UINT_MAX || sliceBytes > UINT_MAX)
            {
                if (pHResult) *pHResult = E_FAIL;
                return E_FAIL;
            }
            if (mip >= firstMip && mip < firstMip + mipLevels)
            {
                D3D11_SUBRESOURCE_DATA subresource = {};
				if (info.sourceBytesPerPixel == 3)
				{
					const size_t destinationRowBytes = size_t(width) * 4;
					convertedSubresources.emplace_back(destinationRowBytes * height);
					auto& converted = convertedSubresources.back();
					for (UINT row = 0; row < height; ++row)
					{
						const auto* source = cursor + row * rowBytes;
						auto* destination = converted.data() + row * destinationRowBytes;
						for (UINT column = 0; column < width; ++column)
						{
							destination[column * 4 + 0] = source[column * 3 + 0];
							destination[column * 4 + 1] = source[column * 3 + 1];
							destination[column * 4 + 2] = source[column * 3 + 2];
							destination[column * 4 + 3] = 0xff;
						}
					}
					subresource.pSysMem = converted.data();
					subresource.SysMemPitch = UINT(destinationRowBytes);
					subresource.SysMemSlicePitch = UINT(converted.size());
				}
				else
				{
					subresource.pSysMem = cursor;
					subresource.SysMemPitch = UINT(rowBytes);
					subresource.SysMemSlicePitch = UINT(sliceBytes);
				}
                initialData.push_back(subresource);
            }
            cursor += sliceBytes;
            remaining -= sliceBytes;
        }
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = std::max(1u, info.width >> firstMip);
    desc.Height = std::max(1u, info.height >> firstMip);
    desc.MipLevels = mipLevels;
    desc.ArraySize = info.arraySize;
    desc.Format = pLoadInfo && pLoadInfo->Format != static_cast<DXGI_FORMAT>(D3DX11Default)
        && pLoadInfo->Format != D3DX11FormatFromFile
        ? pLoadInfo->Format : info.format;
    desc.SampleDesc.Count = 1;
    desc.Usage = pLoadInfo && pLoadInfo->Usage != D3DX11Default
        ? static_cast<D3D11_USAGE>(pLoadInfo->Usage) : D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = pLoadInfo && pLoadInfo->BindFlags != D3DX11Default
        ? pLoadInfo->BindFlags : D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = pLoadInfo && pLoadInfo->CpuAccessFlags != D3DX11Default
        ? pLoadInfo->CpuAccessFlags : 0;
    desc.MiscFlags = pLoadInfo && pLoadInfo->MiscFlags != D3DX11Default
        ? pLoadInfo->MiscFlags : info.miscFlags;

    ID3D11Texture2D* texture = nullptr;
    const HRESULT hr = pDevice->CreateTexture2D(&desc, initialData.data(), &texture);
    if (SUCCEEDED(hr))
        *ppTexture = texture;
    if (pHResult)
        *pHResult = hr;
    return hr;
}

HRESULT WINAPI D3DX11SaveTextureToFile(
    ID3D11DeviceContext* pContext, ID3D11Resource* pSrcTexture,
    UINT DestFormat, LPCWSTR pDestFile)
{
    (void)pContext; (void)pSrcTexture; (void)DestFormat; (void)pDestFile;
    return E_NOTIMPL;
}

HRESULT WINAPI D3DX11SaveTextureToMemory(
    ID3D11DeviceContext* pContext, ID3D11Resource* pSrcTexture,
    UINT DestFormat, void** ppDestBuf, UINT Flags)
{
    (void)pContext; (void)pSrcTexture; (void)DestFormat; (void)Flags;
    if (ppDestBuf) *ppDestBuf = nullptr;
    return E_NOTIMPL;
}

HRESULT WINAPI D3DX11LoadTextureFromTexture(
    ID3D11DeviceContext* pContext,
    ID3D11Resource* pSrcTexture, void* pLoadInfo,
    ID3D11Resource* pDstTexture)
{
    (void)pContext; (void)pSrcTexture; (void)pLoadInfo; (void)pDstTexture;
    return E_NOTIMPL;
}

HRESULT WINAPI D3DX11FilterTexture(
    ID3D11DeviceContext* pContext, ID3D11Resource* pTexture,
    UINT SrcLevel, UINT MipFilter)
{
    (void)pContext; (void)pTexture; (void)SrcLevel; (void)MipFilter;
    return E_NOTIMPL;
}

} /* extern "C" */

/* ──────────────────────────────────────────────────────────
   D3DX9 vertex declaration & FVF helpers
   ────────────────────────────────────────────────────────── */
#include <d3d9.h>
#include <d3dx9mesh.h>

extern "C" {

/* D3DXGetFVFVertexSize: compute stride of a vertex specified by FVF bits */
UINT WINAPI D3DXGetFVFVertexSize(DWORD FVF)
{
    static const DWORD texSizes[5] = { 8, 12, 16, 16, 0 };
    UINT size = 0;
    /* Position */
    DWORD posType = FVF & D3DFVF_POSITION_MASK;
    if      (posType == D3DFVF_XYZ)    size += 12;
    else if (posType == D3DFVF_XYZRHW) size += 16;
    else if (posType == D3DFVF_XYZB1)  size += 16;
    else if (posType == D3DFVF_XYZB2)  size += 20;
    else if (posType == D3DFVF_XYZB3)  size += 24;
    else if (posType == D3DFVF_XYZB4)  size += 28;
    else if (posType == D3DFVF_XYZB5)  size += 32;
    else if (posType == D3DFVF_XYZW)   size += 16;
    /* Normal */
    if (FVF & D3DFVF_NORMAL)  size += 12;
    /* Point size */
    if (FVF & D3DFVF_PSIZE)   size += 4;
    /* Diffuse */
    if (FVF & D3DFVF_DIFFUSE)  size += 4;
    /* Specular */
    if (FVF & D3DFVF_SPECULAR) size += 4;
    /* Texture coordinates */
    DWORD numTex = (FVF & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    for (DWORD i = 0; i < numTex; ++i) {
        DWORD fmt = (FVF >> (16 + i * 2)) & 0x3;
        size += (fmt < 4) ? texSizes[fmt] : 8;
    }
    return size;
}

/* D3DXGetDeclLength: return number of elements in a D3DVERTEXELEMENT9 array
   (not counting the D3DDECL_END sentinel). */
UINT WINAPI D3DXGetDeclLength(const D3DVERTEXELEMENT9* pDecl)
{
    UINT count = 0;
    while (pDecl->Stream != 0xFF) { ++pDecl; ++count; }
    return count;
}

/* D3DXGetDeclVertexSize: return byte stride for the given stream */
UINT WINAPI D3DXGetDeclVertexSize(const D3DVERTEXELEMENT9* pDecl, DWORD Stream)
{
    static const UINT typeSizes[] = {
        4,  /* D3DDECLTYPE_FLOAT1     0 */
        8,  /* D3DDECLTYPE_FLOAT2     1 */
        12, /* D3DDECLTYPE_FLOAT3     2 */
        16, /* D3DDECLTYPE_FLOAT4     3 */
        4,  /* D3DDECLTYPE_D3DCOLOR   4 */
        4,  /* D3DDECLTYPE_UBYTE4     5 */
        4,  /* D3DDECLTYPE_SHORT2     6 */
        8,  /* D3DDECLTYPE_SHORT4     7 */
        4,  /* D3DDECLTYPE_UBYTE4N    8 */
        4,  /* D3DDECLTYPE_SHORT2N    9 */
        8,  /* D3DDECLTYPE_SHORT4N    10 */
        4,  /* D3DDECLTYPE_USHORT2N   11 */
        8,  /* D3DDECLTYPE_USHORT4N   12 */
        4,  /* D3DDECLTYPE_UDEC3      13 */
        4,  /* D3DDECLTYPE_DEC3N      14 */
        4,  /* D3DDECLTYPE_FLOAT16_2  15 */
        8,  /* D3DDECLTYPE_FLOAT16_4  16 */
        0,  /* D3DDECLTYPE_UNUSED     17 */
    };
    UINT maxOff = 0;
    const D3DVERTEXELEMENT9* e = pDecl;
    const D3DVERTEXELEMENT9* last = nullptr;
    while (e->Stream != 0xFF) {
        if ((DWORD)e->Stream == Stream) {
            UINT off = e->Offset;
            if (!last || off > maxOff) { maxOff = off; last = e; }
        }
        ++e;
    }
    if (!last) return 0;
    UINT t = last->Type;
    UINT typeSize = (t < 18) ? typeSizes[t] : 4;
    return maxOff + typeSize;
}

/* D3DXDeclaratorFromFVF: convert FVF bitfield to D3DVERTEXELEMENT9 array */
HRESULT WINAPI D3DXDeclaratorFromFVF(DWORD FVF, D3DVERTEXELEMENT9 pDeclarator[MAX_FVF_DECL_SIZE])
{
    static const D3DVERTEXELEMENT9 END = D3DDECL_END();
    D3DVERTEXELEMENT9* p = pDeclarator;
    WORD offset = 0;

    auto addElem = [&](WORD stream, BYTE type, BYTE usage, BYTE usageIdx) {
        static const UINT typeSizes[] = {
            4,8,12,16,4,4,4,8,4,4,8,4,8,4,4,4,8,0
        };
        D3DVERTEXELEMENT9 e = {stream, offset, type, D3DDECLMETHOD_DEFAULT, usage, usageIdx};
        *p++ = e;
        offset += (WORD)((type < 18) ? typeSizes[type] : 4);
    };

    DWORD posType = FVF & D3DFVF_POSITION_MASK;
    if      (posType == D3DFVF_XYZ)    { addElem(0, D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0); }
    else if (posType == D3DFVF_XYZRHW) { addElem(0, D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITIONT, 0); }
    else if (posType == D3DFVF_XYZW)   { addElem(0, D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITION, 0); }

    if (FVF & D3DFVF_NORMAL)   addElem(0, D3DDECLTYPE_FLOAT3,   D3DDECLUSAGE_NORMAL,   0);
    if (FVF & D3DFVF_PSIZE)    addElem(0, D3DDECLTYPE_FLOAT1,   D3DDECLUSAGE_PSIZE,    0);
    if (FVF & D3DFVF_DIFFUSE)  addElem(0, D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR,    0);
    if (FVF & D3DFVF_SPECULAR) addElem(0, D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR,    1);

    DWORD numTex = (FVF & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    for (DWORD i = 0; i < numTex; ++i) {
        DWORD fmt = (FVF >> (16 + i * 2)) & 0x3;
        BYTE type;
        switch (fmt) {
            // D3DFVF_TEXTUREFORMAT{1,2,3,4} are encoded as {3,0,1,2},
            // not in their numeric component-count order.
            case 1:  type = D3DDECLTYPE_FLOAT3; break;
            case 2:  type = D3DDECLTYPE_FLOAT4; break;
            case 3:  type = D3DDECLTYPE_FLOAT1; break;
            default: type = D3DDECLTYPE_FLOAT2; break;
        }
        addElem(0, type, D3DDECLUSAGE_TEXCOORD, (BYTE)i);
    }

    *p = END;
    return S_OK;
}

/* D3DXGetDriverLevel — returns D3D9 driver level (deprecated, stub 900) */
UINT WINAPI D3DXGetDriverLevel(IDirect3DDevice9* pDevice)
{
    (void)pDevice;
    return 900; /* DX9 level */
}

/* D3DXLoadSurfaceFromSurface — stub (screenshot path, non-critical on macOS) */
HRESULT WINAPI D3DXLoadSurfaceFromSurface(
    IDirect3DSurface9* pDestSurface,
    const PALETTEENTRY* pDestPalette,
    const RECT* pDestRect,
    IDirect3DSurface9* pSrcSurface,
    const PALETTEENTRY* pSrcPalette,
    const RECT* pSrcRect,
    DWORD Filter,
    D3DCOLOR ColorKey)
{
    (void)pDestSurface; (void)pDestPalette; (void)pDestRect;
    (void)pSrcSurface;  (void)pSrcPalette;  (void)pSrcRect;
    (void)Filter; (void)ColorKey;
    return E_NOTIMPL;
}

/* D3DXCreateTexture — stub (D3D9 path, not used for R4/DX11) */
HRESULT WINAPI D3DXCreateTexture(
    IDirect3DDevice9* pDevice,
    UINT Width, UINT Height, UINT MipLevels, DWORD Usage,
    D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DTexture9** ppTexture)
{
    (void)pDevice; (void)Width; (void)Height; (void)MipLevels;
    (void)Usage; (void)Format; (void)Pool;
    if (ppTexture) *ppTexture = nullptr;
    return E_NOTIMPL;
}

/* D3DXSaveTextureToFileInMemory — stub */
HRESULT WINAPI D3DXSaveTextureToFileInMemory(
    LPD3DXBUFFER* ppDestBuf,
    D3DXIMAGE_FILEFORMAT DestFormat,
    IDirect3DBaseTexture9* pSrcTexture,
    const PALETTEENTRY* pSrcPalette)
{
    (void)DestFormat; (void)pSrcTexture; (void)pSrcPalette;
    if (ppDestBuf) *ppDestBuf = nullptr;
    return E_NOTIMPL;
}

} /* extern "C" — D3DX9 mesh helpers */

#endif /* !_WIN32 */
