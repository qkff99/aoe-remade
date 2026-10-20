#include "stdafx.h"
#include "device.h"
#include "EngineThreading.h"
#include "IGame_Level.h"
#include "IGame_Persistent.h"
#include "CustomHUD.h"
#include "../xrCore/profiler.h"

extern int ps_framelimiter;
extern BOOL psLuaGCAlways;
extern bool IsMainMenuActive();

namespace
{
    thread_local unsigned game_logic_depth = 0;

    struct GameLogicScope
    {
        GameLogicScope() { ++game_logic_depth; }
        ~GameLogicScope() { --game_logic_depth; }
    };

    unsigned MonitorRate(HMONITOR monitor)
    {
        MONITORINFOEX info{};
        info.cbSize = sizeof(info);
        DEVMODE mode{};
        mode.dmSize = sizeof(mode);
        return GetMonitorInfo(monitor, &info) &&
            EnumDisplaySettings(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1
            ? mode.dmDisplayFrequency : 60;
    }

    const char* GCPhaseName(int phase)
    {
        static const char* names[] = {"GC.Step.Pause", "GC.Step.Mark", "GC.Step.Remark",
            "GC.Step.Separate", "GC.Step.Atomic", "GC.Step.SweepStrings", "GC.Step.Sweep", "GC.Step.Finalize"};
        return phase >= 0 && phase < 8 ? names[phase] : "GC.Step.Unknown";
    }
}

void CRenderDevice::ResetFramePacing()
{
    frame_pacer.Reset();
    frame_pacer_in_frame = false;
    frame_pacer_menu_rate = 0;
}

void CRenderDevice::BeginFramePacing()
{
    const bool menu = IsMainMenuActive();
    const bool paused = Paused();
    if (menu || paused)
    {
        const HMONITOR monitor = MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST);
        if (!frame_pacer_menu_rate || frame_pacer_menu_monitor != monitor)
        {
            frame_pacer_menu_monitor = monitor;
            frame_pacer_menu_rate = MonitorRate(monitor);
        }
    }
    else
        frame_pacer_menu_rate = 0;
    const unsigned context = (menu ? 1u : 0u) | (paused ? 2u : 0u) |
        (b_is_Active ? 4u : 0u) | (psDeviceFlags.test(rsVSync) ? 8u : 0u) |
        (m_SecondViewport.IsSVPActive() ? 16u : 0u);
    frame_pacer.Configure(CPU::QPC(), CPU::qpc_freq,
        FramePacerClock::EffectiveRate(ps_framelimiter, menu || paused, frame_pacer_menu_rate), context);
    frame_pacer_in_frame = true;
}

bool CRenderDevice::HasPresentSlack() const
{
    if (!frame_pacer_in_frame || !frame_pacer.Deadline())
        return false;
    const u64 now = CPU::QPC();
    return frame_pacer.Deadline() > now && frame_pacer.Deadline() - now > CPU::qpc_freq / 5000;
}

void CRenderDevice::BeforePresent()
{
    if (!frame_pacer_in_frame)
        return;
    frame_pacer_wait_start = CPU::QPC();
    frame_pacer_wait_sample = {};
    frame_pacer_target = frame_pacer.Period();
    if (frame_pacer.Deadline())
    {
        PROF_EVENT("FramePacer.Wait");
        frame_pacer_wait_sample = frame_pacer_wait.Until(frame_pacer.Deadline(), CPU::qpc_freq);
    }
    frame_pacer_present_start = CPU::QPC();
    frame_pacer_sample = frame_pacer.Presented(frame_pacer_present_start);
}

