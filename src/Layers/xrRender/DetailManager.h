// DetailManager.h: interface for the CDetailManager class.
//
//////////////////////////////////////////////////////////////////////

#ifndef DetailManagerH
#define DetailManagerH
#pragma once

#include "../../xrCore/xrpool.h"
#include "detailformat.h"
#include "detailmodel.h"
#include "light.h"

#if RENDER == R_R4
namespace detail_gather
{
struct Gatherer;
}
#endif

#ifdef _EDITOR
//.	#include	"ESceneClassList.h"
	const int	dm_max_decompress	= 14;
	class CCustomObject;
	typedef u32	ObjClassID;

    typedef xr_list<CCustomObject*> 		ObjectList;
    typedef ObjectList::iterator 			ObjectIt;
    typedef xr_map<ObjClassID,ObjectList> 	ObjectMap;
    typedef ObjectMap::iterator 			ObjectPairIt;

#else
const int dm_max_decompress = 7;
#endif
//const int		dm_size				= 24;								//!
const int dm_cache1_count = 4; // 
//const int 		dm_cache1_line		= dm_size*2/dm_cache1_count;		//! dm_size*2 must be div dm_cache1_count
const int dm_max_objects = 16383; // v4 14-bit id range (0x3FFF reserved for empty)
const int dm_obj_in_slot = 4;
//const int		dm_cache_line		= dm_size+1+dm_size;
//const int		dm_cache_size		= dm_cache_line*dm_cache_line;
//const float		dm_fade				= float(2*dm_size)-.5f;
const float dm_slot_size = DETAIL_SLOT_SIZE;


//AVO: detail radius
#include "../../build_config_defines.h"
#ifdef DETAIL_RADIUS
const u32 dm_max_cache_size = 62001 * 2; // assuming max dm_size = 124
extern u32 dm_size;
extern u32 dm_cache1_line;
extern u32 dm_cache_line;
extern u32 dm_cache_size;
extern float dm_fade;
extern u32 dm_current_size; //				= iFloor((float)ps_r__detail_radius/4)*2;				//!
extern u32 dm_current_cache1_line;
//		= dm_current_size*2/dm_cache1_count;		//! dm_current_size*2 must be div dm_cache1_count
extern u32 dm_current_cache_line; //		= dm_current_size+1+dm_current_size;
extern u32 dm_current_cache_size; //		= dm_current_cache_line*dm_current_cache_line;
extern float dm_current_fade; //				= float(2*dm_current_size)-.5f;
extern float ps_current_detail_density;
extern float ps_current_detail_height;
#else
const int		dm_size = 24;								//!
const int 		dm_cache1_line = dm_size * 2 / dm_cache1_count;		//! dm_size*2 must be div dm_cache1_count
const int		dm_cache_line = dm_size + 1 + dm_size;
const int		dm_cache_size = dm_cache_line * dm_cache_line;
const float		dm_fade = float(2 * dm_size) - .5f;
#endif


class ECORE_API CDetailManager
{
public:

	float fade_distance = 99999;
	Fvector light_position;

	void details_clear();

	struct SlotItem
	{
		// один кустик
		float scale;
		Fmatrix mRotY;
		Fmatrix mRotY_calculated;
		u32 vis_ID; // индекс в visibility списке он же тип [не качается, качается1, качается2]
		float c_hemi;
		float c_sun;
		float distance;
		Fvector position;
		Fvector normal;
		float alpha;
		float alpha_target;
#if RENDER==R_R1
		Fvector c_rgb;
#endif
	};

	DEFINE_VECTOR(SlotItem*, SlotItemVec, SlotItemVecIt);

#if RENDER == R_R4
    struct SlotRenderItems : SlotItemVec
    {
        enum CacheState { Invalid, Settled, Ready } cache_state = Invalid;
        xr_vector<Fvector4> cached_constants;
        float cached_max_distance = 0.f;
        float slot_distance = 0.f; // squared camera distance of the slot, as of the last visibility update (every item has it)
        // r4_detail_instancing 2: records live in a GPU arena. `revision` changes whenever the list content may differ,
        // `settled_revision` is the revision at which every item was found at rest.
        u32 revision = 0;
        u32 settled_revision = u32(-1);
        u32 arena_revision = u32(-1), arena_epoch = 0, arena_offset = 0, arena_capacity = 0; // records
        bool Unchanged() const
        {
            return (cache_state == Ready && cached_constants.size() == size() * 5) || settled_revision == revision;
        }
    };
#else
    using SlotRenderItems = SlotItemVec;
#endif
	struct SlotPart
	{
		// 
		u32 id; // ID модельки
		SlotItemVec items; // список кустиков
		SlotRenderItems r_items[3]; // список кустиков for render
	};

