# Skyrim N3DS — v0.12 (реальный D3DX pilot в Azahar)

Экспериментальная основа для будущего порта [Skyrim_Android](https://github.com/ttom795/Skyrim_Android). Это ещё не игровой порт: рекомпилированный TESV пока не подключён, графика доступна только в демонстрационных режимах. Код игры и её файлы не входят в архив.

## v0.12 — реальный D3DX9 в Azahar, скрипты сборки и честные измерения

Проверен путь «оригинальная x86 DLL → рекомпилированный ARM → Azahar» на
системной 32-битной `D3DX9_42.dll` (локальный файл владельца, SHA256
`f8b9cfab7fffbc8f98e41aa439d72921dc180634a1febca2a9d41a0df35d3472`).
`D3DXMatrixMultiply` (217 x86 инструкций, 568 байт кода) и
`D3DXMatrixScaling` дают `PASS (3/3)` и на ПК, и в настольном Azahar 2126.2;
`D3DXMatrixTranspose` и `D3DXMatrixTranslation` — `PASS (3/3)` на ПК.
Скриншоты и логи: `build_azahar/20261124-011746-Skyrim_N3DS_pilot_v012/` и
`build_azahar/20261124-012459-Skyrim_N3DS_pilot_scaling/`.
Проверка на реальной New 3DS по-прежнему не выполнена.

Важное открытие: экспорты этой DLL — не обычные функции, а MSVC lazy-ILT
thunk (`FF 25 <slot>` + 5×`CC`). Слот изначально указывает на guard-wrapper,
который вызывает одноразовый инициализатор и затем переходит через
исправленный слот. `tools/prepare_pilot.py` теперь проверяет ровно эту
раскладку (включая padding, пролог, вызов инициализатора, адрес тела
`export+0xB`) и **явно** фиксирует в манифесте и логе, что инициализатор
обойдён. При несовпадении генерация падает с конкретной ошибкой, а не угадывает.
Добавлены unit-тесты резолвера (`tests/hotpatch_resolver_test.py`).

Новые воспроизводимые инструменты:

```powershell
python -m venv .venv
.venv\Scripts\python.exe -m pip install capstone==5.0.9 pefile==2024.8.26
.venv\Scripts\python.exe tools/stage_upstream.py ..\Skyrim_Android staged
.venv\Scripts\python.exe tools/prepare_pilot.py "C:\Windows\SysWOW64\D3DX9_42.dll" staged pilot_out_mult --export D3DXMatrixMultiply

powershell -ExecutionPolicy Bypass -File tools/run_host_tests.ps1   # 9 проверок
powershell -ExecutionPolicy Bypass -File tools/run_host_tests.ps1 -Sanitize
powershell -ExecutionPolicy Bypass -File tools/build.ps1 -Mode pilot `
  -PilotGen pilot_out_mult\D3DXMatrixMultiply\gen
powershell -ExecutionPolicy Bypass -File tools/run_azahar.ps1 `
  -ThreeDsx dist\Skyrim_N3DS_pilot\Skyrim_N3DS_pilot.3dsx -TimeoutSec 20
```

`tools/build.ps1` всегда делает чистую сборку, кладёт `.elf`, `.3dsx`,
`build.log` и `build_manifest.json` (SHA256, версия toolchain, точная команда)
в `dist/<target>/`. `tools/run_azahar.ps1` копирует `.3dsx` на виртуальную SD,
запускает отдельный экземпляр Azahar, снимает скриншот и логи в
`build_azahar/<время>-<имя>/` и закрывает только свой процесс.

Прочие результаты этой итерации (2026-11-24):

- Все 9 host-проверок проходят, включая ASan; `win32_shims.c` теперь
  компилируется и на Windows-host (QueryPerformanceCounter/Sleep вместо
  POSIX `clock_gettime`/`nanosleep`).
- В Azahar впервые проверены и работают: диагностика (`Lift ABI: PASS`),
  boot smoke (`PEB/TEB/TLS, IAT, DLL attach, entry and ExitProcess passed`),
  графическое демо (куб, 59.8 FPS) и graphics_com (`IAT/COM/upload`, BC,
  SM2 templates, vertex declaration, mip chains — PASS).
- Исправлена ошибка Makefile: `PILOT_GEN`/`LIFT_STAGE` из командной строки не
  абсолютизировались (`override`), из-за чего относительный `PILOT_GEN`
  ломал include-пути в MSYS2. Добавлены `OPT` (по умолчанию `-O2`) и
  `--gc-sections`.
- Добавлены Win32 `InitializeCriticalSection[AndSpinCount]`,
  `Delete/Enter/LeaveCriticalSection` (гостевая 24-байтная структура, счётчик
  рекурсии, один ожидаемый поток) и `Get/SetCurrentDirectoryA`; проверены в
  host-тестах и в Azahar (boot smoke).
- `tools/compare_pe.py` доказывает идентичность незащищённой копии TESV
  установленной игре (совпадают все секции, кроме `.text`/`.bind`);
  `tools/audit_game.py --exe` позволяет аудировать такую копию с DLL игры.
- Измерено расширение кода: 568 байт x86 → 12 348 байт ARM (`-O2`), то есть
  ~21.7×. Наивный полный лифт 13.58 МБ кода TESV дал бы ~295 МБ ARM и
  физически не помещается. Это главный архитектурный блокер; план и цифры —
  в `docs/MEMORY_BUDGET.md`, статус — в `docs/STATUS.md`, roadmap обновлён.

**Уточнение бюджета (CIA, 178 МиБ).** Ранее расчёты опирались на 96 МиБ,
которые libctru сообщает в `.3dsx`-режиме. В `exheader.h` libctru
`SYSMODE_DEV1` определён как «97MB/178MB» (O3DS/N3DS), то есть CIA с
`SystemModeExt: 178MB` даёт 178 МиБ. При таком бюджете стартовый срез
(~51.8 МиБ ARM + 28.1 МиБ guest + данные GPU) **умещается** с запасом
~90 МиБ, тогда как в 96 МиБ он не помещался. Это меняет вывод только для
стартового среза; полный лифт игры по-прежнему невозможен.

Добавлены `tools/build_cia.ps1` и `tools/cia/template.rsf` (178MB, 804 MHz,
L2): собирается локальный `build_cia/Skyrim_N3DS_tesv.cia` (352 192 байта,
sha256 `ba50a0d6…`). Первый запуск CIA на консоли упал **до `main`**: дамп
`build_crash/crash_dump_00000070.dmp` показывает `svcBreak` из
`__system_allocateHeaps` — libctru по умолчанию запрашивает в режиме 178 МиБ
application heap больше области кучи. Исправлено явными
`__ctru_heap_size = 56 МиБ` и `__ctru_linear_heap_size = 16 МиБ`; приложение
теперь печатает реальные `App region/free` и пишет их в
`Skyrim_N3DS_tesv.log`. Нужен повторный прогон на консоли (и установка CIA
через GUI File → Install CIA — `azahar -i` без GUI не завершился).
CIA содержит локально сгенерированный код и не подлежит распространению.

Аудит `E:\SteamLibrary\steamapps\common\Skyrim` (v6, с текущими шимами):
205 уникальных неразрешённых импортов (kernel32 — 75), 189 записей покрыто
шимами, 14 локальных экспортов ещё не подняты; всего 244 неразрешённые записи.
`d3dx9_42.dll` и `X3DAudio1_7.dll` рядом с игрой отсутствуют; 32-битная D3DX
берётся из системы.

**Обновление по блокеру:** владелец предоставил незащищённую копию
`E:\skyr\TESV.exe` (SHA-1 `17ec1ef6…`, SHA256 `d97eb0cc…`). `tools/compare_pe.py`
доказывает, что это тот же билд 1.9.32.0.8: `.data`, `.rdata`, `.reloc`,
`.rsrc`, `.tls` побайтово совпадают со Steam-копией, отличаются только
расшифрованный `.text` и удалённая секция `.bind`. `tools/inspect_pe.py`
показывает нормальную энтропию секций. DRM проектом не обходился — файл
предоставлен пользователем.

Полный discovery TESV завершён: 90 900 функций, покрыто 12,33 из 13,01 МБ
кода (94,7%), 4 029 гэпов (46,8 КБ), 1 083 таблицы переходов, 1 201
неразрешённый косвенный переход. Результат:
`E:\Skyrim_N3DS_build\analysis\tesv\functions.json` (18,7 МБ). Новый
`tools/slice_pe.py` считает достижимость по прямым вызовам: от точки входа —
13 390 функций, 955 134 инструкции, 159 импортов, проекция ~51,8 МиБ ARM
`.text`. `tests/tesv_probe.c` реально маппит TESV и проходит IAT: 266
импортов, 187 неразрешённых; первые в порядке IAT — `CompareFileTime`,
`FindFirstFileA`, `lstrcpyA`, `lstrcatA`, `FileTimeToSystemTime`,
`FileTimeToLocalFileTime`, `LocalFree`, `FormatMessageA`, `CreateDirectoryA`.
Это и есть точная очередь работ для M3.

**Исправление краха CIA (178 МиБ).** Первый запуск на консоли падал до
`main`: libctru `__system_allocateHeaps` вызывал `svcBreak`, потому что
дефолтный сплит куч в режиме 178 МиБ превышает область кучи. Дамп
(`build_crash/crash_dump_00000070.dmp`) разобран `tools/parse_luma_dump.py`
(PC → `svcBreak`, LR → `__system_allocateHeaps`). Теперь явно заданы
`__ctru_heap_size = 56 МиБ`, `__ctru_linear_heap_size = 16 МиБ`, guest-бюджет
48 МиБ (из newlib-кучи, а не из `osGetMemRegionFree`), а приложение печатает и
пишет в лог `App region/free`. В Azahar `.3dsx` после фикса:
`app_region=96 MiB`, guest 28,1 МиБ, fault `0xf6916b` вместо паники; повторный
прогон CIA на консоли ещё нужен.

Добавлены и проверены на host файловые Win32 API: `GetFileAttributesA/ExA`,
`FindFirstFileA/FindNextFileA/FindClose` (полный 320-байтный
`WIN32_FIND_DATAA`, `*`/`?`), `CreateDirectoryA`, `DeleteFileA`,
`GetFullPathNameA`, запись (`CreateFileA` с write/dispositions,
`WriteFile`, `SetFilePointerEx`, `SetEndOfFile`, `FlushFileBuffers`,
`GetFileSizeEx`). Ограничения (share-режимы не проверяются, 8.3 имени нет,
абсолютные пути вне sandbox отклоняются) записаны в `docs/COMPATIBILITY.md`.

`tools/slice_pe.py` теперь локально декодирует прямые цели, которых нет в
манифесте discovery (рекурсивный спуск, эвристика jump table,
`--extra-root-file`), с тестами `tests/slice_pe_test.py`. Это убирает
пошаговые отказы вида «unknown function» на известных прямых вызовах.

Ранее также добавлены и проверены на host+Azahar: `LocalAlloc/LocalFree`,
`HeapCreate/HeapDestroy` (до 4 отдельных guest-куч), `EncodePointer/DecodePointer`
(XOR-ключ процесса), `CompareFileTime`, преобразования `FILETIME`/`SYSTEMTIME`,
`GetLocalTime`, `GetTimeZoneInformation`, `lstrcpyA/lstrcatA`, `GetDriveTypeA`,
`InterlockedExchangeAdd`, `FormatMessageA` (generic fallback), `DebugBreak`
(trap), `GetModuleHandleW`, кодовые страницы (`GetACP/GetOEMCP/GetCPInfo/
IsValidCodePage`), `MultiByteToWideChar/WideCharToMultiByte` (ACP/UTF-8),
`Get/SetStdHandle`, `GetFileType`, `IsDebuggerPresent`,
`IsProcessorFeaturePresent` (консервативно FALSE), SEH-фильтры и
`TerminateProcess` для текущего процесса.

Синхронизация, user32/GDI и locale вынесены в отдельные модули с собственными
thunk-диапазонами и тестами: `win32_sync` (mutex/semaphore/event, wait с
честным trap на INFINITE-дедлок), `win32_user` (классы/окна/rect/метрики
400×240, пустая очередь сообщений), `win32_locale` (en-US/0x0409:
GetLocaleInfo, CompareString, дата/время; прочие локали — ошибка 87).
`win32_shims` диспетчеризует их через общий `win32_resolve`/`win32_lookup`.

**Первый реальный запуск lifted-кода TESV.** Добавлен диагностический режим
`TESV_SLICE`: `tools/slice_pe.py` выбирает ограниченный срез (entry + оба TLS
callback + прямые/статические косвенные цели), `staged/lift.py` его лифтит,
`tools/instrument_generated.py` вставляет счётчик базовых блоков, а
`source/tesv_slice_main.c` маппит настоящий `TESV.exe`, связывает импорты и
исполняет entry с лимитом. Неизвестные импорты получают trap-адрес и падают
с точным `dll!name` — фиктивного успеха нет. Срез из 70 функций прошёл на
хосте и в Azahar; цепочка первых отказов после исправлений:

1. `KERNEL32.dll!HeapCreate` → реализованы отдельные кучи;
2. `0x00f76345` (unlifted, косвенный вызов) → срез расширен;
3. `0x00f5d5c3` (unlifted) → срез расширен;
4. `KERNEL32.dll!EncodePointer` → реализованы Encode/DecodePointer;
5. текущий отказ: `0x00f6916b` (unlifted); гостевой бюджет 28,1 МиБ.

Это не запуск игры, а точный измеримый шаг M2/M3. Команды:

```powershell
python tools/slice_pe.py "E:\skyr\TESV.exe" `
  E:\Skyrim_N3DS_build\analysis\tesv\functions.json --root entry `
  --extra-root 0xf794ab --extra-root 0xf5b303 --max-functions 70 --emit `
  --out E:\Skyrim_N3DS_build\analysis\slice_small5
python staged/lift.py "E:\skyr\TESV.exe" `
  E:\Skyrim_N3DS_build\analysis\slice_small5\slice_functions.json `
  E:\Skyrim_N3DS_build\analysis\gen_small5 tesv 400000
python tools/instrument_generated.py E:\Skyrim_N3DS_build\analysis\gen_small5
powershell -File tools/build.ps1 -Mode tesv -TesvGen F:\...\gen_small5
powershell -File tools/run_azahar.ps1 -ThreeDsx dist\Skyrim_N3DS_tesv\Skyrim_N3DS_tesv.3dsx -AllowFaultReport
```

Эксперимент по «дешёвому» lowering памяти отклонён: inline fast-path с кэшем
региона раздул функцию с 12 348 до 29 028 байт ARM (2.35×). Уменьшать размер
нужно на уровне эмиссии инструкций/паттернов, а не доступа к памяти.

Ниже сохранены разделы v0.11 и более ранних версий.

## v0.11 — запуск пилотной функции D3DX и аудит модулей

Вместо очередного графического демо проверяем путь от оригинальной x86 DLL до ARM-кода. В Windows, в терминале devkitPro/MSYS2, из каталога проекта:

```sh
python -m pip install capstone==5.0.9 pefile==2024.8.26
python tools/stage_upstream.py ../Skyrim_Android staged
python tools/audit_game.py "C:/Games/Skyrim" --output skyrim_audit.json
python tools/prepare_pilot.py "C:/Games/Skyrim/d3dx9_42.dll" staged pilot_out
```

Если `staged` уже создан предыдущей версией, повторно создавать его не надо. Укажи собственный путь к игре. Для последующих запусков используй новые каталоги отчёта/генерации либо убери уже созданные вручную.

`prepare_pilot.py` пытается выделить замкнутую цепочку прямых вызовов `D3DXMatrixTranspose`, `D3DXMatrixScaling`, `D3DXMatrixTranslation` или `D3DXMatrixMultiply`, передаёт её исходному lifter и проверяет результат. Косвенные переходы, внешние прямые вызовы, рекурсия и отклонённые инструкции не допускаются в этом пилоте. Если ни одна функция не подходит, смотри `pilot_out/rejections.json` и `lift.log` соответствующей попытки. Это возможный и полезный результат диагностики.

При успехе скрипт выведет путь `PILOT_GEN`. Например, для первой функции:

```sh
make LIFT_STAGE=staged PILOT_GEN=pilot_out/D3DXMatrixTranspose/gen clean
make LIFT_STAGE=staged PILOT_GEN=pilot_out/D3DXMatrixTranspose/gen
```

Подставь фактически выбранный экспорт. Внутри каталога сборки не смешивай файлы от разных пилотов. Скопируй `Skyrim_N3DS_pilot.3dsx` на SD, а точно ту же DLL — в `/Skyrim/d3dx9_42.dll`. На New 3DS ожидается имя экспорта, результат `PASS (3/3)` либо конкретный сбой, расход гостевой памяти и файл `/Skyrim_N3DS_pilot.log`. START — выход.

Пилот проверяет соответствие DLL исходнику генерации (размер и FNV64), маппинг PE, вызов C-функции через гостевой CPU, три матрицы, возвращаемый указатель, неизменность входов и контрольных байтов. Выполнение ограничено счётчиком базовых блоков. DllMain, IAT и TLS callbacks в данном режиме не запускаются; это не запуск TESV.

Аудит не требует внешних Python-библиотек и ничего не меняет в игре. `skyrim_audit.json` содержит размеры образов, SHA256, импорты, экспорты и список ещё не покрытых импортов. Наличие shim не гарантирует корректность поведения. Отчёт не включает байты бинарников, динамические импорты и память ресурсов/ARM-кода. Для следующего этапа достаточно прислать этот JSON, вывод генератора и лог консоли.

Проверка на ПК выполнена на самостоятельно созданной минимальной PE32 DLL и написанном вручную C-аналоге функции Scaling: 3/3 матрицы совпали, DLL с несовпадающим хешем отклонена. Фактическая генерация через Capstone/pefile, оригинальная DLL Skyrim и сборка/запуск на New 3DS здесь не проверены.

## v0.10 — Vertex Declaration и геометрия

Сборка: `make GRAPHICS_COM=1 LIFT_STAGE=staged`. В дополнение к A/X/Y/B, **L** переключает стандартный FVF `XYZ|TEX1` и тестовую Vertex Declaration с двумя отдельными потоками. На нижнем экране показывается `Vertex declaration: PASS` и текущий способ (`FVF` или `decl`). Куб должен сохранять геометрию и текстуру при переключении.

Новые COM методы: `Create/Set/GetVertexDeclaration`, `IDirect3DVertexDeclaration9` (`QueryInterface`, `AddRef`, `Release`, `GetDevice`, `GetDeclaration`), `GetStreamSource`, `Set/GetStreamSourceFreq` (только обычная частота 1, без instancing), `GetIndices`. `SetStreamSource` принимает потоки 0..15, offset и stride. Ссылки удерживаются устройством; создание/переключение деклараций работает с обычным COM lifecycle.

`d3d9_vertex.c` проверяет D3DVERTEXELEMENT9, ограничивает декларацию 64 элементами, проверяет конец, типы, дубли семантик, перекрытия, порядок потоков и границы stride. Форматы декодера:

| Формат | Поддержка декодирования |
|---|---|
| FLOAT1–4 | 32-битные float |
| FLOAT16_2, FLOAT16_4 | IEEE binary16 (включая subnormal) |
| D3DCOLOR, UBYTE4, UBYTE4N | packed color / целые / normalized |
| SHORT2/4, SHORT2N/4N, USHORT2N/4N | signed/unsigned 16-битные и normalized |
| UDEC3, DEC3N | 10-битные packed компоненты |

Декларация преобразуется в текущий PICA vertex format `XYZ + TEX1`: реально используются POSITION0 и TEXCOORD0. Дополнительные семантики (NORMAL, TANGENT, COLOR, BLENDWEIGHT и пр.) проверяются на корректное расположение, но **не участвуют в освещении, skinning или произвольных шейдерах**. POSITIONT и нестандартные declaration methods отклоняются. Позиция с 4 компонентами допустима только при w=1. Это поддержка части геометрии, а не полноценный Skyrim shader pipeline.

Добавлены INDEX32, проверка MinVertexIndex/NumVertices и base vertex offset, TRIANGLESTRIP/TRIANGLEFAN для indexed/nonindexed draw (CPU разворачивает в triangle list с правильным winding). Повторяющиеся vertex indices используют небольшой кэш декодированных вершин в рамках draw. Декларации и потоки проверяются на наличие данных и границы прежде чем отправить геометрию на GPU.

В демо stream 0 содержит FLOAT3 position и packed DEC3N normal, stream 1 — FLOAT16_2 UV; у обоих потоков есть ненулевой offset. На ПК проверено побайтовое совпадение выходных 36 вершин с прежним FVF, в том числе с INDEX32, а также COM roundtrip GetDeclaration/GetStreamSource и ошибки отсутствующего потока/индекса.

```sh
cc -std=c11 -O2 -Isource source/d3d9_core.c source/d3d9_texcache.c source/d3d9_texture_data.c source/d3d9_bc.c source/d3d9_vertex.c tests/vertex_smoke.c -lm -o vertex_smoke
./vertex_smoke
```

Тесты нового декодера и COM прошли на ПК с ASan/UBSan. Сборка `.3dsx` v0.10 и фактическая проверка PICA ещё предстоят. Остальные инструкции ниже сохранены для предыдущих этапов.

## v0.9 — mipmaps, частичные прямоугольники и состояния рендера

Сборка на Windows: `make GRAPHICS_COM=1 LIFT_STAGE=staged`. Результат — ожидаемый `Skyrim_N3DS_graphics_com.3dsx`.

- **A** — BGRA8 / DXT1 / DXT3 / DXT5.
- **X** — fixed-function / ограниченные SM2-шаблоны.
- **Y** — меняет минимальный используемый mip-уровень 0..3 (на текстурах DXT уровни имеют разные цвета, чтобы было заметно переключение).
- **B** — включает/выключает SRCALPHA/INVSRCALPHA blending. DXT3 и DXT5 содержат участки с полупрозрачными texel.
- START — выход.

### Mip-цепочки

`CreateTexture` принимает точное количество уровней либо 0 для полной цепочки. В гостевой памяти остаются все размеры вплоть до 1×1, у BC форматов для последних уровней сохраняются целые 4×4 блоки. `GetLevelCount`, `GetLevelDesc`, `LockRect` и `UnlockRect` работают с конкретным уровнем. BGRA rect поддерживает произвольные области; BC rect должен быть выровнен по блокам (правый/нижний край может совпасть с размером уровня). Staging содержит только span требуемых строк, при Unlock копируются только изменяемые области; READONLY не вызывает повторной GPU-загрузки.

PICA200 принимает только уровни, у которых обе стороны не меньше 8. Для текстуры 64×64 хранятся семь гостевых уровней, а четыре (64,32,16,8) загружаются на GPU через `C3D_TexLoadImage`. Остальные три — CPU-only tail. GPU min/max LOD ограничиваются непрерывной последовательностью загруженных уровней, чтобы незаполненный промежуточный mip никогда не семплировался. Мип-фильтрация на GPU включается через sampler states; автоматическая генерация mipmaps не реализована.

### D3D9 render/sampler states

Добавлены COM методы `SetRenderState`, `GetRenderState`, `SetSamplerState`, `GetSamplerState` с валидацией значений. Состояние сохраняется на устройстве и передаётся backend при каждом draw:

| Группа | Поддерживаемые состояния |
|---|---|
| Depth | ZENABLE, ZWRITEENABLE, ZFUNC (PICA reverse-depth mapping) |
| Raster | CULLMODE, COLORWRITEENABLE |
| Alpha | ALPHATESTENABLE, ALPHAREF, ALPHAFUNC, ALPHABLENDENABLE, SRCBLEND, DESTBLEND, BLENDOP |
| Sampler 0 | ADDRESSU/V (wrap, mirror, clamp), MAGFILTER/MINFILTER (point, linear), MIPFILTER (none, point, linear), MAXMIPLEVEL |

Citro3D backend применяет depth/alpha/blend/cull и texture sampler params. Аппаратные результаты новых состояний ещё не проверены на 3DS; CPU тесты проверяют именно корректность параметров, выделения ресурсов и вызовов upload/draw. Значения по умолчанию теперь соответствуют D3D9 (WRAP, POINT min/mag, CULL_CCW, alpha LESS). Другие sampler stages, anisotropic, stencil, fog и отдельный alpha blend пока не поддержаны.

Проверено на ПК: неполная/полная mip chain для прямоугольных BGRA и DXT, pitch и размеры CPU tail, частичный BC rect, сохранение данных, пропуск GPU-выгрузки tail/READONLY, блокировка семплирования незагруженных mip, передача render/sampler states через COM, ASan/UBSan. Тесты собираются как в v0.8 ниже. Новый режим на реальной 3DS пока не проверен.

## v0.8 — BC-текстуры и первые адаптивные шаблоны шейдеров

Сборка на Windows как в v0.7:

```sh
make GRAPHICS_COM=1 LIFT_STAGE=staged
```

На нижнем экране ожидаются `IAT / COM / upload: PASS`, `BC textures: PASS` и `SM2 templates: PASS`.

- **A** — последовательно переключает BGRA8 / DXT1 / DXT3 / DXT5.
- **X** — переключает fixed-function путь и два тестовых шейдера SM2.0.
- START — выход.

### Текстуры

`d3d9_bc.c` содержит декодеры BC1 (DXT1), BC2 (DXT3), BC3 (DXT5) с RGB565-палитрой и прозрачностью (DXT1 1-bit alpha, DXT3 4-bit alpha, оба режима интерполяции DXT5). Для PICA200 преобразует 4×4-блоки непосредственно в 8×8 tiled ABGR, без полного промежуточного линейного RGBA-буфера. GPU всё равно хранит распакованное RGBA8; это не аппаратное BC-сжатие. Гостевой texture object хранит исходное сжатое представление, `CheckDeviceFormat`, `GetLevelDesc` и `LockRect` возвращают DXT-формат и правильный block pitch.

| Формат | CPU payload (64×64) | Pitch LockRect | GPU payload |
|---|---:|---:|---:|
| BGRA8 | 16 KiB | 256 B | 16 KiB |
| DXT1 | 2 KiB | 128 B | 16 KiB |
| DXT3/DXT5 | 4 KiB | 256 B | 16 KiB |

В v0.8 поддерживался только 2D level 0. В v0.9 добавлены mipmaps, частичные rect и отдельные blend/alpha-test states, как описано выше. Базовые размеры остаются степенями двойки 8..1024; декодирование происходит при UnlockRect.

### Шейдеры

`d3d9_shader.c` — bounded scanner токенов SM2/SM3, распознаёт два точных тестовых шаблона без runtime-компиляции произвольного шейдера. При создании COM shader object разрешены:

- `vs_2_0`: `dcl_position v0; dcl_texcoord v1; m4x4 oPos,v0,c0; mov oT0,v1` — константы c0..c3 задают clip transform, который преобразуется в существующую PICA-программу.
- `ps_2_0`: `dcl t0.xy; dcl_2d s0; texld r0,t0,s0; mov oC0,r0` — выборка одной текстуры через TEV.

Добавлены `CreateVertexShader`, `SetVertexShader`, `GetVertexShader`, `Set/GetVertexShaderConstantF` (только c0..c3), `Create/Set/GetPixelShader`, `IDirect3DVertexShader9` и `IDirect3DPixelShader9` (`IUnknown`, `GetDevice`, `GetFunction`). Другие токены и шейдеры Skyrim пока не переводятся: неизвестный шаблон возвращает `D3DERR_NOTAVAILABLE` и диагностирует версию, число инструкций, хеш и несовпадение. Framing scan не является полной валидацией произвольного shader bytecode. Это первый шаг, не поддержка реальных игровых шейдеров.

### Проверка на ПК

```sh
cc -std=c11 -O2 -Isource source/d3d9_core.c source/d3d9_texcache.c source/d3d9_texture_data.c source/d3d9_bc.c source/d3d9_vertex.c source/d3d9_shader.c tests/bc_shader_smoke.c -lm -o bc_shader_smoke
./bc_shader_smoke
```

Отдельно тест COM проверяет создание и рисование всех DXT-форматов, CreateShader/SetShader, сохранение исходного байткода и совпадение матриц fixed-function/VS-шаблона. Тесты прошли на ПК (включая ASan/UBSan). В архиве `preview_bc.png` — CPU reference render с DXT5. Сборка и запуск новой версии на 3DS пока не проверены.

## v0.7 — гостевой D3D9 COM → Citro3D

Новый деморежим рисует тот же куб, но вся работа с ресурсами и кадром проходит через 32-битные гостевые COM-объекты, а не прямые вызовы C backend. В тестовой области гостевой памяти создаётся PE IAT для `d3d9.dll!Direct3DCreate9`, затем x86-подобный C-диспетчер передаёт параметры через стек (`stdcall`) и вызывает методы через `rt_call_ic`. Вызываются также GetModuleHandleA/GetProcAddress. Успешные HRESULT и восстановление стека проверяются после вызовов.

### Сборка на Windows

Если папка `staged` уже создана для предыдущих тестов, используйте её повторно (в v0.7 генератор не менялся). Если нет, подготовьте из оригинального репозитория:

```sh
git clone https://github.com/ttom795/Skyrim_Android ../Skyrim_Android
python tools/stage_upstream.py ../Skyrim_Android staged
make GRAPHICS_COM=1 LIFT_STAGE=staged
```

Ожидаемый результат: `Skyrim_N3DS_graphics_com.3dsx`. Нужны devkitPro/Citro3D, как в v0.6. Никаких файлов игры и Capstone/pefile для этого теста не требуется. На нижнем экране ожидается `IAT / COM / upload: PASS`, на верхнем — вращающийся куб, START — выход. Сборка `make GRAPHICS=1` без COM остаётся доступной.

### Реализованное подмножество COM (правильные номера слотов D3D9)

| Интерфейс | Методы |
|---|---|
| Все объекты | QueryInterface (IUnknown, свой интерфейс и соответствующие базовые интерфейсы), AddRef, Release |
| IDirect3D9 | GetAdapterCount, GetAdapterDisplayMode, CreateDevice |
| IDirect3DDevice9 | TestCooperativeLevel, GetDirect3D, CreateVertexBuffer, CreateIndexBuffer, CreateTexture, BeginScene, EndScene, Clear, Present, SetTransform, SetFVF/GetFVF, SetStreamSource, SetIndices, SetTexture, DrawPrimitive, DrawIndexedPrimitive |
| IDirect3DVertexBuffer9 / IDirect3DIndexBuffer9 | GetDevice, Lock/Unlock, GetType, GetDesc |
| IDirect3DTexture9 | GetDevice, GetType, GetLevelCount, GetLevelDesc, LockRect/UnlockRect |

`d3d9_com.c` хранит реальные гостевые адреса объектов и vtable (не host pointers); Lock/LockRect выделяет временный гостевой staging-диапазон, Unlock копирует изменения в графическое ядро и освобождает staging. В режиме READONLY изменения не переносятся. Привязанные ресурсы удерживают COM-ссылки, освобождение ресурсов при активном кадре откладывается до Present. Поддерживается один статический device, одна texture stage и ограниченный набор форматов; Lock во время кадра и произвольные FVF пока не реализованы. В v0.8 добавлено ограниченное подмножество shader objects, в v0.9 — mipmaps, rect и часть states, в v0.10 — declarations и несколько streams. Неподдержанный слот выдаёт диагностический trap, а неподдержанный параметр — HRESULT, а не фиктивный успех.

Интеграция с runtime для будущего игрового loader: `d9_com_init(&session.memory, device)` перед подготовкой PE, добавить `d9_com_module()` к массиву `FnModule` и передать его в `n3ds_boot_prepare`. Мост автоматически подключает `d3d9.dll` к `win32_resolve`; `d9_com_shutdown` вызывается перед уничтожением `D9Device` и памяти. Сессия должна оставаться живой на протяжении вызовов. Настоящие Skyrim-таблицы функций и остальная часть D3D9 пока отсутствуют.

### Тест на ПК

```sh
cc -std=gnu11 -O2 -fwrapv -fno-strict-aliasing -Istaged -Isource source/guest.c source/guest_heap.c source/pe_imports.c source/win32_shims.c source/n3ds_lift_runtime.c staged/fpu.c source/d3d9_core.c source/d3d9_texcache.c source/d3d9_texture_data.c source/d3d9_bc.c source/d3d9_vertex.c source/d3d9_shader.c source/d3d9_demo_scene.c source/d3d9_com.c source/d3d9_com_demo.c tests/com_smoke.c -lm -o com_smoke
./com_smoke
```

Проверено на ПК: IAT, динамическое разрешение D3D9, косвенный вызов, COM vtable, GUID/счётчики ссылок, staging resource upload, partial/read-only locks, смена текстур и отложенное Release, оба режима рисования и Present. Преднамеренный trap неподдержанного ProcessVertices — часть теста. Тесты также прошли с ASan/UBSan. Реальная сборка `.3dsx` v0.10 ещё не проверена; пользователь подтвердил работу прямого графического демо v0.6 на New 3DS.

## v0.6 — графический backend

Добавлен экспериментальный fixed-function графический backend D3D9 → Citro3D/PICA200. Это базовый прямой демонстрационный режим; в v0.7 добавлен отдельный COM-режим (см. выше).

На Windows в devkitPro/MSYS2 (нужны `3ds-dev`, Citro3D и `picasso`) из папки проекта:

```sh
make GRAPHICS=1
```

Ожидаемый результат — `Skyrim_N3DS_graphics.3dsx`. Скопируйте его в `/3ds/Skyrim_N3DS_graphics/` на SD. Демонстрация рисует вращающийся текстурированный куб на верхнем экране и показывает FPS на нижнем. START — выход. Игровые файлы для этой демонстрации не нужны. Обычная команда `make` по-прежнему собирает предыдущую диагностику.

### Реализовано

- `d3d9_core.c`: создание устройства, вершинных/индексных буферов и текстур, lock/unlock, ограничение ресурсов, SetStreamSource, SetIndices, SetTexture, SetTransform, DrawPrimitive, DrawIndexedPrimitive и Present. Поддерживается один поток вершин `XYZ + TEX1`, 16-битные индексы, triangle list, текстуры A8R8G8B8/X8R8G8B8 (степень двойки 8–1024), level 0.
- `d3d9_citro3d.c`: верхний экран PICA200, render target, глубина, GPU-текстуры, линейный буфер вершин, загрузка данных, uniform матриц и команды рисования Citro3D. `d9shader.v.pica` — собственный простой вершинный шейдер.
- Преобразование матриц D3D (row-vector, depth 0..1) в формат PICA с поправкой на повернутый экран, BGRA → ABGR и 8×8 Morton texture tiling. Объём staging-ресурсов ограничен; индексные треугольники пока разворачиваются на CPU.
- `d3d9_demo_scene.c`: куб из 24 вершин и 36 индексов с собственной процедурной текстурой.

### Тесты на ПК

```sh
cc -std=c11 -O2 -Isource source/d3d9_core.c source/d3d9_texcache.c source/d3d9_texture_data.c source/d3d9_bc.c source/d3d9_vertex.c source/d3d9_demo_scene.c tests/graphics_smoke.c -lm -o graphics_smoke
./graphics_smoke
cc -std=c11 -O2 -Isource source/d3d9_core.c source/d3d9_texcache.c source/d3d9_texture_data.c source/d3d9_bc.c source/d3d9_vertex.c source/d3d9_demo_scene.c tests/graphics_render.c -lm -o graphics_render
./graphics_render preview.ppm
```

Тесты проверяют преобразования, раскладку пикселей, оба вида рисования, память/ошибки ресурсов. Дополнительный CPU reference rasterizer строит изображение того же куба с z-buffer и перспективной интерполяцией UV; сделанный на ПК `preview.png` находится в архиве. Эти тесты прошли, включая ASan/UBSan графического ядра. Это не скриншот 3DS: компиляция PICA-шейдера, сборка и запуск на реальной консоли пока не проверены.

Ограничения: отсутствуют общий D3D9 programmable shader translator, расширенная обработка vertex semantics, RT texture, autogen mipmaps, расширенные blend/fog/stencil states и прочие возможности Skyrim. Начальный backend не является заменой DXVK для полной игры.

## Сборка

1. Установите [devkitPro](https://devkitpro.org/wiki/Getting_Started) с пакетом `3ds-dev` (devkitARM, libctru, 3ds tools).
2. В терминале devkitPro/MSYS2 или Linux выполните `make` из этой папки. Например, на Windows `cd /c/Users/YourName/Downloads/Skyrim_N3DS` (замените путь на свой). Ожидаемый результат — `Skyrim_N3DS.3dsx`.
3. Скопируйте его на SD в `/3ds/Skyrim_N3DS/` и запустите через Homebrew Launcher.
4. Опционально положите свои `TESV.exe`, `TESV_Unpacked.exe`, `d3dx9_42.dll`, `binkw32.dll`, `X3DAudio1_7.dll` в `/Skyrim/` на SD. При старте проверяются заголовки. Кнопка A загружает распакованный TESV и три DLL как гостевые данные, затем показывает число разрешимых импортов из всех загруженных модулей. В сборке с `LIFT_STAGE` также учитываются реализованные Win32-шимы. Это сухой аудит: оригинальные x86-инструкции не исполняются, IAT файлов игры не изменяется. Для одного теста заголовков распакованный TESV не обязателен.
5. START — выход. Журнал `/Skyrim_N3DS.log` на SD содержит результаты запуска и всех попыток загрузки (перезаписывается при каждом запуске).

## Что проверяется

- Модель New 3DS, размер указателя и доступная область памяти libctru.
- `guest.c`: разреженная гостевая память, диапазоны, лимит выделения, проверки переполнения и little-endian доступ к 32-битным значениям. Доступ через границу региона пока не поддерживается.
- `pe_image.c`: чтение PE32 с SD, загрузка секций сразу в гостевой буфер, обнуление BSS, перенос i386 HIGHLOW, возврат ошибок и освобождение памяти. Сохраняются RVA и размеры таблиц импорта/экспорта/TLS; данные не исполняются. Это не проверка точной версии игры.
- `pe_imports.c`: обход импортов по имени/ординалу, поиск экспортов, связывание модулей с Win32-функциями и запись IAT только после разрешения всех импортов одного модуля. Forwarded exports пока не поддержаны.
- `guest_process.c`: минимальные PEB/TEB, стек, списки загруженных модулей, ANSI/UTF-16 командная строка и окружение, копирование шаблонов PE TLS с zero fill и заполнение индексов.
- `guest_heap.c`: пробная однопоточная куча с alloc/free/realloc, выравниванием и слиянием свободных блоков.
- `guest_call.c`: экспериментальная таблица адресов и вызовы C-функций через стек x86 (cdecl/stdcall). Тест использует собственные простые функции, не код Skyrim.
- Лимит отображения модулей — до 48 МиБ (меньше, если свободная системная область мала); это диагностический лимит, не гарантированный доступный размер heap.

## Тест на ПК

`cc -std=c11 -Wall -Wextra -Werror -Isource source/probe.c source/guest.c source/pe_image.c source/guest_heap.c source/guest_call.c tests/host.c -o host_test && ./host_test`

Тест создаёт временный синтетический PE-файл без исполняемого кода `test_fixture.pe` в текущей папке и удаляет его после успеха. Запускайте в рабочей папке, где такого файла нет. При наличии файла можно выполнить `./host_test /path/to/TESV.exe` для проверки заголовка. На ПК пройдены тесты загрузки, перемещения адресов, ограничений памяти, кучи и таблицы вызовов. Проверка AddressSanitizer/UBSan также пройдена с отключённым LeakSanitizer (не работает в тестовой среде). Сборка ARM и запуск на консоли пока не проверялись.

## Новый этап: адаптация реального lift.py

В `tools/stage_upstream.py` добавлены преобразования исходного `Skyrim_Android`: скалярные обращения к гостевой памяти, чтение/запись XMM, строковое копирование, x87/FPU, однопоточные операции LOCK/CAS. Скрипт проверяет количество ожидаемых участков, прекращает работу при несовпадении и создаёт отдельную папку. Оригинальный checkout не изменяется. Исходники upstream не включены в архив.

В `source/n3ds_lift_runtime.c` реализованы CPU-инициализация, несколько таблиц функций с двоичным поиском, косвенный вызов с кэшем, передача аргументов через гостевой стек, диспетчеризация Win32-шимов, проверка результата и виртуальный CPUID (профиль upstream). Явные ошибки гостевой памяти и неподдержанные операции возвращаются в host через отдельную границу вызова, а `ExitProcess` возвращает код выхода. Это не обработчик произвольных ARM-исключений. Гостевые `setjmp`/SEH, многопоточность и большая часть Win32 API пока не поддержаны; это не полноценный runtime Skyrim.

### Win32 и CRT

`source/win32_shims.c` связывает известные вызовы с фиксированными виртуальными адресами в диапазоне `0xff000000`. Это адреса C-функций в гостевой таблице, не реальные адреса ARM.

| Группа | Реализованное подмножество |
|---|---|
| Heap | GetProcessHeap, HeapAlloc/Free/Size/ReAlloc |
| Virtual memory | VirtualAlloc/VirtualFree, только простые RW-области, без Windows reserve/commit semantics или защиты страниц |
| Time | GetTickCount, QueryPerformanceCounter/Frequency, Sleep, GetSystemTimeAsFileTime |
| Thread | GetCurrentThread/Process (+Id), динамические TlsAlloc/Free/GetValue/SetValue до 576 слотов (в TEB), Interlocked* в одном гостевом потоке |
| File | CreateFileA, ReadFile, SetFilePointer, GetFileSize, CloseHandle, только синхронное чтение относительных путей внутри Skyrim/; без создания/записи файлов, wide paths и неполной семантики Win32 |
| CRT | malloc/calloc/realloc/free, memcpy/memmove/memset/memcmp, strlen/strcmp/strcpy/_stricmp/_strdup |
| Other | GetLastError/SetLastError, GetSystemInfo, lstrlenA, lstrcmpiA, OutputDebugStringA |
| Process | GetCommandLineA/W, GetEnvironmentStringsA/W, FreeEnvironmentStringsA/W, GetStartupInfoA, ExitProcess |
| Modules | GetModuleHandleA, LoadLibraryA (только уже загруженные модули), GetProcAddress по имени/ординалу, GetModuleFileNameA, FreeLibrary (статические модули остаются в памяти) |

Тест `tests/win32_smoke.c` использует синтетические PE-таблицы, проверяет импорт по имени и ординалу, отсутствие частичных изменений при нерешённом импорте, связывание IAT, вызовы Win32 из гостевого стека и часть функций CRT. На ПК создаётся собственный временный `shim_fixture.bin` (не перезаписывает имеющийся файл) для проверки файлового чтения. На консоли тест ограничен памятью и диагностическими вызовами, без записи тестовых файлов. В режиме A неизвестные импорты записываются в `/Skyrim_N3DS.log` (до 256 на модуль).

### Проверка совместимости на Windows

В devkitPro/MSYS2 из папки проекта (желательно без пробелов в путях):

```sh
git clone https://github.com/ttom795/Skyrim_Android ../Skyrim_Android
python tools/stage_upstream.py ../Skyrim_Android staged
make LIFT_STAGE=staged
```

Должен получиться `Skyrim_N3DS_lift.3dsx`. Он дополнительно проверяет C-функции, написанные в стиле генератора, используя реальные адаптированные заголовки CPU/FPU из upstream. Повторная подготовка требует новой папки. Обычная команда `make` сохраняет базовую диагностику.

Если есть host GCC (`cc`), тот же тест можно запустить на ПК:

```sh
cc -std=gnu11 -O2 -fwrapv -fno-strict-aliasing -Istaged -Isource source/guest.c source/guest_heap.c source/pe_imports.c source/win32_shims.c source/n3ds_lift_runtime.c staged/fpu.c tests/lift_smoke.c tests/win32_smoke.c -lm -o lift_smoke
./lift_smoke
```

### Тест реальной генерации C из x86

Никаких игровых файлов не требуется; используется только маленькая собственная программа, складывающая числа и меняющая значение в гостевой памяти. В терминале с Python:

```sh
python -m pip install capstone==5.0.9 pefile==2024.8.26
python tools/make_lift_fixture.py fixture
LIFT_FILES=1 python staged/lift.py fixture/fixture.exe fixture/functions.json gen_fixture fixture
make LIFT_STAGE=staged LIFT_GEN=gen_fixture
```

Ожидаемый результат — `Skyrim_N3DS_generated.3dsx`, где тест вызывает два действительно сгенерированных блока. `fixture.exe` предназначен исключительно для рекомпиляции, запускать его как приложение Windows не нужно.

**Проверено при разработке:** скрипт подготовки применён к актуальным файлам upstream, адаптированные заголовки и FPU успешно скомпилированы GCC; тест памяти, атомиков, FPU/SSE, косвенных вызовов, PE-импортов/экспортов и Win32-шимов проходил и с ASan/UBSan. Создание PE fixture проверено. **Не проверено:** выполнение самого lift.py с Capstone/pefile (пакеты не доступны в среде), линковка `.3dsx`, реальные Skyrim-файлы и консоль. Поэтому готовность второго сценария требует проверки.

## Следующий этап

### Новый этап: подготовка игрового процесса

`n3ds_boot.c` предоставляет `BootSession`, загрузку файлов, предварительную проверку импортов, инициализацию процесса и стеков, связывание таблиц C-функций и проверку наличия всех точек входа и TLS callbacks. Если всё готово, `n3ds_boot_run` последовательно вызывает TLS и DllMain каждого DLL (в порядке, заданном вызывающей стороной), TLS executable и его точку входа. Если любой импорт отсутствует, подготовка останавливается. При ошибке используйте `n3ds_boot_destroy` для освобождения ресурсов; за один запуск приложения допускается только одна связанная runtime-сессия. DLL detach пока не реализован.

`n3ds_boot_prepare_skyrim` загружает четыре файла Skyrim из указанной папки с адресами из upstream. Это интерфейс для будущих реальных таблиц функций: сам по себе он Skyrim не запускает. В текущем архиве нет сгенерированного кода TESV и DLL, и ещё не перенесены специальные host overrides оригинального порта.

Для запуска синтетического теста на консоли после подготовки `staged` из раздела выше:

```sh
make LIFT_STAGE=staged BOOT_SMOKE=1
```

Результат — ожидаемый `Skyrim_N3DS_boot.3dsx`, который проверяет цепочку загрузки на двух искусственных модулях с написанными вручную C-функциями. При успешном тесте отображается `Lift ABI: PASS`. Кнопка A по-прежнему выполняет сухой аудит игровых файлов, дополнительно пытаясь создать процесс без запуска guest-кода.

Проверка на ПК из папки проекта:

```sh
cc -std=gnu11 -O2 -fwrapv -fno-strict-aliasing -Istaged -Isource source/guest.c source/guest_heap.c source/pe_image.c source/pe_imports.c source/guest_process.c source/n3ds_boot.c source/win32_shims.c source/n3ds_lift_runtime.c staged/fpu.c tests/boot_smoke.c -lm -o boot_smoke
./boot_smoke
```

Ожидаемый вывод включает намеренную ошибку `Guest fault at deadbeef: unmapped guest memory`, затем сообщение об успешном тесте PEB/TEB/TLS, IAT, DLL attach, entry и ExitProcess. Это проверка контролируемой остановки, а не сбой самого теста. Дополнительно `./boot_smoke fixture/fixture.exe` проверяет загрузку PE-файла, созданного `make_lift_fixture.py`.

Проверено на ПК: полный bootstrap синтетического процесса, включая статический TLS двух модулей, порядок вызовов, динамические TLS slots, GetProcAddress, PEB/TEB, код завершения 42, отказ на отсутствующем импорте, возврат при unmapped address, ASan/UBSan. Под ARMv6 и на 3DS пока не проверено.

### Дальнейшие задачи

1. Расширить Win32-шимы по результатам аудита реальных игровых файлов и перенести специальные host overrides upstream; связать подготовленный loader с фактическими таблицами сгенерированных функций; пересмотреть бюджет памяти и стека.
2. Регенерировать код из собственной Skyrim 2011 1.9.32.0.8 и собрать маленький набор функций под ARMv6K, проверить его результаты на ПК и консоли. Не приступать сразу к сборке полного игрового EXE: размер кода и расход памяти могут превысить возможности консоли.
3. Замерить размеры машинного кода и фактическую память. Не переходить к загрузке игры, пока бюджет памяти не подтверждён.
4. Реализовать необходимые платформенные методы и экспериментальный D3D9 backend на Citro3D. DXVK/Vulkan здесь неприменимы.

Реальная производительность и возможность запуска игры пока неизвестны.
