// Spout2 round-trip test: a sender and a receiver in one process.
//
// Proves the Spout2 shared-texture transport works on this machine before
// the worker integration: the sender publishes a solid-colour texture,
// the receiver (same process, same D3D11 device) pulls it back and the
// pixels are compared. If this passes, the OBS Spout2 Capture source will
// see the worker's frames too (same shared-texture mechanism).
//
// Build:  build-spout-test.bat
// Run:    spout_roundtrip.exe
#include "spout/SpoutDX.h"
#include <d3d11.h>
#include <cstdio>
#include <cstring>
#include <vector>

#pragma comment(lib, "d3d11.lib")

static ID3D11Device *g_dev = nullptr;
static ID3D11DeviceContext *g_ctx = nullptr;

static bool init_d3d11()
{
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   0, levels, 1, D3D11_SDK_VERSION,
                                   &g_dev, nullptr, &g_ctx);
    if (FAILED(hr)) { printf("D3D11CreateDevice failed 0x%08X\n", (unsigned)hr); return false; }
    return true;
}

static ID3D11Texture2D *make_texture(UINT w, UINT h, BYTE r, BYTE g, BYTE b)
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    std::vector<BYTE> pixels((size_t)w * h * 4);
    for (size_t i = 0; i < pixels.size(); i += 4)
    {
        pixels[i] = b; pixels[i + 1] = g; pixels[i + 2] = r; pixels[i + 3] = 255;
    }
    D3D11_SUBRESOURCE_DATA sd = {};
    sd.pSysMem = pixels.data();
    sd.SysMemPitch = w * 4;
    ID3D11Texture2D *tex = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&td, &sd, &tex)))
        return nullptr;
    return tex;
}

static bool read_texture(ID3D11Texture2D *tex, UINT w, UINT h, std::vector<BYTE> &out)
{
    D3D11_TEXTURE2D_DESC td = {};
    tex->GetDesc(&td);
    D3D11_TEXTURE2D_DESC rd = td;
    rd.Usage = D3D11_USAGE_STAGING; rd.BindFlags = 0; rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *staging = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&rd, nullptr, &staging)))
        return false;
    g_ctx->CopyResource(staging, tex);
    D3D11_MAPPED_SUBRESOURCE map = {};
    if (FAILED(g_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map)))
    { staging->Release(); return false; }
    out.resize((size_t)w * h * 4);
    const BYTE *src = (const BYTE *)map.pData;
    for (UINT y = 0; y < h; ++y)
        memcpy(out.data() + (size_t)y * w * 4, src + (size_t)y * map.RowPitch, (size_t)w * 4);
    g_ctx->Unmap(staging, 0);
    staging->Release();
    return true;
}

int main()
{
    if (!init_d3d11()) return 1;

    const UINT W = 320, H = 180;
    const BYTE R = 200, G = 30, B = 90;   // a colour nothing else uses

    spoutDX sender;
    if (!sender.OpenDirectX11(g_dev)) { printf("sender OpenDirectX11 failed\n"); return 1; }
    sender.SetSenderName("NeuralScreenTest");
    sender.SetSenderFormat(DXGI_FORMAT_R8G8B8A8_UNORM);

    ID3D11Texture2D *tex = make_texture(W, H, R, G, B);
    if (!tex) { printf("texture creation failed\n"); return 1; }

    // Send a few frames so the receiver has something to connect to.
    for (int i = 0; i < 5; ++i)
    {
        if (!sender.SendTexture(tex))
        { printf("SendTexture failed on frame %d\n", i); return 1; }
    }
    printf("sender published %ux%u\n", W, H);

    spoutDX receiver;
    if (!receiver.OpenDirectX11(g_dev)) { printf("receiver OpenDirectX11 failed\n"); return 1; }
    receiver.SetReceiverName("NeuralScreenTest");

    // Poll until the receiver connects and a frame arrives.
    bool got = false;
    for (int i = 0; i < 50 && !got; ++i)
    {
        if (receiver.IsConnected() && receiver.IsUpdated())
        {
            ID3D11Texture2D *rx = nullptr;
            if (receiver.ReceiveTexture(&rx) && rx)
            {
                UINT rw = receiver.GetSenderWidth(), rh = receiver.GetSenderHeight();
                std::vector<BYTE> pixels;
                if (read_texture(rx, rw, rh, pixels))
                {
                    // Check the centre pixel against the sent colour.
                    size_t idx = ((size_t)rh / 2 * rw + rw / 2) * 4;
                    BYTE pr = pixels[idx + 2], pg = pixels[idx + 1], pb = pixels[idx];
                    printf("received %ux%u, centre RGB(%u,%u,%u) expected (%u,%u,%u)\n",
                           rw, rh, pr, pg, pb, R, G, B);
                    got = (pr == R && pg == G && pb == B);
                }
                rx->Release();
            }
        }
        Sleep(100);
    }

    sender.ReleaseSender();
    receiver.ReleaseReceiver();
    tex->Release();
    g_ctx->Release();
    g_dev->Release();

    if (!got) { printf("FAIL: no matching frame received\n"); return 1; }
    printf("OK: Spout2 round-trip works\n");
    return 0;
}
