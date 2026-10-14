"""Build the default palette from the Fab packs the project already has.

Slots are looked up by asset *name* through the asset registry, so a pack can
be moved under /Game/Fab without touching this file; a missing asset just
leaves its slot empty and is reported. Footprints and heights are read from
the mesh bounds so placement can keep clearances without loading meshes at
generation time.
"""

import unreal

from . import _helpers as H

DEFAULT_PALETTE = H.PALETTES + "/PAL_Default"
ENGINE_MAT = "/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"

# slot -> list of static mesh asset names (any that exist are used, weight 1). Names come from the
# packs the project carries (MoStreetProps, Megascans, FreeFurniturePack, RestaurantScene,
# AmericanCityPacks, TownShops, CityBeachStrip, SuburbNeighborhoodHousePack); a missing pack just
# leaves fewer variants in a slot.
MESH_SLOTS = {
    "Curb": ["semf4_LOD0_TIER2_000", "semf4_LOD0_TIER2_001", "sepxW_LOD0_TIER2_000", "sepxW_LOD0_TIER2_001"],
    "SidewalkCorner": ["uiksdjydw_tier_2"],
    "Bench": ["SM_Bench01", "SM_Bench02", "SM_Bench03", "SM_bench_outdoor_01_rm"],
    "Bollard": ["SM_Bollard01B", "SM_Bollard01P", "SM_electric_bollard_metal"],
    "BusStop": ["SM_BusStop01", "SM_BusStop02", "SM_BusStation"],
    "StreetLight": ["SM_LampPost", "SM_Lamp_Street_High", "SM_StreetLight01", "SM_StreetLight02", "SM_light_pole_baytrail_01"],
    "Railing": ["SM_StreetRailing01", "SM_StreetRailing02"],
    "TrashBin": ["SM_streetBin", "SM_TrashCanClosed", "SM_Bin_01", "SM_Trashbin02", "SM_Trashbin01B", "SM_Trash_Container", "SM_garbage_bin_concrete_RM_01"],
    "InfoBoard": ["SM_InfoTable01", "SM_InfoTable02", "SM_InfoTable03"],
    "Hydrant": ["SM_hydrant", "SM_Fire_Hydrant"],
    "ParkingMeter": ["SM_ParkingMeter"],
    "NewspaperBox": ["SM_NewspaperBox"],
    "Mailbox": ["SM_Mailbox", "SM_Mailbox_01"],
    "StreetSign": ["SM_Sign_StopSign", "SM_Sign_NoParking01", "SM_Sign_SpeedLimit25", "SM_Sign_OneWay01", "SM_Sign_Residential", "SM_Stop_Sign", "SM_Street_Sign"],
    "UtilityPole": ["SM_PowerPole_Solo", "SM_UtilityPole"],
    "Car": ["SM_Car", "SM_veh07_car_sedan_parked", "SM_veh_03_car_parked", "SM_veh_05_car_suv_parked", "SM_veh_04_truck_parked"],
    "Flowerbed": ["SM_Flowerbed_Round", "SM_WoodPlanter01", "SM_WoodPlanter03", "SM_Pot04", "SM_Pot05"],
    "Fence": ["SM_Fence_Frontyard_Tile_Left_2x", "SM_Fence_Frontyard_Tile_Right_2x"],
    "Stairs": ["stairs_street"],
    "RoofUnit": ["SM_AC_Box", "SM_AC_Exaust", "SM_ac_01", "SM_ac_02"],
    # foliage
    "Tree": ["SM_Tree1", "SM_Tree2", "SM_Tree3", "SM_BeechTree02", "SM_BeechTree03"],
    "TreeSmall": ["SM_MyrtleTree01", "SM_MyrtleTree02", "SM_PrivetTree01"],
    "Shrub": ["SM_Shrub01", "SM_Shrub01_b", "SM_Plant_Bush_A", "SM_Plant_Bush_B", "SM_Plant_Treebush", "SM_Hedge_Sphere", "SM_Plant_FlowersRedBush", "SM_Plant_FlowersWhiteBush"],
    "Hedge": ["SM_Hedge02_4m_Half", "SM_Plant_HedgeBush"],
    # buildings
    "House": ["SM_Background_House_A", "SM_Background_House_B"],
    # hazards: things that end up in the access route
    "Hazard.Upheaval": ["adoquines_levantados_alcorque"],
    "Hazard.BinOut": ["SM_TrashCan_WithTrash_01", "SM_TrashCan_WithTrash_02", "SM_BinOpenedWithTrash", "SM_Trashbag", "SM_Trashbin01"],
    "Hazard.Barrier": ["SM_metallicBarrier", "SM_Roadblock_Wood_Roadblock_Woo", "SM_Roadblock_Concrete", "SM_StreetRailing01", "SM_barricade_metal_rm_01", "SM_construction_fence_orange_rm_01"],
    "Hazard.Cone": ["SM_RoadConstruction_cone_01", "SM_RoadConstruction_cone_02", "SM_RoadConstruction_cone_03"],
    "Hazard.AFrame": ["SM_InfoTable01", "SM_InfoTable02"],
    "Hazard.Cafe": ["SM_RestaurantTable", "SM_OutsideTable", "SM_GardenTable", "viejdi1_tier_2", "SM_Table"],
    "Hazard.CafeChair": ["SM_OutsideChair", "SM_GardenChair", "SM_PlasticChair", "SM_Chair"],
    "Hazard.Scooter": ["SM_Scooter"],
    "Hazard.Bike": ["SM_Bike"],
    "Hazard.Dumpster": ["SM_Dumpster", "SM_dumpster"],
    "Hazard.Box": ["SM_Cardboard_Closed_2", "SM_Cardboard_Closed_3", "SM_CardboardBox_01", "SM_CardboardBoxOpened_02"],
    # indoor
    "Furniture.Table": ["SM_Kitchen_table_large", "SM_Kitchen_table_rectangle", "SM_Kitchen_table_small", "SM_Table_dining", "SM_KitchenTable_01a", "SM_Table", "SM_Classic_Round_Table", "SM_Modern_Table"],
    "Furniture.Chair": ["SM_Kitchen_chair_fabric", "SM_Kitchen_chair_wooden", "SM_Chair_dining", "SM_KitchenChair_01a", "SM_Chair", "SM_Classic_chair", "SM_Modern_Chair_1", "SM_Old_Chair"],
    "Furniture.Sofa": ["SM_Couch_3seat", "SM_Couch_2seat", "SM_Couch", "SM_Sofa_01a", "SM_Sofa_02a", "SM_Classic_Leather_Sofa", "SM_Classic_Leather_sofa_2", "SM_Modern_Sofa_2"],
    "Furniture.Armchair": ["SM_Couch_1seat", "SM_Chair_arm", "SM_LoungeChair_01a", "SM_Chair_leather", "SM_Classic_Leather_Armchair", "SM_Modern_Armchair"],
    "Furniture.CoffeeTable": ["SM_Coffee_Table", "SM_Sidetable_small", "SM_Sidetable_C", "SM_CoffeeTable_01a"],
    "Furniture.Bed": ["SM_Bed_queen", "SM_Bed_small", "SM_Bed_king", "SM_Bed_single_person", "SM_Modern_Bed_1", "SM_Modern_Bed_2"],
    "Furniture.BedTable": ["SM_Sidetable_small", "SM_Nightstand_A", "SM_Nightstand_B", "SM_Bed_Table"],
    "Furniture.Cupboard": ["SM_Classic_Cupboard", "SM_Modern_Cupboard", "SM_Dresser_C", "SM_Dresser_large_A", "SM_Shelf_standing_A", "SM_Commode", "SM_Classic_Commode", "SM_Garage_shelf"],
    "Furniture.Bookshelf": ["SM_Book_set_bookshelf_A", "SM_Book_set_bookshelf_B", "SM_Bookshelf", "SM_BookShelf_01a"],
    "Furniture.Fireplace": ["SM_Fireplace_Single"],
    "Furniture.TVStand": ["SM_Sidetable_dark_large", "SM_Tv_stand", "SM_Commode", "SM_Classic_Commode_2"],
    "Furniture.TV": ["SM_TV_standing", "SM_TV", "SM_TVFlat_01a"],
    "Furniture.Desk": ["SM_Desk_office", "SM_Workdesk", "SM_Old_Table", "SM_Classic_table", "SM_Modern_Table"],
    "Furniture.OfficeChair": ["SM_Modern_Office_chair", "SM_Chair_office", "SM_OfficeChair_01a", "SM_OfficeChair02a"],
    "Furniture.Booth": ["SM_Booth"],
    "Furniture.Plant": ["SM_Plant_interior_yucca", "SM_Plant_interior_aglaonema", "SM_Plant_A", "SM_Plant_01a", "SM_PlantPot"],
    "Furniture.Lamp": ["SM_Lamp_table_A", "SM_Lamp_table_B", "SM_TableLamp_01a", "SM_Table_Lamp", "SM_Bed_lamp_3"],
    "Furniture.FloorLamp": ["SM_Lamp_standing"],
    "Furniture.Toilet": ["SM_Toilet"],
    "Furniture.Bathtub": ["SM_To_bathtub", "SM_Bathtub"],
    "Furniture.Appliance": ["SM_Washingmachine", "SM_Refrigerator_04a", "SM_Fridge_01a"],
    "Furniture.KitchenLow": ["SM_Kitcabinet_low_1x_A", "SM_Kitcabinet_low_1x_B", "SM_Kitcabinet_low_1halfx_A"],
    "Furniture.KitchenSink": ["SM_Kitcabinet_low_1halfx_sink"],
    "Furniture.KitchenStove": ["SM_Kitcabinet_low_1x_stove"],
    "Furniture.KitchenDishwasher": ["SM_Kitcabinet_low_1x_dishwasher", "SM_Dishwasher_01a"],
    "Furniture.KitchenUpper": ["SM_Kitcabinet_upper_1x_A", "SM_Kitcabinet_upper_1x_B"],
    "Furniture.KitchenTall": ["SM_Kitcabinet_full_fridge", "SM_Kitcabinet_full_cabinet", "SM_Fridge_01a", "SM_Refrigerator_04a", "SM_Refridgerator_05a"],
    "Furniture.OfficeStorage": ["SM_FilingCabinet01a", "SM_FilingCabinet01b", "SM_FilingCabinet01e", "SM_MetalShelf_100a", "SM_Shelf_standing_A", "SM_BookShelf_01a"],
    "Furniture.OfficeProp": ["SM_Printer_02a", "SM_WaterCooler_01b", "SM_VendingMachine_01a", "SM_StepLadder_01a", "SM_PaperBoxes_01a"],
    "Furniture.Whiteboard": ["SM_WhiteBoard_01a", "SM_NoticeBoard_01a", "SM_NoticeBoard_01e"],
    "Furniture.Picture": ["SM_Painting_A", "SM_Painting_B", "SM_Painting_C", "SM_Painting_E", "SM_Painting_F", "SM_Painting_large_A", "SM_Painting_wide_A", "SM_Mirror", "SM_Picture", "SM_Classic_Picture"],
    "Furniture.Clutter": ["SM_UtilityCart", "SM_Garage_plastic_box", "SM_Cardboard_Closed_3", "SM_To_laundrybasket", "SM_Old_Chair", "SM_WaterCooler_01b", "SM_Printer_02a", "SM_ShippingBoxes_01b", "SM_FilingCabinet01a", "SM_Laundrybasket"],
}

