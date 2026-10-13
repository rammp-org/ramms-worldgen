"""RammsWorldGen — procedural pedestrian / wheelchair environments.

Editor-side package: graph builders, palette and preset builders, example
level assembly, and the MCP toolset. Registered at editor startup by the
plugin's Content/Python/init_unreal.py.

    import ramms_worldgen.build_all as b; b.build("all")    # assets from code
    import ramms_worldgen.levels as l; l.build_level("downtown", seed=7)
"""

__all__ = ["register", "unregister"]

_registration = None


def register() -> bool:
    """Register the MCP toolset with the running editor. Idempotent."""
    global _registration
    if _registration is None:
        from toolset_registry.registration import Registration
        from .toolset import RammsWorldGenToolset

        _registration = Registration([RammsWorldGenToolset])
    return _registration.register()


def unregister() -> None:
    global _registration
    if _registration is not None:
        _registration.unregister()
        _registration = None
