#include "stdafx.h"
#include "dxEnvironmentRender.h"

#include "dxRenderDeviceRender.h"

#include "xrRender_console.h"

#include "../../xrEngine/environment.h"
#include "../../Layers/xrRender/ResourceManager.h"

#include "../../xrEngine/xr_efflensflare.h"
#include "../../Include/xrRender/R4BackendSelection.h"
#if defined(XR_PLATFORM_APPLE)
#include "../xrRenderMetal/MetalRenderDeviceRender.h"
#include <filesystem>
#include <fstream>
#endif

static Fmatrix mSky_prev = Fidentity;

#if defined(XR_PLATFORM_APPLE)
static bool dumpMetalR4ShaderSources(const std::filesystem::path& outputDirectory,
	std::string& error)
{
	error.clear();
	if (outputDirectory.empty() || !outputDirectory.is_absolute() ||
		std::filesystem::is_symlink(outputDirectory) ||
		outputDirectory.generic_string().find("/runtime/") != std::string::npos)
	{
		error = "shader source dump requires an absolute non-symlink output directory";
		return false;
	}
	std::error_code filesystemError;
	if (std::filesystem::exists(outputDirectory, filesystemError) &&
		std::filesystem::directory_iterator(outputDirectory,
			std::filesystem::directory_options::skip_permission_denied,
			filesystemError) != std::filesystem::directory_iterator{})
	{
		error = "shader source dump output must be new or empty";
		return false;
	}
	std::filesystem::create_directories(outputDirectory, filesystemError);
	if (filesystemError)
	{
		error = "cannot create shader source dump output: " + filesystemError.message();
		return false;
	}
	xr_vector<char*>* files = FS.file_list_open("$game_shaders$", "r3\\", FS_ListFiles);
	if (!files)
	{
		error = "engine shader source view r3 is unavailable";
		return false;
	}
	std::size_t written = 0;
	bool succeeded = true;
	for (const char* entry : *files)
	{
		std::string relative = entry ? entry : "";
		std::replace(relative.begin(), relative.end(), '\\', '/');
		const std::filesystem::path relativePath(relative);
		if (relative.empty() || relativePath.is_absolute() || relative.find("..") != std::string::npos)
		{
			error = "engine shader source view contains an unsafe relative path";
			succeeded = false;
			break;
		}
		string_path virtualName;
		strconcat(sizeof(virtualName), virtualName, "r3\\", entry);
		IReader* reader = FS.r_open("$game_shaders$", virtualName);
		if (!reader)
		{
			error = "cannot read engine shader source " + relative;
			succeeded = false;
			break;
		}
		const std::filesystem::path destination = outputDirectory / relativePath;
		std::filesystem::create_directories(destination.parent_path(), filesystemError);
		std::ofstream stream(destination, std::ios::binary | std::ios::trunc);
		if (filesystemError || !stream ||
			(reader->length() != 0u &&
				!stream.write(static_cast<const char*>(reader->pointer()), reader->length())))
		{
			error = "cannot write shader source dump file " + destination.generic_string();
			succeeded = false;
			FS.r_close(reader);
			break;
		}
		FS.r_close(reader);
		++written;
	}
	FS.file_list_close(files);
	if (!succeeded)
		return false;
	if (written == 0u)
	{
		error = "engine shader source view r3 is empty";
		return false;
	}
	Msg("* METAL_SHADER_SOURCE_DUMP_READY files=%zu output=%s",
		written, outputDirectory.generic_string().c_str());
	return true;
}
#endif

