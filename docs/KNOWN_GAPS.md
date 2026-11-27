# KNOWN_GAPS — сводка известных пробелов по существующим отчётам

Дата: 2026-11-27. Ветка `gaps-doc`, база `31d58e6`.

Настоящий файл — только сводка **уже написанных** отчётов. Источники:
`STATUS_2026-11-27.md`, `MENU_SHADER_RUNTIME.md`, `WORLD_SHADER_CLOSURE.md` (§2d–2i),
`MEMORY_BUDGET_178.md`, `THREADS.md`, `AUDIO.md`, `TEXTURE_FORMATS.md`,
`SAVE_LOADER.md`, `PAPYRUS_AUDIT.md`, `HAVOK_AUDIT.md`, `MENU_CRASH_HANDOFF.md`.

Правила:

* каждая строка содержит ссылку на файл и раздел; вывода сверх прочитанного нет;
* колонка «Размер» заполняется **только** там, где сам отчёт дал оценку (`M` — полдня, `L` — 1–2 дня). Прочерк «—» означает «отчёт не оценивал», а не «мало»;
* «Кто/что нужно» — действие и компонент; владелец указывается, только если он прямо назван в источнике;
* `DevCaps` (`0x00082000`) нигде не предлагается менять; `F:` не используется.

## 1. Меню: загрузка, IO, кадр

| Пробел | Доказательство / где | Кто / что нужно | Размер |
|---|---|---|---|
| Загрузка меню встаёт: main блокируется на семафоре `0x50c`, единственный runnable-поток 9 сидит в длинном цикле `[io] read` на 64 KiB; watchdog прерывает прогон, который ещё может прогрессировать | `STATUS_2026-11-27.md` §Open blockers #1 | Рабочее дерево `menu-io-stall`: телеметрия IO и watchdog, считающий прогресс IO/PC | — |
| Первый GFx-draw меню не разрешён в runtime-программу PICA | `STATUS_2026-11-27.md` §Open blockers #2; `MENU_SHADER_RUNTIME.md` заголовок и §3 | Рабочее дерево `world-runtime-paths`: `N3DS_SHADER_DRAW_TRACE` и gate, который падает, если план есть, а программа `none`; см. также `WORLD_SHADER_CLOSURE.md` §2f | — |
| Нет отрисованного кадра меню / pixel proof | `STATUS_2026-11-27.md` §Where the real exe is now (BeginScene=4, Present=1, DrawPrimitive=8 — только фон fixed-function, «no GFx menu draw yet, and no frame is claimed»); `MENU_SHADER_RUNTIME.md` §3 | Реальный прогон с GFx-draw и capture; не подменять Present от Bink-интро | — |
| Capture первого draw не записывает id селектора | `MENU_SHADER_RUNTIME.md` §3, «Capture audit of the entry id» | Добавить id в first-draw capture | — |

## 2. Меню: шейдеры

| Пробел | Доказательство / где | Кто / что нужно | Размер |
|---|---|---|---|
| Нет VS-исполнения: фиксированный PICA VS даёт только position+UV, не produce `color0`/`color1` и лишние texcoord; все члены семейства выбираются, но draw refuses by name | `MENU_SHADER_RUNTIME.md` §1.1, §3; `STATUS_2026-11-27.md` §Open blockers #3 | Бэкенд: `color0`/`color1` varying и последние texcoord; глобальные caps не трогать | — |
| Логотип refusal: `normal_prebaked`, `tangent_basis_prebaked`, `ambient_emissive_varying` не выводятся из текстур/декларации, их должен сообщить загрузчик материала | `MENU_SHADER_RUNTIME.md` §1.3, §3 | NIF/material/preprocessing loader вызывает `d9_menu_shader_note_preconditions()` | — |
| residual VS: 46 `tangent0`/`binormal0` selected, но draw refused (clip position через temp), плюс 37 skinning, 92 `nrm`, 10 `lrp`, 20 структурных — именованные refusals | `MENU_SHADER_RUNTIME.md` §1.6; `STATUS_2026-11-27.md` §Open blockers #3 | Для первого GFx-draw не нужны (это мировые/эффектные шейдеры, загруженные вместе с меню); оставить именованными refusals | — |
| ~6.6k оставшихся уникальных bytecode-блобов отклоняются, пока closure вооружён; generic SM3-трансляции и fallback нет | `MENU_SHADER_RUNTIME.md` §3, «Other measured blobs» | Решение о политике; не обход closure | — |
| Точная семантика retail fog/tint неизвестна: `.fxp` для `BSEffectShaderProperty` нет, планы — явные контракты (explicit contract), не bytecode-совпадение | `MENU_SHADER_RUNTIME.md` §3, «Exact retail fog/tint stage semantics» | Не выдавать за bytecode-match; при необходимости найти/reverse-инженерить контракт | — |

