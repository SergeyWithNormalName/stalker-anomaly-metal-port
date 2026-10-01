# CPU HOM: подготовка диагностики целиком закрытых источников света

Дата: 2026-09-28. Статус: **DIAGNOSTIC IMPLEMENTED / RUNTIME SCREENED;
LIGHT SUPPRESSION NOT ENABLED.**

Реализована только подготовка одного эксперимента из
[OPENXRAY_OPTIMIZATION_REVIEW_2026-09-28.md](OPENXRAY_OPTIMIZATION_REVIEW_2026-09-28.md):
диагностика полностью закрытого консервативного объёма света существующим CPU HOM.
Весь свет продолжает рисоваться. После разрешения пользователя выполнены три
диагностических запуска. Есть измеренный резерв CPU-работы в S4; фактическое
ускорение от отсева, совпадение изображения при отсеве и 60 FPS не установлены.

## Результат живой диагностики

Все три запуска выполнены от AC, без Low Power Mode, без Metal validation/capture
и без другого игрового процесса. Thermal state до/после был nominal; после
сборки дождались охлаждения из fair. Каждый запуск завершился с exit 0,
archive-winning HOM загрузился, вся геометрия и все источники продолжали рисоваться.
Один бинарник SHA-256 указан в `scratch/release_runs/20260928-light-hom-prestate/build-proof.json`.

| Запуск | Warmup / window | Полные диагностические samples | HOM + grouping/logging p50 | CPU prep + encoding закрытых shadow tiles p50 |
|---|---:|---:|---:|---:|
| S4 короткий screen | 15 / 30 s | 694 | 0.2664 ms | 2.5799 ms |
| S2 короткий screen | 15 / 30 s | 1,001 | 0.2259 ms | 0 ms |
| S4 основной diagnostic | 60 / 150 s | 3,382 | 0.2787 ms | 2.6770 ms |

В основном S4 окне 1,425–4,806: медиана 10 закрытых источников из 52 пригодных
для проверки. На них приходится 24 shadow views, 1,935 static caster rows,
**297 encoded static draws** после merge и 90 dynamic draws; общий baseline —
12,135 static rows / 1,889 encoded static draws. Это marginal medians по окну,
не обещание исключить всю baseline работу. 3,181 из 3,382 samples имеют ненулевую
известную работу закрытых tiles. В S2 окне 122–1,122 закрытых источников/работы нет.

Разность известных S4 компонентов около **2.40 ms CPU** (2.6770 − 0.2787).
Она не является end-to-end выигрышем: остаются shared encoder/cache effects,
стоимость дополнительной cost-телеметрии и пока неизвестная доля accumulator.
В основном S4 есть 6,323 mixed shadow accumulations и 3,181 mixed unshadowed
accumulations. Полнота GROUP/TILE/BATCH/ACCUM receipts — 100%, но полностью
атрибутировать общую экономию можно лишь в 201 sample, где закрытой работы нет.
Использовать эту смещённую подвыборку для заключения «резерв 0» нельзя.

CPU scene p50 в основном диагностическом S4 — 38.740 ms, encode — 40.604 ms,
GPU frame — 23.153 ms. Эти inclusive бюджеты не прибавляются к tile intervals.
GPU экономия отдельных lights не измерена. Presentation callbacks неполны
(5 missing/zero timestamps), pacing/native60 gate открыт. FPS improvement не заявляется.

Источник закрыт **данным HOM**; соответствие proxy opaque-сцене всё ещё имеет
`geometry_verified=0`. Поэтому режим реального отключения источников не добавлен.
Наблюдаемый небольшой резерв оправдывает только ограниченную следующую проверку
геометрии/изображения, не расширение в три других направления плана.

Свидетельства:
[S4 60/150 light-hom.json](../../scratch/release_runs/20260928-light-hom-s4-steady-01/light-hom.json),
[S4 counts](../../scratch/release_runs/20260928-light-hom-s4-steady-01/light-hom-work-counts.json),
[S4 timing baseline](../../scratch/release_runs/20260928-light-hom-s4-steady-01/baseline.json),
[S4 screen](../../scratch/release_runs/20260928-light-hom-s4-screen-01/light-hom.json),
[S2 screen](../../scratch/release_runs/20260928-light-hom-s2-screen-01/light-hom.json).

## Что подготовлено

- `XRAY_METAL_LIGHT_HOM_DIAGNOSTIC=1` включает загрузку archive-winning
  `$level$/level.hom` в существующий `RImplementation.HOM`. Без этой переменной
  Metal не загружает HOM и не строит его raster. Другие значения не включают режим.
