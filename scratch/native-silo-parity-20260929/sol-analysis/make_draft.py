from pathlib import Path
import difflib
import hashlib
import json

ROOT = Path(__file__).resolve().parents[3]
OUT = Path(__file__).resolve().parent
BASE = 'src/Layers/xrRenderMetal/'
FILES = [BASE + name for name in (
    'MetalR4LocalLightAdapter.cpp', 'MetalR4LocalLightAdapter.h',
    'MetalR4Backend.mm', 'IR4Backend.h',
    'tests/R4LocalLightRolloverGpuProbe.cpp')]
original = {name: (ROOT / name).read_text() for name in FILES}
if (OUT / 'source-manifest.json').exists():
    expected = json.loads((OUT / 'source-manifest.json').read_text())
    assert all(hashlib.sha256(original[name].encode()).hexdigest() ==
        expected[name]['prestate_sha256'] for name in FILES), 'production prestate changed'
changed = dict(original)

def replace(name, before, after):
    assert changed[name].count(before) == 1, (name, before[:120], changed[name].count(before))
    changed[name] = changed[name].replace(before, after, 1)

adapter = BASE + 'MetalR4LocalLightAdapter.cpp'
replace(adapter, '    std::array<std::shared_ptr<IR4GraphicsPipeline>, 2> maskFront;',
        '    bool bindReuse = false;\n    std::array<std::shared_ptr<IR4GraphicsPipeline>, 2> maskFront;')
replace(adapter,
    '    m_ready = false; m_status.clear(); m_telemetry = {}; m_impl = std::make_unique<Impl>();',
    '    m_ready = false; m_status.clear(); m_telemetry = {}; m_impl = std::make_unique<Impl>();\n'
    '    const char* bindReuse = std::getenv("XRAY_METAL_LOCAL_LIGHT_BIND_REUSE");\n'
    '    m_impl->bindReuse = bindReuse && std::strcmp(bindReuse, "1") == 0;')
replace(adapter,
    '''            const R4WholeLightVisibilitySample* visibilitySample) {
            if (!backend.BindGraphicsPipeline(pipeline,error) || !backend.SetStencilReference(reference,error) ||
                !backend.BindVertexBuffer(6u,m_impl->vertices[volumeKind],0u,error) ||
                !backend.BindMscResources(vsBindings,error) || !backend.BindMscResources(fragmentBindings,error))
                return false;
            if (!visibilitySample) return backend.DrawIndexed(draw,error);''',
    '''            const R4WholeLightVisibilitySample* visibilitySample,
            const bool reuseVertex, const bool reuseFragment) {
            if (!backend.BindGraphicsPipeline(pipeline,error) || !backend.SetStencilReference(reference,error) ||
                !backend.BindVertexBuffer(6u,m_impl->vertices[volumeKind],0u,error))
                return false;
            // Only the immediate front/back/accumulation triplet of this light
            // can reuse its immutable TLABs. The first draw always binds both
            // stages; the final draw always binds the accumulation PS layout.
            // Pipeline changes and PS-only binds preserve VS slot 2, and the
            // MSC indexed draw helper writes auxiliary slots 4/5 only.
            if (!reuseVertex)
            {
                if (!backend.BindMscResources(vsBindings,error)) return false;
                ++m_telemetry.mscVertexBinds;
            }
            else ++m_telemetry.mscVertexReuses;
            if (!reuseFragment)
            {
                if (!backend.BindMscResources(fragmentBindings,error)) return false;
                ++m_telemetry.mscFragmentBinds;
            }
            else ++m_telemetry.mscFragmentReuses;
            if (!visibilitySample) return backend.DrawIndexed(draw,error);''')
replace(adapter,
    '''        if (!bindDraw(m_impl->maskFront[shaderKind], maskPsBindings, marker, nullptr) ||
            !bindDraw(m_impl->maskBack[shaderKind], maskPsBindings, 1u, nullptr)) return false;''',
    '''        if (!bindDraw(m_impl->maskFront[shaderKind], maskPsBindings, marker, nullptr, false, false) ||
            !bindDraw(m_impl->maskBack[shaderKind], maskPsBindings, 1u, nullptr,
                m_impl->bindReuse, m_impl->bindReuse)) return false;''')