# Real-world heights (cm) a slot's meshes are normalised to when a pack models them at another
# scale (the FreeFurniturePack seating is ~15% large; a couple of chairs are armchair-sized).
# A mesh within 12% of the expected height is left alone; otherwise it is scaled uniformly.
SLOT_HEIGHTS = {
    "Furniture.Sofa": 86, "Furniture.Armchair": 90, "Furniture.Chair": 92, "Furniture.OfficeChair": 100,
    "Furniture.Table": 75, "Furniture.Desk": 75, "Furniture.CoffeeTable": 45, "Furniture.BedTable": 55,
    "Furniture.Bed": 95, "Furniture.Cupboard": 190, "Furniture.Bookshelf": 190, "Furniture.TVStand": 60,
    "Furniture.TV": 70, "Furniture.Toilet": 76, "Furniture.Bathtub": 60, "Furniture.Plant": 120,
    "Furniture.Lamp": 50, "Furniture.FloorLamp": 150, "Furniture.Booth": 110, "Furniture.Fireplace": 240,
    "Furniture.Appliance": 88, "Furniture.KitchenLow": 92, "Furniture.KitchenSink": 92, "Furniture.KitchenStove": 92,
    "Furniture.KitchenDishwasher": 92, "Furniture.KitchenUpper": 95, "Furniture.KitchenTall": 200,
    "Bench": 85, "TrashBin": 100, "Hydrant": 80, "ParkingMeter": 140, "NewspaperBox": 120, "Mailbox": 120,
    "Hazard.BinOut": 100, "Hazard.Cafe": 75, "Hazard.CafeChair": 90, "Hazard.Scooter": 115, "Hazard.Bike": 105,
    "Hazard.Cone": 75, "Hazard.Dumpster": 150, "Car": 150,
}
# Slots whose meshes keep their native size whatever the height (poles, trees, buildings, hedges).
NO_NORMALISE = {"Tree", "TreeSmall", "Shrub", "Hedge", "House", "Car", "StreetLight", "UtilityPole", "StreetSign", "BusStop",
                "Stairs", "Curb", "SidewalkCorner", "Railing", "Hazard.Upheaval", "Hazard.Barrier", "RoofUnit",
                "Flowerbed", "Fence", "Furniture.Picture", "Furniture.Clutter", "Furniture.OfficeProp", "Furniture.Whiteboard", "Furniture.OfficeStorage", "Hazard.Box", "Hazard.AFrame", "InfoBoard", "Bollard"}

