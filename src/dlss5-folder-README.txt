DSF Enhanced: the DLSS 5 folder (experimental)

This folder goes next to Driver.exe, as "dlss5". DSF Enhanced finds the helper and NVIDIA's
runtime here (HelperPath and HelperRuntime in dsf_enhanced.ini).

  dsf-dlss5-helper.exe            the 64-bit helper that runs DLSS 5 for the 32-bit game
  NVIDIA-RTX-SDKs-LICENSE.txt     NVIDIA's license for the part of the helper it covers (below)
  nvngx_dlssnr.dll                NOT included: you put it here yourself

nvngx_dlssnr.dll comes from NVIDIA's RTX Remix 1.6.1 release:

  https://github.com/NVIDIAGameWorks/rtx-remix/releases/tag/remix-1.6.1
  file:    rtx-remix-for-x64-games-release-remix-1.6.1.zip
  inside:  nvngx_dlssnr.dll, at the top level of the zip (version 310.9.16.0, 266,424,944 bytes)
  SHA-256: 2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f

The helper refuses any other copy: it checks the SHA-256, the NVIDIA Corporation signature and
the version before it loads it.

Turning DLSS 5 on and off, requirements and known limitations: README.md, "DLSS 5 (experimental)".

Licenses

dsf-dlss5-helper.exe is built from DSF Enhanced's MIT-licensed source (dlss5/ in the
repository). It also contains NVIDIA's NGX loader (nvsdk_ngx_s.lib from the NVIDIA DLSS SDK
310.9.16, https://github.com/NVIDIA/DLSS), linked in object code form. That part is NVIDIA
software under the NVIDIA RTX SDKs license in NVIDIA-RTX-SDKs-LICENSE.txt, not under the MIT
license. NVIDIA, RTX and DLSS are trademarks of NVIDIA Corporation. This project is not
sponsored or endorsed by NVIDIA.
