cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(XRayMetalBootstrap LANGUAGES CXX OBJC OBJCXX)
else()
  enable_language(OBJC)
  enable_language(OBJCXX)
endif()

include(CTest)

add_library(XRay.Metal.R4Backend STATIC
  MetalR4DefObjectsLodAdapter.cpp
  MetalR4DefObjectsLodAdapter.h
  MetalR4DefVertexAdapter.cpp
  MetalR4DefVertexAdapter.h
  MetalR4DetailGrassAdapter.cpp
  MetalR4DetailGrassAdapter.h
  MetalR4DynamicWorldAdapter.cpp
  MetalR4DynamicWorldAdapter.h
  MetalR4FloraAdapter.cpp
  MetalR4FloraAdapter.h
  MetalR4HudAdapter.cpp
  MetalR4HudAdapter.h
  MetalR4ScopeAdapter.cpp
  MetalR4ScopeAdapter.h
  MetalR4LocalLightAdapter.cpp
  MetalR4LocalLightAdapter.h
  MetalR4ParticleAdapter.cpp
  MetalR4ParticleAdapter.h
  MetalR4RainAdapter.cpp
  MetalR4RainAdapter.h
  MetalR4LensFlareAdapter.h
  MetalR4LensFlareAdapter.cpp
  MetalR4StaticForwardAdapter.cpp
  MetalR4StaticForwardAdapter.h
  MetalR4SsfxAoAdapter.cpp
  MetalR4SsfxAoAdapter.h
  MetalR4WaterAdapter.cpp
  MetalR4WaterAdapter.h
  R4LocalLightFoundation.cpp
  R4LocalLightFoundation.h
  R4LocalShadowAtlasFoundation.cpp
  R4LocalShadowAtlasFoundation.h
  R4ParticleFoundation.cpp
  R4ParticleFoundation.h
  R4StaticForwardFoundation.cpp
  R4StaticForwardFoundation.h
  R4WaterFoundation.cpp
  R4WaterFoundation.h
  R4HudFoundation.cpp
  R4HudFoundation.h
  R4NightVisionFoundation.cpp
  R4NightVisionFoundation.h
  R4DefVertexFoundation.cpp
  R4DefVertexFoundation.h
  R4DefObjectsLodFoundation.cpp
  R4DefObjectsLodFoundation.h
  MetalR4EngineFs.cpp
  MetalR4EngineFs.h
  MetalR4UiVideo.cpp
  MetalR4UiVideo.h
  MetalR4EngineScene.mm
  MetalR4EngineScene.h
  MetalR4Backend.mm
  MetalScreenshotCodec.cpp
  MetalScreenshotCodec.h
  R4DdsTexture.cpp
  R4DdsTexture.h
  R4MaterialSet.cpp
  R4MaterialSet.h
  R4LevelGeometry.cpp
  R4LevelGeometry.h
  IR4Backend.h
  IR4Pipeline.h
  IR4Resources.h
)
target_compile_options(XRay.Metal.R4Backend PRIVATE -fobjc-arc)
target_compile_features(XRay.Metal.R4Backend PUBLIC cxx_std_17)
target_include_directories(XRay.Metal.R4Backend PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  /opt/homebrew/include/SDL2
)
target_link_libraries(XRay.Metal.R4Backend PUBLIC
  /opt/homebrew/lib/libSDL2.dylib
  "-framework Cocoa"
  "-framework Metal"
  "-framework QuartzCore"
  "-framework ImageIO"
)

add_executable(XRay.Metal.Bootstrap
  MetalBootstrap.mm
)

set_target_properties(XRay.Metal.Bootstrap
  PROPERTIES
  OUTPUT_NAME AnomalyMetalBootstrap
)

target_compile_options(XRay.Metal.Bootstrap PRIVATE -fobjc-arc)
target_compile_features(XRay.Metal.Bootstrap PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.Bootstrap PRIVATE XRay.Metal.R4Backend)
target_link_options(XRay.Metal.Bootstrap PRIVATE "-Wl,-rpath,/opt/homebrew/lib")

add_executable(XRay.Metal.BackendSelectionSmoke
  tests/R4BackendSelectionSmoke.cpp
  ../../Include/xrRender/R4BackendSelection.h
)
target_compile_features(XRay.Metal.BackendSelectionSmoke PRIVATE cxx_std_17)

add_executable(XRay.Metal.ScreenshotFallbackProbe
  tests/MetalScreenshotFallbackProbe.cpp
  MetalScreenshotFallback.h
)
target_compile_features(XRay.Metal.ScreenshotFallbackProbe PRIVATE cxx_std_17)
add_test(
  NAME XRay.Metal.ScreenshotFallback.SelfTest
  COMMAND XRay.Metal.ScreenshotFallbackProbe
)

