"""Vehicle lanes, a signal clock and moving traffic for a generated street level (RammsCrowd's Mass traffic).

After generation, and after crowd.py (whose street crossings this ties to the signals), the street network's Road,
Intersection and Crosswalk records (manifest features) become ZoneGraph shapes:

- each road segment is a spline shape with the VehicleLanes profile (two-way, right-hand: lane 0 forward on the
  shape's right; VehicleLanesNarrow on roads under 10 m). Every 1.5 m both lane centres are traced onto the generated
  road surface (each from its previous height; a pothole is bridged): the spline point takes their mean height and its
  roll tilts the profile so each lane lies on its own side's surface -- grades, crowns, a hill's cross slope, humps;
- each intersection is a polygon shape whose lane-profile entries face into it; ZoneGraph joins them with turning
  lanes (never back the way they came). On level streets the entries stand at the stop lines (70 cm beyond each arm's
  own crosswalk, the painted line's outer edge; 1.5 m beyond the square without one), the outline runs along the curb
  lines and round the square's corners, and arc routing puts a right turn's centre on the curb corner. On graded
  streets (MaxStreetGrade > 0) the grade bends at the intersection's centre lines while ZoneGraph interpolates an
  intersection lane's height between its two entries, so the intersection is compact: the entries stand at the
  square's edges, pitched with the road and rolled with its cross slope, and Bezier routing carries those slopes in (a
  turn within one quarter of the square lies on that quarter's plane). The cars still stop at the stop lines: the
  spawner's StopLineGap is the stop line's distance from the square. Where three or four roads meet the polygon
  carries the Intersection tag (stop or signal control), and Signalized too when a TrafficSignal record stands at one
  of its corner crosswalks (or, without crosswalk records, nearest it); a two-road bend or straight is a plain polygon
  (free flow) and is never signalized.

The crowd's street crossings at the signalized intersections get the ZoneGraph Crossing tag (crowd.tag_crossings:
MassCrowd closes them until the signal clock opens them for Walk): one area per controlled crosswalk, stored on the
intersection shape (signalized_areas), so a crowd rebuild tags the same crossings. The ZoneGraph is rebuilt, and two
actors are placed: an ARammsTrafficSignalClock reading MPC_RammsTrafficSignals (built here) and an
ARammsTrafficSpawner with the palette's Car meshes (on graded or rough ground its cars trace the ground over the whole
site). The signal materials of the level's palette recipe are rebuilt so they read the collection.

check_traffic (run by build_traffic, its result in the summary) re-reads the shapes and reports how the lanes join,
dead ends, how far the lanes (intersection lanes reconstructed as ZoneGraph builds them) are from the road surface,
stop lines on crosswalks, the crowd crossings' tags and walk axes, and how the signals were assigned.

    import ramms_worldgen.traffic as T
    T.build_traffic(H.find_generator())       # editor only; save the level afterwards
    T.check_traffic()                          # the self-check alone

Every actor made here carries the tag RammsTraffic and sits in the WorldGen/Traffic outliner folder, so a rebuild
replaces it.
"""
import math

import unreal

from . import _helpers as H
from . import crowd as C

TAG = "RammsTraffic"
SIGNALIZED_TAG = "RammsTrafficSignalized"   # actor tag on signalized intersection shapes
XING_TAG = "RammsTrafficXing:"              # + "x,y,axis": a crosswalk the signal clock controls (signalized_areas)
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
STOP_BEYOND_CROSSWALK = 70.0    # the stop line's outer edge beyond its crosswalk's far edge (painted 40-70 cm out)
STOP_NO_CROSSWALK = 150.0       # without a crosswalk on an arm, the stop line this far beyond the intersection square
STOP_FALLBACK = 0.0             # a road too short for both stop lines beyond their crosswalks: this far beyond the square
MIN_ROAD_LANE = 100.0           # shortest road spline left between two stop lines (level streets)
SIGNAL_XWALK_RADIUS = 1200.0    # a TrafficSignal this close to a corner crosswalk stands at that crosswalk's intersection
SIGNAL_RADIUS = 2500.0          # (without crosswalk records: the nearest intersection this close)
XING_RADIUS = 400.0             # a crowd crossing whose middle is this close to a controlled crosswalk waits for Walk
CAR_HALF_WIDTH = 100.0          # a moving car's half width when the palette has no Car meshes
CAR_LENGTH = 450.0              # compact intersections: a stop line leaves at least this much road behind it
DEFAULT_STOP_LINE_GAP = 40.0    # RammsCrowd's FRammsTrafficDrivingParams.StopLineGap default
TRACE_MARGIN = 2000.0           # graded ground: the cars trace the ground this far beyond the site's extent
CHECK_STEP = 100.0              # check_traffic: a lane-ground sample every this many cm along every lane
TRACE_OK_CM = 55.0              # the runtime's ground trace corrects a car within 60 cm of its lane
ZG_VEHICLE, ZG_INTERSECTION, ZG_SIGNALIZED, ZG_CROSSING = "Vehicle", "Intersection", "Signalized", "Crossing"


# --- geometry -------------------------------------------------------------------------------------

def _sub(a, b): return (a[0] - b[0], a[1] - b[1])
def _add(a, b): return (a[0] + b[0], a[1] + b[1])
def _mul(a, k): return (a[0] * k, a[1] * k)
def _dot(a, b): return a[0] * b[0] + a[1] * b[1]
def _cross(a, b): return a[0] * b[1] - a[1] * b[0]
def _len(a): return math.hypot(a[0], a[1])


def _unit(a):
    n = _len(a)
    return (a[0] / n, a[1] / n) if n > 1e-9 else (1.0, 0.0)


