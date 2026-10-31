#include <RED4ext/RED4ext.hpp>

#include "Diagnostics.hpp"
#include "DebugDraw.hpp"
#include "Engine.hpp"

#include <atomic>
#include <cstdint>

// Pass-through probes along the render side's debug path, installed only with DEBUG_DRAW_DIAGNOSTICS: does
// CollectDebug see our drawer and pass its phase gate, does DebugDrawerImpl::Collect walk our fragments, does each
// fragment reach CollectDebugImpl, does RenderDebugFragments run for a normal frame, and is anything in the buckets
// it draws. Each logs its first call involving our drawer and then every 600th call. They run on render job
// threads, so counters are atomic.

namespace Diagnostics {
namespace {

const RED4ext::Sdk *s_sdk = nullptr;
RED4ext::PluginHandle s_handle = nullptr;

template <typename T> T At(const void *aBase, size_t aOffset) {
  return *reinterpret_cast<const T *>(reinterpret_cast<const uint8_t *>(aBase) + aOffset);
}

bool Every(std::atomic<uint64_t> &aCounter, uint64_t &aIndex) {
  aIndex = aCounter.fetch_add(1);
  return aIndex % 600 == 0;
}

// --- CRenderNode_CollectDebug::Execute(const SRenderNodeImplContext&, job::Builder*) const
// Gate: ctx+0x30 bit 1 (render flow phase 2); drawer: *(ctx+0x18) + 0x24D0; skips when the drawer's vfunc +8
// returns true (the null drawer).
using NodeExecute_t = void (*)(const void *aNode, const void *aCtx, void *aJobs);
NodeExecute_t s_collectDebugOriginal = nullptr;
std::atomic<uint64_t> s_collectDebugCalls{0};
std::atomic<uint64_t> s_collectDebugOurs{0};
std::atomic<bool> s_collectDebugSawOurs{false};
std::atomic<uint64_t> s_collectDebugPhase2{0};

void CollectDebug_Execute(const void *aNode, const void *aCtx, void *aJobs) {
  uint64_t index;
  bool sample = Every(s_collectDebugCalls, index);
  auto flags = At<uint8_t>(aCtx, 0x30);
  auto cameraData = At<const void *>(aCtx, 0x18);
  const void *drawer = cameraData ? At<const void *>(cameraData, 0x24D0) : nullptr;
  bool isNull = true;
  if (drawer) {
    using IsNull_t = bool (*)(const void *);
    isNull = (*reinterpret_cast<IsNull_t *const *>(drawer))[1](drawer);
  }
  bool ours = drawer && drawer == DebugDraw::LastDrawer();
  if (flags & 2) {
    s_collectDebugPhase2.fetch_add(1);
  }
  if (ours) {
    s_collectDebugOurs.fetch_add(1);
  }
  bool first = ours && !s_collectDebugSawOurs.exchange(true);
  if (sample || first) {
    s_sdk->logger->InfoF(s_handle,
                         "[diag] CollectDebug call %llu: ctx+0x30=0x%02X (phase-2 bit1 %s), cameraData %p, drawer %p "
                         "(ours %s, isNull %s, last ours %p), calls with ours so far %llu, phase-2 calls %llu",
                         index, flags, (flags & 2) ? "on" : "OFF", cameraData, drawer, ours ? "yes" : "no",
                         isNull ? "yes" : "no", DebugDraw::LastDrawer(), s_collectDebugOurs.load(),
                         s_collectDebugPhase2.load());
  }
  s_collectDebugOriginal(aNode, aCtx, aJobs);
}

// --- rend::DebugDrawerImpl::Collect(EFeatureFlag, RenderStageCollector&, const CRenderCamera*, uint32, uint32,
//     Vector2, Vector2, bool) const. The bool picks the fragment list (+0x128 when true, +0x118 when false).
using DrawerCollect_t = void (*)(const void *aDrawer, int32_t aFlag, void *aCollector, const void *aCamera,
                                 uint32_t aWidth, uint32_t aHeight, uint64_t aScale, uint64_t aOffset, bool aSecond);
DrawerCollect_t s_drawerCollectOriginal = nullptr;
std::atomic<uint64_t> s_drawerCollectCalls{0};
std::atomic<bool> s_drawerCollectSawOurs{false};

void DrawerCollect(const void *aDrawer, int32_t aFlag, void *aCollector, const void *aCamera, uint32_t aWidth,
                   uint32_t aHeight, uint64_t aScale, uint64_t aOffset, bool aSecond) {
  uint64_t index;
  bool sample = Every(s_drawerCollectCalls, index);
  bool ours = aDrawer == DebugDraw::LastDrawer();
  bool first = ours && !s_drawerCollectSawOurs.exchange(true);
  if (sample || first) {
    uint32_t fragments = 0;
    for (auto node = At<const void *>(aDrawer, aSecond ? 0x128 : 0x118); node && fragments < 100000;
         node = At<const void *>(node, 0x20)) {
      ++fragments;
    }
    s_sdk->logger->InfoF(s_handle,
                         "[diag] DebugDrawerImpl::Collect call %llu: drawer %p (ours %s), featureFlag %d, camera %p, "
                         "%ux%u, second list %s, %u fragments in that list",
                         index, aDrawer, ours ? "yes" : "no", aFlag, aCamera, aWidth, aHeight, aSecond ? "yes" : "no",
                         fragments);
  }
  s_drawerCollectOriginal(aDrawer, aFlag, aCollector, aCamera, aWidth, aHeight, aScale, aOffset, aSecond);
}

// --- CRenderDrawer::CollectDebug(EFeatureFlag, RenderStageCollector&, const RenderSettings&, const RenderGeometry&,
//     const CRenderCamera*): one call per world-space fragment. RenderSettings byte +0x10 comes from the builder's
//     byte +8 (0xFF by default), byte +0x11 bit0 overlay, bit1 transparency, bit2 camera-relative (translates by
//     -camera position); dword +0x6C instance count. RenderGeometry dword +0x10 is its vertex count.
using CollectDebug_t = void (*)(void *aDrawer, int32_t aFlag, void *aCollector, const void *aSettings,
                                const void *aGeometry, const void *aCamera);
CollectDebug_t s_collectFragmentOriginal = nullptr;
std::atomic<uint64_t> s_collectFragmentCalls{0};

void CollectFragment(void *aDrawer, int32_t aFlag, void *aCollector, const void *aSettings, const void *aGeometry,
                     const void *aCamera) {
  uint64_t index;
  if (Every(s_collectFragmentCalls, index)) {
    s_sdk->logger->InfoF(s_handle,
                         "[diag] CRenderDrawer::CollectDebug call %llu: featureFlag %d, settings +0x10=0x%02X "
                         "+0x11=0x%02X material %p instances %u, geometry %p vertices %u, camera %p",
                         index, aFlag, At<uint8_t>(aSettings, 0x10), At<uint8_t>(aSettings, 0x11),
                         At<const void *>(aSettings, 0x0), At<uint32_t>(aSettings, 0x6C), aGeometry,
                         aGeometry ? At<uint32_t>(aGeometry, 0x10) : 0, aCamera);
  }
  s_collectFragmentOriginal(aDrawer, aFlag, aCollector, aSettings, aGeometry, aCamera);
}

// --- CollectDebugImpl(EFeatureFlag, RenderStageCollector&, const SDebugCollectSettings&, const RenderGeometry&):
//     where a fragment becomes render stage items (returns early for hit proxies).
using CollectDebugImpl_t = void (*)(int32_t aFlag, void *aCollector, const void *aSettings, const void *aGeometry);
CollectDebugImpl_t s_collectImplOriginal = nullptr;
std::atomic<uint64_t> s_collectImplCalls{0};

void CollectImpl(int32_t aFlag, void *aCollector, const void *aSettings, const void *aGeometry) {
  uint64_t index;
  if (Every(s_collectImplCalls, index)) {
    // Geometry byte 0 picks the default material when the fragment has none (0 and 2: one getter, 1, 4, 5/3, 6
    // others, anything else: none, and CollectDebugImpl returns without collecting).
    s_sdk->logger->InfoF(s_handle,
                         "[diag] CollectDebugImpl call %llu: featureFlag %d, collector %p, geometry %p type %u, "
                         "settings material %p",
                         index, aFlag, aCollector, aGeometry, At<uint8_t>(aGeometry, 0), At<const void *>(aSettings, 0));
  }
  s_collectImplOriginal(aFlag, aCollector, aSettings, aGeometry);
}

// --- CRenderNode_RenderDebugFragments::Execute (hooked by Collect, which calls Before/AfterRenderFragments):
//     returns at once unless FramePurpose::IsNormal(frameInfo+0xFA4) (value < 2); node +0x18 is its DebugFragments
//     kind; ctx+0x30 bit 1 gates the bucket drawing. Phase-2 calls of kind 0 are the ones that draw: they set the
//     camera and call CRenderGeometryBatcher::RenderGeometry on buckets 4, 5, 0, 1 of the "renderstage_debugdraw"
//     stage of cameraData+0x2C40's collector (+0x40). During a sampled one, t_sampleDraw makes the RenderGeometry
//     probe dump each bucket it is handed.
using GetFrameInfo_t = const void *(*)(const void *aCtx);
GetFrameInfo_t s_getFrameInfo = nullptr;
std::atomic<uint64_t> s_renderFragmentsCalls{0};
std::atomic<uint64_t> s_renderFragmentsPhase2{0};
thread_local bool t_sampleDraw = false;
thread_local uint32_t t_bucketsLogged = 0;

// --- CRenderGeometryBatcher::RenderGeometry(const RenderContext&, const RenderStageCollectorBucket&, uint32, uint32).
//     Buckets are 0x1C0 bytes with an unknown layout, so dump the first 0x40 bytes: all zero means nothing was
//     collected into it.
using RenderGeometry_t = void (*)(void *aBatcher, const void *aContext, const void *aBucket, uint32_t aA, uint32_t aB);
RenderGeometry_t s_renderGeometryOriginal = nullptr;

void RenderGeometry(void *aBatcher, const void *aContext, const void *aBucket, uint32_t aA, uint32_t aB) {
  if (t_sampleDraw && t_bucketsLogged < 8) {
    ++t_bucketsLogged;
    auto q = reinterpret_cast<const uint64_t *>(aBucket);
    s_sdk->logger->InfoF(s_handle,
                         "[diag]   RenderGeometry #%u bucket %p: %016llX %016llX %016llX %016llX %016llX %016llX "
                         "%016llX %016llX",
                         t_bucketsLogged, aBucket, q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7]);
  }
  s_renderGeometryOriginal(aBatcher, aContext, aBucket, aA, aB);
}

struct Hook {
  const char *name;
  uint32_t hash;
  void *detour;
  void **original;
  uintptr_t address = 0;
};

Hook s_hooks[] = {
    {"CRenderNode_CollectDebug::Execute", 1862147922, reinterpret_cast<void *>(&CollectDebug_Execute),
     reinterpret_cast<void **>(&s_collectDebugOriginal)},
    {"DebugDrawerImpl::Collect", 845293647, reinterpret_cast<void *>(&DrawerCollect),
     reinterpret_cast<void **>(&s_drawerCollectOriginal)},
    {"CRenderDrawer::CollectDebug", 1892759622, reinterpret_cast<void *>(&CollectFragment),
     reinterpret_cast<void **>(&s_collectFragmentOriginal)},
    {"CollectDebugImpl", 885010350, reinterpret_cast<void *>(&CollectImpl),
     reinterpret_cast<void **>(&s_collectImplOriginal)},
    {"CRenderGeometryBatcher::RenderGeometry", 1914643706, reinterpret_cast<void *>(&RenderGeometry),
     reinterpret_cast<void **>(&s_renderGeometryOriginal)},
};

} // namespace

