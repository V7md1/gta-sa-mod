#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
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
IDirect3DStateBlock9* g_stateBlock = nullptr;
UINT g_width = 0;
UINT g_height = 0;
D3DFORMAT g_format = D3DFMT_UNKNOWN;
bool g_historyValid = false;
bool g_inPresent = false;
IDirect3DDevice9* g_device = nullptr;

struct Vertex {
    float x, y, z, rhw;
    float u, v;
};

constexpr DWORD FVF = D3DFVF_XYZRHW | D3DFVF_TEX1;

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
    SafeRelease(g_stateBlock);
    SafeRelease(g_current);
    SafeRelease(g_history);
    g_width = g_height = 0;
    g_format = D3DFMT_UNKNOWN;
    g_historyValid = false;
    g_device = nullptr;
}

bool CreateResources(IDirect3DDevice9* device) {
    if (!device)
        return false;

    IDirect3DSurface9* backBuffer = nullptr;
    if (FAILED(device->GetRenderTarget(0, &backBuffer)) || !backBuffer)
        return false;

    D3DSURFACE_DESC desc{};
    const HRESULT descHr = backBuffer->GetDesc(&desc);
    backBuffer->Release();

    if (FAILED(descHr) || desc.Width < 2 || desc.Height < 2)
        return false;

    IDirect3DTexture9* current = nullptr;
    IDirect3DTexture9* history = nullptr;
    IDirect3DStateBlock9* stateBlock = nullptr;

    HRESULT hr = device->CreateTexture(
        desc.Width, desc.Height, 1,
        D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT,
        &current, nullptr);

    if (SUCCEEDED(hr)) {
        hr = device->CreateTexture(
            desc.Width, desc.Height, 1,
            D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT,
            &history, nullptr);
    }

    if (SUCCEEDED(hr))
        hr = device->CreateStateBlock(D3DSBT_ALL, &stateBlock);

    if (FAILED(hr)) {
        SafeRelease(current);
        SafeRelease(history);
        SafeRelease(stateBlock);
        return false;
    }

    ReleaseResources();

    g_current = current;
    g_history = history;
    g_stateBlock = stateBlock;
    g_width = desc.Width;
    g_height = desc.Height;
    g_format = desc.Format;
    g_device = device;
    return true;
}

bool EnsureResources(IDirect3DDevice9* device) {
    if (!device)
        return false;

    if (g_device == device && g_current && g_history && g_stateBlock &&
        g_width >= 2 && g_height >= 2)
        return true;

    return CreateResources(device);
}

void ConfigureFullscreenDraw(IDirect3DDevice9* device) {
    device->SetFVF(FVF);
    device->SetVertexShader(nullptr);
    device->SetPixelShader(nullptr);

    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(D3DRS_CLIPPING, FALSE);
    device->SetRenderState(D3DRS_DITHERENABLE, FALSE);

    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);
}

void DrawTexture(IDirect3DDevice9* device, IDirect3DTexture9* texture, float alpha) {
    if (!texture || g_width < 2 || g_height < 2)
        return;

    const float w = static_cast<float>(g_width);
    const float h = static_cast<float>(g_height);

    Vertex quad[4] = {
        {-0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f},
        {w - 0.5f, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f},
        {-0.5f, h - 0.5f, 0.0f, 1.0f, 0.0f, 1.0f},
        {w - 0.5f, h - 0.5f, 0.0f, 1.0f, 1.0f, 1.0f}
    };

    alpha = std::clamp(alpha, 0.0f, 1.0f);
    const DWORD a = static_cast<DWORD>(alpha * 255.0f + 0.5f);
    const DWORD factor = (a << 24) | (a << 16) | (a << 8) | a;

    device->SetTexture(0, texture);
    device->SetRenderState(D3DRS_TEXTUREFACTOR, factor);

    if (alpha >= 0.999f) {
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    } else {
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    }

    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));
}

