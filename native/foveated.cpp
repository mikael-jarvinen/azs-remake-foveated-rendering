// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Mikael Järvinen
// Foveated Rendering for Arizona Sunshine Remake - https://github.com/mikael-jarvinen/azs-remake-foveated-rendering
// Licensed under GPL-3.0 with the additional terms in ADDITIONAL-TERMS.md (attribution, permission to run with the game).

// AZS1HD.Foveated.Native v3 - eye-tracked Variable Rate Shading for Arizona Sunshine Remake (Unity URP, D3D11).
//
// Pipeline
//  * Hooks ID3D11DeviceContext::OMSetRenderTargets(+AndUnorderedAccessViews) on the immediate and deferred
//    context vtables. A bind is a "scene pass" when depth is bound and colour target 0 is an eye-shaped
//    texture array that is multisampled or an HDR float format (URP's scene colour buffer). Scene passes
//    get VRS with a shading-rate image (SRI) of the matching size; any other bind turns VRS off.
//  * Foveation is angular: every SRI tile knows its view direction in eye space (from OpenVR's raw
//    projection, the same one Unity renders with), and the gaze is a head-space direction. A tile is full
//    rate when the angle between them is below InnerDegrees, 2x2 below MidDegrees, 4x4 beyond.
//  * Gaze: SteamVR eye tracking. GetEyeTrackingDataForNextFrame is paired with the compositor's render
//    pose for the same frame (GetLastPoses), so gaze and the rendered image refer to the same head pose.
//    Only the direction is used; the fixation distance is noisy.
//  * Unity eye textures are bottom-up: texture row 0 is the bottom of the view.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include "nvapi.h"
#include "openvr.h"

#define FOV_API extern "C" __declspec(dllexport)

// ------------------------------------------------------------------------------------------------ logging
static FILE* g_log = nullptr;
static std::mutex g_logMutex;
static void Log(const char* fmt, ...)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fprintf(g_log, "\n"); fflush(g_log);
}

// ------------------------------------------------------------------------------------------------ params / gaze
struct Params
{
    int enabled = 1;
    float cosInner = cosf(0.30f), cosMid = cosf(0.50f);
    int innerRate = 0, midRate = 1, outerRate = 2;   // indices into the rate table
    int flipRows = 0;                                // troubleshooting: treat texture rows as top-down
    int noGazeMode = 0;                              // 0 = full rate everywhere, 1 = foveate around straight ahead
    int allowNonMsaaHdr = 1;                         // accept single-sampled HDR scene buffers (MSAA off)
};
static Params g_params;
static std::mutex g_paramMutex;
static std::atomic<int> g_enabledFast{ 1 };

static std::mutex g_gazeMutex;
static float g_gazeDir[3] = { 0, 0, -1 };   // head space: +x right, +y up, -z forward
static int g_gazeValid = 0;

// ------------------------------------------------------------------------------------------------ OpenVR
static vr::IVRInput* g_vrInput = nullptr;
static vr::IVRSystem* g_vrSystem = nullptr;
static vr::IVRCompositor* g_vrCompositor = nullptr;
static vr::VRActionHandle_t g_gazeAction = vr::k_ulInvalidActionHandle;

static bool OpenVrInterfaces()
{
    if (g_vrSystem) return true;
    HMODULE m = GetModuleHandleA("openvr_api.dll");
    if (!m) return false;
    typedef void* (*GetGenericInterface_t)(const char*, vr::EVRInitError*);
    auto getIface = (GetGenericInterface_t)GetProcAddress(m, "VR_GetGenericInterface");
    if (!getIface) return false;
    vr::EVRInitError err = vr::VRInitError_None;
    g_vrSystem = (vr::IVRSystem*)getIface(vr::IVRSystem_Version, &err);
    g_vrInput = (vr::IVRInput*)getIface(vr::IVRInput_Version, &err);
    g_vrCompositor = (vr::IVRCompositor*)getIface(vr::IVRCompositor_Version, &err);
    Log("OpenVR interfaces: system %p input %p compositor %p", g_vrSystem, g_vrInput, g_vrCompositor);
    return g_vrSystem != nullptr;
}

static void InvRotate(const vr::HmdMatrix34_t& m, const float* v, float* o)   // R^T * v
{
    for (int c = 0; c < 3; c++) o[c] = m.m[0][c] * v[0] + m.m[1][c] * v[1] + m.m[2][c] * v[2];
}
static void Normalize(float* v) { float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (l > 1e-8f) { v[0] /= l; v[1] /= l; v[2] /= l; } }

struct EyeProj { float l, r, t, b; vr::HmdMatrix34_t eyeToHead; bool fromRuntime; };
static EyeProj GetEyeProj(int eye)
{
    EyeProj p;
    if (OpenVrInterfaces())
    {
        g_vrSystem->GetProjectionRaw((vr::EVREye)eye, &p.l, &p.r, &p.t, &p.b);
        p.eyeToHead = g_vrSystem->GetEyeToHeadTransform((vr::EVREye)eye);
        p.fromRuntime = true;
    }
    else
    {
        p.l = -1.2f; p.r = 1.2f; p.t = -1.2f; p.b = 1.2f; p.fromRuntime = false;
        memset(&p.eyeToHead, 0, sizeof(p.eyeToHead)); p.eyeToHead.m[0][0] = p.eyeToHead.m[1][1] = p.eyeToHead.m[2][2] = 1;
    }
    return p;
}

