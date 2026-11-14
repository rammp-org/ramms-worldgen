"""What a content pack offers the world generator, declared once per pack.

A `Pack` names the content folders it lives in and, per palette slot, the assets it contributes: meshes for the
mesh slots (benches, trees, street lights...), material instances for the material sets (decals, facade
materials), and the world-aligned surfaces `surfaces.py` can build from its textures (road, sidewalk, brick).
Assets are given by path relative to the pack's first folder (or absolute), so two packs that both ship an
`SM_Bench01` never get mixed up the way a name lookup across /Game would; a pack that is not installed simply
contributes nothing.

Palettes are put together from packs by `recipes.py`.
"""

import fnmatch
import re

import unreal

EAL = unreal.EditorAssetLibrary


class Ref:
    """One asset a pack offers a slot.

    path: relative to the pack's root (or absolute, starting with "/"); a trailing "*" (or any fnmatch pattern
    in the last component) matches every asset of that name pattern in that folder.
    weight: how often a node picks it relative to the slot's other meshes (palette `Weight`).
    normalise: pull the mesh toward the slot's real-world height (palettes.SLOT_HEIGHTS); False keeps its size.
    yaw: the yaw that turns its front to +X when the heuristic gets it wrong (None: measured).
    """

    def __init__(self, path, weight=1.0, normalise=True, yaw=None):
        self.path = path
        self.weight = weight
        self.normalise = normalise
        self.yaw = yaw

    def __repr__(self):
        return "Ref(%r)" % self.path


def _refs(items):
    return [i if isinstance(i, Ref) else Ref(i) for i in items]


class Pack:
    """A content pack: where it lives and what it contributes, per slot.

    name: the key recipes use.
    roots: its content folders; the first is the base of relative paths. Installed when any exists.
    meshes: slot -> [Ref | path] of static meshes.
    material_sets: slot -> [Ref | path] of material interfaces (decals, facade material sets).
    surfaces: key -> a `surfaces._build` spec (bc / n / rma texture paths relative to the root or absolute,
        tile size, tint...): world-aligned ground and wall materials, built on demand into a recipe's
        material folder as M_<Recipe>_<key>.
    notes: free text (licence, scale quirks) shown in reports.
    """

    def __init__(self, name, roots, meshes=None, material_sets=None, surfaces=None, notes=""):
        self.name = name
        self.roots = list(roots)
        self.meshes = {k: _refs(v) for k, v in (meshes or {}).items()}
        self.material_sets = {k: _refs(v) for k, v in (material_sets or {}).items()}
        self.surfaces = dict(surfaces or {})
        self.notes = notes

    def installed(self):
        return any(EAL.does_directory_exist(r) for r in self.roots)

    def abs_path(self, path):
        return path if path.startswith("/") else "%s/%s" % (self.roots[0], path)

    def resolve(self, ref, classes=("StaticMesh",)):
        """The assets a ref names (several for a pattern), loaded; [] when none exist."""
        path = self.abs_path(ref.path)
        folder, leaf = path.rsplit("/", 1)
        if not any(c in leaf for c in "*?["):
            if not EAL.does_asset_exist(path):
                return []
            a = EAL.load_asset(path)
            return [a] if a is not None else []
        reg = unreal.AssetRegistryHelpers.get_asset_registry()
        out = []
        for ad in sorted(reg.get_assets_by_path(folder, recursive=False), key=lambda d: str(d.asset_name)):
            if str(ad.asset_class_path.asset_name) in classes and fnmatch.fnmatchcase(str(ad.asset_name), leaf):
                a = ad.get_asset()
                if a is not None:
                    out.append(a)
        return out

    def slot_meshes(self, slot):
        """[(mesh, ref)] for a mesh slot."""
        out = []
        for ref in self.meshes.get(slot, []):
            for m in self.resolve(ref, ("StaticMesh",)):
                out.append((m, ref))
        return out

    def slot_materials(self, slot):
        """[(material, ref)] for a material set."""
        out = []
        for ref in self.material_sets.get(slot, []):
            for m in self.resolve(ref, ("MaterialInstanceConstant", "Material")):
                out.append((m, ref))
        return out

    def surface_spec(self, key):
        """The surface's builder spec with its texture paths made absolute."""
        spec = dict(self.surfaces[key])
        for k in ("bc", "n", "rma", "bc2", "n2", "rma2"):
            if isinstance(spec.get(k), str):
                spec[k] = self.abs_path(spec[k])
        return spec


def matches(name, patterns):
    """Whether name matches any of the regexes (compiled or strings)."""
    return any((p if hasattr(p, "match") else re.compile(p)).match(name) for p in patterns)
