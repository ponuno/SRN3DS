# STREAMING - cell queue on the real read path (asset-prefetch)

Ветка `asset-prefetch`, база `2969b62`. Этот документ фиксирует только то,
что сделано и проверено в этой ветке. Кадр не заявляется, `DevCaps` не
менялся, IR/thread/sync/guest-lift не затрагивались.

## 1. Что подключено

| Слой | Файл | Состояние |
|---|---|---|
| Таблица клеток в индексе | `source/asset_stream.{h,c}` | 64-байтные строки читаются, валидируются, ищутся бинарным поиском |
| Сборка индекса | `tools/stream_index_build.py` | Интерьерные клетки: `MODL` → NIF, строки NIF → DDS; строки ресурсов строятся по реальному каталогу BSA |
| Триггер клетки | `source/stream_cell.{h,c}` | Читает guest-память, P0 по cell-row, центр visible, trim при сжатии |
| Раз в кадр | `source/d3d9_com.c` (`present_common`) | `stream_host_frame_tick(g.mem)` после `d9_present()` |
| Demand-hit | `source/win32_shims.c` (`s_ReadFile`) | Слабый хук `stream_host_note_read()`: READY-окно отдаётся из RAM, дублирующий prefetch отменяется |
| Метрики | `StreamMetrics` + `StreamCellMetrics` | `physical_reads`, `bytes`, `misses`, `demand_hits`, `prefetch_demand_hits`, `demand_cancelled_prefetch`, переходы/trim |

Host-обёртки вокруг `0x69cbd0` в коде нет: frame pump целиком исполняется
лифтованным гостем (`n3ds_boot_run`). Поэтому триггер висит на `Present` -
единственном host-вызове, который guest делает ровно один раз за игровой
кадр. Это выбор задокументирован здесь и в комментарии `d3d9_com.c`;
`0x69cbd0` и его окрестности не патчатся.

## 2. Формат клетки (schema 1, 64 байта)

```
key[24]:
  0  u32 kind                 0 = интерьер, 1 = экстерьер
  4  u32 form_id              TESForm::formID клетки (интерьер)
  8  i32 grid_x               экстерьер
 12  i32 grid_y               экстерьер
 16  u32 worldspace_form_id   экстерьер
 20  u32 reserved            0
 24  u32 resource_count      0..8
 28  u32 flags               0
 32  8 x u32 resource_id     только первые resource_count
```

Строки отсортированы по `(kind, form_id, grid_x, grid_y, worldspace_form_id)`;
загрузчик отвергает нарушение порядка, `resource_count > 8`, `flags != 0`,
`reserved != 0` и id вне таблицы ресурсов. В схеме 1 ровно 8 id на клетку -
это ограничение формата, а не сборщика.

## 3. Сборка индекса

```powershell
python tools/stream_index_build.py `
  --data "E:/SteamLibrary/steamapps/common/Skyrim/Data" `
  --cell 0x133C6 `
  --out build/stream_index.bin `
  --report build/stream_index_report.json
```

`--cell` принимает editor id (`RiverwoodSleepingGiantInn`) или form id
(`0x133C6`, десятичный). Можно указать несколько раз. `--no-archive-sha`
оставляет SHA-256 архивов нулевым (размер всё равно реальный).
По умолчанию ESM - `Data/Skyrim.esm`, архивы - `Skyrim - Meshes.bsa`,
`Skyrim - Textures.bsa`, `Update.bsa`, `Dawnguard.bsa`, `Dragonborn.bsa`,
`HearthFires.bsa` в этом порядке.

Что делает сборщик:

1. Один проход по ESM: CELL/REFR/`NAME`, `MODL` базовых объектов
   (существующий `tools/skyrim/esm.py`).
2. Для каждой выбранной интерьерной клетки - её NIF-пути, затем чтение NIF
   из loose/BSA и извлечение строк `textures\...dds` из
   `BSShaderTextureSet` (`tools/skyrim/nif.py`). Дополнительно учитываются
   REFR, база которых - TXST (`TX00`); на практике их в клетках нет, а
   основные DDS приходят из NIF-текстурсета - это те же строки, которые
   несёт TXST, и именно их связывает рендерер.
3. Каждый путь резолвится в реальную запись каталога BSA; физический
   диапазон считается из фактического префикса записи (embedded name,
   4-байтный размер для deflate), а не оценивается. Loose-файлы имеют
   приоритет, но в schema 1 у ресурса нет loose-диапазона: они считаются в
   `resolver.loose`/`loose_skipped` и в индекс не попадают (иначе запись
   была бы фиктивной).
