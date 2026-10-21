"""Import UrbanVerse-100K assets and build a palette from them.

The assets are chosen and downloaded outside Unreal by Tools/urbanverse_fetch.py, which writes
<cache>/ramms_selection.json (slot -> assets, with their annotations). This module imports those GLBs
(Interchange glTF) under /Game/UrbanVerse/<Slot>/ and builds /RammsWorldGen/Palettes/PAL_UrbanVerse: a copy of
PAL_Default whose street-furniture, vehicle and hazard slots hold the UrbanVerse meshes instead (or as well,
with mode="mix"). Everything else (surfaces, facades, trees, houses, interiors) stays as in PAL_Default.

    import ramms_worldgen.urbanverse as UV
    UV.build(r"D:/urbanverse")                     # import + palette
    UV.build(r"D:/urbanverse", mode="mix")         # UrbanVerse variants alongside the Fab ones

The imported assets are not committed (their licenses vary per asset; see <cache>/ATTRIBUTION.md, which is
copied next to them as /Game/UrbanVerse/ATTRIBUTION.md on disk).

Assets are metric and keep their own size. Their front comes from the annotation's frontView (which of the
four turntable renders, 0/90/180/270 degrees, shows the functional front); FRONT_YAW_BASE / FRONT_YAW_STEP
map that to the yaw that turns the front to +X, the palette convention (checked with Scripts/worldgen/lineup.py).
"""
import json
import os
import shutil

import unreal

from . import _helpers as H
from . import palettes as P

DEST = "/Game/UrbanVerse"
PALETTE = H.PALETTES + "/PAL_UrbanVerse"
# yaw (degrees) that turns an imported UrbanVerse mesh's front to +X, given its annotated frontView index
FRONT_YAW_BASE = 0.0
FRONT_YAW_STEP = 90.0
# triangle count above which an imported mesh is made a Nanite mesh
NANITE_TRIS = 50000
# Slots whose front is a long face (shelters, dumpsters, barriers): only the axis is decided, from the bounds.
LONG_FRONT_SLOTS = {"BusStop", "Hazard.Dumpster", "Hazard.Barrier", "Bench", "NewspaperBox"}
# GLBs bigger than this are skipped (scanned assets with 4K+ textures: slow to import, little gain on a street)
MAX_GLB_MB = 40.0


def _asset_name(item):
    cat = "".join(c if c.isalnum() else "_" for c in str(item.get("category", "obj"))).strip("_")[:32]
    return "SM_UV_%s_%s" % (cat, item["uid"][:8])


def _import_glb(glb, dest_dir, name):
    """Import one GLB as a single static mesh (with its materials and textures). Returns the mesh."""
    path = dest_dir + "/" + name
    if H.EAL.does_asset_exist(path):
        return unreal.load_asset(path)
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", glb)
    task.set_editor_property("destination_path", dest_dir)
    task.set_editor_property("destination_name", name)
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", True)
    try:
        pipe = unreal.InterchangeGenericAssetsPipeline()
        mp = pipe.get_editor_property("mesh_pipeline")
        # (glTF scenes already come in as one combined static mesh by default in 5.8)
        mp.set_editor_property("import_skeletal_meshes", False)
        try:
            pipe.get_editor_property("animation_pipeline").set_editor_property("import_animations", False)
        except Exception:  # noqa: BLE001
            pass
        task.set_editor_property("options", pipe)
    except Exception as exc:  # noqa: BLE001 - fall back to the project's default glTF import settings
        H.warn("urbanverse: interchange options not set (%s)" % exc)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    meshes = []
    for p in task.get_editor_property("imported_object_paths") or []:
        a = unreal.load_asset(p)
        if isinstance(a, unreal.StaticMesh):
            meshes.append(a)
    if not meshes:
        # Interchange can name the mesh after the glTF node and put it in a folder named after the file: look
        # (by class, without loading everything) for static meshes under the destination that match either.
        stem = os.path.splitext(os.path.basename(glb))[0]
        for p in H.EAL.list_assets(dest_dir, recursive=True):
            ad = H.EAL.find_asset_data(p)
            if str(ad.asset_class_path.asset_name) != "StaticMesh":
                continue
            if name in str(ad.asset_name) or ("/" + stem + "/") in str(ad.package_path) + "/" or str(ad.package_path).endswith("/" + stem):
                a = unreal.load_asset(p)
                if a is not None:
                    meshes.append(a)
    if not meshes:
        return None
    if len(meshes) > 1:
        # Only the largest is used: a GLB holding several objects (a scene, or a model split per part) loses the rest.
        H.warn("urbanverse: %s imported as %d meshes (%s); keeping the largest" % (os.path.basename(glb), len(meshes), ", ".join(m.get_name() for m in meshes[:4])))
    mesh = max(meshes, key=lambda m: m.get_bounds().box_extent.length())
    if mesh.get_path_name().split(".")[0] != path:
        try:
            H.EAL.rename_asset(mesh.get_path_name().split(".")[0], path)
            mesh = unreal.load_asset(path)
        except Exception:  # noqa: BLE001
            pass
    try:
        if mesh.get_num_triangles(0) > NANITE_TRIS:
            ns = mesh.get_editor_property("nanite_settings")
            ns.set_editor_property("enabled", True)
            mesh.set_editor_property("nanite_settings", ns)
    except Exception as exc:  # noqa: BLE001
        H.warn("urbanverse: nanite for %s: %s" % (name, exc))
    H.EAL.save_loaded_asset(mesh, False)
    return mesh