// ------------------------------------------------------------------------------------------------ D3D state
static const UINT kTile = 16;   // NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH/HEIGHT
static ID3D11Device*        g_device = nullptr;
static ID3D11DeviceContext* g_immediate = nullptr;
static bool g_nvapiReady = false, g_vrsSupported = false;
static std::atomic<int> g_eyeW{ 0 }, g_eyeH{ 0 }, g_eyeSlices{ 2 };

struct SriEntry
{
    UINT w = 0, h = 0, slices = 0, sw = 0, sh = 0;
    ID3D11Texture2D* tex = nullptr;
    ID3D11NvShadingRateResourceView* view = nullptr;
    std::vector<BYTE> data;                 // sw*sh*slices
    std::vector<float> dirs;                // per slice, per tile: unit view direction in eye space (xyz)
    EyeProj proj[2];
    std::atomic<long> lastUseFrame{ 0 };
};
static const int kMaxSri = 8;
static SriEntry* g_sri[kMaxSri] = {};         // published entries, read lock-free
static std::atomic<int> g_sriCount{ 0 };
static std::mutex g_sriCreateMutex;
static bool g_sriFullLogged = false;

static std::atomic<int> g_dumpEnabled{ 0 };
static ID3D11Texture2D* g_dumpTex = nullptr;
static std::mutex g_dumpMutex;
static std::atomic<int> g_dumpRequest{ 0 };
static char g_dumpPath[MAX_PATH] = { 0 };

static std::atomic<long> g_enables{ 0 }, g_disables{ 0 }, g_disablesSkipped{ 0 }, g_frames{ 0 }, g_descMatches{ 0 };

// ------------------------------------------------------------------------------------------------ SRI
static void BuildDirs(SriEntry& e)
{
    e.dirs.resize((size_t)e.sw * e.sh * e.slices * 3);
    for (UINT s = 0; s < e.slices; s++)
    {
        const EyeProj& p = e.proj[s < 2 ? s : 1];
        for (UINT y = 0; y < e.sh; y++)
            for (UINT x = 0; x < e.sw; x++)
            {
                float u = (x + 0.5f) * kTile / (float)e.w;                 // 0 = left
                float v = (y + 0.5f) * kTile / (float)e.h;                 // 0 = bottom row of a bottom-up texture
                if (g_params.flipRows) v = 1.0f - v;
                float d[3] = { p.l + u * (p.r - p.l), p.t + v * (p.b - p.t), -1.0f };   // OpenVR raw: b = top tangent
                Normalize(d);
                float* o = &e.dirs[(((size_t)s * e.sh + y) * e.sw + x) * 3];
                o[0] = d[0]; o[1] = d[1]; o[2] = d[2];
            }
    }
}

static void FillPattern(SriEntry& e, const Params& p, const float* gazeHead, bool valid)
{
    for (UINT s = 0; s < e.slices; s++)
    {
        BYTE* out = e.data.data() + (size_t)s * e.sw * e.sh;
        if (!valid && p.noGazeMode == 0) { memset(out, 0, (size_t)e.sw * e.sh); continue; }
        float gh[3] = { 0, 0, -1 };
        if (valid) { gh[0] = gazeHead[0]; gh[1] = gazeHead[1]; gh[2] = gazeHead[2]; }
        float g[3]; InvRotate(e.proj[s < 2 ? s : 1].eyeToHead, gh, g); Normalize(g);
        const float* d = &e.dirs[(size_t)s * e.sh * e.sw * 3];
        size_t n = (size_t)e.sw * e.sh;
        for (size_t i = 0; i < n; i++, d += 3)
        {
            float c = d[0] * g[0] + d[1] * g[1] + d[2] * g[2];
            out[i] = (BYTE)(c > p.cosInner ? p.innerRate : (c > p.cosMid ? p.midRate : p.outerRate));
        }
    }
}

static void CurrentGaze(float* dir, bool* valid)
{
    std::lock_guard<std::mutex> lock(g_gazeMutex);
    dir[0] = g_gazeDir[0]; dir[1] = g_gazeDir[1]; dir[2] = g_gazeDir[2]; *valid = g_gazeValid != 0;
}

