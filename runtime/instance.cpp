// Instance, system, paths, events and the object registry.
#include "api.h"
#include "metalvr_headsets.h"
#include "upscale.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

#include <openxr/openxr_reflection.h>

namespace metalvr {

// ---- logging -------------------------------------------------------------------------------------------------

void logLine(const char *fmt, ...) {
  static const bool enabled = [] {
    const char *env = std::getenv("METALVR_LOG");
    return env && *env && *env != '0';
  }();
  if (!enabled)
    return;
  va_list args;
  va_start(args, fmt);
  std::fputs("[metalvr] ", stderr);
  std::vfprintf(stderr, fmt, args);
  std::fputc('\n', stderr);
  std::fflush(stderr);
  va_end(args);
}

// ---- object registry -----------------------------------------------------------------------------------------

namespace {
std::mutex g_objects_mutex;
std::unordered_map<void *, ObjectType> &objects() {
  static auto *map = new std::unordered_map<void *, ObjectType>();
  return *map;
}
Instance *g_instance = nullptr;
} // namespace

Object::Object(ObjectType t) : type(t) {
  std::lock_guard<std::mutex> lock(g_objects_mutex);
  objects().emplace(this, t);
}

Object::~Object() {
  std::lock_guard<std::mutex> lock(g_objects_mutex);
  objects().erase(this);
}

Object *lookupObject(void *handle, ObjectType type) {
  if (!handle)
    return nullptr;
  std::lock_guard<std::mutex> lock(g_objects_mutex);
  auto found = objects().find(handle);
  if (found == objects().end() || found->second != type)
    return nullptr;
  return static_cast<Object *>(handle);
}

// ---- instance ------------------------------------------------------------------------------------------------

Instance::~Instance() {
  session.reset();
  action_sets.clear();
  connection.close();
}

XrPath Instance::path(const std::string &string) {
  auto found = path_ids.find(string);
  if (found != path_ids.end())
    return found->second;
  paths.push_back(string);
  const XrPath id = static_cast<XrPath>(paths.size());
  path_ids.emplace(string, id);
  return id;
}

const std::string *Instance::pathString(XrPath path) const {
  if (path == XR_NULL_PATH || path > paths.size())
    return nullptr;
  return &paths[path - 1];
}

void Instance::markLost() {
  lost = true;
  if (loss_announced)
    return;
  loss_announced = true;
  logLine("the MetalVR server stopped answering; the instance is lost");
  XrEventDataInstanceLossPending event{XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING};
  event.lossTime = now();
  pushEvent(event);
  if (session && session->state != XR_SESSION_STATE_LOSS_PENDING)
    session->setState(XR_SESSION_STATE_LOSS_PENDING);
}

bool Instance::refreshSystem() {
  mvr_system_response response{};
  if (!connection.call(MVR_MSG_SYSTEM, response) || response.result != MVR_OK) {
    markLost();
    return false;
  }
  system = response.system;
  return true;
}

namespace {

struct ExtensionInfo {
  const char *name;
  uint32_t version;
};

const ExtensionInfo kExtensions[] = {
#ifdef _WIN32
    {XR_KHR_D3D11_ENABLE_EXTENSION_NAME, XR_KHR_D3D11_enable_SPEC_VERSION},
    {XR_KHR_D3D12_ENABLE_EXTENSION_NAME, XR_KHR_D3D12_enable_SPEC_VERSION},
    {XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME,
     XR_KHR_win32_convert_performance_counter_time_SPEC_VERSION},
#else
    {XR_KHR_METAL_ENABLE_EXTENSION_NAME, XR_KHR_metal_enable_SPEC_VERSION},
    {XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME, XR_KHR_convert_timespec_time_SPEC_VERSION},
#endif
    // depth submitted with a projection layer is accepted and not used
    {XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME, XR_KHR_composition_layer_depth_SPEC_VERSION},
    // the mask is empty: every pixel of the image is visible
    {XR_KHR_VISIBILITY_MASK_EXTENSION_NAME, XR_KHR_visibility_mask_SPEC_VERSION},
    {XR_EXT_LOCAL_FLOOR_EXTENSION_NAME, XR_EXT_local_floor_SPEC_VERSION},
    {XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME, XR_FB_display_refresh_rate_SPEC_VERSION},
    // interaction profiles: suggested bindings for them are read on whatever controllers the user holds
    {XR_META_TOUCH_CONTROLLER_PLUS_EXTENSION_NAME, XR_META_touch_controller_plus_SPEC_VERSION},
    // the hands' joints, when the headset tracks bare hands (hands.cpp)
    {XR_EXT_HAND_TRACKING_EXTENSION_NAME, XR_EXT_hand_tracking_SPEC_VERSION},
    // Valve's, for the Steam Frame's controllers; not in the Khronos registry (docs/HEADSETS.md)
    {"XR_VALVE_frame_controller_interaction", 1},
};

bool extensionSupported(const char *name) {
  for (const ExtensionInfo &extension : kExtensions)
    if (!std::strcmp(extension.name, name))
      return true;
  return false;
}

} // namespace

namespace api {

MVR_API xrEnumerateApiLayerProperties(uint32_t, uint32_t *count, XrApiLayerProperties *) {
  if (!count)
    return XR_ERROR_VALIDATION_FAILURE;
  *count = 0;
  return XR_SUCCESS;
}

MVR_API xrEnumerateInstanceExtensionProperties(const char *layer, uint32_t capacity, uint32_t *count,
                                               XrExtensionProperties *properties) {
  if (layer)
    return XR_ERROR_API_LAYER_NOT_PRESENT;
  if (!count)
    return XR_ERROR_VALIDATION_FAILURE;
  const uint32_t n = static_cast<uint32_t>(sizeof(kExtensions) / sizeof(kExtensions[0]));
  *count = n;
  if (capacity == 0)
    return XR_SUCCESS;
  if (capacity < n)
    return XR_ERROR_SIZE_INSUFFICIENT;
  if (!properties)
    return XR_ERROR_VALIDATION_FAILURE;
  for (uint32_t i = 0; i < n; ++i) {
    if (properties[i].type != XR_TYPE_EXTENSION_PROPERTIES)
      return XR_ERROR_VALIDATION_FAILURE;
    copyString(properties[i].extensionName, kExtensions[i].name);
    properties[i].extensionVersion = kExtensions[i].version;
  }
  return XR_SUCCESS;
}

MVR_API xrCreateInstance(const XrInstanceCreateInfo *info, XrInstance *out) {
  if (!info || !out || info->type != XR_TYPE_INSTANCE_CREATE_INFO)
    return XR_ERROR_VALIDATION_FAILURE;
  const XrVersion requested = info->applicationInfo.apiVersion;
  if (XR_VERSION_MAJOR(requested) != 1 || XR_VERSION_MINOR(requested) > XR_VERSION_MINOR(XR_CURRENT_API_VERSION))
    return XR_ERROR_API_VERSION_UNSUPPORTED;
  for (uint32_t i = 0; i < info->enabledExtensionCount; ++i)
    if (!info->enabledExtensionNames[i] || !extensionSupported(info->enabledExtensionNames[i])) {
      logLine("xrCreateInstance: extension %s is not supported",
              info->enabledExtensionNames[i] ? info->enabledExtensionNames[i] : "(null)");
      return XR_ERROR_EXTENSION_NOT_PRESENT;
    }
  {
    std::lock_guard<std::mutex> lock(g_objects_mutex);
    if (g_instance)
      return XR_ERROR_LIMIT_REACHED;
  }

  auto instance = std::make_unique<Instance>();
  instance->api_version = requested;
  for (uint32_t i = 0; i < info->enabledExtensionCount; ++i)
    instance->extensions.insert(info->enabledExtensionNames[i]);

  // no server, no runtime: an application that can run without VR does
  if (!instance->connection.connect()) {
    logLine("xrCreateInstance: metalvrd is not running");
    return XR_ERROR_RUNTIME_UNAVAILABLE;
  }
  mvr_hello_request hello{};
  hello.version = MVR_PROTOCOL_VERSION;
  hello.pid = currentProcessId();
  hello.graphics_api = platformGraphicsApi();
#ifdef _WIN32
  // which Direct3D is only certain once a session is made; until then, the one extension the application enabled
  if (instance->extensions.count(XR_KHR_D3D12_ENABLE_EXTENSION_NAME) &&
      !instance->extensions.count(XR_KHR_D3D11_ENABLE_EXTENSION_NAME))
    hello.graphics_api = MVR_GFX_D3D12;
#endif
  copyString(hello.application_name, std::string(info->applicationInfo.applicationName,
                                                 strnlen(info->applicationInfo.applicationName,
                                                         XR_MAX_APPLICATION_NAME_SIZE)));
  mvr_hello_response response{};
  if (!instance->connection.call(MVR_MSG_HELLO, hello, response) || response.result != MVR_OK) {
    logLine("xrCreateInstance: metalvrd refused the connection (protocol %u, result %u)", response.version,
            response.result);
    return XR_ERROR_RUNTIME_UNAVAILABLE;
  }
  instance->system = response.system;
  if (!instance->connection.syncClock())
    return XR_ERROR_RUNTIME_UNAVAILABLE;

  logLine("instance for '%s', OpenXR %u.%u, headset '%s'%s", hello.application_name, XR_VERSION_MAJOR(requested),
          XR_VERSION_MINOR(requested), instance->system.system_name,
          (instance->system.flags & MVR_SYSTEM_HMD_CONNECTED) ? "" : " (not connected)");
  {
    std::lock_guard<std::mutex> lock(g_objects_mutex);
    g_instance = instance.get();
  }
  *out = reinterpret_cast<XrInstance>(instance.release());
  return XR_SUCCESS;
}

MVR_API xrDestroyInstance(XrInstance handle) {
  Instance *instance = lookup<Instance>(handle);
  if (!instance)
    return XR_ERROR_HANDLE_INVALID;
  {
    std::lock_guard<std::recursive_mutex> lock(instance->mutex);
    instance->session.reset();
  }
  {
    std::lock_guard<std::mutex> lock(g_objects_mutex);
    if (g_instance == instance)
      g_instance = nullptr;
  }
  delete instance;
  return XR_SUCCESS;
}

MVR_API xrGetInstanceProperties(XrInstance handle, XrInstanceProperties *properties) {
  MVR_INSTANCE(handle);
  if (!properties || properties->type != XR_TYPE_INSTANCE_PROPERTIES)
    return XR_ERROR_VALIDATION_FAILURE;
  properties->runtimeVersion = METALVR_RUNTIME_VERSION;
  copyString(properties->runtimeName, METALVR_RUNTIME_NAME);
  return XR_SUCCESS;
}

MVR_API xrPollEvent(XrInstance handle, XrEventDataBuffer *event) {
  MVR_INSTANCE(handle);
  if (!event)
    return XR_ERROR_VALIDATION_FAILURE;
  // an application that is not in its frame loop still learns that the headset came or went
  if (Session *session = instance->session.get(); session && !instance->lost) {
    const int64_t now = localNowNs();
    if (now - session->last_presence_check_ns > 100'000'000) {
      mvr_tracking_response sample{};
      if (session->tracking(0, sample))
        session->updatePresence(sample.flags);
    }
  }
  if (instance->events.empty())
    return XR_EVENT_UNAVAILABLE;
  *event = instance->events.front();
  instance->events.pop_front();
  return XR_SUCCESS;
}

MVR_API xrResultToString(XrInstance handle, XrResult value, char buffer[XR_MAX_RESULT_STRING_SIZE]) {
  MVR_INSTANCE(handle);
  if (!buffer)
    return XR_ERROR_VALIDATION_FAILURE;
  const char *name = nullptr;
  switch (value) {
#define MVR_CASE(n, v)                                                                                                 \
  case n:                                                                                                              \
    name = #n;                                                                                                         \
    break;
    XR_LIST_ENUM_XrResult(MVR_CASE)
#undef MVR_CASE
  default:
    break;
  }
  if (name)
    std::snprintf(buffer, XR_MAX_RESULT_STRING_SIZE, "%s", name);
  else
    std::snprintf(buffer, XR_MAX_RESULT_STRING_SIZE, value < 0 ? "XR_UNKNOWN_FAILURE_%d" : "XR_UNKNOWN_SUCCESS_%d",
                  static_cast<int>(value));
  return XR_SUCCESS;
}

MVR_API xrStructureTypeToString(XrInstance handle, XrStructureType value,
                                char buffer[XR_MAX_STRUCTURE_NAME_SIZE]) {
  MVR_INSTANCE(handle);
  if (!buffer)
    return XR_ERROR_VALIDATION_FAILURE;
  const char *name = nullptr;
  switch (value) {
#define MVR_CASE(n, v)                                                                                                 \
  case n:                                                                                                              \
    name = #n;                                                                                                         \
    break;
    XR_LIST_ENUM_XrStructureType(MVR_CASE)
#undef MVR_CASE
  default:
    break;
  }
  if (name)
    std::snprintf(buffer, XR_MAX_STRUCTURE_NAME_SIZE, "%s", name);
  else
    std::snprintf(buffer, XR_MAX_STRUCTURE_NAME_SIZE, "XR_UNKNOWN_STRUCTURE_TYPE_%d", static_cast<int>(value));
  return XR_SUCCESS;
}

// ---- system --------------------------------------------------------------------------------------------------

MVR_API xrGetSystem(XrInstance handle, const XrSystemGetInfo *info, XrSystemId *system_id) {
  Instance *instance = lookup<Instance>(handle);
  if (!instance)
    return XR_ERROR_HANDLE_INVALID;
  if (!info || !system_id || info->type != XR_TYPE_SYSTEM_GET_INFO)
    return XR_ERROR_VALIDATION_FAILURE;
  if (info->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY)
    return XR_ERROR_FORM_FACTOR_UNSUPPORTED;

  // The headset's own resolution and field of view are only known once it is there, so without one there is no
  // system. METALVR_HEADSET_WAIT gives a headset that many seconds to connect before that answer: an application
  // that treats a missing headset as fatal (an OpenVR game, through OpenComposite) can then be started first.
  double wait_s = 0;
  if (const char *env = std::getenv("METALVR_HEADSET_WAIT"))
    wait_s = std::strtod(env, nullptr);
  if (!(wait_s > 0))
    wait_s = 0;
  if (wait_s > 600)
    wait_s = 600;
  const int64_t deadline = localNowNs() + static_cast<int64_t>(wait_s * 1e9);
  bool announced = false;
  for (;;) {
    {
      // held per poll, not across the wait: another thread's calls go through meanwhile
      std::lock_guard<std::recursive_mutex> instance_lock(instance->mutex);
      MVR_ALIVE();
      if (!instance->refreshSystem())
        return XR_ERROR_INSTANCE_LOST;
      if (instance->system.flags & MVR_SYSTEM_HMD_CONNECTED) {
        *system_id = kSystemId;
        return XR_SUCCESS;
      }
    }
    if (localNowNs() >= deadline)
      return XR_ERROR_FORM_FACTOR_UNAVAILABLE;
    if (!announced) {
      announced = true;
      logLine("xrGetSystem: no headset yet; waiting up to %.0f s for one to connect", wait_s);
    }
    sleepUntilLocalNs(localNowNs() + 250'000'000);
    if (!lookup<Instance>(handle))
      return XR_ERROR_HANDLE_INVALID;
  }
}

MVR_API xrGetSystemProperties(XrInstance handle, XrSystemId system_id, XrSystemProperties *properties) {
  MVR_INSTANCE(handle);
  if (!properties || properties->type != XR_TYPE_SYSTEM_PROPERTIES)
    return XR_ERROR_VALIDATION_FAILURE;
  if (system_id != kSystemId)
    return XR_ERROR_SYSTEM_INVALID;
  properties->systemId = kSystemId;
  properties->vendorId = mvr_describe_headset(instance->system.headset)->vendor_id;
  copyString(properties->systemName, std::string(instance->system.system_name,
                                                 strnlen(instance->system.system_name,
                                                         sizeof(instance->system.system_name))));
  properties->graphicsProperties.maxSwapchainImageWidth = kMaxSwapchainSize;
  properties->graphicsProperties.maxSwapchainImageHeight = kMaxSwapchainSize;
  properties->graphicsProperties.maxLayerCount = MVR_MAX_LAYERS;
  properties->trackingProperties.orientationTracking = XR_TRUE;
  properties->trackingProperties.positionTracking = XR_TRUE;
  for (auto *next = static_cast<XrBaseOutStructure *>(properties->next); next; next = next->next)
    if (next->type == XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT)
      reinterpret_cast<XrSystemHandTrackingPropertiesEXT *>(next)->supportsHandTracking =
          (instance->system.flags & MVR_SYSTEM_HAND_TRACKING) ? XR_TRUE : XR_FALSE;
  return XR_SUCCESS;
}

MVR_API xrEnumerateEnvironmentBlendModes(XrInstance handle, XrSystemId system_id, XrViewConfigurationType type,
                                         uint32_t capacity, uint32_t *count, XrEnvironmentBlendMode *modes) {
  MVR_INSTANCE(handle);
  if (system_id != kSystemId)
    return XR_ERROR_SYSTEM_INVALID;
  if (type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
    return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
  return fillArray<XrEnvironmentBlendMode>(capacity, count, modes, {XR_ENVIRONMENT_BLEND_MODE_OPAQUE});
}

MVR_API xrEnumerateViewConfigurations(XrInstance handle, XrSystemId system_id, uint32_t capacity, uint32_t *count,
                                      XrViewConfigurationType *types) {
  MVR_INSTANCE(handle);
  if (system_id != kSystemId)
    return XR_ERROR_SYSTEM_INVALID;
  return fillArray<XrViewConfigurationType>(capacity, count, types, {XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO});
}

MVR_API xrGetViewConfigurationProperties(XrInstance handle, XrSystemId system_id, XrViewConfigurationType type,
                                         XrViewConfigurationProperties *properties) {
  MVR_INSTANCE(handle);
  if (!properties || properties->type != XR_TYPE_VIEW_CONFIGURATION_PROPERTIES)
    return XR_ERROR_VALIDATION_FAILURE;
  if (system_id != kSystemId)
    return XR_ERROR_SYSTEM_INVALID;
  if (type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
    return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
  properties->viewConfigurationType = type;
  properties->fovMutable = XR_FALSE;
  return XR_SUCCESS;
}

MVR_API xrEnumerateViewConfigurationViews(XrInstance handle, XrSystemId system_id, XrViewConfigurationType type,
                                          uint32_t capacity, uint32_t *count, XrViewConfigurationView *views) {
  MVR_INSTANCE(handle);
  if (system_id != kSystemId)
    return XR_ERROR_SYSTEM_INVALID;
  if (type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
    return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
  if (!count)
    return XR_ERROR_VALIDATION_FAILURE;
  *count = 2;
  if (capacity == 0)
    return XR_SUCCESS;
  if (capacity < 2)
    return XR_ERROR_SIZE_INSUFFICIENT;
  if (!views)
    return XR_ERROR_VALIDATION_FAILURE;
  // with MetalFX spatial upscaling the application renders smaller eyes and metalvrd scales them up (upscale.h)
  const uint32_t width = upscaledRenderSize(instance->system.eye_width);
  const uint32_t height = upscaledRenderSize(instance->system.eye_height);
  static std::once_flag logged;
  if (spatialUpscale().enabled)
    std::call_once(logged, [&] {
      logLine("MetalFX spatial upscaling: recommending %ux%u per eye for %ux%u (factor %.2f, from %s)", width, height,
              instance->system.eye_width, instance->system.eye_height, spatialUpscale().factor,
              spatialUpscale().source.c_str());
    });
  for (uint32_t i = 0; i < 2; ++i) {
    if (views[i].type != XR_TYPE_VIEW_CONFIGURATION_VIEW)
      return XR_ERROR_VALIDATION_FAILURE;
    views[i].recommendedImageRectWidth = width;
    views[i].recommendedImageRectHeight = height;
    views[i].maxImageRectWidth = kMaxSwapchainSize;
    views[i].maxImageRectHeight = kMaxSwapchainSize;
    views[i].recommendedSwapchainSampleCount = 1;
    views[i].maxSwapchainSampleCount = kMaxSampleCount;
  }
  return XR_SUCCESS;
}

// ---- paths ---------------------------------------------------------------------------------------------------

MVR_API xrStringToPath(XrInstance handle, const char *string, XrPath *path) {
  MVR_INSTANCE(handle);
  if (!string || !path)
    return XR_ERROR_VALIDATION_FAILURE;
  const size_t length = strnlen(string, XR_MAX_PATH_LENGTH);
  if (length == 0 || length >= XR_MAX_PATH_LENGTH || string[0] != '/')
    return XR_ERROR_PATH_FORMAT_INVALID;
  *path = instance->path(std::string(string, length));
  return XR_SUCCESS;
}

MVR_API xrPathToString(XrInstance handle, XrPath path, uint32_t capacity, uint32_t *count, char *buffer) {
  MVR_INSTANCE(handle);
  if (!count)
    return XR_ERROR_VALIDATION_FAILURE;
  const std::string *string = instance->pathString(path);
  if (!string)
    return XR_ERROR_PATH_INVALID;
  *count = static_cast<uint32_t>(string->size() + 1);
  if (capacity == 0)
    return XR_SUCCESS;
  if (capacity < *count)
    return XR_ERROR_SIZE_INSUFFICIENT;
  if (!buffer)
    return XR_ERROR_VALIDATION_FAILURE;
  std::memcpy(buffer, string->c_str(), *count);
  return XR_SUCCESS;
}

// ---- time ----------------------------------------------------------------------------------------------------
// XrTime is the server's clock; these convert to and from the application's.

#ifdef _WIN32

MVR_API xrConvertWin32PerformanceCounterToTimeKHR(XrInstance handle, const LARGE_INTEGER *counter, XrTime *time) {
  MVR_INSTANCE(handle);
  if (!counter || !time)
    return XR_ERROR_VALIDATION_FAILURE;
  LARGE_INTEGER frequency;
  QueryPerformanceFrequency(&frequency);
  const int64_t seconds = counter->QuadPart / frequency.QuadPart;
  const int64_t rest = counter->QuadPart % frequency.QuadPart;
  *time = seconds * 1'000'000'000 + rest * 1'000'000'000 / frequency.QuadPart + instance->connection.clockOffsetNs();
  return XR_SUCCESS;
}

MVR_API xrConvertTimeToWin32PerformanceCounterKHR(XrInstance handle, XrTime time, LARGE_INTEGER *counter) {
  MVR_INSTANCE(handle);
  if (!counter)
    return XR_ERROR_VALIDATION_FAILURE;
  if (time <= 0)
    return XR_ERROR_TIME_INVALID;
  LARGE_INTEGER frequency;
  QueryPerformanceFrequency(&frequency);
  const int64_t local = time - instance->connection.clockOffsetNs();
  counter->QuadPart = local / 1'000'000'000 * frequency.QuadPart + local % 1'000'000'000 * frequency.QuadPart / 1'000'000'000;
  return XR_SUCCESS;
}

#else

MVR_API xrConvertTimespecTimeToTimeKHR(XrInstance handle, const struct timespec *timespec, XrTime *time) {
  MVR_INSTANCE(handle);
  if (!timespec || !time)
    return XR_ERROR_VALIDATION_FAILURE;
  // the application's timespec is CLOCK_MONOTONIC; bring it to the local clock the offset was measured against
  const int64_t monotonic_now = static_cast<int64_t>(clock_gettime_nsec_np(CLOCK_MONOTONIC));
  const int64_t delta = localNowNs() - monotonic_now;
  const int64_t value = static_cast<int64_t>(timespec->tv_sec) * 1'000'000'000 + timespec->tv_nsec;
  *time = value + delta + instance->connection.clockOffsetNs();
  return XR_SUCCESS;
}

MVR_API xrConvertTimeToTimespecTimeKHR(XrInstance handle, XrTime time, struct timespec *timespec) {
  MVR_INSTANCE(handle);
  if (!timespec)
    return XR_ERROR_VALIDATION_FAILURE;
  if (time <= 0)
    return XR_ERROR_TIME_INVALID;
  const int64_t monotonic_now = static_cast<int64_t>(clock_gettime_nsec_np(CLOCK_MONOTONIC));
  const int64_t delta = localNowNs() - monotonic_now;
  const int64_t value = time - instance->connection.clockOffsetNs() - delta;
  timespec->tv_sec = static_cast<time_t>(value / 1'000'000'000);
  timespec->tv_nsec = static_cast<long>(value % 1'000'000'000);
  return XR_SUCCESS;
}

#endif

} // namespace api
} // namespace metalvr
