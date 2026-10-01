#include "stdafx.h"

#include <cstring>
#include <cmath>
#include <unordered_map>

#include "../../xrEngine/render.h"
#include "../../xrEngine/irenderable.h"
#include "../../xrEngine/igame_persistent.h"
#include "../../xrEngine/environment.h"
#include "../../xrEngine/CustomHUD.h"

#include "FBasicVisual.h"
#include "CHudInitializer.h"
#include "MetalParticleCapture.h"
#include "../xrRenderMetal/MetalR4SunCascadeCapture.h"

#include "fhierrarhyvisual.h"
#include "SkeletonCustom.h"
#include "../../xrEngine/fmesh.h"
#include "flod.h"

#include "../../xrEngine/xr_object.h"

using namespace R_dsgraph;

extern ENGINE_API BOOL r2_advanced_pp;

extern float r_ssaDISCARD;
extern float r_ssaDONTSORT;
extern float r_ssaHZBvsTEX;
extern float r_ssaGLOD_start, r_ssaGLOD_end;
extern ENGINE_API float psHUD_FOV_def;
// Console globals consumed by the legacy exact 3DSS binders.
extern Fvector4 ps_dev_param_8;
extern int ps_markswitch_current;
extern Fvector4 ps_markswitch_color;
extern Fvector4 ps_s3ds_param_1;
extern Fvector4 ps_s3ds_param_2;
extern Fvector4 ps_s3ds_param_3;
extern Fvector4 ps_s3ds_param_4;

ICF float calcLOD(float ssa/*fDistSq*/, float R)
{
	return _sqrt(clampr((ssa - r_ssaGLOD_end) / (r_ssaGLOD_start - r_ssaGLOD_end), 0.f, 1.f));
}

template<typename T, bool Reverse>
void CDSGraphManager::r_dsgraph_render_graph_sorted(R_dsgraph::mapDSGraphItems<T, Reverse>& graph, bool _clear)
{
    if (graph.empty())
        return;

    std::sort(graph.begin(), graph.end());

	for (auto& item : graph)
	{
		dxRender_Visual* V = item.pVisual;
		VERIFY(V && V->shader._get());
		RCache.set_Element(item.pSE);
		RCache.set_xform_world(*item.pMatrix);
		RImplementation.apply_object(item.pObject);
		RImplementation.apply_lmaterial();
		//if (item.b_hud_mode)
		//{
		//	//new feature
		//}
		V->Render(calcLOD(item.ssa, V->vis.sphere.R));
	}

	if (_clear)
		graph.clear();

	RCache.set_xform_world(Fidentity);
}

void CDSGraphManager::r_dsgraph_render_graph(RenderQueueArray& queues, u32 _priority, bool _clear, bool static_geometry)
{
	RCache.set_xform_world(Fidentity);

	for (u32 iPass = 0; iPass < SHADER_PASSES_MAX; ++iPass)
	{
		auto& queue = queues[_priority][iPass];
		if (queue.empty())
			continue;

		// 1. Sort by generated sort key to replicate previous fixed map behaviour
		if (queue.size() < 4096)
            std::sort(queue.begin(), queue.end());
		else
            xr_parallel_sort(queue.begin(), queue.end());

		// 2. Render
		vs_type pVS = nullptr;

#if defined(USE_DX10) || defined(USE_DX11)
		gs_type pGS = nullptr;
#endif

		ps_type pPS = nullptr;

#ifdef USE_DX11
		hs_type pHS = nullptr;
		ds_type pDS = nullptr;
#endif
		
		R_constant_table* pCS = nullptr;
		ID3DState* pState = nullptr;
		STextureList* pTextures = nullptr;

        u64 high = 0;

        for (auto& packet : queue)
        {
            auto& currentKey = packet.sortKey;
            if (currentKey.high != high)
            {
                high = currentKey.high;

                if (packet.pState != pState)
                {
                    pState = packet.pState;
                    RCache.set_States(pState);
                }

#if defined(USE_DX10) || defined(USE_DX11)
                if (packet.pGS != pGS)
                {
                    pGS = packet.pGS;
                    RCache.set_GS(pGS);
                }
#endif

#ifdef USE_DX11
                if (packet.pHS != pHS)
                {
                    pHS = packet.pHS;
                    RCache.set_HS(pHS);
                }
                if (packet.pDS != pDS)
                {
                    pDS = packet.pDS;
                    RCache.set_DS(pDS);
                }
#endif
            }

            // Compare low key stuff regardless, too high collision probability
            if (packet.pVS != pVS)
            {
                pVS = packet.pVS;
                RCache.set_VS(pVS);
            }

            if (packet.pPS != pPS)
            {
                pPS = packet.pPS;
                RCache.set_PS(pPS);
            }

            if (packet.pCS != pCS)
            {
                pCS = packet.pCS;
                RCache.set_Constants(pCS);
            }

            if (packet.pTextures != pTextures)
            {
                pTextures = packet.pTextures;
                RCache.set_Textures(pTextures);
                RImplementation.apply_lmaterial();
            }

			auto& item = packet.item;
			if (!static_geometry)
			{
				RCache.set_xform_world(*item.pMatrix);
				RImplementation.apply_object(item.pObject);
				RImplementation.apply_lmaterial();
			}

			float LOD = calcLOD(item.ssa, item.pVisual->vis.sphere.R);
#ifdef USE_DX11
			RCache.LOD.set_LOD(LOD);
#endif
			item.pVisual->Render(LOD);
		}

		if (_clear)
			queue.clear();
	}
}

//////////////////////////////////////////////////////////////////////////
// HUD render
void CDSGraphManager::r_dsgraph_render_hud()
{
	PROF_EVENT("r_dsgraph_render_hud");
	CHudInitializer initializer(true);

	// Rendering
	RImplementation.rmNear();
	r_dsgraph_render_graph_sorted(RGraph.mapHUD);

	RImplementation.rmNormal();

#if defined(USE_DX11) //  Redotix99: for 3D Shader Based Scopes 		
	if (scope_3D_fake_enabled)
	{
		RCache.set_RT(RImplementation.Target->rt_ssfx_temp->pRT, 3); // Render scope_3D to any buffer

		r_dsgraph_render_graph_sorted(RGraph.mapScopeHUD);

		if (!RImplementation.o.ssfx_motionvectors)
			RCache.set_RT(NULL, 3);
		else
			RCache.set_RT(RImplementation.Target->rt_ssfx_motion_vectors->pRT, 3);
	}
#endif

	if (RGraph.mapCamAttached.size())
	{
		RImplementation.rmNear();

		// Change projection
		initializer.SetCamMode();

		// Rendering
		r_dsgraph_render_graph_sorted(RGraph.mapCamAttached);

		RImplementation.rmNormal();
	}
}

