#include <RED4ext/RED4ext.hpp>

#include "Shaders.hpp"

#include "ShaderManifest.hpp"

#include <Windows.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Material shaders all come from engine\shader_final.cache, read through CShaderCacheReadonly (one reader, no mod
// or DLC merge). CRenderMaterial::CreateTechnique looks a technique up with GetMaterial(key), where
//   key = fold(FNV1a64(template name)) << 32 | FNV1a32(vertex factory flags, technique index, pass name, pass index)
// and fold(h) = lo32(h) ^ hi32(h). The depot path is not in the key, so a new .mt only gets shaders when its name
// matches a compiled template. The technique's MaterialEntry then names a VS and a PS ID, each fetched with
// GetShader(id), which hands out a pointer to a 0x38-byte ShaderEntry: +0 id, +8 reflection id, +0x10 DataBuffer
// (bytecode pointer +0x10, size +0x18). The bytecode is a signed DXBC container with DXIL (SM 6.0).
//
// Manifest lines (any *.txt under red4ext/plugins/debug_draw/shaders/; files relative to the manifest):
//   shader <id hex> <file>   GetShader(id) returns that file's bytecode instead (a copy of the entry, so the cache
//                            itself is untouched)
//   alias <name> <donor>     a template called <name> gets <donor>'s compiled shaders (GetMaterial looks up the
//                            key with the donor's name half)
//   material <name> <donor> <ps file>
//                            the same, with that pixel shader in every technique: GetMaterial rewrites the PS ID of
//                            the MaterialEntry copy it hands out (+0x10; +0x08 is the VS) to a synthetic ID, and
//                            GetShader answers that ID with the donor PS's entry pointing at the file's bytecode
//   vertex <name> <donor VS id hex> <file>
//                            after a material line: where the donor's technique uses that VS (one per vertex
//                            factory: DrawBuffer 46AB074CDBB5F2E3, Debug 687287DC8EC613C8 for debugdraw), use this
//                            one instead, the same way (MaterialEntry +0x08)
//
// Reload (DebugDraw.ReloadShaders): the manifests are read again into a new generation, and DebugDraw invalidates its
// materials' render resources, so the engine creates their techniques again and asks GetMaterial / GetShader anew.
// The engine keeps pointing at the bytecode it was handed (its shader store keeps {pointer, size}; PSOs compile from
// it later), so no generation is ever freed, and synthetic IDs carry the generation so a reload is never answered
// with an entry made for an older one.

