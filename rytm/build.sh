#!/usr/bin/env bash
# Build SK Rytm (Overbridge for the Elektron Analog Rytm MKII) for MPC OS: the two plugins, SK Rytm Track and
# SK Rytm Send, for both install routes.
#
#   rytm/build.sh [version]          default version: 0.1.0
#
# dist/SK-Rytm-<version>-mpc-sdk.zip            MPC OS 3.10.1 and later, every MPC: both plugin folders in the format
#                                               of Akai's MPC Plugin Builder SDK, for a "VST2 Plugins" folder.
# dist/SK-Rytm-Track-<version>-mpc-armv7.zip    MPC OS 3.10.0 and older on the Force and the other Gen1 MPCs (root SSH,
# dist/SK-Rytm-Send-<version>-mpc-armv7.zip     MockbaMod): each with install.sh, which registers it in MPC.settings.
#
# Needs what mpc/build.sh needs (cross compilers, python3 + Pillow, zip, the mpc-vst-plugins checkout in $MPC_VST).
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)
VERSION="${1:-0.1.0}"
MPC_VST_REF=3a54b72cd172c840881b0be4f887653cc0829bf0
MPC_VST="${MPC_VST:-$ROOT/../mpc-vst-plugins}"
VENDOR="Sher Khan"
GLIBC_MAX=2.31
B=rytm/build

die() { echo "error: $*" >&2; exit 1; }
[ -f "$MPC_VST/tools/release.py" ] || die "no mpc-vst-plugins checkout at $MPC_VST (see mpc/build.sh)"
have=$(git -C "$MPC_VST" rev-parse HEAD 2>/dev/null || echo unknown)
[ "$have" = "$MPC_VST_REF" ] || echo "warning: mpc-vst-plugins is at $have, this was checked with $MPC_VST_REF"
python3 -c 'import PIL' 2>/dev/null || die "python3 needs Pillow (pip install pillow)"
for d in track send; do
    v=$(python3 -c "import json; v = json.load(open('rytm/$d/vst.json'))['version']; print('%d.%d.%d' % (v // 10000, v // 1000 % 10, v % 1000))")
    [ "$v" = "$VERSION" ] || die "rytm/$d/vst.json's version is $v, not $VERSION (keep kVersion in rytm/rytm_vst.cpp the same)"
done

echo "== 1. Engine and plugin tests (x86)"
python3 rytm/check_params.py
make -s rytm-test

echo "== 2. MPC builds: armv7 (Force and the other Gen1 MPCs) and arm64, both plugins"
make -s rytm-arm
if command -v qemu-arm >/dev/null && command -v qemu-aarch64 >/dev/null; then make -s rytm-test-arm; else echo "(qemu not found: skipping the ARM tests)"; fi

echo "== 3. Check the binaries MPC will load"
check_elf() {  # <.so> <readelf> <loader> <machine>
    local elf=$1 readelf=$2 loader=$3 machine=$4 exports needed glibc
    command -v "$readelf" >/dev/null || readelf=readelf
    $readelf -h "$elf" | grep -q "Machine: *$machine" || die "$elf is not built for $machine"
    exports=$($readelf --dyn-syms -W "$elf" | awk '$7 != "UND" && $5 == "GLOBAL" {print $8}' | tr '\n' ' ')
    [ "$exports" = "VSTPluginMain " ] || die "$elf should export only VSTPluginMain, got: $exports"
    needed=$($readelf -d "$elf" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | sort | tr '\n' ' ')
    for lib in $needed; do
        case "$lib" in libc.so.6|libm.so.6|libpthread.so.0|"$loader") ;; *) die "$elf: unexpected library: $lib" ;; esac
    done
    $readelf -d "$elf" | grep -q "NODELETE" || die "$elf must be linked with -z nodelete (the engine's threads run its code)"
    glibc=$($readelf -V "$elf" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -n 1)
    [ "$(printf '%s\n%s\n' "$glibc" "$GLIBC_MAX" | sort -V | tail -n 1)" = "$GLIBC_MAX" ] ||
        die "$elf needs GLIBC_$glibc, more than $GLIBC_MAX"
    echo "$elf: exports $exports| needs $needed| highest glibc symbol $glibc"
}
for p in track send; do
    check_elf build/armhf/sk_rytm_$p.so arm-linux-gnueabihf-readelf ld-linux-armhf.so.3 ARM
    check_elf build/arm64/sk_rytm_$p.so aarch64-linux-gnu-readelf ld-linux-aarch64.so.1 AArch64
done