void CDSGraphManager::r_dsgraph_render_hud_ui()
{
	PROF_EVENT("r_dsgraph_render_hud_ui");
	CHudInitializer initializer(true);

	RImplementation.rmNear();
	g_hud->RenderActiveItemUI();
	RImplementation.rmNormal();
}

void CDSGraphManager::r_dsgraph_render_cam_ui()
{
	PROF_EVENT("r_dsgraph_render_cam_ui");

	// Change projection
	CHudInitializer initializer(2);
	
	// Rendering
	RImplementation.rmNear();
	g_hud->RenderCamAttachedUI();
	RImplementation.rmNormal();
}

//////////////////////////////////////////////////////////////////////////
// strict-sorted render
void CDSGraphManager::r_dsgraph_render_sorted(bool render_hud)
{
	{
		PROF_EVENT("r_dsgraph_render_sorted");
		// Rendering
		r_dsgraph_render_graph_sorted(RGraph.mapStaticSorted.Sorted, true);
		r_dsgraph_render_graph_sorted(RGraph.mapDynamicSorted.Sorted, true);
	}

	if (render_hud)
		r_dsgraph_render_sorted_hud();

	// Camera Script Attachments
	if (RGraph.mapCamAttachedSorted.Sorted.size())
	{
		RImplementation.rmNear();
		// Change projection
		CHudInitializer initializer(2);

		// Rendering
		r_dsgraph_render_graph_sorted(RGraph.mapCamAttachedSorted.Sorted, true);
		RImplementation.rmNormal();
	}
}

void CDSGraphManager::r_dsgraph_capture_hud()
{
	if (g_hud)
	{
		g_hud->Render_Last(dcast_IPortalTraverser());
		set_Object();
	}
}

#if defined(USE_DX11)
//////////////////////////////////////////////////////////////////////////
// strict-sorted render
void CDSGraphManager::r_dsgraph_render_ScopeSorted()  //  Redotix99: for 3D Shader Based Scopes 	
{
	// Change projection
	CHudInitializer initializer(true);

	// Rendering
	RImplementation.rmNear();
	r_dsgraph_render_graph_sorted(RGraph.mapScopeHUDSorted, true);
	RImplementation.rmNormal();
}
#endif

void CDSGraphManager::r_dsgraph_render_sorted_hud()
{
	PROF_EVENT("r_dsgraph_render_sorted_hud");
#if	RENDER==R_R4
	HW.pContext->CopyResource(RImplementation.Target->rt_Accumulator->pSurface, RImplementation.Target->rt_Generic_0->pSurface);
#endif
	CHudInitializer initializer(true);

	RImplementation.rmNear();
	r_dsgraph_render_graph_sorted(RGraph.mapHUDSorted.Sorted, true);
	RImplementation.rmNormal();
}

//////////////////////////////////////////////////////////////////////////
// strict-sorted render
void CDSGraphManager::r_dsgraph_render_emissive(bool clear, bool renderHUD)
{
	PROF_EVENT("r_dsgraph_render_emissive");
#if	RENDER!=R_R1
	r_dsgraph_render_graph_sorted(RGraph.mapStaticSorted.Emissive, clear);
	r_dsgraph_render_graph_sorted(RGraph.mapDynamicSorted.Emissive, clear);
	
	//	HACK: Calculate this only once
	CHudInitializer initializer(true);

	RImplementation.rmNear();
	r_dsgraph_render_graph_sorted(RGraph.mapHUDSorted.Emissive, clear);
	
	if (renderHUD)
		r_dsgraph_render_graph_sorted(RGraph.mapHUDSorted.Sorted, false);

	RImplementation.rmNormal();
#endif
}

void CDSGraphManager::r_dsgraph_render_water_ssr()
{
#ifdef USE_DX11
	PROF_EVENT("r_dsgraph_render_water_ssr");
	std::sort(RGraph.mapWater.begin(), RGraph.mapWater.end());
	for (auto& N : RGraph.mapWater)
	{
		dxRender_Visual* V = N.pVisual;
		VERIFY(V);

		RCache.set_Shader(RImplementation.Target->s_ssfx_water_ssr);

		RCache.set_xform_world(*N.pMatrix);
		RImplementation.apply_object(N.pObject);
		RImplementation.apply_lmaterial();

		RCache.set_c("cam_pos", RImplementation.Target->Position_previous.x, RImplementation.Target->Position_previous.y, RImplementation.Target->Position_previous.z, 0.0f);

		// Previous matrix data
		RCache.set_c("m_current", RImplementation.Target->Matrix_current);
		RCache.set_c("m_previous", RImplementation.Target->Matrix_previous);

		V->Render(calcLOD(N.ssa, V->vis.sphere.R));
	}
#endif
}

void CDSGraphManager::r_dsgraph_render_water()
{
	PROF_EVENT("r_dsgraph_render_water_ssr");
    std::sort(RGraph.mapWater.begin(), RGraph.mapWater.end());
    for (auto& N : RGraph.mapWater)
	{
		dxRender_Visual* V = N.pVisual;
		VERIFY(V);

#ifdef USE_DX11
		if (RImplementation.o.ssfx_water)
		{
			RCache.set_Shader(RImplementation.Target->s_ssfx_water);
		}
#endif

		RCache.set_xform_world(*N.pMatrix);
		RImplementation.apply_object(N.pObject);
		RImplementation.apply_lmaterial();

		// Wind settings
		float WindDir = g_pGamePersistent->Environment().CurrentEnv->wind_direction;
		float WindVel = g_pGamePersistent->Environment().CurrentEnv->wind_velocity;
		RCache.set_c("wind_setup", WindDir, WindVel, 0, 0);

		V->Render(calcLOD(N.ssa, V->vis.sphere.R));
	}
	RGraph.mapWater.clear();
}

//////////////////////////////////////////////////////////////////////////
// strict-sorted render
void CDSGraphManager::r_dsgraph_render_wmarks()
{
	PROF_EVENT("r_dsgraph_render_wmarks");
#if	RENDER!=R_R1
	// Rendering
	r_dsgraph_render_graph_sorted(RGraph.mapStaticSorted.Wmark);
	r_dsgraph_render_graph_sorted(RGraph.mapDynamicSorted.Wmark);
	//	HACK: Calculate this only once
	CHudInitializer initalizer(true);

	RImplementation.rmNear();
	r_dsgraph_render_graph_sorted(RGraph.mapHUDSorted.Wmark);
	RImplementation.rmNormal();
#endif
}

