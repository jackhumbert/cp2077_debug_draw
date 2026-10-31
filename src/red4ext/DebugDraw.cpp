#include <RED4ext/RED4ext.hpp>

#include "DebugDraw.hpp"
#include "Diagnostics.hpp"
#include "Engine.hpp"
#include "Geometry.hpp"
#include "Items.hpp"
#include "Overlays.hpp"
#include "Shaders.hpp"

#include <RED4ext/CName.hpp>
#include <RED4ext/CString.hpp>
#include <RED4ext/RTTISystem.hpp>
#include <RED4ext/ResourceLoader.hpp>
#include <RED4ext/ResourcePath.hpp>
#include <RED4ext/Scripting/Natives/Generated/CMaterialInstance.hpp>
#include <RED4ext/Scripting/Natives/Generated/IMaterial.hpp>
#include <RED4ext/Scripting/Functions.hpp>
#include <RED4ext/Memory/Allocators.hpp>
#include <RED4ext/Scripting/Natives/Generated/Box.hpp>
#include <RED4ext/Scripting/Natives/Generated/Color.hpp>
#include <RED4ext/Scripting/Natives/Generated/Quaternion.hpp>
#include <RED4ext/Scripting/Natives/Generated/Transform.hpp>
#include <RED4ext/Scripting/Natives/Vector4.hpp>
#include <RED4ext/Scripting/Stack.hpp>
#include <RED4ext/Scripting/Utils.hpp>

