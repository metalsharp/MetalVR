// MetalVR's SteamVR driver: the headset metalvrd drives, as a headset of SteamVR's own.
//
// SteamVR (Windows, under Wine with Mullion) loads this driver into vrserver. It registers one HMD whose poses come
// from metalvrd, and takes SteamVR's frames in direct mode: SteamVR's compositor renders each eye into textures this
// driver made (IVRDriverDirectModeComponent::CreateSwapTextureSet), and hands them back in SubmitLayer and Present.
// Those textures are Mullion shared textures, so metalvrd opens the same Metal textures and shows them through its
// own lens correction, the way it shows an OpenXR application's swapchains. SteamVR does no lens correction of its
// own here: ComputeDistortion is the identity.
#include <winsock2.h>
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi.h>

#include <openvr_driver.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hand_skeleton.h"
#include "ipc.h"
#include "metalvr_headsets.h"
#include "metalvr_protocol.h"
#include "upscale.h"

namespace metalvr {
// backend_win32.cpp: the bootstrap name of a Mullion shared texture's Metal port, from an NT handle of its mapping
bool mullionShareNameFromHandle(HANDLE handle, char out[MVR_SHARE_NAME_LEN]);
} // namespace metalvr

namespace {

using namespace metalvr;

void log(const char *fmt, ...) {
  char line[1024];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(line, sizeof(line) - 2, fmt, args);
  va_end(args);
  std::strcat(line, "\n");
  if (vr::VRDriverLog())
    vr::VRDriverLog()->Log(line);
}

template <typename T> void release(T *&object) {
  if (object) {
    object->Release();
    object = nullptr;
  }
}

// ---- formats ----

struct Format {
  DXGI_FORMAT typed;
  DXGI_FORMAT typeless;
  uint32_t share; // mvr_format
};

const Format kFormats[] = {
    {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_TYPELESS, MVR_FORMAT_RGBA8_SRGB},
    {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_TYPELESS, MVR_FORMAT_BGRA8_SRGB},
    {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_TYPELESS, MVR_FORMAT_RGBA8_UNORM},
    {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_TYPELESS, MVR_FORMAT_BGRA8_UNORM},
    {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_TYPELESS, MVR_FORMAT_RGBA16_FLOAT},
    {DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_TYPELESS, MVR_FORMAT_RGB10A2_UNORM},
    {DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_TYPELESS, MVR_FORMAT_RGBA16_UNORM},
    {DXGI_FORMAT_B8G8R8X8_UNORM_SRGB, DXGI_FORMAT_B8G8R8X8_TYPELESS, MVR_FORMAT_BGRX8_SRGB},
    {DXGI_FORMAT_B8G8R8X8_UNORM, DXGI_FORMAT_B8G8R8X8_TYPELESS, MVR_FORMAT_BGRX8_UNORM},
};

// SteamVR asks for a typed or a typeless format; a typeless one is shown as its sRGB (or only) typed format, as
// SteamVR's compositor writes sRGB-encoded colour into it
const Format *findFormat(uint32_t dxgi) {
  for (const Format &f : kFormats)
    if (f.typed == static_cast<DXGI_FORMAT>(dxgi) || f.typeless == static_cast<DXGI_FORMAT>(dxgi))
      return &f;
  return nullptr;
}

// ---- math ----

mvr_quat quatFromMatrix(const vr::HmdMatrix34_t &m) {
  // the rotation part of a 3x4 row-major matrix
  const float m00 = m.m[0][0], m11 = m.m[1][1], m22 = m.m[2][2];
  mvr_quat q{};
  const float trace = m00 + m11 + m22;
  if (trace > 0) {
    const float s = 0.5f / std::sqrt(trace + 1.0f);
    q.w = 0.25f / s;
    q.x = (m.m[2][1] - m.m[1][2]) * s;
    q.y = (m.m[0][2] - m.m[2][0]) * s;
    q.z = (m.m[1][0] - m.m[0][1]) * s;
  } else if (m00 > m11 && m00 > m22) {
    const float s = 2.0f * std::sqrt(1.0f + m00 - m11 - m22);
    q.w = (m.m[2][1] - m.m[1][2]) / s;
    q.x = 0.25f * s;
    q.y = (m.m[0][1] + m.m[1][0]) / s;
    q.z = (m.m[0][2] + m.m[2][0]) / s;
  } else if (m11 > m22) {
    const float s = 2.0f * std::sqrt(1.0f + m11 - m00 - m22);
    q.w = (m.m[0][2] - m.m[2][0]) / s;
    q.x = (m.m[0][1] + m.m[1][0]) / s;
    q.y = 0.25f * s;
    q.z = (m.m[1][2] + m.m[2][1]) / s;
  } else {
    const float s = 2.0f * std::sqrt(1.0f + m22 - m00 - m11);
    q.w = (m.m[1][0] - m.m[0][1]) / s;
    q.x = (m.m[0][2] + m.m[2][0]) / s;
    q.y = (m.m[1][2] + m.m[2][1]) / s;
    q.z = 0.25f * s;
  }
  return q;
}

mvr_vec3 rotate(const mvr_quat &q, const mvr_vec3 &v) {
  // v + 2w(q×v) + 2 q×(q×v)
  const mvr_vec3 u{q.x, q.y, q.z};
  const mvr_vec3 t{2 * (u.y * v.z - u.z * v.y), 2 * (u.z * v.x - u.x * v.z), 2 * (u.x * v.y - u.y * v.x)};
  return {v.x + q.w * t.x + (u.y * t.z - u.z * t.y), v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
          v.z + q.w * t.z + (u.x * t.y - u.y * t.x)};
}

// ---- the connection to metalvrd ----

class Link {
public:
  bool open() {
    if (!connection_.connect())
      return false;
    mvr_hello_request hello{};
    hello.version = MVR_PROTOCOL_VERSION;
    hello.pid = currentProcessId();
    hello.graphics_api = MVR_GFX_D3D11;
    std::strncpy(hello.application_name, "SteamVR", sizeof(hello.application_name) - 1);
    mvr_hello_response response{};
    if (!connection_.call(MVR_MSG_HELLO, hello, response) || response.result != MVR_OK)
      return false;
    system_ = response.system;
    return connection_.syncClock();
  }

  Connection &connection() { return connection_; }
  const mvr_system_info &system() const { return system_; }

  // The headset's own fields of view and eye distance, which a streamed headset (ALVR) reports only once it
  // connects, usually after HELLO: until then the system info holds the model's defaults.
  void setViews(const mvr_fov &left, const mvr_fov &right, float ipd) {
    system_.fov[0] = left;
    system_.fov[1] = right;
    system_.ipd = ipd;
  }

private:
  Connection connection_;
  mvr_system_info system_{};
};

// ---- direct mode ----

class DirectMode final : public vr::IVRDriverDirectModeComponent {
public:
  explicit DirectMode(Link &link) : link_(link) {}

