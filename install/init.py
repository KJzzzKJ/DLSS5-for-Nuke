"""
DLSS 5 Native Live - Version Router for Foundry Nuke
Automatically appends the correct bin/Nuke<version> directory to nuke.pluginPath.
"""
import os
import nuke

_plugin_dir = os.path.dirname(__file__).replace("\\", "/")
_version_bin = f"{_plugin_dir}/bin/Nuke{nuke.NUKE_VERSION_MAJOR}"

if os.path.isdir(_version_bin) and _version_bin not in nuke.pluginPath():
    nuke.pluginAddPath(_version_bin)
