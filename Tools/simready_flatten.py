"""Flatten a SimReady package's root USD into one mesh for Unreal (used by simready_fetch.py).

A SimReady asset is an articulated, instanced USD: rigid bodies with their own transforms, joints, colliders,
physics-engine variants, OmniPBR (MDL) materials with a UsdPreviewSurface fallback. Unreal's Interchange USD
import brings every part in as its own static mesh at a local pivot (its combine option does not apply to USD),
so the parts are merged here instead: every renderable mesh (not guide/proxy/invisible, not a collider), world
transforms baked, converted to Z-up metres, re-centred on the bottom centre of its bounds, one GeomSubset per
material. Materials keep their UsdPreviewSurface network (identical copies on several parts become one), with
texture paths made absolute. Faces without a material get one from their displayColor.

    from simready_flatten import flatten
    report = flatten("…/simready_usd/sm_bin_waste_a01_01.usd", "…/ramms_merged.usdc")

Needs usd-core (pip install usd-core).
"""
from __future__ import annotations

import re

from pxr import Gf, Kind, Sdf, Usd, UsdGeom, UsdPhysics, UsdShade, Vt

SKIP_NAME = re.compile(r"collider|collision|_coll\b|convex|physics_proxy|proxy_geom", re.I)
PREVIEW_IDS = {"UsdPreviewSurface", "UsdUVTexture", "UsdPrimvarReader_float2", "UsdPrimvarReader_float", "UsdPrimvarReader_float3",
               "UsdPrimvarReader_int", "UsdPrimvarReader_string", "UsdPrimvarReader_normal", "UsdPrimvarReader_point", "UsdPrimvarReader_vector",
               "UsdPrimvarReader_float4", "UsdTransform2d"}


def _to_face_varying(values, interp, counts, indices):
    """Expand a primvar's flattened values to one value per face-vertex."""
    if values is None:
        return None
    n_fv = len(indices)
    if interp == UsdGeom.Tokens.faceVarying:
        return list(values) if len(values) == n_fv else None
    if interp in (UsdGeom.Tokens.vertex, UsdGeom.Tokens.varying):
        try:
            return [values[i] for i in indices]
        except IndexError:
            return None
    if interp == UsdGeom.Tokens.uniform:
        out = []
        for f, c in enumerate(counts):
            out.extend([values[f]] * c)
        return out if len(values) >= len(counts) else None
    if interp == UsdGeom.Tokens.constant and len(values):
        return [values[0]] * n_fv
    return None


def _smooth_normals(points, counts, indices):
    acc = [Gf.Vec3d(0, 0, 0) for _ in points]
    o = 0
    for c in counts:
        ids = indices[o:o + c]
        p0 = Gf.Vec3d(points[ids[0]])
        for k in range(1, c - 1):
            n = Gf.Cross(Gf.Vec3d(points[ids[k]]) - p0, Gf.Vec3d(points[ids[k + 1]]) - p0)
            for i in (ids[0], ids[k], ids[k + 1]):
                acc[i] += n
        o += c
    out = []
    for v in acc:
        L = v.GetLength()
        out.append(Gf.Vec3f(v / L) if L > 1e-12 else Gf.Vec3f(0, 0, 1))
    return [out[i] for i in indices]


def _uv_primvar(prim):
    pvs = UsdGeom.PrimvarsAPI(prim).GetPrimvars()
    best = None
    for pv in pvs:
        tn = pv.GetTypeName()
        if tn not in (Sdf.ValueTypeNames.TexCoord2fArray, Sdf.ValueTypeNames.Float2Array, Sdf.ValueTypeNames.TexCoord2dArray, Sdf.ValueTypeNames.Double2Array):
            continue
        name = pv.GetPrimvarName()
        rank = 0 if name == "st" else 1 if name.lower() in ("st0", "uv", "uvmap", "map1", "st_0", "uv0") else 2
        if best is None or rank < best[0]:
            best = (rank, pv)
    return best[1] if best else None