add_executable(XRay.Metal.ScreenshotCodecProbe
  tests/MetalScreenshotCodecProbe.cpp
  MetalScreenshotCodec.cpp
  MetalScreenshotCodec.h
  R4DdsTexture.cpp
  R4DdsTexture.h
)
target_compile_features(XRay.Metal.ScreenshotCodecProbe PRIVATE cxx_std_17)
add_test(
  NAME XRay.Metal.ScreenshotCodec.SelfTest
  COMMAND XRay.Metal.ScreenshotCodecProbe
)

add_executable(XRay.Metal.UICaptureProbe
  tests/MetalUICaptureProbe.cpp
  ../xrRender/MetalUICapture.h
)
target_compile_features(XRay.Metal.UICaptureProbe PRIVATE cxx_std_17)
add_test(
  NAME XRay.Metal.UICapture.SelfTest
  COMMAND XRay.Metal.UICaptureProbe
)

add_executable(XRay.Metal.UIOverlayProbe
  tests/MetalUIOverlayProbe.cpp
  MetalUIOverlayCommon.h
  ../xrRender/MetalUICapture.h
)
target_compile_features(XRay.Metal.UIOverlayProbe PRIVATE cxx_std_17)
add_test(
  NAME XRay.Metal.UIOverlay.SelfTest
  COMMAND XRay.Metal.UIOverlayProbe
)

add_executable(XRay.Metal.LevelGeometryProbe
  tests/R4LevelGeometryProbe.cpp
)
target_compile_features(XRay.Metal.LevelGeometryProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.LevelGeometryProbe PRIVATE XRay.Metal.R4Backend)
add_test(
  NAME XRay.Metal.LevelGeometry.SelfTest
  COMMAND XRay.Metal.LevelGeometryProbe --self-test
)

add_executable(XRay.Metal.DdsTextureProbe
  tests/R4DdsTextureProbe.cpp
)
target_compile_features(XRay.Metal.DdsTextureProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.DdsTextureProbe PRIVATE XRay.Metal.R4Backend)
add_test(
  NAME XRay.Metal.DdsTexture.SelfTest
  COMMAND XRay.Metal.DdsTextureProbe --self-test
)

add_executable(XRay.Metal.MaterialSetProbe
  tests/R4MaterialSetProbe.cpp
)
target_compile_features(XRay.Metal.MaterialSetProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.MaterialSetProbe PRIVATE XRay.Metal.R4Backend)
add_test(
  NAME XRay.Metal.MaterialSet.SelfTest
  COMMAND XRay.Metal.MaterialSetProbe --self-test
)

add_executable(XRay.Metal.EngineWorldSnapshotProbe
  tests/R4EngineWorldSnapshotProbe.cpp
)
target_compile_features(XRay.Metal.EngineWorldSnapshotProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.EngineWorldSnapshotProbe PRIVATE XRay.Metal.R4Backend)
add_test(
  NAME XRay.Metal.EngineWorldSnapshot.SelfTest
  COMMAND XRay.Metal.EngineWorldSnapshotProbe --self-test
)

add_executable(XRay.Metal.LocalLightSnapshotProbe
  tests/R4LocalLightSnapshotProbe.cpp
)
target_compile_features(XRay.Metal.LocalLightSnapshotProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.LocalLightSnapshotProbe PRIVATE XRay.Metal.R4Backend)
add_test(
  NAME XRay.Metal.LocalLightSnapshot.SelfTest
  COMMAND XRay.Metal.LocalLightSnapshotProbe
)

add_executable(XRay.Metal.DynamicWorldAdapterProbe
  tests/R4DynamicWorldAdapterProbe.cpp
)
target_compile_features(XRay.Metal.DynamicWorldAdapterProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.DynamicWorldAdapterProbe PRIVATE XRay.Metal.R4Backend)
add_test(
  NAME XRay.Metal.DynamicWorldAdapter.SelfTest
  COMMAND XRay.Metal.DynamicWorldAdapterProbe --self-test
)

add_executable(XRay.Metal.FloraAdapterProbe
  tests/R4FloraAdapterProbe.cpp
)
target_compile_features(XRay.Metal.FloraAdapterProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.FloraAdapterProbe PRIVATE XRay.Metal.R4Backend)
add_test(
  NAME XRay.Metal.FloraAdapter.SelfTest
  COMMAND XRay.Metal.FloraAdapterProbe --self-test
)

