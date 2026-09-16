"""Reader for `dlssnr_on_amd_weights.bin` (OptiScaler AMD pack format).

Layout, reverse-engineered and verified byte-for-byte against the real
140.8 MB file (153 entries, table + data == file size exactly):

  offset 0:  magic "DLSSNRW1" (8 bytes)
  offset 8:  count u32 LE (153)
  offset 12: data_start u32 LE (absolute file offset where tensor data
             begins; 5673 = 16-byte header + 5657-byte name table)
  offset 16: count entries of {u8 len, name[len], u64 data_off, u64 size}
             data_off is relative to data_start; entries are contiguous
             and cover the rest of the file.
  blocks 0..70, mostly `blockN.layerM.layer`, plus one scalar:
  `block70.layer0.blend_scale` (2 bytes: fp16).

What this does NOT contain (and why Phase 2b still needs the HIP
source): shapes, dtypes, layer order, activations. Names + sizes only.
See docs/GPL_SOURCE_REQUEST.md for the request covering the rest.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path

MAGIC = b"DLSSNRW1"
HEADER_FMT = "<8sII"
HEADER_SIZE = struct.calcsize(HEADER_FMT)


@dataclass(frozen=True)
class TensorEntry:
    name: str
    data_off: int  # relative to the end of the name table
    size: int


@dataclass(frozen=True)
class WeightsFile:
    path: Path
    count: int
    data_start: int  # absolute file offset where tensor data begins
    entries: tuple

    @property
    def tensor_bytes(self) -> int:
        return sum(e.size for e in self.entries)


def parse_weights(path) -> WeightsFile:
    """Parse the name table; raises ValueError on any inconsistency."""
    path = Path(path)
    with open(path, "rb") as fh:
        header = fh.read(HEADER_SIZE)
        if len(header) != HEADER_SIZE:
            raise ValueError(f"{path}: truncated header")
        magic, count, data_start = struct.unpack(HEADER_FMT, header)
        if magic != MAGIC:
            raise ValueError(f"{path}: bad magic {magic!r}")
        if not (1 <= count <= 100000):
            raise ValueError(f"{path}: absurd tensor count {count}")
        if not (HEADER_SIZE <= data_start <= 10_000_000):
            raise ValueError(f"{path}: absurd data_start {data_start}")
        entries = []
        for i in range(count):
            raw_len = fh.read(1)
            if not raw_len:
                raise ValueError(f"{path}: table ends at entry {i}")
            (length,) = struct.unpack("B", raw_len)
            name = fh.read(length)
            blob = fh.read(16)
            if len(name) != length or len(blob) != 16:
                raise ValueError(f"{path}: table truncated at entry {i}")
            try:
                text = name.decode("ascii")
            except UnicodeDecodeError:
                raise ValueError(f"{path}: non-ascii name at entry {i}")
            data_off, size = struct.unpack("<QQ", blob)
            entries.append(TensorEntry(text, data_off, size))
        table_end = fh.tell()
        if table_end != data_start:
            raise ValueError(
                f"{path}: table ends at {table_end}, "
                f"header claims data starts at {data_start}")
        # Contiguity: each tensor starts where the previous one ends.
        for prev, cur in zip(entries, entries[1:]):
            if cur.data_off != prev.data_off + prev.size:
                raise ValueError(
                    f"{path}: gap/overlap between {prev.name} and {cur.name}")
        if entries and entries[0].data_off != 0:
            raise ValueError(f"{path}: data does not start at table end")
    return WeightsFile(path, count, table_end, tuple(entries))


def find_pack_weights(extra_dirs=()) -> Path | None:
    """Locate a user-supplied .bin (never committed): weights dir, then extras."""
    from amd_mode.python.weights import WEIGHTS_NAME, _weights_dir
    candidates = [_weights_dir()]
    candidates += [Path(d) for d in extra_dirs]
    for directory in candidates:
        candidate = directory / WEIGHTS_NAME
        try:
            if candidate.is_file():
                return candidate
        except OSError:
            continue
    return None
