// World-space debug drawing through the game's own debug renderer (red4ext/plugins/debug_draw).
//
// Positions are world space (Z up). seconds <= 0 draws for one frame (shapes queued in one frame replace the
// previous frame's set, so calling every frame is flicker free and stopping clears them within 0.1 s); a positive
// value keeps the shape for that many real-time seconds (it keeps counting while the game is paused).
// Shapes are depth tested against the scene unless overlay is true. Text is always drawn on top.
// A color with alpha below 255 draws translucent (most useful on the solid shapes).
//
// thickness (metres, optional, last) draws lines as thin prisms of that radius instead of hairlines; the line-built
// shapes take it (Line, Lines, Path, Arrow, Box, OrientedBox, Circle, Cone, Bounds).
//
// channel tags shapes so Clear(channel) drops only those: give your mod its own (n"my_mod") so another mod's
// Clear() cannot remove your shapes, or yours theirs.

native func DebugDraw_Line(a: Vector4, b: Vector4, color: Color, seconds: Float, overlay: Bool, channel: CName, thickness: Float)
native func DebugDraw_Lines(points: array<Vector4>, color: Color, seconds: Float, overlay: Bool, channel: CName, thickness: Float)
native func DebugDraw_Arrow(from: Vector4, to: Vector4, color: Color, seconds: Float, overlay: Bool, channel: CName, thickness: Float)
native func DebugDraw_Box(min: Vector4, max: Vector4, color: Color, seconds: Float, overlay: Bool, channel: CName, thickness: Float)
native func DebugDraw_SolidBox(min: Vector4, max: Vector4, color: Color, seconds: Float, overlay: Bool, channel: CName, thickness: Float)
native func DebugDraw_OrientedBox(transform: Transform, halfExtents: Vector4, color: Color, seconds: Float, overlay: Bool, channel: CName, thickness: Float)
native func DebugDraw_Sphere(center: Vector4, radius: Float, color: Color, seconds: Float, overlay: Bool, channel: CName, thickness: Float)
native func DebugDraw_SolidSphere(center: Vector4, radius: Float, color: Color, seconds: Float, overlay: Bool, channel: CName, thickness: Float)
native func DebugDraw_Circle(center: Vector4, normal: Vector4, radius: Float, color: Color, seconds: Float, overlay: Bool, channel: CName, thickness: Float)
native func DebugDraw_Capsule(center: Vector4, radius: Float, height: Float, color: Color, seconds: Float, overlay: Bool, channel: CName, thickness: Float)
native func DebugDraw_Cylinder(a: Vector4, b: Vector4, radius: Float, color: Color, seconds: Float, overlay: Bool, channel: CName, thickness: Float)
native func DebugDraw_Cone(apex: Vector4, direction: Vector4, length: Float, angle: Float, color: Color, seconds: Float, overlay: Bool, channel: CName, thickness: Float)
native func DebugDraw_Axes(transform: Transform, length: Float, seconds: Float, overlay: Bool, channel: CName)
native func DebugDraw_Text(position: Vector4, text: String, color: Color, seconds: Float, channel: CName, scale: Float)
native func DebugDraw_ScreenText(x: Int32, y: Int32, text: String, color: Color, seconds: Float, channel: CName, scale: Float)
native func DebugDraw_Triangles(points: array<Vector4>, color: Color, seconds: Float, overlay: Bool, channel: CName, thickness: Float)
native func DebugDraw_Disc(center: Vector4, normal: Vector4, radius: Float, color: Color, seconds: Float, overlay: Bool, channel: CName, thickness: Float)
native func DebugDraw_SolidOrientedBox(transform: Transform, halfExtents: Vector4, color: Color, seconds: Float, overlay: Bool, channel: CName, thickness: Float)
native func DebugDraw_SolidCone(apex: Vector4, direction: Vector4, length: Float, angle: Float, color: Color, seconds: Float, overlay: Bool, channel: CName, thickness: Float)
native func DebugDraw_SolidCapsule(center: Vector4, radius: Float, height: Float, color: Color, seconds: Float, overlay: Bool, channel: CName, thickness: Float)
native func DebugDraw_PushTransform(transform: Transform)
native func DebugDraw_PushEntity(entity: ref<Entity>)
native func DebugDraw_PopTransform()
native func DebugDraw_SetXRay(channel: CName, alpha: Float)
native func DebugDraw_Dim(camera: Vector4, radius: Float, amount: Float)
native func DebugDraw_GetBounds(entity: ref<Entity>) -> Box
native func DebugDraw_SetMaterial(channel: CName, path: String, translucent: Bool)
native func DebugDraw_SetMaterialFloat(channel: CName, name: CName, value: Float)
native func DebugDraw_SetMaterialColor(channel: CName, name: CName, value: Color)
native func DebugDraw_SetMaterialVector(channel: CName, name: CName, value: Vector4)
native func DebugDraw_ReloadShaders() -> Int32
native func DebugDraw_SetDisplay(visible: Bool, textScale: Float, labelDistance: Float, dimStrength: Float)
native func DebugDraw_LabelDistanceScale() -> Float
native func DebugDraw_SetWorldOverlays(entity: ref<Entity>, kinds: Int32, radius: Float, throughWalls: Bool)
native func DebugDraw_Plot(name: String, value: Float, color: Color)
native func DebugDraw_ClearPlot(name: String)
native func DebugDraw_Clear(channel: CName)
native func DebugDraw_ClearAll()

