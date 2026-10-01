/*
 * E00-only DXBC materializer for missing GAMMA scope shader records.
 *
 * The command that builds and runs this file is deliberately kept in
 * script/e00_compile_scope_dxbc.sh.  It uses Silo Wine's d3dcompiler_47
 * import library and runs with the bottle's native Microsoft DLL override.
 * This program writes only raw DXBC; the verifier adds X-Ray's source-CRC
 * prefix after independently reproducing ShaderSourceCRC.cpp.
 */

#define COBJMACROS
#include <windows.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string.h>

typedef HRESULT (WINAPI *D3DReflectProc)(const void*, SIZE_T, REFIID, void**);
/* IID_ID3D11ShaderReflection from d3d11shader.h.  Keep it local so the
 * MinGW fallback does not need a DirectX UUID import library. */
static const IID kIID_ID3D11ShaderReflection = {
    0x8d536ca1, 0x0cca, 0x4956, {0xa8, 0x37, 0x78, 0x69, 0x63, 0x75, 0x55, 0x84}
};

static D3D_SHADER_MACRO kShaderDefines[] = {
    { "SMAP_size", "2048" },
    { "FP16_FILTER", "1" },
    { "FP16_BLEND", "1" },
    { "USE_HWSMAP", "1" },
    { "USE_HWSMAP_PCF", "1" },
    { "USE_BRANCHING", "1" },
    { "USE_VTF", "1" },
    { "SKIN_2", "1" },
    { "USE_SOFT_WATER", "1" },
    { "USE_DOF", "1" },
    { "SUN_SHAFTS_QUALITY", "1" },
    { "USE_REFLECTIONS", "1" },
    { "SM_5", "1" },
    { "SSFX_SSR_QUALITY", "0" },
    { "SSFX_WATER_QUALITY", "1" },
    { "SSFX_WATER_PARALLAX", "2" },
    { "SSFX_IL_QUALITY", "16" },
    { "SSFX_AO_QUALITY", "2" },
    { "SSFX_POM_REFINE", "0" },
    { "SSFX_TERRA_POM_REFINE", "0" },
    { "SSFX_SSS_DIR_QUALITY", "6" },
    { "SSFX_SSS_OMNI_QUALITY", "2" },
    { "SSFX_MODEXE", "1" },
    { NULL, NULL },
};

static int write_blob(const char* output_path, ID3DBlob* blob)
{
    FILE* output = fopen(output_path, "wb");
    if (!output) {
        fprintf(stderr, "cannot open output: %s\n", output_path);
        return 1;
    }

    const size_t size = ID3D10Blob_GetBufferSize(blob);
    const size_t written = fwrite(ID3D10Blob_GetBufferPointer(blob), 1, size, output);
    fclose(output);
    if (written != size) {
        fprintf(stderr, "short write: %s\n", output_path);
        return 1;
    }
    return 0;
}

