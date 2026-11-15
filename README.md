# RammsWorldGen

Seed-driven generation and randomization of pedestrian / wheelchair
environments for RAMMS, built on UE 5.8 PCG: street grids with sidewalks,
curb ramps and crosswalks; building shells with level, stepped and ramped
entrances; street furniture and the hazards that narrow a route; suburban
lots, lawns and parks; and indoor layouts with doors, thresholds and
furniture. Every world comes with a **ground-truth manifest** that says where
the routes are, how wide they really are, and which features break the
accessibility profile — so an episode knows which curb, door or A-frame is
the hard one.


> **Repository layout.** This plugin is developed in
> [rammp-org/ramms-worldgen](https://github.com/rammp-org/ramms-worldgen) and
> used by [ramms-sim](https://github.com/rammp-org/ramms-sim) as a submodule at
> `Plugins/RammsWorldGen`. The plugin carries its content (`/RammsWorldGen/`:
> graphs, presets, ADA profile, palette and materials, all rebuilt from
> `Content/Python/ramms_worldgen`). The palette and materials point at the
> project's Fab / marketplace packs, which are not part of this repo; without
> them palette slots stay empty and materials use default textures. Paths
> below like `Scripts/worldgen/`, `doc/world_generation.md` and
> `Content/Maps/Gen/` belong to ramms-sim (build / verify scripts and the
> example levels).

```
preset (params + palette + profile + graph)  ──►  ARammsWorldGenerator (seed)
                                                        │  PCG graph
          Street Network ─► Subdivide Lots ─► Building Massing
                 │                │
                 │                └─► Park Layout ─┐
                 └─► Hazard Placer ◄───────────────┘
                          │
   Primitives (ISM cubes + materials)   Props (Fab meshes)   Pieces (ARammsProcPiece actors)
                          └──────────────── Manifest ──────────────────┘
                                                        │
                                              FRammsWorldManifest (JSON)
```

The five example levels (`Content/Maps/Gen/Map_Gen_*`, built by `Scripts/worldgen/build_levels.py`):

| Downtown (seed 7) | Suburban (seed 11) | Park (seed 3) |
|---|---|---|
| ![](Docs/images/Map_Gen_Downtown.jpg) | ![](Docs/images/Map_Gen_Suburban.jpg) | ![](Docs/images/Map_Gen_Park.jpg) |

| Indoor home (seed 5) | Office (seed 2) |
|---|---|
| ![](Docs/images/Map_Gen_Indoor.jpg) | ![](Docs/images/Map_Gen_Office.jpg) |

## Contents

- [How it fits together](#how-it-fits-together)
- [Building](#building)
- [First run](#first-run)
- [Using it](#using-it)
  - [In the editor](#in-the-editor)
  - [From Python](#from-python)
  - [From an AI assistant (MCP)](#from-an-ai-assistant-mcp)
  - [At runtime](#at-runtime)
- [The manifest](#the-manifest)
- [Segmentation labels](#segmentation-labels)
- [Collision shapes](#collision-shapes)
- [Parameters](#parameters)
- [Graded streets and rough ground](#graded-streets-and-rough-ground)
- [Packs and palette recipes](#packs-and-palette-recipes)
- [Extending](#extending)
- [UrbanVerse assets](#urbanverse-assets)
- [SimReady assets](#simready-assets)
  - [Articulated SimReady assets in MuJoCo](#articulated-simready-assets-in-mujoco)
  - [Articulated SimReady assets in generated levels](#articulated-simready-assets-in-generated-levels)
  - [Hinged doors and ceilings](#hinged-doors-and-ceilings)
- [Traffic (RammsCrowd)](#traffic-rammscrowd)
- [Design notes and limits](#design-notes-and-limits)
- [First-compile checklist](#first-compile-checklist)

## How it fits together

| Layer | What | Where |
|---|---|---|
| **Data assets** | `URammsWorldPreset` (scene kind, params, palette, profile, graph), `URammsWorldPalette` (semantic slots → Fab meshes / materials / actor classes), `URammsAccessibilityProfile` (ADA thresholds + the ranges used to break them on purpose) | `Source/.../Public/RammsWorld*.h`, instances under `/RammsWorldGen/` |
| **Generator** | `ARammsWorldGenerator`: owns the `UPCGComponent`, a seed, a preset; flattens preset + profile + per-instance overrides into `FRammsWorldGenParams`; `Regenerate()` / `Randomize()` / `Cleanup()`; builds the manifest after generation; `PlacePlayerStart()`; `ExportManifest()` | `RammsWorldGenerator.h` |
| **Procedural pieces** | `ARammsProcPiece`: a GeometryScript dynamic mesh whose dimensions decide usability — curb ramps with flares and detectable-warning strips, curbs, steps, ramps with landings and edge protection, walls with door openings, door leaves and thresholds, bollards, planters, low-poly trees and shrubs, and on graded ground `GroundPrisms` (convex prisms: roads, sidewalk panels, potholes, covers, pavers) and `Hump` (speed humps and tables). Spawned by PCG's Spawn Actor with every property mapped from a same-named point attribute. Complex-as-simple collision; `AppendCollisionShapes` describes it as exact boxes, convex hulls and height grids. | `RammsProcPiece.h`, `RammsCollisionShapes.h` |
| **Ground model** | `FRammsGroundModel`: the street world's elevation (street grades, crown, curb heights, cross slopes) as one function every node evaluates, handed downstream as the `Ground` point attribute | `Public/PCG/RammsGround.h` |
| **PCG nodes (C++)** | `Ramms: Street Network`, `Subdivide Lots`, `Building Massing`, `Park Layout`, `Hazard Placer`, `Indoor Layout`, `Manifest`. Deterministic by seed. They emit *oriented points with attributes*; stock spawners do the placing. | `Public/PCG/*.h` |
| **Graphs** | `PCG_Streets` (downtown, suburban, park, main street) and `PCG_Indoor` (home, office, store), both ending in the shared subgraph `PCG_SpawnAndRecord`; **built from code** by `ramms_worldgen.graphs` so they are reviewable and reproducible (see `doc/world_generation.md`, "The PCG graphs"). | `/RammsWorldGen/Graphs/` |
| **Python** | Graph builder DSL, palette/preset builders, example-level assembly, MCP toolset | `Content/Python/ramms_worldgen/` |

The one wiring rule: each custom node's overridable properties are named
exactly like the members of `FRammsWorldGenParams`, and the graph feeds a
single **Get Actor Property (Self, `Params`)** attribute set into every
node's **Overrides** pin. Adding a parameter is: add a member to the struct,
add a same-named `PCG_Overridable` property to the node that uses it, done.

Points carry three families of attributes:

- **Spawner inputs**: `Mesh` (soft path) and, for primitives, `Material`
  (slot-0 override). Primitives are `/Engine/BasicShapes/Cube` instances
  scaled to size, which is how roads, sidewalks, floors, building shells,
  windows and crosswalk bars are drawn with the Megascans surface materials.
- **Piece properties**: `KindId`, `SizeX/Y/Z`, `Rise`, `Slope`, `FlareSlope`,
  `StepCount`, `OpeningWidth/Offset/Height`, `LandingSize`, `WarningDepth`, `UVScale`,
  `MaterialPath`, `MaterialPath2`, `MaterialPath3`, `Openings`, `Parts` — the Spawn Actor node maps each onto
  `ARammsProcPiece` and then calls `Rebuild()` (`URammsWorldGenGraphLibrary::PiecePropertyNames`; a new
  property needs the graphs rebuilt).
- **Manifest**: `ManifestKind`, `ManifestSubtype`, `bCompliant`, `IssueId`,
  `Measure`, `Threshold` — read by the Manifest node for mesh-spawned
  features, and reported by the pieces themselves for actors.

## Building

The plugin is a Runtime module (`RammsWorldGen`) depending on `PCG`,
`GeometryScriptingCore`, `GeometryFramework`, `GeometryCore`, `Json` and
`JsonUtilities`. The `.uplugin` enables `PCG`, `GeometryScripting` and
`PythonScriptPlugin`; the project `.uproject` enables `RammsWorldGen`.

```powershell
$UE = "C:\Program Files\Epic Games\UE_5.8"
& "$UE\Engine\Build\BatchFiles\Build.bat" RammsEditor Win64 Development -Project="$PWD\Ramms.uproject" -WaitMutex
```

Reflection changes (any `UPROPERTY`/`UCLASS` here) need the full build, not
Live Coding — same rule as the rest of the project.

## First run

1. Build and open the editor. The Output Log should show
   `[RammsWorldGen] registered RammsWorldGenToolset with the toolset registry.`
2. Make sure the Fab packs are present (`Content/Fab/MoStreetProps`,
   `Megascans`, `FreeFurniturePack`, `RestaurantScene`, `Stairs_Street`,
   `adoquines_levantados_alcorque`). They are not committed; see the project
   README. Missing packs only leave palette slots empty (procedural stand-ins
   take over), they do not stop generation.
3. Build the assets from code:
   ```
   python Scripts/editor_remote_exec.py --file Scripts/worldgen/build_assets.py
   ```
   or, in the editor's Python console, `import ramms_worldgen.build_all as b; b.build("all")`,
   or ask the assistant for `worldgen_build_assets`. This writes
   `/RammsWorldGen/Palettes/PAL_Default`, `/RammsWorldGen/Profiles/PROF_ADA`,
   `/RammsWorldGen/Presets/PRE_*`, and `/RammsWorldGen/Graphs/PCG_Streets`,
   `PCG_Indoor` and `PCG_SpawnAndRecord`. The report lists every palette slot it could not fill.
4. Build the example levels:
   ```
   python Scripts/editor_remote_exec.py --file Scripts/worldgen/build_levels.py
   ```
   Creates `/Game/Maps/Gen/Map_Gen_Downtown`, `Map_Gen_Suburban`,
   `Map_Gen_Park`, `Map_Gen_Indoor`, `Map_Gen_Office`, `Map_Gen_MainStreet` and
   `Map_Gen_Store`, each with lights, a
   ground slab, a `WorldGenerator`, the `BP_RammpGen1GameMode`, and a PlayerStart
   on the widest compliant route. Generation is asynchronous: the level is
   saved when the log says `level ... generated`.

## Using it

### In the editor

Drop a **Ramms World Generator** into any level, set **Preset** and **Seed**,
click **Regenerate**. **Randomize** draws a new seed; **Cleanup** removes
everything it spawned; **Place Player Start** moves the PlayerStart to the
manifest's suggested pose; **Export Manifest** writes the JSON under
`Saved/WorldGen/`. The green box is the site; the generator must sit at ground
level (sidewalk level = its Z). Yaw is ignored — sites are axis-aligned.

Per-instance tweaks go in **Param Overrides** + **Override Keys** (only the
listed members apply, so preset updates still flow through). Everything else
is edited on the preset, the palette, or the profile.

The PCG graphs are ordinary graphs: open `PCG_Streets` to see the wiring, add
stock nodes between ours, or make a copy and point a preset at it. If you
re-run the graph builder your edits to those assets are overwritten —
copy first. A new graph only has to emit the standard point streams
(`Primitives`, `Props`, `Modules`, `Grass`, `Pieces`, `Routes`, `Features`) and
end in a `PCG_SpawnAndRecord` subgraph node; the builder's `subgraph()`,
`comment()` and `named_reroute()` helpers keep it tidy.
`Scripts/worldgen/graph_shots.py` captures the open graphs as 1:1 PNG tiles
for docs.

### From Python

```python
import ramms_worldgen._helpers as H
gen = H.find_generator()                 # the generator in the open level
gen.set_editor_property("seed", 42); gen.regenerate()
# ... wait until not gen.is_generating() ...
m = H.manifest_to_dict(gen)              # routes, features, suggestedStart
bad = [f for f in m["features"] if not f["bCompliant"]]
```

`ramms_worldgen.levels.build_level("downtown", seed=7)` makes a level;
`ramms_worldgen.build_all.build("graphs")` rebuilds just the graphs.
`Scripts/worldgen/regenerate.py` regenerates the open level with a random seed.

### From an AI assistant (MCP)

The in-editor MCP server (see the project README) lists the toolset
`RammsWorldGenToolset`:

| Tool | Purpose |
|---|---|
| `worldgen_status` | Generators in the level: preset, seed, generating?, feature/route/issue counts |
| `worldgen_list_presets` | The presets and what each produces |
| `worldgen_generate` | Regenerate; optionally switch `preset` and `seed` |
| `worldgen_randomize` | New random seed |
| `worldgen_cleanup` | Remove what a generator spawned |
| `worldgen_manifest` | The manifest; filter by `kind` or `only_non_compliant` |
| `worldgen_get_params` / `worldgen_set_param` | Read / override one parameter by name (and regenerate) |
| `worldgen_export_manifest` | Write the manifest JSON |
| `worldgen_place_player_start` | PlayerStart to the suggested start |
| `worldgen_build_assets` | Rebuild palette / presets / graphs from code |
| `worldgen_build_level` | Create an example level (`downtown`, `suburban`, `park`, `indoor`, `office`, `mainstreet`, `store`; recipe-built `dense_downtown`, `traffic`) |

A typical session: `worldgen_list_presets` → `worldgen_generate(preset="PRE_Downtown", seed=7)`
→ poll `worldgen_status` → `worldgen_manifest(only_non_compliant=true)` →
`begin_pie` (RAMMS toolset) → drive the chair at a hazard.

### From outside the engine (Remote Control / ramms-tools)

`URammsWorldGenBridge` is a function library made for the Remote Control
API: every call takes and returns strings and numbers, generators are
addressed by actor path (or name / label; `""` = the first one), presets and
maps by asset path or short name. The CDO path is
`/Script/RammsWorldGen.Default__RammsWorldGenBridge`.

| Function | Purpose |
|---|---|
| `GetWorldInfo` | JSON: current map, editor vs play world, generator count |
| `FindGenerators` / `ListPresets` / `DescribePresets` / `ListPalettes` / `ListGeneratedMaps` | Discovery |
| `SpawnGenerator(Preset, Seed, Location, Label)` | Spawn a generator pointed at a preset (the label is also an actor tag, so it finds the generator in packaged builds) |
| `SetPreset`, `SetPalette(Path, Palette)`, `Generate(Path, Seed)`, `Randomize`, `Cleanup`, `IsGenerating`, `PlacePlayerStart` | Lifecycle. `SetPalette("")` goes back to the preset's palette; `Generate` returns false when nothing could be scheduled (no graph) |
| `GetStatusJson` | Cheap poll: preset, palette, seed, generating?, counts, suggested start |
| `GetManifestJson` / `ExportManifest` | The manifest |
| `GetParamsJson` / `SetParamsJson` / `ClearOverrides` | Parameters as a JSON object of name → number, or string for text parameters (`Palette`, `FacadeStyles`) |
| `OpenMap` | Load a map in the editor, or travel to it in a game |

[ramms-tools](https://github.com/rammp-org/ramms-tools) wraps this as
`ramms_tools.worldgen.WorldGen` and the `ramms-worldgen` CLI:

```bash
ramms-worldgen presets
ramms-worldgen generate --preset PRE_Downtown --seed 42 --set HazardDensity=6
ramms-worldgen manifest --issues
ramms-worldgen batch --preset PRE_Park --seeds 1-50 --out data/park
```

```python
from ramms_tools.worldgen import WorldGen
gen = WorldGen().generator()
gen.generate(seed=42)
m = gen.manifest()            # routes, features, issues, suggested start — parsed
```

This is the intended path for episode runners, dataset builders and any
program that is not running inside the editor; the Python module above is
for editor-side tooling and the MCP toolset for assistants.

### At runtime

`ARammsWorldGenerator` is a runtime actor and the nodes are runtime
elements, so a packaged or `-game` sim can regenerate per episode:
`RegenerateWithSeed(N)`, wait for `OnWorldGenerated(Manifest)`.
`bGenerateOnBeginPlay` / `bRandomizeSeedOnBeginPlay` cover the simple case.
Generation is asynchronous (PCG schedules it over frames); `IsGenerating()` is
the poll.

**The robot during a play-time regeneration.** With `bPlacePlayersOnGenerate`
(on by default), regenerating in PIE or a game holds the player pawns still
while the world is rebuilt. Their Unreal-simulated bodies (the RAMMP Gen1's
vehicle, arm and gripper skeletal meshes) go kinematic, so the robot neither
falls through the old floor nor gets shoved by geometry spawning into it.
When the new world is in, the pawns are teleported to
`Manifest.SuggestedStart` (`+PlayerPlacementLift`), velocities are zeroed, and
physics is switched back on after `PlayerReleaseDelay`. The PlayerStart moves
there too. `PlacePlayers()` does the placement on demand. A pawn simulated by
an external engine (the MuJoCo / URLab backend) has no Unreal-simulated
bodies: it is teleported, but that backend has to reset its own state.

**Same seed, same world, everywhere.** The module compiles with
`FPSemantics = Precise`. Unreal compiles editor targets with `/fp:precise` and
game targets with `/fp:fast`, and the difference flipped layout decisions on
near-ties, so packaged stores came out different from the editor's. Placement
tests also compare boxes with a 0.05 cm tolerance (`RammsPCG::Overlaps`,
`InsideBox`), so pieces spaced exactly one gap apart do not depend on rounding.
`ramms.WorldGen.TraceIndoor 1` logs every indoor placement attempt, for
diffing two builds.

The only editor-only parts are the Python builders and the MCP toolset.

## The manifest

`FRammsWorldManifest` (also the JSON): `seed`, `preset`, `palette` (object
path), `sceneKind`, `bounds`, `suggestedStart`, then

- `routes[]`: `name`, `points[]` (polyline, cm), `nominalWidth`,
  `minClearWidth` (after hazards), `bCompliant`.
- `features[]`: `kind` / `subtype`, `location`, `rotation`, `size`,
  `bCompliant`, `issue`, `measure`, `threshold`, `asset` (the palette mesh
  placed, if any), `source` (where it came from, e.g.
  `urbanverse:<uid>|<category>`), `route` (the sidewalk side or path it was
  placed along). For palette meshes `location` / `rotation` / `size` are the
  box the mesh was fitted to (the centre of its base on the ground, its
  facing, its size after scale), not the mesh pivot; trees are placed by their
  trunk, so for them `location` is the trunk and the canopy box can sit off it; for curb ramps `size.x` is the ramp run and `size.y`
  includes the flares.

Kinds and the issues they can carry:

| Kind | Subtypes | Issue |
|---|---|---|
| `CurbRamp` | Standard, TooSteep, NoDetectableWarning, CrossSlope (graded: its cross slope, the street's grade, above `MaxCrossSlope`) | SlopeTooSteep, CrossSlopeTooSteep |
| `Grade` | Running, Cross (graded ground: per sidewalk side, its steepest running grade along the route's centre line and its cross slope at mid-side; `route` = the side) | SlopeTooSteep (above `RampMaxSlope`), CrossSlopeTooSteep (above `MaxCrossSlope`) |
| `Curb` | Low (under 10 cm), Standard, High (over 17 cm) (graded ground: per side, measure = mean curb height, size.z the highest) | — |
| `SpeedHump` | Hump, Table (measure = height) | — |
| `Cover` | Raised, Sunken (a utility cover set into the walk; measure = its lip) | VerticalChangeTooHigh |
| `Crossing` | MissingCurbRamp | MissingCurbRamp |
| `Crosswalk` | Ramped, SteepRamp, OneRamp, NoRamps (whether both ends have a curb ramp; size.x = curb to curb, yaw = the crossing direction) | — |
| `Entrance` | Level, SteppedWithRamp, SteppedSteepRamp, SteppedNoRamp | StepAtEntrance, SlopeTooSteep, DoorTooNarrow, ThresholdTooHigh |
| `Door`, `Threshold` | Standard, Narrow, RaisedThreshold, High/Low | DoorTooNarrow, ThresholdTooHigh |
| `Ramp`, `Step` | Standard, TooSteep / WithRamp, NoRamp | SlopeTooSteep, StepAtEntrance |
| `Hazard` | AFrame, Scooter, BinOut, Cafe, Barrier, Upheaval, Heave (a tilted sidewalk panel), RootHeave (the walk lifted beside a street tree), or a furniture slot indoors | ClearWidthBelowMinimum, VerticalChangeTooHigh, ObstacleInRoute |
| `Building` / `House` | house footprint | — |
| `Prop`, `Furniture`, `Decor`, `Tree`, `Shrub` | palette slot | ClearWidthBelowMinimum when a prop spills out of the furniture zone |
| `Surface` | Cobbled, GravelTrail / Gravel, Dirt, Mud (park patches); on rough ground JointLip (two sidewalk panels' lip over `VerticalChangeMax`), Crack (a panel split, one part proud), Pothole (measure = depth), Brick, Setts (pavers) | SurfaceUneven, VerticalChangeTooHigh |
| `Terrain`, `Water` | Mound (measure = steepest grade) / Pond | — |
| `Room` | Living, Bedroom, Office, Meeting, Kitchen, Dining, Bathroom, Restroom, Corridor, Shop, Storage | — |
| `Stall` | Standard, Accessible (restroom toilet stalls; measure = stall width, threshold 152.4 for the accessible one) | — |
| `Aisle` | Standard, Narrow (shop floors; measure = aisle width) | ClearWidthBelowMinimum |
| `Path`, `Driveway`, `Vehicle`, `CarPark` | FrontWalk / Concrete / Parked, Driveway, CarPark / SingleRow, DoubleRow, MultiRow | — |
| `CheckoutLane` | Standard, Narrow (store checkouts; measure = lane width) | ClearWidthBelowMinimum |
| `Tree`, `Shrub`, `Prop` (lots) | StreetTree, Canopy / Foundation / Mailbox, StreetLight | — |
| `Road` | AlongX, AlongY (a live street segment, recorded for the traffic: location on its centreline at road level, size.x = length between the intersection squares, size.y = width = measure) | — |
| `Intersection` | Cross, Tee, Through, Bend, End (a node live segments meet at: location its centre at road level, size = the square, measure = how many segments meet) | — |

`measure` / `threshold` are the number that decided it (slope as rise/run,
widths and heights in cm) and what it was compared to.

## Segmentation labels

A generator tags what it made with a fine segmentation class, a `seg:<Class>`
tag on each primitive (`URammsSegmentationLabels::LabelGeneratorOutput`). It
does this after every generation and in `BeginPlay`, so worlds generated
before the labels existed are labelled when played. CameraCapture's
segmentation pass turns the tags into custom-depth stencil values and writes
class masks; see its README, "Segmentation". This plugin only writes tags and
does not depend on CameraCapture.

| What | Fine class |
|---|---|
| Palette meshes, instanced or not | the slot: `Bench`, `StreetLight`, `Hazard.AFrame`, `Furniture.Chair`, `Store.Fridge` ... |
| Facade modules | `Facade.<Part>` (the style is dropped): `Facade.GF`, `Facade.GFDoor`, `Facade.Win`, `Facade.Top` ... |
| Composite entries (houses) | every part gets the entry's slot (`House`) |
| Instanced boxes with a `Mat.*` override (roads, sidewalks, paths) | the surface: `Road`, `Sidewalk`, `Path`, `Lawn`, `Floor`, `Wall.Interior` ... |
| Slabs, patches, walls | their surface (as above); a slab more than 2.5 m above the generator is a `Roof` |
| Other procedural pieces | the kind: `Curb`, `CurbRamp`, `Stairs`, `Ramp`, `Door`, `Bollard`, `Tree`, `Mound` ... |
| Hazard pieces (upheavals, heaves, root heaves) | `Hazard.<Subtype>` |
| Graded ground (`GroundPrisms`, `Hump`) | their surface (`Road`, `Sidewalk`, `Lawn`, `Metal` covers, `Cobblestone` setts and brick pavers) |
| Decals (cracks, puddles) | none: a decal takes the class of what it is projected on |

A mesh that answers several slots (a chair used as furniture and as a cafe
hazard) gets its non-hazard slot. Primitives that already carry a `seg:` tag,
or whose actor does, are left alone, so a hand-placed label wins.

The coarse classes these roll up to, and their stencil values, are project
config: RAMMS lists them in `Config/DefaultGame.ini` under
`[/Script/CameraCapture.CameraCaptureSegmentationSettings]`.
`URammsSegmentationLabels::ClassesForPalette` lists every class a palette can
produce, for keeping that list complete when slots are added.

## Collision shapes

A generated world's collision is also available as plain shapes
(`RammsCollisionShapes.h`, no engine types beyond Core), so another physics engine can
build the same world: RammsMujocoSupport turns them into MuJoCo world geoms.
`ARammsWorldGenerator::GatherCollisionShapes(Shapes[, Filter])` returns them in
world space; `DescribeCollisionShapes()` counts them by type and surface. A
`FRammsCollisionShapeFilter` limits them to a box or to a radius in plan.

The shapes are exactly what Chaos collides with, so a curb face stays a vertical
step and a 6 mm lip stays 6 mm:

| Source | Shapes |
|---|---|
| Slab, curb, column, steps, ramp (slab, landing, lips), wall (around its openings, frames, glass), door (leaf, handles, threshold), planter, roof soffit, warning strip | one box per part |
| Bollard, tree trunk and branches, tree and shrub canopy blobs, gable roof, the ramp of a curb ramp | one convex hull per part, of the part's own vertices |
| Curb ramp flares (their top is twisted), mounds and hollows | a height grid over the mesh's own vertices |
| Patch | the slab under the disc (its top is the disc) |
| Decal | none |
| Instanced and static meshes the generator made | per instance, the mesh's simple collision as Chaos builds it (`FK*Elem::GetFinalScaled`): boxes, spheres, capsules, convex hulls; a complex-as-simple mesh as the hull of its triangles (coarser where it is concave; logged once per asset) |

Foliage and anything without physics collision is skipped, as are articulated
stand-ins (they simulate as articulations), pawns, and actors or components
tagged `RammsDynamic`: tag whatever moves after generation (traffic), or it is
exported as a fixed obstacle where it stood.

**One code path.** `ARammsProcPiece::AppendCollisionShapes` runs the kind's own
builder into a scratch mesh with a capture open: every box the builder appends
reports itself as a box, every other convex primitive as the hull of the vertices
it appended, every lifted grid as a height grid. The mesh and the shapes cannot
drift apart, and a new builder part is exported without extra code. A kind added
later reports its shapes from its own case in `AppendCollisionShapes`. A
sphere-box (a canopy blob) splits some quads inward, a dent of a few centimetres,
so its edges are flipped to fold outward when it is built: its mesh is then its
hull, and Chaos collides with exactly the hull that is exported.

**Height grids** split each cell along the diagonal from sample (i+1, j) to
(i, j+1), which is how MuJoCo's height fields split theirs once Unreal's Y is
mirrored. A mesh drawn from a grid must split its cells the same way, or the
two agree only at the samples: `RammsCollisionShapes::MatchHeightGridDiagonals`
flips a grid mesh's edges to match (flares and mounds use it, so their meshes
changed by that flip).

**Surfaces** come from the segmentation label (`Road` is `Asphalt`, `Sidewalk`
`Concrete`, `Curb` `Curb`, `Lawn` `Grass`, `Hazard.Mud` `Mud` ...,
`RammsCollisionShapes::SurfaceForClass`), else from a physical material's name,
else from the piece kind; anything else is `Default`.

`Ramms.WorldGen.CollisionShapes.ProcPieces` checks every kind (and pitched,
rolled pieces) against its mesh: bounds within 1 mm, vertical probes within
2 mm of Chaos's trace. `Ramms.WorldGen.CollisionShapes.InstancedMeshes` checks
scaled, rotated instances against Chaos's simple collision.

## Parameters

All in cm unless noted; defaults are the 2010 ADA numbers where one applies.
`NonCompliance` (0..1) is the single dial: 0 gives a fully accessible world,
and above 0 each feature independently rolls to be generated in one of its
non-compliant forms, always recorded in the manifest. The suburban preset
runs it at 0.2, the office at 0.1.

| Group | Parameters |
|---|---|
| Site | `SiteSizeX/Y`, `NonCompliance`, `Palette` |
| Streets | `BlockSizeMin/Max`, `RoadWidth`, `SidewalkWidth`, `FurnitureZoneWidth`, `PlantingStrip` (1 = the furniture zone is a grass strip), `CurbHeight`, `StreetJitter`, `StreetDropout`, `MidblockCrossingChance`, `CrosswalkWidth`, `ParkFraction`, `ParkingDensity`, `DecalDensity` (cracks, potholes, patches, stains per 100 m), `StreetRules` (hydrants, bus stop zones, clear crossing approaches, cars facing their traffic, corner signals), `LitterDensity` (per 100 m of side), `KeepClear` (world boxes `x0,y0,x1,y1;...` nothing is placed in) |
| Buildings | `LotWidthMin/Max`, `SetbackMin/Max`, `FloorsMin/Max`, `FloorHeight`, `LotCoverage`, `WholeBuildingChance`, `EntranceStepChance`, `EntranceRampChance`, `WindowSpacing`, `FacadeStyles`, `RearParkingChance` |
| Hazards | `PropDensity`, `HazardDensity` (per 100 m), `TreeSpacing`, `CanopyChance` (big old trees, closer together), `StreetLightSpacing`, `HazardIntrusionChance` |
| Accessibility | `MinClearWidth`, `PreferredClearWidth`, `RampMaxSlope`, `MaxCrossSlope`, `CurbRampFlareSlope`, `DoorClearWidth`, `DoorThresholdMax`, `VerticalChangeMax`, `LandingSize`, `DetectableWarningDepth` |
| Terrain (all 0: flat) | `MaxStreetGrade` (0..0.15, rise / run between intersections), `RoadCrown` (0..0.08), `SidewalkCrossSlope` (0..0.08, each block draws 0.6-1 of it), `NonCompliantCrossSlopeChance` (0..1, per block: 3-6 %), `CurbHeightMin` / `CurbHeightMax` (cm; Max 0 = every curb `CurbHeight`) — see [Graded streets and rough ground](#graded-streets-and-rough-ground) |
| Roughness (all 0: smooth) | `SurfaceRoughness` (0..1, panel lips), `PanelLength` (cm, 150), `HeaveDensity`, `CrackDensity`, `CoverDensity`, `PavingPatchDensity` (per 100 m of side), `PotholeDensity` (per 100 m of road), `SpeedHumpChance` (per street segment), `RootHeaveChance` (per street tree in a pit) |
| Suburban / park | `LawnLots`, `YardTreeChance`, `DrivewayChance`, `DrivewayWidth`, `FrontBoundaryChance`, `PathWidth`, `FoliageDensity`, `GrassDensity` (grass clumps per m^2 on lawns, strips and parks) |
| Indoor | `RoomSizeMin/Max`, `CorridorWidth`, `CorridorChance`, `WallThickness`, `WallHeight`, `DoorWidthMean/Std`, `ThresholdChance`, `FurnitureDensity`, `OfficeFraction`, `RetailFraction` |

## Packs and palette recipes

A palette that mixes several content packs is built from code, so the same look comes back on any machine
with the same packs, and a pack that is not installed only leaves its slots to the others.

- **Packs** (`Content/Python/ramms_worldgen/packs/`): one module per pack declares where it lives and what it
  offers, per slot: meshes (`meshes={"Bench": [paths]}`), material sets (`material_sets`, e.g. the decal
  slots) and world-aligned **surfaces** (`surfaces`: a `surfaces.py` spec over the pack's own textures, built
  on demand). Paths are relative to the pack's folder (or absolute; a `*` matches a name pattern), so two
  packs that ship a mesh of the same name never get mixed up; a `Ref` can also fix a mesh's facing (`yaw`), its
  size (`scale`, for a pack modelled at another scale) or some of its materials (`materials`: material slot ->
  pack surface, built into the recipe's folder; the entry becomes a one-part composite, so only in the slots
  whose node places composites, `recipes.MATERIAL_COMPOSITE_SLOTS`: today `TrafficSignal`; elsewhere it is
  reported and not applied). Known today: American City Packs (decals, street
  litter, manholes, whole buildings, skyline towers), Downtown West (street trees, furniture, the corner signal
  mast, manhole covers, decals, asphalt and curb, facade kits), Camden Market (facade kits, a pole signal,
  utility covers, dirt decals), the Restaurant Scene (stain decals), Megascans (sidewalk, asphalt, concrete,
  brick, plaster and weathered-concrete walls).
- **Recipes** (`recipes.py`): a palette is a base (`PAL_Default`'s slots, optionally with UrbanVerse-100K
  furniture, cars and trees on top), slots to clear, pack **layers** that add to or replace the slots they
  offer (in order, with a weight for their mesh entries; material sets have no weights), and `Mat.*` slots
  pointed at pack surfaces. `Downtown` is the README's hero
  street: UrbanVerse furniture and cars, Downtown West street trees and furniture, decals from every pack,
  Megascans sidewalks, concrete and walls, Downtown West asphalt and curbs, the built-in puddle decal, and
  the Downtown West and Camden Market facade kits (`kits`; `PRE_DenseDowntown`'s `FacadeStyles` lists them).
- **Building one**: `recipes.build("Downtown")` (or the `worldgen_build_palette` MCP tool) writes
  `/Game/WorldGen/Palettes/PAL_Downtown` and its materials under `/Game/WorldGen/Materials/Downtown`; it needs
  the UrbanVerse cache (`urbanverse_cache=` or `$RAMMS_URBANVERSE_CACHE`) for that layer and skips it
  otherwise. The report lists the packs found, each layer's additions, the surfaces built, what was
  skipped, and the pack meshes whose collision the build changed (`modified_pack_assets`) or found missing
  (`missing_collision`). `/Game/WorldGen` holds only what is built from local packs and is not committed.
- **A level**: `levels.build_level("dense_downtown")` builds the recipe if needed and generates
  `/Game/WorldGen/Maps/Map_Gen_DenseDowntown` with `PRE_DenseDowntown` (shorter blocks, narrow lots to the
  sidewalk, cars along the curbs, more furniture and wear, no parks, the street rules and litter on).
  `worldgen_list_packs` shows what is installed.
- **Adding a pack**: a module in `packs/` with `PACK = Pack(name, [folders], meshes=..., material_sets=...,
  surfaces=...)`, listed in `packs.ALL`; then a layer for it in a recipe.

## Extending

- **New assets**: add names to `palettes.py` (`MESH_SLOTS`, `MATERIAL_SLOTS`)
  and rebuild the palette, or edit `PAL_Default` by hand. Footprints and
  heights are read from mesh bounds and used for clearances; `LocationOffset`
  puts the mesh's lowest point on the ground. A pack with trees or doors goes
  in the `Tree` / `Shrub` / `DoorActor` slots; the procedural stand-ins stop
  being used as soon as a slot is filled. Furniture and street props are
  normalised to real-world heights per slot (`SLOT_HEIGHTS`); whole houses go
  in the `House` slot and replace the shell on set-back low-rise lots.
- **Ground surfaces**: `surfaces.py` builds world-aligned materials
  (`M_RammsAsphalt`, `M_RammsSidewalk`, `M_RammsConcrete`, `M_RammsGrass`,
  `M_RammsLawn`, `M_RammsPath`, `M_RammsDirt`) from the packs' tileable
  textures. UVs come from world XY at a real tile size, so stretched road
  cubes and huge ground slabs tile correctly; repetition is broken by a second
  sample at another scale (or a variant texture set) blended by a
  low-frequency mask, plus macro brightness variation. Edit `SURFACES` to swap
  textures or tile sizes; the Mat.* slots prefer these materials.
- **Shelving**: `Store.Shelf` entries record their board heights
  (`ShelfHeights`: vertex clusters holding at least 4% of the mesh's vertices,
  plinth and top cap excluded) and face the open side away from the back panel
  (the end of the short axis with the denser vertex band). Shop floors stock
  every board below 175 cm with `Store.Product` meshes that fit under the next
  board.
- **Composite entries**: a palette mesh entry with `Parts` (mesh + relative
  transform + materials) instead of a `Mesh`. `palettes.COMPOSITE_SLOTS`
  captures blueprint houses (Residential Houses - Modular Pack, ~300 child
  actors each) by spawning them once at build time, measuring the union
  bounds and facing them by their front door. These houses have a front and a
  back entrance, so each `EntranceDoor` is scored (the wide front door counts
  for it, a `*_Back` frame against it, nearby porch parts and a front garage
  add a little) and the front is the direction of its porch stairs. The entry
  records `EntranceOffsetY` / `EntranceRise` (front walk and manifest records
  meet the real door; no procedural steps or door are added) and, when a
  garage door is on the front, `GarageDoor` / `GarageWidth` (the driveway runs
  up to it). The massing node spawns the parts as instanced modules
  (`WriteComposite`), so a street of them costs nothing.
- **Facade kits**: `Facade.<style>.GF` / `GFDoor` / `GFCorner` / `Win` /
  `WinTop` / `WinCorner` mesh slots plus the generic `Facade.Top`,
  `Facade.TallTop` (+ `Corner`) and `Facade.WinCorner`, filled by
  `palettes.build_facade_slots` from the AmericanCityPacks module names. Each
  entry records its material slot categories (`MaterialSlots`); the
  `Facade.Mat.<Category>` slots hold the material sets (`Bricks`, `Windows`,
  `Frame`, `Concrete`, `Ornament`, `Shop`, `Metal`, `Roof`...) that Building
  Massing picks from once per building and writes as `Material0..7`
  attributes for the module spawner. Modules have their pivot at the start of
  the run, the facade plane on local Y=0 with the exterior toward +Y, and the
  L1 corner pieces turn the next face toward local −Y. Node settings
  `bFacadeModules` (default on) and `FacadeStyles` (`dt01,dt02,dt03`) choose
  the kits; a lot narrower than 5 m, or a style that cannot tile it, falls
  back to the cube dressing.
- **Pack facade kits** (`packs.base.Kit`): a pack's own modular kit becomes a style with a
  `FRammsFacadeKitInfo` in the palette's `FacadeKits` (family, weight, `MaxStretch`, `UpperMixChance`).
  Its modules need no common grid: each floor of a face is a run of modules (the door module placed first,
  inside the front's ground floor) stretched or squeezed by at most `MaxStretch` to fill the face exactly, a
  knapsack over the kit's widths. Only the street faces are dressed -- the front, a corner lot's side on the
  cross street (`StreetSideNeg` / `StreetSidePos` from Subdivide Lots), the back of a one-row block
  (`StreetBack`) -- over the massing shell; the cornice is the upper floors' style's `Facade.<style>.Top`
  (the style itself, or with `UpperMixChance` another of its family), on every dressed face or none; a module's
  material slots keep the pack's materials unless the kit maps a slot to `Facade.<style>.Mat.<Category>`
  (the glass slots of Downtown West and Camden take the built-in `M_RammsWindowGlass`: their own fake-interior
  glass renders as flat panels without reflection captures). Each module is stretched to fill its slot exactly.
  The massing shell stands back from the dressed faces by the deepest inward reach of the modules placed (read
  from their meshes; at most a quarter of the building's width or depth), so recessed window panes are not
  behind its wall, and stays on the building line on the faces left bare.
  Each entry carries the yaw, offset and scale that bring the pack's pieces to the convention (measured from
  the bounds, so centred and end pivots both work). A style that cannot tile a building gives way to another
  (up to four) before the cube dressing. Today: Downtown West's buildings a, b (two storefront lines), d, e and
  f at 0.75 scale (the pack's 6 m storeys), Camden Market's pub, and its residential and industrial terraces
  over the Building04 storefronts. The original kits are untouched (same picks for the same seed).
- **Whole buildings**: `Building.Downtown`, `Building.CornerNeg` and `Building.CornerPos` hold complete buildings
  as composites (`palettes.packed_building_entry` captures a packed-level-actor blueprint instance by instance:
  its front -- the side its ground-floor modules face -- toward the street, the facade line, not a cornice's
  overhang, on the lot's front, the entrance at its front door; its parts are its only collision, so they are
  made solid, apart from the decoration a pack lists in `building_decor`). With `WholeBuildingChance`, Subdivide Lots
  cuts a lot to the frontage of a building that fits the row (a row's first lot, a corner, takes a corner
  building whose second street face is on its cross-street side) and Building Massing places it instead of a
  massing: `Building/Commercial` in the manifest. The Downtown recipe fills them from American City Packs'
  `BPP_Building[Corner]_DT_*` (13-24 storeys, 17-40 m of frontage: they need deep rows).
- **Skyline**: a downtown level's ring of distant towers outside the site (`levels._add_skyline`, the palette's
  `Skyline` slot: American City Packs' closed low-poly background buildings in the Downtown recipe), so a view
  down a street ends in a city.
- **New preset**: add an entry to `presets.PRESETS` (or duplicate an asset),
  point it at a graph.
- **New parameter**: `FRammsWorldGenParams` member + same-named
  `PCG_Overridable` property on the consuming node.
- **New node**: derive from `URammsPCGSettingsBase` / `FRammsPCGElementBase`,
  emit through `FRammsPointWriter` (`WriteCube`, `WritePaletteMesh`,
  `WritePiece`, `WriteManifest`), add it to `graphs.py`.
- **New piece kind**: add to `ERammsProcPieceKind` (at the end; `GetEffectiveKind` clamps `KindId` to the
  last value), a `Build*` in `RammsProcPiece.cpp` (the ground kinds are in `RammsProcPieceGround.cpp`), its
  collision shapes in `AppendCollisionShapes`, and emit it from a node.

## UrbanVerse assets

[UrbanVerse-100K](https://huggingface.co/datasets/Oatmealliu/UrbanVerse-100K) (Liu et al., ICLR 2026) is
a gated Hugging Face dataset of about 100K metric-scaled, annotated urban objects (GLB). It can fill the
street-furniture, vehicle and hazard slots with far more variety than the Fab packs. It takes two steps:

1. **Choose and download** (outside Unreal). `Tools/urbanverse_fetch.py` needs Python with
   `pip install --no-deps urbanverse-asset` plus `pip install "huggingface_hub[cli]" pillow tqdm trimesh`,
   and a Hugging Face login (`hf auth login`) on an account that has accepted the dataset's terms.
   ```
   python Tools/urbanverse_fetch.py --cache D:/urbanverse --per-slot 8
   python Tools/urbanverse_fetch.py --cache D:/urbanverse --analyze    # material analysis only, no downloads
   ```
   - The `SLOTS` table maps each palette slot to UrbanVerse leaf categories (`--list` writes all 659), with
     height and length limits in metres and a minimum quality score (0–10, from the annotations). Only single
     objects are taken, and the categories in a slot are taken in turn.
   - Description-based excludes (picnic tables are not benches) match whole words of the short description.
   - It writes `ramms_selection.json` (slot to assets, with size, mass, friction, license) and
     `ATTRIBUTION.md` (per asset: title, Sketchfab link, author, license with its link). Fetching some slots
     (`--slots Bench,Car`) keeps the other slots of an earlier run; each slot's choice depends only on
     `--seed` and that slot.
   - `--commercial` keeps only assets whose license allows commercial use (CC0, CC BY, CC BY-SA; anything
     with NC or ND, or a license it does not recognise, is dropped). Licenses vary per asset: most are
     CC BY, some CC BY-NC.
2. **Import and build the palette** (in the editor):
   ```python
   import ramms_worldgen.urbanverse as UV
   UV.build(r"D:/urbanverse")                 # or mode="mix" to keep the Fab variants too
   ```
   - The GLBs are imported with Interchange under `/Game/UrbanVerse/<Slot>/`, with their materials and
     textures. GLBs over 40 MB (heavy scans) are skipped.
   - Materials: Interchange makes every glTF material flagged `BLEND` translucent, and many UrbanVerse
     exporters flag whole models that way (opaque bodywork, benches, shelters baked into one texture), which
     renders milky and badly lit. The fetch step records each material's alpha (factor, and a histogram of
     the texture's alpha; `--analyze` adds it to an existing selection), and the import makes such a
     material opaque, masked (cut-outs, leaves) or real glass: `M_RammsGlass` for panes (thin, two-sided,
     near-black base, Fresnel opacity) or `M_RammsGlassAtlas` for one texture holding glass and solid parts
     (masked; the glass texels dithered with glass roughness). `urbanverse.MATERIAL_OVERRIDES` covers
     panes the analysis cannot tell from opaque parts. Fully transparent helpers and baked shadow cards stay
     as authored. glTF `MASK` cut-outs (leaves) that Interchange brings in translucent are made masked at
     their own `alphaCutoff` (0 keeps every texel; 0.5, the glTF default, when the importer recorded none),
     so canopies cast and receive shadows; a `BLEND` material made masked clips at 0.5. The glTF roughness
     factor of a foliage cut-out (a plant's: a foliage word in the material's path or the asset's category) is
     held at 0.5 or more, and any other cut-out's only when it is near zero (some leaf exports carry 0, a
     dark mirror in shade; grilles and bike parts keep their gloss). The factor scales any roughness texture.
   - Glass look per placement: both glass materials take `Frost` (0 clear .. 1 milky), `Bump` (0 flat .. 1
     rippled / patterned), `Tint`, `Roughness` and opacity parameters per material instance, and read six
     floats of per-instance custom data on top: frost offset, opacity multiplier, tint multiplier (RGB) and
     bump offset. `M_RammsGlass` also reads what is behind the pane back from the scene colour, so frost
     really blurs it (`BlurRadius`, cm), ripples shift it (`Refraction`, cm) and the tint colours it
     (`ScreenSpace` 0 limits that to frosted or rippled glass). After each generation
     `ARammsWorldGenerator::ApplyGlassVariation` gives every placed glass instance its own look from the seed
     and its position: `GlassVariation` (0 = all identical) scales the range, `GlassFrostChance`,
     `GlassTintChance` and `GlassBumpChance` pick frosted, tinted (green, blue, bronze, smoked) and rippled
     ones. Vehicle glass (`InstanceFrost` / `InstanceBump` 0) varies in tint and opacity only.
     `URammsWorldGenGraphLibrary::SetGlassInstanceLook` / `VaryGlassInstances` / `SpawnInstancedMesh` do the
     same from Blueprint or Python. In the atlas material the glass texels are dithered, so frost there is
     denser, brighter coverage rather than a blur.
   - Each mesh gets complex-as-simple collision and a physical material with its annotated friction, set on
     the body and on the mesh's own material instances (complex collision takes the material's physical
     material). Meshes over 50K triangles become Nanite. A GLB that imports as several meshes keeps the
     largest, with a warning.
   - `PAL_UrbanVerse` is `PAL_Default` with those slots replaced, refreshed in place on a rebuild (references
     to it stay valid). Assets keep their real size; their front comes from the same geometric test as the
     Fab meshes, and long-fronted pieces (benches, shelters, dumpsters) are turned so a long side is the
     front, the annotation's `frontView` choosing which. Each entry's `SourceId` is
     `urbanverse:<uid>|<category>`, which the manifest carries as the feature's `source`.
   - To check facing: `LINEUP_PALETTE = "/RammsWorldGen/Palettes/PAL_UrbanVerse"` with
     `Scripts/worldgen/lineup.py`.

Use it by setting a preset's or generator's `Palette` (a generator override does not touch the preset), or
from outside with `SetPalette` / `ramms-worldgen generate --palette PAL_UrbanVerse`. Packaged builds cook
`/RammsWorldGen/Palettes` and `/Game/UrbanVerse` through ramms-sim's `DirectoriesToAlwaysCook`.
The imported assets stay out of git, like the Fab packs.

## SimReady assets

[SimReady Central](https://github.com/NVIDIA-Omniverse/simready-central) is NVIDIA's index of OpenUSD
"SimReady" assets that NVIDIA and its partners host in their own public, ungated Hugging Face datasets. Each
file under its `submissions/` lists one dataset's reviewed packages, one URL per package pinned to a dataset
commit. A package is metric USD with physics (mass, colliders, friction), OmniPBR (MDL) materials with a
`UsdPreviewSurface` fallback, and a `SimReady_Metadata` block (extents, mass, license, author). It fills
the indoor-furniture, appliance, bin, sign and hazard slots, which complements UrbanVerse's street furniture.
Three datasets are used: `nvidia/simready-assets` (about 2,500 props: furniture, bins, signs, boxes, crates,
office and medical equipment), `ForgeCAD/simready-assets` (household and kitchen items) and
`LGElectronics/simready-assets` (appliances, TVs, monitors). The robot arms, data-centre equipment and
PalatialSim's numbered scans are left out. It takes two steps:

1. **Choose, download and flatten** (outside Unreal). `Tools/simready_fetch.py` needs Python with
   `pip install huggingface_hub usd-core`; no Hugging Face login.
   ```
   python Tools/simready_fetch.py --cache D:/simready --list            # candidates per slot, no downloads
   python Tools/simready_fetch.py --cache D:/simready --per-slot 4
   python Tools/simready_fetch.py --cache D:/simready --slots Furniture.Sofa,TrashBin --per-slot 6
   ```
   - The `SLOTS` table maps each palette slot to asset-name patterns, with height and length limits in metres.
     Candidates are taken from asset families in turn (`box_cardboard_a01` and `_b02` are one family), so a
     slot gets variety rather than colour variants of one model.
   - For each candidate only the package definition and the root USD (tens of KB) are read first, for the
     license and `SimReady_Metadata` extents. Wall-mounted slots (`WALL_SLOTS`) are checked on the merged mesh
     only, since their extents can include the height they hang at.
   - The chosen packages are downloaded at the commit the index pins, without thumbnails and without NVIDIA's
     second copy of every texture under `source_assets/`. NVIDIA's tiling textures are up to 8K, so four
     per slot is about 7 GB; `--max-mb` (600) skips the heaviest packages.
   - `Tools/simready_flatten.py` then merges each package into one mesh, `ramms_merged.usdc` beside it.
     Unreal's Interchange USD import brings every rigid body in as its own static mesh at a local pivot (its
     combine option does not apply to USD), so a ForgeCAD bin would arrive as 319 meshes. The flattener keeps
     the renderable meshes (not guide, proxy, invisible or collider prims, nor grasp markers), bakes their
     world transforms, converts to Z-up metres, puts the origin at the bottom centre, and writes one
     `GeomSubset` per material. Materials keep their `UsdPreviewSurface` network with absolute texture paths;
     identical copies on several parts become one slot. It also records the face-weighted friction of the
     asset's physics materials.
   - It writes `ramms_selection.json` (slot to assets, with package id, license, author, mass, friction, size)
     and `ATTRIBUTION.md` (per asset: a link to it at its pinned commit, the dataset, author and license).
     Fetching some slots keeps the other slots of an earlier run; each slot's choice depends only on `--seed`
     and that slot. `--refresh-index` re-reads the index from GitHub; `--commercial` keeps only packages
     whose license allows commercial use (all three datasets are CC BY 4.0 at the time of writing).
     `--packages` downloads named packages whole, without choosing or flattening, for the MuJoCo conversion below.
2. **Import and build the palette** (in the editor):
   ```python
   import ramms_worldgen.simready as SR
   SR.build(r"D:/simready")                       # or mode="mix" to keep the base palette's variants too
   SR.build(r"D:/simready", base="/RammsWorldGen/Palettes/PAL_UrbanVerse")   # UrbanVerse streets, SimReady rooms
   ```
   - Each merged USD is imported with Interchange into its own folder, `/Game/SimReady/SM_SR_<NV|FC|LG>_<asset>/`,
     with its materials and textures (textures are clamped to 2048). An asset chosen for several slots is
     imported once. Meshes over 50K triangles become Nanite. Importing the 76 packages of a four-per-slot run (81 slot entries)
     takes about 8 minutes, most of it texture compression; a rebuild that finds them imported takes seconds.
   - Each mesh gets collision as the palette meshes do and a physical material with the asset's SimReady
     friction, on the body and on its own material instances.
   - `PAL_SimReady` is the base palette (`PAL_Default` unless `base` says otherwise) with the filled slots
     replaced, refreshed in place on a rebuild. Assets keep their real size. Their front comes from the same
     geometric test as the Fab meshes; SimReady has no common front axis (NVIDIA's two office-chair packages
     are authored 90 degrees apart), so `simready.FACING_FIX` corrects the assets where that test picks the
     back or a side (refrigerators and washers carry their weight high at the back, cabinets are plain boxes).
     A new asset should be checked with `LINEUP_PALETTE = "/RammsWorldGen/Palettes/PAL_SimReady"` and
     `Scripts/worldgen/lineup.py`, and added there if it faces the wrong way. Each entry's `SourceId` is
     `simready:<namespace>/<dataset>/<asset>@<commit>|<category>`, which the manifest carries as the
     feature's `source`.
   - Articulated assets (bins with lids, fridges, office chairs) come in as one rigid mesh in their authored
     pose; one LG refrigerator is authored with a door ajar. To open their doors and drawers, convert them to
     articulations (below); a generated level then places the articulation in the mesh's stead.

Use it like `PAL_UrbanVerse`: a preset's or generator's `Palette`, `SetPalette`, or
`ramms-worldgen generate --palette PAL_SimReady`. Packaged builds cook `/Game/SimReady` through ramms-sim's
`DirectoriesToAlwaysCook`. The imported assets and `PAL_SimReady` stay out of git.

### Articulated SimReady assets in MuJoCo

Many SimReady packages are articulated: their UsdPhysics rigid bodies are joined by revolute and prismatic joints
with limits (fridge and microwave doors, drawers, crisper bins), with mass, colliders and friction. MuJoCo reads USD
only from a source build with USD support, which its pip wheels and URLab's bundled MuJoCo lack, so
`Tools/simready_mjcf.py` converts a package to MJCF. The result loads in MuJoCo as it is and imports through URLab
as an `AMjArticulation`, where the Gen3 + 2F-85 can open it (see ramms-sim's `mujoco/articulation_test.py` and
`Scripts/pie_tests/simready_door/`).

1. **Download** the package if no palette slot chose it (`--packages` takes it whole, at the commit the index pins):
   ```
   python Tools/simready_fetch.py --cache D:/simready --packages ForgeCAD/simready-assets:refrigerator,microwave_oven
   ```
   It prints each package's root USD, license and author.
2. **Convert** (needs `pip install usd-core numpy pillow scipy`; with `mujoco` installed it also loads the result):
   ```
   python Tools/simready_mjcf.py D:/simready/ForgeCAD__simready-assets/refrigerator/simready_usd/refrigerator.usda \
       --out D:/simready/mjcf/sr_fc_refrigerator
   ```
   - One MJCF body per rigid body, nested along the joints from the body welded to the world (or the articulation
     root). Revolute joints become hinges and prismatic ones slides, with the USD limits; fixed joints nest a body
     without a joint. A body authored with its door ajar keeps that pose: the authored joint value becomes the
     joint's `ref`, so the limits still mean what they meant in USD.
   - `<inertial>` from PhysicsMassAPI (NVIDIA puts it on the colliders; those are summed per body). Joint damping and
     springs come from the joints' drives, else `--hinge-damping` / `--slide-damping`.
   - Collider cubes, spheres, capsules and cylinders become MJCF primitives (ForgeCAD's handles are capsules and
     cylinders, which a gripper can hold); collider meshes are written as their convex hull, which is what MuJoCo
     collides with. NVIDIA's and LG's whole-part SDF colliders are concave, so a hull closes a cavity; `--decompose`
     splits them with CoACD (`pip install coacd`; minutes per asset). Colliders keep their physics-material friction.
   - Visual meshes, one per body and material, with UVs and normals, as OBJ for MuJoCo and as GLB beside it, which
     URLab imports instead (its OBJ path goes through the FBX importer, which reads metres as centimetres and drops
     small parts as degenerate). Materials and base-colour textures come from the `UsdPreviewSurface`.
   - A joint's two bodies, each with the bodies welded to it, never collide (`<contact><exclude>`), as in USD.
     MuJoCo's own parent filter does not apply to a parent welded to the world, which the root is unless `--free`:
     without the pairs a bin's lid or a washer's door rests in contact with its base and cannot move. Chaos never
     collides a joint's two bodies, so the pairs also keep the two engines alike.
   - Keyframes `closed` (the authored pose) and `open` (each limited joint 90% of the way to its far limit), for
     URLab's `ResetToKeyframe`.
   - The model is named `sr_<nv|fc|lg>_<asset>`, and its default classes are namespaced, so it composes with the
     robot (`MjSpec.attach`) and in URLab. `conversion.json` lists the bodies, joints (range, authored value) and
     any warning.
3. **Import into Unreal** (or drag the `.xml` onto the Content Browser):
   ```python
   import ramms_worldgen.simready as SR
   SR.import_mjcf(r"D:/simready/mjcf")        # every <name>/<name>.xml in the folder, or one .xml
   ```
   URLab's `MujocoImportFactory` makes the Blueprint `/Game/SimReady/MuJoCo/<name>` and puts its meshes and textures
   in `/Game/MuJoCoImports/<name>_Assets`; ramms-sim's `.gitignore` leaves out both. A Blueprint that already exists is
   kept, because a replace-import of a URLab Blueprint asserts in the editor: delete it, restart the editor, and
   import again to refresh it.

Checked so far: the ForgeCAD `refrigerator` (two doors, freezer drawer, two crispers) and `microwave_oven` (door,
turntable, release button) convert, load, swing within their limits and import through URLab with their textures,
and so do `chest_of_drawers`, `front_load_washer`, `kitchen_base_cabinet`, `kitchen_wall_cabinet`,
`pedal_trash_can`, `range_oven`, `vanity_sink_faucet` and `wheelie_bin`. Each one's doors, drawers and lids open
and close under the same controller on MuJoCo and on Chaos (ramms-sim's
`Scripts/pie_tests/chaos_articulation/simready_open.py`).
The Gen3 + 2F-85 opens and closes the fridge's left door (by its bar handle), its freezer drawer (by its
handle) and the microwave door (its free edge straddled by the half-closed fingers), in MuJoCo and in Unreal PIE
through `RammsMjEndEffectorController` (ramms-sim's `mujoco/articulation_test.py` and
`Scripts/pie_tests/simready_door/`). NVIDIA's `refrigerator_f01` (15 joints)
and LG's `cooking_mser1130b` (with `--decompose`) convert and load; their visual meshes are dense (265 MB of
OBJ/GLB for the NVIDIA fridge).

### Articulated SimReady assets in generated levels

A palette entry can name an articulated stand-in for its mesh: `FRammsPaletteMesh::ArticulatedClass` (an
`AMjArticulation` Blueprint, such as one `SR.import_mjcf` made) and `ArticulatedTransform` (the articulation's
transform in the mesh's frame). After generation the generator replaces every unit-scale instance of such a mesh
with a spawned articulation at `ArticulatedTransform * instance` (`bSpawnArticulated`, default on;
`SpawnArticulated()` redoes it and `GetArticulatedActors()` lists them). Only the instanced meshes the generator's
own PCG component manages are replaced (a partition actor can hold several generators' output). The actors are
spawned into the generator's level, tagged `RammsArticulated:<GeneratorId>` (a GUID per generator, so two loaded
levels' generators never share one), `RammsGen:<generator>` and `RammsSlot:<slot>`, are saved with the level, are
cleared on `Regenerate` / `Cleanup`, and keep their slot's segmentation label. Instances that are scaled stay rigid
meshes.

```python
import ramms_worldgen.simready as SR
SR.import_mjcf(r"D:/simready/mjcf")   # the articulations (above)
SR.articulate()                        # PAL_SimReady entries whose package has one get it as their stand-in
```

`SR.build(..., articulated=True)` (the default) does the same while it builds the palette. `articulate` pairs a mesh
with `/Game/SimReady/MuJoCo/sr_<nv|fc|lg>_<asset>` and measures the offset between the two (the flattened mesh's
origin is its bottom centre, the MJCF's the package's). With the ten ForgeCAD conversions above, `PAL_SimReady`'s
TrashBin, Cupboard, Sink, Appliance, KitchenLow, KitchenStove and Clutter slots get wheelie bins, chests of drawers,
vanities, washers, base cabinets, ranges and pedal bins.

The articulations run on MuJoCo or Chaos, whichever the level chooses with RammsMujocoSupport's
`ARammsPhysicsSettings` (or `-RammsPhysics=` / `ramms.Physics.Engine` for one run), with the same assets either way.
ramms-sim's `Scripts/worldgen/simready_level.py` generates such a level (a furnished home on `PAL_SimReady` with a
settings actor), and `Scripts/pie_tests/chaos_articulation/simready_showroom.py` builds a showroom of the
articulated assets for side-by-side runs.

### Hinged doors and ceilings

`ramms_worldgen.doors` closes up an indoor level for a robot's sensors. It turns a door-leaf mesh (the palette's
`Door.Interior` / `Door.Exterior` meshes) into a URLab articulation: the leaf's triangles, UVs and material in an MJCF of
a frame welded to the world and the leaf on a hinge (`door`, 0 to 100 degrees, a box collider for the panel and one
for the handle), imported with `SR.import_mjcf` as `/Game/SimReady/MuJoCo/sr_<mesh>_w<width>_h<height>_s<swing>_r<rev>`.
Each of the mesh's material slots stays a slot of its own.

It runs on MuJoCo or Chaos as the level's `ARammsPhysicsSettings` says. A robot can push the door, and
`simready_open.py` can drive it.

```python
import ramms_worldgen.doors as DR
DR.articulate_level_doors()   # after generation: each leaf standing open beside its opening -> a closed, hinged door
DR.add_ceiling()              # a ceiling slab over each room of the manifest, with ceiling lights
bp = DR.build_door(DR.DOOR_INTERIOR, 90.0)                                  # or one door by hand:
DR.place_door(bp, jamb=(415, -5, 0), along=(0, 1), into=(1, 0), wall_thickness=12)
```

`articulate_level_doors` works from the generator's output:

- It reads each leaf's pose back from its instance.
- The opening is the leaf's width plus 1 cm, and 210 cm high above the room's floor (at most the wall height),
  less any raised threshold the leaf stands on.
- The door takes the leaf's segmentation class (`seg:` tag), which the labeller can't give it because the door is not
  generator output.
- It finds which side of the leaf the opening is on with a trace across the wall, ignoring doors an earlier run
  left in the openings.
- It uses the generator's resolved `WallThickness`.
- It removes the leaf instances.

The doors are tagged `RammsDoors:<generator id>` and `RammsDoorsSeed:<generator id>:<seed>`, with the
generator's `GeneratorId`, which is unique across loaded levels, as its articulated stand-ins are. A run replaces the doors at
the openings it converts and any from another seed (a Regenerate with a new seed), and keeps the rest. So a run
that skipped an opening can be repeated without losing the doors it made. `add_ceiling` (tagged
`RammsCeiling:<generator id>`, each slab `seg:Ceiling`) is replaced on every run. The hinge end of each leaf comes
from the generation palette's `HandleSide` and `RotationOffset`, the way the generator turned the leaf. Leaves in
PCG partition actors are converted when the partition actor holds only this generator's output.

The leaf turns about its corner on the face it opens toward, so it never swings into the jamb. Its collider stops
3 cm short of the floor and 1 cm short of the jambs and head, so Chaos's contact margins do not drag it. The
handle's collider covers only the handle: a proud porthole frame or a hinge knuckle stopped a door at 17 degrees
before that.

The MJCFs go to `RAMMS_SIMREADY_MJCF` (or `C:/Users/waemf/data/simready/mjcf`), beside the SimReady
conversions. The Blueprints, like those conversions, are not committed. `MODEL_REV` is in the names because
URLab cannot replace-import a Blueprint.

`Scripts/worldgen/simready_level.py` exposes both as `SL_DOORS` and `SL_CEILING`. The showcase level
(`simready_showcase.py`, `SC_ENCLOSED`) uses `build_door` / `place_door` and `ceiling_slab`.

## Streets and yards

- **Curbs and ground**: every surface material projects its texture by the
  dominant normal axis (XY on tops, YZ / XZ on faces), so curb faces, slab
  edges and stand-in building shells tile at real size instead of smearing.
  Curbs use their own finer concrete (`Mat.Curb`); stand-in houses use
  world-aligned brick / siding (`Mat.Facade`, `Mat.FacadeAlt`).
- **Entrances**: steps rise from the street side to the door; a ramp, when
  there is room for one, runs along the facade beside the steps with its
  landing against the top step (it never crosses the sidewalk).
- **Planting strips** (`PlantingStrip`): the furniture zone becomes grass.
  Each house lot dresses the strip in front of it - street trees spaced along
  it, a street light every few lots, grass clumps - around its driveway, whose
  apron continues across the strip and sidewalk to the curb. `CanopyChance`
  turns a block's street into big old trees (`TreeLarge`) closer together.
  Paved streets keep trees in pits, the grate scaled to fit the furniture
  zone; trees are placed by their trunk (measured from the mesh), not the
  canopy's centre.
- **Yards**: foundation shrubs along the house front, flowers by the walk, a
  mailbox at the street end of the walk or driveway, a garage driveway, back
  yard fences, grass clumps (`GrassDensity`) over lawns, parks and strips.
- **Houses**: whole houses are the captured `ResidentialHouses` blueprints
  (porch, stairs and front door included); the backdrop-only meshes are not
  used, so every house has a door and steps the front walk meets.
- **Lawns**: ground cover goes out on its own `Grass` pin to a foliage
  spawner (no collision, culled 40-70 m away, no distance-field or decal
  cost): grass clumps plus a dense layer of short tufts (`LawnTuft`) over
  every yard and park lawn, following the terrain.
- **Terrain**: yards get gentle grassy rises (`TerrainVariation` scales
  them; back yards and the open side of deep front yards, clear of the
  house, walk, driveway and planting), and the ground outside the site
  rolls away in large hills.
- **Parks**: besides the loop and cross paths, each quarter of a park gets
  grassy mounds (`Mound` pieces, smooth cosine bumps), boulders (`Rock`),
  a patch of gravel, dirt or mud, and one quarter a pond (water in a mud
  ring, edged with stones); a gravel trail cuts across one quarter and the
  crossing is a cobbled plaza. Trees and grass follow the mounds' height.
- **Wear**: `Decal` pieces project cracks, potholes, patches, manholes and
  puddles onto roads and cracks / stains onto sidewalks (`DecalDensity`;
  palette `Decal.*` material sets, deferred-decal materials only).
- **Street rules** (`StreetRules`, off by default; `PRE_DenseDowntown` has
  them on): the Street Network keeps curb uses before it parks cars - a
  hydrant a few metres from one corner of about one side in three (more on
  long sides) with no parking 4.6 m either side, now and then a bus stop zone
  on a long side, no car within 6 m before a crossing in the direction of
  travel (1.5 m after it) - and parks cars facing their traffic (right-hand:
  the curb on their right). The Hazard Placer puts the hydrant and the bus
  shelter (a bench where a shelter would narrow the route) where they were
  kept, places no other hydrants or shelters, and stands a signal at each
  corner where an intersection crossing (not a mid-block one) meets the side
  (`TrafficSignal`: the entry's front is where its heads face, a mast arm
  runs to its +Y over the road; masts are placed by their pole). None of
  these is placed where it would narrow the route below `MinClearWidth`.
  Off, placement is exactly what it was.
- **Lit signals**: a composite signal entry (parts carrying their own
  materials) goes out with the facade modules, its manifest record (the pole)
  with the features. The Downtown recipe's Downtown West mast is one: its pack
  material renders it black and its texture maps line up with the mesh only
  on the heads and the street sign, so `Ref(materials=...)` gives it
  `surfaces.build_signal` (the pack's maps on the heads and sign, paint
  elsewhere; lamps lit from the lens geometry; the pedestrian head's icon
  projected onto its panel) and `build_paint` on the arm. Every signal in a
  level shows one phase by the axis its heads face: on a level with traffic the
  signal clock cycles it (amber and walk / don't walk included, see "Traffic"),
  otherwise heads facing nearer world X are green, nearer Y red, and each
  pedestrian head the opposite. Camden
  Market's pole signal cycles on its own timer, so the recipe leaves it out.
- **Pack look controls** (palette `CollectionValues`): scalars of a pack's
  material parameter collections, set in the world by the generator (on
  construction, Regenerate, `SetPalette` and BeginPlay: collection values live
  in the world, not in assets; with several generators in a world the last
  one wins, and nothing is reverted; a collection whose pack is not installed
  is skipped). Packs declare them (`collection_values`); the default
  palette and recipes carry their packs'. American City Packs' windows and
  background towers glow by `MPC_Emissive`'s `Building Emissive Intensity`,
  shipped at 800 (night: every window blown out in daylight); its daylight
  value here is 10.
- **Litter** (`LitterDensity`, per 100 m of side): leaves under the street
  trees, leaf drifts and trash in the gutter, manholes in the lanes and
  utility covers on the walk go out on the `Grass` pin (no collision:
  `Litter.Leaves`, `Litter.Gutter`, `Litter.Trash`, `Cover.Manhole`,
  `Cover.Utility`); bags and heaps of rubbish at the curb (`Litter.Bags`) are
  props, solid and in the manifest.
- **Keep-clear areas** (`KeepClear`, `levels.keep_clear(gen, boxes, tag)`):
  world XY boxes on which nothing is placed along the streets - no parked
  car, street tree, furniture, hazard, signal or litter - e.g. a test
  scenario's start or a robot's path. `keep_clear` takes boxes and the bounds
  of actors tagged `RammsKeepClear` and sets the generator's override (with
  no box at all it drops the override, and the preset's boxes apply again).

## Graded streets and rough ground

With the terrain and roughness parameters at 0 (every preset but two) the streets are level and smooth and are
placed exactly as before. Turning any of them on switches the Street Network to the **ground model**
(`FRammsGroundModel`, `Public/PCG/RammsGround.h`) and lays the ground as `GroundPrisms` pieces on it. Its draws come
from random streams of their own, so a seed keeps its blocks, lots, crossings and props: a graded world is the flat
world of the same seed on a hill.

- **Elevation.** Each interval of the street grid climbs at its own grade in `[-MaxStreetGrade, MaxStreetGrade]`
  (columns for the streets along X, rows for those along Y), so intersections sit at different heights and every
  street has a constant grade between them; the lowest street is at the generator's height. The reference height is
  `Z0 + FX(x) + FY(y)`, one plane per grid cell.
- **Roads.** `CurbHeight` under the reference at the gutters, `RoadCrown` higher toward the centre line: each half
  of a segment is one plane. An intersection square is eight planar triangles: the triangle toward an edge carries the
  crown of the road through it, or of the other road where the edge is a curb (no lip anywhere). Markings, crosswalk
  bars, decals and parked cars lie on the road's plane.
- **Curbs.** Each block draws curb heights in `[CurbHeightMin, CurbHeightMax]` at its corners, varying linearly along
  each side (a plane), so a block can have a 6 cm curb on one street and an 18 cm one on the next. Curbs are
  procedural pieces tilted along the curb and across it with the walk.
- **Sidewalks.** The walk rises from the curb top toward the lots at the block's cross slope (`SidewalkCrossSlope`,
  now and then 3-6 % with `NonCompliantCrossSlopeChance`) on top of the hill's own slope, up to the sidewalk's back
  edge; the lots' ground is level with that edge. Corner squares split on their diagonals; streets dropped inside a
  merged block bend the ground along their line, and every piece is split there.
- **Curb ramps** are tilted onto the walk: the run is sized for the ramp's real running slope (rise plus the walk's
  inward slope over the run), recorded `TooSteep` above `RampMaxSlope`, and `CrossSlope` / `CrossSlopeTooSteep` when
  the street's grade (the ramp's and its landing's cross slope) is above `MaxCrossSlope`.
- **Buildings and lots** stand on a level pad at the ground in front of their entrance (so the door meets the walk),
  with a concrete plinth down to the lowest ground under the footprint; yards, walks, driveways, aprons, fences,
  planting, trees, the street furniture and hazards, litter and parks follow the ground (wide furniture stands on the
  lowest ground under its footprint; flat things lie on its slope).
- **Banks.** A raised site gets grassy banks from its outer gutters down to 50 cm under the generator, at 1:3 at the
  steepest, so the street grid has no cliff edge (the example levels' ground slab is 45 cm under the generator).
- **Roughness** is real geometry the chair and the rider feel. With `SurfaceRoughness` (or any density) above 0 the
  walk is cast in panels `PanelLength` long, the furniture zone its own band, each a few millimetres high or low
  (about one in eight 4-16 mm off at 1; panels a curb ramp's pour touches are level with it): lips over
  `VerticalChangeMax` along the route are `Surface/JointLip` records. `HeaveDensity` tilts whole panels (an edge 1-5 cm
  proud), `CrackDensity` splits panels with a 2-15 mm lip, `CoverDensity` sets metal covers into them (raised 4-25 mm
  or sunken 4-30 mm, the panel cut round them), `PavingPatchDensity` relays them in brick (running bond) or granite
  setts with 1.5 cm joints, `RootHeaveChance` lifts the walk beside a street tree in a pit (0.5-4.5 cm). In the road,
  `PotholeDensity` cuts real holes 2.5-9 cm deep (the road is open under them) and `SpeedHumpChance` puts a sinusoidal
  hump (7-10 cm over 3.6-4.2 m) or a table (7.5-9 cm, flat top) across a segment, clear of its crossings. Every
  non-compliant feature carries its own manifest record. Surfaces: bricks `Mat.Brick`, else `Mat.Facade`; setts
  `Mat.Cobble`, else `Mat.Concrete`; covers `Mat.Metal`, else `Mat.Asphalt` (a recipe palette need not have them; the
  Downtown recipe gives `Mat.Metal` Downtown West's dark grey paint).
- **Exact collision.** Every ground piece describes itself to `ARammsProcPiece::AppendCollisionShapes`: a
  `GroundPrisms` part is a box when it is a level rectangle, else a convex hull (its top polygon and the same footprint
  `SizeZ` below its lowest vertex); a hump is a height grid along its profile (2 cm spacing, constant across). Lips are
  vertical faces between separate solids. Tilted curbs, ramps, slabs and mounds are the existing kinds through their
  actor transform.
- **Downstream.** The Street Network writes the model's text on its `Blocks` and `Sidewalks` points (`Ground`);
  Subdivide Lots and Park Layout pass it on; every node parses the same text (`FRammsGroundCache`), so every height
  agrees. On graded ground the Hazard Placer writes each route's ground-following centre line (`RoutePoints`, every
  2 m), which the manifest's `routes[].points` carry, and the crowd lanes take their heights from it.

To get a graded level: `levels.build_level("graded_downtown")` (PRE_GradedDowntown: PRE_Downtown with
`presets.GRADED_DOWNTOWN`, seed 7), `levels.build_level("hilly_residential")` (PRE_HillyResidential: PRE_Suburban
with `presets.HILLY_RESIDENTIAL`, seed 11), or, with traffic and the Downtown recipe (local packs, saved under
`/Game/WorldGen/Maps`), `"graded_traffic"` (streets up to 5 %) and `"hilly_downtown"` (PRE_HillyDowntown: up to 12 %
allowed, 2-7 % with seed 21; the centre intersection is a dip all four streets fall into). `build_all_levels()` leaves
these out unless named. Or set the parameters on any street generator and regenerate (below).
`worldgen_manifest(kind="Grade")` lists every side's grades.

### Adjusting slope, cross slope and curb heights

**Where to set them.**

- **One level (one generator):** select the World Generator, set the value under **Param Overrides** and add the
  parameter's name to **Override Keys** (only listed names apply), then **Regenerate**. From Python:
  `gen.set_param("MaxStreetGrade", 0.08)` (adds the key for you), then `gen.regenerate()`; from an AI assistant,
  `worldgen_set_param`; from Remote Control, `SetParamsJson`. Overrides are saved with the level and leave its
  preset alone; on a level with crowd lanes or traffic, run `levels.finish_level(gen)` after regenerating (or
  rebuild the level) so the lanes follow the new ground.
- **A preset (every level using it):** edit its entry in `Content/Python/ramms_worldgen/presets.py` (the graded ones
  are a base preset plus a dict such as `GRADED_DOWNTOWN`), run `build_all.build("presets")`, then rebuild the levels
  (`levels.build_level(kind)`).

**What each one does.** Slopes are fractions (rise / run: 0.05 = 5 %), heights cm.

| To change | Parameter | Range | How it is used |
|---|---|---|---|
| How steep streets are | `MaxStreetGrade` | 0..0.15 | Each interval of the street grid (between two street lines) draws its grade uniformly in ±`MaxStreetGrade`, separately along X and along Y, so intersections end up at different heights and each street has one grade between intersections, changing at them. It is a bound, not a target: a small site has few intervals, so its steepest street is often well under it (0.05 gives 2.9 % in `graded_traffic`, 0.12 gives 7 % in `hilly_downtown`). Change `Seed` for other grades. |
| How the road falls to its gutters | `RoadCrown` | 0..0.08 | Each half of a road falls from the centre line at this slope (0.02 is typical). |
| How the walk falls toward the curb | `SidewalkCrossSlope` | 0..0.08 | Each block's walks rise from the curb top toward the lots at 0.6-1 times this (0.02 or less is compliant: `MaxCrossSlope`). |
| Some steeper walks | `NonCompliantCrossSlopeChance` | 0..1 | That share of blocks instead draws a cross slope from max(`MaxCrossSlope` + 0.01, 0.03) to max(3 × `MaxCrossSlope`, 0.06). |
| Curb heights | `CurbHeight`, `CurbHeightMin`, `CurbHeightMax` | cm | With `CurbHeightMax` 0 every curb is `CurbHeight` (15 by default). Otherwise each block draws a curb height in [`CurbHeightMin`, `CurbHeightMax`] that also varies linearly along each of its sides, so one block can have a 6 cm curb on one street and 18 cm on the next. |
| What counts as non-compliant | `MaxCrossSlope` (0.02), `RampMaxSlope` (0.0833), `VerticalChangeMax` | | Thresholds the manifest's compliance records use; they do not shape the ground. |

**Worth knowing.**

- A walk's total cross slope is its own (`SidewalkCrossSlope`) plus the hill's grade across its street: every grid cell
  is one plane, so a walk along a north-south street on a block whose east-west grade is 5.6 % falls about 9-10 %
  toward the curb with `SidewalkCrossSlope` 0.035. The manifest's `Grade/Cross` records give the total, `Grade/Running`
  the grade along the side.
- The ground model switches on as soon as any terrain or roughness parameter is above 0; with all of them at 0 the
  world is the flat one, placed exactly as before. A graded world keeps its seed's blocks, lots, crossings and props.
- Curb ramps are sized for the real running slope on a grade and recorded `TooSteep` above `RampMaxSlope`, so steeper
  streets and walks give more non-compliant ramps (as they do in real cities).
- Grades change only at street lines (intersections, and where a dropped street bends the ground inside a merged
  block), at curb ramps and at the site's banks: there are no vertical curves or grade breaks part way along a block.

Example, steeper hills and higher curbs on one level, from the editor's Python console:

```python
import unreal
gen = unreal.GameplayStatics.get_all_actors_of_class(unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world(), unreal.RammsWorldGenerator)[0]
for name, value in (("MaxStreetGrade", 0.10), ("RoadCrown", 0.02), ("SidewalkCrossSlope", 0.02),
                    ("NonCompliantCrossSlopeChance", 0.2), ("CurbHeightMin", 10.0), ("CurbHeightMax", 20.0)):
    gen.set_param(name, value)
gen.regenerate()
# when generation is done (not gen.is_generating()):
#   import ramms_worldgen.levels as L; L.finish_level(gen)   # crowd lanes (and traffic) on the new ground
```

## Indoor furnishing and routes

Walls are finished per room: a 1 cm liner in the room's paint (`Mat.Wall.Paint`,
tinted instances of `M_RammsPaint`), wallpaper (`Mat.Wall.Paper`) or tile below
120 cm (`Mat.Wall.Tile`, bathrooms and some kitchens), cut around doors and
windows, with a skirting board, so the two sides of a shared wall differ.
Floors are picked per room from `Mat.Floor.Wood / Carpet / Tile / Vinyl` and
meet under the wall centre lines (no overlap, no z-fighting). Exterior walls
get windows (frame, sill, mullion and glass in the wall's openings). Doors
are real leaves from `Door.Interior` / `Door.Exterior` (handles included,
turned so the handle is on the free edge), scaled to the opening and standing
open against the casing; casings and frames stand 2 cm proud of the wall so
nothing shares a plane.

A shop floor is laid out like a small supermarket: checkout lanes standing
out from the entrance wall (each lane a `CheckoutLane` record with its
width), baskets and an exit sign by the door, wall freezers along one long
wall, produce tables with crates of fruit and vegetables along the other, a
deli counter across the far end, gondola rows with end-cap displays and a
hanging sign over each aisle, and clutter in the corners.

Rooms are furnished the way a person would arrange them, in groups: a sofa
with its coffee table, armchair and a TV on a stand opposite; a bed with
night tables and lamps; desks in a row along the long wall with a chair at
each; a table in the middle with chairs on every side; a counter of
cupboards in a kitchen; a toilet, bathtub and vanity with a mirror in the
bathroom; a picture on the wall. Offices are dense: desks along both long
walls and, in a deep room, a back-to-back bench down the middle, each desk
with a chair, a monitor and a keyboard.

Every door, including the deliberately narrow (non-compliant) ones, is at
least `MinPassableDoorWidth` wide (80 cm: the RAMMP Gen1 is about 72 cm across),
and the open leaf stands outside the opening so it does not eat the clear
width.

Bathrooms are kept to a real size: a home bathroom longer than ~3.2 m is cut
into two bathrooms (a closet takes a third piece). In a workplace or shop the
bathroom is a restroom: a run of 90 cm toilet stalls (partitions on legs, an
open door each) with a 155 cm accessible stall at the end away from the
entrance, and sinks, a mirror and a hand dryer on the opposite wall; a very
long one becomes two restrooms. `OfficeFraction` turns a home (living rooms, bedrooms) into a
workplace (offices, meeting rooms, a break room).

Every piece is a footprint box in its mesh's *facing frame* (the palette
builder detects the front of seating, beds and toilets from where their
upper vertices gather, and stores the footprint and pivot offset in that
frame; `FACING_FIX` in `palettes.py` corrects the meshes the heuristics get
wrong, checked with `Scripts/worldgen/lineup.py`, which lines a slot's meshes
up facing a camera). A piece is placed only if its box lies inside the room's inner
rectangle, overlaps nothing placed before it, and stays out of every door's
swing and approach strip, so furniture never penetrates a wall, a door or
another piece. The verification pass checks that against the spawned
instances (`collisions` in the report).

Routes are then found through the furnished room on a 10 cm occupancy grid:
a distance transform gives each free cell its clearance, a widest-path
search from each door finds the route with the most room, and the shortest
path at that width is the polyline the manifest carries. Its bottleneck,
doubled, is the route's clear width; the pieces at the pinch point are the
ones reported as `Hazard` / `ObstacleInRoute`. With `NonCompliance` > 0 a
chair, cart or plant is deliberately pulled into an aisle.

## Pedestrians (RammsCrowd)

After a level is generated, `ramms_worldgen.crowd.build_crowd` (run by
`levels.finish_level`) lays a ZoneGraph network for the RammsCrowd plugin
(Mass + ZoneGraph) along the manifest's routes:

- outdoors, sidewalk sides and park paths are cut where they cross into a
  graph and dangling corner stubs are dropped. Each `Crosswalk` feature joins
  the two sidewalks it connects: a run from one sidewalk's centre line
  straight to the curb (over the curb ramp), along the zebra, and up the far
  side;
- indoors, every room's door-to-door routes (already bent around the
  furniture) are linked through the doorways, and a shop's aisles become a
  ladder tied to the door routes.

Agents follow lane centre lines and Mass avoidance only steers around other
agents, so the lanes themselves are kept clear of geometry: every run is
tested with a person-sized capsule against everything that blocks (walls,
furniture, shelving, props, cars); doorway crossings are straightened
through the middle of the opening, run ends are nudged off furniture, and a
run that hits something is rerouted around it on a 25 cm grid (A*, then
string-pulled), or dropped when there is no way through. Outdoors, a run
whose straight line keeps to one walking level (a sidewalk, a park path) is
rerouted at that level's height only: around a tree or a bench, never off the
curb and down the street (the capsule starts 30 cm up, so the curb's drop
would otherwise read as free). A level is a height, not a surface: a detour
may cross a planting strip, a lawn or a front yard that lies at the sidewalk's
height. The ground must not drop more than 60% of the curb height (at most
8 cm) below the run, under the walker's centre and out to the outer edge of
either lane's walkers, whatever clearance the detour squeezes through with, so
neither lane hangs over the road; ground more than 8 cm higher under the
centre (a planter) is an obstacle. A run end nudged off a prop stays on its
level the same way. Crossings, which go down to the road by design, are not
constrained, nor are streets whose curb is under 4 cm (no level to keep). A
run with no way through on its level is dropped rather than sent down the
street. Junctions shrink to fit the free space around them.
`crowd.check_lanes()` audits a level (verify.py fails a level whose lanes
touch geometry), and `crowd.check_lane_levels()` counts sidewalk-lane samples
whose walkers would stand off their level (crossings, junctions and indoor
lanes aside): `off_level_lane_samples` in `build_crowd`'s summary, 0 when every
lane keeps to its sidewalk.

Every run is sampled about every 50 cm onto the surface below it, so lanes
follow curb ramps, the drop to the road, path slopes and thresholds instead
of floating in a straight line between their ends. On graded ground (any terrain
or roughness parameter above 0) each node takes its height from its route's
ground-following polyline, a run is cut where the route's grade bends (so a
straight run keeps to one slope), and "one walking level" is the walk's plane
rather than one height: a nudged run end keeps to the plane through it, a rerouted
run to its own grade and the walk's cross slope across it, and the level audit
measures the walkers' edges against the cross slope at each sample (slopes are
medians of 20 cm steps over 80 cm, so a curb edge or a panel's lip does not tilt
them). Planes are fitted to the ground along each run, the manifest's curb ramps
count as the walk (a graded ramp, sized for its real slope, can reach under the
route's centre line, so a crossing's end stands on it), and a rerouted run's points
take the plane's heights. A curb is still a drop off that plane; the tolerance uses
the lowest curb (`CurbHeightMin`). On flat streets the tests are the original ones.

Branch nodes become polygon `ZoneShape` junctions, the runs between them
spline `ZoneShape`s on the `PedestrianLanesSidewalk` profile (2 x 60 cm)
outdoors or `PedestrianLanesNarrow` (2 x 25 cm, one person each way through
an 80 cm door) indoors (`Config/DefaultPlugins.ini`).
The ZoneGraph is rebuilt and a `RammsCrowdSpawner` in ZoneGraphLanes mode is
placed with `/Game/NPCs/DA_CrowdAgentProfile` (about one pedestrian per 9 m
of lane outdoors, per 8 m indoors). Everything sits in the `WorldGen/Crowd`
outliner folder with the tag `RammsCrowd`, so a rebuild replaces it. The
ZoneGraph is built in the editor, so the crowd comes with levels generated
and saved there (the example levels), not with runtime regeneration.

## Traffic (RammsCrowd)

Moving cars on the generated streets, with RammsCrowd's lightweight Mass traffic (its README, "Traffic").
`ramms_worldgen.traffic.build_traffic` (run by `levels.finish_level` after the crowd on a traffic level) turns the
street network's `Road` and `Intersection` manifest records into ZoneGraph vehicle lanes:

- each road segment becomes a spline `ZoneShape` on the `VehicleLanes` profile (2 x 320 cm, lane centres 160 cm
  off the centreline: room beside the parked cars of an 11 m street; `VehicleLanesNarrow`, 2 x 250 cm, on roads
  under 10 m), right-hand traffic, from the stop line at one end to the stop line at the other. It is sampled
  every 1.5 m onto the generated road surface, each sample traced from the previous one's height, so the lanes
  follow grades, crowns and humps; the cars also trace their axles near the robot.
- each intersection becomes a polygon `ZoneShape`: a lane-profile entry on every arm at its stop line (70 cm
  beyond the arm's crosswalk, where the street network paints it; 1.5 m beyond the square without one), the
  outline along the arms' curb lines and round the square's corners, arc routing with an inner radius that puts a
  right turn's centre on the curb corner (and a left turn's near the opposite one). ZoneGraph adds the turning
  lanes, never back the way they came. Three- and four-way intersections carry the ZoneGraph tag `Intersection`
  (all-way stop), and `Signalized` too when a `TrafficSignal` record stands at their corners (the corner signals
  of `StreetRules`); a two-road bend is a plain polygon (free flow).
- the crowd's street crossings at the signalized intersections get the ZoneGraph tag `Crossing`
  (`crowd.tag_crossings`; `build_crowd` does it too when the level already has traffic): MassCrowd closes them
  until the signal clock opens them for Walk, and the crowd waits at the curb. Mid-block crossings and crossings
  at all-way stops stay open; the cars yield there.
- an `ARammsTrafficSignalClock` (one phase for every signal: 20 s green, 3.5 s amber, 1.5 s all red per axis;
  Walk with the parallel green, then 8 s of Flashing Don't Walk) reading `/Game/WorldGen/Traffic/MPC_RammsTrafficSignals`
  (built by `traffic.signal_collection()`), and an `ARammsTrafficSpawner` with the palette's `Car` meshes (their
  palette offsets: cars face the way they drive), about one car per 45 m of lane (6..60), speed limit 10 m/s, the
  generator's seed.

The traffic signal material (`surfaces.build_signal`, built into a recipe's folder) reads the clock's collection:
per head axis `GreenX` / `AmberX` / `RedX` (heads facing nearer world X) and `...Y`, and `WalkX` / `HandX`,
`WalkY` / `HandY` for the pedestrian heads on those poles. Its resting values are the old fixed look (X green, Y
red), so a level without a clock looks as before; a signal material built outside `/Game` keeps the `GreenAlongX`
scalar. Downtown West's amber lens positions are estimated (between the red and green lenses). After pulling this,
`build_all.build("traffic")` builds the collection and rebuilds the recipes' signal materials.

`levels.build_level("traffic")` makes `/Game/WorldGen/Maps/Map_Gen_Traffic` (`PRE_DowntownTraffic`, Downtown recipe,
seed 21): the dense downtown with 11 m streets, a 120 m site and signals. Any street level gets traffic with
`build_level(kind, traffic=True)` or `traffic.build_traffic(gen)` after `crowd.build_crowd(gen)`; `build_traffic`'s
summary lists the intersections (signalized, all-way stop, bends), lane metres, cars, tagged crossings and
`parked_in_lane`: parked cars a moving car would brush (an 8 m street with parking on both sides has no room for
two lanes, which is why the dense downtown preset stays without traffic). Everything sits in the `WorldGen/Traffic`
outliner folder with the tag `RammsTraffic`. Not yet: MuJoCo does not see the cars, no per-intersection signal
offsets, wheels do not turn.

## Design notes and limits

- Streets are an axis-aligned jittered grid with pairwise block merges; no
  diagonals or curves yet. Buildings are shells (no interiors); the robot does
  not enter them. Indoor and street worlds are separate presets, not one
  continuous world — a doorway from a street into a generated interior is the
  natural next step (same pieces, a `Doorway` node).
- Curb ramps are perpendicular ramps cut into the sidewalk at every crossing
  that joins two blocks: the ramp's top is the walking surface (the sidewalk
  slab and planting strip are cut around it and its flares, and the curb is
  open there), with flares warped from road level at the ramp to full curb
  height at their outer edge. Crossings near an intersection are set back
  from the corner far enough for both corners' ramps and flares to fit.
  A ramp runs at `RampMaxSlope` when the sidewalk leaves a 90 cm landing behind
  it; on a narrower sidewalk it is shortened (no shorter than half the
  sidewalk), recorded as `TooSteep` and the crossing as `SteepRamp`. Where the
  cut is deeper than the ramp (a planting strip), a level walk fills the rest.
  Lane markings stop at a midblock crossing, which gets its own stop lines;
  parked cars, street-side props and park benches keep clear of crossings,
  ramps and path junctions.
  Streets are level and sidewalks flat unless the terrain parameters say otherwise
  (see [Graded streets and rough ground](#graded-streets-and-rough-ground)).
- Hazard meshes stand in: an info board plays the A-frame, railings play
  barriers, the raised-cobble tree pit plays the heaved slab. Scooters have no
  mesh and use a low planter piece until a pack provides one.
- Trees and shrubs are procedural (a tapered trunk, stub branches and a
  canopy of overlapping spheres in solid foliage / bark materials made by the
  palette builder); a `Tree` / `Shrub` palette slot swaps in real meshes.
  Buildings get the palette's modular facade kits (ground floor with a door
  module, window floors, cornice, corner pieces, one material pick per
  category per building) or, without a kit, storefronts, floor trim,
  cornices, parapets and framed windows; rooftop units come from the
  `RoofUnit` slot; set-back one/two-storey buildings get a house mesh from
  the `House` slot or a gable roof piece. Street parking (`ParkingDensity`)
  puts cars along the curbs. Roads carry a dashed centre line, edge lines and stop
  lines in solid marking materials.
- Everything generated collides (block-all): the spawners' instances, and
  every palette mesh is given usable collision when the palette is built. A
  mesh keeps the simple collision it ships with; one with none collides
  against its own triangles (complex as simple), a tree on a 50 cm box around
  its trunk (so a canopy does not block); grass and flowers get none. A tree
  whose own collision is a crown hull (wider than 1.5 m, or reaching well
  above the trunk box: imports and packs can wrap the whole crown, which
  blocks a sidewalk edge to edge) gets the trunk box instead, but only when
  its trunk can be measured (one trunk at the centroid of its lowest 60 cm,
  not several stems, foliage or a wide base at the ground); otherwise the hull
  stays and the build lists it. A tree never collides complex-as-simple.
  - Palette builds may change the collision of your local copies of pack
    meshes (Fab packs, UrbanVerse and SimReady imports), and save them. Those
    copies are never committed: another checkout gets the same changes from
    its own palette build, and re-downloading a pack (or re-importing an
    asset) undoes them; local copies an earlier build changed stay changed
    until then. Every change
    is logged and listed in the build's report (`modified_pack_assets`, also
    `palettes.MODIFIED_PACK_ASSETS`), with what lacks usable collision
    (`missing_collision`). Set `palettes.FIX_PACK_COLLISION = False` before a
    build to change nothing and only report.
  - Instances already placed in an open level keep their old collision until
    the level is regenerated or reloaded (Python cannot refresh them), so after
    a palette build that changed collision, regenerate the level
    (`levels.build_level` does) before `crowd.build_crowd`.

  Generated geometry is Unreal collision. The Chaos backend drives on it
  directly; MuJoCo gets the same world through the [collision shapes](#collision-shapes)
  (RammsMujocoSupport exports them as world geoms). The Newton backend does not
  read them yet.
- Pieces are one actor each (walls, ramps, doors, trees). A dense downtown
  spawns a few hundred; fine in the editor, and `Collapse` in the Spawn Actor
  node is the lever if it ever matters.
- Pedestrians are not placed by the generator; drop a `RammsCrowdSpawner`
  over the sidewalks as in `Map_CrowdTest`.

## First-compile checklist

The PCG side has been checked against the UE 5.8 engine headers
(`Engine/Plugins/PCG`): pin construction (`EPCGDataType` converts implicitly
to `FPCGDataTypeIdentifier`), the `UPCGBasePointData` write path
(`SetNumPoints` / `AllocateProperties` / value ranges, `EPCGPointNativeProperties::All`),
`FPCGContext::NewPointData_AnyThread`, metadata create/read, `UPCGSplineData::Initialize`,
the component API (`GenerateLocal(bool)`, `CleanupLocal(bool)`,
`GetGeneratedGraphOutput`, `OnPCGGraphGeneratedDelegate`), the settings
virtuals, `PCGE_LOG`, and the graph Input/Output node pin labels (`In` / `Out`,
params pin `Overrides`). `PCGParamData.h` lives at the module root in 5.8,
not under `Data/`, and the includes reflect that.

Not yet checked against headers (engine `GeometryScripting` folder):

| Symptom | Where | Note |
|---|---|---|
| `SetMeshUVsFromBoxProjection` / `AppendSphereBox` / `AppendCylinder` signature | `RammsProcPiece.cpp` | stable since 5.1; float casts are in place |
| `UDynamicMeshComponent::UpdateCollision` / `SetComplexAsSimpleCollisionEnabled` arity | `RammsProcPiece.cpp: ApplyCollision` | |
| C4244 double→float elsewhere | anywhere flagged | add a cast |

Python-side risks fail loudly at build time: `_graph.py` validates every pin
label against the live node and raises with the labels that exist, and the
palette builder reports every slot it could not fill.