  ~DirectMode() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &entry : sets_)
      freeSet(*entry.second);
    sets_.clear();
    handles_.clear();
    release(drawn_);
    if (drawn_event_)
      CloseHandle(drawn_event_);
    release(context4_);
    release(context_);
    release(device_);
  }

  bool init() {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                         &device_, nullptr, &context_);
    if (FAILED(hr)) {
      log("metalvr: D3D11CreateDevice failed (0x%08lx)", static_cast<unsigned long>(hr));
      return false;
    }
    // metalvrd reads the eye images on its own GPU queue, which the sync texture's keyed mutex does not order: the
    // fence tells when this device's queue, and so the compositor's drawing it waits for, is done
    ID3D11Device5 *device5 = nullptr;
    if (SUCCEEDED(device_->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void **>(&device5))) &&
        SUCCEEDED(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, __uuidof(ID3D11Fence), reinterpret_cast<void **>(&drawn_))) &&
        SUCCEEDED(context_->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void **>(&context4_))))
      drawn_event_ = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    release(device5);
    if (!drawn_event_)
      log("metalvr: no Direct3D 11 fence; metalvrd may show eye images the compositor is still drawing");
    return true;
  }

  void CreateSwapTextureSet(uint32_t pid, const SwapTextureSetDesc_t *desc, SwapTextureSet_t *out) override {
    std::memset(out, 0, sizeof(*out));
    const Format *format = findFormat(desc->nFormat);
    if (!format) {
      log("metalvr: CreateSwapTextureSet: format %u is not one the headset can show", desc->nFormat);
      return;
    }
    auto set = std::make_unique<Set>();
    set->pid = pid;
    set->width = desc->nWidth;
    set->height = desc->nHeight;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = desc->nWidth;
    td.Height = desc->nHeight;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = static_cast<DXGI_FORMAT>(desc->nFormat);
    td.SampleDesc.Count = 1; // the compositor's eye images are single-sampled
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    mvr_swapchain_create_request request{};
    request.width = desc->nWidth;
    request.height = desc->nHeight;
    request.format = format->share;
    request.array_size = 1;
    request.share_kind = MVR_SHARE_MULLION_BOOTSTRAP;
    request.image_count = 3;

    std::lock_guard<std::mutex> lock(mutex_);
    for (int i = 0; i < 3; ++i) {
      HRESULT hr = device_->CreateTexture2D(&td, nullptr, &set->textures[i]);
      IDXGIResource *resource = nullptr;
      if (SUCCEEDED(hr))
        hr = set->textures[i]->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void **>(&resource));
      if (SUCCEEDED(hr))
        hr = resource->GetSharedHandle(&set->handles[i]);
      release(resource);
      if (FAILED(hr) || !shareName(set->handles[i], request.images[i].name)) {
        log("metalvr: CreateSwapTextureSet %ux%u format %u: shared texture %d failed (0x%08lx); MetalVR's SteamVR "
            "driver needs Mullion's Direct3D 11",
            desc->nWidth, desc->nHeight, desc->nFormat, i, static_cast<unsigned long>(hr));
        freeSet(*set);
        return;
      }
    }
    request.swapchain_id = set->swapchain_id = next_swapchain_id_++;
    mvr_result_response response{};
    if (!link_.connection().call(MVR_MSG_SWAPCHAIN_CREATE, request, response) || response.result != MVR_OK) {
      log("metalvr: metalvrd did not take the swapchain (%u)", response.result);
      freeSet(*set);
      return;
    }
    for (int i = 0; i < 3; ++i) {
      out->rSharedTextureHandles[i] = reinterpret_cast<vr::SharedTextureHandle_t>(set->handles[i]);
      handles_[out->rSharedTextureHandles[i]] = {set.get(), static_cast<uint32_t>(i)};
    }
    out->unTextureFlags = 0;
    log("metalvr: swap texture set %u: %ux%u, format %u, for process %u", set->swapchain_id, desc->nWidth,
        desc->nHeight, desc->nFormat, pid);
    sets_[set->swapchain_id] = std::move(set);
  }

  void DestroySwapTextureSet(vr::SharedTextureHandle_t handle) override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = handles_.find(handle);
    if (found != handles_.end())
      destroySet(found->second.first->swapchain_id);
  }

  void DestroyAllSwapTextureSets(uint32_t pid) override {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<uint32_t> ids;
    for (auto &entry : sets_)
      if (entry.second->pid == pid)
        ids.push_back(entry.first);
    for (uint32_t id : ids)
      destroySet(id);
  }

  void GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t handles[2], uint32_t (*indices)[2]) override {
    for (int eye = 0; eye < 2; ++eye)
      (*indices)[eye] = ((*indices)[eye] + 1) % 3;
  }

  void SubmitLayer(const SubmitLayerPerEye_t (&eyes)[2]) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (layers_.size() < MVR_MAX_LAYERS)
      layers_.push_back({eyes[0], eyes[1]});
  }

  void Present(vr::SharedTextureHandle_t sync_texture) override {
    std::vector<std::array<SubmitLayerPerEye_t, 2>> layers;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      layers.swap(layers_);
    }
    // the compositor's rendering into the eye textures is done once it lets go of the sync texture
    IDXGIKeyedMutex *keyed = syncMutex(sync_texture);
    const HRESULT acquire_hr = keyed ? keyed->AcquireSync(0, 100) : E_NOINTERFACE;
    const bool acquired = SUCCEEDED(acquire_hr) && acquire_hr != static_cast<HRESULT>(WAIT_TIMEOUT);
    if (acquire_hr != S_OK) {
      ++sync_failures_;
      if (GetTickCount64() - last_sync_log_ > 5000) {
        last_sync_log_ = GetTickCount64();
        log("metalvr: SteamVR's sync texture was not acquired (0x%08lx), %llu times so far",
            static_cast<unsigned long>(acquire_hr), sync_failures_);
      }
    }

    if (acquired)
      waitForDrawing();

    std::vector<mvr_layer> out;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const mvr_system_info &system = link_.system();
      for (size_t l = 0; l < layers.size(); ++l) {
        mvr_layer layer{};
        layer.type = MVR_LAYER_PROJECTION;
        layer.flags = l == 0 ? 0 : MVR_LAYER_BLEND_ALPHA;
        bool ok = true;
        for (int eye = 0; eye < 2 && ok; ++eye) {
          const SubmitLayerPerEye_t &in = layers[l][eye];
          auto found = handles_.find(in.hTexture);
          if (found == handles_.end()) {
            ok = false;
            break;
          }
          const Set &set = *found->second.first;
          mvr_layer_view &view = layer.views[eye];
          view.swapchain_id = set.swapchain_id;
          view.image_index = found->second.second;
          view.array_index = 0;
          // the bounds may run backwards to flip the image: the protocol's negative extents say the same
          const float w = static_cast<float>(set.width), h = static_cast<float>(set.height);
          view.rect.x = static_cast<int32_t>(std::lround(in.bounds.uMin * w));
          view.rect.y = static_cast<int32_t>(std::lround(in.bounds.vMin * h));
          view.rect.width = static_cast<int32_t>(std::lround((in.bounds.uMax - in.bounds.uMin) * w));
          view.rect.height = static_cast<int32_t>(std::lround((in.bounds.vMax - in.bounds.vMin) * h));
          if (!view.rect.width || !view.rect.height) {
            view.rect = {0, 0, static_cast<int32_t>(set.width), static_cast<int32_t>(set.height)};
          }
          // the head pose the frame was rendered with, and the eye beside it
          mvr_pose head{};
          head.orientation = quatFromMatrix(in.mHmdPose);
          head.position = {in.mHmdPose.m[0][3], in.mHmdPose.m[1][3], in.mHmdPose.m[2][3]};
          const mvr_vec3 offset = rotate(head.orientation, {(eye == 0 ? -0.5f : 0.5f) * system.ipd, 0, 0});
          view.pose.orientation = head.orientation;
          view.pose.position = {head.position.x + offset.x, head.position.y + offset.y, head.position.z + offset.z};
          view.fov = system.fov[eye];
        }
        if (ok)
          out.push_back(layer);
      }
    }
    if (keyed) {
      if (acquired)
        keyed->ReleaseSync(0);
      keyed->Release();
    }
    if (out.empty())
      return;
    beginSession();
    mvr_frame_end_request request{};
    request.frame_id = ++frame_id_;
    request.display_time_ns = localNowNs() + link_.connection().clockOffsetNs();
    request.layer_count = static_cast<uint32_t>(out.size());
    mvr_result_response response{};
    if (!link_.connection().call(MVR_MSG_FRAME_END, &request, sizeof(request), &response, sizeof(response),
                                 out.data(), out.size() * sizeof(mvr_layer)) ||
        response.result != MVR_OK) {
      if (!warned_frame_) {
        log("metalvr: metalvrd did not take a frame (%u)", response.result);
        warned_frame_ = true;
      }
      session_ = false;
    }
    ++presented_;
  }

  void PostPresent() override {
    // the compositor's next frame starts at the headset's next refresh, as metalvrd paces it
    mvr_frame_wait_response wait{};
    if (link_.connection().call(MVR_MSG_FRAME_WAIT, wait) && wait.result == MVR_OK)
      sleepUntilLocalNs(wait.wake_time_ns - link_.connection().clockOffsetNs());
  }

  void GetFrameTiming(vr::DriverDirectMode_FrameTiming *timing) override {
    if (!timing || timing->m_nSize < sizeof(vr::DriverDirectMode_FrameTiming))
      return;
    timing->m_nNumFramePresents = 1;
    timing->m_nNumMisPresented = 0;
    timing->m_nNumDroppedFrames = 0;
  }

  void endSession() {
    if (!session_)
      return;
    mvr_result_response response{};
    link_.connection().call(MVR_MSG_SESSION_END, response);
    session_ = false;
  }

