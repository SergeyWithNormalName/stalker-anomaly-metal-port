#include <d3d11shader.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

HRESULT CreateCachedDxbcReflection(const void* data, SIZE_T size, REFIID iid, void** reflector);
HRESULT CreateCachedDxbcInputSignatureBlob(const void* data, SIZE_T size, ID3DBlob** blob);

namespace
{
bool inspect(const char* path)
{
    std::ifstream file(path, std::ios::binary);
    std::vector<unsigned char> bytes(std::istreambuf_iterator<char>(file), {});
    if (bytes.size() < 4)
    {
        std::fprintf(stderr, "too small: %s\n", path);
        return false;
    }

    size_t dxbcOffset = 0;
    if (std::memcmp(bytes.data(), "DXBC", 4) != 0)
    {
        if (bytes.size() < 8 || std::memcmp(bytes.data() + 4, "DXBC", 4) != 0)
        {
            std::fprintf(stderr, "missing DXBC container: %s\n", path);
            return false;
        }
        dxbcOffset = 4; // Anomaly cache prefix: source CRC32.
    }

    const void* dxbc = bytes.data() + dxbcOffset;
    const SIZE_T dxbcSize = bytes.size() - dxbcOffset;
    ID3D11ShaderReflection* reflection = nullptr;
    HRESULT hr = CreateCachedDxbcReflection(dxbc, dxbcSize, IID_ID3D11ShaderReflection,
                                             reinterpret_cast<void**>(&reflection));
    if (FAILED(hr) || !reflection)
    {
        std::fprintf(stderr, "reflection failed 0x%08x: %s\n", unsigned(hr), path);
        return false;
    }

    D3D11_SHADER_DESC shader = {};
    if (FAILED(reflection->GetDesc(&shader)))
    {
        reflection->Release();
        return false;
    }

    std::printf("%s: CB=%u resources=%u input=%u output=%u\n", path,
                shader.ConstantBuffers, shader.BoundResources,
                shader.InputParameters, shader.OutputParameters);

    for (UINT i = 0; i < shader.ConstantBuffers; ++i)
    {
        auto* buffer = reflection->GetConstantBufferByIndex(i);
        D3D11_SHADER_BUFFER_DESC bufferDesc = {};
        if (!buffer || FAILED(buffer->GetDesc(&bufferDesc)) || !bufferDesc.Name)
        {
            reflection->Release();
            return false;
        }
        for (UINT j = 0; j < bufferDesc.Variables; ++j)
        {
            auto* variable = buffer->GetVariableByIndex(j);
            D3D11_SHADER_VARIABLE_DESC variableDesc = {};
            D3D11_SHADER_TYPE_DESC typeDesc = {};
            if (!variable || FAILED(variable->GetDesc(&variableDesc)) || !variableDesc.Name
                || !variable->GetType() || FAILED(variable->GetType()->GetDesc(&typeDesc)))
            {
                reflection->Release();
                return false;
            }
        }
    }

    for (UINT i = 0; i < shader.BoundResources; ++i)
    {
        D3D11_SHADER_INPUT_BIND_DESC resource = {};
        if (FAILED(reflection->GetResourceBindingDesc(i, &resource)) || !resource.Name)
        {
            reflection->Release();
            return false;
        }
		std::printf("  resource[%u] name=%s type=%u slot=%u count=%u\n", i,
		            resource.Name, unsigned(resource.Type), resource.BindPoint, resource.BindCount);
    }

    for (UINT i = 0; i < shader.InputParameters; ++i)
    {
        D3D11_SIGNATURE_PARAMETER_DESC parameter = {};
        if (FAILED(reflection->GetInputParameterDesc(i, &parameter)) || !parameter.SemanticName)
        {
            reflection->Release();
            return false;
        }
    }

    ID3DBlob* inputSignature = nullptr;
    hr = CreateCachedDxbcInputSignatureBlob(dxbc, dxbcSize, &inputSignature);
    if (shader.InputParameters && (FAILED(hr) || !inputSignature
        || inputSignature->GetBufferSize() != dxbcSize
        || std::memcmp(inputSignature->GetBufferPointer(), "DXBC", 4) != 0))
    {
        reflection->Release();
        return false;
    }
    if (inputSignature)
        inputSignature->Release();
    reflection->Release();
    return true;
}
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <cached-dxbc> [cached-dxbc...]\n", argv[0]);
        return 2;
    }

    for (int i = 1; i < argc; ++i)
        if (!inspect(argv[i]))
            return 1;
    return 0;
}
