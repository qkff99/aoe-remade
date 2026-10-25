#include "stdafx.h"
#include "../xrRender/DetailManager.h"

#include "../../xrEngine/igame_persistent.h"
#include "../../xrEngine/environment.h"

#include "../xrRenderDX10/dx10BufferUtils.h"
#if RENDER == R_R4
#include "../xrRenderPC_R4/RenderContext.h"
#include "../xrRenderPC_R4/DetailInstancing.h"
#include "../xrRenderPC_R4/DetailGather.h"
#include "../xrRender/dxRenderDeviceRender.h"
#include <ppl.h>
#include <vector>
#endif

#if RENDER == R_R4 && XRCORE_PROFILER == PROFILER_OPTICK
namespace
{
// GPU time of the main-pass grass, Optick builds only. Stamps: 0 before the instance fill, 1 after it (stream gather/copies),
// 2 after the draws. Results arrive a few frames late and are attached as tags to Render.Grass.GpuTimer.
struct GrassGpuTimer
{
    static constexpr u32 Depth = 4;
    struct Slot
    {
        ID3D11Query* disjoint = nullptr;
        ID3D11Query* stamp[3] = {};
        bool pending = false;
    };
    Slot slots[Depth];
    u32 next = 0, frame = u32(-1);
    ID3D11Device* owner = nullptr;
    bool failed = false;
    Slot* active = nullptr;

    void Release()
    {
        for (Slot& s : slots)
        {
            _RELEASE(s.disjoint);
            for (ID3D11Query*& q : s.stamp) _RELEASE(q);
            s.pending = false;
        }
        active = nullptr;
    }
    ~GrassGpuTimer() { Release(); }

    static bool Create(ID3D11Device* device, D3D11_QUERY type, ID3D11Query*& query)
    {
        if (query) return true;
        D3D11_QUERY_DESC desc{};
        desc.Query = type;
        return SUCCEEDED(device->CreateQuery(&desc, &query));
    }

