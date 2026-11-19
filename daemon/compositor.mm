#import "compositor.h"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <MetalFX/MetalFX.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "common.h"
#include "metalvr_math.h"

namespace metalvr {

namespace {

// The target is written through an sRGB view, so every source is brought to linear light first: an sRGB texture
// decodes when sampled, a float texture is linear already, and an 8- or 10-bit UNORM texture is taken to hold
// display-referred values, as applications that pick those formats write them.
NSString *const kShaderSource = @R"(
#include <metal_stdlib>
using namespace metal;

struct Vertex {
  float4 position;
  float2 uv;
  float2 pad;
};

struct Uniforms {
  uint decode_gamma;
  uint unpremultiplied;
  uint opaque;
  uint pad;
};

struct Varyings {
  float4 position [[position]];
  float2 uv;
};

vertex Varyings layer_vertex(uint vid [[vertex_id]], const device Vertex *vertices [[buffer(0)]]) {
  Varyings out;
  out.position = vertices[vid].position;
  out.uv = vertices[vid].uv;
  return out;
}

static float3 srgb_to_linear(float3 c) {
  float3 lo = c / 12.92;
  float3 hi = pow((c + 0.055) / 1.055, float3(2.4));
  return select(hi, lo, c <= float3(0.04045));
}

// ALVR's foveated encoding (foveation.h, CompressAxisAlignedPixelShader.hlsl): each pixel of the squeezed frame
// reads the composed frame where ALVR's client will put it back
struct Squeeze {
  float2 frame_size;
  float2 eye_size_ratio;
  float2 center_size;
  float2 center_shift;
  float2 edge_ratio;
};

vertex Varyings squeeze_vertex(uint vid [[vertex_id]]) {
  float2 p = float2((vid << 1) & 2, vid & 2);
  Varyings out;
  out.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
  out.uv = float2(p.x, 1.0 - p.y);
  return out;
}

fragment float4 squeeze_fragment(Varyings in [[stage_in]], texture2d<float> full [[texture(0)]],
                                 sampler smp [[sampler(0)]], constant Squeeze &f [[buffer(0)]]) {
  float2 uv = in.position.xy / f.frame_size;
  bool right = uv.x > 0.5;
  float2 eye = float2((uv.x + (right ? 1.0 - 2.0 * uv.x : 0.0)) * 2.0, uv.y) / f.eye_size_ratio;
  float2 c0 = (1.0 - f.center_size) / 2.0;
  float2 c1 = (f.edge_ratio - 1.0) * c0 * (f.center_shift + 1.0) / f.edge_ratio;
  float2 c2 = (f.edge_ratio - 1.0) * f.center_size + 1.0;
  float2 lo = c0 * (f.center_shift + 1.0) / c2;
  float2 hi = c0 * (f.center_shift - 1.0) / c2 + 1.0;
  float2 center = eye * c2 / f.edge_ratio + c1;
  float2 d2 = eye * c2;
  float2 d3 = (eye - 1.0) * c2 + 1.0;
  float2 g1 = eye / lo;
  float2 g2 = (1.0 - eye) / (1.0 - hi);
  float2 left_edge = g1 * center + (1.0 - g1) * d2;
  float2 right_edge = g2 * center + (1.0 - g2) * d3;
  float2 source = select(select(center, right_edge, eye > hi), left_edge, eye < lo);
  return full.sample(smp, float2(source.x * 0.5 + (right ? 1.0 - source.x : 0.0), source.y));
}

fragment float4 layer_fragment(Varyings in [[stage_in]], texture2d<float> source [[texture(0)]],
                               sampler smp [[sampler(0)]], constant Uniforms &u [[buffer(0)]]) {
  float4 c = source.sample(smp, in.uv);
  if (u.decode_gamma)
    c.rgb = srgb_to_linear(saturate(c.rgb));
  if (u.opaque)
    c.a = 1.0;
  else if (u.unpremultiplied)
    c.rgb *= c.a;
  return c;
}
)";

struct Vertex {
  float position[4];
  float uv[2];
  float pad[2];
};

struct Uniforms {
  uint32_t decode_gamma;
  uint32_t unpremultiplied;
  uint32_t opaque;
  uint32_t pad;
};

bool decodesWhenSampled(MTLPixelFormat format) {
  switch (format) {
  case MTLPixelFormatRGBA8Unorm_sRGB:
  case MTLPixelFormatBGRA8Unorm_sRGB:
    return true;
  default:
    return false;
  }
}

bool isLinearFormat(MTLPixelFormat format) {
  switch (format) {
  case MTLPixelFormatRGBA16Float:
  case MTLPixelFormatRGBA32Float:
  case MTLPixelFormatRG11B10Float:
    return true;
  default:
    return false;
  }
}

// The format MetalFX works in. An sRGB texture's bytes are given to it as they are, values in a perceptual space as
// its perceptual mode expects; its output is then a UNORM texture of those values, which layer_fragment decodes as
// it does any 8-bit UNORM source.
MTLPixelFormat scalerFormat(MTLPixelFormat format) {
  switch (format) {
  case MTLPixelFormatRGBA8Unorm_sRGB:
    return MTLPixelFormatRGBA8Unorm;
  case MTLPixelFormatBGRA8Unorm_sRGB:
    return MTLPixelFormatBGRA8Unorm;
  default:
    return format;
  }
}

// METALVR_METALFX_SPATIAL=0 (off, false, no) draws smaller eyes bilinearly, as before MetalFX (runtime/upscale.h)
bool metalfxTurnedOff() {
  const char *env = std::getenv("METALVR_METALFX_SPATIAL");
  if (!env)
    return false;
  return !std::strcmp(env, "0") || !strcasecmp(env, "off") || !strcasecmp(env, "false") || !strcasecmp(env, "no");
}

// the clip-space position of a point given in the eye's own space
void project(const mvr_fov &fov, mvr_vec3 p, float out[4]) {
  float tl = std::tan(fov.left), tr = std::tan(fov.right), tu = std::tan(fov.up), td = std::tan(fov.down);
  out[0] = (2.0f * p.x + (tr + tl) * p.z) / (tr - tl);
  out[1] = (2.0f * p.y + (tu + td) * p.z) / (tu - td);
  out[3] = -p.z;
  out[2] = 0.5f * out[3]; // layers are drawn in order, with no depth test
}

} // namespace

