#!/usr/bin/env bash
# Builds a release and packages it:
#   dist/LumenStudio-<version>-win64/              deployed folder (windeployqt)
#   dist/LumenStudio-<version>-win64-portable.zip  portable build (portable.txt → data in userdata/)
#   dist/LumenStudio-<version>-win64.exe           NSIS installer, when makensis is installed
# Usage: source scripts/env.sh && bash scripts/package.sh [build-dir]
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:-$root/build/package}"
dist="$root/dist"

cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DLUMEN_BUILD_TESTS=OFF \
    -DCMAKE_PREFIX_PATH="${QT_ROOT:-C:/Qt/6.9.2/mingw_64}"
cmake --build "$build" --parallel

version="$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$build/CMakeCache.txt")"
name="LumenStudio-$version-win64"
stage="$dist/$name"
rm -rf "$stage" "$dist/$name-portable.zip"
mkdir -p "$dist"

cmake --install "$build" --prefix "$stage"
test -f "$stage/LumenStudio.exe" || { echo "install did not produce LumenStudio.exe" >&2; exit 1; }

# Portable ZIP: the marker keeps settings, cache and projects next to the executable.
printf 'Portable mode: settings, logs, cache and projects are stored in the userdata folder.\nDelete this file to use the per-user locations instead.\n' \
    > "$stage/portable.txt"
(cd "$dist" && cmake -E tar cf "$name-portable.zip" --format=zip "$name")
rm "$stage/portable.txt"
echo "Portable ZIP: $dist/$name-portable.zip"

# Installer (needs NSIS: https://nsis.sourceforge.io).
if (cd "$build" && grep -q '^LUMEN_MAKENSIS:FILEPATH=.*makensis' CMakeCache.txt); then
    (cd "$build" && cpack -G NSIS -B "$dist")
    echo "Installer: $dist/$name.exe"
else
    echo "makensis not found - skipping the installer (install NSIS and re-run to build it)."
fi