void CRenderDevice::AfterPresent(bool accepted)
{
    if (!frame_pacer_in_frame)
        return;
    const u64 end = CPU::QPC();
    const double ms = 1000.0 / double(CPU::qpc_freq);
    PROF_TAG("FramePacer target ms", float(frame_pacer_target * ms));
    PROF_TAG("FramePacer interval ms", float(frame_pacer_sample.interval * ms));
    PROF_TAG("FramePacer wait ms", float((frame_pacer_present_start - frame_pacer_wait_start) * ms));
    PROF_TAG("FramePacer sleep ms", float(frame_pacer_wait_sample.slept * ms));
    PROF_TAG("FramePacer spin ms", float(frame_pacer_wait_sample.spun * ms));
    PROF_TAG("FramePacer late ms", float(frame_pacer_sample.late * ms));
    PROF_TAG("FramePacer present ms", float((end - frame_pacer_present_start) * ms));
    PROF_TAG("FramePacer missed", frame_pacer_sample.rebased ? 1u : 0u);
    if (!accepted)
        ResetFramePacing();
}

bool CRenderDevice::IsGameLogicThread() const
{
    return game_logic_depth != 0;
}

bool CRenderDevice::CanContinueGameLogicWork() const
{
    return isRendering.load(std::memory_order_acquire) && lua_gc_window.IsOpen();
}

bool CRenderDevice::LuaGCAutomaticAllowed() const
{
    if (!IsGameLogicThread() && (!IsMainThread() || game_logic_pending.load(std::memory_order_acquire)))
        return false;
    return lua_gc_always_frame && !lua_gc_working.load(std::memory_order_acquire);
}

void CRenderDevice::WaitForGameLogic()
{
    if (IsGameLogicThread() || GetCurrentThreadId() != frame_main_thread ||
        !game_logic_pending.load(std::memory_order_acquire))
        return;
    PROF_EVENT("Lua.RenderEntry.Wait");
    JoinGameLogic(LuaGCStopReason::LuaRequest);
}

void CRenderDevice::JoinGameLogic(LuaGCStopReason reason)
{
    if (IsGameLogicThread())
        return;
    StopLuaGCWindow(reason);
    if (!game_logic_pending.load(std::memory_order_acquire))
        return;
    PROF_EVENT("GC.Owner.Wait");
    PROF_TAG("GC stop reason", unsigned(reason));
    try { game_logic_worker.Wait(); }
    catch (...)
    {
        lua_gc_window.Stop(LuaGCStopReason::Shutdown);
        game_logic_pending.store(false, std::memory_order_release);
        throw;
    }
    game_logic_pending.store(false, std::memory_order_release);
}

void CRenderDevice::StopLuaGCWindow(LuaGCStopReason reason)
{
    lua_gc_window.Stop(reason);
}

void CRenderDevice::RunGameLogicWork(void* context)
{
    auto& device = *static_cast<CRenderDevice*>(context);
    GameLogicScope scope;
    PROF_THREAD("Lua/Game Logic");
    thread_local bool initialized = false;
    if (!initialized)
    {
        _initialize_cpu_thread();
        initialized = true;
    }
    FPU::m24r();
    if (!device.game_logic_gc_only)
        XRay::Engine::GameThread();
    if (device.lua_gc_always_frame)
    {
        device.JoinLuaGCReaders();
        PROF_EVENT_DYNAMIC(device.game_logic_gc_only ? "GC.RenderContinuation" : "GC.Frame.Initial");
        device.RunLuaGCSteps();
    }
}

void CRenderDevice::StartGameLogic()
{
    // Keep one Lua/game owner independently of the collector toggle. RT and
    // Commander callbacks still need the main-entry and lifetime fences when
    // lua_gc_always is disabled (the same owner arrangement as AOE).
    R_ASSERT2(game_logic_worker.Initialize(&CRenderDevice::RunGameLogicWork, this, native_gc_pool),
        "Cannot create native Lua/game-logic worker");
    game_logic_gc_only = false;
    lua_gc_readers_joined = false;
    lua_gc_window.Open();
    game_logic_pending.store(true, std::memory_order_release);
    try { game_logic_worker.Start(); }
    catch (...)
    {
        lua_gc_window.Stop(LuaGCStopReason::Shutdown);
        game_logic_pending.store(false, std::memory_order_release);
        throw;
    }
}