# Meshes whose "front" the facing heuristic cannot tell (symmetric tops): yaw that turns the front to +X.
MESH_FACING = {
    "SM_TV": 0.0,
}

# Solid materials built from code when no pack provides them (name -> (rgb, roughness, metallic)).
SOLID_MATERIALS = {
    "M_RammsFoliage": ((0.07, 0.24, 0.05), 0.85, 0.0),
    "M_RammsTrunk": ((0.20, 0.13, 0.08), 0.9, 0.0),
    "M_RammsWarning": ((0.95, 0.70, 0.05), 0.75, 0.0),
    "M_RammsMarkingWhite": ((0.85, 0.85, 0.82), 0.6, 0.0),
    "M_RammsMarkingYellow": ((0.90, 0.72, 0.08), 0.6, 0.0),
    "M_RammsRoof": ((0.22, 0.22, 0.24), 0.95, 0.0),
    "M_RammsStorefront": ((0.10, 0.10, 0.12), 0.45, 0.6),
    "M_RammsTrim": ((0.62, 0.60, 0.56), 0.8, 0.0),
}
MATERIALS_DIR = "/Game/WorldGen/Materials"

# slot -> list of material (instance) asset names, first that exists wins
MATERIAL_SLOTS = {
    "Mat.Asphalt": ["MI_rh0ribp0", "MI_sjfnbeaa", "MI_vizhdcz"],
    "Mat.Sidewalk": ["MI_ugxjcdpn", "MI_vmcxajwfw"],
    "Mat.Concrete": ["MI_ugkjbgplw", "MI_vmcxajwfw"],
    "Mat.Cobble": ["MI_CobbleStone", "MI_tbjjai0s"],
    "Mat.Facade": ["MI_vizcegj"],
    "Mat.FacadeAlt": ["MI_ugtkdffg", "MI_vi4idbm"],
    "Mat.Glass": ["MI_Glass", "M_Glass", "MI_AdvancedGlass", "M_AdvancedGlass"],
    "Mat.Paint": ["M_RammsMarkingWhite"],
    "Mat.Marking": ["M_RammsMarkingWhite"],
    "Mat.MarkingYellow": ["M_RammsMarkingYellow"],
    "Mat.Warning": ["M_RammsWarning"],
    "Mat.Grass": ["MI_SFPark_grass_01_RM", "MI_GrassPatchy", "MI_pjwey0", "MI_tkynejer"],
    "Mat.Lawn": ["MI_SFPark_grass_01_RM", "MI_GrassPatchy", "MI_tkynejer", "MI_pjwey0"],
    "Mat.Path": ["MI_vmcxajwfw", "MI_ugkjbgplw"],
    "Mat.Foliage": ["M_RammsFoliage"],
    "Mat.Trunk": ["M_RammsTrunk"],
    "Mat.Metal": ["MI_teeaccoc"],
    "Mat.Trim": ["M_RammsTrim"],
    "Mat.Roof": ["M_RammsRoof"],
    "Mat.Storefront": ["M_RammsStorefront"],
    "Mat.Floor.Wood": ["MI_ucklejqdy", "MI_ufeleiln", "MI_ugcwcevaw"],
    "Mat.Floor.Tile": ["MI_wfribhq", "MI_uenobapfw"],
    "Mat.Floor.Carpet": ["MI_umonbbqdy"],
    "Mat.Wall.Interior": ["MI_qj2luvs0", "MI_umridamn"],
    "Mat.Ceiling": ["MI_ufgnagwew", "MI_ugtmehro"],
    "Mat.Door": ["MI_vlznbajfw", "MI_ugcwcevaw"],
}

