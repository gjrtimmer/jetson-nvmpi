#!/usr/bin/env bash
# Create placeholder Jetson codec device nodes for the mock test run.
#
# libnvmpi probes /dev/nvhost-nvdec | /dev/v4l2-nvdec (decoder) and
# /dev/nvhost-msenc | /dev/v4l2-nvenc (encoder) with access()/open() before it
# calls the MMAPI factory, and refuses to create a codec when none exists.
# On a mock runner there is no Tegra, so point those paths at /dev/null: the
# probe succeeds, and the real I/O goes through the mock libnvv4l2 (whose
# v4l2_open only looks at the path prefix and never touches the node).
#
# Idempotent: existing nodes (a real Jetson!) are never replaced.
# Usage: scripts/mock-dev-nodes.sh      (needs write access to /dev, i.e. root)
set -euo pipefail

nodes=(/dev/v4l2-nvdec /dev/v4l2-nvenc)

for n in "${nodes[@]}"; do
    if [ -e "${n}" ]; then
        echo "[i] ${n} exists, leaving it alone"
        continue
    fi
    if ! ln -s /dev/null "${n}" 2>/dev/null; then
        echo "[E] cannot create ${n} (need root / writable /dev)" >&2
        exit 1
    fi
    echo "[i] created ${n} -> /dev/null"
done
