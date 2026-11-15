"""Pedestrian lanes and a crowd for a generated level (RammsCrowd: Mass + ZoneGraph).

After generation, the manifest's routes become a ZoneGraph network:

- outdoors, each block's sidewalk sides and each park's paths are straight runs; they are cut where they
  cross (block corners, the park's cross paths) into a graph and short dangling stubs are dropped. The
  street network's crosswalks (manifest features of kind Crosswalk) join the sidewalks across the street:
  each one is a run from one sidewalk's centre line over its curb ramp, along the zebra, and up the ramp
  on the far side. (Manifests from before crosswalks were recorded join neighbouring blocks' corners
  instead.);
- indoors, every room's door-to-centre routes (already bent around the furniture) join at the shared
  door centres and room centres.

Every run is sampled about every 50 cm onto the ground below it (a line trace), so lanes follow curb
ramps, the drop to the road and any slope instead of cutting straight through the air between their
end points.

Nodes where three or more runs meet (and one node per closed loop) become polygon junctions
(AZoneShape, polygon type, a lane-profile point per connecting run); every run between them becomes a
spline AZoneShape whose ends sit on those points, so ZoneGraph links the lanes. Lanes use the project's
"PedestrianLanes" profile (two 150 cm lanes, Crowd | Sidewalk | DensityStd tags); indoors the narrower
"PedestrianLanesNarrow" (2 x 75 cm) when the project defines it. Then the ZoneGraph is rebuilt and an
ARammsCrowdSpawner in ZoneGraphLanes mode is placed with /Game/NPCs/DA_CrowdAgentProfile.

    import ramms_worldgen.crowd as C
    C.build_crowd(H.find_generator())     # editor only; save the level afterwards

Every actor made here carries the tag RammsCrowd and sits in the WorldGen/Crowd outliner folder, so a
rebuild replaces it.

Street crossings are tagged CROSSING_TAG (an actor tag). On a level with traffic (traffic.py) the crossings at its
signalized intersections also get the ZoneGraph tag Crossing (tag_crossings; MassCrowd's CrossingTag): MassCrowd
closes those lanes, the traffic signal clock opens them for Walk, and walkers wait at the curb meanwhile. Other
crossings (mid-block, all-way stops, every crossing of a level without traffic) never carry it: there the crowd
walks when it likes and the cars yield.
"""
import math

import unreal

from . import _helpers as H

TAG = "RammsCrowd"
FOLDER = "WorldGen/Crowd"
PROFILE_ASSET = "/Game/NPCs/DA_CrowdAgentProfile"
SPAWNER_CLASS = "/Script/RammsCrowd.RammsCrowdSpawner"
# Lane profiles from Config/DefaultPlugins.ini ([/Script/ZoneGraph.ZoneGraphSettings]).
# (name, guid, total width): two opposite lanes each. Generated levels use the narrower ones so a lane
# fits the clear part of a sidewalk or a doorway: agents walk a lane's centre line and Mass avoidance only
# steers around other agents, not around static geometry, so the lanes themselves must be clear.
LANE_PROFILES = {
    "wide": ("PedestrianLanes", "8A3C5B1E4F2D47A9B6E1C0D2F3A4B5C6", 300.0),
    "sidewalk": ("PedestrianLanesSidewalk", "7B1D4C9E2A3F4B5C8D6E0F1A2B3C4D5E", 120.0),
    "narrow": ("PedestrianLanesNarrow", "5D2E8B7A1C4F4E3B9A6D0C1E2F3B4A57", 50.0),
}
AGENT_RADIUS = 30.0     # clearance kept for a lane's walkers (the crowd agents' radius is 25 cm, RammsCrowdAgentTrait)
CELL = 25.0             # clearance grid for rerouting a lane around furniture and props
LEVEL_TOL = 8.0         # a detour of a run on one walking level drops at most this below it (cm): not down a curb
LEVEL_CURB_SHARE = 0.6  # ... and at most this share of the curb height, so a low curb is still caught
LEVEL_MIN_CURB = 4.0    # streets with a lower curb than this (cm) have no level to keep: detours are not constrained
LEVEL_RISE = 8.0        # ground higher than this above the level under a walker's centre is an obstacle (a planter)
CROSSING_TAG = "RammsCrowdCrossing"   # extra tag on the lane shapes that carry a street crossing


def _level_tol(curb_height):
    """The drop (cm) that takes a walker off a run's level for streets with this curb height (None: unknown, the
    default curb), or None when the streets have no real curb to keep off."""
    if curb_height is None:
        return LEVEL_TOL
    ch = float(curb_height)
    if ch < LEVEL_MIN_CURB:
        return None
    return min(LEVEL_TOL, LEVEL_CURB_SHARE * ch)


# --- geometry -------------------------------------------------------------------------------------

def _sub(a, b): return (a[0] - b[0], a[1] - b[1])
def _add(a, b): return (a[0] + b[0], a[1] + b[1])
def _mul(a, k): return (a[0] * k, a[1] * k)
def _len(a): return math.hypot(a[0], a[1])


def _unit(a):
    n = _len(a)
    return (a[0] / n, a[1] / n) if n > 1e-6 else (1.0, 0.0)


def _seg_hit(p, p2, q, q2, ext=0.0):
    """Parameters (t, u) where segment p-p2 meets q-q2 (each may be extended by `ext` cm past its ends),
    or None when they are parallel or miss."""
    r = _sub(p2, p); s = _sub(q2, q)
    den = r[0] * s[1] - r[1] * s[0]
    if abs(den) < 1e-6:
        return None
    qp = _sub(q, p)
    t = (qp[0] * s[1] - qp[1] * s[0]) / den
    u = (qp[0] * r[1] - qp[1] * r[0]) / den
    et, eu = ext / max(_len(r), 1.0), ext / max(_len(s), 1.0)
    if -et <= t <= 1.0 + et and -eu <= u <= 1.0 + eu:
        return t, u
    return None


class _Graph:
    """Undirected graph of 2D nodes; an edge keeps its interior polyline points (from a to b)."""

    def __init__(self, snap):
        self.snap = snap
        self.nodes = []      # [(x, y)]
        self.z = []          # node heights
        self.edges = {}      # id -> [a, b, [interior (x, y, z)...]]
        self._eid = 0
        self.links = set()   # edge ids that cross a doorway between two rooms
        self.crossing_edges = set()   # edge ids that cross a street (they go down to the road by design)

    def node(self, p, z=0.0):
        for i, n in enumerate(self.nodes):
            if _len(_sub(n, p)) <= self.snap:
                return i
        self.nodes.append((p[0], p[1]))
        self.z.append(z)
        return len(self.nodes) - 1

    def add(self, a, b, pts=()):
        if a == b:
            return None
        for e in self.edges.values():
            if {e[0], e[1]} == {a, b}:
                return None  # one run per pair (a room's door-to-door routes come in both directions)
        self._eid += 1
        self.edges[self._eid] = [a, b, list(pts)]
        return self._eid

    def incident(self, n):
        return [k for k, e in self.edges.items() if e[0] == n or e[1] == n]

    def degree(self, n):
        return len(self.incident(n))

    def length(self, k):
        a, b, pts = self.edges[k]
        chain = [self.nodes[a]] + [(p[0], p[1]) for p in pts] + [self.nodes[b]]
        return sum(_len(_sub(chain[i + 1], chain[i])) for i in range(len(chain) - 1))

    def prune_stubs(self, max_len):
        """Drop dead-end edges shorter than max_len (the overhang of sidewalk sides past a corner)."""
        changed = True
        while changed:
            changed = False
            for k in list(self.edges):
                a, b, _ = self.edges[k]
                if (self.degree(a) == 1 or self.degree(b) == 1) and self.length(k) < max_len and self.degree(a) + self.degree(b) > 2:
                    del self.edges[k]
                    changed = True


def _crosswalk_ends(features):
    """[(curb end a, curb end b, z)] for every Crosswalk feature: the two ends of its centre line, at the curbs."""
    out = []
    for f in features or ():
        if f.get("kind") != "Crosswalk":
            continue
        loc, size = f.get("location", {}), f.get("size", {})
        yaw = math.radians((f.get("rotation") or {}).get("yaw", 0.0))
        d = (math.cos(yaw), math.sin(yaw))
        c = (loc.get("x", 0.0), loc.get("y", 0.0))
        h = size.get("x", 0.0) * 0.5
        if h > 10.0:
            out.append((_sub(c, _mul(d, h)), _add(c, _mul(d, h)), loc.get("z", 0.0)))
    return out


GRADE_BREAK = 0.003     # a route whose grade changes by more than this between its points gets a node there