//////////////////////////////////////////////////////////////////////////
// half box def
static Fvector3 hbox_verts[24] =
{
	{-1.f, -1.f, -1.f}, {-1.f, -1.01f, -1.f}, // down
	{1.f, -1.f, -1.f}, {1.f, -1.01f, -1.f}, // down
	{-1.f, -1.f, 1.f}, {-1.f, -1.01f, 1.f}, // down
	{1.f, -1.f, 1.f}, {1.f, -1.01f, 1.f}, // down
	/* AVO: sky box stretch fix
	{-1.f,	 2.f,	-1.f}, {-1.f,	 1.f,	-1.f},
	{ 1.f,	 2.f,	-1.f}, { 1.f,	 1.f,	-1.f},
	{-1.f,	 2.f,	 1.f}, {-1.f,	 1.f,	 1.f},
	{ 1.f,	 2.f,	 1.f}, { 1.f,	 1.f,	 1.f},
	*/
	{-1.f, 1.f, -1.f}, {-1.f, 1.f, -1.f},
	{1.f, 1.f, -1.f}, {1.f, 1.f, -1.f},
	{-1.f, 1.f, 1.f}, {-1.f, 1.f, 1.f},
	{1.f, 1.f, 1.f}, {1.f, 1.f, 1.f},
	/* AVO: end */
	{-1.f, 0.f, -1.f}, {-1.f, -1.f, -1.f}, // half
	{1.f, 0.f, -1.f}, {1.f, -1.f, -1.f}, // half
	{1.f, 0.f, 1.f}, {1.f, -1.f, 1.f}, // half
	{-1.f, 0.f, 1.f}, {-1.f, -1.f, 1.f} // half
};
static u16 hbox_faces[20 * 3] =
{
	0, 2, 3,
	3, 1, 0,
	4, 5, 7,
	7, 6, 4,
	0, 1, 9,
	9, 8, 0,
	8, 9, 5,
	5, 4, 8,
	1, 3, 10,
	10, 9, 1,
	9, 10, 7,
	7, 5, 9,
	3, 2, 11,
	11, 10, 3,
	10, 11, 6,
	6, 7, 10,
	2, 0, 8,
	8, 11, 2,
	11, 8, 4,
	4, 6, 11
};

#pragma pack(push,1)
struct v_skybox
{
	Fvector3 p;
	u32 color;
	Fvector3 uv [2];

	void set(Fvector3& _p, u32 _c, Fvector3& _tc)
	{
		p = _p;
		color = _c;
		uv[0] = _tc;
		uv[1] = _tc;
	}
};

const u32 v_skybox_fvf = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX2 | D3DFVF_TEXCOORDSIZE3(0) | D3DFVF_TEXCOORDSIZE3(1);

struct v_clouds
{
	Fvector3 p;
	u32 color;
	u32 intensity;

	void set(Fvector3& _p, u32 _c, u32 _i)
	{
		p = _p;
		color = _c;
		intensity = _i;
	}
};

const u32 v_clouds_fvf = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_SPECULAR;
#pragma pack(pop)

void dxEnvDescriptorRender::Copy(IEnvDescriptorRender& _in)
{
	*this = *(dxEnvDescriptorRender*)&_in;
}

void dxEnvDescriptorMixerRender::Copy(IEnvDescriptorMixerRender& _in)
{
	dxEnvDescriptorMixerRender* other = (dxEnvDescriptorMixerRender*)&_in;

	clouds_r_textures.assign(other->clouds_r_textures.begin(), other->clouds_r_textures.end());
	clouds_r_textures.dwReference++;

	sky_r_textures.assign(other->sky_r_textures.begin(), other->sky_r_textures.end());
	sky_r_textures.dwReference++;

	sky_r_textures_env.assign(other->sky_r_textures_env.begin(), other->sky_r_textures_env.end());
	sky_r_textures_env.dwReference++;
}

void dxEnvironmentRender::Copy(IEnvironmentRender& _in)
{
	*this = *(dxEnvironmentRender*)&_in;
}

particles_systems::library_interface const& dxEnvironmentRender::particles_systems_library()
{
	return (RImplementation.PSLibrary);
}

void dxEnvDescriptorMixerRender::Destroy()
{
	sky_r_textures.clear();
	sky_r_textures_env.clear();
	clouds_r_textures.clear();
}

void dxEnvDescriptorMixerRender::Clear()
{
	std::pair<u32, ref_texture> zero = mk_pair(u32(0), ref_texture(0));
	sky_r_textures.clear();
	sky_r_textures.push_back(zero);
	sky_r_textures.push_back(zero);
	sky_r_textures.push_back(zero);

	sky_r_textures_env.clear();
	sky_r_textures_env.push_back(zero);
	sky_r_textures_env.push_back(zero);
	sky_r_textures_env.push_back(zero);

	clouds_r_textures.clear();
	clouds_r_textures.push_back(zero);
	clouds_r_textures.push_back(zero);
	clouds_r_textures.push_back(zero);
}

