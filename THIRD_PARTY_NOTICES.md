# Third-party notices

## OptiScaler-DLSSNR-PreSR-Multipass (AMD backend design source)

- Source: https://github.com/wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass
- Licence: **GPL-3.0** (see `amd_mode/third_party/licenses/` when derived
  files land there).
- Used **with the author's (wilsjo2) permission**, granted 2026-09-16,
  for the AMD backend (`amd_mode/`): DlssNr multipass design
  (pre-SR ping-pong, matched residual, per-pass profiles), the `[DlssNr]`
  parameter schema and the BYO runtime model (pinned SHA-256 table).
- Derived code stays under `amd_mode/third_party/` with GPL headers;
  `amd_mode/native/` and `amd_mode/python/` are clean-room MIT and only
  consume its documented behaviour. Upstream credits (OptiScaler,
  Dagherbou's Neural Rendering fork, RenoDX colour work, FidelityFX,
  XeSS) ride along in the same folder.

## NVIDIA runtimes (shipped unmodified, research use, takedown on request)

- `nvngx_dlssnr.dll` (leaked 310.8.0), `nvngx_dlssg.dll` (public
  310.9.1.0 redistributable): NVIDIA's property, see README notice.
- ShortFuse cross-generation 310.8 compatibility runtime (RTX 20/30/40):
  pinned hash `E67DEE20…`, source: RenoDX Discord (see OptiScaler
  `INSTALL-DLSSNR.md`); Authenticode invalid by design for that hash.
