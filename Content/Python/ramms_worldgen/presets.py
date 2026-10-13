"""The accessibility profile and the example presets, as data assets."""

import unreal

from . import _helpers as H

PROFILE_ADA = H.PROFILES + "/PROF_ADA"
STREETS_GRAPH = H.GRAPHS + "/PCG_Streets"
INDOOR_GRAPH = H.GRAPHS + "/PCG_Indoor"

# name -> (scene kind, graph path, description, params)
PRESETS = {
    "PRE_Downtown": (
        unreal.RammsSceneKind.DOWNTOWN, STREETS_GRAPH,
        "Dense downtown: small blocks, wide sidewalks with a furniture zone, mid-rise shells with "
        "level, stepped and ramped entrances, street trees, cafe seating, A-frames and scooters.",
        dict(SiteSizeX=14000.0, SiteSizeY=14000.0, NonCompliance=0.15,
             BlockSizeMin=4000.0, BlockSizeMax=7000.0, RoadWidth=800.0, SidewalkWidth=360.0, FurnitureZoneWidth=110.0,
             StreetJitter=0.2, StreetDropout=0.1, MidblockCrossingChance=0.3, CrosswalkWidth=300.0,
             LotWidthMin=1200.0, LotWidthMax=2400.0, SetbackMin=0.0, SetbackMax=0.0, FloorsMin=3, FloorsMax=9,
             FloorHeight=380.0, LotCoverage=0.95, EntranceStepChance=0.35, EntranceRampChance=0.6, WindowSpacing=300.0,
             PropDensity=7.0, HazardDensity=4.0, TreeSpacing=900.0, HazardIntrusionChance=0.5, ParkFraction=0.08,
             ParkingDensity=0.6),
    ),
    "PRE_Suburban": (
        unreal.RammsSceneKind.SUBURBAN, STREETS_GRAPH,
        "Residential streets: long blocks, narrow sidewalks with no furniture zone, set-back houses with "
        "stepped porches, driveways, lawns, and a third of the blocks as parks.",
        dict(SiteSizeX=16000.0, SiteSizeY=16000.0, NonCompliance=0.2,
             BlockSizeMin=7000.0, BlockSizeMax=10000.0, RoadWidth=600.0, SidewalkWidth=150.0, FurnitureZoneWidth=0.0,
             StreetJitter=0.3, StreetDropout=0.2, MidblockCrossingChance=0.1, CrosswalkWidth=250.0,
             LotWidthMin=1800.0, LotWidthMax=3000.0, SetbackMin=400.0, SetbackMax=900.0, FloorsMin=1, FloorsMax=2,
             FloorHeight=320.0, LotCoverage=0.6, EntranceStepChance=0.7, EntranceRampChance=0.3, WindowSpacing=250.0,
             PropDensity=1.5, HazardDensity=1.5, TreeSpacing=0.0, HazardIntrusionChance=0.6,
             ParkFraction=0.3, LawnLots=1.0, YardTreeChance=0.85, DrivewayChance=0.8, FoliageDensity=0.8, ParkingDensity=0.35,
             PathWidth=180.0),
    ),
    "PRE_Park": (
        unreal.RammsSceneKind.PARK, STREETS_GRAPH,
        "A single park block ringed by sidewalk: loop and cross paths, lawn trees and shrubs, benches, "
        "and the occasional cobbled segment.",
        dict(SiteSizeX=12000.0, SiteSizeY=12000.0, NonCompliance=0.15,
             BlockSizeMin=10000.0, BlockSizeMax=10500.0, RoadWidth=700.0, SidewalkWidth=240.0, FurnitureZoneWidth=80.0,
             StreetJitter=0.0, StreetDropout=0.0, MidblockCrossingChance=0.5, PropDensity=4.0, HazardDensity=1.0,
             TreeSpacing=1200.0, ParkFraction=1.0, FoliageDensity=1.0, PathWidth=200.0, ParkingDensity=0.3),
    ),
    "PRE_Indoor_Home": (
        unreal.RammsSceneKind.INDOOR, INDOOR_GRAPH,
        "A flat: open-plan BSP rooms most of the time, furniture against the walls and around tables, "
        "a few narrow doors and raised thresholds.",
        dict(SiteSizeX=1800.0, SiteSizeY=1300.0, NonCompliance=0.2,
             RoomSizeMin=350.0, RoomSizeMax=650.0, CorridorWidth=150.0, CorridorChance=0.3, WallThickness=15.0,
             WallHeight=270.0, DoorWidthMean=84.0, DoorWidthStd=8.0, ThresholdChance=0.35, FurnitureDensity=0.6,
             HazardIntrusionChance=0.5),
    ),
    "PRE_Indoor_Office": (
        unreal.RammsSceneKind.INDOOR, INDOOR_GRAPH,
        "An office floor: a central corridor with rooms off it, wider doors, desks and chairs, sparser furniture.",
        dict(SiteSizeX=3200.0, SiteSizeY=1900.0, NonCompliance=0.1,
             RoomSizeMin=400.0, RoomSizeMax=750.0, CorridorWidth=210.0, CorridorChance=0.95, WallThickness=15.0,
             WallHeight=300.0, DoorWidthMean=92.0, DoorWidthStd=6.0, ThresholdChance=0.15, FurnitureDensity=0.45,
             HazardIntrusionChance=0.3, OfficeFraction=0.9),
    ),
}


def build_profile(path=PROFILE_ADA):
    prof, _ = H.get_or_create(path, unreal.RammsAccessibilityProfile)
    # Defaults in the class are the 2010 ADA numbers; the asset exists so a project can tune them.
    H.save(prof)
    return prof


def build_presets(palette, profile=None):
    """Create or refresh every preset. Graph references are soft, so graphs may be built later."""
    profile = profile or build_profile()
    out = {}
    for name, (kind, graph_path, desc, params) in PRESETS.items():
        preset, _ = H.get_or_create(H.PRESETS + "/" + name, unreal.RammsWorldPreset)
        preset.set_editor_property("scene_kind", kind)
        preset.set_editor_property("description", desc)
        p = unreal.RammsWorldGenParams()
        p.set_editor_property("palette", palette)
        for k, v in params.items():
            # UPROPERTY names are PascalCase; the Python reflection uses snake_case.
            p.set_editor_property(_snake(k), v)
        preset.set_editor_property("params", p)
        preset.set_editor_property("palette", palette)
        preset.set_editor_property("profile", profile)
        preset.set_editor_property("graph", unreal.load_asset(graph_path) if H.EAL.does_asset_exist(graph_path) else None)
        H.save(preset)
        out[name] = preset
    H.log("presets: %s" % ", ".join(out))
    return out


def _snake(name):
    out = []
    for i, ch in enumerate(name):
        if ch.isupper() and i > 0 and (not name[i - 1].isupper() or (i + 1 < len(name) and name[i + 1].islower())):
            out.append("_")
        out.append(ch.lower())
    return "".join(out)


def preset_names():
    return list(PRESETS)


def load_preset(name_or_path):
    path = name_or_path if name_or_path.startswith("/") else H.PRESETS + "/" + name_or_path
    if not H.EAL.does_asset_exist(path):
        raise RuntimeError("no preset at %s; known: %s" % (path, preset_names()))
    return H.EAL.load_asset(path)