static int report_reflection(D3DReflectProc reflect, ID3DBlob* blob)
{
    ID3D11ShaderReflection* reflection = NULL;
    HRESULT hr = reflect(ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
        &kIID_ID3D11ShaderReflection, (void**)&reflection);
    if (FAILED(hr) || !reflection) {
        fprintf(stderr, "D3DReflect failed: 0x%08lx\n", (unsigned long)hr);
        return 1;
    }

    D3D11_SHADER_DESC description = {0};
    hr = reflection->lpVtbl->GetDesc(reflection, &description);
    if (FAILED(hr)) {
        fprintf(stderr, "ID3D11ShaderReflection::GetDesc failed: 0x%08lx\n", (unsigned long)hr);
        reflection->lpVtbl->Release(reflection);
        return 1;
    }

    printf("DXBC_REFLECTION cbuffers=%u resources=%u input=%u output=%u instructions=%u\n",
        description.ConstantBuffers, description.BoundResources, description.InputParameters,
        description.OutputParameters, description.InstructionCount);
    for (UINT index = 0; index < description.BoundResources; ++index) {
        D3D11_SHADER_INPUT_BIND_DESC resource = {0};
        hr = reflection->lpVtbl->GetResourceBindingDesc(reflection, index, &resource);
        if (FAILED(hr) || !resource.Name) {
            fprintf(stderr, "reflection resource %u failed: 0x%08lx\n", index, (unsigned long)hr);
            reflection->lpVtbl->Release(reflection);
            return 1;
        }
        printf("DXBC_RESOURCE index=%u name=%s type=%u slot=%u count=%u\n", index,
            resource.Name, (unsigned)resource.Type, resource.BindPoint, resource.BindCount);
    }

    reflection->lpVtbl->Release(reflection);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: %s <source> <raw-dxbc-output> <SKIN_NONE|SKIN_0..SKIN_4> <ps_5_0|vs_5_0>\n", argv[0]);
        return 2;
    }
    if (strcmp(argv[4], "ps_5_0") != 0 && strcmp(argv[4], "vs_5_0") != 0) {
        fprintf(stderr, "unsupported shader target: %s\n", argv[4]);
        return 2;
    }
    if (strcmp(argv[3], "SKIN_NONE") != 0 && strcmp(argv[3], "SKIN_0") != 0
        && strcmp(argv[3], "SKIN_1") != 0 && strcmp(argv[3], "SKIN_2") != 0
        && strcmp(argv[3], "SKIN_3") != 0 && strcmp(argv[3], "SKIN_4") != 0) {
        fprintf(stderr, "unsupported skin define: %s\n", argv[3]);
        return 2;
    }
    kShaderDefines[7].Name = argv[3];

    FILE* input = fopen(argv[1], "rb");
    if (!input) {
        fprintf(stderr, "cannot open source: %s\n", argv[1]);
        return 1;
    }
    if (fseek(input, 0, SEEK_END) != 0) {
        fclose(input);
        return 1;
    }
    const long length = ftell(input);
    if (length <= 0 || fseek(input, 0, SEEK_SET) != 0) {
        fclose(input);
        fprintf(stderr, "invalid source length: %s\n", argv[1]);
        return 1;
    }
    void* source = malloc((size_t)length);
    if (!source || fread(source, 1, (size_t)length, input) != (size_t)length) {
        free(source);
        fclose(input);
        fprintf(stderr, "cannot read source: %s\n", argv[1]);
        return 1;
    }
    fclose(input);

    /* Load this versioned DLL explicitly: Wine's unversioned d3dcompiler
     * import library is not evidence that the bottle's native 47 DLL ran. */
    HMODULE compiler = LoadLibraryA("d3dcompiler_47.dll");
    FARPROC compile_symbol = compiler ? GetProcAddress(compiler, "D3DCompile") : NULL;
    FARPROC reflect_symbol = compiler ? GetProcAddress(compiler, "D3DReflect") : NULL;
    pD3DCompile compile = NULL;
    D3DReflectProc reflect = NULL;
    /* FARPROC's old no-prototype type is intentionally not cast directly:
     * GCC correctly warns about that under -Wcast-function-type. */
    memcpy(&compile, &compile_symbol, sizeof(compile));
    memcpy(&reflect, &reflect_symbol, sizeof(reflect));
    if (!compile || !reflect) {
        fprintf(stderr, "native d3dcompiler_47.dll lacks D3DCompile/D3DReflect (error=%lu)\n",
            (unsigned long)GetLastError());
        if (compiler) FreeLibrary(compiler);
        return 1;
    }
    char compiler_path[MAX_PATH] = {0};
    if (!GetModuleFileNameA(compiler, compiler_path, MAX_PATH)) {
        fprintf(stderr, "cannot resolve loaded d3dcompiler_47.dll path (error=%lu)\n",
            (unsigned long)GetLastError());
        FreeLibrary(compiler);
        return 1;
    }
    printf("DXBC_COMPILER path=%s\n", compiler_path);

    ID3DBlob* dxbc = NULL;
    ID3DBlob* diagnostics = NULL;
    /* ShaderResourceTraits.h selects exactly D3D10_SHADER_PACK_MATRIX_ROW_MAJOR
     * for this R4 path.  Do not add DEBUG: compiler options affect DXBC. */
    const UINT flags = D3DCOMPILE_PACK_MATRIX_ROW_MAJOR;
    const HRESULT hr = compile(source, (SIZE_T)length, argv[1], kShaderDefines,
        D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", argv[4], flags, 0, &dxbc, &diagnostics);
    free(source);
    if (FAILED(hr) || !dxbc) {
        fprintf(stderr, "D3DCompile failed: 0x%08lx\n", (unsigned long)hr);
        if (diagnostics) {
            fwrite(ID3D10Blob_GetBufferPointer(diagnostics), 1, ID3D10Blob_GetBufferSize(diagnostics), stderr);
            fputc('\n', stderr);
        }
        if (diagnostics) ID3D10Blob_Release(diagnostics);
        if (dxbc) ID3D10Blob_Release(dxbc);
        FreeLibrary(compiler);
        return 1;
    }
    if (diagnostics) ID3D10Blob_Release(diagnostics);

    const unsigned char* bytes = (const unsigned char*)ID3D10Blob_GetBufferPointer(dxbc);
    if (ID3D10Blob_GetBufferSize(dxbc) < 4 || bytes[0] != 'D' || bytes[1] != 'X'
        || bytes[2] != 'B' || bytes[3] != 'C') {
        fprintf(stderr, "D3DCompile did not return DXBC\n");
        ID3D10Blob_Release(dxbc);
        FreeLibrary(compiler);
        return 1;
    }
    printf("DXBC_COMPILED bytes=%lu entry=main target=%s flags=0x%08x\n",
        (unsigned long)ID3D10Blob_GetBufferSize(dxbc), argv[4], flags);
    const int reflection_result = report_reflection(reflect, dxbc);
    const int write_result = reflection_result ? 1 : write_blob(argv[2], dxbc);
    ID3D10Blob_Release(dxbc);
    FreeLibrary(compiler);
    return write_result;
}
