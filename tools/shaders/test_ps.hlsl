cbuffer C7 : register(b7) { float4 k[4]; };
SamplerState s0 : register(s0);
Texture2D<float4> t77 : register(t77);
float4 main(float4 pos : SV_Position, float4 col : TEXCOORD0, float2 uv : TEXCOORD1) : SV_Target0
{
    float checker = fmod(floor(pos.x/8)+floor(pos.y/8), 2);
    return col * lerp(0.3, 1.0, checker) * t77.Sample(s0, uv) + k[0]*0;
}
