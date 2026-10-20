#include "stdafx.h"
#include "ParticlesAsyncManager.h"

#include "../Include/xrRender/ParticleCustom.h"
#include "../Include/xrRender/RenderVisual.h"
#include "../xrEngine/IGame_Persistent.h"

static CParticlesAsync Instance;

void CParticlesAsync::Play()
{
	if (DevicePtr && !Device.ParticleWorkerCallback)
		Device.ParticleWorkerCallback.bind(&CParticlesAsync::Start);

	if (!psDeviceFlags.test(mtParticles))
	{
		Instance.Start();
	}
}

void CParticlesAsync::Start()
{
	{
		PROF_EVENT("Particle Update");
        for (intrusive_ptr<CPS_Instance>& particle: g_pGamePersistent->ps_active)
        {
            if (particle->m_bDead)
                continue;

            Instance.UpdateParticle(particle);
        }
    }

    PROF_EVENT("Particle Shedule");
    for (intrusive_ptr<CPS_Instance>& particle : g_pGamePersistent->ps_active)
	{
		particle->Update(Device.dwTimeDelta);
	}

}

CParticlesAsync::CParticlesAsync()
{
	if (!DevicePtr)
		return;

	if (g_dedicated_server)
		return;

    Device.ParticleWorkerCallback.bind(&CParticlesAsync::Start);
}

void CParticlesAsync::UpdateParticle(intrusive_ptr<CPS_Instance> particle) const
{
    const u32 dt = Device.dwTimeGlobal - particle->dwLastTime;
    PROF_EVENT("Particle OnFrame");
    particle->Simulate(dt);
    particle->dwLastTime = Device.dwTimeGlobal;
}
