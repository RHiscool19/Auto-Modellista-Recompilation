/**
 * Auto Modellista: Direct3D 11 render back end.
 *
 * Plugs into xboxrecomp's render back-end slot (nv2a_backend.h). The
 * pushbuffer executor still decodes the game's GPU commands, runs vertex
 * programs and clips on the CPU, but instead of rasterising every pixel in
 * software it hands finished triangles (in surface pixels) to this file,
 * which draws them on the host GPU and presents them in its own window.
 *
 * Enabled with AM_D3D11=1 (scripts/run.ps1 -D3D11). Off by default: the CPU
 * rasteriser stays the reference.
 *
 * Version 1 scope:
 *   - one render target per guest colour surface, with its own depth/stencil
 *   - texture stage 0 modulated by the vertex colour (what the interface
 *     passes on); register combiners and stages 1-3 are NOT applied yet, so
 *     the cel shading is missing
 *   - blend, alpha test, depth, stencil, colour mask, culling
 *   - render-to-texture: sampling a surface this back end rendered uses the
 *     GPU copy (nothing is written back to guest memory)
 *   - textures decoded with nv2a_backend_decode_texture and cached, re-checked
 *     once per frame with a sparse hash of their guest bytes
 *
 * All callbacks run on the NV2A poll thread, one at a time, so nothing here
 * is locked. The window is created lazily on the first call and its messages
 * are pumped from flip().
 */

#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include "../external/xboxrecomp/src/kernel/nv2a_backend.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);

/* The input layout below assumes this exact layout. */
typedef char nv2a_vertex_is_32_bytes[(sizeof(Nv2aVertex) == 32) ? 1 : -1];

/* ---- Device and window ------------------------------------------------ */

static HWND                    s_hwnd;
static ID3D11Device           *s_dev;
static ID3D11DeviceContext    *s_ctx;
static IDXGISwapChain         *s_swap;
static ID3D11RenderTargetView *s_back_rtv;
static int                     s_resize_pending;
static int                     s_init_failed;
static int                     s_ready;        /* every object below created */

static ID3D11VertexShader *s_vs, *s_blit_vs;
static ID3D11PixelShader  *s_ps, *s_blit_ps;
static ID3D11InputLayout  *s_layout;
static ID3D11Buffer       *s_vb, *s_cb;
static ID3D11RasterizerState *s_rs;
static ID3D11RasterizerState *s_rs_scissor;   /* draws: clipped to the surface clip */
static ID3D11DeviceContext1  *s_ctx1;         /* ClearView, for partial clears */
static const GUID s_iid_ctx1 = { 0xbb2c6faa, 0xb5fb, 0x4082, { 0x8e, 0x6b, 0x38, 0x8b, 0x8c, 0xfa, 0x90, 0xe1 } };
static ID3D11SamplerState *s_blit_sampler;

#define VB_VERTS 262144u           /* ring capacity in vertices (7 MB) */
static UINT s_vb_pos;

typedef struct {
    float rt_w, rt_h, zscale, textured;
    float alpha_func, alpha_ref, tex_w, tex_h;
} Consts;

/* Mirrors cbuffer RC (b1). 448 bytes. */
typedef struct {
    uint32_t st[8][4];            /* color_icw, alpha_icw, color_ocw, alpha_ocw */
    float    f0[8][4], f1[8][4];  /* factor0/1 as r,g,b,a */
    uint32_t fin[4];              /* final0, final1, control, stage_program */
    float    fc0[4], fc1[4];
    float    fog[4];              /* fog colour (rgb); .a unused (per-vertex) */
    uint32_t on[4];
} RcConsts;
typedef char rc_consts_is_464[(sizeof(RcConsts) == 464) ? 1 : -1];
static ID3D11Buffer *s_rc_cb;
static int           s_rc_enabled = 1;   /* AM_D3D11_COMBINERS=0 turns them off */
static unsigned      s_st_rc;            /* draws that went through the combiners */

static unsigned      s_st_rc_skip;       /* combiner programs that need inputs we don't pass yet */

/* 1 if the program reads only what this back end supplies: v0, t0, c0/c1,
 * zero, r0/r1, fog (3). Registers 5 (v1) and 9-11 (t1-t3), or texture stages
 * 1-3 switched on, need inputs the interface does not carry yet; such a
 * program is drawn as texture x diffuse instead, which is what the game
 * looked like before combiners and is far closer than a black result. */
static int rc_supported(const Nv2aBackendCombiner *bc)
{
    uint32_t n = bc->control & 0xFF, s, k;
    if (n > 8) n = 8;
    if (bc->stage_program & ~0x1Fu) return 0;          /* stages 1-3 in use */
    for (s = 0; s < n; s++)
        for (k = 0; k < 4; k++) {
            uint32_t c = (bc->color_icw[s] >> (8 * k)) & 0xFF, a = (bc->alpha_icw[s] >> (8 * k)) & 0xFF;
            uint32_t rc_ = c & 0xF, ra = a & 0xF;
            if (rc_ == 5 || (rc_ >= 9 && rc_ <= 11)) return 0;
            if (ra == 5 || (ra >= 9 && ra <= 11)) return 0;
        }
    if (bc->final0 || bc->final1) {
        for (k = 0; k < 4; k++) {
            uint32_t r0_ = (bc->final0 >> (8 * k)) & 0xF;
            if (r0_ == 5 || r0_ == 14 || (r0_ >= 9 && r0_ <= 11)) return 0;
        }
        for (k = 1; k < 4; k++) {                      /* E, F, G (byte 0 is flags) */
            uint32_t r1_ = (bc->final1 >> (8 * k)) & 0xF;
            if (r1_ == 5 || r1_ == 14 || (r1_ >= 9 && r1_ <= 11)) return 0;
        }
    }
    return 1;
}

static void unpack_argb(uint32_t c, float o[4])
{
    o[0] = (float)((c >> 16) & 0xFF) / 255.0f;
    o[1] = (float)((c >> 8) & 0xFF) / 255.0f;
    o[2] = (float)(c & 0xFF) / 255.0f;
    o[3] = (float)(c >> 24) / 255.0f;
}
static RcConsts      s_rc_last;
static int           s_rc_last_valid;
/* ---- Shaders ------------------------------------------------------------ */

