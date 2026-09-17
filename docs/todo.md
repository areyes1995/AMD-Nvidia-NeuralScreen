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

## Phase 2b-prep — Real inputs, stub dispatch (done 2026-09-16)

- [x] Verified vs `dlss5-feed-host64.cpp`: feature-18 takes color + MV +
      exposure + reset + tuning; NO depth, NO jitter (zero `DLSSNR.Depth`
      refs, subrects fixed at 0)
- [x] `amd_mode/native/nr_host_full.cpp` → `amd_nr_host.exe`: DIS motion
      stats, PaperWhite adaptive exposure, reset counting, tuning record;
      `DispatchPassthrough()` frozen for the HIP swap
- [x] `tests/test_amd_full_worker.py` (passthrough + exp range + mv mean
      + reset count + tuning ride-along)
- [x] `.bin` format fully decoded (magic + count + data_start + 153
      `{u8 len, name, u64 off, u64 size}` entries, contiguous, table + data
      == file size): `amd_mode/python/dlssnr_weights.py`
- [x] `amd_mode/python/executor.py` scaffold (`PassthroughExecutor`,
      DirectML probe); `onnxruntime-directml` installed, DML provider
      ready on this machine (plan B executable, graph still missing)
- [x] `tests/test_amd_weights_exec.py` (synthetic tables + real 153-tensor
      pack when present + probe shape)
- [x] `docs/GPL_SOURCE_REQUEST.md`: §6 source request, final text (issue +
      DM versions), target verified (repo live, GPL-3.0, issues open)
- [x] `tools/send_gpl_request.py`: builds the prefilled issue from the doc
      (single source of truth) — browser, `--print`, or `--gh`; never posts
      on its own. `tests/test_gpl_request.py` guards the seam

## Phase 2b — Real HIP engine (pending)

### Route A — host the shipped runtime (WORKING 2026-09-16, docs/AMD_HIP_HOSTING.md)

- [x] Runtime identified: Danielblnc DLSS-NR on AMD, standalone version.dll
      proxy; kernels already compiled in `.hip_fat` (no HIP SDK needed)
- [x] `probe_dlssnr_host.exe`: it adopts our D3D12 device, queue and swapchain
- [x] Real FSR dispatch from our host (FFX API headers vendored, MIT)
- [x] `engine init ok`: wait for the runtime hooks before creating anything
      D3D12, and `UseFsrInputs=1` in its ini (both silent failures otherwise)
- [x] `dlssnr_engine.{h,cpp}` wired into `nr_host_full.cpp`: async start,
      passthrough until ready, per-frame dispatch + readback, RNSZ resize
- [x] Output returned over D5V3: A/B harness shows `byte_identical: false`,
      6 ms/job at 320x180, engine live after ~3.4 s
- [x] `tests/test_amd_hip_engine.py` (skips without the runtime installed)
- [x] `tools/ab_compare.py --wait-neural` + `docs/ab_baseline_amd_hip.json`
- [x] Live run of the full app on this machine: 7.6 -> 17.2 FPS at 2560x1440,
      neural pass on, clicks verified (click-through is menu-state, by design)
- [x] Network at WORK resolution: the runtime takes its colour from the
      dispatch OUTPUT, so FSR now outputs at work res and our compute pass
      scales up - the scale slider went from useless to 20/14/9 ms
- [x] SHM/OUTS channels accepted (send 14.2 -> 2.2 ms, recv 92 -> 30 ms), the
      engine writes straight into the output section
- [x] Clean exit: the worker leaves without running the hosted runtime's
      teardown (it was failing fast, 0xC0000409, on a normal session end)
- [ ] Next for FPS: DDA capture inside the worker (grab 5 ms + guides 7 ms +
      show 10 ms are all still Python-side per frame)
- [ ] Not shippable as-is (third-party runtime + NVIDIA-derived weights)

### Route B — own engine from the GPL source (blocked on author source)

- [ ] **SEND the issue** (one human click): `python tools/send_gpl_request.py`
      → review → «Submit new issue». Nothing else in 2b can start first.
- [ ] HIP engine source (graph + kernels) from the pack author
- [ ] HIP SDK for the `amd_nr_host` build (dynamic-load `amdhip64_7.dll`)
- [ ] Weights converter (`weights.convert_weights`, `.bin` cache)
- [ ] DDA capture + D3D12 device + swapchain present in the AMD worker
- [ ] SHM/DDA/GRAY/OUTS channels (kill the pipe cost → target 60+ FPS)
- [ ] Multipass ×3 pre-SR + matched residual composite
- [ ] DIS-flow → MV repack, flat depth, synthesized exposure
      (SUPERSEDED: MV/exposure/reset are real in nr_host_full; depth is
      not an input of this interface — do not synthesise it)
- [ ] DXBC Gather/ResolveCS lighting integration
- [ ] RNSZ live resize + watchdog/auto-revive parity
- [ ] Perf targets: ~15–16 ms/job @720p, ~31 ms cold (field data)

## Phase 3 — Integration (pending)

- [ ] `nr_backend` in menu (processing page) + config documentation
- [ ] Activate guarded actions on AMD (toggle, Boost, scales, profiles)
- [ ] AMD presets + per-pass overrides UI
- [ ] `System using:` live backend state (`neural pass: on (AMD)`)
- [ ] Frame Generation on AMD: deferred (separate project, needs Streamline)

## Phase 4 — Validation & release (in progress)

- [x] `tools/ab_compare.py` A/B harness (pan/dark/cut scenes, FPS +
      integrity + telemetry) + `docs/ab_baseline_amd.json`
      (515–558 FPS pipe round-trip, byte-identical, exp/mv/reset OK)
- [ ] NVIDIA baseline (`ab_baseline_nvidia.json`, needs NVIDIA machine)
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
