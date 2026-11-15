"""Vehicle lanes, a signal clock and moving traffic for a generated street level (RammsCrowd's Mass traffic).

After generation, and after crowd.py (whose street crossings this ties to the signals), the street network's Road
and Intersection records (manifest features) become ZoneGraph shapes:

- each road segment is a spline shape with the VehicleLanes profile (two-way, right-hand: lane 0 forward on the
  shape's right; VehicleLanesNarrow on roads under 10 m), from the stop line at one end to the stop line at the
  other, sampled every 1.5 m onto the generated road surface (a line trace from the previous sample's height), so
  the lanes follow grades, crowns and humps;
- each intersection is a polygon shape: a lane-profile entry on each arm at its stop line (just beyond the arm's
  crosswalk, where the street network paints the line), an outline along the arms' curb lines and round the
  square's corners, and arc routing whose inner radius puts a right turn's centre on the curb corner. ZoneGraph
  joins the entries with turning lanes (never back the way they came). Where three or four roads meet the
  polygon carries the Intersection tag (stop or signal control), and Signalized too when a TrafficSignal record
  stands at its corners; a two-road bend is a plain polygon (free flow).

The crowd's street crossings at the signalized intersections get the ZoneGraph Crossing tag (crowd.tag_crossings:
MassCrowd closes them until the signal clock opens them for Walk), the ZoneGraph is rebuilt, and two actors are
placed: an ARammsTrafficSignalClock reading MPC_RammsTrafficSignals (built here) and an ARammsTrafficSpawner with
the palette's Car meshes. The signal materials of the level's palette recipe are rebuilt so they read the
collection (amber and walk / don't-walk included).

    import ramms_worldgen.traffic as T
    T.build_traffic(H.find_generator())       # editor only; save the level afterwards

Every actor made here carries the tag RammsTraffic and sits in the WorldGen/Traffic outliner folder, so a rebuild
replaces it.
"""
import math

import unreal

from . import _helpers as H
from . import crowd as C

TAG = "RammsTraffic"
SIGNALIZED_TAG = "RammsTrafficSignalized"   # actor tag on signalized intersection shapes (crowd.py looks for them)
LEVEL_TAG = "RammsTrafficLevel"             # generator actor tag: levels.finish_level builds traffic for that level
FOLDER = "WorldGen/Traffic"
SPAWNER_CLASS = "/Script/RammsCrowd.RammsTrafficSpawner"
CLOCK_CLASS = "/Script/RammsCrowd.RammsTrafficSignalClock"
SIGNAL_MPC = "/Game/WorldGen/Traffic/MPC_RammsTrafficSignals"
# Lane profiles from Config/DefaultPlugins.ini: (name, guid, lane width). Two lanes, no spacer: each lane's centre is
# half a lane off the road's centreline.
LANE_PROFILES = {
    "wide": ("VehicleLanes", "9C4E2B7D5A1F4E6B8D3C0A9B7E5F1D2C", 320.0),
    "narrow": ("VehicleLanesNarrow", "6E1F3A8B2C4D4F5A9B7C0D1E2F3A4B5C", 250.0),
}
WIDE_ROAD = 1000.0              # roads this wide (cm) or wider take the wide profile (room beside parked cars)
SAMPLE_STEP = 150.0             # lane points along a road, cm
STOP_BEYOND_CROSSWALK = 70.0    # stop line centre beyond a crosswalk's far edge from the intersection (painted 55 cm out, 30 wide)
STOP_NO_CROSSWALK = 150.0       # without a crosswalk on an arm, the stop line this far beyond the intersection square
MIN_ROAD_LANE = 400.0           # shortest road spline left between two stop lines
SIGNAL_RADIUS = 2500.0          # a TrafficSignal record this close to an intersection's centre (and nearest it) signalizes it
CAR_HALF_WIDTH = 100.0          # for the parked-car check: a moving car's half width
ZG_VEHICLE, ZG_INTERSECTION, ZG_SIGNALIZED, ZG_CROSSING = "Vehicle", "Intersection", "Signalized", "Crossing"


# --- geometry -------------------------------------------------------------------------------------