	enum SlotType
	{
		stReady = 0,
		// Ready to use
		stPending,
		// Pending for decompression

		stFORCEDWORD = 0xffffffff
	};

	struct Slot
	{
		// распакованый слот размером DETAIL_SLOT_SIZE
		struct
		{
			u32 empty :1;
			u32 type :1;
			u32 frame :30;
		};

		int sx, sz; // координаты слота X x Y
		vis_data vis; // 
		SlotPart G[dm_obj_in_slot]; // 
		bool hidden;

		Slot()
		{
			frame = 0;
            hidden = false;
			empty = 1;
			type = stReady;
			sx = sz = 0;
			vis.clear();
		}
	};

	struct CacheSlot1
	{
		u32 empty;
		vis_data vis;
		Slot** slots[dm_cache1_count * dm_cache1_count];

		CacheSlot1()
		{
			empty = 1;
			vis.clear();
		}
	};

	typedef xr_vector<xr_vector<SlotItemVec*>> vis_list;
	typedef xr_vector<CDetail*> DetailVec; // dynamic; 14-bit id range enforced at Load()
	typedef DetailVec::iterator DetailIt;
	typedef poolSS<SlotItem, 4096> PSS;
public:
	int dither [16][16];
public:
	// swing values
	struct SSwingValue
	{
		float rot1;
		float rot2;
		float amp1;
		float amp2;
		float speed;
		void lerp(const SSwingValue& v1, const SSwingValue& v2, float factor);
	};

	SSwingValue swing_desc[2];
	SSwingValue swing_current;
	float m_time_rot_1;
	float m_time_rot_2;
	float m_time_pos;
	float m_global_time_old;
public:
	IReader* dtFS;
	DetailHeader dtH;
	DetailSlot* dtSlots; // note: pointer into VFS
	DetailSlot DS_empty;

public:
	DetailVec objects;
	#if RENDER == R_R4
    u32 m_render_cache_bytes = 0;
    bool BuildRenderCache(SlotRenderItems& items);
    void ReleaseRenderCache(Slot& slot);
#endif
    vis_list m_visibles [3]; // 0=still, 1=Wave1, 2=Wave2

#ifndef _EDITOR
	xrXRC xrc;
#endif
	//AVO: detail draw raius
	//CacheSlot1 					cache_level1[dm_cache1_line][dm_cache1_line];
	//Slot*							cache		[dm_cache_line][dm_cache_line];	// grid-cache itself
	//svector<Slot*,dm_cache_size>	cache_task;									// non-unpacked slots
	//Slot							cache_pool	[dm_cache_size];				// just memory for slots

#ifdef DETAIL_RADIUS
	CacheSlot1** cache_level1;
	Slot*** cache; // grid-cache itself
	svector<Slot*, dm_max_cache_size> cache_task; // non-unpacked slots
	Slot* cache_pool; // just memory for slots
#else
    CacheSlot1 						cache_level1[dm_cache1_line][dm_cache1_line];
    Slot*							cache[dm_cache_line][dm_cache_line];	// grid-cache itself
    svector<Slot*, dm_cache_size>	cache_task;									// non-unpacked slots
    Slot							cache_pool[dm_cache_size];				// just memory for slots*/
#endif

	int cache_cx;
	int cache_cz;

	PSS poolSI; // pool из которого выделяются SlotItem

    Fbox m_visible_bounds;
    bool GetVisibleBounds(Fbox& bounds);
    bool TryGetVisibleBounds(Fbox& bounds);
	void UpdateVisibleM();
	void UpdateVisibleS();
public:
#ifdef _EDITOR
	virtual ObjectList* 			GetSnapList		()=0;
#endif

	IC bool UseVS() { return HW.Caps.geometry_major >= 1; }

	// Software processor
	ref_geom soft_Geom;
	void soft_Load();
	void soft_Unload();
	void soft_Render();

