// metalvrd: the MetalVR server. OpenXR runtimes connect to it; it composes their frames, encodes them, and
// streams them to a headset through a driver.
#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

#include <chrono>
#include <vector>

#include "common.h"
#include "compositor.h"
#include "direct_backend.h"
#include "psvr2_protocol.h"
#include "driver.h"
#include "foveation.h"
#include "headsets.h"
#include "lighthouse_protocol.h"
#include "lighthouse_tracking.h"
#include "server.h"

namespace metalvr {
bool g_verbose = false;
volatile sig_atomic_t g_snapshot_requested = 0;
}

using namespace metalvr;

namespace {

Server *g_server = nullptr;

void onSnapshotSignal(int) { g_snapshot_requested = 1; }

void onSignal(int) {
  if (g_server)
    g_server->requestStop();
}

void usage() {
  std::fprintf(
      stderr,
      "usage: metalvrd [options]\n"
      "  --driver sim|alvr|rift|lighthouse|psvr2\n"
      "                           headset driver (default: alvr if built in, else sim). rift: an Oculus Rift CV1\n"
      "                           or Rift S on this Mac's video output and USB (docs/RIFT.md); lighthouse: an HTC\n"
      "                           Vive, Vive Pro, Vive Pro 2 or Valve Index there; psvr2: a PlayStation VR2\n"
      "                           through Sony's PC adapter (docs/HEADSETS.md)\n"
      "  --port N                 loopback port runtimes connect to (default %d, or $%s)\n"
      "  --config-dir PATH        where the ALVR driver keeps its session and logs\n"
      "  --trust HOSTNAME         let this headset connect (as its ALVR client shows it, e.g. 1234.client.local.);\n"
      "                           repeatable. A headset that is not trusted is logged with its name and refused.\n"
      "  --headset MODEL          which headset this is (docs/HEADSETS.md): quest-1, quest-2, quest-3, quest-3s,\n"
      "                           quest-pro, steam-frame; alvr: instead of the model its client reports; sim: the\n"
      "                           headset to stand in for, also rift-cv1, rift-s, index, vive, vive-pro, vive-pro-2,\n"
      "                           psvr2\n"
      "  --adb PATH               alvr: the adb a headset on USB (--trust client.wired) is reached with (default:\n"
      "                           adb on PATH)\n"
      "  --foveated-encoding      alvr: squeeze each eye toward its edges before encoding, as ALVR's foveated\n"
      "                           encoding does (its shape from ALVR's settings); fewer pixels for the headset\n"
      "  --verbose                log per-swapchain and per-interval detail\n"
      "rift, lighthouse and psvr2:\n"
      "  --ipd MM                 the distance between the eyes (default: the headset's lens separation)\n"
      "  --passthrough            lighthouse: the home shows the room through the headset's cameras\n"
      "  --refresh HZ             set the headset's display to this rate, if it has it (an Index: 80 to 144)\n"
      "  --pro2-mode N            lighthouse: switch a Vive Pro 2 to mode N first (0 2448x1224@90, 1 @120,\n"
      "                           2 3264x1632@90, 3 3680x1836@90; 4 and 5, 4896x2448, need DSC)\n"
      "  --test-scene             between applications, a grid for checking the lens correction instead of\n"
      "                           the home\n"
      "  --home-logo PATH         the logo the home shows (default: ../share/metalvr/home-logo.png beside\n"
      "                           metalvrd)\n"
      "simulated headset, rift, lighthouse and psvr2:\n"
      "  --eye WxH                per-eye render size (default 1440x1584; the others: the headset's)\n"
      "simulated headset:\n"
      "  --fps N                  refresh rate (default 72)\n"
      "  --codec h264|hevc        (default h264)\n"
      "  --bitrate MBPS           (default 30)\n"
      "  --dump FILE              write the encoded stream as Annex B\n"
      "  --no-animate             hold the head and controllers still\n"
      "testing:\n"
      "  --snapshot FILE.png      write one composed frame as a PNG\n"
      "  --snapshot-frame N       which composed frame (default 30)\n"
      "  --exit-after-session     quit when the client that ran a session disconnects\n"
      "  --passthrough-check      draw an Index's home with a test camera picture, check each eye, and exit\n"
      "  --survive-check [sim]    start libsurvive (Lighthouse positions) for a few seconds, list what it tracks;\n"
      "                           sim: its simulated device and base stations, which must be tracked\n"
      "  --foveation-check        squeeze a test picture for foveated encoding on the GPU, check it, and exit\n"
      "  --upscale-check          scale smaller eyes up with MetalFX as an upscaling game's are, check them, and exit\n"
      "  --panel-check MODEL PNG  draw the lens-check grid for rift-cv1, rift-s, index, vive, vive-pro, vive-pro-2\n"
      "                           or psvr2 into PNG, check each eye's straight-ahead mark, and exit\n"
      "  --software-encoder       encode on the CPU; for machines with no hardware encoder, not for a headset\n",
      MVR_DEFAULT_PORT, MVR_PORT_ENV);
}

// --panel-check: the lens-check grid for a headset's model, drawn as the headset is drawn but into a picture; each
// eye's straight-ahead mark (the red dot of the grid) must be inside that eye's part of the panel. The Lighthouse
// headsets' lenses come from their firmware: here a stand-in lens, barrel-shaped, with the model's layout.
int panelCheck(const std::string &key, const std::string &png) {
  const uint32_t headset = headsetFromKey(key);
  DirectModel model;
  uint32_t width = 0, height = 0;
  if (headset == MVR_HEADSET_RIFT_CV1) {
    model = riftCv1Model();
    width = 2160, height = 1200;
  } else if (headset == MVR_HEADSET_RIFT_S) {
    model = riftSModel();
    width = 1440, height = 2560;
  } else if (headset == MVR_HEADSET_PSVR2) {
    // the lens as an uncalibrated headset has it (Monado's default), on the 4000x2040 panel
    model = psvr2Model(psvr2::defaultLensCalibration());
    width = 4000, height = 2040;
  } else if (headset == MVR_HEADSET_VALVE_INDEX || headset == MVR_HEADSET_VIVE || headset == MVR_HEADSET_VIVE_PRO ||
             headset == MVR_HEADSET_VIVE_PRO_2) {
    lighthouse::HeadsetConfig config;
    config.model = headset == MVR_HEADSET_VALVE_INDEX ? lighthouse::Model::Index
                   : headset == MVR_HEADSET_VIVE      ? lighthouse::Model::Vive
                   : headset == MVR_HEADSET_VIVE_PRO  ? lighthouse::Model::VivePro
                                                      : lighthouse::Model::VivePro2;
    config.eye_width = headset == MVR_HEADSET_VIVE ? 1080 : headset == MVR_HEADSET_VIVE_PRO_2 ? 1632 : 1440;
    config.eye_height = headset == MVR_HEADSET_VIVE ? 1200 : headset == MVR_HEADSET_VIVE_PRO_2 ? 1632 : 1600;
    for (int eye = 0; eye < 2; ++eye) {
      lighthouse::Distortion &d = config.distortion[eye];
      d.intrinsic_focus[0] = d.intrinsic_focus[1] = 1.5f; // 45 degrees to each edge
      for (int c = 0; c < 3; ++c)
        d.coefficients[c][0] = 0.08f + 0.02f * c;
      config.fov[eye] = {-0.785f, 0.785f, 0.785f, -0.785f};
      if (headset == MVR_HEADSET_VALVE_INDEX) {
        // canted 5 degrees outward
        const float half = (eye == 0 ? 2.5f : -2.5f) * 3.14159265f / 180.0f;
        config.eye_rotation[eye] = {0, std::sin(half), 0, std::cos(half)};
      }
    }
    model = lighthouseModel(config, 2);
    width = static_cast<uint32_t>(config.eye_width * 2), height = static_cast<uint32_t>(config.eye_height);
  } else {
    std::fprintf(stderr, "--panel-check: %s is not a headset drawn on this Mac's display\n", key.c_str());
    return 2;
  }
  std::vector<uint8_t> bgra;
  if (!drawTestPanel(model, width, height, bgra)) {
    std::fprintf(stderr, "--panel-check: drawing failed\n");
    return 1;
  }
  int failures = 0;
  for (int eye = 0; eye < 2; ++eye) {
    const uint32_t x0 = model.portrait_eyes ? 0 : eye * width / 2, y0 = model.portrait_eyes ? eye * height / 2 : 0;
    const uint32_t w = model.portrait_eyes ? width : width / 2, h = model.portrait_eyes ? height / 2 : height;
    double sx = 0, sy = 0;
    uint64_t mark = 0, lit = 0;
    for (uint32_t y = y0; y < y0 + h; ++y)
      for (uint32_t x = x0; x < x0 + w; ++x) {
        const uint8_t *p = &bgra[(static_cast<size_t>(y) * width + x) * 4];
        const uint8_t b = p[0], g = p[1], r = p[2];
        lit += r > 8 || g > 8 || b > 8;
        if (r > 230 && g < 110 && b < 110) {
          ++mark;
          sx += x - x0;
          sy += y - y0;
        }
      }
    const double fx = mark ? sx / mark / w : -1, fy = mark ? sy / mark / h : -1;
    const bool ok = mark >= 20 && fx > 0.1 && fx < 0.9 && fy > 0.1 && fy < 0.9 && lit > static_cast<uint64_t>(w) * h / 4;
    std::printf("%s eye: straight ahead at %.2f, %.2f of its part of the panel (%llu pixels), %.0f%% drawn: %s\n",
                eye == 0 ? "left" : "right", fx, fy, static_cast<unsigned long long>(mark),
                100.0 * lit / (static_cast<double>(w) * h), ok ? "ok" : "FAIL");
    failures += !ok;
  }
  if (!png.empty()) {
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(bgra.data(), width, height, 8, width * 4, space,
                                                 static_cast<uint32_t>(kCGImageAlphaNoneSkipFirst) |
                                                     static_cast<uint32_t>(kCGBitmapByteOrder32Little));
    CGImageRef image = context ? CGBitmapContextCreateImage(context) : nullptr;
    CGImageDestinationRef out = image ? CGImageDestinationCreateWithURL(
                                            (__bridge CFURLRef)[NSURL fileURLWithPath:@(png.c_str())],
                                            CFSTR("public.png"), 1, nullptr)
                                      : nullptr;
    if (out) {
      CGImageDestinationAddImage(out, image, nullptr);
      CGImageDestinationFinalize(out);
      CFRelease(out);
    }
    CGImageRelease(image);
    if (context)
      CGContextRelease(context);
    CGColorSpaceRelease(space);
  }
  std::printf("%s: %s panel %ux%u\n", failures ? "FAILED" : "PASSED", key.c_str(), width, height);
  return failures ? 1 : 0;
}

// --foveation-check: each eye an image whose colour is its own position (red across, green down), composed and
// squeezed for ALVR's foveated encoding on the GPU; every pixel of the squeezed frame must show the point of the
// eye the CPU's port of ALVR's shader says (foveationSource), which ALVR's client's stretch undoes
// (tests/headset_protocol_check.cpp).
int foveationCheck() {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  const uint32_t eye_w = 1024, eye_h = 1024;
  const Foveation f = computeFoveation(eye_w, eye_h, 0.45f, 0.4f, 0.4f, 0.1f, 4.0f, 5.0f);
  Compositor compositor(device);
  const uint32_t out_w = f.optimized_eye_width * 2, out_h = f.optimized_eye_height;
  if (!device || !compositor.configure(out_w, out_h, f)) {
    std::fprintf(stderr, "--foveation-check: no compositor\n");
    return 1;
  }
  // linear-light float textures, so the values come back as written
  MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                                width:eye_w
                                                                               height:eye_h
                                                                            mipmapped:NO];
  td.storageMode = MTLStorageModeShared;
  ComposeFrame frame;
  ComposeLayer layer;
  layer.type = MVR_LAYER_PROJECTION;
  std::vector<float> pixels(static_cast<size_t>(eye_w) * eye_h * 4);
  for (int eye = 0; eye < 2; ++eye) {
    for (uint32_t y = 0; y < eye_h; ++y)
      for (uint32_t x = 0; x < eye_w; ++x) {
        float *p = &pixels[(static_cast<size_t>(y) * eye_w + x) * 4];
        p[0] = (x + 0.5f) / eye_w;
        p[1] = (y + 0.5f) / eye_h;
        p[2] = eye ? 1.0f : 0.0f;
        p[3] = 1;
      }
    id<MTLTexture> t = [device newTextureWithDescriptor:td];
    [t replaceRegion:MTLRegionMake2D(0, 0, eye_w, eye_h) mipmapLevel:0 withBytes:pixels.data() bytesPerRow:eye_w * 16];
    layer.views[eye].texture = t;
    layer.views[eye].rect = {0, 0, static_cast<int32_t>(eye_w), static_cast<int32_t>(eye_h)};
  }
  frame.layers.push_back(layer);
  CVPixelBufferRef buffer = compositor.compose(frame);
  if (!buffer) {
    std::fprintf(stderr, "--foveation-check: compose failed\n");
    return 1;
  }
  CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
  const uint8_t *base = static_cast<const uint8_t *>(CVPixelBufferGetBaseAddress(buffer));
  const size_t stride = CVPixelBufferGetBytesPerRow(buffer);
  // the frame is 8-bit sRGB: back to linear, as the position was written
  const auto linear = [](uint8_t v) {
    const double c = v / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
  };
  double worst = 0;
  uint64_t wrong_eye = 0, counted = 0;
  for (uint32_t y = 2; y + 2 < out_h; y += 7)
    for (uint32_t x = 2; x + 2 < out_w; x += 7) {
      if (x + 3 > out_w / 2 && x < out_w / 2 + 3)
        continue; // the seam between the eyes
      const uint8_t *p = base + y * stride + x * 4; // BGRA
      const float uv[2] = {(x + 0.5f) / out_w, (y + 0.5f) / out_h};
      float source[2];
      foveationSource(f, uv, source);
      const int eye = uv[0] > 0.5f ? 1 : 0;
      const double expected_x = source[0] * 2.0 - eye, expected_y = source[1];
      if (expected_x < 0.01 || expected_x > 0.99 || expected_y < 0.01 || expected_y > 0.99)
        continue; // the squeeze reads past the eye there: clamped at its edge
      wrong_eye += (linear(p[0]) > 0.5) != (eye == 1);
      const double dx = std::fabs(linear(p[2]) - expected_x), dy = std::fabs(linear(p[1]) - expected_y);
      // 8 bits of sRGB are about 1/400 at the bright end
      worst = std::max(worst, std::max(dx, dy));
      ++counted;
    }
  CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
  CVPixelBufferRelease(buffer);
  const bool ok = counted > 1000 && wrong_eye == 0 && worst < 0.01;
  std::printf("%s: %ux%u per eye squeezed to %ux%u; %llu pixels, worst %.4f off, %llu from the wrong eye\n",
              ok ? "PASSED" : "FAILED", eye_w, eye_h, f.optimized_eye_width, f.optimized_eye_height,
              static_cast<unsigned long long>(counted), worst, static_cast<unsigned long long>(wrong_eye));
  return ok ? 0 : 1;
}

