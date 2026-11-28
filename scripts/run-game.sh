#!/usr/bin/env bash
# Generic Proton+vkd3d-proton game runner for Turnip/DGC testing.
#
# Usage:
#   ./dgc/scripts/run-game.sh --exe /path/to/game.exe [options]
#
# Options:
#   --exe PATH          Game executable (required unless GAME_EXE env is set)
#   --dll PATH          vkd3d-proton DLL file or directory to test (optional).
#                       If omitted, Proton/system vkd3d-proton is used.
#   --mode MODE         patched|stock (default: patched)
#   --timeout SEC       Timeout in seconds, 0=no timeout (default: 0)
#   --icd PATH          Vulkan ICD JSON for patched mode (default: build/out/freedreno_icd.json or ICD env)
#   --prefix PATH       Steam compat/prefix dir (default: auto under Heroic/Prefixes)
#   --appid ID          Steam AppID for protonfixes (default: from steam_emu.ini or 0)
#   --proton PATH       Proton executable (default: proton-cachyos)
#   --hud MODE          FPS/GPU overlay: mangohud|dxvk|vkd3d|none (default: none)
#   --gameargs "..."    Extra args passed to game executable
#   -h, --help          Show help
#
# Local paths (this machine):
#   Games: /run/media/armada/Slim/Games/
#     Starfield: /run/media/armada/Slim/Games/Starfield.Digital.Premium.Edition-InsaneRamZes/Starfield.exe
#     Teardown:  /run/media/armada/Slim/Games/Teardown (2022)/Teardown/teardown.exe
#   vkd3d-proton build DLLs: /var/home/armada/opencode/build/vkd3d-proton/
#   If game disk is not mounted, script tries to mount lsblk label "Slim"/"Z Slim" via udisksctl.
#
# DLL behavior:
#   --dll /path/to/d3d12.dll|dir    use that vkd3d-proton build via WINEDLLPATH/WINEDLLOVERRIDES
#   no --dll                        use Proton/system vkd3d-proton
#   The script never copies DLLs into the game directory.
set -euo pipefail

usage() { sed -n '2,35p' "$0"; }

MODE="${MODE:-patched}"
TIMEOUT="${TIMEOUT:-0}"
EXE="${GAME_EXE:-}"
DLL="${VKD3D_DLL_OVERRIDE:-}"
ICD="${ICD:-}"
PREFIX="${PREFIX:-${STEAM_COMPAT_DATA_PATH:-}}"
APP_ID="${APP_ID:-}"
PROTON="${PROTON:-/usr/share/steam/compatibilitytools.d/proton-cachyos-11.0-arm64/proton}"
GAMEARGS="${GAMEARGS:-}"
HUD="${HUD:-none}"

while [ $# -gt 0 ]; do
  case "$1" in
    --exe) EXE="$2"; shift 2 ;;
    --dll) DLL="$2"; shift 2 ;;
    --mode) MODE="$2"; shift 2 ;;
    --timeout) TIMEOUT="$2"; shift 2 ;;
    --icd) ICD="$2"; shift 2 ;;
    --prefix) PREFIX="$2"; shift 2 ;;
    --appid) APP_ID="$2"; shift 2 ;;
    --proton) PROTON="$2"; shift 2 ;;
    --hud) HUD="$2"; shift 2 ;;
    --gameargs) GAMEARGS="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    --) shift; GAMEARGS="${GAMEARGS:+$GAMEARGS }$*"; break ;;
    -*)
      echo "unknown option: $1" >&2; usage >&2; exit 1 ;;
    *)
      if [ -z "$EXE" ]; then EXE="$1"; shift
      else echo "unexpected arg: $1" >&2; usage >&2; exit 1
      fi
      ;;
  esac
done

maybe_mount_game_disk() {
  [ -e "$EXE" ] && return 0
  command -v udisksctl >/dev/null 2>&1 || return 0
  command -v python3 >/dev/null 2>&1 || return 0
  python3 - <<'PY' 2>/dev/null | while IFS= read -r dev; do
import json, subprocess
try:
    data = json.loads(subprocess.check_output(['lsblk', '-Jno', 'PATH,LABEL,MOUNTPOINT'], text=True))
except Exception:
    raise SystemExit(0)
def walk(nodes):
    for n in nodes:
        if n.get('label') in ('Slim', 'Z Slim') and not n.get('mountpoint'):
            p = n.get('path')
            if p:
                print(p)
        walk(n.get('children') or [])
walk(data.get('blockdevices') or [])
PY
    [ -n "$dev" ] || continue
    udisksctl mount -b "$dev" --no-user-interaction >/dev/null 2>&1 || echo "warning: failed to mount $dev" >&2
  done
  sleep 1
}