    void Poll(ID3D11DeviceContext* context)
    {
        PROF_EVENT("Render.Grass.GpuTimer");
        // state: 0 nothing pending, 1 oldest still running, 2 query error, 3 disjoint, 40+k stamp k not available, 5 result
        u32 state = 0, pending = 0, last_hr = 0;
        for (u32 i = 0; i < Depth; ++i)
        {
            Slot& s = slots[(next + i) % Depth];
            if (!s.pending) continue;
            ++pending;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
            const HRESULT hr = context->GetData(s.disjoint, &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            last_hr = u32(hr);
            if (hr == S_FALSE)
            {
                state = 1;
                break; // newer slots cannot be ready either
            }
            UINT64 t[3] = {};
            if (hr != S_OK || disjoint.Disjoint)
            {
                s.pending = false;
                state = hr != S_OK ? 2 : 3;
                continue;
            }
            // The disjoint query can be answered before the stamps are; keep the slot and ask again next frame.
            bool ready = true, failed_stamp = false;
            for (u32 k = 0; k < 3 && ready; ++k)
            {
                const HRESULT stamp_hr = context->GetData(s.stamp[k], &t[k], sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH);
                if (stamp_hr != S_OK)
                {
                    ready = false;
                    failed_stamp = stamp_hr != S_FALSE;
                    state = 40 + k;
                    last_hr = u32(stamp_hr);
                }
            }
            if (!ready)
            {
                if (failed_stamp)
                {
                    s.pending = false;
                    continue;
                }
                break;
            }
            s.pending = false;
            state = 5;
            const double us = 1e6 / double(disjoint.Frequency);
            PROF_TAG("GPU grass us", u32((t[2] - t[1]) * us));
            PROF_TAG("GPU gather us", u32((t[1] - t[0]) * us));
            PROF_TAG("GPU grass total us", u32((t[2] - t[0]) * us));
        }
        PROF_TAG("GPU timer state", state);
        PROF_TAG("GPU timer pending", pending);
        PROF_TAG("GPU timer hr", last_hr);
    }

    bool Begin()
    {
        ID3D11Device* device = HW.pDevice;
        if (device != owner)
        {
            Release();
            owner = device;
            failed = false;
        }
        if (failed || frame == Device.dwFrame) return false;
        auto* context = RenderDeviceContext();
        Poll(context);
        Slot& s = slots[next];
        if (s.pending) return false;
        bool ok = Create(device, D3D11_QUERY_TIMESTAMP_DISJOINT, s.disjoint);
        for (ID3D11Query*& q : s.stamp) ok = ok && Create(device, D3D11_QUERY_TIMESTAMP, q);
        if (!ok)
        {
            failed = true;
            return false;
        }
        frame = Device.dwFrame;
        context->Begin(s.disjoint);
        context->End(s.stamp[0]);
        active = &s;
        return true;
    }

    void Stamp(u32 index) { RenderDeviceContext()->End(active->stamp[index]); }

    void End()
    {
        auto* context = RenderDeviceContext();
        context->End(active->stamp[2]);
        context->End(active->disjoint);
        active->pending = true;
        active = nullptr;
        next = (next + 1) % Depth;
    }
};

GrassGpuTimer& GrassTimer()
{
    static GrassGpuTimer timer;
    return timer;
}
}

struct GrassGpuScope
{
    bool on;
    explicit GrassGpuScope(bool owner) : on(owner && GrassTimer().Begin()) {}
    void Split() { if (on) GrassTimer().Stamp(1); }
    ~GrassGpuScope() { if (on) GrassTimer().End(); }
};
#else
struct GrassGpuScope
{
    explicit GrassGpuScope(bool) {}
    void Split() {}
};
#endif

// Vars to store wind prev frame data ( Motion vectors )
static u32 prev_frame = -1;
static float prev_time = 0;
static Fvector4	prev_dir1 = { 0, 0, 0 }, prev_dir2 = { 0, 0, 0 };

const int quant = 16384;
const int c_hdr = 10;
const int c_size = 4;

static D3DVERTEXELEMENT9 dwDecl[] =
{
	{0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, // pos
	{0, 12, D3DDECLTYPE_SHORT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0}, // uv
	D3DDECL_END()
};

#pragma pack(push,1)
struct vertHW
{
	float x, y, z;
	short u, v, t, mid;
};
#pragma pack(pop)

short QC(float v);
//{
//	int t=iFloor(v*float(quant)); clamp(t,-32768,32767);
//	return short(t&0xffff);
//}

float GoToValue(float& current, float go_to)
{
	float diff = abs(current - go_to);

	float r_value = Device.fTimeDelta;

	if (diff - r_value <= 0)
	{
		current = go_to;
		return 0;
	}

	return current < go_to ? r_value : -r_value;
}

void CDetailManager::hw_Load_Shaders()
{
	// Create shader to access constant storage
	ref_shader S;
	S.create("details\\set");
	R_constant_table& T0 = *(S->E[0]->passes[0]->constants);
	R_constant_table& T1 = *(S->E[1]->passes[0]->constants);
	hwc_consts = T0.get("consts");
	hwc_wave = T0.get("wave");
	hwc_wind = T0.get("dir2D");
	hwc_array = T0.get("array");
	hwc_s_consts = T1.get("consts");
	hwc_s_xform = T1.get("xform");
	hwc_s_array = T1.get("array");
}

#if RENDER == R_R4
static bool RecordablePass(const SPass& pass)
{
    if (!pass.vs || !pass.ps || !pass.vs->recording_safe || !pass.ps->recording_safe ||
        (pass.gs && pass.gs->gs && !pass.gs->recording_safe) ||
        (pass.hs && pass.hs->sh && !pass.hs->recording_safe) ||
        (pass.ds && pass.ds->sh && !pass.ds->recording_safe)) return false;
    if (pass.constants) for (const auto& constant : pass.constants->table)
        if (constant->handler && !constant->handler->parallel_safe()) return false;
    return true;
}

bool CDetailManager::CanRecordShadow() const
{
    if (!dtFS || !psDeviceFlags.is(rsDetails)) return true;
    for (const auto* object : objects)
    {
        if (!object->shader || !object->shader->E[0]) return false;
        for (const auto& pass : object->shader->E[0]->passes)
            if (!RecordablePass(*pass)) return false;
    }
    if (ps_r4_detail_instancing)
        for (const auto& entry : hw_instanced_passes)
            if (entry.shadow && entry.twin && !RecordablePass(*entry.twin)) return false;
    return true;
}

bool CDetailManager::PrimeShadow(RenderContext& context)
{
    if (!CanRecordShadow()) return false;
    if (!dtFS || !psDeviceFlags.is(rsDetails)) return true;
    for (const auto* object : objects)
        for (const auto& pass : object->shader->E[0]->passes) context.Prime(*pass);
    if (ps_r4_detail_instancing)
        for (const auto& entry : hw_instanced_passes)
            if (entry.shadow && entry.twin) context.Prime(*entry.twin);
    return true;
}

bool CDetailManager::hw_Instanced(light* L) const
{
    return ps_r4_detail_instancing && !L && hw_Geom_inst && hw_instance_frame == Device.dwFrame;
}

SPass* CDetailManager::hw_InstancedPass(ref_pass& source)
{
    for (auto& entry : hw_instanced_passes)
        if (entry.source._get() == source._get()) return entry.twin._get();
    return nullptr;
}

void CDetailManager::hw_BuildInstancedPasses()
{
    for (CDetail* object : objects)
        for (u32 lod = 0; lod < 2; ++lod)
        {
            ShaderElement* element = object->shader ? object->shader->E[lod]._get() : nullptr;
            if (!element) continue;
            for (auto& pass : element->passes)
            {
                if (!pass) continue;
                bool known = false;
                for (auto& entry : hw_instanced_passes)
                    if (entry.source._get() == pass._get())
                    {
                        entry.shadow = entry.shadow || lod == 0;
                        known = true;
                        break;
                    }
                if (known) continue;
                // Unsupported shaders get a null twin once and stay on the batched path.
                SPass* twin = DEV->_CreateDetailTwin(*pass);
                InstancedPass entry;
                entry.source = pass;
                if (twin) entry.twin = twin;
                entry.shadow = lod == 0;
                hw_instanced_passes.push_back(entry);
                Msg("* [DETAILS] Instancing %s: %s", twin ? "ready" : "unavailable, batched path stays",
                    pass->vs ? pass->vs->cName.c_str() : "?");
            }
        }
}

void CDetailManager::hw_EnsureInstanceCapacity(u32 required, bool gpu)
{
    if (hw_instanceVB && required <= hw_instance_capacity && gpu == hw_instance_gpu) return;
    u32 capacity = hw_instance_capacity;
    if (required > capacity)
    {
        R_ASSERT2(required <= (u32(-1) >> 8), "detail instance capacity overflow");
        capacity = (required + required / 4 + 4095) & ~4095u;
    }
    capacity = _max(capacity, 1u << 16);
    if (hw_instanceVB)
    {
        HW.stats_manager.decrement_stats_vb(hw_instanceVB);
        _RELEASE(hw_instanceVB);
    }
    if (hw_gatherer) hw_gatherer->ResetOutput();
    D3D_BUFFER_DESC desc{};
    desc.ByteWidth = capacity * sizeof(detail_instancing::Instance);
    if (gpu)
    {
        // Written by the gather shader, read as a vertex buffer.
        desc.Usage = D3D_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_UNORDERED_ACCESS;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    }
    else
    {
        desc.Usage = D3D_USAGE_DYNAMIC;
        desc.BindFlags = D3D_BIND_VERTEX_BUFFER;
        desc.CPUAccessFlags = D3D_CPU_ACCESS_WRITE;
    }
    R_CHK(HW.pDevice->CreateBuffer(&desc, nullptr, &hw_instanceVB));
    HW.stats_manager.increment_stats_vb(hw_instanceVB);
    hw_instance_capacity = capacity;
    hw_instance_gpu = gpu;
    Msg("* [DETAILS] Instance stream: %u instances (%u KB, %s)", capacity, desc.ByteWidth / 1024, gpu ? "gpu gather" : "cpu fill");
}

// Fills the instance record of one list. Every item gets a record (alpha 0 collapses the blade in the shader), so the
// output offset of each list is known before any list is processed and the lists can be written in parallel.
static void FillInstanceList(CDetailManager::SlotRenderItems& list, detail_instancing::Instance* destination, bool valid_delta)
{
    const u32 count = u32(list.size());
    if (list.cache_state == CDetailManager::SlotRenderItems::Ready && list.cached_constants.size() == count * 5)
    {
        const Fvector4* rows = list.cached_constants.data();
        const Fvector4* extra = rows + count * 4;
        for (u32 i = 0; i < count; ++i)
        {
            auto& out = destination[i];
            memcpy(out.m0, &rows[i * 4 + 0], sizeof(Fvector4));
            memcpy(out.m1, &rows[i * 4 + 1], sizeof(Fvector4));
            memcpy(out.m2, &rows[i * 4 + 2], sizeof(Fvector4));
            memcpy(out.normal_alpha, &extra[i], sizeof(Fvector4));
            out.sun_hemi_distance[0] = rows[i * 4 + 3].x;
            out.sun_hemi_distance[1] = rows[i * 4 + 3].w;
            out.sun_hemi_distance[2] = list.cached_max_distance;
            out.sun_hemi_distance[3] = 0.f;
        }
        return;
    }
    bool settled = valid_delta;
    for (u32 i = 0; i < count; ++i)
    {
        CDetailManager::SlotItem& instance = *list[i];
        instance.alpha += GoToValue(instance.alpha, instance.alpha_target);
        settled = settled && instance.alpha == instance.alpha_target && _valid(instance.alpha) &&
            instance.alpha > 0.f && _valid(instance.distance) && instance.distance >= 0.f;
        auto& out = destination[i];
        const Fmatrix& M = instance.mRotY_calculated;
        out.m0[0] = M._11; out.m0[1] = M._21; out.m0[2] = M._31; out.m0[3] = M._41;
        out.m1[0] = M._12; out.m1[1] = M._22; out.m1[2] = M._32; out.m1[3] = M._42;
        out.m2[0] = M._13; out.m2[1] = M._23; out.m2[2] = M._33; out.m2[3] = M._43;
        out.normal_alpha[0] = instance.normal.x;
        out.normal_alpha[1] = instance.normal.y;
        out.normal_alpha[2] = instance.normal.z;
        out.normal_alpha[3] = instance.alpha;
        out.sun_hemi_distance[0] = instance.c_sun;
        out.sun_hemi_distance[1] = instance.c_hemi;
        out.sun_hemi_distance[2] = instance.distance;
        out.sun_hemi_distance[3] = 0.f;
    }
    list.cache_state = settled ? CDetailManager::SlotRenderItems::Settled : CDetailManager::SlotRenderItems::Invalid;
}

// Writes every visible blade of every wave variant once per frame. Distance fade moves to the shader, so the same
// stream serves the main pass and the sun cascades.
void CDetailManager::hw_Fill_Instances()
{
    PROF_EVENT("Render.Grass.FillInstances");
    hw_BuildInstancedPasses();
    // The settings this capture was taken with.
    PROF_TAG("Cfg density x1000", u32(ps_r__Detail_density * 1000.f + 0.5f));
    PROF_TAG("Cfg radius m", dm_current_size * 2);
    PROF_TAG("Cfg thin start", u32(ps_r4_detail_thin.x + 0.5f));
    PROF_TAG("Cfg thin end", u32(ps_r4_detail_thin.y + 0.5f));
    PROF_TAG("Cfg thin keep x100", u32(ps_r4_detail_thin.z * 100.f + 0.5f));
    std::vector<InstanceJob> jobs;
    u32 total = 0, full_total = 0;
    // Only the gather stream can be thinned (r4_detail_thin): the CPU stream writes whole lists.
    auto layout = [&](bool thin)
    {
        const detail_gather::Thin thinning = detail_gather::ReadThin();
        jobs.clear();
        total = full_total = 0;
        for (u32 variant = 0; variant < 3; ++variant)
        {
            hw_inst_base[variant].assign(objects.size(), 0);
            hw_inst_count[variant].assign(objects.size(), 0);
            for (u32 O = 0; O < objects.size(); ++O)
            {
                hw_inst_base[variant][O] = total;
                for (SlotItemVec* items : m_visibles[variant][O])
                {
                    SlotRenderItems* list = static_cast<SlotRenderItems*>(items);
                    const u32 full = u32(list->size());
                    const detail_gather::Thinned kept = thin && thinning.keep < 1.f ?
                        detail_gather::ThinnedCount(full, list->slot_distance, thinning) : detail_gather::Thinned{full, 65535u};
                    jobs.push_back({list, total, kept.count, kept.tip});
                    total += kept.count;
                    full_total += full;
                }
                hw_inst_count[variant][O] = total - hw_inst_base[variant][O];
            }
        }
    };
    bool gpu = ps_r4_detail_instancing >= 2 && detail_gather::Acquire(hw_gatherer);
    layout(gpu);
    hw_EnsureInstanceCapacity(_max(total, 1u), gpu);
    if (gpu && hw_Fill_Instances_Gpu(jobs.data(), u32(jobs.size()), total))
    {
        hw_instance_frame = Device.dwFrame;
        PROF_TAG("Instance stream", total);
        PROF_TAG("Instance stream before thinning", full_total);
        return;
    }
    if (gpu)
    {
        // Resource creation failed: stay on the CPU stream for good.
        hw_gatherer->failed = true;
        Msg("! [DETAILS] GPU gather unavailable, using the CPU instance stream");
        layout(false);
        hw_EnsureInstanceCapacity(_max(total, 1u), false);
    }
    const bool valid_delta = _valid(Device.fTimeDelta) && Device.fTimeDelta >= 0.f;
    {
        // Cache blocks are built on the owner (it keeps a shared byte budget); the fill itself is parallel.
        PROF_EVENT("Render.Grass.FillInstances.Cache");
        for (const InstanceJob& job : jobs)
        {
            if (!valid_delta) job.list->cache_state = SlotRenderItems::Invalid;
            if (job.list->cache_state == SlotRenderItems::Settled) BuildRenderCache(*job.list);
        }
    }
    D3D11_MAPPED_SUBRESOURCE mapped;
    CHK_DX(HW.pContext->Map(hw_instanceVB, 0, D3D_MAP_WRITE_DISCARD, 0, &mapped));
    auto* destination = static_cast<detail_instancing::Instance*>(mapped.pData);
    {
        PROF_EVENT("Render.Grass.FillInstances.Write");
        concurrency::parallel_for(size_t(0), jobs.size(), [&](size_t index)
        {
            FillInstanceList(*jobs[index].list, destination + jobs[index].base, valid_delta);
        });
    }
    HW.pContext->Unmap(hw_instanceVB, 0);
    hw_instance_frame = Device.dwFrame;
    PROF_TAG("Instance stream", total);
}
#endif

void CDetailManager::hw_Render(light* L)
{
	PROF_EVENT("CDetailManager::hw_Render");
	// Render-prepare
	//	Update timer
	//	Can't use Device.fTimeDelta since it is smoothed! Don't know why, but smoothed value looks more choppy!
	float fDelta = Device.fTimeGlobal - m_global_time_old;
	if ((fDelta < 0) || (fDelta > 1)) fDelta = 0.03;
	m_global_time_old = Device.fTimeGlobal;

	m_time_rot_1 += (PI_MUL_2 * fDelta / swing_current.rot1);
	m_time_rot_2 += (PI_MUL_2 * fDelta / swing_current.rot2);
	m_time_pos += fDelta * swing_current.speed;

	//float		tm_rot1		= (PI_MUL_2*Device.fTimeGlobal/swing_current.rot1);
	//float		tm_rot2		= (PI_MUL_2*Device.fTimeGlobal/swing_current.rot2);
	float tm_rot1 = m_time_rot_1;
	float tm_rot2 = m_time_rot_2;

	Fvector4 dir1, dir2;
	dir1.set(_sin(tm_rot1), 0, _cos(tm_rot1), 0).normalize().mul(swing_current.amp1);
	dir2.set(_sin(tm_rot2), 0, _cos(tm_rot2), 0).normalize().mul(swing_current.amp2);

	// Setup geometry and DMA
	RCache.set_CullMode(CULL_NONE);
	RCache.set_xform_world(Fidentity);
	RCache.set_Geometry(hw_Geom);
#if RENDER == R_R4
	// Main pass and sun cascades share one instance stream per frame; it is filled here, on the owner.
	GrassGpuScope gpu_timer(ps_r4_detail_instancing && !L && !RenderContextId());
	if (ps_r4_detail_instancing && !L && !RenderContextId())
	{
		// Built on first use, so the cvar can be switched at any time.
		if (!hw_Geom_inst)
			hw_Load_Instanced();
		if (hw_Geom_inst && hw_instance_frame != Device.dwFrame)
			hw_Fill_Instances();
	}
	gpu_timer.Split();
#endif
	float scale = 1.f / float(quant);
	Fvector4 wave, prev_wave;
	Fvector4 consts;

	// Wave0
	consts.set(scale, scale, ps_r__Detail_l_aniso, ps_r__Detail_l_ambient);
	//wave.set				(1.f/5.f,		1.f/7.f,	1.f/3.f,	Device.fTimeGlobal*swing_current.speed);
	wave.set(1.f / 5.f, 1.f / 7.f, 1.f / 3.f, m_time_pos);
	prev_wave.set(1.f / 5.f, 1.f / 7.f, 1.f / 3.f, prev_time);
	//RCache.set_c			(&*hwc_consts,	scale,		scale,		ps_r__Detail_l_aniso,	ps_r__Detail_l_ambient);				// consts
	//RCache.set_c			(&*hwc_wave,	wave.div(PI_MUL_2));	// wave
	//RCache.set_c			(&*hwc_wind,	dir1);																					// wind-dir
	//hw_Render_dump			(&*hwc_array,	1, 0, c_hdr );
	hw_Render_dump(consts, wave.div(PI_MUL_2), dir1, prev_wave.div(PI_MUL_2), prev_dir1, 1, 0, L);

	// Wave1
	//wave.set				(1.f/3.f,		1.f/7.f,	1.f/5.f,	Device.fTimeGlobal*swing_current.speed);
	wave.set(1.f / 3.f, 1.f / 7.f, 1.f / 5.f, m_time_pos);
	prev_wave.set(1.f / 3.f, 1.f / 7.f, 1.f / 5.f, prev_time);
	//RCache.set_c			(&*hwc_wave,	wave.div(PI_MUL_2));	// wave
	//RCache.set_c			(&*hwc_wind,	dir2);																					// wind-dir
	//hw_Render_dump			(&*hwc_array,	2, 0, c_hdr );
	hw_Render_dump(consts, wave.div(PI_MUL_2), dir2, prev_wave.div(PI_MUL_2), prev_dir2, 2, 0, L);

	// Still
	consts.set(scale, scale, scale, 1.f);
	//RCache.set_c			(&*hwc_s_consts,scale,		scale,		scale,				1.f);
	//RCache.set_c			(&*hwc_s_xform,	Device.mFullTransform);
	//hw_Render_dump			(&*hwc_s_array,	0, 1, c_hdr );
	hw_Render_dump(consts, wave.div(PI_MUL_2), dir2, prev_wave.div(PI_MUL_2), prev_dir2, 0, 1, L);

	if (prev_frame != Device.dwFrame) 
	{
		prev_frame = Device.dwFrame;
		
		// Prev Frame swing time
		prev_time = m_time_pos;

		// Prev frame dir
		prev_dir1.set(dir1);
		prev_dir2.set(dir2);
	}

	RCache.set_CullMode(CULL_CCW);
}

void CDetailManager::hw_Render_dump(const Fvector4& consts, const Fvector4& wave, const Fvector4& wind, 
									const Fvector4& prev_wave, const Fvector4& prev_wind, u32 var_id, u32 lod_id, light* L)
{
    PROF_EVENT("Render.Grass.ConstantBlocks");
#if RENDER == R_R4
    aoe_temporal::SetIdentity(this, nullptr);
    const bool valid_delta = _valid(Device.fTimeDelta) && Device.fTimeDelta >= 0.f;
    const bool use_cache = hw_BatchSize && valid_delta && RCache.RenderPhase(RImplementation.phase) == CRender::PHASE_NORMAL &&
        !Device.m_SecondViewport.IsSVPFrame() && _valid(fade_distance) && fade_distance >= 0.f &&
        RImplementation.GMBase.is_sector_visible(RImplementation.pOutdoorSector);
    u32 cached_instances = 0, legacy_instances = 0, instanced_total = 0;
#endif
	if (RCache.RenderPhase(RImplementation.phase) == CRender::PHASE_SMAP && var_id == 0)
		return;
#if RENDER == R_R4
    aoe_temporal::AnimationHistory(true, prev_wave, prev_wind);
    const bool instanced = hw_Instanced(L) && RImplementation.GMBase.is_sector_visible(RImplementation.pOutdoorSector);
    bool geometry_instanced = false;
    if (instanced) RCache.set_Geometry(hw_Geom);
#endif

	static shared_str strConsts("consts");
	static shared_str strWave("wave");
	static shared_str strDir2D("dir2D");
	static shared_str strArray("array");
	static shared_str strXForm("xform");

	// Vanilla grass/trees wind
	static shared_str strWavePrev("wave_prev");
	static shared_str strDir2DPrev("dir2D_prev");

	// Grass Benders
	static shared_str strPrevPos("benders_prevpos");
	static shared_str strPos("benders_pos");
	static shared_str strGrassSetup("benders_setup");

	static shared_str strExData("exdata");
	static shared_str strGrassAlign("grass_align");

	// Grass benders data
	IGame_Persistent::grass_data& GData = g_pGamePersistent->grass_shader_data;
	Fvector4 player_pos = { 0, 0, 0, 0 };
	int BendersQty = _min(16, ps_ssfx_grass_interactive.y + 1);

	// Add Player?
	if (ps_ssfx_grass_interactive.x > 0)
		player_pos.set(Device.vCameraPosition.x, Device.vCameraPosition.y, Device.vCameraPosition.z, -1);

	if (!RenderContextId()) Device.Statistic->RenderDUMP_DT_Count = 0;

	// Matrices and offsets
	u32 vOffset = 0;
	u32 iOffset = 0;

	vis_list& list = m_visibles[var_id];

	CEnvDescriptor& desc = *g_pGamePersistent->Environment().CurrentEnv;
	Fvector c_sun, c_ambient, c_hemi;
	c_sun.set(desc.sun_color.x, desc.sun_color.y, desc.sun_color.z);
	c_sun.mul(.5f);
	c_ambient.set(desc.ambient.x, desc.ambient.y, desc.ambient.z);
	c_hemi.set(desc.hemi_color.x, desc.hemi_color.y, desc.hemi_color.z);

	// Iterate
	for (u32 O = 0; O < objects.size(); O++)
	{
		CDetail& Object = *objects[O];
		xr_vector<SlotItemVec*>& vis = list[O];
		if (!vis.empty())
		{
#if RENDER == R_R4
			// An object is drawn instanced only when every one of its passes has an instanced twin.
			bool object_instanced = instanced && O < hw_inst_count[var_id].size() && hw_inst_count[var_id][O];
			if (object_instanced)
				for (auto& source : Object.shader->E[lod_id]->passes)
					if (!source || !hw_InstancedPass(source))
					{
						object_instanced = false;
						break;
					}
			if (instanced && object_instanced != geometry_instanced)
			{
				RCache.set_Geometry(object_instanced ? hw_Geom_inst : hw_Geom);
				if (object_instanced)
				{
					ID3D11Buffer* stream = hw_instanceVB;
					const UINT stride = sizeof(detail_instancing::Instance), offset = 0;
					RenderDeviceContext()->IASetVertexBuffers(1, 1, &stream, &stride, &offset);
				}
				geometry_instanced = object_instanced;
			}
#endif
			for (u32 iPass = 0; iPass < Object.shader->E[lod_id]->passes.size(); ++iPass)
			{
				// Setup matrices + colors (and flush it as necessary)
				//RCache.set_Element				(Object.shader->E[lod_id]);
#if RENDER == R_R4
				SPass* twin = object_instanced ? hw_InstancedPass(Object.shader->E[lod_id]->passes[iPass]) : nullptr;
				if (twin)
					RCache.set_Pass(*twin);
				else
#endif
				RCache.set_Element(Object.shader->E[lod_id], iPass);
				RImplementation.apply_lmaterial();

				//	This could be cached in the corresponding consatant buffer
				//	as it is done for DX9
				RCache.set_c(strConsts, consts);
				RCache.set_c(strWave, wave);
				RCache.set_c(strDir2D, wind);
				RCache.set_c(strXForm, Device.mFullTransform);
				RCache.set_c(strGrassAlign, ps_ssfx_terrain_grass_align);

				RCache.set_c(strWavePrev, prev_wave);
				RCache.set_c(strDir2DPrev, prev_wind);

				if (ps_ssfx_grass_interactive.y > 0)
				{
					RCache.set_c(strGrassSetup, ps_ssfx_int_grass_params_1);

					Fvector4* c_grass;
					{
						void* GrassData;
						RCache.get_ConstantDirect(strPos, BendersQty * sizeof(Fvector4) * 2, &GrassData, 0, 0);
						c_grass = (Fvector4*)GrassData;
					}
					VERIFY(c_grass);

					if (c_grass)
					{
						c_grass[0].set(player_pos);
						c_grass[16].set(0.0f, -99.0f, 0.0f, 1.0f);

						for (int Bend = 1; Bend < BendersQty; Bend++)
						{
							c_grass[Bend].set(GData.pos[Bend].x, GData.pos[Bend].y, GData.pos[Bend].z, GData.radius_curr[Bend]);
							c_grass[Bend + 16].set(GData.dir[Bend].x, GData.dir[Bend].y, GData.dir[Bend].z, GData.str[Bend]);
						}
					}

					Fvector4* c_prev_grass;
					{
						void* prev_GrassData;
						RCache.get_ConstantDirect(strPrevPos, BendersQty * sizeof(Fvector4) * 2, &prev_GrassData, 0, 0);
						c_prev_grass = (Fvector4*)prev_GrassData;
					}
					VERIFY(c_prev_grass);

					if (c_prev_grass)
					{
						for (int Bend = 0; Bend < BendersQty; Bend++)
						{
							c_prev_grass[Bend].set(GData.prev_pos[Bend]);
							c_prev_grass[Bend + 16].set(GData.prev_dir[Bend]);
						}
					}
				}

#if RENDER == R_R4
				if (twin)
				{
					static shared_str strFade("aoe_detail_fade");
					RCache.set_c(strFade, Fvector4().set(fade_distance, 0.f, 0.f, 0.f));
					const u32 count = hw_inst_count[var_id][O];
					RCache.RenderInstanced(D3DPT_TRIANGLELIST, count, hw_inst_vertex_offset[O], 0, Object.number_vertices,
						hw_inst_index_offset[O], Object.number_indices / 3, hw_inst_base[var_id][O]);
					if (!RenderContextId()) Device.Statistic->RenderDUMP_DT_Count += count;
					RCache.stat.r.s_details.add(count * Object.number_vertices);
					instanced_total += count;
					continue;
				}
#endif
				Fvector4* c_ExData = 0;
				{
					void* pExtraData;
					RCache.get_ConstantDirect(strExData, hw_BatchSize * sizeof(Fvector4), &pExtraData, 0, 0);
					c_ExData = (Fvector4*)pExtraData;
				}
				VERIFY(c_ExData);

				//ref_constant constArray = RCache.get_c(strArray);
				//VERIFY(constArray);

				//u32			c_base				= x_array->vs.index;
				//Fvector4*	c_storage			= RCache.get_ConstantCache_Vertex().get_array_f().access(c_base);
				Fvector4* c_storage = 0;
				//	Map constants to memory directly
				{
					void* pVData;
					RCache.get_ConstantDirect(strArray,
					                          hw_BatchSize * sizeof(Fvector4) * 4,
					                          &pVData, 0, 0);
					c_storage = (Fvector4*)pVData;
				}
				VERIFY(c_storage);

				u32 dwBatch = 0;
                auto flush_batch = [&]()
                {
                    if (!RenderContextId()) Device.Statistic->RenderDUMP_DT_Count += dwBatch;
                    const u32 vertices = dwBatch * Object.number_vertices;
                    RCache.Render(D3DPT_TRIANGLELIST, vOffset, 0, vertices, iOffset, dwBatch * Object.number_indices / 3);
                    RCache.stat.r.s_details.add(vertices);
                    dwBatch = 0;
                    void* data = nullptr;
                    RCache.get_ConstantDirect(strArray, hw_BatchSize * sizeof(Fvector4) * 4, &data, 0, 0);
                    c_storage = static_cast<Fvector4*>(data);
                    VERIFY(c_storage);
                    data = nullptr;
                    RCache.get_ConstantDirect(strExData, hw_BatchSize * sizeof(Fvector4), &data, 0, 0);
                    c_ExData = static_cast<Fvector4*>(data);
                };


				xr_vector<SlotItemVec*>::iterator _vI = vis.begin();
				xr_vector<SlotItemVec*>::iterator _vE = vis.end();
				for (; _vI != _vE; _vI++)
				{
					SlotItemVec* items = *_vI;
#if RENDER == R_R4
                    auto& cached = *static_cast<SlotRenderItems*>(items);
                    if (!valid_delta) cached.cache_state = SlotRenderItems::Invalid;
                    if (use_cache && cached.cache_state == SlotRenderItems::Settled) BuildRenderCache(cached);
                    if (use_cache && cached.cache_state == SlotRenderItems::Ready && cached.cached_max_distance <= fade_distance)
                    {
                        const u32 count = u32(items->size());
                        cached_instances += count;
                        for (u32 first = 0; first < count;)
                        {
                            const u32 amount = _min(count - first, hw_BatchSize - dwBatch);
                            CopyMemory(c_storage + dwBatch * 4, cached.cached_constants.data() + first * 4, amount * 4 * sizeof(Fvector4));
                            if (c_ExData) CopyMemory(c_ExData + dwBatch, cached.cached_constants.data() + count * 4 + first, amount * sizeof(Fvector4));
                            first += amount;
                            dwBatch += amount;
                            if (dwBatch == hw_BatchSize) flush_batch();
                        }
                        continue;
                    }
                    bool settled = use_cache;
#endif
					SlotItemVecIt _iI = items->begin();
					SlotItemVecIt _iE = items->end();
					for (; _iI != _iE; _iI++)
					{
						SlotItem& Instance = **_iI;

						if (!RImplementation.GMBase.is_sector_visible(RImplementation.pOutdoorSector))
							continue;

						if (RCache.RenderPhase(RImplementation.phase) == CRender::PHASE_SMAP && L)
						{
							if (!L->GMLight.is_sector_visible(RImplementation.pOutdoorSector))
								continue;

							if (L->position.distance_to_sqr(Instance.position) >= _sqr(L->range))
								continue;
						}

						u32 base = dwBatch * 4;

						Instance.alpha += GoToValue(Instance.alpha, Instance.alpha_target);

						float scale = 1.f;

						// Sort of fade using the scale
						// fade_distance == -1 use light_position to define "fade", anything else uses fade_distance
						if (fade_distance <= -1)
							scale *= 1.0f - Instance.position.distance_to_xz_sqr(light_position) * 0.005f;
						else if (Instance.distance > fade_distance)
							scale *= 1.0f - abs(Instance.distance - fade_distance) * 0.005f;

						if (scale <= 0 || Instance.alpha <= 0)
                            break;
#if RENDER == R_R4
                        settled = settled && scale == 1.f && Instance.alpha == Instance.alpha_target && _valid(Instance.alpha);
                        ++legacy_instances;
#endif

						// Build matrix ( 3x4 matrix, last row - color )
						Fmatrix& M = Instance.mRotY_calculated;
						c_storage[base + 0].set(M._11 * scale, M._21 * scale, M._31 * scale, M._41);
						c_storage[base + 1].set(M._12 * scale, M._22 * scale, M._32 * scale, M._42);
						c_storage[base + 2].set(M._13 * scale, M._23 * scale, M._33 * scale, M._43);
						//RCache.set_ca(&*constArray, base+0, M._11*scale,	M._21*scale,	M._31*scale,	M._41	);
						//RCache.set_ca(&*constArray, base+1, M._12*scale,	M._22*scale,	M._32*scale,	M._42	);
						//RCache.set_ca(&*constArray, base+2, M._13*scale,	M._23*scale,	M._33*scale,	M._43	);

						// Build color
						// R2 only needs hemisphere
						float h = Instance.c_hemi;
						float s = Instance.c_sun;
						c_storage[base + 3].set(s, s, s, h);

						if (c_ExData)
							c_ExData[dwBatch].set(Instance.normal.x, Instance.normal.y, Instance.normal.z, Instance.alpha);

						//RCache.set_ca(&*constArray, base+3, s,				s,				s,				h		);
						dwBatch ++;
						if (dwBatch == hw_BatchSize)
						{
                            flush_batch();
						}
					}
#if RENDER == R_R4
                    if (use_cache) cached.cache_state = settled && _iI == _iE ? SlotRenderItems::Settled : SlotRenderItems::Invalid;
#endif
				}
				// flush if nessecary
				if (dwBatch)
				{
					if (!RenderContextId()) Device.Statistic->RenderDUMP_DT_Count += dwBatch;
					u32 dwCNT_verts = dwBatch * Object.number_vertices;
					u32 dwCNT_prims = (dwBatch * Object.number_indices) / 3;
					//RCache.get_ConstantCache_Vertex().b_dirty				=	TRUE;
					//RCache.get_ConstantCache_Vertex().get_array_f().dirty	(c_base,c_base+dwBatch*4);
					RCache.Render(D3DPT_TRIANGLELIST, vOffset, 0, dwCNT_verts, iOffset, dwCNT_prims);
					RCache.stat.r.s_details.add(dwCNT_verts);
				}
			}
			// Clean up
			// KD: we must not clear vis on r2 since we want details shadows
			if (ps_ssfx_grass_shadows.x <= 0)
			{
				if (!psDeviceFlags2.test(rsGrassShadow) || RImplementation.PHASE_NORMAL == RCache.RenderPhase(RImplementation.phase)) // phase normal without shadows
					vis.clear_not_free();
			}
		}
		vOffset += hw_BatchSize * Object.number_vertices;
		iOffset += hw_BatchSize * Object.number_indices;
	}
#if RENDER == R_R4
    if (geometry_instanced) RCache.set_Geometry(hw_Geom);
    PROF_TAG("Instanced instances", instanced_total);
    PROF_TAG("Cached instances", cached_instances);
    PROF_TAG("Legacy instances", legacy_instances);
    PROF_TAG("Cache bytes", m_render_cache_bytes);
#endif

}
