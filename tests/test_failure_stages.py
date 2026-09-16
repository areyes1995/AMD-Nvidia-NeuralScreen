r"""Native failure stages are explicit and fatal fence failures stop GPU use.

Run: runtime\python.exe tests\test_failure_stages.py
"""
from __future__ import annotations

import os
from pathlib import Path
import subprocess
import struct
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
NATIVE = ROOT / "nvidia_mode" / "native"
SOURCE = NATIVE / "dlss5-feed-host64.cpp"


def check_source_contract() -> None:
    source = SOURCE.read_text(encoding="utf-8")
    required = (
        'TestFailureOnce("requirements")',
        'TestFailureOnce("create")',
        '"first-evaluate" : "steady-evaluate"',
        'TestFailureOnce(failure_stage)',
        'TestFailureOnce("present")',
        'TestFailureOnce("fence-timeout")',
        'TestFailureOnce("device-removed")',
        '"input-fence", false',
    )
    for marker in required:
        assert marker in source, f"missing native failure-stage contract: {marker}"

    resize = source.index('"resize-drain"')
    release = source.index("SafeReleaseFeature(h.feature);", resize)
    resources = source.index("ReleaseVideoTextures(v);", resize)
    assert resize < release < resources, (
        "RNSZ must prove retirement before releasing the feature or textures"
    )

    nvofa = (NATIVE / "nvofa.inl").read_text(encoding="utf-8")
    close = nvofa.index("static void CloseNvofa()")
    unregister = nvofa.index("nvOFUnregisterResourceD3D12", close)
    assert nvofa.index("if (g_submission_failed)", close, unregister) < unregister

    fg = (NATIVE / "frame_generation.inl").read_text(encoding="utf-8")
    stop = fg.index("static void StopFgPresentation()")
    clear = fg.index("for (auto &slot : g_fg.slots)", stop)
    assert fg.index("if (g_submission_failed)", stop, clear) < clear
    print("OK: all release stages are injectable and failed fences gate resource release")


def build_harness(work: Path) -> Path:
    vswhere = (
        Path(os.environ["ProgramFiles(x86)"])
        / "Microsoft Visual Studio/Installer/vswhere.exe"
    )
    install = subprocess.check_output(
        [
            str(vswhere),
            "-latest",
            "-products",
            "*",
            "-requires",
            "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
            "-property",
            "installationPath",
        ],
        text=True,
    ).strip()
    vcvars = Path(install) / "VC/Auxiliary/Build/vcvars64.bat"
    assert vcvars.is_file(), "MSVC compiler environment unavailable"

    source = work / "failure_stages.cpp"
    source.write_text(
        r'''
#define main neural_app_main
#include "SOURCE"
#undef main
#include <cassert>
#include <chrono>

int main() {
    char stage[48] = {};
    GetEnvironmentVariableA("NS_TEST_FAIL_STAGE", stage, sizeof(stage));
    assert(SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                                       IID_PPV_ARGS(&h.dev))));
    D3D12_COMMAND_QUEUE_DESC q = {};
    assert(SUCCEEDED(h.dev->CreateCommandQueue(&q, IID_PPV_ARGS(&h.queue))));
    for (int i = 0; i < Host::kFrames; ++i)
        assert(SUCCEEDED(h.dev->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&h.alloc[i]))));
    assert(SUCCEEDED(h.dev->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, h.alloc[0], nullptr,
        IID_PPV_ARGS(&h.list))));
    assert(SUCCEEDED(h.list->Close()));
    assert(SUCCEEDED(h.dev->CreateFence(
        0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&h.fence))));
    h.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    assert(h.fence_event != nullptr);

    if (strcmp(stage, "present") == 0) {
        assert(!PresentStatus(S_OK, "test-present"));
        assert(g_submission_failed);
    } else {
        assert(BeginCommands());
        const UINT64 value = EndCommands();
        assert(value != 0);
        const auto started = std::chrono::steady_clock::now();
        const bool waited = WaitFenceValue(
            h.fence, value, 2000, "test-fence");
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        if (stage[0] == '\0') {
            assert(waited && !g_submission_failed);
        } else {
            assert(!waited && g_submission_failed);
            assert(elapsed < 500); // injected timeout, not a real two-second wait
        }
    }

    if (stage[0] != '\0') {
        assert(!BeginCommands());
        FILE *out = tmpfile();
        const char byte = 0;
        assert(out && !WriteExact(out, &byte, 1) && ftell(out) == 0);
        fclose(out);
    }
    return 0;
}
'''.replace("SOURCE", SOURCE.as_posix()),
        encoding="utf-8",
    )
    build = work / "build.bat"
    build.write_text(
        f'''@echo off
call "{vcvars}" >nul
if errorlevel 1 exit /b 1
cl /nologo /O2 /EHsc /W3 /MD /std:c++17 /I"{NATIVE / 'include'}" /I"{NATIVE / 'src'}" "{source}" "{NATIVE / 'spout_bridge.cpp'}" /Fe:"{work / 'failure_stages.exe'}" /link "{NATIVE / 'lib/Windows_x86_64/x64/nvsdk_ngx_d.lib'}" "{NATIVE / 'SpoutDX.lib'}" version.lib kernel32.lib user32.lib gdi32.lib advapi32.lib ole32.lib d3d11.lib d3d12.lib dxgi.lib d3dcompiler.lib WindowsApp.lib dwmapi.lib
''',
        encoding="ascii",
    )
    built = subprocess.run(
        [os.environ["COMSPEC"], "/d", "/c", str(build)],
        cwd=work,
        capture_output=True,
        text=True,
        timeout=120,
    )
    assert built.returncode == 0, built.stdout + built.stderr
    return work / "failure_stages.exe"


