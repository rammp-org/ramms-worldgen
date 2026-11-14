"""World-aligned, anti-tiling ground materials built from pack textures.

Generated roads, sidewalks and lawns are boxes and slabs of arbitrary size, so a
material that relies on mesh UVs either stretches (scaled cubes) or repeats in
an obvious grid. These materials take their UVs from the world XY position
instead, at a real-world tile size, and break the repetition up:

* the texture set is sampled twice, at the tile size and at a rotated-ish 0.537x
  scale with an offset, and blended by a low-frequency mask,
* a second, much larger mask modulates brightness (macro variation),
* an optional second texture set (e.g. a damaged / patchy variant) is blended in
  by the same mask instead of the second sample of the first set.

Built once per surface by `build_surfaces()`; the palette builder points the
Mat.* slots at them. Missing textures just skip that surface.
"""

import unreal

from . import _helpers as H

DIR = "/RammsWorldGen/Materials"
MEL = unreal.MaterialEditingLibrary

# name -> spec. bc/n/rma: texture paths; layout: how the packed texture holds roughness / AO -
# "rma" (R rough, B ao), "mrao" (G rough, B ao), "orm" (R ao, G rough), "mask" (HDRP mask map: G ao,
# A smoothness); bc2/n2/rma2: optional variant set; tile: tile size in cm; second: blend a second,
# rescaled sample (off for regular patterns: tiles, planks, bricks, wallpaper); mask: low-frequency mask
# texture; macro: brightness range of the large-scale variation; tint: base colour multiplier; tint2: a
# second tint the macro mask blends toward (colour patches on lawns); tint_param: expose the tint as a
# "Tint" vector parameter so material instances can recolour it (paints).
_MASK = "/Game/CityBeachStrip/Textures/Masks/T_Dirt_Mask"
_SNH = "/Game/SuburbNeighborhoodHousePack/Textures/"
_CBS = "/Game/CityBeachStrip/Textures/"
_RSF = "/Game/RealCitySF/Textures/Remastered/Bldg_GP_Textures/"
SURFACES = {
    "M_RammsAsphalt": dict(
        bc="/Game/StripMall/Textures/Tileable/Roads/T_RoadAsphalt01_BC", n="/Game/StripMall/Textures/Tileable/Roads/T_RoadAsphalt01_N",
        rma="/Game/StripMall/Textures/Tileable/Roads/T_RoadAsphalt01_RMA",
        bc2="/Game/StripMall/Textures/Tileable/Roads/T_RoadAsphalt02_BC", n2="/Game/StripMall/Textures/Tileable/Roads/T_RoadAsphalt02_N",
        rma2="/Game/StripMall/Textures/Tileable/Roads/T_RoadAsphalt02_RMA", tile=450.0, mask=_MASK, tint=(0.95, 0.95, 0.97)),
    "M_RammsSidewalk": dict(
        bc="/Game/StripMall/Textures/Tileable/Sidewalk/T_Sidewalk01_BC", n="/Game/StripMall/Textures/Tileable/Sidewalk/T_Sidewalk01_N",
        rma="/Game/StripMall/Textures/Tileable/Sidewalk/T_Sidewalk01_RMA", tile=300.0, mask=_MASK, second=False),
    "M_RammsConcrete": dict(
        bc="/Game/TownShops/Textures/Tileable/Concrete/T_ConcreteBase_BC", n="/Game/TownShops/Textures/Tileable/Concrete/T_ConcreteBase_N",
        rma="/Game/TownShops/Textures/Tileable/Concrete/T_ConcreteBase_RMA", tile=400.0, mask=_MASK),
    "M_RammsGrass": dict(
        bc="/Game/CityBeachStrip/Textures/Landscape/T_GrassGround_BC", n="/Game/CityBeachStrip/Textures/Landscape/T_GrassGround_N",
        rma="/Game/CityBeachStrip/Textures/Landscape/T_GrassGround_RMA",
        bc2="/Game/CityBeachStrip/Textures/Landscape/T_GrassGround_02_BC", n2="/Game/CityBeachStrip/Textures/Landscape/T_GrassGround_02_N",
        rma2="/Game/CityBeachStrip/Textures/Landscape/T_GrassGround_02_RMA", tile=380.0, mask=_MASK, macro=(0.78, 1.12), tint=(0.62, 0.80, 0.42), tint2=(0.78, 0.80, 0.46)),
    "M_RammsLawn": dict(
        bc="/Game/ResidentialHouses/Textures/Foliage/Grass/T_GrassGround_BC", n="/Game/ResidentialHouses/Textures/Foliage/Grass/T_GrassGround_N",
        rma="/Game/ResidentialHouses/Textures/Foliage/Grass/T_GrassGround_RMA",
        bc2="/Game/CityBeachStrip/Textures/Landscape/T_GrassGround_02_BC", n2="/Game/CityBeachStrip/Textures/Landscape/T_GrassGround_02_N",
        rma2="/Game/CityBeachStrip/Textures/Landscape/T_GrassGround_02_RMA", tile=320.0, mask=_MASK, macro=(0.82, 1.1), tint=(0.55, 0.80, 0.38), tint2=(0.74, 0.78, 0.44)),
    "M_RammsPath": dict(
        bc="/Game/ResidentialHouses/Textures/Tileable/Paths/T_Path_Concrete_BC", n="/Game/ResidentialHouses/Textures/Tileable/Paths/T_Path_Concrete_N",
        rma="/Game/ResidentialHouses/Textures/Tileable/Paths/T_Path_Concrete_RMA", tile=220.0, mask=_MASK, second=False),
    "M_RammsDirt": dict(
        bc="/Game/TownShops/Textures/Tileable/Dirt/T_Dirt_BC", n="/Game/TownShops/Textures/Tileable/Dirt/T_Dirt_N",
        rma="/Game/TownShops/Textures/Tileable/Dirt/T_Dirt_RMA", tile=300.0, mask=_MASK),
    # curbs: a finer concrete so the 15 cm face reads as cast concrete, slightly darker than the walk
    # park ground patches
    "M_RammsGravel": dict(
        bc=_CBS + "Tileable/Soil/T_Stones01_BC", n=_CBS + "Tileable/Soil/T_Stones01_N", rma=_CBS + "Tileable/Soil/T_Stones01_RMA",
        tile=160.0, mask=_MASK, macro=(0.85, 1.1), tint=(0.92, 0.9, 0.86)),
    "M_RammsMud": dict(
        bc=_CBS + "Tileable/Soil/T_Soil01_BC", n=_CBS + "Tileable/Soil/T_Soil01_N", rma=_CBS + "Tileable/Soil/T_Soil01_RMA",
        tile=220.0, mask=_MASK, macro=(0.7, 0.95), tint=(0.55, 0.47, 0.38)),
    "M_RammsCurb": dict(
        bc="/Game/RealCitySF/Textures/Remastered/T_Sidewalk_conc_tan_tile_01_BaseColor", n="/Game/RealCitySF/Textures/Remastered/T_Sidewalk_conc_tan_tile_01_Normal",
        rma="/Game/RealCitySF/Textures/Remastered/T_Sidewalk_conc_tan_tile_01_MRAO", layout="mrao", tile=140.0, mask=_MASK, second=False, tint=(0.82, 0.82, 0.82)),
    # stand-in building shells (when no house / facade kit fits): world-aligned so nothing stretches
    "M_RammsBrick": dict(
        bc="/Game/StripMall/Textures/Tileable/Brick/T_BrickWall03_BC", n="/Game/StripMall/Textures/Tileable/Brick/T_BrickWall03_N",
        rma="/Game/StripMall/Textures/Tileable/Brick/T_BrickWall03_RMA", tile=220.0, mask=_MASK, second=False, macro=(0.9, 1.05)),
    "M_RammsSiding": dict(
        bc=_SNH + "T_Wall_HouseWoodPlank_White_BaseColor", n=_SNH + "T_Wall_HouseWoodPlank_Normal",
        rma=_SNH + "T_Wall_HouseWoodPlank_White_MaskMap", layout="mask", tile=200.0, mask=_MASK, second=False, macro=(0.92, 1.03)),
    # interior walls
    "M_RammsPaint": dict(
        bc=_RSF + "T_int_wall_drywall_tile_01_BaseColor", n=_RSF + "T_int_wall_drywall_tile_01_Normal",
        rma=_RSF + "T_int_wall_drywall_tile_01_MRAO", layout="mrao", tile=200.0, second=False, tint=(0.92, 0.91, 0.88), tint_param=True),
    "M_RammsWallpaper": dict(
        bc="/Game/ResidentialHouses/Textures/Walls/Inside/T_WallPaper_BC", n="/Game/ResidentialHouses/Textures/Walls/Inside/T_WallPaper_N",
        rma="/Game/ResidentialHouses/Textures/Walls/Inside/T_WallPaper_RMA", tile=90.0, second=False),
    "M_RammsWallpaperDiamond": dict(
        bc=_SNH + "T_In_WallPaper_Diamond_BaseColor", n=_SNH + "T_In_WallPaper_Diamond_Normal",
        rma=_SNH + "T_In_WallPaper_Diamond_MaskMap", layout="mask", tile=100.0, second=False),
    "M_RammsWallpaperFlowers": dict(
        bc=_SNH + "T_In_WallPaper_Flowers_BaseColor", n=_SNH + "T_In_WallPaper_Normal",
        rma=_SNH + "T_In_WallPaper_MaskMap", layout="mask", tile=100.0, second=False),
    "M_RammsWallpaperRed": dict(
        bc=_SNH + "T_In_WallPaper_Red_BaseColor", n=_SNH + "T_In_WallPaper_Normal",
        rma=_SNH + "T_In_WallPaper_Red_MaskMap", layout="mask", tile=100.0, second=False),
    "M_RammsWallTile": dict(
        bc="/Game/ResidentialHouses/Textures/Walls/Inside/T_WallTile_BC", n="/Game/ResidentialHouses/Textures/Walls/Inside/T_WallTile_N",
        rma="/Game/ResidentialHouses/Textures/Walls/Inside/T_WallTile_RMA", tile=120.0, second=False),
    "M_RammsWallTile2": dict(
        bc=_CBS + "Tileable/Tiles/T_CeramicTiles_01_BC", n=_CBS + "Tileable/Tiles/T_CeramicTiles_01_N",
        rma=_CBS + "Tileable/Tiles/T_CeramicTiles_01_RMA", tile=120.0, second=False),
    # floors
    "M_RammsFloorWood1": dict(
        bc=_CBS + "Architecture/Interiors/Floor/T_WoodFloor01_BC", n=_CBS + "Architecture/Interiors/Floor/T_WoodFloor01_N",
        rma=_CBS + "Architecture/Interiors/Floor/T_WoodFloor01_RMA", tile=250.0, second=False, macro=(0.92, 1.05), mask=_MASK),
    "M_RammsFloorWood2": dict(
        bc=_CBS + "Architecture/Interiors/Floor/T_WoodFloor02_BC", n=_CBS + "Architecture/Interiors/Floor/T_WoodFloor02_N",
        rma=_CBS + "Architecture/Interiors/Floor/T_WoodFloor02_RMA", tile=250.0, second=False, macro=(0.92, 1.05), mask=_MASK),
    "M_RammsFloorParquet": dict(
        bc="/Game/ResidentialHouses/Textures/Tileable/Wood/T_ParquetFloor_BC", n="/Game/ResidentialHouses/Textures/Tileable/Wood/T_ParquetFloor_N",
        rma="/Game/ResidentialHouses/Textures/Tileable/Wood/T_ParquetFloor_RMA", tile=200.0, second=False),
    "M_RammsFloorHardwood": dict(
        bc="/Game/HousePropsFurniture/Textures/Textures_building/T_Floor_hardwood_BaseColor", n="/Game/HousePropsFurniture/Textures/Textures_building/T_Floor_hardwood_Normal",
        rma="/Game/HousePropsFurniture/Textures/Textures_building/T_Floor_hardwood_MaskMap", layout="mask", tile=250.0, second=False),
    "M_RammsFloorCarpet1": dict(
        bc=_CBS + "Architecture/Interiors/Floor/T_Carpet01_BC", n=_CBS + "Architecture/Interiors/Floor/T_Carpet01_N",
        rma=_CBS + "Architecture/Interiors/Floor/T_Carpet01_RMA", tile=200.0, mask=_MASK, macro=(0.9, 1.05)),
    "M_RammsFloorCarpet2": dict(
        bc=_CBS + "Architecture/Interiors/Floor/T_Carpet02_BC", n=_CBS + "Architecture/Interiors/Floor/T_Carpet02_N",
        rma=_CBS + "Architecture/Interiors/Floor/T_Carpet02_RMA", tile=200.0, mask=_MASK, macro=(0.9, 1.05)),
    "M_RammsFloorCarpetOffice": dict(
        bc="/Game/Office/Textures/TX_Carpet_BaseColor", n="/Game/Office/Textures/TX_Carpet_Normal",
        rma="/Game/Office/Textures/TX_Carpet_OcclusionRoughnessMetallic", layout="orm", tile=200.0, mask=_MASK, macro=(0.9, 1.05)),
    "M_RammsFloorCarpetTiles": dict(
        bc=_RSF + "T_int_floor_carpet_tiles_01_BaseColor", n=_RSF + "T_int_floor_carpet_tiles_01_Normal",
        rma=_RSF + "T_int_floor_carpet_tiles_01_MRAO", layout="mrao", tile=200.0, second=False),
    "M_RammsFloorBath": dict(
        bc=_CBS + "Architecture/Interiors/Floor/T_BathroomFloor01_BC", n=_CBS + "Architecture/Interiors/Floor/T_BathroomFloor01_N",
        rma=_CBS + "Architecture/Interiors/Floor/T_BathroomFloor01_RMA", tile=150.0, second=False),
    "M_RammsFloorCeramic": dict(
        bc=_CBS + "Tileable/Tiles/T_CeramicTiles_02_BC", n=_CBS + "Tileable/Tiles/T_CeramicTiles_02_N",
        rma=_CBS + "Tileable/Tiles/T_CeramicTiles_02_RMA", tile=150.0, second=False),
    "M_RammsFloorTiles": dict(
        bc=_RSF + "T_int_floor_tiles_01_BaseColor", n=_RSF + "T_int_floor_tiles_01_Normal",
        rma=_RSF + "T_int_floor_tiles_01_MRAO", layout="mrao", tile=200.0, second=False),
    "M_RammsFloorMarble": dict(
        bc=_CBS + "Tileable/Marble/T_MarbleTiles_BC", n=_CBS + "Tileable/Marble/T_MarbleTiles_N",
        rma=_CBS + "Tileable/Marble/T_MarbleTiles_RMA", tile=200.0, second=False),
    "M_RammsFloorPVC": dict(
        bc=_CBS + "Architecture/Interiors/Floor/T_PVC_Floor01_BC", n=_CBS + "Architecture/Interiors/Floor/T_PVC_Floor01_N",
        rma=_CBS + "Architecture/Interiors/Floor/T_PVC_Floor01_RMA", tile=250.0, second=False),
    "M_RammsFloorPVC2": dict(
        bc="/Game/TownShops/Textures/Tileable/Flooring/T_PVC_Flooring_01_BC", n="/Game/TownShops/Textures/Tileable/Flooring/T_PVC_Flooring_01_N",
        rma="/Game/TownShops/Textures/Tileable/Flooring/T_PVC_Flooring_01_RMA", tile=250.0, second=False),
    "M_RammsFloorShop": dict(
        bc=_CBS + "Architecture/Interiors/Floor/T_Floor01_BC", n=_CBS + "Architecture/Interiors/Floor/T_Floor01_N",
        rma=_CBS + "Architecture/Interiors/Floor/T_Floor01_RMA", tile=250.0, second=False),
    "M_RammsFloorConcrete": dict(
        bc=_RSF + "T_int_floor_concrete_glossy_tiled_01_BaseColor", n=_RSF + "T_int_floor_concrete_glossy_tiled_01_Normal",
        rma=_RSF + "T_int_floor_concrete_glossy_tiled_01_MRAO", layout="mrao", tile=300.0, mask=_MASK),
}

