// Android GPU self-test: a few tiny D3D11 jobs through DXVK and vkd3d-shader, each read back and
// compared, written to <root>/TTT2Recomp/gpu_selftest.txt (the launcher's Logs shows it). It runs
// before the game starts and tells which layer loses the picture when the screen stays black:
//   1 clear + copy + map          DXVK basics, readback
//   2 draw, constant colour        vkd3d HLSL -> DXBC, DXVK's DXBC -> SPIR-V, rasterizer, render target
//   3 draw, colour from cbuffer    constant buffers
//   4 draw, texel from a texture   shader resources (Load)
//   5 draw, sampled texture        samplers
//   6 draw into a B8G8R8A8 target  the format the game presents from
//   7 draw with DepthClipEnable = FALSE (the game's own rasterizer states): a) as DXVK is, b) with the
//     Android depth-clip workaround (DXVK_ANDROID_FORCE_DEPTH_CLIP)
//   8 uint target, nointerpolation uint varyings (the runtime's constant-ring self-test shape)
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {
FILE* g_out = nullptr;

void Log(const char* fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (g_out) { std::fputs(line, g_out); std::fflush(g_out); }
    std::fputs(line, stderr);
}

ComPtr<ID3DBlob> Compile(const char* src, const char* target)
{
    ComPtr<ID3DBlob> code, err;
    const HRESULT hr = D3DCompile(src, std::strlen(src), "selftest", nullptr, nullptr, "main", target, 0, 0, &code, &err);
    if (FAILED(hr))
    {
        Log("    D3DCompile(%s) failed %08X: %s\n", target, unsigned(hr), err ? static_cast<const char*>(err->GetBufferPointer()) : "");
        return nullptr;
    }
    return code;
}

struct Ctx
{
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
};

// The 2x2 target's first pixel as 0xRRGGBBAA (from an RGBA8 or BGRA8 texture).
bool ReadPixel(Ctx& c, ID3D11Texture2D* tex, uint32_t& rgba)
{
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> st;
    if (FAILED(c.dev->CreateTexture2D(&d, nullptr, &st))) { Log("    staging texture failed\n"); return false; }
    c.ctx->CopyResource(st.Get(), tex);
    D3D11_MAPPED_SUBRESOURCE m{};
    const HRESULT hr = c.ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr)) { Log("    Map failed %08X\n", unsigned(hr)); return false; }
    const uint8_t* p = static_cast<const uint8_t*>(m.pData);
    if (d.Format == DXGI_FORMAT_B8G8R8A8_UNORM) rgba = uint32_t(p[2]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[0]) << 8 | p[3];
    else rgba = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    c.ctx->Unmap(st.Get(), 0);
    return true;
}

ComPtr<ID3D11Texture2D> Target(Ctx& c, DXGI_FORMAT fmt, ComPtr<ID3D11RenderTargetView>& rtv)
{
    D3D11_TEXTURE2D_DESC d{};
    d.Width = 2; d.Height = 2; d.MipLevels = d.ArraySize = 1; d.Format = fmt; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> t;
    if (FAILED(c.dev->CreateTexture2D(&d, nullptr, &t))) return nullptr;
    if (FAILED(c.dev->CreateRenderTargetView(t.Get(), nullptr, &rtv))) return nullptr;
    return t;
}

const char* kVs =
    "struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };"
    "O main(uint id : SV_VertexID) { O o; float2 t = float2((id << 1) & 2, id & 2);"
    " o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = t; return o; }";

// Draws a full-screen triangle with the pixel shader into a fresh 2x2 target (cleared to grey first).
bool Draw(Ctx& c, const char* ps, DXGI_FORMAT fmt, uint32_t& got,
          ID3D11Buffer* cb = nullptr, ID3D11ShaderResourceView* srv = nullptr, ID3D11SamplerState* smp = nullptr,
          bool depthClip = true)
{
    auto vsCode = Compile(kVs, "vs_5_0");
    auto psCode = Compile(ps, "ps_5_0");
    if (!vsCode || !psCode) return false;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> pso;
    HRESULT hr = c.dev->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs);
    if (FAILED(hr)) { Log("    CreateVertexShader failed %08X\n", unsigned(hr)); return false; }
    hr = c.dev->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &pso);
    if (FAILED(hr)) { Log("    CreatePixelShader failed %08X\n", unsigned(hr)); return false; }
    ComPtr<ID3D11RenderTargetView> rtv;
    auto tex = Target(c, fmt, rtv);
    if (!tex) { Log("    render target failed\n"); return false; }
    const float grey[4] = { 0.5f, 0.5f, 0.5f, 1 };
    c.ctx->ClearState();
    c.ctx->ClearRenderTargetView(rtv.Get(), grey);
    c.ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    const D3D11_VIEWPORT vp{ 0, 0, 2, 2, 0, 1 };
    c.ctx->RSSetViewports(1, &vp);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = depthClip ? TRUE : FALSE;
    ComPtr<ID3D11RasterizerState> rs;
    c.dev->CreateRasterizerState(&rd, &rs);
    c.ctx->RSSetState(rs.Get());
    c.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c.ctx->VSSetShader(vs.Get(), nullptr, 0);
    c.ctx->PSSetShader(pso.Get(), nullptr, 0);
    if (cb) c.ctx->PSSetConstantBuffers(0, 1, &cb);
    if (srv) c.ctx->PSSetShaderResources(0, 1, &srv);
    if (smp) c.ctx->PSSetSamplers(0, 1, &smp);
    c.ctx->Draw(3, 0);
    return ReadPixel(c, tex.Get(), got);
}

