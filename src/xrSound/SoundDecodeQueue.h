#pragma once

#include "SoundRender_Source.h"
#include <exception>
#include <memory>
#include <string>
#include <vector>

class CSoundRender_Core;
struct SoundSourceRequest
{
    CSoundRender_Core* core = nullptr; // Kept alive by the explicit streaming barrier.
    std::string name;
    CSoundRender_Source placeholder;
    CSoundRender_Source* source = nullptr;
    std::exception_ptr failure;
    bool published = false; // Frontend owner only.
    bool complete = false, cancelled = false; // Loader queue mutex.
};

void SoundSourceEnqueue(const std::shared_ptr<SoundSourceRequest>& request);
void SoundSourceWait(const std::shared_ptr<SoundSourceRequest>& request);
std::vector<std::shared_ptr<SoundSourceRequest>> SoundSourceTakeReady();

// The sound owner requests/publishes cache lines. The single reusable decoder
// owns file/codec work and copied metadata, never emitters, Lua or game objects.
bool SoundDecodeRequest(CSoundRender_Source* source, u32 line);
void SoundDecodeStart();
void SoundDecodePublish();
void SoundDecodeWait();
void SoundDecodeShutdown();
void SoundDecodeForget(CSoundRender_Source* source);
