// A world-space grid on solid shapes, for judging sizes and distances: lines every Spacing metres along the two axes
// that run across each face (a floor gets x and y lines, a wall x or y and z), a brighter line every tenth, and the
// lit shading under it (pairs with lit_vs.hlsl for the world position). Lines have no area and draw plain.
// @vertex lit_vs.hlsl
// @param Spacing scalar 1
// @param LineColor color 255 255 255 255
#include "debugdraw.hlsli"

cbuffer ShaderSpecificConstants : register(b4)
{
    float4 spacing;   // register 0, scalar in x
    float4 lineColor; // register 1
};

struct LitPixel
{
    float4 position : SV_Position;
    float4 uvRG : TEXCOORD0;
    float2 BA : TEXCOORD1;
    float clipDistance : SV_ClipDistance0;
    float3 world : TEXCOORD2;
};

// 1 on a line through every multiple of aStep, fading out over a pixel; per axis.
float3 GridLines(float3 aWorld, float aStep, float aWidth)
{
    float3 cell = aWorld / aStep;
    float3 width = max(fwidth(cell), 1e-5);
    float3 distance = abs(frac(cell - 0.5) - 0.5) / width;
    return 1.0 - saturate(distance / aWidth);
}

float4 main(LitPixel input) : SV_Target0
{
    DebugPixel pixel;
    pixel.position = input.position;
    pixel.uvRG = input.uvRG;
    pixel.BA = input.BA;
    float4 color = DebugColor(pixel);

    float3 normal = cross(ddy(input.world), ddx(input.world));
    float area = length(normal);
    if (area > 1e-8)
    {
        normal /= area;
        float3 light = normalize(float3(0.35, 0.25, 0.9));
        color.rgb *= lerp(0.35, 0.55, normal.z * 0.5 + 0.5) + 0.6 * saturate(dot(normal, light));

        // An axis's lines only show on faces it runs across: none on a face it is normal to.
        float3 across = saturate((1.0 - abs(normal)) * 4.0 - 0.4);
        float step = max(spacing.x, 0.01);
        float3 minor = GridLines(input.world, step, 1.0) * across;
        float3 major = GridLines(input.world, step * 10.0, 2.0) * across;
        float strength = max(max(minor.x, minor.y), minor.z) * 0.55;
        strength = max(strength, max(max(major.x, major.y), major.z));
        color.rgb = lerp(color.rgb, lineColor.rgb, strength * lineColor.a);
    }
    return Premultiply(color);
}
