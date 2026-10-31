// The interface of the game's debug-draw pixel shaders (renderstage_debugdraw, Debug / DrawBuffer vertex factories),
// read from the debugdraw and debugdraw_diagonal_stripes pixel shaders in engine\shader_final.cache (3.0.88).
// A replacement must keep these inputs and bindings: the engine pairs it with the game's own vertex shader and
// the one global root signature.
//
// Inputs: the vertex colour arrives split over two interpolants, red and green in TEXCOORD0.zw and blue and alpha
// in TEXCOORD1.xy, with the texture UV in TEXCOORD0.xy. Output is premultiplied: (rgb * a, a).

// Engine-wide constants, the same register in every material shader (type name GlobalShaderConsts in the DXIL).
// From the game's pixel shaders: c0.x is the animation time (174 templates animate with it), c3.xy the render
// target size in pixels.
cbuffer GlobalShaderConsts : register(b0)
{
    float4 globalTime;   // x time in seconds (inferred unit); w time of day 0..1 (inferred)
    float4 globalUnused1;
    float4 globalUnused2;
    float4 renderTarget; // xy size in pixels
};

// Camera constants (type name CameraShaderConsts, 852 bytes). Bound in the debug-draw pixel stage (seen non-zero in
// game, 3.0.88). Rows 0..3 are world to clip (the debug-draw vertex shader's transform, so known good). c36.xyz is
// the camera world position according to the shader census (1,709 pixel shaders subtract it from the world
// position), not checked in game; CameraWorldPosition() derives it from rows 0..3 instead (in game 2026-10-30: the
// hologram's rims, built on it, face the camera).
cbuffer CameraShaderConsts : register(b1)
{
    float4 worldToClip[4];
    float4 cameraUnused[32];
    float4 cameraPosition; // c36: not checked, see above
};

// The camera's world position from the world-to-clip rows: the one point whose clip x, y and w are all 0, solved
// with Cramer's rule.
float3 CameraWorldPosition()
{
    float3 a = worldToClip[0].xyz, b = worldToClip[1].xyz, d = worldToClip[3].xyz;
    float3 bd = cross(b, d);
    return -(worldToClip[0].w * bd + worldToClip[1].w * cross(d, a) + worldToClip[3].w * cross(a, b)) / dot(a, bd);
}

cbuffer MaterialModifiersConsts : register(b7)
{
    float4 materialModifiers[32]; // [1] is the colour tint the debug shaders multiply by
};

SamplerState debugSampler : register(s0);
Texture2D<float4> debugTexture : register(t77); // white for plain shapes; the glyph atlas for text (inferred)

struct DebugPixel
{
    float4 position : SV_Position; // pixel centre in render-target pixels
    float4 uvRG : TEXCOORD0;       // xy uv, zw red green
    float2 BA : TEXCOORD1;         // blue alpha
};

// What the vanilla debugdraw pixel shader returns before premultiplying: texture * vertex colour * tint.
float4 DebugColor(DebugPixel pixel)
{
    float4 vertexColor = float4(pixel.uvRG.zw, pixel.BA);
    return debugTexture.Sample(debugSampler, pixel.uvRG.xy) * vertexColor * materialModifiers[1];
}

float4 Premultiply(float4 color)
{
    return float4(color.rgb * color.a, color.a);
}
