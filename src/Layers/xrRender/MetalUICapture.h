#pragma once

#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace xray::metal::ui
{
enum class PrimitiveType : std::uint8_t
{
    TriangleList,
    TriangleStrip,
    LineStrip,
    LineList,
};

enum class PointType : std::uint8_t
{
    TL,
    LIT,
};

enum class TextureCoordinateSpace : std::uint8_t
{
    Normalized,
    AtlasPixels,
};

struct ShaderIdentity
{
    std::string shader;
    std::string texture;
};

struct Vertex
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    std::uint32_t color = 0;
    float u = 0.0f;
    float v = 0.0f;
};

struct Scissor
{
    bool enabled = false;
    std::int32_t x1 = 0;
    std::int32_t y1 = 0;
    std::int32_t x2 = 0;
    std::int32_t y2 = 0;
};

struct DrawRecord
{
    ShaderIdentity shader;
    PrimitiveType primitive = PrimitiveType::TriangleList;
    PointType pointType = PointType::TL;
    TextureCoordinateSpace textureCoordinateSpace = TextureCoordinateSpace::Normalized;
    Scissor scissor;
    std::int32_t alphaRef = 0;
    std::uint8_t cullMode = 0;
    std::uint32_t firstVertex = 0;
    std::uint32_t vertexCount = 0;
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
    bool font = false;
};

struct FrameBatch
{
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<DrawRecord> draws;
    std::uint32_t droppedDraws = 0;
    std::uint32_t droppedVertices = 0;
};

class FrameCapture
{
public:
    static constexpr std::uint32_t MaxVertices = 1u << 20;
    static constexpr std::uint32_t MaxIndices = 3u << 20;
    static constexpr std::uint32_t MaxDraws = 4096u;

    explicit FrameCapture(std::uint32_t maxVertices = MaxVertices, std::uint32_t maxIndices = MaxIndices,
        std::uint32_t maxDraws = MaxDraws)
        : m_maxVertices(maxVertices), m_maxIndices(maxIndices), m_maxDraws(maxDraws)
    {
    }

    bool append(DrawRecord record, const std::vector<Vertex>& vertices, const std::vector<std::uint32_t>& indices)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        bool validIndices = true;
        for (const std::uint32_t index : indices)
            validIndices = validIndices && index < vertices.size();
        if (vertices.empty() || indices.empty() || !validIndices || m_batch.draws.size() >= m_maxDraws ||
            vertices.size() > m_maxVertices - m_batch.vertices.size() ||
            indices.size() > m_maxIndices - m_batch.indices.size())
        {
            recordDroppedLocked(vertices.size());
            return false;
        }

        record.firstVertex = static_cast<std::uint32_t>(m_batch.vertices.size());
        record.vertexCount = static_cast<std::uint32_t>(vertices.size());
        record.firstIndex = static_cast<std::uint32_t>(m_batch.indices.size());
        record.indexCount = static_cast<std::uint32_t>(indices.size());
        m_batch.vertices.insert(m_batch.vertices.end(), vertices.begin(), vertices.end());
        m_batch.indices.reserve(m_batch.indices.size() + indices.size());
        for (const std::uint32_t index : indices)
            m_batch.indices.push_back(record.firstVertex + index);
        m_batch.draws.push_back(std::move(record));
        return true;
    }

    void reject(std::size_t vertexCount)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        recordDroppedLocked(vertexCount);
    }

    FrameBatch drain()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        FrameBatch drained;
        drained.vertices.swap(m_batch.vertices);
        drained.indices.swap(m_batch.indices);
        drained.draws.swap(m_batch.draws);
        std::swap(drained.droppedDraws, m_batch.droppedDraws);
        std::swap(drained.droppedVertices, m_batch.droppedVertices);
        return drained;
    }

    void reset()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_batch = {};
    }

private:
    void recordDroppedLocked(std::size_t vertexCount)
    {
        ++m_batch.droppedDraws;
        const std::uint32_t dropped = static_cast<std::uint32_t>(vertexCount >
            std::numeric_limits<std::uint32_t>::max() ? std::numeric_limits<std::uint32_t>::max() : vertexCount);
        m_batch.droppedVertices = dropped > std::numeric_limits<std::uint32_t>::max() - m_batch.droppedVertices
            ? std::numeric_limits<std::uint32_t>::max() : m_batch.droppedVertices + dropped;
    }

    std::mutex m_mutex;
    std::uint32_t m_maxVertices;
    std::uint32_t m_maxIndices;
    std::uint32_t m_maxDraws;
    FrameBatch m_batch;
};

inline FrameCapture& capture()
{
    static FrameCapture instance;
    return instance;
}

inline bool AppendDraw(DrawRecord record, const std::vector<Vertex>& vertices, const std::vector<std::uint32_t>& indices)
{
    return capture().append(std::move(record), vertices, indices);
}

inline bool AppendSequentialDraw(DrawRecord record, const std::vector<Vertex>& vertices)
{
    std::vector<std::uint32_t> indices(vertices.size());
    for (std::uint32_t index = 0; index < indices.size(); ++index)
        indices[index] = index;
    return AppendDraw(std::move(record), vertices, indices);
}

inline void RecordDroppedDraw(std::size_t vertexCount)
{
    capture().reject(vertexCount);
}

inline FrameBatch DrainFrameBatch()
{
    return capture().drain();
}

inline void ResetFrameBatch()
{
    capture().reset();
}
} // namespace xray::metal::ui
