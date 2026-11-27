# Карта тестов `tools/run_host_tests.ps1`

Источник: `tools/run_host_tests.ps1` (ветка `test-matrix`, HEAD `84b93da808d4bfd926fb46a959d86a42e154d7c0`). Ниже перечислены **все** записи: 183 вызова `Add-Test "name"` и 33 Python-скрипта (32 в массиве `$pyTests` + `tests/ir_container_growth_test.py`, добавляемый при наличии `E:\Skyrim_N3DS_build\ir_container\tesv.irc`). Итого **216 записей**.

Вывод `python tools/scan_unregistered_tests.py`: 234 тестовых файла (186 C, 48 Python), 203 зарегистрированы, 31 нет. 216 vs 203: 12 C-регистраций используют уже учтённый файл (`tests/win32_smoke.c` ×2, `tests/thread_coop.c` ×2, `tests/tnl_proto_smoke.c` ×2, `tests/ir_aot_fixture.c` ×2, `tests/pilot_host.c` ×9), а `tools/check_shim_bases.py` не попадает в выборку сканера (сканируются `tests/` и `source/shader_packs/`).

Условные регистрации (в счёт 216 включены): `menu_closure` (fixture `menu_closure_real_shaders.h`), `tev_real` (`SKYRIM_N3DS_TEV_CHUNKS` / `E:\Skyrim_N3DS_build\assets\shaders\tev_chunks.bin`), `stream_cell_real` (`staged/rt_table.h`), `havok_world` (`TESV.exe` + `tesv.irc`), `ir_equiv` (pilot fixture), `pilot_real`, `pilot_transpose`, `pilot_multranspose`, `pilot_planetransform`, `pilot_vec3coord`, `pilot_vec3normal`, `pilot_vec3normalize`, `pilot_planenormalize`, `pilot_matrixinverse` (pilot + `D3DX9_42.dll`), `tesv_probe`, `tesv_slice`, `tesv_slice_cube_probe` (`TESV.exe`, slice-генератор), `tests/ir_container_growth_test.py` (реальный IRC). `tesv_slice` и `tesv_slice_cube_probe` раннер помечает как диагностику (diagnostic), не PASS (нужен `-AllowDiagnosticFault`).

Формат строк: `имя | что проверяет (по имени/файлу/комментарию раннера; «см. файл» — если по имени неясно) | тип`.

## Сводка по областям

| Область | C (`Add-Test`) | Python (`$pyTests`) | Всего |
|---|---:|---:|---:|
| thread/sync | 4 | 0 | 4 |
| IR | 28 | 4 | 32 |
| d3d9/menu | 63 | 4 | 67 |
| textures | 35 | 1 | 36 |
| BSA/stream | 9 | 1 | 10 |
| win32 | 16 | 0 | 16 |
| shaders | 25 | 14 | 39 |
| tools/инфраструктура | 3 | 9 | 12 |
| **Итого** | **183** | **33** | **216** |

Группы непересекающиеся: каждый тест отнесён ровно к одной области. `depth_pair_scaled` — отдельная регистрация того же `tests/back_buffer_lockable_smoke.c` с аргументом `--scaled-pair`; `tnl_proto_off`/`tnl_proto` — один `tests/tnl_proto_smoke.c` с аргументами `off`/`on`; `lift` и `boot` оба линкуют `tests/win32_smoke.c`.

## thread/sync (4 C)

| Имя | Что проверяет | Тип |
|---|---|---|
| `sync` | `tests/win32_sync_smoke.c`: Win32 sync-примитивы guest (`source/win32_sync.c`) | C |
| `thread` | `tests/win32_thread_smoke.c`: потоки Win32 через guest | C |
| `thread_coop` | `tests/thread_coop.c`: кооперативные потоки guest + process/boot/pilot | C |
| `thread_coop_asm` | тот же `tests/thread_coop.c` с `-DWIN32_HOSTCTX_BACKEND=3` (ASM-бэкенд hostctx) | C |

## IR (28 C + 4 Python)