ACTOR_SLOTS = {
    "DoorActor": "/Game/ada_door/BP_Moving_Door.BP_Moving_Door_C",
}

# slot -> blueprint classes whose child-actor static meshes are captured as one composite entry
# (Residential Houses - Modular Pack assembles each house from ~300 child actors; spawned as ISM
# instances they cost nothing). The front is taken from the porch / front door parts.
COMPOSITE_SLOTS = {
    "House": ["/Game/ResidentialHouses/Blueprints/Houses/NoInteriors/House%02d.House%02d_C" % (n, n) for n in range(1, 13)],
}
COMPOSITE_FRONT_HINTS = ("Porch", "FrontDoor", "Front_Door", "SM_Door", "Steps")

# --- facade module kits (AmericanCityPacks) ------------------------------------------------
# Module meshes are found by name pattern: SM_<style>_GroundFloor_L<n>T<t>[_door|_noDoor]_piece,
# SM_<style>_windows[Top]_L<n>T<t>_piece, SM_<style>_corner_L1T<t>_piece, SM_01_top_L<n> ... Each
# becomes an entry of "Facade.<style>.<Part>" with its material slot categories recorded, so the
# massing node can tile a facade and override every slot from one set of picks per building.
import re as _re

FACADE_PATTERNS = [
    # (part, regex with a 'style' group; corners are only the L1 pieces)
    ("GFDoor", _re.compile(r"^SM_(?P<style>[a-z]+\d+)_[Gg]roundFloor_L\d+T\d+_door_piece$")),
    ("GF", _re.compile(r"^SM_(?P<style>[a-z]+\d+)_[Gg]roundFloor_L\d+T\d+(_noDoor)?_piece$")),
    ("GFCorner", _re.compile(r"^SM_(?P<style>[a-z]+\d+)_GroundFloorCorner_L1T\d+_piece$")),
    ("WinTop", _re.compile(r"^SM_(?P<style>[a-z]+\d+)_windowsTop_L\d+T\d+_piece$")),
    ("Win", _re.compile(r"^SM_(?P<style>[a-z]+\d+)_windows_L\d+T\d+_piece$")),
    ("WinCorner", _re.compile(r"^SM_(?P<style>[a-z]+\d+)_corner_L1T\d+_piece$")),
]
FACADE_GENERIC = {
    "Facade.WinCorner": _re.compile(r"^SM_01_corner_L1T\d+_piece$"),
    "Facade.Top": _re.compile(r"^SM_01_top_L\d$"),
    "Facade.TopCorner": _re.compile(r"^SM_01_top_corner_L1$"),
    "Facade.TallTop": _re.compile(r"^SM_01_TallTop_L\d$"),
    "Facade.TallTopCorner": _re.compile(r"^SM_01_TallTop_Corner_L1$"),
}
# mesh material slot name (lower-cased) -> category; a slot not listed falls back to the bricks.
FACADE_SLOT_CATEGORIES = {
    "facade": "Bricks", "facade2": "Bricks", "facade3": "Bricks2",
    "glass": "Windows", "glassnocurtains": "WindowsNoCurtain",
    "shopglass": "Shop", "glassfake": "Shop",
    "window": "Frame", "windows": "Frame", "black": "Frame", "framewindow": "Frame",
    "concrete": "Concrete", "ornaments": "Ornament", "border": "Border",
    "metal": "Metal", "metallic": "Metal", "metallic2": "Metal", "roof": "Roof", "door": "Door",
}
# category -> (asset folder, name filter) for the material sets
FACADE_MATERIAL_SETS = {
    "Bricks": ("/Game/AmericanCityPacks/Materials/Bricks", _re.compile(r"^MI_(bricks_|BrownBricks)")),
    # ground-floor panels (slot facade3): the regular bricks read at the right size; the pack's own
    # "F" bricks come out as 4x-size blocks on these UVs
    "Bricks2": ("/Game/AmericanCityPacks/Materials/Bricks", _re.compile(r"^MI_(bricks_|BrownBricks)")),
    # mostly bare glass; the curtained variants light up white in full sun
    "Windows": ("/Game/AmericanCityPacks/Materials/Windows", _re.compile(r"^MI_(3?windows_\d+|3?windows_NoCurtain_\d+|SbWindows_NoCurtain_\d+)$")),
    "WindowsNoCurtain": ("/Game/AmericanCityPacks/Materials/Windows/NoCurtains", _re.compile(r"^MI_(3?windows|SbWindows)_NoCurtain_\d+$")),
    # the pack's fake-interior shop glass renders as an opaque brick box here (no reflection capture, no VT):
    # plain glass over the dark shell reads as a shop front instead
    "Shop": ("/Game/AmericanCityPacks/Materials", _re.compile(r"^MI_Glass_0\d$")),
    "Frame": ("/Game/AmericanCityPacks/Materials/Simple", _re.compile(r"^MI_simple_")),
    "Concrete": ("/Game/AmericanCityPacks/Materials/Concrete", _re.compile(r"^MI_concrete_")),
    "Ornament": ("/Game/AmericanCityPacks/Materials/Ornaments", _re.compile(r"^MI_ornament_")),
    "Border": ("/Game/AmericanCityPacks/Materials/Ornaments", _re.compile(r"^MI_Ornaments_border_")),
    "Metal": ("/Game/AmericanCityPacks/Materials/Metal", _re.compile(r"^MI_metal_")),
    "Roof": ("/Game/AmericanCityPacks/Materials/Roof", _re.compile(r"^MI_RoofPlaster_")),
    "Door": ("/Game/AmericanCityPacks/Materials/Props", _re.compile(r"^MI_Door")),
}


