#include "stdafx.h"
#include "DlssIntegration.h"
#include "DlssD3D11.h"
#include "Fsr3D3D11.h"
#include "TemporalShaders.h"
#include "../../../xrEngine/igame_persistent.h"
#include "../../../xrEngine/RendererError.h"

namespace
{
dlss::D3D11Backend backend;
fsr3::D3D11Backend fsrBackend;
bool useFsr = false;
Microsoft::WRL::ComPtr<ID3D11Texture2D> sceneDepth;
Microsoft::WRL::ComPtr<ID3D11DepthStencilView> sceneDepthView;
dlss::Size renderSize;
dlss::Offset rasterJitter;
dlss::Offset previousJitter;
bool inScene = false, mainView = false, configured = false;
unsigned jitterIndex = 0;
Fvector lastPosition, lastDirection;
float lastFov = 0;
bool haveCamera = false;
bool reportedEvaluation = false;
string512 backendError = {};

const char* Method() { return useFsr ? "FSR3" : "DLSS"; }
void ShowBackendError(const char* method)
{
    string128 summary;
    xr_sprintf(summary, "%s is not active.", method);
    SetRendererError(summary, "See the console/log for details.");
}
void LogBackendError(const char* method, const char* stage, unsigned result)
{
    xr_sprintf(backendError, "%s (0x%08X)", stage, result);
    Msg("! [%s] %s", method, backendError);
    ShowBackendError(method);
}
void LogDlss(const char* stage, unsigned result) { LogBackendError("DLSS", stage, result); }
void LogFsr(const char* stage, unsigned result) { LogBackendError("FSR3", stage, result); }
void ActivationFailed(const char* reason, const char* recovery)
{
    string128 summary;
    xr_sprintf(summary, "[%s] NOT ACTIVE - rendering at native resolution", Method());
    Msg("! %s: %s", summary, reason);
    Msg("! [%s] %s", Method(), recovery);
    ShowBackendError(Method());
}
const char* BackendReason(const char* fallback) { return backendError[0] ? backendError : fallback; }
void ResetHistory() { backend.ResetHistory(); fsrBackend.ResetHistory(); aoe_temporal::ResetHistory(); }
}

unsigned RenderScreenWidth() { return inScene ? dlss::RenderWidth() : Device.dwWidth; }
unsigned RenderScreenHeight() { return inScene ? dlss::RenderHeight() : Device.dwHeight; }