| Имя | Что проверяет | Тип |
|---|---|---|
| `tesv_tls_policy` | см. файл (`tests/tesv_tls_policy.c`, TLS-политика TESV) | C |
| `tesv_sm3_probe` | SM3-проба guest (`tests/tesv_sm3_probe_smoke.c`) | C |
| `tesv_cube_probe` | cube-проба guest (`tests/tesv_cube_probe_smoke.c`) | C |
| `tesv_cecdc0_probe` | cecdc0-проба guest (`tests/tesv_cecdc0_probe_smoke.c`) | C |
| `tesv_cecdc0_source` | cecdc0 source-проба (`tests/tesv_cecdc0_source_smoke.c`) | C |
| `tesv_fault_regs` | регистры guest-фолта (`tests/tesv_fault_regs_smoke.c`) | C |
| `tesv_cube_desc_probe` | cube descriptor probe (`tests/tesv_cube_desc_probe_smoke.c`) | C |
| `lift` | `tests/lift_smoke.c` + `tests/win32_smoke.c`: lift-runtime | C |
| `boot` | `tests/boot_smoke.c` + `tests/win32_smoke.c`: загрузка guest (`$comSources`) | C |
| `ir_havok_ext` | Havok SSE/x87 extension-формы, дифференциал AOT vs IR по self-authored corpus | C |
| `havok_world` | retail Havok world/body/add/step диагностика; asserts текущий no-solver результат (условный) | C |
| `ir_long_block` | прямые блоки длиннее uint8 block count должны делиться, не фолтить | C |
| `ir_cache_flush` | IR cache flush (`tests/ir_cache_flush_smoke.c`) | C |
| `rep_movs` | lifetime host-указателей rep movs при вытеснении страницы между src/dst | C |
| `rep_movs_slab` | slab-вариант rep movs (`tests/rep_movs_slab_smoke.c`) | C |
| `ir_equiv` | эквивалентность AOT и slice IR (`tests/ir_equiv.c`, `tests/ir_aot_fixture.c`;условный) | C |
| `pilot_real` | D3DX-пилот `D3DXMatrixMultiply` против реального `D3DX9_42.dll` (условный) | C |
| `pilot_transpose` | D3DX-пилот `D3DXMatrixTranspose` против реальной DLL (условный) | C |
| `pilot_multranspose` | D3DX-пилот `D3DXMatrixMultiplyTranspose` (условный) | C |
| `pilot_planetransform` | D3DX-пилот `D3DXPlaneTransform` (условный) | C |
| `pilot_vec3coord` | D3DX-пилот `D3DXVec3TransformCoord` (условный) | C |
| `pilot_vec3normal` | D3DX-пилот `D3DXVec3TransformNormal` (условный) | C |
| `pilot_vec3normalize` | D3DX-пилот `D3DXVec3Normalize` (условный) | C |
| `pilot_planenormalize` | D3DX-пилот `D3DXPlaneNormalize` (условный) | C |
| `pilot_matrixinverse` | D3DX-пилот `D3DXMatrixInverse` (условный) | C |
| `tesv_probe` | проба `TESV.exe` (`tests/tesv_probe.c`; условный) | C |
| `tesv_slice` | диагностический срез TESV: 2e8 блоков, guest-фолт/ExitProcess — диагностика, не PASS (условный) | C |
| `tesv_slice_cube_probe` | measurement-вариант среза с cube/cecdc0 shadow (условный, диагностика) | C |
| `tests/ir_partition_test.py` | валидация partition из `tools/ir_select.py`: синтетика + реальная выборка при наличии | Python |
| `tests/ir_select_rank_test.py` | синтетические тесты ранжирования `tools/ir_select.py` (time vs call, weighted_insns и т.д.) | Python |
| `tests/ir_container_test.py` | дифференциальные и integrity-тесты `source/ir_container.c` (через C-помощников) | Python |
| `tests/ir_container_growth_test.py` | growth-проверка реального контейнера на 90 900 функций; запускает `tests/ir_container_growth.c` (условный) | Python |

## d3d9/menu (63 C + 4 Python)

