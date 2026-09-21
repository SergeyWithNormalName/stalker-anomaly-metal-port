add_library(serial INTERFACE)
target_folder(serial ${FOLDER_SDK})
target_include_directories(serial INTERFACE "${CMAKE_SOURCE_DIR}/src/3rd party/serial/include")