replace(adapter,
    '''                m_impl->accumulationUnshadowed[shaderKind], psBindings, marker, visibilitySample)) return false;''',
    '''                m_impl->accumulationUnshadowed[shaderKind], psBindings, marker, visibilitySample,
                m_impl->bindReuse, false)) return false;''')
header = BASE + 'MetalR4LocalLightAdapter.h'
replace(header, '        std::size_t markerResets = 0;',
    '''        std::size_t markerResets = 0;
        std::size_t mscVertexBinds = 0;
        std::size_t mscFragmentBinds = 0;
        std::size_t mscVertexReuses = 0;
        std::size_t mscFragmentReuses = 0;''')

# Offscreen oracle is a narrow extension of the existing admitted production
# local-light rollover fixture, rather than a new shader or rendering model.
backend = BASE + 'MetalR4Backend.mm'
replace(backend, '    static bool ProbeWholeLightVisibilityGpu(std::string& error);',
    '''    static bool ProbeWholeLightVisibilityGpu(std::string& error);
    friend bool ::RunMetalR4LocalLightRolloverGpuProbe(const std::string&,
        const std::string&, std::size_t, std::string&, bool);''')
replace(backend,
    '''bool RunMetalR4LocalLightRolloverGpuProbe(const std::string& shaderDirectory,
    const std::string& artifactDirectory, const std::size_t lightCount, std::string& error)''',
    '''bool RunMetalR4LocalLightRolloverGpuProbe(const std::string& shaderDirectory,
    const std::string& artifactDirectory, const std::size_t lightCount, std::string& error,
    const bool compareBindReuse)''')
replace(backend, '''        constexpr std::array<float, 4> Seed = {0.0625f, 0.125f, 0.1875f, 0.25f};
        if (shaderDirectory.empty()''',
    '''        constexpr std::array<float, 4> Seed = {0.0625f, 0.125f, 0.1875f, 0.25f};
        struct SavedEnv
        {
            const char* name;
            bool present;
            std::string value;
            explicit SavedEnv(const char* key) : name(key), present(std::getenv(key) != nullptr),
                value(present ? std::getenv(key) : "") {}
            ~SavedEnv() { if (present) ::setenv(name, value.c_str(), 1); else ::unsetenv(name); }
        } bindEnv("XRAY_METAL_LOCAL_LIGHT_BIND_REUSE"),
            lifecycleEnv("XRAY_METAL_TEMPORAL_LIFECYCLE_PROBE"),
            retryEnv("XRAY_METAL_TEMPORAL_FORCE_ACQUIRE_RETRY");
        const char* familyText = compareBindReuse ?
            std::getenv("XRAY_METAL_LOCAL_LIGHT_BIND_REUSE_PROBE_TYPE") : nullptr;
        const std::string family = familyText ? familyText : "mixed";
        const char* shadowText = compareBindReuse ?
            std::getenv("XRAY_METAL_LOCAL_LIGHT_BIND_REUSE_PROBE_SHADOWED") : nullptr;
        const bool shadowFixture = shadowText && std::strcmp(shadowText, "1") == 0;
        if ((family != "mixed" && family != "point" && family != "spot" && family != "omni") ||
            (shadowFixture && family == "point"))
        { error = "invalid bind-reuse probe family or unsupported shadowed point"; return false; }
        if (compareBindReuse) ::setenv("XRAY_METAL_LOCAL_LIGHT_BIND_REUSE", "0", 1);
        if (shaderDirectory.empty()''')
replace(backend, '''            MetalR4LocalLightAdapter adapter;
            MetalR4LocalLightAdapter::Options adapterOptions;''',
    '''            MetalR4LocalLightAdapter adapter;
            MetalR4LocalLightAdapter reusedAdapter;
            MetalR4LocalLightAdapter::Options adapterOptions;''')