void dxEnvDescriptorMixerRender::lerp(IEnvDescriptorRender* inA, IEnvDescriptorRender* inB)
{
	dxEnvDescriptorRender* pA = (dxEnvDescriptorRender *)inA;
	dxEnvDescriptorRender* pB = (dxEnvDescriptorRender *)inB;

	sky_r_textures.clear();
	sky_r_textures.push_back(mk_pair(0, pA->sky_texture));
	sky_r_textures.push_back(mk_pair(1, pB->sky_texture));

	sky_r_textures_env.clear();

	sky_r_textures_env.push_back(mk_pair(0, pA->sky_texture_env));
	sky_r_textures_env.push_back(mk_pair(1, pB->sky_texture_env));

	clouds_r_textures.clear();
	clouds_r_textures.push_back(mk_pair(0, pA->clouds_texture));
	clouds_r_textures.push_back(mk_pair(1, pB->clouds_texture));
}

void dxEnvDescriptorRender::OnDeviceCreate(CEnvDescriptor& owner)
{
	if (SelectedR4Backend() == R4BackendKind::Metal)
		return;
	dxEnvDescriptorMixerRender& mixRen = *(dxEnvDescriptorMixerRender*)&*g_pGamePersistent
	                                                                     ->Environment().CurrentEnv->m_pDescriptorMixer;

	if (owner.sky_texture_name.size())
	{
		sky_texture.create(owner.sky_texture_name.c_str());
		if (g_pGamePersistent->Environment().m_paused)
		{
			mixRen.sky_r_textures.clear();
			mixRen.sky_r_textures.push_back(mk_pair(0, sky_texture));
			mixRen.sky_r_textures.push_back(mk_pair(1, sky_texture));
		}
	}

	if (owner.sky_texture_env_name.size())
	{
		sky_texture_env.create(owner.sky_texture_env_name.c_str());
		if (g_pGamePersistent->Environment().m_paused)
		{
			mixRen.sky_r_textures_env.clear();
			mixRen.sky_r_textures_env.push_back(mk_pair(0, sky_texture_env));
			mixRen.sky_r_textures_env.push_back(mk_pair(1, sky_texture_env));
		}
	}

	if (owner.clouds_texture_name.size())
	{
		clouds_texture.create(owner.clouds_texture_name.c_str());
		if (g_pGamePersistent->Environment().m_paused)
		{
			mixRen.clouds_r_textures.clear();
			mixRen.clouds_r_textures.push_back(mk_pair(0, clouds_texture));
			mixRen.clouds_r_textures.push_back(mk_pair(1, clouds_texture));
		}
	}
}

void dxEnvDescriptorRender::OnDeviceDestroy()
{
	sky_texture.destroy();
	sky_texture_env.destroy();
	clouds_texture.destroy();
}

dxEnvironmentRender::dxEnvironmentRender()
{
	if (SelectedR4Backend() == R4BackendKind::Metal)
		return;
	tsky0 = DEV->_CreateTexture("$user$sky0");
	tsky1 = DEV->_CreateTexture("$user$sky1");
}

