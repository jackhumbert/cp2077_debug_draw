# Runtime material parameters and shader hot reload (game 2.5 / exe 3.0.88 IDB)

Read-only IDA session. Hashes = Adler-32 of the mangled name, all present in
`D:\Games\SteamLibrary\steamapps\common\Cyberpunk 2077\bin\x64\cyberpunk2077_addresses.json` (checked
against that file). Grades: (observed) = read in the IDB this session; (inferred) = reasoning, not checked.

## Function table

| addr | hash | mangled name |
|---|---|---|
| 0x140FEE3E0 | 855317233 | `?WriteParameterRaw@CMaterialInstance@@UEAA_NVCName@@PEBVIType@rtti@@PEBX_N@Z` |
| 0x142F9CCB4 | 2329354740 | `?WriteParameterRaw_NoDefinitionCheck@CMaterialInstance@@UEAA_NVCName@@PEBVIType@rtti@@PEBX_N@Z` |
| 0x1426B8C28 | 874191608 | `?WriteParameterRaw@CMaterialTemplate@@UEAA_NVCName@@PEBVIType@rtti@@PEBX_N@Z` |
| 0x14032CEE8 | 3568179637 | `?ReadParameterRaw@CMaterialInstance@@UEBA_NVCName@@PEBVIType@rtti@@PEAX@Z` |
| 0x1407893C0 | 2059407427 | `?InvalidateRenderResource@IMaterial@@QEAAXXZ` |
| 0x140339AA4 | 3016235445 | `?GetRenderResource@IMaterial@@QEBA?AV?$TRenderPtr@VIRenderResource@@@@XZ` |
| 0x142609454 | 3728609626 | `?WriteParameter@IMaterial@@QEAA_NVCName@@AEBV2@_N@Z` (CName overload) |
| 0x142E592F4 | 2680363190 | `?WriteParameter@IMaterial@@QEAA_NVCName@@AEBV?$TResRef@VITexture@@@@_N@Z` |
| 0x14078947C | 382547433 | `??$CreateHandle@VCMaterialInstance@@$$TAEAV?$THandle@VCMaterialTemplate@@@@@@YA?AV?$THandle@VCMaterialInstance@@@@$$QEA$$TAEAV?$THandle@VCMaterialTemplate@@@@@Z` |
| 0x1423269E0 | 1470438272 | `??$CreateHandle@VCMaterialInstance@@$$V@@YA?AV?$THandle@VCMaterialInstance@@@@XZ` |
| 0x140577914 | 82451987 | `?CreateRenderResource@CMaterialInstance@@MEAA?AV?$TRenderPtr@VIRenderResource@@@@XZ` (vslot 0x160) |
| 0x1405779DC | 2881566596 | `?UploadMaterial@CRenderInterface@@UEAA?AV?$TRenderPtr@VIRenderResource@@@@PEBVIMaterialDataProvider@@PEBVIMaterialParametersProvider@@PEAVBuilder@job@@@Z` (CRenderInterface vslot 0x1B0) |
| 0x142E9BBE0 | 2199466548 | `?UpdateMaterial@CRenderInterface@@UEAAXAEBV?$TRenderPtr@VIRenderResource@@@@PEBVIMaterialDataProvider@@PEBVIMaterialParametersProvider@@@Z` (vslot 0x1B8) |
| 0x14057213C | 3840681550 | `?SetupParameters@CRenderMaterial@@QEAAXPEBVIMaterialDataProvider@@PEBVIMaterialParametersProvider@@_N2@Z` |
| 0x142E9BC0C | 3510573544 | `?UpdateMaterialData@CRenderMaterial@@QEAAX$$QEAUSMaterialUpdateData@@_N@Z` |
| 0x140574028 | 3824487489 | `?Cleanup@CRenderMaterial@@AEAAX_N@Z` |
| 0x1401EF7C8 | 2138515882 | `?GetBindShadersTechnique@CRenderMaterial@@AEBA?AV?$TRenderPtr@VCompiledTechnique@@@@AEBUCompiledTechniqueDesc@@_N@Z` |
| 0x140575D4C | 3718653418 | `?CreateTechnique@CRenderMaterial@@...` |
| 0x140331F84 | 1417227012 | `?GetRenderPassParamsId@CRenderMaterialParameters@@AEBA?AUTBatchDataId@@AEAVCBatchDataAllocator@@PEBVCRenderMaterial@@E@Z` |
| 0x140572364 | 2488676096 | `?CompileDataBuffer@IMaterial@@UEBAXW4EMaterialShaderTarget@@AEBVBlobSpan@red@@AEAV?$DynArray@V?$TRenderPtr@VIRenderResource@@@@@4@_N@Z` |
| 0x1426C3D80 | 2570131657 | `?BindMaterial@FragmentBuilder@rend@@QEAAXAEBV?$THandle@VIMaterial@@@@@Z` |