_SIG_KEYS = {}


def _material_key(mat):
    if not mat:
        return None
    path = str(mat.GetPath())
    if path not in _SIG_KEYS:
        _SIG_KEYS[path] = "sig:%s:%x" % _signature(mat) if len(_signature(mat)) == 2 else "sig:%s" % _signature(mat)[0]
    return _SIG_KEYS[path]


def _surface_shader(mat):
    """The UsdPreviewSurface feeding the material (universal render context), or None."""
    for ctx in ("", "universal"):
        out = mat.GetSurfaceOutput(ctx) if ctx else mat.GetSurfaceOutput()
        if not out:
            continue
        srcs = out.GetConnectedSources()[0]
        for s in srcs:
            sh = UsdShade.Shader(s.source.GetPrim())
            if sh and sh.GetIdAttr().Get() == "UsdPreviewSurface":
                return sh
    # some exporters wire the preview surface without the plain surface output
    for c in Usd.PrimRange(mat.GetPrim(), Usd.TraverseInstanceProxies()):
        sh = UsdShade.Shader(c)
        if sh and sh.GetIdAttr().Get() == "UsdPreviewSurface":
            return sh
    return None


def _abs_asset(v, attr=None):
    """An asset path made absolute: resolved if it resolves, else anchored to the layer that authored it."""
    if not isinstance(v, Sdf.AssetPath) or not v.path:
        return v
    p = v.resolvedPath
    if not p and attr is not None:
        for spec in attr.GetPropertyStack(Usd.TimeCode.Default()):
            if spec.HasDefaultValue():
                p = spec.layer.ComputeAbsolutePath(v.path)
                break
    return Sdf.AssetPath((p or v.path).replace("\\", "/"))


def _signature(mat):
    """What a material looks like (its preview inputs and textures): parts that carry their own copy of the
    same material collapse into one slot."""
    surf = _surface_shader(mat)
    if surf is None:
        return (mat.GetPrim().GetName(),)
    sig, todo, seen = [], [surf], set()
    while todo:
        sh = todo.pop()
        if str(sh.GetPath()) in seen:
            continue
        seen.add(str(sh.GetPath()))
        for inp in sorted(sh.GetInputs(), key=lambda i: i.GetBaseName()):
            srcs = inp.GetConnectedSources()[0]
            if srcs:
                up = UsdShade.Shader(srcs[0].source.GetPrim())
                sig.append((sh.GetIdAttr().Get(), inp.GetBaseName(), "->", srcs[0].sourceName))
                if up:
                    todo.append(up)
            else:
                v = inp.Get()
                sig.append((sh.GetIdAttr().Get(), inp.GetBaseName(), str(_abs_asset(v, inp.GetAttr()).path if isinstance(v, Sdf.AssetPath) else v)))
    return (mat.GetPrim().GetName(), hash(tuple(sig)))


