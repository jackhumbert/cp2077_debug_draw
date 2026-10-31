// CRT-style scanlines: every third pixel row dimmed, the rest lifted toward white a little.
#include "debugdraw.hlsli"

float4 main(DebugPixel pixel) : SV_Target0
{
    float4 color = DebugColor(pixel);
    float row = fmod(floor(pixel.position.y), 3.0);
    float3 lifted = lerp(color.rgb, 1.0, 0.25);
    color.rgb = row < 1.0 ? color.rgb * 0.35 : lifted;
    return Premultiply(color);
}
