"""BYO weights management for the AMD backend.

Nothing neural is ever committed: the user supplies their own
nvngx_dlssnr.dll (verified by SHA-256, same table as OptiScaler's
INSTALL-DLSSNR.md) or a prebuilt dlssnr_on_amd_weights.bin, and the
first run converts/caches the .bin next to it. Mirrors the
native/libraries/ BYO pattern of the NVIDIA side.
"""
from __future__ import annotations

import hashlib
from pathlib import Path


#: Pinned runtimes (OptiScaler INSTALL-DLSSNR.md, "Choose the correct
#: runtime"). The ShortFuse cross-generation build is the modified one:
#: Windows reports its NVIDIA signature as invalid, which is expected
#: for exactly this hash - verify it and keep protection enabled.
RUNTIME_SHA256 = {
    # Original NVIDIA-signed 310.8 (RTX 50).
    "e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e": "nvidia-310.8",
    # ShortFuse cross-generation 310.8 (RTX 20/30/40).
    "e67dee209320cdaffE0e93e45675d7aa34323a53acc57a72b2e40a181581c989a".lower(): "shortfuse-310.8",
}

RUNTIME_NAME = "nvngx_dlssnr.dll"
WEIGHTS_NAME = "dlssnr_on_amd_weights.bin"


def _weights_dir() -> Path:
    return Path(__file__).resolve().parent.parent / "weights"


def weights_path() -> Path:
    """Where the converted weights live (amd_mode/weights/)."""
    return _weights_dir() / WEIGHTS_NAME


def sha256_of(path: Path) -> str:
    """Hex digest of a file, streamed (runtimes are ~165 MB)."""
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def verify_runtime(path: Path) -> tuple:
    """(ok, label|reason) for a user-supplied runtime DLL."""
    path = Path(path)
    if not path.is_file():
        return False, f"not found: {path}"
    try:
        digest = sha256_of(path)
    except OSError as exc:
        return False, f"unreadable ({exc})"
    label = RUNTIME_SHA256.get(digest.lower())
    if label is None:
        return False, ("hash not in the pinned table - use the original "
                       "310.8 or the ShortFuse cross-gen 310.8 build")
    return True, label


def find_runtime(extra_dirs=()) -> Path | None:
    """First usable nvngx_dlssnr.dll: weights dir, NVIDIA tree, cwd."""
    from paths import BASE_DIR, NATIVE_DIR
    candidates = [Path(d) for d in extra_dirs]
    candidates += [
        _weights_dir(),
        NATIVE_DIR,
        NATIVE_DIR / "libraries",
        Path.cwd(),
    ]
    for directory in candidates:
        try:
            candidate = directory / RUNTIME_NAME
        except Exception:
            continue
        ok, _label = verify_runtime(candidate)
        if ok:
            return candidate
    return None


def needs_conversion() -> bool:
    """True when the .bin is missing but a runtime is available."""
    return not weights_path().is_file() and find_runtime() is not None


def convert_weights(runtime: Path | None = None) -> Path:
    """Build the .bin from a user runtime (Phase 2: the HIP converter).

    Raises NotImplementedError until the native converter lands; the
    contract is already fixed: verified input in, cached .bin out.
    """
    raise NotImplementedError(
        "weights conversion lands with the Phase 2 HIP worker; "
        "see amd_mode/README.md")
