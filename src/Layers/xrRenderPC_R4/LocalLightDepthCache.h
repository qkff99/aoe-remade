#pragma once

#include <d3dcompiler.h>
#include <wrl/client.h>
#include "../xrRender/light.h"
#include "../xrRender/du_sphere_part.h"
#include "../xrRender/du_cone.h"

extern Fvector3 ps_ssfx_shadow_bias;

namespace local_light_depth
{
using Microsoft::WRL::ComPtr;
constexpr size_t budget_bytes = 128u * 1024u * 1024u;

struct ReceiverQueries
{
    struct Query
    {
        ComPtr<ID3D11Query> handle;
        u32 frame = 0;
        u64 generation = 0;
    };
    static constexpr u32 capacity = 3;
    Query queries[capacity];
    u32 first = 0, pending = 0, result_frame = 0;
    u64 result_generation = 0;
    bool hidden = false;

    Query* Available()
    {
        if (pending == capacity) return nullptr;
        auto& query = queries[(first + pending) % capacity];
        if (!query.handle)
        {
            const D3D11_QUERY_DESC description{D3D11_QUERY_OCCLUSION, 0};
            if (FAILED(HW.pDevice->CreateQuery(&description, query.handle.GetAddressOf()))) return nullptr;
        }
        return &query;
    }

    bool Resolve(u32 frame, u64 generation, u32& polls, u32& ready)
    {
        polls = ready = 0;
        while (pending)
        {
            auto& query = queries[first];
            if (query.frame == frame) break;
            UINT64 samples = 0;
            ++polls;
            const HRESULT result = HW.pContext->GetData(query.handle.Get(), &samples, sizeof(samples), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (result == S_FALSE) break;
            --pending;
            first = (first + 1) % capacity;
            hidden = result == S_OK && samples == 0;
            result_frame = query.frame;
            result_generation = query.generation;
            ready += result == S_OK;
            if (FAILED(result)) query.handle.Reset();
        }
        // A pending test does not revoke the last completed answer. A changed
        // camera/settings generation or a new visible/error answer does.
        return hidden && result_generation == generation;
    }
};

struct Tile : ReceiverQueries
{
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    ComPtr<ID3D11RenderTargetView> target;
    Fmatrix transform;
    u32 size = 0;
    bool valid = false, written = false;

    ~Tile()
    {
        if (texture) HW.stats_manager.decrement_stats_rtarget(texture.Get());
    }

    size_t Bytes() const { return size_t(size) * size * sizeof(float); }
    bool Matches(const light& source) const
    {
        return size == source.X.S.size && !memcmp(&transform, &source.X.S.combine, sizeof(transform));
    }