# Find this function's admitted adapter preparation block without matching
# other backend fixture code.
anchor = changed[backend].index('bool RunMetalR4LocalLightRolloverGpuProbe(')
suffix = changed[backend][anchor:]
prepare_end = suffix.index('            const std::array<std::uint16_t, 6>')
prepare_block = suffix[:prepare_end]
needle = '            const std::array<std::uint16_t, 6>'
replace(backend, needle,
    '''            if (compareBindReuse)
            {
                ::setenv("XRAY_METAL_LOCAL_LIGHT_BIND_REUSE", "1", 1);
                if (!reusedAdapter.Prepare(*backend, adapterOptions, adapterStatus) || !reusedAdapter.IsReady())
                { fail("reuse adapter production MSC admission failed: " + adapterStatus); break; }
            }
            const std::array<std::uint16_t, 6>''')
replace(backend, '''                light.position = {0.0f, 0.0f, 0.1f};''',
    '''                if (family == "point") light.type = R4EngineLocalLightType::Point;
                else if (family == "spot") light.type = R4EngineLocalLightType::Spot;
                else if (family == "omni") light.type = R4EngineLocalLightType::OmniFace;
                if (shadowFixture && light.type == R4EngineLocalLightType::Point)
                    light.type = R4EngineLocalLightType::Spot;
                light.shadowed = shadowFixture;
                light.omniFace = light.type == R4EngineLocalLightType::OmniFace ? i % 6u : 0u;
                light.position = {compareBindReuse ? (float(int(i % 5u) - 2) * 0.008f) : 0.0f,
                    0.0f, 0.1f};''')
replace(backend, '''                light.range = 0.6f; light.cone = 1.2f; light.virtualSize = 1.0f;''',
    '''                light.range = compareBindReuse ? 0.6f - float(i % 5u) * 0.002f : 0.6f;
                light.cone = 1.2f; light.virtualSize = 1.0f;''')
replace(backend, '''            struct Captured
            {
                std::shared_ptr<IR4Texture> color;''',
    '''            std::shared_ptr<IR4Texture> shadowAtlas;
            std::shared_ptr<IR4Texture> blockedShadowAtlas;
            std::shared_ptr<IR4Sampler> shadowSampler;
            std::vector<xray::metal::R4LocalShadowAtlasRecord> shadowRecords;
            if (shadowFixture)
            {
                R4Texture2DDesc desc; desc.width = desc.height = Width;
                desc.format = R4PixelFormat::R32Float; desc.storage = R4ResourceStorage::Shared;
                desc.usage = R4TextureUsage::ShaderRead; desc.label = "bind reuse positive shadow atlas";
                shadowAtlas = resources.CreateTexture2D(desc, error);
                R4SamplerDesc samplerDesc = linearDesc;
                samplerDesc.compareFunction = R4SamplerCompareFunction::LessEqual;
                samplerDesc.addressU = samplerDesc.addressV = samplerDesc.addressW =
                    R4SamplerAddressMode::ClampToBorder;
                samplerDesc.borderColor = R4SamplerBorderColor::OpaqueWhite;
                samplerDesc.label = "bind reuse production shadow comparison sampler";
                shadowSampler = resources.CreateSampler(samplerDesc, error);
                std::array<float, Width * Width> depth{}; depth.fill(0.95f);
                if (!shadowAtlas || !shadowSampler || !resources.UploadTexture2D(shadowAtlas, 0u, 0u, depth.data(),
                        sizeof(depth), Width * sizeof(float), sizeof(depth), error))
                { fail("bind reuse shadow fixture allocation/upload failed: " + error); break; }
                desc.label = "bind reuse blocked shadow atlas";
                blockedShadowAtlas = resources.CreateTexture2D(desc, error);
                depth.fill(0.0f);
                if (!blockedShadowAtlas || !resources.UploadTexture2D(blockedShadowAtlas, 0u, 0u,
                        depth.data(), sizeof(depth), Width * sizeof(float), sizeof(depth), error))
                { fail("bind reuse blocked shadow fixture allocation/upload failed: " + error); break; }
                const std::array<float, 16> identity{{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}};
                for (const auto& light : lightSnapshot.lights)
                {
                    xray::metal::R4LocalShadowAtlasRecord record;
                    record.lightIdentity = light.lightId; record.engineFrame = lightSnapshot.engineFrame;
                    record.atlasExtent = record.atlasSize = Width; record.face = light.omniFace;
                    record.renderedDepth = true; record.lightView = record.lightProjection = identity;
                    record.deviceInverseView = record.shadowMatrix = record.lightMapMatrix = identity;
                    shadowRecords.push_back(record);
                }
            }
            struct Captured
            {
                std::shared_ptr<IR4Texture> color;''')
