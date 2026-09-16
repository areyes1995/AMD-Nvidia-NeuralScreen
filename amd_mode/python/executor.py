"""Neural-execution scaffold for the AMD backend (Phase 2b-prep).

The dispatch contract is frozen (see nr_host_full.cpp `Dispatch`): the
worker hands over color + motion + exposure + reset + tuning, and whatever
implements `NeuralExecutor` turns them into an output frame. Backends:

- PassthroughExecutor: what amd_nr_host.exe does today (byte-identical).
- DirectMLProbe: reports whether this machine could run an ONNX graph via
  DirectML right now (onnxruntime-directml import + device query). It does
  NOT run the network: the graph definition (shapes, order, activations)
  only exists in the HIP source we requested (docs/GPL_SOURCE_REQUEST.md).

When the graph lands, the third backend is HipExecutor (native worker
change, not Python): same Dispatch record, real multipass dispatch.
"""
from __future__ import annotations


class NeuralExecutor:
    name = "base"

    def describe(self) -> str:
        raise NotImplementedError

    def dispatch(self, color: bytes, width: int, height: int,
                 motion: bytes, exposure: float, reset: int,
                 tuning: dict) -> bytes:
        """Color BGRA in, BGRA out (same size)."""
        raise NotImplementedError


class PassthroughExecutor(NeuralExecutor):
    name = "passthrough"

    def describe(self) -> str:
        return ("passthrough: input colour is the output; exposure/motion/ "
                "tuning are computed and logged but not applied")

    def dispatch(self, color: bytes, width: int, height: int,
                 motion: bytes, exposure: float, reset: int,
                 tuning: dict) -> bytes:
        if len(color) != width * height * 4:
            raise ValueError(
                f"color is {len(color)} bytes, expected {width * height * 4}")
        return color


def probe_directml() -> tuple:
    """(ready, detail): can this machine execute ONNX via DirectML today?"""
    try:
        import onnxruntime as ort
    except ImportError:
        return False, "onnxruntime not installed (pip install onnxruntime-directml)"
    providers = getattr(ort, "get_available_providers", lambda: [])()
    if "DmlExecutionProvider" in providers:
        return True, f"DmlExecutionProvider available ({providers})"
    return False, f"no DML provider (have: {providers})"


def select_executor(name: str) -> NeuralExecutor:
    if name == "passthrough":
        return PassthroughExecutor()
    raise ValueError(f"unknown executor {name!r} (have: passthrough)")