void RenderTemporalBlend(IDirect3DDevice9* device) {
    if (!g_cfg.enabled || !EnsureResources(device))
        return;

    IDirect3DSurface9* backBuffer = nullptr;
    IDirect3DSurface9* currentSurface = nullptr;
    IDirect3DSurface9* historySurface = nullptr;

    if (FAILED(device->GetRenderTarget(0, &backBuffer)) || !backBuffer)
        return;

    if (FAILED(g_current->GetSurfaceLevel(0, &currentSurface)) ||
        FAILED(g_history->GetSurfaceLevel(0, &historySurface))) {
        SafeRelease(backBuffer);
        SafeRelease(currentSurface);
        SafeRelease(historySurface);
        return;
    }

    D3DSURFACE_DESC backDesc{};
    if (FAILED(backBuffer->GetDesc(&backDesc)) ||
        backDesc.Width != g_width ||
        backDesc.Height != g_height ||
        backDesc.Format != g_format) {
        SafeRelease(backBuffer);
        SafeRelease(currentSurface);
        SafeRelease(historySurface);
        ReleaseResources();
        return;
    }

    // Save every D3D state that the effect changes. This prevents the HUD/game
    // renderer from inheriting our blend, texture, FVF, depth, or sampler state.
    if (FAILED(g_stateBlock->Capture())) {
        SafeRelease(backBuffer);
        SafeRelease(currentSurface);
        SafeRelease(historySurface);
        return;
    }

    // Capture the frame exactly as GTA left it.
    if (FAILED(device->StretchRect(
            backBuffer, nullptr, currentSurface, nullptr, D3DTEXF_NONE))) {
        g_stateBlock->Apply();
        SafeRelease(backBuffer);
        SafeRelease(currentSurface);
        SafeRelease(historySurface);
        return;
    }

    if (!g_historyValid) {
        if (SUCCEEDED(device->StretchRect(
                currentSurface, nullptr, historySurface, nullptr, D3DTEXF_NONE))) {
            g_historyValid = true;
        }

        g_stateBlock->Apply();
        SafeRelease(backBuffer);
        SafeRelease(currentSurface);
        SafeRelease(historySurface);
        return;
    }

    // Always render back into the actual current render target.
    if (FAILED(device->SetRenderTarget(0, backBuffer))) {
        g_stateBlock->Apply();
        SafeRelease(backBuffer);
        SafeRelease(currentSurface);
        SafeRelease(historySurface);
        return;
    }

    D3DVIEWPORT9 vp{};
    if (SUCCEEDED(device->GetViewport(&vp))) {
        vp.X = 0;
        vp.Y = 0;
        vp.Width = g_width;
        vp.Height = g_height;
        vp.MinZ = 0.0f;
        vp.MaxZ = 1.0f;
        device->SetViewport(&vp);
    }

    ConfigureFullscreenDraw(device);

    // Current frame is fully opaque.
    DrawTexture(device, g_current, 1.0f);

    float historyAlpha =
        g_cfg.blend * g_cfg.persistence * g_cfg.strength;

    // Quality controls persistence slightly without adding expensive passes.
    if (g_cfg.quality >= 3)
        historyAlpha += 0.04f;
    if (g_cfg.quality >= 4)
        historyAlpha += 0.04f;

    historyAlpha = std::clamp(historyAlpha, 0.0f, 0.85f);

    // Previous frame is composited over the current frame.
    DrawTexture(device, g_history, historyAlpha);

    // The completed temporal frame becomes history for the next frame.
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetTexture(0, nullptr);

    if (FAILED(device->StretchRect(
            backBuffer, nullptr, historySurface, nullptr, D3DTEXF_NONE))) {
        g_historyValid = false;
    }

    // Restore GTA's exact D3D state before returning to its renderer/present path.
    g_stateBlock->Apply();

    SafeRelease(backBuffer);
    SafeRelease(currentSurface);
    SafeRelease(historySurface);
}

HRESULT WINAPI HookReset(IDirect3DDevice9* device,
                         D3DPRESENT_PARAMETERS* pp) {
    // D3DPOOL_DEFAULT resources must be released before Reset.
    // Re-create them only after a successful reset.
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

        // Do not touch the device if the original hook is not ready.
        if (g_originalPresent)
            RenderTemporalBlend(device);

        g_inPresent = false;
    }

    return g_originalPresent(device, src, dst, wnd, dirty);
}

bool PatchVTable(void** vtable, size_t index,
                 void* replacement, void** original) {
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
        0, wc.lpszClassName, "MotionBlur",
        WS_OVERLAPPEDWINDOW, 0, 0, 64, 64,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!hwnd) {
        UnregisterClassA(wc.lpszClassName, wc.hInstance);
        return false;
    }

    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) {
        DestroyWindow(hwnd);
        UnregisterClassA(wc.lpszClassName, wc.hInstance);
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
    const HRESULT hr = d3d->CreateDevice(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING,
        &pp, &device);

    if (FAILED(hr) || !device) {
        d3d->Release();
        DestroyWindow(hwnd);
        UnregisterClassA(wc.lpszClassName, wc.hInstance);
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

    // IDirect3DDevice9 vtable:
    // Reset = 16, Present = 17.
    if (!PatchVTable(g_vtable, 16,
                     reinterpret_cast<void*>(&HookReset),
                     reinterpret_cast<void**>(&g_originalReset)))
        return 0;

    if (!PatchVTable(g_vtable, 17,
                     reinterpret_cast<void*>(&HookPresent),
                     reinterpret_cast<void**>(&g_originalPresent))) {
        // If Present could not be patched, restore Reset immediately.
        DWORD oldProtect = 0;
        if (VirtualProtect(&g_vtable[16], sizeof(void*),
                           PAGE_EXECUTE_READWRITE, &oldProtect)) {
            g_vtable[16] = reinterpret_cast<void*>(g_originalReset);
            DWORD ignored = 0;
            VirtualProtect(&g_vtable[16], sizeof(void*), oldProtect, &ignored);
        }
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
