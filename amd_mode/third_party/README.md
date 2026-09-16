# amd_mode/third_party

Quarantine for GPL-derived code (OptiScaler-DLSSNR engine pieces, used
with author wilsjo2's permission). Rules:

- Derived files live ONLY here, keep their GPL-3.0 headers, and are
  listed in `licenses/` with the full texts.
- Nothing in `amd_mode/native/` or `amd_mode/python/` may be copied
  from GPL sources; those trees stay under this repository's MIT
  licence and may only *link to / load* third_party artefacts at run
  time, or reimplement documented behaviour (design docs are
  descriptions, not code).
- See `THIRD_PARTY_NOTICES.md` at the root.