void dxEnvironmentRender::OnFrame(CEnvironment& env)
{
	PROF_EVENT("EnvironmentRender::OnFrame");
	if (SelectedR4Backend() == R4BackendKind::Metal)
	{
#if defined(XR_PLATFORM_APPLE)
		static bool shaderDumpAttempted = false;
		if (!shaderDumpAttempted)
		{
			shaderDumpAttempted = true;
			if (const char* dumpDirectory = std::getenv("XRAY_METAL_SHADER_SOURCE_DUMP");
				dumpDirectory && *dumpDirectory)
			{
				std::string dumpError;
				if (!dumpMetalR4ShaderSources(dumpDirectory, dumpError))
					Msg("! [Metal] shader source dump failed: %s", dumpError.c_str());
			}
		}
		R4EnvironmentSnapshot snapshot;
		snapshot.engineFrame = Device.dwFrame;
		const float tonemapAmount = ps_r2_ls_flags.test(R2FLAG_TONEMAP) ?
			ps_r2_tonemap_amount : 0.0f;
		snapshot.tonemapParameters = {
			1.0f + (ps_r2_tonemap_middlegray - 1.0f) * tonemapAmount,
			tonemapAmount,
			1.0f + (ps_r2_tonemap_low_lum - 1.0f) * tonemapAmount,
			ps_r2_tonemap_adaptation,
		};
		snapshot.imageCorrections = {
			ps_r2_img_exposure, ps_r2_img_gamma, ps_r2_img_saturation, 1.0f,
		};
		snapshot.colorGrading = {ps_r2_img_cg.x, ps_r2_img_cg.y, ps_r2_img_cg.z, 1.0f};
		snapshot.frameDeltaSeconds = Device.fTimeDelta;
		if (env.CurrentEnv)
		{
			const CEnvDescriptorMixer& current = *env.CurrentEnv;
			snapshot.skyColor = {current.sky_color.x, current.sky_color.y, current.sky_color.z};
			snapshot.skyBlend = current.weight;
			snapshot.skyRotation = current.sky_rotation;
			snapshot.ambient = {current.ambient.x, current.ambient.y, current.ambient.z};
			snapshot.hemiColor = {current.hemi_color.x, current.hemi_color.y,
				current.hemi_color.z, current.hemi_color.w};
			snapshot.sunColor = {current.sun_color.x, current.sun_color.y, current.sun_color.z};
			snapshot.sunDirection = {current.sun_dir.x, current.sun_dir.y, current.sun_dir.z};
			snapshot.fogColor = {current.fog_color.x, current.fog_color.y, current.fog_color.z};
			snapshot.fogDensity = current.fog_density;
			snapshot.fogNear = current.fog_near;
			snapshot.fogFar = current.fog_far;
			const float ambientScale = ps_r2_sun_lumscale_amb +
				g_pGamePersistent->nv_shader_data.lum_factor;
			snapshot.combineAmbient = {
				_max(current.ambient.x * 2.0f, 0.001f) * ambientScale,
				_max(current.ambient.y * 2.0f, 0.001f) * ambientScale,
				_max(current.ambient.z * 2.0f, 0.001f) * ambientScale,
				0.0f,
			};
			const float hemiScale = 2.0f * ps_r2_sun_lumscale_hemi;
			snapshot.combineEnvironment = {
				(current.hemi_color.x * 2.0f + EPS) * hemiScale,
				(current.hemi_color.y * 2.0f + EPS) * hemiScale,
				(current.hemi_color.z * 2.0f + EPS) * hemiScale,
				current.weight,
			};
			snapshot.hmodelStuff = {current.m_fHemiVibrance, current.m_fHemiContrast,
			current.m_fWetSurfaces, 0.0f};
			snapshot.globalTime = Device.fTimeGlobal;
			snapshot.ssfxWater = {ps_ssfx_water.x, ps_ssfx_water.y,
				ps_ssfx_water.z, ps_ssfx_water.w};
			snapshot.ssfxWaterSetup1 = {ps_ssfx_water_setup1.x, ps_ssfx_water_setup1.y,
				ps_ssfx_water_setup1.z, ps_ssfx_water_setup1.w};
			snapshot.ssfxWaterSetup2 = {ps_ssfx_water_setup2.x, ps_ssfx_water_setup2.y,
				ps_ssfx_water_setup2.z, ps_ssfx_water_setup2.w};
			snapshot.ssfxWaterQuality = {ps_ssfx_water_quality.x, ps_ssfx_water_quality.y,
				ps_ssfx_water_quality.z};
			snapshot.ssfxAo = {ps_ssfx_ao.x, ps_ssfx_ao.y, ps_ssfx_ao.z, ps_ssfx_ao.w};
			snapshot.ssfxAoSetup1 = {ps_ssfx_ao_setup1.x, ps_ssfx_ao_setup1.y,
				ps_ssfx_ao_setup1.z, ps_ssfx_ao_setup1.w};
			snapshot.ssfxIl = {ps_ssfx_il.x, ps_ssfx_il.y, ps_ssfx_il.z, ps_ssfx_il.w};
			snapshot.ssfxIlSetup1 = {ps_ssfx_il_setup1.x, ps_ssfx_il_setup1.y,
				ps_ssfx_il_setup1.z, ps_ssfx_il_setup1.w};
			snapshot.windAnimation = env.wind_anim.w;
			snapshot.windVelocity = current.wind_velocity;
			snapshot.rainDensity = current.rain_density;
			snapshot.wetnessFactor = env.wetness_factor;
			snapshot.treeScaleBiasCorrection = ps_r__Tree_SBC;
			if (light* adaptedSun = static_cast<light*>(RImplementation.Lights.sun_adapted._get()))
			{
				snapshot.adaptedSunColor = {adaptedSun->color.r, adaptedSun->color.g,
					adaptedSun->color.b};
				snapshot.adaptedSunDirection = {adaptedSun->direction.x, adaptedSun->direction.y,
					adaptedSun->direction.z};
			}
			else
			{
				snapshot.adaptedSunColor = snapshot.sunColor;
				snapshot.adaptedSunDirection = snapshot.sunDirection;
			}
			if (env.Current[0] && env.Current[1] && env.Current[0]->sky_texture_name.size() &&
				env.Current[1]->sky_texture_name.size())
			{
				snapshot.sky0TextureName = env.Current[0]->sky_texture_name.c_str();
				snapshot.sky1TextureName = env.Current[1]->sky_texture_name.c_str();
				snapshot.environment0TextureName = env.Current[0]->sky_texture_env_name.c_str();
				snapshot.environment1TextureName = env.Current[1]->sky_texture_env_name.c_str();
			}
			if (env.Current[0] && env.Current[1] &&
				env.Current[0]->clouds_texture_name.size() &&
				env.Current[1]->clouds_texture_name.size() &&
				!env.CloudsVerts.empty() && !env.CloudsIndices.empty())
			{
				snapshot.clouds0TextureName = env.Current[0]->clouds_texture_name.c_str();
				snapshot.clouds1TextureName = env.Current[1]->clouds_texture_name.c_str();
				snapshot.cloudsColor = {current.clouds_color.x, current.clouds_color.y,
					current.clouds_color.z, current.clouds_color.w};
				snapshot.cloudsVertices.reserve(env.CloudsVerts.size());
				for (const Fvector& vertex : env.CloudsVerts)
					snapshot.cloudsVertices.push_back({vertex.x, vertex.y, vertex.z});
				snapshot.cloudsIndices.assign(env.CloudsIndices.begin(), env.CloudsIndices.end());
				snapshot.cloudsValid = true;
			}
			static std::string lastCloudSignature;
			const std::string cloudSignature = snapshot.clouds0TextureName + "|" +
				snapshot.clouds1TextureName + "|" + std::to_string(snapshot.cloudsVertices.size()) +
				"|" + std::to_string(snapshot.cloudsIndices.size()) + "|" +
				std::to_string(snapshot.cloudsColor[3]);
			if (cloudSignature != lastCloudSignature)
			{
				Msg("* METAL_CLOUD_CAPTURE frame=%u valid=%s textures=%s,%s vertices=%zu indices=%zu alpha=%.3f",
					snapshot.engineFrame, snapshot.cloudsValid ? "true" : "false",
					snapshot.clouds0TextureName.c_str(), snapshot.clouds1TextureName.c_str(),
					snapshot.cloudsVertices.size(), snapshot.cloudsIndices.size(), snapshot.cloudsColor[3]);
				lastCloudSignature = cloudSignature;
			}
			snapshot.valid = true;
		}
		static_cast<MetalRenderDeviceRender&>(dxRenderDeviceRender::Instance())
			.SubmitEnvironmentSnapshot(snapshot);
#endif
		return;
	}
	dxEnvDescriptorMixerRender& mixRen = *(dxEnvDescriptorMixerRender*)&*env.CurrentEnv->m_pDescriptorMixer;

	if (::Render->get_generation() == IRender_interface::GENERATION_R2)
	{
		//. very very ugly hack
		if (HW.Caps.raster_major >= 3 && HW.Caps.geometry.bVTF)
		{
			// tonemapping in VS
			mixRen.sky_r_textures.push_back(mk_pair(u32(D3DVERTEXTEXTURESAMPLER0), tonemap)); //. hack
			mixRen.sky_r_textures_env.push_back(mk_pair(u32(D3DVERTEXTEXTURESAMPLER0), tonemap)); //. hack
			mixRen.clouds_r_textures.push_back(mk_pair(u32(D3DVERTEXTEXTURESAMPLER0), tonemap)); //. hack
		}
		else
		{
			// tonemapping in PS
			mixRen.sky_r_textures.push_back(mk_pair(2, tonemap)); //. hack
			mixRen.sky_r_textures_env.push_back(mk_pair(2, tonemap)); //. hack
			mixRen.clouds_r_textures.push_back(mk_pair(2, tonemap)); //. hack
		}
	}

	//. Setup skybox textures, somewhat ugly
	ID3DBaseTexture* e0 = mixRen.sky_r_textures[0].second->surface_get();
	ID3DBaseTexture* e1 = mixRen.sky_r_textures[1].second->surface_get();

	tsky0->surface_set(e0);
	_RELEASE(e0);
	tsky1->surface_set(e1);
	_RELEASE(e1);

	// ******************** Environment params (setting)
#if defined(USE_DX10) || defined(USE_DX11)
	//	TODO: DX10: Implement environment parameters setting for DX10 (if necessary)
#else	//	USE_DX10

#if		RENDER==R_R1
	Fvector3 fog_color = env.CurrentEnv->fog_color;
	fog_color.mul(ps_r1_fog_luminance);
#else	//	RENDER==R_R1
	Fvector3& fog_color = env.CurrentEnv->fog_color;
#endif	//	RENDER==R_R1

	CHK_DX(HW.pDevice->SetRenderState( D3DRS_FOGCOLOR, color_rgba_f(fog_color.x,fog_color.y,fog_color.z,0) ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_FOGSTART, *(u32 *)(&env.CurrentEnv->fog_near) ));
	CHK_DX(HW.pDevice->SetRenderState( D3DRS_FOGEND, *(u32 *)(&env.CurrentEnv->fog_far) ));
#endif	//	USE_DX10
}

