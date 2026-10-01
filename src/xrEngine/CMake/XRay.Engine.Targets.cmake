include_guard()

# Add a target with the given renderer, and an AVX variant
function(add_engine_target NAME)
  add_executable(${NAME})
  if(WIN32)
    set_target_properties(${NAME} PROPERTIES WIN32_EXECUTABLE TRUE)
  endif()

  if(${XRAY_AVX})
    set(${NAME}_NAME_EXECUTABLE ${NAME}.AVX)
  else()
    set(${NAME}_NAME_EXECUTABLE ${NAME})
  endif()

  string(REPLACE "." "" ${NAME}_NAME_OUTPUT ${${NAME}_NAME_EXECUTABLE})
  set_target_properties(${NAME}
    PROPERTIES OUTPUT_NAME
    ${${NAME}_NAME_OUTPUT}
  )
  
  # Apply artifact output directories
  set_target_properties(${NAME}
    PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${COMPILE_OUTPUT_DIR}
    LIBRARY_OUTPUT_DIRECTORY ${COMPILE_OUTPUT_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${COMPILE_OUTPUT_DIR}
    PDB_OUTPUT_DIRECTORY ${COMPILE_OUTPUT_DIR}
    COMPILE_PDB_OUTPUT_DIRECTORY ${COMPILE_OUTPUT_DIR}
  )

  target_folder(${NAME} ${FOLDER_EXECUTABLES})

  target_link_libraries(${NAME}
    PRIVATE
    XRay.Engine.Main
    ${ARGN}
  )

  if(APPLE)
    # ld64 does not rescan a static archive after later game/engine archives
    # introduce references to renderer globals.  Load the selected renderer
    # archive eagerly so R4's D3D state singletons remain available.
    foreach(RENDER_TARGET IN LISTS ARGN)
      target_link_options(${NAME} PRIVATE
        "-Wl,-force_load,$<TARGET_FILE:${RENDER_TARGET}>"
      )
    endforeach()

    target_link_libraries(${NAME}
      PRIVATE
      "/opt/homebrew/lib/libSDL2.dylib"
      "-framework AppKit"
      "-framework Foundation"
      "-framework CoreServices"
      "-framework IOKit"
      "-framework CoreAudio"
      "-framework CoreVideo"
      "-framework QuartzCore"
      "-framework Metal"
      "-framework AudioToolbox"
    )

    # DXVK Native loads the SDL WSI backend with dlopen("libSDL2-2.0.0.dylib").
    # Keep Homebrew's dylib directory in the executable's search paths so the
    # backend resolves without requiring DYLD_LIBRARY_PATH at launch.
    target_link_options(${NAME} PRIVATE "-Wl,-rpath,/opt/homebrew/lib")

    # Unified deployment target resolution for Info.plist LSMinimumSystemVersion.
    # If CMAKE_OSX_DEPLOYMENT_TARGET is not explicitly set, AppleClang targets
    # the active macOS SDK by default. Query it so Info.plist matches the
    # Mach-O executable's LC_BUILD_VERSION minos exactly.
    if(NOT "${CMAKE_OSX_DEPLOYMENT_TARGET}" STREQUAL "")
      set(MACOS_BUNDLE_MINIMUM_SYSTEM_VERSION "${CMAKE_OSX_DEPLOYMENT_TARGET}")
    else()
      execute_process(
        COMMAND xcrun --sdk macosx --show-sdk-version
        OUTPUT_VARIABLE _MACOS_DETECTED_SDK_VER
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
      )
      if(_MACOS_DETECTED_SDK_VER)
        set(MACOS_BUNDLE_MINIMUM_SYSTEM_VERSION "${_MACOS_DETECTED_SDK_VER}")
      else()
        set(MACOS_BUNDLE_MINIMUM_SYSTEM_VERSION "${CMAKE_SYSTEM_VERSION}")
      endif()
    endif()

    set(MACOS_BUNDLE_IDENTIFIER "com.stalker-anomaly.game")
    set(MACOS_BUNDLE_NAME "Anomaly")
    set(MACOS_BUNDLE_DISPLAY_NAME "S.T.A.L.K.E.R. Anomaly")
    set(MACOS_BUNDLE_EXECUTABLE "${${NAME}_NAME_OUTPUT}")
    set(MACOS_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
    set(MACOS_BUNDLE_VERSION "${TODAY_DOTS}")
    set(MACOS_BUNDLE_ICON_FILE "AppIcon")

    set(PLIST_IN "${CMAKE_SOURCE_DIR}/packaging/macos/Info.plist.in")
    set(PLIST_OUT "${CMAKE_CURRENT_BINARY_DIR}/${NAME}.Info.plist")
    configure_file("${PLIST_IN}" "${PLIST_OUT}" @ONLY)

    set(SYNC_DEV_BUNDLE_SCRIPT "${CMAKE_SOURCE_DIR}/packaging/macos/sync_dev_bundle.sh")

    add_custom_command(TARGET ${NAME} POST_BUILD
      COMMAND /bin/bash "${SYNC_DEV_BUNDLE_SCRIPT}"
        --target-file "$<TARGET_FILE:${NAME}>"
        --bundle-dir "$<TARGET_FILE_DIR:${NAME}>/Anomaly.app"
        --plist-file "${PLIST_OUT}"
        --output-name "${${NAME}_NAME_OUTPUT}"
      COMMENT "Updating DEV macOS bundle: Anomaly.app"
      VERBATIM
    )
  endif()
endfunction()

# Setup executable targets
if(WIN32)
  add_engine_target(
    Anomaly.DX8
    XRay.Render.R1
  )
  add_engine_target(
    Anomaly.DX9
    XRay.Render.R2
  )
  add_engine_target(
    Anomaly.DX10
    XRay.Render.R3
  )
endif()
add_engine_target(
  Anomaly.DX11
  XRay.Render.R4
)

# Set visual studio startup project
set_property(
  DIRECTORY ${CMAKE_SOURCE_DIR}
  PROPERTY VS_STARTUP_PROJECT
  Anomaly.DX11
)
