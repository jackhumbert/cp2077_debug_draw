#pragma once

#include <RED4ext/Api/PluginHandle.hpp>
#include <RED4ext/Api/Sdk.hpp>

// Build with -DDEBUG_DRAW_DIAGNOSTICS=1 for the render-side probes below and the verbose pipeline log (stages,
// received shapes, per-600-frame stats). Off, the plugin logs its version, its first draw, and failures.
#ifndef DEBUG_DRAW_DIAGNOSTICS
#define DEBUG_DRAW_DIAGNOSTICS 0
#endif

// Pass-through hooks on the render side that log where queued debug geometry goes (see Diagnostics.cpp).
namespace Diagnostics {

constexpr bool kEnabled = DEBUG_DRAW_DIAGNOSTICS != 0;

void Load(const RED4ext::Sdk *aSdk, RED4ext::PluginHandle aHandle);
void Unload(const RED4ext::Sdk *aSdk, RED4ext::PluginHandle aHandle);

// The trampoline to CRenderNode_CollectDebug::Execute while its probe is hooked, otherwise nullptr.
void *CollectDebugTrampoline();

// Called by Collect around every CRenderNode_RenderDebugFragments::Execute: logs a sample of them and, during a
// sampled one, makes the RenderGeometry probe dump the buckets it draws. No-ops without diagnostics.
void BeforeRenderFragments(const void *aNode, const void *aCtx);
void AfterRenderFragments();

} // namespace Diagnostics
