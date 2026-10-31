// Material name hashing, synthetic shader IDs and manifest parsing. The expected IDs are the ones the game logged
// (red4ext/logs/debug_draw-*.log, 2026-10-30): if they change, our materials no longer find their shaders.
#include "check.hpp"

#include "ShaderManifest.hpp"

#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>

using namespace Shaders;

namespace {

constexpr uint64_t kDebugDrawPS = 0x2B2312895A89DAD6ull;      // debugdraw's pixel shader
constexpr uint64_t kDrawBufferVS = 0x46AB074CDBB5F2E3ull;     // its DrawBuffer vertex factory's VS
constexpr uint64_t kDebugVS = 0x687287DC8EC613C8ull;          // its Debug (instanced) vertex factory's VS

std::vector<uint8_t> Dxbc(uint8_t aTag) {
  std::vector<uint8_t> bytes(64, aTag);
  std::memcpy(bytes.data(), "DXBC", 4);
  return bytes;
}

// A reader over named in-memory files.
ReadFile Files(std::map<std::string, std::vector<uint8_t>> aFiles) {
  return [files = std::move(aFiles)](const std::string &aFile, std::vector<uint8_t> &aBytes) {
    auto found = files.find(aFile);
    if (found == files.end()) {
      return false;
    }
    aBytes = found->second;
    return true;
  };
}

} // namespace

TEST(fnv1a64_reference_values) {
  CHECK(Fnv1a64("") == 0xCBF29CE484222325ull);
  CHECK(Fnv1a64("a") == 0xAF63DC4C8601EC8Cull);
}

TEST(name_halves_match_the_game) {
  CHECK(NameHalf("debugdraw") == 0x3C52AAB0u);
  CHECK(NameHalf("debug_draw_pulse") == 0x636CEBC0u);
  CHECK(NameHalf("debug_draw_scanlines") == 0xC1A19D79u);
  CHECK(NameHalf("debug_draw_lit") == 0x297A1617u);
  CHECK(NameHalf("debug_draw_grid") == 0x2AA7A3AAu);
  CHECK(NameHalf("debug_draw_hologram") == 0x1E88AE5Cu);
  CHECK(NameHalf("debug_draw_fade") == 0x0B59B525u);
}

TEST(synthetic_ids_match_the_game) {
  CHECK(SyntheticId(kDebugDrawPS, NameHalf("debug_draw_pulse"), GenerationSalt(0)) == 0x2C2A9B3C5E814377ull);
  CHECK(SyntheticId(kDebugDrawPS, NameHalf("debug_draw_scanlines"), GenerationSalt(0)) == 0x8EE7ED85FC4C35CEull);
  // After the first and second ReloadShaders.
  CHECK(SyntheticId(kDebugDrawPS, NameHalf("debug_draw_grid"), GenerationSalt(1)) == 0xFBD6AAEF68007708ull);
  CHECK(SyntheticId(kDebugDrawPS, NameHalf("debug_draw_grid"), GenerationSalt(2)) == 0x598F2024E9DEF337ull);
}

TEST(synthetic_ids_differ_per_material_shader_and_generation) {
  const auto grid = NameHalf("debug_draw_grid"), lit = NameHalf("debug_draw_lit");
  const auto id = SyntheticId(kDebugDrawPS, grid, 0);
  CHECK(id != kDebugDrawPS);
  CHECK(id != SyntheticId(kDebugDrawPS, lit, 0));
  CHECK(id != SyntheticId(kDebugVS, grid, 0));
  for (size_t generation = 1; generation < 64; ++generation) {
    CHECK(id != SyntheticId(kDebugDrawPS, grid, GenerationSalt(generation)));
  }
  CHECK(GenerationSalt(0) == 0);
}

