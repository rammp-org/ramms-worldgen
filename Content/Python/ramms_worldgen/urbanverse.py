"""Import UrbanVerse-100K assets and build a palette from them.

The assets are chosen and downloaded outside Unreal by Tools/urbanverse_fetch.py, which writes
<cache>/ramms_selection.json (slot -> assets, with their annotations). This module imports those GLBs
(Interchange glTF) under /Game/UrbanVerse/<Slot>/ and builds /RammsWorldGen/Palettes/PAL_UrbanVerse: a copy of
PAL_Default whose street-furniture, vehicle and hazard slots hold the UrbanVerse meshes instead (or as well,
with mode="mix"). Everything else (surfaces, facades, trees, houses, interiors) stays as in PAL_Default.

    import ramms_worldgen.urbanverse as UV
    UV.build(r"D:/urbanverse")                     # import + palette
    UV.build(r"D:/urbanverse", mode="mix")         # UrbanVerse variants alongside the Fab ones

The imported assets are not committed (their licenses vary per asset; see <cache>/ATTRIBUTION.md, which is
copied next to them as /Game/UrbanVerse/ATTRIBUTION.md on disk).

Assets are metric and keep their own size. Their front comes from the annotation's frontView (which of the
four turntable renders, 0/90/180/270 degrees, shows the functional front); FRONT_YAW_BASE / FRONT_YAW_STEP
map that to the yaw that turns the front to +X, the palette convention (checked with Scripts/worldgen/lineup.py).
"""
import json
import os
import shutil

import unreal

from . import _helpers as H
from . import palettes as P

DEST = "/Game/UrbanVerse"
PALETTE = H.PALETTES + "/PAL_UrbanVerse"
# yaw (degrees) that turns an imported UrbanVerse mesh's front to +X, given its annotated frontView index
FRONT_YAW_BASE = 0.0
FRONT_YAW_STEP = 90.0
# triangle count above which an imported mesh is made a Nanite mesh
NANITE_TRIS = 50000
# Slots whose front is a long face (shelters, dumpsters, barriers): only the axis is decided, from the bounds.
LONG_FRONT_SLOTS = {"BusStop", "Hazard.Dumpster", "Hazard.Barrier", "Bench", "NewspaperBox"}
# GLBs bigger than this are skipped (scanned assets with 4K+ textures: slow to import, little gain on a street)
MAX_GLB_MB = 40.0


def _asset_name(item):
    cat = "".join(c if c.isalnum() else "_" for c in str(item.get("category", "obj"))).strip("_")[:32]
    return "SM_UV_%s_%s" % (cat, item["uid"][:8])