public abstract class DebugDraw {
  public static func Line(a: Vector4, b: Vector4, color: Color, opt seconds: Float, opt overlay: Bool, opt channel: CName, opt thickness: Float) {
    DebugDraw_Line(a, b, color, seconds, overlay, channel, thickness);
  }

  // Many lines in one call: point pairs (a, b, a, b, ...). An odd last point is ignored.
  public static func Lines(points: array<Vector4>, color: Color, opt seconds: Float, opt overlay: Bool, opt channel: CName, opt thickness: Float) {
    DebugDraw_Lines(points, color, seconds, overlay, channel, thickness);
  }

  // A line with a head at to; the head is the last 20 % of the length.
  public static func Arrow(from: Vector4, to: Vector4, color: Color, opt seconds: Float, opt overlay: Bool, opt channel: CName, opt thickness: Float) {
    DebugDraw_Arrow(from, to, color, seconds, overlay, channel, thickness);
  }

  // Axis-aligned wireframe box between two corners.
  public static func Box(min: Vector4, max: Vector4, color: Color, opt seconds: Float, opt overlay: Bool, opt channel: CName, opt thickness: Float) {
    DebugDraw_Box(min, max, color, seconds, overlay, channel, thickness);
  }

  // Axis-aligned filled box between two corners.
  public static func SolidBox(min: Vector4, max: Vector4, color: Color, opt seconds: Float, opt overlay: Bool, opt channel: CName) {
    DebugDraw_SolidBox(min, max, color, seconds, overlay, channel, 0.0);
  }

  // Wireframe box centred on transform's position and rotated with it; halfExtents along its X, Y and Z.
  public static func OrientedBox(transform: Transform, halfExtents: Vector4, color: Color, opt seconds: Float, opt overlay: Bool, opt channel: CName, opt thickness: Float) {
    DebugDraw_OrientedBox(transform, halfExtents, color, seconds, overlay, channel, thickness);
  }

  public static func Sphere(center: Vector4, radius: Float, color: Color, opt seconds: Float, opt overlay: Bool, opt channel: CName) {
    DebugDraw_Sphere(center, radius, color, seconds, overlay, channel, 0.0);
  }

  public static func SolidSphere(center: Vector4, radius: Float, color: Color, opt seconds: Float, opt overlay: Bool, opt channel: CName) {
    DebugDraw_SolidSphere(center, radius, color, seconds, overlay, channel, 0.0);
  }

