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

# slot -> list of static mesh asset names (any that exist are used, weight 1)
MESH_SLOTS = {
    "Curb": ["semf4_LOD0_TIER2_000", "semf4_LOD0_TIER2_001", "sepxW_LOD0_TIER2_000", "sepxW_LOD0_TIER2_001"],
    "SidewalkCorner": ["uiksdjydw_tier_2"],
    "Bench": ["SM_Bench01", "SM_Bench02", "SM_Bench03"],
    "Bollard": ["SM_Bollard01B", "SM_Bollard01P"],
    "BusStop": ["SM_BusStop01", "SM_BusStop02"],
    "StreetLight": ["SM_StreetLight01", "SM_StreetLight02", "SM_StreetLight03"],
    "Railing": ["SM_StreetRailing01", "SM_StreetRailing02"],
    "TrashBin": ["SM_Trashbin01", "SM_Trashbin01B", "SM_Trashbin01M", "SM_Trashbin02"],
    "InfoBoard": ["SM_InfoTable01", "SM_InfoTable02", "SM_InfoTable03"],
    "Stairs": ["stairs_street"],
    "RoofUnit": ["SM_AC_Box", "SM_AC_Exaust"],
    # hazards: things that end up in the access route
    "Hazard.Upheaval": ["adoquines_levantados_alcorque"],
    "Hazard.BinOut": ["SM_Trashbin01", "SM_Trashbin02"],
    "Hazard.Barrier": ["SM_StreetRailing01"],
    "Hazard.AFrame": ["SM_InfoTable01", "SM_InfoTable02"],
    "Hazard.Cafe": ["viejdi1_tier_2", "SM_Table"],
    # indoor
    "Furniture.Table": ["SM_Table", "SM_Classic_Round_Table", "SM_Modern_Table"],
    "Furniture.Chair": ["SM_Chair", "SM_Classic_chair", "SM_Modern_Chair_1", "SM_Elegant_chair", "SM_Old_Chair"],
    "Furniture.Sofa": ["SM_Classic_Leather_Sofa", "SM_Classic_Leather_sofa_2", "SM_Modern_Sofa_1", "SM_Modern_Sofa_2"],
    "Furniture.Armchair": ["SM_Classic_Leather_Armchair", "SM_Modern_Armchair"],
    "Furniture.CoffeeTable": ["SM_Coffee_Table"],
    "Furniture.Bed": ["SM_Modern_Bed_1", "SM_Modern_Bed_2", "SM_Bed_Drawer", "SM_Military_Bed"],
    "Furniture.BedTable": ["SM_Bed_Table"],
    "Furniture.Cupboard": ["SM_Classic_Cupboard", "SM_Modern_Cupboard", "SM_Commode", "SM_Classic_Commode"],
    "Furniture.TVStand": ["SM_Commode", "SM_Classic_Commode", "SM_Classic_Commode_2"],
    "Furniture.TV": ["SM_TV"],
    "Furniture.Desk": ["SM_Old_Table", "SM_Classic_table", "SM_Modern_Table"],
    "Furniture.OfficeChair": ["SM_Modern_Office_chair"],
    "Furniture.Booth": ["SM_Booth"],
    "Furniture.Plant": ["SM_PlantPot"],
    "Furniture.Lamp": ["SM_Table_Lamp", "SM_Bed_Lamp_1", "SM_Bed_Lamp_2", "SM_Bed_lamp_3"],
    "Furniture.Toilet": ["SM_Toilet"],
    "Furniture.Bathtub": ["SM_Bathtub"],
    "Furniture.Picture": ["SM_Picture", "SM_Classic_Picture"],
    "Furniture.Clutter": ["SM_UtilityCart", "SM_PlantPot", "SM_Old_Chair"],
}

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
    "Mat.Grass": ["MI_pjwey0", "MI_tkynejer"],
    "Mat.Lawn": ["MI_tkynejer", "MI_pjwey0"],
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


def _facing_yaw(mesh, bounds):
    """
    Yaw (degrees) that turns the mesh's front to +X. Seating, beds and toilets carry their mass high
    at the back (backrest, headboard, cistern), so the front is opposite the side where the upper
    vertices gather; symmetric pieces (tables, cupboards) get 0. Explicit MESH_FACING entries win.
    """
    name = mesh.get_name()
    if name in MESH_FACING:
        return MESH_FACING[name]
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


def _mesh_entry(mesh):
    import math
    bounds = mesh.get_bounds()
    ext = bounds.box_extent
    origin = bounds.origin
    yaw = _facing_yaw(mesh, bounds)
    e = unreal.RammsPaletteMesh()
    e.set_editor_property("mesh", mesh)
    e.set_editor_property("weight", 1.0)
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
                pal.add_mesh(slot, _mesh_entry(mesh))
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
