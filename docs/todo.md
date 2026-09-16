# AMD backend — TODO

Legend: `[x]` done, `[ ]` pending. Backend selection: `NS_NR_BACKEND` /
`nr_backend` (`auto` / `nvidia` / `amd`).

## Phase 0 — Restructure (done)

- [x] `native/` → `nvidia_mode/native/` (history preserved), all
      references updated (paths, launchers, packager, tests, docs)
- [x] `nvidia_mode/python/params.py`: NR vocabulary moved out of
      `settings_io` (shim re-exports, zero behaviour change)
- [x] `amd_mode/` skeleton: `python/` (params mapping, BYO weights),
      `native/`, `weights/`, `third_party/` (GPL quarantine)
- [x] `THIRD_PARTY_NOTICES.md` (OptiScaler GPL-3.0 + permission,
      RenoDX/FFX/XeSS, NVIDIA runtimes, ShortFuse)
- [x] `System using:` vendor detection + gating + menu label
- [x] Degraded control window (plain, fitted menu, monitor-Hz pacing)

## Phase 1 — Research (done)

- [x] Upstream survey: no public HIP source (all branches NVIDIA-side)
- [x] `.bin` container decoded (`DLSSNRW1`, ~153 named tensors)
- [x] HIP runtime validated (`amdhip64_7.dll`, 1 device, memcpy OK)
- [x] D3D12↔HIP interop API present (external memory + semaphores)
- [x] Field evidence (RE Requiem install): input contract, lifecycle,
      recovery policy, BYO weights, reusable DXBC lighting shaders
- [x] MSVC 2022 Build Tools installed (was missing)

## Phase 2a — Echo worker (done)

- [x] `amd_mode/native/nr_host_echo.cpp` + `build-amd.bat` → `amd_nr_host.exe`
- [x] Full protocol parity (header/FRM1/OUT1/SHMI/RNSZ/MOTS + honest
      refusals), byte-exact structs, `_O_BINARY` pipes
- [x] Backend plumbing (`worker_target`, selector, restart paths)
- [x] Live proof on RX 9070 XT: NR ON ~25 FPS, 3000+ frames, 0 restarts
- [x] `tests/test_amd_backend.py` (selector matrix, params, weights)

## Phase 2b — Real HIP engine (pending, blocked on author source)

- [ ] HIP engine source (graph + kernels) from the pack author
- [ ] HIP SDK for the `amd_nr_host` build (dynamic-load `amdhip64_7.dll`)
- [ ] Weights converter (`weights.convert_weights`, `.bin` cache)
- [ ] DDA capture + D3D12 device + swapchain present in the AMD worker
- [ ] SHM/DDA/GRAY/OUTS channels (kill the pipe cost → target 60+ FPS)
- [ ] Multipass ×3 pre-SR + matched residual composite
- [ ] DIS-flow → MV repack, flat depth, synthesized exposure
- [ ] DXBC Gather/ResolveCS lighting integration
- [ ] RNSZ live resize + watchdog/auto-revive parity
- [ ] Perf targets: ~15–16 ms/job @720p, ~31 ms cold (field data)

## Phase 3 — Integration (pending)

- [ ] `nr_backend` in menu (processing page) + config documentation
- [ ] Activate guarded actions on AMD (toggle, Boost, scales, profiles)
- [ ] AMD presets + per-pass overrides UI
- [ ] `System using:` live backend state (`neural pass: on (AMD)`)
- [ ] Frame Generation on AMD: deferred (separate project, needs Streamline)

## Phase 4 — Validation & release (pending)

- [ ] Quality A/B vs NVIDIA worker (Before/after wipe harness)
- [ ] Perf table (eval ms vs MPix, per TECHNICAL.md method)
- [ ] Soak/stability runs + timeout/recovery parity with field spec
- [ ] `build_release_zip.py` + `autocheck.py` entries for the AMD worker
- [ ] Docs: README/TECHNICAL AMD backend section (EN+RU)
- [ ] Release packaging: prebuilt `amd_nr_host.exe` in the archive

## Blockers (right now)

1. HIP engine source/graph from the pack author — nothing in 2b
   starts without it (orchestration spec exists, execution core doesn't).
2. Nothing else: toolchain, runtime, weights donor and plumbing are ready.