def _copy_preview_network(src_mat, dst_stage, dst_path):
    """Recreate src_mat's UsdPreviewSurface network at dst_path; texture paths made absolute. Returns (material, info)."""
    surf = _surface_shader(src_mat)
    mat = UsdShade.Material.Define(dst_stage, dst_path)
    info = {"preview": surf is not None}
    if surf is None:
        return mat, info
    todo, done = [surf], {}
    while todo:
        sh = todo.pop()
        key = str(sh.GetPath())
        if key in done:
            continue
        dname = re.sub(r"[^A-Za-z0-9_]", "_", sh.GetPrim().GetName())
        while any(d == dname for d in done.values()):
            dname += "_"
        done[key] = dname
        for inp in sh.GetInputs():
            for s in inp.GetConnectedSources()[0]:
                up = UsdShade.Shader(s.source.GetPrim())
                if up and up.GetIdAttr().Get() in PREVIEW_IDS:
                    todo.append(up)
    src_stage = src_mat.GetPrim().GetStage()
    for key, dname in done.items():           # define every node (and its outputs) before wiring
        sh = UsdShade.Shader(src_stage.GetPrimAtPath(key))
        d = UsdShade.Shader.Define(dst_stage, dst_path.AppendChild(dname))
        d.CreateIdAttr(sh.GetIdAttr().Get())
        for out in sh.GetOutputs():
            d.CreateOutput(out.GetBaseName(), out.GetTypeName())
    for key, dname in done.items():
        sh = UsdShade.Shader(src_stage.GetPrimAtPath(key))
        d = UsdShade.Shader(dst_stage.GetPrimAtPath(dst_path.AppendChild(dname)))
        for inp in sh.GetInputs():
            di = d.CreateInput(inp.GetBaseName(), inp.GetTypeName())
            srcs = inp.GetConnectedSources()[0]
            if srcs:
                s = srcs[0]
                up_key = str(s.source.GetPath())
                if up_key in done:
                    di.ConnectToSource(dst_path.AppendChild(done[up_key]).AppendProperty("outputs:" + s.sourceName))
                    continue
            v = inp.Get()
            if v is not None:
                di.Set(_abs_asset(v, inp.GetAttr()))
            cs = inp.GetAttr().GetMetadata("colorSpace")
            if cs:
                di.GetAttr().SetMetadata("colorSpace", cs)
    mat.CreateSurfaceOutput().ConnectToSource(dst_path.AppendChild(done[str(surf.GetPath())]).AppendProperty("outputs:surface"))
    info["shaders"] = len(done)
    return mat, info


def _color_material(dst_stage, dst_path, rgb):
    mat = UsdShade.Material.Define(dst_stage, dst_path)
    sh = UsdShade.Shader.Define(dst_stage, dst_path.AppendChild("Surface"))
    sh.CreateIdAttr("UsdPreviewSurface")
    sh.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(*rgb))
    sh.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(0.6)
    sh.CreateInput("metallic", Sdf.ValueTypeNames.Float).Set(0.0)
    mat.CreateSurfaceOutput().ConnectToSource(sh.ConnectableAPI(), "surface")
    return mat