// Creates and publishes an SRI for a target size. Returns nullptr when out of slots or on failure.
static SriEntry* CreateSri(UINT w, UINT h, UINT slices)
{
    std::lock_guard<std::mutex> lock(g_sriCreateMutex);
    int n = g_sriCount.load();
    for (int i = 0; i < n; i++)
        if (g_sri[i]->w == w && g_sri[i]->h == h && g_sri[i]->slices == slices) return g_sri[i];
    if (n >= kMaxSri) { if (!g_sriFullLogged) { g_sriFullLogged = true; Log("SRI table full (%d sizes); %ux%u not foveated", kMaxSri, w, h); } return nullptr; }
    SriEntry* e = new SriEntry();
    e->w = w; e->h = h; e->slices = slices;
    e->sw = (w + kTile - 1) / kTile; e->sh = (h + kTile - 1) / kTile;
    e->proj[0] = GetEyeProj(0); e->proj[1] = GetEyeProj(1);
    BuildDirs(*e);
    e->data.assign((size_t)e->sw * e->sh * slices, 0);
    Params p; { std::lock_guard<std::mutex> pl(g_paramMutex); p = g_params; }
    float gaze[3]; bool valid; CurrentGaze(gaze, &valid);
    FillPattern(*e, p, gaze, valid);
    std::vector<D3D11_SUBRESOURCE_DATA> init(slices);
    for (UINT s = 0; s < slices; s++) { init[s].pSysMem = e->data.data() + (size_t)s * e->sw * e->sh; init[s].SysMemPitch = e->sw; }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = e->sw; td.Height = e->sh; td.MipLevels = 1; td.ArraySize = slices;
    td.Format = DXGI_FORMAT_R8_UINT; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = g_device->CreateTexture2D(&td, init.data(), &e->tex);
    if (FAILED(hr)) { Log("CreateTexture2D(SRI %ux%u) failed 0x%08X", e->sw, e->sh, hr); delete e; return nullptr; }
    NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC vd = {};
    vd.version = NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC_VER;
    vd.Format = DXGI_FORMAT_R8_UINT;
    if (slices > 1) { vd.ViewDimension = NV_SRRV_DIMENSION_TEXTURE2DARRAY; vd.Texture2DArray.MipSlice = 0; vd.Texture2DArray.FirstArraySlice = 0; vd.Texture2DArray.ArraySize = slices; }
    else { vd.ViewDimension = NV_SRRV_DIMENSION_TEXTURE2D; vd.Texture2D.MipSlice = 0; }
    NvAPI_Status st = NvAPI_D3D11_CreateShadingRateResourceView(g_device, e->tex, &vd, &e->view);
    if (st != NVAPI_OK) { Log("CreateShadingRateResourceView failed %d", (int)st); e->tex->Release(); delete e; return nullptr; }
    g_sri[n] = e;
    g_sriCount.store(n + 1);   // publish after the entry is complete
    Log("SRI #%d: %ux%u tiles x%u for %ux%u buffer; eye0 tangents l%.3f r%.3f t%.3f b%.3f%s", n, e->sw, e->sh, slices, w, h,
        e->proj[0].l, e->proj[0].r, e->proj[0].t, e->proj[0].b, e->proj[0].fromRuntime ? "" : " (fallback, OpenVR not ready)");
    return e;
}

static void RefreshSris()
{
    if (!g_immediate) return;
    Params p; { std::lock_guard<std::mutex> lock(g_paramMutex); p = g_params; }
    float gaze[3]; bool valid; CurrentGaze(gaze, &valid);
    int n = g_sriCount.load();
    for (int i = 0; i < n; i++)
    {
        SriEntry& e = *g_sri[i];
        if (!e.proj[0].fromRuntime && OpenVrInterfaces())      // projection became available later
        { e.proj[0] = GetEyeProj(0); e.proj[1] = GetEyeProj(1); BuildDirs(e); Log("SRI #%d: projection refreshed from OpenVR", i); }
        if (g_frames.load() - e.lastUseFrame.load() > 600) continue;   // size not used for ~7 s: skip uploads
        FillPattern(e, p, gaze, valid);
        for (UINT s = 0; s < e.slices; s++)
            g_immediate->UpdateSubresource(e.tex, s, nullptr, e.data.data() + (size_t)s * e.sw * e.sh, e.sw, 0);
    }
}

// ------------------------------------------------------------------------------------------------ VRS state
static NV_D3D11_VIEWPORT_SHADING_RATE_DESC g_vpOn[16], g_vpOff[16];
static void InitRateTables()
{
    for (int i = 0; i < 16; i++)
    {
        for (int k = 0; k < NV_MAX_PIXEL_SHADING_RATES; k++) g_vpOn[i].shadingRateTable[k] = g_vpOff[i].shadingRateTable[k] = NV_PIXEL_X1_PER_RASTER_PIXEL;
        g_vpOn[i].shadingRateTable[1] = NV_PIXEL_X1_PER_2X2_RASTER_PIXELS;
        g_vpOn[i].shadingRateTable[2] = NV_PIXEL_X1_PER_4X4_RASTER_PIXELS;
        g_vpOn[i].shadingRateTable[3] = NV_PIXEL_X1_PER_2X1_RASTER_PIXELS;
        g_vpOn[i].shadingRateTable[4] = NV_PIXEL_X1_PER_1X2_RASTER_PIXELS;
        g_vpOn[i].enableVariablePixelShadingRate = true;
        g_vpOff[i].enableVariablePixelShadingRate = false;
    }
}

// Per-context "VRS is on" flag so redundant disables can be skipped. A context is only used by one thread
// at a time; a stale "on" flag only causes a harmless extra disable.
struct CtxState { std::atomic<void*> ctx{ nullptr }; std::atomic<int> on{ 1 }; };
static CtxState g_ctxState[32];
static CtxState* StateFor(ID3D11DeviceContext* ctx)
{
    for (auto& s : g_ctxState) if (s.ctx.load() == ctx) return &s;
    for (auto& s : g_ctxState) { void* expected = nullptr; if (s.ctx.compare_exchange_strong(expected, ctx)) { s.on.store(1); return &s; } }
    return nullptr;
}

