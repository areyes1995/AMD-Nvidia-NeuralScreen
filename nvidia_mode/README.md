# NVIDIA backend

The NGX neural pipeline (RTX cards) — the program's original engine,
moved here unchanged in Phase 0.

```
nvidia_mode/
  native/      # NGX worker sources (dlss5-feed-host64.cpp, ns_forwarder,
               # frame_generation.inl, nvofa.inl, hdr_*, spout_bridge),
               # NGX headers (include/), import lib (lib/), build scripts.
               # Build artefacts (nvngx.dll, forwarder) and the NVIDIA
               # runtimes (nvngx_dlssnr.dll, nvngx_dlssg.dll) are NOT
               # committed - see .gitignore and libraries/README.md.
  python/
    params.py  # NR parameter vocabulary (profiles, ranges, clamping),
               # re-exported by settings_io for the menu/pipeline/tests.
```

Build: `nvidia_mode\native\build-host.bat` (MSVC 2022 Build Tools).
Docs: `TECHNICAL.md` ("Building the worker").