# Paint colours: material instances of M_RammsPaint with the Tint parameter set.
PAINTS = {
    "Cream": (0.93, 0.85, 0.68), "Sage": (0.56, 0.68, 0.50), "SkyBlue": (0.52, 0.68, 0.84), "Grey": (0.62, 0.64, 0.66),
    "Beige": (0.84, 0.72, 0.56), "Terracotta": (0.78, 0.47, 0.34), "Mint": (0.60, 0.82, 0.70), "Lavender": (0.70, 0.64, 0.82),
    "Butter": (0.95, 0.84, 0.50), "Slate": (0.40, 0.48, 0.56), "Blush": (0.90, 0.68, 0.66), "White": (0.92, 0.91, 0.88),
    "Olive": (0.62, 0.62, 0.42), "Teal": (0.36, 0.60, 0.62),
}


def _clear(mat):
    """Remove every expression of a material being rebuilt. MaterialEditingLibrary.delete_all_material_expressions
    removes only every other one (it deletes while iterating), so it is repeated until none is left; otherwise
    each rebuild would leave the old graph behind, unconnected."""
    for _ in range(32):
        if MEL.get_num_material_expressions(mat) == 0:
            return
        MEL.delete_all_material_expressions(mat)


def _instanced_usage(mat):
    """Usage flags for a material the PCG spawners put on instanced (and often Nanite) pack meshes: set when it is
    built, so a cooked build has those shader permutations. The editor adds a missing flag only when it first
    renders the combination, and leaves the material dirty rather than saving it."""
    for prop in ("used_with_instanced_static_meshes", "used_with_nanite"):
        try:
            mat.set_editor_property(prop, True)
        except Exception as exc:  # noqa: BLE001
            H.warn("%s: could not set %s: %s" % (mat.get_name(), prop, exc))


