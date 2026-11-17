#!/usr/bin/env bash
# Build SK TI Link (Access Virus TI over USB) for MPC OS as two zips:
#
#   virus/mpc/build.sh [version]          default version: 0.1.0
#
# dist/SK-TI-Link-<version>-mpc-sdk.zip    MPC OS 3.10.1 and later, every MPC: the "Sher Khan - VST - SK TI Link" folder
#                                          in Akai's MPC Plugin Builder SDK format (plugin.json, armv7 + arm64 builds,
#                                          the skin), for a "VST2 Plugins" folder on a drive. No root needed.
# dist/SK-TI-Link-<version>-mpc-armv7.zip  MPC OS 3.10.0 and older (Force and the other Gen1 MPCs): the folder plus
#                                          install.sh, which registers it in MPC.settings (root SSH, e.g. MockbaMod).
#
# Needs what the repository's mpc/build.sh needs (ARM cross compilers, qemu-user optional, python3 with Pillow, zip and
# the mpc-vst-plugins checkout at ../mpc-vst-plugins or $MPC_VST).
set -euo pipefail
cd "$(dirname "$0")/.."
VIRUS=$(pwd)
ROOT=$(cd .. && pwd)
VERSION="${1:-0.1.0}"
MPC_VST_REF=3a54b72cd172c840881b0be4f887653cc0829bf0
MPC_VST="${MPC_VST:-$ROOT/../mpc-vst-plugins}"
SO=sk_ti_link.so
NAME="SK TI Link"
VENDOR="Sher Khan"
SKIN="$VENDOR - VST - $NAME"
SLUG=sk-ti-link
NPARAMS=390         # the 17 LINK controls and the 373 Virus parameters
GLIBC_MAX=2.31
B=mpc/build

die() { echo "error: $*" >&2; exit 1; }
[ -f "$MPC_VST/tools/release.py" ] || die "no mpc-vst-plugins checkout at $MPC_VST"
have=$(git -C "$MPC_VST" rev-parse HEAD 2>/dev/null || echo unknown)
[ "$have" = "$MPC_VST_REF" ] || echo "warning: mpc-vst-plugins is at $have, this was checked with $MPC_VST_REF"
[ "$(python3 -c 'import json; v = json.load(open("mpc/vst.json"))["version"]; print("%d.%d.%d" % (v // 10000, v // 1000 % 10, v % 1000))')" = "$VERSION" ] ||
    die "mpc/vst.json's version is not $VERSION (keep kVersion in sk_ti.cpp the same)"

echo "== 1. Parameter table and pages are up to date"
python3 -I gen.py
git -C "$ROOT" diff --quiet -- virus/ti_table.h virus/mpc/params.json virus/mpc/layout.conf ||
    echo "warning: gen.py changed ti_table.h / params.json / layout.conf: commit them"

echo "== 2. Tests (x86), MPC builds (armv7, arm64), tests under qemu"
make -s test
make -s force arm64
for t in test-force:qemu-arm test-arm64:qemu-aarch64; do
    if command -v "${t#*:}" >/dev/null; then make -s "${t%%:*}"; else echo "(${t#*:} not found: skipping make ${t%%:*})"; fi
done

