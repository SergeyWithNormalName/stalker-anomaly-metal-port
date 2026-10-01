#include "MetalR4DetailGrassAdapter.h"
#include "MetalR4EngineFs.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <limits>
#include <unordered_map>
#include <utility>

namespace
{
constexpr std::uint32_t kDetailVersion3 = 3u;
constexpr std::uint32_t kDetailVersion4 = 4u;
constexpr std::uint16_t kEmptyModelId = 0x3fffu;
constexpr std::uint32_t kChunkCompressed = 0x80000000u;
constexpr std::size_t kHeaderBytes = 24u;
constexpr std::size_t kVertexBytes = 20u;
constexpr std::size_t kV3SlotBytes = 16u;
constexpr std::size_t kV4SlotBytes = 20u;
constexpr std::size_t kMaxModels = 16383u;
constexpr std::size_t kMaxVerticesPerModel = 1u << 20u;
constexpr std::size_t kMaxIndicesPerModel = 3u << 20u;
constexpr std::size_t kVertexGlobalsSize = 6272u;
constexpr std::size_t kFragmentGlobalsSize = 224u;
constexpr std::size_t kTransformsSize = 256u;
constexpr std::size_t kStaticGlobalsSize = 528u;

struct Matrix4
{
    float elements[16]{};
};

Matrix4 multiply(const Matrix4& left, const Matrix4& right)
{
    Matrix4 result{};
    for (std::size_t column = 0; column != 4u; ++column)
        for (std::size_t row = 0; row != 4u; ++row)
            for (std::size_t index = 0; index != 4u; ++index)
                result.elements[column * 4u + row] += left.elements[index * 4u + row] *
                    right.elements[column * 4u + index];
    return result;
}

Matrix4 inverseRigidTransform(const Matrix4& value)
{
    Matrix4 result{};
    result.elements[0] = value.elements[0]; result.elements[1] = value.elements[4];
    result.elements[2] = value.elements[8]; result.elements[4] = value.elements[1];
    result.elements[5] = value.elements[5]; result.elements[6] = value.elements[9];
    result.elements[8] = value.elements[2]; result.elements[9] = value.elements[6];
    result.elements[10] = value.elements[10]; result.elements[15] = 1.0f;
    const float x = value.elements[12];
    const float y = value.elements[13];
    const float z = value.elements[14];
    result.elements[12] = -(x * result.elements[0] + y * result.elements[4] + z * result.elements[8]);
    result.elements[13] = -(x * result.elements[1] + y * result.elements[5] + z * result.elements[9]);
    result.elements[14] = -(x * result.elements[2] + y * result.elements[6] + z * result.elements[10]);
    return result;
}

void putFloat4(std::vector<std::uint8_t>& bytes, const std::size_t offset,
    const float x, const float y, const float z, const float w)
{
    const float value[4] = {x, y, z, w};
    std::memcpy(bytes.data() + offset, value, sizeof(value));
}

bool isOutsideMetalClipVolume(const R4DetailGrassBatch& batch, const Matrix4& matrix)
{
    if (!batch.hasBounds)
        return false;
    bool left = true, right = true, bottom = true, top = true, nearPlane = true, farPlane = true;
    for (std::uint32_t mask = 0u; mask != 8u; ++mask)
    {
        const float x = (mask & 1u) ? batch.boundsMaximum[0] : batch.boundsMinimum[0];
        const float y = (mask & 2u) ? batch.boundsMaximum[1] : batch.boundsMinimum[1];
        const float z = (mask & 4u) ? batch.boundsMaximum[2] : batch.boundsMinimum[2];
        const float clipX = x * matrix.elements[0] + y * matrix.elements[4] + z * matrix.elements[8] + matrix.elements[12];
        const float clipY = x * matrix.elements[1] + y * matrix.elements[5] + z * matrix.elements[9] + matrix.elements[13];
        const float clipZ = x * matrix.elements[2] + y * matrix.elements[6] + z * matrix.elements[10] + matrix.elements[14];
        const float clipW = x * matrix.elements[3] + y * matrix.elements[7] + z * matrix.elements[11] + matrix.elements[15];
        if (!std::isfinite(clipX) || !std::isfinite(clipY) || !std::isfinite(clipZ) || !std::isfinite(clipW))
            return false;
        left &= clipX < -clipW; right &= clipX > clipW;
        bottom &= clipY < -clipW; top &= clipY > clipW;
        nearPlane &= clipZ < 0.0f; farPlane &= clipZ > clipW;
    }
    return left || right || bottom || top || nearPlane || farPlane;
}

struct ByteReader
{
    const std::vector<std::uint8_t>& bytes;
    std::size_t cursor = 0;
    std::size_t end = 0;

    bool remaining(const std::size_t count) const
    {
        return cursor <= end && count <= end - cursor;
    }

    bool u32(std::uint32_t& value)
    {
        if (!remaining(sizeof(value)))
            return false;
        std::memcpy(&value, bytes.data() + cursor, sizeof(value));
        cursor += sizeof(value);
        return true;
    }

    bool f32(float& value)
    {
        if (!remaining(sizeof(value)))
            return false;
        std::memcpy(&value, bytes.data() + cursor, sizeof(value));
        cursor += sizeof(value);
        return true;
    }

    bool u16(std::uint16_t& value)
    {
        if (!remaining(sizeof(value)))
            return false;
        std::memcpy(&value, bytes.data() + cursor, sizeof(value));
        cursor += sizeof(value);
        return true;
    }

    bool zstring(std::string& value)
    {
        value.clear();
        while (cursor < end)
        {
            const char valueByte = static_cast<char>(bytes[cursor++]);
            if (valueByte == '\0')
                return !value.empty() && value.size() <= 1024u;
            value.push_back(valueByte);
            if (value.size() > 1024u)
                return false;
        }
        return false;
    }
};

struct Chunk
{
    std::size_t begin = 0;
    std::size_t end = 0;
};

bool findChunk(const std::vector<std::uint8_t>& bytes, const std::size_t begin,
    const std::size_t end, const std::uint32_t target, Chunk& result)
{
    std::size_t cursor = begin;
    while (cursor < end)
    {
        if (end - cursor < 8u)
            return false;
        std::uint32_t id = 0;
        std::uint32_t size = 0;
        std::memcpy(&id, bytes.data() + cursor, sizeof(id));
        std::memcpy(&size, bytes.data() + cursor + sizeof(id), sizeof(size));
        cursor += 8u;
        if ((size & kChunkCompressed) != 0u)
            return false;
        const std::size_t payload = size;
        if (payload > end - cursor)
            return false;
        if (id == target)
        {
            result = {cursor, cursor + payload};
            return true;
        }
        cursor += payload;
    }
    return false;
}

bool finite3(const std::array<float, 3>& value)
{
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}

float detailGrassShadowFadeScale(const R4DetailGrassInstance& instance,
    const R4DetailGrassShadowState& shadow)
{
    if (shadow.fadeFromLight)
    {
        const float dx = instance.shadowFadePosition[0] - shadow.lightPosition[0];
        const float dy = instance.shadowFadePosition[1] - shadow.lightPosition[1];
        const float dz = instance.shadowFadePosition[2] - shadow.lightPosition[2];
        const float lightDistanceSquared = dx * dx + dy * dy + dz * dz;
        const float lightRangeSquared = shadow.lightRange * shadow.lightRange;
        if (lightDistanceSquared >= lightRangeSquared)
            return 0.0f;

        const float dxzSquared = dx * dx + dz * dz;
        return 1.0f - dxzSquared * 0.005f;
    }
    if (instance.shadowDistanceSquared > shadow.fadeDistance)
        return 1.0f - std::abs(instance.shadowDistanceSquared - shadow.fadeDistance) * 0.005f;
    return 1.0f;
}

bool readText(const std::filesystem::path& path, std::string& value)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    value.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return input.good() || input.eof();
}

std::string compactJson(const std::string& input)
{
    std::string result;
    result.reserve(input.size());
    bool quoted = false;
    bool escaped = false;
    for (const char value : input)
    {
        if (quoted)
        {
            result.push_back(value);
            if (!escaped && value == '"')
                quoted = false;
            escaped = !escaped && value == '\\';
        }
        else if (value == '"')
        {
            quoted = true;
            result.push_back(value);
        }
        else if (!std::isspace(static_cast<unsigned char>(value)))
            result.push_back(value);
    }
    return result;
}

