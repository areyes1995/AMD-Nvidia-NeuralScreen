# amd_mode/weights (BYO - bring your own)

Drop your files here; they are never committed (gitignored):

- `nvngx_dlssnr.dll` — your own copy (original NVIDIA-signed 310.8 or
  the ShortFuse cross-gen 310.8). Verified by SHA-256, then converted
  once (Phase 2) into the `.bin` below.
- `dlssnr_on_amd_weights.bin` — converted weights cache (Phase 2;
  alternatively a prebuilt you trust, same verification).

See `amd_mode/python/weights.py` (`find_runtime`, `verify_runtime`).
Where to get the DLL: DLSS Swapper, NVIDIA/DLSS GitHub
(`lib/Windows_x86_64/rel/`), or the ShortFuse pinned attachment for
older cards (see OptiScaler `INSTALL-DLSSNR.md`).