static void SetVrs(ID3D11DeviceContext* ctx, ID3D11NvShadingRateResourceView* view)
{
    CtxState* st = StateFor(ctx);
    if (!view && st && st->on.load() == 0) { g_disablesSkipped++; return; }
    NV_D3D11_VIEWPORTS_SHADING_RATE_DESC desc = {};
    desc.version = NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_VER;
    desc.numViewports = 16;
    desc.pViewports = view ? g_vpOn : g_vpOff;
    NvAPI_D3D11_RSSetViewportsPixelShadingRates(ctx, &desc);
    NvAPI_D3D11_RSSetShadingRateResourceView(ctx, view);
    if (st) st->on.store(view ? 1 : 0);
}

static bool IsHdrFloat(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R11G11B10_FLOAT || f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R16G16B16A16_TYPELESS || f == DXGI_FORMAT_R32G32B32A32_FLOAT;
}

static ID3D11NvShadingRateResourceView* SriForBind(UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv)
{
    if (!g_enabledFast.load() || !dsv || n == 0 || !rtvs || !rtvs[0]) return nullptr;
    ID3D11Resource* res = nullptr;
    rtvs[0]->GetResource(&res);
    if (!res) return nullptr;
    D3D11_RESOURCE_DIMENSION dim; res->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) { res->Release(); return nullptr; }
    ID3D11Texture2D* tex = (ID3D11Texture2D*)res;   // same object, dimension checked
    D3D11_TEXTURE2D_DESC d; tex->GetDesc(&d);
    int ew = g_eyeW.load(), eh = g_eyeH.load(), es = g_eyeSlices.load();
    float eyeAspect = (ew > 0 && eh > 0) ? (float)ew / (float)eh : (float)d.Width / (float)d.Height;
    float aspect = (float)d.Width / (float)d.Height;
    bool shape = d.ArraySize == (UINT)es && d.MipLevels == 1 && fabsf(aspect - eyeAspect) < 0.03f * eyeAspect
              && d.Width >= 256 && (ew == 0 || d.Width <= (UINT)ew * 2);
    bool scene = shape && (d.SampleDesc.Count > 1 || (g_params.allowNonMsaaHdr && IsHdrFloat(d.Format)));
    ID3D11NvShadingRateResourceView* view = nullptr;
    if (scene)
    {
        g_descMatches++;
        SriEntry* e = nullptr;
        int c = g_sriCount.load();
        for (int i = 0; i < c; i++)
            if (g_sri[i]->w == d.Width && g_sri[i]->h == d.Height && g_sri[i]->slices == d.ArraySize) { e = g_sri[i]; break; }
        if (!e) e = CreateSri(d.Width, d.Height, d.ArraySize);
        if (e) { view = e->view; e->lastUseFrame.store(g_frames.load()); }
        if (view && g_dumpEnabled.load())
        {
            std::lock_guard<std::mutex> lock(g_dumpMutex);
            D3D11_TEXTURE2D_DESC cur = {};
            if (g_dumpTex) g_dumpTex->GetDesc(&cur);
            if (g_dumpTex != tex && d.Width >= cur.Width) { if (g_dumpTex) g_dumpTex->Release(); tex->AddRef(); g_dumpTex = tex; }
        }
    }
    res->Release();
    return view;
}

// ------------------------------------------------------------------------------------------------ hooks
typedef void (STDMETHODCALLTYPE* OMSetRenderTargets_t)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
typedef void (STDMETHODCALLTYPE* OMSetRenderTargetsAndUAV_t)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
typedef HRESULT(STDMETHODCALLTYPE* CreateDeferredContext_t)(ID3D11Device*, UINT, ID3D11DeviceContext**);
struct HookedVtbl { void** vtbl; OMSetRenderTargets_t origOM; OMSetRenderTargetsAndUAV_t origOMUAV; };
static HookedVtbl g_hooked[8];
static std::atomic<int> g_hookedCount{ 0 };
static CreateDeferredContext_t g_origCreateDeferred = nullptr;
static std::mutex g_hookMutex;

static void OnBind(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv)
{
    ID3D11NvShadingRateResourceView* view = SriForBind(n, rtvs, dsv);
    SetVrs(ctx, view);
    if (view) g_enables++; else g_disables++;
}

static HookedVtbl* FindHook(ID3D11DeviceContext* ctx)
{
    void** vt = *(void***)ctx;
    int n = g_hookedCount.load();
    for (int i = 0; i < n; i++) if (g_hooked[i].vtbl == vt) return &g_hooked[i];
    return nullptr;
}

static void STDMETHODCALLTYPE Hook_OMSetRenderTargets(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv)
{
    HookedVtbl* h = FindHook(ctx);
    if (h) h->origOM(ctx, n, rtvs, dsv);
    OnBind(ctx, n, rtvs, dsv);
}

static void STDMETHODCALLTYPE Hook_OMSetRenderTargetsAndUAV(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv, UINT uavStart, UINT uavCount, ID3D11UnorderedAccessView* const* uavs, const UINT* counts)
{
    HookedVtbl* h = FindHook(ctx);
    if (h) h->origOMUAV(ctx, n, rtvs, dsv, uavStart, uavCount, uavs, counts);
    if (n == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) return;
    OnBind(ctx, n, rtvs, dsv);
}