namespace Shaders {
namespace {

const RED4ext::Sdk *s_sdk = nullptr;
RED4ext::PluginHandle s_handle = nullptr;

// EResult IShaderCache::GetShader(uint64 id, const ShaderEntry*& out) const: 1 found, 0 missing, -1 not loaded
using GetShader_t = int32_t (*)(const void *aCache, uint64_t aId, const void **aEntry);
// EResult IShaderCache::GetMaterial(uint64 key, SharedStorage<MaterialEntry>& out) const
using GetMaterial_t = int32_t (*)(const void *aCache, uint64_t aKey, void *aEntry);
// ?GetShader@CShaderCacheReadonly@@UEBA?AW4EResult@IShaderCache@@_KAEAPEBUShaderEntry@@@Z
constexpr uint32_t kGetShader = 1196170851;
// ?GetMaterial@CShaderCacheReadonly@@UEBA?AW4EResult@IShaderCache@@_KAEAV?$SharedStorage@UMaterialEntry@@...@Z
constexpr uint32_t kGetMaterial = 1118057771;

constexpr size_t kShaderEntrySize = 0x38;
constexpr size_t kBytecodePointer = 0x10;
constexpr size_t kBytecodeSize = 0x18;

constexpr size_t kMaterialEntryVertexShader = 0x08;
constexpr size_t kMaterialEntryPixelShader = 0x10;

// A shader ID made up for one (material, donor shader) pair.
struct Synthetic {
  uint64_t donorShader;
  const std::vector<uint8_t> *bytecode;
  std::vector<uint8_t> entry;
};

GetShader_t s_getShaderOriginal = nullptr;
GetMaterial_t s_getMaterialOriginal = nullptr;
uintptr_t s_getShaderAddress = 0;
uintptr_t s_getMaterialAddress = 0;
std::mutex s_lock; // render threads create techniques in parallel

std::vector<std::unique_ptr<Generation>> s_generations; // the last is current
Generation *s_current = nullptr;
uint64_t s_generationSalt = 0; // folded into synthetic IDs
std::unordered_map<uint64_t, Synthetic> s_synthetic;
std::set<uint32_t> s_reportedAliases;
std::set<std::pair<uint32_t, uint64_t>> s_reportedVertexMisses;

// A copy of a cache ShaderEntry with another id and bytecode.
std::vector<uint8_t> EntryFor(const void *aEntry, uint64_t aId, const std::vector<uint8_t> &aBytecode) {
  std::vector<uint8_t> entry(static_cast<const uint8_t *>(aEntry), static_cast<const uint8_t *>(aEntry) + kShaderEntrySize);
  auto data = aBytecode.data();
  auto size = static_cast<uint32_t>(aBytecode.size());
  std::memcpy(entry.data(), &aId, sizeof(aId));
  std::memcpy(entry.data() + kBytecodePointer, &data, sizeof(data));
  std::memcpy(entry.data() + kBytecodeSize, &size, sizeof(size));
  return entry;
}

int32_t GetShader(const void *aCache, uint64_t aId, const void **aEntry) {
  uint64_t lookup = aId;
  {
    std::lock_guard lock(s_lock);
    auto synthetic = s_synthetic.find(aId);
    if (synthetic != s_synthetic.end()) {
      lookup = synthetic->second.donorShader;
    }
  }
  auto result = s_getShaderOriginal(aCache, lookup, aEntry);
  if (result != 1 || !aEntry || !*aEntry) {
    return result;
  }
  std::lock_guard lock(s_lock);
  if (lookup != aId) {
    auto &synthetic = s_synthetic[aId];
    if (synthetic.entry.empty()) {
      synthetic.entry = EntryFor(*aEntry, aId, *synthetic.bytecode);
      s_sdk->logger->InfoF(s_handle, "Shader %016llX: our shader (%u bytes) in place of %016llX", aId,
                           static_cast<uint32_t>(synthetic.bytecode->size()), lookup);
    }
    *aEntry = synthetic.entry.data();
    return result;
  }
  auto found = s_current->overrides.find(aId);
  if (found == s_current->overrides.end()) {
    return result;
  }
  auto &override = found->second;
  if (override.entry.empty()) {
    override.entry = EntryFor(*aEntry, aId, override.bytecode);
  }
  *aEntry = override.entry.data();
  if (!override.reported) {
    override.reported = true;
    s_sdk->logger->InfoF(s_handle, "Shader %016llX: serving %u bytes from a manifest", aId,
                         static_cast<uint32_t>(override.bytecode.size()));
  }
  return result;
}

int32_t GetMaterial(const void *aCache, uint64_t aKey, void *aEntry) {
  auto name = static_cast<uint32_t>(aKey >> 32);
  const Alias *alias = nullptr;
  uint64_t salt = 0;
  {
    std::lock_guard lock(s_lock);
    salt = s_generationSalt;
    auto found = s_current->aliases.find(name);
    if (found != s_current->aliases.end()) {
      alias = &found->second;
      if (s_reportedAliases.insert(name).second) {
        s_sdk->logger->InfoF(s_handle, "Material name %08X: using the shaders of %08X%s%s", name, alias->donor,
                             alias->bytecode.empty() ? "" : " with our pixel shader",
                             alias->vertex.empty() ? "" : " and our vertex shaders");
      }
    }
  }
  if (!alias) {
    return s_getMaterialOriginal(aCache, aKey, aEntry);
  }
  auto result = s_getMaterialOriginal(aCache, static_cast<uint64_t>(alias->donor) << 32 | static_cast<uint32_t>(aKey),
                                      aEntry);
  // The out SharedStorage<MaterialEntry> starts with the entry pointer: a fresh copy made for this caller.
  auto entry = aEntry ? *static_cast<uint8_t **>(aEntry) : nullptr;
  if (result != 1 || !entry) {
    return result;
  }
  // Points the entry's shader at offset aField to a synthetic ID that GetShader answers with aBytecode.
  auto replace = [&](size_t aField, const std::vector<uint8_t> &aBytecode) {
    uint64_t donorShader;
    std::memcpy(&donorShader, entry + aField, sizeof(donorShader));
    if (!donorShader) {
      return;
    }
    uint64_t id = SyntheticId(donorShader, name, salt);
    {
      std::lock_guard lock(s_lock);
      auto &synthetic = s_synthetic[id];
      synthetic.donorShader = donorShader;
      synthetic.bytecode = &aBytecode;
    }
    std::memcpy(entry + aField, &id, sizeof(id));
  };
  if (!alias->vertex.empty()) {
    uint64_t donorVertex;
    std::memcpy(&donorVertex, entry + kMaterialEntryVertexShader, sizeof(donorVertex));
    auto found = alias->vertex.find(donorVertex);
    if (found != alias->vertex.end()) {
      replace(kMaterialEntryVertexShader, found->second);
    } else {
      // Our pixel shader then runs after a vertex shader that may not write what it reads, and the pass draws
      // nothing: say which donor VS the material has no replacement for, once each.
      std::lock_guard lock(s_lock);
      if (s_reportedVertexMisses.insert({name, donorVertex}).second) {
        s_sdk->logger->WarnF(s_handle, "Material name %08X (key %016llX): no vertex shader of ours for donor VS %016llX",
                             name, aKey, donorVertex);
      }
    }
  }
  if (!alias->bytecode.empty()) {
    replace(kMaterialEntryPixelShader, alias->bytecode);
  }
  return result;
}

std::filesystem::path PluginFolder() {
  HMODULE module = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&PluginFolder), &module);
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(module, path, MAX_PATH);
  return std::filesystem::path(path).parent_path();
}

