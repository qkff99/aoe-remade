////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_schedule_registry.cpp
//	Created 	: 15.01.2003
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife schedule registry
////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "alife_schedule_registry.h"

#include "../xrEngine/device.h"

namespace
{
    bool CanContinueALife()
    {
        return Device.CanContinueGameLogicWork();
    }
}

void CALifeScheduleRegistry::update_object(CSE_ALifeSchedulable* object, bool frame_limited)
{
    const u64 started_at = frame_limited ? CPU::QPC() : 0;
    START_PROFILE("ALife/scheduled/update")
#if XRCORE_PROFILER == PROFILER_OPTICK
        if (Optick::IsActive())
        {
            const CSE_Abstract* base = object->base();
            PROF_TAG("ALife object id", u32(base->ID));
            PROF_TAG("ALife section", base->s_name.c_str());
            PROF_TAG("ALife name", base->name_replace());
        }
#endif
        object->update();
    STOP_PROFILE
    if (frame_limited)
        Device.LuaGCAllocationSafePoint(CPU::QPC() - started_at);
}

bool CALifeScheduleRegistry::update(bool frame_limited)
{
	if (!frame_limited || objects().empty() || !m_objects_per_update)
	{
		m_objects_remaining = 0;
		m_update_in_progress = false;
		if (!objects().empty())
			inherited::update(CUpdatePredicate(this, m_objects_per_update), false);
		return true;
	}

	if (!m_objects_remaining)
		m_objects_remaining = m_objects_per_update;
	bool completed = false;
	const u32 processed = inherited::update(CUpdatePredicate(this, m_objects_remaining, true),
		false, &completed, &CanContinueALife);
	if (completed || processed >= m_objects_remaining)
	{
		m_objects_remaining = 0;
		m_update_in_progress = false;
	}
	else
		m_objects_remaining -= processed;
	PROF_TAG("ALife objects processed", processed);
	PROF_TAG("ALife objects remaining", m_objects_remaining);
	return m_objects_remaining == 0;
}

CALifeScheduleRegistry::~CALifeScheduleRegistry()
{
}

void CALifeScheduleRegistry::add(CSE_ALifeDynamicObject* object)
{
	CSE_ALifeSchedulable* schedulable = smart_cast<CSE_ALifeSchedulable*>(object);
	if (!schedulable)
		return;

	if (!schedulable->need_update(object))
		return;

	inherited::add(object->ID, schedulable);
}

void CALifeScheduleRegistry::remove(CSE_ALifeDynamicObject* object, bool no_assert)
{
	CSE_ALifeSchedulable* schedulable = smart_cast<CSE_ALifeSchedulable*>(object);
	if (!schedulable)
		return;

	inherited::remove(object->ID, no_assert || !schedulable->need_update(object));
}