  // A circle facing normal; a zero normal means up (a horizontal circle).
  public static func Circle(center: Vector4, normal: Vector4, radius: Float, color: Color, opt seconds: Float, opt overlay: Bool, opt channel: CName, opt thickness: Float) {
    DebugDraw_Circle(center, normal, radius, color, seconds, overlay, channel, thickness);
  }

  // An upright capsule centred on center; height is the whole capsule, end caps included.
  public static func Capsule(center: Vector4, radius: Float, height: Float, color: Color, opt seconds: Float, opt overlay: Bool, opt channel: CName) {
    DebugDraw_Capsule(center, radius, height, color, seconds, overlay, channel, 0.0);
  }

  // A wireframe cylinder from a to b.
  public static func Cylinder(a: Vector4, b: Vector4, radius: Float, color: Color, opt seconds: Float, opt overlay: Bool, opt channel: CName) {
    DebugDraw_Cylinder(a, b, radius, color, seconds, overlay, channel, 0.0);
  }

  // A cone opening from apex along direction, length long; angle is the half-angle in degrees.
  public static func Cone(apex: Vector4, direction: Vector4, length: Float, angle: Float, color: Color, opt seconds: Float, opt overlay: Bool, opt channel: CName, opt thickness: Float) {
    DebugDraw_Cone(apex, direction, length, angle, color, seconds, overlay, channel, thickness);
  }

  // transform's X, Y and Z axes as red, green and blue lines, length long.
  public static func Axes(transform: Transform, length: Float, opt seconds: Float, opt overlay: Bool, opt channel: CName) {
    DebugDraw_Axes(transform, length, seconds, overlay, channel);
  }

  // A label at a world position, always on top, with a dark outline. scale 0 (or left out) is 1. maxDistance (metres,
  // 0 for no limit) skips it when the camera is farther away; it is checked when the label is queued, so it suits
  // labels drawn every frame.
  public static func Text(position: Vector4, text: String, color: Color, opt seconds: Float, opt channel: CName, opt scale: Float, opt maxDistance: Float) {
    if maxDistance > 0.0 && Vector4.Distance(DebugDraw.CameraPosition(), position) > maxDistance * DebugDraw_LabelDistanceScale() {
      return;
    }
    DebugDraw_Text(position, text, color, seconds, channel, scale);
  }

  // The active camera's world position (zero when there is none).
  public static func CameraPosition() -> Vector4 {
    let camera: Transform;
    GameInstance.GetCameraSystem(GetGameInstance()).GetActiveCameraWorldTransform(camera);
    return camera.position;
  }

  // A label at x, y screen pixels from the top left. scale 0 (or left out) is 1.
  public static func ScreenText(x: Int32, y: Int32, text: String, color: Color, opt seconds: Float, opt channel: CName, opt scale: Float) {
    DebugDraw_ScreenText(x, y, text, color, seconds, channel, scale);
  }

  // The box around an entity's skinned meshes (characters, and vehicles' skinned parts) in the entity's own space:
  // the engine keeps mesh bounds in model space. Min is above Max when it has none: check with HasBounds.
  public static func GetLocalBounds(entity: ref<Entity>) -> Box {
    return DebugDraw_GetBounds(entity);
  }

  public static func HasBounds(box: Box) -> Bool {
    return box.Min.X <= box.Max.X;
  }

  // The entity's bounds as a box turned with it. False (and nothing drawn) when it has none.
  public static func Bounds(entity: ref<Entity>, color: Color, opt seconds: Float, opt overlay: Bool, opt channel: CName, opt thickness: Float) -> Bool {
    let box = DebugDraw_GetBounds(entity);
    if !DebugDraw.HasBounds(box) {
      return false;
    }
    let transform: Transform;
    transform.orientation = entity.GetWorldOrientation();
    transform.position = DebugDraw.ToWorld(entity, (box.Min + box.Max) * 0.5);
    DebugDraw_OrientedBox(transform, (box.Max - box.Min) * 0.5, color, seconds, overlay, channel, thickness);
    return true;
  }