// One manifest; file names in it are relative to its folder.
void ReadManifest(const std::filesystem::path &aManifest, Generation &aInto) {
  std::ifstream file(aManifest, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  const auto folder = aManifest.parent_path();
  auto read = [&](const std::string &aFile, std::vector<uint8_t> &aBytes) {
    std::ifstream blob(folder / aFile, std::ios::binary);
    if (!blob) {
      return false;
    }
    aBytes.assign(std::istreambuf_iterator<char>(blob), std::istreambuf_iterator<char>());
    return true;
  };
  std::vector<std::string> warnings;
  ParseManifest(text, aManifest.filename().string(), read, aInto, warnings);
  for (const auto &warning : warnings) {
    s_sdk->logger->Warn(s_handle, warning.c_str());
  }
}

// Every *.txt under red4ext/plugins/debug_draw/shaders/, so each mod can ship its own folder there.
std::unique_ptr<Generation> ReadManifests() {
  auto generation = std::make_unique<Generation>();
  const auto root = PluginFolder() / "shaders";
  std::error_code error;
  if (!std::filesystem::is_directory(root, error)) {
    return generation;
  }
  size_t manifests = 0;
  for (const auto &entry : std::filesystem::recursive_directory_iterator(root, error)) {
    if (entry.is_regular_file() && entry.path().extension() == ".txt") {
      ReadManifest(entry.path(), *generation);
      ++manifests;
    }
  }
  if (!generation->overrides.empty() || !generation->aliases.empty()) {
    s_sdk->logger->InfoF(s_handle, "Shaders: %zu materials and %zu shader replacements from %zu manifests",
                         generation->aliases.size(), generation->overrides.size(), manifests);
  }
  return generation;
}

bool Attach(uint32_t aHash, void *aDetour, void **aOriginal, uintptr_t &aAddress, const char *aName) {
  aAddress = RED4ext::UniversalRelocBase::Resolve(aHash);
  if (!aAddress || !s_sdk->hooking->Attach(s_handle, reinterpret_cast<void *>(aAddress), aDetour, aOriginal)) {
    s_sdk->logger->ErrorF(s_handle, "Could not hook %s; shader overrides off", aName);
    aAddress = 0;
    return false;
  }
  return true;
}

// Hooks what the current generation needs and is not hooked yet (a reload can add the first manifest).
void AttachFor(const Generation &aGeneration) {
  bool customShaders = !aGeneration.overrides.empty();
  for (const auto &[name, alias] : aGeneration.aliases) {
    customShaders |= !alias.bytecode.empty() || !alias.vertex.empty();
  }
  if (customShaders && !s_getShaderAddress) {
    Attach(kGetShader, reinterpret_cast<void *>(&GetShader), reinterpret_cast<void **>(&s_getShaderOriginal),
           s_getShaderAddress, "CShaderCacheReadonly::GetShader");
  }
  if (!aGeneration.aliases.empty() && !s_getMaterialAddress) {
    Attach(kGetMaterial, reinterpret_cast<void *>(&GetMaterial), reinterpret_cast<void **>(&s_getMaterialOriginal),
           s_getMaterialAddress, "CShaderCacheReadonly::GetMaterial");
  }
}

} // namespace

void Load(const RED4ext::Sdk *aSdk, RED4ext::PluginHandle aHandle) {
  s_sdk = aSdk;
  s_handle = aHandle;
  s_generations.push_back(ReadManifests());
  s_current = s_generations.back().get();
  AttachFor(*s_current);
}

int32_t Reload() {
  auto generation = ReadManifests();
  auto materials = static_cast<int32_t>(generation->aliases.size());
  {
    std::lock_guard lock(s_lock);
    s_generations.push_back(std::move(generation));
    s_current = s_generations.back().get();
    s_generationSalt = GenerationSalt(s_generations.size() - 1);
    s_reportedAliases.clear();
  }
  // Hooking only ever adds: the generations a hook reads are never freed.
  AttachFor(*s_current);
  return materials;
}

void Unload(const RED4ext::Sdk *aSdk, RED4ext::PluginHandle aHandle) {
  for (auto address : {s_getShaderAddress, s_getMaterialAddress}) {
    if (address) {
      aSdk->hooking->Detach(aHandle, reinterpret_cast<void *>(address));
    }
  }
}

} // namespace Shaders
