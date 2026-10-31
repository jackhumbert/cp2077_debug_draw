#pragma once

// Engine pieces of the retail debug renderer, reversed from 3.0.88.
// Hashes are the Adler-32 "hash" column of cyberpunk2077_addresses.json; all are unique in that file.
//
// How the retail debug renderer is wired:
// - CGameEngine::ProcessFrame calls EngineViewport::BeginFrame, ticks the game, then EngineViewport::SubmitFrame.
// - BeginFrame builds the camera frame info with an EMPTY drawer, so CRenderCameraFrameInfo's ctor swaps in the
//   static NullDebugDrawer. During the tick, gsm::StateMachine::ProcessRender hands camInfo to the renderer
//   (CRenderFrameCameraStorage::UpdateCameraData copies it to cameraData+0x2460), so its drawer (+0x70) is the
//   cameraData+0x24D0 the render nodes read. SubmitFrame only copies it into frameInfo+0x158, which is too late.
// - CRenderNode_CollectDebug skips when that drawer's vfunc +8 says it is the null one, otherwise calls
//   DebugDrawerImpl::Collect, and CRenderNode_RenderDebugFragments draws the buckets with the memory-resident
//   engine\materials\debugdraw*.mt materials. Neither node, nor Collect, nor SubmitGeometry looks at the
//   DebugFilterMask, so no config var (Rendering/RenderMask/DebugDraw) is involved on this path.
// - Neither node is in the normal gameplay render graph (GraphInsert.cpp adds RenderDebugFragments), and the
//   collect is done by hand right before drawing (Collect.cpp).
// So: right after BeginFrame, hand camInfo a real drawer (CRenderInterface::CreateDebugDrawer) filled through
// rend::FragmentBuilder.

#include <RED4ext/Relocation.hpp>
#include <cstdint>