replace(backend, '''                std::vector<__fp16> verify;
                std::size_t resets = 0;''',
    '''                std::vector<__fp16> verify;
                std::vector<std::uint16_t> colorBits;
                std::vector<std::uint32_t> depthBits;
                std::vector<std::uint8_t> stencilBits;
                id<MTLBuffer> aspectReadback = nil;
                MetalR4LocalLightAdapter::Telemetry telemetry;
                std::size_t resets = 0;''')
replace(backend, '            std::array<Captured, 8> captures;',
    '            std::vector<Captured> captures(compareBindReuse ? (shadowFixture ? 17u : 16u) : 8u);')
replace(backend, '''            const auto renderOne = [&](Captured& capture, const std::size_t pose, const std::size_t mode) {''',
    '''            const auto renderOne = [&](Captured& capture, const std::size_t pose,
                const std::size_t mode, MetalR4LocalLightAdapter& activeAdapter,
                const bool blockedShadow = false) {''')
replace(backend, '''                inputs.noFilterSampler = nearest; inputs.linearSampler = linear;
                MetalR4LocalLightAdapter::ViewState view;''',
    '''                inputs.noFilterSampler = nearest; inputs.linearSampler = linear;
                if (shadowFixture)
                {
                    inputs.shadowAtlas = blockedShadow ? blockedShadowAtlas : shadowAtlas;
                    inputs.shadowSampler = shadowSampler;
                    inputs.shadowRecords = &shadowRecords;
                }
                MetalR4LocalLightAdapter::ViewState view;''')
# Adapt only the rollover render lambda's adapter identifier.
start = changed[backend].index('            const auto renderOne = [&](Captured& capture')
end = changed[backend].index('            for (std::size_t pose = 0; pose < 2u; ++pose)', start)
block = changed[backend][start:end]
block = block.replace('adapter.DrawIntoActiveAccumulator', 'activeAdapter.DrawIntoActiveAccumulator')
block = block.replace('adapter.LastTelemetry()', 'activeAdapter.LastTelemetry()')
block = block.replace('                    capture.submitted = activeAdapter.LastTelemetry().submitted;',
    '''                    capture.submitted = activeAdapter.LastTelemetry().submitted;
                    capture.telemetry = activeAdapter.LastTelemetry();''')
block = block.replace('                        capture.submitted += activeAdapter.LastTelemetry().submitted;',
    '''                        capture.submitted += activeAdapter.LastTelemetry().submitted;
                        const auto& t = activeAdapter.LastTelemetry();
                        capture.telemetry.mscVertexBinds += t.mscVertexBinds;
                        capture.telemetry.mscFragmentBinds += t.mscFragmentBinds;
                        capture.telemetry.mscVertexReuses += t.mscVertexReuses;
                        capture.telemetry.mscFragmentReuses += t.mscFragmentReuses;''')
