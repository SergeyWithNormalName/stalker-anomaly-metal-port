add_library(tbb INTERFACE)
target_folder(tbb ${FOLDER_SDK})

if(APPLE)
  target_include_directories(tbb INTERFACE /opt/homebrew/include)
  target_link_libraries(tbb INTERFACE /opt/homebrew/lib/libtbb.dylib)
else()
  target_include_directories(tbb INTERFACE "${CMAKE_SOURCE_DIR}/src/3rd party/tbb/include")
endif()