  // An arrow from the entity's centre showing where it will be in half a second, with its speed: vehicles (linear
  // velocity) and characters (puppet velocity). False for anything else.
  public static func Velocity(entity: ref<GameObject>, opt seconds: Float, opt overlay: Bool, opt channel: CName) -> Bool {
    let velocity: Vector4;
    let vehicle = entity as VehicleObject;
    let puppet = entity as gamePuppet;
    if IsDefined(vehicle) {
      velocity = vehicle.GetLinearVelocity();
    } else if IsDefined(puppet) {
      velocity = puppet.GetVelocity();
    } else {
      return false;
    }
    let center = DebugDraw.Center(entity);
    let speed = Vector4.Length(velocity);
    if speed > 0.05 {
      DebugDraw_Arrow(center, center + velocity * 0.5, DebugDraw.Rgba(255, 90, 60), seconds, overlay, channel, 0.0);
    }
    DebugDraw_Text(center + velocity * 0.5, FloatToStringPrec(speed, 1) + " m/s", DebugDraw.Rgba(255, 170, 150), seconds, channel, 0.0);
    return true;
  }

  // A label just above the entity (above its bounds, or 2 m above its position without bounds). Call it every
  // frame with seconds 0 to keep it on a moving entity.
  public static func Label(entity: ref<Entity>, text: String, color: Color, opt seconds: Float, opt channel: CName, opt scale: Float, opt maxDistance: Float) {
    let box = DebugDraw_GetBounds(entity);
    let top = DebugDraw.HasBounds(box)
      ? DebugDraw.ToWorld(entity, new Vector4((box.Min.X + box.Max.X) * 0.5, (box.Min.Y + box.Max.Y) * 0.5, box.Max.Z + 0.25, 1.0))
      : entity.GetWorldPosition() + new Vector4(0.0, 0.0, 2.0, 0.0);
    DebugDraw.Text(top, text, color, seconds, channel, scale, maxDistance);
  }

  // The centre of the entity's bounds in world space, or 1 m above its position without bounds.
  public static func Center(entity: ref<Entity>) -> Vector4 {
    let box = DebugDraw_GetBounds(entity);
    if DebugDraw.HasBounds(box) {
      return DebugDraw.ToWorld(entity, (box.Min + box.Max) * 0.5);
    }
    return entity.GetWorldPosition() + new Vector4(0.0, 0.0, 1.0, 0.0);
  }

  // A point in the entity's own space, in world space.
  public static func ToWorld(entity: ref<Entity>, local: Vector4) -> Vector4 {
    let offset = Quaternion.Transform(entity.GetWorldOrientation(), new Vector4(local.X, local.Y, local.Z, 0.0));
    return entity.GetWorldPosition() + offset;
  }

  // Connected lines through points in order (a route, a path, a camera track).
  public static func Path(points: array<Vector4>, color: Color, opt seconds: Float, opt overlay: Bool, opt channel: CName, opt thickness: Float) {
    let pairs: array<Vector4>;
    let i = 1;
    while i < ArraySize(points) {
      ArrayPush(pairs, points[i - 1]);
      ArrayPush(pairs, points[i]);
      i += 1;
    }
    DebugDraw_Lines(pairs, color, seconds, overlay, channel, thickness);
  }

