#if !defined(_DSGMANAGE_H_)
#define _DSGMANAGE_H_
#pragma once
#include "r__dsgraph_types.h"
#include "r__sector.h"
#include "../../xrCore/FixedMap.h"

#include <memory>
#include <array>
#include <vector>

struct MetalCpuVisualAsset;
class CDSGraphManager : public IDSGraphManager
{
public:
	// A CPU-owned description of a live engine world instance.  It intentionally
	// contains no D3D resource: the Direct Metal backend receives an immutable
	// copy at frame end and may resolve the asset through its own loader.
	struct MetalWorldRenderable
	{
		shared_str assetName;
		Fmatrix transform;
		Fsphere bounds;
		u32 visualType = 0;
		u16 objectId = u16(-1);
		// Legacy R4 publishes L_hotness only when the active base texture is
		// explicitly marked hot.  Preserve that resolved value in the CPU-only
		// capture so Direct Metal never needs to borrow a CTexture or D3D state.
		Fvector4 hotness;
		// Value-only copy of the object's live CROS lighting. Converted model
		// shaders consume these through the same dynamic_transforms offsets as R4.
		Fvector4 material;
		Fvector4 hemiCubePositive;
		Fvector4 hemiCubeNegative;
		std::shared_ptr<const MetalCpuVisualAsset> asset;
		std::shared_ptr<const std::vector<std::array<float, 16>>> boneTransforms;
	};
	// HUD is captured independently while CHudInitializer has installed the
	// near projection.  It must never be appended to metalWorldRenderables.
	struct MetalHudRenderable
	{
		enum class ScopeRole : u8
		{
			Back,
			Zwrite,
			ReticlePrecise,
		};
		// The dynamic shader selected at the leaf is data, not a D3D resource.
		// Scope leaves need it later to reproduce their distinct R4 route.
		struct SelectedShaderFlags
		{
			u8 priority = 0;
			bool strictB2F = false;
			bool emissive = false;
			bool distort = false;
			bool wmark = false;
			u8 scopeLense = 0;
		};
		struct ScopeLeaf
		{
			u32 geometryIndex = 0;
			ScopeRole role = ScopeRole::Back;
			SelectedShaderFlags selectedShader;
		};
		shared_str assetName;
		Fmatrix transform;
		// The previous leaf transform is a value-only history sample. It starts
		// as `transform` and is replaced by the Metal submission bridge only when
		// the preceding HUD snapshot has the identical ordinal/count layout.
		Fmatrix previousTransform;
		// `transform` is the selected leaf's m_W.  These values are copied while
		// the HUD near transform is installed, rather than reconstructed in the
		// Metal encoder from stale world state.
		Fvector4 material;
		Fvector4 hotness;
		Fvector4 hemiCubePositive;
		Fvector4 hemiCubeNegative;
		std::shared_ptr<const MetalCpuVisualAsset> asset;
		std::shared_ptr<const std::vector<std::array<float, 16>>> boneTransforms;
		// This palette has the same bone ordinal and count as boneTransforms. It
		// is copied with the current palette under UCalc_Mutex; a non-finite
		// engine history entry falls back to its current transform.
		std::shared_ptr<const std::vector<std::array<float, 16>>> previousBoneTransforms;
		// Explicit CPU-asset geometry subset selected by renderer-owned Metal
		// admission. An empty list intentionally means no drawable geometry.
		xr_vector<u32> eligibleGeometryIndices;
		// Scope materials never fall through eligibleGeometryIndices. Each value
		// identifies one immutable OGF leaf and its role in the 3DSS triplet.
		xr_vector<ScopeLeaf> scopeLeaves;
	};
	struct MetalHudCaptureStats
	{
		u32 roots = 0;
		u32 hierarchyNodes = 0;
		u32 skeletonNodes = 0;
		u32 childEdges = 0;
		u32 traversed = 0;
		u32 ssaRejected = 0;
		u32 homRejected = 0;
		u32 shaderRejected = 0;
		u32 priorityRejected = 0;
		u32 requested = 0;
		u32 admitted = 0;
		u32 aggregateFallback = 0;
		u32 strict = 0;
		u32 scopeForward = 0;
		u32 scopeBack = 0;
		u32 scopeZwrite = 0;
		u32 scopeReticlePrecise = 0;
		u32 scopeTriplets = 0;
		u32 scopePartialRejected = 0;
		u32 scopeDuplicateRejected = 0;
		u32 nonHq = 0;
		u32 distortFollowup = 0;
		u32 emissiveFollowup = 0;
		u32 wmarkFollowup = 0;
	};
	// Value-only equivalents of the precise-scope b0 inputs. They are sampled
	// at HUD capture while the legacy binders would observe the same frame.
	struct MetalHudScopeGlobals
	{
		Fvector4 hudParams;
		Fvector4 hudFovParams;
		Fvector4 ogseCScreen;
		Fvector4 s3dsParam1;
		Fvector4 s3dsParam2;
		Fvector4 s3dsParam3;
		Fvector4 s3dsParam4;
		Fvector4 markswitchCurrent;
		Fvector4 markswitchColor;
		Fvector4 shaderParam8;
	};
	// Immutable, CPU-only local-light admission record.  It is built during
	// Calculate using the same frustum/sector/fog/LOD gates as R4, before the
	// Metal frame is encoded.  Keeping it value-only prevents D3D visibility
	// queries, light resources and live pointers from leaking to the backend.
	struct MetalLocalLight
	{
		enum class Type : u8 { Point, Spot, OmniFace };
		u64 identity = 0;
		Type type = Type::Point;
		Fvector position;
		Fvector direction;
		Fvector right;
		Fcolor color;
		float range = 0.f;
		float cone = 0.f;
		float virtualSize = 0.1f;
		bool active = false;
		bool staticLight = false;
		bool shadowed = false;
		bool hudMode = false;
		u8 omniFace = 0xff;
		shared_str projectorTexture;
	};
	struct MetalLocalLightCaptureStats
	{
		u32 spatial = 0;
		u32 captured = 0;
		u32 admitted = 0;
		u32 rejectedFrustum = 0;
		u32 rejectedInactive = 0;
		u32 rejectedSector = 0;
		u32 sectorFallback = 0;
		u32 rejectedHom = 0;
		u32 rejectedFog = 0;
		u32 rejectedLod = 0;
	};

