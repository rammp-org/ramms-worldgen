"""Quixel Megascans surfaces (Fab), as world-aligned ground and wall materials.

Megascans surfaces land under /Game/Fab/Megascans/Surfaces/<Name>_<code>/Medium/<code>_tier_2/Textures/
T_<code>_2K_{B,N,ORM}: the helper below spells that out from the folder name and the code.
"""

from .base import Pack


def scan(folder, code, **spec):
    base = "Surfaces/%s/Medium/%s_tier_2/Textures/T_%s_2K_" % (folder, code, code)
    return dict(bc=base + "B", n=base + "N", rma=base + "ORM", layout="orm", second=False, mask=None, **spec)


PACK = Pack(
    "Megascans",
    ["/Game/Fab/Megascans"],
    surfaces={
        # the texture holds 2 x 2 panels: a 305 cm repeat makes each a 5 ft (152 cm) US sidewalk panel
        "Sidewalk": scan("Dirty_Sidewalk_Tiles_ugxjcdpn", "ugxjcdpn", tile=305.0),
        "Curb": scan("Smooth_Concrete_ugkjbgplw", "ugkjbgplw", tile=150.0, tint=(0.85, 0.85, 0.85)),
        # the same cast concrete at slab scale, for forecourts, lot ground and plazas
        "Concrete": scan("Smooth_Concrete_ugkjbgplw", "ugkjbgplw", tile=400.0, tint=(0.88, 0.88, 0.88)),
        # plain asphalt: Fine American Road has lane lines painted into its texture (world-aligned, they would
        # run across side streets)
        "Asphalt": scan("Road_Asphalt_rh0ribp0", "rh0ribp0", tile=250.0, tint=(0.9, 0.9, 0.92)),
        "Brick": scan("Brick_Facade_vizcegj", "vizcegj", tile=200.0),
        "Plaster": scan("Plastered_Concrete_Wall_ugtkdffg", "ugtkdffg", tile=300.0, tint=(0.92, 0.88, 0.82)),
        "ConcreteWall": scan("Weathered_Concrete_Wall_vi4idbm", "vi4idbm", tile=300.0),
    },
)