def _import_glb(glb, dest_dir, name):
    """Import one GLB as a single static mesh (with its materials and textures). Returns the mesh."""
    path = dest_dir + "/" + name
    if H.EAL.does_asset_exist(path):
        return unreal.load_asset(path)
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", glb)
    task.set_editor_property("destination_path", dest_dir)
    task.set_editor_property("destination_name", name)
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", True)
    try:
        pipe = unreal.InterchangeGenericAssetsPipeline()
        mp = pipe.get_editor_property("mesh_pipeline")
        # (glTF scenes already come in as one combined static mesh by default in 5.8)
        mp.set_editor_property("import_skeletal_meshes", False)
        try:
            pipe.get_editor_property("animation_pipeline").set_editor_property("import_animations", False)
        except Exception:  # noqa: BLE001
            pass
        task.set_editor_property("options", pipe)
    except Exception as exc:  # noqa: BLE001 - fall back to the project's default glTF import settings
        H.warn("urbanverse: interchange options not set (%s)" % exc)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    meshes = []
    for p in task.get_editor_property("imported_object_paths") or []:
        a = unreal.load_asset(p)
        if isinstance(a, unreal.StaticMesh):
            meshes.append(a)
    if not meshes:
        # Interchange can name the mesh after the glTF node and put it in a folder named after the file: look
        # (by class, without loading everything) for static meshes under the destination that match either.
        stem = os.path.splitext(os.path.basename(glb))[0]
        for p in H.EAL.list_assets(dest_dir, recursive=True):
            ad = H.EAL.find_asset_data(p)
            if str(ad.asset_class_path.asset_name) != "StaticMesh":
                continue
            if name in str(ad.asset_name) or ("/" + stem + "/") in str(ad.package_path) + "/" or str(ad.package_path).endswith("/" + stem):
                a = unreal.load_asset(p)
                if a is not None:
                    meshes.append(a)
    if not meshes:
        return None
    if len(meshes) > 1:
        # Only the largest is used: a GLB holding several objects (a scene, or a model split per part) loses the rest.
        H.warn("urbanverse: %s imported as %d meshes (%s); keeping the largest" % (os.path.basename(glb), len(meshes), ", ".join(m.get_name() for m in meshes[:4])))
    mesh = max(meshes, key=lambda m: m.get_bounds().box_extent.length())
    if mesh.get_path_name().split(".")[0] != path:
        try:
            H.EAL.rename_asset(mesh.get_path_name().split(".")[0], path)
            mesh = unreal.load_asset(path)
        except Exception:  # noqa: BLE001
            pass
    try:
        if mesh.get_num_triangles(0) > NANITE_TRIS:
            ns = mesh.get_editor_property("nanite_settings")
            ns.set_editor_property("enabled", True)
            mesh.set_editor_property("nanite_settings", ns)
    except Exception as exc:  # noqa: BLE001
        H.warn("urbanverse: nanite for %s: %s" % (name, exc))
    H.EAL.save_loaded_asset(mesh, False)
    return mesh


def _physical_material(item):
    """A physical material per friction value (rounded to 0.05), so the robot's wheels see the annotated friction."""
    f = item.get("friction")
    try:
        f = max(0.05, min(2.0, round(float(f) * 20.0) / 20.0))
    except (TypeError, ValueError):
        return None
    path = DEST + "/Physics/PM_UV_Friction_%03d" % int(round(f * 100))
    if H.EAL.does_asset_exist(path):
        return unreal.load_asset(path)
    H.ensure_dir(DEST + "/Physics")
    pm = unreal.AssetToolsHelpers.get_asset_tools().create_asset(path.rsplit("/", 1)[1], DEST + "/Physics", unreal.PhysicalMaterial, unreal.PhysicalMaterialFactoryNew())
    pm.set_editor_property("friction", f)
    pm.set_editor_property("static_friction", f)
    H.save(pm)
    return pm


def _apply_friction(mesh, pm):
    """Give the mesh the friction of its physical material on every collision path. The body setup's physical
    material only covers simple shapes; palette meshes without simple collision collide complex-as-simple, where
    the material's physical material is the one used. The imported material instances are per asset (Interchange
    writes them under the GLB's own folder), so they can carry it without touching anything else."""
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
                H.warn("urbanverse: physical material on %s: %s" % (mat.get_name(), exc))
    H.EAL.save_loaded_asset(mesh, False)


def import_selection(cache_dir, slots=None):
    """Import every asset listed in <cache_dir>/ramms_selection.json. Returns {slot: [(mesh, item)]}."""
    sel = json.load(open(os.path.join(cache_dir, "ramms_selection.json"), encoding="utf-8"))
    out = {}
    for slot, items in sel.get("slots", {}).items():
        if slots and slot not in slots:
            continue
        dest = DEST + "/" + slot.replace(".", "_")
        H.ensure_dir(dest)
        for it in items:
            glb = it.get("glb")
            if not glb or not os.path.exists(glb):
                H.warn("urbanverse: %s %s has no GLB" % (slot, it.get("uid")))
                continue
            if os.path.getsize(glb) > MAX_GLB_MB * 1e6:
                H.log("urbanverse: skip %s %s (%.0f MB)" % (slot, it.get("uid"), os.path.getsize(glb) / 1e6))
                continue
            try:
                mesh = _import_glb(glb, dest, _asset_name(it))
            except Exception as exc:  # noqa: BLE001
                H.warn("urbanverse: import %s failed: %s" % (glb, exc))
                mesh = None
            if mesh is None:
                continue
            pm = _physical_material(it)
            if pm is not None:
                try:
                    _apply_friction(mesh, pm)
                except Exception as exc:  # noqa: BLE001
                    H.warn("urbanverse: physical material for %s: %s" % (mesh.get_name(), exc))
            out.setdefault(slot, []).append((mesh, it))
    # Interchange can leave the materials and textures it creates unsaved: save the whole folder.
    H.EAL.save_directory(DEST, only_if_is_dirty=True, recursive=True)
    # Keep the attribution next to the imported assets.
    try:
        src = os.path.join(cache_dir, "ATTRIBUTION.md")
        dst = os.path.join(unreal.Paths.project_content_dir(), "UrbanVerse", "ATTRIBUTION.md")
        if os.path.exists(src):
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copyfile(src, dst)
    except Exception as exc:  # noqa: BLE001
        H.warn("urbanverse: attribution copy: %s" % exc)
    return out