static const char s_hlsl[] =
"cbuffer C : register(b0) {\n"
"  float2 rtSize; float zscale; float textured;\n"
"  float alphaFunc; float alphaRef; float2 texSize; };\n"
"Texture2D T : register(t0); SamplerState S : register(s0);\n"
"struct VI { float4 p : POSITION; float4 c : COLOR; float2 uv : TEXCOORD; float fog : FOG; };\n"
"struct PI { float4 pos : SV_Position; float4 c : COLOR; float2 uv : TEXCOORD; float fog : FOG; };\n"
"PI vs_main(VI i) {\n"
"  PI o;\n"
"  float w = i.p.w > 0.0 ? 1.0 / i.p.w : 1.0;\n"
"  float x = i.p.x / rtSize.x * 2.0 - 1.0;\n"
"  float y = 1.0 - i.p.y / rtSize.y * 2.0;\n"
"  float z = saturate(i.p.z / zscale);\n"
"  o.pos = float4(x * w, y * w, z * w, w);\n"
"  o.c = i.c; o.uv = i.uv / texSize; o.fog = i.fog;\n"
"  return o; }\n"
"cbuffer RC : register(b1) {\n"
"  uint4 rcSt[8]; float4 rcF0[8]; float4 rcF1[8];\n"
"  uint4 rcFin; float4 rcFc0; float4 rcFc1; float4 rcFog; uint4 rcOn; };\n"
"float mapIn(float x, uint b) {\n"
"  float p = max(x, 0.0); uint m = (b >> 5) & 7;\n"
"  if (m == 0) return p; if (m == 1) return 1.0 - saturate(x);\n"
"  if (m == 2) return 2.0 * p - 1.0; if (m == 3) return -2.0 * p + 1.0;\n"
"  if (m == 4) return p - 0.5; if (m == 5) return -p + 0.5;\n"
"  if (m == 6) return x; return -x; }\n"
"float mapOut(float x, uint m) {\n"
"  if (m == 0x08) return x - 0.5; if (m == 0x10) return x * 2.0;\n"
"  if (m == 0x18) return (x - 0.5) * 2.0; if (m == 0x20) return x * 4.0;\n"
"  if (m == 0x30) return x * 0.5; return x; }\n"
"float3 mapOut3(float3 v, uint m) { return float3(mapOut(v.x, m), mapOut(v.y, m), mapOut(v.z, m)); }\n"
"float3 rdRGB(float4 R[16], uint b) {\n"
"  b &= 0xFF; float4 r = R[b & 15]; float3 v = (b & 16) ? r.aaa : r.rgb;\n"
"  return float3(mapIn(v.x, b), mapIn(v.y, b), mapIn(v.z, b)); }\n"
"float rdA(float4 R[16], uint b) {\n"
"  b &= 0xFF; float4 r = R[b & 15]; return mapIn((b & 16) ? r.a : r.b, b); }\n"
"float4 rcEval(float4 v0, float4 t0, float fogf) {\n"
"  float4 R[16]; uint k;\n"
"  [unroll] for (k = 0; k < 16; k++) R[k] = float4(0, 0, 0, 0);\n"
"  R[3] = float4(rcFog.rgb, fogf);\n"
"  R[4] = v0; R[8] = t0; R[9] = float4(0,0,0,1); R[10] = float4(0,0,0,1); R[11] = float4(0,0,0,1);\n"
"  R[12].a = (RCFIN.w & 0x1F) ? t0.a : 1.0;\n"
"  uint n = min(RCFIN.z & 0xFF, 8), fl0 = RCFIN.z >> 8;\n"
"  RC_LOOP for (uint s = 0; s < n; s++) {\n"
"    uint icw = RCST[s].x, aicw = RCST[s].y, ocw = RCST[s].z, aocw = RCST[s].w;\n"
"    uint fl = ocw >> 12, afl = aocw >> 12, d;\n"
"    R[1] = rcF0[(fl0 & 0x010) ? s : 0]; R[2] = rcF1[(fl0 & 0x100) ? s : 0];\n"
"    bool mux = (fl0 & 1) ? (R[12].a >= 0.5) : ((((int)(R[12].a * 255.0)) & 1) != 0);\n"
"    float3 A = rdRGB(R, icw >> 24), B = rdRGB(R, icw >> 16), C = rdRGB(R, icw >> 8), D = rdRGB(R, icw);\n"
"    float aA = rdA(R, aicw >> 24), aB = rdA(R, aicw >> 16), aC = rdA(R, aicw >> 8), aD = rdA(R, aicw);\n"
"    float3 pab = (fl & 2) ? dot(A, B).xxx : A * B;\n"
"    float3 pcd = (fl & 1) ? dot(C, D).xxx : C * D;\n"
"    float3 sm = (fl & 4) ? (mux ? pcd : pab) : pab + pcd;\n"
"    float3 ab = clamp(mapOut3(pab, fl & 0x38), -1.0, 1.0);\n"
"    float3 cd = clamp(mapOut3(pcd, fl & 0x38), -1.0, 1.0);\n"
"    float3 ms = clamp(mapOut3(sm, fl & 0x38), -1.0, 1.0);\n"
"    float qab = aA * aB, qcd = aC * aD;\n"
"    float qs = (afl & 4) ? (mux ? qcd : qab) : qab + qcd;\n"
"    float aab = clamp(mapOut(qab, afl & 0x38), -1.0, 1.0);\n"
"    float acd = clamp(mapOut(qcd, afl & 0x38), -1.0, 1.0);\n"
"    float ams = clamp(mapOut(qs, afl & 0x38), -1.0, 1.0);\n"
"    d = (ocw >> 4) & 15; if (d) { R[d].rgb = ab; if (fl & 0x80) R[d].a = ab.b; }\n"
"    d = ocw & 15;        if (d) { R[d].rgb = cd; if (fl & 0x40) R[d].a = cd.b; }\n"
"    d = (ocw >> 8) & 15; if (d) R[d].rgb = ms;\n"
"    d = (aocw >> 4) & 15; if (d) R[d].a = aab;\n"
"    d = aocw & 15;        if (d) R[d].a = acd;\n"
"    d = (aocw >> 8) & 15; if (d) R[d].a = ams;\n"
"    R[0] = float4(0, 0, 0, 0); }\n"
"  float4 o;\n"
"  if (RCFIN.x || RCFIN.y) {\n"
"    uint f0 = RCFIN.x, f1 = RCFIN.y, ff = f1 & 0xFF;\n"
"    R[1] = rcFc0; R[2] = rcFc1;\n"
"    float3 sa = (ff & 0x40) ? 1.0 - R[5].rgb : R[5].rgb;\n"
"    float3 sb = (ff & 0x20) ? 1.0 - R[12].rgb : R[12].rgb;\n"
"    R[14] = float4((ff & 0x80) ? saturate(sa + sb) : sa + sb, 0);\n"
"    float3 E = rdRGB(R, f1 >> 24), F = rdRGB(R, f1 >> 16);\n"
"    R[15] = float4(E * F, 0);\n"
"    float3 A = rdRGB(R, f0 >> 24), B = rdRGB(R, f0 >> 16), C = rdRGB(R, f0 >> 8), D = rdRGB(R, f0);\n"
"    o = float4(D + C * (1.0 - A) + B * A, rdA(R, f1 >> 8));\n"
"  } else o = R[12];\n"
"  return saturate(o); }\n"
"float4 ps_main(PI i) : SV_Target {\n"
"  float4 col;\n"
"  if (rcOn.x) {\n"
"    float4 t0 = (textured > 0.5) ? T.Sample(S, i.uv) : float4(0, 0, 0, 1);\n"
"    col = rcEval(i.c, t0, i.fog);\n"
"  } else {\n"
"    col = i.c;\n"
"    if (textured > 0.5) col *= T.Sample(S, i.uv);\n"
"  }\n"
"  if (alphaFunc >= 0.0) {\n"
"    float a = round(col.a * 255.0); int f = (int)alphaFunc; bool ok = true;\n"
"    if (f == 0) ok = false;\n"
"    else if (f == 1) ok = a <  alphaRef;\n"
"    else if (f == 2) ok = a == alphaRef;\n"
"    else if (f == 3) ok = a <= alphaRef;\n"
"    else if (f == 4) ok = a >  alphaRef;\n"
"    else if (f == 5) ok = a != alphaRef;\n"
"    else if (f == 6) ok = a >= alphaRef;\n"
"    if (!ok) discard; }\n"
"  return col; }\n"
"struct BO { float4 pos : SV_Position; float2 uv : TEXCOORD; };\n"
"BO blit_vs(uint id : SV_VertexID) {\n"
"  BO o; float2 t = float2((id << 1) & 2, id & 2);\n"
"  o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = t; return o; }\n"
"float4 blit_ps(BO i) : SV_Target { float4 c = T.Sample(S, i.uv); return float4(c.rgb, 1); }\n";

/* The combiner evaluator reads its registers by index. With the program in
 * a constant buffer the GPU has to keep them in an indexable scratch array,
 * which made every combiner pixel slow (menus ran at ~16 fps once fog moved
 * every draw onto this path). The generic shader still works that way; each
 * program the game uses also gets its own copy with the stage words baked in
 * as constants and the loop unrolled, so every index is known at compile time
 * and the registers are plain registers. */
static const char s_generic_prefix[] =
    "#define RCST rcSt\n#define RCFIN rcFin\n#define RC_LOOP [loop]\n";

static ID3DBlob *compile_src(const char *prefix, const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    size_t pl = strlen(prefix), hl = sizeof s_hlsl - 1;
    char *src = (char *)malloc(pl + hl + 1);
    HRESULT hr;
    if (!src) return NULL;
    memcpy(src, prefix, pl); memcpy(src + pl, s_hlsl, hl); src[pl + hl] = 0;
    hr = D3DCompile(src, pl + hl, "am_d3d11", NULL, NULL,
                    entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                    &code, &err);
    free(src);
    if (FAILED(hr)) {
        fprintf(stderr, "[D3D11] shader %s failed: %s\n", entry,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        if (err) ID3D10Blob_Release(err);
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return code;
}

static ID3DBlob *compile(const char *entry, const char *target)
{
    return compile_src(s_generic_prefix, entry, target);
}

/* Per-program pixel shaders (see s_generic_prefix). */
#define MAX_RC_PS 64
typedef struct { uint32_t key[36]; ID3D11PixelShader *ps; int failed; } RcPs;
static RcPs s_rc_ps[MAX_RC_PS];
static int  s_rc_ps_n;

static ID3D11PixelShader *rc_ps_for(const uint32_t st[8][4], const uint32_t fin[4])
{
    uint32_t key[36];
    char pre[2048];
    int i, n = 0;
    ID3DBlob *blob;
    RcPs *e;
    memcpy(key, st, 32 * 4); memcpy(key + 32, fin, 16);
    for (i = 0; i < s_rc_ps_n; i++)
        if (!memcmp(s_rc_ps[i].key, key, sizeof key))
            return s_rc_ps[i].failed ? NULL : s_rc_ps[i].ps;
    if (s_rc_ps_n >= MAX_RC_PS) return NULL;
    n += snprintf(pre + n, sizeof pre - n, "static const uint4 kSt[8] = {");
    for (i = 0; i < 8; i++)
        n += snprintf(pre + n, sizeof pre - n, "%suint4(%uu,%uu,%uu,%uu)", i ? "," : "",
                      st[i][0], st[i][1], st[i][2], st[i][3]);
    n += snprintf(pre + n, sizeof pre - n,
                  "};\nstatic const uint4 kFin = uint4(%uu,%uu,%uu,%uu);\n"
                  "#define RCST kSt\n#define RCFIN kFin\n#define RC_LOOP [unroll]\n",
                  fin[0], fin[1], fin[2], fin[3]);
    e = &s_rc_ps[s_rc_ps_n++];
    memcpy(e->key, key, sizeof key);
    blob = compile_src(pre, "ps_main", "ps_5_0");
    if (blob) {
        ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(blob),
                                       ID3D10Blob_GetBufferSize(blob), NULL, &e->ps);
        ID3D10Blob_Release(blob);
    }
    e->failed = !e->ps;
    return e->ps;
}

/* ---- Window ------------------------------------------------------------- */

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CLOSE:
        ExitProcess(0);
        return 0;
    case WM_SIZE:
        s_resize_pending = 1;
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static void pump(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

static void make_back_rtv(void)
{
    ID3D11Texture2D *bb = NULL;
    if (s_back_rtv) { ID3D11RenderTargetView_Release(s_back_rtv); s_back_rtv = NULL; }
    if (SUCCEEDED(IDXGISwapChain_GetBuffer(s_swap, 0, &IID_ID3D11Texture2D, (void **)&bb))) {
        ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)bb, NULL, &s_back_rtv);
        ID3D11Texture2D_Release(bb);
    }
}

