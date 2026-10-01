# Ограниченная проверка CPU submission, 2026-09-28

Раунд выполняет PLAN_OPTIMIZATION.md. Существующий WIP сохранён: собственные исходники и manifest до правок в `scratch/release_runs/20260928-opt-prestate/source/`, Git status — в `status.txt`. Никаких reset/stage/commit, изменения AI, качества, разрешения, shaders или DXVK/MoltenVK fallback.

## P0

Один S4 diagnostic: 15 s warmup, 65 s window, 30 s settle; нативные 1920×1080, exit0/scenario OK. Батарея (63% перед работой, 55% после); отдельного AC timing acceptance нет. Начальная executable SHA256 `c59f24183aa6111f926fd2f7db51eb05f292152d34299028e83f9dd932637987`. Binary/shader/save/config provenance в run directory. Исторические 24 FPS E08 не используются как изолированный контроль новых правок.

Новая диагностика `XRAY_METAL_OPT_BIND_PROFILE=<absolute.csv>` измеряет раз в 120 кадров без GPU counters: checks/preparation, snapshots shared constants, indirect declarations, owner retention, TLAB allocation/fill и encoder bind. Контроль проверки типов/offsets/occupied slots и immutable per-draw snapshots сохранён. CSV пишет только после GPU completion со status; failed frames не допускаются.

Тяжёлая S4 камера после settle: 5 sampled frames, медиана 22611 stage bindings / 66603 entries, snapshots 4.26 MB/frame. Инструментированный bind aggregate 21.375 ms; вложенная preparation 12.906 ms, snapshot 0.923, declarations 2.908, retain 1.834, TLAB allocation 0.494, fill 2.674, encoder bind 0.616. Эти категории вложены; не складывать медианы. Final TLAB declaration также входит в declaration_ms, поэтому subtraction не является точным чистым checks budget.

Сравнение sampled/unsampled тяжёлых кадров внутри одного процесса: CPU encode 58.726 vs 48.634 ms p50. Это не отдельный случайный on/off A/B; оно показывает существенную стоимость clocks на sampled frames. Диагностика не доказывает ROI P1a/P1b; дальнейшие решения принимаются без подробных таймеров. Sampled кадры учитываются в игровом диагностическом distribution. Итог краткого P0 окна: encode 48.639 ms p50 / 54.606 ms p95, GPU 32.009 ms p50; mean displayed 18.43 FPS. Не production gate.

Raw: `20260928-opt-p0-s4/`; `p0-bindings.csv`, `p0-summary.json`, `p0-overhead-diagnostic.json` в opt-prestate. Карта изменяемого состояния и владельцев — `ownership-map.md`.

## Выбор следующего участка

P2: статические local-shadow caster packets в одном atlas pass. Material entries, merged index runs и WVP frame slices уже готовятся до encoding. Сначала serial packet oracle против прежнего direct loop; затем parent с 1/2/4 children в исходных непрерывных диапазонах. Все snapshots, validation и owner/frame writes — serial; workers получают только неизменяемые Metal objects/ranges и собственный encoder state. Production integration не выполняется до положительного экономического screen: исходный draw loop сохранён. Черновая перегруппировка static draws всех tiles была исключена, поскольку план требует сохранить порядок команд между static и другими семействами casters. После успеха требуется кодировать contiguous ranges внутри исходного места static drawing, оставляя остальные семейства casters, порядок tiles и потребление atlas последовательными.

P1a не подменяется E08 resource dedup: Residency Sets здесь пока не проверены. Immutable material-only P1b ограничен стабильной fragment table AREF; changing CBV/WVP адреса нельзя кешировать по material name. P3 ICB не стартует автоматически.

Результаты P2 и окончательное решение будут добавлены после pixel oracle и representative probe. Production acceptance требует обычного same-binary S4/S2 A/B; короткий GPU/игровой diagnostic не закрывает native60 или Windows parity.

Предварительный независимый review обнаружил и потребовал исправить N+1 child encoders из-за пустого preparation child и cleanup на ошибке создания child. Prototype использует существующий child как worker 0; остальные children создаются в порядке draw ranges. Это исправления до запуска oracle; наличие тестового ожидания childCount не заменяет проверку фактического API path.

## P2: результат и STOP

Prototype собран как `XRay.Metal.StaticShadowPacketGpuProbe`. Начальная ошибка fixture (отсутствующий TileCount и недостающие 4 bytes packed AREF color до UV offset28) исправлена; сборка repair1 PASS. Это исправления fixture до принятого запуска, не улучшение алгоритма после negative screen.

Один короткий GPU oracle под API+shader validation: PASS, без validation errors. Exact full R32 maps сравниваются direct/serial packets/1/2/4 children: 4 regions и одна область с пересекающимися unequal-depth casters; есть положительное покрытие, реальный AREF discard отличается от opaque control, большой baseVertex с UInt16/nonzero IB offset, второй WVP меняет output и повторно совпадает direct/4. Подготовительный child переиспользуется как draw worker0: фактически 1/2/4 draw children. Frame-slot семафоры защищают ranges; submission retry не был принудительно проверен. GPU duration в benchmark недоступен. Raw: `20260928-opt-p2-oracle/`, `p2-oracle.log`.

