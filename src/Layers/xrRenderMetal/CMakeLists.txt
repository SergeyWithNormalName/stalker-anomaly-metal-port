cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(XRayMetalBootstrap LANGUAGES CXX OBJC OBJCXX)
else()
  enable_language(OBJC)
  enable_language(OBJCXX)
endif()

include(CTest)

# Embed the canonical production skin kernel at configure time. A changed MSL
# source must trigger reconfiguration before the backend can rebuild.
set(R4_COMPUTE_SKIN_MSL_PATH "${CMAKE_CURRENT_SOURCE_DIR}/R4ComputeSkin.metal")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${R4_COMPUTE_SKIN_MSL_PATH}")
file(SHA256 "${R4_COMPUTE_SKIN_MSL_PATH}" R4_COMPUTE_SKIN_SHA256)
file(READ "${R4_COMPUTE_SKIN_MSL_PATH}" R4_COMPUTE_SKIN_MSL)
set(R4_COMPUTE_SKIN_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${R4_COMPUTE_SKIN_GENERATED_DIR}")
configure_file(R4ComputeSkinSource.h.in
  "${R4_COMPUTE_SKIN_GENERATED_DIR}/R4ComputeSkinSource.h" @ONLY)

add_executable(XRay.Metal.LightHomBoundsProbe tests/R4LightHomBoundsProbe.cpp)
target_compile_features(XRay.Metal.LightHomBoundsProbe PRIVATE cxx_std_17)
add_test(NAME XRay.Metal.LightHomBounds.SelfTest COMMAND XRay.Metal.LightHomBoundsProbe)

add_executable(XRay.Metal.LocalLightFaceVisibilityProbe tests/R4LocalLightFaceVisibilityProbe.cpp)
target_compile_features(XRay.Metal.LocalLightFaceVisibilityProbe PRIVATE cxx_std_17)
add_test(NAME XRay.Metal.LocalLightFaceVisibility.SelfTest COMMAND XRay.Metal.LocalLightFaceVisibilityProbe)

add_executable(XRay.Metal.DynamicWorldPreparedStateProbe tests/R4DynamicWorldPreparedStateProbe.cpp)
target_compile_features(XRay.Metal.DynamicWorldPreparedStateProbe PRIVATE cxx_std_17)
target_include_directories(XRay.Metal.DynamicWorldPreparedStateProbe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME XRay.Metal.DynamicWorldPreparedState.SelfTest COMMAND XRay.Metal.DynamicWorldPreparedStateProbe)

add_executable(XRay.Metal.ComputeSkinPoseJournalProbe
  tests/R4ComputeSkinPoseJournalProbe.cpp
)

# Strict FP is limited to the coverage/bridge arithmetic and independent probes.
set_source_files_properties(
  R4ActiveOcclusionCoverage.cpp
  R4ActiveOcclusionSceneBridge.cpp
  tests/R4ActiveOcclusionCoverageInputProbe.cpp
  tests/R4ActiveOcclusionCoverageOracleProbe.cpp
  tests/R4ActiveOcclusionSceneBridgeProbe.cpp
  R4ActiveOcclusionCatalogSelection.cpp
  tests/R4ActiveOcclusionCatalogSelectionProbe.cpp
  PROPERTIES COMPILE_OPTIONS "-fno-fast-math;-fno-finite-math-only;-ffp-contract=off"
)

add_executable(XRay.Metal.ActiveOcclusionCatalogSelectionProbe
  tests/R4ActiveOcclusionCatalogSelectionProbe.cpp
  R4ActiveOcclusionCatalogSelection.cpp
)
target_compile_features(XRay.Metal.ActiveOcclusionCatalogSelectionProbe PRIVATE cxx_std_17)
target_include_directories(XRay.Metal.ActiveOcclusionCatalogSelectionProbe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME XRay.Metal.ActiveOcclusion.CatalogSelection
  COMMAND XRay.Metal.ActiveOcclusionCatalogSelectionProbe
)

add_executable(XRay.Metal.ActiveOcclusionCoverageInputProbe
  tests/R4ActiveOcclusionCoverageInputProbe.cpp
  R4ActiveOcclusionCoverage.cpp
)
target_compile_features(XRay.Metal.ActiveOcclusionCoverageInputProbe PRIVATE cxx_std_17)
target_include_directories(XRay.Metal.ActiveOcclusionCoverageInputProbe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME XRay.Metal.ActiveOcclusion.CoverageInput
  COMMAND XRay.Metal.ActiveOcclusionCoverageInputProbe
)

add_executable(XRay.Metal.ActiveOcclusionCoverageOracleProbe
  tests/R4ActiveOcclusionCoverageOracleProbe.cpp
  R4ActiveOcclusionCoverage.cpp
)
target_compile_features(XRay.Metal.ActiveOcclusionCoverageOracleProbe PRIVATE cxx_std_17)
target_include_directories(XRay.Metal.ActiveOcclusionCoverageOracleProbe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME XRay.Metal.ActiveOcclusion.CoverageOracle
  COMMAND XRay.Metal.ActiveOcclusionCoverageOracleProbe
)

add_executable(XRay.Metal.ActiveOcclusionSceneBridgeProbe
  tests/R4ActiveOcclusionSceneBridgeProbe.cpp
  R4ActiveOcclusionSceneBridge.cpp
  R4ActiveOcclusionCoverage.cpp
)
target_compile_features(XRay.Metal.ActiveOcclusionSceneBridgeProbe PRIVATE cxx_std_17)
target_include_directories(XRay.Metal.ActiveOcclusionSceneBridgeProbe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME XRay.Metal.ActiveOcclusionSceneBridge.SelfTest
  COMMAND XRay.Metal.ActiveOcclusionSceneBridgeProbe
)

add_executable(XRay.Metal.WholeLightVisibilityLedgerProbe
  tests/R4WholeLightVisibilityLedgerProbe.cpp
)
target_compile_features(XRay.Metal.WholeLightVisibilityLedgerProbe PRIVATE cxx_std_17)
target_include_directories(XRay.Metal.WholeLightVisibilityLedgerProbe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME XRay.Metal.WholeLightVisibilityLedger.SelfTest
  COMMAND XRay.Metal.WholeLightVisibilityLedgerProbe
)

add_executable(XRay.Metal.WholeLightVisibilitySelectionProbe
  tests/R4WholeLightVisibilitySelectionProbe.cpp
  R4WholeLightVisibilitySelection.cpp
  R4LocalShadowBatchPlan.cpp
)
target_compile_features(XRay.Metal.WholeLightVisibilitySelectionProbe PRIVATE cxx_std_17)
target_include_directories(XRay.Metal.WholeLightVisibilitySelectionProbe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME XRay.Metal.WholeLightVisibilitySelection.SelfTest
  COMMAND XRay.Metal.WholeLightVisibilitySelectionProbe
)
target_compile_features(XRay.Metal.ComputeSkinPoseJournalProbe PRIVATE cxx_std_17)
target_include_directories(XRay.Metal.ComputeSkinPoseJournalProbe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME XRay.Metal.ComputeSkinPoseJournal.SelfTest
  COMMAND XRay.Metal.ComputeSkinPoseJournalProbe
)

add_executable(XRay.Metal.StaticConstantGroupsProbe
  tests/R4StaticConstantGroupsProbe.cpp
)
target_compile_features(XRay.Metal.StaticConstantGroupsProbe PRIVATE cxx_std_17)
target_include_directories(XRay.Metal.StaticConstantGroupsProbe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME XRay.Metal.StaticConstantGroups.SelfTest
  COMMAND XRay.Metal.StaticConstantGroupsProbe
)

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
  MetalR4LightHomDiagnostic.cpp
  MetalR4LightHomDiagnostic.h
  MetalR4ParticleAdapter.cpp
  MetalR4ParticleAdapter.h
  MetalR4RainAdapter.cpp
  MetalR4RainAdapter.h
  MetalR4LensFlareAdapter.h
  MetalR4LensFlareAdapter.cpp
  MetalR4StaticForwardAdapter.cpp
  MetalR4StaticForwardAdapter.h
  MetalR4BloomAdapter.cpp
  MetalR4BloomAdapter.h
  MetalR4VolumetricAdapter.cpp
  MetalR4VolumetricAdapter.h
  MetalR4VolumetricCapture.cpp
  MetalR4VolumetricCapture.h
  MetalR4SsfxAoAdapter.cpp
  MetalR4SsfxAoAdapter.h
  MetalR4FogScatteringAdapter.cpp
  MetalR4FogScatteringAdapter.h
  MetalR4WaterAdapter.cpp
  MetalR4WaterAdapter.h
  MetalR4SsrAdapter.cpp
  MetalR4SsrAdapter.h
  R4LocalLightFoundation.cpp
  R4LocalLightFoundation.h
  R4LocalShadowBatchPlan.cpp
  R4LocalShadowBatchPlan.h
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
  R4ComputeSkinFoundation.cpp
  R4ComputeSkinFoundation.h
  R4StaticConstantGroups.h
  R4ActiveOcclusionCoverage.cpp
  R4ActiveOcclusionCoverage.h
  R4ActiveOcclusionCatalogSelection.cpp
  R4ActiveOcclusionCatalogSelection.h
  R4ActiveOcclusionSceneBridge.cpp
  R4ActiveOcclusionSceneBridge.h
  MetalR4EngineFs.cpp
  MetalR4EngineFs.h
  MetalR4UiVideo.cpp
  MetalR4UiVideo.h
  MetalR4EngineScene.mm
  MetalR4EngineScene.h
  MetalR4Backend.mm
  MetalR4MotionValidation.mm
  MetalR4LocalShadowBatchValidation.mm
  R4LocalShadowBatchValidation.h
  MetalR4SunShadowValidation.mm
  R4SunCascade.h
  R4SunShadowValidation.h
  MetalScreenshotCodec.cpp
  MetalScreenshotCodec.h
  R4DdsTexture.cpp
  R4DdsTexture.h
  R4MaterialSet.cpp
  R4MaterialSet.h
  R4LevelGeometry.cpp
  R4LevelGeometry.h
  DisplayCapabilities.h
  DisplayCapabilities.mm
  IR4Backend.h
  IR4Pipeline.h
  IR4Resources.h
  R4ComputeSkinMetal.h
  R4WholeLightVisibilityLedger.h
  R4WholeLightVisibilitySelection.cpp
  R4WholeLightVisibilitySelection.h
  "${R4_COMPUTE_SKIN_GENERATED_DIR}/R4ComputeSkinSource.h"
)
target_compile_options(XRay.Metal.R4Backend PRIVATE -fobjc-arc)
target_compile_features(XRay.Metal.R4Backend PUBLIC cxx_std_17)
target_include_directories(XRay.Metal.R4Backend PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  "${R4_COMPUTE_SKIN_GENERATED_DIR}"
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

add_executable(XRay.Metal.FrameUploadGpuProbe
  tests/R4FrameUploadGpuProbe.cpp
  tests/R4LocalLightRolloverGpuSupport.cpp
)
target_compile_features(XRay.Metal.FrameUploadGpuProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.FrameUploadGpuProbe PRIVATE XRay.Metal.R4Backend XRay.Core)
add_test(
  NAME XRay.Metal.FrameUploadGpu.SelfTest
  COMMAND XRay.Metal.FrameUploadGpuProbe --self-test
)
add_test(
  NAME XRay.Metal.PrivateUploadRecovery
  COMMAND XRay.Metal.FrameUploadGpuProbe --private-upload-recovery
)


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
target_link_libraries(XRay.Metal.DynamicWorldAdapterProbe PRIVATE XRay.Metal.R4Backend XRay.Core)
add_test(
  NAME XRay.Metal.DynamicWorldAdapter.SelfTest
  COMMAND XRay.Metal.DynamicWorldAdapterProbe --self-test
)

# Isolated native MSL prototype: production skinning is unchanged. The GPU
# oracle compares its output against the existing CPU repack implementation.
add_executable(XRay.Metal.ComputeSkinGpuProbe
  tests/R4ComputeSkinGpuProbe.mm
)
target_compile_features(XRay.Metal.ComputeSkinGpuProbe PRIVATE cxx_std_17)
target_compile_options(XRay.Metal.ComputeSkinGpuProbe PRIVATE -fobjc-arc)
target_compile_definitions(XRay.Metal.ComputeSkinGpuProbe PRIVATE
  R4_COMPUTE_SKIN_SOURCE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/R4ComputeSkin.metal"
)
target_link_libraries(XRay.Metal.ComputeSkinGpuProbe PRIVATE
  XRay.Metal.R4Backend XRay.Core "-framework Foundation" "-framework Metal"
)
add_test(NAME XRay.Metal.ComputeSkin.GpuOracle
  COMMAND XRay.Metal.ComputeSkinGpuProbe --self-test
)

add_executable(XRay.Metal.ComputeSkinBoundsProbe
  tests/R4ComputeSkinBoundsProbe.cpp
  tests/R4LocalLightRolloverGpuSupport.cpp
)
target_compile_features(XRay.Metal.ComputeSkinBoundsProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.ComputeSkinBoundsProbe PRIVATE
  XRay.Metal.R4Backend XRay.Core
)
add_test(NAME XRay.Metal.ComputeSkin.Bounds
  COMMAND XRay.Metal.ComputeSkinBoundsProbe
)

add_executable(XRay.Metal.ComputeSkinBackendGpuProbe
  tests/R4ComputeSkinBackendGpuProbe.cpp
  tests/R4LocalLightRolloverGpuSupport.cpp
)
target_compile_features(XRay.Metal.ComputeSkinBackendGpuProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.ComputeSkinBackendGpuProbe PRIVATE
  XRay.Metal.R4Backend XRay.Core
)
add_test(NAME XRay.Metal.ComputeSkin.BackendGpu
  COMMAND XRay.Metal.ComputeSkinBackendGpuProbe --self-test
)

add_executable(XRay.Metal.WholeLightVisibilityBackendGpuProbe
  tests/R4WholeLightVisibilityBackendGpuProbe.cpp
  tests/R4LocalLightRolloverGpuSupport.cpp
)
target_compile_features(XRay.Metal.WholeLightVisibilityBackendGpuProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.WholeLightVisibilityBackendGpuProbe PRIVATE
  XRay.Metal.R4Backend XRay.Core
)
add_test(NAME XRay.Metal.WholeLightVisibility.BackendGpu
  COMMAND XRay.Metal.WholeLightVisibilityBackendGpuProbe --self-test
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
target_link_libraries(XRay.Metal.LocalLightFoundationProbe PRIVATE XRay.Metal.R4Backend XRay.Core)
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

add_executable(XRay.Metal.DisplayCapabilitiesProbe
  tests/DisplayCapabilitiesProbe.mm
  DisplayCapabilities.mm
  DisplayCapabilities.h
)
target_compile_features(XRay.Metal.DisplayCapabilitiesProbe PRIVATE cxx_std_17)
target_compile_options(XRay.Metal.DisplayCapabilitiesProbe PRIVATE -fobjc-arc)
target_include_directories(XRay.Metal.DisplayCapabilitiesProbe PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  /opt/homebrew/include/SDL2
)
target_link_libraries(XRay.Metal.DisplayCapabilitiesProbe PRIVATE
  /opt/homebrew/lib/libSDL2.dylib
  "-framework Cocoa"
  "-framework Metal"
  "-framework QuartzCore"
)
target_link_options(XRay.Metal.DisplayCapabilitiesProbe PRIVATE "-Wl,-rpath,/opt/homebrew/lib")
add_test(
  NAME XRay.Metal.DisplayCapabilities.SelfTest
  COMMAND XRay.Metal.DisplayCapabilitiesProbe
)

add_executable(XRay.Metal.MetalFXSpatialScalerProbe
  tests/MetalFXSpatialScalerProbe.mm
)
target_compile_features(XRay.Metal.MetalFXSpatialScalerProbe PRIVATE cxx_std_17)
target_compile_options(XRay.Metal.MetalFXSpatialScalerProbe PRIVATE -fobjc-arc)
target_include_directories(XRay.Metal.MetalFXSpatialScalerProbe PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(XRay.Metal.MetalFXSpatialScalerProbe PRIVATE
  "-framework Foundation"
  "-framework Metal"
  "-framework MetalFX"
  "-framework CoreGraphics"
  "-framework ImageIO"
)
add_test(
  NAME XRay.Metal.MetalFXSpatialScaler.SelfTest
  COMMAND XRay.Metal.MetalFXSpatialScalerProbe
)

add_executable(XRay.Metal.CAMetalDisplayLinkProbe
  tests/CAMetalDisplayLinkProbe.mm
)
target_compile_features(XRay.Metal.CAMetalDisplayLinkProbe PRIVATE cxx_std_17)
target_compile_options(XRay.Metal.CAMetalDisplayLinkProbe PRIVATE -fobjc-arc)
target_include_directories(XRay.Metal.CAMetalDisplayLinkProbe PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(XRay.Metal.CAMetalDisplayLinkProbe PRIVATE
  "-framework Cocoa"
  "-framework Metal"
  "-framework QuartzCore"
)
add_test(
  NAME XRay.Metal.CAMetalDisplayLink.SelfTest
  COMMAND XRay.Metal.CAMetalDisplayLinkProbe --self-test
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
target_link_libraries(XRay.Metal.ParticleAdapterProbe PRIVATE XRay.Metal.R4Backend XRay.Core)
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
target_link_libraries(XRay.Metal.NightVisionFoundationProbe PRIVATE XRay.Metal.R4Backend XRay.Core)
add_test(
  NAME XRay.Metal.NightVisionFoundation.SelfTest
  COMMAND XRay.Metal.NightVisionFoundationProbe --self-test
)

add_executable(XRay.Metal.HudAdapterProbe
  tests/MetalR4HudAdapterProbe.cpp
)
target_compile_features(XRay.Metal.HudAdapterProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.HudAdapterProbe PRIVATE XRay.Metal.R4Backend XRay.Core)
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
  ../xrRender/du_sphere_part.cpp
)
target_compile_features(XRay.Metal.LocalLightAdapterProbe PRIVATE cxx_std_17)
target_compile_definitions(XRay.Metal.LocalLightAdapterProbe PRIVATE ECORE_API=)
target_include_directories(XRay.Metal.LocalLightAdapterProbe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../xrCore)
if(TARGET XRay.Core.Defines)
  target_link_libraries(XRay.Metal.LocalLightAdapterProbe PRIVATE XRay.Core.Defines)
endif()
target_link_libraries(XRay.Metal.LocalLightAdapterProbe PRIVATE XRay.Metal.R4Backend)
add_test(
  NAME XRay.Metal.LocalLightAdapter.SelfTest
  COMMAND XRay.Metal.LocalLightAdapterProbe --self-test
)

add_executable(XRay.Metal.LocalLightRolloverGpuProbe
  tests/R4LocalLightRolloverGpuProbe.cpp
  tests/R4LocalLightRolloverGpuSupport.cpp
  ../xrRender/du_sphere_part.cpp
)
target_compile_features(XRay.Metal.LocalLightRolloverGpuProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.LocalLightRolloverGpuProbe PRIVATE XRay.Metal.R4Backend XRay.Core)
# Canonical du_sphere_part data needs no renderer DLL export in this probe.
target_compile_definitions(XRay.Metal.LocalLightRolloverGpuProbe PRIVATE ECORE_API=)

add_executable(XRay.Metal.DefVertexAdapterProbe
  tests/MetalR4DefVertexAdapterProbe.cpp
)
target_compile_features(XRay.Metal.DefVertexAdapterProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.DefVertexAdapterProbe PRIVATE XRay.Metal.R4Backend XRay.Core)
add_test(
  NAME XRay.Metal.DefVertexAdapter.SelfTest
  COMMAND XRay.Metal.DefVertexAdapterProbe --self-test
)

add_executable(XRay.Metal.DefObjectsLodFoundationProbe
  tests/R4DefObjectsLodFoundationProbe.cpp
)
target_compile_features(XRay.Metal.DefObjectsLodFoundationProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.DefObjectsLodFoundationProbe PRIVATE XRay.Metal.R4Backend XRay.Core)
add_test(
  NAME XRay.Metal.DefObjectsLodFoundation.SelfTest
  COMMAND XRay.Metal.DefObjectsLodFoundationProbe --self-test
)

add_executable(XRay.Metal.DefObjectsLodAdapterProbe
  tests/MetalR4DefObjectsLodAdapterProbe.cpp
)
target_compile_features(XRay.Metal.DefObjectsLodAdapterProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.DefObjectsLodAdapterProbe PRIVATE XRay.Metal.R4Backend XRay.Core)
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

add_executable(XRay.Metal.SubmittedViewHistoryProbe tests/R4SubmittedViewHistoryProbe.cpp)
target_compile_features(XRay.Metal.SubmittedViewHistoryProbe PRIVATE cxx_std_17)
add_test(NAME XRay.Metal.SubmittedViewHistory.SelfTest COMMAND XRay.Metal.SubmittedViewHistoryProbe)

add_executable(XRay.Metal.SunCascadeCaptureProbe tests/R4SunCascadeCaptureProbe.cpp)
target_compile_features(XRay.Metal.SunCascadeCaptureProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.SunCascadeCaptureProbe PRIVATE XRay.Render.R4 XRay.Core XRay.Collision)
add_test(NAME XRay.Metal.SunCascadeCapture.SelfTest COMMAND XRay.Metal.SunCascadeCaptureProbe --self-test)

add_executable(XRay.Metal.LocalShadowBatchPlanProbe tests/R4LocalShadowBatchPlanProbe.cpp)
target_compile_features(XRay.Metal.LocalShadowBatchPlanProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.LocalShadowBatchPlanProbe PRIVATE XRay.Metal.R4Backend)
add_test(NAME XRay.Metal.LocalShadowBatchPlan.SelfTest COMMAND XRay.Metal.LocalShadowBatchPlanProbe --self-test)

add_executable(XRay.Metal.LocalShadowBatchGpuProbe
  tests/R4LocalShadowBatchGpuProbe.cpp
  tests/R4LocalLightRolloverGpuSupport.cpp
  ../xrRender/du_sphere_part.cpp
)
target_compile_features(XRay.Metal.LocalShadowBatchGpuProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.LocalShadowBatchGpuProbe PRIVATE XRay.Metal.R4Backend XRay.Core)
target_compile_definitions(XRay.Metal.LocalShadowBatchGpuProbe PRIVATE ECORE_API=)

add_executable(XRay.Metal.FloraInstancingGpuProbe
  tests/R4FloraInstancingGpuProbe.cpp
  tests/R4LocalLightRolloverGpuSupport.cpp
)
target_compile_features(XRay.Metal.FloraInstancingGpuProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.FloraInstancingGpuProbe PRIVATE XRay.Metal.R4Backend XRay.Core)
target_compile_definitions(XRay.Metal.FloraInstancingGpuProbe PRIVATE ECORE_API=)

add_executable(XRay.Metal.AoIlControlsGpuProbe tests/R4AoIlControlsGpuProbe.cpp tests/R4LocalLightRolloverGpuSupport.cpp)
target_compile_features(XRay.Metal.AoIlControlsGpuProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.AoIlControlsGpuProbe PRIVATE XRay.Metal.R4Backend XRay.Core)
target_compile_definitions(XRay.Metal.AoIlControlsGpuProbe PRIVATE ECORE_API=)

add_executable(XRay.Metal.VolumetricCaptureProbe tests/R4VolumetricCaptureProbe.cpp)
target_compile_features(XRay.Metal.VolumetricCaptureProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.VolumetricCaptureProbe PRIVATE XRay.Metal.R4Backend XRay.Render.R4 XRay.Core XRay.Collision)
add_test(NAME XRay.Metal.VolumetricCapture.SelfTest COMMAND XRay.Metal.VolumetricCaptureProbe --self-test)

add_executable(XRay.Metal.EmissiveAccumulatorGpuProbe
  tests/R4EmissiveAccumulatorGpuProbe.cpp
  tests/R4LocalLightRolloverGpuSupport.cpp
)
target_compile_features(XRay.Metal.EmissiveAccumulatorGpuProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.EmissiveAccumulatorGpuProbe PRIVATE XRay.Metal.R4Backend XRay.Core)
target_compile_definitions(XRay.Metal.EmissiveAccumulatorGpuProbe PRIVATE ECORE_API=)

# E08 order-preserving index batching, independent CPU contract and real MSC GPU oracle.
add_executable(XRay.Metal.StaticShadowMergeProbe tests/R4StaticShadowMergeProbe.cpp)
target_compile_features(XRay.Metal.StaticShadowMergeProbe PRIVATE cxx_std_17)
target_include_directories(XRay.Metal.StaticShadowMergeProbe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME XRay.Metal.StaticShadowMerge.SelfTest COMMAND XRay.Metal.StaticShadowMergeProbe --self-test)
add_executable(XRay.Metal.StaticShadowMergeGpuProbe
  tests/R4StaticShadowMergeGpuProbe.cpp tests/R4LocalLightRolloverGpuSupport.cpp)
target_compile_features(XRay.Metal.StaticShadowMergeGpuProbe PRIVATE cxx_std_17)
target_link_libraries(XRay.Metal.StaticShadowMergeGpuProbe PRIVATE XRay.Metal.R4Backend XRay.Core)
target_compile_definitions(XRay.Metal.StaticShadowMergeGpuProbe PRIVATE ECORE_API=)