static int init(void)
{
    WNDCLASSA wc;
    RECT r = { 0, 0, 1280, 960 };
    DXGI_SWAP_CHAIN_DESC sd;
    D3D11_INPUT_ELEMENT_DESC ie[4];
    D3D11_BUFFER_DESC bd;
    D3D11_RASTERIZER_DESC rd;
    D3D11_SAMPLER_DESC smp;
    ID3DBlob *vs, *ps, *bvs, *bps;
    HRESULT hr;

    if (s_ready) return 1;
    if (s_init_failed) return 0;
    s_init_failed = 1;                 /* cleared on success */

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "AMD3D11";
    RegisterClassA(&wc);
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    s_hwnd = CreateWindowA("AMD3D11", "Auto Modellista [D3D11]", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                           NULL, NULL, wc.hInstance, NULL);
    if (!s_hwnd) return 0;

    memset(&sd, 0, sizeof sd);
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = s_hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                                       D3D11_SDK_VERSION, &sd, &s_swap, &s_dev, NULL, &s_ctx);
    if (FAILED(hr)) {
        fprintf(stderr, "[D3D11] device creation failed (0x%08lX)\n", (unsigned long)hr);
        return 0;
    }
    make_back_rtv();

    vs = compile("vs_main", "vs_5_0");  ps = compile("ps_main", "ps_5_0");
    bvs = compile("blit_vs", "vs_5_0"); bps = compile("blit_ps", "ps_5_0");
    if (!vs || !ps || !bvs || !bps) {
        fprintf(stderr, "[D3D11] shader compile failed; back end disabled\n");
        return 0;
    }
    ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vs), ID3D10Blob_GetBufferSize(vs), NULL, &s_vs);
    ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(ps), ID3D10Blob_GetBufferSize(ps), NULL, &s_ps);
    ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(bvs), ID3D10Blob_GetBufferSize(bvs), NULL, &s_blit_vs);
    ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(bps), ID3D10Blob_GetBufferSize(bps), NULL, &s_blit_ps);

    /* Matches Nv2aVertex: x y z rhw, diffuse 0xAARRGGBB, u v (texels). */
    memset(ie, 0, sizeof ie);
    ie[0].SemanticName = "POSITION"; ie[0].Format = DXGI_FORMAT_R32G32B32A32_FLOAT; ie[0].AlignedByteOffset = 0;
    ie[1].SemanticName = "COLOR";    ie[1].Format = DXGI_FORMAT_B8G8R8A8_UNORM;     ie[1].AlignedByteOffset = 16;
    ie[2].SemanticName = "TEXCOORD"; ie[2].Format = DXGI_FORMAT_R32G32_FLOAT;       ie[2].AlignedByteOffset = 20;
    ie[3].SemanticName = "FOG";      ie[3].Format = DXGI_FORMAT_R32_FLOAT;          ie[3].AlignedByteOffset = 28;
    hr = ID3D11Device_CreateInputLayout(s_dev, ie, 4, ID3D10Blob_GetBufferPointer(vs),
                                        ID3D10Blob_GetBufferSize(vs), &s_layout);
    ID3D10Blob_Release(vs); ID3D10Blob_Release(ps); ID3D10Blob_Release(bvs); ID3D10Blob_Release(bps);
    if (FAILED(hr)) return 0;

    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = VB_VERTS * sizeof(Nv2aVertex);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_vb))) return 0;
    bd.ByteWidth = sizeof(Consts);
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_cb))) return 0;
    bd.ByteWidth = sizeof(RcConsts);
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_rc_cb))) return 0;

    /* Culling is done per triangle on the CPU from projected positions (see
     * nv2a_backend.h's advice), so the rasteriser never culls. */
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs);
    rd.ScissorEnable = TRUE;
    ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs_scissor);
    if (FAILED(ID3D11DeviceContext_QueryInterface(s_ctx, &s_iid_ctx1, (void **)&s_ctx1)))
        s_ctx1 = NULL;

    memset(&smp, 0, sizeof smp);
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11Device_CreateSamplerState(s_dev, &smp, &s_blit_sampler);

    if (!s_vs || !s_ps || !s_blit_vs || !s_blit_ps || !s_rs || !s_rs_scissor || !s_blit_sampler)
        return 0;
    s_init_failed = 0;
    {
        const char *e = getenv("AM_D3D11_COMBINERS");
        s_rc_enabled = !(e && e[0] == '0');
    }
    s_ready = 1;
    fprintf(stderr, "[D3D11] back end up\n");
    return 1;
}

/* ---- Render targets (one per guest colour surface) ------------------- */

typedef struct {
    uint32_t color_va, w, h;
    ID3D11Texture2D          *tex;
    ID3D11RenderTargetView   *rtv;
    ID3D11ShaderResourceView *srv;
    ID3D11Texture2D          *ds_tex;
    ID3D11DepthStencilView   *dsv;
    uint32_t last_used;
    /* Who wrote it last: the GPU (a draw into it) or the game's CPU (its
     * guest memory changed). A surface the CPU fills -- menu art, movie
     * frames -- and the game then samples must come from guest memory; our
     * GPU copy of it was only ever cleared. */
    uint32_t gpu_frame, cpu_frame, guest_hash, hash_frame;
    int      hash_init;
    uint32_t draws_since_flip;    /* 0 at a flip: cleared but never drawn -> don't show it */
    int      sampled;             /* ever read back as a texture: off-screen, never presented */
} Target;

#define MAX_TARGETS 32
static Target   s_targets[MAX_TARGETS];
static Target  *s_bound;           /* currently bound as render target */
static Target  *s_present;         /* the surface to show at flip */
static Target  *s_last_onscreen;   /* the last flip-buffer candidate (never an off-screen one) */
/* Per-interval counters, printed every ~5 s (RECOMP_D3D11_STATS=0 silences). */
static unsigned s_st_held;
static unsigned s_st_uploads, s_st_evicts;     /* texture decodes / cache evictions */
static double   s_st_upload_ms, s_st_upload_max_ms;          /* flips that would have shown an empty frame */
static int      s_shown_once;
static unsigned s_st_draws, s_st_texreq, s_st_texok, s_st_rtt, s_st_rtt_self, s_st_rtt_guest;
static uint32_t s_frame = 1;

static uint32_t tex_hash(const Nv2aTexture *t, int full);
static int s_tex_force_full;      /* set around tex_get for CPU-fed render targets */

/* 1: sample this target from guest memory instead of the GPU copy. */
static int target_cpu_newer(Target *rt, const Nv2aTexture *tx)
{
    if (rt->hash_frame != s_frame) {
        /* Whole hash: the game may write a few glyphs into a CPU-fed
         * surface, and a sparse sample missed them (letters stayed blank). */
        uint32_t h = tex_hash(tx, 2);
        rt->hash_frame = s_frame;
        if (!rt->hash_init) { rt->hash_init = 1; rt->guest_hash = h; }
        else if (h != rt->guest_hash) { rt->guest_hash = h; rt->cpu_frame = s_frame; }
    }
    if (!rt->gpu_frame) return 1;               /* never drawn into: CPU-fed */
    return rt->cpu_frame > rt->gpu_frame;
}

static void target_free(Target *t)
{
    if (t->srv) ID3D11ShaderResourceView_Release(t->srv);
    if (t->rtv) ID3D11RenderTargetView_Release(t->rtv);
    if (t->tex) ID3D11Texture2D_Release(t->tex);
    if (t->dsv) ID3D11DepthStencilView_Release(t->dsv);
    if (t->ds_tex) ID3D11Texture2D_Release(t->ds_tex);
    memset(t, 0, sizeof *t);
}

static Target *target_find(uint32_t va)
{
    int i;
    for (i = 0; i < MAX_TARGETS; i++)
        if (s_targets[i].tex && s_targets[i].color_va == va)
            return &s_targets[i];
    return NULL;
}

static unsigned s_st_regrow;

static int target_alloc(Target *t, uint32_t w, uint32_t h)
{
    D3D11_TEXTURE2D_DESC td;

    memset(&td, 0, sizeof td);
    td.Width = w; td.Height = h;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &t->tex))) return 0;
    ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)t->tex, NULL, &t->rtv);
    ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)t->tex, NULL, &t->srv);

    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &t->ds_tex)))
        ID3D11Device_CreateDepthStencilView(s_dev, (ID3D11Resource *)t->ds_tex, NULL, &t->dsv);
    t->w = w; t->h = h;
    return 1;
}

/* Snapshot of a target for a draw that samples its own render target. */
static ID3D11Texture2D          *s_selfcp_tex;
static ID3D11ShaderResourceView *s_selfcp_srv;
static uint32_t                  s_selfcp_w, s_selfcp_h;

