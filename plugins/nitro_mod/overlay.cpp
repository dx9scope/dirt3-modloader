// Minimal D3D9 / D3D11 overlay: hooks Present, lets the plugin fill a triangle list, draws it on top.
//
// No hard link dependency on d3d9/d3d11/d3dcompiler: everything is resolved with LoadLibrary/GetProcAddress.
// The Present entry points are located by creating a throw-away device (the vtable is shared by every
// device in the process), then detoured through the host's MinHook (ModloaderAPI::CreateHook).
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d9.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include "overlay.hpp"

namespace overlay {
namespace {

const ModloaderAPI* g_Api = nullptr;
DrawCallback g_Callback = nullptr;
std::atomic<int> g_Backend{ 0 };          // 0 none, 9 = DX9 drew a frame, 11 = DX11 drew a frame
std::atomic<bool> g_Faulted{ false };      // set if our drawing code ever crashes -> overlay disables itself
std::atomic<bool> g_Stop{ false };
LARGE_INTEGER g_Freq{}, g_LastTick{};

void Log(const char* fmt, ...) {
    if (!g_Api) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_Api->LogInfo("NitroOverlay", buf);
}

float FrameDt() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = 0.016f;
    if (g_LastTick.QuadPart != 0 && g_Freq.QuadPart != 0) {
        dt = static_cast<float>(now.QuadPart - g_LastTick.QuadPart) / static_cast<float>(g_Freq.QuadPart);
    }
    g_LastTick = now;
    if (dt < 0.0001f) dt = 0.0001f;
    if (dt > 0.1f) dt = 0.1f;
    return dt;
}

HWND CreateDummyWindow() {
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "NitroOverlayDummyWnd";
    RegisterClassExA(&wc);
    return CreateWindowExA(0, wc.lpszClassName, "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64,
                           nullptr, nullptr, wc.hInstance, nullptr);
}

// ============================================================================
// Direct3D 9 backend
// ============================================================================
typedef HRESULT(WINAPI* PFN_Present9)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT(WINAPI* PFN_CreateDevice9)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                           D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
PFN_Present9 g_OrigPresent9 = nullptr;
PFN_CreateDevice9 g_OrigCreateDevice9 = nullptr;
bool g_Present9Hooked = false;
DrawList g_List9;

struct Vtx9 {
    float x, y, z, rhw;
    DWORD color;
};

void DrawOverlay9(IDirect3DDevice9* dev) {
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    D3DSURFACE_DESC desc{};
    bb->GetDesc(&desc);
    if (desc.Width == 0 || desc.Height == 0) { bb->Release(); return; }

    g_List9.Clear();
    g_Callback(g_List9, static_cast<float>(desc.Width), static_cast<float>(desc.Height), FrameDt());
    if (g_List9.verts.empty()) { bb->Release(); return; }

    // Snapshot everything we are about to touch and restore it afterwards.
    IDirect3DStateBlock9* sb = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb)) || !sb) { bb->Release(); return; }

    IDirect3DSurface9* oldRt = nullptr;
    dev->GetRenderTarget(0, &oldRt);
    dev->SetRenderTarget(0, bb);

    D3DVIEWPORT9 vp{ 0, 0, desc.Width, desc.Height, 0.0f, 1.0f };
    dev->SetViewport(&vp);

    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    dev->SetTexture(0, nullptr);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

    static std::vector<Vtx9> tmp;
    tmp.resize(g_List9.verts.size());
    for (size_t i = 0; i < tmp.size(); ++i) {
        const Vertex& v = g_List9.verts[i];
        tmp[i] = { v.x - 0.5f, v.y - 0.5f, 0.0f, 1.0f, v.color };
    }

    if (SUCCEEDED(dev->BeginScene())) {
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(tmp.size() / 3), tmp.data(), sizeof(Vtx9));
        dev->EndScene();
    }

    sb->Apply();
    sb->Release();
    if (oldRt) { dev->SetRenderTarget(0, oldRt); oldRt->Release(); }
    bb->Release();
}

void SafeDraw9(IDirect3DDevice9* dev) {
    __try {
        DrawOverlay9(dev);
        g_Backend.store(9);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_Faulted.store(true);
    }
}

