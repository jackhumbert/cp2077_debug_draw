// The debug-draw vertex shader, plus the world position for lit_ps.hlsl (TEXCOORD2).
#include "debugdraw_vs.hlsli"

struct LitVertexOut
{
    float4 position : SV_Position;
    float4 uvRG : TEXCOORD0;
    float2 BA : TEXCOORD1;
    float clipDistance : SV_ClipDistance0;
    float3 world : TEXCOORD2;
};

LitVertexOut main(DebugVertex vertex)
{
    LitVertexOut output;
    float3 world = DebugWorldPosition(vertex);
    output.position = DebugClipPosition(world);
    output.uvRG = float4(vertex.uv, vertex.color.rg);
    output.BA = vertex.color.ba;
    output.clipDistance = DebugClipDistance(world);
    output.world = world;
    return output;
}
