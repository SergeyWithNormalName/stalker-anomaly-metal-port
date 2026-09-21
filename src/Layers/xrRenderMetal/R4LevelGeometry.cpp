#include "R4LevelGeometry.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>

namespace xray::metal
{
namespace
{
constexpr std::uint32_t kCompressedChunkBit = 0x80000000u;
constexpr std::uint32_t kLevelVisualsChunk = 3u;
constexpr std::uint32_t kLevelShadersChunk = 2u;
constexpr std::uint32_t kLevelVertexBuffersChunk = 9u;
constexpr std::uint32_t kLevelIndexBuffersChunk = 10u;
constexpr std::uint32_t kOgfGeometryContainerChunk = 21u;
constexpr std::uint32_t kOgfHeaderChunk = 1u;
constexpr std::uint32_t kOgfLinkedChildrenChunk = 10u;
constexpr std::uint32_t kOgfTreeDefinitionChunk = 12u;
constexpr std::uint8_t kOgfFormatVersion = 4u;
constexpr std::uint16_t kD3dDeclEndStream = 0xffu;
constexpr std::uint8_t kD3dDeclTypeFloat3 = 2u;
constexpr std::uint8_t kD3dDeclUsagePosition = 0u;
constexpr std::size_t kVertexElementSize = 8u;
constexpr std::size_t kMaxVertexElements = 65u;

struct Chunk
{
	std::uint32_t id = 0;
	std::size_t dataBegin = 0;
	std::size_t dataEnd = 0;
};

struct VertexBufferData
{
	R4VertexBufferSummary summary;
	std::size_t dataBegin = 0;
};

struct IndexBufferData
{
	R4IndexBufferSummary summary;
	std::size_t dataBegin = 0;
};

bool checkedAdd(const std::size_t left, const std::size_t right, std::size_t& result)
{
	if (right > std::numeric_limits<std::size_t>::max() - left)
		return false;
	result = left + right;
	return true;
}

bool checkedMultiply(const std::size_t left, const std::size_t right, std::size_t& result)
{
	if (left != 0u && right > std::numeric_limits<std::size_t>::max() / left)
		return false;
	result = left * right;
	return true;
}

bool readU16(const std::vector<std::uint8_t>& bytes, const std::size_t offset, std::uint16_t& value)
{
	std::size_t end = 0;
	if (!checkedAdd(offset, 2u, end) || end > bytes.size())
		return false;
	value = static_cast<std::uint16_t>(bytes[offset])
		| static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1u]) << 8u);
	return true;
}

bool readU32(const std::vector<std::uint8_t>& bytes, const std::size_t offset, std::uint32_t& value)
{
	std::size_t end = 0;
	if (!checkedAdd(offset, 4u, end) || end > bytes.size())
		return false;
	value = static_cast<std::uint32_t>(bytes[offset])
		| (static_cast<std::uint32_t>(bytes[offset + 1u]) << 8u)
		| (static_cast<std::uint32_t>(bytes[offset + 2u]) << 16u)
		| (static_cast<std::uint32_t>(bytes[offset + 3u]) << 24u);
	return true;
}

bool readFloat(const std::vector<std::uint8_t>& bytes, const std::size_t offset, float& value)
{
	std::uint32_t bits = 0;
	if (!readU32(bytes, offset, bits))
		return false;
	static_assert(sizeof(bits) == sizeof(value), "float must be 32-bit");
	std::memcpy(&value, &bits, sizeof(value));
	return true;
}

bool readStringZ(
	const std::vector<std::uint8_t>& bytes,
	std::size_t& cursor,
	const std::size_t end,
	std::string& value)
{
	if (cursor > end || end > bytes.size())
		return false;
	const std::size_t begin = cursor;
	while (cursor < end && bytes[cursor] != 0u)
		++cursor;
	if (cursor == end)
		return false;
	value.assign(reinterpret_cast<const char*>(bytes.data() + begin), cursor - begin);
	++cursor;
	return true;
}

bool scanChunks(
	const std::vector<std::uint8_t>& bytes,
	const std::size_t begin,
	const std::size_t end,
	std::vector<Chunk>& chunks,
	std::string& error)
{
	if (begin > end || end > bytes.size())
	{
		error = "invalid chunk scan range";
		return false;
	}

	for (std::size_t cursor = begin; cursor < end;)
	{
		std::size_t headerEnd = 0;
		if (!checkedAdd(cursor, 8u, headerEnd) || headerEnd > end)
		{
			error = "truncated chunk header";
			return false;
		}

		std::uint32_t rawId = 0;
		std::uint32_t byteCount = 0;
		if (!readU32(bytes, cursor, rawId) || !readU32(bytes, cursor + 4u, byteCount))
		{
			error = "unreadable chunk header";
			return false;
		}
		if ((rawId & kCompressedChunkBit) != 0u)
		{
			error = "compressed chunks are not supported by the portable probe";
			return false;
		}

		std::size_t dataEnd = 0;
		if (!checkedAdd(headerEnd, static_cast<std::size_t>(byteCount), dataEnd) || dataEnd > end)
		{
			error = "chunk payload exceeds parent range";
			return false;
		}
		chunks.push_back({rawId, headerEnd, dataEnd});
		cursor = dataEnd;
	}
	return true;
}