add_executable(XRay.Metal.DetailGrassAdapterProbe
  tests/R4DetailGrassAdapterProbe.cpp
)
target_compile_features(XRay.Metal.DetailGrassAdapterProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.DetailGrassAdapterProbe PRIVATE XRay.Metal.R4Backend)
add_test(
  NAME XRay.Metal.DetailGrassAdapter.SelfTest
  COMMAND XRay.Metal.DetailGrassAdapterProbe --self-test
)

add_executable(XRay.Metal.EngineSceneFrustumProbe
  tests/MetalR4EngineSceneFrustumProbe.cpp
)
target_compile_features(XRay.Metal.EngineSceneFrustumProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.EngineSceneFrustumProbe PRIVATE XRay.Metal.R4Backend)
add_test(
  NAME XRay.Metal.EngineSceneFrustum.SelfTest
  COMMAND XRay.Metal.EngineSceneFrustumProbe --self-test
)

add_executable(XRay.Metal.DefVertexFoundationProbe
  tests/R4DefVertexFoundationProbe.cpp
)
target_compile_features(XRay.Metal.DefVertexFoundationProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.DefVertexFoundationProbe PRIVATE XRay.Metal.R4Backend)
add_test(
  NAME XRay.Metal.DefVertexFoundation.SelfTest
  COMMAND XRay.Metal.DefVertexFoundationProbe --self-test
)

add_executable(XRay.Metal.LocalLightFoundationProbe
  tests/R4LocalLightFoundationProbe.cpp
)
target_compile_features(XRay.Metal.LocalLightFoundationProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.LocalLightFoundationProbe PRIVATE XRay.Metal.R4Backend)
add_test(
  NAME XRay.Metal.LocalLightFoundation.SelfTest
  COMMAND XRay.Metal.LocalLightFoundationProbe --self-test
)

add_executable(XRay.Metal.LocalShadowAtlasFoundationProbe
  tests/R4LocalShadowAtlasFoundationProbe.cpp
)
target_compile_features(XRay.Metal.LocalShadowAtlasFoundationProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.LocalShadowAtlasFoundationProbe PRIVATE XRay.Metal.R4Backend)
add_test(
  NAME XRay.Metal.LocalShadowAtlasFoundation.SelfTest
  COMMAND XRay.Metal.LocalShadowAtlasFoundationProbe
)

add_executable(XRay.Metal.ParticleFoundationProbe
  tests/R4ParticleFoundationProbe.cpp
)
target_compile_features(XRay.Metal.ParticleFoundationProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.ParticleFoundationProbe PRIVATE XRay.Metal.R4Backend)
add_test(
  NAME XRay.Metal.ParticleFoundation.SelfTest
  COMMAND XRay.Metal.ParticleFoundationProbe --self-test
)

add_executable(XRay.Metal.ParticleAdapterProbe
  tests/MetalR4ParticleAdapterProbe.cpp
)
target_compile_features(XRay.Metal.ParticleAdapterProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.ParticleAdapterProbe PRIVATE XRay.Metal.R4Backend)
add_test(
  NAME XRay.Metal.ParticleAdapter.SelfTest
  COMMAND XRay.Metal.ParticleAdapterProbe --self-test
)

add_executable(XRay.Metal.HudFoundationProbe
  tests/R4HudFoundationProbe.cpp
)
target_compile_features(XRay.Metal.HudFoundationProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.HudFoundationProbe PRIVATE XRay.Metal.R4Backend)
add_test(
  NAME XRay.Metal.HudFoundation.SelfTest
  COMMAND XRay.Metal.HudFoundationProbe --self-test
)

add_executable(XRay.Metal.NightVisionFoundationProbe
  tests/R4NightVisionFoundationProbe.cpp
)
target_compile_features(XRay.Metal.NightVisionFoundationProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.NightVisionFoundationProbe PRIVATE XRay.Metal.R4Backend)
add_test(
  NAME XRay.Metal.NightVisionFoundation.SelfTest
  COMMAND XRay.Metal.NightVisionFoundationProbe --self-test
)

add_executable(XRay.Metal.HudAdapterProbe
  tests/MetalR4HudAdapterProbe.cpp
)
target_compile_features(XRay.Metal.HudAdapterProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.HudAdapterProbe PRIVATE XRay.Metal.R4Backend)
add_test(
  NAME XRay.Metal.HudAdapter.SelfTest
  COMMAND XRay.Metal.HudAdapterProbe --self-test
)

