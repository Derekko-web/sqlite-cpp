#!/bin/sh
# Build and run the local executable.
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
(
  cd "$project_dir"
  if [ -n "${VCPKG_ROOT:-}" ]; then
    cmake -S . -B build "-DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
  else
    cmake -S . -B build
  fi
  cmake --build build
)
exec "$project_dir/build/sqlite" "$@"
