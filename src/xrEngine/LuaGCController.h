#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <Windows.h>

#pragma pack(push, 8)

enum class LuaGCStopReason : unsigned
{
	Open, End, LuaRequest, FrameBarrier, Shutdown, NoWork, CycleComplete, Emergency, PressureSlice
};

// The dispatch gate and the per-step gate must agree even if End/Lua closes
// the window between them. Shutdown always wins; a closed ordinary window
// permits at most the first missing step, not an extra catch-up cycle.
inline bool LuaGCStepAllowed(LuaGCStopReason reason, unsigned completed_steps,
	bool require_first_step, bool emergency)
{
	return reason != LuaGCStopReason::Shutdown &&
		(emergency || reason == LuaGCStopReason::Open || (require_first_step && completed_steps == 0));
}

class LuaGCWindow
{
	// Preserve the first stop reason for diagnostics, but atomically remember
	// a later terminal stop too. Otherwise LuaRequest -> End could be reopened.
	static constexpr unsigned terminal = 0x100;
	std::atomic<unsigned> state{unsigned(LuaGCStopReason::FrameBarrier) | terminal};
public:
	void Open() { state.store(unsigned(LuaGCStopReason::Open)); }
	// Only the joined main owner may reopen an early Lua handoff. A frame-end
	// or lifetime stop must never be undone by a later render callback.
	bool ResumeAfterLua()
	{
		auto expected = unsigned(LuaGCStopReason::LuaRequest);
		return state.compare_exchange_strong(expected, unsigned(LuaGCStopReason::Open));
	}
	void Stop(LuaGCStopReason value)
	{
		if (value == LuaGCStopReason::Open) return;
		if (value == LuaGCStopReason::Shutdown)
			state.store(unsigned(value) | terminal);
		else
		{
			auto previous = state.load();
			unsigned next;
			do
			{
				next = (previous & ~terminal) == unsigned(LuaGCStopReason::Open)
					? unsigned(value) : previous;
				if (value != LuaGCStopReason::LuaRequest) next |= terminal;
			} while (!state.compare_exchange_weak(previous, next));
		}
	}
	LuaGCStopReason Reason() const { return LuaGCStopReason(state.load() & ~terminal); }
	bool IsOpen() const { return Reason() == LuaGCStopReason::Open; }
};

struct LuaGCInfo
{
	const void* vm = nullptr;
	std::size_t heap_bytes = 0;
	std::size_t next_cycle_bytes = 0;
	std::uint32_t cycles = 0;
	int phase = 0;
};

// Main-owner diagnostics/recovery policy, independent of collector activation.
// The intervals below limit diagnostics/retries, not GC time or steps per frame.
class LuaGCHealthPolicy
{
	unsigned missing_frames = 0, unavailable_frames = 0;
	unsigned no_step_frames = 0, cycle_frames = 0;
	const void* vm = nullptr;
	std::uint32_t cycles = 0;
	std::size_t reference_heap = 0;
	bool no_step_reported = false, cycle_reported = false;
	bool throughput_starved = false;
	static void Advance(unsigned& value) { if (value != ~0u) ++value; }
public:
	enum Warning : unsigned { None = 0, NoSteps = 1, LongCycle = 2 };
	void Reset() { *this = LuaGCHealthPolicy(); }
	bool BindingRepairDue(bool expected, unsigned missing_mask)
	{
		if (!expected || !missing_mask) { missing_frames = 0; return false; }
		Advance(missing_frames);
		return missing_frames == 1 || missing_frames % 120 == 0;
	}
	bool AvailabilityWarning(bool expected, bool available)
	{
		if (!expected) { Reset(); return false; }
		if (available) { unavailable_frames = 0; return false; }
		Advance(unavailable_frames);
		return unavailable_frames == 1 || unavailable_frames % 1200 == 0;
	}
	unsigned ObserveProgress(const LuaGCInfo& info, unsigned steps)
	{
		if (!info.vm) return None;
		if (vm != info.vm || cycles != info.cycles)
		{
			vm = info.vm;
			cycles = info.cycles;
			reference_heap = info.heap_bytes;
			cycle_frames = no_step_frames = 0;
			no_step_reported = cycle_reported = false;
		}
		if (info.heap_bytes < reference_heap) reference_heap = info.heap_bytes;
		Advance(cycle_frames);
		if (steps) { no_step_frames = 0; no_step_reported = false; }
		else Advance(no_step_frames);
		unsigned warning = None;
		if (no_step_frames >= 120 && !no_step_reported)
		{
			no_step_reported = true;
			warning |= NoSteps;
		}
		const std::size_t min_growth = 64u * 1024u * 1024u;
		const std::size_t threshold = reference_heap / 2 > min_growth ? reference_heap / 2 : min_growth;
		throughput_starved = cycle_frames >= 120 && GrowthBytes(info) >= threshold;
		if (throughput_starved && !cycle_reported)
		{
			cycle_reported = true;
			warning |= LongCycle;
		}
		return warning;
	}
	unsigned UnavailableFrames() const { return unavailable_frames; }
	unsigned NoStepFrames() const { return no_step_frames; }
	unsigned CycleFrames() const { return cycle_frames; }
	bool ThroughputStarved() const { return throughput_starved; }
	std::size_t GrowthBytes(const LuaGCInfo& info) const
	{
		return info.heap_bytes > reference_heap ? info.heap_bytes - reference_heap : 0;
	}
};

