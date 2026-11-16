#!/usr/bin/env bash
# Build SK Filterbank for MPC OS (Akai Force and the other MPCs) as two zips:
#
#   mpc/build.sh [version]          default version: 0.4.0
#
# dist/SK-Filterbank-<version>-mpc-sdk.zip    MPC OS 3.10.1 and later, every MPC: the "Sher Khan - VST - SK Filterbank"
#                                             folder in the format of Akai's MPC Plugin Builder SDK (plugin.json, the
#                                             armv7 and arm64 builds, the skin), for a "VST2 Plugins" folder on a
#                                             drive. No root access needed.
# dist/SK-Filterbank-<version>-mpc-armv7.zip  MPC OS 3.10.0 and older on the Force and the other Gen1 MPCs: the folder
#                                             plus install.sh, which registers it in MPC.settings (root SSH, e.g.
#                                             MockbaMod). See mpc/README.md.
#
# Needs: arm-linux-gnueabihf-g++/gcc and aarch64-linux-gnu-g++/gcc (Debian/Ubuntu: g++-arm-linux-gnueabihf,
# g++-aarch64-linux-gnu), a host gcc/g++, python3 with Pillow, zip, and a checkout of the community toolkit (MIT) for
# the skin builder and the legacy installer:
#   git clone https://github.com/open-mpc/mpc-vst-plugins ../mpc-vst-plugins && git -C ../mpc-vst-plugins checkout $MPC_VST_REF
# or point MPC_VST at one. qemu-arm and qemu-aarch64 (qemu-user) are optional: with them, the DSP and host tests also
# run against both ARM builds.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)
VERSION="${1:-0.4.0}"
MPC_VST_REF=3a54b72cd172c840881b0be4f887653cc0829bf0   # the toolkit commit the skin and installer were built and checked with
MPC_VST="${MPC_VST:-$ROOT/../mpc-vst-plugins}"
SO=sk_filterbank.so
NAME="SK Filterbank"
VENDOR="Sher Khan"
SKIN="$VENDOR - VST - $NAME"
SLUG=sk-filterbank
NPARAMS=36          # VST parameters: the 29 sound parameters and the PRESETS page's 7 controls
GLIBC_MAX=2.31      # Akai's SDK targets glibc 2.31; MPC OS 2.x has 2.32, 3.10.1 has 2.39
B=mpc/build

die() { echo "error: $*" >&2; exit 1; }
[ -f "$MPC_VST/tools/release.py" ] || die "no mpc-vst-plugins checkout at $MPC_VST (see the header of this script)"
have=$(git -C "$MPC_VST" rev-parse HEAD 2>/dev/null || echo unknown)
[ "$have" = "$MPC_VST_REF" ] || echo "warning: mpc-vst-plugins is at $have, this was checked with $MPC_VST_REF"
python3 -c 'import PIL' 2>/dev/null || die "python3 needs Pillow (pip install pillow)"
command -v zip >/dev/null || die "zip not found"
[ "$(python3 -c 'import json; v = json.load(open("mpc/vst.json"))["version"]; print("%d.%d.%d" % (v // 10000, v // 1000 % 10, v % 1000))')" = "$VERSION" ] ||
    die "mpc/vst.json's version is not $VERSION (it is also what the plugin reports: keep kVersion in mpc/sk_vst.cpp the same)"

echo "== 1. DSP and plugin tests (x86)"
make -s test

echo "== 2. MPC builds: armv7 (Force and the other Gen1 MPCs) and arm64"
make -s force arm64
for t in test-force:qemu-arm test-arm64:qemu-aarch64; do
    if command -v "${t#*:}" >/dev/null; then make -s "${t%%:*}"; else echo "(${t#*:} not found: skipping make ${t%%:*})"; fi
done

