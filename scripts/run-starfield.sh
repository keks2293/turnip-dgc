#!/usr/bin/env bash
# Запуск Starfield (DX12, RUNE-репак) на патченом или стоковом turnip.
#
#   scripts/run-starfield.sh [stock|patched] [timeout-sec]   # по умолчанию patched, 0 = без таймаута
#
# Запуск идёт напрямую через proton-cachyos: Wine в системе НЕ установлен
# (rpm -q wine → нет), а игра не зарегистрирована в Steam, то есть Heroic её
# не запустит. Лаунчерные конфиги к тому же не подсовывают VK_DRIVER_FILES —
# патченый драйвер не подхватился бы (../turnip-patches/docs/analysis.md §11).
#
# Драйвер меняется именно этими переменными, и только ими. Путь до turnip такой:
# d3d12.dll в папке игры — x86_64 (сборка vkd3d-proton под x86), значит vkd3d
# работает внутри FEX и зовёт Vulkan через FEX thunk «Vulkan» (ThunksDB в
# /usr/share/fex-emu/Config.json: Vulkan=1); thunk грузит host-ный loader,
# который и читает VK_DRIVER_FILES из нашего окружения. Всё остальное, что
# делает armada-game-launch — профиль FEX через FEX_APP_CONFIG, аффинитет,
# починка контроллера, per-driver VKD3D_SHADER_CACHE_PATH — к выбору драйвера
# отношения не имеет, а его отсутствие мы видим глазами в логе, поэтому вместо
# обёртки дописано сюда ровно то, что нужно прогону, — кэш шейдеров.
#
# Префикс создаётся заново (ранее на устройстве префикса Starfield не было):
# имя обязано содержать цифры — protonfixes вытаскивает номер игры из
# STEAM_COMPAT_DATA_PATH (re.findall(r'\d+')).
#
# CWD обязан быть папкой игры: репак ищет данные относительно текущего
# каталога. Проверка «пропажи моделей» — визуальная, окно появится на экране:
# лог фиксирует только здоровье стека (устройство, swapchain, ошибки Vulkan).
set -euo pipefail

MODE="${1:-patched}"
TIMEOUT="${2:-0}"

# Игра берётся из окружения, Starfield — значение по умолчанию. Носитель
# монтируется по метке, а она менялась ("Z Slim" → "Slim"), тот же повод
# переопределять путь. Другая DX12-игра — те же четыре переменные:
#   GAME_DIR=<папка игры> GAME_EXE=$GAME_DIR/<Game>.exe \
#   PREFIX=.../Prefixes/<game>1234 APP_ID=1234 scripts/run-starfield.sh patched 1200
# Префикс proton создаёт сам, но каталог должен существовать (он кладёт в него
# pfx.lock), а имя обязано содержать цифры: protonfixes вытаскивает номер игры
# из STEAM_COMPAT_DATA_PATH (re.findall(r'\d+')), APP_ID — он же.
GAME_DIR="${GAME_DIR:-/run/media/armada/Slim/Games/Starfield.Digital.Premium.Edition-InsaneRamZes}"
GAME_EXE="${GAME_EXE:-$GAME_DIR/Starfield.exe}"
PREFIX="${PREFIX:-/var/home/armada/Games/Heroic/Prefixes/starfield1716740}"
APP_ID="${APP_ID:-1716740}"
PROTON="/usr/share/steam/compatibilitytools.d/proton-cachyos-11.0-arm64/proton"

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# ICD=${ICD:-игровой стек}; для DGC-линии:
#   ICD=/var/home/armada/opencode/build/out-dgc27/freedreno_icd.json scripts/run-starfield.sh patched 600
PATCHED_ICD="${ICD:-$REPO/../build/out/freedreno_icd.json}"

