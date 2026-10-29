// SPDX-License-Identifier: MIT
//
// Minimal declarations for NVIDIA's DLSS 3D-Guided Neural Rendering feature ("DLSS 5",
// NGX feature DLSSNR). NVIDIA's public DLSS SDK (310.9.16) does not ship a header for it.
// These are written for this project from public, openly licensed sources:
//
//  - dxvk-remix (zlib), src/dxvk/rtx_render/rtx_ngx_wrapper.cpp and
//    rtx_dlss_neural_rendering.cpp: which parameters exist, their meaning and the values
//    Remix passes (Preset B at create; Reset, DepthInverted=0, Enabled=1, Intensity,
//    LocalToneStrength, LocalStructureStrength, Style, UseAutoMask, MaskBlendMode,
//    SkinStructureStrength, MVecScale, the five subrects, and Jitter.Offset.X/Y).
//  - The NGX feature id: the public SDK's NVSDK_NGX_Feature_Reserved18 (= 18).
//  - The parameter name strings, which are the keys the signed runtime nvngx_dlssnr.dll
//    reads. They are the same strings the open-source RenoDX dlss5 add-on (MIT) sets, and
//    they appear verbatim in the runtime's string table.
//
// research/dlss5-helper.md has the full provenance notes. No NVIDIA header text is
// reproduced here.

#pragma once

#include "nvsdk_ngx_defs.h"

namespace dlssnr {

constexpr NVSDK_NGX_Feature kFeature = NVSDK_NGX_Feature_Reserved18;

// Values for kHintRenderPreset. Remix creates the feature with preset B, which it
// describes as "the lightweight network", independent of the appearance model (Style).
enum Preset : int {
  kPresetDefault = 0,
  kPresetA = 1,
  kPresetB = 2,
  kPresetC = 3,
};

// How the application control mask and the runtime's automatic character mask combine
// when both are used. Remix uses Min with the auto mask and None without it.
enum MaskBlendMode : int {
  kMaskBlendNone = 0,
  kMaskBlendMean = 1,
  kMaskBlendMin = 2,
  kMaskBlendMax = 3,
};

// Capability parameters (read from NVSDK_NGX_D3D12_GetCapabilityParameters).
constexpr const char kAvailable[] = "DLSSNR.Available";
constexpr const char kNeedsUpdatedDriver[] = "DLSSNR.NeedsUpdatedDriver";
constexpr const char kMinDriverVersionMajor[] = "DLSSNR.MinDriverVersionMajor";
constexpr const char kMinDriverVersionMinor[] = "DLSSNR.MinDriverVersionMinor";
constexpr const char kFeatureInitResult[] = "DLSSNR.FeatureInitResult";

// Creation parameters (unsigned int, unsigned int, int).
constexpr const char kWidth[] = "DLSSNR.Width";
constexpr const char kHeight[] = "DLSSNR.Height";
constexpr const char kHintRenderPreset[] = "DLSSNR.Hint.Render.Preset";

// Evaluation resources. On D3D12 each is an ID3D12Resource*.
constexpr const char kColor[] = "DLSSNR.Color";
constexpr const char kOutput[] = "DLSSNR.Output";
constexpr const char kMVec[] = "DLSSNR.MVec";
constexpr const char kDepth[] = "DLSSNR.Depth";
constexpr const char kControlMask[] = "DLSSNR.ControlMask";

// Subrects (unsigned int) for each resource: <prefix>SubrectBaseX/BaseY/Width/Height.
constexpr const char kColorSubrect[] = "DLSSNR.ColorSubrect";
constexpr const char kOutputSubrect[] = "DLSSNR.OutputSubrect";
constexpr const char kMVecSubrect[] = "DLSSNR.MVecSubrect";
constexpr const char kDepthSubrect[] = "DLSSNR.DepthSubrect";
constexpr const char kControlMaskSubrect[] = "DLSSNR.ControlMaskSubrect";

// Evaluation controls.
constexpr const char kReset[] = "DLSSNR.Reset";                        // int
constexpr const char kDepthInverted[] = "DLSSNR.DepthInverted";        // int
constexpr const char kEnabled[] = "DLSSNR.Enabled";                    // int
constexpr const char kUICorrection[] = "DLSSNR.UICorrection";          // int
constexpr const char kIntensity[] = "DLSSNR.Intensity";                // float, 0..1
constexpr const char kLocalToneStrength[] = "DLSSNR.LocalToneStrength";            // float, 0..1
constexpr const char kLocalStructureStrength[] = "DLSSNR.LocalStructureStrength";  // float, 0..1
constexpr const char kStyle[] = "DLSSNR.Style";                        // unsigned int, model 0..2
constexpr const char kUseAutoMask[] = "DLSSNR.UseAutoMask";            // int
constexpr const char kMaskBlendMode[] = "DLSSNR.MaskBlendMode";        // int, MaskBlendMode
constexpr const char kSkinStructureStrength[] = "DLSSNR.SkinStructureStrength";    // float, 0..1
constexpr const char kMVecScaleX[] = "DLSSNR.MVecScaleX";              // float
constexpr const char kMVecScaleY[] = "DLSSNR.MVecScaleY";              // float

}  // namespace dlssnr
