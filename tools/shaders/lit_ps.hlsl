// Simple lighting for solid debug shapes: the face normal from the world position's screen derivatives (pairs with
// lit_vs.hlsl), a fixed light from above and a hemisphere ambient. Lines have no area and stay unlit.
// @vertex lit_vs.hlsl
#include "debugdraw.hlsli"

struct LitPixel
{
    float4 position : SV_Position;
    float4 uvRG : TEXCOORD0;
    float2 BA : TEXCOORD1;
    float clipDistance : SV_ClipDistance0;
    float3 world : TEXCOORD2;
};

float4 main(LitPixel input) : SV_Target0
{
    DebugPixel pixel;
    pixel.position = input.position;
    pixel.uvRG = input.uvRG;
    pixel.BA = input.BA;
    float4 color = DebugColor(pixel);

    // Screen right x screen down, in a right-handed Z-up world, points back toward the camera.
    float3 normal = cross(ddy(input.world), ddx(input.world));
    float area = length(normal);
    if (area > 1e-8)
    {
        normal /= area;
        float3 light = normalize(float3(0.35, 0.25, 0.9));
        float diffuse = saturate(dot(normal, light));
        float ambient = lerp(0.25, 0.5, normal.z * 0.5 + 0.5);
        color.rgb *= ambient + 0.75 * diffuse;
    }
    return Premultiply(color);
}
