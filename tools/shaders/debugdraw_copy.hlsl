// The vanilla debugdraw pixel shader rebuilt from debugdraw.hlsli: should look exactly like the default material.
#include "debugdraw.hlsli"

float4 main(DebugPixel pixel) : SV_Target0
{
    return Premultiply(DebugColor(pixel));
}
