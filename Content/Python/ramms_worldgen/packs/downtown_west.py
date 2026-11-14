"""Downtown West Modular Pack (Fab): street furniture, street trees, decals and ground textures.

Its trees are native UE foliage (masked leaves, wind) and replace other packs' in the tree slots. Its street
light comes in three dressings: only the plain pole is a street light - `SM_light_streetlight_complete` carries
traffic signals on a mast arm and is the corner signal (TrafficSignal: heads face +X, the arm runs along +Y), and
the light posts `light_post_a/b` carry "DOWNTOWN WEST" banners. The pack is modelled ~1.3-1.5x real size (6 m storeys); props here are normalised per slot as usual.
"""

from .base import Kit, Pack, Ref

A = "Assets/"

# The building kits: pieces face +X with the facade plane on X = 0 and run along Y (mostly centred pivots), so a
# quarter turn brings them to the convention; at 0.75 the 6 m storeys are 4.5 m and the 3 m doors 2.2 m. Each
# kit keeps its own trim-sheet materials. Building b has two storefront lines of different heights (3.97 and
# 4.3 m): two styles. No corner pieces are used (most kits have none): street faces are dressed, corners show
# the shell. building_a's lvl2 corner is a 45 degree bay; building_f's lvl1 has only entrances, so its ground
# floor repeats the 8 m window bay.
DW_SCALE = 0.75
_B = A + "building_b/SM_build_b_mod_"
_B_UPPER = dict(Win=[_B + "lvl2_doublewindow", _B + "lvl2_singlewindow", _B + "lvl2_widewindow"],
                WinTop=[_B + "lvl3_doublewindow", _B + "lvl3_singlewindow"], Top=[_B + "top_trim"])
# the pack's building glass is a fake-interior material that renders as flat grey panels here: every glass slot
# (glass, glass3, glass_001...) takes the built-in window glass
_KIT = dict(family="Downtown_West", scale=DW_SCALE, yaw=90.0, weight=0.5, max_stretch=0.12, upper_mix=0.0, materials=[(r"^glass", "Glass")])
KITS = [
    Kit("dwba", roles=dict(GF=[_B + "lvl1_storefront_a_5m", _B + "lvl1_storefront_a_3_5m"], GFDoor=[_B + "lvl1_doubleblackdoor"], **_B_UPPER), **_KIT),
    Kit("dwbb", roles=dict(GF=[_B + "lvl1_storefront_b_wall3m", _B + "lvl1_storefront_b_wall5m"],
                           GFDoor=[_B + "lvl1_storefront_b_doubledoorframe", _B + "lvl1_storefront_b_singledoorframe"], **_B_UPPER), **_KIT),
    Kit("dwcafe", roles=dict(GF=[A + "building_a/SM_build_cafe_a_mod_windows_f", A + "building_a/SM_build_cafe_a_mod_windows_long_a",
                                 A + "building_a/SM_build_cafe_a_mod_windows_long_d", A + "building_a/SM_build_cafe_a_mod_wall_b"],
                             GFDoor=[A + "building_a/SM_build_cafe_a_mod_door_a", A + "building_a/SM_build_cafe_a_mod_door_b"],
                             Win=[A + "building_a/SM_build_cafe_a_mod_lvl2_windows_%s" % c for c in "abce"],
                             Top=[A + "building_a/SM_build_cafe_a_mod_top_trim_a"]), **_KIT),
    Kit("dwd", roles=dict(GF=[A + "building_d/SM_build_d_mod_lvl1_window", A + "building_d/SM_build_d_mod_lvl1_window_vent"],
                          GFDoor=[A + "building_d/SM_build_d_mod_lvl1_window_door"], Win=[A + "building_d/SM_build_d_mod_wall_lvl2_window"],
                          Top=[A + "building_d/SM_build_d_mod_trim_top_a"]), **_KIT),
    Kit("dwe", roles=dict(GF=[A + "building_e/SM_build_e_mod_lvl1_window_a", A + "building_e/SM_build_e_mod_wall_a"],
                          GFDoor=[A + "building_e/SM_build_e_mod_lvl1_door_b"], Win=[A + "building_e/SM_build_e_mod_wall_lvl2_window"],
                          Top=[A + "building_e/SM_build_e_mod_trim_top_a"]), **_KIT),
    Kit("dwf", roles=dict(GF=[A + "building_f/SM_build_f_mod_lvl2_window_b"], GFDoor=[A + "building_f/SM_build_f_mod_lvl1_door_a"],
                          Win=[A + "building_f/SM_build_f_mod_lvl2_window_b"], Top=[A + "building_f/SM_build_f_mod_trim_top_a_long"]), **_KIT),
]