// Only the serial Lua owner offers/charges time. A pressure slice shares time
// with the preceding callback; it must not consume the entire render window.
class LuaGCPressurePolicy
{
	const void* vm = nullptr;
	std::uint32_t cycles = 0;
	std::size_t reference_heap = 0;
	double credit_ticks = 0;
	bool active = false;
public:
	void Reset() { *this = LuaGCPressurePolicy(); }
	void Observe(const LuaGCInfo& info)
	{
		if (!info.vm) { Reset(); return; }
		if (vm != info.vm || cycles != info.cycles)
		{
			Reset();
			vm = info.vm;
			cycles = info.cycles;
			reference_heap = info.heap_bytes;
		}
		if (info.heap_bytes < reference_heap) reference_heap = info.heap_bytes;
	}
	bool Offer(const LuaGCInfo& info, std::uint64_t callback_ticks)
	{
		Observe(info);
		if (!vm) return false;
		const std::size_t growth = info.heap_bytes - reference_heap;
		const std::size_t mib = 1024u * 1024u;
		if (growth >= mib) active = true;
		if (!active) return false;
		// Retain pressure until cycle completion. Increasing heap growth buys
		// more work, rather than resetting the baseline after every tiny slice.
		const double scale = growth > mib ? double(growth) / double(mib) : 1.0;
		credit_ticks += double(callback_ticks) * scale;
		return credit_ticks >= 1.0;
	}
	std::uint64_t BudgetTicks() const
	{
		return credit_ticks >= 1.0 ? std::uint64_t(credit_ticks) : 0;
	}
	void Charge(std::uint64_t ticks) { credit_ticks -= double(ticks); }
};

// Mutated by main only after the normal Lua owner has joined. Ordinary frames
// never wait for extra GC. This is a one-shot OOM escape for a starved cycle,
// reset only when that cycle completes or the VM changes. Starved means the cycle
// has been open for many frames while the heap kept growing; taking a step every
// frame is not progress (an inactive window gets one step per frame and was never
// rescued, so the heap grew until the low-address pool ran out).
class LuaGCEmergencyPolicy
{
	const void* vm = nullptr;
	std::uint32_t cycles = 0;
	std::size_t reference_heap = 0;
	unsigned cycle_frames = 0;
	unsigned emergency_count = 0;
	bool emergency_attempted = false;
public:
	void Reset() { *this = LuaGCEmergencyPolicy(); }
	unsigned CycleFrames() const { return cycle_frames; }
	unsigned EmergencyCount() const { return emergency_count; }
	std::size_t GrowthBytes(const LuaGCInfo& info) const
	{
		return info.heap_bytes > reference_heap ? info.heap_bytes - reference_heap : 0;
	}
	bool Observe(const LuaGCInfo& info, unsigned steps)
	{
		if (!info.vm) { Reset(); return false; }
		if (info.vm != vm || info.cycles != cycles)
		{
			const unsigned total_emergencies = info.vm == vm ? emergency_count : 0;
			Reset();
			vm = info.vm;
			cycles = info.cycles;
			reference_heap = info.heap_bytes;
			emergency_count = total_emergencies;
		}
		(void)steps;
		if (cycle_frames != ~0u) ++cycle_frames;
		const std::size_t mib = 1024u * 1024u;
		const std::size_t threshold = reference_heap / 2 > 64u * mib ? reference_heap / 2 : 64u * mib;
		if (emergency_attempted || cycle_frames < 120 || GrowthBytes(info) < threshold)
			return false;
		emergency_attempted = true;
		++emergency_count;
		return true;
	}
};

// Reusable native pool for the exclusive Lua/game-logic owner.
// Must outlive every work handle using it. SDK/Vista details stay in the .cpp.
class NativeWorkerPool
{
	struct State;
	State* state = nullptr;
	unsigned limit;
	bool Initialize(); // Serial owner only, before publishing any work handle.
	friend class LuaGCFrameWorker;
public:
	explicit NativeWorkerPool(unsigned maximum) : limit(maximum) {}
	NativeWorkerPool(const NativeWorkerPool&) = delete;
	NativeWorkerPool& operator=(const NativeWorkerPool&) = delete;
	~NativeWorkerPool();
	unsigned Limit() const { return limit; }
};

// Asynchronous Lua/game-logic owner. Wait cannot execute GC on the waiting
// main thread. One serial caller submits/joins and receives callback exceptions.
class LuaGCFrameWorker
{
	using Function = void (*)(void*);
	PTP_WORK work = nullptr;
	Function function = nullptr;
	void* context = nullptr;
	std::exception_ptr error;
	std::atomic_bool running{false};
	static void CALLBACK Invoke(PTP_CALLBACK_INSTANCE, void* context, PTP_WORK);
public:
	LuaGCFrameWorker() = default;
	LuaGCFrameWorker(const LuaGCFrameWorker&) = delete;
	LuaGCFrameWorker& operator=(const LuaGCFrameWorker&) = delete;
	~LuaGCFrameWorker();
	bool Initialize(Function callback, void* argument, NativeWorkerPool& pool);
	void Start();
	bool Wait();
	void Shutdown();
};

#pragma pack(pop)
