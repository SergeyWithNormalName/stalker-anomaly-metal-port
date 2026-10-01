set(EDITOR_BUILD Off)

add_module(XRay.Sound
  TYPE STATIC
  
  INCLUDES
  ${CMAKE_CURRENT_SOURCE_DIR}

  DEFINES
  # mmsystem.h
  MMNOSOUND
  MMNOMIDI
  MMNOAUX
  MMNOMIXER
  MMNOJOY

  # mmreg.h
  NOMMIDS
  NONEWRIFFF
  NOJPEGDIB
  NONEWIC
  NOBITMAP

  XRSOUND_EXPORTS

  LINKS
  dxsdk
  FastDynamicCast
  libogg
  libtheora
  libvorbis
  libvorbisfile
  OpenAL
  tbb

  XRay.Core.Defines

  XRay.Core.Includes
  XRay.Collision.Includes
  XRay.Engine.Includes
  XRay.Physics.Includes
  XRay.Render.API.Includes
  XRay.Render.Common.Includes
  XRay.ServerEntities.Includes

  PRECOMPILES
  #[["xrCore.h"]]
  #[["xrCDB.h"]]

  #[["mmsystem.h"]]
  #[["mmreg.h"]]

  #[["vorbis/codec.h"]]
  #[["vorbis/vorbisfile.h"]]

  #sound.h
  #resource.h

  SOURCES
  guids.cpp
  OpenALDeviceList.cpp
  sound.cpp

  cl_intersect.h
  OpenALDeviceList.h
  Sound.h
  SoundRender.h

  NotificationClient.cpp
  NotificationClient.h
)

if(EDITOR_BUILD)
  target_precompile_headers(XRay.Sound
    PRIVATE
    ETools.h
  )
endif()

target_compile_options(XRay.Sound
  PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/wd4005>
  $<$<CXX_COMPILER_ID:MSVC>:/wd4995>
)

if(WIN32)
  target_link_libraries(XRay.Sound
    PRIVATE
    dxguid
  )
endif()

if(APPLE)
  enable_language(OBJCXX)

  set_source_files_properties(
    AppleSpatialBridge.mm
    PROPERTIES
    SKIP_UNITY_BUILD_INCLUSION true
  )

  target_sources(XRay.Sound PRIVATE
    AppleSpatialBridge.h
    AppleSpatialBridge.mm
    SoundRender_CoreApple.h
    SoundRender_CoreApple.cpp
    SoundRender_TargetApple.h
    SoundRender_TargetApple.cpp
    AppleSpatialAudioTypes.h
  )

  target_compile_options(XRay.Sound PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    $<$<COMPILE_LANGUAGE:OBJCXX>:-Wno-deprecated-declarations>
  )
  target_link_libraries(XRay.Sound
    PRIVATE
    "-framework Foundation"
    "-framework AVFAudio"
    "-framework AudioToolbox"
    "-framework CoreAudio"
  )

  add_executable(XRay.Sound.AppleSpatialProbe
    tests/AppleSpatialAudioProbe.cpp
    AppleSpatialBridge.mm
    AppleSpatialBridge.h
  )
  target_compile_features(XRay.Sound.AppleSpatialProbe PRIVATE cxx_std_17)
  target_include_directories(XRay.Sound.AppleSpatialProbe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
  )
  target_compile_options(XRay.Sound.AppleSpatialProbe PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    $<$<COMPILE_LANGUAGE:OBJCXX>:-Wno-deprecated-declarations>
  )
  target_link_libraries(XRay.Sound.AppleSpatialProbe PRIVATE
    "-framework Foundation"
    "-framework AVFAudio"
    "-framework AudioToolbox"
    "-framework CoreAudio"
  )
endif()

include(XRay.Sound.Cache)
include(XRay.Sound.Core)
include(XRay.Sound.Emitter)
include(XRay.Sound.Environment)
include(XRay.Sound.Source)
include(XRay.Sound.Target)
