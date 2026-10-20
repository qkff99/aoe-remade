#include "stdafx.h"
#include "SoundDecodeQueue.h"
#include "SoundRender_Core.h"
#include "SoundRender_Source.h"
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <stdexcept>
#include <thread>
#include <vector>

extern int ov_seek_func(void*, s64, int);
extern size_t ov_read_func(void*, size_t, size_t, void*);
extern int ov_close_func(void*);
extern long ov_tell_func(void*);

namespace
{
class DecodeQueue
{
	using Key = std::pair<uintptr_t, u32>;
	struct Request
	{
		CSoundRender_Source* source = nullptr; // PCM identity only; worker never dereferences it.
        std::shared_ptr<SoundSourceRequest> metadata;
		std::string path;
		u32 line, line_size, bytes, channels;
		std::vector<char> pcm;
		bool cancelled = false; // queue mutex
		bool failed = false; // worker -> completed queue publication
		Key key() const { return {reinterpret_cast<uintptr_t>(source), line}; }
	};
	std::mutex mutex;
	std::condition_variable ready, idle, sourceReady;
	std::deque<std::shared_ptr<Request>> queued, completed;
	std::map<Key, std::shared_ptr<Request>> pending;
    std::vector<std::shared_ptr<SoundSourceRequest>> completedSources;
	std::thread worker;
	bool active = false, stopping = false;
	// Memory/in-flight bound, NOT a per-frame work quota. Cache misses retry
	// next frame when saturated; they never block a caller on disk or decoding.
	static constexpr size_t max_pending = 256;

	struct Decoder
	{
		std::string path;
		IReader* reader = nullptr;
		OggVorbis_File stream{};
		bool opened = false;
		~Decoder() { Close(); }
		void Close()
		{
			if (opened) ov_clear(&stream);
			opened = false;
			if (reader) FS.r_close(reader);
			path.clear();
		}
		bool Open(const std::string& name)
		{
			if (opened && path == name) return true;
			Close();
			reader = FS.r_open(name.c_str());
			if (!reader) return false;
			ov_callbacks callbacks = {ov_read_func, ov_seek_func, ov_close_func, ov_tell_func};
			if (ov_open_callbacks(reader, &stream, nullptr, 0, callbacks) < 0) { Close(); return false; }
			opened = true;
			path = name;
			return true;
		}
		bool Decode(Request& request)
		{
			request.pcm.assign(request.line_size, 0);
			if (!Open(request.path)) return false;
			auto* info = ov_info(&stream, -1);
			if (!info || info->rate != 44100 || info->channels != int(request.channels)) return false;
			const u32 offset = request.line * request.line_size;
			const ogg_int64_t sample = offset / (2 * request.channels);
			if (ov_pcm_tell(&stream) != sample && ov_pcm_seek(&stream, sample) < 0) return false;
			const u32 bytes = (std::min)(request.line_size, request.bytes - offset);
			u32 written = 0;
			int section = 0;
			while (written < bytes)
			{
				const long count = ov_read(&stream, request.pcm.data() + written, bytes - written, 0, 2, 1, &section);
				if (count < 0) return false;
				if (!count) break; // EOF remainder stays zero.
				written += u32(count);
			}
			return true;
		}
	};

