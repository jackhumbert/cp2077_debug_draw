#include "Items.hpp"

#include "Geometry.hpp"

namespace DebugDraw {

using namespace Geometry;

void TransformItem(Item &aItem, const Engine::Mat &aM) {
  auto &v = aItem.v;
  switch (aItem.kind) {
  case Kind::Line:
  case Kind::Arrow:
  case Kind::Cylinder:
    v[0] = PointBy(aM, v[0]);
    v[1] = PointBy(aM, v[1]);
    break;
  case Kind::Box:
  case Kind::SolidBox:
    if (IsTranslationOnly(aM)) {
      v[0] = PointBy(aM, v[0]);
      v[1] = PointBy(aM, v[1]);
    } else {
      Engine::Vec4 center{(v[0].x + v[1].x) * 0.5f, (v[0].y + v[1].y) * 0.5f, (v[0].z + v[1].z) * 0.5f, 1.0f};
      Engine::Vec4 half{(v[1].x - v[0].x) * 0.5f, (v[1].y - v[0].y) * 0.5f, (v[1].z - v[0].z) * 0.5f, 0.0f};
      aItem.kind = aItem.kind == Kind::Box ? Kind::OrientedBox : Kind::SolidOrientedBox;
      v[0] = DirectionBy(aM, {half.x, 0, 0, 0});
      v[1] = DirectionBy(aM, {0, half.y, 0, 0});
      v[2] = DirectionBy(aM, {0, 0, half.z, 0});
      v[3] = PointBy(aM, center);
    }
    break;
  case Kind::Sphere:
  case Kind::SolidSphere:
  case Kind::Text:
  case Kind::Capsule:
  case Kind::SolidCapsule:
    v[0] = PointBy(aM, v[0]);
    break;
  case Kind::Circle:
  case Kind::Disc:
  case Kind::Cone:
  case Kind::SolidCone:
    v[0] = PointBy(aM, v[0]);
    v[1] = Normalized(DirectionBy(aM, v[1]), v[1]);
    break;
  case Kind::Axes:
  case Kind::OrientedBox:
  case Kind::SolidOrientedBox:
    v[0] = DirectionBy(aM, v[0]);
    v[1] = DirectionBy(aM, v[1]);
    v[2] = DirectionBy(aM, v[2]);
    v[3] = PointBy(aM, v[3]);
    break;
  case Kind::Triangle:
    v[0] = PointBy(aM, v[0]);
    v[1] = PointBy(aM, v[1]);
    v[2] = PointBy(aM, v[2]);
    break;
  case Kind::ScreenText:
  case Kind::Dim:
    break;
  }
}

} // namespace DebugDraw
