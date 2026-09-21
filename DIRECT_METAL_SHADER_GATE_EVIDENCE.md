# Direct Metal representative shader gate evidence

Date: 2026-09-09

Host GPU: Apple M1 Pro

This is a durable summary of the successful generated runtime receipt. The
original local receipt was
`/private/tmp/gamma-r3-metal-gate-runtime-proof-7.txt`, SHA-256
`328840aab833c8d84123aec8f1cfe6db8fc041b3c22f9807ad2f56e12391ab0e`.
Generated DXIL, metallibs, reflection JSON, and temporary receipts are not
checked into the repository.

## Evidence chain

```text
DXC receipt SHA-256:
4723c6e72f35ef9fd1f2815f81e9aba26c0f83f128b011c4f0efc5385e46369a

MSC receipt SHA-256:
47cad99ff274f400336fb8cb39c14b4c6b1623f4e0e784e4d2734c1a9aa93ccb

Manifest SHA-256:
9328e880ee2e0476b65056f8f1dc413e232816e2714c51ff0efbc8988f0380a7

Metal runtime harness SHA-256:
4bcc93d94a3a20f4a32de546379d8160125f8ea08d3473d7e8312441f2b231c9

Metal gate executable SHA-256:
e6e3051861b5cfba45769c8030e00ba914536369432f204a5abd5ea28f71a58f

MSC runtime binding header SHA-256:
b0106d6f59bdd78af8280301f741c7a21cfac8ff3dbda30ad9f53927eda9ef47

MSC receipt validator SHA-256:
2737d367161f96cbd86e6450ef52fe425b1d1adcf37e97f4bf5eab2600d27f92
```

The runtime launcher validated the MSC receipt and all nine metallib and
reflection hashes before creating a Metal device. `MTL_DEBUG_LAYER=1` and
`MTL_SHADER_VALIDATION=1` were both set before device creation.

## Metallib hashes

```text
deffer_base_bump.vs       36de2f888980629c62d9e8240923d33e671b14ef03567a0b67cf16b7dd186ac4
deffer_base_bump.ps       22d5ff4ef98e9c7b249ea479eaec2218550ecd546028bfb771edc9aa410a493d
deffer_model_bump.vs      bac1c8c5550fc6894ad6b89c27c5dbc72d2e804789c8bd82f0aa7e4df4c14b73
accum_sun_nomsaa.ps       50c427a0a06608fa8dde3c0cf95730930fe54fa81eb7fd233432b264df640de2
shadow_direct_tree.vs     18448ada281c9d651da08414516d2e4481dc3114c84687db31cae3b16ffb4502
deffer_grass.vs           7b6475f1c5f1e1eddc6a8a48862c58530bdd54d22ba256ff897e58c166375463
ssfx_water.ps             a485d69418123f679f19223d5ab5b2def9d151f906136e76a38f00e4c31bdabb
ssfx_taa.ps               f2f0c902b3867e3e48c37b2d36f64e37de1b2857794cc93e6fca3edfec74db87
hud_font.ps               4c7b5b4eb6edc1a06c0383b6350b093a96cde99dedb60d885737fd614d458b17
```

## Successful runtime result

```text
Metal API Validation Enabled
Metal GPU Validation Enabled
METAL_FUNCTION_READY deffer_base_bump.vs type=1
METAL_FUNCTION_READY deffer_base_bump.ps type=2
METAL_FUNCTION_READY deffer_model_bump.vs type=1
METAL_FUNCTION_READY accum_sun_nomsaa.ps type=2
METAL_FUNCTION_READY shadow_direct_tree.vs type=1
METAL_FUNCTION_READY deffer_grass.vs type=1
METAL_FUNCTION_READY ssfx_water.ps type=2
METAL_FUNCTION_READY ssfx_taa.ps type=2
METAL_FUNCTION_READY hud_font.ps type=2
METAL_SHADOW_PSO_READY depth=Depth32Float vertex_attribute=11 vertex_buffer=6
METAL_GBUFFER_PSO_READY layout=dwDecl_01W stride=36 render_targets=4 formats=RGBA16Float,RGBA8Unorm,RGBA8Unorm,RGBA16Float depth=Depth32Float vertex_attribute_base=11 vertex_buffer=6
METAL_SHADER_GATE_COMPLETE device="Apple M1 Pro" functions=9
```

## Boundary

This receipt proves real-source compilation, conversion, structural
reflection, Metal library/function loading, and constructibility of the shadow
and representative four-target skinned G-buffer pipelines. It does not prove
argument-buffer population, residency declarations, command encoding, a draw,
or visual parity.
