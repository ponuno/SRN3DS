# STREAMING - cell queue on the real read path (asset-prefetch)

Ветка `asset-prefetch`, база `2969b62`; схема 2 и экстерьерные клетки добавлены
в ветке `world-index` (база `841825d`). Этот документ фиксирует только то,
что сделано и проверено. Кадр не заявляется, `DevCaps` не менялся,
IR/thread/sync/guest-lift не затрагивались.

## 1. Что подключено

| Слой | Файл | Состояние |
|---|---|---|
| Таблица клеток в индексе | `source/asset_stream.{h,c}` | 64-байтные строки читаются, валидируются, ищутся бинарным поиском; схема 1 и 2 |
| Сборка индекса | `tools/stream_index_build.py` | Интерьер `--cell` и экстерьерное окно `--world/--grid/--radius`: WRLD → CELL `(gx,gy)` → REFR → base `MODL` → NIF → DDS |
| Парсер WRLD | `tools/skyrim/esm.py` | WRLD `EDID`/`DATA`/`NAM0`/`NAM9`, владелец exterior CELL по вложенности GRUP |
| Триггер клетки | `source/stream_cell.{h,c}` | Читает guest-память, P0 по cell-row, центр visible, trim при сжатии |
| Раз в кадр | `source/d3d9_com.c` (`present_common`) | `stream_host_frame_tick(g.mem)` после `d9_present()` |
| Demand-hit | `source/win32_shims.c` (`s_ReadFile`) | Слабый хук `stream_host_note_read()`: READY-окно отдаётся из RAM, дублирующий prefetch отменяется |
| Метрики | `StreamMetrics` + `StreamCellMetrics` | `physical_reads`, `bytes`, `misses`, `demand_hits`, `prefetch_demand_hits`, `demand_cancelled_prefetch`, переходы/trim |

Host-обёртки вокруг `0x69cbd0` в коде нет: frame pump целиком исполняется
лифтованным гостем (`n3ds_boot_run`). Поэтому триггер висит на `Present` -
единственном host-вызове, который guest делает ровно один раз за игровой
кадр. Это выбор задокументирован здесь и в комментарии `d3d9_com.c`;
`0x69cbd0` и его окрестности не патчатся.

## 2. Формат клетки (schema 2, 64 байта)

Схема 2 использует ту же 64-байтную строку, что и схема 1; меняется только
номер схемы в заголовке и то, что writer заполняет экстерьерные ключи. Читатель
принимает `schema == 1 || schema == 2`; файл схемы 1 читается как раньше.

```
key[24]:
  0  u32 kind                 0 = интерьер, 1 = экстерьер
  4  u32 form_id              TESForm::formID клетки (интерьер; 0 у экстерьера)
  8  i32 grid_x               экстерьер
 12  i32 grid_y               экстерьер
 16  u32 worldspace_form_id   экстерьер
 20  u32 reserved            0
 24  u32 resource_count      0..8
 28  u32 flags               0
 32  8 x u32 resource_id     только первые resource_count
```

Экстерьерная строка адресуется ровно тройкой `(worldspace_form_id, grid_x,
grid_y)`; `form_id` в ключе равен 0, потому что триггер его не знает (он
читает `[worldspace+0x0c]`, `+0x5c`, `+0x60`). Строки отсортированы по
`(kind, form_id, grid_x, grid_y, worldspace_form_id)`; загрузчик отвергает
нарушение порядка, `resource_count > 8`, `flags != 0`, `reserved != 0` и id
вне таблицы ресурсов. 8 id на клетку - ограничение формата, а не сборщика.

## 3. Сборка индекса

```powershell
python tools/stream_index_build.py `
  --data "E:/SteamLibrary/steamapps/common/Skyrim/Data" `
  --cell 0x133C6 `
  --out build/stream_index.bin `
  --report build/stream_index_report.json

# экстерьерное окно Tamriel вокруг Riverwood (9 клеток, 4..6 x -12..-10)
python tools/stream_index_build.py `
  --data "E:/SteamLibrary/steamapps/common/Skyrim/Data" `
  --world Tamriel --grid 5,-11 --radius 1 `
  --out build/stream_index_riverwood.bin
