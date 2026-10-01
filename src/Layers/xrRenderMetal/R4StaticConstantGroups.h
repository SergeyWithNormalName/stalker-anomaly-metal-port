#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace xray::metal
{
using R4StaticConstantModelKey = std::array<std::uint8_t, 64>;

struct R4StaticConstantGroupPlan
{
	std::vector<std::size_t> materialGroup;
	std::vector<std::size_t> representativeMaterial;
	std::vector<std::size_t> memberOffsets;
	std::vector<std::size_t> members;

	void Clear() noexcept
	{
		materialGroup.clear();
		representativeMaterial.clear();
		memberOffsets.clear();
		members.clear();
	}

	std::size_t GroupCount() const noexcept { return representativeMaterial.size(); }
};

struct R4StaticConstantPayload
{
	std::array<std::uint8_t, 160> vertexGlobals{};
	std::array<std::uint8_t, 256> vertexTransforms{};
	std::array<std::uint8_t, 256> fragmentTransforms{};
};

inline void InvalidateR4StaticConstantOcclusionStamp(
	std::uint64_t& scopeId, std::uint32_t& frame, bool& valid) noexcept
{
	scopeId = 0u;
	frame = UINT32_MAX;
	valid = false;
}

inline void PublishR4StaticConstantOcclusionStamp(
	std::array<float, 16>& outputWvp,
	std::array<float, 2>& outputJitter,
	std::uint64_t& scopeId,
	std::uint32_t& frame,
	bool& valid,
	const R4StaticConstantPayload& payload,
	const std::uint64_t newScopeId,
	const std::uint32_t newFrame) noexcept
{
	std::memcpy(outputWvp.data(), payload.vertexTransforms.data(), sizeof(outputWvp));
	std::memcpy(outputJitter.data(), payload.vertexGlobals.data() + 128u, sizeof(outputJitter));
	scopeId = newScopeId;
	frame = newFrame;
	valid = true;
}

struct R4StaticConstantKeyHash
{
	std::size_t operator()(const R4StaticConstantModelKey& key) const noexcept
	{
		std::size_t value = static_cast<std::size_t>(1469598103934665603ull);
		for (const std::uint8_t byte : key)
		{
			value ^= static_cast<std::size_t>(byte);
			value *= static_cast<std::size_t>(1099511628211ull);
		}
		return value;
	}
};

inline R4StaticConstantModelKey R4StaticConstantKeyFromBytes(const void* matrixBytes) noexcept
{
	R4StaticConstantModelKey key{};
	if (matrixBytes)
		std::memcpy(key.data(), matrixBytes, key.size());
	return key;
}

// The key is the exact 64-byte matrix representation: signed zero and NaN
// payloads remain distinct. Allocation failure clears the partial plan.
inline bool BuildR4StaticConstantGroupPlan(
	const std::vector<R4StaticConstantModelKey>& keys,
	R4StaticConstantGroupPlan& output) noexcept
{
	output.Clear();
	try
	{
		R4StaticConstantGroupPlan plan;
		plan.materialGroup.resize(keys.size());
		std::unordered_map<R4StaticConstantModelKey, std::size_t, R4StaticConstantKeyHash> groupByKey;
		groupByKey.reserve(keys.size());
		std::vector<std::size_t> counts;
		counts.reserve(keys.size());
		for (std::size_t material = 0; material < keys.size(); ++material)
		{
			const auto found = groupByKey.find(keys[material]);
			if (found != groupByKey.end())
			{
				plan.materialGroup[material] = found->second;
				++counts[found->second];
				continue;
			}
			const std::size_t group = plan.representativeMaterial.size();
			groupByKey.emplace(keys[material], group);
			plan.representativeMaterial.push_back(material);
			counts.push_back(1u);
			plan.materialGroup[material] = group;
		}

		plan.memberOffsets.resize(plan.GroupCount() + 1u);
		for (std::size_t group = 0; group < plan.GroupCount(); ++group)
		{
			if (counts[group] > std::numeric_limits<std::size_t>::max() - plan.memberOffsets[group])
				return false;
			plan.memberOffsets[group + 1u] = plan.memberOffsets[group] + counts[group];
		}
		plan.members.resize(keys.size());
		std::vector<std::size_t> next = plan.memberOffsets;
		for (std::size_t material = 0; material < keys.size(); ++material)
			plan.members[next[plan.materialGroup[material]]++] = material;
		output = std::move(plan);
		return true;
	}
	catch (...)
	{
		output.Clear();
		return false;
	}
}

template <typename Matrix4, typename Multiply>
inline R4StaticConstantPayload BuildR4StaticConstantFramePayload(
	const Matrix4& model,
	const Matrix4& viewProjection,
	const Matrix4& view,
	const Matrix4& previousViewProjection,
	Multiply&& multiply)
{
	static_assert(sizeof(Matrix4) == 64u, "R4 transform matrices must be exactly 64 bytes");
	static_assert(std::is_trivially_copyable<Matrix4>::value,
		"R4 transform matrices must be trivially copyable");
	R4StaticConstantPayload payload{};
	const Matrix4 worldViewProjection = multiply(viewProjection, model);
	const Matrix4 worldView = multiply(view, model);
	const Matrix4 previousWorldViewProjection = multiply(previousViewProjection, model);
	std::memcpy(payload.vertexGlobals.data(), &previousWorldViewProjection, sizeof(Matrix4));
	std::memcpy(payload.vertexGlobals.data() + 64u, &previousViewProjection, sizeof(Matrix4));
	std::memcpy(payload.vertexTransforms.data(), &worldViewProjection, sizeof(Matrix4));
	std::memcpy(payload.vertexTransforms.data() + 64u, &worldView, sizeof(Matrix4));
	std::memcpy(payload.vertexTransforms.data() + 128u, &model, sizeof(Matrix4));
	constexpr float materialClass[4] = {0.0f, 0.0f, 0.0f, 0.25f};
	std::memcpy(payload.vertexTransforms.data() + 192u, materialClass, sizeof(materialClass));
	payload.fragmentTransforms = payload.vertexTransforms;
	return payload;
}

template <typename Matrix4, typename Multiply>
inline R4StaticConstantPayload BuildR4StaticConstantInitialPayload(
	const Matrix4& model,
	const Matrix4& viewProjection,
	const Matrix4& view,
	Multiply&& multiply)
{
	static_assert(sizeof(Matrix4) == 64u, "R4 transform matrices must be exactly 64 bytes");
	static_assert(std::is_trivially_copyable<Matrix4>::value,
		"R4 transform matrices must be trivially copyable");
	R4StaticConstantPayload payload{};
	const Matrix4 worldViewProjection = multiply(viewProjection, model);
	const Matrix4 worldView = multiply(view, model);
	std::memcpy(payload.vertexGlobals.data(), &worldViewProjection, sizeof(Matrix4));
	std::memcpy(payload.vertexGlobals.data() + 64u, &viewProjection, sizeof(Matrix4));
	std::memcpy(payload.vertexTransforms.data(), &worldViewProjection, sizeof(Matrix4));
	std::memcpy(payload.vertexTransforms.data() + 64u, &worldView, sizeof(Matrix4));
	std::memcpy(payload.vertexTransforms.data() + 128u, &model, sizeof(Matrix4));
	constexpr float materialClass[4] = {0.0f, 0.0f, 0.0f, 0.25f};
	std::memcpy(payload.vertexTransforms.data() + 192u, materialClass, sizeof(materialClass));
	payload.fragmentTransforms = payload.vertexTransforms;
	return payload;
}

template <typename BuildPayload, typename UploadGroup, typename PublishMaterial, typename InvalidateAll>
inline bool ApplyR4StaticConstantGroupUpdates(
	const R4StaticConstantGroupPlan& plan,
	const bool publishMembers,
	BuildPayload&& buildPayload,
	UploadGroup&& uploadGroup,
	PublishMaterial&& publishMaterial,
	InvalidateAll&& invalidateAll)
{
	invalidateAll();
	for (std::size_t group = 0; group < plan.GroupCount(); ++group)
	{
		R4StaticConstantPayload payload{};
		if (!buildPayload(group, plan.representativeMaterial[group], payload) ||
			!uploadGroup(group, payload))
		{
			invalidateAll();
			return false;
		}
		if (publishMembers)
			for (std::size_t memberIndex = plan.memberOffsets[group];
				memberIndex < plan.memberOffsets[group + 1u]; ++memberIndex)
				publishMaterial(plan.members[memberIndex], group, payload);
	}
	return true;
}
} // namespace xray::metal
