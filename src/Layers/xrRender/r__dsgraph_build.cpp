#include "stdafx.h"

#include "fhierrarhyvisual.h"
#include "SkeletonCustom.h"
#include "LightTrack.h"
#include "../../xrEngine/fmesh.h"
#include "../../xrEngine/irenderable.h"
#include "../../xrEngine/xr_object.h"
#include "../../Include/xrRender/R4BackendSelection.h"

#include "flod.h"
#include "particlegroup.h"
#include "FTreeVisual.h"

#include <cstddef>

using namespace R_dsgraph;

////////////////////////////////////////////////////////////////////////////////////////////////////
// Scene graph actual insertion and sorting ////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
float r_ssaDISCARD;
float r_ssaDONTSORT;
float r_ssaLOD_A, r_ssaLOD_B;
float r_ssaGLOD_start, r_ssaGLOD_end;
float r_ssaHZBvsTEX;

namespace
{
bool captureMetalParticleEffect(dxRender_Visual* visual)
{
    if (SelectedR4Backend() != R4BackendKind::Metal || !visual || visual->Type != MT_PARTICLE_EFFECT)
        return false;
    // ParticleEffect::Render copies its already CPU-expanded FVF::LIT sprites
    // into the value-only Metal frame batch. No legacy RCache object is used.
    visual->Render(1.0f);
    return true;
}

bool hasUsableMetalHudBoneHistory(const Fmatrix& transform)
{
	// CBoneInstance::construct() intentionally initializes only the current
	// render transform. A zero-filled previous Fmatrix is finite, but it is not
	// an affine bone pose and must not enter the SKIN_4 history palette.
	return _valid(transform) && _abs(transform._14_) <= EPS_S &&
		_abs(transform._24_) <= EPS_S && _abs(transform._34_) <= EPS_S &&
		_abs(transform._44_ - 1.f) <= EPS_S;
}

bool isMetalHudOpaqueGeometry(const MetalCpuGeometry& geometry)
{
	const LPCSTR shader = geometry.shaderName.c_str();
	return geometry.skinned && shader &&
		(xr_strcmp(shader, "models\\model") == 0 || xr_strcmp(shader, "models\\weapons") == 0);
}

// OGF_TEXTURE stores the shader identity with a path separator.  The GAMMA
// 3DSS assets contain these exact values (not models_scope_* cache basenames).
bool classifyMetalHudScopeGeometry(const MetalCpuGeometry& geometry,
	CDSGraphManager::MetalHudRenderable::ScopeRole& role)
{
	const LPCSTR shader = geometry.shaderName.c_str();
	if (!geometry.skinned || !shader)
		return false;
	if (xr_strcmp(shader, "models\\scope_back") == 0)
	{
		role = CDSGraphManager::MetalHudRenderable::ScopeRole::Back;
		return true;
	}
	if (xr_strcmp(shader, "models\\scope_zwrite") == 0)
	{
		role = CDSGraphManager::MetalHudRenderable::ScopeRole::Zwrite;
		return true;
	}
	if (xr_strcmp(shader, "models\\scope_reticle_precise") == 0)
	{
		role = CDSGraphManager::MetalHudRenderable::ScopeRole::ReticlePrecise;
		return true;
	}
	return false;
}

// CSkeletonX omits an entire child when none of its BonesUsed entries is
// visible. Aggregate Metal capture has no CSkeletonX instance, so reproduce
// that admission decision from the immutable raw OGF payload. The caller owns
// kinematics->UCalc_Mutex for the whole check. Any format/payload/index
// mismatch is deliberately rejected rather than admitting hidden geometry.
bool hasMetalHudVisibleBoneInfluence(const MetalCpuGeometry& geometry, IKinematics& kinematics)
{
	if (!geometry.skinned || !geometry.vertexBytes || geometry.vertexCount == 0u)
		return false;
	const u16 boneCount = kinematics.LL_BoneCount();
	if (boneCount == 0u)
		return false;
	const std::vector<u8>& vertices = *geometry.vertexBytes;
	auto validPayload = [&vertices, &geometry](const std::size_t stride)
	{
		return geometry.vertexStride == stride &&
			geometry.vertexCount <= vertices.size() / stride &&
			vertices.size() == std::size_t(geometry.vertexCount) * stride;
	};
	auto anyVisible = [&vertices, &geometry, &kinematics, boneCount](const std::size_t stride,
		auto readBones)
	{
		bool valid = true;
		bool hasVisible = false;
		auto inspectBone = [&kinematics, boneCount, &valid, &hasVisible](const u32 bone)
		{
			if (bone >= boneCount)
			{
				valid = false;
				return;
			}
			hasVisible = hasVisible || !!kinematics.LL_GetBoneVisible(u16(bone));
		};
		for (u32 vertex = 0u; vertex < geometry.vertexCount; ++vertex)
		{
			readBones(vertices.data() + std::size_t(vertex) * stride, inspectBone);
			if (!valid)
				return false;
		}
		return hasVisible;
	};
	switch (geometry.vertexFormat)
	{
	case OGF_VERTEXFORMAT_FVF_1L:
	case 1:
		if (!validPayload(sizeof(vertBoned1W))) return false;
		return anyVisible(sizeof(vertBoned1W), [](const u8* raw, const auto& inspectBone)
		{
			// Legacy CSkeletonX deliberately truncates vertBoned1W::matrix to
			// the authored 16-bit bone id before building BonesUsed.
			u16 bone = 0u;
			std::memcpy(&bone, raw + offsetof(vertBoned1W, matrix), sizeof(bone));
			inspectBone(bone);
		});
	case OGF_VERTEXFORMAT_FVF_2L:
	case 2:
		if (!validPayload(sizeof(vertBoned2W))) return false;
		return anyVisible(sizeof(vertBoned2W), [](const u8* raw, const auto& inspectBone)
		{
			u16 bones[2] = {};
			std::memcpy(bones, raw + offsetof(vertBoned2W, matrix0), sizeof(bones));
			inspectBone(bones[0]);
			inspectBone(bones[1]);
		});
	case OGF_VERTEXFORMAT_FVF_3L:
	case 3:
		if (!validPayload(sizeof(vertBoned3W))) return false;
		return anyVisible(sizeof(vertBoned3W), [](const u8* raw, const auto& inspectBone)
		{
			u16 bones[3] = {};
			std::memcpy(bones, raw + offsetof(vertBoned3W, m), sizeof(bones));
			inspectBone(bones[0]);
			inspectBone(bones[1]);
			inspectBone(bones[2]);
		});
	case OGF_VERTEXFORMAT_FVF_4L:
	case 4:
		if (!validPayload(sizeof(vertBoned4W))) return false;
		return anyVisible(sizeof(vertBoned4W), [](const u8* raw, const auto& inspectBone)
		{
			u16 bones[4] = {};
			std::memcpy(bones, raw + offsetof(vertBoned4W, m), sizeof(bones));
			inspectBone(bones[0]);
			inspectBone(bones[1]);
			inspectBone(bones[2]);
			inspectBone(bones[3]);
		});
	default:
		return false;
	}
}

CDSGraphManager::MetalHudRenderable::SelectedShaderFlags copyMetalHudSelectedShaderFlags(
	const ShaderElement* shader)
{
	CDSGraphManager::MetalHudRenderable::SelectedShaderFlags copied;
	if (!shader)
		return copied;
	copied.priority = u8(shader->flags.iPriority);
	copied.strictB2F = !!shader->flags.bStrictB2F;
	copied.emissive = !!shader->flags.bEmissive;
	copied.distort = !!shader->flags.bDistort;
	copied.wmark = !!shader->flags.bWmark;
	copied.scopeLense = u8(shader->flags.iScopeLense);
	return copied;
}

void recordMetalHudScopeAccepted(CDSGraphManager::MetalHudCaptureStats& stats,
	CDSGraphManager::MetalHudRenderable::ScopeRole role)
{
	switch (role)
	{
	case CDSGraphManager::MetalHudRenderable::ScopeRole::Back: ++stats.scopeBack; break;
	case CDSGraphManager::MetalHudRenderable::ScopeRole::Zwrite: ++stats.scopeZwrite; break;
	case CDSGraphManager::MetalHudRenderable::ScopeRole::ReticlePrecise: ++stats.scopeReticlePrecise; break;
	}
}

void appendMetalHudScopeLeaf(xr_vector<CDSGraphManager::MetalHudRenderable::ScopeLeaf>& output,
	u32 geometryIndex, CDSGraphManager::MetalHudRenderable::ScopeRole role,
	const ShaderElement* selectedShader, CDSGraphManager::MetalHudCaptureStats& stats)
{
	const u8 expectedScopeLense = u8(role) + 1u;
	if (!selectedShader || u8(selectedShader->flags.iScopeLense) != expectedScopeLense)
	{
		++stats.shaderRejected;
		return;
	}
	CDSGraphManager::MetalHudRenderable::ScopeLeaf leaf;
	leaf.geometryIndex = geometryIndex;
	leaf.role = role;
	leaf.selectedShader = copyMetalHudSelectedShaderFlags(selectedShader);
	output.push_back(leaf);
	recordMetalHudScopeAccepted(stats, role);
}

u32 captureMetalHudRenderable(xr_vector<CDSGraphManager::MetalHudRenderable>& output,
    dxRender_Visual* visual, const Fmatrix& transform, IRenderable* object,
	IKinematics* kinematics, const xr_vector<u32>* eligibleGeometryIndices = nullptr,
	const xr_vector<CDSGraphManager::MetalHudRenderable::ScopeLeaf>* scopeLeaves = nullptr)
{
    CDSGraphManager::MetalHudRenderable item;
    item.assetName = visual->dbg_name;
    item.transform.set(transform);
	// The bridge overwrites this with the preceding value-only HUD snapshot
	// only after it verifies the captured layout.  Until then an explicit
	// current -> previous fallback is safer than leaking an unrelated matrix.
	item.previousTransform.set(transform);
	item.material.set(0.f, 0.f, 0.f, 0.25f);
	item.hotness.set(object ? object->GetHotness() : 0.f,
		object ? object->GetTransparency() : 0.f, 0.f, 0.f);
	item.hemiCubePositive.set(0.f, 0.f, 0.f, 0.f);
	item.hemiCubeNegative.set(0.f, 0.f, 0.f, 0.f);
	if (object && object->renderable_ROS())
	{
		CROS_impl& lighting = *static_cast<CROS_impl*>(object->renderable_ROS());
		lighting.update_smooth(object);
		item.material.x = .75f * lighting.get_hemi();
		item.material.y = .75f * lighting.get_sun();
		const float* cube = lighting.get_hemi_cube();
		item.hemiCubePositive.set(cube[CROS_impl::CUBE_FACE_POS_X],
			cube[CROS_impl::CUBE_FACE_POS_Y], cube[CROS_impl::CUBE_FACE_POS_Z], 0.f);
		item.hemiCubeNegative.set(cube[CROS_impl::CUBE_FACE_NEG_X],
			cube[CROS_impl::CUBE_FACE_NEG_Y], cube[CROS_impl::CUBE_FACE_NEG_Z], 0.f);
		const float directionalHemi = _max(
			_max(_max(_abs(item.hemiCubePositive.x), _abs(item.hemiCubePositive.y)),
				_abs(item.hemiCubePositive.z)),
			_max(_max(_abs(item.hemiCubeNegative.x), _abs(item.hemiCubeNegative.y)),
				_abs(item.hemiCubeNegative.z)));
		if (!_valid(directionalHemi) || directionalHemi <= EPS_S)
		{
			// CPU-only Metal skeleton admission can observe CROS after its
			// scalar smoothing updated but before a directional cube sample is
			// available. The production HUD VS replaced its historical
			// L_material.x path with that cube, so retain the same scalar value
			// on every face instead of emitting an unlit first-person model.
			item.hemiCubePositive.set(item.material.x, item.material.x, item.material.x, 0.f);
			item.hemiCubeNegative.set(item.material.x, item.material.x, item.material.x, 0.f);
		}
	}
	item.asset = visual->GetMetalCpuAsset();
	if (kinematics)
    {
        xrCriticalSectionGuard boneGuard(&kinematics->UCalc_Mutex);
        const u16 count = kinematics->LL_BoneCount();
        if (count > 0u && count <= 256u)
        {
            auto transforms = std::make_shared<std::vector<std::array<float, 16>>>();
			auto previousTransforms = std::make_shared<std::vector<std::array<float, 16>>>();
            transforms->resize(count);
			previousTransforms->resize(count);
            for (u16 bone = 0; bone < count; ++bone)
			{
				const CBoneInstance& instance = kinematics->LL_GetBoneInstance(bone);
				const Fmatrix& currentTransform = instance.mRenderTransform;
				// mRenderTransform_prev is the legacy SKIN_4 source. New/reattached
				// HUD skeletons can have no usable previous pose yet, so preserve the
				// palette's bone ordinal/count and explicitly copy current -> previous.
				const Fmatrix& previousTransform = hasUsableMetalHudBoneHistory(instance.mRenderTransform_prev) ?
					instance.mRenderTransform_prev : currentTransform;
				static_assert(sizeof(Fmatrix) == sizeof(float) * 16,
					"Metal HUD skinning requires packed Fmatrix transforms");
				std::memcpy((*transforms)[bone].data(), &currentTransform, sizeof(currentTransform));
				std::memcpy((*previousTransforms)[bone].data(), &previousTransform,
					sizeof(previousTransform));
			}
            item.boneTransforms = std::move(transforms);
			item.previousBoneTransforms = std::move(previousTransforms);
        }
	}
	if (!item.asset || item.asset->geometry.empty())
		return 0u;
	if (eligibleGeometryIndices)
		item.eligibleGeometryIndices = *eligibleGeometryIndices;
	else
	{
		item.eligibleGeometryIndices.reserve(item.asset->geometry.size());
		for (u32 geometryIndex = 0; geometryIndex < item.asset->geometry.size(); ++geometryIndex)
			item.eligibleGeometryIndices.push_back(geometryIndex);
	}
	if (scopeLeaves)
		item.scopeLeaves = *scopeLeaves;
	if (item.eligibleGeometryIndices.empty() && item.scopeLeaves.empty())
		return 0u;
	const u32 admitted = u32(item.eligibleGeometryIndices.size() + item.scopeLeaves.size());
    output.push_back(std::move(item));
	return admitted;
}

u32 captureMetalHudLeaf(dxRender_Visual* visual, const Fmatrix& transform, IRenderable* object,
	IKinematics* kinematics, const ShaderElement* selectedShader, bool allowOpaque,
	CDSGraphManager::MetalHudCaptureStats& stats,
	xr_vector<CDSGraphManager::MetalHudRenderable>& output)
{
	const std::shared_ptr<const MetalCpuVisualAsset>& asset = visual->GetMetalCpuAsset();
	if (!asset)
		return 0u;
	xr_vector<u32> eligibleGeometryIndices;
	xr_vector<CDSGraphManager::MetalHudRenderable::ScopeLeaf> scopeLeaves;
	eligibleGeometryIndices.reserve(asset->geometry.size());
	scopeLeaves.reserve(asset->geometry.size());
	for (u32 geometryIndex = 0; geometryIndex < asset->geometry.size(); ++geometryIndex)
	{
		const MetalCpuGeometry& geometry = asset->geometry[geometryIndex];
		CDSGraphManager::MetalHudRenderable::ScopeRole role;
		if (classifyMetalHudScopeGeometry(geometry, role))
			appendMetalHudScopeLeaf(scopeLeaves, geometryIndex, role, selectedShader, stats);
		else if (allowOpaque && isMetalHudOpaqueGeometry(geometry))
			eligibleGeometryIndices.push_back(geometryIndex);
		else
			++stats.shaderRejected;
	}
	return captureMetalHudRenderable(output, visual, transform, object, kinematics,
		&eligibleGeometryIndices, &scopeLeaves);
}

u32 captureMetalHudAggregate(dxRender_Visual* visual, const Fmatrix& transform, IRenderable* object,
	IKinematics* kinematics, CDSGraphManager::MetalHudCaptureStats& stats,
	xr_vector<CDSGraphManager::MetalHudRenderable>& output)
{
	const std::shared_ptr<const MetalCpuVisualAsset>& asset = visual->GetMetalCpuAsset();
	if (!asset)
		return 0u;
	xr_vector<u32> eligibleGeometryIndices;
	xr_vector<CDSGraphManager::MetalHudRenderable::ScopeLeaf> scopeCandidates;
	u32 scopeRoleCounts[3] = {};
	if (!kinematics)
		return 0u;
	eligibleGeometryIndices.reserve(asset->geometry.size());
	scopeCandidates.reserve(asset->geometry.size());
	// Keep visibility and raw bone-ID validation in the same critical section
	// as legacy CSkeletonX::has_visible_bones().
	{
		xrCriticalSectionGuard boneGuard(&kinematics->UCalc_Mutex);
		for (u32 geometryIndex = 0; geometryIndex < asset->geometry.size(); ++geometryIndex)
		{
			const MetalCpuGeometry& geometry = asset->geometry[geometryIndex];
			if (!hasMetalHudVisibleBoneInfluence(geometry, *kinematics))
				continue;
			CDSGraphManager::MetalHudRenderable::ScopeRole role;
			if (classifyMetalHudScopeGeometry(geometry, role))
			{
				CDSGraphManager::MetalHudRenderable::ScopeLeaf leaf;
				leaf.geometryIndex = geometryIndex;
				leaf.role = role;
				scopeCandidates.push_back(leaf);
				++scopeRoleCounts[u32(role)];
			}
			else if (isMetalHudOpaqueGeometry(geometry))
				eligibleGeometryIndices.push_back(geometryIndex);
			else
				++stats.shaderRejected;
		}
	}
	if (!scopeCandidates.empty())
	{
		const bool completeTriplet = scopeRoleCounts[u32(CDSGraphManager::MetalHudRenderable::ScopeRole::Back)] == 1u &&
			scopeRoleCounts[u32(CDSGraphManager::MetalHudRenderable::ScopeRole::Zwrite)] == 1u &&
			scopeRoleCounts[u32(CDSGraphManager::MetalHudRenderable::ScopeRole::ReticlePrecise)] == 1u;
		if (completeTriplet)
		{
			for (const auto& leaf : scopeCandidates)
				recordMetalHudScopeAccepted(stats, leaf.role);
			++stats.scopeTriplets;
		}
		else
		{
			stats.scopePartialRejected += u32(scopeCandidates.size());
			for (const u32 count : scopeRoleCounts)
				if (count > 1u)
					stats.scopeDuplicateRejected += count - 1u;
			scopeCandidates.clear();
		}
	}
	return captureMetalHudRenderable(output, visual, transform, object, kinematics,
		&eligibleGeometryIndices, &scopeCandidates);
}
}