void CRenderDevice::BeginLuaGCFrame()
{
    R_ASSERT(!game_logic_pending.load(std::memory_order_acquire));
    lua_gc_render_resumed = false;
    LuaGCUsec = 0;
    const bool expected = psLuaGCAlways && !g_dedicated_server && g_pGameLevel && g_pGameLevel->bReady;
    const unsigned missing = (!LuaGC ? 1u : 0u) | (!LuaGCReadInfo ? 2u : 0u) | (!LuaGCPrepare ? 4u : 0u);
    if (lua_gc_health.BindingRepairDue(expected, missing))
    {
        PROF_EVENT("GC.Binding.Recover");
        g_pGameLevel->RestoreLuaGCBindings();
    }
    const bool was_always = lua_gc_always_frame;
    lua_gc_always_frame = expected && LuaGC && LuaGCReadInfo && LuaGCPrepare;
    if (lua_gc_always_frame)
        lua_gc_always_frame = LuaGCReadInfo().vm != nullptr;
    if (lua_gc_health.AvailabilityWarning(expected, lua_gc_always_frame))
        Msg("! [Lua] background GC unavailable for %u ready frames; bindings=%u VM=%u",
            lua_gc_health.UnavailableFrames(), unsigned(bool(LuaGC && LuaGCReadInfo && LuaGCPrepare)),
            unsigned(lua_gc_always_frame));
    if ((lua_gc_always_frame || was_always) && LuaGCPrepare)
        LuaGCPrepare(lua_gc_always_frame);
    lua_gc_window.Stop(LuaGCStopReason::Shutdown);
    if (!lua_gc_always_frame)
    {
        lua_gc_pressure_policy.Reset();
        lua_gc_policy.Reset();
    }
}

void CRenderDevice::ResumeLuaGCForRender()
{
    if (GetCurrentThreadId() != frame_main_thread || !lua_gc_always_frame ||
        lua_gc_render_resumed || !isRendering || game_logic_pending.load(std::memory_order_acquire) ||
        !g_pGameLevel || !g_pGameLevel->bReady ||
        lua_gc_window.Reason() != LuaGCStopReason::LuaRequest)
        return;
    JoinGameLogic(LuaGCStopReason::LuaRequest);
    if (LuaGCDone)
        return;
    if (!lua_gc_window.ResumeAfterLua())
        return;
    PROF_EVENT("GC.RenderContinuation.Submit");
    lua_gc_render_resumed = true;
    game_logic_gc_only = true;
    game_logic_pending.store(true, std::memory_order_release);
    try { game_logic_worker.Start(); }
    catch (...)
    {
        lua_gc_window.Stop(LuaGCStopReason::Shutdown);
        game_logic_pending.store(false, std::memory_order_release);
        throw;
    }
}

void CRenderDevice::JoinLuaGCReaders()
{
    if (lua_gc_readers_joined)
        return;
    // This fork's bones/animation callbacks can still enter Lua. AOE's prepared
    // pose workers cannot. Keep the current parallel jobs, but retire those
    // readers before collecting instead of importing its entire pose pipeline.
    PROF_EVENT("GC.Wait.FrameReaders");
    secondary_tasks.wait();
    lua_gc_readers_joined = true;
}

void CRenderDevice::RunLuaGCSteps()
{
    R_ASSERT(GetCurrentThreadId() != frame_main_thread);
    PROF_EVENT("GC.Always.Window");
    unsigned steps = 0;
    const bool require_first_step = !game_logic_gc_only && LuaGCCount == 0;
    const u64 started = CPU::QPC();
    lua_gc_working.store(true, std::memory_order_release);
    try
    {
        for (;;)
        {
            const auto reason = lua_gc_window.Reason();
            if (!LuaGCStepAllowed(reason, steps, require_first_step, lua_gc_emergency_work))
                break;
            const LuaGCInfo info = LuaGCReadInfo();
            int result;
            {
                PROF_EVENT_DYNAMIC(GCPhaseName(info.phase));
                result = LuaGC();
            }
            if (result < 0)
                break;
            ++steps;
            ++LuaGCCount;
            if (result == 1)
            {
                LuaGCDone = true;
                break;
            }
            if (lua_gc_pressure_work && CPU::QPC() >= lua_gc_pressure_deadline)
                break;
        }
    }
    catch (...)
    {
        lua_gc_working.store(false, std::memory_order_release);
        throw;
    }
    LuaGCUsec += u32(double(CPU::QPC() - started) * 1000000.0 / double(CPU::qpc_freq));
    PROF_TAG("GC steps", steps);
    PROF_TAG("GC stop reason", unsigned(lua_gc_window.Reason()));
    PROF_TAG("GC usec", LuaGCUsec);
    lua_gc_working.store(false, std::memory_order_release);
}