Compositor::Compositor(id<MTLDevice> device) : device_(device) {
  queue_ = [device_ newCommandQueue];
  queue_.label = @"metalvr-compositor";
  NSError *error = nil;
  library_ = [device_ newLibraryWithSource:kShaderSource options:nil error:&error];
  if (!library_)
    logInfo("compositor: shader compile failed: %s", error.localizedDescription.UTF8String);
  MTLSamplerDescriptor *sampler = [MTLSamplerDescriptor new];
  sampler.minFilter = MTLSamplerMinMagFilterLinear;
  sampler.magFilter = MTLSamplerMinMagFilterLinear;
  sampler.sAddressMode = MTLSamplerAddressModeClampToEdge;
  sampler.tAddressMode = MTLSamplerAddressModeClampToEdge;
  sampler_ = [device_ newSamplerStateWithDescriptor:sampler];
  if (@available(macOS 13.0, *)) {
    if (metalfxTurnedOff())
      logInfo("compositor: MetalFX spatial upscaling turned off by METALVR_METALFX_SPATIAL");
    else if (![MTLFXSpatialScalerDescriptor supportsDevice:device_])
      logInfo("compositor: MetalFX spatial upscaling is not supported on %s; smaller eyes are drawn bilinearly",
              device_.name.UTF8String);
    else
      metalfx_ = true;
  }
}