## 3. Меню: offscreen render targets

| Пробел | Доказательство / где | Кто / что нужно | Размер |
|---|---|---|---|
| GFx запрашивает NPOT render-target textures (ровно 3 отказа `CreateTexture: render-target textures must be power-of-two 8..1024` в старых логах); NPOT-логический target теперь реализован POT-хранилищем + sub-rect scaling | `MENU_SHADER_RUNTIME.md` §1.7; `STATUS_2026-11-27.md` §Update (late evening); `docs/NPOT_RENDER_TARGETS.md` | Сделано `b5abe8f`: `pot_pad`/`storage_w/h` в `source/d3d9_core.c`, UV-масштаб и sub-rect readback. Противоречие №1 снято | — |

## 4. Мир: шейдеры

| Пробел | Доказательство / где | Кто / что нужно | Размер |
|---|---|---|---|
| Из 691 world-плана runtime-путь имеют только 18; 673 остаются offline | `WORLD_SHADER_CLOSURE.md` §1 (таблица per-pack и итог); `STATUS_2026-11-27.md` §Open blockers #4 | Очередь по частоте — `WORLD_SHADER_CLOSURE.md` §3; не повышать счётчики без артефакта | — |
| `terrain`: 138 TEV-планов draw 0; 109 хэшей без плана (`needs_multipass` 103, `stage_budget` 6) | `WORLD_SHADER_CLOSURE.md` §5 | Multipass / stage budget | — |
| `sky_water`: все 45 TEV-планов draw 0, пак catalog-only без общего runtime-слота | `WORLD_SHADER_CLOSURE.md` §5 | Выделить слот или оставить честно offline | — |
| `lighting`: 123/130 PICA VS emitted-verified, но вне 7-слотового каталога; 107 TEV offline; 4 754 хэша без плана, в т.ч. 22 VS с 8–9 varying | `WORLD_SHADER_CLOSURE.md` §5 | Много-texcoord loader + PS-ппара, не новый ALU-опкод | — |
| `skinning`: 104/108 PICA VS вне 4-слотового каталога; 94 TEV ждут material contract; 992 хэша без плана | `WORLD_SHADER_CLOSURE.md` §5 | Material contract и слоты каталога | — |
| `blit_post`: 59 TEV offline; два VS (`4d155cd6`, `c43418bb`) написаны и численно проверены, но не promoted | `WORLD_SHADER_CLOSURE.md` §2, §5 | Шире varying loader (usage index >0) + спаренный PS-контракт | — |
| 15 «loader-ready» wide VS: 0 promotable (`ps_reads_color1` 6, `vs_attr_source_unsupported` 4, `ps_not_runtime` 3, `no_ps_partner` 2) | `WORLD_SHADER_CLOSURE.md` §2c | См. COLOR1 и источник tangent/binormal ниже; `D9SkinVertex` не имеет поля для TANGENT/BINORMAL | — |
| COLOR1 не имеет PICA TEV-носителя: один primary color, `map_source` без `FRAG_PRIMARY`; 6 кандидатов читают COLOR0+COLOR1, promoted 0 | `WORLD_SHADER_CLOSURE.md` §2g, §2c | Либо переписать спаренный PS-план без COLOR1 с измеренной ошибкой (`quality: approx`), либо оставить VS выключенным. **См. противоречие №3** | — |
| Translator-level `def` fold: `d3d9_pica_tev_preflight_pixel` отвергает `def` вне RGBA8 раньше плана; добавление строки даст fake path | `WORLD_SHADER_CLOSURE.md` §2e | `d3d9_pica_tev.c` + `lighting_plans.py`: заменить lane на cN, `constant_ops`, согласовать N и op, добавить сэмплы | **M** (полдня) |
| Один `def` fold не снимает блок 9 refused 10-chunk PS: они падают в planner раньше (`nrm`, `rcp`, `lrp`), `ignore_def_range=True` плана не даёт | `WORLD_SHADER_CLOSURE.md` §2g | Сначала opcode lowering (`nrm/rcp/lrp`), затем translated-stage→plan serializer и literal `constant_ops` | — |
| PS с зависимым чтением (`06aa117c`/`14db34d7`, 16 chunks каждый, 32 chunks всего): TEV не может писать текстурную координату, двух-pass RT не решает | `WORLD_SHADER_CLOSURE.md` §2d; `STATUS_2026-11-27.md` §Open blockers #4 | CPU/software fallback для двух PS: tile 64×64 RGBA8, readback, SM3 в хостовом интерпретаторе, upload; за frame-budget flag, измерить readback до claims | **L** (1–2 дня) |
| Восемь 10-chunk lighting PS даже с approximation упираются в текстурный бюджет: исходник сэмплирует 4–7 разных sampler при 2 единицах в `d3d9_pica_tev` | `WORLD_SHADER_CLOSURE.md` §2h, §2i | Multi-pass texture splitting: ≤2 unit на pass + аккумулятор; нужны per-pass texture handles в `D9ShaderPlanPass`, биндинг в core и COM-хранение stage >1 (`d3d9_core.c` принадлежит другому агенту, §2i) | **L** |
| Двухтекстурная редукция — другой шейдер: лучший 2-texture plan даёт max err 0.94–0.97 при пороге 0.10; promotion 0 из 8 | `WORLD_SHADER_CLOSURE.md` §2i | Не promoting dominant-texture fallback; измерить fill-rate 2–4 pass (~384k px на 240×400) | — |
| `d3d9_core` отправляет любой PICA VS через `draw_skinned()` → `fetch_skin()`, которая требует BLENDWEIGHT0/BLENDINDICES0; 13 из 17 world VS резолвятся на gate, но core отказывает до submission | `WORLD_SHADER_CLOSURE.md` §2f | Нескинный fetch/pack путь в `d3d9_core.c`; без этого «runtime path» не является draw | — |
| `d9_pica_vs_declare` связывает только usage index 0; 15 emitted-verified VS читают TEXCOORD1..4 и ждут envelope + PS-пары | `WORLD_SHADER_CLOSURE.md` §2b | Расширение loader + сертификация пар (запись в `wide_vs_certification.json`) | — |

