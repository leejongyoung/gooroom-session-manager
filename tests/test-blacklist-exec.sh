#!/bin/sh
# Linux: gcc, pkg-config, libglib2.0-dev, libjson-c-dev
set -eu
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
cc -Wall -Wextra -Wno-unused-parameter -ffunction-sections -fdata-sections \
  -Wl,--gc-sections tests/blacklist-exec.c -o "$build_dir/test" \
  $(pkg-config --cflags --libs gio-unix-2.0 json-c)
for case in invalid-desktop invalid-exec missing-program normal; do
  "$build_dir/test" "$case"
done