def _module_entry(mesh):
    """Palette entry for a facade module: native pivot (start of run, base on the ground), no facing fix."""
    bounds = mesh.get_bounds()
    ext = bounds.box_extent
    e = unreal.RammsPaletteMesh()
    e.set_editor_property("mesh", mesh)
    e.set_editor_property("weight", 1.0)
    e.set_editor_property("scale", 1.0)
    e.set_editor_property("footprint", unreal.Vector2D(max(ext.x * 2.0, 1.0), max(ext.y * 2.0, 1.0)))
    e.set_editor_property("height", max(ext.z * 2.0, 1.0))
    e.set_editor_property("rotation_offset", unreal.Rotator(roll=0.0, pitch=0.0, yaw=0.0))
    e.set_editor_property("location_offset", unreal.Vector(0.0, 0.0, 0.0))
    cats = []
    for sm in mesh.get_editor_property("static_materials"):
        cats.append(FACADE_SLOT_CATEGORIES.get(str(sm.material_slot_name).lower(), "Default"))
    e.set_editor_property("material_slots", cats)
    return e


def build_facade_slots(pal, report):
    """Fill Facade.* slots from whatever facade kits the registry holds."""
    reg = unreal.AssetRegistryHelpers.get_asset_registry()
    counts = {}
    for ad in reg.get_assets_by_path("/Game/AmericanCityPacks/Geometry", recursive=True):
        if str(ad.asset_class_path.asset_name) != "StaticMesh":
            continue
        name = str(ad.asset_name)
        if name.endswith("_INT"):
            continue
        slot = None
        for part, rx in FACADE_PATTERNS:
            m = rx.match(name)
            if m:
                slot = "Facade.%s.%s" % (m.group("style"), part)
                break
        if slot is None:
            for generic, rx in FACADE_GENERIC.items():
                if rx.match(name):
                    slot = generic
                    break
        if slot is None:
            continue
        mesh = ad.get_asset()
        if mesh is None:
            continue
        pal.add_mesh(slot, _module_entry(mesh))
        counts[slot] = counts.get(slot, 0) + 1
    for cat, (folder, rx) in FACADE_MATERIAL_SETS.items():
        n = 0
        for ad in reg.get_assets_by_path(folder, recursive=True):
            if str(ad.asset_class_path.asset_name) not in ("MaterialInstanceConstant", "Material"):
                continue
            if not rx.match(str(ad.asset_name)):
                continue
            mat = ad.get_asset()
            if mat is None:
                continue
            pal.add_material("Facade.Mat." + cat, mat)
            n += 1
        counts["Facade.Mat." + cat] = n
    report["facade"] = counts
    return counts