# --- materials ----------------------------------------------------------------------------------------------
# Interchange turns every glTF material flagged alphaMode BLEND into a translucent material. Many UrbanVerse
# exporters flag whole models BLEND although their textures are opaque (or only cut out), and translucent
# surfaces get no Lumen GI or proper shadowing: shelters come out milky and ghostly, some car bodies near
# black. Tools/urbanverse_fetch.py records each material's alpha (factor and texture histogram); from it a
# material is made opaque, masked, or real glass (our own glass material), or left alone.
GLASS = H.ROOT + "/Materials/M_RammsGlass"
GLASS_TEX = H.ROOT + "/Materials/M_RammsGlassAtlas"
GLASS_VERSION = "8"      # bump when the glass graphs change: existing assets are rebuilt in place
GLASS_WORDS = ("glass", "vitre", "window", "windshield", "windscreen", "lens", "clear", "transparent", "phare", "headlight")
FOLIAGE_WORDS = ("fern", "leaf", "leaves", "plant", "grass", "foliage", "flower", "bush", "ivy")


# Per-asset decisions the analysis cannot make: flat (untextured) materials flagged BLEND at full alpha are
# usually opaque parts, but on these assets they are the glass panes. {uid prefix: {material name: kind}}
MATERIAL_OVERRIDES = {
    "1ff7401e": {"Texture003": "glass", "Texture004": "glass"},     # bus shelter: side and back panes
    "e79f70d2": {"lambert1": "glass"},                              # bus shelter: back and side panes
    "977fe8fc": {"lambert1": "glass"},                              # bus shelter: back and side panes
}


def _norm(name):
    return "".join(c for c in str(name).lower() if c.isalnum())


def material_kind(name, info):
    """"opaque" | "mask" | "glass" | "glass_tex" | "keep" for one glTF material's analysis."""
    if info.get("alpha_mode") != "BLEND" or "shadow" in str(name).lower():
        return "keep"       # opaque / masked already; baked contact-shadow cards stay as authored
    f = float(info.get("alpha_factor", 1.0))
    glassy = any(w in str(name).lower() for w in GLASS_WORDS) or float(info.get("transmission") or 0.0) > 0.0
    h = info.get("alpha_hist")
    if not info.get("textured") or h is None:
        if f < 0.02:
            return "keep"   # fully transparent on purpose (hidden helper geometry)
        return "glass" if (f < 0.95 or glassy) else "opaque"
    clear, opaque = h[0], h[-1]
    mid = max(0.0, 1.0 - clear - opaque)
    if clear > 0.9:
        return "keep"
    if any(w in str(name).lower() for w in FOLIAGE_WORDS):
        return "mask"       # leaf cards: soft alpha edges, but cut-outs, not glass
    if mid < 0.02 and clear < 0.01:
        return "glass_tex" if glassy else "opaque"
    if mid < 0.12 and not glassy:
        return "mask"
    return "glass_tex"


def _param(mat, cls, name, x, y, default=None):
    mel = unreal.MaterialEditingLibrary
    e = mel.create_material_expression(mat, cls, x, y)
    e.set_editor_property("parameter_name", name)
    if default is not None:
        e.set_editor_property("texture" if cls is unreal.MaterialExpressionTextureSampleParameter2D else "default_value", default)
    return e