private:
  struct Set {
    uint32_t pid = 0;
    uint32_t swapchain_id = 0;
    uint32_t width = 0, height = 0;
    ID3D11Texture2D *textures[3] = {};
    HANDLE handles[3] = {};
  };

  // the bootstrap name of a texture's Metal port: a legacy handle is the share's id with a marker bit, and the share
  // keeps a file mapping named after the id that holds the name (Mullion's mullion_shared_texture.hpp)
  static bool shareName(HANDLE legacy, char out[MVR_SHARE_NAME_LEN]) {
    constexpr uint64_t marker = uint64_t(1) << 30;
    const uint64_t value = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(legacy));
    if (!(value & marker))
      return false;
    char name[64];
    std::snprintf(name, sizeof(name), "MULLION_shared_texture_%016llx",
                  static_cast<unsigned long long>(value & ~marker));
    HANDLE mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, name);
    return mapping && mullionShareNameFromHandle(mapping, out);
  }

  IDXGIKeyedMutex *syncMutex(vr::SharedTextureHandle_t handle) {
    if (!handle)
      return nullptr;
    auto cached = sync_.find(handle);
    if (cached == sync_.end()) {
      ID3D11Texture2D *texture = nullptr;
      const HRESULT hr = device_->OpenSharedResource(reinterpret_cast<HANDLE>(handle), __uuidof(ID3D11Texture2D),
                                                     reinterpret_cast<void **>(&texture));
      if (FAILED(hr))
        texture = nullptr;
      D3D11_TEXTURE2D_DESC td{};
      if (texture)
        texture->GetDesc(&td);
      log("metalvr: sync texture %p: open 0x%08lx, %ux%u format %u misc 0x%x", reinterpret_cast<void *>(handle),
          static_cast<unsigned long>(hr), td.Width, td.Height, td.Format, td.MiscFlags);
      cached = sync_.emplace(handle, texture).first;
    }
    IDXGIKeyedMutex *keyed = nullptr;
    if (cached->second &&
        FAILED(cached->second->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void **>(&keyed))))
      keyed = nullptr;
    return keyed;
  }

  // The acquire above only makes this device's queue wait for the compositor's release; metalvrd's queue does not.
  // A fence signalled behind that wait is reached once the compositor's drawing into the eye images is done, and
  // only then are they handed over: read earlier, they show blocks and lines of the half-drawn frame.
  void waitForDrawing() {
    if (!drawn_event_)
      return;
    const UINT64 value = ++drawn_value_;
    if (FAILED(context4_->Signal(drawn_, value)) || FAILED(drawn_->SetEventOnCompletion(value, drawn_event_)))
      return;
    if (WaitForSingleObject(drawn_event_, 100) != WAIT_OBJECT_0 && GetTickCount64() - last_drawn_log_ > 5000) {
      last_drawn_log_ = GetTickCount64();
      log("metalvr: the compositor's drawing was not done after 100 ms; the frame is shown anyway");
    }
  }

  void beginSession() {
    if (session_)
      return;
    mvr_result_response response{};
    if (link_.connection().call(MVR_MSG_SESSION_BEGIN, response) && response.result == MVR_OK) {
      session_ = true;
      warned_frame_ = false;
    } else if (!warned_session_) {
      log("metalvr: metalvrd's session is held by another application (%u)", response.result);
      warned_session_ = true;
    }
  }

  // mutex_ held
  void destroySet(uint32_t id) {
    auto found = sets_.find(id);
    if (found == sets_.end())
      return;
    Set &set = *found->second;
    for (HANDLE h : set.handles)
      handles_.erase(reinterpret_cast<vr::SharedTextureHandle_t>(h));
    mvr_swapchain_destroy_request request{id};
    mvr_result_response response{};
    link_.connection().call(MVR_MSG_SWAPCHAIN_DESTROY, request, response);
    freeSet(set);
    sets_.erase(found);
  }

  static void freeSet(Set &set) {
    for (auto *&texture : set.textures)
      release(texture);
  }

  Link &link_;
  std::mutex mutex_;
  ID3D11Device *device_ = nullptr;
  ID3D11DeviceContext *context_ = nullptr;
  ID3D11DeviceContext4 *context4_ = nullptr;
  ID3D11Fence *drawn_ = nullptr;
  HANDLE drawn_event_ = nullptr;
  UINT64 drawn_value_ = 0;
  ULONGLONG last_drawn_log_ = 0;
  std::map<uint32_t, std::unique_ptr<Set>> sets_;
  std::map<vr::SharedTextureHandle_t, std::pair<Set *, uint32_t>> handles_;
  std::map<vr::SharedTextureHandle_t, ID3D11Texture2D *> sync_;
  std::vector<std::array<SubmitLayerPerEye_t, 2>> layers_;
  unsigned long long sync_failures_ = 0;
  ULONGLONG last_sync_log_ = 0;
  uint32_t next_swapchain_id_ = 1;
  uint64_t frame_id_ = 0;
  bool session_ = false;
  bool warned_frame_ = false, warned_session_ = false;
  std::atomic<uint64_t> presented_{0};
};

// ---- the two interfaces whose methods return structures ----
//
// SteamVR is built with MSVC, this driver with MinGW. They return a structure from a member function differently: MSVC
// passes `this` first and the caller's buffer second, MinGW's C++ ABI the buffer first. ITrackedDeviceServerDriver::GetPose
// and IVRDisplayComponent::ComputeDistortion are the only such methods here, so these two interfaces are declared again
// with the same virtual functions in the same order, those two written as MSVC calls them: the buffer an explicit
// parameter after `this`, and returned. A class implementing one is handed to SteamVR cast to the real interface.

struct TrackedDevice {
  virtual vr::EVRInitError Activate(uint32_t id) = 0;
  virtual void Deactivate() = 0;
  virtual void EnterStandby() = 0;
  virtual void *GetComponent(const char *name) = 0;
  virtual void DebugRequest(const char *request, char *response, uint32_t size) = 0;
  virtual vr::DriverPose_t *GetPose(vr::DriverPose_t *out) = 0; // DriverPose_t GetPose()

  vr::ITrackedDeviceServerDriver *asDriver() { return reinterpret_cast<vr::ITrackedDeviceServerDriver *>(this); }
};

struct DisplayComponent {
  virtual void GetWindowBounds(int32_t *x, int32_t *y, uint32_t *w, uint32_t *h) = 0;
  virtual bool IsDisplayOnDesktop() = 0;
  virtual bool IsDisplayRealDisplay() = 0;
  virtual void GetRecommendedRenderTargetSize(uint32_t *w, uint32_t *h) = 0;
  virtual void GetEyeOutputViewport(vr::EVREye eye, uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h) = 0;
  virtual void GetProjectionRaw(vr::EVREye eye, float *left, float *right, float *top, float *bottom) = 0;
  // DistortionCoordinates_t ComputeDistortion(EVREye, float, float)
  virtual vr::DistortionCoordinates_t *ComputeDistortion(vr::DistortionCoordinates_t *out, vr::EVREye eye, float u,
                                                         float v) = 0;
};

// ---- the headset ----

class Display final : public DisplayComponent {
public:
  explicit Display(const mvr_system_info &system) : system_(system) {}

  void GetWindowBounds(int32_t *x, int32_t *y, uint32_t *w, uint32_t *h) override {
    *x = 0;
    *y = 0;
    *w = system_.eye_width * 2;
    *h = system_.eye_height;
  }
  bool IsDisplayOnDesktop() override { return false; }
  bool IsDisplayRealDisplay() override { return false; }
  void GetRecommendedRenderTargetSize(uint32_t *w, uint32_t *h) override {
    // smaller with MetalFX spatial upscaling, which metalvrd undoes (runtime/upscale.h)
    *w = upscaledRenderSize(system_.eye_width);
    *h = upscaledRenderSize(system_.eye_height);
  }
  void GetEyeOutputViewport(vr::EVREye eye, uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h) override {
    *x = eye == vr::Eye_Left ? 0 : system_.eye_width;
    *y = 0;
    *w = system_.eye_width;
    *h = system_.eye_height;
  }
  void GetProjectionRaw(vr::EVREye eye, float *left, float *right, float *top, float *bottom) override {
    // OpenVR's vertical tangents run downwards: top is the tangent of the angle below the axis
    const mvr_fov &fov = system_.fov[eye == vr::Eye_Left ? 0 : 1];
    *left = std::tan(fov.left);
    *right = std::tan(fov.right);
    *top = std::tan(fov.down);
    *bottom = std::tan(fov.up);
  }
  vr::DistortionCoordinates_t *ComputeDistortion(vr::DistortionCoordinates_t *out, vr::EVREye, float u,
                                                 float v) override {
    // metalvrd corrects for the lenses itself
    *out = {{u, v}, {u, v}, {u, v}};
    return out;
  }

private:
  const mvr_system_info &system_;
};


// ---- the Touch controllers ----

mvr_quat quatConjugate(const mvr_quat &q) { return {-q.x, -q.y, -q.z, q.w}; }