```

`--cell` принимает editor id (`RiverwoodSleepingGiantInn`) или form id
(`0x133C6`, десятичный), можно указать несколько раз. `--world` принимает
editor id (`Tamriel`) или form id (`0x3c`); `--grid X,Y` - центр окна,
`--radius N` - половина окна в клетках (по умолчанию 0: одна клетка).
`--cell` и `--world` можно совмещать в одном индексе. `--no-archive-sha`
оставляет SHA-256 архивов нулевым (размер всё равно реальный).
По умолчанию ESM - `Data/Skyrim.esm`, архивы - `Skyrim - Meshes.bsa`,
`Skyrim - Textures.bsa`, `Update.bsa`, `Dawnguard.bsa`, `Dragonborn.bsa`,
`HearthFires.bsa` в этом порядке.

Что делает сборщик:

1. Один проход по ESM: WRLD (`EDID`, `DATA` flags, `NAM0`/`NAM9` bounds),
   CELL (`EDID`/`XCLC`/`DATA`), REFR/`NAME`, `MODL` базовых объектов.
   Экстерьерная CELL получает form id своего WRLD по вложенности GRUP:
   только группы `Exterior Cell Block` (4) / `Sub-Block` (5). Persisent-клетка
   мира (например Tamriel `0x00000d74`) лежит прямо в `World Children` и
   несёт фиктивный `XCLC 0,0`, поэтому она не занимает реальный grid-ключ.
2. Для каждой выбранной клетки - её NIF-пути, затем чтение NIF из loose/BSA
   и извлечение строк `textures\...dds` из `BSShaderTextureSet`
   (`tools/skyrim/nif.py`). Дополнительно учитываются REFR, база которых -
   TXST (`TX00`).
3. Каждый путь резолвится в реальную запись каталога BSA; физический
   диапазон считается из фактического префикса записи (embedded name,
   4-байтный размер для deflate), а не оценивается. Loose-файлы имеют
   приоритет, но у ресурса нет loose-диапазона: они считаются в
   `resolver.loose`/`loose_skipped` и в индекс не попадают.
4. Ресурсы сортируются по нормализованному пути, id = номер строки; для
   каждой строки считается SHA-256 stored payload.
5. На клетку остаются 8 самых больших stored payload; усечение пишется в
   отчёт (`indexed_bytes` - сумма unpacked размеров этих восьми). Пустая
   клетка даёт строку с `resource_count = 0`, ничего не выдумывается.

### RiverwoodSleepingGiantInn (0x133C6), интерьер

```
refs=481  meshes=142  assets_total=292  indexed=8  truncated=284
indexed_bytes=1804543
archives=2 (Skyrim - Meshes.bsa, Skyrim - Textures.bsa)
resources=8  cell_count=1
```

### Tamriel 5,-11 radius 1 (Riverwood exterior), реальные цифры

```
schema=2 cells=9 (exterior=9, interior=0) resources=28 archives=2
resolver: bsa=793 loose=1173 missing=5
per cell: 8 ids each (rows are truncated from 41..124 resolved assets)
```

Клетки окна (3x3 вокруг `(5,-11)`): `Riverwood (4,-12)`,
`Riverwood01 (4,-11)`, безымянная `0x96f4 (4,-10)`,
`Riverwood04 (5,-12)`, `Riverwood02 (5,-11)`, `RiverwoodBridge (5,-10)`,
`Riverwood05 (6,-12)`, `Riverwood03 (6,-11)`, безымянная `0x96f2 (6,-10)`.
Пример индексированных путей: `meshes\architecture\farmhouse\inn01.nif`,
`meshes\landscape\trees\treepineforest01.nif`,
`textures\landscape\tundra01_n.dds`, `textures\clutter\...dds`.

### Экстерьер: что индексируется и чего нет

Индексируется цепочка `REFR -> NAME (base form) -> MODL -> NIF ->
BSShaderTextureSet DDS` - та же, что для интерьера, плюс прямые TXST-базы.
Не индексируются и честно записаны в отчёт:

* **LOD**: `.btr`/`.bto`/`_lod.nif` не выводятся из `(world,gx,gy)`: имена
  terrain-LOD строятся по блокам/квадам, а не по клеточному grid. Без
  отдельного разбора LOD-структуры мира ключи не выдумываются;
  `report["lod"]` фиксирует это.
* **LAND/landscape geometry и terrain textures**: `LAND`/`LTEX` не
  разбираются; в индекс попадают только NIF/DDS размещённых REFR.
* **Leveled lists/actors**: REFR на `LVLI`/`NPC_` не разворачивается.
* **Persistent-клетка мира** не даёт grid-строку (см. п.1).

## 4. Триггер `stream_cell_tick()`

```c
uint32_t holder = rd32(0x1b2e864);
uint32_t cell   = rd32(holder + 0x6c);
int32_t  gx     = rd32(holder + 0x5c);
int32_t  gy     = rd32(holder + 0x60);
uint32_t ws     = rd32(holder + 0xbc);
uint8_t  latch  = rd8(0x12e3560);     // "grid moved"
uint32_t ugrids = rd32(0x1241000);    // clamp 1..7
```

Смена кортежа `(cell, gx, gy, ws)` **или** `latch == 1` - переход. Для
интерьера (`ws == 0`) берётся ровно одна строка: `kind=0`,
`form_id = rd32(cell + 0x0c)`; для экстерьера - `kind=1`, `(gx+dx, gy+dy)` и
`worldspace_form_id = rd32(ws + 0x0c)`, обход центр → 4 соседа → кольцо.

Порядок работы:

1. Снять `visible` со старых handle.
2. `stream_mount_prefetch_ex(..., STREAM_PRIO_P0, deadline=0)` для каждой
   строки; для deflate-ресурса целевой размер берётся из
   `stream_mount_stage_extent()` (unpacked size), а не из stored
   `payload_length`. `bsa_read_range()` теперь читает сжатые записи, поэтому
   пропусков по `compressed` нет; `skipped_compressed` растёт только если
   архивный extent не разрешился (битый/изменённый индекс).
3. Все handle центральной строки - `stream_mount_mark_visible(1)`.
4. `stream_mount_pump(mount, 2, ...)` - не больше двух квантов за кадр.
5. Если рабочий набор сжался (`working < previous`), `stream_mount_trim()`
   до нового размера; освобождённые байты - в `trim_bytes`.

`formID +0x0C` - стандартная раскладка TESForm (флаги `+0x08`), но в этой
ветке она не подтверждена исполнением. Ошибка смещения даёт только промах
по индексу: guest-память читается в пределах объекта, ничего не пишется.
Для экстерьера `form_id` в ключе равен 0, поэтому этот путь не зависит от
`cell + 0x0c`; ключ - `(rd32(ws+0x0c), gx+dx, gy+dy)`.

### Включение

```
N3DS_STREAM=1
N3DS_STREAM_INDEX=sdmc:/Skyrim/stream_index.bin   # по умолчанию
N3DS_STREAM_DATA=sdmc:/Skyrim/Data               # по умолчанию
```

Без `N3DS_STREAM=1` модуль не открывает индекс и не трогает I/O.
В `Makefile` (`TESV_SLICE`) добавлены `asset_stream.o stream_cell.o`.
Хуки в `win32_shims.c` и `d3d9_com.c` объявлены слабыми, поэтому host-тесты,
не линкующие `stream_cell.c`, сохраняют прежнее поведение.

## 5. `s_ReadFile` и demand-hit

Перед чтением BSA-виртуального файла `s_ReadFile` вызывает
`stream_host_note_read(host_path, rel, dst, n, &out_len)`:

* host-путь режется по компоненту `Data` и канонизируется (lowercase, `\`);
* `stream_mount_demand_path()` ищет ресурс по пути;
* если READY-окно покрывает `[rel, rel+n)`, байты копируются из RAM,
  `physical_reads` не растёт, считаются `demand_hits` и
  `prefetch_demand_hits` (окно было prefetch, а не demand);
* если окно не покрывает запрос, живой prefetch без потребителей
  отменяется (`demand_cancelled_prefetch`), чтобы overlay не читал те же
  байты второй раз;
* иначе `0` - обычный overlay/loose путь.

Demand не создаёт новых запросов, поэтому prefetch не дублируется.
Loose-файлы и записи вне индекса идут старым путём.

## 5a. Снапшот памяти (`StreamStatsSnapshot`)

`stream_stats_snapshot(mount, &out)` снимает живые значения очереди и кэшей;
`stream_mount_open()` регистрирует последний mount глобально, поэтому
`mem_refresh()` в `tesv_slice_main.c` печатает их на `boot`/`start`/каждых
`N3DS_MEM_EVERY` кадрах/`exit` через `stream_stats_global()` и
`mem_stats_set_stream()`.

| Поле снапшота | Ключ `mem_stats` | Смысл |
|---|---|---|
| `queued` | `stream_queued` | дескрипторы в `STREAM_REQ_QUEUED` |
| `reading` | — | дескрипторы с активным `bsa_read_range` (`READING`) |
| `descriptors` | — | живые дескрипторы, включая терминальные |
| `staged_bytes` | `stream_staged` | живые READY/QUEUED окна (`cache_bytes`), бюджет по умолчанию 2 MiB |
| `staged_limit_bytes` | — | `cache_limit_bytes` из `StreamConfig` |
| `peak_staged_bytes` | — | максимум `staged_bytes` за сессию |
| `physical_reads` | `stream_reads` | успешные `bsa_read_range` исполнителя |
| `physical_bytes` | `stream_bytes` | байты, реально прочитанные и списанные с rate-бюджета (`quantum_bytes`) |
| `physical_misses` | `stream_misses` | промахи `StreamMetrics.misses` |
| `demand_hits` | `stream_demand_hits` | чтения, обслуженные из READY-окна |
| `demand_bytes` | `stream_demand_bytes` | байты этих demand-hit |
| `prefetch_demand_hits` | `stream_prefetch_demand_hits` | demand-hit, чьё окно было prefetch |
| `cancelled` | `stream_cancelled` | явные cancel + вытесненные P2 |
| `budget_denied` | `stream_budget_denied` | pump не получил байтовых токенов |

Счётчики монотонны, кроме `queued`/`staged_bytes`, которые отражают текущее
состояние. Проверка на реальном архиве: `mem_stats_bsa` (см. §6).

## 6. Тесты

```
python tests/stream_index_build_test.py                     # 422 checks
python tests/stream_index_test.py                           # 49 checks
powershell -ExecutionPolicy Bypass -File tools/run_host_tests.ps1 `
  -Only stream_range,stream_queue,stream_cell,stream_cell_real_prefetch -SkipPython
```

