#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace xray::metal
{
// CPU-only, order-preserving merge of consecutive static shadow draws. The
// caller supplies rows in source draw order after admission/culling. A run's
// indices are relative to Run::baseVertex; adding that base to every emitted
// index must recover the exact original absolute vertex IDs.
class R4StaticShadowMerge
{
public:
    struct Row
    {
        std::size_t vertexBufferId = 0;
        const std::uint16_t* indices = nullptr;
        std::size_t indexCount = 0;
        std::size_t vertexBase = 0;
        std::size_t vertexCount = 0;
        std::size_t vertexBufferVertexCount = 0;
        const void* modelBytes = nullptr;
        std::size_t modelByteCount = 0;
        const void* wvpBytes = nullptr;
        std::size_t wvpByteCount = 0;
        bool admitted = true;
        bool opaque = true;
        bool supported = true;
    };

    struct Run
    {
        std::size_t firstRow = 0;
        std::size_t rowCount = 0;
        std::size_t vertexBufferId = 0;
        std::size_t baseVertex = 0;
        std::size_t indexOffset = 0;
        std::size_t indexCount = 0;
        bool merged = false;
    };

    struct Result
    {
        std::vector<Run> runs;
        std::vector<std::uint16_t> indices;
    };

    // On failure, result is cleared so callers cannot accidentally consume a
    // partial plan. Rejected rows are omitted and form a hard adjacency break.
    static bool Build(const Row* rows, std::size_t rowCount, Result& result,
        const char*& error)
    {
        result.runs.clear();
        result.indices.clear();
        error = nullptr;
        if (rowCount && !rows)
        {
            error = "static shadow merge has null row array";
            return false;
        }
        if (rowCount > result.runs.max_size() || rowCount > result.indices.max_size())
        {
            error = "static shadow merge row count exceeds output capacity";
            return false;
        }
        std::size_t totalIndices = 0;
        for (std::size_t i = 0; i < rowCount; ++i)
        {
            if (!rows[i].admitted) continue;
            if (!Validate(rows[i], error)) return false;
            if (rows[i].indexCount > std::numeric_limits<std::size_t>::max() - totalIndices ||
                totalIndices + rows[i].indexCount > result.indices.max_size())
            {
                error = "static shadow merge index count exceeds output capacity";
                return false;
            }
            totalIndices += rows[i].indexCount;
        }
        result.runs.reserve(rowCount);
        result.indices.reserve(totalIndices);

        bool haveCurrent = false;
        Run current;
        const Row* currentKey = nullptr;
        const auto flush = [&]() {
            if (!haveCurrent) return;
            result.runs.push_back(current);
            haveCurrent = false;
            currentKey = nullptr;
        };

        for (std::size_t rowIndex = 0; rowIndex < rowCount; ++rowIndex)
        {
            const Row& row = rows[rowIndex];
            if (!row.admitted)
            {
                flush();
                continue;
            }
            bool canMerge = row.opaque && row.supported && row.indexCount % 3u == 0u &&
                haveCurrent && currentKey &&
                currentKey->opaque && currentKey->supported &&
                current.vertexBufferId == row.vertexBufferId &&
                row.indexCount <= std::numeric_limits<std::uint32_t>::max() - current.indexCount &&
                SameBytes(currentKey->modelBytes, currentKey->modelByteCount,
                    row.modelBytes, row.modelByteCount) &&
                SameBytes(currentKey->wvpBytes, currentKey->wvpByteCount,
                    row.wvpBytes, row.wvpByteCount);

            if (canMerge && CanRebase(row, current.baseVertex))
            {
                if (!AppendRebased(row, current.baseVertex, result.indices, error))
                {
                    result.runs.clear();
                    result.indices.clear();
                    return false;
                }
                current.indexCount += row.indexCount;
                ++current.rowCount;
                current.merged = true;
                continue;
            }

            flush();
            current = Run{rowIndex, 1u, row.vertexBufferId, row.vertexBase,
                result.indices.size(), row.indexCount, false};
            if (!AppendOriginal(row, result.indices, error))
            {
                result.runs.clear();
                result.indices.clear();
                return false;
            }
            haveCurrent = true;
            currentKey = &row;
            // An ineligible row is represented as a legacy singleton and
            // cannot be joined by the following row.
            if (!row.opaque || !row.supported || row.indexCount % 3u != 0u)
                flush();
        }
        flush();
        return true;
    }

private:
    static bool SameBytes(const void* a, std::size_t aSize,
        const void* b, std::size_t bSize)
    {
        return a && b && aSize && aSize == bSize && std::memcmp(a, b, aSize) == 0;
    }

    static bool Validate(const Row& row, const char*& error)
    {
        if (row.vertexBase > static_cast<std::size_t>(
                std::numeric_limits<std::int32_t>::max()))
        {
            error = "static shadow merge base vertex exceeds signed draw range";
            return false;
        }
        if (row.indexCount > std::numeric_limits<std::uint32_t>::max())
        {
            error = "static shadow merge row index count exceeds draw range";
            return false;
        }
        if (row.indexCount && !row.indices)
        {
            error = "static shadow merge has null index span";
            return false;
        }
        if (row.vertexBase > row.vertexBufferVertexCount ||
            row.vertexCount > row.vertexBufferVertexCount - row.vertexBase)
        {
            error = "static shadow merge vertex range exceeds buffer";
            return false;
        }
        if (row.vertexBase > std::numeric_limits<std::size_t>::max() - row.vertexCount)
        {
            error = "static shadow merge vertex range overflows";
            return false;
        }
        for (std::size_t i = 0; i < row.indexCount; ++i)
        {
            if (row.indices[i] >= row.vertexCount)
            {
                error = "static shadow merge index exceeds row vertex range";
                return false;
            }
            if (row.vertexBase > static_cast<std::size_t>(
                    std::numeric_limits<std::int32_t>::max()) - row.indices[i])
            {
                error = "static shadow merge absolute vertex exceeds draw range";
                return false;
            }
        }
        return true;
    }

    static bool CanRebase(const Row& row, std::size_t baseVertex)
    {
        for (std::size_t i = 0; i < row.indexCount; ++i)
        {
            const std::size_t index = row.indices[i];
            if (row.vertexBase > std::numeric_limits<std::size_t>::max() - index)
                return false;
            const std::size_t absolute = row.vertexBase + index;
            if (absolute < baseVertex || absolute - baseVertex >
                std::numeric_limits<std::uint16_t>::max())
                return false;
        }
        return true;
    }

    static bool CheckOutputSize(std::size_t current, std::size_t additional,
        const std::vector<std::uint16_t>& output, const char*& error)
    {
        if (additional > std::numeric_limits<std::size_t>::max() - current ||
            current + additional > output.max_size())
        {
            error = "static shadow merge index output overflows";
            return false;
        }
        return true;
    }

    static bool AppendOriginal(const Row& row, std::vector<std::uint16_t>& output,
        const char*& error)
    {
        if (!CheckOutputSize(output.size(), row.indexCount, output, error)) return false;
        if (!row.indexCount) return true;
        output.insert(output.end(), row.indices, row.indices + row.indexCount);
        return true;
    }

    static bool AppendRebased(const Row& row, std::size_t baseVertex,
        std::vector<std::uint16_t>& output, const char*& error)
    {
        if (!CheckOutputSize(output.size(), row.indexCount, output, error)) return false;
        for (std::size_t i = 0; i < row.indexCount; ++i)
        {
            const std::size_t absolute = row.vertexBase + row.indices[i];
            output.push_back(static_cast<std::uint16_t>(absolute - baseVertex));
        }
        return true;
    }
};
} // namespace xray::metal
