// The interface of the game's debug-draw vertex shaders, read from debugdraw's two (3.0.88):
//   DrawBuffer factory, VS 46AB074CDBB5F2E3: local to world from cb5 rows 0..2
//   Debug factory (instanced), VS 687287DC8EC613C8: local to world from the INSTANCE_TRANSFORM0..2 inputs
// Both: world to clip from cb1 rows 0..3, a clip plane in cb1 row 50, colour and uv passed on split the way the pixel
// shaders read them (debugdraw.hlsli). Build with -D DEBUG_FACTORY for the Debug factory's variant; the inputs must
// stay exactly these, because they are the factory's input layout.

cbuffer CameraShaderConsts : register(b1)
{
    float4 cameraRows[51]; // [0..3] world to clip, [36].xyz camera position, [50] clip plane
};

#ifndef DEBUG_FACTORY
cbuffer DrawConsts : register(b5)
{
    float4 localToWorld[3]; // rows: world = (dot(row.xyz, p) + row.w) per axis
};
#endif

struct DebugVertex
{
    float3 position : POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR;
#ifdef DEBUG_FACTORY
    float4 instance0 : INSTANCE_TRANSFORM0;
    float4 instance1 : INSTANCE_TRANSFORM1;
    float4 instance2 : INSTANCE_TRANSFORM2;
#endif
};

float3 DebugWorldPosition(DebugVertex vertex)
{
#ifdef DEBUG_FACTORY
    float4 rows[3] = {vertex.instance0, vertex.instance1, vertex.instance2};
#else
    float4 rows[3] = {localToWorld[0], localToWorld[1], localToWorld[2]};
#endif
    float4 p = float4(vertex.position, 1.0);
    return float3(dot(rows[0], p), dot(rows[1], p), dot(rows[2], p));
}

float4 DebugClipPosition(float3 world)
{
    float4 w = float4(world, 1.0);
    return float4(dot(cameraRows[0], w), dot(cameraRows[1], w), dot(cameraRows[2], w), dot(cameraRows[3], w));
}

float DebugClipDistance(float3 world)
{
    return dot(cameraRows[50], float4(world, 1.0));
}
