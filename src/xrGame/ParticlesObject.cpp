//----------------------------------------------------
// file: PSObject.cpp
//----------------------------------------------------
#include "stdafx.h"
#include "ParticlesObject.h"
#include "../xrEngine/defines.h"
#include "../Include/xrRender/RenderVisual.h"
#include "../Include/xrRender/ParticleCustom.h"
#include "../xrEngine/render.h"
#include "../xrEngine/IGame_Persistent.h"
#include "../xrEngine/environment.h"
#include "_thread_types.h"

#include "../xrCore/profiler.h"

const Fvector zero_vel = {0.f, 0.f, 0.f};

CParticlesObject::CParticlesObject(LPCSTR p_name, BOOL bAutoRemove, bool destroy_on_game_load) :
	inherited(destroy_on_game_load)
{
	Init(p_name, 0, bAutoRemove);
}

void CParticlesObject::Init(LPCSTR p_name, IRender_Sector* S, BOOL bAutoRemove)
{
    simulation_xform.identity();
    pending_xform.identity();
    simulation_position.set(0, 0, 0);
	m_bLooped = false;
	m_bStopping = false;
	m_bAutoRemove = bAutoRemove;
	float time_limit = 0.0f;

	if (!g_dedicated_server)
	{
		// create visual
		renderable.visual = Render->model_CreateParticles(p_name);
		VERIFY(renderable.visual);
		IParticleCustom* V = renderable.visual ? renderable.visual->dcast_ParticleCustom() : NULL;
		VERIFY(V);
		time_limit = V->GetTimeLimit();
	}
	else
	{
		time_limit = 1.0f;
	}

	if (time_limit > 0.f)
	{
		m_iLifeTime = iFloor(time_limit * 1000.f);
	}
	else
	{
		if (bAutoRemove)
		{
			R_ASSERT3(!m_bAutoRemove, "Can't set auto-remove flag for looped particle system.", p_name);
		}
		else
		{
			m_iLifeTime = 0;
			m_bLooped = true;
		}
	}


	// spatial
	SpatialComponent->spatial.type = 0;
	SpatialComponent->spatial.sector = S;
	dwLastTime = Device.dwTimeGlobal;
    StageRenderState();
}

//----------------------------------------------------
CParticlesObject::~CParticlesObject()
{
}

void CParticlesObject::UpdateSpatial()
{
	if (g_dedicated_server) return;

	// spatial	(+ workaround occasional bug inside particle-system)
	vis_data& vis = renderable.visual->getVisData();
	if (_valid(vis.sphere))
	{
		Fvector P;
		float R;
		renderable.xform.transform_tiny(P, vis.sphere.P);
		R = vis.sphere.R;
		if (0 == SpatialComponent->spatial.type)
		{
			// First 'valid' update - register
			SpatialComponent->spatial.type = STYPE_PARTICLE;
			SpatialComponent->spatial.sphere.set(P, R);
			spatial_register();
		}
		else
		{
			bool bMove = false;
			if (!P.similar(SpatialComponent->spatial.sphere.P, EPS_L * 10.f)) bMove = true;
			if (!fsimilar(R, SpatialComponent->spatial.sphere.R, 0.15f)) bMove = true;
			if (bMove)
			{
				SpatialComponent->spatial.sphere.set(P, R);
				spatial_move();
			}
		}
	}
}

const shared_str CParticlesObject::Name()
{
	if (g_dedicated_server) return "";

	IParticleCustom* V = renderable.visual ? renderable.visual->dcast_ParticleCustom() : NULL;
	VERIFY(V);
	return (V) ? V->Name() : "";
}

intrusive_ptr<CParticlesObject> Particles::Details::Create(LPCSTR p_name, BOOL bAutoRemove, bool remove_on_game_load)
{
	intrusive_ptr<CParticlesObject> Particle = xr_new<CParticlesObject>(p_name, bAutoRemove, remove_on_game_load);
	g_pGamePersistent->ps_active_deffer.push_back(Particle);

	return Particle;
}

void CParticlesObject::Play(bool bHudMode)
{
    xrCriticalSectionGuard guard(&simulation_lock);
	if (g_dedicated_server) return;

	IParticleCustom* V = renderable.visual ? renderable.visual->dcast_ParticleCustom() : NULL;
	VERIFY(V);
	if (bHudMode)
		V->SetHudMode(bHudMode);

	V->Play();
	dwLastTime = Device.dwTimeGlobal - 33ul;

	PerformAllTheWork();
	m_bStopping = false;
}

void CParticlesObject::play_at_pos(const Fvector& pos, BOOL xform)
{
    xrCriticalSectionGuard guard(&simulation_lock);
	if (g_dedicated_server) return;

	IParticleCustom* V = renderable.visual ? renderable.visual->dcast_ParticleCustom() : NULL;
	VERIFY(V);
	Fmatrix m;
	m.translate(pos);
	V->UpdateParent(m, zero_vel, xform);
	V->Play();
	dwLastTime = Device.dwTimeGlobal - 33ul;

	PerformAllTheWork();
	m_bStopping = false;
}