bool validateMscBundle(const MetalR4DetailGrassAdapter::Options& options, std::string& error)
{
    if (options.vertexMetallibPath.filename() != "deffer_grass.vs.metallib" ||
        options.fragmentMetallibPath.filename() != "deffer_grass.ps.metallib" ||
        options.vertexMetallibPath.parent_path() != options.fragmentMetallibPath.parent_path())
    {
        error = "detail grass requires sibling deffer_grass VS/PS MSC artifacts";
        return false;
    }
    std::filesystem::path vsReflection = options.vertexMetallibPath;
    std::filesystem::path psReflection = options.fragmentMetallibPath;
    vsReflection.replace_extension(".json");
    psReflection.replace_extension(".json");
    std::string vs;
    std::string ps;
    if (!std::filesystem::is_regular_file(options.vertexMetallibPath) ||
        !std::filesystem::is_regular_file(options.fragmentMetallibPath) ||
        !readText(vsReflection, vs) || !readText(psReflection, ps))
    {
        error = "detail grass MSC artifacts or adjacent reflection JSON are unavailable";
        return false;
    }
    vs = compactJson(vs);
    ps = compactJson(ps);
    static constexpr const char* VertexInputs =
        "\"vertex_inputs\":[{\"columnCount\":4,\"elementType\":\"Float\",\"index\":0,\"name\":\"position0\"},{\"columnCount\":4,\"elementType\":\"Int32\",\"index\":1,\"name\":\"texcoord0\"}]";
    static constexpr const char* VertexTlab =
        "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":48,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":72,\"Name\":\"\",\"Size\":24,\"Slot\":2,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":96,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"Sampler\"}]";
    static constexpr const char* FragmentTlab =
        "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":48,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":72,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":96,\"Name\":\"\",\"Size\":24,\"Slot\":2,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":120,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"Sampler\"}]";
    if (vs.find("\"ShaderType\":\"Vertex\"") == std::string::npos ||
        vs.find("\"ResourceCount\":5") == std::string::npos ||
        vs.find(VertexInputs) == std::string::npos || vs.find(VertexTlab) == std::string::npos ||
        ps.find("\"ShaderType\":\"Fragment\"") == std::string::npos ||
        ps.find("\"ResourceCount\":6") == std::string::npos ||
        ps.find(FragmentTlab) == std::string::npos ||
        ps.find("\"num_render_targets\":4") == std::string::npos ||
        ps.find("\"discards\":true") == std::string::npos)
    {
        error = "detail grass MSC reflection differs from the recorded details\\\\blend contract";
        return false;
    }
    return true;
}

bool validateShadowMscBundle(const std::filesystem::path& directory, std::string& error)
{
    const std::filesystem::path vsPath = directory / "deffer_grass_shadow.vs.metallib";
    const std::filesystem::path psPath = directory / "deffer_grass_shadow.ps.metallib";
    if (!std::filesystem::is_regular_file(vsPath) || !std::filesystem::is_regular_file(psPath))
    {
        error = "detail grass shadow bundle is missing its dedicated VS or R32 PS wrapper";
        return false;
    }
    std::filesystem::path vsReflection = vsPath;
    std::filesystem::path psReflection = psPath;
    vsReflection.replace_extension(".reflection.json");
    psReflection.replace_extension(".reflection.json");
    std::string vs;
    std::string ps;
    if (!readText(vsReflection, vs) || !readText(psReflection, ps))
    {
        error = "detail grass shadow reflection receipt is missing";
        return false;
    }
    vs = compactJson(vs);
    ps = compactJson(ps);
    static constexpr const char* VertexTlab =
        "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":48,\"Name\":\"\",\"Size\":24,\"Slot\":1,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":72,\"Name\":\"\",\"Size\":24,\"Slot\":2,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":96,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"Sampler\"}]";
    static constexpr const char* FragmentTlab =
        "\"TopLevelArgumentBuffer\":[{\"EltOffset\":0,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"SRV\"},{\"EltOffset\":24,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"CBV\"},{\"EltOffset\":48,\"Name\":\"\",\"Size\":24,\"Slot\":0,\"Space\":0,\"Type\":\"Sampler\"}]";
    if (vs.find("\"ShaderType\":\"Vertex\"") == std::string::npos ||
        vs.find("\"ResourceCount\":5") == std::string::npos ||
        vs.find(VertexTlab) == std::string::npos ||
        ps.find("\"ShaderType\":\"Fragment\"") == std::string::npos ||
        ps.find("\"ResourceCount\":3") == std::string::npos ||
        ps.find(FragmentTlab) == std::string::npos ||
        ps.find("\"discards\":true") == std::string::npos ||
        ps.find("\"num_render_targets\":1") == std::string::npos)
    {
        error = "detail grass shadow reflection differs from the production wind/alpha wrapper ABI";
        return false;
    }
    return true;
}

bool uploadDds(IR4Resources& resources, xray::metal::R4DdsTexture& dds,
    const char* label, std::shared_ptr<IR4Texture>& texture, std::string& error)
{
    dds.descriptor.label = label;
    dds.descriptor.storage = R4ResourceStorage::Shared;
    texture = resources.CreateTexture2D(dds.descriptor, error);
    if (!texture)
        return false;
    for (std::size_t level = 0; level < dds.mipLevels.size(); ++level)
    {
        const xray::metal::R4DdsMipLevel& mip = dds.mipLevels[level];
        if (mip.dataOffset > dds.bytes.size() || mip.bytesPerImage > dds.bytes.size() - mip.dataOffset ||
            !resources.UploadTexture2D(texture, static_cast<std::uint32_t>(level), 0,
                dds.bytes.data() + mip.dataOffset, mip.bytesPerImage, mip.bytesPerRow,
                mip.bytesPerImage, error))
            return false;
    }
    return true;
}

std::int32_t quantize(const float value, bool& valid)
{
    const float scaled = value * 16384.0f;
    if (!std::isfinite(scaled) || scaled < -32768.0f || scaled > 32767.0f)
    {
        valid = false;
        return 0;
    }
    return static_cast<std::int32_t>(std::floor(scaled));
}
} // namespace

