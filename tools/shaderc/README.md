# shaderc: Debug Draw shader compiler

`shaderc.py` turns HLSL pixel shaders into a mod folder that Debug Draw's RED4ext plugin serves. Each `.hlsl` is one
effect, named after its file stem (or `name=file.hlsl`). One mod name covers the whole run.

For each effect it:

1. compiles it with dxc: `-T ps_6_0 -E main -O3 -Qstrip_reflect -Qstrip_debug`, with the input's folder and
   `include\` on the include path;
2. compiles it again without stripping, runs `dxc -dumpbin` and checks it against the debug-draw interface (below);
3. builds a material template from the game's `engine\materials\debugdraw.remt` whose `name` is `<mod>_<effect>`.
   The game finds compiled shaders by that name, never by the path. The tool refuses a name that a vanilla
   `.mt` / `.remt` already uses, and a name whose 32-bit fold (lo32 ^ hi32 of FNV-1a 64) equals a vanilla template's.
   The shader's `// @param` lines become the template's parameters;
4. writes a material instance (`.mi`) for each `// @instance` line;
   with a `// @vertex` line, also builds and checks that vertex shader for both debug-draw vertex factories;
5. packages the effects (below).

The template is extracted and serialized with WolvenKit once, then cached in the work folder. The listing of
vanilla templates is cached there too (base game and Phantom Liberty). Both are rebuilt when the game's archives
change.

## Requirements

- Windows 10 SDK `dxc.exe`, with `dxil.dll` beside it so the blobs are signed. By default the newest
  `C:\Program Files (x86)\Windows Kits\10\bin\*\x64\dxc.exe` is used; `--dxc` overrides it.
- WolvenKit 10 CLI (`WolvenKit.CLI.exe`, 10.0.0-beta.1 tested), with the game's `bin\x64\oo2ext_7_win64.dll` copied
  next to it. Put it on `PATH`, or use `--wolvenkit` or `WOLVENKIT_CLI` to point at it.
- A Cyberpunk 2077 install, to read the template and the vanilla names: `--game`, else `CYBERPUNK_2077_GAME_DIR`, else
  the Steam path `D:\Games\SteamLibrary\steamapps\common\Cyberpunk 2077`. Nothing is written into it.
- Python 3, standard library only.

`--check` needs only dxc.

## Usage

```
python -I tools\shaderc\shaderc.py --mod <name> [--game <dir>] [--wolvenkit <exe>] [--dxc <exe>] [--out <dir>] effect1.hlsl effect2.hlsl ...
python -I tools\shaderc\shaderc.py --mod <name> --check effect1.hlsl ...
```

- `--mod`: `[a-z0-9_]`. It is the prefix of every template name, so pick something only your mod uses.
- `--out`: the output folder (default `tools\shaderc\build\<mod>`). Work files (unstripped builds, dumpbin text,
  caches, staging) go to `<out>.work`, outside the output folder.
- `--check`: compile and validate only. It uses no WolvenKit and writes no output. Names are checked against each
  other only, not against the game.
- An argument `name=path.hlsl` gives that effect a name other than its file stem.

Errors are printed per file. The exit code is non-zero if anything failed, and nothing is packaged if any effect
failed. A summary table ends every run: effect, template name, its fold, blob size, parameter and instance counts,
then the archive listing.

Debug Draw's built-in effects:

```
python -I tools\shaderc\shaderc.py --mod debug_draw tools\shaders\pulse.hlsl tools\shaders\scanlines.hlsl lit=tools\shaders\lit_ps.hlsl tools\shaders\grid.hlsl tools\shaders\hologram.hlsl tools\shaders\fade.hlsl
```

A shader with parameters and an instance: `examples\params_demo.hlsl` (`--mod shaderc_demo`).

Tests for the pure-Python parts (hashing, dumpbin parsing, validation, annotations, parameter layout, .mt / .mi
JSON): `python -I -m unittest` in `tools\shaderc`.

## The interface a shader must follow