Результаты в этой ветке:

| тест | результат |
|---|---|
| `stream_range_smoke` | 91 checks, 0 failures |
| `stream_queue_smoke` | 67 checks, 0 failures |
| `mem_stats_bsa` | 13 архивов: index 2,054,436 B, page 425,984 B, resident 2,492,692 B; range_cache 0 → 1,057,557 B; stream reads=1, staged=65,536 B, demand_hits=1 |
| `mem_stats` | unit-счётчики BSA/stream без файлов |
| `stream_cell_smoke` | 69 checks, 0 failures (2 interior + 2 exterior rows, schema 1 reparse) |
| `stream_cell_real_prefetch` (Riverwood exterior 5,-11, r1) | 27 checks, 0 failures, 9 rows/8 ids, prefetch=39, skipped=0, physical_reads=33 |
| `stream_index_build_test.py` | 422 checks, 0 failures |
| `stream_index_test.py` (synthetic schema 1) | 49 checks, 0 failures |
| `stream_cell_real_smoke` (Riverwood interior) | 87 checks, 0 failures, 8/8 файлов, overlay resolves=8 |

`stream_cell_smoke` проверяет: смену кортежа → два P0; `0x12e3560==1` → P0;
интерьер → одна центральная строка; поиск по `(world,gx,gy)`; экстерьерный
тик находит центр и соседа и ставит их в P0; сжатие A(2 файла)→B(1 файл) →
trim; demand-hit из READY не увеличивает `prefetch_requests`; путь без окна -
промах; отсутствующий экстерьерный grid - `rows_missing`, а не выдуманные
запросы; индекс со `schema=1` по-прежнему парсится и ищется.

