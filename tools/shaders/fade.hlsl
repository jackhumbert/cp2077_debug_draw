// Shapes fade out with distance from the camera: fully drawn up to Near metres, gone at Far, so dense debug output
// thins out into the distance. The distance is the view depth (SV_Position.w), so no vertex shader is needed. The
// shader writes the alpha, so set it with SetMaterial(..., true) (translucent).
// @param Near scalar 15
// @param Far scalar 60
// @instance fade_near Near=5 Far=25
#include "debugdraw.hlsli"

cbuffer ShaderSpecificConstants : register(b4)
{
    float4 nearDistance; // register 0, scalar in x
    float4 farDistance;  // register 1, scalar in x
};

float4 main(DebugPixel pixel) : SV_Target0
{
    float4 color = DebugColor(pixel);
    float fadeEnd = max(farDistance.x, nearDistance.x + 0.01);
    color.a *= 1.0 - smoothstep(nearDistance.x, fadeEnd, pixel.position.w);
    return Premultiply(color);
}
