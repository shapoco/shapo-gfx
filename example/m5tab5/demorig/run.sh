#!/bin/bash
# Build and flash. Pass the port as the first argument, or let esptool find it.
set -eux
IDF_ROOT="${IDF_ROOT:-${HOME}/esp/5.5}"
cd "$(dirname "$0")"
source "${IDF_ROOT}/esp-idf/export.sh"
idf.py build
if [ $# -ge 1 ]; then idf.py -p "$1" flash; else idf.py flash; fi