## 5. Память

| Пробел | Доказательство / где | Кто / что нужно | Размер |
|---|---|---|---|
| На устройстве первыми свяжут 92 MiB guest-бюджет и 8/24 MiB linear heap; профиль 120/24 не доказан на железе | `STATUS_2026-11-27.md` §Memory; `MEMORY_BUDGET_178.md` §6, §8 | Прогон одного маршрута (cold start → меню → первая ячейка → сосед → возврат) на 96/8, 104/16, 120/24; принять 120/24 только по результату: оба heap созданы, ≥4 MiB newlib и ≥2 MiB linear free на пике | — |
| Нет `linearSpaceFree()`-учёта; логгируется только `vramSpaceFree()`. 4.35 MiB арен до первой текстуры съедает больше половины 8 MiB linear | `MEMORY_BUDGET_178.md` §5 | Счётчики `linear_live/peak/min_free/fail_*` в `mem_stats` и измерение high-water | — |
| Текстурного бюджета нет: `d9_texcache_init(...,0,0)` — `guest_limit=0, gpu_limit=0`; лимиты есть, но никто их не ставит и по умолчанию выключены | `MEMORY_BUDGET_178.md` §5, §8; `STATUS_2026-11-27.md` §Memory | `d9_texcache_set_limits()` + pin «texture used this frame»; PICA VRAM остаётся жёсткими 6 MiB | — |
| Cell-событие в `mem_stats` не подключено (`mem_stats_note_cell()`, `stream_cell` вне изменения); периодический snapshot пропускает пик, когда старый ассет, копия и upload buffer живы одновременно | `MEMORY_BUDGET_178.md` §8, §6 п.2 | Подключить cell-событие и сэмплировать в момент аллокации | — |
| Неизмеренные величины: реальный `linearAlloc` high-water в меню/мире, guest `used` high-water под реальной ячейкой, вызов `CreateFileMapping` на BSA-пути, нужен ли `osSetMemMode` для linear >8 MiB | `MEMORY_BUDGET_178.md` §9 | Измерения на устройстве | — |
| 512 MiB `AvailPageFile` gate: два маршрута описаны (paging provider / патч `0x00a48a67`), ни один не реализован | `MEMORY_BUDGET_178.md` §7 п.7 | Решение, а не задача | — |
| Хост-измерение на данный момент: `guest_peak` 129 MiB, `tex_gpu` 29 MiB, IR decode cache ~14.6 MiB, BSA resident 2.5 MiB (index 1.96 + page cache 0.41) | `STATUS_2026-11-27.md` §Memory; `MEMORY_BUDGET_178.md` §4 | Сравнить с пиками на устройстве; 42.12 MiB — порог gate, не working set | — |

