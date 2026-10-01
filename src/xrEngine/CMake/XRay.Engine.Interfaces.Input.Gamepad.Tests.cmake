# Standalone tests for the gamepad state processing and mapping policy. They
# deliberately do not link the engine: both units under test are free of engine
# and platform dependencies, so the tests need no controller and no game.

include(CTest)

add_executable(XRay.Gamepad.LogicTests
  tests/GamepadLogicTests.cpp
  xr_gamepad_processor.cpp
  xr_gamepad_mapping.cpp
  xr_gamepad_nav.cpp
  xr_gamepad_touchpad.cpp
  xr_gamepad_radial.cpp
  xr_gamepad_gyro.cpp
  xr_gamepad_chord.cpp
  xr_gamepad_haptics.cpp
  xr_gamepad_processor.h
  xr_gamepad_mapping.h
  xr_gamepad_types.h
  xr_gamepad_nav.h
  xr_gamepad_touchpad.h
  xr_gamepad_radial.h
  xr_gamepad_gyro.h
  xr_gamepad_chord.h
  xr_gamepad_haptics.h
)

target_compile_features(XRay.Gamepad.LogicTests PRIVATE cxx_std_17)

target_include_directories(XRay.Gamepad.LogicTests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

add_test(
  NAME XRay.Gamepad.Logic.SelfTest
  COMMAND XRay.Gamepad.LogicTests
)

if(APPLE)
  # Smoke probe for the real Apple bridge. It is an opt-in executable rather
  # than a registered test: it links the frameworks and needs a window server,
  # so it does not belong in a headless test run. Run it by hand to confirm the
  # bridge starts, and to see what a connected pad actually reports.
  enable_language(OBJCXX)

  add_executable(XRay.Gamepad.AppleBridgeProbe
    tests/AppleGamepadBridgeProbe.cpp
  )

  target_compile_features(XRay.Gamepad.AppleBridgeProbe PRIVATE cxx_std_17)

  target_include_directories(XRay.Gamepad.AppleBridgeProbe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
  )

  target_sources(XRay.Gamepad.AppleBridgeProbe PRIVATE
    xr_gamepad_apple.mm
    xr_gamepad_apple.h
  )

  target_compile_options(XRay.Gamepad.AppleBridgeProbe PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
  )

  target_link_libraries(XRay.Gamepad.AppleBridgeProbe PRIVATE
    "-framework GameController"
    "-framework CoreHaptics"
    "-framework Foundation"
  )
endif()