block = block.replace('                return true;\n            };',
    '''                if (compareBindReuse)
                {
                    constexpr std::size_t RowBytes = 256u;
                    constexpr std::size_t StencilOffset = RowBytes * Height;
                    auto& nativeBackend = static_cast<MetalR4Backend&>(*backend);
                    auto& frame = nativeBackend.m_frames[nativeBackend.m_activeFrameIndex];
                    capture.aspectReadback = [nativeBackend.m_device
                        newBufferWithLength:StencilOffset * 2u options:MTLResourceStorageModeShared];
                    id<MTLBlitCommandEncoder> blit = [frame.commandBuffer blitCommandEncoder];
                    const auto depth = std::dynamic_pointer_cast<MetalTexture>(capture.depthStencil);
                    if (!capture.aspectReadback || !blit || !depth)
                    { error = "bind reuse exact depth/stencil readback allocation failed"; return false; }
                    const MTLSize size = MTLSizeMake(Width, Height, 1);
                    [blit copyFromTexture:depth->Native() sourceSlice:0 sourceLevel:0
                        sourceOrigin:MTLOriginMake(0,0,0) sourceSize:size toBuffer:capture.aspectReadback
                        destinationOffset:0 destinationBytesPerRow:RowBytes destinationBytesPerImage:0
                        options:MTLBlitOptionDepthFromDepthStencil];
                    [blit copyFromTexture:depth->Native() sourceSlice:0 sourceLevel:0
                        sourceOrigin:MTLOriginMake(0,0,0) sourceSize:size toBuffer:capture.aspectReadback
                        destinationOffset:StencilOffset destinationBytesPerRow:RowBytes destinationBytesPerImage:0
                        options:MTLBlitOptionStencilFromDepthStencil];
                    [blit endEncoding];
                }
                return true;
            };''')
assert 'RowBytes' in block
changed[backend] = changed[backend][:start] + block + changed[backend][end:]
replace(backend, '''            for (std::size_t pose = 0; pose < 2u; ++pose)
            {
                const std::size_t base = pose * 4u;
                for (std::size_t mode = 0; mode < 4u; ++mode)
                    if (!renderOne(captures[base + mode], pose, mode))
                    { fail("bulk/reference/tail adapter rendering failed: " + error); break; }
                if (!error.empty()) break;
            }
            if (!error.empty()) break;
            if (!backend->SubmitFrame(error))
            { fail("rollover probe frame submit failed: " + error); break; }''',
    '''            if (compareBindReuse)
            {
                // Encode a real triplet and abandon it, then exercise the
                // production no-drawable retry before any compared submission.
                if (!renderOne(captures.back(), 0u, 0u, reusedAdapter)) break;
                backend->DiscardFrame();
                ::setenv("XRAY_METAL_TEMPORAL_LIFECYCLE_PROBE", "1", 1);
                ::setenv("XRAY_METAL_TEMPORAL_FORCE_ACQUIRE_RETRY", "1", 1);
                if (backend->BeginFrame(error) != R4FrameAcquireResult::Retry)
                { fail("bind reuse oracle did not exercise actual acquire Retry"); break; }
                ::unsetenv("XRAY_METAL_TEMPORAL_LIFECYCLE_PROBE");
                if (backend->BeginFrame(error) != R4FrameAcquireResult::Ready)
                { fail("bind reuse oracle failed after discard/acquire retry"); break; }
                captures.back().resets = captures.back().submitted = 0u;
                captures.back().telemetry = {};
            }
            const std::size_t armCount = compareBindReuse ? 2u : 1u;
            for (std::size_t arm = 0; arm < armCount; ++arm)
            {
                auto& activeAdapter = arm == 0u ? adapter : reusedAdapter;
                for (std::size_t pose = 0; pose < 2u; ++pose)
                {
                    const std::size_t base = arm * 8u + pose * 4u;
                    for (std::size_t mode = 0; mode < 4u; ++mode)
                    {
                        if (!renderOne(captures[base + mode], pose, mode, activeAdapter))
                        { fail("bulk/reference/tail adapter rendering failed: " + error); break; }
                        if (compareBindReuse)
                        {
                            if (!backend->SubmitFrame(error)) break;
                            if (base + mode + 1u < 16u &&
                                backend->BeginFrame(error) != R4FrameAcquireResult::Ready)
                            { fail("bind reuse oracle acquire failed during submitted slot wrap"); break; }
                        }
                    }
                    if (!error.empty()) break;
                }
                if (!error.empty()) break;
            }
            if (!error.empty()) break;
            if (compareBindReuse && shadowFixture)
            {
                // Same adapter, PSO, light transforms and receivers as the
                // lit on-arm. Only the atlas depths change from .95 to 0.
                if (backend->BeginFrame(error) != R4FrameAcquireResult::Ready ||
                    !renderOne(captures[16u], 0u, 0u, reusedAdapter, true) ||
                    !backend->SubmitFrame(error))
                { fail("bind reuse blocked-shadow render failed: " + error); break; }
            }
            if (!compareBindReuse && !backend->SubmitFrame(error))
            { fail("rollover probe frame submit failed: " + error); break; }''')
