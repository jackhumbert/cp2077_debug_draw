# CP2077 3.0.88 material shader engine constants

Source: all 5,437 pixel shaders and 1,000 sampled vertex shaders from `engine\shader_final.cache`, disassembled with SDK dxc. The DXIL keeps the cbuffer **type names** and the `!dx.resources` metadata (space, register, size), so the bindings below are observed, not guessed. All are in **space 0**.

## Engine cbuffers (observed: type name, register, byte size, count of PS that bind it)

| reg | DXIL type name | size | PS using it | notes |
|---|---|---|---|---|
| b0 | `GlobalShaderConsts` | 528 (33 float4) | 4,236 | per-frame globals; bound by `RenderBindManager::BindGlobalConstants` (0x14033FF10, `GpuApi::BindConstantBuffer(0, ...)`), called from `CRenderNode_BindGlobalConstants::Execute` (0x14033BD84) |
| b1 | `CameraShaderConsts` | 852 | ~3,400 | camera; `CRenderStateManagerData::GetCameraConstantBuffer`. The debugdraw VS (stripes, DrawBuffer) binds it |
| b2 | `ENV_PROBES` | 7,280 | ~400 | env probes |
| b3 | `CustomPixelConsts` | 400 | 459 | |
| b4 | `ShaderSpecificConstants` | varies | many | material parameters (.mt usedParameters) |
| b5 | `FrequentVertexConsts` (VS) | 112 | VS | per-draw object transform |
| b7 | `MaterialModifiersConsts` | 512 | 4,483 | per-draw modifiers; debugdraw tint is c1 |
| b8 | `CullObjectsCB` (VS) | 512 | VS | |
| b12 | `SharedPixelConsts` | 2,512 (0x9D0) | 2,443 | filled by `CRenderInterface::CalculateSharedConstants` (0x14229B520, `UpdateBuffer(.., 0x9D0, ..)`); bound by `BindSharedConstants` (0x142609BCC, slot 0xC) |
| b13 | `CSConstants` | 928 | ~670 | lighting/shadows |

The full struct layouts are in `psd/*.txt` (`%GlobalShaderConsts = type {...}`). `layout.py` prints the float4/component offset of every member.

GlobalShaderConsts member offsets (from the type, HLSL packing): c0..c10 float4; c11.xyz float + c11.w int; c12..c19 floats; c20.z int; c21.zw float2; c22.yzw float3; c23..c28 float4[6]; c29 float4; c30.xy int2, c30.z int, c30.w int; c31.x float, c31.yz int, c31.w float; c32.x int, c32.yzw float3.

## Identified values

Method: `flow.py` follows every `cbufferLoadLegacy` component forward (6 steps) to sin/cos/frac; `pos.py` finds direct arithmetic between SV_Position and cbuffer components; `ctx.py` lists each component's immediate consumers; `who.py` lists the templates that read a register.

| what | binding | evidence | confidence |
|---|---|---|---|
| **time, seconds** | **b0 c0.x** | Read by 174 templates (animated_emissive, device_diode, big_screen_glitch, hologram*, blackwall*, decal_glitch, ...). Feeds frac() in 1,115 PS, sin 229, cos 117, usually after `* const` (x3, x4, x15) or `+1000`. `ui_effect_glitch` (renderstage_composition_element, **DrawBuffer VF, binds only b7, b0, s0, t77**): `frac(c0.x * 2.06 * intensity)` as a hash seed. | high that it is the animation time; seconds is (inferred) from the speed constants. Whether it wraps or pauses with the game is unknown |
| secondary time-like value | b0 c0.z | 31 templates: rain, cloth_wind, cloth_tarps, glass_window_rain, hair, skin, metal_base, multilayered. Used as `frac(c0.z * 0.1 / 0.4 / 0.015)` | medium; possibly game or sim time, or wind time (inferred) |
| time of day 0..1 | b0 c0.w | skybox: `saturate((c0.w - 0.175) * 66.7)` and `saturate((c0.w - 0.86) * 66.7)`, i.e. about 04:12 and 20:38 thresholds. Also in window_parallax_interior and rain | medium (inferred) |
| c0.y | b0 c0.y | only simple_refraction(_mask) and sandevistan_trails, `c0.y != 1.0` then a multiply | unknown (perhaps time dilation, inferred) |
| **render target size, pixels** | **b0 c3.xy** | skybox: `uv = saturate(SV_Position.xy / c3.xy)` then sample. 1,047 PS do `x / c3.y`. blackbodyradiation_v2: in stereo, `if (pos.x >= c3.x * 0.5) pos.x -= c3.x*0.5` (c3.x is the full width). c3.zw are never read by PS or by the sampled VS | high for c3.xy = (width, height). The reciprocal is not in b0 |
| render target size + reciprocal | b12 c27 = (W, H, 1/W, 1/H) | `SV_Position.z * c26.x` (depth), `pos.y * c27.w` in 772 PS, `x / c27.xy` in 30 PS. CalculateSharedConstants builds `(a3, a4, 1/a3, 1/a4)` float4s | medium (inferred mapping to exact args). **No debug or DrawBuffer PS binds b12**, so it may not be bound there |
| depth linearisation | b12 c26.xy | `pos.z * c26.x + c26.y` | medium |
| **camera world position** | **b1 c36.xyz** | 1,709 PS do `fsub(worldPos, c36.xyz)` then normalise (rsqrt) | high |
| stereo eye split X | b1 c47.x | `pos.x >= c47.x ? pos.x - c47.x : pos.x` (1,822 PS) | medium |
| frame index | b0 c30.y (int) | `c30.y & 63` selects the blue-noise array slice (vehicle_lights_proxy) | medium |
| frame index | b1 c51.z (int) | `uitofp`, `& 3` (448 PS) | medium |
| stereo flag | b0 c11.w (int) | selects between the c3.x/2 and cb1 c47.x paths | low |

