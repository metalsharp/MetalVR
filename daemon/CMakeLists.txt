# The ALVR driver needs ALVR's server_core, which scripts/build-alvr.sh builds into build/alvr. It is linked when
# that library is there, so a tree without it still builds a metalvrd that has only the simulated headset.
set(METALVR_ALVR_LIBRARY "${CMAKE_SOURCE_DIR}/build/alvr/target/release/libalvr_server_core.a"
    CACHE FILEPATH "libalvr_server_core built by scripts/build-alvr.sh")
set(METALVR_ALVR_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/build/alvr/include"
    CACHE PATH "directory of the generated alvr_server_core.h")
# what scripts/build-alvr.sh prints as ALVR_LINK_FLAGS; each framework is one token so repeated options survive
set(METALVR_ALVR_LINK_FLAGS
    "-Wl,-framework,CoreFoundation -lobjc -Wl,-framework,IOKit -Wl,-framework,AudioUnit -Wl,-framework,CoreAudio -Wl,-framework,Security -lbz2 -Wl,-framework,AppKit -Wl,-framework,Foundation -liconv"
    CACHE STRING "link flags the ALVR library needs")
if(EXISTS "${METALVR_ALVR_LIBRARY}" AND EXISTS "${METALVR_ALVR_INCLUDE_DIR}/alvr_server_core.h")
  set(alvr_default ON)
else()
  set(alvr_default OFF)
endif()
option(METALVR_WITH_ALVR "Link ALVR's server_core for streaming to a headset" ${alvr_default})

add_executable(metalvrd
  main.mm
  server.mm
  compositor.mm
  encoder.mm
  shared_texture.mm
  driver_sim.cpp
  headsets.cpp
  foveation.cpp
  json_value.cpp
  driver_rift.mm
  direct_models.cpp
  backend_rift_s.mm
  rift_s_protocol.cpp
  backend_lighthouse.mm
  backend_psvr2.mm
  psvr2_protocol.cpp
  lighthouse_protocol.cpp
  lighthouse_tracking.cpp
  camera_passthrough.mm
  camera_tracking.cpp
  rift_s_tracking.cpp
  rift_s_cameras.mm
  window_capture.mm
)

# The Rift S's camera tracking loads Basalt (scripts/build-basalt.sh) when it starts, as Monado does: looked for in
# this build's output first, then next to metalvrd. Nothing is linked.
set(METALVR_BASALT_LIBRARY_DIR "${CMAKE_SOURCE_DIR}/build/basalt/prefix/lib" CACHE PATH
  "where scripts/build-basalt.sh puts libbasalt.dylib")
set_property(SOURCE camera_tracking.cpp APPEND PROPERTY COMPILE_DEFINITIONS
  "METALVR_BASALT_DIR=\"${METALVR_BASALT_LIBRARY_DIR}\"")

# Position tracking for the Lighthouse headsets with their base stations is libsurvive (MIT) on hidapi (BSD), which
# scripts/build-libsurvive.sh builds statically into build/libsurvive. Its parts register themselves as the program
# starts, so each is linked whole. Without it the Lighthouse driver tracks rotation only.
set(METALVR_LIBSURVIVE_DIR "${CMAKE_SOURCE_DIR}/build/libsurvive" CACHE PATH "scripts/build-libsurvive.sh's output")
if(EXISTS "${METALVR_LIBSURVIVE_DIR}/build/src/libsurvive.a")
  set(libsurvive_default ON)
else()
  set(libsurvive_default OFF)