def _seg_z(s, p):
    """The walking height of a run at point p: interpolated along its route's polyline (evenly spaced from the route's
    start to its end; on graded ground it follows the ground, on flat streets every point has the same height), by
    p's position along the route's original straight line."""
    pts = s["pts"]
    if len(pts) < 2:
        return s["z"]
    a0, b0 = s["a0"], s["b0"]
    v = _sub(b0, a0)
    L2 = v[0] * v[0] + v[1] * v[1]
    t = 0.0 if L2 < 1e-6 else ((p[0] - a0[0]) * v[0] + (p[1] - a0[1]) * v[1]) / L2
    f = min(max(t, 0.0), 1.0) * (len(pts) - 1)
    k = min(int(f), len(pts) - 2)
    z0, z1 = pts[k].get("z", 0.0), pts[k + 1].get("z", 0.0)
    return z0 + (z1 - z0) * (f - k)


def _grade_breaks(pts):
    """Fractions along a route where its grade bends: a run between two nodes is then one straight slope, so a lane
    interpolated between its ends keeps to the ground (graded streets bend where a street was dropped inside a block,
    and at a block's corner squares). A bend that falls between two of the route's points (two slope changes in a row)
    is placed where the straight grades either side of it meet. None on flat or evenly graded routes."""
    out = []
    n = len(pts)
    if n < 3:
        return out
    xs = [0.0]
    for i in range(1, n):
        xs.append(xs[-1] + math.hypot(pts[i]["x"] - pts[i - 1]["x"], pts[i]["y"] - pts[i - 1]["y"]))
    total = xs[-1]
    if total < 1.0:
        return out
    zs = [p.get("z", 0.0) for p in pts]
    slopes = [0.0 if xs[i + 1] - xs[i] < 1e-6 else (zs[i + 1] - zs[i]) / (xs[i + 1] - xs[i]) for i in range(n - 1)]
    bend = [False] + [abs(slopes[k] - slopes[k - 1]) > GRADE_BREAK for k in range(1, n - 1)] + [False]
    k = 1
    while k < n - 1:
        if not bend[k]:
            k += 1
            continue
        if bend[k + 1] and k + 1 < n - 1:
            # The bend is inside segment (k, k+1): where the grade before it (k-1..k) meets the grade after (k+1..k+2).
            sa, sb = slopes[k - 1], slopes[k + 1]
            if abs(sa - sb) > 1e-9:
                x = (zs[k + 1] - zs[k] + sa * xs[k] - sb * xs[k + 1]) / (sa - sb)
                if xs[k] < x < xs[k + 1]:
                    out.append(x / total)
                    k += 2
                    continue
        out.append(xs[k] / total)
        k += 1
    return out


def _outdoor_graph(routes, features=()):
    """Straight runs (sidewalk sides, park paths) cut at every crossing into a graph, joined across the
    streets along the crosswalks."""
    segs = []
    for r in routes:
        pts = r.get("points") or []
        if len(pts) < 2:
            continue
        a = (pts[0]["x"], pts[0]["y"]); b = (pts[-1]["x"], pts[-1]["y"])
        if _len(_sub(b, a)) < 50.0:
            continue
        segs.append({"a": a, "b": b, "a0": a, "b0": b, "pts": pts, "z": pts[0].get("z", 0.0),
                     "cuts": [0.0, 1.0] + _grade_breaks(pts), "name": r.get("name", "")})
    # A park's entrance paths stop at the back of the sidewalk, short of its centre line: carry each one on
    # to the sidewalk run it meets.
    for ps in segs:
        if not ps["name"].startswith("Park"):
            continue
        for ws in segs:
            if not ws["name"].startswith("Block"):
                continue
            h = _seg_hit(ps["a"], ps["b"], ws["a"], ws["b"], ext=300.0)
            if h and not 0.0 <= h[0] <= 1.0 and 0.0 <= h[1] <= 1.0:
                p = _add(ps["a"], _mul(_sub(ps["b"], ps["a"]), h[0]))
                ps["a" if h[0] < 0.0 else "b"] = p
    # Crossings and T-junctions: sidewalk sides overrun the corners a little, so allow 120 cm of extension.
    for i in range(len(segs)):
        for j in range(i + 1, len(segs)):
            h = _seg_hit(segs[i]["a"], segs[i]["b"], segs[j]["a"], segs[j]["b"], ext=120.0)
            if h:
                segs[i]["cuts"].append(min(max(h[0], 0.0), 1.0))
                segs[j]["cuts"].append(min(max(h[1], 0.0), 1.0))
    # Crosswalks: each curb end meets the sidewalk run parallel to the curb behind it, straight back from
    # the curb. That point becomes a cut (a node) on the run; the crossing is a run between the two.
    crossings = []
    unmatched = 0
    for ea, eb, zr in _crosswalk_ends(features):
        d = _unit(_sub(eb, ea))
        ends = []
        for e, out in ((ea, _mul(d, -1.0)), (eb, d)):
            best = None
            for si, s in enumerate(segs):
                if not s["name"].startswith("Block"):
                    continue
                v = _sub(s["b"], s["a"])
                L = _len(v)
                if L < 1.0 or abs((v[0] * d[0] + v[1] * d[1]) / L) > 0.1:
                    continue                         # not parallel to this curb
                t = ((e[0] - s["a"][0]) * v[0] + (e[1] - s["a"][1]) * v[1]) / (L * L)
                if not 0.02 < t < 0.98:
                    continue
                q = _add(s["a"], _mul(v, t))
                back = _sub(q, e)
                dist = back[0] * out[0] + back[1] * out[1]   # how far behind the curb, away from the road
                if 0.0 < dist < 700.0 and (best is None or dist < best[0]):
                    best = (dist, si, t, q)
            ends.append((e, best))
        if all(b is not None for _e, b in ends):
            for _e, b in ends:
                segs[b[1]]["cuts"].append(b[2])
            # Each end at its own sidewalk's height (on graded ground the two can differ).
            za = _seg_z(segs[ends[0][1][1]], ends[0][1][3])
            zb = _seg_z(segs[ends[1][1][1]], ends[1][1][3])
            crossings.append((ends[0][1][3], ends[0][0], ends[1][0], ends[1][1][3], za, zb, zr))
        else:
            unmatched += 1                           # no sidewalk run behind one of its curbs: no lane across
    g = _Graph(snap=60.0)
    g.crosswalks = len(crossings)
    g.crosswalks_unmatched = unmatched
    for s in segs:
        cuts = sorted(set(round(c, 5) for c in s["cuts"]))
        pts = [_add(s["a"], _mul(_sub(s["b"], s["a"]), c)) for c in cuts]
        ids = [g.node(p, _seg_z(s, p)) for p in pts]
        for k in range(len(ids) - 1):
            if _len(_sub(pts[k + 1], pts[k])) > 40.0:
                g.add(ids[k], ids[k + 1])
    g.prune_stubs(450.0)
    if crossings:
        for qa, ca, cb, qb, za, zb, zr in crossings:
            # Interior points 2 cm either side of both curb lines, so the lane's height is sampled on the
            # sidewalk (ramp foot) and on the road, never on the curb edge itself, where a trace can land on
            # the curb's top corner and put a spike in the lane.
            d = _unit(_sub(cb, ca))
            inner = [(p[0], p[1], z) for p, z in ((_sub(ca, _mul(d, 2.0)), za), (_add(ca, _mul(d, 2.0)), za), (_sub(cb, _mul(d, 2.0)), zb), (_add(cb, _mul(d, 2.0)), zb))]
            k = g.add(g.node(qa, za), g.node(qb, zb), inner)
            if k is not None:
                g.crossing_edges.add(k)
        return g
    # No crosswalks recorded (an older manifest): join each block's corners to the nearest corner of a neighbouring block straight across the street.
    corner_of = {}
    for s in segs:
        if s["name"].startswith("Block"):
            blk = s["name"].split("_")[0]
            for n in range(len(g.nodes)):
                p = g.nodes[n]
                if g.degree(n) >= 2 and min(_len(_sub(p, s["a"])), _len(_sub(p, s["b"]))) < 400.0:
                    corner_of.setdefault(n, blk)
    corners = [n for n in corner_of if g.degree(n) > 0]
    for n in corners:
        best = {}
        for m in corners:
            if corner_of[m] == corner_of[n]:
                continue
            d = _sub(g.nodes[m], g.nodes[n])
            dist = _len(d)
            if dist < 200.0 or dist > 2600.0:
                continue
            ax = abs(d[0]) < 0.15 * dist     # straight across along Y
            ay = abs(d[1]) < 0.15 * dist     # straight across along X
            if not (ax or ay):
                continue
            key = ("y" if ax else "x") + ("+" if (d[1] if ax else d[0]) > 0 else "-")
            if key not in best or dist < best[key][0]:
                best[key] = (dist, m)
        for _dist, m in best.values():
            k = g.add(n, m)
            if k is not None:
                g.crossing_edges.add(k)
    return g


