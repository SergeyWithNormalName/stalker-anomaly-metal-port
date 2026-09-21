if(NOT APPLE)
  message(FATAL_ERROR "The Direct Metal shader toolchain probe requires macOS")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/XRay.MetalShaderToolchain.cmake")

message(STATUS "DXC executable: ${XRAY_DXC_EXECUTABLE}")
if(XRAY_DXC_VERSION)
  message(STATUS "DXC version: ${XRAY_DXC_VERSION}")
else()
  message(STATUS "DXC version: unavailable")
endif()

message(STATUS
  "Metal Shader Converter executable: ${XRAY_METAL_SHADER_CONVERTER_EXECUTABLE}")
if(XRAY_METAL_SHADER_CONVERTER_VERSION)
  message(STATUS
    "Metal Shader Converter version: ${XRAY_METAL_SHADER_CONVERTER_VERSION}")
else()
  message(STATUS "Metal Shader Converter version: unavailable")
endif()

message(STATUS
  "Metal IR Converter include: ${XRAY_METAL_IRCONVERTER_INCLUDE_DIR}")
message(STATUS
  "Metal IR Converter library: ${XRAY_METAL_IRCONVERTER_LIBRARY}")

xray_require_metal_shader_toolchain()
message(STATUS "Direct Metal shader toolchain: READY")