const Chunk* findSingleChunk(const std::vector<Chunk>& chunks, const std::uint32_t id, std::string& error)
{
	const Chunk* found = nullptr;
	for (const Chunk& chunk : chunks)
	{
		if (chunk.id != id)
			continue;
		if (found != nullptr)
		{
			error = "duplicate required chunk " + std::to_string(id);
			return nullptr;
		}
		found = &chunk;
	}
	if (found == nullptr)
		error = "missing required chunk " + std::to_string(id);
	return found;
}

bool typeByteSize(const std::uint8_t type, std::uint32_t& size)
{
	switch (type)
	{
	case 0u: size = 4u; return true;  // FLOAT1
	case 1u: size = 8u; return true;  // FLOAT2
	case 2u: size = 12u; return true; // FLOAT3
	case 3u: size = 16u; return true; // FLOAT4
	case 4u: case 5u: case 8u: case 13u: case 14u: size = 4u; return true;
	case 6u: case 9u: case 11u: case 15u: size = 4u; return true;
	case 7u: case 10u: case 12u: case 16u: size = 8u; return true;
	default: return false;
	}
}

bool parseVertexBuffers(
	const std::vector<std::uint8_t>& bytes,
	const Chunk& chunk,
	std::vector<VertexBufferData>& vertexBuffers,
	std::string& error)
{
	std::size_t cursor = chunk.dataBegin;
	std::uint32_t count = 0;
	if (chunk.dataEnd - cursor < 4u || !readU32(bytes, cursor, count))
	{
		error = "vertex-buffer chunk is missing its count";
		return false;
	}
	cursor += 4u;
	if (count > (chunk.dataEnd - cursor) / (kVertexElementSize + 4u))
	{
		error = "vertex-buffer count cannot fit in chunk";
		return false;
	}

	vertexBuffers.clear();
	vertexBuffers.reserve(count);
	for (std::uint32_t bufferIndex = 0; bufferIndex < count; ++bufferIndex)
	{
		R4VertexBufferSummary summary;
		bool foundEnd = false;
		for (std::size_t elementIndex = 0; elementIndex < kMaxVertexElements; ++elementIndex)
		{
			std::size_t elementEnd = 0;
			if (!checkedAdd(cursor, kVertexElementSize, elementEnd) || elementEnd > chunk.dataEnd)
			{
				error = "truncated vertex declaration in buffer " + std::to_string(bufferIndex);
				return false;
			}

			std::uint16_t stream = 0;
			std::uint16_t offset = 0;
			if (!readU16(bytes, cursor, stream) || !readU16(bytes, cursor + 2u, offset))
			{
				error = "unreadable vertex declaration";
				return false;
			}
			const std::uint8_t type = bytes[cursor + 4u];
			const std::uint8_t method = bytes[cursor + 5u];
			const std::uint8_t usage = bytes[cursor + 6u];
			const std::uint8_t usageIndex = bytes[cursor + 7u];
			cursor = elementEnd;

			if (stream == kD3dDeclEndStream)
			{
				foundEnd = true;
				break;
			}
			summary.declaration.push_back({stream, offset, type, method, usage, usageIndex});

			std::uint32_t elementSize = 0;
			if (!typeByteSize(type, elementSize))
			{
				error = "unsupported D3D vertex element type in buffer " + std::to_string(bufferIndex);
				return false;
			}
			if (stream == 0u)
			{
				std::size_t elementLimit = 0;
				if (!checkedAdd(static_cast<std::size_t>(offset), elementSize, elementLimit)
					|| elementLimit > std::numeric_limits<std::uint32_t>::max())
				{
					error = "vertex element stride overflow";
					return false;
				}
				summary.stride = std::max(summary.stride, static_cast<std::uint32_t>(elementLimit));
				if (type == kD3dDeclTypeFloat3 && usage == kD3dDeclUsagePosition && usageIndex == 0u)
				{
					summary.hasPositionFloat3 = true;
					summary.positionOffset = offset;
				}
			}
		}
		if (!foundEnd)
		{
			error = "vertex declaration has no D3DDECL_END in buffer " + std::to_string(bufferIndex);
			return false;
		}
		if (summary.stride == 0u)
		{
			error = "vertex declaration has no stream 0 data in buffer " + std::to_string(bufferIndex);
			return false;
		}

		if (chunk.dataEnd - cursor < 4u || !readU32(bytes, cursor, summary.vertexCount))
		{
			error = "vertex buffer is missing its vertex count";
			return false;
		}
		cursor += 4u;
		std::size_t byteCount = 0;
		std::size_t dataEnd = 0;
		if (!checkedMultiply(summary.vertexCount, summary.stride, byteCount)
			|| !checkedAdd(cursor, byteCount, dataEnd) || dataEnd > chunk.dataEnd)
		{
			error = "vertex buffer data exceeds chunk";
			return false;
		}
		summary.dataOffset = cursor;
		summary.dataSize = byteCount;
		vertexBuffers.push_back({summary, cursor});
		cursor = dataEnd;
	}
	if (cursor != chunk.dataEnd)
	{
		error = "trailing bytes in vertex-buffer chunk";
		return false;
	}
	return true;
}