def _indoor_graph(routes, features=()):
    """Room routes (door to door, or door to the room's centre, bent around the furniture) joined into one
    network: route ends in different rooms that face each other through a doorway are linked, and a shop's
    aisles become a ladder (each aisle's centre line, its ends joined to the next aisle's) tied to the routes."""
    g = _Graph(snap=45.0)
    room_of = {}
    door_end = set()
    per_room = {}
    for r in routes:
        per_room[r.get("name", "").split("_")[0]] = per_room.get(r.get("name", "").split("_")[0], 0) + 1
    for r in routes:
        pts = r.get("points") or []
        if len(pts) < 2:
            continue
        room = r.get("name", "").split("_")[0]
        a = g.node((pts[0]["x"], pts[0]["y"]), pts[0].get("z", 0.0))
        b = g.node((pts[-1]["x"], pts[-1]["y"]), pts[-1].get("z", 0.0))
        room_of.setdefault(a, set()).add(room)
        room_of.setdefault(b, set()).add(room)
        # A room with several doors has routes from door to door; a room with one door has a single route
        # from its door to its centre (the centre is not a doorway).
        door_end.add(a)
        if per_room[room] > 1:
            door_end.add(b)
        inner = [(p["x"], p["y"], p.get("z", 0.0)) for p in pts[1:-1]]
        g.add(a, b, inner)
    # Through the doorways: each door end links to the nearest door end of another room when that one
    # links back (mutual nearest) and they are no farther apart than a wall plus both approaches.
    ends = sorted(door_end)
    nearest = {}
    for a in ends:
        best = None
        for b in ends:
            if b == a or room_of[a] & room_of[b]:
                continue
            d = _len(_sub(g.nodes[a], g.nodes[b]))
            if d < 110.0 and (best is None or d < best[0]):
                best = (d, b)
        if best:
            nearest[a] = best[1]
    for a, b in nearest.items():
        if nearest.get(b) == a and a < b:
            k = g.add(a, b)
            if k:
                g.links.add(k)
    # Shop aisles: a ladder.
    aisles = []
    for f in features:
        if f.get("kind") != "Aisle":
            continue
        c = (f["location"]["x"], f["location"]["y"]); sz = f["size"]
        if sz["x"] >= sz["y"]:
            a, b = (c[0] - sz["x"] * 0.5 + 60.0, c[1]), (c[0] + sz["x"] * 0.5 - 60.0, c[1]); across = c[1]
        else:
            a, b = (c[0], c[1] - sz["y"] * 0.5 + 60.0), (c[0], c[1] + sz["y"] * 0.5 - 60.0); across = c[0]
        aisles.append((across, g.node(a, f["location"].get("z", 0.0)), g.node(b, f["location"].get("z", 0.0))))
    aisles.sort()
    for k, (_c, a, b) in enumerate(aisles):
        g.add(a, b)
        if k + 1 < len(aisles):
            g.add(a, aisles[k + 1][1])
            g.add(b, aisles[k + 1][2])
    if aisles:
        # Tie the ladder to the nearest route end (the shop's door routes), at both of its ends.
        route_nodes = [n for n in door_end]
        for corner in (aisles[0][1], aisles[0][2], aisles[-1][1], aisles[-1][2]):
            if route_nodes:
                m = min(route_nodes, key=lambda n: _len(_sub(g.nodes[n], g.nodes[corner])))
                if _len(_sub(g.nodes[m], g.nodes[corner])) < 1500.0:
                    g.add(corner, m)
    return g


# --- keeping lanes clear of walls, furniture and props ----------------------------------------------

class _Clearance:
    """Is a pedestrian of a given radius free to stand at (x, y) on the floor at height z? A capsule from
    30 cm above the floor (thresholds, curbs and floor slabs do not count) to head height, tested against
    everything that blocks (walls, furniture, props, vehicles). Results are cached on a 25 cm grid."""

    def __init__(self, world):
        self.world = world
        self.level_tol = None   # set while rerouting a run that keeps to one level: the ground must stay at it
        self.level_reach = 0.0  # ... out to at least this either side (cm): the outer edge of either lane's walkers
        # Graded ground: the walk's plane (x0, y0, z0, slope x, slope y) the level tests follow instead of one height
        # (a run on a 4 % grade, or across a cross-sloped walk, is still one walking level). None: a level height.
        self.plane = None
        self.plane_id = 0
        # ... and the curb ramps' footprints (_ramp_footprints), which count as the walk: on graded ground a ramp can
        # reach under the route's centre line, so a crossing's end stands on it.
        self.ramps = []
        self.cache = {}
        self.types = [unreal.ObjectTypeQuery.OBJECT_TYPE_QUERY1, unreal.ObjectTypeQuery.OBJECT_TYPE_QUERY2]
        self.calls = 0

    def set_plane(self, plane):
        self.plane = plane
        if plane is not None:
            self.plane_id += 1

    def plane_z(self, x, y):
        return _plane_z(self.plane, x, y)

    def free(self, x, y, z, r):
        # Cached at 5 cm, not the 25 cm search grid: a doorway is only ~8 cm wider than a person, and
        # snapping a test point to the grid would close it.
        lvl = self.level_tol
        reach = max(r, self.level_reach) if lvl is not None else 0.0
        on_plane = lvl is not None and self.plane is not None
        if on_plane:
            # The heights come from the plane, so the answer depends on the plane, not on z.
            key = (int(round(x / 5.0)), int(round(y / 5.0)), "plane", self.plane_id, int(r), lvl, int(reach))
        else:
            # A level answer depends on the floor height to within the tolerance, so it is cached on a finer z band.
            key = (int(round(x / 5.0)), int(round(y / 5.0)), int(round(z / (2.0 if lvl is not None else 20.0))), int(r), lvl, int(reach))
        hit = self.cache.get(key)
        if hit is None:
            self.calls += 1
            cx, cy = key[0] * 5.0, key[1] * 5.0
            zc = self.plane_z(cx, cy) if on_plane else z
            half = 70.0
            # Returns the overlapping components, or None when there are none (the bool return is folded in).
            res = unreal.SystemLibrary.capsule_overlap_components(
                self.world, unreal.Vector(cx, cy, zc + 30.0 + half), r, max(half, r), self.types,
                unreal.PrimitiveComponent.static_class(), [])
            if isinstance(res, tuple):
                res = res[-1] if res and res[0] else None
            hit = bool(res)
            if not hit and lvl is not None:
                # The capsule starts 30 cm up, so a curb's 15 cm drop into the road reads as free: on a run that
                # keeps to one level, ground off that level (or none) blocks too -- under the person's centre, and
                # a drop out to the outer edge of either lane's walkers (lane_half + AGENT_RADIUS), whatever radius
                # the search is squeezing through with (a centre line along the curb edge would put the
                # street-side lane and its walkers over the road). On graded ground "the level" is the walk's
                # plane, evaluated at each probe.
                if on_plane:
                    hit = not (_in_ramp(self.ramps, cx, cy) or _level_at(self.world, cx, cy, zc, lvl)) or not all(
                        _in_ramp(self.ramps, cx + dx, cy + dy) or _level_at(self.world, cx + dx, cy + dy, self.plane_z(cx + dx, cy + dy), lvl, raised_ok=True)
                        for dx, dy in ((reach, 0.0), (-reach, 0.0), (0.0, reach), (0.0, -reach)))
                else:
                    hit = not _level_at(self.world, cx, cy, z, lvl) or not all(
                        _level_at(self.world, cx + dx, cy + dy, z, lvl, raised_ok=True)
                        for dx, dy in ((reach, 0.0), (-reach, 0.0), (0.0, reach), (0.0, -reach)))
            self.cache[key] = hit
        return not hit

    def segment_free(self, a, b, za, zb, r):
        d = _len(_sub(b, a))
        n = max(1, int(d / 15.0))
        for i in range(n + 1):
            t = i / n
            if not self.free(a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, za + (zb - za) * t, r):
                return False
        return True

    def nearest_free(self, p, z, r, reach=120.0):
        if self.free(p[0], p[1], z, r):
            return p
        best = None
        steps = int(reach / CELL)
        for i in range(-steps, steps + 1):
            for j in range(-steps, steps + 1):
                q = (p[0] + i * CELL, p[1] + j * CELL)
                d = _len(_sub(q, p))
                if d <= reach and (best is None or d < best[0]) and self.free(q[0], q[1], z, r):
                    best = (d, q)
        return best[1] if best else None


def _on_level(world, chain, za, zb, tol=LEVEL_TOL, step=50.0):
    """Does the run keep to one walking level (a sidewalk, a floor) along its straight line, rather than
    going down to the road and back (a crossing)? Only a drop of more than tol counts: ground above the level
    on the line is whatever the run is being rerouted around (a planter, a raised tree pit), not another level."""
    total = sum(_len(_sub(chain[i + 1], chain[i])) for i in range(len(chain) - 1)) or 1.0
    done = 0.0
    for i in range(len(chain) - 1):
        a, b = chain[i], chain[i + 1]
        d = _len(_sub(b, a))
        n = max(1, int(d / step))
        for j in range(n + 1):
            t = j / n
            z = za + (zb - za) * ((done + d * t) / total)
            g = _ground_z(world, a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, z)
            if g < z - tol:
                return False
        done += d
    return True