## 6. Потоки

| Пробел | Доказательство / где | Кто / что нужно | Размер |
|---|---|---|---|
| Backend ARMv6K (`WIN32_HOSTCTX_BACKEND=2`) только compile-guarded, не запускался на железе | `THREADS.md` таблица backends | Прогон на 3DS; остальные два backend'а host-verified | — |
| Watchdog аварийно завершает прогон, который может ещё прогрессировать (связано с IO-stall меню) | `STATUS_2026-11-27.md` §Open blockers #1; `THREADS.md` §Starvation fix (signed-разность и диагностика) | Watchdog, считающий IO/PC-прогресс, в `menu-io-stall` | — |

## 7. Аудио

| Пробел | Доказательство / где | Кто / что нужно | Размер |
|---|---|---|---|
| XAudio2 недостижим из этого TESV.exe: нет импорта и строки `XAudio2Create`; `module_handle()` не знает `xaudio2_*.dll`/`xapofx1_5.dll`, поэтому `LoadLibraryA` даёт error 126 | `AUDIO.md` §1, §6 п.1 | Добавить имена в `win32_shims.c` `module_handle`/`module_name` — прямо отмечено как вне разрешённых файлов той задачи | — |
| WMA/XMA (`.xwm`/`.fuz`) — тишина с корректной длительностью, реального декода нет | `AUDIO.md` §2, §6 п.2 | Реальный декод | — |
| Нет слухового/hardware-доказательства: DSP-путь только ARM compile-only, хост — counting null sink | `AUDIO.md` заголовок, §4, §6 п.3 | Прогон на New 3DS / Azahar с захватом аудио: ndsp, питч 32730 Hz, underrun при stall потока | — |
| Эффекты/фильтры no-op, нет реверба/sends, нет 5.1 matrix, нет X3DAudio→XAudio2 интеграции | `AUDIO.md` §3 known differences, §6 п.4 | По конкретному вызывающему коду; сейчас S_OK без действия | — |

