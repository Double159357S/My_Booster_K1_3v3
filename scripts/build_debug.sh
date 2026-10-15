#!/bin/bash
set -e -o pipefail

exec bash "$(dirname -- "$0")/build.sh" --symlink-install --cmake-args -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-g -fno-omit-frame-pointer" "$@"