def _astar(clr, a, b, za, zb, r, margin):
    """Shortest 8-connected path on the 25 cm grid from a to b keeping radius r free, inside the pair's
    bounding box grown by margin. Returns a list of points (a ... b) or None."""
    import heapq
    # The grid is laid so the start sits exactly on a cell centre: from a doorway, a path straight out
    # along the door's axis then keeps its full clearance (a misaligned grid would close an 80 cm door).
    x0 = a[0] - math.ceil((a[0] - (min(a[0], b[0]) - margin)) / CELL) * CELL
    y0 = a[1] - math.ceil((a[1] - (min(a[1], b[1]) - margin)) / CELL) * CELL
    x1, y1 = max(a[0], b[0]) + margin, max(a[1], b[1]) + margin
    nx, ny = int((x1 - x0) / CELL) + 1, int((y1 - y0) / CELL) + 1
    if nx * ny > 60000:
        return None
    L = _len(_sub(b, a)) or 1.0

    def cell(p): return (int(round((p[0] - x0) / CELL)), int(round((p[1] - y0) / CELL)))
    def pos(c): return (x0 + c[0] * CELL, y0 + c[1] * CELL)
    def zat(p):
        t = max(0.0, min(1.0, _len(_sub(p, a)) / L))
        return za + (zb - za) * t

    s, g = cell(a), cell(b)
    openq = [(0.0, s)]
    cost = {s: 0.0}
    came = {}
    nbrs = [(1, 0, 1.0), (-1, 0, 1.0), (0, 1, 1.0), (0, -1, 1.0), (1, 1, 1.414), (1, -1, 1.414), (-1, 1, 1.414), (-1, -1, 1.414)]
    while openq:
        _f, c = heapq.heappop(openq)
        pc = pos(c)
        # Done at the goal cell, or anywhere close to the goal with a clear straight run onto it (the goal
        # is usually off-grid, often in a doorway).
        if c == g or (_len(_sub(pc, b)) < 75.0 and clr.segment_free(pc, b, zat(pc), zb, r)):
            path = [b] if c == g else [b, pc]
            while c in came:
                c = came[c]
                path.append(pos(c))
            if len(path) > 1:
                path[-1] = a
            else:
                path.append(a)
            return list(reversed(path))
        for dx, dy, w in nbrs:
            n = (c[0] + dx, c[1] + dy)
            if not (0 <= n[0] < nx and 0 <= n[1] < ny):
                continue
            p = pos(n)
            if n != g and not clr.free(p[0], p[1], zat(p), r):
                continue
            nc = cost[c] + w
            if nc < cost.get(n, 1e18):
                cost[n] = nc
                came[n] = c
                h = math.hypot(g[0] - n[0], g[1] - n[1])
                heapq.heappush(openq, (nc + h, n))
    return None


def _pull(clr, pts, za, zb, r):
    """String-pull a grid path: keep a point only where the straight line to a later one is blocked. Each
    shortcut is tested at the heights its ends have along the path (za at the start, zb at the end, by distance
    walked), not at the run's end heights: on a sloped run a level test would otherwise read the far end as off
    its level and keep every grid step."""
    cum = [0.0]
    for i in range(1, len(pts)):
        cum.append(cum[-1] + _len(_sub(pts[i], pts[i - 1])))
    total = cum[-1] or 1.0
    zs = [za + (zb - za) * (c / total) for c in cum]
    out = [pts[0]]
    i = 0
    while i < len(pts) - 1:
        j = len(pts) - 1
        while j > i + 1 and not clr.segment_free(pts[i], pts[j], zs[i], zs[j], r):
            j -= 1
        out.append(pts[j])
        i = j
    return out


def _doorway(clr, a, b, z):
    """A doorway between route ends a and b (either side of a wall): its centre in the wall, found by
    scanning across the crossing for the free span, and the crossing direction. None when no gap is found."""
    d = _sub(b, a)
    ax = (1.0 if d[0] > 0 else -1.0, 0.0) if abs(d[0]) >= abs(d[1]) else (0.0, 1.0 if d[1] > 0 else -1.0)
    lat = (-ax[1], ax[0])
    mid = _mul(_add(a, b), 0.5)
    free = [o for o in range(-80, 81, 5) if clr.free(mid[0] + lat[0] * o, mid[1] + lat[1] * o, z, 12.0)]
    if not free:
        return None
    # The longest run of consecutive free offsets is the opening.
    runs, cur = [], [free[0]]
    for o in free[1:]:
        if o - cur[-1] == 5:
            cur.append(o)
        else:
            runs.append(cur); cur = [o]
    runs.append(cur)
    best = max(runs, key=len)
    c = (best[0] + best[-1]) * 0.5
    return _add(mid, _mul(lat, c)), ax


def _clear_lanes(g, world, lane_half, indoor, curb_height=None, graded=False, ramps=None):
    """Make every run of the graph clear for a pedestrian walking either of its lanes.

    Doorway crossings are straightened through the middle of the opening. Every other run end is nudged
    off furniture and out of door jambs, back toward its own room. A run whose line hits something is
    rerouted around it on a grid (first with room for both lanes, then with room for one person); a run
    with no way through is dropped. Outdoors, a nudged run end and a rerouted run that keeps to one walking
    level stay at that level's height (see _level_tol; curb_height is the streets' curb, None for the
    default). On graded ground (graded) "that level" is the walk's plane (_run_plane: fitted to the ground
    along the run; at a run end the plane of its longest sidewalk run), so a sloped or cross-sloped walk is one
    level and a curb is still a drop off it; the curb ramps (ramps: _ramp_footprints) count as the walk, since a
    crossing's end can stand on one; and a rerouted run's points take the plane's heights."""
    clr = _Clearance(world)
    if graded:
        clr.ramps = list(ramps or ())
    r_lane = lane_half + AGENT_RADIUS          # both lanes clear
    r_min = AGENT_RADIUS + 6.0                 # one person squeezing through a pinch point
    tol = None if indoor else _level_tol(curb_height)
    clr.level_reach = r_lane                   # a level test covers both lanes' walkers, whatever the radius
    stats = {"rerouted": 0, "dropped": 0, "doorways": 0}
    z = {n: _ground_z(world, g.nodes[n][0], g.nodes[n][1], g.z[n]) for n in range(len(g.nodes))}
    links = getattr(g, "links", set())

    # 1. Doorways: the crossing becomes a straight run through the opening's centre, 45 cm either side
    #    of the wall line, so the lane never clips a jamb.
    for k in list(links):
        if k not in g.edges:
            continue
        a, b, _pts = g.edges[k]
        dw = _doorway(clr, g.nodes[a], g.nodes[b], z[a])
        if dw is None:
            continue
        c, ax = dw
        p0, p1 = _sub(c, _mul(ax, 45.0)), _add(c, _mul(ax, 45.0))
        g.edges[k][2] = [(p0[0], p0[1], z[a]), (c[0], c[1], z[a]), (p1[0], p1[1], z[b])]
        stats["doorways"] += 1

    # 2. Run ends: off furniture and jambs, moving back along the node's own (non-doorway) runs.
    for n in range(len(g.nodes)):
        inc = g.incident(n)
        if not inc or clr.free(g.nodes[n][0], g.nodes[n][1], z[n], r_min):
            continue
        dirs = []
        for k in inc:
            if k in links:
                continue
            a, b, pts = g.edges[k]
            chain = [g.nodes[a]] + [(p[0], p[1]) for p in pts] + [g.nodes[b]]
            nxt = chain[1] if a == n else chain[-2]
            dirs.append(_unit(_sub(nxt, g.nodes[n])))
        q = None
        # Outdoors the end moves only on its own level: a sidewalk node blocked by a bench is not pushed off
        # the curb (at a crossing the runs' mean direction points at the street).
        clr.level_tol = tol
        pl = None
        if graded and tol is not None:
            # The plane of the end's longest sidewalk run (fitted along it, so the end standing on a heaved panel or a
            # ramp does not tilt it); the plane at the end itself when it has none.
            best = None
            for k in inc:
                if k in links or k in g.crossing_edges:
                    continue
                if best is None or g.length(k) > g.length(best):
                    best = k
            if best is not None:
                ea, eb, _ep = g.edges[best]
                pl = _run_plane(world, g.nodes[ea], g.nodes[eb], z[ea], z[eb], clr.ramps)
            else:
                pl = _node_plane(world, g.nodes[n], z[n])
        clr.set_plane(pl)
        try:
            if dirs:
                dv = _unit((sum(d[0] for d in dirs), sum(d[1] for d in dirs)))
                for step in range(1, 31):
                    cand = _add(g.nodes[n], _mul(dv, step * 5.0))
                    if clr.free(cand[0], cand[1], z[n], r_min):
                        q = cand
                        break
            if q is None:
                q = clr.nearest_free(g.nodes[n], z[n], r_min, 150.0)
        finally:
            clr.level_tol = None
            clr.set_plane(None)
        if q is None:
            for k in inc:
                del g.edges[k]
                stats["dropped"] += 1
            continue
        g.nodes[n] = q
        if pl is not None:
            # On a slope the end's height moves with it (and the lanes are laid from it).
            z[n] = _ground_z(world, q[0], q[1], _plane_z(pl, q[0], q[1]))
            g.z[n] = z[n]

    # 3. Runs.
    margin = 300.0 if indoor else 450.0
    for k in list(g.edges):
        a, b, pts = g.edges[k]
        chain = [g.nodes[a]] + [(p[0], p[1]) for p in pts] + [g.nodes[b]]
        za, zb = z[a], z[b]
        segs = range(len(chain) - 1)
        if all(clr.segment_free(chain[i], chain[i + 1], za, zb, r_lane) for i in segs):
            continue
        if k in links:
            # A doorway: passable if a person fits along the straightened line; otherwise rerouted.
            if all(clr.segment_free(chain[i], chain[i + 1], za, zb, 15.0) for i in segs):
                continue
        ok_min = all(clr.segment_free(chain[i], chain[i + 1], za, zb, r_min) for i in segs)
        # A run on one level (a sidewalk) is rerouted on that level only: around a tree or a bench, not off the
        # curb and down the street. A crossing, which goes down to the road by design, is not constrained.
        rp = None
        if graded:
            # Graded ground: the level is the run's own plane, fitted along it.
            if tol is not None and k not in g.crossing_edges:
                rp = _run_plane(world, chain[0], chain[-1], za, zb, clr.ramps)
                if not _on_plane_level(world, chain, rp, tol + LEVEL_TOL_GRADED, clr.ramps):
                    rp = None
            on_level = rp is not None
        else:
            on_level = tol is not None and k not in g.crossing_edges and _on_level(world, chain, za, zb, tol)
        clr.level_tol = tol if on_level else None
        clr.set_plane(rp)
        path = _astar(clr, chain[0], chain[-1], za, zb, r_lane, margin)
        r_used = r_lane
        if path is None and not ok_min:
            path = _astar(clr, chain[0], chain[-1], za, zb, r_min, margin)
            r_used = r_min
        if path is None and not ok_min:
            path = _astar(clr, chain[0], chain[-1], za, zb, 24.0, margin)
            r_used = 24.0
        if path is None:
            clr.level_tol = None
            clr.set_plane(None)
            if ok_min:
                continue            # tight but passable as laid out: keep it
            del g.edges[k]
            stats["dropped"] += 1
            stats.setdefault("dropped_runs", []).append([round(v) for v in chain[0] + chain[-1]])
            continue
        path = _pull(clr, path, za, zb, r_used)
        clr.level_tol = None
        clr.set_plane(None)
        if graded:
            # On graded ground each point at its height: on the run's plane, else by the distance walked (a height by
            # point count can put a point near one end a metre off a steep run's ground, and the lane would float).
            cum = [0.0]
            for i in range(1, len(path)):
                cum.append(cum[-1] + _len(_sub(path[i], path[i - 1])))
            total = cum[-1] or 1.0
            g.edges[k][2] = [(p[0], p[1], _plane_z(rp, p[0], p[1]) if rp is not None else za + (zb - za) * (cum[i] / total))
                             for i, p in enumerate(path[1:-1], 1)]
        else:
            g.edges[k][2] = [(p[0], p[1], za + (zb - za) * (i / max(len(path) - 1, 1))) for i, p in enumerate(path[1:-1], 1)]
        stats["rerouted"] += 1
    stats["queries"] = clr.calls
    return stats


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


