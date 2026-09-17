"""A/B harness: same synthetic scenes through any NR worker.

Feeds three scenes over the D5V3/FRM1/OUT1 protocol and reports per-scene
round-trip FPS plus output integrity, so backends can be compared
apples-to-apples on different machines:

  scene pan   - horizontal gradient scrolling 4 px/frame, MV matches
  scene dark  - near-black frames (exposure must swing up, worker logs it)
  scene cut   - reset=1 on one frame (temporal history must restart)

Usage:
  python tools/ab_compare.py --exe amd_mode/native/amd_nr_host.exe --out docs/ab_baseline_amd.json
  python tools/ab_compare.py --exe nvidia_mode/native/dlss5-feed-host64.exe --out docs/ab_baseline_nvidia.json

The NVIDIA worker needs an NVIDIA GPU + NGX runtime; on other machines it
exits immediately and the harness records run.ok=false instead of failing.
For passthrough workers (today's AMD binary) integrity means
byte-identical output; for neural workers it records a payload hash per
scene so later runs can diff quality regressions.
"""
import argparse
import hashlib
import json
import re
import struct
import subprocess
import sys
import time
from pathlib import Path

BASE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(BASE))

import protocol as P

W, H = 320, 180
MW, MH = 160, 90
N = 120


def fp16(x: float) -> bytes:
    return struct.pack("<e", x)


def scene_pan(i: int) -> tuple:
    shift = (i * 4) % W
    row = bytearray()
    for x in range(W):
        v = (x + shift) % 256
        row += bytes((v, v, v, 255))
    color = bytes(row) * H
    mv = (fp16(4.0) + fp16(0.0)) * (MW * MH)
    return color, mv, 0


def scene_dark(i: int) -> tuple:
    v = 8
    color = bytes((v, v, v, 255)) * (W * H)
    mv = (fp16(0.0) + fp16(0.0)) * (MW * MH)
    return color, mv, 0


def scene_cut(i: int) -> tuple:
    color, mv, _ = scene_pan(i)
    return color, mv, 1 if i == N // 2 else 0


SCENES = {"pan": scene_pan, "dark": scene_dark, "cut": scene_cut}


def read_exact(pipe, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = pipe.read(n - len(buf))
        if not chunk:
            raise EOFError("worker closed the pipe")
        buf += chunk
    return buf


def wait_for_neural(proc, seconds: float) -> dict:
    """Feed a still scene until the worker stops echoing it back.

    A worker whose dispatch is a passthrough returns the frame byte for byte,
    so "the output changed" is the one signal that works without asking the
    worker anything. Returns what happened, for the report: a run measured on
    passthrough and a run measured on the network must not look alike in the
    JSON.
    """
    color, mv, _ = scene_pan(0)
    deadline = time.perf_counter() + seconds
    sent = 0
    while time.perf_counter() < deadline:
        proc.stdin.write(struct.pack(P.FRAME_FMT, P.FRAME_MAGIC, sent, 0, 0, sent))
        proc.stdin.write(color)
        proc.stdin.write(mv)
        proc.stdin.flush()
        head = read_exact(proc.stdout, struct.calcsize(P.OUT_FMT))
        _m, _i, ok, nbytes, _r, _p = struct.unpack(P.OUT_FMT, head)
        payload = read_exact(proc.stdout, nbytes) if (ok and nbytes) else b""
        sent += 1
        if payload and payload != color:
            waited = seconds - (deadline - time.perf_counter())
            return {"active": True, "frames": sent, "seconds": round(waited, 2)}
    return {"active": False, "frames": sent, "seconds": seconds}


def run_scene(proc, fn) -> dict:
    hashes, identical, t0 = [], True, time.perf_counter()
    for i in range(N):
        color, mv, reset = fn(i)
        proc.stdin.write(struct.pack(P.FRAME_FMT, P.FRAME_MAGIC, i, reset, 0, i))
        proc.stdin.write(color)
        proc.stdin.write(mv)
        proc.stdin.flush()
        out = read_exact(proc.stdout, struct.calcsize(P.OUT_FMT))
        _magic, _index, ok, nbytes, _ngx, _pts = struct.unpack(P.OUT_FMT, out)
        if ok != 1 or nbytes != W * H * 4:
            return {"ok": False, "reason": f"bad OUT1 frame {i}: ok={ok} bytes={nbytes}"}
        payload = read_exact(proc.stdout, nbytes)
        hashes.append(hashlib.sha256(payload).hexdigest()[:16])
        identical = identical and payload == color
    dt = time.perf_counter() - t0
    return {"ok": True, "fps": round(N / dt, 1),
            "byte_identical": identical, "hash_first": hashes[0],
            "hash_last": hashes[-1],
            "hash_changes": sum(1 for a, b in zip(hashes, hashes[1:]) if a != b)}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--label", default="")
    ap.add_argument("--out", default="")
    ap.add_argument("--wait-neural", type=float, default=0.0, metavar="SECONDS",
                    help="feed filler frames until the worker's output stops "
                         "being byte-identical, then measure. The AMD HIP "
                         "engine comes up asynchronously (a few seconds), so "
                         "without this the scenes are all measured on the "
                         "passthrough that runs meanwhile. 20 is plenty.")
    args = ap.parse_args()

    report = {"backend": args.label or Path(args.exe).stem,
              "exe": args.exe, "scenes": {}, "ok": False}
    try:
        proc = subprocess.Popen(
            [args.exe, "--live"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except OSError as exc:
        report["reason"] = f"cannot spawn: {exc}"
        print(json.dumps(report, indent=2))
        return 0
    try:
        proc.stdin.write(struct.pack(
            P.HEADER_FMT, P.VIDEO_MAGIC, W, H, 0, 0, 0, 0, 2, 1, 0,
            1.0, 0.9, 1.5, 1.0, W, H))
        proc.stdin.write(struct.pack(P.MOTION_FMT, P.MOTION_MAGIC, MW, MH, 0, 0))
        proc.stdin.flush()
        try:
            read_exact(proc.stdout, 24)  # MACK
        except EOFError:
            err = proc.stderr.read().decode("utf-8", "replace")[-500:]
            report["reason"] = f"worker exited during handshake: {err}"
            print(json.dumps(report, indent=2))
            return 0
        if args.wait_neural > 0:
            report["neural"] = wait_for_neural(proc, args.wait_neural)
        for name, fn in SCENES.items():
            report["scenes"][name] = run_scene(proc, fn)
            if not report["scenes"][name]["ok"]:
                break
        report["ok"] = all(s.get("ok") for s in report["scenes"].values())
    except (EOFError, BrokenPipeError) as exc:
        report["reason"] = f"worker died mid-run: {exc}"
    finally:
        try:
            proc.stdin.close()
        except OSError:
            pass
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        try:
            err = proc.stderr.read().decode("utf-8", "replace")
        except Exception:
            err = ""
    tel = re.findall(r"\[nr\] f=(\d+) exp=([\d.]+) mv=([\d.]+) resets=(\d+)", err)
    if tel:
        last = tel[-1]
        report["telemetry"] = {"frames": int(last[0]), "exposure": float(last[1]),
                               "mv_mean": float(last[2]), "resets": int(last[3])}
    text = json.dumps(report, indent=2)
    if args.out:
        Path(args.out).write_text(text, encoding="utf-8")
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