| Имя | Что проверяет | Тип |
|---|---|---|
| `menu_crash_repro` | детерминированный repro краша меню: реальные байты 0x60aa70/0x604750 через IR-контейнер, 3 состояния (valid / bad `this` / NULL `this`) | C |
| `graphics` | graphics smoke на фиксированной демо-сцене (`source/d3d9_demo_scene.c`) | C |
| `vertex` | вершинный путь d3d9 (`tests/vertex_smoke.c`) | C |
| `uicopy_host` | изолированный UI-copy host smoke (`tests/isolated/uicopy/host_smoke.c`) | C |
| `com` | COM-слой d3d9 (`tests/com_smoke.c`, demo/COM сцена) | C |
| `com_object_table` | таблица COM-объектов (`tests/com_object_table_smoke.c`) | C |
| `device_extra_methods` | дополнительные методы `IDirect3DDevice9` (по имени; см. файл) | C |
| `sampler_state` | `D3DSAMPLERSTATETYPE` int/float/dword, именованные отказы, очистка 16 стадий | C |
| `patch_process_refusals` | именованные отказы patch/process (по имени; см. файл) | C |
| `device_ps_constants` | PS-константы устройства (`tests/device_ps_constants_smoke.c`) | C |
| `device_vs_constants` | VS-константы устройства (`tests/device_vs_constants_smoke.c`) | C |
| `query` | query-интерфейс устройства (`tests/query_smoke.c`) | C |
| `device_misc_state` | прочее состояние устройства (`tests/device_misc_state_smoke.c`) | C |
| `state_block` | state blocks (`tests/state_block_smoke.c`) | C |
| `menu_text_draw` | отрисовка текста меню через `source/menu_swf_batch.c` + `source/d3d9_menu_text.c` | C |
| `menu_indexed_draw` | индексированная отрисовка меню (те же SWF batch/menu text) | C |
| `menu_pipeline_e2e` | сквозной конвейер меню: display/display_list/font/shape/tess/text/raster/atlas/batch | C |
| `tesv_init_contract` | контракт инициализации TESV в d3d9 (`tests/tesv_init_contract_smoke.c`) | C |
| `private_data` | private data устройства/ресурсов (`tests/private_data_smoke.c`) | C |
| `menu_text_plan_draw` | отрисовка текстового плана меню (`tests/menu_text_plan_draw_smoke.c`) | C |
| `swap_chain` | swap chain (`tests/swap_chain_smoke.c`) | C |
| `device_cursor_palette` | палитра курсора (`tests/device_cursor_palette_smoke.c`) | C |
| `com_postcaps` | COM post-caps (по имени; см. файл) | C |
| `adapter` | адаптер d3d9 (`tests/adapter_smoke.c`) | C |
| `check_format_matrix` | матрица форматов адаптера (`tests/check_format_matrix_smoke.c`) | C |
| `n3ds_sr` | PICA-SR: env parsing, DRS ladder, scaled target sizes, POT rounding, UV region, 2D/2DQ pass plan (без GPU) | C |
| `device_create` | `IDirect3D9::CreateDevice` (`tests/device_create_smoke.c`) | C |
| `device_reset` | контракт `IDirect3DDevice9::Reset` (состояние/ресурсы) | C |
| `scaled_target_blit` | blit масштабируемого target (`tests/scaled_target_blit_smoke.c`) | C |
| `stretchrect` | `StretchRect` (`tests/stretchrect_smoke.c`) | C |
| `up_draw` | up-draw путь (`tests/up_draw_smoke.c`; см. `docs/D3D9_UP_DRAWS.md`) | C |
| `null_backend` | путь устройство/null backend (`tests/null_backend_smoke.c`) | C |
| `alpha_scissor` | alpha/scissor состояние (`tests/alpha_scissor_smoke.c`) | C |
| `back_buffer_lockable` | lockable back buffer (`tests/back_buffer_lockable_smoke.c`) | C |
| `depth_pair_scaled` | тот же `back_buffer_lockable_smoke.c` с аргументом `--scaled-pair` | C |
| `gfx_d3d9_menu_compat` | совместимость меню с d3d9 gfx (по имени; см. файл) | C |
| `core_table_capacity` | ёмкость таблиц ядра d3d9 (`tests/core_table_capacity_smoke.c`) | C |
| `stretchrect_com` | `StretchRect` через COM (`tests/stretchrect_com_smoke.c`) | C |
| `stretchrect_com_sw` | software `StretchRect` через COM (`tests/stretchrect_com_sw_smoke.c`) | C |
| `gfxmesh_core` | gfxmesh core path (`tests/gfxmesh_core_smoke.c`) | C |
| `gfxmesh_com` | gfxmesh COM path (`tests/gfxmesh_com_smoke.c`) | C |
| `renderer_requirements` | требования рендерера (`tests/renderer_requirements_smoke.c`) | C |
| `menu_swf` | base SWF (`tests/menu_swf_smoke.c`) | C |
| `menu_swf_avm` | AVM SWF (`source/menu_swf_avm.c`, display/display_list) | C |
| `menu_swf_display` | SWF display (`source/menu_swf_display.c`) | C |
| `menu_swf_display_list` | SWF display list (`source/menu_swf_display_list.c`) | C |
| `menu_swf_clip` | SWF clip (`source/menu_swf_clip.c`) | C |
| `menu_swf_bitmap` | SWF bitmap (`source/menu_swf_bitmap.c`) | C |
| `menu_swf_shape` | SWF shape (`source/menu_swf_shape.c`) | C |
| `menu_swf_tess` | SWF tessellation (`source/menu_swf_tess.c`) | C |
| `menu_swf_text` | SWF text (`source/menu_swf_text.c`, font/shape/tess) | C |
| `menu_font_resolve` | разрешение шрифтов (`source/menu_swf_font.c` + raster/atlas/batch) | C |
| `menu_swf_glyph` | глифы SWF (`source/menu_swf_glyph_smoke.c`, font/raster/atlas) | C |
| `menu_swf_raster` | растеризация SWF (`source/menu_swf_raster.c`) | C |
| `menu_swf_atlas` | атлас SWF (`source/menu_swf_atlas.c`) | C |
| `menu_swf_batch` | батчинг SWF (`source/menu_swf_batch.c`) | C |
| `menu_swf_fill` | заливки SWF (`tests/menu_swf_fill_smoke.c`) | C |
| `menu_swf_mask_batch` | batch с масками (`source/menu_swf_clip.c` + batch) | C |
| `menu_swf_partial_mask` | частичные маски (clip/batch) | C |
| `menu_swf_gradient` | градиенты SWF (`tests/menu_swf_gradient_smoke.c`) | C |
| `menu_swf_html` | HTML-текст SWF (`tests/menu_swf_html_smoke.c`) | C |
| `menu_swf_frame_probe` | один диагностический кадр StartMenu.swf → RGBA8 на синтетическом movie, `--self-test` (НЕ кадр движка) | C |
| `d3d9_capture` | d3d9 capture (`source/d3d9_capture.c`) | C |
| `tests/menu_harness_test.py` | read-only карта StartMenu harness без игровых байт; реальный TESV проверяется только при наличии | Python |
| `tests/menu_shader_usage_test.py` | `tools/menu_shader_usage.py` на синтетических строках использования шейдеров | Python |
| `tests/menu_swf_probe_test.py` | `tools/menu_swf_probe.py` на синтетическом slice-логе, без retail-байт | Python |
| `tests/menu_swf_assets_test.py` | `tools/menu_swf_assets.py`: синтетические FWS + маленький BSA overlay | Python |