#include <array>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <malloc.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace DebugDraw {
namespace {

using namespace Geometry;


// Shapes are queued from script on the game thread and turned into an engine drawer once per frame in BeginFrame.
//
// One-frame shapes (seconds <= 0) are not dropped by the first drawer built after they were queued: BeginFrame has
// two callers per frame (CGameEngine::ProcessBaseFrame and ProcessFrame), and the later drawer, the one the render
// side keeps, would then miss them. Instead the set queued since the last drawer replaces the drawn set as a whole,
// and the drawn set stays until a newer one arrives: no flicker while a script queues every frame, and a script
// that stops queueing stops drawing after kOneFrameStale.
std::mutex s_lock;
std::vector<Item> s_items;   // timed (seconds > 0), dropped when expired
std::vector<Item> s_pending; // one-frame, queued since the last drawer was built
std::vector<Item> s_frame;   // one-frame, the set currently drawn
Clock::time_point s_frameAdopted;
constexpr auto kOneFrameStale = std::chrono::milliseconds(100);

// Timed plus pending shapes. A script that queues long-lived shapes every frame would otherwise grow these without
// bound; past the cap new shapes are dropped and a warning is logged once.
constexpr size_t kMaxQueued = 1'000'000;
bool s_warnedFull = false;

// Plots: a rolling graph per name, fed one value per call (DebugDraw.Plot) and drawn in screen space, stacked down
// the left edge. A plot that gets no value for kPlotStale disappears.
struct Plot {
  Engine::Rgba color;
  std::deque<float> values;
  Clock::time_point updated;
};
std::map<std::string, Plot> s_plots; // under s_lock, drawn in name order
constexpr size_t kPlotSamples = 240;
constexpr auto kPlotStale = std::chrono::seconds(2);

// Materials per channel (DebugDraw.SetMaterial): a .mt or .mi loaded through the resource loader and bound with
// FragmentBuilder::BindMaterial for that channel's shapes. Until it has loaded (or if it fails) the shapes use the
// engine's debug materials. A translucent material draws all its channel's shapes in the translucent pass, so the
// alpha its shader writes counts even for opaque colours (fades, rims).
// Parameters set from script (DebugDraw.SetMaterialFloat / Color / Vector) go to a CMaterialInstance of our own on
// top of the loaded material, made once it has loaded, so the shared resource is never changed. Each change writes
// the value and invalidates that instance's render resource; the next BindMaterial builds a new render material
// (and cb4 buffer) from it. Under s_lock, which BindMaterial also runs under.
struct MaterialParam {
  RED4ext::CName name;
  RED4ext::CName type; // Float, Color or Vector4
  std::array<uint8_t, 16> value{};
  bool warned = false;
};
struct Material {
  std::string path;
  bool translucent = false;
  RED4ext::SharedPtr<RED4ext::ResourceToken<RED4ext::IMaterial>> token;
  bool reported = false;
  RED4ext::Handle<RED4ext::IMaterial> instance; // ours, once a parameter is set and the material has loaded
  std::vector<MaterialParam> params;            // one per name, latest value
  // What BindMaterial gets: our instance if there is one, else the loaded resource.
  const RED4ext::Handle<RED4ext::IMaterial> &Bound() const { return instance ? instance : token->resource; }
};
std::unordered_map<uint64_t, Material> s_materials; // by channel
std::vector<std::pair<uint64_t, const Material *>> s_boundMaterials; // per build: loaded ones, in use this frame

// The player's display options (DebugDraw_SetDisplay, from Debug Draw's own settings): they apply to every mod's
// shapes. Hidden still expires timed shapes; it only skips drawing. Under s_lock.
bool s_hidden = false;
float s_textScale = 1.0f;     // multiplies every text and label size
float s_labelDistance = 1.0f; // multiplies every maxDistance passed to Text / Label (read by the script)
float s_dimStrength = 1.0f;   // multiplies every Dim amount

// Diagnostics counters (logged every 600 frames with DEBUG_DRAW_DIAGNOSTICS).
size_t s_lastEmitted = 0;
size_t s_lastTimed = 0;
size_t s_lastOneFrame = 0;
uint64_t s_receivedOneFrame = 0;
uint64_t s_receivedTimed = 0;
uint64_t s_swaps = 0;
uint64_t s_staleClears = 0;

// One builder reused every frame: it is 0x40550 bytes, too big for the stack of whatever thread calls BeginFrame.
void *s_builder = nullptr;

Engine::BeginFrame_t s_beginFrameOriginal = nullptr;
Engine::GetRenderingMask_t s_getRenderingMask = nullptr;
void **s_gRender = nullptr;
Engine::CreateDebugDrawer_t s_createDebugDrawer = nullptr;
const RED4ext::Sdk *s_sdk = nullptr;
RED4ext::PluginHandle s_handle = nullptr;

// With diagnostics, each stage logs once, so a log says how far the first frame got.
enum Stage : uint32_t {
  DrawerCreated = 1,
  BuilderConstructed = 2,
  Flushed = 4,
  Swapped = 8,
  NoDrawerWithItems = 16,
  SkipNoHeld = 32,
  SkipAlreadyOurs = 64,
  Wrote = 128,
};
uint32_t s_loggedStages = 0;

void LogStageOnce(Stage aStage, const char *aMessage, const void *aPointer = nullptr) {
  if (!Diagnostics::kEnabled || (s_loggedStages & aStage)) {
    return;
  }
  s_loggedStages |= aStage;
  s_sdk->logger->InfoF(s_handle, "[diag] %s %p", aMessage, aPointer);
}

Engine::AtomicSharedStorage_ctor_t s_storageCtor = nullptr;
Engine::DrawerStorage_dtor_t s_storageDtor = nullptr;
Engine::DrawerStorage_assign_t s_storageAssign = nullptr;
Engine::FragmentBuilder_ctor_t s_builderCtor = nullptr;
Engine::FragmentBuilder_dtor_t s_builderDtor = nullptr;
Engine::FragmentBuilder_EnableOverlay_t s_enableOverlay = nullptr;
Engine::FragmentBuilder_EnableTransparency_t s_enableTransparency = nullptr;
Engine::FragmentBuilder_Flush_t s_builderFlush = nullptr;
Engine::FragmentBuilder_AddLine_t s_addLine = nullptr;
Engine::FragmentBuilder_AddWireBox_t s_addWireBox = nullptr;
Engine::FragmentBuilder_AddWireSphere_t s_addWireSphere = nullptr;
Engine::FragmentBuilder_AddWorldText_t s_addWorldText = nullptr;
Engine::FragmentBuilder_AddFlatText_t s_addFlatText = nullptr;
Engine::FragmentBuilder_AddFlatLine_t s_addFlatLine = nullptr;
Engine::FragmentBuilder_AddArrow_t s_addArrow = nullptr;
Engine::FragmentBuilder_AddFatLine_t s_addFatLine = nullptr;
Engine::FragmentBuilder_AddSolidIndexedTriangles_t s_addSolidTriangles = nullptr;
Engine::FragmentBuilder_AddSolidCapsule_t s_addSolidCapsule = nullptr;
Engine::FragmentBuilder_BindMaterial_t s_bindMaterial = nullptr;
// Optional (material parameters and shader reload): if one does not resolve, those are off.
Engine::CreateMaterialInstance_t s_createMaterialInstance = nullptr;
Engine::MaterialInstance_WriteParameterRaw_t s_writeParameter = nullptr;
Engine::Material_InvalidateRenderResource_t s_invalidateMaterial = nullptr;
Engine::FragmentBuilder_AddFatLine_t s_addFatArrow = nullptr;
Engine::FragmentBuilder_AddTransform_t s_addTransform = nullptr;
Engine::FragmentBuilder_AddWireCapsule_t s_addWireCapsule = nullptr;
Engine::FragmentBuilder_AddWireTube_t s_addWireTube = nullptr;
Engine::FragmentBuilder_AddSolidBox_t s_addSolidBox = nullptr;
Engine::FragmentBuilder_AddSolidSphere_t s_addSolidSphere = nullptr;
Engine::GetSkinnedMeshBoxes_t s_getSkinnedMeshBoxes = nullptr;
Engine::GetBoundingBox_t s_getBoundingBox = nullptr;
uintptr_t s_beginFrameAddress = 0;

// Writes one parameter into the channel's own instance. Under s_lock.
void WriteParam(Material &aMaterial, MaterialParam &aParam) {
  auto type = RED4ext::CRTTISystem::Get()->GetType(aParam.type);
  if (!s_writeParameter(aMaterial.instance.instance, aParam.name.hash, type, aParam.value.data(), false) &&
      !aParam.warned) {
    aParam.warned = true;
    s_sdk->logger->WarnF(s_handle, "Material %s has no %s parameter called %s", aMaterial.path.c_str(),
                         aParam.type.ToString(), aParam.name.ToString());
  }
}

// Makes the channel's own CMaterialInstance on top of its loaded material and writes the parameters set so far.
void MakeInstance(Material &aMaterial) {
  RED4ext::Handle<RED4ext::CMaterialInstance> instance;
  void *parent = nullptr;
  s_createMaterialInstance(&instance, &parent, &aMaterial.token->resource);
  if (!instance) {
    s_sdk->logger->WarnF(s_handle, "Could not make an instance of %s; its parameters are not set",
                         aMaterial.path.c_str());
    aMaterial.params.clear();
    return;
  }
  aMaterial.instance = instance;
  for (auto &param : aMaterial.params) {
    WriteParam(aMaterial, param);
  }
}

template <typename T> T At(const void *aBase, size_t aOffset) {
  return *reinterpret_cast<const T *>(reinterpret_cast<const uint8_t *>(aBase) + aOffset);
}

void SetColor(void *aBuilder, Engine::Rgba aColor) {
  std::memcpy(reinterpret_cast<uint8_t *>(aBuilder) + Engine::kFragmentBuilderColorOffset, &aColor, sizeof(aColor));
}

constexpr float kPi = 3.14159265358979f;

// The engine's own circles cannot do this: AddWireCircle ignores its segment count and is always horizontal.
constexpr uint32_t kCircleSegments = 32;

// A line, or with a thickness a prism of that radius around it.
void Segment(void *aBuilder, const Engine::Vec4 &aA, const Engine::Vec4 &aB, float aThickness) {
  if (aThickness > 0.0f) {
    s_addFatLine(aBuilder, &aA, &aB, aThickness);
  } else {
    s_addLine(aBuilder, &aA, &aB);
  }
}

// The 12 edges of a box around aCenter whose half-extent axes are aAxes. Corner i has bit k set when it is on the
// + side of axis k; edges join corners one bit apart.
void AddBoxEdges(void *aBuilder, const Engine::Vec4 &aCenter, const Engine::Vec4 aAxes[3], float aThickness) {
  Engine::Vec4 corners[8];
  for (uint32_t i = 0; i < 8; ++i) {
    auto corner = aCenter;
    for (uint32_t axis = 0; axis < 3; ++axis) {
      corner = Add(corner, Scale(aAxes[axis], (i >> axis) & 1 ? 1.0f : -1.0f));
    }
    corners[i] = corner;
  }
  for (uint32_t i = 0; i < 8; ++i) {
    for (uint32_t axis = 0; axis < 3; ++axis) {
      if (!((i >> axis) & 1)) {
        Segment(aBuilder, corners[i], corners[i | (1u << axis)], aThickness);
      }
    }
  }
}

void AddCircle(void *aBuilder, Engine::Vec4 aCenter, Engine::Vec4 aNormal, float aRadius, float aThickness) {
  Engine::Vec4 u, w;
  Basis(aNormal, u, w);
  auto previous = OnCircle(aCenter, u, w, aRadius, 0.0f);
  for (uint32_t i = 1; i <= kCircleSegments; ++i) {
    auto next = OnCircle(aCenter, u, w, aRadius, 2.0f * kPi * static_cast<float>(i) / kCircleSegments);
    Segment(aBuilder, previous, next, aThickness);
    previous = next;
  }
}

// Filled triangles through the engine, two-sided: each is added in both windings because the debug material culls
// back faces.
void AddTriangles(void *aBuilder, const Engine::Vec4 *aVertices, size_t aVertexCount, const uint32_t *aIndices,
                  size_t aIndexCount) {
  std::vector<uint32_t> both(aIndices, aIndices + aIndexCount);
  for (size_t i = 0; i + 2 < aIndexCount; i += 3) {
    both.insert(both.end(), {aIndices[i], aIndices[i + 2], aIndices[i + 1]});
  }
  Engine::Span vertices{aVertices, aVertices + aVertexCount};
  Engine::Span indices{both.data(), both.data() + both.size()};
  s_addSolidTriangles(aBuilder, &vertices, &indices, false);
}

// A filled circle facing aNormal: a fan from the centre.
void AddDisc(void *aBuilder, Engine::Vec4 aCenter, Engine::Vec4 aNormal, float aRadius) {
  Engine::Vec4 u, w;
  Basis(aNormal, u, w);
  Engine::Vec4 vertices[kCircleSegments + 1];
  uint32_t indices[kCircleSegments * 3];
  vertices[kCircleSegments] = aCenter;
  for (uint32_t i = 0; i < kCircleSegments; ++i) {
    vertices[i] = OnCircle(aCenter, u, w, aRadius, 2.0f * kPi * static_cast<float>(i) / kCircleSegments);
    indices[i * 3] = kCircleSegments;
    indices[i * 3 + 1] = i;
    indices[i * 3 + 2] = (i + 1) % kCircleSegments;
  }
  AddTriangles(aBuilder, vertices, kCircleSegments + 1, indices, kCircleSegments * 3);
}

void AddSolidOrientedBox(void *aBuilder, const Engine::Vec4 &aCenter, const Engine::Vec4 aAxes[3]) {
  Engine::Vec4 corners[8];
  BoxCorners(aCenter, aAxes, corners);
  // Corner i has bit k set on the + side of axis k; two triangles per face.
  static const uint32_t kFaces[36] = {0, 2, 3, 0, 3, 1, 4, 5, 7, 4, 7, 6, 0, 1, 5, 0, 5, 4,
                                      2, 6, 7, 2, 7, 3, 0, 4, 6, 0, 6, 2, 1, 3, 7, 1, 7, 5};
  AddTriangles(aBuilder, corners, 8, kFaces, 36);
}

void AddSolidCone(void *aBuilder, Engine::Vec4 aApex, Engine::Vec4 aDirection, float aLength, float aRadius) {
  Engine::Vec4 u, w;
  Basis(aDirection, u, w);
  auto base = Add(aApex, Scale(aDirection, aLength));
  Engine::Vec4 vertices[kCircleSegments + 2];
  uint32_t indices[kCircleSegments * 6];
  vertices[kCircleSegments] = aApex;
  vertices[kCircleSegments + 1] = base;
  for (uint32_t i = 0; i < kCircleSegments; ++i) {
    vertices[i] = OnCircle(base, u, w, aRadius, 2.0f * kPi * static_cast<float>(i) / kCircleSegments);
    uint32_t next = (i + 1) % kCircleSegments;
    uint32_t *side = indices + i * 6;
    side[0] = kCircleSegments, side[1] = i, side[2] = next;          // side
    side[3] = kCircleSegments + 1, side[4] = next, side[5] = i;      // base
  }
  AddTriangles(aBuilder, vertices, kCircleSegments + 2, indices, kCircleSegments * 6);
}

// Labels get a dark outline so they read on bright and busy backgrounds: four copies one pixel off, then the text.
template <typename Draw> void WithOutline(void *aBuilder, Engine::Rgba aColor, Draw aDraw) {
  SetColor(aBuilder, {0, 0, 0, aColor.a});
  static const int32_t kOffsets[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
  for (const auto &offset : kOffsets) {
    aDraw(offset[0], offset[1]);
  }
  SetColor(aBuilder, aColor);
  aDraw(0, 0);
}

// Returns roughly how many lines the shape added, so fragments can be kept small (see BuildDrawer).
size_t Emit(void *aBuilder, const Item &aItem) {
  SetColor(aBuilder, aItem.color);
  const auto &v = aItem.v;
  const float thickness = aItem.thickness;
  const size_t fat = thickness > 0.0f ? 4 : 1; // a prism is 6 vertices to a line's 2
  switch (aItem.kind) {
  case Kind::Line:
    Segment(aBuilder, v[0], v[1], thickness);
    return fat;
  case Kind::Box:
    if (thickness > 0.0f) {
      Engine::Vec4 center = Scale(Add(v[0], v[1]), 0.5f);
      center.w = 1.0f;
      const Engine::Vec4 axes[3] = {{(v[1].x - v[0].x) * 0.5f, 0, 0, 0},
                                    {0, (v[1].y - v[0].y) * 0.5f, 0, 0},
                                    {0, 0, (v[1].z - v[0].z) * 0.5f, 0}};
      AddBoxEdges(aBuilder, center, axes, thickness);
    } else {
      s_addWireBox(aBuilder, &v[0], &v[1], &aItem.color);
    }
    return 12 * fat;
  case Kind::Sphere:
    s_addWireSphere(aBuilder, &v[0], aItem.f[0]);
    return 1;
  case Kind::Text:
    WithOutline(aBuilder, aItem.color, [&](int32_t aDx, int32_t aDy) {
      s_addWorldText(aBuilder, &v[0], aDx, aDy, {aItem.text.c_str(), static_cast<uint32_t>(aItem.text.size())},
                     aItem.f[0] * s_textScale);
    });
    return 5;
  case Kind::ScreenText:
    WithOutline(aBuilder, aItem.color, [&](int32_t aDx, int32_t aDy) {
      s_addFlatText(aBuilder, static_cast<int32_t>(v[0].x) + aDx, static_cast<int32_t>(v[0].y) + aDy,
                    {aItem.text.c_str(), static_cast<uint32_t>(aItem.text.size())}, aItem.f[0] * s_textScale);
    });
    return 5;
  case Kind::Arrow:
    if (thickness > 0.0f) {
      s_addFatArrow(aBuilder, &v[0], &v[1], thickness);
    } else {
      s_addArrow(aBuilder, &v[0], &v[1]);
    }
    return 9 * fat;
  case Kind::Axes: {
    Engine::Mat matrix{v[0], v[1], v[2], v[3]};
    s_addTransform(aBuilder, &matrix, aItem.f[0]);
    return 3;
  }
  case Kind::Circle:
    AddCircle(aBuilder, v[0], v[1], aItem.f[0], thickness);
    return kCircleSegments * fat;
  case Kind::Capsule:
    s_addWireCapsule(aBuilder, &v[0], aItem.f[0], aItem.f[1]);
    return 96;
  case Kind::Cylinder:
    s_addWireTube(aBuilder, &v[0], &v[1], aItem.f[0], aItem.f[0], 16, true);
    return 48;
  case Kind::Cone: {
    // AddWireCone exists, but which of its two rings the spokes reach is unclear; this one is plain.
    auto base = Add(v[0], Scale(v[1], aItem.f[0]));
    AddCircle(aBuilder, base, v[1], aItem.f[1], thickness);
    Engine::Vec4 u, w;
    Basis(v[1], u, w);
    for (uint32_t i = 0; i < 8; ++i) {
      auto rim = OnCircle(base, u, w, aItem.f[1], 2.0f * kPi * static_cast<float>(i) / 8);
      Segment(aBuilder, v[0], rim, thickness);
    }
    return (kCircleSegments + 8) * fat;
  }
  case Kind::OrientedBox:
    AddBoxEdges(aBuilder, v[3], v, thickness);
    return 12 * fat;
  case Kind::SolidBox:
    s_addSolidBox(aBuilder, &v[0], &v[1], &aItem.color);
    return 12;
  case Kind::SolidSphere:
    s_addSolidSphere(aBuilder, &v[0], aItem.f[0], false);
    return 1;
  case Kind::Dim:
    s_addSolidSphere(aBuilder, &v[0], aItem.f[0], true); // inside: faces the camera within it
    return 1;
  case Kind::Triangle: {
    static const uint32_t kIndices[3] = {0, 1, 2};
    AddTriangles(aBuilder, v, 3, kIndices, 3);
    return 2;
  }
  case Kind::Disc:
    AddDisc(aBuilder, v[0], v[1], aItem.f[0]);
    return kCircleSegments * 2;
  case Kind::SolidOrientedBox:
    AddSolidOrientedBox(aBuilder, v[3], v);
    return 24;
  case Kind::SolidCone:
    AddSolidCone(aBuilder, v[0], v[1], aItem.f[0], aItem.f[1]);
    return kCircleSegments * 4;
  case Kind::SolidCapsule:
    s_addSolidCapsule(aBuilder, &v[0], aItem.f[0], aItem.f[1], false);
    return 96;
  }
  return 1;
}

void FlatLine(void *aBuilder, float aX0, float aY0, float aX1, float aY1) {
  Engine::Vec2 a{aX0, aY0}, b{aX1, aY1};
  s_addFlatLine(aBuilder, &a, &b);
}

void FlatText(void *aBuilder, float aX, float aY, const char *aText) {
  s_addFlatText(aBuilder, static_cast<int32_t>(aX), static_cast<int32_t>(aY),
                {aText, static_cast<uint32_t>(std::strlen(aText))}, 1.0f);
}

// Each plot: its name and latest value above a framed graph scaled to its own min..max, which are printed at the
// right. Caller holds s_lock.
void EmitPlots(void *aBuilder, uint32_t aHeight) {
  constexpr float kX = 40.0f, kWidth = 300.0f, kGraph = 60.0f, kStep = 100.0f;
  const Engine::Rgba frame{255, 255, 255, 255}, dim{150, 150, 150, 255};
  float top = static_cast<float>(aHeight) * 0.28f;
  char text[160];
  for (const auto &[name, plot] : s_plots) {
    if (plot.values.empty()) {
      continue;
    }
    float low = plot.values.front(), high = low;
    for (float value : plot.values) {
      low = std::fmin(low, value);
      high = std::fmax(high, value);
    }
    const float range = high - low > 1e-6f ? high - low : 1.0f;
    const float y0 = top + 18.0f, y1 = y0 + kGraph;
    SetColor(aBuilder, frame);
    std::snprintf(text, sizeof(text), "%s  %.3g", name.c_str(), plot.values.back());
    FlatText(aBuilder, kX, top, text);
    SetColor(aBuilder, dim);
    FlatLine(aBuilder, kX, y0, kX + kWidth, y0);
    FlatLine(aBuilder, kX, y1, kX + kWidth, y1);
    FlatLine(aBuilder, kX, y0, kX, y1);
    FlatLine(aBuilder, kX + kWidth, y0, kX + kWidth, y1);
    std::snprintf(text, sizeof(text), "%.3g", high);
    FlatText(aBuilder, kX + kWidth + 6.0f, y0 - 6.0f, text);
    std::snprintf(text, sizeof(text), "%.3g", low);
    FlatText(aBuilder, kX + kWidth + 6.0f, y1 - 6.0f, text);
    SetColor(aBuilder, plot.color);
    const float dx = kWidth / static_cast<float>(kPlotSamples - 1);
    float px = 0.0f, py = 0.0f;
    size_t i = 0;
    for (float value : plot.values) {
      float x = kX + dx * static_cast<float>(i + kPlotSamples - plot.values.size());
      float y = y1 - (value - low) / range * kGraph;
      if (i++ > 0) {
        FlatLine(aBuilder, px, py, x, y);
      }
      px = x;
      py = y;
    }
    s_builderFlush(aBuilder); // a plot is up to ~250 lines: keep each its own fragment
    top += kStep;
  }
}

// A rotation's X, Y and Z axes (the game's Z up, Y forward), the rows of its matrix.
void RotationRows(const RED4ext::Quaternion &aQ, Engine::Vec4 aRows[3]) {
  Geometry::RotationRows(aQ.i, aQ.j, aQ.k, aQ.r, aRows);
}

// PushTransform / PushEntity / PopTransform. Shapes queued under a frame are moved by its transform; under an entity
// they keep that entity's space and follow it each frame. The stack is per frame: whatever a script left pushed is
// dropped at the next BeginFrame, so a missing Pop cannot leak into other mods' shapes. Under s_lock.
struct TransformFrame {
  Engine::Mat transform;
  RED4ext::WeakHandle<RED4ext::IScriptable> anchor; // empty when not under PushEntity
};
std::vector<TransformFrame> s_transforms;
bool s_warnedUnpopped = false;

// Caller holds s_lock.
void Place(Item &aItem) {
  if (s_transforms.empty()) {
    return;
  }
  const auto &top = s_transforms.back();
  TransformItem(aItem, top.transform);
  aItem.anchor = top.anchor;
}

// An entity's current world transform, through its script functions (game thread).
bool EntityTransform(const RED4ext::WeakHandle<RED4ext::IScriptable> &aEntity, Engine::Mat &aOut) {
  auto entity = aEntity.Lock();
  if (!entity) {
    return false;
  }
  static RED4ext::CBaseFunction *getPosition = nullptr;
  static RED4ext::CBaseFunction *getOrientation = nullptr;
  if (!getPosition) {
    auto type = RED4ext::CRTTISystem::Get()->GetClass("entEntity");
    getPosition = type ? type->GetFunction("GetWorldPosition") : nullptr;
    getOrientation = type ? type->GetFunction("GetWorldOrientation") : nullptr;
    if (!getPosition || !getOrientation) {
      return false;
    }
  }
  RED4ext::Vector4 position{};
  RED4ext::Quaternion orientation{};
  if (!RED4ext::ExecuteFunction(entity.instance, getPosition, &position) ||
      !RED4ext::ExecuteFunction(entity.instance, getOrientation, &orientation)) {
    return false;
  }
  RotationRows(orientation, &aOut.x);
  aOut.w = {position.X, position.Y, position.Z, 1.0f};
  return true;
}

// Entity transforms for this build, so each tracked entity is looked up once. Under s_lock.
std::unordered_map<const void *, std::pair<bool, Engine::Mat>> s_anchorTransforms;

// aItem as it is drawn this frame: moved into its entity's current place when it follows one; false when that entity
// is gone. aScratch holds the moved copy.
const Item *Placed(const Item &aItem, Item &aScratch) {
  if (!aItem.anchor.instance) {
    return &aItem;
  }
  auto key = static_cast<const void *>(aItem.anchor.instance);
  auto found = s_anchorTransforms.find(key);
  if (found == s_anchorTransforms.end()) {
    Engine::Mat transform{};
    bool ok = EntityTransform(aItem.anchor, transform);
    found = s_anchorTransforms.emplace(key, std::make_pair(ok, transform)).first;
  }
  if (!found->second.first) {
    return nullptr;
  }
  aScratch = aItem;
  TransformItem(aScratch, found->second.second);
  return &aScratch;
}

// World overlays drawn every frame around an entity (DebugDraw_SetWorldOverlays, from Debug Draw's own settings):
// rebuilt at each drawer build from the overlays' caches (see OverlaySlot; moving bodies are re-queried each
// frame), so they follow the entity and the cars around it with no script timer. Under s_lock.
constexpr uint32_t kOverlayNavmesh = 1, kOverlayTriggers = 2, kOverlayCollision = 4;
struct WorldOverlays {
  RED4ext::WeakHandle<RED4ext::IScriptable> entity;
  uint32_t kinds = 0;
  float radius = 20.0f;
  bool throughWalls = false;
};
WorldOverlays s_worldOverlays;
// One list of line shapes per overlay source, remade only when that source's build changes: the static world,
// navmesh and triggers then cost nothing on most frames; only moving bodies are remade every frame.
struct OverlaySlot {
  uint64_t build = 0;
  bool used = false; // drawn this build
  std::vector<Item> items;
};
constexpr size_t kOverlaySlots = 8;
OverlaySlot s_overlaySlots[kOverlaySlots];
std::vector<RED4ext::Vector4> s_overlayPoints; // scratch, kept for its capacity
// Everything BuildDrawer draws: the queued lists and the overlay slots in use. Remade each build.
std::vector<const std::vector<Item> *> s_drawLists;

void AddOverlayLines(size_t aSlot, Overlays::Source aSource, const RED4ext::Vector4 &aCenter, float aRadius,
                     int32_t aA, int32_t aB, Engine::Rgba aColor, float aThickness) {
  auto &slot = s_overlaySlots[aSlot];
  slot.used = true;
  s_overlayPoints.clear();
  const auto build = Overlays::AppendLines(aSource, aCenter, aRadius, aA, aB, slot.build, s_overlayPoints);
  if (build == slot.build) {
    return;
  }
  slot.build = build;
  slot.items.clear();
  slot.items.reserve(s_overlayPoints.size() / 2);
  for (size_t i = 0; i + 1 < s_overlayPoints.size(); i += 2) {
    auto &item = slot.items.emplace_back();
    item.kind = Kind::Line;
    item.overlay = s_worldOverlays.throughWalls;
    item.color = aColor;
    item.v[0] = {s_overlayPoints[i].X, s_overlayPoints[i].Y, s_overlayPoints[i].Z, 1.0f};
    item.v[1] = {s_overlayPoints[i + 1].X, s_overlayPoints[i + 1].Y, s_overlayPoints[i + 1].Z, 1.0f};
    item.thickness = aThickness;
  }
}

// Forgets the slots' lines (the overlays were turned off, or drawing changed, e.g. through walls).
void ResetOverlaySlots() {
  for (auto &slot : s_overlaySlots) {
    slot = {};
  }
}

// The colours match DebugDrawOverlays.Show* in Overlays.reds.
// How long the last BuildWorldOverlays took (DebugDraw_OverlayFrameMs, for tests). Under s_lock.
double s_overlayFrameMs = 0.0;

void BuildWorldOverlays() {
  const auto started = Clock::now();
  struct Timer {
    Clock::time_point started;
    ~Timer() { s_overlayFrameMs = std::chrono::duration<double, std::milli>(Clock::now() - started).count(); }
  } timer{started};
  for (auto &slot : s_overlaySlots) {
    slot.used = false;
  }
  const auto &o = s_worldOverlays;
  Engine::Mat transform{};
  if (!o.kinds || !EntityTransform(o.entity, transform)) {
    return;
  }
  const RED4ext::Vector4 at{transform.w.x, transform.w.y, transform.w.z, 1.0f};
  const float wide = o.radius * 1.5f;
  if (o.kinds & kOverlayNavmesh) {
    AddOverlayLines(0, Overlays::Source::Navmesh, at, wide, 0, 0, {0, 150, 170, 255}, 0.0f);
    AddOverlayLines(1, Overlays::Source::Navmesh, at, wide, 0, 1, {60, 255, 255, 255}, 0.02f);
  }
  if (o.kinds & kOverlayTriggers) {
    AddOverlayLines(2, Overlays::Source::Triggers, at, wide, 0, 0, {230, 60, 230, 255}, 0.015f);
    AddOverlayLines(3, Overlays::Source::Triggers, at, wide, 16 | 32 | 512 | 1024, 0, {70, 120, 255, 255}, 0.015f);
    AddOverlayLines(4, Overlays::Source::Triggers, at, wide, 2, 0, {60, 230, 90, 255}, 0.015f);
    AddOverlayLines(5, Overlays::Source::Triggers, at, wide, 64, 0, {255, 220, 40, 255}, 0.015f);
  }
  if (o.kinds & kOverlayCollision) {
    AddOverlayLines(6, Overlays::Source::Collision, at, o.radius, 1, 0, {150, 255, 150, 255}, 0.0f);
    AddOverlayLines(7, Overlays::Source::Collision, at, o.radius, 2, 0, {255, 150, 40, 255}, 0.0f);
  }
}

// X-ray per channel (SetXRay): depth-tested shapes are drawn again on top of everything at this alpha factor, so the
// parts behind walls show faintly. Under s_lock.
std::unordered_map<uint64_t, float> s_xray;

// Fills a fresh engine drawer with the timed shapes and the current one-frame set (see s_pending/s_frame), after
// dropping expired and stale ones. Returns nullptr when there is nothing to draw, so the frame keeps its null drawer.
void *BuildDrawer(void *aViewport) {
  std::lock_guard lock(s_lock);
  auto now = Clock::now();
  if (!s_pending.empty()) {
    s_frame.swap(s_pending);
    s_pending.clear();
    s_frameAdopted = now;
    ++s_swaps;
  } else if (!s_frame.empty() && now - s_frameAdopted > kOneFrameStale) {
    s_frame.clear();
    ++s_staleClears;
  }
  std::erase_if(s_items, [now](const Item &item) { return item.expires <= now; });
  std::erase_if(s_plots, [now](const auto &entry) { return now - entry.second.updated > kPlotStale; });
  if (!s_transforms.empty()) {
    if (!s_warnedUnpopped) {
      s_warnedUnpopped = true;
      s_sdk->logger->Warn(s_handle, "A script left a PushTransform / PushEntity without a PopTransform; dropped at "
                                    "the end of the frame");
    }
    s_transforms.clear();
  }
  // Shapes that follow an entity that no longer exists go too. Looked up again for drawing below.
  s_anchorTransforms.clear();
  for (auto *list : {&s_items, &s_frame}) {
    std::erase_if(*list, [](const Item &item) {
      Item scratch;
      return item.anchor.instance && !Placed(item, scratch);
    });
  }
  if (s_hidden) {
    return nullptr;
  }
  BuildWorldOverlays();
  s_drawLists.assign({&s_items, &s_frame});
  bool overlayLines = false;
  for (const auto &slot : s_overlaySlots) {
    if (slot.used && !slot.items.empty()) {
      s_drawLists.push_back(&slot.items);
      overlayLines = true;
    }
  }
  if (s_items.empty() && s_frame.empty() && s_plots.empty() && !overlayLines) {
    return nullptr;
  }

  auto viewport = At<void *>(aViewport, Engine::kEngineViewportViewportOffset);
  if (!viewport || !*s_gRender) {
    return nullptr;
  }
  auto width = At<uint32_t>(viewport, Engine::kViewportWidthOffset);
  auto height = At<uint32_t>(viewport, Engine::kViewportHeightOffset);
  if (!width || !height) {
    return nullptr;
  }

  auto render = *s_gRender;
  auto drawer = s_createDebugDrawer(render, s_getRenderingMask(aViewport), width, height);
  if (!drawer) {
    return nullptr;
  }
  LogStageOnce(DrawerCreated, "Created debug drawer", drawer);

  std::memset(s_builder, 0, Engine::kFragmentBuilderSize);
  s_builderCtor(s_builder, drawer, 0, 0);
  LogStageOnce(BuilderConstructed, "Constructed fragment builder", s_builder);
  // Shapes go out in up to eight groups: depth tested before overlay, opaque before translucent (alpha < 255, drawn
  // with EnableTransparency), and within those, the cached-mesh shapes (spheres, each a fragment of its own) after
  // the line-built ones. EnableOverlay and EnableTransparency flush when they change, so grouping keeps fragments
  // few. Fragments are also flushed about every kLinesPerFragment lines: a single 2000-vertex fragment built after a
  // sphere did not draw, and keeping fragments small with spheres last fixed it.
  constexpr size_t kLinesPerFragment = 128;
  size_t emitted = 0;
  size_t inFragment = 0;
  bool overlay = false;
  bool transparent = false;
  // Dimming goes first. The renderer draws opaque debug geometry before translucent, and translucent in the order it
  // was submitted, so while dimming every other shape is drawn translucent too: after the dim sphere instead of
  // under it (opaque colours look the same either way). Overlay shapes and text are drawn later anyway.
  bool dimming = false;
  for (const auto *list : s_drawLists) {
    for (const auto &item : *list) {
      if (item.kind != Kind::Dim) {
        continue;
      }
      dimming = true;
      if (!transparent) {
        transparent = true;
        s_enableTransparency(s_builder, true);
      }
      Emit(s_builder, item);
      ++emitted;
      s_builderFlush(s_builder);
    }
  }
  // Loaded materials, reported once each.
  s_boundMaterials.clear();
  for (auto &[channel, material] : s_materials) {
    if (!material.token) {
      continue;
    }
    if (material.token->IsLoaded()) {
      if (!material.params.empty() && !material.instance) {
        MakeInstance(material);
      }
      s_boundMaterials.emplace_back(channel, &material);
    }
    if (!material.reported && material.token->IsFinished()) {
      material.reported = true;
      if (material.token->IsLoaded()) {
        s_sdk->logger->InfoF(s_handle, "Material %s loaded", material.path.c_str());
      } else {
        s_sdk->logger->WarnF(s_handle, "Material %s failed to load; its shapes use the default", material.path.c_str());
      }
    }
  }
  // Which of s_boundMaterials an item uses: 0 for the default, i + 1 for s_boundMaterials[i]. Text keeps the default.
  auto materialOf = [](const Item &aItem) -> size_t {
    if (s_boundMaterials.empty() || IsText(aItem.kind)) {
      return 0;
    }
    for (size_t i = 0; i < s_boundMaterials.size(); ++i) {
      if (s_boundMaterials[i].first == aItem.channel) {
        return i + 1;
      }
    }
    return 0;
  };
  for (uint32_t group = 0; group < 8; ++group) {
    const bool groupOverlay = group & 4, groupTransparent = group & 2, groupMesh = group & 1;
    for (size_t pass = 0; pass <= s_boundMaterials.size(); ++pass) {
      bool bound = false;
      for (const auto *list : s_drawLists) {
        for (const auto &item : *list) {
          const auto itemMaterial = materialOf(item);
          bool itemTransparent = (dimming || item.color.a < 255 ||
                                  (itemMaterial && s_boundMaterials[itemMaterial - 1].second->translucent)) &&
                                 !IsText(item.kind);
          if (item.kind == Kind::Dim || item.overlay != groupOverlay || itemTransparent != groupTransparent ||
              IsMesh(item.kind) != groupMesh || itemMaterial != pass) {
            continue;
          }
          if (pass > 0 && !bound) {
            bound = true;
            s_bindMaterial(s_builder, &s_boundMaterials[pass - 1].second->Bound());
            inFragment = 0;
          }
          if (overlay != groupOverlay) {
            overlay = groupOverlay;
            s_enableOverlay(s_builder, overlay);
            inFragment = 0;
          }
          if (transparent != groupTransparent) {
            transparent = groupTransparent;
            s_enableTransparency(s_builder, transparent);
            inFragment = 0;
          }
          Item scratch;
          auto placed = Placed(item, scratch);
          if (!placed) {
            continue;
          }
          inFragment += Emit(s_builder, *placed);
          ++emitted;
          if (inFragment >= kLinesPerFragment) {
            s_builderFlush(s_builder);
            inFragment = 0;
          }
        }
      }
      if (bound) {
        RED4ext::Handle<RED4ext::IMaterial> none;
        s_bindMaterial(s_builder, &none);
        inFragment = 0;
      }
    }
  }
  // X-ray: the depth-tested shapes of x-ray channels once more, on top and faint.
  if (!s_xray.empty()) {
    for (const auto *list : s_drawLists) {
      for (const auto &item : *list) {
        if (item.overlay || IsText(item.kind) || item.kind == Kind::Dim) {
          continue;
        }
        auto found = s_xray.find(item.channel);
        if (found == s_xray.end()) {
          continue;
        }
        if (!overlay) {
          overlay = true;
          s_enableOverlay(s_builder, true);
          inFragment = 0;
        }
        if (!transparent) {
          transparent = true;
          s_enableTransparency(s_builder, true);
          inFragment = 0;
        }
        Item scratch;
        auto placed = Placed(item, scratch);
        if (!placed) {
          continue;
        }
        Item faint = *placed;
        faint.color.a = static_cast<uint8_t>(std::fmin(255.0f, faint.color.a * found->second));
        inFragment += Emit(s_builder, faint);
        ++emitted;
        if (inFragment >= kLinesPerFragment) {
          s_builderFlush(s_builder);
          inFragment = 0;
        }
      }
    }
  }
  if (!s_plots.empty()) {
    if (transparent) {
      s_enableTransparency(s_builder, false);
    }
    s_builderFlush(s_builder);
    EmitPlots(s_builder, height);
  }
  s_builderDtor(s_builder); // flushes the last fragment into the drawer
  s_lastEmitted = emitted;
  s_lastTimed = s_items.size();
  s_lastOneFrame = s_frame.size();
  LogStageOnce(Flushed, "Flushed fragments into drawer", drawer);
  return drawer;
}

// DebugDrawerImpl keeps two lock-free fragment lists (normal at +0x118, the "+8 flag" one at +0x128), each node
// {settings, ?, ?, geometry, next at +0x20}. Counting them says whether Flush really reached the drawer.
uint32_t CountFragments(const void *aDrawer) {
  uint32_t count = 0;
  for (size_t head : {size_t{0x118}, size_t{0x128}}) {
    for (auto node = At<const void *>(aDrawer, head); node && count < 100000; node = At<const void *>(node, 0x20)) {
      ++count;
    }
  }
  return count;
}

uint64_t s_frames = 0;
std::atomic<void *> s_lastDrawer{nullptr};

// A strong reference to the current drawer. Copies of the camera frame info do not reliably carry it to the camera
// data slot the render side reads, so Collect assigns it there itself right before drawing (AssignCurrentDrawer).
// Game thread writes, render threads read: guarded by a mutex.
std::mutex s_currentLock;
Engine::DrawerStorage s_current;
uint64_t s_currentSerial = 0; // bumped for every drawer put in s_current
uint64_t s_framesWithDrawer = 0;

// void rend::EngineViewport::BeginFrame(CRenderFrameInfo&, CRenderCameraFrameInfo&, bool, bool)
// The camera frame info reaches the renderer during the game tick (gsm::StateMachine::ProcessRender ->
// CRenderFrameCameraStorage::UpdateCameraData), not at SubmitFrame, so the drawer goes in right after BeginFrame
// resets camInfo and before the tick.
void BeginFrame(void *aViewport, void *aFrameInfo, void *aCameraFrameInfo, bool aArg3, bool aArg4) {
  s_beginFrameOriginal(aViewport, aFrameInfo, aCameraFrameInfo, aArg3, aArg4);
  ++s_frames;
  auto drawer = BuildDrawer(aViewport);
  if (!drawer) {
    size_t queued = 0;
    {
      // After BuildDrawer, which drops expired and stale shapes: anything left means this viewport was the reason.
      std::lock_guard lock(s_lock);
      queued = s_items.size() + s_pending.size() + s_frame.size() + s_plots.size();
    }
    // Only drop the held drawer when nothing is queued: one of the two BeginFrame calls a frame can come from a
    // viewport with no size, and clearing then would empty the reference Collect hands the render side.
    if (queued != 0) {
      if (Diagnostics::kEnabled && !(s_loggedStages & NoDrawerWithItems)) {
        s_loggedStages |= NoDrawerWithItems;
        s_sdk->logger->InfoF(s_handle, "[diag] BeginFrame from viewport %p built no drawer with %zu items queued; "
                                       "keeping the previous one", aViewport, queued);
      }
      return;
    }
    Engine::DrawerStorage empty;
    std::lock_guard lock(s_currentLock);
    if (s_current.drawer) {
      s_storageAssign(&s_current, &empty);
      s_lastDrawer.store(nullptr);
    }
  } else {
    Engine::DrawerStorage fresh;
    s_storageCtor(&fresh, drawer); // refs {1, 1}: from here the engine frees the drawer when the frame is done
    {
      std::lock_guard lock(s_currentLock);
      s_storageAssign(&s_current, &fresh);
      ++s_currentSerial;
    }
    auto slot = reinterpret_cast<Engine::DrawerStorage *>(reinterpret_cast<uint8_t *>(aCameraFrameInfo) +
                                                          Engine::kCameraFrameInfoDrawerOffset);
    s_storageAssign(slot, &fresh);
    s_storageDtor(&fresh);
    s_lastDrawer.store(drawer);
    LogStageOnce(Swapped, "Swapped drawer into camera frame info", drawer);
    if (Diagnostics::kEnabled && s_framesWithDrawer++ % 600 == 0) {
      s_sdk->logger->InfoF(s_handle,
                           "[diag] frame %llu: %zu shapes drawn (%zu timed, %zu one-frame), %u fragments in drawer %p "
                           "(held %p); received %llu one-frame, %llu timed; %llu swaps, %llu stale clears",
                           s_frames, s_lastEmitted, s_lastTimed, s_lastOneFrame, CountFragments(drawer), drawer,
                           s_current.drawer, s_receivedOneFrame, s_receivedTimed, s_swaps, s_staleClears);
    }
  }
}

Clock::time_point ExpiryFrom(float aSeconds) {
  if (!(aSeconds > 0.0f)) { // NaN too: a bad seconds value must not become an already-expired timed shape
    return Clock::time_point::min(); // one frame
  }
  return Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(aSeconds));
}

Engine::Rgba ToRgba(const RED4ext::Color &aColor) { return {aColor.Red, aColor.Green, aColor.Blue, aColor.Alpha}; }

Engine::Vec4 ToVec4(const RED4ext::Vector4 &aVector) { return {aVector.X, aVector.Y, aVector.Z, 1.0f}; }

Engine::Vec4 ToDirection(const RED4ext::Vector4 &aVector, Engine::Vec4 aFallback) {
  return Normalized({aVector.X, aVector.Y, aVector.Z, 0.0f}, aFallback);
}

uint64_t ToChannel(RED4ext::CName aName) { return aName.IsNone() ? 0 : aName.hash; }


// Caller holds s_lock. False (and a warning the first time) when the queue is full.
bool HasRoom(size_t aCount) {
  if (s_items.size() + s_pending.size() + aCount <= kMaxQueued) {
    return true;
  }
  if (!s_warnedFull) {
    s_warnedFull = true;
    s_sdk->logger->WarnF(s_handle, "More than %zu shapes queued; dropping new ones until some expire. A script is "
                                   "probably queueing long-lived shapes every frame.", kMaxQueued);
  }
  return false;
}

void Push(Item &&aItem) {
  std::lock_guard lock(s_lock);
  if (!HasRoom(1)) {
    return;
  }
  Place(aItem);
  bool oneFrame = aItem.expires == Clock::time_point::min();
  auto received = oneFrame ? ++s_receivedOneFrame : ++s_receivedTimed;
  if (Diagnostics::kEnabled && received <= 2) {
    s_sdk->logger->InfoF(s_handle, "[diag] received %s shape #%llu (kind %u)", oneFrame ? "one-frame" : "timed",
                         received, static_cast<uint32_t>(aItem.kind));
  }
  auto &list = oneFrame ? s_pending : s_items;
  list.emplace_back(std::move(aItem));
}

template <typename... T> void Read(RED4ext::CStackFrame *aFrame, T *...aParams) {
  (RED4ext::GetParameter(aFrame, aParams), ...);
  aFrame->code++; // skip ParamEnd
}

// The parameters most shape natives end with: color, seconds, overlay, channel, thickness.
struct Common {
  RED4ext::Color color{255, 255, 255, 255};
  float seconds = 0.0f;
  bool overlay = false;
  RED4ext::CName channel;
  float thickness = 0.0f;
};

Item MakeItem(Kind aKind, const Common &aCommon) {
  Item item{};
  item.kind = aKind;
  item.overlay = aCommon.overlay;
  item.color = ToRgba(aCommon.color);
  item.expires = ExpiryFrom(aCommon.seconds);
  item.channel = ToChannel(aCommon.channel);
  item.thickness = aCommon.thickness > 0.0f ? std::fmin(aCommon.thickness, 10.0f) : 0.0f; // NaN too
  return item;
}

// native func DebugDraw_Line(a: Vector4, b: Vector4, color: Color, seconds: Float, overlay: Bool, channel: CName)
// native func DebugDraw_Arrow(from: Vector4, to: Vector4, ...same): the engine's head is the last 20 % of the line
// native func DebugDraw_Box(min: Vector4, max: Vector4, ...same)
// native func DebugDraw_SolidBox(min: Vector4, max: Vector4, ...same)
template <Kind K> void Native_TwoPoints(RED4ext::IScriptable *, RED4ext::CStackFrame *aFrame, void *, int64_t) {
  RED4ext::Vector4 a{}, b{};
  Common c;
  Read(aFrame, &a, &b, &c.color, &c.seconds, &c.overlay, &c.channel, &c.thickness);
  auto item = MakeItem(K, c);
  item.v[0] = ToVec4(a);
  item.v[1] = ToVec4(b);
  Push(std::move(item));
}

// native func DebugDraw_Lines(points: array<Vector4>, color: Color, seconds: Float, overlay: Bool, channel: CName)
// Point pairs (a, b, a, b, ...) in one call, so a script can draw tens of thousands of lines a frame without a call
// each. An odd last point is ignored.
void Native_Lines(RED4ext::IScriptable *, RED4ext::CStackFrame *aFrame, void *, int64_t) {
  RED4ext::DynArray<RED4ext::Vector4> points;
  Common c;
  Read(aFrame, &points, &c.color, &c.seconds, &c.overlay, &c.channel, &c.thickness);
  auto item = MakeItem(Kind::Line, c);
  const bool oneFrame = item.expires == Clock::time_point::min();
  const uint32_t lines = points.size / 2;
  std::lock_guard lock(s_lock);
  if (!HasRoom(lines)) {
    return;
  }
  auto &list = oneFrame ? s_pending : s_items;
  for (uint32_t i = 0; i < lines; ++i) {
    item.v[0] = ToVec4(points[2 * i]);
    item.v[1] = ToVec4(points[2 * i + 1]);
    Place(item);
    list.push_back(item);
  }
  (oneFrame ? s_receivedOneFrame : s_receivedTimed) += lines;
}

// native func DebugDraw_Sphere(center: Vector4, radius: Float, color: Color, seconds: Float, overlay: Bool,
//                              channel: CName)
// native func DebugDraw_SolidSphere(...same)
template <Kind K> void Native_Ball(RED4ext::IScriptable *, RED4ext::CStackFrame *aFrame, void *, int64_t) {
  RED4ext::Vector4 center{};
  float radius = 0.0f;
  Common c;
  Read(aFrame, &center, &radius, &c.color, &c.seconds, &c.overlay, &c.channel, &c.thickness);
  auto item = MakeItem(K, c);
  item.v[0] = ToVec4(center);
  item.f[0] = radius;
  Push(std::move(item));
}

// native func DebugDraw_Axes(transform: Transform, length: Float, seconds: Float, overlay: Bool, channel: CName)
// The engine colours X red, Y green and Z blue.
void Native_Axes(RED4ext::IScriptable *, RED4ext::CStackFrame *aFrame, void *, int64_t) {
  RED4ext::Transform transform{};
  float length = 0.0f;
  Common c;
  Read(aFrame, &transform, &length, &c.seconds, &c.overlay, &c.channel);
  auto item = MakeItem(Kind::Axes, c);
  RotationRows(transform.orientation, item.v);
  item.v[3] = ToVec4(transform.position);
  item.f[0] = length;
  Push(std::move(item));
}

// native func DebugDraw_Circle(center: Vector4, normal: Vector4, radius: Float, color: Color, seconds: Float,
//                              overlay: Bool, channel: CName)
// A zero normal means up (a horizontal circle).
void Native_Circle(RED4ext::IScriptable *, RED4ext::CStackFrame *aFrame, void *, int64_t) {
  RED4ext::Vector4 center{}, normal{};
  float radius = 0.0f;
  Common c;
  Read(aFrame, &center, &normal, &radius, &c.color, &c.seconds, &c.overlay, &c.channel, &c.thickness);
  auto item = MakeItem(Kind::Circle, c);
  item.v[0] = ToVec4(center);
  item.v[1] = ToDirection(normal, {0, 0, 1, 0});
  item.f[0] = radius;
  Push(std::move(item));
}

// native func DebugDraw_Capsule(center: Vector4, radius: Float, height: Float, color: Color, seconds: Float,
//                               overlay: Bool, channel: CName)
// Upright (the engine's capsule is always along world Z); height is the whole capsule, caps included.
void Native_Capsule(RED4ext::IScriptable *, RED4ext::CStackFrame *aFrame, void *, int64_t) {
  RED4ext::Vector4 center{};
  float radius = 0.0f, height = 0.0f;
  Common c;
  Read(aFrame, &center, &radius, &height, &c.color, &c.seconds, &c.overlay, &c.channel, &c.thickness);
  auto item = MakeItem(Kind::Capsule, c);
  item.v[0] = ToVec4(center);
  item.f[0] = radius;
  item.f[1] = std::fmax(0.0f, height * 0.5f - radius);
  Push(std::move(item));
}

// native func DebugDraw_Cylinder(a: Vector4, b: Vector4, radius: Float, color: Color, seconds: Float, overlay: Bool,
//                                channel: CName)
void Native_Cylinder(RED4ext::IScriptable *, RED4ext::CStackFrame *aFrame, void *, int64_t) {
  RED4ext::Vector4 a{}, b{};
  float radius = 0.0f;
  Common c;
  Read(aFrame, &a, &b, &radius, &c.color, &c.seconds, &c.overlay, &c.channel, &c.thickness);
  auto item = MakeItem(Kind::Cylinder, c);
  item.v[0] = ToVec4(a);
  item.v[1] = ToVec4(b);
  item.f[0] = radius;
  Push(std::move(item));
}

// native func DebugDraw_Cone(apex: Vector4, direction: Vector4, length: Float, angle: Float, color: Color,
//                            seconds: Float, overlay: Bool, channel: CName)
// angle is the half-angle in degrees, clamped below 89.
void Native_Cone(RED4ext::IScriptable *, RED4ext::CStackFrame *aFrame, void *, int64_t) {
  RED4ext::Vector4 apex{}, direction{};
  float length = 0.0f, angle = 0.0f;
  Common c;
  Read(aFrame, &apex, &direction, &length, &angle, &c.color, &c.seconds, &c.overlay, &c.channel, &c.thickness);
  auto item = MakeItem(Kind::Cone, c);
  item.v[0] = ToVec4(apex);
  item.v[1] = ToDirection(direction, {0, 1, 0, 0});
  item.f[0] = length;
  item.f[1] = length * std::tan(std::fmin(std::fabs(angle), 89.0f) * kPi / 180.0f);
  Push(std::move(item));
}

// native func DebugDraw_OrientedBox(transform: Transform, halfExtents: Vector4, color: Color, seconds: Float,
//                                   overlay: Bool, channel: CName)
void Native_OrientedBox(RED4ext::IScriptable *, RED4ext::CStackFrame *aFrame, void *, int64_t) {
  RED4ext::Transform transform{};
  RED4ext::Vector4 half{};
  Common c;
  Read(aFrame, &transform, &half, &c.color, &c.seconds, &c.overlay, &c.channel, &c.thickness);
  auto item = MakeItem(Kind::OrientedBox, c);
  RotationRows(transform.orientation, item.v);
  item.v[0] = Scale(item.v[0], half.X);
  item.v[1] = Scale(item.v[1], half.Y);
  item.v[2] = Scale(item.v[2], half.Z);
  item.v[3] = ToVec4(transform.position);
  Push(std::move(item));
}

float TextScale(float aScale) { return aScale > 0.0f ? aScale : 1.0f; }

// native func DebugDraw_Text(position: Vector4, text: String, color: Color, seconds: Float, channel: CName,
//                            scale: Float)
// World text is projected to the screen in DebugDrawerImpl::CollectImpl, so it is never depth tested.
void Native_Text(RED4ext::IScriptable *, RED4ext::CStackFrame *aFrame, void *, int64_t) {
  RED4ext::Vector4 position{};
  RED4ext::CString text;
  float scale = 0.0f;
  Common c;
  Read(aFrame, &position, &text, &c.color, &c.seconds, &c.channel, &scale);
  c.overlay = true;
  auto item = MakeItem(Kind::Text, c);
  item.v[0] = ToVec4(position);
  item.f[0] = TextScale(scale);
  item.text.assign(text.c_str(), text.Length());
  Push(std::move(item));
}

// native func DebugDraw_ScreenText(x: Int32, y: Int32, text: String, color: Color, seconds: Float, channel: CName,
//                                  scale: Float)
// x, y in screen pixels from the top left.
void Native_ScreenText(RED4ext::IScriptable *, RED4ext::CStackFrame *aFrame, void *, int64_t) {
  int32_t x = 0, y = 0;
  RED4ext::CString text;
  float scale = 0.0f;
  Common c;
  Read(aFrame, &x, &y, &text, &c.color, &c.seconds, &c.channel, &scale);
  c.overlay = true;
  auto item = MakeItem(Kind::ScreenText, c);
  item.v[0] = {static_cast<float>(x), static_cast<float>(y), 0.0f, 0.0f};
  item.f[0] = TextScale(scale);
  item.text.assign(text.c_str(), text.Length());
  Push(std::move(item));
}

// native func DebugDraw_Triangles(points: array<Vector4>, ...common): corner triples, filled and two-sided
void Native_Triangles(RED4ext::IScriptable *, RED4ext::CStackFrame *aFrame, void *, int64_t) {
  RED4ext::DynArray<RED4ext::Vector4> points;
  Common c;
  Read(aFrame, &points, &c.color, &c.seconds, &c.overlay, &c.channel, &c.thickness);
  const uint32_t triangles = points.size / 3;
  for (uint32_t i = 0; i < triangles; ++i) {
    auto item = MakeItem(Kind::Triangle, c);
    item.v[0] = ToVec4(points[3 * i]);
    item.v[1] = ToVec4(points[3 * i + 1]);
    item.v[2] = ToVec4(points[3 * i + 2]);
    Push(std::move(item));
  }
}

// native func DebugDraw_Disc(center: Vector4, normal: Vector4, radius: Float, ...common)
void Native_Disc(RED4ext::IScriptable *, RED4ext::CStackFrame *aFrame, void *, int64_t) {
  RED4ext::Vector4 center{}, normal{};
  float radius = 0.0f;
  Common c;
  Read(aFrame, &center, &normal, &radius, &c.color, &c.seconds, &c.overlay, &c.channel, &c.thickness);
  auto item = MakeItem(Kind::Disc, c);
  item.v[0] = ToVec4(center);
  item.v[1] = ToDirection(normal, {0, 0, 1, 0});
  item.f[0] = radius;
  Push(std::move(item));
}

// native func DebugDraw_SolidOrientedBox(transform: Transform, halfExtents: Vector4, ...common)
void Native_SolidOrientedBox(RED4ext::IScriptable *, RED4ext::CStackFrame *aFrame, void *, int64_t) {
  RED4ext::Transform transform{};
  RED4ext::Vector4 half{};
  Common c;
  Read(aFrame, &transform, &half, &c.color, &c.seconds, &c.overlay, &c.channel, &c.thickness);
  auto item = MakeItem(Kind::SolidOrientedBox, c);
  RotationRows(transform.orientation, item.v);
  item.v[0] = Scale(item.v[0], half.X);
  item.v[1] = Scale(item.v[1], half.Y);
  item.v[2] = Scale(item.v[2], half.Z);
  item.v[3] = ToVec4(transform.position);
  Push(std::move(item));
}

// native func DebugDraw_SolidCone(apex: Vector4, direction: Vector4, length: Float, angle: Float, ...common)
void Native_SolidCone(RED4ext::IScriptable *, RED4ext::CStackFrame *aFrame, void *, int64_t) {
  RED4ext::Vector4 apex{}, direction{};
  float length = 0.0f, angle = 0.0f;
  Common c;
  Read(aFrame, &apex, &direction, &length, &angle, &c.color, &c.seconds, &c.overlay, &c.channel, &c.thickness);
  auto item = MakeItem(Kind::SolidCone, c);
  item.v[0] = ToVec4(apex);
  item.v[1] = ToDirection(direction, {0, 1, 0, 0});
  item.f[0] = length;
  item.f[1] = length * std::tan(std::fmin(std::fabs(angle), 89.0f) * kPi / 180.0f);
  Push(std::move(item));
}

// native func DebugDraw_SolidCapsule(center: Vector4, radius: Float, height: Float, ...common)
void Native_SolidCapsule(RED4ext::IScriptable *, RED4ext::CStackFrame *aFrame, void *, int64_t) {
  RED4ext::Vector4 center{};
  float radius = 0.0f, height = 0.0f;
  Common c;
  Read(aFrame, &center, &radius, &height, &c.color, &c.seconds, &c.overlay, &c.channel, &c.thickness);
  auto item = MakeItem(Kind::SolidCapsule, c);
  item.v[0] = ToVec4(center);
  item.f[0] = radius;
  item.f[1] = std::fmax(0.0f, height * 0.5f - radius);
  Push(std::move(item));
}

// native func DebugDraw_PushTransform(transform: Transform)
void Native_PushTransform(RED4ext::IScriptable *, RED4ext::CStackFrame *aFrame, void *, int64_t) {
  RED4ext::Transform transform{};
  Read(aFrame, &transform);
  Engine::Mat local{};
  RotationRows(transform.orientation, &local.x);
  local.w = ToVec4(transform.position);
  std::lock_guard lock(s_lock);
  TransformFrame frame{local, {}};
  if (!s_transforms.empty()) {
    frame.transform = Compose(s_transforms.back().transform, local);
    frame.anchor = s_transforms.back().anchor;
  }
  s_transforms.push_back(std::move(frame));
}

// native func DebugDraw_PushEntity(entity: ref<Entity>)
void Native_PushEntity(RED4ext::IScriptable *, RED4ext::CStackFrame *aFrame, void *, int64_t) {
  RED4ext::Handle<RED4ext::IScriptable> entity;
  Read(aFrame, &entity);
  std::lock_guard lock(s_lock);
  TransformFrame frame{kIdentity, {}};
  frame.anchor = entity;
  s_transforms.push_back(std::move(frame));
}

// native func DebugDraw_PopTransform()
void Native_PopTransform(RED4ext::IScriptable *, RED4ext::CStackFrame *aFrame, void *, int64_t) {
  aFrame->code++;
  std::lock_guard lock(s_lock);
  if (!s_transforms.empty()) {
    s_transforms.pop_back();
  }
}

// native func DebugDraw_SetXRay(channel: CName, alpha: Float): alpha 0 turns it off
void Native_SetXRay(RED4ext::IScriptable *, RED4ext::CStackFrame *aFrame, void *, int64_t) {
  RED4ext::CName channel;
  float alpha = 0.0f;
  Read(aFrame, &channel, &alpha);
  std::lock_guard lock(s_lock);
  if (alpha > 0.0f) {
    s_xray[ToChannel(channel)] = std::fmin(alpha, 1.0f);
  } else {
    s_xray.erase(ToChannel(channel));
  }
}

// native func DebugDraw_Dim(camera: Vector4, radius: Float, amount: Float)
// One frame of a black, depth-tested, inward-facing sphere around the camera, amount (0..1) opaque. Small enough
// to be in front of everything; a large one (50 m) left nearby geometry undimmed.
void Native_Dim(RED4ext::IScriptable *, RED4ext::CStackFrame *aFrame, void *, int64_t) {
  RED4ext::Vector4 camera{};
  float radius = 0.0f, amount = 0.0f;
  Common c;
  Read(aFrame, &camera, &radius, &amount);
  float strength;
  {
    std::lock_guard lock(s_lock); // Push takes it too
    strength = s_dimStrength;
  }
  amount = std::fmin(std::fmax(std::isfinite(amount) ? amount : 0.0f, 0.0f), 1.0f) * strength;
  if (amount <= 0.0f) {
    return;
  }
  c.color = {0, 0, 0, static_cast<uint8_t>(amount * 254.0f)}; // below 255 so it stays translucent
  auto item = MakeItem(Kind::Dim, c);
  item.v[0] = ToVec4(camera);
  item.f[0] = radius;
  Push(std::move(item));
}

// native func DebugDraw_SetDisplay(visible: Bool, textScale: Float, labelDistance: Float, dimStrength: Float)
// The player's display options, for every mod's shapes (Debug Draw's settings page).
void Native_SetDisplay(RED4ext::IScriptable *, RED4ext::CStackFrame *aFrame, void *, int64_t) {
  bool visible = true;
  float textScale = 1.0f, labelDistance = 1.0f, dimStrength = 1.0f;
  Read(aFrame, &visible, &textScale, &labelDistance, &dimStrength);
  auto clamp = [](float aValue, float aMin, float aMax) {
    return std::isfinite(aValue) ? std::fmin(std::fmax(aValue, aMin), aMax) : 1.0f;
  };
  std::lock_guard lock(s_lock);
  s_hidden = !visible;
  s_textScale = clamp(textScale, 0.1f, 10.0f);
  s_labelDistance = clamp(labelDistance, 0.0f, 100.0f);
  s_dimStrength = clamp(dimStrength, 0.0f, 1.0f);
}

// native func DebugDraw_SetWorldOverlays(entity: ref<Entity>, kinds: Int32, radius: Float, throughWalls: Bool)
// Draw the world overlays around entity every frame: kinds 1 navmesh, 2 triggers, 4 collision (0 off).
void Native_SetWorldOverlays(RED4ext::IScriptable *, RED4ext::CStackFrame *aFrame, void *, int64_t) {
  RED4ext::Handle<RED4ext::IScriptable> entity;
  int32_t kinds = 0;
  float radius = 20.0f;
  bool throughWalls = false;
  Read(aFrame, &entity, &kinds, &radius, &throughWalls);
  std::lock_guard lock(s_lock);
  s_worldOverlays.entity = entity;
  s_worldOverlays.kinds = entity ? static_cast<uint32_t>(kinds) & 7 : 0;
  s_worldOverlays.radius = std::isfinite(radius) ? std::fmin(std::fmax(radius, 1.0f), 200.0f) : 20.0f;
  s_worldOverlays.throughWalls = throughWalls;
  ResetOverlaySlots(); // the new settings (through walls, radius) apply to every line
}

// native func DebugDraw_OverlayFrameMs() -> Float: the last frame's world overlay build, all of it (for tests)
void Native_OverlayFrameMs(RED4ext::IScriptable *, RED4ext::CStackFrame *aFrame, float *aOut, int64_t) {
  aFrame->code++;
  std::lock_guard lock(s_lock);
  if (aOut) {
    *aOut = static_cast<float>(s_overlayFrameMs);
  }
}

// native func DebugDraw_LabelDistanceScale() -> Float
void Native_LabelDistanceScale(RED4ext::IScriptable *, RED4ext::CStackFrame *aFrame, float *aOut, int64_t) {
  aFrame->code++;
  std::lock_guard lock(s_lock);
  if (aOut) {
    *aOut = s_labelDistance;
  }
}

// native func DebugDraw_GetBounds(entity: ref<Entity>) -> Box
// The box around an entity's skinned meshes, from the engine's own AI helper. It is in the entity's own space (the
// meshes' model-space bounds); the script helpers turn it with the entity. Min above Max when it has none.
void Native_GetBounds(RED4ext::IScriptable *, RED4ext::CStackFrame *aFrame, RED4ext::Box *aOut, int64_t) {
  RED4ext::Handle<RED4ext::IScriptable> entity;
  Read(aFrame, &entity);
  Engine::Box box{{FLT_MAX, FLT_MAX, FLT_MAX, 1.0f}, {-FLT_MAX, -FLT_MAX, -FLT_MAX, 1.0f}};
  if (entity && s_getSkinnedMeshBoxes && s_getBoundingBox) {
    RED4ext::Memory::DefaultAllocator allocator;
    RED4ext::DynArray<RED4ext::Box> boxes(&allocator);
    uint8_t functor = 0;
    s_getSkinnedMeshBoxes(&functor, entity.instance, &boxes);
    if (boxes.size > 0) {
      s_getBoundingBox(&box, &boxes);
    }
  }
  if (aOut) {
    aOut->Min = {box.min.x, box.min.y, box.min.z, 1.0f};
    aOut->Max = {box.max.x, box.max.y, box.max.z, 1.0f};
  }
}

// native func DebugDraw_SetMaterial(channel: CName, path: String, translucent: Bool)
// The material (.mt or .mi depot path) for a channel's shapes; an empty path goes back to the default.
void Native_SetMaterial(RED4ext::IScriptable *, RED4ext::CStackFrame *aFrame, void *, int64_t) {
  RED4ext::CName channel;
  RED4ext::CString path;
  bool translucent = false;
  Read(aFrame, &channel, &path, &translucent);
  std::string text(path.c_str(), path.Length());
  const auto tag = ToChannel(channel);
  std::lock_guard lock(s_lock);
  if (text.empty()) {
    s_materials.erase(tag);
    return;
  }
  auto &material = s_materials[tag];
  material.translucent = translucent;
  if (material.path == text && material.token) {
    return;
  }
  material.path = text;
  material.reported = false;
  material.instance = {};
  material.params.clear();
  material.token = RED4ext::ResourceLoader::Get()->LoadAsync<RED4ext::IMaterial>(RED4ext::ResourcePath(text.c_str()));
  s_sdk->logger->InfoF(s_handle, "Loading material %s for a channel", text.c_str());
}

// Sets a parameter of a channel's material (after SetMaterial); applied once it has loaded.
void SetMaterialParam(RED4ext::CName aChannel, RED4ext::CName aName, const char *aType, const void *aValue,
                      size_t aSize) {
  std::lock_guard lock(s_lock);
  auto found = s_materials.find(ToChannel(aChannel));
  if (!s_writeParameter) {
    return;
  }
  if (found == s_materials.end()) {
    s_sdk->logger->WarnF(s_handle, "SetMaterial%s(%s): the channel has no material; call SetMaterial first",
                         aType, aName.ToString());
    return;
  }
  auto &material = found->second;
  MaterialParam *param = nullptr;
  for (auto &existing : material.params) {
    if (existing.name == aName) {
      param = &existing;
    }
  }
  if (!param) {
    param = &material.params.emplace_back();
    param->name = aName;
  }
  param->type = RED4ext::CName(aType);
  param->value = {};
  std::memcpy(param->value.data(), aValue, aSize);
  if (material.instance) {
    WriteParam(material, *param);
    s_invalidateMaterial(material.instance.instance);
  }
}

// native func DebugDraw_SetMaterialFloat(channel: CName, name: CName, value: Float)
void Native_SetMaterialFloat(RED4ext::IScriptable *, RED4ext::CStackFrame *aFrame, void *, int64_t) {
  RED4ext::CName channel, name;
  float value = 0.0f;
  Read(aFrame, &channel, &name, &value);
  SetMaterialParam(channel, name, "Float", &value, sizeof(value));
}

// native func DebugDraw_SetMaterialColor(channel: CName, name: CName, value: Color)
void Native_SetMaterialColor(RED4ext::IScriptable *, RED4ext::CStackFrame *aFrame, void *, int64_t) {
  RED4ext::CName channel, name;
  RED4ext::Color value{};
  Read(aFrame, &channel, &name, &value);
  SetMaterialParam(channel, name, "Color", &value, sizeof(value));
}

// native func DebugDraw_SetMaterialVector(channel: CName, name: CName, value: Vector4)
void Native_SetMaterialVector(RED4ext::IScriptable *, RED4ext::CStackFrame *aFrame, void *, int64_t) {
  RED4ext::CName channel, name;
  RED4ext::Vector4 value{};
  Read(aFrame, &channel, &name, &value);
  SetMaterialParam(channel, name, "Vector4", &value, sizeof(value));
}

// native func DebugDraw_ReloadShaders() -> Int32
// Reads the shader manifests again and rebuilds every channel material's render resource, so new bytecode (under new
// synthetic shader IDs) is fetched. Returns the number of materials in the manifests, or -1 if reloading is off.
void Native_ReloadShaders(RED4ext::IScriptable *, RED4ext::CStackFrame *aFrame, int32_t *aOut, int64_t) {
  aFrame->code++;
  int32_t materials = -1;
  if (s_invalidateMaterial) {
    materials = Shaders::Reload();
    std::lock_guard lock(s_lock);
    size_t rebuilt = 0;
    for (auto &[channel, material] : s_materials) {
      if (material.token && material.token->IsLoaded()) {
        s_invalidateMaterial(material.Bound().instance);
        ++rebuilt;
      }
    }
    s_sdk->logger->InfoF(s_handle, "Shaders reloaded: %d materials in the manifests, %zu channel materials rebuilt",
                         materials, rebuilt);
  }
  if (aOut) {
    *aOut = materials;
  }
}

// native func DebugDraw_Plot(name: String, value: Float, color: Color)
void Native_Plot(RED4ext::IScriptable *, RED4ext::CStackFrame *aFrame, void *, int64_t) {
  RED4ext::CString name;
  float value = 0.0f;
  RED4ext::Color color{};
  Read(aFrame, &name, &value, &color);
  std::lock_guard lock(s_lock);
  auto &plot = s_plots[std::string(name.c_str(), name.Length())];
  plot.color = ToRgba(color);
  plot.updated = Clock::now();
  plot.values.push_back(std::isfinite(value) ? value : 0.0f);
  if (plot.values.size() > kPlotSamples) {
    plot.values.pop_front();
  }
}

// native func DebugDraw_ClearPlot(name: String)
void Native_ClearPlot(RED4ext::IScriptable *, RED4ext::CStackFrame *aFrame, void *, int64_t) {
  RED4ext::CString name;
  Read(aFrame, &name);
  std::lock_guard lock(s_lock);
  s_plots.erase(std::string(name.c_str(), name.Length()));
}

// native func DebugDraw_Clear(channel: CName): drops that channel's shapes (None: the shapes queued without one)
void Native_Clear(RED4ext::IScriptable *, RED4ext::CStackFrame *aFrame, void *, int64_t) {
  RED4ext::CName channel;
  Read(aFrame, &channel);
  const auto tag = ToChannel(channel);
  auto inChannel = [tag](const Item &item) { return item.channel == tag; };
  std::lock_guard lock(s_lock);
  std::erase_if(s_items, inChannel);
  std::erase_if(s_pending, inChannel);
  std::erase_if(s_frame, inChannel);
}

// native func DebugDraw_ClearAll()
void Native_ClearAll(RED4ext::IScriptable *, RED4ext::CStackFrame *aFrame, void *, int64_t) {
  aFrame->code++;
  std::lock_guard lock(s_lock);
  s_items.clear();
  s_pending.clear();
  s_frame.clear();
  s_plots.clear();
}

using Params = std::vector<std::pair<const char *, const char *>>;

void Register(RED4ext::CRTTISystem *aRtti, const char *aName, RED4ext::ScriptingFunction_t<void *> aFunc,
              const Params &aParams) {
  auto function = RED4ext::CGlobalFunction::Create(aName, aName, aFunc);
  for (const auto &[type, name] : aParams) {
    function->AddParam(type, name);
  }
  aRtti->RegisterFunction(function);
}

// aShape followed by the Common parameters.
Params WithCommon(Params aShape) {
  aShape.insert(aShape.end(), {{"Color", "color"},
                               {"Float", "seconds"},
                               {"Bool", "overlay"},
                               {"CName", "channel"},
                               {"Float", "thickness"}});
  return aShape;
}

} // namespace

