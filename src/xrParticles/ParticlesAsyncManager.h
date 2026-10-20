#pragma once
#include "../xrGame/ParticlesObject.h"

class CPS_Instance;

class CParticlesAsync
{
public:
	static void Play();

private:
	void UpdateParticle(intrusive_ptr<CPS_Instance> particle) const;
	static void Start();

public:
	CParticlesAsync();

};
