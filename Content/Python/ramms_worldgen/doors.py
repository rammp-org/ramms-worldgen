"""Hinged doors as URLab articulations, and closing up an indoor level (doors that open and close, a ceiling).

A door is made from a door-leaf static mesh (the palette's Door.Interior / Door.Exterior meshes): its geometry is
written as an OBJ into an MJCF model of a fixed frame and a leaf on a hinge, which URLab imports as an
AMjArticulation Blueprint like the SimReady conversions (simready.import_mjcf), so it runs on MuJoCo or Chaos as
the level's ARammsPhysicsSettings says, a robot can push it, and simready_open.py can drive it.

    import ramms_worldgen.doors as DR
    bp = DR.build_door("/Game/SuburbNeighborhoodHousePack/Meshes/Meshes_construction/SM_Door_interior", 89.0)
    DR.place_door(bp, jamb=(415.0, -5.0, 0.0), along=(0.0, 1.0), into=(1.0, 0.0), wall_thickness=12.0)

    DR.articulate_level_doors()      # in a generated level: swap each standing-open leaf for a closed, hinged door
    DR.add_ceiling()                 # in a generated indoor level: a ceiling over each room, with lights

The model (metres, MuJoCo frame): the leaf spans x 0..width from the hinge and y -thickness..0, so it turns about
its corner on the +Y face (the side it opens to) and never swings into the jamb; the joint "door" opens 0..swing
degrees toward +Y, and its lower limit is the closed door against its frame. The frame carries a strip of the
hinge-side jamb (behind the hinge, clear of the leaf), since Chaos builds the welded root from its collision. URLab mirrors Y on import, so in Unreal (actor yaw 0) the leaf runs along +X and opens toward -Y;
place_door turns and mirrors that to the opening it is given. The leaf mesh keeps its Unreal material.

The MJCF goes to <mjcf_dir>/<stem>/<stem>.xml, where simready_open.py reads joints: mjcf_dir is the argument, else
the RAMMS_SIMREADY_MJCF environment variable, else C:/Users/waemf/data/simready/mjcf. The Blueprints are made under
/Game/SimReady/MuJoCo (not committed: they hold the door meshes of uncommitted packs). An existing Blueprint is
kept, as simready.import_mjcf does.
"""
import json
import math
import os
import struct

import unreal

from . import _helpers as H
from . import simready as SR

DEFAULT_MJCF = "C:/Users/waemf/data/simready/mjcf"
DOOR_INTERIOR = "/Game/SuburbNeighborhoodHousePack/Meshes/Meshes_construction/SM_Door_interior"
DOOR_EXTERIOR = "/Game/SuburbNeighborhoodHousePack/Meshes/Meshes_construction/SM_Door_exterior_windowed"
LEAF_GAP = 0.8          # cm under the leaf
# The leaf's collision box stops 3 cm above the floor and 1 cm short of each jamb and the head: Chaos's contact
# margins would otherwise drag it on the floor and the wall.
# bumped when the model changes: an imported Blueprint is kept, and a replace-import of one asserts
MODEL_REV = 10
JAMB_GAP = 0.5          # cm between the leaf and each jamb


def _mjcf_dir(mjcf_dir=None):
    return mjcf_dir or os.environ.get("RAMMS_SIMREADY_MJCF") or DEFAULT_MJCF


def door_stem(mesh_path, width, height, swing=100.0):
    """The model's name: mesh, leaf size, swing (degrees) and MODEL_REV, all of which the MJCF depends on."""
    name = mesh_path.rsplit("/", 1)[-1].replace("SM_", "").lower()
    name = name if name.startswith("door") else "door_" + name
    return "sr_%s_w%d_h%d_s%d_r%d" % (name, int(round(width)), int(round(height)), int(round(swing)), MODEL_REV)


