#!/bin/bash
set -e -o pipefail

exec bash "$(dirname -- "$0")/build.sh" --symlink-install --packages-select brain "$@"