def _glass_parent(textured):
    """The two glass materials (built once under /RammsWorldGen/Materials):

    M_RammsGlass, for panes: thin two-sided translucency, forward shaded, with a near-black base colour (glass
    shows what is behind it and what it reflects, not a colour of its own) and Fresnel opacity - almost clear
    face-on, reflective at grazing angles.

    M_RammsGlassAtlas, for one texture holding both glass and solid parts (a whole shelter, a car with its
    windows): masked, so frames, posters and bodywork stay solid and fully lit; the see-through parts are
    dithered (temporal AA resolves them to partial cover) with glass roughness and a dark base, so they read
    as glass with real reflections rather than a milky film."""
    path = GLASS_TEX if textured else GLASS
    mel = unreal.MaterialEditingLibrary
    MP = unreal.MaterialProperty
    if H.EAL.does_asset_exist(path):
        mat = unreal.load_asset(path)
        if H.EAL.get_metadata_tag(mat, "RammsGlassVersion") == GLASS_VERSION:
            return mat
        mel.delete_all_material_expressions(mat)     # an older graph: rebuilt in place (instances keep pointing at it)
    else:
        d, n = path.rsplit("/", 1)
        H.ensure_dir(d)
        mat = H.asset_tools().create_asset(n, d, unreal.Material, unreal.MaterialFactoryNew())
    mat.set_editor_property("two_sided", True)
    metal = mel.create_material_expression(mat, unreal.MaterialExpressionConstant, -300, 450)
    metal.set_editor_property("r", 0.0)
    mel.connect_material_property(metal, "", MP.MP_METALLIC)
    spec = mel.create_material_expression(mat, unreal.MaterialExpressionConstant, -300, 350)
    spec.set_editor_property("r", 0.5)            # F0 0.04, as for glass
    mel.connect_material_property(spec, "", MP.MP_SPECULAR)
    tint = _param(mat, unreal.MaterialExpressionVectorParameter, "Tint", -1100, -300, unreal.LinearColor(1.0, 1.0, 1.0, 1.0))
    rough = _param(mat, unreal.MaterialExpressionScalarParameter, "Roughness", -1100, 250, 0.03)
    dark = _param(mat, unreal.MaterialExpressionScalarParameter, "GlassBrightness", -1100, -180, 0.08)
    gcol = mel.create_material_expression(mat, unreal.MaterialExpressionMultiply, -800, -250)
    mel.connect_material_expressions(tint, "", gcol, "A")
    mel.connect_material_expressions(dark, "", gcol, "B")
    if not textured:
        mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
        mat.set_editor_property("translucency_lighting_mode", unreal.TranslucencyLightingMode.TLM_SURFACE_PER_PIXEL_LIGHTING)
        for prop, val in (("use_hq_forward_reflections", True), ("forward_blends_sky_light_cubemaps", True)):
            try:
                mat.set_editor_property(prop, val)
            except Exception:  # noqa: BLE001
                pass
        mel.connect_material_property(gcol, "", MP.MP_BASE_COLOR)
        mel.connect_material_property(rough, "", MP.MP_ROUGHNESS)
        fres = mel.create_material_expression(mat, unreal.MaterialExpressionFresnel, -800, 100)
        fres.set_editor_property("exponent", 5.0)
        fres.set_editor_property("base_reflect_fraction", 0.0)
        o0 = _param(mat, unreal.MaterialExpressionScalarParameter, "OpacityFront", -1100, 50, 0.5)
        o1 = _param(mat, unreal.MaterialExpressionScalarParameter, "OpacityEdge", -1100, 150, 0.85)
        ol = mel.create_material_expression(mat, unreal.MaterialExpressionLinearInterpolate, -500, 100)
        mel.connect_material_expressions(o0, "", ol, "A")
        mel.connect_material_expressions(o1, "", ol, "B")
        mel.connect_material_expressions(fres, "", ol, "Alpha")
        mel.connect_material_property(ol, "", MP.MP_OPACITY)
    else:
        mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
        mat.set_editor_property("opacity_mask_clip_value", 0.5)
        try:
            mat.set_editor_property("dithered_lod_transition", False)
        except Exception:  # noqa: BLE001
            pass
        white = unreal.load_asset("/Engine/EngineResources/WhiteSquareTexture")
        black = unreal.load_asset("/Engine/EngineResources/Black")
        tex = _param(mat, unreal.MaterialExpressionTextureSampleParameter2D, "BaseTexture", -1500, -100, white)
        scale = _param(mat, unreal.MaterialExpressionScalarParameter, "AlphaScale", -1500, 150, 1.0)
        a = mel.create_material_expression(mat, unreal.MaterialExpressionMultiply, -1200, 100)
        mel.connect_material_expressions(tex, "A", a, "A")
        mel.connect_material_expressions(scale, "", a, "B")
        # solid weight: 0 for glass (alpha < ~0.85), 1 for opaque texels
        w = mel.create_material_expression(mat, unreal.MaterialExpressionSubtract, -1000, 250)
        mel.connect_material_expressions(a, "", w, "A")
        w.set_editor_property("const_b", 0.85)
        w2 = mel.create_material_expression(mat, unreal.MaterialExpressionMultiply, -850, 250)
        mel.connect_material_expressions(w, "", w2, "A")
        w2.set_editor_property("const_b", 6.67)
        # clamp to [0, 1] with Max / Min (their inputs have names Python can address, unlike Saturate's)
        lo = mel.create_material_expression(mat, unreal.MaterialExpressionMax, -720, 250)
        _connect(w2, "", lo, ("A",))
        lo.set_editor_property("const_b", 0.0)
        sw = mel.create_material_expression(mat, unreal.MaterialExpressionMin, -600, 250)
        _connect(lo, "", sw, ("A",))
        sw.set_editor_property("const_b", 1.0)
        tcol = mel.create_material_expression(mat, unreal.MaterialExpressionMultiply, -800, -100)
        mel.connect_material_expressions(tex, "RGB", tcol, "A")
        mel.connect_material_expressions(tint, "", tcol, "B")
        col = mel.create_material_expression(mat, unreal.MaterialExpressionLinearInterpolate, -450, -150)
        mel.connect_material_expressions(gcol, "", col, "A")
        mel.connect_material_expressions(tcol, "", col, "B")
        mel.connect_material_expressions(sw, "", col, "Alpha")
        mel.connect_material_property(col, "", MP.MP_BASE_COLOR)
        solid = _param(mat, unreal.MaterialExpressionScalarParameter, "SolidRoughness", -1100, 400, 0.5)
        rl = mel.create_material_expression(mat, unreal.MaterialExpressionLinearInterpolate, -450, 250)
        mel.connect_material_expressions(rough, "", rl, "A")
        mel.connect_material_expressions(solid, "", rl, "B")
        mel.connect_material_expressions(sw, "", rl, "Alpha")
        mel.connect_material_property(rl, "", MP.MP_ROUGHNESS)
        # coverage: solid texels 1; glass texels dithered at GlassCover (their alpha is an exporter's guess,
        # not a measure of the glass, so it does not set the density)
        gc = _param(mat, unreal.MaterialExpressionScalarParameter, "GlassCover", -1100, 550, 0.5)
        cv = mel.create_material_expression(mat, unreal.MaterialExpressionLinearInterpolate, -650, 550)
        mel.connect_material_expressions(gc, "", cv, "A")
        cv.set_editor_property("const_b", 1.0)
        mel.connect_material_expressions(sw, "", cv, "Alpha")
        cut = mel.create_material_expression(mat, unreal.MaterialExpressionSubtract, -650, 700)
        mel.connect_material_expressions(a, "", cut, "A")
        cut.set_editor_property("const_b", 0.02)    # fully clear texels (alpha ~0) stay holes
        # DitherTemporalAA is an engine material function, not an expression class
        dith = mel.create_material_expression(mat, unreal.MaterialExpressionMaterialFunctionCall, -450, 550)
        fn = unreal.load_asset("/Engine/Functions/Engine_MaterialFunctions02/Utility/DitherTemporalAA")
        try:
            dith.set_material_function(fn)
        except Exception:  # noqa: BLE001
            dith.set_editor_property("material_function", fn)
        _connect(cv, "", dith, ("Alpha Threshold", "AlphaThreshold"))
        keep = mel.create_material_expression(mat, unreal.MaterialExpressionIf, -250, 600)
        mel.connect_material_expressions(cut, "", keep, "A")
        keep.set_editor_property("const_b", 0.0)
        _connect(dith, "Result", keep, ("AGreaterThanB", "A > B"))
        zero = mel.create_material_expression(mat, unreal.MaterialExpressionConstant, -450, 750)
        _connect(zero, "", keep, ("ALessThanB", "A < B"))
        _connect(dith, "Result", keep, ("AEqualsB", "A == B"))
        mel.connect_material_property(keep, "", MP.MP_OPACITY_MASK)
        etex = _param(mat, unreal.MaterialExpressionTextureSampleParameter2D, "EmissiveTexture", -1500, 850, black)
        estr = _param(mat, unreal.MaterialExpressionScalarParameter, "EmissiveStrength", -1500, 1050, 0.0)
        emul = mel.create_material_expression(mat, unreal.MaterialExpressionMultiply, -450, 900)
        mel.connect_material_expressions(etex, "RGB", emul, "A")
        mel.connect_material_expressions(estr, "", emul, "B")
        mel.connect_material_property(emul, "", MP.MP_EMISSIVE_COLOR)
    mel.layout_material_expressions(mat)
    mel.recompile_material(mat)
    H.EAL.set_metadata_tag(mat, "RammsGlassVersion", GLASS_VERSION)
    H.save(mat)
    return mat