bool LoadR4DetailGrassSnapshot(const std::vector<std::uint8_t>& bytes,
    R4DetailGrassSnapshot& snapshot, std::string& error)
{
    snapshot = {};
    if (bytes.size() < 8u)
    {
        error = "level.details has no top-level chunk header";
        return false;
    }
    Chunk headerChunk;
    Chunk modelsChunk;
    Chunk slotsChunk;
    if (!findChunk(bytes, 0, bytes.size(), 0u, headerChunk) ||
        !findChunk(bytes, 0, bytes.size(), 1u, modelsChunk) ||
        !findChunk(bytes, 0, bytes.size(), 2u, slotsChunk) ||
        headerChunk.end - headerChunk.begin != kHeaderBytes)
    {
        error = "level.details is missing a valid header, models, or slots chunk";
        return false;
    }
    ByteReader header{bytes, headerChunk.begin, headerChunk.end};
    std::uint32_t modelCount = 0;
    std::uint32_t rawOffsetX = 0;
    std::uint32_t rawOffsetZ = 0;
    if (!header.u32(snapshot.version) || !header.u32(modelCount) || !header.u32(rawOffsetX) ||
        !header.u32(rawOffsetZ) || !header.u32(snapshot.sizeX) || !header.u32(snapshot.sizeZ) ||
        (snapshot.version != kDetailVersion3 && snapshot.version != kDetailVersion4) ||
        modelCount == 0u || modelCount > kMaxModels || snapshot.sizeX == 0u || snapshot.sizeZ == 0u)
    {
        error = "level.details header has an unsupported version or invalid dimensions";
        return false;
    }
    snapshot.offsetX = static_cast<std::int32_t>(rawOffsetX);
    snapshot.offsetZ = static_cast<std::int32_t>(rawOffsetZ);
    if (snapshot.sizeX > std::numeric_limits<std::size_t>::max() / snapshot.sizeZ)
    {
        error = "level.details slot dimensions overflow";
        return false;
    }

    snapshot.models.reserve(modelCount);
    for (std::uint32_t modelId = 0; modelId < modelCount; ++modelId)
    {
        Chunk modelChunk;
        if (!findChunk(bytes, modelsChunk.begin, modelsChunk.end, modelId, modelChunk))
        {
            error = "level.details models chunk has a missing model record";
            return false;
        }
        ByteReader reader{bytes, modelChunk.begin, modelChunk.end};
        R4DetailGrassModel model;
        std::uint32_t vertexCount = 0;
        std::uint32_t indexCount = 0;
        const bool headerRead = reader.zstring(model.shaderName) &&
            reader.zstring(model.textureName) && reader.u32(model.flags) &&
            reader.f32(model.minimumScale) && reader.f32(model.maximumScale) &&
            reader.u32(vertexCount) && reader.u32(indexCount);
        if (!headerRead ||
            !std::isfinite(model.minimumScale) || !std::isfinite(model.maximumScale) ||
            model.minimumScale <= 0.0f || model.maximumScale <= 0.0f ||
            vertexCount == 0u || vertexCount > kMaxVerticesPerModel || indexCount == 0u ||
            indexCount > kMaxIndicesPerModel || indexCount % 3u != 0u ||
            vertexCount > std::numeric_limits<std::uint16_t>::max())
        {
            char details[512];
            std::snprintf(details, sizeof(details),
                "level.details model %u header is invalid (read=%s shader=%s texture=%s "
                "min=%.6g max=%.6g vertices=%u indices=%u bytes_remaining=%zu)",
                modelId, headerRead ? "true" : "false", model.shaderName.c_str(),
                model.textureName.c_str(), model.minimumScale, model.maximumScale,
                vertexCount, indexCount, reader.end - reader.cursor);
            error = details;
            return false;
        }
        if (vertexCount > (reader.end - reader.cursor) / kVertexBytes)
        {
            error = "level.details model vertex bytes are truncated";
            return false;
        }
        model.vertices.resize(vertexCount);
        model.boundsMinimum.fill(std::numeric_limits<float>::infinity());
        model.boundsMaximum.fill(-std::numeric_limits<float>::infinity());
        for (R4DetailGrassModel::Vertex& vertex : model.vertices)
        {
            if (!reader.f32(vertex.position[0]) || !reader.f32(vertex.position[1]) ||
                !reader.f32(vertex.position[2]) || !reader.f32(vertex.u) || !reader.f32(vertex.v) ||
                !finite3(vertex.position) || !std::isfinite(vertex.u) || !std::isfinite(vertex.v))
            {
                error = "level.details model contains a non-finite vertex";
                return false;
            }
            for (std::size_t axis = 0; axis < 3u; ++axis)
            {
                model.boundsMinimum[axis] = std::min(model.boundsMinimum[axis], vertex.position[axis]);
                model.boundsMaximum[axis] = std::max(model.boundsMaximum[axis], vertex.position[axis]);
            }
        }
        if (indexCount > (reader.end - reader.cursor) / sizeof(std::uint16_t))
        {
            error = "level.details model index bytes are truncated";
            return false;
        }
        model.indices.resize(indexCount);
        for (std::uint16_t& index : model.indices)
        {
            if (!reader.u16(index) || index >= vertexCount)
            {
                error = "level.details model index is not local to its model";
                return false;
            }
        }
        if (reader.cursor != reader.end)
        {
            error = "level.details model record has trailing bytes";
            return false;
        }
        snapshot.models.emplace_back(std::move(model));
    }

    const std::size_t slotCount = static_cast<std::size_t>(snapshot.sizeX) * snapshot.sizeZ;
    const std::size_t slotSize = snapshot.version == kDetailVersion4 ? kV4SlotBytes : kV3SlotBytes;
    if (slotCount > (slotsChunk.end - slotsChunk.begin) / slotSize ||
        slotsChunk.end - slotsChunk.begin != slotCount * slotSize)
    {
        error = "level.details slots payload has an invalid byte size";
        return false;
    }
    ByteReader slots{bytes, slotsChunk.begin, slotsChunk.end};
    snapshot.slots.resize(slotCount);
    for (R4DetailGrassSlot& slot : snapshot.slots)
    {
        std::uint32_t word0 = 0;
        std::uint32_t word1 = 0;
        std::uint32_t word2 = 0;
        if (!slots.u32(word0) || !slots.u32(word1) ||
            (snapshot.version == kDetailVersion4 && !slots.u32(word2)))
        {
            error = "level.details slots payload is truncated";
            return false;
        }
        if (snapshot.version == kDetailVersion4)
        {
            slot.modelIds[0] = static_cast<std::uint16_t>(word0 & 0x3fffu);
            slot.modelIds[1] = static_cast<std::uint16_t>((word0 >> 14u) & 0x3fffu);
            slot.modelIds[2] = static_cast<std::uint16_t>(word1 & 0x3fffu);
            slot.modelIds[3] = static_cast<std::uint16_t>((word1 >> 14u) & 0x3fffu);
            slot.sun = float((word0 >> 28u) & 0x0fu) / 15.0f;
            slot.hemi = float((word1 >> 28u) & 0x0fu) / 15.0f;
            slot.yBase = float(word2 & 0x0fffu) * 0.2f - 200.0f;
            slot.yHeight = float((word2 >> 12u) & 0xffu) * 0.1f;
        }
        else
        {
            slot.modelIds[0] = static_cast<std::uint16_t>((word0 >> 20u) & 0x3fu);
            slot.modelIds[1] = static_cast<std::uint16_t>((word0 >> 26u) & 0x3fu);
            slot.modelIds[2] = static_cast<std::uint16_t>(word1 & 0x3fu);
            slot.modelIds[3] = static_cast<std::uint16_t>((word1 >> 6u) & 0x3fu);
            slot.sun = float((word1 >> 12u) & 0x0fu) / 15.0f;
            slot.hemi = float((word1 >> 16u) & 0x0fu) / 15.0f;
            slot.yBase = float(word0 & 0x0fffu) * 0.2f - 200.0f;
            slot.yHeight = float((word0 >> 12u) & 0xffu) * 0.1f;
            for (std::uint16_t& id : slot.modelIds)
                if (id == 0x3fu)
                    id = kEmptyModelId;
        }
        for (std::array<std::uint8_t, 4>& palette : slot.palette)
        {
            std::uint16_t packed = 0;
            if (!slots.u16(packed))
            {
                error = "level.details slot palette is truncated";
                return false;
            }
            for (std::size_t corner = 0; corner < palette.size(); ++corner)
                palette[corner] = static_cast<std::uint8_t>((packed >> (corner * 4u)) & 0x0fu);
        }
        for (const std::uint16_t id : slot.modelIds)
        {
            if (id != kEmptyModelId && id >= snapshot.models.size())
            {
                error = "level.details slot references a missing model";
                return false;
            }
        }
    }
    error.clear();
    return true;
}

bool RepackR4DetailGrassBatch(const R4DetailGrassModel& model,
    const std::vector<R4DetailGrassInstance>& instances,
    std::vector<R4DetailGrassVertex>& vertices,
    std::vector<std::uint16_t>& indices,
    std::string& error)
{
    vertices.clear();
    indices.clear();
    if (model.shaderName != "details\\blend" || model.vertices.empty() || model.indices.empty() ||
        model.indices.size() % 3u != 0u || instances.empty() || instances.size() > kR4DetailGrassBatchSize ||
        model.vertices.size() > std::numeric_limits<std::uint16_t>::max() ||
        model.vertices.size() > std::numeric_limits<std::uint16_t>::max() / instances.size())
    {
        error = "detail grass batch is outside the details\\\\blend 61-instance uint16 contract";
        return false;
    }
    const float height = model.boundsMaximum[1] - model.boundsMinimum[1];
    if (!std::isfinite(height) || height <= std::numeric_limits<float>::epsilon())
    {
        error = "detail grass model has a degenerate vertical extent";
        return false;
    }
    for (const std::uint16_t index : model.indices)
    {
        if (index >= model.vertices.size())
        {
            error = "detail grass model index is not local to its model";
            return false;
        }
    }
    vertices.reserve(model.vertices.size() * instances.size());
    indices.reserve(model.indices.size() * instances.size());
    for (std::size_t instanceIndex = 0; instanceIndex < instances.size(); ++instanceIndex)
    {
        const R4DetailGrassInstance& instance = instances[instanceIndex];
        if (!finite3(instance.terrainNormal) || !std::isfinite(instance.sun) || !std::isfinite(instance.hemi) ||
            !std::isfinite(instance.alpha) || !instance.hasShadowFadePosition ||
            !finite3(instance.shadowFadePosition) || !std::isfinite(instance.shadowDistanceSquared) ||
            !std::all_of(instance.transform.begin(), instance.transform.end(), [](const float value) {
                return std::isfinite(value);
            }))
        {
            error = "detail grass instance has invalid transform, lighting, terrain normal, or source shadow-fade position";
            return false;
        }
        const std::size_t vertexBase = vertices.size();
        for (const R4DetailGrassModel::Vertex& source : model.vertices)
        {
            bool valid = true;
            const std::int32_t u = quantize(source.u, valid);
            const std::int32_t v = quantize(source.v, valid);
            const std::int32_t t = quantize(source.position[1] / height, valid);
            if (!valid)
            {
                error = "detail grass UV or vertical blend cannot be represented as signed SHORT";
                return false;
            }
            R4DetailGrassVertex destination{};
            destination.position[0] = source.position[0];
            destination.position[1] = source.position[1];
            destination.position[2] = source.position[2];
            destination.position[3] = 1.0f;
            destination.misc[0] = u;
            destination.misc[1] = v;
            destination.misc[2] = t;
            destination.misc[3] = static_cast<std::int32_t>(instanceIndex * 4u);
            vertices.emplace_back(destination);
        }
        for (const std::uint16_t source : model.indices)
            indices.emplace_back(static_cast<std::uint16_t>(vertexBase + source));
    }
    error.clear();
    return true;
}

struct MetalR4DetailGrassAdapter::Impl
{
    struct Draw
    {
        std::shared_ptr<IR4Buffer> vertices;
        std::shared_ptr<IR4Buffer> indices;
        std::shared_ptr<IR4Texture> baseTexture;
        std::shared_ptr<IR4Texture> bumpTexture;
        std::size_t indexCount = 0;
        std::vector<R4DetailGrassInstance> instances;
        R4DetailGrassBatch bounds;
    };
    std::shared_ptr<IR4GraphicsPipeline> pipeline;
    std::shared_ptr<IR4GraphicsPipeline> shadowPipeline;
    std::string shadowError;
    std::shared_ptr<IR4Sampler> baseSampler;
    std::shared_ptr<IR4Sampler> wavesSampler;
    std::shared_ptr<IR4Texture> wavesTexture;
    R4DetailGrassSnapshot snapshot;
    Options options;
    std::vector<Draw> draws;
    std::uint64_t stagedGeneration = UINT64_MAX;
    float cacheFadeDistance = 0.0f;
    std::uint32_t lastMotionTelemetryFrame = UINT32_MAX;
    std::unordered_map<std::string, std::shared_ptr<IR4Texture>> textures;
};

MetalR4DetailGrassAdapter::MetalR4DetailGrassAdapter() = default;
MetalR4DetailGrassAdapter::~MetalR4DetailGrassAdapter() = default;