void CParticlesObject::Stop(BOOL bDefferedStop)
{
    xrCriticalSectionGuard guard(&simulation_lock);
	if (g_dedicated_server) return;

	IParticleCustom* V = renderable.visual ? renderable.visual->dcast_ParticleCustom() : NULL;
	VERIFY(V);
	V->Stop(bDefferedStop);
	m_bStopping = true;
}

void CParticlesObject::Update(u32 _dt)
{
    xrCriticalSectionGuard guard(&simulation_lock);
    inherited::shedule_Update(_dt);
}

void CParticlesObject::Simulate(u32 dt)
{
    xrCriticalSectionGuard guard(&simulation_lock);
    if (g_dedicated_server || m_bDead) return;
    auto* visual = renderable.visual ? renderable.visual->dcast_ParticleCustom() : nullptr;
    if (!visual) return;
    visual->OnFrame(dt);
    StageRenderState();
}

void CParticlesObject::StageRenderState()
{
    if (g_dedicated_server || !renderable.visual) return;
    auto* visual = renderable.visual->dcast_ParticleCustom();
    auto state = visual->CaptureRenderState();
    simulation_position.add(state->bounds.min, state->bounds.max).mul(0.5f);
    xrCriticalSectionGuard guard(&snapshot_lock);
    pending_frame = std::move(state);
    pending_xform = simulation_xform;
}

bool CParticlesObject::PublishRenderState()
{
    IParticleCustom::RenderStatePtr state;
    Fmatrix transform;
    {
        xrCriticalSectionGuard guard(&snapshot_lock);
        state.swap(pending_frame);
        transform = pending_xform;
    }
    if (!state || m_bDead) return false;
    renderable.visual->dcast_ParticleCustom()->PublishRenderState(state);
    renderable.xform = transform;
    UpdateSpatial();
    return true;
}

void CParticlesObject::PerformAllTheWork()
{
	xrCriticalSectionGuard guard(&simulation_lock);
	if (g_dedicated_server) return;

	// Update
	StageRenderState();
}

void CParticlesObject::SetXFORM(const Fmatrix& m)
{
    xrCriticalSectionGuard guard(&simulation_lock);
	if (g_dedicated_server) return;

	IParticleCustom* V = renderable.visual ? renderable.visual->dcast_ParticleCustom() : NULL;
	VERIFY(V);
	V->UpdateParent(m, zero_vel,TRUE);
	simulation_xform.set(m);
}

void CParticlesObject::UpdateParent(const Fmatrix& m, const Fvector& vel)
{
    xrCriticalSectionGuard guard(&simulation_lock);
	if (g_dedicated_server) return;

	IParticleCustom* V = renderable.visual ? renderable.visual->dcast_ParticleCustom() : NULL;
	VERIFY(V);
	V->UpdateParent(m, vel,FALSE);
}

Fvector CParticlesObject::Position()
{
    xrCriticalSectionGuard guard(&simulation_lock);
    return simulation_position;
}

float CParticlesObject::shedule_Scale()
{
	if (g_dedicated_server) return 5.0f;

	return Device.vCameraPosition.distance_to(Position()) / 200.f;
}

void CParticlesObject::renderable_Render(IDSGraphManager* DM)
{
    if (!renderable.visual)
        return;

	DM->add_Dynamic(renderable.visual, &renderable.xform);
	renderable.visual->getVisData().hom_frame = Device.dwFrame;
}

bool CParticlesObject::IsAutoRemove()
{
	if (m_bAutoRemove) return true;
	else return false;
}

void CParticlesObject::SetAutoRemove(bool auto_remove)
{
    xrCriticalSectionGuard guard(&simulation_lock);
	VERIFY(m_bStopping || !IsLooped());
	m_bAutoRemove = auto_remove;
}

void CParticlesObject::SetHudMode(bool bHudMode)
{
    xrCriticalSectionGuard guard(&simulation_lock);
	if (g_dedicated_server) return;

	IParticleCustom* V = renderable.visual ? renderable.visual->dcast_ParticleCustom() : NULL;
	VERIFY(V);
	V->SetHudMode(bHudMode);
}

void CParticlesObject::SetLiveUpdate(BOOL b)
{
    xrCriticalSectionGuard guard(&simulation_lock);
	if(g_dedicated_server)		return;

	IParticleCustom* V = renderable.visual ? renderable.visual->dcast_ParticleCustom() : NULL;
	VERIFY(V);
	V->SetLiveUpdate(b);
}

BOOL CParticlesObject::GetLiveUpdate()
{
    xrCriticalSectionGuard guard(&simulation_lock);
	if(g_dedicated_server)		return FALSE;

	IParticleCustom* V = renderable.visual ? renderable.visual->dcast_ParticleCustom() : NULL;
	VERIFY(V);
	return V->GetLiveUpdate();
}

//играются ли партиклы, отличается от PSI_Alive, тем что после
//остановки Stop партиклы могут еще доигрывать анимацию IsPlaying = true
bool CParticlesObject::IsPlaying()
{
    xrCriticalSectionGuard guard(&simulation_lock);
	if (g_dedicated_server) return false;

	IParticleCustom* V = renderable.visual ? renderable.visual->dcast_ParticleCustom() : NULL;
	VERIFY(V);
	return !!V->IsPlaying();
}
