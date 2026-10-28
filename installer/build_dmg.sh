#!/bin/sh
# Wraps the installer in a disk image: dist/Automixer-<version>.dmg
# (the .pkg plus the read-me). Builds the .pkg first if it is missing.
set -e
cd "$(dirname "$0")/.."
version="$(sed -n 's/^project(Automixer VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)"
pkg="dist/Automixer-$version.pkg"
[ -f "$pkg" ] || installer/build_pkg.sh
stage="installer/build/dmg"
rm -rf "$stage" && mkdir -p "$stage"
cp "$pkg" "$stage/Install Automixer.pkg"
cp installer/resources/ReadMe.txt "$stage/ReadMe.txt"
rm -f "dist/Automixer-$version.dmg"
hdiutil create -volname "Automixer $version" -srcfolder "$stage" -fs HFS+ -format UDZO "dist/Automixer-$version.dmg" >/dev/null
hdiutil verify "dist/Automixer-$version.dmg" >/dev/null
echo "built: dist/Automixer-$version.dmg"
