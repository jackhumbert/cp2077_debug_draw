# Debug Draw

A RED4ext plugin that turns the game's own debug renderer back on and exposes it to redscript, so mods can draw
lines, boxes, spheres and text in the world:

```swift
DebugDraw.Line(a, b, DebugDraw.Rgba(255, 64, 0), 5.0);
DebugDraw.Box(min, max, DebugDraw.Rgba(0, 255, 0));
DebugDraw.Sphere(center, 2.0, DebugDraw.Rgba(0, 128, 255));
DebugDraw.Text(position, "hello", DebugDraw.Rgba(255, 255, 255), 1.0);
```

## API

Every shape ends with `color`, then optional `seconds`, `overlay` and `channel`. `Axes` has no color, and `Text` /
`ScreenText` take `seconds`, `channel` and then `scale`.

| Function | Draws |
|---|---|
| `Line(a, b, color, ...)` | a line from `a` to `b` |
| `Lines(points, color, ...)` | many lines in one call, from point pairs `a, b, a, b, ...` (an odd last point is ignored) |
| `Arrow(from, to, color, ...)` | a line with a head at `to` (the head is the last 20 % of the length) |
| `Box(min, max, color, ...)` | a wireframe axis-aligned box between two corners |
| `SolidBox(min, max, color, ...)` | a filled axis-aligned box |
| `OrientedBox(transform, halfExtents, color, ...)` | a wireframe box centred on and rotated with `transform` |
| `Sphere(center, radius, color, ...)` | a wireframe sphere |
| `SolidSphere(center, radius, color, ...)` | a filled sphere |
| `Circle(center, normal, radius, color, ...)` | a circle facing `normal` (a zero normal is a horizontal circle) |
| `Capsule(center, radius, height, color, ...)` | an upright capsule; `height` includes the end caps |
| `Cylinder(a, b, radius, color, ...)` | a wireframe cylinder from `a` to `b` |
| `Cone(apex, direction, length, angle, color, ...)` | a cone opening from `apex`; `angle` is the half-angle in degrees |
| `Axes(transform, length, ...)` | a transform's X, Y and Z axes in red, green and blue |
| `Text(position, text, color, seconds, channel, scale)` | a label at a world position |
| `ScreenText(x, y, text, color, seconds, channel, scale)` | a label at screen pixels from the top left |
| `Path(points, color, ...)` | connected lines through `points` in order |
| `Triangles(points, color, ...)` | filled triangles from corner triples |
| `Polygon(points, color, ...)` | a filled convex polygon through `points` |
| `Disc(center, normal, radius, color, ...)` | a filled circle facing `normal` |
| `SolidOrientedBox(transform, halfExtents, color, ...)` | a filled box turned with `transform` |
| `SolidCone(apex, direction, length, angle, color, ...)` | a filled cone |
| `SolidCapsule(center, radius, height, color, ...)` | a filled upright capsule |
| `PushTransform(transform)` / `PopTransform()` | shapes in between are in `transform`'s space (nests) |
| `PushEntity(entity)` / `PopTransform()` | shapes in between are in the entity's space and follow it, timed ones too |
| `SetXRay(channel, enabled, alpha)` | the channel's depth-tested shapes also show faintly through walls |
| `Raycast(from, to, hit, hitPosition, ...)` | a ray: green to the hit with a marker and grey beyond, or red when it missed |
| `Trace(from, to, result, ...)` | `Raycast` for a `TraceResult`, plus the hit normal as an arrow |
| `Bounds(entity, color, ...)` | the box around an entity's skinned meshes (characters, vehicles), turned with it; false when it has none. It covers every skinned part, so it can be taller than the body |
| `Velocity(entity, ...)` | an arrow to where a vehicle or character will be in 0.5 s, with its speed |
| `Label(entity, text, color, seconds, channel, scale)` | a label just above an entity |
| `Plot(name, value, color)` | a rolling on-screen graph, one sample per call; disappears 2 s after the last |
| `GetLocalBounds(entity)`, `HasBounds(box)`, `Center(entity)`, `ToWorld(entity, local)` | the values the entity helpers use |
| `Clear(channel)` | removes that channel's shapes (no channel: the shapes drawn without one) |
| `Dim(amount)` | darkens the scene by `amount` (0 to 1) for one frame so the debug shapes stand out; call it every frame |
| `SetMaterial(channel, path, translucent)` | draws a channel's shapes with another material (see Materials and custom shaders) |
| `SetMaterialFloat` / `SetMaterialColor` / `SetMaterialVector(channel, name, value)` | sets one of the channel's material parameters |
| `ReloadShaders()` | reads the shader manifests and files again and rebuilds the channels' materials |
| `ClearPlot(name)` | removes a plot |
| `ClearAll()` | removes every shape and plot |
| `Rgba(r, g, b, a)` | a `Color`; leaving out `a` (or 0) is opaque |
| `Heat(value, low, high, a)` | a `Color` on a blue, cyan, green, yellow, red scale for `value` between `low` and `high` |

