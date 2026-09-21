// Minimal D3DCompiler reflection support for cached DXBC shaders on macOS.
//
// R4 consumes only the RDEF constant-buffer/resource metadata and vertex
// signatures.  DXVK Native already provides the MIT-licensed dxbc-spirv
// container and signature parser; this file adapts that parser and the RDEF
// payload into the D3D11 reflection interfaces that R4 actually calls.

#if !defined(_WIN32)

#include <d3dcompiler.h>
#include <d3d10.h>
#include <d3d10shader.h>
#include <d3d11.h>
#include <d3d11shader.h>

#include "../xrRenderDX10/DXCommonTypes.h"

#include <dxbc/dxbc_container.h>
#include <dxbc/dxbc_signature.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace
{
constexpr size_t RdefChunkHeaderSize = 8;
constexpr size_t RdefHeaderSize = 28;
constexpr size_t RdefConstantBufferSize = 24;
constexpr size_t RdefVariableSize = 40;
constexpr size_t RdefTypeSize = 20;
constexpr size_t RdefResourceSize = 32;

class Blob final : public ID3DBlob
{
public:
    explicit Blob(size_t size) : m_data(size) {}
    Blob(const void* data, size_t size) : m_data(size)
    {
        if (size)
            std::memcpy(m_data.data(), data, size);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
    {
        if (!object)
            return E_INVALIDARG;
        *object = nullptr;
        if (riid != IID_IUnknown && riid != IID_ID3DBlob)
            return E_NOINTERFACE;
        *object = static_cast<ID3DBlob*>(this);
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG refs = --m_refs;
        if (!refs)
            delete this;
        return refs;
    }

    void* STDMETHODCALLTYPE GetBufferPointer() override { return m_data.empty() ? nullptr : m_data.data(); }
    SIZE_T STDMETHODCALLTYPE GetBufferSize() override { return m_data.size(); }

private:
    std::atomic<ULONG> m_refs { 1 };
    std::vector<unsigned char> m_data;
};

class RdefView
{
public:
    explicit RdefView(const dxbc_spv::util::ByteReader& reader)
        : m_data(static_cast<const unsigned char*>(reader.getData(0))), m_size(reader.getSize()) {}

    bool dataU16(size_t offset, uint16_t& value) const
    {
        offset += RdefChunkHeaderSize;
        if (offset > m_size || sizeof(value) > m_size - offset)
            return false;
        value = uint16_t(m_data[offset]) | (uint16_t(m_data[offset + 1]) << 8);
        return true;
    }

    bool dataU32(size_t offset, uint32_t& value) const
    {
        offset += RdefChunkHeaderSize;
        if (offset > m_size || sizeof(value) > m_size - offset)
            return false;
        value = uint32_t(m_data[offset])
            | (uint32_t(m_data[offset + 1]) << 8)
            | (uint32_t(m_data[offset + 2]) << 16)
            | (uint32_t(m_data[offset + 3]) << 24);
        return true;
    }

    // dxbc-spirv's findChunk reader includes the RDEF FourCC/size header,
    // while RDEF's internal offsets are relative to the payload immediately
    // after that header. dataU* performs this adjustment for scalar fields;
    // the pointer-like helpers below must do it themselves.
    bool rdefU16(uint32_t offset, uint16_t& value) const
    {
        size_t dataOffset;
        return rdefOffset(offset, dataOffset) && dataU16(dataOffset, value);
    }

    bool rdefU32(uint32_t offset, uint32_t& value) const
    {
        size_t dataOffset;
        return rdefOffset(offset, dataOffset) && dataU32(dataOffset, value);
    }

    const char* stringAt(uint32_t offset) const
    {
        size_t dataOffset;
        if (!rdefOffset(offset, dataOffset))
            return nullptr;
		dataOffset += RdefChunkHeaderSize;
		if (dataOffset > m_size)
			return nullptr;

        const void* terminator = std::memchr(m_data + dataOffset, '\0', m_size - dataOffset);
        return terminator ? reinterpret_cast<const char*>(m_data + dataOffset) : nullptr;
    }

    const void* bytesAt(uint32_t offset, size_t size) const
    {
        size_t dataOffset;
        if (!rdefOffset(offset, dataOffset))
            return nullptr;
		dataOffset += RdefChunkHeaderSize;
		if (dataOffset > m_size || size > m_size - dataOffset)
			return nullptr;
        return m_data + dataOffset;
    }

    bool rdefRange(uint32_t offset, size_t size) const
    {
        size_t dataOffset;
        if (!rdefOffset(offset, dataOffset))
            return false;
		dataOffset += RdefChunkHeaderSize;
		return dataOffset <= m_size && size <= m_size - dataOffset;
    }

    size_t size() const { return m_size; }

private:
    bool rdefOffset(uint32_t offset, size_t& dataOffset) const
    {
        dataOffset = size_t(offset);
        return dataOffset <= m_size;
    }

    const unsigned char* m_data = nullptr;
    size_t m_size = 0;
};

class ReflectionType final : public ID3DShaderReflectionType
{
public:
    explicit ReflectionType(const D3D11_SHADER_TYPE_DESC& desc) : m_desc(desc) {}

    HRESULT STDMETHODCALLTYPE GetDesc(D3D11_SHADER_TYPE_DESC* desc) override
    {
        if (!desc)
            return E_INVALIDARG;
        *desc = m_desc;
        return S_OK;
    }
    ID3DShaderReflectionType* STDMETHODCALLTYPE GetMemberTypeByIndex(UINT) override { return nullptr; }
    ID3DShaderReflectionType* STDMETHODCALLTYPE GetMemberTypeByName(const char*) override { return nullptr; }
    const char* STDMETHODCALLTYPE GetMemberTypeName(UINT) override { return nullptr; }
    HRESULT STDMETHODCALLTYPE IsEqual(ID3DShaderReflectionType* type) override { return type == this ? S_OK : S_FALSE; }
    ID3DShaderReflectionType* STDMETHODCALLTYPE GetSubType() override { return nullptr; }
    ID3DShaderReflectionType* STDMETHODCALLTYPE GetBaseClass() override { return nullptr; }
    UINT STDMETHODCALLTYPE GetNumInterfaces() override { return 0; }
    ID3DShaderReflectionType* STDMETHODCALLTYPE GetInterfaceByIndex(UINT) override { return nullptr; }
    HRESULT STDMETHODCALLTYPE IsOfType(ID3DShaderReflectionType* type) override { return type == this ? S_OK : S_FALSE; }
    HRESULT STDMETHODCALLTYPE ImplementsInterface(ID3DShaderReflectionType*) override { return S_FALSE; }

private:
    D3D11_SHADER_TYPE_DESC m_desc = {};
};

class ReflectionConstantBuffer;

class ReflectionVariable final : public ID3DShaderReflectionVariable
{
public:
    ReflectionVariable(const D3D11_SHADER_VARIABLE_DESC& desc, ReflectionType* type, ReflectionConstantBuffer* buffer)
        : m_desc(desc), m_type(type), m_buffer(buffer) {}

    HRESULT STDMETHODCALLTYPE GetDesc(D3D11_SHADER_VARIABLE_DESC* desc) override
    {
        if (!desc)
            return E_INVALIDARG;
        *desc = m_desc;
        return S_OK;
    }
    ID3DShaderReflectionType* STDMETHODCALLTYPE GetType() override { return m_type; }
    ID3DShaderReflectionConstantBuffer* STDMETHODCALLTYPE GetBuffer() override;
    UINT STDMETHODCALLTYPE GetInterfaceSlot(UINT) override { return UINT(-1); }

private:
    D3D11_SHADER_VARIABLE_DESC m_desc = {};
    ReflectionType* m_type = nullptr;
    ReflectionConstantBuffer* m_buffer = nullptr;
};

class ReflectionConstantBuffer final : public ID3DShaderReflectionConstantBuffer
{
public:
    explicit ReflectionConstantBuffer(const D3D11_SHADER_BUFFER_DESC& desc) : m_desc(desc) {}

    ReflectionVariable* addVariable(const D3D11_SHADER_VARIABLE_DESC& desc, ReflectionType* type)
    {
        m_variables.emplace_back(std::make_unique<ReflectionVariable>(desc, type, this));
        m_desc.Variables = static_cast<UINT>(m_variables.size());
        return m_variables.back().get();
    }

    HRESULT STDMETHODCALLTYPE GetDesc(D3D11_SHADER_BUFFER_DESC* desc) override
    {
        if (!desc)
            return E_INVALIDARG;
        *desc = m_desc;
        return S_OK;
    }
    ID3DShaderReflectionVariable* STDMETHODCALLTYPE GetVariableByIndex(UINT index) override
    {
        return index < m_variables.size() ? m_variables[index].get() : nullptr;
    }
    ID3DShaderReflectionVariable* STDMETHODCALLTYPE GetVariableByName(const char* name) override
    {
        if (!name)
            return nullptr;
        for (const auto& variable : m_variables)
        {
            D3D11_SHADER_VARIABLE_DESC desc;
            variable->GetDesc(&desc);
            if (desc.Name && std::strcmp(desc.Name, name) == 0)
                return variable.get();
        }
        return nullptr;
    }

private:
    D3D11_SHADER_BUFFER_DESC m_desc = {};
    std::vector<std::unique_ptr<ReflectionVariable>> m_variables;
};

ID3DShaderReflectionConstantBuffer* ReflectionVariable::GetBuffer()
{
    return m_buffer;
}

class Reflection final : public ID3DShaderReflection
{
public:
    Reflection(const void* data, size_t size)
    {
        m_valid = initialize(data, size);
    }

    bool valid() const { return m_valid; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
    {
        if (!object)
            return E_INVALIDARG;
        *object = nullptr;
        if (riid != IID_IUnknown && riid != IID_ID3D11ShaderReflection)
            return E_NOINTERFACE;
        *object = static_cast<ID3DShaderReflection*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG refs = --m_refs;
        if (!refs)
            delete this;
        return refs;
    }

    HRESULT STDMETHODCALLTYPE GetDesc(D3D11_SHADER_DESC* desc) override
    {
        if (!desc)
            return E_INVALIDARG;
        *desc = m_desc;
        return S_OK;
    }
    ID3DShaderReflectionConstantBuffer* STDMETHODCALLTYPE GetConstantBufferByIndex(UINT index) override
    {
        return index < m_constantBuffers.size() ? m_constantBuffers[index].get() : nullptr;
    }
    ID3DShaderReflectionConstantBuffer* STDMETHODCALLTYPE GetConstantBufferByName(const char* name) override
    {
        if (!name)
            return nullptr;
        for (const auto& buffer : m_constantBuffers)
        {
            D3D11_SHADER_BUFFER_DESC desc;
            buffer->GetDesc(&desc);
            if (desc.Name && std::strcmp(desc.Name, name) == 0)
                return buffer.get();
        }
        return nullptr;
    }
    HRESULT STDMETHODCALLTYPE GetResourceBindingDesc(UINT index, D3D11_SHADER_INPUT_BIND_DESC* desc) override
    {
        if (!desc)
            return E_INVALIDARG;
        if (index >= m_resources.size())
            return E_FAIL;
        *desc = m_resources[index];
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetInputParameterDesc(UINT index, D3D11_SIGNATURE_PARAMETER_DESC* desc) override
    {
        return getSignatureDesc(m_inputParameters, index, desc);
    }
    HRESULT STDMETHODCALLTYPE GetOutputParameterDesc(UINT index, D3D11_SIGNATURE_PARAMETER_DESC* desc) override
    {
        return getSignatureDesc(m_outputParameters, index, desc);
    }
    HRESULT STDMETHODCALLTYPE GetPatchConstantParameterDesc(UINT index, D3D11_SIGNATURE_PARAMETER_DESC* desc) override
    {
        return getSignatureDesc(m_patchParameters, index, desc);
    }
    ID3DShaderReflectionVariable* STDMETHODCALLTYPE GetVariableByName(const char* name) override
    {
        if (!name)
            return nullptr;
        for (const auto& buffer : m_constantBuffers)
            if (auto* variable = buffer->GetVariableByName(name))
                return variable;
        return nullptr;
    }
    HRESULT STDMETHODCALLTYPE GetResourceBindingDescByName(const char* name, D3D11_SHADER_INPUT_BIND_DESC* desc) override
    {
        if (!name || !desc)
            return E_INVALIDARG;
        for (const auto& resource : m_resources)
        {
            if (resource.Name && std::strcmp(resource.Name, name) == 0)
            {
                *desc = resource;
                return S_OK;
            }
        }
        return E_FAIL;
    }
    UINT STDMETHODCALLTYPE GetMovInstructionCount() override { return 0; }
    UINT STDMETHODCALLTYPE GetMovcInstructionCount() override { return 0; }
    UINT STDMETHODCALLTYPE GetConversionInstructionCount() override { return 0; }
    UINT STDMETHODCALLTYPE GetBitwiseInstructionCount() override { return 0; }
    D3D_PRIMITIVE STDMETHODCALLTYPE GetGSInputPrimitive() override { return D3D_PRIMITIVE_UNDEFINED; }
    WINBOOL STDMETHODCALLTYPE IsSampleFrequencyShader() override { return FALSE; }
    UINT STDMETHODCALLTYPE GetNumInterfaceSlots() override { return 0; }
    HRESULT STDMETHODCALLTYPE GetMinFeatureLevel(D3D_FEATURE_LEVEL* level) override
    {
        if (!level)
            return E_INVALIDARG;
        *level = D3D_FEATURE_LEVEL_9_1;
        return S_OK;
    }
    UINT STDMETHODCALLTYPE GetThreadGroupSize(UINT* x, UINT* y, UINT* z) override
    {
        if (x) *x = 0;
        if (y) *y = 0;
        if (z) *z = 0;
        return 0;
    }
    UINT64 STDMETHODCALLTYPE GetRequiresFlags() override { return 0; }

private:
    static HRESULT getSignatureDesc(const std::vector<D3D11_SIGNATURE_PARAMETER_DESC>& signatures,
                                    UINT index, D3D11_SIGNATURE_PARAMETER_DESC* desc)
    {
        if (!desc)
            return E_INVALIDARG;
        if (index >= signatures.size())
            return E_FAIL;
        *desc = signatures[index];
        return S_OK;
    }

    bool initialize(const void* data, size_t size)
    {
        if (!data || !size)
            return false;

        dxbc_spv::dxbc::Container container(data, size);
        if (!container)
            return false;

        const auto rdef = container.findChunk(dxbc_spv::util::FourCC("RDEF"));
        if (!rdef || rdef.getSize() < RdefChunkHeaderSize + RdefHeaderSize)
            return false;

        m_rdef.assign(static_cast<const unsigned char*>(rdef.getData(0)),
                      static_cast<const unsigned char*>(rdef.getData(0)) + rdef.getSize());
        const RdefView view(dxbc_spv::util::ByteReader(m_rdef.data(), m_rdef.size()));

        uint32_t constantBufferCount = 0;
        uint32_t constantBufferOffset = 0;
        uint32_t resourceCount = 0;
        uint32_t resourceOffset = 0;
        uint32_t creatorOffset = 0;
        if (!view.dataU32(0, constantBufferCount)
            || !view.dataU32(4, constantBufferOffset)
            || !view.dataU32(8, resourceCount)
            || !view.dataU32(12, resourceOffset)
            || !view.dataU32(24, creatorOffset)
            || constantBufferCount > 4096
            || resourceCount > 4096
            || !view.rdefRange(constantBufferOffset, size_t(constantBufferCount) * RdefConstantBufferSize)
            || !view.rdefRange(resourceOffset, size_t(resourceCount) * RdefResourceSize))
            return false;

        m_desc = {};
        m_desc.Creator = view.stringAt(creatorOffset);
        m_desc.ConstantBuffers = constantBufferCount;
        m_desc.BoundResources = resourceCount;

        if (auto code = container.getCodeChunk(); code && code.getSize() >= sizeof(uint32_t))
            code.read(m_desc.Version);

        if (!parseResources(view, resourceCount, resourceOffset))
            return false;
        if (!parseConstantBuffers(view, constantBufferCount, constantBufferOffset))
            return false;
        if (!parseSignature(container.getInputSignatureChunk(), m_inputParameters)
            || !parseSignature(container.getOutputSignatureChunk(), m_outputParameters)
            || !parseSignature(container.getPatchConstantSignatureChunk(), m_patchParameters))
            return false;

        m_desc.InputParameters = static_cast<UINT>(m_inputParameters.size());
        m_desc.OutputParameters = static_cast<UINT>(m_outputParameters.size());
        m_desc.PatchConstantParameters = static_cast<UINT>(m_patchParameters.size());
        return true;
    }

    bool parseResources(const RdefView& view, uint32_t count, uint32_t offset)
    {
        m_resources.reserve(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t entry = offset + i * RdefResourceSize;
            uint32_t name = 0, type = 0, returnType = 0, dimension = 0;
            uint32_t samples = 0, bindPoint = 0, bindCount = 0, flags = 0;
            if (!view.rdefU32(entry + 0, name) || !view.rdefU32(entry + 4, type)
                || !view.rdefU32(entry + 8, returnType) || !view.rdefU32(entry + 12, dimension)
                || !view.rdefU32(entry + 16, samples) || !view.rdefU32(entry + 20, bindPoint)
                || !view.rdefU32(entry + 24, bindCount) || !view.rdefU32(entry + 28, flags)
                || !view.stringAt(name))
                return false;

            D3D11_SHADER_INPUT_BIND_DESC desc = {};
            desc.Name = view.stringAt(name);
            desc.Type = static_cast<D3D_SHADER_INPUT_TYPE>(type);
            desc.ReturnType = static_cast<D3D_RESOURCE_RETURN_TYPE>(returnType);
            desc.Dimension = static_cast<D3D_SRV_DIMENSION>(dimension);
            desc.NumSamples = samples;
            desc.BindPoint = bindPoint;
            desc.BindCount = bindCount;
            desc.uFlags = flags;
            m_resources.push_back(desc);
        }
        return true;
    }

    bool parseConstantBuffers(const RdefView& view, uint32_t count, uint32_t offset)
    {
        m_constantBuffers.reserve(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t entry = offset + i * RdefConstantBufferSize;
            uint32_t name = 0, variableCount = 0, variableOffset = 0, size = 0, flags = 0, type = 0;
            if (!view.rdefU32(entry + 0, name) || !view.rdefU32(entry + 4, variableCount)
                || !view.rdefU32(entry + 8, variableOffset) || !view.rdefU32(entry + 12, size)
                || !view.rdefU32(entry + 16, flags) || !view.rdefU32(entry + 20, type)
                || variableCount > 4096
                || !view.rdefRange(variableOffset, size_t(variableCount) * RdefVariableSize)
                || !view.stringAt(name))
                return false;

            D3D11_SHADER_BUFFER_DESC desc = {};
            desc.Name = view.stringAt(name);
            desc.Type = static_cast<D3D_CBUFFER_TYPE>(type);
            desc.Size = size;
            desc.uFlags = flags;
            auto buffer = std::make_unique<ReflectionConstantBuffer>(desc);

            for (uint32_t j = 0; j < variableCount; ++j)
            {
                const uint32_t variable = variableOffset + j * RdefVariableSize;
                uint32_t variableName = 0, startOffset = 0, variableSize = 0, variableFlags = 0;
                uint32_t typeOffset = 0, defaultValue = 0, startTexture = 0, textureSize = 0;
                uint32_t startSampler = 0, samplerSize = 0;
                if (!view.rdefU32(variable + 0, variableName) || !view.rdefU32(variable + 4, startOffset)
                    || !view.rdefU32(variable + 8, variableSize) || !view.rdefU32(variable + 12, variableFlags)
                    || !view.rdefU32(variable + 16, typeOffset) || !view.rdefU32(variable + 20, defaultValue)
                    || !view.rdefU32(variable + 24, startTexture) || !view.rdefU32(variable + 28, textureSize)
                    || !view.rdefU32(variable + 32, startSampler) || !view.rdefU32(variable + 36, samplerSize)
                    || !view.stringAt(variableName))
                    return false;

                D3D11_SHADER_TYPE_DESC typeDesc = {};
                if (!parseType(view, typeOffset, typeDesc))
                    return false;
                auto reflectedType = std::make_unique<ReflectionType>(typeDesc);
                ReflectionType* reflectedTypePtr = reflectedType.get();
                m_types.emplace_back(std::move(reflectedType));

                D3D11_SHADER_VARIABLE_DESC variableDesc = {};
                variableDesc.Name = view.stringAt(variableName);
                variableDesc.StartOffset = startOffset;
                variableDesc.Size = variableSize;
                variableDesc.uFlags = variableFlags;
                variableDesc.DefaultValue = defaultValue ? const_cast<void*>(view.bytesAt(defaultValue, variableSize)) : nullptr;
                variableDesc.StartTexture = startTexture;
                variableDesc.TextureSize = textureSize;
                variableDesc.StartSampler = startSampler;
                variableDesc.SamplerSize = samplerSize;
                buffer->addVariable(variableDesc, reflectedTypePtr);
            }
            m_constantBuffers.emplace_back(std::move(buffer));
        }
        return true;
    }

    static bool parseType(const RdefView& view, uint32_t offset, D3D11_SHADER_TYPE_DESC& desc)
    {
        uint16_t variableClass = 0, variableType = 0, rows = 0, columns = 0, elements = 0, members = 0;
        uint32_t memberOffset = 0, nameOffset = 0;
        if (!view.rdefU16(offset + 0, variableClass) || !view.rdefU16(offset + 2, variableType)
            || !view.rdefU16(offset + 4, rows) || !view.rdefU16(offset + 6, columns)
            || !view.rdefU16(offset + 8, elements) || !view.rdefU16(offset + 10, members)
            || !view.rdefU32(offset + 12, memberOffset) || !view.rdefU32(offset + 16, nameOffset))
            return false;

        desc.Class = static_cast<D3D_SHADER_VARIABLE_CLASS>(variableClass);
        desc.Type = static_cast<D3D_SHADER_VARIABLE_TYPE>(variableType);
        desc.Rows = rows;
        desc.Columns = columns;
        desc.Elements = elements;
        desc.Members = members;
        desc.Offset = memberOffset;
        desc.Name = nameOffset ? view.stringAt(nameOffset) : nullptr;
        return !nameOffset || desc.Name;
    }

    static bool parseSignature(const dxbc_spv::util::ByteReader& reader,
                               std::vector<D3D11_SIGNATURE_PARAMETER_DESC>& output)
    {
        if (!reader)
            return true; // Shader stages without this signature are valid.

        dxbc_spv::dxbc::Signature signature(reader);
        if (!signature)
            return false;

        for (const auto& entry : signature)
        {
            D3D11_SIGNATURE_PARAMETER_DESC desc = {};
            desc.SemanticName = entry.getSemanticName();
            desc.SemanticIndex = entry.getSemanticIndex();
            desc.Register = static_cast<UINT>(entry.getRegisterIndex());
            desc.SystemValueType = static_cast<D3D_NAME>(entry.getSystemValue());
            switch (entry.getScalarType())
            {
            case dxbc_spv::ir::ScalarType::eF32: desc.ComponentType = D3D_REGISTER_COMPONENT_FLOAT32; break;
            case dxbc_spv::ir::ScalarType::eI32: desc.ComponentType = D3D_REGISTER_COMPONENT_SINT32; break;
            case dxbc_spv::ir::ScalarType::eU32: desc.ComponentType = D3D_REGISTER_COMPONENT_UINT32; break;
            default: desc.ComponentType = D3D_REGISTER_COMPONENT_UNKNOWN; break;
            }
            desc.Mask = static_cast<BYTE>(static_cast<dxbc_spv::util::Flags<dxbc_spv::util::ComponentBit>::IntType>(entry.getComponentMask()));
            desc.ReadWriteMask = static_cast<BYTE>(static_cast<dxbc_spv::util::Flags<dxbc_spv::util::ComponentBit>::IntType>(entry.getUsedComponentMask()));
            desc.Stream = entry.getStreamIndex();
            desc.MinPrecision = D3D_MIN_PRECISION_DEFAULT;
            output.push_back(desc);
        }
        return true;
    }

    std::atomic<ULONG> m_refs { 1 };
    bool m_valid = false;
    std::vector<unsigned char> m_rdef;
    D3D11_SHADER_DESC m_desc = {};
    std::vector<std::unique_ptr<ReflectionType>> m_types;
    std::vector<std::unique_ptr<ReflectionConstantBuffer>> m_constantBuffers;
    std::vector<D3D11_SHADER_INPUT_BIND_DESC> m_resources;
    std::vector<D3D11_SIGNATURE_PARAMETER_DESC> m_inputParameters;
    std::vector<D3D11_SIGNATURE_PARAMETER_DESC> m_outputParameters;
    std::vector<D3D11_SIGNATURE_PARAMETER_DESC> m_patchParameters;
};
} // namespace

HRESULT CreateCachedDxbcReflection(const void* data, SIZE_T size, REFIID iid, void** reflector)
{
    if (!reflector || !data || !size)
        return E_INVALIDARG;
    *reflector = nullptr;
    if (iid != IID_ID3D11ShaderReflection)
        return E_NOINTERFACE;

    auto* result = new (std::nothrow) Reflection(data, size);
    if (!result)
        return E_OUTOFMEMORY;
    if (!result->valid())
    {
        delete result;
        return E_FAIL;
    }
    *reflector = static_cast<ID3DShaderReflection*>(result);
    return S_OK;
}

HRESULT CreateCachedDxbcInputSignatureBlob(const void* data, SIZE_T size, ID3DBlob** blob)
{
    if (!blob || !data || !size)
        return E_INVALIDARG;
    *blob = nullptr;

    dxbc_spv::dxbc::Container container(data, size);
    if (!container || !container.getInputSignatureChunk())
        return E_FAIL;

    // CreateInputLayout accepts the complete vertex DXBC container. Returning
    // it preserves the exact cached ISGN/ISG1 payload and lets DXVK Native use
    // its own container parser rather than a fabricated signature blob.
    auto* result = new (std::nothrow) Blob(data, size);
    if (!result)
        return E_OUTOFMEMORY;
    *blob = result;
    return S_OK;
}

HRESULT CreateD3DCompilerBlob(SIZE_T size, ID3DBlob** blob)
{
    if (!blob)
        return E_INVALIDARG;
    *blob = nullptr;
    auto* result = new (std::nothrow) Blob(size);
    if (!result)
        return E_OUTOFMEMORY;
    *blob = result;
    return S_OK;
}

#endif // !defined(_WIN32)