ICF float CalcSSA(float& distSQ, Fvector& C, float R)
{
	distSQ = Device.vCameraPosition.distance_to_sqr(C) + EPS;
    return (R * R / distSQ);
}

ICF float CalcSSA(float& distSQ, Fvector& C, dxRender_Visual* V)
{
    return CalcSSA(distSQ, C, V->vis.sphere.R);
}

void CDSGraphManager::r_dsgraph_insert_dynamic(dxRender_Visual *pVisual, Fmatrix* xform)
{
	if (metalHudCaptureActive)
		++metalHudCaptureStats.traversed;
	Fvector Center;
	xform->transform_tiny(Center, pVisual->vis.sphere.P);

	float distSQ;
	float SSA = CalcSSA(distSQ, Center, pVisual);
    Flags16& flags = pVisual->flags;
    ShaderElement* sh_d = &*pVisual->shader->E[4];
    if (!(flags.test(IRenderVisualFlags::eIgnoreOptimization) || (sh_d && sh_d->flags.bEmissive)))
    {
        if (SSA < r_ssaDISCARD)
        {
			if (metalHudCaptureActive)
				++metalHudCaptureStats.ssaRejected;
            //Msg("SSA %.2f discarded", SSA);
            return;
        }
    }
    

	// HOM occlusion culling for dynamic objects
    if (ps_r__common_flags.test(RFLAG_HOM_DYNAMIC))
    {
#if RENDER!=R_R1
        if (i_mask[CDSGraphManager::fl_normal])
#endif
        {
            Fbox world_bb;
            world_bb.xform(pVisual->vis.box, *xform);
            if (!RImplementation.HOM.visible(world_bb))
			{
				if (metalHudCaptureActive)
					++metalHudCaptureStats.homRejected;
                return;
			}
        }
    }

	// Distortive geometry should be marked and R2 special-cases it
	// a) Allow to optimize RT order
	// b) Should be rendered to special distort buffer in another pass
	VERIFY(pVisual->shader._get());

	if (!metalHudCaptureActive && sh_d && sh_d->flags.bDistort && i_mask[sh_d->flags.iPriority/2])
	{
		if (i_mask[CDSGraphManager::fl_hud])
			RGraph.mapHUDSorted.Distort.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh_d, i_mask[CDSGraphManager::fl_hud]);
		else
			RGraph.mapDynamicSorted.Distort.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh_d, i_mask[CDSGraphManager::fl_hud]);
	}

	// Select shader
	ShaderElement* sh = RImplementation.rimp_select_sh_dynamic(pVisual, distSQ);

	if (0==sh)
	{
		if (metalHudCaptureActive)
			++metalHudCaptureStats.shaderRejected;
		return;
	}
	u32 shader_priority = sh->flags.iPriority/2;
	if (!i_mask[shader_priority])
	{
		if (metalHudCaptureActive)
			++metalHudCaptureStats.priorityRejected;
		return;
	}

	// HUD models are emitted through add_Dynamic() as a hierarchy.  Capture
	// only here, after the hierarchy reached a leaf and R4 selected its exact
	// dynamic shader element/priority.  Capturing at add_Dynamic() would admit
	// parents, LOD branches and strict/forward variants that normal R4 never
	// submits into the opaque HUD G-buffer.
	if (metalHudCaptureActive)
	{
		++metalHudCaptureStats.requested;
		if (sh_d && sh_d->flags.bDistort)
			++metalHudCaptureStats.distortFollowup;
		if (sh->flags.bEmissive)
			++metalHudCaptureStats.emissiveFollowup;
		if (sh->flags.bWmark)
			++metalHudCaptureStats.wmarkFollowup;

		const ShaderElement* hq = pVisual->shader->E[SE_R2_NORMAL_HQ]._get();
		const bool selectedScopeForward = sh->flags.iScopeLense != 0u;
		const bool selectedOpaqueHud = !sh->flags.bStrictB2F && !selectedScopeForward &&
			i_mask[CDSGraphManager::fl_hud] && sh == hq;
		if (sh->flags.bStrictB2F)
			++metalHudCaptureStats.strict;
		else if (selectedScopeForward)
			++metalHudCaptureStats.scopeForward;
		else if (!i_mask[CDSGraphManager::fl_hud] || sh != hq)
			++metalHudCaptureStats.nonHq;

		// Scope leaves are intentionally allowed through their selected forward
		// route. They retain that route's flags in the immutable capture record;
		// ordinary opaque geometry still needs the exact HQ HUD selection.
		if (selectedOpaqueHud || selectedScopeForward)
		{
			if (captureMetalHudLeaf(pVisual, *xform, val_pObject, metalHudCaptureKinematics,
				sh, selectedOpaqueHud, metalHudCaptureStats, metalHudCaptureStaging) != 0u)
				++metalHudCaptureStats.admitted;
		}
		// The Metal capture path intentionally replaces legacy queue insertion.
		return;
	}

	// Create common node
	// NOTE: Invisible elements exist only in R1