bool parseIndexBuffers(
	const std::vector<std::uint8_t>& bytes,
	const Chunk& chunk,
	std::vector<IndexBufferData>& indexBuffers,
	std::string& error)
{
	std::size_t cursor = chunk.dataBegin;
	std::uint32_t count = 0;
	if (chunk.dataEnd - cursor < 4u || !readU32(bytes, cursor, count))
	{
		error = "index-buffer chunk is missing its count";
		return false;
	}
	cursor += 4u;
	if (count > (chunk.dataEnd - cursor) / 4u)
	{
		error = "index-buffer count cannot fit in chunk";
		return false;
	}

	indexBuffers.clear();
	indexBuffers.reserve(count);
	for (std::uint32_t bufferIndex = 0; bufferIndex < count; ++bufferIndex)
	{
		R4IndexBufferSummary summary;
		if (chunk.dataEnd - cursor < 4u || !readU32(bytes, cursor, summary.indexCount))
		{
			error = "index buffer is missing its index count";
			return false;
		}
		cursor += 4u;
		std::size_t byteCount = 0;
		std::size_t dataEnd = 0;
		if (!checkedMultiply(summary.indexCount, 2u, byteCount)
			|| !checkedAdd(cursor, byteCount, dataEnd) || dataEnd > chunk.dataEnd)
		{
			error = "index buffer data exceeds chunk";
			return false;
		}
		summary.dataOffset = cursor;
		summary.dataSize = byteCount;
		indexBuffers.push_back({summary, cursor});
		cursor = dataEnd;
	}
	if (cursor != chunk.dataEnd)
	{
		error = "trailing bytes in index-buffer chunk";
		return false;
	}
	return true;
}

bool rangeFits(const std::uint32_t start, const std::uint32_t count, const std::uint32_t total)
{
	return start <= total && count <= total - start;
}

bool calculateVisualBounds(
	const std::vector<std::uint8_t>& geometryBytes,
	const VertexBufferData& vertexBuffer,
	const IndexBufferData& indexBuffer,
	R4StaticVisual& visual)
{
	if (!vertexBuffer.summary.hasPositionFloat3)
		return true;

	R4PositionBounds bounds;
	bounds.available = true;
	bool havePoint = false;
	for (std::uint32_t i = 0; i < visual.indexCount; ++i)
	{
		std::size_t indexNumber = 0;
		std::size_t indexOffset = 0;
		if (!checkedAdd(static_cast<std::size_t>(visual.indexBase), i, indexNumber)
			|| !checkedMultiply(indexNumber, 2u, indexOffset)
			|| !checkedAdd(indexBuffer.dataBegin, indexOffset, indexOffset))
		{
			visual.rejection = "index address overflow";
			return false;
		}
		std::uint16_t localIndex = 0;
		if (!readU16(geometryBytes, indexOffset, localIndex) || localIndex >= visual.vertexCount)
		{
			visual.rejection = "index is outside visual vertex range";
			return false;
		}
		const std::size_t vertexIndex = static_cast<std::size_t>(visual.vertexBase) + localIndex;
		std::size_t vertexOffset = 0;
		if (!checkedMultiply(vertexIndex, vertexBuffer.summary.stride, vertexOffset)
			|| !checkedAdd(vertexBuffer.dataBegin, vertexOffset, vertexOffset)
			|| !checkedAdd(vertexOffset, vertexBuffer.summary.positionOffset, vertexOffset))
		{
			visual.rejection = "vertex address overflow";
			return false;
		}

		std::array<float, 3> position{};
		for (std::size_t axis = 0; axis < position.size(); ++axis)
		{
			if (!readFloat(geometryBytes, vertexOffset + axis * sizeof(float), position[axis]) || !std::isfinite(position[axis]))
			{
				visual.rejection = "non-finite or truncated POSITION0";
				return false;
			}
		}
		if (!havePoint)
		{
			bounds.minimum = position;
			bounds.maximum = position;
			havePoint = true;
		}
		else
		{
			for (std::size_t axis = 0; axis < position.size(); ++axis)
			{
				bounds.minimum[axis] = std::min(bounds.minimum[axis], position[axis]);
				bounds.maximum[axis] = std::max(bounds.maximum[axis], position[axis]);
			}
		}
	}
	visual.referencedPositionBounds = bounds;
	return true;
}