replace(backend, '''                capture.verify = std::move(proof);
                for (std::size_t i = 0; i < capture.rgba.size(); ++i)''',
    '''                capture.verify = std::move(proof);
                if (compareBindReuse)
                {
                    capture.colorBits.resize(PixelCount * 4u);
                    std::memcpy(capture.colorBits.data(), half.data(), half.size() * sizeof(__fp16));
                    capture.depthBits.resize(PixelCount); capture.stencilBits.resize(PixelCount);
                    const auto* bytes = static_cast<const std::uint8_t*>(capture.aspectReadback.contents);
                    if (!bytes) { fail("bind reuse exact aspect readback is missing"); break; }
                    for (std::size_t row = 0; row < Height; ++row)
                    {
                        std::memcpy(capture.depthBits.data() + row * Width,
                            bytes + row * 256u, Width * sizeof(float));
                        std::memcpy(capture.stencilBits.data() + row * Width,
                            bytes + 256u * Height + row * 256u, Width);
                    }
                }
                for (std::size_t i = 0; i < capture.rgba.size(); ++i)''')
replace(backend, '''            float maxDifference = 0.0f;
            float maxGeometrySignal = 0.0f;''',
    '''            float maxShadowSignalDelta = 0.0f;
            std::size_t shadowSignalPixels = 0u;
            bool shadowDepthStencilPreserved = false;
            if (compareBindReuse)
            {
                for (std::size_t index = 0; index < 8u; ++index)
                {
                    const auto& off = captures[index]; const auto& on = captures[index + 8u];
                    if (off.colorBits != on.colorBits || off.depthBits != on.depthBits ||
                        off.stencilBits != on.stencilBits || off.verify != on.verify ||
                        off.resets != on.resets || off.submitted != on.submitted ||
                        off.telemetry.mscVertexBinds != off.submitted * 3u ||
                        off.telemetry.mscFragmentBinds != off.submitted * 3u ||
                        off.telemetry.mscVertexReuses != 0u || off.telemetry.mscFragmentReuses != 0u ||
                        on.telemetry.mscVertexBinds != on.submitted ||
                        on.telemetry.mscFragmentBinds != on.submitted * 2u ||
                        on.telemetry.mscVertexReuses != on.submitted * 2u ||
                        on.telemetry.mscFragmentReuses != on.submitted)
                    { fail("bind reuse off/on color/depth/stencil or exact work-count oracle failed"); break; }
                }
                if (!error.empty()) break;
            }
            if (compareBindReuse && shadowFixture)
            {
                const auto& lit = captures[8u]; const auto& blocked = captures[16u];
                shadowDepthStencilPreserved = lit.depthBits == blocked.depthBits &&
                    lit.stencilBits == blocked.stencilBits && lit.verify == blocked.verify;
                if (!shadowDepthStencilPreserved || lit.submitted != blocked.submitted ||
                    lit.resets != blocked.resets)
                { fail("changing shadow atlas depths changed volume coverage/depth/stencil"); break; }
                for (std::size_t pixel = 0; pixel < PixelCount; ++pixel)
                {
                    bool changed = false;
                    for (std::size_t channel = 0; channel < 4u; ++channel)
                    {
                        const std::size_t at = pixel * 4u + channel;
                        if (!std::isfinite(lit.rgba[at]) || !std::isfinite(blocked.rgba[at]))
                        { fail("lit/blocked shadow comparison produced non-finite pixels"); break; }
                        if (channel < 3u && pixel % Width < Width / 2u)
                        {
                            const float delta = lit.rgba[at] - blocked.rgba[at];
                            maxShadowSignalDelta = std::max(maxShadowSignalDelta, delta);
                            changed = changed || delta > 0.001f;
                        }
                    }
                    if (!error.empty()) break;
                    if (changed) ++shadowSignalPixels;
                }
                if (!error.empty()) break;
                if (maxShadowSignalDelta <= 0.001f || shadowSignalPixels == 0u)
                { fail("real shadowed MSC PSO ignored lit versus blocked atlas depths"); break; }
            }
            float maxDifference = 0.0f;
            float maxGeometrySignal = 0.0f;''')
