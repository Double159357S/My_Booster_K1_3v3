#!/bin/bash
set -e -o pipefail

cd -- "$(dirname -- "$0")/.."

export PYTHONWARNINGS="ignore::DeprecationWarning,ignore::UserWarning,ignore::FutureWarning"

# A previous copy install can leave real directories where ament now needs links.
# Cached CMake settings can keep symlink mode enabled even without the CLI flag.
python3 - "$@" <<'PY'
from datetime import datetime, timezone
from pathlib import Path
import sys
import uuid
from scripts.deployment.robot_sync import prepare_symlink_build

run_id = 'manual-build-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ') + '-' + uuid.uuid4().hex[:8]
result = prepare_symlink_build(Path.cwd(), run_id, cached_only='--symlink-install' not in sys.argv[1:])
if result['moved']:
    print(f"Backed up {result['moved']} build conflict(s): {result['backup']}", flush=True)
PY

colcon build --executor parallel --parallel-workers "$(nproc)" "$@"

espeak "build complete" >/dev/null 2>&1 || echo "Build complete"
