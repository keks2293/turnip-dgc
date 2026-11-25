# Зависание рабочего стола во время прогона (25.11.2026)

Прогон Starfield 23:10–23:18 (`results/starfield/starfield-patched-20261125-231047.log`)
закончился тем, что рабочий стол встал и устройство пришлось перезагружать.
К драйверу и к DGC это не относится; ниже — что именно встало, по журналу, и что
с этим делать. Пишем, потому что признак легко спутать с регрессией драйвера:
зависание выглядит как «поставили новый turnip — и всё умерло».

## Прогон был рабочий

```
# drirc: DGC gate on, ожидание в логе: Enabling support for SM 6.6
19609.399:...:info:vkd3d-proton:vkd3d_bindless_state_get_bindless_flags: Enabling fast paths for advanced ExecuteIndirect() graphics and compute (EXT_dgc).
19609.788:...:info:vkd3d-proton:d3d12_device_caps_init_shader_model: Enabling support for SM 6.6.
```

* DGC включён и SM 6.6 включён — в частности это подтверждает, что нулевые
  `maxIndirect{Pipeline,Shader}ObjectCount` и отказ `CreateIndirectExecutionSetEXT`
  (коммит `tu/dgc: refuse indirect execution sets`) vkd3d не сломали: он читает
  только `supportedIndirectCommandsShaderStages` и execution set не создаёт.
* Ни одной строки уровня `:err:vkd3d`.
* Последняя строка — 20038 (≈429 с от старта), хвостом `KeyboardInterrupt`:
  игру убил таймаут скрипта, а не собственный сбой.
* `Failed to map read-only cache: Z:\...\vkd3d-cache\8326fa90fda3fb37\...` —
  норма для первого прогона драйвера с новым хэшем: кэш под него пустой.

## Что встало

```
23:16:03 systemd[1]: armada-powerd.service: Watchdog timeout (limit 15s)!
23:17:58 armada-control[187259]: subprocess.TimeoutExpired:
         Command '['/usr/libexec/armada/device-env']' timed out after 5 seconds
23:18:17 systemd[1]: armada-powerd.service: Control process exited, code=dumped, status=6/ABRT
23:18:17 systemd[1]: armada-powerd.service: Failed with result 'watchdog'.
23:18:48 armada-powerd[556886]: armada-powerd: irq affinity 7 (85 irqs)   # перезапуск
23:19:42 kwin_wayland[15530]: The main thread was hanging temporarily!
```

Цепочка:

1. `armada-powerd` (`Type=dbus`, `WatchdogSec=15`) кормит watchdog из
   `fan_tick()` (`/usr/libexec/armada/armada-powerd`, `sd_notify("WATCHDOG=1")`
   в 940/974/978) — то есть сигнал надо ждать **после** чтения температуры и
   записи `fan_pwm`. Не дождались за 15 с — systemd снимает сервис.
2. Снял сервис — запустился `ExecStopPost=…/armada-powerd --fan-safe`. Это
   control-процесс, а Fedora ставит всем `TimeoutStopFailureMode=abort`
   (`/usr/lib/systemd/system/service.d/10-timeout-abort.conf`), поэтому
   неповоротливый `--fan-safe` был добит SIGABRT — отсюда coredump с бэктрейсом
   `import_find_and_load` → `PyImport_ImportModuleLevelObject`: python не успел
   даже импорты доделать.
3. Дальше — следствие, а не причина. `armada-control.service` («privileged
   settings service») умер на том же пятисекундном таймауте: traceback
   `File "/usr/libexec/armada/armada-control", line 1073, in <module> main()` с
   `TimeoutExpired`. Значит тот, кто спросил у него `get_device_env`, получил не
   ответ, а исключение. `kwin_wayland` сообщил о зависшем main thread в 23:19:42 —
   через полторы минуты после снятия `armada-powerd` и уже внутри шторма
   `Under memory pressure` (с 23:19:53 такие строки идут пачками). **Какую именно
   D-Bus-операцию kwin ждал, мы не установили**: имя `org.armada.Power` больше
   нигде в системе не встречается (grep по `/usr/libexec`, Qt-плагинам и plasma
   каталогам дал только самого `armada-powerd`), так что списывать зависание
   композитора на снятый сервис нельзя — правдоподобнее, что kwin встал по той же
   причине, по которой встали `device-env` и `--fan-safe`: страницы и CPU не
   отдавали.

