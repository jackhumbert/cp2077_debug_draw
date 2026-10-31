#pragma once

// Vector and transform maths for shapes: no engine calls, so tests/ checks it without the game. Vec4 w is 1 for a
// point and 0 for a direction. A Mat's rows X, Y, Z are the axes and W the translation.

#include "Engine.hpp"

#include <cmath>
#include <cstdint>

namespace Geometry {

using Engine::Mat;
using Engine::Vec4;

inline Vec4 Add(Vec4 aA, Vec4 aB) { return {aA.x + aB.x, aA.y + aB.y, aA.z + aB.z, 1.0f}; }
inline Vec4 Scale(Vec4 aV, float aS) { return {aV.x * aS, aV.y * aS, aV.z * aS, 0.0f}; }
inline Vec4 Cross(Vec4 aA, Vec4 aB) {
  return {aA.y * aB.z - aA.z * aB.y, aA.z * aB.x - aA.x * aB.z, aA.x * aB.y - aA.y * aB.x, 0.0f};
}
inline float Dot(Vec4 aA, Vec4 aB) { return aA.x * aB.x + aA.y * aB.y + aA.z * aB.z; }
inline Vec4 Normalized(Vec4 aV, Vec4 aFallback) {
  float length = std::sqrt(aV.x * aV.x + aV.y * aV.y + aV.z * aV.z);
  return length > 1e-6f ? Scale(aV, 1.0f / length) : aFallback;
}

// Two unit vectors perpendicular to aNormal and to each other.
inline void Basis(Vec4 aNormal, Vec4 &aU, Vec4 &aW) {
  Vec4 helper = std::fabs(aNormal.z) < 0.9f ? Vec4{0, 0, 1, 0} : Vec4{1, 0, 0, 0};
  aU = Normalized(Cross(aNormal, helper), {1, 0, 0, 0});
  aW = Cross(aNormal, aU);
}

inline Vec4 OnCircle(Vec4 aCenter, Vec4 aU, Vec4 aW, float aRadius, float aAngle) {
  return Add(aCenter, Add(Scale(aU, aRadius * std::cos(aAngle)), Scale(aW, aRadius * std::sin(aAngle))));
}

// Corner i takes +axis or -axis by bit (0 x, 1 y, 2 z) of i.
inline void BoxCorners(const Vec4 &aCenter, const Vec4 aAxes[3], Vec4 aCorners[8]) {
  for (uint32_t i = 0; i < 8; ++i) {
    auto corner = aCenter;
    for (uint32_t axis = 0; axis < 3; ++axis) {
      corner = Add(corner, Scale(aAxes[axis], (i >> axis) & 1 ? 1.0f : -1.0f));
    }
    aCorners[i] = corner;
  }
}

// A rotation's X, Y and Z axes (the game's Z up, Y forward), the rows of its matrix, from the quaternion i, j, k, r.
inline void RotationRows(float aI, float aJ, float aK, float aR, Vec4 aRows[3]) {
  const float x = aI, y = aJ, z = aK, w = aR;
  aRows[0] = {1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y), 0};
  aRows[1] = {2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x), 0};
  aRows[2] = {2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y), 0};
}

inline const Mat kIdentity{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};

inline Vec4 PointBy(const Mat &aM, const Vec4 &aP) {
  return {aM.w.x + aM.x.x * aP.x + aM.y.x * aP.y + aM.z.x * aP.z, aM.w.y + aM.x.y * aP.x + aM.y.y * aP.y + aM.z.y * aP.z,
          aM.w.z + aM.x.z * aP.x + aM.y.z * aP.y + aM.z.z * aP.z, 1.0f};
}

inline Vec4 DirectionBy(const Mat &aM, const Vec4 &aD) {
  return {aM.x.x * aD.x + aM.y.x * aD.y + aM.z.x * aD.z, aM.x.y * aD.x + aM.y.y * aD.y + aM.z.y * aD.z,
          aM.x.z * aD.x + aM.y.z * aD.y + aM.z.z * aD.z, 0.0f};
}

// aParent applied after aChild.
inline Mat Compose(const Mat &aParent, const Mat &aChild) {
  return {DirectionBy(aParent, aChild.x), DirectionBy(aParent, aChild.y), DirectionBy(aParent, aChild.z),
          PointBy(aParent, aChild.w)};
}

inline bool IsTranslationOnly(const Mat &aM) {
  return std::fabs(aM.x.x - 1) + std::fabs(aM.y.y - 1) + std::fabs(aM.z.z - 1) + std::fabs(aM.x.y) + std::fabs(aM.x.z) +
             std::fabs(aM.y.x) + std::fabs(aM.y.z) + std::fabs(aM.z.x) + std::fabs(aM.z.y) <
         1e-5f;
}

} // namespace Geometry
