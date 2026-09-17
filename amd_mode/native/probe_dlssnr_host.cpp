// probe_dlssnr_host.exe -- can NeuralScreen host the AMD DLSS-NR runtime?
//
// The pack's dlssnr_amd_passN.dll is a standalone version.dll proxy: loaded
// into a process it detours IDXGIFactory::CreateSwapChain, Present/Present1,
// ID3D12CommandQueue::ExecuteCommandLists and the FSR3 upscaler entries, then
// runs its HIP network on what the game renders. None of that is documented,
// so this probe answers one question with evidence instead of guesswork: does
// the runtime attach, find our device, load the weights and report a usable
// route when the "game" is a plain D3D12 present loop?
//
// It draws nothing clever - an animated clear on a real swapchain - and the
// verdict is whatever the runtime writes to its own log.
//
// Build: build-probe.bat   Run: probe_dlssnr_host.exe [frames]
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>

// FidelityFX API headers (MIT, vendored under third_party): the runtime takes
// its colour/motion/depth from an FSR upscaler dispatch, so hosting it means
// making that dispatch ourselves. We import the entries rather than export
// them, which is why the vendored header's FFX_API_ENTRY is overridable.
#define FFX_API_ENTRY __declspec(dllimport)
#include "../third_party/ffx_api/api/include/ffx_api.h"
#include "../third_party/ffx_api/api/include/dx12/ffx_api_dx12.h"
#include "../third_party/ffx_api/upscalers/include/ffx_upscale.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
// Static import on purpose: a proxy has to be in the import table so the
// loader brings it in before d3d12.dll and dxgi.dll, which is the only order
// in which it can detour their entry points. LoadLibrary after the fact (the
// first thing this probe tried) attaches the DLL but never installs a hook.
#pragma comment(lib, "version.lib")

namespace {

const UINT kWidth = 1280, kHeight = 720, kBuffers = 3;

template <class T> void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

void Log(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr); fflush(stderr);
}