void Check(const char* name, bool ran, uint32_t got, uint32_t want, int& failed)
{
    const bool ok = ran && got == want;
    if (!ok) ++failed;
    Log("  %-34s %s  got %08X want %08X\n", name, ok ? "OK  " : "FAIL", ran ? got : 0u, want);
}
} // namespace

extern "C" void Ttt2GpuSelfTest(const char* root)
{
    const std::string path = std::string(root) + "/TTT2Recomp/gpu_selftest.txt";
    g_out = std::fopen(path.c_str(), "w");
    Log("[gpu-selftest] D3D11 (DXVK) + D3DCompile (vkd3d-shader)\n");
    Ctx c;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL fl{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &c.dev, &fl, &c.ctx);
    if (FAILED(hr)) { Log("  D3D11CreateDevice failed %08X\n", unsigned(hr)); if (g_out) std::fclose(g_out); g_out = nullptr; return; }
    {
        ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> ad; DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(c.dev.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&ad))) ad->GetDesc(&desc);
        char name[512] = {};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof name - 1, nullptr, nullptr);
        Log("  adapter: %s, feature level %X, vendor %04X device %04X, %llu MB\n", name, unsigned(fl), desc.VendorId, desc.DeviceId,
            (unsigned long long)(desc.DedicatedVideoMemory >> 20));
    }
    int failed = 0;
    uint32_t got = 0;

    // 1: clear + readback
    {
        ComPtr<ID3D11RenderTargetView> rtv;
        auto tex = Target(c, DXGI_FORMAT_R8G8B8A8_UNORM, rtv);
        const float red[4] = { 1, 0, 0, 1 };
        bool ran = false;
        if (tex) { c.ctx->ClearRenderTargetView(rtv.Get(), red); ran = ReadPixel(c, tex.Get(), got); }
        Check("1 clear red + readback", ran, got, 0xFF0000FFu, failed);
    }
    // 2: constant colour
    {
        bool ran = Draw(c, "float4 main(float4 p : SV_Position) : SV_Target { return float4(0, 1, 0, 1); }", DXGI_FORMAT_R8G8B8A8_UNORM, got);
        Check("2 draw green (shader constant)", ran, got, 0x00FF00FFu, failed);
    }
    // 3: cbuffer colour
    {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        const float blue[4] = { 0, 0, 1, 1 };
        D3D11_SUBRESOURCE_DATA init{ blue, 0, 0 };
        ComPtr<ID3D11Buffer> cb;
        bool ran = SUCCEEDED(c.dev->CreateBuffer(&bd, &init, &cb)) &&
                   Draw(c, "cbuffer C : register(b0) { float4 col; }; float4 main(float4 p : SV_Position) : SV_Target { return col; }",
                        DXGI_FORMAT_R8G8B8A8_UNORM, got, cb.Get());
        Check("3 draw blue (constant buffer)", ran, got, 0x0000FFFFu, failed);
    }
    // 4 + 5: texture Load and Sample
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = 2; td.Height = 2; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const uint32_t texels[4] = { 0xFF00FFFFu, 0xFF00FFFFu, 0xFF00FFFFu, 0xFF00FFFFu };   // ABGR in memory: R=FF G=FF B=00 A=FF (yellow)
        D3D11_SUBRESOURCE_DATA init{ texels, 8, 0 };
        ComPtr<ID3D11Texture2D> t;
        ComPtr<ID3D11ShaderResourceView> srv;
        bool made = SUCCEEDED(c.dev->CreateTexture2D(&td, &init, &t)) && SUCCEEDED(c.dev->CreateShaderResourceView(t.Get(), nullptr, &srv));
        bool ran = made && Draw(c, "Texture2D<float4> tx : register(t0); float4 main(float4 p : SV_Position) : SV_Target { return tx.Load(int3(int2(p.xy), 0)); }",
                                DXGI_FORMAT_R8G8B8A8_UNORM, got, nullptr, srv.Get());
        Check("4 draw yellow (texture Load)", ran, got, 0xFFFF00FFu, failed);
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
        ComPtr<ID3D11SamplerState> smp;
        ran = made && SUCCEEDED(c.dev->CreateSamplerState(&sd, &smp)) &&
              Draw(c, "Texture2D tx : register(t0); SamplerState s : register(s0);"
                      " float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return tx.Sample(s, uv * 0.5); }",
                   DXGI_FORMAT_R8G8B8A8_UNORM, got, nullptr, srv.Get(), smp.Get());
        Check("5 draw yellow (sampled texture)", ran, got, 0xFFFF00FFu, failed);
    }
    // 6: BGRA target
    {
        bool ran = Draw(c, "float4 main(float4 p : SV_Position) : SV_Target { return float4(1, 0, 1, 1); }", DXGI_FORMAT_B8G8R8A8_UNORM, got);
        Check("6 draw magenta into B8G8R8A8", ran, got, 0xFF00FFFFu, failed);
    }
    // 7: depth clip off, without and with the workaround (DXVK reads the variable when a rasterizer state is made)
    {
        const char* green = "float4 main(float4 p : SV_Position) : SV_Target { return float4(0, 1, 0, 1); }";
        setenv("DXVK_ANDROID_FORCE_DEPTH_CLIP", "0", 1);
        bool ran = Draw(c, green, DXGI_FORMAT_R8G8B8A8_UNORM, got, nullptr, nullptr, nullptr, false);
        int ignored = 0;
        Check("7a depth clip off (DXVK as is)", ran, got, 0x00FF00FFu, ignored);
        Log("     -> %s\n", ran && got == 0x00FF00FFu ? "this GPU draws without depth clip" : "this GPU drops draws without depth clip: the workaround is needed");
        setenv("DXVK_ANDROID_FORCE_DEPTH_CLIP", "1", 1);
        ran = Draw(c, green, DXGI_FORMAT_R8G8B8A8_UNORM, got, nullptr, nullptr, nullptr, false);
        Check("7b depth clip off (workaround on)", ran, got, 0x00FF00FFu, failed);
    }
    // 8: uint render target + nointerpolation uint varyings
    {
        auto vsCode = Compile("struct V { float4 pos : SV_Position; nointerpolation uint4 v : TEXCOORD0; };"
                              "V main(uint id : SV_VertexID) { V o; float2 p = float2((id << 1) & 2, id & 2);"
                              " o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1); o.v = uint4(11, 22, 33, 44); return o; }", "vs_5_0");
        auto psCode = Compile("struct V { float4 pos : SV_Position; nointerpolation uint4 v : TEXCOORD0; };"
                              "uint4 main(V i) : SV_Target { return i.v; }", "ps_5_0");
        bool ran = false;
        uint32_t u[4] = {};
        ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> ps;
        if (vsCode && psCode && SUCCEEDED(c.dev->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs)) &&
            SUCCEEDED(c.dev->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps)))
        {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = 2; td.Height = 1; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R32G32B32A32_UINT; td.SampleDesc.Count = 1;
            td.BindFlags = D3D11_BIND_RENDER_TARGET;
            ComPtr<ID3D11Texture2D> t, st; ComPtr<ID3D11RenderTargetView> rtv;
            c.dev->CreateTexture2D(&td, nullptr, &t);
            td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            c.dev->CreateTexture2D(&td, nullptr, &st);
            if (t && st && SUCCEEDED(c.dev->CreateRenderTargetView(t.Get(), nullptr, &rtv)))
            {
                D3D11_RASTERIZER_DESC rd{};
                rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
                ComPtr<ID3D11RasterizerState> rs; c.dev->CreateRasterizerState(&rd, &rs);
                c.ctx->ClearState();
                c.ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
                const D3D11_VIEWPORT vp{ 0, 0, 2, 1, 0, 1 };
                c.ctx->RSSetViewports(1, &vp);
                c.ctx->RSSetState(rs.Get());
                c.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                c.ctx->VSSetShader(vs.Get(), nullptr, 0);
                c.ctx->PSSetShader(ps.Get(), nullptr, 0);
                c.ctx->Draw(3, 0);
                c.ctx->CopyResource(st.Get(), t.Get());
                D3D11_MAPPED_SUBRESOURCE m{};
                if (SUCCEEDED(c.ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) { std::memcpy(u, m.pData, 16); c.ctx->Unmap(st.Get(), 0); ran = true; }
            }
        }
        const bool ok = ran && u[0] == 11 && u[1] == 22 && u[2] == 33 && u[3] == 44;
        if (!ok) ++failed;
        Log("  %-34s %s  got %u %u %u %u want 11 22 33 44\n", "8 uint target, nointerpolation", ok ? "OK  " : "FAIL", u[0], u[1], u[2], u[3]);
    }
    Log("[gpu-selftest] %s (%d failed)\n", failed ? "FAILED" : "all passed", failed);
    if (g_out) std::fclose(g_out);
    g_out = nullptr;
}