mvr_quat quatMultiply(const mvr_quat &a, const mvr_quat &b) {
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// What SteamVR is told the controllers are: its own profile and models for the controllers in the user's hands,
// so games' bindings for them apply as they are.
struct ControllerLook {
  const char *profile;
  const char *type;
  const char *render_model[2]; // left, right
  const char *model_number[2];
  const char *manufacturer;
};

ControllerLook lookFor(uint32_t headset, uint32_t kind) {
  switch (kind) {
  case MVR_CONTROLLERS_INDEX:
    return {"{indexcontroller}/input/index_controller_profile.json",
            "knuckles",
            {"{indexcontroller}valve_controller_knu_1_0_left", "{indexcontroller}valve_controller_knu_1_0_right"},
            {"Knuckles (Left Controller)", "Knuckles (Right Controller)"},
            "Valve"};
  case MVR_CONTROLLERS_VIVE_WAND:
    return {"{htc}/input/vive_controller_profile.json",
            "vive_controller",
            {"vr_controller_vive_1_5", "vr_controller_vive_1_5"},
            {"Vive. Controller MV", "Vive. Controller MV"},
            "HTC"};
  case MVR_CONTROLLERS_TOUCH_PLUS:
    if (headset == MVR_HEADSET_QUEST_PRO)
      return {"{oculus}/input/touch_profile.json",
              "oculus_touch",
              {"oculus_quest_pro_controller_left", "oculus_quest_pro_controller_right"},
              {"Meta Quest Pro (Left Controller)", "Meta Quest Pro (Right Controller)"},
              "Meta"};
    return {"{oculus}/input/touch_profile.json",
            "oculus_touch",
            {"oculus_quest_plus_controller_left", "oculus_quest_plus_controller_right"},
            {"Meta Quest 3 (Left Controller)", "Meta Quest 3 (Right Controller)"},
            "Meta"};
  case MVR_CONTROLLERS_PS_SENSE:
    // Sony's SteamVR driver's names are its own: shown as Touch controllers, whose bindings games have, with the
    // Sense's face buttons on Touch's A, B, X and Y
    return {"{oculus}/input/touch_profile.json",
            "oculus_touch",
            {"oculus_quest2_controller_left", "oculus_quest2_controller_right"},
            {"PlayStation VR2 Sense (Left Controller)", "PlayStation VR2 Sense (Right Controller)"},
            "Sony"};
  case MVR_CONTROLLERS_STEAM_FRAME:
    // SteamVR's own names for these are not known yet (docs/HEADSETS.md): shown as ringless Touch controllers,
    // whose bindings games have, with the Frame's extra buttons on Touch's nearest inputs
    return {"{oculus}/input/touch_profile.json",
            "oculus_touch",
            {"oculus_quest_plus_controller_left", "oculus_quest_plus_controller_right"},
            {"Steam Frame (Left Controller)", "Steam Frame (Right Controller)"},
            "Valve"};
  default:
    if (headset == MVR_HEADSET_RIFT_S || headset == MVR_HEADSET_QUEST_1)
      return {"{oculus}/input/touch_profile.json",
              "oculus_touch",
              {"oculus_rifts_controller_left", "oculus_rifts_controller_right"},
              {"Oculus Rift S (Left Controller)", "Oculus Rift S (Right Controller)"},
              "Oculus"};
    if (headset == MVR_HEADSET_QUEST_2)
      return {"{oculus}/input/touch_profile.json",
              "oculus_touch",
              {"oculus_quest2_controller_left", "oculus_quest2_controller_right"},
              {"Miramar (Left Controller)", "Miramar (Right Controller)"},
              "Oculus"};
    return {"{oculus}/input/touch_profile.json",
            "oculus_touch",
            {"oculus_cv1_controller_left", "oculus_cv1_controller_right"},
            {"Oculus Rift CV1 (Left Controller)", "Oculus Rift CV1 (Right Controller)"},
            "Oculus"};
  }
}

// whether a headset's tracking can drift in yaw: one tracked by its rotation alone (MetalVR's Rift S, a Rift CV1
// without sensors, a Lighthouse headset without base stations) can; inside-out tracking cannot
bool driftsInYaw(uint32_t headset) {
  switch (headset) {
  case MVR_HEADSET_QUEST_1:
  case MVR_HEADSET_QUEST_2:
  case MVR_HEADSET_QUEST_3:
  case MVR_HEADSET_QUEST_3S:
  case MVR_HEADSET_QUEST_PRO:
  case MVR_HEADSET_STEAM_FRAME:
  case MVR_HEADSET_PSVR2:
    return false;
  default:
    return true;
  }
}

class Controller final : public TrackedDevice {
public:
  Controller(Link &link, uint32_t hand) : link_(link), hand_(hand) {}

  vr::EVRInitError Activate(uint32_t id) override {
    auto *p = vr::VRProperties();
    container_ = p->TrackedDeviceToPropertyContainer(id);
    const bool left = hand_ == 0;
    const mvr_system_info &system = link_.system();
    headset_ = system.headset;
    kind_ = system.controllers;
    const ControllerLook look = lookFor(headset_, kind_);
    p->SetInt32Property(container_, vr::Prop_ControllerRoleHint_Int32,
                        left ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
    p->SetStringProperty(container_, vr::Prop_InputProfilePath_String, look.profile);
    p->SetStringProperty(container_, vr::Prop_ControllerType_String, look.type);
    p->SetStringProperty(container_, vr::Prop_RenderModelName_String, look.render_model[hand_]);
    p->SetStringProperty(container_, vr::Prop_ModelNumber_String, look.model_number[hand_]);
    p->SetStringProperty(container_, vr::Prop_ManufacturerName_String, look.manufacturer);
    p->SetStringProperty(container_, vr::Prop_TrackingSystemName_String, "metalvr");
    p->SetStringProperty(container_, vr::Prop_SerialNumber_String, left ? "MetalVR-Touch-Left" : "MetalVR-Touch-Right");
    p->SetStringProperty(container_, vr::Prop_RegisteredDeviceType_String,
                         left ? "metalvr/MetalVR-Touch-Left" : "metalvr/MetalVR-Touch-Right");
    p->SetBoolProperty(container_, vr::Prop_DeviceProvidesBatteryStatus_Bool, false);
    log("metalvr: %s controller: %s (%s)", left ? "left" : "right", look.model_number[hand_], look.type);

    auto *in = vr::VRDriverInput();
    const auto boolean = [&](const char *path) {
      vr::VRInputComponentHandle_t handle = 0;
      in->CreateBooleanComponent(container_, path, &handle);
      return handle;
    };
    const auto scalar = [&](const char *path, bool two_sided) {
      vr::VRInputComponentHandle_t handle = 0;
      in->CreateScalarComponent(container_, path, &handle, vr::VRScalarType_Absolute,
                                two_sided ? vr::VRScalarUnits_NormalizedTwoSided : vr::VRScalarUnits_NormalizedOneSided);
      return handle;
    };
    if (kind_ == MVR_CONTROLLERS_VIVE_WAND) {
      // the paths of SteamVR's Vive controller profile
      c_.system_click = boolean("/input/system/click");
      c_.menu_click = boolean("/input/application_menu/click");
      c_.grip_click = boolean("/input/grip/click");
      c_.trigger_click = boolean("/input/trigger/click");
      c_.trigger = scalar("/input/trigger/value", false);
      c_.pad_x = scalar("/input/trackpad/x", true);
      c_.pad_y = scalar("/input/trackpad/y", true);
      c_.pad_click = boolean("/input/trackpad/click");
      c_.pad_touch = boolean("/input/trackpad/touch");
    } else if (kind_ == MVR_CONTROLLERS_INDEX) {
      // the paths of SteamVR's Index controller profile
      c_.primary_click = boolean("/input/a/click");
      c_.primary_touch = boolean("/input/a/touch");
      c_.secondary_click = boolean("/input/b/click");
      c_.secondary_touch = boolean("/input/b/touch");
      c_.system_click = boolean("/input/system/click");
      c_.system_touch = boolean("/input/system/touch");
      c_.trigger_click = boolean("/input/trigger/click");
      c_.trigger = scalar("/input/trigger/value", false);
      c_.trigger_touch = boolean("/input/trigger/touch");
      c_.pad_x = scalar("/input/trackpad/x", true);
      c_.pad_y = scalar("/input/trackpad/y", true);
      c_.pad_touch = boolean("/input/trackpad/touch");
      c_.pad_force = scalar("/input/trackpad/force", false);
      c_.grip = scalar("/input/grip/value", false);
      c_.grip_force = scalar("/input/grip/force", false);
      c_.grip_touch = boolean("/input/grip/touch");
      c_.stick_x = scalar("/input/thumbstick/x", true);
      c_.stick_y = scalar("/input/thumbstick/y", true);
      c_.stick_click = boolean("/input/thumbstick/click");
      c_.stick_touch = boolean("/input/thumbstick/touch");
      for (int finger = 0; finger < 4; ++finger) {
        static const char *const paths[4] = {"/input/finger/index", "/input/finger/middle", "/input/finger/ring",
                                             "/input/finger/pinky"};
        c_.finger[finger] = scalar(paths[finger], false);
      }
      in->CreateSkeletonComponent(container_, left ? "/input/skeleton/left" : "/input/skeleton/right",
                                  left ? "/skeleton/hand/left" : "/skeleton/hand/right", "/pose/raw",
                                  vr::VRSkeletalTracking_Partial, nullptr, 0, &skeleton_);
    } else {
      // the Touch family's paths, which the Steam Frame's controllers are shown with too
      const char *first = left ? "/input/x" : "/input/a";
      const char *second = left ? "/input/y" : "/input/b";
      char path[64];
      std::snprintf(path, sizeof(path), "%s/click", first);
      c_.primary_click = boolean(path);
      std::snprintf(path, sizeof(path), "%s/touch", first);
      c_.primary_touch = boolean(path);
      std::snprintf(path, sizeof(path), "%s/click", second);
      c_.secondary_click = boolean(path);
      std::snprintf(path, sizeof(path), "%s/touch", second);
      c_.secondary_touch = boolean(path);
      c_.system_click = boolean("/input/system/click");
      c_.system_touch = boolean("/input/system/touch");
      c_.stick_click = boolean("/input/joystick/click");
      c_.stick_touch = boolean("/input/joystick/touch");
      c_.stick_x = scalar("/input/joystick/x", true);
      c_.stick_y = scalar("/input/joystick/y", true);
      c_.trigger = scalar("/input/trigger/value", false);
      c_.trigger_touch = boolean("/input/trigger/touch");
      c_.grip = scalar("/input/grip/value", false);
      c_.grip_touch = boolean("/input/grip/touch");
      c_.thumbrest_touch = boolean("/input/thumbrest/touch");
    }
    in->CreateHapticComponent(container_, "/output/haptic", &haptic_);
    // only now may the pose thread update it: its inputs exist
    id_ = id;
    return vr::VRInitError_None;
  }

  void Deactivate() override { id_ = vr::k_unTrackedDeviceIndexInvalid; }
  void EnterStandby() override {}
  void *GetComponent(const char *) override { return nullptr; }
  void DebugRequest(const char *, char *response, uint32_t size) override {
    if (size)
      response[0] = 0;
  }
  vr::DriverPose_t *GetPose(vr::DriverPose_t *out) override {
    std::lock_guard<std::mutex> lock(mutex_);
    *out = pose_;
    return out;
  }

  // one tracking sample: the pose, then the inputs
  void update(const mvr_tracking_response &sample, double pose_time_offset) {
    if (id_ == vr::k_unTrackedDeviceIndexInvalid)
      return;
    const mvr_controller_state &c = sample.controllers[hand_];
    vr::DriverPose_t pose{};
    pose.qWorldFromDriverRotation.w = 1;
    pose.qDriverFromHeadRotation.w = 1;
    pose.deviceIsConnected = (c.flags & MVR_CONTROLLER_CONNECTED) != 0;
    pose.poseIsValid = (c.flags & MVR_CONTROLLER_POSE_VALID) != 0;
    pose.result = pose.poseIsValid ? vr::TrackingResult_Running_OK : vr::TrackingResult_Running_OutOfRange;
    pose.willDriftInYaw = driftsInYaw(headset_);
    if (pose.poseIsValid) {
      mvr_quat q = c.grip.orientation;
      mvr_vec3 offset{0, 0, 0};
      if (kind_ != MVR_CONTROLLERS_INDEX && kind_ != MVR_CONTROLLERS_VIVE_WAND) {
        // the controller's own frame, which SteamVR's Touch models place their grip and tip from: the grip pose
        // without its offset. SteamVR's render models for every Touch controller (CV1, Rift S, Quest 2, Touch Plus,
        // Touch Pro) put their openxr_grip component 20.6 degrees about x from that frame, at +-0.007, -0.0018, 0.102
        // (x mirrored for the right hand), and SteamVR's OpenXR rebuilds the grip from it, so taking the same
        // transform off here gives an application the grip the headset reported. (60 degrees, used before, is the
        // angle between the grip and the aim: openxr_aim is at -39.4.) Lighthouse controllers report their own frame
        // as the grip already.
        const mvr_quat grip_rotation{0.17880222f, 0, 0, 0.98388504f}; // 20.6 degrees about x
        q = quatMultiply(c.grip.orientation, quatConjugate(grip_rotation));
        offset = rotate(q, {hand_ == 0 ? 0.007f : -0.007f, -0.00182941f, 0.1019482f});
      }
      pose.qRotation = {q.w, q.x, q.y, q.z};
      pose.vecPosition[0] = c.grip.position.x - offset.x;
      pose.vecPosition[1] = c.grip.position.y - offset.y;
      pose.vecPosition[2] = c.grip.position.z - offset.z;
      pose.vecAngularVelocity[0] = c.angular_velocity.x;
      pose.vecAngularVelocity[1] = c.angular_velocity.y;
      pose.vecAngularVelocity[2] = c.angular_velocity.z;
      pose.poseTimeOffset = pose_time_offset;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      pose_ = pose;
    }
    vr::VRServerDriverHost()->TrackedDevicePoseUpdated(id_, pose, sizeof(pose));
    if (!pose.deviceIsConnected)
      return;
    updateBattery(c);
    auto *in = vr::VRDriverInput();
    const auto set = [&](vr::VRInputComponentHandle_t handle, bool value) {
      if (handle)
        in->UpdateBooleanComponent(handle, value, 0);
    };
    const auto value = [&](vr::VRInputComponentHandle_t handle, float v) {
      if (handle)
        in->UpdateScalarComponent(handle, v, 0);
    };
    const bool frame = kind_ == MVR_CONTROLLERS_STEAM_FRAME;
    // Steam Frame on Touch's paths: its right hand's X and Y as A and B, its left hand's D-pad up and down as X
    // and Y, the bumper as the grip's click
    const uint32_t primary = MVR_BUTTON_PRIMARY | (frame ? uint32_t{MVR_BUTTON_TERTIARY | MVR_BUTTON_DPAD_UP} : 0u);
    const uint32_t secondary =
        MVR_BUTTON_SECONDARY | (frame ? uint32_t{MVR_BUTTON_QUATERNARY | MVR_BUTTON_DPAD_DOWN} : 0u);
    set(c_.primary_click, c.buttons & primary);
    set(c_.primary_touch, c.touches & primary);
    set(c_.secondary_click, c.buttons & secondary);
    set(c_.secondary_touch, c.touches & secondary);
    const uint32_t system = static_cast<uint32_t>(MVR_BUTTON_SYSTEM) |
                            (kind_ == MVR_CONTROLLERS_VIVE_WAND ? 0u : static_cast<uint32_t>(MVR_BUTTON_MENU)) |
                            (frame ? static_cast<uint32_t>(MVR_BUTTON_VIEW) : 0u);
    set(c_.system_click, c.buttons & system);
    set(c_.system_touch, c.buttons & system);
    set(c_.menu_click, c.buttons & MVR_BUTTON_MENU);
    set(c_.stick_click, c.buttons & MVR_BUTTON_THUMBSTICK);
    set(c_.stick_touch, c.touches & MVR_BUTTON_THUMBSTICK);
    value(c_.stick_x, c.thumbstick_x);
    value(c_.stick_y, c.thumbstick_y);
    set(c_.trigger_click, (c.buttons & MVR_BUTTON_TRIGGER) || c.trigger > 0.95f);
    value(c_.trigger, c.trigger);
    set(c_.trigger_touch, (c.touches & MVR_BUTTON_TRIGGER) || c.trigger > 0.05f);
    const float squeeze = frame && (c.buttons & MVR_BUTTON_BUMPER) ? 1.0f : c.squeeze;
    value(c_.grip, squeeze);
    value(c_.grip_force, c.squeeze_force);
    set(c_.grip_touch, squeeze > 0.05f || (c.touches & MVR_BUTTON_SQUEEZE));
    set(c_.grip_click, (c.buttons & MVR_BUTTON_SQUEEZE) || c.squeeze > 0.7f);
    set(c_.thumbrest_touch, c.touches & MVR_BUTTON_THUMBREST);
    value(c_.pad_x, c.trackpad_x);
    value(c_.pad_y, c.trackpad_y);
    set(c_.pad_click, c.buttons & MVR_BUTTON_TRACKPAD);
    set(c_.pad_touch, c.touches & MVR_BUTTON_TRACKPAD);
    value(c_.pad_force, c.trackpad_force);
    if (kind_ == MVR_CONTROLLERS_INDEX)
      updateFingers(c);
  }

  // SteamVR's vibration event for this controller, to metalvrd
  bool vibrate(const vr::VREvent_HapticVibration_t &v) {
    if (v.componentHandle != haptic_)
      return false;
    mvr_haptic_request request{hand_, v.fDurationSeconds, v.fFrequency, v.fAmplitude};
    mvr_result_response response{};
    link_.connection().call(MVR_MSG_HAPTIC, request, response);
    return true;
  }

private:
  // the finger curls, as SteamVR's finger inputs and as the hand skeleton
  void updateFingers(const mvr_controller_state &c) {
    float curl[5];
    const bool measured = (c.flags & MVR_CONTROLLER_FINGERS_VALID) != 0;
    // without measured fingers: the index finger follows the trigger, the others the grip
    curl[1] = measured ? c.fingers[MVR_FINGER_INDEX] : c.trigger;
    curl[2] = measured ? c.fingers[MVR_FINGER_MIDDLE] : c.squeeze;
    curl[3] = measured ? c.fingers[MVR_FINGER_RING] : c.squeeze;
    curl[4] = measured ? c.fingers[MVR_FINGER_PINKY] : c.squeeze;
    curl[0] = (c.touches & (MVR_BUTTON_PRIMARY | MVR_BUTTON_SECONDARY | MVR_BUTTON_THUMBSTICK | MVR_BUTTON_TRACKPAD))
                  ? 1.0f
                  : 0.0f;
    auto *in = vr::VRDriverInput();
    for (int finger = 0; finger < 4; ++finger)
      if (c_.finger[finger])
        in->UpdateScalarComponent(c_.finger[finger], std::clamp(curl[finger + 1], 0.0f, 1.0f), 0);
    if (!skeleton_)
      return;
    vr::VRBoneTransform_t bones[skeleton::kBoneCount];
    const skeleton::Bone *open = hand_ == 0 ? skeleton::kLeftOpen : skeleton::kRightOpen;
    const skeleton::Bone *closed = hand_ == 0 ? skeleton::kLeftClosed : skeleton::kRightClosed;
    for (int bone = 0; bone < skeleton::kBoneCount; ++bone) {
      // which finger a bone belongs to: 0 thumb .. 4 pinky; the root and wrist to none
      int finger = -1;
      if (bone >= 2 && bone <= 25)
        finger = bone < 6 ? 0 : 1 + (bone - 6) / 5;
      else if (bone >= 26)
        finger = bone - 26;
      const float t = finger < 0 ? 0.0f : std::clamp(curl[finger], 0.0f, 1.0f);
      bones[bone] = blendBone(open[bone], closed[bone], t);
    }
    in->UpdateSkeletonComponent(skeleton_, vr::VRSkeletalMotionRange_WithController, bones, skeleton::kBoneCount);
    in->UpdateSkeletonComponent(skeleton_, vr::VRSkeletalMotionRange_WithoutController, bones,
                                skeleton::kBoneCount);
  }

  static vr::VRBoneTransform_t blendBone(const skeleton::Bone &a, const skeleton::Bone &b, float t) {
    vr::VRBoneTransform_t out{};
    for (int i = 0; i < 3; ++i)
      out.position.v[i] = a.position[i] + (b.position[i] - a.position[i]) * t;
    out.position.v[3] = 1;
    // normalized lerp along the shorter way: close enough to a slerp between two poses of one finger
    float bq[4] = {b.orientation[0], b.orientation[1], b.orientation[2], b.orientation[3]};
    const float dot = a.orientation[0] * bq[0] + a.orientation[1] * bq[1] + a.orientation[2] * bq[2] +
                      a.orientation[3] * bq[3];
    if (dot < 0)
      for (float &v : bq)
        v = -v;
    float q[4];
    float length = 0;
    for (int i = 0; i < 4; ++i) {
      q[i] = a.orientation[i] + (bq[i] - a.orientation[i]) * t;
      length += q[i] * q[i];
    }
    length = length > 0 ? std::sqrt(length) : 1.0f;
    out.orientation = {q[0] / length, q[1] / length, q[2] / length, q[3] / length};
    return out;
  }

  // the controller's charge, when the headset reports it, a few times a minute
  void updateBattery(const mvr_controller_state &c) {
    const bool valid = (c.flags & MVR_CONTROLLER_BATTERY_VALID) != 0;
    const ULONGLONG now = GetTickCount64();
    if (valid == battery_valid_ && now - battery_set_ < 10000)
      return;
    battery_set_ = now;
    battery_valid_ = valid;
    auto *p = vr::VRProperties();
    p->SetBoolProperty(container_, vr::Prop_DeviceProvidesBatteryStatus_Bool, valid);
    if (!valid)
      return;
    p->SetFloatProperty(container_, vr::Prop_DeviceBatteryPercentage_Float, std::clamp(c.battery, 0.0f, 1.0f));
    p->SetBoolProperty(container_, vr::Prop_DeviceIsCharging_Bool, (c.flags & MVR_CONTROLLER_CHARGING) != 0);
  }

  struct Components {
    vr::VRInputComponentHandle_t primary_click = 0, primary_touch = 0, secondary_click = 0, secondary_touch = 0;
    vr::VRInputComponentHandle_t system_click = 0, system_touch = 0, menu_click = 0;
    vr::VRInputComponentHandle_t stick_click = 0, stick_touch = 0, stick_x = 0, stick_y = 0;
    vr::VRInputComponentHandle_t trigger = 0, trigger_click = 0, trigger_touch = 0;
    vr::VRInputComponentHandle_t grip = 0, grip_force = 0, grip_touch = 0, grip_click = 0, thumbrest_touch = 0;
    vr::VRInputComponentHandle_t pad_x = 0, pad_y = 0, pad_click = 0, pad_touch = 0, pad_force = 0;
    vr::VRInputComponentHandle_t finger[4] = {};
  };

  Link &link_;
  const uint32_t hand_;
  uint32_t headset_ = MVR_HEADSET_UNKNOWN;
  uint32_t kind_ = MVR_CONTROLLERS_UNKNOWN;
  std::atomic<uint32_t> id_{vr::k_unTrackedDeviceIndexInvalid};
  vr::PropertyContainerHandle_t container_ = 0;
  std::mutex mutex_;
  vr::DriverPose_t pose_{};
  Components c_;
  vr::VRInputComponentHandle_t skeleton_ = 0, haptic_ = 0;
  bool battery_valid_ = false;
  ULONGLONG battery_set_ = 0;
};

// ---- the tracking system's sensors ----

// A sensor that stands in the room (a Rift sensor): SteamVR shows it as a base station where metalvrd says it is.
class Reference final : public TrackedDevice {
public:
  // a Lighthouse headset's base station, or a Rift's sensor
  Reference(const char *serial, bool lighthouse) : serial_(serial), lighthouse_(lighthouse) {}
  const std::string &serial() const { return serial_; }

  vr::EVRInitError Activate(uint32_t id) override {
    auto *p = vr::VRProperties();
    const vr::PropertyContainerHandle_t c = p->TrackedDeviceToPropertyContainer(id);
    p->SetStringProperty(c, vr::Prop_ModelNumber_String, lighthouse_ ? "Valve SR Imp" : "Oculus Rift CV1 Sensor");
    p->SetStringProperty(c, vr::Prop_ManufacturerName_String, lighthouse_ ? "Valve" : "Oculus");
    p->SetStringProperty(c, vr::Prop_TrackingSystemName_String, "metalvr");
    p->SetStringProperty(c, vr::Prop_SerialNumber_String, serial_.c_str());
    p->SetStringProperty(c, vr::Prop_RenderModelName_String, lighthouse_ ? "lh_basestation_vive" : "rift_camera");
    p->SetStringProperty(c, vr::Prop_ModeLabel_String, "");
    // what it takes in: a Rift sensor's camera about 100 by 70 degrees, from 0.4 to 3.5 m; a base station sweeps
    // about 150 by 110 degrees, to 5 m
    p->SetFloatProperty(c, vr::Prop_FieldOfViewLeftDegrees_Float, lighthouse_ ? 75.0f : 50.0f);
    p->SetFloatProperty(c, vr::Prop_FieldOfViewRightDegrees_Float, lighthouse_ ? 75.0f : 50.0f);
    p->SetFloatProperty(c, vr::Prop_FieldOfViewTopDegrees_Float, lighthouse_ ? 55.0f : 35.0f);
    p->SetFloatProperty(c, vr::Prop_FieldOfViewBottomDegrees_Float, lighthouse_ ? 55.0f : 35.0f);
    p->SetFloatProperty(c, vr::Prop_TrackingRangeMinimumMeters_Float, lighthouse_ ? 0.2f : 0.4f);
    p->SetFloatProperty(c, vr::Prop_TrackingRangeMaximumMeters_Float, lighthouse_ ? 5.0f : 3.5f);
    p->SetBoolProperty(c, vr::Prop_DeviceProvidesBatteryStatus_Bool, false);
    p->SetBoolProperty(c, vr::Prop_CanWirelessIdentify_Bool, false);
    p->SetBoolProperty(c, vr::Prop_WillDriftInYaw_Bool, false);
    id_ = id;
    return vr::VRInitError_None;
  }

  void Deactivate() override { id_ = vr::k_unTrackedDeviceIndexInvalid; }
  void EnterStandby() override {}
  void *GetComponent(const char *) override { return nullptr; }
  void DebugRequest(const char *, char *response, uint32_t size) override {
    if (size)
      response[0] = 0;
  }
  vr::DriverPose_t *GetPose(vr::DriverPose_t *out) override {
    std::lock_guard<std::mutex> lock(mutex_);
    *out = pose_;
    return out;
  }

  void update(const mvr_tracking_reference &r) {
    if (id_ == vr::k_unTrackedDeviceIndexInvalid)
      return;
    vr::DriverPose_t pose{};
    pose.qWorldFromDriverRotation.w = 1;
    pose.qDriverFromHeadRotation.w = 1;
    pose.deviceIsConnected = true;
    pose.poseIsValid = (r.flags & MVR_REFERENCE_POSE_VALID) != 0;
    // until the sensor has seen the headset it does not know where it stands
    pose.result = pose.poseIsValid ? vr::TrackingResult_Running_OK : vr::TrackingResult_Calibrating_OutOfRange;
    pose.qRotation = {r.pose.orientation.w, r.pose.orientation.x, r.pose.orientation.y, r.pose.orientation.z};
    if (!pose.poseIsValid)
      pose.qRotation = {1, 0, 0, 0};
    pose.vecPosition[0] = r.pose.position.x;
    pose.vecPosition[1] = r.pose.position.y;
    pose.vecPosition[2] = r.pose.position.z;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      pose_ = pose;
    }
    vr::VRServerDriverHost()->TrackedDevicePoseUpdated(id_, pose, sizeof(pose));
  }

private:
  const std::string serial_;
  const bool lighthouse_;
  std::atomic<uint32_t> id_{vr::k_unTrackedDeviceIndexInvalid};
  std::mutex mutex_;
  vr::DriverPose_t pose_{};
};

class Headset final : public TrackedDevice {
public:
  explicit Headset(Link &link) : link_(link), display_(link.system()), direct_(link) {}

  bool init() { return direct_.init(); }

  vr::EVRInitError Activate(uint32_t id) override {
    id_ = id;
    const mvr_system_info &system = link_.system();
    vr::PropertyContainerHandle_t c = vr::VRProperties()->TrackedDeviceToPropertyContainer(id);
    auto *p = vr::VRProperties();
    const char *name = system.system_name[0] ? system.system_name : "MetalVR headset";
    headset_ = system.headset;
    p->SetStringProperty(c, vr::Prop_ModelNumber_String, name);
    p->SetStringProperty(c, vr::Prop_ManufacturerName_String, mvr_describe_headset(system.headset)->manufacturer);
    p->SetStringProperty(c, vr::Prop_TrackingSystemName_String, "metalvr");
    p->SetStringProperty(c, vr::Prop_SerialNumber_String, "MetalVR-HMD");
    p->SetStringProperty(c, vr::Prop_RenderModelName_String, "generic_hmd");
    p->SetFloatProperty(c, vr::Prop_UserIpdMeters_Float, system.ipd > 0 ? system.ipd : 0.0635f);
    p->SetFloatProperty(c, vr::Prop_UserHeadToEyeDepthMeters_Float, 0.0f);
    p->SetFloatProperty(c, vr::Prop_DisplayFrequency_Float, system.refresh_hz > 1 ? system.refresh_hz : 90.0f);
    p->SetFloatProperty(c, vr::Prop_SecondsFromVsyncToPhotons_Float, 0.011f);
    p->SetBoolProperty(c, vr::Prop_IsOnDesktop_Bool, false);
    p->SetBoolProperty(c, vr::Prop_HasDisplayComponent_Bool, true);
    p->SetBoolProperty(c, vr::Prop_HasDriverDirectModeComponent_Bool, true);
    p->SetBoolProperty(c, vr::Prop_DriverDirectModeSendsVsyncEvents_Bool, false);
    p->SetBoolProperty(c, vr::Prop_DeviceProvidesBatteryStatus_Bool, false);
    p->SetBoolProperty(c, vr::Prop_DeviceCanPowerOff_Bool, false);
    container_ = c;
    p->SetUint64Property(c, vr::Prop_CurrentUniverseId_Uint64, 0x4d56); // "MV": one fixed play space
    p->SetBoolProperty(c, vr::Prop_DisplayDebugMode_Bool, false);
    p->SetBoolProperty(c, vr::Prop_NeverTracked_Bool, false);
    log("metalvr: headset '%s', %ux%u per eye, %.0f Hz", name, system.eye_width, system.eye_height,
        system.refresh_hz);
    if (spatialUpscale().enabled)
      log("metalvr: MetalFX spatial upscaling: rendering %ux%u per eye (factor %.2f, from %s)",
          upscaledRenderSize(system.eye_width), upscaledRenderSize(system.eye_height), spatialUpscale().factor,
          spatialUpscale().source.c_str());
    running_ = true;
    poses_ = std::thread([this] { poseThread(); });
    return vr::VRInitError_None;
  }

  void Deactivate() override {
    running_ = false;
    if (poses_.joinable())
      poses_.join();
    direct_.endSession();
    id_ = vr::k_unTrackedDeviceIndexInvalid;
  }

  void EnterStandby() override {}

  void *GetComponent(const char *name) override {
    if (!std::strcmp(name, vr::IVRDisplayComponent_Version))
      return reinterpret_cast<vr::IVRDisplayComponent *>(static_cast<DisplayComponent *>(&display_));
    if (!std::strcmp(name, vr::IVRDriverDirectModeComponent_Version))
      return static_cast<vr::IVRDriverDirectModeComponent *>(&direct_);
    return nullptr;
  }

  void DebugRequest(const char *, char *response, uint32_t size) override {
    if (size)
      response[0] = 0;
  }

  vr::DriverPose_t *GetPose(vr::DriverPose_t *out) override {
    *out = pose();
    return out;
  }

private:
  vr::DriverPose_t pose(mvr_tracking_response *out_sample = nullptr) {
    vr::DriverPose_t pose{};
    pose.qWorldFromDriverRotation.w = 1;
    pose.qDriverFromHeadRotation.w = 1;
    mvr_tracking_request request{};
    mvr_tracking_response sample{};
    if (!link_.connection().call(MVR_MSG_TRACKING, request, sample) || sample.result != MVR_OK) {
      pose.result = vr::TrackingResult_Uninitialized;
      pose.deviceIsConnected = false;
      return pose;
    }
    const bool valid = (sample.flags & MVR_TRACKING_HEAD_VALID) != 0;
    pose.deviceIsConnected = (sample.flags & MVR_TRACKING_HMD_CONNECTED) != 0;
    pose.poseIsValid = valid;
    pose.result = valid ? vr::TrackingResult_Running_OK : vr::TrackingResult_Running_OutOfRange;
    pose.willDriftInYaw = driftsInYaw(headset_);
    pose.shouldApplyHeadModel = false;
    pose.qRotation = {sample.head.orientation.w, sample.head.orientation.x, sample.head.orientation.y,
                      sample.head.orientation.z};
    pose.vecPosition[0] = sample.head.position.x;
    pose.vecPosition[1] = sample.head.position.y;
    pose.vecPosition[2] = sample.head.position.z;
    pose.vecVelocity[0] = sample.head_linear_velocity.x;
    pose.vecVelocity[1] = sample.head_linear_velocity.y;
    pose.vecVelocity[2] = sample.head_linear_velocity.z;
    pose.vecAngularVelocity[0] = sample.head_angular_velocity.x;
    pose.vecAngularVelocity[1] = sample.head_angular_velocity.y;
    pose.vecAngularVelocity[2] = sample.head_angular_velocity.z;
    // the sample is for now: how long ago that was, so SteamVR predicts from the right moment
    const int64_t now_server = localNowNs() + link_.connection().clockOffsetNs();
    pose.poseTimeOffset = -static_cast<double>(now_server - sample.time_ns) / 1e9;
    if (out_sample)
      *out_sample = sample;
    return pose;
  }

  void poseThread() {
    while (running_) {
      if (id_ != vr::k_unTrackedDeviceIndexInvalid) {
        mvr_tracking_response sample{};
        const vr::DriverPose_t head = pose(&sample);
        if (head.deviceIsConnected)
          updateViews(sample);
        vr::VRServerDriverHost()->TrackedDevicePoseUpdated(id_, head, sizeof(vr::DriverPose_t));
        updateBattery(sample);
        if (head.poseIsValid != last_valid_ || head.deviceIsConnected != last_connected_) {
          last_valid_ = head.poseIsValid;
          last_connected_ = head.deviceIsConnected;
          log("metalvr: head pose %s, headset %s", head.poseIsValid ? "valid" : "not valid",
              head.deviceIsConnected ? "connected" : "not connected");
        }
        if (head.deviceIsConnected)
          for (Controller *c : controllers_)
            if (c)
              c->update(sample, head.poseTimeOffset);
      }
      Sleep(2); // about 500 poses a second
    }
  }

  // SteamVR renders each eye with the projection and eye position it was given, and metalvrd sends the eyes on as
  // they are drawn, so they have to be the headset's own: a streamed headset displays each half of the frame with
  // its real, asymmetric field of view, and an image drawn with another one sits shifted in each eye, which the eyes
  // cannot fuse. Those arrive with the tracking once the headset has connected; SteamVR is told whenever they change,
  // as ALVR's own driver does (SetDisplayEyeToHead, SetDisplayProjectionRaw).
  void updateViews(const mvr_tracking_response &sample) {
    const mvr_fov &l = sample.views[0].fov, &r = sample.views[1].fov;
    const auto usable = [](const mvr_fov &f) {
      return f.left < 0 && f.right > 0 && f.up > 0 && f.down < 0 && f.right - f.left < 3.0f && f.up - f.down < 3.0f;
    };
    if (!usable(l) || !usable(r))
      return;
    const mvr_vec3 a = sample.views[0].pose.position, b = sample.views[1].pose.position;
    float ipd = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y) + (b.z - a.z) * (b.z - a.z));
    const mvr_system_info &system = link_.system();
    if (!(ipd > 0.04f && ipd < 0.09f))
      ipd = system.ipd > 0 ? system.ipd : 0.0635f;
    const auto same = [](const mvr_fov &x, const mvr_fov &y) {
      return std::fabs(x.left - y.left) < 1e-4f && std::fabs(x.right - y.right) < 1e-4f &&
             std::fabs(x.up - y.up) < 1e-4f && std::fabs(x.down - y.down) < 1e-4f;
    };
    if (views_sent_ && same(l, system.fov[0]) && same(r, system.fov[1]) && std::fabs(ipd - system.ipd) < 1e-4f)
      return;
    link_.setViews(l, r, ipd);
    views_sent_ = true;

    vr::HmdMatrix34_t eye_to_head[2] = {};
    for (int eye = 0; eye < 2; ++eye) {
      eye_to_head[eye].m[0][0] = eye_to_head[eye].m[1][1] = eye_to_head[eye].m[2][2] = 1.0f;
      eye_to_head[eye].m[0][3] = (eye == 0 ? -0.5f : 0.5f) * ipd;
    }
    vr::HmdRect2_t projection[2] = {};
    for (int eye = 0; eye < 2; ++eye) {
      // as Display::GetProjectionRaw: OpenVR's top is the tangent of the angle below the axis
      const mvr_fov &f = eye == 0 ? l : r;
      projection[eye].vTopLeft = {{std::tan(f.left), std::tan(f.down)}};
      projection[eye].vBottomRight = {{std::tan(f.right), std::tan(f.up)}};
    }
    auto *host = vr::VRServerDriverHost();
    host->SetDisplayEyeToHead(id_, eye_to_head[0], eye_to_head[1]);
    host->SetDisplayProjectionRaw(id_, projection[0], projection[1]);
    vr::VRProperties()->SetFloatProperty(container_, vr::Prop_UserIpdMeters_Float, ipd);
    vr::VREvent_Data_t data{};
    data.ipd.ipdMeters = ipd;
    host->VendorSpecificEvent(id_, vr::VREvent_IpdChanged, data, 0);
    log("metalvr: the headset's views: IPD %.1f mm, left eye %.1f %.1f %.1f %.1f, right eye %.1f %.1f %.1f %.1f "
        "degrees (left, right, up, down)",
        ipd * 1000.0f, l.left * 57.29578f, l.right * 57.29578f, l.up * 57.29578f, l.down * 57.29578f,
        r.left * 57.29578f, r.right * 57.29578f, r.up * 57.29578f, r.down * 57.29578f);
  }

  // the headset's charge, when it reports one, a few times a minute
  void updateBattery(const mvr_tracking_response &sample) {
    const bool valid = (sample.flags & MVR_TRACKING_BATTERY_VALID) != 0;
    const ULONGLONG now = GetTickCount64();
    if (valid == battery_valid_ && now - battery_set_ < 10000)
      return;
    battery_set_ = now;
    battery_valid_ = valid;
    auto *p = vr::VRProperties();
    p->SetBoolProperty(container_, vr::Prop_DeviceProvidesBatteryStatus_Bool, valid);
    if (!valid)
      return;
    p->SetFloatProperty(container_, vr::Prop_DeviceBatteryPercentage_Float, std::clamp(sample.head_battery, 0.0f, 1.0f));
    p->SetBoolProperty(container_, vr::Prop_DeviceIsCharging_Bool, (sample.flags & MVR_TRACKING_CHARGING) != 0);
  }

  bool last_valid_ = false, last_connected_ = false;
  bool views_sent_ = false;
  bool battery_valid_ = false;
  ULONGLONG battery_set_ = 0;
  uint32_t headset_ = MVR_HEADSET_UNKNOWN;
  vr::PropertyContainerHandle_t container_ = 0;

