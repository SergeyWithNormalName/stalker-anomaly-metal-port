# OpenXRay: кандидаты после E08 и CPU submission probes

Дата: 2026-09-28. Статус: **SOURCE REVIEW; новые ускорения не измерены.**

## Вывод

Есть основания для следующих ограниченных исследований. Самое интересное —
убирать целиком работу света, который не может дать видимого вклада. Далее —
возвращать консервативную видимость основной сцены и проверять стоимость CPU
подготовки отдельных light views. Переносить весь renderer или task scheduler
OpenXRay оснований нет.

Это не четыре полностью новые идеи: portal/HOM уже явно обсуждался в E08,
static shadow caching и grouping есть в release plan. Новое здесь — конкретные
механизмы OpenXRay и установленные разрывы с Direct Metal. В прочитанной
документации нет принятого A/B, отвергающего описанные ниже реализации.
Отсутствие такого результата не доказывает, что идею никогда не обсуждали.

## Что сверено

- [OPTIMIZATION_PROBES.md](OPTIMIZATION_PROBES.md),
  [PLAN_OPTIMIZATION.md](../../PLAN_OPTIMIZATION.md),
  [E08_PERFORMANCE.md](E08_PERFORMANCE.md),
  [E07_NPC_PERFORMANCE.md](E07_NPC_PERFORMANCE.md), E01, E05, E02–E06 audit,
  release plan, remaining work, Direct Metal architecture/baseline.
- Внешний `STALKER/PERFORMANCE_DIAGNOSIS.md` прочитан как история DXVK/MoltenVK;
  его старые настройки и FPS не являются контролем нынешнего Direct Metal.
- Обновлён remote ref локального `sources/openxray-dev` через fetch;
  HEAD/рабочие файлы этой копии не переключались. Исследован отдельный архив
  исходников текущего upstream `dev`:
  **b8c995cbb4f02826057d0f5b34112fdd8a645511**, commit от 2026-09-26.
  Ссылки ниже закреплены за этой ревизией.
- Проверены соответствующие текущие исходники Monolith/Direct Metal.
  Область обзора — renderer visibility, shadow preparation, binding/grouping
  и связанные CPU механизмы; это не аудит каждого subsystem/открытого PR.

Игра, GPU probes и сборки в этом обзоре не запускались. Runtime, shaders,
конфигурации и существующий WIP не редактировались; добавлен только этот отчёт.

## Границы сегодняшнего отрицательного результата

P2 проверял parallel **encoding статических shadow packets** после E08 merge.
Direct encode: 1.246 ms; 2 workers: 1.757 ms; 4 workers: 2.654 ms в коротком
representative probe. Поэтому этот прототип остановлен. Это не проверка
parallel visibility/preparation или исключения лишних light views.

E08 уже сократил S4 static local-shadow rows 12,135 → 1,889 encoded draws.
Нельзя снова обещать экономию всех 12,135 draw calls. В финальном историческом
S4 профиле весь local atlas имел CPU p50 15.473 ms, G-buffer — 12.429 ms.
Это бюджеты проходов, а не свободно устранимое время; вложенные binding
категории и GPU stage spans к ним не прибавляются.

Краткий P0 следующего раунда дал другие CPU/GPU времена на другом бинарнике
и условиях. Его 18.43 FPS не образуют A/B с E08 23.08 FPS.

## 1. Отсечение полностью закрытого света до shadow preparation

**Первый кандидат для оценки пользы. Не уникален для OpenXRay; отсутствует
в активном Direct Metal пути.**

