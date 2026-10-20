#pragma once

// Profiler backends
#define PROFILER_NONE   (0)
#ifndef PROFILER_OPTICK
#   define PROFILER_OPTICK (1)
#endif
#define PROFILER_TRACY  (2)

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
#elif XRCORE_PROFILER == PROFILER_TRACY
#	ifndef TRACY_ENABLE
#		define TRACY_ENABLE 1
#	endif
#	ifndef TRACY_ON_DEMAND
#		define TRACY_ON_DEMAND 1
#	endif
#	ifndef TRACY_CALLSTACK
#		define TRACY_CALLSTACK 10
#	endif
#	ifndef TRACY_FIBERS
#		define TRACY_FIBERS 1
#	endif
#	ifndef TRACY_ONLY_LOCALHOST
#		define TRACY_ONLY_LOCALHOST 1
#	endif
#	include "../3rd party/tracy-0.14.1/public/tracy/Tracy.hpp"
#	include <cstddef>
#	include <cstdint>
#	include <cstdio>
#	include <cstring>
#	include <type_traits>

namespace xr_tracy
{
    inline tracy::ScopedZone*& CurrentZone()
    {
        static thread_local tracy::ScopedZone* zone = nullptr;
        return zone;
    }

    class Zone : public tracy::ScopedZone
    {
    public:
        explicit Zone(const tracy::SourceLocationData* source, bool active = true)
            : tracy::ScopedZone(source, -1, active), previous(CurrentZone()), owns(IsActive())
        {
            if (owns)
                CurrentZone() = this;
        }

        Zone(uint32_t line, const char* file, size_t fileSize, const char* function, size_t functionSize,
             const char* name, size_t nameSize, bool active = true)
            : tracy::ScopedZone(line, file, fileSize, function, functionSize, name, nameSize, 0, -1, active),
              previous(CurrentZone()), owns(IsActive())
        {
            if (owns)
                CurrentZone() = this;
        }

        Zone(const Zone&) = delete;
        Zone& operator=(const Zone&) = delete;

        ~Zone()
        {
            if (owns && CurrentZone() == this)
                CurrentZone() = previous;
        }

    private:
        tracy::ScopedZone* previous;
        bool owns;
    };

    constexpr const char* SelectName(const char* fallback)
    {
        return fallback;
    }

    constexpr const char* SelectName(const char* /*fallback*/, const char* name)
    {
        return name;
    }

    inline const char* SafeName(const char* name)
    {
        return name ? name : "<null>";
    }

    inline void Emit(const char* text, int length, size_t capacity)
    {
        if (length <= 0)
            return;

        tracy::ScopedZone* zone = CurrentZone();
        if (!zone)
            return;

        const size_t size = size_t(length) < capacity ? size_t(length) : capacity - 1;
        zone->Text(text, size);
    }

    inline void Tag(const char* name, const char* value)
    {
        char buffer[512];
        const int length = std::snprintf(buffer, sizeof(buffer), "%s: %s", name, SafeName(value));
        Emit(buffer, length, sizeof(buffer));
    }

    inline void Tag(const char* name, bool value)
    {
        char buffer[128];
        const int length = std::snprintf(buffer, sizeof(buffer), "%s: %s", name, value ? "true" : "false");
        Emit(buffer, length, sizeof(buffer));
    }

    template <typename T, typename std::enable_if<std::is_arithmetic<T>::value && !std::is_same<T, bool>::value, int>::type = 0>
    inline void Tag(const char* name, T value)
    {
        char buffer[128];
        int length;
        if (std::is_floating_point<T>::value)
            length = std::snprintf(buffer, sizeof(buffer), "%s: %.4f", name, double(value));
        else if (std::is_signed<T>::value)
            length = std::snprintf(buffer, sizeof(buffer), "%s: %lld", name, (long long)value);
        else
            length = std::snprintf(buffer, sizeof(buffer), "%s: %llu", name, (unsigned long long)value);

        Emit(buffer, length, sizeof(buffer));
    }
}

#	define XR_TRACY_CONCAT_INNER(a, b) a##b
#	define XR_TRACY_CONCAT(a, b) XR_TRACY_CONCAT_INNER(a, b)
#	define XR_TRACY_UNIQUE(prefix) XR_TRACY_CONCAT(prefix, __LINE__)
#	define PROF_THREAD(Name) ::tracy::SetThreadName(Name);
#	define PROF_THREAD_IF(Condition, Name) do { if (Condition) ::tracy::SetThreadName(Name); } while (0);
#	define PROF_START_CAPTURE(...) ((void)0);
#	define PROF_STOP_CAPTURE(...) ((void)0);
#	define PROF_SAVE_CAPTURE(...) ((void)0);
#	define PROF_FRAME(...) FrameMarkNamed(__VA_ARGS__);
#	define PROF_EVENT_DETAIL(Function, ...) \
		static constexpr tracy::SourceLocationData XR_TRACY_UNIQUE(xrTracySourceLocation_) { \
			::xr_tracy::SelectName(Function, ##__VA_ARGS__), Function, TracyFile, (uint32_t)TracyLine, 0 }; \
		::xr_tracy::Zone XR_TRACY_UNIQUE(xrTracyZone_)(&XR_TRACY_UNIQUE(xrTracySourceLocation_));
#	define PROF_EVENT(...) PROF_EVENT_DETAIL(TracyFunction, ##__VA_ARGS__)
#	define PROF_EVENT_DYNAMIC(Name) \
		const char* XR_TRACY_UNIQUE(xrTracyDynamicName_) = ::xr_tracy::SafeName(Name); \
		::xr_tracy::Zone XR_TRACY_UNIQUE(xrTracyDynamicZone_)(uint32_t(TracyLine), TracyFile, sizeof(TracyFile) - 1, \
			TracyFunction, sizeof(TracyFunction) - 1, XR_TRACY_UNIQUE(xrTracyDynamicName_), \
			std::strlen(XR_TRACY_UNIQUE(xrTracyDynamicName_)));
#	define PROF_TAG(Name, Value) ::xr_tracy::Tag(Name, Value);
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
