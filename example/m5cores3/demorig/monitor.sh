#!/bin/bash
# Serial log: the frame rate and the time spent per frame every 2 seconds.
set -eux
IDF_ROOT="${IDF_ROOT:-${HOME}/esp/5.5}"
cd "$(dirname "$0")"
source "${IDF_ROOT}/esp-idf/export.sh"
if [ $# -ge 1 ]; then idf.py -p "$1" monitor; else idf.py monitor; fi
