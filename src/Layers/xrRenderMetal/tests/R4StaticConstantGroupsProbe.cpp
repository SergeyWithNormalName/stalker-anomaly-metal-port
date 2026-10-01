#include "R4StaticConstantGroups.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
struct alignas(16) Matrix4
{
	float elements[16]{};
};

struct Stamp
{
	std::array<float, 16> wvp{};
	std::array<float, 2> jitter{};
	std::uint64_t scope = 0;
	std::uint32_t frame = UINT32_MAX;
	bool valid = false;
};

Matrix4 multiply(const Matrix4& left, const Matrix4& right)
{
	Matrix4 result{};
	for (int column = 0; column < 4; ++column)
		for (int row = 0; row < 4; ++row)
			for (int k = 0; k < 4; ++k)
				result.elements[column * 4 + row] +=
					left.elements[k * 4 + row] * right.elements[column * 4 + k];
	return result;
}

Matrix4 identity()
{
	Matrix4 value{};
	value.elements[0] = value.elements[5] = value.elements[10] = value.elements[15] = 1.0f;
	return value;
}

Matrix4 camera(float seed)
{
	Matrix4 value = identity();
	value.elements[0] = 0.91f + seed * 0.001f;
	value.elements[1] = 0.07f - seed * 0.0003f;
	value.elements[4] = -0.11f + seed * 0.0002f;
	value.elements[5] = 1.03f + seed * 0.0004f;
	value.elements[10] = 0.83f;
	value.elements[12] = -3.25f + seed * 0.03125f;
	value.elements[13] = 8.5f - seed * 0.015625f;
	value.elements[14] = 1.75f + seed * 0.0078125f;
	return value;
}

Matrix4 model(unsigned kind)
{
	Matrix4 value = identity();
	if (kind == 1u)
	{
		value.elements[0] = 0.875f;
		value.elements[1] = 0.125f;
		value.elements[4] = -0.25f;
		value.elements[5] = 1.125f;
		value.elements[12] = 13.25f;
		value.elements[13] = -4.5f;
	}
	else if (kind == 2u)
	{
		value.elements[0] = 0.0f;
		value.elements[1] = 1.0f;
		value.elements[4] = -1.0f;
		value.elements[5] = 0.0f;
		value.elements[12] = -21.0f;
		value.elements[14] = 2.0f;
	}
	else if (kind == 3u)
		value.elements[4] = -0.0f; // Exact-byte key must distinguish signed zero.
	return value;
}

xray::metal::R4StaticConstantPayload legacyInitial(
	const Matrix4& modelMatrix, const Matrix4& viewProjection, const Matrix4& view)
{
	xray::metal::R4StaticConstantPayload payload{};
	const Matrix4 worldViewProjection = multiply(viewProjection, modelMatrix);
	const Matrix4 worldView = multiply(view, modelMatrix);
	std::memcpy(payload.vertexGlobals.data(), &worldViewProjection, sizeof(Matrix4));
	std::memcpy(payload.vertexGlobals.data() + 64u, &viewProjection, sizeof(Matrix4));
	std::memcpy(payload.vertexTransforms.data(), &worldViewProjection, sizeof(Matrix4));
	std::memcpy(payload.vertexTransforms.data() + 64u, &worldView, sizeof(Matrix4));
	std::memcpy(payload.vertexTransforms.data() + 128u, &modelMatrix, sizeof(Matrix4));
	constexpr float materialClass[4] = {0.0f, 0.0f, 0.0f, 0.25f};
	std::memcpy(payload.vertexTransforms.data() + 192u, materialClass, sizeof(materialClass));
	payload.fragmentTransforms = payload.vertexTransforms;
	return payload;
}