case "$MODE" in
  patched)
    [ -r "$PATCHED_ICD" ] || { echo "нет $PATCHED_ICD — собрать: ../turnip-patches/scripts/build-turnip.sh (игровой стек) или scripts/build-dgc27.sh (DGC-линия)" >&2; exit 1; }
    # Оба имени, как в armada-game-launch (VK_DRIVER_ENV там — пара): VK_ICD_FILENAMES
    # читает legacy-путь лоадера, VK_DRIVER_FILES — современный. Манифест заменяет
    # поиск целиком, поэтому свой ICD здесь единственный: /usr/share/vulkan/icd.d
    # с пакетным turnip loader больше не видит.
    export VK_DRIVER_FILES="$PATCHED_ICD"
    export VK_ICD_FILENAMES="$PATCHED_ICD"
    ;;
  stock)
    unset VK_DRIVER_FILES VK_ICD_FILENAMES || true
    ;;
  *) echo "usage: $0 [stock|patched] [timeout-sec]" >&2; exit 1 ;;
esac

[ -x "$PROTON" ] || { echo "нет proton: $PROTON" >&2; exit 1; }
[ -f "$GAME_EXE" ] || { echo "нет игры: $GAME_EXE" >&2; exit 1; }
mkdir -p "$PREFIX"   # proton кладёт pfx.lock только если каталог уже есть

# Ловушка build-turnip.sh №3: сборка без -Dplatforms=x11,wayland даёт драйвер
# без VK_KHR_xcb_surface — wine не переводит win32-поверхность, и DXVK падает
# на vkCreateInstance за секунду до картинки (лог: "Failed to create Vulkan
# instance"). Ловим сразу, а не таймаутом в 600 с.
# Вывод копим целиком: под set -o pipefail ранний выход grep -q рвёт трубу,
# vulkaninfo ловит SIGPIPE, и пайплайн падает ложно.
if ! grep -q 'VK_KHR_xcb_surface' <<<"$(vulkaninfo 2>/dev/null)"; then
    echo "нет VK_KHR_xcb_surface: драйвер собран без -Dplatforms=x11,wayland" >&2
    echo "(см. build-turnip.sh, ловушка №3; пересборка scripts/build-turnip.sh)" >&2
    exit 1
fi

# vkd3d-proton: в proton-cachyos старая версия без фикса «fp16 on Turnip» —
# Turnip честно не умеет shaderDenormPreserveFloat16 (гейт A8XX), vkd3d из-за
# этого отказывает в fp16-шейдере Starfield (compute PSO → hr 0x80070057) и
# игра падает после компиляции шейдеров. В master vkd3d-proton починено
# (allow non-compliant denorm), но релиза нет — собираем сами:
#   /var/home/armada/opencode/build/vkd3d-proton/build.sh
# и кладём dll в ПАПКУ ИГРЫ: proton при каждом запуске перезаписывает свои
# копии в system32 префикса (проверено по sha256), а exe-каталог первым идёт
# в поиске нативных dll. Подмена — только здесь (WINEDLLOVERRIDES), RE4 со
# своим префиксом не затрагивается; /usr не трогаем.
VKD3D_DLL="$GAME_DIR/d3d12.dll"
if [ -r "$VKD3D_DLL" ]; then
  export WINEDLLOVERRIDES="d3d12,d3d12core=n"
fi

# Кэш шейдеров vkd3d привязан к драйверу: смена .so помечает его недействительным
# ("driver version mismatch"), и игра пересобирает шейдеры с нуля — это минуты
# прогона впустую. Кэш по хэшу драйвера (тот же приём в armada-game-launch, там
# хэш имени водителя) даёт каждому драйверу свой тёплый кэш вместо общего,
# который каждый прогон выбрасывает из-за соседа.
DRIVER_LIB="$(dirname "$PATCHED_ICD")/libvulkan_freedreno.so"
DRIVER_SHA=""
if [ "$MODE" = patched ] && [ -r "$DRIVER_LIB" ]; then
  DRIVER_SHA="$(sha256sum "$DRIVER_LIB" | cut -c1-16)"
  mkdir -p "$REPO/../build/vkd3d-cache/$DRIVER_SHA"
  # realpath обязателен: путь с ".." посередине vkd3d превращает в
  # Z:\...\dgc\..\build\... и «Failed to map read-only cache» — кэш не читается.
  export VKD3D_SHADER_CACHE_PATH="${VKD3D_SHADER_CACHE_PATH:-\
$(realpath "$REPO/../build/vkd3d-cache/$DRIVER_SHA")}"
fi