	enum
	{
		VQ_HOM = (1 << 0),
		VQ_SSA = (1 << 1),
		VQ_FADE = (1 << 2),				// requires SSA to work
	};

	R_dsgraph::DynamicSceneRgraph RGraph;
	u32										i_options;		// input:	culling options
	u32										i_doptions;
	Fvector									i_vBase;		// input:	"view" point
	CFrustum								i_frustum;		// input:	"view" frustum
	Fmatrix									i_mXFORM;		// input:	4x4 xform
	CSector* i_start;		// input:	starting point
	xrCriticalSection						P_CS;
	xrSRWLock								S_LC;

	FixedMAP<CSector*, std::pair<xr_vector<CFrustum>, FixedSet<CPortal*>>>		m_sector_frustums;
	xr_unordered_flat_set<dxRender_Visual*>				m_static_seen;
	xr_vector<ISpatialShared>				lstRenderables, lstLights;
	xr_vector<MetalWorldRenderable>				metalWorldRenderables;
	// Filled only while r_dsgraph_capture_metal_world asks each live object to
	// submit its actual render visual. This preserves special replacements such
	// as player_legs_controller and their explicit transforms.
	xr_vector<MetalWorldRenderable>				metalWorldCaptureStaging;
	bool									metalWorldCaptureActive = false;
	IKinematics*							metalWorldCaptureKinematics = nullptr;
	u32									metalWorldFrame = u32(-1);
	xr_vector<MetalHudRenderable>					metalHudRenderables;
	// Only the HUD capture call writes this staging vector.  Published records
	// are replaced once under S_LC, so End() never copies a half-built vector.
	xr_vector<MetalHudRenderable>					metalHudCaptureStaging;
	xr_vector<MetalLocalLight>					metalLocalLights;
	xr_vector<MetalLocalLight>					metalLocalLightCaptureStaging;
	MetalLocalLightCaptureStats					metalLocalLightCaptureStats;
	MetalLocalLightCaptureStats					metalLocalLightPublishedStats;
	u32									metalLocalLightFrame = u32(-1);
	MetalHudCaptureStats						metalHudCaptureStats;
	MetalHudCaptureStats						metalHudPublishedStats;
	MetalHudScopeGlobals						metalHudScopeGlobals;
	Fmatrix									metalHudView;
	Fmatrix									metalHudProjection;
	Fmatrix									metalHudPreviousView;
	Fmatrix									metalHudPreviousProjection;
	bool									metalHudCaptureActive = false;
	IKinematics*							metalHudCaptureKinematics = nullptr;
	u32									metalHudFrame = u32(-1);
	FixedMAP<CPortal*, float>				f_portals;
	sPoly S, D;
	ref_shader								f_shader;
	ref_geom								f_geom;

