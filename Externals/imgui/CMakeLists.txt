add_library(imgui STATIC)

target_folder(imgui ${FOLDER_EXTERNAL})

target_compile_definitions(imgui
  PRIVATE
  IMGUI_DISABLE_DEFAULT_ALLOCATORS
  IMGUI_DISABLE_DEFAULT_FONT
  IMGUI_INCLUDE_IMGUI_USER_H
  IMGUI_USER_H_FILENAME="imgui_custom.h"
  IMGUI_DEFINE_MATH_OPERATORS
)

target_compile_options(imgui
  PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/permissive->
  $<$<CXX_COMPILER_ID:MSVC>:/wd4005>
  $<$<CXX_COMPILER_ID:Clang>:-Wno-macro-redefined>
  $<$<CXX_COMPILER_ID:AppleClang>:-Wno-macro-redefined>
)

set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/src/3rd party/imgui")

target_link_libraries(imgui
  PUBLIC
  dxsdk
)

target_include_directories(imgui
  PUBLIC
  "${IMGUI_DIR}"
  "${IMGUI_DIR}/backends"
  "${CMAKE_SOURCE_DIR}/src/3rd party"
)

target_sources(imgui PRIVATE
  "${IMGUI_DIR}/imgui_custom.cpp"
  "${IMGUI_DIR}/imgui_custom.h"
  "${IMGUI_DIR}/IconsFontAwesome6.h"
  "${IMGUI_DIR}/imconfig.h"
  "${IMGUI_DIR}/imgui.cpp"
  "${IMGUI_DIR}/imgui.h"
  "${IMGUI_DIR}/imgui_demo.cpp"
  "${IMGUI_DIR}/imgui_draw.cpp"
  "${IMGUI_DIR}/imgui_internal.h"
  "${IMGUI_DIR}/imgui_tables.cpp"
  "${IMGUI_DIR}/imgui_widgets.cpp"
  "${IMGUI_DIR}/imstb_rectpack.h"
  "${IMGUI_DIR}/imstb_textedit.h"
  "${IMGUI_DIR}/imstb_truetype.h"
  "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp"
  "${IMGUI_DIR}/backends/imgui_impl_dx11.h"
)
