#pragma once

#include <RED4ext/Api/PluginHandle.hpp>
#include <RED4ext/Api/Sdk.hpp>

// Collects our drawer into the camera's debug collector right before RenderDebugFragments draws it (see Collect.cpp).
namespace Collect {
bool Load(const RED4ext::Sdk *aSdk, RED4ext::PluginHandle aHandle);
void Unload(const RED4ext::Sdk *aSdk, RED4ext::PluginHandle aHandle);
} // namespace Collect