PACK = Pack(
    "Downtown_West",
    ["/Game/Downtown_West"],
    meshes={
        "Tree": [A + "foliage/tree_oak_a/sm_tree_oak_a"] + [A + "foliage/tree_narrowleaf/SM_tree_narrowleaf_%s" % c for c in "abc"],
        "TreeLarge": [A + "foliage/tree_oak_a/sm_tree_oak_a"],
        "TreeSmall": [A + "foliage/tree_narrowleaf/SM_tree_narrowleaf_%s" % c for c in "abc"],
        "Bench": [A + "props/prop_bench_wood/SM_bench_wood_a", A + "props/prop_bench_wood/SM_bench_wood_b"],
        "Hydrant": [A + "props/prop_fire_hydrant/SM_fire_hydrant"],
        "TrashBin": [A + "props/prop_recycle_bin/SM_prop_recycle_bin"],
        "Flowerbed": [A + "props/prop_pots/SM_pot_%s" % c for c in "abcd"],
        "StreetLight": [A + "props/props_streetlight/SM_light_streetlight"],
        "InfoBoard": [A + "props/prop_poster_stands/SM_prop_poster_pillar_with_posters"],
        "Hazard.Cafe": [A + "props/prop_table/SM_prop_dining_table_a"],
        "Hazard.CafeChair": [A + "props/prop_table/SM_prop_dining_chair_a", A + "props/prop_table/SM_prop_dining_chair_b"],
        "TrafficSignal": [Ref(A + "props/props_streetlight/SM_light_streetlight_complete", yaw=0.0, scale=DW_SCALE)],
        # 1.33 m at the pack's scale: ~80 cm covers
        "Cover.Manhole": [Ref(A + "props/prop_sewer_caps/SM_sewer_cap_*", scale=0.6)],
    },
    material_sets={
        "Decal.Road.Crack": ["Materials/Decals/MI_Decal_Crack_%s" % c for c in "ABC"],
        "Decal.Sidewalk.Crack": ["Materials/Decals/MI_Decal_Crack_%s" % c for c in "ABC"] + ["Materials/Decals/MI_Decal_Cracks"],
        "Decal.Stain": ["Materials/Decals/MI_Decal_Grunge_%s" % c for c in "abc"],
    },
    kits=KITS,
    surfaces={
        # separate albedo / normal / roughness maps: the greyscale roughness stands in as the packed map (its
        # channel doubling as a mild AO)
        "Asphalt": dict(bc="Textures/Asphalt/T_asphalt_base_albedo", n="Textures/Asphalt/T_asphalt_base_normal",
                        rma="Textures/Asphalt/T_asphalt_base_rough", layout="rma", tile=400.0, second=False, mask=None),
        "Curb": dict(bc="Textures/Concrete_Curb/T_curb_straight_concrete_albedo", n="Textures/Concrete_Curb/T_curb_straight_concrete_normal",
                     rma="Textures/Concrete_Curb/T_curb_straight_concrete_rough", layout="rma", tile=150.0, second=False, mask=None),
    },
    notes="Modelled ~1.3-1.5x real size (kits at 0.75); facades are open single-sided shells over the massing.",
)