4. Ресурсы сортируются по нормализованному пути, id = номер строки; для
   каждой строки считается SHA-256 stored payload.
5. На клетку остаются 8 самых больших stored payload; усечение пишется в
   отчёт (`indexed_bytes` - сумма unpacked размеров этих восьми). Пустая
   клетка даёт строку с `resource_count = 0`, ничего не выдумывается.

### RiverwoodSleepingGiantInn (0x133C6), реальные цифры

```
refs=481  meshes=142  assets_total=292  indexed=8  truncated=284
indexed_bytes=1804543
archives=2 (Skyrim - Meshes.bsa, Skyrim - Textures.bsa)
resources=8  cell_count=1
```

Индексированные пути (порядок = от большего stored payload):

```
meshes\furniture\mq203_esberntable.nif
meshes\furniture\alchemyworkbench.nif
textures\landscape\trees\treepineforestbarkcomp_n.dds
textures\clutter\deadanimals\hanginganimals01_n.dds
textures\clutter\containers\miscbag01.dds
textures\clutter\moneybag01.dds
textures\architecture\farmhouse\stonewall02_n.dds
textures\clutter\barrel02_n.dds
```

### Экстерьер

Не собран. Текущий проход ESM не отслеживает WRLD form id, которому
принадлежит CELL, а придумывать его нельзя: неверный ключ даёт только
промахи. Поэтому `cell_count` считает только интерьеры; это записано и в
JSON-отчёте (`exterior: "not emitted: no WRLD form id in this ESM pass"`).
Триггер экстерьерную ветку уже умеет (центр → 4 соседа → кольцо
`uGridsToLoad`), но по пустой таблице она честно считает `rows_missing`.

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

## 6. Тесты

```
python tests/stream_index_build_test.py                     # 52 checks
powershell -ExecutionPolicy Bypass -File tools/run_host_tests.ps1 `
  -Only stream_range,stream_queue,stream_cell,stream_cell_real_prefetch -SkipPython
```

Результаты в этой ветке:

| тест | результат |
|---|---|
| `stream_range_smoke` | 91 checks, 0 failures |
| `stream_queue_smoke` | 67 checks, 0 failures |
| `stream_cell_smoke` | 57 checks, 0 failures |
| `stream_cell_real_prefetch` (Riverwood) | 21 checks, 0 failures, 8 compress×, prefetch=8, skipped=0, physical_reads=26 |
| `stream_index_build_test.py` | 52 checks, 0 failures |
| `stream_cell_real_smoke` (Riverwood) | 87 checks, 0 failures, 8/8 файлов, overlay resolves=8 |

`stream_cell_smoke` проверяет: смену кортежа → два P0; `0x12e3560==1` → P0;
интерьер → одна центральная строка; сжатие A(2 файла)→B(1 файл) → trim;
demand-hit из READY не увеличивает `prefetch_requests`; путь без окна -
промах; экстерьер без строк - `rows_missing`, а не выдуманные запросы.

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
гоняет настоящий `stream_cell` + pump на retail-архиве:

```
python tools/stream_index_build.py --data E:/harness_skyrim/Data \
  --cell RiverwoodSleepingGiantInn --out build/stream_index_riverwood.bin
$env:STREAM_INDEX_REAL="$PWD/build/stream_index_riverwood.bin"
powershell -ExecutionPolicy Bypass -File tools/run_host_tests.ps1 `
  -Only stream_cell_real_prefetch -SkipPython
```

Наблюдаемые цифры (8/8 строк Riverwood compressed): `prefetch_requests=8`,
`skipped_compressed=0` (было 8 → prefetch 0), `physical_reads=26`,
`quanta=26`, `ready=8`; demand-чтение первого ресурса обслуживается из RAM
(`served=1`).

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
- **8 id на клетку.** Формат schema 1 жёстко задаёт 8; Riverwood усечён с
  292 до 8. Расширение требует смены схемы/записи, а не обхода.
- **Экстерьер.** Нет WRLD form id в проходе ESM; ключи `kind=1` пишутся
  только когда появится эта связь. LOD/landscape ключи не выдумывались.
- **`formID +0x0C`** не подтверждён рантаймом; цена ошибки - промах.
- **Поток.** Pump синхронный, 2 кванта/кадр, отдельного I/O-потока нет.
- **GPU.** READY - это RAM, публикации в SKTX/GPU нет.
- **Метрика "доля P0, догнавших demand"** считается по
  `prefetch_demand_hits / demand_hits`; на retail-индексе окно и demand теперь
  совпадают, метрика перестала быть структурно нулевой.
