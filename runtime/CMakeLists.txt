set(RUNTIME_SOURCES
  instance.cpp
  session.cpp
  input.cpp
  hands.cpp
  dispatch.cpp
  ipc.cpp
  upscale.cpp
)

if(WIN32)
  add_library(metalvr_openxr SHARED ${RUNTIME_SOURCES} backend_win32.cpp backend_d3d11.cpp backend_d3d12.cpp)
  # one self-contained DLL: no MinGW runtime DLLs to ship into a prefix
  target_link_options(metalvr_openxr PRIVATE -static -static-libgcc -static-libstdc++)
  target_link_libraries(metalvr_openxr PRIVATE ws2_32 dxgi d3d12)
  set_target_properties(metalvr_openxr PROPERTIES PREFIX "" OUTPUT_NAME "metalvr_openxr")
  set(METALVR_MANIFEST_LIBRARY ".\\\\metalvr_openxr.dll")
elseif(APPLE)
  add_library(metalvr_openxr SHARED ${RUNTIME_SOURCES} backend_metal.mm)
  set_source_files_properties(backend_metal.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_link_libraries(metalvr_openxr PRIVATE "-framework Foundation" "-framework Metal" "-framework IOSurface")
  set(METALVR_MANIFEST_LIBRARY "./libmetalvr_openxr.dylib")
else()
  message(FATAL_ERROR "MetalVR's runtime builds for Windows (MinGW) and macOS")
endif()

target_include_directories(metalvr_openxr PRIVATE
  "${OPENXR_INCLUDE_DIR}"
  "${CMAKE_SOURCE_DIR}/protocol"
)
# only the loader's negotiation entry point is visible from outside
set_target_properties(metalvr_openxr PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  OBJCXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)

configure_file(metalvr_openxr.json.in "${CMAKE_CURRENT_BINARY_DIR}/metalvr_openxr.json" @ONLY)
