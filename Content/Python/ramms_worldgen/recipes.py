"""Palettes put together from content packs (packs/), reproducibly from code.

A recipe says how one palette is made:

  base       PAL_Default's slots, optionally with UrbanVerse-100K street furniture, cars and trees on top
             (`urbanverse`: mode and slots for urbanverse.build; needs its local cache, else skipped);
  clear      slots emptied (a pack's scan that reads wrong in this look, houses on a downtown palette);
  layers     (pack, mode, slots, weight): what each pack adds to ("add") or puts in place of ("replace") the
             slots it offers -- slots None for every slot it offers. Later layers see earlier ones;
  surfaces   palette Mat.* slots -> "<pack>:<surface>" (or a list: the slot's material set), built from that
             pack's textures as world-aligned materials into the recipe's material folder;
  puddles    add the built-in puddle decal (surfaces.build_puddle_decal: no pack ships one) to Decal.Puddle;
  buildings  packs whose whole buildings fill Building.Downtown / Building.CornerNeg / CornerPos (lots are cut to
             them with the WholeBuildingChance parameter, e.g. PRE_DenseDowntown's);
  kits       packs whose modular facade kits become facade styles (packs.base.Kit; a level picks among them
             through the FacadeStyles parameter, e.g. PRE_DenseDowntown's).

Packs that are not installed contribute nothing: a recipe still builds (and reports what it skipped).

    import ramms_worldgen.recipes as R
    pal, report = R.build("Downtown")
"""

import os

import unreal

from . import _helpers as H
from . import packs as PK
from . import palettes as P

OUT = "/Game/WorldGen"

# UrbanVerse-100K street furniture, hazards, cars and trees that suit a busy downtown (see urbanverse.py).
URBAN_FURNITURE = ["Bench", "Bollard", "BusStop", "StreetLight", "TrashBin", "Hydrant", "ParkingMeter", "NewspaperBox", "Mailbox",
                   "StreetSign", "Flowerbed", "Hazard.Bike", "Hazard.Scooter", "Hazard.Cone", "Hazard.Barrier", "Hazard.Cafe",
                   "Hazard.CafeChair", "Hazard.Dumpster", "Hazard.Box", "Car", "Tree"]

RECIPES = {
    # A dense, worn downtown (the README's hero street): UrbanVerse furniture and cars, Downtown West street
    # trees and furniture, decals from every pack that has them, Megascans sidewalks and walls, Downtown West
    # asphalt and curbs. Facades stay the default kits; no houses (downtown lots), no tree-pit scan.
    "Downtown": dict(
        urbanverse=dict(mode="replace", slots=URBAN_FURNITURE),
        clear=["House", "TreePit", "Hazard.Upheaval"],
        layers=[
            ("Downtown_West", "replace", ["Tree", "TreeLarge", "TreeSmall"], 1.0),
            ("Downtown_West", "add", None, 1.0),
            ("AmericanCityPacks", "replace", ["Decal.Road.Patch", "Decal.Road.Pothole"], 1.0),
            ("AmericanCityPacks", "add", ["Decal.Road.Crack", "Decal.Sidewalk.Crack", "Decal.Stain"], 1.0),
            ("AmericanCityPacks", "replace", ["Skyline"], 1.0),
            # street litter and manholes (LitterDensity); Downtown West and Camden add theirs, and the corner
            # signals (StreetRules), with their other slots below
            ("AmericanCityPacks", "add", ["Litter.Leaves", "Litter.Gutter", "Litter.Trash", "Litter.Bags", "Cover.Manhole"], 1.0),
            # (not Camden's pole signal: it cycles on its own timer, out of phase with the lit Downtown West masts)
            ("CamdenMarket", "add", ["Cover.Utility", "Decal.Stain"], 1.0),
            ("RestaurantScene", "add", None, 1.0),
        ],
        surfaces={
            "Mat.Sidewalk": "Megascans:Sidewalk", "Mat.Path": "Megascans:Sidewalk",
            # Downtown West's curb texture is a curb's (edge lines): on curbs only, plain concrete on slabs
            "Mat.Asphalt": "Downtown_West:Asphalt", "Mat.Curb": "Downtown_West:Curb", "Mat.Concrete": "Megascans:Concrete",
            "Mat.Facade": ["Megascans:Brick"], "Mat.FacadeAlt": ["Megascans:Plaster", "Megascans:ConcreteWall"],
        },
        puddles=True,
        kits=["Downtown_West", "CamdenMarket"],
        buildings=["AmericanCityPacks"],
    ),
}


def palette_path(name):
    return "%s/Palettes/PAL_%s" % (OUT, name)