def _load(path):
    if not path:
        return None
    return H.EAL.load_asset(path) if H.EAL.does_asset_exist(path) else None


def _sampler_type(tex):
    cs = tex.get_editor_property("compression_settings")
    srgb = tex.get_editor_property("srgb")
    TC, ST = unreal.TextureCompressionSettings, unreal.MaterialSamplerType
    if cs == TC.TC_NORMALMAP:
        return ST.SAMPLERTYPE_NORMAL
    if cs == TC.TC_GRAYSCALE:
        return ST.SAMPLERTYPE_GRAYSCALE if srgb else ST.SAMPLERTYPE_LINEAR_GRAYSCALE
    if cs == TC.TC_ALPHA:
        return ST.SAMPLERTYPE_ALPHA
    if cs == TC.TC_MASKS:
        return ST.SAMPLERTYPE_MASKS
    if not srgb:
        return ST.SAMPLERTYPE_LINEAR_COLOR
    return ST.SAMPLERTYPE_COLOR


class _G:
    """Tiny graph helper: places nodes in columns and connects by name."""

    def __init__(self, mat):
        self.m = mat
        self.y = 0

    def node(self, cls, x, **props):
        e = MEL.create_material_expression(self.m, cls, x, self.y)
        self.y += 140
        for k, v in props.items():
            e.set_editor_property(k, v)
        return e

    def tex(self, x, tex, uv):
        e = self.node(unreal.MaterialExpressionTextureSample, x, texture=tex, sampler_type=_sampler_type(tex))
        MEL.connect_material_expressions(uv, "", e, "UVs")
        return e

    def lerp(self, x, a, a_out, b, b_out, alpha, alpha_out="R"):
        e = self.node(unreal.MaterialExpressionLinearInterpolate, x)
        MEL.connect_material_expressions(a, a_out, e, "A")
        MEL.connect_material_expressions(b, b_out, e, "B")
        MEL.connect_material_expressions(alpha, alpha_out, e, "Alpha")
        return e


