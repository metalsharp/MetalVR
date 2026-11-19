// MetalFX spatial upscaling of the eyes: whether the application is asked to render its eyes smaller, so that
// metalvrd scales them up to the headset's size with MetalFX (daemon/compositor.mm) instead of a bilinear sample.
//
// METALVR_METALFX_SPATIAL decides when it is set: 0 (or off, false, no) turns it off; on (true, yes) turns it on
// with Mullion's factor, or 2; a number is the factor itself (1.5 renders each eye at 1/1.5 of its width and
// height). Unset, MetalVR follows Mullion's own MetalFX setting, d3d11.metalSpatialUpscale and
// d3d11.metalSpatialUpscaleFactor, in the file MULLION_CONFIG_FILE names and in MULLION_CONFIG, as MetalSharp's
// MetalFX toggle writes them, so the toggle works for VR games too.
#pragma once

#include <cstdint>
#include <string>

namespace metalvr {

struct SpatialUpscale {
  float factor = 1.0f;  // the headset's eye size over the recommended size; 1 when off
  std::string source;   // where the setting came from, for the log
};

// read once, on the first call
const SpatialUpscale &spatialUpscale();

// the recommended render size for an eye of the given size: divided by the factor, and never below 16 pixels
uint32_t upscaledRenderSize(uint32_t eye_size);

} // namespace metalvr
