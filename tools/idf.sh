#!/bin/zsh
# ESP-IDF wrapper: sources the IDF environment, then runs idf.py with the given args.
# Usage: tools/idf.sh [idf.py args...]
#   tools/idf.sh build
#   tools/idf.sh flash monitor
#   ESPPORT=/dev/cu.usbmodem123 tools/idf.sh flash   # force a specific serial port
export IDF_PATH="$HOME/esp/esp-idf"
source "$IDF_PATH/export.sh" >/dev/null
if [ -n "$ESPPORT" ]; then
  exec idf.py -p "$ESPPORT" "$@"
else
  exec idf.py "$@"
fi
