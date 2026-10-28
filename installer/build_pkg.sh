#!/bin/sh
# Builds the double-click installer: dist/Automixer-<version>.pkg
#
# It installs, for all users:
#   /Library/Audio/Plug-Ins/VST3/Automixer.vst3        the plugin (Resolve scans this folder by default)
#   /Library/Application Support/Automixer/            the sync-check tool, a read-me and an uninstaller
# The plugin and the tool are universal (Apple Silicon and Intel) and need nothing else installed.
set -e
cd "$(dirname "$0")"
here="$(pwd)"
project="$(cd .. && pwd)"
version="$(sed -n 's/^project(Automixer VERSION \([0-9.]*\).*/\1/p' "$project/CMakeLists.txt")"
[ -n "$version" ] || { echo "no version found in CMakeLists.txt" >&2; exit 1; }
support="/Library/Application Support/Automixer"

make -C "$project" test

stage="$here/build"
rm -rf "${stage:?}"
root="$stage/root"
mkdir -p "$root/Library/Audio/Plug-Ins/VST3" "$root$support"
cp -R "$project/build/Automixer_artefacts/Release/VST3/Automixer.vst3" "$root/Library/Audio/Plug-Ins/VST3/"
cp "$project/build/sync_check_artefacts/Release/sync_check" "$root$support/automixer-sync-check"
cp "$here/uninstall.sh" "$root$support/uninstall.sh"
cp "$here/resources/ReadMe.txt" "$root$support/ReadMe.txt"
chmod 755 "$root$support/uninstall.sh" "$root$support/automixer-sync-check"

# Both binaries must be universal, or the installer would break on one kind of Mac.
for bin in "$root/Library/Audio/Plug-Ins/VST3/Automixer.vst3/Contents/MacOS/Automixer" "$root$support/automixer-sync-check"; do
    archs="$(lipo -archs "$bin")"
    case "$archs" in *arm64*) ;; *) echo "not universal ($archs): $bin" >&2; exit 1 ;; esac
    case "$archs" in *x86_64*) ;; *) echo "not universal ($archs): $bin" >&2; exit 1 ;; esac
done
codesign --force --deep --sign - "$root/Library/Audio/Plug-Ins/VST3/Automixer.vst3"
codesign --force --sign - "$root$support/automixer-sync-check"

# Extended attributes would end up in the package as "._" clutter files.
xattr -cr "$root"
export COPYFILE_DISABLE=1

# Install exactly where stated (never "update" a development copy found
# elsewhere), and always replace the plugin, also with the same version.
pkgbuild --analyze --root "$root" "$stage/components.plist"
/usr/libexec/PlistBuddy -c "Add :0:BundleIsRelocatable bool false" "$stage/components.plist"
/usr/libexec/PlistBuddy -c "Set :0:BundleIsVersionChecked false" "$stage/components.plist"

pkgbuild --root "$root" --component-plist "$stage/components.plist" --scripts "$here/scripts" \
    --identifier com.ilias823.automixer --version "$version" --install-location / \
    --ownership recommended "$stage/Automixer.pkg"

sed -e "s|@VERSION@|$version|g" "$here/distribution.xml" > "$stage/distribution.xml"
mkdir -p "$project/dist"
productbuild --distribution "$stage/distribution.xml" --resources "$here/resources" --package-path "$stage" \
    "$project/dist/Automixer-$version.pkg"
echo "built: $project/dist/Automixer-$version.pkg"
