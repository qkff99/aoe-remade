#include "stdafx.h"
#include "SoundAsync.h"
#include "SoundDecodeQueue.h"
#include "SoundRender_CoreA.h"
#include "SoundRender_Emitter.h"
#include "SoundRender_Source.h"
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern float psSoundCull;
xrCriticalSection sound_frontend_lock;
namespace
{
SoundRuntimeSettings CaptureSettings()
{
    SoundRuntimeSettings s;
    s.psSpeedOfSound = ::psSpeedOfSound;
    s.psSoundVEffects = ::psSoundVEffects;
    s.psSoundVFactor = ::psSoundVFactor;
    s.psSoundVMusic = ::psSoundVMusic;
    s.psSoundVMusicFactor = ::psSoundVMusicFactor;
    s.psSoundRolloff = ::psSoundRolloff;
    s.psSoundCull = ::psSoundCull;
    s.psSoundOcclusionScale = ::psSoundOcclusionScale;
    s.snd_efx_environment_change_time = ::snd_efx_environment_change_time;
    s.psReverbDensity = ::psReverbDensity;
    s.psReverbDiffusion = ::psReverbDiffusion;
    s.psReverbGain = ::psReverbGain;
    s.psReverbGainHF = ::psReverbGainHF;
    s.psReverbGainLF = ::psReverbGainLF;
    s.psReverbDecayTime = ::psReverbDecayTime;
    s.psReverbDecayHFRatio = ::psReverbDecayHFRatio;
    s.psReverbDecayLFRatio = ::psReverbDecayLFRatio;
    s.psReverbReflectionsGain = ::psReverbReflectionsGain;
    s.psReverbReflectionsDelay = ::psReverbReflectionsDelay;
    s.psReverbLateReverbGain = ::psReverbLateReverbGain;
    s.psReverbLateReverbDelay = ::psReverbLateReverbDelay;
    s.psReverbEchoTime = ::psReverbEchoTime;
    s.psReverbEchoDepth = ::psReverbEchoDepth;
    s.psReverbModulationTime = ::psReverbModulationTime;
    s.psReverbModulationDepth = ::psReverbModulationDepth;
    s.psReverbAirAbsorptionGainHF = ::psReverbAirAbsorptionGainHF;
    s.psReverbHFReference = ::psReverbHFReference;
    s.psReverbLFReference = ::psReverbLFReference;
    s.psReverbRoomRolloffFactor = ::psReverbRoomRolloffFactor;
    s.delayPower = soundSmoothingParams::distanceBasedDelayPower;
    s.delayMinDistance = soundSmoothingParams::distanceBasedDelayMinDistance;
    s.velocityPower = soundSmoothingParams::power;
    s.velocityAlpha = soundSmoothingParams::getTimeDeltaSmoothing();
    s.reverbOverwrite = reverb_overwrite != FALSE;
    s.decayHFLimit = psReverbDecayHFLimit != FALSE;
    s.efx = psSoundFlags.test(ss_EFX);
    s.maxDistanceMultiplier = psSoundMaxDistanceMultiplier;
    s.maxAIDistanceMultiplier = psSoundMaxAIDistanceMultiplier;
    return s;
}
struct VoiceControl
{
    u64 id = 0;
    u32 restart = 1, metadata = 1;
    CSoundRender_Source* source = nullptr; // Immutable metadata, retained until sound shutdown.
    u32 bytes = 0, flags = 0;
    float length = 0.f, delay = 0.f, priority = 1.f, frequency = 1.f;
    int gameType = 0, pause = 0;
    esound_type type = st_Effect;
    bool stopping = false;
    std::string tails[2];
    std::vector<std::pair<u32, std::string>> appendedTails;
    CSound_params params{false};
};
struct VoiceResult
{
    u64 id;
    u32 restart, time, metadata, tailAck;
    bool playing, rendered;
    CSound_source* source;
    std::string tails[2];
    u32 bytes = 0;
    float length = 0.f;
};
struct HearingResult
{
    u64 id;
    u32 restart;
    float range;
    CSound_params params{false};
};
struct AudioPacket
{
    Fvector position{0.f, 0.f, 0.f}, direction{0.f, 0.f, 1.f}, up{0.f, 1.f, 0.f};
    float volume = 1.f;
    SoundRuntimeSettings settings;
    std::vector<VoiceControl> voices;
};
class VoiceProxy final : public CSound_emitter
{
public:
    ref_sound_data_ptr data; // Gameplay-owner only; never copied to a packet.
    VoiceControl control;
    u32 elapsed = 0;
    u32 nextTail = 0;
    bool rendered = false, stopped = false, retireNext = false;
    bool waitingSource = false, rangeSet = false;
    const CSound_params* eventParams = nullptr;
    explicit VoiceProxy(ref_sound_data_ptr d, u64 id) : data(d)
    {
        control.id = id;
        control.source = static_cast<CSoundRender_Source*>(d->handle);
        control.params.position.set(0.f, 0.f, 0.f);
        control.params.velocity.set(0.f, 0.f, 0.f);
        control.params.curVelocity.set(0.f, 0.f, 0.f);
        control.params.prevVelocity.set(0.f, 0.f, 0.f);
        control.params.accVelocity.set(0.f, 0.f, 0.f);
        const auto* authored = control.source;
        control.params.min_distance = authored->m_fMinDist;
        control.params.max_distance = authored->m_fMaxDist * psSoundMaxDistanceMultiplier;
        control.params.max_ai_distance = authored->m_fMaxAIDist * psSoundMaxAIDistanceMultiplier;
        control.params.base_volume = control.source->m_fBaseVolume;
        control.params.volume = 1.f;
        control.params.freq = 1.f;
        control.params.pitch_variation = .02f * Random.randF(-1.f, 1.f) * soundSmoothingParams::pitchVariationPower;
        Metadata();
    }
    ~VoiceProxy() { if (data->feedback == this) data->feedback = nullptr; }
    void Metadata()
    {
        control.bytes = data->dwBytesTotal;
        control.length = data->fTimeTotal;
        control.gameType = data->g_type;
        control.type = data->s_type;
        for (int i = 0; i != 2; ++i) control.tails[i] = data->fn_attached[i].size() ? data->fn_attached[i].c_str() : "";
        ++control.metadata;
    }
    BOOL is_2D() override { xrCriticalSection::raii frontendGuard(&sound_frontend_lock); return eventParams ? FALSE : !!(control.flags & sm_2D); }
    void switch_to_2D() override { xrCriticalSection::raii frontendGuard(&sound_frontend_lock); control.flags |= sm_2D; control.priority = 100.f; }
    void switch_to_Intro() override { xrCriticalSection::raii frontendGuard(&sound_frontend_lock); control.flags |= sm_Intro; switch_to_2D(); }
    void switch_to_3D() override { xrCriticalSection::raii frontendGuard(&sound_frontend_lock); control.flags &= ~sm_2D; }
    void set_position(const Fvector& p) override
    {
        xrCriticalSection::raii frontendGuard(&sound_frontend_lock);
        if (control.source->channels_num() == 1) control.params.update_position(p);
        else control.params.position.set(0.f, 0.f, 0.f);
    }
    void set_frequency(float f) override
    {
        xrCriticalSection::raii frontendGuard(&sound_frontend_lock);
        VERIFY(_valid(f));
        control.frequency = f;
        control.params.freq = f * (control.length < 10.f ? 1.f + control.params.pitch_variation : 1.f);
    }
    void set_range(float a, float b) override { xrCriticalSection::raii frontendGuard(&sound_frontend_lock); rangeSet = true; control.params.min_distance = a; control.params.max_distance = b; }
    void set_volume(float v) override { xrCriticalSection::raii frontendGuard(&sound_frontend_lock); control.params.volume = _valid(v) ? v : 0.f; }
    void set_priority(float p) override { xrCriticalSection::raii frontendGuard(&sound_frontend_lock); control.priority = p; }
    void stop(BOOL deferred) override
    {
        xrCriticalSection::raii frontendGuard(&sound_frontend_lock);
        control.stopping = true;
        if (!deferred)
        {
            stopped = true;
            if (data->feedback == this) data->feedback = nullptr;
        }
    }
    const CSound_params* get_params() override { xrCriticalSection::raii frontendGuard(&sound_frontend_lock); return eventParams ? eventParams : &control.params; }
    u32 play_time() override { xrCriticalSection::raii frontendGuard(&sound_frontend_lock); return elapsed; }
};
}