- **seconds**: 0 (the default) draws for one frame. Call it every frame to keep a shape on screen; it disappears
  within 0.1 s of the last call. A positive value keeps the shape that many real-time seconds (it keeps counting
  while the game is paused).
- **overlay**: shapes are depth tested against the scene unless `overlay` is true, which draws them through walls.
  Text is always drawn on top.
- **alpha**: a color with alpha below 255 is drawn translucent, which is mostly useful for the solid shapes.
- **channel**: a `CName` that groups shapes so `Clear(n"my_mod")` removes only yours. Use your own channel if you
  call `Clear`, so you do not wipe out another mod's shapes.
- **scale**: text size, 1 when left out.
- **labels**: `Text`, `ScreenText` and `Label` get a dark outline. `Text` and `Label` take a last `maxDistance`
  (metres): beyond it from the camera they are skipped, checked when queued, so it suits labels drawn every frame.
- **filled shapes** are two-sided; a colour with alpha below 255 makes them translucent.
- **PushTransform / PushEntity**: pop in the same frame. Anything left pushed at the end of a frame is dropped, with
  a warning in the log, so a missing pop cannot move another mod's shapes. Boxes given by two corners become turned
  boxes under a rotation; capsules stay upright; text keeps facing the screen.
- **thickness**: the line-built shapes (`Line`, `Lines`, `Path`, `Arrow`, `Box`, `OrientedBox`, `Circle`, `Cone`,
  `Bounds`) take an optional last `thickness` in metres, drawn as a thin prism of that radius (0.02 to 0.05 reads
  well at street distances). The other shapes are drawn by the engine with hairlines.

At most 1,000,000 timed shapes can be queued at once; past that new shapes are dropped and a warning is logged.

## Cookbook

Calls with `seconds` left out draw for one frame, so these go in something that runs every frame (an update
callback, a `Tick`). Give each mod its own channel.

**Vehicle handling: see the forces, graph the tuning**
```swift
DebugDraw.Velocity(vehicle, 0.0, false, n"my_mod");
DebugDraw.Arrow(centerOfMass, centerOfMass + thrust * 0.01, DebugDraw.Rgba(80, 160, 255), 0.0, false, n"my_mod");
DebugDraw.Plot("yaw rate", yawRate);
DebugDraw.Plot("steer input", steer, DebugDraw.Rgba(255, 170, 60));
```

**AI: what each NPC sees and is doing**
```swift
let head = npc.GetWorldPosition() + new Vector4(0.0, 0.0, 1.65, 0.0);
DebugDraw.Cone(head, npc.GetWorldForward(), 15.0, 30.0, DebugDraw.Rgba(255, 80, 200, 140), 0.0, false, n"my_mod");
DebugDraw.Line(head, target.GetWorldPosition(), DebugDraw.Rgba(255, 60, 60), 0.0, false, n"my_mod");
DebugDraw.Label(npc, "searching", DebugDraw.Rgba(255, 255, 255), 0.0, n"my_mod");
```

**Navigation: the route and its waypoints**
```swift
DebugDraw.Path(route, DebugDraw.Rgba(60, 200, 255), 0.0, false, n"my_mod");
for point in waypoints {
  DebugDraw.Circle(point, new Vector4(0.0, 0.0, 0.0, 0.0), 1.5, DebugDraw.Rgba(255, 230, 0), 0.0, false, n"my_mod");
}
```

**Shapes that follow something: queue once, they stay on it**
```swift
DebugDraw.PushEntity(vehicle);
DebugDraw.Box(new Vector4(-1.1, -2.5, 0.0, 1.0), new Vector4(1.1, 2.5, 1.5, 1.0), DebugDraw.Rgba(80, 255, 120), 30.0, false, n"my_mod", 0.03);
DebugDraw.Text(new Vector4(0.0, 0.0, 2.3, 1.0), "tracked", DebugDraw.Rgba(255, 255, 255), 30.0, n"my_mod");
DebugDraw.PopTransform();
```