#if defined(USE_DX11) //  Redotix99: for 3D Shader Based Scopes 		
	switch (sh->flags.iScopeLense) {	
		case 0:
			break;

		case 1: {
			RGraph.mapHUD.emplace_back(EPS, SSA, val_pObject, pVisual, xform, sh, i_mask[CDSGraphManager::fl_hud]);

			// SSS: Deprecated
			/*if (!sh->passes[0]->ps->hud_disabled)
			{
				HUDMask_Node* N2 = HUDMask.insertInAnyWay(EPS);
				N2->val.ssa = SSA;
				N2->val.pObject = RI.val_pObject;
				N2->val.pVisual = pVisual;
				N2->val.Matrix = *RI.val_pTransform;
				N2->val.se = sh;
			}*/
			return;
		}

		case 2: {
			RGraph.mapScopeHUD.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh, i_mask[CDSGraphManager::fl_hud]);
			return;
		}

		case 3: {
			RGraph.mapScopeHUDSorted.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh, i_mask[CDSGraphManager::fl_hud]);
			return;
		}
	}
#endif
	// HUD rendering
	if (i_mask[CDSGraphManager::fl_hud])
	{
		if (sh->flags.bStrictB2F)
		{
#if RENDER!=R_R1
			if (sh->flags.bEmissive)
			{
				if (i_mask[CDSGraphManager::fl_cam])
					RGraph.mapCamAttachedSorted.Emissive.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh_d, i_mask[CDSGraphManager::fl_cam]);
				else
					RGraph.mapHUDSorted.Emissive.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh_d, i_mask[CDSGraphManager::fl_hud]);
			}
