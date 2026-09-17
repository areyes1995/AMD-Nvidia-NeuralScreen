# AMD backend (Phase 2b: the neural pass runs)

HIP neural pipeline for AMD cards. `native/amd_nr_host.exe` (built by
`build-amd.bat`, MSVC 2022) speaks the full worker protocol —
`NS_NR_BACKEND=amd` (or auto, when it is the only worker that can run)
drives the complete pipeline on AMD: capture → guides → worker →
fullscreen overlay, screenshots, recording, RNSZ. Every channel refusal
is honest (pipe/dxcam/pygame fallbacks).

The neural pass itself is real when the DLSS-NR HIP runtime is installed
in `weights/` (BYO, see its README): `native/dlssnr_engine.cpp` hosts
that runtime inside the worker and dispatches the network per frame —
6 ms/job at 320x180, 12 ms at 720p on an RX 9070 XT. Without it the
worker serves byte-identical passthrough and says so. The how and the
why are in `docs/AMD_HIP_HOSTING.md`; Python does not change either way.

```
amd_mode/
  native/        # Phase 2: D3D12+HIP NR host (same protocol as nvidia_mode)
  python/
    params.py    # DlssNr multipass mapping (nvidia_to_dlssnr)
    weights.py   # BYO weights: verify + cache, never committed
  weights/       # user-supplied nvngx_dlssnr.dll / dlssnr_on_amd_weights.bin
  third_party/   # GPL-derived code only, with its own licenses
```

## BYO weights (bring your own)

Same model as OptiScaler's `INSTALL-DLSSNR.md` and our
`nvidia_mode/native/libraries/`: the neural runtime/weights are never
shipped and never committed.

1. Drop your `nvngx_dlssnr.dll` (original NVIDIA-signed 310.8 for
   RDNA4-class cards, or the ShortFuse cross-gen 310.8 for older ones)
   into `amd_mode/weights/`.
2. `amd_mode/python/weights.py` verifies its SHA-256 against the pinned
   table and, from Phase 2 on, converts it once into
   `dlssnr_on_amd_weights.bin` beside it.
3. The HIP worker loads only the cached `.bin`.

`amd_mode/weights/*.bin`, `*.dll` and `*.part` are gitignored.

## Scheme

`amd_mode/python/params.py` mirrors `nvidia_mode/python/params.py`:
the menu keeps speaking sliders+style, and `nvidia_to_dlssnr()` emits
the `[DlssNr]` multipass description (Passes 1–3, WorkingScale,
RunBeforeSR, per-pass Style/Intensity/LocalStructure/LocalTone/
SkinStructure/AutoMask, AmdModelScale, AmdNeuralLighting).

## Phase 1 findings (2026-09-16, RX 9070 XT)

- **No public HIP source**: all upstream branches are NVIDIA-side; the
  AMD adaptation exists only as local binaries. Engine source must come
  from the pack author.
- **`.bin` decoded**: `DLSSNRW1` + count + `{u8 len, name, u64 offset,
  u64 size}` × ~153 named tensors (`blockN.layerM.layer`), dense float
  blobs. Our loader can parse it; dtype/layout per tensor is Phase 2.
- **HIP runtime works**: `amdhip64_7.dll` ships with the AMD driver —
  1 device, malloc/memcpy roundtrip OK via ctypes.
- **Zero-copy path viable**: `hipImportExternalMemory`,
  `hipExternalMemoryGetMappedBuffer` + external semaphores all
  exported (12/12 core APIs too). DDA texture → HIP without CPU copy.
- **Blocked**: no MSVC Build Tools on this machine — required before
  any Phase 2 native compile.

## Runtime independence (verified, RE Requiem install)

- Game folder has **no `nvngx_dlssnr.dll`** — the AMD path runs without
  the original driver. It was a one-time donor only: `_storage_` keeps
  a ShortFuse cross-gen 310.8 copy (hash `E67DEE20…`) that the v0.2.14
  standalone proxy used to build the `.bin`.
- Runtime file set for NR: `dxgi.dll` (OptiScaler host, self-contained
  `DlssNr_Dx12` engine) + `dlssnr_on_amd_weights.bin` +
  `experimental_lighting/*.cso` + `OptiScaler.ini`. The pass DLLs are
  dev harnesses; `dlss-enabler-headless.dll` is DLSS-G only (688
  `DLSSG_*` exports, zero NR) and ships `.DISABLED` — irrelevant to NR.
- Perf targets (RX 9070 XT, 1280x720, history on, zero-copy):
  ~15–16 ms/job steady, ~31 ms first jobs; self-check "pre-block zero
  ~0.05% (healthy)".
- Staging contract (harness log): colour/motion/depth + exposure,
  `residual on`, inline same-frame zero-copy interop.
- Extra sections in the local ini (`[AmdLook]`, `[AmdRtgi]`) are
  bundled extras, out of NR scope.

## Attribution

Engine design: wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass (GPL-3.0, used
with the author's permission), itself built on OptiScaler, Dagherbou's
Neural Rendering fork and RenoDX colour work. See
`THIRD_PARTY_NOTICES.md` at the root and `third_party/licenses/`.
Derived code stays under `third_party/` under its own licence headers.