## textures (35 C + 1 Python)

| Имя | Что проверяет | Тип |
|---|---|---|
| `texture_residency` | резидентность текстур в texcache (`tests/texture_residency_smoke.c`) | C |
| `texture_residency_upload` | резидентность + upload (`source/d3d9_texture_data.c`) | C |
| `texcache_pinning` | frame-scoped pinning: bind/draw pins живут до границы кадра; пул из одних pinned отказывается | C |
| `texcache_limits_core` | лимиты `N3DS_TEX_*_LIMIT` + cube/volume/RT pinning через ядро d3d9 | C |
| `texture_container` | контейнер текстур (`source/d3d9_texture_data.c`) | C |
| `texture_runtime` | runtime текстур через core/texcache/texture_data/bc/vertex | C |
| `cube_rt` | cube render target (`tests/cube_rt_smoke.c`) | C |
| `cube_rt_sampling` | готовность выборки отрендеренных граней; incomplete/no-draw_cube отказ | C |
| `cube_volume_rehydration` | rehydration cube/volume texcache после GPU eviction | C |
| `budget_trim` | автоматическая LRU-trim под давлением device budget | C |
| `render_texture` | render texture (`tests/render_texture_smoke.c`) | C |
| `render_texture_hdr` | HDR render texture (`tests/render_texture_hdr_smoke.c`) | C |
| `d3dx_texture_shims` | D3DX texture shims через core/COM | C |
| `d3dx_cube_texture_shims` | D3DX cube-map loader + `IDirect3DCubeTexture9` COM path, пиксели граней null backend | C |
| `d3dx_volume_texture_shims` | D3DX volume loader + `IDirect3DVolumeTexture9` LockBox/UnlockBox, null slices | C |
| `cube_volume_residency` | cube/volume резидентность, accounting, повторно отказуемый release, no-evict | C |
| `d3dx_file_cube_volume` | file-path cube/volume D3DX exports через sandbox + общий DDS parser | C |
| `cube_sampling` | выборка cube через опциональный `draw_cube` backend callback | C |
| `volume_sampling` | выборка volume через опциональный `draw_volume` backend callback | C |
| `cube_draw_plan` | контракт cube draw ядра + таблица направлений, отказы level-0/no-callback | C |
| `cube_mip_sampling` | cube mip chain: gate резидентного уровня, выбор level-0 point | C |
| `pica_cube_texture` | контракт PICA cube backend через core (recording backend) | C |
| `texture_update` | семейство `UpdateTexture` / `UpdateSurface` / `ColorFill` (CPU store) | C |
| `rt_readback` | `GetRenderTargetData` readback в CPU offscreen surface; отказ front-buffer | C |
| `render_target` | render target (`tests/render_target_smoke.c`) | C |
| `render_target_bounded` | render target с ограничениями (`_bounded`) | C |
| `render_target_budget` | render target под бюджет (`_budget`) | C |
| `scaled_readback` | readback масштабируемого target (`tests/scaled_readback_smoke.c`) | C |
| `render_texture_sample` | выборка render texture (`tests/render_texture_sample_smoke.c`) | C |
| `depth_rt` | depth render target (`tests/depth_rt_smoke.c`) | C |
| `pica_depth_attachment` | PICA depth attachment (`tests/pica_depth_attachment_smoke.c`) | C |
| `pica_lazy_depth` | PICA lazy depth (`tests/pica_lazy_depth_smoke.c`) | C |
| `depth_target_matching` | matching depth target (`tests/depth_target_matching_smoke.c`) | C |
| `d3dx_surface_shims` | D3DX9 surface/shader exports: FromMemory/FromSurface + named `E_NOTIMPL` CompileShader | C |
| `texture_surface` | поверхность текстуры (`tests/texture_surface_smoke.c`) | C |
| `tests/texture_pipeline_test.py` | DDS/BC → ETC1 pipeline и контейнер SKTX: BC1/2/3, alpha policy, mip/dimensions | Python |

