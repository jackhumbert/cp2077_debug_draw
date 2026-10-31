# World overlays: collision, navmesh, triggers

Three wireframe overlays of data the engine keeps at runtime, each read around a centre (normally V) within a
radius. Natives in `src/red4ext/Overlays*.cpp` return line point pairs; `src/redscript/debug_draw/Overlays.reds`
(`DebugDrawOverlays.Show*`) draws them with `DebugDraw.Lines`. `Main.cpp` loads them and registers their natives
next to the others (the script and the natives go together: a packed script declaring natives nobody registers hangs
the game at startup). Reversed on game 3.0.88; every address goes
through the Adler-32 hash table (`cyberpunk2077_addresses.json`), struct offsets are listed in each source file.

```swift
// every frame, around V
let at = GetPlayer(game).GetWorldPosition();
DebugDrawOverlays.ShowNavmesh(at, 30.0);
DebugDrawOverlays.ShowTriggers(at, 40.0);
DebugDrawOverlays.ShowCollision(at, 15.0);
```

| Native | Returns |
|---|---|
| `DebugDraw_NavmeshEdges(center, radius, agent, boundary)` | navmesh triangle edges (agent 0 human, 1 vehicle); `boundary` true: the outer edges only, false: the inner ones |
| `DebugDraw_TriggerEdges(center, radius, channels)` | edges of every trigger convex near the centre or containing it, filtered by `TriggerChannel` mask (0: all) |
| `DebugDraw_CollisionEdges(center, radius, actors)` | PhysX shape wireframes (actors 0 all, 1 static, 2 dynamic) |
| `DebugDraw_OverlayStats()` | one line per overlay: what its last build found |

Results are cached in the plugin per (overlay, parameters) and rebuilt when the centre moves by 1 m or 5 % of the
radius, or after 1 s. Collision of moving bodies only (`actors` 2) is rebuilt every frame (a 5 ms cache only shares
one build between the frame's two BeginFrame calls), and collision of both kinds after 0.1 s. Calling the helpers
every frame costs one array copy per call. A build that faults (an offset moved after a patch) is caught with SEH,
logged with the stage it was in, and turns that overlay off until the game restarts. `Stats()` gives each build's
time.

Moving bodies every frame stay cheap enough because each shape's wireframe is kept in its own space, keyed by the
shape pointer from this frame's query plus a hash of its geometry (so a reused address with other geometry is not
mistaken for it), and its placed lines are reused while its pose is unchanged: most PhysX "dynamic" bodies near V are
parked props. Collision queries run on one long-lived thread with a 16 MB stack.

The settings page's "World overlays" are drawn by the plugin itself (`DebugDraw_SetWorldOverlays`): each drawer
build looks up the entity's position and keeps one list of line shapes per overlay source, remade only when that
source's build changes. Measured at the Heywood market with about 1,100 moving shapes within 20 m (2026-10-31): the
PhysX query 0.6 to 0.8 ms, the moving shapes 1.7 to 2.1 ms, the whole per-frame overlay work 3.7 to 4 ms (6 to 8.7 ms
before the caching).

## Where the data lives

All three start from the `world::RuntimeScene` (`CGameEngine +0x348` framework, `+0x18` runtime scene), which
begins with one `Handle<IRuntimeSystem>` per system slot in the RED4ext SDK's order (the engine's own getters read
navigation at `+0x20`, slot 2, and traffic at `+0x170`, slot 23). The plugin checks the RTTI type name of the slot
before using it.

### Navmesh: Recast/Detour tiles (works)

- `world::RuntimeSystemNavigation` (slot 2): `+0x48` `red::old::RWSpinLock` that every query takes shared,
  `+0x50` `world::Navmesh` = `dtNavMesh*[2]` (`NavGen::AgentSize` Human, Vehicle), then `dtTileCache*[2]`, then the
  per-size `SafeQueries` (`world::Navmesh::Initialize`, `GetQuickQuery`).
