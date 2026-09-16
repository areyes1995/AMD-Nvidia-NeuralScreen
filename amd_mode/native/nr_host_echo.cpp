// amd_nr_host -- Phase 2a echo worker for the AMD backend.
//
// Speaks the SAME stdin/stdout protocol as nvidia_mode/native (see
// protocol.py): D5V3 header, FRM1 in, OUT1 out, plus every channel ack
// (SACK/MACK/WACK/DACK/WGAK/OAK2/GAK/RACK). The neural pass is a
// passthrough for now: the input colour comes back byte-identical, so
// the Python side drives the full pipeline (overlay, screenshots,
// recording, RNSZ) against the AMD binary before any HIP exists.
//
// Refusals are honest: SHMI/GRAY/OUTS/WNDO/DDA/WGCW answer ok=0 and
// Python falls back to its pipe/dxcam/pygame paths (channels.py).
// RNSZ and MOTS are honoured (sizes tracked, RACK ok=1).
//
// Build: build-amd.bat (MSVC 2022, CRT only - no NGX, no HIP yet).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <io.h>
#include <vector>

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
}  // namespace

int main(int argc, char** argv) {
    bool live = argc > 1 && strcmp(argv[1], "--live") == 0;
    if (!live) {
        fprintf(stderr, "amd_nr_host: Phase 2a echo worker, use --live\n");
        return 2;
    }
    // Binary pipes: MSVC opens stdio in TEXT mode by default and
    // \r\n translation would corrupt the byte counts (and desync the
    // whole stream on the first 0x0D byte of a frame).
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    setvbuf(stdout, nullptr, _IONBF, 0);

    Header hdr{};
    if (!ReadExact(&hdr, sizeof(hdr)) || (hdr.magic != kVideo && hdr.magic != kResize)) {
        fprintf(stderr, "[host] amd echo: no header, exiting\n");
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
    fprintf(stderr, "[host] amd echo worker ready: work %ux%u full %ux%u\n",
            work_w, work_h, full_w, full_h);

    std::vector<uint8_t> frame, mv;  // reused: no per-frame alloc
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
            // Passthrough: the input colour IS the output.
            Out o{kOut, fr.index, 1, (uint32_t)out_size(), 1, fr.pts};
            WriteAll(&o, sizeof(o));
            if (!frame.empty()) WriteAll(frame.data(), frame.size());
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
            fprintf(stderr, "[host] amd echo: resize %ux%u full %ux%u\n",
                    work_w, work_h, full_w, full_h);
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
            fprintf(stderr, "[host] amd echo: unknown magic 0x%08X, exiting\n", magic);
            return 1;
        }
    }
}
