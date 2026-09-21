if (APPLE)
  find_library(OGG_LIB NAMES ogg PATHS /opt/homebrew/lib)
  find_path(OGG_INCLUDE_DIR NAMES ogg/ogg.h PATHS /opt/homebrew/include)
  if (OGG_LIB AND OGG_INCLUDE_DIR)
    message(STATUS "Found Homebrew libogg: ${OGG_LIB} (${OGG_INCLUDE_DIR})")
    add_library(libogg INTERFACE)
    target_include_directories(libogg INTERFACE ${OGG_INCLUDE_DIR})
    target_link_libraries(libogg INTERFACE ${OGG_LIB})
    return()
  endif()
endif()

add_library(libogg STATIC)

target_folder(libogg ${FOLDER_EXTERNAL})

set(LIBOGG_DIR ../libogg-git)

target_sources_sandboxed(libogg
  STRIP
  ${LIBOGG_DIR}/src/
  ${LIBOGG_DIR}/include/

  SOURCES
  ${LIBOGG_DIR}/src/bitwise.c
  ${LIBOGG_DIR}/src/framing.c

  ${LIBOGG_DIR}/include/ogg/ogg.h
  ${LIBOGG_DIR}/include/ogg/os_types.h
)

target_include_directories(libogg
  PUBLIC
  ${CMAKE_CURRENT_BINARY_DIR}
)

target_compile_options(libogg
  PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/permissive->
  $<$<CXX_COMPILER_ID:MSVC>:/wd4456>
)
