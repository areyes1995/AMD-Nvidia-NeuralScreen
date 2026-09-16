"""Backend selection and AMD helpers without hardware.

select_backend() is pure; weights/params need only temp files. Runs
anywhere - no GPU, no worker, no MSVC.
"""
import sys
import tempfile
from pathlib import Path

BASE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(BASE))

from amd_mode.python.backend import normalize_backend, select_backend
from amd_mode.python import params as amd_params
from amd_mode.python import weights as amd_weights


def main() -> int:
    failures = []

    def check(cond, what):
        if not cond:
            failures.append(what)

    # normalize: env wins, unknown means auto
    check(normalize_backend("amd", "nvidia", "auto") == "amd", "env must win")
    check(normalize_backend(None, "", "auto") == "auto", "empty means auto")
    check(normalize_backend("bogus") == "auto", "unknown means auto")
    check(normalize_backend(None, "NVIDIA") == "nvidia", "case-insensitive")

    # explicit overrides
    check(select_backend("nvidia", True, True, False) == ("nvidia", ""),
          "forced nvidia with files runs")
    check(select_backend("nvidia", True, False, True) == ("degraded", "no_worker"),
          "forced nvidia without files degrades")
    check(select_backend("amd", False, False, True) == ("amd", ""),
          "forced amd with worker runs")
    check(select_backend("amd", False, False, False) == ("degraded", "no_amd_worker"),
          "forced amd without worker degrades")

    # auto: NVIDIA hw + files -> nvidia, untouched normal path
    check(select_backend("auto", True, True, False) == ("nvidia", ""),
          "auto on NVIDIA runs nvidia")
    check(select_backend("auto", True, True, True) == ("nvidia", ""),
          "auto prefers nvidia on NVIDIA hw")
    # auto: AMD hw + amd worker -> amd (the whole point of Phase 2a)
    check(select_backend("auto", False, False, True) == ("amd", ""),
          "auto on AMD with worker runs amd")
    # auto: nothing usable -> degraded with the honest reason
    check(select_backend("auto", False, False, False) == ("degraded", "no_nvidia"),
          "auto on AMD without worker is no_nvidia")
    check(select_backend("auto", True, False, False) == ("degraded", "no_worker"),
          "auto on NVIDIA without files is no_worker")

    # params mapping keeps the slider vocabulary
    got = amd_params.nvidia_to_dlssnr(
        {"style": 2, "intensity": 1.0, "local_tone": 0.9,
         "local_structure": 1.5, "skin_structure": 1.0, "auto_mask": 1},
        work_scale=0.65, passes=2)
    check(got["Passes"] == 2, "passes ride along")
    check(got["WorkingScale"] == 0.65, "work_scale rides along")
    check(got["Pass1"]["Intensity"] == 1.0, "sliders survive the mapping")
    check(got["Pass1"]["Style"] == 2, "style survives the mapping")
    check(amd_params.clamp_passes(9) == 3, "passes clamp high")
    check(amd_params.clamp_passes(0) == 1, "passes clamp low")
    check(amd_params.clamp_passes("x") == 1, "passes garbage means one")

    # weights: garbage is rejected, missing is missing
    with tempfile.TemporaryDirectory() as tmp:
        junk = Path(tmp) / "nvngx_dlssnr.dll"
        junk.write_bytes(b"not a runtime")
        ok, reason = amd_weights.verify_runtime(junk)
        check(ok is False and bool(reason), "garbage runtime rejected")
        check(amd_weights.find_runtime(extra_dirs=[tmp]) != junk,
              "garbage runtime not picked up")
        check(amd_weights.verify_runtime(None) == (False, "no runtime supplied"),
              "None rejected")
        check(amd_weights.verify_runtime(Path(tmp) / "absent.dll")[0] is False,
              "absent file rejected")

    for f in failures:
        print("FAIL:", f)
    if failures:
        return 1
    print("OK: backend matrix, params mapping and weights guards")
    return 0


if __name__ == "__main__":
    sys.exit(main())