[ -n "$EXE" ] || { echo "missing --exe /path/to/game.exe" >&2; usage >&2; exit 1; }
maybe_mount_game_disk
[ -f "$EXE" ] || { echo "game exe not found: $EXE" >&2; exit 1; }
EXE="$(realpath "$EXE")"
GAME_DIR="$(dirname "$EXE")"
EXE_NAME="$(basename "$EXE")"
TAG="$(basename "$EXE" .exe | tr '[:upper:]' '[:lower:]' | tr ' .()' '---' | sed 's/--*/-/g; s/^-//; s/-$//')"

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PATCHED_ICD="${ICD:-$REPO/../build/out/freedreno_icd.json}"

detect_appid() {
  local ini="$GAME_DIR/steam_emu.ini"
  [ -r "$ini" ] || return 1
  sed -n 's/^[[:space:]]*AppId[[:space:]]*=[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$ini" | head -1
}

if [ -z "$APP_ID" ]; then
  APP_ID="$(detect_appid || true)"
fi
[ -n "$APP_ID" ] || APP_ID="0"

if [ -z "$PREFIX" ]; then
  PREFIX="/var/home/armada/Games/Heroic/Prefixes/${TAG}${APP_ID}"
fi

case "$MODE" in
  patched)
    [ -r "$PATCHED_ICD" ] || { echo "missing ICD: $PATCHED_ICD" >&2; exit 1; }
    export VK_DRIVER_FILES="$PATCHED_ICD"
    export VK_ICD_FILENAMES="$PATCHED_ICD"
    ;;
  stock)
    unset VK_DRIVER_FILES VK_ICD_FILENAMES || true
    ;;
  *) echo "bad --mode: $MODE (use patched|stock)" >&2; exit 1 ;;
esac

[ -x "$PROTON" ] || { echo "proton not found/executable: $PROTON" >&2; exit 1; }
mkdir -p "$PREFIX"

# Ensure driver has X11/Wayland WSI; avoids long timeout on broken builds.
if ! grep -q 'VK_KHR_xcb_surface' <<<"$(vulkaninfo 2>/dev/null)"; then
  echo "missing VK_KHR_xcb_surface: driver built without -Dplatforms=x11,wayland" >&2
  exit 1
fi