- Detour is Y up; game `(x, y, z)` is Recast `(x, z, y)` (`FindNavmeshPointInSphere` swaps them).
- CDPR's Detour: 64-bit poly refs, `DT_VERTS_PER_POLYGON` 3 (triangles), 20-byte `dtPoly`
  (`+0x04 verts[3]`, `+0x0A neis[3]`, `+0x12 vertCount`, `+0x13 area | type << 6`), 0x68-byte `dtMeshTile`, the stock
  `dtMeshHeader`. Offsets from `dtNavMesh::queryPolygonsInTile`, `connectIntLinks`, `getTileAndPolyByRef`,
  `calcTileLoc`, `getTilesAt`.
- Read: tiles overlapping the circle via the engine's own `dtNavMesh::getTilesAt` (hash 3827700168) under the
  shared lock, then every triangle with a corner within the radius. `neis == 0` is a wall or drop (boundary).
- Script-visible alternatives: `AINavigationSystem` / `NavigationSystem` only answer point and path queries
  (`FindPointInSphere`, `IsPointOnNavmesh`); nothing returns polygons. `RuntimeSystemNavigation::FindPolygonsInBoundingBox`
  exists natively but is not script-visible.
- Cost: 9 tiles, about 1000 triangles, 474 outer and 1374 inner edges at 30 m (Kabuki street, the test save); not
  timed, but a rebuild only walks those tiles.
- Limits: only streamed-in tiles exist (a few hundred metres round V); off-mesh links (jumps, ladders) are not
  drawn; the tile cache's obstacles (doors, cars) are already carved into the tiles.

### Triggers: TriggerManagerImpl (works)

- `world::RuntimeSystemTriggers` (slot 9) `+0x48` `TriggerManagerImpl*` (its `OnRenderDebug` reads it there and
  only draws bounding boxes). The manager: `+0x20` objects (`SharedStorage<TriggerObject>`, 16 bytes), `+0x2C`
  count, `+0x564` `RWSpinLock` (`GetTriggersAtPoint` takes it).
