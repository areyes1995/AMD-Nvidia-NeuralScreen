// dlssnr_engine.cpp -- see dlssnr_engine.h for what this is and why.
#define WIN32_LEAN_AND_MEAN
#include "dlssnr_engine.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdio>
#include <cstring>

// FFX API headers (MIT, vendored). We resolve the entries with GetProcAddress
// so the worker still starts on a machine with no upscaler DLL.
#define FFX_API_ENTRY
#include "../third_party/ffx_api/api/include/ffx_api.h"
#include "../third_party/ffx_api/api/include/dx12/ffx_api_dx12.h"
#include "../third_party/ffx_api/upscalers/include/ffx_upscale.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

namespace nsamd {
namespace {

const wchar_t* kProxyName = L"version.dll";
const wchar_t* kUpscalerName = L"amd_fidelityfx_upscaler_dx12.dll";

template <class T> void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

void Say(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[hip] ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProcW(h, m, w, l);
}

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (wchar_t* slash = wcsrchr(buf, L'\\')) *(slash + 1) = 0;
    return std::wstring(buf);
}

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), out.data(), n,
                        nullptr, nullptr);
    return out;
}

// Size of the runtime's log now, so we only ever read what this session
// wrote: the file survives runs, and an older "hooked ..." would make us
// start before the detours of THIS process are in.
uint64_t FileSize(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad)) return 0;
    return ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
}

// Does the log carry `needle` past `from`? Shared-open, because the runtime
// holds the file too, and tolerant of the file being replaced under us.
bool LogHas(const std::string& path, uint64_t from, const char* needle) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    uint64_t start = ((uint64_t)size.QuadPart >= from) ? from : 0;  // truncated
    uint64_t len = (uint64_t)size.QuadPart - start;
    bool found = false;
    if (len > 0 && len < (16u << 20)) {
        LARGE_INTEGER at{};
        at.QuadPart = (LONGLONG)start;
        SetFilePointerEx(h, at, nullptr, FILE_BEGIN);
        std::string buf((size_t)len, '\0');
        DWORD got = 0;
        if (ReadFile(h, buf.data(), (DWORD)len, &got, nullptr) && got) {
            buf.resize(got);
            found = buf.find(needle) != std::string::npos;
        }
    }
    CloseHandle(h);
    return found;
}

uint32_t EnvUint(const char* name, uint32_t fallback) {
    char buf[32];
    DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    if (!n || n >= sizeof(buf)) return fallback;
    return (uint32_t)strtoul(buf, nullptr, 10);
}

}  // namespace

struct DlssNrEngine::Impl {
    HMODULE proxy = nullptr;
    HMODULE upscaler = nullptr;
    PfnFfxCreateContext create = nullptr;
    PfnFfxDispatch dispatch = nullptr;
    PfnFfxDestroyContext destroy = nullptr;

    HWND hwnd = nullptr;
    IDXGIFactory4* factory = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    IDXGISwapChain3* swap = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cl = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fence_event = nullptr;
    UINT64 fence_value = 0;

    ID3D12Resource* color = nullptr;   // BGRA8, what we were handed
    ID3D12Resource* depth = nullptr;   // R32F, flat: a desktop frame has none
    ID3D12Resource* mv = nullptr;      // RG16F, zeroed for the same reason
    ID3D12Resource* out = nullptr;     // RGBA8 + UAV, what the network wrote
    ID3D12Resource* upload = nullptr;
    ID3D12Resource* readback = nullptr;
    UINT upload_pitch = 0, readback_pitch = 0;
    UINT64 upload_bytes = 0, readback_bytes = 0;

    ffxContext ffx = nullptr;

    bool CreateDeviceObjects(uint32_t w, uint32_t h, std::string& why);
    bool CreateFrameObjects(uint32_t w, uint32_t h, std::string& why);
    void DestroyFrameObjects();
    void DestroyAll();
    bool Present();
};