//////////////////////////////////////////////////////////////////////////
// strict-sorted render
void CDSGraphManager::r_dsgraph_render_distort()
{
	PROF_EVENT("r_dsgraph_render_distort");
	// Rendering
	r_dsgraph_render_graph_sorted(RGraph.mapStaticSorted.Distort, true);
	r_dsgraph_render_graph_sorted(RGraph.mapDynamicSorted.Distort, true);
	//	HACK: Calculate this only once
	CHudInitializer initalizer(true);

	RImplementation.rmNear();
	r_dsgraph_render_graph_sorted(RGraph.mapHUDSorted.Distort);
	RImplementation.rmNormal();
}

#include"LightTrack.h"
void CDSGraphManager::r_dsgraph_capture_static()
{
	PROF_EVENT("r_dsgraph_capture_static")
	const bool dbg_enabled = PortalTraverseDbg_Enabled();
	PortalTraverseDebugStats* dbg = dbg_enabled ? &PortalTraverseDbg_Get() : nullptr;
	const bool opt_bucket = PortalTraverseDbg_IsOptions(i_options);

	if (i_start)
	{
		// Traverse sector/portal structure
		if (psDeviceFlags.test(rsDrawStatic))
		{
			if (dbg)
			{
				const u32 sector_nodes = u32(m_sector_frustums.size());
				dbg->static_sector_nodes += sector_nodes;
				if (opt_bucket)
					dbg->static_sector_nodes_opt += sector_nodes;
				else
					dbg->static_sector_nodes_noopt += sector_nodes;
			}

			// Determine visibility for static geometry hierrarhy
			for (auto& pair : m_sector_frustums)
			{
				xr_vector<CFrustum>& frustums = pair.val.first;
				if (frustums.empty())
					continue;

				if (frustums.size() == 1)
				{
					if (dbg)
					{
						++dbg->static_frustum_nodes;
						++dbg->static_add_root_calls;
						if (opt_bucket)
						{
							++dbg->static_frustum_nodes_opt;
							++dbg->static_add_root_calls_opt;
						}
						else
						{
							++dbg->static_frustum_nodes_noopt;
							++dbg->static_add_root_calls_noopt;
						}
					}
					add_Static((IRenderVisual*)pair.key->root(), frustums.front(), frustums.front().getMask());
					continue;
				}

				xr_vector<u32> masks;
				masks.reserve(frustums.size());
				for (CFrustum& frustum_node : frustums)
					masks.push_back(frustum_node.getMask());

				if (dbg)
				{
					dbg->static_frustum_nodes += u32(frustums.size());
					++dbg->static_add_root_calls;
					if (opt_bucket)
					{
						dbg->static_frustum_nodes_opt += u32(frustums.size());
						++dbg->static_add_root_calls_opt;
					}
					else
					{
						dbg->static_frustum_nodes_noopt += u32(frustums.size());
						++dbg->static_add_root_calls_noopt;
					}
				}

				add_Static_MultiFrustum((IRenderVisual*)pair.key->root(), frustums, masks);
			}
		}
	}
}

void CDSGraphManager::r_dsgraph_capture_lights()
{
	PROF_EVENT("r_dsgraph_capture_lights")
	g_SpatialSpaceLights->q_frustum
	(
		lstLights,
		ISpatial_DB::O_ORDERED,
		STYPE_LIGHTSOURCE,
		i_frustum
	);

#if	RENDER==R_R1
	std::sort(lstLights.begin(), lstLights.end(), [](const ISpatialShared& _1, const ISpatialShared& _2) noexcept
	{
		if (!_1.get() || !_2.get()) return false;

		return	_1->spatial.sphere.P.distance_to_sqr(Device.vCameraPosition_saved) < _2->spatial.sphere.P.distance_to_sqr(Device.vCameraPosition_saved);
	});
#endif

	for (ISpatialShared spatial : lstLights)
	{
		if (0 == spatial) continue; spatial->spatial_updatesector();
		CSector* sector = (CSector*)spatial->spatial.sector;
		if (0 == sector) continue;

		if (!RImplementation.HOM.visible(spatial->spatial.sphere)) continue;

		if ((spatial->spatial.type & STYPE_LIGHTSOURCE))
		{
			// lightsource
			if (light* L = (light*)(spatial->dcast_Light()))
			{
#if	RENDER==R_R1
				RImplementation.L_DB->add_light(L);
#else
				if (L->get_LOD() > ps_r2_shadow_lod_min && L->has_light_visible_from_sectors(*this))
				{
					RImplementation.Lights.add_light(L);
				}
#endif
			}
		}
	}
}