# Тег логов — имя exe, чтобы прогоны разных игр не перезаписывали друг друга.
TAG="$(basename "$GAME_EXE" .exe | tr '[:upper:]' '[:lower:]')"
mkdir -p "$REPO/results/raw" "$REPO/results/$TAG"
TS="$(date +%Y%m%d-%H%M%S)"
RAW="$REPO/results/raw/$TAG-$MODE-$TS.log"
LOG="$REPO/results/$TAG/$TAG-$MODE-$TS.log"
CURATE="$REPO/scripts/curate-game-log.sh"
[ -r "$CURATE" ] || CURATE="$REPO/../turnip-patches/scripts/curate-game-log.sh"

export STEAM_COMPAT_DATA_PATH="$PREFIX"
export STEAM_COMPAT_CLIENT_INSTALL_PATH="/var/home/armada/.local/share/Steam"
export STEAM_COMPAT_APP_ID="$APP_ID"   # нужен protonfixes для фиксов
export PROTON_LOG=1
export WINEDEBUG="${WINEDEBUG:--all}"   # для диагностики: WINEDEBUG=+vulkan ./scripts/...
export VK_LOADER_DEBUG="${VK_LOADER_DEBUG:-error,warn}"

# Диагностические passthrough (dgc-plan §9.9):
#   VKD3D_DISABLE_EXTENSIONS=VK_EXT_device_generated_commands — A/B без DGC
#   (vkd3d device.c:202, список через запятую)
#   VKD3D_FEATURE_LEVEL=12_0 — если vkd3d считает FL из фич и Starfield не
#   доходит до картинки, 12_0 задаётся явно; с поднятым 0004 не нужен
#   TU_DEBUG=dgcdump — дамп сгенерированного PM4-стрима в stderr (turnip).
#   Осторожно: Starfield гоняет DGC каждый кадр, лог разрастается
#   MESA_DEBUG=1 — mesa_log-вывод драйвера (в release-сборке vk_errorf
#   дополнительно требует debug-колбэк — см. dgc-plan §9.9)
for name in VKD3D_DISABLE_EXTENSIONS VKD3D_FEATURE_LEVEL TU_DEBUG MESA_DEBUG; do
    if [ -n "${!name:-}" ]; then export "$name"; fi
done

# driconf сюда не passthrough, и это ловушка, на которой игра уже умирала.
# DRIRC_CONFIGDIR не дополняет поиск drirc, а заменяет его (src/util/
# xmlconfig.c:1541: или он, или $datadir/drirc.d + SYSCONFDIR/drirc + ~/.drirc).
# В /usr/share/drirc.d/00-turnip-defaults.conf сидит tu_enable_softfloat32 —
# только с ним shaderDenormPreserveFloat32=true, а без него vkd3d не даёт
# SM 6.6, и Starfield отвечает «your GPU does not meet the minimum
# requirements» (turnip-patches: README, analysis.md §10). Прогон 25.11.2026 с
# DRIRC_CONFIGDIR на каталог из одного файла терял и softfloat32, и сам гейт
# DGC: файл лежал в /tmp и перезагрузку не пережил, а vkd3d честно писал
# «Not all relevant pipeline stages are supported by EXT_dgc. Skipping».
# DGC включён по executable в ~/.drirc — он читается в дополнение к /usr.
if [ -n "${DRIRC_CONFIGDIR:-}" ]; then
  echo "DRIRC_CONFIGDIR=$DRIRC_CONFIGDIR перекрывает /usr/share/drirc.d:" >&2
  echo "без 00-turnip-defaults.conf vkd3d не получит SM 6.6, и Starfield" >&2
  echo "откажет GPU по железу. Включай DGC в ~/.drirc, переменную убери." >&2
  exit 1
