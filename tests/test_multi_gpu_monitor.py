"""A desktop monitor is an adapter/output pair, never an output number alone.

DXcam numbers outputs locally for each adapter.  In #88 a machine with one
monitor on a 4070 and three on a 3050 therefore treated the 3050's local
``output_idx=1`` as output 1 of the 4070.  Missing indices then fell back to
adapter 0/output 0, so every full-screen selection showed the same monitor.

This test does not require a second GPU: it gives the real capture module a
two-adapter dxcam factory and checks that the selected DeviceName reaches
``dxcam.create(device_idx=..., output_idx=...)`` intact.  It also guards the
worker-side rule: an explicit NS_OUTPUT that is not wired to its selected GPU
must refuse DDA and let the Python capture path relay frames, never silently
substitute output 0.
"""
from __future__ import annotations

import sys
import types
from pathlib import Path

BASE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(BASE))

import capture  # noqa: E402
from capture import ScreenCapture, resolve_output_idx  # noqa: E402


class _Camera:
    def __init__(self, devicename: str):
        self._output = types.SimpleNamespace(
            devicename=devicename, resolution=(2560, 1440))

    def grab(self):
        return None

    def release(self):
        pass


def main() -> int:
    failures: list[str] = []
    original_dxcam = sys.modules.get("dxcam")
    calls: list[tuple[int, int]] = []
    names = ("\\\\.\\DISPLAY1", "\\\\.\\DISPLAY2", "\\\\.\\DISPLAY3")
    fake = types.ModuleType("dxcam")
    fake.__factory = types.SimpleNamespace(outputs=[
        [types.SimpleNamespace(devicename=names[0])],
        [types.SimpleNamespace(devicename=names[1]),
         types.SimpleNamespace(devicename=names[2])],
    ])

    def create(*, device_idx=0, output_idx=None, **_kwargs):
        calls.append((device_idx, output_idx))
        return _Camera(fake.__factory.outputs[device_idx][output_idx].devicename)

    fake.create = create
    sys.modules["dxcam"] = fake
    try:
        expected_targets = [
            (0, 0, names[0]), (1, 0, names[1]), (1, 1, names[2])]
        if capture._dxcam_capture_targets() != expected_targets:
            failures.append(
                f"capture targets {capture._dxcam_capture_targets()!r} != "
                f"{expected_targets!r}")
        if resolve_output_idx(names[2]) != 2:
            failures.append("secondary adapter output did not receive a unique menu index")
        if capture._dxcam_capture_target(2) != (1, 1):
            failures.append("flat output 2 did not resolve to adapter 1/output 1")

        cap = ScreenCapture(devicename=names[2])
        try:
            if calls != [(1, 1)]:
                failures.append(f"dxcam.create calls {calls!r}, want [(1, 1)]")
            if cap.monitor_idx != 2 or cap.devicename != names[2]:
                failures.append(
                    f"capture opened {cap.monitor_idx!r}/{cap.devicename!r}, "
                    f"want 2/{names[2]!r}")
        finally:
            cap.close()
    finally:
        if original_dxcam is None:
            del sys.modules["dxcam"]
        else:
            sys.modules["dxcam"] = original_dxcam

    worker = (BASE / "nvidia_mode" / "native" / "dlss5-feed-host64.cpp").read_text(
        encoding="utf-8", errors="replace")
    start = worker.find("static IDXGIOutput *EnumCaptureOutput")
    end = worker.find("static bool OpenDda", start)
    body = worker[start:end]
    if "refusing DDA" not in body or "return nullptr;" not in body:
        failures.append("a GPU/output mismatch no longer refuses DDA explicitly")
    if "using output 0" in body:
        failures.append("a GPU/output mismatch still substitutes output 0")

    for failure in failures:
        print("FAIL:", failure)
    if failures:
        return 1
    print("OK: multi-GPU monitor selection keeps the adapter/output pair; "
          "a mismatch falls back to Python frames")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