def _trace_hit_z(world, x, y, z):
    """Height of the first blocking surface going down from z + 60 cm to z - 120 cm at (x, y), or None."""
    s = unreal.Vector(x, y, z + 60.0)
    e = unreal.Vector(x, y, z - 120.0)
    hit = unreal.SystemLibrary.line_trace_single(world, s, e, unreal.TraceTypeQuery.TRACE_TYPE_QUERY1, False, [],
                                                 unreal.DrawDebugTrace.NONE, True)
    if hit:
        t = hit.to_tuple()
        if t and t[0]:
            loc = next((v for v in t if isinstance(v, unreal.Vector)), None)
            if loc is not None:
                return loc.z
    return None


def _trace_down(world, x, y, z):
    """Walking surface height at (x, y) near z: the first hit going down, None when there is none or it stands
    more than 40 cm above z (a bench seat, a tabletop: not ground)."""
    g = _trace_hit_z(world, x, y, z)
    return g if g is not None and g <= z + 40.0 else None


SEAM_OFFSETS = ((2.0, 2.0), (-2.0, -2.0), (2.0, -2.0), (-2.0, 2.0))


def _level_at(world, x, y, z, tol, raised_ok=False):
    """Is there walking ground at (x, y) no more than tol below z and no more than LEVEL_RISE above it? With
    raised_ok, anything not more than tol below z will do: a raised surface (a planter rim, a bench seat, the
    top of a box, at any height) is something standing there, which the clearance capsule judges, not a drop.
    A trace on the seam between two slabs can slip through: a miss is retried 2 cm to either side, as
    _ground_z does."""
    def ok(g):
        return g is not None and g >= z - tol and (raised_ok or g <= z + LEVEL_RISE)
    return ok(_trace_hit_z(world, x, y, z)) or any(ok(_trace_hit_z(world, x + dx, y + dy, z)) for dx, dy in SEAM_OFFSETS)


def _ground_z(world, x, y, z):
    """Walking surface height at (x, y): the first hit going down from just above the route, ignoring
    anything standing more than 40 cm above it (props, furniture). A line exactly on the seam between two
    abutting slabs can slip between them (a lane rerouted on the 25 cm grid lands on such seams): when the
    trace finds nothing or a drop of more than a curb, it is repeated 2 cm to either side and the highest
    surface wins."""
    g = _trace_down(world, x, y, z)
    if g is None or g < z - 20.0:
        alts = [h for h in (_trace_down(world, x + dx, y + dy, z) for dx, dy in SEAM_OFFSETS) if h is not None]
        if alts:
            g = max(alts + ([g] if g is not None else []))
    return z if g is None else g


LOCAL_SLOPE_MAX = 0.15  # graded ground: steeper than this between two samples 20 cm apart is a step, not the walk's slope
SLOPE_PROBE = 20.0      # ... and that spacing (cm)
LEVEL_TOL_GRADED = 1.5  # graded, rough ground: a run is still on its level over a sunken cover or a panel's lip (cm more)


def _ground_or_none(world, x, y, z):
    """Walking surface near z at (x, y), as _ground_z but None where there is none (seams retried 2 cm aside)."""
    g = _trace_down(world, x, y, z)
    if g is None:
        alts = [h for h in (_trace_down(world, x + dx, y + dy, z) for dx, dy in SEAM_OFFSETS) if h is not None]
        g = max(alts) if alts else None
    return g


def _local_slope(world, x, y, z, d):
    """Graded ground: the walk's slope along the unit direction d at (x, y): the ground every 20 cm from 40 cm before
    to 40 cm after the point, and the median of the four slopes between neighbouring samples that are slopes at all
    (LOCAL_SLOPE_MAX). A curb edge, a panel's lip or a cover between two samples is a step, steeper than that or
    outvoted, so it does not tilt the walk; 0 where nothing can be measured."""
    return _local_slope_level(world, x, y, z, d)[0]


def _median(vals):
    v = sorted(vals)
    m = len(v) // 2
    return v[m] if len(v) % 2 else 0.5 * (v[m - 1] + v[m])


def _local_slope_level(world, x, y, z, d):
    """_local_slope, and the walk's height at (x, y) on that slope: the median of the five samples brought back to the
    point along it, so a panel set a little high or low under the point does not shift the level (None: no ground)."""
    c = _ground_or_none(world, x, y, z)
    if c is None:
        return 0.0, None
    hs = []
    for k in (-2, -1, 0, 1, 2):
        if k == 0:
            hs.append(c)
            continue
        hs.append(_ground_or_none(world, x + d[0] * SLOPE_PROBE * k, y + d[1] * SLOPE_PROBE * k, c))
    ss = [(hs[i + 1] - hs[i]) / SLOPE_PROBE for i in range(4)
          if hs[i] is not None and hs[i + 1] is not None and abs(hs[i + 1] - hs[i]) <= LOCAL_SLOPE_MAX * SLOPE_PROBE]
    s = _median(ss) if ss else 0.0
    level = _median([hs[i] - s * SLOPE_PROBE * (i - 2) for i in range(5) if hs[i] is not None])
    return s, level


def _node_plane(world, p, z):
    """Graded ground: the walk's plane at a run end (x0, y0, z0, slope x, slope y), its height the walk's own there (a
    panel set high or low under the end does not lift the plane)."""
    sx, hx = _local_slope_level(world, p[0], p[1], z, (1.0, 0.0))
    sy, hy = _local_slope_level(world, p[0], p[1], z, (0.0, 1.0))
    hs = [h for h in (hx, hy) if h is not None]
    return (p[0], p[1], sum(hs) / len(hs) if hs else z, sx, sy)


RAMP_MARGIN = 10.0      # graded ground: a curb ramp's footprint, grown by this (cm), counts as the walk