#endif // RENDER!=R_R1
			if (i_mask[CDSGraphManager::fl_cam])
				RGraph.mapCamAttachedSorted.Sorted.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh, i_mask[CDSGraphManager::fl_cam]);
			else
				RGraph.mapHUDSorted.Sorted.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh, i_mask[CDSGraphManager::fl_hud]);
			return;
		}
		else
		{
			if (i_mask[CDSGraphManager::fl_cam])
				RGraph.mapCamAttached.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh, i_mask[CDSGraphManager::fl_cam]);
			else
				RGraph.mapHUD.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh, i_mask[CDSGraphManager::fl_hud]);

			/*
#if RENDER==R_R4
			if (RImplementation.o.ssfx_core && !sh->passes[0]->ps->hud_disabled)
			{
				HUDMask_Node* N2 = val_bCamAttached ? HUDMaskCamAttached.insertInAnyWay(distSQ) : HUDMask.insertInAnyWay(distSQ);
				N2->val.ssa = SSA;
				N2->val.pObject = RI.val_pObject;
				N2->val.pVisual = pVisual;
				N2->val.Matrix = *RI.val_pTransform;
				N2->val.se = sh;
			}
#endif*/

#if RENDER!=R_R1
			if (sh->flags.bEmissive)
			{
				if (i_mask[CDSGraphManager::fl_cam])
					RGraph.mapCamAttachedSorted.Emissive.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh_d, i_mask[CDSGraphManager::fl_cam]);
				else
					RGraph.mapHUDSorted.Emissive.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh_d, i_mask[CDSGraphManager::fl_hud]);
			}
				
#endif	//	RENDER!=R_R1

			return;
		}
	}

	// Shadows registering
#if RENDER==R_R1
	DSGraphItem<u32, false> item = { 0, SSA, val_pObject, pVisual, xform, nullptr, i_mask[CDSGraphManager::fl_hud] };
	R_dsgraph::mapDSGraphItemsMap<u32, false>::TNode N = { 0, item };
	RImplementation.L_Shadows->add_element(N);
#endif
	if (i_mask[CDSGraphManager::fl_invisible])
		return;

	// strict-sorting selection
	if (sh->flags.bStrictB2F && !pVisual->dcast_ParticleCustom())
	{
		RGraph.mapDynamicSorted.Sorted.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh, i_mask[CDSGraphManager::fl_hud]);
		return;
	}

