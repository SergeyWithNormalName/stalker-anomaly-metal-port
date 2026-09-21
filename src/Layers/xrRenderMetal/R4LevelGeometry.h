#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace xray::metal
{
struct R4PositionBounds
{
	bool available = false;
	std::array<float, 3> minimum{};
	std::array<float, 3> maximum{};
};

struct R4VertexElement
{
	std::uint16_t stream = 0;
	std::uint16_t offset = 0;
	std::uint8_t type = 0;
	std::uint8_t method = 0;
	std::uint8_t usage = 0;
	std::uint8_t usageIndex = 0;
};

struct R4VertexBufferSummary
{
	std::uint32_t vertexCount = 0;
	std::uint32_t stride = 0;
	bool hasPositionFloat3 = false;
	std::uint32_t positionOffset = 0;
	std::size_t dataOffset = 0;
	std::size_t dataSize = 0;
	std::vector<R4VertexElement> declaration;
};

struct R4IndexBufferSummary
{
	std::uint32_t indexCount = 0;
	std::size_t dataOffset = 0;
	std::size_t dataSize = 0;
};

struct R4StaticVisual
{
	std::uint32_t visualId = 0;
	std::uint8_t visualType = 0;
	std::uint16_t shaderId = 0;
	std::uint32_t vertexBufferId = 0;
	std::uint32_t vertexBase = 0;
	std::uint32_t vertexCount = 0;
	std::uint32_t indexBufferId = 0;
	std::uint32_t indexBase = 0;
	std::uint32_t indexCount = 0;
	bool valid = false;
	bool hasModelTransform = false;
	std::array<float, 16> modelTransform{
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f};
	std::string rejection;
	// OGF_TREEDEF2 stores two authored _5color values immediately after the
	// 4x4 transform.  Keeping them with the static visual is required for the
	// tree c_scale/c_bias/c_sun constants; a generic value turns foliage into
	// a different lighting material.
	std::array<float, 5> treeScale{};
	std::array<float, 5> treeBias{};
	bool hasTreeLighting = false;
	R4PositionBounds referencedPositionBounds;
};

struct R4LevelShader
{
	std::string shaderName;
	std::string textureList;
	std::vector<std::string> textures;
	std::string baseTexture;
};

struct R4VisualNode
{
	std::uint32_t visualId = 0;
	std::uint8_t visualType = 0;
	std::uint16_t shaderId = 0;
	std::vector<std::uint32_t> linkedChildren;
};

struct R4LevelGeometry
{
	std::vector<R4VertexBufferSummary> vertexBuffers;
	std::vector<R4IndexBufferSummary> indexBuffers;
	std::vector<R4LevelShader> shaders;
	std::vector<R4VisualNode> visualNodes;
	std::vector<R4StaticVisual> visuals;
};

bool CollectR4StaticVisualDescendants(
	const R4LevelGeometry& geometry,
	std::uint32_t rootVisualId,
	std::vector<std::uint32_t>& staticVisualIds,
	std::string& error);

// Parses uncompressed X-Ray chunk streams.  The input vectors are deliberately
// supplied by the caller so the parser remains independent of the engine I/O layer.
bool ParseR4LevelGeometry(
	const std::vector<std::uint8_t>& levelGeometryBytes,
	const std::vector<std::uint8_t>& levelBytes,
	R4LevelGeometry& result,
	std::string& error);

bool LoadR4LevelGeometry(
	const std::filesystem::path& levelDirectory,
	R4LevelGeometry& result,
	std::string& error);

bool LoadR4LevelGeometryWithBytes(
	const std::filesystem::path& levelDirectory,
	R4LevelGeometry& result,
	std::vector<std::uint8_t>& levelGeometryBytes,
	std::string& error);

bool RunR4LevelGeometrySelfTest(std::string& error);
} // namespace xray::metal
