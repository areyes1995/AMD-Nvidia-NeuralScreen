// amd_nr_host -- Phase 2b-prep "full-input" worker for the AMD backend.
//
// SAME stdin/stdout protocol as nvidia_mode/native (D5V3/FRM1/OUT1 + all
// channel acks). Difference vs nr_host_echo.cpp (Phase 2a): every neural
// input the NGX path uses is now COMPUTED from the real stream, exactly
// like dlss5-feed-host64.cpp does on the NVIDIA side:
//
//   color      -> used directly (BGRA pipe frame)
//   motion     -> fp16 2ch field from guides.py (DIS flow); mean |v| logged
//   exposure   -> PaperWhite adaptive: dark/lit fractions of the frame map
//                 into [PW_MIN, PW_MAX], smoothed with tau (mirrors
//                 UpdateAdaptiveExposure in the NVIDIA worker)
//   reset      -> fr.reset counted as temporal-history resets
//   tuning     -> intensity / local_tone / local_structure / skin_structure
//                 / auto_mask / style / ui_correction carried from the header
//                 and echoed into the per-frame dispatch record
//   depth      -> NOT used: feature-18 DLSSNR interface takes none
//                 (verified: zero DLSSNR.Depth refs in the NVIDIA worker)
//   jitter     -> NOT used: subrects fixed at 0, scale 1.0 (no camera)
//
// The dispatch is real when the DLSS-NR HIP runtime is installed next to
// this worker (dlssnr_engine.cpp hosts it; docs/AMD_HIP_HOSTING.md explains
// how and why). Without it -- no runtime, no weights, another GPU -- the
// worker keeps the byte-identical passthrough instead of failing, and says
// which path it took on stderr. NS_AMD_NR=0 forces passthrough.
//
// Telemetry goes to stderr, parseable: "[nr] f=.. exp=.. mv=.. resets=..
// .. neural=..", where neural counts the frames the HIP network actually ran.
//
// Build: build-amd.bat (MSVC 2022 + D3D12/DXGI; the HIP runtime is loaded
// at run time, never linked).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <fcntl.h>
#include <io.h>
#include <vector>
#include <string>
#include <atomic>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "dlssnr_engine.h"

#pragma pack(push, 1)
struct Header { // D5V3 / RNSZ, 64 bytes (HEADER_FMT "<10I4f2I")
    uint32_t magic, w, h, warmup, frame_count;
    uint32_t profile, preset, style, auto_mask, ui_correction;
    float intensity, local_tone, local_structure, skin_structure;
    uint32_t full_w, full_h;
};
struct Frame { // FRM1 / CAP1, 24 bytes (FRAME_FMT "<4Iq")
    uint32_t magic, index, reset, flags;
    int64_t pts;
};
struct Out { // OUT1, 28 bytes (OUT_FMT "<5Iq")
    uint32_t magic, index, ok, bytes, ngx_result;
    int64_t pts;
};
struct Ack { // SACK/MACK/WACK/DACK/OAK2/GAK/RACK, 24 bytes ("<4Iq")
    uint32_t magic, ok, a, b;
    int64_t pts;
};
struct Shm { // SHMI, 88 bytes (SHM_FMT "<4Iq64s")
    uint32_t magic, color_bytes, motion_bytes, flags;
    int64_t pts;
    char name[64];
};
struct GrayOut { // GRAY/OUTS, 88 bytes ("<4Iq64s")
    uint32_t magic, w, h, flags;
    int64_t pts;
    char name[64];
};
struct Wgc { // WGCW, 32 bytes ("<4IqQ")
    uint32_t magic, w, h, flags;
    int64_t pts;
    uint64_t hwnd;
};
struct Small { // MOTS/WNDO/DDA1, 24 bytes ("<4Iq")
    uint32_t magic, a, b, flags;
    int64_t pts;
};
#pragma pack(pop)

static_assert(sizeof(Header) == 64, "header != 64");
static_assert(sizeof(Frame) == 24, "frame != 24");
static_assert(sizeof(Out) == 28, "out != 28");
static_assert(sizeof(Ack) == 24, "ack != 24");
static_assert(sizeof(Shm) == 88, "shmi != 88");
static_assert(sizeof(GrayOut) == 88, "gray/outs != 88");
static_assert(sizeof(Wgc) == 32, "wgc != 32");
static_assert(sizeof(Small) == 24, "small != 24");

