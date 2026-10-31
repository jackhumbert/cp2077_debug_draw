// RED4ext.hpp first: it defines RED4EXT_HEADER_ONLY, which the Api headers need to pull in their -inl bodies.
#include <RED4ext/RED4ext.hpp>

#include "Collect.hpp"
#include "DebugDraw.hpp"
#include "Diagnostics.hpp"
#include "GraphInsert.hpp"
#include "Overlays.hpp"
#include "Shaders.hpp"

#include <RED4ext/Api/EMainReason.hpp>
#include <RED4ext/Api/PluginHandle.hpp>
#include <RED4ext/Api/Runtime.hpp>
#include <RED4ext/Api/Sdk.hpp>
#include <RED4ext/Api/SemVer.hpp>
#include <RED4ext/Api/Version.hpp>
#include <RED4ext/RTTISystem.hpp>

#include <Windows.h>

#include <filesystem>

namespace {

// Input Loader is optional: its exported Add() is looked up at runtime instead of linked, so the plugin still loads
// (without the overlay toggle key) when it is not installed. It merges our inputs.xml into the game's mappings.
using InputLoaderAdd_t = void (*)(RED4ext::PluginHandle, const wchar_t *);

[[maybe_unused]] void RegisterInputs(const RED4ext::Sdk *aSdk, RED4ext::PluginHandle aHandle) {
  auto module = GetModuleHandleW(L"input_loader");
  if (!module) {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH); // <game>/bin/x64/Cyberpunk2077.exe
    auto folder = std::filesystem::path(exe).parent_path().parent_path().parent_path() / "red4ext" / "plugins" /
                  "input_loader";
    SetDllDirectoryW(folder.c_str());
    module = LoadLibraryW(L"input_loader");
    SetDllDirectoryW(nullptr);
  }
  auto add = module ? reinterpret_cast<InputLoaderAdd_t>(GetProcAddress(module, "Add")) : nullptr;
  if (!add) {
    aSdk->logger->Info(aHandle, "Input Loader not found: the overlay toggle key is not available");
    return;
  }
  add(aHandle, L"inputs.xml");
}

void PostRegisterTypes() {
  DebugDraw::RegisterNatives();
  Overlays::RegisterNatives();
}

} // namespace

RED4EXT_C_EXPORT bool RED4EXT_CALL Main(RED4ext::PluginHandle aHandle, RED4ext::EMainReason aReason,
                                        const RED4ext::Sdk *aSdk) {
  switch (aReason) {
  case RED4ext::EMainReason::Load: {
    aSdk->logger->Info(aHandle, "Debug Draw " MOD_VERSION_STR);
    RED4ext::CRTTISystem::Get()->AddPostRegisterCallback(&PostRegisterTypes);
    aSdk->scripts->Add(aHandle, L"packed.reds");
    // 2026-10-31: off. With inputs.xml merged (its button IK_None), the game crashed in
    // input::ContextManager::Action::NotifyListeners during load; the toggle key waits for a fix.
    // RegisterInputs(aSdk, aHandle);
    // Natives stay registered even if the hooks fail, so scripts calling DebugDraw still compile and run; they
    // just draw nothing on an unsupported game version. The render-side hooks only go in when the queue side works.
    // Diagnostics loads before Collect, which calls through its CollectDebug probe when that is hooked.
    Shaders::Load(aSdk, aHandle);
    Overlays::Load(aSdk, aHandle);
    if (DebugDraw::Load(aSdk, aHandle)) {
      Diagnostics::Load(aSdk, aHandle);
      if (GraphInsert::Load(aSdk, aHandle)) {
        Collect::Load(aSdk, aHandle);
      }
    }
    break;
  }
  case RED4ext::EMainReason::Unload: {
    Overlays::Unload(aSdk, aHandle);
    Collect::Unload(aSdk, aHandle);
    GraphInsert::Unload(aSdk, aHandle);
    Diagnostics::Unload(aSdk, aHandle);
    DebugDraw::Unload(aSdk, aHandle);
    Shaders::Unload(aSdk, aHandle);
    break;
  }
  }
  return true;
}

RED4EXT_C_EXPORT void RED4EXT_CALL Query(RED4ext::PluginInfo *aInfo) {
  aInfo->name = L"Debug Draw";
  aInfo->author = L"Jack Humbert";
  aInfo->version = RED4EXT_SEMVER(MOD_VERSION_MAJOR, MOD_VERSION_MINOR, MOD_VERSION_PATCH);
  aInfo->runtime = RED4EXT_RUNTIME_INDEPENDENT;
  aInfo->sdk = RED4EXT_SDK_LATEST;
}

RED4EXT_C_EXPORT uint32_t RED4EXT_CALL Supports() { return RED4EXT_API_VERSION_LATEST; }
