# Direct Metal R4/GAMMA: план доведения до готовой игры

Дата: 2026-09-21. Владелец исполнения: Sol. Исполнители ограниченных задач: Terra.

**E08, 2026-09-28: OPTIMIZATIONS IMPLEMENTED / NATIVE60 OPEN.** Выполнен только E08 по отдельному поручению пользователя: conservative caster BVH, atlas batching, MSC stack scratch, late drawable, static sun culling, particle arena и order-preserving static shadow merge. Контролируемое сравнение S2: медиана 8,57→30 FPS; отдельный S4 A/B: 20→24 FPS. Финальная native1080p матрица: 24–40 FPS, все шесть процессов exit0. Полный gate 60 FPS не закрыт; продолжение после снижения отдачи остановлено по условию пользователя. [Измерения, проверки и ограничения E08](docs/direct-metal-release/E08_PERFORMANCE.md).

**E07, 2026-09-28 (предыдущий handoff):** по прямому запросу пользователя визуальный паритет принят как временное рабочее допущение, дальнейшие визуальные поиски отложены. Диагностика E07 выбрала **ветку B, shadow/rendering costs**. CPU skin oracle сохранён; exact visible-count curve остаётся **OPEN**. [Результаты и границы E07](docs/direct-metal-release/E07_NPC_PERFORMANCE.md).

Корень проекта: `<XRAY_SOURCE_ROOT>`.

**Актуализация 2026-09-27:** [аудит E02–E06](docs/direct-metal-release/E02_E06_AUDIT_2026-09-27.md) подтвердил ошибки cutoff локального света, production motion/history и shadow coverage/cascades. E04/E05 повторно открыты, E06 не завершён; перед E07 выполнить пакеты исправлений из аудита. Исторические AUTO PASS не закрывают эти замечания. В аудите изменена только диагностика; большие исправления оставлены Sol по запросу пользователя.

**Пакет 1 выполнен, 2026-09-27:** A1 cutoff локального света устранён переносом Windows stencil rollover. Production-MSC GPU-тесты 125/126/127/200/400 записей, point/spot/OmniFace в хвосте списка и две camera poses прошли; накопленный свет, geometry bit и depth сохранены. Bar motion: exit0, API/GPU validation, 232..1135 записей без unsupported cutoff. Новый Bar GPU trace: 590 записей, 1774 draws, 4 сброса, подтверждённое продолжение 255 → masked reset → 5. [Доказательства и границы](docs/direct-metal-release/E05_LIGHT_MARKER_ROLLOVER.md). Следующий пакет — 2, production motion/history; A3/A4 и E06 остаются открытыми.

Аудит выполнен на `feature/native-metal-r4`, HEAD `c8add841849cf7c4f1269d94e0addaac3b5d3427`, **с 665 изменёнными/неотслеживаемыми путями**. HEAD не идентифицирует текущую сборку и WIP.
Машина аудита: Apple M1 Pro, 16 GiB RAM; точное число GPU-ядер, macOS, режим питания и дисплея записать в baseline.

**Результат этой задачи — план, не исправление игры.** Изучены текущие исходники, launcher, профиль, тестовый реестр и сохранённые результаты. Нового игрового запуска, GPU trace или замера FPS в этой задаче не было. Текущие визуальные и performance gates открыты.

Этот документ задаёт новый порядок исполнения по запросу пользователя. `PLANFORGEMINIFLASH.md` и `DIRECT_METAL_REMAINING_WORK.md` использовать как справочники по прежним решениям и командам, а не как доказательство готовности. Их старые PASS относятся к отдельным сценариям и прежнему коду. В частности, правило «производительность только в самом конце» заменяется на **замеры с начала, исправление ошибок перед оценкой качества, оптимизация подтверждённых затрат без ухудшения паритета**. Не повторять уже написанные water/AO/weather реализации с нуля.

## 1. Что требуется получить

1. Визуальное и функциональное соответствие **активному Windows Anomaly/GAMMA с тем же MO2-профилем**, сохранением и настройками. Не абстрактному ванильному STALKER и не произвольному красивому пресету.
2. Отсутствие растянутых полигонов, пурпурных ошибок, отставания частей сцены при повороте, скачущего освещения травы, неверного света на NPC и заметных провалов эффектов.
3. Комфортные **нативные 1920×1080, минимум стабильные 60 FPS** в согласованной матрице сцен, включая NPC. Производительность существенно выше исходной — измеряемая цель, а не заранее гарантированный результат смены API. 90–120 FPS — последующая цель при наличии запаса.
4. Обычный запуск, рабочие настройки, сохранения, переходы между уровнями, оружие/прицелы, UI, погода, стабильность длительной игры; DXVK Native/MoltenVK сохраняется рабочим fallback.
5. ИИ сам собирает, запускает, измеряет, снимает скриншоты/видео, разбирает логи и делает повторные проверки. Пользователь нужен для ограниченной визуальной оценки и только для внешнего эталона, который агент действительно не может получить сам.

**Gate 60 FPS:** после прогрева, без capture/validation/debug overlay, на сетке сцен из раздела 4, по 3 прогона × 120 секунд: средний FPS ≥60, p99 интервала реально показанных кадров ≤16.67 ms и 1% low ≥60 FPS. 1% low считать как `1000 / mean(самые медленные 1% frame_ms)`, а не как обратный p99. Хранить сырые интервалы, p50/p95/p99/max и число кадров >25/33/50 ms. Пограничные результаты около 16.67 перепроверять с учётом точной частоты дисплея и погрешности часов; не округлять 17–20 ms в PASS. Загрузки/переходы считать отдельной категорией, не вырезать hitch при обычной ходьбе или появлении NPC. Все три прогона должны проходить; одного удачного среднего недостаточно.

Рабочий ориентир с запасом: p95 GPU ≤14 ms и p95 критического CPU-пути ≤14 ms. Это **целевые бюджеты**, не текущие измерения. Перекрывающиеся CPU/GPU интервалы не складывать. Отдельный uncapped прогон показывает запас, прогон с ограничением 60 проверяет pacing. Frame generation, рендер ниже 1080p, уменьшение NPC/дальности/травы не засчитываются как выполнение этой цели. Возможный отдельный режим с апскейлом маркировать явно.

Если после измеренных оптимизаций M1 Pro не выполняет gate при эталонном качестве, результат — **PERFORMANCE OPEN**, с доказанным остаточным ограничением и вариантами компромисса. Не обещать невозможное и не объявлять сниженное качество паритетом.

## 2. Что установлено сейчас, а что является гипотезой

Исходный пользовательский кадр сохранён в [docs/direct-metal-release/2026-09-21-user-report.png](docs/direct-metal-release/2026-09-21-user-report.png). Это изображение всего окна/экрана; его размер не доказывает размер render target.

| ID | Наблюдение / проверенный факт | Следствие для плана |
|---|---|---|
| F01 | На кадре крупные светлые вытянутые формы в небе, пурпурные фрагменты около построек, очень тёмные участки растительности/строений. | P0: определить draw/pass, создавший каждый дефект. По картинке нельзя назначить причиной UV, skinning или lens flare. |
| F02 | Пользователь сообщает об отставании объектов при резкой камере, мерцании света травы, около 20 FPS и ухудшении при NPC. | Нужны motion fixture и разделение CPU/GPU. Это сообщения пользователя, не новый benchmark. |
| F03 | `MetalR4Backend.mm:62,815,2102`: 3 frame contexts; `BeginFrame` ждёт только свой слот; `UpdateBuffer` для Shared делает непосредственный memcpy. `BindMscResources` сохраняет GPU-адрес и ссылку, не копирует данные constant buffer. | Удержание lifetime не защищает содержимое от перезаписи. Требуется аудит изменяемых данных по кадрам и по draw/pass. |
| F04 | `MetalR4SsfxAoAdapter.cpp:58,140,381,440,478,537`: общие Shared `aoGlobals`, `blurGlobals`, fullscreen vertices. Один `aoGlobals` сначала получает AO, затем IL; blur constants и vertices меняются между проходами до общего submit. | Подтверждён конфликт версий содержимого внутри command buffer. Triple buffering только по кадрам его не исправит. Связь с конкретным пользовательским мерцанием ещё проверить. |
| F05 | `MetalR4FloraAdapter.cpp:574,677` и `MetalR4DetailGrassAdapter.cpp:970` обновляют постоянные draw buffers; frame slot в этих местах отсутствует. `StageFrame` травы повторно использует draws при одинаковой generation. | Высокоприоритетный риск CPU/GPU гонки между кадрами. Проверить все потребители, а не только AO. |
| F06 | `MetalR4DynamicWorldAdapter.cpp:1326` уже выбирает `ActiveFrameContextIndex`; `MetalR4WaterAdapter.cpp:288,511` имеет frame resources. | Не переписывать их под видом отсутствующего buffering; проверить достаточность по passes/views и перенести верный контракт на другие адаптеры. |
| F07 | DynamicWorld CPU-skins геометрию (`:699,798`, обработка 1–4 весов); `Prewarm` имеет pose hash и cache. DetailGrass StageFrame (`:722`) перепаковывает batches и создаёт buffers при смене generation. | Реальные кандидаты затрат при NPC/движении. Их доля во времени пока неизвестна; сначала profile, затем GPU skinning/кэширование при подтверждении. |
| F08 | Актуальный launcher подключает `water-next`, `ssfx-ao-il-next`, `clouds-next`, `rain-next-v2`, `lens-flare-next`. | Старое утверждение «воды/AO/облаков нет» нельзя использовать как текущий диагноз. Проверять admission, фактические draws, значения, order и результат. |
| F09 | В `scratch/parity_results/parity_manifest.json` камера reference/Metal расходится примерно на 0.853 m; направление тоже отличается. `w_cloudy3_dark` против `w_cloudy3`, fog 399.5 против 464.468903, save SHA пустой, при этом `validation_errors=[]`. | Существующий comparator допускает несопоставимые сцены. Его MAE/PSNR не являются доказательством паритета. E00 должен исправить gate сравнения. |
| F10 | `appdata/user.ltx`: `vid_mode 1920x1200`, `rs_stats on`, `rs_v_sync off`, motion blur off; активные SSFX AO/IL настройки. В старом harness Metal использует native-retina override. | Новый baseline обязан доказать 1920×1080 для drawable **и внутренних targets**, эффективные параметры и среду процесса. Нельзя приписывать lag motion blur без проверки. |
| F11 | Корневой `ctest --test-dir build -N` даёт 0 tests. В `build/src/Layers/xrRenderMetal` зарегистрировано 28, часть probe binaries отсутствует. | `ctest` без тестов не является PASS. Сначала построить нужные targets, затем запускать правильный каталог и проверять ненулевое число выполненных тестов. |
| F12 | HUD на пользовательском кадре содержит явно неправдоподобные огромные ms и нулевые счётчики при видимой сцене. В backend completion handler нет полноценного CPU/GPU/present отчёта. | HUD не годится для определения bottleneck. Сделать независимые монотонные timings, после этого чинить отображение Stats. |
| F13 | `MetalR4EngineScene.mm:26` включает `MetalBootstrap.mm` как production implementation. | Это не только demo. Не отдать двум агентам независимые изменения этих файлов без согласованного владельца. |
| F14 | `DisplayCapabilities.h/.mm`: read-only диагностический слой дисплея и presentation state полностью реализован и верифицирован (`DisplayCapabilitiesProbe`, 8/8 PASS). | Готовая инфраструктура для E00/E01/E04/E06: исключает догадки о backing scale, refresh rates, drawableSize vs render targets, EDR headroom. Статус: INFRASTRUCTURE COMPLETE / PRODUCTION READ-ONLY. |
| F15 | `MetalFXSpatialScalerProbe.mm`: автономный probe апскейла создан и верифицирован (7/7 PASS, M1 Pro ~1.1 ms GPU на 540p..900p→1080p, BGRA8/RGBA16F, Perceptual/Linear/HDR, DRS). | API research закрыт, повторять не нужно. MetalFX остаётся опциональной post-baseline задачей; не может подменять нативный 1080p/60 FPS gate. Статус: PROBE COMPLETE / OPTIONAL FUTURE WORK. |
| F16 | `CAMetalDisplayLinkProbe.mm`: автономный probe презентации создан и верифицирован (7/7 PASS, экспорт сырых данных в CSV/JSON). | Источник экспериментальной телеметрии для E01/E04/E08. Не догма поведения OS; Sol должен использовать сырые метрики для A/B сравнения с текущим presentation path. Статус: PROBE COMPLETE / EXPERIMENTAL TELEMETRY. |

