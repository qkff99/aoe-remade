#pragma once

ENGINE_API extern Flags32 psHUD_Flags;
// r4_hud_dump: one frame of log lines explaining why HUD models are or are not drawn.
ENGINE_API extern bool g_hud_trace, g_hud_trace_armed;
// r4_hud_watch: trace every frame, print only the frames whose HUD queue differs from the previous frame.
ENGINE_API extern int g_hud_watch;
ENGINE_API void HudTraceMsg(const char* format, ...);
ENGINE_API void HudTraceBegin(bool one_shot);
ENGINE_API void HudTraceEnd(u32 queued);
#define HUD_CROSSHAIR (1<<0)
#define HUD_CROSSHAIR_DIST (1<<1)
#define HUD_WEAPON (1<<2)
#define HUD_INFO (1<<3)
#define HUD_DRAW (1<<4)
#define HUD_CROSSHAIR_RT (1<<5)
#define HUD_WEAPON_RT (1<<6)
#define HUD_CROSSHAIR_DYNAMIC (1<<7)
#define HUD_CROSSHAIR_RT2 (1<<9)
#define HUD_DRAW_RT (1<<10)
#define HUD_WEAPON_RT2 (1<<11)
#define HUD_DRAW_RT2 (1<<12)

class ENGINE_API IRender_Visual;
class CUI;
class IDSGraphManager;

class ENGINE_API CCustomHUD :
	public DLL_Pure,
	public IEventReceiver,
	public pureScreenResolutionChanged
{
public:
	CCustomHUD();
	virtual ~CCustomHUD();

	BENCH_SEC_SCRAMBLEVTBL2

	virtual void Render_First(IDSGraphManager* DM) { ; }
	virtual void Render_Last(IDSGraphManager* DM) { ; }
	BENCH_SEC_SCRAMBLEVTBL1

	virtual		void		OnFrame					(){;}
	virtual		void		OnFrameMT				(){;}
	virtual		void		OnEvent					(EVENT E, u64 P1, u64 P2){;}

	virtual void Load() { ; }
	virtual void OnDisconnected() = 0;
	virtual void OnConnected() = 0;
	virtual void RenderActiveItemUI() = 0;
	virtual void RenderCamAttachedUI() = 0;
	virtual bool RenderActiveItemUIQuery() = 0;
	virtual bool RenderCamAttachedUIQuery() = 0;
	virtual void Render_R1_Attachment_UI() = 0;
	virtual void net_Relcase(CObject* object) = 0;
};

extern ENGINE_API CCustomHUD* g_hud;