void CDSGraphManager::r_dsgraph_capture_dynamic(CObject* O)
{
	PROF_EVENT("r_dsgraph_capture_dynamic")
	const bool dbg_enabled = PortalTraverseDbg_Enabled();
	PortalTraverseDebugStats* dbg = dbg_enabled ? &PortalTraverseDbg_Get() : nullptr;
	const bool opt_bucket = PortalTraverseDbg_IsOptions(i_options);

	if (i_start)
	{
		if (psDeviceFlags.test(rsDrawDynamic))
		{
			// Traverse object database
			g_SpatialSpace->q_frustum
			(
				lstRenderables,
				ISpatial_DB::O_ORDERED,
				i_doptions,
				i_frustum
			);
			if (dbg)
			{
				const u32 spatial_count = u32(lstRenderables.size());
				dbg->dynamic_spatials += spatial_count;
				if (opt_bucket)
					dbg->dynamic_spatials_opt += spatial_count;
				else
					dbg->dynamic_spatials_noopt += spatial_count;
			}
			set_Object();
#if	RENDER==R_R1
			if (i_mask[CDSGraphManager::fl_normal])//normal phase
			{
				std::sort(lstRenderables.begin(), lstRenderables.end(), [](const ISpatialShared& _1, const ISpatialShared& _2) noexcept
				{
					if (!_1.get() || !_2.get()) return false;

					return	_1->spatial.sphere.P.distance_to_sqr(Device.vCameraPosition_saved) < _2->spatial.sphere.P.distance_to_sqr(Device.vCameraPosition_saved);
				});

				if (ps_actor_shadow_flags.test(1))
					g_hud->Render_First(dcast_IPortalTraverser());

				r_dsgraph_capture_hud();
			}
#endif
			u32 uID_LTRACK = u32(-1);
			if (i_mask[CDSGraphManager::fl_normal])//normal phase
			{
				// update light-vis for current entity / actor
				if (CObject* O = g_pGameLevel->CurrentViewEntity())
				{
					if (!O->getDestroy())
					{
						if (CROS_impl* R = (CROS_impl*)O->ROS())
							R->update(O);
					}
				}

				RImplementation.uLastLTRACK++;
				if (!lstRenderables.empty())
				{
					uID_LTRACK = RImplementation.uLastLTRACK % lstRenderables.size();
#if	RENDER!=R_R1
					// update light-vis for selected entity
					// track lighting environment
					if (IRenderable* renderable = (IRenderable*)lstRenderables[uID_LTRACK]->dcast_Renderable())
					{
						if (CROS_impl* T = (CROS_impl*)renderable->renderable_ROS())
							T->update(renderable);
					}
#endif
				}

			}

			// Determine visibility for dynamic part of scene
			for (u32 o_it = 0; o_it < lstRenderables.size(); o_it++)
			{
				ISpatialShared spatial = lstRenderables[o_it];
				if (0 == spatial) continue;
				CSector* sector = (CSector*)spatial->spatial.sector;
				if (0 == sector) continue;

				if (i_mask[CDSGraphManager::fl_normal] && !RImplementation.HOM.visible(spatial->spatial.sphere))
					continue;

#if	RENDER==R_R1
				if ((spatial->spatial.type & STYPE_GLOW))
				{
					if (CGlow* glow = spatial->dcast_CGlow())
					{
						// It may be an glow
						RImplementation.L_Glows->add(glow);
					}
					continue;
				}
#endif

//				if ((spatial->spatial.type & STYPE_LIGHTSOURCE))
//				{
//					// lightsource
//					if (light* L = (light*)(spatial->dcast_Light()))
//					{
//#if	RENDER==R_R1
//						RImplementation.L_DB->add_light(L);
//#else
//						if (L->get_LOD() > EPS_L && L->has_light_visible_from_sectors(PT))
//						{
//							RImplementation.Lights.add_light(L);
//						}
//#endif
//					}
//					continue;
//				}

				if(!(spatial->spatial.type & STYPE_RENDERABLE) && !(spatial->spatial.type & STYPE_PARTICLE) && !(spatial->spatial.type & STYPE_RENDERABLESHADOW))
					continue;
				if (!is_sector_visible(sector))
					continue;

				for (CFrustum& frustum : m_sector_frustums.find(sector)->val.first)
				{
					if (dbg)
					{
						++dbg->dynamic_frustum_tests;
						if (opt_bucket)
							++dbg->dynamic_frustum_tests_opt;
						else
							++dbg->dynamic_frustum_tests_noopt;
					}

					if (frustum.testSphere_dirty(spatial->spatial.sphere.P, spatial->spatial.sphere.R))
					{
						if (dbg)
						{
							++dbg->dynamic_frustum_hits;
							if (opt_bucket)
								++dbg->dynamic_frustum_hits_opt;
							else
								++dbg->dynamic_frustum_hits_noopt;
						}

						// renderable
						IRenderable* renderable = spatial->dcast_Renderable();
						if (0 == renderable) break;

						if (O && O->dcast_Renderable() == renderable) break;

						// Rendering
#if	RENDER==R_R1
						if (i_mask[CDSGraphManager::fl_normal] && o_it == uID_LTRACK && renderable->renderable_ROS())
						{
							// track lighting environment
							if(CROS_impl* T = (CROS_impl*)renderable->renderable_ROS())
								T->update(renderable);
						}
#endif
						if (i_mask[CDSGraphManager::fl_normal] && !(spatial->spatial.type & STYPE_PARTICLE))
							set_Object(renderable);

						renderable->renderable_Render(dcast_IPortalTraverser());
						if (dbg)
						{
							++dbg->dynamic_rendered;
							if (opt_bucket)
								++dbg->dynamic_rendered_opt;
							else
								++dbg->dynamic_rendered_noopt;
						}

						if (i_mask[CDSGraphManager::fl_normal] && !(spatial->spatial.type & STYPE_PARTICLE))
							set_Object();
						break;
					}
				}
			}
		}
	}
}

void CDSGraphManager::r_dsgraph_capture(bool lights, bool dynamic, CObject* O)
{
	PROF_EVENT("r_dsgraph_capture")
	r_dsgraph_capture_static();

	if(lights)
		r_dsgraph_capture_lights();

	if (dynamic)
		r_dsgraph_capture_dynamic(O);
}

