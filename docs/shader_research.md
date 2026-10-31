# Community shader compiler for Cyberpunk 2077 3.0.88 ("2.5"): feasibility

Date: 2026-10-30. Game: the Steam release (exe FileVersion 3.0.5813403), with an IDA database of the 2.5 executable.

Grades: **(observed)** means I read it from the files, the exe or the IDB in this session. **(inferred)** means I reasoned it out and did not check it. **(prior work)** means it comes from someone else's published notes and I did not re-check it.

The research used these throwaway scripts and dumps (not kept in this repo):
- `walk.py`: walks the shader blobs.
- `t1.py`: parses the technique table and writes `t1_records.txt` (one line per compiled technique) and `mats.txt` (the 402 template names).
- `lo2.py`: reproduces the lookup key.
- `exestr.py`: string scan of the exe.
- Dumps: `stripes_ps.dxbc` / `.txt`, `debugdraw_ps.dxbc` / `.txt` (dxc disassembly), and `test_ps.hlsl` / `test_ps.dxbc` (a test shader compiled with the Windows SDK dxc).

Nothing was written to the game folder or to any repo.

---

## 0. Bottom line

| Path | Verdict |
|---|---|
| (a) Replace an existing shader's bytecode | **Feasible.** The cache format is fully decoded (observed). The engine looks up shaders by a 64-bit ID through a virtual call, so a RED4ext hook can swap the bytes. A public mod already does this in shipping code: dariulone's cyberpunk-vr-port replaces dozens of game shaders ("twins") at runtime (prior work). A stock Windows SDK `dxc` produces a container with the same part list and signature layout as the game's shaders (observed). |
| (b) A brand-new shader that a new .mt references | **Feasible only with a RED4ext plugin.** It is not possible natively. The engine opens exactly one material cache, `engine\shader_final.cache` (observed). A technique is found by a key built from the template's `name` CName plus the permutation, not from its depot path (observed: the key is reproduced for all 20,716 records). A new name has no records, so the template draws nothing. A plugin that hooks `GetMaterial` / `GetShader` / `GetReflectionParams` on the cache reader can serve a side cache. |
| (c) An end-to-end "compiler" | **Feasible as a moderate-sized project.** The hard part is not compiling. The hard part is matching the engine's implicit ABI: the one global root signature, the engine constant buffers (whose reflection names are stripped), the material `cb4` packing, and the vertex-factory interpolators. Start with pixel shaders only, reuse the game's vertex shaders, and copy the signatures from the shader being replaced. |