В OpenXRay [light_vis.cpp](https://github.com/OpenXRay/xray-16/blob/b8c995cbb4f02826057d0f5b34112fdd8a645511/src/Layers/xrRender/light_vis.cpp#L14)
проверяет light volume через occlusion query. Затем
[render_lights](https://github.com/OpenXRay/xray-16/blob/b8c995cbb4f02826057d0f5b34112fdd8a645511/src/Layers/xrRender_R2/r2_R_lights.cpp#L13)
исключает невидимый свет до построения shadow graph. Проверки распределены
по кадрам; близкий к камере объём допускается без query.

У нас механизм `vis_prepare/vis_update` есть в старом D3D пути:
`src/Layers/xrRender/light_vis.cpp`, `lights_render.cpp:76`.
Но Metal берёт value snapshot из registry
(`r__dsgraph_render.cpp:966–1030`) с frustum/fog/LOD и sector fallback;
`planLocalShadowBatches` (`MetalR4EngineScene.mm:2404`) затем проверяет
пересечение volume с frustum. Проверки экранного перекрытия нет; интерфейс
`IR4Backend` не предоставляет light visibility query.

**Гипотеза для нас:** исключить полностью закрытый источник до atlas planning,
caster selection, transforms, bindings и GPU shadow work. При невидимом point
parent это может убрать все шесть faces. Число таких источников в S4 сейчас
не измерено; «157 shadow views» не означает «157 лишних views».

Начальный probe должен только собирать результаты, продолжая рисовать все
источники. Считать нулевые результаты, цену queries и стоимость относящихся
к ним shadow batches; учесть, что частичный отсев может не убрать encoder.
CPU не должен ждать GPU ради culling. Применять результат можно лишь с
доказанным контрактом актуальности камеры, света и occluders; иначе fail open.

Для сохранения качества нельзя механически копировать threshold `<=4` fragments
или temporal задержки OpenXRay. Начать с доказанного нулевого вклада,
near/camera-inside fallback и исключения volumetric/HUD случаев, пока их
контракт не проверен. Старый нулевой query при движении камеры/двери/NPC не
является доказательством нынешней невидимости. Atlas/accumulator и динамическое
раскрытие света должны совпасть с полным контрольным рендером.

**STOP:** нулевых источников мало, query/maintenance съедают экономию либо
корректная invalidation почти всегда возвращает полный путь.

## 2. Portal/HOM для основной видимости

**Уже думали; не испытали и не отвергли. Более крупная интеграция.**

OpenXRay
[render_main::calculate](https://github.com/OpenXRay/xray-16/blob/b8c995cbb4f02826057d0f5b34112fdd8a645511/src/Layers/xrRender_R2/r2_R_calculate.cpp#L30)
строит основной graph с HOM и portal traversal. HOM подготавливается после
обновления камеры, а Calculate ждёт готовности перед использованием.

В нашем `r4_loader.cpp:35–56` Metal возвращается до LoadVisuals/LoadSectors/HOM.Load;
`r4_R_render.cpp:69` — до старого HOM/static render;
`r4.cpp:826–828` отвечает «видимо» на occ_visible для Metal.
LMH G-buffer (`MetalBootstrap.mm:3389`) и static forward
(`MetalR4StaticForwardAdapter.cpp:475`) фильтруются frustum, а не этим graph.
E08 прямо отметил отсутствие готового current-frame static visible-ID snapshot.
Наличие `GMBase.traverse` в Metal Calculate не восстанавливает отсутствующие
level data и не делает snapshot HOM-qualified.

Следующее ограниченное действие — отдельное представление CPU occluders/portals
с отображением source visual ID в Metal families; сначала сравнивать списки
в диагностике без изменения рисования. Для статических opaque receivers
возможна экономия и CPU submissions, и GPU vertex/fragment work.

**Ключевая граница:** список основной камеры используется для основной сцены.
Shadow casters отбираются независимо по light/cascade; невидимый камерой
объект может отбрасывать видимую тень. Это уже было предметом E05 исправлений.
Нужны conservative clipping, fail-open при неопределённости, matching projection
и проверка дверей/portal edges. Не загружать весь старый D3D renderer ради HOM.

**STOP:** полезных отбрасываний недостаточно или мост требует слишком широкой
перестройки ownership. Выигрыш пока неизвестен.

## 3. Независимая CPU preparation для light views

**Другой эксперимент, чем сегодняшний P2; сначала нужен его собственный бюджет.**

В OpenXRay
[r2_R_lights.cpp:86–210](https://github.com/OpenXRay/xray-16/blob/b8c995cbb4f02826057d0f5b34112fdd8a645511/src/Layers/xrRender_R2/r2_R_lights.cpp#L86)
каждый свет получает отдельный dsgraph context; `calc_lights` строит subspace
в task, а flush ждёт его перед последовательным рисованием.
`alloc_context(false)` отделяет CPU graph state от command-list allocation.

У нас `renderLocalShadowBatch` (`MetalR4EngineScene.mm:2535–2648`)
последовательно строит matrices, BVH selections, final predicates и WVP payloads.
Потом использует общий upload/cache/backend. Можно исследовать только чистую
подготовку immutable per-view результатов на двух workers; flatten/upload,
cache mutation и Metal encoding оставить на прежнем потоке и в прежнем порядке.
BVH Query и adapter scratch нужно отдельно проверить на независимость; их
текущая thread safety здесь не доказана. AI/physics/bones в этот перенос не входят.

Сначала измерить именно CPU preparation в S4 по всем нужным families.
15.473 ms всего atlas не являются её бюджетом. P2 preparation ~0.798 ms
в synthetic block также не доказывает большой игровой резерв. Распараллеливать
маленький остаток бессмысленно. CPU probe должен считать весь путь с dispatch,
join, allocations и конкуренцией с игровыми потоками, а не сумму worker times.

Особенно важно:
[parallel traversal отдельных static children](https://github.com/OpenXRay/xray-16/blob/b8c995cbb4f02826057d0f5b34112fdd8a645511/src/Layers/xrRender/r__dsgraph_build.cpp#L749)
в upstream выключен через `#if 0`, с комментарием о неразделённых visual maps.
Это нельзя выдавать за готовое работающее ускорение внутри каждого graph.
Перенос всего TaskManager/contexts API не требуется для узкого probe.

**STOP:** чистой независимой работы мало или ownership требует общей
сериализации/широкого engine refactor.

## 4. Кеш невидимых статических shadow casters (`smapvis`)

**Конкретизация ранее предусмотренного static cache; резерв с высоким риском.**

OpenXRay
[light_smapvis.cpp](https://github.com/OpenXRay/xray-16/blob/b8c995cbb4f02826057d0f5b34112fdd8a645511/src/Layers/xrRender/light_smapvis.cpp#L24)
постепенно проверяет static visuals против shadow depth, сохраняет невидимые
и исключает их из следующих graph builds. Spatial/light change вызывает
[invalidate](https://github.com/OpenXRay/xray-16/blob/b8c995cbb4f02826057d0f5b34112fdd8a645511/src/Layers/xrRender/light.cpp#L216).
Это кеш исключаемых visuals, не кеш готовой shadow texture.

В текущем Metal есть conservative BVH и кеш объединённых индексов; они не
проверяют depth occlusion и не кешируют невидимость caster. Release plan E08
уже допускает static cache при неизменных свете/геометрии, но этот конкретный
механизм не квалифицирован. Merge уменьшил CPU вызовы; возможный дополнительный
резерв TC лежит также в GPU geometry work, но пока не измерен.

Прямое копирование политики небезопасно: временно закрытый NPC объект или
wind/AREF geometry нельзя навсегда исключить. Первый вариант — только
immutable opaque casters и occluders, фиксированный light projection,
source depth semantics, bounded cache и полный reset при смене owner/level/light.
Dynamic/Flora/DetailGrass продолжают идти полным путём. Пробные maps должны
совпасть с oracle, включая depth ties и будущую смену состояния.

**STOP:** мало новых отброшенных casters после BVH/merge, цена queries велика,
invalidation не локальна или pixel oracle показывает потерю shadows.

## Что не выдавать за новую найденную оптимизацию

| Направление | Статус у нас и решение |
|---|---|
| Static BVH, sun culling, atlas encoder batching, particle arena | Уже приняты в E08. |
| Parallel Metal encoding этого static участка | Отрицательный representative screen; не повторять без новой причины. |
| Resource dedup / private upload batching | Нет существенной изолированной пользы S2; успешного S4 dedup результата нет. |
| Flora shadow instancing | Эксперимент сохранён; S2 умеренный результат, S4 полезный timing не получен из-за startup failures. Не считать полностью отвергнутым или включённым. |
| Residency Sets / MSC tables / ICB | P1/P3 предварительно остановлены, не A/B-отвергнуты. OpenXRay не добавляет доказательств ROI этих Metal механизмов. |
| Pass/material grouping | Есть в [OpenXRay render_graph](https://github.com/OpenXRay/xray-16/blob/b8c995cbb4f02826057d0f5b34112fdd8a645511/src/Layers/xrRender/r__dsgraph_render.cpp#L39), но уже предусмотрено release plan. Наш LMH избегает повторного PSO/VB bind; TLAB всё ещё создаётся per draw. Новый scope требует количества реально одинаковых descriptors/constants, а не обещания убрать весь MSC bucket. |
| GPU skinning / общий scheduler / Lua GC tuning | Не следующий обоснованный перенос после E07/E08. У нас уже есть bones task и отдельные Lua GC механизмы; текущего evidence об их доминировании нет. |
| Adaptive shadow resolution, SSA/дальность, реже обновляемые позы | Изменяют quality/simulation contract; не входят в оптимизацию без потери качества. |

## Предложенная последовательность

1. Оценить **whole-light occlusion** в режиме observe-only: сколько atlas work
   потенциально исчезает и какой ценой. Затем pixel/motion oracle, лишь после
   положительного screen — ограниченный игровой A/B.
2. Отдельно оценить **portal/HOM** как известную, ещё не проверенную ветку:
   сначала CPU/data bridge и source-ID diagnostics. Это больший объём работы.
3. Проверять **parallel preparation** только при достаточном измеренном
   независимом CPU бюджете; не возобновлять P2 под новым названием.
4. **smapvis/static TC** оставить резервом после доказанного static-only
   контракта и оценки остаточного GPU/draw бюджета.

Для игрового A/B нужен новый same-binary control/candidate, S4 и контрольная S2,
обычная телеметрия без detailed clocks/validation/capture, фиксированные inputs
и питание/термальные условия. Наследуем экономический gate PLAN_OPTIMIZATION:
воспроизводимое сокращение S4 encode p95 ≥3 ms и ≥10% и полезный итог целого
кадра; для преимущественно GPU кандидата CPU gate следует явно пересмотреть
до запуска, сохранив требование whole-frame/presentation benefit.
Ни один кандидат сейчас не обещает native 1080p60.