def _world_uv(g):
    """World-space UVs by the dominant normal axis: XY on floors and tops, YZ / XZ on walls and curb faces
    (V = -Z so textures stay upright). Selection is a hard switch built from lerps so no If node is needed."""
    wp = g.node(unreal.MaterialExpressionWorldPosition, -2600)
    nrm = g.node(unreal.MaterialExpressionVertexNormalWS, -2600)
    an = g.node(unreal.MaterialExpressionAbs, -2450)
    MEL.connect_material_expressions(nrm, "", an, "")
    def mask(src, r, gg, b):
        e = g.node(unreal.MaterialExpressionComponentMask, -2300, r=r, g=gg, b=b, a=False)
        MEL.connect_material_expressions(src, "", e, "")
        return e
    nx, ny, nz = mask(an, True, False, False), mask(an, False, True, False), mask(an, False, False, True)
    wx, wy, wz = mask(wp, True, False, False), mask(wp, False, True, False), mask(wp, False, False, True)
    xy = mask(wp, True, True, False)
    negz = g.node(unreal.MaterialExpressionMultiply, -2150, const_b=-1.0)
    MEL.connect_material_expressions(wz, "", negz, "A")
    def append(a_, b_):
        e = g.node(unreal.MaterialExpressionAppendVector, -2000)
        MEL.connect_material_expressions(a_, "", e, "A")
        MEL.connect_material_expressions(b_, "", e, "B")
        return e
    xz, yz = append(wx, negz), append(wy, negz)
    def step(a_, b_=None, const=None):
        sub = g.node(unreal.MaterialExpressionSubtract, -2000, **({"const_b": const} if const is not None else {}))
        MEL.connect_material_expressions(a_, "", sub, "A")
        if b_ is not None:
            MEL.connect_material_expressions(b_, "", sub, "B")
        mul = g.node(unreal.MaterialExpressionMultiply, -1900, const_b=1000.0)
        MEL.connect_material_expressions(sub, "", mul, "A")
        sat = g.node(unreal.MaterialExpressionSaturate, -1800)
        MEL.connect_material_expressions(mul, "", sat, "")
        return sat
    side = g.lerp(-1700, xz, "", yz, "", step(nx, ny), "")
    return g.lerp(-1650, side, "", xy, "", step(nz, const=0.5), "")


# packed texture layouts: (roughness channel, ao channel, roughness is smoothness)
_LAYOUTS = {"rma": ("R", "B", False), "mrao": ("G", "B", False), "orm": ("G", "R", False), "mask": ("A", "G", True)}


def _build(name, spec, directory=None):
    """Create or refresh the world-aligned material `name` from spec (see SURFACES) in directory (DIR by
    default: a recipe builds its own surfaces into its own folder). A spec of kind "signal" is a traffic signal's
    lit lenses instead (build_signal)."""
    if spec.get("kind") == "signal":
        return build_signal(name, spec, directory)
    if spec.get("kind") == "paint":
        return build_paint(name, spec, directory)
    directory = directory or DIR
    bc, n, rma, mask = _load(spec["bc"]), _load(spec.get("n")), _load(spec.get("rma")), _load(spec.get("mask"))
    if bc is None:
        H.warn("surface %s: base colour %s missing" % (name, spec["bc"]))
        return None
    bc2, n2, rma2 = _load(spec.get("bc2")), _load(spec.get("n2")), _load(spec.get("rma2"))
    second = spec.get("second", True) or bc2 is not None
    path = directory + "/" + name
    H.ensure_dir(directory)
    if H.EAL.does_asset_exist(path):
        mat = H.EAL.load_asset(path)
        _clear(mat)
    else:
        mat = H.asset_tools().create_asset(name, directory, unreal.Material, unreal.MaterialFactoryNew())
    g = _G(mat)
    tile = float(spec.get("tile", 300.0))

    xy = _world_uv(g)
    uv1 = g.node(unreal.MaterialExpressionDivide, -1300, const_b=tile)
    MEL.connect_material_expressions(xy, "", uv1, "A")
    uvs = g.node(unreal.MaterialExpressionMultiply, -1150, const_b=0.537)
    MEL.connect_material_expressions(uv1, "", uvs, "A")
    uv2 = g.node(unreal.MaterialExpressionAdd, -1000, const_b=0.371)
    MEL.connect_material_expressions(uvs, "", uv2, "A")
    blend = None
    macro = None
    if mask is not None:
        uvm = g.node(unreal.MaterialExpressionDivide, -1300, const_b=tile * 7.3)
        MEL.connect_material_expressions(xy, "", uvm, "A")
        blend = g.tex(-1000, mask, uvm)
        uvM = g.node(unreal.MaterialExpressionDivide, -1300, const_b=tile * 29.0)
        MEL.connect_material_expressions(xy, "", uvM, "A")
        macro = g.tex(-1000, mask, uvM)

    def pair(t1, t2, out_all):
        a = g.tex(-700, t1, uv1)
        if blend is None or not second:
            return a, out_all
        b = g.tex(-700, t2 or t1, uv2)
        return g.lerp(-450, a, out_all, b, out_all, blend), ""

    col, col_out = pair(bc, bc2, "RGB")
    if macro is not None:
        lo, hi = spec.get("macro", (0.85, 1.1))
        bright = g.node(unreal.MaterialExpressionLinearInterpolate, -450, const_a=lo, const_b=hi)
        MEL.connect_material_expressions(macro, "R", bright, "Alpha")
        mul = g.node(unreal.MaterialExpressionMultiply, -250)
        MEL.connect_material_expressions(col, col_out, mul, "A")
        MEL.connect_material_expressions(bright, "", mul, "B")
        col, col_out = mul, ""
    tint = spec.get("tint")
    if tint:
        if spec.get("tint_param"):
            tn = g.node(unreal.MaterialExpressionVectorParameter, -250, parameter_name="Tint", default_value=unreal.LinearColor(tint[0], tint[1], tint[2], 1.0))
            tn_out = "RGB"
        else:
            tn = g.node(unreal.MaterialExpressionConstant3Vector, -250, constant=unreal.LinearColor(tint[0], tint[1], tint[2], 1.0))
            tn_out = ""
        t2 = spec.get("tint2")
        if t2 and macro is not None:
            # Colour patches: the large-scale mask drifts the tint toward a second hue (dry / lush grass).
            tn2 = g.node(unreal.MaterialExpressionConstant3Vector, -250, constant=unreal.LinearColor(t2[0], t2[1], t2[2], 1.0))
            sharp = g.node(unreal.MaterialExpressionPower, -350, const_exponent=2.0)
            MEL.connect_material_expressions(macro, "G", sharp, "Base")
            tn = g.lerp(-200, tn, tn_out, tn2, "", sharp, "")
            tn_out = ""
        mul = g.node(unreal.MaterialExpressionMultiply, -100)
        MEL.connect_material_expressions(col, col_out, mul, "A")
        MEL.connect_material_expressions(tn, tn_out, mul, "B")
        col, col_out = mul, ""
    MEL.connect_material_property(col, col_out, unreal.MaterialProperty.MP_BASE_COLOR)
    if n is not None:
        nn, nn_out = pair(n, n2 if bc2 is not None else None, "RGB")
        MEL.connect_material_property(nn, nn_out, unreal.MaterialProperty.MP_NORMAL)
    if rma is not None:
        rc, ac, smooth = _LAYOUTS.get(spec.get("layout", "rma"), _LAYOUTS["rma"])
        rr, rr_out = pair(rma, rma2 if bc2 is not None else None, "RGBA" if smooth else "RGB")
        rough = g.node(unreal.MaterialExpressionComponentMask, -100, r=rc == "R", g=rc == "G", b=rc == "B", a=rc == "A")
        MEL.connect_material_expressions(rr, rr_out, rough, "")
        if smooth:
            inv = g.node(unreal.MaterialExpressionOneMinus, -50)
            MEL.connect_material_expressions(rough, "", inv, "")
            rough = inv
        MEL.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
        ao = g.node(unreal.MaterialExpressionComponentMask, -100, r=ac == "R", g=ac == "G", b=ac == "B", a=False)
        MEL.connect_material_expressions(rr, rr_out, ao, "")
        MEL.connect_material_property(ao, "", unreal.MaterialProperty.MP_AMBIENT_OCCLUSION)
    else:
        r = g.node(unreal.MaterialExpressionConstant, -100, r=0.85)
        MEL.connect_material_property(r, "", unreal.MaterialProperty.MP_ROUGHNESS)
    MEL.layout_material_expressions(mat)
    MEL.recompile_material(mat)
    H.save(mat)
    return mat


