add_library(FastDynamicCast INTERFACE)

target_folder(FastDynamicCast ${FOLDER_EXTERNAL})

target_include_directories(FastDynamicCast
  INTERFACE
  "${CMAKE_SOURCE_DIR}/src/3rd party/fast_dynamic_cast"
)