	// Hardware processor
	ref_geom hw_Geom;
	u32 hw_BatchSize;
	ID3DVertexBuffer* hw_VB;
	ID3DIndexBuffer* hw_IB;
	ref_constant hwc_consts;
	ref_constant hwc_wave;
	ref_constant hwc_wind;
	ref_constant hwc_array;
	ref_constant hwc_s_consts;
	ref_constant hwc_s_xform;
	ref_constant hwc_s_array;
	void hw_Load();
	void hw_Load_Geom();
#if RENDER == R_R4
	// Instanced grass (r4_detail_instancing): one mesh copy per model, per-instance stream 1 and instanced twins of the
	// installed detail passes. Legacy batched geometry above stays as the fallback.
	struct InstancedPass
	{
		ref_pass source, twin;
		bool shadow; // E[0] passes are the ones shadow recording uses
	};
	xr_vector<InstancedPass> hw_instanced_passes;
	ref_geom hw_Geom_inst;
	ID3DVertexBuffer* hw_inst_VB = nullptr;
	ID3DIndexBuffer* hw_inst_IB = nullptr;
	xr_vector<u32> hw_inst_vertex_offset, hw_inst_index_offset; // per object, into the single-copy mesh
	ID3DVertexBuffer* hw_instanceVB = nullptr;
	u32 hw_instance_capacity = 0;
	u32 hw_instance_frame = u32(-1); // frame the instance stream was filled for
	xr_vector<u32> hw_inst_base[3], hw_inst_count[3]; // per wave variant and object
	bool hw_instance_gpu = false; // stream is written by the gather shader (r4_detail_instancing 2)
	detail_gather::Gatherer* hw_gatherer = nullptr;
	struct InstanceJob
	{
		SlotRenderItems* list;
		u32 base;
		u32 count; // instances taken from the list (all of them unless the stream is thinned)
		u32 tip; // scale of the last of them, 0..65535 (see detail_gather::ThinnedCount)
	};
	void hw_Load_Instanced();
	void hw_Unload_Instanced();
	void hw_EnsureInstanceCapacity(u32 required, bool gpu);
	void hw_BuildInstancedPasses();
	void hw_Fill_Instances();
	bool hw_Fill_Instances_Gpu(const InstanceJob* jobs, u32 job_count, u32 total);
	SPass* hw_InstancedPass(ref_pass& source);
	// Instanced pass lookup never creates anything, so recording threads may call it.
	bool hw_Instanced(light* L) const;
#endif
	void hw_Load_Shaders();
	void hw_Unload();
	void hw_Render(light* L = nullptr);
#if defined(USE_DX10) || defined(USE_DX11)
	void hw_Render_dump(const Fvector4 &consts, const Fvector4 &wave, const Fvector4 &wind, const Fvector4& prev_wave, const Fvector4& prev_wind, u32 var_id, u32 lod_id, light* L = nullptr);
#else	//	USE_DX10
	void hw_Render_dump(ref_constant array, u32 var_id, u32 lod_id, u32 c_base, light* L = nullptr);
#endif	//	USE_DX10

public:
	// get unpacked slot
	DetailSlot& QueryDB(int sx, int sz);

	void cache_Initialize();
	void cache_Update(int sx, int sz, Fvector& view, int limit);
	void cache_Task(int gx, int gz, Slot* D);
	Slot* cache_Query(int sx, int sz);
	void cache_Decompress(Slot* D);
	BOOL cache_Validate();
	// cache grid to world
	int cg2w_X(int x) { return cache_cx - dm_size + x; }
	int cg2w_Z(int z) { return cache_cz - dm_size + (dm_cache_line - 1 - z); }
	// world to cache grid 
	int w2cg_X(int x) { return x - cache_cx + dm_size; }
	int w2cg_Z(int z) { return cache_cz - dm_size + (dm_cache_line - 1 - z); }

	void Load();
	void Unload();
	void Render();
#if RENDER == R_R4
    bool CanRecordShadow() const;
    bool PrimeShadow(class RenderContext& context);
    void RenderShadow(float fade, light* source = nullptr);
    xrCriticalSection m_shadow_render_guard;
#endif

	/// MT stuff
	u32 m_frame_calc;
	xr_atomic_u32 m_frame_rendered;
	xrCriticalSection m_mt_calc_guard;
	void __stdcall MT_CALC();

	CDetailManager();
	virtual ~CDetailManager();
};

#endif //DetailManagerH