static ID3D11ShaderResourceView *self_copy(Target *t)
{
    if (!s_selfcp_tex || s_selfcp_w != t->w || s_selfcp_h != t->h) {   /* same size: UVs line up */
        D3D11_TEXTURE2D_DESC td;
        if (s_selfcp_srv) { ID3D11ShaderResourceView_Release(s_selfcp_srv); s_selfcp_srv = NULL; }
        if (s_selfcp_tex) { ID3D11Texture2D_Release(s_selfcp_tex); s_selfcp_tex = NULL; }
        memset(&td, 0, sizeof td);
        td.Width = t->w; td.Height = t->h;
        td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_selfcp_tex))) return NULL;
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)s_selfcp_tex, NULL, &s_selfcp_srv);
        s_selfcp_w = t->w; s_selfcp_h = t->h;
    }
    {
        D3D11_BOX box;
        box.left = 0; box.top = 0; box.front = 0;
        box.right = t->w; box.bottom = t->h; box.back = 1;
        ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)s_selfcp_tex, 0, 0, 0, 0,
                                                  (ID3D11Resource *)t->tex, 0, &box);
    }
    return s_selfcp_srv;
}

/* One target per guest colour address. Its size only grows: the surface
 * size the executor reports is the clip rectangle, which a title narrows to
 * draw a panel or a sprite, and recreating the target there wiped everything
 * drawn before it (black menus). Growing copies the old pixels across. */
static Target *target_get(const Nv2aSurface *s)
{
    Target *t = target_find(s->color_va);
    uint32_t want_w = s->clip_x + s->width, want_h = s->clip_y + s->height;
    int i, victim = 0;

    if (want_w > 4096) want_w = 4096;
    if (want_h > 4096) want_h = 4096;
    if (t && want_w <= t->w && want_h <= t->h) {
        t->last_used = s_frame;
        return t;
    }
    if (t) {                                       /* grow, keeping the contents */
        Target old = *t;
        uint32_t nw = want_w > t->w ? want_w : t->w;
        uint32_t nh = want_h > t->h ? want_h : t->h;
        D3D11_BOX box;

        if (t == s_bound) { ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL); s_bound = NULL; }
        memset(t, 0, sizeof *t);
        t->color_va = old.color_va;
        t->gpu_frame = old.gpu_frame; t->cpu_frame = old.cpu_frame;
        t->guest_hash = old.guest_hash; t->hash_frame = old.hash_frame; t->hash_init = old.hash_init;
        if (!target_alloc(t, nw, nh)) { target_free(&old); memset(t, 0, sizeof *t); return NULL; }
        box.left = 0; box.top = 0; box.front = 0;
        box.right = old.w; box.bottom = old.h; box.back = 1;
        ID3D11DeviceContext_CopySubresourceRegion(s_ctx, (ID3D11Resource *)t->tex, 0, 0, 0, 0,
                                                  (ID3D11Resource *)old.tex, 0, &box);
        target_free(&old);
        t->last_used = s_frame;
        s_st_regrow++;
        return t;
    }
    for (i = 0; i < MAX_TARGETS; i++) {
        if (!s_targets[i].tex) { victim = i; break; }
        if (s_targets[i].last_used < s_targets[victim].last_used) victim = i;
    }
    t = &s_targets[victim];
    if (t == s_bound) { ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL); s_bound = NULL; }
    if (t == s_present) s_present = NULL;
    if (t == s_last_onscreen) s_last_onscreen = NULL;
    target_free(t);
    if (!target_alloc(t, want_w, want_h)) { memset(t, 0, sizeof *t); return NULL; }
    t->color_va = s->color_va;
    t->last_used = s_frame;
    return t;
}

static void bind_target(Target *t)
{
    D3D11_VIEWPORT vp;
    if (s_bound == t) return;
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &t->rtv, t->dsv);
    memset(&vp, 0, sizeof vp);
    vp.Width = (float)t->w; vp.Height = (float)t->h; vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    s_bound = t;
}

/* The main frame buffer: the most recently used target at least 512 wide
 * (smaller ones are shadow maps, reflections and the like). */
/* Off-screen surfaces are never shown. The rule was "the last 640x480
 * surface drawn", but in races the game also renders a 640x480 off-screen
 * surface (0x822E0000) and samples it back as a texture once a frame for an
 * effect. When it happened to be drawn last before a flip, it was presented:
 * the occasional black or half-black frame. A surface the game has ever
 * sampled as a texture is off-screen; the two flip buffers never are.
 * (PCRTC_START can't tell them apart here: XDK D3D never writes it after
 * AvSetDisplayMode.) */
static void note_present_candidate(Target *t, const Nv2aSurface *s)
{
    if (t->w / (s->aa_sx ? s->aa_sx : 1) < 512) return;
    if (t->sampled) {
        static uint32_t told[8]; static int nt;
        int i, seen = 0;
        for (i = 0; i < nt; i++) if (told[i] == t->color_va) seen = 1;
        if (!seen && nt < 8) {
            told[nt++] = t->color_va;
            fprintf(stderr, "[D3D11] not presenting 0x%08X: sampled as a texture (off-screen surface)\n",
                    t->color_va);
        }
        return;
    }
    s_present = t;
    s_last_onscreen = t;
}

/* ---- Depth scale ------------------------------------------------------- */

/* Window z arrives as the title produced it: 0..65535 for a 16-bit depth
 * buffer, 0..16777215 for 24-bit. One scale for the whole run, never per
 * draw (nv2a_backend.h: rescaling per draw breaks layered depth ranges). */
static float s_zscale = 65535.0f;

static void note_depth_range(const Nv2aRenderState *rs)
{
    if (rs->depth_max > 70000.0f)
        s_zscale = 16777215.0f;
}

/* ---- Textures ----------------------------------------------------------- */

typedef struct {
    int      used;
    uint32_t offset, w, h, pitch, color, palette;
    uint32_t hash, checked_frame, last_used;
    int      dynamic;       /* changed after creation: re-checked on every draw */
    uint32_t hash_full;     /* whole-texture hash (textures <= 256 KB) */
    int      hash_full_valid;
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;    /* NULL if the format could not be decoded */
} TexEntry;

#define MAX_TEX 512
static TexEntry s_tex[MAX_TEX];
static uint32_t *s_decode_buf;
static size_t    s_decode_cap;

static int guest_readable(uint32_t va, uint32_t len)
{
    uint64_t end = (uint64_t)va + len;
    if (va >= 0x10000u && end <= 0x04000000u) return 1;
    if (va >= 0x80000000u && end <= 0x84000000u) return 1;
    return 0;
}

/* Bytes per texel for NV097 colour formats (DXT handled separately); 4 for
 * codes not listed, which can only over-hash a little memory past the end. */
static uint32_t nv_format_bpp(uint32_t f)
{
    switch (f) {
    case 0x00: case 0x01: case 0x0B: case 0x13: case 0x19: case 0x1B: case 0x1F:
        return 1;                                     /* Y8, AY8, P8, A8 (+LU) */
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x10: case 0x11: case 0x1A:
    case 0x1C: case 0x1D: case 0x20: case 0x24: case 0x25: case 0x27: case 0x28:
    case 0x29: case 0x30: case 0x31:
        return 2;                                     /* 16-bit colour/YUV/A8Y8 */
    default:
        return 4;
    }
}

/* NV097 linear colour formats (as d3d8_format_is_swizzled lists them). */
static int nv_format_linear(uint32_t f)
{
    return (f >= 0x10 && f <= 0x18) || (f >= 0x1B && f <= 0x20) ||
           (f >= 0x2E && f <= 0x31) || (f >= 0x35 && f <= 0x37) ||
           (f >= 0x3D && f <= 0x41);
}

/* Bytes the texture occupies in guest memory (level 0). */
static uint32_t tex_bytes(const Nv2aTexture *t)
{
    uint32_t blk = 0;
    if (t->color == 0x0C) blk = 8;
    else if (t->color == 0x0E || t->color == 0x0F) blk = 16;
    if (blk) return ((t->width + 3) / 4) * ((t->height + 3) / 4) * blk;
    /* Only linear (LIN_) formats use the pitch. A swizzled texture can
     * arrive with a stale pitch from an earlier linear one: the glyph sheet
     * (P8 128x128, 16 KB) came through as 2560 x 128 = 320 KB, so it was
     * treated as big, never hashed whole, and its sparse samples landed
     * mostly outside it -- new letters stayed blank. */
    if (t->pitch && nv_format_linear(t->color)) return t->pitch * t->height;
    return t->width * t->height * nv_format_bpp(t->color);
}

/* Change detection, once per frame per texture. Textures up to 256 KB are
 * hashed whole: the menu description text is written into its texture by the
 * CPU a glyph at a time, and a sparse sample (256 bytes + 64 dwords, the
 * first version) missed most of the glyphs written after the first upload --
 * those letters never appeared. Bigger textures (track, car) rarely change
 * after loading and keep a sparse sample of 1,024 points. */
#define TEX_FULL_HASH_MAX (256u * 1024u)
static uint32_t tex_hash(const Nv2aTexture *t, int full)
{
    uint32_t len = tex_bytes(t), i;
    uint64_t h = 1469598103934665603ull;
    const uint8_t *base;

    if (len < 8) len = 8;
    if (!guest_readable(t->offset, len)) return 0;
    base = (const uint8_t *)xbox_GetMemoryOffset() + t->offset;
    if (full == 2 || (full && len <= TEX_FULL_HASH_MAX)) {   /* 2: whole, any size */
        const uint64_t *q = (const uint64_t *)base;
        uint32_t n = len / 8;
        for (i = 0; i < n; i++) { h ^= q[i]; h *= 1099511628211ull; }
    } else {
        uint32_t step = (len / 1024) & ~7u;
        for (i = 0; i < 1024; i++) { h ^= *(const uint64_t *)(base + (size_t)i * step); h *= 1099511628211ull; }
    }
    if (t->palette && guest_readable(t->palette, 1024)) {
        const uint64_t *pal = (const uint64_t *)((const uint8_t *)xbox_GetMemoryOffset() + t->palette);
        for (i = 0; i < 128; i++) { h ^= pal[i]; h *= 1099511628211ull; }
    }
    return (uint32_t)(h ^ (h >> 32)) ^ len;
}

