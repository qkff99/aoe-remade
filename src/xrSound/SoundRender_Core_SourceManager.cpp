#include "stdafx.h"
#pragma hdrstop

#include "SoundRender_Core.h"
#include "SoundRender_Source.h"
#include "../xrCore/ScopeLock.hpp"

CSoundRender_Source* CSoundRender_Core::i_create_source(LPCSTR name)
{
	xrCriticalSectionGuard guard(&sources_lock);
	// Search
	string256 id;
	xr_strcpy(id, name);
	strlwr(id);
	if (strext(id)) *strext(id) = 0;
	auto it = s_sources.find(id);
	if (it != s_sources.end())
	{
		return it->second;
	}

	// Load a _new one
	CSoundRender_Source* S = xr_new<CSoundRender_Source>();
	const auto prepared = m_prepared_sources.find(id);
	if (prepared != m_prepared_sources.end())
	{
		PROF_EVENT("Sound.PublishSource");
		S->load_prepared(id, prepared->second);
		if (prepared->second.warning == PreparedSoundSource::Warning::InvalidComment)
			Log("! Invalid ogg-comment version, file: ", prepared->second.path.c_str());
		else if (prepared->second.warning == PreparedSoundSource::Warning::MissingComment)
			Log("! Missing ogg-comment, file: ", prepared->second.path.c_str());
#ifdef DEBUG
		if (prepared->second.format.nChannels == 2)
			Msg("stereo sound source [%s]", prepared->second.path.c_str());
#endif
		m_prepared_sources.erase(prepared);
	}
	else
		S->load(id);
	s_sources.insert({id, S});
	return S;
}

void CSoundRender_Core::i_destroy_source(CSoundRender_Source* S)
{
	// No actual destroy at all
}

void CSoundRender_Core::source_prefetch_prepare(const xr_vector<xr_string>& sources)
{
	PROF_EVENT("Sound.PrepareSources");
	m_prepared_sources.clear();
	struct Job
	{
		xr_string id;
		PreparedSoundSource prepared;
		xr_string error;
	};
	xr_vector<Job> jobs;
	xr_unordered_flat_set<xr_string> requested;
	for (const xr_string& name : sources)
	{
		string256 id;
		xr_strcpy(id, name.c_str());
		xr_strlwr(id);
		if (strext(id))
			*strext(id) = 0;
		if (!id[0] || s_sources.find(id) != s_sources.end() || !requested.emplace(id).second)
			continue;

		string_path path;
		strconcat(sizeof(path), path, id, ".ogg");
		if (!FS.exist("$level$", path))
			FS.update_path(path, "$game_sounds$", path);
		// Let the unchanged demand loader report missing or invalid sources.
		if (!FS.exist(path))
			continue;
		Job job;
		job.id = id;
		job.prepared.path = path;
		jobs.push_back(std::move(job));
	}
	PROF_TAG("RequestedSources", static_cast<u32>(jobs.size()));
	xr_parallel_for(0u, static_cast<u32>(jobs.size()), [&](u32 index)
	{
		PROF_THREAD("Startup sound metadata");
		Job& job = jobs[index];
		const xr_string path = job.prepared.path;
		CSoundRender_Source::prepare(path.c_str(), job.prepared, job.error);
	});

	PROF_EVENT("Sound.StageMetadata");
	u32 staged = 0;
	for (Job& job : jobs)
	{
		if (!job.prepared.loaded || !job.error.empty() || s_sources.find(job.id) != s_sources.end())
			continue;
		m_prepared_sources.emplace(job.id, std::move(job.prepared));
		++staged;
	}
	PROF_TAG("StagedSources", staged);
}

void CSoundRender_Core::i_create_all_sources()
{
	PROF_EVENT();
	CTimer T;
	T.Start();

	FS_FileSet flist;
	FS.file_list(flist, "$game_sounds$", FS_ListFiles, "*.ogg");
	const size_t sizeBefore = s_sources.size();

	Lock lock;
	const auto processFile = [&](const FS_File& file)
	{
		string256 id;
		xr_strcpy(id, file.name.c_str());

		xr_strlwr(id);
		if (strext(id))
			*strext(id) = 0;

		{
			ScopeLock scope(&lock);
			const auto it = s_sources.find(id);
			if (it != s_sources.end())
				return;
			UNUSED(scope);
		}

		CSoundRender_Source* S = new CSoundRender_Source();
		S->load(id);

		lock.Enter();
		s_sources.insert({ id, S });
		lock.Leave();
	};

	xr_parallel_foreach(flist.begin(), flist.end(), processFile);

	Msg("Finished creating %d sound sources. Duration: %d ms", s_sources.size() - sizeBefore, T.GetElapsed_ms());
}
