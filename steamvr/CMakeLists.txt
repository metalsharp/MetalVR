# MetalVR's SteamVR driver (Windows SteamVR under Wine): the headset metalvrd drives, as one of SteamVR's own, with
# SteamVR's frames handed over in direct mode. Builds the driver folder SteamVR loads, metalvr/, into this directory:
#   metalvr/driver.vrdrivermanifest, metalvr/resources/settings/default.vrsettings, metalvr/bin/win64/driver_metalvr.dll
set(OPENVR_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/external/ALVR/openvr/headers" CACHE PATH "OpenVR driver headers")
if(NOT EXISTS "${OPENVR_INCLUDE_DIR}/openvr_driver.h")
  message(STATUS "steamvr: no openvr_driver.h in ${OPENVR_INCLUDE_DIR}; the SteamVR driver is not built")
  return()
endif()

add_library(driver_metalvr SHARED
  driver_metalvr.cpp
  "${CMAKE_SOURCE_DIR}/runtime/ipc.cpp"
  "${CMAKE_SOURCE_DIR}/runtime/backend_win32.cpp"
  "${CMAKE_SOURCE_DIR}/runtime/upscale.cpp"
)
target_include_directories(driver_metalvr PRIVATE
  "${OPENVR_INCLUDE_DIR}"
  "${CMAKE_SOURCE_DIR}/runtime"
  "${CMAKE_SOURCE_DIR}/protocol"
  "${OPENXR_INCLUDE_DIR}"
)
target_link_options(driver_metalvr PRIVATE -static -static-libgcc -static-libstdc++)
target_link_libraries(driver_metalvr PRIVATE ws2_32 d3d11 dxgi)
set(driver_dir "${CMAKE_CURRENT_BINARY_DIR}/metalvr")
set_target_properties(driver_metalvr PROPERTIES
  PREFIX ""
  OUTPUT_NAME "driver_metalvr"
  RUNTIME_OUTPUT_DIRECTORY "${driver_dir}/bin/win64"
  LIBRARY_OUTPUT_DIRECTORY "${driver_dir}/bin/win64"
  CXX_VISIBILITY_PRESET hidden
)
add_custom_command(TARGET driver_metalvr POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/metalvr" "${driver_dir}"
)
