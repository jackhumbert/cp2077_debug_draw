// Vector and transform maths (Geometry.hpp).
#include "check.hpp"

#include "Geometry.hpp"

using namespace Geometry;

namespace {

constexpr float kEps = 1e-5f;

float Length(Vec4 aV) { return std::sqrt(Dot(aV, aV)); }

#define CHECK_VEC(v, ex, ey, ez)                                                                                       \
  do {                                                                                                                 \
    CHECK_NEAR((v).x, ex, kEps);                                                                                       \
    CHECK_NEAR((v).y, ey, kEps);                                                                                       \
    CHECK_NEAR((v).z, ez, kEps);                                                                                       \
  } while (0)

// A rotation about Z by aDegrees (counter-clockwise seen from above) as a quaternion i, j, k, r.
void Yaw(float aDegrees, float aQ[4]) {
  const float half = aDegrees * 3.14159265f / 360.0f;
  aQ[0] = 0;
  aQ[1] = 0;
  aQ[2] = std::sin(half);
  aQ[3] = std::cos(half);
}

Mat FromQuaternion(float aI, float aJ, float aK, float aR, Vec4 aTranslation) {
  Vec4 rows[3];
  RotationRows(aI, aJ, aK, aR, rows);
  return {rows[0], rows[1], rows[2], aTranslation};
}

} // namespace

TEST(cross_dot_normalized) {
  CHECK_VEC(Cross({1, 0, 0, 0}, {0, 1, 0, 0}), 0, 0, 1);
  CHECK_NEAR(Dot({1, 2, 3, 0}, {4, 5, 6, 0}), 32, kEps);
  CHECK_VEC(Normalized({3, 0, 4, 0}, {}), 0.6f, 0, 0.8f);
  CHECK_VEC(Normalized({0, 0, 0, 0}, {0, 0, 1, 0}), 0, 0, 1); // the fallback for a zero vector
}

TEST(basis_is_orthonormal_and_right_handed) {
  for (Vec4 n : {Vec4{0, 0, 1, 0}, Vec4{0, 0, -1, 0}, Vec4{1, 0, 0, 0}, Normalized({0.3f, -0.5f, 0.8f, 0}, {})}) {
    Vec4 u, w;
    Basis(n, u, w);
    CHECK_NEAR(Length(u), 1, kEps);
    CHECK_NEAR(Length(w), 1, kEps);
    CHECK_NEAR(Dot(u, n), 0, kEps);
    CHECK_NEAR(Dot(w, n), 0, kEps);
    CHECK_NEAR(Dot(u, w), 0, kEps);
    const auto nu = Cross(n, u);
    CHECK_VEC(nu, w.x, w.y, w.z);
  }
}

TEST(on_circle_stays_in_the_plane_at_the_radius) {
  const Vec4 center{10, -5, 2, 1}, normal = Normalized({1, 1, 1, 0}, {});
  Vec4 u, w;
  Basis(normal, u, w);
  for (int i = 0; i < 16; ++i) {
    const auto p = OnCircle(center, u, w, 2.5f, i * 0.4f);
    const Vec4 offset{p.x - center.x, p.y - center.y, p.z - center.z, 0};
    CHECK_NEAR(Length(offset), 2.5f, 1e-4f);
    CHECK_NEAR(Dot(offset, normal), 0, 1e-4f);
    CHECK(p.w == 1.0f);
  }
}

TEST(box_corners_follow_the_bit_pattern) {
  const Vec4 center{1, 2, 3, 1};
  const Vec4 axes[3]{{1, 0, 0, 0}, {0, 2, 0, 0}, {0, 0, 3, 0}};
  Vec4 corners[8];
  BoxCorners(center, axes, corners);
  CHECK_VEC(corners[0], 0, 0, 0);
  CHECK_VEC(corners[1], 2, 0, 0); // bit 0: +x
  CHECK_VEC(corners[2], 0, 4, 0); // bit 1: +y
  CHECK_VEC(corners[4], 0, 0, 6); // bit 2: +z
  CHECK_VEC(corners[7], 2, 4, 6);
}

TEST(rotation_rows_identity_and_yaw) {
  Vec4 rows[3];
  RotationRows(0, 0, 0, 1, rows);
  CHECK_VEC(rows[0], 1, 0, 0);
  CHECK_VEC(rows[1], 0, 1, 0);
  CHECK_VEC(rows[2], 0, 0, 1);
  // 90 degrees about Z, counter-clockwise from above: X turns to Y, Y (forward) to -X.
  float q[4];
  Yaw(90, q);
  RotationRows(q[0], q[1], q[2], q[3], rows);
  CHECK_VEC(rows[0], 0, 1, 0);
  CHECK_VEC(rows[1], -1, 0, 0);
  CHECK_VEC(rows[2], 0, 0, 1);
}

TEST(rotation_rows_are_a_proper_rotation) {
  const float n = std::sqrt(0.1f * 0.1f + 0.7f * 0.7f + 0.2f * 0.2f + 0.5f * 0.5f);
  Vec4 r[3];
  RotationRows(0.1f / n, -0.7f / n, 0.2f / n, 0.5f / n, r);
  for (int a = 0; a < 3; ++a) {
    CHECK_NEAR(Length(r[a]), 1, kEps);
    for (int b = a + 1; b < 3; ++b) {
      CHECK_NEAR(Dot(r[a], r[b]), 0, kEps);
    }
  }
  CHECK_NEAR(Dot(Cross(r[0], r[1]), r[2]), 1, kEps); // determinant +1: no mirroring
}

TEST(points_move_directions_only_turn) {
  float q[4];
  Yaw(90, q);
  const auto m = FromQuaternion(q[0], q[1], q[2], q[3], {10, 20, 30, 1});
  CHECK_VEC(PointBy(m, {1, 0, 0, 1}), 10, 21, 30);
  CHECK_VEC(DirectionBy(m, {1, 0, 0, 0}), 0, 1, 0);
  CHECK(PointBy(m, {}).w == 1.0f);
  CHECK(DirectionBy(m, {}).w == 0.0f);
  CHECK_VEC(PointBy(kIdentity, {4, 5, 6, 1}), 4, 5, 6);
}

TEST(compose_applies_the_child_first) {
  float q[4];
  Yaw(90, q);
  const auto parent = FromQuaternion(q[0], q[1], q[2], q[3], {100, 0, 0, 1});
  const auto child = FromQuaternion(0.2f, 0.1f, -0.3f, std::sqrt(1 - 0.04f - 0.01f - 0.09f), {1, 2, 3, 1});
  const auto both = Compose(parent, child);
  for (Vec4 p : {Vec4{0, 0, 0, 1}, Vec4{1, -2, 0.5f, 1}, Vec4{-3, 4, 7, 1}}) {
    const auto expected = PointBy(parent, PointBy(child, p));
    CHECK_VEC(PointBy(both, p), expected.x, expected.y, expected.z);
  }
}

TEST(translation_only_detection) {
  CHECK(IsTranslationOnly(kIdentity));
  CHECK(IsTranslationOnly({{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {5, -6, 7, 1}}));
  float q[4];
  Yaw(1, q);
  CHECK(!IsTranslationOnly(FromQuaternion(q[0], q[1], q[2], q[3], {})));
  CHECK(!IsTranslationOnly({{2, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {}})); // a scale is not a translation
}
