add_library(reshade INTERFACE)
target_folder(reshade ${FOLDER_SDK})
target_include_directories(reshade INTERFACE
  "${CMAKE_SOURCE_DIR}/src/3rd party"
  "${CMAKE_SOURCE_DIR}/src/3rd party/reshade"
)