Почему `fan_tick` не успел за 15 секунд — память, а не логика:

```
Nov 25 19:32:22 kernel: avahi-daemon invoked oom-killer: ... global_oom
Nov 25 19:32:22 kernel: Out of memory: Killed process 178906 (Main) total-vm:143077832kB, anon-rss:1720200kB
23:19:53 systemd-journald[735]: Under memory pressure, flushing caches.   # 677 таких за загрузку
/dev/zram0  7.3 GiB swap
```

7.5 ГиБ RAM, Starfield + wine + FEX + vkd3d-потоки компиляции — это стабильно
за границей: zram затёрт, страницы демонов выгружены. `device-env` в простое —
14 мс:

```
$ time /usr/libexec/armada/device-env >/dev/null
real  0m0.014s
```

Тот же запуск во время прогона не уложился в 5 с (`armada-control`,
`subprocess.check_output([DEVICE_ENV], text=True, timeout=5)`), потому что
`fork`+`exec` bash'а означают чтение кода с устройства, которое в этот момент
занято подкачкой. То же самое с `python3.14 -c 'import gi.repository'`:
интерпретер и его .so нужно заново занести в RAM.

## Что чинит зависание

Медлит не логика демонов, а нехватка страниц: встал не один процесс, а весь путь
чтения кода с забитого подкачкой устройства — от `device-env` и `--fan-safe` до
композитора. Поэтому чинить надо узкое место — сколько страниц и CPU остаётся
демонам на горячем пути. Что помогает, по возрастанию цены:

1. **Не исполнять bash на D-Bus-пути.** Вывод `device-env` статичен на
   загрузку (модель из devicetree, класс SoC, ядра; живые только
   `/sys/power/mem_sleep` и `/etc/armada/sleep.conf`). Значит один раз на старте
   — в `/run/armada/device.env`, читатели читают файл. Сейчас `device_env()`
   дёргается из `bottom_screen_supported()`, из запросов к подсветке
   (`armada-control:266`, `:293`) и из окружения сессии (`:644`) — то есть
   несколько раз на движение ползунка яркости.
2. **Сигнал watchdog — вне пути сенсоров.** `sd_notify("WATCHDOG=1")` до `read_temp()`,
   а не после, либо отдельный `timeout_add`, который только и делает что кормит
   watchdog. Тогда 15-секундный лимит перестаёт зависеть от того, успел ли файл
   hwmon'а отдать число.
3. **Защитить демонов от reclaim'а** — копеечно и снимает симптом прямо сейчас:

   ```
   # /etc/systemd/system/armada-powerd.service.d/pressure.conf
   [Service]
   MemoryMin=64M
   CPUWeight=1000
   WatchdogSec=60
   TimeoutStopFailureMode=final
   ```

   `MemoryMin` держит страницы демона в RAM, `CPUWeight` даёт ему ход перед
   игрой, `WatchdogSec=60` убирает гонку с подкачкой, `final` — чтобы
   `--fan-safe` без coredump'а (сам coredump — ещё одна запись на то самое
   устройство в самый плохой момент).
4. **Не доводить машину до OOM.** Игра в юните с `MemoryHigh=`/`CPUWeight=10`
   (слайс или `systemd-run --user --slice=...`) меняет местами «игра тормозит» и
   «пропал веер и композитор».
5. Если хочется именно переписывать — то не `powerd`, а `device-env` в маленький
   статический бинарник, который читает два файла и печатает переменные, плюс
   кэш. Это дни работы и ноль python/bash на горячем пути.

## Что делать прогонам DGC, пока это не починено

* Прогон дольше ~10 минут на 7.5 ГиБ — лотерея. Для A/B хватает 5–7 минут:
  решение vkd3d о DGC принимается на создании устройства, оно видно в первых
  строках лога.
* Дамп `TU_DEBUG=dgcdump` во время игры раздувает лог и добавляет записи на то
  же устройство — на время проверки памяти его лучше не включать.
* Признак того, что встал не драйвер: в игровом логе нет `:err:vkd3d`, а в
  журнале — `armada-powerd.service: Watchdog timeout`. Признак обратной
  ситуации (это драйвер): `musa`/`kgsl` fault или `:err:vkd3d` с `DEVICE_LOST`.