- `HOM.Load(false)` не регистрирует legacy `seqParallelRender` callback.
  Такой owner оставляет обычный HOM выключенным: `visible` остаётся fail open,
  `Enable` не включает scene culling, `MT_RENDER` не строит raster. Это защищает
  и прямые старые HOM call sites за пределами `occ_visible`.
  Metal level unload освобождает HOM. Диагностический загрузчик проверяет наличие
  chunk 1, размер/выравнивание записей, finite/nondegenerate vertices, пригодность
  packed mesh для CDB. Ошибка оставляет HOM недоступным и не исключает свет.
- Raster строится синхронно в render thread из **точных view/projection/eye**
  текущего Metal `ViewState`. Требуются совпадающие stamps камеры, окружения,
  snapshot света и `Device.dwFrame`. Второй viewport/retry получает новый `sample`
  и заново строит HOM даже внутри того же engine frame.
- CPU raster использует прежние CDB/frustum/clipping/rasterization/test.
  Диагностическая ветка не использует stochastic triangle skipping, `Random`
  или cached `vis_data`, не меняет игровые RNG/skip states и не вызывает D3D debug
  overlay. `Raster` и raster-core scratch остаются глобальными: параллельное
  использование HOM этим режимом запрещено; scheduler не восстанавливался.
- Проверяется world AABB **всего аккумуляторного mesh**, построенный из того же
  `localLightModel`, который используется при рисовании. Для OmniFace локальные
  границы берутся из настоящего `du_sphere_part`; point parent объединяет все
  шесть faces и получает одну метку. Box расширяется на 1 cm и float margin.
  Лампочка или положение источника отдельно не проверяются.
- Camera-inside, near/far-plane crossing, stale frame, nonfinite/overflowing
  projection, malformed basis/неполная шестёрка faces возвращают visible/unknown.
  HUD и потенциальный текущий volumetric-проход исключены. SSFX volumetrics
  учитываются по настоящим условиям `BuildMetalR4VolumetricSpotPass`, включая
  global enable/mode, type и shadow flag.
- Метки **не входят** в admission, batch planner, caster selection или GPU
  команды. Ordinary Metal `occ_visible` по-прежнему не применяет HOM.

## Что известно об assets

S2/S4 используют `l02_garbage`. В materialized profile нет loose `level.hom`,
но он **есть** в базовом `gamma/anomaly/db/levels/level_l02_garbage.db0`.
Индекс архива декодирован офлайн штатным LZHUF-алгоритмом, нужные записи извлечены;
игра для этого не запускалась.

| Ресурс | Размер | SHA-256 |
|---|---:|---|
| Базовый архивный `level.hom` | 28,540 B | `44c3cfc8a198f500b09c235cfc221c761447b8705738d8339b878a6ce19ce53f` |
| Активный profile `level` | 4,942,565 B | `22db7dcee2428a8b990ebc11a7726a95e862eae6b8ec80baccb0d06ca2a2ab62` |
| Активный profile `level.geom` | 66,075,492 B | `262f35806e6cc22407cc2bc8825bdddad264837bf10f06fd1f954551058cff12` |
| Базовый архивный `level` | 6,317,789 B | `2b7e1b869fc64076aa4ec4c56b43dafe8ec607291aae75039388db5ade23597f` |
| Базовый архивный `level.geom` | 82,312,792 B | `996b07465abfbe7486e4d74f4d0b36c711ed6d36ea74d96d048563b931bb1ce8` |

HOM chunk 1 содержит 713 triangle records по 40 B. Офлайн нет nonfinite vertices
или raw/packed degenerate triangles; после 1 cm packing получаются 706 vertices,
минимальная triangle area 0.030474 m². Структурные loader gates выполняются.
Активные `level`/`level.geom`
поставляет mod `442- Expanded & Fixed Maps`; Metal читает именно эту геометрию.
Офлайн loader разбирает обе версии геометрии, но их vertex/index payloads отличаются.
Это **не доказывает** неправильность упрощённого HOM; совпадение его закрывающих
поверхностей с фактически нарисованными opaque surfaces пока не квалифицировано.
Поэтому все runtime receipts имеют `geometry_verified=0`; даже кандидат `hidden`
означает лишь «закрыт данным HOM». Он не разрешает отключение света.

Офлайн свидетельства:
[audit.md](../../scratch/release_runs/20260928-light-hom-assets/audit.md) и
[audit.json](../../scratch/release_runs/20260928-light-hom-assets/audit.json).
Совпадение общего world envelope обеих геометрий и размещение HOM/fixture camera
внутри него — только проверка согласованности координат, не surface coverage.
Материалы этой директории диагностические, не заменяют runtime assets.

## Стоимость и анализ будущих логов