**Seeing through walls**
```swift
DebugDraw.SetXRay(n"my_mod", true);   // once; then draw as usual in n"my_mod"
```

**Placing things: bounds, pivots, distances**
```swift
DebugDraw.Bounds(entity, DebugDraw.Rgba(255, 210, 60), 0.0, false, n"my_mod");
let pivot: Transform;
pivot.position = entity.GetWorldPosition();
pivot.orientation = entity.GetWorldOrientation();
DebugDraw.Axes(pivot, 1.0, 0.0, false, n"my_mod");
DebugDraw.Text(midpoint, FloatToStringPrec(distance, 2) + " m", DebugDraw.Rgba(255, 255, 255), 0.0, n"my_mod");
```

**Weapons and raycasts: what the ray hit**
```swift
let result: TraceResult;
let filter: QueryFilter;
QueryFilter.AddGroup(filter, n"Static");
QueryFilter.AddGroup(filter, n"Terrain");
GameInstance.GetSpatialQueriesSystem(game).SyncRaycastByQueryFilter(muzzle, end, filter, result, false, false);
DebugDraw.Trace(muzzle, end, result, 5.0, false, n"my_mod"); // held 5 s, so each shot stays visible
```

**Spawning and streaming: radii and spawn points**
```swift
DebugDraw.Circle(playerPosition, new Vector4(0.0, 0.0, 0.0, 0.0), spawnRadius, DebugDraw.Rgba(80, 255, 120), 0.0, false, n"my_mod");
DebugDraw.Circle(playerPosition, new Vector4(0.0, 0.0, 0.0, 0.0), despawnRadius, DebugDraw.Rgba(255, 80, 80), 0.0, false, n"my_mod");
DebugDraw.Sphere(spawnPoint, 0.5, DebugDraw.Rgba(80, 255, 120), 10.0, false, n"my_mod");
```

## From Cyber Engine Tweaks (Lua)

The same functions work from a CET mod; optional arguments can be left out there too:

```lua
registerForEvent("onUpdate", function()
  local player = Game.GetPlayer()
  if not player then return end
  local p = player:GetWorldPosition()
  DebugDraw.Sphere(Vector4.new(p.x, p.y, p.z + 2.5, 1), 0.5, DebugDraw.Rgba(255, 128, 0, 0), 0.0, false, CName.new("my_mod"))
  DebugDraw.Text(Vector4.new(p.x, p.y, p.z + 3.2, 1), "hello from CET", DebugDraw.Rgba(255, 255, 255, 0))
  DebugDraw.Plot("height", p.z)
end)
```

`DebugDraw.Rgba(r, g, b, 0)` is opaque. Positions read before a save has finished loading can be stale, so draw
per frame (seconds 0) rather than once at startup.

## World overlays: collision, navmesh, triggers

`DebugDrawOverlays` draws what the engine itself knows about the world around a point, as wireframes: the physics
shapes (static in pale green, moving in orange), the navmesh V and NPCs walk on, and trigger volumes.

```swift
// every frame, around V
let at = GetPlayer(game).GetWorldPosition();
DebugDrawOverlays.ShowCollision(at, 15.0);
DebugDrawOverlays.ShowNavmesh(at, 30.0);
DebugDrawOverlays.ShowTriggers(at, 40.0);
```

Results are cached and rebuilt when the centre moves or once a second, so calling them every frame is cheap.
`NavmeshEdges`, `TriggerEdges` and `CollisionEdges` return the line points instead, and `Stats()` says what each found.
How they work and their limits: `docs/overlays.md`.

## Settings

