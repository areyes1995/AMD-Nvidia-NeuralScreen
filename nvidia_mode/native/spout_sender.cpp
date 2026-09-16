// Spout2 sender process: publishes a solid-colour texture for N seconds.
// The receiver (spout_receiver.exe) runs in a separate process - Spout2
// shared memory does not support sender+receiver in one process.
//
// Build:  build-spout-test.bat
// Run:    spout_sender.exe [seconds]
#include "spout/SpoutDX.h"
#include <d3d11.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

#pragma comment(lib, "d3d11.lib")

int main(int argc, char **argv)
{
    const int seconds = argc > 1 ? atoi(argv[1]) : 10;
    const UINT W = 320, H = 180;
    const BYTE R = 200, G = 30, B = 90;

    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 0, levels, 1, D3D11_SDK_VERSION,
                                 &dev, nullptr, &ctx)))
    { printf("D3D11CreateDevice failed\n"); return 1; }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    std::vector<BYTE> pixels((size_t)W * H * 4);
    for (size_t i = 0; i < pixels.size(); i += 4)
    {
        pixels[i] = B; pixels[i + 1] = G; pixels[i + 2] = R; pixels[i + 3] = 255;
    }
    D3D11_SUBRESOURCE_DATA sd = {};
    sd.pSysMem = pixels.data();
    sd.SysMemPitch = W * 4;
    ID3D11Texture2D *tex = nullptr;
    if (FAILED(dev->CreateTexture2D(&td, &sd, &tex)))
    { printf("texture creation failed\n"); return 1; }

    spoutDX sender;
    if (!sender.OpenDirectX11(dev)) { printf("OpenDirectX11 failed\n"); return 1; }
    sender.SetSenderName("NeuralScreenTest");
    sender.SetSenderFormat(DXGI_FORMAT_R8G8B8A8_UNORM);

    printf("sender publishing %ux%u for %d s\n", W, H, seconds);
    for (int i = 0; i < seconds * 30; ++i)
    {
        if (!sender.SendTexture(tex))
        { printf("SendTexture failed at frame %d\n", i); break; }
        if (i == 0)
            printf("sender: name=%s init=%d w=%u h=%u\n",
                   sender.GetName(), (int)sender.IsInitialized(),
                   sender.GetWidth(), sender.GetHeight());
        Sleep(33);
    }

    sender.ReleaseSender();
    tex->Release();
    ctx->Release();
    dev->Release();
    printf("sender done\n");
    return 0;
}