`METAL_LIGHT_HOM_FRAME/GROUP` связывают frame/sample, камеру, источник,
консервативные bounds, exemptions и цену HOM. В FRAME есть точное ожидаемое
число batches, shadow records и unshadowed records; недописанный лог не считается
нулевой работой. `METAL_LIGHT_HOM_TILE` содержит измеренные CPU intervals
preparation/merge и shadow encoding, реальные static caster rows, **encoded draws
после E08 merge**, dynamic draws и static transform bytes.
`BATCH` содержит inclusive CPU interval, `ACCUM` — цену накопления целого batch.

Анализатор `script/analyze_metal_light_hom.py` соединяет данные по `(frame,sample)`,
проверяет stamps, counts и completeness, отказывается от `suppressed!=0`.
Batch total не прибавляется к вложенным tile intervals. Mixed-batch accumulation
остаётся неизвестной; он не распределяется по доле ламп. Для unshadowed records
batch `-1` накопление можно отнести к кандидатам лишь когда весь batch eligible-hidden.
Нет лога/неполная запись/недоступный HOM означают отсутствие данных.
Обычные CP1251/non-UTF-8 строки игрового лога допускаются без изменения raw file;
структурные поля HOM записей остаются строго валидируемыми. `--window` выбирает
inclusive engine frames из стандартного `timing-window.json`, убирая warmup.
Известная tile-работа агрегируется по всей подвыборке с полной TILE coverage;
полная cost attribution показывается отдельно, чтобы mixed samples не создавали
смещение к frames без закрытого света.

При полной диагностике цена работы на eligible-hidden tiles — оценка возможного
CPU резерва, а не измеренное ускорение. В ней остаются shared encoder/cache effects;
общие upload/planning costs и фактические GPU durations отдельно не атрибутированы.
`overhead_ms` покрывает построение/проверки HOM, grouping и GROUP logging, но не
все последующие cost-logging/clock calls. Подробная диагностика влияет на время:
для нормального A/B её нужно отключить. По этим логам нельзя объявлять FPS benefit,
GPU savings, visual correctness или автоматически принимать/отвергать весь резерв.

## Проверки до запусков

Собраны `Anomaly.DX11`, `XRay.Metal.LocalLightAdapterProbe`,
`XRay.Metal.LightHomBoundsProbe`. Два focused CPU CTests проходят: actual mesh
containment для Point/Spot/OmniFace в разных bases и projection admission
(stale/camera-inside/near/far/nonfinite/overflow).

18 Python unit tests анализатора проходят: counts, sample/frame joins, missing
records, invalid durations, whole/mixed batches, unshadowed-only work, legacy
encoding, timing-window selection и selection bias.
Green build/CPU tests сами по себе не подтверждают качество HOM геометрии,
совпадение изображения при отсеве или ускорение. Live loading и завершение этих
трёх fixtures подтверждены отдельно; long-session/full-AI/visual acceptance не заявляются.

Исходный WIP и бинарник сохранены в
`scratch/release_runs/20260928-light-hom-prestate/`. Здесь же task-only patch,
manifest и build/check logs. Коммита, публикации и правок runtime profile нет.

## Воспроизведение и следующая граница

Пример повторного диагностического запуска. Нужны AC power, nominal thermal
state и отсутствие другой игры/профилирования. Указать новый run id.

```sh
cd <XRAY_SOURCE_ROOT>
XRAY_METAL_LIGHT_HOM_DIAGNOSTIC=1 script/run_metal_release_fixture.sh \
  --timing --fixture S4 --backend metal --warmup 60 --duration 150 \
  --run-id light-hom-s4-repeat-01
python3 script/analyze_metal_light_hom.py \
  scratch/release_runs/light-hom-s4-repeat-01/engine.log \
  --window scratch/release_runs/light-hom-s4-repeat-01/timing-window.json \
  --output scratch/release_runs/light-hom-s4-repeat-01/light-hom.json
```

Выполненные запуски — диагностические, не A/B отсева. Для количественного
performance вывода нужна отдельная оценка instrumentation overhead и обычный
A/B на том же бинарнике, настройках и камере. Whole-log summary включает warmup;
для приведённых выше чисел использован только `timing-window.json`.

Перед любой веткой с реальным отключением нужно квалифицировать HOM occluders
против нарисованной сцены (особенно mod geometry, doors/holes/transparent surfaces),
затем проверить изображение при движении/раскрытии объёма и обычный A/B.
Режима отключения в подготовленном коде **нет**; это отдельный следующий шаг,
допустимый только после существенного подтверждённого резерва.

Если HOM недоступен, геометрия не соответствует, цена HOM/интеграции съедает
пользу или пригодных источников/работы мало — остановить эксперимент.
Автоматического перехода к portal graph, parallel preparation, `smapvis` или
другим трём идеям нет.
