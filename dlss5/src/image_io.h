// SPDX-License-Identifier: MIT
//
// Image files for the offscreen test: PNG/JPEG/BMP through WIC (8-bit RGBA), and PFM
// (portable float map) for depth (1 channel) and motion vectors (3 channels, R=x G=y).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dsf_dlss5 {

struct Rgba8Image {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> pixels;  // RGBA, top row first
};

struct FloatImage {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t channels = 0;       // 1 or 3
  std::vector<float> pixels;   // top row first
};

bool LoadImageRgba8(const std::wstring& path, Rgba8Image* image, std::string* error);
bool SavePngRgba8(const std::wstring& path, const Rgba8Image& image, std::string* error);
bool LoadPfm(const std::wstring& path, FloatImage* image, std::string* error);
bool SavePfm(const std::wstring& path, const FloatImage& image, std::string* error);

}  // namespace dsf_dlss5