namespace {

ID3D12Resource* Tex(ID3D12Device* dev, UINT w, UINT h, DXGI_FORMAT fmt,
                    D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w; rd.Height = h;
    rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = fmt;
    rd.SampleDesc.Count = 1;
    rd.Flags = flags;
    ID3D12Resource* r = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state,
                                            nullptr, IID_PPV_ARGS(&r)))) {
        return nullptr;
    }
    return r;
}

ID3D12Resource* Buffer(ID3D12Device* dev, UINT64 bytes, D3D12_HEAP_TYPE heap,
                       D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes; rd.Height = 1;
    rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* r = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state,
                                            nullptr, IID_PPV_ARGS(&r)))) {
        return nullptr;
    }
    return r;
}

void Barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* res,
             D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    if (from == to) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &b);
}

UINT Align(UINT v, UINT a) { return (v + a - 1) & ~(a - 1); }

}  // namespace

bool DlssNrEngine::Impl::CreateDeviceObjects(uint32_t w, uint32_t h, std::string& why) {
    if (!hwnd) { why = "Prepare() was not called on the frame thread"; return false; }

    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        why = "CreateDXGIFactory2 failed"; return false;
    }
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0,
                                        IID_PPV_ARGS(&device)))) {
            Say("device on %ls", desc.Description);
            Rel(adapter);
            break;
        }
        Rel(adapter);
    }
    if (!device) { why = "no D3D12 device"; return false; }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) {
        why = "CreateCommandQueue failed"; return false;
    }

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = w; scd.Height = h;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 3;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* sc1 = nullptr;
    HRESULT hr = factory->CreateSwapChainForHwnd(queue, hwnd, &scd, nullptr, nullptr, &sc1);
    if (FAILED(hr)) {
        char buf[96];
        snprintf(buf, sizeof(buf), "CreateSwapChainForHwnd 0x%08lX", (unsigned long)hr);
        why = buf; return false;
    }
    sc1->QueryInterface(IID_PPV_ARGS(&swap));
    Rel(sc1);

    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&alloc))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc,
                                         nullptr, IID_PPV_ARGS(&cl))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        why = "command objects failed"; return false;
    }
    cl->Close();
    fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return fence_event != nullptr;
}

