// dlssnr_engine.h -- the real neural pass for the AMD worker.
//
// It hosts the DLSS-NR on AMD runtime (a standalone version.dll proxy) inside
// our own process: we build a D3D12 device, a swapchain and an FSR upscaler
// dispatch, the runtime detours those and runs its HIP network on the frame we
// hand it. docs/AMD_HIP_HOSTING.md has the evidence and the reverse
// engineering behind every step here; the short version of what it cost to
// find out:
//
//   * the runtime must be in the process before we build anything D3D12, and
//     its hooks land from a thread it starts on load, so we wait for them -
//     a swapchain created too early is never recorded in its swapchain ->
//     command queue map and every frame is dropped as "not on our device";
//   * it only arms the upscaler hook when `UseFsrInputs=1` in its own ini;
//   * it needs its weights file next to the DLL.
//
// Nothing here is required for the worker to run: with no runtime on disk
// Start() fails with a reason and the worker stays on passthrough.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nsamd {

// What the network is told about this frame. Mirrors the worker's Dispatch
// record; the engine only needs the parts the upscaler contract carries.
struct FrameParams {
    uint32_t index = 0;
    uint32_t reset = 0;
    float exposure = 1.0f;
    float frame_time_ms = 16.6f;
};

class DlssNrEngine {
public:
    DlssNrEngine() = default;
    ~DlssNrEngine();
    DlssNrEngine(const DlssNrEngine&) = delete;
    DlssNrEngine& operator=(const DlssNrEngine&) = delete;

    // Creates the host window, and nothing else. It has to run on the thread
    // that will later call Dispatch: the swapchain's window must be owned by
    // a thread that pumps messages, and DXGI blocks inside Present when it is
    // not. Start() takes seconds, so it is meant to run on a worker thread -
    // which is exactly the thread that must not own this window.
    bool Prepare(uint32_t w, uint32_t h, std::string& why);

    // Brings the runtime up for `w`x`h` BGRA frames; call Prepare() first.
    // False means "no neural pass": `why` says what was missing, and the
    // caller keeps passthrough.
    bool Start(uint32_t w, uint32_t h, std::string& why);
    bool Ready() const { return ready_; }

    // Same frame size contract as the worker's OUT1: `bgra` is w*h*4 in, and
    // `out` comes back the same size. False means this frame failed; the
    // caller answers ok=0 rather than pretending.
    bool Dispatch(const uint8_t* bgra, size_t bytes, const FrameParams& p,
                  std::vector<uint8_t>& out);

    bool Resize(uint32_t w, uint32_t h, std::string& why);
    void Stop();

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    bool ready_ = false;
    uint32_t width_ = 0, height_ = 0;
};

}  // namespace nsamd
