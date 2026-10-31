// Camera constants test: colours by the camera's world position (cb1 c36.xyz, declared in debugdraw.hlsli). Black
// means cb1 is not bound in the debug-draw pixel stage.
#include "debugdraw.hlsli"

float4 main(DebugPixel pixel) : SV_Target0
{
    float3 tint = frac(abs(cameraPosition.xyz) * float3(0.37, 0.53, 0.71));
    return Premultiply(float4(0.2 + 0.8 * tint, DebugColor(pixel).a));
}