namespace Engine {

// red::SharedStorage<rend::IDebugDrawer, red::internal::AtomicSharedStorage>: 16 bytes. refs points at an 8-byte
// PoolRefCount block {strong, weak}, both 1 after AtomicSharedStorage's ctor. On the last release the dtor calls
// the drawer's vfunc 0 (scalar dtor, flag 0) and frees it into PoolRenderDebug, which is where
// CreateDebugDrawer allocated it, so the engine owns the drawer once it is wrapped.
struct DrawerStorage {
  void *drawer = nullptr;
  int32_t *refs = nullptr;
};
static_assert(sizeof(DrawerStorage) == 0x10);

// red::StringView, passed by value (MSVC hands >8-byte structs over as a pointer to a copy).
struct StringView {
  const char *data;
  uint32_t size;
};

// rend::FragmentBuilder: 0x40550 bytes, needs 16-byte alignment (movaps at +0x10). The script proxy
// (rendFragmentBuilderScriptProxy, 0x40560) is this plus an "engaged" byte at +0x40550.
constexpr size_t kFragmentBuilderSize = 0x40550;
// Current vertex color, RGBA bytes (Red at the low byte), read by every Add* that takes no Color. The script
// proxy's SetColor writes exactly this dword; there is no setter in the exe.
constexpr size_t kFragmentBuilderColorOffset = 0x4E4;

// CRenderCameraFrameInfo: the drawer storage lives at +0x70.
constexpr size_t kCameraFrameInfoDrawerOffset = 0x70;

// rend::EngineViewport: IViewport* at +0x08; IViewport width/height at +0x08/+0x0C (from CRenderFrameSetup's ctor).
constexpr size_t kEngineViewportViewportOffset = 0x08;
constexpr size_t kViewportWidthOffset = 0x08;
constexpr size_t kViewportHeightOffset = 0x0C;

// CRenderInterface::CreateDebugDrawer(const DebugFilterMask&, uint32 w, uint32 h), called directly by hash with GRender
// as this. w or h of 0 returns the null drawer.
// Called directly by hash with GRender as this, never through the IRender vtable: the slot moves between game
// versions (0x278 in older headers is a bare `ret` in 3.0.88). w,h are the drawer's virtual screen size;
// CollectImpl divides screen-space and world-text pixel positions by them, so they must be the real viewport size
// or text lands in the wrong place.

namespace Hash {
// void rend::EngineViewport::BeginFrame(CRenderFrameInfo&, CRenderCameraFrameInfo&, bool, bool)
constexpr uint32_t EngineViewport_BeginFrame = 661528847;
// const rend::DebugFilterMask& rend::EngineViewport::GetRenderingMask() const
constexpr uint32_t EngineViewport_GetRenderingMask = 1337661532;
// IRender* GRender
constexpr uint32_t GRender = 1931610229;
// rend::IDebugDrawer* CRenderInterface::CreateDebugDrawer(const rend::DebugFilterMask&, uint32, uint32)
constexpr uint32_t CRenderInterface_CreateDebugDrawer = 3379896304;
// red::internal::AtomicSharedStorage::AtomicSharedStorage(void*)
constexpr uint32_t AtomicSharedStorage_ctor = 2893943016;
// red::SharedStorage<rend::IDebugDrawer, AtomicSharedStorage>::~SharedStorage()
constexpr uint32_t DrawerStorage_dtor = 2215321511;
// red::SharedStorage<rend::IDebugDrawer, AtomicSharedStorage>::operator=(const SharedStorage&)
constexpr uint32_t DrawerStorage_assign = 892084943;
// rend::FragmentBuilder::FragmentBuilder(rend::IDebugDrawer&, uint8, int8)
constexpr uint32_t FragmentBuilder_ctor = 4016837104;
// rend::FragmentBuilder::~FragmentBuilder(): Flush, then release the font/material handles
constexpr uint32_t FragmentBuilder_dtor = 3134196510;
// rend::FragmentBuilder::Flush(): submits the pending vertices to the drawer as one fragment
constexpr uint32_t FragmentBuilder_Flush = 4163898632;
// rend::FragmentBuilder::EnableOverlay(bool): flushes and toggles the no-depth-test mode bit
constexpr uint32_t FragmentBuilder_EnableOverlay = 2534346948;
// rend::FragmentBuilder::AddLine(const Vector4&, const Vector4&)
constexpr uint32_t FragmentBuilder_AddLine = 3973190131;
// rend::FragmentBuilder::AddLines(const Vector4*, uint32 count): count is points, pairs make segments
constexpr uint32_t FragmentBuilder_AddLines = 86250126;
// rend::FragmentBuilder::AddWireBox(const Vector4& min, const Vector4& max, const Color&)
constexpr uint32_t FragmentBuilder_AddWireBox = 365369031;
// rend::FragmentBuilder::AddWireSphere(const Vector4& center, float radius)
constexpr uint32_t FragmentBuilder_AddWireSphere = 1932399750;
// rend::FragmentBuilder::AddWorldText(const Vector4&, int32 dx, int32 dy, red::StringView, float scale)
constexpr uint32_t FragmentBuilder_AddWorldText = 705305384;
// rend::FragmentBuilder::AddFlatText(int32 x, int32 y, red::StringView, float scale): screen pixels
constexpr uint32_t FragmentBuilder_AddFlatText = 3423147619;
// rend::FragmentBuilder::AddFlatLine(Vector2, Vector2): screen pixels, in the builder's colour
constexpr uint32_t FragmentBuilder_AddFlatLine = 132125360;
// rend::FragmentBuilder::AddFatLineFast(const Vector4& a, const Vector4& b, float radius): a triangular prism of
// that radius (metres) around the segment, in the builder's colour
constexpr uint32_t FragmentBuilder_AddFatLineFast = 2489980137;
// rend::FragmentBuilder::AddFatArrow(const Vector4& from, const Vector4& to, float radius): AddArrow built from
// AddFatLineFast prisms
constexpr uint32_t FragmentBuilder_AddFatArrow = 1453724638;
// rend::FragmentBuilder::AddSolidIndexedTriangles(red::ArraySpan<const Vector4>, red::ArraySpan<const uint32>, bool):
// filled triangles in the builder's colour; a span is {begin, end} and arrives as a pointer
constexpr uint32_t FragmentBuilder_AddSolidIndexedTriangles = 3333957163;
// rend::FragmentBuilder::AddSolidCapsule(const Vector4& center, float radius, float halfCylinder, bool): along world Z
constexpr uint32_t FragmentBuilder_AddSolidCapsule = 3812759114;
// rend::FragmentBuilder::AddArrow(const Vector4& from, const Vector4& to): head is the last 20 % of the length
constexpr uint32_t FragmentBuilder_AddArrow = 112857718;
// rend::FragmentBuilder::AddTransform(const Matrix&, float scale): X/Y/Z rows as red/green/blue lines, scaled
constexpr uint32_t FragmentBuilder_AddTransform = 1328813074;
// rend::FragmentBuilder::AddWireCapsule(const Vector4& center, float radius, float halfCylinder): along world Z
constexpr uint32_t FragmentBuilder_AddWireCapsule = 2672301369;
// rend::FragmentBuilder::AddWireTube(const Vector4& a, const Vector4& b, float radiusA, float radiusB,
// uint16 segments (5..1000), bool caps): without caps only the side lines
constexpr uint32_t FragmentBuilder_AddWireTube = 2857047328;
// rend::FragmentBuilder::AddSolidBox(const Vector4& min, const Vector4& max, const Color&): uses the Color argument
constexpr uint32_t FragmentBuilder_AddSolidBox = 799872811;
// rend::FragmentBuilder::AddSolidSphere(const Vector4& center, float radius, bool inside): cached sphere mesh
constexpr uint32_t FragmentBuilder_AddSolidSphere = 3036288407;
// rend::FragmentBuilder::EnableTransparency(bool): mode bit 0x2 at +0x40534, flushes when it changes
constexpr uint32_t FragmentBuilder_EnableTransparency = 4215804636;
// void AI::Utils::GetBoundingBoxesFromComponentsOfType<ent::SkinnedMeshComponent>::operator()(const ent::Entity&,
// red::DynArray<Box>&) const: appends the box at (*(component + 0x200)) + 0x40 of each skinned mesh component; the
// functor has no state
constexpr uint32_t AIUtils_GetSkinnedMeshBoxes = 919678575;
// Box AI::Utils::GetBoundingBox(red::DynArray<Box>&): min/max over the boxes ({FLT_MAX, -FLT_MAX} for none)
constexpr uint32_t AIUtils_GetBoundingBox = 3848410607;
// void rend::FragmentBuilder::BindMaterial(const THandle<IMaterial>&): flushes, then the following geometry uses
// that material's render resource (a null handle goes back to the engine's debugdraw*.mt)
constexpr uint32_t FragmentBuilder_BindMaterial = 2570131657;
// THandle<CMaterialInstance> CreateHandle<CMaterialInstance>(ISerializable* const& parent, THandle<IMaterial>& base):
// a new instance on top of base (a .mt, or a .mi whose chain is walked for parameters). The engine's own runtime
// materials (VirtualCameraViewComponent::CreateRemoteMaterial) are made this way.
constexpr uint32_t CreateHandle_CMaterialInstance = 382547433;
// bool CMaterialInstance::WriteParameterRaw(CName, const rtti::IType*, const void*, bool recreateParent): sets one
// of the root template's parameters in the instance's values; false if there is none of that name and type
constexpr uint32_t CMaterialInstance_WriteParameterRaw = 855317233;
// void IMaterial::InvalidateRenderResource(): drops the render material (its cb4 buffer and technique caches); the
// next GetRenderResource, which BindMaterial calls, builds a new one from the current values and shaders
constexpr uint32_t IMaterial_InvalidateRenderResource = 2059407427;
// Not wrapped: AddSolidCircle ignores its radius (fixed 0.065), AddWireCircle its segment count and plane (16
// spokes in XY), AddArrow(a, b, float) its float (1 m head), AddFlatRect* their colour floats.
} // namespace Hash

struct alignas(16) Vec4 {
  float x, y, z, w;
};

struct Vec2 {
  float x, y;
};

// Rows X, Y, Z (axes) and W (translation), the layout of the game's Matrix.
struct alignas(16) Mat {
  Vec4 x, y, z, w;
};

struct Rgba {
  uint8_t r, g, b, a;
};
static_assert(sizeof(Rgba) == 4);

using BeginFrame_t = void (*)(void *aViewport, void *aFrameInfo, void *aCameraFrameInfo, bool aArg3, bool aArg4);
using GetRenderingMask_t = const void *(*)(const void *aViewport);
using CreateDebugDrawer_t = void *(*)(void *aRender, const void *aMask, uint32_t aWidth, uint32_t aHeight);
using AtomicSharedStorage_ctor_t = void *(*)(DrawerStorage *aStorage, void *aPtr);
using DrawerStorage_dtor_t = void (*)(DrawerStorage *aStorage);
using DrawerStorage_assign_t = DrawerStorage *(*)(DrawerStorage *aStorage, const DrawerStorage *aOther);
// The uint8/int8 arguments are dead in the ctor; the script proxy's Construct does not even load them
// (they arrive as whatever was in r8/r9), so 0, 0 is as good as anything.
using FragmentBuilder_ctor_t = void *(*)(void *aBuilder, void *aDrawer, uint8_t aUnused1, int8_t aUnused2);
using FragmentBuilder_dtor_t = void (*)(void *aBuilder);
using FragmentBuilder_Flush_t = void (*)(void *aBuilder);
using FragmentBuilder_EnableOverlay_t = void (*)(void *aBuilder, bool aOn);
using FragmentBuilder_AddLine_t = void (*)(void *aBuilder, const Vec4 *aA, const Vec4 *aB);
using FragmentBuilder_AddLines_t = void (*)(void *aBuilder, const Vec4 *aPoints, uint32_t aCount);
using FragmentBuilder_AddWireBox_t = void (*)(void *aBuilder, const Vec4 *aMin, const Vec4 *aMax, const Rgba *aColor);
using FragmentBuilder_AddWireSphere_t = void (*)(void *aBuilder, const Vec4 *aCenter, float aRadius);
using FragmentBuilder_AddWorldText_t = void (*)(void *aBuilder, const Vec4 *aPos, int32_t aDx, int32_t aDy,
                                                StringView aText, float aScale);
using FragmentBuilder_AddFlatText_t = void (*)(void *aBuilder, int32_t aX, int32_t aY, StringView aText, float aScale);
// The engine's Vector2 is not trivially copyable, so although it is 8 bytes it arrives as a pointer to a copy.
using FragmentBuilder_AddFlatLine_t = void (*)(void *aBuilder, const Vec2 *aA, const Vec2 *aB);
using FragmentBuilder_AddFatLine_t = void (*)(void *aBuilder, const Vec4 *aA, const Vec4 *aB, float aRadius);
using FragmentBuilder_BindMaterial_t = void (*)(void *aBuilder, const void *aMaterialHandle);
using CreateMaterialInstance_t = void *(*)(void *aOutHandle, void *const *aParent, const void *aBaseHandle);
using MaterialInstance_WriteParameterRaw_t = bool (*)(void *aInstance, uint64_t aName, const void *aType,
                                                      const void *aValue, bool aRecreateParent);
using Material_InvalidateRenderResource_t = void (*)(void *aMaterial);
struct Span {
  const void *begin;
  const void *end;
};
using FragmentBuilder_AddSolidIndexedTriangles_t = void (*)(void *aBuilder, const Span *aVertices, const Span *aIndices,
                                                            bool aUnknown);
using FragmentBuilder_AddSolidCapsule_t = void (*)(void *aBuilder, const Vec4 *aCenter, float aRadius,
                                                   float aHalfCylinder, bool aUnknown);
using FragmentBuilder_AddArrow_t = void (*)(void *aBuilder, const Vec4 *aFrom, const Vec4 *aTo);
using FragmentBuilder_AddTransform_t = void (*)(void *aBuilder, const Mat *aMatrix, float aScale);
using FragmentBuilder_AddWireCapsule_t = void (*)(void *aBuilder, const Vec4 *aCenter, float aRadius,
                                                  float aHalfCylinder);
using FragmentBuilder_AddWireTube_t = void (*)(void *aBuilder, const Vec4 *aA, const Vec4 *aB, float aRadiusA,
                                               float aRadiusB, uint16_t aSegments, bool aCaps);
using FragmentBuilder_AddSolidBox_t = void (*)(void *aBuilder, const Vec4 *aMin, const Vec4 *aMax, const Rgba *aColor);
using FragmentBuilder_AddSolidSphere_t = void (*)(void *aBuilder, const Vec4 *aCenter, float aRadius, bool aInside);
using FragmentBuilder_EnableTransparency_t = void (*)(void *aBuilder, bool aOn);

struct alignas(16) Box {
  Vec4 min, max;
};
using GetSkinnedMeshBoxes_t = void (*)(const void *aFunctor, const void *aEntity, void *aBoxes);
using GetBoundingBox_t = Box *(*)(Box *aResult, void *aBoxes);

template <typename T> inline T Resolve(uint32_t aHash) {
  return reinterpret_cast<T>(RED4ext::UniversalRelocBase::Resolve(aHash));
}

} // namespace Engine