bool Load(const RED4ext::Sdk *aSdk, RED4ext::PluginHandle aHandle) {
  using namespace Engine;
  s_sdk = aSdk;
  s_handle = aHandle;
  s_createDebugDrawer = Resolve<CreateDebugDrawer_t>(Hash::CRenderInterface_CreateDebugDrawer);
  s_beginFrameAddress = RED4ext::UniversalRelocBase::Resolve(Hash::EngineViewport_BeginFrame);
  s_getRenderingMask = Resolve<GetRenderingMask_t>(Hash::EngineViewport_GetRenderingMask);
  s_gRender = Resolve<void **>(Hash::GRender);
  s_storageCtor = Resolve<AtomicSharedStorage_ctor_t>(Hash::AtomicSharedStorage_ctor);
  s_storageDtor = Resolve<DrawerStorage_dtor_t>(Hash::DrawerStorage_dtor);
  s_storageAssign = Resolve<DrawerStorage_assign_t>(Hash::DrawerStorage_assign);
  s_builderCtor = Resolve<FragmentBuilder_ctor_t>(Hash::FragmentBuilder_ctor);
  s_builderDtor = Resolve<FragmentBuilder_dtor_t>(Hash::FragmentBuilder_dtor);
  s_enableOverlay = Resolve<FragmentBuilder_EnableOverlay_t>(Hash::FragmentBuilder_EnableOverlay);
  s_enableTransparency = Resolve<FragmentBuilder_EnableTransparency_t>(Hash::FragmentBuilder_EnableTransparency);
  s_builderFlush = Resolve<FragmentBuilder_Flush_t>(Hash::FragmentBuilder_Flush);
  s_addLine = Resolve<FragmentBuilder_AddLine_t>(Hash::FragmentBuilder_AddLine);
  s_addWireBox = Resolve<FragmentBuilder_AddWireBox_t>(Hash::FragmentBuilder_AddWireBox);
  s_addWireSphere = Resolve<FragmentBuilder_AddWireSphere_t>(Hash::FragmentBuilder_AddWireSphere);
  s_addWorldText = Resolve<FragmentBuilder_AddWorldText_t>(Hash::FragmentBuilder_AddWorldText);
  s_addFlatText = Resolve<FragmentBuilder_AddFlatText_t>(Hash::FragmentBuilder_AddFlatText);
  s_addFlatLine = Resolve<FragmentBuilder_AddFlatLine_t>(Hash::FragmentBuilder_AddFlatLine);
  // Optional: without them GetBounds reports no bounds.
  s_getSkinnedMeshBoxes = Resolve<GetSkinnedMeshBoxes_t>(Hash::AIUtils_GetSkinnedMeshBoxes);
  s_getBoundingBox = Resolve<GetBoundingBox_t>(Hash::AIUtils_GetBoundingBox);
  s_addArrow = Resolve<FragmentBuilder_AddArrow_t>(Hash::FragmentBuilder_AddArrow);
  s_addFatLine = Resolve<FragmentBuilder_AddFatLine_t>(Hash::FragmentBuilder_AddFatLineFast);
  s_bindMaterial = Resolve<FragmentBuilder_BindMaterial_t>(Hash::FragmentBuilder_BindMaterial);
  s_createMaterialInstance = Resolve<CreateMaterialInstance_t>(Hash::CreateHandle_CMaterialInstance);
  s_writeParameter = Resolve<MaterialInstance_WriteParameterRaw_t>(Hash::CMaterialInstance_WriteParameterRaw);
  s_invalidateMaterial = Resolve<Material_InvalidateRenderResource_t>(Hash::IMaterial_InvalidateRenderResource);
  if (!s_createMaterialInstance || !s_writeParameter || !s_invalidateMaterial) {
    aSdk->logger->Warn(aHandle, "Material instance functions did not resolve; material parameters and shader "
                                "reload are off");
    s_createMaterialInstance = nullptr;
    s_writeParameter = nullptr;
    s_invalidateMaterial = nullptr;
  }
  s_addSolidTriangles =
      Resolve<FragmentBuilder_AddSolidIndexedTriangles_t>(Hash::FragmentBuilder_AddSolidIndexedTriangles);
  s_addSolidCapsule = Resolve<FragmentBuilder_AddSolidCapsule_t>(Hash::FragmentBuilder_AddSolidCapsule);
  s_addFatArrow = Resolve<FragmentBuilder_AddFatLine_t>(Hash::FragmentBuilder_AddFatArrow);
  s_addTransform = Resolve<FragmentBuilder_AddTransform_t>(Hash::FragmentBuilder_AddTransform);
  s_addWireCapsule = Resolve<FragmentBuilder_AddWireCapsule_t>(Hash::FragmentBuilder_AddWireCapsule);
  s_addWireTube = Resolve<FragmentBuilder_AddWireTube_t>(Hash::FragmentBuilder_AddWireTube);
  s_addSolidBox = Resolve<FragmentBuilder_AddSolidBox_t>(Hash::FragmentBuilder_AddSolidBox);
  s_addSolidSphere = Resolve<FragmentBuilder_AddSolidSphere_t>(Hash::FragmentBuilder_AddSolidSphere);

  const void *resolved[] = {
      reinterpret_cast<void *>(s_beginFrameAddress), reinterpret_cast<void *>(s_getRenderingMask),
      reinterpret_cast<void *>(s_gRender),           reinterpret_cast<void *>(s_storageCtor),
      reinterpret_cast<void *>(s_storageDtor),       reinterpret_cast<void *>(s_storageAssign),
      reinterpret_cast<void *>(s_builderCtor),       reinterpret_cast<void *>(s_builderDtor),
      reinterpret_cast<void *>(s_enableOverlay),     reinterpret_cast<void *>(s_enableTransparency),
      reinterpret_cast<void *>(s_addLine),           reinterpret_cast<void *>(s_addWireBox),
      reinterpret_cast<void *>(s_addWireSphere),     reinterpret_cast<void *>(s_addWorldText),
      reinterpret_cast<void *>(s_addFlatText),       reinterpret_cast<void *>(s_addArrow),
      reinterpret_cast<void *>(s_addTransform),      reinterpret_cast<void *>(s_addWireCapsule),
      reinterpret_cast<void *>(s_addWireTube),       reinterpret_cast<void *>(s_addSolidBox),
      reinterpret_cast<void *>(s_addSolidSphere),    reinterpret_cast<void *>(s_createDebugDrawer),
      reinterpret_cast<void *>(s_builderFlush),      reinterpret_cast<void *>(s_addFlatLine),
      reinterpret_cast<void *>(s_addFatLine),        reinterpret_cast<void *>(s_addFatArrow),
      reinterpret_cast<void *>(s_bindMaterial),      reinterpret_cast<void *>(s_addSolidTriangles),
      reinterpret_cast<void *>(s_addSolidCapsule)};
  for (auto address : resolved) {
    if (!address) {
      aSdk->logger->Error(aHandle, "An engine address did not resolve; this game version is not supported");
      return false;
    }
  }

  s_builder = _aligned_malloc(Engine::kFragmentBuilderSize, 16);
  if (!s_builder) {
    return false;
  }

  if (!aSdk->hooking->Attach(aHandle, reinterpret_cast<void *>(s_beginFrameAddress), reinterpret_cast<void *>(&BeginFrame),
                             reinterpret_cast<void **>(&s_beginFrameOriginal))) {
    aSdk->logger->Error(aHandle, "Could not hook EngineViewport::BeginFrame");
    _aligned_free(s_builder);
    s_builder = nullptr;
    return false;
  }
  if (Diagnostics::kEnabled) {
    aSdk->logger->InfoF(aHandle, "[diag] hooked EngineViewport::BeginFrame at 0x%llX", s_beginFrameAddress);
  }
  return true;
}

