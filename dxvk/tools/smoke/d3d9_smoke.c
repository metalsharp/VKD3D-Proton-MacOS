/* d3d9_smoke.c — D3D9 device creation + clear/present loop (DXVK smoke)
 * build (mingw): x86_64-w64-mingw32-gcc -O2 d3d9_smoke.c -o d3d9_smoke.exe -ld3d9 -lgdi32 -luser32
 * usage: d3d9_smoke.exe [frames]
 */
#include <windows.h>
#include <d3d9.h>
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
    wc.lpszClassName = "d3d9smoke";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("d3d9smoke", "d3d9 smoke", WS_OVERLAPPEDWINDOW,
                              40, 40, 320, 240, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) { printf("FAIL CreateWindow\n"); return 1; }
    ShowWindow(hwnd, SW_SHOW);

    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { printf("FAIL Direct3DCreate9\n"); return 1; }

    D3DPRESENT_PARAMETERS pp = {0};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.hDeviceWindow = hwnd;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;

    IDirect3DDevice9 *dev = NULL;
    HRESULT hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                         D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) {
        hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                     D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    }
    if (FAILED(hr)) { printf("FAIL CreateDevice hr=0x%08lx\n", (unsigned long)hr); return 1; }

    LARGE_INTEGER f0, f1, freq; QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&f0);
    for (int i = 0; i < frames; i++) {
        BYTE r = (BYTE)(i * 7), g = (BYTE)(i * 3), b = (BYTE)(i * 11);
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET,
                               D3DCOLOR_XRGB(r, g, b), 1.0f, 0);
        IDirect3DDevice9_BeginScene(dev);
        IDirect3DDevice9_EndScene(dev);
        IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            DispatchMessageA(&msg);
        }
    }
done:
    QueryPerformanceCounter(&f1);
    double secs = (double)(f1.QuadPart - f0.QuadPart) / (double)freq.QuadPart;
    printf("OK d3d9 %d frames in %.2fs = %.1f fps\n", frames, secs, frames / (secs ? secs : 1.0));
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    return 0;
}