bool MetalR4DetailGrassAdapter::Configure(IR4Backend& backend, const Options& options, std::string& status)
{
    m_ready = false;
    m_status.clear();
    m_impl = std::make_unique<Impl>();
    m_impl->options = options;
    std::string error;
    if (!options.ddsBytesProvider || options.wavesTexturePath.empty())
    {
        m_status = "detail grass adapter needs an engine XRFS provider and fx\\\\wind_wave DDS";
        status = m_status;
        return true;
    }
    if (!validateMscBundle(options, error))
    {
        m_status = error;
        status = m_status;
        return true;
    }
    R4GraphicsPipelineDesc pipeline;
    pipeline.label = "R4 details blend MSC G-buffer";
    pipeline.shaderKey = "r4-details-blend-msc-v1";
    const std::string vsPath = options.vertexMetallibPath.string();
    const std::string psPath = options.fragmentMetallibPath.string();
    pipeline.vertexMetallibPath = vsPath.c_str();
    pipeline.fragmentMetallibPath = psPath.c_str();
    pipeline.vertexAttributes = {{11u, 6u, 0u, R4VertexFormat::Float4},
        {12u, 6u, 16u, R4VertexFormat::Int4}};
    pipeline.vertexLayouts = {{6u, sizeof(R4DetailGrassVertex)}};
    pipeline.colorFormats = {R4PixelFormat::RGBA16Float, R4PixelFormat::RGBA8Unorm,
        R4PixelFormat::RGBA8Unorm, R4PixelFormat::RGBA16Float};
    pipeline.depthFormat = R4PixelFormat::Depth32FloatStencil8;
    pipeline.depthCompare = R4CompareFunction::Less;
    pipeline.depthWrite = true;
    pipeline.cullMode = R4CullMode::None;
    pipeline.stencil.enabled = true;
    pipeline.stencil.frontFace.depthStencilPass = R4StencilOperation::Replace;
    pipeline.stencil.frontFace.writeMask = 0x7fu;
    pipeline.stencil.backFace = pipeline.stencil.frontFace;
    m_impl->pipeline = backend.CreateGraphicsPipeline(pipeline, error);
    if (!m_impl->pipeline)
    {
        m_status = "detail grass MSC pipeline unavailable: " + error;
        status = m_status;
        return true;
    }
    R4SamplerDesc sampler;
    sampler.label = "R4 details blend base sampler";
    sampler.maxAnisotropy = 8u;
    m_impl->baseSampler = backend.Resources().CreateSampler(sampler, error);
    sampler.label = "R4 details blend waves sampler";
    m_impl->wavesSampler = backend.Resources().CreateSampler(sampler, error);
    if (!m_impl->baseSampler || !m_impl->wavesSampler)
    {
        m_status = "detail grass MSC sampler unavailable: " + error;
        status = m_status;
        return true;
    }
    xray::metal::R4DdsTexture waves;
    if (!xray::metal::LoadR4DdsTexture(options.wavesTexturePath, options.ddsBytesProvider, waves, error) ||
        !uploadDds(backend.Resources(), waves, "R4 details blend wind wave DDS", m_impl->wavesTexture, error))
    {
        m_status = "detail grass wind-wave texture unavailable: " + error;
        status = m_status;
        return true;
    }
    if (!options.shadowShaderDirectory.empty())
    {
        if (!validateShadowMscBundle(options.shadowShaderDirectory, m_impl->shadowError))
        {
            std::fprintf(stderr, "METAL_DETAIL_GRASS_SHADOW_UNAVAILABLE reason=\"%s\"\n",
                m_impl->shadowError.c_str());
        }
        else
        {
            R4GraphicsPipelineDesc shadow;
            shadow.label = "R4 details blend shadow alpha/dither";
            shadow.shaderKey = "r4-details-blend-shadow-alpha-dither-v1";
            const std::string shadowVs =
                (options.shadowShaderDirectory / "deffer_grass_shadow.vs.metallib").string();
            const std::string shadowPs =
                (options.shadowShaderDirectory / "deffer_grass_shadow.ps.metallib").string();
            shadow.vertexMetallibPath = shadowVs.c_str();
            shadow.fragmentMetallibPath = shadowPs.c_str();
            shadow.vertexAttributes = {{11u, 6u, 0u, R4VertexFormat::Float4},
                {12u, 6u, 16u, R4VertexFormat::Int4}};
            shadow.vertexLayouts = {{6u, sizeof(R4DetailGrassVertex)}};
            shadow.colorFormats = {R4PixelFormat::R32Float};
            shadow.depthFormat = R4PixelFormat::Depth32Float;
            shadow.depthCompare = R4CompareFunction::Less;
            shadow.depthWrite = true;
            shadow.cullMode = R4CullMode::None;
            shadow.frontFace = R4Winding::Clockwise;
            m_impl->shadowPipeline = backend.CreateGraphicsPipeline(shadow, error);
            if (!m_impl->shadowPipeline)
            {
                m_impl->shadowError = "detail grass shadow PSO creation failed: " + error;
                std::fprintf(stderr, "METAL_DETAIL_GRASS_SHADOW_UNAVAILABLE reason=\"%s\"\n",
                    m_impl->shadowError.c_str());
            }
        }
    }
    m_ready = true;
    m_status = "details blend MSC foundation ready";
    status = m_status;
    return true;
}

bool MetalR4DetailGrassAdapter::SetSnapshot(const R4DetailGrassSnapshot& snapshot, std::string& error)
{
    if (!m_impl)
    {
        error = "detail grass adapter must be configured before it accepts a snapshot";
        return false;
    }
    R4DetailGrassSnapshot filtered = snapshot;
    filtered.models.clear();
    std::vector<std::uint16_t> modelRemap(snapshot.models.size(), kEmptyModelId);
    for (std::size_t index = 0; index < snapshot.models.size(); ++index)
    {
        const R4DetailGrassModel& model = snapshot.models[index];
        if (model.shaderName == "details\\blend")
        {
            if (filtered.models.size() >= kEmptyModelId)
            {
                error = "details\\\\blend model count exceeds the level.details ID range";
                return false;
            }
            modelRemap[index] = static_cast<std::uint16_t>(filtered.models.size());
            filtered.models.push_back(model);
        }
    }
    if (filtered.models.empty())
    {
        error = "level.details contains no details\\\\blend model";
        return false;
    }
    for (R4DetailGrassSlot& slot : filtered.slots)
    {
        for (std::uint16_t& modelId : slot.modelIds)
        {
            if (modelId == kEmptyModelId)
                continue;
            if (modelId >= modelRemap.size())
            {
                error = "detail grass snapshot slot has an invalid pre-filter model ID";
                return false;
            }
            modelId = modelRemap[modelId];
        }
    }
    m_impl->snapshot = std::move(filtered);
    m_telemetry.models = m_impl->snapshot.models.size();
    m_telemetry.slots = m_impl->snapshot.slots.size();
    error.clear();
    return true;
}

const R4DetailGrassSnapshot* MetalR4DetailGrassAdapter::Snapshot() const
{
    return m_impl ? &m_impl->snapshot : nullptr;
}

bool MetalR4DetailGrassAdapter::StageFrame(IR4Backend& backend,
    const R4DetailGrassFrame& frame, std::string& error)
{
    if (!m_ready || !m_impl)
        return true;
    if (frame.generation == m_impl->stagedGeneration && !m_impl->draws.empty())
        return true;
    m_impl->draws.clear();
    m_impl->stagedGeneration = frame.generation;
    m_impl->cacheFadeDistance = frame.cacheFadeDistance;
    m_telemetry.requestedInstances = frame.requestedInstances;
    m_telemetry.admittedInstances = 0u;
    m_telemetry.rejectedBatches = 0u;
    m_telemetry.textureRejectedBatches = 0u;
    m_telemetry.shadowDraws = 0u;
    m_telemetry.shadowCulledBatches = 0u;
    m_telemetry.shadowRejected = 0u;
    m_telemetry.distanceRejectedSlots = frame.distanceRejectedSlots;
    m_telemetry.stillInstances = frame.stillInstances;
    m_telemetry.cacheFadeDistance = frame.cacheFadeDistance;
    IR4Resources& resources = backend.Resources();
    for (const R4DetailGrassBatch& batch : frame.batches)
    {
        std::string localError;
        if (batch.modelId >= m_impl->snapshot.models.size() || batch.instances.empty() ||
            batch.instances.size() > kR4DetailGrassBatchSize)
            localError = "invalid filtered model ID or batch size";
        const R4DetailGrassModel* model = localError.empty() ?
            &m_impl->snapshot.models[batch.modelId] : nullptr;
        std::vector<R4DetailGrassVertex> vertices;
        std::vector<std::uint16_t> indices;
        if (localError.empty() && !RepackR4DetailGrassBatch(*model, batch.instances,
                vertices, indices, localError))
            ;
        Impl::Draw draw;
        if (localError.empty())
        {
            std::filesystem::path basePath;
            std::filesystem::path bumpPath;
            if (!m_impl->options.resolveMaterialTextures ||
                !m_impl->options.resolveMaterialTextures(model->textureName, basePath, bumpPath, localError))
            {
                if (localError.empty()) localError = "details blend texture resolver rejected model";
                ++m_telemetry.textureRejectedBatches;
            }
            const auto loadTexture = [&resources, this, &localError](const std::filesystem::path& path,
                const char* label, std::shared_ptr<IR4Texture>& texture) {
                const std::string key = path.generic_string();
                const auto found = m_impl->textures.find(key);
                if (found != m_impl->textures.end())
                {
                    texture = found->second;
                    return true;
                }
                xray::metal::R4DdsTexture dds;
                if (!xray::metal::LoadR4DdsTexture(path, m_impl->options.ddsBytesProvider, dds, localError) ||
                    !uploadDds(resources, dds, label, texture, localError))
                    return false;
                m_impl->textures.emplace(key, texture);
                return true;
            };
            if (localError.empty() && (!loadTexture(basePath, "R4 details blend base DDS", draw.baseTexture) ||
                    !loadTexture(bumpPath, "R4 details blend bump DDS", draw.bumpTexture)))
                ;
        }
        const auto makeBuffer = [&resources, &localError](const std::size_t size,
            const R4BufferUsage usage, const char* label) {
            R4BufferDesc descriptor{size, R4ResourceStorage::Shared, usage, label};
            return resources.CreateBuffer(descriptor, nullptr, 0u, localError);
        };
        if (localError.empty())
        {
            draw.vertices = makeBuffer(vertices.size() * sizeof(R4DetailGrassVertex),
                R4BufferUsage::Vertex, "R4 details blend repacked vertices");
            draw.indices = makeBuffer(indices.size() * sizeof(std::uint16_t),
                R4BufferUsage::Index, "R4 details blend repacked indices");
            if (!draw.vertices || !draw.indices ||
                !resources.UpdateBuffer(draw.vertices, 0u, vertices.data(),
                    vertices.size() * sizeof(R4DetailGrassVertex), localError) ||
                !resources.UpdateBuffer(draw.indices, 0u, indices.data(),
                    indices.size() * sizeof(std::uint16_t), localError))
                if (localError.empty()) localError = "detail grass batch resource upload failed";
            draw.indexCount = indices.size();
            draw.instances = batch.instances;
            draw.bounds = batch;
        }
        if (!localError.empty())
        {
            ++m_telemetry.rejectedBatches;
            std::fprintf(stderr, "METAL_DETAIL_GRASS_BATCH_REJECT model=%u instances=%zu reason=\"%s\"\n",
                batch.modelId, batch.instances.size(), localError.c_str());
            continue;
        }
        m_telemetry.admittedInstances += batch.instances.size();
        m_impl->draws.emplace_back(std::move(draw));
    }
    std::fprintf(stderr, "METAL_DETAIL_GRASS_STAGE generation=%llu cache_slots=%zu fade=%.3f requested=%zu grounded=%zu "
        "rejected_instances=%zu distance_rejected_slots=%zu still=%zu admitted=%zu "
        "rejected_batches=%zu texture_rejected=%zu draws=%zu\n",
        static_cast<unsigned long long>(frame.generation), frame.cacheSlots, frame.cacheFadeDistance,
        frame.requestedInstances, frame.groundedInstances, frame.rejectedInstances,
        frame.distanceRejectedSlots, frame.stillInstances, m_telemetry.admittedInstances,
        m_telemetry.rejectedBatches, m_telemetry.textureRejectedBatches, m_impl->draws.size());
    error.clear();
    return true;
}