HRESULT WINAPI Hook_Present9(IDirect3DDevice9* dev, const RECT* a, const RECT* b, HWND c, const RGNDATA* d) {
    if (!g_Faulted.load() && g_Callback) SafeDraw9(dev);
    return g_OrigPresent9(dev, a, b, c, d);
}

bool HookPresent9FromDevice(void* devVtblPresent, const char* how) {
    if (g_Present9Hooked) return true;
    if (!g_Api->CreateHook("Overlay::D3D9Present", reinterpret_cast<uintptr_t>(devVtblPresent),
                           reinterpret_cast<void*>(Hook_Present9), reinterpret_cast<void**>(&g_OrigPresent9))) {
        Log("Failed to hook IDirect3DDevice9::Present (%s)", how);
        return false;
    }
    g_Present9Hooked = true;
    Log("Hooked IDirect3DDevice9::Present via %s (target %p)", how, devVtblPresent);
    return true;
}

HRESULT WINAPI Hook_CreateDevice9(IDirect3D9* self, UINT adapter, D3DDEVTYPE type, HWND wnd, DWORD flags,
                                  D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    HRESULT hr = g_OrigCreateDevice9(self, adapter, type, wnd, flags, pp, out);
    if (SUCCEEDED(hr) && out && *out && !g_Present9Hooked) {
        void** vt = *reinterpret_cast<void***>(*out);
        HookPresent9FromDevice(vt[17], "game CreateDevice");
    }
    return hr;
}

bool InstallD3D9() {
    HMODULE mod = GetModuleHandleA("d3d9.dll");
    if (!mod) return false;
    typedef IDirect3D9*(WINAPI * PFN_Create9)(UINT);
    auto create = reinterpret_cast<PFN_Create9>(GetProcAddress(mod, "Direct3DCreate9"));
    if (!create) return false;

    IDirect3D9* d3d = create(D3D_SDK_VERSION);
    if (!d3d) return false;

    HWND wnd = CreateDummyWindow();
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.hDeviceWindow = wnd;

    IDirect3DDevice9* dummy = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd,
                                   D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_NOWINDOWCHANGES, &pp, &dummy);
    bool ok = false;
    if (SUCCEEDED(hr) && dummy) {
        void** vt = *reinterpret_cast<void***>(dummy);
        ok = HookPresent9FromDevice(vt[16 + 1], "dummy device");
        dummy->Release();
    } else {
        Log("Dummy D3D9 device failed (0x%08X) - falling back to IDirect3D9::CreateDevice hook", static_cast<unsigned>(hr));
        void** vt = *reinterpret_cast<void***>(d3d);
        ok = g_Api->CreateHook("Overlay::D3D9CreateDevice", reinterpret_cast<uintptr_t>(vt[16]),
                               reinterpret_cast<void*>(Hook_CreateDevice9),
                               reinterpret_cast<void**>(&g_OrigCreateDevice9));
    }
    d3d->Release();
    if (wnd) DestroyWindow(wnd);
    return ok;
}

// ============================================================================
// Direct3D 11 backend
// ============================================================================
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present11)(IDXGISwapChain*, UINT, UINT);
PFN_Present11 g_OrigPresent11 = nullptr;
bool g_Present11Hooked = false;
DrawList g_List11;

struct Dx11Res {
    ID3D11Device* dev = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11InputLayout* il = nullptr;
    ID3D11Buffer* vb = nullptr;
    ID3D11Buffer* cb = nullptr;
    ID3D11BlendState* bs = nullptr;
    ID3D11RasterizerState* rs = nullptr;
    ID3D11DepthStencilState* ds = nullptr;
    UINT vbCapacity = 0; // vertices
};
Dx11Res g_R11;

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

const char* kHlsl =
    "cbuffer cb : register(b0) { float2 screen; float2 pad; };\n"
    "struct VSIn { float2 pos : POSITION; float4 col : COLOR; };\n"
    "struct PSIn { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
    "PSIn vs_main(VSIn i) { PSIn o; o.pos = float4(i.pos.x / screen.x * 2.0 - 1.0, 1.0 - i.pos.y / screen.y * 2.0, 0.0, 1.0); o.col = i.col; return o; }\n"
    "float4 ps_main(PSIn i) : SV_Target { return i.col; }\n";