def _registry_index():
    """name -> asset data for every StaticMesh and MaterialInterface under /Game."""
    reg = unreal.AssetRegistryHelpers.get_asset_registry()
    index = {}
    for ad in reg.get_assets_by_path("/Game", recursive=True):
        cls = str(ad.asset_class_path.asset_name)
        if cls in ("StaticMesh", "MaterialInstanceConstant", "Material"):
            index.setdefault(str(ad.asset_name), []).append(ad)
    return index


def _vertex_positions(mesh):
    """Vertex positions of LOD0 in mesh space via GeometryScript, or None when that is unavailable."""
    try:
        dm = unreal.DynamicMesh()
        opts = unreal.GeometryScriptCopyMeshFromAssetOptions()
        lod = unreal.GeometryScriptMeshReadLOD()
        res = unreal.GeometryScript_AssetUtils.copy_mesh_from_static_mesh(mesh, dm, opts, lod)
        dm = res[0] if isinstance(res, tuple) else res
        res = unreal.GeometryScript_MeshQueries.get_all_vertex_positions(dm, False)
        # Python returns the out-params: (target_mesh, position_list, has_gaps) or (position_list, has_gaps).
        vlist = next(v for v in (res if isinstance(res, tuple) else (res,)) if isinstance(v, unreal.GeometryScriptVectorList))
        arr = unreal.GeometryScript_List.convert_vector_list_to_array(vlist)
        return [(v.x, v.y, v.z) for v in arr]
    except Exception as exc:  # noqa: BLE001 - any failure just means "no facing info"
        H.warn("facing: no vertices for %s (%s)" % (mesh.get_name(), exc))
        return None


LONG_AXIS_SLOTS = {"Car"}


def _facing_yaw(mesh, bounds, slot=""):
    """
    Yaw (degrees) that turns the mesh's front to +X. Seating, beds and toilets carry their mass high
    at the back (backrest, headboard, cistern), so the front is opposite the side where the upper
    vertices gather; symmetric pieces (tables, cupboards) get 0. Explicit MESH_FACING entries win.
    """
    name = mesh.get_name()
    if name in MESH_FACING:
        return MESH_FACING[name]
    if slot in LONG_AXIS_SLOTS:
        # vehicles: the long axis is the driving axis; which end is the nose does not matter for parking
        return 90.0 if bounds.box_extent.y > bounds.box_extent.x else 0.0
    verts = _vertex_positions(mesh)
    if not verts or len(verts) < 12:
        return 0.0
    ext = bounds.box_extent
    org = bounds.origin
    zlo = org.z - ext.z
    top = [v for v in verts if v[2] > zlo + ext.z * 2.0 * 0.5]
    if len(verts) - len(top) < 4 or len(top) < 4:
        return 0.0
    mx = sum(v[0] for v in top) / len(top) - org.x
    my = sum(v[1] for v in top) / len(top) - org.y
    nx = mx / max(ext.x, 1.0)
    ny = my / max(ext.y, 1.0)
    if max(abs(nx), abs(ny)) < 0.12:
        return 0.0
    # Back is where the high vertices are; front is the opposite axis direction.
    if abs(nx) >= abs(ny):
        front = (-1.0, 0.0) if nx > 0 else (1.0, 0.0)
    else:
        front = (0.0, -1.0) if ny > 0 else (0.0, 1.0)
    import math
    return -math.degrees(math.atan2(front[1], front[0]))


