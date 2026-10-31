#pragma once

// The queued shape (Item) and the transform stack's effect on it. No engine calls, so tests/ checks it without the
// game.

// RED4ext.hpp first: it defines RED4EXT_HEADER_ONLY, which the SDK headers need for their inline bodies.
#include <RED4ext/RED4ext.hpp>

#include "Engine.hpp"

#include <RED4ext/Handle.hpp>
#include <RED4ext/Scripting/IScriptable.hpp>

#include <chrono>
#include <cstdint>
#include <string>

namespace DebugDraw {

using Clock = std::chrono::steady_clock;

enum class Kind : uint8_t {
  Line,        // v0 a, v1 b
  Box,         // v0 min, v1 max
  Sphere,      // v0 center, f0 radius
  Text,        // v0 position, f0 scale
  ScreenText,  // v0.x, v0.y pixels, f0 scale
  Arrow,       // v0 from, v1 to
  Axes,        // v0..v2 rotation rows, v3 position, f0 length
  Circle,      // v0 center, v1 unit normal, f0 radius
  Capsule,     // v0 center, f0 radius, f1 half the cylinder's length (vertical)
  Cylinder,    // v0 a, v1 b, f0 radius
  Cone,        // v0 apex, v1 unit direction, f0 length, f1 base radius
  OrientedBox, // v0..v2 half-extent axes, v3 center
  SolidBox,    // v0 min, v1 max
  SolidSphere, // v0 center, f0 radius
  Dim,         // v0 camera position, f0 radius: an inward-facing translucent sphere around the camera, drawn first
  Triangle,    // v0, v1, v2 corners (two-sided)
  Disc,        // v0 center, v1 unit normal, f0 radius
  SolidOrientedBox, // v0..v2 half-extent axes, v3 center
  SolidCone,   // v0 apex, v1 unit direction, f0 length, f1 base radius
  SolidCapsule, // v0 center, f0 radius, f1 half the cylinder's length (vertical)
};

struct Item {
  Kind kind;
  bool overlay;
  Engine::Rgba color;
  Engine::Vec4 v[4];
  float f[2];
  float thickness; // metres; 0 draws thin lines. Only the line-built shapes and Arrow use it
  std::string text;
  Clock::time_point expires;
  uint64_t channel; // CName hash, 0 for None
  // Shapes queued under PushEntity: v and f are in that entity's space and follow it (see Place / BuildDrawer).
  RED4ext::WeakHandle<RED4ext::IScriptable> anchor;
};

// Shapes the engine draws from a cached mesh, as a fragment of their own.
inline bool IsMesh(Kind aKind) { return aKind == Kind::Sphere || aKind == Kind::SolidSphere; }

inline bool IsText(Kind aKind) { return aKind == Kind::Text || aKind == Kind::ScreenText; }

// Moves a shape by aM. Boxes given by two corners become oriented boxes when aM rotates. Capsules stay upright (the
// engine's are vertical), and text keeps its screen-facing look.
void TransformItem(Item &aItem, const Engine::Mat &aM);

} // namespace DebugDraw