echo "== 3. Check the binaries MPC will load"
check_elf() {  # <.so> <readelf> <loader> <machine as readelf names it>
    local elf=$1 readelf=$2 loader=$3 machine=$4 exports needed glibc
    command -v "$readelf" >/dev/null || readelf=readelf
    $readelf -h "$elf" | grep -q "Machine: *$machine" || die "$elf is not built for $machine"
    exports=$($readelf --dyn-syms -W "$elf" | awk '$7 != "UND" && $5 == "GLOBAL" {print $8}' | tr '\n' ' ')
    [ "$exports" = "VSTPluginMain " ] || die "$elf should export only VSTPluginMain, got: $exports"
    needed=$($readelf -d "$elf" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | sort | tr '\n' ' ')
    for lib in $needed; do
        case "$lib" in libc.so.6|libm.so.6|"$loader") ;; *) die "$elf: unexpected library dependency: $lib" ;; esac
    done
    glibc=$($readelf -V "$elf" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -n 1)
    [ "$(printf '%s\n%s\n' "$glibc" "$GLIBC_MAX" | sort -V | tail -n 1)" = "$GLIBC_MAX" ] ||
        die "$elf needs GLIBC_$glibc, more than $GLIBC_MAX"
    echo "$elf: exports $exports| needs $needed| highest glibc symbol $glibc"
}
check_elf build/armhf/$SO arm-linux-gnueabihf-readelf ld-linux-armhf.so.3 ARM
arm-linux-gnueabihf-readelf -h build/armhf/$SO | grep -q "hard-float ABI" || die "the armv7 build is not hard-float"
check_elf build/arm64/$SO aarch64-linux-gnu-readelf ld-linux-aarch64.so.1 AArch64

echo "== 4. Skin (mpc-vst-plugins shadow_skin, Titillium titles, colour-coded knobs from mpc/skin_post.py)"
python3 mpc/check_params.py
rm -rf "$B"
mkdir -p "$B/preview"
gcc -O2 -w -I"$MPC_VST/tools/vendor/force-shadow/tools" -o "$B/shadow_art" "$MPC_VST/tools/shadow_art.c" -lm
SHADOW_TITLE_FONT="$MPC_VST/tools/html_art/fonts/TitilliumWeb-SemiBold.ttf" python3 "$MPC_VST/tools/gen_vst.py" mpc/vst.json
python3 "$MPC_VST/tools/studio.py" preview "$B/skin/$SKIN/Plugin Skins" -o "$B/preview/page_%d.png"
# version.xml as Akai's SDK writes it: MPC OS 3.10.1 wants <uiversion> to use the Plugin Skins (the toolkit's has none).
pad=$(echo "$VERSION.0.0.0" | cut -d. -f1-4)
printf '<?xml version="1.0" encoding="utf-8"?>\n<plugincontent version="1.0">\n    <version>%s</version>\n    <uiversion>0.1.0.0</uiversion>\n</plugincontent>\n' \
    "$pad" > "$B/skin/$SKIN/version.xml"

echo "== 5. Packages"
mkdir -p "$B/notices" dist
cp "$MPC_VST/LICENSE" "$B/notices/LICENSE-mpc-vst-plugins.txt"
cp "$MPC_VST/tools/html_art/fonts/OFL.txt" "$B/notices/OFL-TitilliumWeb.txt"

