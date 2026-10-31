#include <RED4ext/RED4ext.hpp>

#include "Collect.hpp"
#include "DebugDraw.hpp"
#include "Diagnostics.hpp"
#include "Engine.hpp"

#include <atomic>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <vector>

// The collect that makes shapes draw. CRenderNode_CollectDebug, where the debug graph collects, runs in the culling
// group as parallel jobs that are not ordered against the collector's reset or the draw, so its buckets were empty at
// draw time most frames. Instead, this hooks CRenderNode_RenderDebugFragments::Execute and, on the drawing thread
// right before kind 0 draws, puts our drawer into the camera data slot and runs CollectDebug::Execute with this
// node's context.
//
// CollectDebug::Execute reads only ctx+0x18 (camera data: camera +0xE0, collector +0x2C40, size +0x54/+0x58),
// ctx+0x30 (bit 1: render flow phase 2, the only phase that collects or draws) and its node's +0x18 bool (which
// fragment list), and ignores the job builder, so a zeroed 0x20-byte stand-in node does.

namespace Collect {
namespace {

const RED4ext::Sdk *s_sdk = nullptr;
RED4ext::PluginHandle s_handle = nullptr;

template <typename T> T At(const void *aBase, size_t aOffset) {
  return *reinterpret_cast<const T *>(reinterpret_cast<const uint8_t *>(aBase) + aOffset);
}

using NodeExecute_t = void (*)(const void *aNode, const void *aCtx, void *aJobs);

// CRenderNode_RenderDebugFragments::Execute(const SRenderNodeImplContext&, job::Builder*) const
constexpr uint32_t kRenderFragmentsExecute = 2431525523;
// CRenderNode_CollectDebug::Execute(const SRenderNodeImplContext&, job::Builder*) const
constexpr uint32_t kCollectDebugExecute = 1862147922;
// Camera data: the SharedStorage<IDebugDrawer> CollectDebug::Execute reads.
constexpr size_t kCameraDataDrawerOffset = 0x24D0;

NodeExecute_t s_renderFragmentsOriginal = nullptr;
NodeExecute_t s_collectDebug = nullptr;
uintptr_t s_address = 0;
std::atomic<uint64_t> s_collects{0};

// World text has to be put back before a drawer is collected again. DebugDrawerImpl::CollectImpl projects a
// world-anchored fragment (node +0x08 anchor, not FLT_MAX at anchor +0x24: those are screen space) with
// rend::helper::WorldToScreen and adds the screen position to the fragment's own vertices, in place. Collecting the
// same drawer twice, which happens whenever the renderer draws a frame before BeginFrame has built a newer drawer,
// adds it twice and puts the text off screen: world labels vanished in one or two frames in ten while lines and
// screen text stayed. So the first collect of each drawer saves those vertices, and every later one restores them.
// RenderGeometry: positions at +0x08 (24-byte vertices, x, y at +0/+4, count +0x10) and +0x30 (28-byte, x, y at
// +4/+8, count +0x3C).
struct SavedPoint {
  float *xy;
  float x, y;
};
std::mutex s_textLock;
uint64_t s_textSerial = 0;
std::vector<SavedPoint> s_textPoints;
std::atomic<uint64_t> s_textRestores{0};

void SaveText(const void *aDrawer) {
  s_textPoints.clear();
  for (auto node = At<const uint8_t *>(aDrawer, 0x118); node; node = At<const uint8_t *>(node, 0x20)) {
    auto anchor = At<const float *>(node, 0x08);
    auto geometry = At<const uint8_t *>(node, 0x18);
    if (At<const void *>(node, 0x00) || !anchor || !geometry || anchor[9] == FLT_MAX) {
      continue;
    }
    struct Array {
      size_t pointer, count, stride, xy;
    };
    for (auto array : {Array{0x08, 0x10, 24, 0}, Array{0x30, 0x3C, 28, 4}}) {
      auto vertices = At<uint8_t *>(geometry, array.pointer);
      auto count = At<uint32_t>(geometry, array.count);
      for (uint32_t i = 0; vertices && i < count; ++i) {
        auto xy = reinterpret_cast<float *>(vertices + i * array.stride + array.xy);
        s_textPoints.push_back({xy, xy[0], xy[1]});
      }
    }
  }
}

// Before the collect of drawer aSerial: save its world text the first time, restore it every time after.
void PrepareText(const void *aDrawer, uint64_t aSerial) {
  std::lock_guard lock(s_textLock);
  if (aSerial != s_textSerial) {
    s_textSerial = aSerial;
    SaveText(aDrawer);
    return;
  }
  for (const auto &point : s_textPoints) {
    point.xy[0] = point.x;
    point.xy[1] = point.y;
  }
  auto restores = s_textRestores.fetch_add(1);
  if (Diagnostics::kEnabled && restores % 600 == 0) {
    s_sdk->logger->InfoF(s_handle, "[diag] drawer %llu collected again, %zu world text points restored (%llu so far)",
                         aSerial, s_textPoints.size(), restores + 1);
  }
}

// Diagnostics: which drawer is collected and what is in it (fragments, and the vertex counts of the first ones).
void LogCollect(uint64_t aIndex, void *aBefore, void *aAfter, bool aWrote) {
  uint32_t fragments = 0, vertices = 0;
  char counts[128] = {};
  size_t used = 0;
  for (auto node = aAfter ? At<const void *>(aAfter, 0x118) : nullptr; node && fragments < 100000;
       node = At<const void *>(node, 0x20)) {
    auto geometry = At<const void *>(node, 0x18);
    uint32_t count = geometry ? At<uint32_t>(geometry, 0x10) : 0;
    vertices += count;
    if (fragments < 12 && used + 12 < sizeof(counts)) {
      used += snprintf(counts + used, sizeof(counts) - used, "%u ", count);
    }
    ++fragments;
  }
  s_sdk->logger->InfoF(s_handle, "[diag] collect #%llu: slot drawer %p -> %p (%s), current %p; %u fragments, %u "
                                 "vertices [%s]",
                       aIndex, aBefore, aAfter, aWrote ? "wrote" : "kept", DebugDraw::LastDrawer(), fragments,
                       vertices, counts);
}

void RenderFragments_Execute(const void *aNode, const void *aCtx, void *aJobs) {
  Diagnostics::BeforeRenderFragments(aNode, aCtx);
  auto flags = At<uint8_t>(aCtx, 0x30);
  auto cameraData = At<const void *>(aCtx, 0x18);
  if ((flags & 2) && At<uint32_t>(aNode, 0x18) == 0 && cameraData && DebugDraw::LastDrawer()) {
    auto slot = reinterpret_cast<Engine::DrawerStorage *>(reinterpret_cast<uint8_t *>(const_cast<void *>(cameraData)) +
                                                          kCameraDataDrawerOffset);
    void *before = slot->drawer;
    uint64_t serial = 0;
    bool wrote = DebugDraw::AssignCurrentDrawer(slot, &serial);
    if (serial && slot->drawer) {
      PrepareText(slot->drawer, serial);
    }
    auto index = s_collects.fetch_add(1);
    if (Diagnostics::kEnabled && index % 600 == 0) {
      LogCollect(index, before, slot->drawer, wrote);
    }
    alignas(16) uint8_t node[0x20] = {}; // +0x18 = 0: the first fragment list (+0x118), where Flush puts ours
    s_collectDebug(node, aCtx, aJobs);
    if (index == 0) {
      s_sdk->logger->Info(s_handle, "Drawing: first debug shapes collected");
    }
  }
  s_renderFragmentsOriginal(aNode, aCtx, aJobs);
  Diagnostics::AfterRenderFragments();
}

} // namespace

bool Load(const RED4ext::Sdk *aSdk, RED4ext::PluginHandle aHandle) {
  s_sdk = aSdk;
  s_handle = aHandle;
  // Through the CollectDebug probe's trampoline when that probe is hooked, so it still sees these calls.
  auto trampoline = Diagnostics::CollectDebugTrampoline();
  s_collectDebug = trampoline ? reinterpret_cast<NodeExecute_t>(trampoline)
                              : Engine::Resolve<NodeExecute_t>(kCollectDebugExecute);
  s_address = RED4ext::UniversalRelocBase::Resolve(kRenderFragmentsExecute);
  if (!s_collectDebug || !s_address) {
    aSdk->logger->Error(aHandle, "The debug render nodes did not resolve; nothing will draw");
    s_address = 0;
    return false;
  }
  if (!aSdk->hooking->Attach(aHandle, reinterpret_cast<void *>(s_address),
                             reinterpret_cast<void *>(&RenderFragments_Execute),
                             reinterpret_cast<void **>(&s_renderFragmentsOriginal))) {
    aSdk->logger->Error(aHandle, "Could not hook CRenderNode_RenderDebugFragments::Execute; nothing will draw");
    s_address = 0;
    return false;
  }
  return true;
}

void Unload(const RED4ext::Sdk *aSdk, RED4ext::PluginHandle aHandle) {
  if (s_address) {
    aSdk->hooking->Detach(aHandle, reinterpret_cast<void *>(s_address));
    s_address = 0;
  }
}

} // namespace Collect