Include `debugdraw.hlsli` (the copy in `include\` is identical to `tools\shaders\debugdraw.hlsli`) and write
`float4 main(DebugPixel pixel) : SV_Target0`. The game pairs the pixel shader with its own debug-draw vertex shader
and its one global root signature. So the validator requires:

- inputs: exactly `SV_Position` (register 0), `TEXCOORD0` float4 (register 1: uv in xy, red and green in zw) and
  `TEXCOORD1` float2 (register 2: blue and alpha). Unused inputs must still be declared. With your own vertex shader
  (`@vertex`, below) this becomes: every input must be written by it instead;
- output: only `SV_Target0`, a float4, premultiplied (`Premultiply()`);
- bindings, all in space 0, nothing else:
  - `cb0` GlobalShaderConsts (at most 528 bytes): time in c0.x, render target size in c3.xy;
  - `cb1` CameraShaderConsts (at most 852 bytes): declared in `debugdraw.hlsli`, camera world position
    `cameraPosition` at c36.xyz. Use it from the include; do not declare b1 again;
  - `cb4` ShaderSpecificConstants: the material parameters, only with `// @param` lines (below), at most one float4
    per parameter;
  - `cb7` MaterialModifiersConsts (at most 512 bytes): the tint in `materialModifiers[1]`;
  - sampler `s0` and `Texture2D` `t77`.
- Global variables outside a cbuffer (`$Globals`) are refused: nothing fills them;
- shader model `ps_6_0` or lower.

What the constants hold: `debugdraw.hlsli` and `docs\shader_constants.md`.

Proven in game (3.0.88, 2026-10-30, with hand-made assets matching what this tool writes):
- cb0 time animates (pulse.hlsl);
- cb1 is bound in the debug-draw pixel stage, with non-zero values. That c36.xyz is the camera position comes from
  the shader census and was not yet checked against a known position;
- cb4 is filled from the material: a parameter's `.mt` default arrived exactly, and a `.mi` on top overrode a colour
  and a scalar.

### Material parameters

Declare each parameter in a comment line, in the order of its cb4 rows. The first is register 0 (row c0), the next
register 1, and so on:

```
// @param <Name> scalar <default>
// @param <Name> color <r> <g> <b> [<a>]      (0..255; alpha defaults to 255)
```

The shader declares the matching cbuffer at `b4` itself, one float4 per parameter in the same order. A scalar is in
`.x`. A colour fills the row's rgba; `tools\shaders\params_test.hlsl` uses it directly as a colour. The in-game test
read the `.mt` default back exactly; the float scale was not written down here, so check it if it matters:

```hlsl
// @param Speed scalar 1
// @param GlowColor color 60 255 60
#include "debugdraw.hlsli"

cbuffer ShaderSpecificConstants : register(b4)
{
    float4 speed;     // register 0, scalar in x
    float4 glowColor; // register 1
};
```

The tool writes them into the `.mt` like vanilla `emissive_basic_transparent.mt`:
- a `CMaterialParameterScalar` / `CMaterialParameterColor` with that `register` in `parameters`;
- a `usedParameters` entry;
- a `parameterInfo` entry in the CPU block, packed in order: scalar 4 bytes (type 5), colour 4 bytes (type 2);
- `paramBlockSize`, the total.

Only scalar and colour exist for now; other types are refused. A shader that declares `register(b4)` without
`@param` lines is refused, and so are `@param` lines without a `b4` cbuffer.

### Instances

```
// @instance <instanceName> <Name>=<value> ...
```

A scalar value is a float. A colour is `r,g,b[,a]`, comma-separated, 0..255. Each instance becomes
`<effect>_<instanceName>.mi` next to the effect's `.mt`. Its `baseMaterial` is the effect's `.mt`, and it sets only the
listed values; the others keep the `.mt` defaults. The names must be `@param` names; instance names are `[a-z0-9_]`.

### Vertex shaders

A vertex shader is only needed to pass extra data to the pixel shader (like the world position for lighting) or to
move vertices. Without one, the effect keeps the game's debug-draw vertex shader. To use one, name it in the pixel
shader, relative to the pixel shader's file:

```
// @vertex lit_vs.hlsl
```

debugdraw has two vertex shaders, one per vertex factory, sharing one pixel shader:

| factory | donor VS | inputs (the factory's input layout) | constant buffers |
|---|---|---|---|
| DrawBuffer | `46AB074CDBB5F2E3` | `POSITION0` xyz r0, `TEXCOORD0` xy r1, `COLOR0` xyzw r2 | cb0, cb1, cb5 (local to world) |
| Debug (instanced) | `687287DC8EC613C8` | the same, plus `INSTANCE_TRANSFORM0..2` xyzw r3..r5 | cb0, cb1 |

The vanilla shaders output `SV_Position` r0, `TEXCOORD0` xyzw r1, `TEXCOORD1` xy r2, `SV_ClipDistance0` x r3.

One HLSL file builds both: the tool compiles it with `-T vs_6_0 -O3` once as is (DrawBuffer) and once with
`-D DEBUG_FACTORY` (Debug). Include `debugdraw_vs.hlsli` (the copy in `include\` is identical to
`tools\shaders\debugdraw_vs.hlsli`): its `DebugVertex` switches on `DEBUG_FACTORY`, and `DebugWorldPosition`,
`DebugClipPosition` and `DebugClipDistance` do what the vanilla shaders do. Example: `tools\shaders\lit_vs.hlsl` with
`lit_ps.hlsl` (proven in game: shaded shapes from both factories).

The validator checks, with clear messages naming the `@vertex` line and the variant:
- each variant's inputs equal its factory's layout exactly (semantic, index, mask, register, float);
- the outputs include `SV_Position`, and both variants write the same outputs;
- bindings: cb0 (528 bytes max) and cb1 (852), plus cb5 (112) for DrawBuffer, in space 0; nothing else;
- `vs_6_0` or lower;
- every pixel shader input (semantic, index, register, format) is written by both variants at the same register,
  with a mask that covers it. `SV_IsFrontFace`, `SV_PrimitiveID`, `SV_SampleIndex` and `SV_Coverage` are exempt (the
  rasterizer makes them).

The output gets `<effect>.drawbuffer.vs.dxbc` and `<effect>.debug.vs.dxbc`, and the manifest gets two lines after the
effect's `material` line:

```
vertex <mod>_<effect> 46AB074CDBB5F2E3 <effect>.drawbuffer.vs.dxbc
vertex <mod>_<effect> 687287DC8EC613C8 <effect>.debug.vs.dxbc
```

The plugin swaps a vertex shader only where the donor's technique uses that ID, so each build serves its own factory.

## What the output folder holds

Laid out like the game folder: copy it over the game folder, or zip it.

```
red4ext\plugins\debug_draw\shaders\<mod>\<effect>.dxbc   one per effect
red4ext\plugins\debug_draw\shaders\<mod>\<effect>.drawbuffer.vs.dxbc, <effect>.debug.vs.dxbc   with @vertex
red4ext\plugins\debug_draw\shaders\<mod>\<mod>.txt       manifest: material <mod>_<effect> debugdraw <effect>.dxbc
archive\pc\mod\<mod>_shaders.archive                     debug_draw\shaders\<mod>\<effect>.mt and
                                                         debug_draw\shaders\<mod>\<effect>_<instance>.mi
readme.txt                                               the material path of each effect and instance, the
                                                         vertex shader files
```

The plugin reads every `*.txt` under `red4ext\plugins\debug_draw\shaders\`. A `material` line gives the template called
`<mod>_<effect>` the techniques of the donor `debugdraw`, with our pixel shader in each. To use an effect:

```
DebugDraw.SetMaterial(n"channel", "debug_draw\\shaders\\<mod>\\<effect>.mt")
DebugDraw.SetMaterial(n"channel", "debug_draw\\shaders\\<mod>\\<effect>_<instance>.mi")
```

After packaging, the tool checks the result:
- it serializes each written `.mt` and `.mi` back to JSON and compares them with what was asked for: the name,
  parameter names, registers, defaults, offsets and block size; the instance's `baseMaterial` and values;
- it lists the packed archive and makes sure every `.mt` and `.mi` is in it.

## Known limits

- Every effect keeps the donor's pipeline state (blend, depth, cull) and vertex factories. A `@vertex` shader must
  read exactly the factory's input layout; there is no way to add vertex inputs.
- The donor is always `debugdraw` (`engine\materials\debugdraw.remt`).
- Constant buffers b0, b1, b4 and b7 only. Material parameters are scalars and colours; there are no texture
  parameters (only t77, the debug texture).
- Debug Draw's RED4ext plugin must be installed for the shaders to load. Without it, the templates find no compiled
  shaders and draw nothing.
- The tool checks names only against vanilla templates and the effects of one run. Two mods that use the same
  `--mod` clash, and the plugin keeps whichever manifest it reads first.
