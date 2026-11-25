# turnip: `VK_EXT_device_generated_commands`

Отдельная линия разработки драйвера, не патч. `VK_EXT_device_generated_commands`
(DGC) в turnip для Starfield через vkd3d-proton: приложение записывает
последовательности команд в буфер на GPU, драйвер транслирует их в PM4 тем же
GPU, и CP исполняет результат через `CP_INDIRECT_BUFFER`.

Сюда же переехало всё, что относилось к DGC и висело в репозитории патчей
(`../turnip-patches`): план, анализ, пробы, скрипты, логи игры.

## Где код

`../mesa27-dgc` — git worktree дерева Mesa (`../mesa27-src`), ветка **`dgc-27`**:
4 коммита поверх апстрима `origin/main` = `fc930002fc5` (27.0.0-devel, 23.11.2026).

```
tu/dgc: emit GPU-written PM4 through CP_INDIRECT_BUFFER
tu/dgc: add indirect commands layout and execution set objects
tu/dgc: translate command sequences into a PM4 stream on the GPU
tu/dgc: advertise VK_EXT_device_generated_commands behind a driconf option
```

Порядок не случаен: экстеншен заявляется последним и под опцией, поэтому ни
одна промежуточная точка его не заявляет. Экстеншен, который врёт про
поддержку токенов, для vkd3d-proton означает не fallback, а тихую потерю
геометрии или `device removed`.

Прежняя линия (26.3.0-devel) — в форке `keks2293/mesa`, ветка `dgc`; её
коммиты в виде патчей лежат в `mesa-patches/` как история, к `dgc-27` они не
применяются.

Поддерживается перевод токенов `PUSH_CONSTANT`, `SEQUENCE_INDEX`, `DRAW`,
`DRAW_INDEXED`. `DISPATCH` (Starfield использует его в `[CONSTANT, DISPATCH]`)
пока отвергается на `vkCreateIndirectCommandsLayoutEXT`: его нельзя честно
объявить неподдержанным (нет поштучного запроса, а `COMPUTE` из стадий не
убрать — vkd3d тогда выключит DGC целиком), поэтому его нужно реализовать.
Цена и открытый замер — `docs/dgc-plan.md` §10.

## Сборка

```
scripts/build-dgc27-host.sh          # свежая настройка + сборка
FRESH=0 scripts/build-dgc27-host.sh  # инкрементальная
```

Она же — проверка, что игра/Wine не запущены, и что `$SRC` действительно дерево
Mesa. Внутри запускается `scripts/build-dgc27.sh` (он работает в контейнере):

```
podman run --rm \
  -v /var/home/armada/opencode/mesa27-dgc:/src:Z \
  -v /var/home/armada/opencode/dgc:/repo:Z \
  -v /tmp/opencode/mesa27-dgc-build:/out:Z \
  -v /tmp/opencode/ccache:/ccache:Z \
  -v /var/home/armada/opencode/build/out-dgc27:/hostout:Z \
  localhost/mesa-build-fedora44-wsi-glslang bash /repo/scripts/build-dgc27.sh
```

Правила те же, что в `../turnip-patches`: сборка только в podman, build dir и
ccache в tmpfs (`/var` — SD-карта), `-j2`, никогда при запущенной игре.
Артефакт:
`../build/out-dgc27/{libvulkan_freedreno.so,freedreno_icd.json,SHA256SUMS}`.

## Как включить

По умолчанию экстеншена не видно вообще: гасится только бит в
`supported_extensions`, а общий слой mesa по нему же обнуляет features и
properties, и точки входа не разрешаются через `GetDeviceProcAddr`.

`/usr/share/drirc.d/99-dgc.conf` (или `~/.drirc`):

```xml
<?xml version="1.0"?>
<driconf>
    <device driver="turnip">
        <option name="tu_enable_device_generated_commands" value="true" />
    </device>
</driconf>
```

Для отдельного приложения — `<application>` или `<engine>` внутри
`<device driver="turnip">`, как в `00-turnip-defaults.conf` (там vkd3d-ные
квирки пишут через `engine_name_match="vkd3d"`; для одной игры точнее так):

```xml
<?xml version="1.0"?>
<driconf>
    <device driver="turnip">
        <application name="Starfield" executable="Starfield.exe">
            <option name="tu_enable_device_generated_commands" value="true" />
        </application>
    </device>
</driconf>
```

Для разовой проверки — `DRIRC_CONFIGDIR=/tmp/opencode/drirc-dgc-on`
(перезадаёт весь каталог конфигов: системные дефолты turnip в этот момент не
применяются, так что для прогонов игры способ не годится).

Проверка A/B одним и тем же `.so`:

```
VK_DRIVER_FILES=/var/home/armada/opencode/build/out-dgc27/freedreno_icd.json vulkaninfo \
  | grep -E 'device_generated_commands|deviceGeneratedCommands|supportedIndirectCommandsShaderStages'
```

## Проба

`tests/dgc-probe` — Vulkan-пробы без vkd3d (стенд машины: 512x256 RGBA8,
magenta clear, чтение пикселей). `make` на устройстве нет, поэтому `build.sh`:

```
cd tests/dgc-probe
./build.sh                      # все пробы
./build.sh --run probe12        # собрать и запустить на out-dgc27
```

`probe12` — контроль «машина конформна» (ветер по спеку + `CULL_BACK`), 3/3
зелёный на `dgc-27`. Квик: верхние ~32–48 строк кадра машина не растеризует
(`docs/dgc-plan.md` §9.7) — проверки ставить на `y >= 64`.

## Игра

```
ICD=/var/home/armada/opencode/build/out-dgc27/freedreno_icd.json \
  scripts/run-starfield.sh patched 600
```

Логи — в `results/starfield/`. Игровой стек (Starfield-props, UBWC, sparse) на
Mesa 27 ещё не перенесён: `dgc-27` — это только DGC поверх апстрима.

## Структура

| Путь | Что |
|---|---|
| `docs/dgc-plan.md` | план и статусы, §9 — разбор «draw не виден» и §9.9 — краш на загрузке уровня |
| `docs/dgc-analysis.md` | дамп сигнатур ExecuteIndirect из Starfield, гейт vkd3d, обзор реализаций (RADV/ANV/nvk/lavapipe) |
| `tests/dgc-probe/` | пробы + `pm4dec3.py`/`where_state.py` (декодер PM4) |
| `scripts/` | сборка DGC-линии, запуск игры |
| `mesa-patches/` | история прежней линии (26.3) в виде патчей |
| `results/starfield/` | прогоны игры |

## Открытое

1. `DISPATCH` не переводится → сигнатура `[CONSTANT, DISPATCH]` отклоняется на
   create. Старое поведение (отказ на Execute) давал `device removed` — это и
   есть §9.9 (меню 600 с безупречно, краш на загрузке уровня). A/B из §9.9 с
   `DGC_DUMP=1` и `VKD3D_DISABLE_EXTENSIONS` не сделан: нужен игровой стек на
   Mesa 27.
2. `GetPhysicalDeviceProperties` обещает `COMPUTE` в
   `supportedIndirectCommandsShaderStages`, хотя compute-токен не переводится:
   без compute-бита vkd3d выключает DGC целиком (нужны `ALL_GRAPHICS|COMPUTE`),
   то есть и draw-путь тоже.
3. Про end-to-end: на `dgc-27` подтверждён обычный draw (probe12) и видимость
   экстеншена; сам путь `vkCmdExecuteGeneratedCommandsEXT` на этом билде ещё не
   прогонялся.