// --- minimal half-float, for the R16G16B16A16_FLOAT colour input ------------
uint16_t ToHalf(float f) {
    uint32_t bits;
    memcpy(&bits, &f, 4);
    uint32_t sign = (bits >> 16) & 0x8000u;
    int32_t exp = (int32_t)((bits >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = bits & 0x7FFFFFu;
    if (exp <= 0) return (uint16_t)sign;
    if (exp >= 31) return (uint16_t)(sign | 0x7C00u);
    return (uint16_t)(sign | ((uint32_t)exp << 10) | (mant >> 13));
}

ID3D12Resource* CreateTex2D(ID3D12Device* dev, UINT w, UINT h, DXGI_FORMAT fmt,
                            D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w; rd.Height = h;
    rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = fmt;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = flags;
    ID3D12Resource* r = nullptr;
    HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state,
                                              nullptr, IID_PPV_ARGS(&r));
    if (FAILED(hr)) Log("[probe] CreateTex2D %ux%u fmt %d -> 0x%08lX", w, h, (int)fmt, hr);
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

}  // namespace

int main(int argc, char** argv) {
    int frames = argc > 1 ? atoi(argv[1]) : 240;
    bool loadFfx = false;
    int lateFrames = 0;
    int warmupMs = 2000;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--ffx") == 0) loadFfx = true;
        if (strcmp(argv[i], "--late") == 0 && i + 1 < argc) lateFrames = atoi(argv[++i]);
        if (strcmp(argv[i], "--warmup") == 0 && i + 1 < argc) warmupMs = atoi(argv[++i]);
    }

    // 1. Touch the import so the linker keeps version.dll in the import
    //    table; by the time main() runs the proxy is already loaded and its
    //    hooks are in. Report where it came from - our folder, not System32,
    //    is the whole point.
#ifdef NS_PROXY_LOADLIBRARY
    // The other half of the question: can the worker load the runtime itself
    // instead of hard-importing it? A static import means the exe refuses to
    // start when the runtime is absent, which is the normal case.
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (wchar_t* slash = wcsrchr(self, L'\\')) *(slash + 1) = 0;
    wcscat_s(self, L"version.dll");
    HMODULE nr = LoadLibraryW(self);
#else
    DWORD handle = 0;
    GetFileVersionInfoSizeW(L"nonexistent-probe-file", &handle);
    HMODULE nr = GetModuleHandleW(L"version.dll");
#endif
    wchar_t nrPath[MAX_PATH] = L"?";
    if (nr) GetModuleFileNameW(nr, nrPath, MAX_PATH);
    Log("[probe] version.dll at %p: %ls", (void*)nr, nrPath);
    if (!nr) return 2;

    // 1b. The runtime's input path comes from the FSR3 upscaler dispatch, and
    //     it may only arm itself once those modules are in the process. Give
    //     it the chance to see them before any device exists.
    if (loadFfx) {
        const wchar_t* ffx[] = { L"amd_fidelityfx_upscaler_dx12.dll",
                                 L"ffx_fsr3upscaler_x64.dll" };
        for (const wchar_t* name : ffx) {
            HMODULE m = LoadLibraryW(name);
            Log("[probe] LoadLibrary(%ls) -> %p (err %lu)", name, (void*)m,
                m ? 0UL : GetLastError());
        }
    }

    // 2. A real window and a real swapchain: the runtime ignores swapchains
    //    that are not on its device, and keeps a dummy-swapchain fallback
    //    only for installing the present hook.
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"NsDlssNrProbe";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"NeuralScreen DLSS-NR probe",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                kWidth, kHeight, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { Log("[probe] no window"); return 3; }
    ShowWindow(hwnd, SW_SHOW);

    // The runtime installs its detours from a thread started in DllMain, and
    // it builds a dummy device and swapchain first, which takes a moment. A
    // swapchain created before its CreateSwapChain hook is in place is never
    // recorded in its swapchain -> command queue map, and its only fallback is
    // IDXGISwapChain::GetDevice(IID_ID3D12CommandQueue), which cannot work on
    // D3D12: the frame is then dropped as "a swapchain that is not on our
    // device". A game takes seconds to boot, so it never hits this; we do.
    if (warmupMs > 0) {
        Log("[probe] waiting %d ms for the runtime's hooks", warmupMs);
        Sleep((DWORD)warmupMs);
    }

    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        Log("[probe] no factory"); return 4;
    }

    IDXGIAdapter1* adapter = nullptr;
    ID3D12Device* device = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { Release(adapter); continue; }
        if (SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
            Log("[probe] adapter %u: %ls (vendor 0x%04x, device 0x%04x)", i,
                desc.Description, desc.VendorId, desc.DeviceId);
            break;
        }
        Release(adapter);
    }
    if (!device) { Log("[probe] no D3D12 device"); return 5; }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));

    // The runtime prefers a backbuffer it can write through a UAV (that is
    // its residual-apply path; without it it falls back to frame replacement),
    // so ask for UAV usage first and step down if the driver refuses.
    struct Variant { UINT usage; DXGI_SWAP_EFFECT effect; const char* label; };
    const Variant variants[] = {
        { DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_UNORDERED_ACCESS,
          DXGI_SWAP_EFFECT_FLIP_DISCARD, "flip_discard+uav" },
        { DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_UNORDERED_ACCESS,
          DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, "flip_sequential+uav" },
        { DXGI_USAGE_RENDER_TARGET_OUTPUT,
          DXGI_SWAP_EFFECT_FLIP_DISCARD, "flip_discard" },
    };
    IDXGISwapChain1* sc1 = nullptr;
    HRESULT hr = E_FAIL;
    for (const Variant& v : variants) {
        DXGI_SWAP_CHAIN_DESC1 scd{};
        scd.Width = kWidth; scd.Height = kHeight;
        scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        scd.SampleDesc.Count = 1;
        scd.BufferUsage = v.usage;
        scd.BufferCount = kBuffers;
        scd.SwapEffect = v.effect;
        hr = factory->CreateSwapChainForHwnd(queue, hwnd, &scd, nullptr, nullptr, &sc1);
        Log("[probe] swapchain %-20s -> 0x%08lX", v.label, hr);
        if (SUCCEEDED(hr)) break;
    }
    if (FAILED(hr)) { Log("[probe] no swapchain"); return 6; }
    IDXGISwapChain3* sc = nullptr;
    sc1->QueryInterface(IID_PPV_ARGS(&sc));

    // Printed so the runtime's own log can be read against ours: it names the
    // device, the present queue and the swapchains it accepts or ignores.
    Log("[probe] device %p, queue %p, swapchain %p (as IDXGISwapChain1 %p)",
        (void*)device, (void*)queue, (void*)sc, (void*)sc1);

    ID3D12DescriptorHeap* rtvHeap = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.NumDescriptors = kBuffers;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap));
    UINT rtvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ID3D12Resource* backbuf[kBuffers]{};
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kBuffers; ++i) {
        sc->GetBuffer(i, IID_PPV_ARGS(&backbuf[i]));
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtv;
        h.ptr += (SIZE_T)i * rtvSize;
        device->CreateRenderTargetView(backbuf[i], nullptr, h);
    }

    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cl = nullptr;
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cl));
    cl->Close();
    ID3D12Fence* fence = nullptr;
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 fenceValue = 0;

    // --- the FSR upscaler dispatch: the trigger the runtime listens for ----
    // Colour, motion and depth reach the network through this call; without
    // it the runtime hooks itself in and then sits idle, which is exactly what
    // the first runs of this probe showed.
    ffxContext fsr = nullptr;
    PfnFfxCreateContext pfnCreate = nullptr;
    PfnFfxDispatch pfnDispatch = nullptr;
    PfnFfxDestroyContext pfnDestroy = nullptr;
    ID3D12Resource* texColor = nullptr;
    ID3D12Resource* texDepth = nullptr;
    ID3D12Resource* texMv = nullptr;
    ID3D12Resource* texOut = nullptr;
    ID3D12Resource* upload = nullptr;
    if (loadFfx) {
        // Statically imported, like the upscaler DLL is in a real game: the
        // runtime detours ffxDispatch while it initialises, so a module that
        // arrives later (our first attempt used LoadLibrary here) is never
        // hooked and the network never sees a frame.
        pfnCreate = &ffxCreateContext;
        pfnDispatch = &ffxDispatch;
        pfnDestroy = &ffxDestroyContext;
        HMODULE ffxMod = GetModuleHandleW(L"amd_fidelityfx_upscaler_dx12.dll");
        Log("[probe] ffx module %p, entries: create %p dispatch %p destroy %p",
            (void*)ffxMod, (void*)pfnCreate, (void*)pfnDispatch, (void*)pfnDestroy);

        texColor = CreateTex2D(device, kWidth, kHeight, DXGI_FORMAT_R16G16B16A16_FLOAT,
                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATE_COPY_DEST);
        texDepth = CreateTex2D(device, kWidth, kHeight, DXGI_FORMAT_R32_FLOAT,
                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATE_COPY_DEST);
        texMv = CreateTex2D(device, kWidth, kHeight, DXGI_FORMAT_R16G16_FLOAT,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_COPY_DEST);
        texOut = CreateTex2D(device, kWidth, kHeight, DXGI_FORMAT_R16G16B16A16_FLOAT,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        // One upload buffer, three staged copies: a lit gradient for colour,
        // zeroes for motion and depth (a desktop frame has neither).
        D3D12_RESOURCE_DESC cd = texColor->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 totalBytes = 0, rowBytes = 0;
        UINT rows = 0;
        device->GetCopyableFootprints(&cd, 0, 1, 0, &fp, &rows, &rowBytes, &totalBytes);
        D3D12_HEAP_PROPERTIES up{}; up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = totalBytes; bd.Height = 1;
        bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd,
                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                        IID_PPV_ARGS(&upload));
        uint8_t* mapped = nullptr;
        D3D12_RANGE none{ 0, 0 };
        upload->Map(0, &none, (void**)&mapped);
        for (UINT y = 0; y < kHeight; ++y) {
            uint16_t* row = (uint16_t*)(mapped + (size_t)y * fp.Footprint.RowPitch);
            for (UINT x = 0; x < kWidth; ++x) {
                float u = (float)x / kWidth, v = (float)y / kHeight;
                float shade = 0.5f + 0.5f * sinf(u * 26.0f) * cosf(v * 18.0f);
                row[x * 4 + 0] = ToHalf(0.20f + 0.60f * u * shade);
                row[x * 4 + 1] = ToHalf(0.15f + 0.55f * v);
                row[x * 4 + 2] = ToHalf(0.60f - 0.40f * u * v);
                row[x * 4 + 3] = ToHalf(1.0f);
            }
        }
        upload->Unmap(0, nullptr);

        alloc->Reset();
        cl->Reset(alloc, nullptr);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = texColor; dst.SubresourceIndex = 0;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.pResource = upload; src.PlacedFootprint = fp;
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Barrier(cl, texColor, D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cl, texDepth, D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cl, texMv, D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cl->Close();
        ID3D12CommandList* prep[] = { cl };
        queue->ExecuteCommandLists(1, prep);
        queue->Signal(fence, ++fenceValue);
        fence->SetEventOnCompletion(fenceValue, fenceEvent);
        WaitForSingleObject(fenceEvent, 5000);

    }

    // The runtime installs its ffxCreateContext / ffxDispatch detours from its
    // per-present tick, so a context created before the first present is
    // invisible to it. `--late N` holds the creation back N frames.
    auto createFsrContext = [&](int atFrame) {
        if (!pfnCreate || fsr) return;
        ffxCreateBackendDX12Desc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.device = device;
        ffxCreateContextDescUpscale desc{};
        desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        desc.header.pNext = &backend.header;
        desc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE |
                     FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
        desc.maxRenderSize = { kWidth, kHeight };
        desc.maxUpscaleSize = { kWidth, kHeight };
        ffxReturnCode_t rc = pfnCreate(&fsr, &desc.header, nullptr);
        Log("[probe] ffxCreateContext at frame %d -> %u (context %p)", atFrame, rc,
            (void*)fsr);
        if (rc != FFX_API_RETURN_OK) fsr = nullptr;
    };
    if (lateFrames == 0) createFsrContext(0);

    Log("[probe] presenting %d frames at %ux%u%s", frames, kWidth, kHeight,
        fsr ? " with an FSR upscale dispatch per frame" : "");
    for (int f = 0; f < frames; ++f) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        UINT idx = sc->GetCurrentBackBufferIndex();
        alloc->Reset();
        cl->Reset(alloc, nullptr);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = backbuf[idx];
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &b);
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtv;
        h.ptr += (SIZE_T)idx * rtvSize;
        // A moving gradient, so a network that ran would have something to chew.
        float t = (float)f / (float)frames;
        const float clear[4] = { 0.15f + 0.5f * t, 0.25f, 0.65f - 0.4f * t, 1.0f };
        cl->ClearRenderTargetView(h, clear, 0, nullptr);

        if (loadFfx && lateFrames > 0 && f == lateFrames) createFsrContext(f);

        if (fsr && pfnDispatch) {
            ffxDispatchDescUpscale dd{};
            dd.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
            dd.commandList = cl;
            dd.color = ffxApiGetResourceDX12(texColor, FFX_API_RESOURCE_STATE_COMPUTE_READ);
            dd.depth = ffxApiGetResourceDX12(texDepth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
            dd.motionVectors = ffxApiGetResourceDX12(texMv, FFX_API_RESOURCE_STATE_COMPUTE_READ);
            dd.output = ffxApiGetResourceDX12(texOut, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
            dd.jitterOffset = { 0.0f, 0.0f };
            dd.motionVectorScale = { (float)kWidth, (float)kHeight };
            dd.renderSize = { kWidth, kHeight };
            dd.upscaleSize = { kWidth, kHeight };
            dd.enableSharpening = false;
            dd.sharpness = 0.0f;
            dd.frameTimeDelta = 16.6f;
            dd.preExposure = 1.0f;
            dd.reset = (f == 0);
            dd.cameraNear = 0.1f;
            dd.cameraFar = 1000.0f;
            dd.cameraFovAngleVertical = 1.0f;
            dd.viewSpaceToMetersFactor = 1.0f;
            ffxReturnCode_t rc = pfnDispatch(&fsr, &dd.header);
            if (f == 0 || (rc != FFX_API_RETURN_OK && f < 3)) {
                Log("[probe] ffxDispatch frame %d -> %u", f, rc);
            }
        }

        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cl->ResourceBarrier(1, &b);
        cl->Close();
        ID3D12CommandList* lists[] = { cl };
        queue->ExecuteCommandLists(1, lists);
        sc->Present(1, 0);
        queue->Signal(fence, ++fenceValue);
        if (fence->GetCompletedValue() < fenceValue) {
            fence->SetEventOnCompletion(fenceValue, fenceEvent);
            WaitForSingleObject(fenceEvent, 1000);
        }
    }
    Log("[probe] done, %d frames presented", frames);

    // Drain before we leave: the runtime has a worker thread of its own.
    Sleep(500);
    return 0;
}