endif()
option(METALVR_WITH_LIBSURVIVE "Track the Lighthouse headsets' position with libsurvive" ${libsurvive_default})
if(METALVR_WITH_LIBSURVIVE)
  set(ls "${METALVR_LIBSURVIVE_DIR}/build")
  set(ls_src "${CMAKE_SOURCE_DIR}/external/libsurvive")
  if(NOT EXISTS "${ls}/src/libsurvive.a")
    message(FATAL_ERROR "METALVR_WITH_LIBSURVIVE needs ${ls}; run scripts/build-libsurvive.sh")
  endif()
  message(STATUS "metalvrd: with libsurvive for Lighthouse positions (${ls})")
  set_source_files_properties(lighthouse_tracking.cpp PROPERTIES COMPILE_DEFINITIONS METALVR_LIBSURVIVE=1)
  target_include_directories(metalvrd PRIVATE "${ls_src}/include/libsurvive" "${ls_src}/include" "${ls_src}/redist"
    "${ls_src}/libs/cnmatrix/include" "${ls_src}/libs/cnkalman/include"
    "${METALVR_LIBSURVIVE_DIR}/prefix/include/hidapi")
  foreach(plugin driver_vive driver_simulator disambiguator_statebased poser_kalman_only poser_mpfit
                 driver_global_scene_solver)
    target_link_options(metalvrd PRIVATE "-Wl,-force_load,${ls}/src/lib${plugin}.a")
  endforeach()
  target_link_libraries(metalvrd PRIVATE "${ls}/src/libsurvive.a" "${ls}/redist/libmpfit.a"
    "${ls}/libs/cnkalman/src/libcnkalman.a" "${ls}/libs/cnmatrix/src/libcnmatrix.a"
    "${METALVR_LIBSURVIVE_DIR}/prefix/lib/libhidapi.a")
else()
  message(STATUS "metalvrd: without libsurvive (scripts/build-libsurvive.sh builds it); Lighthouse rotation only")
endif()

if(METALVR_WITH_ALVR)
  if(NOT EXISTS "${METALVR_ALVR_LIBRARY}")
    message(FATAL_ERROR "METALVR_WITH_ALVR needs ${METALVR_ALVR_LIBRARY}; run scripts/build-alvr.sh")
  endif()
  message(STATUS "metalvrd: with the ALVR driver (${METALVR_ALVR_LIBRARY})")
  target_sources(metalvrd PRIVATE driver_alvr.cpp)
  target_include_directories(metalvrd PRIVATE "${METALVR_ALVR_INCLUDE_DIR}")
  target_link_libraries(metalvrd PRIVATE "${METALVR_ALVR_LIBRARY}")
  separate_arguments(alvr_flags NATIVE_COMMAND "${METALVR_ALVR_LINK_FLAGS}")
  target_link_options(metalvrd PRIVATE ${alvr_flags})
else()
  message(STATUS "metalvrd: without the ALVR driver (scripts/build-alvr.sh builds it)")
  target_sources(metalvrd PRIVATE driver_alvr_stub.cpp)
endif()

# Positional tracking for the Rift CV1 with its sensors is OpenHMD's tracker (Boost Software License 1.0), which
# scripts/fetch-deps.sh puts in external/OpenHMD-rift. Its sources are compiled unchanged; rift_tracking/ gives
# them the libusb calls they make (on IOUSBHost) and gives the Rift driver a narrow interface to them. Without
# the sources the Rift driver tracks rotation only.
set(METALVR_OPENHMD_DIR "${CMAKE_SOURCE_DIR}/external/OpenHMD-rift" CACHE PATH "OpenHMD with the Rift tracker")
if(EXISTS "${METALVR_OPENHMD_DIR}/src/drv_oculus_rift/rift-tracker.c")
  set(rift_tracking_default ON)
else()
  set(rift_tracking_default OFF)
endif()
option(METALVR_WITH_RIFT_TRACKING "Track the Rift CV1's position with its sensors" ${rift_tracking_default})

