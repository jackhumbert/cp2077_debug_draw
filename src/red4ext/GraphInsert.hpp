#pragma once

#include <RED4ext/Api/PluginHandle.hpp>
#include <RED4ext/Api/Sdk.hpp>

// Adds the debug collect/render nodes to the main gameplay render graph (see GraphInsert.cpp).
namespace GraphInsert {
bool Load(const RED4ext::Sdk *aSdk, RED4ext::PluginHandle aHandle);
void Unload(const RED4ext::Sdk *aSdk, RED4ext::PluginHandle aHandle);
} // namespace GraphInsert