add_executable(XRay.Metal.ScopeAdapterProbe
  tests/MetalR4ScopeAdapterProbe.cpp
)
target_compile_features(XRay.Metal.ScopeAdapterProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.ScopeAdapterProbe PRIVATE XRay.Metal.R4Backend)
add_test(
  NAME XRay.Metal.ScopeAdapter.SelfTest
  COMMAND XRay.Metal.ScopeAdapterProbe --self-test
)

add_executable(XRay.Metal.LocalLightAdapterProbe
  tests/MetalR4LocalLightAdapterProbe.cpp
)
target_compile_features(XRay.Metal.LocalLightAdapterProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.LocalLightAdapterProbe PRIVATE XRay.Metal.R4Backend)
add_test(
  NAME XRay.Metal.LocalLightAdapter.SelfTest
  COMMAND XRay.Metal.LocalLightAdapterProbe --self-test
)

add_executable(XRay.Metal.DefVertexAdapterProbe
  tests/MetalR4DefVertexAdapterProbe.cpp
)
target_compile_features(XRay.Metal.DefVertexAdapterProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.DefVertexAdapterProbe PRIVATE XRay.Metal.R4Backend)
add_test(
  NAME XRay.Metal.DefVertexAdapter.SelfTest
  COMMAND XRay.Metal.DefVertexAdapterProbe --self-test
)

add_executable(XRay.Metal.DefObjectsLodFoundationProbe
  tests/R4DefObjectsLodFoundationProbe.cpp
)
target_compile_features(XRay.Metal.DefObjectsLodFoundationProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.DefObjectsLodFoundationProbe PRIVATE XRay.Metal.R4Backend)
add_test(
  NAME XRay.Metal.DefObjectsLodFoundation.SelfTest
  COMMAND XRay.Metal.DefObjectsLodFoundationProbe --self-test
)

add_executable(XRay.Metal.DefObjectsLodAdapterProbe
  tests/MetalR4DefObjectsLodAdapterProbe.cpp
)
target_compile_features(XRay.Metal.DefObjectsLodAdapterProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.DefObjectsLodAdapterProbe PRIVATE XRay.Metal.R4Backend)
add_test(
  NAME XRay.Metal.DefObjectsLodAdapter.SelfTest
  COMMAND XRay.Metal.DefObjectsLodAdapterProbe --self-test
)

add_executable(XRay.Metal.StaticForwardFoundationProbe
  tests/R4StaticForwardFoundationProbe.cpp
)
target_compile_features(XRay.Metal.StaticForwardFoundationProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.StaticForwardFoundationProbe PRIVATE XRay.Metal.R4Backend)
add_test(
  NAME XRay.Metal.StaticForwardFoundation.SelfTest
  COMMAND XRay.Metal.StaticForwardFoundationProbe --self-test
)

add_executable(XRay.Metal.StaticForwardAdapterProbe
  tests/MetalR4StaticForwardAdapterProbe.cpp
)
target_compile_features(XRay.Metal.StaticForwardAdapterProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.StaticForwardAdapterProbe PRIVATE XRay.Metal.R4Backend)
add_test(
  NAME XRay.Metal.StaticForwardAdapter.SelfTest
  COMMAND XRay.Metal.StaticForwardAdapterProbe --self-test
)

find_path(XRAY_METAL_IR_RUNTIME_INCLUDE_DIR
  NAMES metal_irconverter_runtime/metal_irconverter_runtime.h
  HINTS
    "${CMAKE_CURRENT_LIST_DIR}/../../../build/direct-metal-toolchain/metal-shaderconverter-4.0.1/include"
    /usr/local/include
    /opt/homebrew/include
)

if(XRAY_METAL_IR_RUNTIME_INCLUDE_DIR)
  target_include_directories(XRay.Metal.R4Backend PRIVATE
    "${XRAY_METAL_IR_RUNTIME_INCLUDE_DIR}"
  )
  add_executable(XRay.Metal.ShaderGate
    MetalShaderGate.m
  )
  set_target_properties(XRay.Metal.ShaderGate
    PROPERTIES
    OUTPUT_NAME AnomalyMetalShaderGate
  )
  target_compile_options(XRay.Metal.ShaderGate PRIVATE -fobjc-arc)
  target_include_directories(XRay.Metal.ShaderGate PRIVATE
    "${XRAY_METAL_IR_RUNTIME_INCLUDE_DIR}"
  )
  target_link_libraries(XRay.Metal.ShaderGate PRIVATE
    "-framework Foundation"
    "-framework Metal"
  )
endif()