With [Mod Settings](https://www.nexusmods.com/cyberpunk2077/mods/4885) installed, Debug Draw has its own page. Without
it, the defaults apply. They act on every mod's shapes:

- **Show debug drawing**: hides everything every mod draws, without uninstalling anything.
- **Text size** and **Label distance**: multiply every mod's text and label sizes and label distance limits.
- **Dim strength**: how much mods may darken the scene with `Dim`; 0 turns dimming off.
- **World overlays**: collision, navmesh and triggers drawn around V every frame, with a radius and an option to
  draw them through walls. Moving bodies (cars, NPCs) are queried every frame, so their wireframes stay on them.

## Materials and custom shaders

`DebugDraw.SetMaterial(channel, path)` draws a channel's shapes with another material, until it is set back to `""`.
Text keeps the default. With `translucent` true the channel's shapes are drawn in the translucent pass, so the alpha
the material's shader writes counts even for opaque colours; the fade and hologram materials need it.

```swift
DebugDraw.SetMaterial(n"my_mod", DebugDraw.MaterialPulse());
DebugDraw.SolidSphere(center, 1.0, DebugDraw.Rgba(255, 80, 80), 0.0, false, n"my_mod");
```

| Material | Look |
|---|---|
| `""` | the default |
| `DebugDraw.MaterialStripes()` | diagonal stripes (the engine's own) |
| `DebugDraw.MaterialPulse()` | a pulsing glow, with a band sweeping down the screen |
| `DebugDraw.MaterialScanlines()` | CRT-style scanlines |
| `DebugDraw.MaterialLit()` | simple lighting for solid shapes and thick lines: flat-shaded faces lit from above |
| `DebugDraw.MaterialGrid()` | a world-space grid on solids, a line every metre and a brighter one every 10 m, over the lit shading |
| `DebugDraw.MaterialHologram()` | (translucent) edges glow, faces seen head-on are nearly clear, faint lines drift up |
| `DebugDraw.MaterialFade(nearer)` | (translucent) shapes fade out with distance: from 15 m to 60 m, or 5 m to 25 m with `nearer` |

Only materials with shaders for the debug-draw pass can draw shapes. Most of the game's materials load but draw
nothing. The game compiles its shaders ahead of time and looks them up by the material template's name, so Debug Draw
lets a mod bring its own pixel shaders. Write one in HLSL against `tools/shaderc/include/debugdraw.hlsli`, then run
`tools/shaderc` to compile it, check it against the debug-draw interface, make the material, and lay out the files to
ship. See `tools/shaderc/README.md`. Mods put the result in `red4ext/plugins/debug_draw/shaders/<mod>/` and
`archive/pc/mod/`; Debug Draw loads every manifest it finds there at startup.

A custom material is a pixel shader, optionally with its own vertex shader. The pixel shader can read the vertex
colour and uv, the frame time, the screen size, the camera position and the material's own parameters, which a
`.mi` or a script can set. A vertex shader can pass more on, such as the world position that `MaterialLit` lights with, or move
vertices. Debug shapes are drawn after the game's lighting, so they never receive its shadows, AO or lights; lit
looks are computed in the shader. How this works: `docs/shader_research.md`.

Parameters from script: after `SetMaterial`, `SetMaterialFloat(channel, n"Spacing", 0.5)` (or `SetMaterialColor`,
`SetMaterialVector`) sets a parameter by its name in the `.mt`; the type must match (`Float` for a scalar, `Color` for
a colour). The channel gets its own copy of the material, so other channels using it are not changed. Each change
rebuilds that copy's render data, which suits tweaks and gameplay values; to animate every frame, use the time in
the shader. A wrong name or type is logged once.

While working on a shader, `DebugDraw.ReloadShaders()` (from redscript, or from the CET console) reads the
manifests and compiled shaders again, so a shader rebuilt with `tools/shaderc` and copied into the game shows without
a restart. A new material still needs its `.mt` in an archive, which the game loads at startup.

## Requirements

- Cyberpunk 2077 2.5 (executable 3.0.88). Engine addresses are found by hash, so other versions may work. If one does not resolve,
  the log says so and the plugin does nothing: scripts that call `DebugDraw` still compile and run, and draw
  nothing.
- RED4ext 1.31.0+
- Redscript 0.5.32+

## Troubleshooting

The plugin logs to `red4ext/logs/debug_draw*.log`. A working install logs "Added the debug render nodes to the main
render graph" when a save loads, and "Drawing: first debug shapes collected" the first time something is drawn.

## Building from source

`cmake -B build` and `cmake --build build` under a Visual Studio x64 developer prompt build the plugin into
`game_dir/`. The unit tests cover the parts that need no game (shape transforms and maths, material name hashing,
shader manifests): `ctest --test-dir build`, or `build/debug_draw_tests.exe` for each test's result. Everything
that talks to the engine is tested in game.