  // A ray and what it hit: green up to hitPosition with a marker there and grey beyond, or red all the way when
  // it missed.
  public static func Raycast(from: Vector4, to: Vector4, hit: Bool, hitPosition: Vector4, opt seconds: Float, opt overlay: Bool, opt channel: CName) {
    if hit {
      DebugDraw_Line(from, hitPosition, DebugDraw.Rgba(60, 255, 90), seconds, overlay, channel, 0.0);
      DebugDraw_Sphere(hitPosition, 0.06, DebugDraw.Rgba(60, 255, 90), seconds, overlay, channel, 0.0);
      DebugDraw_Line(hitPosition, to, DebugDraw.Rgba(170, 170, 170, 110), seconds, overlay, channel, 0.0);
    } else {
      DebugDraw_Line(from, to, DebugDraw.Rgba(255, 60, 60), seconds, overlay, channel, 0.0);
    }
  }

  // Raycast for a TraceResult (SpatialQueriesSystem), with the hit normal as a short arrow.
  public static func Trace(from: Vector4, to: Vector4, result: TraceResult, opt seconds: Float, opt overlay: Bool, opt channel: CName) {
    let hit = TraceResult.IsValid(result);
    let position = Vector4.Vector3To4(result.position);
    DebugDraw.Raycast(from, to, hit, position, seconds, overlay, channel);
    if hit {
      DebugDraw_Arrow(position, position + Vector4.Vector3To4(result.normal) * 0.5, DebugDraw.Rgba(255, 220, 60), seconds, overlay, channel, 0.0);
    }
  }

  // A rolling graph of a value on screen (one sample per call, the last 240 kept), with its name, latest value and
  // range. Call it every frame; a plot that gets no value for 2 s disappears.
  public static func Plot(name: String, value: Float, opt color: Color) {
    if Cast<Int32>(color.Alpha) == 0 {
      color = DebugDraw.Rgba(80, 220, 255);
    }
    DebugDraw_Plot(name, value, color);
  }

  public static func ClearPlot(name: String) {
    DebugDraw_ClearPlot(name);
  }

  // Darkens the scene by amount (0..1) for this frame, so the debug shapes stand out; call it every frame.
  public static func Dim(amount: Float) {
    let camera: Transform;
    if GameInstance.GetCameraSystem(GetGameInstance()).GetActiveCameraWorldTransform(camera) {
      DebugDraw_Dim(camera.position, 0.5, amount);
    }
  }

  // Draws this channel's shapes with a material (a .mt or .mi depot path) instead of the engine's debug material;
  // "" goes back to the default. Text keeps the default. Only materials with shaders for the debug-draw pass draw:
  // the engine's own two (default and MaterialStripes) and the custom-shader materials made with tools/shaderc,
  // such as Debug Draw's built-in effects below. Others load but draw nothing. translucent draws all the channel's
  // shapes in the translucent pass, so the alpha the material's shader writes counts even for opaque colours: needed
  // for MaterialFade and MaterialHologram.
  public static func SetMaterial(channel: CName, path: String, opt translucent: Bool) {
    DebugDraw_SetMaterial(channel, path, translucent);
  }

  // Sets a parameter of the channel's material (after SetMaterial; a new SetMaterial path drops them). name is the
  // parameter's name in the .mt (a shaderc @param), and the type must match it: Float for scalar, Color for color.
  // The channel gets its own copy of the material, so other channels using the same one are not changed. Each change
  // rebuilds that copy's render data: fine for tweaks and gameplay values, not meant for every frame (animate in the
  // shader with the time instead). A wrong name or type is logged once.
  public static func SetMaterialFloat(channel: CName, name: CName, value: Float) {
    DebugDraw_SetMaterialFloat(channel, name, value);
  }

  public static func SetMaterialColor(channel: CName, name: CName, value: Color) {
    DebugDraw_SetMaterialColor(channel, name, value);
  }

  public static func SetMaterialVector(channel: CName, name: CName, value: Vector4) {
    DebugDraw_SetMaterialVector(channel, name, value);
  }

