// SPDX-License-Identifier: MIT
//
// A small CPU ray-traced street scene with exact guides: colour, hardware depth and
// screen-space motion vectors for a moving camera, so the DLSS 5 pass can be fed a real
// temporal sequence without a game. Conventions match what the helper expects from the
// game (research/dlss5-helper.md, "Inputs"):
//  - colour is display-encoded (sRGB) SDR in [0,1];
//  - depth is z/w of a D3D-style left-handed projection, 0 at the near plane, 1 at far;
//  - motion is previous minus current position, in pixels, +x right, +y down, without
//    jitter;
//  - the sample of pixel (x, y) is taken at (x + 0.5 - jitterX, y + 0.5 - jitterY).

#pragma once

#include <cstdint>
#include <vector>

namespace synthetic {

struct Frame {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<float> color;   // RGBA
  std::vector<float> depth;   // 1 channel
  std::vector<float> motion;  // RG
};

// frame is the index along the camera path; jitter is in pixels.
void Render(uint32_t width, uint32_t height, int frame, float jitterX, float jitterY, Frame* out);

// Halton (2, 3) sample minus 0.5, the jitter sequence Remix and the DLSS samples use.
void HaltonJitter(int index, float* x, float* y);

}  // namespace synthetic
