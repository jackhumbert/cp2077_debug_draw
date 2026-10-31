// What the transform stack (PushTransform / PushEntity) does to each kind of shape (Items.cpp TransformItem).
#include "check.hpp"

#include "Geometry.hpp"
#include "Items.hpp"

using namespace DebugDraw;
using namespace Geometry;

namespace {

constexpr float kEps = 1e-5f;

#define CHECK_VEC(v, ex, ey, ez)                                                                                       \
  do {                                                                                                                 \
    CHECK_NEAR((v).x, ex, kEps);                                                                                       \
    CHECK_NEAR((v).y, ey, kEps);                                                                                       \
    CHECK_NEAR((v).z, ez, kEps);                                                                                       \
  } while (0)

Item Make(Kind aKind) {
  Item item{};
  item.kind = aKind;
  return item;
}

const Mat kMove{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {10, 20, 30, 1}};
// 90 degrees about Z (X to Y, Y to -X), then moved by (10, 20, 30).
const Mat kTurn{{0, 1, 0, 0}, {-1, 0, 0, 0}, {0, 0, 1, 0}, {10, 20, 30, 1}};

} // namespace

TEST(lines_move_both_ends) {
  for (Kind kind : {Kind::Line, Kind::Arrow, Kind::Cylinder}) {
    auto item = Make(kind);
    item.v[0] = {1, 0, 0, 1};
    item.v[1] = {0, 1, 0, 1};
    TransformItem(item, kTurn);
    CHECK(item.kind == kind);
    CHECK_VEC(item.v[0], 10, 21, 30);
    CHECK_VEC(item.v[1], 9, 20, 30);
  }
}

TEST(boxes_stay_boxes_when_only_moved) {
  auto item = Make(Kind::Box);
  item.v[0] = {-1, -2, -3, 1};
  item.v[1] = {1, 2, 3, 1};
  TransformItem(item, kMove);
  CHECK(item.kind == Kind::Box);
  CHECK_VEC(item.v[0], 9, 18, 27);
  CHECK_VEC(item.v[1], 11, 22, 33);
}

TEST(boxes_become_oriented_boxes_when_turned) {
  for (auto [from, to] : {std::pair{Kind::Box, Kind::OrientedBox}, std::pair{Kind::SolidBox, Kind::SolidOrientedBox}}) {
    auto item = Make(from);
    item.v[0] = {0, 0, 0, 1};
    item.v[1] = {2, 4, 6, 1}; // centre (1, 2, 3), half extents (1, 2, 3)
    TransformItem(item, kTurn);
    CHECK(item.kind == to);
    CHECK_VEC(item.v[0], 0, 1, 0);  // half x along the turned x
    CHECK_VEC(item.v[1], -2, 0, 0); // half y along the turned y
    CHECK_VEC(item.v[2], 0, 0, 3);
    CHECK_VEC(item.v[3], 8, 21, 33); // the centre, turned and moved
  }
}

TEST(round_shapes_move_their_centre_only) {
  for (Kind kind : {Kind::Sphere, Kind::SolidSphere, Kind::Text, Kind::Capsule, Kind::SolidCapsule}) {
    auto item = Make(kind);
    item.v[0] = {1, 0, 0, 1};
    item.v[1] = {7, 7, 7, 1}; // unused by these kinds: left alone
    item.f[0] = 0.5f;
    item.f[1] = 1.5f;
    TransformItem(item, kTurn);
    CHECK_VEC(item.v[0], 10, 21, 30);
    CHECK_VEC(item.v[1], 7, 7, 7);
    CHECK(item.f[0] == 0.5f && item.f[1] == 1.5f);
  }
}

TEST(normals_turn_and_stay_unit_length) {
  // Turned and scaled by 2: the normal is turned and renormalised.
  const Mat turnAndScale{{0, 2, 0, 0}, {-2, 0, 0, 0}, {0, 0, 2, 0}, {0, 0, 0, 1}};
  for (Kind kind : {Kind::Circle, Kind::Disc, Kind::Cone, Kind::SolidCone}) {
    auto item = Make(kind);
    item.v[0] = {1, 1, 1, 1};
    item.v[1] = {1, 0, 0, 0};
    TransformItem(item, turnAndScale);
    CHECK_VEC(item.v[0], -2, 2, 2);
    CHECK_VEC(item.v[1], 0, 1, 0);
  }
}

TEST(axes_and_oriented_boxes_turn_their_rows) {
  for (Kind kind : {Kind::Axes, Kind::OrientedBox, Kind::SolidOrientedBox}) {
    auto item = Make(kind);
    item.v[0] = {1, 0, 0, 0};
    item.v[1] = {0, 1, 0, 0};
    item.v[2] = {0, 0, 1, 0};
    item.v[3] = {0, 0, 0, 1};
    TransformItem(item, kTurn);
    CHECK(item.kind == kind);
    CHECK_VEC(item.v[0], 0, 1, 0);
    CHECK_VEC(item.v[1], -1, 0, 0);
    CHECK_VEC(item.v[2], 0, 0, 1);
    CHECK_VEC(item.v[3], 10, 20, 30);
  }
}

TEST(triangles_move_all_three_corners) {
  auto item = Make(Kind::Triangle);
  item.v[0] = {1, 0, 0, 1};
  item.v[1] = {0, 1, 0, 1};
  item.v[2] = {0, 0, 1, 1};
  TransformItem(item, kTurn);
  CHECK_VEC(item.v[0], 10, 21, 30);
  CHECK_VEC(item.v[1], 9, 20, 30);
  CHECK_VEC(item.v[2], 10, 20, 31);
}

TEST(screen_shapes_are_not_moved) {
  for (Kind kind : {Kind::ScreenText, Kind::Dim}) {
    auto item = Make(kind);
    item.v[0] = {100, 200, 0, 1};
    TransformItem(item, kTurn);
    CHECK_VEC(item.v[0], 100, 200, 0);
  }
}

TEST(kind_helpers) {
  CHECK(IsMesh(Kind::Sphere) && IsMesh(Kind::SolidSphere) && !IsMesh(Kind::Box));
  CHECK(IsText(Kind::Text) && IsText(Kind::ScreenText) && !IsText(Kind::Line));
}
