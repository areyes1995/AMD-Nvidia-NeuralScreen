// dlssnr_engine.cpp -- see dlssnr_engine.h for what this is and why.
#define WIN32_LEAN_AND_MEAN
#include "dlssnr_engine.h"

#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
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
#pragma comment(lib, "d3dcompiler.lib")

namespace nsamd {
namespace {

const wchar_t* kProxyName = L"version.dll";
const wchar_t* kUpscalerName = L"amd_fidelityfx_upscaler_dx12.dll";

// One shader for both ends of the pass. Besides resampling it converts the
// colour space, and that is not a detail: the network is fed linear light,
// like the game feeds it (its own log says `colour dxgi 10 ... tonemap 1`
// there against `dxgi 28 ... tonemap 0` for an 8-bit sRGB input). Handing it
// sRGB bytes as if they were linear made it apply a tone curve that crushed
// the highlights - measured, -48/255 at the top end - while its actual
// spatial contribution was 0.67/255. That reads as "the neural pass does
// nothing except make it look worse", which is exactly what it was.
//
// mode 0: sRGB in -> linear out (the network's input)
// mode 1: linear in -> sRGB out, channels reversed (the protocol's BGRA)
const char kResampleHLSL[] = R"(
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
SamplerState smp : register(s0);
cbuffer C : register(b0) { uint dstW; uint dstH; uint mode; uint pad; };

float3 SrgbToLinear(float3 c) {
    return c <= 0.04045 ? c / 12.92 : pow(abs(c + 0.055) / 1.055, 2.4);
}
float3 LinearToSrgb(float3 c) {
    c = max(c, 0.0);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= dstW || id.y >= dstH) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(dstW, dstH);
    float4 c = src.SampleLevel(smp, uv, 0);
    if (mode == 0) {
        dst[id.xy] = float4(SrgbToLinear(c.rgb), c.a);
    } else {
        float3 s = saturate(LinearToSrgb(c.rgb));
        dst[id.xy] = float4(s.b, s.g, s.r, c.a);
    }
}
)";

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
    if (from == to || !res) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &b);
}

UINT Align(UINT v, UINT a) { return (v + a - 1) & ~(a - 1); }
UINT Groups(UINT n) { return (n + 7) / 8; }

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

    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;   // 4 descriptors: 2 per pass
    UINT heap_step = 0;

    ID3D12Resource* src = nullptr;    // BGRA8, the captured frame as it arrived
    ID3D12Resource* color = nullptr;  // RGBA8 at work res: the network's input
    ID3D12Resource* depth = nullptr;  // R32F, flat: a desktop frame has none
    ID3D12Resource* mv = nullptr;     // RG16F, zeroed for the same reason
    ID3D12Resource* out = nullptr;    // RGBA8 at out res, what the network wrote
    ID3D12Resource* bgra = nullptr;   // RGBA8 holding BGRA bytes, for readback
    ID3D12Resource* upload = nullptr;
    ID3D12Resource* readback = nullptr;
    UINT upload_pitch = 0, readback_pitch = 0;
    UINT64 upload_bytes = 0, readback_bytes = 0;

    ffxContext ffx = nullptr;

    bool CreateDeviceObjects(uint32_t out_w, uint32_t out_h, std::string& why);
    bool CreatePipeline(std::string& why);
    bool CreateFrameObjects(uint32_t work_w, uint32_t work_h,
                            uint32_t out_w, uint32_t out_h, std::string& why);
    void DestroyFrameObjects();
    bool Present();
    void WaitGpu(DWORD ms);
};