TEST(parses_the_shipped_manifest) {
  std::ifstream file(DEBUG_DRAW_SOURCE_DIR "/src/game_files/red4ext/plugins/debug_draw/shaders/debug_draw/debug_draw.txt",
                     std::ios::binary);
  CHECK(file.good());
  const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  const ReadFile anyFile = [](const std::string &, std::vector<uint8_t> &aBytes) {
    aBytes = Dxbc(1);
    return true;
  };
  Generation generation;
  std::vector<std::string> warnings;
  ParseManifest(text, "debug_draw.txt", anyFile, generation, warnings);
  CHECK(warnings.empty());
  CHECK(generation.aliases.size() == 6);
  CHECK(generation.overrides.empty());
  for (const char *name : {"debug_draw_pulse", "debug_draw_scanlines", "debug_draw_lit", "debug_draw_grid",
                           "debug_draw_hologram", "debug_draw_fade"}) {
    auto found = generation.aliases.find(NameHalf(name));
    CHECK(found != generation.aliases.end());
    if (found != generation.aliases.end()) {
      CHECK(found->second.donor == NameHalf("debugdraw"));
      CHECK(!found->second.bytecode.empty());
    }
  }
  // The materials with their own vertex shaders have one per debugdraw vertex factory; the others none.
  for (const char *name : {"debug_draw_lit", "debug_draw_grid", "debug_draw_hologram"}) {
    const auto &vertex = generation.aliases[NameHalf(name)].vertex;
    CHECK(vertex.size() == 2 && vertex.contains(kDrawBufferVS) && vertex.contains(kDebugVS));
  }
  for (const char *name : {"debug_draw_pulse", "debug_draw_scanlines", "debug_draw_fade"}) {
    CHECK(generation.aliases[NameHalf(name)].vertex.empty());
  }
}

TEST(manifest_lines_of_every_kind) {
  const auto read = Files({{"ps.dxbc", Dxbc(1)}, {"vs.dxbc", Dxbc(2)}, {"other.dxbc", Dxbc(3)}});
  Generation generation;
  std::vector<std::string> warnings;
  ParseManifest("# a comment\r\n"
                "\r\n"
                "material my_mod_glow debugdraw ps.dxbc\r\n"
                "vertex my_mod_glow 46AB074CDBB5F2E3 vs.dxbc\r\n"
                "alias my_mod_plain debugdraw\r\n"
                "shader 1234ABCD other.dxbc\r\n",
                "test.txt", read, generation, warnings);
  CHECK(warnings.empty());
  const auto &glow = generation.aliases[NameHalf("my_mod_glow")];
  CHECK(glow.donor == NameHalf("debugdraw"));
  CHECK(glow.bytecode == Dxbc(1));
  CHECK(glow.vertex.size() == 1 && glow.vertex.at(kDrawBufferVS) == Dxbc(2));
  const auto &plain = generation.aliases[NameHalf("my_mod_plain")];
  CHECK(plain.bytecode.empty() && plain.vertex.empty());
  CHECK(generation.overrides.size() == 1 && generation.overrides[0x1234ABCD].bytecode == Dxbc(3));
}

TEST(manifest_problems_are_reported_and_skipped) {
  auto bad = Dxbc(1);
  bad[0] = 'X';
  const auto read = Files({{"ps.dxbc", Dxbc(1)}, {"bad.dxbc", bad}, {"short.dxbc", {'D', 'X', 'B', 'C'}}});
  Generation generation;
  std::vector<std::string> warnings;
  ParseManifest("material a_missing debugdraw nowhere.dxbc\n"     // file missing
                "material a_bad debugdraw bad.dxbc\n"             // not DXBC
                "material a_short debugdraw short.dxbc\n"         // too short
                "vertex a_first 46AB074CDBB5F2E3 ps.dxbc\n"       // before its material
                "material a_ok debugdraw ps.dxbc\n"
                "material a_ok debugdraw ps.dxbc\n"               // defined twice
                "vertex a_ok notahexid ps.dxbc\n"                 // bad VS ID
                "vertex a_ok 0 ps.dxbc\n"                         // VS ID 0
                "shader 12zz ps.dxbc\n"                           // bad shader ID
                "texture a_ok foo bar\n"                          // unknown kind
                "material\n",                                     // too short
                "test.txt", read, generation, warnings);
  CHECK(warnings.size() == 10);
  CHECK(generation.aliases.size() == 1 && generation.aliases.contains(NameHalf("a_ok")));
  CHECK(generation.aliases[NameHalf("a_ok")].vertex.empty());
  CHECK(generation.overrides.empty());
  for (const auto &warning : warnings) {
    CHECK(warning.rfind("test.txt: ", 0) == 0);
  }
}