void CDSGraphManager::r_dsgraph_capture_metal_world()
{
	PROF_EVENT("r_dsgraph_capture_metal_world")
	xray::metal::particles::ResetFrameBatch();
	struct CaptureCandidate
	{
		ISpatialShared spatial;
		bool mainView = false;
	};
	xr_vector<CaptureCandidate> candidates;
	std::unordered_map<ISpatial*, std::size_t> candidateIndices;
	xr_vector<ISpatialShared> queryResults;
	const auto appendCandidates = [&](const xr_vector<ISpatialShared>& query, const bool mainView)
	{
		for (const ISpatialShared& spatial : query)
		{
			if (!spatial)
				continue;
			const auto [entry, inserted] = candidateIndices.emplace(spatial.get(), candidates.size());
			if (inserted)
				candidates.push_back({spatial, mainView});
			else if (mainView)
				candidates[entry->second].mainView = true;
		}
	};
	CFrustum frustum;
	frustum.CreateFromMatrix(Device.mFullTransform, FRUSTUM_P_LRTB | FRUSTUM_P_FAR);
	const bool captureWorld = g_SpatialSpace && psDeviceFlags.test(rsDrawDynamic);
	if (captureWorld)
	{
		g_SpatialSpace->q_frustum(queryResults, ISpatial_DB::O_ORDERED,
			STYPE_RENDERABLE | STYPE_RENDERABLESHADOW | STYPE_PARTICLE, frustum);
		appendCandidates(queryResults, true);
	}

	// Shadow producers need active spatial renderables that can affect the
	// actual source light volumes, even when the main camera cannot see them.
	// Sun uses the same Windows FixedConvexVolume cascade caster planes; local
	// point/spot lights contribute a conservative range sphere and their tile
	// LVP performs the final exact per-light cull later.
	std::size_t shadowSpatialHits = 0u;
	if (captureWorld)
	{
		const CEnvDescriptorMixer* current = g_pGamePersistent ?
			g_pGamePersistent->Environment().CurrentEnv : nullptr;
		const std::array<float, 3> sizes = {ps_ssfx_shadow_cascades.x,
			ps_ssfx_shadow_cascades.y, ps_ssfx_shadow_cascades.z};
		Fvector sunDirection;
		if (current)
			sunDirection.set(current->sun_dir);
		else
			sunDirection.set(0.f, -1.f, 0.f);
		Fvector sunPosition;
		sunPosition.mad(Device.vCameraPosition, sunDirection, -500.f);
		if (light* adaptedSun = static_cast<light*>(RImplementation.Lights.sun_adapted._get()))
			sunDirection.set(adaptedSun->direction);
		sunPosition.mad(Device.vCameraPosition, sunDirection, -500.f);
		Fvector forward{Device.mView._13, Device.mView._23, Device.mView._33};
		const bool resetLastCascade = r2_advanced_pp && ps_sunshafts_mode && current &&
			current->m_fSunShaftsIntensity >= 0.0001f;
		const R4SunCascadeSet sunCascades = BuildMetalR4SunCascades(Device.mView,
			Device.mProject, Device.vCameraPosition, forward, sunPosition, sunDirection,
			sizes, 2048u, resetLastCascade, Device.dwFrame);
		if (sunCascades.valid)
		{
			for (const R4SunCascade& cascade : sunCascades.cascades)
			{
				CFrustum casterFrustum;
				casterFrustum._clear();
				for (const std::array<float, 4>& values : cascade.casterPlanes)
				{
					Fplane plane;
					plane.n.set(values[0], values[1], values[2]);
					plane.d = values[3];
					casterFrustum._add(plane);
				}
				g_SpatialSpace->q_frustum(queryResults, ISpatial_DB::O_ORDERED,
					STYPE_RENDERABLE | STYPE_RENDERABLESHADOW, casterFrustum);
				shadowSpatialHits += queryResults.size();
				appendCandidates(queryResults, false);
			}
		}

		const float fogDistance = current ? current->fog_distance :
			std::numeric_limits<float>::max();
		for (light* source : RImplementation.v_all_lights)
		{
			if (!source || !source->flags.bActive || !source->flags.bShadow ||
				!source->SpatialComponent || source->flags.bHudMode)
				continue;
			const auto type = static_cast<IRender_Light::LT>(source->flags.type);
			if (type != IRender_Light::POINT && type != IRender_Light::SPOT)
				continue;
			Fvector center;
			center.set(source->SpatialComponent->spatial.sphere.P);
			if (!frustum.testSphere_dirty(center,
					source->SpatialComponent->spatial.sphere.R) ||
				(source->flags.bStatic && !ps_r2_ls_flags.test(R2FLAG_R1LIGHTS)) ||
				Device.vCameraPosition.distance_to_sqr(center) > _sqr(fogDistance) ||
				!(source->get_LOD() > ps_r2_shadow_lod_min) ||
				!std::isfinite(source->range) || source->range <= 0.f)
				continue;
			g_SpatialSpace->q_sphere(queryResults, ISpatial_DB::O_ORDERED,
				STYPE_RENDERABLE | STYPE_RENDERABLESHADOW,
				source->position, source->range);
			shadowSpatialHits += queryResults.size();
			appendCandidates(queryResults, false);
		}
	}

	metalWorldCaptureStaging.clear();
	metalWorldCaptureStaging.reserve(candidates.size());
	metalWorldCaptureKinematics = nullptr;
	metalWorldCaptureActive = true;
	std::size_t cameraCandidateCount = 0u;
	std::size_t shadowOnlyCandidateCount = 0u;
	for (const CaptureCandidate& candidate : candidates)
	{
		const ISpatialShared& spatial = candidate.spatial;
		if (!spatial)
			continue;
		if (candidate.mainView)
			++cameraCandidateCount;
		else
		{
			++shadowOnlyCandidateCount;
			if (spatial->spatial.type & STYPE_PARTICLE)
				continue;
		}
		IRenderable* renderable = spatial->dcast_Renderable();
		if (!renderable || !renderable->renderable.visual)
			continue;
		// Ask the object to submit exactly what legacy R4 would draw.  In first
		// person this captures player_legs_controller's replacement model and
		// m_legs_transform instead of substituting the actor's full visual/XFORM.
		if (!(spatial->spatial.type & STYPE_PARTICLE))
			set_Object(renderable);
		metalWorldCaptureObjectCastsShadow = renderable->renderable_ShadowGenerate() != FALSE;
		metalWorldCaptureShadowAllowed = true;
		renderable->renderable_Render(dcast_IPortalTraverser());
		metalWorldCaptureObjectCastsShadow = false;
		metalWorldCaptureShadowAllowed = false;
		if (!(spatial->spatial.type & STYPE_PARTICLE))
			set_Object();
	}
	metalWorldCaptureActive = false;
	metalWorldCaptureKinematics = nullptr;

	const u32 capturedCount = u32(metalWorldCaptureStaging.size());
	{
		xrSRWLockGuard guard(&S_LC, false);
		metalWorldRenderables = std::move(metalWorldCaptureStaging);
		metalWorldFrame = Device.dwFrame;
	}

	static u32 lastLoggedFrame = u32(-1);
	if (lastLoggedFrame == u32(-1) || Device.dwFrame - lastLoggedFrame >= 300)
	{
		lastLoggedFrame = Device.dwFrame;
		Msg("* METAL_WORLD_CAPTURE frame=%u spatial=%u main=%u shadow_only=%u shadow_hits=%u renderables=%u",
			Device.dwFrame, u32(candidates.size()), u32(cameraCandidateCount),
			u32(shadowOnlyCandidateCount), u32(shadowSpatialHits), capturedCount);
	}
}

