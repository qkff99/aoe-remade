// AOE-owned full-screen geometry. No dependency on material/common headers.
struct AOE_Screen
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

AOE_Screen main(float2 uv : TEXCOORD0)
{
    AOE_Screen result;
    result.position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    result.uv = uv;
    return result;
}