// --upscale-check: eyes rendered at 2/3 of the headset's size, as with METALVR_METALFX_SPATIAL=1.5, side by side in
// one texture as SteamVR hands them over (the right eye's rect away from the origin, and mirrored upside down), in
// an 8-bit sRGB format and a float one. Each eye's colour is its own position (red across, green down, blue which
// eye); MetalFX must have scaled both eyes, and every pixel of the frame must show its own point of its own eye.
int upscaleCheck() {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  const uint32_t in_w = 640, in_h = 704, out_eye_w = 960, out_eye_h = 1056;
  Compositor compositor(device);
  if (!device || !compositor.configure(out_eye_w * 2, out_eye_h)) {
    std::fprintf(stderr, "--upscale-check: no compositor\n");
    return 1;
  }
  if (!compositor.upscalesWithMetalFX()) {
    std::printf("SKIPPED: MetalFX spatial upscaling is not available here (see the line above)\n");
    return 0;
  }
  const auto decode = [](double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
  int failures = 0;
  for (const MTLPixelFormat format : {MTLPixelFormatBGRA8Unorm_sRGB, MTLPixelFormatRGBA16Float}) {
    const bool half = format == MTLPixelFormatRGBA16Float;
    // both eyes side by side in one texture, each eye's colour its own position
    const auto eyes = [&](uint32_t w, uint32_t h) {
      MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                    width:w * 2
                                                                                   height:h
                                                                                mipmapped:NO];
      td.storageMode = MTLStorageModeShared;
      td.usage = MTLTextureUsageShaderRead; // as metalvrd opens an application's shared texture
      id<MTLTexture> texture = [device newTextureWithDescriptor:td];
      std::vector<uint8_t> bytes(static_cast<size_t>(w) * 2 * h * 4);
      std::vector<__fp16> halves(static_cast<size_t>(w) * 2 * h * 4);
      for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w * 2; ++x) {
          const uint32_t eye = x >= w ? 1 : 0;
          // the position in perceptual values, so 8 bits hold it evenly
          const uint32_t row = eye ? h - 1 - y : y;
          const double c[3] = {(x - eye * w + 0.5) / w, (row + 0.5) / h, eye ? 1.0 : 0.0};
          const size_t i = (static_cast<size_t>(y) * w * 2 + x) * 4;
          if (half) {
            for (int k = 0; k < 3; ++k)
              halves[i + k] = static_cast<__fp16>(decode(c[k]));
            halves[i + 3] = 1;
          } else {
            bytes[i + 0] = static_cast<uint8_t>(std::lround(c[2] * 255)); // BGRA
            bytes[i + 1] = static_cast<uint8_t>(std::lround(c[1] * 255));
            bytes[i + 2] = static_cast<uint8_t>(std::lround(c[0] * 255));
            bytes[i + 3] = 255;
          }
        }
      if (half)
        [texture replaceRegion:MTLRegionMake2D(0, 0, w * 2, h) mipmapLevel:0 withBytes:halves.data()
                   bytesPerRow:w * 2 * 8];
      else
        [texture replaceRegion:MTLRegionMake2D(0, 0, w * 2, h) mipmapLevel:0 withBytes:bytes.data()
                   bytesPerRow:w * 2 * 4];
      ComposeFrame frame;
      ComposeLayer layer;
      layer.type = MVR_LAYER_PROJECTION;
      for (int eye = 0; eye < 2; ++eye)
        layer.views[eye].texture = texture;
      layer.views[0].rect = {0, 0, static_cast<int32_t>(w), static_cast<int32_t>(h)};
      // the right eye upside down in the texture, and its rect mirrored to say so, as an OpenGL application's are
      layer.views[1].rect = {static_cast<int32_t>(w), static_cast<int32_t>(h), static_cast<int32_t>(w),
                             -static_cast<int32_t>(h)};
      frame.layers.push_back(layer);
      return frame;
    };
    // Two frames at a larger size first, then the one checked: the scalers are the ones kept from those frames,
    // told the new size, as for an application whose render size changes from frame to frame. Smaller after
    // larger, so the input still holds the larger frame's pixels past the checked one's edges.
    const ComposeFrame larger = eyes(in_w * 5 / 4, in_h * 5 / 4), frame = eyes(in_w, in_h);
    CVPixelBufferRef buffer = nullptr;
    uint32_t made_before = 0;
    for (int i = 0; i < 3; ++i) {
      if (buffer)
        CVPixelBufferRelease(buffer);
      if (i == 2)
        made_before = compositor.upscalersCreated();
      buffer = compositor.compose(i < 2 ? larger : frame);
      if (!buffer) {
        std::fprintf(stderr, "--upscale-check: compose failed\n");
        return 1;
      }
    }
    const uint32_t made_after = compositor.upscalersCreated() - made_before;
    const uint32_t scaled = compositor.lastUpscaledViews();
    CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
    const uint8_t *base = static_cast<const uint8_t *>(CVPixelBufferGetBaseAddress(buffer));
    const size_t stride = CVPixelBufferGetBytesPerRow(buffer);
    double worst = 0, total = 0;
    uint64_t wrong_eye = 0, counted = 0;
    for (uint32_t y = 4; y + 4 < out_eye_h; y += 5)
      for (uint32_t x = 0; x < out_eye_w * 2; x += 5) {
        const uint32_t eye = x >= out_eye_w ? 1 : 0, ex = x - eye * out_eye_w;
        if (ex < 4 || ex + 4 >= out_eye_w)
          continue; // the eye's edges, where a scaler clamps
        const uint8_t *p = base + y * stride + x * 4; // BGRA, sRGB
        wrong_eye += (p[0] > 127) != (eye == 1);
        const double dx = std::fabs(p[2] / 255.0 - (ex + 0.5) / out_eye_w);
        const double dy = std::fabs(p[1] / 255.0 - (y + 0.5) / out_eye_h);
        worst = std::max(worst, std::max(dx, dy));
        total += dx + dy;
        ++counted;
      }
    CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
    CVPixelBufferRelease(buffer);
    // a gradient is smooth, so the scaler's sharpening must leave it within a few steps of 8 bits
    const double mean = counted ? total / (2.0 * counted) : 1;
    const bool ok = scaled == 2 && made_after == 0 && counted > 10000 && wrong_eye == 0 && worst < 0.03 &&
                    mean < 0.006;
    std::printf("%s: %s %ux%u per eye (after %ux%u) to %ux%u; %u eyes by MetalFX, %u scalers made for the new size, "
                "%llu pixels, mean %.4f worst %.4f off, %llu from the wrong eye\n",
                ok ? "PASSED" : "FAILED", half ? "RGBA16Float" : "BGRA8Unorm_sRGB", in_w, in_h, in_w * 5 / 4,
                in_h * 5 / 4, out_eye_w, out_eye_h, scaled, made_after, static_cast<unsigned long long>(counted), mean,
                worst, static_cast<unsigned long long>(wrong_eye));
    failures += !ok;
  }
  // SteamVR's layers come and go (its dashboard, overlays): frames of one projection layer and of two at different
  // sizes, one after another, must keep reusing the scalers they had, as making one takes longer than a frame
  {
    const auto eyes = [&](uint32_t w, uint32_t h) {
      MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB
                                                                                    width:w * 2
                                                                                   height:h
                                                                                mipmapped:NO];
      td.usage = MTLTextureUsageShaderRead;
      ComposeLayer layer;
      layer.type = MVR_LAYER_PROJECTION;
      id<MTLTexture> t = [device newTextureWithDescriptor:td];
      for (int eye = 0; eye < 2; ++eye) {
        layer.views[eye].texture = t;
        layer.views[eye].rect = {static_cast<int32_t>(eye * w), 0, static_cast<int32_t>(w), static_cast<int32_t>(h)};
      }
      return layer;
    };
    ComposeFrame one, two;
    // the larger layer alone, then under the smaller one: matched by position in the frame, every frame would
    // need its scalers made again
    one.layers.push_back(eyes(800, 880));
    two.layers.push_back(eyes(640, 704));
    two.layers.push_back(eyes(800, 880));
    two.layers.back().flags = MVR_LAYER_BLEND_ALPHA;
    uint32_t after_first = 0, scaled = 0;
    bool composed = true, overlay_scaled = false;
    for (int i = 0; i < 20 && composed; ++i) {
      CVPixelBufferRef buffer = compositor.compose(i % 2 ? two : one);
      composed = buffer != nullptr;
      if (buffer)
        CVPixelBufferRelease(buffer);
      scaled += compositor.lastUpscaledViews();
      // only the bottom layer: the blended one over it is sampled, not scaled with MetalFX
      overlay_scaled |= (i % 2) && compositor.lastUpscaledViews() != 2;
      if (i == 1)
        after_first = compositor.upscalersCreated();
    }
    const uint32_t made = compositor.upscalersCreated() - after_first;
    const bool ok = composed && made == 0 && !overlay_scaled && scaled == 20 * 2;
    std::printf("%s: layers changing for 20 frames: %u eyes by MetalFX, %u scalers made after the first two frames\n",
                ok ? "PASSED" : "FAILED", scaled, made);
    failures += !ok;
  }
  return failures ? 1 : 0;
}

