#include "ShaderManifest.hpp"

#include <cstring>
#include <exception>
#include <sstream>

namespace Shaders {

void ParseManifest(std::string_view aText, const std::string &aLabel, const ReadFile &aRead, Generation &aInto,
                   std::vector<std::string> &aWarnings) {
  auto warn = [&](const std::string &aMessage) { aWarnings.push_back(aLabel + ": " + aMessage); };
  auto readBlob = [&](const std::string &aFile, std::vector<uint8_t> &aBytes) {
    aBytes.clear();
    if (!aRead(aFile, aBytes) || aBytes.size() < 32 || std::memcmp(aBytes.data(), "DXBC", 4) != 0) {
      warn(aFile + " is missing or not a DXBC container; ignored");
      return false;
    }
    return true;
  };
  auto hex = [](const std::string &aText, uint64_t &aOut) {
    try {
      size_t used = 0;
      aOut = std::stoull(aText, &used, 16);
      return used == aText.size();
    } catch (const std::exception &) {
      return false;
    }
  };
  std::istringstream lines{std::string(aText)};
  std::string line;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    std::istringstream words(line);
    std::string kind, a, b, c;
    if (!(words >> kind) || kind[0] == '#') {
      continue;
    }
    if (!(words >> a >> b)) {
      warn("cannot read \"" + line + "\"");
      continue;
    }
    words >> c;
    if (kind == "shader") {
      uint64_t id = 0;
      if (!hex(a, id)) {
        warn(a + " is not a shader ID; ignored");
        continue;
      }
      std::vector<uint8_t> bytes;
      if (!readBlob(b, bytes)) {
        continue;
      }
      aInto.overrides[id].bytecode = std::move(bytes);
    } else if (kind == "alias" || (kind == "material" && !c.empty())) {
      const auto name = NameHalf(a);
      if (aInto.aliases.contains(name)) {
        warn("material " + a + " is already defined by another manifest; ignored");
        continue;
      }
      Alias alias{NameHalf(b), {}, {}};
      if (kind == "material" && !readBlob(c, alias.bytecode)) {
        continue;
      }
      aInto.aliases[name] = std::move(alias);
    } else if (kind == "vertex" && !c.empty()) {
      auto found = aInto.aliases.find(NameHalf(a));
      uint64_t donorVertex = 0;
      if (found == aInto.aliases.end() || !hex(b, donorVertex) || !donorVertex) {
        warn("\"" + line + "\" needs a material line for " + a + " before it and a VS ID; ignored");
        continue;
      }
      std::vector<uint8_t> bytes;
      if (!readBlob(c, bytes)) {
        continue;
      }
      found->second.vertex[donorVertex] = std::move(bytes);
    } else {
      warn("cannot read \"" + line + "\"");
    }
  }
}

} // namespace Shaders
