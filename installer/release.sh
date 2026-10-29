#!/bin/sh
# Publishes a new version: the installer (.pkg and .dmg) goes to the GitHub
# releases of this repository, where the plugin's "Check for Updates" button
# finds it.
#
#   1. raise the version in CMakeLists.txt: project(Automixer VERSION x.y.z)
#   2. commit and push your changes
#   3. installer/release.sh "What is new in this version"
#
# Needs the GitHub CLI (gh) signed in to the account that owns the repository.
set -e
cd "$(dirname "$0")/.."
repo="Ilias823/automixer"   # keep in step with kReleasesRepo in src/plugin/Updater.h
version="$(sed -n 's/^project(Automixer VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)"
notes="${1:?usage: installer/release.sh \"what is new in this version\"}"

if [ -n "$(git status --porcelain)" ]; then
    echo "commit your changes first" >&2
    exit 1
fi
if gh release view "v$version" --repo "$repo" >/dev/null 2>&1; then
    echo "v$version is already released: raise the version in CMakeLists.txt first." >&2
    exit 1
fi
installer/build_pkg.sh
installer/build_dmg.sh
gh release create "v$version" "dist/Automixer-$version.pkg" "dist/Automixer-$version.dmg" --repo "$repo" \
    --title "Automixer $version" --notes "$notes"
git tag "v$version" && git push origin "v$version"
echo "released v$version"