static void PatchVtbl(void** vtbl, int index, void* fn, void** outOrig)
{
    DWORD old;
    VirtualProtect(&vtbl[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    *outOrig = vtbl[index];
    vtbl[index] = fn;
    VirtualProtect(&vtbl[index], sizeof(void*), old, &old);
}

static void HookContext(ID3D11DeviceContext* ctx, const char* what)
{
    std::lock_guard<std::mutex> lock(g_hookMutex);
    if (FindHook(ctx)) return;
    int i = g_hookedCount.load();
    if (i >= 8) return;
    void** vt = *(void***)ctx;
    g_hooked[i].vtbl = vt;
    PatchVtbl(vt, 33, (void*)&Hook_OMSetRenderTargets, (void**)&g_hooked[i].origOM);
    PatchVtbl(vt, 34, (void*)&Hook_OMSetRenderTargetsAndUAV, (void**)&g_hooked[i].origOMUAV);
    g_hookedCount.store(i + 1);
    Log("Hooked OMSetRenderTargets on %s context vtable %p", what, vt);
}

static HRESULT STDMETHODCALLTYPE Hook_CreateDeferredContext(ID3D11Device* dev, UINT flags, ID3D11DeviceContext** out)
{
    HRESULT hr = g_origCreateDeferred(dev, flags, out);
    if (SUCCEEDED(hr) && out && *out) HookContext(*out, "deferred");
    return hr;
}

static void InstallHooks()
{
    HookContext(g_immediate, "immediate");
    ID3D11DeviceContext* dc = nullptr;
    if (SUCCEEDED(g_device->CreateDeferredContext(0, &dc)) && dc) { HookContext(dc, "deferred"); dc->Release(); }
    if (!g_origCreateDeferred)
        PatchVtbl(*(void***)g_device, 27, (void*)&Hook_CreateDeferredContext, (void**)&g_origCreateDeferred);
}

// ------------------------------------------------------------------------------------------------ projection helper
// Head-space direction -> per-eye (u, v): u left->right, v bottom->top. Used for logs and the dump marker.
static void ProjectHeadDir(const float* dirHead, float* out)
{
    for (int eye = 0; eye < 2; eye++)
    {
        EyeProj p = GetEyeProj(eye);
        float de[3]; InvRotate(p.eyeToHead, dirHead, de);
        float z = -de[2]; if (z < 1e-3f) z = 1e-3f;
        float tx = de[0] / z, ty = de[1] / z;
        out[eye * 2 + 0] = (tx - p.l) / (p.r - p.l);
        out[eye * 2 + 1] = (ty - p.t) / (p.b - p.t);
    }
}

// ------------------------------------------------------------------------------------------------ debug dump
static float HalfToFloat(uint16_t h)
{
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1f, m = h & 0x3ff; float f;
    if (e == 0) f = ldexpf((float)m, -24); else if (e == 31) f = m ? NAN : INFINITY; else f = ldexpf((float)(m + 1024), (int)e - 25);
    return s ? -f : f;
}
static float F11(uint32_t v) { uint32_t e = (v >> 6) & 0x1f, m = v & 0x3f; if (e == 0) return ldexpf((float)m, -20); if (e == 31) return INFINITY; return ldexpf((float)(m + 64), (int)e - 21); }
static float F10(uint32_t v) { uint32_t e = (v >> 5) & 0x1f, m = v & 0x1f; if (e == 0) return ldexpf((float)m, -19); if (e == 31) return INFINITY; return ldexpf((float)(m + 32), (int)e - 20); }
static BYTE Tone(float v) { if (!(v > 0)) return 0; v = v / (1.0f + v); v = powf(v, 1.0f / 2.2f); return (BYTE)(v * 255.0f + 0.5f); }

static void DumpScene(const char* path)
{
    ID3D11Texture2D* src = nullptr;
    { std::lock_guard<std::mutex> lock(g_dumpMutex); src = g_dumpTex; if (src) src->AddRef(); }
    if (!src || !g_immediate) { Log("dump: no scene buffer seen yet"); if (src) src->Release(); return; }
    D3D11_TEXTURE2D_DESC d; src->GetDesc(&d);
    DXGI_FORMAT fmt = d.Format;
    if (fmt == DXGI_FORMAT_R16G16B16A16_TYPELESS) fmt = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (fmt == DXGI_FORMAT_R8G8B8A8_TYPELESS) fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    D3D11_TEXTURE2D_DESC rd = d; rd.SampleDesc.Count = 1; rd.SampleDesc.Quality = 0; rd.BindFlags = 0; rd.MiscFlags = 0; rd.MipLevels = 1; rd.Format = fmt; rd.Usage = D3D11_USAGE_DEFAULT;
    ID3D11Texture2D* resolved = nullptr; ID3D11Texture2D* staging = nullptr;
    if (FAILED(g_device->CreateTexture2D(&rd, nullptr, &resolved))) { Log("dump: resolve tex failed"); src->Release(); return; }
    D3D11_TEXTURE2D_DESC sd = rd; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(g_device->CreateTexture2D(&sd, nullptr, &staging))) { Log("dump: staging failed"); resolved->Release(); src->Release(); return; }
    for (UINT s = 0; s < d.ArraySize; s++)
    {
        if (d.SampleDesc.Count > 1) g_immediate->ResolveSubresource(resolved, s, src, s, fmt);
        else g_immediate->CopySubresourceRegion(resolved, s, 0, 0, 0, src, s, nullptr);
    }
    g_immediate->CopyResource(staging, resolved);
    float gaze[3]; bool valid; CurrentGaze(gaze, &valid);
    float uv[4] = { 0.5f, 0.5f, 0.5f, 0.5f }; ProjectHeadDir(gaze, uv);
    UINT w = d.Width, h = d.Height, slices = d.ArraySize;
    std::vector<BYTE> rgb((size_t)w * slices * h * 3, 0);
    bool hdr = fmt == DXGI_FORMAT_R11G11B10_FLOAT || fmt == DXGI_FORMAT_R16G16B16A16_FLOAT;
    for (UINT s = 0; s < slices; s++)
    {
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(g_immediate->Map(staging, s, D3D11_MAP_READ, 0, &m))) continue;
        for (UINT y = 0; y < h; y++)
        {
            const BYTE* row = (const BYTE*)m.pData + (size_t)y * m.RowPitch;
            UINT oy = h - 1 - y;   // bottom-up texture -> upright image
            for (UINT x = 0; x < w; x++)
            {
                float r = 0, g = 0, b = 0;
                if (fmt == DXGI_FORMAT_R11G11B10_FLOAT) { uint32_t v = *(const uint32_t*)(row + x * 4); r = F11(v); g = F11(v >> 11); b = F10(v >> 22); }
                else if (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) { const uint16_t* p = (const uint16_t*)(row + x * 8); r = HalfToFloat(p[0]); g = HalfToFloat(p[1]); b = HalfToFloat(p[2]); }
                else { const BYTE* p = row + x * 4; r = p[0] / 255.f; g = p[1] / 255.f; b = p[2] / 255.f; }
                BYTE* o = &rgb[((size_t)oy * w * slices + (size_t)s * w + x) * 3];
                o[0] = hdr ? Tone(r) : (BYTE)(r * 255); o[1] = hdr ? Tone(g) : (BYTE)(g * 255); o[2] = hdr ? Tone(b) : (BYTE)(b * 255);
            }
        }
        g_immediate->Unmap(staging, s);
        if (!valid) continue;   // no gaze: no marker
        int eye = s < 2 ? (int)s : 1;
        int gx = (int)(uv[eye * 2] * w), gy = (int)((1.0f - uv[eye * 2 + 1]) * h);
        for (int k = -14; k <= 14; k++)
            for (int t = -1; t <= 1; t++)
            {
                int pts[2][2] = { { gx + k, gy + t }, { gx + t, gy + k } };
                for (auto& pt : pts)
                    if (pt[0] >= 0 && (UINT)pt[0] < w && pt[1] >= 0 && (UINT)pt[1] < h)
                    { BYTE* o = &rgb[((size_t)pt[1] * w * slices + (size_t)s * w + pt[0]) * 3]; o[0] = 255; o[1] = 0; o[2] = 0; }
            }
    }
    FILE* f = fopen(path, "wb");
    if (f) { fprintf(f, "P6\n%u %u\n255\n", w * slices, h); fwrite(rgb.data(), 1, rgb.size(), f); fclose(f); Log("dump written: %ux%u x%u (fmt %d, MSAA %u)", w, h, slices, (int)fmt, d.SampleDesc.Count); }
    staging->Release(); resolved->Release(); src->Release();
}

