// Composes a frame's layers into one stereo image: both eyes side by side, left eye in the left half.
#pragma once

#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "foveation.h"
#include "metalvr_protocol.h"

@protocol MTLFXSpatialScaler;

namespace metalvr {

struct ComposeLayerView {
  id<MTLTexture> texture = nil;
  mvr_rect rect{};
};

struct ComposeLayer {
  uint32_t type = 0;  // mvr_layer_type
  uint32_t flags = 0; // mvr_layer_flags
  uint32_t eye_mask = 3;
  ComposeLayerView views[2]; // projection: per eye; quad: views[0]
  mvr_pose quad_pose{};      // in tracking space, or head space with MVR_LAYER_HEAD_LOCKED
  float quad_width = 0;
  float quad_height = 0;
};

struct ComposeFrame {
  std::vector<ComposeLayer> layers;
  // where each eye was when the frame was rendered, for placing quads
  mvr_pose eye_pose[2]{};
  mvr_fov eye_fov[2]{};
  mvr_pose head_pose{};
};

class Compositor {
public:
  // a 4-byte-per-pixel texture as it is, written as a PNG (for looking at what an application sent)
  // the means of its colour channels and of its alpha (0 to 255) too, when asked: the PNG leaves alpha out
  bool writeTexturePNG(id<MTLTexture> texture, const char *path, double *mean_color = nullptr,
                       double *mean_alpha = nullptr);
  explicit Compositor(id<MTLDevice> device);
  ~Compositor();

  // the size of the whole frame; a change drops the buffer pool. With foveation enabled, the eyes are composed at
  // its eye size and squeezed into a frame of this size.
  bool configure(uint32_t frame_width, uint32_t frame_height, const Foveation &foveation = {});
  uint32_t frameWidth() const { return width_; }
  uint32_t frameHeight() const { return height_; }

  // Draws the frame and waits for the GPU to finish it. The caller releases the returned buffer; null on failure.
  CVPixelBufferRef compose(const ComposeFrame &frame);

  // whether eyes smaller than the frame's are scaled up with MetalFX (the GPU has it and it is not turned off), and
  // how many the last compose scaled (for --upscale-check)
  bool upscalesWithMetalFX() const { return metalfx_; }
  uint32_t lastUpscaledViews() const { return last_upscaled_; }
  uint32_t upscalersCreated() const { return upscalers_created_; }

private:
  // MetalFX spatial upscaling of a view of the bottom projection layer smaller than its eye: the view's rect is
  // copied out of the application's texture (MetalFX reads from the origin) and scaled to the eye's size. Each
  // view of a frame takes one of these of its format and eye size, kept across frames however the layers come and
  // go (SteamVR's change with its dashboard), since making one takes far longer than a frame has. Its input is
  // the eye's size and MetalFX is told each frame how much of it the view fills, so an application that changes
  // its render size every frame (dynamic resolution) keeps the same two.
  struct Upscaler {
    MTLPixelFormat format = MTLPixelFormatInvalid; // the application's
    uint32_t out_width = 0, out_height = 0;
    id<MTLFXSpatialScaler> scaler = nil;
    id<MTLTexture> input = nil;  // the view's rect at its origin, in the application's format; the eye's size
    id<MTLTexture> output = nil; // the eye, at its full size
    uint64_t used = 0;           // the last frame that took it
  };

  id<MTLRenderPipelineState> pipeline(bool blend);
  // the index of an upscaler of this format and eye size that this frame has not taken yet, made if there is
  // none, set to scale a view of the size given; -1 when MetalFX cannot scale it
  int upscaler(id<MTLTexture> source, uint32_t in_width, uint32_t in_height, uint32_t out_width,
               uint32_t out_height);

  id<MTLDevice> device_;
  id<MTLCommandQueue> queue_;
  id<MTLLibrary> library_;
  id<MTLSamplerState> sampler_;
  id<MTLRenderPipelineState> pipelines_[2]; // opaque, blended
  id<MTLRenderPipelineState> squeeze_ = nil;
  Foveation foveation_;
  id<MTLTexture> full_ = nil; // the eyes at their composed size, when foveated
  CVPixelBufferPoolRef pool_ = nullptr;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  bool metalfx_ = false; // MetalFX spatial upscaling on this GPU, and not turned off
  std::vector<Upscaler> upscalers_;
  std::vector<MTLPixelFormat> unscalable_; // formats MetalFX refused, drawn bilinearly
  std::set<std::string> upscale_logged_;   // each size and format, logged once (the first few)
  bool upscale_alloc_logged_ = false;
  uint64_t frame_ = 0;
  uint32_t last_upscaled_ = 0;
  uint32_t upscalers_created_ = 0;
};

// writes a composed frame as a PNG, for inspecting what the server would send
bool writePixelBufferPNG(CVPixelBufferRef buffer, const char *path);

} // namespace metalvr