void CDSGraphManager::r_dsgraph_capture_metal_lights()
{
	PROF_EVENT("r_dsgraph_capture_metal_lights")
	metalLocalLightCaptureStaging.clear();
	metalLocalLightCaptureStats = {};
	if (Device.dwFrame < 64u)
		Msg("* METAL_LIGHT_CAPTURE_ENTRY frame=%u", Device.dwFrame);
	IMainMenu* mainMenu = g_pGamePersistent ? g_pGamePersistent->m_pMainMenu : nullptr;
	const bool sceneReady = g_pGameLevel && g_hud &&
		!(mainMenu && mainMenu->CanSkipSceneRendering());
	if (Device.dwFrame < 64u)
		Msg("* METAL_LIGHT_CAPTURE_ADMISSION frame=%u ready=%s level=%s hud=%s menu=%s", Device.dwFrame,
			sceneReady ? "true" : "false", g_pGameLevel ? "true" : "false", g_hud ? "true" : "false",
			mainMenu ? "present" : "missing");
	if (!sceneReady)
	{
		{
			xrSRWLockGuard guard(&S_LC, false);
			metalLocalLights.clear();
			metalLocalLightPublishedStats = {};
			metalLocalLightFrame = Device.dwFrame;
		}
		static u32 lastSkippedFrame = u32(-1);
		if (lastSkippedFrame == u32(-1) || Device.dwFrame - lastSkippedFrame >= 300u)
		{
			lastSkippedFrame = Device.dwFrame;
			Msg("* METAL_LIGHT_CAPTURE_SKIPPED frame=%u reason=scene_not_ready", Device.dwFrame);
		}
		return;
	}
	// End() invokes this after the engine object update and before Metal frame
	// encoding. Do not infer readiness from the dynamic-world vector:
	// static-only views legitimately have zero dynamic renderables while still
	// needing lamps and sun light.
	if (Device.dwFrame < 64u)
		Msg("* METAL_LIGHT_CAPTURE_FRUSTUM_BEGIN frame=%u", Device.dwFrame);
	CFrustum frustum;
	frustum.CreateFromMatrix(Device.mFullTransform, FRUSTUM_P_LRTB | FRUSTUM_P_FAR);
	if (Device.dwFrame < 64u)
		Msg("* METAL_LIGHT_CAPTURE_FRUSTUM_READY frame=%u", Device.dwFrame);
	const u32 registeredLightCount = u32(RImplementation.v_all_lights.size());
	if (Device.dwFrame < 64u)
		Msg("* METAL_LIGHT_CAPTURE_REGISTRY_READY frame=%u registered=%u", Device.dwFrame, registeredLightCount);
	static u32 lastBeginLoggedFrame = u32(-1);
	if (lastBeginLoggedFrame == u32(-1) || Device.dwFrame - lastBeginLoggedFrame >= 300u)
	{
		lastBeginLoggedFrame = Device.dwFrame;
		Msg("* METAL_LIGHT_CAPTURE_BEGIN frame=%u source=engine_registry registered=%u", Device.dwFrame,
			registeredLightCount);
	}
	// Direct Metal deliberately does not load the D3D sector graph. Its light
	// octree can therefore exist as an object while its traversal storage is
	// invalid; q_frustum then faults before it can return an empty candidate
	// list. v_all_lights is the engine's authoritative registration set. It is
	// read only on this update thread after Lights.Update(), filtered below by
	// the same camera-space gates, and converted to value records before End().
	// This is not the old raw-registry submission: no pointer crosses the
	// backend boundary and all admission/LOD/HUD transforms happen here.
	metalLocalLightCaptureStaging.clear();
	metalLocalLightCaptureStaging.reserve(registeredLightCount * 6u);
	metalLocalLightCaptureStats.spatial = registeredLightCount;

	static const Fvector omniNorm[6] = {
		{0.f, 1.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, -1.f},
		{0.f, 0.f, 1.f}, {0.f, 1.f, 0.f}, {0.f, 1.f, 0.f}};
	static const Fvector omniDir[6] = {
		{1.f, 0.f, 0.f}, {-1.f, 0.f, 0.f}, {0.f, 1.f, 0.f},
		{0.f, -1.f, 0.f}, {0.f, 0.f, 1.f}, {0.f, 0.f, -1.f}};

	const float fogDistance = g_pGamePersistent && g_pGamePersistent->Environment().CurrentEnv ?
		g_pGamePersistent->Environment().CurrentEnv->fog_distance : std::numeric_limits<float>::max();
	for (light* source : RImplementation.v_all_lights)
	{
		if (!source)
			continue;
		const auto type = static_cast<IRender_Light::LT>(source->flags.type);
		// Point parents are expanded into six immutable OmniFace records below.
		// Their D3D-only OMNIPART children must not become duplicate lights.
		if (type == IRender_Light::DIRECT || type == IRender_Light::REFLECTED ||
			type == IRender_Light::OMNIPART)
			continue;
		if (!source->flags.bActive || !source->SpatialComponent)
		{
			++metalLocalLightCaptureStats.rejectedInactive;
			continue;
		}
		Fvector sphereCenter;
		sphereCenter.set(source->SpatialComponent->spatial.sphere.P);
		if (!frustum.testSphere_dirty(sphereCenter, source->SpatialComponent->spatial.sphere.R))
		{
			++metalLocalLightCaptureStats.rejectedFrustum;
			continue;
		}
		// Metal has no loaded D3D portal/HOM graph. Treat that unsupported filter
		// as a documented fallback instead of dereferencing stale sector state.
		++metalLocalLightCaptureStats.sectorFallback;
		if (source->flags.bStatic && !ps_r2_ls_flags.test(R2FLAG_R1LIGHTS))
			continue;
		if (Device.vCameraPosition.distance_to_sqr(sphereCenter) > _sqr(fogDistance))
		{
			++metalLocalLightCaptureStats.rejectedFog;
			continue;
		}
		const float lod = source->get_LOD();
		if (!(lod > ps_r2_shadow_lod_min))
		{
			++metalLocalLightCaptureStats.rejectedLod;
			continue;
		}

		const u64 parentIdentity = static_cast<u64>(reinterpret_cast<uintptr_t>(source));
		bool decorative = false;
		for (CObject* object : source->decor_object)
			decorative = decorative || (object && !object->getDestroy());
		const float cameraDistance = Device.vCameraPosition.distance_to(sphereCenter);
		const auto append = [&](const MetalLocalLight::Type type, const Fvector& direction,
			const Fvector& right, const u8 omniFace)
		{
			MetalLocalLight target;
			target.parentIdentity = parentIdentity;
            target.identity = (parentIdentity << 3u) | (omniFace == 0xff ? 0u : u64(omniFace + 1u));
			target.type = type;
			target.position.set(source->position);
			target.direction.set(direction);
			target.right.set(right);
			target.color.set(source->color);
			target.color.mul_rgb(lod);
			target.range = source->range;
			target.cone = type == MetalLocalLight::Type::OmniFace ? PI_DIV_2 : source->cone;
			target.virtualSize = source->virtual_size;
			target.active = true;
			target.staticLight = source->flags.bStatic;
			target.shadowed = source->flags.bShadow;
			target.decorative = decorative;
			target.cameraDistance = cameraDistance;
			target.hudMode = source->flags.bHudMode;
			target.omniFace = omniFace;
			target.projectorTexture = source->projector_texture;
			if (target.hudMode)
			{
				Device.hud_to_world(target.position);
				Device.hud_to_world_dir(target.direction);
			}
			metalLocalLightCaptureStaging.emplace_back(std::move(target));
		};
		if (type == IRender_Light::POINT && source->flags.bShadow)
		{
			for (u8 face = 0; face != 6; ++face)
			{
				Fvector right;
				right.crossproduct(omniNorm[face], omniDir[face]);
				append(MetalLocalLight::Type::OmniFace, omniDir[face], right, face);
			}
		}
		else if (type == IRender_Light::POINT)
			append(MetalLocalLight::Type::Point, source->direction, source->right, 0xff);
		else if (type == IRender_Light::SPOT)
			append(MetalLocalLight::Type::Spot, source->direction, source->right, 0xff);
		else if (type == IRender_Light::OMNIPART)
			append(MetalLocalLight::Type::OmniFace, source->direction, source->right,
				static_cast<u8>(source->omnipart_num));
	}
	metalLocalLightCaptureStats.captured = u32(metalLocalLightCaptureStaging.size());
	metalLocalLightCaptureStats.admitted = metalLocalLightCaptureStats.captured;
	{
		xrSRWLockGuard guard(&S_LC, false);
		metalLocalLights = std::move(metalLocalLightCaptureStaging);
		metalLocalLightPublishedStats = metalLocalLightCaptureStats;
		metalLocalLightFrame = Device.dwFrame;
	}
	if (Device.dwFrame == 0u || Device.dwFrame % 300u == 0u)
		Msg("* METAL_LIGHT_CAPTURE frame=%u registered=%u captured=%u sector_fallback=%u rejected(frustum=%u inactive=%u sector=%u hom=%u fog=%u lod=%u)",
			Device.dwFrame, metalLocalLightCaptureStats.spatial, metalLocalLightCaptureStats.captured,
			metalLocalLightCaptureStats.sectorFallback,
			metalLocalLightCaptureStats.rejectedFrustum, metalLocalLightCaptureStats.rejectedInactive,
			metalLocalLightCaptureStats.rejectedSector,
			metalLocalLightCaptureStats.rejectedHom, metalLocalLightCaptureStats.rejectedFog,
			metalLocalLightCaptureStats.rejectedLod);
}

