"""Convert an articulated SimReady USD package into an MJCF model for MuJoCo and URLab.

SimReady packages carry UsdPhysics rigid bodies, colliders, mass properties and
revolute/prismatic joints (a fridge door, a microwave door, drawers). MuJoCo reads
USD only from a source build with USD support (pip wheels have none), and URLab
imports MJCF, so this writes the articulation out as MJCF:

  <out>/<name>.xml          the model: one body per rigid body, nested by joints,
                            with "closed" and "open" keyframes
  <out>/meshes/*.obj        visual meshes (one per body and material, with UVs and
                            normals) and collider hulls, in their body's frame
  <out>/meshes/*.glb        the same meshes as glTF, which URLab imports instead of
                            the OBJ (its OBJ path reads metres as centimetres)
  <out>/textures/*.png      base-colour textures, clamped to --max-tex
  <out>/conversion.json     bodies, joints (range, authored value), warnings

  pip install usd-core numpy pillow scipy   (scipy: collider hulls; mujoco: a load check)
  python Tools/simready_mjcf.py <cache>/ForgeCAD__simready-assets/refrigerator/simready_usd/refrigerator.usda \
      --out D:/simready/mjcf/sr_fc_refrigerator

The model is named sr_<nv|fc|lg>_<asset> unless --name says otherwise; URLab names the
Blueprint and its /Game/MuJoCoImports/<name>_Assets folder after it.

Mapping:
- A prim with PhysicsRigidBodyAPI becomes a body. A joint whose body0 or body1
  names a prim under a body (NVIDIA's joints name the mesh) is resolved to that
  body. The body welded to the world, else the articulation root, else the
  heaviest body no joint moves, is the root, welded to the world unless --free.
- Revolute joints become hinges, prismatic ones slides, spherical ones balls;
  fixed joints nest the child without a joint; other joint types are welded,
  with a warning. A body no joint reaches is welded to the root.
- The joint value of the authored pose becomes the MJCF joint's ref, so an
  asset authored with a door ajar keeps its pose and its USD limits.
- Drives with damping become joint damping (PhysX angular drives are per degree),
  drives with stiffness a spring (stiffness, springref); joints without a drive
  get --hinge-damping / --slide-damping.
- PhysicsMassAPI mass, centre of mass, diagonal inertia and principal axes
  become <inertial>; mass on the colliders (NVIDIA) is summed per body.
- Collider Cube/Sphere/Capsule/Cylinder become MJCF primitives; collider meshes
  become mesh geoms written as their convex hull, which is what MuJoCo collides
  with. Colliders keep the friction of their physics material. A concave mesh
  collider (approximation none, sdf or meshSimplification: NVIDIA's and LG's
  whole-part colliders) is only hulled unless --decompose splits it with CoACD.
- Renderable meshes (not guide or proxy, visible) become visual geoms, one per
  body and material, coloured from the UsdPreviewSurface (texture, colour,
  opacity, metallic, roughness).
- Default classes are namespaced (<name>_visual, ...) so the model composes with
  others (MjSpec.attach, URLab).
"""
import argparse
import json
import math
import os
import re
import shutil
import sys

import numpy as np
from pxr import Gf, Usd, UsdGeom, UsdPhysics, UsdShade

INF_LIMIT = 1e5            # |limit| at or above this is treated as unlimited
VARIANT_DEFAULTS = {"physics": "physics", "animation": "static"}


def _name(s):
    s = re.sub(r"[^A-Za-z0-9_]+", "_", s).strip("_")
    return s or "x"


def _fmt(v, nd=6):
    out = []
    for x in np.ravel(v):
        x = float(x)
        if abs(x) < 10 ** -nd:
            x = 0.0
        out.append(("%.*f" % (nd, x)).rstrip("0").rstrip(".") if "." in ("%.*f" % (nd, x)) else "%d" % x)
    return " ".join(o if o not in ("-0", "") else "0" for o in out)


def _np(m):
    return np.array([[m[i][j] for j in range(4)] for i in range(4)], dtype=np.float64)


def _decompose(M):
    """Row-vector 4x4 -> (translation, 3x3 rotation (row-vector), scale)."""
    A = M[:3, :3]
    sc = np.linalg.norm(A, axis=1)
    sc[sc == 0] = 1.0
    R = A / sc[:, None]
    # Re-orthonormalise (tiny shears from float authoring).
    u, _, vt = np.linalg.svd(R)
    R = u @ vt
    if np.linalg.det(R) < 0:
        R[2] *= -1
        sc[2] *= -1
    return M[3, :3].copy(), R, sc


def _rigid(M):
    t, R, _ = _decompose(M)
    out = np.eye(4)
    out[:3, :3] = R
    out[3, :3] = t
    return out


def _quat_wxyz(R):
    """Rotation (row-vector convention, p' = p R) -> MuJoCo quaternion w x y z."""
    m = R.T  # column-vector rotation
    q = Gf.Matrix3d(*m.ravel().tolist()).ExtractRotation().GetQuat()
    w, (x, y, z) = q.GetReal(), q.GetImaginary()
    return np.array([w, x, y, z])


def _rot_from_quat(q):
    """Gf.Quatf/Quatd -> row-vector 3x3."""
    m = Gf.Matrix4d().SetRotate(Gf.Rotation(Gf.Quatd(q.GetReal(), Gf.Vec3d(q.GetImaginary()))))
    return _np(m)[:3, :3]


def _axis_vec(a):
    return {"X": np.array([1.0, 0, 0]), "Y": np.array([0, 1.0, 0]), "Z": np.array([0, 0, 1.0])}[a or "X"]


def _purpose_ok(p):
    return UsdGeom.Imageable(p).ComputePurpose() in (UsdGeom.Tokens.default_, UsdGeom.Tokens.render)


def _visible(p):
    return UsdGeom.Imageable(p).ComputeVisibility() != UsdGeom.Tokens.invisible