# Optional vkd3d-proton DLL override. No game-dir copy.
VKD3D_OVERRIDE=""
VKD3D_OVERRIDE_DLLS=""
VKD3D_OVERRIDE_BACKUP=()
VKD3D_OVERRIDE_TARGETS=()
if [ -n "$DLL" ]; then
  [ -e "$DLL" ] || { echo "--dll path not found: $DLL" >&2; exit 1; }
  DLL="$(realpath "$DLL")"
  if [ -d "$DLL" ]; then
    VKD3D_OVERRIDE="$DLL"
  else
    VKD3D_OVERRIDE="$(dirname "$DLL")"
  fi
  for n in d3d12 d3d12core; do
    if [ -r "$VKD3D_OVERRIDE/$n.dll" ]; then
      VKD3D_OVERRIDE_DLLS="${VKD3D_OVERRIDE_DLLS:+$VKD3D_OVERRIDE_DLLS;}$n=n"
    fi
  done
  [ -n "$VKD3D_OVERRIDE_DLLS" ] || { echo "no d3d12.dll/d3d12core.dll found in $VKD3D_OVERRIDE" >&2; exit 1; }

  # Proton overwrites prefix d3d12/d3d12core from its own files on startup.
  # Use a private Proton copy with overridden vkd3d-proton DLLs. No game-dir changes.
  PROTON_BASE="$PROTON"
  PROTON_BASE_DIR="$(dirname "$PROTON_BASE")"
  PROTON_OVERRIDE_ROOT="$REPO/../build/proton-overrides"
  PROTON_TAG="$(basename "$PROTON_BASE_DIR")-dgc-$(sha256sum "$VKD3D_OVERRIDE/d3d12core.dll" "$VKD3D_OVERRIDE/d3d12.dll" 2>/dev/null | sha256sum | cut -c1-12)"
  PROTON="$PROTON_OVERRIDE_ROOT/$PROTON_TAG/proton"
  if [ ! -x "$PROTON" ]; then
    mkdir -p "$PROTON_OVERRIDE_ROOT"
    cp -a "$PROTON_BASE_DIR" "$PROTON_OVERRIDE_ROOT/$PROTON_TAG"
    chmod -R u+w "$PROTON_OVERRIDE_ROOT/$PROTON_TAG"
    # Override only matching-arch vkd3d-proton dirs. Do not put x64 DLLs into
    # aarch64/i386 slots; arm64 Proton copies host/wow64 arch into prefix.
    for arch in x86_64-windows aarch64-windows i386-windows; do
      for sub in "files/lib/wine/vkd3d-proton/$arch" "files/lib/wine/$arch"; do
        dstroot="$PROTON_OVERRIDE_ROOT/$PROTON_TAG/$sub"
        [ -d "$dstroot" ] || continue
        for n in d3d12 d3d12core; do
          src="$VKD3D_OVERRIDE/$n.dll"
          dst="$dstroot/$n.dll"
          [ -r "$src" ] || continue
          [ -e "$dst" ] || continue
          srcarch="$(file -b "$src" 2>/dev/null || true)"
          dstarch="$(file -b "$dst" 2>/dev/null || true)"
          srcarm64x=0; python3 -c 'import struct,sys; b=open(sys.argv[1],"rb").read(); pe=struct.unpack_from("<I",b,0x3c)[0]; nsec=struct.unpack_from("<H",b,pe+6)[0]; sh=pe+24+struct.unpack_from("<H",b,pe+20)[0]; secs=[b[sh+i*40:sh+i*40+8].rstrip(b"\0") for i in range(nsec)]; sys.exit(0 if (b".a64xrm" in secs or b".hybmp$x" in secs) else 1)' "$src" 2>/dev/null && srcarm64x=1 || true
          case "$arch" in
            x86_64-windows)  [[ "$srcarch" == *x86-64* ]] && [ "$srcarm64x" -eq 0 ] && cp -f "$src" "$dst" ;;
            aarch64-windows) [[ "$srcarch" == *ARM64* || "$srcarch" == *Aarch64* || "$srcarm64x" -eq 1 ]] && cp -f "$src" "$dst" ;;
            i386-windows)    [[ "$srcarch" == *"Intel i386"* || "$srcarch" == *80386* ]] && cp -f "$src" "$dst" ;;
          esac
        done
      done
    done
  fi

  # Existing prefix may already contain old DLLs; replace matching-arch files and restore on cleanup.
  PREFIX_PFX="$PREFIX/pfx"
  for n in d3d12 d3d12core; do
    src="$VKD3D_OVERRIDE/$n.dll"
    [ -r "$src" ] || continue
    srcarch="$(file -b "$src" 2>/dev/null || true)"
    while IFS= read -r -d '' dst; do
      dstarch="$(file -b "$dst" 2>/dev/null || true)"
      srcarm64x=0; python3 -c 'import struct,sys; b=open(sys.argv[1],"rb").read(); pe=struct.unpack_from("<I",b,0x3c)[0]; nsec=struct.unpack_from("<H",b,pe+6)[0]; sh=pe+24+struct.unpack_from("<H",b,pe+20)[0]; secs=[b[sh+i*40:sh+i*40+8].rstrip(b"\0") for i in range(nsec)]; sys.exit(0 if (b".a64xrm" in secs or b".hybmp$x" in secs) else 1)' "$src" 2>/dev/null && srcarm64x=1 || true
      match=0
      case "$dstarch" in
        *x86-64*) [[ "$srcarch" == *x86-64* ]] && [ "$srcarm64x" -eq 0 ] && match=1 ;;
        *ARM64*|*Aarch64*) [[ "$srcarch" == *ARM64* || "$srcarch" == *Aarch64* || "$srcarm64x" -eq 1 ]] && match=1 ;;
        *Intel\ i386*|*80386*) [[ "$srcarch" == *"Intel i386"* || "$srcarch" == *80386* ]] && match=1 ;;
      esac
      [ "$match" -eq 1 ] || continue
      bak="$dst.dgc-bak.$$"
      cp -a "$dst" "$bak"
      VKD3D_OVERRIDE_BACKUP+=("$bak::$dst")
      cp -f "$src" "$dst"
      VKD3D_OVERRIDE_TARGETS+=("$dst")
    done < <(find "$PREFIX_PFX/drive_c/windows" -type f -iname "$n.dll" -not -name '*.dgc-bak.*' -print0 2>/dev/null)
  done
  export WINEDLLOVERRIDES="$VKD3D_OVERRIDE_DLLS${WINEDLLOVERRIDES:+;$WINEDLLOVERRIDES}"
fi