void Unload(const RED4ext::Sdk *aSdk, RED4ext::PluginHandle aHandle) {
  if (s_beginFrameOriginal) {
    aSdk->hooking->Detach(aHandle, reinterpret_cast<void *>(s_beginFrameAddress));
  }
  {
    Engine::DrawerStorage empty;
    std::lock_guard lock(s_currentLock);
    if (s_current.drawer && s_storageAssign) {
      s_storageAssign(&s_current, &empty);
    }
  }
  if (s_builder) {
    _aligned_free(s_builder);
    s_builder = nullptr;
  }
}

void *LastDrawer() { return s_lastDrawer.load(); }

// s_loggedStages is touched from render threads here too; a lost bit only means a repeated diagnostics line.
bool AssignCurrentDrawer(Engine::DrawerStorage *aSlot, uint64_t *aSerial) {
  std::lock_guard lock(s_currentLock);
  *aSerial = s_current.drawer ? s_currentSerial : 0;
  if (!s_current.drawer) {
    LogStageOnce(SkipNoHeld, "CollectDebug write skipped: no held drawer; slot holds", aSlot->drawer);
    return false;
  }
  if (aSlot->drawer == s_current.drawer) {
    LogStageOnce(SkipAlreadyOurs, "CollectDebug write skipped: slot already holds our drawer", aSlot->drawer);
    return false;
  }
  void *previous = aSlot->drawer;
  s_storageAssign(aSlot, &s_current); // releases the slot's old drawer, adds a reference to ours
  LogStageOnce(Wrote, "CollectDebug write: replaced slot drawer", previous);
  return true;
}

