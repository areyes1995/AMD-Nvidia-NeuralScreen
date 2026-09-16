# amd_mode/native (Phase 2)

Home of the HIP NR host worker: a D3D12 desktop-capture host that runs
the DlssNr multipass (pre-SR, ping-pong `base -> A -> B -> A`, matched
residual composite) over HIP kernels and speaks the same stdin/stdout
protocol as `nvidia_mode/native` (D5V3 header, FRM1 in, OUT1 out, RNSZ,
DDA1, WNDO) so the Python side drives both backends unchanged.

Inputs the desktop cannot provide (depth, motion vectors, exposure,
jitter) are synthesised: DIS flow repacked as MV (the MOTS channel),
flat depth, synthesized exposure — same caveat as desktop FG.

Build requirements (Phase 2): MSVC 2022, AMD HIP SDK (`amdhip64_7.dll`
ships with current AMD drivers), D3D12 Agility SDK.
