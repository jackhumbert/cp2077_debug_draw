#pragma once

// World overlays: geometry the engine keeps at runtime, returned to script as line point pairs (a, b, a, b, ...)
// for DebugDraw.Lines. Three sources, each read around a centre within a radius:
// - the AI navmesh (OverlaysNavmesh.cpp): Detour tiles of world::RuntimeSystemNavigation
// - trigger volumes (OverlaysTriggers.cpp): TriggerManagerImpl's TriggerObjects and their convexes
// - collision (OverlaysCollision.cpp): a PhysX overlap query on the game's PxScene, then each shape's geometry
// docs/overlays.md has the reversing notes, costs and limits.

#include <RED4ext/Api/PluginHandle.hpp>
#include <RED4ext/Api/Sdk.hpp>
#include <RED4ext/Scripting/Natives/Vector4.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Overlays {

bool Load(const RED4ext::Sdk *aSdk, RED4ext::PluginHandle aHandle);
void Unload(const RED4ext::Sdk *aSdk, RED4ext::PluginHandle aHandle);
void RegisterNatives();

// The overlays Debug Draw draws by itself every frame (DebugDraw_SetWorldOverlays): the same cached builds the
// natives use, appended to aOut as point pairs. aA and aB are as for the natives (navmesh: agent and boundary;
// triggers: channel mask; collision: actors, 1 static, 2 moving). Returns the number of the build the lines come from;
// when that is aHave (the caller already has them), nothing is appended. Game thread.
enum class Source { Navmesh, Triggers, Collision };
uint64_t AppendLines(Source aSource, const RED4ext::Vector4 &aCenter, float aRadius, int32_t aA, int32_t aB,
                     uint64_t aHave, std::vector<RED4ext::Vector4> &aOut);

namespace Detail {

struct V3 {
  float x, y, z;
};

using Lines = std::vector<V3>; // point pairs

// Engine objects shared by the overlays, null when the address or the world is not there.
void *RuntimeScene();
// The runtime system at handle slot aSlot of the scene, if its RTTI type is aTypeName.
void *RuntimeSystem(uint32_t aSlot, const char *aTypeName);

void *Resolve(uint32_t aHash);
void Log(const std::string &aText);

// Shared read lock on a red::old::RWSpinLock (the engine's ScopedSharedLock), held for the object's lifetime.
class SharedLock {
public:
  explicit SharedLock(void *aLock);
  ~SharedLock();
  SharedLock(const SharedLock &) = delete;
  SharedLock &operator=(const SharedLock &) = delete;

private:
  void *m_storage = nullptr;
  bool m_held = false;
};

// Each Build* fills aOut and returns a one-line summary for DebugDraw_OverlayStats.
std::string BuildNavmesh(const V3 &aCenter, float aRadius, int32_t aAgent, bool aBoundary, Lines &aOut);
std::string BuildTriggers(const V3 &aCenter, float aRadius, uint32_t aChannels, Lines &aOut);
std::string BuildCollision(const V3 &aCenter, float aRadius, int32_t aActors, Lines &aOut);

// Where a build was when it faulted, for the log: a stage name and the engine object it was reading.
extern const char *volatile g_stage;
extern const void *volatile g_object;

bool LoadNavmesh();
bool LoadTriggers();
bool LoadCollision();

} // namespace Detail
} // namespace Overlays
