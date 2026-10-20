#pragma once

// Profiler backends
#define PROFILER_NONE   (0)
#ifndef PROFILER_OPTICK
#   define PROFILER_OPTICK (1)
#endif

// Verified configurations also define XRCORE_PROFILER; keep Optick in Profiled only.
#if defined(USE_VERIFY_IN_RELEASE)
#   undef XRCORE_PROFILER
#endif

// Set active profiler backend
#if !defined(XRCORE_PROFILER)
    #define XRCORE_PROFILER PROFILER_NONE
//  #define XRCORE_PROFILER PROFILER_OPTICK
#endif

// Implement profiler macro interface
#if XRCORE_PROFILER == PROFILER_OPTICK
#	include "../3rd party/optick-git/src/optick.h"

void XRCORE_API xrOptickEnableThreadTags();

struct xrOptickThreadScope
{
    xrOptickThreadScope(const xrOptickThreadScope&) = delete;
    xrOptickThreadScope& operator=(const xrOptickThreadScope&) = delete;

    explicit xrOptickThreadScope(const char* name, bool enabled = true) : registered(enabled)
    {
        if (registered && Depth()++ == 0 && !*Optick::GetEventStorageSlotForCurrentThread())
        {
            owns_registration = Optick::RegisterThread(name);
            xrOptickEnableThreadTags();
        }
    }

    explicit xrOptickThreadScope(const wchar_t* name, bool enabled = true) : registered(enabled)
    {
        if (registered && Depth()++ == 0 && !*Optick::GetEventStorageSlotForCurrentThread())
        {
            owns_registration = Optick::RegisterThread(name);
            xrOptickEnableThreadTags();
        }
    }

    ~xrOptickThreadScope()
    {
        if (!registered || --Depth() != 0 || !owns_registration)
            return;

        *Optick::GetEventStorageSlotForCurrentThread() = nullptr;
        Optick::UnRegisterThread(false);
    }

private:
    // PPL waits can run nested tasks on an already registered thread.
    static unsigned& Depth() { static thread_local unsigned depth = 0; return depth; }
    bool registered;
    bool owns_registration = false;
};

#	define PROF_THREAD(Name) xrOptickThreadScope OPTICK_CONCAT(optickThreadScope_, __LINE__)(Name);
#	define PROF_THREAD_IF(Condition, Name) xrOptickThreadScope OPTICK_CONCAT(optickThreadScope_, __LINE__)(Name, Condition);
#	define PROF_START_CAPTURE(...) OPTICK_START_CAPTURE(__VA_ARGS__)
#	define PROF_STOP_CAPTURE(...) OPTICK_STOP_CAPTURE(__VA_ARGS__)
#	define PROF_SAVE_CAPTURE(...) OPTICK_SAVE_CAPTURE(__VA_ARGS__)
#	define PROF_FRAME(...) OPTICK_FRAME(__VA_ARGS__)
#	define PROF_EVENT(...) OPTICK_EVENT(__VA_ARGS__)
#	define PROF_EVENT_DYNAMIC(Name) OPTICK_EVENT_DYNAMIC(Name)
#   define PROF_TAG(Name, Value) OPTICK_TAG(Name, Value)
#	define START_PROFILE(a) { PROF_EVENT(a)
#	define STOP_PROFILE		}
#else
#	define START_PROFILE(a) {
#	define STOP_PROFILE		}

#	define PROF_THREAD(Name)
#	define PROF_THREAD_IF(Condition, Name)
#	define PROF_START_CAPTURE(...)
#	define PROF_STOP_CAPTURE(...)
#	define PROF_SAVE_CAPTURE(...)
#	define PROF_FRAME(...)
#	define PROF_EVENT(...)
#	define PROF_EVENT_DYNAMIC(...) {};
#   define PROF_TAG(Name, Value)
#endif
