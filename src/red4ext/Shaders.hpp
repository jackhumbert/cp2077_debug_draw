#pragma once

#include <RED4ext/Api/PluginHandle.hpp>
#include <RED4ext/Api/Sdk.hpp>

#include <cstdint>

// Custom shaders for materials (see Shaders.cpp): give a new material name a compiled template's techniques with our
// own pixel shader, borrow a template's shaders unchanged, or replace a compiled shader by ID. Driven by the
// manifests (*.txt) under red4ext/plugins/debug_draw/shaders/; nothing is hooked when there are none.
namespace Shaders {
void Load(const RED4ext::Sdk *aSdk, RED4ext::PluginHandle aHandle);
// Reads the manifests again (DebugDraw.ReloadShaders); returns the number of materials in them. The caller then
// invalidates the materials that use them so their techniques are created again.
int32_t Reload();
void Unload(const RED4ext::Sdk *aSdk, RED4ext::PluginHandle aHandle);
} // namespace Shaders
