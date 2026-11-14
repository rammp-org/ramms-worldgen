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


class Part:
    """Kit pieces for a facade role: path (relative to the pack's root or absolute; fnmatch patterns allowed in the
    last component), the yaw that turns the pack's piece to the facade convention (exterior toward +Y, the run
    along +X; None: the kit's), and dy, a shift out of (+) or into (-) the facade plane in cm at native size (a
    wall modelled centred on its plane is moved in by half its thickness)."""

    def __init__(self, path, yaw=None, dy=0.0, weight=1.0):
        self.path = path
        self.yaw = yaw
        self.dy = dy
        self.weight = weight


class Kit:
    """A pack's modular facade kit as one facade style (palette FacadeKits, FRammsFacadeKitInfo).

    style: the style name ("Facade.<style>.<Role>" slots; no dots).
    family: styles of one family may share upper floors (UpperMixChance).
    roles: {"GF" | "GFDoor" | "Win" | "WinTop" | "Top": [Part | path]}: ground-floor modules, the front's door
        module, window floors, the top floor's variant, the cornice. GF and Win are required.
    scale: uniform scale of every module (a pack modelled oversize comes down to real storeys).
    yaw: the yaw that brings the pack's pieces to the convention (Parts may override it). The run's start
        (pivot) is measured from the bounds, so centred and end pivots both work.
    weight, max_stretch, upper_mix: FRammsFacadeKitInfo's Weight, MaxStretch, UpperMixChance.
    materials: [(regex on the mesh's material slot name, category)]: those slots take a pick from the
        palette's "Facade.<style>.Mat.<category>" set; every other slot keeps the pack's own material.
    material_sets: {category: [paths]}: that set's materials (relative to the pack or absolute); the "Glass"
        category defaults to the built-in window glass (surfaces.build_window_glass).
    """

    def __init__(self, style, family, roles, scale=1.0, yaw=0.0, weight=1.0, max_stretch=0.08, upper_mix=0.0, materials=(), material_sets=None, notes=""):
        assert "." not in style, style
        self.style = style
        self.family = family
        self.roles = {r: [p if isinstance(p, Part) else Part(p) for p in parts] for r, parts in roles.items()}
        self.scale = scale
        self.yaw = yaw
        self.weight = weight
        self.max_stretch = max_stretch
        self.upper_mix = upper_mix
        self.materials = list(materials)
        self.material_sets = dict(material_sets or {})
        self.notes = notes


class Pack:
    """A content pack: where it lives and what it contributes, per slot.

    name: the key recipes use.
    roots: its content folders; the first is the base of relative paths. Installed when any exists.
    meshes: slot -> [Ref | path] of static meshes.
    material_sets: slot -> [Ref | path] of material interfaces (decals, facade material sets).
    kits: [Kit]: modular facade kits, each one facade style.
    buildings: {"Downtown" | "DowntownCorner": [blueprint paths]}: whole buildings (packed level actors), captured
        part by part (palettes.packed_building_entry); a corner one's hand is measured.
    surfaces: key -> a `surfaces._build` spec (bc / n / rma texture paths relative to the root or absolute,
        tile size, tint...): world-aligned ground and wall materials, built on demand into a recipe's
        material folder as M_<Recipe>_<key>.
    notes: free text (licence, scale quirks) shown in reports.
    """

    def __init__(self, name, roots, meshes=None, material_sets=None, surfaces=None, kits=None, buildings=None, notes=""):
        self.name = name
        self.roots = list(roots)
        self.meshes = {k: _refs(v) for k, v in (meshes or {}).items()}
        self.material_sets = {k: _refs(v) for k, v in (material_sets or {}).items()}
        self.surfaces = dict(surfaces or {})
        self.kits = list(kits or [])
        self.buildings = dict(buildings or {})
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

    def kit_meshes(self, kit, role):
        """[(mesh, part)] of a kit's role."""
        out = []
        for part in kit.roles.get(role, []):
            for m in self.resolve(Ref(part.path), ("StaticMesh",)):
                out.append((m, part))
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
