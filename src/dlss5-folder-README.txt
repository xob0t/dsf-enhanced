DSF Enhanced: the DLSS 5 and DLAA folder (experimental)

This folder goes next to Driver.exe, as "dlss5". DSF Enhanced finds the helper and NVIDIA's
runtimes here (HelperPath and HelperRuntime in dsf_enhanced.ini).

  dsf-dlss5-helper.exe            the 64-bit helper that runs DLSS 5 and DLAA for the 32-bit game
  NVIDIA-RTX-SDKs-LICENSE.txt     NVIDIA's license for the part of the helper it covers (below)
  nvngx_dlssnr.dll                NOT included: you put it here yourself, for DLSS 5 ([DLSS5])
  nvngx_dlss.dll                  NOT included: you put it here yourself, for DLAA ([DLAA] Enable=1)

Either DLL works without the other: DLAA needs only nvngx_dlss.dll (any RTX GPU), DLSS 5 only
nvngx_dlssnr.dll (an RTX 50 GPU).

nvngx_dlssnr.dll comes from NVIDIA's RTX Remix 1.6.1 release:

  https://github.com/NVIDIAGameWorks/rtx-remix/releases/tag/remix-1.6.1
  file:    rtx-remix-for-x64-games-release-remix-1.6.1.zip
  inside:  nvngx_dlssnr.dll, at the top level of the zip (version 310.9.16.0, 266,424,944 bytes)
  SHA-256: 2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f

nvngx_dlss.dll (for DLAA) comes from NVIDIA's DLSS SDK, release v310.9.16 (use "rel", not "dev"):

  https://github.com/NVIDIA/DLSS/tree/v310.9.16/lib/Windows_x86_64/rel
  file:    nvngx_dlss.dll (version 310.9.16.0, 58,956,912 bytes)
  SHA-256: d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e

The helper refuses any other copy of either file: it checks the SHA-256, the NVIDIA Corporation
signature and the version before it loads it.

Turning DLSS 5 and DLAA on and off, requirements and known limitations: README.md, "DLSS 5
(experimental)" and "DLAA (experimental)".

Licenses

dsf-dlss5-helper.exe is built from DSF Enhanced's MIT-licensed source (dlss5/ in the
repository). It also contains NVIDIA's NGX loader (nvsdk_ngx_s.lib from the NVIDIA DLSS SDK
310.9.16, https://github.com/NVIDIA/DLSS), linked in object code form. That part is NVIDIA
software under the NVIDIA RTX SDKs license in NVIDIA-RTX-SDKs-LICENSE.txt, not under the MIT
license. NVIDIA, RTX and DLSS are trademarks of NVIDIA Corporation. This project is not
sponsored or endorsed by NVIDIA.