// Gameplay proxy state and backend state are disjoint. Only the mailbox crosses
// threads; no lock is held while executing OpenAL, callbacks, file reads or rays.
class AsyncSound final : public CSound_manager_interface
{
    CSoundRender_CoreA* core;
    std::map<u64, std::unique_ptr<VoiceProxy>> front;
    std::vector<std::unique_ptr<VoiceProxy>> retired; // Reaped at main FrameMove after all previous-frame consumers.
    u64 nextId = 0;
    int pauseDepth = 1;
    std::atomic<float> masterVolume{1.f};
    struct PauseCommand { bool pause; int id; u64 lastVoice; };
    std::mutex controlMutex;
    std::vector<PauseCommand> pauseCommands;
    xr_vector<ref_sound_data_ptr> deferredDestroy;
    std::string deviceRequest;
    Fvector listener{0.f, 0.f, 0.f};
    sound_event* handler = nullptr;
    bool asynchronous = true, initialized = false, streamSources = false;
    std::map<std::string, std::shared_ptr<SoundSourceRequest>> sourceRequests;
    std::map<ref_sound_data*, std::shared_ptr<SoundSourceRequest>> pendingSources;
    std::map<ref_sound_data*, std::vector<std::string>> pendingTails;
    struct SourceLength
    {
        float known = 0.f;
        std::vector<std::shared_ptr<SoundSourceRequest>> requests;
    };
    std::map<const ref_sound_data*, SourceLength> sourceLengths;


