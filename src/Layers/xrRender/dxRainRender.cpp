#include "stdafx.h"
#include "dxRainRender.h"

#include "../../xrEngine/Rain.h"
#include "../../Include/xrRender/R4BackendSelection.h"
#include "MetalRainCapture.h"

//	Warning: duplicated in rain.cpp
static const int max_desired_items = 2500;
static const float source_radius = 15;//12.5f;
static const float source_offset = 20.f; // 40
static const float max_distance = source_offset * 1.5f; //1.25f;
static const float sink_offset = -(max_distance - source_offset);
static const float drop_length = 5.f;
static const float drop_width = 0.30f;
static const float drop_angle = 3.0f;
static const float drop_max_angle = deg2rad(35.f); //10
static const float drop_max_wind_vel = 20.0f;
static const float drop_speed_min = 40.f;
static const float drop_speed_max = 80.f;

const int max_particles = 1000;
const int particles_cache = 400;
const float particles_time = .3f;

xr_atomic_u32 current_items;
extern float r_rain_exp;
extern float r_rain_k;
dxRainRender::dxRainRender()
{
	if (SelectedR4Backend() == R4BackendKind::Metal)
		return;
	current_items = 0;

	IReader* F = FS.r_open("$game_meshes$", "dm\\rain.dm");
	VERIFY3(F, "Can't open file.", "dm\\rain.dm");

	DM_Drop = ::RImplementation.model_CreateDM(F);

	//
	SH_Rain.create("effects\\rain", "fx\\fx_rain");
	hGeom_Rain.create(FVF::F_LIT, RCache.Vertex.Buffer(), RCache.QuadIB);
	hGeom_Drops.create(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1, RCache.Vertex.Buffer(), RCache.Index.Buffer());

#if defined(USE_DX11)
	if (RImplementation.o.ssfx_rain)
		SH_Splash.create("effects\\rain_splash", "fx\\fx_rain");
#endif

	FS.r_close(F);
}

dxRainRender::~dxRainRender()
{
	::RImplementation.model_Delete(DM_Drop);
}

void dxRainRender::Copy(IRainRender& _in)
{
	*this = *(dxRainRender*)&_in;
}

#include "../../xrEngine/iGame_persistent.h"

