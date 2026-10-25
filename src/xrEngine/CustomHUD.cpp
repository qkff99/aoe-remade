#include "stdafx.h"
#include "CustomHUD.h"

Flags32 psHUD_Flags = {
	HUD_CROSSHAIR_RT | HUD_WEAPON_RT | HUD_WEAPON_RT2 | HUD_CROSSHAIR_DYNAMIC | HUD_CROSSHAIR_RT2 | HUD_DRAW_RT |
	HUD_DRAW_RT2
};

ENGINE_API CCustomHUD* g_hud = NULL;
ENGINE_API bool g_hud_trace = false, g_hud_trace_armed = false;
ENGINE_API int g_hud_watch = 0;
static xr_vector<xr_string> g_hud_trace_lines;
static bool g_hud_trace_one_shot = false;
static u32 g_hud_trace_last = u32(-1), g_hud_trace_prints = 0;

ENGINE_API void HudTraceMsg(const char* format, ...)
{
	char line[512];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	if (g_hud_trace_one_shot) Msg("%s", line);
	else if (g_hud_trace_lines.size() < 256) g_hud_trace_lines.emplace_back(line);
}

ENGINE_API void HudTraceBegin(bool one_shot)
{
	g_hud_trace_one_shot = one_shot;
	g_hud_trace_lines.clear();
	g_hud_trace = true;
}

ENGINE_API void HudTraceEnd(u32 queued)
{
	g_hud_trace = false;
	if (g_hud_trace_one_shot) return;
	// The first frame after the command only establishes the baseline.
	if (g_hud_trace_last != u32(-1) && queued != g_hud_trace_last && g_hud_trace_prints++ < 40)
	{
		Msg("[HUD watch] frame %u: HUD queue changed %u -> %u", Device.dwFrame, g_hud_trace_last, queued);
		for (const auto& line : g_hud_trace_lines) Msg("%s", line.c_str());
	}
	g_hud_trace_last = queued;
	g_hud_trace_lines.clear();
}

CCustomHUD::CCustomHUD()
{
	g_hud = this;
	Device.seqResolutionChanged.Add(this);
}

CCustomHUD::~CCustomHUD()
{
	g_hud = NULL;
	Device.seqResolutionChanged.Remove(this);
}
