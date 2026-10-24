#include "StdAfx.h"
#include "light.h"
#include "QueryHelper.h"
#include "../../xrEngine/cl_intersect.h"
#if defined(USE_DX11) && RENDER == R_R4
#include "../xrRenderPC_R4/LocalLightDebug.h"
#include "../xrRenderPC_R4/Dlss/DlssIntegration.h"
#endif

const u32 delay_small_min = 1;
const u32 delay_small_max = 3;
const u32 delay_large_min = 10;
const u32 delay_large_max = 20;
const u32 cullfragments = 4;
#if defined(USE_DX11) && RENDER == R_R4
const u32 hidden_confirmations = 16;
#endif

#if defined(USE_DX11) && RENDER == R_R4
// A still player's camera position still flips between neighbouring floats (~1.5e-5 m at
// 250 m from the origin); a bitwise compare then revokes valid hidden answers on alternate frames.
static bool view_unchanged(const Fmatrix& a, const Fmatrix& b)
{
    const float* x = &a._11;
    const float* y = &b._11;
    for (u32 i = 0; i < 16; ++i)
        if (_abs(x[i] - y[i]) > (i >= 12 && i < 15 ? 1e-3f : 1e-4f))
            return false;
    return true;
}

bool light::vis_query_current() const
{
    // A zero sample count only establishes occlusion for the query's camera
    // and light volume. Jitter and weather far-plane drift do not change its rays.
    return vis.query_view_valid &&
        view_unchanged(vis.query_view, Device.mView) &&
        !memcmp(&vis.query_world, &m_xform, sizeof(Fmatrix)) &&
        _abs(vis.query_parameters.x - Device.mProject._11) < .001f &&
        _abs(vis.query_parameters.y - Device.mProject._22) < .001f &&
        vis.query_parameters.z == float(RenderScreenWidth()) &&
        vis.query_parameters.w == float(RenderScreenHeight()) &&
        vis.query_svp == Device.m_SecondViewport.IsSVPFrame();
}
#endif

void light::vis_prepare()
{
    if (int(indirect_photons) != ps_r2_GI_photons)
        gi_generate();

    //	. test is sheduled for future	= keep old result
    //	. test time comes :)
    //		. camera inside light volume	= visible,	shedule for 'small' interval
    //		. perform testing				= ???,		pending
    if (!flags.bActive)
    {
        vis.visible = false;
        vis.pending = false;
#if defined(USE_DX11) && RENDER == R_R4
        if (LocalLightDebug::active) LocalLightDebug::Visibility(*this, LocalLightDebug::Reason::Inactive);
#endif
        return;
    }
    if (!flags.bOccq || flags.bHudMode)
    {
        vis.visible = true;
        vis.pending = false;
#if defined(USE_DX11) && RENDER == R_R4
        if (LocalLightDebug::active) LocalLightDebug::Visibility(*this,
            flags.bHudMode ? LocalLightDebug::Reason::Hud : LocalLightDebug::Reason::OccqDisabled);
#endif
        return;
    }

    u32 frame = Device.dwFrame;
#if defined(USE_DX11) && RENDER == R_R4
    if (!vis.visible && !vis_query_current())
    {
        // Preserve pending-query ownership; only revoke its old hidden answer.
        vis.visible = true;
        vis.frame2test = frame;
        if (LocalLightDebug::active) LocalLightDebug::Visibility(*this, LocalLightDebug::Reason::ViewChanged);
    }
#endif
    if (frame < vis.frame2test)
    {
#if defined(USE_DX11) && RENDER == R_R4
        if (LocalLightDebug::active) LocalLightDebug::Cached(*this);
#endif
        return;
    }

    float safe_area = VIEWPORT_NEAR;
    {
        float a0 = deg2rad(Device.fFOV * Device.fASPECT * 0.5f);
        float a1 = deg2rad(Device.fFOV * 0.5f);
        float x0 = VIEWPORT_NEAR / _cos(a0);
        float x1 = VIEWPORT_NEAR / _cos(a1);
        float c = _sqrt(x0 * x0 + x1 * x1);
        safe_area = _max(_max(VIEWPORT_NEAR, _max(x0, x1)), c);
    }

    //Msg	("sc[%f,%f,%f]/c[%f,%f,%f] - sr[%f]/r[%f]",VPUSH(spatial.center),VPUSH(position),spatial.radius,range);
    //Msg	("dist:%f, sa:%f",Device.vCameraPosition.distance_to(spatial.center),safe_area);
    bool skiptest = false;
    if (ps_r2_ls_flags.test(R2FLAG_EXP_DONT_TEST_UNSHADOWED) && !flags.bShadow) skiptest = true;
    if (ps_r2_ls_flags.test(R2FLAG_EXP_DONT_TEST_SHADOWED) && flags.bShadow) skiptest = true;
#ifdef USE_DX11
    //if (ps_ssfx_volumetric.x > 0 && flags.bShadow) skiptest = true; // Temp Fix
#endif

    vis.distance = Device.vCameraPosition.distance_to(SpatialComponent->spatial.sphere.P);

    const float near_limit = SpatialComponent->spatial.sphere.R * 1.01f + safe_area + (SpatialComponent->spatial.sphere.R * 0.1f);
    if (skiptest || vis.distance <= near_limit) // small error
    {
        // small error
        vis.visible = true;
        vis.pending = false;
        vis.frame2test = frame + ::Random.randI(delay_small_min, delay_small_max);
#if defined(USE_DX11) && RENDER == R_R4
        if (LocalLightDebug::active) LocalLightDebug::Visibility(*this, skiptest ?
            (flags.bShadow ? LocalLightDebug::Reason::ShadowTestDisabled : LocalLightDebug::Reason::UnshadowedTestDisabled) :
            LocalLightDebug::Reason::NearCamera, near_limit);
#endif
        return;
    }

    // testing
    if (vis.pending)
        return;

    vis.pending = true;
#if defined(USE_DX11) && RENDER == R_R4
    vis.query_view = Device.mView;
    vis.query_world = m_xform;
    vis.query_parameters.set(Device.mProject._11, Device.mProject._22,
        float(RenderScreenWidth()), float(RenderScreenHeight()));
    vis.query_svp = Device.m_SecondViewport.IsSVPFrame();
    vis.query_view_valid = true;
#endif
    RCache.set_xform_world(m_xform);
#if defined(USE_DX11) && RENDER == R_R4
    if (dlss::TemporalScene())
    {
        // Test the light volume on the same raster sample as the scene depth.
        // The native query shader does not use the material jitter wrapper.
        Fmatrix query_projection = RCache.xforms.m_wvp;
        aoe_temporal::Projection(query_projection, false);
        RCache.set_c("m_WVP", query_projection);
    }
#endif
    CHK_DX(BeginQuery(vis.Q));
    //	Hack: Igor. Light is visible if it's frutum is visible. (Only for volumetric)
    //	Hope it won't slow down too much since there's not too much volumetric lights
    //	TODO: sort for performance improvement if this technique hurts
    if ((flags.type == IRender_Light::SPOT) && flags.bShadow && flags.bVolumetric)
        RCache.set_Stencil(FALSE);
    else
        RCache.set_Stencil(TRUE, D3DCMP_LESSEQUAL, 0x01, 0xff, 0x00);
    RImplementation.Target->draw_volume(this);
    CHK_DX(EndQuery(vis.Q));
#if defined(USE_DX11) && RENDER == R_R4
    if (LocalLightDebug::active) LocalLightDebug::QueryIssued(*this);
#endif
}

