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
import math
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
GLASS_VERSION = "14"      # bump when the glass graphs change: existing assets are rebuilt in place
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


# Per-instance custom data the glass materials read (ISM instances; RammsWorldGen's generator writes it, see
# ARammsWorldGenerator::ApplyGlassVariation). A mesh that is not instanced, or an ISM without custom data,
# gets the defaults, i.e. the material instance's own look.
GLASS_CUSTOM_DATA = (
    ("Frost", 0.0),        # 0: added to the instance's Frost (0 clear .. 1 milky)
    ("Opacity", 1.0),      # 1: opacity multiplier
    ("TintR", 1.0),        # 2-4: colour multiplier
    ("TintG", 1.0),
    ("TintB", 1.0),
    ("Bump", 0.0),         # 5: added to the instance's Bump (0 flat .. 1 strongly rippled)
)
BUMP_TEXTURE = "/Engine/Functions/Engine_MaterialFunctions02/ExampleContent/Textures/water_n"


class _G:
    """Small helper for building material graphs from Python."""

    def __init__(self, mat):
        self.m = mat
        self.mel = unreal.MaterialEditingLibrary

    def x(self, cls, pos, **props):
        e = self.mel.create_material_expression(self.m, cls, pos[0], pos[1])
        for k, v in props.items():
            if isinstance(k, str) and "|" in k:
                for alt in k.split("|"):
                    try:
                        e.set_editor_property(alt, v)
                        break
                    except Exception:  # noqa: BLE001
                        continue
            else:
                e.set_editor_property(k, v)
        return e

    def scalar(self, name, default, pos):
        return self.x(unreal.MaterialExpressionScalarParameter, pos, parameter_name=name, default_value=default)

    def c(self, src, dst, pin, out=""):
        return _connect(src, out, dst, (pin,))

    def op(self, cls, a, b, pos, a_out="", b_out=""):
        """Two-input node (A, B); b may be a number (set as ConstB)."""
        e = self.x(cls, pos)
        self.c(a, e, "A", a_out)
        if isinstance(b, (int, float)):
            e.set_editor_property("const_b", float(b))
        else:
            self.c(b, e, "B", b_out)
        return e

    def lerp(self, a, b, alpha, pos):
        e = self.x(unreal.MaterialExpressionLinearInterpolate, pos)
        for v, pin in ((a, "A"), (b, "B"), (alpha, "Alpha")):
            if isinstance(v, (int, float)):
                e.set_editor_property("const_" + pin.lower(), float(v))
            else:
                self.c(v, e, pin)
        return e

    def clamp01(self, a, pos):
        lo = self.op(unreal.MaterialExpressionMax, a, 0.0, pos)
        return self.op(unreal.MaterialExpressionMin, lo, 1.0, (pos[0] + 120, pos[1]))

    def custom(self, index, default, pos):
        return self.x(unreal.MaterialExpressionPerInstanceCustomData, pos, data_index=index,
                      **{"const_default_value|default_value": float(default)})

    def custom3(self, index, default, pos):
        return self.x(unreal.MaterialExpressionPerInstanceCustomData3Vector, pos, data_index=index,
                      **{"const_default_value|default_value": unreal.LinearColor(default[0], default[1], default[2], 1.0)})

    def prop(self, e, mp):
        self.mel.connect_material_property(e, "", mp)


def _glass_inputs(g, frost_default, y0=-600):
    """Shared inputs of both glass materials: tint, frost and bump, each the instance's parameter combined with
    the per-instance custom data. Returns (tint, frost 0..1, bump 0..1, opacity multiplier, rough, frost_rough,
    frost_bright)."""
    tint_p = g.x(unreal.MaterialExpressionVectorParameter, (-1900, y0), parameter_name="Tint",
                 default_value=unreal.LinearColor(1.0, 1.0, 1.0, 1.0))
    tint = g.op(unreal.MaterialExpressionMultiply, tint_p, g.custom3(2, (1.0, 1.0, 1.0), (-1900, y0 + 120)), (-1600, y0 + 60))
    # InstanceFrost / InstanceBump: how much this material instance follows the per-instance offsets (car
    # windows, for instance, take tint and opacity variation but are never frosted or rippled)
    cfrost = g.op(unreal.MaterialExpressionMultiply, g.custom(0, 0.0, (-2100, y0 + 330)), g.scalar("InstanceFrost", 1.0, (-2100, y0 + 400)), (-1800, y0 + 330))
    frost = g.clamp01(g.op(unreal.MaterialExpressionAdd, g.scalar("Frost", frost_default, (-1900, y0 + 240)), cfrost, (-1600, y0 + 280)), (-1450, y0 + 280))
    cbump = g.op(unreal.MaterialExpressionMultiply, g.custom(5, 0.0, (-2100, y0 + 510)), g.scalar("InstanceBump", 1.0, (-2100, y0 + 580)), (-1800, y0 + 510))
    bump = g.clamp01(g.op(unreal.MaterialExpressionAdd, g.scalar("Bump", 0.0, (-1900, y0 + 420)), cbump, (-1600, y0 + 460)), (-1450, y0 + 460))
    oscale = g.custom(1, 1.0, (-1900, y0 + 600))
    rough = g.scalar("Roughness", 0.03, (-1900, y0 + 690))
    frough = g.scalar("FrostRoughness", 0.55, (-1900, y0 + 780))
    fbright = g.scalar("FrostBrightness", 0.3, (-1900, y0 + 870))
    return tint, frost, bump, oscale, rough, frough, fbright