// ------------------------------------------------------------------------------------------------ exports
FOV_API int FovInit(const char* logPath, const char* dumpPath)
{
    if (!g_log && logPath) g_log = fopen(logPath, "a");
    if (dumpPath) strncpy(g_dumpPath, dumpPath, MAX_PATH - 1);
    Log("FovInit (native v3: angular foveation, frame-matched gaze)");
    InitRateTables();
    NvAPI_Status st = NvAPI_Initialize();
    g_nvapiReady = (st == NVAPI_OK);
    Log("NvAPI_Initialize -> %d", (int)st);
    return (int)st;
}

FOV_API int FovGazeInit(const char* actionPath)
{
    if (!OpenVrInterfaces() || !g_vrInput) { Log("gaze init: OpenVR not ready"); return 0; }
    vr::EVRInputError ie = g_vrInput->GetActionHandle(actionPath, &g_gazeAction);
    Log("OpenVR gaze: action '%s' handle %llu (err %d)", actionPath, (unsigned long long)g_gazeAction, (int)ie);
    return ie == vr::VRInputError_None && g_gazeAction != vr::k_ulInvalidActionHandle;
}

FOV_API void FovSetParams(int enabled, float innerDeg, float midDeg, int innerRate, int midRate, int outerRate, int flipRows, int noGazeMode, int dumpEnabled)
{
    bool rebuild;
    {
        std::lock_guard<std::mutex> lock(g_paramMutex);
        rebuild = g_params.flipRows != flipRows;
        g_params.enabled = enabled;
        g_params.cosInner = cosf(innerDeg * 3.14159265f / 180.0f);
        g_params.cosMid = cosf(midDeg * 3.14159265f / 180.0f);
        g_params.innerRate = innerRate; g_params.midRate = midRate; g_params.outerRate = outerRate;
        g_params.flipRows = flipRows; g_params.noGazeMode = noGazeMode;
    }
    g_enabledFast.store(enabled);
    g_dumpEnabled.store(dumpEnabled);
    if (!dumpEnabled) { std::lock_guard<std::mutex> lock(g_dumpMutex); if (g_dumpTex) { g_dumpTex->Release(); g_dumpTex = nullptr; } }
    if (rebuild) { int n = g_sriCount.load(); for (int i = 0; i < n; i++) BuildDirs(*g_sri[i]); }
}