echo "== 4. Skins"
rm -rf "$B"
mkdir -p "$B/notices" dist
cp "$MPC_VST/LICENSE" "$B/notices/LICENSE-mpc-vst-plugins.txt"
cp "$MPC_VST/tools/html_art/fonts/OFL.txt" "$B/notices/OFL-TitilliumWeb.txt"
cp rytm/NOTICE-overwitch.txt "$B/notices/"
pad=$(echo "$VERSION.0.0.0" | cut -d. -f1-4)
for p in track send; do
    rm -rf rytm/$p/build && mkdir -p rytm/$p/build/preview
    gcc -O2 -w -I"$MPC_VST/tools/vendor/force-shadow/tools" -o rytm/$p/build/shadow_art "$MPC_VST/tools/shadow_art.c" -lm
    SHADOW_TITLE_FONT="$MPC_VST/tools/html_art/fonts/TitilliumWeb-SemiBold.ttf" python3 "$MPC_VST/tools/gen_vst.py" rytm/$p/vst.json
    name=$(python3 -c "import json; print(json.load(open('rytm/$p/vst.json'))['name'])")
    skin="rytm/$p/build/skin/$VENDOR - VST - $name"
    python3 "$MPC_VST/tools/studio.py" preview "$skin/Plugin Skins" -o rytm/$p/build/preview/page_%d.png
    printf '<?xml version="1.0" encoding="utf-8"?>\n<plugincontent version="1.0">\n    <version>%s</version>\n    <uiversion>0.1.0.0</uiversion>\n</plugincontent>\n' \
        "$pad" > "$skin/version.xml"
done

echo "== 5. Packages"
# 5a. Akai SDK folders (MPC OS 3.10.1+), both in one zip.
mkdir -p "$B/sdk"
sdk_folder() {  # <track|send> <slug> <kind> <inputs> <uid> <category> <description>
    local p=$1 slug=$2 kind=$3 ins=$4 uid=$5 cat=$6 desc=$7 name dir
    name=$(python3 -c "import json; print(json.load(open('rytm/$p/vst.json'))['name'])")
    dir="$B/sdk/$VENDOR - VST - $name"
    mkdir -p "$dir/VST"
    cp build/armhf/sk_rytm_$p.so "$dir/VST/$slug-arm32.so"
    cp build/arm64/sk_rytm_$p.so "$dir/VST/$slug-arm64.so"
    cp -r "rytm/$p/build/skin/$VENDOR - VST - $name/Plugin Skins" "rytm/$p/build/skin/$VENDOR - VST - $name/version.xml" "$dir/"
    cp -r "$B/notices" "$dir/notices"
    cat > "$dir/plugin.json" <<JSON
{
    "name": "$name",
    "manufacturer": "$VENDOR",
    "version": "$VERSION",
    "format": "vst2",
    "kind": "$kind",
    "uid": "$uid",
    "numInputs": $ins,
    "numOutputs": 2,
    "category": "$cat",
    "file32": "VST/$slug-arm32.so",
    "file64": "VST/$slug-arm64.so",
    "description": "$desc"
}
JSON
    python3 -I mpc/check_folder.py "$dir" "$(python3 -c "import json; print(len(json.load(open('rytm/$p/params.json'))['params']))")"
}
sdk_folder track sk-rytm-track instrument 0 ShRt "Overbridge" \
    "One Analog Rytm MKII output (Main, a track, or the input) on this track, over Overbridge USB"
sdk_folder send sk-rytm-send effect 2 ShRs "Overbridge" \
    "Sends this track's audio into an Analog Rytm MKII input (Main, FX or a track) over Overbridge USB"
cp rytm/INSTALL.txt "$B/sdk/INSTALL.txt"
sed -i "s/@VERSION@/$VERSION/g" "$B/sdk/INSTALL.txt"
rm -f "dist/SK-Rytm-$VERSION-mpc-sdk.zip"
(cd "$B/sdk" && zip -X -r -q "$ROOT/dist/SK-Rytm-$VERSION-mpc-sdk.zip" "$VENDOR - VST - SK Rytm Track" "$VENDOR - VST - SK Rytm Send" INSTALL.txt)

# 5b. MPC.settings installers (MPC OS 3.10.0 and older, MockbaMod): one zip per plugin.
for p in track send; do
    name=$(python3 -c "import json; print(json.load(open('rytm/$p/vst.json'))['name'])")
    zipname="SK-Rytm-${p^}-$VERSION-mpc-armv7.zip"
    rm -f "dist/$zipname"
    python3 "$MPC_VST/tools/release.py" --so build/armhf/sk_rytm_$p.so --skin "rytm/$p/build/skin/$VENDOR - VST - $name" \
        --entry rytm/$p/build/pluginlist-entry.xml --version "$VERSION" --extra "$B/notices:notices" \
        --about "$(python3 -c "import json; print(json.load(open('rytm/$p/vst.json')).get('about', ''))")" \
        --requires "Root SSH access to the Force (e.g. MockbaMod); MPC OS 3.10.0 or older; an Analog Rytm MKII in Overbridge mode on the MPC's USB host port" \
        -o dist
    made=$(ls -t dist/*-mpc-armv7.zip | head -n 1)
    [ "$made" = "dist/$zipname" ] || mv "$made" "dist/$zipname"
    python3 "$MPC_VST/tools/catalog_check.py" "dist/$zipname" || echo "(catalog_check reported the above)"
done

echo "Done: dist/SK-Rytm-$VERSION-mpc-sdk.zip (MPC OS 3.10.1+), dist/SK-Rytm-Track-$VERSION-mpc-armv7.zip and"
echo "      dist/SK-Rytm-Send-$VERSION-mpc-armv7.zip (3.10.0 and older). Previews: rytm/{track,send}/build/preview/"
