#!/bin/bash
# Host side of the dgc-27 build: starts the container and runs
# scripts/build-dgc27.sh inside it.  Nothing is built on the host, and the
# ship tree (mesa27-src) is never used as a source here - only the worktree.
#
#   scripts/build-dgc27-host.sh              # fresh configure + build
#   FRESH=0 scripts/build-dgc27-host.sh      # incremental
#
# The game must be closed: -j2 on eight weak cores is what makes a Wine
# session stutter.
set -euxo pipefail

ROOT="${ROOT:-/var/home/armada/opencode}"
SRC="${SRC:-$ROOT/mesa27-dgc}"
REPO="${REPO:-$ROOT/dgc}"
IMAGE="${IMAGE:-localhost/mesa-build-fedora44-wsi-glslang}"
OUT="${OUT:-/tmp/opencode/mesa27-dgc-build}"   # tmpfs: build dir
CCACHEDIR="${CCACHEDIR:-/tmp/opencode/ccache}" # tmpfs: ccache
HOSTOUT="${HOSTOUT:-$ROOT/build/out-dgc27}"    # artifacts on the SD card

if pgrep -f 're4\.exe|wine|wineserver|Starfield' >/dev/null 2>&1; then
   echo "Wine/the game is running - close it before building." >&2
   exit 1
fi

[ -f "$SRC/meson.options" ] || { echo "no Mesa tree at $SRC" >&2; exit 1; }

mkdir -p "$OUT" "$CCACHEDIR" "$HOSTOUT"

exec podman run --rm \
   -v "$SRC":/src:Z \
   -v "$REPO":/repo:Z \
   -v "$OUT":/out:Z \
   -v "$CCACHEDIR":/ccache:Z \
   -v "$HOSTOUT":/hostout:Z \
   -e FRESH="${FRESH:-1}" \
   -e BUILD_JOBS="${BUILD_JOBS:-2}" \
   "$IMAGE" bash /repo/scripts/build-dgc27.sh
