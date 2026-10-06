#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#pragma comment(lib, "d3d9.lib")

namespace {

using PresentFn = HRESULT (WINAPI*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using ResetFn   = HRESULT (WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

PresentFn g_originalPresent = nullptr;
ResetFn   g_originalReset = nullptr;
void** g_vtable = nullptr;
std::atomic<bool> g_installed{false};

struct Settings {
    bool enabled = true;
    float strength = 0.70f;
    float blend = 0.65f;
    float persistence = 0.80f;
    int quality = 2;
};

Settings g_cfg;

IDirect3DTexture9* g_current = nullptr;
IDirect3DTexture9* g_history = nullptr;
IDirect3DSurface9* g_backBuffer = nullptr;
UINT g_width = 0;
UINT g_height = 0;
D3DFORMAT g_format = D3DFMT_UNKNOWN;
bool g_historyValid = false;
bool g_inPresent = false;

struct Vertex {
    float x, y, z, rhw;
    float u, v;
};
constexpr DWORD FVF = D3DFVF_XYZRHW | D3DFVF_TEX1;

void SafeRelease(IUnknown*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

std::string PluginDirectory() {
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCSTR>(&PluginDirectory), &self);

    char path[MAX_PATH]{};
    GetModuleFileNameA(self, path, MAX_PATH);
    std::string s(path);
    const size_t slash = s.find_last_of("\\/");
    return slash == std::string::npos ? "." : s.substr(0, slash);
}

int ReadInt(const char* key, int def) {
    const std::string file = PluginDirectory() + "\\MotionBlur.ini";
    return GetPrivateProfileIntA("MotionBlur", key, def, file.c_str());
}

void LoadSettings() {
    g_cfg.enabled = ReadInt("Enabled", 1) != 0;
    g_cfg.strength = std::clamp(ReadInt("StrengthPercent", 70), 0, 100) / 100.0f;
    g_cfg.blend = std::clamp(ReadInt("BlendPercent", 65), 0, 95) / 100.0f;
    g_cfg.persistence = std::clamp(ReadInt("PersistencePercent", 80), 0, 100) / 100.0f;
    g_cfg.quality = std::clamp(ReadInt("Quality", 2), 1, 4);
}

void ReleaseResources() {
    SafeRelease(g_current);
    SafeRelease(g_history);
    SafeRelease(g_backBuffer);
    g_width = g_height = 0;
    g_format = D3DFMT_UNKNOWN;
    g_historyValid = false;
}

bool CreateResources(IDirect3DDevice9* device) {
    ReleaseResources();

    IDirect3DSurface9* bb = nullptr;
    if (FAILED(device->GetRenderTarget(0, &bb)) || !bb)
        return false;

    D3DSURFACE_DESC desc{};
    if (FAILED(bb->GetDesc(&desc))) {
        bb->Release();
        return false;
    }

    if (desc.Width < 2 || desc.Height < 2) {
        bb->Release();
        return false;
    }

    HRESULT hr = device->CreateTexture(
        desc.Width, desc.Height, 1,
        D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT,
        &g_current, nullptr);

    if (SUCCEEDED(hr)) {
        hr = device->CreateTexture(
            desc.Width, desc.Height, 1,
            D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT,
            &g_history, nullptr);
    }

    if (FAILED(hr)) {
        bb->Release();
        ReleaseResources();
        return false;
    }

    g_backBuffer = bb;
    g_width = desc.Width;
    g_height = desc.Height;
    g_format = desc.Format;
    return true;
}

bool EnsureResources(IDirect3DDevice9* device) {
    if (g_current && g_history && g_backBuffer)
        return true;
    return CreateResources(device);
}

void SetFixedFunctionState(IDirect3DDevice9* device) {
    device->SetFVF(FVF);

    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);

    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
}

void DrawTexture(IDirect3DDevice9* device, IDirect3DTexture9* texture, float alpha) {
    const float w = static_cast<float>(g_width);
    const float h = static_cast<float>(g_height);

    Vertex quad[4] = {
        {-0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f},
        {w - 0.5f, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f},
        {-0.5f, h - 0.5f, 0.0f, 1.0f, 0.0f, 1.0f},
        {w - 0.5f, h - 0.5f, 0.0f, 1.0f, 1.0f, 1.0f}
    };

    device->SetTexture(0, texture);

    if (alpha >= 0.999f) {
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    } else {
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    }

    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);

    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));
}