void appendU16(std::vector<std::uint8_t>& bytes, const std::uint16_t value)
{
	bytes.push_back(static_cast<std::uint8_t>(value & 0xffu));
	bytes.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}

void appendU32(std::vector<std::uint8_t>& bytes, const std::uint32_t value)
{
	bytes.push_back(static_cast<std::uint8_t>(value & 0xffu));
	bytes.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
	bytes.push_back(static_cast<std::uint8_t>((value >> 16u) & 0xffu));
	bytes.push_back(static_cast<std::uint8_t>((value >> 24u) & 0xffu));
}

void appendFloat(std::vector<std::uint8_t>& bytes, const float value)
{
	std::uint32_t bits = 0;
	std::memcpy(&bits, &value, sizeof(bits));
	appendU32(bytes, bits);
}

void appendChunk(std::vector<std::uint8_t>& destination, const std::uint32_t id, const std::vector<std::uint8_t>& payload)
{
	appendU32(destination, id);
	appendU32(destination, static_cast<std::uint32_t>(payload.size()));
	destination.insert(destination.end(), payload.begin(), payload.end());
}
} // namespace

bool ParseR4LevelGeometry(
	const std::vector<std::uint8_t>& levelGeometryBytes,
	const std::vector<std::uint8_t>& levelBytes,
	R4LevelGeometry& result,
	std::string& error)
{
	result = {};
	error.clear();
	std::vector<Chunk> geometryChunks;
	if (!scanChunks(levelGeometryBytes, 0u, levelGeometryBytes.size(), geometryChunks, error))
		return false;
	const Chunk* vertexChunk = findSingleChunk(geometryChunks, kLevelVertexBuffersChunk, error);
	if (vertexChunk == nullptr)
		return false;
	const Chunk* indexChunk = findSingleChunk(geometryChunks, kLevelIndexBuffersChunk, error);
	if (indexChunk == nullptr)
		return false;

	std::vector<VertexBufferData> vertexBuffers;
	std::vector<IndexBufferData> indexBuffers;
	if (!parseVertexBuffers(levelGeometryBytes, *vertexChunk, vertexBuffers, error)
		|| !parseIndexBuffers(levelGeometryBytes, *indexChunk, indexBuffers, error))
		return false;
	for (const VertexBufferData& buffer : vertexBuffers)
		result.vertexBuffers.push_back(buffer.summary);
	for (const IndexBufferData& buffer : indexBuffers)
		result.indexBuffers.push_back(buffer.summary);

	std::vector<Chunk> levelChunks;
	if (!scanChunks(levelBytes, 0u, levelBytes.size(), levelChunks, error))
		return false;
	const Chunk* shadersChunk = findSingleChunk(levelChunks, kLevelShadersChunk, error);
	if (shadersChunk == nullptr)
		return false;
	std::size_t shaderCursor = shadersChunk->dataBegin;
	std::uint32_t shaderCount = 0;
	if (shadersChunk->dataEnd - shaderCursor < 4u ||
		!readU32(levelBytes, shaderCursor, shaderCount))
	{
		error = "shader chunk is missing its count";
		return false;
	}
	shaderCursor += 4u;
	if (shaderCount > shadersChunk->dataEnd - shaderCursor)
	{
		error = "shader count cannot fit in chunk";
		return false;
	}
	result.shaders.reserve(shaderCount);
	for (std::uint32_t shaderIndex = 0; shaderIndex < shaderCount; ++shaderIndex)
	{
		std::string entry;
		if (!readStringZ(levelBytes, shaderCursor, shadersChunk->dataEnd, entry))
		{
			error = "unterminated shader entry " + std::to_string(shaderIndex);
			return false;
		}
		R4LevelShader shader;
		if (!entry.empty())
		{
			const std::size_t separator = entry.find('/');
			if (separator == std::string::npos || separator == 0u || separator + 1u == entry.size())
			{
				error = "malformed shader entry " + std::to_string(shaderIndex);
				return false;
			}
			shader.shaderName = entry.substr(0u, separator);
			shader.textureList = entry.substr(separator + 1u);
			for (std::size_t begin = 0u; begin <= shader.textureList.size();)
			{
				const std::size_t comma = shader.textureList.find(',', begin);
				const std::size_t end = comma == std::string::npos ? shader.textureList.size() : comma;
				shader.textures.emplace_back(shader.textureList.substr(begin, end - begin));
				if (comma == std::string::npos)
					break;
				begin = comma + 1u;
			}
			if (shader.textures.empty() ||
				std::any_of(shader.textures.begin(), shader.textures.end(),
					[](const std::string& texture) { return texture.empty(); }))
			{
				error = "shader entry has an empty texture " + std::to_string(shaderIndex);
				return false;
			}
			shader.baseTexture = shader.textures.front();
		}
		result.shaders.emplace_back(std::move(shader));
	}
	if (shaderCursor != shadersChunk->dataEnd)
	{
		error = "trailing bytes in shader chunk";
		return false;
	}
	const Chunk* visualsChunk = findSingleChunk(levelChunks, kLevelVisualsChunk, error);
	if (visualsChunk == nullptr)
		return false;

	std::vector<Chunk> visualChunks;
	if (!scanChunks(levelBytes, visualsChunk->dataBegin, visualsChunk->dataEnd, visualChunks, error))
		return false;
	for (const Chunk& visualChunk : visualChunks)
	{
		std::vector<Chunk> ogfChunks;
		if (!scanChunks(levelBytes, visualChunk.dataBegin, visualChunk.dataEnd, ogfChunks, error))
			return false;
		const Chunk* geometryContainer = nullptr;
		const Chunk* header = nullptr;
		const Chunk* linkedChildren = nullptr;
		const Chunk* treeDefinition = nullptr;
		for (const Chunk& ogfChunk : ogfChunks)
		{
			const Chunk** target = nullptr;
			if (ogfChunk.id == kOgfGeometryContainerChunk)
				target = &geometryContainer;
			else if (ogfChunk.id == kOgfHeaderChunk)
				target = &header;
			else if (ogfChunk.id == kOgfLinkedChildrenChunk)
				target = &linkedChildren;
			else if (ogfChunk.id == kOgfTreeDefinitionChunk)
				target = &treeDefinition;
			if (!target)
				continue;
			if (*target != nullptr)
			{
				error = "duplicate required OGF chunk in visual " + std::to_string(visualChunk.id);
				return false;
			}
			*target = &ogfChunk;
		}
		if (header == nullptr || header->dataEnd - header->dataBegin < 44u ||
			levelBytes[header->dataBegin] != kOgfFormatVersion)
		{
			error = "missing or invalid OGF_HEADER in visual " + std::to_string(visualChunk.id);
			return false;
		}
		R4VisualNode node;
		node.visualId = visualChunk.id;
		node.visualType = levelBytes[header->dataBegin + 1u];
		if (!readU16(levelBytes, header->dataBegin + 2u, node.shaderId))
		{
			error = "unreadable OGF_HEADER shader id";
			return false;
		}
		if (linkedChildren)
		{
			std::size_t childCursor = linkedChildren->dataBegin;
			std::uint32_t childCount = 0;
			if (linkedChildren->dataEnd - childCursor < 4u ||
				!readU32(levelBytes, childCursor, childCount))
			{
				error = "linked child chunk is missing its count";
				return false;
			}
			childCursor += 4u;
			std::size_t childBytes = 0;
			if (!checkedMultiply(childCount, 4u, childBytes) ||
				childBytes != linkedChildren->dataEnd - childCursor)
			{
				error = "linked child count does not match chunk size";
				return false;
			}
			node.linkedChildren.reserve(childCount);
			for (std::uint32_t childIndex = 0; childIndex < childCount; ++childIndex)
			{
				std::uint32_t childId = 0;
				if (!readU32(levelBytes, childCursor, childId))
				{
					error = "unreadable linked child id";
					return false;
				}
				childCursor += 4u;
				node.linkedChildren.push_back(childId);
			}
		}
		result.visualNodes.emplace_back(std::move(node));

		if (geometryContainer == nullptr)
			continue;
		if (geometryContainer->dataEnd - geometryContainer->dataBegin != 24u)
		{
			error = "OGF_GCONTAINER must contain exactly six u32 values";
			return false;
		}

		R4StaticVisual visual;
		visual.visualId = visualChunk.id;
		visual.visualType = result.visualNodes.back().visualType;
		visual.shaderId = result.visualNodes.back().shaderId;
		if (visual.visualType == 7u || visual.visualType == 11u)
		{
			if (!treeDefinition || treeDefinition->dataEnd - treeDefinition->dataBegin < 64u)
			{
				error = "tree visual " + std::to_string(visual.visualId) +
					" is missing its 4x4 model transform";
				return false;
			}
			for (std::size_t element = 0; element < visual.modelTransform.size(); ++element)
			{
				float value = 0.0f;
				std::memcpy(&value,
					levelBytes.data() + treeDefinition->dataBegin + element * sizeof(float),
					sizeof(value));
				if (!std::isfinite(value))
				{
					error = "tree visual model transform contains a non-finite value";
					return false;
				}
				visual.modelTransform[element] = value;
			}
			visual.hasModelTransform = true;
			// FTreeVisual::Load reads _5color c_scale/c_bias directly after
			// the transform and halves them before its R4 render-time correction.
			// Preserve the source values here; the Metal flora adapter applies the
			// same transform plus live r__d_tree_SBC.
			constexpr std::size_t TreeLightingOffset = 64u;
			constexpr std::size_t TreeLightingBytes = sizeof(float) * 10u;
			if (treeDefinition->dataEnd - treeDefinition->dataBegin <
				TreeLightingOffset + TreeLightingBytes)
			{
				error = "tree visual " + std::to_string(visual.visualId) +
					" is missing OGF_TREEDEF2 scale/bias lighting";
				return false;
			}
			for (std::size_t index = 0; index < visual.treeScale.size(); ++index)
			{
				std::memcpy(&visual.treeScale[index], levelBytes.data() +
					treeDefinition->dataBegin + TreeLightingOffset + index * sizeof(float),
					sizeof(float));
				std::memcpy(&visual.treeBias[index], levelBytes.data() +
					treeDefinition->dataBegin + TreeLightingOffset +
					visual.treeScale.size() * sizeof(float) + index * sizeof(float),
					sizeof(float));
				if (!std::isfinite(visual.treeScale[index]) || !std::isfinite(visual.treeBias[index]))
				{
					error = "tree visual " + std::to_string(visual.visualId) +
						" has non-finite OGF_TREEDEF2 scale/bias lighting";
					return false;
				}
			}
			visual.hasTreeLighting = true;
		}
		if (!readU32(levelBytes, geometryContainer->dataBegin, visual.vertexBufferId)
			|| !readU32(levelBytes, geometryContainer->dataBegin + 4u, visual.vertexBase)
			|| !readU32(levelBytes, geometryContainer->dataBegin + 8u, visual.vertexCount)
			|| !readU32(levelBytes, geometryContainer->dataBegin + 12u, visual.indexBufferId)
			|| !readU32(levelBytes, geometryContainer->dataBegin + 16u, visual.indexBase)
			|| !readU32(levelBytes, geometryContainer->dataBegin + 20u, visual.indexCount))
		{
			error = "unreadable OGF_GCONTAINER";
			return false;
		}

		if (visual.vertexBufferId >= vertexBuffers.size() || visual.indexBufferId >= indexBuffers.size())
			visual.rejection = "container buffer id is out of range";
		else if (!rangeFits(visual.vertexBase, visual.vertexCount, vertexBuffers[visual.vertexBufferId].summary.vertexCount))
			visual.rejection = "vertex range exceeds vertex buffer";
		else if (!rangeFits(visual.indexBase, visual.indexCount, indexBuffers[visual.indexBufferId].summary.indexCount))
			visual.rejection = "index range exceeds index buffer";
		else if (visual.shaderId == 0u || visual.shaderId >= result.shaders.size() ||
			result.shaders[visual.shaderId].baseTexture.empty())
			visual.rejection = "visual shader id is unresolved";
		else if (!calculateVisualBounds(levelGeometryBytes, vertexBuffers[visual.vertexBufferId], indexBuffers[visual.indexBufferId], visual))
			; // calculateVisualBounds records the precise rejection.
		else
			visual.valid = true;
		result.visuals.push_back(std::move(visual));
	}
	if (result.visualNodes.size() != visualChunks.size())
	{
		error = "visual node count mismatch";
		return false;
	}
	for (std::size_t nodeIndex = 0; nodeIndex < result.visualNodes.size(); ++nodeIndex)
	{
		const R4VisualNode& node = result.visualNodes[nodeIndex];
		if (node.visualId != nodeIndex)
		{
			error = "level visual ids are not dense and sequential";
			return false;
		}
		for (const std::uint32_t childId : node.linkedChildren)
		{
			if (childId >= result.visualNodes.size())
			{
				error = "linked child visual id is out of range";
				return false;
			}
		}
	}

	return true;
}