void ReleaseRes11() {
    SafeRelease(g_R11.vs); SafeRelease(g_R11.ps); SafeRelease(g_R11.il); SafeRelease(g_R11.vb);
    SafeRelease(g_R11.cb); SafeRelease(g_R11.bs); SafeRelease(g_R11.rs); SafeRelease(g_R11.ds);
    g_R11.dev = nullptr;
    g_R11.vbCapacity = 0;
}

bool EnsureRes11(ID3D11Device* dev) {
    if (g_R11.dev == dev && g_R11.vs) return true;
    if (g_R11.dev) ReleaseRes11();

    HMODULE comp = nullptr;
    const char* names[] = { "d3dcompiler_47.dll", "d3dcompiler_46.dll", "d3dcompiler_43.dll" };
    for (const char* n : names) { comp = LoadLibraryA(n); if (comp) break; }
    if (!comp) { Log("No d3dcompiler dll found - DX11 overlay unavailable"); g_Faulted.store(true); return false; }
    auto compile = reinterpret_cast<pD3DCompile>(GetProcAddress(comp, "D3DCompile"));
    if (!compile) { g_Faulted.store(true); return false; }

    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    HRESULT hr = compile(kHlsl, strlen(kHlsl), "nitro_overlay", nullptr, nullptr, "vs_main", "vs_4_0", 0, 0, &vsb, &err);
    if (FAILED(hr)) { Log("VS compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?"); SafeRelease(err); g_Faulted.store(true); return false; }
    hr = compile(kHlsl, strlen(kHlsl), "nitro_overlay", nullptr, nullptr, "ps_main", "ps_4_0", 0, 0, &psb, &err);
    if (FAILED(hr)) { Log("PS compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?"); SafeRelease(err); SafeRelease(vsb); g_Faulted.store(true); return false; }

    bool ok = SUCCEEDED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_R11.vs)) &&
              SUCCEEDED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_R11.ps));
    if (ok) {
        D3D11_INPUT_ELEMENT_DESC el[2] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        ok = SUCCEEDED(dev->CreateInputLayout(el, 2, vsb->GetBufferPointer(), vsb->GetBufferSize(), &g_R11.il));
    }
    SafeRelease(vsb); SafeRelease(psb);

    if (ok) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 16;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ok = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g_R11.cb));
    }
    if (ok) {
        g_R11.vbCapacity = 16384;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = g_R11.vbCapacity * sizeof(Vertex);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ok = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g_R11.vb));
    }
    if (ok) {
        D3D11_BLEND_DESC bl{};
        bl.RenderTarget[0].BlendEnable = TRUE;
        bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        ok = SUCCEEDED(dev->CreateBlendState(&bl, &g_R11.bs));
    }
    if (ok) {
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = FALSE;
        ok = SUCCEEDED(dev->CreateRasterizerState(&rd, &g_R11.rs));
    }
    if (ok) {
        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = FALSE;
        dd.StencilEnable = FALSE;
        ok = SUCCEEDED(dev->CreateDepthStencilState(&dd, &g_R11.ds));
    }
    if (!ok) { Log("DX11 overlay resource creation failed"); ReleaseRes11(); g_Faulted.store(true); return false; }
    g_R11.dev = dev;
    Log("DX11 overlay resources created");
    return true;
}

DXGI_FORMAT ToUnormView(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    default: return f;
    }
}