echo "== 3. Check the binaries MPC will load"
check_elf() {  # <.so> <readelf> <loader> <machine>
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

echo "== 4. Skin"
rm -rf "$B"
mkdir -p "$B/preview"
gcc -O2 -w -I"$MPC_VST/tools/vendor/force-shadow/tools" -o "$B/shadow_art" "$MPC_VST/tools/shadow_art.c" -lm
SHADOW_TITLE_FONT="$MPC_VST/tools/html_art/fonts/TitilliumWeb-SemiBold.ttf" python3 "$MPC_VST/tools/gen_vst.py" mpc/vst.json
python3 "$MPC_VST/tools/studio.py" preview "$B/skin/$SKIN/Plugin Skins" -o "$B/preview/page_%d.png" >/dev/null
pad=$(echo "$VERSION.0.0.0" | cut -d. -f1-4)
printf '<?xml version="1.0" encoding="utf-8"?>\n<plugincontent version="1.0">\n    <version>%s</version>\n    <uiversion>0.1.0.0</uiversion>\n</plugincontent>\n' \
    "$pad" > "$B/skin/$SKIN/version.xml"

echo "== 5. Packages"
mkdir -p "$B/notices" "$ROOT/dist"
cp LICENSE "$B/notices/LICENSE-GPL-3.0.txt"
cp "$MPC_VST/LICENSE" "$B/notices/LICENSE-mpc-vst-plugins.txt"
cp "$MPC_VST/tools/html_art/fonts/OFL.txt" "$B/notices/OFL-TitilliumWeb.txt"
cat > "$B/notices/NOTICE.txt" <<NOTICE
SK TI Link is free software under the GNU General Public License, version 3 (LICENSE-GPL-3.0.txt).
Its parameter table was derived from gearmulator's Virus TI parameter descriptions (GPL-3.0,
https://github.com/dsp56300/gearmulator). Source: the Sher_Khan- repository, folder virus/.
Not made or endorsed by Access Music or Akai. Virus and Virus TI are trademarks of their owners.
NOTICE

SDKDIR="$B/sdk/$SKIN"
mkdir -p "$SDKDIR/VST"
cp build/armhf/$SO "$SDKDIR/VST/$SLUG-arm32.so"
cp build/arm64/$SO "$SDKDIR/VST/$SLUG-arm64.so"
cp -r "$B/skin/$SKIN/Plugin Skins" "$B/skin/$SKIN/version.xml" "$SDKDIR/"
cp -r "$B/notices" "$SDKDIR/notices"
cat > "$SDKDIR/plugin.json" <<JSON
{
    "name": "$NAME",
    "manufacturer": "$VENDOR",
    "version": "$VERSION",
    "format": "vst2",
    "kind": "instrument",
    "uid": "ShTl",
    "numInputs": 0,
    "numOutputs": 2,
    "category": "Synth",
    "file32": "VST/$SLUG-arm32.so",
    "file64": "VST/$SLUG-arm64.so",
    "description": "Editor and remote for an Access Virus TI / TI2 connected to the MPC by USB (MIDI): every Single parameter, read / send / load / store, notes"
}
JSON
cat > "$B/sdk/INSTALL.txt" <<TXT
SK TI Link $VERSION for MPC OS 3.10.1 and later (Force, MPC Live / One / X / Key, and the 64-bit MPCs)

1. Copy the folder "$SKIN" into a folder named exactly "VST2 Plugins" at the top of a drive
   (SD card, USB drive or the internal drive). Create "VST2 Plugins" if it isn't there.
2. On the MPC: Preferences > Enhancements, turn on "Enable Enhancements" and "External Plugin Support".
3. Connect the Virus TI's USB port to a USB host port of the MPC, switch the Virus on, restart the MPC.
4. Add SK TI Link (Sher Khan) as a plugin instrument on a track. The LINK page says whether it found the Virus.
5. Audio: the Virus TI's USB audio does not work with MPC OS (no Linux driver exists). Cable the Virus's analog
   outputs to the MPC's inputs and monitor them on an audio track.

On MPC OS 3.10.0 or older, use SK-TI-Link-$VERSION-mpc-armv7.zip instead (root SSH, e.g. MockbaMod).
TXT
python3 -I "$ROOT/mpc/check_folder.py" "$SDKDIR" $NPARAMS
rm -f "$ROOT/dist/SK-TI-Link-$VERSION-mpc-sdk.zip"
(cd "$B/sdk" && zip -X -r -q "$ROOT/dist/SK-TI-Link-$VERSION-mpc-sdk.zip" "$SKIN" INSTALL.txt)

rm -f "$ROOT/dist/SK-TI-Link-$VERSION-mpc-armv7.zip"
python3 "$MPC_VST/tools/release.py" --so build/armhf/$SO --skin "$B/skin/$SKIN" --entry "$B/pluginlist-entry.xml" \
    --version "$VERSION" --extra "$B/notices:notices" --license GPL-3.0-only \
    --repo davidpinault2-crypto/Sher_Khan- \
    --about "Editor and remote for an Access Virus TI / TI2 connected by USB (MIDI): every Single parameter on 15 pages, READ / SEND / LOAD / STORE, notes from the track, the patch saved with the project. Audio comes back through the Virus's analog outputs. On MPC OS 3.10.1 and later use the -mpc-sdk zip instead." \
    --requires "Root SSH access to the Force (e.g. MockbaMod); MPC OS 3.10.0 or older; an Access Virus TI / TI2 on USB" -o "$ROOT/dist"
python3 "$MPC_VST/tools/catalog_check.py" "$ROOT/dist/SK-TI-Link-$VERSION-mpc-armv7.zip" || echo "(catalog check: see above)"
rm -rf "$B/legacy" && mkdir -p "$B/legacy" && unzip -q "$ROOT/dist/SK-TI-Link-$VERSION-mpc-armv7.zip" -d "$B/legacy"
python3 -I "$ROOT/mpc/check_folder.py" "$B/legacy/SK-TI-Link-$VERSION/portable/$SKIN" $NPARAMS --legacy

echo "Done: dist/SK-TI-Link-$VERSION-mpc-sdk.zip (MPC OS 3.10.1+), dist/SK-TI-Link-$VERSION-mpc-armv7.zip (3.10.0 and older)"