void dxEnvironmentRender::OnLoad()
{
	if (SelectedR4Backend() == R4BackendKind::Metal)
		return;
	tonemap = DEV->_CreateTexture("$user$tonemap"); //. hack
}

void dxEnvironmentRender::OnUnload()
{
	tonemap = 0;
}

void dxEnvironmentRender::RenderSky(CEnvironment& env, bool OnlyMV)
{
	PROF_EVENT("EnvironmentRender::RenderSky");
	if (SelectedR4Backend() == R4BackendKind::Metal)
		return;
	// clouds_sh.create		("clouds","null");
	//. this is the bug-fix for the case when the sky is broken
	//. for some unknown reason the geoms happen to be invalid sometimes
	//. if vTune show this in profile, please add simple cache (move-to-forward last found) 
	//. to the following functions:
	//.		CResourceManager::_CreateDecl
	//.		CResourceManager::CreateGeom
	if (env.bNeed_re_create_env)
	{
		sh_2sky.create(&m_b_skybox, "skybox_2t");
		sh_2geom.create(v_skybox_fvf, RCache.Vertex.Buffer(), RCache.Index.Buffer());
		clouds_sh.create("clouds", "null");
		clouds_geom.create(v_clouds_fvf, RCache.Vertex.Buffer(), RCache.Index.Buffer());
		env.bNeed_re_create_env = FALSE;
	}
	::Render->rmFar();

	dxEnvDescriptorMixerRender& mixRen = *(dxEnvDescriptorMixerRender*)&*env.CurrentEnv->m_pDescriptorMixer;

	// draw sky box
	Fmatrix mSky;
	mSky.rotateY(env.CurrentEnv->sky_rotation);
	mSky.translate_over(Device.vCameraPosition);

	u32 i_offset, v_offset;
	u32 C = color_rgba(iFloor(env.CurrentEnv->sky_color.x * 255.f), iFloor(env.CurrentEnv->sky_color.y * 255.f),
	                   iFloor(env.CurrentEnv->sky_color.z * 255.f), iFloor(env.CurrentEnv->weight * 255.f));

	// Fill index buffer
	u16* pib = RCache.Index.Lock(20 * 3, i_offset);
	CopyMemory(pib, hbox_faces, 20*3*2);
	RCache.Index.Unlock(20 * 3);

	// Fill vertex buffer
	v_skybox* pv = (v_skybox*)RCache.Vertex.Lock(12, sh_2geom.stride(), v_offset);
	for (u32 v = 0; v < 12; v++) pv[v].set(hbox_verts[v * 2], C, hbox_verts[v * 2 + 1]);
	RCache.Vertex.Unlock(12, sh_2geom.stride());

	// Apply skybox matrix
	RCache.set_xform_world(mSky);

	if (OnlyMV)
	{
		RCache.set_xform_world_prev(mSky_prev);
		mSky_prev = mSky;

		RCache.set_Geometry(sh_2geom);
		RCache.set_Shader(sh_2sky);
		RCache.set_Element(sh_2sky->E[1]);
		RCache.Render(D3DPT_TRIANGLELIST, v_offset, 0, 12, i_offset, 20);

		RCache.set_xform_world_prev(Fidentity);
	}
	else
	{
		RCache.set_Geometry(sh_2geom);
		RCache.set_Shader(sh_2sky);
		RCache.set_Element(sh_2sky->E[0]);
		RCache.set_Textures(&mixRen.sky_r_textures);
		RCache.Render(D3DPT_TRIANGLELIST, v_offset, 0, 12, i_offset, 20);
	}

	// Sun
	::Render->rmNormal();

	if (!OnlyMV)
	{
#if	RENDER!=R_R1
		//
		// This hack is done to make sure that the state is set for sure:
		// The state may be not set by RCache if the state is changed using API SetRenderState() function before 
		// and the RCache flag will remain unchanged to it's old value. 
		// 
		RCache.set_Z(FALSE);
		RCache.set_Z(TRUE);
		env.eff_LensFlare->Render(TRUE, FALSE, FALSE);
		RCache.set_Z(FALSE);
#else
		env.eff_LensFlare->Render(TRUE, FALSE, FALSE);
#endif
	}
}

