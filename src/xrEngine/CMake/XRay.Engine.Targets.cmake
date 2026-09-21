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
