// Spout2 receiver process: connects to the "NeuralScreenTest" sender,
// pulls frames and checks the centre pixel colour. Exit code 0 = match.
//
// Build:  build-spout-test.bat
// Run:    spout_receiver.exe
#include "spout/SpoutDX.h"
#include <d3d11.h>
#include <cstdio>
#include <cstring>
#include <vector>

#pragma comment(lib, "d3d11.lib")

int main()
{
    const BYTE R = 200, G = 30, B = 90;

    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 0, levels, 1, D3D11_SDK_VERSION,
                                 &dev, nullptr, &ctx)))
    { printf("D3D11CreateDevice failed\n"); return 1; }

    spoutDX receiver;
    if (!receiver.OpenDirectX11(dev)) { printf("OpenDirectX11 failed\n"); return 1; }
    // No fixed name: connect to the ACTIVE sender (NeuralScreen when the
    // worker runs, NeuralScreenTest for the standalone test sender).
    receiver.SetReceiverName(nullptr);

    printf("receiver waiting for the active sender...\n");
    bool got = false;
    bool ever_connected = false;
    ID3D11Texture2D *rx_tex = nullptr;
    for (int i = 0; i < 200 && !got; ++i)   // up to 20 s
    {
        if (i == 0 || i == 50)
        {
            char active[256] = {};
            bool has_active = receiver.GetActiveSender(active);
            printf("probe %d: senders=%d active=%s%s\n", i,
                   receiver.GetSenderCount(),
                   has_active ? active : "(none)",
                   has_active ? "" : " (GetActiveSender false)");
        }
        // First call: ReceiveTexture() connects and reports a size change
        // (m_bUpdated). The app then creates its own texture and calls
        // ReceiveTexture(&tex) to copy into it.
        if (rx_tex == nullptr)
        {
            bool rt = receiver.ReceiveTexture();
            if (i < 5)
                printf("  iter %d: ReceiveTexture()=%d connected=%d updated=%d\n",
                       i, (int)rt, (int)receiver.IsConnected(),
                       (int)receiver.IsUpdated());
            if (rt)
            {
                // Create the receive texture from the sender's size. The
                // size is valid after the first successful receive even
                // when m_bUpdated is false (same share handle).
                UINT rw = receiver.GetSenderWidth(), rh = receiver.GetSenderHeight();
                if (rw > 0 && rh > 0)
                {
                    printf("sender %s %ux%u - creating receive texture\n",
                           receiver.GetSenderName(), rw, rh);
                    D3D11_TEXTURE2D_DESC td = {};
                    td.Width = rw; td.Height = rh; td.MipLevels = 1; td.ArraySize = 1;
                    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
                    td.Usage = D3D11_USAGE_DEFAULT;
                    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                    if (FAILED(dev->CreateTexture2D(&td, nullptr, &rx_tex)))
                        printf("receive texture creation failed\n");
                }
            }
        }
        else
        {
            if (receiver.ReceiveTexture(&rx_tex))
            {
                if (!ever_connected)
                {
                    ever_connected = true;
                    printf("connected to %s (%ux%u)\n",
                           receiver.GetSenderName(),
                           receiver.GetSenderWidth(), receiver.GetSenderHeight());
                }
                UINT rw = receiver.GetSenderWidth(), rh = receiver.GetSenderHeight();
                D3D11_TEXTURE2D_DESC rd = {};
                rx_tex->GetDesc(&rd);
                rd.Usage = D3D11_USAGE_STAGING; rd.BindFlags = 0;
                rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                ID3D11Texture2D *staging = nullptr;
                if (SUCCEEDED(dev->CreateTexture2D(&rd, nullptr, &staging)))
                {
                    ctx->CopyResource(staging, rx_tex);
                    D3D11_MAPPED_SUBRESOURCE map = {};
                    if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map)))
                    {
                        const BYTE *src = (const BYTE *)map.pData;
                        size_t idx = ((size_t)rh / 2 * map.RowPitch) + (rw / 2) * 4;
                        BYTE pr = src[idx + 2], pg = src[idx + 1], pb = src[idx];
                        const char *name = receiver.GetSenderName();
                        const bool is_test = name && strstr(name, "NeuralScreenTest") != nullptr;
                        if (is_test)
                        {
                            printf("received %ux%u, centre RGB(%u,%u,%u) expected (%u,%u,%u)\n",
                                   rw, rh, pr, pg, pb, R, G, B);
                            got = (pr == R && pg == G && pb == B);
                        }
                        else
                        {
                            // Live sender (the worker): any non-black frame
                            // proves the transport. The desktop is dark, so
                            // "not black" means the pixels really moved.
                            const bool alive = (pr + pg + pb) > 0;
                            printf("received %ux%u from %s, centre RGB(%u,%u,%u) alive=%d\n",
                                   rw, rh, name ? name : "?", pr, pg, pb, (int)alive);
                            got = alive;
                        }
                        ctx->Unmap(staging, 0);
                    }
                    staging->Release();
                }
            }
        }
        Sleep(100);
    }
    if (rx_tex) rx_tex->Release();

    receiver.ReleaseReceiver();
    ctx->Release();
    dev->Release();

    if (!got) { printf("FAIL: no matching frame received\n"); return 1; }
    printf("OK: Spout2 cross-process round-trip works\n");
    return 0;
}