`stream_cell_real_smoke` запускается вручную с реальным индексом:

```powershell
$env:STREAM_INDEX_REAL="$env:TEMP\rw_index.bin"
$env:SKYRIM_ROOT="E:/SteamLibrary/steamapps/common/Skyrim"
# тест зарегистрирован только при наличии staged/rt_table.h
```

Он вызывает настоящие `GetFileAttributesExA`/`CreateFileA`/`ReadFile` через
BSA overlay; для всех 8 путей Riverwood размер > 0 и чтение возвращает
байты. Overlay сам больше не материализует deflate-запись: `bsa_read_range()`
читает её напрямую (инфляция один раз в LRU на архив).

`stream_cell_real_prefetch` (регистрируется всегда, пропускается без индекса)
гоняет настоящий `stream_cell` + pump на retail-архиве. Он проверяет обе
ветки, если они есть в индексе: интерьер `0x133C6` и экстерьер
`(world,gx,gy)` из env (по умолчанию `0x3c,0,0`):

```powershell
python tools/stream_index_build.py --data E:/harness_skyrim/Data `
  --world Tamriel --grid 5,-11 --radius 1 `
  --out build/stream_index_riverwood_world.bin
$env:STREAM_INDEX_REAL="$PWD/build/stream_index_riverwood_world.bin"
$env:STREAM_DATA_REAL="E:/harness_skyrim/Data"
$env:STREAM_CELL_WORLD="0x3c"; $env:STREAM_CELL_GX="5"; $env:STREAM_CELL_GY="-11"
powershell -ExecutionPolicy Bypass -File tools/run_host_tests.ps1 `
  -Only stream_cell_real_prefetch -SkipPython
