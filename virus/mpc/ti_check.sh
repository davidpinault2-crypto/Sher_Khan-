#!/bin/sh
# SK TI Link: what MPC OS sees of a Virus TI on USB. Run on the MPC over SSH (MockbaMod): sh ti_check.sh
# Prints the USB device (vendor 133e = Access), whether Linux gave it a MIDI driver, and the sequencer ports.
echo "== USB devices from Access (vendor 133e)"
found=0
for d in /sys/bus/usb/devices/*; do
    [ -f "$d/idVendor" ] || continue
    if [ "$(cat "$d/idVendor")" = "133e" ]; then
        found=1
        echo "$(basename "$d"): 133e:$(cat "$d/idProduct") $(cat "$d/product" 2>/dev/null)"
        ls -d "$d":* 2>/dev/null | while read -r i; do
            echo "  interface $(basename "$i"): class $(cat "$i/bInterfaceClass") driver $(basename "$(readlink "$i/driver" 2>/dev/null)" 2>/dev/null)"
        done
    fi
done
[ $found = 1 ] || echo "none: is the Virus on and its USB cable in a USB host port of the MPC?"
echo "== ALSA cards"
cat /proc/asound/cards
echo "== Sequencer clients"
cat /proc/asound/seq/clients 2>/dev/null | grep -E "^Client|Port" || echo "(no /proc/asound/seq/clients)"
echo "== Raw MIDI devices"
ls -l /dev/snd/ | grep -i midi || echo "none"
echo "== Who has them open"
for f in /dev/snd/midiC*; do [ -e "$f" ] && { echo "$f:"; ls -l /proc/[0-9]*/fd 2>/dev/null | grep -F "$f" | head -3; }; done
uname -r
