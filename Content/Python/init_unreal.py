"""RammsWorldGen bootstrap — runs at editor startup from the plugin's Content/Python.

Registers the world-generation MCP toolset. Failures are logged and swallowed
so a broken toolset never stops the editor from opening.
"""

import unreal

TAG = "[RammsWorldGen] "


def _register():
    try:
        import ramms_worldgen
    except Exception as exc:
        unreal.log_warning(TAG + "not registered: %s" % exc)
        return
    try:
        if ramms_worldgen.register():
            unreal.log(TAG + "registered RammsWorldGenToolset with the toolset registry.")
        else:
            unreal.log(TAG + "toolset registry unavailable; skipped registration.")
    except Exception as exc:
        unreal.log_warning(TAG + "registration failed: %s" % exc)


_register()
