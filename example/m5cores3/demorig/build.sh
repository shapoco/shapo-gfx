#!/bin/bash
# Build the firmware. IDF_ROOT is the ESP-IDF installation directory (the
# one that holds esp-idf/); v5.5 is what this is built against.
set -eux
IDF_ROOT="${IDF_ROOT:-${HOME}/esp/5.5}"
cd "$(dirname "$0")"
source "${IDF_ROOT}/esp-idf/export.sh"
idf.py build