#if RENDER!=R_R1
	// Emissive geometry should be marked and R2 special-cases it
	// a) Allow to skeep already lit pixels
	// b) Allow to make them 100% lit and really bright
	// c) Should not cast shadows
	// d) Should be rendered to accumulation buffer in the second pass
	if (sh->flags.bEmissive)
		RGraph.mapDynamicSorted.Emissive.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh_d, i_mask[CDSGraphManager::fl_hud]);

	if (sh->flags.bWmark && i_mask[CDSGraphManager::fl_wmarks])
	{
		if (i_mask[CDSGraphManager::fl_hud])
			RGraph.mapHUDSorted.Wmark.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh, i_mask[CDSGraphManager::fl_hud]);
		else
			RGraph.mapDynamicSorted.Wmark.emplace_back(distSQ, SSA, val_pObject, pVisual, xform, sh, i_mask[CDSGraphManager::fl_hud]);
		return;
	}
#endif

	for (u32 iPass = 0; iPass < sh->passes.size(); ++iPass)
	{
		// the most common node
		
		if (sh->passes[iPass] == nullptr)
		{
			continue;
		}

		SPass& pass = *sh->passes[iPass];

#if RENDER==R_R1
		AddToRenderQueue(RGraph.mapDynamicPasses[shader_priority][iPass], item, pass);
#else
		AddToRenderQueue(RGraph.mapDynamicPasses[shader_priority][iPass], { 0, SSA, val_pObject, pVisual, xform, nullptr, i_mask[CDSGraphManager::fl_hud] }, pass);
#endif
	}
}

void CDSGraphManager::capture_metal_world_renderable(dxRender_Visual* visual,
	Fmatrix* transform, IKinematics* kinematics)
{
	if (!metalWorldCaptureActive || !visual || !transform)
		return;
	const std::shared_ptr<const MetalCpuVisualAsset> asset = visual->GetMetalCpuAsset();
	if (!asset || asset->geometry.empty())
		return;
	MetalWorldRenderable item;
	item.assetName = visual->dbg_name;
	item.transform.set(*transform);
	item.bounds = visual->getVisData().sphere;
	item.visualType = visual->getType();
	item.hotness.set(0.f, 0.f, 0.f, 0.f);
	item.material.set(0.f, 0.f, 0.f, 0.25f);
	item.hemiCubePositive.set(0.f, 0.f, 0.f, 0.f);
	item.hemiCubeNegative.set(0.f, 0.f, 0.f, 0.f);
	if (val_pObject && visual->IsMetalHeatVisionHot())
		item.hotness.set(val_pObject->GetHotness(), val_pObject->GetTransparency(), 0.f, 0.f);
	if (val_pObject && val_pObject->renderable_ROS())
	{
		CROS_impl& lighting = *static_cast<CROS_impl*>(val_pObject->renderable_ROS());
		lighting.update_smooth(val_pObject);
		item.material.x = .75f * lighting.get_hemi();
		item.material.y = .75f * lighting.get_sun();
		const float* cube = lighting.get_hemi_cube();
		item.hemiCubePositive.set(cube[CROS_impl::CUBE_FACE_POS_X],
			cube[CROS_impl::CUBE_FACE_POS_Y], cube[CROS_impl::CUBE_FACE_POS_Z], 0.f);
		item.hemiCubeNegative.set(cube[CROS_impl::CUBE_FACE_NEG_X],
			cube[CROS_impl::CUBE_FACE_NEG_Y], cube[CROS_impl::CUBE_FACE_NEG_Z], 0.f);
		const float directionalHemi = _max(
			_max(_max(_abs(item.hemiCubePositive.x), _abs(item.hemiCubePositive.y)),
				_abs(item.hemiCubePositive.z)),
			_max(_max(_abs(item.hemiCubeNegative.x), _abs(item.hemiCubeNegative.y)),
				_abs(item.hemiCubeNegative.z)));
		if (!_valid(directionalHemi) || directionalHemi <= EPS_S)
		{
			item.hemiCubePositive.set(item.material.x, item.material.x, item.material.x, 0.f);
			item.hemiCubeNegative.set(item.material.x, item.material.x, item.material.x, 0.f);
		}
	}
	item.asset = asset;
	if (kinematics)
	{
		xrCriticalSectionGuard boneGuard(&kinematics->UCalc_Mutex);
		const u16 boneCount = kinematics->LL_BoneCount();
		if (boneCount > 0u && boneCount <= 256u)
		{
			auto transforms = std::make_shared<std::vector<std::array<float, 16>>>();
			transforms->resize(boneCount);
			for (u16 bone = 0; bone < boneCount; ++bone)
			{
				const Fmatrix& boneTransform =
					kinematics->LL_GetBoneInstance(bone).mRenderTransform;
				static_assert(sizeof(Fmatrix) == sizeof(float) * 16,
					"Metal CPU skinning requires packed Fmatrix transforms");
				std::memcpy((*transforms)[bone].data(), &boneTransform,
					sizeof(boneTransform));
			}
			item.boneTransforms = std::move(transforms);
		}
	}
	if (val_pObject)
		if (CObject* object = val_pObject->dcast_CObject())
			item.objectId = object->ID();
	metalWorldCaptureStaging.push_back(std::move(item));
}