def _mesh_entry(mesh, slot=""):
    import math
    bounds = mesh.get_bounds()
    ext = bounds.box_extent
    origin = bounds.origin
    yaw = _facing_yaw(mesh, bounds, slot)
    e = unreal.RammsPaletteMesh()
    e.set_editor_property("mesh", mesh)
    e.set_editor_property("weight", 1.0)
    scale = 1.0
    expected = SLOT_HEIGHTS.get(slot)
    if expected and slot not in NO_NORMALISE and ext.z > 1.0:
        ratio = expected / (ext.z * 2.0)
        if abs(ratio - 1.0) > 0.12:
            scale = max(0.5, min(1.6, ratio))
    e.set_editor_property("scale", scale)
    ext = unreal.Vector(ext.x * scale, ext.y * scale, ext.z * scale)
    origin = unreal.Vector(origin.x * scale, origin.y * scale, origin.z * scale)
    # Footprint and pivot offset are stored in the facing frame (front = +X), so a node can treat every
    # mesh as "depth fx along its front, width fy across" and set the yaw it wants.
    swap = abs(abs(yaw) % 180.0 - 90.0) < 1.0
    fx, fy = (ext.y, ext.x) if swap else (ext.x, ext.y)
    e.set_editor_property("footprint", unreal.Vector2D(max(fx * 2.0, 1.0), max(fy * 2.0, 1.0)))
    e.set_editor_property("height", max(ext.z * 2.0, 1.0))
    e.set_editor_property("rotation_offset", unreal.Rotator(roll=0.0, pitch=0.0, yaw=yaw))
    # Put the mesh's bounds centre on the requested point and its lowest point on the ground.
    r = math.radians(yaw)
    ox = origin.x * math.cos(r) - origin.y * math.sin(r)
    oy = origin.x * math.sin(r) + origin.y * math.cos(r)
    base_z = origin.z - ext.z
    e.set_editor_property("location_offset", unreal.Vector(-ox, -oy, -base_z))
    return e