xray::metal::R4StaticConstantPayload legacyFrame(
	const Matrix4& modelMatrix, const Matrix4& viewProjection, const Matrix4& view,
	const Matrix4& previousViewProjection)
{
	xray::metal::R4StaticConstantPayload payload{};
	const Matrix4 worldViewProjection = multiply(viewProjection, modelMatrix);
	const Matrix4 worldView = multiply(view, modelMatrix);
	const Matrix4 previousWorldViewProjection = multiply(previousViewProjection, modelMatrix);
	std::memcpy(payload.vertexGlobals.data(), &previousWorldViewProjection, sizeof(Matrix4));
	std::memcpy(payload.vertexGlobals.data() + 64u, &previousViewProjection, sizeof(Matrix4));
	std::memcpy(payload.vertexTransforms.data(), &worldViewProjection, sizeof(Matrix4));
	std::memcpy(payload.vertexTransforms.data() + 64u, &worldView, sizeof(Matrix4));
	std::memcpy(payload.vertexTransforms.data() + 128u, &modelMatrix, sizeof(Matrix4));
	constexpr float materialClass[4] = {0.0f, 0.0f, 0.0f, 0.25f};
	std::memcpy(payload.vertexTransforms.data() + 192u, materialClass, sizeof(materialClass));
	payload.fragmentTransforms = payload.vertexTransforms;
	return payload;
}

bool equal(const xray::metal::R4StaticConstantPayload& left,
	const xray::metal::R4StaticConstantPayload& right)
{
	return std::memcmp(&left, &right, sizeof(left)) == 0;
}

int fail(const char* reason)
{
	std::fprintf(stderr, "STATIC_CONSTANT_GROUPS FAIL %s\n", reason);
	return 1;
}

bool runGroupedFrame(const xray::metal::R4StaticConstantGroupPlan& plan,
	const std::vector<Matrix4>& models, const Matrix4& viewProjection, const Matrix4& view,
	const Matrix4& previousViewProjection, const std::uint64_t scope,
const std::uint32_t frame, const std::size_t failUploadGroup,
	std::vector<xray::metal::R4StaticConstantPayload>& byMaterial,
	std::vector<Stamp>& stamps, std::size_t& groupUpdateCalls)
{
	return xray::metal::ApplyR4StaticConstantGroupUpdates(
		plan, true,
		[&](const std::size_t, const std::size_t representative,
			xray::metal::R4StaticConstantPayload& payload) {
			payload = xray::metal::BuildR4StaticConstantFramePayload(
				models[representative], viewProjection, view, previousViewProjection, multiply);
			return true;
		},
		[&](const std::size_t group, const xray::metal::R4StaticConstantPayload&) {
			++groupUpdateCalls;
			return group != failUploadGroup;
		},
		[&](const std::size_t material, const std::size_t,
			const xray::metal::R4StaticConstantPayload& payload) {
			byMaterial[material] = payload;
			xray::metal::PublishR4StaticConstantOcclusionStamp(
				stamps[material].wvp, stamps[material].jitter, stamps[material].scope,
				stamps[material].frame, stamps[material].valid, payload, scope, frame);
		},
		[&]() {
			for (Stamp& stamp : stamps)
				xray::metal::InvalidateR4StaticConstantOcclusionStamp(
					stamp.scope, stamp.frame, stamp.valid);
		});
}
} // namespace

