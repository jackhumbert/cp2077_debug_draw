// World overlays: the engine's own collision, AI navmesh and trigger volumes around a point, as wireframes.
//
// Each *Edges native returns line point pairs (a, b, a, b, ...) for DebugDraw.Lines. Results are cached in the
// plugin and rebuilt when the centre moves (by 1 m or 5 % of the radius) or every second (every 0.1 s for collision
// that includes moving bodies), so calling a Show* helper every frame is cheap.
//
// navmesh agent: 0 human (the default), 1 vehicle. boundary true returns the navmesh's outer edges (walls, drops),
// false its inner triangle edges.
// trigger channels: a TriggerChannel bit mask (1 Default, 2 Player, 4 Camera, 8 Human, 16 SoundReverbArea,
// 32 SoundAmbientArea, 64 Quest, 128 Projectiles, 256 Vehicle, 512 Environment, 1024 WaterNullArea); 0 is every
// trigger.
// collision actors: 0 every body, 1 static only, 2 dynamic only.

native func DebugDraw_NavmeshEdges(center: Vector4, radius: Float, agent: Int32, boundary: Bool) -> array<Vector4>
native func DebugDraw_TriggerEdges(center: Vector4, radius: Float, channels: Int32) -> array<Vector4>
native func DebugDraw_CollisionEdges(center: Vector4, radius: Float, actors: Int32) -> array<Vector4>
native func DebugDraw_OverlayStats() -> String

public abstract class DebugDrawOverlays {
  public static func NavmeshEdges(center: Vector4, radius: Float, opt agent: Int32, opt boundary: Bool) -> array<Vector4> {
    return DebugDraw_NavmeshEdges(center, radius, agent, boundary);
  }

  public static func TriggerEdges(center: Vector4, radius: Float, opt channels: Int32) -> array<Vector4> {
    return DebugDraw_TriggerEdges(center, radius, channels);
  }

  public static func CollisionEdges(center: Vector4, radius: Float, opt actors: Int32) -> array<Vector4> {
    return DebugDraw_CollisionEdges(center, radius, actors);
  }

  // One line per overlay: what its last build found (tiles, triggers, shapes, line counts).
  public static func Stats() -> String {
    return DebugDraw_OverlayStats();
  }

  // The AI navmesh within radius of center: inner triangle edges in dim cyan, outer edges in bright cyan.
  public static func ShowNavmesh(center: Vector4, radius: Float, opt agent: Int32, opt seconds: Float, opt overlay: Bool, opt channel: CName) {
    DebugDraw.Lines(DebugDraw_NavmeshEdges(center, radius, agent, false), DebugDraw.Rgba(0, 150, 170), seconds, overlay, channel);
    DebugDraw.Lines(DebugDraw_NavmeshEdges(center, radius, agent, true), DebugDraw.Rgba(60, 255, 255), seconds, overlay, channel, 0.02);
  }

  // Trigger volumes touching the sphere (or containing center): quest areas in yellow, the player's other triggers
  // in green, sound and environment areas in blue, the rest in magenta. channels left out (0) draws every group;
  // otherwise only triggers on those channels, in color.
  public static func ShowTriggers(center: Vector4, radius: Float, opt channels: Int32, opt color: Color, opt seconds: Float, opt overlay: Bool, opt channel: CName) {
    if channels != 0 {
      let c = Cast<Int32>(color.Red) + Cast<Int32>(color.Green) + Cast<Int32>(color.Blue) + Cast<Int32>(color.Alpha) == 0 ? DebugDraw.Rgba(255, 220, 40) : color;
      DebugDraw.Lines(DebugDraw_TriggerEdges(center, radius, channels), c, seconds, overlay, channel, 0.015);
      return;
    }
    // A trigger on several channels is drawn once per group it is in, the last group on top.
    DebugDraw.Lines(DebugDraw_TriggerEdges(center, radius, 0), DebugDraw.Rgba(230, 60, 230), seconds, overlay, channel, 0.015);
    DebugDraw.Lines(DebugDraw_TriggerEdges(center, radius, 16 | 32 | 512 | 1024), DebugDraw.Rgba(70, 120, 255), seconds, overlay, channel, 0.015);
    DebugDraw.Lines(DebugDraw_TriggerEdges(center, radius, 2), DebugDraw.Rgba(60, 230, 90), seconds, overlay, channel, 0.015);
    DebugDraw.Lines(DebugDraw_TriggerEdges(center, radius, 64), DebugDraw.Rgba(255, 220, 40), seconds, overlay, channel, 0.015);
  }

  // Collision shapes within radius of center: static world collision in pale green, moving bodies in orange.
  public static func ShowCollision(center: Vector4, radius: Float, opt seconds: Float, opt overlay: Bool, opt channel: CName) {
    DebugDraw.Lines(DebugDraw_CollisionEdges(center, radius, 1), DebugDraw.Rgba(150, 255, 150), seconds, overlay, channel);
    DebugDraw.Lines(DebugDraw_CollisionEdges(center, radius, 2), DebugDraw.Rgba(255, 150, 40), seconds, overlay, channel);
  }
}