public:
  Controller *controllers_[2] = {};

private:

  Link &link_;
  Display display_;
  DirectMode direct_;
  std::atomic<uint32_t> id_{vr::k_unTrackedDeviceIndexInvalid};
  std::atomic<bool> running_{false};
  std::thread poses_;
};

// ---- the driver ----

class Provider final : public vr::IServerTrackedDeviceProvider {
public:
  vr::EVRInitError Init(vr::IVRDriverContext *context) override {
    VR_INIT_SERVER_DRIVER_CONTEXT(context);
    if (!link_.open()) {
      log("metalvr: metalvrd is not running (start it, then SteamVR)");
      return vr::VRInitError_Init_HmdNotFound;
    }
    if (!(link_.system().flags & MVR_SYSTEM_HMD_CONNECTED))
      log("metalvr: metalvrd has no headset connected yet");
    headset_ = std::make_unique<Headset>(link_);
    if (!headset_->init())
      return vr::VRInitError_Driver_Failed;
    if (!vr::VRServerDriverHost()->TrackedDeviceAdded("MetalVR-HMD", vr::TrackedDeviceClass_HMD, headset_->asDriver()))
      return vr::VRInitError_Driver_Failed;
    // the controllers are there from the start, connected while metalvrd hears them
    for (uint32_t hand = 0; hand < 2; ++hand) {
      controllers_[hand] = std::make_unique<Controller>(link_, hand);
      if (vr::VRServerDriverHost()->TrackedDeviceAdded(hand == 0 ? "MetalVR-Touch-Left" : "MetalVR-Touch-Right",
                                                       vr::TrackedDeviceClass_Controller, controllers_[hand]->asDriver()))
        headset_->controllers_[hand] = controllers_[hand].get();
    }
    return vr::VRInitError_None;
  }

