// Material parameter test: GlowColor (cb4 row 1) pulsing at Speed (cb4 row 0) pulses per second.
#include "debugdraw.hlsli"

cbuffer ShaderSpecificConstants : register(b4)
{
    float4 speed;     // register 0, scalar in x
    float4 glowColor; // register 1
};

float4 main(DebugPixel pixel) : SV_Target0
{
    float4 color = DebugColor(pixel);
    float pulse = 0.5 + 0.5 * sin(globalTime.x * speed.x * 6.2831853);
    color.rgb = lerp(color.rgb * 0.25, glowColor.rgb, 0.35 + 0.65 * pulse);
    return Premultiply(color);
}