def materials_dir(name):
    return "%s/Materials/%s" % (OUT, name)


def _slot(pal, name):
    s = pal.get_editor_property("slots").get(unreal.Name(name))
    return s if s is not None else unreal.RammsPaletteSlot()


def _base(name, recipe, path, cache, report):
    """The palette with the recipe's base in it: PAL_Default's slots, plus UrbanVerse when asked and possible."""
    uv = recipe.get("urbanverse")
    if uv:
        if cache and os.path.isdir(cache):
            from . import urbanverse as UV
            pal, uv_report = UV.build(cache, mode=uv.get("mode", "replace"), slots=uv.get("slots"), path=path)
            report["urbanverse"] = {k: len(v) for k, v in uv_report["slots"].items()}
            return pal
        report["skipped"].append("urbanverse (no cache: pass urbanverse_cache or set RAMMS_URBANVERSE_CACHE)")
    if not H.EAL.does_asset_exist(P.DEFAULT_PALETTE):
        P.build_default_palette()
    default = unreal.load_asset(P.DEFAULT_PALETTE)
    H.ensure_dir(path.rsplit("/", 1)[0])
    pal, _ = H.get_or_create(path, unreal.RammsWorldPalette)
    pal.set_editor_property("slots", default.get_editor_property("slots"))
    pal.set_editor_property("default_material", default.get_editor_property("default_material"))
    return pal


def _with_materials(e, mesh, pack, ref, name, report):
    """Make the entry a one-part composite whose part carries per-slot materials: the slots `ref.materials` names
    take the pack's surface (built into the recipe's folder), the others keep the mesh's own."""
    from . import surfaces as SF
    mats = []
    for sm in mesh.static_materials:
        key = ref.materials.get(str(sm.material_slot_name))
        mat = None
        if key:
            if key in pack.surfaces:
                mat = SF._build("M_%s_%s_%s" % (name, pack.name.replace("_", ""), key), pack.surface_spec(key), directory=materials_dir(name))
            if mat is None:
                report["skipped"].append("material %s:%s for %s" % (pack.name, key, mesh.get_name()))
        mats.append(mat or sm.material_interface)
    part = unreal.RammsPalettePart()
    part.set_editor_property("mesh", mesh)
    part.set_editor_property("relative_transform", unreal.Transform())
    part.set_editor_property("materials", mats)
    e.set_editor_property("parts", [part])
    return e


def _apply_layer(pal, pack, mode, slots, weight, report, name=None):
    """One pack's contribution: meshes and material sets for the slots it offers (all, or those listed)."""
    done = {}
    for slot in sorted(pack.meshes):
        if slots is not None and slot not in slots:
            continue
        entries = []
        for mesh, ref in pack.slot_meshes(slot):
            e = P._mesh_entry(mesh, slot, yaw=ref.yaw, normalise=ref.normalise, scale=ref.scale)
            e.set_editor_property("weight", ref.weight * weight)
            if ref.materials and name:
                e = _with_materials(e, mesh, pack, ref, name, report)
            entries.append(e)
        if not entries:
            continue
        s = _slot(pal, slot)
        have = [] if mode == "replace" else list(s.get_editor_property("meshes"))
        paths = {e.get_editor_property("mesh").get_path_name() for e in have if e.get_editor_property("mesh")}
        new = [e for e in entries if e.get_editor_property("mesh").get_path_name() not in paths]
        s.set_editor_property("meshes", have + new)
        pal.set_slot(unreal.Name(slot), s)
        done[slot] = len(new)
    for slot in sorted(pack.material_sets):
        if slots is not None and slot not in slots:
            continue
        mats = [m for m, _ref in pack.slot_materials(slot)]
        if slot.startswith("Decal."):
            mats = [m for m in mats if P._is_decal(m)]
        if not mats:
            continue
        s = _slot(pal, slot)
        have = [] if mode == "replace" else [m for m in s.get_editor_property("materials") if m is not None]
        paths = {m.get_path_name() for m in have}
        new = [m for m in mats if m.get_path_name() not in paths]
        allm = have + new
        s.set_editor_property("materials", allm)
        if allm and (mode == "replace" or s.get_editor_property("material") is None):
            s.set_editor_property("material", allm[0])
        pal.set_slot(unreal.Name(slot), s)
        done[slot] = len(new)
    report["layers"].append({"pack": pack.name, "mode": mode, "added": done})