static void tex_release(TexEntry *e)
{
    if (e->srv) { ID3D11ShaderResourceView_Release(e->srv); e->srv = NULL; }
    if (e->tex) { ID3D11Texture2D_Release(e->tex); e->tex = NULL; }
}

/* Fast whole-texture decode for the formats this game uses: P8 swizzled
 * (0x0B, nearly every texture) and 32-bit ARGB, linear (0x12) or swizzled
 * (0x06/0x07). The generic path decodes through the executor's per-texel
 * sampler -- a function call, a format switch and a Morton deposit per
 * texel, 1-4 ms per texture -- and the pre-race camera pan, where ~100 new
 * textures appear within a few seconds, dropped to ~40 fps on it. This walks
 * the swizzle with the masked-increment trick (as xbox_unswizzle_rect does)
 * and returns the same values the sampler would. 0: not handled here. */
static int is_pow2(uint32_t v) { return v && !(v & (v - 1)); }

static void swz_masks(uint32_t w, uint32_t h, uint32_t *mx, uint32_t *my)
{
    uint32_t x = 0, y = 0, bit = 1, mb = 1;
    while (bit < w || bit < h) {
        if (bit < w) { x |= mb; mb <<= 1; }
        if (bit < h) { y |= mb; mb <<= 1; }
        bit <<= 1;
    }
    *mx = x; *my = y;
}

static int fast_decode(const Nv2aTexture *t, uint32_t *out)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t w = t->width, h = t->height, x, y, mx, my, oy = 0;

    switch (t->color) {
    case 0x0B: {                                     /* SZ_I8_A8R8G8B8 */
        const uint8_t *src;
        const uint32_t *pal;
        if (!is_pow2(w) || !is_pow2(h) || !t->palette) return 0;
        if (!guest_readable(t->offset, w * h) || !guest_readable(t->palette, 1024)) return 0;
        src = mem + t->offset;
        pal = (const uint32_t *)(mem + t->palette);
        swz_masks(w, h, &mx, &my);
        for (y = 0; y < h; y++) {
            uint32_t ox = 0, *row = out + (size_t)y * w;
            for (x = 0; x < w; x++) {
                row[x] = pal[src[oy + ox]];
                ox = (ox - mx) & mx;
            }
            oy = (oy - my) & my;
        }
        return 1;
    }
    case 0x06: case 0x07: {                          /* SZ_A8R8G8B8 / SZ_X8R8G8B8 */
        const uint32_t *src;
        uint32_t or_mask = t->color == 0x07 ? 0xFF000000u : 0;
        if (!is_pow2(w) || !is_pow2(h)) return 0;
        if (!guest_readable(t->offset, w * h * 4)) return 0;
        src = (const uint32_t *)(mem + t->offset);
        swz_masks(w, h, &mx, &my);
        for (y = 0; y < h; y++) {
            uint32_t ox = 0, *row = out + (size_t)y * w;
            for (x = 0; x < w; x++) {
                row[x] = src[oy + ox] | or_mask;
                ox = (ox - mx) & mx;
            }
            oy = (oy - my) & my;
        }
        return 1;
    }
    case 0x12: case 0x1E: {                          /* LIN_A8R8G8B8 / LIN_X8R8G8B8 */
        uint32_t or_mask = t->color == 0x1E ? 0xFF000000u : 0;
        if (!t->pitch || t->pitch < w * 4) return 0;
        if (!guest_readable(t->offset, t->pitch * h)) return 0;
        for (y = 0; y < h; y++) {
            const uint32_t *src = (const uint32_t *)(mem + t->offset + (size_t)y * t->pitch);
            uint32_t *row = out + (size_t)y * w;
            if (!or_mask) memcpy(row, src, (size_t)w * 4);
            else for (x = 0; x < w; x++) row[x] = src[x] | or_mask;
        }
        return 1;
    }
    }
    return 0;
}

static void tex_upload_impl(TexEntry *e, const Nv2aTexture *t);
static void tex_upload(TexEntry *e, const Nv2aTexture *t)
{
    LARGE_INTEGER a, b, f;
    double ms;
    QueryPerformanceCounter(&a);
    tex_upload_impl(e, t);
    QueryPerformanceCounter(&b);
    QueryPerformanceFrequency(&f);
    ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
    s_st_uploads++;
    s_st_upload_ms += ms;
    if (ms > s_st_upload_max_ms) s_st_upload_max_ms = ms;
}
static void tex_upload_impl(TexEntry *e, const Nv2aTexture *t)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA sd;
    size_t need = (size_t)t->width * t->height;

    tex_release(e);
    if (need > s_decode_cap) {
        free(s_decode_buf);
        s_decode_buf = (uint32_t *)malloc(need * 4);
        s_decode_cap = s_decode_buf ? need : 0;
        if (!s_decode_buf) return;
    }
    if (!fast_decode(t, s_decode_buf) && !nv2a_backend_decode_texture(t, s_decode_buf)) {
        /* Once per format code: what the back end cannot show. */
        static uint32_t told[64]; static int ntold;
        int k, seen = 0;
        for (k = 0; k < ntold; k++) if (told[k] == t->color) seen = 1;
        if (!seen && ntold < 64) {
            told[ntold++] = t->color;
            fprintf(stderr, "[D3D11] texture decode failed: format 0x%02X %ux%u"
                    " pitch %u offset 0x%08X palette 0x%08X\n", t->color,
                    t->width, t->height, t->pitch, t->offset, t->palette);
        }
        return;
    }

    memset(&td, 0, sizeof td);
    td.Width = t->width; td.Height = t->height;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;   /* 0xAARRGGBB little-endian */
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sd.pSysMem = s_decode_buf;
    sd.SysMemPitch = t->width * 4;
    sd.SysMemSlicePitch = 0;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &td, &sd, &e->tex)))
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)e->tex, NULL, &e->srv);
}

static ID3D11ShaderResourceView *tex_get(const Nv2aTexture *t)
{
    TexEntry *e = NULL;
    int i, free_slot = -1, lru = 0;

    if (!t->width || !t->height || t->width > 4096 || t->height > 4096)
        return NULL;
    for (i = 0; i < MAX_TEX; i++) {
        TexEntry *c = &s_tex[i];
        if (!c->used) { if (free_slot < 0) free_slot = i; continue; }
        if (c->offset == t->offset && c->w == t->width && c->h == t->height
            && c->color == t->color && c->pitch == t->pitch
            && c->palette == t->palette) { e = c; break; }
        if (c->last_used < s_tex[lru].last_used || !s_tex[lru].used) lru = i;
    }
    if (e) {
        /* Dynamic textures (the game rewrote them after the first upload --
         * text it renders a string at a time, often several strings through
         * one texture in a frame) are hashed whole on every draw. The rest
         * get a sparse hash once a frame and a whole one every 32 frames,
         * staggered by slot, so a static menu doesn't pay to re-hash every
         * background every frame (menus fell from 60 to 40 fps when it did). */
        int slot = (int)(e - s_tex);
        /* Text is drawn from a glyph sheet the game fills in as it needs
         * letters. The sheet is bigger than TEX_FULL_HASH_MAX, so it only
         * ever got the sparse check, which steps over a newly drawn letter:
         * the letter stayed blank until the menu was re-entered. Now the
         * periodic check hashes the whole texture whatever its size, and a
         * texture that has changed once (dynamic) is hashed whole every
         * frame -- every draw if it is small. CPU-fed render targets
         * (s_tex_force_full) are checked whole on every draw. */
        int big = tex_bytes(t) > TEX_FULL_HASH_MAX;
        int recheck = s_tex_force_full || (e->dynamic && !big) || e->checked_frame != s_frame;
        if (recheck) {
            int full = s_tex_force_full || e->dynamic || ((s_frame + (uint32_t)slot) & 31u) == 0;
            uint32_t h;
            h = tex_hash(t, full ? 2 : 0);
            e->checked_frame = s_frame;
            if (full) {
                if (h != e->hash_full) {
                    if (e->hash_full_valid && !e->dynamic) {
                        static int told;
                        e->dynamic = 1;
                        if (told++ < 40)
                            fprintf(stderr, "[D3D11] dynamic texture 0x%08X %ux%u fmt 0x%02X pitch %u (%u bytes)\n",
                                    t->offset, t->width, t->height, t->color, t->pitch, tex_bytes(t));
                    }
                    e->hash_full = h; e->hash_full_valid = 1;
                    tex_upload(e, t);
                    e->hash = tex_hash(t, 0);
                }
            } else if (h != e->hash) {
                e->hash = h;
                e->dynamic = 1;
                tex_upload(e, t);
                e->hash_full = tex_hash(t, 2); e->hash_full_valid = 1;
            }
        }
        e->last_used = s_frame;
        return e->srv;
    }
    e = &s_tex[free_slot >= 0 ? free_slot : lru];
    if (free_slot < 0) s_st_evicts++;
    tex_release(e);
    memset(e, 0, sizeof *e);
    e->used = 1;
    e->offset = t->offset; e->w = t->width; e->h = t->height;
    e->color = t->color; e->pitch = t->pitch; e->palette = t->palette;
    e->hash = tex_hash(t, 0);
    e->hash_full = tex_hash(t, 2); e->hash_full_valid = 1;
    e->checked_frame = e->last_used = s_frame;
    tex_upload(e, t);
    return e->srv;
}

