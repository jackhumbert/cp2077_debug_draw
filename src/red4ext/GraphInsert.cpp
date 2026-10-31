#include <RED4ext/RED4ext.hpp>

#include "GraphInsert.hpp"
#include "Diagnostics.hpp"
#include "Engine.hpp"

#include <atomic>
#include <cstdint>

// The debug nodes are not in the game's normal render graph. CRenderInterface::RenderFrame picks the graph builder
// per camera from the frame's mode (frameInfo+0x334): modes 1..4 get the debug-capable builder (sub_14227BD10),
// which creates CRenderNode_CollectDebug and CRenderNode_RenderDebugFragments (kind 0 before ApplyStaticUpscaling,
// kind 1 before RenderFinal2D). Normal gameplay takes sub_142276E60, which has no debug node anywhere.
//
// So while that main graph is built, this adds the two RenderDebugFragments nodes where the debug graph has them:
// - the main-scene DoCulling marks the build as the main one;
// - right after ApplyFXAA (PostFX group, render resolution, before upscaling): kind 0;
// - right before UpdateGlyphCacheTexture (end of PostFX, after upscaling/sharpening): kind 1.
// CollectDebug is not added: Collect.cpp collects on the drawing thread instead (see there for why).
// Graphs are cached per camera setup (CRenderGraphCache), so this runs once per rebuild, not per frame.
// Builders that already have the nodes (debug, hit proxies, no scene) are recognised by their own
// Create<CRenderNode_CollectDebug> call coming before DoCulling, and left alone (see CreateCollectDebug for the
// hit-proxies builder, where it comes after).

