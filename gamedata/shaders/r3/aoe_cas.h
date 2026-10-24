// CAS adapted from OGSR postprocess_cas.ps and AMD FidelityFX CAS:
// https://gpuopen.com/wp-content/uploads/2019/07/FidelityFX-CAS.pptx
// Reconstructed colour is filtered before display overlays and UI.
float3 AoeCasSample(float2 uv, int2 offset)
{
    float3 value = max(aoe_image.SampleLevel(aoe_sampler, uv, 0, offset).rgb, 0);
    return saturate(pow(value, aoe_encode.xxx));
}

float4 AoeCas(float2 uv)
{
    float4 original = aoe_image.SampleLevel(aoe_sampler, uv, 0);
    float strength = saturate(aoe_sharpening.x);
    float3 e = saturate(pow(max(original.rgb, 0), aoe_encode.xxx));
    if (strength <= 0) return float4(e, original.a);
    float3 a = AoeCasSample(uv, int2(-1, -1));
    float3 b = AoeCasSample(uv, int2(0, -1));
    float3 c = AoeCasSample(uv, int2(1, -1));
    float3 d = AoeCasSample(uv, int2(-1, 0));
    float3 f = AoeCasSample(uv, int2(1, 0));
    float3 g = AoeCasSample(uv, int2(-1, 1));
    float3 h = AoeCasSample(uv, int2(0, 1));
    float3 i = AoeCasSample(uv, int2(1, 1));
    float3 minimum = min(min(min(d, e), min(f, b)), h);
    minimum += min(minimum, min(min(a, c), min(g, i)));
    float3 maximum = max(max(max(d, e), max(f, b)), h);
    maximum += max(maximum, max(max(a, c), max(g, i)));
    float3 amplitude = sqrt(saturate(min(minimum, 2 - maximum) / max(maximum, 1e-6)));
    float3 weight = -amplitude / 5;
    float3 filtered = saturate(((b + d + f + h) * weight + e) / (1 + 4 * weight));
    float3 result = lerp(e, filtered, strength);
    return float4(result, original.a);
}
