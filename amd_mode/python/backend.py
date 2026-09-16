"""Backend selection: nvidia / amd / degraded.

Pure function of (override, hardware, files on disk) - no imports from
the pipeline, so tests cover the whole matrix without a GPU, a worker
or MSVC. startup.bring_up is the only caller.
"""
from __future__ import annotations


VALID_OVERRIDES = ("auto", "nvidia", "amd")


def normalize_backend(*values) -> str:
    """First non-empty value wins; anything unknown means auto.

    Call as normalize_backend(env, cfg, "auto"): NS_NR_BACKEND wins over
    config.json's nr_backend, unset means auto-detect.
    """
    for value in values:
        if value is None:
            continue
        text = str(value).strip().lower()
        if not text:
            continue
        return text if text in VALID_OVERRIDES else "auto"
    return "auto"


def select_backend(override: str, has_nvidia: bool,
                   nvidia_worker_ok: bool, amd_worker_ok: bool) -> tuple:
    """(backend, reason) for this machine.

    backend is "nvidia", "amd" or "degraded". reason is "" when a worker
    runs, otherwise one of "no_nvidia" (no NVIDIA card at all),
    "no_worker" (NVIDIA card, NVIDIA files missing) or "no_amd_worker"
    (AMD requested/available, AMD worker missing).

    Auto prefers NVIDIA on NVIDIA hardware and takes the AMD worker
    whenever it is the only one that can run - a working pipeline beats
    an ideological one.
    """
    override = normalize_backend(override)
    if override == "nvidia":
        if nvidia_worker_ok:
            return "nvidia", ""
        return "degraded", "no_worker"
    if override == "amd":
        if amd_worker_ok:
            return "amd", ""
        return "degraded", "no_amd_worker"
    if has_nvidia and nvidia_worker_ok:
        return "nvidia", ""
    if amd_worker_ok:
        return "amd", ""
    if not has_nvidia:
        return "degraded", "no_nvidia"
    return "degraded", "no_worker"
