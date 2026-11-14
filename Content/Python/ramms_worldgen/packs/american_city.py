"""American City Packs (Fab): decals here; its facade kits are built by palettes.build_facade_slots.

The pack's decals are deferred decals in Materials/Decals: cracks for roads and sidewalks, road patches (no
potholes as such: the smaller patches stand in), and grime that reads as stains on concrete.
"""

from .base import Pack

D = "Materials/Decals/"

PACK = Pack(
    "AmericanCityPacks",
    ["/Game/AmericanCityPacks"],
    material_sets={
        "Decal.Road.Crack": [D + "Cracks/MI_Cracks_0%d" % i for i in range(1, 5)],
        "Decal.Sidewalk.Crack": [D + "Cracks/MI_Cracks_0%d" % i for i in range(1, 5)],
        "Decal.Road.Patch": [D + "Road/MI_Patch_0%d" % i for i in range(1, 7)],
        "Decal.Road.Pothole": [D + "Road/MI_Patch_0%d" % i for i in range(1, 4)],
        "Decal.Stain": [D + "Dirt/MI_dirt_0%d" % i for i in range(1, 4)] + [D + "Dirt/MI_dirt2_0%d" % i for i in range(1, 5)],
    },
)