void CDSGraphManager::r_dsgraph_capture_metal_hud()
{
    // The HUD models are generated outside the spatial database.  Keep all
    // writes in capture-thread-only staging until the complete near-view
    // traversal has finished; End() may copy the published vector meanwhile.
    metalHudCaptureStaging.clear();
    metalHudCaptureStats = {};
    struct CaptureScope final
    {
        CDSGraphManager& graph;
		explicit CaptureScope(CDSGraphManager& value) : graph(value)
		{
			graph.metalHudCaptureKinematics = nullptr;
			graph.metalHudCaptureActive = true;
		}
		~CaptureScope()
		{
			graph.metalHudCaptureActive = false;
			graph.metalHudCaptureKinematics = nullptr;
		}
    } capture(*this);
    // CHudInitializer owns the HUD view/projection transform. rmNear/rmNormal
    // only change the legacy D3D viewport through IRender_Target/HW.pContext;
    // both are deliberately absent for Direct Metal and must not be touched by
    // this value-only capture traversal.
    CHudInitializer initializer(true);
    if (g_hud)
        g_hud->Render_Last(dcast_IPortalTraverser());
	validate_metal_hud_scope_capture();
	MetalHudScopeGlobals capturedScopeGlobals;
	capturedScopeGlobals.hudParams.set(0.f, 0.f, 0.f, 0.f);
	capturedScopeGlobals.hudFovParams.set(0.f, 0.f, 0.f, 0.f);
	capturedScopeGlobals.ogseCScreen.set(0.f, 0.f, 0.f, 0.f);
	capturedScopeGlobals.s3dsParam1.set(ps_s3ds_param_1);
	capturedScopeGlobals.s3dsParam2.set(ps_s3ds_param_2);
	capturedScopeGlobals.s3dsParam3.set(ps_s3ds_param_3);
	capturedScopeGlobals.s3dsParam4.set(ps_s3ds_param_4);
	capturedScopeGlobals.markswitchCurrent.set(float(ps_markswitch_current), 0.f, 0.f, 0.f);
	capturedScopeGlobals.markswitchColor.set(ps_markswitch_color);
	capturedScopeGlobals.shaderParam8.set(ps_dev_param_8);
	if (g_pGamePersistent)
	{
		// Match cl_hud_params/cl_hud_fov_params exactly, rather than deriving
		// values from the current weapon after the legacy binding point.
		capturedScopeGlobals.hudParams.set(g_pGamePersistent->m_pGShaderConstants->hud_params);
		capturedScopeGlobals.hudFovParams.set(g_pGamePersistent->m_pGShaderConstants->hud_fov_params);
		const float fov = Device.fFOV;
		const float aspect = Device.fASPECT;
		const float farPlane = g_pGamePersistent->Environment().CurrentEnv ?
			g_pGamePersistent->Environment().CurrentEnv->far_plane * .75f : 0.f;
		// Match cl_screen_params, the authoritative ogse_c_screen binder.
		capturedScopeGlobals.ogseCScreen.set(fov, aspect, tan(deg2rad(fov) / 2.f), farPlane);
	}
    {
        xrSRWLockGuard guard(&S_LC, false);
        metalHudRenderables = std::move(metalHudCaptureStaging);
		metalHudPublishedStats = metalHudCaptureStats;
		metalHudScopeGlobals = capturedScopeGlobals;
        metalHudView.set(Device.mViewHud);
        // A newly attached CHudItem exposes its interpolated HUD FOV as zero
        // for the first update. build_projection() turns that transient value
        // into infinities, which native Metal cannot admit as clip-space data.
        // The legacy backend happens to discard that frame. Keep the capture
        // finite with the configured default HUD FOV until the interpolated
        // item FOV becomes valid on a following update.
        const bool currentHudProjectionValid = _valid(Device.mProjectHud);
        const bool previousHudProjectionValid = _valid(Device.mProjectHud_prev);
		Fmatrix fallbackHudProjection;
		if (!currentHudProjectionValid || !previousHudProjectionValid)
		{
			const float aspect = _valid(Device.fASPECT) && Device.fASPECT > EPS_S ?
				Device.fASPECT : Device.mProject_saved._11 / Device.mProject_saved._22;
			const float projectionDelta = Device.mProject_saved._33 - 1.f;
			const float farPlane = _valid(projectionDelta) && _abs(projectionDelta) > EPS_S ?
				-Device.mProject_saved._43 / projectionDelta : 500.f;
			const float hudFovScale = _valid(psHUD_FOV_def) && psHUD_FOV_def >= .1f ?
				psHUD_FOV_def : .45f;
			fallbackHudProjection.build_projection(
				deg2rad(hudFovScale * 83.f), aspect, R_VIEWPORT_NEAR,
				_valid(farPlane) && farPlane > R_VIEWPORT_NEAR ? farPlane : 500.f);
		}
        metalHudProjection.set(currentHudProjectionValid ? Device.mProjectHud : fallbackHudProjection);
        metalHudPreviousView.set(Device.mViewHud_prev);
        metalHudPreviousProjection.set(previousHudProjectionValid ?
			Device.mProjectHud_prev : fallbackHudProjection);
        metalHudFrame = Device.dwFrame;
		if (!currentHudProjectionValid)
		{
			static u32 lastProjectionFallbackFrame = u32(-1);
			static u32 lastProjectionFallbackRequested = u32(-1);
			if (lastProjectionFallbackFrame == u32(-1) ||
				lastProjectionFallbackRequested != metalHudCaptureStats.requested ||
				Device.dwFrame - lastProjectionFallbackFrame >= 300u)
			{
				Msg("* METAL_HUD_PROJECTION_FALLBACK frame=%u requested=%u reason=nonfinite_hud_projection",
					Device.dwFrame, metalHudCaptureStats.requested);
				lastProjectionFallbackFrame = Device.dwFrame;
				lastProjectionFallbackRequested = metalHudCaptureStats.requested;
			}
		}
    }
	static u32 lastLoggedFrame = u32(-1);
	static u32 lastLoggedRequested = u32(-1);
	static bool lastLoggedHudManager = false;
	const bool hasHudManager = g_hud != nullptr;
	if (lastLoggedFrame == u32(-1) || lastLoggedRequested != metalHudCaptureStats.requested ||
		lastLoggedHudManager != hasHudManager || Device.dwFrame - lastLoggedFrame >= 300u)
	{
		Msg("* METAL_HUD_CAPTURE_PRECONDITION frame=%u phase=%u hud_manager=%s roots=%u hierarchy=%u skeleton=%u child_edges=%u traversed=%u ssa_rejected=%u hom_rejected=%u shader_rejected=%u priority_rejected=%u requested=%u admitted=%u aggregate=%u strict=%u scope_forward=%u scope_back=%u scope_zwrite=%u scope_reticle_precise=%u scope_triplets=%u scope_partial_rejected=%u scope_duplicate_rejected=%u non_hq=%u",
			Device.dwFrame, u32(RImplementation.phase), hasHudManager ? "true" : "false",
			metalHudCaptureStats.roots, metalHudCaptureStats.hierarchyNodes,
			metalHudCaptureStats.skeletonNodes, metalHudCaptureStats.childEdges,
			metalHudCaptureStats.traversed, metalHudCaptureStats.ssaRejected,
			metalHudCaptureStats.homRejected, metalHudCaptureStats.shaderRejected,
			metalHudCaptureStats.priorityRejected,
			metalHudCaptureStats.requested, metalHudCaptureStats.admitted,
			metalHudCaptureStats.aggregateFallback, metalHudCaptureStats.strict,
			metalHudCaptureStats.scopeForward, metalHudCaptureStats.scopeBack,
			metalHudCaptureStats.scopeZwrite, metalHudCaptureStats.scopeReticlePrecise,
			metalHudCaptureStats.scopeTriplets, metalHudCaptureStats.scopePartialRejected,
			metalHudCaptureStats.scopeDuplicateRejected, metalHudCaptureStats.nonHq);
		lastLoggedFrame = Device.dwFrame;
		lastLoggedRequested = metalHudCaptureStats.requested;
		lastLoggedHudManager = hasHudManager;
	}
}

