/* d3d11_smoke.c — D3D11 device + swapchain creation, clear/present loop (DXVK smoke)
 * build (mingw): x86_64-w64-mingw32-gcc -O2 d3d11_smoke.c -o d3d11_smoke.exe -ld3d11 -ldxgi -lgdi32 -luser32
 * usage: d3d11_smoke.exe [frames]
 */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

int main(int argc, char **argv) {
    int frames = (argc > 1) ? atoi(argv[1]) : 300;
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "d3d11smoke";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("d3d11smoke", "d3d11 smoke", WS_OVERLAPPEDWINDOW,
                              40, 40, 320, 240, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) { printf("FAIL CreateWindow\n"); return 1; }
    ShowWindow(hwnd, SW_SHOW);

    DXGI_SWAP_CHAIN_DESC scd = {0};
    scd.BufferDesc.Width = 320; scd.BufferDesc.Height = 240;
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.OutputWindow = hwnd;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ID3D11Device *dev = NULL; ID3D11DeviceContext *ctx = NULL; IDXGISwapChain *sc = NULL;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                                               NULL, 0, D3D11_SDK_VERSION, &scd, &sc, &dev, NULL, &ctx);
    if (FAILED(hr)) { printf("FAIL D3D11CreateDeviceAndSwapChain hr=0x%08lx\n", (unsigned long)hr); return 1; }

    ID3D11Texture2D *bb = NULL;
    hr = IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb);
    if (FAILED(hr)) { printf("FAIL GetBuffer hr=0x%08lx\n", (unsigned long)hr); return 1; }
    ID3D11RenderTargetView *rtv = NULL;
    hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, NULL, &rtv);
    if (FAILED(hr)) { printf("FAIL CreateRenderTargetView hr=0x%08lx\n", (unsigned long)hr); return 1; }

    LARGE_INTEGER f0, f1, freq; QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&f0);
    for (int i = 0; i < frames; i++) {
        FLOAT col[4] = { (i % 256) / 255.0f, ((i * 3) % 256) / 255.0f, ((i * 7) % 256) / 255.0f, 1.0f };
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, col);
        IDXGISwapChain_Present(sc, 1, 0);
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            DispatchMessageA(&msg);
        }
    }
done:
    QueryPerformanceCounter(&f1);
    double secs = (double)(f1.QuadPart - f0.QuadPart) / (double)freq.QuadPart;
    printf("OK d3d11 %d frames in %.2fs = %.1f fps\n", frames, secs, frames / (secs ? secs : 1.0));
    if (rtv) ID3D11RenderTargetView_Release(rtv);
    if (bb) ID3D11Texture2D_Release(bb);
    if (sc) IDXGISwapChain_Release(sc);
    if (ctx) ID3D11DeviceContext_Release(ctx);
    if (dev) ID3D11Device_Release(dev);
    return 0;
}
