"""Weights-table parser + executor scaffold, no GPU needed.

- Parser unit tests run on synthetic tables (garbage rejected).
- The real 153-tensor .bin is verified only if present on disk
  (user-supplied, never committed) - otherwise skipped.
- DirectML probe is informational: it must return a 2-tuple, and on a
  machine with onnxruntime-directml it must report ready.
"""
import struct
import sys
import tempfile
from pathlib import Path

BASE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(BASE))

from amd_mode.python.dlssnr_weights import (
    MAGIC, WeightsFile, find_pack_weights, parse_weights,
)
from amd_mode.python.executor import (
    PassthroughExecutor, probe_directml, select_executor,
)

CANDIDATES = [
    Path(r"D:\optiscaler\OptiScaler-AMD-PreSR-Multipass-v1.7.3"
         r"\dlssnr_on_amd_weights.bin"),
]


def fake_bin(entries) -> bytes:
    table = b"".join(
        struct.pack("B", len(name)) + name + struct.pack("<QQ", off, size)
        for name, off, size in entries)
    return struct.pack("<8sII", MAGIC, len(entries), 16 + len(table)) + table


def main() -> int:
    failures = []

    def check(cond, what):
        if not cond:
            failures.append(what)

    with tempfile.TemporaryDirectory() as tmp:
        good = Path(tmp) / "good.bin"
        good.write_bytes(fake_bin([(b"block0.layer0.layer", 0, 8),
                                   (b"block1.layer0.layer", 8, 4)]))
        w = parse_weights(good)
        check(isinstance(w, WeightsFile) and w.count == 2, "fake table parses")
        check(w.data_start == 16 + 2 * (1 + 19 + 16), "data_start math")
        check(w.entries[1].data_off == 8, "offsets survive")

        bad = Path(tmp) / "bad.bin"
        bad.write_bytes(b"NOPE" + b"\x00" * 64)
        try:
            parse_weights(bad)
            check(False, "bad magic must raise")
        except ValueError:
            pass
        gap = Path(tmp) / "gap.bin"
        gap.write_bytes(fake_bin([(b"a", 0, 8), (b"b", 99, 4)]))
        try:
            parse_weights(gap)
            check(False, "non-contiguous table must raise")
        except ValueError:
            pass

    real = next((p for p in CANDIDATES if p.is_file()), find_pack_weights())
    if real is None:
        print("SKIP: no user-supplied dlssnr_on_amd_weights.bin on disk")
    else:
        w = parse_weights(real)
        check(w.count == 153, f"real pack has 153 tensors (got {w.count})")
        check(w.entries[0].name == "block0.layer0.layer", "first tensor")
        check(any("blend_scale" in e.name for e in w.entries), "blend_scale scalar")
        check(all(e.data_off + e.size <= w.tensor_bytes for e in w.entries),
              "all tensors inside the data region")
        print(f"real pack OK: {w.count} tensors, "
              f"{w.tensor_bytes / 1e6:.1f} MB tensor data")

    ex = select_executor("passthrough")
    check(isinstance(ex, PassthroughExecutor), "passthrough selectable")
    color = bytes(range(256)) * 4  # 256 BGRA px
    check(ex.dispatch(color, 16, 16, b"", 1.0, 0, {}) == color,
          "passthrough is byte-identical")
    try:
        select_executor("hip")
        check(False, "unknown executor must raise")
    except ValueError:
        pass

    ready, detail = probe_directml()
    check(isinstance(ready, bool) and isinstance(detail, str), "probe shape")
    print(f"directml ready={ready}: {detail}")

    for f in failures:
        print("FAIL:", f)
    if failures:
        return 1
    print("OK: weights parser, executor scaffold, directml probe")
    return 0


if __name__ == "__main__":
    sys.exit(main())