namespace {
constexpr uint32_t kVideo = 0x33563544;   // D5V3
constexpr uint32_t kResize = 0x5A534E52;  // RNSZ
constexpr uint32_t kFrame = 0x314D5246;   // FRM1
constexpr uint32_t kPrep = 0x31504143;    // CAP1
constexpr uint32_t kOut = 0x3154554F;     // OUT1
constexpr uint32_t kShm = 0x494D4853;     // SHMI
constexpr uint32_t kSack = 0x4B434153;    // SACK
constexpr uint32_t kMotion = 0x53544F4D;  // MOTS
constexpr uint32_t kMack = 0x4B43414D;    // MACK
constexpr uint32_t kWndo = 0x4F444E57;    // WNDO
constexpr uint32_t kWack = 0x4B434157;    // WACK
constexpr uint32_t kDda = 0x31414444;     // DDA1
constexpr uint32_t kDack = 0x4B434144;    // DACK
constexpr uint32_t kWgc = 0x57434757;     // WGCW
constexpr uint32_t kWgak = 0x4B414757;    // WGAK
constexpr uint32_t kOuts = 0x5354554F;    // OUTS
constexpr uint32_t kOak = 0x324B414F;     // OAK2
constexpr uint32_t kGray = 0x59415247;    // GRAY
constexpr uint32_t kGak = 0x4B434147;     // GAK
constexpr uint32_t kRack = 0x4B434152;    // RACK
constexpr uint32_t kFlagNoColor = 0x8;

// PaperWhite adaptive exposure (mirrors the NVIDIA worker principle:
// static exposure leaves dark scenes underexposed). Tunables match the
// NS_PW_* env knobs of dlss5-feed-host64.cpp.
constexpr float kPwMin = 0.5f;
constexpr float kPwMax = 2.0f;
constexpr float kPwTau = 0.5f;      // smoothing time constant (s)
constexpr float kDarkThr = 0.06f;   // luma below = "dark"
constexpr float kLitThr = 0.75f;    // luma above = "lit"

bool ReadExact(void* dst, size_t n) {
    auto* p = static_cast<uint8_t*>(dst);
    while (n > 0) {
        size_t got = fread(p, 1, n, stdin);
        if (got == 0) return false;  // EOF -> graceful exit
        p += got;
        n -= got;
    }
    return true;
}
void WriteAll(const void* src, size_t n) {
    fwrite(src, 1, n, stdout);
    fflush(stdout);
}
void SendAck(uint32_t magic, uint32_t ok, int64_t pts = 0) {
    Ack a{magic, ok, 0, 0, pts};
    WriteAll(&a, sizeof(a));
}

// fp16 -> float (motion field is fp16 2ch, like the NGX MVLowRes path).
float HalfToFloat(uint16_t h) {
    uint32_t sign = (h >> 15) & 1;
    uint32_t exp = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FF;
    uint32_t f;
    if (exp == 0) {
        if (mant == 0) f = 0;
        else {  // subnormal -> normalize
            exp = 1;
            while ((mant & 0x400) == 0) { mant <<= 1; exp--; }
            mant &= 0x3FF;
            f = (sign << 31) | ((exp + 112) << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        f = (sign << 31) | (0xFF << 23) | (mant << 13);  // inf/nan
    } else {
        f = (sign << 31) | ((exp + 112) << 23) | (mant << 13);
    }
    float out;
    memcpy(&out, &f, 4);
    return out;
}

// One dispatch record: everything Phase 2b's HIP call needs, in one place.
struct Dispatch {
    uint32_t frame_index = 0;
    uint32_t reset = 0;
    float exposure = 1.0f;      // DLSS.Exposure.Scale equivalent
    float pre_exposure = 1.0f;  // DLSS.Pre.Exposure equivalent
    float mv_mean = 0.0f;       // mean |v| of the motion field (px @ work res)
    float intensity = 0.0f, local_tone = 0.0f, local_structure = 0.0f;
    float skin_structure = 0.0f;
    uint32_t auto_mask = 0, style = 0, ui_correction = 0;
};

// The fallback dispatch: byte-identical output. Used until the HIP engine is
// up (it comes up on its own thread), for every frame it refuses, and on any
// machine without the runtime.
bool DispatchPassthrough(const uint8_t* color, size_t bytes,
                         const Dispatch& /*d*/, std::vector<uint8_t>& out) {
    if (out.size() != bytes) out.assign(bytes, 0);
    if (bytes) memcpy(out.data(), color, bytes);
    return true;
}
}  // namespace

int main(int argc, char** argv) {
    bool live = argc > 1 && strcmp(argv[1], "--live") == 0;
    if (!live) {
        fprintf(stderr, "amd_nr_host: full-input worker, use --live\n");
        return 2;
    }
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    setvbuf(stdout, nullptr, _IONBF, 0);

    Header hdr{};
    if (!ReadExact(&hdr, sizeof(hdr)) || (hdr.magic != kVideo && hdr.magic != kResize)) {
        fprintf(stderr, "[host] amd full: no header, exiting\n");
        return 1;
    }
    uint32_t work_w = hdr.w, work_h = hdr.h;
    uint32_t full_w = hdr.full_w, full_h = hdr.full_h;
    uint32_t mot_w = work_w, mot_h = work_h;  // MOTS overrides
    auto out_size = [&] {
        uint32_t ow = full_w ? full_w : work_w;
        uint32_t oh = full_h ? full_h : work_h;
        return (size_t)ow * oh * 4;
    };
    fprintf(stderr,
            "[host] amd full-input worker ready: work %ux%u full %ux%u "
            "int=%.2f tone=%.2f struct=%.2f skin=%.2f mask=%u style=%u uic=%u\n",
            work_w, work_h, full_w, full_h, hdr.intensity, hdr.local_tone,
            hdr.local_structure, hdr.skin_structure, hdr.auto_mask, hdr.style,
            hdr.ui_correction);

    // The neural pass, if this machine has the runtime next to us. It is a
    // best-effort upgrade: no runtime, no weights or an unsupported GPU and we
    // keep serving passthrough frames rather than failing the session.
    // NS_AMD_NR=0 forces passthrough (used by the A/B harness).
    // ...and it comes up on its own thread. Bringing it up costs a few
    // seconds (waiting for the runtime's hooks, then building the upscaler
    // context), and main gives a worker five seconds per frame before it
    // declares it silent and restarts it. So the frames keep flowing as
    // passthrough from the first one and the neural pass takes over when it
    // is ready, instead of the session hanging on frame 0.
    nsamd::DlssNrEngine engine;
    std::atomic<bool> engine_live{false};
    std::thread engine_thread;
    bool engine_tried = false;
    auto engine_start = [&] {
        if (engine_tried) return;
        engine_tried = true;
        char want[8]{};
        GetEnvironmentVariableA("NS_AMD_NR", want, sizeof(want));
        if (want[0] == '0') {
            fprintf(stderr, "[host] amd full: NS_AMD_NR=0, passthrough by request\n");
            return;
        }
        uint32_t ow = full_w ? full_w : work_w;
        uint32_t oh = full_h ? full_h : work_h;
        // The host window is created here, on the thread that serves frames
        // and therefore pumps its messages; everything slow happens on the
        // thread below. See DlssNrEngine::Prepare.
        std::string why;
        if (!engine.Prepare(ow, oh, why)) {
            fprintf(stderr, "[host] amd full: neural pass off (%s); passthrough\n",
                    why.c_str());
            return;
        }
        engine_thread = std::thread([&engine, &engine_live, ow, oh] {
            std::string why;
            if (engine.Start(ow, oh, why)) {
                fprintf(stderr, "[host] amd full: neural pass ON (HIP runtime, %ux%u)\n",
                        ow, oh);
                engine_live.store(true, std::memory_order_release);
            } else {
                fprintf(stderr, "[host] amd full: neural pass off (%s); passthrough\n",
                        why.c_str());
            }
        });
    };
    // The loop leaves through a dozen `return`s on EOF, and a joinable thread
    // at destruction is std::terminate. Declared after both, so it joins
    // before the engine it is still writing to goes away.
    struct Joiner {
        std::thread& t;
        ~Joiner() { if (t.joinable()) t.join(); }
    } joiner{engine_thread};

    float exposure = 1.0f;
    uint64_t frames = 0, resets = 0, neural = 0;
    std::vector<uint8_t> frame, mv, out;
    for (;;) {
        uint32_t magic = 0;
        if (!ReadExact(&magic, 4)) return 0;  // stdin closed -> clean exit
        if (magic == kFrame || magic == kPrep) {
            Frame fr{};
            fr.magic = magic;
            if (!ReadExact(((uint8_t*)&fr) + 4, sizeof(fr) - 4)) return 0;
            size_t color = (size_t)(full_w ? full_w : work_w) * (full_h ? full_h : work_h) * 4;
            size_t motion = (size_t)mot_w * mot_h * 4;  // fp16 2ch
            bool no_color = (magic == kPrep) || (fr.flags & kFlagNoColor);
            bool in_shm = (fr.flags & 0x1) != 0;  // never set: SACK refused
            if (frame.size() != out_size()) frame.assign(out_size(), 0);
            if (!no_color && !in_shm) {
                if (!ReadExact(frame.data(), color)) return 0;
                if (mv.size() != motion) mv.resize(motion);
                if (motion && !ReadExact(mv.data(), motion)) return 0;
            } else {
                if (mv.size() != motion) mv.resize(motion);
                if (motion && !in_shm && !ReadExact(mv.data(), motion)) return 0;
                if (in_shm || no_color) std::fill(frame.begin(), frame.end(), 0);
            }

            // --- exposure: PaperWhite over the real colour (stride sample) ---
            size_t px = color / 4;
            size_t step = px > 200000 ? px / 200000 : 1;  // ~200k samples max
            double luma_sum = 0.0;
            uint64_t dark = 0, lit = 0, n = 0;
            for (size_t i = 0; i < px; i += step) {
                const uint8_t* p = frame.data() + i * 4;
                float luma = (0.2126f * p[2] + 0.7152f * p[1] + 0.0722f * p[0]) / 255.0f;
                luma_sum += luma;
                if (luma < kDarkThr) ++dark;
                if (luma > kLitThr) ++lit;
                ++n;
            }
            float target = exposure;
            if (n) {
                float avg = (float)(luma_sum / n);
                float dark_f = (float)dark / n, lit_f = (float)lit / n;
                // Dark scenes need exposure UP, bright scenes stay near 1.0.
                target = 1.0f + (0.35f - avg) * 2.0f + dark_f * 0.5f - lit_f * 0.3f;
                target = std::clamp(target, kPwMin, kPwMax);
            }
            float a = 1.0f - expf(-1.0f / (kPwTau * 60.0f));  // per-frame @60fps
            exposure += (target - exposure) * a;

            // --- motion stats: mean |v| over the fp16 2ch field ---
            float mv_mean = 0.0f;
            size_t mv_px = motion / 4;
            if (mv_px && mv.size() >= motion) {
                double acc = 0.0;
                const uint16_t* h = reinterpret_cast<const uint16_t*>(mv.data());
                size_t mstep = mv_px > 50000 ? mv_px / 50000 : 1;
                uint64_t mn = 0;
                for (size_t i = 0; i < mv_px; i += mstep) {
                    float vx = HalfToFloat(h[i * 2]), vy = HalfToFloat(h[i * 2 + 1]);
                    if (std::isfinite(vx) && std::isfinite(vy)) {
                        acc += sqrtf(vx * vx + vy * vy);
                        ++mn;
                    }
                }
                if (mn) mv_mean = (float)(acc / mn);
            }

            if (fr.reset) ++resets;
            Dispatch d;
            d.frame_index = fr.index;
            d.reset = fr.reset;
            d.exposure = exposure;
            d.mv_mean = mv_mean;
            d.intensity = hdr.intensity;
            d.local_tone = hdr.local_tone;
            d.local_structure = hdr.local_structure;
            d.skin_structure = hdr.skin_structure;
            d.auto_mask = hdr.auto_mask;
            d.style = hdr.style;
            d.ui_correction = hdr.ui_correction;

            engine_start();
            bool ok = false;
            if (engine_live.load(std::memory_order_acquire) && engine.Ready()) {
                // A resize can land while the engine is still coming up, so
                // the size it started with is not necessarily the one we are
                // serving now.
                uint32_t ow = full_w ? full_w : work_w;
                uint32_t oh = full_h ? full_h : work_h;
                if (engine.width() != ow || engine.height() != oh) {
                    std::string why;
                    if (!engine.Resize(ow, oh, why)) {
                        fprintf(stderr, "[host] amd full: neural pass lost on resize"
                                " (%s); passthrough\n", why.c_str());
                        engine_live.store(false, std::memory_order_release);
                    }
                }
            }
            if (engine_live.load(std::memory_order_acquire) && engine.Ready()) {
                nsamd::FrameParams fp;
                fp.index = fr.index;
                fp.reset = fr.reset;
                fp.exposure = exposure;
                ok = engine.Dispatch(frame.data(), out_size(), fp, out);
                if (ok) {
                    ++neural;
                } else if (frames == 0 || (frames % 120) == 0) {
                    // Say it once in a while rather than per frame: a dropped
                    // job falls back to passthrough, it does not kill the run.
                    fprintf(stderr, "[host] amd full: neural job %u failed, passthrough\n",
                            fr.index);
                }
            }
            if (!ok && !DispatchPassthrough(frame.data(), out_size(), d, out)) {
                Out o{kOut, fr.index, 0, 0, 0, fr.pts};
                WriteAll(&o, sizeof(o));
                continue;
            }
            ++frames;
            // The first neural frame gets its own line: the engine comes up
            // asynchronously, so "when did the pass actually take over" is not
            // something the 60-frame cadence can answer.
            if (frames == 1 || frames % 60 == 0 || neural == 1) {
                fprintf(stderr,
                        "[nr] f=%llu exp=%.3f mv=%.3f resets=%llu "
                        "int=%.2f tone=%.2f struct=%.2f skin=%.2f mask=%u style=%u uic=%u "
                        "neural=%llu\n",
                        (unsigned long long)frames, d.exposure, d.mv_mean,
                        (unsigned long long)resets, d.intensity, d.local_tone,
                        d.local_structure, d.skin_structure, d.auto_mask,
                        d.style, d.ui_correction, (unsigned long long)neural);
            }
            Out o{kOut, fr.index, 1, (uint32_t)out.size(), 1, fr.pts};
            WriteAll(&o, sizeof(o));
            if (!out.empty()) WriteAll(out.data(), out.size());
        } else if (magic == kShm) {
            Shm m{};
            m.magic = magic;
            if (!ReadExact(((uint8_t*)&m) + 4, sizeof(m) - 4)) return 0;
            SendAck(kSack, 0, m.pts);  // refuse: Python falls back to the pipe
        } else if (magic == kMotion) {
            Small m{};
            m.magic = magic;
            if (!ReadExact(((uint8_t*)&m) + 4, sizeof(m) - 4)) return 0;
            if (m.a && m.b) { mot_w = m.a; mot_h = m.b; }
            SendAck(kMack, 1, m.pts);
        } else if (magic == kResize) {
            Header r{};
            r.magic = magic;
            if (!ReadExact(((uint8_t*)&r) + 4, sizeof(r) - 4)) return 0;
            work_w = r.w; work_h = r.h; full_w = r.full_w; full_h = r.full_h;
            mot_w = work_w; mot_h = work_h;
            hdr = r;  // tuning params follow the resize
            fprintf(stderr, "[host] amd full: resize %ux%u full %ux%u\n",
                    work_w, work_h, full_w, full_h);
            // Nothing to do for the engine here: the frame path resizes it,
            // which also covers a resize that lands while it is still coming
            // up on its own thread.
            SendAck(kRack, 1, 0);
        } else if (magic == kWndo) {
            Small m{};
            m.magic = magic;
            if (!ReadExact(((uint8_t*)&m) + 4, sizeof(m) - 4)) return 0;
            SendAck(kWack, 0, m.pts);  // refuse: pygame output stays
        } else if (magic == kDda) {
            Small m{};
            m.magic = magic;
            if (!ReadExact(((uint8_t*)&m) + 4, sizeof(m) - 4)) return 0;
            SendAck(kDack, 0, m.pts);  // refuse: Python-side capture stays
        } else if (magic == kWgc) {
            Wgc m{};
            m.magic = magic;
            if (!ReadExact(((uint8_t*)&m) + 4, sizeof(m) - 4)) return 0;
            SendAck(kWgak, 0, m.pts);
        } else if (magic == kGray || magic == kOuts) {
            GrayOut m{};
            m.magic = magic;
            if (!ReadExact(((uint8_t*)&m) + 4, sizeof(m) - 4)) return 0;
            SendAck(magic == kGray ? kGak : kOak, 0, m.pts);  // refuse
        } else {
            fprintf(stderr, "[host] amd full: unknown magic 0x%08X, exiting\n", magic);
            return 1;
        }
    }
}