Representative screen — один блок менее секунды после initialisation, 2048 draws, 1 warmup +3 reps на вариант, без validation/counters/readback. Все сравниваются с исходным direct path, не только с более дорогим serial packet control. Внешнее построение packets также записано отдельно. Результаты diagnostic, не игровой FPS и не production gate:

| Путь | encode mean ms | packet build + encode mean ms |
|---|---:|---:|
| Original direct | 1.246 | 1.333 |
| Serial packets | 1.631 | 1.707 |
| Parent /1 worker | 2.090 | 2.163 |
| Parent /2 workers | 1.757 | 1.819 |
| Parent /4 workers | 2.654 | 2.728 |

При 2 workers сама preparation остаётся ~0.798 ms, dispatch+join ~0.744 ms; max worker ~0.604 ms не равен задержке кадра и не складывается с dispatch wall. Существующий static merge уже уменьшил реальные S4 12135 source rows до1889 draws (`engine.log` frames627/927/1227). Этот representative узкий участок не показывает полезного резерва для3 ms и10% encode p95; 2/4-worker prototype медленнее исходного direct. Ускорение только относительно специально более дорогого serial builder не принимается. Порядок вариантов фиксированный, reps мало; общего вывода о неэффективности Metal parallel API здесь нет.

**STOP этого P2 кандидата:** sequence/ownership/pixel contract поддержан, экономический screen не пройден. Не выполнять длинные S4/S2 A/B для заведомо более дорогого кандидата, не расширять его на другие draw families. Production Scene integration не выполнена; её черновик восстановлен из prestate. Прототип, binary hash, CSV и patch сохранены для воспроизведения, из рабочего backend/header/CMake он удаляется.

## P1/P3 и итог раунда

P0 не обнаружил достаточного резерва для выбранного минимального P1 scope. Даже инструментированные общие p95 declarations2.965 ms и allocation+fill ~3.199 ms меньше требуемых ~5.46 ms (10% текущего S4 encode p95), а это совокупность **всех** bindings с clock overhead: только часть ресурсов immutable; changing CBV/WVP не кешируются. Поэтому P1a/P1b не получают прототип в этом ограниченном раунде. Это prefilter/оценка целесообразности, **не результат A/B Residency Sets или cache** и не доказательство их нулевого эффекта. Данных для реализации на другой семье недостаточно; остаются непроверенными. P3 ICB не запущен: его условие крупной стоимости повторяемых commands и полезного локального ownership контракта/ROI не выполнено. Tile/MetalFX/FI/AI/quality изменения не запускаются.

**Нового подтверждённого прироста FPS нет.** Раунд останавливается по условиям плана, нормальный рендер восстанавливается. Native1080p60, полная AI/cold/long-session матрица и Windows parity остаются OPEN. Бюджет: один короткий игровой diagnostic, один compact pixel oracle, один representative timing block; ни одной полной игровой A/B матрицы. Заряд после проверок48%, ноутбук не разряжается длинными benchmark сериями.

## Финальное состояние

`MetalR4Backend.mm`, `IR4Backend.h`, `MetalR4EngineScene.mm`, `CMakeLists.txt` и неизменённый `MetalBootstrap.mm` побайтово совпадают с prestate; diagnostic branches тоже убраны из рабочего runtime. Полная пересборка затронутых зависимостей `Anomaly.DX11 -j3` PASS (`build-restored-verified.log`). Окончательная проверка не включала новый игровой запуск. Analyzer admission tests2/2 PASS. Новые постоянные файлы — план/отчёт и анализатор с тестами; существующий WIP не stage/reset/commit.

Archive: `scratch/release_runs/20260928-opt-prestate/round-source/` содержит проверенный prototype source, `runtime-prototype.patch` — только runtime/probe diff к сохранённым prestate исходникам, `StaticShadowPacketGpuProbe-tested` — exact tested executable. `git apply --check runtime-prototype.patch` PASS на восстановленных файлах; не применять к другому WIP без проверки hashes/отдельной копии. SHA в `round-source-manifest.json`, `p2-screen-summary.json`, `restored-source-manifest.json` и `final-artifacts.json`.

Известная непринятая production граница prototype: между join и EndRenderPass есть активный parallel parent без active child; ряд outside-pass guards старого backend проверяет только child. Тест всегда корректно завершает parent до validation/submit; общего hardened API/retry/level-change admission нет. Для будущей интеграции нужны parent-aware guards и сохранение исходного inter-family command order, затем игровые A/B. Ни один probe не заменяет эти gates.

После завершения сборки батарея46%; активного игрового процесса нет.