// --passthrough-check: an Index's home drawn with a stereo camera picture (left half red, right half blue); the
// middle of each eye must show its own half
int passthroughCheck() {
  lighthouse::HeadsetConfig config;
  config.model = lighthouse::Model::Index;
  config.eye_width = 1440, config.eye_height = 1600;
  for (int eye = 0; eye < 2; ++eye) {
    config.distortion[eye].intrinsic_focus[0] = config.distortion[eye].intrinsic_focus[1] = 1.5f;
    config.fov[eye] = {-0.785f, 0.785f, 0.785f, -0.785f};
  }
  const DirectModel model = lighthouseModel(config, 2);
  std::vector<uint8_t> bgra;
  if (!drawTestPanel(model, 2880, 1600, bgra, true)) {
    std::printf("FAILED: drawing\n");
    return 1;
  }
  const auto at = [&](uint32_t x, uint32_t y) { return &bgra[(static_cast<size_t>(y) * 2880 + x) * 4]; };
  const uint8_t *left = at(720, 800), *right = at(2160, 800);
  const bool ok = left[2] > 200 && left[0] < 50 && right[0] > 200 && right[2] < 50;
  std::printf("%s: left eye shows (%u %u %u), right eye (%u %u %u)\n", ok ? "PASSED" : "FAILED", left[2], left[1],
              left[0], right[2], right[1], right[0]);
  return ok ? 0 : 1;
}

