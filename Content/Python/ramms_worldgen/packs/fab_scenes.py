"""Single Fab scenes the project carries under /Game/Fab: the Restaurant Scene's stain and leak decals."""

from .base import Pack

PACK = Pack(
    "RestaurantScene",
    ["/Game/Fab/RestaurantScene"],
    material_sets={
        "Decal.Stain": ["Materials/MI_Stains", "Materials/MI_Leaks_1"],
    },
)
