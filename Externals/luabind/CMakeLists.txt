add_library(luabind STATIC)

target_folder(luabind ${FOLDER_LUA})

target_include_directories(luabind
  PUBLIC
  "${CMAKE_SOURCE_DIR}/src/3rd party"
  "${CMAKE_SOURCE_DIR}/src/3rd party/luabind"
  PRIVATE
  "${CMAKE_SOURCE_DIR}/src/3rd party/luabind/luabind/src"
  "${CMAKE_SOURCE_DIR}/src"
  "${CMAKE_SOURCE_DIR}/src/xrCore"
  "${CMAKE_SOURCE_DIR}/src/Common"
)

target_link_libraries(luabind
  PUBLIC
  boost
  PRIVATE
  FastDynamicCast
  LuaJIT
)

target_compile_options(luabind
  PUBLIC
  $<$<CXX_COMPILER_ID:MSVC>:/wd4456>
  $<$<CXX_COMPILER_ID:MSVC>:/wd4459>
  PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/wd4100>
  $<$<CXX_COMPILER_ID:Clang>:-Wno-microsoft-extra-qualification>
  $<$<CXX_COMPILER_ID:Clang>:-Wno-microsoft-unqualified-friend>
  $<$<CXX_COMPILER_ID:AppleClang>:-Wno-microsoft-extra-qualification>
  $<$<CXX_COMPILER_ID:AppleClang>:-Wno-microsoft-unqualified-friend>
)

target_compile_definitions(luabind
  PUBLIC
  LUABIND_NO_EXCEPTIONS
  PRIVATE
  LUABIND_BUILDING
)

target_sources(luabind PRIVATE
  "${CMAKE_SOURCE_DIR}/src/3rd party/luabind/luabind/src/class.cpp"
  "${CMAKE_SOURCE_DIR}/src/3rd party/luabind/luabind/src/class_info.cpp"
  "${CMAKE_SOURCE_DIR}/src/3rd party/luabind/luabind/src/class_registry.cpp"
  "${CMAKE_SOURCE_DIR}/src/3rd party/luabind/luabind/src/class_rep.cpp"
  "${CMAKE_SOURCE_DIR}/src/3rd party/luabind/luabind/src/create_class.cpp"
  "${CMAKE_SOURCE_DIR}/src/3rd party/luabind/luabind/src/error.cpp"
  "${CMAKE_SOURCE_DIR}/src/3rd party/luabind/luabind/src/find_best_match.cpp"
  "${CMAKE_SOURCE_DIR}/src/3rd party/luabind/luabind/src/function.cpp"
  "${CMAKE_SOURCE_DIR}/src/3rd party/luabind/luabind/src/implicit_cast.cpp"
  "${CMAKE_SOURCE_DIR}/src/3rd party/luabind/luabind/src/link_compatibility.cpp"
  "${CMAKE_SOURCE_DIR}/src/3rd party/luabind/luabind/src/object.cpp"
  "${CMAKE_SOURCE_DIR}/src/3rd party/luabind/luabind/src/object_rep.cpp"
  "${CMAKE_SOURCE_DIR}/src/3rd party/luabind/luabind/src/open.cpp"
  "${CMAKE_SOURCE_DIR}/src/3rd party/luabind/luabind/src/pcall.cpp"
  "${CMAKE_SOURCE_DIR}/src/3rd party/luabind/luabind/src/ref.cpp"
  "${CMAKE_SOURCE_DIR}/src/3rd party/luabind/luabind/src/scope.cpp"
  "${CMAKE_SOURCE_DIR}/src/3rd party/luabind/luabind/src/stack_content_by_name.cpp"
  "${CMAKE_SOURCE_DIR}/src/3rd party/luabind/luabind/src/weak_ref.cpp"
  "${CMAKE_SOURCE_DIR}/src/3rd party/luabind/luabind/src/wrapper_base.cpp"
)
