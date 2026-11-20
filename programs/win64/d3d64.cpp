#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>

#include "d3d64_shaders.h"

/* A 64-bit Direct3D 11 program: a device and swap chain on a window, a triangle drawn with a vertex buffer, an input
   layout, shaders and a constant buffer written through Map, read back through a staging texture, then Present. */
typedef HRESULT(WINAPI *device_fn)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT,
                                   UINT, ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
typedef HRESULT(WINAPI *create_fn)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT,
                                   UINT, const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D11Device **,
                                   D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);

struct vertex {
    float pos[4], color[4];
};

/* the pixel at (x, y) of a 2D texture, read through a staging copy */
static unsigned pixel(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *tex, int x, int y)
{
    D3D11_TEXTURE2D_DESC td;
    tex->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *staging = NULL;
    unsigned v = 0;
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (SUCCEEDED(dev->CreateTexture2D(&td, NULL, &staging))) {
        ctx->CopyResource(staging, tex);
        if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
            memcpy(&v, (const unsigned char *)m.pData + y * m.RowPitch + x * 4, 4);
            ctx->Unmap(staging, 0);
        }
        staging->Release();
    }
    return v & 0xffffff;
}

int main(int argc, char **argv)
{
    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "d3d64";
    RegisterClassA(&wc);
    /* d3d64 border: a window without borders over the whole screen */
    bool border = argc > 1 && !strcmp(argv[1], "border");
    HWND window = border ? CreateWindowExA(0, "d3d64", "Direct3D 11", WS_POPUP, 0, 0, GetSystemMetrics(SM_CXSCREEN),
                                           GetSystemMetrics(SM_CYSCREEN), NULL, NULL, wc.hInstance, NULL)
                         : CreateWindowExA(0, "d3d64", "Direct3D 11", WS_OVERLAPPEDWINDOW, 40, 40, 200, 160, NULL,
                                           NULL, wc.hInstance, NULL);
    ShowWindow(window, SW_SHOW);

    HMODULE d3d11 = LoadLibraryA("d3d11.dll");
    create_fn create = (create_fn)(void *)GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain");
    device_fn create_device = (device_fn)(void *)GetProcAddress(d3d11, "D3D11CreateDevice");
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferDesc.Width = 64;
    sd.BufferDesc.Height = 64;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.OutputWindow = window;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain *chain = NULL;
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    D3D_FEATURE_LEVEL level = (D3D_FEATURE_LEVEL)0;
    /* without a desktop (the serial console) there is no window: the picture goes to a texture */
    HRESULT hr = !create || !create_device ? E_FAIL
                 : window ? create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &sd, &chain,
                                   &dev, &level, &ctx)
                          : create_device(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev,
                                          &level, &ctx);
    if (FAILED(hr) || !dev || !ctx || (window && !chain)) {
        printf("d3d11 create failed %lx window=%d\n", (unsigned long)hr, window != NULL);
        return 1;
    }

    IDXGIDevice *dxgi = NULL;
    IDXGIAdapter *adapter = NULL;
    DXGI_ADAPTER_DESC ad = {};
    dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi);
    if (dxgi && SUCCEEDED(dxgi->GetAdapter(&adapter)))
        adapter->GetDesc(&ad);
    IDXGIOutput *out = NULL;
    UINT modes = 0;
    if (adapter && SUCCEEDED(adapter->EnumOutputs(0, &out)))
        out->GetDisplayModeList(DXGI_FORMAT_B8G8R8A8_UNORM, 0, &modes, NULL);

    ID3D11Texture2D *back = NULL;
    if (chain) {
        chain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back);
    } else {
        D3D11_TEXTURE2D_DESC bd2 = {64, 64, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                    D3D11_BIND_RENDER_TARGET, 0, 0};
        dev->CreateTexture2D(&bd2, NULL, &back);
    }
    ID3D11RenderTargetView *rtv = NULL;
    dev->CreateRenderTargetView(back, NULL, &rtv);

    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps = NULL;
    HRESULT shaders = dev->CreateVertexShader(vs_blob, sizeof(vs_blob), NULL, &vs);
    if (SUCCEEDED(shaders))
        shaders = dev->CreatePixelShader(ps_blob, sizeof(ps_blob), NULL, &ps);
    D3D11_INPUT_ELEMENT_DESC el[2] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ID3D11InputLayout *layout = NULL;
    dev->CreateInputLayout(el, 2, vs_blob, sizeof(vs_blob), &layout);

    /* a triangle over the whole target, in the left half only through the scissor-free viewport */
    vertex v[3] = {{{-1, -1, 0.5f, 1}, {1, 1, 1, 1}}, {{-1, 3, 0.5f, 1}, {1, 1, 1, 1}}, {{3, -1, 0.5f, 1}, {1, 1, 1, 1}}};
    D3D11_BUFFER_DESC bd = {sizeof(v), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA init = {v, 0, 0};
    ID3D11Buffer *vb = NULL;
    dev->CreateBuffer(&bd, &init, &vb);
    D3D11_BUFFER_DESC cd = {16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    ID3D11Buffer *cb = NULL;
    dev->CreateBuffer(&cd, NULL, &cb);
    D3D11_MAPPED_SUBRESOURCE m = {};
    int mapped = cb && SUCCEEDED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) && m.pData;
    if (mapped) {
        const float tint[4] = {1.0f, 0.5f, 0.25f, 1.0f};
        memcpy(m.pData, tint, sizeof(tint));
        ctx->Unmap(cb, 0);
    }

    const float blue[4] = {0, 0, 1, 1};
    ctx->ClearRenderTargetView(rtv, blue);
    ctx->OMSetRenderTargets(1, &rtv, NULL);
    D3D11_VIEWPORT vp = {0, 0, 32, 64, 0, 1};
    ctx->RSSetViewports(1, &vp);
    UINT stride = sizeof(vertex), offset = 0;
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    ctx->IASetInputLayout(layout);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, NULL, 0);
    ctx->PSSetShader(ps, NULL, 0);
    ctx->PSSetConstantBuffers(0, 1, &cb);
    D3D11_QUERY_DESC qd = {D3D11_QUERY_OCCLUSION, 0};
    ID3D11Query *q = NULL;
    dev->CreateQuery(&qd, &q);
    ctx->Begin(q);
    ctx->Draw(3, 0);
    ctx->End(q);
    UINT64 samples = 0;
    while (ctx->GetData(q, &samples, sizeof(samples), 0) == S_FALSE)
        ;

    /* read back: the left half drawn, the right half the clear color */
    D3D11_TEXTURE2D_DESC td;
    back->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *staging = NULL;
    dev->CreateTexture2D(&td, NULL, &staging);
    ctx->CopyResource(staging, back);
    unsigned left = 0, right = 0;
    if (staging && SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
        const unsigned char *row = (const unsigned char *)m.pData + 32 * m.RowPitch;
        memcpy(&left, row + 8 * 4, 4);
        memcpy(&right, row + 56 * 4, 4);
        ctx->Unmap(staging, 0);
    }
    int presented = chain ? chain->Present(0, 0) == S_OK : -1;

    ID3D11Device *again = NULL;
    ctx->GetDevice(&again);
    ID3D11RenderTargetView *bound = NULL;
    ctx->OMGetRenderTargets(1, &bound, NULL);

    /* a deferred context: a clear and a draw kept in a command list, made on the immediate context later */
    ID3D11DeviceContext *def = NULL;
    unsigned before = 1, after = 0;
    int deferred = 0;
    D3D11_TEXTURE2D_DESC sd2 = {16, 16, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                D3D11_BIND_RENDER_TARGET, 0, 0};
    ID3D11Texture2D *side = NULL;
    ID3D11RenderTargetView *side_rtv = NULL;
    if (SUCCEEDED(dev->CreateDeferredContext(0, &def)) && SUCCEEDED(dev->CreateTexture2D(&sd2, NULL, &side)) &&
        SUCCEEDED(dev->CreateRenderTargetView(side, NULL, &side_rtv))) {
        const float green[4] = {0, 1, 0, 1};
        def->ClearRenderTargetView(side_rtv, green);
        def->OMSetRenderTargets(1, &side_rtv, NULL);
        D3D11_VIEWPORT vp2 = {0, 0, 8, 16, 0, 1};
        def->RSSetViewports(1, &vp2);
        def->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        def->IASetInputLayout(layout);
        def->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        def->VSSetShader(vs, NULL, 0);
        def->PSSetShader(ps, NULL, 0);
        def->PSSetConstantBuffers(0, 1, &cb);
        if (SUCCEEDED(def->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            const float tint[4] = {0.25f, 1.0f, 0.0f, 1.0f};
            memcpy(m.pData, tint, sizeof(tint));
            def->Unmap(cb, 0);
        }
        def->Draw(3, 0);
        ID3D11CommandList *list = NULL;
        if (SUCCEEDED(def->FinishCommandList(FALSE, &list))) {
            before = pixel(dev, ctx, side, 4, 8);
            ctx->ExecuteCommandList(list, TRUE);
            after = pixel(dev, ctx, side, 4, 8) << 0;
            unsigned right_half = pixel(dev, ctx, side, 12, 8);
            ID3D11RenderTargetView *still = NULL;
            ctx->OMGetRenderTargets(1, &still, NULL);
            deferred = def->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED && before == 0 && right_half == 0x00ff00 &&
                       still == rtv;
            list->Release();
        }
    }

    /* stream output: the vertex shader's colours and positions written to a buffer, nothing drawn */
    int streamed = 0;
    {
        D3D11_SO_DECLARATION_ENTRY so[2] = {{0, "COLOR", 0, 0, 4, 0}, {0, "SV_Position", 0, 0, 2, 0}};
        UINT so_stride = 24, so_off = 0;
        D3D11_BUFFER_DESC sbd = {72, D3D11_USAGE_DEFAULT, D3D11_BIND_STREAM_OUTPUT, 0, 0, 0};
        D3D11_BUFFER_DESC rbd = {72, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
        ID3D11GeometryShader *sogs = NULL;
        ID3D11Buffer *sob = NULL, *sor = NULL;
        if (SUCCEEDED(dev->CreateGeometryShaderWithStreamOutput(vs_blob, sizeof(vs_blob), so, 2, &so_stride, 1,
                                                                D3D11_SO_NO_RASTERIZED_STREAM, NULL, &sogs)) &&
            SUCCEEDED(dev->CreateBuffer(&sbd, NULL, &sob)) && SUCCEEDED(dev->CreateBuffer(&rbd, NULL, &sor))) {
            ctx->GSSetShader(sogs, NULL, 0);
            ctx->SOSetTargets(1, &sob, &so_off);
            ctx->Draw(3, 0);
            ID3D11Buffer *got = NULL;
            ctx->SOGetTargets(1, &got);
            ctx->GSSetShader(NULL, NULL, 0);
            ctx->SOSetTargets(0, NULL, NULL);
            ctx->CopyResource(sor, sob);
            D3D11_MAPPED_SUBRESOURCE sm = {};
            if (SUCCEEDED(ctx->Map(sor, 0, D3D11_MAP_READ, 0, &sm))) {
                const float *f = (const float *)sm.pData;
                streamed = got == sob && f[0] == 1 && f[3] == 1 && f[4] == -1 && f[5] == -1 && f[10] == -1 && f[11] == 3 &&
                           f[16] == 3 && f[17] == -1;
                ctx->Unmap(sor, 0);
            }
            if (got)
                got->Release();
        }
        if (sogs)
            sogs->Release();
        if (sob)
            sob->Release();
        if (sor)
            sor->Release();
    }

    /* a large dynamic vertex buffer: a triangle written with WRITE_DISCARD at the start, then one with NO_OVERWRITE
       far into it; each drawn into its half of the target, both must arrive */
    int dynamic = 0;
    {
        D3D11_BUFFER_DESC dd = {1 << 20, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
        ID3D11Buffer *dvb = NULL;
        const UINT far_off = 600000;
        if (SUCCEEDED(dev->CreateBuffer(&dd, NULL, &dvb))) {
            const float cyan[4] = {0, 1, 1, 1}, black[4] = {0, 0, 0, 1};
            if (SUCCEEDED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                memcpy(m.pData, cyan, sizeof(cyan));
                ctx->Unmap(cb, 0);
            }
            ctx->OMSetRenderTargets(1, &rtv, NULL);
            ctx->ClearRenderTargetView(rtv, black);
            D3D11_VIEWPORT halves[2] = {{0, 0, 32, 64, 0, 1}, {32, 0, 32, 64, 0, 1}};
            for (int i = 0; i < 2; i++) {
                UINT at = i ? far_off : 0;
                if (SUCCEEDED(ctx->Map(dvb, 0, i ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                    memcpy((unsigned char *)m.pData + at, v, sizeof(v));
                    ctx->Unmap(dvb, 0);
                }
                ctx->RSSetViewports(1, &halves[i]);
                ctx->IASetVertexBuffers(0, 1, &dvb, &stride, &at);
                ctx->Draw(3, 0);
            }
            unsigned l = pixel(dev, ctx, back, 8, 32), r = pixel(dev, ctx, back, 56, 32);
            dynamic = l == 0x00ffff && r == 0x00ffff;
            if (!dynamic)
                printf("d3d11 dynamic left=%06x right=%06x\n", l, r);
            ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
            ctx->RSSetViewports(1, &vp);
            dvb->Release();
        }
    }

    /* the newer interfaces: a fence signalled on the immediate context, and the device's lock */
    int newer = 0;
    ID3D11Device5 *dev5 = NULL;
    ID3D11DeviceContext4 *ctx4 = NULL;
    ID3D10Multithread *mt = NULL;
    ID3D11Fence *fence = NULL;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D11Device5), (void **)&dev5)) &&
        SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&ctx4)) &&
        SUCCEEDED(dev->QueryInterface(__uuidof(ID3D10Multithread), (void **)&mt)) &&
        SUCCEEDED(dev5->CreateFence(3, D3D11_FENCE_FLAG_NONE, __uuidof(ID3D11Fence), (void **)&fence))) {
        HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
        mt->SetMultithreadProtected(TRUE);
        mt->Enter();
        fence->SetEventOnCompletion(5, ev);
        bool early = WaitForSingleObject(ev, 0) == WAIT_OBJECT_0;
        ctx4->Signal(fence, 5);
        mt->Leave();
        newer = fence->GetCompletedValue() == 5 && !early && WaitForSingleObject(ev, 0) == WAIT_OBJECT_0 &&
                mt->GetMultithreadProtected();
        CloseHandle(ev);
    }
    IDXGIOutput6 *out6 = NULL;
    IDXGIFactory7 *factory7 = NULL;
    DXGI_OUTPUT_DESC1 od = {};
    if (newer && out && SUCCEEDED(out->QueryInterface(__uuidof(IDXGIOutput6), (void **)&out6)) &&
        SUCCEEDED(out6->GetDesc1(&od)) && SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory7), (void **)&factory7)))
        newer = od.BitsPerColor == 8 && od.MaxLuminance > 0 && od.DesktopCoordinates.right > 0 ? 1 : 0;
    else
        newer = 0;
    if (factory7)
        factory7->Release();
    if (out6)
        out6->Release();
    if (fence)
        fence->Release();
    if (mt)
        mt->Release();
    if (ctx4)
        ctx4->Release();
    if (dev5)
        dev5->Release();

    char name[64] = "";
    WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof(name), NULL, NULL);
    printf("d3d11 level=%x adapter=%s modes=%d shaders=%d map=%d samples=%llu left=%06x right=%06x present=%d "
           "get=%d deferred=%d drawn=%06x so=%d newer=%d dynamic=%d\n",
           (unsigned)level, name, modes > 0, SUCCEEDED(shaders), mapped, (unsigned long long)samples,
           left & 0xffffff, right & 0xffffff, presented, again == dev && bound == rtv, deferred, after, streamed, newer,
           dynamic);
    /* d3d64 bench: frames of 1000 draws, each after its own constant buffer write, the way games draw; the last
       frame is read back so the time includes the host's work */
    if (argc > 1 && !strcmp(argv[1], "bench")) {
        D3D11_VIEWPORT small = {0, 0, 4, 4, 0, 1};
        ctx->RSSetViewports(1, &small);
        DWORD t0 = GetTickCount();
        int frames = 0;
        for (; frames < 20; frames++) {
            ctx->ClearRenderTargetView(rtv, blue);
            for (int i = 0; i < 1000; i++) {
                if (SUCCEEDED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                    const float tint[4] = {(float)(i & 255) / 255.0f, 0.5f, 0.25f, 1.0f};
                    memcpy(m.pData, tint, sizeof(tint));
                    ctx->Unmap(cb, 0);
                }
                ctx->Draw(3, 0);
            }
            if (chain)
                chain->Present(0, 0);
        }
        unsigned last = pixel(dev, ctx, back, 1, 1);
        printf("d3d11 bench %d frames of 1000 draws in %lu ms pixel=%06x\n", frames,
               (unsigned long)(GetTickCount() - t0), last);
        /* the same with each draw's triangle appended to a 4 MB dynamic vertex buffer (NO_OVERWRITE) */
        D3D11_BUFFER_DESC dd = {4 << 20, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
        ID3D11Buffer *dvb = NULL;
        if (SUCCEEDED(dev->CreateBuffer(&dd, NULL, &dvb))) {
            t0 = GetTickCount();
            UINT at = dd.ByteWidth;
            for (frames = 0; frames < 20; frames++) {
                ctx->ClearRenderTargetView(rtv, blue);
                for (int i = 0; i < 1000; i++) {
                    bool wrap = at + sizeof(v) > dd.ByteWidth;
                    if (wrap)
                        at = 0;
                    if (SUCCEEDED(ctx->Map(dvb, 0, wrap ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0,
                                           &m))) {
                        memcpy((unsigned char *)m.pData + at, v, sizeof(v));
                        ctx->Unmap(dvb, 0);
                    }
                    ctx->IASetVertexBuffers(0, 1, &dvb, &stride, &at);
                    ctx->Draw(3, 0);
                    at += sizeof(v);
                }
                if (chain)
                    chain->Present(0, 0);
            }
            last = pixel(dev, ctx, back, 1, 1);
            printf("d3d11 bench %d frames of 1000 appended draws in %lu ms pixel=%06x\n", frames,
                   (unsigned long)(GetTickCount() - t0), last);
            dvb->Release();
        }
    }
    /* d3d64 show: the triangle stays on screen, its tint changing every frame; d3d64 full: the same on the whole
       screen for five seconds */
    bool full = chain && argc > 1 && !strcmp(argv[1], "full");
    if (border) {
        SetForegroundWindow(window);
        D3D11_VIEWPORT top = {0, 0, 32, 32, 0, 1};
        ctx->RSSetViewports(1, &top);
    }
    if (full) {
        HRESULT hr = chain->SetFullscreenState(TRUE, NULL);
        BOOL state = FALSE;
        chain->GetFullscreenState(&state, NULL);
        printf("d3d11 full screen=%d set=%lx\n", state, (unsigned long)hr);
        D3D11_VIEWPORT top = {0, 0, 32, 32, 0, 1}; /* the top left quarter: shows which way up the screen is */
        ctx->RSSetViewports(1, &top);
    }
    DWORD started = GetTickCount();
    for (int frame = 0; chain && argc > 1 && (full || border || !strcmp(argv[1], "show")) &&
                        frame < (full || border ? 300 : 600);
         frame++) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
            DispatchMessageA(&msg);
        if (SUCCEEDED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            float t = (float)(frame % 120) / 120.0f;
            const float tint[4] = {1.0f - t, t, 0.5f, 1.0f};
            memcpy(m.pData, tint, sizeof(tint));
            ctx->Unmap(cb, 0);
        }
        ctx->ClearRenderTargetView(rtv, blue);
        ctx->Draw(3, 0);
        chain->Present(1, 0);
    }
    if (full || border)
        printf("d3d11 300 frames in %lu ms\n", (unsigned long)(GetTickCount() - started));
    if (full) {
        chain->SetFullscreenState(FALSE, NULL);
        ctx->ClearRenderTargetView(rtv, blue);
        chain->Present(0, 0);
        printf("d3d11 windowed again\n");
    }
    return 0;
}