namespace GraphInsert {
namespace {

const RED4ext::Sdk *s_sdk = nullptr;
RED4ext::PluginHandle s_handle = nullptr;

using CreatePlain_t = uint32_t (*)(void *aFactory, uint8_t aGroup, int32_t aType, uint64_t aSubtype);
using CreateCulling_t = uint32_t (*)(void *aFactory, uint8_t aGroup, int32_t aType, uint64_t aSubtype,
                                     int32_t *aMainScene, int32_t *aCascades, int32_t *aDynamicShadows,
                                     int32_t *aRayTraced, int32_t *aQueryOnlyBoxes);
using CreateCollectDebug_t = uint32_t (*)(void *aFactory, uint8_t aGroup, int32_t aType, uint64_t aSubtype,
                                          bool *aSecondList);
using CreateRenderFragments_t = uint32_t (*)(void *aFactory, uint8_t aGroup, int32_t aType, uint64_t aSubtype,
                                             int32_t *aKind, uint8_t *aFeatureFlag);

CreateCulling_t s_cullingOriginal = nullptr;
CreatePlain_t s_fxaaOriginal = nullptr;
CreatePlain_t s_glyphOriginal = nullptr;
CreateCollectDebug_t s_collectDebugOriginal = nullptr;
CreateRenderFragments_t s_renderFragmentsOriginal = nullptr;

// Graph building runs on one render thread at a time, but keep these atomic to be safe.
std::atomic<void *> s_nativeFactory{nullptr};   // a builder that created CollectDebug itself
std::atomic<void *> s_insertedFactory{nullptr}; // the main builder we added CollectDebug to
std::atomic<uint32_t> s_graphsPatched{0};
thread_local bool t_ours = false;

// One value per message: aFormat takes the factory pointer and one %u. Diagnostics only.
void Log(const char *aFormat, void *aFactory, uint32_t aValue) {
  if (Diagnostics::kEnabled) {
    s_sdk->logger->InfoF(s_handle, aFormat, aFactory, aValue);
  }
}

uint32_t CreateCollectDebug(void *aFactory, uint8_t aGroup, int32_t aType, uint64_t aSubtype, bool *aSecondList) {
  if (!t_ours) {
    s_nativeFactory.store(aFactory);
    // BuildRenderGraphHitProxies creates its DoCulling BEFORE its own CollectDebug nodes, so we may already have
    // added ours there (harmless: two extra collect nodes). It has no ApplyFXAA, so forget it here or the next
    // main build at the same stack address would look already patched.
    void *expected = aFactory;
    s_insertedFactory.compare_exchange_strong(expected, nullptr);
  }
  return s_collectDebugOriginal(aFactory, aGroup, aType, aSubtype, aSecondList);
}

uint32_t CreateRenderFragments(void *aFactory, uint8_t aGroup, int32_t aType, uint64_t aSubtype, int32_t *aKind,
                               uint8_t *aFeatureFlag) {
  return s_renderFragmentsOriginal(aFactory, aGroup, aType, aSubtype, aKind, aFeatureFlag);
}

void AddRenderFragments(void *aFactory, uint64_t aSubtype, int32_t aKind) {
  int32_t kind = aKind;
  uint8_t featureFlag = 0; // FEATFLAG_Default, what the main graph hands RenderFinal2D
  t_ours = true;
  // Create<CRenderNode_RenderDebugFragments> ignores group and type: it registers {4, subtype, node} in group 0.
  s_renderFragmentsOriginal(aFactory, 0, 4, aSubtype, &kind, &featureFlag);
  t_ours = false;
}

uint32_t CreateCulling(void *aFactory, uint8_t aGroup, int32_t aType, uint64_t aSubtype, int32_t *aMainScene,
                       int32_t *aCascades, int32_t *aDynamicShadows, int32_t *aRayTraced, int32_t *aQueryOnlyBoxes) {
  bool mainScene = aMainScene && *aMainScene == 1;
  auto result = s_cullingOriginal(aFactory, aGroup, aType, aSubtype, aMainScene, aCascades, aDynamicShadows,
                                  aRayTraced, aQueryOnlyBoxes);
  if (!mainScene) {
    return result;
  }
  if (s_nativeFactory.exchange(nullptr) == aFactory) {
    return result; // a debug-capable builder: it has its own nodes
  }
  if (s_insertedFactory.load() == aFactory) {
    return result; // second main-scene culling in the same build
  }
  // Only marks the main build so the two RenderDebugFragments get added (no CollectDebug here: see Collect.cpp).
  s_insertedFactory.store(aFactory);
  Log("[graph] factory %p: main build recognised at main-scene DoCulling (group %u)", aFactory, aGroup);
  return result;
}

uint32_t CreateFxaa(void *aFactory, uint8_t aGroup, int32_t aType, uint64_t aSubtype) {
  auto result = s_fxaaOriginal(aFactory, aGroup, aType, aSubtype);
  if (s_insertedFactory.load() == aFactory) {
    AddRenderFragments(aFactory, aSubtype, 0);
    Log("[graph] factory %p: added RenderDebugFragments kind %u after ApplyFXAA", aFactory, 0);
  }
  return result;
}

uint32_t CreateGlyph(void *aFactory, uint8_t aGroup, int32_t aType, uint64_t aSubtype) {
  if (s_insertedFactory.load() == aFactory) {
    AddRenderFragments(aFactory, aSubtype, 1);
    s_insertedFactory.store(nullptr);
    auto patched = s_graphsPatched.fetch_add(1) + 1;
    if (patched == 1) {
      s_sdk->logger->Info(s_handle, "Added the debug render nodes to the main render graph");
    }
    Log("[graph] factory %p: added RenderDebugFragments kind 1 before UpdateGlyphCacheTexture; graphs patched %u",
        aFactory, patched);
  }
  return s_glyphOriginal(aFactory, aGroup, aType, aSubtype);
}

struct Hook {
  const char *name;
  uint32_t hash;
  void *detour;
  void **original;
  uintptr_t address = 0;
};

Hook s_hooks[] = {
    {"Create<CRenderNode_CollectDebug>", 851979914, reinterpret_cast<void *>(&CreateCollectDebug),
     reinterpret_cast<void **>(&s_collectDebugOriginal)},
    {"Create<CRenderNode_RenderDebugFragments>", 4053617245, reinterpret_cast<void *>(&CreateRenderFragments),
     reinterpret_cast<void **>(&s_renderFragmentsOriginal)},
    {"Create<CRenderNode_DoCulling>", 3208738178, reinterpret_cast<void *>(&CreateCulling),
     reinterpret_cast<void **>(&s_cullingOriginal)},
    {"Create<CRenderNode_ApplyFXAA>", 1266953000, reinterpret_cast<void *>(&CreateFxaa),
     reinterpret_cast<void **>(&s_fxaaOriginal)},
    {"Create<CRenderNode_UpdateGlyphCacheTexture>", 1015427374, reinterpret_cast<void *>(&CreateGlyph),
     reinterpret_cast<void **>(&s_glyphOriginal)},
};

} // namespace

bool Load(const RED4ext::Sdk *aSdk, RED4ext::PluginHandle aHandle) {
  s_sdk = aSdk;
  s_handle = aHandle;
  for (auto &hook : s_hooks) {
    hook.address = RED4ext::UniversalRelocBase::Resolve(hook.hash);
    if (!hook.address) {
      aSdk->logger->ErrorF(aHandle, "%s did not resolve; not patching the render graph, nothing will draw", hook.name);
      Unload(aSdk, aHandle);
      return false;
    }
  }
  for (auto &hook : s_hooks) {
    if (!aSdk->hooking->Attach(aHandle, reinterpret_cast<void *>(hook.address), hook.detour, hook.original)) {
      aSdk->logger->ErrorF(aHandle, "Could not hook %s; not patching the render graph, nothing will draw", hook.name);
      hook.address = 0;
      Unload(aSdk, aHandle);
      return false;
    }
    if (Diagnostics::kEnabled) {
      aSdk->logger->InfoF(aHandle, "[graph] hooked %s at 0x%llX", hook.name, hook.address);
    }
  }
  return true;
}

void Unload(const RED4ext::Sdk *aSdk, RED4ext::PluginHandle aHandle) {
  for (auto &hook : s_hooks) {
    if (hook.address && *hook.original) {
      aSdk->hooking->Detach(aHandle, reinterpret_cast<void *>(hook.address));
    }
    hook.address = 0;
  }
}

} // namespace GraphInsert
