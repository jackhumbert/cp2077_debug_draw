#pragma once

// The pure half of Shaders.cpp: material name hashing, synthetic shader IDs and manifest parsing. No engine calls, so
// tests/ checks it without the game.

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Shaders {

// GetShader(id) replaced from a manifest's `shader` line.
struct Override {
  std::vector<uint8_t> bytecode;
  std::vector<uint8_t> entry; // a copy of the cache's entry pointing at bytecode, made on first use
  bool reported = false;
};

// A material name that uses a donor template's techniques (`alias` / `material` / `vertex` lines).
struct Alias {
  uint32_t donor;                // the donor's name half
  std::vector<uint8_t> bytecode; // the pixel shader to use instead, or empty for the donor's own
  // Vertex shaders to use instead, by the donor's VS ID. Each vertex factory has its own VS (and input layout), so a
  // replacement is only ever put in place of the donor VS it was written for.
  std::unordered_map<uint64_t, std::vector<uint8_t>> vertex;
};

// What one reading of the manifests made. Kept for good: the engine may still use any generation's bytecode.
struct Generation {
  std::unordered_map<uint64_t, Override> overrides;
  std::unordered_map<uint32_t, Alias> aliases; // by name half
};

inline uint64_t Fnv1a64(std::string_view aText) {
  uint64_t hash = 0xCBF29CE484222325ull;
  for (unsigned char c : aText) {
    hash = (hash ^ c) * 0x100000001B3ull;
  }
  return hash;
}

inline uint32_t Fold(uint64_t aHash) { return static_cast<uint32_t>(aHash) ^ static_cast<uint32_t>(aHash >> 32); }

// The template-name half of a material lookup key (GetMaterial's key >> 32).
inline uint32_t NameHalf(std::string_view aTemplateName) { return Fold(Fnv1a64(aTemplateName)); }

// Folded into synthetic IDs per reload generation; 0 for the first.
inline uint64_t GenerationSalt(size_t aGeneration) { return aGeneration * 0x9E3779B97F4A7C15ull; }

// The ID GetMaterial hands out in place of aDonorShader for the material with name half aName.
inline uint64_t SyntheticId(uint64_t aDonorShader, uint32_t aName, uint64_t aSalt) {
  return aDonorShader ^ (static_cast<uint64_t>(aName) << 32 | aName) ^ 0x6465627567647261ull ^ aSalt; // "debugdra"
}

// Reads a shader file named in a manifest (relative to it); false if it cannot be read.
using ReadFile = std::function<bool(const std::string &aFile, std::vector<uint8_t> &aBytes)>;

// Adds one manifest's lines to aInto. aLabel names the manifest in the messages added to aWarnings, one per line
// that was ignored and why. Lines:
//   shader <id hex> <file>
//   alias <name> <donor>
//   material <name> <donor> <ps file>
//   vertex <name> <donor VS id hex> <file>     (after that material's line)
//   # comment
void ParseManifest(std::string_view aText, const std::string &aLabel, const ReadFile &aRead, Generation &aInto,
                   std::vector<std::string> &aWarnings);

} // namespace Shaders