def check_runtime_injection() -> None:
    expected = {
        "": None,
        "fence-timeout": "stage=test-fence kind=fence-timeout",
        "device-removed": "stage=test-fence kind=device-removed",
        "present": "stage=present kind=present-error",
    }
    with tempfile.TemporaryDirectory(prefix="ns-failure-stages-") as temp:
        exe = build_harness(Path(temp))
        for stage, marker in expected.items():
            env = dict(os.environ, PATH=str(NATIVE) + os.pathsep + os.environ["PATH"])
            if stage:
                env["NS_TEST_FAIL_STAGE"] = stage
            else:
                env.pop("NS_TEST_FAIL_STAGE", None)
            checked = subprocess.run(
                [str(exe)],
                cwd=exe.parent,
                env=env,
                capture_output=True,
                text=True,
                timeout=15,
            )
            output = checked.stdout + checked.stderr
            assert checked.returncode == 0, output
            if marker is not None:
                assert marker in output, (stage, output)
                assert "[test] injecting failure" in output, (stage, output)
    print("OK: fence timeout, present error and device removal stop immediately")


def check_ngx_stage_injection() -> None:
    sys.path.insert(0, str(ROOT))
    from main import (  # noqa: PLC0415
        FRAME_FLAG_WANT_PIXELS,
        FRAME_FMT,
        FRAME_MAGIC,
        HEADER_FMT,
        NATIVE_DIR,
        PROFILES,
        VIDEO_MAGIC,
        WORKER_EXE,
    )
    from protocol import WorkerReader  # noqa: PLC0415

    width, height = 320, 180
    params = PROFILES["Natural"]
    color = bytes((41, 97, 173, 255)) * (width * height)
    motion = bytes(width * height * 4)  # two float16 components per pixel

    cases = {
        "requirements": (1, None),
        "create": (1, None),
        "first-evaluate": (1, 7),
        "steady-evaluate": (2, 7),
    }
    for stage, (warmup, expected_code) in cases.items():
        header = struct.pack(
            HEADER_FMT,
            VIDEO_MAGIC,
            width,
            height,
            warmup,
            0,
            0,
            0,
            int(params["style"]),
            int(params["auto_mask"]),
            int(params.get("ui_correction", 0)),
            float(params["intensity"]),
            float(params["local_tone"]),
            float(params["local_structure"]),
            float(params["skin_structure"]),
            0,
            0,
        )
        frame = struct.pack(
            FRAME_FMT, FRAME_MAGIC, 0, 1, FRAME_FLAG_WANT_PIXELS, 0
        )
        env = dict(
            os.environ,
            NS_TEST_FAIL_STAGE=stage,
            NS_FRAMEGEN="0",
            NS_NR_SMALL="0",
        )
        worker = subprocess.Popen(
            [str(WORKER_EXE), "--live"],
            cwd=NATIVE_DIR,
            env=env,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        reader = WorkerReader(worker, width, height)
        try:
            worker.stdin.write(header + frame + color + motion)
            worker.stdin.flush()
            if expected_code is None:
                pixels = reader.recv(0, 30)
                assert pixels is not None and pixels.shape == (height, width, 4)
                if stage == "create":
                    assert tuple(pixels[0, 0]) == (41, 97, 173, 255)
            else:
                assert worker.wait(timeout=30) == expected_code
        finally:
            if worker.poll() is None:
                worker.terminate()
                try:
                    worker.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    worker.kill()
                    worker.wait(timeout=10)
            log = worker.stderr.read().decode("utf-8", "replace")
        assert f"[test] injecting failure at stage={stage}" in log, (stage, log)
        assert f"[failure] stage={stage}" in log, (stage, log)
    print("OK: requirements, CreateFeature, first Evaluate and steady Evaluate are distinct")


def main() -> int:
    check_source_contract()
    check_runtime_injection()
    check_ngx_stage_injection()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(f"FAIL: {exc}")
        raise