bool DlssNrEngine::Impl::CreateDeviceObjects(uint32_t out_w, uint32_t out_h,
                                             std::string& why) {
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
    scd.Width = out_w; scd.Height = out_h;
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

bool DlssNrEngine::Impl::CreatePipeline(std::string& why) {
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 1;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 0;
    params[1].Constants.Num32BitValues = 4;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderRegister = 0;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 2;
    rs.pParameters = params;
    rs.NumStaticSamplers = 1;
    rs.pStaticSamplers = &samp;

    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1,
                                           &blob, &err))) {
        why = "root signature failed";
        Rel(err); Rel(blob);
        return false;
    }
    HRESULT hr = device->CreateRootSignature(0, blob->GetBufferPointer(),
                                             blob->GetBufferSize(), IID_PPV_ARGS(&root));
    Rel(blob); Rel(err);
    if (FAILED(hr)) { why = "CreateRootSignature failed"; return false; }

    ID3DBlob* cs = nullptr;
    if (FAILED(D3DCompile(kResampleHLSL, sizeof(kResampleHLSL) - 1, "resample",
                          nullptr, nullptr, "main", "cs_5_0", 0, 0, &cs, &err))) {
        why = err ? std::string("resample shader: ") + (const char*)err->GetBufferPointer()
                  : "resample shader failed to compile";
        Rel(cs); Rel(err);
        return false;
    }
    Rel(err);
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root;
    pd.CS.pShaderBytecode = cs->GetBufferPointer();
    pd.CS.BytecodeLength = cs->GetBufferSize();
    hr = device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso));
    Rel(cs);
    if (FAILED(hr)) { why = "CreateComputePipelineState failed"; return false; }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 4;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)))) {
        why = "descriptor heap failed"; return false;
    }
    heap_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

