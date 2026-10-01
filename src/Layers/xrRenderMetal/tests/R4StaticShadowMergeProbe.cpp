#include "../R4StaticShadowMerge.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace
{
using Merge = xray::metal::R4StaticShadowMerge;
using Row = Merge::Row;
using Result = Merge::Result;

constexpr std::array<std::uint32_t, 4> ModelA{{1u, 2u, 3u, 4u}};
constexpr std::array<std::uint32_t, 4> ModelB{{1u, 2u, 3u, 5u}};
constexpr std::array<std::uint32_t, 4> WvpA{{9u, 8u, 7u, 6u}};
constexpr std::array<std::uint32_t, 4> WvpB{{9u, 8u, 7u, 5u}};

template <typename Model = std::array<std::uint32_t, 4>,
    typename Wvp = std::array<std::uint32_t, 4>>
Row makeRow(std::size_t vb, const std::uint16_t* indices, std::size_t indexCount,
    std::size_t base, std::size_t vertices, std::size_t bufferVertices,
    const Model& model = ModelA, const Wvp& wvp = WvpA,
    bool admitted = true, bool opaque = true, bool supported = true)
{
    Row row;
    row.vertexBufferId = vb;
    row.indices = indices;
    row.indexCount = indexCount;
    row.vertexBase = base;
    row.vertexCount = vertices;
    row.vertexBufferVertexCount = bufferVertices;
    row.modelBytes = model.data();
    row.modelByteCount = sizeof(model);
    row.wvpBytes = wvp.data();
    row.wvpByteCount = sizeof(wvp);
    row.admitted = admitted;
    row.opaque = opaque;
    row.supported = supported;
    return row;
}

bool fail(std::string& error, const char* message)
{
    error = message;
    return false;
}

bool verifyReconstruction(const std::vector<Row>& rows, const Result& result,
    std::string& error)
{
    std::size_t outputCursor = 0;
    std::size_t sourceCursor = 0;
    for (const auto& run : result.runs)
    {
        if (run.firstRow < sourceCursor || run.firstRow + run.rowCount > rows.size())
            return fail(error, "run source range is invalid or reordered");
        while (sourceCursor < run.firstRow)
        {
            if (rows[sourceCursor].admitted)
                return fail(error, "an admitted source row was omitted");
            ++sourceCursor;
        }
        if (run.indexOffset != outputCursor)
            return fail(error, "run index offsets are not contiguous");
        std::size_t expectedCount = 0;
        for (std::size_t r = 0; r < run.rowCount; ++r)
        {
            const Row& source = rows[run.firstRow + r];
            if (!source.admitted)
                return fail(error, "a run crosses a rejected source row");
            if (source.indexCount > std::numeric_limits<std::size_t>::max() - expectedCount)
                return fail(error, "test expected index count overflow");
            expectedCount += source.indexCount;
            for (std::size_t i = 0; i < source.indexCount; ++i)
            {
                if (outputCursor >= result.indices.size())
                    return fail(error, "output index stream ended early");
                const std::size_t got = run.baseVertex + result.indices[outputCursor++];
                const std::size_t expected = source.vertexBase + source.indices[i];
                if (got != expected)
                    return fail(error, "rebased output changed an absolute vertex ID");
            }
            ++sourceCursor;
        }
        if (run.indexCount != expectedCount)
            return fail(error, "run index count does not match source rows");
    }
    while (sourceCursor < rows.size())
    {
        if (rows[sourceCursor].admitted)
            return fail(error, "trailing admitted source row was omitted");
        ++sourceCursor;
    }
    if (outputCursor != result.indices.size())
        return fail(error, "output contains unused indices");
    return true;
}

bool build(const std::vector<Row>& rows, Result& result, std::string& error)
{
    const char* why = nullptr;
    if (!Merge::Build(rows.data(), rows.size(), result, why))
    {
        error = why ? why : "merge failed without an error";
        return false;
    }
    return verifyReconstruction(rows, result, error);
}

bool testOrderAndCoplanarRows(std::string& error)
{
    // Deliberately overlapping/coplanar synthetic triangles. The merge must
    // retain the input triangle and material-row order byte-for-byte in IDs.
    static constexpr std::uint16_t i0[] = {0, 1, 2};
    static constexpr std::uint16_t i1[] = {2, 1, 3};
    static constexpr std::uint16_t i2[] = {4, 5, 6};
    const std::vector<Row> rows{
        makeRow(3, i0, 3, 20, 8, 128),
        makeRow(3, i1, 3, 22, 8, 128),
        makeRow(3, i2, 3, 20, 8, 128),
    };
    Result result;
    if (!build(rows, result, error)) return false;
    if (result.runs.size() != 1 || result.runs[0].firstRow != 0 ||
        result.runs[0].rowCount != 3 || !result.runs[0].merged ||
        result.runs[0].baseVertex != 20 ||
        result.indices != std::vector<std::uint16_t>({0, 1, 2, 4, 3, 5, 4, 5, 6}))
        return fail(error, "compatible adjacent rows did not merge in exact source order");
    return true;
}

bool testCompatibilityBreaks(std::string& error)
{
    static constexpr std::uint16_t tri[] = {0, 1, 2};
    static constexpr std::uint16_t triListWithRemainder[] = {0, 1, 2, 3};
    const std::vector<Row> rows{
        makeRow(4, tri, 3, 0, 8, 32),
        makeRow(4, tri, 3, 0, 8, 32),
        makeRow(4, tri, 3, 0, 8, 32, ModelB), // model bytes differ
        makeRow(4, tri, 3, 0, 8, 32, ModelA, WvpB), // WVP bytes differ
        makeRow(5, tri, 3, 0, 8, 32), // vertex buffer differs
        makeRow(5, tri, 3, 0, 8, 32, ModelA, WvpA, true, false), // alpha
        makeRow(5, tri, 3, 0, 8, 32),
        makeRow(5, tri, 3, 0, 8, 32),
        makeRow(5, tri, 3, 0, 8, 32, ModelA, WvpA, true, true, false), // unsupported
        makeRow(5, tri, 3, 0, 8, 32),
        makeRow(5, triListWithRemainder, 4, 0, 8, 32), // no cross-draw triangle join
        makeRow(5, tri, 3, 0, 8, 32),
        makeRow(5, tri, 3, 0, 8, 32),
    };
    Result result;
    if (!build(rows, result, error)) return false;
    const std::array<std::size_t, 10> firstRows{{0, 2, 3, 4, 5, 6, 8, 9, 10, 11}};
    const std::array<std::size_t, 10> rowCounts{{2, 1, 1, 1, 1, 2, 1, 1, 1, 2}};
    if (result.runs.size() != firstRows.size())
        return fail(error, "model/VB/alpha/unsupported changes failed to split runs");
    for (std::size_t i = 0; i < result.runs.size(); ++i)
        if (result.runs[i].firstRow != firstRows[i] ||
            result.runs[i].rowCount != rowCounts[i])
            return fail(error, "compatibility split changed source-run order");
    if (result.runs[4].merged || result.runs[6].merged || result.runs[8].merged ||
        result.runs[9].firstRow != 11)
        return fail(error, "alpha/unsupported/triangle-remainder row was not a legacy singleton");

    // A rejected source row is an adjacency barrier even though it emits no draw.
    const std::vector<Row> withGap{
        makeRow(4, tri, 3, 0, 8, 32),
        makeRow(4, nullptr, 0, 0, 0, 32, ModelA, WvpA, false),
        makeRow(4, tri, 3, 0, 8, 32),
    };
    if (!build(withGap, result, error)) return false;
    if (result.runs.size() != 2 || result.runs[0].firstRow != 0 ||
        result.runs[1].firstRow != 2 || result.runs[0].rowCount != 1 ||
        result.runs[1].rowCount != 1)
        return fail(error, "merge crossed a non-admitted source-row gap");
    return true;
}

bool testIndexBoundaries(std::string& error)
{
    static constexpr std::uint16_t firstTriangle[] = {0, 1, 2};
    static constexpr std::uint16_t edgeTriangle[] = {1, 0, 1};
    static constexpr std::uint16_t nextTriangle[] = {0, 0, 0};
    static constexpr std::uint16_t zeroTriangle[] = {0, 0, 0};
    const std::vector<Row> rows{
        makeRow(9, firstTriangle, 3, 0, 65536, 65536),
        makeRow(9, edgeTriangle, 3, 65534, 2, 65536), // rebases to exactly 65535
        makeRow(9, nextTriangle, 3, 65536, 1, 65537), // delta 65536: split
    };
    Result result;
    if (!build(rows, result, error)) return false;
    if (result.runs.size() != 2 || result.runs[0].rowCount != 2 ||
        result.indices[3] != std::numeric_limits<std::uint16_t>::max() ||
        result.runs[1].firstRow != 2 || result.runs[1].baseVertex != 65536)
        return fail(error, "65535 boundary merge/split is incorrect");

    // The second row has a valid original address below the chosen first-row
    // base. It must start a new run rather than underflow during rebasing.
    const std::vector<Row> negative{
        makeRow(10, zeroTriangle, 3, 100, 4, 200),
        makeRow(10, zeroTriangle, 3, 99, 4, 200),
    };
    if (!build(negative, result, error)) return false;
    if (result.runs.size() != 2 || result.runs[1].baseVertex != 99)
        return fail(error, "negative rebase did not conservatively split");
    return true;
}

bool expectInvalid(const Row& row, const char* expected, std::string& error)
{
    Result result;
    result.runs.push_back({});
    result.indices.push_back(77);
    const char* why = nullptr;
    if (Merge::Build(&row, 1, result, why))
        return fail(error, "invalid row was accepted");
    if (!why || std::strstr(why, expected) == nullptr)
        return fail(error, "invalid row reported an unexpected error");
    if (!result.runs.empty() || !result.indices.empty())
        return fail(error, "failed plan left partially usable output");
    return true;
}

bool testRangeFailures(std::string& error)
{
    static constexpr std::uint16_t outOfRange[] = {2};
    const Row badIndex = makeRow(1, outOfRange, 1, 0, 2, 2);
    if (!expectInvalid(badIndex, "index exceeds", error)) return false;

    static constexpr std::uint16_t zero[] = {0};
    const Row badVertexRange = makeRow(1, zero, 1, 9, 2, 10);
    if (!expectInvalid(badVertexRange, "vertex range exceeds", error)) return false;

    const Row nullIndices = makeRow(1, nullptr, 1, 0, 1, 1);
    if (!expectInvalid(nullIndices, "null index span", error)) return false;

    const Row overflow = makeRow(1, nullptr, 0,
        std::numeric_limits<std::size_t>::max(), 1,
        std::numeric_limits<std::size_t>::max());
    if (!expectInvalid(overflow, "base vertex exceeds", error)) return false;

    const std::size_t tooManyIndices =
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) + 1u;
    const Row oversizedDraw = makeRow(1, nullptr, tooManyIndices, 0, 1, 1);
    if (!expectInvalid(oversizedDraw, "row index count exceeds", error)) return false;

    static constexpr std::uint16_t one[] = {1};
    const std::size_t signedLimit = static_cast<std::size_t>(
        std::numeric_limits<std::int32_t>::max());
    const Row absoluteOverflow = makeRow(1, one, 1, signedLimit, 2, signedLimit + 2u);
    if (!expectInvalid(absoluteOverflow, "absolute vertex exceeds", error)) return false;

    static constexpr std::uint16_t signedEdgeTriangle[] = {0, 1, 2};
    const std::size_t signedEdgeBase = signedLimit - 2u;
    const std::vector<Row> signedEdge{
        makeRow(2, signedEdgeTriangle, 3, signedEdgeBase, 3, signedLimit + 1u),
    };
    Result edgeResult;
    if (!build(signedEdge, edgeResult, error)) return false;
    if (edgeResult.runs.size() != 1 || edgeResult.runs[0].baseVertex != signedEdgeBase ||
        edgeResult.runs[0].indexCount != 3 || edgeResult.indices.size() != 3 ||
        signedEdgeBase + edgeResult.indices[2] != signedLimit)
        return fail(error, "INT32_MAX absolute vertex was not accepted/reconstructed");

    static constexpr std::uint16_t signedOverflowTriangle[] = {0, 1, 3};
    const Row signedOverflow = makeRow(2, signedOverflowTriangle, 3,
        signedEdgeBase, 4, signedLimit + 2u);
    if (!expectInvalid(signedOverflow, "absolute vertex exceeds", error)) return false;

    const char* why = nullptr;
    Result result;
    if (Merge::Build(nullptr, 1, result, why) || !why ||
        std::strstr(why, "null row array") == nullptr)
        return fail(error, "null row array was not rejected");
    return true;
}

bool runSelfTest(std::string& error)
{
    return testOrderAndCoplanarRows(error) &&
        testCompatibilityBreaks(error) &&
        testIndexBoundaries(error) &&
        testRangeFailures(error);
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2 || std::string(argv[1]) != "--self-test")
    {
        std::fprintf(stderr, "usage: R4StaticShadowMergeProbe --self-test\n");
        return 2;
    }
    std::string error;
    if (runSelfTest(error)) return 0;
    std::fprintf(stderr, "static shadow merge self-test failed: %s\n", error.c_str());
    return 1;
}