void CRenderDevice::LuaGCAllocationSafePoint(u64 callback_ticks)
{
    if (!lua_gc_always_frame || !IsGameLogicThread() || LuaGCDone ||
        lua_gc_working.load(std::memory_order_acquire) || !lua_gc_window.IsOpen())
        return;
    if (!lua_gc_pressure_policy.Offer(LuaGCReadInfo(), callback_ticks))
        return;
    JoinLuaGCReaders();
    PROF_EVENT("GC.Pressure.SafePoint");
    const u64 start = CPU::QPC();
    lua_gc_pressure_deadline = start + lua_gc_pressure_policy.BudgetTicks();
    lua_gc_pressure_work = true;
    try { RunLuaGCSteps(); }
    catch (...) { lua_gc_pressure_work = false; throw; }
    lua_gc_pressure_work = false;
    lua_gc_pressure_policy.Charge(CPU::QPC() - start);
    lua_gc_pressure_policy.Observe(LuaGCReadInfo());
}

void CRenderDevice::FinishLuaGCFrame()
{
    R_ASSERT(!game_logic_pending.load(std::memory_order_acquire));
    if (!lua_gc_always_frame || !g_pGameLevel || !g_pGameLevel->bReady || !LuaGCReadInfo)
        return;
    const LuaGCInfo info = LuaGCReadInfo();
    const unsigned warning = lua_gc_health.ObserveProgress(info, u32(LuaGCCount));
    if (warning & LuaGCHealthPolicy::NoSteps)
        Msg("! [Lua] background GC made no progress for %u ready frames; heap=%llu KB phase=%d",
            lua_gc_health.NoStepFrames(), (unsigned long long)(info.heap_bytes / 1024), info.phase);
    if (warning & LuaGCHealthPolicy::LongCycle)
        Msg("! [Lua] background GC cycle stalled for %u ready frames; heap growth=%llu KB phase=%d",
            lua_gc_health.CycleFrames(), (unsigned long long)lua_gc_health.GrowthBytes(info) / 1024, info.phase);
    PROF_TAG("GC heap KB", u32(info.heap_bytes / 1024));
    PROF_TAG("GC cycle", info.cycles);
    PROF_TAG("GC render continuation", lua_gc_render_resumed ? 1u : 0u);
    lua_gc_pressure_policy.Observe(info);
    if (lua_gc_policy.Observe(info, u32(LuaGCCount)))
    {
        PROF_EVENT("GC.Emergency.Wait");
        lua_gc_emergency_work = true;
        game_logic_gc_only = true;
        game_logic_pending.store(true, std::memory_order_release);
        try
        {
            game_logic_worker.Start();
            JoinGameLogic();
        }
        catch (...)
        {
            lua_gc_emergency_work = false;
            game_logic_pending.store(false, std::memory_order_release);
            throw;
        }
        lua_gc_emergency_work = false;
    }
}

void CRenderDevice::ResetLuaGCState()
{
    JoinGameLogic(LuaGCStopReason::Shutdown);
    if (lua_gc_always_frame && LuaGCPrepare)
        LuaGCPrepare(false);
    lua_gc_always_frame = false;
    lua_gc_render_resumed = false;
    game_logic_gc_only = false;
    lua_gc_pressure_work = false;
    lua_gc_emergency_work = false;
    lua_gc_health.Reset();
    lua_gc_policy.Reset();
    lua_gc_pressure_policy.Reset();
    if (++lua_gc_generation == 0)
        ++lua_gc_generation;
}

void CRenderDevice::ShutdownLuaGCWorker()
{
    ResetLuaGCState();
    game_logic_worker.Shutdown();
}
