"""Build everything from code: palette, profile, presets, graphs, (levels), (traffic).

In the editor:   import ramms_worldgen.build_all as b; b.build("all")
                 b.build("traffic")   # the signal parameter collection + recipes' signal materials (/Game)
From a shell:    python Scripts/editor_remote_exec.py --file Plugins/RammsWorldGen/Content/Python/ramms_worldgen/build_all.py
"""

from . import _helpers as H


def build(what="all"):
    report = {}
    if what in ("all", "palette"):
        from . import palettes
        pal, rep = palettes.build_default_palette()
        report["palette"] = rep
    if what in ("all", "graphs"):
        from . import graphs
        report["graphs"] = {k: g.get_path_name() for k, g in graphs.build_all_graphs().items()}
    if what in ("all", "presets"):
        from . import palettes, presets
        pal = H.EAL.load_asset(palettes.DEFAULT_PALETTE)
        if pal is None:
            pal, _ = palettes.build_default_palette()
        report["presets"] = {k: p.get_path_name() for k, p in presets.build_presets(pal).items()}
    if what == "levels":
        from . import levels
        report["levels"] = levels.build_all_levels()
    if what == "traffic":
        # The traffic signal collection, and every recipe's signal materials rebuilt to read it (local content).
        from . import recipes, traffic
        mpc = traffic.signal_collection()
        report["traffic"] = {"signal_collection": mpc.get_path_name() if mpc else None,
                             "signal_materials": {r: traffic.refresh_signal_materials(r) for r in recipes.RECIPES} if mpc else {}}
    return report


if __name__ == "__main__":
    import unreal
    unreal.log(H.dumps(build("all")))