**How a shader is found (observed).**
- The lookup key is `fold32(FNV1a64(template.name)) << 32 | FNV1a32(u32 permutationFlags, u32 techniqueIndex, passName) stepped once with the passIndex byte`.
- The depot path plays no part in it.
- The shaders live in `engine\shader_final.cache`, in the technique table at file offset `0xA3A5A15`.
- So a new `.mt` at a new path gets shaders only if:
  - its `name` equals an existing compiled template name (it then reuses that template's shaders exactly, which is why `debugdraw_diagonal_stripes.mt` looks different from `debugdraw.remt` even though only `name` differs), or
  - a plugin serves records for its name.

  Nothing else gives it shaders.
- `selection`, `debugdraw_wireframe` and `overdraw_seethrough` have **zero** records in the retail cache.
- `emissive_basic_transparent`, `hologram_v2` and `emissive_control` have records, but none for `renderstage_debugdraw` with the `Debug` / `DrawBuffer` vertex factories.

That explains all six "loaded but drew nothing" results.

---

## 1. Earlier work

There were no earlier notes on `shader_final.cache`, DXIL or techniques. What exists:

- A `CMaterialTemplate` vtable and offset skeleton from earlier reversing.
  - Fields: `parameters` at 0xE0, `usedParameters` at 0x110, `samplerStates` at 0x140, `techniques` at 0x170, `name` at 0x188, `materialType` at 0x19A.
  - Methods: `GetShaderName`, `GetNumTechniques`.
- Generated per-class headers for `IShaderCache`, `CShaderCacheReadonly`, `ShaderEntry`, `ShaderReflectionParamsEntry`, `StaticShaderEntry`, `rend::PoolShaderCompilation` and others, from the symbol work.
- RTTI and non-RTTI type lists of the 2.5 executable name the related types.
- The RED4ext.SDK headers give the layouts (`include/RED4ext/Scripting/Natives/Generated/`):
  - `MaterialTechnique`: `passes`, `featureFlagsEnabledMask`, `streamsToBind`.
  - `MaterialPass`: `stagePassNameRegular` / `Discarded`, depth-stencil / rasterizer / blend modes, `enablePixelShader` at 0x67.
  - `MaterialUsedParameter`: `name`, `register_` at 0x08.
  - `CMaterialTemplate`: as above.

  No `.mt` field names a shader (this matches the template JSON from WolvenKit).

## 2. The compiled shaders on disk (observed)

| File | Size | Footer | Contents |
|---|---|---|---|
| `engine\shader_final.cache` | 176,912,852 B (2026-10-23) | ends `RDHS` + u32 `10` | 20,109 material programs, 20,716 technique records, 1,030 reflection sets, 402 template rows, 179 include files |
| `engine\staticshader_final.cache` | 16,192,446 B | ends `RDHS` + u32 `8` | 906 engine "static" shaders (post-FX, compute, 78 `RDAT` = DXR libraries), named (`m_GaussianBlurSeparable`, `m_downsample_SquarePow2_2channels_Array`, ...) |

There are no other `.cache` shader files. `r6\cache` holds scripts and tweakdb only, and `ep1\` has no shader cache.

**Container.** Every material program is a **DXBC-container DXIL** (magic `DXBC`, then a `DXIL` part).
- Parts: `SFI0 ISG1 OSG1 PSV0 HASH DXIL`.
- There is no `RTS0` (no embedded root signature) and no `RDEF` / `STAT` / `ILDB` (reflection and debug info are stripped).
- The digest at +4 is non-zero, so the containers are validator-signed.
- Programs: 14,672 vertex shaders and 5,437 pixel shaders, all SM **6.0**.

The game has no DXBC (SM5 `SHEX` / `SHDR`) programs. `dxilconv7.dll` in `bin\x64` belongs to the D3D12-on-Win7 path and is not a compiler (inferred).

### 2.1 `shader_final.cache` layout (observed; every count and offset matches exactly)

```
[0 .. 0xA3A5A15)        ShaderEntry x 20109:  u64 shaderId, u64 reflectionId, u32 size, DXBC[size]
[0xA3A5A15, +0x4F96CB)  MaterialEntry (technique) x 20716:
                          u64 key, String desc ("<name> CompiledTechnique [Index, Pass, PassIndex, Fallback, RenderStageContext [ID, VF; flags]]"),
                          u32 key.lo, u64 vsId, u64 psId (0 = none), u64 m, u64 m (always equal; 422 distinct),
                          u64 timestamp-like (0x7EA4A000_00xxxxxx), u32 key.hi, u32 n1, u64[n1], u32 n2, u64[n2]
[0xA89F0E0, +0x15E02)   ShaderReflectionParamsEntry x 1030 (MatMod_* slot names + small ints)
[0xA8B4EE2, +0x12DC)    u32 count=402, then (u32 templateKey, u64 timestamp-like)   = one row per template
[0xA8B61BE, +0x17A6)    179 include files: name + u64 hash (vertexFactory*.fx, include_*.fx, scaffolds\*_scaffold_ps.fx ...)
footer (0x68 B) + "RDHS" 0x0A:
                          u32 20109, u32 20716, u32 1030, u64 0x3261C0736C6DB2B5 (includes CRC? inferred), u64 date-like, u32 179,
                          u64 blobsEnd, then four (size, offset) pairs for the four tables
```

Cross-checks (observed):
- All 38,703 non-zero VS/PS IDs in the technique records resolve to a blob, and all 20,109 blobs are referenced.
- All 1,030 distinct blob `reflectionId`s occur in the reflection table.

Each string length is a CR2W-style varint: bit 7 is an ASCII flag, bit 6 means more bytes follow.

### 2.2 The lookup key (observed, checked in the exe and against the data)

`CRenderMaterial::CreateTechnique` (0x140575D4C):

```c
lo  = FNV1a32(u32 packedFlags) ; FNV1a32(u32 techniqueIndex) ; FNV1a32(passName chars)   // red::CalculateHash32, seed 0x811C9DC5
lo  = (lo ^ passIndexByte) * 16777619
hi  = *(u32*)(CRenderMaterial + 0x128)  // = IMaterialDataProvider slot 2 = IMaterialDefinition::GetHash (0x140573A98)
                                       //   = lo32 ^ hi32 of a 64-bit value from vslot 0x150 (CName hash of `name`)
key = hi << 32 | lo
GShaderCache->reader->GetMaterial(key)   // vtable +0x28 -> CShaderCacheReadonly::GetMaterial 0x140575FB8, HashMap<u64,u32>
```

- `packedFlags` is the number the record label prints as `RenderStageContext: [ID: n]`. For example, 160 for `DrawBuffer` and 152 for `Debug`, which suggests `vertexFactory << 3 | Dismembered·4 | Discarded·2 | PreSkinned·1`. That bit split comes from prior work, and it fits 160 = 20<<3 and 152 = 19<<3.
- `lo2.py` reproduces the low word for **20,716 of 20,716** records.
- `hi = fold(FNV1a64(name))` matches 20,398 of 20,716. The other 318 belong to four templates whose CName differs from their file stem. Two I confirmed: `silverhand_props_overlay` is named `silverhand_overlay` (`0xD218B115`), and `water_plane` is named `water_test` (`0x7CD1600C`). The names behind `mq061_hologram_glitch_size30` and `q305_thunderstorm_lightning` were not found.
- So the key depends on the template **name**, the technique index, the pass name and index, and the vertex factory and its flags. It does **not** depend on the file path or on the material parameters.

After a hit, `sub_14229FDA0` does the following:
1. Reads `MaterialEntry+8` (VS) or `+16` (PS).
2. Calls `reader->GetShader(id)` (vtable +0x20, 0x14057608C).
3. Creates the GPU shader from `ShaderEntry+16` (the bytecode).
4. Calls `reader->GetReflectionParams(ShaderEntry+8)` (vtable +0x58, 0x140576100) and merges the material-modifier slots.

On a miss, `GetMaterial` returns 0, `CreateTechnique` returns null, and the pass is not bound. That is the "loads but draws nothing" behaviour (observed in code; confirmed in game).

**Which debug-stage looks exist.** These are the only templates with compiled `renderstage_debugdraw` / `renderstage_debugdraw_wireframe` permutations for the `Debug` / `DrawBuffer` vertex factories:
- `debugdraw`, techniques 0, 1 and 3
- `debugdraw_diagonal_stripes`, techniques 0, 1 and 3
- `font`, techniques 0 and 1
- `ui_panel`, techniques 0 and 1

Other templates have `Debug` / `DrawBuffer` permutations for other stages: `metal_base_bink`, `metal_base_ui`, `refraction`, `parallaxscreen_transparent_ui*`, `bink_simple`, `optical_camouflage`, and the `ui_*` composition elements. The full list is in `t1_records.txt`.

The debugdraw-family pixel shaders share one interface:
- Inputs: `SV_Position`, `TEXCOORD0` float4 (colour) and `TEXCOORD1` float2 (uv).
- Output: `SV_Target0`.
- Bindings: `cb7`, `s0` and `t77`. `t77` is presumably a fixed engine texture slot (inferred). The stripes PS is 2,680 B and the plain debugdraw PS is 2,600 B; both use reflection set `0x7F8A166FCB24DD05`.

### 2.3 Loader and engine entry points (3.0.88 IDB, observed)

| Address | Symbol |
|---|---|
| 0x14052C898 | `CShaderCacheResolver::Init`: `GetEngineRoot() + "shader_final.cache"` → `IShaderCache::CreateReadOnly` → stored at `GShaderCache+0x10`. Only one path; no mod or DLC merge. |
| 0x143BCDC08 | `GShaderCache` (CShaderCacheResolver*) |
| 0x14052C9C4 / 0x14052CC38 / 0x14052D980 | `CShaderCacheReadonly::Initialize`, `CShaderCacheAsyncLoader::StartLoading` / `FinishLoading` |
| 0x14052D0FC / 0x14052D410 / 0x14052D66C | `SerializeShaderEntries` / `SerializeMaterialEntries` / `SerializeShaderReflectionEntries` |
| 0x140575FB8 / 0x14057608C / 0x140576100 | `GetMaterial(u64)` / `GetShader(u64)` / `GetReflectionParams(u64)`, all virtual |
| (names only) | `AddMaterial`, `AddShader`, `AddReflectionParams`, `GetIncludeFileCRC`, `IShaderCache::GetCurrentIncludesCRC`: editor-side writers, still in the retail exe |
| 0x140575D4C | `CRenderMaterial::CreateTechnique` (builds the key) |
| 0x1405734B0 | `CRenderMaterial::CacheEngineData` (stores the name hash at +0x128) |
| 0x14229FDA0 | Loads the VS/PS for a MaterialEntry |
| 0x140F26270 | `GpuApi::RootSignatureBuilder::GetDefaultRootSignatureDefinition` |
| (names only) | `GpuApi::CreateGraphicsRootSignature` / `CreateGraphicsRootSignature1`: the engine builds one global root signature at runtime and serialises it with `D3D12SerializeRootSignature` / `SerializeVersionedRootSignature` |
| 0x1427FDF08 | `RenderEngineHandler::ReloadEngineShaders`; the cvars `AllowShaderCacheReload` and `ShaderCacheReloadTimeCheck` exist |

## 3. Is there a runtime compiler? (observed)

- `Cyberpunk2077.exe` has no `D3DCompile*`, `d3dcompiler_*`, `dxcompiler`, `DxcCreateInstance` / `IDxc*`, `dxil.dll` or `hlsl` strings.
- It does not statically import any D3D DLL: `d3d12.dll` is loaded by name, and the only static D3D-related import is `amd_fidelityfx_loader_dx12.dll`.
- No compiler DLL ships in `bin\x64` or in `tools\redmod\bin`.
- Strings such as `ShaderCompiler`, `staticShaderCompiler` and `PoolShaderCompilation` are memory-pool and RTTI names. `redMod.exe` contains the same shader-cache classes, but REDmod does not compile shaders (inferred: it ships no compiler DLL and no `.fx` sources; I found no `.fx` / `.hlsl` anywhere in the install).
- Conclusion: shaders are only ever loaded precompiled, by 64-bit ID. The engine compiles **PSOs** at runtime (`GamePipelineLibrary.cache`, `CompilePSOAsync`, per prior work) but never shaders.

## 4. Prior community work

- **axefrog/xf-studio**, `research/materials/xf-shaders-pipeline-trace.md` (public, game 2.31, Sept 2026): https://github.com/axefrog/xf-studio/blob/main/research/materials/xf-shaders-pipeline-trace.md. The closest prior work by far: the same goal, custom material shaders.
  - It decodes RDHS v10 and reproduces the key for 19,647 of 19,647 records (2.31), with the same formula found here.
  - It traces template → technique → GpuApi shader (deduplicated by FNV-1a-32 of the bytecode) → PSO cache → `ID3D12PipelineLibrary`. It also finds one global graphics root signature.
  - It lists what a replacement shader must match.
  - It proposes route 1: hook `FindTechnique` / `GetProgram` (our `GetMaterial` / `GetShader`) and serve a renamed template's pixel shader from a side cache in the same format.
  - Its verdict: native loading of new shaders is a no-go, and a plugin side cache is a go. Its findings were all offline, "nothing observed in a running game", at that time.
  - Script: `research/materials/shader-system/exe_shaders.py`, `shader_cache.py`.
  - One small disagreement: it says the label's `RenderStageContext ID` is not in the key. In 3.0.88 the label ID is exactly the packed-flags word that is hashed first. This is probably only a labelling difference.
- **dariulone/cyberpunk-vr-port** (public, ~440 stars, pushed 2026-10-27): https://github.com/dariulone/cyberpunk-vr-port. Its RED4ext plugin `CyberpunkVR_Stereo` replaces game shaders at runtime with patched "twins" (fog, SSR, AO, particles, screen FX, world markers).
  - Each twin is `<FNV-1a-64 of original bytecode>.dxil`, a signed container generated by Python recipes from the shipped DXIL.
  - `tools/patchday/check_shaders.py` re-indexes them after each patch.
  - This is shipped proof of path (a) for both caches.
- **WolvenKit**, `WolvenKit.RED4/Archive/IO/ShaderCacheReader.cs` and `Archive/Shaders/ShaderStructs.cs`, found via GitHub code search; I did not read them. They are the original public decode of the footer and regions. xf-studio notes that its pixel/vertex labels are swapped.
- **dimkatet/REDGraph**, `sdk/tools/rg_shader_cache.py`: another cache reader.
- **flibdev/Shaderpunk** ("tool to modify the compiled shader cache files"): it appears in search, but the repo now returns 404.
- **RED modding wiki** (https://wiki.redmodding.org/cyberpunk-2077-modding/developers/shaders and `.../for-mod-creators/materials/shaders`): covers .mt / .mi usage only. Nothing on compiling.

## 5. Feasibility in detail

### (a) Replace an existing shader's bytecode

- **Hook route (recommended).** Detour `CShaderCacheReadonly::GetShader` (vtable +0x20), or patch the returned `ShaderEntry` once after load. When the ID matches, return an entry whose bytecode pointer and size point at our blob. The blob must stay alive for the whole process: GpuApi keeps the pointer (prior work).
- **File route.** Rewrite `shader_final.cache` with a new blob and recomputed offsets and footer. This is technically possible: prior work found only the `RDHS` magic checked, and I did not check for other checks. But it means a 177 MB overwrite that every patch reverts and that cannot be combined with other mods. It is useful only for a one-off experiment, on a copy, with a backup.
- Constraints:
  - The same `ISG1` / `OSG1` as the original.
  - Only registers that exist in the global root signature.
  - SM 6.0 and validator-signed.

  I checked that `dxc.exe` 10.0.26100 (`C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\`, with `dxil.dll` beside it) compiles `test_ps.hlsl` to a 2,556 B signed container. It has the identical part list and the same input/output signature as the stripes PS. Whether the runtime accepts the newer validator version is (inferred) yes on Win11 and was not tested.
- Caveat: a shader ID can be shared across permutations of the same template. Changing it changes every permutation of that template that uses it (inferred).

### (b) A brand-new shader referenced by a new .mt

- Natively impossible: one cache, keyed by name, no mod path (observed). The `.mt` itself carries no shader reference, only `name`, techniques, passes (PSO state and stage names), parameters with registers, sampler states and feature flags.
- **Zero-code workaround:** a new `.mt` at a new path whose `name` is an existing template's name reuses that template's shaders, and its techniques and passes must line up with the original's. This matches the stripes result. It gives new PSO state (blend, depth) and new defaults, but no new shader code.
- **With a plugin:**
  1. Detour `GetMaterial`. When `key.hi` equals the fold of our CName, either return records from our side cache, or map the key onto a donor template (`key.hi` → the donor's fold) and swap in our PS ID.
  2. Detour `GetShader` and `GetReflectionParams` for our IDs.

  Everything downstream (GpuApi deduplication, the PSO cache, the pipeline library) keys on bytecode and state hashes, so a new shader separates by itself (prior work). The hooks are cold: they run once per (template, permutation) on a technique-map miss.

### (c) What a compiler tool needs, end to end

1. **Authoring.**
   - HLSL entry points plus an engine include that recreates the implicit ABI:
     - the engine constant buffers (`cb0` globals, `cb1`, `cb7` debug-draw constants, `cb12`, ...; names per prior work)
     - material parameters in `cb4`: one 16-byte register per `usedParameters` entry, textures as bindless indices, `t0 space1`, per prior work
     - fixed texture slots such as `t77`
     - sampler slots `s0..`
   - The engine reflection names are stripped (observed), so these layouts must come from disassembling the originals with `dxc -dumpbin`, which works on the extracted blobs, and from runtime dumps.
2. **Interface matching.** The tool reads the donor permutation's VS `OSG1`, the PS `ISG1` / `OSG1` and the PSV0 resource list. It refuses a compiled PS whose signatures differ or whose bindings are outside the donor's set (plus the template's `cb4` registers). Pixel shaders come first: reuse the game's vertex shaders, because each vertex factory has its own input layout and interpolators. Custom vertex shaders are a later phase.
3. **Compile.** `dxc -T ps_6_0 -Qstrip_reflect -Qstrip_debug`, signed by `dxil.dll`. Optionally emit permutations per vertex factory, Discarded and Dismembered, because a PS may differ per flag.
4. **Records.** Compute the keys (§2.2) for every (technique, pass, passIndex, VF / flags) the new `.mt` declares. Write MaterialEntry rows: VS ID = donor VS, PS ID = new, `m` and the extras copied from the donor. Write ShaderEntry rows that reuse the donor's reflection set. Pack the result as an RDHS-v10-style side cache.
5. **Delivery.**
   - A RED4ext plugin, with functions resolved through the address library like ArchiveXL does, loads side caches from a mod folder and serves them through the three hooks.
   - The `.mt` / `.mi` go in a normal `.archive`. ArchiveXL is not needed for shaders; it is only the model for the hooking pattern.
   - The plugin checks the game version and the include hashes against the shipped cache, and refuses stale packs.
6. **Tooling.** A WolvenKit-JSON `.mt` template generator (copy the donor's techniques and passes, set a new `name`, add parameters) and a validator.

## 6. Hardest unknowns, and the cheapest experiment for each

| # | Unknown | Cheapest experiment that proves or kills it |
|---|---|---|
| 1 | Does a runtime bytecode swap render (validator version, signature linkage, PSO creation)? | RED4ext detour on `CShaderCacheReadonly::GetShader` (`0x14057608C` in this build; resolve by address-library ID). For ID `0x280A0CDD024C3125` (the stripes PS, `DrawBuffer` / `Debug`, technique 1), return `test_ps.dxbc` (8 px checker × vertex colour). Draw the debug shapes that already work with `debugdraw_diagonal_stripes.mt`. A checker means path (a) is proved. About 50 lines of code. |
| 2 | Can a new name get shaders (path b)? | A copy of `debugdraw_diagonal_stripes.mt` with `name = jack_test`. With no plugin it should draw nothing (confirms the miss). Then detour `GetMaterial`: if `key.hi == fold(FNV1a64("jack_test"))`, replace hi with `0xB5F1EADF` (the stripes key) and call the original. It should draw stripes. Then also swap the PS ID to the test blob. |
| 3 | The global root signature and the engine constant-buffer contents (names stripped) | At runtime, hook or read `GpuApi::CreateGraphicsRootSignature` / the definition from `GetDefaultRootSignatureDefinition` (0x140F26270) and dump the parameter table. Offline, cross-reference `dxc -dumpbin` bindings across many shaders. |
| 4 | `cb4` packing for new parameters | Add one float parameter to the jack_test template's `usedParameters` at a free register. Have the test PS output it as colour and set it from a `.mi`. |
| 5 | Custom vertex shaders / vertex factories | Defer. First prove (1) and (2) with game vertex shaders. Then try a VS that recompiles a disassembled donor VS unchanged and check that it still renders. |
| 6 | Patch fragility | The key recipe and the table layout matched between 2.31 (prior work) and 3.0.88 (this session). Hook by address-library ID and gate on the RDHS version. Expect to re-validate the twins each patch, as the VR port does. |
| 7 | Hidden integrity checks on the file route | Only matters if (1) is done by patching a copy of the cache; check by trying it on a backed-up install. I did not look beyond the loader entry points. |

Not determined: the meaning of the record fields `m` (422 distinct), the `n1` / `n2` arrays and the footer's unknown words; the exact layout of the reflection-table entries; the names behind two of the renamed CNames; whether a `Fallback` technique path exists for misses (all retail records have Fallback 0); whether `GamePipelineLibrary.cache` persists in retail.