def _glass_normal(g, bump, pos, mask=None):
    """Rippled-glass normal: a tiling normal map, world-aligned on vertical panes (x+y across, z up) so it
    does not depend on the model's UVs, blended in by `bump` (times `mask`, e.g. glass texels only)."""
    wp = g.x(unreal.MaterialExpressionWorldPosition, (pos[0] - 900, pos[1]))
    across = g.x(unreal.MaterialExpressionConstant3Vector, (pos[0] - 900, pos[1] + 90), constant=unreal.LinearColor(1.0, 1.0, 0.0, 1.0))
    up = g.x(unreal.MaterialExpressionConstant3Vector, (pos[0] - 900, pos[1] + 170), constant=unreal.LinearColor(0.0, 0.0, 1.0, 1.0))
    u = g.op(unreal.MaterialExpressionDotProduct, wp, across, (pos[0] - 700, pos[1]))
    v = g.op(unreal.MaterialExpressionDotProduct, wp, up, (pos[0] - 700, pos[1] + 120))
    uv = g.op(unreal.MaterialExpressionAppendVector, u, v, (pos[0] - 550, pos[1] + 60))
    tile = g.scalar("BumpTiling", 90.0, (pos[0] - 700, pos[1] + 220))
    uvs = g.op(unreal.MaterialExpressionDivide, uv, tile, (pos[0] - 420, pos[1] + 60))
    tex = g.x(unreal.MaterialExpressionTextureSampleParameter2D, (pos[0] - 300, pos[1] + 60), parameter_name="BumpTexture",
              texture=unreal.load_asset(BUMP_TEXTURE), sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)
    g.c(uvs, tex, "UVs")
    flat = g.x(unreal.MaterialExpressionConstant3Vector, (pos[0] - 300, pos[1] - 60), constant=unreal.LinearColor(0.0, 0.0, 1.0, 1.0))
    w = bump if mask is None else g.op(unreal.MaterialExpressionMultiply, bump, mask, (pos[0] - 300, pos[1] + 250))
    n = g.x(unreal.MaterialExpressionLinearInterpolate, pos)
    g.c(flat, n, "A")
    _connect(tex, "RGB", n, ("B",))
    g.c(w, n, "Alpha")
    g.prop(n, unreal.MaterialProperty.MP_NORMAL)
    return tex


