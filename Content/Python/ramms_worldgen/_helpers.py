"""Small editor helpers shared by the world-generation builders and the toolset."""

import json

import unreal

TAG = "[RammsWorldGen] "
EAL = unreal.EditorAssetLibrary

# Where the project keeps its generated content. Palettes and presets reference
# /Game/Fab assets, so they live in the project, not the plugin.
ROOT = "/Game/WorldGen"
PALETTES = ROOT + "/Palettes"
PRESETS = ROOT + "/Presets"
GRAPHS = ROOT + "/Graphs"
PROFILES = ROOT + "/Profiles"
MAPS = "/Game/Maps/Gen"


def log(msg):
    unreal.log(TAG + str(msg))


def warn(msg):
    unreal.log_warning(TAG + str(msg))


def dumps(obj):
    return json.dumps(obj, indent=2, default=str)


def asset_tools():
    return unreal.AssetToolsHelpers.get_asset_tools()


def ensure_dir(path):
    if not EAL.does_directory_exist(path):
        EAL.make_directory(path)


def get_or_create(path, asset_class, factory=None):
    """Load an asset at `path`, creating it with `factory` when missing."""
    if EAL.does_asset_exist(path):
        return EAL.load_asset(path), False
    pkg, name = path.rsplit("/", 1)
    ensure_dir(pkg)
    if factory is None:
        factory = unreal.DataAssetFactory()
        factory.set_editor_property("data_asset_class", asset_class)
    obj = asset_tools().create_asset(name, pkg, asset_class, factory)
    if obj is None:
        raise RuntimeError("could not create %s" % path)
    return obj, True


def save(obj):
    EAL.save_loaded_asset(obj, only_if_is_dirty=False)


def soft_path(obj):
    return unreal.SoftObjectPath(obj.get_path_name()) if obj is not None else unreal.SoftObjectPath()


def editor_world():
    return unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()


def level_actors():
    return unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()


def find_generators(world=None):
    """Every ARammsWorldGenerator in the editor world (or the given world)."""
    cls = unreal.RammsWorldGenerator
    if world is None:
        return [a for a in level_actors() if isinstance(a, cls)]
    return list(unreal.GameplayStatics.get_all_actors_of_class(world, cls))


def find_generator(name=""):
    gens = find_generators()
    if name:
        n = name.lower()
        gens = [g for g in gens if n in g.get_name().lower() or n in g.get_actor_label().lower()]
    if not gens:
        raise RuntimeError("no RammsWorldGenerator in the level%s" % (" matching '%s'" % name if name else ""))
    return gens[0]


def manifest_to_dict(gen):
    """The generator's manifest as a plain dict (via its JSON export)."""
    return json.loads(gen.get_manifest_json() or "{}")