void CDSGraphManager::validate_metal_hud_scope_capture()
{
	u32 roleCounts[3] = {};
	u32 total = 0u;
	bool roleMismatch = false;
	for (const MetalHudRenderable& renderable : metalHudCaptureStaging)
	{
		for (const MetalHudRenderable::ScopeLeaf& leaf : renderable.scopeLeaves)
		{
			++roleCounts[u32(leaf.role)];
			++total;
			const u8 selected = leaf.selectedShader.scopeLense;
			if (selected != 0u && selected != u8(leaf.role) + 1u)
				roleMismatch = true;
		}
	}
	metalHudCaptureStats.scopeBack = 0u;
	metalHudCaptureStats.scopeZwrite = 0u;
	metalHudCaptureStats.scopeReticlePrecise = 0u;
	metalHudCaptureStats.scopeTriplets = 0u;
	if (total == 0u)
		return;

	const bool enabled = scope_3D_fake_enabled != 0;
	if (!enabled)
	{
		for (MetalHudRenderable& renderable : metalHudCaptureStaging)
			renderable.scopeLeaves.clear();
		return;
	}
	const bool completeTriplet = roleCounts[u32(MetalHudRenderable::ScopeRole::Back)] == 1u &&
		roleCounts[u32(MetalHudRenderable::ScopeRole::Zwrite)] == 1u &&
		roleCounts[u32(MetalHudRenderable::ScopeRole::ReticlePrecise)] == 1u;
	if (completeTriplet && !roleMismatch)
	{
		metalHudCaptureStats.scopeBack = 1u;
		metalHudCaptureStats.scopeZwrite = 1u;
		metalHudCaptureStats.scopeReticlePrecise = 1u;
		metalHudCaptureStats.scopeTriplets = 1u;
		return;
	}

	metalHudCaptureStats.scopePartialRejected += total;
	for (const u32 count : roleCounts)
		if (count > 1u)
			metalHudCaptureStats.scopeDuplicateRejected += count - 1u;
	if (roleMismatch)
		metalHudCaptureStats.shaderRejected += total;
	for (MetalHudRenderable& renderable : metalHudCaptureStaging)
		renderable.scopeLeaves.clear();
}

void CDSGraphManager::copy_metal_world_renderables(xr_vector<MetalWorldRenderable>& out, u32& frame)
{
	xrSRWLockGuard guard(&S_LC, true);
	out = metalWorldRenderables;
	frame = metalWorldFrame;
}

void CDSGraphManager::copy_metal_hud_renderables(xr_vector<MetalHudRenderable>& out, Fmatrix& view,
    Fmatrix& projection, Fmatrix& previousView, Fmatrix& previousProjection, MetalHudCaptureStats& stats,
	MetalHudScopeGlobals& scopeGlobals, u32& frame)
{
    xrSRWLockGuard guard(&S_LC, true);
    out = metalHudRenderables;
    view.set(metalHudView); projection.set(metalHudProjection);
    previousView.set(metalHudPreviousView); previousProjection.set(metalHudPreviousProjection);
    stats = metalHudPublishedStats;
	scopeGlobals = metalHudScopeGlobals;
    frame = metalHudFrame;
}

void CDSGraphManager::copy_metal_local_lights(xr_vector<MetalLocalLight>& out,
	MetalLocalLightCaptureStats& stats, u32& frame)
{
	xrSRWLockGuard guard(&S_LC, true);
	out = metalLocalLights;
	stats = metalLocalLightPublishedStats;
	frame = metalLocalLightFrame;
}