def write_glb(path, P, N, T, F, name="mesh"):
    """Minimal glTF 2.0 binary: one mesh, one primitive. Z-up metres -> glTF Y-up
    metres ((x, y, z) -> (x, z, -y), as the Gen3 model's GLBs are written); UVs
    flipped to glTF's top-left origin."""
    import struct
    conv = lambda a: np.column_stack([a[:, 0], a[:, 2], -a[:, 1]]).astype(np.float32)
    pos, nrm = conv(P), conv(N)
    chunks = [pos.tobytes(), nrm.tobytes()]
    acc = [
        {"bufferView": 0, "componentType": 5126, "count": len(pos), "type": "VEC3",
         "min": pos.min(axis=0).tolist(), "max": pos.max(axis=0).tolist()},
        {"bufferView": 1, "componentType": 5126, "count": len(nrm), "type": "VEC3"},
    ]
    attrs = {"POSITION": 0, "NORMAL": 1}
    if T is not None:
        uv = np.column_stack([T[:, 0], 1.0 - T[:, 1]]).astype(np.float32)
        chunks.append(uv.tobytes())
        acc.append({"bufferView": 2, "componentType": 5126, "count": len(uv), "type": "VEC2"})
        attrs["TEXCOORD_0"] = 2
    idx = F.astype(np.uint32).ravel()
    chunks.append(idx.tobytes())
    acc.append({"bufferView": len(chunks) - 1, "componentType": 5125, "count": len(idx), "type": "SCALAR"})
    views, blob, off = [], b"", 0
    for i, c in enumerate(chunks):
        views.append({"buffer": 0, "byteOffset": off, "byteLength": len(c),
                      "target": 34963 if i == len(chunks) - 1 else 34962})
        pad = (-len(c)) % 4
        blob += c + b"\0" * pad
        off += len(c) + pad
    gltf = {"asset": {"version": "2.0", "generator": "simready_mjcf.py"}, "scene": 0,
            "scenes": [{"nodes": [0]}], "nodes": [{"name": name, "mesh": 0}],
            "meshes": [{"name": name, "primitives": [{"attributes": attrs, "indices": len(acc) - 1}]}],
            "accessors": acc, "bufferViews": views, "buffers": [{"byteLength": len(blob)}]}
    js = json.dumps(gltf, separators=(",", ":")).encode()
    js += b" " * ((-len(js)) % 4)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(blob)))
        f.write(struct.pack("<II", len(js), 0x4E4F534A) + js)
        f.write(struct.pack("<II", len(blob), 0x004E4942) + blob)