## shaders (25 C + 14 Python)

| Имя | Что проверяет | Тип |
|---|---|---|
| `bc_shader` | BC shader smoke на `$d3dFull` | C |
| `tev_translate` | транслятор TEV (`tests/tev_translate_smoke.c`) | C |
| `tev_accuracy` | точность TEV (`tests/tev_accuracy_smoke.c`) | C |
| `menu_light` | меню-освещение через TEV (`tests/menu_light_smoke.c`) | C |
| `pica_tev` | PICA TEV (`tests/pica_tev_smoke.c`) | C |
| `shader_plan_table` | таблица shader plan (`tests/shader_plan_table_smoke.c`) | C |
| `menu_vs_family` | измеренные перестановки `menu.generic.vs`: normal0/color1, abs, color outputs, texgen dp4 c4/c5, zyxw | C |
| `shader_plan_multipass` | multi-pass plan: 1 draw → 2..4 TEV программ, blend/depth без утечки, named refusal | C |
| `pica_vs_runtime` | выпущенный PICA VS: SHA-256 `.shbin`, mapping деклараций, банк 4-bone/WVP (без устройства) | C |
| `shader_draw_trace` | трассировка shader draw (`tests/shader_draw_trace_smoke.c`) | C |
| `pica_vs_draw` | skinned PICA VS: dirty-mask 256 строк, BLENDWEIGHT/BLENDINDICES, packed loader, gate `draw_pica_vs` | C |
| `pica_vs_flat_draw` | не-skinned (blit/lit) plain fetch: без blend-атрибутов, identity uniform bank, счётчик сабмитов | C |
| `shader_plan_com` | `CreatePixelShader` exact-hash plan: свой хэш → TEV, чужой → named refusal, precondition, caps без изм. | C |
| `tev_com` | TEV через COM (`tests/tev_com_smoke.c`) | C |
| `tev_text_plan` | TEV text plan (`tests/tev_text_plan_smoke.c`) | C |
| `shader_skip_probe` | measurement-only: неизвестный shader → неисполняемый объект, отказ draw | C |
| `shader_relative_addressing` | SM3 token walk: relative-addressed constants дают дополнительный address token | C |
| `shader_create_contract` | `CreateVertexShader`/Declaration: `INVALIDCALL` vs `NOTAVAILABLE` vs probe object | C |
| `tnl_proto_off` | CPU T&L prototype, производственный путь: named refusals, честные caps (аргумент `off`) | C |
| `tnl_proto` | тот же prototype с `TESV_D3D9_TNL_PROTO=1`: lighting math + fixed-function state (аргумент `on`) | C |
| `color_tev` | vertex-colour PICA: stage-0 → один TEV stage, CPU reference, точный RGBA null backend, COM-отказ | C |
| `two_stage` | two-stage TEV (`tests/two_stage_smoke.c`) | C |
| `color_com` | vertex-colour через COM (`tests/d3d9_color_com_smoke.c`) | C |
| `menu_closure` | контракт closure меню на реальных shader fixture (условный: `menu_closure_real_shaders.h`) | C |
| `tev_real` | измерение TEV-транслятора на кусках из retail-архива (условный: `tev_chunks.bin`) | C |
| `tests/shader_plan_merge_test.py` | merge-путь планов в `tools/fxp_compile.py` (канонические записи + остальные) | Python |
| `tests/shader_plan_extensions_test.py` | схема/верификатор/эмиттер для `constant_ops` и `passes[]` | Python |
| `tests/pica_runtime_audit_test.py` | gate: каждый draw-enabled план имеет конкретный runtime путь | Python |
| `tests/blit_vs_runtime_test.py` | runtime-gate семейства `blit_post` fullscreen-quad PICA VS (6 программ) | Python |
| `tests/vs_wide_runtime_test.py` | runtime-gate двух wide-varying world PICA VS (`lit`, `skin`) | Python |
| `tests/wide_vs_certification_test.py` | gate сертифика wide-varying VS (TEXCOORD1..4 + парный PS) | Python |
| `tests/shader_draw_resolution_test.py` | gate: draw-enabled world/menu plan разрешается в конкретную программу | Python |
| `tests/multipass_split_test.py` | gate feasibility записи multi-pass texture splitting | Python |
| `tests/world_shader_audit_test.py` | gate: каждый draw-enabled *world* план имеет рабочий runtime путь | Python |
| `source/shader_packs/skinning/test_pack.py` | skinned pack: catalog vs manifest, схема, именованные отказы (offline) | Python |
| `source/shader_packs/terrain/test_terrain_classes.py` | pack-local coverage + canonical-IR регресс terrain | Python |
| `source/shader_packs/lighting/test_lighting_plans.py` | lighting pack: exact + approximate рецепты против retail IR | Python |
| `tests/sm3_phase1_test.py` | SM3 phase 1: differential fields, schema/verifier планов, pack partition | Python |
| `tests/sm3_asm_gaps_test.py` | синтетические регрессы трёх IR-пробелов `asm.py` (texld control bits и др.) | Python |

