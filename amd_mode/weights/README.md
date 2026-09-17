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

## Para el pase neuronal real (Ruta A, docs/AMD_HIP_HOSTING.md)

El worker hospeda aquí el runtime DLSS-NR on AMD. Añade en esta carpeta:

- `version.dll` — el runtime **standalone** (el de la carpeta del juego, no
  los `dlssnr_amd_pass*.dll` del pack: esos tienen la auto-inicialización
  parcheada y no arrancan solos).
- `dlssnr_on_amd_weights.bin` — los pesos (140,8 MB).
- `amd_fidelityfx_upscaler_dx12.dll` — el upscaler FSR del pack; es lo que
  dispara la red.
- `dlssnr_on_amd.ini` — con `[DlssNrOnAmd] Enabled=1` y **`UseFsrInputs=1`**
  (con 0 el motor arranca y no procesa nada, en silencio).

`NS_AMD_NR_RUNTIME` apunta a otra carpeta si prefieres. Sin estos archivos el
worker sirve passthrough y lo dice; `NS_AMD_NR=0` lo fuerza.