def build_paints():
    """Material instances of M_RammsPaint, one per PAINTS colour. Returns their paths."""
    parent_path = DIR + "/M_RammsPaint"
    if not H.EAL.does_asset_exist(parent_path):
        return []
    parent = H.EAL.load_asset(parent_path)
    out = []
    for cname, rgb in PAINTS.items():
        name = "MI_RammsPaint_" + cname
        path = DIR + "/" + name
        if H.EAL.does_asset_exist(path):
            mi = H.EAL.load_asset(path)
        else:
            mi = H.asset_tools().create_asset(name, DIR, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
        if mi is None:
            continue
        MEL.set_material_instance_parent(mi, parent)
        MEL.set_material_instance_vector_parameter_value(mi, "Tint", unreal.LinearColor(rgb[0], rgb[1], rgb[2], 1.0))
        H.save(mi)
        out.append(mi.get_path_name())
    return out


def build_signal(name, spec, directory=None):
    """A traffic signal's material with its lamps lit, over the mesh's own UV maps (bc, n, orm: occlusion,
    roughness, metallic). The lamps are found from geometry, not a texture (Downtown West's lens mask does not
    line up with its mesh): spec "lamps" {"red": [(y, z, radius)], "green": [...]} are lens discs in mesh units on
    heads facing +X, lit forward of "lamp_front_x" ("amber" lenses too, when the spec has them). An optional
    pedestrian head ("ped": its lens panel's plane "plane_y", "centre" (x, z), icon "size" (w, h) and the icon's
    "icon_uv" box (u0, v0, u1, v1) in the `lens` texture, "hand" / "walk" channels) shows the icon projected onto
    the panel.

    Every signal in a level shows one phase, by the axis its heads face: the instance's +X (where the palette turns
    them) runs nearer world X or nearer world Y -- so the cross directions of an intersection never disagree, at
    any instance scale. A pole's pedestrian head faces across the road its heads control and serves the people
    crossing it. Built into /Game (a recipe's folder), the lamps read the traffic signal clock's material
    parameter collection (traffic.SIGNAL_MPC: GreenX / AmberX / RedX for heads along X, ...Y for the others, and
    WalkX / HandX, WalkY / HandY for the pedestrian heads on those poles); its resting values -- and, built
    elsewhere (no collection), the scalar GreenAlongX (1: heads along X green) -- give the old fixed look: one axis
    green, the other red, the pedestrian heads the opposite. LampBrightness and the RedLamp / AmberLamp / GreenLamp
    / PedHand / PedWalk colours tune it. Returns the material."""
    directory = directory or DIR
    bc, n, orm, lens = _load(spec["bc"]), _load(spec.get("n")), _load(spec.get("orm")), _load(spec.get("lens"))
    if bc is None:
        H.warn("signal %s: base colour %s missing" % (name, spec["bc"]))
        return None
    path = directory + "/" + name
    H.ensure_dir(directory)
    if H.EAL.does_asset_exist(path):
        mat = H.EAL.load_asset(path)
        _clear(mat)
    else:
        mat = H.asset_tools().create_asset(name, directory, unreal.Material, unreal.MaterialFactoryNew())
    _instanced_usage(mat)
    g = _G(mat)
    P = unreal.MaterialProperty

    def link(a, outs, b, ins):
        # pin names differ between expressions and engine versions: the first pair that connects
        for o in outs:
            for i in ins:
                if MEL.connect_material_expressions(a, o, b, i):
                    return
        raise RuntimeError("signal %s: could not connect %s %s -> %s %s" % (name, a.get_name(), outs, b.get_name(), ins))

    def op(cls, x, a, b=None, **props):
        e = g.node(cls, x, **props)
        link(a, [""], e, ["A", ""])
        if b is not None:
            link(b, [""], e, ["B"])
        return e

    def rsub(x, const, e):
        # const - e (a connected A would override const_a)
        r_ = g.node(unreal.MaterialExpressionSubtract, x, const_a=float(const))
        link(e, [""], r_, ["B"])
        return r_

    def comp(src, x, ch):
        e = g.node(unreal.MaterialExpressionComponentMask, x, r=ch == "R", g=ch == "G", b=ch == "B", a=False)
        link(src, [""], e, [""])
        return e

    uv = g.node(unreal.MaterialExpressionTextureCoordinate, -1600)
    col = g.tex(-1200, bc, uv)
    nn = g.tex(-1200, n, uv) if n is not None else None
    o = g.tex(-1200, orm, uv) if orm is not None else None

    # Per vertex (instance data, the mesh's own coordinates), passed to the pixels: the instance's +X in world
    # space (which way its heads face) and the mesh-space position (where the lenses are).
    g.y = 1400
    ax = g.node(unreal.MaterialExpressionConstant3Vector, -2000, constant=unreal.LinearColor(1.0, 0.0, 0.0, 0.0))
    tr = g.node(unreal.MaterialExpressionTransform, -1800, transform_source_type=unreal.MaterialVectorCoordTransformSource.TRANSFORMSOURCE_LOCAL,
                transform_type=unreal.MaterialVectorCoordTransform.TRANSFORM_WORLD)
    link(ax, [""], tr, ["", "Input"])
    vi = g.node(unreal.MaterialExpressionVertexInterpolator, -1600)
    link(tr, [""], vi, ["VS", ""])
    lp = g.node(unreal.MaterialExpressionPreSkinnedPosition, -1800)
    vl = g.node(unreal.MaterialExpressionVertexInterpolator, -1600)
    link(lp, [""], vl, ["VS", ""])
    pos = g.node(unreal.MaterialExpressionComponentMask, -1400, r=True, g=True, b=True, a=False)
    link(vl, ["PS", ""], pos, [""])
    wx = g.node(unreal.MaterialExpressionComponentMask, -1400, r=True, g=False, b=False, a=False)
    link(vi, ["PS", ""], wx, [""])
    wy = g.node(unreal.MaterialExpressionComponentMask, -1400, r=False, g=True, b=False, a=False)
    link(vi, ["PS", ""], wy, [""])
    # along X when |x| > |y|: the transformed axis carries the instance's scale, so it is compared with its own other
    # component, not with a fixed length -- any scale, and perpendicular heads never agree (the switch is at 45 deg)
    absx = op(unreal.MaterialExpressionAbs, -1250, wx)
    absy = op(unreal.MaterialExpressionAbs, -1250, wy)
    along_x = op(unreal.MaterialExpressionSaturate, -950, op(unreal.MaterialExpressionMultiply, -1100, op(unreal.MaterialExpressionSubtract, -1175, absx, absy), const_b=1000.0))
    # The lamps' state per head axis: from the signal clock's collection (a /Game material: the level's own content),
    # else one fixed phase (GreenAlongX).
    mpc = None
    if directory.startswith("/Game/"):
        from . import traffic as TR
        mpc = TR.signal_collection()
    if mpc is not None:
        def per_axis(stem):
            on_x = g.node(unreal.MaterialExpressionCollectionParameter, -1100, collection=mpc, parameter_name=stem + "X")
            on_y = g.node(unreal.MaterialExpressionCollectionParameter, -1100, collection=mpc, parameter_name=stem + "Y")
            return g.lerp(-800, on_y, "", on_x, "", along_x, "")
        green, amber, red = per_axis("Green"), per_axis("Amber"), per_axis("Red")
        walk, hand = per_axis("Walk"), per_axis("Hand")
    else:
        phase = g.node(unreal.MaterialExpressionScalarParameter, -1100, parameter_name="GreenAlongX", default_value=1.0)
        not_phase = op(unreal.MaterialExpressionOneMinus, -950, phase)
        green = g.lerp(-800, not_phase, "", phase, "", along_x, "")
        red = op(unreal.MaterialExpressionOneMinus, -650, green)
        amber = g.node(unreal.MaterialExpressionConstant, -650, r=0.0)
        # a pole's pedestrian head: the hand while its heads are green, the walking figure while they are red
        walk, hand = red, green

    px, py, pz = comp(pos, -1250, "R"), comp(pos, -1250, "G"), comp(pos, -1250, "B")
    pyz = g.node(unreal.MaterialExpressionAppendVector, -1100)
    link(py, [""], pyz, ["A"])
    link(pz, [""], pyz, ["B"])
    front = op(unreal.MaterialExpressionSaturate, -950, op(unreal.MaterialExpressionMultiply, -1100,
               op(unreal.MaterialExpressionSubtract, -1250, px, const_b=float(spec.get("lamp_front_x", 0.0))), const_b=0.5))

    def band(e, lo, hi, x, sharp=4.0):
        a_ = op(unreal.MaterialExpressionSaturate, x, op(unreal.MaterialExpressionMultiply, x - 75, op(unreal.MaterialExpressionSubtract, x - 150, e, const_b=float(lo)), const_b=sharp))
        b_ = op(unreal.MaterialExpressionSaturate, x, op(unreal.MaterialExpressionMultiply, x - 75, rsub(x - 150, hi, e), const_b=sharp))
        return op(unreal.MaterialExpressionMultiply, x + 75, a_, b_)

    # The pack's UV maps only where they line up with the mesh ("textured_boxes", mesh units: the heads, the
    # sign); everything else (arm and pole parts that sample stray patches of the atlas) is plain paint.
    boxes = spec.get("textured_boxes")
    paint = g.node(unreal.MaterialExpressionVectorParameter, -900, parameter_name="Paint", default_value=unreal.LinearColor(*(list(spec.get("paint", (0.1, 0.1, 0.095))) + [1.0])))
    paint_rough = g.node(unreal.MaterialExpressionScalarParameter, -900, parameter_name="PaintRoughness", default_value=float(spec.get("paint_roughness", 0.55)))
    paint_metal = g.node(unreal.MaterialExpressionScalarParameter, -900, parameter_name="PaintMetallic", default_value=float(spec.get("paint_metallic", 0.0)))
    if boxes:
        textured = None
        for (x0, x1, y0, y1, z0, z1) in boxes:
            b3 = op(unreal.MaterialExpressionMultiply, -600, op(unreal.MaterialExpressionMultiply, -675, band(px, x0, x1, -750), band(py, y0, y1, -750)), band(pz, z0, z1, -750))
            textured = b3 if textured is None else op(unreal.MaterialExpressionAdd, -525, textured, b3)
        textured = op(unreal.MaterialExpressionSaturate, -450, textured)
        base = g.lerp(-300, paint, "RGB", col, "RGB", textured, "")
        MEL.connect_material_property(base, "", P.MP_BASE_COLOR)
        if nn is not None:
            flat = g.node(unreal.MaterialExpressionConstant3Vector, -450, constant=unreal.LinearColor(0.0, 0.0, 1.0, 0.0))
            MEL.connect_material_property(g.lerp(-300, flat, "", nn, "RGB", textured, ""), "", P.MP_NORMAL)
        if o is not None:
            one = g.node(unreal.MaterialExpressionConstant, -450, r=1.0)
            MEL.connect_material_property(g.lerp(-300, one, "", o, "R", textured, ""), "", P.MP_AMBIENT_OCCLUSION)
            MEL.connect_material_property(g.lerp(-300, paint_rough, "", o, "G", textured, ""), "", P.MP_ROUGHNESS)
            MEL.connect_material_property(g.lerp(-300, paint_metal, "", o, "B", textured, ""), "", P.MP_METALLIC)
    else:
        MEL.connect_material_property(col, "RGB", P.MP_BASE_COLOR)
        if nn is not None:
            MEL.connect_material_property(nn, "RGB", P.MP_NORMAL)
        if o is not None:
            MEL.connect_material_property(o, "R", P.MP_AMBIENT_OCCLUSION)
            MEL.connect_material_property(o, "G", P.MP_ROUGHNESS)
            MEL.connect_material_property(o, "B", P.MP_METALLIC)

    def discs(lamps, x):
        total = None
        for (cy, cz, r) in lamps:
            c = g.node(unreal.MaterialExpressionConstant2Vector, x - 300, r=float(cy), g=float(cz))
            d = g.node(unreal.MaterialExpressionDistance, x - 150)
            link(pyz, [""], d, ["A"])
            link(c, [""], d, ["B"])
            # 1 inside the lens, a 0.5 cm soft rim, 0 on the visor around it
            inside = op(unreal.MaterialExpressionSaturate, x, op(unreal.MaterialExpressionMultiply, x - 75, rsub(x - 150, r, d), const_b=2.0))
            total = inside if total is None else op(unreal.MaterialExpressionAdd, x + 75, total, inside)
        return op(unreal.MaterialExpressionMultiply, x + 150, op(unreal.MaterialExpressionSaturate, x + 100, total), front)

    def tinted(mask_e, on, colour_name, rgb, x):
        lit = op(unreal.MaterialExpressionMultiply, x, mask_e, on)
        c = g.node(unreal.MaterialExpressionVectorParameter, x, parameter_name=colour_name, default_value=unreal.LinearColor(rgb[0], rgb[1], rgb[2], 1.0))
        t = g.node(unreal.MaterialExpressionMultiply, x + 150)
        link(lit, [""], t, ["A"])
        link(c, ["RGB", ""], t, ["B"])
        return t

    lamps = spec.get("lamps", {})
    parts = []
    if lamps.get("red"):
        parts.append(tinted(discs(lamps["red"], -500), red, "RedLamp", (1.0, 0.04, 0.01), 0))
    if lamps.get("amber"):
        parts.append(tinted(discs(lamps["amber"], -500), amber, "AmberLamp", (1.0, 0.42, 0.02), 0))
    if lamps.get("green"):
        parts.append(tinted(discs(lamps["green"], -500), green, "GreenLamp", (0.08, 1.0, 0.5), 0))

    ped = spec.get("ped")
    if ped and lens is not None:
        # The icon is sampled at UVs made from geometry, which the texture streamer cannot see: it would keep the
        # texture at a few texels. A copy that is always resident (and smaller) next to the material instead; the
        # pack's own texture is left as it is.
        icon_path = directory + "/T_" + name[2:] + "_Icons" if name.startswith("M_") else directory + "/T_" + name + "_Icons"
        if not H.EAL.does_asset_exist(icon_path):
            H.EAL.duplicate_asset(lens.get_path_name().split(".")[0], icon_path)
        icon_tex = H.EAL.load_asset(icon_path)
        if icon_tex is not None:
            icon_tex.set_editor_property("never_stream", True)
            icon_tex.set_editor_property("max_texture_size", int(ped.get("icon_texture_size", 1024)))
            H.save(icon_tex)
            lens = icon_tex
        u0, v0, u1, v1 = ped["icon_uv"]
        w, h = ped["size"]
        cx, cz = ped["centre"]
        k = (v1 - v0) / float(h)          # icon UV per cm (the texture is square: same along u)
        # icon UV from the panel position: u grows with +X (the viewer's right, facing the panel from +Y), v down
        uu = op(unreal.MaterialExpressionAdd, -1000, op(unreal.MaterialExpressionMultiply, -1100, op(unreal.MaterialExpressionSubtract, -1250, px, const_b=float(cx)), const_b=k), const_b=(u0 + u1) * 0.5)
        vv = op(unreal.MaterialExpressionAdd, -1000, op(unreal.MaterialExpressionMultiply, -1100, op(unreal.MaterialExpressionSubtract, -1250, pz, const_b=float(cz)), const_b=-k), const_b=(v0 + v1) * 0.5)
        iuv = g.node(unreal.MaterialExpressionAppendVector, -850)
        link(uu, [""], iuv, ["A"])
        link(vv, [""], iuv, ["B"])
        icon = g.tex(-700, lens, iuv)

        on_panel = op(unreal.MaterialExpressionMultiply, -400,
                      op(unreal.MaterialExpressionMultiply, -475, band(px, cx - w * 0.5, cx + w * 0.5, -550), band(pz, cz - h * 0.5, cz + h * 0.5, -550)),
                      band(py, ped["plane_y"] - 1.0, ped["plane_y"] + 1.0, -550))
        hand_icon = op(unreal.MaterialExpressionMultiply, -250, comp(icon, -400, ped.get("hand", "R")), on_panel)
        walk_icon = op(unreal.MaterialExpressionMultiply, -250, comp(icon, -400, ped.get("walk", "G")), on_panel)
        parts.append(tinted(hand_icon, hand, "PedHand", (1.0, 0.3, 0.03), 0))
        parts.append(tinted(walk_icon, walk, "PedWalk", (0.85, 0.92, 1.0), 0))

    if parts:
        total = parts[0]
        for e in parts[1:]:
            total = op(unreal.MaterialExpressionAdd, 300, total, e)
        bright = g.node(unreal.MaterialExpressionScalarParameter, 300, parameter_name="LampBrightness", default_value=float(spec.get("brightness", 30.0)))
        em = op(unreal.MaterialExpressionMultiply, 450, total, bright)
        MEL.connect_material_property(em, "", P.MP_EMISSIVE_COLOR)
    MEL.layout_material_expressions(mat)
    MEL.recompile_material(mat)
    H.save(mat)
    return mat


def build_paint(name, spec, directory=None):
    """A plain painted finish (spec "color" linear RGB, "roughness", "metallic"), e.g. a signal mast's arm and pole
    where a pack's own trim material reads wrong. Paint / PaintRoughness / PaintMetallic tune it."""
    directory = directory or DIR
    path = directory + "/" + name
    H.ensure_dir(directory)
    if H.EAL.does_asset_exist(path):
        mat = H.EAL.load_asset(path)
        _clear(mat)
    else:
        mat = H.asset_tools().create_asset(name, directory, unreal.Material, unreal.MaterialFactoryNew())
    _instanced_usage(mat)
    c = spec.get("color", (0.1, 0.1, 0.095))
    col = MEL.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, -400, 0)
    col.set_editor_property("parameter_name", "Paint")
    col.set_editor_property("default_value", unreal.LinearColor(c[0], c[1], c[2], 1.0))
    MEL.connect_material_property(col, "RGB", unreal.MaterialProperty.MP_BASE_COLOR)
    for i, (pname, prop, val) in enumerate((("PaintRoughness", unreal.MaterialProperty.MP_ROUGHNESS, spec.get("roughness", 0.55)),
                                            ("PaintMetallic", unreal.MaterialProperty.MP_METALLIC, spec.get("metallic", 0.0)))):
        e = MEL.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -400, 200 + 150 * i)
        e.set_editor_property("parameter_name", pname)
        e.set_editor_property("default_value", float(val))
        MEL.connect_material_property(e, "", prop)
    MEL.recompile_material(mat)
    H.save(mat)
    return mat


