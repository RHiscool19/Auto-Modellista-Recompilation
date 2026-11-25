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
#include <d3d11.h>
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
typedef char nv2a_vertex_is_28_bytes[(sizeof(Nv2aVertex) == 28) ? 1 : -1];

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
static ID3D11SamplerState *s_blit_sampler;

#define VB_VERTS 262144u           /* ring capacity in vertices (7 MB) */
static UINT s_vb_pos;

typedef struct {
    float rt_w, rt_h, zscale, textured;
    float alpha_func, alpha_ref, tex_w, tex_h;
} Consts;

/* ---- Shaders ------------------------------------------------------------ */

static const char s_hlsl[] =
"cbuffer C : register(b0) {\n"
"  float2 rtSize; float zscale; float textured;\n"
"  float alphaFunc; float alphaRef; float2 texSize; };\n"
"Texture2D T : register(t0); SamplerState S : register(s0);\n"
"struct VI { float4 p : POSITION; float4 c : COLOR; float2 uv : TEXCOORD; };\n"
"struct PI { float4 pos : SV_Position; float4 c : COLOR; float2 uv : TEXCOORD; };\n"
"PI vs_main(VI i) {\n"
"  PI o;\n"
"  float w = i.p.w > 0.0 ? 1.0 / i.p.w : 1.0;\n"
"  float x = i.p.x / rtSize.x * 2.0 - 1.0;\n"
"  float y = 1.0 - i.p.y / rtSize.y * 2.0;\n"
"  float z = saturate(i.p.z / zscale);\n"
"  o.pos = float4(x * w, y * w, z * w, w);\n"
"  o.c = i.c; o.uv = i.uv / texSize;\n"
"  return o; }\n"
"float4 ps_main(PI i) : SV_Target {\n"
"  float4 col = i.c;\n"
"  if (textured > 0.5) col *= T.Sample(S, i.uv);\n"
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