  void Cleanup() override {
    // SteamVR may still reach a device while it shuts down: the devices stay until the driver is unloaded (their
    // threads stopped in Deactivate)
    (void)headset_.release();
    for (auto &c : controllers_)
      (void)c.release();
    for (auto &r : references_)
      (void)r.release();
    VR_CLEANUP_SERVER_DRIVER_CONTEXT();
  }

  const char *const *GetInterfaceVersions() override { return vr::k_InterfaceVersions; }
  void RunFrame() override {
    vr::VREvent_t event;
    while (vr::VRServerDriverHost()->PollNextEvent(&event, sizeof(event))) {
      if (event.eventType == vr::VREvent_Input_HapticVibration)
        for (auto &c : controllers_)
          if (c && c->vibrate(event.data.hapticVibration))
            break;
    }
    updateReferences();
  }
  bool ShouldBlockStandbyMode() override { return false; }
  void EnterStandby() override {}
  void LeaveStandby() override {}

private:
  // The tracking sensors, asked for twice a second: a sensor metalvrd names for the first time is added to
  // SteamVR, and each is put where metalvrd has it (they move with every re-center).
  void updateReferences() {
    const ULONGLONG now = GetTickCount64();
    if (now - references_asked_ < 500)
      return;
    references_asked_ = now;
    mvr_references_response response{};
    if (!link_.connection().call(MVR_MSG_REFERENCES, response) || response.result != MVR_OK)
      return;
    for (uint32_t i = 0; i < response.count && i < MVR_MAX_TRACKING_REFERENCES; ++i) {
      mvr_tracking_reference &r = response.references[i];
      r.serial[sizeof(r.serial) - 1] = 0;
      if (!r.serial[0])
        continue;
      Reference *reference = nullptr;
      for (auto &known : references_)
        if (known && known->serial() == r.serial)
          reference = known.get();
      if (!reference) {
        for (auto &slot : references_) {
          if (slot)
            continue;
          const uint32_t headset = link_.system().headset;
          slot = std::make_unique<Reference>(r.serial, headset == MVR_HEADSET_VALVE_INDEX ||
                                                           headset == MVR_HEADSET_VIVE ||
                                                           headset == MVR_HEADSET_VIVE_PRO ||
                                                           headset == MVR_HEADSET_VIVE_PRO_2);
          if (vr::VRServerDriverHost()->TrackedDeviceAdded(r.serial, vr::TrackedDeviceClass_TrackingReference,
                                                           slot->asDriver())) {
            log("metalvr: tracking sensor %s", r.serial);
            reference = slot.get();
          } else {
            slot.reset();
          }
          break;
        }
      }
      if (reference)
        reference->update(r);
    }
  }

  Link link_;
  std::unique_ptr<Headset> headset_;
  std::unique_ptr<Controller> controllers_[2];
  std::unique_ptr<Reference> references_[MVR_MAX_TRACKING_REFERENCES];
  ULONGLONG references_asked_ = 0;
};

Provider g_provider;

} // namespace

extern "C" __declspec(dllexport) void *HmdDriverFactory(const char *interface_name, int *return_code) {
  if (!std::strcmp(interface_name, vr::IServerTrackedDeviceProvider_Version))
    return &g_provider;
  if (return_code)
    *return_code = vr::VRInitError_Init_InterfaceNotFound;
  return nullptr;
}