int main()
{
	static_assert(sizeof(xray::metal::R4StaticConstantPayload) == 672u, "payload layout changed");
	std::vector<Matrix4> models;
	for (std::size_t i = 0; i < 12u; ++i) models.push_back(model(0u));
	models.push_back(model(1u));
	models.push_back(model(1u));
	models.push_back(model(2u));
	models.push_back(model(2u));
	models.push_back(model(3u));

	std::vector<xray::metal::R4StaticConstantModelKey> keys;
	for (const Matrix4& value : models)
		keys.push_back(xray::metal::R4StaticConstantKeyFromBytes(value.elements));
	xray::metal::R4StaticConstantGroupPlan plan;
	if (!xray::metal::BuildR4StaticConstantGroupPlan(keys, plan) ||
		plan.GroupCount() != 4u || plan.materialGroup.size() != models.size() ||
		plan.members.size() != models.size())
		return fail("exact model-byte grouping/cardinality");
	for (std::size_t i = 0; i < 12u; ++i)
		if (plan.materialGroup[i] != plan.materialGroup[0])
			return fail("identity fanout group");
	if (plan.materialGroup[12] != plan.materialGroup[13] ||
		plan.materialGroup[14] != plan.materialGroup[15] ||
		plan.materialGroup[0] == plan.materialGroup[16])
		return fail("distinct transforms or signed-zero key");

	const Matrix4 initialVp = camera(0.0f);
	const Matrix4 initialView = camera(2.0f);
	std::vector<xray::metal::R4StaticConstantPayload> legacyInitialPayloads(models.size());
	std::vector<xray::metal::R4StaticConstantPayload> groupedInitial(plan.GroupCount());
	for (std::size_t i = 0; i < models.size(); ++i)
		legacyInitialPayloads[i] = legacyInitial(models[i], initialVp, initialView);
	for (std::size_t group = 0; group < plan.GroupCount(); ++group)
	{
		const std::size_t representative = plan.representativeMaterial[group];
		groupedInitial[group] = xray::metal::BuildR4StaticConstantInitialPayload(
			models[representative], initialVp, initialView, multiply);
	}
	for (std::size_t i = 0; i < models.size(); ++i)
		if (!equal(legacyInitialPayloads[i], groupedInitial[plan.materialGroup[i]]))
			return fail("initial payload bytes differ from legacy");

	std::vector<xray::metal::R4StaticConstantPayload> grouped(models.size());
	std::vector<Stamp> stamps(models.size());
	std::size_t groupUpdateCalls = 0u;
	const Matrix4 vpA = camera(3.0f), viewA = camera(5.0f), prevA = camera(-2.0f);
	if (!runGroupedFrame(plan, models, vpA, viewA, prevA, 10u, 300u,
		std::numeric_limits<std::size_t>::max(), grouped, stamps, groupUpdateCalls) ||
	groupUpdateCalls != plan.GroupCount())
		return fail("one group-update callback per group and initial fanout");
	for (std::size_t i = 0; i < models.size(); ++i)
	{
		if (!equal(grouped[i], legacyFrame(models[i], vpA, viewA, prevA)) ||
			!stamps[i].valid || stamps[i].scope != 10u || stamps[i].frame != 300u ||
			std::memcmp(stamps[i].wvp.data(), grouped[i].vertexTransforms.data(), 64u) != 0 ||
			std::memcmp(stamps[i].jitter.data(), grouped[i].vertexGlobals.data() + 128u, 8u) != 0)
			return fail("frame payload/fanout/stamp exactness");
	}

	const Matrix4 vpB = camera(21.0f), viewB = camera(-8.0f), prevB = camera(11.0f);
	const std::size_t beforeRetry = groupUpdateCalls;
	if (runGroupedFrame(plan, models, vpB, viewB, prevB, 11u, 301u,
		1u, grouped, stamps, groupUpdateCalls))
		return fail("injected update failure was not returned");
	for (const Stamp& stamp : stamps)
		if (stamp.valid || stamp.scope != 0u || stamp.frame != UINT32_MAX)
			return fail("failed update retained stale scope stamp");
	if (groupUpdateCalls - beforeRetry != 2u)
		return fail("failure did not stop at rejected group");

	groupUpdateCalls = 0u;
	if (!runGroupedFrame(plan, models, vpB, viewB, prevB, 12u, 301u,
		std::numeric_limits<std::size_t>::max(), grouped, stamps, groupUpdateCalls) ||
	groupUpdateCalls != plan.GroupCount())
		return fail("retry update failed");
	for (std::size_t i = 0; i < models.size(); ++i)
		if (!equal(grouped[i], legacyFrame(models[i], vpB, viewB, prevB)) ||
			!stamps[i].valid || stamps[i].scope != 12u || stamps[i].frame != 301u)
			return fail("retry used stale payload or scope");

	std::size_t updateOnlyCount = 0u, publishedCount = 0u;
	const bool updateOnlySucceeded = xray::metal::ApplyR4StaticConstantGroupUpdates(
		plan, false,
		[&](std::size_t, std::size_t representative, xray::metal::R4StaticConstantPayload& payload) {
			payload = xray::metal::BuildR4StaticConstantFramePayload(
				models[representative], vpB, viewB, prevB, multiply);
			return true;
		},
		[&](std::size_t, const xray::metal::R4StaticConstantPayload&) {
			++updateOnlyCount; return true;
		},
		[&](std::size_t, std::size_t, const xray::metal::R4StaticConstantPayload&) {
			++publishedCount;
		},
		[]() {});
	if (!updateOnlySucceeded || updateOnlyCount != plan.GroupCount() || publishedCount != 0u)
		return fail("update-only path performed per-material fanout");

	std::puts("STATIC_CONSTANT_GROUPS PASS exact-keys initial-and-frame-bytes group-fanout same-frame-retry-stamps");
	return 0;
}