/* ---- State objects (small caches) --------------------------------------- */

static D3D11_BLEND map_blend(uint32_t f)
{
    switch (f) {
    case 0:      return D3D11_BLEND_ZERO;
    case 1:      return D3D11_BLEND_ONE;
    case 0x300:  return D3D11_BLEND_SRC_COLOR;
    case 0x301:  return D3D11_BLEND_INV_SRC_COLOR;
    case 0x302:  return D3D11_BLEND_SRC_ALPHA;
    case 0x303:  return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x304:  return D3D11_BLEND_DEST_ALPHA;
    case 0x305:  return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x306:  return D3D11_BLEND_DEST_COLOR;
    case 0x307:  return D3D11_BLEND_INV_DEST_COLOR;
    case 0x308:  return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    default:     return D3D11_BLEND_ONE;
    }
}

static D3D11_BLEND_OP map_blend_op(uint32_t e)
{
    switch (e) {
    case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX;
    default:     return D3D11_BLEND_OP_ADD;
    }
}

static D3D11_COMPARISON_FUNC map_cmp(uint32_t f)
{
    if (f >= 0x200 && f <= 0x207)
        return (D3D11_COMPARISON_FUNC)(f - 0x200 + 1);   /* NEVER..ALWAYS line up */
    return D3D11_COMPARISON_ALWAYS;
}

static D3D11_STENCIL_OP map_stencil_op(uint32_t o)
{
    switch (o) {
    case 0:      return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    default:     return D3D11_STENCIL_OP_KEEP;
    }
}

static UINT8 map_color_mask(uint32_t m)
{
    UINT8 r = 0;
    if (m & 0x00010000) r |= D3D11_COLOR_WRITE_ENABLE_RED;
    if (m & 0x00000100) r |= D3D11_COLOR_WRITE_ENABLE_GREEN;
    if (m & 0x00000001) r |= D3D11_COLOR_WRITE_ENABLE_BLUE;
    if (m & 0x01000000) r |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    return r;
}

typedef struct { uint32_t en, src, dst, eq, mask; ID3D11BlendState *st; } BlendKey;
static BlendKey s_blends[128]; static int s_nblends;

static ID3D11BlendState *blend_state(const Nv2aRenderState *rs)
{
    D3D11_BLEND_DESC bd;
    uint32_t en = rs->blend_enable ? 1 : 0;
    UINT8 mask = map_color_mask(rs->color_mask);
    int i;

    for (i = 0; i < s_nblends; i++) {
        BlendKey *k = &s_blends[i];
        if (k->en == en && k->mask == mask
            && (!en || (k->src == rs->blend_src && k->dst == rs->blend_dst && k->eq == rs->blend_eq)))
            return k->st;
    }
    memset(&bd, 0, sizeof bd);
    bd.RenderTarget[0].BlendEnable = en;
    bd.RenderTarget[0].SrcBlend = bd.RenderTarget[0].SrcBlendAlpha = map_blend(rs->blend_src);
    bd.RenderTarget[0].DestBlend = bd.RenderTarget[0].DestBlendAlpha = map_blend(rs->blend_dst);
    /* Colour factors are not valid for the alpha channel in D3D11. */
    {
        D3D11_BLEND *a[2] = { &bd.RenderTarget[0].SrcBlendAlpha, &bd.RenderTarget[0].DestBlendAlpha };
        int j;
        for (j = 0; j < 2; j++) {
            if (*a[j] == D3D11_BLEND_SRC_COLOR) *a[j] = D3D11_BLEND_SRC_ALPHA;
            else if (*a[j] == D3D11_BLEND_INV_SRC_COLOR) *a[j] = D3D11_BLEND_INV_SRC_ALPHA;
            else if (*a[j] == D3D11_BLEND_DEST_COLOR) *a[j] = D3D11_BLEND_DEST_ALPHA;
            else if (*a[j] == D3D11_BLEND_INV_DEST_COLOR) *a[j] = D3D11_BLEND_INV_DEST_ALPHA;
        }
    }
    bd.RenderTarget[0].BlendOp = bd.RenderTarget[0].BlendOpAlpha = map_blend_op(rs->blend_eq);
    bd.RenderTarget[0].RenderTargetWriteMask = mask;
    if (s_nblends == 128) { ID3D11BlendState_Release(s_blends[0].st); memmove(s_blends, s_blends + 1, sizeof s_blends[0] * 127); s_nblends--; }
    {
        BlendKey *k = &s_blends[s_nblends];
        k->en = en; k->src = rs->blend_src; k->dst = rs->blend_dst; k->eq = rs->blend_eq; k->mask = mask;
        if (FAILED(ID3D11Device_CreateBlendState(s_dev, &bd, &k->st))) return NULL;
        s_nblends++;
        return k->st;
    }
}

typedef struct {
    uint32_t dt, df, dw, se, sf, srm, swm, sfail, szf, szp;
    ID3D11DepthStencilState *st;
} DepthKey;
static DepthKey s_depths[128]; static int s_ndepths;

static ID3D11DepthStencilState *depth_state(const Nv2aRenderState *rs, int has_depth)
{
    D3D11_DEPTH_STENCIL_DESC dd;
    DepthKey k;
    int i;

    memset(&k, 0, sizeof k);
    k.dt = has_depth && rs->depth_test_enable;
    k.df = rs->depth_func; k.dw = has_depth && rs->depth_write;
    k.se = has_depth && rs->stencil_enable;
    if (k.se) {
        k.sf = rs->stencil_func; k.srm = rs->stencil_func_mask & 0xFF; k.swm = rs->stencil_write_mask & 0xFF;
        k.sfail = rs->stencil_fail; k.szf = rs->stencil_zfail; k.szp = rs->stencil_zpass;
    }
    for (i = 0; i < s_ndepths; i++)
        if (!memcmp(&s_depths[i], &k, offsetof(DepthKey, st)))
            return s_depths[i].st;

    memset(&dd, 0, sizeof dd);
    /* Depth writes on the NV2A happen even with the test off (D3D's ZENABLE
     * off but ZWRITEENABLE on); emulate with an always-pass test. */
    dd.DepthEnable = k.dt || k.dw;
    dd.DepthFunc = k.dt ? map_cmp(k.df) : D3D11_COMPARISON_ALWAYS;
    dd.DepthWriteMask = k.dw ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.StencilEnable = k.se;
    dd.StencilReadMask = (UINT8)k.srm; dd.StencilWriteMask = (UINT8)k.swm;
    dd.FrontFace.StencilFunc = map_cmp(k.sf);
    dd.FrontFace.StencilFailOp = map_stencil_op(k.sfail);
    dd.FrontFace.StencilDepthFailOp = map_stencil_op(k.szf);
    dd.FrontFace.StencilPassOp = map_stencil_op(k.szp);
    dd.BackFace = dd.FrontFace;
    if (s_ndepths == 128) { ID3D11DepthStencilState_Release(s_depths[0].st); memmove(s_depths, s_depths + 1, sizeof s_depths[0] * 127); s_ndepths--; }
    k.st = NULL;
    if (FAILED(ID3D11Device_CreateDepthStencilState(s_dev, &dd, &k.st))) return NULL;
    s_depths[s_ndepths++] = k;
    return k.st;
}

typedef struct { uint32_t u, v, linear; ID3D11SamplerState *st; } SampKey;
static SampKey s_samps[32]; static int s_nsamps;

static D3D11_TEXTURE_ADDRESS_MODE map_addr(uint32_t a)
{
    switch (a) {
    case 1:  return D3D11_TEXTURE_ADDRESS_WRAP;
    case 2:  return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 4:  return D3D11_TEXTURE_ADDRESS_BORDER;
    default: return D3D11_TEXTURE_ADDRESS_CLAMP;
    }
}

static ID3D11SamplerState *sampler(const Nv2aTexture *t)
{
    D3D11_SAMPLER_DESC sd;
    uint32_t linear = ((t->filter >> 24) & 0xFF) != 1;   /* MAG: 1 = nearest */
    int i;

    for (i = 0; i < s_nsamps; i++)
        if (s_samps[i].u == t->addr_u && s_samps[i].v == t->addr_v && s_samps[i].linear == linear)
            return s_samps[i].st;
    if (s_nsamps == 32) return s_blit_sampler;
    memset(&sd, 0, sizeof sd);
    sd.Filter = linear ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = map_addr(t->addr_u);
    sd.AddressV = map_addr(t->addr_v);
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    s_samps[s_nsamps].u = t->addr_u; s_samps[s_nsamps].v = t->addr_v; s_samps[s_nsamps].linear = linear;
    if (FAILED(ID3D11Device_CreateSamplerState(s_dev, &sd, &s_samps[s_nsamps].st))) return s_blit_sampler;
    return s_samps[s_nsamps++].st;
}

/* ---- Back-end callbacks -------------------------------------------------- */

static void be_draw(const Nv2aSurface *s, const Nv2aBatch *b);

