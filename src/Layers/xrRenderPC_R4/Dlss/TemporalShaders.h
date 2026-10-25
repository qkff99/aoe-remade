#pragma once

#include <d3d11.h>
#include <d3dcompiler.h>

class CBackend;
class CTexture;
struct R_constant;

namespace aoe_temporal
{
void Register(const char *name, const char *target, const char *entry, const void *source, unsigned source_size,
              const D3D_SHADER_MACRO *defines, unsigned flags, ID3D11DeviceChild *shader, const void *bytecode,
              size_t bytes);
void Initialize();
void RegisterPass(ID3D11VertexShader *vertex, ID3D11PixelShader *pixel);
void Release();
void Shutdown();
void BeginFrame();
void AnimationHistory(bool grass, const Fvector4 &previous_wave, const Fvector4 &previous_wind);
void ProfileFrame();
void ResetHistory();
void SetSceneGeometry(bool enabled);
void SetIdentity(const void *visual, const void *object, bool hud = false);
bool Bind(CBackend &backend, unsigned base_vertex, unsigned first, unsigned count, bool indexed);
void Restore(CBackend &backend);
bool Geometry(ID3D11VertexShader *shader);
void Projection(Fmatrix &matrix, bool previous);
// Current clip space to previous clip space; the sky variant ignores camera translation.
void Reprojection(Fmatrix &result, bool sky, bool jittered);
void SetMatrix(CBackend &backend, R_constant *constant, const Fmatrix &value);
void DisplayInputs(CTexture *position, ID3D11ShaderResourceView *position_view, CTexture *heat,
                   ID3D11ShaderResourceView *heat_view);
ID3D11ShaderResourceView *TextureView(CTexture *source, ID3D11ShaderResourceView *native);
void Forget(ID3D11DeviceChild *shader);
} // namespace aoe_temporal
