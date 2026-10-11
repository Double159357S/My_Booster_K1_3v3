#!/bin/bash
set -e -o pipefail

cd -- "$(dirname -- "$0")/.."

export PYTHONWARNINGS="ignore::DeprecationWarning,ignore::UserWarning,ignore::FutureWarning"

# colcon build  --symlink-install  --parallel-workers $(nproc) "$@"
colcon build --executor parallel --parallel-workers "$(nproc)" "$@"

espeak "build complete" >/dev/null 2>&1 || echo "Build complete"