## BSA/stream (9 C + 1 Python)

| Имя | Что проверяет | Тип |
|---|---|---|
| `mem_stats_bsa` | измерение BSA resident/index/page/range bytes по `E:\harness_skyrim\Data` + сжатая запись через stream snapshot (skip без данных) | C |
| `win32_bsa_overlay` | read-only `Data\...` BSA overlay: stat/open/read через `bsa_read_range` + compressed fallback | C |
| `menu_bsa` | BSA runtime меню (`source/menu_bsa_runtime.c`) | C |
| `bsa_index` | реальный BSA index: resident bytes, old/new reference equivalence, 100k random поисков (skip без архива) | C |
| `stream_range` | stream range (`source/asset_stream.c`) | C |
| `stream_queue` | stream queue (`source/asset_stream.c`) | C |
| `stream_cell` | cell trigger: синтетический guest tuple + 2-cell index (P0, latch, trim, demand) | C |
| `stream_cell_real_prefetch` | реальный prefetch по сжатым retail-записям (skip без индекса) | C |
| `stream_cell_real` | реальный Riverwood check при `STREAM_INDEX_REAL` (условный: `staged/rt_table.h`) | C |
| `tests/stream_index_build_test.py` | реальные данные для `tools/stream_index_build.py`: schema 2 cell table, Riverwood и 3×3 Tamriel (skip без Data) | Python |

## win32 (16 C)