	void Run()
	{
		PROF_THREAD("X-Ray Sound Decoder");
		_initialize_cpu_thread();
		Decoder decoder;
		for (;;)
		{
			std::shared_ptr<Request> request;
			{
				std::unique_lock<std::mutex> lock(mutex);
				if (queued.empty())
				{
					// Keep active published until the reader is closed. In particular,
					// Forget() may remove the last queued job between iterations.
					active = true;
					lock.unlock();
					decoder.Close(); // No archive reader survives an idle/lifetime barrier.
					lock.lock();
					active = false;
					if (queued.empty()) idle.notify_all();
				}
				ready.wait(lock, [this]() { return stopping || !queued.empty(); });
				if (stopping && queued.empty()) return;
				request = queued.front();
				queued.pop_front();
				active = true;
				if (request->cancelled) request.reset();
			}
			if (request && request->metadata)
            {
                auto& metadata = *request->metadata;
                PROF_EVENT("Sound.Source.Worker");
                PROF_TAG("Sound source", metadata.name.c_str());
                try { metadata.source = metadata.core->i_create_source(metadata.name.c_str()); }
                catch (...) { metadata.failure = std::current_exception(); }
                std::lock_guard<std::mutex> lock(mutex);
                metadata.complete = true;
                completedSources.push_back(std::move(request->metadata));
                sourceReady.notify_all();
                request.reset();
            }
            if (request)
            {
                PROF_EVENT("Sound.Decode.Worker");
				PROF_TAG("Sound decode path", request->path.c_str());
				PROF_TAG("Sound decode line", request->line);
				try { request->failed = !decoder.Decode(*request); }
				catch (...) { request->failed = true; }
			}
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (request && !request->cancelled) completed.push_back(std::move(request));
			}
		}
	}

public:
	~DecodeQueue() { Shutdown(); }
	void Start()
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (!worker.joinable()) worker = std::thread([this]() { Run(); });
	}
    void RequestSource(const std::shared_ptr<SoundSourceRequest>& source)
    {
        auto request = std::make_shared<Request>();
        request->metadata = source;
        std::lock_guard<std::mutex> lock(mutex);
        R_ASSERT(!stopping);
        if (!worker.joinable()) worker = std::thread([this]() { Run(); });
        queued.push_back(std::move(request));
        ready.notify_one();
    }
    void WaitSource(const std::shared_ptr<SoundSourceRequest>& source)
    {
        PROF_EVENT("Sound.Source.LengthWait");
        std::unique_lock<std::mutex> lock(mutex);
        sourceReady.wait(lock, [&source]() { return source->complete; });
        if (source->cancelled) throw std::runtime_error("Sound source loading stopped");
        if (source->failure) std::rethrow_exception(source->failure);
    }
    std::vector<std::shared_ptr<SoundSourceRequest>> TakeSources()
    {
        std::vector<std::shared_ptr<SoundSourceRequest>> results;
        std::lock_guard<std::mutex> lock(mutex);
        results.swap(completedSources);
        return results;
    }
    bool RequestLine(CSoundRender_Source* source, u32 line)
	{
		auto& cache = SoundRender->cache;
		if (!source->CAT.size || !source->m_wformat.nChannels) return false;
		line %= source->CAT.size;
		if (source->CAT.table[line] != CAT_FREE) return true;
		const Key key{reinterpret_cast<uintptr_t>(source), line};
		std::lock_guard<std::mutex> lock(mutex);
		if (stopping || pending.find(key) != pending.end() || pending.size() >= max_pending) return false;
		if (!worker.joinable()) worker = std::thread([this]() { Run(); });
		auto request = std::make_shared<Request>();
		request->source = source;
		request->path = source->pname.c_str();
		request->line = line;
		request->line_size = cache.get_linesize();
		request->bytes = source->dwBytesTotal;
		request->channels = source->m_wformat.nChannels;
		queued.push_back(request);
		try { pending.emplace(key, request); }
		catch (...) { queued.pop_back(); throw; }
		ready.notify_one();
		return false;
	}
	void Publish()
	{
		PROF_EVENT("Sound.Decode.PublishReady");
		std::deque<std::shared_ptr<Request>> results;
		u32 in_flight;
		{
			std::lock_guard<std::mutex> lock(mutex);
			results.swap(completed);
			for (const auto& result : results) pending.erase(result->key());
			in_flight = u32(pending.size());
		}
		PROF_TAG("Sound decode pending lines", in_flight);
		PROF_TAG("Sound decode ready lines", u32(results.size()));
		for (const auto& result : results)
		{
			if (result->cancelled) continue;
			auto& cache = SoundRender->cache;
			auto& cat = result->source->CAT;
			if (!cat.size || cache.get_linesize() != result->line_size) continue;
			if (cache.request(cat, result->line))
			{
				void* destination = cache.get_dataptr(cat, result->line);
				if (!result->failed && result->pcm.size() == result->line_size)
					CopyMemory(destination, result->pcm.data(), result->line_size);
				else
				{
					ZeroMemory(destination, result->line_size);
					Msg("! Sound decode failed: %s (line %u)", result->path.c_str(), result->line);
				}
			}
		}
	}
	void Forget(CSoundRender_Source* source)
	{
		std::lock_guard<std::mutex> lock(mutex);
		for (auto it = pending.begin(); it != pending.end();)
			if (it->second->source == source)
			{
				it->second->cancelled = true;
				it = pending.erase(it);
			}
			else ++it;
		const auto belongs = [source](const std::shared_ptr<Request>& request) { return request->source == source; };
		queued.erase(std::remove_if(queued.begin(), queued.end(), belongs), queued.end());
		completed.erase(std::remove_if(completed.begin(), completed.end(), belongs), completed.end());
		if (queued.empty() && !active) idle.notify_all();
	}
	void Wait()
	{
		PROF_EVENT("Sound.Decode.LifetimeWait");
		std::unique_lock<std::mutex> lock(mutex);
		idle.wait(lock, [this]() { return queued.empty() && !active; });
	}
	void Shutdown()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			stopping = true;
            for (auto& request : queued)
                if (request->metadata)
                {
                    request->metadata->cancelled = true;
                    request->metadata->complete = true;
                }
            sourceReady.notify_all();
			for (auto& item : pending) item.second->cancelled = true;
			queued.clear();
		}
		ready.notify_all();
		if (worker.joinable()) worker.join();
		std::lock_guard<std::mutex> lock(mutex);
		completed.clear();
        completedSources.clear();
		pending.clear();
		active = stopping = false;
	}
} decoderQueue;
}

bool SoundDecodeRequest(CSoundRender_Source* source, u32 line) { return decoderQueue.RequestLine(source, line); }
void SoundDecodeStart() { decoderQueue.Start(); }
void SoundDecodePublish() { decoderQueue.Publish(); }
void SoundDecodeWait() { decoderQueue.Wait(); }
void SoundDecodeShutdown() { decoderQueue.Shutdown(); }
void SoundDecodeForget(CSoundRender_Source* source) { decoderQueue.Forget(source); }

void SoundSourceEnqueue(const std::shared_ptr<SoundSourceRequest>& request) { decoderQueue.RequestSource(request); }
std::vector<std::shared_ptr<SoundSourceRequest>> SoundSourceTakeReady() { return decoderQueue.TakeSources(); }

void SoundSourceWait(const std::shared_ptr<SoundSourceRequest>& request) { decoderQueue.WaitSource(request); }
