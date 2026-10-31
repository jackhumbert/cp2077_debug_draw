// A glow that pulses once a second, with a bright band sweeping down the screen: needs the engine time (b0 c0.x).
#include "debugdraw.hlsli"

float4 main(DebugPixel pixel) : SV_Target0
{
    float4 color = DebugColor(pixel);
    float pulse = 0.5 + 0.5 * sin(globalTime.x * 6.2831853);
    float sweep = frac(pixel.position.y / max(renderTarget.y, 1.0) - globalTime.x * 0.5);
    float band = smoothstep(0.92, 1.0, sweep);
    color.rgb = color.rgb * (0.45 + 0.75 * pulse) + (0.25 * pulse + 0.6 * band);
    return Premultiply(color);
}