Other addresses (no hash needed): CRenderMaterialParameters::SetupParameters 0x140572010, ::CompileDataBuffer
0x1405720CC, ::GetDataSpan 0x14057D874, ::Cleanup 0x140534C4C; cb4 builder sub_14057DF88; SurfaceCache::CreateSurface
0x140578218; CRenderMaterial::Create 0x140571A24 (0xB00 bytes); m_techniquesMutex 0x143C16470 (CRITICAL_SECTION);
GpuApi shader create-or-dedup sub_14057114C; PSOCache::Get 0x1403E3F18; IMaterial::IMaterial(THandle<IMaterial>)
0x1406262F8; vtables CMaterialInstance(CResource) 0x143066000, CRenderInterface 0x1436CE318.

## 1. Where parameter values go (observed)

Layouts:
- IMaterial (0x78): +0x40 IMaterialParametersProvider vptr; +0x48 baseMaterial (res::ResourceReference, 0x18);
  +0x60 u8 "render resource creation failed"; +0x68 TRenderPtr<IRenderResource> renderResource (the CRenderMaterial);
  +0x70 RWSpinLock guarding +0x60/+0x68.
- CMaterialInstance (0x98): +0x78 DynArray<MaterialParameterInstance> `values`, 32 B each: +0 CName, +8 rtti::Variant
  (IType* with bit0 = inline storage, data at +0x10).

Pipeline:
1. `IMaterial::GetRenderResource` (lazy, under the +0x70 lock): if +0x68 is null and +0x60 is 0, calls vslot 0x160
   CreateRenderResource and stores the result; sets +0x60 = (result == null).
2. `CMaterialInstance::CreateRenderResource` -> `GRender->UploadMaterial(dataProvider = template+0x78,
   paramsProvider = this+0x40, builder = nullptr)` -> SurfaceCache::CreateSurface -> CRenderMaterial::Create ->
   ctor -> CRenderMaterial::SetupParameters -> CRenderMaterialParameters::SetupParameters. All synchronous on the
   calling thread (builder == null skips the async PSO prewarm).
