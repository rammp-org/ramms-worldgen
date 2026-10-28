"""Import NVIDIA SimReady Central assets and build a palette from them.

The assets are chosen, downloaded and flattened outside Unreal by Tools/simready_fetch.py, which writes
<cache>/ramms_selection.json (slot -> assets, with their SimReady metadata) and one merged USD per asset
(ramms_merged.usdc: the asset's renderable parts as one mesh, UsdPreviewSurface materials, Z-up metres, origin
at the bottom centre). This module imports those (Interchange USD) under /Game/SimReady/<Slot>/<Asset>/ and builds
/RammsWorldGen/Palettes/PAL_SimReady: a copy of a base palette (PAL_Default, or PAL_UrbanVerse for UrbanVerse
streets with SimReady interiors) whose indoor-furniture, bin, sign and hazard slots hold the SimReady meshes instead
(or as well, with mode="mix"). Everything else stays as in the base palette.

    import ramms_worldgen.simready as SR
    SR.build(r"D:/simready")                                        # import + palette
    SR.build(r"D:/simready", mode="mix")                            # SimReady variants alongside the base ones
    SR.build(r"D:/simready", base="/RammsWorldGen/Palettes/PAL_UrbanVerse")

The imported assets are not committed (each package keeps its own license; see <cache>/ATTRIBUTION.md, copied to
Content/SimReady/ATTRIBUTION.md on disk).

Assets are metric and keep their own size. Their facing comes from the palette's geometric heuristic (backrests,
cisterns and screens mark the back), with FACING_FIX for the assets where it picks the back or a side; slots whose
front is a long face keep the heuristic's axis. Each mesh gets a physical material with the friction of its
SimReady physics materials.
"""
import json
import os
import re
import shutil

import unreal

from . import _helpers as H
from . import palettes as P

DEST = "/Game/SimReady"
PALETTE = H.PALETTES + "/PAL_SimReady"
# yaw (degrees) that turns a SimReady asset's authored front to +X, for the slots whose front is a long face
FRONT_YAW = 0.0
LONG_FRONT_SLOTS = {"Hazard.Barrier", "Furniture.Sofa", "Furniture.Desk", "Furniture.TV", "Furniture.KitchenLow", "Furniture.KitchenUpper",
                    "Furniture.RestroomSink", "Furniture.OfficeStorage", "Store.Shelf"}
# Per-asset corrections (degrees, added to the measured facing yaw, as palettes.FACING_FIX) where the geometric
# heuristic picks the back or a side: refrigerators and washers carry their weight high at the back (hinges,
# compressor housings, motors), cabinets are boxes. SimReady has no common front axis (NVIDIA's two office-chair
# packages are authored 90 degrees apart), so each was checked by rendering it at yaw 0 and turned.
FACING_FIX = {
    "LGElectronics/simready-assets/refrigerator_lrfs28xbs": 180.0, "LGElectronics/simready-assets/refrigerator_lf31s6360s": 180.0,
    "LGElectronics/simready-assets/refrigerator_lf24z6330s": 180.0, "nvidia/simready-assets/appliance_refrigerator_a01": 180.0,
    "ForgeCAD/simready-assets/front_load_washer": 180.0, "ForgeCAD/simready-assets/chest_of_drawers": 180.0,
    "ForgeCAD/simready-assets/range_oven": -90.0,
    "nvidia/simready-assets/facilityFurniture_lockerUnit_multiDoor_a01": 180.0, "nvidia/simready-assets/cabinet_file_b04": 180.0,
    "nvidia/simready-assets/cabinet_file_a01": 180.0, "nvidia/simready-assets/table_bedside_b01": 180.0,
    "nvidia/simready-assets/computer_monitor_office_a01": 180.0,
}
# triangle count above which an imported mesh is made a Nanite mesh
NANITE_TRIS = 50000
# SimReady textures are authored up to 8K (tiling materials): clamp what Unreal keeps
MAX_TEXTURE_SIZE = 2048
_DATASET_TAG = {"nvidia": "NV", "ForgeCAD": "FC", "LGElectronics": "LG"}


def _asset_name(item):
    ns = item.get("repo", "x/").split("/")[0]
    tag = _DATASET_TAG.get(ns, re.sub(r"[^A-Za-z0-9]", "", ns)[:4] or "X")
    return "SM_SR_%s_%s" % (tag, re.sub(r"[^A-Za-z0-9_]", "_", item.get("asset", "asset"))[:48])


def _static_meshes(folder):
    out = []
    for p in H.EAL.list_assets(folder, recursive=True):
        ad = H.EAL.find_asset_data(p)
        if str(ad.asset_class_path.asset_name) == "StaticMesh":
            out.append(p)
    return out