bool DlssNrEngine::Impl::CreateFrameObjects(uint32_t w, uint32_t h, std::string& why) {
    // Colour arrives as BGRA and is only read, so it stays BGRA: no swizzle on
    // the hot path in. The output has to carry a typed UAV store for the
    // runtime's residual apply, and BGRA8 does not guarantee one - hence RGBA8
    // out and a swizzle on the way back.
    color = Tex(device, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_NONE,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    depth = Tex(device, w, h, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    mv = Tex(device, w, h, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE,
             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    out = Tex(device, w, h, DXGI_FORMAT_R8G8B8A8_UNORM,
              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!color || !depth || !mv || !out) { why = "input textures failed"; return false; }

    upload_pitch = Align(w * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    upload_bytes = (UINT64)upload_pitch * h;
    readback_pitch = upload_pitch;
    readback_bytes = upload_bytes;
    upload = Buffer(device, upload_bytes, D3D12_HEAP_TYPE_UPLOAD,
                    D3D12_RESOURCE_STATE_GENERIC_READ);
    readback = Buffer(device, readback_bytes, D3D12_HEAP_TYPE_READBACK,
                      D3D12_RESOURCE_STATE_COPY_DEST);
    if (!upload || !readback) { why = "staging buffers failed"; return false; }

    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = device;
    ffxCreateContextDescUpscale desc{};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    desc.header.pNext = &backend.header;
    desc.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    desc.maxRenderSize = { w, h };
    desc.maxUpscaleSize = { w, h };
    ffxReturnCode_t rc = create(&ffx, &desc.header, nullptr);
    if (rc != FFX_API_RETURN_OK) {
        char buf[64];
        snprintf(buf, sizeof(buf), "ffxCreateContext returned %u", rc);
        ffx = nullptr;
        why = buf;
        return false;
    }
    return true;
}

void DlssNrEngine::Impl::DestroyFrameObjects() {
    if (ffx && destroy) destroy(&ffx, nullptr);
    ffx = nullptr;
    Rel(readback); Rel(upload);
    Rel(out); Rel(mv); Rel(depth); Rel(color);
}

void DlssNrEngine::Impl::DestroyAll() {
    DestroyFrameObjects();
    if (fence_event) { CloseHandle(fence_event); fence_event = nullptr; }
    Rel(fence); Rel(cl); Rel(alloc); Rel(swap); Rel(queue); Rel(device); Rel(factory);
    if (hwnd) { DestroyWindow(hwnd); hwnd = nullptr; }
}

bool DlssNrEngine::Impl::Present() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    // No sync interval: this swapchain exists to drive the runtime's per-frame
    // tick, not to pace anything. The pipeline does the pacing.
    HRESULT hr = swap->Present(0, 0);
    return SUCCEEDED(hr) || hr == DXGI_STATUS_OCCLUDED;
}

DlssNrEngine::~DlssNrEngine() { Stop(); }

bool DlssNrEngine::Prepare(uint32_t w, uint32_t h, std::string& why) {
    if (!w || !h) { why = "zero frame size"; return false; }
    if (!impl_) impl_ = new Impl();
    if (impl_->hwnd) return true;

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"NsAmdNrHost";
    RegisterClassW(&wc);
    // Off-screen and never shown: the runtime needs a real swapchain that gets
    // presented, the user must not get a second window on their desktop.
    impl_->hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName,
                                  L"NeuralScreen AMD NR", WS_POPUP,
                                  -32000, -32000, (int)w, (int)h,
                                  nullptr, nullptr, wc.hInstance, nullptr);
    if (!impl_->hwnd) { why = "no host window"; return false; }
    return true;
}

bool DlssNrEngine::Start(uint32_t w, uint32_t h, std::string& why) {
    if (ready_) return true;
    if (!w || !h) { why = "zero frame size"; return false; }
    if (!impl_ || !impl_->hwnd) { why = "Prepare() was not called first"; return false; }

    // The runtime keeps its ini, its log and its weights next to itself, so
    // wherever it is found is also where the rest has to live. Order: an
    // explicit path, the worker's own folder, then the BYO weights folder,
    // which is where amd_mode/weights/README.md tells people to put things.
    const std::wstring dir = ExeDir();
    std::wstring home;
    uint64_t log_from = 0;
    wchar_t env[MAX_PATH];
    std::wstring candidates[3];
    int n_candidates = 0;
    if (GetEnvironmentVariableW(L"NS_AMD_NR_RUNTIME", env, MAX_PATH)) {
        std::wstring p = env;
        if (!p.empty() && p.back() != L'\\') p += L'\\';
        candidates[n_candidates++] = p;
    }
    candidates[n_candidates++] = dir;
    candidates[n_candidates++] = dir + L"..\\weights\\";
    for (int i = 0; i < n_candidates && !impl_->proxy; ++i) {
        const std::wstring path = candidates[i] + kProxyName;
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        home = candidates[i];
        // Before the load, so the hook lines we wait for are this run's.
        log_from = FileSize(ToUtf8(home + L"dlssnr_on_amd.log"));
        impl_->proxy = LoadLibraryW(path.c_str());
    }
    if (!impl_->proxy) {
        why = "no DLSS-NR runtime (version.dll) beside the worker or in amd_mode/weights";
        Stop();
        return false;
    }
    impl_->upscaler = LoadLibraryW((home + kUpscalerName).c_str());
    if (!impl_->upscaler) impl_->upscaler = LoadLibraryW((dir + kUpscalerName).c_str());
    if (!impl_->upscaler) impl_->upscaler = LoadLibraryW(kUpscalerName);
    if (!impl_->upscaler) {
        why = "no amd_fidelityfx_upscaler_dx12.dll beside the runtime";
        Stop();
        return false;
    }
    impl_->create = (PfnFfxCreateContext)(void*)GetProcAddress(impl_->upscaler, "ffxCreateContext");
    impl_->dispatch = (PfnFfxDispatch)(void*)GetProcAddress(impl_->upscaler, "ffxDispatch");
    impl_->destroy = (PfnFfxDestroyContext)(void*)GetProcAddress(impl_->upscaler, "ffxDestroyContext");
    if (!impl_->create || !impl_->dispatch || !impl_->destroy) {
        why = "upscaler DLL has no FFX API entries";
        Stop();
        return false;
    }

    // The runtime hooks D3D12/DXGI from a thread it starts on load, and it
    // builds a dummy device first. Anything we create before that lands is
    // invisible to it - this wait is the difference between a neural pass and
    // a silent passthrough. See docs/AMD_HIP_HOSTING.md.
    //
    // It is a wait for evidence, not a blind sleep: the runtime names every
    // detour in its own log, and the pipeline gives a worker five seconds to
    // answer a frame, so a fixed two-second guess would spend most of that
    // budget doing nothing on a machine where the hooks land in 200 ms.
    const uint32_t cap = EnvUint("NS_AMD_NR_WARMUP_MS", 3000);
    const std::string log_path = ToUtf8(home + L"dlssnr_on_amd.log");
    const char* kLastHook = "hooked IDXGISwapChain1::Present1";
    DWORD waited = 0;
    bool hooked = false;
    while (waited < cap) {
        if (LogHas(log_path, log_from, kLastHook)) { hooked = true; break; }
        Sleep(25);
        waited += 25;
    }
    if (hooked) {
        Say("runtime hooks in after %u ms", waited);
    } else {
        Say("runtime hooks not seen in %u ms; going on anyway, watch %s",
            waited, log_path.c_str());
    }

    if (!impl_->CreateDeviceObjects(w, h, why) || !impl_->CreateFrameObjects(w, h, why)) {
        Stop();
        return false;
    }

    // No warm-up presents here on purpose. Present belongs to the thread that
    // owns the window, and that thread is the one serving frames; presenting
    // from this one would wait on a pump that is busy answering the pipe. The
    // runtime's engine init runs on the first few frames we do serve, which
    // costs one ~35 ms kernel load and then settles.
    width_ = w; height_ = h;
    ready_ = true;
    Say("engine up at %ux%u (check dlssnr_on_amd.log for 'engine init ok')", w, h);
    return true;
}

bool DlssNrEngine::Resize(uint32_t w, uint32_t h, std::string& why) {
    if (!ready_) return false;
    if (w == width_ && h == height_) return true;
    impl_->DestroyFrameObjects();
    if (impl_->swap) impl_->swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    if (!impl_->CreateFrameObjects(w, h, why)) {
        ready_ = false;
        return false;
    }
    width_ = w; height_ = h;
    Say("resized to %ux%u", w, h);
    return true;
}

bool DlssNrEngine::Dispatch(const uint8_t* bgra, size_t bytes, const FrameParams& p,
                            std::vector<uint8_t>& out) {
    if (!ready_ || !impl_) return false;
    const size_t expect = (size_t)width_ * height_ * 4;
    if (bytes != expect) return false;

    Impl& s = *impl_;

    uint8_t* mapped = nullptr;
    D3D12_RANGE nothing{ 0, 0 };
    if (FAILED(s.upload->Map(0, &nothing, (void**)&mapped))) return false;
    for (uint32_t y = 0; y < height_; ++y) {
        memcpy(mapped + (size_t)y * s.upload_pitch, bgra + (size_t)y * width_ * 4,
               (size_t)width_ * 4);
    }
    s.upload->Unmap(0, nullptr);

    s.alloc->Reset();
    s.cl->Reset(s.alloc, nullptr);

    Barrier(s.cl, s.color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = s.color; dst.SubresourceIndex = 0;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.pResource = s.upload;
    src.PlacedFootprint.Offset = 0;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = width_;
    src.PlacedFootprint.Footprint.Height = height_;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = s.upload_pitch;
    s.cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Barrier(s.cl, s.color, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    ffxDispatchDescUpscale dd{};
    dd.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dd.commandList = s.cl;
    dd.color = ffxApiGetResourceDX12(s.color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dd.depth = ffxApiGetResourceDX12(s.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dd.motionVectors = ffxApiGetResourceDX12(s.mv, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dd.output = ffxApiGetResourceDX12(s.out, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    dd.jitterOffset = { 0.0f, 0.0f };
    dd.motionVectorScale = { (float)width_, (float)height_ };
    dd.renderSize = { width_, height_ };
    dd.upscaleSize = { width_, height_ };
    dd.enableSharpening = false;
    dd.sharpness = 0.0f;
    dd.frameTimeDelta = p.frame_time_ms;
    dd.preExposure = p.exposure > 0.0f ? p.exposure : 1.0f;
    dd.reset = p.reset != 0;
    dd.cameraNear = 0.1f;
    dd.cameraFar = 1000.0f;
    dd.cameraFovAngleVertical = 1.0f;
    dd.viewSpaceToMetersFactor = 1.0f;
    ffxReturnCode_t rc = s.dispatch(&s.ffx, &dd.header);
    if (rc != FFX_API_RETURN_OK) {
        s.cl->Close();
        Say("ffxDispatch returned %u on frame %u", rc, p.index);
        return false;
    }

    Barrier(s.cl, s.out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION rdst{}, rsrc{};
    rdst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    rdst.pResource = s.readback;
    rdst.PlacedFootprint.Offset = 0;
    rdst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rdst.PlacedFootprint.Footprint.Width = width_;
    rdst.PlacedFootprint.Footprint.Height = height_;
    rdst.PlacedFootprint.Footprint.Depth = 1;
    rdst.PlacedFootprint.Footprint.RowPitch = s.readback_pitch;
    rsrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    rsrc.pResource = s.out; rsrc.SubresourceIndex = 0;
    s.cl->CopyTextureRegion(&rdst, 0, 0, 0, &rsrc, nullptr);
    Barrier(s.cl, s.out, D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    s.cl->Close();

    ID3D12CommandList* lists[] = { s.cl };
    s.queue->ExecuteCommandLists(1, lists);
    // The runtime runs its network inline off this submission and the present
    // that follows, so both happen before we read anything back.
    s.Present();
    s.queue->Signal(s.fence, ++s.fence_value);
    if (s.fence->GetCompletedValue() < s.fence_value) {
        s.fence->SetEventOnCompletion(s.fence_value, s.fence_event);
        if (WaitForSingleObject(s.fence_event, 2000) != WAIT_OBJECT_0) {
            Say("GPU wait timed out on frame %u", p.index);
            return false;
        }
    }

    if (out.size() != bytes) out.assign(bytes, 0);
    uint8_t* got = nullptr;
    D3D12_RANGE all{ 0, (SIZE_T)s.readback_bytes };
    if (FAILED(s.readback->Map(0, &all, (void**)&got))) return false;
    for (uint32_t y = 0; y < height_; ++y) {
        const uint8_t* srow = got + (size_t)y * s.readback_pitch;
        uint8_t* drow = out.data() + (size_t)y * width_ * 4;
        for (uint32_t x = 0; x < width_; ++x) {
            drow[x * 4 + 0] = srow[x * 4 + 2];  // RGBA out of the UAV, BGRA in
            drow[x * 4 + 1] = srow[x * 4 + 1];  // the protocol
            drow[x * 4 + 2] = srow[x * 4 + 0];
            drow[x * 4 + 3] = srow[x * 4 + 3];
        }
    }
    s.readback->Unmap(0, &nothing);
    return true;
}

void DlssNrEngine::Stop() {
    if (!impl_) return;
    if (impl_->queue && impl_->fence && impl_->fence_event) {
        impl_->queue->Signal(impl_->fence, ++impl_->fence_value);
        if (impl_->fence->GetCompletedValue() < impl_->fence_value) {
            impl_->fence->SetEventOnCompletion(impl_->fence_value, impl_->fence_event);
            WaitForSingleObject(impl_->fence_event, 1000);
        }
    }
    impl_->DestroyAll();
    delete impl_;
    impl_ = nullptr;
    ready_ = false;
}

}  // namespace nsamd