## 8. Текстуры и форматы

| Пробел | Доказательство / где | Кто / что нужно | Размер |
|---|---|---|---|
| Прямой `CreateTexture(R8G8B8/A8B8G8R8)` не реализован; ответ `D3DERR_NOTAVAILABLE`. Путь латентный: 15 измеренных menu-вызовов просят только A8/A8L8/A4R4G4B4/A8R8G8B8 | `TEXTURE_FORMATS.md` §2a, §6; `GFX_COMPAT.md` §3/§4 (как процитировано в `TEXTURE_FORMATS.md`) | Только если появится реальный вызов; грузчики D3DX уже расширяют до A8R8G8B8 | — |
| BC7 4×4 (`eye01_sk.dds`, 1 файл UBE) отклоняется по общему полу 8×8; padding сделан только для 1×1 *нескомпрессированного* cube | `TEXTURE_FORMATS.md` §6, §2c | Специальный путь для 4×4 BC7 | — |
| Настоящий float RT невозможен на PICA200; LDR fallback — measurement-only (`TESV_D3D9_HDR_LDR_FALLBACK`) | `TEXTURE_FORMATS.md` §5, §6 | Shader-side reformulation; владелец по источнику — renderer/shadow-map init (`FLOAT_RENDER_TARGETS.md` §5, процитировано в `TEXTURE_FORMATS.md` §5) | — |

## 9. Сохранения

| Пробел | Доказательство / где | Кто / что нужно | Размер |
|---|---|---|---|
| Profile builder пока не создаёт каталог `Saves`; без `.ess` Continue имеет пустой список | `SAVE_LOADER.md` §4 (ссылка на `SAVE_PATHS_AUDIT.md`) | Стейджить минимум одну реальную `.ess` и соответствующий `Plugins.txt` | — |
| papyrus-блок (global table 3, type 1001) до 5,313,831 B в `quicksave.ess` — давление на guest heap при загрузке | `SAVE_LOADER.md` §2, checklist п.8 | Измерить пик guest allocation до суждения о падении после Papyrus | — |
| Остаточные host-риски (`share > 3`, OVERLAPPED, `ReadFileEx`, 32-bit `SetFilePointer`, отсутствующий `FlushViewOfFile`) — не на save-пути | `SAVE_LOADER.md` §3, «Residual host risks» | Не трогать без трассы; host coverage read-path уже полный | — |

## 10. Papyrus

| Пробел | Доказательство / где | Кто / что нужно | Размер |
|---|---|---|---|
| `operator new`/`operator delete` (`??2@YAPAXI@Z`, `??3@YAXPAX@Z`) импортированы, не resolved; BSScript — C++ и аллоцирует объекты | `PAPYRUS_AUDIT.md` §4 п.1 | Нужно решение до кода: map на `heap_alloc`/`heap_free` или throwing-форма с оговоркой про отсутствие exceptions | — |
| `_snprintf`/`_vsnprintf`/`_vsnwprintf` и `sscanf` — гостевой `va_list` (32-bit cdecl, выравнивание double/int64, `%s` из guest memory) | `PAPYRUS_AUDIT.md` §4 п.2–3 | Guest-форматтер; нельзя передавать гостевую строку в host `vsnprintf` | — |
| CRT startup/SEH: `_initterm`, `_amsg_exit`, `_XcptFilter`, `__CxxFrameHandler` — явные traps | `PAPYRUS_AUDIT.md` §4 п.4 | Не подключать без модели SEH (иначе тихая обработка access violation); вместе с `_onexit`/`__dllonexit` | — |
| После аудита 524 resolved импортов, 116 неразрешённых имён (109 символов); `RegOpenKeyA` отсутствует | `PAPYRUS_AUDIT.md` §3 таблица, §2.3, §4 п.9 | `RegOpenKeyA` — в registry shim, который уже владеет остальным; остальные 116 — по приоритету | — |
| stdio FILE\* (`fread`/`fwrite`/`fflush`/`_fileno`/`_write`/`_iob`/`_lseeki64`), `wctomb`/`wcstombs`, `_strtime`/`_strdate`, x87 control (`_controlfp` и др.) | `PAPYRUS_AUDIT.md` §4 п.6–8, п.10 | По конкретному caller; `_controlfp` eventual map на `fpu_cw`; code-page решение для wchar | — |
| Минорный дефект `vtime_ticks()` (первый tick = 0 даёт `2*tick`) и `timeBeginPeriod` no-op → `Sleep(1)` может oversleep на один host tick | `PAPYRUS_AUDIT.md` §5 | Исправить с тестом; на Windows — refcounted реальные `timeBeginPeriod`/`timeEndPeriod` (нужен link flag) | — |