static void be_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    Target *t;
    int full;
    if (!init() || !s->color_va || !s->width || !s->height) return;
    t = target_get(s);
    if (!t) return;
    note_depth_range(rs);
    note_present_candidate(t, s);
    full = (s->clear_x1 <= s->clear_x0 || s->clear_y1 <= s->clear_y0)   /* not given */
        || (s->clear_x0 == 0 && s->clear_y0 == 0
            && s->clear_x1 >= t->w && s->clear_y1 >= t->h);
    if (flags & 0xF0) {
        float c[4];
        c[0] = ((argb >> 16) & 0xFF) / 255.0f;
        c[1] = ((argb >> 8) & 0xFF) / 255.0f;
        c[2] = (argb & 0xFF) / 255.0f;
        c[3] = ((argb >> 24) & 0xFF) / 255.0f;
        if (!full && s_ctx1) {
            D3D11_RECT r;
            r.left = (LONG)s->clear_x0; r.top = (LONG)s->clear_y0;
            r.right = (LONG)s->clear_x1; r.bottom = (LONG)s->clear_y1;
            ID3D11DeviceContext1_ClearView(s_ctx1, (ID3D11View *)t->rtv, c, &r, 1);
        } else
            ID3D11DeviceContext_ClearRenderTargetView(s_ctx, t->rtv, c);
    }
    /* D3D11 has no rect clear for depth/stencil: a partial one is drawn as a
     * quad over the clear rectangle that writes only depth (and stencil),
     * test ALWAYS, colour writes off. Skipping it left stale depth in the
     * menu text box and glyphs failed the depth test (missing letters). */
    if ((flags & 0x3) && t->dsv && !full) {
        Nv2aSurface qs = *s;
        Nv2aRenderState q;
        Nv2aVertex v[6];
        Nv2aBatch qb;
        float depth = s_zscale > 70000.0f ? (float)(zstencil >> 8) / 16777215.0f
                                          : (float)(zstencil & 0xFFFF) / 65535.0f;
        float x0 = (float)s->clear_x0, y0 = (float)s->clear_y0;
        float x1 = (float)s->clear_x1, y1 = (float)s->clear_y1;
        int k;

        memset(&q, 0, sizeof q);
        q.depth_test_enable = 1; q.depth_func = 0x207;      /* GL_ALWAYS */
        q.depth_write = (flags & 0x1) ? 1 : 0;
        q.color_mask = 0;                                    /* no colour */
        q.zeta_va = rs->zeta_va ? rs->zeta_va : 1;
        q.depth_min = rs->depth_min; q.depth_max = rs->depth_max;
        q.blend_src = 1; q.blend_dst = 0; q.blend_eq = 0x8006;
        if (flags & 0x2) {
            q.stencil_enable = 1; q.stencil_func = 0x207; q.stencil_ref = zstencil & 0xFF;
            q.stencil_func_mask = 0xFF; q.stencil_write_mask = 0xFF;
            q.stencil_fail = q.stencil_zfail = q.stencil_zpass = 0x1E01;   /* REPLACE */
        }
        qs.clip_x = s->clear_x0; qs.clip_y = s->clear_y0;
        qs.width = s->clear_x1 - s->clear_x0; qs.height = s->clear_y1 - s->clear_y0;
        memset(v, 0, sizeof v);
        v[0].x = x0; v[0].y = y0; v[1].x = x1; v[1].y = y0; v[2].x = x0; v[2].y = y1;
        v[3].x = x1; v[3].y = y0; v[4].x = x1; v[4].y = y1; v[5].x = x0; v[5].y = y1;
        for (k = 0; k < 6; k++) { v[k].z = depth * s_zscale; v[k].rhw = 1.0f; v[k].fog = 1.0f; }
        qb.vertices = v; qb.count = 6; qb.texture = NULL; qb.state = &q; qb.combiner = NULL; qb.fog_color = 0;
        be_draw(&qs, &qb);
    }
    if ((flags & 0x3) && t->dsv && full) {
        UINT f = 0;
        float depth;
        if (flags & 0x1) f |= D3D11_CLEAR_DEPTH;
        if (flags & 0x2) f |= D3D11_CLEAR_STENCIL;
        depth = s_zscale > 70000.0f ? (float)(zstencil >> 8) / 16777215.0f
                                    : (float)(zstencil & 0xFFFF) / 65535.0f;
        ID3D11DeviceContext_ClearDepthStencilView(s_ctx, t->dsv, f, depth, (UINT8)(zstencil & 0xFF));
    }
}

/* Front-facing test from projected screen positions (y down). */
static int culled(const Nv2aVertex *v, const Nv2aRenderState *rs)
{
    float area;
    int cw, front;
    if (!rs->cull_enable) return 0;
    area = (v[1].x - v[0].x) * (v[2].y - v[0].y) - (v[2].x - v[0].x) * (v[1].y - v[0].y);
    if (area == 0.0f) return 1;
    cw = area > 0.0f;                         /* clockwise on screen */
    front = (rs->front_face == 0x900) ? cw : !cw;   /* GL_CW 0x900, GL_CCW 0x901 */
    switch (rs->cull_face) {
    case 0x404: return front;                 /* GL_FRONT */
    case 0x408: return 1;                     /* GL_FRONT_AND_BACK */
    default:    return !front;                /* GL_BACK */
    }
}