def build_puddle_decal(name="M_RammsPuddle", directory=None):
    """A deferred decal of standing water, made from nodes alone (no pack ships one): an irregular blob (a radial
    falloff broken up by noise) of dark, near-mirror water with a flat normal. Returns the material."""
    directory = directory or DIR
    path = directory + "/" + name
    H.ensure_dir(directory)
    if H.EAL.does_asset_exist(path):
        mat = H.EAL.load_asset(path)
        _clear(mat)
    else:
        mat = H.asset_tools().create_asset(name, directory, unreal.Material, unreal.MaterialFactoryNew())
    mat.set_editor_property("material_domain", unreal.MaterialDomain.MD_DEFERRED_DECAL)
    mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)

    def node(cls, x, y, **props):
        e = MEL.create_material_expression(mat, cls, x, y)
        for k, v in props.items():
            e.set_editor_property(k, v)
        return e

    uv = node(unreal.MaterialExpressionTextureCoordinate, -1200, 0)
    centre = node(unreal.MaterialExpressionConstant2Vector, -1200, 120, r=0.5, g=0.5)
    dist = node(unreal.MaterialExpressionDistance, -1000, 40)
    MEL.connect_material_expressions(uv, "", dist, "A")
    MEL.connect_material_expressions(centre, "", dist, "B")
    # 1 at the centre, 0 on the decal's edge circle, broken up by noise and sharpened into a blob
    two = node(unreal.MaterialExpressionMultiply, -930, 40, const_b=2.0)
    MEL.connect_material_expressions(dist, "", two, "A")
    radial = node(unreal.MaterialExpressionOneMinus, -850, 40)
    MEL.connect_material_expressions(two, "", radial, "")
    noise = node(unreal.MaterialExpressionNoise, -1200, 260, scale=0.012, levels=4, output_min=-0.6, output_max=0.6)
    add = node(unreal.MaterialExpressionAdd, -650, 120)
    MEL.connect_material_expressions(radial, "", add, "A")
    MEL.connect_material_expressions(noise, "", add, "B")
    sharp = node(unreal.MaterialExpressionMultiply, -500, 120, const_b=4.0)
    MEL.connect_material_expressions(add, "", sharp, "A")
    mask = node(unreal.MaterialExpressionSaturate, -350, 120)
    MEL.connect_material_expressions(sharp, "", mask, "")
    MEL.connect_material_property(mask, "", unreal.MaterialProperty.MP_OPACITY)
    base = node(unreal.MaterialExpressionConstant3Vector, -350, -200, constant=unreal.LinearColor(0.025, 0.025, 0.03, 1.0))
    MEL.connect_material_property(base, "", unreal.MaterialProperty.MP_BASE_COLOR)
    rough = node(unreal.MaterialExpressionConstant, -350, -60, r=0.04)
    MEL.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    spec = node(unreal.MaterialExpressionConstant, -350, 0, r=0.5)
    MEL.connect_material_property(spec, "", unreal.MaterialProperty.MP_SPECULAR)
    flat = node(unreal.MaterialExpressionConstant3Vector, -350, 260, constant=unreal.LinearColor(0.0, 0.0, 1.0, 1.0))
    MEL.connect_material_property(flat, "", unreal.MaterialProperty.MP_NORMAL)
    MEL.recompile_material(mat)
    H.save(mat)
    return mat


