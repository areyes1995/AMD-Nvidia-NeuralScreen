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

// The menu's effect values as the hosted runtime wants them. Everything the
// header carries that the runtime has a knob for; `intensity` becomes its
// `Scale`, the strength of the network's contribution.
nsamd::EffectParams EffectFromHeader(const Header& h) {
    nsamd::EffectParams p;
    p.intensity = h.intensity;
    p.local_tone = h.local_tone;
    p.local_structure = h.local_structure;
    p.skin_structure = h.skin_structure;
    p.auto_mask = h.auto_mask;
    p.tone_channels = 0;
    return p;
}

constexpr uint32_t kOutBytesInShm = 0xFFFFFFFFu;  // OUT1.bytes sentinel
constexpr uint32_t kFlagShm = 0x1;                // FRM1: colour is in SHMI

// --- Shared memory, both directions ----------------------------------------
// The pipe costs more than the network does: a 2560x1440 frame is 14.7 MB
// each way, and that was ~45 ms of an 81 ms frame. Python already offers both
// channels and the worker used to refuse them (SACK/OAK2 with ok=0); these
// mirror the NVIDIA worker's layout exactly, so the client side needs nothing.
struct SharedIn {
    HANDLE file = nullptr;
    const uint8_t* base = nullptr;
    size_t bytes = 0;
    size_t motion_off = 0;

    void Close() {
        if (base) { UnmapViewOfFile(base); base = nullptr; }
        if (file) { CloseHandle(file); file = nullptr; }
        bytes = 0; motion_off = 0;
    }
    bool Open(const char* name, uint32_t color_bytes, uint32_t motion_bytes) {
        Close();
        const size_t need = (size_t)color_bytes + (size_t)motion_bytes;
        if (!color_bytes || !motion_bytes || need > ((size_t)1 << 31)) return false;
        file = OpenFileMappingA(FILE_MAP_READ, FALSE, name);
        if (!file) return false;
        base = (const uint8_t*)MapViewOfFile(file, FILE_MAP_READ, 0, 0, need);
        if (!base) { CloseHandle(file); file = nullptr; return false; }
        bytes = need;
        motion_off = color_bytes;
        return true;
    }
};

// [0..8) uint64 seqlock, odd while writing; [8..) the RGBA8 frame.
struct SharedOut {
    HANDLE file = nullptr;
    uint8_t* map = nullptr;
    size_t bytes = 0;      // seqlock included
    uint64_t seq = 0;