def _import_usd(usd, folder, name):
    """Import one merged SimReady USD (a single mesh) into its own folder. Returns the static mesh."""
    path = folder + "/" + name
    if H.EAL.does_asset_exist(path):
        return unreal.load_asset(path)
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", usd)
    task.set_editor_property("destination_path", folder)
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", True)
    try:
        pipe = unreal.InterchangeGenericAssetsPipeline()
        mp = pipe.get_editor_property("mesh_pipeline")
        mp.set_editor_property("import_skeletal_meshes", False)
        mp.set_editor_property("combine_static_meshes_behavior", unreal.InterchangeCombineStaticMeshesBehavior.VISIBLE_ONLY)
        mp.set_editor_property("build_nanite", False)       # decided below, by triangle count
        try:
            pipe.get_editor_property("animation_pipeline").set_editor_property("import_animations", False)
        except Exception:  # noqa: BLE001
            pass
        task.set_editor_property("options", pipe)
    except Exception as exc:  # noqa: BLE001 - fall back to the project's default USD import settings
        H.warn("simready: interchange options not set (%s)" % exc)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    meshes = [unreal.load_asset(p) for p in _static_meshes(folder)]
    meshes = [m for m in meshes if m is not None]
    if not meshes:
        return None
    if len(meshes) > 1:
        H.warn("simready: %s imported as %d meshes; keeping the largest" % (os.path.basename(os.path.dirname(usd)), len(meshes)))
    mesh = max(meshes, key=lambda m: m.get_bounds().box_extent.length())
    if mesh.get_path_name().split(".")[0] != path:
        try:
            H.EAL.rename_asset(mesh.get_path_name().split(".")[0], path)
            mesh = unreal.load_asset(path)
        except Exception as exc:  # noqa: BLE001
            H.warn("simready: rename to %s: %s" % (path, exc))
    try:
        if mesh.get_num_triangles(0) > NANITE_TRIS:
            ns = mesh.get_editor_property("nanite_settings")
            ns.set_editor_property("enabled", True)
            mesh.set_editor_property("nanite_settings", ns)
    except Exception as exc:  # noqa: BLE001
        H.warn("simready: nanite for %s: %s" % (name, exc))
    H.EAL.save_loaded_asset(mesh, False)
    _clamp_textures(folder)
    return mesh


def _clamp_textures(folder):
    for p in H.EAL.list_assets(folder, recursive=True):
        ad = H.EAL.find_asset_data(p)
        if str(ad.asset_class_path.asset_name) != "Texture2D":
            continue
        t = unreal.load_asset(p)
        try:
            if t.get_editor_property("max_texture_size") in (0, None) or t.get_editor_property("max_texture_size") > MAX_TEXTURE_SIZE:
                if max(t.blueprint_get_size_x(), t.blueprint_get_size_y()) > MAX_TEXTURE_SIZE:
                    t.set_editor_property("max_texture_size", MAX_TEXTURE_SIZE)
                    H.EAL.save_loaded_asset(t, False)
        except Exception as exc:  # noqa: BLE001
            H.warn("simready: texture size of %s: %s" % (p, exc))


def _physical_material(item):
    """A physical material per friction value (rounded to 0.05), from the asset's SimReady physics materials."""
    f = item.get("friction")
    try:
        f = max(0.05, min(2.0, round(float(f) * 20.0) / 20.0))
    except (TypeError, ValueError):
        return None
    path = DEST + "/Physics/PM_SR_Friction_%03d" % int(round(f * 100))
    if H.EAL.does_asset_exist(path):
        return unreal.load_asset(path)
    H.ensure_dir(DEST + "/Physics")
    pm = unreal.AssetToolsHelpers.get_asset_tools().create_asset(path.rsplit("/", 1)[1], DEST + "/Physics", unreal.PhysicalMaterial, unreal.PhysicalMaterialFactoryNew())
    pm.set_editor_property("friction", f)
    pm.set_editor_property("static_friction", f)
    H.save(pm)
    return pm


def _apply_friction(mesh, pm):
    """The physical material on the body setup (simple collision) and on the mesh's own material instances
    (complex-as-simple collision); the instances are per asset, under its import folder."""
    bs = mesh.get_editor_property("body_setup")
    if bs is not None:
        bs.set_editor_property("phys_material", pm)
    for sm in mesh.get_editor_property("static_materials") or []:
        mat = sm.get_editor_property("material_interface")
        if isinstance(mat, unreal.MaterialInstanceConstant) and mat.get_path_name().startswith(DEST + "/"):
            try:
                if mat.get_editor_property("phys_material") != pm:
                    mat.set_editor_property("phys_material", pm)
                    H.EAL.save_loaded_asset(mat, False)
            except Exception as exc:  # noqa: BLE001
                H.warn("simready: physical material on %s: %s" % (mat.get_name(), exc))
    H.EAL.save_loaded_asset(mesh, False)