if(METALVR_WITH_RIFT_TRACKING)
  set(ohmd "${METALVR_OPENHMD_DIR}/src")
  set(rift "${ohmd}/drv_oculus_rift")
  if(NOT EXISTS "${rift}/rift-tracker.c")
    message(FATAL_ERROR "METALVR_WITH_RIFT_TRACKING needs ${METALVR_OPENHMD_DIR}; run scripts/fetch-deps.sh")
  endif()
  message(STATUS "metalvrd: with Rift positional tracking (${METALVR_OPENHMD_DIR})")
  # rift-tracker.c is not listed: rift_tracking.c includes it
  add_library(rift_tracking STATIC
    rift_tracking/rift_tracking.c
    rift_tracking/ohmd_compat.c
    rift_tracking/camera_tap.c
    rift_tracking/camera_math.c
    rift_tracking/uvc_stop.c
    rift_tracking/camera_jpeg.c
    rift_tracking/usb_shim.mm
    "${ohmd}/omath.c"
    "${ohmd}/matrices.c"
    "${ohmd}/ukf.c"
    "${ohmd}/unscented.c"
    "${ohmd}/exponential-filter.c"
    "${ohmd}/platform-posix.c"
    "${rift}/packet.c"
    "${rift}/correspondence_search.c"
    "${rift}/led_search.c"
    "${rift}/ohmd-video.c"
    "${rift}/rift-kalman-6dof.c"
    "${rift}/rift-sensor.c"
    "${rift}/rift-sensor-blobwatch.c"
    "${rift}/rift-sensor-flicker.c"
    "${rift}/rift-sensor-maths.c"
    "${rift}/rift-sensor-pose-helper.c"
    "${rift}/rift-sensor-pose-search.c"
    "${rift}/rift-sensor-usb.c"
    "${rift}/sensor/ar0134.c"
    "${rift}/sensor/esp570.c"
    "${rift}/sensor/esp770u.c"
    "${rift}/sensor/mt9v034.c"
    "${rift}/sensor/uvc.c"
  )
  target_include_directories(rift_tracking
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/rift_tracking"
    PRIVATE "${ohmd}" "${METALVR_OPENHMD_DIR}/include" "${rift}")
  # OpenHMD's sources are not ours to tidy: their warnings are off, and what they assume of Linux is supplied by
  # ohmd_compat.h, included in front of each of them
  # HAVE_OPENCV: the camera geometry OpenHMD takes from OpenCV is in rift_tracking/camera_math.c
  # HAVE_LIBJPEG: a sensor's JPEG pictures (on USB 2) are decoded by rift_tracking/camera_jpeg.c, on ImageIO
  target_compile_definitions(rift_tracking PRIVATE HAVE_LIBJPEG=1 HAVE_OPENCV=1 DRIVER_OCULUS_RIFT)
  target_compile_options(rift_tracking PRIVATE -w -fobjc-arc
    $<$<COMPILE_LANGUAGE:C>:-include> $<$<COMPILE_LANGUAGE:C>:${CMAKE_CURRENT_SOURCE_DIR}/rift_tracking/ohmd_compat.h>)
  # camera_tap.c sees every picture on its way to the blob detector, and each sensor's pose finder as it is set up
  set_source_files_properties("${rift}/rift-sensor.c" PROPERTIES
    COMPILE_DEFINITIONS "blobwatch_process=metalvr_blobwatch_process;rift_pose_finder_init=metalvr_pose_finder_init;rift_tracker_frame_captured=metalvr_frame_captured;rift_tracked_device_model_pose_update=metalvr_model_pose_update")
  # uvc_stop.c stops a sensor's video without freeing frames the analysis threads still hold
  set_source_files_properties("${rift}/rift-sensor-usb.c" PROPERTIES COMPILE_DEFINITIONS
    "rift_sensor_uvc_stream_stop=metalvr_uvc_stream_stop;rift_sensor_uvc_stream_clear=metalvr_uvc_stream_clear")
  target_link_libraries(rift_tracking PUBLIC "-framework Foundation" "-framework IOKit" "-framework IOUSBHost"
    "-framework CoreGraphics" "-framework ImageIO")
  target_link_libraries(metalvrd PRIVATE rift_tracking)
  target_compile_definitions(metalvrd PRIVATE METALVR_RIFT_TRACKING=1)
else()
  message(STATUS "metalvrd: without Rift positional tracking (scripts/fetch-deps.sh fetches OpenHMD)")
endif()

target_include_directories(metalvrd PRIVATE "${CMAKE_SOURCE_DIR}/protocol" "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(metalvrd PRIVATE -fobjc-arc)
target_link_libraries(metalvrd PRIVATE
  "-framework Foundation"
  "-framework Metal"
  "-framework MetalFX"
  "-framework CoreVideo"
  "-framework CoreMedia"
  "-framework VideoToolbox"
  "-framework IOSurface"
  "-framework CoreGraphics"
  "-framework ImageIO"
  "-framework UniformTypeIdentifiers"
  "-framework AppKit"
  "-framework IOKit"
  "-framework IOUSBHost"
  "-framework QuartzCore"
  "-framework ScreenCaptureKit"
  "-framework AVFoundation"
  z
)