// --survive-check: libsurvive started in this process for a few seconds, as the Lighthouse driver starts it, and
// stopped again; what it tracks (nothing without a headset and base stations) is listed
int surviveCheck(bool simulated) {
  if (!lighthouseTrackingBuilt()) {
    std::printf("metalvrd was built without libsurvive (scripts/build-libsurvive.sh)\n");
    return 2;
  }
  auto tracking = startLighthouseTracking(simulated);
  if (!tracking) {
    std::printf("FAILED: libsurvive did not start\n");
    return 1;
  }
  std::this_thread::sleep_for(std::chrono::seconds(simulated ? 8 : 4));
  // the simulator's device (SM0) is posed, and its base stations stand where it puts them: 1 m up, which only
  // comes out so when libsurvive's +z up is turned into OpenXR's +y up
  bool device_posed = false;
  int stations_placed = 0;
  for (const auto &[name, p] : tracking->objects()) {
    std::printf("%s at %.3f %.3f %.3f m%s\n", name.c_str(), p.position.x, p.position.y, p.position.z,
                p.position_tracked ? ", tracked" : "");
    device_posed = device_posed || name == "SM0";
    stations_placed += name.rfind("LH", 0) == 0 && std::fabs(p.position.y - 1.0f) < 0.05f;
  }
  mvr_tracking_reference references[MVR_MAX_TRACKING_REFERENCES];
  const uint32_t stations = tracking->references(references);
  static const char *const names[3] = {"head", "left hand", "right hand"};
  for (int device = 0; device < 3; ++device) {
    TrackedPose p;
    const bool has = tracking->pose(device, p);
    std::printf("%s: %s\n", names[device],
                has ? (p.position_tracked ? "tracked" : "known, not seen lately") : "not there");
  }
  std::printf("base stations: %u\n", stations);
  tracking.reset();
  if (simulated && (!device_posed || stations_placed < 2 || stations < 2)) {
    std::printf("FAILED: libsurvive's simulated device and its two base stations were not all placed\n");
    return 1;
  }
  std::printf("PASSED: libsurvive started%s and stopped\n", simulated ? ", tracked its simulated device," : "");
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  @autoreleasepool {
    std::string driver_name;
    ServerOptions server_options;
    SimOptions sim;
    AlvrOptions alvr;
    RiftOptions rift;
    if (const char *port = std::getenv(MVR_PORT_ENV))
      server_options.port = static_cast<uint16_t>(std::atoi(port));
    NSString *support =
        [NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES) firstObject];
    alvr.config_dir = std::string(support.UTF8String) + "/MetalVR";

    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      const auto value = [&]() -> const char * {
        if (i + 1 >= argc) {
          usage();
          std::exit(2);
        }
        return argv[++i];
      };
      if (arg == "--driver")
        driver_name = value();
      else if (arg == "--port")
        server_options.port = static_cast<uint16_t>(std::atoi(value()));
      else if (arg == "--config-dir")
        alvr.config_dir = value();
      else if (arg == "--trust")
        alvr.trusted_clients.push_back(value());
      else if (arg == "--headset") {
        const uint32_t headset = headsetFromKey(value());
        if (headset == MVR_HEADSET_UNKNOWN) {
          std::fprintf(stderr, "--headset: %s is not a headset MetalVR knows (docs/HEADSETS.md)\n", argv[i]);
          return 2;
        }
        alvr.headset = sim.headset = headset;
      } else if (arg == "--adb")
        alvr.adb = value();
      else if (arg == "--foveated-encoding")
        alvr.foveated_encoding = sim.foveated_encoding = true;
      else if (arg == "--verbose")
        g_verbose = true;
      else if (arg == "--eye") {
        unsigned w = 0, h = 0;
        if (std::sscanf(value(), "%ux%u", &w, &h) != 2 || !w || !h || (w & 1) || (h & 1)) {
          std::fprintf(stderr, "--eye wants WxH with even numbers\n");
          return 2;
        }
        sim.eye_width = rift.eye_width = w;
        sim.eye_height = rift.eye_height = h;
      } else if (arg == "--test-scene")
        rift.test_scene = true;
      else if (arg == "--home-logo")
        rift.home_logo = value();
      else if (arg == "--ipd")
        rift.ipd = static_cast<float>(std::atof(value())) / 1000.0f;
      else if (arg == "--passthrough")
        rift.passthrough = true;
      else if (arg == "--refresh")
        rift.refresh_hz = static_cast<float>(std::atof(value()));
      else if (arg == "--pro2-mode") {
        rift.pro2_mode = std::atoi(value());
        if (rift.pro2_mode < 0 || rift.pro2_mode > 5) {
          std::fprintf(stderr, "--pro2-mode wants 0 to 5\n");
          return 2;
        }
      }
      else if (arg == "--fps")
        sim.refresh_hz = static_cast<float>(std::atof(value()));
      else if (arg == "--codec") {
        const std::string codec = value();
        if (codec == "hevc")
          sim.codec = Codec::HEVC;
        else if (codec == "h264")
          sim.codec = Codec::H264;
        else {
          usage();
          return 2;
        }
      } else if (arg == "--bitrate")
        sim.bitrate_bps = static_cast<uint64_t>(std::atof(value()) * 1e6);
      else if (arg == "--dump")
        sim.dump_path = value();
      else if (arg == "--no-animate")
        sim.animate = false;
      else if (arg == "--snapshot")
        server_options.snapshot_path = value();
      else if (arg == "--snapshot-frame")
        server_options.snapshot_frame = static_cast<uint32_t>(std::atoi(value()));
      else if (arg == "--passthrough-check")
        return passthroughCheck();
      else if (arg == "--survive-check")
        return surviveCheck(i + 1 < argc && std::string(argv[i + 1]) == "sim");
      else if (arg == "--foveation-check")
        return foveationCheck();
      else if (arg == "--upscale-check")
        return upscaleCheck();
      else if (arg == "--panel-check") {
        const std::string key = value();
        return panelCheck(key, value());
      } else if (arg == "--exit-after-session")
        server_options.exit_after_session = true;
      else if (arg == "--software-encoder")
        server_options.software_encoder = true;
      else {
        usage();
        return arg == "--help" || arg == "-h" ? 0 : 2;
      }
    }

    // a streamed headset is named for the ALVR driver; one on this Mac's own display is found by its driver
    if (alvr.headset != MVR_HEADSET_UNKNOWN && (driver_name.empty() || driver_name == "alvr")) {
      const uint32_t h = alvr.headset;
      const bool streamed = h == MVR_HEADSET_QUEST_1 || h == MVR_HEADSET_QUEST_2 || h == MVR_HEADSET_QUEST_3 ||
                            h == MVR_HEADSET_QUEST_3S || h == MVR_HEADSET_QUEST_PRO || h == MVR_HEADSET_STEAM_FRAME;
      if (!streamed) {
        std::fprintf(stderr, "--headset %s is not streamed: use --driver %s, which finds it on USB\n",
                     mvr_describe_headset(h)->key,
                     h == MVR_HEADSET_RIFT_CV1 || h == MVR_HEADSET_RIFT_S ? "rift"
                     : h == MVR_HEADSET_PSVR2                             ? "psvr2"
                                                                          : "lighthouse");
        return 2;
      }
    }

    std::unique_ptr<Driver> driver;
    if (driver_name == "rift" || driver_name == "lighthouse" || driver_name == "psvr2") {
      rift.lighthouse = driver_name == "lighthouse";
      rift.psvr2 = driver_name == "psvr2";
      driver = makeRiftDriver(rift);
      if (!driver) {
        std::fprintf(stderr, "no %s driver\n", driver_name.c_str());
        return 1;
      }
    } else if (driver_name.empty() || driver_name == "alvr") {
      [[NSFileManager defaultManager] createDirectoryAtPath:[NSString stringWithUTF8String:alvr.config_dir.c_str()]
                                withIntermediateDirectories:YES
                                                 attributes:nil
                                                      error:nil];
      driver = makeAlvrDriver(alvr);
      if (!driver && driver_name == "alvr") {
        std::fprintf(stderr, "metalvrd was built without the ALVR driver (see docs/ALVR.md)\n");
        return 1;
      }
    }
    if (!driver) {
      if (!driver_name.empty() && driver_name != "sim") {
        usage();
        return 2;
      }
      driver = makeSimDriver(sim);
    }

    const bool server_driver_needs_main_run_loop = driver->needsMainRunLoop();
    Server server(std::move(driver), server_options);
    g_server = &server;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGUSR1, onSnapshotSignal);
    const bool app = server_driver_needs_main_run_loop;
    if (!server.start())
      return 1;
    if (app) {
      // the driver puts a window on the headset's display: the main thread runs AppKit, the server a thread of its own
      [NSApplication sharedApplication];
      [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
      std::thread serving([&server] {
        server.run();
        dispatch_async(dispatch_get_main_queue(), ^{
          [NSApp stop:nil];
          // stop: takes effect after the next event
          [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                              location:NSZeroPoint
                                         modifierFlags:0
                                             timestamp:0
                                          windowNumber:0
                                               context:nil
                                               subtype:0
                                                 data1:0
                                                 data2:0]
                   atStart:YES];
        });
      });
      [NSApp run];
      serving.join();
    } else {
      server.run();
    }
    g_server = nullptr;
    logInfo("stopped");
  }
  return 0;
}