	CDSGraphManager(u32 options, u32 doptions, bool(&& mask)[7]) : i_options(options), i_doptions(doptions)
	{
		std::copy(mask, mask + 7, i_mask);
	}
	void initialize();
	void destroy();
	void traverse(CSector* start, CFrustum& F, Fvector& vBase, Fmatrix& mXFORM);
	IC bool is_sector_visible(CSector* sector)
	{
		if (sector == i_start) return true;

		{
			xrSRWLockGuard guard(&S_LC, true);
			if(m_sector_frustums.size())
			{
				if (auto node = m_sector_frustums.find(sector))
					return !node->val.first.empty();
			}
		}

		return false;
	};
	CDSGraphManager& get_traverser_safed() { xrSRWLockGuard guard(&S_LC, false); return *this; };
	void fade_portal(CPortal* _p, float ssa);
	void fade_render();

	virtual void set_Object(IRenderable* O = nullptr);

	virtual void add_Static(IRenderVisual* pVisual, CFrustum& frustum, u32 planes);
	virtual void add_Static_MultiFrustum(IRenderVisual* pVisual, const xr_vector<CFrustum>& frustums, const xr_vector<u32>& masks);
	void add_leaf_Static(dxRender_Visual* pVisual);
	void add_leafs_Static(xr_vector<dxRender_Visual*>& children);
	void add_leafs_Static(xr_vector<IRenderVisual*>& children);
	void r_dsgraph_insert_static(dxRender_Visual* pVisual);

	virtual void add_Dynamic(IRenderVisual* piVisual, Fmatrix* xform);
	void add_Dynamic(dxRender_Visual* pVisual, Fmatrix* xform);
	void add_leafs_Dynamic(xr_vector<dxRender_Visual*>& children, Fmatrix* xform);
	void add_leafs_Dynamic(xr_vector<IRenderVisual*>& children, Fmatrix* xform);
	void r_dsgraph_insert_dynamic(dxRender_Visual* pVisual, Fmatrix* xform);

	void AddToRenderQueue(R_dsgraph::RenderQueue& queue, const R_dsgraph::DSGraphItem<u32, false>& item, const SPass& pass);
	void r_dsgraph_render_graph(R_dsgraph::RenderQueueArray& queue, u32 _priority, bool _clear = true, bool static_geometry = true);
	IC void r_dsgraph_render_graph(u32 _priority, bool _clear = true)
	{
		r_dsgraph_render_static(_priority, _clear);
		r_dsgraph_render_dynamic(_priority, _clear);
	}
	IC void r_dsgraph_render_static(u32 _priority, bool _clear = true)
	{
		PROF_EVENT("r_dsgraph_render_static");
		r_dsgraph_render_graph(RGraph.mapStaticPasses, _priority, _clear);
	};
	IC void	r_dsgraph_render_dynamic(u32 _priority, bool _clear = true)
	{
		PROF_EVENT("r_dsgraph_render_dynamic");
		r_dsgraph_render_graph(RGraph.mapDynamicPasses, _priority, _clear, false);
	};

	template<typename T, bool Reverse>
	void r_dsgraph_render_graph_sorted(R_dsgraph::mapDSGraphItems<T, Reverse>& graph, bool _clear = true);
	void r_dsgraph_capture_hud();
	void r_dsgraph_render_hud();
	void r_dsgraph_render_hud_ui();
	void r_dsgraph_render_lods(bool _setup_zb, bool _clear);
	void r_dsgraph_render_sorted(bool render_hud = true);
	void r_dsgraph_render_sorted_hud();
	void r_dsgraph_render_emissive(bool clear = true, bool renderHUD = false);
	void r_dsgraph_render_wmarks();
	void r_dsgraph_render_distort();

	// Anomaly
#if defined(USE_DX11)
	void r_dsgraph_render_ScopeSorted();
#endif
	void r_dsgraph_render_cam_ui();
	void r_dsgraph_render_water_ssr();
	void r_dsgraph_render_water();

	void r_dsgraph_capture(bool lights = false, bool dynamic = false, CObject* O = nullptr);
	void r_dsgraph_capture_lights();
	void r_dsgraph_capture_static();
	void r_dsgraph_capture_dynamic(CObject* O = nullptr);
	// Sector data and D3D shaders are deliberately absent in the Metal level
	// lifecycle. This queries visible active CLevel objects from the spatial DB,
	// then captures the exact dynamic visuals/transforms they submit instead of
	// inserting D3D graph nodes.
	void r_dsgraph_capture_metal_world();
	void capture_metal_world_renderable(dxRender_Visual* visual, Fmatrix* transform,
		IKinematics* kinematics);
	void copy_metal_world_renderables(xr_vector<MetalWorldRenderable>& out, u32& frame);
	void r_dsgraph_capture_metal_hud();
	void r_dsgraph_capture_metal_lights();
	void validate_metal_hud_scope_capture();
	void copy_metal_hud_renderables(xr_vector<MetalHudRenderable>& out, Fmatrix& view,
		Fmatrix& projection, Fmatrix& previousView, Fmatrix& previousProjection,
		MetalHudCaptureStats& stats, MetalHudScopeGlobals& scopeGlobals, u32& frame);
	void copy_metal_local_lights(xr_vector<MetalLocalLight>& out, MetalLocalLightCaptureStats& stats,
		u32& frame);