bool CollectR4StaticVisualDescendants(
	const R4LevelGeometry& geometry,
	const std::uint32_t rootVisualId,
	std::vector<std::uint32_t>& staticVisualIds,
	std::string& error)
{
	staticVisualIds.clear();
	error.clear();
	if (rootVisualId >= geometry.visualNodes.size())
	{
		error = "root visual id is out of range";
		return false;
	}
	std::vector<const R4StaticVisual*> staticById(geometry.visualNodes.size(), nullptr);
	for (const R4StaticVisual& visual : geometry.visuals)
	{
		if (visual.visualId >= staticById.size() || staticById[visual.visualId])
		{
			error = "duplicate or out-of-range static visual id";
			return false;
		}
		staticById[visual.visualId] = &visual;
	}
	struct VisitFrame
	{
		std::uint32_t visualId;
		std::size_t nextChild = 0;
	};
	std::vector<std::uint8_t> visitState(geometry.visualNodes.size(), 0u);
	std::vector<VisitFrame> stack;
	visitState[rootVisualId] = 1u;
	stack.push_back({rootVisualId});
	while (!stack.empty())
	{
		VisitFrame& frame = stack.back();
		const R4VisualNode& node = geometry.visualNodes[frame.visualId];
		if (node.linkedChildren.empty())
		{
			if (staticById[frame.visualId] && staticById[frame.visualId]->valid)
				staticVisualIds.push_back(frame.visualId);
			visitState[frame.visualId] = 2u;
			stack.pop_back();
			continue;
		}
		if (frame.nextChild == node.linkedChildren.size())
		{
			visitState[frame.visualId] = 2u;
			stack.pop_back();
			continue;
		}
		const std::uint32_t childId = node.linkedChildren[frame.nextChild++];
		if (visitState[childId] == 1u)
		{
			error = "cycle in linked visual hierarchy";
			return false;
		}
		if (visitState[childId] == 0u)
		{
			visitState[childId] = 1u;
			stack.push_back({childId});
		}
	}
	if (staticVisualIds.empty())
	{
		error = "visual hierarchy has no valid static geometry descendants";
		return false;
	}
	return true;
}

