#pragma once
// Engine-only bridge. FixedConvexVolume is shared unchanged with R_sun.cpp.
#include "R4SunCascade.h"
#include "../xrRender/R_sun_fixed_support.h"
#include <cstring>
#include <cmath>

inline Fvector3 metalSunWform(Fmatrix& m, Fvector3 const& v)
{
    Fvector4 r;
    r.x=v.x*m._11+v.y*m._21+v.z*m._31+m._41;
    r.y=v.x*m._12+v.y*m._22+v.z*m._32+m._42;
    r.z=v.x*m._13+v.y*m._23+v.z*m._33+m._43;
    r.w=v.x*m._14+v.y*m._24+v.z*m._34+m._44;
    return Fvector3{r.x/r.w,r.y/r.w,r.z/r.w};
}
inline R4SunCascadeSet BuildMetalR4SunCascades(const Fmatrix& view, const Fmatrix& projection,
    const Fvector& eye, const Fvector& forward, const Fvector& sunPosition,
    const Fvector& sunDirection, const std::array<float,3>& sizes,
    std::uint32_t resolution, bool resetLast, std::uint32_t engineFrame)
{
    static Fvector3 corners[8]={{-1,-1,0},{-1,-1,1},{-1,1,1},{-1,1,0},
        {1,1,1},{1,1,0},{1,-1,1},{1,-1,0}};
    static int facetable[6][4]={{6,7,5,4},{1,0,7,6},{1,2,3,0},
        {3,2,4,5},{0,3,5,7},{1,6,4,2}};
    struct Stage {float size; bool reset_chain; Fmatrix xform; xr_vector<sun::ray> rays;};
    std::array<Stage,3> m_sun_cascades{};
    R4SunCascadeSet output;
    output.engineFrame=engineFrame; output.resolution=resolution;
    for (unsigned i=0;i<3;++i)
    {
        if (!std::isfinite(sizes[i]) || sizes[i]<=0 || (i && sizes[i]<=sizes[i-1])) return output;
        m_sun_cascades[i].size=sizes[i];
        m_sun_cascades[i].reset_chain=i==0 || (i==2 && resetLast);
    }
    for (unsigned cascade_ind=0;cascade_ind<3;++cascade_ind)
    {
        Stage& cascade=m_sun_cascades[cascade_ind];
        xr_vector<Fplane> cull_planes;
        Fvector3 cull_COP;
        CFrustum cull_frustum;
        Fmatrix cull_xform;
    {
		// calculate view-frustum bounds in world space
		Fmatrix ex_project, ex_full, ex_full_inverse;
		{
			ex_project = projection;
			ex_full.mul(ex_project, view);
			D3DXMatrixInverse((D3DXMATRIX*)&ex_full_inverse, 0, (D3DXMATRIX*)&ex_full);
		}

		// Compute volume(s) - something like a frustum for infinite directional light
		// Also compute virtual light position and sector it is inside
		{
			
			// Lets begin from base frustum
			Fmatrix fullxform_inv = ex_full_inverse;
			//******************************* Need to be placed after cuboid built **************************

			// COP - 100 km away
			cull_COP.mad(eye, sunDirection, -1200.f);			

			// Create approximate ortho-xform
			// view: auto find 'up' and 'right' vectors
			Fmatrix mdir_View, mdir_Project;
			Fvector L_dir, L_up, L_right, L_pos;
			L_pos.set(sunPosition);
			L_dir.set(sunDirection).normalize();
			L_right.set(1, 0, 0);
			if (_abs(L_right.dotproduct(L_dir)) > .99f) L_right.set(0, 0, 1);
			L_up.crossproduct(L_dir, L_right).normalize();
			L_right.crossproduct(L_up, L_dir).normalize();
			mdir_View.build_camera_dir(L_pos, L_dir, L_up);


			//////////////////////////////////////////////////////////////////////////
	#ifdef	_DEBUG
			typedef FixedConvexVolume<true> t_cuboid;
	#else
			typedef FixedConvexVolume<false> t_cuboid;
	#endif

			t_cuboid light_cuboid;
			light_cuboid.view_frustum_rays.clear();
			{
				// Initialize the first cascade rays, then each cascade will initialize rays for next one.
				if (cascade_ind == 0 || cascade.reset_chain)
				{
					Fvector3 near_p, edge_vec;
					for (int p = 0; p < 4; p++)
					{
						// 					Fvector asd = forward;
						// 					asd.mul(-2);
						// 					asd.add(eye);
						// 					near_p		= eye;//wform		(fullxform_inv,asd); //
						near_p = metalSunWform(fullxform_inv, corners[facetable[4][p]]);

						edge_vec = metalSunWform(fullxform_inv, corners[facetable[5][p]]);
						edge_vec.sub(near_p);
						edge_vec.normalize();

						light_cuboid.view_frustum_rays.push_back(sun::ray(near_p, edge_vec));
					}
				}
				else
					light_cuboid.view_frustum_rays = cascade.rays;

				// Preserve the exact input handoff for CPU validation. The fixed
				// caster-volume pass below mutates ray origins to the light-cuboid
				// intersections, which can hide a reset-vs-chain difference.
				for (unsigned ray=0;ray<4;++ray)
					output.cascades[cascade_ind].initialRayOrigins[ray]={
						light_cuboid.view_frustum_rays[ray].P.x,
						light_cuboid.view_frustum_rays[ray].P.y,
						light_cuboid.view_frustum_rays[ray].P.z};

				light_cuboid.view_ray.P = eye;
				light_cuboid.view_ray.D = forward;
				light_cuboid.light_ray.P = L_pos;
				light_cuboid.light_ray.D = L_dir;
			}

			// THIS NEED TO BE A CONSTATNT
			Fplane light_top_plane;
			light_top_plane.build_unit_normal(L_pos, L_dir);
			float dist = light_top_plane.classify(eye);

			float map_size = cascade.size;
			D3DXMatrixOrthoOffCenterLH((D3DXMATRIX*)&mdir_Project, -map_size * 0.5f, map_size * 0.5f, -map_size * 0.5f,
																map_size * 0.5f, 0.1, dist + map_size);

			// build viewport xform
			float view_dim = float(resolution);
			Fmatrix m_viewport = {
				view_dim / 2.f, 0.0f, 0.0f, 0.0f,
				0.0f, -view_dim / 2.f, 0.0f, 0.0f,
				0.0f, 0.0f, 1.0f, 0.0f,
				view_dim / 2.f, view_dim / 2.f, 0.0f, 1.0f
			};
			Fmatrix m_viewport_inv;
			D3DXMatrixInverse((D3DXMATRIX*)&m_viewport_inv, 0, (D3DXMATRIX*)&m_viewport);

			// snap view-position to pixel
			cull_xform.mul(mdir_Project, mdir_View);
			Fmatrix cull_xform_inv;
			cull_xform_inv.invert(cull_xform);


			//		light_cuboid.light_cuboid_points.reserve		(9);
			for (int p = 0; p < 8; p++)
				light_cuboid.light_cuboid_points[p] = metalSunWform(cull_xform_inv, corners[p]);

			// only side planes
			for (int plane = 0; plane < 4; plane++)
			{
				for (int pt = 0; pt < 4; pt++)
					light_cuboid.light_cuboid_polys[plane].points[pt] = facetable[plane][pt];
			}


			Fvector lightXZshift;
			light_cuboid.compute_caster_model_fixed(cull_planes, lightXZshift, cascade.size, cascade.reset_chain);
			Fvector proj_view = forward;
			proj_view.y = 0;
			proj_view.normalize();
			//			lightXZshift.mad(proj_view, 20);

			// Initialize rays for the next cascade
			if (cascade_ind < m_sun_cascades.size() - 1)
				m_sun_cascades[cascade_ind + 1].rays = light_cuboid.view_frustum_rays;



			Fvector cam_shifted = L_pos;
			cam_shifted.add(lightXZshift);

			// rebuild the view transform with the shift.
			mdir_View.identity();
			mdir_View.build_camera_dir(cam_shifted, L_dir, L_up);
			cull_xform.identity();
			cull_xform.mul(mdir_Project, mdir_View);
			cull_xform_inv.invert(cull_xform);


			// Create frustum for query
			cull_frustum._clear();
			for (u32 p = 0; p < cull_planes.size(); p++)
				cull_frustum._add(cull_planes[p]);

			{
				Fvector cam_proj = eye;
				const float align_aim_step_coef = 4.f;
				cam_proj.set(floorf(cam_proj.x / align_aim_step_coef) + align_aim_step_coef / 2,
										floorf(cam_proj.y / align_aim_step_coef) + align_aim_step_coef / 2,
										floorf(cam_proj.z / align_aim_step_coef) + align_aim_step_coef / 2);
				cam_proj.mul(align_aim_step_coef);
				Fvector cam_pixel = metalSunWform(cull_xform, cam_proj);
				cam_pixel = metalSunWform(m_viewport, cam_pixel);
				Fvector shift_proj = lightXZshift;
				cull_xform.transform_dir(shift_proj);
				m_viewport.transform_dir(shift_proj);

				const float align_granularity = 4.f;
				shift_proj.x = shift_proj.x > 0 ? align_granularity : -align_granularity;
				shift_proj.y = shift_proj.y > 0 ? align_granularity : -align_granularity;
				shift_proj.z = 0;

				cam_pixel.x = cam_pixel.x / align_granularity - floorf(cam_pixel.x / align_granularity);
				cam_pixel.y = cam_pixel.y / align_granularity - floorf(cam_pixel.y / align_granularity);
				cam_pixel.x *= align_granularity;
				cam_pixel.y *= align_granularity;
				cam_pixel.z = 0;

				cam_pixel.sub(shift_proj);

				m_viewport_inv.transform_dir(cam_pixel);
				cull_xform_inv.transform_dir(cam_pixel);
				Fvector diff = cam_pixel;
				static float sign_test = -1.f;
				diff.mul(sign_test);
				Fmatrix adjust;
				adjust.translate(diff);
				cull_xform.mulB_44(adjust);
			}

			cascade.xform = cull_xform;
            auto& result=output.cascades[cascade_ind];
            result.size=cascade.size; result.bias=-0.0000025f*cascade.size;
            result.resetChain=cascade.reset_chain;
            std::memcpy(result.lightViewProjection.data(),&cull_xform,sizeof(Fmatrix));
            for (const Fplane& plane:cull_planes)
                result.casterPlanes.push_back({plane.n.x,plane.n.y,plane.n.z,plane.d});
            for (unsigned ray=0;ray<4;++ray)
                result.nextRayOrigins[ray]={light_cuboid.view_frustum_rays[ray].P.x,
                    light_cuboid.view_frustum_rays[ray].P.y,light_cuboid.view_frustum_rays[ray].P.z};

		}
	}
    }
    for (unsigned i=0;i<3;++i)
    {
        Fmatrix light, inverse;
        std::memcpy(&light,output.cascades[i==2?1:i].lightViewProjection.data(),sizeof(light));
        inverse.invert(light);
        for (unsigned p=0;p<8;++p)
        {
            Fvector3 source=corners[p]; if (source.z==0) source.z=0.7f;
            Fvector3 world=metalSunWform(inverse,source);
            output.cascades[i].receiverVolume[p]={world.x,world.y,world.z,1};
        }
        for (float value:output.cascades[i].lightViewProjection) if (!std::isfinite(value)) return output;
    }
    output.valid=true;
    return output;
}