def _connect(src, out, dst, names):
    """Connect to the first input name the expression accepts (Python sees some pins by display name)."""
    for n in names:
        if unreal.MaterialEditingLibrary.connect_material_expressions(src, out, dst, n):
            return True
    H.warn("glass material: could not connect %s to %s (%s)" % (src.get_name(), dst.get_name(), names))
    return False


def _mi_params(mi):
    sc = {str(p.parameter_info.name): p.parameter_value for p in mi.get_editor_property("scalar_parameter_values")}
    ve = {str(p.parameter_info.name): p.parameter_value for p in mi.get_editor_property("vector_parameter_values")}
    tx = {str(p.parameter_info.name): p.parameter_value for p in mi.get_editor_property("texture_parameter_values")}
    return sc, ve, tx


def _set_blend(mi, blend, alpha_mode):
    mel = unreal.MaterialEditingLibrary
    bo = mi.get_editor_property("base_property_overrides")
    bo.set_editor_property("override_blend_mode", True)
    bo.set_editor_property("blend_mode", blend)
    if blend == unreal.BlendMode.BLEND_MASKED:
        bo.set_editor_property("override_opacity_mask_clip_value", True)
        bo.set_editor_property("opacity_mask_clip_value", 0.5)
    mi.set_editor_property("base_property_overrides", bo)
    mel.set_material_instance_scalar_parameter_value(mi, "AlphaMode", float(alpha_mode))
    mel.update_material_instance(mi)
    H.EAL.save_loaded_asset(mi, False)


