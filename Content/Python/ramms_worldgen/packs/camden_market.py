"""Camden Market (Fab): dirt decals here; its building kits come with the facade kit work.

The pack carries 43 object redirectors and a third-person demo folder; only Materials and Buildings are used.
"""

from .base import Pack

DIRT = "Materials/MaterialInstances/DirtDecals/"

PACK = Pack(
    "CamdenMarket",
    ["/Game/CamdenMarket"],
    material_sets={
        "Decal.Stain": [DIRT + n for n in ("MI_Decal_Dirt01", "MI_Decal_Dirt02", "MI_Decal_Dirt03", "MI_Decal_DirtSpot01",
                                           "MI_Decal_DirtSpot02", "MI_Decal_DirtSpot03")],
    },
)
