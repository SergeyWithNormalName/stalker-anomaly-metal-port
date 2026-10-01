# E05 — полнота локальных теней / пакет 4

Дата: 2026-09-27. A4 FIXED / GPU VERIFIED в проверенном scope. Финальный controlled Bar route exit0 / PASS; natural Bar проверен отдельными completed GPU frames. Windows image parity и production performance остаются OPEN.

## Production contract

- Atlas остаётся 2048×2048, 16 плиток по 512. Планировщик распределяет все поддерживаемые видимые shadow records в последовательность batches; cutoff/overflow fallback удалён.
- Point-light имеет явный parent ID в обоих capture paths. Все шесть OmniFace одного parent входят в один batch; идентификаторы не декодируются. План валидирует уникальность и полноту, затем детерминированно пакует группы без distance/hysteresis selection.
- Каждый batch отрисовывает карты, немедленно накапливает только соответствующие light indices в RGBA16F accumulator и затем переиспользует atlas. Accumulator/depth/stencil имеют Load/Store. Один `DrawState` и stencil-marker охватывают unshadowed sources и все shadow batches; masked rollover сохраняет geometry bit 0, depth и уже накопленный цвет.
- Shadowed light без текущего валидного atlas record вызывает ошибку кадра. Освещение без теней применяется к действительно unshadowed источникам.
- Плитки внутри batch последовательны: устранено несовпадение сохранённого physical tile и ordinal CB offset. LMH получает отдельный immutable frame slice на batch, упакованный только для casters, прошедших light frustum.
- Shadowed Omni `$Globals` исправлен с 208 до 400 bytes: фактический DXIL читает `m_shadow` по offset 288 и `sss_id` по 352.
- Повторный общий CB Flora/DetailGrass загружается один раз на shadow call. DynamicWorld shadow transforms получают immutable 256-byte slices вместо общего capped 2048-entry ring. Ограничение frame arena 128 MiB сохранено.

## Проверки

Evidence: `scratch/release_runs/20260927-local-shadow-batches/`.

### CPU

Rebuilt CPU gates: DynamicWorld, Flora, DetailGrass, AtlasFoundation и BatchPlan — 5/5 PASS (`cpu-rebuilt-final.log`). Pure planner проверяет overflow, отсутствие потерь/дубликатов, детерминизм при обратном порядке входа, непрозрачные light ID, шесть граней одного parent, incomplete/duplicate rejection и capacity < 6. Atlas foundation сохраняет source +1 border, half-texel и порядок матриц.

### Изолированный GPU

`gpu17-seventh`, `gpu25-final`, `gpu37-final`, `gpu125-final`, `gpu200-final`: PASS с Metal API + shader validation. Используются production planner, local-light adapter и converted `shadow_direct_base.vs` / `shadow_direct_base_color.ps` из bundle `sun`; G-buffer этого ограниченного probe синтетический.

- Реальные converted VS/PS сначала пишут R32 caster depth. Atlas рендерится, потребляется и переиспользуется в одном command buffer.
- Batch и serial-per-light accumulation точно совпадают в трёх позах. A→B→A возвращает идентичный RGBA16F output.
- Unshadowed и empty-atlas controls дают положительный RGB signal. Реальный caster меняет RGB output отдельно для каждой канонической грани 0..5; остальные пять граней в соответствующем контрольном случае имеют нулевой вклад.
- 125/200 записей проверяют общий marker cursor и rollover через несколько atlas batches.
- Native readback snapshots содержат atlas depth, actual texture data, per-tile hashes/coverage/receiver overlap, camera matrices, depth и geometry-bit hashes. Проверка receiver сначала проверяет light frustum и inner tile bounds.

### Реальный Бар

Предварительный controlled run `20260927-shadow-batches25-second` exit0: 25 real records (3 point × 6 + 7 spot), два batches, 28 GPU snapshots / 350 records. Исходная поза frame96 → out111 → returned135; все 25 ID сохранены и имеют caster coverage, все шесть point faces покрыты. Depth и geometry-bit hashes совпадают между batches; NaN/fallback отсутствуют. Бинарник этого предварительного run: `4fc59aa25e8067266b634399f0f469a3b89cec4a890ab53cd8b1e9fc3fd46322`.

Полный список Бара потребовал дополнительного уменьшения uploads: dense tables всех level materials для каждого batch исчерпывали arena. Первый unfiltered run остановлен и не считается PASS (arena capacity exceeded, exit137 после остановки owned process).

После исправления `20260927-shadow-batches-bar-second` на финальном production binary достиг 1107 records / 91 batches; все logged records rendered, fallback=0. Frame30: 989 records / 82 batches / 163 point parents; frame45: 663 / 55 / 109. Все 137 GPU snapshots PASS, IDs/tiles точно совпадают с stdout, шесть граней каждого parent находятся в одном batch; все 1652 плитки имеют caster depth. Между batches depth/geometry-bit hash неизменен, accumulator finite; есть shadowed primary receivers. Максимальная LMH transform загрузка 25,086,976 bytes.