def build_window_glass(name="M_RammsWindowGlass", directory=None):
    """Window glass for facade modules, made from nodes alone: a dark, smooth dielectric that reflects the sky and
    the street (Lumen), its tint varied per module instance (PerInstanceRandom) so a facade's windows do not all
    read the same. Packs' own facade glass is often a fake-interior material that needs reflection captures or
    virtual textures and renders as a flat light panel without them. Returns the material."""
    directory = directory or DIR
    path = directory + "/" + name
    H.ensure_dir(directory)
    if H.EAL.does_asset_exist(path):
        mat = H.EAL.load_asset(path)
        _clear(mat)
    else:
        mat = H.asset_tools().create_asset(name, directory, unreal.Material, unreal.MaterialFactoryNew())
    # two-sided: packs model window panes facing either way (their own glass is two-sided too)
    mat.set_editor_property("two_sided", True)
    _instanced_usage(mat)   # on facade modules, which the spawners place as instances (pack kits are often Nanite)

    def node(cls, x, y, **props):
        e = MEL.create_material_expression(mat, cls, x, y)
        for k, v in props.items():
            e.set_editor_property(k, v)
        return e

    rnd = node(unreal.MaterialExpressionPerInstanceRandom, -700, 0)
    dark = node(unreal.MaterialExpressionConstant3Vector, -700, -160, constant=unreal.LinearColor(0.010, 0.013, 0.018, 1.0))
    lighter = node(unreal.MaterialExpressionConstant3Vector, -700, -60, constant=unreal.LinearColor(0.045, 0.050, 0.052, 1.0))
    tint = node(unreal.MaterialExpressionLinearInterpolate, -450, -100)
    MEL.connect_material_expressions(dark, "", tint, "A")
    MEL.connect_material_expressions(lighter, "", tint, "B")
    MEL.connect_material_expressions(rnd, "", tint, "Alpha")
    MEL.connect_material_property(tint, "", unreal.MaterialProperty.MP_BASE_COLOR)
    rough = node(unreal.MaterialExpressionConstant, -450, 60, r=0.05)
    MEL.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    spec = node(unreal.MaterialExpressionConstant, -450, 140, r=0.6)
    MEL.connect_material_property(spec, "", unreal.MaterialProperty.MP_SPECULAR)
    MEL.recompile_material(mat)
    H.save(mat)
    return mat


def build_surfaces():
    """Create or refresh every surface material, the paint instances, the puddle decal and the window glass.
    Returns {name: path or None}."""
    out = {}
    for name, spec in SURFACES.items():
        try:
            m = _build(name, spec)
        except Exception as exc:  # noqa: BLE001
            H.warn("surface %s failed: %s" % (name, exc))
            m = None
        out[name] = m.get_path_name() if m else None
    try:
        out["paints"] = build_paints()
    except Exception as exc:  # noqa: BLE001
        H.warn("paints failed: %s" % exc)
    try:
        out["M_RammsPuddle"] = build_puddle_decal().get_path_name()
    except Exception as exc:  # noqa: BLE001
        H.warn("puddle decal failed: %s" % exc)
    try:
        out["M_RammsWindowGlass"] = build_window_glass().get_path_name()
    except Exception as exc:  # noqa: BLE001
        H.warn("window glass failed: %s" % exc)
    H.log("surfaces: %s" % out)
    return out