Compositor::Upscaler *Compositor::upscaler(size_t slot, id<MTLTexture> source, uint32_t in_width,
                                           uint32_t in_height, uint32_t out_width, uint32_t out_height) {
  const MTLPixelFormat format = source.pixelFormat;
  if (std::find(unscalable_.begin(), unscalable_.end(), format) != unscalable_.end())
    return nullptr;
  if (upscalers_.size() <= slot)
    upscalers_.resize(slot + 1);
  Upscaler &u = upscalers_[slot];
  if (u.scaler && u.format == format && u.in_width == in_width && u.in_height == in_height &&
      u.out_width == out_width && u.out_height == out_height)
    return &u;
  u = Upscaler{};
  if (@available(macOS 13.0, *)) {
    const MTLPixelFormat working = scalerFormat(format);
    MTLFXSpatialScalerDescriptor *desc = [MTLFXSpatialScalerDescriptor new];
    desc.colorTextureFormat = working;
    desc.outputTextureFormat = working;
    desc.inputWidth = in_width;
    desc.inputHeight = in_height;
    desc.outputWidth = out_width;
    desc.outputHeight = out_height;
    // a float texture holds linear light, which may go past 1; anything else holds display-referred values
    desc.colorProcessingMode = isLinearFormat(format) ? MTLFXSpatialScalerColorProcessingModeHDR
                                                      : MTLFXSpatialScalerColorProcessingModePerceptual;
    id<MTLFXSpatialScaler> scaler = [desc newSpatialScalerWithDevice:device_];
    id<MTLTexture> input = nil, color = nil, output = nil;
    if (scaler) {
      MTLTextureDescriptor *input_desc =
          [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:in_width height:in_height mipmapped:NO];
      input_desc.usage = scaler.colorTextureUsage | MTLTextureUsagePixelFormatView;
      input_desc.storageMode = MTLStorageModePrivate;
      input = [device_ newTextureWithDescriptor:input_desc];
      color = working == format ? input : [input newTextureViewWithPixelFormat:working];
      MTLTextureDescriptor *output_desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:working
                                                                                            width:out_width
                                                                                           height:out_height
                                                                                        mipmapped:NO];
      output_desc.usage = scaler.outputTextureUsage | MTLTextureUsageShaderRead;
      output_desc.storageMode = MTLStorageModePrivate;
      output = [device_ newTextureWithDescriptor:output_desc];
    }
    if (!scaler || !color || !output) {
      unscalable_.push_back(format);
      logInfo("compositor: MetalFX cannot scale pixel format %lu; those eyes are drawn bilinearly",
              static_cast<unsigned long>(format));
      return nullptr;
    }
    scaler.colorTexture = color;
    scaler.outputTexture = output;
    scaler.inputContentWidth = in_width;
    scaler.inputContentHeight = in_height;
    u.format = format;
    u.in_width = in_width;
    u.in_height = in_height;
    u.out_width = out_width;
    u.out_height = out_height;
    u.scaler = scaler;
    u.input = input;
    u.output = output;
    char what[160];
    std::snprintf(what, sizeof(what), "%ux%u to %ux%u per eye (pixel format %lu, %s)", in_width, in_height,
                  out_width, out_height, static_cast<unsigned long>(format),
                  isLinearFormat(format) ? "HDR" : "perceptual");
    if (upscale_logged_ != what) {
      upscale_logged_ = what;
      logInfo("compositor: MetalFX spatial upscaling %s", what);
    }
    return &u;
  }
  return nullptr;
}

Compositor::~Compositor() {
  if (pool_)
    CVPixelBufferPoolRelease(pool_);
}

id<MTLRenderPipelineState> Compositor::pipeline(bool blend) {
  // the fragment shader premultiplies an unpremultiplied source, so one blend state serves both kinds of alpha
  const int index = blend ? 1 : 0;
  if (pipelines_[index])
    return pipelines_[index];
  if (!library_)
    return nil;
  MTLRenderPipelineDescriptor *desc = [MTLRenderPipelineDescriptor new];
  desc.vertexFunction = [library_ newFunctionWithName:@"layer_vertex"];
  desc.fragmentFunction = [library_ newFunctionWithName:@"layer_fragment"];
  desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm_sRGB;
  if (blend) {
    desc.colorAttachments[0].blendingEnabled = YES;
    desc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
    desc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    desc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    desc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
  }
  NSError *error = nil;
  pipelines_[index] = [device_ newRenderPipelineStateWithDescriptor:desc error:&error];
  if (!pipelines_[index])
    logInfo("compositor: pipeline failed: %s", error.localizedDescription.UTF8String);
  return pipelines_[index];
}