def front_yaw(item):
    try:
        v = int(item.get("front_view") or 0)
    except (TypeError, ValueError):
        v = 0
    return ((FRONT_YAW_BASE + FRONT_YAW_STEP * v) + 180.0) % 360.0 - 180.0


def _yaw(mesh, item, slot, facing):
    """Facing yaw for an imported mesh: the annotation's frontView ("annotation"), or the palette's geometric
    heuristic ("geometry", the default: backrests and cisterns mark the back). For long-fronted slots the
    heuristic's answer is kept only if it puts the long side across the facing direction; otherwise the front is
    one of the two long faces, and the annotation picks which."""
    if facing == "annotation":
        return front_yaw(item)
    if slot in LONG_FRONT_SLOTS:
        b = mesh.get_bounds().box_extent
        y = P._facing_yaw(mesh, mesh.get_bounds(), slot)
        long_x = b.x >= b.y
        across = (abs(y) % 180.0 < 1.0) != long_x      # yaw 0/180 keeps X as the facing axis
        if across:
            return y
        cands = (90.0, -90.0) if long_x else (0.0, 180.0)
        a = front_yaw(item)
        return min(cands, key=lambda c: abs((c - a + 180.0) % 360.0 - 180.0))
    return None


def source_id(item):
    """Where a palette entry came from, carried into the manifest (features' "source")."""
    return "urbanverse:%s|%s" % (item.get("uid", ""), item.get("category", ""))


def build(cache_dir, mode="replace", slots=None, path=PALETTE, facing="geometry"):
    """Import the selection and build the UrbanVerse palette. mode: "replace" (UrbanVerse only in the slots it
    fills) or "mix" (added to the Fab variants). Returns (palette, report)."""
    P._COLLISION_DONE.clear()
    imported = import_selection(cache_dir, slots)
    if not H.EAL.does_asset_exist(P.DEFAULT_PALETTE):
        P.build_default_palette()
    default = unreal.load_asset(P.DEFAULT_PALETTE)
    if default is None:
        raise RuntimeError("urbanverse: default palette %s did not load" % P.DEFAULT_PALETTE)
    # Refresh in place (deleting it would break every reference to it: presets, open levels, generators set to it).
    pal = unreal.load_asset(path) if H.EAL.does_asset_exist(path) else H.EAL.duplicate_asset(P.DEFAULT_PALETTE, path)
    if pal is None:
        raise RuntimeError("urbanverse: could not create palette %s" % path)
    pal.set_editor_property("slots", default.get_editor_property("slots"))
    pal.set_editor_property("default_material", default.get_editor_property("default_material"))
    slots_map = pal.get_editor_property("slots")
    report = {"slots": {}, "mode": mode}
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
        s.set_editor_property("note", "UrbanVerse-100K (%s): %s" % (mode, ", ".join(sorted({it["category"] for _m, it in pairs}))))
        pal.set_slot(unreal.Name(slot), s)
        report["slots"][slot] = [m.get_name() for m, _it in pairs]
    H.save(pal)
    H.log("urbanverse palette %s: %s" % (path, {k: len(v) for k, v in report["slots"].items()}))
    return pal, report