    void Close() {
        if (map) { UnmapViewOfFile(map); map = nullptr; }
        if (file) { CloseHandle(file); file = nullptr; }
        bytes = 0;
    }
    bool Open(const char* name, uint32_t w, uint32_t h) {
        Close();
        if (!w || !h) return true;  // "off" is not a failure
        const size_t need = (size_t)w * h * 4 + 8;
        file = OpenFileMappingA(FILE_MAP_WRITE, FALSE, name);
        if (!file) return false;
        map = (uint8_t*)MapViewOfFile(file, FILE_MAP_WRITE, 0, 0, 0);
        if (!map) { CloseHandle(file); file = nullptr; return false; }
        // A section is not a file: its size cannot be asked for directly, so
        // measure the mapped region instead (same reasoning as the NVIDIA
        // worker, and the same bug if you skip it).
        MEMORY_BASIC_INFORMATION mbi{};
        const size_t have = (VirtualQuery(map, &mbi, sizeof(mbi)) == sizeof(mbi))
                                ? (size_t)mbi.RegionSize : 0u;
        if (have < need) { Close(); return false; }
        bytes = need;
        return true;
    }
    // The slot to write `n` bytes of frame into, or null when this channel
    // cannot take it. Only an exact fit: a short frame would leave stale bytes
    // in the tail and the client copies the whole slot.
    uint8_t* Slot(size_t n) {
        return (map && n && n + 8 == bytes) ? map + 8 : nullptr;
    }
    // The seqlock around a write straight into the slot: odd while writing,
    // even when done, so the client can tell a torn frame.
    void BeginWrite() {
        if (!map) return;
        uint64_t s = ++seq;
        if ((s & 1) == 0) ++s;
        seq = s;
        memcpy(map, &s, sizeof(s));
    }
    void EndWrite() {
        if (!map) return;
        uint64_t s = ++seq;
        seq = s;
        memcpy(map, &s, sizeof(s));
    }
    // The copying form, for frames the engine did not write itself
    // (passthrough, and any job it refused).
    bool Write(const uint8_t* pixels, size_t n) {
        uint8_t* slot = Slot(n);
        if (!slot) return false;
        BeginWrite();
        memcpy(slot, pixels, n);
        EndWrite();
        return true;
    }
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

static int RunWorker(int argc, char** argv);

int main(int argc, char** argv) {
    int rc = RunWorker(argc, argv);
    fflush(nullptr);
    // Leave without running the teardown. When the HIP runtime is hosted, the
    // process carries a third party's detours, its worker thread and its DLL
    // unload path; that teardown fails fast (0xC0000409) and the parent logs a
    // crash for a session that ended normally. There is nothing left to do
    // here that the OS will not do better.
    TerminateProcess(GetCurrentProcess(), (UINT)rc);
    return rc;
}

static int RunWorker(int argc, char** argv) {
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
        // Frames arrive and leave at the output resolution; the network runs
        // at the work resolution, which is what the scale slider sets. Doing
        // it the other way round (the first version ran the network at output
        // resolution) costs multiples of the frame time and makes the slider
        // do nothing at all.
        uint32_t ow = full_w ? full_w : work_w;
        uint32_t oh = full_h ? full_h : work_h;
        uint32_t nw = work_w, nh = work_h;
        // The host window is created here, on the thread that serves frames
        // and therefore pumps its messages; everything slow happens on the
        // thread below. See DlssNrEngine::Prepare.
        std::string why;
        if (!engine.Prepare(ow, oh, why)) {
            fprintf(stderr, "[host] amd full: neural pass off (%s); passthrough\n",
                    why.c_str());
            return;
        }
        // The menu's effect values, before the runtime loads: it reads them
        // from its own ini, so this is the only way they reach it.
        engine.SetEffect(EffectFromHeader(hdr));
        engine_thread = std::thread([&engine, &engine_live, nw, nh, ow, oh] {
            std::string why;
            if (engine.Start(nw, nh, ow, oh, why)) {
                fprintf(stderr, "[host] amd full: neural pass ON (HIP runtime, "
                        "network %ux%u -> output %ux%u)\n", nw, nh, ow, oh);
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

    SharedIn shm_in;
    SharedOut shm_out;

    float exposure = 1.0f;
    uint64_t frames = 0, resets = 0, neural = 0;
    // Where the worker's own time goes, so "the AMD path is slow" can be
    // answered with the split instead of a guess: dispatch is upload + the
    // network + readback, the rest of the frame time is the pipe.
    LARGE_INTEGER qpf{};
    QueryPerformanceFrequency(&qpf);
    long long dispatch_ticks = 0;
    uint64_t dispatch_count = 0;
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
            bool in_shm = (fr.flags & kFlagShm) != 0;
            if (frame.size() != out_size()) frame.assign(out_size(), 0);
            if (mv.size() != motion) mv.resize(motion);
            if (in_shm) {
                // Colour and motion are already in the section; nothing comes
                // down the pipe for this frame. One memcpy instead of 14.7 MB
                // of pipe at 1440p.
                if (!shm_in.base || shm_in.motion_off < color ||
                    shm_in.bytes < shm_in.motion_off + motion) {
                    fprintf(stderr, "[host] amd full: SHM frame but the section is "
                            "too small (%zu bytes, need %zu+%zu)\n",
                            shm_in.bytes, color, motion);
                    Out o{kOut, fr.index, 0, 0, 0, fr.pts};
                    WriteAll(&o, sizeof(o));
                    continue;
                }
                memcpy(frame.data(), shm_in.base, color);
                if (motion) memcpy(mv.data(), shm_in.base + shm_in.motion_off, motion);
            } else if (!no_color) {
                if (!ReadExact(frame.data(), color)) return 0;
                if (motion && !ReadExact(mv.data(), motion)) return 0;
            } else {
                if (motion && !ReadExact(mv.data(), motion)) return 0;
                std::fill(frame.begin(), frame.end(), 0);
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
            bool wrote_shm = false;
            LARGE_INTEGER t_disp0{}, t_disp1{};
            QueryPerformanceCounter(&t_disp0);
            if (engine_live.load(std::memory_order_acquire) && engine.Ready()) {
                // A resize can land while the engine is still coming up, so
                // the size it started with is not necessarily the one we are
                // serving now.
                uint32_t ow = full_w ? full_w : work_w;
                uint32_t oh = full_h ? full_h : work_h;
                if (engine.out_width() != ow || engine.out_height() != oh ||
                    engine.work_width() != work_w || engine.work_height() != work_h) {
                    std::string why;
                    if (!engine.Resize(work_w, work_h, ow, oh, why)) {
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
                // Straight from the input section into the output one when
                // both are open: at 1440p each spare copy of a frame is
                // several milliseconds of the frame budget.
                const uint8_t* in_ptr = in_shm ? shm_in.base : frame.data();
                if (out.size() != out_size()) out.assign(out_size(), 0);
                uint8_t* out_ptr = shm_out.Slot(out_size());
                if (out_ptr) {
                    shm_out.BeginWrite();
                    ok = engine.Dispatch(in_ptr, out_ptr, out_size(), fp);
                    shm_out.EndWrite();
                    wrote_shm = ok;
                } else {
                    ok = engine.Dispatch(in_ptr, out.data(), out_size(), fp);
                }
                if (ok) {
                    ++neural;
                } else if (frames == 0 || (frames % 120) == 0) {
                    // Say it once in a while rather than per frame: a dropped
                    // job falls back to passthrough, it does not kill the run.
                    fprintf(stderr, "[host] amd full: neural job %u failed, passthrough\n",
                            fr.index);
                }
            }
            if (!ok && in_shm) {
                // Passthrough still owes the client the pixels, and with the
                // colour in the section `frame` was never filled.
                if (out.size() != out_size()) out.assign(out_size(), 0);
                memcpy(out.data(), shm_in.base, out_size());
            } else if (!ok && !DispatchPassthrough(frame.data(), out_size(), d, out)) {
                Out o{kOut, fr.index, 0, 0, 0, fr.pts};
                WriteAll(&o, sizeof(o));
                continue;
            }
            QueryPerformanceCounter(&t_disp1);
            if (ok) {
                dispatch_ticks += t_disp1.QuadPart - t_disp0.QuadPart;
                ++dispatch_count;
            }
            ++frames;
            // The first neural frame gets its own line: the engine comes up
            // asynchronously, so "when did the pass actually take over" is not
            // something the 60-frame cadence can answer.
            if (frames == 1 || frames % 60 == 0 || neural == 1) {
                fprintf(stderr,
                        "[nr] f=%llu exp=%.3f mv=%.3f resets=%llu "
                        "int=%.2f tone=%.2f struct=%.2f skin=%.2f mask=%u style=%u uic=%u "
                        "neural=%llu dispatch=%.1fms\n",
                        (unsigned long long)frames, d.exposure, d.mv_mean,
                        (unsigned long long)resets, d.intensity, d.local_tone,
                        d.local_structure, d.skin_structure, d.auto_mask,
                        d.style, d.ui_correction, (unsigned long long)neural,
                        dispatch_count ? (double)dispatch_ticks * 1000.0 /
                                         ((double)qpf.QuadPart * (double)dispatch_count) : 0.0);
            }
            // The pixels go through the section when it is open and the frame
            // fits it exactly; the sentinel tells the client to read there.
            // `wrote_shm` means the engine already wrote them in place.
            if (wrote_shm || shm_out.Write(out.data(), out.size())) {
                Out o{kOut, fr.index, 1, kOutBytesInShm, 1, fr.pts};
                WriteAll(&o, sizeof(o));
            } else {
                Out o{kOut, fr.index, 1, (uint32_t)out.size(), 1, fr.pts};
                WriteAll(&o, sizeof(o));
                if (!out.empty()) WriteAll(out.data(), out.size());
            }
        } else if (magic == kShm) {
            Shm m{};
            m.magic = magic;
            if (!ReadExact(((uint8_t*)&m) + 4, sizeof(m) - 4)) return 0;
            m.name[sizeof(m.name) - 1] = '\0';
            uint32_t ok = shm_in.Open(m.name, m.color_bytes, m.motion_bytes) ? 1 : 0;
            fprintf(stderr, "[host] amd full: SHMI '%s' %s (%u + %u bytes)\n",
                    m.name, ok ? "mapped - frames come through shared memory"
                               : "failed - frames stay on the pipe",
                    m.color_bytes, m.motion_bytes);
            SendAck(kSack, ok, m.pts);
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
            // The geometry is handled on the frame path (which also covers a
            // resize that lands while the engine is still coming up), but the
            // effect values ride along with RNSZ and the runtime only ever
            // sees them through its ini.
            if (engine_tried) engine.SetEffect(EffectFromHeader(hdr));
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
        } else if (magic == kOuts) {
            GrayOut m{};
            m.magic = magic;
            if (!ReadExact(((uint8_t*)&m) + 4, sizeof(m) - 4)) return 0;
            m.name[sizeof(m.name) - 1] = '\0';
            uint32_t ok = shm_out.Open(m.name, m.w, m.h) ? 1 : 0;
            fprintf(stderr, "[host] amd full: OUTS '%s' %ux%u %s\n", m.name, m.w, m.h,
                    ok ? "mapped - pixels go back through shared memory"
                       : "failed - pixels stay on the pipe");
            SendAck(kOak, ok, m.pts);
        } else if (magic == kGray) {
            GrayOut m{};
            m.magic = magic;
            if (!ReadExact(((uint8_t*)&m) + 4, sizeof(m) - 4)) return 0;
            // Still refused: GRAY is the DDA path's luminance channel and this
            // worker does not capture the screen itself.
            SendAck(kGak, 0, m.pts);
        } else {
            fprintf(stderr, "[host] amd full: unknown magic 0x%08X, exiting\n", magic);
            return 1;
        }
    }
}
