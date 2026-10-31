#!/usr/bin/env bash
# Builds a release of Lumen Studio and packages it. Run from anywhere:
#
#   bash scripts/package.sh                 the installer, the portable ZIP and their checksums
#   bash scripts/package.sh --no-installer  everything but the installer (needs no NSIS)
#   bash scripts/package.sh --no-run        skips starting the packaged program
#
# It writes to dist/:
#   LumenStudio-<version>-win64/               the deployed folder (windeployqt)
#   LumenStudio-<version>-win64-portable.zip   portable: portable.txt keeps the data in userdata/
#   LumenStudio-<version>-win64.exe            the NSIS installer
#   LumenStudio-<version>-SHA256.txt           checksums of what to publish
#
# The version comes from project() in CMakeLists.txt. The installer needs NSIS
# (https://nsis.sourceforge.io, or: winget install NSIS.NSIS); it stops early when it is missing.
# Other arguments: the build directory (default build/package). Nothing is signed: a certificate
# is the publisher's; Windows SmartScreen warns about unsigned downloads.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
installer=1
run=1
build="$root/build/package"
for arg in "$@"; do
    case "$arg" in
        --no-installer) installer=0 ;;
        --no-run) run=0 ;;
        -h|--help) sed -n '2,17p' "$0" | cut -c3-; exit 0 ;;
        -*) echo "Unknown option: $arg (see --help)" >&2; exit 2 ;;
        *) build="$arg" ;;
    esac
done
dist="$root/dist"
say() { printf '\n== %s\n' "$*"; }
die() { echo "ERROR: $*" >&2; exit 1; }

# The toolchain: scripts/env.sh says where it is on this machine.
# shellcheck source=/dev/null
source "$root/scripts/env.sh"
for tool in cmake cpack ninja g++ objdump; do
    command -v "$tool" >/dev/null || die "$tool is not on PATH: adjust scripts/env.sh"
done
test -d "$QT_ROOT" || die "Qt is not at $QT_ROOT: adjust QT_ROOT in scripts/env.sh"

makensis=""
if [ "$installer" = 1 ]; then
    makensis="$(command -v makensis || true)"
    for candidate in "/c/Program Files (x86)/NSIS/makensis.exe" "/c/Program Files/NSIS/makensis.exe"; do
        if [ -z "$makensis" ] && [ -x "$candidate" ]; then
            makensis="$candidate"
        fi
    done
    [ -n "$makensis" ] || die "makensis was not found. Install NSIS (winget install NSIS.NSIS), or use --no-installer."
fi

say "Checks before building"
if [ -n "$(git -C "$root" status --porcelain 2>/dev/null)" ]; then
    echo "WARNING: the working tree has uncommitted changes; they go into this package."
fi
echo "Commit: $(git -C "$root" rev-parse --short HEAD 2>/dev/null || echo unknown)"
python -I "$root/scripts/check-translations.py" || die "The translations are not complete."

say "Building the release"
cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DLUMEN_BUILD_TESTS=OFF \
    -DLUMEN_DEPLOY_ON_BUILD=OFF -DCMAKE_PREFIX_PATH="$QT_ROOT"
cmake --build "$build" --parallel

version="$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$build/CMakeCache.txt")"
name="LumenStudio-$version-win64"
stage="$dist/$name"
say "Deploying version $version to dist/$name"
rm -rf "$stage" "$dist/$name-portable.zip" "$dist/$name.exe" "$dist/LumenStudio-$version-SHA256.txt"
mkdir -p "$dist"
cmake --install "$build" --prefix "$stage"

say "Checking the deployed folder"
for file in LumenStudio.exe qt.conf plugins/platforms/qwindows.dll plugins/sqldrivers/qsqlite.dll \
            plugins/tls/qschannelbackend.dll plugins/imageformats/qjpeg.dll data/workflows \
            docs/USER_GUIDE.md docs/THIRD_PARTY_NOTICES.md; do
    test -e "$stage/$file" || die "$file is missing from the deployed folder"
done
# Every DLL the program and its plugins load must be in the folder or part of Windows.
missing=0
while IFS= read -r binary; do
    while IFS= read -r dll; do
        if [ ! -e "$stage/$dll" ] && [ ! -e "/c/Windows/System32/$dll" ]; then
            case "$dll" in
                api-ms-win-*|ext-ms-win-*) ;; # resolved by Windows at load time
                *) echo "  ${binary#"$stage"/} needs $dll"; missing=1 ;;
            esac
        fi
    done < <(objdump -p "$binary" | sed -n 's/^[[:space:]]*DLL Name: //p')
done < <(find "$stage" \( -name '*.exe' -o -name '*.dll' \) -type f)
[ "$missing" = 0 ] || die "The folder lacks DLLs (above): it would not start on another computer."
echo "All DLLs are there."

if [ "$run" = 1 ]; then
    say "Starting the packaged program, without Qt or MinGW on PATH"
    scratch="$(mktemp -d)"
    # Found first: with only Windows on PATH, "timeout" would be Windows' own, another program.
    limit="$(command -v timeout)"
    shot="$(cygpath -m "$scratch/start.png")"
    data="$(cygpath -m "$scratch/data")"
    # An empty LUMEN_DATA_DIR would mean the real settings and projects of whoever runs this.
    [ -n "$data" ] && [ -n "$shot" ] && [ -n "$limit" ] || die "Could not prepare the temporary folder for the start check."
    # It renders its window to a PNG and quits; its data goes to a temporary folder.
    (cd "$scratch" && PATH="/c/Windows/System32:/c/Windows" LUMEN_DATA_DIR="$data" \
        "$limit" 180 "$stage/LumenStudio.exe" --screenshot "$shot" --size 1280x720) \
        || die "The packaged program did not start and quit by itself (exit code $?)."
    test -s "$scratch/start.png" || die "The packaged program started but rendered nothing."
    if grep -qs 'crash\|FATAL' "$scratch/data/logs/lumen.log"; then
        die "The packaged program logged a crash: $scratch/data/logs/lumen.log"
    fi
    if grep -qs 'Could not parse' "$scratch/data/logs/lumen.log"; then
        die "The packaged program lost its stylesheet: $scratch/data/logs/lumen.log"
    fi
    rm -rf "$scratch"
    echo "It starts and renders its window."
fi

say "Portable ZIP"
# The marker keeps settings, cache and projects next to the executable.
printf 'Portable mode: settings, logs, cache and projects are stored in the userdata folder.\nDelete this file to use the per-user locations instead.\n' \
    > "$stage/portable.txt"
(cd "$dist" && cmake -E tar cf "$name-portable.zip" --format=zip "$name")
rm "$stage/portable.txt"
echo "$dist/$name-portable.zip"

if [ "$installer" = 1 ]; then
    say "Installer"
    (cd "$build" && PATH="$(dirname "$makensis"):$PATH" cpack -G NSIS -B "$dist")
    rm -rf "$dist/_CPack_Packages"
    test -s "$dist/$name.exe" || die "cpack did not produce $name.exe"
    echo "$dist/$name.exe"
fi

say "Checksums"
(cd "$dist" && sha256sum "$name-portable.zip" $([ "$installer" = 1 ] && echo "$name.exe") | tee "LumenStudio-$version-SHA256.txt")
echo
echo "Done: $(du -sh "$stage" | cut -f1) deployed in dist/$name"