def _ramp_footprints(features):
    """The manifest's curb ramps as footprints (centre x, y, outward unit x, y, half run, half width with flares)."""
    out = []
    for f in features or ():
        if f.get("kind") != "CurbRamp":
            continue
        loc, size = f.get("location") or {}, f.get("size") or {}
        yaw = math.radians((f.get("rotation") or {}).get("yaw", 0.0))
        out.append((loc.get("x", 0.0), loc.get("y", 0.0), math.cos(yaw), math.sin(yaw),
                    size.get("x", 0.0) * 0.5 + RAMP_MARGIN, size.get("y", 0.0) * 0.5 + RAMP_MARGIN))
    return out


def _in_ramp(ramps, x, y):
    for cx, cy, ux, uy, hx, hy in ramps or ():
        dx, dy = x - cx, y - cy
        if abs(dx * ux + dy * uy) <= hx and abs(dy * ux - dx * uy) <= hy:
            return True
    return False


def _run_plane(world, a, b, za, zb, ramps=()):
    """Graded ground: the plane of a straight run's walk, fitted to the ground along the run (every 50 cm, off the
    curb ramps): its grade the median of the slopes between neighbouring samples, its height the median of the
    samples brought back to its start, and the walk's cross slope across it the median of five samples. A run end on
    a heaved panel or on a ramp, a lip or a prop under a sample does not tilt or lift it."""
    d = _sub(b, a)
    L = _len(d)
    if L < 1.0:
        return _node_plane(world, a, za)
    u = (d[0] / L, d[1] / L)
    v = (-u[1], u[0])
    n = max(2, int(L / 50.0))
    samples = []
    for i in range(n + 1):
        t = i / n
        x, y = a[0] + d[0] * t, a[1] + d[1] * t
        if _in_ramp(ramps, x, y):
            continue
        gz = _ground_or_none(world, x, y, za + (zb - za) * t)
        if gz is not None:
            samples.append((L * t, gz))
    su, z0 = (zb - za) / L, za
    if len(samples) >= 3:
        slopes = [(g1 - g0) / (s1 - s0) for (s0, g0), (s1, g1) in zip(samples, samples[1:])
                  if s1 - s0 > 1.0 and abs(g1 - g0) <= LOCAL_SLOPE_MAX * (s1 - s0)]
        if slopes:
            su = _median(slopes)
        z0 = _median([gz - su * s for s, gz in samples])
    svs = []
    for t in (1.0 / 6.0, 2.0 / 6.0, 0.5, 4.0 / 6.0, 5.0 / 6.0):
        x, y = a[0] + d[0] * t, a[1] + d[1] * t
        if not _in_ramp(ramps, x, y):
            svs.append(_local_slope(world, x, y, z0 + su * L * t, v))
    sv = _median(svs) if svs else 0.0
    return (a[0], a[1], z0, su * u[0] + sv * v[0], su * u[1] + sv * v[1])


def _plane_z(plane, x, y):
    x0, y0, z0, gx, gy = plane
    return z0 + gx * (x - x0) + gy * (y - y0)


def _on_plane_level(world, chain, plane, tol, ramps=(), step=50.0):
    """Graded ground's _on_level: does the run's line keep to its plane (no drop of more than tol below it, off the curb
    ramps)?"""
    for i in range(len(chain) - 1):
        a, b = chain[i], chain[i + 1]
        n = max(1, int(_len(_sub(b, a)) / step))
        for j in range(n + 1):
            t = j / n
            x, y = a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t
            if _in_ramp(ramps, x, y):
                continue
            ze = _plane_z(plane, x, y)
            if _ground_z(world, x, y, ze) < ze - tol:
                return False
    return True


def _graded_ground(gen):
    """Was the level generated on graded or rough ground (any parameter that switches the Street Network to its ground
    model above 0)? Its level tests then follow the walk's plane; on flat streets they keep to one height, as they
    always have. RootHeaveChance on its own lays its heaves on the flat walk: not graded."""
    if gen is None:
        return False
    names = ("max_street_grade", "road_crown", "sidewalk_cross_slope", "non_compliant_cross_slope_chance", "curb_height_max",
             "surface_roughness", "heave_density", "crack_density", "cover_density", "paving_patch_density",
             "pothole_density", "speed_hump_chance")
    for get in (lambda: gen.get_editor_property("params"), lambda: gen.get_editor_property("preset").get_editor_property("params")):
        try:
            p = get()
            return any(float(p.get_editor_property(n)) > 0.0 for n in names)
        except Exception:  # noqa: BLE001 - no params, or a build without the terrain parameters
            continue
    return False


GROUND_STEP = 50.0      # lane sampling interval along a run, cm


def _follow_ground(world, pts):
    """The polyline pts [(x, y, z)] resampled every GROUND_STEP cm onto the walking surface below it.
    Each sample traces down from the previous sample's height, so a run steps down a curb ramp or the
    road and back up the far side; samples where the ground carries on in a straight line are dropped
    again (the corners of the input are always kept)."""
    out = []
    zref = pts[0][2]
    for i in range(len(pts) - 1):
        a, b = pts[i], pts[i + 1]
        L = _len(_sub(b[:2], a[:2]))
        n = max(1, int(math.ceil(L / GROUND_STEP)))
        for k in range(n):
            t = k / n
            x, y = a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t
            zref = _ground_z(world, x, y, max(zref, a[2] + (b[2] - a[2]) * t))
            out.append([x, y, zref, k == 0])
    last = pts[-1]
    out.append([last[0], last[1], _ground_z(world, last[0], last[1], max(zref, last[2])), True])
    # Drop samples that lie on the straight line (in height too) between their neighbours.
    keep = [out[0]]
    for i in range(1, len(out) - 1):
        p, nx = out[i], out[i + 1]
        if p[3]:
            keep.append(p)
            continue
        q = keep[-1]
        dq = _len(_sub(p[:2], q[:2])); dn = _len(_sub(nx[:2], p[:2]))
        zl = q[2] + (nx[2] - q[2]) * dq / max(dq + dn, 1e-6)
        if abs(p[2] - zl) > 1.0:
            keep.append(p)
    keep.append(out[-1])
    return [(p[0], p[1], p[2]) for p in keep]


def _shape(points, shape_type, profile_ref, label, tags=()):
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actor = eas.spawn_actor_from_class(unreal.ZoneShape, unreal.Vector(0, 0, 0))
    actor.set_actor_label(label)
    actor.tags = [unreal.Name(TAG)] + [unreal.Name(t) for t in tags]
    actor.set_folder_path(unreal.Name(FOLDER))
    comp = actor.get_component_by_class(unreal.ZoneShapeComponent)
    comp.set_shape_type(shape_type)
    comp.set_editor_property("lane_profile", profile_ref)
    comp.set_editor_property("points", points)
    unreal.RammsCrowdZoneGraphLibrary.update_zone_shape(comp)
    return actor


def _pt(x, y, z, ptype, yaw=0.0):
    p = unreal.ZoneShapePoint()
    p.position = unreal.Vector(x, y, z)
    p.type = ptype
    p.rotation = unreal.Rotator(roll=0.0, pitch=0.0, yaw=yaw)
    return p