def flatten(root_usd: str, out_usd: str) -> dict:
    _SIG_KEYS.clear()
    stage = Usd.Stage.Open(root_usd, load=Usd.Stage.LoadAll)
    up = UsdGeom.GetStageUpAxis(stage)
    mpu = UsdGeom.GetStageMetersPerUnit(stage) or 1.0
    # world -> Z-up metres
    conv = Gf.Matrix4d().SetScale(Gf.Vec3d(mpu, mpu, mpu))
    if up == UsdGeom.Tokens.y:
        conv = conv * Gf.Matrix4d().SetRotate(Gf.Rotation(Gf.Vec3d(1, 0, 0), 90.0))
    xc = UsdGeom.XformCache()
    pred = Usd.TraverseInstanceProxies(Usd.PrimIsActive & Usd.PrimIsDefined & Usd.PrimIsLoaded & ~Usd.PrimIsAbstract)
    P, N, UV, FC, FI = [], [], [], [], []
    face_mat = []
    mats = {}
    report = {"meshes": 0, "skipped": [], "no_uv": 0}
    friction_w = []
    it = iter(Usd.PrimRange(stage.GetPseudoRoot(), pred))
    for prim in it:
        if prim.IsA(UsdGeom.Imageable):
            img = UsdGeom.Imageable(prim)
            if img.ComputeVisibility() == UsdGeom.Tokens.invisible or img.GetPurposeAttr().Get() in (UsdGeom.Tokens.guide, UsdGeom.Tokens.proxy):
                it.PruneChildren(); report["skipped"].append(str(prim.GetPath())); continue
        if SKIP_NAME.search(prim.GetName()):
            it.PruneChildren(); report["skipped"].append(str(prim.GetPath())); continue
        if not prim.IsA(UsdGeom.Mesh):
            continue
        mesh = UsdGeom.Mesh(prim)
        pts = mesh.GetPointsAttr().Get()
        counts = mesh.GetFaceVertexCountsAttr().Get()
        idx = mesh.GetFaceVertexIndicesAttr().Get()
        if not pts or not counts or not idx:
            continue
        counts, idx = list(counts), list(idx)
        M = xc.GetLocalToWorldTransform(prim) * conv
        flip = (mesh.GetOrientationAttr().Get() == UsdGeom.Tokens.leftHanded) != (M.GetDeterminant3() < 0)
        Mn = M.GetInverse().GetTranspose()
        # normals
        nrm = None
        npv = UsdGeom.PrimvarsAPI(prim).GetPrimvar("normals")
        if npv and npv.HasValue():
            nrm = _to_face_varying(npv.ComputeFlattened(), npv.GetInterpolation(), counts, idx)
        if nrm is None and mesh.GetNormalsAttr().HasValue():
            nrm = _to_face_varying(mesh.GetNormalsAttr().Get(), mesh.GetNormalsInterpolation(), counts, idx)
        if nrm is None:
            nrm = _smooth_normals(pts, counts, idx)
            if flip:
                nrm = [-n for n in nrm]
        # uvs
        uvpv = _uv_primvar(prim)
        uv = _to_face_varying(uvpv.ComputeFlattened(), uvpv.GetInterpolation(), counts, idx) if uvpv else None
        if uv is None:
            report["no_uv"] += 1
            uv = [Gf.Vec2f(0, 0)] * len(idx)
        # per-face material
        mb = UsdShade.MaterialBindingAPI(prim)
        base = mb.ComputeBoundMaterial()[0]
        fm = [_material_key(base)] * len(counts)
        for sub in mb.GetMaterialBindSubsets():
            m = UsdShade.MaterialBindingAPI(sub.GetPrim()).ComputeBoundMaterial()[0]
            for f in sub.GetIndicesAttr().Get() or []:
                if f < len(fm):
                    fm[f] = _material_key(m)
        dc = None
        if base is None:
            dpv = UsdGeom.PrimvarsAPI(prim).GetPrimvar("displayColor")
            if dpv and dpv.HasValue():
                v = dpv.ComputeFlattened()
                if v:
                    c = Gf.Vec3f(0, 0, 0)
                    for x in v: c += Gf.Vec3f(x)
                    dc = tuple(round(float(x) / len(v), 3) for x in c)
        for f, k in enumerate(fm):
            if k is None:
                fm[f] = "__color_%s" % ("_".join("%03d" % int(x * 255) for x in (dc or (0.6, 0.6, 0.6))))
                mats.setdefault(fm[f], ("color", dc or (0.6, 0.6, 0.6)))
            elif k not in mats:
                src_path = next(p for p, kk in _SIG_KEYS.items() if kk == k)
                mats[k] = ("usd", stage.GetPrimAtPath(src_path))
        # friction of the mesh's physics material, weighted by its faces
        pm = mb.ComputeBoundMaterial("physics")[0]
        if pm:
            api = UsdPhysics.MaterialAPI(pm.GetPrim())
            fr = [a.Get() for a in (api.GetStaticFrictionAttr(), api.GetDynamicFrictionAttr()) if a and a.HasAuthoredValue()]
            if fr:
                friction_w.append((sum(fr) / len(fr), len(counts)))
        # append
        base_p = len(P)
        P.extend(Gf.Vec3f(M.Transform(Gf.Vec3d(p))) for p in pts)
        o = 0
        for f, c in enumerate(counts):
            fv = list(range(o, o + c))
            ids = idx[o:o + c]
            if flip:
                fv = fv[::-1]; ids = ids[::-1]
            FC.append(c)
            FI.extend(base_p + i for i in ids)
            for j in fv:
                n = Gf.Vec3f(Mn.TransformDir(Gf.Vec3d(nrm[j])))
                L = n.GetLength()
                N.append(n / L if L > 1e-12 else Gf.Vec3f(0, 0, 1))
                UV.append(Gf.Vec2f(uv[j][0], uv[j][1]))
            face_mat.append(fm[f])
            o += c
        report["meshes"] += 1
    if not FC:
        raise RuntimeError("no renderable mesh in %s" % root_usd)
    # recentre: origin at the bottom centre of the bounds
    lo = Gf.Vec3f(*(min(p[i] for p in P) for i in range(3)))
    hi = Gf.Vec3f(*(max(p[i] for p in P) for i in range(3)))
    off = Gf.Vec3f((lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, lo[2])
    P = [p - off for p in P]
    out = Usd.Stage.CreateNew(out_usd)
    UsdGeom.SetStageUpAxis(out, UsdGeom.Tokens.z)
    UsdGeom.SetStageMetersPerUnit(out, 1.0)
    root = UsdGeom.Xform.Define(out, "/Asset")
    out.SetDefaultPrim(root.GetPrim())
    Usd.ModelAPI(root.GetPrim()).SetKind(Kind.Tokens.component)
    m = UsdGeom.Mesh.Define(out, "/Asset/Mesh")
    m.CreatePointsAttr(Vt.Vec3fArray(P))
    m.CreateFaceVertexCountsAttr(Vt.IntArray(FC))
    m.CreateFaceVertexIndicesAttr(Vt.IntArray(FI))
    m.CreateSubdivisionSchemeAttr(UsdGeom.Tokens.none)
    m.CreateDoubleSidedAttr(False)
    m.CreateExtentAttr(Vt.Vec3fArray([lo - off, hi - off]))
    pv = UsdGeom.PrimvarsAPI(m)
    pv.CreatePrimvar("normals", Sdf.ValueTypeNames.Normal3fArray, UsdGeom.Tokens.faceVarying).Set(Vt.Vec3fArray(N))
    pv.CreatePrimvar("st", Sdf.ValueTypeNames.TexCoord2fArray, UsdGeom.Tokens.faceVarying).Set(Vt.Vec2fArray(UV))
    names = {}
    report["materials"] = {}
    for k, (kind, src) in mats.items():
        nm = re.sub(r"[^A-Za-z0-9_]", "_", (src.GetName() if kind == "usd" else k))[:60] or "Mat"
        if nm[0].isdigit(): nm = "M_" + nm
        while nm in names.values(): nm += "_"
        names[k] = nm
        path = Sdf.Path("/Asset/Looks").AppendChild(nm)
        if kind == "usd":
            mat, info = _copy_preview_network(UsdShade.Material(src), out, path)
            if not info["preview"]:
                _color_material(out, path, (0.6, 0.6, 0.6))
            report["materials"][nm] = info
        else:
            _color_material(out, path, src)
            report["materials"][nm] = {"color": src}
    for k, nm in names.items():
        faces = [f for f, x in enumerate(face_mat) if x == k]
        sub = UsdGeom.Subset.Define(out, "/Asset/Mesh/" + nm)
        sub.CreateElementTypeAttr(UsdGeom.Tokens.face)
        sub.CreateFamilyNameAttr(UsdShade.Tokens.materialBind)
        sub.CreateIndicesAttr(Vt.IntArray(faces))
        UsdShade.MaterialBindingAPI.Apply(sub.GetPrim()).Bind(UsdShade.Material(out.GetPrimAtPath("/Asset/Looks/" + nm)))
    UsdShade.MaterialBindingAPI.Apply(m.GetPrim())
    out.GetRootLayer().Save()
    report.update({"faces": len(FC), "points": len(P), "size_m": [round(hi[i] - lo[i], 3) for i in range(3)], "up": str(up), "mpu": mpu})
    if friction_w:
        tw = sum(w for _f, w in friction_w)
        report["friction"] = round(sum(f * w for f, w in friction_w) / tw, 3)
    return report
