#!/bin/sh
# Removes Automixer. Run with: sudo "/Library/Application Support/Automixer/uninstall.sh"
# Your own presets (~/Library/Application Support/Automixer/Presets) are kept.
set -e
if [ "$(id -u)" -ne 0 ]; then
    echo "run with sudo" >&2
    exit 1
fi
rm -rf "/Library/Audio/Plug-Ins/VST3/Automixer.vst3"
pkgutil --forget com.ilias823.automixer >/dev/null 2>&1 || true
rm -rf "/Library/Application Support/Automixer"
echo "Automixer removed. Restart DaVinci Resolve."