void dxRainRender::Render(CEffect_Rain& owner)
{
	if (SelectedR4Backend() == R4BackendKind::Metal)
	{
		const float density = g_pGamePersistent->Environment().CurrentEnv->rain_density;
		if (density < EPS_L)
			return;
		const float factor = _powf(density, r_rain_exp);
		float dropLength = drop_length;
		float dropWidth = drop_width;
		int rainMaxParticles = max_desired_items;
		bool ssfx = false;
#if defined(USE_DX11)
		if (RImplementation.o.ssfx_rain)
		{
			dropLength = ps_ssfx_rain_1.x;
			dropWidth = ps_ssfx_rain_1.y;
			rainMaxParticles = iFloor(ps_ssfx_rain_drops_setup.x);
			ssfx = true;
		}
#endif
		const u32 desired = iFloor(0.01f * (1.f + factor * r_rain_k) * float(rainMaxParticles));
		const u32 limit = _min(desired, static_cast<u32>(owner.items.size()));
		xray::metal::rain::FrameBatch capture;
		capture.shaderSetup = {ps_ssfx_rain_2.x, ps_ssfx_rain_2.y, ps_ssfx_rain_2.z, ps_ssfx_rain_2.w};
		capture.density = density;
		capture.ssfx = ssfx;
		capture.vertices.reserve(_min(limit * 4u, xray::metal::rain::FrameCapture::MaxVertices));
		const float visual = factor * 0.5f + 0.5f;
		const Fvector3 color = g_pGamePersistent->Environment().CurrentEnv->rain_color;
		const u32 packedColor = color_rgba_f(color.x, color.y, color.z, visual);
		static const Fvector2 uv[2][4] = {
			{{0, 1}, {0, 0}, {1, 1}, {1, 0}},
			{{1, 0}, {1, 1}, {0, 0}, {0, 1}}
		};
		const Fvector& eye = Device.vCameraPosition;
		for (CEffect_Rain::Item& item : owner.items)
		{
			Fvector trail;
			trail.mad(item.P, item.D, -dropLength * visual);
			Fvector center, lineDirection;
			center.sub(item.P, trail);
			lineDirection.normalize(center);
			center.mul(0.5f);
			const float radius = center.magnitude();
			center.add(trail);
			if (!::Render->ViewBase.testSphere_dirty(center, radius))
				continue;
			if (capture.vertices.size() + 4u > xray::metal::rain::FrameCapture::MaxVertices)
			{
				capture.droppedVertices += 4u;
				continue;
			}
			Fvector cameraDirection, lineTop, point;
			cameraDirection.sub(center, eye);
			cameraDirection.normalize();
			lineTop.crossproduct(cameraDirection, lineDirection);
			const u32 set = item.uv_set;
			auto append = [&capture, packedColor](const Fvector& position, const Fvector2& texcoord) {
				xray::metal::particles::SpriteVertex vertex{};
				vertex.position[0] = position.x; vertex.position[1] = position.y; vertex.position[2] = position.z;
				vertex.d3dColor = packedColor;
				vertex.texcoord[0] = texcoord.x; vertex.texcoord[1] = texcoord.y;
				capture.vertices.push_back(vertex);
			};
			point.mad(trail, lineTop, -dropWidth); append(point, uv[set][0]);
			point.mad(trail, lineTop,  dropWidth); append(point, uv[set][1]);
			point.mad(item.P, lineTop, -dropWidth); append(point, uv[set][2]);
			point.mad(item.P, lineTop,  dropWidth); append(point, uv[set][3]);
			if (capture.vertices.size() / 4u >= limit)
				break;
		}
		xray::metal::rain::SubmitFrame(std::move(capture));
		return;
	}
	float factor = g_pGamePersistent->Environment().CurrentEnv->rain_density;
	if (factor < EPS_L) return;
    factor = _powf(factor, r_rain_exp);

	float _drop_len = drop_length;
	float _drop_width = drop_width;
	float _drop_speed = 1.0f;
	ref_shader& _splash_SH = DM_Drop->shader;
	static shared_str s_shader_setup = "ssfx_rain_setup";

	int rain_max_particles = max_desired_items;
	float rain_radius = source_radius;

	// SSS Rain shader is available
#if defined(USE_DX11)
	if (RImplementation.o.ssfx_rain)
	{
		_drop_len = ps_ssfx_rain_1.x;
		_drop_width = ps_ssfx_rain_1.y;
		_drop_speed = ps_ssfx_rain_1.z;
		_splash_SH = SH_Splash;
		rain_max_particles = ps_ssfx_rain_drops_setup.x;
		rain_radius = ps_ssfx_rain_drops_setup.y;
	}
#endif

	u32 desired_items = iFloor(0.01f * (1.f + factor * r_rain_k) * float(rain_max_particles));
	u32 render_items = _min(desired_items, (u32)owner.items.size());
	if (!render_items)
		return;

	// Get to the desired items
	u32 local_current = current_items.load(std::memory_order_relaxed);
	if (local_current < desired_items)
	{
		u32 delta = desired_items - local_current;
		current_items.fetch_add(delta, std::memory_order_relaxed);
	}	

	// visual
	float factor_visual = factor / 2.f + .5f;
	Fvector3 f_rain_color = g_pGamePersistent->Environment().CurrentEnv->rain_color;
	u32 u_rain_color = color_rgba_f(f_rain_color.x, f_rain_color.y, f_rain_color.z, factor_visual);

	// perform update
	u32 vOffset;
	FVF::LIT* verts = (FVF::LIT *)RCache.Vertex.Lock(render_items * 4, hGeom_Rain->vb_stride, vOffset);
	FVF::LIT* start = verts;
	const Fvector& vEye = Device.vCameraPosition;
	u32 rendered_items = 0;
	for (CEffect_Rain::Item& one : owner.items)
	{
		// Build line
		Fvector& pos_head = one.P;
		Fvector pos_trail;
		pos_trail.mad(pos_head, one.D, -_drop_len * factor_visual);

		// Culling
		Fvector sC, lineD;
		float sR;
		sC.sub(pos_head, pos_trail);
		lineD.normalize(sC);
		sC.mul(.5f);
		sR = sC.magnitude();
		sC.add(pos_trail);
		if (!::Render->ViewBase.testSphere_dirty(sC, sR)) continue;

		static Fvector2 UV[2][4] = {
			{{0, 1}, {0, 0}, {1, 1}, {1, 0}},
			{{1, 0}, {1, 1}, {0, 0}, {0, 1}}
		};

		// Everything OK - build vertices
		Fvector P, lineTop, camDir;
		camDir.sub(sC, vEye);
		camDir.normalize();
		lineTop.crossproduct(camDir, lineD);
		float w = _drop_width;
		u32 s = one.uv_set;
		P.mad(pos_trail, lineTop, -w);
		verts->set(P, u_rain_color, UV[s][0].x, UV[s][0].y);
		verts++;
		P.mad(pos_trail, lineTop, w);
		verts->set(P, u_rain_color, UV[s][1].x, UV[s][1].y);
		verts++;
		P.mad(pos_head, lineTop, -w);
		verts->set(P, u_rain_color, UV[s][2].x, UV[s][2].y);
		verts++;
		P.mad(pos_head, lineTop, w);
		verts->set(P, u_rain_color, UV[s][3].x, UV[s][3].y);
		verts++;

		if (++rendered_items >= render_items)
			break;
	}
	u32 vCount = (u32)(verts - start);
	RCache.Vertex.Unlock(vCount, hGeom_Rain->vb_stride);

	// Render if needed
	if (vCount)
	{
		//HW.pDevice->SetRenderState	(D3DRS_CULLMODE,D3DCULL_NONE);
		RCache.set_CullMode(CULL_NONE);
		RCache.set_xform_world(Fidentity);
		RCache.set_Shader(SH_Rain);
		RCache.set_Geometry(hGeom_Rain);
		RCache.Render(D3DPT_TRIANGLELIST, vOffset, 0, vCount, 0, vCount / 2);
		//HW.pDevice->SetRenderState	(D3DRS_CULLMODE,D3DCULL_CCW);
		RCache.set_CullMode(CULL_CCW);
		RCache.set_c(s_shader_setup, ps_ssfx_rain_2); // Alpha, Brigthness, Refraction, Reflection
	}

	// Particles
	CEffect_Rain::Particle* P = owner.particle_active;
	if (0 == P) return;

	{
		float dt = Device.fTimeDelta;
		_IndexStream& _IS = RCache.Index;
		RCache.set_Shader(_splash_SH);
		RCache.set_c(s_shader_setup, ps_ssfx_rain_3); // Alpha, Refraction

		Fmatrix mXform, mScale;
		int pcount = 0;
		u32 v_offset, i_offset;
		u32 vCount_Lock = particles_cache * DM_Drop->number_vertices;
		u32 iCount_Lock = particles_cache * DM_Drop->number_indices;
		IRender_DetailModel::fvfVertexOut* v_ptr = (IRender_DetailModel::fvfVertexOut*)RCache.Vertex.Lock(vCount_Lock,
		                                                                                                  hGeom_Drops->
		                                                                                                  vb_stride,
		                                                                                                  v_offset);
		u16* i_ptr = _IS.Lock(iCount_Lock, i_offset);
		while (P)
		{
			CEffect_Rain::Particle* next = P->next;

			// Update
			// P can be zero sometimes and it crashes
			P->time -= dt;
			if (P->time < 0)
			{
				owner.p_free(P);
				P = next;
				continue;
			}

			// Render
			if (::Render->ViewBase.testSphere_dirty(P->bounds.P, P->bounds.R))
			{
				// Build matrix
				float scale = P->time / particles_time;
				mScale.scale(scale, scale, scale);
				mXform.mul_43(P->mXForm, mScale);

				// XForm verts
				DM_Drop->transfer(mXform, v_ptr, u_rain_color, i_ptr, pcount * DM_Drop->number_vertices);
				v_ptr += DM_Drop->number_vertices;
				i_ptr += DM_Drop->number_indices;
				pcount ++;

				if (pcount >= particles_cache)
				{
					// flush
					u32 dwNumPrimitives = iCount_Lock / 3;
					RCache.Vertex.Unlock(vCount_Lock, hGeom_Drops->vb_stride);
					_IS.Unlock(iCount_Lock);
					RCache.set_Geometry(hGeom_Drops);
					RCache.Render(D3DPT_TRIANGLELIST, v_offset, 0, vCount_Lock, i_offset, dwNumPrimitives);

					v_ptr = (IRender_DetailModel::fvfVertexOut*)RCache.Vertex.Lock(vCount_Lock, hGeom_Drops->vb_stride,
					                                                               v_offset);
					i_ptr = _IS.Lock(iCount_Lock, i_offset);

					pcount = 0;
				}
			}

			P = next;
		}

		// Flush if needed
		vCount_Lock = pcount * DM_Drop->number_vertices;
		iCount_Lock = pcount * DM_Drop->number_indices;
		u32 dwNumPrimitives = iCount_Lock / 3;
		RCache.Vertex.Unlock(vCount_Lock, hGeom_Drops->vb_stride);
		_IS.Unlock(iCount_Lock);
		if (pcount)
		{
			RCache.set_Geometry(hGeom_Drops);
			RCache.Render(D3DPT_TRIANGLELIST, v_offset, 0, vCount_Lock, i_offset, dwNumPrimitives);
		}
	}
}

const Fsphere& dxRainRender::GetDropBounds() const
{
	return DM_Drop->bv_sphere;
}