class Converter:
    def __init__(self, path, out, name, prefix="", free=False, hinge_damping=0.5, slide_damping=5.0,
                 armature=0.005, frictionloss=0.0, max_tex=2048, decompose=False, variants=None, verbose=True,
                 glb=True):
        self.path, self.out, self.name, self.prefix = path, out, name, prefix
        self.free, self.hinge_damping, self.slide_damping = free, hinge_damping, slide_damping
        self.armature, self.frictionloss, self.max_tex, self.decompose = armature, frictionloss, max_tex, decompose
        self.verbose = verbose
        self.glb = glb
        self.warn = []
        self.stage = Usd.Stage.Open(path, load=Usd.Stage.LoadAll)
        dp = self.stage.GetDefaultPrim() or self.stage.GetPseudoRoot()
        vs = dp.GetVariantSets() if dp else None
        chosen = dict(VARIANT_DEFAULTS)
        chosen.update(variants or {})
        if vs:
            for n in vs.GetNames():
                v = vs.GetVariantSet(n)
                if n in chosen and chosen[n] in v.GetVariantNames():
                    v.SetVariantSelection(chosen[n])
        self.variants = {n: vs.GetVariantSet(n).GetVariantSelection() for n in vs.GetNames()} if vs else {}
        mpu = UsdGeom.GetStageMetersPerUnit(self.stage) or 1.0
        C = np.eye(4) * 1.0
        if UsdGeom.GetStageUpAxis(self.stage) == UsdGeom.Tokens.y:
            C[:3, :3] = np.array([[1, 0, 0], [0, 0, 1], [0, -1, 0]], dtype=float)  # Y-up -> Z-up
        C[:3, :3] *= mpu
        self.C = C
        self.kgpu = UsdPhysics.GetStageKilogramsPerUnit(self.stage) or 1.0
        self.mpu = mpu
        self.xc = UsdGeom.XformCache(Usd.TimeCode.Default())
        self.pred = Usd.TraverseInstanceProxies(Usd.PrimAllPrimsPredicate)
        self.mesh_dir = os.path.join(out, "meshes")
        self.tex_dir = os.path.join(out, "textures")
        self.assets = []       # xml lines in <asset>
        self.materials = {}    # key -> material name
        self.textures = {}     # file -> texture name
        self.mesh_names = set()
        self.cls = prefix or (_name(name) + "_")   # default-class namespace

    # -- helpers -----------------------------------------------------------------
    def log(self, *a):
        if self.verbose:
            print(*a)

    def W(self, prim):
        """World matrix (row-vector), Z-up metres."""
        return _np(self.xc.GetLocalToWorldTransform(prim)) @ self.C

    def body_of(self, prim):
        p = prim
        while p and p.IsValid() and not p.IsPseudoRoot():
            if p.HasAPI(UsdPhysics.RigidBodyAPI) and p.GetAttribute("physics:rigidBodyEnabled").Get() is not False:
                return p
            p = p.GetParent()
        return None

    def uname(self, base, used):
        n = _name(base)
        k, i = n, 1
        while k in used:
            i += 1
            k = "%s_%d" % (n, i)
        used.add(k)
        return k

    # -- discovery -----------------------------------------------------------------
    def collect(self):
        prims = list(self.stage.Traverse(self.pred))
        self.bodies = [p for p in prims if p.HasAPI(UsdPhysics.RigidBodyAPI)
                       and p.GetAttribute("physics:rigidBodyEnabled").Get() is not False]
        if not self.bodies:
            raise SystemExit("no rigid bodies in %s" % self.path)
        self.joints = [p for p in prims if p.IsA(UsdPhysics.Joint)
                       and p.GetAttribute("physics:jointEnabled").Get() is not False]
        self.colliders = [p for p in prims if p.HasAPI(UsdPhysics.CollisionAPI)
                          and p.GetAttribute("physics:collisionEnabled").Get() is not False
                          and p.IsA(UsdGeom.Gprim)]
        coll = set(p.GetPath() for p in self.colliders)
        self.visuals = []
        for p in prims:
            if not p.IsA(UsdGeom.Mesh) or not _visible(p) or not _purpose_ok(p):
                continue
            nm = p.GetName().lower()
            if p.GetPath() not in coll and re.search(r"collider|collision|_coll\b|^ucx_|^ubx_", nm):
                continue
            self.visuals.append(p)
        self.log("bodies %d joints %d colliders %d visual meshes %d variants %s" % (
            len(self.bodies), len(self.joints), len(self.colliders), len(self.visuals), self.variants))

    def tree(self):
        """Orient joints into a tree from the root body."""
        edges = []
        world_links = []
        for j in self.joints:
            J = UsdPhysics.Joint(j)
            t0 = J.GetBody0Rel().GetTargets()
            t1 = J.GetBody1Rel().GetTargets()
            b0 = self.body_of(self.stage.GetPrimAtPath(t0[0])) if t0 else None
            b1 = self.body_of(self.stage.GetPrimAtPath(t1[0])) if t1 else None
            if b0 is None and b1 is None:
                continue
            if b0 is not None and b1 is not None and b0.GetPath() == b1.GetPath():
                continue
            if b0 is None or b1 is None:
                world_links.append((j, b1 if b0 is None else b0))
                continue
            edges.append((j, b0, b1))
        self.world_joints = world_links
        # Root: welded to the world, else the articulation root, else the heaviest
        # body that is never a joint's body1.
        root = None
        for j, b in world_links:
            if j.IsA(UsdPhysics.FixedJoint):
                root = b
                break
        if root is None:
            for b in self.bodies:
                if b.HasAPI(UsdPhysics.ArticulationRootAPI):
                    root = b
                    break
        if root is None:
            children = set(b1.GetPath() for _, _, b1 in edges)
            cands = [b for b in self.bodies if b.GetPath() not in children] or self.bodies
            root = max(cands, key=lambda b: self.body_mass(b)[0] or 0.0)
        self.root = root
        adj = {}
        for j, b0, b1 in edges:
            adj.setdefault(b0.GetPath(), []).append((j, b0, b1, False))
            adj.setdefault(b1.GetPath(), []).append((j, b1, b0, True))  # reversed
        self.parent = {root.GetPath(): None}
        self.children = {}
        order = [root]
        i = 0
        while i < len(order):
            b = order[i]
            i += 1
            for j, a, c, rev in adj.get(b.GetPath(), []):
                if c.GetPath() in self.parent:
                    continue
                self.parent[c.GetPath()] = (b, j, rev)
                self.children.setdefault(b.GetPath(), []).append(c)
                order.append(c)
        for b in self.bodies:
            if b.GetPath() not in self.parent:
                self.warn.append("body %s is not reached by a joint; welded to %s" % (b.GetPath(), root.GetPath()))
                self.parent[b.GetPath()] = (root, None, False)
                self.children.setdefault(root.GetPath(), []).append(b)
        used = sum(1 for v in self.parent.values() if v and v[1] is not None)
        if used < len(edges):
            self.warn.append("%d joint(s) close loops and are ignored" % (len(edges) - used))

    # -- mass ----------------------------------------------------------------------
    def body_mass(self, body):
        """(mass, com (body frame), diag inertia, principal R) or (None,...)."""
        def read(p):
            if not p.HasAPI(UsdPhysics.MassAPI):
                return None
            m = p.GetAttribute("physics:mass").Get()
            if not m or m <= 0:
                return None
            com = p.GetAttribute("physics:centerOfMass").Get()
            I = p.GetAttribute("physics:diagonalInertia").Get()
            pa = p.GetAttribute("physics:principalAxes").Get()
            com = np.array(com, dtype=float) if com is not None and not any(math.isinf(c) for c in com) else None
            I = np.array(I, dtype=float) if I is not None and sum(I) > 0 else None
            R = _rot_from_quat(pa) if pa is not None else np.eye(3)
            return m * self.kgpu, com, I, R
        r = read(body)
        if r:
            m, com, I, R = r
            Wb = self.W(body)
            _, _, sc = _decompose(Wb)
            if com is not None:
                com = com * sc
            if I is not None:
                I = I * (self.kgpu * self.mpu * self.mpu) / self.kgpu
            return m, com, I, R
        # Sum the colliders' masses (NVIDIA puts MassAPI on the collision meshes).
        parts = []
        for c in self.colliders:
            if self.body_of(c) is None or self.body_of(c).GetPath() != body.GetPath():
                continue
            rc = read(c)
            if not rc:
                continue
            m, com, I, R = rc
            T = self.W(c) @ np.linalg.inv(_rigid(self.W(body)))
            t, Rc, sc = _decompose(T)
            p = (com * sc if com is not None else np.zeros(3)) @ Rc + t
            parts.append((m, p, I, R @ Rc if I is not None else None))
        if not parts:
            return None, None, None, None
        M = sum(p[0] for p in parts)
        com = sum(p[0] * p[1] for p in parts) / M
        Itot = np.zeros((3, 3))
        have_I = True
        for m, p, I, R in parts:
            if I is None:
                have_I = False
                break
            Ic = R.T @ np.diag(I) @ R
            d = p - com
            Itot += Ic + m * (np.dot(d, d) * np.eye(3) - np.outer(d, d))
        if not have_I:
            return M, com, None, np.eye(3)
        w, V = np.linalg.eigh(Itot)
        if np.linalg.det(V) < 0:
            V[:, 2] *= -1
        return M, com, np.maximum(w, 1e-9), V.T

    # -- materials -----------------------------------------------------------------
    def material(self, prim):
        mat, _ = UsdShade.MaterialBindingAPI(prim).ComputeBoundMaterial()
        if not mat:
            dc = prim.GetAttribute("primvars:displayColor").Get() if prim.GetAttribute("primvars:displayColor") else None
            rgb = tuple(dc[0]) if dc else (0.7, 0.7, 0.7)
            key = "dc_%.3f_%.3f_%.3f" % rgb
            if key not in self.materials:
                n = self.uname(self.prefix + "mat_" + key.replace(".", ""), set(self.materials.values()))
                self.materials[key] = n
                self.assets.append('<material name="%s" rgba="%s 1"/>' % (n, _fmt(rgb, 4)))
            return self.materials[key]
        key = str(mat.GetPath())
        if key in self.materials:
            return self.materials[key]
        surf = None
        out = mat.GetSurfaceOutput()
        src = out.GetConnectedSources()[0] if out and out.HasConnectedSource() else []
        if src:
            surf = UsdShade.Shader(src[0].source.GetPrim())
        if not surf or surf.GetIdAttr().Get() != "UsdPreviewSurface":
            for p in Usd.PrimRange(mat.GetPrim()):
                s = UsdShade.Shader(p)
                if s and s.GetIdAttr().Get() == "UsdPreviewSurface":
                    surf = s
                    break
        rgba = [0.7, 0.7, 0.7, 1.0]
        metallic, roughness, tex = 0.0, 0.5, None
        if surf:
            def inp(n):
                i = surf.GetInput(n)
                if not i:
                    return None, None
                if i.HasConnectedSource():
                    s = i.GetConnectedSources()[0]
                    if s:
                        return None, UsdShade.Shader(s[0].source.GetPrim())
                return i.Get(), None
            v, sh = inp("diffuseColor")
            if v is not None:
                rgba[:3] = list(v)
            if sh is not None and sh.GetIdAttr().Get() == "UsdUVTexture":
                f = sh.GetInput("file").Get() if sh.GetInput("file") else None
                if f is not None:
                    tex = f.resolvedPath or self._abs(sh.GetPrim(), f.path)
                    rgba[:3] = [1, 1, 1]
                    sc = sh.GetInput("scale").Get() if sh.GetInput("scale") else None
                    if sc is not None:
                        rgba[:3] = list(sc)[:3]
            v, _ = inp("opacity")
            if v is not None:
                rgba[3] = float(v)
            v, _ = inp("metallic")
            if v is not None:
                metallic = float(v)
            v, _ = inp("roughness")
            if v is not None:
                roughness = float(v)
        n = self.uname(self.prefix + "mat_" + mat.GetPrim().GetName(), set(self.materials.values()))
        self.materials[key] = n
        attrs = 'name="%s" rgba="%s" metallic="%s" roughness="%s" specular="%s"' % (
            n, _fmt(rgba, 4), _fmt(metallic, 3), _fmt(roughness, 3), _fmt(0.2 + 0.5 * metallic, 3))
        if tex and os.path.isfile(tex):
            t = self.texture(tex)
            if t:
                attrs += ' texture="%s"' % t
        self.assets.append("<material %s/>" % attrs)
        return n

    def _abs(self, prim, p):
        for spec in prim.GetPrimStack():
            d = os.path.dirname(spec.layer.realPath)
            c = os.path.normpath(os.path.join(d, p))
            if os.path.isfile(c):
                return c
        return None

    def texture(self, path):
        if path in self.textures:
            return self.textures[path]
        os.makedirs(self.tex_dir, exist_ok=True)
        base = _name(os.path.splitext(os.path.basename(path))[0])
        n = self.uname(self.prefix + "tex_" + base, set(self.textures.values()))
        dst = os.path.join(self.tex_dir, n + ".png")
        try:
            from PIL import Image
            im = Image.open(path)
            if im.mode not in ("RGB", "RGBA"):
                im = im.convert("RGBA" if "A" in im.mode else "RGB")
            if max(im.size) > self.max_tex:
                s = self.max_tex / max(im.size)
                im = im.resize((max(1, int(im.size[0] * s)), max(1, int(im.size[1] * s))), Image.LANCZOS)
            im.save(dst)
        except ImportError:
            if not path.lower().endswith(".png"):
                self.warn.append("texture %s skipped (needs Pillow to convert)" % path)
                self.textures[path] = None
                return None
            shutil.copyfile(path, dst)
        self.textures[path] = n
        self.assets.append('<texture name="%s" type="2d" file="textures/%s.png"/>' % (n, n))
        return n

    # -- meshes ----------------------------------------------------------------------
    def mesh_data(self, prim, T):
        """Triangulated corners of a UsdGeom.Mesh in frame T (row-vector 4x4).
        Returns (pos[n,3], nrm[n,3], uv[n,2] or None, tris[m,3] corner indices, subsets)."""
        m = UsdGeom.Mesh(prim)
        pts = np.array(m.GetPointsAttr().Get() or [], dtype=np.float64)
        cnt = np.array(m.GetFaceVertexCountsAttr().Get() or [], dtype=np.int64)
        idx = np.array(m.GetFaceVertexIndicesAttr().Get() or [], dtype=np.int64)
        if len(pts) == 0 or len(cnt) == 0:
            return None
        P = np.c_[pts, np.ones(len(pts))] @ T
        P = P[:, :3]
        flip = np.linalg.det(T[:3, :3]) < 0
        if m.GetOrientationAttr().Get() == UsdGeom.Tokens.leftHanded:
            flip = not flip
        # Fan-triangulate faces into corner triples (indices into the corner arrays).
        starts = np.r_[0, np.cumsum(cnt)[:-1]]
        tri = []
        tri_face = []
        for f, (s, c) in enumerate(zip(starts, cnt)):
            for k in range(1, c - 1):
                tri.append((s, s + k + 1, s + k) if flip else (s, s + k, s + k + 1))
                tri_face.append(f)
        tri = np.array(tri, dtype=np.int64).reshape(-1, 3)
        tri_face = np.array(tri_face, dtype=np.int64)
        cpos = P[idx]
        # UVs
        uv = None
        pv = UsdGeom.PrimvarsAPI(prim)
        for nm in ("st", "st0", "UVMap", "uv", "map1"):
            v = pv.GetPrimvar(nm)
            if v and v.HasValue():
                vals = np.array(v.Get(), dtype=np.float64)
                ind = v.GetIndices()
                interp = v.GetInterpolation()
                if ind is not None and len(ind):
                    vals = vals[np.array(ind)]
                if interp == UsdGeom.Tokens.faceVarying and len(vals) == len(idx):
                    uv = vals
                elif interp in (UsdGeom.Tokens.vertex, UsdGeom.Tokens.varying) and len(vals) == len(pts):
                    uv = vals[idx]
                break
        # Normals: authored, else smoothed with a 40 degree crease.
        nrm = None
        na = m.GetNormalsAttr()
        nv = pv.GetPrimvar("normals")
        nvals, ninterp = None, None
        if nv and nv.HasValue():
            nvals = np.array(nv.ComputeFlattened(), dtype=np.float64)
            ninterp = nv.GetInterpolation()
        elif na and na.HasValue():
            nvals = np.array(na.Get(), dtype=np.float64)
            ninterp = m.GetNormalsInterpolation()
        R = T[:3, :3]
        Rn = np.linalg.inv(R).T
        if nvals is not None:
            if ninterp == UsdGeom.Tokens.faceVarying and len(nvals) == len(idx):
                nrm = nvals
            elif ninterp in (UsdGeom.Tokens.vertex, UsdGeom.Tokens.varying) and len(nvals) == len(pts):
                nrm = nvals[idx]
            elif ninterp == UsdGeom.Tokens.uniform and len(nvals) == len(cnt):
                nrm = np.repeat(nvals, cnt, axis=0)
            if nrm is not None:
                nrm = nrm @ Rn
                if m.GetOrientationAttr().Get() == UsdGeom.Tokens.leftHanded:
                    nrm = -nrm
        if nrm is None:
            nrm = self.smooth_normals(cpos, idx, tri, tri_face, len(cnt))
        ln = np.linalg.norm(nrm, axis=1)
        ln[ln == 0] = 1
        nrm = nrm / ln[:, None]
        # Material subsets: face -> material key
        subsets = []
        for ss in UsdGeom.Subset.GetGeomSubsets(UsdGeom.Imageable(prim)):
            if ss.GetElementTypeAttr().Get() != UsdGeom.Tokens.face:
                continue
            faces = np.array(ss.GetIndicesAttr().Get() or [], dtype=np.int64)
            subsets.append((ss.GetPrim(), faces))
        return cpos, nrm, uv, tri, tri_face, subsets

    @staticmethod
    def smooth_normals(cpos, idx, tri, tri_face, nfaces, crease_deg=40.0):
        fn = np.zeros((nfaces, 3))
        a, b, c = cpos[tri[:, 0]], cpos[tri[:, 1]], cpos[tri[:, 2]]
        np.add.at(fn, tri_face, np.cross(b - a, c - a))
        l = np.linalg.norm(fn, axis=1)
        l[l == 0] = 1
        fnu = fn / l[:, None]
        cf = np.zeros(len(idx), dtype=np.int64)   # corner -> face
        for k in range(3):
            cf[tri[:, k]] = tri_face
        # Per corner, sum the area-weighted normals of the faces around its point
        # that lie within the crease angle of its own face.
        cos_t = math.cos(math.radians(crease_deg))
        order = np.argsort(idx, kind="stable")
        sidx = idx[order]
        bounds = np.r_[0, np.nonzero(np.diff(sidx))[0] + 1, len(sidx)]
        out = np.zeros((len(idx), 3))
        for s, e in zip(bounds[:-1], bounds[1:]):
            cs = order[s:e]
            faces = np.unique(cf[cs])
            fu = fnu[faces]
            fw = fn[faces]
            for c in cs:
                f = fnu[cf[c]]
                keep = fu @ f >= cos_t
                out[c] = fw[keep].sum(axis=0)
        return out

    def write_obj(self, name, chunks):
        """chunks: list of (pos, nrm, uv, tris) with per-corner attributes. Writes
        meshes/<name>.obj for MuJoCo and, beside it, meshes/<name>.glb, which URLab
        imports instead (Interchange reads glTF in metres; its OBJ path goes through
        the FBX importer, which takes the numbers as centimetres and drops small parts
        as degenerate)."""
        os.makedirs(self.mesh_dir, exist_ok=True)
        has_uv = all(c[2] is not None for c in chunks)
        P, N, T, F = [], [], [], []
        vo = 0
        for pos, nrm, uv, tris in chunks:
            used = np.unique(tris)
            remap = -np.ones(len(pos), dtype=np.int64)
            remap[used] = np.arange(len(used))
            P.append(pos[used]); N.append(nrm[used])
            if has_uv:
                T.append(uv[used])
            F.append(remap[tris] + vo)
            vo += len(used)
        P = np.vstack(P); N = np.vstack(N); F = np.vstack(F)
        T = np.vstack(T) if has_uv else None
        # Weld corners that share position, normal and UV.
        key = np.hstack([np.round(P, 6), np.round(N, 3)] + ([np.round(T, 5)] if has_uv else []))
        _, first, inv = np.unique(key, axis=0, return_index=True, return_inverse=True)
        inv = inv.reshape(-1)
        P, N = P[first], N[first]
        T = T[first] if has_uv else None
        F = inv[F]
        F = F[(F[:, 0] != F[:, 1]) & (F[:, 1] != F[:, 2]) & (F[:, 0] != F[:, 2])]
        path = os.path.join(self.mesh_dir, name + ".obj")
        with open(path, "w") as f:
            f.write("# %s, converted by simready_mjcf.py\n" % name)
            np.savetxt(f, P, fmt="v %.6f %.6f %.6f")
            if has_uv:
                np.savetxt(f, T, fmt="vt %.6f %.6f")
            np.savetxt(f, N, fmt="vn %.5f %.5f %.5f")
            if has_uv:
                np.savetxt(f, np.repeat(F + 1, 3, axis=1), fmt="f %d/%d/%d %d/%d/%d %d/%d/%d")
            else:
                np.savetxt(f, np.repeat(F + 1, 2, axis=1), fmt="f %d//%d %d//%d %d//%d")
        if self.glb:
            write_glb(os.path.join(self.mesh_dir, name + ".glb"), P, N, T, F, name)
        return path, len(P), len(F)

    # -- geoms -----------------------------------------------------------------------
    def friction(self, prim):
        mat, _ = UsdShade.MaterialBindingAPI(prim).ComputeBoundMaterial("physics")
        if mat and mat.GetPrim().HasAPI(UsdPhysics.MaterialAPI):
            pm = UsdPhysics.MaterialAPI(mat.GetPrim())
            f = pm.GetDynamicFrictionAttr().Get()
            if f is None:
                f = pm.GetStaticFrictionAttr().Get()
            if f is not None and f > 0:
                return float(f)
        return None

    def collider_geom(self, c, body, Wb, used):
        T = self.W(c) @ np.linalg.inv(Wb)
        t, R, sc = _decompose(T)
        sc = np.abs(sc)
        tname = c.GetTypeName()
        nm = self.uname(self.prefix + "col_" + c.GetName(), used)
        a = {"name": nm, "class": "%scollision" % self.cls}
        Rg = R
        if tname == "Cube":
            s = c.GetAttribute("size").Get() or 2.0
            a.update(type="box", size=_fmt(0.5 * s * sc))
        elif tname == "Sphere":
            r = c.GetAttribute("radius").Get() or 1.0
            a.update(type="sphere", size=_fmt([r * sc.max()]))
        elif tname in ("Capsule", "Cylinder", "Capsule_1", "Cylinder_1"):
            ax = c.GetAttribute("axis").Get() or "Z"
            r = c.GetAttribute("radius").Get()
            if r is None:
                r = max(c.GetAttribute("radiusTop").Get() or 0, c.GetAttribute("radiusBottom").Get() or 0) or 1.0
            h = c.GetAttribute("height").Get() or 2.0
            i = "XYZ".index(ax)
            rr = r * max(sc[j] for j in range(3) if j != i)
            hh = 0.5 * h * sc[i]
            align = {"X": np.array([[0, 0, -1], [0, 1, 0], [1, 0, 0]], float),   # z -> x
                     "Y": np.array([[1, 0, 0], [0, 0, 1], [0, -1, 0]], float),   # z -> y
                     "Z": np.eye(3)}[ax]
            Rg = align @ R
            a.update(type="capsule" if tname.startswith("Capsule") else "cylinder", size=_fmt([rr, hh]))
        elif tname == "Mesh":
            approx = None
            if c.HasAPI(UsdPhysics.MeshCollisionAPI):
                approx = UsdPhysics.MeshCollisionAPI(c).GetApproximationAttr().Get()
            md = self.mesh_data(c, self.W(c) @ np.linalg.inv(Wb))
            if not md:
                return []
            cpos, nrm, uv, tri, _, _ = md
            pieces = [(cpos, tri)]
            if self.decompose and approx in (None, "none", "sdf", "meshSimplification"):
                pieces = self.coacd(cpos, tri) or pieces
            pieces = [self.hull(pp, tt) for pp, tt in pieces]
            out = []
            for k, (pp, tt) in enumerate(pieces):
                mn = self.uname(self.prefix + "colmesh_" + c.GetName() + ("_%d" % k if len(pieces) > 1 else ""),
                                self.mesh_names)
                self.write_obj(mn, [(pp, np.zeros_like(pp) + [0, 0, 1], None, tt)])
                self.assets.append('<mesh name="%s" file="meshes/%s.obj" maxhullvert="64"/>' % (mn, mn))
                g = {"name": self.uname(self.prefix + "col_" + c.GetName(), used), "class": "%scollision" % self.cls,
                     "type": "mesh", "mesh": mn}
                f = self.friction(c)
                if f is not None:
                    g["friction"] = "%s 0.005 0.0001" % _fmt(f, 3)
                out.append(g)
            if approx in ("none", "sdf", "meshSimplification") and not self.decompose:
                self.warn.append("collider %s (%s) is hulled; --decompose splits concave colliders" % (c.GetPath(), approx))
            return out
        else:
            self.warn.append("collider %s of type %s skipped" % (c.GetPath(), tname))
            return []
        a["pos"] = _fmt(t)
        q = _quat_wxyz(Rg)
        if abs(abs(q[0]) - 1) > 1e-9:
            a["quat"] = _fmt(q)
        f = self.friction(c)
        if f is not None:
            a["friction"] = "%s 0.005 0.0001" % _fmt(f, 3)
        return [a]

    @staticmethod
    def hull(pts, tri):
        """Convex hull of a collider (MuJoCo collides with the hull anyway), so a
        whole-mesh collider is not written out a second time in full. Needs SciPy;
        without it the mesh is kept as it is."""
        try:
            from scipy.spatial import ConvexHull
        except ImportError:
            return pts, tri
        u = np.unique(np.round(pts, 7), axis=0)
        if len(u) < 4:
            return pts, tri
        try:
            h = ConvexHull(u)
        except Exception:
            return pts, tri
        used = np.unique(h.simplices)
        remap = -np.ones(len(u), dtype=np.int64)
        remap[used] = np.arange(len(used))
        v = u[used]
        f = remap[h.simplices]
        c = v.mean(axis=0)
        n = np.cross(v[f[:, 1]] - v[f[:, 0]], v[f[:, 2]] - v[f[:, 0]])
        flip = np.einsum("ij,ij->i", n, v[f[:, 0]] - c) < 0
        f[flip] = f[flip][:, ::-1]
        return v, f

    def coacd(self, cpos, tri):
        try:
            import coacd
        except ImportError:
            self.warn.append("--decompose needs `pip install coacd`; hulling instead")
            return None
        # Weld corners into vertices for CoACD.
        key = np.round(cpos, 6)
        uniq, inv = np.unique(key, axis=0, return_inverse=True)
        mesh = coacd.Mesh(uniq, inv.reshape(-1)[tri])
        parts = coacd.run_coacd(mesh, threshold=0.05, max_convex_hull=24)
        return [(np.asarray(v, float), np.asarray(f, np.int64)) for v, f in parts]

    # -- output ----------------------------------------------------------------------
    def convert(self):
        self.collect()
        self.tree()
        os.makedirs(self.out, exist_ok=True)
        bname = {}
        used = set()
        for b in self.bodies:
            bname[b.GetPath()] = self.uname(self.prefix + b.GetName(), used)
        frames = {b.GetPath(): _rigid(self.W(b)) for b in self.bodies}
        # Visual meshes grouped by (body, material).
        vis = {}
        for v in self.visuals:
            b = self.body_of(v) or self.root
            vis.setdefault(b.GetPath(), []).append(v)
        cols = {}
        for c in self.colliders:
            b = self.body_of(c) or self.root
            cols.setdefault(b.GetPath(), []).append(c)
        geom_names = set()
        report = {"source": self.path, "variants": self.variants, "bodies": [], "joints": []}

        def body_xml(b, depth):
            P = b.GetPath()
            Wb = frames[P]
            par = self.parent[P]
            ind = "  " * depth
            if par is None:
                rel = Wb
            else:
                rel = Wb @ np.linalg.inv(frames[par[0].GetPath()])
            t, R, _ = _decompose(rel)
            attrs = 'name="%s" pos="%s"' % (bname[P], _fmt(t))
            q = _quat_wxyz(R)
            if abs(abs(q[0]) - 1) > 1e-9:
                attrs += ' quat="%s"' % _fmt(q)
            lines = [ind + "<body %s>" % attrs]
            if par is None and self.free:
                lines.append(ind + '  <freejoint name="%sroot"/>' % self.prefix)
            if par is not None and par[1] is not None:
                jl = self.joint_xml(par[1], par[0], b, par[2], frames)
                if jl:
                    lines.append(ind + "  " + jl[0])
                    report["joints"].append(jl[1])
            m, com, I, Rp = self.body_mass(b)
            if m:
                if I is None:
                    lines.append(ind + '  <!-- mass only: inertia from the colliders -->')
                    self._mass_only = getattr(self, "_mass_only", {})
                    self._mass_only[P] = m
                else:
                    ia = 'mass="%s" pos="%s" diaginertia="%s"' % (_fmt(m), _fmt(com if com is not None else [0, 0, 0]), _fmt(I, 9))
                    q = _quat_wxyz(Rp)
                    if abs(abs(q[0]) - 1) > 1e-9:
                        ia += ' quat="%s"' % _fmt(q)
                    lines.append(ind + "  <inertial %s/>" % ia)
            # Visuals
            groups = {}
            for v in vis.get(P, []):
                md = self.mesh_data(v, self.W(v) @ np.linalg.inv(Wb))
                if not md:
                    continue
                cpos, nrm, uv, tri, tface, subsets = md
                if subsets:
                    covered = np.zeros(tface.max() + 1 if len(tface) else 0, bool)
                    for sp, faces in subsets:
                        sel = np.isin(tface, faces)
                        covered[faces[faces < len(covered)]] = True
                        if sel.any():
                            groups.setdefault(self.material(sp), []).append((cpos, nrm, uv, tri[sel]))
                    rest = ~covered[tface]
                    if rest.any():
                        groups.setdefault(self.material(v), []).append((cpos, nrm, uv, tri[rest]))
                else:
                    groups.setdefault(self.material(v), []).append((cpos, nrm, uv, tri))
            nvert = 0
            for mat, chunks in groups.items():
                if any(c[2] is None for c in chunks):
                    chunks = [(p, n, None, t) for p, n, _, t in chunks]
                mn = self.uname(self.prefix + "vis_" + b.GetName() + "_" + mat[len(self.prefix) + 4:], self.mesh_names)
                _, nv, nf = self.write_obj(mn, chunks)
                nvert += nv
                self.assets.append('<mesh name="%s" file="meshes/%s.obj" inertia="shell"/>' % (mn, mn))
                lines.append(ind + '  <geom name="%s" class="%svisual" mesh="%s" material="%s"/>' % (
                    self.uname(mn, geom_names), self.cls, mn, mat))
            # Colliders
            ncol = 0
            for c in cols.get(P, []):
                for g in self.collider_geom(c, b, Wb, geom_names):
                    ncol += 1
                    lines.append(ind + "  <geom %s/>" % " ".join('%s="%s"' % kv for kv in g.items()))
            report["bodies"].append({"name": bname[P], "prim": str(P), "mass": m, "colliders": ncol,
                                     "visual_vertices": nvert, "materials": len(groups)})
            for ch in self.children.get(P, []):
                lines += body_xml(ch, depth + 1)
            lines.append(ind + "</body>")
            return lines

        body_lines = body_xml(self.root, 2)
        # Keyframes: "closed" is the authored pose; "open" puts each limited joint at
        # 90% of the way to the limit farther from it (URLab's ResetToKeyframe).
        closed, opened = [], []
        if self.free:
            t, R, _ = _decompose(frames[self.root.GetPath()])
            closed += list(t) + list(_quat_wxyz(R)); opened += list(t) + list(_quat_wxyz(R))
        for j in report["joints"]:
            if j["type"] == "ball":
                closed += [1, 0, 0, 0]; opened += [1, 0, 0, 0]
                continue
            unit = math.radians if j["type"] == "hinge" else float
            ref = j["authored_value"]
            tgt = ref
            if j["limits"]:
                far = max(j["limits"], key=lambda v: abs(v - ref))
                tgt = ref + 0.9 * (far - ref)
            closed.append(unit(ref)); opened.append(unit(tgt))
        keys = []
        if closed:
            keys = ['  <keyframe>',
                    '    <key name="closed" qpos="%s"/>' % _fmt(closed),
                    '    <key name="open" qpos="%s"/>' % _fmt(opened),
                    '  </keyframe>']
        p = self.cls
        xml = ['<mujoco model="%s">' % self.name,
               '  <!-- Converted from %s by simready_mjcf.py; variants %s -->' % (
                   os.path.basename(self.path), json.dumps(self.variants).replace("--", "-")),
               '  <compiler angle="degree" autolimits="true"/>',
               '  <default>',
               '    <default class="%svisual">' % p,
               '      <geom type="mesh" contype="0" conaffinity="0" group="2" mass="0"/>',
               '    </default>',
               '    <default class="%scollision">' % p,
               '      <geom group="3" rgba="0.6 0.3 0.3 0.5" condim="4" solref="0.004 1"/>',
               '    </default>',
               '    <default class="%shinge">' % p,
               '      <joint type="hinge" damping="%s" armature="%s" frictionloss="%s"/>' % (
                   _fmt(self.hinge_damping), _fmt(self.armature), _fmt(self.frictionloss)),
               '    </default>',
               '    <default class="%sslide">' % p,
               '      <joint type="slide" damping="%s" armature="%s" frictionloss="%s"/>' % (
                   _fmt(self.slide_damping), _fmt(self.armature), _fmt(self.frictionloss)),
               '    </default>',
               '  </default>',
               '  <asset>'] + ["    " + a for a in self.assets] + [
               '  </asset>',
               '  <worldbody>'] + body_lines + ['  </worldbody>'] + keys + ['</mujoco>']
        path = os.path.join(self.out, self.name + ".xml")
        with open(path, "w") as f:
            f.write("\n".join(xml) + "\n")
        report["xml"] = path
        report["warnings"] = self.warn
        with open(os.path.join(self.out, "conversion.json"), "w") as f:
            json.dump(report, f, indent=1, default=str)
        for w in self.warn:
            print("warning:", w)
        return path, report

    def joint_xml(self, jprim, parent, child, rev, frames):
        """Joint living in `child`. rev: the USD joint's body0 is the child."""
        J = UsdPhysics.Joint(jprim)
        ln = self.uname(self.prefix + jprim.GetName(), getattr(self, "_jn", set()))
        self._jn = getattr(self, "_jn", set()) | {ln}
        if jprim.IsA(UsdPhysics.FixedJoint):
            return None
        is_rev = jprim.IsA(UsdPhysics.RevoluteJoint)
        is_pri = jprim.IsA(UsdPhysics.PrismaticJoint)
        is_sph = jprim.IsA(UsdPhysics.SphericalJoint)
        if not (is_rev or is_pri or is_sph):
            self.warn.append("joint %s (%s) welded" % (jprim.GetPath(), jprim.GetTypeName()))
            return None

        def frame(k, body):
            lp = J.GetLocalPos0Attr().Get() if k == 0 else J.GetLocalPos1Attr().Get()
            lr = J.GetLocalRot0Attr().Get() if k == 0 else J.GetLocalRot1Attr().Get()
            Wfull = self.W(body)
            _, _, sc = _decompose(Wfull)
            lp = np.array(lp if lp is not None else (0, 0, 0), float) * sc
            Rl = _rot_from_quat(lr) if lr is not None else np.eye(3)
            return lp, Rl  # in body frame (rigid)

        # USD body0/body1 as authored, resolved to bodies.
        t0 = J.GetBody0Rel().GetTargets()
        b0 = self.body_of(self.stage.GetPrimAtPath(t0[0])) if t0 else None
        if b0 is None:
            return None
        b1 = child if not rev else parent
        b0 = parent if not rev else child
        # Joint frames are given relative to the prim the joint names, which may be a
        # mesh under the body (NVIDIA): re-express them in the body frame.
        def named(k):
            t = (J.GetBody0Rel() if k == 0 else J.GetBody1Rel()).GetTargets()
            return self.stage.GetPrimAtPath(t[0])
        def body_frame(k, body):
            lp, Rl = frame(k, named(k))
            Tn = _rigid(self.W(named(k))) @ np.linalg.inv(frames[body.GetPath()])
            tn, Rn, _ = _decompose(Tn)
            return lp @ Rn + tn, Rl @ Rn
        p0, R0 = body_frame(0, b0)
        p1, R1 = body_frame(1, b1)
        ax = _axis_vec(J.GetPrim().GetAttribute("physics:axis").Get() if not is_sph else "X")
        # World joint frames in the authored pose -> current joint value.
        W0 = frames[b0.GetPath()]
        W1 = frames[b1.GetPath()]
        Rw0 = R0 @ W0[:3, :3]
        Rw1 = R1 @ W1[:3, :3]
        pw0 = np.r_[p0, 1] @ W0
        pw1 = np.r_[p1, 1] @ W1
        value = 0.0
        if is_rev:
            Rrel = Rw1 @ Rw0.T          # frame1 relative to frame0 (row-vector)
            Rc = Rrel.T                  # column form
            # Twist angle about the axis (expressed in frame0).
            q = Gf.Matrix3d(*Rc.ravel().tolist()).ExtractRotation().GetQuat()
            w, v = q.GetReal(), np.array(q.GetImaginary())
            ang = 2 * math.atan2(np.dot(v, ax), w)
            ang = (ang + math.pi) % (2 * math.pi) - math.pi
            value = math.degrees(ang)
        elif is_pri:
            value = float(np.dot((pw1 - pw0)[:3] @ Rw0.T, ax))
        # MJCF joint in the child body: frame of the child side.
        if not rev:
            pos, axis_c, sign = p1, ax @ R1, 1.0
        else:
            pos, axis_c, sign = p0, ax @ R0, -1.0
        lo = J.GetPrim().GetAttribute("physics:lowerLimit").Get() if not is_sph else None
        hi = J.GetPrim().GetAttribute("physics:upperLimit").Get() if not is_sph else None
        limited = lo is not None and hi is not None and abs(lo) < INF_LIMIT and abs(hi) < INF_LIMIT and lo <= hi
        if is_pri and limited:
            lo, hi = lo * self.mpu, hi * self.mpu
        jtype = "hinge" if is_rev else "slide" if is_pri else "ball"
        attrs = {"name": ln}
        if is_sph:
            attrs["type"] = "ball"
        else:
            attrs["class"] = self.cls + jtype
        attrs["pos"] = _fmt(pos)
        if not is_sph:
            attrs["axis"] = _fmt(axis_c)
        if limited:
            r = sorted((sign * lo, sign * hi))
            attrs["range"] = _fmt(r)
        if abs(value) > 1e-6 and not is_sph:
            attrs["ref"] = _fmt(sign * value)
        # Drives: damping / spring.
        dname = "angular" if is_rev else "linear"
        if not is_sph and jprim.HasAPI(UsdPhysics.DriveAPI, dname):
            d = UsdPhysics.DriveAPI(jprim, dname)
            k = d.GetStiffnessAttr().Get() or 0.0
            c = d.GetDampingAttr().Get() or 0.0
            conv = math.degrees(1.0) if is_rev else 1.0  # PhysX angular drives are per degree
            if c > 0:
                attrs["damping"] = _fmt(c * conv)
            if k > 0:
                attrs["stiffness"] = _fmt(k * conv)
                tp = d.GetTargetPositionAttr().Get() or 0.0
                attrs["springref"] = _fmt(sign * tp * (self.mpu if is_pri else 1.0))
        fe = jprim.GetAttribute("physxJointAxis:%s:dynamicFrictionEffort" % ("angular" if is_rev else "linear"))
        if fe and fe.HasAuthoredValue() and fe.Get():
            attrs["frictionloss"] = _fmt(fe.Get(), 4)
        info = {"name": ln, "type": jtype, "prim": str(jprim.GetPath()), "parent": str(parent.GetPath()),
                "child": str(child.GetPath()), "range": attrs.get("range"), "authored_value": sign * value,
                "reversed": rev, "limits": sorted((sign * lo, sign * hi)) if limited else None}
        return "<joint %s/>" % " ".join('%s="%s"' % kv for kv in attrs.items()), info