  // Reads the shader manifests and files under red4ext/plugins/debug_draw/shaders/ again and rebuilds the channels'
  // materials, so a shader rebuilt with tools/shaderc shows without restarting the game. New materials need their .mt
  // in an archive, which still needs a restart. Returns the number of materials in the manifests (-1: unsupported).
  public static func ReloadShaders() -> Int32 {
    return DebugDraw_ReloadShaders();
  }

  // Diagonal stripes (the engine's own debugdraw_diagonal_stripes material).
  public static func MaterialStripes() -> String {
    return "engine\\materials\\debugdraw_diagonal_stripes.mt";
  }

  // A pulsing glow with a band sweeping down the screen (built in, tools/shaders/pulse.hlsl).
  public static func MaterialPulse() -> String {
    return "debug_draw\\shaders\\debug_draw\\pulse.mt";
  }

  // CRT-style scanlines (built in, tools/shaders/scanlines.hlsl).
  public static func MaterialScanlines() -> String {
    return "debug_draw\\shaders\\debug_draw\\scanlines.mt";
  }

  // Simple lighting for solid shapes (spheres, boxes, thick lines): flat-shaded faces lit from above (built in,
  // tools/shaders/lit_vs.hlsl and lit_ps.hlsl). Not the scene's lighting: no shadows, sun direction or AO.
  public static func MaterialLit() -> String {
    return "debug_draw\\shaders\\debug_draw\\lit.mt";
  }

  // A world-space grid on solid shapes, a line every metre and a brighter one every 10 m, over the lit shading
  // (built in, tools/shaders/grid.hlsl). Parameters Spacing (metres) and LineColor.
  public static func MaterialGrid() -> String {
    return "debug_draw\\shaders\\debug_draw\\grid.mt";
  }

  // Solids glow at their edges and are nearly clear face-on, with faint lines drifting up (built in,
  // tools/shaders/hologram.hlsl). Use with SetMaterial(channel, DebugDraw.MaterialHologram(), true).
  public static func MaterialHologram() -> String {
    return "debug_draw\\shaders\\debug_draw\\hologram.mt";
  }

  // Shapes fade out with distance: drawn fully up to 15 m, gone at 60 m; nearer is true for 5 m to 25 m (built in,
  // tools/shaders/fade.hlsl; parameters Near and Far). Use with SetMaterial(channel, DebugDraw.MaterialFade(), true).
  public static func MaterialFade(opt nearer: Bool) -> String {
    return nearer ? "debug_draw\\shaders\\debug_draw\\fade_fade_near.mi"
                  : "debug_draw\\shaders\\debug_draw\\fade.mt";
  }

  // ---- Filled shapes. All are two-sided; a colour with alpha below 255 makes them translucent.

  // Filled triangles from corner triples (a, b, c, a, b, c, ...); extra points at the end are ignored.
  public static func Triangles(points: array<Vector4>, color: Color, opt seconds: Float, opt overlay: Bool, opt channel: CName) {
    DebugDraw_Triangles(points, color, seconds, overlay, channel, 0.0);
  }

  // A filled convex polygon through points in order (a fan from the first point).
  public static func Polygon(points: array<Vector4>, color: Color, opt seconds: Float, opt overlay: Bool, opt channel: CName) {
    let triangles: array<Vector4>;
    let i = 2;
    while i < ArraySize(points) {
      ArrayPush(triangles, points[0]);
      ArrayPush(triangles, points[i - 1]);
      ArrayPush(triangles, points[i]);
      i += 1;
    }
    DebugDraw_Triangles(triangles, color, seconds, overlay, channel, 0.0);
  }

  // A filled circle facing normal; a zero normal means up (a horizontal disc).
  public static func Disc(center: Vector4, normal: Vector4, radius: Float, color: Color, opt seconds: Float, opt overlay: Bool, opt channel: CName) {
    DebugDraw_Disc(center, normal, radius, color, seconds, overlay, channel, 0.0);
  }