| Имя | Что проверяет | Тип |
|---|---|---|
| `pe_imports` | PE imports (`source/pe_imports.c`) | C |
| `input` | Win32 input (`tests/win32_input_smoke.c`) | C |
| `user` | Win32 user (`tests/win32_user_smoke.c`) | C |
| `locale` | Win32 locale (`tests/win32_locale_smoke.c`) | C |
| `shell` | Win32 shell (`tests/win32_shell_smoke.c`) | C |
| `audio` | Win32 audio (`tests/audio_smoke.c`) | C |
| `audio_ndsp_host` | `N3DS_AUDIO=ndsp` host null sink: open/prepare/write/tick/reset/close, без NODRIVER | C |
| `audio_3ds_backend` | реальный `source/win32_audio_3ds.c` против fake libctru (ARM-путь на хосте) | C |
| `xaudio2` | портированный XAudio2 COM/mixer/sink (без SDL и engine thread) | C |
| `xaudio2_signal` | signal-level mixer: sine через граф, callbacks/context, SamplesPlayed drift, underruns, flush/destroy, 2 voice, WMA duration | C |
| `win32_crt` | портированный msvcrt heap/qsort/strtol/locale/setjmp | C |
| `win32_papyrus_crt` | papyrus-поверхность msvcrt: strtod/_itoa/rand/_errno/is*/math (ST0 return) | C |
| `steam` | Win32 Steam shim (`tests/win32_steam_smoke.c`) | C |
| `registry` | Win32 registry shim (`tests/win32_registry_smoke.c`) | C |
| `save_paths` | профиль/save path mapping + реальный round-trip `.ess`/`.skse` write-read-enumerate | C |
| `ini_overlay` | `TESV_INI_OVERLAY`: merge/replace `[General]` только при чтении | C |

## tools/инфраструктура (3 C + 9 Python)

| Имя | Что проверяет | Тип |
|---|---|---|
| `host` | базовый host probe: guest/PE/heap/call (`tests/host.c`) | C |
| `mem_stats` | учёт памяти (`tests/mem_stats_test.c`) | C |
| `bink_host` | BINK decoder host smoke (`source/bink_decode.c`) | C |
| `tools/check_shim_bases.py` | статическая проверка: thunk-окна Win32/D3DX host shim не пересекаются | Python |
| `tests/hotpatch_resolver_test.py` | unit-тесты MSVC lazy-ILT resolver в `prepare_pilot.py` | Python |
| `tests/import_bindings_test.py` | unit-тесты `tools/check_import_bindings.py` | Python |
| `tests/slice_pe_test.py` | unit-тесты локального декодирования функций в `tools/slice_pe.py` | Python |
| `tests/skyrim_assets_test.py` | тесты read-only пакета анализа ассетов `tools/skyrim` | Python |
| `tests/deploy_device_test.py` | `tools/deploy_device.py` на синтетическом дереве Skyrim Data | Python |
| `tests/save_ess_test.py` | `tools/save_ess.py`, два fixture | Python |
| `tests/pex_dump_test.py` | `tools/pex_dump.py`: синтетический Skyrim PEX 3.2 | Python |
| `tests/capture_raster_test.py` | `tools/capture_raster.py` без GPU/Pillow | Python |

## Не зарегистрированы (`python tools/scan_unregistered_tests.py`)

Сканер: 31 файл. Ниже — разделение на сборщики/фикстуры/помощники и реальные тесты. Все 31 в раннере `run_host_tests.ps1` не запускаются; часть запускается другими инструментами вручную.

### Сборщики, измерители, фикстуры и helper-бинарники (6)

| Файл | Природа | Кто запускает / зачем |
|---|---|---|
| `tests/ir_container_pack.c` | сборщик (C-пакер IR-контейнера) | `tools/ir_pack.py`, `tools/menu_repro_pack.py`, `tests/ir_container_test.py` |
| `tests/ir_measure.c` | измеритель декодера IR | `tools/ir_measure.py`; источник чисел в `docs/IR.md`, не часть интерпретатора |
| `tests/graphics_render.c` | CPU-эталонный растеризатор демо-сцены | строится вручную по `README.md`, не тест раннера |
| `tests/generated_smoke.c` | фикстурный smoke | линкуется с `fixture_*.c`, сгенерированными staged `lift.py` |
| `tests/ir_container_integrity.c` | helper с реальными проверками API/LRU/коррупции/short-read | запускается зарегистрированным `tests/ir_container_test.py` |
| `tests/ir_container_growth.c` | helper с реальной growth-проверкой на 90 900 функций | запускается условно зарегистрированным `tests/ir_container_growth_test.py` |

### Реальные тесты, не зарегистрированные в раннере (25)

C (9):