extern float ps_r__ssaDISCARD_exp;
extern float ps_r__ssaDISCARD_fade_k;
void CDSGraphManager::r_dsgraph_insert_static(dxRender_Visual *pVisual)
{
	if (m_static_seen.find(pVisual) != m_static_seen.end())
	{
		if (PortalTraverseDbg_Enabled())
		{
			PortalTraverseDebugStats& dbg = PortalTraverseDbg_Get();
			const bool opt_bucket = PortalTraverseDbg_IsOptions(i_options);
			++dbg.static_dedup_skipped;
			if (opt_bucket)
				++dbg.static_dedup_skipped_opt;
			else
				++dbg.static_dedup_skipped_noopt;
		}
		return;
	}
	m_static_seen.insert(pVisual);
	if (PortalTraverseDbg_Enabled())
	{
		PortalTraverseDebugStats& dbg = PortalTraverseDbg_Get();
		const bool opt_bucket = PortalTraverseDbg_IsOptions(i_options);
		++dbg.static_dedup_seen;
		if (opt_bucket)
			++dbg.static_dedup_seen_opt;
		else
			++dbg.static_dedup_seen_noopt;
	}

	float distSQ;
	float SSA = CalcSSA(distSQ, pVisual->vis.sphere.P, pVisual);
    Flags16& flags = pVisual->flags;
    ShaderElement* sh_d = &*pVisual->shader->E[4];
    if (!(flags.test(IRenderVisualFlags::eIgnoreOptimization) || (sh_d && sh_d->flags.bEmissive)))
    {
        if (SSA < r_ssaDISCARD)
            return;

        // demonized: Replace hard cutoff with gradient cutoff
        // Smaller objects that fail the SSA test will still render depending on how much smaller they are than the discard limit.
        // Reduces the "rendering radius" effect and makes pop-in less noticeable
        // Allows to increase the discard limit for better performance without making pop-in much worse
        // Define where the "thinning" begins. 
        // E.g., objects 4x the size of the discard limit start fading.
        float fade_start = r_ssaDISCARD * ps_r__ssaDISCARD_fade_k;

        // The Gradient Zone
        if (SSA < fade_start)
        {
            // Calculate a linear survival probability between 0.0 and 1.0
            float survival_chance = (SSA - r_ssaDISCARD) / (fade_start - r_ssaDISCARD);

            // Convert the 32-bit hash to a float between 0.0 and 1.0
            // Multiplying by 1.0 / 2^32 is faster than float division
            u32 hash = GetFvectorHash(pVisual->vis.sphere.P);
            constexpr float hash_to_float = 1.0f / 4294967296.0f;
            float val = hash * hash_to_float;

            // If the object's hash value is higher than its survival chance, cull it
            if (val > _powf(survival_chance, ps_r__ssaDISCARD_exp))
                return;
        }
    }

	// Distortive geometry should be marked and R2 special-cases it
	// a) Allow to optimize RT order
	// b) Should be rendered to special distort buffer in another pass
	VERIFY(pVisual->shader._get());
	if (sh_d && sh_d->flags.bDistort && i_mask[sh_d->flags.iPriority/2])
		RGraph.mapStaticSorted.Distort.emplace_back(distSQ, SSA, nullptr, pVisual, &Fidentity, sh_d, false);

	// Select shader
	ShaderElement* sh = RImplementation.rimp_select_sh_static(pVisual, distSQ);

	if (0 == sh)
		return;
	u32 shader_priority = sh->flags.iPriority / 2;
	if (!i_mask[shader_priority])
		return;

	// Water rendering
#if RENDER==R_R4
	if (sh->flags.isWater && RImplementation.o.ssfx_water)
	{
		RGraph.mapWater.emplace_back(distSQ, SSA, nullptr, pVisual, &Fidentity, sh, false);
		return;
	}
#endif

	// strict-sorting selection
	if (sh->flags.bStrictB2F)
	{
		RGraph.mapStaticSorted.Sorted.emplace_back(distSQ, SSA, nullptr, pVisual, &Fidentity, sh, false);
		return;
	}

#if RENDER!=R_R1
	// Emissive geometry should be marked and R2 special-cases it
	// a) Allow to skeep already lit pixels
	// b) Allow to make them 100% lit and really bright
	// c) Should not cast shadows
	// d) Should be rendered to accumulation buffer in the second pass
	if (sh->flags.bEmissive)
		RGraph.mapStaticSorted.Emissive.emplace_back(distSQ, SSA, nullptr, pVisual, &Fidentity, sh_d, false );

	if (sh->flags.bWmark && i_mask[CDSGraphManager::fl_wmarks])
	{
		RGraph.mapStaticSorted.Wmark.emplace_back(distSQ, SSA, nullptr, pVisual, &Fidentity, sh, false);
		return;
	}
#endif

	for (u32 iPass = 0; iPass < sh->passes.size(); ++iPass)
	{
		// the most common node
		if (sh->passes[iPass] == nullptr)
			continue;

		SPass& pass	= *sh->passes[iPass];

		AddToRenderQueue(RGraph.mapStaticPasses[shader_priority][iPass], { 0, SSA, nullptr, pVisual, nullptr, nullptr, false }, pass);
	}
}