replace(backend, r'''                << "  \"camera_poses\": 2,\n  \"bulk_marker_resets\": " << captures[0].resets''',
    r'''                << "  \"bind_reuse_compared\": " << (compareBindReuse ? "true" : "false")
                << ",\n  \"bind_reuse_family\": \"" << family << "\""
                << ",\n  \"bind_reuse_shadowed\": " << (shadowFixture ? "true" : "false")
                << ",\n  \"shadow_lit_depth\": 0.95,\n  \"shadow_blocked_depth\": 0.0"
                << ",\n  \"shadow_signal_pixels\": " << shadowSignalPixels
                << ",\n  \"max_shadow_lit_minus_blocked_delta\": " << maxShadowSignalDelta
                << ",\n  \"shadow_depth_stencil_preserved\": " << (shadowDepthStencilPreserved ? "true" : "false")
                << ",\n  \"exact_color_depth_stencil_pairs\": " << (compareBindReuse ? 8u : 0u)
                << ",\n  \"discard_retry_and_submitted_slot_wrap\": " << (compareBindReuse ? "true" : "false")
                << ",\n  \"off_vertex_binds\": " << captures[0].telemetry.mscVertexBinds
                << ",\n  \"off_fragment_binds\": " << captures[0].telemetry.mscFragmentBinds
                << ",\n  \"on_vertex_binds\": " << (compareBindReuse ? captures[8].telemetry.mscVertexBinds : 0u)
                << ",\n  \"on_fragment_binds\": " << (compareBindReuse ? captures[8].telemetry.mscFragmentBinds : 0u)
                << ",\n  \"camera_poses\": 2,\n  \"bulk_marker_resets\": " << captures[0].resets''')

api = BASE + 'IR4Backend.h'
replace(api, '''    std::size_t lightCount,
    std::string& error);''',
    '''    std::size_t lightCount,
    std::string& error,
    bool compareBindReuse = false);''')
cli = BASE + 'tests/R4LocalLightRolloverGpuProbe.cpp'
replace(cli, '''    if (argc != 5 || std::string(argv[1]) != "--gpu")''',
    '''    if (argc != 5 || (std::string(argv[1]) != "--gpu" && std::string(argv[1]) != "--bind-reuse-gpu"))''')
replace(cli, '''    const bool passed = RunMetalR4LocalLightRolloverGpuProbe(argv[2], argv[4], lightCount, error);''',
    '''    const bool passed = RunMetalR4LocalLightRolloverGpuProbe(argv[2], argv[4], lightCount, error,
        std::string(argv[1]) == "--bind-reuse-gpu");''')

def write_patch(names, filename):
    patch = []
    for name in names:
        patch.extend(difflib.unified_diff(original[name].splitlines(True), changed[name].splitlines(True),
            fromfile='a/' + name, tofile='b/' + name))
    (OUT / filename).write_text(''.join(patch))

for name in FILES:
    for directory, content in [('prestate', original[name]), ('draft', changed[name])]:
        path = OUT / directory / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
write_patch([adapter, header], 'local-light-bind-reuse.patch')
write_patch([backend, api, cli], 'local-light-bind-reuse-oracle.patch')
(OUT / 'source-manifest.json').write_text(json.dumps({name: {
    'prestate_sha256': hashlib.sha256(original[name].encode()).hexdigest(),
    'draft_sha256': hashlib.sha256(changed[name].encode()).hexdigest(),
} for name in FILES}, indent=2) + '\n')
print(json.dumps({'status': 'DRAFT_ONLY', 'files': len(FILES), 'patches': [
    'local-light-bind-reuse.patch', 'local-light-bind-reuse-oracle.patch']}))
