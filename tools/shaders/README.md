# Custom pixel shaders for debug-draw materials (spike)

The game compiles its shaders ahead of time into `engine\shader_final.cache` and looks them up by the material
template's **name**, never by path (see `docs/shader_research.md`). `src/red4ext/Shaders.cpp` lets a new material
name borrow a compiled template and swap in our own pixel shader, compiled from HLSL with the Windows SDK dxc.

## Files

- `debugdraw.hlsli`: the interface of the game's debug-draw pixel shaders (inputs, `cb7`, `s0`, `t77`, premultiplied
  output). A shader that keeps to it runs with the game's own vertex shader and root signature.
- `debugdraw_copy.hlsl`: the vanilla debugdraw pixel shader rebuilt from the include. In game (2026-10-30) it matched
  the default material to within 0.2 of a colour level.
- `scanlines.hlsl`: CRT-style scanlines, the first custom effect.
- `pulse.hlsl`: a glow pulsing with the engine time (`b0` `c0.x`) and a band sweeping down the screen (render target size
  `b0` `c3.xy`). In game it animated, so `GlobalShaderConsts` is bound in the debug-draw pass. Where those constants
  come from: the frame constant notes in `docs/shader_constants.md`.
- `params_test.hlsl`: material parameters. In game (2026-10-30) `cb4` (`ShaderSpecificConstants`) was bound and fed
  from the material: one float4 row per parameter at row = its `register`; a `.mt` default colour arrived exactly
  (60, 255, 60), and a `.mi` on top of the `.mt` overrode the colour and a scalar.
- `camera_test.hlsl`: `cb1` (`CameraShaderConsts`) is bound in the debug-draw pixel stage (non-zero values; the camera
  position at `c36.xyz` comes from the shader census, not yet checked numerically).
- `debugdraw_vs.hlsli`, `lit_vs.hlsl`, `lit_ps.hlsl`: custom vertex shaders. `lit_vs` is the game's debug-draw vertex
  shader (world from `cb5` rows 0..2 for the DrawBuffer factory, or from `INSTANCE_TRANSFORM0..2` for the instanced
  Debug factory, `-D DEBUG_FACTORY`; clip from `cb1` rows 0..3) that also passes the world position on in
  `TEXCOORD2`. `lit_ps` lights solids with face normals from the world position's screen derivatives. In game
  (2026-10-30) both factories' vertex shaders were replaced and spheres, boxes and thick lines drew shaded. Manifest:
  `vertex <material> <donor VS id> <file>` after the material's line, once per factory (debugdraw: DrawBuffer
  `46AB074CDBB5F2E3`, Debug `687287DC8EC613C8`). `tools/shaderc` builds them from a `// @vertex` line.
- `grid.hlsl`, `hologram.hlsl`, `fade.hlsl`: built-in effects. Grid and hologram use `lit_vs.hlsl` for the world
  position; hologram also reads the camera position. Fade reads the view depth from `SV_Position.w`, so it needs no
  vertex shader. Hologram and fade write the alpha, so they are meant for `SetMaterial(..., true)` (translucent).
- `test_ps.hlsl`: the first test (a checker that read the inputs wrong; kept as the record of what proved the swap).

## Using one

Use `tools/shaderc`: it compiles, checks the shader against the interface, makes the material and lays out the files to
ship (`red4ext/plugins/debug_draw/shaders/<mod>/` with a manifest, and an archive with the `.mt`). Debug Draw's built-in
effects in `src/game_files/` are this folder's effects built with it:

```
python -I tools\shaderc\shaderc.py --mod debug_draw tools\shaders\pulse.hlsl tools\shaders\scanlines.hlsl lit=tools\shaders\lit_ps.hlsl tools\shaders\grid.hlsl tools\shaders\hologram.hlsl tools\shaders\fade.hlsl
```

then copy `tools\shaderc\build\debug_draw\archive` and `red4ext` into `src\game_files`.

Manifest lines (any `*.txt` under `red4ext/plugins/debug_draw/shaders/`, file names relative to the manifest):

- `material <name> <donor template> <pixel shader>`: the template called `<name>` gets the donor's techniques with our
  pixel shader.
- `alias <name> <donor>`: the donor's shaders unchanged.
- `shader <id> <file>`: replaces one compiled shader by ID.
