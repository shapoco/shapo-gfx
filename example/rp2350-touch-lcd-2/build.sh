#!/bin/bash
# Build the demos of this directory (or the ones named) for the Waveshare
# RP2350-Touch-LCD-2: <demo>/build/<demo>.uf2. Extra arguments after "--" go
# to the cmake configure step, e.g. ./build.sh demorig -- -DDEMORIG_ATLAS=ON
#
# The Pico SDK comes from $PICO_SDK_PATH (default ~/pico/pico-sdk).
set -e
cd "$(dirname "$0")"
DEMOS=()
while [ $# -gt 0 ] && [ "$1" != "--" ]; do DEMOS+=("${1%/}"); shift; done
[ "$1" = "--" ] && shift
[ ${#DEMOS[@]} -eq 0 ] && DEMOS=(demo2d demo3d demorig)
for d in "${DEMOS[@]}"; do
  cmake -S "$d" -B "$d/build" -G Ninja "$@"
  cmake --build "$d/build"
done
