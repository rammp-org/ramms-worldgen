"""American City Packs (Fab): decals and whole downtown buildings; its facade kits are built by
palettes.build_facade_slots.

The pack's decals are deferred decals in Materials/Decals: cracks for roads and sidewalks, road patches (no
potholes as such: the smaller patches stand in), and grime that reads as stains on concrete. Its buildings are
packed level actors assembled from the same modules as the kits: 13-24 storeys, 17-40 m of frontage; the corner
ones wrap one end (either hand). BPP_Building_DT_06 stands free (four fronts) and is left out of the street.
"""

from .base import Pack, Ref

D = "Materials/Decals/"
G = "Geometry/Buildings/"

PACK = Pack(
    "AmericanCityPacks",
    ["/Game/AmericanCityPacks"],
    meshes={
        # closed low-poly towers (12-35 storeys at 3 m), for the skyline outside a downtown site (levels._add_skyline)
        "Skyline": [Ref("Geometry/BackgroundBuildings/SM_*", normalise=False)],
    },
    buildings={
        "Downtown": [G + "BPP_Building_DT_0%d" % n for n in (1, 2, 3, 4, 5, 7)],
        "DowntownCorner": [G + "BPP_BuildingCorner_DT_0%d" % n for n in range(1, 7)],
    },
    material_sets={
        "Decal.Road.Crack": [D + "Cracks/MI_Cracks_0%d" % i for i in range(1, 5)],
        "Decal.Sidewalk.Crack": [D + "Cracks/MI_Cracks_0%d" % i for i in range(1, 5)],
        "Decal.Road.Patch": [D + "Road/MI_Patch_0%d" % i for i in range(1, 7)],
        "Decal.Road.Pothole": [D + "Road/MI_Patch_0%d" % i for i in range(1, 4)],
        "Decal.Stain": [D + "Dirt/MI_dirt_0%d" % i for i in range(1, 4)] + [D + "Dirt/MI_dirt2_0%d" % i for i in range(1, 5)],
    },
)