def _emit(g, profile_kind, world, label_prefix):
    """Polygon junctions at branch nodes, splines along the runs between them. Returns (shapes, lane cm)."""
    ref, width = _profile_ref(profile_kind)
    clr = _Clearance(world)
    T = unreal.FZoneShapePointType
    # Junctions: nodes of degree >= 3, plus one node on any loop without one.
    junction = {n for n in range(len(g.nodes)) if g.degree(n) >= 3}
    # Walk chains between junctions / dead ends.
    used = set()
    chains = []

    def neighbours(n, k):
        a, b, pts = g.edges[k]
        if a == n:
            return b, list(pts)
        return a, list(reversed(pts))

    def walk(start, k):
        nodes = [start]
        inner = []
        crossing = False
        cur = start
        while True:
            used.add(k)
            crossing = crossing or k in g.crossing_edges
            nxt, pts = neighbours(cur, k)
            inner += [(p[0], p[1]) + ((p[2],) if len(p) > 2 else (g.z[cur],)) for p in pts]
            if nxt in junction or g.degree(nxt) != 2:
                nodes.append(nxt)
                return nodes, inner, crossing
            inner.append((g.nodes[nxt][0], g.nodes[nxt][1], g.z[nxt]))
            nodes.append(nxt)
            ks = [x for x in g.incident(nxt) if x not in used]
            if not ks:
                return nodes, inner, crossing
            cur, k = nxt, ks[0]

    for n in sorted(junction) + [n for n in range(len(g.nodes)) if g.degree(n) == 1]:
        for k in g.incident(n):
            if k not in used:
                chains.append(walk(n, k))
    # What is left are closed loops with no junction: make their first node one and walk them.
    for k in list(g.edges):
        if k in used:
            continue
        n = g.edges[k][0]
        junction.add(n)
        for k2 in g.incident(n):
            if k2 not in used:
                chains.append(walk(n, k2))

    # Junction radius: half the lane profile plus a margin, smaller when runs are short.
    radius = {}
    for n in junction:
        lens = [g.length(k) for k in g.incident(n)]
        radius[n] = max(min(width * 0.5 + 25.0, (min(lens) if lens else 1e9) * 0.35), 30.0)
        # The junction's lanes curve inside this circle: keep it clear of walls, jambs and props.
        zc = _ground_z(world, g.nodes[n][0], g.nodes[n][1], g.z[n])
        while radius[n] > 30.0 and not clr.free(g.nodes[n][0], g.nodes[n][1], zc, max(radius[n], width * 0.5) + AGENT_RADIUS * 0.5):
            radius[n] -= 5.0

    shapes = 0
    lane_cm = 0.0
    entries = {n: [] for n in junction}
    for idx, (nodes, inner, crossing) in enumerate(chains):
        a, b = nodes[0], nodes[-1]
        pts = [(g.nodes[a][0], g.nodes[a][1], g.z[a])] + inner + [(g.nodes[b][0], g.nodes[b][1], g.z[b])]
        if len(pts) < 2:
            continue
        # Pull each end back onto its junction's boundary, along the run's first / last direction.
        ents = []
        if a in junction:
            d = _unit(_sub(pts[1][:2], pts[0][:2]))
            p0 = _add(pts[0][:2], _mul(d, radius[a]))
            pts[0] = (p0[0], p0[1], pts[0][2])
            ents.append((a, (math.atan2(d[1], d[0]), p0, pts[0][2])))
        if b in junction:
            d = _unit(_sub(pts[-2][:2], pts[-1][:2]))
            p1 = _add(pts[-1][:2], _mul(d, radius[b]))
            pts[-1] = (p1[0], p1[1], pts[-1][2])
            ents.append((b, (math.atan2(d[1], d[0]), p1, pts[-1][2])))
        # Drop interior points that the pulled-back ends now pass (very short runs).
        clean = [pts[0]]
        for p in pts[1:-1]:
            if _len(_sub(p[:2], clean[-1][:2])) > 30.0 and _len(_sub(p[:2], pts[-1][:2])) > 30.0:
                clean.append(p)
        clean.append(pts[-1])
        if _len(_sub(clean[-1][:2], clean[0][:2])) < 30.0:
            continue
        # Only a run that is laid gets an entry in its junctions' outlines (else the polygon has a lane
        # profile leading nowhere).
        for n, e in ents:
            entries[n].append(e)
        # Sharp points with a small turn radius: auto-bezier corners bulge out over the street.
        sp = []
        for x, y, z in _follow_ground(world, clean):
            q = _pt(x, y, z + 1.0, T.SHARP)
            q.inner_turn_radius = 40.0
            sp.append(q)
        _shape(sp, unreal.FZoneShapeType.SPLINE, ref, "%s_Lane%02d" % (label_prefix, idx), (CROSSING_TAG,) if crossing else ())
        shapes += 1
        lane_cm += sum(_len(_sub(clean[i + 1][:2], clean[i][:2])) for i in range(len(clean) - 1))

    for n, ents in entries.items():
        if len(ents) < 2:
            continue
        c = g.nodes[n]
        cz = _ground_z(world, c[0], c[1], g.z[n]) + 1.0
        ents.sort(key=lambda e: e[0])
        poly = []
        for i, (ang, p, _z) in enumerate(ents):
            # Lane-profile point on the boundary, facing into the junction, on the ground where it is (a
            # junction at the top of a ramp or on a slope is not flat).
            pz = _ground_z(world, p[0], p[1], cz - 1.0) + 1.0
            poly.append(_pt(p[0], p[1], pz, T.LANE_PROFILE, math.degrees(ang) + 180.0))
            # A sharp corner between this run and the next one, out along the bisector.
            nxt = ents[(i + 1) % len(ents)][0]
            gap = (nxt - ang) % (2.0 * math.pi)
            if gap < 1e-3:
                gap = 2.0 * math.pi
            mid = ang + gap * 0.5
            # On the junction's (cleared) circle, not beyond it: the outline must not poke through a wall.
            r = radius[n] * (1.0 if gap < math.pi else 0.85)
            qx, qy = c[0] + math.cos(mid) * r, c[1] + math.sin(mid) * r
            poly.append(_pt(qx, qy, _ground_z(world, qx, qy, cz - 1.0) + 1.0, T.SHARP))
        _shape(poly, unreal.FZoneShapeType.POLYGON, ref, "%s_Junction%02d" % (label_prefix, n))
        shapes += 1
    return shapes, lane_cm


BLOCKED = []       # (shape, from, to) of every segment the last check_lanes() found blocked


def check_lanes(world=None):
    """Audit the level's crowd shapes against the geometry: (spline segments whose centre line comes within
    15 cm of something, polygon outline edges that cross something). Both should be 0."""
    world = world or unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    clr = _Clearance(world)
    bad_lane = bad_poly = 0
    del BLOCKED[:]
    for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors():
        if not isinstance(a, unreal.ZoneShape) or not a.actor_has_tag(TAG):
            continue
        comp = a.get_component_by_class(unreal.ZoneShapeComponent)
        P = [(p.position.x, p.position.y, p.position.z - 1.0) for p in comp.get_editor_property("points")]
        poly = "POLYGON" in str(comp.get_shape_type())
        segs = list(zip(P, P[1:] + ([P[0]] if poly else [])))
        for p0, p1 in segs:
            if not clr.segment_free(p0[:2], p1[:2], p0[2], p1[2], 4.0 if poly else 15.0):
                BLOCKED.append((a.get_actor_label(), [round(v) for v in p0], [round(v) for v in p1]))
                if poly:
                    bad_poly += 1
                else:
                    bad_lane += 1
    return bad_lane, bad_poly


OFF_LEVEL = []     # (shape, sample) of the first lane samples the last check_lane_levels() found off their level


CURB_FLOOR = 2.0   # the lowest curb the Street Network draws when curbs vary (cm; PCGRammsStreetNetwork.cpp)


def _curb_height(gen):
    """The streets' curb height (cm): the generator's resolved parameters, else its preset's; None when neither
    can be read. With varied curbs (CurbHeightMax > 0) the lowest one, so a low curb still bounds a level:
    CurbHeightMin, never under the Street Network's floor (CURB_FLOOR)."""
    if gen is None:
        return None
    for get in (lambda: gen.get_editor_property("params"), lambda: gen.get_editor_property("preset").get_editor_property("params")):
        try:
            p = get()
            ch = float(p.get_editor_property("curb_height"))
            try:
                hi = float(p.get_editor_property("curb_height_max"))
                lo = float(p.get_editor_property("curb_height_min"))
            except Exception:  # noqa: BLE001 - an engine build without the terrain parameters
                hi = lo = 0.0
            return max(min(lo, hi), CURB_FLOOR) if hi > 0.0 else ch
        except Exception:  # noqa: BLE001
            continue
    return None


def check_lane_levels(world=None, curb_height=None, step=25.0, graded=None, ramps=None):
    """Audit the level's sidewalk lanes against the ground: every `step` cm along each lane shape's centre line,
    is the ground under it, and at the outer edge of either lane's walkers (lane_half + AGENT_RADIUS either side),
    no more than the level tolerance (_level_tol) below the lane? A sample that fails has a lane over the road
    or down a curb. Not audited: shapes that carry a street crossing (tagged CROSSING_TAG; they go down to the
    road by design), junction polygons and indoor (narrow-profile) lanes. curb_height None reads it from the
    level's generator. On graded ground (graded; None reads it from the generator, _graded_ground) the walkers' edges
    are measured against the walk's cross slope at the sample, not the lane's own height: a cross-sloped walk is one
    level, a curb a drop off it, and the curb ramps count as the walk (ramps: _ramp_footprints; None reads them from
    the generator's manifest). Returns the number of failing samples, 0 when every lane keeps to its sidewalk;
    OFF_LEVEL keeps the first ones."""
    world = world or unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    del OFF_LEVEL[:]
    if curb_height is None or graded is None or (graded and ramps is None):
        try:
            gen = H.find_generator()
        except Exception:  # noqa: BLE001
            gen = None
        if curb_height is None:
            curb_height = _curb_height(gen)
        if graded is None:
            graded = _graded_ground(gen)
        if graded and ramps is None and gen is not None:
            try:
                ramps = _ramp_footprints(H.manifest_to_dict(gen).get("features", []))
            except Exception:  # noqa: BLE001
                ramps = []
    tol = _level_tol(curb_height)
    if tol is None:
        return 0            # no real curb: no level to keep
    profile, _guid, width = LANE_PROFILES["sidewalk"]
    reach = width * 0.25 + AGENT_RADIUS
    bad = 0
    for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors():
        if not isinstance(a, unreal.ZoneShape) or not a.actor_has_tag(TAG) or a.actor_has_tag(CROSSING_TAG):
            continue
        comp = a.get_component_by_class(unreal.ZoneShapeComponent)
        if "POLYGON" in str(comp.get_shape_type()):
            continue
        try:
            if str(comp.get_editor_property("lane_profile").get_editor_property("name")) != profile:
                continue
        except Exception:  # noqa: BLE001 - audit it anyway
            pass
        P = [(p.position.x, p.position.y, p.position.z - 1.0) for p in comp.get_editor_property("points")]
        samples = []
        for p0, p1 in zip(P, P[1:]):
            d = _len(_sub(p1[:2], p0[:2]))
            if d < 1.0:
                continue
            u = _unit(_sub(p1[:2], p0[:2]))
            n = max(1, int(math.ceil(d / step)))
            # Each segment's samples stop short of its end, which is the next segment's start.
            samples += [(p0[0] + (p1[0] - p0[0]) * (i / n), p0[1] + (p1[1] - p0[1]) * (i / n), p0[2] + (p1[2] - p0[2]) * (i / n), u) for i in range(n)]
        if len(P) > 1:
            samples.append(P[-1] + (_unit(_sub(P[-1][:2], P[-2][:2])),))
        for x, y, z, u in samples:
            lat = (-u[1] * reach, u[0] * reach)
            if graded:
                # The walk's cross slope here (along lat): the edges' ground is expected that much higher or lower; a
                # point on a curb ramp is on the walk.
                s = _local_slope(world, x, y, z, (-u[1], u[0]))
                probes = ((0.0, 0.0, z), (lat[0], lat[1], z + s * reach), (-lat[0], -lat[1], z - s * reach))
                if all(_in_ramp(ramps, x + ox, y + oy) or _level_at(world, x + ox, y + oy, oz, tol, raised_ok=True) for ox, oy, oz in probes):
                    continue
            elif all(_level_at(world, x + ox, y + oy, z, tol, raised_ok=True) for ox, oy in ((0.0, 0.0), lat, (-lat[0], -lat[1]))):
                continue
            bad += 1
            if len(OFF_LEVEL) < 50:
                OFF_LEVEL.append((a.get_actor_label(), [round(x), round(y), round(z)]))
    return bad


