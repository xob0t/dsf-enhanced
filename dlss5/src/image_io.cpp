// SPDX-License-Identifier: MIT

#include "image_io.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <utility>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace dsf_dlss5 {

using Microsoft::WRL::ComPtr;

namespace {

ComPtr<IWICImagingFactory> Factory() {
  static bool initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) || true;
  (void)initialized;
  ComPtr<IWICImagingFactory> factory;
  CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
  return factory;
}

}  // namespace

bool LoadImageRgba8(const std::wstring& path, Rgba8Image* image, std::string* error) {
  ComPtr<IWICImagingFactory> factory = Factory();
  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICBitmapFrameDecode> frame;
  ComPtr<IWICFormatConverter> converter;
  if (!factory || FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) ||
      FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter)) ||
      FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
    *error = "could not decode the image";
    return false;
  }
  UINT width = 0;
  UINT height = 0;
  converter->GetSize(&width, &height);
  image->width = width;
  image->height = height;
  image->pixels.resize(static_cast<size_t>(width) * height * 4);
  if (FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(image->pixels.size()), image->pixels.data()))) {
    *error = "could not read the pixels";
    return false;
  }
  return true;
}

bool SavePngRgba8(const std::wstring& path, const Rgba8Image& image, std::string* error) {
  ComPtr<IWICImagingFactory> factory = Factory();
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> frame;
  // The PNG encoder takes BGRA.
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  std::vector<uint8_t> bgra(image.pixels);
  for (size_t i = 0; i + 3 < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
  if (!factory || FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
      FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
      FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) || FAILED(encoder->CreateNewFrame(&frame, nullptr)) ||
      FAILED(frame->Initialize(nullptr)) || FAILED(frame->SetSize(image.width, image.height)) ||
      FAILED(frame->SetPixelFormat(&format)) || format != GUID_WICPixelFormat32bppBGRA ||
      FAILED(frame->WritePixels(image.height, image.width * 4, static_cast<UINT>(bgra.size()), bgra.data())) ||
      FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
    *error = "could not encode the PNG";
    return false;
  }
  return true;
}

bool LoadPfm(const std::wstring& path, FloatImage* image, std::string* error) {
  FILE* file = _wfopen(path.c_str(), L"rb");
  if (file == nullptr) {
    *error = "could not open the PFM";
    return false;
  }
  char kind[3] = {};
  unsigned width = 0;
  unsigned height = 0;
  float scale = 0.0f;
  bool ok = std::fscanf(file, "%2s %u %u %f", kind, &width, &height, &scale) == 4 && std::fgetc(file) != EOF;
  const uint32_t channels = std::strcmp(kind, "Pf") == 0 ? 1 : (std::strcmp(kind, "PF") == 0 ? 3 : 0);
  ok = ok && channels != 0 && width != 0 && height != 0;
  if (ok) {
    image->width = width;
    image->height = height;
    image->channels = channels;
    image->pixels.resize(static_cast<size_t>(width) * height * channels);
    const size_t row = static_cast<size_t>(width) * channels;
    // PFM stores the bottom row first.
    for (unsigned y = 0; ok && y < height; ++y) {
      ok = std::fread(image->pixels.data() + (height - 1 - y) * row, sizeof(float), row, file) == row;
    }
    if (ok && scale > 0.0f) {
      // Positive scale means big-endian data.
      for (float& value : image->pixels) {
        uint32_t bits;
        std::memcpy(&bits, &value, 4);
        bits = _byteswap_ulong(bits);
        std::memcpy(&value, &bits, 4);
      }
    }
  }
  std::fclose(file);
  if (!ok) *error = "not a valid PFM";
  return ok;
}

bool SavePfm(const std::wstring& path, const FloatImage& image, std::string* error) {
  FILE* file = _wfopen(path.c_str(), L"wb");
  if (file == nullptr) {
    *error = "could not create the PFM";
    return false;
  }
  std::fprintf(file, "%s\n%u %u\n-1.0\n", image.channels == 1 ? "Pf" : "PF", image.width, image.height);
  const size_t row = static_cast<size_t>(image.width) * image.channels;
  for (uint32_t y = 0; y < image.height; ++y) {
    std::fwrite(image.pixels.data() + (image.height - 1 - y) * row, sizeof(float), row, file);
  }
  std::fclose(file);
  return true;
}

}  // namespace dsf_dlss5
