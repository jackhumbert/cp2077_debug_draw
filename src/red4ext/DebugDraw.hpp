#pragma once

#include <RED4ext/Api/PluginHandle.hpp>
#include <RED4ext/Api/Sdk.hpp>

namespace Engine {
struct DrawerStorage;
}

namespace DebugDraw {

// Resolves engine addresses and hooks EngineViewport::BeginFrame. False if anything failed to resolve, in which
// case nothing is hooked and the natives just queue into the void.
bool Load(const RED4ext::Sdk *aSdk, RED4ext::PluginHandle aHandle);
void Unload(const RED4ext::Sdk *aSdk, RED4ext::PluginHandle aHandle);

// The drawer most recently swapped into the main camera's frame info (diagnostics compare against it).
void *LastDrawer();

// Assigns the current drawer (if any, and if the slot does not already hold it) into an engine
// SharedStorage<IDebugDrawer> slot, refcounted. Safe from render threads. True when it wrote. aSerial gets the
// current drawer's build number (0 for none): unlike its address, which the drawer pool reuses, it tells two
// drawers apart.
bool AssignCurrentDrawer(Engine::DrawerStorage *aSlot, uint64_t *aSerial);

// Registers the DebugDraw_* global natives (call from PostRegisterTypes).
void RegisterNatives();

} // namespace DebugDraw
