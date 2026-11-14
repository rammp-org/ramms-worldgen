"""Camden Market (Fab): its building kits as facade styles, and dirt decals.

The pack carries 43 object redirectors and a third-person demo folder; only Materials and Buildings are used.
Its sets run along different axes: the pub's PUB_Main pieces face +X and run along -Y (a quarter turn), the
Residential / Industrial walls are mirror-symmetric ~40 cm slabs centred on X = 0 that run along +Y (the other
quarter turn, moved in by half their thickness), and the Building04 storefronts are already in the convention.
Nothing of Camden's has a door module: the generator's own door goes on its ground floors. Real scale.
"""

from .base import Kit, Pack, Part

DIRT = "Materials/MaterialInstances/DirtDecals/"
B = "Buildings/"
# the storefronts are modelled 68 cm proud of their back plane: set back to project ~28 cm, like a shopfront
_STORE = [Part(B + "Building04/SM_Building04_StoreFrontA_LOD0", yaw=0.0, dy=-40.0), Part(B + "Building04/SM_Building04_StoreFrontB_LOD0", yaw=0.0, dy=-40.0)]
_PARAPET = [Part(B + "Building04/SM_Building04_WallShort_LOD0", yaw=0.0)]
# MI_BuildingGlass renders as flat cream panels here (a fake-interior glass): the built-in window glass instead
_GLASS = [(r"^glass$", "Glass")]
KITS = [
    # the pub: tall arched ground-floor windows, single sash windows above, a cornice
    Kit("cmpub", "CamdenMarket", roles=dict(GF=[B + "PUB/SM_PUB_Main_Window_LOD0"], Win=[B + "PUB/SM_PUB_Main_WindowSingle_LOD0"],
                                             Top=[B + "PUB/SM_PUB_Main_TopWall_LOD0"]), yaw=90.0, weight=1.0, max_stretch=0.15, materials=_GLASS),
    # brick terraces over the Building04 storefronts, a parapet on top
    Kit("cmres", "CamdenMarket", roles=dict(GF=_STORE, Win=[Part(B + "Residential/SM_Residential0%d_LOD0" % n, yaw=-90.0, dy=-20.0) for n in (1, 3)]
                                             + [Part(B + "Residential/SM_Residential02_LOD0", yaw=-90.0, dy=-20.0, weight=0.4)], Top=_PARAPET),
        yaw=-90.0, weight=0.8, max_stretch=0.1, upper_mix=0.3, materials=_GLASS),
    Kit("cmind", "CamdenMarket", roles=dict(GF=_STORE, Win=[Part(B + "Industrial/SM_BuildingIndustrial_01_LOD0", yaw=-90.0, dy=-20.0)]
                                             + [Part(B + "Industrial/SM_BuildingIndustrial_0%d_LOD0" % n, yaw=-90.0, dy=-20.0, weight=0.4) for n in (2, 3)], Top=_PARAPET),
        yaw=-90.0, weight=0.6, max_stretch=0.1, upper_mix=0.3, materials=_GLASS),
]

PACK = Pack(
    "CamdenMarket",
    ["/Game/CamdenMarket"],
    kits=KITS,
    material_sets={
        "Decal.Stain": [DIRT + n for n in ("MI_Decal_Dirt01", "MI_Decal_Dirt02", "MI_Decal_Dirt03", "MI_Decal_DirtSpot01",
                                           "MI_Decal_DirtSpot02", "MI_Decal_DirtSpot03")],
    },
)