def import_selection(cache_dir, slots=None):
    """Import every asset listed in <cache_dir>/ramms_selection.json. Returns {slot: [(mesh, item)]}. An asset
    chosen for several slots is imported once."""
    with open(os.path.join(cache_dir, "ramms_selection.json"), encoding="utf-8") as fh:
        sel = json.load(fh)
    out, done = {}, {}
    for slot, items in sel.get("slots", {}).items():
        if slots and slot not in slots:
            continue
        for it in items:
            usd = it.get("usd")
            if not usd or not os.path.exists(usd):
                H.warn("simready: %s %s has no merged USD (run Tools/simready_fetch.py)" % (slot, it.get("uid")))
                continue
            if it["uid"] not in done:
                name = _asset_name(it)
                folder = DEST + "/" + name
                H.ensure_dir(folder)
                try:
                    mesh = _import_usd(usd, folder, name)
                except Exception as exc:  # noqa: BLE001
                    H.warn("simready: import %s failed: %s" % (usd, exc))
                    mesh = None
                if mesh is not None:
                    pm = _physical_material(it)
                    if pm is not None:
                        try:
                            _apply_friction(mesh, pm)
                        except Exception as exc:  # noqa: BLE001
                            H.warn("simready: physical material for %s: %s" % (mesh.get_name(), exc))
                done[it["uid"]] = mesh
            if done[it["uid"]] is not None:
                out.setdefault(slot, []).append((done[it["uid"]], it))
    # Interchange can leave the materials and textures it creates unsaved: save the whole folder.
    H.EAL.save_directory(DEST, only_if_is_dirty=True, recursive=True)
    try:
        src = os.path.join(cache_dir, "ATTRIBUTION.md")
        dst = os.path.join(unreal.Paths.project_content_dir(), "SimReady", "ATTRIBUTION.md")
        if os.path.exists(src):
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copyfile(src, dst)
    except Exception as exc:  # noqa: BLE001
        H.warn("simready: attribution copy: %s" % exc)
    return out


def _yaw(mesh, item, slot, facing):
    """Facing yaw: the palette's geometric heuristic ("geometry", the default), or FRONT_YAW for every asset
    ("authored"). For long-fronted slots the heuristic's answer is kept only if it puts the long side across the
    facing direction; otherwise the front is one of the two long faces, and FRONT_YAW picks which."""
    if facing == "authored":
        return FRONT_YAW
    if slot in LONG_FRONT_SLOTS:
        b = mesh.get_bounds().box_extent
        y = P._facing_yaw(mesh, mesh.get_bounds(), slot)
        long_x = b.x >= b.y
        across = (abs(y) % 180.0 < 1.0) != long_x      # yaw 0/180 keeps X as the facing axis
        if across:
            return y
        cands = (90.0, -90.0) if long_x else (0.0, 180.0)
        return min(cands, key=lambda c: abs((c - FRONT_YAW + 180.0) % 360.0 - 180.0))
    return None


def source_id(item):
    """Where a palette entry came from, carried into the manifest (features' "source"): the dataset, the asset and
    the dataset commit the SimReady Central index pins."""
    return "simready:%s@%s|%s" % (item.get("uid", ""), str(item.get("revision", ""))[:12], item.get("category", ""))


def build(cache_dir, mode="replace", slots=None, path=PALETTE, base=None, facing="geometry"):
    """Import the selection and build the SimReady palette from `base` (PAL_Default by default). mode: "replace"
    (SimReady only in the slots it fills) or "mix" (added to the base palette's variants). Returns (palette, report)."""
    P._COLLISION_DONE.clear()
    imported = import_selection(cache_dir, slots)
    for pairs in imported.values():
        for mesh, it in pairs:
            if it.get("uid") in FACING_FIX:
                P.FACING_FIX[mesh.get_name()] = FACING_FIX[it["uid"]]
    base = base or P.DEFAULT_PALETTE
    if base == P.DEFAULT_PALETTE and not H.EAL.does_asset_exist(base):
        P.build_default_palette()
    src = unreal.load_asset(base)
    if src is None:
        raise RuntimeError("simready: base palette %s did not load" % base)
    # Refresh in place (deleting it would break every reference to it: presets, open levels, generators set to it).
    pal = unreal.load_asset(path) if H.EAL.does_asset_exist(path) else H.EAL.duplicate_asset(base, path)
    if pal is None:
        raise RuntimeError("simready: could not create palette %s" % path)
    pal.set_editor_property("slots", src.get_editor_property("slots"))
    pal.set_editor_property("default_material", src.get_editor_property("default_material"))
    slots_map = pal.get_editor_property("slots")
    report = {"slots": {}, "mode": mode, "base": base}
    for slot, pairs in imported.items():
        entries = []
        for mesh, it in pairs:
            e = P._mesh_entry(mesh, slot, yaw=_yaw(mesh, it, slot, facing), normalise=False)
            e.set_editor_property("source_id", source_id(it))
            entries.append(e)
        s = slots_map.get(unreal.Name(slot)) if slots_map else None
        if s is None:
            s = unreal.RammsPaletteSlot()
        existing = list(s.get_editor_property("meshes")) if mode == "mix" else []
        s.set_editor_property("meshes", existing + entries)
        s.set_editor_property("note", "SimReady Central (%s): %s" % (mode, ", ".join(sorted({it["asset"] for _m, it in pairs}))))
        pal.set_slot(unreal.Name(slot), s)
        report["slots"][slot] = [m.get_name() for m, _it in pairs]
    H.save(pal)
    H.log("simready palette %s: %s" % (path, {k: len(v) for k, v in report["slots"].items()}))
    return pal, report