def _sub(a, b): return (a[0] - b[0], a[1] - b[1])
def _add(a, b): return (a[0] + b[0], a[1] + b[1])
def _mul(a, k): return (a[0] * k, a[1] * k)
def _dot(a, b): return a[0] * b[0] + a[1] * b[1]
def _cross(a, b): return a[0] * b[1] - a[1] * b[0]
def _len(a): return math.hypot(a[0], a[1])


def _roads_and_nodes(features):
    """Road records -> roads, Intersection records -> nodes, each road's two end nodes and each node's arms
    (road index, unit direction from the node into the road)."""
    roads, nodes = [], []
    for f in features or ():
        loc, size, rot = f.get("location", {}), f.get("size", {}), f.get("rotation") or {}
        if f.get("kind") == "Road":
            yaw = math.radians(rot.get("yaw", 0.0))
            along_x = abs(math.cos(yaw)) > 0.7
            roads.append(dict(c=(loc.get("x", 0.0), loc.get("y", 0.0)), z=loc.get("z", 0.0), d=(1.0, 0.0) if along_x else (0.0, 1.0),
                              length=size.get("x", 0.0), width=size.get("y", 0.0), along_x=along_x))
        elif f.get("kind") == "Intersection":
            nodes.append(dict(c=(loc.get("x", 0.0), loc.get("y", 0.0)), z=loc.get("z", 0.0), width=size.get("x", 0.0),
                              kind=f.get("subtype", ""), arms=[]))

    def node_at(p):
        best = None
        for i, n in enumerate(nodes):
            d = _len(_sub(n["c"], p))
            if d < 80.0 and (best is None or d < best[0]):
                best = (d, i)
        return best[1] if best else None

    for ri, r in enumerate(roads):
        half = r["length"] * 0.5 + r["width"] * 0.5
        r["a"] = node_at(_sub(r["c"], _mul(r["d"], half)))
        r["b"] = node_at(_add(r["c"], _mul(r["d"], half)))
        if r["a"] is not None:
            nodes[r["a"]]["arms"].append(dict(road=ri, u=r["d"]))
        if r["b"] is not None:
            nodes[r["b"]]["arms"].append(dict(road=ri, u=_mul(r["d"], -1.0)))
    return roads, nodes


def _crosswalks(features):
    """[(centre, width along the road)] of every Crosswalk record."""
    out = []
    for f in features or ():
        if f.get("kind") == "Crosswalk":
            loc, size = f.get("location", {}), f.get("size", {})
            out.append(((loc.get("x", 0.0), loc.get("y", 0.0)), size.get("y", 300.0)))
    return out


def _stop_distance(node, arm, crosswalks):
    """From the node's centre along the arm to its stop line: just beyond the nearest crosswalk on the arm, else a
    little beyond the intersection square."""
    h = node["width"] * 0.5
    best = None
    for q, cw in crosswalks:
        rel = _sub(q, node["c"])
        along, across = _dot(rel, arm["u"]), abs(_cross(arm["u"], rel))
        if across < h and h * 0.5 <= along <= h + 2500.0 and (best is None or along < best[0]):
            best = (along, cw)
    if best:
        return best[0] + best[1] * 0.5 + STOP_BEYOND_CROSSWALK
    return h + STOP_NO_CROSSWALK


def _surface_z(world, x, y, z_ref):
    """The road surface at (x, y), searched from well above: the first upward-facing blocking hit going down (an
    intersection's middle has nothing over it). z_ref when there is none."""
    s, e = unreal.Vector(x, y, z_ref + 3000.0), unreal.Vector(x, y, z_ref - 3000.0)
    hits = unreal.SystemLibrary.line_trace_multi(world, s, e, unreal.TraceTypeQuery.TRACE_TYPE_QUERY1, False, [],
                                                 unreal.DrawDebugTrace.NONE, True)
    if isinstance(hits, tuple):
        hits = hits[-1] if hits else []
    for hit in hits or []:
        t = hit.to_tuple()
        vecs = [v for v in t if isinstance(v, unreal.Vector)]
        if len(vecs) >= 4 and vecs[3].z > 0.7:      # (location, impact point, normal, impact normal, ...)
            return vecs[1].z
        if vecs and len(vecs) < 4:
            return vecs[0].z
    return z_ref