    bool Initialize(const light& source)
    {
        const auto& original = RImplementation.Target->rt_smap_depth;
        D3D11_TEXTURE2D_DESC description{};
        original->pSurface->GetDesc(&description);
        description.Width = description.Height = source.X.S.size;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        description.MiscFlags = description.CPUAccessFlags = 0;
        if (description.SampleDesc.Count != 1 || description.MipLevels != 1 || description.ArraySize != 1 ||
            description.Format != DXGI_FORMAT_R32_TYPELESS) return false;
        description.Format = DXGI_FORMAT_R32_FLOAT;
        if (FAILED(HW.pDevice->CreateTexture2D(&description, nullptr, texture.GetAddressOf()))) return false;
        HW.stats_manager.increment_stats_rtarget(texture.Get());
        if (FAILED(HW.pDevice->CreateShaderResourceView(texture.Get(), nullptr, view.GetAddressOf())) ||
            FAILED(HW.pDevice->CreateRenderTargetView(texture.Get(), nullptr, target.GetAddressOf()))) return false;
        size = source.X.S.size;
        transform = source.X.S.combine;
        return true;
    }

};

inline bool Compile(const char* source, const char* target, ID3DBlob** code)
{
    ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(source, strlen(source), "local_light_depth", nullptr, nullptr,
        "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code, errors.GetAddressOf());
    if (FAILED(result)) Msg("! Local light depth shader unavailable: %s", errors ?
        static_cast<const char*>(errors->GetBufferPointer()) : "compiler failure");
    return SUCCEEDED(result);
}

// D3D11 forbids partial copies of depth-stencil subresources. Use exact texel
// loads into R32_FLOAT and SV_Depth on restore; no filtering or depth bias.
class Copier
{
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> color, depth_pixel;
    ComPtr<ID3D11Buffer> buffer;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11BlendState> blend;
public:
    bool Initialize()
    {
        if (depth_pixel) return true;
        static constexpr char vertex_source[] = R"(
float4 main(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
})";
        static constexpr char color_source[] = R"(
cbuffer CopyConstants : register(b0) { int4 offset; };
Texture2D<float> source : register(t0);
float main(float4 pixel : SV_Position) : SV_Target
{ return source.Load(int3(int2(pixel.xy) + offset.xy, 0)); }
)";
        static constexpr char depth_source[] = R"(
cbuffer CopyConstants : register(b0) { int4 offset; };
Texture2D<float> source : register(t0);
float main(float4 pixel : SV_Position) : SV_Depth
{ return source.Load(int3(int2(pixel.xy) + offset.xy, 0)); }
)";
        ComPtr<ID3DBlob> vs, ps, ds;
        if (!Compile(vertex_source, "vs_5_0", vs.GetAddressOf()) ||
            !Compile(color_source, "ps_5_0", ps.GetAddressOf()) || !Compile(depth_source, "ps_5_0", ds.GetAddressOf())) return false;
        if (FAILED(HW.pDevice->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, vertex.ReleaseAndGetAddressOf()))) return false;
        if (FAILED(HW.pDevice->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, color.ReleaseAndGetAddressOf()))) return false;
        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = 16; cb.Usage = D3D11_USAGE_DYNAMIC;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(HW.pDevice->CreateBuffer(&cb, nullptr, buffer.ReleaseAndGetAddressOf()))) return false;
        D3D11_DEPTH_STENCIL_DESC dz{};
        dz.DepthEnable = TRUE; dz.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dz.DepthFunc = D3D11_COMPARISON_ALWAYS;
        dz.FrontFace = dz.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
        if (FAILED(HW.pDevice->CreateDepthStencilState(&dz, depth.ReleaseAndGetAddressOf()))) return false;
        D3D11_RASTERIZER_DESC rs{};
        rs.FillMode = D3D11_FILL_SOLID; rs.CullMode = D3D11_CULL_NONE; rs.DepthClipEnable = TRUE;
        if (FAILED(HW.pDevice->CreateRasterizerState(&rs, rasterizer.ReleaseAndGetAddressOf()))) return false;
        D3D11_BLEND_DESC bs{};
        for (auto& rt : bs.RenderTarget)
        {
            rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
            rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_ZERO;
            rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        }
        if (FAILED(HW.pDevice->CreateBlendState(&bs, blend.ReleaseAndGetAddressOf()))) return false;
        return SUCCEEDED(HW.pDevice->CreatePixelShader(ds->GetBufferPointer(), ds->GetBufferSize(), nullptr, depth_pixel.ReleaseAndGetAddressOf()));
    }

    bool Transfer(Tile& tile, const light& source, ShadowMap& map, bool store)
    {
        auto* context = RenderDeviceContext();
        const int offset[4] = {store ? int(source.X.S.posX) : -int(source.X.S.posX),
            store ? int(source.X.S.posY) : -int(source.X.S.posY), 0, 0};
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
        CopyMemory(mapped.pData, offset, sizeof(offset));
        context->Unmap(buffer.Get(), 0);
        ID3D11RenderTargetView* target = store ? tile.target.Get() : nullptr;
        context->OMSetRenderTargets(store ? 1 : 0, store ? &target : nullptr, store ? nullptr : map.Depth());
        context->OMSetDepthStencilState(depth.Get(), 0);
        context->OMSetBlendState(blend.Get(), nullptr, UINT_MAX);
        context->RSSetState(rasterizer.Get());
        const D3D11_VIEWPORT viewport{store ? 0.f : float(source.X.S.posX), store ? 0.f : float(source.X.S.posY),
            float(tile.size), float(tile.size), 0.f, 1.f};
        context->RSSetViewports(1, &viewport);
        context->IASetInputLayout(nullptr); context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex.Get(), nullptr, 0);
        context->GSSetShader(nullptr, nullptr, 0); context->HSSetShader(nullptr, nullptr, 0); context->DSSetShader(nullptr, nullptr, 0);
        context->PSSetShader(store ? color.Get() : depth_pixel.Get(), nullptr, 0);
        auto* constants = buffer.Get();
        context->PSSetConstantBuffers(0, 1, &constants);
        ID3D11ShaderResourceView* input = store ? map.View() : tile.view.Get();
        context->PSSetShaderResources(0, 1, &input);
        context->Draw(3, 0);
        input = nullptr;
        context->PSSetShaderResources(0, 1, &input);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        RCache.ResetBindings();
        return true;
    }
};