## 11. Havok

| Пробел | Доказательство / где | Кто / что нужно | Размер |
|---|---|---|---|
| 100 lifter-отказов вне SSE/x87: `push/pop es/ss/ds` 45, `junk enter` 13, `junk les` 9, `loopne/loope` 10, `int1` 7, BCD-опкоды 15, один segment override; в успешно поднятых функциях остаются traps `int`/`notrack call`/`clts` | `HAVOK_AUDIT.md` §4, §5 | Именованные traps, не silent skips; обработать по мере необходимости | — |
| `invalid.csv`/контейнер устарели (226 — историческое число до патчей); статическое зеркало patched decoder уже даёт 0 SSE/x87-отказов | `HAVOK_AUDIT.md` §5 | Перегенерировать 90,900-function container и повторить C-декодер — авторитетный следующий шаг | — |
| `tesv_slice` прогонялся на готовом hwnext2 slice, не на свежем Havok-aware; `Not enough memory` — диагностика, не PASS | `HAVOK_AUDIT.md` §4, §5 | Свежий срез и прогон | — |

## 12. Краш / discovery / сборка

| Пробел | Доказательство / где | Кто / что нужно | Размер |
|---|---|---|---|
| discovery-разрывы `0xb7f450` и `0xb779c0` (в `functions.json` не было реальных конструкторов) | `MENU_CRASH_HANDOFF.md` §Fixed 0x429872 (ещё как «next blocker»); `STATUS_2026-11-27.md` §Fixed on this path — уже исправлено, контейнер пересобран | Сверить статус: в STATUS закрыто; в handoff — устаревшая запись. **См. противоречие №8** | — |
| Блок `0x429872` длиннее 255 инструкций → `no terminator` | `MENU_CRASH_HANDOFF.md` §Fixed 0x429872; `STATUS_2026-11-27.md` §Fixed on this path — уже исправлено synthetic fall-through `jmp` | Закрыто; регрессия `tests/ir_long_block_smoke.c` | — |
| Корень краша `0x60aa70` — `rep movsd` по освобождённой host-памяти при cross-page `guest_ptr()` | `MENU_CRASH_HANDOFF.md` §Root cause found (2026-11-27); `STATUS_2026-11-27.md` §Fixed on this path | Закрыто `n3ds_rep_movsd` в `source/rep_movs.c`, регрессия `tests/rep_movs_slab_smoke.c` | — |
| Без локального `pilot_out_mult/` срез падает на `0x802fa80` | `MENU_CRASH_HANDOFF.md` §0802fa80; `STATUS_2026-11-27.md` §Fast loop | Артефакт сборки, **не** гостевой пробел: `fast_slice.ps1` уже предупреждает | — |

## Противоречия и расхождения между отчётами

Зафиксированы как факты; ни одна из сторон не выбрана в качестве «правильной».
Разрешения, внесённые 2026-11-27 в ветке `portlog-doc`, перечислены после таблицы
(сняты №1, №2 и №4).

