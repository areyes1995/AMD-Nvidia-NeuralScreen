#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

int main()
{
    IDXGIFactory1 *factory = nullptr;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory);
    for (UINT i = 0; ; ++i)
    {
        IDXGIAdapter1 *adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);
        printf("adapter %u: %ls vendor=0x%04X luid=%08X%08X\n", i, desc.Description,
               desc.VendorId, desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart);
        adapter->Release();
    }
    factory->Release();

    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   0, levels, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr)) { printf("D3D11CreateDevice failed 0x%08X\n", (unsigned)hr); return 1; }
    IDXGIDevice *dxgi_dev = nullptr;
    dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi_dev);
    IDXGIAdapter *ad = nullptr;
    dxgi_dev->GetAdapter(&ad);
    DXGI_ADAPTER_DESC d = {};
    ad->GetDesc(&d);
    printf("default device adapter: %ls luid=%08X%08X\n", d.Description,
           d.AdapterLuid.HighPart, d.AdapterLuid.LowPart);
    return 0;
}