void dxEnvironmentRender::RenderClouds(CEnvironment& env)
{
	PROF_EVENT("EnvironmentRender::RenderClouds");
	if (SelectedR4Backend() == R4BackendKind::Metal)
		return;
	::Render->rmFar();

	Fmatrix mXFORM, mScale;
	mScale.scale(10, 0.4f, 10);
	mXFORM.rotateY(env.CurrentEnv->sky_rotation);
	mXFORM.mulB_43(mScale);
	mXFORM.translate_over(Device.vCameraPosition);

	Fvector wd0, wd1;
	Fvector4 wind_dir;
	wd0.setHP(PI_DIV_4, 0);
	wd1.setHP(PI_DIV_4 + PI_DIV_8, 0);
	wind_dir.set(wd0.x, wd0.z, wd1.x, wd1.z).mul(0.5f).add(0.5f).mul(255.f);
	u32 i_offset, v_offset;
	u32 C0 = color_rgba(iFloor(wind_dir.x), iFloor(wind_dir.y), iFloor(wind_dir.w), iFloor(wind_dir.z));
	u32 C1 = color_rgba(iFloor(env.CurrentEnv->clouds_color.x * 255.f), iFloor(env.CurrentEnv->clouds_color.y * 255.f),
	                    iFloor(env.CurrentEnv->clouds_color.z * 255.f), iFloor(env.CurrentEnv->clouds_color.w * 255.f));

	// Fill index buffer
	u16* pib = RCache.Index.Lock(env.CloudsIndices.size(), i_offset);
	CopyMemory(pib, &env.CloudsIndices.front(), env.CloudsIndices.size()*sizeof(u16));
	RCache.Index.Unlock(env.CloudsIndices.size());

	// Fill vertex buffer
	v_clouds* pv = (v_clouds*)RCache.Vertex.Lock(env.CloudsVerts.size(), clouds_geom.stride(), v_offset);
	for (FvectorIt it = env.CloudsVerts.begin(); it != env.CloudsVerts.end(); it++, pv++)
		pv->set(*it, C0, C1);
	RCache.Vertex.Unlock(env.CloudsVerts.size(), clouds_geom.stride());

	// Render
	RCache.set_xform_world(mXFORM);
	RCache.set_Geometry(clouds_geom);
	RCache.set_Shader(clouds_sh);
	dxEnvDescriptorMixerRender& mixRen = *(dxEnvDescriptorMixerRender*)&*env.CurrentEnv->m_pDescriptorMixer;
	RCache.set_Textures(&mixRen.clouds_r_textures);
	RCache.Render(D3DPT_TRIANGLELIST, v_offset, 0, env.CloudsVerts.size(), i_offset, env.CloudsIndices.size() / 3);

	::Render->rmNormal();
}

void dxEnvironmentRender::OnDeviceCreate()
{
	if (SelectedR4Backend() == R4BackendKind::Metal)
		return;
	sh_2sky.create(&m_b_skybox, "skybox_2t");
	sh_2geom.create(v_skybox_fvf, RCache.Vertex.Buffer(), RCache.Index.Buffer());
	clouds_sh.create("clouds", "null");
	clouds_geom.create(v_clouds_fvf, RCache.Vertex.Buffer(), RCache.Index.Buffer());
}

void dxEnvironmentRender::OnDeviceDestroy()
{
	if (SelectedR4Backend() == R4BackendKind::Metal)
		return;
	tsky0->surface_set(NULL);
	tsky1->surface_set(NULL);

	sh_2sky.destroy();
	sh_2geom.destroy();
	clouds_sh.destroy();
	clouds_geom.destroy();
}