def _sub3(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def _add3(a, b): return (a[0] + b[0], a[1] + b[1], a[2] + b[2])
def _mul3(a, k): return (a[0] * k, a[1] * k, a[2] * k)
def _dot3(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def _len3(a): return math.sqrt(_dot3(a, a))
def _lerp3(a, b, t): return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t)


def _cross3(a, b):
    # FVector::CrossProduct
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def _norm3(a):
    n = _len3(a)
    return _mul3(a, 1.0 / n) if n > 1e-9 else a


def _v(vec):
    return (float(vec.x), float(vec.y), float(vec.z))


def _axes(rot):
    """(forward, right, up) of a Rotator, as FRotationMatrix builds them (UE's left-handed frame: heading +X the right
    is +Y; a positive roll lowers the right side)."""
    p, y, r = math.radians(rot.pitch), math.radians(rot.yaw), math.radians(rot.roll)
    sp, cp, sy, cy, sr, cr = math.sin(p), math.cos(p), math.sin(y), math.cos(y), math.sin(r), math.cos(r)
    return ((cp * cy, cp * sy, sp),
            (sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp),
            (-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp))


def _roll(z_right, z_left, half):
    """The roll (degrees) that tilts a lane profile so its lanes, `half` either side of its centre, stand at z_right
    (on the heading's right) and z_left."""
    return -math.degrees(math.atan2(z_right - z_left, 2.0 * half))


def _walk_axis(dx, dy):
    """RammsTraffic::TravelAxis: 0 along X, 1 along Y (how the signal clock reads a crossing lane's direction)."""
    return 0 if abs(dx) >= abs(dy) else 1


# --- manifest records -----------------------------------------------------------------------------

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
    """Every Crosswalk record: centre c, w its width along the road, span its length curb to curb, along_x whether its
    road runs along X (the record's yaw is 90 then), axis the walkers' (1 across a road along X)."""
    out = []
    for f in features or ():
        if f.get("kind") == "Crosswalk":
            loc, size = f.get("location", {}), f.get("size", {})
            yaw = math.radians((f.get("rotation") or {}).get("yaw", 0.0))
            along_x = abs(math.sin(yaw)) > 0.7
            out.append(dict(c=(loc.get("x", 0.0), loc.get("y", 0.0)), w=size.get("y", 300.0), span=size.get("x", 0.0),
                            along_x=along_x, axis=1 if along_x else 0, road=None, node=None, dist=None))
    return out


def _assign_crosswalks(xwalks, roads):
    """Each crosswalk's road, and for one in a road's own half next to an intersection (a corner crossing) that
    intersection and its distance from the intersection's centre. A crossing in the middle of a road (mid-block, or
    the one crossing of a short road) belongs to neither end."""
    for x in xwalks:
        for ri, r in enumerate(roads):
            if r["along_x"] != x["along_x"]:
                continue
            rel = _sub(x["c"], r["c"])
            along, across = _dot(rel, r["d"]), abs(_cross(r["d"], rel))
            half = r["length"] * 0.5 + r["width"] * 0.5
            if across > r["width"] * 0.5 or abs(along) > half:
                continue
            x["road"] = ri
            from_a = along + half
            if from_a < half - x["w"] * 0.5 - 1.0 and r["a"] is not None:
                x["node"], x["dist"] = r["a"], from_a
            elif from_a > half + x["w"] * 0.5 + 1.0 and r["b"] is not None:
                x["node"], x["dist"] = r["b"], 2.0 * half - from_a
            break
    return xwalks


def _arm_crosswalk(ni, arm, xwalks):
    """The arm's own crosswalk (a corner crossing of that intersection on that road), or None."""
    best = None
    for x in xwalks:
        if x["node"] == ni and x["road"] == arm["road"] and (best is None or x["dist"] < best["dist"]):
            best = x
    return best


def _natural_stop(ni, n, arm, xwalks):
    """From the node's centre along the arm to its stop line: the painted line's outer edge just beyond the arm's own
    crosswalk, else a little beyond the intersection square."""
    x = _arm_crosswalk(ni, arm, xwalks)
    if x is not None:
        return x["dist"] + x["w"] * 0.5 + STOP_BEYOND_CROSSWALK
    return n["width"] * 0.5 + STOP_NO_CROSSWALK


def _map_signals(features, nodes, xwalks):
    """Mark the signalized intersections: each TrafficSignal record belongs to the intersection of its nearest corner
    crosswalk (the street network posts a signal just past one), else (no crosswalk records) to the nearest
    intersection within SIGNAL_RADIUS. Only three- and four-way intersections are signalized. Returns counts."""
    signals = [(f["location"]["x"], f["location"]["y"]) for f in features or ()
               if f.get("kind") == "Prop" and f.get("subtype") == "TrafficSignal" and f.get("location")]
    for n in nodes:
        n["signalized"] = False
    stats = dict(records=len(signals), by_crosswalk=0, by_nearest_intersection=0, at_two_arm_nodes=0, unassigned=0)
    corner = [x for x in xwalks if x["node"] is not None]
    for s in signals:
        ni = None
        best = None
        for x in corner:
            d = _len(_sub(x["c"], s))
            if d < SIGNAL_XWALK_RADIUS and (best is None or d < best[0]):
                best = (d, x["node"])
        if best is not None:
            ni = best[1]
            stats["by_crosswalk"] += 1
        elif nodes:
            near = min(range(len(nodes)), key=lambda i: _len(_sub(nodes[i]["c"], s)))
            if _len(_sub(nodes[near]["c"], s)) < SIGNAL_RADIUS:
                ni = near
                stats["by_nearest_intersection"] += 1
        if ni is None:
            stats["unassigned"] += 1
        elif len(nodes[ni]["arms"]) >= 3:
            nodes[ni]["signalized"] = True
        else:
            stats["at_two_arm_nodes"] += 1
    stats["signalized_intersections"] = sum(1 for n in nodes if n["signalized"])
    return stats


# --- tracing --------------------------------------------------------------------------------------

def _down_hit(world, x, y, z_top, z_bottom, ignore=()):
    """The first blocking hit going straight down from z_top to z_bottom at (x, y): (height, impact normal's z,
    actor), or None."""
    hit = unreal.SystemLibrary.line_trace_single(world, unreal.Vector(x, y, z_top), unreal.Vector(x, y, z_bottom),
                                                 unreal.TraceTypeQuery.TRACE_TYPE_QUERY1, False, list(ignore),
                                                 unreal.DrawDebugTrace.NONE, True)
    if not hit:
        return None
    t = hit.to_tuple()
    if not t or not t[0]:
        return None
    vecs = [v for v in t if isinstance(v, unreal.Vector)]     # location, impact point, normal, impact normal, ...
    actor = next((v for v in t if isinstance(v, unreal.Actor)), None)
    if len(vecs) >= 4:
        return vecs[1].z, vecs[3].z, actor
    return (vecs[0].z, 1.0, actor) if vecs else None


def _is_pothole(actor):
    if actor is None or not isinstance(actor, unreal.RammsProcPiece):
        return False
    try:
        return str(actor.get_editor_property("manifest_subtype")) == "Pothole"
    except Exception:  # noqa: BLE001
        return False


def _lane_ground(world, x, y, z):
    """The road a lane rolls on at (x, y) near height z (as crowd._ground_z: from 60 cm above to 120 cm below,
    ignoring anything standing more than 40 cm above z, a seam retried 2 cm aside). None where there is none, or over a
    pothole: a lane bridges it, keeping its previous height."""
    for dx, dy in ((0.0, 0.0),) + tuple(C.SEAM_OFFSETS):
        h = _down_hit(world, x + dx, y + dy, z + 60.0, z - 120.0)
        if h is None or h[0] > z + 40.0:
            continue
        return None if _is_pothole(h[2]) else h[0]
    return None


def _road_z(world, x, y, z):
    """The road surface under a lane point (x, y, z), for measuring: the first upward-facing hit going down from 1.5 m
    above it to 3 m below, passing whatever else is there (a canopy, a mast arm, anything more than 1 m above the
    lane). (height, "road" or "pothole"), or None."""
    ignore = []
    for _ in range(4):
        h = _down_hit(world, x, y, z + 150.0, z - 300.0, ignore)
        if h is None:
            return None
        hz, nz, actor = h
        if nz > 0.7 and hz <= z + 100.0:
            return hz, ("pothole" if _is_pothole(actor) else "road")
        if actor is None:
            return None
        ignore.append(actor)
    return None


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
    """(x, y, z) about every `step` cm from a to b along a centre line, each traced onto the ground from the previous
    sample's height."""
    L = _len(_sub(b, a))
    n = max(1, int(math.ceil(L / step)))
    out, z = [], za
    for k in range(n + 1):
        t = k / n
        x, y = a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t
        z = C._ground_z(world, x, y, z)
        out.append((x, y, z))
    return out


def _lane_heights(world, p, right, half, zr, zl):
    """The two lane centres' heights at p, `half` either side along the unit `right`, each traced near its previous
    height (kept where there is no road, or a pothole)."""
    gr = _lane_ground(world, p[0] + right[0] * half, p[1] + right[1] * half, zr)
    gl = _lane_ground(world, p[0] - right[0] * half, p[1] - right[1] * half, zl)
    return (zr if gr is None else gr), (zl if gl is None else gl)


def _entry(world, p, u, zc, half, pitched):
    """A road end at p on an arm leaving its intersection along u: its lanes' heights (hp on the right of heading u,
    hn on its left), their mean z and, when pitched, the road's slope heading in (-u), as a pitch in degrees."""
    nrm = (-u[1], u[0])
    hp, hn = _lane_heights(world, p, nrm, half, zc, zc)
    pitch = 0.0
    if pitched:
        a, b = _add(p, _mul(u, 50.0)), _sub(p, _mul(u, 50.0))
        ap, an = _lane_heights(world, a, nrm, half, hp, hn)
        bp, bn = _lane_heights(world, b, nrm, half, hp, hn)
        pitch = math.degrees(math.atan2(0.5 * (bp + bn) - 0.5 * (ap + an), 100.0))
    return dict(x=p[0], y=p[1], z=0.5 * (hp + hn), hp=hp, hn=hn, pitch=pitch, u=u)


def _sides(e, heading):
    """(right, left) lane heights of entry e for a shape heading along `heading` there."""
    return (e["hp"], e["hn"]) if _dot(heading, e["u"]) > 0.0 else (e["hn"], e["hp"])


def _road_points(world, ea, eb, half, step=SAMPLE_STEP):
    """Spline points (x, y, z, roll) from entry ea to entry eb: both lane centres traced every `step` cm, the point at
    their mean height, rolled so each lane lies on its own side. The ends are the entries' own points, so the lanes meet
    the intersections' exactly."""
    a, b = (ea["x"], ea["y"]), (eb["x"], eb["y"])
    d = _unit(_sub(b, a))
    right = (-d[1], d[0])
    zr, zl = _sides(ea, d)
    out = [(a[0], a[1], ea["z"], _roll(zr, zl, half))]
    L = _len(_sub(b, a))
    n = max(1, int(math.ceil(L / step)))
    for k in range(1, n):
        p = _add(a, _mul(_sub(b, a), k / n))
        zr, zl = _lane_heights(world, p, right, half, zr, zl)
        out.append((p[0], p[1], 0.5 * (zr + zl), _roll(zr, zl, half)))
    zr, zl = _sides(eb, d)
    out.append((b[0], b[1], eb["z"], _roll(zr, zl, half)))
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


def _pt(x, y, z, ptype, yaw=0.0, pitch=0.0, roll=0.0):
    p = unreal.ZoneShapePoint()
    p.position = unreal.Vector(x, y, z)
    p.type = ptype
    p.rotation = unreal.Rotator(roll=roll, pitch=pitch, yaw=yaw)
    return p


def _shape(points, shape_type, profile_ref, label, zone_tags=(), actor_tags=(), routing=None):
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actor = eas.spawn_actor_from_class(unreal.ZoneShape, unreal.Vector(0, 0, 0))
    actor.set_actor_label(label)
    actor.tags = [unreal.Name(TAG)] + [unreal.Name(t) for t in actor_tags]
    actor.set_folder_path(unreal.Name(FOLDER))
    comp = actor.get_component_by_class(unreal.ZoneShapeComponent)
    comp.set_shape_type(shape_type)
    comp.set_editor_property("lane_profile", profile_ref)
    if routing is not None:
        comp.set_polygon_routing_type(routing)
    comp.set_editor_property("points", points)
    if zone_tags:
        unreal.RammsCrowdZoneGraphLibrary.set_zone_shape_tags(comp, [unreal.Name(t) for t in zone_tags], False)
    unreal.RammsCrowdZoneGraphLibrary.update_zone_shape(comp)
    return actor


def signalized_areas():
    """[(x, y, radius, walk axis)]: the crosswalks the level's traffic signal clock controls, read from the signalized
    intersections build_traffic laid (none without traffic). crowd.tag_crossings takes them, from build_traffic and
    from a crowd rebuild alike, so both tag the same crossings."""
    out = []
    for a in H.level_actors():
        if not isinstance(a, unreal.ZoneShape) or not a.actor_has_tag(SIGNALIZED_TAG):
            continue
        for t in a.tags:
            s = str(t)
            if not s.startswith(XING_TAG):
                continue
            try:
                x, y, axis = s[len(XING_TAG):].split(",")
                out.append((float(x), float(y), XING_RADIUS, int(axis)))
            except ValueError:
                continue
    return sorted(out)


def signal_collection():
    """MPC_RammsTrafficSignals: the scalars the signal clock drives (created or refreshed; saved when that changed it).
    None when RammsCrowd's editor module is not loaded."""
    lib = getattr(unreal, "RammsTrafficEditorLibrary", None)
    if lib is None:
        H.warn("traffic: RammsTrafficEditorLibrary missing (RammsCrowd editor module); no signal collection")
        return None
    H.ensure_dir(SIGNAL_MPC.rsplit("/", 1)[0])
    mpc = lib.ensure_signal_parameter_collection(SIGNAL_MPC)
    if mpc is not None:
        H.EAL.save_loaded_asset(mpc, only_if_is_dirty=True)
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


def _param(gen, name, default=0.0):
    """A generator parameter (its resolved params, else its preset's), as a float."""
    for get in (lambda: gen.get_editor_property("params"), lambda: gen.get_editor_property("preset").get_editor_property("params")):
        try:
            return float(get().get_editor_property(name))
        except Exception:  # noqa: BLE001 - no params, or a build without that parameter
            continue
    return default


# --- build ----------------------------------------------------------------------------------------

def build_traffic(gen, count=None, speed_limit=1000.0, seed=None, timing=None, compact=None):
    """Lay vehicle lanes along the generated level's roads, tie the crowd's crossings at signalized intersections to
    the signals, rebuild the ZoneGraph and place a signal clock and a traffic spawner. `count` None: about one car per
    45 m of lane (6..60); 0: the spawner's DensityPerKm decides; `speed_limit` cm/s; `seed` None: the generator's;
    `timing` a dict of FRammsSignalTiming fields (green_seconds_x, amber_seconds, ...); `compact` None: compact
    intersections on graded streets (MaxStreetGrade > 0), True / False to force it. Runs check_traffic and returns a
    summary dict with its result. Editor only; the caller saves the level."""
    m = H.manifest_to_dict(gen)
    features = m.get("features", [])
    removed = _clear()
    roads, nodes = _roads_and_nodes(features)
    if not roads:
        C.tag_crossings([])     # nothing signalized any more: no crossing waits for a clock
        unreal.RammsCrowdZoneGraphLibrary.rebuild_zone_graph()
        H.warn("traffic: the manifest has no Road records (regenerate with the current street network)")
        return {"roads": 0, "removed": removed, "note": "no Road records"}
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    xwalks = _assign_crosswalks(_crosswalks(features), roads)
    width = max(r["width"] for r in roads)
    kind = "wide" if width >= WIDE_ROAD else "narrow"
    ref, lane_w = _profile_ref(kind)
    half = lane_w * 0.5
    graded = C._graded_ground(gen)
    if compact is None:
        compact = _param(gen, "max_street_grade") > 0.0
    T = unreal.FZoneShapePointType
    warnings = []

    # Each node: its surface height, the signals at its corners, where its arms' stop lines are.
    for n in nodes:
        n["zs"] = _surface_z(world, n["c"][0], n["c"][1], n["z"])
    signal_stats = _map_signals(features, nodes, xwalks)
    for ni, n in enumerate(nodes):
        for arm in n["arms"]:
            arm["stop"] = _natural_stop(ni, n, arm, xwalks)
    if not compact:
        # Level streets: the two stop lines on one road must leave some road between them. A road too short for both
        # beyond their crosswalks keeps one or neither there; the other's stop line is the square's edge, where a car's
        # front (StopLineGap, 40 cm, short of it) stands before any crosswalk (the street network sets crosswalks at
        # least 40 cm back from the square), never on one.
        for ri, r in enumerate(roads):
            if r["a"] is None or r["b"] is None:
                continue
            aa = next(a for a in nodes[r["a"]]["arms"] if a["road"] == ri)
            ab = next(a for a in nodes[r["b"]]["arms"] if a["road"] == ri)
            total = r["length"] + r["width"]
            room = total - MIN_ROAD_LANE
            if aa["stop"] + ab["stop"] <= room:
                continue
            fa, fb = nodes[r["a"]]["width"] * 0.5 + STOP_FALLBACK, nodes[r["b"]]["width"] * 0.5 + STOP_FALLBACK
            for da, db in ((aa["stop"], fb), (fa, ab["stop"]), (fa, fb)):
                if da + db <= room:
                    break
            aa["stop"], ab["stop"] = da, db
            warnings.append("road at (%.0f, %.0f): too short for both stop lines beyond their crosswalks; stop lines at %.0f / %.0f cm "
                            "from the intersections (a waiting car stands over a crosswalk there)" % (r["c"][0], r["c"][1], da, db))
    # Compact intersections: the cars stop at the stop lines, StopLineGap short of the square's edge.
    gap_line = max((arm["stop"] - n["width"] * 0.5 for n in nodes for arm in n["arms"]), default=STOP_NO_CROSSWALK)
    for n in nodes:
        h = n["width"] * 0.5
        for arm in n["arms"]:
            dist = h if compact else arm["stop"]
            p = _add(n["c"], _mul(arm["u"], dist))
            zc = _follow(world, n["c"], n["zs"], p)[-1][2]
            arm["e"] = _entry(world, p, arm["u"], zc, half, compact)

    shapes = 0
    lane_cm = 0.0
    # Roads: from the intersection entry at end a to the one at end b, along the centreline, on the road surface.
    for ri, r in enumerate(roads):
        d = r["d"]
        ends = []
        for ni, sign in ((r["a"], 1.0), (r["b"], -1.0)):
            e = next((a["e"] for a in nodes[ni]["arms"] if a["road"] == ri), None) if ni is not None else None
            if e is None:
                # No intersection record at this end: the road ends at its square's edge.
                u = _mul(d, sign)
                p = _sub(r["c"], _mul(u, r["length"] * 0.5))
                e = _entry(world, p, u, C._ground_z(world, p[0], p[1], r["z"]), half, False)
            ends.append(e)
        pts = _road_points(world, ends[0], ends[1], half)
        sp = [_pt(x, y, z, T.SHARP, math.degrees(math.atan2(d[1], d[0])), 0.0, roll) for x, y, z, roll in pts]
        _shape(sp, unreal.FZoneShapeType.SPLINE, ref, "Traffic_Road%02d" % ri, (ZG_VEHICLE,))
        shapes += 1
        L = _len(_sub((ends[1]["x"], ends[1]["y"]), (ends[0]["x"], ends[0]["y"])))
        lane_cm += 2.0 * L
        if compact and L - gap_line < CAR_LENGTH:
            warnings.append("road at (%.0f, %.0f): %.0f cm long, its stop lines %.0f cm from the squares leave under a car's length" % (r["c"][0], r["c"][1], L, gap_line))

    # Intersections: an entry per arm, the outline along the curb lines (level streets) and round the corners.
    inner = max(50.0, max(r["width"] for r in roads) * 0.5 - lane_w)   # arcs: a right turn's centre on the curb corner
    routing = unreal.ZoneShapePolygonRoutingType.BEZIER if compact else unreal.ZoneShapePolygonRoutingType.ARCS
    counts = {"signalized": 0, "stop": 0, "free": 0}
    for ni, n in enumerate(nodes):
        arms = sorted(n["arms"], key=lambda a: math.atan2(a["u"][1], a["u"][0]))
        if len(arms) < 2:
            continue
        h = n["width"] * 0.5
        c = n["c"]
        poly = []
        for k, arm in enumerate(arms):
            u, e = arm["u"], arm["e"]
            nrm = (-u[1], u[0])                    # toward the next arm (increasing angle): the entry's left
            # Facing in (-u): its right is -nrm, so its right lane is at hn.
            q = _pt(e["x"], e["y"], e["z"], T.LANE_PROFILE, math.degrees(math.atan2(-u[1], -u[0])), e["pitch"], _roll(e["hn"], e["hp"], half))
            q.inner_turn_radius = inner
            poly.append(q)
            nxt = arms[(k + 1) % len(arms)]
            ne = nxt["e"]
            if not compact:
                e1 = _add((e["x"], e["y"]), _mul(nrm, h))
                poly.append(_pt(e1[0], e1[1], e["z"], T.SHARP))
            # The square's corners between this arm and the next one (compact: the entries' curb-line ends are corners).
            a0 = math.atan2(u[1], u[0])
            a1 = math.atan2(nxt["u"][1], nxt["u"][0])
            gap = (a1 - a0) % (2.0 * math.pi)
            if gap < 1e-3:
                gap = 2.0 * math.pi
            cz = 0.5 * (e["z"] + ne["z"]) if compact else n["zs"]
            corners = sorted(((math.pi * 0.25 + j * math.pi * 0.5 - a0) % (2.0 * math.pi) for j in range(4)))
            for off in corners:
                if 1e-3 < off < gap - 1e-3:
                    ang = a0 + off
                    poly.append(_pt(c[0] + math.cos(ang) * h * math.sqrt(2.0), c[1] + math.sin(ang) * h * math.sqrt(2.0), cz, T.SHARP))
            if not compact:
                nu = nxt["u"]
                nn = (-nu[1], nu[0])
                e2 = _sub((ne["x"], ne["y"]), _mul(nn, h))
                poly.append(_pt(e2[0], e2[1], ne["z"], T.SHARP))
        tags, actor_tags = (ZG_VEHICLE,), ()
        if len(arms) >= 3:
            tags = (ZG_VEHICLE, ZG_INTERSECTION) + ((ZG_SIGNALIZED,) if n["signalized"] else ())
            if n["signalized"]:
                actor_tags = (SIGNALIZED_TAG,) + tuple("%s%.0f,%.0f,%d" % (XING_TAG, x["c"][0], x["c"][1], x["axis"])
                                                       for x in xwalks if x["node"] == ni)
            counts["signalized" if n["signalized"] else "stop"] += 1
        else:
            counts["free"] += 1
        _shape(poly, unreal.FZoneShapeType.POLYGON, ref, "Traffic_Intersection%02d" % ni, tags, actor_tags, routing)
        shapes += 1

    # The crowd's crossings at the signalized intersections wait for Walk.
    crossings = C.tag_crossings(signalized_areas())
    unreal.RammsCrowdZoneGraphLibrary.rebuild_zone_graph()

    # Parked cars in a lane's way (a road too narrow for parking and two lanes), against the widest palette car.
    palette_path = gen.get_palette_path()
    palette = unreal.load_asset(palette_path) if palette_path else None
    cars = _car_meshes(palette)
    if not cars:
        warnings.append("the palette has no Car meshes: traffic drives car-sized boxes")
    car_half = max([float(cm.get_editor_property("footprint").y) * 0.5 for cm in cars] or [CAR_HALF_WIDTH])
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
            if across < r["width"] * 0.5 and across - size.get("y", 180.0) * 0.5 < half + car_half:
                parked_in_lane += 1
                break
    if parked_in_lane:
        warnings.append("%d parked cars stand in a traffic lane's way (roads %.0f cm wide; traffic wants %.0f cm or more with parking)" % (parked_in_lane, width, WIDE_ROAD))

    # The signal clock and its collection, the spawner with the palette's cars, the signal materials.
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    g = gen.get_actor_location()
    centre = unreal.Vector(g.x, g.y, g.z + 200.0)
    mpc = signal_collection()
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
    count_mode = "fixed"
    if count is None:
        count = int(max(6, min(60, lane_cm / 4500.0)))
        count_mode = "estimate"
    elif int(count) <= 0:
        count, count_mode = 0, "density"
    vehicles = int(count)
    stop_gap = None
    trace_radius = None
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
        stop_gap = float(drv.get_editor_property("stop_line_gap"))
        if compact:
            stop_gap += gap_line
            drv.set_editor_property("stop_line_gap", stop_gap)
        spawner.set_editor_property("driving", drv)
        spawner.set_editor_property("car_meshes", cars)
        if graded:
            # Graded or rough ground: every car traces its axles onto the ground, wherever it is (not only near the
            # robot), so a car on an intersection lane a little off the road still sits on it.
            pts = [n["c"] for n in nodes] or [r["c"] for r in roads]
            extent = max(_len(_sub(a, b)) for a in pts for b in pts) if len(pts) > 1 else 0.0
            trace_radius = extent + TRACE_MARGIN
            spawner.set_editor_property("ground_trace_radius", float(trace_radius))
        if count_mode == "density":
            vehicles = int(round(float(spawner.get_editor_property("density_per_km")) * lane_cm / 100000.0))
    else:
        warnings.append("no %s (RammsCrowd not built?)" % SPAWNER_CLASS)
        vehicles = 0
    materials = refresh_signal_materials(_recipe_of(palette_path)) if mpc is not None else []

    check = check_traffic(gen, world=world, features=features)
    res = {"roads": len(roads), "intersections": len(nodes), "signalized": counts["signalized"], "all_way_stop": counts["stop"],
           "free_bends": counts["free"], "shapes": shapes, "lane_m": round(lane_cm / 100.0), "profile": LANE_PROFILES[kind][0],
           "layout": "compact" if compact else "stop_lines", "routing": "bezier" if compact else "arcs",
           "stop_line_gap_cm": stop_gap, "ground_trace_radius_cm": trace_radius, "graded": graded,
           "vehicles": vehicles, "vehicle_count_mode": count_mode, "car_meshes": len(cars), "car_half_width_cm": car_half,
           "crossings_tagged": crossings, "parked_in_lane": parked_in_lane, "signals": signal_stats,
           "signal_collection": mpc.get_path_name() if mpc else None, "signal_materials": materials, "removed": removed,
           "lane_ground_dev_cm": check.get("lane_ground_dev_cm"), "check": check, "warnings": warnings}
    H.log("traffic: %s" % res)
    return res


# --- self-check -----------------------------------------------------------------------------------

def _spline_lanes(pts, lane_w):
    """A spline shape's two lanes as ZoneGraph tessellates sharp points (ZoneShapeUtilities TessellateSplineShape): each
    point offset along its mitred right vector (edge normals Up x Forward, the ends' from their rotations) by half a
    lane, lane 0 forward on the right, lane 1 back on the left."""
    P = [_v(p.position) for p in pts]
    n = len(P)
    if n < 2:
        return []
    E = [None] * n
    for i in range(n - 1):
        E[i] = _norm3(_cross3(_axes(pts[i].rotation)[2], _sub3(P[i + 1], P[i])))
    E[n - 1] = E[n - 2]
    E[0] = _norm3(_axes(pts[0].rotation)[1])
    E[n - 1] = _norm3(_axes(pts[-1].rotation)[1])
    R = [E[0]]
    for i in range(1, n - 1):
        avg = _mul3(_add3(E[i - 1], E[i]), 0.5)
        l2 = _dot3(avg, avg)
        if l2 > 1e-8:
            s = min(1.0 / l2, 20.0)
            R.append(_mul3(avg, 1.0 if s < 1.0 - 1e-4 else s))
        else:
            R.append(E[i])
    R.append(E[n - 1])
    k = lane_w * 0.5
    return [[_add3(P[i], _mul3(R[i], k)) for i in range(n)], [_sub3(P[i], _mul3(R[i], k)) for i in reversed(range(n))]]


def _bezier(p0, p1, p2, p3, t):
    u = 1.0 - t
    a, b, c, d = u * u * u, 3.0 * u * u * t, 3.0 * u * t * t, t * t * t
    return tuple(a * p0[i] + b * p1[i] + c * p2[i] + d * p3[i] for i in range(3))


def _arc_lane(s, d, lane_w, step=50.0):
    """An arc-routed intersection lane, approximately (ZoneShapeUtilities CalculateIntersectingPath): straight in, a
    circular turn of the inner radius plus the lane's distance from the profile's inner edge (fitted to the corner),
    straight out; its height lerped along it between the two ends (ZoneGraph lerps by point index)."""
    p0, p3 = s["out"], d["inn"]
    a = _unit((s["f"][0], s["f"][1]))
    b = _unit((-d["f"][0], -d["f"][1]))
    cr = _cross(a, b)
    path = [(p0[0], p0[1])]
    rel = (p3[0] - p0[0], p3[1] - p0[1])
    if abs(cr) > 1e-3:
        ta, tb = _cross(rel, b) / cr, _cross(a, rel) / cr
        if ta > 0.0 and tb > 0.0:
            right_turn = _dot((a[1], -a[0]), rel) < 0.0          # ZoneGraph's "CCW": the lane's own edge is inside
            radius = s["inner"] + (0.5 if right_turn else 1.5) * lane_w
            theta = math.atan2(abs(cr), _dot(a, b))
            k = math.tan(theta * 0.5)
            off = radius * k
            if off > min(ta, tb):
                off = min(ta, tb)
                radius = off / max(k, 1e-6)
            q = _add((p0[0], p0[1]), _mul(a, ta))
            t1, t2 = _sub(q, _mul(a, off)), _add(q, _mul(b, off))
            na = (-a[1], a[0]) if cr > 0.0 else (a[1], -a[0])
            ctr = _add(t1, _mul(na, radius))
            s0 = math.atan2(t1[1] - ctr[1], t1[0] - ctr[0])
            s1 = math.atan2(t2[1] - ctr[1], t2[0] - ctr[0])
            sweep = (s1 - s0) % (2.0 * math.pi) if cr > 0.0 else -((s0 - s1) % (2.0 * math.pi))
            divs = max(2, int(abs(sweep) * radius / step))
            path.append(t1)
            for i in range(1, divs):
                ang = s0 + sweep * i / divs
                path.append((ctr[0] + math.cos(ang) * radius, ctr[1] + math.sin(ang) * radius))
            path.append(t2)
    path.append((p3[0], p3[1]))
    cum = [0.0]
    for i in range(1, len(path)):
        cum.append(cum[-1] + _len(_sub(path[i], path[i - 1])))
    total = cum[-1] or 1.0
    return [(x, y, p0[2] + (p3[2] - p0[2]) * (cum[i] / total)) for i, (x, y) in enumerate(path)]


def _polygon_lanes(pts, lane_w, bezier, step=50.0):
    """A polygon shape's lanes as ZoneGraph builds them between its lane-profile entries (ZoneShapeUtilities
    BuildLanesBetweenPoints): from each entry's incoming lane slot (lane 0, a quarter across the profile from its
    right edge) to every other entry's outgoing slot (three quarters across), pruned past LaneConnectionAngle (120
    deg); Bezier routing: control points a third of the slots' distance along each entry's forward (3D: its pitch)."""
    T = unreal.FZoneShapePointType
    ents = []
    for p in pts:
        if p.type != T.LANE_PROFILE:
            continue
        f, r, _up = _axes(p.rotation)
        P = _v(p.position)
        tl = float(p.tangent_length) or lane_w
        right_edge, left_edge = _add3(P, _mul3(r, tl)), _sub3(P, _mul3(r, tl))
        ents.append(dict(f=f, out=_lerp3(right_edge, left_edge, 0.25), inn=_lerp3(right_edge, left_edge, 0.75), inner=float(p.inner_turn_radius)))
    cos_max = math.cos(math.radians(120.0))
    lanes = []
    for i, s in enumerate(ents):
        for j, d in enumerate(ents):
            if i == j or _dot3(s["f"], _mul3(d["f"], -1.0)) < cos_max:
                continue
            if bezier:
                p0, p3 = s["out"], d["inn"]
                tl = _len3(_sub3(p3, p0)) / 3.0
                p1, p2 = _add3(p0, _mul3(s["f"], tl)), _add3(p3, _mul3(d["f"], tl))
                nseg = max(4, int(math.ceil(3.0 * tl / step)))
                lanes.append([_bezier(p0, p1, p2, p3, k / nseg) for k in range(nseg + 1)])
            else:
                lanes.append(_arc_lane(s, d, lane_w, step))
    return lanes


def _resample(poly, step):
    out = [poly[0]]
    for a, b in zip(poly, poly[1:]):
        L = _len((b[0] - a[0], b[1] - a[1]))
        nseg = max(1, int(math.ceil(L / step)))
        for k in range(1, nseg + 1):
            out.append(_lerp3(a, b, k / nseg))
    return out


def _lane_dev(world, lanes, step):
    """Lane points' height above (+) or below the road under them, every `step` cm: max / mean / p95 of its size, per
    kind ("road" splines, "intersection" polygons), and the worst one. Potholes are bridged (not counted)."""
    devs = {"road": [], "intersection": []}
    worst = None
    potholes = no_ground = 0
    for label, kind, poly in lanes:
        for x, y, z in _resample(poly, step):
            g = _road_z(world, x, y, z)
            if g is None:
                no_ground += 1
                continue
            if g[1] == "pothole":
                potholes += 1
                continue
            dz = z - g[0]
            devs[kind].append(abs(dz))
            if worst is None or abs(dz) > abs(worst["dev"]):
                worst = {"dev": round(dz, 1), "shape": label, "x": round(x), "y": round(y)}
    every = sorted(devs["road"] + devs["intersection"])

    def mx(v):
        return round(max(v), 1) if v else 0.0
    return {"max": mx(every), "mean": round(sum(every) / len(every), 2) if every else 0.0,
            "p95": round(every[min(len(every) - 1, int(0.95 * len(every)))], 1) if every else 0.0,
            "road_max": mx(devs["road"]), "intersection_max": mx(devs["intersection"]),
            "samples": len(every), "potholes_bridged": potholes, "no_ground": no_ground, "worst": worst}


def check_traffic(gen=None, world=None, features=None, step=CHECK_STEP):
    """Self-check of the level's traffic as build_traffic laid it (editor only). Re-reads the traffic shapes and
    reports, under "problems" anything that needs a look:

    - joins: every road spline end must meet an intersection entry as ZoneGraph joins them (within 25 cm, facing
      within 10 deg); an end at an intersection of one road is a dead end (the runtime starts a car there again
      elsewhere); entries no road meets lead nowhere;
    - lane_ground_dev_cm: every lane sampled every `step` cm -- road splines as ZoneGraph tessellates them, the
      intersections' lanes reconstructed as ZoneGraph builds them (Bezier exactly; arcs approximately) -- against the
      road surface traced under it;
    - stops_on_crosswalk: where each approach's cars stop (StopLineGap short of the intersection entry) lies on a
      crosswalk;
    - crossings: the crowd's street crossings (crowd.CROSSING_TAG shapes): which carry the ZoneGraph Crossing tag, any
      whose end-to-end walk axis (the one the signal clock reads) is not its crosswalk's, any tagged outside or
      untagged inside the signalized areas, any not a straight crossing;
    - signals: how the TrafficSignal records were assigned to intersections.

    gen None: the level's generator (for the manifest)."""
    world = world or H.editor_world()
    if features is None:
        gen = gen or H.find_generator()
        features = H.manifest_to_dict(gen).get("features", [])
    T = unreal.FZoneShapePointType
    roads, nodes = _roads_and_nodes(features)
    xwalks = _assign_crosswalks(_crosswalks(features), roads)
    problems = []

    splines, polys = [], []
    gap = DEFAULT_STOP_LINE_GAP
    for a in H.level_actors():
        if not a.actor_has_tag(TAG):
            continue
        if not isinstance(a, unreal.ZoneShape):
            try:
                gap = float(a.get_editor_property("driving").get_editor_property("stop_line_gap"))
            except Exception:  # noqa: BLE001 - the clock
                pass
            continue
        comp = a.get_component_by_class(unreal.ZoneShapeComponent)
        prof = str(comp.get_editor_property("lane_profile").get_editor_property("name"))
        lane_w = next((w for nm, _g, w in LANE_PROFILES.values() if nm == prof), 320.0)
        entry = dict(label=a.get_actor_label(), pts=list(comp.get_editor_property("points")), lane_w=lane_w)
        if comp.get_shape_type() == unreal.FZoneShapeType.POLYGON:
            entry["bezier"] = comp.get_editor_property("polygon_routing_type") == unreal.ZoneShapePolygonRoutingType.BEZIER
            polys.append(entry)
        else:
            splines.append(entry)

    # Joins.
    snap, snap_cos = 25.0, math.cos(math.radians(10.0))
    lps = []
    for P in polys:
        for p in P["pts"]:
            if p.type == T.LANE_PROFILE:
                lps.append(dict(pos=_v(p.position), n=_mul3(_axes(p.rotation)[0], -1.0), matched=False, label=P["label"]))
    joined, dead_ends, unjoined = 0, 0, []
    for S in splines:
        for p, nrm in ((S["pts"][0], _mul3(_axes(S["pts"][0].rotation)[0], -1.0)), (S["pts"][-1], _axes(S["pts"][-1].rotation)[0])):
            pos = _v(p.position)
            hit = next((e for e in lps if _len3(_sub3(e["pos"], pos)) < snap and _dot3(nrm, _mul3(e["n"], -1.0)) > snap_cos), None)
            if hit is not None:
                hit["matched"] = True
                joined += 1
                continue
            near = min(nodes, key=lambda n: _len(_sub(n["c"], pos[:2]))) if nodes else None
            if near is not None and len(near["arms"]) < 2:
                dead_ends += 1
            else:
                unjoined.append((S["label"], round(pos[0]), round(pos[1])))
    orphans = [(e["label"], round(e["pos"][0]), round(e["pos"][1])) for e in lps if not e["matched"]]
    if unjoined:
        problems.append("%d road ends meet no intersection entry, e.g. %s" % (len(unjoined), unjoined[:3]))
    if orphans:
        problems.append("%d intersection entries meet no road, e.g. %s" % (len(orphans), orphans[:3]))

    # Lanes against the ground.
    lanes = []
    for S in splines:
        for lane in _spline_lanes(S["pts"], S["lane_w"]):
            lanes.append((S["label"], "road", lane))
    for P in polys:
        for lane in _polygon_lanes(P["pts"], P["lane_w"], P.get("bezier", False)):
            lanes.append((P["label"], "intersection", lane))
    dev = _lane_dev(world, lanes, step)
    if dev["max"] > TRACE_OK_CM:
        problems.append("a lane %.0f cm off the road at %s (the runtime's ground trace corrects up to 60 cm)" % (dev["max"], dev["worst"]))

    # Where the cars stop: StopLineGap short of every intersection entry a road meets, back along the road.
    on_xwalk = []
    for e in lps:
        if not e["matched"]:
            continue
        u = _unit((e["n"][0], e["n"][1]))          # the connector faces out of the intersection, back along the road
        front = _add((e["pos"][0], e["pos"][1]), _mul(u, gap))
        for x in xwalks:
            dirx = (1.0, 0.0) if x["along_x"] else (0.0, 1.0)
            rel = _sub(front, x["c"])
            if abs(_dot(rel, dirx)) < x["w"] * 0.5 - 1.0 and abs(_cross(dirx, rel)) < max(x["span"], 1.0) * 0.5:
                on_xwalk.append((e["label"], round(front[0]), round(front[1])))
                break
    if on_xwalk:
        problems.append("%d approaches stop on a crosswalk, e.g. %s" % (len(on_xwalk), on_xwalk[:3]))

    # The crowd's crossings.
    areas = signalized_areas()
    lib = unreal.RammsCrowdZoneGraphLibrary
    xs = dict(crossings=0, tagged=0, axis_mismatch=[], tagged_outside=[], untagged_inside=[], not_straight=[])
    for a in H.level_actors():
        if not isinstance(a, unreal.ZoneShape) or not a.actor_has_tag(C.TAG) or not a.actor_has_tag(C.CROSSING_TAG):
            continue
        comp = a.get_component_by_class(unreal.ZoneShapeComponent)
        P = [_v(p.position) for p in comp.get_editor_property("points")]
        if len(P) < 2:
            continue
        xs["crossings"] += 1
        label = a.get_actor_label()
        mx, my = sum(p[0] for p in P) / len(P), sum(p[1] for p in P) / len(P)
        dx, dy = P[-1][0] - P[0][0], P[-1][1] - P[0][1]
        path = sum(_len(_sub(P[i + 1][:2], P[i][:2])) for i in range(len(P) - 1))
        straight = _len((dx, dy)) >= 0.9 * path
        tagged = hasattr(lib, "get_zone_shape_tag_names") and ZG_CROSSING in [str(t) for t in lib.get_zone_shape_tag_names(comp)]
        area = next((ar for ar in areas if math.hypot(mx - ar[0], my - ar[1]) < ar[2]), None)
        near = min(xwalks, key=lambda x: _len(_sub(x["c"], (mx, my)))) if xwalks else None
        want = near["axis"] if near is not None and _len(_sub(near["c"], (mx, my))) < 2.0 * XING_RADIUS else None
        if not straight and (tagged or area is not None):
            xs["not_straight"].append((label, round(mx), round(my)))     # a signal-controlled crossing that is more than a crossing
        if tagged:
            xs["tagged"] += 1
            if want is not None and _walk_axis(dx, dy) != want:
                xs["axis_mismatch"].append((label, round(mx), round(my)))
            if area is None:
                xs["tagged_outside"].append((label, round(mx), round(my)))
        elif area is not None:
            xs["untagged_inside"].append((label, round(mx), round(my)))
    for key, what in (("axis_mismatch", "tagged crossings walk along the wrong axis"), ("tagged_outside", "crossings are tagged outside the signalized areas"),
                      ("untagged_inside", "crossings at signalized intersections are not tagged"), ("not_straight", "crossing shapes are not one straight crossing")):
        if xs[key]:
            problems.append("%d %s, e.g. %s" % (len(xs[key]), what, xs[key][:3]))
    for key in ("axis_mismatch", "tagged_outside", "untagged_inside", "not_straight"):
        xs[key] = xs[key][:10]

    signal_stats = _map_signals(features, nodes, xwalks)
    if signal_stats["unassigned"]:
        problems.append("%d TrafficSignal records belong to no intersection" % signal_stats["unassigned"])

    return {"roads": len(splines), "intersections": len(polys), "joined_ends": joined, "dead_end_lanes": dead_ends,
            "unjoined_ends": unjoined[:10], "orphan_entries": orphans[:10], "lane_ground_dev_cm": dev,
            "stop_line_gap_cm": gap, "stops_on_crosswalk": len(on_xwalk), "stops_on_crosswalk_at": on_xwalk[:10],
            "crossings": xs, "signals": signal_stats, "areas": len(areas), "ok": not problems, "problems": problems}