// Read the previous completed query before issuing another; never wait for GPU.
class Receiver
{
    struct Constants
    {
        Fmatrix shadow;
        Fvector4 light, decompression, dimensions, jitter, settings, axes[3];
    };
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    ComPtr<ID3D11Buffer> buffer;
    ComPtr<ID3D11DepthStencilState> depth;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11SamplerState> sampler;
    bool attempted = false;

    bool Initialize()
    {
        static_assert(sizeof(Constants) == 192, "Receiver constants must match HLSL");
        static constexpr char vertex_source[] = R"(
float4 main(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
})";
        static constexpr char pixel_source[] = R"(
cbuffer ReceiverConstants : register(b0)
{
    row_major float4x4 shadow;
    float4 light, decompression, dimensions, jitter, settings, axes[3];
};
Texture2D<float> shadow_map : register(t0);
Texture2D<float4> position_buffer : register(t1);
SamplerComparisonState comparison : register(s0);

float shadow_potential(float3 position)
{
    float4 projected = mul(shadow, float4(position, 1));
    if (!all(isfinite(projected)) || projected.w <= 0.000001) return 1;
    float3 coord = projected.xyz / projected.w;
    coord.z -= 0.000095; // Retain a depth margin beyond the native PCF bias.
    float2 texel = coord.xy * settings.y + 0.5;
    float2 f = frac(texel), base = (floor(texel) - 0.5) / settings.y;
    float2 a = (3 - 2 * f) / (4 - 3 * f) - 2;
    float2 b = (f + 3) / 7;
    float2 c = f / (1 + 3 * f) + 2;
    float result = 0;
    result += shadow_map.SampleCmpLevelZero(comparison, base + float2(a.x, a.y) / settings.y, coord.z);
    result += shadow_map.SampleCmpLevelZero(comparison, base + float2(b.x, a.y) / settings.y, coord.z);
    result += shadow_map.SampleCmpLevelZero(comparison, base + float2(c.x, a.y) / settings.y, coord.z);
    result += shadow_map.SampleCmpLevelZero(comparison, base + float2(a.x, b.y) / settings.y, coord.z);
    result += shadow_map.SampleCmpLevelZero(comparison, base + float2(b.x, b.y) / settings.y, coord.z);
    result += shadow_map.SampleCmpLevelZero(comparison, base + float2(c.x, b.y) / settings.y, coord.z);
    result += shadow_map.SampleCmpLevelZero(comparison, base + float2(a.x, c.y) / settings.y, coord.z);
    result += shadow_map.SampleCmpLevelZero(comparison, base + float2(b.x, c.y) / settings.y, coord.z);
    result += shadow_map.SampleCmpLevelZero(comparison, base + float2(c.x, c.y) / settings.y, coord.z);
    return result;
}

void main(float4 pixel : SV_Position)
{
    float4 packed = position_buffer.Load(int3(uint2(pixel.xy), 0));
    if (!all(isfinite(packed)) || abs(packed.y) > 1.0001) return;
    clip(packed.z - 0.0001);
    float2 p = pixel.xy;
    p -= jitter.xy * dimensions.xy * float2(0.5, -0.5);
    float3 position = float3((p * decompression.zw - decompression.xy) * packed.z, packed.z);
    float3 to_light = light.xyz - position;
    float distance_squared = dot(to_light, to_light);
    clip(1.0001 - distance_squared * light.w);
    float3 from_light = -to_light;
    float3 local = float3(dot(from_light, axes[0].xyz), dot(from_light, axes[1].xyz), dot(from_light, axes[2].xyz));
    clip(local.z + 0.005);
    if (settings.w > 0)
    {
        float radius = max(local.z, 0) * settings.w + 0.005;
        clip(radius * radius - dot(local.xy, local.xy));
    }
    else clip(local.z + 0.005 - max(abs(local.x), abs(local.y)));
    float3 direction = to_light * rsqrt(max(distance_squared, 0.00000001));
    uint bits = asuint(packed.w);
    uint material = ((bits >> 21) & 15) | (((bits >> 31) & 1) * 16);
    bool transmitting = abs(float(material) * 0.043011 - 0.15) <= 0.05001;
    float sine, cosine;
    sincos(packed.x * 3.141590, sine, cosine);
    float radius = sqrt(max(1 - packed.y * packed.y, 0));
    float3 normal = float3(radius * cosine, radius * sine, packed.y);
    if (!transmitting) clip(dot(normal, direction) + 0.001);
    else normal = (normal + direction) * 0.5;
    float bias = 0.025 + distance_squared * light.w * settings.x * (1 - saturate(dot(normal, direction)));
    if (settings.y > 0) clip(shadow_potential(position + normal * bias) == 0 ? -1 : 1);
})";
        ComPtr<ID3DBlob> vs, ps;
        if (!Compile(vertex_source, "vs_5_0", vs.GetAddressOf()) || !Compile(pixel_source, "ps_5_0", ps.GetAddressOf())) return false;
        if (FAILED(HW.pDevice->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, vertex.GetAddressOf()))) return false;
        D3D11_BUFFER_DESC constants{};
        constants.ByteWidth = sizeof(Constants); constants.Usage = D3D11_USAGE_DYNAMIC;
        constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER; constants.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(HW.pDevice->CreateBuffer(&constants, nullptr, buffer.GetAddressOf()))) return false;
        D3D11_DEPTH_STENCIL_DESC ds{};
        ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
        ds.FrontFace = ds.BackFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
        if (FAILED(HW.pDevice->CreateDepthStencilState(&ds, depth.GetAddressOf()))) return false;
        D3D11_RASTERIZER_DESC rs{};
        rs.FillMode = D3D11_FILL_SOLID; rs.CullMode = D3D11_CULL_NONE;
        rs.DepthClipEnable = TRUE; rs.ScissorEnable = TRUE;
        if (FAILED(HW.pDevice->CreateRasterizerState(&rs, rasterizer.GetAddressOf()))) return false;
        D3D11_BLEND_DESC bs{};
        for (auto& rt : bs.RenderTarget)
        {
            rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
            rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_ZERO;
            rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        }
        if (FAILED(HW.pDevice->CreateBlendState(&bs, blend.GetAddressOf()))) return false;
        D3D11_SAMPLER_DESC ss{};
        ss.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        ss.AddressU = ss.AddressV = ss.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
        ss.MaxAnisotropy = 1;
        std::fill(std::begin(ss.BorderColor), std::end(ss.BorderColor), 1.f);
        ss.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL; ss.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(HW.pDevice->CreateSamplerState(&ss, sampler.GetAddressOf()))) return false;
        return SUCCEEDED(HW.pDevice->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, pixel.GetAddressOf()));
    }