// Gaze for the frame being prepared, as a head-space unit direction.
FOV_API void FovSetGazeDir(float x, float y, float z, int valid)
{
    float d[3] = { x, y, z }; Normalize(d);
    std::lock_guard<std::mutex> lock(g_gazeMutex);
    g_gazeDir[0] = d[0]; g_gazeDir[1] = d[1]; g_gazeDir[2] = d[2]; g_gazeValid = valid;
}

// Diagnostics for the gaze path. Writes the connected headset's controller type to typeOut and a one-line
// description (model, eye-gaze support, what the gaze action is bound to) to textOut. Returns the number of
// input bindings SteamVR reports for the gaze action, or -1 when OpenVR is not available.
FOV_API int FovGazeDiagnostics(char* typeOut, int typeLen, char* textOut, int textLen)
{
    if (typeOut && typeLen > 0) typeOut[0] = 0;
    if (textOut && textLen > 0) textOut[0] = 0;
    if (!OpenVrInterfaces()) return -1;
    char type[128] = "", model[128] = "", manu[128] = "";
    vr::ETrackedPropertyError pe;
    g_vrSystem->GetStringTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_ControllerType_String, type, sizeof(type), &pe);
    g_vrSystem->GetStringTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_ModelNumber_String, model, sizeof(model), &pe);
    g_vrSystem->GetStringTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_ManufacturerName_String, manu, sizeof(manu), &pe);
    bool eyeGaze = g_vrSystem->GetBoolTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_SupportsXrEyeGazeInteraction_Bool, &pe);
    bool eyeGazeKnown = pe == vr::TrackedProp_Success;
    if (typeOut && typeLen > 0) { strncpy(typeOut, type, typeLen - 1); typeOut[typeLen - 1] = 0; }

    char bound[512] = "none";
    int count = 0;
    if (g_vrInput && g_gazeAction != vr::k_ulInvalidActionHandle)
    {
        vr::InputBindingInfo_t info[4] = {};
        uint32_t n = 0;
        vr::EVRInputError e = g_vrInput->GetActionBindingInfo(g_gazeAction, info, sizeof(vr::InputBindingInfo_t), 4, &n);
        if (e == vr::VRInputError_None && n > 0)
        {
            bound[0] = 0; count = (int)n;
            for (uint32_t i = 0; i < n; i++)
            {
                char one[200];
                snprintf(one, sizeof(one), "%s%s%s (%s)", i ? ", " : "", info[i].rchDevicePathName, info[i].rchInputPathName, info[i].rchInputSourceType);
                strncat(bound, one, sizeof(bound) - strlen(bound) - 1);
            }
        }
        else if (e != vr::VRInputError_None) snprintf(bound, sizeof(bound), "unknown (GetActionBindingInfo error %d)", (int)e);
    }
    if (textOut && textLen > 0)
        snprintf(textOut, textLen, "headset '%s %s' type '%s', advertises eye gaze: %s, gaze action bound to: %s",
                 manu, model, type, eyeGazeKnown ? (eyeGaze ? "yes" : "no") : "not reported", bound);
    Log("diagnostics: %s", textOut ? textOut : "");
    return count;
}

FOV_API int FovProjectHeadDir(float x, float y, float z, float* out)
{
    float d[3] = { x, y, z }; Normalize(d);
    ProjectHeadDir(d, out);
    return g_vrSystem ? 1 : 0;
}

// Reads SteamVR gaze for the frame Unity is about to render. Writes a head-space unit direction to dirOut[3].
// mode: 1 = next-frame data with the frame's render pose, 2 = latest sample with the current pose (fallback).
static DWORD g_lastRawLog = 0;
FOV_API int FovGazeUpdate(float* dirOut, int* outStatus, int* outMode)
{
    if (!g_vrInput || !g_vrSystem || g_gazeAction == vr::k_ulInvalidActionHandle) { if (outStatus) *outStatus = -1; return 0; }
    const vr::ETrackingUniverseOrigin uni = g_vrCompositor ? g_vrCompositor->GetTrackingSpace() : vr::TrackingUniverseStanding;
    vr::VREyeTrackingData_t d = {};
    int mode = 1;
    vr::EVRInputError e = g_vrInput->GetEyeTrackingDataForNextFrame(g_gazeAction, uni, &d, sizeof(d));
    vr::TrackedDevicePose_t pose = {};
    bool poseOk = false;
    if (e == vr::VRInputError_None && g_vrCompositor)
    {
        vr::TrackedDevicePose_t render[1];
        poseOk = g_vrCompositor->GetLastPoses(render, 1, nullptr, 0) == vr::VRCompositorError_None && render[0].bPoseIsValid;
        pose = render[0];
    }
    if (e != vr::VRInputError_None || !poseOk)
    {
        mode = 2;
        e = g_vrInput->GetEyeTrackingDataRelativeToNow(g_gazeAction, uni, 0.0f, &d, sizeof(d));
        vr::TrackedDevicePose_t now[1];
        g_vrSystem->GetDeviceToAbsoluteTrackingPose(uni, 0.0f, now, 1);
        pose = now[0]; poseOk = pose.bPoseIsValid;
    }
    if (outMode) *outMode = mode;
    if (outStatus) *outStatus = (int)e * 10 + (d.bActive ? 4 : 0) + (d.bValid ? 2 : 0) + (d.bTracked ? 1 : 0);
    if (e != vr::VRInputError_None || !d.bActive || !d.bValid || !poseOk) return 0;

    float dir[3] = { d.vGazeTarget.v[0] - d.vGazeOrigin.v[0], d.vGazeTarget.v[1] - d.vGazeOrigin.v[1], d.vGazeTarget.v[2] - d.vGazeOrigin.v[2] };
    float len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (!(len > 1e-4f)) return 0;
    for (int i = 0; i < 3; i++) dir[i] /= len;
    InvRotate(pose.mDeviceToAbsoluteTracking, dir, dirOut);
    Normalize(dirOut);

    DWORD now = GetTickCount();
    if (now - g_lastRawLog > 5000)
    {
        g_lastRawLog = now;
        const auto& hm = pose.mDeviceToAbsoluteTracking;
        float uv[4]; ProjectHeadDir(dirOut, uv);
        Log("gaze raw (mode %d, universe %d): origin (%.3f %.3f %.3f) target (%.3f %.3f %.3f) dist %.2f | head (%.3f %.3f %.3f) | dirHead (%.3f %.3f %.3f) = %.1f deg right, %.1f deg up | uv L(%.3f %.3f) R(%.3f %.3f)",
            mode, (int)uni, d.vGazeOrigin.v[0], d.vGazeOrigin.v[1], d.vGazeOrigin.v[2], d.vGazeTarget.v[0], d.vGazeTarget.v[1], d.vGazeTarget.v[2], len,
            hm.m[0][3], hm.m[1][3], hm.m[2][3], dirOut[0], dirOut[1], dirOut[2],
            atan2f(dirOut[0], -dirOut[2]) * 57.2958f, atan2f(dirOut[1], -dirOut[2]) * 57.2958f, uv[0], uv[1], uv[2], uv[3]);
    }
    return 1;
}

