add_library(DxErr STATIC)
target_folder(DxErr ${FOLDER_EXTERNAL})
set(DXERR_DIR "${CMAKE_SOURCE_DIR}/src/3rd party/DXERR")
target_sources(DxErr PRIVATE "${DXERR_DIR}/dxerr.cpp" "${DXERR_DIR}/dxerr.h")
target_include_directories(DxErr PUBLIC "${DXERR_DIR}" "${CMAKE_SOURCE_DIR}/src")

target_link_libraries(DxErr
  PRIVATE
  XRay.Platform
  XRay.Includes
)

target_compile_options(DxErr
  PUBLIC
  $<$<CXX_COMPILER_ID:MSVC>:/wd4005>
  $<$<CXX_COMPILER_ID:Clang>:-Wno-macro-redefined>
  PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/permissive->
  $<$<CXX_COMPILER_ID:MSVC>:/wd4313>
  $<$<CXX_COMPILER_ID:MSVC>:/wd4474>
  $<$<CXX_COMPILER_ID:MSVC>:/wd4477>
)
