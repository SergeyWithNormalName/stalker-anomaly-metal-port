#pragma once

#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// A value-only handoff for the CPU-expanded R4 particle sprite stream.  It is
// intentionally independent of RCache/D3D lifetimes: the producer copies its
// FVF::LIT vertices while they are still valid, and the Metal scene can later
// drain an immutable frame batch.
namespace xray::metal::particles
{
enum class Permutation : std::uint8_t
{
    Soft,
    Hard,
};

enum class CullMode : std::uint8_t
{
    None,
    Clockwise,
    CounterClockwise,
};

enum class MaterialFamily : std::uint8_t
{
    Unknown,
    BlenderParticle,
};

enum class BlendMode : std::uint8_t
{
    Unknown,
    Set,
    Blend,
    Add,
    Multiply,
    Multiply2x,
    AlphaAdd,
};

enum class BlendFactor : std::uint8_t
{
    Unknown,
    Zero,
    One,
    SourceAlpha,
    OneMinusSourceAlpha,
    DestinationColor,
    SourceColor,
};

struct SpriteVertex
{
    float position[3]{};
    std::uint32_t d3dColor = 0;
    float texcoord[2]{};
};
static_assert(sizeof(SpriteVertex) == 24u,
    "particle capture must preserve the packed FVF::LIT float3/D3DCOLOR/float2 ABI");

struct DrawRecord
{
    std::string shaderIdentity;
    std::string textureIdentity;
    Permutation permutation = Permutation::Hard;
    CullMode cullMode = CullMode::None;
    MaterialFamily materialFamily = MaterialFamily::Unknown;
    BlendMode blendMode = BlendMode::Unknown;
    BlendFactor sourceBlend = BlendFactor::Unknown;
    BlendFactor destinationBlend = BlendFactor::Unknown;
    std::int32_t priority = 0;
    bool strictBackToFront = false;
    bool baseSamplerClamp = false;
    bool materialMetadataValid = false;
    // Exact particles\\xdistort and particles\\xadd request the scripted
    // l_special particle_distort pass in addition to (or instead of) their
    // ordinary color pass. The Metal consumer keeps it in a separate mask.
    bool distortionPass = false;
    float distanceSquared = 0.0f;
    // The sprite generator has already applied m_XFORM to positions if this
    // is set. HUD projection is separately active through CHudInitializer;
    // worldIdentity is true because the legacy draw sets Fidentity directly.
    bool particleTransformApplied = false;
    bool hudMode = false;
    bool worldIdentity = true;
    std::uint32_t firstVertex = 0;
    std::uint32_t vertexCount = 0;
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
};

struct FrameBatch
{
    std::vector<SpriteVertex> vertices;
    // Frame capture uses a wide global index domain. The Metal preparation
    // stage splits this into <= uint16-local sprite batches before upload.
    std::vector<std::uint32_t> indices;
    std::vector<DrawRecord> draws;
    std::uint32_t droppedDraws = 0;
    std::uint32_t droppedVertices = 0;
};

class FrameCapture
{
public:
    static constexpr std::uint32_t MaxVertices = 1u << 20u;
    static constexpr std::uint32_t MaxIndices = 3u << 20u;
    static constexpr std::uint32_t MaxDraws = 4096u;

    explicit FrameCapture(const std::uint32_t maxVertices = MaxVertices,
        const std::uint32_t maxIndices = MaxIndices, const std::uint32_t maxDraws = MaxDraws)
        : m_maxVertices(maxVertices), m_maxIndices(maxIndices), m_maxDraws(maxDraws)
    {
    }

    bool append(DrawRecord record, const SpriteVertex* vertices, const std::uint32_t vertexCount)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const std::uint32_t quadCount = vertexCount / 4u;
        if (!vertices || vertexCount == 0u || vertexCount % 4u != 0u ||
            m_batch.draws.size() >= m_maxDraws || vertexCount > m_maxVertices - m_batch.vertices.size() ||
            quadCount > (m_maxIndices - m_batch.indices.size()) / 6u)
        {
            recordDroppedLocked(vertexCount);
            return false;
        }

        record.firstVertex = static_cast<std::uint32_t>(m_batch.vertices.size());
        record.vertexCount = vertexCount;
        record.firstIndex = static_cast<std::uint32_t>(m_batch.indices.size());
        record.indexCount = quadCount * 6u;
        m_batch.vertices.insert(m_batch.vertices.end(), vertices, vertices + vertexCount);
        m_batch.indices.reserve(m_batch.indices.size() + record.indexCount);
        for (std::uint32_t quad = 0u; quad < quadCount; ++quad)
        {
            const std::uint32_t base = record.firstVertex + quad * 4u;
            m_batch.indices.insert(m_batch.indices.end(), {
                base, base + 1u, base + 2u, base, base + 2u, base + 3u,
            });
        }
        m_batch.draws.push_back(std::move(record));
        return true;
    }

    void reject(const std::size_t vertexCount)
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
    void recordDroppedLocked(const std::size_t vertexCount)
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

inline bool AppendSpriteDraw(DrawRecord record, const SpriteVertex* vertices, const std::uint32_t vertexCount)
{
    return capture().append(std::move(record), vertices, vertexCount);
}

inline void RecordDroppedSpriteDraw(const std::size_t vertexCount)
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
} // namespace xray::metal::particles
