#!/usr/bin/env bash
# Build SK Filterbank for the Akai Force (MPC OS 3.x, Gen1 armv7) as an installable zip:
#
#   mpc/build.sh [version]          default version: 0.3.0
#
# Output: dist/SK-Filterbank-<version>-mpc-armv7.zip (plugin folder + install.sh/uninstall.sh, see mpc/README.md).
#
# Needs: arm-linux-gnueabihf-g++/gcc (Debian/Ubuntu: g++-arm-linux-gnueabihf), a host gcc/g++, python3 with Pillow,
# and a checkout of the community toolkit (MIT) for the skin builder and the installer:
#   git clone https://github.com/sd88me/mpc-vst-plugins ../mpc-vst-plugins && git -C ../mpc-vst-plugins checkout $MPC_VST_REF
# or point MPC_VST at one. qemu-arm (qemu-user) is optional: with it, the host test also runs against the ARM build.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)
VERSION="${1:-0.3.0}"
MPC_VST_REF=3a54b72cd172c840881b0be4f887653cc0829bf0   # the toolkit commit this was built and checked with
MPC_VST="${MPC_VST:-$ROOT/../mpc-vst-plugins}"
SO=sk_filterbank.so
SKIN="Sher Khan - VST - SK Filterbank"
B=mpc/build

die() { echo "error: $*" >&2; exit 1; }
[ -f "$MPC_VST/tools/release.py" ] || die "no mpc-vst-plugins checkout at $MPC_VST (see the header of this script)"
have=$(git -C "$MPC_VST" rev-parse HEAD 2>/dev/null || echo unknown)
[ "$have" = "$MPC_VST_REF" ] || echo "warning: mpc-vst-plugins is at $have, this was checked with $MPC_VST_REF"
python3 -c 'import PIL' 2>/dev/null || die "python3 needs Pillow (pip install pillow)"

echo "== 1. DSP and plugin tests (x86)"
make -s test

echo "== 2. Force build (armv7 hard-float)"
make -s force
if command -v qemu-arm >/dev/null; then
    make -s test-force
else
    echo "(qemu-arm not found: skipping the host test against the ARM build)"
fi

echo "== 3. Check the binary MPC will load"
ELF=build/armhf/$SO
READELF=arm-linux-gnueabihf-readelf
command -v $READELF >/dev/null || READELF=readelf
exports=$($READELF --dyn-syms -W "$ELF" | awk '$7 != "UND" && $5 == "GLOBAL" {print $8}' | tr '\n' ' ')
[ "$exports" = "VSTPluginMain " ] || die "exports should be only VSTPluginMain, got: $exports"
needed=$($READELF -d "$ELF" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | sort | tr '\n' ' ')
for lib in $needed; do
    case "$lib" in libc.so.6|libm.so.6|ld-linux-armhf.so.3) ;; *) die "unexpected library dependency: $lib" ;; esac
done
glibc=$($READELF -V "$ELF" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -n 1)
[ "$(printf '%s\n2.32\n' "$glibc" | sort -V | tail -n 1)" = 2.32 ] || die "needs GLIBC_$glibc; MPC OS 2.x has 2.32"
$READELF -h "$ELF" | grep -q "hard-float ABI" || die "not a hard-float build"
echo "exports: $exports| needs: $needed| highest glibc symbol: $glibc (Force: 2.39)"

echo "== 4. Skin (mpc-vst-plugins shadow_skin, Titillium titles, colour-coded knobs from mpc/skin_post.py)"
python3 mpc/check_params.py
rm -rf "$B"
mkdir -p "$B/preview"
gcc -O2 -w -I"$MPC_VST/tools/vendor/force-shadow/tools" -o "$B/shadow_art" "$MPC_VST/tools/shadow_art.c" -lm
SHADOW_TITLE_FONT="$MPC_VST/tools/html_art/fonts/TitilliumWeb-SemiBold.ttf" python3 "$MPC_VST/tools/gen_vst.py" mpc/vst.json
python3 "$MPC_VST/tools/studio.py" preview "$B/skin/$SKIN/Plugin Skins" -o "$B/preview/page_%d.png"

echo "== 5. Package"
mkdir -p "$B/notices"
cp "$MPC_VST/LICENSE" "$B/notices/LICENSE-mpc-vst-plugins.txt"
cp "$MPC_VST/tools/html_art/fonts/OFL.txt" "$B/notices/OFL-TitilliumWeb.txt"
rm -f dist/SK-Filterbank-"$VERSION"-mpc-armv7.zip
python3 "$MPC_VST/tools/release.py" --so "$ELF" --skin "$B/skin/$SKIN" --entry "$B/pluginlist-entry.xml" \
    --version "$VERSION" --extra "$B/notices:notices" --user-data presets \
    --about "Two multimode filters with overdrive, ADSR/AR/follower, LFO, FM and AM, laid out like the Sherman Filterbank 2 (modelled from public descriptions, no hardware measurements yet). Your User presets live in presets/ inside the plugin folder and survive upgrades." \
    --requires "Root SSH access to the Force (e.g. MockbaMod)" -o dist
python3 "$MPC_VST/tools/catalog_check.py" dist/SK-Filterbank-"$VERSION"-mpc-armv7.zip
echo "Done: dist/SK-Filterbank-$VERSION-mpc-armv7.zip  (skin preview: $B/preview/page_0.png)"
