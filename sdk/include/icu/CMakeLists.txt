add_library(icu INTERFACE)
target_folder(icu ${FOLDER_SDK})
target_include_directories(icu INTERFACE "${CMAKE_SOURCE_DIR}/src/3rd party/icu/include")
