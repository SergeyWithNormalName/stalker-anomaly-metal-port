#include "../R4LevelGeometry.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <locale>
#include <map>
#include <cmath>
#include <string>

namespace
{
void printBounds(const xray::metal::R4PositionBounds& bounds)
{
	std::cout << std::setprecision(9)
		<< " min=" << bounds.minimum[0] << ',' << bounds.minimum[1] << ',' << bounds.minimum[2]
		<< " max=" << bounds.maximum[0] << ',' << bounds.maximum[1] << ',' << bounds.maximum[2];
}

bool parsePosition(const char* value, std::array<float, 3>& position)
{
	char* end = nullptr;
	for (std::size_t component = 0; component < position.size(); ++component)
	{
		position[component] = std::strtof(value, &end);
		if (end == value || (component + 1u < position.size() && *end != ','))
			return false;
		value = end + (component + 1u < position.size() ? 1 : 0);
	}
	return *end == '\0';
}

bool parseRay(const char* value, std::array<float, 6>& ray)
{
	char* end = nullptr;
	for (std::size_t component = 0; component < ray.size(); ++component)
	{
		ray[component] = std::strtof(value, &end);
		if (end == value || (component + 1u < ray.size() && *end != ','))
			return false;
		value = end + (component + 1u < ray.size() ? 1 : 0);
	}
	return *end == '\0';
}

std::array<float, 3> worldCenter(const xray::metal::R4StaticVisual& visual)
{
	const auto& bounds = visual.referencedPositionBounds;
	const std::array<float, 3> local = {
		(bounds.minimum[0] + bounds.maximum[0]) * 0.5f,
		(bounds.minimum[1] + bounds.maximum[1]) * 0.5f,
		(bounds.minimum[2] + bounds.maximum[2]) * 0.5f,
	};
	if (!visual.hasModelTransform)
		return local;
	const auto& m = visual.modelTransform;
	return {
		m[0] * local[0] + m[4] * local[1] + m[8] * local[2] + m[12],
		m[1] * local[0] + m[5] * local[1] + m[9] * local[2] + m[13],
		m[2] * local[0] + m[6] * local[1] + m[10] * local[2] + m[14],
	};
}
} // namespace