void RegisterNatives() {
  auto rtti = RED4ext::CRTTISystem::Get();
  Register(rtti, "DebugDraw_Line", &Native_TwoPoints<Kind::Line>, WithCommon({{"Vector4", "a"}, {"Vector4", "b"}}));
  Register(rtti, "DebugDraw_Lines", &Native_Lines, WithCommon({{"array:Vector4", "points"}}));
  Register(rtti, "DebugDraw_Arrow", &Native_TwoPoints<Kind::Arrow>,
           WithCommon({{"Vector4", "from"}, {"Vector4", "to"}}));
  Register(rtti, "DebugDraw_Box", &Native_TwoPoints<Kind::Box>, WithCommon({{"Vector4", "min"}, {"Vector4", "max"}}));
  Register(rtti, "DebugDraw_SolidBox", &Native_TwoPoints<Kind::SolidBox>,
           WithCommon({{"Vector4", "min"}, {"Vector4", "max"}}));
  Register(rtti, "DebugDraw_OrientedBox", &Native_OrientedBox,
           WithCommon({{"Transform", "transform"}, {"Vector4", "halfExtents"}}));
  Register(rtti, "DebugDraw_Sphere", &Native_Ball<Kind::Sphere>,
           WithCommon({{"Vector4", "center"}, {"Float", "radius"}}));
  Register(rtti, "DebugDraw_SolidSphere", &Native_Ball<Kind::SolidSphere>,
           WithCommon({{"Vector4", "center"}, {"Float", "radius"}}));
  Register(rtti, "DebugDraw_Circle", &Native_Circle,
           WithCommon({{"Vector4", "center"}, {"Vector4", "normal"}, {"Float", "radius"}}));
  Register(rtti, "DebugDraw_Capsule", &Native_Capsule,
           WithCommon({{"Vector4", "center"}, {"Float", "radius"}, {"Float", "height"}}));
  Register(rtti, "DebugDraw_Cylinder", &Native_Cylinder,
           WithCommon({{"Vector4", "a"}, {"Vector4", "b"}, {"Float", "radius"}}));
  Register(rtti, "DebugDraw_Cone", &Native_Cone,
           WithCommon({{"Vector4", "apex"}, {"Vector4", "direction"}, {"Float", "length"}, {"Float", "angle"}}));
  Register(rtti, "DebugDraw_Axes", &Native_Axes,
           {{"Transform", "transform"}, {"Float", "length"}, {"Float", "seconds"}, {"Bool", "overlay"},
            {"CName", "channel"}});
  Register(rtti, "DebugDraw_Text", &Native_Text,
           {{"Vector4", "position"}, {"String", "text"}, {"Color", "color"}, {"Float", "seconds"},
            {"CName", "channel"}, {"Float", "scale"}});
  Register(rtti, "DebugDraw_ScreenText", &Native_ScreenText,
           {{"Int32", "x"}, {"Int32", "y"}, {"String", "text"}, {"Color", "color"}, {"Float", "seconds"},
            {"CName", "channel"}, {"Float", "scale"}});
  {
    auto function = RED4ext::CGlobalFunction::Create("DebugDraw_GetBounds", "DebugDraw_GetBounds", &Native_GetBounds);
    function->AddParam("handle:entEntity", "entity");
    function->SetReturnType("Box");
    rtti->RegisterFunction(function);
  }
  Register(rtti, "DebugDraw_Triangles", &Native_Triangles, WithCommon({{"array:Vector4", "points"}}));
  Register(rtti, "DebugDraw_Disc", &Native_Disc,
           WithCommon({{"Vector4", "center"}, {"Vector4", "normal"}, {"Float", "radius"}}));
  Register(rtti, "DebugDraw_SolidOrientedBox", &Native_SolidOrientedBox,
           WithCommon({{"Transform", "transform"}, {"Vector4", "halfExtents"}}));
  Register(rtti, "DebugDraw_SolidCone", &Native_SolidCone,
           WithCommon({{"Vector4", "apex"}, {"Vector4", "direction"}, {"Float", "length"}, {"Float", "angle"}}));
  Register(rtti, "DebugDraw_SolidCapsule", &Native_SolidCapsule,
           WithCommon({{"Vector4", "center"}, {"Float", "radius"}, {"Float", "height"}}));
  Register(rtti, "DebugDraw_PushTransform", &Native_PushTransform, {{"Transform", "transform"}});
  Register(rtti, "DebugDraw_PushEntity", &Native_PushEntity, {{"handle:entEntity", "entity"}});
  Register(rtti, "DebugDraw_PopTransform", &Native_PopTransform, {});
  Register(rtti, "DebugDraw_SetXRay", &Native_SetXRay, {{"CName", "channel"}, {"Float", "alpha"}});
  Register(rtti, "DebugDraw_Dim", &Native_Dim,
           {{"Vector4", "camera"}, {"Float", "radius"}, {"Float", "amount"}});
  Register(rtti, "DebugDraw_SetMaterial", &Native_SetMaterial,
           {{"CName", "channel"}, {"String", "path"}, {"Bool", "translucent"}});
  Register(rtti, "DebugDraw_Plot", &Native_Plot, {{"String", "name"}, {"Float", "value"}, {"Color", "color"}});
  Register(rtti, "DebugDraw_ClearPlot", &Native_ClearPlot, {{"String", "name"}});
  Register(rtti, "DebugDraw_Clear", &Native_Clear, {{"CName", "channel"}});
  Register(rtti, "DebugDraw_ClearAll", &Native_ClearAll, {});
  Register(rtti, "DebugDraw_SetWorldOverlays", &Native_SetWorldOverlays,
           {{"handle:entEntity", "entity"}, {"Int32", "kinds"}, {"Float", "radius"}, {"Bool", "throughWalls"}});
  Register(rtti, "DebugDraw_SetDisplay", &Native_SetDisplay,
           {{"Bool", "visible"}, {"Float", "textScale"}, {"Float", "labelDistance"}, {"Float", "dimStrength"}});
  {
    auto function = RED4ext::CGlobalFunction::Create("DebugDraw_OverlayFrameMs", "DebugDraw_OverlayFrameMs",
                                                     &Native_OverlayFrameMs);
    function->SetReturnType("Float");
    rtti->RegisterFunction(function);
  }
  {
    auto function = RED4ext::CGlobalFunction::Create("DebugDraw_LabelDistanceScale", "DebugDraw_LabelDistanceScale",
                                                     &Native_LabelDistanceScale);
    function->SetReturnType("Float");
    rtti->RegisterFunction(function);
  }
  Register(rtti, "DebugDraw_SetMaterialFloat", &Native_SetMaterialFloat,
           {{"CName", "channel"}, {"CName", "name"}, {"Float", "value"}});
  Register(rtti, "DebugDraw_SetMaterialColor", &Native_SetMaterialColor,
           {{"CName", "channel"}, {"CName", "name"}, {"Color", "value"}});
  Register(rtti, "DebugDraw_SetMaterialVector", &Native_SetMaterialVector,
           {{"CName", "channel"}, {"CName", "name"}, {"Vector4", "value"}});
  {
    auto reload = RED4ext::CGlobalFunction::Create("DebugDraw_ReloadShaders", "DebugDraw_ReloadShaders",
                                                   &Native_ReloadShaders);
    reload->SetReturnType("Int32");
    rtti->RegisterFunction(reload);
  }
}

} // namespace DebugDraw