    std::shared_ptr<SoundSourceRequest> RequestSource(LPCSTR name)
    {
        string_path id;
        xr_strcpy(id, name);
        xr_strlwr(id);
        if (strext(id)) *strext(id) = 0;
        auto found = sourceRequests.find(id);
        if (found != sourceRequests.end()) return found->second;
        auto request = std::make_shared<SoundSourceRequest>();
        request->core = core;
        request->name = id;
        request->placeholder.fname = id;
        sourceRequests.emplace(id, request);
        try { SoundSourceEnqueue(request); }
        catch (...) { sourceRequests.erase(id); throw; }
        return request;
    }
    void SetSource(ref_sound_data& data, CSoundRender_Source* source)
    {
        data.handle = source;
        if (data.g_type == sg_SourceType) data.g_type = source->game_type();
        data.dwBytesTotal = source->bytes_total();
        data.fTimeTotal = source->length_sec();
        for (int slot = 0; slot != 2; ++slot)
            if (data.fn_attached[slot].size())
            {
                // These tails were already prepared before this base was published.
                auto tail = RequestSource(data.fn_attached[slot].c_str());
                if (tail->published)
                {
                    data.dwBytesTotal += tail->source->bytes_total();
                    data.fTimeTotal += tail->source->length_sec();
                }
            }
        if (auto* proxy = static_cast<VoiceProxy*>(data.feedback))
        {
            proxy->control.source = source;
            proxy->waitingSource = false;
            const auto* authored = source;
            
            if (!proxy->rangeSet)
            {
                proxy->control.params.min_distance = authored->m_fMinDist;
                proxy->control.params.max_distance = authored->m_fMaxDist;
            }
            proxy->control.params.max_ai_distance = authored->m_fMaxAIDist;
            proxy->control.params.base_volume = source->m_fBaseVolume;
            if (source->channels_num() == 2)
            {
                proxy->control.params.position.set(0.f, 0.f, 0.f);
                proxy->switch_to_2D();
            }
            proxy->Metadata();
            proxy->set_frequency(proxy->control.frequency);
        }
    }
    void PublishSources()
    {
        PROF_EVENT("Sound.Source.PublishReady");
        auto readySources = SoundSourceTakeReady();
        PROF_TAG("Sound source ready", u32(readySources.size()));
        for (auto& request : readySources)
        {
            if (request->failure) std::rethrow_exception(request->failure);
            request->published = true;
        }
        for (auto it = pendingSources.begin(); it != pendingSources.end();)
            if (it->second->published)
            {
                SetSource(*it->first, it->second->source);
                it = pendingSources.erase(it);
            }
            else ++it;
        for (auto it = pendingTails.begin(); it != pendingTails.end();)
        {
            auto& data = *it->first;
            bool ready = pendingSources.find(&data) == pendingSources.end();
            for (const auto& name : it->second) ready = RequestSource(name.c_str())->published && ready;
            if (!ready) { ++it; continue; }
            for (const auto& name : it->second)
            {
                const auto source = RequestSource(name.c_str())->source;
                const u32 slot = data.fn_attached[0].size() ? 1u : 0u;
                R_ASSERT(!data.fn_attached[slot].size());
                data.fn_attached[slot] = name.c_str();
                data.dwBytesTotal += source->bytes_total();
                data.fTimeTotal += source->length_sec();
            }
            if (auto* proxy = static_cast<VoiceProxy*>(data.feedback)) proxy->Metadata();
            it = pendingTails.erase(it);
        }
    }

    std::mutex mutex;
    std::condition_variable ready, idle;
    std::thread worker;
    bool stopping = false, active = false, pending = false;
    AudioPacket queued;
    AudioPacket staging; // Gameplay owner; swap through queued/worker storage instead of reallocating every frame.
    std::vector<VoiceResult> completed;
    std::vector<VoiceResult> resultScratch; // Backend owner.
    std::vector<VoiceResult> readyResults; // Gameplay owner.
    std::vector<HearingResult> completedHearing, hearingScratch, readyHearing;
    CSound_stats completedStats{};
    u32 completedDeviceRevision = 0, publishedDeviceRevision = 0;
    xr_string completedDeviceName;
    xr_vector<xr_string> completedDeviceNames;
#ifdef DEBUG
    xr_vector<CSound_stats_ext::SItem> completedDetails;
#endif
    std::exception_ptr error;