def _leaf_geometry(mesh, slots=None):
    """Vertices (cm, mesh space), normals, UVs and triangles of LOD 0, all sections; with a list `slots`, each
    triangle's material slot is appended to it."""
    P, N, UV, F = [], [], [], []
    sms = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
    for s in range(mesh.get_num_sections(0)):
        v, t, n, uv, _tan = unreal.ProceduralMeshLibrary.get_section_from_static_mesh(mesh, 0, s)
        if slots is not None:
            try:
                slot = sms.get_lod_material_slot(mesh, 0, s)
            except Exception:  # noqa: BLE001
                slot = s
            slots += [slot] * (len(t) // 3)
        base = len(P)
        P += [(p.x, p.y, p.z) for p in v]
        N += [(q.x, q.y, q.z) for q in n] if len(n) == len(v) else [(0.0, 0.0, 1.0)] * len(v)
        UV += [(u.x, u.y) for u in uv] if len(uv) == len(v) else [(0.0, 0.0)] * len(v)
        F += [(base + t[i], base + t[i + 1], base + t[i + 2]) for i in range(0, len(t) - 2, 3)]
    return P, N, UV, F


def _write_glb(path, P, N, UV, F, name, slots=None):
    """A minimal binary glTF of Z-up metres, which URLab imports in place of the OBJ beside it (Interchange reads
    glTF in metres; its OBJ path reads centimetres): (x, y, z) -> (x, z, -y), UVs already top-left. One primitive,
    and one material named slot_<k>, per source material slot k (`slots`: each triangle's slot), so a door of
    several materials keeps them."""
    slots = slots if slots and len(slots) == len(F) else [0] * len(F)
    keys = sorted(set(slots))
    pos = [(p[0], p[2], -p[1]) for p in P]
    nrm = [(n[0], n[2], -n[1]) for n in N]
    b_pos = struct.pack("<%df" % (3 * len(pos)), *[c for p in pos for c in p])
    b_nrm = struct.pack("<%df" % (3 * len(nrm)), *[c for n in nrm for c in n])
    b_uv = struct.pack("<%df" % (2 * len(UV)), *[c for u in UV for c in u])
    chunks = [b_pos, b_nrm, b_uv]
    acc = [{"bufferView": 0, "componentType": 5126, "count": len(pos), "type": "VEC3",
            "min": [min(p[k] for p in pos) for k in range(3)], "max": [max(p[k] for p in pos) for k in range(3)]},
           {"bufferView": 1, "componentType": 5126, "count": len(nrm), "type": "VEC3"},
           {"bufferView": 2, "componentType": 5126, "count": len(UV), "type": "VEC2"}]
    prims = []
    for m, k in enumerate(keys):
        idx = [i for f, sl in zip(F, slots) if sl == k for i in f]
        chunks.append(struct.pack("<%dI" % len(idx), *idx))
        acc.append({"bufferView": len(chunks) - 1, "componentType": 5125, "count": len(idx), "type": "SCALAR"})
        prims.append({"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": len(acc) - 1, "material": m})
    views, blob, off = [], b"", 0
    for i, c in enumerate(chunks):
        views.append({"buffer": 0, "byteOffset": off, "byteLength": len(c), "target": 34963 if i >= 3 else 34962})
        pad = (-len(c)) % 4
        blob += c + b"\0" * pad
        off += len(c) + pad
    gltf = {"asset": {"version": "2.0", "generator": "ramms_worldgen.doors"}, "scene": 0, "scenes": [{"nodes": [0]}],
            "nodes": [{"name": name, "mesh": 0}],
            "materials": [{"name": "slot_%d" % k} for k in keys],
            "meshes": [{"name": name, "primitives": prims}],
            "accessors": acc, "bufferViews": views, "buffers": [{"byteLength": len(blob)}]}
    js = json.dumps(gltf, separators=(",", ":")).encode()
    js += b" " * ((-len(js)) % 4)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(blob)))
        f.write(struct.pack("<II", len(js), 0x4E4F534A) + js)
        f.write(struct.pack("<II", len(blob), 0x004E4942) + blob)


def _classify(P, F):
    """Split a leaf mesh into its panel and the hardware proud of it. Returns a dict: wa / ta (the width and
    thickness axes, 0 = X, 1 = Y), wmin / wmax, zmin / zmax, tlo / thi (the panel's two faces), proud (vertices
    more than 1 cm off the panel), handle (the proud vertices on the latch half) and hinge_at_min. The faces are
    where the bulk of the surface area lies (the 3rd and 97th area percentiles of the triangles' thickness), so a
    plain panel has no hardware, and a handle, however many vertices it has, does not move them."""
    xs, ys, zs = [p[0] for p in P], [p[1] for p in P], [p[2] for p in P]
    # the width runs along the mesh's longer horizontal axis, the thickness along the other
    wa = 1 if (max(ys) - min(ys)) >= (max(xs) - min(xs)) else 0
    ta = 1 - wa
    tri = []
    for a, b, c in F:
        pa, pb, pc = P[a], P[b], P[c]
        ux, uy, uz = pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]
        vx, vy, vz = pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2]
        area = 0.5 * math.sqrt((uy * vz - uz * vy) ** 2 + (uz * vx - ux * vz) ** 2 + (ux * vy - uy * vx) ** 2)
        tri.append(((pa[ta] + pb[ta] + pc[ta]) / 3.0, area))
    tri.sort()
    total = sum(t[1] for t in tri) or 1.0
    acc, tlo, thi = 0.0, tri[0][0], tri[-1][0]
    for t, ar in tri:
        acc += ar
        if acc < 0.03 * total:
            tlo = t
        if acc <= 0.97 * total:
            thi = t
    tlo = min(tlo, thi)
    proud = [p for p in P if p[ta] < tlo - 1.0 or p[ta] > thi + 1.0]
    wmin, wmax = min(p[wa] for p in P), max(p[wa] for p in P)
    mid = (wmin + wmax) * 0.5
    # the handles stand proud on the latch half (hinge knuckles may too, on the other half): the hinge goes on
    # the edge away from the half with more proud vertices, and only those make the handle's collision box
    hi_side = sum(1 for p in proud if p[wa] > mid) >= sum(1 for p in proud if p[wa] <= mid)
    return {"wa": wa, "ta": ta, "wmin": wmin, "wmax": wmax, "zmin": min(zs), "zmax": max(zs), "tlo": tlo, "thi": thi,
            "proud": proud, "handle": [p for p in proud if (p[wa] > mid) == hi_side], "hinge_at_min": hi_side if proud else True}