  // A filled box centred on transform's position and turned with it; halfExtents along its X, Y and Z.
  public static func SolidOrientedBox(transform: Transform, halfExtents: Vector4, color: Color, opt seconds: Float, opt overlay: Bool, opt channel: CName) {
    DebugDraw_SolidOrientedBox(transform, halfExtents, color, seconds, overlay, channel, 0.0);
  }

  // A filled cone opening from apex along direction, length long; angle is the half-angle in degrees.
  public static func SolidCone(apex: Vector4, direction: Vector4, length: Float, angle: Float, color: Color, opt seconds: Float, opt overlay: Bool, opt channel: CName) {
    DebugDraw_SolidCone(apex, direction, length, angle, color, seconds, overlay, channel, 0.0);
  }

  // A filled upright capsule centred on center; height includes the end caps.
  public static func SolidCapsule(center: Vector4, radius: Float, height: Float, color: Color, opt seconds: Float, opt overlay: Bool, opt channel: CName) {
    DebugDraw_SolidCapsule(center, radius, height, color, seconds, overlay, channel, 0.0);
  }

  // ---- Local spaces.

  // Shapes queued until the matching PopTransform are in transform's space: positions and directions are moved by
  // it (boxes become turned boxes; capsules stay upright; text keeps facing the screen). Pushes nest. Pop before the
  // end of the frame: anything left pushed is dropped then, with a warning in the log.
  public static func PushTransform(transform: Transform) {
    DebugDraw_PushTransform(transform);
  }

  // Shapes queued until the matching PopTransform are in entity's space and follow it: timed ones keep moving with
  // it and go away with it. A PushTransform inside it is relative to the entity.
  public static func PushEntity(entity: ref<Entity>) {
    DebugDraw_PushEntity(entity);
  }

  public static func PopTransform() {
    DebugDraw_PopTransform();
  }

  // X-ray for a channel: its depth-tested shapes are drawn once more on top of everything, faint (alpha times
  // `alpha`, 0.3 when left out), so the parts behind walls still show. Off again with enabled false.
  public static func SetXRay(channel: CName, enabled: Bool, opt alpha: Float) {
    DebugDraw_SetXRay(channel, enabled ? (alpha > 0.0 ? alpha : 0.3) : 0.0);
  }

  // Drops the shapes queued with this channel, including ones with time left. No channel: the shapes queued
  // without one.
  public static func Clear(opt channel: CName) {
    DebugDraw_Clear(channel);
  }

  // Drops every shape from every channel, and every plot.
  public static func ClearAll() {
    DebugDraw_ClearAll();
  }

  // a = 0 (or left out) is opaque.
  // A colour for value on a blue, cyan, green, yellow, red scale from low to high (clamped), for costs, speeds,
  // heights and the like. Any shape and material takes it.
  public static func Heat(value: Float, low: Float, high: Float, opt a: Int32) -> Color {
    let t = high > low ? ClampF((value - low) / (high - low), 0.0, 1.0) * 4.0 : 0.0;
    let reds: array<Float> = [40.0, 0.0, 60.0, 255.0, 255.0];
    let greens: array<Float> = [70.0, 210.0, 255.0, 225.0, 50.0];
    let blues: array<Float> = [255.0, 255.0, 90.0, 40.0, 40.0];
    let i = Min(FloorF(t), 3);
    let f = t - Cast<Float>(i);
    return DebugDraw.Rgba(RoundF(LerpF(f, reds[i], reds[i + 1])), RoundF(LerpF(f, greens[i], greens[i + 1])),
                          RoundF(LerpF(f, blues[i], blues[i + 1])), a);
  }

  public static func Rgba(r: Int32, g: Int32, b: Int32, opt a: Int32) -> Color {
    let color: Color;
    color.Red = Cast<Uint8>(r);
    color.Green = Cast<Uint8>(g);
    color.Blue = Cast<Uint8>(b);
    color.Alpha = Cast<Uint8>(a == 0 ? 255 : a);
    return color;
  }
}