def _glass_instance(mi, kind):
    """A glass instance next to the imported one (<name>_RammsGlass), carrying its colour and texture."""
    parent = _glass_parent(kind == "glass_tex")
    d = mi.get_path_name().rsplit("/", 1)[0]
    name = mi.get_name() + "_RammsGlass"
    path = d + "/" + name
    g = unreal.load_asset(path) if H.EAL.does_asset_exist(path) else \
        H.asset_tools().create_asset(name, d, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
    mel = unreal.MaterialEditingLibrary
    g.set_editor_property("parent", parent)
    sc, ve, tx = _mi_params(mi)
    c = ve.get("BaseColorFactor")
    col = (c.r, c.g, c.b) if c is not None else (1.0, 1.0, 1.0)
    a = c.a if c is not None else 1.0
    if kind == "glass":
        # Face-on opacity from the glTF alpha (tinted / smoked glass is denser), a light tint from its colour.
        # (a slight green-grey, as float glass has, when the source colour is plain white)
        base = (0.86, 0.93, 0.92) if min(col) > 0.95 else col
        tint = tuple(0.5 * v + 0.5 * b for v, b in zip(col, base))
        mel.set_material_instance_vector_parameter_value(g, "Tint", unreal.LinearColor(tint[0], tint[1], tint[2], 1.0))
        # Translucent specular is scaled by opacity, so a pane needs real face-on opacity to show reflections;
        # the near-black base keeps it from going milky (it darkens what is behind, as real glass does).
        # (checked by eye on shelters and windscreens: below ~0.4 a pane all but disappears)
        mel.set_material_instance_scalar_parameter_value(g, "OpacityFront", max(0.4, min(0.75, 0.4 + a * 0.4)) if a < 0.95 else 0.5)
        mel.set_material_instance_scalar_parameter_value(g, "GlassBrightness", 0.08 if sum(col) > 0.3 else 0.02)
        mel.set_material_instance_scalar_parameter_value(g, "Roughness", min(float(sc.get("RoughnessFactor", 0.03)), 0.06))
    else:
        if tx.get("BaseColorTexture") is not None:
            mel.set_material_instance_texture_parameter_value(g, "BaseTexture", tx["BaseColorTexture"])
        mel.set_material_instance_vector_parameter_value(g, "Tint", unreal.LinearColor(col[0], col[1], col[2], 1.0))
        mel.set_material_instance_scalar_parameter_value(g, "AlphaScale", a)
        mel.set_material_instance_scalar_parameter_value(g, "SolidRoughness", max(0.3, float(sc.get("RoughnessFactor", 0.6))))
        if tx.get("EmissiveTexture") is not None:
            mel.set_material_instance_texture_parameter_value(g, "EmissiveTexture", tx["EmissiveTexture"])
            e = ve.get("EmissiveFactor")
            # glTF emissive is often a lightmap-like bake of the whole atlas: keep it faint
            mel.set_material_instance_scalar_parameter_value(g, "EmissiveStrength", 0.25 * (1.0 if e is None else max(e.r, e.g, e.b)))
    try:
        g.set_editor_property("phys_material", mi.get_editor_property("phys_material"))
    except Exception:  # noqa: BLE001
        pass
    mel.update_material_instance(g)
    H.EAL.save_loaded_asset(g, False)
    return g


def _decide(mi, info, uid=""):
    """material_kind for an imported instance: MATERIAL_OVERRIDES, else the fetch analysis when there is one;
    without it, the instance's own factors, leaving textured materials alone."""
    name = mi.get_name()
    for pre, table in MATERIAL_OVERRIDES.items():
        if uid.startswith(pre) and name in table:
            return table[name]
    inf = info.get(_norm(name))
    if inf is not None:
        return material_kind(name, inf)
    sc, ve, tx = _mi_params(mi)
    if "BaseColorTexture" in tx:
        return "keep"
    c = ve.get("BaseColorFactor")
    return material_kind(name, {"alpha_mode": "BLEND", "alpha_factor": c.a if c is not None else 1.0, "textured": False,
                                "transmission": sc.get("TransmissionFactor")})


def fix_materials(mesh, item):
    """Make the mesh's mis-flagged translucent materials opaque or masked and its glass real glass.
    Returns {kind: count}. Idempotent (a slot already on our glass is left alone)."""
    info = {_norm(k): v for k, v in (item.get("materials") or {}).items()}
    done = {}
    changed_mesh = False
    mats = list(mesh.get_editor_property("static_materials") or [])
    for i, sm in enumerate(mats):
        mi = sm.get_editor_property("material_interface")
        if not isinstance(mi, unreal.MaterialInstanceConstant):
            continue
        if mi.get_name().endswith("_RammsGlass"):
            # Decided before: re-decide from the imported instance (the rules may have changed).
            orig = mi.get_path_name().rsplit("/", 1)[0] + "/" + mi.get_name()[:-len("_RammsGlass")]
            o = unreal.load_asset(orig) if H.EAL.does_asset_exist(orig) else None
            if o is None:
                continue
            kind = _decide(o, info, item.get("uid", ""))
            if kind in ("glass", "glass_tex"):
                _glass_instance(o, kind)          # refresh its parameters
                done[kind] = done.get(kind, 0) + 1
                continue
            mesh.set_material(i, o)               # no longer glass: back to the imported instance
            mi = o
            changed_mesh = True
        bo = mi.get_editor_property("base_property_overrides")
        blend = bo.get_editor_property("blend_mode") if bo.get_editor_property("override_blend_mode") else None
        kind = _decide(mi, info, item.get("uid", ""))
        if blend not in (unreal.BlendMode.BLEND_TRANSLUCENT, unreal.BlendMode.BLEND_TRANSLUCENT_COLORED_TRANSMITTANCE):
            # Opaque as imported (or made opaque by an earlier build): only an override turns it into glass.
            if not (kind in ("glass", "glass_tex") and any(item.get("uid", "").startswith(p) and mi.get_name() in t
                                                           for p, t in MATERIAL_OVERRIDES.items())):
                continue
        if kind == "opaque":
            _set_blend(mi, unreal.BlendMode.BLEND_OPAQUE, 0)
        elif kind == "mask":
            _set_blend(mi, unreal.BlendMode.BLEND_MASKED, 1)
        elif kind in ("glass", "glass_tex"):
            mesh.set_material(i, _glass_instance(mi, kind))
            changed_mesh = True
        done[kind] = done.get(kind, 0) + 1
    if changed_mesh:
        H.EAL.save_loaded_asset(mesh, False)
    return done


def _physical_material(item):
    """A physical material per friction value (rounded to 0.05), so the robot's wheels see the annotated friction."""
    f = item.get("friction")
    try:
        f = max(0.05, min(2.0, round(float(f) * 20.0) / 20.0))
    except (TypeError, ValueError):
        return None
    path = DEST + "/Physics/PM_UV_Friction_%03d" % int(round(f * 100))
    if H.EAL.does_asset_exist(path):
        return unreal.load_asset(path)
    H.ensure_dir(DEST + "/Physics")
    pm = unreal.AssetToolsHelpers.get_asset_tools().create_asset(path.rsplit("/", 1)[1], DEST + "/Physics", unreal.PhysicalMaterial, unreal.PhysicalMaterialFactoryNew())
    pm.set_editor_property("friction", f)
    pm.set_editor_property("static_friction", f)
    H.save(pm)
    return pm


def _apply_friction(mesh, pm):
    """Give the mesh the friction of its physical material on every collision path. The body setup's physical
    material only covers simple shapes; palette meshes without simple collision collide complex-as-simple, where
    the material's physical material is the one used. The imported material instances are per asset (Interchange
    writes them under the GLB's own folder), so they can carry it without touching anything else."""
    bs = mesh.get_editor_property("body_setup")
    if bs is not None:
        bs.set_editor_property("phys_material", pm)
    for sm in mesh.get_editor_property("static_materials") or []:
        mat = sm.get_editor_property("material_interface")
        if isinstance(mat, unreal.MaterialInstanceConstant) and mat.get_path_name().startswith(DEST + "/"):
            try:
                if mat.get_editor_property("phys_material") != pm:
                    mat.set_editor_property("phys_material", pm)
                    H.EAL.save_loaded_asset(mat, False)
            except Exception as exc:  # noqa: BLE001
                H.warn("urbanverse: physical material on %s: %s" % (mat.get_name(), exc))
    H.EAL.save_loaded_asset(mesh, False)


MATERIAL_REPORT = {}   # mesh -> {kind: count} from the last import_selection


def import_selection(cache_dir, slots=None):
    """Import every asset listed in <cache_dir>/ramms_selection.json. Returns {slot: [(mesh, item)]}."""
    with open(os.path.join(cache_dir, "ramms_selection.json"), encoding="utf-8") as fh:
        sel = json.load(fh)
    out = {}
    report_materials = MATERIAL_REPORT
    report_materials.clear()
    for slot, items in sel.get("slots", {}).items():
        if slots and slot not in slots:
            continue
        dest = DEST + "/" + slot.replace(".", "_")
        H.ensure_dir(dest)
        for it in items:
            glb = it.get("glb")
            if not glb or not os.path.exists(glb):
                H.warn("urbanverse: %s %s has no GLB" % (slot, it.get("uid")))
                continue
            if os.path.getsize(glb) > MAX_GLB_MB * 1e6:
                H.log("urbanverse: skip %s %s (%.0f MB)" % (slot, it.get("uid"), os.path.getsize(glb) / 1e6))
                continue
            try:
                mesh = _import_glb(glb, dest, _asset_name(it))
            except Exception as exc:  # noqa: BLE001
                H.warn("urbanverse: import %s failed: %s" % (glb, exc))
                mesh = None
            if mesh is None:
                continue
            try:
                kinds = fix_materials(mesh, it)
                if kinds:
                    report_materials[mesh.get_name()] = kinds
            except Exception as exc:  # noqa: BLE001
                H.warn("urbanverse: materials of %s: %s" % (mesh.get_name(), exc))
            pm = _physical_material(it)
            if pm is not None:
                try:
                    _apply_friction(mesh, pm)
                except Exception as exc:  # noqa: BLE001
                    H.warn("urbanverse: physical material for %s: %s" % (mesh.get_name(), exc))
            out.setdefault(slot, []).append((mesh, it))
    # Interchange can leave the materials and textures it creates unsaved: save the whole folder.
    H.EAL.save_directory(DEST, only_if_is_dirty=True, recursive=True)
    # Keep the attribution next to the imported assets.
    try:
        src = os.path.join(cache_dir, "ATTRIBUTION.md")
        dst = os.path.join(unreal.Paths.project_content_dir(), "UrbanVerse", "ATTRIBUTION.md")
        if os.path.exists(src):
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copyfile(src, dst)
    except Exception as exc:  # noqa: BLE001
        H.warn("urbanverse: attribution copy: %s" % exc)
    return out


def front_yaw(item):
    try:
        v = int(item.get("front_view") or 0)
    except (TypeError, ValueError):
        v = 0
    return ((FRONT_YAW_BASE + FRONT_YAW_STEP * v) + 180.0) % 360.0 - 180.0


def _yaw(mesh, item, slot, facing):
    """Facing yaw for an imported mesh: the annotation's frontView ("annotation"), or the palette's geometric
    heuristic ("geometry", the default: backrests and cisterns mark the back). For long-fronted slots the
    heuristic's answer is kept only if it puts the long side across the facing direction; otherwise the front is
    one of the two long faces, and the annotation picks which."""
    if facing == "annotation":
        return front_yaw(item)
    if slot in LONG_FRONT_SLOTS:
        b = mesh.get_bounds().box_extent
        y = P._facing_yaw(mesh, mesh.get_bounds(), slot)
        long_x = b.x >= b.y
        across = (abs(y) % 180.0 < 1.0) != long_x      # yaw 0/180 keeps X as the facing axis
        if across:
            return y
        cands = (90.0, -90.0) if long_x else (0.0, 180.0)
        a = front_yaw(item)
        return min(cands, key=lambda c: abs((c - a + 180.0) % 360.0 - 180.0))
    return None


def source_id(item):
    """Where a palette entry came from, carried into the manifest (features' "source")."""
    return "urbanverse:%s|%s" % (item.get("uid", ""), item.get("category", ""))


def build(cache_dir, mode="replace", slots=None, path=PALETTE, facing="geometry"):
    """Import the selection and build the UrbanVerse palette. mode: "replace" (UrbanVerse only in the slots it
    fills) or "mix" (added to the Fab variants). Returns (palette, report)."""
    P._COLLISION_DONE.clear()
    imported = import_selection(cache_dir, slots)
    if not H.EAL.does_asset_exist(P.DEFAULT_PALETTE):
        P.build_default_palette()
    default = unreal.load_asset(P.DEFAULT_PALETTE)
    if default is None:
        raise RuntimeError("urbanverse: default palette %s did not load" % P.DEFAULT_PALETTE)
    # Refresh in place (deleting it would break every reference to it: presets, open levels, generators set to it).
    pal = unreal.load_asset(path) if H.EAL.does_asset_exist(path) else H.EAL.duplicate_asset(P.DEFAULT_PALETTE, path)
    if pal is None:
        raise RuntimeError("urbanverse: could not create palette %s" % path)
    pal.set_editor_property("slots", default.get_editor_property("slots"))
    pal.set_editor_property("default_material", default.get_editor_property("default_material"))
    slots_map = pal.get_editor_property("slots")
    report = {"slots": {}, "mode": mode, "materials": dict(MATERIAL_REPORT)}
    for slot, pairs in imported.items():
        entries = []
        for mesh, it in pairs:
            e = P._mesh_entry(mesh, slot, yaw=_yaw(mesh, it, slot, facing), normalise=False)
            e.set_editor_property("source_id", source_id(it))
            entries.append(e)
        s = slots_map.get(unreal.Name(slot)) if slots_map else None
        if s is None:
            s = unreal.RammsPaletteSlot()
        existing = list(s.get_editor_property("meshes")) if mode == "mix" else []
        s.set_editor_property("meshes", existing + entries)
        s.set_editor_property("note", "UrbanVerse-100K (%s): %s" % (mode, ", ".join(sorted({it["category"] for _m, it in pairs}))))
        pal.set_slot(unreal.Name(slot), s)
        report["slots"][slot] = [m.get_name() for m, _it in pairs]
    H.save(pal)
    H.log("urbanverse palette %s: %s" % (path, {k: len(v) for k, v in report["slots"].items()}))
    return pal, report