## Which buffers are bound for a debugdraw pixel shader

- Every retail PS compiled for the Debug / DrawBuffer vertex factories binds only b7, except: `ui_effect_glitch` (b7, **b0**), `metal_base_ui` gbuffer (b0, b1, b4, b7), `ui_default_element_stereo1` (b1, b7) and the bink shaders (b4). So **b0 is the global most likely to be bound in the DrawBuffer path**. It is used there by a composition-element pass. The renderstage_debugdraw pass itself was not proven (inferred).
- b1 is bound to the debugdraw **vertex** shader (stripes VS: b1, b5). Whether the engine sets it for the pixel stage in that pass was not proven. `BindConstantBuffer` takes a shader-stage mask.
- Risk: the reflection entry (`reflectionId`) of the donor shader may drive what the engine binds per draw. Global b0 and b1 are bound by render-graph nodes, not by the material, so they should not depend on it (inferred). If the PS reads zeros, b0 is not bound for that pass.

## HLSL to paste (debugdraw PS)

```hlsl
cbuffer GlobalShaderConsts : register(b0)      // 528 B engine buffer; declaring a prefix is fine
{
    float4 g_Time;      // c0: x = time (s), z = 2nd time (inferred), w = time of day 0..1 (inferred)
    float4 g_c1;        // unknown
    float4 g_c2;        // unknown
    float4 g_RTSize;    // c3: xy = render target size in pixels (zw unused by the game)
};

cbuffer MaterialModifiersConsts : register(b7) // existing debugdraw buffer
{
    float4 mm[32];      // mm[1] = colour tint
};

// optional, may not be bound to the PS in the debugdraw pass:
// cbuffer CameraShaderConsts : register(b1) { float4 cam_pad[36]; float4 g_CameraPos; /* xyz */ };

Texture2D    t77 : register(t77);
SamplerState s0  : register(s0);

float4 ps_main_(float4 pos : SV_Position, float4 c : TEXCOORD0, float2 uv : TEXCOORD1) : SV_Target
{
    float2 suv = pos.xy / g_RTSize.xy;                        // screen UV
    float  p   = 0.5 + 0.5 * sin(g_Time.x * 6.2831853);       // 1 Hz pulse
    return t77.Sample(s0, uv) * c * mm[1] * float4(p.xxx, 1);
}
```

Keep the input signature the same as the donor (SV_Position, TEXCOORD0 xyzw, TEXCOORD1 xy).

## Files (scratch\cb)

- `extract.py`: dumps every PS to `ps\<id>.dxbc` plus `psmap.txt` (id, reflectionId, templates) and `vsmap.txt`.
- `psd\*.txt`: dxc disassembly of all 5,437 PS. `vsd\` holds 1,000 sampled VS.
- `flow.py`, `pos.py`, `ctx.py`, `find.py`, `show.py`, `who.py`, `layout.py`: the analysis scripts (run with `python -I`, from this folder).
- `csc.txt`: IDA pseudocode of `CalculateSharedConstants`. `ugb.txt`: `UpdateGlobalBindings`.

Not done: the CPU-side writer of GlobalShaderConsts c0 was not found, so "seconds" and wrap or pause behaviour are not checked on the engine side. A quick runtime check would be to output `frac(g_Time.x)` as colour and see a 1 s ramp.
