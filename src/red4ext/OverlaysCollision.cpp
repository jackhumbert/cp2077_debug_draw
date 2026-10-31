#include <RED4ext/RED4ext.hpp>

#include "Overlays.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <limits>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Collision is the game's PhysX 3.4 scene (PhysX3_x64.dll), reached the way physics::SyncOverlapN does:
//   world::GetSceneID(RuntimeScene) -> physics::SceneID; physics::g_sceneLookup->GetScene(id) -> physics::PhysXScene
//   PhysXScene +0x18 physx::PxScene*, +0x134 Vector3 origin (PhysX positions are world minus this)
// Then one PxScene::overlap (vtable +0x300, as SyncOverlapN calls it) with a box around the centre returns every
// static and dynamic shape it touches, and each shape's geometry is drawn as lines. PhysX virtuals used, offsets
// checked against the game's own calls (cloth AddTrianglesToContext, PxConvexMeshRepXSerializer, PxShapeExt):
//   PxActor +0x28 getType (0 static, 1 dynamic), PxRigidActor +0xA0 getGlobalPose
//   PxShape +0x38 getGeometryType, +0x50 box, +0x58 sphere, +0x60 capsule, +0x70 convex, +0x78 triangle mesh
//           geometry getters, +0x98 getLocalPose
//   PxConvexMesh +0x28 getNbVertices, +0x30 getVertices, +0x38 getIndexBuffer, +0x40 getNbPolygons,
//                +0x48 getPolygonData (the last two also from the cloth collider setup)
//   PxTriangleMesh +0x28 getNbVertices, +0x30 getVertices, +0x48 getNbTriangles, +0x50 getTriangles,
//                  +0x58 getTriangleMeshFlags (returned through a hidden pointer; 2: 16-bit indices)

namespace Overlays::Detail {
namespace {

// physics::SceneID world::GetSceneID(const world::RuntimeScene&)
constexpr uint32_t kGetSceneID = 2913539499;
// physics::PhysXScene* physics::SceneIdLookup::GetScene(physics::SceneID)
constexpr uint32_t kGetScene = 926881634;
// red::UniquePtr<physics::SceneIdLookup> physics::g_sceneLookup
constexpr uint32_t kSceneLookup = 3885377886;

using GetSceneID_t = uint32_t *(*)(uint32_t *aOut, const void *aRuntimeScene);
using GetScene_t = void *(*)(void *aLookup, uint32_t aSceneId);
GetSceneID_t s_getSceneID = nullptr;
GetScene_t s_getScene = nullptr;
void **s_sceneLookup = nullptr;

struct Quat {
  float x, y, z, w;
};
struct PxTransform {
  Quat q;
  V3 p;
};
static_assert(sizeof(PxTransform) == 28);

// physx::PxOverlapHit as the game's PhysX lays it out (32 bytes: SyncOverlapN steps its touches by 32).
struct OverlapHit {
  void *actor;
  void *shape;
  uint32_t faceIndex;
  uint8_t pad[12];
};
static_assert(sizeof(OverlapHit) == 32);

// physx::PxHitCallback<PxOverlapHit>: vtable (processTouches, finalizeQuery, destructor), block, hasBlock, touches,
// maxNbTouches, nbTouches.
struct OverlapCallback {
  virtual bool processTouches(const OverlapHit *aBuffer, uint32_t aCount) {
    hits->insert(hits->end(), aBuffer, aBuffer + aCount);
    return hits->size() < 100000;
  }
  virtual void finalizeQuery() {}
  virtual ~OverlapCallback() = default;