void DrawOverlay11(IDXGISwapChain* sc) {
    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) || !dev) return;
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    if (!ctx) { dev->Release(); return; }

    ID3D11Texture2D* bb = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    do {
        if (!EnsureRes11(dev)) break;
        if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) || !bb) break;
        D3D11_TEXTURE2D_DESC td{};
        bb->GetDesc(&td);
        if (td.Width == 0 || td.Height == 0) break;

        g_List11.Clear();
        g_Callback(g_List11, static_cast<float>(td.Width), static_cast<float>(td.Height), FrameDt());
        UINT count = static_cast<UINT>(g_List11.verts.size());
        if (count == 0) break;
        if (count > g_R11.vbCapacity) count = (g_R11.vbCapacity / 3) * 3;

        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = ToUnormView(td.Format);
        rd.ViewDimension = (td.SampleDesc.Count > 1) ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(dev->CreateRenderTargetView(bb, &rd, &rtv)) || !rtv) break;

        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(g_R11.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            float* f = static_cast<float*>(m.pData);
            f[0] = static_cast<float>(td.Width); f[1] = static_cast<float>(td.Height); f[2] = f[3] = 0.0f;
            ctx->Unmap(g_R11.cb, 0);
        }
        if (FAILED(ctx->Map(g_R11.vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) break;
        memcpy(m.pData, g_List11.verts.data(), count * sizeof(Vertex));
        ctx->Unmap(g_R11.vb, 0);

        // ---- save state ----
        ID3D11RenderTargetView* oldRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        ID3D11DepthStencilView* oldDsv = nullptr;
        ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, &oldDsv);
        ID3D11BlendState* oldBs = nullptr; FLOAT oldBf[4] = {}; UINT oldMask = 0;
        ctx->OMGetBlendState(&oldBs, oldBf, &oldMask);
        ID3D11DepthStencilState* oldDs = nullptr; UINT oldRef = 0;
        ctx->OMGetDepthStencilState(&oldDs, &oldRef);
        ID3D11RasterizerState* oldRs = nullptr;
        ctx->RSGetState(&oldRs);
        D3D11_VIEWPORT oldVp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]; UINT nVp = ARRAYSIZE(oldVp);
        ctx->RSGetViewports(&nVp, oldVp);
        ID3D11InputLayout* oldIl = nullptr; ctx->IAGetInputLayout(&oldIl);
        D3D11_PRIMITIVE_TOPOLOGY oldTopo; ctx->IAGetPrimitiveTopology(&oldTopo);
        ID3D11Buffer* oldVb = nullptr; UINT oldStride = 0, oldOff = 0;
        ctx->IAGetVertexBuffers(0, 1, &oldVb, &oldStride, &oldOff);
        ID3D11VertexShader* oldVs = nullptr; ctx->VSGetShader(&oldVs, nullptr, nullptr);
        ID3D11PixelShader* oldPs = nullptr; ctx->PSGetShader(&oldPs, nullptr, nullptr);
        ID3D11GeometryShader* oldGs = nullptr; ctx->GSGetShader(&oldGs, nullptr, nullptr);
        ID3D11HullShader* oldHs = nullptr; ctx->HSGetShader(&oldHs, nullptr, nullptr);
        ID3D11DomainShader* oldDomain = nullptr; ctx->DSGetShader(&oldDomain, nullptr, nullptr);
        ID3D11Buffer* oldCb = nullptr; ctx->VSGetConstantBuffers(0, 1, &oldCb);

        // ---- draw ----
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->OMSetBlendState(g_R11.bs, nullptr, 0xFFFFFFFF);
        ctx->OMSetDepthStencilState(g_R11.ds, 0);
        ctx->RSSetState(g_R11.rs);
        D3D11_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(td.Width), static_cast<float>(td.Height), 0.0f, 1.0f };
        ctx->RSSetViewports(1, &vp);
        ctx->IASetInputLayout(g_R11.il);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        UINT stride = sizeof(Vertex), off = 0;
        ctx->IASetVertexBuffers(0, 1, &g_R11.vb, &stride, &off);
        ctx->VSSetShader(g_R11.vs, nullptr, 0);
        ctx->PSSetShader(g_R11.ps, nullptr, 0);
        ctx->GSSetShader(nullptr, nullptr, 0);
        ctx->HSSetShader(nullptr, nullptr, 0);
        ctx->DSSetShader(nullptr, nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, &g_R11.cb);
        ctx->Draw(count, 0);

        // ---- restore state ----
        ctx->VSSetConstantBuffers(0, 1, &oldCb);
        ctx->DSSetShader(oldDomain, nullptr, 0);
        ctx->HSSetShader(oldHs, nullptr, 0);
        ctx->GSSetShader(oldGs, nullptr, 0);
        ctx->PSSetShader(oldPs, nullptr, 0);
        ctx->VSSetShader(oldVs, nullptr, 0);
        ctx->IASetVertexBuffers(0, 1, &oldVb, &oldStride, &oldOff);
        ctx->IASetPrimitiveTopology(oldTopo);
        ctx->IASetInputLayout(oldIl);
        ctx->RSSetViewports(nVp, oldVp);
        ctx->RSSetState(oldRs);
        ctx->OMSetDepthStencilState(oldDs, oldRef);
        ctx->OMSetBlendState(oldBs, oldBf, oldMask);
        ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, oldDsv);

        SafeRelease(oldCb); SafeRelease(oldDomain); SafeRelease(oldHs); SafeRelease(oldGs);
        SafeRelease(oldPs); SafeRelease(oldVs); SafeRelease(oldVb); SafeRelease(oldIl);
        SafeRelease(oldRs); SafeRelease(oldDs); SafeRelease(oldBs); SafeRelease(oldDsv);
        for (auto& r : oldRtv) SafeRelease(r);
    } while (false);

    SafeRelease(rtv);
    SafeRelease(bb);
    ctx->Release();
    dev->Release();
}