void light::vis_update()
{
    //	. not pending	->>> return (early out)
    //	. test-result:	visible:
    //		. shedule for 'large' interval
    //	. test-result:	invisible:
    //		. shedule for 'next-frame' interval

#if defined(USE_DX11) && RENDER == R_R4
    const bool current_view = vis_query_current();
    if (flags.bActive && !vis.visible && !current_view)
    {
        vis.visible = true;
        vis.frame2test = Device.dwFrame;
        if (LocalLightDebug::active) LocalLightDebug::Visibility(*this, LocalLightDebug::Reason::ViewChanged);
    }
#endif
    if (!vis.pending) return;

    u32 frame = Device.dwFrame;
    R_occlusion::occq_result fragments = 0;
    HRESULT hr = GetData(vis.Q, &fragments, sizeof(fragments), 0x1 /*D3D11_ASYNC_GETDATA_DONOTFLUSH*/);

    vis.pending = hr != S_OK;
    if (hr != S_OK)
    {
        vis.frame2test = frame + 1;
        if (hr != S_FALSE)
        {
            // Fail-open on timeout/device/query errors to avoid popping.
            vis.visible = true;
            vis.pending = false;
            vis.frame2test = frame + ::Random.randI(delay_small_min, delay_small_max);
        }  
#if defined(USE_DX11) && RENDER == R_R4
        if (LocalLightDebug::active) LocalLightDebug::QueryResult(*this, hr, fragments);
#endif
        return;
    }
    
    vis.visible = (fragments > cullfragments);
    vis.frame2test = vis.visible ? (frame + ::Random.randI(delay_large_min, delay_large_max)) : (frame + 1);
#if defined(USE_DX11) && RENDER == R_R4
    // Jittered depth makes a barely visible volume read above and below the threshold on
    // alternate frames, while a visible light is retested only every 10-20 frames. Hide it
    // after consecutive hidden answers, or the light blinks at that retest rate.
    if (dlss::TemporalScene())
    {
        if (vis.visible) vis.hidden_streak = 0;
        else if (++vis.hidden_streak < hidden_confirmations) vis.visible = true;
    }
    if (!current_view)
    {
        // The GPU may be several frames behind. A late zero from another view
        // must not hide a face that the camera has already revealed.
        vis.visible = true;
        vis.frame2test = frame + 1;
        vis.hidden_streak = 0;
    }
    if (LocalLightDebug::active) LocalLightDebug::QueryResult(*this, hr, fragments);
    if (!current_view && LocalLightDebug::active) LocalLightDebug::Visibility(*this, LocalLightDebug::Reason::ViewChanged);
#endif
}