def _glass_parent(textured):
    """The two glass materials (built once under /RammsWorldGen/Materials, versioned by GLASS_VERSION).

    Both take, per material instance: Tint, Roughness, Frost (0 clear .. 1 milky: rougher, brighter, denser),
    FrostRoughness / FrostBrightness, Bump (0 flat .. 1 rippled), BumpTexture / BumpTiling; and per placed
    instance (ISM custom data, GLASS_CUSTOM_DATA) a frost and bump offset, an opacity multiplier and a tint
    multiplier, so one asset can look different at each placement.

    M_RammsGlass, for panes: thin, two-sided, forward-shaded translucency with a near-black base (glass shows
    what is behind it and what it reflects) and Fresnel opacity (OpacityFront / OpacityEdge). What is behind is
    also read back from the scene colour (screen-space transmission, weight ScreenSpace): blurred by frost
    (BlurRadius, cm), shifted by the ripples (Refraction, cm) and coloured by the tint, so frosted glass blurs,
    patterned glass distorts and tinted glass colours the view; frost also dims it (FrostTransmit) and adds a
    lit milky layer (FrostBrightness).

    M_RammsGlassAtlas, for one texture holding glass and solid parts (a whole shelter, a car with its
    windows): masked, so frames, posters and bodywork stay solid and fully lit; the see-through texels are
    dithered at GlassCover (FrostCover when frosted) with glass roughness, tint and bump."""
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
    g = _G(mat)
    mat.set_editor_property("two_sided", True)
    mat.set_editor_property("used_with_instanced_static_meshes", True)   # PCG places them as instances
    g.prop(g.x(unreal.MaterialExpressionConstant, (-300, 450), r=0.0), MP.MP_METALLIC)
    g.prop(g.x(unreal.MaterialExpressionConstant, (-300, 350), r=0.5), MP.MP_SPECULAR)   # F0 0.04, as for glass
    tint, frost, bump, oscale, rough, frough, fbright = _glass_inputs(g, 0.0)
    gbright = g.scalar("GlassBrightness", 0.08, (-1900, 400))
    clear_col = g.op(unreal.MaterialExpressionMultiply, tint, gbright, (-1250, 380))
    frost_col = g.op(unreal.MaterialExpressionMultiply, tint, fbright, (-1250, 480))
    gcol = g.lerp(clear_col, frost_col, frost, (-1050, 420))
    grough = g.lerp(rough, frough, frost, (-1050, 560))
    if not textured:
        mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
        mat.set_editor_property("translucency_lighting_mode", unreal.TranslucencyLightingMode.TLM_SURFACE_PER_PIXEL_LIGHTING)
        for prop, val in (("use_hq_forward_reflections", True), ("forward_blends_sky_light_cubemaps", True)):
            try:
                mat.set_editor_property(prop, val)
            except Exception:  # noqa: BLE001
                pass
        g.prop(grough, MP.MP_ROUGHNESS)
        # Clear-glass opacity (Fresnel: denser at grazing angles) times the per-instance multiplier.
        fres = g.x(unreal.MaterialExpressionFresnel, (-1250, 700), exponent=5.0, base_reflect_fraction=0.0)
        o_clear = g.lerp(g.scalar("OpacityFront", 0.5, (-1450, 650)), g.scalar("OpacityEdge", 0.85, (-1450, 740)), fres, (-1050, 700))
        o_s = g.op(unreal.MaterialExpressionMin, g.op(unreal.MaterialExpressionMultiply, o_clear, oscale, (-900, 700)), 0.97, (-780, 700))
        # Screen-space transmission: what is behind the pane, read from the scene colour, blurred by frost,
        # shifted by the ripple normal (bump) and coloured by the tint, so frosted glass really blurs, patterned
        # glass really distorts and tinted glass really colours the view through it. Weight ScreenSpace (1 =
        # always; 0 = only for frosted or rippled glass); the rest of the light goes straight through as before.
        ntex = _glass_normal(g, bump, (-300, 1600))
        w = g.clamp01(g.op(unreal.MaterialExpressionAdd, g.op(unreal.MaterialExpressionAdd, frost, bump, (-900, 900)),
                           g.scalar("ScreenSpace", 1.0, (-1100, 980)), (-760, 920)), (-640, 920))
        opac = g.lerp(o_s, 1.0, w, (-450, 760))
        g.prop(opac, MP.MP_OPACITY)
        depth = g.op(unreal.MaterialExpressionMax, g.x(unreal.MaterialExpressionPixelDepth, (-1450, 1300)), 10.0, (-1300, 1300))
        # blur radius and ripple shift are sizes in cm on the pane, turned into screen fractions by depth
        # screen offsets are fractions of width and height: scale x by height / width so the blur is round
        vs = g.x(unreal.MaterialExpressionViewSize, (-1700, 1100))
        vsx = g.op(unreal.MaterialExpressionDotProduct, vs, g.x(unreal.MaterialExpressionConstant2Vector, (-1700, 1170), r=1.0, g=0.0), (-1550, 1100))
        vsy = g.op(unreal.MaterialExpressionDotProduct, vs, g.x(unreal.MaterialExpressionConstant2Vector, (-1700, 1240), r=0.0, g=1.0), (-1550, 1170))
        aspect = g.op(unreal.MaterialExpressionAppendVector, g.op(unreal.MaterialExpressionDivide, vsy, vsx, (-1400, 1130)),
                      g.x(unreal.MaterialExpressionConstant, (-1400, 1200), r=1.0), (-1250, 1150))
        rad1 = g.op(unreal.MaterialExpressionDivide, g.op(unreal.MaterialExpressionMultiply, frost, g.scalar("BlurRadius", 5.0, (-1450, 1200)), (-1300, 1200)), depth, (-1150, 1250))
        rad = g.op(unreal.MaterialExpressionMultiply, rad1, aspect, (-1000, 1250))
        nx = g.op(unreal.MaterialExpressionDotProduct, ntex, g.x(unreal.MaterialExpressionConstant3Vector, (-1450, 1450), constant=unreal.LinearColor(1, 0, 0, 1)), (-1300, 1420))
        ny = g.op(unreal.MaterialExpressionDotProduct, ntex, g.x(unreal.MaterialExpressionConstant3Vector, (-1450, 1530), constant=unreal.LinearColor(0, 1, 0, 1)), (-1300, 1500))
        nxy = g.op(unreal.MaterialExpressionAppendVector, nx, ny, (-1150, 1460))
        shift = g.op(unreal.MaterialExpressionMultiply, bump, g.scalar("Refraction", 3.0, (-1450, 1620)), (-1300, 1600))
        dist = g.op(unreal.MaterialExpressionMultiply, g.op(unreal.MaterialExpressionDivide, g.op(unreal.MaterialExpressionMultiply, nxy, shift, (-1150, 1560)), depth, (-1000, 1500)), aspect, (-900, 1500))
        # 25 taps on three staggered rings (a disc blur; few enough to stay cheap on glass pixels only)
        taps = [(0.0, 0.0)]
        for ring, n in ((0.34, 6), (0.67, 8), (1.0, 10)):
            for k in range(n):
                a0 = 2.0 * math.pi * (k + 0.5 * ring) / n
                taps.append((ring * math.cos(a0), ring * math.sin(a0)))
        acc = None
        for k, (dx, dy) in enumerate(taps):
            yy = 1800 + k * 60
            d2 = g.x(unreal.MaterialExpressionConstant2Vector, (-1000, yy), r=dx, g=dy)
            off = g.op(unreal.MaterialExpressionAdd, dist, g.op(unreal.MaterialExpressionMultiply, d2, rad, (-850, yy)), (-700, yy))
            sc = g.x(unreal.MaterialExpressionSceneColor, (-550, yy), input_mode=unreal.MaterialSceneAttributeInputMode.OFFSET_FRACTION)
            _connect(off, "", sc, ("",))         # its one input is unnamed from Python
            acc = sc if acc is None else g.op(unreal.MaterialExpressionAdd, acc, sc, (-400, yy))
        blur = g.op(unreal.MaterialExpressionMultiply, acc, 1.0 / len(taps), (-250, 1800))
        # frosted glass passes less light straight through
        ftrans = g.lerp(1.0, g.scalar("FrostTransmit", 0.8, (-450, 1950)), frost, (-250, 1950))
        tr = g.op(unreal.MaterialExpressionMultiply, g.op(unreal.MaterialExpressionMultiply, blur, tint, (-100, 1800)), ftrans, (50, 1850))
        # emissive x opacity = w (1 - o_s) x transmitted; the straight-through share (1 - opacity) is unchanged
        ew = g.op(unreal.MaterialExpressionMultiply, w, g.op(unreal.MaterialExpressionSubtract,
              g.x(unreal.MaterialExpressionConstant, (-450, 1100), r=1.0), o_s, (-300, 1100)), (-150, 1050))
        g.prop(g.op(unreal.MaterialExpressionMultiply, tr, g.op(unreal.MaterialExpressionDivide, ew, opac, (0, 1050)), (200, 1300)),
               MP.MP_EMISSIVE_COLOR)
        # the lit glass layer keeps its clear-glass strength (o_s), up to full strength when frosted (milky)
        lscale = g.lerp(g.op(unreal.MaterialExpressionDivide, o_s, opac, (-300, 400)), 1.0, frost, (-150, 400))
        g.prop(g.op(unreal.MaterialExpressionMultiply, gcol, lscale, (0, 400)), MP.MP_BASE_COLOR)
    else:
        mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
        mat.set_editor_property("opacity_mask_clip_value", 0.5)
        try:
            mat.set_editor_property("dithered_lod_transition", False)
        except Exception:  # noqa: BLE001
            pass
        white = unreal.load_asset("/Engine/EngineResources/WhiteSquareTexture")
        black = unreal.load_asset("/Engine/EngineResources/Black")
        tex = g.x(unreal.MaterialExpressionTextureSampleParameter2D, (-2300, -1000), parameter_name="BaseTexture", texture=white)
        a = g.op(unreal.MaterialExpressionMultiply, tex, g.scalar("AlphaScale", 1.0, (-2300, -800)), (-2000, -900), a_out="A")
        # solid weight: 0 for glass texels (alpha < ~0.85), 1 for opaque ones
        sw = g.clamp01(g.op(unreal.MaterialExpressionMultiply, g.op(unreal.MaterialExpressionSubtract, a, 0.85, (-1850, -900)), 6.67, (-1720, -900)), (-1600, -900))
        tcol = g.op(unreal.MaterialExpressionMultiply, tex, g.x(unreal.MaterialExpressionVectorParameter, (-2300, -700), parameter_name="SolidTint",
                                                                default_value=unreal.LinearColor(1.0, 1.0, 1.0, 1.0)), (-1600, -1050), a_out="RGB")
        g.prop(g.lerp(gcol, tcol, sw, (-450, -150)), MP.MP_BASE_COLOR)
        g.prop(g.lerp(grough, g.scalar("SolidRoughness", 0.5, (-900, 250)), sw, (-450, 250)), MP.MP_ROUGHNESS)
        # coverage: solid texels 1; glass texels dithered at GlassCover (FrostCover when frosted) x the
        # per-instance opacity multiplier
        cover = g.lerp(g.scalar("GlassCover", 0.5, (-1450, 650)), g.scalar("FrostCover", 0.85, (-1450, 740)), frost, (-1050, 700))
        cover = g.op(unreal.MaterialExpressionMin, g.op(unreal.MaterialExpressionMultiply, cover, oscale, (-900, 700)), 1.0, (-780, 700))
        cv = g.lerp(cover, 1.0, sw, (-650, 650))
        cut = g.op(unreal.MaterialExpressionSubtract, a, 0.02, (-650, 820))     # fully clear texels stay holes
        dith = g.x(unreal.MaterialExpressionMaterialFunctionCall, (-450, 650))
        fn = unreal.load_asset("/Engine/Functions/Engine_MaterialFunctions02/Utility/DitherTemporalAA")
        try:
            dith.set_material_function(fn)
        except Exception:  # noqa: BLE001
            dith.set_editor_property("material_function", fn)
        _connect(cv, "", dith, ("Alpha Threshold", "AlphaThreshold"))
        keep = g.x(unreal.MaterialExpressionIf, (-250, 700), const_b=0.0)
        g.c(cut, keep, "A")
        _connect(dith, "Result", keep, ("AGreaterThanB", "A > B"))
        _connect(g.x(unreal.MaterialExpressionConstant, (-450, 850), r=0.0), "", keep, ("ALessThanB", "A < B"))
        _connect(dith, "Result", keep, ("AEqualsB", "A == B"))
        g.prop(keep, MP.MP_OPACITY_MASK)
        etex = g.x(unreal.MaterialExpressionTextureSampleParameter2D, (-1100, 1000), parameter_name="EmissiveTexture", texture=black)
        g.prop(g.op(unreal.MaterialExpressionMultiply, etex, g.scalar("EmissiveStrength", 0.0, (-1100, 1200)), (-450, 1050), a_out="RGB"),
               MP.MP_EMISSIVE_COLOR)
        glass_w = g.op(unreal.MaterialExpressionSubtract, g.x(unreal.MaterialExpressionConstant, (-1450, 1350), r=1.0), sw, (-1300, 1350))
        _glass_normal(g, bump, (-300, 1400), mask=glass_w)
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
        for p in ("Tint", "SolidTint"):
            mel.set_material_instance_vector_parameter_value(g, p, unreal.LinearColor(col[0], col[1], col[2], 1.0))
        mel.set_material_instance_scalar_parameter_value(g, "AlphaScale", a)
        mel.set_material_instance_scalar_parameter_value(g, "SolidRoughness", max(0.3, float(sc.get("RoughnessFactor", 0.6))))
        if tx.get("EmissiveTexture") is not None:
            mel.set_material_instance_texture_parameter_value(g, "EmissiveTexture", tx["EmissiveTexture"])
            e = ve.get("EmissiveFactor")
            # glTF emissive is often a lightmap-like bake of the whole atlas: keep it faint
            mel.set_material_instance_scalar_parameter_value(g, "EmissiveStrength", 0.25 * (1.0 if e is None else max(e.r, e.g, e.b)))
    # Vehicle glass varies in tint and opacity only (no frosted or patterned windscreens).
    vehicle = "/Car/" in mi.get_path_name() or "/Bus/" in mi.get_path_name() or "/Truck/" in mi.get_path_name()
    mel.set_material_instance_scalar_parameter_value(g, "InstanceFrost", 0.0 if vehicle else 1.0)
    mel.set_material_instance_scalar_parameter_value(g, "InstanceBump", 0.0 if vehicle else 1.0)
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
