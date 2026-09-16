# AMD backend (Phase 1: skeleton)

HIP neural pipeline for AMD cards. Today this folder is the contract
Phase 2 will implement; the program runs degraded on AMD until the HIP
worker lands.

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

## Attribution

Engine design: wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass (GPL-3.0, used
with the author's permission), itself built on OptiScaler, Dagherbou's
Neural Rendering fork and RenoDX colour work. See
`THIRD_PARTY_NOTICES.md` at the root and `third_party/licenses/`.
Derived code stays under `third_party/` under its own licence headers.
