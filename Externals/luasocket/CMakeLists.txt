add_library(luasocket STATIC)

target_folder(luasocket ${FOLDER_LUA_EXTENSIONS})

set(LUASOCKET_DIR "${CMAKE_SOURCE_DIR}/src/3rd party/lua-extensions/luasocket")

set(LUASOCKET_SOURCES
  "${LUASOCKET_DIR}/auxiliar.c"
  "${LUASOCKET_DIR}/buffer.c"
  "${LUASOCKET_DIR}/compat.c"
  "${LUASOCKET_DIR}/except.c"
  "${LUASOCKET_DIR}/inet.c"
  "${LUASOCKET_DIR}/io.c"
  "${LUASOCKET_DIR}/luasocket.c"
  "${LUASOCKET_DIR}/mime.c"
  "${LUASOCKET_DIR}/options.c"
  "${LUASOCKET_DIR}/select.c"
  "${LUASOCKET_DIR}/tcp.c"
  "${LUASOCKET_DIR}/timeout.c"
  "${LUASOCKET_DIR}/udp.c"
  "${LUASOCKET_DIR}/auxiliar.h"
  "${LUASOCKET_DIR}/buffer.h"
  "${LUASOCKET_DIR}/compat.h"
  "${LUASOCKET_DIR}/except.h"
  "${LUASOCKET_DIR}/inet.h"
  "${LUASOCKET_DIR}/io.h"
  "${LUASOCKET_DIR}/luasocket.h"
  "${LUASOCKET_DIR}/mime.h"
  "${LUASOCKET_DIR}/options.h"
  "${LUASOCKET_DIR}/pierror.h"
  "${LUASOCKET_DIR}/select.h"
  "${LUASOCKET_DIR}/socket.h"
  "${LUASOCKET_DIR}/tcp.h"
  "${LUASOCKET_DIR}/timeout.h"
  "${LUASOCKET_DIR}/udp.h"
)

if (WIN32)
  list(APPEND LUASOCKET_SOURCES
    "${LUASOCKET_DIR}/wsocket.c"
    "${LUASOCKET_DIR}/wsocket.h"
  )
else()
  list(APPEND LUASOCKET_SOURCES
    "${LUASOCKET_DIR}/usocket.c"
    "${LUASOCKET_DIR}/usocket.h"
  )
endif()

target_sources(luasocket PRIVATE ${LUASOCKET_SOURCES})

target_include_directories(luasocket
  PUBLIC
  "${LUASOCKET_DIR}"
)

target_compile_options(luasocket
  PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/permissive->
  $<$<CXX_COMPILER_ID:MSVC>:/wd4100>
)

target_compile_definitions(luasocket
  PRIVATE
  _WINSOCK_DEPRECATED_NO_WARNINGS
  _CRT_SECURE_NO_WARNINGS
)

target_link_libraries(luasocket
  PRIVATE
  LuaJIT
)