def _apply_surfaces(pal, name, surfaces, report):
    from . import surfaces as SF
    built = {}

    def surface(key):
        if key in built:
            return built[key]
        pack_name, surf = key.split(":", 1)
        pack = PK.get(pack_name)
        mat = None
        if pack.installed() and surf in pack.surfaces:
            mat = SF._build("M_%s_%s_%s" % (name, pack_name.replace("_", ""), surf), pack.surface_spec(surf), directory=materials_dir(name))
        if mat is None:
            report["skipped"].append("surface %s" % key)
        built[key] = mat
        return mat

    for slot, keys in surfaces.items():
        mats = [m for m in (surface(k) for k in (keys if isinstance(keys, list) else [keys])) if m is not None]
        if not mats:
            continue
        s = _slot(pal, slot)
        s.set_editor_property("material", mats[0])
        if isinstance(keys, list):
            s.set_editor_property("materials", mats)
        pal.set_slot(unreal.Name(slot), s)
        report["surfaces"][slot] = [m.get_name() for m in mats]


def build(name, path=None, urbanverse_cache=None):
    """Build (or refresh in place) the recipe's palette. Returns (palette, report)."""
    if name not in RECIPES:
        raise KeyError("no recipe %r (known: %s)" % (name, ", ".join(RECIPES)))
    recipe = RECIPES[name]
    path = path or palette_path(name)
    cache = urbanverse_cache or os.environ.get("RAMMS_URBANVERSE_CACHE")
    report = {"recipe": name, "path": path, "packs": PK.installed(), "skipped": [], "layers": [], "surfaces": {}, "cleared": []}
    P._COLLISION_DONE.clear()
    pal = _base(name, recipe, path, cache, report)
    for slot in recipe.get("clear", []):
        s = _slot(pal, slot)
        s.set_editor_property("meshes", [])
        pal.set_slot(unreal.Name(slot), s)
        report["cleared"].append(slot)
    for pack_name, mode, slots, weight in recipe.get("layers", []):
        pack = PK.get(pack_name)
        if not pack.installed():
            report["skipped"].append("pack %s (not installed)" % pack_name)
            continue
        _apply_layer(pal, pack, mode, slots, weight, report, name=name)
    _apply_surfaces(pal, name, recipe.get("surfaces", {}), report)
    for pack_name in recipe.get("kits", []):
        pack = PK.get(pack_name)
        if not pack.installed():
            report["skipped"].append("kits of %s (not installed)" % pack_name)
            continue
        for kit in pack.kits:
            got = P.build_kit_slots(pal, pack, kit)
            if got:
                report.setdefault("kits", {})[kit.style] = got
            else:
                report["skipped"].append("kit %s (no ground floor or window floor found)" % kit.style)
    for pack_name in recipe.get("buildings", []):
        pack = PK.get(pack_name)
        if not pack.installed():
            report["skipped"].append("buildings of %s (not installed)" % pack_name)
            continue
        for slot in ("Building.Downtown", "Building.CornerNeg", "Building.CornerPos"):
            s = _slot(pal, slot)
            s.set_editor_property("meshes", [])
            pal.set_slot(unreal.Name(slot), s)
        report.setdefault("buildings", {}).update(P.build_building_slots(pal, pack))
    if recipe.get("puddles"):
        from . import surfaces as SF
        # built once (it is plugin content): rebuilding it every time would re-save it for nothing
        path = SF.DIR + "/M_RammsPuddle"
        puddle = unreal.load_asset(path) if H.EAL.does_asset_exist(path) else SF.build_puddle_decal()
        s = _slot(pal, "Decal.Puddle")
        have = [m for m in s.get_editor_property("materials") if m is not None]
        if puddle.get_path_name() not in {m.get_path_name() for m in have}:
            s.set_editor_property("materials", have + [puddle])
        if s.get_editor_property("material") is None:
            s.set_editor_property("material", puddle)
        pal.set_slot(unreal.Name("Decal.Puddle"), s)
    H.save(pal)
    H.log("recipe %s -> %s: %d layers, surfaces %s; skipped %s" % (name, path, len(report["layers"]), sorted(report["surfaces"]), report["skipped"]))
    return pal, report


def describe(pal):
    """{slot: sorted mesh / material paths} of a palette: for comparing two builds."""
    out = {}
    for k, s in pal.get_editor_property("slots").items():
        meshes = sorted(e.get_editor_property("mesh").get_path_name() for e in s.get_editor_property("meshes") if e.get_editor_property("mesh"))
        mats = sorted(m.get_path_name() for m in s.get_editor_property("materials") if m is not None)
        single = s.get_editor_property("material")
        out[str(k)] = {"meshes": meshes, "materials": mats, "material": single.get_path_name() if single else None}
    return out