void Load(const RED4ext::Sdk *aSdk, RED4ext::PluginHandle aHandle) {
  if (!kEnabled) {
    return;
  }
  s_sdk = aSdk;
  s_handle = aHandle;
  // SRenderNodeImplContext::GetFrameInfo() const
  s_getFrameInfo = Engine::Resolve<GetFrameInfo_t>(643897290);
  if (!s_getFrameInfo) {
    aSdk->logger->Error(aHandle, "[diag] GetFrameInfo did not resolve; diagnostics off");
    return;
  }
  for (auto &hook : s_hooks) {
    hook.address = RED4ext::UniversalRelocBase::Resolve(hook.hash);
    if (!hook.address ||
        !aSdk->hooking->Attach(aHandle, reinterpret_cast<void *>(hook.address), hook.detour, hook.original)) {
      aSdk->logger->InfoF(aHandle, "[diag] could not hook %s", hook.name);
      hook.address = 0;
      continue;
    }
    aSdk->logger->InfoF(aHandle, "[diag] hooked %s at 0x%llX", hook.name, hook.address);
  }
}

void Unload(const RED4ext::Sdk *aSdk, RED4ext::PluginHandle aHandle) {
  for (auto &hook : s_hooks) {
    if (hook.address) {
      aSdk->hooking->Detach(aHandle, reinterpret_cast<void *>(hook.address));
      hook.address = 0;
    }
  }
}

