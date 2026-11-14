"""The content packs the world generator knows, by name (see base.Pack); recipes.py composes palettes from them.

Adding a pack: a module here declaring PACK = Pack(name, [content folders], meshes={slot: [paths]},
material_sets={slot: [paths]}, surfaces={key: spec}), listed in ALL below.
"""

from . import american_city, camden_market, downtown_west, fab_scenes, megascans
from .base import Kit, Pack, Part, Ref

ALL = [american_city.PACK, downtown_west.PACK, camden_market.PACK, megascans.PACK, fab_scenes.PACK]
BY_NAME = {p.name: p for p in ALL}


def get(name):
    if name not in BY_NAME:
        raise KeyError("no pack %r (known: %s)" % (name, ", ".join(BY_NAME)))
    return BY_NAME[name]


def installed():
    """Names of the packs whose content is in the project."""
    return [p.name for p in ALL if p.installed()]


__all__ = ["ALL", "BY_NAME", "Kit", "Pack", "Part", "Ref", "get", "installed"]