| # | Расхождение | Сторона A | Сторона B |
|---|---|---|---|
| 1 | NPOT render targets | `STATUS_2026-11-27.md` §Fixed on this path: «NPOT render targets use POT storage with a logical sub-rect» — уже сделано | `MENU_SHADER_RUNTIME.md` §1.7: `create_target()` rejects NPOT, три запроса остаются `D3DERR_NOTAVAILABLE`, «That is not implemented in this change» |
| 2 | Enum normal/tangent/binormal | `STATUS_2026-11-27.md` §Open blockers #4 называет его следующим unlock'ом (9 plans) | `WORLD_SHADER_CLOSURE.md` §2c: enum уже есть (`D9_PICA_VS_ATTR_NORMAL/TANGENT/BINORMAL`, usage 3/6/7), старый blocker `vs_input_not_loadable` устранён; осталось 4 программы `vs_attr_source_unsupported` |
| 3 | COLOR1 | `STATUS_2026-11-27.md` §Open blockers #4: COLOR1 carrier на 4 плана | `WORLD_SHADER_CLOSURE.md` §2c: `ps_reads_color1` = 6 программ; §2g: все шесть читают COLOR0+COLOR1, promoted 0 |
| 4 | Глобальные счётчики планов | `MENU_SHADER_RUNTIME.md` §4.1: `runtime_audit.py` даёт 128 draw-enabled plans / 128 runtime paths / 30 валидных PICA VS | `WORLD_SHADER_CLOSURE.md` §1: global report 6,659 hashes / 6,542 translated / 95 draw-enabled / 22 refused; 18 world draws |
| 5 | Число dumped VS | `MENU_SHADER_RUNTIME.md` §1.6 говорит о 314 уникальных `vs_*.bin`, «were replayed» | Таблица в том же §1.6 даёт 5+46+37+92+10+20 = 210; 104 хэша в классах не учтены. Внутренняя несогласованность |
| 6 | Число заблокированных world VS | `WORLD_SHADER_CLOSURE.md` §2f: «13 of the 17 world VS rows» заблокированы skinned-fetch | Там же перечислено 6 blit + 6 lighting + 2 wide = 14; при этом identity-bank ровно у одной wide-строки (`lit.vs.b3a9babe`), вторая — 4-bone `skin.vs.8513aaac`. Внутренняя несогласованность |
| 7 | Достигнута ли StartMenu | `MENU_CRASH_HANDOFF.md` §Not achieved: «StartMenu/SWF open, 0x8779d0 type-0 dispatch, GFx Draw*: 0» | Тот же файл §Root cause found: прогон проходит дальше `0x5fc640`; `STATUS_2026-11-27.md` §Where the real exe is now: StartMenu create и Present достигнуты. Ранний раздел handoff устарел |
| 8 | `0xb7f450` | `MENU_CRASH_HANDOFF.md` §Fixed 0x429872: «Next independent blocker: 0xb7f450 unknown function/import», не в `functions.json` | `STATUS_2026-11-27.md` §Fixed on this path: discovery gaps `0xb7f450`, `0xb779c0` добавлены, контейнер пересобран. Handoff устарел |

### Разрешено 2026-11-27 (ветка `portlog-doc`)

* №1: `MENU_SHADER_RUNTIME.md` §1.7 приведено к факту — NPOT offscreen RT
  реализован (`b5abe8f`): `source/d3d9_core.c` (`pot_pad`, `storage_w/h`) и
  `docs/NPOT_RENDER_TARGETS.md`. Три `D3DERR_NOTAVAILABLE` в старых логах —
  исторические; `STATUS_2026-11-27.md` остаётся фактической стороной.