void RenderTemporalBlend(IDirect3DDevice9* device) {
    if (!g_cfg.enabled || !EnsureResources(device))
        return;

    IDirect3DSurface9* currentSurface = nullptr;
    IDirect3DSurface9* historySurface = nullptr;

    if (FAILED(g_current->GetSurfaceLevel(0, &currentSurface)) ||
        FAILED(g_history->GetSurfaceLevel(0, &historySurface))) {
        SafeRelease(currentSurface);
        SafeRelease(historySurface);
        return;
    }

    if (FAILED(device->StretchRect(g_backBuffer, nullptr,
                                   currentSurface, nullptr, D3DTEXF_NONE))) {
        SafeRelease(currentSurface);
        SafeRelease(historySurface);
        return;
    }

    if (!g_historyValid) {
        device->StretchRect(currentSurface, nullptr, historySurface, nullptr, D3DTEXF_NONE);
        g_historyValid = true;
        SafeRelease(currentSurface);
        SafeRelease(historySurface);
        return;
    }

    device->SetRenderTarget(0, g_backBuffer);
    SetFixedFunctionState(device);

    DrawTexture(device, g_current, 1.0f);

    float historyAlpha = std::clamp(
        g_cfg.blend * g_cfg.persistence * g_cfg.strength, 0.0f, 0.92f);

    if (g_cfg.quality >= 3)
        historyAlpha = std::clamp(historyAlpha + 0.05f, 0.0f, 0.94f);
    if (g_cfg.quality >= 4)
        historyAlpha = std::clamp(historyAlpha + 0.04f, 0.0f, 0.95f);

    DrawTexture(device, g_history, historyAlpha);

    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetTexture(0, nullptr);

    device->StretchRect(g_backBuffer, nullptr,
                        historySurface, nullptr, D3DTEXF_NONE);

    SafeRelease(currentSurface);
    SafeRelease(historySurface);
}

HRESULT WINAPI HookReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pp) {
    ReleaseResources();
    const HRESULT hr = g_originalReset(device, pp);
    if (SUCCEEDED(hr))
        CreateResources(device);
    return hr;
}

HRESULT WINAPI HookPresent(IDirect3DDevice9* device,
                           const RECT* src,
                           const RECT* dst,
                           HWND wnd,
                           const RGNDATA* dirty) {
    if (!g_inPresent) {
        g_inPresent = true;
        LoadSettings();
        RenderTemporalBlend(device);
        g_inPresent = false;
    }

    return g_originalPresent(device, src, dst, wnd, dirty);
}

bool PatchVTable(void** vtable, size_t index, void* replacement, void** original) {
    if (!vtable || !replacement || !original)
        return false;

    DWORD oldProtect = 0;
    if (!VirtualProtect(&vtable[index], sizeof(void*),
                        PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;

    *original = vtable[index];
    vtable[index] = replacement;

    DWORD ignored = 0;
    VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), &vtable[index], sizeof(void*));
    return true;
}

LRESULT CALLBACK DummyWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProcA(h, m, w, l);
}

bool GetD3D9VTable(void*** outVtable) {
    if (!outVtable)
        return false;

    HMODULE d3d9 = LoadLibraryA("d3d9.dll");
    if (!d3d9)
        return false;

    WNDCLASSEXA wc{sizeof(wc)};
    wc.lpfnWndProc = DummyWndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "GTA_SA_MotionBlur_Dummy";
    RegisterClassExA(&wc);

    HWND hwnd = CreateWindowExA(
        0, wc.lpszClassName, "MotionBlur", WS_OVERLAPPEDWINDOW,
        0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);

    if (!hwnd)
        return false;

    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) {
        DestroyWindow(hwnd);
        return false;
    }

    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hwnd;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.BackBufferWidth = 64;
    pp.BackBufferHeight = 64;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    IDirect3DDevice9* device = nullptr;
    HRESULT hr = d3d->CreateDevice(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING,
        &pp, &device);

    if (FAILED(hr) || !device) {
        d3d->Release();
        DestroyWindow(hwnd);
        return false;
    }

    *outVtable = *reinterpret_cast<void***>(device);

    device->Release();
    d3d->Release();
    DestroyWindow(hwnd);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);
    return true;
}

DWORD WINAPI InstallThread(LPVOID) {
    while (!GetModuleHandleA("d3d9.dll"))
        Sleep(100);

    void** vtable = nullptr;
    if (!GetD3D9VTable(&vtable))
        return 0;

    g_vtable = vtable;

    if (!PatchVTable(g_vtable, 16,
                     reinterpret_cast<void*>(&HookReset),
                     reinterpret_cast<void**>(&g_originalReset)))
        return 0;

    if (!PatchVTable(g_vtable, 17,
                     reinterpret_cast<void*>(&HookPresent),
                     reinterpret_cast<void**>(&g_originalPresent))) {
        return 0;
    }

    g_installed = true;
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, InstallThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