# 5a. Akai SDK folder (MPC OS 3.10.1+): plugin.json, VST/<slug>-arm32.so and -arm64.so, version.xml, Plugin Skins.
SDKDIR="$B/sdk/$SKIN"
mkdir -p "$SDKDIR/VST"
cp build/armhf/$SO "$SDKDIR/VST/$SLUG-arm32.so"
cp build/arm64/$SO "$SDKDIR/VST/$SLUG-arm64.so"
cp -r "$B/skin/$SKIN/Plugin Skins" "$B/skin/$SKIN/version.xml" "$SDKDIR/"
cp -r "$B/notices" "$SDKDIR/notices"
cat > "$SDKDIR/plugin.json" <<EOF
{
    "name": "$NAME",
    "manufacturer": "$VENDOR",
    "version": "$VERSION",
    "format": "vst2",
    "kind": "effect",
    "uid": "ShKf",
    "numInputs": 2,
    "numOutputs": 2,
    "category": "Effect",
    "file32": "VST/$SLUG-arm32.so",
    "file64": "VST/$SLUG-arm64.so",
    "description": "Two multimode filters with overdrive, envelope, LFO, FM and AM, modelled on the Sherman Filterbank 2"
}
EOF
cat > "$B/sdk/INSTALL.txt" <<EOF
SK Filterbank $VERSION for MPC OS 3.10.1 and later (Force, MPC Live / One / X / Key, and the 64-bit MPCs)

Install
1. Copy the folder "$SKIN" into a folder named exactly "VST2 Plugins" at the top of a drive: an SD card,
   a USB drive, or the MPC's internal drive (in Computer Mode, or with the MPC's file browser). Create
   "VST2 Plugins" if it isn't there yet.
2. On the MPC: Preferences > Enhancements, turn on "Enable Enhancements" and "External Plugin Support".
3. Restart the MPC. Add SK Filterbank as an insert effect: it is listed under the external plugins (Sher Khan).

Update: copy the new folder over the old one, then restart the MPC. Your User presets (presets/ in the folder) stay.
Coming from the MPC.settings install of MPC OS 3.10.0 and older: projects that used it keep working, and the first
start copies your User presets over from /sdcard/Synths/$SKIN/presets if they are there.
Uninstall: delete the folder.

On MPC OS 3.10.0 or older, use SK-Filterbank-$VERSION-mpc-armv7.zip instead (it needs root SSH, e.g. MockbaMod).
EOF
python3 -I mpc/check_folder.py "$SDKDIR" $NPARAMS
rm -f "dist/SK-Filterbank-$VERSION-mpc-sdk.zip"
(cd "$B/sdk" && zip -X -r -q "$ROOT/dist/SK-Filterbank-$VERSION-mpc-sdk.zip" "$SKIN" INSTALL.txt)

# 5b. The MPC.settings installer (MPC OS 3.10.0 and older): the armv7 .so at the folder's root, as in every release
# before, so the plugin's path in existing projects and .xpl presets stays the same.
rm -f dist/SK-Filterbank-"$VERSION"-mpc-armv7.zip
python3 "$MPC_VST/tools/release.py" --so build/armhf/$SO --skin "$B/skin/$SKIN" --entry "$B/pluginlist-entry.xml" \
    --version "$VERSION" --extra "$B/notices:notices" --user-data presets \
    --about "Two multimode filters with overdrive, ADSR/AR/follower, LFO, FM and AM, laid out like the Sherman Filterbank 2 (modelled from public descriptions, no hardware measurements yet). Your User presets live in presets/ inside the plugin folder and survive upgrades. On MPC OS 3.10.1 and later use the -mpc-sdk zip instead." \
    --requires "Root SSH access to the Force (e.g. MockbaMod); MPC OS 3.10.0 or older" -o dist
python3 "$MPC_VST/tools/catalog_check.py" dist/SK-Filterbank-"$VERSION"-mpc-armv7.zip
rm -rf "$B/legacy" && mkdir -p "$B/legacy" && unzip -q dist/SK-Filterbank-"$VERSION"-mpc-armv7.zip -d "$B/legacy"
python3 -I mpc/check_folder.py "$B/legacy/SK-Filterbank-$VERSION/portable/$SKIN" $NPARAMS --legacy

echo "Done: dist/SK-Filterbank-$VERSION-mpc-sdk.zip (MPC OS 3.10.1+), dist/SK-Filterbank-$VERSION-mpc-armv7.zip (3.10.0 and older)"
echo "      skin preview: $B/preview/page_0.png"