FOV_API int FovAttachDevice(void* anyTex, int eyeW, int eyeH, int eyeSlices)
{
    if (!g_nvapiReady || !anyTex) return -1;
    g_eyeW.store(eyeW); g_eyeH.store(eyeH); g_eyeSlices.store(eyeSlices > 0 ? eyeSlices : 2);
    Log("Attach: eye texture %dx%d x%d", eyeW, eyeH, eyeSlices);
    OpenVrInterfaces();
    if (g_device) return 1;
    ID3D11DeviceChild* child = nullptr;
    if (FAILED(((IUnknown*)anyTex)->QueryInterface(__uuidof(ID3D11DeviceChild), (void**)&child)) || !child) { Log("attach: not a D3D11 object"); return -2; }
    child->GetDevice(&g_device); child->Release();
    if (!g_device) return -3;
    g_device->GetImmediateContext(&g_immediate);
    NvAPI_Status st = NvAPI_D3D_RegisterDevice(g_device);
    Log("Device %p registered with NVAPI -> %d", g_device, (int)st);
    NV_D3D1x_GRAPHICS_CAPS caps = {};
    st = NvAPI_D3D1x_GetGraphicsCapabilities(g_device, NV_D3D1x_GRAPHICS_CAPS_VER, &caps);
    g_vrsSupported = (st == NVAPI_OK) && caps.bVariablePixelRateShadingSupported;
    Log("GetGraphicsCapabilities -> %d, VRS supported: %d", (int)st, (int)g_vrsSupported);
    if (!g_vrsSupported) return -4;
    InstallHooks();
    return 1;
}

// Unity render-thread plugin event: 1 = refresh SRIs (+ pending dump), 3 = dump now.
FOV_API void __stdcall FovRenderEvent(int eventId)
{
    if (eventId == 1)
    {
        g_frames++;
        RefreshSris();
        if (g_dumpRequest.exchange(0)) DumpScene(g_dumpPath);
    }
    else if (eventId == 3) DumpScene(g_dumpPath);
}

FOV_API void* FovGetRenderEventFunc() { return (void*)&FovRenderEvent; }
FOV_API void FovRequestDump() { g_dumpRequest.store(1); }

FOV_API void FovGetStats(long* frames, long* enables, long* disables, long* skipped, int* vrsSupported, int* hookedContexts, long* descMatches)
{
    if (frames) *frames = g_frames.load();
    if (enables) *enables = g_enables.exchange(0);
    if (disables) *disables = g_disables.exchange(0);
    if (skipped) *skipped = g_disablesSkipped.exchange(0);
    if (descMatches) *descMatches = g_descMatches.exchange(0);
    if (vrsSupported) *vrsSupported = g_vrsSupported ? 1 : 0;
    if (hookedContexts) *hookedContexts = g_hookedCount.load();
}

// Average app GPU time per frame over the last `frames` frames, from SteamVR's compositor timing.
FOV_API int FovGetGpuTiming(int frames, float* preSubmitMs, float* totalRenderMs)
{
    if (!g_vrCompositor) return 0;
    double a = 0, b = 0; int n = 0;
    for (int i = 0; i < frames; i++)
    {
        vr::Compositor_FrameTiming t = {}; t.m_nSize = sizeof(t);
        if (!g_vrCompositor->GetFrameTiming(&t, (uint32_t)i)) break;
        a += t.m_flPreSubmitGpuMs; b += t.m_flTotalRenderGpuMs; n++;
    }
    if (!n) return 0;
    *preSubmitMs = (float)(a / n); *totalRenderMs = (float)(b / n);
    return n;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