    struct BackendVoice
    {
        ref_sound sound;
        u32 restart = 0, metadata = 0, tailAck = 0;
    };
    std::map<u64, BackendVoice> backend; // Audio owner only.
    void ApplyControls()
    {
        std::vector<PauseCommand> pauses;
        xr_vector<ref_sound_data_ptr> destroy;
        {
            std::lock_guard<std::mutex> lock(controlMutex);
            pauses.swap(pauseCommands);
            destroy.swap(deferredDestroy);
        }
        for (const auto& command : pauses)
            for (auto& entry : front)
                if (entry.first <= command.lastVoice)
                {
                    auto& pause = entry.second->control.pause;
                    if (command.pause ? pause == 0 : pause == command.id)
                        pause = command.pause ? command.id : 0;
                }
        for (auto& data : destroy)
            if (data->feedback) data->feedback->stop(FALSE);
    }

    void Execute(AudioPacket& packet)
    {
        PROF_EVENT("Sound.Backend.Update");
        core->runtime = packet.settings;
        if (core->bPendingDeviceListRefresh.exchange(false)) core->refresh_devices();
        if (core->bPendingDefaultDeviceSwitch.exchange(false)) core->default_device_changed();
        std::string switchTo;
        {
            std::lock_guard<std::mutex> guard(controlMutex);
            switchTo.swap(deviceRequest);
        }
        if (!switchTo.empty()) core->switch_device(switchTo.c_str());
        core->set_master_volume(packet.volume);
        auto live = packet.voices.begin();
        // The complete snapshot is sorted by monotonic identity. Omitted voices
        // are stopped, never resurrected by an older completion.
        for (auto it = backend.begin(); it != backend.end();)
        {
            while (live != packet.voices.end() && live->id < it->first) ++live;
            if (live == packet.voices.end() || live->id != it->first)
            {
                core->destroy(it->second.sound);
                it = backend.erase(it);
            }
            else ++it;
        }
        for (const auto& c : packet.voices)
        {
            auto& v = backend[c.id];
            if (!v.sound._p)
            {
                v.sound._p = xr_new<ref_sound_data>();
                auto& d = *v.sound._p;
                d.async_voice = c.id;
                d.handle = c.source;
                d.s_type = c.type;
                d.g_type = c.gameType;
                d.dwBytesTotal = c.bytes;
                d.fTimeTotal = c.length;
            }
            if (v.metadata != c.metadata)
            {
                auto& d = *v.sound._p;
                const float extra = c.length - d.fTimeTotal;
                d.dwBytesTotal = c.bytes;
                d.fTimeTotal = c.length;
                if (!v.metadata)
                {
                    for (int i = 0; i != 2; ++i) d.fn_attached[i] = c.tails[i].c_str();
                }
                if (d.feedback) static_cast<CSoundRender_Emitter*>(d.feedback)->fTimeToStop += extra;
                v.metadata = c.metadata;
            }
            for (const auto& tail : c.appendedTails)
            {
                if (tail.first <= v.tailAck) continue;
                core->attach_tail(v.sound, tail.second.c_str());
                v.tailAck = tail.first;
            }
            if (v.restart != c.restart)
            {
                if (v.sound._feedback()) static_cast<CSoundRender_Emitter*>(v.sound._feedback())->rewind();
                else core->i_play(&v.sound, c.flags & sm_Looped, c.delay);
                v.restart = c.restart;
            }
            if (auto* e = static_cast<CSoundRender_Emitter*>(v.sound._feedback()))
            {
                e->p_source.pitch_variation = c.params.pitch_variation;
                if (e->p_source.freq != c.params.freq) e->set_frequency(c.frequency);
                if (!e->p_source.position.similar(c.params.position, 0.f)) e->set_position(c.params.position);
                e->set_range(c.params.min_distance, c.params.max_distance);
                e->p_source.max_ai_distance = c.params.max_ai_distance;
                e->set_volume(c.params.volume);
                e->set_priority(c.priority);
                e->b2D = !!(c.flags & sm_2D);
                e->bIntro = !!(c.flags & sm_Intro);
                e->iPaused = c.pause;
                if (c.stopping) e->stop(TRUE);
            }
        }
        core->update(packet.position, packet.direction, packet.up);
        auto& result = resultScratch;
        result.clear();
        result.reserve(backend.size());
        hearingScratch.clear();
        for (auto& entry : backend)
        {
            auto& v = entry.second;
            auto* e = static_cast<CSoundRender_Emitter*>(v.sound._feedback());
            result.push_back({entry.first, v.restart, e ? e->play_time() : 0, v.metadata, v.tailAck, e != nullptr,
                e && e->target, v.sound._p->handle,
                {v.sound._p->fn_attached[0].size() ? v.sound._p->fn_attached[0].c_str() : "",
                 v.sound._p->fn_attached[1].size() ? v.sound._p->fn_attached[1].c_str() : ""}});
            result.back().bytes = v.sound._p->dwBytesTotal;
            result.back().length = v.sound._p->fTimeTotal;
            if (e && e->pending_ai_range > 0.f)
            {
                hearingScratch.push_back({entry.first, v.restart, e->pending_ai_range, e->p_source});
                if (e->b2D) hearingScratch.back().params.position.add(packet.position);
                e->pending_ai_range = 0.f;
            }
        }
        CSound_stats stats{};
        core->statistic(&stats, nullptr);
        std::lock_guard<std::mutex> guard(mutex);
        // Controls/results may coalesce; gameplay hearing events must keep their order.
        completedHearing.insert(completedHearing.end(), hearingScratch.begin(), hearingScratch.end());
        completed.swap(result);
        completedStats = stats;
        if (completedDeviceRevision != core->DeviceRevision())
        {
            core->CopyDeviceState(completedDeviceName, completedDeviceNames);
            completedDeviceRevision = core->DeviceRevision();
        }
    }
    void Run()
    {
        PROF_THREAD("X-Ray Audio Backend");
        _initialize_cpu_thread();
        AudioPacket packet;
        for (;;)
        {
            {
                std::unique_lock<std::mutex> lock(mutex);
                ready.wait(lock, [this] { return stopping || pending; });
                if (stopping && !pending) break;
                std::swap(packet, queued);
                pending = false;
                active = true;
            }
            try { Execute(packet); }
            catch (...) { std::lock_guard<std::mutex> lock(mutex); error = std::current_exception(); }
            {
                std::lock_guard<std::mutex> lock(mutex);
                active = false;
                idle.notify_all();
            }
        }
    }
    void Wait()
    {
        PROF_EVENT("Sound.Backend.LifetimeWait");
        std::unique_lock<std::mutex> lock(mutex);
        idle.wait(lock, [this] { return !pending && !active; });
        if (error) std::rethrow_exception(error);
    }
    void Poll()
    {
        PROF_EVENT("Sound.Frontend.Publish");
        for (auto& entry : front)
            if (entry.second->retireNext) entry.second->stop(FALSE);
        auto& results = readyResults;
        results.clear();
        readyHearing.clear();
        {
            std::lock_guard<std::mutex> lock(mutex);
            results.swap(completed);
            readyHearing.swap(completedHearing);
            if (error) std::rethrow_exception(error);
        }
        for (const auto& r : results)
        {
            auto it = front.find(r.id);
            if (it == front.end()) continue;
            auto& voice = *it->second;
            if (voice.stopped || voice.control.restart != r.restart) continue;
            voice.elapsed = r.time;
            voice.rendered = r.rendered;
            auto& tails = voice.control.appendedTails;
            tails.erase(std::remove_if(tails.begin(), tails.end(), [&](const auto& t) { return t.first <= r.tailAck; }), tails.end());
            if (voice.control.metadata == r.metadata)
            {
                voice.data->handle = r.source;
                voice.control.source = static_cast<CSoundRender_Source*>(r.source);
                voice.data->dwBytesTotal = voice.control.bytes = r.bytes;
                voice.data->fTimeTotal = voice.control.length = r.length;
                for (int i = 0; i != 2; ++i)
                {
                    voice.data->fn_attached[i] = r.tails[i].c_str();
                    voice.control.tails[i] = r.tails[i];
                }
            }
            // Keep feedback through this owner's SoundEvent_Dispatch. Otherwise a
            // short sound's queued hearing event would be discarded as already dead.
            if (!r.playing) voice.retireNext = true;
        }
        for (const auto& event : readyHearing)
        {
            const auto it = front.find(event.id);
            if (it == front.end()) continue;
            auto& voice = *it->second;
            if (voice.stopped || voice.control.restart != event.restart || !handler || !voice.data->g_object)
                continue;
            struct EventScope
            {
                VoiceProxy& voice;
                EventScope(VoiceProxy& v, const CSound_params& p) : voice(v) { voice.eventParams = &p; }
                ~EventScope() { voice.eventParams = nullptr; }
            } snapshot(voice, event.params);
            handler(voice.data, event.range);
        }
        for (auto it = front.begin(); it != front.end();)
            if (it->second->stopped)
            {
                retired.push_back(std::move(it->second));
                it = front.erase(it);
            }
            else ++it;
    }
    void FlushStopped()
    {
        ApplyControls();
        for (auto& entry : front) entry.second->stop(FALSE);
        AudioPacket packet;
        packet.position = listener;
        packet.settings = CaptureSettings();
        Wait();
        Execute(packet);
        Poll();
    }
    void _initialize(int stage) override
    {
        xrCriticalSection::raii frontendGuard(&sound_frontend_lock);
        core->runtime = CaptureSettings();
        core->_initialize(stage);
        if (stage == 1 && core->bReady)
        {
            initialized = true;
            worker = std::thread([this] { Run(); });
        }
    }
    void _clear() override
    {
        xrCriticalSection::raii frontendGuard(&sound_frontend_lock);
        if (initialized)
        {
            FlushStopped();
            {
                std::lock_guard<std::mutex> guard(mutex);
                stopping = true;
            }
            ready.notify_one();
            worker.join();
            initialized = false;
        }
        backend.clear();
        retired.clear();
        SoundDecodeWait();
        pendingSources.clear();
        pendingTails.clear();
        sourceLengths.clear();
        sourceRequests.clear();
        core->_clear();
    }
    void _create_data(ref_sound_data& d, LPCSTR name, esound_type type, int game) override
    {
        xrCriticalSection::raii frontendGuard(&sound_frontend_lock);
        if (!streamSources) { core->_create_data(d, name, type, game); return; }
        auto request = RequestSource(name);
        d.handle = &request->placeholder;
        d.g_type = game;
        d.s_type = type;
        d.feedback = nullptr;
        d.g_object = nullptr;
        d.g_userdata = nullptr;
        d.dwBytesTotal = 0;
        d.fTimeTotal = 0.f;
        sourceLengths.emplace(&d, SourceLength{0.f, {request}});
        if (request->published) SetSource(d, request->source);
        else pendingSources.emplace(&d, request);
    }
    void _destroy_data(ref_sound_data& d) override
    {
        if (d.async_voice) core->_destroy_data(d);
        else
        {
            xrCriticalSection::raii frontendGuard(&sound_frontend_lock);
            if (d.feedback) d.feedback->stop(FALSE);
            pendingSources.erase(&d);
            pendingTails.erase(&d);
            sourceLengths.erase(&d);
            d.handle = nullptr;
        }
    }
public:
    explicit AsyncSound(CSoundRender_CoreA* backendCore) : core(backendCore) {}
    ~AsyncSound() override { R_ASSERT(!worker.joinable()); }
    BOOL i_locked() override { return FALSE; } // Gameplay state has its original exclusive owner.
    BOOL is_ready() override { return initialized; }
    void publish_device_state(bool allowStreaming) override
    {
        xrCriticalSection::raii frontend(&sound_frontend_lock);
        streamSources = allowStreaming;
        PublishSources();
        retired.clear();
        xr_vector<xr_string> names;
        xr_string name;
        {
            std::lock_guard<std::mutex> guard(mutex);
            if (publishedDeviceRevision == completedDeviceRevision) return;
            names = completedDeviceNames;
            name = completedDeviceName;
            publishedDeviceRevision = completedDeviceRevision;
        }
        PublishSoundDeviceTokens(names);
        snd_device_name = name;
    }
    void set_async(bool enabled) override
    {
        if (asynchronous == enabled) return;
        Wait(); // Explicit runtime mode transition, not a recurring frame barrier.
        asynchronous = enabled;
    }
    void create(ref_sound& s, LPCSTR n, esound_type t, int g) override { xrCriticalSection::raii frontendGuard(&sound_frontend_lock); core->create(s, n, t, g); }
    void clone(ref_sound& s, const ref_sound& f, esound_type type, int game) override
    {
        xrCriticalSection::raii frontendGuard(&sound_frontend_lock);
        if (!core->bPresent || !f._p) return;
        s._p = xr_new<ref_sound_data>();
        s._p->handle = f._p->handle;
        s._p->dwBytesTotal = f._p->dwBytesTotal;
        s._p->fTimeTotal = f._p->fTimeTotal;
        s._p->fn_attached[0] = f._p->fn_attached[0];
        s._p->fn_attached[1] = f._p->fn_attached[1];
        s._p->s_type = type;
        const auto pendingSource = pendingSources.find(f._p._get());
        s._p->g_type = game == sg_SourceType && pendingSource == pendingSources.end() ? s._p->handle->game_type() : game;
        if (pendingSource != pendingSources.end()) pendingSources.emplace(s._p._get(), pendingSource->second);
        const auto tails = pendingTails.find(f._p._get());
        if (tails != pendingTails.end()) pendingTails.emplace(s._p._get(), tails->second);
        const auto length = sourceLengths.find(f._p._get());
        if (length != sourceLengths.end()) sourceLengths.emplace(s._p._get(), length->second);
    }
    void attach_tail(ref_sound& s, LPCSTR name) override
    {
        xrCriticalSection::raii frontendGuard(&sound_frontend_lock);
        if (!core->bPresent || !s._p) return;
        if (!streamSources && !s._feedback() && sourceLengths.find(s._p._get()) == sourceLengths.end())
        {
            core->attach_tail(s, name);
            return;
        }
        const size_t attached = !!s._p->fn_attached[0].size() + !!s._p->fn_attached[1].size();
        auto* proxy = static_cast<VoiceProxy*>(s._feedback());
        const auto pendingTail = pendingTails.find(s._p._get());
        const size_t waiting = pendingTail == pendingTails.end() ? 0 : pendingTail->second.size();
        if (attached + waiting + (proxy ? proxy->control.appendedTails.size() : 0) >= 2) return;
        auto request = RequestSource(name);
        auto length = sourceLengths.find(s._p._get());
        if (length == sourceLengths.end())
            length = sourceLengths.emplace(s._p._get(), SourceLength{s._p->fTimeTotal, {}}).first;
        length->second.requests.push_back(request);
        if (proxy && !proxy->waitingSource && pendingSources.find(s._p._get()) == pendingSources.end())
        {
            // Already playing: append on the audio owner. Its original path
            // preserves the playback cursor and publishes the new totals back.
            proxy->control.appendedTails.emplace_back(++proxy->nextTail, request->name);
            pendingTails.erase(s._p._get());
        }
        else pendingTails[s._p._get()].push_back(request->name);
    }
    void destroy(ref_sound& s) override { xrCriticalSection::raii frontendGuard(&sound_frontend_lock); if (s._feedback()) s._feedback()->stop(FALSE); s._p = nullptr; }
    void destroy_deferred(ref_sound& s) override
    {
        if (!s._p) return;
        std::lock_guard<std::mutex> lock(controlMutex);
        deferredDestroy.emplace_back();
        deferredDestroy.back().swap(s._p);
    }
    void play(ref_sound& s, CObject* o, u32 flags, float delay) override
    {
        xrCriticalSection::raii frontendGuard(&sound_frontend_lock);
        if (!core->bPresent || !s._handle() || (!s._handle()->bytes_total() && pendingSources.find(s._p._get()) == pendingSources.end())) return;
        s._p->g_object = o;
        auto* proxy = static_cast<VoiceProxy*>(s._feedback());
        if (!proxy)
        {
            u64 id;
            {
                std::lock_guard<std::mutex> lock(controlMutex);
                id = ++nextId;
            }
            auto owned = std::make_unique<VoiceProxy>(s._p, id);
            proxy = owned.get();
            proxy->waitingSource = pendingSources.find(s._p._get()) != pendingSources.end();
            front.emplace(id, std::move(owned));
            s._p->feedback = proxy;
            proxy->control.flags = flags;
            proxy->control.delay = delay;
            proxy->set_frequency(1.f);
        }
        else
        {
            ++proxy->control.restart;
            proxy->control.stopping = false;
            proxy->retireNext = false;
            proxy->elapsed = 0;
        }
        if ((flags & sm_2D) || s._handle()->channels_num() == 2) proxy->switch_to_2D();
        if (flags & sm_Intro) proxy->switch_to_Intro();
    }
    void play_at_pos(ref_sound& s, CObject* o, const Fvector& p, u32 f, float d) override
    {
        xrCriticalSection::raii frontendGuard(&sound_frontend_lock);
        play(s, o, f, d);
        if (s._feedback()) s._feedback()->set_position(p);
    }
    void play_no_feedback(ref_sound& s, CObject* o, u32 f, float d, Fvector* p, float* v, float* frequency, Fvector2* range) override
    {
        xrCriticalSection::raii frontendGuard(&sound_frontend_lock);
        if (!s._p) return;
        ref_sound copy;
        clone(copy, s, s._p->s_type, s._p->g_type);
        play(copy, o, f, d);
        if (!copy._feedback()) return;
        if (p) copy._feedback()->set_position(*p);
        if (v) copy._feedback()->set_volume(*v);
        if (frequency) copy._feedback()->set_frequency(*frequency);
        if (range) copy._feedback()->set_range(range->x, range->y);
    }
    int request_pause_emitters(bool pause) override
    {
        std::lock_guard<std::mutex> lock(controlMutex);
        const int next = pauseDepth + (pause ? 1 : -1);
        R_ASSERT(next >= 0);
        const int id = pause ? next : next + 1;
        pauseCommands.push_back({pause, id, nextId});
        pauseDepth = next;
        return next;
    }
    int pause_emitters(bool p) override { return request_pause_emitters(p); }
    void stop_emitters() override { xrCriticalSection::raii frontendGuard(&sound_frontend_lock); if (initialized) FlushStopped(); }
    void wait_for_streaming() override { xrCriticalSection::raii guard(&sound_frontend_lock); streamSources = false; Wait(); core->wait_for_streaming(); PublishSources(); }
    float get_length_sec(const ref_sound_data& data) override
    {
        xrCriticalSection::raii guard(&sound_frontend_lock);
        const auto length = sourceLengths.find(&data);
        if (length == sourceLengths.end()) return data.fTimeTotal;
        float seconds = length->second.known;
        for (const auto& request : length->second.requests)
        {
            // Wait only for this sound's metadata, never for unrelated PCM or
            // the backend frame. Playback publication remains at FrameMove.
            SoundSourceWait(request);
            seconds += request->source->length_sec();
        }
        return seconds;
    }
    void set_master_volume(float v) override { masterVolume = v; }
    const Fvector& listener_position() override { return listener; }
    void object_relcase(CObject* o) override
    {
        xrCriticalSection::raii frontendGuard(&sound_frontend_lock);
        for (auto& e : front) if (e.second->data->g_object == o) { if (e.second->waitingSource || pendingTails.find(e.second->data._get()) != pendingTails.end()) e.second->stop(FALSE); e.second->data->g_object = nullptr; }
#ifdef DEBUG
        std::lock_guard<std::mutex> guard(mutex);
        for (auto& e : completedDetails) if (e.game_object == o) e.game_object = nullptr;
#endif
    }
    void set_handler(sound_event* h) override { xrCriticalSection::raii guard(&sound_frontend_lock); Wait(); core->wait_for_streaming(); PublishSources(); handler = h; }
    void set_geometry_env(IReader* r) override { xrCriticalSection::raii guard(&sound_frontend_lock); Wait(); core->wait_for_streaming(); PublishSources(); core->set_geometry_env(r); }
    void set_geometry_som(IReader* r) override { xrCriticalSection::raii guard(&sound_frontend_lock); Wait(); core->wait_for_streaming(); PublishSources(); core->set_geometry_som(r); }
    void set_geometry_occ(CDB::MODEL* m) override { xrCriticalSection::raii guard(&sound_frontend_lock); Wait(); core->wait_for_streaming(); PublishSources(); core->set_geometry_occ(m); }
    void _restart() override { xrCriticalSection::raii guard(&sound_frontend_lock); Wait(); core->wait_for_streaming(); PublishSources(); core->_restart(); }
    void refresh_devices() override { core->bPendingDeviceListRefresh = true; }
    void default_device_changed() override { core->bPendingDefaultDeviceSwitch = true; }
    void switch_device(LPCSTR name) override
    {
        std::lock_guard<std::mutex> guard(controlMutex);
        deviceRequest = name;
    }
    float get_occlusion_to(const Fvector& a, const Fvector& b, float d) override { return core->get_occlusion_to(a, b, d); }
    float get_occlusion(Fvector& p, float r, Fvector* o) override { xrCriticalSection::raii guard(&sound_frontend_lock); Wait(); return core->get_occlusion(p, r, o); }
    void source_prefetch_prepare(const xr_vector<xr_string>& names) override { xrCriticalSection::raii guard(&sound_frontend_lock); Wait(); core->wait_for_streaming(); PublishSources(); core->source_prefetch_prepare(names); }
    void source_prefetch_finish() override { xrCriticalSection::raii guard(&sound_frontend_lock); Wait(); core->wait_for_streaming(); PublishSources(); core->source_prefetch_finish(); }
    void statistic(CSound_stats* s, CSound_stats_ext* ext) override
    {
        std::lock_guard<std::mutex> guard(mutex);
        if (s) *s = completedStats;
#ifdef DEBUG
        if (ext) ext->items = completedDetails;
#endif
    }
    void SnapshotDebugStatistics()
    {
#ifdef DEBUG
        xr_vector<CSound_stats_ext::SItem> details;
        for (const auto& entry : front)
        {
            const auto& p = *entry.second;
            CSound_stats_ext::SItem item;
            item.name = p.data->handle->file_name();
            item.params = p.control.params;
            item.volume = p.control.params.volume;
            item.type = p.data->s_type;
            item.game_type = p.data->g_type;
            item.game_object = p.data->g_object;
            item._3D = !(p.control.flags & sm_2D);
            item._rendered = p.rendered;
            details.push_back(item);
        }
        std::lock_guard<std::mutex> guard(mutex);
        completedDetails.swap(details);
#endif
    }
    void update(const Fvector& p, const Fvector& d, const Fvector& n) override
    {
        xrCriticalSection::raii frontendGuard(&sound_frontend_lock);
        if (!initialized) return;
        PROF_EVENT("Sound.Frontend.Submit");
        ApplyControls();
        Poll();
        SnapshotDebugStatistics();
        listener = p;
        auto& packet = staging;
        packet.voices.clear();
        packet.position = p; packet.direction = d; packet.up = n;
        packet.volume = masterVolume;
        packet.settings = CaptureSettings();
        packet.voices.reserve(front.size());
        for (const auto& entry : front)
        {
            auto& voice = *entry.second;
            if (voice.waitingSource || pendingTails.find(voice.data._get()) != pendingTails.end())
            {
                if (voice.control.stopping) voice.stop(FALSE);
                continue;
            }
            if (voice.control.source->bytes_total()) packet.voices.push_back(voice.control);
            else voice.stop(FALSE);
        }
        if (!asynchronous) { Execute(packet); return; }
        {
            std::lock_guard<std::mutex> guard(mutex);
            std::swap(queued, packet); // Latest full state replaces an unconsumed frame.
            pending = true;
        }
        ready.notify_one();
    }
};

CSound_manager_interface* CreateAsyncSound(CSoundRender_CoreA* core) { return xr_new<AsyncSound>(core); }