bool MetalR4DetailGrassAdapter::DrawIntoActiveGBuffer(IR4Backend& backend,
    const R4DetailGrassViewState& view, std::string& error)
{
    if (!m_ready || !m_impl || !view.valid)
        return true;
    Matrix4 viewMatrix{};
    Matrix4 projection{};
    Matrix4 previousViewMatrix{};
    Matrix4 previousProjection{};
    std::memcpy(viewMatrix.elements, view.view.data(), sizeof(viewMatrix));
    std::memcpy(projection.elements, view.projection.data(), sizeof(projection));
    std::memcpy(previousViewMatrix.elements,
        view.previousValid ? view.previousView.data() : view.view.data(),
        sizeof(previousViewMatrix));
    std::memcpy(previousProjection.elements,
        view.previousValid ? view.previousProjection.data() : view.projection.data(),
        sizeof(previousProjection));
    const Matrix4 vp = multiply(projection, viewMatrix);
    const Matrix4 previousVp = multiply(previousProjection, previousViewMatrix);
    const Matrix4 inverseView = inverseRigidTransform(viewMatrix);
    R4DetailGrassMotionSnapshot motion;
    if (!CaptureMetalR4EngineDetailGrassMotion(motion, error) || !motion.valid)
    {
        ++m_telemetry.motionCaptureFailures;
        if (error.empty()) error = "detail grass motion snapshot is unavailable";
        std::fprintf(stderr, "METAL_DETAIL_GRASS_MOTION_SKIP reason=\"%s\"\n", error.c_str());
        // The surrounding G-buffer remains valid; do not invent neutral
        // bender/wind constants while the authoritative engine state is not
        // live yet.
        error.clear();
        return true;
    }
    m_telemetry.activeBenders = motion.activeBenders;
    if (m_impl->lastMotionTelemetryFrame == UINT32_MAX ||
        motion.engineFrame - m_impl->lastMotionTelemetryFrame >= 300u)
    {
        std::fprintf(stderr, "METAL_DETAIL_GRASS_MOTION frame=%u active_benders=%u wind_dir=%.4f wind_velocity=%.4f "
            "wind_anim=(%.4f,%.4f,%.4f,%.4f)\n", motion.engineFrame, motion.activeBenders,
            motion.windParams[0], motion.windParams[1], motion.windAnimation[0], motion.windAnimation[1],
            motion.windAnimation[2], motion.windAnimation[3]);
        m_impl->lastMotionTelemetryFrame = motion.engineFrame;
    }
    std::uint32_t width = 0u, height = 0u;
    backend.DrawableSize(width, height);
    if (width == 0u || height == 0u)
    {
        error = "detail grass draw rejected a zero-sized drawable";
        return false;
    }
    m_telemetry.draws = 0u;
    m_telemetry.frustumRejectedBatches = 0u;
    for (Impl::Draw& draw : m_impl->draws)
    {
        if (isOutsideMetalClipVolume(draw.bounds, vp))
        {
            ++m_telemetry.frustumRejectedBatches;
            continue;
        }
        std::vector<std::uint8_t> vs(kVertexGlobalsSize);
        std::vector<std::uint8_t> ps(kFragmentGlobalsSize);
        std::vector<std::uint8_t> transforms(kTransformsSize);
        std::vector<std::uint8_t> statics(kStaticGlobalsSize);
        std::memcpy(vs.data(), &vp, sizeof(vp));
        std::memcpy(vs.data() + 64u, &previousVp, sizeof(previousVp));
        putFloat4(vs, 2224u, 1.0f / 16384.0f, 1.0f / 16384.0f, 0.0f, 0.0f);
        putFloat4(vs, 2240u, 0.0f, 0.0f, 0.0f, 0.0f);
        putFloat4(vs, 2256u, 1.0f, 0.0f, 0.0f, 0.0f);
        putFloat4(vs, 6176u, 0.0f, 0.0f, 0.0f, view.timeSeconds);
        putFloat4(vs, 6192u, motion.windParams[0], motion.windParams[1], motion.windParams[2], motion.windParams[3]);
        putFloat4(vs, 6208u, motion.windSetupGrass[0], motion.windSetupGrass[1],
            motion.windSetupGrass[2], motion.windSetupGrass[3]);
        putFloat4(vs, 6224u, motion.windSetupTrees[0], motion.windSetupTrees[1],
            motion.windSetupTrees[2], motion.windSetupTrees[3]);
        putFloat4(vs, 6240u, motion.windAnimation[0], motion.windAnimation[1],
            motion.windAnimation[2], motion.windAnimation[3]);
        putFloat4(vs, 6256u, motion.windAnimationPrevious[0], motion.windAnimationPrevious[1],
            motion.windAnimationPrevious[2], motion.windAnimationPrevious[3]);
        std::memcpy(vs.data() + 192u, motion.previousBenders.data(),
            sizeof(motion.previousBenders));
        std::memcpy(vs.data() + 704u, motion.currentBenders.data(),
            sizeof(motion.currentBenders));
        putFloat4(vs, 1216u, motion.bendersSetup[0], motion.bendersSetup[1],
            motion.bendersSetup[2], motion.bendersSetup[3]);
        std::memcpy(vs.data() + 2208u, &motion.grassAlign, sizeof(motion.grassAlign));
        for (std::size_t instanceIndex = 0; instanceIndex < draw.instances.size(); ++instanceIndex)
        {
            const R4DetailGrassInstance& instance = draw.instances[instanceIndex];
            const std::size_t exdata = 1232u + instanceIndex * 16u;
            const std::size_t matrix = 2272u + instanceIndex * 64u;
            // Legacy UpdateVisibleM changes detail visibility continuously
            // with camera distance.  A staged Metal cell is intentionally
            // world-stable, so calculate that fade per frame instead of
            // letting a whole cell pop when the cache origin advances.
            const float dx = instance.transform[12] - view.eye[0];
            const float dy = instance.transform[13] - view.eye[1];
            const float dz = instance.transform[14] - view.eye[2];
            const float distanceSquared = dx * dx + dy * dy + dz * dz;
            const float fadeLimit = m_impl->cacheFadeDistance * m_impl->cacheFadeDistance;
            const float fadeStartDistance = m_impl->cacheFadeDistance * 0.75f;
            const float fadeStart = fadeStartDistance * fadeStartDistance;
            const float fadeRange = std::max(1.0f, fadeLimit - fadeStart);
            const float distanceAlpha = (distanceSquared <= fadeStart) ? 1.0f :
                std::clamp(1.0f - (distanceSquared - fadeStart) / fadeRange, 0.0f, 1.0f);
            putFloat4(vs, exdata, instance.terrainNormal[0], instance.terrainNormal[1],
                instance.terrainNormal[2], instance.alpha * distanceAlpha);
            putFloat4(vs, matrix, instance.transform[0], instance.transform[4],
                instance.transform[8], instance.transform[12]);
            putFloat4(vs, matrix + 16u, instance.transform[1], instance.transform[5],
                instance.transform[9], instance.transform[13]);
            putFloat4(vs, matrix + 32u, instance.transform[2], instance.transform[6],
                instance.transform[10], instance.transform[14]);
            putFloat4(vs, matrix + 48u, instance.sun, instance.sun, instance.sun, instance.hemi);
        }
        std::memcpy(ps.data(), &vp, sizeof(vp));
        std::memcpy(ps.data() + 64u, &previousVp, sizeof(previousVp));
        std::memcpy(transforms.data(), &vp, sizeof(vp));
        std::memcpy(transforms.data() + 64u, &viewMatrix, 48u);
        std::memcpy(transforms.data() + 128u, &inverseView, 48u);
        std::memcpy(statics.data(), &viewMatrix, 48u);
        std::memcpy(statics.data() + 64u, &inverseView, 48u);
        std::memcpy(statics.data() + 128u, &projection, sizeof(projection));
        std::memcpy(statics.data() + 192u, &vp, sizeof(vp));
        const float fogRange = view.fogFar - view.fogNear;
        const float fogInverse = fogRange > 1.0e-5f ? 1.0f / fogRange : 0.0f;
        putFloat4(statics, 256u, view.timeSeconds, view.timeSeconds * 10.0f,
            view.timeSeconds / 10.0f, std::sin(view.timeSeconds));
        putFloat4(statics, 288u, -view.fogNear * fogInverse, view.fogNear,
            view.fogFar, fogInverse);
        putFloat4(statics, 304u, view.fogColor[0], view.fogColor[1], view.fogColor[2], view.fogDensity);
        putFloat4(statics, 320u, view.ambient[0], view.ambient[1], view.ambient[2], 0.0f);
        putFloat4(statics, 336u, view.sunColor[0], view.sunColor[1], view.sunColor[2], 0.0f);
        putFloat4(statics, 352u, view.sunDirection[0], view.sunDirection[1], view.sunDirection[2], 0.0f);
        putFloat4(statics, 368u, view.hemiColor[0], view.hemiColor[1], view.hemiColor[2], view.hemiColor[3]);
        putFloat4(statics, 384u, view.eye[0], view.eye[1], view.eye[2], 0.0f);
        putFloat4(statics, 416u, static_cast<float>(width), static_cast<float>(height),
            1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height));
        R4FrameBufferSlice vertexGlobalsSlice;
        R4FrameBufferSlice fragmentGlobalsSlice;
        R4FrameBufferSlice transformsSlice;
        R4FrameBufferSlice staticGlobalsSlice;
        if (!backend.UploadFrameData(vs.data(), vs.size(), 256u, vertexGlobalsSlice, error) ||
            !backend.UploadFrameData(ps.data(), ps.size(), 256u, fragmentGlobalsSlice, error) ||
            !backend.UploadFrameData(transforms.data(), transforms.size(), 256u, transformsSlice, error) ||
            !backend.UploadFrameData(statics.data(), statics.size(), 256u, staticGlobalsSlice, error))
            return false;
        R4MscStageBindings vertex{R4ShaderStage::Vertex, 120u, {{R4MscResourceType::Texture, 0u,
            {}, 0u, 0u, m_impl->wavesTexture}, {R4MscResourceType::ConstantBuffer, 24u,
            vertexGlobalsSlice.buffer, vertexGlobalsSlice.offset, vertexGlobalsSlice.size}, {R4MscResourceType::ConstantBuffer, 48u,
            transformsSlice.buffer, transformsSlice.offset, transformsSlice.size}, {R4MscResourceType::ConstantBuffer, 72u,
            staticGlobalsSlice.buffer, staticGlobalsSlice.offset, staticGlobalsSlice.size}, {R4MscResourceType::Sampler, 96u,
            {}, 0u, 0u, {}, m_impl->wavesSampler}}};
        R4MscStageBindings fragment{R4ShaderStage::Fragment, 144u, {{R4MscResourceType::Texture, 0u,
            {}, 0u, 0u, draw.baseTexture}, {R4MscResourceType::Texture, 24u, {}, 0u, 0u, draw.bumpTexture},
            {R4MscResourceType::ConstantBuffer, 48u, fragmentGlobalsSlice.buffer, fragmentGlobalsSlice.offset, fragmentGlobalsSlice.size},
            {R4MscResourceType::ConstantBuffer, 72u, transformsSlice.buffer, transformsSlice.offset, transformsSlice.size},
            {R4MscResourceType::ConstantBuffer, 96u, staticGlobalsSlice.buffer, staticGlobalsSlice.offset, staticGlobalsSlice.size},
            {R4MscResourceType::Sampler, 120u, {}, 0u, 0u, {}, m_impl->baseSampler}}};
        R4IndexedDrawDesc indexed;
        indexed.indexBuffer = draw.indices;
        indexed.indexCount = draw.indexCount;
        indexed.indexType = R4IndexType::UInt16;
        if (!backend.BindGraphicsPipeline(m_impl->pipeline, error) ||
            !backend.BindVertexBuffer(6u, draw.vertices, 0u, error) ||
            !backend.BindMscResources(vertex, error) || !backend.BindMscResources(fragment, error) ||
            !backend.DrawIndexed(indexed, error))
            return false;
        ++m_telemetry.draws;
    }
    return true;
}