Линии — ориентиры на дату аудита; искать также по символам. Нет доказательств, что все симптомы имеют одну причину. Отдельно проверяются corruption, transform mismatch, temporal ghosting и общая задержка представления.

### 2.1. Завершённые платформенные исследования Apple (probes & read-only infrastructure)

В ходе предварительных исследований реализованы изолированные диагностические модули и собрана аппаратно-программная телеметрия на целевой машине (Apple M1 Pro, macOS 27.2). Будущему Sol **запрещено повторять эти исследования с нуля**; результаты используются как готовый вход для соответствующих этапов.

1. **Диагностический слой возможностей дисплея (DisplayCapabilities):**
   - **Файлы:** [`DisplayCapabilities.h`](file://<XRAY_SOURCE_ROOT>/src/Layers/xrRenderMetal/DisplayCapabilities.h), [`DisplayCapabilities.mm`](file://<XRAY_SOURCE_ROOT>/src/Layers/xrRenderMetal/DisplayCapabilities.mm), [`DisplayCapabilitiesProbe.mm`](file://<XRAY_SOURCE_ROOT>/src/Layers/xrRenderMetal/tests/DisplayCapabilitiesProbe.mm).
   - **Таргет сборки:** `XRay.Metal.DisplayCapabilitiesProbe` (CTest: `XRay.Metal.DisplayCapabilities.SelfTest`).
   - **Статус:** `INFRASTRUCTURE COMPLETE / PRODUCTION READ-ONLY`.
   - **Получаемые параметры:** `maximumFramesPerSecond`, `minimumRefreshInterval`, `maximumRefreshInterval`, `displayUpdateGranularity`, `backingScaleFactor` (Retina), размеры окна в points и backing pixels, `CAMetalLayer.drawableSize`, внутреннее разрешение рендера, `maxExtendedDynamicRangeColorComponentValue`, `currentExtendedDynamicRangeColorComponentValue`, `layerPixelFormat`, `layerColorSpace`, `wantsExtendedDynamicRangeContent`, признак VSync/present timing.
   - **Верифицированные сценарии (8/8 PASS):** Built-in Liquid Retina XDR (120 Hz ProMotion, EDR headroom 1.0–3.8), внешний монитор (PHL 276B1 @ 75 Hz SDR), resize окна, Retina scaling, перенос окна между экранами с разной частотой и плотностью пикселей, disconnect-safe / null-handle robustness.
   - **Применение в плане:** Фундамент для E00 (честный baseline без ошибок масштабирования), E01/E04 (проверка соответствия частот и drawable size), E06 (инспекция EDR/SDR форматов). *Не означает готовность HDR или ProMotion в игре.*

2. **Автономный probe MetalFX Spatial Upscaling:**
   - **Файлы:** [`MetalFXSpatialScalerProbe.mm`](file://<XRAY_SOURCE_ROOT>/src/Layers/xrRenderMetal/tests/MetalFXSpatialScalerProbe.mm).
   - **Таргет сборки:** `XRay.Metal.MetalFXSpatialScalerProbe` (CTest: `XRay.Metal.MetalFXSpatialScaler.SelfTest`).
   - **Артефакты валидации:** `scratch/metalfx_input_540p.png`, `scratch/metalfx_upscaled_1080p.png`.
   - **Статус:** `PROBE COMPLETE / OPTIONAL FUTURE WORK`.
   - **Проверенные факты на Apple M1 Pro:**
     - `[MTLFXSpatialScalerDescriptor supportsDevice:]` возвращает `YES`.
     - Масштабирование до 1920×1080 проверено для: `960×540` (2.0x), `1280×720` (1.5x), `1600×900` (1.2x), `1920×1080` (1.0x control / passthrough).
     - Форматы текстур: поддержаны `MTLPixelFormatBGRA8Unorm` и `MTLPixelFormatRGBA16Float`.
     - Цветовые пространства (`colorProcessingMode`): `Perceptual (0)` (sRGB/SDR, рекомендован для X-Ray после тонемаппинга во избежание тёмных ореолов), `Linear (1)` (SDR линейный), `HDR (2)` (расширенный диапазон $>1.0$ с обратимым сжатием кривой).
     - CPU readback & валидация ориентации: габариты совпадают, данные не вырождены, угловые маркеры (Top-Left Red, Top-Right Green, Bottom-Left Blue, Bottom-Right White) подтверждают отсутствие переворота координат.
     - Стоимость апскейла на Apple M1 Pro GPU: ~1.05–1.20 ms (в синтетическом изолированном тесте; это **НЕ** измерение выигрыша FPS в игре).
     - Правила пересоздания: изменение максимальных габаритов `inputWidth/Height`, `outputWidth/Height`, форматов или color mode требует пересоздания `MTLFXSpatialScaler` (они read-only). Изменение `inputContentWidth/Height` динамически поддерживается **без пересоздания** (основа для будущего DRS). Выходная текстура строго требует `MTLStorageModePrivate`.
   - **Применение в плане:** Рабочая гипотеза размещения в pipeline: `3D scene (low-res) -> scene postprocess/tonemap -> [MetalFX Spatial Scaler] -> native-resolution UI/HUD -> present`. Точное местоположение подтверждается Sol только после стабилизации E06. MetalFX не может подменять нативный gate 1080p/60 FPS; апскейл-режим маркируется отдельно.

3. **Автономный probe CAMetalDisplayLink и ProMotion/VRR:**
   - **Файлы:** [`CAMetalDisplayLinkProbe.mm`](file://<XRAY_SOURCE_ROOT>/src/Layers/xrRenderMetal/tests/CAMetalDisplayLinkProbe.mm).
   - **Таргет сборки:** `XRay.Metal.CAMetalDisplayLinkProbe` (CTest: `XRay.Metal.CAMetalDisplayLink.SelfTest`).
   - **Сырая телеметрия:** `scratch/cametaldisplaylink_samples.csv` (441 KB сэмплов), `scratch/cametaldisplaylink_summary.json`.
   - **Статус:** `PROBE COMPLETE / EXPERIMENTAL TELEMETRY`.
   - **Проверенные факты и собранные метрики:**
     - Исследованы `CAMetalDisplayLink` (macOS 14+), `CAMetalDisplayLinkUpdate` (`drawable`, `targetTimestamp`, `targetPresentationTimestamp`).
     - Презентация выполнена через штатный `[cb presentDrawable:update.drawable]` перед `commit` без вызова конфликтующих timed-методов.
     - Проверены `preferredFrameLatency = 1.0` и `2.0`: в Windowed режиме оба дают `presentation lead` ~39.9 ms (3 кадра конвейера композитора) и `deadline lead` к моменту рендера ~13.2 ms при 75 Hz.
     - Проверены запросы `preferredFrameRateRange` (120, 60, 40, 30 FPS): при наличии внешнего 75 Hz экрана WindowServer квантует частоту кратными субгармониками ($75/1 = 75$ FPS при запросе 120/60, $75/2 = 37.5$ FPS при запросе 40/30).
     - Переменная нагрузка (0.1–18 ms GPU и 4–8 ms CPU): при GPU нагрузках $\le 6$ ms дедлайны удерживаются со 100% стабильностью; при $\ge 10$ ms фиксируются единичные пропуски дедлайна с переходом на субгармонику 37.5 FPS.
   - **Ограничение интерпретации для Sol:** Данные probe являются **источником экспериментальных наблюдений, а не догмами поведения macOS**. Запрещено без дополнительной проверки утверждать, что:
     - внешний 75 Hz монитор безусловно принудительно ограничивает встроенный XDR во всех режимах;
     - `CAMetalDisplayLink` сам по себе волшебно устраняет микрофризы (он лишь выдаёт временные метки планировщика);
     - `preferredFrameLatency = 1` доказанно лучший режим для игрового цикла X-Ray;
     - `maximumDrawableCount = 3` гарантированно улучшает pacing.
     Sol должен использовать сохранённую телеметрию для A/B сравнения текущего presentation path игры с `CAMetalDisplayLink` в E01/E04/E08.

4. **Статус HDR / EDR:**
   - Чтение параметров EDR (headroom, color space, dynamic range limits) уже работает через `DisplayCapabilities`.
   - Сам рендеринг HDR/EDR в игре **НЕ реализован**.
   - Полноценный HDR (формат слоя `RGBA16Float`/`RGB10A2Unorm`, расширенное цветовое пространство `kCGColorSpaceExtendedSRGB`, `wantsExtendedDynamicRangeContent = YES`, tone mapping, LUT/exposure interaction, SDR fallback, скриншоты) выносится в отдельную post-parity задачу после полного закрытия базового SDR Windows-паритета (E06).

## 3. Правила исполнения и экономия токенов

- Не менять внешнюю архитектуру R4 и не начинать полный rewrite/Metal 4 migration ради предполагаемого ускорения. Текущий backend использует обычные `MTLCommandBuffer`/encoders; инструкции Metal 4 применять только при фактической работе с Metal 4 API.
- Сохранить пользовательский WIP. Перед этапом записать статус и diff затрагиваемых файлов. Запрещены массовые reset/checkout/stage/format. Коммиты и push не требуются этим планом; при отдельной просьбе stage только изменения этапа, учитывая уже существующий diff в тех же файлах.
- Не создавать чистый worktree от HEAD для WIP-задачи: он потеряет актуальную реализацию. Для изоляции нужен явно подготовленный снимок текущего дерева, включая необходимые untracked файлы. По умолчанию один рабочий checkout и один интегратор.
- Один Sol владеет `IR4Backend.h`, `MetalR4Backend.mm`, `MetalR4EngineScene.mm`, `MetalBootstrap.mm`, центральными CMake/launcher и frame contracts. Terra получает закрытый список файлов и готовый интерфейс.
- Максимум **две Terra одновременно**, только независимые полезные задачи. Один build/cook/game/GPU capture за раз. Параллельные агенты не запускают одну игру, не перезаписывают shader bundle и не меняют общий профиль.
- Terra самостоятельно выполняет короткую однозначную задачу. Sol нужен для frame ownership, temporal math, skinning ABI, pass graph и интеграции. Sol + Terra нужен для повторяемого переноса уже определённого контракта и независимых отчётов. Распределение ниже — проектное решение, не утверждение о ценах моделей.
- Не включать agent-router/внешний Gemini: запрошены Sol и Terra. При создании агентов явно выбирать Terra доступным инструментом, не угадывать недоступное имя модели. Если нужная модель недоступна, сообщить и оставить пакет задач, не тратить незаметно более дорогую модель.
- Для каждой сессии читать этот раздел, **один активный этап**, его прошлый короткий handoff и нужные функции. Не перечитывать все исторические документы и многотысячный backend целиком.
- Два неудачных цикла одной гипотезы у Terra → передать Sol минимальный reproducer, diff и 30–50 строк evidence. Не отправлять полный лог и не продолжать серию случайных настроек.
- Сначала короткий self-test/smoke затронутого контракта; полную матрицу — на интеграционных gates. Повторять успешные тесты только после релевантного изменения.
- После первого запуска измерений работать по наиболее дорогому **доказанному** участку, фиксируя before/after. Нельзя назвать отключение эффектов оптимизацией паритетного режима.

Создать в E00 `docs/direct-metal-release/STATUS.md`: этап, code/test/runtime/visual/perf статусы отдельно, текущий пакет, блокер, следующий шаг, ссылки на evidence. Подробные результаты хранить в `scratch/release_runs/<run-id>/`, а маленькие итоговые таблицы и манифесты — в docs. Не делать старые логи единственным источником истины.

### Назначение этапов

| Этап | Кто выполняет | Зависимость | Результат |
|---|---|---|---|
| E00 | Terra; Sol проверяет manifest gate | Сейчас | Воспроизводимые fixtures и честный baseline contract |
| E01 | Sol + Terra для анализа отчётов | E00 | Timings, GPU/CPU attribution и исходный бюджет |
| E02 | Sol; затем Sol + 2 Terra для миграции адаптеров | E00, минимальные timings E01 | Immutable данные draw/pass/frame, без гонок |
| E03 | Sol + Terra для census/probes | E02 | Устранение светлых полигонов и пурпурных дефектов |
| E04 | Sol | E02–E03 | Исправленные motion/history/camera; визуальная проверка V1 |
| E05 | Sol + 2 Terra | E04 | Стабильные тени, свет травы/NPC, базовые материалы |
| E06 | Sol + 2 Terra по отдельным пакетам | E05 | Windows pass parity, вода/небо/погода/post; проверка V2 |
| E07 | Sol; Terra пишет probes/отчёт | E01, E02, исправленная измеряемая сцена | NPC performance по подтверждённому bottleneck |
| E08 | Sol + Terra по одному bottleneck | E01; целевые passes исправлены | GPU/CPU/streaming бюджет 60 FPS |
| E09 | Terra; Sol разбирает найденные дефекты | E06–E08 | Длительная стабильность, input/UI/save/resize |
| E10 | Terra; Sol интегрирует | E09 | Повторяемая сборка, штатный launcher и fallback |
| E11 | Sol | Все предыдущие | Итоговая матрица, проверка V3, решение о готовности |

E07/E08 допустимо начать до полного завершения E06 для уже корректных сцен. После добавления недостающего прохода повторить budget. Не откладывать все замеры до полного паритета, но и не принимать ускорение незавершённого рендера за финальное.

## 4. E00 — воспроизводимость и эталон (Terra, review Sol)

**Вход:** этот план; `scratch/run_visual_parity_harness.sh`, `scratch/zzzz_visual_parity_harness.script`, `scratch/compute_parity_metrics.py`, текущий manifest; оба launcher; runtime README и profile manifest.

1. Сохранить hash/mtime исполняемого файла, dirty patch и список untracked, build options, DXC/MSC версии, receipts/hashes **действительно загружаемых** metallib, эффективный MO2 manifest и `user.ltx`. Полный profile не копировать в каждый отчёт; достаточно hash + resolver identity. В качестве единого источника фактических параметров окна, экрана и кадра использовать готовую инфраструктуру `DisplayCapabilities` (`DisplayCapabilities.h/.mm`, таргет `XRay.Metal.DisplayCapabilitiesProbe`): backing scale, physical/points window size, `CAMetalLayer.drawableSize`, internal render resolution, refresh rates/intervals, layer pixel format и VSync/presentation state. Это гарантирует отсутствие догадок о фактическом разрешении буферов и исключает неверную интерпретацию Retina как мультипликатора нагрузки. Проверить, нет ли другого работающего исполнителя в тех же файлах.
2. Исправить admission comparator: отсутствующий save hash, иная карта/погода/камера/FOV/aspect/внутренний размер/пресет/версия материалов → `INVALID_COMPARISON`, без итогового PASS. Сравнивать actual state на кадре, не только желаемые аргументы launcher. Фактические размеры render targets сверять с `DisplayCapabilities`. Начальные допуски для камеры: 1e-4 m и 1e-4 rad; матрицы 1e-5 при равном clip range. Если replay даёт больше — сначала исправить replay.
3. Зафиксировать время, weather preset **и blend weight/параметры**, random seed где доступен, exposure warm-up; убедиться, что скрипты не меняют их после установки. В динамическом тесте воспроизводить временную линию в секундах, не в номерах render frames: 20 и 60 FPS должны проходить одинаковый маршрут за одинаковое время.
4. Развести профиль/сохранения тестов от обычной игры, unique run directory и IPC-файлы вместо общих `/private/tmp/parity_*`; backup/restore в `trap` и безопасное восстановление прерванного запуска. Учитывать symlink MO2: запись по ссылке не должна изменять оригинальный mod. Команды завершения — только PID своего запуска. Текущий `run-gamma-metal.command` сам останавливает прежний процесс того же binary: не вызывать его поверх пользовательской сессии.
5. Объединить источник shader paths для штатного запуска и harness. Сейчас наборы окружения отличаются; фиксировать effective env и пер-проходный active/disabled/rejected reason. Не добавлять новые зависимости от случайного содержимого shell.
6. Сделать scripted screenshots и 10–20 s motion capture. Если захват недоступен по системному разрешению, запросить только конкретное разрешение/действие, остальные проверки продолжить. Скриншот должен содержать известный framebuffer crop, UI/chrome не сравнивать как часть сцены.
7. Подготовить fixtures ниже из существующих сохранений. Не считать имя save гарантией локации — проверять загруженный level. Если конкретная сцена ещё не достигнута, создать тестовое сохранение отдельным именем агентом; не переписывать пользовательский autosave.

| Fixture | Содержание | Назначение |
|---|---|---|
| S0 | Место текущего скриншота на Болотах: постройки, трава, дальние объекты; определить точную камеру | Corruption, baseline и исходный пользовательский дефект |
| S1 | Та же сцена: pan ±90°, резкий 180°, strafe/ходьба вперёд-назад, остановка камеры | Lag, lighting popping, culling, temporal reset |
| S2 | Группа NPC: 0/1/4/8 видимых при одинаковом фоне; несколько одинаковых моделей с разными позами | Cost curve skinning/shadows; instance identity correctness |
| S3 | Вода Болот, берег, отражение неба/растений, поворот и шаг к воде | Water SSR/refraction/depth/history |
| S4 | Bar/Garbage: плотная растительность, дальность, переход через shadow splits | Sun cascades, grass LOD, worst outdoor frame |
| S5 | Подземелье, фонарь, движущийся NPC, несколько локальных источников | Shadow atlas, point/spot, dark materials, overflow |
| S6 | Ночь/дождь/гроза + переход clear→rain→clear | Clouds, fog, wetness, thunder, exposure, particles |
| S7 | Оружие/ADS/3DSS/NV/thermal, стрельба и перезарядка | HUD view, второй view/targets, post, gameplay |

**Эталоны:** A — настоящий Windows D3D11 с тем же профилем; B — Windows binary через CrossOver/GPTK (если доступен); C — native DXVK/MoltenVK; D — Direct Metal. B/C — полезные proxy, не заменяют A в утверждении «доказан Windows-паритет». ИИ сам ищет доступные локальные исходные assets, runtime, старые валидные captures. Если A недоступен, один объединённый запрос пользователю на согласованные reference captures; до этого и параллельно исправлять доказанные дефекты и C/D. Сравнивать FPS с Windows на другом GPU как эффективность backend нельзя; отдельно фиксировать пользовательский исходный опыт и same-machine comparison.

**Автотест:** синтетические manifest пары — ровно один различающийся параметр/пустой hash/иной shader set; comparator обязан отвергать. Три повторных reference screenshots определяют шум внутри одного backend. MAE/PSNR — диагностика; не усреднять большой локальный дефект по всему кадру и не маскировать его «noise floor».

**Выход:** manifest schema + валидная C/D пара, fixture registry, evidence source A/B/C для каждой сцены, команды воспроизведения, STATUS. Если A нет — Windows gate OPEN, остальные этапы продолжаются.

## 5. E01 — честные timings и бюджет (Sol + Terra)

Sol меняет backend/engine instrumentation; Terra независимо готовит parser и итоговые таблицы по утверждённой схеме.

**Точки:** `MetalR4Backend.mm::BeginFrame/SubmitFrame`, `MetalRenderDeviceRender.mm::End`, `MetalR4EngineScene.mm::Render`, `MetalBootstrap.mm`, `MetalR4DynamicWorldAdapter.cpp::Prewarm`, DetailGrass StageFrame, `src/xrEngine/device.cpp`, `Stats.cpp`, `FTimer.h`, engine animation/AI/scheduler.

1. CPU monotonic timestamps: input/update, AI/physics/scripts, skeleton calculation, capture/scene build, CPU skinning, grass repack, resource upload, command encoding, semaphore wait, drawable acquisition, commit. Развести nested timing и self time.
2. GPU: completion timestamp pair командного буфера; per-pass timings через доступные на этой машине counters/Metal System Trace. Подписать encoders G-buffer, sun near/far, local shadow, local lights, AO, IL, water, rain, luminance/combine, HUD/scope, present/readback. При отсутствии counters не выдавать CPU encode time за GPU time.
3. Отдельно записать scheduled/completed serial, engineFrame/cameraFrame, queue depth, displayed drawable timestamps (если API доступен) и dropped/repeated frames. Callback arrival time не равен on-screen time. Для анализа очередей и presentation scheduling использовать готовую сырую телеметрию probe (`scratch/cametaldisplaylink_samples.csv`, `scratch/cametaldisplaylink_summary.json`, таргет `XRay.Metal.CAMetalDisplayLinkProbe`), показывающую соотношение `targetTimestamp` (дедлайн завершения кадра), `targetPresentationTimestamp` (ожидаемый показ на экране) и поведение при задержках. Если показ кадров не измерен — throughput можно оценить, pacing gate OPEN.
4. Counters: visible/captured/drawn/skinned vertices и NPC, casters на light/cascade, draws/PSO switches, bytes updated, buffer/texture allocations, cache misses/evictions, readback bytes, residency/RSS, pipeline compilation events. Лог выводить пакетами, исключить synchronous flush каждый draw.
5. Validation и capture запускать отдельно от performance. В normal Metal launcher validation-переменные могут наследоваться; записывать фактический env, timing runner явно очищает diagnostic env в своём процессе. `rs_stats` off для timing.
6. 60 s прогрев либо до прекращения streaming/compilation, затем 3×120 s S0/S2/S4/S5. Контрольный 10–20 s CPU sample/Time Profiler и Metal System Trace на узком участке. Никаких параллельных builds/GPU captures в измерении.
7. Проверить Stats/таймеры на неинициализированные значения, непарные Begin/End, несовместимый формат, разные clock units/underflow. Причина гигантских значений пока не известна. Не чинить физический timestep по неправдоподобной статистике.

**Выход:** `baseline.json`, `frames.csv`, `cpu_top.md`, `gpu_passes.csv`, `budget.md` с 3 главными затратами и уверенностью. Бюджет выдаёт порядок E07/E08; никакой заранее выбранной «магической» оптимизации.

**Проверка:** overhead telemetry on/off ≤2% median в повторных прогонах; если выше — облегчить сбор или честно пометить intrusive. Parser проверяется на известном наборе интервалов, включая пропуски, NaN/нулевые значения и p99/1% low.

## 6. E02 — исправить ownership данных GPU (Sol, затем две Terra)

**Это первый кодовый P0.** Можно начинать после минимального воспроизведения E00 и базовых frame timings, не дожидаясь всех trace E01.

Sol сначала пишет короткий `FRAME_DATA_CONTRACT.md` и минимальный доказательный GPU test. Затем реализует общий механизм и только после этого раздаёт миграции.

**Обязательный контракт:**

- CPU не изменяет память, которую читает незавершённый GPU frame. Uniform/vertex/instance data живут в слоте фактически приобретённого `ActiveFrameContextIndex`, а не `engineFrame % 3`.
- Разные draws/passes/views внутри одного command buffer получают **разные immutable ranges**, если содержимое отличается. Повторный `UpdateBuffer` по тому же адресу до commit не допустим даже при одном кадре in flight.
- Предпочтительный путь: per-frame upload arena с выровненными suballocations для constants и transient vertices; allocation возвращает buffer+offset+size, MSC TLAB указывает именно этот slice. Arena сбрасывается только после completion соответствующего context; spill blocks тоже принадлежат слоту. Постоянные mesh/texture ресурсы кэшируются отдельно.
- Backend-level snapshot констант при binding допустим как узкий safety patch, если он действительно копирует bytes в arena и измерен; он не защищает динамические vertex buffers. Sol выбирает один общий путь и исключает двойное копирование при последующей миграции.
- Reference retention обеспечивает lifetime, а versioned slices — неизменность данных. Нужны оба условия. `useResource` не является копированием/синхронизацией CPU writes.
- При skipped acquisition/failed encode/reset не продвигать history как успешно показанный кадр. Submission serial и engine frame — разные сущности.
- Общие render targets/history на одной serial queue не нужно автоматически утраивать: проверить GPU read/write order, attachment load/store, hazard tracking и отсутствующее aliasing. CPU constants — отдельная проблема.

**Порядок:**

1. Воспроизвести AO/IL overwrite: закодировать AO constants A, затем IL B и четыре blur strengths, проверить readback каждого результата; текущий код не должен проходить тест. Второй тест — обновление камера/цвет A/B/C при нескольких кадрах в работе.
2. Sol исправляет `IR4Backend.h`, backend arena, bindings и AO/IL per-pass vertices/constants. Нельзя считать `MaxFramesInFlight=1` решением: он не устраняет overwrite внутри command buffer. Однокадровый режим — только diagnostic A/B с неизменной correctness нагрузкой.
3. Terra A мигрирует Flora + DetailGrass на готовый API. Terra B проводит read-only census остальных `UpdateBuffer` callsites и переносит отдельно назначенные небольшие адаптеры. Sol сохраняет владение EngineScene/Bootstrap и интеграцией.
4. Покрыть terrain/static, sun/local-light/shadow, sky/clouds, rain/lens flare, HUD/particles, postprocess/fullscreen. Проверить multiple views 3DSS. DynamicWorld/Water имеют slots, но должны также пройти аудит уникальности per-pass updates.
5. Не обходить ошибку глобальным `waitUntilCompleted` каждый кадр, отключением AO/IL или уменьшением глубины очереди без доказательства. Diagnostic patch удалить или оставить opt-in, production остаётся asynchronous.

**Автотесты:** два draw с разными transform/color до одного submit → оба правильны; 3–6 последовательных кадров без immediate wait → каждый имеет свой pattern; forced slot reuse после completion; разные blur parameters; resize/acquire retry/failed frame без stale history; проверка bounds/alignment и переполнения arena. Readback ждать только в тесте, не в обычном frame loop.

**Gate:** новые tests сначала воспроизводят старую ошибку, затем проходят; API/GPU validation clean, memory bounded; S1 записан без corruption. Сравнить FPS до/после, но визуальное закрытие ещё не объявлять: дальше E03/E04.

## 7. E03 — локализовать светлые полигоны и пурпурные области (Sol + Terra)

**Не назначать причину по цвету.** Крупная форма может принадлежать static/dynamic mesh, sprite, flare, shadow/combine или некорректным constants.

1. На S0 получить frame capture и координаты проблемных пикселей. Найти первый pass, где артефакт появляется: position/normal/albedo → lighting → forward/weather → final. Для этого добавить opt-in dumps промежуточных targets/object-material ID; в final mode этих расходов нет.
2. Из capture определить draw ID, visual/asset/material, index count, baseVertex/index offsets, stride/attribute formats, matrices и texture identities. Последовательно исключать **одну** семью draws только для локализации; финальная картинка должна включать всё.
3. Для geometry: проверить finite positions, bounds, index range, OGF links/LOD, правильный формат 1/2/3/4W, matrix convention и 16-bit indices/base vertex. Для billboard/flare: camera-facing basis, FVF decode и clip-space semantics.
4. Для пурпурного: материал и каналы DDS/BC decode, bump/bumpX, sampling, missing texture fallback, shader variant и debug fill. Сопоставить exact MO2 winner через `MetalR4EngineFs` и `R4DdsTexture`; `_bump` missing сам по себе не доказывает corruption.
5. Исправить конкретный контракт и добавить fixture offending asset (или metadata/hash без копии лицензированных assets). Terra готовит geometry/texture probe; Sol делает runtime fix.

**Gate:** на S0/S1 дефект отсутствует с включёнными production passes; причина описана цепочкой «draw → неверные данные → исправление → capture». Нельзя скрыть объект/caster/material для PASS. После фикса прогнать тот же ресурс в другой сцене и несколько экземпляров.

## 8. E04 — motion, camera и temporal history (Sol)

**Точки:** `device.cpp` (`SetCacheXform`, `mView_prev`, `mView_saved`), `MetalRenderDeviceRender.mm::End/SetCacheXform/SetCacheXform_prev`, capture в `R_calculate.cpp`, `r__dsgraph_build.cpp`, `r__dsgraph_render.cpp`, `IR4Backend.h`, все adapter view structs, AO/IL/Water history.

1. Добавить только diagnostic frame stamps к camera, world, light, detail, HUD, bones и environment snapshots. Утвердить, какие данные обязаны быть same-frame, какие simulation updates правомерно переиспользовать. Не отбрасывать статический snapshot только потому, что он старше camera frame; проверять generation/validity по смыслу.
2. На S1 сравнить screen-space проекцию стабильных мировых точек между CPU матрицей и G-buffer, со скриптовой камерой без сглаживания и с обычным input. Для каждого draw current view/projection + current transform должны относиться к одному render state; previous нужны только соответствующему temporal алгоритму.
3. Разделить четыре симптома: целая сцена запаздывает = queue/input/pacing; отдельные классы «едут» = mixed snapshots/buffers; остающийся шлейф = temporal history/reprojection; исчезновение целых объектов = culling/LOD. Снимать видео и target dumps, а не лечить всё «motion vectors».
4. Проверить stable object identity, pose/history на spawn/despawn, два NPC одного asset, pause/reload/teleport, FOV/ADS/3DSS, resolution change. Сброс history на camera cut, discontinuity, level/device generation. First frame: previous=current, history valid только после успешного producer.
5. AO/IL: правильное previous VP × inverse current view, depth/position convention, UV Y и viewport scale, bilinear/clamp, disocclusion/edge rejection ровно как Windows shader. Water: правильный SSR history producer/copy order. Не добавлять TAA/MetalFX для маскировки рассинхронизации.
6. После correctness измерить очередь: сравнить ограничение 2 и 3 in-flight при одинаковом качестве и отсутствии data races. Использовать телеметрию `CAMetalDisplayLinkProbe` (`cametaldisplaylink_samples.csv` и `cametaldisplaylink_summary.json`) для понимания presentation lead times: в windowed-режиме macOS target presentation lead time составляет ~39.9 ms (~3 кадра при 75 Hz), а target deadline lead time — ~13.2 ms (~1 интервал развёртки). Latency mode 1 vs 2 задаёт более ранний target deadline (~13.2 ms vs ~26.5 ms), но реальный pacing определяется тем, укладывается ли GPU encode/render в дедлайн; при всплесках нагрузки (>=10 ms) частота квантуется (75 -> 37.5 Hz). Данные probe — экспериментальная телеметрия, а не универсальное правило macOS: Sol не утверждает безоговорочно превосходство latency=1 или 3 drawables, а проводит A/B замер для X-Ray, чётко разделяя software queue latency и display presentation scheduling.

**Gate:** CPU/GPU reprojection test в пределах 0.5 pixel для неподвижных точек без jitter; отсутствие систематического одно- или многокадрового сдвига по frame stamps; движение/остановка без отставания частей сцены и trails сверх reference. Пройти V1 из раздела 16. Без ответа пользователя — `AUTO PASS / VISUAL PENDING`, а не закрытая приёмка.

**Пакет 2 audit repair, 2026-09-27 — STATIC MOTION/HISTORY VERIFIED:** LMH/terrain/DefVertex/DefObjectsLod получают previous camera последнего accepted producer. Фактический converted G-buffer turn/pure pan ≤0.127 px; cut/return, reload, resize, acquire Retry, discarded/cold encode и pre-encode second-view rejection прошли на финальном Bar diagnostic exit0 с API/shader validation. AO/IL clear/copy/source ownership проверены. S1 Device clock исправлен; precache rotation не засчитывается как scripted pan. NPC previous bones/identity generations, Windows/V1 и очередь/pacing остаются открытыми. [Доказательства](docs/direct-metal-release/E04_PRODUCTION_MOTION_HISTORY.md).

## 9. E05 — стабильный свет, трава, NPC и тени (Sol + две Terra)

Sol владеет shadow/environment contract и центральными файлами. Terra A — detail/flora snapshots и material probes; Terra B — CROS/material identity + проверка NPC/texture census, без параллельной правки DynamicWorld с Sol.

1. Повторить S1/S4 после E02. Зафиксировать выбранный куст/пучок травы по world ID. Записать sun/hemi/normal/material, slot ID, shadow matrices, AO/IL и final luminance на маршруте туда/обратно.
2. Поочерёдные diagnostic режимы G-buffer, direct sun, hemi, local lights, shadow factor, AO, IL определяют первый скачущий сигнал. Видимый final flicker не доказывает, что ошибка именно в тенях.
3. DetailManager → detail snapshot: persistent instance identity, slot coordinates, terrain normal, packed sun/hemi unpacking, cache generation, per-instance fade, wind/bender time. Камера может менять видимость/fade, но не произвольно переприсваивать свет соседнего слота.
4. Flora: normal/tangent/BGR packing, double-sided normal/cull/AREF, base alpha, mip chain и bumpX. Не заменять весь лес общим ambient multiplier и не объявлять все dummy bump ошибкой без MO2 lookup.
5. Sun: сравнить Windows cascade graph/clip ranges/split blend/texel snapping/caster sets и depth bias. В текущем EngineScene видны near/far проходы, в профильных настройках — три SSFX cascade distances: провести семантическое сопоставление, а не механически добавить третью карту. Shadow geometry/alpha/wind должны соответствовать visible geometry. Проверить, что shadow frustum включает нужные off-camera casters.
6. Local shadows: сохранить динамические casters, стабильные IDs/приоритеты и selection при atlas overflow; сравнить point/spot/omni-face semantics. 16 slots и skip лишних lights — не автоматический parity PASS. При изменении atlas selection не должно мигать освещение видимых объектов.
7. NPC: сопоставить CROS hemi/sun/hemi-cube и matrices на теле, оружии и attached meshes; не наследовать material от предыдущего персонажа. Отдельно ноги от первого лица.

**Gate:** выбранная статичная трава не чередует свет/тьму при шаге без реального изменения освещения; границы shadow splits и смена atlas selection не дают вспышек; reference crops материалов совпадают по семантике. Автотесты slot unpacking/cache move/cascade bounds/atlas stability и короткий validation run. Визуальную оценку травы объединить с V2, если промежуточный ролик не требует отдельного решения.

## 10. E06 — закончить Windows pass parity (Sol + две Terra)

> [!NOTE]
> **Предварительное исследование (E06 Prework) завершено:**
> Полный исследовательский discovery по перестановкам материалов и погодным эффектам уже выполнен в read-only режиме и зафиксирован в отчётах:
> - [`docs/direct-metal-release/MATERIAL_PERMUTATION_CENSUS.md`](docs/direct-metal-release/MATERIAL_PERMUTATION_CENSUS.md) (~30 категорий перестановок и путей рендера);
> - [`docs/direct-metal-release/WEATHER_EFFECTS_CENSUS.md`](docs/direct-metal-release/WEATHER_EFFECTS_CENSUS.md) (24 погодных, атмосферных и post-process эффекта).
>
> **Инструкция для Sol:** Sol **НЕ должен** повторять полный census с нуля. Результаты census передаются как готовый предварительный evidence. Все зафиксированные проблемы трактуются как `prework finding / confirmed code-path gap`. Перед реализацией каждого конкретного исправления Sol выполняет лишь короткую перепроверку актуального checkout/config/runtime. Census не является доказательством окончательного паритета — окончательный паритет подтверждается прохождением гейтов и эталоном Windows DX11.

Сначала Sol формирует `PARITY_MATRIX.md` на основе данных census: функция/активное условие Windows → источник HLSL/defines → Metal capture/producer → consumer/pass order/target format → admission → визуальное evidence. Статусы `implemented`, `runtime exercised`, `reference matched`, `human accepted` раздельные. Ни `draws>0`, ни успешный cook сами по себе не означают parity.

**Справочные источники:** `src/Layers/xrRenderPC_R4/r4_rendertarget_phase_combine.cpp`, соответствующие Windows `phase_*`, blenders и активные materialized HLSL в build/runtime. Прежде чем править граф, прочитать реальную ветку с текущими `o.*`/SSFX defines. Использовать исходный HLSL через DXC→MSC, reflection и receipts, без замены «похожим» MSL.

**Исполнение E06, 2026-09-27 — IN PROGRESS, не закрыт:** Gemini census переиспользован; два GPT-6 Luna leaf-agent и один интегратор. Реализованы water blur/wind contract, активная SSFX bloom chain, subset selflightl emissive target, точный256² luminance source/order, AO/IL blur-scale contract, rain DM/splashes/CPU policy, lens adapter+TGA logical identity. Build и5 probes PASS; final S6 API/shader validation exit0/zero reports; S3 lens source/overlay и S6 wet CPU/draw exercise подтверждены раздельно от image parity. Полное покрытие emissive, general SSR, volumetric/fog/DOF/material/HUD gaps и Windows/V2 остаются OPEN. [Execution/evidence](docs/direct-metal-release/E06_EXECUTION.md), [матрица](docs/direct-metal-release/PARITY_MATRIX.md). Следующий critical packet6D: оригинальный SSR HLSL блокируется DXC parser на unroll annotation; [конкретный compatibility proposal](docs/direct-metal-release/E06_SSR_COMPATIBILITY_PROPOSAL.md) проверен только в scratch и ожидает трактовки original-HLSL правила пользователем.

**Остановка по просьбе пользователя, 2026-09-27:** после исправления AO/IL bindings и live combine controls пользователь предварительно сообщил, что круги исчезли, но появились другие проблемы, и попросил свернуть работу. Исправленный source/binary сохранён; build/probe/Bar Metal validation PASS. Точное Street A/B, Windows и V2 остаются OPEN. Новые запуски и переход к другим этапам не выполнять без возобновления пользователем. [Состояние и handoff](docs/direct-metal-release/E06_CIRCLES_FIX.md).

### Приоритезация задач для Sol
Sol определяет приоритет устранения пробелов не по временным тегам предварительного отчёта, а по инженерным критериям:
1. Активная конфигурация текущего профиля (`user.ltx`, launcher).
2. Влияние на весь итоговый кадр (full-screen vs локальный эффект).
3. Соответствие эталону Windows DX11.
4. Частота появления эффекта в реальном геймплее.
5. Зависимости в центральном render graph (producer перед consumer).

**Рекомендуемая последовательность реализации:**
- **Этап 1 (Критичный пайплайн и композит):** Water known issues (6A) → Combine chain (6C) → Post-process Bloom & Emissive pipeline (6C) → Exposure/SDR output baseline (6C).
- **Этап 2 (Активные видимые проходы):** SSFX AO/IL/SSR верификация (6D) → Sky/clouds live execution (6B) → Sunshafts raymarching & composite (6E) → Volumetric lighting (6E) → Lens Flare adapter integration (6E) → Rain ecosystem (6E).
- **Этап 3 (Локальные и зависимые от конфига пробелы):** Dynamic wallmarks / bullet & blood decals (6F) → FLOD / Impostors (6F) → bump / bumpX fallbacks (6F) → HUD particles (6E/6G) → SSFX Fog Scattering (6E) → Depth of Field (6E) → Thunderbolt geometry (6E) → остальные опциональные пути.

Выполнять пакетами; одновременно максимум два независимых leaf adapter. Центральный graph меняет Sol после готовности обоих.

| Пакет | Владелец | Конкретная работа и gate |
|---|---|---|
| 6A: Water | Sol | **Аудит и стабилизация `MetalR4WaterAdapter`:** SSR, history copy, blur, waves, final transparency/refraction; правильный scene-color source, depth, scale, time, shoreline.<br>• *Prework finding:* генерация карты волн `ssfx_water_waves.ps` (512×512 RGBA8) подтверждена как `ACTIVE_VERIFIED` (`METAL_WATER_FRAME ... waves=512x512`).<br>• *Gate S3:* нет белого пласта, берег мягкий как reference, отражение следует сцене, нет старого кадра при pan. |
| 6B: Sky/clouds | Terra по контракту Sol | **Атмосферный проход `dxEnvironmentRender.cpp` + `drawCloudScene` (`clouds_msc`):** валидные texture pairs, vertex colors/alpha, geometry, rotation/time, blend state.<br>• *Prework finding:* проход неба `sky2_msc` полностью подтверждён (`ACTIVE_VERIFIED`, `METAL_SKY_DRAW` в каждом outdoor-кадре). Пайплайн облаков скомпилирован (`METAL_CLOUDS_PREPARED`), но в тестовых сейвах alpha=0 (`cloudsValid == false`). Sol проверяет на фикстуре с ненулевыми облаками. Не оставлять вечный skip на invalid capture. |
| 6C: Exposure/fog/output, Bloom & Emissive | Sol | **Композит `combine_1`/`combine_2`, Bloom и Emissive:**<br>• *Combine & Luminance:* G-buffer → `combine_1` → luminance adaptation (`bloom_luminance_1,2,3`) → `combine_2` → LUT (`lut_atlas.dds`) → output. Linear/sRGB decode/encode, black/gray/white ramps. Строго SDR baseline: соответствие Windows DX11 SDR. Полноценный HDR/EDR вынесен в post-parity backlog и НЕ блокирует E06.<br>• **Post-Process Bloom (`prework finding / confirmed code-path gap`):** в Windows R4 строится пирамида мипов и blur (`r4_rendertarget_phase_bloom.cpp`, `bloom_filter.ps`, `bloom_build.ps`). В Metal R4 вход `bloom` в `combine_2` (`DrawPostprocess`) сейчас привязан к 1×1 black dummy (`scene.lightingBlack`). Sol реализует пирамиду downsample/blur и подключает реальный bloom-таргет. Bloom проверяется и реализуется раньше косметических погодных эффектов.<br>• **Emissive Pipeline (`prework finding / confirmed code-path gap`):** в Metal R4 есть только телеметрия `ancillary_emissive`; отдельный render pass, буфер `rt_ssfx_bloom_emissive` и композит отсутствуют. Не рассматривать эмиссив отдельно: Sol связывает эмиссивные материалы с Bloom/Combine pipeline.<br>• *Gate:* корректный dynamic range, мягкие света вокруг ярких источников, отсутствие ступеней на градиентах, рабочий LUT. |
| 6D: AO/IL/SSR | Sol; Terra probes | **Завершение интеграции `MetalR4SsfxAoAdapter`:**<br>• *Prework finding:* 5 проходов AO + 5 проходов IL с temporal history подтверждены как `ACTIVE_VERIFIED` (`METAL_SSFX_AO_IL_FRAME`).<br>• *Задачи:* проверить actual settings/quality defines/scale, edge handling, history reset при camera cut и contribution в освещение. Факт пяти проходов не доказывает одинаковое качество. Проверить active general SSR отдельно от water SSR. |
| 6E: Weather/effects | Terra; Sol integration | **Ликвидация подтверждённых пробелов погоды и спецэффектов:**<br>• **Lens Flare (`prework finding / confirmed code-path gap`):** producer в `dxLensFlareRender.cpp` захватывает квады в `MetalLensFlareCapture.h`, но `MetalR4LensFlareAdapter` не инстанциирован в сцене, а его методы — пустые заглушки `return true`. Sol перепроверяет checkout, инстанциирует адаптер, аллоцирует буферы и вызывает draw во forward-проходе (`effects_sun.ps/vs`).<br>• **Rain Ecosystem (отдельные строки/гейты для каждого компонента):**<br>  - *Falling rain:* `MetalR4RainAdapter` (1/8 refraction prepass + forward sprites) реализован; Sol подтверждает исполнение на дождливой фикстуре S6 (`rain_density > 0`).<br>  - *Rain occlusion:* `prework finding / confirmed gap` (CPU culling работает, GPU depth/shadow projection отсутствует).<br>  - *Ground splashes:* `prework finding / confirmed code-path gap` (MISSING из-за раннего `return` в `dxRainRender.cpp:138`, срезающего цикл `DM_Drop` / `effects_rain_splash.ps`). Sol восстанавливает захват и отрисовку сплешей.<br>  - *Surface wetness:* `prework finding / confirmed gap` (скаляр `rain_params.y` пишется в `staticGlobals[464]`, но screen-space accumulation pass `draw_rain` отсутствует).<br>  - *Water waves / ripples:* `ACTIVE_VERIFIED` в `MetalR4WaterAdapter` (не смешивать со сплешами о землю).<br>  - *Thunder flash:* `ACTIVE_VERIFIED` через environment light modulation.<br>  - *Thunderbolt geometry:* `prework finding / confirmed code-path gap` (STUB/NO_OP, ранний `return` в `dxThunderboltRender.cpp:31`).<br>• **Sunshafts (`prework finding / confirmed code-path gap`):** активен в конфиге (`r2_sunshafts_mode volumetric`), но в `xrRenderMetal` полностью отсутствует. Выделить буферы `rt_sunshafts_0/1`, реализовать raymarching/blur pass. Приоритет определяет Sol по visual impact.<br>• **Volumetric Lighting (`prework finding / confirmed code-path gap`):** активен в конфиге (`r2_volumetric_lights on`), полностью отсутствует в Metal (слот в `MetalR4RainAdapter` привязан к 1×1 black dummy). Реализовать raymarching (`accum_volumetric_sun`, `accum_volumetric`) и `phase_combine_volumetric`.<br>• **SSFX Fog Scattering (`prework finding / confirmed code-path gap`):** активен в конфиге (`ssfx_fog_scattering 1.`), отсутствует в Metal. Не смешивать с обычным distance fog в 6C.<br>• **Depth of Field (DOF) (`prework finding / confirmed gap`):** отсутствует в Metal. Sol проверяет настройки (`r2_dof_enable on`, `r2_dof_radius 0.25`); если профиль не использует, допускается `DISABLED_BY_CONFIG` / deferred.<br>• **Motion Blur (`prework finding`):** в текущем профиле отключен (`r2_mblur 0.`, `r2_mblur_enabled off`). Не является блокером E06; config-dependent.<br>• **HUD Particles (`prework finding / confirmed code-path gap`):** подтверждён explicit fail-closed reject в `MetalR4ParticleAdapter.cpp:474` для `record.hudMode`. Проверить актуальность на S7.<br>• **Fullscreen Heatvision / Nightvision (`prework finding`):** не смешивать scope thermal, fullscreen heatvision и nightvision. Nightvision готов (3 blur levels + gen 1..3), dormant при `r__nightvision 0`. Heatvision postprocess отсутствует, dormant при `r__heatvision 0`. Проверять активацию до реализации.<br>• *Gate:* Clear/rain/night/thunder fixtures обязательны. |
| 6F: Materials/permutations | Terra | **Ликвидация подтверждённых пробелов материалов и геометрии:**<br>• *Prework finding:* в census выявлено ~30 категорий перестановок материалов; Sol опирается на `MATERIAL_PERMUTATION_CENSUS.md`.<br>• **FLOD / Impostors (`prework finding / confirmed code-path gap`):** подтверждено отсутствие полноценного Metal FLOD/impostor path (distant LOD/impostor rendering не соответствует Windows; шейдер/путь отсутствует или отбрасывается, что приводит к исчезновению дальней растительности). Sol проверяет фактический текущий vertex layout (не фиксируя 76 vs 80 bytes как догму) перед реализацией. Добавлено как подтверждённый gap, а не просто «assessment».<br>• **Dynamic Wallmarks / Decals (`prework finding / confirmed code-path gap`):** статические декали частично работают, но dynamic gameplay wallmarks (следы пуль, кровь, взрывы) не доходят до Metal-рендера. Добавить отдельную проверку и реализацию пути отрисовки.<br>• **bump / bumpX Fallbacks (`prework finding / confirmed gap`):** неполная поддержка bumpX, dynamic-world fallback в flat, поведение не всегда соответствует Windows dummy bump fallback. Обязательная проверка для Sol: `missing texture vs missing shader permutation vs MO2 resolver vs case mismatch vs deliberate dummy bump`. Не лечить все missing bump одинаково.<br>• **AREF / Alpha-test Permutations:** пороги 200/201/202/203, grass dither, flora two-sided culling.<br>• *Gate:* нет required material/pass rejection на тестовых локациях. |
| 6G: HUD/scopes/UI | Sol при отклонении | **Интерфейс, оружие и прицелы:**<br>• 3DSS weapon scopes (PIP reticle pass подтверждён в census как `ACTIVE_VERIFIED`).<br>• Матрицы отдельного view, ADS transition, UI alpha, координация с HUD particles (6E).<br>• Старые тесты не заменяют проверку после изменений frame ABI. |

**Место MetalFX Spatial Scaler в пайплайне (working hypothesis для post-parity / upscaling):**
MetalFX Spatial Scaler исследован и подтверждён автономным тестом (`MetalFXSpatialScalerProbe`, ~1.1 ms на M1 Pro для 720p/900p → 1080p). Рабочая гипотеза размещения скейлера в графе рендера:
`3D сцена (low-res) → postprocess / tonemap → MetalFX Spatial Scaler → native UI / HUD overlay → CAMetalLayer present`.
Финальное решение о включении и интеграции Sol принимает после закрытия parity E06. Скейлер аппаратно поддерживает Dynamic Resolution Scaling (DRS) без пересоздания объекта через `inputContentWidth` / `inputContentHeight` (DRS остаётся опциональным). MetalFX является опциональной надстройкой и НЕ может подменять выполнение нативного performance gate 1080p60 в E08.

Компиляцию ограничивать изменённым bundle. Не менять compiler version в том же A/B, что rendering fix. Для каждого bundle сохранить inputs/hash/defines/reflection/outputs и сверить launcher. При MSC работе исполнителю загрузить навыки `compiling-with-metal-shaderconverter` и `integrating-metal-shaderconverter-shaders`.

**Gate V2:** полная active-feature матрица, same-state reference crops + motion S1/S3/S4/S6/S7. Нет required material/pass rejection. Если эталон Windows отсутствует, честный статус `PROXY MATCH / WINDOWS OPEN`; работоспособность и performance продолжают проверяться.

## 11. E07 — просадки при NPC (Sol, Terra для probes)

**Результат 2026-09-28: BRANCH B / PERFORMANCE OPEN.** Добавлена opt-in диагностика и выполнены S2 прогоны 0/1/4/8 добавленных NPC; CPU skin ~4–6 ms против ~36 ms local-shadow CPU и ~8 ms sun-shadow CPU. Это диагноз, не улучшение FPS. Точный on-screen count, individual skeleton/AI и GPU pass timings остаются открыты; CPU oracle сохранён. Дальнейшая оптимизация относится к E08 и здесь не выполнялась. [Полный отчёт](docs/direct-metal-release/E07_NPC_PERFORMANCE.md).

Сначала таблица S2: NPC visible/alive, bones, submitted/skinned vertices, pose cache hits, CPU skin ms, skeleton/AI ms, shadow caster ms, GPU shadow/G-buffer ms, memory. NPC издалека/за камерой и NPC без отображения использовать только диагностически. Отключение AI или теней должно быть ограничено fixture и не оставаться в gameplay.

**Ветка A: CPU skinning доказан дорогим.**

*Входной факт из E06 prework:* предварительное исследование материалов ([`MATERIAL_PERMUTATION_CENSUS.md`](docs/direct-metal-release/MATERIAL_PERMUTATION_CENSUS.md)) подтвердило, что в текущем Metal-пайплайне геометрия world NPC `SKIN_1/2/3/4` в значительной мере/полностью преобразуется CPU-веткой в `SKIN_0`-совместимую геометрию. Это **НЕ** является блокером визуальной корректности E06 при визуальном совпадении кадра с Windows, но является ключевой исходной информацией для E07: Sol начинает E07 с прямого профилирования и измерения стоимости этого CPU skinning по уже известному бейзлайну, не тратя время на повторное установление факта его существования.

1. Сохранить текущий CPU path как oracle. Сопоставить vertex layouts и `skin.h` для SKIN_0…4; нынешний CPU-pretransformed SKIN_0 не считать настоящим GPU skeletal path.
2. Загрузить неизменяемую mesh geometry один раз; передавать current/previous palette на instance в immutable per-frame slices. Реализовать реальные MSC SKIN_1/2/3/4 variants и transforms/normal/tangent семантику. Альтернатива compute-skin-once для нескольких shadow/G-buffer passes допустима только после сравнения затрат bandwidth vs повторного vertex skinning. Не делать оба решения сразу.
3. Не перепутать веса 2W (`lerp` вторым весом), implicit последнего веса, packed bone indices, matrix order. Проверить CROS/material и object identity отдельно от asset identity.
4. Одна pose evaluation на simulation update, reuse для нужных проходов без stale pose. Animated bounds должны соответствовать culling; off-camera shadow casters нельзя удалить ради FPS.
5. CPU oracle vs GPU readback на 1/2/3/4W, нулевых/крайних весах и разных pose: position error в масштабе asset с заданной погрешностью, normals/tangents/angular error; затем несколько одинаковых NPC с разными анимациями в S2/S5. Допуски зафиксировать до теста, не ослаблять под результат.

**Ветка B: shadows/GPU доминируют.** Перейти к E08 shadow work: per-light caster culling, неизменяемая геометрия, reuse только при валидных generation keys; не переходить к GPU skinning просто потому, что это современнее.

**Ветка C: AI/scheduler/physics/scripts доминируют.** CPU profile по видимым/невидимым NPC; проверить repeated work/lock contention/over-log и port-specific slow paths. Не менять игровой интеллект, simulation frequency и физику для выполнения графического benchmark. Большой engine concurrency redesign — отдельное решение Sol с reproducer, не задача Terra «ускорить всё».

**Gate:** measured cost curve 0/1/4/8 NPC улучшилась за пределами шума, нет ошибки анимации/света/теней, общей CPU/GPU регрессии и stutter; итоговая S2 всё равно должна пройти 60-FPS gate. Если выгоды не доказаны, не усложнять production path; хранить результаты неудачного эксперимента.

## 12. E08 — довести время кадра и ровность (Sol + Terra)

**Результат 2026-09-28: IMPLEMENTED / NATIVE60 OPEN.** Штатная сборка использует принятые оптимизации; значимый общий выигрыш подтверждён контролируемым S2 comparison, дополнительный static merge — S4 A/B/A. Финальная матрица S0/S2/S4/S5/S6/S7 выполнена по одному 120-секундному settled окну, native1920×1080: медианы 24/30/24/40/30/40 FPS. Это не три повтора и не pacing PASS; частичные presentation callbacks явно помечены. Нет уменьшения качества, NPC, разрешения или частоты поз/симуляции. По пользовательскому условию остановки обычные подтверждённые следующие улучшения с заметной отдачей не найдены. E09 не выполнялся. [Полный отчёт](docs/direct-metal-release/E08_PERFORMANCE.md).

Работать по E01 `budget.md`: один bottleneck → одна гипотеза → bounded patch → A/B → следующий. After/before брать в одной термальной/энергетической конфигурации, чередовать A/B; записывать idle/warm/cold отдельно.

| Измеренный bottleneck | Порядок действий | Запрещённая подмена |
|---|---|---|
| CPU allocation/upload/encoding | Считать allocations/bytes; persistent VB/IB, reuse textures/PSO, arena pooling; убрать повторный resolver/texture read/hash на draw, batch одинаковые states; проверить окупаемость cache. Для травы geometry/model reuse и update только изменившихся slot/instance данных. | Скрыть траву или реже обновлять позы без reference-контракта. |
| Shadow GPU/draw count | Проверить caster visibility по light/cascade; убрать дубли/пустые passes, stable light selection; cache только static casters при неизменном свете/геометрии, отдельно dynamic, с полной invalidation. | Выключить NPC/grass shadows, уменьшить дальность или resolution против эталона. |
| Fullscreen AO/IL/water/post | Сначала корректные UV/extent/format и реальные settings; затем eliminate redundant copies/unused passes, scoped half-res только если таков reference quality mode, корректное load/store и attachment lifetime. | Выключить IL/AO и назвать разницу оптимизацией. |
| Bandwidth/overdraw/material | Проверить GPU evidence, mipmaps/texture format, двойную отрисовку, early-depth opportunities; PSO/grouping без нарушения order transparency. Compression/precision менять с image error gate. | Объявить bandwidth bound только по большому GPU ms. |
| Drawable/semaphore waits | Разнести CPU busy/wait, проверить frame queue/pacing/focus/display; использовать результаты E04 и телеметрию `CAMetalDisplayLinkProbe` (квантование каденса 75 → 37.5 Hz при превышении дедлайна ~13.2 ms при нагрузке >=10 ms), back-pressure с bounded queue (2 vs 3 drawables/slots). | Удалить синхронизацию или накапливать больше кадров ради среднего FPS. |
| First-use shader/texture hitches | Cache/prewarm реальных PSO/permutations, асинхронная подготовка с корректным ownership, bounded upload per frame, кеш ошибок отсутствующих assets. Отдельно cold traversal. | Оценивать только тёплую неподвижную камеру. |
| Readback/thumbnail/log | Измерить `encodePresentation`/thumbnail update, screenshot paths, повторяющиеся logs; сократить cadence/делать по запросу с валидным completed frame и без потери save thumbnail. | Выключить сохранения/скриншоты для FPS. |

Промежуточные ориентиры: сначала устранить кадры >33 ms и выйти на 30+, затем критический путь ≤20 ms, затем целевые ≤16.67 ms с запасом. Это контрольные точки, не замена конечной цели. При пересечении очередного порога переснять profile: bottleneck может смениться.

**Gate:** E06 качество сохраняется; S0/S2/S4/S5/S6/S7 проходят раздел 1, затем полноценный 60 cap pacing run. Оптимизация без статистически различимой пользы или с визуальной регрессией не принимается. MetalFX Spatial Scaler НЕ может использоваться для закрытия нативного performance gate 1080p60: целевой 1080p60 baseline подтверждается в нативном разрешении без апскейлинга (режимы MetalFX масштабирования измеряются и оцениваются отдельно). Пользователь не должен сам измерять FPS.

## 13. E09 — стабильность и функциональная готовность (Terra, Sol по сбоям)

Не доверять старой строке «UI PASSED» как доказательству inventory/trade/death/resize: у каждого сценария должны быть собственные действия и evidence.

1. На копии профиля: холодный старт → меню → новая игра и загрузка существующего save; inventory/PDA/dialogue/trade/settings/pause; movement/mouse capture/focus/audio; бой, попадания, смерть/reload; quicksave/load, named save, валидные thumbnails.
2. S7: разные классы оружия, reload/switch, 2D/3DSS, NV/thermal, torch, grenades/particles; проверить совместимость gameplay scripts, не только число draw calls.
3. 30 min с API validation и короткие отдельные GPU validation сценарии: S0/S2/S3/S5/S7. Затем 2 h без validation: Bar→Garbage→Underground→Marsh и повтор маршрута, save/load минимум 10 циклов. Это minimum soak, не доказательство отсутствия всех багов.
4. Windowed/borderless, resize в том числе нечётные размеры, minimize/restore, смена фокуса, pause/long idle, switch resolution, Retina policy. После resize пересоздать зависимые targets/history корректно и проверить итоговый extent.
5. RSS + Metal allocations, cache counts, command-buffer errors, durations переходов. После полного цикла и прогрева caches потребление должно выходить на плато: три одинаковых цикла без монотонного роста; сохранять сырые данные. Не объявлять утечку только из-за первого cache warm-up.
6. Каждый crash/hang: binary+dirty hash, fixture, last completed serial, thread stacks/crash report и минимальное воспроизведение; исправление получает regression test. Никаких catch-and-ignore для renderer failures.
7. Запустить DXVK fallback после общих engine изменений; сравнить со своим baseline. Исходные save/mod assets не изменены; test overlays сняты.

**Gate:** нет crash/device lost/assert/зависаний в матрице; required UI/gameplay работает; memory bounded; ни одного неразобранного повторяемого P0/P1. Missing evidence — OPEN, не PASS. Performance измерить ещё раз после исправлений стабильности.

## 14. E10 — штатная поставка и воспроизводимая сборка (Terra + Sol)

1. Один release manifest: source snapshot (включая нужный WIP), dependencies/toolchain, shader sources/defines/receipts, runtime profile expectations. Не нужно коммитить весь 665-path WIP ради воспроизводимости: описать и сохранить точный контролируемый snapshot.
2. Отдельный build/release directory, чтобы проверка воспроизводимости не затёрла рабочую сборку. Сборка и cook из манифеста должны давать проверяемый комплект артефактов; timestamp-sensitive hash differences объяснить.
3. Штатный launcher не должен требовать случайных `*-next` вручную настроенных путей, начинать обязательный build перед каждой игрой или наследовать test overlay. Production config и diagnostic modes должны быть явными; defaults проходят матрицу.
4. Меню честно применяет заявленные опции. Для ещё неподдержанной опции либо реализация, либо понятный статус; не показывать работающую галку, которую backend игнорирует. Готовность эталонного пресета отдельно от exhaustive parity всех возможных конфигураций.
5. Пакет установки/обновления с backup appdata и rollback backend, пользовательские mods/saves отдельно. Проверить зависимости и startup из обычного Finder/Terminal окружения. Distribution signing/notarization — если выбран распространяемый app bundle, не искусственный blocker локальной игровой готовности. Архитектура и аудит DEV/RELEASE bundle зафиксированы в [docs/macos-app-bundle.md](docs/macos-app-bundle.md) (автоматическая синхронизация DEV `.app` и LaunchServices/Game Mode запуск интегрированы в CMake и launcher).
6. Короткий `PLAYING.md`: запуск, выбор backend, поддержанные режимы, расположение save/log, известные ограничения и способ приложить diagnostic archive. Отдельно техдок, игроку не нужны reflection/TLAB инструкции.

**Gate:** Terra выполняет документированную установку в отдельную staging-папку и первый запуск без ручного шаманства; Sol проверяет точное соответствие принятым binary/shader/config.

## 15. E11 — финальная приёмка (Sol)

- Повторить exact baseline fixtures с release candidate; приложить Windows/proxy provenance, final screenshots/clips, performance raw data и summary, soak report, конфигурацию машины.
- Таблица финального состояния по каждому требованию раздела 1: `PASS / OPEN / BLOCKED_EXTERNAL`, причина и evidence. Нельзя заменять отсутствие Windows reference локальным красивым скриншотом.
- Разница с исходным ~20 FPS показывается только как ориентир пользовательского опыта; честный speedup считать по контролируемому before/after при равном качестве/размере. Не публиковать сравнение разных сцен.
- Провести V3. При оставшемся defect открыть узкий follow-up в STATUS и вернуться к соответствующему этапу; не перезапускать архитектурный аудит целиком.
- Назвать сборку готовой только если visual/function/stability/performance gates все закрыты. Если какой-то gate не выполнен, сообщить конкретный остаток; успех остальных сохранить.

## 16. Минимальное участие пользователя: три коротких просмотра

ИИ сначала сам завершает автоматическую часть, готовит сцену, сравнение/ролик и одну инструкцию. Просит конкретную оценку, а не «потестируй игру». Пользователь не запускает команды и не разбирает логи. Не просить повторно одну и ту же проверку без релевантного изменения. При отсутствии ответа продолжать независимые работы, оставляя visual gate pending.

**V1 после E04 — 1–2 минуты, S0/S1:**

> Исправления геометрии и движения прошли автоматические проверки. Я подготовил ту же сцену. Быстро поверните камеру вправо-влево и на 180°, затем пройдите вперёд и вбок. Ожидается: постройки, NPC и растительность двигаются в кадре одновременно; после остановки ничего не «догоняет» камеру; нет светлых растянутых полигонов и пурпурных пятен. Сообщите только оставшийся дефект и примерно где/на какой секунде он виден.

**V2 после E05/E06 — 3–5 минут, готовая подборка кадров/роликов:**

> Подготовлено сравнение одинаковых сцен с подписанным эталоном. При ходьбе трава сохраняет свет, тени меняются плавно и по геометрии; тела NPC освещены без чёрных провалов; у воды есть корректный берег и отражение без шлейфа; облака/дождь и общий цвет соответствуют эталону. Посмотрите на эти признаки и отметьте только заметные различия. Если эталон здесь DXVK/GPTK, это прямо подписано, Windows-проверка ещё не закрыта.

**V3 после E09/E10 — 5–10 минут обычной игры:**

> Готов release candidate, автоматические FPS/soak/сохранения проверены. Поиграйте в подготовленной сцене с NPC: ходьба, резкий поворот, стрельба/прицел, инвентарь. Ожидается ровная отзывчивая игра без рывков, визуальных распадов и отставания объектов. Ваше действие — только оценить комфорт и оставшиеся заметные дефекты; числовые замеры уже приложены.

Если внешний Windows reference необходимо получить от пользователя, запросить его **одним пакетом** после подготовки точного save/preset/camera маршрута; это отдельная внешняя зависимость, не повод передать пользователю остальные тесты. Известный недоступный capture/Windows инструмент указывать прямо.

## 17. Проверенные команды и будущие инструменты

Все команды ниже из Git root. Во время аудита выполнялся только discovery (`ctest -N`), не build/тесты игры. Перед исполнением нужные binary targets проверить по CMake, особенно после изменения дерева.

```sh
cd <XRAY_SOURCE_ROOT>

git -c core.fsmonitor=false status --short --branch
git rev-parse HEAD
rg -n 'add_executable|add_test' src/Layers/xrRenderMetal/CMakeLists.txt

# Рабочая game target; одно выполнение на весь общий checkout.
cmake --build build --target Anomaly.DX11 -j 4

# Примеры существующих целевых probes; набор выбирать по изменению.
cmake --build build --target XRay.Metal.DynamicWorldAdapterProbe XRay.Metal.EngineWorldSnapshotProbe XRay.Metal.DetailGrassAdapterProbe XRay.Metal.FloraAdapterProbe -j 4

# Автономные платформенные probes Apple/macOS (уже собраны и проверены 8/8, 7/7, 7/7 PASS):
cmake --build build --target XRay.Metal.DisplayCapabilitiesProbe XRay.Metal.MetalFXSpatialScalerProbe XRay.Metal.CAMetalDisplayLinkProbe -j 4

# ВАЖНО: корневой каталог сейчас не перечисляет Metal tests.
ctest --test-dir build/src/Layers/xrRenderMetal -N
ctest --test-dir build/src/Layers/xrRenderMetal --output-on-failure -R 'XRay.Metal.(DynamicWorldAdapter|EngineWorldSnapshot|DetailGrassAdapter|FloraAdapter).SelfTest'
ctest --test-dir build/src/Layers/xrRenderMetal --output-on-failure -R 'XRay.Metal.(DisplayCapabilities|MetalFXSpatialScaler|CAMetalDisplayLink).SelfTest'

# Запускать после проверки, что пользовательская игровая сессия не затрагивается.
./run-gamma-metal.command --load 'player - autosave'
./run-gamma-macos.command --load 'player - autosave'

# Отдельная diagnostic сессия; capture только после world-ready marker.
./run-gamma-metal.command --capture-ready --load 'player - autosave'
```

`player - autosave` существует в исторических fixtures, но его фактическую карту/hash каждый раз проверяет runner. Не запускать старый `scratch/run_visual_parity_harness.sh` как финальный acceptance, пока E00 не исправлен.

Для trace/validation исполнитель загружает установленные навыки `using-gpucapture`, `using-gpudebug`, `using-metal-validation`, а для presentation/lifetime изменений — соответствующие навыки. Инструменты и команды проверяются на текущей установке; не придумывать gpudebug/xctrace flags. Ни один GPU capture не включать в FPS-замер.

**Уже созданные и верифицированные платформенные файлы:** `src/Layers/xrRenderMetal/DisplayCapabilities.h/.mm` (production read-only layer), `src/Layers/xrRenderMetal/tests/DisplayCapabilitiesProbe.mm`, `src/Layers/xrRenderMetal/tests/MetalFXSpatialScalerProbe.mm`, `src/Layers/xrRenderMetal/tests/CAMetalDisplayLinkProbe.mm`, `docs/direct-metal-release/MATERIAL_PERMUTATION_CENSUS.md` (read-only materials/permutations census для E06/E07), `docs/direct-metal-release/WEATHER_EFFECTS_CENSUS.md` (read-only weather/effects census для E06). Sol не нужно пересоздавать эти probes и census с нуля.

**Предлагаемые новые файлы (их ещё нет; создать соответствующим этапом):** `script/run_metal_release_fixture.sh` (E00), `script/analyze_metal_frame_times.py` (E01), `docs/direct-metal-release/FRAME_DATA_CONTRACT.md` (E02), `docs/direct-metal-release/PARITY_MATRIX.md` (E06), `docs/direct-metal-release/STATUS.md` (E00), `docs/direct-metal-release/PLAYING.md` (E10). Названия новых probes утверждает Sol при E02/E07; не выдавать их за уже существующие команды.

## 18. Шаблоны задач и передачи работы

### Запуск Sol

```text
Выполни следующий открытый этап DIRECT_METAL_RELEASE_PLAN.md в текущем dirty checkout.
Сейчас это E00 (инфраструктура Terra, проверка admission тобой), затем E01 и E02.
Сначала сохрани состояние WIP и прочитай только активный этап/STATUS/нужные функции.
Не переписывай уже существующие water/AO/weather paths с нуля.
Главный кодовый P0 — mutable Shared buffers: per-frame и отдельно per-pass ownership.
Распределяй Terra только готовые узкие пакеты по таблице, максимум два одновременно.
Сборку/игру/GPU capture запускает один интегратор. Автотесты и замеры делай сам.
Визуальную приёмку запрашивай по V1/V2/V3 с конкретным ожидаемым результатом.
Если blocked только Windows reference или human review, продолжай независимые этапы.
Не объявляй 60 FPS, parity или стабильность по build/log PASS.
В конце обнови короткий STATUS: факты, evidence, открытый gate, точный следующий пакет.
```

### Пакет Terra

```text
Этап: <ID и одна подзадача>.
Вход: <короткий контракт + конкретные файлы/символы + fixture/evidence>.
Разрешённые изменения: <закрытый список файлов>.
Не менять: центральный backend/ABI/launcher и файлы другого агента.
Результат: <один проверяемый артефакт или исправление>.
Acceptance: <команды, ожидаемые assertions/data, негативный кейс>.
Build/runtime выполняет <назначенный интегратор или ты в выделенное окно>.
После двух неудачных гипотез передай Sol reproducer и diff, не расширяй scope.
Ответ до 30 строк: что изменено, проверки, evidence paths, риск/блокер, следующий шаг.
```

### Обязательная запись по завершении пакета

`ID → cause/evidence → files → build/test counts → runtime fixture → visual status → CPU/GPU/present before/after → unresolved → next`.

Итоговая дисциплина проекта: каждый закрытый пункт имеет воспроизводимый факт. Код, тест, картинка, движение, скорость и длительная стабильность — разные проверки; ни одна не заменяет остальные.

## Packet 3 repair evidence (2026-09-27)

The active Bar shadow-caster/cascade contract is repaired and GPU verified;
see [E05_SHADOW_CASTERS_AND_SUN_CASCADES](docs/direct-metal-release/E05_SHADOW_CASTERS_AND_SUN_CASCADES.md).
Flora/conditional grass, cold off-camera static geometry and current CPU-skinned
casters are covered by production converted GPU witnesses. Source 20/60/160
footprints and the exact source three-phase receiver/stencil graph are carried
through. Same-binary grass off/on, observed camera motion/cut, alpha and wind
map pairs pass. This supersedes A3's active Bar missing-route/near20+far findings,
while A4 atlas overflow, portal/HOM fallback, unobserved Flora permutations and
Windows image parity remain open. No release or performance PASS is inferred.

## Packet 4 repair evidence (2026-09-27)

A4 overflow is now repaired in the tested scope with complete sequential local
shadow atlas batches, atomic six-face point groups, immediate atlas consumption,
and preserved accumulation/depth/stencil. Permanent overflow fallback is removed.
GPU 17/25/37/125/200, canonical point-face controls and camera return pass;
rebuilt CPU gates pass 5/5. Final same-binary Bar grass-on route exits0: 25 real
records, 28 GPU snapshots/350 records, A96→out108→A135, no lost IDs/faces,
NaN or fallback. Natural Bar completed frames reach 1107 records/91 batches;
the full validation run was interrupted for MetalTools memory pressure and is
not runtime acceptance. The runner now preserves the scenario window after
an early recorder exit; the short movie does not prove the full visual route.
See [E05_LOCAL_SHADOW_BATCHES](docs/direct-metal-release/E05_LOCAL_SHADOW_BATCHES.md)
for provenance, rejected runs and limitations. A5 controls/color attribution,
remaining E06 parity, Windows images, production performance and startup
empty-visual diagnosis remain open. No E07/release PASS is inferred.