def _composite_entry(cls_path, slot=""):
    """Spawn the blueprint once, capture every child static mesh as a part, measure and face it."""
    import math
    cls = unreal.load_class(None, cls_path)
    if cls is None:
        return None
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    base = unreal.Vector(0.0, 0.0, 50000.0)
    actor = eas.spawn_actor_from_class(cls, base)
    if actor is None:
        return None
    parts = []
    lo = [1e9, 1e9, 1e9]
    hi = [-1e9, -1e9, -1e9]
    front = [0.0, 0.0, 0]
    try:
        owners = []
        for c in actor.get_components_by_class(unreal.ChildActorComponent):
            child = c.get_editor_property("child_actor")
            if child is not None:
                owners.append(child)
        owners.append(actor)
        # Stray parts far from the house (a road piece, a backdrop) would inflate the footprint: keep
        # the cluster around the median part position.
        measured = []
        for owner in owners:
            smcs = [c for c in owner.get_components_by_class(unreal.StaticMeshComponent) if c.get_editor_property("static_mesh") is not None]
            if not smcs:
                continue
            bo, be = owner.get_actor_bounds(False, False)
            if be.x < 1.0 and be.y < 1.0:
                continue
            measured.append((owner, smcs, bo, be))
        xs = sorted(m[2].x for m in measured); ys = sorted(m[2].y for m in measured)
        mx, my = (xs[len(xs) // 2], ys[len(ys) // 2]) if xs else (base.x, base.y)
        for owner, smcs, bo, be in measured:
            if abs(bo.x - mx) > 2500.0 or abs(bo.y - my) > 2500.0:
                continue
            lo = [min(lo[0], bo.x - be.x - base.x), min(lo[1], bo.y - be.y - base.y), min(lo[2], bo.z - be.z - base.z)]
            hi = [max(hi[0], bo.x + be.x - base.x), max(hi[1], bo.y + be.y - base.y), max(hi[2], bo.z + be.z - base.z)]
            for smc in smcs:
                mesh = smc.get_editor_property("static_mesh")
                loc = smc.get_world_location()
                rel = unreal.Transform(unreal.Vector(loc.x - base.x, loc.y - base.y, loc.z - base.z), smc.get_world_rotation(), smc.get_world_scale())
                part = unreal.RammsPalettePart()
                part.set_editor_property("mesh", mesh)
                part.set_editor_property("relative_transform", rel)
                mats = [smc.get_material(i) for i in range(smc.get_num_materials())]
                part.set_editor_property("materials", mats if mats and all(m is not None for m in mats) else [])
                parts.append(part)
                if any(h.lower() in mesh.get_name().lower() for h in COMPOSITE_FRONT_HINTS):
                    front[0] += bo.x - base.x; front[1] += bo.y - base.y; front[2] += 1
    finally:
        eas.destroy_actor(actor)
    if not parts or lo[0] > hi[0]:
        H.warn("composite %s: no parts" % cls_path)
        return None
    cx, cy = (lo[0] + hi[0]) * 0.5, (lo[1] + hi[1]) * 0.5
    ex, ey, ez = (hi[0] - lo[0]) * 0.5, (hi[1] - lo[1]) * 0.5, (hi[2] - lo[2]) * 0.5
    yaw = 0.0
    if front[2] > 0:
        fx, fy = front[0] / front[2] - cx, front[1] / front[2] - cy
        # snap to the nearest axis: the front faces the side the porch sits on
        if abs(fx) / max(ex, 1.0) >= abs(fy) / max(ey, 1.0):
            yaw = 0.0 if fx > 0 else 180.0
        else:
            yaw = -90.0 if fy > 0 else 90.0
    e = unreal.RammsPaletteMesh()
    e.set_editor_property("weight", 1.0)
    e.set_editor_property("scale", 1.0)
    e.set_editor_property("parts", parts)
    swap = abs(abs(yaw) % 180.0 - 90.0) < 1.0
    fx_, fy_ = (ey, ex) if swap else (ex, ey)
    e.set_editor_property("footprint", unreal.Vector2D(max(fx_ * 2.0, 1.0), max(fy_ * 2.0, 1.0)))
    e.set_editor_property("height", max(ez * 2.0, 1.0))
    e.set_editor_property("rotation_offset", unreal.Rotator(roll=0.0, pitch=0.0, yaw=yaw))
    r = math.radians(yaw)
    ox = cx * math.cos(r) - cy * math.sin(r)
    oy = cx * math.sin(r) + cy * math.cos(r)
    e.set_editor_property("location_offset", unreal.Vector(-ox, -oy, -lo[2]))
    H.log("composite %s: %d parts, footprint %.0fx%.0f h %.0f yaw %.0f" % (cls_path.rsplit("/", 1)[-1], len(parts), fx_ * 2, fy_ * 2, ez * 2, yaw))
    return e


def _ensure_solid_material(name, rgb, roughness, metallic):
    """A one-colour material asset under /Game/WorldGen/Materials, created once."""
    path = MATERIALS_DIR + "/" + name
    if H.EAL.does_asset_exist(path):
        return H.EAL.load_asset(path)
    H.ensure_dir(MATERIALS_DIR)
    mat = H.asset_tools().create_asset(name, MATERIALS_DIR, unreal.Material, unreal.MaterialFactoryNew())
    if mat is None:
        return None
    mel = unreal.MaterialEditingLibrary
    colour = mel.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, -350, 0)
    colour.set_editor_property("constant", unreal.LinearColor(rgb[0], rgb[1], rgb[2], 1.0))
    mel.connect_material_property(colour, "", unreal.MaterialProperty.MP_BASE_COLOR)
    rough = mel.create_material_expression(mat, unreal.MaterialExpressionConstant, -350, 200)
    rough.set_editor_property("r", roughness)
    mel.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
    metal = mel.create_material_expression(mat, unreal.MaterialExpressionConstant, -350, 300)
    metal.set_editor_property("r", metallic)
    mel.connect_material_property(metal, "", unreal.MaterialProperty.MP_METALLIC)
    mel.recompile_material(mat)
    H.save(mat)
    return mat


def build_default_palette(path=DEFAULT_PALETTE):
    """Create or refresh the default palette. Returns (palette, report dict)."""
    for name, (rgb, rough, metal) in SOLID_MATERIALS.items():
        _ensure_solid_material(name, rgb, rough, metal)
    index = _registry_index()
    pal, _ = H.get_or_create(path, unreal.RammsWorldPalette)
    pal.set_editor_property("slots", {})
    pal.set_editor_property("default_material", unreal.load_asset(ENGINE_MAT))
    report = {"meshes": {}, "materials": {}, "missing": []}

    for slot, names in MESH_SLOTS.items():
        found = []
        for n in names:
            for ad in index.get(n, []):
                if str(ad.asset_class_path.asset_name) != "StaticMesh":
                    continue
                mesh = ad.get_asset()
                if mesh is None:
                    continue
                pal.add_mesh(slot, _mesh_entry(mesh, slot))
                found.append(mesh.get_path_name())
                break
        report["meshes"][slot] = found
        if not found:
            report["missing"].append(slot)

    for slot, names in MATERIAL_SLOTS.items():
        chosen = None
        for n in names:
            if n in SOLID_MATERIALS and H.EAL.does_asset_exist(MATERIALS_DIR + "/" + n):
                chosen = H.EAL.load_asset(MATERIALS_DIR + "/" + n)
                break
            for ad in index.get(n, []):
                if str(ad.asset_class_path.asset_name) in ("MaterialInstanceConstant", "Material"):
                    chosen = ad.get_asset()
                    break
            if chosen:
                break
        if chosen is None:
            chosen = unreal.load_asset(ENGINE_MAT)
            report["missing"].append(slot)
        pal.set_material(slot, chosen)
        report["materials"][slot] = chosen.get_path_name()

    build_facade_slots(pal, report)

    report["composites"] = {}
    for slot, cls_paths in COMPOSITE_SLOTS.items():
        for cls_path in cls_paths:
            try:
                e = _composite_entry(cls_path, slot)
            except Exception as exc:  # noqa: BLE001
                H.warn("composite %s failed: %s" % (cls_path, exc))
                e = None
            if e is not None:
                pal.add_mesh(slot, e)
                report["composites"].setdefault(slot, []).append(cls_path)

    for slot, cls_path in ACTOR_SLOTS.items():
        cls = unreal.load_class(None, cls_path)
        if cls:
            pal.set_actor_class(slot, cls)
        else:
            report["missing"].append(slot)

    H.save(pal)
    missing = [str(n) for n in pal.missing_well_known_slots()]
    report["still_empty"] = missing
    H.log("palette %s: %d mesh slots, %d material slots; empty: %s" % (path, len(report["meshes"]), len(report["materials"]), missing))
    return pal, report
