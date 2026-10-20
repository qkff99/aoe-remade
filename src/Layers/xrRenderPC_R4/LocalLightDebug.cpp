#include "stdafx.h"
#include "LocalLightDebug.h"
#include "../xrRender/light.h"
#include <imgui.h>
#include <map>

namespace LocalLightDebug
{
int show = 0;
bool active = false;

namespace
{
constexpr u32 unknown_frame = u32(-1);
struct Entry
{
    u32 id = 0, hom_frame = unknown_frame, accepted_frame = unknown_frame, submitted_frame = unknown_frame;
    u32 reason_frame = unknown_frame, query_frame = unknown_frame, result_frame = unknown_frame, poll_frame = unknown_frame;
    u32 cached_frame = unknown_frame, next_test = 0;
    u32 first_pending_frame = unknown_frame;
    Reason reason = Reason::Unknown;
    Fvector position{}, sphere_position{};
    float range = 0.f, radius = 0.f, distance = 0.f, near_limit = 0.f;
    unsigned long long samples = 0;
    long query_result = S_OK;
    int face = -1;
    u32 type = 0;
    bool visible = false, pending = false, shadow = false, camera_in_hom_box = false;
    bool samples_valid = false, shared_query = false;
};
xr_unordered_map<const light*, Entry> entries;
bool dump_requested = false;
u32 next_id = 1;
Fmatrix view_projection;
Fvector camera_position;
constexpr u32 receiver_history_size = 256;
ReceiverHistoryStats receiver_history[receiver_history_size]{};
ReceiverHistoryStats last_receiver_delay{}, last_receiver_reset{};
u32 receiver_history_next = 0, receiver_history_count = 0;

void PrintReceiverHistory(const ReceiverHistoryStats& row, u32 last_frame)
{
    Msg("[LREC] frames=%u..%u sources=%u tests=%u spots=%u ready=%u pending=%u full=%u age=%u hidden=%u reset=%u",
        row.frame, last_frame, row.sources, row.tests, row.spots, row.ready, row.pending,
        row.full, row.age, row.hidden, row.reset);
}

void DumpReceiverHistory()
{
    Msg("[LREC] Previous %u frames retained without Optick; pending counts outstanding queries before new submission.", receiver_history_count);
    if (last_receiver_delay.pending || last_receiver_delay.full)
    {
        Msg("[LREC] Last delayed-query frame (retained even if outside the rolling history):");
        PrintReceiverHistory(last_receiver_delay, last_receiver_delay.frame);
    }
    if (last_receiver_reset.reset)
    {
        Msg("[LREC] Last history reset:");
        PrintReceiverHistory(last_receiver_reset, last_receiver_reset.frame);
    }
    for (u32 i = 0; i < receiver_history_count;)
    {
        const auto& first = receiver_history[(receiver_history_next + receiver_history_size - receiver_history_count + i) % receiver_history_size];
        u32 last_frame = first.frame;
        ++i;
        while (i < receiver_history_count)
        {
            auto next = receiver_history[(receiver_history_next + receiver_history_size - receiver_history_count + i) % receiver_history_size];
            const u32 frame = next.frame;
            next.frame = first.frame;
            if (memcmp(&first, &next, sizeof(first))) break;
            last_frame = frame;
            ++i;
        }
        PrintReceiverHistory(first, last_frame);
    }
}

Entry& Touch(light& source)
{
    light& parent = source.m_parent ? *source.m_parent : source;
    auto& root = entries[&parent];
    if (!root.id) root.id = next_id++;
    if (root.hom_frame != Device.dwFrame) root.position = parent.position;
    const u32 id = root.id, hom_frame = root.hom_frame;
    const Fvector position = root.position;
    const bool camera_in_box = root.camera_in_hom_box;
    auto& entry = entries[&source];
    entry.id = id;
    entry.position = position;
    entry.hom_frame = hom_frame;
    entry.camera_in_hom_box = camera_in_box;
    entry.range = parent.range;
    entry.type = parent.flags.type;
    entry.face = source.m_parent ? source.omnipart_num : -1;
    entry.shadow = source.flags.bShadow;
    entry.visible = source.vis.visible;
    entry.pending = source.vis.pending;
    if (!entry.pending) entry.first_pending_frame = unknown_frame;
    else if (entry.first_pending_frame == unknown_frame) entry.first_pending_frame = Device.dwFrame;
    entry.next_test = source.vis.frame2test;
    entry.sphere_position = source.SpatialComponent->spatial.sphere.P;
    entry.radius = source.SpatialComponent->spatial.sphere.R;
    entry.distance = camera_position.distance_to(entry.sphere_position);
    entry.shared_query = source.m_parent && !ps_r2_shadow_omnipart_vischeck;
    if (entry.shared_query)
    {
        entry.reason = root.reason;
        entry.reason_frame = root.reason_frame;
        entry.query_frame = root.query_frame;
        entry.result_frame = root.result_frame;
        entry.poll_frame = root.poll_frame;
        entry.first_pending_frame = root.first_pending_frame;
        entry.cached_frame = root.cached_frame;
        entry.samples = root.samples;
        entry.samples_valid = root.samples_valid;
        entry.query_result = root.query_result;
        entry.near_limit = root.near_limit;
        entry.distance = root.distance;
    }
    return entry;
}

const char* Name(Reason reason)
{
    switch (reason)
    {
    case Reason::Inactive: return "inactive";
    case Reason::Hud: return "hud_bypass";
    case Reason::OccqDisabled: return "occq_disabled_on_light";
    case Reason::ShadowTestDisabled: return "r2_exp_donttest_shad";
    case Reason::UnshadowedTestDisabled: return "unshadowed_test_disabled";
    case Reason::NearCamera: return "near_camera_bypass";
    case Reason::GpuVisible: return "gpu_samples_gt_4";
    case Reason::GpuHidden: return "gpu_samples_le_4";
    case Reason::GpuError: return "gpu_error_fail_open";
    case Reason::ViewChanged: return "query_view_changed_fail_open";
    default: return "not_observed_enable_debug_earlier";
    }
}

int Age(u32 frame) { return frame == unknown_frame ? -1 : int(Device.dwFrame - frame); }
}

void BeginFrame()
{
    active = (show || dump_requested) && !Device.m_SecondViewport.IsSVPFrame();
    if (!show && !dump_requested && !entries.empty()) entries.clear();
    if (!active) return;
    view_projection = Device.mFullTransform;
    camera_position = Device.vCameraPosition;
}

void RequestDump()
{
    dump_requested = true;
    Msg("[LDBG] Dump requested for the next main-view level frame. Use r4_light_debug 1 first to retain query history.");
}

void ReceiverFrame(const ReceiverHistoryStats& stats)
{
    receiver_history[receiver_history_next] = stats;
    receiver_history_next = (receiver_history_next + 1) % receiver_history_size;
    receiver_history_count = _min(receiver_history_count + 1, receiver_history_size);
    if (stats.pending || stats.full) last_receiver_delay = stats;
    if (stats.reset) last_receiver_reset = stats;
}

void Forget(const light* source) { entries.erase(source); }

void ParentPassed(light& source)
{
    auto& entry = Touch(source);
    entry.hom_frame = Device.dwFrame;
    entry.position = source.position;
    if (source.flags.bHudMode) Device.hud_to_world(entry.position);
    Fbox bounds;
    bounds.setb(source.SpatialComponent->spatial.sphere.P, Fvector().set(entry.radius, entry.radius, entry.radius));
    entry.camera_in_hom_box = bounds.contains(camera_position);
}

void Visibility(light& source, Reason reason, float nearLimit)
{
    auto& entry = Touch(source);
    entry.reason = reason;
    entry.reason_frame = Device.dwFrame;
    entry.near_limit = nearLimit;
}

void Cached(light& source) { Touch(source).cached_frame = Device.dwFrame; }
void QueryIssued(light& source) { Touch(source).query_frame = Device.dwFrame; }

void QueryResult(light& source, long result, unsigned long long samples)
{
    auto& entry = Touch(source);
    entry.poll_frame = Device.dwFrame;
    entry.query_result = result;
    if (result == S_FALSE) return;
    entry.reason = result == S_OK ? (source.vis.visible ? Reason::GpuVisible : Reason::GpuHidden) : Reason::GpuError;
    entry.reason_frame = entry.result_frame = Device.dwFrame;
    entry.samples_valid = result == S_OK;
    entry.samples = samples;
}

void Accepted(light& source) { Touch(source).accepted_frame = Device.dwFrame; }
void ReceiverRejected(light& source)
{
    auto& entry = Touch(source);
    entry.accepted_frame = entry.submitted_frame = unknown_frame;
}
void Submitted(light& source) { Touch(source).submitted_frame = Device.dwFrame; }

void EndFrame()
{
    if (!active) return;
    PROF_EVENT("Render.LocalLights.Debug");
    struct Source
    {
        const Entry* entry = nullptr;
        u32 shadows = 0, lights = 0;
        bool pending = false, near_camera = false, bypass = false;
    };
    std::map<u32, Source> sources;
    std::map<std::pair<u32, int>, const Entry*> rows;
    u32 shadow_views = 0, light_views = 0;
    for (const auto& item : entries)
    {
        const auto& entry = item.second;
        const bool shadow = entry.shadow && entry.accepted_frame == Device.dwFrame;
        const bool submitted = entry.submitted_frame == Device.dwFrame;
        if (!shadow && !submitted) continue;
        rows.emplace(std::make_pair(entry.id, entry.face), &entry);
        auto& source = sources[entry.id];
        source.entry = &entry;
        source.shadows += shadow;
        source.lights += submitted;
        source.pending |= entry.pending;
        source.near_camera |= entry.reason == Reason::NearCamera;
        source.bypass |= entry.reason == Reason::OccqDisabled || entry.reason == Reason::ShadowTestDisabled ||
            entry.reason == Reason::UnshadowedTestDisabled || entry.reason == Reason::Hud;
        shadow_views += shadow;
        light_views += submitted;
    }
    ImDrawList* draw = show && ImGui::GetCurrentContext() ? ImGui::GetForegroundDrawList() : nullptr;
    // ImGui's DX backend maps draw coordinates 1:1 to target pixels. In borderless
    // mode its window-sized DisplaySize can exceed the presented backbuffer.
    const ImVec2 size = ImVec2(float(Device.dwWidth), float(Device.dwHeight));
    const ImVec2 origin = draw ? ImGui::GetMainViewport()->Pos : ImVec2(0.f, 0.f);
    if (dump_requested)
        Msg("[LDBG] frame=%u sources=%u shadow_views=%u lighting_views=%u camera=(%.3f,%.3f,%.3f) output=%ux%u client=%ux%u; ages=-1 means history unknown",
            Device.dwFrame, u32(sources.size()), shadow_views, light_views, VPUSH(camera_position),
            Device.dwWidth, Device.dwHeight, Device.clientWidth, Device.clientHeight);
    for (const auto& item : sources)
    {
        const auto& source = item.second;
        const auto& entry = *source.entry;
        Fvector4 world, clip;
        world.set(entry.position.x, entry.position.y, entry.position.z, 1.f);
        // The Fvector overload already divides by w; Project needs raw clip coordinates.
        view_projection.transform(clip, world);
        const auto marker = Project(clip.x, clip.y, clip.w, size.x, size.y);
        const char* screen = !marker.valid ? "invalid" : marker.behind ? "behind" : marker.outside ? "outside" : "inside";
        if (draw && marker.valid)
        {
            const ImU32 color = source.bypass ? IM_COL32(255, 100, 100, 255) : source.pending ? IM_COL32(255, 160, 60, 255) :
                source.near_camera ? IM_COL32(255, 230, 80, 255) : IM_COL32(100, 255, 140, 255);
            const ImVec2 p(origin.x + marker.x, origin.y + marker.y);
            draw->AddRect(ImVec2(p.x - 6.f, p.y - 6.f), ImVec2(p.x + 6.f, p.y + 6.f), color, 0.f, 0, 2.f);
            string128 label;
            xr_sprintf(label, "#%u S%u L%u%s", item.first, source.shadows, source.lights,
                marker.behind ? " behind" : marker.outside ? " edge" : "");
            const ImVec2 label_size = ImGui::CalcTextSize(label);
            const float label_x = marker.x > size.x * 0.5f ? p.x - 9.f - label_size.x : p.x + 9.f;
            draw->AddText(ImVec2(label_x, p.y - 8.f), color, label);
        }
        if (dump_requested)
            Msg("[LDBG] #%u type=%s pos=(%.3f,%.3f,%.3f) range=%.3f screen=%s frustum_HOM_LOD_sectors=%s camera_in_HOM_box=%u shadow_views=%u lighting_views=%u",
                item.first, entry.type == IRender_Light::POINT ? "point" : "spot", VPUSH(entry.position), entry.range, screen,
                entry.hom_frame == Device.dwFrame ? "passed_this_view" : "not_observed", u32(entry.camera_in_hom_box),
                source.shadows, source.lights);
    }
    if (draw)
    {
        string256 label;
        xr_sprintf(label, "Local lights: %u sources | S: %u shadow views | L: %u lighting views",
            u32(sources.size()), shadow_views, light_views);
        draw->AddText(ImVec2(origin.x + 16.f, origin.y + 8.f), IM_COL32_WHITE, label);
    }
    if (dump_requested)
    {
        DumpReceiverHistory();
        static const char* faces[] = {"+X", "-X", "+Y", "-Y", "+Z", "-Z"};
        // Print in stable source/face order, matching overlay IDs.
        for (const auto& row : rows)
        {
            const auto& entry = *row.second;
            string64 samples;
            if (entry.samples_valid) xr_sprintf(samples, "%llu", entry.samples);
            else xr_strcpy(samples, "unknown");
            Msg("[LDBG] #%u face=%s shadow_job=%u lighting=%u engine_visible=%u pending=%u pending_observed_age=%d query=%s last_decision=%s decision_age=%d scheduled_reuse=%u distance=%.3f last_near_limit=%.3f samples=%s result_age=%d query_age=%d poll_age=%d query_hr=0x%08X next_test=%u",
                entry.id, entry.face < 0 ? "whole" : faces[entry.face], u32(entry.shadow && entry.accepted_frame == Device.dwFrame),
                u32(entry.submitted_frame == Device.dwFrame), u32(entry.visible), u32(entry.pending), Age(entry.first_pending_frame),
                entry.shared_query ? "parent" : "own",
                Name(entry.reason), Age(entry.reason_frame), u32(entry.cached_frame == Device.dwFrame), entry.distance, entry.near_limit,
                samples, Age(entry.result_frame), Age(entry.query_frame), Age(entry.poll_frame), unsigned(entry.query_result), entry.next_test);
        }
        Msg("[LDBG] End. GPU samples describe volume visibility, not measured illumination. No extra visibility queries were issued.");
        dump_requested = false;
    }
    active = false;
}
}
