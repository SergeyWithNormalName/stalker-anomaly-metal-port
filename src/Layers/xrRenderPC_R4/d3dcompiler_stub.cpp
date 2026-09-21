/*
 * d3dcompiler_stub.cpp
 *
 * Non-Windows stub implementations for D3DCompiler functions.
 * On macOS the engine uses pre-compiled shader cache exclusively;
 * runtime HLSL compilation is a Windows-only feature. Cached DXBC still
 * requires real reflection and signature extraction, implemented in
 * d3dcompiler_dxbc_reflection.cpp.
 *
 * Compilation and developer disassembly remain unavailable. Reflection and
 * input signature extraction intentionally do not use fake success paths.
 */

#if !defined(_WIN32)

#include <d3dcompiler.h>

HRESULT CreateCachedDxbcReflection(const void* data, SIZE_T size, REFIID iid, void** reflector);
HRESULT CreateCachedDxbcInputSignatureBlob(const void* data, SIZE_T size, ID3DBlob** blob);
HRESULT CreateD3DCompilerBlob(SIZE_T size, ID3DBlob** blob);

extern "C" {

HRESULT WINAPI D3DCompile(
    LPCVOID pSrcData, SIZE_T SrcDataSize,
    LPCSTR pSourceName,
    const D3D_SHADER_MACRO* pDefines,
    ID3DInclude* pInclude,
    LPCSTR pEntrypoint, LPCSTR pTarget,
    UINT Flags1, UINT Flags2,
    ID3DBlob** ppCode, ID3DBlob** ppErrorMsgs)
{
    (void)pSrcData; (void)SrcDataSize; (void)pSourceName;
    (void)pDefines; (void)pInclude; (void)pEntrypoint; (void)pTarget;
    (void)Flags1; (void)Flags2;
    if (ppCode)     *ppCode     = nullptr;
    if (ppErrorMsgs)*ppErrorMsgs = nullptr;
    return E_FAIL; /* triggers engine to load from shader cache */
}

HRESULT WINAPI D3DCompile2(
    LPCVOID pSrcData, SIZE_T SrcDataSize,
    LPCSTR pSourceName,
    const D3D_SHADER_MACRO* pDefines,
    ID3DInclude* pInclude,
    LPCSTR pEntrypoint, LPCSTR pTarget,
    UINT Flags1, UINT Flags2,
    UINT SecondaryDataFlags,
    LPCVOID pSecondaryData, SIZE_T SecondaryDataSize,
    ID3DBlob** ppCode, ID3DBlob** ppErrorMsgs)
{
    (void)pSrcData; (void)SrcDataSize; (void)pSourceName;
    (void)pDefines; (void)pInclude; (void)pEntrypoint; (void)pTarget;
    (void)Flags1; (void)Flags2; (void)SecondaryDataFlags;
    (void)pSecondaryData; (void)SecondaryDataSize;
    if (ppCode)     *ppCode     = nullptr;
    if (ppErrorMsgs)*ppErrorMsgs = nullptr;
    return E_FAIL;
}

HRESULT WINAPI D3DCompileFromFile(
    LPCWSTR pFileName,
    const D3D_SHADER_MACRO* pDefines,
    ID3DInclude* pInclude,
    LPCSTR pEntrypoint, LPCSTR pTarget,
    UINT Flags1, UINT Flags2,
    ID3DBlob** ppCode, ID3DBlob** ppErrorMsgs)
{
    (void)pFileName; (void)pDefines; (void)pInclude;
    (void)pEntrypoint; (void)pTarget; (void)Flags1; (void)Flags2;
    if (ppCode)     *ppCode     = nullptr;
    if (ppErrorMsgs)*ppErrorMsgs = nullptr;
    return E_FAIL;
}

HRESULT WINAPI D3DReflect(
    LPCVOID pSrcData, SIZE_T SrcDataSize,
    REFIID pInterface, void** ppReflector)
{
    return CreateCachedDxbcReflection(pSrcData, SrcDataSize, pInterface, ppReflector);
}

HRESULT WINAPI D3DGetInputSignatureBlob(
    LPCVOID pSrcData, SIZE_T SrcDataSize, ID3DBlob** ppSignatureBlob)
{
    return CreateCachedDxbcInputSignatureBlob(pSrcData, SrcDataSize, ppSignatureBlob);
}

HRESULT WINAPI D3DGetOutputSignatureBlob(
    LPCVOID pSrcData, SIZE_T SrcDataSize, ID3DBlob** ppSignatureBlob)
{
    (void)pSrcData; (void)SrcDataSize;
    if (ppSignatureBlob) *ppSignatureBlob = nullptr;
    return E_NOTIMPL;
}

HRESULT WINAPI D3DGetInputAndOutputSignatureBlob(
    LPCVOID pSrcData, SIZE_T SrcDataSize, ID3DBlob** ppSignatureBlob)
{
    (void)pSrcData; (void)SrcDataSize;
    if (ppSignatureBlob) *ppSignatureBlob = nullptr;
    return E_NOTIMPL;
}

HRESULT WINAPI D3DDisassemble(
    LPCVOID pSrcData, SIZE_T SrcDataSize, UINT Flags,
    LPCSTR szComments, ID3DBlob** ppDisassembly)
{
    (void)pSrcData; (void)SrcDataSize; (void)Flags; (void)szComments;
    if (ppDisassembly) *ppDisassembly = nullptr;
    return E_NOTIMPL;
}

HRESULT WINAPI D3DStripShader(
    LPCVOID pShaderBytecode, SIZE_T BytecodeLength,
    UINT uStripFlags, ID3DBlob** ppStrippedBlob)
{
    (void)pShaderBytecode; (void)BytecodeLength; (void)uStripFlags;
    if (ppStrippedBlob) *ppStrippedBlob = nullptr;
    return E_NOTIMPL;
}

HRESULT WINAPI D3DGetBlobPart(
    LPCVOID pSrcData, SIZE_T SrcDataSize,
    D3D_BLOB_PART Part, UINT Flags, ID3DBlob** ppPart)
{
    (void)pSrcData; (void)SrcDataSize; (void)Part; (void)Flags;
    if (ppPart) *ppPart = nullptr;
    return E_NOTIMPL;
}

HRESULT WINAPI D3DSetBlobPart(
    LPCVOID pSrcData, SIZE_T SrcDataSize,
    D3D_BLOB_PART Part, UINT Flags,
    LPCVOID pPart, SIZE_T PartSize, ID3DBlob** ppNewShader)
{
    (void)pSrcData; (void)SrcDataSize; (void)Part; (void)Flags;
    (void)pPart; (void)PartSize;
    if (ppNewShader) *ppNewShader = nullptr;
    return E_NOTIMPL;
}

HRESULT WINAPI D3DCreateBlob(SIZE_T Size, ID3DBlob** ppBlob)
{
    return CreateD3DCompilerBlob(Size, ppBlob);
}

HRESULT WINAPI D3DPreprocess(
    LPCVOID pSrcData, SIZE_T SrcDataSize,
    LPCSTR pSourceName,
    const D3D_SHADER_MACRO* pDefines,
    ID3DInclude* pInclude,
    ID3DBlob** ppCodeText, ID3DBlob** ppErrorMsgs)
{
    (void)pSrcData; (void)SrcDataSize; (void)pSourceName;
    (void)pDefines; (void)pInclude;
    if (ppCodeText)  *ppCodeText  = nullptr;
    if (ppErrorMsgs) *ppErrorMsgs = nullptr;
    return E_NOTIMPL;
}

} /* extern "C" */

#endif /* !_WIN32 */