Финальный binary с grass shadows on в `20260927-shadow-batches25-final` завершился exit0: 25/25 records в двух batches, 12 native GPU snapshots, depth/geometry/finite/coverage gates проходят. Однако route gate FAIL: capture остановил scenario на elapsed55.538, до observed camera return. Это partial coverage evidence, не camera-route PASS. Две extended попытки завершились abort до setup fixture: `CModelPool::Exists` → `Instance_Load` → `FS.cpp:184`, empty file name; это не batch evidence и не PASS. Повтор `p4-g25-final` в коротком каталоге загрузился и завершился exit0; startup причина не установлена. Но recorder завершил 110-second request через 48.453 seconds и отправил ACK до возврата камеры, поэтому route gate снова FAIL.

Runner теперь удерживает scenario до полного заданного wall-time окна, отдельно сохраняет recorder exit status и не включает Metal validation у utility `screencapture`. Camera acceptance берётся из Device + actual GPU matrices; длительность ролика остаётся отдельным фактом.

**Финальный `p4-g25-ack`: exit0, API + shader validation, grass shadows on, route gate PASS (0 errors).** 25 real records / 2 batches, 28 GPU snapshots / 350 records, A96 → out108 → returned135. Все 25 IDs/tile mappings сопоставлены с stdout; каждый из трёх point parents сохраняет шесть покрытых граней в одном batch. Depth/geometry-bit hashes одинаковы между batches каждого кадра; accumulator finite, fallback=0. `native25-ack-final-summary.json` содержит проверку фактических GPU camera matrices и receiver coverage. Wind/NPC дают ожидаемые различия tile hashes при возврате. Финальный production SHA256: `549d7a235b3ad40ed5dadbce1a70ee1d5258786510ebef769af35c7e20c9eccf`.

Recorder завершился exit0, но фактическая длительность `motion.mov` — 26.402 seconds при request110. Runner удержал полный сценарий после раннего выхода utility. Этот ролик не является полным visual route acceptance; подтверждение camera route дают Device telemetry и production GPU readbacks. Не заявляется Windows visual parity.

Это доказательство отдельных completed GPU frames. Полный validation run прерван SIGINT (exit130): MetalTools shader-validation bindings достигли 49.3 GB peak footprint и swap; sampling показывает CPU внутри validation draw encoding. Он не считается полным runtime/motion PASS и не измеряет production performance. Readbacks и sampling сохранены в `native-bar-second-summary.json` / `native-bar-second-sample.txt`.

## Граница результата

Доказательство local-shadow batching отделено от Windows image parity, performance benchmark и оставшихся E06 gaps. Шесть point faces хранятся атомарно как выбранная coherency policy; Windows может размещать их в разных `smap_ID` pools. Не каждая грань обязана иметь shadowed primary receivers: в её направлении может быть небо или отсутствовать occluder. Wind и NPC продолжают двигаться, поэтому native camera-return tile hashes не обязаны совпадать побитно.

Portal/HOM fallback, unobserved Flora permutations и NPC previous-bones/identity policy из предыдущих пакетов не закрываются этим исправлением. В активном профиле grass shadows выключены.

## Воспроизведение и review

- `cmake --build build --target Anomaly.DX11 XRay.Metal.LocalShadowBatchGpuProbe XRay.Metal.LocalShadowBatchPlanProbe -j 4`
- GPU probe: `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 build/src/Layers/xrRenderMetal/XRay.Metal.LocalShadowBatchGpuProbe --gpu build/metal-runtime-shaders/local-lights build/metal-runtime-shaders/sun 200 <output-dir>`
- CPU: `ctest --test-dir build/src/Layers/xrRenderMetal -R 'Metal\.(DynamicWorldAdapter|FloraAdapter|DetailGrassAdapter|LocalShadowAtlasFoundation|LocalShadowBatchPlan)' --output-on-failure`
- Native gate: `python3 script/verify_metal_local_shadow_batches.py --stdout scratch/release_runs/p4-g25-ack/engine.log --probe-dir scratch/release_runs/20260927-local-shadow-batches/native25-ack-final --scenario-report scratch/release_runs/p4-g25-ack/telemetry.json --probe-every 3 --output <summary.json>`
- Fixture copies, overlay, provenance, binary/source hashes, successful and rejected run logs находятся в evidence folder. `focused-existing-files.patch` сравнивает с pre-packet snapshots и не включает прежний WIP. Новые planner/validation/probe/analyzer files перечислены в `final-source-manifest.json`.