* №2: `WORLD_SHADER_CLOSURE.md` §2c сверено с `source/d3d9_pica_vs.h/.c`:
  attr ids 5/6/7, D3D usage 3/6/7; `D9SkinVertex` имеет `tx/bx`,
  `fetch_plain()` декодирует TANGENT0/BINORMAL0. Причина «нет поля» в
  сертификаторе устарела; 4 verdict'а в `wide_vs_certification.json` требуют
  повторного прогона — **требует проверки**.
* №4: `MENU_SHADER_RUNTIME.md` §4.1 и `WORLD_SHADER_CLOSURE.md` §1: 128 —
  меню-side обзор общих `docs` + pack catalogs; 18 — мир, каталог пяти мировых
  паков. Разные каталоги/области аудита, не две оценки одного множества.

## Уточнения, чтобы не принять за пробел

* `D3DXCompileShader` остаётся `E_NOTIMPL`, но трасса (`trace12.err`, процитирована в `MENU_SHADER_RUNTIME.md` §2) показывает только две `[import]`-привязки и **ни одного** вызова до SM3-gate; TESV грузит прекомпилированный FXP. Это не путь меню.
* `Present=1` в `STATUS_2026-11-27.md` не является кадром меню: GFx-draw нет, Bink-интро декодируется на хосте (там же, §Fixed on this path, Bink).
* BC5/ATI2 отсутствует и в рознице, и в установленных модах (`TEXTURE_FORMATS.md` §2); декодер есть, но не «проверен корпусом» — только синтетические reference blocks.
* Save host read-path уже покрыт полностью; `CreateFileMappingA`/`MapViewOfFile` в IAT TESV отсутствуют (`SAVE_LOADER.md` §3) — это не блокер.

## Топ-5: наибольший прогресс к меню и к миру

1. **Снять stall загрузки меню** — семафор `0x50c`, 64 KiB `[io] read`, watchdog (`STATUS_2026-11-27.md` §Open blockers #1). Без этого не воспроизводится ни первый GFx-draw, ни путь в мир. Готовое направление — `menu-io-stall`.
2. **Первый GFx-draw + его доказательство** — runtime PICA-программа вместо `none` и gate `N3DS_SHADER_DRAW_TRACE` (`STATUS_2026-11-27.md` §Open blockers #2; `WORLD_SHADER_CLOSURE.md` §2f), плюс нескинный fetch/pack в `d3d9_core.c`, без которого 13 из 17 world VS отказывает ядро (`WORLD_SHADER_CLOSURE.md` §2f). Это один и тот же момент «план → реальный draw» для меню и мира.
3. **Бэкенд varyings и явные precondition'ы материала** — `color0`/`color1` и дополнительные texcoord в PICA VS + `d9_menu_shader_note_preconditions()` для `normal`/`tangent`/c8 (`MENU_SHADER_RUNTIME.md` §1.1, §1.3, §3). Снимает refusal у logo/copy/fog/tint и у 46 выбранных VS; caps не трогаются.
4. **NPOT offscreen RT для GFx-масок** — противоречие №1 снято: `MENU_SHADER_RUNTIME.md` §1.7 теперь совпадает со STATUS и кодом (`b5abe8f`, POT-хранилище + логический sub-rect, `docs/NPOT_RENDER_TARGETS.md`). Три отказа в старых логах исторические; это больше не открытый RT-блокер.
5. **Мировой PS-пул: `def` fold (M) → COLOR1-drop с измеренной ошибкой → software PS fallback (L)** — ровно тот порядок работы, который указан в `WORLD_SHADER_CLOSURE.md` §2g; плюс 32 chunks зависимого чтения из §2d и 5×10 `lighting.approx.*` из §2e. Даёт наибольший выигрыш по частоте после уже закрытых 18 draw-путей.

Следующий эшелон (не в топ-5, но обязателен для железа): профиль 120/24 и cell-пики памяти (`MEMORY_BUDGET_178.md` §6, §8), стейджинг `Saves` (`SAVE_LOADER.md` §4), `operator new`/`delete` для Papyrus (`PAPYRUS_AUDIT.md` §4 п.1).
