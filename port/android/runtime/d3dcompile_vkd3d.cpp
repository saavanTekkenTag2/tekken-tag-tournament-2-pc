// Android: D3DCompile (d3dcompiler_47) over vkd3d-shader's HLSL compiler.
//
// The renderer translates Xenos microcode to HLSL and compiles it at run time (gpu_hw_d3d11.h
// Compile/GuardedCompile, D3DCompile with entry "main" and a vs_/ps_ profile). On Windows that is
// Microsoft's compiler; here vkd3d-shader compiles the same HLSL to DXBC (SM4/5 "TPF"), which
// DXVK then turns into SPIR-V. Option mapping follows vkd3d-utils' D3DCompile2VKD3D.
#include <windows.h>
#include <d3dcommon.h>
#include <d3dcompiler.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <vkd3d_shader.h>

namespace {

class Blob final : public ID3DBlob
{
public:
    Blob(const void* data, size_t size) : data_(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D10Blob)) { AddRef(); *out = this; return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG r = --refs_; if (!r) delete this; return r; }
    LPVOID STDMETHODCALLTYPE GetBufferPointer() override { return data_.data(); }
    SIZE_T STDMETHODCALLTYPE GetBufferSize() override { return data_.size(); }
private:
    std::atomic<ULONG> refs_{ 1 };
    std::vector<uint8_t> data_;
};

vkd3d_shader_target_type TargetFor(const char* profile)
{
    // "vs_5_0", "ps_4_0_level_9_3", "fx_5_0", "vs_3_0"
    if (!profile) return VKD3D_SHADER_TARGET_DXBC_TPF;
    if (!std::strncmp(profile, "fx_", 3)) return VKD3D_SHADER_TARGET_FX;
    const char* u = std::strchr(profile, '_');
    const int major = u ? std::atoi(u + 1) : 4;
    return major >= 4 ? VKD3D_SHADER_TARGET_DXBC_TPF : VKD3D_SHADER_TARGET_D3D_BYTECODE;
}

} // namespace

extern "C" HRESULT WINAPI D3DCompile(const void* data, SIZE_T size, const char* name, const D3D_SHADER_MACRO* macros,
                                     ID3DInclude* include, const char* entry, const char* profile, UINT flags, UINT effectFlags,
                                     ID3DBlob** shader, ID3DBlob** errors)
{
    if (shader) *shader = nullptr;
    if (errors) *errors = nullptr;
    if (!data || !profile) return E_INVALIDARG;
    if (include) std::fprintf(stderr, "[d3dcompile] ID3DInclude is not supported on Android; #include lines will fail\n");

    vkd3d_shader_compile_option options[6];
    unsigned count = 0;
    options[count++] = { VKD3D_SHADER_COMPILE_OPTION_API_VERSION, VKD3D_SHADER_API_VERSION_1_18 };
    if (!(flags & D3DCOMPILE_DEBUG)) options[count++] = { VKD3D_SHADER_COMPILE_OPTION_STRIP_DEBUG, 1 };
    if (flags & (D3DCOMPILE_PACK_MATRIX_ROW_MAJOR | D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR))
        options[count++] = { VKD3D_SHADER_COMPILE_OPTION_PACK_MATRIX_ORDER,
                             (flags & D3DCOMPILE_PACK_MATRIX_ROW_MAJOR) ? unsigned(VKD3D_SHADER_COMPILE_OPTION_PACK_MATRIX_ROW_MAJOR)
                                                                         : unsigned(VKD3D_SHADER_COMPILE_OPTION_PACK_MATRIX_COLUMN_MAJOR) };
    if (flags & D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY)
        options[count++] = { VKD3D_SHADER_COMPILE_OPTION_BACKWARD_COMPATIBILITY, VKD3D_SHADER_COMPILE_OPTION_BACKCOMPAT_MAP_SEMANTIC_NAMES };
    // d3dcompiler_47 behaviour
    options[count++] = { VKD3D_SHADER_COMPILE_OPTION_WARN_IMPLICIT_TRUNCATION, 1 };
    (void)effectFlags;

    // Macros: D3D_SHADER_MACRO and vkd3d_shader_macro share the {name, value} layout.
    std::vector<vkd3d_shader_macro> macroList;
    for (const D3D_SHADER_MACRO* m = macros; m && m->Name; ++m) macroList.push_back({ m->Name, m->Definition ? m->Definition : "" });

    vkd3d_shader_hlsl_source_info hlsl{};
    hlsl.type = VKD3D_SHADER_STRUCTURE_TYPE_HLSL_SOURCE_INFO;
    hlsl.entry_point = entry ? entry : "main";
    hlsl.profile = profile;

    vkd3d_shader_preprocess_info pre{};
    pre.type = VKD3D_SHADER_STRUCTURE_TYPE_PREPROCESS_INFO;
    pre.next = &hlsl;
    pre.macros = macroList.empty() ? nullptr : macroList.data();
    pre.macro_count = unsigned(macroList.size());

    vkd3d_shader_compile_info info{};
    info.type = VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;
    info.next = &pre;
    info.source.code = data;
    info.source.size = size;
    info.source_type = VKD3D_SHADER_SOURCE_HLSL;
    info.target_type = TargetFor(profile);
    info.options = options;
    info.option_count = count;
    info.log_level = VKD3D_SHADER_LOG_WARNING;
    info.source_name = name ? name : "shader";

    vkd3d_shader_code out{};
    char* messages = nullptr;
    const int ret = vkd3d_shader_compile(&info, &out, &messages);
    if (messages)
    {
        if (errors && *messages) *errors = new Blob(messages, std::strlen(messages));
        vkd3d_shader_free_messages(messages);
    }
    if (ret < 0)
    {
        if (errors && !*errors)
        {
            static const char kFail[] = "vkd3d-shader: HLSL compilation failed";
            *errors = new Blob(kFail, sizeof(kFail) - 1);
        }
        return E_FAIL;
    }
    if (shader) *shader = new Blob(out.code, out.size);
    vkd3d_shader_free_shader_code(&out);
    return S_OK;
}

extern "C" HRESULT WINAPI D3DCreateBlob(SIZE_T size, ID3DBlob** blob)
{
    if (!blob) return E_POINTER;
    std::vector<uint8_t> zero(size);
    *blob = new Blob(zero.data(), size);
    return S_OK;
}
