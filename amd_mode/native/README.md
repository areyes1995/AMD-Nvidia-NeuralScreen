# amd_mode/native (Phase 2a: echo worker; 2b: HIP engine)

`nr_host_echo.cpp` is the Phase 2a stand-in: the full stdin/stdout
protocol of `nvidia_mode/native` (D5V3 header, FRM1 in, OUT1 out, every
channel ack, RNSZ, MOTS tracking) with the neural pass as a passthrough
(input colour comes back identical). It proves the Python side drives an
AMD binary end to end before any HIP exists; every refusal is honest
(SACK/GAK/OAK2/WACK/DACK/WGAK ok=0 -> Python falls back to pipe/dxcam/
pygame, exactly like the NVIDIA worker's own fallbacks).

Build: `build-amd.bat` (MSVC 2022, CRT only). Output `amd_nr_host.exe`
is gitignored. Selected with `NS_NR_BACKEND=amd` (or config
`nr_backend`), or automatically when it is the only worker that can run.

Phase 2b replaces the passthrough with the real host: a D3D12
desktop-capture host that runs the DlssNr multipass (pre-SR, ping-pong
`base -> A -> B -> A`, matched residual composite) over HIP kernels.

Inputs the desktop cannot provide (depth, motion vectors, exposure,
jitter) are synthesised: DIS flow repacked as MV (the MOTS channel),
flat depth, synthesized exposure — same caveat as desktop FG.

Build requirements (Phase 2): MSVC 2022, AMD HIP SDK (`amdhip64_7.dll`
ships with current AMD drivers), D3D12 Agility SDK.