| Файл | Что проверяет | Почему не в раннере |
|---|---|---|
| `tests/audio_3ds_arm_smoke.c` | ARM/Azahar regression реального 3DS waveOut backend | требует `<3ds.h>`, отдельная сборка через `make AUDIO=1 TARGET=audio_3ds_arm` |
| `tests/d3d9_dynamic_chain_smoke.c` | end-to-end цепочка LoadLibrary → GetProcAddress → thunk → COM → CreateDevice → Present | отдельная ручная сборка, описана в `docs/D3D9_DYNAMIC_CHAIN.md` |
| `tests/ir_container_equiv.c` | дифференциал AOT vs slice IR vs predecoded container | требует подготовленный контейнер/корпус |
| `tests/ir_container_integration.c` | интегрированный growth-тест: контейнер → AOT callback без decoder fallback | требует контейнер (или standalone corpus mode) |
| `tests/ir_container_preentry.c` | холодный блок перед точкой входа должен паковаться в ту же функцию | отдельный сценарий контейнера |
| `tests/ir_container_preentry_dispatch.c` | dispatch slice-IR с pre-entry cold block обязан использовать frame контейнера | отдельный сценарий контейнера |
| `tests/pilot_smoke.c` | handwritten аналог синтетического DLL (`D3DXMatrixScaling`) | требует аргумент DLL, не 생성ный pilot |
| `tests/pipeline_real.c` | реальный inventory specialised pipeline registry (TSV для `inventory.py`) | требует локальный derived dump от `tools/pipelines/inventory.py` |
| `tests/pipeline_registry_smoke.c` | синтетические структурные/plan/fallback тесты registry | запускается `tools/pipelines/run.ps1` |

Python (16):

| Файл | Что проверяет |
|---|---|
| `tests/check_hw_log_test.py` | контракт `tools/check_hw_log.py` на hardware-log |
| `tests/fxp_compile_test.py` | офлайн-компилятор FXP → PICA200 (вариант B), синтетика + retail при наличии |
| `tests/menu_preprocess_test.py` | офлайн-препроцессоры меню, синтетика с bounded memory |
| `tests/menu_probe_test.py` | регресс `tools/menu_probe.py` на синтетическом slice-логе |
| `tests/mod_compat_test.py` | синтетические тесты mod-слоя (data_overlay, dds, mod_scan/optimize) |
| `tests/pipeline_test.py` | data/schema тесты registry (`tools/pipelines`) |
| `tests/stream_index_test.py` | `tools/skyrim/stream_index.py` на синтетических cooked BSA |
| `tests/test_runtime_contract.py` | сгенерированный runtime closure header: актуальность и отказы на ослабленный manifest |
| `tests/test_sky_water_pack.py` | sky_water pack: схема, покрытие 45 хэшей/82 кусков, план vs IR |
| `tests/tev_sched_test.py` | синтетика + каталог планировщика TEV-стадий |
| `tests/vs_emit_test.py` | straight-line PICA VS emitter и его hash table |
| `source/shader_packs/blit_post/test_blit_post_plans.py` | blit_post: каталог vs manifest, TEV-контракт, план vs IR |
| `source/shader_packs/skinning/test_emitter.py` | skinned PICA VS emitter, числовая верификация (retail-часть пропускается без архива) |
| `source/shader_packs/terrain/test_approximate_plans.py` | объявленные port-lite аппроксимации terrain |
| `source/shader_packs/ui/test_bsa_plans.py` | каталог планов `ui/interface`, emitter report и `.shbin` pins |
| `source/shader_packs/ui/test_menu_plans.py` | планы StartMenu/GFx (offline, опционально `--tesv PATH`) |

## Минимальный набор для быстрой проверки меню и мира (15)

C-часть (10, все безусловные): `menu_bsa`, `menu_swf_frame_probe`, `menu_pipeline_e2e`, `menu_text_draw`, `menu_indexed_draw`, `pica_vs_draw`, `pica_vs_flat_draw`, `gfxmesh_core`, `tnl_proto`, `stream_cell`.

```powershell
powershell -NoProfile -File tools/run_host_tests.ps1 -Only menu_bsa,menu_swf_frame_probe,menu_pipeline_e2e,menu_text_draw,menu_indexed_draw,pica_vs_draw,pica_vs_flat_draw,gfxmesh_core,tnl_proto,stream_cell -SkipPython
```

Python-часть (5, запускать отдельно; `-Only` фильтрует только `Add-Test`):

```powershell
.venv\Scripts\python.exe tests/menu_shader_usage_test.py
.venv\Scripts\python.exe tests/menu_swf_probe_test.py
.venv\Scripts\python.exe tests/menu_swf_assets_test.py
.venv\Scripts\python.exe tests/world_shader_audit_test.py
.venv\Scripts\python.exe tests/shader_draw_resolution_test.py
```