bool Compositor::configure(uint32_t frame_width, uint32_t frame_height, const Foveation &foveation) {
  const bool same_foveation = foveation.enabled == foveation_.enabled &&
                              (!foveation.enabled || (foveation.eye_width == foveation_.eye_width &&
                                                      foveation.eye_height == foveation_.eye_height &&
                                                      foveation.center_size[0] == foveation_.center_size[0] &&
                                                      foveation.center_size[1] == foveation_.center_size[1] &&
                                                      foveation.center_shift[0] == foveation_.center_shift[0] &&
                                                      foveation.center_shift[1] == foveation_.center_shift[1] &&
                                                      foveation.edge_ratio[0] == foveation_.edge_ratio[0] &&
                                                      foveation.edge_ratio[1] == foveation_.edge_ratio[1]));
  if (frame_width == width_ && frame_height == height_ && pool_ && same_foveation)
    return true;
  foveation_ = foveation;
  full_ = nil;
  if (foveation_.enabled) {
    MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm_sRGB
                                                                                    width:foveation_.eye_width * 2
                                                                                   height:foveation_.eye_height
                                                                                mipmapped:NO];
    desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    desc.storageMode = MTLStorageModePrivate;
    full_ = [device_ newTextureWithDescriptor:desc];
    if (!squeeze_ && library_) {
      MTLRenderPipelineDescriptor *pipeline = [MTLRenderPipelineDescriptor new];
      pipeline.vertexFunction = [library_ newFunctionWithName:@"squeeze_vertex"];
      pipeline.fragmentFunction = [library_ newFunctionWithName:@"squeeze_fragment"];
      pipeline.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm_sRGB;
      NSError *error = nil;
      squeeze_ = [device_ newRenderPipelineStateWithDescriptor:pipeline error:&error];
      if (!squeeze_)
        logInfo("compositor: foveation pipeline failed: %s", error.localizedDescription.UTF8String);
    }
    if (!full_ || !squeeze_)
      return false;
    logInfo("compositor: foveated: %ux%u per eye squeezed to %ux%u", foveation_.eye_width, foveation_.eye_height,
            frame_width / 2, frame_height);
  }
  if (pool_) {
    CVPixelBufferPoolRelease(pool_);
    pool_ = nullptr;
  }
  width_ = frame_width;
  height_ = frame_height;
  NSDictionary *attributes = @{
    (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
    (id)kCVPixelBufferWidthKey : @(frame_width),
    (id)kCVPixelBufferHeightKey : @(frame_height),
    (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
    (id)kCVPixelBufferMetalCompatibilityKey : @YES,
  };
  CVReturn result =
      CVPixelBufferPoolCreate(kCFAllocatorDefault, nullptr, (__bridge CFDictionaryRef)attributes, &pool_);
  if (result != kCVReturnSuccess) {
    logInfo("compositor: buffer pool %ux%u failed: %d", frame_width, frame_height, result);
    pool_ = nullptr;
    return false;
  }
  return true;
}

CVPixelBufferRef Compositor::compose(const ComposeFrame &frame) {
  if (!pool_ || !library_)
    return nullptr;
  CVPixelBufferRef buffer = nullptr;
  if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool_, &buffer) != kCVReturnSuccess || !buffer)
    return nullptr;
  IOSurfaceRef surface = CVPixelBufferGetIOSurface(buffer);
  if (!surface) {
    CVPixelBufferRelease(buffer);
    return nullptr;
  }

  @autoreleasepool {
    MTLTextureDescriptor *target_desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm_sRGB
                                                           width:width_
                                                          height:height_
                                                       mipmapped:NO];
    target_desc.usage = MTLTextureUsageRenderTarget;
    target_desc.storageMode = MTLStorageModeShared;
    id<MTLTexture> target = [device_ newTextureWithDescriptor:target_desc iosurface:surface plane:0];
    if (!target) {
      CVPixelBufferRelease(buffer);
      return nullptr;
    }

    // foveated: the layers go to the eyes at their full size first, and are squeezed into the frame after
    const bool squeezed = foveation_.enabled && full_ && squeeze_;
    id<MTLTexture> layers_target = squeezed ? full_ : target;
    const uint32_t compose_width = static_cast<uint32_t>(layers_target.width);
    const uint32_t compose_height = static_cast<uint32_t>(layers_target.height);

    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = layers_target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);

    id<MTLCommandBuffer> commands = [queue_ commandBuffer];
    const uint32_t eye_width = compose_width / 2;

    // MetalFX first: a projection view smaller than the eye (an application rendering below the headset's size
    // for upscaling, runtime/upscale.h) is scaled up to the eye's size here, and drawn 1:1 below
    std::vector<id<MTLTexture>> upscaled(frame.layers.size() * 2, nil);
    last_upscaled_ = 0;
    if (metalfx_) {
      struct Job {
        size_t slot;
        id<MTLTexture> source;
        mvr_rect rect;
      };
      std::vector<Job> jobs;
      for (size_t l = 0; l < frame.layers.size(); ++l) {
        const ComposeLayer &layer = frame.layers[l];
        if (layer.type != MVR_LAYER_PROJECTION)
          continue;
        for (uint32_t eye = 0; eye < 2; ++eye) {
          const ComposeLayerView &view = layer.views[eye];
          if (!view.texture || view.texture.textureType != MTLTextureType2D)
            continue;
          // a mirrored rect (negative extents) is scaled by the region it covers, and flipped when drawn
          const int64_t x0 = std::min<int64_t>(view.rect.x, int64_t{view.rect.x} + view.rect.width);
          const int64_t y0 = std::min<int64_t>(view.rect.y, int64_t{view.rect.y} + view.rect.height);
          const int64_t rw = std::abs(int64_t{view.rect.width}), rh = std::abs(int64_t{view.rect.height});
          if (x0 < 0 || y0 < 0 || rw == 0 || rh == 0 || x0 + rw > static_cast<int64_t>(view.texture.width) ||
              y0 + rh > static_cast<int64_t>(view.texture.height))
            continue;
          const mvr_rect r = {static_cast<int32_t>(x0), static_cast<int32_t>(y0), static_cast<int32_t>(rw),
                              static_cast<int32_t>(rh)};
          const uint32_t w = static_cast<uint32_t>(rw), h = static_cast<uint32_t>(rh);
          if (w > eye_width || h > compose_height || (w == eye_width && h == compose_height))
            continue;
          if (Upscaler *u = upscaler(jobs.size(), view.texture, w, h, eye_width, compose_height)) {
            upscaled[l * 2 + eye] = u->output;
            jobs.push_back({jobs.size(), view.texture, r});
          }
        }
      }
      if (!jobs.empty()) {
        id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
        for (const Job &job : jobs)
          [blit copyFromTexture:job.source
                    sourceSlice:0
                    sourceLevel:0
                   sourceOrigin:MTLOriginMake(job.rect.x, job.rect.y, 0)
                     sourceSize:MTLSizeMake(job.rect.width, job.rect.height, 1)
                      toTexture:upscalers_[job.slot].input
               destinationSlice:0
               destinationLevel:0
              destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        if (@available(macOS 13.0, *)) {
          for (const Job &job : jobs)
            [upscalers_[job.slot].scaler encodeToCommandBuffer:commands];
        }
        last_upscaled_ = static_cast<uint32_t>(jobs.size());
      }
    }

    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
    [encoder setFragmentSamplerState:sampler_ atIndex:0];

    bool first = true;
    for (size_t l = 0; l < frame.layers.size(); ++l) {
      const ComposeLayer &layer = frame.layers[l];
      const bool alpha = (layer.flags & MVR_LAYER_BLEND_ALPHA) != 0;
      // the bottom layer covers the eye whatever its alpha holds; a quad never does
      const bool blend = layer.type == MVR_LAYER_QUAD ? alpha : (alpha && !first);
      id<MTLRenderPipelineState> state = pipeline(blend);
      if (!state)
        continue;
      [encoder setRenderPipelineState:state];

      for (uint32_t eye = 0; eye < 2; ++eye) {
        const ComposeLayerView &view = layer.type == MVR_LAYER_QUAD ? layer.views[0] : layer.views[eye];
        if (!view.texture)
          continue;
        if (layer.type == MVR_LAYER_QUAD && !(layer.eye_mask & (1u << eye)))
          continue;

        id<MTLTexture> texture = view.texture;
        const float tw = static_cast<float>(view.texture.width), th = static_cast<float>(view.texture.height);
        float u0 = view.rect.x / tw, u1 = (view.rect.x + view.rect.width) / tw;
        float v0 = view.rect.y / th, v1 = (view.rect.y + view.rect.height) / th;
        if (layer.type == MVR_LAYER_PROJECTION && upscaled[l * 2 + eye]) {
          texture = upscaled[l * 2 + eye]; // the whole of it, the eye's size, flipped as the rect was
          u0 = view.rect.width < 0 ? 1 : 0;
          u1 = 1 - u0;
          v0 = view.rect.height < 0 ? 1 : 0;
          v1 = 1 - v0;
        }

        Vertex vertices[4];
        if (layer.type == MVR_LAYER_QUAD) {
          // the quad's corners in the eye's space: bottom-left, bottom-right, top-left, top-right
          mvr_pose quad_in_tracking =
              (layer.flags & MVR_LAYER_HEAD_LOCKED) ? poseMul(frame.head_pose, layer.quad_pose) : layer.quad_pose;
          mvr_pose quad_in_eye = poseMul(poseInverse(frame.eye_pose[eye]), quad_in_tracking);
          const float hw = layer.quad_width * 0.5f, hh = layer.quad_height * 0.5f;
          const mvr_vec3 corners[4] = {{-hw, -hh, 0}, {hw, -hh, 0}, {-hw, hh, 0}, {hw, hh, 0}};
          const float uvs[4][2] = {{u0, v1}, {u1, v1}, {u0, v0}, {u1, v0}};
          for (int i = 0; i < 4; ++i) {
            mvr_vec3 p = quad_in_eye.position + quatRotate(quad_in_eye.orientation, corners[i]);
            project(frame.eye_fov[eye], p, vertices[i].position);
            vertices[i].uv[0] = uvs[i][0];
            vertices[i].uv[1] = uvs[i][1];
          }
        } else {
          const float positions[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
          const float uvs[4][2] = {{u0, v1}, {u1, v1}, {u0, v0}, {u1, v0}};
          for (int i = 0; i < 4; ++i) {
            vertices[i].position[0] = positions[i][0];
            vertices[i].position[1] = positions[i][1];
            vertices[i].position[2] = 0.5f;
            vertices[i].position[3] = 1.0f;
            vertices[i].uv[0] = uvs[i][0];
            vertices[i].uv[1] = uvs[i][1];
          }
        }

        Uniforms uniforms{};
        MTLPixelFormat format = texture.pixelFormat;
        uniforms.decode_gamma = !decodesWhenSampled(format) && !isLinearFormat(format);
        uniforms.unpremultiplied = (layer.flags & MVR_LAYER_UNPREMULTIPLIED_ALPHA) != 0;
        uniforms.opaque = !blend;

        MTLViewport viewport = {static_cast<double>(eye * eye_width), 0, static_cast<double>(eye_width),
                                static_cast<double>(compose_height), 0, 1};
        [encoder setViewport:viewport];
        MTLScissorRect scissor = {eye * eye_width, 0, eye_width, compose_height};
        [encoder setScissorRect:scissor];
        [encoder setVertexBytes:vertices length:sizeof(vertices) atIndex:0];
        [encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
        [encoder setFragmentTexture:texture atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
      }
      first = false;
    }

    [encoder endEncoding];
    if (squeezed) {
      struct {
        float frame_size[2];
        float eye_size_ratio[2];
        float center_size[2];
        float center_shift[2];
        float edge_ratio[2];
      } squeeze = {{static_cast<float>(width_), static_cast<float>(height_)},
                   {foveation_.eye_size_ratio[0], foveation_.eye_size_ratio[1]},
                   {foveation_.center_size[0], foveation_.center_size[1]},
                   {foveation_.center_shift[0], foveation_.center_shift[1]},
                   {foveation_.edge_ratio[0], foveation_.edge_ratio[1]}};
      MTLRenderPassDescriptor *out = [MTLRenderPassDescriptor renderPassDescriptor];
      out.colorAttachments[0].texture = target;
      out.colorAttachments[0].loadAction = MTLLoadActionDontCare;
      out.colorAttachments[0].storeAction = MTLStoreActionStore;
      id<MTLRenderCommandEncoder> squeeze_encoder = [commands renderCommandEncoderWithDescriptor:out];
      [squeeze_encoder setRenderPipelineState:squeeze_];
      [squeeze_encoder setFragmentTexture:full_ atIndex:0];
      [squeeze_encoder setFragmentSamplerState:sampler_ atIndex:0];
      [squeeze_encoder setFragmentBytes:&squeeze length:sizeof(squeeze) atIndex:0];
      [squeeze_encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
      [squeeze_encoder endEncoding];
    }
    [commands commit];
    [commands waitUntilCompleted];
    if (commands.status != MTLCommandBufferStatusCompleted) {
      logInfo("compositor: command buffer failed: %s", commands.error.localizedDescription.UTF8String);
      CVPixelBufferRelease(buffer);
      return nullptr;
    }
  }
  return buffer;
}

bool writePixelBufferPNG(CVPixelBufferRef buffer, const char *path) {
  if (CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess)
    return false;
  bool ok = false;
  const size_t width = CVPixelBufferGetWidth(buffer), height = CVPixelBufferGetHeight(buffer);
  CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef context = CGBitmapContextCreate(
      CVPixelBufferGetBaseAddress(buffer), width, height, 8, CVPixelBufferGetBytesPerRow(buffer), space,
      static_cast<uint32_t>(kCGImageAlphaNoneSkipFirst) | static_cast<uint32_t>(kCGBitmapByteOrder32Little));
  if (context) {
    CGImageRef image = CGBitmapContextCreateImage(context);
    if (image) {
      NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
      CGImageDestinationRef destination = CGImageDestinationCreateWithURL(
          (__bridge CFURLRef)url, (__bridge CFStringRef)UTTypePNG.identifier, 1, nullptr);
      if (destination) {
        CGImageDestinationAddImage(destination, image, nullptr);
        ok = CGImageDestinationFinalize(destination);
        CFRelease(destination);
      }
      CGImageRelease(image);
    }
    CGContextRelease(context);
  }
  CGColorSpaceRelease(space);
  CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
  return ok;
}

} // namespace metalvr

bool metalvr::Compositor::writeTexturePNG(id<MTLTexture> texture, const char *path, double *mean_color,
                                          double *mean_alpha) {
  if (!texture || texture.textureType != MTLTextureType2D)
    return false;
  const NSUInteger width = texture.width, height = texture.height, row = width * 4;
  id<MTLBuffer> bytes = [device_ newBufferWithLength:row * height options:MTLResourceStorageModeShared];
  id<MTLCommandBuffer> commands = [queue_ commandBuffer];
  id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
  [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
             sourceSize:MTLSizeMake(width, height, 1) toBuffer:bytes destinationOffset:0
    destinationBytesPerRow:row destinationBytesPerImage:row * height];
  [blit endEncoding];
  [commands commit];
  [commands waitUntilCompleted];
  if (commands.error)
    return false;
  if (mean_color && mean_alpha) {
    const uint8_t *px = static_cast<const uint8_t *>(bytes.contents);
    double color = 0, alpha = 0;
    for (NSUInteger i = 0; i < row * height; i += 4) {
      color += px[i] + px[i + 1] + px[i + 2];
      alpha += px[i + 3];
    }
    *mean_color = color / (width * height * 3.0);
    *mean_alpha = alpha / (width * height);
  }
  const MTLPixelFormat f = texture.pixelFormat;
  const bool bgra = f == MTLPixelFormatBGRA8Unorm || f == MTLPixelFormatBGRA8Unorm_sRGB;
  CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  const uint32_t info = bgra ? (static_cast<uint32_t>(kCGImageAlphaNoneSkipFirst) | static_cast<uint32_t>(kCGBitmapByteOrder32Little))
                             : static_cast<uint32_t>(kCGImageAlphaNoneSkipLast);
  CGContextRef context = CGBitmapContextCreate(bytes.contents, width, height, 8, row, space, info);
  bool ok = false;
  if (context) {
    CGImageRef image = CGBitmapContextCreateImage(context);
    if (image) {
      NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
      CGImageDestinationRef destination = CGImageDestinationCreateWithURL(
          (__bridge CFURLRef)url, (__bridge CFStringRef)UTTypePNG.identifier, 1, nullptr);
      if (destination) {
        CGImageDestinationAddImage(destination, image, nullptr);
        ok = CGImageDestinationFinalize(destination);
        CFRelease(destination);
      }
      CGImageRelease(image);
    }
    CGContextRelease(context);
  }
  CGColorSpaceRelease(space);
  return ok;
}
