add_library(luafilesystem STATIC)

target_folder(luafilesystem ${FOLDER_LUA_EXTENSIONS})

set(LUAFILESYSTEM_DIR "${CMAKE_SOURCE_DIR}/src/3rd party/lua-extensions")

target_sources(luafilesystem
  PRIVATE
  "${LUAFILESYSTEM_DIR}/lfs.c"
  "${LUAFILESYSTEM_DIR}/lfs.h"
)

target_include_directories(luafilesystem
  PUBLIC
  "${LUAFILESYSTEM_DIR}"
)

target_compile_options(luafilesystem
  PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/permissive->
)

target_compile_definitions(luafilesystem
  PRIVATE
  _CRT_SECURE_NO_WARNINGS
)

target_link_libraries(luafilesystem
  PRIVATE
  LuaJIT
)