fi

{
  echo "# date: $(date -Is)"
  echo "# mode: $MODE"
  echo "# VK_DRIVER_FILES=${VK_DRIVER_FILES:-<unset>}"
  echo "# VKD3D_FEATURE_LEVEL=${VKD3D_FEATURE_LEVEL:-<unset>}"
  # Две вещи, без которых прогон бессмыслен, и обе читаются из drirc:
  # DGC-гейт (он в ~/.drirc) и SM 6.6 (он в /usr/share/drirc.d, в логе это
  # строка «Enabling support for SM 6.6»).
  echo "# drirc: DGC gate $(grep -q tu_enable_device_generated_commands "$HOME/.drirc" 2>/dev/null && echo on || echo off), ожидание в логе: Enabling support for SM 6.6"
  [ -n "$DRIVER_SHA" ] && echo "# driver: $DRIVER_SHA  vkd3d cache: ${VKD3D_SHADER_CACHE_PATH:-<default>}"
  [ -r "$VKD3D_DLL" ] && \
    echo "# vkd3d-proton: master-сборка $(sha256sum "$VKD3D_DLL" | cut -c1-16) (native-override)" || \
    echo "# vkd3d-proton: из proton-cachyos (без фикса fp16)"
  echo "# devices (name | sparseResidencyImage2D):"
  vulkaninfo 2>/dev/null | awk '
    /deviceName/ { dev=$3" "$4" "$5" "$6" "$7 }
    /sparseResidencyImage2D/ { print "#   " dev " | " $3; dev="" }
  '
  echo "# ---"
} > "$RAW"

# Переживший таймаут wine мешает следующему прогону (см. docs/analysis.md §12):
# дочищаем до старта и после. Только -x по имени процесса: -f зацепил бы
# командную строку шелла, который нас запустил (в ней путь к exe) — тот же
# класс ошибки, что «stop the running-game check matching itself».
EXE_NAME="$(basename "$GAME_EXE")"
pkill -x "$EXE_NAME" 2>/dev/null || true
sleep 1

cd "$GAME_DIR"
set +e
read -r -a EXTRA <<< "${GAMEARGS:-}"
if [ "$TIMEOUT" -gt 0 ] 2>/dev/null; then
  timeout --foreground --signal=INT "$TIMEOUT" \
    "$PROTON" waitforexitandrun "$GAME_EXE" ${EXTRA[@]+"${EXTRA[@]}"} >>"$RAW" 2>&1
else
  "$PROTON" waitforexitandrun "$GAME_EXE" ${EXTRA[@]+"${EXTRA[@]}"} >>"$RAW" 2>&1
fi
RC=$?
set -e

LEFTOVER="$(pgrep -x "$EXE_NAME" 2>/dev/null || true)"
if [ -n "$LEFTOVER" ]; then
  echo "игра пережила таймаут (pid $LEFTOVER) — убиваю, иначе логи смешаются"
  pkill -x "$EXE_NAME" 2>/dev/null || true
  sleep 2
  pkill -f 'wineserver' 2>/dev/null || true
  sleep 1
  pgrep -x "$EXE_NAME" >/dev/null 2>&1 && pkill -9 -x "$EXE_NAME" 2>/dev/null || true
fi

echo "код возврата: $RC"
if [ -r "$CURATE" ]; then
    "$CURATE" "$RAW" "$LOG" || echo "курация не удалась — смотрим полный лог" >&2
else
    echo "нет $CURATE — курация пропущена, смотрим полный лог" >&2
fi
echo "лог: $LOG  (полный: $RAW)"
for f in /tmp/proton-"${USER:-armada}"/*.log; do
  [ -e "$f" ] && cp "$f" "$REPO/results/raw/proton-$TAG-$MODE-$(basename "$f")"
done
exit $RC