def _follow(world, a, za, b, step=SAMPLE_STEP):
    """(x, y, z) about every `step` cm from a to b, each traced onto the ground from the previous sample's height."""
    L = _len(_sub(b, a))
    n = max(1, int(math.ceil(L / step)))
    out, z = [], za
    for k in range(n + 1):
        t = k / n
        x, y = a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t
        z = C._ground_z(world, x, y, z)
        out.append((x, y, z))
    return out


# --- level actors ---------------------------------------------------------------------------------

def _clear():
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    old = [a for a in eas.get_all_level_actors() if a.actor_has_tag(TAG)]
    if old:
        eas.destroy_actors(old)
    return len(old)


def _profile_ref(kind):
    name, guid_s, width = LANE_PROFILES[kind]
    ref = unreal.ZoneLaneProfileRef()
    ref.set_editor_property("name", unreal.Name(name))
    guid, ok = unreal.GuidLibrary.parse_string_to_guid(guid_s)
    if ok:
        ref.set_editor_property("id", guid)
    return ref, width


def _shape(points, shape_type, profile_ref, label, zone_tags=(), actor_tags=(), arcs=False):
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actor = eas.spawn_actor_from_class(unreal.ZoneShape, unreal.Vector(0, 0, 0))
    actor.set_actor_label(label)
    actor.tags = [unreal.Name(TAG)] + [unreal.Name(t) for t in actor_tags]
    actor.set_folder_path(unreal.Name(FOLDER))
    comp = actor.get_component_by_class(unreal.ZoneShapeComponent)
    comp.set_shape_type(shape_type)
    comp.set_editor_property("lane_profile", profile_ref)
    if arcs:
        comp.set_polygon_routing_type(unreal.ZoneShapePolygonRoutingType.ARCS)
    comp.set_editor_property("points", points)
    if zone_tags:
        unreal.RammsCrowdZoneGraphLibrary.set_zone_shape_tags(comp, [unreal.Name(t) for t in zone_tags], False)
    unreal.RammsCrowdZoneGraphLibrary.update_zone_shape(comp)
    return actor


def signal_collection():
    """MPC_RammsTrafficSignals: the scalars the signal clock drives (created or refreshed, saved). None when
    RammsCrowd's editor module is not loaded."""
    lib = getattr(unreal, "RammsTrafficEditorLibrary", None)
    if lib is None:
        H.warn("traffic: RammsTrafficEditorLibrary missing (RammsCrowd editor module); no signal collection")
        return None
    H.ensure_dir(SIGNAL_MPC.rsplit("/", 1)[0])
    mpc = lib.ensure_signal_parameter_collection(SIGNAL_MPC)
    if mpc is not None:
        H.save(mpc)
    return mpc


def _recipe_of(palette_path):
    """'Downtown' for /Game/WorldGen/Palettes/PAL_Downtown, else None."""
    from . import recipes as R
    prefix = R.OUT + "/Palettes/PAL_"
    p = (palette_path or "").split(".")[0]
    return p[len(prefix):] if p.startswith(prefix) else None


def refresh_signal_materials(recipe):
    """Rebuild a palette recipe's traffic signal materials (surfaces.build_signal) so they read the signal collection.
    Returns the material paths rebuilt (only those the recipe built: their packs are installed)."""
    from . import packs as PK
    from . import recipes as R
    from . import surfaces as SF
    out = []
    if not recipe:
        return out
    directory = R.materials_dir(recipe)
    for pack in PK.ALL:
        for key, spec in pack.surfaces.items():
            if spec.get("kind") != "signal":
                continue
            name = "M_%s_%s_%s" % (recipe, pack.name.replace("_", ""), key)
            if H.EAL.does_asset_exist(directory + "/" + name):
                if SF._build(name, pack.surface_spec(key), directory=directory) is not None:
                    out.append(directory + "/" + name)
    return out