	void clear()
	{
		xrSRWLockGuard guard(&S_LC, false);
		RGraph.clear();
		if (m_sector_frustums.size())
		{
			for (auto& pair : m_sector_frustums)
			{
				pair.val.first.clear();
				pair.val.second.clear();
			}
			m_sector_frustums.clear();
		}
		lstRenderables.clear();
		lstLights.clear();
		metalWorldRenderables.clear();
		metalWorldCaptureStaging.clear();
		metalWorldCaptureActive = false;
		metalWorldCaptureKinematics = nullptr;
		metalWorldFrame = u32(-1);
		metalHudRenderables.clear();
		metalHudCaptureStaging.clear();
		metalHudCaptureStats = {};
		metalHudPublishedStats = {};
		metalHudCaptureKinematics = nullptr;
		metalHudFrame = u32(-1);
		f_portals.clear();
		m_static_seen.clear();
	}
};

struct PortalTraverseDebugStats
{
	u32 frame_id = 0;

	u32 traverse_calls = 0;
	u32 traverse_calls_with_options = 0;
	u32 traverse_calls_without_options = 0;
	u32 frustums_pushed = 0;
	u32 max_frustums_in_sector = 0;
	u32 frustums_pushed_opt = 0;
	u32 frustums_pushed_noopt = 0;
	u32 max_frustums_in_sector_opt = 0;
	u32 max_frustums_in_sector_noopt = 0;

	u32 portals_checked = 0;
	u32 portals_checked_opt = 0;
	u32 portals_checked_noopt = 0;
	u32 portals_skipped_already_visited = 0;
	u32 portals_rejected_sphere = 0;
	u32 portals_rejected_sector = 0;
	u32 portals_rejected_ssa = 0;
	u32 portals_rejected_clip = 0;
	u32 portals_rejected_hom = 0;
	u32 portals_recursed = 0;
	u32 portals_recursed_opt = 0;
	u32 portals_recursed_noopt = 0;

	u32 portals_debug_branch_taken = 0;
	u32 portals_debug_branch_precedence_hits = 0;

	u32 static_sector_nodes = 0;
	u32 static_frustum_nodes = 0;
	u32 static_add_root_calls = 0;
	u32 static_sector_nodes_opt = 0;
	u32 static_sector_nodes_noopt = 0;
	u32 static_frustum_nodes_opt = 0;
	u32 static_frustum_nodes_noopt = 0;
	u32 static_add_root_calls_opt = 0;
	u32 static_add_root_calls_noopt = 0;

	u32 dynamic_spatials = 0;
	u32 dynamic_frustum_tests = 0;
	u32 dynamic_frustum_hits = 0;
	u32 dynamic_rendered = 0;
	u32 dynamic_spatials_opt = 0;
	u32 dynamic_spatials_noopt = 0;
	u32 dynamic_frustum_tests_opt = 0;
	u32 dynamic_frustum_tests_noopt = 0;
	u32 dynamic_frustum_hits_opt = 0;
	u32 dynamic_frustum_hits_noopt = 0;
	u32 dynamic_rendered_opt = 0;
	u32 dynamic_rendered_noopt = 0;

	u32 queue_static_packets = 0;
	u32 queue_dynamic_packets = 0;
	u32 queue_static_packets_opt = 0;
	u32 queue_static_packets_noopt = 0;
	u32 queue_dynamic_packets_opt = 0;
	u32 queue_dynamic_packets_noopt = 0;

	u32 static_dedup_seen = 0;
	u32 static_dedup_skipped = 0;
	u32 static_dedup_seen_opt = 0;
	u32 static_dedup_seen_noopt = 0;
	u32 static_dedup_skipped_opt = 0;
	u32 static_dedup_skipped_noopt = 0;

	void reset(u32 frame)
	{
		*this = {};
		frame_id = frame;
	}
};

bool PortalTraverseDbg_Enabled();
bool PortalTraverseDbg_IsOptions(u32 options);
PortalTraverseDebugStats& PortalTraverseDbg_Get();
const PortalTraverseDebugStats& PortalTraverseDbg_Peek();

#endif // !defined(AFX_PORTAL_H__1FC2D371_4A19_49EA_BD1E_2D0F8DEBBF15__INCLUDED_)