NAMESPACES = {"nvidia": "nv", "forgecad": "fc", "lgelectronics": "lg"}


def default_name(usd):
    """sr_<nv|fc|lg>_<asset> from the cache layout <namespace>__<dataset>/<asset>/..., the
    stem URLab names the Blueprint and its /Game/MuJoCoImports/<stem>_Assets folder after."""
    parts = os.path.normpath(usd).split(os.sep)
    for i, p in enumerate(parts[:-1]):
        if "__" in p and i + 1 < len(parts) - 1:
            ns = NAMESPACES.get(p.split("__")[0].lower(), _name(p.split("__")[0]).lower()[:6])
            return "sr_%s_%s" % (ns, _name(parts[i + 1]).lower())
    return "sr_" + _name(os.path.splitext(os.path.basename(usd))[0]).lower()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("usd", help="root USD of a SimReady package (or ramms_selection.json's root_usd)")
    ap.add_argument("--out", required=True, help="output folder")
    ap.add_argument("--name", help="model name (default sr_<nv|fc|lg>_<asset>)")
    ap.add_argument("--prefix", default="", help="prefix for every MJCF name, for composing several models")
    ap.add_argument("--free", action="store_true", help="give the root body a free joint instead of welding it")
    ap.add_argument("--hinge-damping", type=float, default=0.5, help="hinge damping without a USD drive (N m s/rad)")
    ap.add_argument("--slide-damping", type=float, default=5.0, help="slide damping without a USD drive (N s/m)")
    ap.add_argument("--armature", type=float, default=0.005)
    ap.add_argument("--frictionloss", type=float, default=0.0)
    ap.add_argument("--max-tex", type=int, default=2048)
    ap.add_argument("--decompose", action="store_true", help="CoACD-decompose concave mesh colliders")
    ap.add_argument("--no-glb", action="store_true", help="skip the .glb copies URLab imports")
    ap.add_argument("--variant", action="append", default=[], help="variantSet=selection (repeatable)")
    a = ap.parse_args(argv)
    usd = os.path.abspath(a.usd)
    name = a.name or default_name(usd)
    variants = dict(v.split("=", 1) for v in a.variant)
    conv = Converter(usd, os.path.abspath(a.out), name, a.prefix, a.free, a.hinge_damping, a.slide_damping,
                     a.armature, a.frictionloss, a.max_tex, a.decompose, variants, glb=not a.no_glb)
    path, rep = conv.convert()
    print("wrote", path)
    for j in rep["joints"]:
        print("  joint %(name)s %(type)s range %(range)s authored %(authored_value).3f" % j)
    try:
        import mujoco
        m = mujoco.MjModel.from_xml_path(path)
        print("mujoco %s: nbody %d njnt %d ngeom %d nmesh %d mass %.2f kg" % (
            mujoco.__version__, m.nbody, m.njnt, m.ngeom, m.nmesh, sum(m.body_mass)))
    except ImportError:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
