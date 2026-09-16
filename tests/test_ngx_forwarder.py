"""The NGX caller rule, both ways it can be satisfied.

nvngx_dlssnr.dll decides whether to answer by looking at the module the call
RETURNS to - not at the process name. If that module's path does not carry the
substring "nvngx.dll", it refuses with FAIL_PlatformError (0xBAD00002) before
it even reads the arguments.

R8 settled what that means for the worker: the worker IS an executable named
nvngx.dll (nvidia_mode\native\nvngx.dll), so its own path carries the substring and the
calls leave the worker itself - the forwarding layer is the escape hatch
(NS_FORWARDER=1), not the default. Measured live: ~2400 frames through the
direct path, zero restarts.

Three legs, all against the real library:

* the worker as shipped: the log says the calls leave the worker itself, and
  Init_Ext returns Success;
* NS_FORWARDER=1: the log names the forwarder it loaded (path carries the
  substring), and Init_Ext returns Success through it;
* the same forwarder binary copied to a name WITHOUT the substring
  (NS_FORWARDER points at the copy): the call is refused with 0xBAD00002.

The third leg is the one that matters. Without it this test would pass just as
happily if the library had stopped checking anything at all.

Run:  runtime\\python.exe tests\\test_ngx_forwarder.py
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

BASE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(BASE))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import autocheck  # noqa: E402
from paths import NATIVE_DIR, WORKER_EXE  # noqa: E402

FORWARDER = NATIVE_DIR / "nvngx.dll_ns-forwarder.dll"
TIMEOUT = 60.0


def run_worker(env_extra: dict, needle: str) -> str:
    """Start the worker, wait for `needle` in its output, return that output.

    The worker's own log lines go to its error stream; they only reach
    NeuralScreen.log because the program relays them. Started here on its own,
    nobody relays anything, so the stream is captured to a file and read as it
    fills - a pipe would have to be drained on a thread to avoid deadlocking
    against the worker's other stream.
    """
    env = dict(os.environ, **env_extra)
    with tempfile.TemporaryFile() as err:
        proc = subprocess.Popen([str(WORKER_EXE), "--live"], cwd=str(NATIVE_DIR),
                                env=env, stdin=subprocess.PIPE,
                                stdout=subprocess.DEVNULL, stderr=err,
                                creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            deadline = time.monotonic() + TIMEOUT
            text = ""
            while time.monotonic() < deadline:
                time.sleep(0.2)
                err.seek(0)
                text = err.read().decode("utf-8", "replace")
                if needle in text:
                    # The routing line and the Init_Ext line are written back
                    # to back; give the second one a moment.
                    time.sleep(0.5)
                    err.seek(0)
                    return err.read().decode("utf-8", "replace")
                if proc.poll() is not None:
                    break
            return text
        finally:
            try:
                proc.stdin.close()
            except Exception:
                pass
            proc.terminate()
            try:
                proc.wait(timeout=15)
            except Exception:
                proc.kill()


def main() -> int:
    failures = []

    if not WORKER_EXE.is_file():
        print(f"FAIL: worker not found: {WORKER_EXE}")
        return 1
    if not FORWARDER.is_file():
        print(f"FAIL: the forwarder is not built: {FORWARDER}")
        print("      run nvidia_mode\\native\\build-host.bat")
        return 1
    if "nvngx.dll" not in FORWARDER.name:
        failures.append(f"the forwarder is named {FORWARDER.name!r} - the "
                        f"substring nvngx.dll is the reason it works")
    if autocheck.running_instances():
        print("FAIL: a copy of the program is already running - close it first")
        return 1

    # As shipped: the calls leave the worker itself.
    tail = run_worker({}, "worker itself")
    direct = [l for l in tail.splitlines() if "worker itself" in l]
    if not direct:
        failures.append("the worker did not report the direct (worker-own) path")
    if "Init_Ext (from the worker) -> 0x00000001" not in tail:
        line = [l for l in tail.splitlines() if "Init_Ext" in l]
        failures.append(f"Init_Ext did not succeed from the worker: "
                        f"{line[-1].strip() if line else 'no Init_Ext line at all'}")
    print("    direct:    ", direct[-1].strip()[:100] if direct else "(nothing)")

    # NS_FORWARDER=1: the escape hatch - routed through the forwarder.
    tail = run_worker({"NS_FORWARDER": "1"}, "NGX calls go through")
    routed = [l for l in tail.splitlines() if "NGX calls go through" in l]
    if not routed:
        failures.append("NS_FORWARDER=1: the worker did not report which module "
                        "it calls through")
    elif "nvngx.dll" not in routed[-1]:
        failures.append(f"NS_FORWARDER=1: the module has no nvngx.dll in its "
                        f"path: {routed[-1].strip()}")
    if "Init_Ext (through the forwarder) -> 0x00000001" not in tail:
        line = [l for l in tail.splitlines() if "Init_Ext" in l]
        failures.append(f"NS_FORWARDER=1: Init_Ext did not succeed through the "
                        f"forwarder: {line[-1].strip() if line else 'no line'}")
    print("    forwarded: ", routed[-1].strip()[:100] if routed else "(nothing)")

    # The negative control: the same binary under a name the library will not
    # serve. The copy goes to a temporary directory, so neither the file name
    # nor any folder above it carries the substring.
    plain_dir = Path(tempfile.mkdtemp(prefix="ns-fwd-"))
    plain = plain_dir / "ns-forwarder.dll"
    shutil.copy2(FORWARDER, plain)
    if "nvngx.dll" in str(plain).lower():
        failures.append(f"the control copy sits at {plain} - the substring is "
                        f"in the path, the control proves nothing")
    try:
        tail = run_worker({"NS_FORWARDER": str(plain)}, "Init_Ext")
        line = [l for l in tail.splitlines() if "Init_Ext" in l]
        got = line[-1].strip() if line else ""
        print("    renamed:   ", got[:100] or "(no Init_Ext line)")
        if "0xBAD00002" not in got:
            failures.append(f"a module without the substring was NOT refused: "
                            f"{got or 'no Init_Ext line at all'} - either the "
                            f"library stopped checking, or the worker fell "
                            f"back to calling it directly")
    finally:
        shutil.rmtree(plain_dir, ignore_errors=True)

    for f in failures:
        print("FAIL:", f)
    if failures:
        return 1
    print("OK: the direct path ships, the forwarder still routes, and the "
          "library still refuses a caller without the substring")
    return 0


if __name__ == "__main__":
    sys.exit(main())
