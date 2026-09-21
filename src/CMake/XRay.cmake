add_module(XRay
  INCLUDES
  ${CMAKE_CURRENT_SOURCE_DIR}
  "${CMAKE_CURRENT_SOURCE_DIR}/3rd party"
  $<$<PLATFORM_ID:Darwin>:/opt/homebrew/include>
  $<$<PLATFORM_ID:Darwin>:/opt/homebrew/include/SDL2>

  SOURCES
  build_config_defines.h
)

include(XRay.Platform)