static ID3DBlob *compile(const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(s_hlsl, sizeof s_hlsl - 1, "am_d3d11", NULL, NULL,
                            entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                            &code, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "[D3D11] shader %s failed: %s\n", entry,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        if (err) ID3D10Blob_Release(err);
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return code;
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
    D3D11_INPUT_ELEMENT_DESC ie[3];
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
    hr = ID3D11Device_CreateInputLayout(s_dev, ie, 3, ID3D10Blob_GetBufferPointer(vs),
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

    /* Culling is done per triangle on the CPU from projected positions (see
     * nv2a_backend.h's advice), so the rasteriser never culls. */
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs);

    memset(&smp, 0, sizeof smp);
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11Device_CreateSamplerState(s_dev, &smp, &s_blit_sampler);

    if (!s_vs || !s_ps || !s_blit_vs || !s_blit_ps || !s_rs || !s_blit_sampler)
        return 0;
    s_init_failed = 0;
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
} Target;

#define MAX_TARGETS 32
static Target   s_targets[MAX_TARGETS];
static Target  *s_bound;           /* currently bound as render target */
static Target  *s_present;         /* the surface to show at flip */
/* Per-interval counters, printed every ~5 s (RECOMP_D3D11_STATS=0 silences). */
static unsigned s_st_draws, s_st_texreq, s_st_texok, s_st_rtt, s_st_rtt_self, s_st_rtt_guest;
static uint32_t s_frame = 1;

static uint32_t tex_hash(const Nv2aTexture *t);

/* 1: sample this target from guest memory instead of the GPU copy. */
static int target_cpu_newer(Target *rt, const Nv2aTexture *tx)
{
    if (rt->hash_frame != s_frame) {
        uint32_t h = tex_hash(tx);
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

static Target *target_get(const Nv2aSurface *s)
{
    Target *t = target_find(s->color_va);
    D3D11_TEXTURE2D_DESC td;
    int i, victim = 0;

    if (t && t->w == s->width && t->h == s->height) {
        t->last_used = s_frame;
        return t;
    }
    if (!t) {
        for (i = 0; i < MAX_TARGETS; i++) {
            if (!s_targets[i].tex) { victim = i; break; }
            if (s_targets[i].last_used < s_targets[victim].last_used) victim = i;
        }
        t = &s_targets[victim];
    }
    if (t == s_bound) { ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL); s_bound = NULL; }
    if (t == s_present) s_present = NULL;
    target_free(t);

    memset(&td, 0, sizeof td);
    td.Width = s->width; td.Height = s->height;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &t->tex))) return NULL;
    ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)t->tex, NULL, &t->rtv);
    ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)t->tex, NULL, &t->srv);

    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &t->ds_tex)))
        ID3D11Device_CreateDepthStencilView(s_dev, (ID3D11Resource *)t->ds_tex, NULL, &t->dsv);

    t->color_va = s->color_va; t->w = s->width; t->h = s->height;
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
static void note_present_candidate(Target *t, const Nv2aSurface *s)
{
    if (s->width / s->aa_sx >= 512)
        s_present = t;
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

/* Sparse hash: the first 256 bytes plus 64 dwords spread over the smallest
 * size the texture can have (DXT1 is half a byte per texel). Cheap enough to
 * run on every texture once per frame; catches nearly every real change. */
static uint32_t tex_hash(const Nv2aTexture *t)
{
    uint32_t len = t->width * t->height / 2, h = 2166136261u, i, step;
    const uint8_t *base;

    if (len < 256) len = 256;
    if (!guest_readable(t->offset, len)) return 0;
    base = (const uint8_t *)xbox_GetMemoryOffset() + t->offset;
    for (i = 0; i < 256; i += 4) { h ^= *(const uint32_t *)(base + i); h *= 16777619u; }
    step = (len / 64) & ~3u;
    if (step)
        for (i = 0; i < 64; i++) { h ^= *(const uint32_t *)(base + i * step); h *= 16777619u; }
    if (t->palette && guest_readable(t->palette, 1024)) {
        const uint32_t *pal = (const uint32_t *)((const uint8_t *)xbox_GetMemoryOffset() + t->palette);
        for (i = 0; i < 256; i += 8) { h ^= pal[i]; h *= 16777619u; }
    }
    return h ^ len;
}

static void tex_release(TexEntry *e)
{
    if (e->srv) { ID3D11ShaderResourceView_Release(e->srv); e->srv = NULL; }
    if (e->tex) { ID3D11Texture2D_Release(e->tex); e->tex = NULL; }
}

static void tex_upload(TexEntry *e, const Nv2aTexture *t)
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
    if (!nv2a_backend_decode_texture(t, s_decode_buf)) {
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
        if (e->checked_frame != s_frame) {
            uint32_t h = tex_hash(t);
            e->checked_frame = s_frame;
            if (h != e->hash) { e->hash = h; tex_upload(e, t); }
        }
        e->last_used = s_frame;
        return e->srv;
    }
    e = &s_tex[free_slot >= 0 ? free_slot : lru];
    tex_release(e);
    memset(e, 0, sizeof *e);
    e->used = 1;
    e->offset = t->offset; e->w = t->width; e->h = t->height;
    e->color = t->color; e->pitch = t->pitch; e->palette = t->palette;
    e->hash = tex_hash(t);
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

static void be_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    Target *t;
    if (!init() || !s->color_va || !s->width || !s->height) return;
    t = target_get(s);
    if (!t) return;
    note_depth_range(rs);
    note_present_candidate(t, s);
    if (flags & 0xF0) {
        float c[4];
        c[0] = ((argb >> 16) & 0xFF) / 255.0f;
        c[1] = ((argb >> 8) & 0xFF) / 255.0f;
        c[2] = (argb & 0xFF) / 255.0f;
        c[3] = ((argb >> 24) & 0xFF) / 255.0f;
        ID3D11DeviceContext_ClearRenderTargetView(s_ctx, t->rtv, c);
    }
    if ((flags & 0x3) && t->dsv) {
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
        if (rt && target_cpu_newer(rt, b->texture)) {
            srv = tex_get(b->texture); s_st_rtt_guest++;
        }
        else if (rt && rt != t) { srv = rt->srv; s_st_rtt++; }
        else if (rt) s_st_rtt_self++;
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
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &srv);
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, &samp);
    ID3D11DeviceContext_RSSetState(s_ctx, s_rs);

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

    if (s_present && cr.right > 0 && cr.bottom > 0) {
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
                fprintf(stderr, "[D3D11] %u draws, textured %u/%u (rtt %u, self %u, guest %u),"
                        " %d targets, present 0x%08X %ux%u\n",
                        s_st_draws, s_st_texok, s_st_texreq, s_st_rtt, s_st_rtt_self, s_st_rtt_guest, nt,
                        s_present ? s_present->color_va : 0,
                        s_present ? s_present->w : 0, s_present ? s_present->h : 0);
                s_st_draws = s_st_texreq = s_st_texok = s_st_rtt = s_st_rtt_self = s_st_rtt_guest = 0;
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
