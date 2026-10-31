#include <RED4ext/RED4ext.hpp>

#include "Overlays.hpp"

#include <cmath>
#include <cstdint>
#include <string>

// Trigger volumes live in TriggerManagerImpl, owned by world::RuntimeSystemTriggers (runtime scene slot 9, the
// manager pointer at +0x48, as its OnRenderDebug reads it). Every trigger area node, trigger component and
// quest area in the streamed world registers a TriggerObject there:
//   TriggerManagerImpl +0x20 objects (red::SharedStorage<TriggerObject>, 16 bytes each), +0x2C count,
//                      +0x564 red::old::RWSpinLock (GetTriggersAtPoint takes it)
//   TriggerObject +0x00/+0x10/+0x20 rotation rows (world = x*row0 + y*row1 + z*row2 + position),
//                 +0xA8 DynArray<UniquePtr<TriggerConvex>> (+0xB4 count),
//                 +0xBC TriggerChannel mask of the activators it reacts to (OR of its notifiers' channels)
//   TriggerConvex +0x08 AreaConvex* (the shape, local space), +0x20 int position * 256 (fixed point, world)
//   AreaConvex +0x78 half-edges (48 bytes: +0x20 vertex, +0x24 next edge), +0x84 count,
//              +0x88 vertices (Vector3), +0x94 count

namespace Overlays::Detail {
namespace {

constexpr uint32_t kTriggersSlot = 9;
constexpr size_t kTriggersManager = 0x48;
constexpr size_t kManagerObjects = 0x20;
constexpr size_t kManagerObjectCount = 0x2C;
constexpr size_t kManagerLock = 0x564;

template <typename T> T At(const void *aBase, size_t aOffset) {
  return *reinterpret_cast<const T *>(reinterpret_cast<const char *>(aBase) + aOffset);
}

float SegmentDistance2(const V3 &p, const V3 &a, const V3 &b) {
  const float abx = b.x - a.x, aby = b.y - a.y, abz = b.z - a.z;
  const float apx = p.x - a.x, apy = p.y - a.y, apz = p.z - a.z;
  const float len2 = abx * abx + aby * aby + abz * abz;
  float t = len2 > 0.0f ? (apx * abx + apy * aby + apz * abz) / len2 : 0.0f;
  t = std::fmin(1.0f, std::fmax(0.0f, t));
  const float dx = apx - abx * t, dy = apy - aby * t, dz = apz - abz * t;
  return dx * dx + dy * dy + dz * dz;
}

} // namespace

bool LoadTriggers() { return true; }

std::string BuildTriggers(const V3 &aCenter, float aRadius, uint32_t aChannels, Lines &aOut) {
  auto system = reinterpret_cast<char *>(RuntimeSystem(kTriggersSlot, "worldRuntimeSystemTriggers"));
  if (!system) {
    return "no world::RuntimeSystemTriggers";
  }
  auto manager = At<const char *>(system, kTriggersManager);
  if (!manager) {
    return "no TriggerManagerImpl";
  }
  SharedLock lock(const_cast<char *>(manager) + kManagerLock);
  auto objects = At<const char *>(manager, kManagerObjects);
  const uint32_t count = At<uint32_t>(manager, kManagerObjectCount);
  if (!objects || count > (1u << 20)) {
    return "no trigger objects";
  }
  const float r2 = aRadius * aRadius;
  uint32_t shown = 0, total = 0;
  for (uint32_t i = 0; i < count; ++i) {
    auto object = At<const char *>(objects, 16 * i);
    if (!object) {
      continue;
    }
    ++total;
    const uint32_t channels = At<uint32_t>(object, 0xBC);
    if (aChannels != 0 && (channels & aChannels) == 0) {
      continue;
    }
    const float *row0 = reinterpret_cast<const float *>(object + 0x00);
    const float *row1 = reinterpret_cast<const float *>(object + 0x10);
    const float *row2 = reinterpret_cast<const float *>(object + 0x20);
    auto convexes = At<const char *const *>(object, 0xA8);
    const uint32_t convexCount = At<uint32_t>(object, 0xB4);
    if (!convexes || convexCount > 4096) {
      continue;
    }
    const size_t before = aOut.size();
    bool isNear = false;
    V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (uint32_t k = 0; k < convexCount; ++k) {
      auto convex = convexes[k];
      if (!convex) {
        continue;
      }
      auto area = At<const char *>(convex, 0x08);
      if (!area) {
        continue;
      }
      const V3 pos{At<int32_t>(convex, 0x20) / 256.0f, At<int32_t>(convex, 0x24) / 256.0f,
                   At<int32_t>(convex, 0x28) / 256.0f};
      auto verts = At<const float *>(area, 0x88);
      const uint32_t nv = At<uint32_t>(area, 0x94);
      auto edges = At<const char *>(area, 0x78);
      const uint32_t ne = At<uint32_t>(area, 0x84);
      if (!verts || !edges || nv > 65536 || ne > 65536) {
        continue;
      }
      auto world = [&](uint16_t aIndex) {
        const float *v = verts + 3 * aIndex;
        return V3{v[0] * row0[0] + v[1] * row1[0] + v[2] * row2[0] + pos.x,
                  v[0] * row0[1] + v[1] * row1[1] + v[2] * row2[1] + pos.y,
                  v[0] * row0[2] + v[1] * row1[2] + v[2] * row2[2] + pos.z};
      };
      for (uint32_t e = 0; e < ne; ++e) {
        const char *edge = edges + 48 * e;
        const uint16_t a = At<uint16_t>(edge, 0x20);
        const uint16_t next = At<uint16_t>(edge, 0x24);
        if (next >= ne) {
          continue;
        }
        const uint16_t b = At<uint16_t>(edges + 48 * next, 0x20);
        // Each edge is two half-edges (a to b and b to a): draw it once.
        if (a >= nv || b >= nv || a >= b) {
          continue;
        }
        const V3 wa = world(a), wb = world(b);
        isNear = isNear || SegmentDistance2(aCenter, wa, wb) <= r2;
        for (const V3 &w : {wa, wb}) {
          lo = {std::fmin(lo.x, w.x), std::fmin(lo.y, w.y), std::fmin(lo.z, w.z)};
          hi = {std::fmax(hi.x, w.x), std::fmax(hi.y, w.y), std::fmax(hi.z, w.z)};
        }
        aOut.push_back(wa);
        aOut.push_back(wb);
      }
    }
    // A trigger is drawn whole when any edge comes within the radius or the centre is inside its box (a big
    // area around the player has no edge nearby).
    const bool inside = aCenter.x >= lo.x && aCenter.x <= hi.x && aCenter.y >= lo.y && aCenter.y <= hi.y &&
                        aCenter.z >= lo.z && aCenter.z <= hi.z;
    if (!isNear && !inside) {
      aOut.resize(before);
    } else if (aOut.size() > before) {
      ++shown;
    }
  }
  return std::to_string(shown) + " of " + std::to_string(total) + " triggers, " + std::to_string(aOut.size() / 2) +
         " edges (channels 0x" + [&] {
           char buf[16];
           snprintf(buf, sizeof(buf), "%X", aChannels);
           return std::string(buf);
         }() + ")";
}

} // namespace Overlays::Detail