static void be_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    Target *t;
    const Nv2aRenderState *rs = b->state;
    ID3D11ShaderResourceView *srv = NULL;
    ID3D11SamplerState *samp = s_blit_sampler;
    D3D11_MAPPED_SUBRESOURCE m;
    Consts c;
    float bf[4];
    UINT stride = sizeof(Nv2aVertex), offset = 0;
    uint32_t i;

    if (!init() || !b->count || !s->color_va || !s->width || !s->height) return;
    t = target_get(s);
    if (!t) return;
    t->gpu_frame = s_frame;
    t->draws_since_flip++;
    note_depth_range(rs);
    note_present_candidate(t, s);

    memset(&c, 0, sizeof c);
    c.rt_w = (float)t->w; c.rt_h = (float)t->h;
    c.zscale = s_zscale;
    c.tex_w = c.tex_h = 1.0f;
    c.alpha_func = rs->alpha_test_enable ? (float)((int)rs->alpha_func - 0x200) : -1.0f;
    if (c.alpha_func < -0.5f || c.alpha_func > 7.5f) c.alpha_func = -1.0f;
    c.alpha_ref = (float)(rs->alpha_ref & 0xFF);

    s_st_draws++;
    if (b->texture) {
        Target *rt = target_find(b->texture->offset);     /* render-to-texture */
        s_st_texreq++;
        if (rt && !rt->sampled) {
            rt->sampled = 1;
            if (s_present == rt) s_present = s_last_onscreen;   /* back to the flip buffer */
        }
        if (rt) {   /* once per surface: what the game samples from a render target */
            static uint32_t seen[32]; static int ns;
            int q, f = 0;
            for (q = 0; q < ns; q++) if (seen[q] == b->texture->offset) f = 1;
            if (!f && ns < 32) {
                seen[ns++] = b->texture->offset;
                fprintf(stderr, "[D3D11] rtt source 0x%08X tex %ux%u fmt 0x%02X pitch %u, target %ux%u%s\n",
                        b->texture->offset, b->texture->width, b->texture->height, b->texture->color,
                        b->texture->pitch, rt->w, rt->h, rt == t ? " (self)" : "");
            }
        }
        if (rt && target_cpu_newer(rt, b->texture)) {
            s_tex_force_full = 1;   /* CPU-fed surface: small edits, check it whole */
            srv = tex_get(b->texture); s_st_rtt_guest++;
            s_tex_force_full = 0;
        }
        else if (rt && rt != t) { srv = rt->srv; s_st_rtt++; }
        else if (rt) {
            /* The draw reads the surface it is drawing into (a menu effect
             * that shrinks the screen into a corner, or feedback). D3D11
             * can't bind one texture as both, so the draw went untextured:
             * a flicker in that corner. Read from a snapshot instead. */
            srv = self_copy(rt);
            s_st_rtt_self++;
        }
        else srv = tex_get(b->texture);
        if (srv) s_st_texok++;
        if (srv) {
            c.textured = 1.0f;
            c.tex_w = (float)(b->texture->width ? b->texture->width : 1);
            c.tex_h = (float)(b->texture->height ? b->texture->height : 1);
            samp = sampler(b->texture);
        }
    }

    bind_target(t);
    ID3D11DeviceContext_IASetInputLayout(s_ctx, s_layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vb, &stride, &offset);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_ps, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &s_cb);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_cb);
    {
        /* Combiner program: re-uploaded only when it changes. */
        RcConsts rc;
        const Nv2aBackendCombiner *bc = b->combiner;
        int k;

        memset(&rc, 0, sizeof rc);
        if (bc) {
            /* Each distinct program once (up to 48): what the game asks of
             * the combiners, and whether this back end can run it yet. */
            static uint32_t seen[48]; static int nseen;
            uint32_t h = 2166136261u; int q, dup = 0;
            const uint32_t *w = (const uint32_t *)bc;
            for (q = 0; q < (int)(sizeof *bc / 4); q++) { h ^= w[q]; h *= 16777619u; }
            for (q = 0; q < nseen; q++) if (seen[q] == h) dup = 1;
            if (!dup && nseen < 48) {
                uint32_t st, n = bc->control & 0xFF;
                seen[nseen++] = h;
                fprintf(stderr, "[RC] prog %08X %s: stages %u control %08X stage_prog %08X final %08X %08X fc %08X %08X\n",
                        h, rc_supported(bc) ? "ok" : "FALLBACK", n, bc->control, bc->stage_program,
                        bc->final0, bc->final1, bc->final_c0, bc->final_c1);
                for (st = 0; st < n && st < 8; st++)
                    fprintf(stderr, "[RC]   %u: cI %08X aI %08X cO %08X aO %08X f0 %08X f1 %08X\n", st,
                            bc->color_icw[st], bc->alpha_icw[st], bc->color_ocw[st], bc->alpha_ocw[st],
                            bc->factor0[st], bc->factor1[st]);
            }
        }
        if (bc && s_rc_enabled && !rc_supported(bc)) {
            s_st_rc_skip++;
            bc = NULL;
        }
        if (bc && s_rc_enabled) {
            for (k = 0; k < 8; k++) {
                rc.st[k][0] = bc->color_icw[k]; rc.st[k][1] = bc->alpha_icw[k];
                rc.st[k][2] = bc->color_ocw[k]; rc.st[k][3] = bc->alpha_ocw[k];
                unpack_argb(bc->factor0[k], rc.f0[k]);
                unpack_argb(bc->factor1[k], rc.f1[k]);
            }
            rc.fin[0] = bc->final0; rc.fin[1] = bc->final1;
            rc.fin[2] = bc->control; rc.fin[3] = bc->stage_program;
            unpack_argb(bc->final_c0, rc.fc0);
            unpack_argb(bc->final_c1, rc.fc1);
            unpack_argb(b->fog_color, rc.fog);
            rc.on[0] = 1;
            s_st_rc++;
        }
        if (!s_rc_last_valid || memcmp(&rc, &s_rc_last, sizeof rc)) {
            D3D11_MAPPED_SUBRESOURCE rm;
            if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_rc_cb, 0,
                                                  D3D11_MAP_WRITE_DISCARD, 0, &rm))) {
                memcpy(rm.pData, &rc, sizeof rc);
                ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_rc_cb, 0);
                s_rc_last = rc; s_rc_last_valid = 1;
            }
        }
        ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 1, 1, &s_rc_cb);
        {
            ID3D11PixelShader *sp = rc.on[0] ? rc_ps_for((const uint32_t (*)[4])rc.st, rc.fin) : NULL;
            ID3D11DeviceContext_PSSetShader(s_ctx, sp ? sp : s_ps, NULL, 0);
        }
    }
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &srv);
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, &samp);
    {
        D3D11_RECT sc;
        sc.left = (LONG)s->clip_x; sc.top = (LONG)s->clip_y;
        sc.right = (LONG)(s->clip_x + s->width); sc.bottom = (LONG)(s->clip_y + s->height);
        ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, &sc);
        ID3D11DeviceContext_RSSetState(s_ctx, s_rs_scissor);
    }

    bf[0] = ((rs->blend_color >> 16) & 0xFF) / 255.0f;
    bf[1] = ((rs->blend_color >> 8) & 0xFF) / 255.0f;
    bf[2] = (rs->blend_color & 0xFF) / 255.0f;
    bf[3] = ((rs->blend_color >> 24) & 0xFF) / 255.0f;
    if (rs->blend_src == 0x8003 || rs->blend_src == 0x8004 || rs->blend_dst == 0x8003 || rs->blend_dst == 0x8004)
        bf[0] = bf[1] = bf[2] = bf[3];        /* CONSTANT_ALPHA: alpha in every channel */
    ID3D11DeviceContext_OMSetBlendState(s_ctx, blend_state(rs), bf, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, depth_state(rs, t->dsv != NULL && rs->zeta_va != 0),
                                               rs->stencil_ref & 0xFF);

    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &c, sizeof c);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_cb, 0);
    }

    /* Copy surviving triangles into the ring buffer and draw, in chunks.
     * Each pass consumes at least one triangle, so this always ends. */
    i = 0;
    while (i + 2 < b->count) {
        D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
        uint32_t room, n = 0, first;
        Nv2aVertex *dst;

        if (s_vb_pos + 3 > VB_VERTS || s_vb_pos == 0) {
            s_vb_pos = 0;
            mode = D3D11_MAP_WRITE_DISCARD;
        }
        room = VB_VERTS - s_vb_pos;
        if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vb, 0, mode, 0, &m))) return;
        dst = (Nv2aVertex *)m.pData + s_vb_pos;
        for (; i + 2 < b->count && n + 3 <= room; i += 3) {
            if (culled(&b->vertices[i], rs)) continue;
            memcpy(dst + n, &b->vertices[i], 3 * sizeof(Nv2aVertex));
            n += 3;
        }
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vb, 0);
        first = s_vb_pos;
        s_vb_pos += n;
        if (n) ID3D11DeviceContext_Draw(s_ctx, n, first);
    }
}

static void be_flip(void)
{
    static DWORD fps_t0;
    static unsigned fps_frames;
    RECT cr;
    D3D11_VIEWPORT vp;
    float black[4] = { 0, 0, 0, 1 };
    ID3D11ShaderResourceView *none = NULL;

    if (!init()) return;
    pump();
    if (s_resize_pending) {
        s_resize_pending = 0;
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
        s_bound = NULL;
        if (s_back_rtv) { ID3D11RenderTargetView_Release(s_back_rtv); s_back_rtv = NULL; }
        IDXGISwapChain_ResizeBuffers(s_swap, 0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
        make_back_rtv();
    }
    if (!s_back_rtv) return;

    GetClientRect(s_hwnd, &cr);
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s_back_rtv, NULL);
    s_bound = NULL;
    ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s_back_rtv, black);

    /* A flip whose surface got no draws since the last flip would show a
     * cleared, empty frame: the occasional one-frame black flash seen in
     * races. Keep the previous image on screen instead (skip this Present). */
    if (s_present && !s_present->draws_since_flip && s_shown_once) {
        int i;
        s_st_held++;
        for (i = 0; i < MAX_TARGETS; i++) s_targets[i].draws_since_flip = 0;
        s_frame++;
        return;
    }
    if (s_present && cr.right > 0 && cr.bottom > 0) {
        s_shown_once = 1;
        /* Letterbox to 4:3. */
        float W = (float)cr.right, H = (float)cr.bottom, w = W, h = W * 3.0f / 4.0f;
        if (h > H) { h = H; w = H * 4.0f / 3.0f; }
        memset(&vp, 0, sizeof vp);
        vp.TopLeftX = (W - w) * 0.5f; vp.TopLeftY = (H - h) * 0.5f;
        vp.Width = w; vp.Height = h; vp.MaxDepth = 1.0f;
        ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
        ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
        ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(s_ctx, s_blit_vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(s_ctx, s_blit_ps, NULL, 0);
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &s_present->srv);
        ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, &s_blit_sampler);
        ID3D11DeviceContext_RSSetState(s_ctx, s_rs);
        ID3D11DeviceContext_OMSetBlendState(s_ctx, NULL, NULL, 0xFFFFFFFF);
        ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, NULL, 0);
        ID3D11DeviceContext_Draw(s_ctx, 3, 0);
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &none);
    }
    IDXGISwapChain_Present(s_swap, 0, 0);
    {
        int i;
        for (i = 0; i < MAX_TARGETS; i++) s_targets[i].draws_since_flip = 0;
    }
    s_frame++;

    fps_frames++;
    if (!fps_t0) fps_t0 = GetTickCount();
    if (GetTickCount() - fps_t0 >= 1000) {
        char title[96];
        snprintf(title, sizeof title, "Auto Modellista [D3D11] | FPS: %.1f",
                 fps_frames * 1000.0 / (GetTickCount() - fps_t0));
        SetWindowTextA(s_hwnd, title);
        {
            static int stats = -1; static unsigned tick;
            if (stats < 0) { const char *e = getenv("RECOMP_D3D11_STATS"); stats = !(e && e[0] == '0'); }
            if (stats && ++tick % 5 == 0) {
                int i, nt = 0;
                for (i = 0; i < MAX_TARGETS; i++) if (s_targets[i].tex) nt++;
                fprintf(stderr, "[D3D11] %u draws (%u combiner, %u fallback), textured %u/%u (rtt %u, self %u, guest %u), %u regrows,"
                        " %d targets, present 0x%08X %ux%u, %u empty flips held,"
                        " %u tex uploads (%.0f ms total, worst %.1f ms), %u evictions\n",
                        s_st_draws, s_st_rc, s_st_rc_skip, s_st_texok, s_st_texreq, s_st_rtt, s_st_rtt_self, s_st_rtt_guest, s_st_regrow, nt,
                        s_present ? s_present->color_va : 0,
                        s_present ? s_present->w : 0, s_present ? s_present->h : 0, s_st_held,
                        s_st_uploads, s_st_upload_ms, s_st_upload_max_ms, s_st_evicts);
                s_st_uploads = s_st_evicts = 0; s_st_upload_ms = s_st_upload_max_ms = 0.0;
                s_st_draws = s_st_texreq = s_st_texok = s_st_rtt = s_st_rtt_self = s_st_rtt_guest = s_st_regrow = s_st_rc = s_st_rc_skip = s_st_held = 0;
            }
        }
        fps_t0 = GetTickCount();
        fps_frames = 0;
    }
}

static const Nv2aBackend s_backend = { be_clear, be_draw, be_flip };

void am_d3d11_register(void)
{
    nv2a_backend_register(&s_backend);
    fprintf(stderr, "[D3D11] back end registered (AM_D3D11=1)\n");
}