- `TriggerObject` (ctor, `CollectTriggersDebugInfo`): `+0x00/0x10/0x20` rotation rows, `+0xA8`
  `DynArray<UniquePtr<TriggerConvex>>` (`+0xB4` count), `+0xBC` channel mask (OR of its notifiers' channels),
  `+0xA0` owner (for a world trigger area node, the node instance `+208`).
- `TriggerConvex` `+0x08` `AreaConvex*` (local space), `+0x20` int position * 256 (`TestOverlap`,
  `GetWorldBoundingBox`). `AreaConvex` (`UpdateEdgesData`, `GetLocalBoundingBox`): `+0x78` half-edges (48 bytes,
  `+0x20` vertex, `+0x24` next), `+0x84` count, `+0x88` vertices (`Vector3`), `+0x94` count, `+0x98` face planes.
- Read: every object, every convex, every half-edge once (`a < b`); a trigger is drawn whole when any edge comes
  within the radius or the centre is inside its box. Cost: 5819 objects in the streamed city, 48 drawn at 20 m with
  2691 edges; not timed (a rebuild walks every object's first edges).
- Covers world trigger area nodes (quest, location, interior, ambient, environment areas), trigger components and
  anything else that registers with the trigger system, but not what the trigger is for (the owner's type is not
  read yet); `ShowTriggers` colours by channel: quest yellow, player green, sound/environment blue, the rest magenta.
- Script-visible: `TriggerComponent` / area entities can be found per entity, but there is no script listing of
  trigger nodes; the node data (`worldTriggerAreaNode` outlines) is only in the streaming sectors.

### Collision: PhysX 3.4 scene query

- The game links PhysX 3.4 (`PhysX3_x64.dll`). `physics::SyncOverlapN` shows the path:
  `world::GetSceneID(RuntimeScene)` (hash 2913539499, reads `RuntimeSystemPhysics +88`), then
  `physics::g_sceneLookup` (hash 3885377886) `->GetScene(id)` (hash 926881634) = `physics::PhysXScene`, whose
  `+0x18` is the `PxScene*` (`PhysXScene::Initialize` stores `PxPhysics::createScene` there) and `+0x134` the floating
  origin (PhysX positions are world minus it; `helper::ConvertHit` adds it back).
- `PxScene::overlap` is vtable `+0x300` (as `SyncOverlapN` calls it); the hit callback is the stock
  `PxHitCallback<PxOverlapHit>` with 32-byte hits (touches at `+0x30`, count at `+0x3C`).
- PhysX virtuals checked against the game's own calls: `PxShape::getGeometry +0x48` and `getLocalPose +0x98`
  (`PxRigidBodyExt::computeMassPropertiesFromShapes`, `PxShapeExt`), `PxRigidActor::getGlobalPose +0xA0`,
  `PxTriangleMesh` `getVertices +0x30`, `getNbTriangles +0x48`, `getTriangles +0x50` (cloth
  `AddTrianglesToContext`), `PxConvexMesh` `getNbVertices +0x28`, `getVertices +0x30` (RepX serializer). The rest
  follow the PhysX 3.4 headers' order.
- Script-visible alternatives: `SpatialQueriesSystem.Overlap` / `SyncRaycastByQueryFilter` give hit points, not
  shapes; collider components (`entColliderComponent`) describe only entity colliders, not streamed world collision.
  Offline, LTBF's `tools/navmap_bake/sectors.py` reads `worldCollisionNode` cooked meshes from the sectors.

- Cost (not timed yet): one overlap and the shape walk per rebuild; at 20 m about 500 shapes (230 boxes, 140 convexes, 110
  triangle meshes with 5700 triangles, 30 capsules) and 21000 lines; at 30 m 900 hits and 24000 lines. Lines are
  capped at 300000 per call. Static results are kept 1 s, dynamic ones 0.1 s.
- Limits: height fields are counted but not drawn (none in the city around the test spot, the terrain outside
  the city may use them); planes are not drawn; only what is streamed into the PhysX scene exists (the same
  bubble as the navmesh); triangle mesh edges shared by two triangles are drawn twice.

### Two traps found on the way

- `PxTriangleMesh::getTriangleMeshFlags` (+0x58) returns `PxFlags` through a hidden pointer (the game's cloth
  code passes one and tests `byte [rax] & 2`). Called as if it returned a byte, it wrote through a garbage
  register and corrupted the game; the next frames crashed with "write to an inaccessible address".
- `PxScene::overlap` needs more stack than a script native has left when called from the bridge (and from some
  game jobs): it faulted inside PhysX at random after a few calls, and catching that left the game hung. The
  collision build now runs on its own thread with a 16 MB stack while the caller waits; 1900 frames of all three
  overlays ran clean after that.

Every build runs under SEH: a fault (an offset moved after a patch) is logged with the exception code, the stage
and the object (`[overlays] faulted ... (exception 0x..., at <stage>, object <ptr>)`) and turns that overlay off.

## Testing

The overlays are checked in game: probe each native, draw it around V and compare with the scene. A game
started with scripts that declare the overlay natives, but without a build that registers them, hangs at startup.

## Next steps

- Name what each trigger is for: `TriggerObject +0xA0` points into the owner (a `TriggerAreaNodeInstance` at
  `-208` for world areas); reading its notifiers' types would allow labels (quest, location, interior...).
- Height fields: `PxHeightFieldGeometry` (+8 `PxHeightField*`, +16 height scale, +20 row, +24 column scale) and
  the `PxHeightField` sample getters, whose vtable slots still need checking against a game call.
- Off-mesh links (jumps, ladders, doors) of the navmesh: `dtOffMeshConnection` array of the tile.
