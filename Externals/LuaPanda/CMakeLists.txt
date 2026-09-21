add_library(LuaPanda STATIC)

target_folder(LuaPanda ${FOLDER_LUA_EXTENSIONS})

set(LUAPANDA_DIR "${CMAKE_SOURCE_DIR}/src/3rd party/lua-extensions")

target_sources(LuaPanda
  PRIVATE
  "${LUAPANDA_DIR}/luapanda.cpp"
  "${LUAPANDA_DIR}/luapanda.h"
)

target_include_directories(LuaPanda
  PUBLIC
  "${LUAPANDA_DIR}"
)

target_compile_options(LuaPanda
  PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/permissive->
  $<$<CXX_COMPILER_ID:MSVC>:/wd4100>
  $<$<CXX_COMPILER_ID:MSVC>:/wd4244>
  $<$<CXX_COMPILER_ID:MSVC>:/wd4267>
)

target_compile_definitions(LuaPanda
  PRIVATE
  USE_SOURCE_CODE
)

target_link_libraries(LuaPanda
  PRIVATE
  LuaJIT
)