bool LoadR4LevelGeometry(
	const std::filesystem::path& levelDirectory,
	R4LevelGeometry& result,
	std::string& error)
{
	std::vector<std::uint8_t> geometryBytes;
	return LoadR4LevelGeometryWithBytes(levelDirectory, result, geometryBytes, error);
}

bool LoadR4LevelGeometryWithBytes(
	const std::filesystem::path& levelDirectory,
	R4LevelGeometry& result,
	std::vector<std::uint8_t>& geometryBytes,
	std::string& error)
{
	auto readFile = [&error](const std::filesystem::path& path, std::vector<std::uint8_t>& bytes) -> bool
	{
		std::ifstream input(path, std::ios::binary | std::ios::ate);
		if (!input)
		{
			error = "cannot open " + path.string();
			return false;
		}
		const std::ifstream::pos_type length = input.tellg();
		if (length < 0 || static_cast<std::uintmax_t>(length) > std::numeric_limits<std::size_t>::max()
			|| static_cast<std::uintmax_t>(length) > std::numeric_limits<std::streamsize>::max())
		{
			error = "invalid file size for " + path.string();
			return false;
		}
		bytes.resize(static_cast<std::size_t>(length));
		input.seekg(0, std::ios::beg);
		if (!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
		{
			error = "cannot read " + path.string();
			return false;
		}
		return true;
	};

	std::vector<std::uint8_t> levelBytes;
	if (!readFile(levelDirectory / "level.geom", geometryBytes) || !readFile(levelDirectory / "level", levelBytes))
		return false;
	return ParseR4LevelGeometry(geometryBytes, levelBytes, result, error);
}

bool RunR4LevelGeometrySelfTest(std::string& error)
{
	std::vector<std::uint8_t> vertexPayload;
	appendU32(vertexPayload, 1u);
	appendU16(vertexPayload, 0u);
	appendU16(vertexPayload, 0u);
	vertexPayload.insert(vertexPayload.end(), {2u, 0u, 0u, 0u});
	appendU16(vertexPayload, 0xffu);
	appendU16(vertexPayload, 0u);
	vertexPayload.insert(vertexPayload.end(), {17u, 0u, 0u, 0u});
	appendU32(vertexPayload, 3u);
	appendFloat(vertexPayload, 0.0f); appendFloat(vertexPayload, 0.0f); appendFloat(vertexPayload, 0.0f);
	appendFloat(vertexPayload, 1.0f); appendFloat(vertexPayload, 0.0f); appendFloat(vertexPayload, 0.0f);
	appendFloat(vertexPayload, 0.0f); appendFloat(vertexPayload, 1.0f); appendFloat(vertexPayload, 0.0f);

	std::vector<std::uint8_t> indexPayload;
	appendU32(indexPayload, 1u);
	appendU32(indexPayload, 3u);
	appendU16(indexPayload, 0u); appendU16(indexPayload, 1u); appendU16(indexPayload, 2u);

	std::vector<std::uint8_t> geometryBytes;
	appendChunk(geometryBytes, kLevelVertexBuffersChunk, vertexPayload);
	appendChunk(geometryBytes, kLevelIndexBuffersChunk, indexPayload);

	std::vector<std::uint8_t> geometryContainer;
	appendU32(geometryContainer, 0u); appendU32(geometryContainer, 0u); appendU32(geometryContainer, 3u);
	appendU32(geometryContainer, 0u); appendU32(geometryContainer, 0u); appendU32(geometryContainer, 3u);
	std::vector<std::uint8_t> visual;
	std::vector<std::uint8_t> header(44u, 0u);
	header[0] = kOgfFormatVersion;
	header[1] = 7u;
	header[2] = 1u;
	appendChunk(visual, kOgfHeaderChunk, header);
	std::vector<std::uint8_t> treeDefinition;
	for (std::size_t element = 0; element < 16u; ++element)
	{
		float value = element % 5u == 0u ? 1.0f : 0.0f;
		if (element == 12u) value = 2.0f;
		if (element == 13u) value = 3.0f;
		if (element == 14u) value = 4.0f;
		appendFloat(treeDefinition, value);
	}
	treeDefinition.resize(96u, 0u);
	appendChunk(visual, kOgfTreeDefinitionChunk, treeDefinition);
	appendChunk(visual, kOgfGeometryContainerChunk, geometryContainer);
	std::vector<std::uint8_t> visuals;
	appendChunk(visuals, 0u, visual);
	std::vector<std::uint8_t> hierarchy;
	std::vector<std::uint8_t> hierarchyHeader(44u, 0u);
	hierarchyHeader[0] = kOgfFormatVersion;
	hierarchyHeader[1] = 1u;
	appendChunk(hierarchy, kOgfHeaderChunk, hierarchyHeader);
	std::vector<std::uint8_t> children;
	appendU32(children, 1u);
	appendU32(children, 0u);
	appendChunk(hierarchy, kOgfLinkedChildrenChunk, children);
	appendChunk(visuals, 1u, hierarchy);
	std::vector<std::uint8_t> shaders;
	appendU32(shaders, 2u);
	shaders.push_back(0u);
	const char shaderEntry[] = "default/test\\base,test_lmap,test_hemi";
	shaders.insert(shaders.end(), shaderEntry, shaderEntry + sizeof(shaderEntry));
	std::vector<std::uint8_t> levelBytes;
	appendChunk(levelBytes, kLevelShadersChunk, shaders);
	appendChunk(levelBytes, kLevelVisualsChunk, visuals);

	R4LevelGeometry parsed;
	if (!ParseR4LevelGeometry(geometryBytes, levelBytes, parsed, error))
		return false;
	if (parsed.vertexBuffers.size() != 1u || parsed.indexBuffers.size() != 1u || parsed.shaders.size() != 2u ||
		parsed.shaders[1].baseTexture != "test\\base" || parsed.shaders[1].textures.size() != 3u ||
		parsed.shaders[1].textures[1] != "test_lmap" ||
		parsed.shaders[1].textures[2] != "test_hemi" || parsed.visuals.size() != 1u
		|| parsed.visualNodes.size() != 2u || parsed.visualNodes[1].linkedChildren != std::vector<std::uint32_t>{0u}
		|| !parsed.visuals.front().valid || !parsed.visuals.front().hasModelTransform
		|| parsed.visuals.front().modelTransform[12] != 2.0f
		|| parsed.visuals.front().modelTransform[13] != 3.0f
		|| parsed.visuals.front().modelTransform[14] != 4.0f
		|| !parsed.visuals.front().referencedPositionBounds.available
		|| parsed.visuals.front().referencedPositionBounds.maximum[0] != 1.0f
		|| parsed.visuals.front().referencedPositionBounds.maximum[1] != 1.0f)
	{
		error = "valid geometry self-test did not produce the expected summary";
		return false;
	}
	std::vector<std::uint32_t> descendants;
	if (!CollectR4StaticVisualDescendants(parsed, 1u, descendants, error) ||
		descendants != std::vector<std::uint32_t>{0u})
	{
		if (error.empty())
			error = "linked hierarchy self-test did not collect its static leaf";
		return false;
	}
	R4LevelGeometry cyclic = parsed;
	cyclic.visualNodes[1].linkedChildren = {1u};
	if (CollectR4StaticVisualDescendants(cyclic, 1u, descendants, error) ||
		error != "cycle in linked visual hierarchy")
	{
		error = "linked hierarchy cycle self-test was not rejected";
		return false;
	}
	error.clear();
	if (CollectR4StaticVisualDescendants(parsed, 2u, descendants, error) ||
		error != "root visual id is out of range")
	{
		error = "out-of-range hierarchy root self-test was not rejected";
		return false;
	}
	error.clear();

	geometryBytes.back() = 3u;
	if (!ParseR4LevelGeometry(geometryBytes, levelBytes, parsed, error))
		return false;
	if (parsed.visuals.size() != 1u || parsed.visuals.front().valid || parsed.visuals.front().rejection.empty())
	{
		error = "out-of-range index self-test was not rejected";
		return false;
	}
	error.clear();
	return true;
}
} // namespace xray::metal