def _car_meshes(palette):
    """The palette's Car slot as RammsTrafficCarMesh entries (mesh, weight, footprint, height, placement)."""
    out = []
    slot = palette.get_editor_property("slots").get(unreal.Name("Car")) if palette else None
    for e in (slot.get_editor_property("meshes") if slot else []):
        mesh = e.get_editor_property("mesh")
        if mesh is None:
            continue
        m = unreal.RammsTrafficCarMesh()
        m.set_editor_property("mesh", mesh)
        m.set_editor_property("weight", float(e.get_editor_property("weight")))
        m.set_editor_property("footprint", e.get_editor_property("footprint"))
        m.set_editor_property("height", float(e.get_editor_property("height")))
        m.set_editor_property("location_offset", e.get_editor_property("location_offset"))
        m.set_editor_property("rotation_offset", e.get_editor_property("rotation_offset"))
        m.set_editor_property("scale", float(e.get_editor_property("scale")))
        out.append(m)
    return out


def build_traffic(gen, count=None, speed_limit=1000.0, seed=None, timing=None):
    """Lay vehicle lanes along the generated level's roads, tie the crowd's crossings at signalized intersections to
    the signals, rebuild the ZoneGraph and place a signal clock and a traffic spawner. `count` None: about one car per
    45 m of lane (6..60); `speed_limit` cm/s; `seed` None: the generator's; `timing` a dict of FRammsSignalTiming
    fields (green_seconds_x, amber_seconds, ...). Returns a summary dict. Editor only; the caller saves the level."""
    m = H.manifest_to_dict(gen)
    features = m.get("features", [])
    removed = _clear()
    roads, nodes = _roads_and_nodes(features)
    if not roads:
        H.warn("traffic: the manifest has no Road records (regenerate with the current street network)")
        return {"roads": 0, "removed": removed, "note": "no Road records"}
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    crosswalks = _crosswalks(features)
    signals = [(f["location"]["x"], f["location"]["y"]) for f in features
               if f.get("kind") == "Prop" and f.get("subtype") == "TrafficSignal" and f.get("location")]
    width = max(r["width"] for r in roads)
    kind = "wide" if width >= WIDE_ROAD else "narrow"
    ref, lane_w = _profile_ref(kind)
    T = unreal.FZoneShapePointType
    warnings = []

    # Each node: its surface height, which arms it has, where their stop lines are (traced along the arm), signals.
    for ni, n in enumerate(nodes):
        n["zs"] = _surface_z(world, n["c"][0], n["c"][1], n["z"])
        n["signalized"] = False
    for sx, sy in signals:
        near = min(range(len(nodes)), key=lambda i: _len(_sub(nodes[i]["c"], (sx, sy)))) if nodes else None
        if near is not None and _len(_sub(nodes[near]["c"], (sx, sy))) < SIGNAL_RADIUS and len(nodes[near]["arms"]) >= 3:
            nodes[near]["signalized"] = True
    for n in nodes:
        for arm in n["arms"]:
            arm["d"] = _stop_distance(n, arm, crosswalks)
    # Two stop lines on one road must leave some road between them.
    for ri, r in enumerate(roads):
        arms = [a for ni in (r["a"], r["b"]) if ni is not None for a in nodes[ni]["arms"] if a["road"] == ri]
        total = r["length"] + r["width"]
        if len(arms) == 2 and arms[0]["d"] + arms[1]["d"] > total - MIN_ROAD_LANE:
            each = max(r["width"] * 0.5 + 50.0, (total - MIN_ROAD_LANE) * 0.5)
            for a in arms:
                a["d"] = min(a["d"], each)
            warnings.append("road at (%.0f, %.0f): stop lines pulled in to %.0f cm from the intersections" % (r["c"][0], r["c"][1], each))
    for n in nodes:
        for arm in n["arms"]:
            p = _add(n["c"], _mul(arm["u"], arm["d"]))
            arm["p"] = _follow(world, n["c"], n["zs"], p)[-1]

    shapes = 0
    lane_cm = 0.0
    # Roads: from the stop line at end a to the one at end b, along the centreline, on the road surface.
    for ri, r in enumerate(roads):
        d = r["d"]
        half = r["length"] * 0.5 + r["width"] * 0.5
        end_a = next((a["p"] for a in nodes[r["a"]]["arms"] if a["road"] == ri), None) if r["a"] is not None else None
        end_b = next((a["p"] for a in nodes[r["b"]]["arms"] if a["road"] == ri), None) if r["b"] is not None else None
        if end_a is None:
            pa = _sub(r["c"], _mul(d, half - r["width"] * 0.5))
            end_a = (pa[0], pa[1], C._ground_z(world, pa[0], pa[1], r["z"]))
        if end_b is None:
            pb = _add(r["c"], _mul(d, half - r["width"] * 0.5))
            end_b = (pb[0], pb[1], C._ground_z(world, pb[0], pb[1], r["z"]))
        pts = _follow(world, end_a[:2], end_a[2], end_b[:2])
        pts[0], pts[-1] = end_a, end_b         # the very points the intersections use: the lanes must meet exactly
        sp = [C._pt(x, y, z, T.SHARP) for x, y, z in pts]
        _shape(sp, unreal.FZoneShapeType.SPLINE, ref, "Traffic_Road%02d" % ri, (ZG_VEHICLE,))
        shapes += 1
        lane_cm += 2.0 * _len(_sub(end_b[:2], end_a[:2]))

    # Intersections: an entry per arm at its stop line, the outline along the curb lines and round the corners.
    inner = max(50.0, max(r["width"] for r in roads) * 0.5 - lane_w)   # right turn: centre on the curb corner
    counts = {"signalized": 0, "stop": 0, "free": 0}
    signalized_nodes = []
    for ni, n in enumerate(nodes):
        arms = sorted(n["arms"], key=lambda a: math.atan2(a["u"][1], a["u"][0]))
        if len(arms) < 2:
            continue
        h = n["width"] * 0.5
        c = n["c"]
        poly = []
        for k, arm in enumerate(arms):
            u = arm["u"]
            nrm = (-u[1], u[0])                    # toward the next arm (increasing angle)
            px, py, pz = arm["p"]
            q = C._pt(px, py, pz, T.LANE_PROFILE, math.degrees(math.atan2(-u[1], -u[0])))
            q.inner_turn_radius = inner
            poly.append(q)
            e1 = _add((px, py), _mul(nrm, h))
            poly.append(C._pt(e1[0], e1[1], pz, T.SHARP))
            # The square's corners between this arm and the next one.
            nxt = arms[(k + 1) % len(arms)]
            a0 = math.atan2(u[1], u[0])
            a1 = math.atan2(nxt["u"][1], nxt["u"][0])
            gap = (a1 - a0) % (2.0 * math.pi)
            if gap < 1e-3:
                gap = 2.0 * math.pi
            corners = sorted(((math.pi * 0.25 + j * math.pi * 0.5 - a0) % (2.0 * math.pi) for j in range(4)))
            for off in corners:
                if 1e-3 < off < gap - 1e-3:
                    ang = a0 + off
                    poly.append(C._pt(c[0] + math.cos(ang) * h * math.sqrt(2.0), c[1] + math.sin(ang) * h * math.sqrt(2.0), n["zs"], T.SHARP))
            nu = nxt["u"]
            nn = (-nu[1], nu[0])
            npx, npy, npz = nxt["p"]
            e2 = _sub((npx, npy), _mul(nn, h))
            poly.append(C._pt(e2[0], e2[1], npz, T.SHARP))
        tags, actor_tags = (), ()
        if len(arms) >= 3:
            tags = (ZG_VEHICLE, ZG_INTERSECTION) + ((ZG_SIGNALIZED,) if n["signalized"] else ())
            actor_tags = (SIGNALIZED_TAG,) if n["signalized"] else ()
            counts["signalized" if n["signalized"] else "stop"] += 1
            if n["signalized"]:
                signalized_nodes.append(n)
        else:
            tags = (ZG_VEHICLE,)
            counts["free"] += 1
        _shape(poly, unreal.FZoneShapeType.POLYGON, ref, "Traffic_Intersection%02d" % ni, tags, actor_tags, arcs=True)
        shapes += 1

    # The crowd's crossings at the signalized intersections wait for Walk.
    areas = [(n["c"][0], n["c"][1], max(a["d"] for a in n["arms"]) + 300.0) for n in signalized_nodes]
    crossings = C.tag_crossings(areas)
    unreal.RammsCrowdZoneGraphLibrary.rebuild_zone_graph()

    # Parked cars in a lane's way (a road too narrow for parking and two lanes).
    offset = lane_w * 0.5
    parked_in_lane = 0
    for f in features:
        if f.get("kind") != "Vehicle" or f.get("subtype") != "Parked":
            continue
        loc, size = f.get("location", {}), f.get("size", {})
        p = (loc.get("x", 0.0), loc.get("y", 0.0))
        for r in roads:
            rel = _sub(p, r["c"])
            if abs(_dot(rel, r["d"])) > r["length"] * 0.5 + 100.0:
                continue
            across = abs(_cross(r["d"], rel))
            if across < r["width"] * 0.5 and across - size.get("y", 180.0) * 0.5 < offset + CAR_HALF_WIDTH:
                parked_in_lane += 1
                break
    if parked_in_lane:
        warnings.append("%d parked cars stand in a traffic lane's way (roads %.0f cm wide; traffic wants %.0f cm or more with parking)" % (parked_in_lane, width, WIDE_ROAD))

    # The signal clock and its collection, the spawner with the palette's cars, the signal materials.
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    g = gen.get_actor_location()
    centre = unreal.Vector(g.x, g.y, g.z + 200.0)
    mpc = signal_collection()
    clock = None
    clock_cls = unreal.load_class(None, CLOCK_CLASS)
    if clock_cls:
        clock = eas.spawn_actor_from_class(clock_cls, centre)
        clock.set_actor_label("TrafficSignalClock")
        clock.tags = [unreal.Name(TAG)]
        clock.set_folder_path(unreal.Name(FOLDER))
        if mpc is not None:
            clock.set_editor_property("signal_parameters", mpc)
        if timing:
            t = clock.get_editor_property("timing")
            for k, v in timing.items():
                t.set_editor_property(k, float(v))
            clock.set_editor_property("timing", t)
    else:
        warnings.append("no %s (RammsCrowd not built?)" % CLOCK_CLASS)
    palette_path = gen.get_palette_path()
    palette = unreal.load_asset(palette_path) if palette_path else None
    cars = _car_meshes(palette)
    if not cars:
        warnings.append("the palette has no Car meshes: traffic drives car-sized boxes")
    if count is None:
        count = int(max(6, min(60, lane_cm / 4500.0)))
    spawner = None
    spawner_cls = unreal.load_class(None, SPAWNER_CLASS)
    if spawner_cls:
        spawner = eas.spawn_actor_from_class(spawner_cls, centre)
        spawner.set_actor_label("TrafficSpawner")
        spawner.tags = [unreal.Name(TAG)]
        spawner.set_folder_path(unreal.Name(FOLDER))
        spawner.set_editor_property("vehicle_count", int(count))
        spawner.set_editor_property("seed", int(gen.get_editor_property("seed") if seed is None else seed))
        drv = spawner.get_editor_property("driving")
        drv.set_editor_property("speed_limit", float(speed_limit))
        spawner.set_editor_property("driving", drv)
        spawner.set_editor_property("car_meshes", cars)
    else:
        warnings.append("no %s (RammsCrowd not built?)" % SPAWNER_CLASS)
    materials = refresh_signal_materials(_recipe_of(palette_path)) if mpc is not None else []

    res = {"roads": len(roads), "intersections": len(nodes), "signalized": counts["signalized"], "all_way_stop": counts["stop"],
           "free_bends": counts["free"], "shapes": shapes, "lane_m": round(lane_cm / 100.0), "profile": LANE_PROFILES[kind][0],
           "vehicles": count if spawner else 0, "car_meshes": len(cars), "crossings_tagged": crossings,
           "parked_in_lane": parked_in_lane, "signal_collection": mpc.get_path_name() if mpc else None,
           "signal_materials": materials, "removed": removed, "warnings": warnings}
    H.log("traffic: %s" % res)
    return res