void SafeDraw11(IDXGISwapChain* sc) {
    __try {
        DrawOverlay11(sc);
        g_Backend.store(11);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        g_Faulted.store(true);
    }
}

HRESULT STDMETHODCALLTYPE Hook_Present11(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!(flags & DXGI_PRESENT_TEST) && !g_Faulted.load() && g_Callback) SafeDraw11(sc);
    return g_OrigPresent11(sc, sync, flags);
}

bool InstallD3D11() {
    HMODULE mod = GetModuleHandleA("d3d11.dll");
    if (!mod) return false;
    typedef HRESULT(WINAPI * PFN_Create)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT,
                                         UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**,
                                         D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
    auto create = reinterpret_cast<PFN_Create>(GetProcAddress(mod, "D3D11CreateDeviceAndSwapChain"));
    if (!create) return false;

    HWND wnd = CreateDummyWindow();
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 64;
    sd.BufferDesc.Height = 64;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = wnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl{};
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, ARRAYSIZE(levels),
                        D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
    bool ok = false;
    if (SUCCEEDED(hr) && sc) {
        void** vt = *reinterpret_cast<void***>(sc);
        ok = g_Api->CreateHook("Overlay::D3D11Present", reinterpret_cast<uintptr_t>(vt[8]),
                               reinterpret_cast<void*>(Hook_Present11), reinterpret_cast<void**>(&g_OrigPresent11));
        if (ok) { g_Present11Hooked = true; Log("Hooked IDXGISwapChain::Present (DX11, target %p)", vt[8]); }
    } else {
        Log("Dummy D3D11 device failed (0x%08X)", static_cast<unsigned>(hr));
    }
    SafeRelease(ctx); SafeRelease(dev); SafeRelease(sc);
    if (wnd) DestroyWindow(wnd);
    return ok;
}

// ============================================================================
// Installer thread
// ============================================================================
DWORD WINAPI InstallThread(LPVOID) {
    // d3d9.dll is a static import of the game, so it is already there; d3d11.dll only appears if the game
    // is running with the DX11 renderer, so give it time and keep looking.
    for (int i = 0; i < 600 && !g_Stop.load(); ++i) {           // up to ~60 s
        if (!g_Present9Hooked && GetModuleHandleA("d3d9.dll")) InstallD3D9();
        if (!g_Present11Hooked && GetModuleHandleA("d3d11.dll") && GetModuleHandleA("dxgi.dll")) InstallD3D11();
        if (g_Present9Hooked && (g_Present11Hooked || i > 100)) break;
        Sleep(100);
    }
    Log("Overlay install finished: DX9 hook=%s, DX11 hook=%s",
        g_Present9Hooked ? "yes" : "no", g_Present11Hooked ? "yes" : "no");
    // Report which backend actually draws, once.
    for (int i = 0; i < 600 && !g_Stop.load(); ++i) {
        Sleep(100);
        int b = g_Backend.load();
        if (b != 0) { Log("Overlay is drawing through Direct3D %d", b); return 0; }
        if (g_Faulted.load()) { Log("Overlay disabled after a fault"); return 0; }
    }
    Log("Overlay has not rendered a frame yet (menu/loading, or the game uses another present path)");
    return 0;
}

} // namespace

bool Start(const ModloaderAPI* api, DrawCallback callback) {
    if (!api || !callback || g_Api) return false;
    g_Api = api;
    g_Callback = callback;
    QueryPerformanceFrequency(&g_Freq);
    HANDLE h = CreateThread(nullptr, 0, InstallThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    return h != nullptr;
}

const char* ActiveBackendName() {
    switch (g_Backend.load()) {
    case 9: return "DX9";
    case 11: return "DX11";
    default: return "none";
    }
}

} // namespace overlay
