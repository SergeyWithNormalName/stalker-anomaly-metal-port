# Gamepad support. The state processing is platform independent and is what the
# unit tests exercise; the Apple backend is the only Objective-C++ in the tree
# outside the Metal layer.

option(XRAY_GAMEPAD_TESTS "Build the standalone gamepad logic tests" OFF)

add_module(XRay.Engine.Interfaces.Input.Gamepad
  TYPE STATIC

  SOURCES
  xr_gamepad_types.h
  xr_gamepad_processor.cpp
  xr_gamepad_processor.h
  xr_gamepad_mapping.cpp
  xr_gamepad_mapping.h
  xr_gamepad_nav.cpp
  xr_gamepad_nav.h
  xr_gamepad_touchpad.cpp
  xr_gamepad_touchpad.h
  xr_gamepad_radial.cpp
  xr_gamepad_radial.h
  xr_gamepad_gyro.cpp
  xr_gamepad_gyro.h
  xr_gamepad_chord.cpp
  xr_gamepad_chord.h
  xr_gamepad_haptics.cpp
  xr_gamepad_haptics.h
  xr_gamepad.cpp
  xr_gamepad.h
)

if(APPLE)
  # The Apple backend is Objective-C++. Enable the language here rather than
  # relying on the Metal layer having been configured first.
  enable_language(OBJCXX)

  # Objective-C++ must not land in a unity batch with the C++ sources.
  set_source_files_properties(
    xr_gamepad_apple.mm
    PROPERTIES
    SKIP_UNITY_BUILD_INCLUSION true
  )

  target_sources(XRay.Engine.Interfaces.Input.Gamepad PRIVATE
    xr_gamepad_apple.mm
    xr_gamepad_apple.cpp
    xr_gamepad_apple.h
  )

  # The bridge is the only Objective-C++ here and it deliberately includes no
  # engine header, so it needs ARC and nothing else from the project's flags.
  target_compile_options(XRay.Engine.Interfaces.Input.Gamepad PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
  )

  target_link_libraries(XRay.Engine.Interfaces.Input.Gamepad PRIVATE
    "-framework GameController"
    "-framework CoreHaptics"
    "-framework Foundation"
  )
endif()

if(XRAY_GAMEPAD_TESTS)
  include(XRay.Engine.Interfaces.Input.Gamepad.Tests)
endif()
