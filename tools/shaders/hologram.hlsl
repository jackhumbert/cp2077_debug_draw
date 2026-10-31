// A hologram look for solid shapes: faces seen edge-on glow, faces seen head-on are nearly clear, so a solid does not
// hide what is behind it, with faint lines drifting up in world space. Uses the camera position (from the b1
// world-to-clip rows) and the world position from lit_vs.hlsl. The shader writes the alpha, so set it with
// SetMaterial(..., true) (translucent). Lines have no area and draw plain.
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

    float3 normal = cross(ddy(input.world), ddx(input.world));
    float area = length(normal);
    if (area > 1e-8)
    {
        normal /= area;
        float3 view = normalize(CameraWorldPosition() - input.world);
        float rim = 1.0 - saturate(abs(dot(normal, view)));
        rim = rim >= 0.0 ? rim * rim : 0.0; // NaN (a degenerate camera) reads as face-on
        float drift = frac(input.world.z * 4.0 - globalTime.x * 0.6);
        float band = smoothstep(0.85, 1.0, drift) * 0.25;
        color.rgb = color.rgb * (0.6 + 0.9 * rim) + band;
        color.a *= saturate(0.12 + 0.88 * rim + band);
    }
    return Premultiply(color);
}
