# amd_mode/native (Phase 2a: echo worker; 2b-prep: full-input worker; 2b: HIP)

`nr_host_echo.cpp` is the Phase 2a stand-in: the full stdin/stdout
protocol of `nvidia_mode/native` (D5V3 header, FRM1 in, OUT1 out, every
channel ack, RNSZ, MOTS tracking) with the neural pass as a passthrough.
`nr_host_full.cpp` is the Phase 2b-prep worker and the current
`amd_nr_host.exe`: same protocol, but every neural input the NGX path
uses is now real — DIS motion field from the MOTS channel, PaperWhite
adaptive exposure from the colour frame, per-frame resets, tuning params
from the header. Verified against `dlss5-feed-host64.cpp`: the feature-18
DLSSNR interface takes no depth (`DLSSNR.Depth` has zero references) and
no jitter (subrects fixed at 0). Only `DispatchPassthrough()` still needs
swapping for the HIP graph call; its `Dispatch` record signature is frozen
for Phase 2b.

Build: `build-amd.bat` (MSVC 2022, CRT only). Output `amd_nr_host.exe`
is gitignored. Selected with `NS_NR_BACKEND=amd` (or config
`nr_backend`), or automatically when it is the only worker that can run.

Phase 2b replaces the passthrough with the real host: a D3D12
desktop-capture host that runs the DlssNr multipass (pre-SR, ping-pong
`base -> A -> B -> A`, matched residual composite) over HIP kernels.

Inputs the desktop cannot provide natively are synthesised, never
intercepted (there is no game engine here): DIS flow repacked as MV,
PaperWhite exposure from frame stats, scene-cut resets. Depth and jitter
are not synthesised — the interface does not take them, on either side.

Build requirements (Phase 2): MSVC 2022, AMD HIP SDK (`amdhip64_7.dll`
ships with current AMD drivers), D3D12 Agility SDK.