def _write_door(mesh, width, height, swing, stem, out_dir, mass=12.0):
    """Write <out_dir>/<stem>/<stem>.xml and meshes/vis_Leaf.obj for a leaf `width` x `height` cm (the opening
    less its gaps). Returns the XML path."""
    slots = []
    P, N, UV, F = _leaf_geometry(mesh, slots)
    if not F:
        raise RuntimeError("doors: %s has no triangles at LOD 0" % mesh.get_path_name())
    k = _classify(P, F)
    wa, ta, wmin, wmax, zmin, zmax = k["wa"], k["ta"], k["wmin"], k["wmax"], k["zmin"], k["zmax"]
    tlo, thi, handle, hinge_at_min = k["tlo"], k["thi"], k["handle"], k["hinge_at_min"]
    thick = max(thi - tlo, 2.0)
    sw = width / (wmax - wmin)
    sz = height / (zmax - zmin)

    def to_mj(p):
        u = (p[wa] - wmin) if hinge_at_min else (wmax - p[wa])
        x = u * sw
        y = (p[ta] - thi)                 # the panel's +Y face on y = 0, the panel at -thick..0
        z = (p[2] - zmin) * sz + LEAF_GAP
        return (x / 100.0, y / 100.0, z / 100.0)

    Q = [to_mj(p) for p in P]

    def nrm_mj(n):
        # normals take the inverse of the (non-uniform) scale, then unit length
        u = n[wa] * (1.0 if hinge_at_min else -1.0) / sw
        v, w = n[ta], n[2] / sz
        ln = math.sqrt(u * u + v * v + w * w) or 1.0
        return (u / ln, v / ln, w / ln)
    QN = [nrm_mj(n) for n in N]
    # winding: Unreal's triangles face out under a reflection; flip them when the map to MuJoCo has none
    sx = 1.0 if hinge_at_min else -1.0
    det = -sx if wa == 1 else sx               # taking the width from Y and the thickness from X is a reflection
    flip = det > 0.0
    d = os.path.join(out_dir, stem)
    os.makedirs(os.path.join(d, "meshes"), exist_ok=True)
    with open(os.path.join(d, "meshes", "vis_Leaf.obj"), "w") as f:
        f.write("# %s, %s\n" % (stem, mesh.get_path_name()))
        for q in Q:
            f.write("v %.6f %.6f %.6f\n" % q)
        for u in UV:
            f.write("vt %.6f %.6f\n" % (u[0], 1.0 - u[1]))
        for a, b, c in F:
            if flip:
                b, c = c, b
            f.write("f %d/%d %d/%d %d/%d\n" % (a + 1, a + 1, b + 1, b + 1, c + 1, c + 1))
    _write_glb(os.path.join(d, "meshes", "vis_Leaf.glb"), Q, QN, UV, [(a, c, b) if flip else (a, b, c) for a, b, c in F], "vis_Leaf", slots)
    W, T, Hh = width / 100.0, thick / 100.0, height / 100.0
    zc = LEAF_GAP / 100.0 + Hh * 0.5
    ixx, iyy, izz = mass * (T * T + Hh * Hh) / 12.0, mass * (W * W + Hh * Hh) / 12.0, mass * (W * W + T * T) / 12.0
    hb = ""
    if handle:
        # the handle itself: the proud vertices within 15 cm of their median height, at most 6 cm off each face
        # (a porthole frame or a kick plate on the latch half is not a handle)
        zs_h = sorted(p[2] for p in handle)
        zmed = zs_h[len(zs_h) // 2]
        hq = [to_mj(p) for p in handle if abs(p[2] - zmed) <= 15.0]
        lo = [min(q[k] for q in hq) for k in range(3)]
        hi = [max(q[k] for q in hq) for k in range(3)]
        lo[1], hi[1] = max(lo[1], -T - 0.06), min(hi[1], 0.06)
        hb = ('        <geom name="col_handle" class="%s_collision" type="box" size="%.4f %.4f %.4f" pos="%.4f %.4f %.4f" friction="0.6 0.005 0.0001"/>\n'
              % (stem, max((hi[0] - lo[0]) * 0.5, 0.005), max((hi[1] - lo[1]) * 0.5, 0.005), max((hi[2] - lo[2]) * 0.5, 0.005),
                 (hi[0] + lo[0]) * 0.5, (hi[1] + lo[1]) * 0.5, (hi[2] + lo[2]) * 0.5))
    xml = '''<mujoco model="{s}">
  <!-- A hinged door from {m} by ramms_worldgen.doors: {w:.1f} x {h:.1f} cm, opens 0..{sw:.0f} degrees toward +Y -->
  <compiler angle="degree" autolimits="true"/>
  <default>
    <default class="{s}_visual">
      <geom type="mesh" contype="0" conaffinity="0" group="2" mass="0"/>
    </default>
    <default class="{s}_collision">
      <geom group="3" rgba="0.6 0.3 0.3 0.5" condim="4" solref="0.004 1"/>
    </default>
  </default>
  <asset>
    <material name="mat_Leaf" rgba="0.92 0.91 0.88 1" roughness="0.6"/>
    <material name="mat_Pin" rgba="0.55 0.55 0.53 1" metallic="1" roughness="0.4"/>
    <mesh name="vis_Leaf" file="meshes/vis_Leaf.obj"/>
  </asset>
  <worldbody>
    <body name="Frame" pos="0 0 0">
      <inertial mass="5" pos="{w2:.4f} 0 {zc:.4f}" diaginertia="0.5 0.5 0.5"/>
      <geom name="vis_Pin" class="{s}_visual" type="cylinder" size="0.006 {h2:.4f}" pos="0 0 {zc:.4f}" material="mat_Pin"/>
      <geom name="col_jamb" class="{s}_collision" type="box" size="0.015 {th:.4f} {h2:.4f}" pos="-0.02 {t2:.4f} {zc:.4f}" friction="0.6 0.005 0.0001"/>
      <body name="Leaf" pos="0 0 0">
        <joint name="door" type="hinge" pos="0 0 0" axis="0 0 1" range="0 {sw:.1f}" damping="1.5" armature="0.005" frictionloss="0"/>
        <inertial mass="{ms:.3f}" pos="{w2:.4f} {t2:.4f} {zc:.4f}" diaginertia="{ixx:.5f} {iyy:.5f} {izz:.5f}"/>
        <geom name="vis_Leaf" class="{s}_visual" mesh="vis_Leaf" material="mat_Leaf"/>
        <geom name="col_leaf" class="{s}_collision" type="box" size="{cw2:.4f} {th:.4f} {ch2:.4f}" pos="{w2:.4f} {t2:.4f} {czc:.4f}" friction="0.6 0.005 0.0001"/>
{hb}      </body>
    </body>
  </worldbody>
  <contact>
    <!-- the frame is welded to the world, so MuJoCo would not skip its contacts with the leaf -->
    <exclude body1="Frame" body2="Leaf"/>
  </contact>
</mujoco>
'''.format(s=stem, m=mesh.get_path_name(), w=width, h=height, sw=swing, w2=W * 0.5, h2=Hh * 0.5, zc=zc, th=T * 0.5, t2=-T * 0.5,
           cw2=W * 0.5 - 0.01, ch2=(Hh - 0.04) * 0.5, czc=zc + 0.01, ms=mass, ixx=ixx, iyy=iyy, izz=izz, hb=hb)
    path = os.path.join(d, stem + ".xml")
    with open(path, "w") as f:
        f.write(xml)
    return path


def build_door(mesh_path=DOOR_INTERIOR, opening_width=90.0, opening_height=210.0, swing=100.0, mjcf_dir=None, dest=SR.MJ_DEST):
    """The Blueprint of a hinged door for an opening `opening_width` x `opening_height` cm, made (MJCF, import,
    the leaf's Unreal material) if it does not exist yet. Returns the Blueprint path, or None."""
    width = opening_width - 2.0 * JAMB_GAP
    height = opening_height - LEAF_GAP - 1.0
    stem = door_stem(mesh_path, width, height, swing)
    bp = "%s/%s" % (dest, stem)
    if unreal.EditorAssetLibrary.does_asset_exist(bp):
        return bp
    mesh = unreal.load_asset(mesh_path)
    if not isinstance(mesh, unreal.StaticMesh):
        H.log("doors: %s is not a static mesh in this project" % mesh_path)
        return None
    xml = _write_door(mesh, width, height, swing, stem, _mjcf_dir(mjcf_dir))
    out = SR.import_mjcf(xml, dest).get(stem)
    if not out:
        return None
    # the leaf keeps the source mesh's materials: each imported slot is named slot_<k> after the source slot k
    vis = [p for p in unreal.EditorAssetLibrary.list_assets("/Game/MuJoCoImports/%s_Assets" % stem, True, False) if p.split(".")[0].endswith("/vis_Leaf")]
    sm = unreal.load_asset(vis[0].split(".")[0]) if vis else None
    if sm is not None:
        src = mesh.static_materials
        for i, m in enumerate(sm.static_materials):
            name = str(m.material_slot_name)
            k = int(name.rsplit("_", 1)[-1]) if name.startswith("slot_") and name.rsplit("_", 1)[-1].isdigit() else i
            if k < len(src):
                sm.set_material(i, src[k].material_interface)
        unreal.EditorAssetLibrary.save_loaded_asset(sm, only_if_is_dirty=False)
    return out


def place_door(bp, jamb, along, into, wall_thickness=12.0, folder=None, label=None, tag=None, seg=("seg:Door",)):
    """Spawn the door Blueprint `bp` closed in an opening. `jamb` is the opening's edge (x, y, floor z) the hinge
    goes on, `along` the unit direction across the opening from it, `into` the unit direction (perpendicular,
    horizontal) of the room the door opens into. Returns the actor."""
    ux, uy = along
    vx, vy = into
    # the leaf runs along the actor's +X and opens toward its -Y: with `into` on the left of `along`, hinge at
    # the other jamb and run the other way
    if ux * vy - uy * vx > 0.0:
        w = _bp_width(bp)
        jamb = (jamb[0] + ux * (w + 2.0 * JAMB_GAP), jamb[1] + uy * (w + 2.0 * JAMB_GAP), jamb[2])
        ux, uy = -ux, -uy
    hx = jamb[0] + ux * JAMB_GAP + vx * wall_thickness * 0.5
    hy = jamb[1] + uy * JAMB_GAP + vy * wall_thickness * 0.5
    yaw = math.degrees(math.atan2(uy, ux))
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    a = eas.spawn_actor_from_class(unreal.EditorAssetLibrary.load_blueprint_class(bp), unreal.Vector(hx, hy, jamb[2]), unreal.Rotator(yaw=yaw))
    if folder:
        a.set_folder_path(folder)
    if label:
        a.set_actor_label(label)
    if tag:
        a.tags = list(a.tags) + [unreal.Name(tag)]
    # its segmentation class, on every primitive component as the labeller tags them
    for c in a.get_components_by_class(unreal.PrimitiveComponent):
        c.component_tags = [t for t in c.component_tags if not str(t).startswith("seg:")] + [unreal.Name(t) for t in seg]
    return a


def _bp_width(bp):
    """The leaf width (cm) a door Blueprint was made for, from its name (sr_<mesh>_w<width>_...)."""
    stem = bp.rsplit("/", 1)[-1]
    for part in stem.split("_"):
        if part.startswith("w") and part[1:].isdigit():
            return float(part[1:])
    return 89.0


def _into_level(actors, gen):
    """Move actors spawned into the editor's current level into the generator's level (a loaded sublevel need not
    be the current one), so they save, unload and are found with it. Returns the actors where they now are."""
    level = gen.get_outer()
    stray = [a for a in actors if a is not None and a.get_outer() != level]
    if stray:
        unreal.EditorLevelUtils.move_actors_to_level(stray, level, False, False)
    return stray


def _tagged(tag):
    """Every actor carrying `tag`, in any loaded level."""
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    return [a for a in eas.get_all_level_actors() if tag in [str(t) for t in a.tags]]


def _leaf_frame(mesh):
    """(width axis 0/1, hinge at the low end?, local bounds min, max) of a door-leaf mesh, classified as _write_door
    does."""
    P, _N, _UV, F = _leaf_geometry(mesh)
    k = _classify(P, F)
    lo = [min(p[i] for p in P) for i in range(3)]
    hi = [max(p[i] for p in P) for i in range(3)]
    return k["wa"], k["hinge_at_min"], lo, hi


def _generator(generator=None):
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    if generator is not None:
        return generator
    gens = [a for a in eas.get_all_level_actors() if isinstance(a, unreal.RammsWorldGenerator)]
    return gens[0] if gens else None


def _params(gen):
    """The generator's resolved parameters (preset, profile and its own overrides), else its preset's."""
    try:
        return gen.get_editor_property("params")
    except Exception:  # noqa: BLE001
        return gen.get_editor_property("preset").get_editor_property("params")


def _owner_tag(prefix, gen):
    """prefix:<the generator's id>: unique across loaded levels, as ARammsWorldGenerator::ArticulatedTag()."""
    try:
        gid = unreal.GuidLibrary.conv_guid_to_string(gen.get_editor_property("generator_id"))
    except Exception:  # noqa: BLE001
        gid = gen.get_name()
    return "%s:%s" % (prefix, gid)


def _owned(tag, gen):
    """The actors in the generator's level carrying `tag`."""
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    return [a for a in eas.get_all_level_actors() if a.get_outer() == gen.get_outer() and tag in [str(t) for t in a.tags]]


def _generator_isms(gen):
    """The instanced meshes of the generator's output: its own, and those of PCG partition actors that hold only
    this generator's local component (a partition actor that also holds output of any other PCG component in the
    level, generator or not, is skipped)."""
    out = list(gen.get_components_by_class(unreal.InstancedStaticMeshComponent))
    pcg = gen.get_component_by_class(unreal.PCGComponent)
    if pcg is None or not hasattr(unreal, "PCGPartitionActor"):
        return out
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actors = eas.get_all_level_actors()
    parts = [a for a in actors if isinstance(a, unreal.PCGPartitionActor)]
    owners = [c for a in actors if not isinstance(a, unreal.PCGPartitionActor)
              for c in a.get_components_by_class(unreal.PCGComponent)]
    for pa in parts:
        try:
            locals_ = [c for c in owners if pa.get_local_component(c)]
        except Exception:  # noqa: BLE001
            continue
        if locals_ == [pcg]:
            out += list(pa.get_components_by_class(unreal.InstancedStaticMeshComponent))
        elif pcg in locals_:
            H.log("doors: %s also holds other PCG components' output; its door leaves are left as they are" % pa.get_name())
    return out


def _palette_handles(palette_path):
    """{door-leaf mesh path: (rotation offset yaw, HandleSide)} from the palette the level was generated with."""
    pal = unreal.load_asset(str(palette_path).split(".")[0]) if palette_path else None
    out = {}
    if pal is None:
        return out
    for _slot, s in pal.get_editor_property("slots").items():
        for e in s.get_editor_property("meshes"):
            m = e.get_editor_property("mesh")
            path = m.get_path_name() if hasattr(m, "get_path_name") else str(m)
            hs = e.get_editor_property("handle_side")
            if path and hs:
                out[path.split(".")[0]] = (e.get_editor_property("rotation_offset").yaw, hs)
    return out


def articulate_level_doors(generator=None, swing=100.0, mjcf_dir=None, wall_thickness=None, folder="Doors"):
    """In a generated indoor level: replace each door leaf the generator stood open beside its opening (an
    instance of a Door.Interior / Door.Exterior palette mesh) with a closed, hinged door of the same mesh in the
    opening, opening into the room the leaf stood in. The opening's width is the leaf's plus 1 cm (PCGRammsIndoor
    fits the leaf to it), its height 210 cm above the room's floor less any raised threshold the leaf stands on;
    which side of the leaf the opening is on comes from a trace across the wall. The leaf instances are removed.

    Doors are tagged RammsDoors:<generator id> and RammsDoorsSeed:<generator id>:<seed>. A run replaces the doors at the
    openings it converts and those of another seed (a Regenerate with a new seed), and keeps the rest, so a run that
    skipped an opening can be repeated without losing the doors it made. Returns the door actors placed."""
    gen = _generator(generator)
    if gen is None:
        H.log("doors: no RammsWorldGenerator in the level")
        return []
    params = _params(gen)
    # PCGRammsIndoor clamps the wall thickness to 5..60 cm
    T = wall_thickness or min(max(params.get_editor_property("wall_thickness"), 5.0), 60.0)
    wall_h = params.get_editor_property("wall_height")
    manifest = H.manifest_to_dict(gen)
    door_assets = {str(f.get("asset", "")).split(".")[0] for f in manifest.get("features", []) if str(f.get("kind")) in ("Door", "Entrance")}
    rooms = [f for f in manifest.get("features", []) if str(f.get("kind")) == "Room"]
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    tag = _owner_tag("RammsDoors", gen)
    seed_tag = "%s:%d" % (_owner_tag("RammsDoorsSeed", gen), gen.get_editor_property("seed"))
    mine = _owned(tag, gen)
    for a in [a for a in mine if seed_tag not in [str(t) for t in a.tags]]:
        eas.destroy_actor(a)          # from another seed's layout
        mine.remove(a)
    isms = [c for c in _generator_isms(gen)
            if c.static_mesh and c.static_mesh.get_path_name().split(".")[0] in door_assets and c.get_instance_count() > 0]
    handle_side = _palette_handles(manifest.get("palette"))
    if not isms:
        H.log("doors: no standing door leaves (already articulated, or no doors); kept the doors there are")
        return mine

    def floor_at(x, y):
        inside = [r for r in rooms if abs(x - r["location"]["x"]) <= r["size"]["x"] * 0.5 + T and abs(y - r["location"]["y"]) <= r["size"]["y"] * 0.5 + T]
        return min(r["location"]["z"] for r in (inside or rooms)) if rooms else 0.0

    placed = []
    for ism in isms:
        mesh = ism.static_mesh
        path = mesh.get_path_name().split(".")[0]
        wa, hinge_at_min, lo, hi = _leaf_frame(mesh)
        ta = 1 - wa
        pal_leaf = handle_side.get(path)
        # the leaf's segmentation class goes with it (the labeller only tags generator-owned output)
        seg = [str(t) for t in ism.component_tags if str(t).startswith("seg:")] or ["seg:Door"]
        remove = []
        for i in range(ism.get_instance_count()):
            xf = ism.get_instance_transform(i, True)
            xf = xf[-1] if isinstance(xf, tuple) else xf

            def to_world(pt):
                return xf.transform_location(unreal.Vector(*pt))
            a = [0.0, 0.0, lo[2]]
            b = [0.0, 0.0, lo[2]]
            # PCGRammsIndoor centres the leaf's bounds (handles included) on its line beside the jamb
            a[ta] = b[ta] = (lo[ta] + hi[ta]) * 0.5
            a[wa], b[wa] = (lo[wa], hi[wa]) if hinge_at_min else (hi[wa], lo[wa])
            ph, pf = to_world(a), to_world(b)
            if pal_leaf:
                # the palette says which way the generator turned the leaf: its facing frame's +Y points away from
                # the wall, or toward it when HandleSide < 0; the hinge end is the one nearer the wall
                yaw = math.radians(xf.rotation.rotator().yaw - pal_leaf[0])
                fy = (-math.sin(yaw), math.cos(yaw))
                sign = 1.0 if pal_leaf[1] >= 0.0 else -1.0
                if (pf.x - ph.x) * fy[0] * sign + (pf.y - ph.y) * fy[1] * sign < 0.0:
                    ph, pf = pf, ph
            vx, vy = pf.x - ph.x, pf.y - ph.y
            leaf_w = math.hypot(vx, vy)
            if leaf_w < 40.0:
                continue
            vx, vy = vx / leaf_w, vy / leaf_w
            ux, uy = -vy, vx
            # the wall's centre line, T/2 + 2.5 cm behind the leaf's hinge end (PCGRammsIndoor stands it there)
            cx, cy = ph.x - vx * (T * 0.5 + 2.5), ph.y - vy * (T * 0.5 + 2.5)
            # the leaf is the opening less 1 cm (the manifest's measure is a raised threshold's rise for those)
            opening = leaf_w + 1.0
            # the opening is on the side of the leaf where a trace across the wall meets nothing
            z = ph.z + 100.0
            clear = []
            for sgn in (1.0, -1.0):
                qx, qy = cx + sgn * ux * (3.0 + opening * 0.5), cy + sgn * uy * (3.0 + opening * 0.5)
                # (doors an earlier run left in the openings would block the trace: they are ignored)
                r = unreal.SystemLibrary.line_trace_single(world, unreal.Vector(qx - vx * T, qy - vy * T, z), unreal.Vector(qx + vx * T, qy + vy * T, z),
                                                           unreal.TraceTypeQuery.TRACE_TYPE_QUERY1, True, mine + placed, unreal.DrawDebugTrace.NONE, True)
                clear.append(not (r[0] if isinstance(r, tuple) else r is not None))
            if clear[0] == clear[1]:
                H.log("doors: cannot tell which side of the leaf at (%.0f, %.0f) the opening is on; left as it is" % (ph.x, ph.y))
                continue
            sgn = 1.0 if clear[0] else -1.0
            ux, uy = ux * sgn, uy * sgn
            jamb = (cx + ux * 3.0, cy + uy * 3.0, ph.z)
            # the head is 210 cm above the floor (ARammsProcPiece clamps it to the wall height); a raised threshold
            # lifts the leaf, not the head
            rise = max(ph.z - floor_at(cx, cy), 0.0)
            head = min(210.0, wall_h)
            # widths round down to 2 cm (and 1 cm more of slack, for the leaf's pose read back from its instance)
            # so a few Blueprints serve
            bp = build_door(path, math.floor((opening - 1.0) / 2.0) * 2.0, head - rise, swing, mjcf_dir)
            if not bp:
                continue
            # the door an earlier run put in this opening, if any, is replaced
            ox, oy = cx + ux * (3.0 + opening * 0.5), cy + uy * (3.0 + opening * 0.5)
            for old in [d for d in mine if math.hypot(d.get_actor_location().x - ox, d.get_actor_location().y - oy) < opening * 0.5 + 20.0]:
                eas.destroy_actor(old)
                mine.remove(old)
            d = place_door(bp, jamb, (ux, uy), (vx, vy), T, folder=folder, tag=tag, seg=seg)
            d.tags = list(d.tags) + [unreal.Name(seed_tag)]
            placed.append(d)
            remove.append(i)
        for i in sorted(remove, reverse=True):
            ism.remove_instance(i)
    if _into_level([a for a in _tagged(seed_tag)], gen):
        placed = [a for a in _owned(seed_tag, gen) if a not in mine]
    H.log("doors: %d articulated doors (%d kept from an earlier run)" % (len(placed), len(mine)))
    return placed + mine


def add_ceiling(generator=None, material="Mat.Wall.Interior", palette=None, lights=True, spacing=350.0, folder="Ceiling"):
    """In a generated indoor level: a ceiling slab over each room of the manifest, its underside at the top of the
    walls, with ceiling lights (one per room, more every `spacing` cm in large rooms). Returns the slabs."""
    gen = _generator(generator)
    if gen is None:
        H.log("doors: no RammsWorldGenerator in the level, no ceiling")
        return []
    pal = unreal.load_asset(palette or "/RammsWorldGen/Palettes/PAL_Default")
    mat = pal.material_path(material) if pal else ""
    tag = _owner_tag("RammsCeiling", gen)
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    for a in _owned(tag, gen):
        eas.destroy_actor(a)
    slabs = []
    for f in H.manifest_to_dict(gen).get("features", []):
        if str(f.get("kind")) != "Room":
            continue
        c, sz = f["location"], f["size"]
        slabs.append(ceiling_slab(c["x"] - sz["x"] * 0.5, c["x"] + sz["x"] * 0.5, c["y"] - sz["y"] * 0.5, c["y"] + sz["y"] * 0.5,
                                  c["z"] + sz["z"], mat, lights=lights, spacing=spacing, folder=folder, tag=tag))
    if _into_level(_tagged(tag), gen):
        slabs = [a for a in _owned(tag, gen) if isinstance(a, unreal.RammsProcPiece)]
    H.log("doors: ceiling over %d rooms" % len(slabs))
    return slabs


def ceiling_slab(x0, x1, y0, y1, z, material, thickness=12.0, lights=True, spacing=350.0, folder="Ceiling", lamp=None, candela=60.0, tag=None):
    """A ceiling slab whose underside is at z over x0..x1, y0..y1, and ceiling lights under it. Returns the slab."""
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    s = eas.spawn_actor_from_class(unreal.RammsProcPiece, unreal.Vector(x0, (y0 + y1) * 0.5, z + thickness), unreal.Rotator())
    for k, v in (("kind", unreal.RammsProcPieceKind.SLAB), ("size_x", x1 - x0), ("size_y", y1 - y0), ("size_z", thickness),
                 ("material_path", material), ("uv_scale", 200.0)):
        s.set_editor_property(k, v)
    s.rebuild()
    s.set_folder_path(folder)
    if tag:
        s.tags = [unreal.Name(tag)]
    # its segmentation class (the labeller only classifies generator output)
    for c in s.get_components_by_class(unreal.PrimitiveComponent):
        c.component_tags = [t for t in c.component_tags if not str(t).startswith("seg:")] + [unreal.Name("seg:Ceiling")]
    if lights:
        lamp = lamp or "/Game/HousePropsFurniture/Meshes/Meshes_props/Lamps/SM_Lamp_ceiling_A"
        lm = unreal.load_asset(lamp) if unreal.EditorAssetLibrary.does_asset_exist(lamp) else None
        nx, ny = max(1, int(round((x1 - x0) / spacing))), max(1, int(round((y1 - y0) / spacing)))
        for i in range(nx):
            for j in range(ny):
                x, y = x0 + (i + 0.5) * (x1 - x0) / nx, y0 + (j + 0.5) * (y1 - y0) / ny
                for a in ceiling_light(x, y, z, lm, candela, folder):
                    if tag:
                        a.tags = [unreal.Name(tag)]
    return s


def ceiling_light(x, y, z, lamp_mesh=None, candela=60.0, folder="Ceiling", radius=700.0):
    """A ceiling lamp (static mesh, hung from z) with a warm, shadowed point light under it. Returns the actors."""
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    out = []
    if lamp_mesh is not None:
        m = eas.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector(x, y, z), unreal.Rotator())
        m.static_mesh_component.set_static_mesh(lamp_mesh)
        m.set_folder_path(folder)
        out.append(m)
    l = eas.spawn_actor_from_class(unreal.PointLight, unreal.Vector(x, y, z - 30.0), unreal.Rotator())
    c = l.get_component_by_class(unreal.PointLightComponent)
    c.set_editor_property("intensity_units", unreal.LightUnits.CANDELAS)
    c.set_intensity(candela)
    c.set_attenuation_radius(radius)
    c.set_cast_shadows(True)
    c.set_light_color(unreal.LinearColor(1.0, 0.9, 0.78, 1.0))
    l.set_folder_path(folder)
    return out + [l]