void *CollectDebugTrampoline() { return reinterpret_cast<void *>(s_collectDebugOriginal); }

void BeforeRenderFragments(const void *aNode, const void *aCtx) {
  if (!kEnabled || !s_getFrameInfo) {
    return;
  }
  uint64_t index;
  auto flags = At<uint8_t>(aCtx, 0x30);
  bool sample = Every(s_renderFragmentsCalls, index);
  bool samplePhase2 = false;
  if (flags & 2) {
    uint64_t phase2Index;
    samplePhase2 = Every(s_renderFragmentsPhase2, phase2Index) || phase2Index < 3;
  }
  if (sample || samplePhase2) {
    auto frameInfo = s_getFrameInfo(aCtx);
    auto purpose = frameInfo ? At<uint32_t>(frameInfo, 0xFA4) : 0xFFFFFFFF;
    auto cameraData = At<const void *>(aCtx, 0x18);
    auto collector = cameraData ? At<const void *>(cameraData, 0x2C40) : nullptr;
    s_sdk->logger->InfoF(s_handle,
                         "[diag] RenderDebugFragments call %llu (phase-2 calls %llu): kind %u, framePurpose %u "
                         "(normal %s), ctx+0x30=0x%02X (phase-2 bit1 %s), cameraData %p, collector %p",
                         index, s_renderFragmentsPhase2.load(), At<uint32_t>(aNode, 0x18), purpose,
                         purpose < 2 ? "yes" : "NO", flags, (flags & 2) ? "on" : "OFF", cameraData, collector);
  }
  t_sampleDraw = samplePhase2;
  t_bucketsLogged = 0;
}

void AfterRenderFragments() {
  if (kEnabled) {
    t_sampleDraw = false;
  }
}

} // namespace Diagnostics