DRIVER_LIB="$(dirname "$PATCHED_ICD")/libvulkan_freedreno.so"
DRIVER_SHA=""
if [ "$MODE" = patched ] && [ -r "$DRIVER_LIB" ]; then
  DRIVER_SHA="$(sha256sum "$DRIVER_LIB" | cut -c1-16)"
  mkdir -p "$REPO/../build/vkd3d-cache/$DRIVER_SHA"
  export VKD3D_SHADER_CACHE_PATH="${VKD3D_SHADER_CACHE_PATH:-$(realpath "$REPO/../build/vkd3d-cache/$DRIVER_SHA")}"
fi

CURATE="$REPO/scripts/curate-game-log.sh"
[ -r "$CURATE" ] || CURATE="$REPO/../turnip-patches/scripts/curate-game-log.sh"
mkdir -p "$REPO/results/raw" "$REPO/results/$TAG"
TS="$(date +%Y%m%d-%H%M%S)"
RAW="$REPO/results/raw/$TAG-$MODE-$TS.log"
LOG="$REPO/results/$TAG/$TAG-$MODE-$TS.log"

export STEAM_COMPAT_DATA_PATH="$PREFIX"
export STEAM_COMPAT_CLIENT_INSTALL_PATH="${STEAM_COMPAT_CLIENT_INSTALL_PATH:-/var/home/armada/.local/share/Steam}"
export STEAM_COMPAT_APP_ID="$APP_ID"
export PROTON_LOG="${PROTON_LOG:-1}"
export WINEDEBUG="${WINEDEBUG:--all}"
export VK_LOADER_DEBUG="${VK_LOADER_DEBUG:-error,warn}"
export WINEPREFIX="$PREFIX/pfx"

for name in VKD3D_DISABLE_EXTENSIONS VKD3D_FEATURE_LEVEL TU_DEBUG MESA_DEBUG DXVK_HUD VKD3D_HUD MANGOHUD_CONFIG; do
  if [ -n "${!name:-}" ]; then export "$name"; fi
done

LAUNCH_PREFIX=()
case "$HUD" in
  none|"") ;;
  mangohud)
    command -v mangohud >/dev/null 2>&1 || { echo "mangohud not found" >&2; exit 1; }
    export MANGOHUD="${MANGOHUD:-1}"
    export MANGOHUD_CONFIG="${MANGOHUD_CONFIG:-fps,frametime,gpu_stats,cpu_stats,ram,vram,frame_timing=1}"
    LAUNCH_PREFIX=(mangohud --dlsym)
    ;;
  dxvk)
    export DXVK_HUD="${DXVK_HUD:-fps,frametimes,gpuload,devinfo,version}"
    ;;
  vkd3d)
    export VKD3D_HUD="${VKD3D_HUD:-fps,frametimes}"
    ;;
  *) echo "bad --hud: $HUD (use mangohud|dxvk|vkd3d|none)" >&2; exit 1 ;;
esac

if [ -n "${DRIRC_CONFIGDIR:-}" ]; then
  echo "DRIRC_CONFIGDIR is set and would override /usr/share/drirc.d; unset it" >&2
  exit 1
fi

{
  echo "# date: $(date -Is)"
  echo "# mode: $MODE"
  echo "# exe: $EXE"
  echo "# prefix: $PREFIX"
  echo "# appid: $APP_ID"
  echo "# VK_DRIVER_FILES=${VK_DRIVER_FILES:-<unset>}"
  echo "# VKD3D_FEATURE_LEVEL=${VKD3D_FEATURE_LEVEL:-<unset>}"
  echo "# DGC: advertised unconditionally (no drirc gate), ожидается: Enabling support for SM 6.6"
  [ -n "$DRIVER_SHA" ] && echo "# driver: $DRIVER_SHA  vkd3d cache: ${VKD3D_SHADER_CACHE_PATH:-<default>}"
  echo "# hud: $HUD"
  if [ -n "$VKD3D_OVERRIDE" ]; then
    echo "# vkd3d-proton override: $VKD3D_OVERRIDE ($VKD3D_OVERRIDE_DLLS)"
  else
    echo "# vkd3d-proton: Proton/system default (no override)"
  fi
  echo "# devices (name | sparseResidencyImage2D):"
  vulkaninfo 2>/dev/null | awk '
    /deviceName/ { dev=$3" "$4" "$5" "$6" "$7 }
    /sparseResidencyImage2D/ { print "#   " dev " | " $3; dev="" }
  '
  echo "# ---"
} > "$RAW"

