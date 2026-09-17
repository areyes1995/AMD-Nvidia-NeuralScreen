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
// Sizes matter as much as any of that. The pipeline hands us a full-resolution
// frame and expects one back, while `work` is the resolution the network is
// meant to run at - that is the whole point of the scale slider. So the pass
// is: full-res frame in -> GPU downscale to work res -> network -> upscaled to
// full res by FSR -> full-res frame out. Running the network at output
// resolution (which is what the first version did) costs multiples of the
// frame time and makes the slider do nothing.
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

// The menu's effect controls. The hosted runtime does not take parameters
// through the dispatch: it reads its own ini. So this is the bridge, and
// without it the sliders move and nothing happens - which is exactly what
// they did on AMD.
//
// `intensity` drives the runtime's `Scale`, which is the strength of the
// network's contribution. Its shipped default was 0.03125: at 3% the pass is
// measurable (0.56/255) and invisible. NS_AMD_NR_SCALE_MAX sets what
// intensity 1.0 means, because the useful range depends on the content.
struct EffectParams {
    float intensity = 1.0f;
    float local_tone = 0.0f;
    float local_structure = 1.0f;
    float skin_structure = -1.0f;
    uint32_t auto_mask = 1;
    uint32_t tone_channels = 0;

    bool operator==(const EffectParams& o) const {
        return intensity == o.intensity && local_tone == o.local_tone &&
               local_structure == o.local_structure &&
               skin_structure == o.skin_structure && auto_mask == o.auto_mask &&
               tone_channels == o.tone_channels;
    }
    bool operator!=(const EffectParams& o) const { return !(*this == o); }
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
    bool Prepare(uint32_t out_w, uint32_t out_h, std::string& why);

    // Brings the runtime up: frames arrive and leave at out_w x out_h, the
    // network runs at work_w x work_h. Call Prepare() first. False means "no
    // neural pass": `why` says what was missing, and the caller keeps
    // passthrough.
    bool Start(uint32_t work_w, uint32_t work_h, uint32_t out_w, uint32_t out_h,
               std::string& why);
    bool Ready() const { return ready_; }

    // Same frame contract as the worker's OUT1: `bgra_in` is out_w*out_h*4,
    // and the result is written to `bgra_out`, which must hold as many bytes.
    // Both are raw pointers so the caller can hand us the shared-memory slots
    // directly - at 1440p a spare copy of a frame is several milliseconds.
    // False means this frame failed; the caller answers ok=0 rather than
    // pretending.
    bool Dispatch(const uint8_t* bgra_in, uint8_t* bgra_out, size_t bytes,
                  const FrameParams& p);

    bool Resize(uint32_t work_w, uint32_t work_h, uint32_t out_w, uint32_t out_h,
                std::string& why);

    // Writes the runtime's ini from the menu's values. Call it before Start()
    // and whenever the parameters change; the runtime picks the file up while
    // it runs. A no-op when nothing moved.
    void SetEffect(const EffectParams& p);

    // Quiesces the GPU and stops using the runtime. It deliberately does not
    // tear the D3D12 objects down: a third party's detours and worker threads
    // are still live in this process, and releasing objects they hold is a
    // crash we cannot fix from out here. The process is about to exit anyway.
    void Stop();

    uint32_t work_width() const { return work_w_; }
    uint32_t work_height() const { return work_h_; }
    uint32_t out_width() const { return out_w_; }
    uint32_t out_height() const { return out_h_; }

private:
    void WriteIni() const;

    struct Impl;
    Impl* impl_ = nullptr;
    bool ready_ = false;
    uint32_t work_w_ = 0, work_h_ = 0;
    uint32_t out_w_ = 0, out_h_ = 0;
    EffectParams effect_{};
    std::string ini_path_;
};

}  // namespace nsamd
