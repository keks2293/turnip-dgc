#!/usr/bin/env bash
# Compatibility wrapper: Starfield defaults over the generic run-game.sh.
# Prefer run-game.sh for new runs.
#
#   scripts/run-starfield.sh [stock|patched] [timeout-sec]
#
# Env overrides: GAME_DIR, GAME_EXE, PREFIX, APP_ID, ICD, VKD3D_DLL_OVERRIDE, GAMEARGS, PROTON.
set -euo pipefail

case "${1:-}" in -h|--help) exec "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/scripts/run-game.sh" --help ;; esac

MODE="${1:-patched}"
TIMEOUT="${2:-0}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

EXE="${GAME_EXE:-${GAME_DIR:-/run/media/armada/Slim/Games/Starfield.Digital.Premium.Edition-InsaneRamZes}/Starfield.exe}"
if [ -n "${GAME_DIR:-}" ] && [ -z "${GAME_EXE:-}" ]; then
  EXE="$GAME_DIR/Starfield.exe"
fi

ARGS=(
  --mode "$MODE"
  --timeout "$TIMEOUT"
  --exe "$EXE"
  --prefix "${PREFIX:-/var/home/armada/Games/Heroic/Prefixes/starfield1716740}"
  --appid "${APP_ID:-1716740}"
)
[ -n "${ICD:-}" ] && ARGS+=(--icd "$ICD")
[ -n "${VKD3D_DLL_OVERRIDE:-}" ] && ARGS+=(--dll "$VKD3D_DLL_OVERRIDE")
[ -n "${PROTON:-}" ] && ARGS+=(--proton "$PROTON")
[ -n "${GAMEARGS:-}" ] && ARGS+=(--gameargs "$GAMEARGS")

exec "$REPO/scripts/run-game.sh" "${ARGS[@]}"
