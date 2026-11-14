"""Downtown West Modular Pack (Fab): street furniture, street trees, decals and ground textures.

Its trees are native UE foliage (masked leaves, wind) and replace other packs' in the tree slots. Its street
light comes in three dressings: only the plain pole is offered - `SM_light_streetlight_complete` carries traffic
signals (they belong at intersections, not mid-block) and the light posts `light_post_a/b` carry "DOWNTOWN WEST"
banners. The pack is modelled ~1.3-1.5x real size (6 m storeys); props here are normalised per slot as usual.
"""

from .base import Pack

A = "Assets/"

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
    },
    material_sets={
        "Decal.Road.Crack": ["Materials/Decals/MI_Decal_Crack_%s" % c for c in "ABC"],
        "Decal.Sidewalk.Crack": ["Materials/Decals/MI_Decal_Crack_%s" % c for c in "ABC"] + ["Materials/Decals/MI_Decal_Cracks"],
        "Decal.Stain": ["Materials/Decals/MI_Decal_Grunge_%s" % c for c in "abc"],
    },
    surfaces={
        # separate albedo / normal / roughness maps: the greyscale roughness stands in as the packed map (its
        # channel doubling as a mild AO)
        "Asphalt": dict(bc="Textures/Asphalt/T_asphalt_base_albedo", n="Textures/Asphalt/T_asphalt_base_normal",
                        rma="Textures/Asphalt/T_asphalt_base_rough", layout="rma", tile=400.0, second=False, mask=None),
        "Curb": dict(bc="Textures/Concrete_Curb/T_curb_straight_concrete_albedo", n="Textures/Concrete_Curb/T_curb_straight_concrete_normal",
                     rma="Textures/Concrete_Curb/T_curb_straight_concrete_rough", layout="rma", tile=150.0, second=False, mask=None),
    },
    notes="Modelled ~1.3-1.5x real size; facades are open single-sided shells (phase 2 kits).",
)
