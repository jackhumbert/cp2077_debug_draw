#include <RED4ext/RED4ext.hpp>

#include "Overlays.hpp"

#include <RED4ext/CName.hpp>
#include <RED4ext/CString.hpp>
#include <RED4ext/GameEngine.hpp>
#include <RED4ext/ISerializable.hpp>
#include <RED4ext/RTTISystem.hpp>
#include <RED4ext/Relocation.hpp>
#include <RED4ext/Scripting/Functions.hpp>
#include <RED4ext/Scripting/Natives/Vector4.hpp>
#include <RED4ext/Scripting/Stack.hpp>
#include <RED4ext/Scripting/Utils.hpp>

#include <Windows.h>

#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

namespace Overlays {
namespace Detail {
namespace {

const RED4ext::Sdk *s_sdk = nullptr;
RED4ext::PluginHandle s_handle = nullptr;

// red::ScopedSharedLock<red::old::RWSpinLock&>: an 8-byte object holding the lock pointer; the ctor takes the read
// side, the dtor drops it.
constexpr uint32_t kScopedSharedLock_ctor = 1248140025;
constexpr uint32_t kScopedSharedLock_dtor = 1509364729;
using SharedLockCtor_t = void *(*)(void *aStorage, void *aLock);
using SharedLockDtor_t = void (*)(void *aStorage);
SharedLockCtor_t s_lockCtor = nullptr;
SharedLockDtor_t s_lockDtor = nullptr;

// Locks the overlay code holds on this thread, so a fault while reading engine memory can still let go of them
// (a reader count left behind would stall the engine's writers for good).
thread_local std::vector<void *> t_held;

// CGameEngine::framework (+0x348), CGameFramework::runtimeScene (+0x18).
constexpr size_t kEngineFramework = 0x348;
constexpr size_t kFrameworkRuntimeScene = 0x18;

} // namespace

const char *volatile g_stage = "";
const void *volatile g_object = nullptr;

void *Resolve(uint32_t aHash) { return reinterpret_cast<void *>(RED4ext::UniversalRelocBase::Resolve(aHash)); }

void Log(const std::string &aText) {
  if (s_sdk && s_handle) {
    s_sdk->logger->Info(s_handle, ("[overlays] " + aText).c_str());
  }
}

SharedLock::SharedLock(void *aLock) {
  if (aLock && s_lockCtor && s_lockDtor) {
    s_lockCtor(&m_storage, aLock);
    m_held = true;
    t_held.push_back(&m_storage);
  }
}

SharedLock::~SharedLock() {
  if (m_held) {
    s_lockDtor(&m_storage);
    if (!t_held.empty()) {
      t_held.pop_back();
    }
  }
}

void *RuntimeScene() {
  auto engine = reinterpret_cast<char *>(RED4ext::CGameEngine::Get());
  if (!engine) {
    return nullptr;
  }
  auto framework = *reinterpret_cast<char **>(engine + kEngineFramework);
  if (!framework) {
    return nullptr;
  }
  return *reinterpret_cast<void **>(framework + kFrameworkRuntimeScene);
}

// world::RuntimeScene starts with one Handle<IRuntimeSystem> (16 bytes) per system slot; the engine's own getters read
// navigation at +0x20 (slot 2) and traffic at +0x170 (slot 23), the RED4ext SDK's order.
void *RuntimeSystem(uint32_t aSlot, const char *aTypeName) {
  auto scene = reinterpret_cast<char *>(RuntimeScene());
  if (!scene || aSlot >= 65) {
    return nullptr;
  }
  auto system = *reinterpret_cast<RED4ext::ISerializable **>(scene + 16 * aSlot);
  if (!system) {
    return nullptr;
  }
  auto type = system->GetType();
  if (!type || type->GetName() != RED4ext::CName(aTypeName)) {
    return nullptr;
  }
  return system;
}

} // namespace Detail

namespace {

using namespace Detail;
using Clock = std::chrono::steady_clock;

enum class Kind { Navmesh, Triggers, Collision };

struct Cache {
  V3 center{};
  float radius = 0.0f;
  Clock::time_point built{};
  bool valid = false;
  uint64_t build = 0; // numbers every rebuild, so callers can tell whether their copy is current
  Lines lines;
};
uint64_t s_lastBuild = 0;

std::mutex s_lock;
std::map<std::tuple<int, int32_t, int32_t>, Cache> s_caches;
std::map<Kind, std::string> s_stats;
std::map<Kind, bool> s_broken; // a build that faulted is not tried again this session

using BuildFn = std::string (*)(const V3 &, float, int32_t, int32_t, Lines &);

struct BuildCall {
  BuildFn fn;
  const V3 *center;
  float radius;
  int32_t a, b;
  Lines *out;
  std::string *summary;
};

void RunBuild(BuildCall *aCall) {
  *aCall->summary = aCall->fn(*aCall->center, aCall->radius, aCall->a, aCall->b, *aCall->out);
}

// Runs a build under SEH so a wrong offset after a game patch turns the overlay off instead of crashing the game.
volatile DWORD s_faultCode = 0;

bool GuardedBuild(BuildCall *aCall) {
  __try {
    RunBuild(aCall);
    return true;
  } __except (s_faultCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Collision runs on a thread with a large stack while the caller waits: a PhysX scene query needs more stack than a
// script call made from a game job has left, and an overflow there faulted inside PxScene::overlap. One thread for
// the session, woken per build (moving bodies are built every frame); callers are serialised by s_lock.
struct BigStackWorker {
  HANDLE thread = nullptr;
  HANDLE start = nullptr;
  HANDLE done = nullptr;
  BuildCall *call = nullptr;
  bool ok = false;
};
BigStackWorker s_worker;

DWORD WINAPI BigStackThread(LPVOID) {
  while (WaitForSingleObject(s_worker.start, INFINITE) == WAIT_OBJECT_0 && s_worker.call) {
    s_worker.ok = GuardedBuild(s_worker.call);
    SetEvent(s_worker.done);
  }
  return 0;
}

bool GuardedBuildBigStack(BuildCall *aCall) {
  if (!s_worker.thread) {
    s_worker.start = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s_worker.done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (s_worker.start && s_worker.done) {
      s_worker.thread =
          CreateThread(nullptr, 16 << 20, &BigStackThread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    }
    if (!s_worker.thread) {
      return GuardedBuild(aCall);
    }
  }
  s_worker.call = aCall;
  SetEvent(s_worker.start);
  WaitForSingleObject(s_worker.done, INFINITE);
  return s_worker.ok;
}

std::string NavmeshFn(const V3 &c, float r, int32_t a, int32_t b, Lines &o) { return BuildNavmesh(c, r, a, b != 0, o); }
std::string TriggersFn(const V3 &c, float r, int32_t a, int32_t, Lines &o) {
  return BuildTriggers(c, r, static_cast<uint32_t>(a), o);
}
std::string CollisionFn(const V3 &c, float r, int32_t a, int32_t, Lines &o) { return BuildCollision(c, r, a, o); }

// How long a result is reused while the centre stays put: the navmesh and triggers stream in and out with the
// world. Collision with only moving bodies is rebuilt every frame, so wireframes stay on the cars and NPCs (5 ms only
// shares one build between the frame's two BeginFrame calls); the static world keeps a second.
float RefreshSeconds(Kind aKind, int32_t aParam) {
  switch (aKind) {
  case Kind::Navmesh:
  case Kind::Triggers:
    return 1.0f;
  case Kind::Collision:
    return aParam == 1 ? 1.0f : aParam == 2 ? 0.005f : 0.1f; // static only, moving only, or both
  }
  return 1.0f;
}

const Lines &Get(Kind aKind, const V3 &aCenter, float aRadius, int32_t aA, int32_t aB) {
  static const Lines empty;
  if (!(aRadius > 0.0f) || !std::isfinite(aRadius) || !std::isfinite(aCenter.x) || !std::isfinite(aCenter.y) ||
      !std::isfinite(aCenter.z)) {
    return empty;
  }
  auto &cache = s_caches[{static_cast<int>(aKind), aA, aB}];
  const auto now = Clock::now();
  const float dx = aCenter.x - cache.center.x, dy = aCenter.y - cache.center.y, dz = aCenter.z - cache.center.z;
  const float moved = std::sqrt(dx * dx + dy * dy + dz * dz);
  const float age = std::chrono::duration<float>(now - cache.built).count();
  if (cache.valid && cache.radius == aRadius && moved < std::fmax(1.0f, aRadius * 0.05f) &&
      age < RefreshSeconds(aKind, aA)) {
    return cache.lines;
  }
  cache.center = aCenter;
  cache.radius = aRadius;
  cache.built = now;
  cache.valid = true;
  cache.build = ++s_lastBuild;
  cache.lines.clear();
  if (s_broken[aKind]) {
    return cache.lines;
  }
  BuildCall call{};
  call.fn = aKind == Kind::Navmesh ? &NavmeshFn : aKind == Kind::Triggers ? &TriggersFn : &CollisionFn;
  call.center = &aCenter;
  call.radius = aRadius;
  call.a = aA;
  call.b = aB;
  call.out = &cache.lines;
  std::string summary;
  call.summary = &summary;
  g_stage = "start";
  g_object = nullptr;
  const auto started = Clock::now();
  const bool ok = aKind == Kind::Collision ? GuardedBuildBigStack(&call) : GuardedBuild(&call);
  if (ok) {
    char took[48];
    snprintf(took, sizeof(took), ", built in %.2f ms",
             std::chrono::duration<double, std::milli>(Clock::now() - started).count());
    summary += took;
  }
  if (!ok) {
    // Let go of any engine lock the faulting build still held.
    while (!t_held.empty()) {
      s_lockDtor(t_held.back());
      t_held.pop_back();
    }
    s_broken[aKind] = true;
    cache.lines.clear();
    char where[160];
    const void *object = g_object;
    snprintf(where, sizeof(where), " (exception 0x%08lX at %s, object %p)", static_cast<unsigned long>(s_faultCode), g_stage, object);
    summary = std::string("faulted reading engine memory; this overlay is off until the game restarts") + where;
    Log(summary);
  }
  s_stats[aKind] = summary;
  return cache.lines;
}

V3 ToV3(const RED4ext::Vector4 &aV) { return {aV.X, aV.Y, aV.Z}; }

void Return(const Lines &aLines, RED4ext::DynArray<RED4ext::Vector4> *aOut) {
  if (!aOut) {
    return;
  }
  aOut->Clear();
  aOut->Reserve(static_cast<uint32_t>(aLines.size()));
  for (const auto &p : aLines) {
    aOut->PushBack({p.x, p.y, p.z, 1.0f});
  }
}

// native func DebugDraw_NavmeshEdges(center: Vector4, radius: Float, agent: Int32, boundary: Bool) -> array<Vector4>
void Native_NavmeshEdges(RED4ext::IScriptable *, RED4ext::CStackFrame *aFrame,
                         RED4ext::DynArray<RED4ext::Vector4> *aOut, int64_t) {
  RED4ext::Vector4 center{};
  float radius = 0.0f;
  int32_t agent = 0;
  bool boundary = false;
  RED4ext::GetParameter(aFrame, &center);
  RED4ext::GetParameter(aFrame, &radius);
  RED4ext::GetParameter(aFrame, &agent);
  RED4ext::GetParameter(aFrame, &boundary);
  aFrame->code++;
  std::lock_guard lock(s_lock);
  Return(Get(Kind::Navmesh, ToV3(center), radius, agent, boundary ? 1 : 0), aOut);
}

// native func DebugDraw_TriggerEdges(center: Vector4, radius: Float, channels: Int32) -> array<Vector4>
void Native_TriggerEdges(RED4ext::IScriptable *, RED4ext::CStackFrame *aFrame,
                         RED4ext::DynArray<RED4ext::Vector4> *aOut, int64_t) {
  RED4ext::Vector4 center{};
  float radius = 0.0f;
  int32_t channels = 0;
  RED4ext::GetParameter(aFrame, &center);
  RED4ext::GetParameter(aFrame, &radius);
  RED4ext::GetParameter(aFrame, &channels);
  aFrame->code++;
  std::lock_guard lock(s_lock);
  Return(Get(Kind::Triggers, ToV3(center), radius, channels, 0), aOut);
}

// native func DebugDraw_CollisionEdges(center: Vector4, radius: Float, actors: Int32) -> array<Vector4>
void Native_CollisionEdges(RED4ext::IScriptable *, RED4ext::CStackFrame *aFrame,
                           RED4ext::DynArray<RED4ext::Vector4> *aOut, int64_t) {
  RED4ext::Vector4 center{};
  float radius = 0.0f;
  int32_t actors = 0;
  RED4ext::GetParameter(aFrame, &center);
  RED4ext::GetParameter(aFrame, &radius);
  RED4ext::GetParameter(aFrame, &actors);
  aFrame->code++;
  std::lock_guard lock(s_lock);
  Return(Get(Kind::Collision, ToV3(center), radius, actors, 0), aOut);
}

// native func DebugDraw_OverlayStats() -> String
void Native_OverlayStats(RED4ext::IScriptable *, RED4ext::CStackFrame *aFrame, RED4ext::CString *aOut, int64_t) {
  aFrame->code++;
  std::lock_guard lock(s_lock);
  std::string text;
  const char *names[] = {"navmesh", "triggers", "collision"};
  for (int i = 0; i < 3; ++i) {
    auto it = s_stats.find(static_cast<Kind>(i));
    text += std::string(names[i]) + ": " + (it == s_stats.end() ? "not built" : it->second) + "\n";
  }
  if (aOut) {
    *aOut = RED4ext::CString(text.c_str());
  }
}

void RegisterArrayNative(RED4ext::CRTTISystem *aRtti, const char *aName, RED4ext::ScriptingFunction_t<void *> aFunc,
                         std::vector<std::pair<const char *, const char *>> aParams, const char *aReturn) {
  auto function = RED4ext::CGlobalFunction::Create(aName, aName, aFunc);
  for (const auto &[type, name] : aParams) {
    function->AddParam(type, name);
  }
  function->SetReturnType(aReturn);
  aRtti->RegisterFunction(function);
}

} // namespace

uint64_t AppendLines(Source aSource, const RED4ext::Vector4 &aCenter, float aRadius, int32_t aA, int32_t aB,
                     uint64_t aHave, std::vector<RED4ext::Vector4> &aOut) {
  const auto kind = aSource == Source::Navmesh ? Kind::Navmesh : aSource == Source::Triggers ? Kind::Triggers : Kind::Collision;
  std::lock_guard lock(s_lock);
  const auto &lines = Get(kind, ToV3(aCenter), aRadius, aA, aB);
  const auto build = s_caches[{static_cast<int>(kind), aA, aB}].build;
  if (build != aHave) {
    aOut.reserve(aOut.size() + lines.size());
    for (const auto &p : lines) {
      aOut.push_back({p.x, p.y, p.z, 1.0f});
    }
  }
  return build;
}

bool Load(const RED4ext::Sdk *aSdk, RED4ext::PluginHandle aHandle) {
  Detail::s_sdk = aSdk;
  Detail::s_handle = aHandle;
  Detail::s_lockCtor = reinterpret_cast<Detail::SharedLockCtor_t>(Resolve(Detail::kScopedSharedLock_ctor));
  Detail::s_lockDtor = reinterpret_cast<Detail::SharedLockDtor_t>(Resolve(Detail::kScopedSharedLock_dtor));
  if (!LoadNavmesh()) {
    s_broken[Kind::Navmesh] = true;
    Log("navmesh: addresses missing, overlay off");
  }
  if (!LoadTriggers()) {
    s_broken[Kind::Triggers] = true;
    Log("triggers: addresses missing, overlay off");
  }
  if (!LoadCollision()) {
    s_broken[Kind::Collision] = true;
    Log("collision: addresses missing, overlay off");
  }
  return Detail::s_lockCtor && Detail::s_lockDtor;
}

void Unload(const RED4ext::Sdk *, RED4ext::PluginHandle) {
  std::lock_guard lock(s_lock);
  s_caches.clear();
  if (s_worker.thread) {
    s_worker.call = nullptr; // the worker leaves its loop
    SetEvent(s_worker.start);
    WaitForSingleObject(s_worker.thread, 1000);
    CloseHandle(s_worker.thread);
    CloseHandle(s_worker.start);
    CloseHandle(s_worker.done);
    s_worker = {};
  }
}

void RegisterNatives() {
  auto rtti = RED4ext::CRTTISystem::Get();
  using F = RED4ext::ScriptingFunction_t<void *>;
  RegisterArrayNative(rtti, "DebugDraw_NavmeshEdges", reinterpret_cast<F>(&Native_NavmeshEdges),
                      {{"Vector4", "center"}, {"Float", "radius"}, {"Int32", "agent"}, {"Bool", "boundary"}},
                      "array:Vector4");
  RegisterArrayNative(rtti, "DebugDraw_TriggerEdges", reinterpret_cast<F>(&Native_TriggerEdges),
                      {{"Vector4", "center"}, {"Float", "radius"}, {"Int32", "channels"}}, "array:Vector4");
  RegisterArrayNative(rtti, "DebugDraw_CollisionEdges", reinterpret_cast<F>(&Native_CollisionEdges),
                      {{"Vector4", "center"}, {"Float", "radius"}, {"Int32", "actors"}}, "array:Vector4");
  RegisterArrayNative(rtti, "DebugDraw_OverlayStats", reinterpret_cast<F>(&Native_OverlayStats), {}, "String");
}

} // namespace Overlays
