/* d3d8_smoke.c — D3D8 device creation + clear/present loop (DXVK smoke, rides d3d9 lane)
 * build (mingw): x86_64-w64-mingw32-gcc -O2 d3d8_smoke.c -o d3d8_smoke.exe -ld3d8 -lgdi32 -luser32
 * usage: d3d8_smoke.exe [frames]
 */
#define COBJMACROS
#define COBJMACROS
#include <windows.h>
#include <d3d8.h>
#include <stdio.h>

typedef void *(*Direct3DCreate8_t)(unsigned int sdk);

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

int main(int argc, char **argv) {
    int frames = (argc > 1) ? atoi(argv[1]) : 300;
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "d3d8smoke";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("d3d8smoke", "d3d8 smoke", WS_OVERLAPPEDWINDOW,
                              40, 40, 320, 240, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) { printf("FAIL CreateWindow\n"); return 1; }
    ShowWindow(hwnd, SW_SHOW);

    /* d3d8.dll is a Wine/DXVK builtin — resolve Direct3DCreate8 dynamically */
    HMODULE d3d8mod = LoadLibraryA("d3d8.dll");
    if (!d3d8mod) { printf("FAIL LoadLibrary(d3d8.dll)\n"); return 1; }
    Direct3DCreate8_t pDirect3DCreate8 = (Direct3DCreate8_t)GetProcAddress(d3d8mod, "Direct3DCreate8");
    if (!pDirect3DCreate8) { printf("FAIL GetProcAddress(Direct3DCreate8)\n"); return 1; }
    IDirect3D8 *d3d = (IDirect3D8 *)pDirect3DCreate8(220 /* D3D_SDK_VERSION */);
    if (!d3d) { printf("FAIL Direct3DCreate8\n"); return 1; }

    D3DPRESENT_PARAMETERS pp = {0};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.hDeviceWindow = hwnd;

    IDirect3DDevice8 *dev = NULL;
    HRESULT hr = IDirect3D8_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                         D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) {
        hr = IDirect3D8_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                     D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    }
    if (FAILED(hr)) { printf("FAIL CreateDevice hr=0x%08lx\n", (unsigned long)hr); return 1; }

    LARGE_INTEGER f0, f1, freq; QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&f0);
    for (int i = 0; i < frames; i++) {
        IDirect3DDevice8_Clear(dev, 0, NULL, D3DCLEAR_TARGET,
                               D3DCOLOR_XRGB((BYTE)(i * 5), (BYTE)(i * 9), (BYTE)(i * 2)), 1.0f, 0);
        IDirect3DDevice8_Present(dev, NULL, NULL, NULL, NULL);
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            DispatchMessageA(&msg);
        }
    }
done:
    QueryPerformanceCounter(&f1);
    double secs = (double)(f1.QuadPart - f0.QuadPart) / (double)freq.QuadPart;
    printf("OK d3d8 %d frames in %.2fs = %.1f fps\n", frames, secs, frames / (secs ? secs : 1.0));
    IDirect3DDevice8_Release(dev);
    IDirect3D8_Release(d3d);
    return 0;
}