bool DlssNrEngine::Impl::CreateFrameObjects(uint32_t work_w, uint32_t work_h,
                                            uint32_t out_w, uint32_t out_h,
                                            std::string& why) {
    // Frames arrive and leave at out res; the network sees work res.
    src = Tex(device, out_w, out_h, DXGI_FORMAT_B8G8R8A8_UNORM,
              D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    // Linear fp16, the format the network is built around.
    color = Tex(device, work_w, work_h, DXGI_FORMAT_R16G16B16A16_FLOAT,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    depth = Tex(device, work_w, work_h, DXGI_FORMAT_R32_FLOAT,
                D3D12_RESOURCE_FLAG_NONE,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    mv = Tex(device, work_w, work_h, DXGI_FORMAT_R16G16_FLOAT,
             D3D12_RESOURCE_FLAG_NONE,
             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    // The upscaler's output stays at WORK resolution on purpose. The runtime
    // takes its colour from the dispatch output, not from the input, so this
    // is the only thing that decides what the network costs: with the output
    // at 2560x1440 it spent 45-49 ms a frame whatever the render size, and
    // the scale slider did nothing. At work resolution the slider is the
    // control it was meant to be, and the last step back up to the output
    // size is the resample pass below.
    //
    // It also has to carry a typed UAV store for the runtime's residual
    // apply, which B8G8R8A8 does not guarantee - hence RGBA8 here and one
    // GPU pass to put the channels back in protocol order.
    out = Tex(device, work_w, work_h, DXGI_FORMAT_R16G16B16A16_FLOAT,
              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    bgra = Tex(device, out_w, out_h, DXGI_FORMAT_R8G8B8A8_UNORM,
               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!src || !color || !depth || !mv || !out || !bgra) {
        why = "frame textures failed"; return false;
    }

    upload_pitch = Align(out_w * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    upload_bytes = (UINT64)upload_pitch * out_h;
    readback_pitch = upload_pitch;
    readback_bytes = upload_bytes;
    upload = Buffer(device, upload_bytes, D3D12_HEAP_TYPE_UPLOAD,
                    D3D12_RESOURCE_STATE_GENERIC_READ);
    readback = Buffer(device, readback_bytes, D3D12_HEAP_TYPE_READBACK,
                      D3D12_RESOURCE_STATE_COPY_DEST);
    if (!upload || !readback) { why = "staging buffers failed"; return false; }

    // Descriptors: [0]=src SRV [1]=color UAV (downscale), [2]=out SRV
    // [3]=bgra UAV (channel order). Written once, used every frame.
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    srv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    device->CreateShaderResourceView(src, &srv, h);
    h.ptr += heap_step;
    uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    device->CreateUnorderedAccessView(color, nullptr, &uav, h);
    h.ptr += heap_step;
    srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    device->CreateShaderResourceView(out, &srv, h);
    h.ptr += heap_step;
    uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    device->CreateUnorderedAccessView(bgra, nullptr, &uav, h);

    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = device;
    ffxCreateContextDescUpscale desc{};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    desc.header.pNext = &backend.header;
    // The colour we hand over is linear light now, so say so: the flag
    // changes how the upscaler and the hooked network read it.
    desc.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE |
                 FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
    desc.maxRenderSize = { work_w, work_h };
    desc.maxUpscaleSize = { work_w, work_h };
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
    Rel(bgra); Rel(out); Rel(mv); Rel(depth); Rel(color); Rel(src);
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

void DlssNrEngine::Impl::WaitGpu(DWORD ms) {
    if (!queue || !fence || !fence_event) return;
    queue->Signal(fence, ++fence_value);
    if (fence->GetCompletedValue() < fence_value) {
        fence->SetEventOnCompletion(fence_value, fence_event);
        WaitForSingleObject(fence_event, ms);
    }
}

void DlssNrEngine::SetEffect(const EffectParams& p) {
    if (p == effect_ && !ini_path_.empty()) return;
    effect_ = p;
    WriteIni();
}

void DlssNrEngine::WriteIni() const {
    if (ini_path_.empty()) return;
    // Everything the runtime reads, in one write: the keys it does not find
    // fall back to ITS defaults, and one of those defaults (Scale=0.03125)
    // is what made the whole pass invisible. UseFsrInputs=1 is not optional
    // either - with 0 the upscaler hook is never armed and no frame is ever
    // processed, silently.
    char env[32];
    float scale_max = 0.25f;
    if (GetEnvironmentVariableA("NS_AMD_NR_SCALE_MAX", env, sizeof(env))) {
        float v = (float)atof(env);
        if (v > 0.0f && v <= 4.0f) scale_max = v;
    }
    const float intensity = effect_.intensity < 0.0f ? 0.0f
                          : (effect_.intensity > 1.0f ? 1.0f : effect_.intensity);
    FILE* f = nullptr;
    if (fopen_s(&f, ini_path_.c_str(), "wb") != 0 || !f) {
        Say("cannot write %s - the effect controls will not reach the runtime",
            ini_path_.c_str());
        return;
    }
    fprintf(f,
            "[DlssNrOnAmd]\n"
            "Enabled=1\n"
            "UseFsrInputs=1\n"
            "UseDepth=0\n"
            "Interop=1\n"
            "Inline=1\n"
            "InlineWaitMs=200\n"
            "Temporal=1\n"
            "Tonemap=-1\n"
            "HipDevice=-1\n"
            "Scale=%.5f\n"
            "LocalTone=%.3f\n"
            "LocalStructure=%.3f\n"
            "SkinStructure=%.3f\n"
            "UseAutoMask=%u\n"
            "ToneChannels=%u\n",
            intensity * scale_max, effect_.local_tone, effect_.local_structure,
            effect_.skin_structure, effect_.auto_mask, effect_.tone_channels);
    fclose(f);
}

DlssNrEngine::~DlssNrEngine() { Stop(); }

bool DlssNrEngine::Prepare(uint32_t out_w, uint32_t out_h, std::string& why) {
    if (!out_w || !out_h) { why = "zero frame size"; return false; }
    if (!impl_) impl_ = new Impl();

    // Find where the runtime lives before anything else: its ini has to be
    // written before it loads, and the menu's values go into that ini.
    if (impl_->home.empty()) {
        const std::wstring dir = ExeDir();
        std::wstring candidates[3];
        int n = 0;
        wchar_t env[MAX_PATH];
        if (GetEnvironmentVariableW(L"NS_AMD_NR_RUNTIME", env, MAX_PATH)) {
            std::wstring p = env;
            if (!p.empty() && p.back() != L'\\') p += L'\\';
            candidates[n++] = p;
        }
        candidates[n++] = dir;
        candidates[n++] = dir + L"..\\weights\\";
        for (int i = 0; i < n; ++i) {
            if (GetFileAttributesW((candidates[i] + kProxyName).c_str())
                != INVALID_FILE_ATTRIBUTES) {
                impl_->home = candidates[i];
                break;
            }
        }
        if (impl_->home.empty()) impl_->home = dir + L"..\\weights\\";
        ini_path_ = ToUtf8(impl_->home + L"dlssnr_on_amd.ini");
    }

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
                                  -32000, -32000, (int)out_w, (int)out_h,
                                  nullptr, nullptr, wc.hInstance, nullptr);
    if (!impl_->hwnd) { why = "no host window"; return false; }
    return true;
}

bool DlssNrEngine::Start(uint32_t work_w, uint32_t work_h,
                         uint32_t out_w, uint32_t out_h, std::string& why) {
    if (ready_) return true;
    if (!work_w || !work_h || !out_w || !out_h) { why = "zero frame size"; return false; }
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
    // NS_AMD_NR_FSR_ONLY=1 builds the same pipeline without the neural
    // runtime in it. It exists to answer "is this the network or the
    // upscaler?" about anything we see in the picture, which is not a
    // question you can answer by staring at one image.
    const bool fsr_only = EnvUint("NS_AMD_NR_FSR_ONLY", 0) != 0;
    for (int i = 0; i < n_candidates && !impl_->proxy && !fsr_only; ++i) {
        const std::wstring path = candidates[i] + kProxyName;
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        home = candidates[i];
        // Before the load, so the hook lines we wait for are this run's.
        log_from = FileSize(ToUtf8(home + L"dlssnr_on_amd.log"));
        impl_->proxy = LoadLibraryW(path.c_str());
    }
    if (!impl_->proxy && !fsr_only) {
        why = "no DLSS-NR runtime (version.dll) beside the worker or in amd_mode/weights";
        return false;
    }
    if (fsr_only) {
        home = dir + L"..\\weights\\";
        Say("NS_AMD_NR_FSR_ONLY=1: upscaler only, no neural runtime");
    }
    impl_->upscaler = LoadLibraryW((home + kUpscalerName).c_str());
    if (!impl_->upscaler) impl_->upscaler = LoadLibraryW((dir + kUpscalerName).c_str());
    if (!impl_->upscaler) impl_->upscaler = LoadLibraryW(kUpscalerName);
    if (!impl_->upscaler) {
        why = "no amd_fidelityfx_upscaler_dx12.dll beside the runtime";
        return false;
    }
    impl_->create = (PfnFfxCreateContext)(void*)GetProcAddress(impl_->upscaler, "ffxCreateContext");
    impl_->dispatch = (PfnFfxDispatch)(void*)GetProcAddress(impl_->upscaler, "ffxDispatch");
    impl_->destroy = (PfnFfxDestroyContext)(void*)GetProcAddress(impl_->upscaler, "ffxDestroyContext");
    if (!impl_->create || !impl_->dispatch || !impl_->destroy) {
        why = "upscaler DLL has no FFX API entries";
        return false;
    }

    // The runtime hooks D3D12/DXGI from a thread it starts on load, and it
    // builds a dummy device first. Anything we create before that lands is
    // invisible to it - this wait is the difference between a neural pass and
    // a silent passthrough. See docs/AMD_HIP_HOSTING.md.
    //
    // It is a wait for evidence, not a blind sleep: the runtime names every
    // detour in its own log, and the pipeline gives a worker five seconds to
    // answer a frame, so a fixed guess would spend that budget doing nothing
    // on a machine where the hooks land in 200 ms.
    const uint32_t cap = EnvUint("NS_AMD_NR_WARMUP_MS", 3000);
    const std::string log_path = ToUtf8(home + L"dlssnr_on_amd.log");
    const char* kLastHook = "hooked IDXGISwapChain1::Present1";
    DWORD waited = 0;
    bool hooked = fsr_only;  // nothing to wait for without the runtime
    while (waited < cap && !fsr_only) {
        if (LogHas(log_path, log_from, kLastHook)) { hooked = true; break; }
        Sleep(25);
        waited += 25;
    }
    if (hooked && !fsr_only) {
        Say("runtime hooks in after %u ms", waited);
    } else if (fsr_only) {
        // nothing to report
    } else {
        Say("runtime hooks not seen in %u ms; going on anyway, watch %s",
            waited, log_path.c_str());
    }

    if (!impl_->CreateDeviceObjects(out_w, out_h, why) ||
        !impl_->CreatePipeline(why) ||
        !impl_->CreateFrameObjects(work_w, work_h, out_w, out_h, why)) {
        return false;
    }

    // No warm-up presents here on purpose. Present belongs to the thread that
    // owns the window, and that thread is the one serving frames; presenting
    // from this one would wait on a pump that is busy answering the pipe. The
    // runtime's engine init runs on the first few frames we do serve.
    work_w_ = work_w; work_h_ = work_h;
    out_w_ = out_w; out_h_ = out_h;
    ready_ = true;
    Say("engine up: network %ux%u -> output %ux%u", work_w, work_h, out_w, out_h);
    return true;
}

bool DlssNrEngine::Resize(uint32_t work_w, uint32_t work_h,
                          uint32_t out_w, uint32_t out_h, std::string& why) {
    if (!ready_) return false;
    if (work_w == work_w_ && work_h == work_h_ && out_w == out_w_ && out_h == out_h_) {
        return true;
    }
    impl_->WaitGpu(2000);
    impl_->DestroyFrameObjects();
    if (out_w != out_w_ || out_h != out_h_) {
        if (impl_->swap) impl_->swap->ResizeBuffers(0, out_w, out_h, DXGI_FORMAT_UNKNOWN, 0);
    }
    if (!impl_->CreateFrameObjects(work_w, work_h, out_w, out_h, why)) {
        ready_ = false;
        return false;
    }
    work_w_ = work_w; work_h_ = work_h;
    out_w_ = out_w; out_h_ = out_h;
    Say("resized: network %ux%u -> output %ux%u", work_w, work_h, out_w, out_h);
    return true;
}

bool DlssNrEngine::Dispatch(const uint8_t* bgra_in, uint8_t* bgra_out,
                            size_t bytes, const FrameParams& p) {
    if (!ready_ || !impl_) return false;
    const size_t expect = (size_t)out_w_ * out_h_ * 4;
    if (bytes != expect) return false;

    Impl& s = *impl_;

    uint8_t* mapped = nullptr;
    D3D12_RANGE nothing{ 0, 0 };
    if (FAILED(s.upload->Map(0, &nothing, (void**)&mapped))) return false;
    for (uint32_t y = 0; y < out_h_; ++y) {
        memcpy(mapped + (size_t)y * s.upload_pitch, bgra_in + (size_t)y * out_w_ * 4,
               (size_t)out_w_ * 4);
    }
    s.upload->Unmap(0, nullptr);

    s.alloc->Reset();
    s.cl->Reset(s.alloc, nullptr);

    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = s.src; dst.SubresourceIndex = 0;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.pResource = s.upload;
    src.PlacedFootprint.Offset = 0;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = out_w_;
    src.PlacedFootprint.Footprint.Height = out_h_;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = s.upload_pitch;
    s.cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Barrier(s.cl, s.src, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    ID3D12DescriptorHeap* heaps[] = { s.heap };
    s.cl->SetDescriptorHeaps(1, heaps);
    s.cl->SetComputeRootSignature(s.root);
    s.cl->SetPipelineState(s.pso);

    D3D12_GPU_DESCRIPTOR_HANDLE table = s.heap->GetGPUDescriptorHandleForHeapStart();
    // Pass 1: the captured frame down to the network's resolution.
    UINT c1[4] = { work_w_, work_h_, 0, 0 };
    s.cl->SetComputeRootDescriptorTable(0, table);
    s.cl->SetComputeRoot32BitConstants(1, 4, c1, 0);
    s.cl->Dispatch(Groups(work_w_), Groups(work_h_), 1);
    Barrier(s.cl, s.color, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    ffxDispatchDescUpscale dd{};
    dd.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dd.commandList = s.cl;
    dd.color = ffxApiGetResourceDX12(s.color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dd.depth = ffxApiGetResourceDX12(s.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dd.motionVectors = ffxApiGetResourceDX12(s.mv, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dd.output = ffxApiGetResourceDX12(s.out, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    dd.jitterOffset = { 0.0f, 0.0f };
    dd.motionVectorScale = { (float)work_w_, (float)work_h_ };
    dd.renderSize = { work_w_, work_h_ };
    dd.upscaleSize = { work_w_, work_h_ };
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

    // Pass 2: the network's output into protocol channel order, on the GPU.
    Barrier(s.cl, s.out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    s.cl->SetDescriptorHeaps(1, heaps);
    s.cl->SetComputeRootSignature(s.root);
    s.cl->SetPipelineState(s.pso);
    D3D12_GPU_DESCRIPTOR_HANDLE table2 = table;
    table2.ptr += (UINT64)s.heap_step * 2;
    UINT c2[4] = { out_w_, out_h_, 1, 0 };
    s.cl->SetComputeRootDescriptorTable(0, table2);
    s.cl->SetComputeRoot32BitConstants(1, 4, c2, 0);
    s.cl->Dispatch(Groups(out_w_), Groups(out_h_), 1);

    Barrier(s.cl, s.bgra, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION rdst{}, rsrc{};
    rdst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    rdst.pResource = s.readback;
    rdst.PlacedFootprint.Offset = 0;
    rdst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rdst.PlacedFootprint.Footprint.Width = out_w_;
    rdst.PlacedFootprint.Footprint.Height = out_h_;
    rdst.PlacedFootprint.Footprint.Depth = 1;
    rdst.PlacedFootprint.Footprint.RowPitch = s.readback_pitch;
    rsrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    rsrc.pResource = s.bgra; rsrc.SubresourceIndex = 0;
    s.cl->CopyTextureRegion(&rdst, 0, 0, 0, &rsrc, nullptr);

    // Back to the states the next frame starts from.
    Barrier(s.cl, s.bgra, D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(s.cl, s.out, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(s.cl, s.color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(s.cl, s.src, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_DEST);
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

    uint8_t* got = nullptr;
    D3D12_RANGE all{ 0, (SIZE_T)s.readback_bytes };
    if (FAILED(s.readback->Map(0, &all, (void**)&got))) return false;
    for (uint32_t y = 0; y < out_h_; ++y) {
        memcpy(bgra_out + (size_t)y * out_w_ * 4,
               got + (size_t)y * s.readback_pitch, (size_t)out_w_ * 4);
    }
    s.readback->Unmap(0, &nothing);
    return true;
}

void DlssNrEngine::Stop() {
    if (!impl_) return;
    ready_ = false;
    // Wait for our own work, then let go without releasing anything: the
    // runtime's detours and its worker thread are still live in this process
    // and hold references to these objects. Tearing them down here is a race
    // we cannot win from outside, and the process exits right after.
    impl_->WaitGpu(1000);
    impl_ = nullptr;
}

}  // namespace nsamd
