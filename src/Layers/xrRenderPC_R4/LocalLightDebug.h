#pragma once

#include <algorithm>
#include <cmath>

class light;

namespace LocalLightDebug
{
extern int show;
extern bool active;

enum class Reason { Unknown, Inactive, Hud, OccqDisabled, ShadowTestDisabled, UnshadowedTestDisabled, NearCamera, GpuVisible, GpuHidden, GpuError, ViewChanged };
void BeginFrame();
void EndFrame();
void RequestDump();
void Forget(const light* source);
void ParentPassed(light& source);
void Visibility(light& source, Reason reason, float nearLimit = 0.f);
void Cached(light& source);
void QueryIssued(light& source);
void QueryResult(light& source, long result, unsigned long long samples);
void Accepted(light& source);
void ReceiverRejected(light& source);
void Submitted(light& source);

struct ReceiverHistoryStats
{
    unsigned frame, sources, tests, spots, ready, pending, full, age, hidden, reset;
};
void ReceiverFrame(const ReceiverHistoryStats& stats);

struct Marker
{
    float x = 0.f, y = 0.f;
    bool valid = false, outside = false, behind = false;
};

// Native screen coordinates; keep offscreen/behind-camera lights at the edge.
inline Marker Project(float x, float y, float w, float width, float height)
{
    Marker result;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w) ||
        !std::isfinite(width) || !std::isfinite(height) || width < 64.f || height < 64.f)
        return result;
    result.behind = w <= 0.f;
    const float divisor = std::max(std::abs(w), 0.0001f);
    result.outside = result.behind || std::abs(x) > divisor || std::abs(y) > divisor;
    if (result.outside)
    {
        const float scale = std::max(std::abs(x), std::abs(y));
        if (scale > 0.f) { x /= scale; y /= scale; }
        else y = -1.f;
    }
    else { x /= divisor; y /= divisor; }
    result.x = std::clamp((x + 1.f) * width * 0.5f, 16.f, width - 16.f);
    result.y = std::clamp((1.f - y) * height * 0.5f, 32.f, height - 16.f);
    result.valid = true;
    return result;
}
}
