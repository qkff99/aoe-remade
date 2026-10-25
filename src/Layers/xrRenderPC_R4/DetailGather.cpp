#include "stdafx.h"
#include "DetailGather.h"
#include "DetailGatherCodec.h"
#include "DetailInstancing.h"
#include "../xrRender/DetailManager.h"
#include "../xrRender/xrRender_console.h"
#include "../xrRenderDX10/StateManager/dx10ShaderResourceStateCache.h"
#include <algorithm>
#include <numeric>
#include <ppl.h>
#include <vector>

extern float GoToValue(float& current, float go_to);

namespace detail_gather
{
void Buffer::Release()
{
    _RELEASE(srv);
    _RELEASE(buffer);
    capacity = 0;
}

// Typed uint4 view over a plain buffer: `stride` is the record size in bytes (multiple of 16).
static bool CreateBuffer(Buffer& target, u32 elements, u32 stride, bool dynamic)
{
    target.Release();
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = elements * stride;
    desc.Usage = dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = dynamic ? D3D11_CPU_ACCESS_WRITE : 0;
    if (FAILED(HW.pDevice->CreateBuffer(&desc, nullptr, &target.buffer)))
        return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32G32B32A32_UINT;
    view.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    view.Buffer.NumElements = desc.ByteWidth / 16;
    if (FAILED(HW.pDevice->CreateShaderResourceView(target.buffer, &view, &target.srv)))
    {
        target.Release();
        return false;
    }
    target.capacity = elements;
    return true;
}

bool Gatherer::Init()
{
    if (HW.FeatureLevel < D3D_FEATURE_LEVEL_11_0)
    {
        Msg("! [DETAILS] GPU gather needs feature level 11, falling back to the CPU stream");
        return false;
    }
    const char* source = ShaderSource();
    ID3DBlob *code = nullptr, *errors = nullptr;
    HRESULT hr = D3DCompile(source, xr_strlen(source), "aoe_detail_gather", nullptr, nullptr, "main", "cs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (SUCCEEDED(hr))
        hr = HW.pDevice->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader);
    if (FAILED(hr))
        Msg("! [DETAILS] GPU gather shader failed (0x%08x): %s", u32(hr),
            errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
    _RELEASE(code);
    _RELEASE(errors);
    return SUCCEEDED(hr);
}

void Gatherer::ResetOutput()
{
    _RELEASE(output_uav);
    output_buffer = nullptr;
}

Gatherer::~Gatherer()
{
    ResetOutput();
    arena.Release();
    transient.Release();
    ranges.Release();
    _RELEASE(shader);
}

Thin ReadThin()
{
    const float start = _max(ps_r4_detail_thin.x, 0.f), end = ps_r4_detail_thin.y;
    return {start, end > start ? 1.f / (end - start) : 0.f, clampr(ps_r4_detail_thin.z, 0.05f, 1.f)};
}

Thinned ThinnedCount(u32 full, float distance_sq, const Thin& thin)
{
    if (thin.keep >= 1.f || !full)
        return {full, 65535u};
    if (!_valid(distance_sq) || distance_sq < 0.f)
        distance_sq = 0.f;
    const float share = 1.f + (thin.keep - 1.f) * clampr((_sqrt(distance_sq) - thin.start) * thin.inv_range, 0.f, 1.f);
    const float wanted = full * share;
    const u32 count = _min(full, _max(u32(ceilf(wanted)), 1u));
    return {count, u32(clampr(wanted - float(count - 1), 0.f, 1.f) * 65535.f + 0.5f)};
}

Gatherer* Acquire(Gatherer*& slot)
{
    if (!slot)
    {
        slot = xr_new<Gatherer>();
        slot->failed = !slot->Init();
    }
    return slot->failed ? nullptr : slot;
}

// Packs one list into the transient buffer. Returns whether every item is at rest (alpha reached its target).
static bool PackList(CDetailManager::SlotRenderItems& list, Packed* destination, bool valid_delta)
{
    bool settled = valid_delta;
    const u32 count = u32(list.size());
    // Records are stored in a golden-ratio stride order, so the first N of a list are an even sample of the slot and a
    // thinned range stays the same from frame to frame.
    u32 stride = count > 2 ? u32(count * 0.6180339887f) : 1;
    while (std::gcd(stride, count) != 1)
        ++stride;
    u32 next = 0;
    for (u32 i = 0; i < count; ++i, next = next + stride >= count ? next + stride - count : next + stride)
    {
        CDetailManager::SlotItem& instance = *list[next];
        instance.alpha += GoToValue(instance.alpha, instance.alpha_target);
        settled = settled && instance.alpha == instance.alpha_target && _valid(instance.alpha) && instance.alpha > 0.f &&
            _valid(instance.distance) && instance.distance >= 0.f;
        const Fmatrix& M = instance.mRotY_calculated;
        const float m[3][3] = {{M._11, M._12, M._13}, {M._21, M._22, M._23}, {M._31, M._32, M._33}};
        const float t[3] = {M._41, M._42, M._43};
        const float n[3] = {instance.normal.x, instance.normal.y, instance.normal.z};
        Packed packed;
        Pack(m, t, n, instance.c_sun, instance.c_hemi, instance.alpha, packed);
        destination[i] = packed;
    }
    return settled;
}
}

// Builds this frame's instance stream on the GPU: lists that have not changed since they were last uploaded are read from
// the resident arena, everything else is packed into a transient buffer (and, once it is at rest, copied into the arena).
bool CDetailManager::hw_Fill_Instances_Gpu(const InstanceJob* jobs, u32 job_count, u32 total)
{
    using namespace detail_gather;
    PROF_EVENT("Render.Grass.Gather");
    Gatherer* gatherer = Acquire(hw_gatherer);
    if (!gatherer || !hw_instance_gpu || !hw_instanceVB)
        return false;
    Gatherer& g = *gatherer;
    auto* context = RenderDeviceContext();
    const bool valid_delta = _valid(Device.fTimeDelta) && Device.fTimeDelta >= 0.f;

    if (!g.arena.buffer || g.arena_wanted > g.arena.capacity)
    {
        const u32 wanted = _max(g.arena_wanted, total * 2 + (1u << 16));
        if (!CreateBuffer(g.arena, (wanted + 4095) & ~4095u, sizeof(Packed), false))
            return false;
        g.arena_used = 0;
        g.arena_wanted = 0;
        ++g.epoch;
        Msg("* [DETAILS] Gather arena: %u records (%u KB)", g.arena.capacity, g.arena.capacity * u32(sizeof(Packed)) / 1024);
    }

    std::vector<u32> transient_at(job_count, u32(-1)), pending;
    std::vector<u8> settled(job_count, 0);
    u32 transient_total = 0, range_count = 0;
    for (u32 i = 0; i < job_count; ++i)
    {
        SlotRenderItems& list = *jobs[i].list;
        const u32 count = u32(list.size());
        if (!count)
            continue;
        ++range_count;
        if (list.arena_epoch == g.epoch && list.arena_revision == list.revision)
            continue; // resident
        transient_at[i] = transient_total;
        transient_total += count;
        pending.push_back(i);
    }
    if (!range_count)
        return true;

    if (!g.transient.buffer || transient_total > g.transient.capacity)
        if (!CreateBuffer(g.transient, (_max(transient_total, 1u << 14) + transient_total / 4 + 4095) & ~4095u, sizeof(Packed), true))
            return false;
    if (transient_total)
    {
        PROF_EVENT("Render.Grass.Gather.Pack");
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(context->Map(g.transient.buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            return false;
        auto* destination = static_cast<Packed*>(mapped.pData);
        concurrency::parallel_for(size_t(0), pending.size(), [&](size_t index)
        {
            const u32 i = pending[index];
            settled[i] = PackList(*jobs[i].list, destination + transient_at[i], valid_delta);
        });
        context->Unmap(g.transient.buffer, 0);
    }

    // Lists at rest move into the arena (in place when their old region still fits); an overflow leaves them on the
    // transient source this frame and grows the arena before the next one.
    struct Copy
    {
        u32 source, destination, count;
    };
    std::vector<Copy> copies;
    std::vector<u32> source(job_count, 0);
    std::vector<u8> from_arena(job_count, 0);
    for (u32 i = 0; i < job_count; ++i)
    {
        SlotRenderItems& list = *jobs[i].list;
        const u32 count = u32(list.size());
        if (!count)
            continue;
        if (transient_at[i] == u32(-1))
        {
            source[i] = list.arena_offset;
            from_arena[i] = 1;
            continue;
        }
        source[i] = transient_at[i];
        if (!settled[i])
            continue;
        list.settled_revision = list.revision;
        u32 offset;
        if (list.arena_epoch == g.epoch && list.arena_capacity >= count)
            offset = list.arena_offset;
        else
        {
            const u32 capacity = count + count / 4 + 4;
            if (capacity > g.arena.capacity - g.arena_used)
            {
                g.arena_wanted = _max(g.arena_wanted, _max(g.arena.capacity + g.arena.capacity / 2, g.arena_used + capacity));
                continue;
            }
            offset = g.arena_used;
            g.arena_used += capacity;
            list.arena_epoch = g.epoch;
            list.arena_offset = offset;
            list.arena_capacity = capacity;
        }
        list.arena_revision = list.revision;
        source[i] = offset;
        from_arena[i] = 1;
        if (!copies.empty() && copies.back().source + copies.back().count == transient_at[i] &&
            copies.back().destination + copies.back().count == offset)
            copies.back().count += count;
        else
            copies.push_back({transient_at[i], offset, count});
    }
    for (const Copy& copy : copies)
    {
        const D3D11_BOX box = {copy.source * u32(sizeof(Packed)), 0, 0, (copy.source + copy.count) * u32(sizeof(Packed)), 1, 1};
        context->CopySubresourceRegion(g.arena.buffer, 0, copy.destination * u32(sizeof(Packed)), 0, 0, g.transient.buffer, 0, &box);
    }

    // 1 header + the ranges + the thinning parameters.
    const u32 range_entries = range_count + 2;
    if (!g.ranges.buffer || range_entries > g.ranges.capacity)
        if (!CreateBuffer(g.ranges, (_max(range_entries, 1u << 12) + range_count / 4 + 255) & ~255u, 16, true))
            return false;
    const u32 groups = (total + 255) / 256, groups_x = _min(groups, 65535u), groups_y = (groups + groups_x - 1) / groups_x;
    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(context->Map(g.ranges.buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            return false;
        auto* entry = static_cast<u32*>(mapped.pData);
        entry[0] = total;
        entry[1] = range_count;
        entry[2] = groups_x;
        entry[3] = range_count + 1;
        for (u32 i = 0; i < job_count; ++i)
        {
            SlotRenderItems& list = *jobs[i].list;
            const u32 count = u32(list.size());
            if (!count)
                continue;
            entry += 4;
            float distance = list.slot_distance;
            if (!_valid(distance) || distance < 0.f)
                distance = 0.f;
            entry[0] = source[i] | (from_arena[i] ? 0u : 0x80000000u);
            entry[1] = jobs[i].count | (jobs[i].tip << 16); // a slot holds far fewer than 65536 instances
            entry[2] = jobs[i].base;
            entry[3] = Bits(distance);
        }
        const Thin thin = ReadThin();
        entry[4] = Bits(thin.start);
        entry[5] = Bits(thin.inv_range);
        entry[6] = Bits(thin.keep);
        entry[7] = 0;
        context->Unmap(g.ranges.buffer, 0);
    }

    if (g.output_buffer != hw_instanceVB)
    {
        g.ResetOutput();
        D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32_TYPELESS;
        view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        view.Buffer.NumElements = hw_instance_capacity * u32(sizeof(detail_instancing::Instance) / 4);
        view.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        if (FAILED(HW.pDevice->CreateUnorderedAccessView(hw_instanceVB, &view, &g.output_uav)))
            return false;
        g.output_buffer = hw_instanceVB;
    }

    {
        PROF_EVENT("Render.Grass.Gather.Dispatch");
        // The engine's view cache stays authoritative: our compute views go through it and are removed again.
        SRVSManager.SetCSResource(0, g.arena.srv);
        SRVSManager.SetCSResource(1, g.transient.srv);
        SRVSManager.SetCSResource(2, g.ranges.srv);
        SRVSManager.Apply();
        RCache.set_CS(g.shader); // the backend caches the bound compute shader
        const UINT keep_counter = u32(-1);
        context->CSSetUnorderedAccessViews(0, 1, &g.output_uav, &keep_counter);
        context->Dispatch(groups_x, groups_y, 1);
        ID3D11UnorderedAccessView* const none = nullptr;
        context->CSSetUnorderedAccessViews(0, 1, &none, nullptr); // the stream is a vertex buffer from here on
        RCache.set_CS(static_cast<ID3D11ComputeShader*>(nullptr));
        SRVSManager.SetCSResource(0, nullptr);
        SRVSManager.SetCSResource(1, nullptr);
        SRVSManager.SetCSResource(2, nullptr);
        SRVSManager.Apply();
    }
    PROF_TAG("Gather packed records", transient_total);
    PROF_TAG("Gather ranges", range_count);
    return true;
}