namespace dlss
{
void InitializeTargets()
{
    PROF_EVENT("Render.Upscaler.InitializeTargets");
    ReleaseTargets();
    renderSize = {Device.dwWidth, Device.dwHeight};
    backendError[0] = 0;
    if (!ps_r_upscaler || (ps_r_upscaler == 1 && !ps_r_dlss_quality))
    {
        SetRendererError(nullptr);
        Msg("[Upscaler] Off: rendering at native resolution.");
        return;
    }
    useFsr = ps_r_upscaler == 2;
    string_path marker;
    for (const char* shader : {"r3\\aoe_screen.vs", "r3\\aoe_copy.ps", "r3\\aoe_sharpen.ps",
        "r3\\aoe_cas.h", "r3\\aoe_gamma.ps", "r3\\aoe_motion_resolve.ps", "r3\\aoe_post_inputs.ps",
        "r3\\aoe_blur_motion.ps"})
    {
        if (FS.exist(marker, "$game_shaders$", shader)) continue;
        string256 reason;
        xr_sprintf(reason, "Required shader is missing: %s", shader);
        ActivationFailed(reason, "Install the AOE Remade engine shader files, then restart the game.");
        return;
    }
    if (RImplementation.o.dx10_msaa)
    {
        ActivationFailed("MSAA is enabled and cannot be combined with temporal upscaling.",
            "Turn MSAA Off in Video settings and Apply again (or run vid_restart).");
        return;
    }
    Size size;
    const Size display{Device.dwWidth, Device.dwHeight};
    if (useFsr)
    {
        // Release NGX as well when switching methods; FSR does not require NVIDIA.
        backend.Shutdown();
        if (!fsr3::D3D11Backend::OptimalSize(ps_r_fsr3_quality, display, size))
        {
            ActivationFailed("Could not determine the FSR 3 render resolution.",
                "Select a valid display resolution and FSR 3 quality, then Apply again.");
            return;
        }
        if (!fsrBackend.Create(HW.pDevice, size, display, RImplementation.o.dx11_hdr10, LogFsr))
        {
            ActivationFailed(BackendReason("FSR 3 context creation failed."),
                "FSR 3 requires Direct3D feature level 11.1. Check the console/log, then reapply or switch Upscaling Off.");
            return;
        }
    }
    else
    {
        string_path cache;
        FS.update_path(cache, "$app_data_root$", "");
        wchar_t wideCache[MAX_PATH] = {};
        if (!MultiByteToWideChar(CP_ACP, 0, cache, -1, wideCache, MAX_PATH))
        {
            LogDlss("Cannot convert the application-data path for NGX", GetLastError());
            ActivationFailed(backendError, "Check the application-data path in fsgame.ltx, then restart the game.");
            return;
        }
        if (!backend.Initialize(HW.pDevice, wideCache, LogDlss))
        {
            ActivationFailed(BackendReason("NVIDIA NGX initialization failed."),
                "Check RTX GPU/driver support and nvngx_dlss.dll beside the executable, then restart the game.");
            return;
        }
        const auto quality = static_cast<Quality>(ps_r_dlss_quality);
        if (!backend.OptimalSize(quality, display, size))
        {
            ActivationFailed(BackendReason("Could not determine the DLSS render resolution."),
                "Select a valid display resolution and DLSS quality, then Apply again.");
            return;
        }
        if (!backend.Create(quality, size, display, RImplementation.o.dx11_hdr10, static_cast<Preset>(ps_r_dlss_preset)))
        {
            ActivationFailed(BackendReason("DLSS feature creation failed."),
                "Check the console/log and GPU driver/runtime. Reapply Upscaling or switch it Off.");
            return;
        }
    }

    D3D11_TEXTURE2D_DESC depth = {};
    depth.Width = size.width;
    depth.Height = size.height;
    depth.MipLevels = depth.ArraySize = depth.SampleDesc.Count = 1;
    depth.Format = DXGI_FORMAT_R24G8_TYPELESS;
    depth.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    D3D11_DEPTH_STENCIL_VIEW_DESC view = {};
    view.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    view.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    HRESULT result = HW.pDevice->CreateTexture2D(&depth, nullptr, &sceneDepth);
    if (SUCCEEDED(result)) result = HW.pDevice->CreateDepthStencilView(sceneDepth.Get(), &view, &sceneDepthView);
    if (FAILED(result))
    {
        (useFsr ? LogFsr : LogDlss)("Scene depth allocation failed; retaining native resolution", result);
        ActivationFailed(backendError, "Check GPU memory/device errors in the console/log, then restart the game.");
        ReleaseTargets();
        return;
    }
    renderSize = size;
    configured = true;
    aoe_temporal::Initialize();
    if (useFsr)
        Msg("[FSR3] Initialized; awaiting first scene evaluation: quality=%u scene=%ux%u display=%ux%u",
            ps_r_fsr3_quality, size.width, size.height, display.width, display.height);
    else
        Msg("[DLSS] Initialized; awaiting first scene evaluation: quality=%u requested preset=%u scene=%ux%u display=%ux%u",
            ps_r_dlss_quality, ps_r_dlss_preset, size.width, size.height, display.width, display.height);
}

void ReleaseTargets()
{
    aoe_temporal::Release();
    inScene = mainView = configured = false;
    jitterIndex = 0;
    haveCamera = false;
    reportedEvaluation = false;
    rasterJitter = {};
    previousJitter = {};
    backend.ReleaseFeature();
    fsrBackend.Destroy();
    useFsr = false;
    sceneDepthView.Reset();
    sceneDepth.Reset();
    renderSize = {};
}

void ShutdownDevice() { ReleaseTargets(); aoe_temporal::Shutdown(); backend.Shutdown(); }
unsigned RenderWidth() { return renderSize.Valid() ? renderSize.width : Device.dwWidth; }
unsigned RenderHeight() { return renderSize.Valid() ? renderSize.height : Device.dwHeight; }
bool Configured() { return configured; }
bool Active() { return configured && inScene && mainView && (useFsr ? fsrBackend.Ready() : backend.Ready()); }
bool TemporalScene() { return configured && inScene && mainView; }
bool UsesFsr() { return useFsr && configured; }
bool LinearColor() { return UsesFsr() && !RImplementation.o.dx11_hdr10; }
Offset RasterJitter() { return rasterJitter; }
Offset PreviousRasterJitter() { return previousJitter; }
ID3DDepthStencilView* SceneDepth() { return inScene && configured ? sceneDepthView.Get() : HW.pBaseZB; }
ID3D11Texture2D* SceneDepthTexture() { return sceneDepth.Get(); }
ID3D11Texture2D* Output() { return useFsr ? fsrBackend.Output() : backend.Output(); }

void BeginScene()
{
    aoe_temporal::DisplayInputs(nullptr, nullptr, nullptr, nullptr);
    inScene = true;
    if (configured)
    {
        const D3D11_VIEWPORT viewport{0, 0, float(RenderWidth()), float(RenderHeight()), 0, 1};
        HW.pContext->RSSetViewports(1, &viewport);
    }
    // Scope frames render spatially at the selected scene resolution. They must
    // neither consume a jitter sample nor overwrite the main camera's history.
    mainView = !Device.m_SecondViewport.IsSVPFrame();
    if (Active())
    {
        if (haveCamera && (lastPosition.distance_to_sqr(Device.vCameraPosition) > 64.0f * 64.0f ||
            lastDirection.dotproduct(Device.vCameraDirection) < 0.25f || _abs(lastFov - Device.fFOV) > 10.0f))
        {
            ResetHistory();
            jitterIndex = 0;
        }
        lastPosition = Device.vCameraPosition;
        lastDirection = Device.vCameraDirection;
        lastFov = Device.fFOV;
        haveCamera = true;
        // FSR3 recommends at least -1 before the resolution ratio. Keep a
        // stronger user setting, but prevent high-frequency foliage/effects
        // from aliasing at zero or positive bias.
        const float baseBias = useFsr && ps_r__tf_Mipbias > -1.0f ? -1.0f : ps_r__tf_Mipbias;
        SSManager.SetMipLODBias(baseBias + log2f(float(RenderWidth()) / Device.dwWidth));
    }
    rasterJitter = Active() ? Jitter(jitterIndex++, renderSize, {Device.dwWidth, Device.dwHeight}) : Offset{};
    if (TemporalScene()) aoe_temporal::BeginFrame();
}

void EndScene(bool invalidateHistory)
{
    if (configured && inScene && mainView) previousJitter = rasterJitter;
    if (configured && inScene) SSManager.SetMipLODBias(ps_r__tf_Mipbias);
    inScene = mainView = false;
    rasterJitter = {};
    if (invalidateHistory) { ResetHistory(); jitterIndex = 0; haveCamera = false; previousJitter = {}; }
}

bool Evaluate(ID3D11Texture2D* color, ID3D11Texture2D* motion, ID3D11Texture2D* reactive)
{
    if (!Active()) return false;
    PROF_EVENT_DYNAMIC(useFsr ? "Render.Upscaler.Evaluate.FSR3" : "Render.Upscaler.Evaluate.DLSS");
    fsr3::D3D11Backend::Frame frame;
    frame.color = color;
    frame.depth = sceneDepth.Get();
    frame.motion = motion;
    frame.reactive = reactive;
    frame.jitter = rasterJitter;
    frame.deltaMilliseconds = Device.fTimeDelta * 1000.0f;
    frame.enableSharpening = useFsr && ps_r_dlss_sharpness > 0.0f;
    frame.sharpness = clampr(ps_r_dlss_sharpness, 0.0f, 1.0f);
    frame.frame = Device.dwFrame;
    // Derive the actual main-view near/far/FOV from its projection, including
    // camera overrides, rather than assuming the environment's nominal range.
    frame.cameraNear = -Device.mProject._43 / Device.mProject._33;
    frame.cameraFar = -Device.mProject._43 / (Device.mProject._33 - 1.0f);
    frame.verticalFov = 2.0f * atanf(1.0f / Device.mProject._22);
    const bool success = useFsr ? fsrBackend.Evaluate(frame) : backend.Evaluate(frame);
    if (success && !reportedEvaluation)
    {
        SetRendererError(nullptr);
        Msg("- [%s] ACTIVE - first scene evaluation succeeded: frame=%u, scene=%ux%u, jitter=(%.4f,%.4f), MV scale=(-%u,-%u)",
            Method(), Device.dwFrame, RenderWidth(), RenderHeight(), rasterJitter.x, rasterJitter.y, RenderWidth(), RenderHeight());
        reportedEvaluation = true;
    }
    else if (!success)
    {
        string128 summary;
        xr_sprintf(summary, "[%s] STOPPED - using spatial upscale without temporal reconstruction", Method());
        const char* reason = BackendReason("Scene evaluation failed.");
        const char* recovery = "Check the console/log. Reapply Upscaling (vid_restart) or switch it Off in Video settings.";
        Msg("! %s: %s", summary, reason);
        Msg("! [%s] %s", Method(), recovery);
        ShowBackendError(Method());
    }
    return success;
}
}
