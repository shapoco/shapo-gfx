#!/bin/bash
# Write a demo to the board with picotool: ./flash.sh demorig (or a .uf2 /
# .elf path). Hold BOOT while plugging the USB cable in, or let picotool
# reboot a running demo (-f: the demos are built with stdio over USB).
set -e
cd "$(dirname "$0")"
ARG=${1:-demorig}
if [ -d "$ARG" ]; then FILE="$ARG/build/$(basename "$ARG").uf2"; else FILE=$ARG; fi
PICOTOOL=${PICOTOOL:-$(command -v picotool || ls -d "$HOME"/.pico-sdk/picotool/*/picotool/picotool 2>/dev/null | sort | tail -1)}
"$PICOTOOL" load -f -x "$FILE"