void CDSGraphManager::AddToRenderQueue(R_dsgraph::RenderQueue& queue, const R_dsgraph::DSGraphItem<u32, false>& item, const SPass& pass)
{
	if (PortalTraverseDbg_Enabled())
	{
		PortalTraverseDebugStats& dbg = PortalTraverseDbg_Get();
		const bool opt_bucket = PortalTraverseDbg_IsOptions(i_options);
		// Static items use nullptr matrix/object in current pipeline.
		if (item.pMatrix == nullptr && item.pObject == nullptr)
		{
			++dbg.queue_static_packets;
			if (opt_bucket)
				++dbg.queue_static_packets_opt;
			else
				++dbg.queue_static_packets_noopt;
		}
		else
		{
			++dbg.queue_dynamic_packets;
			if (opt_bucket)
				++dbg.queue_dynamic_packets_opt;
			else
				++dbg.queue_dynamic_packets_noopt;
		}
	}

	queue.emplace_back(item, pass);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
void CDSGraphManager::add_Dynamic(IRenderVisual* piVisual, Fmatrix* xform)
{
	
	dxRender_Visual* pVisual = (dxRender_Visual*)piVisual;
	if (!pVisual) return;
	if (metalHudCaptureActive)
		++metalHudCaptureStats.roots;
	if (captureMetalParticleEffect(pVisual)) return;
	Flags16& flags = piVisual->flags;

	if (!metalHudCaptureActive && !i_mask[CDSGraphManager::fl_normal] &&
		!!flags.test(IRenderVisualFlags::eNoShadow))
		return;

	// Visual is 100% visible - simply add it
	switch (pVisual->Type)
	{
	case MT_PARTICLE_GROUP:
	{
		// Add all children, doesn't perform any tests
		PS::CParticleGroup* pG = (PS::CParticleGroup*)pVisual->dcast_ParticleCustom();
		xrCriticalSectionGuard guard(&pG->onframe_lock);
		for (PS::CParticleGroup::SItem& I_ : pG->items)
		{
			add_Dynamic(I_._effect, xform);
			add_leafs_Dynamic(I_._children_related, xform);
			add_leafs_Dynamic(I_._children_free, xform);
		}
	}return;

	case MT_HIERRARHY:
	{
		// Add all children, doesn't perform any tests
		FHierrarhyVisual* pV = (FHierrarhyVisual*)pVisual;
		if (metalHudCaptureActive)
		{
			++metalHudCaptureStats.hierarchyNodes;
			metalHudCaptureStats.childEdges += u32(pV->children.size());
		}
		add_leafs_Dynamic(pV->children, xform);
	}return;

	case MT_SKELETON_ANIM:
	case MT_SKELETON_RIGID:
	{
		// Add all children, doesn't perform any tests
		CKinematics* pV = (CKinematics*)pVisual;
		if (metalHudCaptureActive)
			++metalHudCaptureStats.skeletonNodes;
		BOOL _use_lod = FALSE;
		if (pV->m_lod)
		{
			Fvector Tpos;
			float D;
			xform->transform_tiny(Tpos, pV->vis.sphere.P);
			float ssa = CalcSSA(D, Tpos, pV->vis.sphere.R * 0.5f);	// assume dynamics never consume full sphere
			if (ssa < r_ssaLOD_A)
				_use_lod = TRUE;
		}
		IKinematics* previousKinematics = metalHudCaptureKinematics;
		IKinematics* previousWorldKinematics = metalWorldCaptureKinematics;
		if (metalHudCaptureActive)
			metalHudCaptureKinematics = pV;
		if (metalWorldCaptureActive)
			metalWorldCaptureKinematics = pV;
		if (metalWorldCaptureActive && !_use_lod && pV->children.empty() &&
			pVisual->GetMetalCpuAsset())
		{
			capture_metal_world_renderable(pVisual, xform, pV);
			metalWorldCaptureKinematics = previousWorldKinematics;
			return;
		}
		// The Direct Metal model pool deliberately keeps CKinematics::children
		// empty so loading gameplay skeletons cannot allocate legacy D3D mesh
		// resources. Its immutable CPU OGF asset still contains individual leaf
		// material payloads. Keep the aggregate fallback per-geometry: only proven
		// opaque model/weapons leaves reach the HUD MSC; every other family remains
		// explicitly rejected rather than bypassing transparent/scope routing.
		if (metalHudCaptureActive && !_use_lod && pV->children.empty() && pVisual->GetMetalCpuAsset())
		{
			const u32 leafCount = u32(pVisual->GetMetalCpuAsset()->geometry.size());
			metalHudCaptureStats.traversed += leafCount;
			metalHudCaptureStats.requested += leafCount;
			metalHudCaptureStats.aggregateFallback += leafCount;
			metalHudCaptureStats.admitted += captureMetalHudAggregate(pVisual, *xform,
				val_pObject, pV, metalHudCaptureStats, metalHudCaptureStaging);
			metalHudCaptureKinematics = previousKinematics;
			return;
		}
		if (_use_lod)
			add_Dynamic(pV->m_lod, xform);
		else
		{
			//pV->CalculateBones(TRUE);
			if (i_mask[CDSGraphManager::fl_normal])
				pV->CalculateWallmarks();
			if (metalHudCaptureActive)
				metalHudCaptureStats.childEdges += u32(pV->children.size());
			add_leafs_Dynamic(pV->children, xform);
		}
		if (metalHudCaptureActive)
			metalHudCaptureKinematics = previousKinematics;
		if (metalWorldCaptureActive)
			metalWorldCaptureKinematics = previousWorldKinematics;
	}return;

	default:
	{
		if (metalWorldCaptureActive)
		{
			capture_metal_world_renderable(pVisual, xform, metalWorldCaptureKinematics);
			return;
		}
		// General type of visual
		// Calculate distance to it's center
		r_dsgraph_insert_dynamic(pVisual, xform);
	}return;
	}
}

void CDSGraphManager::add_Dynamic(dxRender_Visual* pVisual, Fmatrix* xform)
{
	if (!pVisual) return;
	if (metalHudCaptureActive)
		++metalHudCaptureStats.roots;
	if (captureMetalParticleEffect(pVisual)) return;
	Flags16& flags = pVisual->dcast_RenderVisual()->flags;

	if (!metalHudCaptureActive && !i_mask[CDSGraphManager::fl_normal] &&
		!!flags.test(IRenderVisualFlags::eNoShadow))
		return;

	// Visual is 100% visible - simply add it
	switch (pVisual->Type)
	{
	case MT_PARTICLE_GROUP:
	{
		// Add all children, doesn't perform any tests
		PS::CParticleGroup* pG = (PS::CParticleGroup*)pVisual->dcast_ParticleCustom();
		xrCriticalSectionGuard guard(&pG->onframe_lock);
		for (PS::CParticleGroup::SItem& I_ : pG->items)
		{
			add_Dynamic(I_._effect, xform);
			add_leafs_Dynamic(I_._children_related, xform);
			add_leafs_Dynamic(I_._children_free, xform);
		}
	}break;

	case MT_HIERRARHY:
	{
		// Add all children, doesn't perform any tests
		FHierrarhyVisual* pV = (FHierrarhyVisual*)pVisual;
		if (metalHudCaptureActive)
		{
			++metalHudCaptureStats.hierarchyNodes;
			metalHudCaptureStats.childEdges += u32(pV->children.size());
		}
		add_leafs_Dynamic(pV->children, xform);
	}break;

	case MT_SKELETON_ANIM:
	case MT_SKELETON_RIGID:
	{
		// Add all children, doesn't perform any tests
		CKinematics* pV = (CKinematics*)pVisual;
		if (metalHudCaptureActive)
			++metalHudCaptureStats.skeletonNodes;
		BOOL _use_lod = FALSE;
		if (pV->m_lod)
		{
			Fvector Tpos;
			float D;
			xform->transform_tiny(Tpos, pV->vis.sphere.P);
			float ssa = CalcSSA(D, Tpos, pV->vis.sphere.R * 0.5f);	// assume dynamics never consume full sphere
			if (ssa < r_ssaLOD_A)
				_use_lod = TRUE;
		}
		IKinematics* previousKinematics = metalHudCaptureKinematics;
		IKinematics* previousWorldKinematics = metalWorldCaptureKinematics;
		if (metalHudCaptureActive)
			metalHudCaptureKinematics = pV;
		if (metalWorldCaptureActive)
			metalWorldCaptureKinematics = pV;
		if (metalWorldCaptureActive && !_use_lod && pV->children.empty() &&
			pVisual->GetMetalCpuAsset())
		{
			capture_metal_world_renderable(pVisual, xform, pV);
			metalWorldCaptureKinematics = previousWorldKinematics;
			break;
		}
		if (metalHudCaptureActive && !_use_lod && pV->children.empty() && pVisual->GetMetalCpuAsset())
		{
			const u32 leafCount = u32(pVisual->GetMetalCpuAsset()->geometry.size());
			metalHudCaptureStats.traversed += leafCount;
			metalHudCaptureStats.requested += leafCount;
			metalHudCaptureStats.aggregateFallback += leafCount;
			metalHudCaptureStats.admitted += captureMetalHudAggregate(pVisual, *xform,
				val_pObject, pV, metalHudCaptureStats, metalHudCaptureStaging);
			metalHudCaptureKinematics = previousKinematics;
			break;
		}
		if (_use_lod)
			add_Dynamic(pV->m_lod, xform);
		else
		{
			//pV->CalculateBones(TRUE);
			if (i_mask[CDSGraphManager::fl_normal])
				pV->CalculateWallmarks();
			if (metalHudCaptureActive)
				metalHudCaptureStats.childEdges += u32(pV->children.size());
			add_leafs_Dynamic(pV->children, xform);
		}
		if (metalHudCaptureActive)
			metalHudCaptureKinematics = previousKinematics;
		if (metalWorldCaptureActive)
			metalWorldCaptureKinematics = previousWorldKinematics;
	}break;

	default:
	{
		if (metalWorldCaptureActive)
		{
			capture_metal_world_renderable(pVisual, xform, metalWorldCaptureKinematics);
			break;
		}
		// General type of visual
		// Calculate distance to it's center
		r_dsgraph_insert_dynamic(pVisual, xform);
	}break;
	}
}

void CDSGraphManager::add_leafs_Dynamic(xr_vector<dxRender_Visual*>& children, Fmatrix* xform)
{
	for (dxRender_Visual* pVisual : children)
	{
		add_Dynamic(pVisual, xform);
	}
}

void CDSGraphManager::add_leafs_Dynamic(xr_vector<IRenderVisual*>& children, Fmatrix* xform)
{
	for (IRenderVisual* pVisual : children)
	{
		add_Dynamic(pVisual, xform);
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////
void CDSGraphManager::add_Static(IRenderVisual* piVisual, CFrustum& frustum, u32 planes)
{
	dxRender_Visual* pVisual = (dxRender_Visual*)piVisual;
	if (!pVisual) return;

	Flags16& flags = piVisual->flags;
	if (!i_mask[CDSGraphManager::fl_normal] && !!flags.test(IRenderVisualFlags::eNoShadow))
		return;

	// Check frustum visibility and calculate distance to visual's center
	vis_data& vis = pVisual->vis;
	EFC_Visible VIS = frustum.testSAABB(vis.sphere.P, vis.sphere.R, vis.box.data(), planes);

	if (fcvNone == VIS)
		return;

#if RENDER!=R_R1
	if (i_mask[CDSGraphManager::fl_normal])//phase normal
#endif
		if (!RImplementation.HOM.visible(vis))
			return;

	// If we get here visual is visible or partially visible
	switch (pVisual->Type)
	{
	case MT_HIERRARHY:
	{
		// Add all children
		FHierrarhyVisual* pV = (FHierrarhyVisual*)pVisual;
		if (fcvPartial == VIS)
		{
			for (auto V : pV->children)
				add_Static(V, frustum, planes);
		}
		else
			add_leafs_Static(pV->children);
	}break;

	case MT_LOD:
	{
		FLOD* pV = (FLOD*)pVisual;
		float D;
		float ssa = CalcSSA(D, pV->vis.sphere.P, pV) * pV->lod_factor;

		if (ssa < r_ssaLOD_A)
		{
			if (ssa < r_ssaDISCARD)
				return;

			RGraph.mapLOD.emplace_back(D, ssa, nullptr, pVisual, nullptr, nullptr, false);
		}

#if RENDER!=R_R1
		if (ssa > r_ssaLOD_B || i_mask[CDSGraphManager::fl_shmap])//phase shmap
#else
		if (ssa > r_ssaLOD_B)
#endif
		{
			// Add all children, perform tests
			add_leafs_Static(pV->children);
		}
	}break;

	case MT_TREE_ST:
	case MT_TREE_PM:
	default:
	{
		// General type of visual
		r_dsgraph_insert_static(pVisual);
	}return;
	}
}

void CDSGraphManager::add_Static_MultiFrustum(IRenderVisual* piVisual, const xr_vector<CFrustum>& frustums, const xr_vector<u32>& masks)
{
	constexpr u32 FULLY_VISIBLE_MASK = u32(-1);

	dxRender_Visual* pVisual = (dxRender_Visual*)piVisual;
	if (!pVisual) return;

	Flags16& flags = piVisual->flags;
	if (!i_mask[CDSGraphManager::fl_normal] && !!flags.test(IRenderVisualFlags::eNoShadow))
		return;

	// Check visibility against all active frustums and propagate per-frustum masks.
	vis_data& vis = pVisual->vis;
	bool anyVisible = false;
	bool hasFullyVisibleFrustum = false;
	xr_vector<u32> childMasks;
	childMasks.resize(masks.size(), 0);

	for (u32 i = 0; i < masks.size(); ++i)
	{
		u32 planeMask = masks[i];
		if (!planeMask)
			continue;

		// Propagated from parent: this visual is fully visible in this frustum.
		if (planeMask == FULLY_VISIBLE_MASK)
		{
			anyVisible = true;
			hasFullyVisibleFrustum = true;
			childMasks[i] = FULLY_VISIBLE_MASK;
			continue;
		}

		EFC_Visible VIS = frustums[i].testSAABB(vis.sphere.P, vis.sphere.R, vis.box.data(), planeMask);
		if (VIS == fcvNone)
		{
			continue;
		}

		anyVisible = true;
		if (VIS == fcvFully)
		{
			// In original per-frustum traversal this path immediately adds leafs.
			// Preserve union semantics by marking this frustum as fully visible.
			hasFullyVisibleFrustum = true;
			childMasks[i] = FULLY_VISIBLE_MASK;
		}
		else
		{
			childMasks[i] = planeMask;
		}
	}

	if (!anyVisible)
		return;

#if RENDER!=R_R1
	if (i_mask[CDSGraphManager::fl_normal])//phase normal
#endif
		if (!RImplementation.HOM.visible(vis))
			return;

	// If we get here visual is visible in at least one frustum
	switch (pVisual->Type)
	{
	case MT_HIERRARHY:
	{
		FHierrarhyVisual* pV = (FHierrarhyVisual*)pVisual;
		if (hasFullyVisibleFrustum)
			add_leafs_Static(pV->children);
		else
		{
			for (auto V : pV->children)
				add_Static_MultiFrustum(V, frustums, childMasks);
		}
	}break;

	case MT_LOD:
	{
		FLOD* pV = (FLOD*)pVisual;
		float D;
		float ssa = CalcSSA(D, pV->vis.sphere.P, pV) * pV->lod_factor;

		if (ssa < r_ssaLOD_A)
		{
			if (ssa < r_ssaDISCARD)
				return;

			RGraph.mapLOD.emplace_back(D, ssa, nullptr, pVisual, nullptr, nullptr, false);
		}

#if RENDER!=R_R1
		if (ssa > r_ssaLOD_B || i_mask[CDSGraphManager::fl_shmap])//phase shmap
#else
		if (ssa > r_ssaLOD_B)
#endif
		{
			add_leafs_Static(pV->children);
		}
	}break;

	case MT_TREE_ST:
	case MT_TREE_PM:
	default:
	{
		r_dsgraph_insert_static(pVisual);
	}return;
	}
}

void CDSGraphManager::add_leaf_Static(dxRender_Visual* pVisual)
{
#if RENDER!=R_R1
	if (i_mask[CDSGraphManager::fl_normal])//phase normal
#endif
		if (!RImplementation.HOM.visible(pVisual->vis))
			return;

	// Visual is 100% visible - simply add it
	switch (pVisual->Type)
	{
	case MT_HIERRARHY:
	{
		FHierrarhyVisual* pV = (FHierrarhyVisual*)pVisual;
		add_leafs_Static(pV->children);
	}break;

	case MT_LOD:
	{
		FLOD* pV = (FLOD*)pVisual;
		float D;
		float ssa = CalcSSA(D, pV->vis.sphere.P, pV) * pV->lod_factor;

		if (ssa < r_ssaLOD_A)
		{
			if (ssa < r_ssaDISCARD)
				break;

			RGraph.mapLOD.emplace_back(D, ssa, nullptr, pVisual, nullptr, nullptr, false);
		}

#if RENDER!=R_R1
		if (ssa > r_ssaLOD_B || i_mask[CDSGraphManager::fl_shmap])//phase shmap
#else
		if (ssa > r_ssaLOD_B)
#endif
		{
			// Add all children, doesn't perform any tests
			add_leafs_Static(pV->children);
		}
	}break;

	case MT_TREE_PM:
	case MT_TREE_ST:
	default:
	{
		// General type of visual
		r_dsgraph_insert_static(pVisual);
	}break;
	}
}

void CDSGraphManager::add_leafs_Static(xr_vector<dxRender_Visual*>& children)
{
	for (dxRender_Visual* pVisual : children)
	{
		add_leaf_Static(pVisual);
	}
}

void CDSGraphManager::add_leafs_Static(xr_vector<IRenderVisual*>& children)
{
	for (IRenderVisual* pVisual : children)
	{
		dxRender_Visual* pV = (dxRender_Visual*)pVisual;
		if (!pV) continue;
		add_leaf_Static(pV);
	}
}