cleanup_prefix() {
  # Kill game/browser helpers that can keep Proton alive after timeout/crash.
  pkill -x "$EXE_NAME" 2>/dev/null || true
  pkill -f '[w]inebrowser' 2>/dev/null || true
  pkill -f '[s]tart.exe /unix.*winebrowser' 2>/dev/null || true
  pkill -f '[e]xplorer.exe' 2>/dev/null || true
  pkill -f '[s]teamwebhelper.exe' 2>/dev/null || true
  pkill -f '[c]rashpad_handler' 2>/dev/null || true
  if [ -n "${WINEPREFIX:-}" ] && command -v wineserver >/dev/null 2>&1; then
    WINEPREFIX="$WINEPREFIX" wineserver -k 2>/dev/null || true
    WINEPREFIX="$WINEPREFIX" wineserver -w 2>/dev/null || true
  fi
  pkill -f "wineserver.*${PREFIX}" 2>/dev/null || true
  pgrep -x "$EXE_NAME" >/dev/null 2>&1 && pkill -9 -x "$EXE_NAME" 2>/dev/null || true
  if [ "${#VKD3D_OVERRIDE_BACKUP[@]}" -gt 0 ]; then
    for entry in "${VKD3D_OVERRIDE_BACKUP[@]}"; do
      bak="${entry%%::*}"
      dst="${entry#*::}"
      [ -r "$bak" ] && mv -f "$bak" "$dst" || true
    done
  fi
  if [ "${#VKD3D_OVERRIDE_TARGETS[@]}" -gt 0 ]; then
    for dst in "${VKD3D_OVERRIDE_TARGETS[@]}"; do
      case "$dst" in *.dgc-bak.*) continue ;; esac
      found=0
      if [ "${#VKD3D_OVERRIDE_BACKUP[@]}" -gt 0 ]; then
        for entry in "${VKD3D_OVERRIDE_BACKUP[@]}"; do
          [ "${entry#*::}" = "$dst" ] && found=1 && break
        done
      fi
      [ "$found" -eq 0 ] && rm -f "$dst" || true
    done
  fi
  return 0
}

cleanup_prefix
sleep 1
# Re-apply prefix DLL override after initial cleanup (cleanup restores backups).
if [ -n "${VKD3D_OVERRIDE:-}" ]; then
  VKD3D_OVERRIDE_BACKUP=()
  VKD3D_OVERRIDE_TARGETS=()
  PREFIX_PFX="$PREFIX/pfx"
  for n in d3d12 d3d12core; do
    src="$VKD3D_OVERRIDE/$n.dll"
    [ -r "$src" ] || continue
    for sysdir in "$PREFIX_PFX/drive_c/windows/system32" "$PREFIX_PFX/drive_c/windows/syswow64"; do
      [ -d "$sysdir" ] || continue
      dst="$sysdir/$n.dll"
      if [ -e "$dst" ]; then
        bak="$dst.dgc-bak.$$"
        cp -a "$dst" "$bak"
        VKD3D_OVERRIDE_BACKUP+=("$bak::$dst")
      fi
      cp -f "$src" "$dst"
      VKD3D_OVERRIDE_TARGETS+=("$dst")
    done
  done
fi
trap cleanup_prefix EXIT INT TERM

cd "$GAME_DIR"
set +e
read -r -a EXTRA <<< "$GAMEARGS"
if [ "$TIMEOUT" -gt 0 ] 2>/dev/null; then
  timeout --foreground --signal=INT "$TIMEOUT" \
    ${LAUNCH_PREFIX[@]+"${LAUNCH_PREFIX[@]}"} "$PROTON" waitforexitandrun "$EXE" ${EXTRA[@]+"${EXTRA[@]}"} >>"$RAW" 2>&1
  RC=$?
else
  ${LAUNCH_PREFIX[@]+"${LAUNCH_PREFIX[@]}"} "$PROTON" waitforexitandrun "$EXE" ${EXTRA[@]+"${EXTRA[@]}"} >>"$RAW" 2>&1
  RC=$?
fi
set -e

cleanup_prefix || true

echo "exit: $RC"
if [ -r "$CURATE" ]; then
  "$CURATE" "$RAW" "$LOG" || echo "curation failed - see raw log" >&2
else
  echo "curator missing: $CURATE - see raw log" >&2
fi
echo "log: $LOG  (raw: $RAW)"
for f in /tmp/proton-"${USER:-armada}"/*.log; do
  [ -e "$f" ] && cp "$f" "$REPO/results/raw/proton-$TAG-$MODE-$TS-$(basename "$f")" || true
done
exit "$RC"
