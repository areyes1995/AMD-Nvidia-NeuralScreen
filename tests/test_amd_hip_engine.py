"""The real neural pass on AMD: the HIP runtime hosted inside our worker.

Skips cleanly when the runtime is not installed (amd_mode/weights/version.dll
+ dlssnr_on_amd_weights.bin + amd_fidelityfx_upscaler_dx12.dll, all BYO), so
this is green on a machine that has nothing to run. Where it does run it
asserts the things that would quietly rot:

  - the worker reports the neural path, not passthrough
  - the output really changed (a passthrough regression is the whole risk)
  - the output is still an image: same size, comparable brightness, and
    R/B not swapped - the pass reads BGRA and writes through an RGBA UAV,
    so a swizzle slip is one line away and invisible in an FPS number
  - the runtime's own log says `engine init ok`
  - the [nr] telemetry counts neural frames

See docs/AMD_HIP_HOSTING.md for what the runtime is and why it is hosted
this way.
"""
import os
import re
import struct
import subprocess
import sys
import time
from pathlib import Path

BASE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(BASE))

import protocol as P

EXE = BASE / "amd_mode" / "native" / "amd_nr_host.exe"
WEIGHTS_DIR = BASE / "amd_mode" / "weights"
RUNTIME = WEIGHTS_DIR / "version.dll"
BLOBS = WEIGHTS_DIR / "dlssnr_on_amd_weights.bin"
UPSCALER = WEIGHTS_DIR / "amd_fidelityfx_upscaler_dx12.dll"
RUNTIME_LOG = WEIGHTS_DIR / "dlssnr_on_amd.log"

W, H = 320, 180
FRAMES = 12


def scene(w: int, h: int) -> bytes:
    """A red-leaning lit gradient: bright enough for the pass to have work,
    lopsided enough in colour that a channel swap cannot hide."""
    out = bytearray()
    for y in range(h):
        for x in range(w):
            r = 90 + int(140 * x / max(w - 1, 1))
            g = 40 + int(60 * y / max(h - 1, 1))
            b = 30
            out += bytes((b, g, r, 255))  # BGRA on the wire
    return bytes(out)


def means(buf: bytes):
    """(B, G, R) means of a BGRA buffer."""
    b = g = r = 0
    n = len(buf) // 4
    for i in range(0, len(buf), 4):
        b += buf[i]; g += buf[i + 1]; r += buf[i + 2]
    return b / n, g / n, r / n


def read_exact(pipe, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = pipe.read(n - len(buf))
        if not chunk:
            raise EOFError("worker closed the pipe")
        buf += chunk
    return buf


def main() -> int:
    if not EXE.exists():
        print(f"SKIP: {EXE} not built (run amd_mode/native/build-amd.bat)")
        return 0
    missing = [p.name for p in (RUNTIME, BLOBS, UPSCALER) if not p.exists()]
    if missing:
        print(f"SKIP: no HIP runtime installed, missing {', '.join(missing)} "
              f"in amd_mode/weights (see docs/AMD_HIP_HOSTING.md)")
        return 0

    failures = []

    def check(cond, what):
        if not cond:
            failures.append(what)

    before = RUNTIME_LOG.stat().st_size if RUNTIME_LOG.exists() else 0
    colour = scene(W, H)
    env = dict(os.environ)
    env.pop("NS_AMD_NR", None)
    proc = subprocess.Popen(
        [str(EXE), "--live"], env=env,
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    outputs = []
    try:
        proc.stdin.write(struct.pack(P.HEADER_FMT, P.VIDEO_MAGIC, W, H, 0, 0,
                                     0, 0, 2, 1, 0,
                                     1.0, 0.0, 1.0, 1.0, W, H))
        proc.stdin.flush()
        motion = b"\x00" * (W * H * 4)  # fp16 2ch, still scene
        # The engine comes up on its own thread (so the worker never stalls a
        # session on frame 0), which means the first frames back are
        # passthrough by design. Feed until the output changes, then take a
        # few more; give up on the clock, not on a frame count.
        deadline = time.monotonic() + 30.0
        i = 0
        after_change = 0
        while time.monotonic() < deadline and after_change < FRAMES:
            proc.stdin.write(struct.pack(P.FRAME_FMT, P.FRAME_MAGIC, i,
                                         1 if i == 0 else 0, 0, i))
            proc.stdin.write(colour)
            proc.stdin.write(motion)
            proc.stdin.flush()
            head = read_exact(proc.stdout, struct.calcsize(P.OUT_FMT))
            magic, idx, ok, nbytes, _res, _pts = struct.unpack(P.OUT_FMT, head)
            check(magic == P.OUT_MAGIC and ok == 1, f"frame {i}: OUT1 ok")
            payload = read_exact(proc.stdout, nbytes) if (ok and nbytes) else b""
            if after_change or payload != colour:
                outputs.append(payload)
                after_change += 1
            i += 1
        check(after_change > 0,
              f"the neural pass took over within 30 s ({i} frames sent)")
    finally:
        try:
            proc.stdin.close()
        except OSError:
            pass
        err = proc.stderr.read().decode("utf-8", "replace")
        proc.wait(timeout=30)

    check("neural pass ON" in err,
          f"worker took the neural path (stderr said: "
          f"{[l for l in err.splitlines() if 'neural pass' in l]})")
    check(len(outputs) >= 1, f"got {len(outputs)} neural frames back")
    if not outputs:
        for f in failures:
            print("FAIL:", f)
        print(err.strip())
        return 1

    last = outputs[-1]
    check(len(last) == len(colour), "output is the same size as the input")
    check(last != colour, "output is NOT byte-identical (the pass ran)")

    ib, ig, ir = means(colour)
    ob, og, orr = means(last)
    # The network changes the image; it does not turn it into a different one.
    check(0.4 * (ib + ig + ir) < (ob + og + orr) < 2.0 * (ib + ig + ir),
          f"brightness stayed sane: in {(ib+ig+ir)/3:.1f} out {(ob+og+orr)/3:.1f}")
    check(orr > og > ob,
          f"channel order survived: out B={ob:.1f} G={og:.1f} R={orr:.1f} "
          f"(in B={ib:.1f} G={ig:.1f} R={ir:.1f})")

    # Every [nr] line, not the first: the pass takes over mid-session, so the
    # early lines legitimately read neural=0.
    counts = [int(n) for n in re.findall(r"\[nr\].*neural=(\d+)", err)]
    check(counts and max(counts) > 0,
          f"telemetry counts neural frames (saw {counts[-3:] or 'no [nr] line'})")

    if RUNTIME_LOG.exists():
        tail = RUNTIME_LOG.read_text("utf-8", "replace")[before:]
        check("engine init ok" in tail, "runtime logged 'engine init ok'")
        job = re.search(r"network job \d+ done in (\d+) ms", tail)
        if job:
            print(f"runtime: network job {job.group(0).split()[2]} at "
                  f"{job.group(1)} ms ({W}x{H})")

    for f in failures:
        print("FAIL:", f)
    if failures:
        print("--- worker stderr ---")
        print(err.strip())
        return 1
    print(f"OK: neural pass on AMD, {len(outputs)} frames, output changed, "
          f"means in B{ib:.0f}/G{ig:.0f}/R{ir:.0f} -> out B{ob:.0f}/G{og:.0f}/R{orr:.0f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