public:
    bool Begin()
    {
        if (!attempted) { attempted = true; Initialize(); }
        if (!pixel) return false;
        auto* context = HW.pContext;
        context->OMSetRenderTargets(0, nullptr, dlss::SceneDepth());
        context->OMSetDepthStencilState(depth.Get(), 0);
        context->OMSetBlendState(blend.Get(), nullptr, UINT_MAX);
        context->RSSetState(rasterizer.Get());
        const D3D11_VIEWPORT viewport{0.f, 0.f, float(RenderScreenWidth()), float(RenderScreenHeight()), 0.f, 1.f};
        context->RSSetViewports(1, &viewport);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex.Get(), nullptr, 0);
        context->GSSetShader(nullptr, nullptr, 0);
        context->HSSetShader(nullptr, nullptr, 0);
        context->DSSetShader(nullptr, nullptr, 0);
        context->PSSetShader(pixel.Get(), nullptr, 0);
        auto* constants = buffer.Get();
        auto* comparison = sampler.Get();
        context->PSSetConstantBuffers(0, 1, &constants);
        context->PSSetSamplers(0, 1, &comparison);
        return true;
    }

    bool Issue(ReceiverQueries& tests, const light& source, u64 generation, const Tile* tile)
    {
        auto* query = tests.Available();
        if (!query) return false;
        Constants value{};
        const bool spot = source.flags.type == IRender_Light::SPOT;
        const float size = tile ? float(tile->size) : float(source.X.S.size);
        Fmatrix adjust = { (size - 2.f) / (2.f * size), 0, 0, 0,
            0, -(size - 2.f) / (2.f * size), 0, 0,
            0, 0, ps_r2_ls_depth_scale, 0,
            .5f + .5f / size, .5f + .5f / size, ps_r2_ls_depth_bias, 1 };
        Fmatrix shadow;
        shadow.mul(source.X.S.view, Device.mInvView);
        shadow.mulA_44(source.X.S.project);
        shadow.mulA_44(adjust);
        value.shadow.transpose(shadow);
        Fvector position;
        Device.mView.transform_tiny(position, source.position);
        value.light.set(position.x, position.y, position.z, 1.f / _sqr(source.range * .95f));
        value.decompression = CalculatePositionDecompression();
        value.dimensions = CalculatePositionDecompressionDimensions();
        value.jitter = CalculateRenderJitter();
        value.settings.set(ps_ssfx_shadow_bias.x, tile ? size : 0.f, CalculateUpscalerParameters().x,
            spot ? tanf(source.cone * .5f) : 0.f);
        const Fvector* world_axes[] = {&source.m_xform.i, &source.m_xform.j, &source.m_xform.k};
        for (u32 i = 0; i < 3; ++i)
        {
            Fvector axis;
            Device.mView.transform_dir(axis, *world_axes[i]);
            axis.normalize_safe();
            value.axes[i].set(axis.x, axis.y, axis.z, 0);
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(HW.pContext->Map(buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
        CopyMemory(mapped.pData, &value, sizeof(value));
        HW.pContext->Unmap(buffer.Get(), 0);

        D3D11_RECT rectangle{0, 0, LONG(value.dimensions.x), LONG(value.dimensions.y)};
        // The spatial omni sphere does not enclose every corner of the actual
        // volume mesh. Bound that mesh instead, preserving its screen coverage.
        static const Fbox omni_bounds = []
        {
            Fbox box;
            box.invalidate();
            for (u32 i = 0; i < DU_SPHERE_PART_NUMVERTEX; ++i) box.modify(du_sphere_part_vertices[i]);
            return box;
        }();
        static const Fbox spot_bounds = []
        {
            Fbox box;
            box.invalidate();
            for (u32 i = 0; i < DU_CONE_NUMVERTEX; ++i) box.modify(du_cone_vertices[i]);
            return box;
        }();
        const Fbox& bounds = spot ? spot_bounds : omni_bounds;
        Fmatrix full;
        full.mul(Device.mFullTransform, source.m_xform);
        Fvector minimum{FLT_MAX, FLT_MAX, 0}, maximum{-FLT_MAX, -FLT_MAX, 0};
        bool bounded = true;
        for (u32 i = 0; i < 8; ++i)
        {
            Fvector4 point, clip;
            point.set(i & 1 ? bounds.max.x : bounds.min.x, i & 2 ? bounds.max.y : bounds.min.y,
                i & 4 ? bounds.max.z : bounds.min.z, 1);
            full.transform(clip, point);
            if (!_valid(clip.w) || clip.w <= VIEWPORT_NEAR) { bounded = false; break; }
            const float x = (clip.x / clip.w + 1.f) * .5f * value.dimensions.x;
            const float y = (1.f - clip.y / clip.w) * .5f * value.dimensions.y;
            if (!_valid(x) || !_valid(y)) { bounded = false; break; }
            minimum.x = _min(minimum.x, x); minimum.y = _min(minimum.y, y);
            maximum.x = _max(maximum.x, x); maximum.y = _max(maximum.y, y);
        }
        if (bounded)
        {
            rectangle.left = clampr(LONG(floorf(minimum.x)) - 2, 0L, LONG(value.dimensions.x));
            rectangle.top = clampr(LONG(floorf(minimum.y)) - 2, 0L, LONG(value.dimensions.y));
            rectangle.right = clampr(LONG(ceilf(maximum.x)) + 2, rectangle.left, LONG(value.dimensions.x));
            rectangle.bottom = clampr(LONG(ceilf(maximum.y)) + 2, rectangle.top, LONG(value.dimensions.y));
        }
        HW.pContext->RSSetScissorRects(1, &rectangle);
        ID3D11ShaderResourceView* resources[] = {tile ? tile->view.Get() : nullptr,
            RImplementation.Target->rt_Position->pTexture->get_SRView()};
        HW.pContext->PSSetShaderResources(0, 2, resources);
        HW.pContext->Begin(query->handle.Get());
        HW.pContext->Draw(3, 0);
        HW.pContext->End(query->handle.Get());
        query->frame = Device.dwFrame;
        query->generation = generation;
        ++tests.pending;
        return true;
    }

    void End()
    {
        ID3D11ShaderResourceView* empty[] = {nullptr, nullptr};
        HW.pContext->PSSetShaderResources(0, 2, empty);
        RCache.InvalidateAfterCommandList();
    }
};
}