3. CRenderMaterialParameters (base of CRenderMaterial): +0x10 UniqueBuffer CPU blob (+0x18 size), per-target start
   offsets at +0x34 (PS) / +0x38 (VS); `CompileDataBuffer` calls provider vslot +0x10 = IMaterial::CompileDataBuffer,
   which for each ParameterDefinition {CName name +0, u16 blobOffset +8, u8 type +10} calls ReadParameterRaw
   (vslot 0x1A0: the instance's `values`, else the base material chain) and writes: Color as 4 x u8, Scalar as
   float, Vector4 as 16 B, textures as resource index (pushed into a TRenderPtr array at CRenderMaterialParameters+0x40).
4. cb4 is built lazily at draw time: `GetRenderPassParamsId` caches FixedArray<ExtractedMaterialDataId,2> at
   CRenderMaterialParameters+0x3C; when the id is <= 1 it calls sub_14057DF88, which walks the template's mapped
   UsedParameters {u16 blobOffset, u8 type, u8 register} and fills `float4 Dst[register]`:
   - type 2 Color: rgb through math::g_gammaToLinearLUT (sRGB -> linear), a = byte/255
   - type 4 Vector: 16 B copy
   - type 5 Scalar: splat to xyzw
   - texture types: bindless texture-indirection index splatted as uint
   then FNV-1a-64 of Dst, a dedup cache lookup (sub_14229FA10), else `GpuApi::CreateBuffer(16*n)`. So the cb4
   buffer is an immutable, content-deduplicated GPU buffer, created once per render material and pass target.
   Changing the CPU blob alone does nothing until that cached id is dropped.
5. `rend::FragmentBuilder::BindMaterial` calls GetMaterialDefinition (vslot 0x180) -> GetRenderResource on it, then
   GetRenderResource on the material itself, every call (so every frame in our BeginFrame build).

So: the .mt parameter register == cb4 float4 index (observed in sub_14057DF88); the CPU blob offsets are the
template's ParameterDefinition offsets (IMaterial::GetParameterDefinitions 0x140571FE4), not the cb4 layout.

### Engine API to change a parameter (observed, with game precedents)

Game code does exactly this, on the game thread:
- `ent::VirtualCameraViewComponent::CreateRemoteMaterial` 0x142976E94: TStaticResource<CMaterialTemplate>::TryGet ->
  `CreateHandle<CMaterialInstance>(nullptr, THandle<CMaterialTemplate>&)` -> `IMaterial::WriteParameter(name, CName)`
  -> `IMaterial::InvalidateRenderResource`.
- `game::PhotoModeBackgroundViewComponent::UpdateBackground` 0x142E590D4: WriteParameter(texture) on a mesh's
  material -> InvalidateRenderResource -> mesh vslot 0x128(8) (mesh render-data refresh; not needed for us).
- `ink::VideoWidget::ValidateVideoPlayer` also calls InvalidateRenderResource.

`CMaterialInstance::WriteParameterRaw(this, CName name, const rtti::IType* type, const void* value, bool recreateParent)`:
finds name in `values` and `Variant::Set`s it, else requires the root template to have the parameter
(CMaterialTemplate::FindParameter) with exactly that IType, and EmplaceBacks it. Returns false if absent / type
mismatch. `recreateParent` only calls the parent CResource's vslot 0x128 (embedded-in-mesh case): pass false.
Types: CMaterialParameterScalar -> `float`, Color -> `Color` (4 x u8 RGBA, sRGB), Vector -> `Vector4`
(use CRTTISystem::GetType("Float"/"Color"/"Vector4"); the pointer must be the identical IType).
`InvalidateRenderResource` swaps +0x68 to null under the lock (does not touch +0x60). The next GetRenderResource
builds a brand-new CRenderMaterial from current values; the old one dies by refcount when in-flight frames drop it.

### Recommended route (high confidence for correctness, medium for cost)

Per channel, own a fresh CMaterialInstance wrapping the loaded .mi/.mt so edits never touch the shared resource:
```
// 0x14078947C, hash 382547433. arg2 is std::nullptr_t&&: a pointer to a null ISerializable* (the parent).
using CreateMI_t = Handle<CMaterialInstance>* (*)(Handle<CMaterialInstance>* out, void* const* parent, Handle<IMaterial>* base);
// IMaterial::IMaterial takes THandle<IMaterial>, so a .mi base works too (ReadParameterRaw chains through bases).
```
SetMaterialParam (script/game thread), all under s_lock (the same lock BeginFrame holds around BindMaterial):
```
using WriteRaw_t = bool (*)(CMaterialInstance*, CName, const IType*, const void*, bool); // 0x140FEE3E0, 855317233
using Invalidate_t = void (*)(IMaterial*);                                               // 0x1407893C0, 2059407427
write(mi, name, floatType/colorType/vector4Type, &value, false); invalidate(mi);
```
Next BeginFrame's BindMaterial recreates the render material synchronously (inside GetRenderResource).
Threading: WriteParameterRaw touches `values` without any lock and GetRenderResource reads it via
CreateRenderResource, so the write must not race a GetRenderResource of the same material. With our own instance the
only caller is our BindMaterial under s_lock, so taking s_lock suffices (inferred). Do not mutate a shared
ResourceLoader .mi/.mt that game meshes also use.
Cost per change: new CRenderMaterial (0xB00 B), new surface id, techniques re-looked-up (GetMaterial/GetShader
hooks run again; GPU shader deduped by bytecode hash), new cb4 GPU buffer. Fine for edits/tweaks; for continuous
per-frame animation prefer b0 time in the shader or throttle (inferred).

Not recommended:
- `CRenderInterface::UpdateMaterial` (0x142E9BBE0) calls CRenderMaterial::SetupParameters(rm, dp, pp, true, true)
  directly, in place, with no render command: it Cleanup()s a render object that render worker threads may be using
  this frame. Only safe at a point where nothing renders (unknown where). No caller found in the IDB by xref
  (only a vtable slot; 250+ `call [reg+1B8h]` sites not triaged).
- `CRenderCommandHandler::UpdateMaterialData` (0x142E8CD68) queues a render command "UpdateMaterialData" with an
  SMaterialUpdateData (~0x140 B, layout not decoded) -> CRenderMaterial::UpdateMaterialData on the render thread.
  Thread-safe but the struct is undecoded.
- Patching CRenderMaterialParameters+0x10 in place and zeroing +0x3C: leaks the dedup-cache refs (Cleanup releases
  them via sub_1422A0AB0) and races GetRenderPassParamsId on render workers.
- CMaterialTemplate::WriteParameterRaw changes the CMaterialParameter object's default (no recreate); other
  instances keep their already-built render materials.

## 2. Shader hot reload (observed unless noted)

Per-CRenderMaterial caches:
- +0x708 HashMap<u32, TRenderPtr<CompiledTechnique>> (key = the low 32-bit technique hash), guarded by the static
  CRenderMaterial::m_techniquesMutex 0x143C16470. GetBindShadersTechnique inserts only successful creations (or the
  fallback technique); a GetMaterial miss is retried on every lookup.
- +0x180 HashMap<u32, SCachedRenderStagePass> (OM state etc.), +0x738 HashMap<u32,SCachedRenderStagePassesRange>,
  per-stage maps at +0x1C0 + 48*i. All cleared by CRenderMaterial::Cleanup (0x140574028), which SetupParameters
  (cleanup=true) and UpdateMaterialData call.

Simplest reload: `IMaterial::InvalidateRenderResource` on each bound material (same as section 1). The new
CRenderMaterial has empty caches, so CreateTechnique -> GetMaterial (our hook) -> GetShader runs again. To get new
bytecode the hook must hand out a new synthetic shader id per reload generation: Shaders.cpp builds
`synthetic.entry` once and caches it, so re-serving the same id would keep the old pointer.
If the material is embedded in something else (mesh), the parent's render data also needs refreshing (not our case).

Bytecode lifetime: MUST stay alive. sub_14229FDA0 -> sub_1422AFF10 -> sub_14057114C stores the {pointer, size} of
ShaderEntry+0x10 in GpuApi's shader container (no copy), deduplicated by FNV-1a-32 of the bytes (up to 0x1000
slots); on a dedup hit it overwrites the stored pointer with the newest caller's. PSOs are created later at draw
time (GpuApi::PSOCache::Get 0x1403E3F18 -> CompileAsync or sync Compile), reading that bytecode. So keep every
generation's blob (append-only, never reallocate a vector a ShaderEntry points into). Our ShaderEntry copies
(0x38 B) are only read during CreateTechnique (inferred: nothing keeps the entry pointer), but keeping them is cheap.

PSO: PSOCache::Get returns the cached PSO, or kicks CompileAsync and returns pending unless a sync flag is set, so a
new shader may skip draws for a frame or two after reload (inferred from the code shape; the skip path itself not
traced). Parameter-only changes reuse the same shaders and state, so the PSO should be a cache hit (inferred).

`RenderEngineHandler::ReloadEngineShaders` (0x1427FDF08) only refreshes the include CRC; it does not reload
materials. `AllowShaderCacheReload` cvar path not traced.

## Not determined
- SMaterialUpdateData layout; who calls CRenderInterface::UpdateMaterial.
- Exact PSO-cache key (bytecode hash vs ShaderRef index) and whether a pending PSO skips the draw.
- Lifetime/eviction of the cb4 dedup cache (sub_14229FA10) under frequent value changes.
- Which thread EngineViewport::BeginFrame runs on (assumed the same thread for every call; we hold s_lock anyway).
