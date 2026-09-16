"""The AMD backend's NR parameter mapping.

Mirrors the shape of nvidia_mode/python/params.py so the menu and the
pipeline treat both backends the same: the four sliders plus the model
style stay the user-facing vocabulary, and nvidia_to_dlssnr() translates
them into the DlssNr multipass parameters (OptiScaler [DlssNr] schema:
Passes, WorkingScale, RunBeforeSR, FinishedPicture, per-pass
Style/Intensity/LocalStructure/LocalTone/SkinStructure/AutoMask,
AmdModelScale, AmdNeuralLighting).

The HIP worker itself lands in Phase 2; this module is already the
contract it will be driven with.
"""
from __future__ import annotations


# Backend defaults: one pre-SR pass at full work resolution, neural
# lighting on. Matches the local pack's OptiScaler.ini
# ([DlssNr] RunBeforeSR=true, AmdModelScale=1, AmdNeuralLighting=true).
AMD_DEFAULTS = {
    "passes": 1,
    "working_scale": 1.0,
    "run_before_sr": True,
    "finished_picture": False,
    "model_scale": 1,
    "neural_lighting": True,
}

#: Pass count clamp (OptiScaler guardrail: later layers converge in cost
#: and artifacts while detail stops improving).
PASSES_MIN = 1
PASSES_MAX = 3


def clamp_passes(value) -> int:
    """Pull a pass count into 1..3 - anything else is not a setting."""
    try:
        n = int(value)
    except (TypeError, ValueError):
        return AMD_DEFAULTS["passes"]
    return min(PASSES_MAX, max(PASSES_MIN, n))


def nvidia_to_dlssnr(params: dict, work_scale: float = 1.0,
                     passes: int = 1) -> dict:
    """Translate NVIDIA-style params into a DlssNr pass description.

    params carries the four sliders + style + auto_mask (the same keys
    nvidia_mode/python/params.py defines); work_scale is the Boost work
    resolution the model sees. Later passes inherit pass 1 unless the
    caller overrides per-pass keys (Pass2*/Pass3*), mirroring the
    OptiScaler multipass contract.
    """
    get = params.get
    base = {
        "Style": int(get("style", 1)),
        "Intensity": float(get("intensity", 1.0)),
        "LocalStructure": float(get("local_structure", 1.0)),
        "LocalTone": float(get("local_tone", 0.5)),
        "SkinStructure": float(get("skin_structure", -1.0)),
        "AutoMask": int(get("auto_mask", 1)),
    }
    return {
        "Passes": clamp_passes(passes),
        "WorkingScale": float(work_scale),
        "RunBeforeSR": True,
        "FinishedPicture": False,
        "AmdModelScale": AMD_DEFAULTS["model_scale"],
        "AmdNeuralLighting": AMD_DEFAULTS["neural_lighting"],
        "Pass1": dict(base),
    }
