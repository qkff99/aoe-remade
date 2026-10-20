//---------------------------------------------------------------------------
#ifndef ParticleCustomH
#define ParticleCustomH

#include "../../Include/xrRender/ParticleCustom.h"
#include "FBasicVisual.h"

//---------------------------------------------------------------------------
class dxParticleCustom : public dxRender_Visual, public IParticleCustom, public intrusive_base_deferred
{
protected:
    Fbox simulation_bounds;
public:
    dxParticleCustom()
    {
        simulation_bounds.set(Fvector().set(0, 0, 0), Fvector().set(0, 0, 0));
        simulation_bounds.grow(EPS_L);
    }
    Fbox SimulationBounds() const { return simulation_bounds; }
    static bool intrusive_release(dxParticleCustom* object)
    {
        return object->__ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1;
    }
    u32 render_references() const { return __ref_count.load(std::memory_order_acquire); }
	// geometry-format
	ref_geom geom;
public:
	virtual ~dxParticleCustom() { ; }

	virtual IParticleCustom* dcast_ParticleCustom() { return this; }
};

//---------------------------------------------------------------------------
#endif //ParticleCustomH
