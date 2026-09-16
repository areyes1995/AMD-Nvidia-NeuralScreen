"""Full-input AMD worker: protocol + real neural inputs, no GPU needed.

Spawns amd_mode/native/amd_nr_host.exe --live, feeds a synthetic D5V3
stream (gradient BGRA + fp16 motion field) and asserts:
  - OUT1 ok=1 with byte-identical passthrough pixels (dispatch is a stub)
  - [nr] telemetry on stderr carries exposure + motion + tuning params
  - MOTS resize is honoured (motion bytes follow the new grid)
  - one reset frame is counted (resets=1 in a later telemetry line)

Needs the built exe + MSVC-built binary; runs anywhere on Windows.
"""
import re
import struct
import subprocess
import sys
from pathlib import Path

BASE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(BASE))

import protocol as P

EXE = BASE / "amd_mode" / "native" / "amd_nr_host.exe"
W, H = 64, 48
MW, MH = 32, 24


def fp16_pair(x: float, y: float) -> bytes:
    import struct as s
    return s.pack("<ee", x, y)


def gradient(w: int, h: int, dark: bool = False) -> bytes:
    out = bytearray()
    for yy in range(h):
        for xx in range(w):
            v = int(255 * xx / max(w - 1, 1))
            if dark:
                v //= 8
            out += bytes((v, v, v, 255))
    return bytes(out)


def read_exact(pipe, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = pipe.read(n - len(buf))
        if not chunk:
            raise EOFError("worker closed the pipe")
        buf += chunk
    return buf


def main() -> int:
    failures = []

    def check(cond, what):
        if not cond:
            failures.append(what)

    if not EXE.exists():
        print(f"SKIP: {EXE} not built (run amd_mode/native/build-amd.bat)")
        return 0

    proc = subprocess.Popen(
        [str(EXE), "--live"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    try:
        hdr = struct.pack(P.HEADER_FMT, P.VIDEO_MAGIC, W, H, 0, 0,
                           0, 0, 2, 1, 0,
                           1.0, 0.9, 1.5, 1.0, W, H)
        proc.stdin.write(hdr)
        proc.stdin.flush()

        # MOTS: motion arrives at MW x MH fp16 2ch
        proc.stdin.write(struct.pack(P.MOTION_FMT, P.MOTION_MAGIC, MW, MH, 0, 0))
        proc.stdin.flush()
        read_exact(proc.stdout, 24)  # MACK

        mv = fp16_pair(2.0, -1.0) * (MW * MH)
        n_frames = 70  # >60 so the periodic [nr] line fires
        got_bytes = 0
        for i in range(n_frames):
            reset = 1 if i == 5 else 0
            color = gradient(W, H, dark=(i >= n_frames // 2))
            proc.stdin.write(struct.pack(P.FRAME_FMT, P.FRAME_MAGIC, i, reset, 0, i))
            proc.stdin.write(color)
            proc.stdin.write(mv)
            proc.stdin.flush()
            out = read_exact(proc.stdout, struct.calcsize(P.OUT_FMT))
            magic, index, ok, nbytes, ngx, pts = struct.unpack(P.OUT_FMT, out)
            check(magic == P.OUT_MAGIC and index == i and ok == 1, f"OUT1 #{i} ok")
            check(nbytes == W * H * 4, f"OUT1 #{i} size")
            payload = read_exact(proc.stdout, nbytes)
            check(payload == color, f"frame #{i} passthrough byte-identical")
            got_bytes += nbytes
        check(got_bytes == n_frames * W * H * 4, "all payloads received")
    finally:
        try:
            proc.stdin.close()
        except OSError:
            pass
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        err = proc.stderr.read().decode("utf-8", "replace")

    hits = re.findall(r"\[nr\] f=(\d+) exp=([\d.]+) mv=([\d.]+) resets=(\d+)"
                      r" int=([\d.]+) tone=([\d.]+) struct=([\d.]+) skin=([\d.]+)"
                      r" mask=(\d+) style=(\d+) uic=(\d+)", err)
    check(len(hits) >= 2, "telemetry fires at f=1 and on the 60-frame cadence")
    m = hits[-1] if hits else None
    check(m is not None, "telemetry [nr] line present on stderr")
    if m:
        n, exp, mv, resets = int(m[0]), float(m[1]), float(m[2]), int(m[3])
        check(n >= 60, "telemetry fires on the 60-frame cadence")
        check(0.5 <= exp <= 2.0, f"exposure in PaperWhite range (got {exp})")
        check(abs(mv - 2.236) < 0.05, f"mean |v| matches synthetic field (got {mv})")
        check(resets == 1, "the reset frame was counted")
        check((m[4], m[8], m[9]) == ("1.00", "1", "2"),
              "tuning params ride from header to dispatch record")
    else:
        print("--- stderr ---")
        print(err[-2000:])

    for f in failures:
        print("FAIL:", f)
    if failures:
        return 1
    print("OK: full-input worker (passthrough + exposure + motion + tuning)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
