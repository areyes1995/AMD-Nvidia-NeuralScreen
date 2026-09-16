"""Pin the 10-bit SDR capture contract from issue #86.

The reporter's log alternates DDA source frames between BGRA8 and FP16. Each
change rebuilds the cross-API bridge and produces visible flicker. This host
cannot force the display driver into that condition, so the test verifies the
load-bearing API contract: SDR asks IDXGIOutput5 for BGRA8 only, and falls back
to legacy DuplicateOutput only when Output5 is unavailable or refuses it.

Run: runtime\\python.exe tests\\test_capture_format_stability.py
"""
from __future__ import annotations

import sys
from pathlib import Path


BASE = Path(__file__).resolve().parent.parent
SOURCE = (BASE / "nvidia_mode" / "native" / "dlss5-feed-host64.cpp").read_text(encoding="utf-8")


def main() -> int:
    failures = []
    begin = SOURCE.index("static bool OpenDda")
    end = SOURCE.index("// The phase profiler", begin)
    dda = SOURCE[begin:end]

    # One advertised SDR format is a format contract: DXGI converts any
    # 10-bit/FP16 scan-out to it before AcquireNextFrame returns the surface.
    pin = "const DXGI_FORMAT sdr_formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM};"
    if pin not in dda:
        failures.append("OpenDda no longer pins SDR duplication to BGRA8")
    if "g_dda_hdr_mode ? hdr_formats : sdr_formats" not in dda:
        failures.append("the SDR format list is not selected when HDR compatibility is off")
    if "DuplicateOutput1(g_dda_d11, 0, format_count, formats, &g_dda_dup)" not in dda:
        failures.append("OpenDda does not use DuplicateOutput1 for the selected format list")
    if "SDR capture pinned to BGRA8 (stable across 10-bit scan-out)" not in dda:
        failures.append("the stable SDR path has no diagnostic for future issue reports")

    # Legacy DDA is still a valid recovery path; it must sit after the
    # Output5 attempt, not replace it on a normal Windows 10+ machine.
    try:
        output5 = dda.index("DuplicateOutput1(")
        legacy = dda.index("output1->DuplicateOutput(")
        if output5 > legacy:
            failures.append("legacy DuplicateOutput runs before the stable Output5 path")
    except ValueError:
        failures.append("one of the DDA duplication paths is missing")

    # A genuinely legacy/driver-refused format change still rebuilds only the
    # bridge. Removing that guard would revive the old full DDA reopen storm.
    stage = SOURCE[SOURCE.index("static StageResult StageCapturedFrame"):]
    if "CloseCaptureBridge();\n            return StageResult::FormatChanged;" not in stage:
        failures.append("a legacy format change no longer tears down only the bridge")

    for failure in failures:
        print("FAIL:", failure)
    if failures:
        return 1
    print("OK: SDR DDA pins BGRA8 across 10-bit scan-out; legacy fallback stays bounded")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