bool MetalR4DetailGrassAdapter::HasShadowCasters() const
{
    return m_ready && m_impl && m_impl->shadowPipeline;
}

bool MetalR4DetailGrassAdapter::DrawShadowCasters(IR4Backend& backend,
    const R4DetailGrassViewState& view, const R4DetailGrassShadowState& shadow,
    std::string& error)
{
    if (!m_ready || !m_impl || !view.valid)
        return true;
    if (!HasShadowCasters())
    {
        error = m_impl->shadowError.empty() ? "detail grass shadow bundle is not configured" : m_impl->shadowError;
        return false;
    }
    Matrix4 lightView{};
    Matrix4 lightProjection{};
    Matrix4 lightViewProjection{};
    std::memcpy(lightView.elements, shadow.lightView.data(), sizeof(lightView));
    std::memcpy(lightProjection.elements, shadow.lightProjection.data(), sizeof(lightProjection));
    std::memcpy(lightViewProjection.elements, shadow.lightViewProjection.data(), sizeof(lightViewProjection));
    for (const float value : lightView.elements)
        if (!std::isfinite(value)) { error = "detail grass shadow view matrix is non-finite"; return false; }
    for (const float value : lightProjection.elements)
        if (!std::isfinite(value)) { error = "detail grass shadow projection matrix is non-finite"; return false; }
    for (const float value : lightViewProjection.elements)
        if (!std::isfinite(value)) { error = "detail grass shadow view-projection matrix is non-finite"; return false; }
    if (shadow.fadeFromLight && (!finite3(shadow.lightPosition) || !std::isfinite(shadow.lightRange) ||
            shadow.lightRange <= 0.0f || !std::isfinite(shadow.lightRange * shadow.lightRange)))
    {
        error = "local detail-grass shadow needs a finite positive light position and range";
        return false;
    }

    R4DetailGrassMotionSnapshot motion;
    if (!CaptureMetalR4EngineDetailGrassMotion(motion, error) || !motion.valid)
    {
        if (error.empty()) error = "detail grass shadow motion snapshot is unavailable";
        return false;
    }
    const Matrix4 inverseLightView = inverseRigidTransform(lightView);
    std::uint32_t width = 0u, height = 0u;
    backend.DrawableSize(width, height);
    if (width == 0u || height == 0u)
    {
        error = "detail grass shadow draw rejected a zero-sized drawable";
        return false;
    }
    m_telemetry.shadowDraws = 0u;
    m_telemetry.shadowCulledBatches = 0u;
    m_telemetry.shadowRejected = 0u;
    // The shadow tile matrices and lighting/fog globals are invariant for
    // every detail batch in this call. Upload these shared blocks only when
    // the first batch survives culling; per-instance vertex globals remain
    // unique to each draw below.
    std::vector<std::uint8_t> transforms(kTransformsSize);
    std::memcpy(transforms.data(), &lightViewProjection, sizeof(lightViewProjection));
    std::memcpy(transforms.data() + 64u, &lightView, 48u);
    std::memcpy(transforms.data() + 128u, &inverseLightView, 48u);
    std::vector<std::uint8_t> statics(kStaticGlobalsSize);
    std::memcpy(statics.data(), &lightView, 48u);
    std::memcpy(statics.data() + 64u, &inverseLightView, 48u);
    std::memcpy(statics.data() + 128u, &lightProjection, sizeof(lightProjection));
    std::memcpy(statics.data() + 192u, &lightViewProjection, sizeof(lightViewProjection));
    const float fogRange = view.fogFar - view.fogNear;
    const float fogInverse = fogRange > 1.0e-5f ? 1.0f / fogRange : 0.0f;
    putFloat4(statics, 256u, view.timeSeconds, view.timeSeconds * 10.0f,
        view.timeSeconds / 10.0f, std::sin(view.timeSeconds));
    putFloat4(statics, 288u, -view.fogNear * fogInverse, view.fogNear,
        view.fogFar, fogInverse);
    putFloat4(statics, 304u, view.fogColor[0], view.fogColor[1], view.fogColor[2], view.fogDensity);
    putFloat4(statics, 320u, view.ambient[0], view.ambient[1], view.ambient[2], 0.0f);
    putFloat4(statics, 336u, view.sunColor[0], view.sunColor[1], view.sunColor[2], 0.0f);
    putFloat4(statics, 352u, view.sunDirection[0], view.sunDirection[1], view.sunDirection[2], 0.0f);
    putFloat4(statics, 368u, view.hemiColor[0], view.hemiColor[1], view.hemiColor[2], view.hemiColor[3]);
    putFloat4(statics, 384u, view.eye[0], view.eye[1], view.eye[2], 0.0f);
    putFloat4(statics, 416u, static_cast<float>(width), static_cast<float>(height),
        1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height));
    R4FrameBufferSlice transformsSlice;
    R4FrameBufferSlice staticGlobalsSlice;
    bool sharedShadowGlobalsUploaded = false;
    for (Impl::Draw& draw : m_impl->draws)
    {
        if (isOutsideMetalClipVolume(draw.bounds, lightViewProjection))
        {
            ++m_telemetry.shadowCulledBatches;
            continue;
        }
        std::vector<std::uint8_t> vs(kVertexGlobalsSize);
        std::memcpy(vs.data(), &lightViewProjection, sizeof(lightViewProjection));
        std::memcpy(vs.data() + 64u, &lightViewProjection, sizeof(lightViewProjection));
        putFloat4(vs, 2224u, 1.0f / 16384.0f, 1.0f / 16384.0f, 0.0f, 0.0f);
        putFloat4(vs, 2240u, 0.0f, 0.0f, 0.0f, view.timeSeconds);
        putFloat4(vs, 2256u, 0.0f, 0.0f, 0.0f, 0.0f);
        std::memcpy(vs.data() + 192u, motion.previousBenders.data(), sizeof(motion.previousBenders));
        std::memcpy(vs.data() + 704u, motion.currentBenders.data(), sizeof(motion.currentBenders));
        putFloat4(vs, 1216u, motion.bendersSetup[0], motion.bendersSetup[1],
            motion.bendersSetup[2], motion.bendersSetup[3]);
        std::memcpy(vs.data() + 2208u, &motion.grassAlign, sizeof(motion.grassAlign));
        putFloat4(vs, 6176u, 0.0f, 0.0f, 0.0f, view.timeSeconds);
        putFloat4(vs, 6192u, motion.windParams[0], motion.windParams[1],
            motion.windParams[2], motion.windParams[3]);
        putFloat4(vs, 6208u, motion.windSetupGrass[0], motion.windSetupGrass[1],
            motion.windSetupGrass[2], motion.windSetupGrass[3]);
        putFloat4(vs, 6224u, motion.windSetupTrees[0], motion.windSetupTrees[1],
            motion.windSetupTrees[2], motion.windSetupTrees[3]);
        putFloat4(vs, 6240u, motion.windAnimation[0], motion.windAnimation[1],
            motion.windAnimation[2], motion.windAnimation[3]);
        putFloat4(vs, 6256u, motion.windAnimationPrevious[0], motion.windAnimationPrevious[1],
            motion.windAnimationPrevious[2], motion.windAnimationPrevious[3]);
        for (std::size_t instanceIndex = 0; instanceIndex < draw.instances.size(); ++instanceIndex)
        {
            const R4DetailGrassInstance& instance = draw.instances[instanceIndex];
            const float fadeScale = detailGrassShadowFadeScale(instance, shadow);
            // Windows stores the decompressed slot sphere center in both
            // Item.position and the source Item.distance (squared). Local
            // lights reject at full 3D light range, then fade by squared XZ
            // distance from that same Item.position.
            // Keep the source alpha for positive scales; at zero scale clear
            // alpha so wind offsets cannot rasterize degenerate matrix pixels.
            const float matrixScale = fadeScale > 0.0f ? fadeScale : 0.0f;
            putFloat4(vs, 1232u + instanceIndex * 16u,
                instance.terrainNormal[0], instance.terrainNormal[1], instance.terrainNormal[2],
                matrixScale > 0.0f ? instance.alpha : 0.0f);
            const std::size_t matrix = 2272u + instanceIndex * 64u;
            putFloat4(vs, matrix, instance.transform[0] * matrixScale, instance.transform[4] * matrixScale,
                instance.transform[8] * matrixScale, instance.transform[12]);
            putFloat4(vs, matrix + 16u, instance.transform[1] * matrixScale,
                instance.transform[5] * matrixScale, instance.transform[9] * matrixScale, instance.transform[13]);
            putFloat4(vs, matrix + 32u, instance.transform[2] * matrixScale,
                instance.transform[6] * matrixScale, instance.transform[10] * matrixScale, instance.transform[14]);
            putFloat4(vs, matrix + 48u, instance.sun, instance.sun, instance.sun, instance.hemi);
        }
        R4FrameBufferSlice vertexGlobalsSlice;
        if (!backend.UploadFrameData(vs.data(), vs.size(), 256u, vertexGlobalsSlice, error))
            return false;
        if (!sharedShadowGlobalsUploaded)
        {
            if (!backend.UploadFrameData(transforms.data(), transforms.size(), 256u, transformsSlice, error) ||
                !backend.UploadFrameData(statics.data(), statics.size(), 256u, staticGlobalsSlice, error))
                return false;
            sharedShadowGlobalsUploaded = true;
        }
        R4MscStageBindings vertex{R4ShaderStage::Vertex, 120u, {{R4MscResourceType::Texture, 0u,
            {}, 0u, 0u, m_impl->wavesTexture}, {R4MscResourceType::ConstantBuffer, 24u,
            vertexGlobalsSlice.buffer, vertexGlobalsSlice.offset, vertexGlobalsSlice.size},
            {R4MscResourceType::ConstantBuffer, 48u, transformsSlice.buffer,
                transformsSlice.offset, transformsSlice.size}, {R4MscResourceType::ConstantBuffer, 72u,
                staticGlobalsSlice.buffer, staticGlobalsSlice.offset, staticGlobalsSlice.size},
            {R4MscResourceType::Sampler, 96u, {}, 0u, 0u, {}, m_impl->wavesSampler}}};
        R4MscStageBindings fragment{R4ShaderStage::Fragment, 72u, {{R4MscResourceType::Texture, 0u,
            {}, 0u, 0u, draw.baseTexture}, {R4MscResourceType::ConstantBuffer, 24u,
            staticGlobalsSlice.buffer, staticGlobalsSlice.offset, staticGlobalsSlice.size},
            {R4MscResourceType::Sampler, 48u, {}, 0u, 0u, {}, m_impl->baseSampler}}};
        R4IndexedDrawDesc indexed;
        indexed.indexBuffer = draw.indices;
        indexed.indexCount = draw.indexCount;
        indexed.indexType = R4IndexType::UInt16;
        if (!backend.BindGraphicsPipeline(m_impl->shadowPipeline, error) ||
            !backend.BindVertexBuffer(6u, draw.vertices, 0u, error) ||
            !backend.BindMscResources(vertex, error) || !backend.BindMscResources(fragment, error) ||
            !backend.DrawIndexed(indexed, error))
            return false;
        ++m_telemetry.shadowDraws;
    }
    return true;
}