```

Наблюдаемые цифры для окна Riverwood (все 8 id центральной строки
compressed): `prefetch_requests=39` (9 найденных строк окна),
`skipped_compressed=0`, `physical_reads=33`, `quanta=33`, `ready=9`;
demand-чтение `meshes\architecture\farmhouse\inn01.nif` обслуживается из RAM
(`served=1`, 4096 байт). Интерьерный прогон с индексом `0x133C6` даёт
прежние `prefetch_requests=8`, `physical_reads=26`, `ready=8`.

ARM compile-only (без линковки):

```
arm-none-eabi-gcc -O2 -std=gnu11 -march=armv6k ... -Isource -c source/bsa_reader.c
arm-none-eabi-gcc -O2 -std=gnu11 -march=armv6k ... -Isource -c source/asset_stream.c
arm-none-eabi-gcc -O2 -std=gnu11 -march=armv6k ... -Isource -c source/stream_cell.c
```

## 7. Что осталось

- **Retail-архивы.** Записи deflate теперь читаются любым диапазоном:
  `bsa_read_range()` держит горячую запись в 8 MiB LRU (≤4 MiB на запись),
  крупные записи стримит через 32 KiB окно. Prefetch сжатых ресурсов работает
  (см. `stream_cell_real_prefetch`); выигрыш ограничен 8 id на клетку и
  пропускной способностью одного pump-потока.
- **8 id на клетку.** Формат (schema 1 и 2) жёстко задаёт 8; Riverwood
  усечён с 292 до 8, экстерьерные клетки - с 41..124 до 8. Расширение требует
  смены записи/схемы, а не обхода.
- **Экстерьер.** WRLD form id теперь отслеживается, строки `kind=1` пишутся
  для выбранного окна `--world/--grid/--radius`. Весь Tamriel (11187 клеток)
  намеренно не индексируется. LOD (`.btr`/`.bto`/`_lod.nif`), `LAND`/`LTEX`
  terrain, leveled lists и persistent-клетка мира не индексируются; это
  записано в отчёте (`lod`, `exterior`, `warnings`).
- **`formID +0x0C`** не подтверждён рантаймом; цена ошибки - промах.
  Экстерьерный ключ от него не зависит: он строится из `ws+0x0c` и grid.
- **Поток.** Pump синхронный, 2 кванта/кадр, отдельного I/O-потока нет.
- **GPU.** READY - это RAM, публикации в SKTX/GPU нет.
- **Метрика "доля P0, догнавших demand"** считается по
  `prefetch_demand_hits / demand_hits`; на retail-индексе окно и demand теперь
  совпадают, метрика перестала быть структурно нулевой.