  OverlapHit block{};
  bool hasBlock = false;
  OverlapHit *touches = nullptr;
  uint32_t maxNbTouches = 0;
  uint32_t nbTouches = 0;
  std::vector<OverlapHit> *hits = nullptr;
};
static_assert(offsetof(OverlapCallback, touches) == 48 && offsetof(OverlapCallback, nbTouches) == 60);

struct PxQueryFilterData {
  uint32_t word[4];
  uint16_t flags;
};

template <typename R, typename... A> R Virtual(const void *aObject, size_t aOffset, A... aArgs) {
  auto vtable = *reinterpret_cast<void *const *const *>(aObject);
  auto fn = reinterpret_cast<R (*)(const void *, A...)>(vtable[aOffset / 8]);
  return fn(aObject, aArgs...);
}

V3 Rotate(const Quat &q, const V3 &v) {
  // v + 2 * cross(q.xyz, cross(q.xyz, v) + w * v)
  const V3 u{q.x, q.y, q.z};
  const V3 c1{u.y * v.z - u.z * v.y + q.w * v.x, u.z * v.x - u.x * v.z + q.w * v.y, u.x * v.y - u.y * v.x + q.w * v.z};
  return {v.x + 2.0f * (u.y * c1.z - u.z * c1.y), v.y + 2.0f * (u.z * c1.x - u.x * c1.z),
          v.z + 2.0f * (u.x * c1.y - u.y * c1.x)};
}

V3 Apply(const PxTransform &t, const V3 &v) {
  const V3 r = Rotate(t.q, v);
  return {r.x + t.p.x, r.y + t.p.y, r.z + t.p.z};
}

PxTransform Mul(const PxTransform &a, const PxTransform &b) {
  const Quat &p = a.q, &q = b.q;
  Quat r{p.w * q.x + p.x * q.w + p.y * q.z - p.z * q.y, p.w * q.y - p.x * q.z + p.y * q.w + p.z * q.x,
         p.w * q.z + p.x * q.y - p.y * q.x + p.z * q.w, p.w * q.w - p.x * q.x - p.y * q.y - p.z * q.z};
  return {r, Apply(a, b.p)};
}

// physx::PxMeshScale: v' = R^-1 * S * R * v with R the rotation (PhysX scales along the rotated axes).
struct MeshScale {
  V3 s;
  Quat r;
  V3 Apply(const V3 &v) const {
    if (s.x == 1.0f && s.y == 1.0f && s.z == 1.0f) {
      return v;
    }
    const Quat inv{-r.x, -r.y, -r.z, r.w};
    V3 a = Rotate(r, v);
    a = {a.x * s.x, a.y * s.y, a.z * s.z};
    return Rotate(inv, a);
  }
};

float SegmentDistance2(const V3 &p, const V3 &a, const V3 &b) {
  const float abx = b.x - a.x, aby = b.y - a.y, abz = b.z - a.z;
  const float apx = p.x - a.x, apy = p.y - a.y, apz = p.z - a.z;
  const float len2 = abx * abx + aby * aby + abz * abz;
  float t = len2 > 0.0f ? (apx * abx + apy * aby + apz * abz) / len2 : 0.0f;
  t = std::fmin(1.0f, std::fmax(0.0f, t));
  const float dx = apx - abx * t, dy = apy - aby * t, dz = apz - abz * t;
  return dx * dx + dy * dy + dz * dz;
}

constexpr size_t kMaxPoints = 600000;

struct Emitter {
  const V3 &center;
  float r2;
  Lines &out;
  void Line(const V3 &a, const V3 &b) {
    if (out.size() < kMaxPoints && SegmentDistance2(center, a, b) <= r2) {
      out.push_back(a);
      out.push_back(b);
    }
  }
  void Circle(const PxTransform &t, int axis, float radius, float offset, int segments = 16) {
    V3 prev{};
    for (int i = 0; i <= segments; ++i) {
      const float a = 6.2831853f * i / segments;
      const float c = std::cos(a) * radius, s = std::sin(a) * radius;
      V3 local = axis == 0 ? V3{offset, c, s} : axis == 1 ? V3{c, offset, s} : V3{c, s, offset};
      const V3 w = Apply(t, local);
      if (i > 0) {
        Line(prev, w);
      }
      prev = w;
    }
  }
};

void Box(Emitter &e, const PxTransform &t, const V3 &h) {
  V3 c[8];
  for (int i = 0; i < 8; ++i) {
    c[i] = Apply(t, {(i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z});
  }
  const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
  for (auto &edge : edges) {
    e.Line(c[edge[0]], c[edge[1]]);
  }
}

// Capsule along local X, halfHeight from the centre to each cap's centre.
void Capsule(Emitter &e, const PxTransform &t, float radius, float halfHeight) {
  e.Circle(t, 0, radius, halfHeight);
  e.Circle(t, 0, radius, -halfHeight);
  for (int i = 0; i < 4; ++i) {
    const float a = 1.5707963f * i;
    const V3 o{0.0f, std::cos(a) * radius, std::sin(a) * radius};
    e.Line(Apply(t, {-halfHeight, o.y, o.z}), Apply(t, {halfHeight, o.y, o.z}));
  }
  // Cap arcs in the XY and XZ planes.
  for (int plane = 0; plane < 2; ++plane) {
    for (int side = -1; side <= 1; side += 2) {
      V3 prev{};
      for (int i = 0; i <= 8; ++i) {
        const float a = -1.5707963f + 3.1415927f * i / 8;
        const float x = side * (halfHeight + std::cos(a) * radius), s = std::sin(a) * radius;
        const V3 w = Apply(t, plane == 0 ? V3{x, s, 0.0f} : V3{x, 0.0f, s});
        if (i > 0) {
          e.Line(prev, w);
        }
        prev = w;
      }
    }
  }
}

void Convex(Emitter &e, const PxTransform &t, const char *aGeometry) {
  MeshScale scale{*reinterpret_cast<const V3 *>(aGeometry + 4), *reinterpret_cast<const Quat *>(aGeometry + 16)};
  auto mesh = *reinterpret_cast<void *const *>(aGeometry + 32);
  if (!mesh) {
    return;
  }
  const uint32_t nv = Virtual<uint32_t>(mesh, 0x28);
  auto verts = Virtual<const V3 *>(mesh, 0x30);
  auto indices = Virtual<const uint8_t *>(mesh, 0x38);
  const uint32_t np = Virtual<uint32_t>(mesh, 0x40);
  if (!verts || !indices || nv > 256 || np > 256) {
    return;
  }
  struct HullPolygon {
    float plane[4];
    uint16_t nbVerts;
    uint16_t indexBase;
  };
  for (uint32_t p = 0; p < np; ++p) {
    HullPolygon poly{};
    if (!Virtual<bool>(mesh, 0x48, p, &poly) || poly.nbVerts > 64) {
      continue;
    }
    for (uint16_t k = 0; k < poly.nbVerts; ++k) {
      const uint8_t a = indices[poly.indexBase + k], b = indices[poly.indexBase + (k + 1) % poly.nbVerts];
      // Each hull edge belongs to two polygons, in opposite directions: draw it once.
      if (a >= b || a >= nv || b >= nv) {
        continue;
      }
      e.Line(Apply(t, scale.Apply(verts[a])), Apply(t, scale.Apply(verts[b])));
    }
  }
}

void TriangleMesh(Emitter &e, const PxTransform &t, const char *aGeometry, uint64_t &aTriangles) {
  MeshScale scale{*reinterpret_cast<const V3 *>(aGeometry + 4), *reinterpret_cast<const Quat *>(aGeometry + 16)};
  auto mesh = *reinterpret_cast<void *const *>(aGeometry + 40);
  if (!mesh) {
    return;
  }
  const uint32_t nv = Virtual<uint32_t>(mesh, 0x28);
  auto verts = Virtual<const V3 *>(mesh, 0x30);
  const uint32_t nt = Virtual<uint32_t>(mesh, 0x48);
  auto tris = Virtual<const void *>(mesh, 0x50);
  // PxTriangleMeshFlags is a class: returned through a hidden pointer (the cloth code tests byte [rax] & 2).
  uint32_t flagsOut = 0;
  const uint8_t flags = *Virtual<const uint8_t *>(mesh, 0x58, &flagsOut);
  if (!verts || !tris || nv == 0) {
    return;
  }
  const bool small = (flags & 2) != 0;
  const float r = std::sqrt(e.r2);
  aTriangles += nt;
  for (uint32_t i = 0; i < nt && e.out.size() < kMaxPoints; ++i) {
    uint32_t idx[3];
    for (int k = 0; k < 3; ++k) {
      idx[k] = small ? reinterpret_cast<const uint16_t *>(tris)[3 * i + k] : reinterpret_cast<const uint32_t *>(tris)[3 * i + k];
    }
    if (idx[0] >= nv || idx[1] >= nv || idx[2] >= nv) {
      continue;
    }
    V3 w[3];
    bool isNear = false;
    for (int k = 0; k < 3; ++k) {
      w[k] = Apply(t, scale.Apply(verts[idx[k]]));
      isNear = isNear || (std::fabs(w[k].x - e.center.x) <= r && std::fabs(w[k].y - e.center.y) <= r &&
                      std::fabs(w[k].z - e.center.z) <= r);
    }
    if (!isNear) {
      // A big triangle can cross the sphere with all its corners outside: check its edges.
      bool crosses = false;
      for (int k = 0; k < 3 && !crosses; ++k) {
        crosses = SegmentDistance2(e.center, w[k], w[(k + 1) % 3]) <= e.r2;
      }
      if (!crosses) {
        continue;
      }
    }
    // Shared edges come twice, once per triangle: cheaper than finding the pairs.
    for (int k = 0; k < 3; ++k) {
      e.Line(w[k], w[(k + 1) % 3]);
    }
  }
}

// Moving bodies are drawn every frame, so each shape's wireframe is kept in its own space and only placed at the
// shape's current pose each frame. The key is the shape pointer from this frame's query (so alive) plus a hash of its
// geometry (type, extents, mesh pointer, scale): a freed shape whose address is reused for other geometry gets a new
// key. Entries unused for a while are dropped.
struct ShapeKey {
  const void *shape;
  uint64_t geometry;
  bool operator==(const ShapeKey &aOther) const { return shape == aOther.shape && geometry == aOther.geometry; }
};
struct ShapeKeyHash {
  size_t operator()(const ShapeKey &aKey) const {
    return std::hash<const void *>()(aKey.shape) ^ static_cast<size_t>(aKey.geometry * 0x9E3779B97F4A7C15ull);
  }
};
struct ShapeLines {
  Lines local; // point pairs in the shape's space
  Lines world; // local placed at pose: reused while the shape has not moved (most "dynamic" bodies are parked)
  PxTransform pose{};
  uint64_t used = 0;
};
std::unordered_map<ShapeKey, ShapeLines, ShapeKeyHash> s_shapeCache;
uint64_t s_buildNumber = 0;
constexpr uint64_t kShapeCacheKeep = 120; // builds: about a second at the per-frame rate (two builds a frame)

uint64_t HashBytes(const void *aData, size_t aSize, int32_t aType) {
  uint64_t hash = 0xCBF29CE484222325ull ^ static_cast<uint32_t>(aType);
  for (size_t i = 0; i < aSize; ++i) {
    hash = (hash ^ static_cast<const uint8_t *>(aData)[i]) * 0x100000001B3ull;
  }
  return hash;
}

const PxTransform kIdentity{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};

// One shape's geometry as lines, posed by aPose.
void EmitGeometry(Emitter &e, const PxTransform &aPose, int32_t aType, const char *aGeometry, uint64_t &aTriangles) {
  switch (aType) {
  case 0: { // sphere
    const float radius = *reinterpret_cast<const float *>(aGeometry + 4);
    for (int axis = 0; axis < 3; ++axis) {
      e.Circle(aPose, axis, radius, 0.0f);
    }
    break;
  }
  case 2: // capsule
    Capsule(e, aPose, *reinterpret_cast<const float *>(aGeometry + 4), *reinterpret_cast<const float *>(aGeometry + 8));
    break;
  case 3: // box
    Box(e, aPose, *reinterpret_cast<const V3 *>(aGeometry + 4));
    break;
  case 4: // convex mesh
    g_stage = "convex mesh";
    g_object = *reinterpret_cast<void *const *>(aGeometry + 32);
    Convex(e, aPose, aGeometry);
    break;
  case 5: // triangle mesh
    g_stage = "triangle mesh";
    g_object = *reinterpret_cast<void *const *>(aGeometry + 40);
    TriangleMesh(e, aPose, aGeometry, aTriangles);
    break;
  default:
    break;
  }
}

} // namespace

bool LoadCollision() {
  s_getSceneID = reinterpret_cast<GetSceneID_t>(Resolve(kGetSceneID));
  s_getScene = reinterpret_cast<GetScene_t>(Resolve(kGetScene));
  s_sceneLookup = reinterpret_cast<void **>(Resolve(kSceneLookup));
  return s_getSceneID && s_getScene && s_sceneLookup;
}

std::string BuildCollision(const V3 &aCenter, float aRadius, int32_t aActors, Lines &aOut) {
  auto runtimeScene = RuntimeScene();
  if (!runtimeScene || !*s_sceneLookup) {
    return "no physics scene";
  }
  uint32_t sceneId = 0xFFFFFFFF;
  s_getSceneID(&sceneId, runtimeScene);
  auto physxScene = reinterpret_cast<const char *>(s_getScene(*s_sceneLookup, sceneId));
  if (!physxScene) {
    return "no PhysXScene";
  }
  auto pxScene = *reinterpret_cast<void *const *>(physxScene + 0x18);
  const V3 origin = *reinterpret_cast<const V3 *>(physxScene + 0x134);
  if (!pxScene) {
    return "no PxScene";
  }

  struct BoxGeometry {
    int32_t type;
    V3 halfExtents;
  } box{3, {aRadius, aRadius, aRadius}};
  const PxTransform pose{{0.0f, 0.0f, 0.0f, 1.0f}, {aCenter.x - origin.x, aCenter.y - origin.y, aCenter.z - origin.z}};
  // eSTATIC | eDYNAMIC | eNO_BLOCK, no filter data: every shape the box touches.
  PxQueryFilterData filter{{0, 0, 0, 0}, 0x1 | 0x2 | 0x20};
  std::vector<OverlapHit> hits;
  std::vector<OverlapHit> buffer(1024);
  OverlapCallback callback;
  callback.touches = buffer.data();
  callback.maxNbTouches = static_cast<uint32_t>(buffer.size());
  callback.hits = &hits;
  // bool PxScene::overlap(const PxGeometry&, const PxTransform&, PxOverlapCallback&, const PxQueryFilterData&,
  //                       PxQueryFilterCallback*) const
  using Clock = std::chrono::steady_clock;
  const auto queryStart = Clock::now();
  g_stage = "PxScene::overlap";
  g_object = pxScene;
  Virtual<bool>(pxScene, 0x300, static_cast<const void *>(&box), &pose, static_cast<void *>(&callback),
                static_cast<const void *>(&filter), static_cast<void *>(nullptr));
  hits.insert(hits.end(), buffer.begin(), buffer.begin() + std::min<uint32_t>(callback.nbTouches, 1024));

  const auto queryEnd = Clock::now();
  g_stage = "hits";
  if (aActors == 8) {
    return std::to_string(hits.size()) + " hits (query only)";
  }
  Emitter e{aCenter, aRadius * aRadius, aOut};
  // Moving bodies only (rebuilt every frame): place each shape's kept wireframe instead of rebuilding it.
  const bool cached = aActors == 2;
  ++s_buildNumber;
  std::set<std::pair<void *, void *>> seen;
  int counts[8] = {};
  int statics = 0, dynamics = 0;
  uint64_t triangles = 0;
  for (const auto &hit : hits) {
    if (!hit.actor || !hit.shape || !seen.insert({hit.actor, hit.shape}).second) {
      continue;
    }
    g_stage = "PxActor::getType";
    g_object = hit.actor;
    const uint16_t actorType = static_cast<uint16_t>(Virtual<uint32_t>(hit.actor, 0x28));
    if (actorType > 1) {
      continue; // particles, cloth
    }
    if ((aActors == 1 && actorType != 0) || (aActors == 2 && actorType != 1)) {
      continue;
    }
    (actorType == 0 ? statics : dynamics)++;
    PxTransform actorPose{}, localPose{};
    g_stage = "poses";
    Virtual<PxTransform *>(hit.actor, 0xA0, &actorPose);
    Virtual<PxTransform *>(hit.shape, 0x98, &localPose);
    PxTransform t = Mul(actorPose, localPose);
    t.p = {t.p.x + origin.x, t.p.y + origin.y, t.p.z + origin.z};
    g_stage = "PxShape::getGeometryType";
    g_object = hit.shape;
    const int32_t geometryType = Virtual<int32_t>(hit.shape, 0x38);
    g_stage = "geometry";
    alignas(16) char geometry[64] = {};
    if (geometryType >= 0 && geometryType < 8) {
      counts[geometryType]++;
    }
    // The geometry getter for this type (sphere 0x58, capsule 0x60, box 0x50, convex 0x70, triangle mesh 0x78).
    const size_t getter = geometryType == 0 ? 0x58 : geometryType == 2 ? 0x60 : geometryType == 3 ? 0x50
                          : geometryType == 4 ? 0x70 : geometryType == 5 ? 0x78 : 0;
    if (!getter || !Virtual<bool>(hit.shape, getter, static_cast<void *>(geometry))) {
      continue; // plane, height field: not drawn
    }
    if (!cached) {
      EmitGeometry(e, t, geometryType, geometry, triangles);
      continue;
    }
    auto &entry = s_shapeCache[{hit.shape, HashBytes(geometry, sizeof(geometry), geometryType)}];
    if (entry.used == 0) {
      const V3 origin0{0.0f, 0.0f, 0.0f};
      Emitter local{origin0, std::numeric_limits<float>::infinity(), entry.local};
      EmitGeometry(local, kIdentity, geometryType, geometry, triangles);
    }
    entry.used = s_buildNumber;
    if (entry.world.size() != entry.local.size() || std::memcmp(&entry.pose, &t, sizeof(t)) != 0) {
      entry.pose = t;
      entry.world.resize(entry.local.size());
      for (size_t i = 0; i < entry.local.size(); ++i) {
        entry.world[i] = Apply(t, entry.local[i]);
      }
    }
    for (size_t i = 0; i + 1 < entry.world.size(); i += 2) {
      e.Line(entry.world[i], entry.world[i + 1]);
    }
  }
  if (cached && s_shapeCache.size() > 4096) {
    std::erase_if(s_shapeCache, [](const auto &aEntry) { return aEntry.second.used + kShapeCacheKeep < s_buildNumber; });
  }
  char timing[96];
  snprintf(timing, sizeof(timing), " [query %.2f ms, shapes %.2f ms]",
           std::chrono::duration<double, std::milli>(queryEnd - queryStart).count(),
           std::chrono::duration<double, std::milli>(Clock::now() - queryEnd).count());
  return std::string(timing) + " " + std::to_string(hits.size()) + " hits, " + std::to_string(statics) + " static / " +
         std::to_string(dynamics) + " dynamic shapes (sphere " + std::to_string(counts[0]) + ", capsule " +
         std::to_string(counts[2]) + ", box " + std::to_string(counts[3]) + ", convex " + std::to_string(counts[4]) +
         ", mesh " + std::to_string(counts[5]) + " with " + std::to_string(triangles) + " tris, heightfield " +
         std::to_string(counts[6]) + "), " + std::to_string(aOut.size() / 2) + " lines" +
         (aOut.size() >= kMaxPoints ? " (capped)" : "");
}

} // namespace Overlays::Detail