SIGNALIZED_ACTOR_TAG = "RammsTrafficSignalized"   # traffic.py's tag on the shapes of signalized intersections
CROSSING_ZG_TAG = "Crossing"                       # ZoneGraph tag (MassCrowd's CrossingTag): a lane a signal opens and closes


def _signalized_areas():
    """[(x, y, radius)] around every signalized intersection traffic.py laid in the level (none without traffic)."""
    out = []
    for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors():
        if isinstance(a, unreal.ZoneShape) and a.actor_has_tag(SIGNALIZED_ACTOR_TAG):
            o, e = a.get_actor_bounds(False)
            out.append((o.x, o.y, max(e.x, e.y) + 300.0))
    return out


def tag_crossings(areas=None, tag=CROSSING_ZG_TAG):
    """Give the crowd's street-crossing shapes (actor tag CROSSING_TAG) whose middle lies in one of `areas`
    [(x, y, radius)] the ZoneGraph tag `tag`, and take it off the others. MassCrowd closes a crossing lane with that
    tag until something opens it -- the traffic signal clock does, for Walk -- so only crossings a signal controls
    may carry it. `areas` None: the signalized intersections in the level. Takes effect when the ZoneGraph is
    rebuilt. Returns how many shapes carry the tag."""
    areas = _signalized_areas() if areas is None else list(areas)
    lib = unreal.RammsCrowdZoneGraphLibrary
    if not hasattr(lib, "set_zone_shape_tags"):
        if areas:
            H.warn("crowd: RammsCrowdZoneGraphLibrary.set_zone_shape_tags missing (rebuild RammsCrowd); crossings not tied to the signals")
        return 0
    tagged = 0
    for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors():
        if not isinstance(a, unreal.ZoneShape) or not a.actor_has_tag(TAG) or not a.actor_has_tag(CROSSING_TAG):
            continue
        comp = a.get_component_by_class(unreal.ZoneShapeComponent)
        P = [p.position for p in comp.get_editor_property("points")]
        if not P:
            continue
        mx, my = sum(p.x for p in P) / len(P), sum(p.y for p in P) / len(P)
        if any(math.hypot(mx - x, my - y) < r for x, y, r in areas):
            lib.set_zone_shape_tags(comp, [unreal.Name(tag)], True)
            tagged += 1
        else:
            lib.remove_zone_shape_tags(comp, [unreal.Name(tag)])
        lib.update_zone_shape(comp)
    return tagged


def build_crowd(gen, count=None, signal_crossings=None):
    """Lay ZoneGraph lanes along the generated level's routes, rebuild the ZoneGraph and place a crowd
    spawner. Street crossings at signalized intersections (`signal_crossings` [(x, y, radius)]; None: those
    traffic.py laid in the level, if any) get the ZoneGraph Crossing tag, so they wait for the signal clock's Walk.
    Returns a summary dict. Editor only; the caller saves the level."""
    m = H.manifest_to_dict(gen)
    routes = m.get("routes", [])
    removed = _clear()
    if not routes:
        return {"shapes": 0, "removed": removed, "note": "no routes"}
    indoor = str(m.get("sceneKind", "")).lower().startswith("indoor") or all(r.get("name", "").startswith("Room") for r in routes)
    g = _indoor_graph(routes, m.get("features", [])) if indoor else _outdoor_graph(routes, m.get("features", []))
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    kind = "narrow" if indoor else "sidewalk"
    lane_half = LANE_PROFILES[kind][2] * 0.25        # each lane's centre sits a quarter of the profile off the run
    curb = None if indoor else _curb_height(gen)
    graded = (not indoor) and _graded_ground(gen)
    ramps = _ramp_footprints(m.get("features", [])) if graded else []
    cleared = _clear_lanes(g, world, lane_half, indoor, curb, graded, ramps)
    par = list(range(len(g.nodes)))

    def _root(x):
        while par[x] != x:
            par[x] = par[par[x]]
            x = par[x]
        return x
    for a, b, _p in g.edges.values():
        par[_root(a)] = _root(b)
    cleared["components"] = len({_root(n) for n in range(len(g.nodes)) if g.degree(n) > 0})
    shapes, lane_cm = _emit(g, kind, world, "Crowd")
    areas = _signalized_areas() if signal_crossings is None else list(signal_crossings)
    signal_tagged = tag_crossings(areas) if (areas and not indoor) else 0
    unreal.RammsCrowdZoneGraphLibrary.rebuild_zone_graph()
    for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors():
        if isinstance(a, unreal.ZoneGraphData):
            a.set_folder_path(unreal.Name(FOLDER))

    # One pedestrian per ~9 m of lane outdoors, per ~8 m indoors; a packed corridor is not the point.
    if count is None:
        count = int(max(3, min(25 if indoor else 80, lane_cm / (800.0 if indoor else 900.0))))
    spawner = None
    cls = unreal.load_class(None, SPAWNER_CLASS)
    profile = unreal.load_asset(PROFILE_ASSET) if H.EAL.does_asset_exist(PROFILE_ASSET) else None
    if cls and profile and shapes:
        eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
        c = g.nodes[0] if g.nodes else (0.0, 0.0)
        spawner = eas.spawn_actor_from_class(cls, unreal.Vector(c[0], c[1], 100.0))
        spawner.set_actor_label("CrowdSpawner")
        spawner.tags = [unreal.Name(TAG)]
        spawner.set_folder_path(unreal.Name(FOLDER))
        spawner.set_editor_property("crowd_profile", profile)
        spawner.set_editor_property("spawn_mode", unreal.RammsCrowdSpawnMode.ZONE_GRAPH_LANES)
        spawner.set_editor_property("desired_count", count)
        zs = spawner.get_editor_property("zone_graph_spawn_settings")
        zs.set_editor_property("min_gap_cm", 250.0 if indoor else 400.0)
        zs.set_editor_property("max_gap_cm", 600.0 if indoor else 1200.0)
        spawner.set_editor_property("zone_graph_spawn_settings", zs)
        spawner.apply_crowd_configuration()
    elif not profile:
        H.warn("crowd: %s not found; lanes laid, no spawner" % PROFILE_ASSET)
    bad_lane, bad_poly = check_lanes(world)
    # Informational for now (verify.py lives in the superproject): lane samples whose walkers stand off their level.
    off_level = 0 if indoor else check_lane_levels(world, curb, graded=graded, ramps=ramps)
    res = {"cleared": cleared, "blocked_lane_segments": bad_lane, "blocked_junction_edges": bad_poly, "blocked": list(BLOCKED[:10]), "shapes": shapes, "nodes": len(g.nodes), "edges": len(g.edges), "lane_m": round(lane_cm / 100.0),
           "count": count if spawner else 0, "removed": removed, "indoor": indoor,
           "crosswalks": getattr(g, "crosswalks", 0), "crosswalks_unmatched": getattr(g, "crosswalks_unmatched", 0),
           "signal_crossings": signal_tagged,
           "curb_height": curb, "graded": graded, "off_level_lane_samples": off_level, "off_level": list(OFF_LEVEL[:10])}
    H.log("crowd: %s" % res)
    return res
