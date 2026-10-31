#include <RED4ext/RED4ext.hpp>

#include "Overlays.hpp"

#include <cmath>
#include <cstdint>
#include <string>

// The AI navmesh is Recast/Detour, kept by world::RuntimeSystemNavigation (runtime scene slot 2):
//   +0x48 red::old::RWSpinLock, taken shared by every query (FindNavmeshPointInSphere)
//   +0x50 world::Navmesh: dtNavMesh* per NavGen::AgentSize (0 Human, 1 Vehicle), then dtTileCache* per size
// Detour here is Y up: game (x, y, z) is Recast (x, z, y). CDPR's build uses 64-bit poly refs and triangles only:
//   dtNavMesh  +0x1C orig[3], +0x28 tileWidth, +0x2C tileHeight
//   dtMeshTile (0x68) +0x08 header, +0x10 polys, +0x18 verts (float[3] each), +0x60 next
//   dtMeshHeader +0x08 x, +0x0C y, +0x18 polyCount, +0x1C vertCount, +0x48 bmin[3], +0x54 bmax[3]
//   dtPoly (20 bytes) +0x00 firstLink, +0x04 verts[3], +0x0A neis[3], +0x10 flags, +0x12 vertCount,
//                     +0x13 area | type << 6 (type 1 = off-mesh connection)
// neis: 0 is a boundary (wall), n is poly n-1 of the same tile, 0x8000 | side is a link to a neighbouring tile.

namespace Overlays::Detail {
namespace {

// int dtNavMesh::getTilesAt(int x, int y, const dtMeshTile** tiles, int maxTiles) const
constexpr uint32_t kGetTilesAt = 3827700168;
using GetTilesAt_t = int (*)(const void *aNavMesh, int aX, int aY, const void **aTiles, int aMax);
GetTilesAt_t s_getTilesAt = nullptr;

constexpr uint32_t kNavigationSlot = 2;
constexpr size_t kNavigationLock = 0x48;
constexpr size_t kNavigationNavmesh = 0x50;

template <typename T> T At(const void *aBase, size_t aOffset) {
  return *reinterpret_cast<const T *>(reinterpret_cast<const char *>(aBase) + aOffset);
}

V3 Game(const float *aRecast, float aLift) { return {aRecast[0], aRecast[2], aRecast[1] + aLift}; }

} // namespace

bool LoadNavmesh() {
  s_getTilesAt = reinterpret_cast<GetTilesAt_t>(Resolve(kGetTilesAt));
  return s_getTilesAt != nullptr;
}

std::string BuildNavmesh(const V3 &aCenter, float aRadius, int32_t aAgent, bool aBoundary, Lines &aOut) {
  auto system = reinterpret_cast<char *>(RuntimeSystem(kNavigationSlot, "worldRuntimeSystemNavigation"));
  if (!system) {
    return "no world::RuntimeSystemNavigation";
  }
  if (aAgent < 0 || aAgent > 1) {
    aAgent = 0;
  }
  SharedLock lock(system + kNavigationLock);
  auto navmesh = At<const char *>(system + kNavigationNavmesh, 8 * aAgent);
  if (!navmesh) {
    return "no dtNavMesh";
  }
  const float *orig = reinterpret_cast<const float *>(navmesh + 0x1C);
  const float tileW = At<float>(navmesh, 0x28), tileH = At<float>(navmesh, 0x2C);
  if (!(tileW > 0.0f) || !(tileH > 0.0f)) {
    return "bad tile size";
  }
  // Recast centre and box.
  const float rc[3] = {aCenter.x, aCenter.z, aCenter.y};
  const int tx0 = static_cast<int>(std::floor((rc[0] - aRadius - orig[0]) / tileW));
  const int tx1 = static_cast<int>(std::floor((rc[0] + aRadius - orig[0]) / tileW));
  const int ty0 = static_cast<int>(std::floor((rc[2] - aRadius - orig[2]) / tileH));
  const int ty1 = static_cast<int>(std::floor((rc[2] + aRadius - orig[2]) / tileH));
  if ((tx1 - tx0) > 64 || (ty1 - ty0) > 64) {
    return "radius too large";
  }
  const float r2 = aRadius * aRadius;
  const float lift = 0.05f;
  int tiles = 0, polys = 0;
  for (int ty = ty0; ty <= ty1; ++ty) {
    for (int tx = tx0; tx <= tx1; ++tx) {
      const void *found[32];
      const int n = s_getTilesAt(navmesh, tx, ty, found, 32);
      for (int t = 0; t < n; ++t) {
        auto tile = reinterpret_cast<const char *>(found[t]);
        auto header = At<const char *>(tile, 0x08);
        auto polyData = At<const uint8_t *>(tile, 0x10);
        auto verts = At<const float *>(tile, 0x18);
        if (!header || !polyData || !verts) {
          continue;
        }
        const int polyCount = At<int32_t>(header, 0x18), vertCount = At<int32_t>(header, 0x1C);
        const float *bmin = reinterpret_cast<const float *>(header + 0x48);
        const float *bmax = reinterpret_cast<const float *>(header + 0x54);
        if (polyCount <= 0 || polyCount > (1 << 20) || bmin[1] > rc[1] + aRadius || bmax[1] < rc[1] - aRadius) {
          continue;
        }
        ++tiles;
        for (int i = 0; i < polyCount; ++i) {
          const uint8_t *poly = polyData + 20 * i;
          if ((poly[0x13] >> 6) == 1) {
            continue; // off-mesh connection
          }
          const int nv = poly[0x12];
          if (nv < 3 || nv > 3) {
            continue;
          }
          const uint16_t *vi = reinterpret_cast<const uint16_t *>(poly + 0x04);
          const uint16_t *neis = reinterpret_cast<const uint16_t *>(poly + 0x0A);
          bool isNear = false;
          for (int j = 0; j < nv && !isNear; ++j) {
            if (vi[j] >= vertCount) {
              isNear = false;
              break;
            }
            const float *v = verts + 3 * vi[j];
            const float dx = v[0] - rc[0], dy = v[1] - rc[1], dz = v[2] - rc[2];
            isNear = dx * dx + dz * dz <= r2 && std::fabs(dy) <= aRadius;
          }
          if (!isNear) {
            continue;
          }
          ++polys;
          for (int j = 0; j < nv; ++j) {
            const uint16_t nei = neis[j];
            const bool wall = nei == 0;
            if (wall != aBoundary) {
              continue;
            }
            // An inner edge is shared with poly nei-1 of the same tile: draw it from the lower index only.
            if (nei != 0 && !(nei & 0x8000) && static_cast<int>(nei) - 1 < i) {
              continue;
            }
            const uint16_t a = vi[j], b = vi[(j + 1) % nv];
            if (a >= vertCount || b >= vertCount) {
              continue;
            }
            aOut.push_back(Game(verts + 3 * a, lift));
            aOut.push_back(Game(verts + 3 * b, lift));
          }
        }
      }
    }
  }
  return std::to_string(tiles) + " tiles, " + std::to_string(polys) + " polys, " + std::to_string(aOut.size() / 2) +
         (aBoundary ? " boundary" : " inner") + " edges (agent " + std::to_string(aAgent) + ")";
}

} // namespace Overlays::Detail
