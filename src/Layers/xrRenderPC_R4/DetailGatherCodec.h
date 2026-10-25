#pragma once

// Compact grass instance record (32 bytes) and the compute shader that expands records into the 80-byte stream that
// the instanced grass vertex shader reads. Header-only and engine-free so that .ai_workspace/tests/detail-gather can
// run exactly this code against the GPU.
#include <cmath>
#include <cstdint>

namespace detail_gather
{
// w0..w2 translation, w3 uniform scale, w4/w5 rotation quaternion (xy, zw as snorm16), w6 octahedral normal (snorm16 x2),
// w7 = sun | hemi << 8 | alpha << 16 | mirrored << 24 (all unorm8 / flag).
struct Packed
{
    uint32_t w[8];
};

inline uint32_t Bits(float value)
{
    union
    {
        float f;
        uint32_t u;
    } bits;
    bits.f = value;
    return bits.u;
}

inline uint32_t Snorm16(float value)
{
    if (!(value == value))
        value = 0.f;
    value = value < -1.f ? -1.f : (value > 1.f ? 1.f : value);
    return uint32_t(int32_t(std::lround(value * 32767.f))) & 0xffffu;
}

inline uint32_t Unorm8(float value)
{
    if (!(value == value))
        value = 0.f;
    value = value < 0.f ? 0.f : (value > 1.f ? 1.f : value);
    return uint32_t(std::lround(value * 255.f));
}

// m[r][c] is Fmatrix::_{r+1}{c+1} (uniform scale times a rotation or a mirrored rotation), t its translation row.
inline void Pack(const float m[3][3], const float t[3], const float n[3], float sun, float hemi, float alpha, Packed& out)
{
    float scale = std::sqrt(m[0][0] * m[0][0] + m[0][1] * m[0][1] + m[0][2] * m[0][2]);
    if (!(scale > 1e-8f))
        scale = 1.f;
    float a[3][3];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            a[r][c] = m[r][c] / scale;
    const float det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
        a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    const bool mirrored = det < 0.f;
    if (mirrored)
        for (int r = 0; r < 3; ++r)
            a[r][2] = -a[r][2];

    float x, y, z, w;
    const float trace = a[0][0] + a[1][1] + a[2][2];
    if (trace > 0.f)
    {
        const float s = 0.5f / std::sqrt(trace + 1.f);
        w = 0.25f / s;
        x = (a[2][1] - a[1][2]) * s;
        y = (a[0][2] - a[2][0]) * s;
        z = (a[1][0] - a[0][1]) * s;
    }
    else if (a[0][0] > a[1][1] && a[0][0] > a[2][2])
    {
        const float s = 2.f * std::sqrt(1.f + a[0][0] - a[1][1] - a[2][2]);
        w = (a[2][1] - a[1][2]) / s;
        x = 0.25f * s;
        y = (a[0][1] + a[1][0]) / s;
        z = (a[0][2] + a[2][0]) / s;
    }
    else if (a[1][1] > a[2][2])
    {
        const float s = 2.f * std::sqrt(1.f + a[1][1] - a[0][0] - a[2][2]);
        w = (a[0][2] - a[2][0]) / s;
        x = (a[0][1] + a[1][0]) / s;
        y = 0.25f * s;
        z = (a[1][2] + a[2][1]) / s;
    }
    else
    {
        const float s = 2.f * std::sqrt(1.f + a[2][2] - a[0][0] - a[1][1]);
        w = (a[1][0] - a[0][1]) / s;
        x = (a[0][2] + a[2][0]) / s;
        y = (a[1][2] + a[2][1]) / s;
        z = 0.25f * s;
    }
    const float length = std::sqrt(x * x + y * y + z * z + w * w);
    if (length > 1e-8f)
    {
        x /= length; y /= length; z /= length; w /= length;
    }
    else
    {
        x = y = z = 0.f; w = 1.f;
    }

    // Octahedral normal; an invalid vector falls back to up.
    float nx = n[0], ny = n[1], nz = n[2];
    float l1 = std::fabs(nx) + std::fabs(ny) + std::fabs(nz);
    if (!(l1 > 1e-6f) || !(l1 == l1))
    {
        nx = 0.f; ny = 1.f; nz = 0.f; l1 = 1.f;
    }
    float px = nx / l1, py = ny / l1;
    if (nz < 0.f)
    {
        const float ox = (1.f - std::fabs(py)) * (px >= 0.f ? 1.f : -1.f);
        const float oy = (1.f - std::fabs(px)) * (py >= 0.f ? 1.f : -1.f);
        px = ox; py = oy;
    }

    out.w[0] = Bits(t[0]);
    out.w[1] = Bits(t[1]);
    out.w[2] = Bits(t[2]);
    out.w[3] = Bits(scale);
    out.w[4] = Snorm16(x) | (Snorm16(y) << 16);
    out.w[5] = Snorm16(z) | (Snorm16(w) << 16);
    out.w[6] = Snorm16(px) | (Snorm16(py) << 16);
    out.w[7] = Unorm8(sun) | (Unorm8(hemi) << 8) | (Unorm8(alpha) << 16) | (mirrored ? 1u << 24 : 0u);
}

// Ranges buffer (Buffer<uint4>): entry 0 is the header {records, range count, dispatch groups in x, index of the thinning
// entry}; entries 1..N are {source record (bit 31: transient buffer), count | last blade's scale << 16 (1/65535),
// destination record, distance bits}, sorted by destination. The thinning entry {start metres, 1 / range metres, kept share
// at the end of the range, 0} widens the blades of a slot whose share is below 1 by 1 / sqrt(share) in x and z (see
// detail_gather::Thin).
inline const char* ShaderSource()
{
    return R"hlsl(
Buffer<uint4> g_arena : register(t0);
Buffer<uint4> g_transient : register(t1);
Buffer<uint4> g_ranges : register(t2);
RWByteAddressBuffer g_out : register(u0);

float snorm16(uint v)
{
    return max(float(asint(v << 16) >> 16) / 32767.0f, -1.0f);
}

void gather(uint index, uint4 header)
{
    uint lo = 1, hi = header.y;
    while (lo < hi)
    {
        uint mid = (lo + hi + 1) >> 1;
        if (g_ranges[mid].z <= index)
            lo = mid;
        else
            hi = mid - 1;
    }
    uint4 range = g_ranges[lo];
    uint record = (range.x & 0x7fffffffu) + (index - range.z);
    uint4 a, b;
    if (range.x & 0x80000000u)
    {
        a = g_transient[record * 2];
        b = g_transient[record * 2 + 1];
    }
    else
    {
        a = g_arena[record * 2];
        b = g_arena[record * 2 + 1];
    }

    float3 pos = asfloat(a.xyz);
    float scale = asfloat(a.w);
    float4 q = float4(snorm16(b.x), snorm16(b.x >> 16), snorm16(b.y), snorm16(b.y >> 16));
    q = q / max(length(q), 1e-6f);
    float x = q.x, y = q.y, z = q.z, w = q.w;
    float3x3 A = float3x3(1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
                          2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                          2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y));
    if ((b.w >> 24) & 1)
    {
        A[0][2] = -A[0][2];
        A[1][2] = -A[1][2];
        A[2][2] = -A[2][2];
    }
    float2 e = float2(snorm16(b.z), snorm16(b.z >> 16));
    float3 n = float3(e.x, e.y, 1.0f - abs(e.x) - abs(e.y));
    if (n.z < 0.0f)
    {
        float2 f = (1.0f - abs(n.yx)) * float2(n.x >= 0.0f ? 1.0f : -1.0f, n.y >= 0.0f ? 1.0f : -1.0f);
        n.xy = f;
    }
    n = normalize(n);
    float sun = float(b.w & 255u) / 255.0f;
    float hemi = float((b.w >> 8) & 255u) / 255.0f;
    float alpha = float((b.w >> 16) & 255u) / 255.0f;
    float distance = asfloat(range.w);
    if (index - range.z + 1 == (range.y & 0xffffu))
        scale *= float(range.y >> 16) / 65535.0f;
    float4 thin = asfloat(g_ranges[header.w]);
    float share = 1.0f + (thin.z - 1.0f) * saturate((sqrt(max(distance, 0.0f)) - thin.x) * thin.y);
    float3 widen = float3(rsqrt(share), 1.0f, rsqrt(share));
    uint address = index * 80;
    g_out.Store4(address + 0, asuint(float4(scale * float3(A[0][0], A[1][0], A[2][0]) * widen, pos.x)));
    g_out.Store4(address + 16, asuint(float4(scale * float3(A[0][1], A[1][1], A[2][1]) * widen, pos.y)));
    g_out.Store4(address + 32, asuint(float4(scale * float3(A[0][2], A[1][2], A[2][2]) * widen, pos.z)));
    g_out.Store4(address + 48, asuint(float4(n, alpha)));
    g_out.Store4(address + 64, asuint(float4(sun, hemi, distance, 0.0f)));
}

[numthreads(256, 1, 1)]
void main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    uint4 header = g_ranges[0];
    uint index = (group.y * header.z + group.x) * 256 + thread.x;
    if (index < header.x)
        gather(index, header);
}
)hlsl";
}
}