int main(int argc, char** argv)
{
	std::cout.imbue(std::locale::classic());
	if (argc == 2 && std::string(argv[1]) == "--self-test")
	{
		std::string error;
		if (!xray::metal::RunR4LevelGeometrySelfTest(error))
		{
			std::cerr << "METAL_LEVEL_GEOMETRY_SELF_TEST result=FAIL error=" << error << '\n';
			return 1;
		}
		std::cout << "METAL_LEVEL_GEOMETRY_SELF_TEST result=PASS\n";
		return 0;
	}
	if ((argc != 3 && argc != 4 && argc != 5) || std::string(argv[1]) != "--level-dir" ||
		(argc == 4 && std::string(argv[3]) != "--lmapped" &&
		 std::string(argv[3]) != "--shader-summary") ||
		(argc == 5 && std::string(argv[3]) != "--shader-id" &&
		 std::string(argv[3]) != "--root" && std::string(argv[3]) != "--near" &&
		 std::string(argv[3]) != "--ray" && std::string(argv[3]) != "--ray-all"))
	{
		std::cerr << "usage: " << argv[0]
			<< " --level-dir <directory> [--shader-id <id> | --root <visual-id> | --near <x,y,z> | --ray <x,y,z,dx,dy,dz> | --ray-all <x,y,z,dx,dy,dz> | --lmapped | --shader-summary] | --self-test\n";
		return 2;
	}
	std::uint32_t filteredShaderId = 0;
	std::uint32_t rootVisualId = 0;
	std::array<float, 3> nearPosition{};
	std::array<float, 6> ray{};
	const bool nearQuery = argc == 5 && std::string(argv[3]) == "--near";
	const bool rayAll = argc == 5 && std::string(argv[3]) == "--ray-all";
	const bool rayQuery = argc == 5 && (std::string(argv[3]) == "--ray" || rayAll);
	if (nearQuery)
	{
		if (!parsePosition(argv[4], nearPosition))
		{
			std::cerr << "invalid --near x,y,z value\n";
			return 2;
		}
	}
	else if (rayQuery)
	{
		if (!parseRay(argv[4], ray))
		{
			std::cerr << "invalid --ray x,y,z,dx,dy,dz value\n";
			return 2;
		}
		const float length = std::sqrt(ray[3] * ray[3] + ray[4] * ray[4] + ray[5] * ray[5]);
		if (!(length > 0.0f))
		{
			std::cerr << "invalid zero --ray direction\n";
			return 2;
		}
		ray[3] /= length;
		ray[4] /= length;
		ray[5] /= length;
	}
	else if (argc == 5)
	{
		char* end = nullptr;
		const unsigned long value = std::strtoul(argv[4], &end, 10);
		if (end == argv[4] || *end != '\0' || value > UINT32_MAX)
		{
			std::cerr << "invalid --shader-id value\n";
			return 2;
		}
		if (std::string(argv[3]) == "--shader-id")
			filteredShaderId = static_cast<std::uint32_t>(value);
		else
			rootVisualId = static_cast<std::uint32_t>(value);
	}

	const std::filesystem::path levelDirectory(argv[2]);
	xray::metal::R4LevelGeometry geometry;
	std::string error;
	if (!xray::metal::LoadR4LevelGeometry(levelDirectory, geometry, error))
	{
		std::cerr << "METAL_LEVEL_GEOMETRY_COMPLETE result=FAIL error=" << error << '\n';
		return 1;
	}

	std::uint64_t totalVertices = 0;
	std::uint64_t totalIndices = 0;
	std::uint32_t positionBuffers = 0;
	for (const auto& buffer : geometry.vertexBuffers)
	{
		totalVertices += buffer.vertexCount;
		positionBuffers += buffer.hasPositionFloat3 ? 1u : 0u;
	}
	for (const auto& buffer : geometry.indexBuffers)
		totalIndices += buffer.indexCount;

	std::uint32_t validVisuals = 0;
	std::uint32_t boundsVisuals = 0;
	std::uint32_t resolvedMaterials = 0;
	const xray::metal::R4StaticVisual* selected = nullptr;
	for (const auto& visual : geometry.visuals)
	{
		if (!visual.valid)
			continue;
		++validVisuals;
		if (visual.referencedPositionBounds.available)
			++boundsVisuals;
		if (visual.shaderId < geometry.shaders.size() &&
			!geometry.shaders[visual.shaderId].baseTexture.empty())
			++resolvedMaterials;
		if (selected == nullptr || (!selected->referencedPositionBounds.available && visual.referencedPositionBounds.available)
			|| (selected->referencedPositionBounds.available == visual.referencedPositionBounds.available && visual.visualId < selected->visualId))
			selected = &visual;
	}

	std::cout << "METAL_LEVEL_GEOMETRY_BEGIN level_dir=" << levelDirectory.generic_string() << '\n';
	std::cout << "METAL_LEVEL_GEOMETRY_BUFFERS vertex_buffers=" << geometry.vertexBuffers.size()
		<< " index_buffers=" << geometry.indexBuffers.size()
		<< " vertices=" << totalVertices
		<< " indices=" << totalIndices
		<< " position_float3_buffers=" << positionBuffers << '\n';
	std::cout << "METAL_LEVEL_GEOMETRY_VISUALS candidates=" << geometry.visuals.size()
		<< " valid=" << validVisuals
		<< " rejected=" << (geometry.visuals.size() - validVisuals)
		<< " bounds=" << boundsVisuals
		<< " resolved_materials=" << resolvedMaterials
		<< " shader_slots=" << geometry.shaders.size() << '\n';
	if (selected == nullptr)
	{
		std::cerr << "METAL_LEVEL_GEOMETRY_COMPLETE result=FAIL error=no valid OGF_GCONTAINER visual\n";
		return 1;
	}
	std::cout << "METAL_LEVEL_GEOMETRY_SELECTED visual_id=" << selected->visualId
		<< " vb=" << selected->vertexBufferId
		<< " vbase=" << selected->vertexBase
		<< " vcount=" << selected->vertexCount
		<< " ib=" << selected->indexBufferId
		<< " ibase=" << selected->indexBase
		<< " icount=" << selected->indexCount;
	if (selected->shaderId < geometry.shaders.size())
	{
		const auto& shader = geometry.shaders[selected->shaderId];
		std::cout << " shader_id=" << selected->shaderId
			<< " shader=" << shader.shaderName
			<< " base_texture=" << shader.baseTexture;
	}
	if (selected->referencedPositionBounds.available)
		printBounds(selected->referencedPositionBounds);
	else
		std::cout << " bounds=unavailable";
	std::cout << '\n';
	if (rayQuery)
	{
		struct RayVisual
		{
			const xray::metal::R4StaticVisual* visual;
			std::array<float, 3> center;
			float depth;
			float lateral;
		};
		std::vector<RayVisual> visible;
		for (const auto& visual : geometry.visuals)
		{
			if (!visual.valid || !visual.referencedPositionBounds.available ||
				visual.shaderId >= geometry.shaders.size())
				continue;
			const auto& shader = geometry.shaders[visual.shaderId];
			if (!rayAll && (shader.textures.size() < 3u || shader.textures[2].rfind("lmap", 0u) != 0u))
				continue;
			const auto center = worldCenter(visual);
			const float x = center[0] - ray[0];
			const float y = center[1] - ray[1];
			const float z = center[2] - ray[2];
			const float depth = x * ray[3] + y * ray[4] + z * ray[5];
			if (depth <= 0.2f || depth > 500.0f)
				continue;
			const float distanceSquared = x * x + y * y + z * z;
			const float lateral = std::sqrt(std::max(0.0f, distanceSquared - depth * depth));
			if (lateral > depth * 0.60f + 5.0f)
				continue;
			visible.push_back({&visual, center, depth, lateral});
		}
		std::sort(visible.begin(), visible.end(), [](const RayVisual& left, const RayVisual& right) {
			if (left.depth != right.depth)
				return left.depth < right.depth;
			return left.lateral < right.lateral;
		});
		std::cout << "METAL_LEVEL_GEOMETRY_RAY origin=" << ray[0] << ',' << ray[1] << ',' << ray[2]
			<< " direction=" << ray[3] << ',' << ray[4] << ',' << ray[5]
			<< " visuals=" << visible.size() << '\n';
		for (std::size_t i = 0; i < std::min<std::size_t>(visible.size(), 80u); ++i)
		{
			const auto& entry = visible[i];
			const auto& visual = *entry.visual;
			const auto& shader = geometry.shaders[visual.shaderId];
			std::cout << "METAL_LEVEL_GEOMETRY_RAY_RANK rank=" << i
				<< " visual_id=" << visual.visualId
				<< " depth=" << entry.depth << " lateral=" << entry.lateral
				<< " center=" << entry.center[0] << ',' << entry.center[1] << ',' << entry.center[2]
				<< " icount=" << visual.indexCount
				<< " shader_id=" << visual.shaderId
				<< " shader=\"" << shader.shaderName << "\""
				<< " texture_list=\"" << shader.textureList << "\""
				<< " base_texture=" << shader.baseTexture << '\n';
		}
	}
	else if (nearQuery)
	{
		struct NearVisual
		{
			const xray::metal::R4StaticVisual* visual;
			std::array<float, 3> center;
			float distanceSquared;
		};
		std::vector<NearVisual> nearest;
		for (const auto& visual : geometry.visuals)
		{
			if (!visual.valid || !visual.referencedPositionBounds.available ||
				visual.shaderId >= geometry.shaders.size())
				continue;
			const auto& shader = geometry.shaders[visual.shaderId];
			if (shader.textures.size() < 3u || shader.textures[2].rfind("lmap", 0u) != 0u)
				continue;
			const auto center = worldCenter(visual);
			const float dx = center[0] - nearPosition[0];
			const float dy = center[1] - nearPosition[1];
			const float dz = center[2] - nearPosition[2];
			nearest.push_back({&visual, center, dx * dx + dy * dy + dz * dz});
		}
		std::sort(nearest.begin(), nearest.end(), [](const NearVisual& left, const NearVisual& right) {
			return left.distanceSquared < right.distanceSquared;
		});
		std::cout << "METAL_LEVEL_GEOMETRY_NEAR position=" << nearPosition[0] << ','
			<< nearPosition[1] << ',' << nearPosition[2] << " visuals=" << nearest.size() << '\n';
		for (std::size_t i = 0; i < std::min<std::size_t>(nearest.size(), 32u); ++i)
		{
			const auto& entry = nearest[i];
			const auto& visual = *entry.visual;
			const auto& shader = geometry.shaders[visual.shaderId];
			std::cout << "METAL_LEVEL_GEOMETRY_NEAR_RANK rank=" << i
				<< " visual_id=" << visual.visualId
				<< " distance=" << std::sqrt(entry.distanceSquared)
				<< " center=" << entry.center[0] << ',' << entry.center[1] << ',' << entry.center[2]
				<< " icount=" << visual.indexCount
				<< " shader_id=" << visual.shaderId
				<< " shader=\"" << shader.shaderName << "\""
				<< " texture_list=\"" << shader.textureList << "\""
				<< " base_texture=" << shader.baseTexture << '\n';
		}
	}
	else if (argc == 4 && std::string(argv[3]) == "--shader-summary")
	{
		struct ShaderFamilySummary
		{
			std::uint64_t indices = 0;
			std::uint64_t vertices = 0;
			std::uint32_t visuals = 0;
			std::uint32_t shaderSlots = 0;
		};
		std::map<std::string, ShaderFamilySummary> families;
		for (const auto& shader : geometry.shaders)
			++families[shader.shaderName].shaderSlots;
		for (const auto& visual : geometry.visuals)
		{
			if (!visual.valid || visual.shaderId >= geometry.shaders.size())
				continue;
			auto& family = families[geometry.shaders[visual.shaderId].shaderName];
			++family.visuals;
			family.vertices += visual.vertexCount;
			family.indices += visual.indexCount;
		}
		std::vector<std::pair<std::string, ShaderFamilySummary>> sorted(families.begin(), families.end());
		std::sort(sorted.begin(), sorted.end(), [](const auto& left, const auto& right) {
			if (left.second.indices != right.second.indices)
				return left.second.indices > right.second.indices;
			if (left.second.visuals != right.second.visuals)
				return left.second.visuals > right.second.visuals;
			return left.first < right.first;
		});
		std::cout << "METAL_LEVEL_GEOMETRY_SHADER_SUMMARY families=" << sorted.size() << '\n';
		for (std::size_t rank = 0; rank < sorted.size(); ++rank)
		{
			const auto& [shaderName, family] = sorted[rank];
			std::cout << "METAL_LEVEL_GEOMETRY_SHADER_FAMILY rank=" << rank
				<< " shader=\"" << shaderName << '\"'
				<< " visuals=" << family.visuals
				<< " vertices=" << family.vertices
				<< " indices=" << family.indices
				<< " shader_slots=" << family.shaderSlots << '\n';
		}
	}
	else if ((argc == 5 && std::string(argv[3]) == "--shader-id") || argc == 4)
	{
		std::vector<const xray::metal::R4StaticVisual*> filtered;
		for (const auto& visual : geometry.visuals)
		{
			const bool hasLightmap = visual.shaderId < geometry.shaders.size() &&
				geometry.shaders[visual.shaderId].textures.size() >= 3u &&
				geometry.shaders[visual.shaderId].textures[2].rfind("lmap", 0u) == 0u;
			if (visual.valid && (argc == 4 || filteredShaderId == UINT32_MAX ||
				visual.shaderId == filteredShaderId) && (argc != 4 || hasLightmap))
				filtered.push_back(&visual);
		}
		std::sort(filtered.begin(), filtered.end(), [](const auto* left, const auto* right) {
			if (left->indexCount != right->indexCount)
				return left->indexCount > right->indexCount;
			return left->visualId < right->visualId;
		});
		std::cout << (argc == 4 ? "METAL_LEVEL_GEOMETRY_LIGHTMAPPED" :
			"METAL_LEVEL_GEOMETRY_SHADER_FILTER") << " shader_id=" << filteredShaderId
			<< " visuals=" << filtered.size() << '\n';
		for (std::size_t i = 0; i < std::min<std::size_t>(filtered.size(), 20u); ++i)
		{
			const auto& visual = *filtered[i];
			std::cout << "METAL_LEVEL_GEOMETRY_RANK rank=" << i
				<< " visual_id=" << visual.visualId
				<< " vb=" << visual.vertexBufferId
				<< " vbase=" << visual.vertexBase
				<< " vcount=" << visual.vertexCount
				<< " ib=" << visual.indexBufferId
				<< " ibase=" << visual.indexBase
				<< " icount=" << visual.indexCount
				<< " shader_id=" << visual.shaderId
				<< " declaration=\"";
			const auto& declaration = geometry.vertexBuffers[visual.vertexBufferId].declaration;
			for (std::size_t elementIndex = 0; elementIndex < declaration.size(); ++elementIndex)
			{
				const auto& element = declaration[elementIndex];
				if (elementIndex != 0u)
					std::cout << ',';
				std::cout << static_cast<unsigned>(element.stream) << ':' << element.offset << ':'
					<< static_cast<unsigned>(element.type) << ':'
					<< static_cast<unsigned>(element.method) << ':'
					<< static_cast<unsigned>(element.usage) << ':'
					<< static_cast<unsigned>(element.usageIndex);
			}
			std::cout << '"';
			if (visual.shaderId < geometry.shaders.size())
				std::cout << " texture_list=\"" << geometry.shaders[visual.shaderId].textureList
					<< "\" base_texture=" << geometry.shaders[visual.shaderId].baseTexture;
			if (visual.referencedPositionBounds.available)
				printBounds(visual.referencedPositionBounds);
			std::cout << '\n';
		}
	}
	else if (argc == 5)
	{
		std::vector<std::uint32_t> descendants;
		if (!xray::metal::CollectR4StaticVisualDescendants(
				geometry, rootVisualId, descendants, error))
		{
			std::cerr << "METAL_LEVEL_GEOMETRY_COMPLETE result=FAIL error=" << error << '\n';
			return 1;
		}
		std::cout << "METAL_LEVEL_GEOMETRY_ROOT root=" << rootVisualId
			<< " static_descendants=" << descendants.size() << '\n';
		for (const std::uint32_t descendantId : descendants)
		{
			const auto found = std::find_if(geometry.visuals.begin(), geometry.visuals.end(),
				[descendantId](const auto& visual) { return visual.visualId == descendantId; });
			if (found == geometry.visuals.end())
				continue;
			std::cout << "METAL_LEVEL_GEOMETRY_DESCENDANT visual_id=" << found->visualId
				<< " shader_id=" << found->shaderId
				<< " icount=" << found->indexCount
				<< " declaration=\"";
			const auto& declaration = geometry.vertexBuffers[found->vertexBufferId].declaration;
			for (std::size_t elementIndex = 0; elementIndex < declaration.size(); ++elementIndex)
			{
				const auto& element = declaration[elementIndex];
				if (elementIndex != 0u)
					std::cout << ',';
				std::cout << static_cast<unsigned>(element.stream) << ':' << element.offset << ':'
					<< static_cast<unsigned>(element.type) << ':'
					<< static_cast<unsigned>(element.method) << ':'
					<< static_cast<unsigned>(element.usage) << ':'
					<< static_cast<unsigned>(element.usageIndex);
			}
			std::cout << '"';
			if (found->hasModelTransform)
				std::cout << " xform_translation=" << found->modelTransform[12] << ','
					<< found->modelTransform[13] << ',' << found->modelTransform[14];
			if (found->shaderId < geometry.shaders.size())
				std::cout << " base_texture=" << geometry.shaders[found->shaderId].baseTexture;
			std::cout << '\n';
		}
	}
	std::cout << "METAL_LEVEL_GEOMETRY_COMPLETE result=PASS\n";
	return 0;
}