bool RunR4DetailGrassAdapterSelfTest(std::string& error)
{
    // A minimal v4 chunked level.details stream with one model and one slot.
    std::vector<std::uint8_t> bytes;
    const auto writeU32 = [&bytes](const std::uint32_t value) {
        const std::size_t offset = bytes.size();
        bytes.resize(offset + sizeof(value));
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    };
    const auto writeU16 = [&bytes](const std::uint16_t value) {
        const std::size_t offset = bytes.size();
        bytes.resize(offset + sizeof(value));
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    };
    const auto writeF32 = [&bytes](const float value) {
        const std::size_t offset = bytes.size();
        bytes.resize(offset + sizeof(value));
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    };
    const auto appendChunk = [&bytes, &writeU32](const std::uint32_t id,
        const std::vector<std::uint8_t>& payload) {
        writeU32(id);
        writeU32(static_cast<std::uint32_t>(payload.size()));
        bytes.insert(bytes.end(), payload.begin(), payload.end());
    };
    std::vector<std::uint8_t> header;
    const auto appendHeaderU32 = [&header](const std::uint32_t value) {
        const std::size_t offset = header.size();
        header.resize(offset + sizeof(value));
        std::memcpy(header.data() + offset, &value, sizeof(value));
    };
    appendHeaderU32(kDetailVersion4); appendHeaderU32(1u); appendHeaderU32(0u);
    appendHeaderU32(0u); appendHeaderU32(1u); appendHeaderU32(1u);
    appendChunk(0u, header);
    std::vector<std::uint8_t> model;
    const char shader[] = "details\\blend";
    const char texture[] = "build_details";
    model.insert(model.end(), shader, shader + sizeof(shader));
    model.insert(model.end(), texture, texture + sizeof(texture));
    const auto appendModelU32 = [&model](const std::uint32_t value) {
        const std::size_t offset = model.size(); model.resize(offset + sizeof(value));
        std::memcpy(model.data() + offset, &value, sizeof(value));
    };
    const auto appendModelU16 = [&model](const std::uint16_t value) {
        const std::size_t offset = model.size(); model.resize(offset + sizeof(value));
        std::memcpy(model.data() + offset, &value, sizeof(value));
    };
    const auto appendModelF32 = [&model](const float value) {
        const std::size_t offset = model.size(); model.resize(offset + sizeof(value));
        std::memcpy(model.data() + offset, &value, sizeof(value));
    };
    // Real GAMMA level.details assets may have a nominal max a few hundredths
    // below min. The legacy renderer applies separate 0.5/0.9 factors before
    // generating scale, so this remains a valid ordered runtime interval.
    appendModelU32(0u); appendModelF32(1.5f); appendModelF32(1.49f); appendModelU32(3u); appendModelU32(3u);
    appendModelF32(0.0f); appendModelF32(0.0f); appendModelF32(0.0f); appendModelF32(0.0f); appendModelF32(0.0f);
    appendModelF32(1.0f); appendModelF32(1.0f); appendModelF32(0.0f); appendModelF32(1.0f); appendModelF32(0.0f);
    appendModelF32(0.0f); appendModelF32(2.0f); appendModelF32(0.0f); appendModelF32(0.0f); appendModelF32(1.0f);
    appendModelU16(0u); appendModelU16(1u); appendModelU16(2u);
    std::vector<std::uint8_t> models;
    const auto appendModelsU32 = [&models](const std::uint32_t value) {
        const std::size_t offset = models.size(); models.resize(offset + sizeof(value));
        std::memcpy(models.data() + offset, &value, sizeof(value));
    };
    appendModelsU32(0u); appendModelsU32(static_cast<std::uint32_t>(model.size()));
    models.insert(models.end(), model.begin(), model.end());
    appendChunk(1u, models);
    std::vector<std::uint8_t> slots;
    const auto appendSlotsU32 = [&slots](const std::uint32_t value) {
        const std::size_t offset = slots.size(); slots.resize(offset + sizeof(value));
        std::memcpy(slots.data() + offset, &value, sizeof(value));
    };
    const auto appendSlotsU16 = [&slots](const std::uint16_t value) {
        const std::size_t offset = slots.size(); slots.resize(offset + sizeof(value));
        std::memcpy(slots.data() + offset, &value, sizeof(value));
    };
    appendSlotsU32(0u | (kEmptyModelId << 14u) | (15u << 28u));
    appendSlotsU32(kEmptyModelId | (kEmptyModelId << 14u) | (15u << 28u));
    appendSlotsU32(1000u | (20u << 12u));
    appendSlotsU16(0xffffu); appendSlotsU16(0xffffu); appendSlotsU16(0xffffu); appendSlotsU16(0xffffu);
    appendChunk(2u, slots);

    R4DetailGrassSnapshot snapshot;
    if (!LoadR4DetailGrassSnapshot(bytes, snapshot, error) || snapshot.models.size() != 1u ||
        snapshot.slots.size() != 1u || snapshot.slots[0].modelIds[0] != 0u ||
        snapshot.slots[0].modelIds[1] != kEmptyModelId ||
        std::abs(snapshot.slots[0].sun - 1.0f) > 1e-4f ||
        std::abs(snapshot.slots[0].hemi - 1.0f) > 1e-4f ||
        std::abs(snapshot.slots[0].yBase - 0.0f) > 1e-4f ||
        std::abs(snapshot.slots[0].yHeight - 2.0f) > 1e-4f ||
        snapshot.slots[0].palette[0][0] != 0x0fu)
    {
        if (error.empty()) error = "level.details v4 slot unpacking verification failed";
        return false;
    }
    std::vector<std::uint8_t> v3Bytes;
    const auto appendV3U32 = [&v3Bytes](const std::uint32_t value) {
        const std::size_t offset = v3Bytes.size();
        v3Bytes.resize(offset + sizeof(value));
        std::memcpy(v3Bytes.data() + offset, &value, sizeof(value));
    };
    const auto appendV3Chunk = [&v3Bytes, &appendV3U32](const std::uint32_t id,
        const std::vector<std::uint8_t>& payload) {
        appendV3U32(id);
        appendV3U32(static_cast<std::uint32_t>(payload.size()));
        v3Bytes.insert(v3Bytes.end(), payload.begin(), payload.end());
    };
    std::vector<std::uint8_t> v3Header = header;
    const std::uint32_t v3Version = kDetailVersion3;
    std::memcpy(v3Header.data(), &v3Version, sizeof(v3Version));
    appendV3Chunk(0u, v3Header);
    appendV3Chunk(1u, models);
    std::vector<std::uint8_t> v3Slots;
    const auto appendV3SlotU32 = [&v3Slots](const std::uint32_t value) {
        const std::size_t offset = v3Slots.size();
        v3Slots.resize(offset + sizeof(value));
        std::memcpy(v3Slots.data() + offset, &value, sizeof(value));
    };
    const auto appendV3SlotU16 = [&v3Slots](const std::uint16_t value) {
        const std::size_t offset = v3Slots.size();
        v3Slots.resize(offset + sizeof(value));
        std::memcpy(v3Slots.data() + offset, &value, sizeof(value));
    };
    appendV3SlotU32(1000u | (20u << 12u) | (0u << 20u) | (0x3fu << 26u));
    appendV3SlotU32(0x3fu | (0x3fu << 6u) | (15u << 12u) | (15u << 16u));
    appendV3SlotU16(0xffffu); appendV3SlotU16(0xffffu); appendV3SlotU16(0xffffu); appendV3SlotU16(0xffffu);
    appendV3Chunk(2u, v3Slots);
    R4DetailGrassSnapshot v3Snapshot;
    if (!LoadR4DetailGrassSnapshot(v3Bytes, v3Snapshot, error) || v3Snapshot.version != kDetailVersion3 ||
        v3Snapshot.slots.size() != 1u || v3Snapshot.slots[0].modelIds[0] != 0u ||
        v3Snapshot.slots[0].modelIds[1] != kEmptyModelId ||
        std::abs(v3Snapshot.slots[0].sun - 1.0f) > 1e-4f ||
        std::abs(v3Snapshot.slots[0].hemi - 1.0f) > 1e-4f ||
        std::abs(v3Snapshot.slots[0].yBase - 0.0f) > 1e-4f ||
        std::abs(v3Snapshot.slots[0].yHeight - 2.0f) > 1e-4f)
    {
        if (error.empty()) error = "level.details v3 conversion self-test failed";
        return false;
    }
    std::vector<R4DetailGrassInstance> instances(kR4DetailGrassBatchSize);
    for (R4DetailGrassInstance& instance : instances)
    {
        instance.transform[0] = instance.transform[5] = instance.transform[10] = instance.transform[15] = 1.0f;
        instance.shadowFadePosition = {4.0f, 2.0f, 6.0f};
        instance.shadowDistanceSquared = 56.0f;
        instance.hasShadowFadePosition = true;
        instance.terrainNormal = {0.0f, 1.0f, 0.0f};
        instance.sun = 1.0f;
        instance.hemi = 1.0f;
    }
    std::vector<R4DetailGrassVertex> vertices;
    std::vector<std::uint16_t> indices;
    if (!RepackR4DetailGrassBatch(snapshot.models[0], instances, vertices, indices, error) ||
        vertices.size() != 183u || indices.size() != 183u || vertices[0].misc[3] != 0 ||
        vertices[3].misc[3] != 4 || vertices.back().misc[3] != 240 || indices.back() != 182u)
    {
        if (error.empty()) error = "detail grass batch self-test produced an unexpected ABI";
        return false;
    }
    std::vector<R4DetailGrassInstance> missingFadePosition = instances;
    missingFadePosition[0].hasShadowFadePosition = false;
    if (RepackR4DetailGrassBatch(snapshot.models[0], missingFadePosition, vertices, indices, error))
    {
        error = "detail grass repack accepted an instance without the Windows slot-center fade metadata";
        return false;
    }
    R4DetailGrassShadowState localShadow;
    localShadow.fadeFromLight = true;
    localShadow.lightPosition = {0.0f, 0.0f, 0.0f};
    localShadow.lightRange = 4.0f;
    R4DetailGrassInstance inRange = instances[0];
    inRange.shadowFadePosition = {2.0f, 1.0f, 2.0f};
    R4DetailGrassInstance atRange = inRange;
    atRange.shadowFadePosition = {0.0f, 4.0f, 0.0f};
    R4DetailGrassInstance outsideRange = inRange;
    outsideRange.shadowFadePosition = {0.0f, 4.1f, 0.0f};
    if (std::abs(detailGrassShadowFadeScale(inRange, localShadow) - 0.96f) > 1e-5f ||
        detailGrassShadowFadeScale(atRange, localShadow) != 0.0f ||
        detailGrassShadowFadeScale(outsideRange, localShadow) != 0.0f)
    {
        error = "detail grass local shadow did not apply Windows full-3D range before XZ fade";
        return false;
    }
    R4DetailGrassShadowState sunShadow;
    sunShadow.fadeDistance = 100.0f;
    R4DetailGrassInstance sunFade = inRange;
    sunFade.shadowDistanceSquared = 110.0f;
    if (std::abs(detailGrassShadowFadeScale(sunFade, sunShadow) - 0.95f) > 1e-5f)
    {
        error = "detail grass sun fade did not use squared source camera distance";
        return false;
    }
    instances.emplace_back();
    if (RepackR4DetailGrassBatch(snapshot.models[0], instances, vertices, indices, error))
    {
        error = "detail grass repack accepted more than 61 instances";
        return false;
    }
    error.clear();
    return true;
}
