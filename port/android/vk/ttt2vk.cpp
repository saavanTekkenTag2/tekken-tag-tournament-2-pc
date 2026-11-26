// TTT2 native renderer (r24): the Direct3D 11 subset the game's renderer uses, implemented directly on Vulkan.
//
// Design (first version - correctness first, kept simple so it can be checked step by step):
//  - Every image lives in VK_IMAGE_LAYOUT_GENERAL; a full memory barrier separates render passes, transfers and
//    the CPU's reads. No layout tracking, no hazards to miss.
//  - Render passes: LOAD/STORE, begun lazily at the first draw after the targets change, ended by anything that
//    is not a draw (a copy, a clear, an upload, a map, a flush, a present).
//  - Shaders: the DXBC the game compiles (vkd3d-shader's D3DCompile) is translated to SPIR-V by vkd3d-shader
//    with an explicit binding map: set 0 = vertex stage, set 1 = pixel stage, one binding per register used.
//  - Dynamic buffers (the vertex / index / constant rings): every WRITE_DISCARD gets a fresh slice of a
//    host-visible upload heap; NO_OVERWRITE writes the current one. Slices are recycled once the GPU is done.
//  - Staging textures are host-visible buffers; Map waits on the submission that wrote them (DO_NOT_WAIT: never).
//  - Presenting: the app draws into its own back-buffer image, Present blits it to the acquired swap-chain image
//    (SDL_Vulkan_CreateSurface on the game's SDL window - an HWND is the SDL_Window* in this port).
#include "ttt2vk.h"
#include "ttt2vk_stubs.h"

#include <dxgi1_2.h>
#include <vulkan/vulkan.h>
#include <SDL.h>
#include <SDL_vulkan.h>
#include <vkd3d_shader.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef DXGI_ERROR_WAS_STILL_DRAWING
#define DXGI_ERROR_WAS_STILL_DRAWING ((HRESULT)0x887A000A)
#endif
#ifndef DXGI_ERROR_UNSUPPORTED
#define DXGI_ERROR_UNSUPPORTED ((HRESULT)0x887A0004)
#endif
#ifndef DXGI_ERROR_NOT_FOUND
#define DXGI_ERROR_NOT_FOUND ((HRESULT)0x887A0002)
#endif

namespace ttt2vk {

static void Log(const char* fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "[native-vk] %s\n", line);
}

static bool Same(REFIID a, REFIID b) { return std::memcmp(&a, &b, sizeof(GUID)) == 0; }

// ================================================================ COM plumbing
template <class Base>
struct Com : Base
{
    std::atomic<ULONG> refs{ 1 };
    virtual ~Com() {}
    // The interfaces this object answers to, besides IUnknown.
    virtual void* Find(REFIID riid) = 0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        void* p = Same(riid, __uuidof(IUnknown)) ? static_cast<IUnknown*>(this) : Find(riid);
        if (!p) return E_NOINTERFACE;
        AddRef();
        *out = p;
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG r = --refs;
        if (!r) delete this;
        return r;
    }
};

template <class T> static T* AddRefd(T* p) { if (p) p->AddRef(); return p; }
template <class T> struct Ref
{
    T* p = nullptr;
    Ref() = default;
    Ref(T* q) : p(AddRefd(q)) {}
    Ref(const Ref& o) : p(AddRefd(o.p)) {}
    Ref& operator=(const Ref& o) { if (o.p != p) { T* old = p; p = AddRefd(o.p); if (old) old->Release(); } return *this; }
    Ref& operator=(T* q) { if (q != p) { T* old = p; p = AddRefd(q); if (old) old->Release(); } return *this; }
    ~Ref() { if (p) p->Release(); }
    T* operator->() const { return p; }
    T* get() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

struct Device;
struct Context;

// ================================================================ Vulkan core
struct Core
{
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceMemoryProperties memProps{};
    VkPhysicalDeviceFeatures enabled{};
    bool maintenance1 = false;
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    std::mutex queueMutex;          // vkQueueSubmit / present / one-off submissions

    // Submissions: ids grow by one per vkQueueSubmit of the context. `next` is the id the open command buffer
    // will get; anything recorded or retired now is safe once `completed` reaches it.
    std::atomic<uint64_t> next{ 1 }, completed{ 0 };
    struct InFlight { uint64_t id; VkFence fence; VkCommandBuffer cmd; std::vector<VkDescriptorPool> pools; };
    std::deque<InFlight> inflight;
    std::mutex deferMutex;
    std::vector<std::pair<uint64_t, std::function<void()>>> deferred;   // destroy once `completed` >= id

    int32_t FindMemory(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid = 0) const
    {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & want) == want && !(memProps.memoryTypes[i].propertyFlags & avoid))
                return int32_t(i);
        if (avoid) return FindMemory(bits, want, 0);
        return -1;
    }
    VkDeviceMemory Allocate(const VkMemoryRequirements& r, VkMemoryPropertyFlags want, VkMemoryPropertyFlags fallback = 0)
    {
        int32_t type = FindMemory(r.memoryTypeBits, want);
        if (type < 0 && fallback) type = FindMemory(r.memoryTypeBits, fallback);
        if (type < 0) return VK_NULL_HANDLE;
        VkMemoryAllocateInfo a{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        a.allocationSize = r.size;
        a.memoryTypeIndex = uint32_t(type);
        VkDeviceMemory m = VK_NULL_HANDLE;
        if (vkAllocateMemory(dev, &a, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
        return m;
    }
    void Defer(std::function<void()> f)
    {
        std::lock_guard l(deferMutex);
        deferred.emplace_back(next.load(), std::move(f));
    }
    void RunDeferred()
    {
        std::vector<std::function<void()>> ready;
        {
            std::lock_guard l(deferMutex);
            const uint64_t done = completed.load();
            auto it = std::stable_partition(deferred.begin(), deferred.end(), [done](auto& d) { return d.first > done; });
            for (auto j = it; j != deferred.end(); ++j) ready.push_back(std::move(j->second));
            deferred.erase(it, deferred.end());
        }
        for (auto& f : ready) f();
    }
};

// ================================================================ upload heap (dynamic buffers, uploads)
struct Chunk
{
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    uint8_t* ptr = nullptr;
    VkDeviceSize size = 0, cursor = 0;
    int live = 0;
    uint64_t busyUntil = 0;
};
struct Slice
{
    Chunk* chunk = nullptr;
    VkDeviceSize off = 0, size = 0;
    VkBuffer buf() const { return chunk ? chunk->buf : VK_NULL_HANDLE; }
    uint8_t* ptr() const { return chunk ? chunk->ptr + off : nullptr; }
};

struct UploadHeap
{
    Core* core = nullptr;
    std::mutex m;
    std::vector<std::unique_ptr<Chunk>> chunks;
    Chunk* current = nullptr;
    static constexpr VkDeviceSize ChunkBytes = 16u << 20;

    Chunk* NewChunk(VkDeviceSize size)
    {
        auto c = std::make_unique<Chunk>();
        VkBufferCreateInfo b{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        b.size = size;
        b.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                  VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if (vkCreateBuffer(core->dev, &b, nullptr, &c->buf) != VK_SUCCESS) return nullptr;
        VkMemoryRequirements r;
        vkGetBufferMemoryRequirements(core->dev, c->buf, &r);
        c->mem = core->Allocate(r, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (!c->mem) { vkDestroyBuffer(core->dev, c->buf, nullptr); return nullptr; }
        vkBindBufferMemory(core->dev, c->buf, c->mem, 0);
        void* p = nullptr;
        vkMapMemory(core->dev, c->mem, 0, VK_WHOLE_SIZE, 0, &p);
        c->ptr = static_cast<uint8_t*>(p);
        c->size = size;
        chunks.push_back(std::move(c));
        return chunks.back().get();
    }
    Slice Alloc(VkDeviceSize size, VkDeviceSize align = 256)
    {
        std::lock_guard l(m);
        size = std::max<VkDeviceSize>(size, 4);
        auto fits = [&](Chunk* c) { return c && ((c->cursor + align - 1) & ~(align - 1)) + size <= c->size; };
        if (!fits(current))
        {
            current = nullptr;
            const uint64_t done = core->completed.load();
            for (auto& c : chunks)
                if (c->live == 0 && c->busyUntil <= done && c->size >= size) { c->cursor = 0; current = c.get(); break; }
            if (!current) current = NewChunk(std::max(ChunkBytes, (size + 0xFFFF) & ~VkDeviceSize(0xFFFF)));
            if (!current) return {};
        }
        Slice s;
        s.chunk = current;
        s.off = (current->cursor + align - 1) & ~(align - 1);
        s.size = size;
        current->cursor = s.off + size;
        ++current->live;
        return s;
    }
    void Retire(Slice& s)
    {
        if (!s.chunk) return;
        std::lock_guard l(m);
        --s.chunk->live;
        s.chunk->busyUntil = std::max<uint64_t>(s.chunk->busyUntil, core->next.load());
        s = {};
    }
};

// ================================================================ formats
struct FormatInfo { VkFormat vk; uint32_t bytes; uint32_t block; VkImageAspectFlags aspect; };
static FormatInfo Format(DXGI_FORMAT f)
{
    const VkImageAspectFlags C = VK_IMAGE_ASPECT_COLOR_BIT, DS = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    switch (f)
    {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return { VK_FORMAT_R32G32B32A32_SFLOAT, 16, 1, C };
    case DXGI_FORMAT_R32G32B32A32_UINT: return { VK_FORMAT_R32G32B32A32_UINT, 16, 1, C };
    case DXGI_FORMAT_R32G32B32A32_SINT: return { VK_FORMAT_R32G32B32A32_SINT, 16, 1, C };
    case DXGI_FORMAT_R32G32B32_FLOAT: return { VK_FORMAT_R32G32B32_SFLOAT, 12, 1, C };
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return { VK_FORMAT_R16G16B16A16_SFLOAT, 8, 1, C };
    case DXGI_FORMAT_R16G16B16A16_UNORM: return { VK_FORMAT_R16G16B16A16_UNORM, 8, 1, C };
    case DXGI_FORMAT_R32G32_FLOAT: return { VK_FORMAT_R32G32_SFLOAT, 8, 1, C };
    case DXGI_FORMAT_R32G32_UINT: return { VK_FORMAT_R32G32_UINT, 8, 1, C };
    case DXGI_FORMAT_R10G10B10A2_UNORM: return { VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, 1, C };
    case DXGI_FORMAT_R11G11B10_FLOAT: return { VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4, 1, C };
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM: return { VK_FORMAT_R8G8B8A8_UNORM, 4, 1, C };
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return { VK_FORMAT_R8G8B8A8_SRGB, 4, 1, C };
    case DXGI_FORMAT_R8G8B8A8_UINT: return { VK_FORMAT_R8G8B8A8_UINT, 4, 1, C };
    case DXGI_FORMAT_R8G8B8A8_SNORM: return { VK_FORMAT_R8G8B8A8_SNORM, 4, 1, C };
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM: return { VK_FORMAT_B8G8R8A8_UNORM, 4, 1, C };
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return { VK_FORMAT_B8G8R8A8_SRGB, 4, 1, C };
    case DXGI_FORMAT_B8G8R8X8_UNORM: return { VK_FORMAT_B8G8R8A8_UNORM, 4, 1, C };
    case DXGI_FORMAT_R16G16_FLOAT: return { VK_FORMAT_R16G16_SFLOAT, 4, 1, C };
    case DXGI_FORMAT_R16G16_UNORM: return { VK_FORMAT_R16G16_UNORM, 4, 1, C };
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_UINT: return { VK_FORMAT_R32_UINT, 4, 1, C };
    case DXGI_FORMAT_R32_SINT: return { VK_FORMAT_R32_SINT, 4, 1, C };
    case DXGI_FORMAT_R32_FLOAT: return { VK_FORMAT_R32_SFLOAT, 4, 1, C };
    case DXGI_FORMAT_R8G8_UNORM: return { VK_FORMAT_R8G8_UNORM, 2, 1, C };
    case DXGI_FORMAT_R16_FLOAT: return { VK_FORMAT_R16_SFLOAT, 2, 1, C };
    case DXGI_FORMAT_R16_UNORM: return { VK_FORMAT_R16_UNORM, 2, 1, C };
    case DXGI_FORMAT_R16_UINT: return { VK_FORMAT_R16_UINT, 2, 1, C };
    case DXGI_FORMAT_B5G6R5_UNORM: return { VK_FORMAT_R5G6B5_UNORM_PACK16, 2, 1, C };
    case DXGI_FORMAT_B5G5R5A1_UNORM: return { VK_FORMAT_A1R5G5B5_UNORM_PACK16, 2, 1, C };
    case DXGI_FORMAT_R8_UNORM: return { VK_FORMAT_R8_UNORM, 1, 1, C };
    case DXGI_FORMAT_R8_UINT: return { VK_FORMAT_R8_UINT, 1, 1, C };
    case DXGI_FORMAT_A8_UNORM: return { VK_FORMAT_R8_UNORM, 1, 1, C };
    case DXGI_FORMAT_BC1_UNORM: return { VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8, 4, C };
    case DXGI_FORMAT_BC2_UNORM: return { VK_FORMAT_BC2_UNORM_BLOCK, 16, 4, C };
    case DXGI_FORMAT_BC3_UNORM: return { VK_FORMAT_BC3_UNORM_BLOCK, 16, 4, C };
    case DXGI_FORMAT_BC4_UNORM: return { VK_FORMAT_BC4_UNORM_BLOCK, 8, 4, C };
    case DXGI_FORMAT_BC5_UNORM: return { VK_FORMAT_BC5_UNORM_BLOCK, 16, 4, C };
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return { VK_FORMAT_D32_SFLOAT_S8_UINT, 8, 1, DS };
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return { VK_FORMAT_D32_SFLOAT_S8_UINT, 8, 1, VK_IMAGE_ASPECT_DEPTH_BIT };
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT: return { VK_FORMAT_D32_SFLOAT_S8_UINT, 8, 1, VK_IMAGE_ASPECT_STENCIL_BIT };
    case DXGI_FORMAT_D32_FLOAT: return { VK_FORMAT_D32_SFLOAT, 4, 1, VK_IMAGE_ASPECT_DEPTH_BIT };
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return { VK_FORMAT_D24_UNORM_S8_UINT, 4, 1, DS };
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return { VK_FORMAT_D24_UNORM_S8_UINT, 4, 1, VK_IMAGE_ASPECT_DEPTH_BIT };
    default: return { VK_FORMAT_UNDEFINED, 0, 1, C };
    }
}
static bool IsDepth(VkFormat f) { return f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D24_UNORM_S8_UINT; }
static VkImageAspectFlags FullAspect(VkFormat f)
{
    if (f == VK_FORMAT_D32_SFLOAT) return VK_IMAGE_ASPECT_DEPTH_BIT;
    if (IsDepth(f)) return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    return VK_IMAGE_ASPECT_COLOR_BIT;
}

// ================================================================ device children
template <class Base>
struct Child : Com<Base>
{
    Device* device;
    explicit Child(Device* d);
    ~Child() override;
    void STDMETHODCALLTYPE GetDevice(ID3D11Device** out) override;
};

// ---------------------------------------------------------------- buffer
struct Buffer : Child<Stub_ID3D11Buffer<ID3D11Buffer>>
{
    D3D11_BUFFER_DESC desc{};
    VkBuffer buf = VK_NULL_HANDLE;           // DEFAULT / IMMUTABLE / STAGING
    VkDeviceMemory mem = VK_NULL_HANDLE;
    uint8_t* map = nullptr;                  // STAGING
    Slice slice;                             // DYNAMIC: the current copy
    uint64_t lastWrite = 0;                  // STAGING: the submission that wrote it
    uint32_t version = 0;                    // DYNAMIC: bumped at every discard (texel views follow)
    explicit Buffer(Device* d) : Child(d) {}
    ~Buffer() override;
    void* Find(REFIID riid) override
    {
        if (Same(riid, __uuidof(ID3D11Buffer)) || Same(riid, __uuidof(ID3D11Resource)) || Same(riid, __uuidof(ID3D11DeviceChild))) return static_cast<ID3D11Buffer*>(this);
        return nullptr;
    }
    void STDMETHODCALLTYPE GetDesc(D3D11_BUFFER_DESC* d) override { *d = desc; }
    void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* t) override { *t = D3D11_RESOURCE_DIMENSION_BUFFER; }
    bool Dynamic() const { return desc.Usage == D3D11_USAGE_DYNAMIC; }
    VkBuffer Handle() const { return Dynamic() ? slice.buf() : buf; }
    VkDeviceSize Base() const { return Dynamic() ? slice.off : 0; }
};

// ---------------------------------------------------------------- texture
struct Texture : Child<Stub_ID3D11Texture2D<ID3D11Texture2D>>
{
    D3D11_TEXTURE2D_DESC desc{};
    FormatInfo fmt{};
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    bool external = false;                   // a swap chain's back buffer: owned by the swap chain
    // STAGING: a linear buffer, subresources packed one after another.
    VkBuffer sbuf = VK_NULL_HANDLE;
    VkDeviceMemory smem = VK_NULL_HANDLE;
    uint8_t* smap = nullptr;
    bool scoherent = true;
    std::vector<VkDeviceSize> soffset;       // per subresource
    uint64_t lastWrite = 0;
    explicit Texture(Device* d) : Child(d) {}
    ~Texture() override;
    void* Find(REFIID riid) override
    {
        if (Same(riid, __uuidof(ID3D11Texture2D)) || Same(riid, __uuidof(ID3D11Resource)) || Same(riid, __uuidof(ID3D11DeviceChild))) return static_cast<ID3D11Texture2D*>(this);
        return nullptr;
    }
    void STDMETHODCALLTYPE GetDesc(D3D11_TEXTURE2D_DESC* d) override { *d = desc; }
    void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* t) override { *t = D3D11_RESOURCE_DIMENSION_TEXTURE2D; }
    bool Staging() const { return desc.Usage == D3D11_USAGE_STAGING; }
    uint32_t W(uint32_t mip) const { return std::max(1u, desc.Width >> mip); }
    uint32_t H(uint32_t mip) const { return std::max(1u, desc.Height >> mip); }
    uint32_t RowPitch(uint32_t mip) const { return (W(mip) + fmt.block - 1) / fmt.block * fmt.bytes; }
    uint32_t Rows(uint32_t mip) const { return (H(mip) + fmt.block - 1) / fmt.block; }
};

// ---------------------------------------------------------------- views
struct Srv : Child<Stub_ID3D11ShaderResourceView<ID3D11ShaderResourceView>>
{
    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
    Ref<ID3D11Resource> resource;
    Texture* tex = nullptr;
    Buffer* buffer = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    // Texel view of a buffer; for a dynamic buffer, made again after each discard.
    VkBufferView bview = VK_NULL_HANDLE;
    VkBuffer bviewBuf = VK_NULL_HANDLE;
    VkDeviceSize bviewOff = ~VkDeviceSize(0);
    VkFormat bfmt = VK_FORMAT_R32_UINT;
    VkDeviceSize bfirst = 0, brange = 0;
    explicit Srv(Device* d) : Child(d) {}
    ~Srv() override;
    void* Find(REFIID riid) override
    {
        if (Same(riid, __uuidof(ID3D11ShaderResourceView)) || Same(riid, __uuidof(ID3D11View)) || Same(riid, __uuidof(ID3D11DeviceChild))) return static_cast<ID3D11ShaderResourceView*>(this);
        return nullptr;
    }
    void STDMETHODCALLTYPE GetDesc(D3D11_SHADER_RESOURCE_VIEW_DESC* d) override { *d = desc; }
    void STDMETHODCALLTYPE GetResource(ID3D11Resource** r) override { *r = AddRefd(resource.get()); }
    VkBufferView BufferView();
};

struct Rtv : Child<Stub_ID3D11RenderTargetView<ID3D11RenderTargetView>>
{
    D3D11_RENDER_TARGET_VIEW_DESC desc{};
    Ref<ID3D11Resource> resource;
    Texture* tex = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t mip = 0, layer = 0;
    explicit Rtv(Device* d) : Child(d) {}
    ~Rtv() override;
    void* Find(REFIID riid) override
    {
        if (Same(riid, __uuidof(ID3D11RenderTargetView)) || Same(riid, __uuidof(ID3D11View)) || Same(riid, __uuidof(ID3D11DeviceChild))) return static_cast<ID3D11RenderTargetView*>(this);
        return nullptr;
    }
    void STDMETHODCALLTYPE GetDesc(D3D11_RENDER_TARGET_VIEW_DESC* d) override { *d = desc; }
    void STDMETHODCALLTYPE GetResource(ID3D11Resource** r) override { *r = AddRefd(resource.get()); }
};

struct Dsv : Child<Stub_ID3D11DepthStencilView<ID3D11DepthStencilView>>
{
    D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
    Ref<ID3D11Resource> resource;
    Texture* tex = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t mip = 0, layer = 0;
    explicit Dsv(Device* d) : Child(d) {}
    ~Dsv() override;
    void* Find(REFIID riid) override
    {
        if (Same(riid, __uuidof(ID3D11DepthStencilView)) || Same(riid, __uuidof(ID3D11View)) || Same(riid, __uuidof(ID3D11DeviceChild))) return static_cast<ID3D11DepthStencilView*>(this);
        return nullptr;
    }
    void STDMETHODCALLTYPE GetDesc(D3D11_DEPTH_STENCIL_VIEW_DESC* d) override { *d = desc; }
    void STDMETHODCALLTYPE GetResource(ID3D11Resource** r) override { *r = AddRefd(resource.get()); }
};

// ---------------------------------------------------------------- states
struct BlendState : Child<Stub_ID3D11BlendState<ID3D11BlendState>>
{
    D3D11_BLEND_DESC desc{};
    explicit BlendState(Device* d) : Child(d) {}
    void* Find(REFIID riid) override { return Same(riid, __uuidof(ID3D11BlendState)) || Same(riid, __uuidof(ID3D11DeviceChild)) ? static_cast<ID3D11BlendState*>(this) : nullptr; }
    void STDMETHODCALLTYPE GetDesc(D3D11_BLEND_DESC* d) override { *d = desc; }
};
struct RasterState : Child<Stub_ID3D11RasterizerState<ID3D11RasterizerState>>
{
    D3D11_RASTERIZER_DESC desc{};
    explicit RasterState(Device* d) : Child(d) {}
    void* Find(REFIID riid) override { return Same(riid, __uuidof(ID3D11RasterizerState)) || Same(riid, __uuidof(ID3D11DeviceChild)) ? static_cast<ID3D11RasterizerState*>(this) : nullptr; }
    void STDMETHODCALLTYPE GetDesc(D3D11_RASTERIZER_DESC* d) override { *d = desc; }
};
struct DepthState : Child<Stub_ID3D11DepthStencilState<ID3D11DepthStencilState>>
{
    D3D11_DEPTH_STENCIL_DESC desc{};
    explicit DepthState(Device* d) : Child(d) {}
    void* Find(REFIID riid) override { return Same(riid, __uuidof(ID3D11DepthStencilState)) || Same(riid, __uuidof(ID3D11DeviceChild)) ? static_cast<ID3D11DepthStencilState*>(this) : nullptr; }
    void STDMETHODCALLTYPE GetDesc(D3D11_DEPTH_STENCIL_DESC* d) override { *d = desc; }
};
struct Sampler : Child<Stub_ID3D11SamplerState<ID3D11SamplerState>>
{
    D3D11_SAMPLER_DESC desc{};
    VkSampler sampler = VK_NULL_HANDLE;
    explicit Sampler(Device* d) : Child(d) {}
    ~Sampler() override;
    void* Find(REFIID riid) override { return Same(riid, __uuidof(ID3D11SamplerState)) || Same(riid, __uuidof(ID3D11DeviceChild)) ? static_cast<ID3D11SamplerState*>(this) : nullptr; }
    void STDMETHODCALLTYPE GetDesc(D3D11_SAMPLER_DESC* d) override { *d = desc; }
};

// ---------------------------------------------------------------- shaders
struct Binding { uint32_t kind; uint32_t reg; uint32_t binding; VkDescriptorType type; };   // kind: 0 cbv 1 srv 2 sampler
struct ShaderCode
{
    VkShaderModule module = VK_NULL_HANDLE;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    std::vector<Binding> bindings;
    std::vector<std::pair<std::string, uint32_t>> inputs;     // vertex: "SEMANTICn" -> register
    uint64_t id = 0;
};
struct VertexShader : Child<Stub_ID3D11VertexShader<ID3D11VertexShader>>
{
    ShaderCode code;
    explicit VertexShader(Device* d) : Child(d) {}
    ~VertexShader() override;
    void* Find(REFIID riid) override { return Same(riid, __uuidof(ID3D11VertexShader)) || Same(riid, __uuidof(ID3D11DeviceChild)) ? static_cast<ID3D11VertexShader*>(this) : nullptr; }
};
struct PixelShader : Child<Stub_ID3D11PixelShader<ID3D11PixelShader>>
{
    ShaderCode code;
    explicit PixelShader(Device* d) : Child(d) {}
    ~PixelShader() override;
    void* Find(REFIID riid) override { return Same(riid, __uuidof(ID3D11PixelShader)) || Same(riid, __uuidof(ID3D11DeviceChild)) ? static_cast<ID3D11PixelShader*>(this) : nullptr; }
};
struct InputLayout : Child<Stub_ID3D11InputLayout<ID3D11InputLayout>>
{
    struct Attr { uint32_t location, slot, offset; VkFormat format; bool instance; };
    std::vector<Attr> attrs;
    uint64_t id = 0;
    explicit InputLayout(Device* d) : Child(d) {}
    void* Find(REFIID riid) override { return Same(riid, __uuidof(ID3D11InputLayout)) || Same(riid, __uuidof(ID3D11DeviceChild)) ? static_cast<ID3D11InputLayout*>(this) : nullptr; }
};

// ---------------------------------------------------------------- queries
struct Query : Child<Stub_ID3D11Query<ID3D11Query>>
{
    D3D11_QUERY_DESC desc{};
    uint64_t at = 0;            // EVENT: the submission that must finish
    bool ended = false;
    explicit Query(Device* d) : Child(d) {}
    void* Find(REFIID riid) override
    {
        if (Same(riid, __uuidof(ID3D11Query)) || Same(riid, __uuidof(ID3D11Asynchronous)) || Same(riid, __uuidof(ID3D11DeviceChild))) return static_cast<ID3D11Query*>(this);
        return nullptr;
    }
    void STDMETHODCALLTYPE GetDesc(D3D11_QUERY_DESC* d) override { *d = desc; }
    UINT STDMETHODCALLTYPE GetDataSize() override
    {
        switch (desc.Query)
        {
        case D3D11_QUERY_EVENT: return sizeof(BOOL);
        case D3D11_QUERY_TIMESTAMP: return sizeof(UINT64);
        case D3D11_QUERY_TIMESTAMP_DISJOINT: return sizeof(D3D11_QUERY_DATA_TIMESTAMP_DISJOINT);
        case D3D11_QUERY_PIPELINE_STATISTICS: return sizeof(D3D11_QUERY_DATA_PIPELINE_STATISTICS);
        case D3D11_QUERY_OCCLUSION: return sizeof(UINT64);
        default: return 0;
        }
    }
};

// ================================================================ device
struct DxgiDevice;
struct Adapter;
struct Factory;

struct Device : Com<Stub_ID3D11Device1<ID3D11Device1>>
{
    Core core;
    UploadHeap heap;
    Context* context = nullptr;
    std::atomic<uint64_t> ids{ 1 };
    std::mutex layoutMutex;
    std::map<std::pair<VkDescriptorSetLayout, VkDescriptorSetLayout>, VkPipelineLayout> pipelineLayouts;
    VkDescriptorSetLayout emptySetLayout = VK_NULL_HANDLE;
    // Images made since the context last looked: brought to GENERAL before their first use.
    std::mutex newImagesMutex;
    std::vector<std::pair<VkImage, VkImageAspectFlags>> newImages;
    // One-off submissions (initial data from any thread).
    std::mutex oneOffMutex;
    VkCommandPool oneOffPool = VK_NULL_HANDLE;
    std::string gpuName;

    ~Device() override;
    void* Find(REFIID riid) override;
    bool Init();

    VkPipelineLayout PipelineLayout(VkDescriptorSetLayout vs, VkDescriptorSetLayout ps)
    {
        std::lock_guard l(layoutMutex);
        auto key = std::make_pair(vs, ps);
        auto it = pipelineLayouts.find(key);
        if (it != pipelineLayouts.end()) return it->second;
        VkDescriptorSetLayout sets[2] = { vs ? vs : emptySetLayout, ps ? ps : emptySetLayout };
        VkPipelineLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        ci.setLayoutCount = 2;
        ci.pSetLayouts = sets;
        VkPipelineLayout pl = VK_NULL_HANDLE;
        vkCreatePipelineLayout(core.dev, &ci, nullptr, &pl);
        pipelineLayouts[key] = pl;
        return pl;
    }
    void NewImage(VkImage img, VkImageAspectFlags aspect)
    {
        std::lock_guard l(newImagesMutex);
        newImages.emplace_back(img, aspect);
    }
    // Records `record` into a command buffer of its own, submits it and waits (initial data).
    void OneOff(const std::function<void(VkCommandBuffer)>& record);
    bool CompileShader(const void* code, SIZE_T size, bool vertex, ShaderCode& out);

    // ---- ID3D11Device
    HRESULT STDMETHODCALLTYPE CreateBuffer(const D3D11_BUFFER_DESC* desc, const D3D11_SUBRESOURCE_DATA* init, ID3D11Buffer** out) override;
    HRESULT STDMETHODCALLTYPE CreateTexture2D(const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out) override;
    HRESULT STDMETHODCALLTYPE CreateShaderResourceView(ID3D11Resource* res, const D3D11_SHADER_RESOURCE_VIEW_DESC* desc, ID3D11ShaderResourceView** out) override;
    HRESULT STDMETHODCALLTYPE CreateRenderTargetView(ID3D11Resource* res, const D3D11_RENDER_TARGET_VIEW_DESC* desc, ID3D11RenderTargetView** out) override;
    HRESULT STDMETHODCALLTYPE CreateDepthStencilView(ID3D11Resource* res, const D3D11_DEPTH_STENCIL_VIEW_DESC* desc, ID3D11DepthStencilView** out) override;
    HRESULT STDMETHODCALLTYPE CreateInputLayout(const D3D11_INPUT_ELEMENT_DESC* elems, UINT count, const void* code, SIZE_T size, ID3D11InputLayout** out) override;
    HRESULT STDMETHODCALLTYPE CreateVertexShader(const void* code, SIZE_T size, ID3D11ClassLinkage*, ID3D11VertexShader** out) override;
    HRESULT STDMETHODCALLTYPE CreatePixelShader(const void* code, SIZE_T size, ID3D11ClassLinkage*, ID3D11PixelShader** out) override;
    HRESULT STDMETHODCALLTYPE CreateBlendState(const D3D11_BLEND_DESC* desc, ID3D11BlendState** out) override
    {
        if (!out) return S_FALSE;
        auto* s = new BlendState(this);
        s->desc = *desc;
        *out = s;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC* desc, ID3D11DepthStencilState** out) override
    {
        if (!out) return S_FALSE;
        auto* s = new DepthState(this);
        s->desc = *desc;
        *out = s;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateRasterizerState(const D3D11_RASTERIZER_DESC* desc, ID3D11RasterizerState** out) override
    {
        if (!out) return S_FALSE;
        auto* s = new RasterState(this);
        s->desc = *desc;
        *out = s;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateSamplerState(const D3D11_SAMPLER_DESC* desc, ID3D11SamplerState** out) override;
    HRESULT STDMETHODCALLTYPE CreateQuery(const D3D11_QUERY_DESC* desc, ID3D11Query** out) override
    {
        if (!out) return S_FALSE;
        auto* q = new Query(this);
        q->desc = *desc;
        *out = q;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CheckFormatSupport(DXGI_FORMAT f, UINT* support) override
    {
        const FormatInfo fi = Format(f);
        if (support) *support = fi.vk == VK_FORMAT_UNDEFINED ? 0 : 0x0FFFFFFFu;
        return fi.vk == VK_FORMAT_UNDEFINED ? E_FAIL : S_OK;
    }
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D11_FEATURE feature, void* data, UINT size) override
    {
        if (feature == D3D11_FEATURE_D3D11_OPTIONS && size == sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS))
        {
            auto* o = static_cast<D3D11_FEATURE_DATA_D3D11_OPTIONS*>(data);
            std::memset(o, 0, sizeof(*o));
            o->ConstantBufferOffsetting = TRUE;
            o->ConstantBufferPartialUpdate = TRUE;
            o->MapNoOverwriteOnDynamicConstantBuffer = TRUE;
            o->MapNoOverwriteOnDynamicBufferSRV = TRUE;
            return S_OK;
        }
        if (feature == D3D11_FEATURE_THREADING && size == sizeof(D3D11_FEATURE_DATA_THREADING))
        {
            auto* t = static_cast<D3D11_FEATURE_DATA_THREADING*>(data);
            t->DriverConcurrentCreates = TRUE;
            t->DriverCommandLists = FALSE;
            return S_OK;
        }
        return E_INVALIDARG;
    }
    D3D_FEATURE_LEVEL STDMETHODCALLTYPE GetFeatureLevel() override { return D3D_FEATURE_LEVEL_11_0; }
    UINT STDMETHODCALLTYPE GetCreationFlags() override { return D3D11_CREATE_DEVICE_BGRA_SUPPORT; }
    HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override { return S_OK; }
    void STDMETHODCALLTYPE GetImmediateContext(ID3D11DeviceContext** out) override;
    void STDMETHODCALLTYPE GetImmediateContext1(ID3D11DeviceContext1** out) override;
    UINT STDMETHODCALLTYPE GetExceptionMode() override { return 0; }
};

template <class Base> Child<Base>::Child(Device* d) : device(d) { d->AddRef(); }
template <class Base> Child<Base>::~Child() { device->Release(); }
template <class Base> void STDMETHODCALLTYPE Child<Base>::GetDevice(ID3D11Device** out) { *out = AddRefd(static_cast<ID3D11Device*>(device)); }

// ---------------------------------------------------------------- destructors (deferred: the GPU may still use them)
Buffer::~Buffer()
{
    Core* c = &device->core;
    if (slice.chunk) device->heap.Retire(slice);
    VkBuffer b = buf;
    VkDeviceMemory m = mem;
    if (b || m) c->Defer([c, b, m] { if (b) vkDestroyBuffer(c->dev, b, nullptr); if (m) vkFreeMemory(c->dev, m, nullptr); });
}
Texture::~Texture()
{
    Core* c = &device->core;
    VkImage i = external ? VK_NULL_HANDLE : image;
    VkDeviceMemory m = external ? VK_NULL_HANDLE : mem;
    VkBuffer sb = sbuf;
    VkDeviceMemory sm = smem;
    c->Defer([c, i, m, sb, sm] {
        if (i) vkDestroyImage(c->dev, i, nullptr);
        if (m) vkFreeMemory(c->dev, m, nullptr);
        if (sb) vkDestroyBuffer(c->dev, sb, nullptr);
        if (sm) vkFreeMemory(c->dev, sm, nullptr);
    });
}
Srv::~Srv()
{
    Core* c = &device->core;
    VkImageView v = view;
    VkBufferView bv = bview;
    c->Defer([c, v, bv] { if (v) vkDestroyImageView(c->dev, v, nullptr); if (bv) vkDestroyBufferView(c->dev, bv, nullptr); });
}
VkBufferView Srv::BufferView()
{
    if (!buffer) return VK_NULL_HANDLE;
    const VkBuffer b = buffer->Handle();
    const VkDeviceSize off = buffer->Base() + bfirst;
    if (bview && bviewBuf == b && bviewOff == off) return bview;
    Core* c = &device->core;
    if (bview) { VkBufferView old = bview; c->Defer([c, old] { vkDestroyBufferView(c->dev, old, nullptr); }); bview = VK_NULL_HANDLE; }
    if (!b) return VK_NULL_HANDLE;
    VkBufferViewCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO };
    ci.buffer = b;
    ci.format = bfmt;
    ci.offset = off;
    const VkDeviceSize maxRange = VkDeviceSize(c->props.limits.maxTexelBufferElements) * Format(desc.Format).bytes;
    ci.range = std::min(brange, maxRange ? maxRange : brange);
    if (vkCreateBufferView(c->dev, &ci, nullptr, &bview) != VK_SUCCESS) bview = VK_NULL_HANDLE;
    bviewBuf = b;
    bviewOff = off;
    return bview;
}
Rtv::~Rtv() { Core* c = &device->core; VkImageView v = view; c->Defer([c, v] { if (v) vkDestroyImageView(c->dev, v, nullptr); }); }
Dsv::~Dsv() { Core* c = &device->core; VkImageView v = view; c->Defer([c, v] { if (v) vkDestroyImageView(c->dev, v, nullptr); }); }
Sampler::~Sampler() { Core* c = &device->core; VkSampler s = sampler; c->Defer([c, s] { if (s) vkDestroySampler(c->dev, s, nullptr); }); }
static void DestroyShader(Core* c, ShaderCode& s)
{
    VkShaderModule m = s.module;
    c->Defer([c, m] { if (m) vkDestroyShaderModule(c->dev, m, nullptr); });
    // The set layout stays: pipeline layouts are cached by it (a few dozen shaders' worth of handles).
}
VertexShader::~VertexShader() { DestroyShader(&device->core, code); }
PixelShader::~PixelShader() { DestroyShader(&device->core, code); }

// ================================================================ context
static VkCompareOp Compare(D3D11_COMPARISON_FUNC f)
{
    switch (f)
    {
    case D3D11_COMPARISON_NEVER: return VK_COMPARE_OP_NEVER;
    case D3D11_COMPARISON_LESS: return VK_COMPARE_OP_LESS;
    case D3D11_COMPARISON_EQUAL: return VK_COMPARE_OP_EQUAL;
    case D3D11_COMPARISON_LESS_EQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case D3D11_COMPARISON_GREATER: return VK_COMPARE_OP_GREATER;
    case D3D11_COMPARISON_NOT_EQUAL: return VK_COMPARE_OP_NOT_EQUAL;
    case D3D11_COMPARISON_GREATER_EQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}
static VkStencilOp StencilOp(D3D11_STENCIL_OP o)
{
    switch (o)
    {
    case D3D11_STENCIL_OP_ZERO: return VK_STENCIL_OP_ZERO;
    case D3D11_STENCIL_OP_REPLACE: return VK_STENCIL_OP_REPLACE;
    case D3D11_STENCIL_OP_INCR_SAT: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case D3D11_STENCIL_OP_DECR_SAT: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case D3D11_STENCIL_OP_INVERT: return VK_STENCIL_OP_INVERT;
    case D3D11_STENCIL_OP_INCR: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case D3D11_STENCIL_OP_DECR: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}
static VkBlendFactor Blend(D3D11_BLEND b)
{
    switch (b)
    {
    case D3D11_BLEND_ZERO: return VK_BLEND_FACTOR_ZERO;
    case D3D11_BLEND_ONE: return VK_BLEND_FACTOR_ONE;
    case D3D11_BLEND_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
    case D3D11_BLEND_INV_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case D3D11_BLEND_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
    case D3D11_BLEND_INV_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case D3D11_BLEND_DEST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
    case D3D11_BLEND_INV_DEST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case D3D11_BLEND_DEST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
    case D3D11_BLEND_INV_DEST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case D3D11_BLEND_SRC_ALPHA_SAT: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case D3D11_BLEND_BLEND_FACTOR: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case D3D11_BLEND_INV_BLEND_FACTOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case D3D11_BLEND_SRC1_COLOR: return VK_BLEND_FACTOR_SRC1_COLOR;
    case D3D11_BLEND_INV_SRC1_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case D3D11_BLEND_SRC1_ALPHA: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case D3D11_BLEND_INV_SRC1_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}
static VkBlendOp BlendOp(D3D11_BLEND_OP o)
{
    switch (o)
    {
    case D3D11_BLEND_OP_SUBTRACT: return VK_BLEND_OP_SUBTRACT;
    case D3D11_BLEND_OP_REV_SUBTRACT: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case D3D11_BLEND_OP_MIN: return VK_BLEND_OP_MIN;
    case D3D11_BLEND_OP_MAX: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}
static VkBlendFactor AlphaOnly(VkBlendFactor f)      // alpha factors: colour terms mean their alpha
{
    switch (f)
    {
    case VK_BLEND_FACTOR_SRC_COLOR: return VK_BLEND_FACTOR_SRC_ALPHA;
    case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case VK_BLEND_FACTOR_DST_COLOR: return VK_BLEND_FACTOR_DST_ALPHA;
    case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case VK_BLEND_FACTOR_CONSTANT_COLOR: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case VK_BLEND_FACTOR_SRC1_COLOR: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    default: return f;
    }
}

struct Context : Com<Stub_ID3D11DeviceContext1<ID3D11DeviceContext1>>
{
    Device* device;          // not AddRef'd: the device owns the context
    Core& core;
    static constexpr int MaxSrv = 64, MaxCb = 14, MaxSampler = 16, MaxVb = 16;

    // ---- state
    struct Cb { Ref<ID3D11Buffer> buffer; UINT first = 0, num = 0; bool ranged = false; };
    struct Stage
    {
        Cb cb[MaxCb];
        Ref<ID3D11ShaderResourceView> srv[MaxSrv];
        Ref<ID3D11SamplerState> sampler[MaxSampler];
        bool dirty = true;
    } vsStage, psStage;
    Ref<ID3D11VertexShader> vs;
    Ref<ID3D11PixelShader> ps;
    Ref<ID3D11InputLayout> il;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    struct Vb { Ref<ID3D11Buffer> buffer; UINT stride = 0, offset = 0; } vb[MaxVb];
    Ref<ID3D11Buffer> ib;
    DXGI_FORMAT ibFormat = DXGI_FORMAT_R16_UINT;
    UINT ibOffset = 0;
    Ref<ID3D11RenderTargetView> rtv[8];
    Ref<ID3D11DepthStencilView> dsv;
    D3D11_VIEWPORT viewports[16]{};
    UINT viewportCount = 0;
    D3D11_RECT scissors[16]{};
    UINT scissorCount = 0;
    Ref<ID3D11BlendState> blend;
    FLOAT blendFactor[4] = { 1, 1, 1, 1 };
    UINT sampleMask = 0xFFFFFFFFu;
    Ref<ID3D11DepthStencilState> depth;
    UINT stencilRef = 0;
    Ref<ID3D11RasterizerState> raster;

    // ---- recording
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> freeCmds;
    std::vector<VkFence> freeFences;
    std::vector<VkDescriptorPool> pools, freePools;      // pools: the open submission's
    bool inPass = false, targetsDirty = true;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkExtent2D passExtent{};
    VkPipeline boundPipeline = VK_NULL_HANDLE;
    VkPipelineLayout boundLayout = VK_NULL_HANDLE;
    VkDescriptorSet sets[2]{};
    std::map<std::vector<uint64_t>, VkRenderPass> renderPasses;
    std::map<std::vector<uint64_t>, VkFramebuffer> framebuffers;
    std::unordered_map<std::string, VkPipeline> pipelines;
    uint64_t commandsRecorded = 0;
    // Dummies for unbound slots.
    VkImage dummyImage = VK_NULL_HANDLE;
    VkDeviceMemory dummyMem = VK_NULL_HANDLE;
    VkImageView dummyView = VK_NULL_HANDLE, dummyArrayView = VK_NULL_HANDLE;
    VkBuffer dummyBuffer = VK_NULL_HANDLE;
    VkDeviceMemory dummyBufMem = VK_NULL_HANDLE;
    VkBufferView dummyTexel = VK_NULL_HANDLE;
    VkSampler dummySampler = VK_NULL_HANDLE;

    explicit Context(Device* d) : device(d), core(d->core) { refs = 0; }
    ~Context() override {}
    // The context's references count toward the device's (as D3D11's immediate context does).
    ULONG STDMETHODCALLTYPE AddRef() override { return device->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return device->Release(); }
    void* Find(REFIID riid) override
    {
        if (Same(riid, __uuidof(ID3D11DeviceContext)) || Same(riid, __uuidof(ID3D11DeviceContext1)) || Same(riid, __uuidof(ID3D11DeviceChild)))
            return static_cast<ID3D11DeviceContext1*>(this);
        return nullptr;
    }
    void STDMETHODCALLTYPE GetDevice(ID3D11Device** out) override { *out = AddRefd(static_cast<ID3D11Device*>(device)); }
    D3D11_DEVICE_CONTEXT_TYPE STDMETHODCALLTYPE GetType() override { return D3D11_DEVICE_CONTEXT_IMMEDIATE; }

    bool Init();

    // ---------------------------------------------------------------- submission
    void Poll()
    {
        std::lock_guard l(core.queueMutex);
        while (!core.inflight.empty() && vkGetFenceStatus(core.dev, core.inflight.front().fence) == VK_SUCCESS)
        {
            auto& f = core.inflight.front();
            core.completed = f.id;
            vkResetFences(core.dev, 1, &f.fence);
            freeFences.push_back(f.fence);
            vkResetCommandBuffer(f.cmd, 0);
            freeCmds.push_back(f.cmd);
            for (VkDescriptorPool p : f.pools) { vkResetDescriptorPool(core.dev, p, 0); freePools.push_back(p); }
            core.inflight.pop_front();
        }
    }
    void WaitFor(uint64_t id)
    {
        if (id == 0) return;
        if (id >= core.next.load()) Submit();
        while (core.completed.load() < id)
        {
            VkFence f = VK_NULL_HANDLE;
            {
                std::lock_guard l(core.queueMutex);
                for (auto& s : core.inflight) if (s.id >= id) { f = s.fence; break; }
            }
            if (!f) break;
            vkWaitForFences(core.dev, 1, &f, VK_TRUE, 5000000000ull);
            Poll();
        }
        core.RunDeferred();
    }
    VkCommandBuffer Cmd()
    {
        if (cmd) return cmd;
        Poll();
        core.RunDeferred();
        if (freeCmds.empty())
        {
            VkCommandBufferAllocateInfo a{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            a.commandPool = pool;
            a.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            a.commandBufferCount = 1;
            VkCommandBuffer c;
            vkAllocateCommandBuffers(core.dev, &a, &c);
            freeCmds.push_back(c);
        }
        cmd = freeCmds.back();
        freeCmds.pop_back();
        VkCommandBufferBeginInfo b{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        b.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &b);
        boundPipeline = VK_NULL_HANDLE;
        boundLayout = VK_NULL_HANDLE;
        vsStage.dirty = psStage.dirty = true;
        return cmd;
    }
    // Submits what was recorded (if anything). waitSem/signalSem: the present's.
    void Submit(VkSemaphore waitSem = VK_NULL_HANDLE, VkSemaphore signalSem = VK_NULL_HANDLE)
    {
        EndPass();
        if (!cmd && !waitSem && !signalSem) return;
        Cmd();
        FlushNewImages(false);
        vkEndCommandBuffer(cmd);
        VkFence fence;
        if (freeFences.empty())
        {
            VkFenceCreateInfo fi{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            vkCreateFence(core.dev, &fi, nullptr, &fence);
        }
        else { fence = freeFences.back(); freeFences.pop_back(); }
        VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        if (waitSem) { si.waitSemaphoreCount = 1; si.pWaitSemaphores = &waitSem; si.pWaitDstStageMask = &waitStage; }
        if (signalSem) { si.signalSemaphoreCount = 1; si.pSignalSemaphores = &signalSem; }
        {
            std::lock_guard l(core.queueMutex);
            const uint64_t id = core.next.load();
            const VkResult r = vkQueueSubmit(core.queue, 1, &si, fence);
            if (r != VK_SUCCESS) Log("vkQueueSubmit failed (%d)", int(r));
            core.inflight.push_back({ id, fence, cmd, std::move(pools) });
            core.next = id + 1;
        }
        pools.clear();
        cmd = VK_NULL_HANDLE;
        commandsRecorded = 0;
    }

    void Barrier()
    {
        VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(Cmd(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    }
    void EndPass()
    {
        if (!inPass) return;
        vkCmdEndRenderPass(cmd);
        inPass = false;
        Barrier();
    }
    // New images to GENERAL (outside any render pass).
    void FlushNewImages(bool endPass = true)
    {
        std::vector<std::pair<VkImage, VkImageAspectFlags>> list;
        {
            std::lock_guard l(device->newImagesMutex);
            if (device->newImages.empty()) return;
            list.swap(device->newImages);
        }
        if (endPass) EndPass();
        std::vector<VkImageMemoryBarrier> b(list.size());
        for (size_t i = 0; i < list.size(); ++i)
        {
            b[i] = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            b[i].srcAccessMask = 0;
            b[i].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            b[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b[i].srcQueueFamilyIndex = b[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b[i].image = list[i].first;
            b[i].subresourceRange = { list[i].second, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };
        }
        vkCmdPipelineBarrier(Cmd(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
                             uint32_t(b.size()), b.data());
    }
    // Before any transfer: outside the render pass, new images ready.
    VkCommandBuffer Transfer()
    {
        FlushNewImages();
        EndPass();
        ++commandsRecorded;
        return Cmd();
    }
    void AfterTransfer() { Barrier(); }

    // ---------------------------------------------------------------- descriptors
    VkDescriptorSet AllocateSet(VkDescriptorSetLayout layout)
    {
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            if (!pools.empty())
            {
                VkDescriptorSetAllocateInfo a{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
                a.descriptorPool = pools.back();
                a.descriptorSetCount = 1;
                a.pSetLayouts = &layout;
                VkDescriptorSet s;
                if (vkAllocateDescriptorSets(core.dev, &a, &s) == VK_SUCCESS) return s;
            }
            VkDescriptorPool p;
            if (!freePools.empty()) { p = freePools.back(); freePools.pop_back(); }
            else
            {
                const VkDescriptorPoolSize sizes[] = {
                    { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8192 }, { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8192 },
                    { VK_DESCRIPTOR_TYPE_SAMPLER, 4096 }, { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 2048 },
                    { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 512 } };
                VkDescriptorPoolCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
                ci.maxSets = 4096;
                ci.poolSizeCount = sizeof(sizes) / sizeof(sizes[0]);
                ci.pPoolSizes = sizes;
                vkCreateDescriptorPool(core.dev, &ci, nullptr, &p);
            }
            pools.push_back(p);
        }
        return VK_NULL_HANDLE;
    }
    VkDescriptorSet WriteSet(const ShaderCode& sc, Stage& st)
    {
        if (!sc.layout || sc.bindings.empty()) return VK_NULL_HANDLE;
        VkDescriptorSet set = AllocateSet(sc.layout);
        if (!set) return VK_NULL_HANDLE;
        std::vector<VkWriteDescriptorSet> writes(sc.bindings.size());
        std::vector<VkDescriptorBufferInfo> bufs(sc.bindings.size());
        std::vector<VkDescriptorImageInfo> imgs(sc.bindings.size());
        std::vector<VkBufferView> texels(sc.bindings.size());
        const VkDeviceSize maxUbo = core.props.limits.maxUniformBufferRange;
        for (size_t i = 0; i < sc.bindings.size(); ++i)
        {
            const Binding& b = sc.bindings[i];
            VkWriteDescriptorSet& w = writes[i];
            w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstSet = set;
            w.dstBinding = b.binding & 0x7FFF;
            w.descriptorCount = 1;
            w.descriptorType = b.type;
            if (b.kind == 0)
            {
                VkDescriptorBufferInfo& bi = bufs[i];
                Buffer* buf = b.reg < MaxCb ? static_cast<Buffer*>(st.cb[b.reg].buffer.get()) : nullptr;
                if (buf && buf->Handle())
                {
                    bi.buffer = buf->Handle();
                    const Cb& c = st.cb[b.reg];
                    const VkDeviceSize first = c.ranged ? VkDeviceSize(c.first) * 16 : 0;
                    VkDeviceSize range = c.ranged ? VkDeviceSize(c.num) * 16 : buf->desc.ByteWidth;
                    if (first + range > buf->desc.ByteWidth) range = buf->desc.ByteWidth > first ? buf->desc.ByteWidth - first : 16;
                    bi.offset = buf->Base() + first;
                    bi.range = std::min(range, maxUbo);
                }
                else { bi.buffer = dummyBuffer; bi.offset = 0; bi.range = 65536 < maxUbo ? 65536 : maxUbo; }
                w.pBufferInfo = &bi;
            }
            else if (b.kind == 1)
            {
                Srv* v = b.reg < MaxSrv ? static_cast<Srv*>(st.srv[b.reg].get()) : nullptr;
                if (b.type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER)
                {
                    VkBufferView bv = v ? v->BufferView() : VK_NULL_HANDLE;
                    texels[i] = bv ? bv : dummyTexel;
                    w.pTexelBufferView = &texels[i];
                }
                else
                {
                    imgs[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                    imgs[i].imageView = v && v->view ? v->view : (b.binding & 0x8000 ? dummyArrayView : dummyView);
                    w.pImageInfo = &imgs[i];
                }
            }
            else
            {
                Sampler* s = b.reg < MaxSampler ? static_cast<Sampler*>(st.sampler[b.reg].get()) : nullptr;
                imgs[i].sampler = s ? s->sampler : dummySampler;
                w.pImageInfo = &imgs[i];
            }
        }
        vkUpdateDescriptorSets(core.dev, uint32_t(writes.size()), writes.data(), 0, nullptr);
        return set;
    }

    // ---------------------------------------------------------------- render pass
    VkRenderPass RenderPass(const std::vector<VkFormat>& colors, VkFormat depthFmt)
    {
        std::vector<uint64_t> key;
        for (VkFormat f : colors) key.push_back(uint64_t(f));
        key.push_back(0xD0000000ull | uint64_t(depthFmt));
        auto it = renderPasses.find(key);
        if (it != renderPasses.end()) return it->second;
        std::vector<VkAttachmentDescription> att;
        std::vector<VkAttachmentReference> refs(colors.size());
        for (size_t i = 0; i < colors.size(); ++i)
        {
            if (colors[i] == VK_FORMAT_UNDEFINED) { refs[i] = { VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_GENERAL }; continue; }
            VkAttachmentDescription a{};
            a.format = colors[i];
            a.samples = VK_SAMPLE_COUNT_1_BIT;
            a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            a.initialLayout = a.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
            refs[i] = { uint32_t(att.size()), VK_IMAGE_LAYOUT_GENERAL };
            att.push_back(a);
        }
        VkAttachmentReference dref{ VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_GENERAL };
        if (depthFmt != VK_FORMAT_UNDEFINED)
        {
            VkAttachmentDescription a{};
            a.format = depthFmt;
            a.samples = VK_SAMPLE_COUNT_1_BIT;
            a.loadOp = a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            a.storeOp = a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
            a.initialLayout = a.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
            dref = { uint32_t(att.size()), VK_IMAGE_LAYOUT_GENERAL };
            att.push_back(a);
        }
        VkSubpassDescription sp{};
        sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount = uint32_t(refs.size());
        sp.pColorAttachments = refs.data();
        sp.pDepthStencilAttachment = &dref;
        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL; deps[0].dstSubpass = 0;
        deps[1].srcSubpass = 0; deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        for (auto& d : deps)
        {
            d.srcStageMask = d.dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            d.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            d.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        }
        VkRenderPassCreateInfo ci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        ci.attachmentCount = uint32_t(att.size());
        ci.pAttachments = att.data();
        ci.subpassCount = 1;
        ci.pSubpasses = &sp;
        ci.dependencyCount = 2;
        ci.pDependencies = deps;
        VkRenderPass rp = VK_NULL_HANDLE;
        vkCreateRenderPass(core.dev, &ci, nullptr, &rp);
        renderPasses[key] = rp;
        return rp;
    }
    bool BeginPass()
    {
        if (inPass && !targetsDirty) return true;
        EndPass();
        targetsDirty = false;
        std::vector<VkFormat> colors;
        std::vector<VkImageView> views;
        uint32_t w = ~0u, h = ~0u;
        int last = -1;
        for (int i = 0; i < 8; ++i) if (rtv[i]) last = i;
        for (int i = 0; i <= last; ++i)
        {
            Rtv* r = static_cast<Rtv*>(rtv[i].get());
            if (!r) { colors.push_back(VK_FORMAT_UNDEFINED); continue; }
            colors.push_back(r->format);
            views.push_back(r->view);
            w = std::min(w, r->tex->W(r->mip));
            h = std::min(h, r->tex->H(r->mip));
        }
        VkFormat df = VK_FORMAT_UNDEFINED;
        if (Dsv* d = static_cast<Dsv*>(dsv.get()))
        {
            df = d->format;
            views.push_back(d->view);
            w = std::min(w, d->tex->W(d->mip));
            h = std::min(h, d->tex->H(d->mip));
        }
        if (views.empty()) return false;
        pass = RenderPass(colors, df);
        std::vector<uint64_t> fkey{ uint64_t(reinterpret_cast<uintptr_t>(pass)), w, h };
        for (VkImageView v : views) fkey.push_back(uint64_t(reinterpret_cast<uintptr_t>(v)));
        VkFramebuffer fb;
        auto it = framebuffers.find(fkey);
        if (it != framebuffers.end()) fb = it->second;
        else
        {
            VkFramebufferCreateInfo ci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            ci.renderPass = pass;
            ci.attachmentCount = uint32_t(views.size());
            ci.pAttachments = views.data();
            ci.width = w;
            ci.height = h;
            ci.layers = 1;
            vkCreateFramebuffer(core.dev, &ci, nullptr, &fb);
            // Views are destroyed and their handles reused: framebuffers are dropped when there are many.
            if (framebuffers.size() > 2048) DropFramebuffers();
            framebuffers[fkey] = fb;
        }
        VkRenderPassBeginInfo bi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        bi.renderPass = pass;
        bi.framebuffer = fb;
        bi.renderArea = { { 0, 0 }, { w, h } };
        FlushNewImages();
        vkCmdBeginRenderPass(Cmd(), &bi, VK_SUBPASS_CONTENTS_INLINE);
        inPass = true;
        passExtent = { w, h };
        boundPipeline = VK_NULL_HANDLE;
        return true;
    }
    void DropFramebuffers()
    {
        std::vector<VkFramebuffer> old;
        for (auto& [k, f] : framebuffers) old.push_back(f);
        framebuffers.clear();
        Core* c = &core;
        c->Defer([c, old] { for (VkFramebuffer f : old) vkDestroyFramebuffer(c->dev, f, nullptr); });
    }
    // A view went away: its framebuffers too (a new view may reuse the handle).
    void ForgetView(VkImageView v)
    {
        const uint64_t key = uint64_t(reinterpret_cast<uintptr_t>(v));
        std::vector<VkFramebuffer> old;
        for (auto it = framebuffers.begin(); it != framebuffers.end();)
        {
            if (std::find(it->first.begin() + 3, it->first.end(), key) != it->first.end()) { old.push_back(it->second); it = framebuffers.erase(it); }
            else ++it;
        }
        if (old.empty()) return;
        Core* c = &core;
        c->Defer([c, old] { for (VkFramebuffer f : old) vkDestroyFramebuffer(c->dev, f, nullptr); });
    }

    // ---------------------------------------------------------------- pipelines
    VkPipeline Pipeline(VkPipelineLayout layout)
    {
        auto* v = static_cast<VertexShader*>(vs.get());
        auto* p = static_cast<PixelShader*>(ps.get());
        auto* l = static_cast<InputLayout*>(il.get());
        auto* b = static_cast<BlendState*>(blend.get());
        auto* r = static_cast<RasterState*>(raster.get());
        auto* d = static_cast<DepthState*>(depth.get());
        D3D11_BLEND_DESC bd{};
        if (b) bd = b->desc;
        else { bd.RenderTarget[0].RenderTargetWriteMask = 0xF; for (auto& t : bd.RenderTarget) t.RenderTargetWriteMask = 0xF; }
        D3D11_RASTERIZER_DESC rd{};
        if (r) rd = r->desc;
        else { rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_BACK; rd.DepthClipEnable = TRUE; }
        D3D11_DEPTH_STENCIL_DESC dd{};
        if (d) dd = d->desc;
        else { dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dd.DepthFunc = D3D11_COMPARISON_LESS; dd.StencilReadMask = dd.StencilWriteMask = 0xFF; }
        // Key: everything the pipeline is made of.
        std::string key;
        auto add = [&](const void* data, size_t n) { key.append(static_cast<const char*>(data), n); };
        const uint64_t ids[4] = { v ? v->code.id : 0, p ? p->code.id : 0, l ? l->id : 0, uint64_t(reinterpret_cast<uintptr_t>(pass)) };
        add(ids, sizeof(ids));
        add(&topology, sizeof(topology));
        add(&bd, sizeof(bd));
        add(&rd, sizeof(rd));
        add(&dd, sizeof(dd));
        add(&sampleMask, sizeof(sampleMask));
        if (l) for (auto& a : l->attrs) add(&vb[a.slot].stride, sizeof(UINT));
        auto it = pipelines.find(key);
        if (it != pipelines.end()) return it->second;

        VkPipelineShaderStageCreateInfo stages[2]{};
        uint32_t stageCount = 0;
        stages[stageCount] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        stages[stageCount].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[stageCount].module = v->code.module;
        stages[stageCount].pName = "main";
        ++stageCount;
        if (p)
        {
            stages[stageCount] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
            stages[stageCount].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            stages[stageCount].module = p->code.module;
            stages[stageCount].pName = "main";
            ++stageCount;
        }
        std::vector<VkVertexInputBindingDescription> vbind;
        std::vector<VkVertexInputAttributeDescription> vattr;
        if (l)
            for (auto& a : l->attrs)
            {
                vattr.push_back({ a.location, a.slot, a.format, a.offset });
                bool have = false;
                for (auto& bnd : vbind) if (bnd.binding == a.slot) have = true;
                if (!have) vbind.push_back({ a.slot, vb[a.slot].stride, a.instance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX });
            }
        VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vi.vertexBindingDescriptionCount = uint32_t(vbind.size());
        vi.pVertexBindingDescriptions = vbind.data();
        vi.vertexAttributeDescriptionCount = uint32_t(vattr.size());
        vi.pVertexAttributeDescriptions = vattr.data();
        VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        switch (topology)
        {
        case D3D11_PRIMITIVE_TOPOLOGY_POINTLIST: ia.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
        case D3D11_PRIMITIVE_TOPOLOGY_LINELIST: ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
        case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP: ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; ia.primitiveRestartEnable = VK_TRUE; break;
        case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; ia.primitiveRestartEnable = VK_TRUE; break;
        default: ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
        }
        VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.depthClampEnable = (!rd.DepthClipEnable && core.enabled.depthClamp) ? VK_TRUE : VK_FALSE;
        rs.polygonMode = (rd.FillMode == D3D11_FILL_WIREFRAME && core.enabled.fillModeNonSolid) ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
        rs.cullMode = rd.CullMode == D3D11_CULL_FRONT ? VK_CULL_MODE_FRONT_BIT : rd.CullMode == D3D11_CULL_BACK ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
        // The viewport is flipped (negative height), so D3D's winding names hold as they are.
        rs.frontFace = rd.FrontCounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
        rs.depthBiasEnable = (rd.DepthBias != 0 || rd.SlopeScaledDepthBias != 0.0f) ? VK_TRUE : VK_FALSE;
        rs.depthBiasConstantFactor = float(rd.DepthBias);
        rs.depthBiasSlopeFactor = rd.SlopeScaledDepthBias;
        rs.depthBiasClamp = core.enabled.depthBiasClamp ? rd.DepthBiasClamp : 0.0f;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        const VkSampleMask mask = sampleMask;
        ms.pSampleMask = &mask;
        ms.alphaToCoverageEnable = bd.AlphaToCoverageEnable ? VK_TRUE : VK_FALSE;
        VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        const bool hasDepth = dsv.get() != nullptr;
        ds.depthTestEnable = (hasDepth && dd.DepthEnable) ? VK_TRUE : VK_FALSE;
        ds.depthWriteEnable = (hasDepth && dd.DepthEnable && dd.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL) ? VK_TRUE : VK_FALSE;
        ds.depthCompareOp = Compare(dd.DepthFunc);
        ds.stencilTestEnable = (hasDepth && dd.StencilEnable) ? VK_TRUE : VK_FALSE;
        auto face = [&](const D3D11_DEPTH_STENCILOP_DESC& f) {
            VkStencilOpState s{};
            s.failOp = StencilOp(f.StencilFailOp);
            s.passOp = StencilOp(f.StencilPassOp);
            s.depthFailOp = StencilOp(f.StencilDepthFailOp);
            s.compareOp = Compare(f.StencilFunc);
            s.compareMask = dd.StencilReadMask;
            s.writeMask = dd.StencilWriteMask;
            return s;
        };
        ds.front = face(dd.FrontFace);
        ds.back = face(dd.BackFace);
        ds.minDepthBounds = 0.0f;
        ds.maxDepthBounds = 1.0f;
        int colorCount = 0;
        for (int i = 0; i < 8; ++i) if (rtv[i]) colorCount = i + 1;
        VkPipelineColorBlendAttachmentState cba[8]{};
        for (int i = 0; i < colorCount; ++i)
        {
            const D3D11_RENDER_TARGET_BLEND_DESC& t = bd.RenderTarget[bd.IndependentBlendEnable ? i : 0];
            cba[i].blendEnable = t.BlendEnable ? VK_TRUE : VK_FALSE;
            cba[i].srcColorBlendFactor = Blend(t.SrcBlend);
            cba[i].dstColorBlendFactor = Blend(t.DestBlend);
            cba[i].colorBlendOp = BlendOp(t.BlendOp);
            cba[i].srcAlphaBlendFactor = AlphaOnly(Blend(t.SrcBlendAlpha));
            cba[i].dstAlphaBlendFactor = AlphaOnly(Blend(t.DestBlendAlpha));
            cba[i].alphaBlendOp = BlendOp(t.BlendOpAlpha);
            cba[i].colorWriteMask = t.RenderTargetWriteMask & 0xF;
        }
        VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = uint32_t(colorCount);
        cb.pAttachments = cba;
        const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_REFERENCE };
        VkPipelineDynamicStateCreateInfo dy{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dy.dynamicStateCount = 4;
        dy.pDynamicStates = dyn;
        VkGraphicsPipelineCreateInfo ci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        ci.stageCount = stageCount;
        ci.pStages = stages;
        ci.pVertexInputState = &vi;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms;
        ci.pDepthStencilState = &ds;
        ci.pColorBlendState = &cb;
        ci.pDynamicState = &dy;
        ci.layout = layout;
        ci.renderPass = pass;
        ci.subpass = 0;
        VkPipeline pl = VK_NULL_HANDLE;
        const VkResult res = vkCreateGraphicsPipelines(core.dev, core.pipelineCache, 1, &ci, nullptr, &pl);
        if (res != VK_SUCCESS) { Log("vkCreateGraphicsPipelines failed (%d)", int(res)); pl = VK_NULL_HANDLE; }
        pipelines[key] = pl;
        return pl;
    }

    // ---------------------------------------------------------------- draws
    bool PrepareDraw()
    {
        auto* v = static_cast<VertexShader*>(vs.get());
        if (!v || !v->code.module) return false;
        FlushNewImages();
        if (!BeginPass()) return false;
        auto* p = static_cast<PixelShader*>(ps.get());
        const VkPipelineLayout layout = device->PipelineLayout(v->code.layout, p ? p->code.layout : VK_NULL_HANDLE);
        const VkPipeline pl = Pipeline(layout);
        if (!pl) return false;
        VkCommandBuffer c = cmd;
        if (pl != boundPipeline) { vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pl); boundPipeline = pl; }
        if (layout != boundLayout) { boundLayout = layout; vsStage.dirty = psStage.dirty = true; }
        if (vsStage.dirty) { sets[0] = WriteSet(v->code, vsStage); vsStage.dirty = false; if (sets[0]) vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &sets[0], 0, nullptr); }
        if (psStage.dirty) { sets[1] = p ? WriteSet(p->code, psStage) : VK_NULL_HANDLE; psStage.dirty = false; if (sets[1]) vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &sets[1], 0, nullptr); }
        // Viewport (flipped: D3D's y grows down the screen, Vulkan's clip y the other way) and scissor.
        VkViewport vpt{};
        if (viewportCount)
        {
            const D3D11_VIEWPORT& d = viewports[0];
            vpt = { d.TopLeftX, d.TopLeftY + d.Height, d.Width, -d.Height, d.MinDepth, d.MaxDepth };
        }
        else vpt = { 0, float(passExtent.height), float(passExtent.width), -float(passExtent.height), 0, 1 };
        if (vpt.width <= 0) vpt.width = 1;
        vkCmdSetViewport(c, 0, 1, &vpt);
        VkRect2D sc{ { 0, 0 }, passExtent };
        auto* r = static_cast<RasterState*>(raster.get());
        if (r && r->desc.ScissorEnable && scissorCount)
        {
            const D3D11_RECT& s = scissors[0];
            const LONG l = std::max<LONG>(0, s.left), t = std::max<LONG>(0, s.top);
            const LONG rr = std::min<LONG>(LONG(passExtent.width), s.right), bb = std::min<LONG>(LONG(passExtent.height), s.bottom);
            sc = { { int32_t(l), int32_t(t) }, { uint32_t(std::max<LONG>(0, rr - l)), uint32_t(std::max<LONG>(0, bb - t)) } };
        }
        vkCmdSetScissor(c, 0, 1, &sc);
        vkCmdSetBlendConstants(c, blendFactor);
        vkCmdSetStencilReference(c, VK_STENCIL_FACE_FRONT_AND_BACK, stencilRef);
        if (auto* l = static_cast<InputLayout*>(il.get()))
            for (auto& a : l->attrs)
            {
                Buffer* b = static_cast<Buffer*>(vb[a.slot].buffer.get());
                VkBuffer h = b ? b->Handle() : dummyBuffer;
                VkDeviceSize off = b ? b->Base() + vb[a.slot].offset : 0;
                vkCmdBindVertexBuffers(c, a.slot, 1, &h, &off);
            }
        ++commandsRecorded;
        return true;
    }
    void STDMETHODCALLTYPE Draw(UINT count, UINT start) override
    {
        if (PrepareDraw()) vkCmdDraw(cmd, count, 1, start, 0);
    }
    void STDMETHODCALLTYPE DrawIndexed(UINT count, UINT start, INT base) override
    {
        Buffer* b = static_cast<Buffer*>(ib.get());
        if (!b || !PrepareDraw()) return;
        vkCmdBindIndexBuffer(cmd, b->Handle(), b->Base() + ibOffset, ibFormat == DXGI_FORMAT_R32_UINT ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(cmd, count, 1, start, base, 0);
    }
    void STDMETHODCALLTYPE DrawInstanced(UINT count, UINT instances, UINT start, UINT startInstance) override
    {
        if (PrepareDraw()) vkCmdDraw(cmd, count, instances, start, startInstance);
    }
    void STDMETHODCALLTYPE DrawIndexedInstanced(UINT count, UINT instances, UINT start, INT base, UINT startInstance) override
    {
        Buffer* b = static_cast<Buffer*>(ib.get());
        if (!b || !PrepareDraw()) return;
        vkCmdBindIndexBuffer(cmd, b->Handle(), b->Base() + ibOffset, ibFormat == DXGI_FORMAT_R32_UINT ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(cmd, count, instances, start, base, startInstance);
    }

    // ---------------------------------------------------------------- state setters
    void SetCbs(Stage& st, UINT start, UINT n, ID3D11Buffer* const* b, const UINT* first, const UINT* num)
    {
        for (UINT i = 0; i < n && start + i < MaxCb; ++i)
        {
            Cb& c = st.cb[start + i];
            c.buffer = b ? b[i] : nullptr;
            c.ranged = first && num;
            c.first = first ? first[i] : 0;
            c.num = num ? num[i] : 0;
        }
        st.dirty = true;
    }
    void STDMETHODCALLTYPE VSSetConstantBuffers(UINT s, UINT n, ID3D11Buffer* const* b) override { SetCbs(vsStage, s, n, b, nullptr, nullptr); }
    void STDMETHODCALLTYPE PSSetConstantBuffers(UINT s, UINT n, ID3D11Buffer* const* b) override { SetCbs(psStage, s, n, b, nullptr, nullptr); }
    void STDMETHODCALLTYPE VSSetConstantBuffers1(UINT s, UINT n, ID3D11Buffer* const* b, const UINT* f, const UINT* c) override { SetCbs(vsStage, s, n, b, f, c); }
    void STDMETHODCALLTYPE PSSetConstantBuffers1(UINT s, UINT n, ID3D11Buffer* const* b, const UINT* f, const UINT* c) override { SetCbs(psStage, s, n, b, f, c); }
    void SetSrvs(Stage& st, UINT start, UINT n, ID3D11ShaderResourceView* const* v)
    {
        for (UINT i = 0; i < n && start + i < MaxSrv; ++i) st.srv[start + i] = v ? v[i] : nullptr;
        st.dirty = true;
    }
    void STDMETHODCALLTYPE VSSetShaderResources(UINT s, UINT n, ID3D11ShaderResourceView* const* v) override { SetSrvs(vsStage, s, n, v); }
    void STDMETHODCALLTYPE PSSetShaderResources(UINT s, UINT n, ID3D11ShaderResourceView* const* v) override { SetSrvs(psStage, s, n, v); }
    void SetSamplers(Stage& st, UINT start, UINT n, ID3D11SamplerState* const* v)
    {
        for (UINT i = 0; i < n && start + i < MaxSampler; ++i) st.sampler[start + i] = v ? v[i] : nullptr;
        st.dirty = true;
    }
    void STDMETHODCALLTYPE VSSetSamplers(UINT s, UINT n, ID3D11SamplerState* const* v) override { SetSamplers(vsStage, s, n, v); }
    void STDMETHODCALLTYPE PSSetSamplers(UINT s, UINT n, ID3D11SamplerState* const* v) override { SetSamplers(psStage, s, n, v); }
    void STDMETHODCALLTYPE VSSetShader(ID3D11VertexShader* s, ID3D11ClassInstance* const*, UINT) override { if (s != vs.get()) { vs = s; vsStage.dirty = true; } }
    void STDMETHODCALLTYPE PSSetShader(ID3D11PixelShader* s, ID3D11ClassInstance* const*, UINT) override { if (s != ps.get()) { ps = s; psStage.dirty = true; } }
    void STDMETHODCALLTYPE IASetInputLayout(ID3D11InputLayout* l) override { il = l; }
    void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY t) override { topology = t; }
    void STDMETHODCALLTYPE IASetVertexBuffers(UINT s, UINT n, ID3D11Buffer* const* b, const UINT* strides, const UINT* offsets) override
    {
        for (UINT i = 0; i < n && s + i < MaxVb; ++i)
        {
            vb[s + i].buffer = b ? b[i] : nullptr;
            vb[s + i].stride = strides ? strides[i] : 0;
            vb[s + i].offset = offsets ? offsets[i] : 0;
        }
    }
    void STDMETHODCALLTYPE IASetIndexBuffer(ID3D11Buffer* b, DXGI_FORMAT f, UINT o) override { ib = b; ibFormat = f; ibOffset = o; }
    void STDMETHODCALLTYPE OMSetRenderTargets(UINT n, ID3D11RenderTargetView* const* v, ID3D11DepthStencilView* d) override
    {
        bool changed = d != dsv.get();
        for (UINT i = 0; i < 8; ++i)
        {
            ID3D11RenderTargetView* nv = (v && i < n) ? v[i] : nullptr;
            if (nv != rtv[i].get()) { rtv[i] = nv; changed = true; }
        }
        dsv = d;
        if (changed) targetsDirty = true;
    }
    void STDMETHODCALLTYPE OMSetBlendState(ID3D11BlendState* b, const FLOAT f[4], UINT mask) override
    {
        blend = b;
        if (f) std::memcpy(blendFactor, f, sizeof(blendFactor));
        else blendFactor[0] = blendFactor[1] = blendFactor[2] = blendFactor[3] = 1.0f;
        sampleMask = mask;
    }
    void STDMETHODCALLTYPE OMSetDepthStencilState(ID3D11DepthStencilState* d, UINT ref) override { depth = d; stencilRef = ref; }
    void STDMETHODCALLTYPE RSSetState(ID3D11RasterizerState* r) override { raster = r; }
    void STDMETHODCALLTYPE RSSetViewports(UINT n, const D3D11_VIEWPORT* v) override
    {
        viewportCount = std::min<UINT>(n, 16);
        for (UINT i = 0; i < viewportCount; ++i) viewports[i] = v[i];
    }
    void STDMETHODCALLTYPE RSSetScissorRects(UINT n, const D3D11_RECT* r) override
    {
        scissorCount = std::min<UINT>(n, 16);
        for (UINT i = 0; i < scissorCount; ++i) scissors[i] = r[i];
    }

    // ---------------------------------------------------------------- getters (the renderer saves / restores state)
    template <class T> static void Out(T* const& in, T** out) { *out = AddRefd(in); }
    void STDMETHODCALLTYPE VSGetShader(ID3D11VertexShader** s, ID3D11ClassInstance**, UINT* n) override { Out(vs.p, s); if (n) *n = 0; }
    void STDMETHODCALLTYPE PSGetShader(ID3D11PixelShader** s, ID3D11ClassInstance**, UINT* n) override { Out(ps.p, s); if (n) *n = 0; }
    void GetCbs(Stage& st, UINT s, UINT n, ID3D11Buffer** b, UINT* f, UINT* c)
    {
        for (UINT i = 0; i < n; ++i)
        {
            const bool in = s + i < MaxCb;
            if (b) b[i] = in ? AddRefd(st.cb[s + i].buffer.get()) : nullptr;
            if (f) f[i] = in && st.cb[s + i].ranged ? st.cb[s + i].first : 0;
            if (c) c[i] = in && st.cb[s + i].ranged ? st.cb[s + i].num : 4096;
        }
    }
    void STDMETHODCALLTYPE VSGetConstantBuffers(UINT s, UINT n, ID3D11Buffer** b) override { GetCbs(vsStage, s, n, b, nullptr, nullptr); }
    void STDMETHODCALLTYPE PSGetConstantBuffers(UINT s, UINT n, ID3D11Buffer** b) override { GetCbs(psStage, s, n, b, nullptr, nullptr); }
    void STDMETHODCALLTYPE VSGetConstantBuffers1(UINT s, UINT n, ID3D11Buffer** b, UINT* f, UINT* c) override { GetCbs(vsStage, s, n, b, f, c); }
    void STDMETHODCALLTYPE PSGetConstantBuffers1(UINT s, UINT n, ID3D11Buffer** b, UINT* f, UINT* c) override { GetCbs(psStage, s, n, b, f, c); }
    void STDMETHODCALLTYPE VSGetShaderResources(UINT s, UINT n, ID3D11ShaderResourceView** v) override { for (UINT i = 0; i < n; ++i) v[i] = s + i < MaxSrv ? AddRefd(vsStage.srv[s + i].get()) : nullptr; }
    void STDMETHODCALLTYPE PSGetShaderResources(UINT s, UINT n, ID3D11ShaderResourceView** v) override { for (UINT i = 0; i < n; ++i) v[i] = s + i < MaxSrv ? AddRefd(psStage.srv[s + i].get()) : nullptr; }
    void STDMETHODCALLTYPE VSGetSamplers(UINT s, UINT n, ID3D11SamplerState** v) override { for (UINT i = 0; i < n; ++i) v[i] = s + i < MaxSampler ? AddRefd(vsStage.sampler[s + i].get()) : nullptr; }
    void STDMETHODCALLTYPE PSGetSamplers(UINT s, UINT n, ID3D11SamplerState** v) override { for (UINT i = 0; i < n; ++i) v[i] = s + i < MaxSampler ? AddRefd(psStage.sampler[s + i].get()) : nullptr; }
    void STDMETHODCALLTYPE IAGetInputLayout(ID3D11InputLayout** l) override { Out(il.p, l); }
    void STDMETHODCALLTYPE IAGetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY* t) override { *t = topology; }
    void STDMETHODCALLTYPE IAGetVertexBuffers(UINT s, UINT n, ID3D11Buffer** b, UINT* strides, UINT* offsets) override
    {
        for (UINT i = 0; i < n; ++i)
        {
            const bool in = s + i < MaxVb;
            if (b) b[i] = in ? AddRefd(vb[s + i].buffer.get()) : nullptr;
            if (strides) strides[i] = in ? vb[s + i].stride : 0;
            if (offsets) offsets[i] = in ? vb[s + i].offset : 0;
        }
    }
    void STDMETHODCALLTYPE IAGetIndexBuffer(ID3D11Buffer** b, DXGI_FORMAT* f, UINT* o) override { if (b) *b = AddRefd(ib.get()); if (f) *f = ibFormat; if (o) *o = ibOffset; }
    void STDMETHODCALLTYPE RSGetState(ID3D11RasterizerState** r) override { Out(raster.p, r); }
    void STDMETHODCALLTYPE RSGetViewports(UINT* n, D3D11_VIEWPORT* v) override
    {
        if (v) for (UINT i = 0; i < std::min(*n, viewportCount); ++i) v[i] = viewports[i];
        *n = viewportCount;
    }
    void STDMETHODCALLTYPE RSGetScissorRects(UINT* n, D3D11_RECT* r) override
    {
        if (r) for (UINT i = 0; i < std::min(*n, scissorCount); ++i) r[i] = scissors[i];
        *n = scissorCount;
    }
    void STDMETHODCALLTYPE OMGetRenderTargets(UINT n, ID3D11RenderTargetView** v, ID3D11DepthStencilView** d) override
    {
        if (v) for (UINT i = 0; i < n; ++i) v[i] = i < 8 ? AddRefd(rtv[i].get()) : nullptr;
        if (d) *d = AddRefd(dsv.get());
    }
    void STDMETHODCALLTYPE OMGetBlendState(ID3D11BlendState** b, FLOAT f[4], UINT* mask) override
    {
        if (b) *b = AddRefd(blend.get());
        if (f) std::memcpy(f, blendFactor, sizeof(blendFactor));
        if (mask) *mask = sampleMask;
    }
    void STDMETHODCALLTYPE OMGetDepthStencilState(ID3D11DepthStencilState** d, UINT* ref) override { if (d) *d = AddRefd(depth.get()); if (ref) *ref = stencilRef; }

    void STDMETHODCALLTYPE ClearState() override
    {
        for (Stage* st : { &vsStage, &psStage })
        {
            for (auto& c : st->cb) c = Cb{};
            for (auto& v : st->srv) v = nullptr;
            for (auto& s : st->sampler) s = nullptr;
            st->dirty = true;
        }
        vs = nullptr; ps = nullptr; il = nullptr;
        topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        for (auto& b : vb) b = Vb{};
        ib = nullptr;
        for (auto& r : rtv) r = nullptr;
        dsv = nullptr;
        targetsDirty = true;
        viewportCount = scissorCount = 0;
        blend = nullptr; depth = nullptr; raster = nullptr;
        blendFactor[0] = blendFactor[1] = blendFactor[2] = blendFactor[3] = 1.0f;
        sampleMask = 0xFFFFFFFFu;
        stencilRef = 0;
    }

    // ---------------------------------------------------------------- clears, copies, uploads
    void STDMETHODCALLTYPE ClearRenderTargetView(ID3D11RenderTargetView* v, const FLOAT color[4]) override
    {
        Rtv* r = static_cast<Rtv*>(v);
        if (!r || !r->tex) return;
        VkCommandBuffer c = Transfer();
        VkClearColorValue cv{};
        std::memcpy(cv.float32, color, sizeof(cv.float32));
        if (r->format == VK_FORMAT_R32G32B32A32_UINT || r->format == VK_FORMAT_R32_UINT || r->format == VK_FORMAT_R8G8B8A8_UINT)
            for (int i = 0; i < 4; ++i) cv.uint32[i] = uint32_t(color[i]);
        const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, r->mip, 1, r->layer, 1 };
        vkCmdClearColorImage(c, r->tex->image, VK_IMAGE_LAYOUT_GENERAL, &cv, 1, &range);
        AfterTransfer();
    }
    void STDMETHODCALLTYPE ClearDepthStencilView(ID3D11DepthStencilView* v, UINT flags, FLOAT depthValue, UINT8 stencil) override
    {
        Dsv* d = static_cast<Dsv*>(v);
        if (!d || !d->tex) return;
        VkCommandBuffer c = Transfer();
        VkClearDepthStencilValue cv{ depthValue, stencil };
        VkImageAspectFlags aspect = 0;
        if (flags & D3D11_CLEAR_DEPTH) aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if ((flags & D3D11_CLEAR_STENCIL) && (FullAspect(d->format) & VK_IMAGE_ASPECT_STENCIL_BIT)) aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
        if (!aspect) return;
        const VkImageSubresourceRange range{ aspect, d->mip, 1, d->layer, 1 };
        vkCmdClearDepthStencilImage(c, d->tex->image, VK_IMAGE_LAYOUT_GENERAL, &cv, 1, &range);
        AfterTransfer();
    }
    static bool IsBuffer(ID3D11Resource* r) { D3D11_RESOURCE_DIMENSION t; r->GetType(&t); return t == D3D11_RESOURCE_DIMENSION_BUFFER; }
    void STDMETHODCALLTYPE CopySubresourceRegion(ID3D11Resource* dst, UINT dstSub, UINT dx, UINT dy, UINT dz, ID3D11Resource* src, UINT srcSub, const D3D11_BOX* box) override
    {
        if (!dst || !src) return;
        if (IsBuffer(dst) && IsBuffer(src))
        {
            Buffer* d = static_cast<Buffer*>(static_cast<ID3D11Buffer*>(dst));
            Buffer* s = static_cast<Buffer*>(static_cast<ID3D11Buffer*>(src));
            const UINT from = box ? box->left : 0, size = box ? box->right - box->left : std::min(s->desc.ByteWidth, d->desc.ByteWidth - dx);
            CopyBuffer(d, dx, s, from, size);
            return;
        }
        if (IsBuffer(dst) || IsBuffer(src)) return;
        Texture* d = static_cast<Texture*>(static_cast<ID3D11Texture2D*>(dst));
        Texture* s = static_cast<Texture*>(static_cast<ID3D11Texture2D*>(src));
        const UINT sMip = srcSub % std::max(1u, s->desc.MipLevels), sLayer = srcSub / std::max(1u, s->desc.MipLevels);
        const UINT dMip = dstSub % std::max(1u, d->desc.MipLevels), dLayer = dstSub / std::max(1u, d->desc.MipLevels);
        UINT x = 0, y = 0, w = s->W(sMip), h = s->H(sMip);
        if (box) { x = box->left; y = box->top; w = box->right - box->left; h = box->bottom - box->top; }
        if (!w || !h) return;
        VkCommandBuffer c = Transfer();
        if (s->Staging()) return;                // staging -> GPU: not used by the renderer
        if (d->Staging())
        {
            VkBufferImageCopy r{};
            r.bufferOffset = d->soffset[dstSub] + VkDeviceSize(dy / d->fmt.block) * d->RowPitch(dMip) + VkDeviceSize(dx / d->fmt.block) * d->fmt.bytes;
            r.bufferRowLength = d->W(dMip);
            r.bufferImageHeight = d->H(dMip);
            r.imageSubresource = { s->fmt.aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : s->fmt.aspect, sMip, sLayer, 1 };
            r.imageOffset = { int32_t(x), int32_t(y), 0 };
            r.imageExtent = { w, h, 1 };
            vkCmdCopyImageToBuffer(c, s->image, VK_IMAGE_LAYOUT_GENERAL, d->sbuf, 1, &r);
            d->lastWrite = core.next.load();
            AfterTransfer();
            return;
        }
        VkImageCopy r{};
        r.srcSubresource = { FullAspect(s->fmt.vk), sMip, sLayer, 1 };
        r.dstSubresource = { FullAspect(d->fmt.vk), dMip, dLayer, 1 };
        r.srcOffset = { int32_t(x), int32_t(y), 0 };
        r.dstOffset = { int32_t(dx), int32_t(dy), 0 };
        r.extent = { std::min(w, d->W(dMip) - std::min(dx, d->W(dMip))), std::min(h, d->H(dMip) - std::min(dy, d->H(dMip))), 1 };
        if (r.extent.width && r.extent.height) vkCmdCopyImage(c, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        AfterTransfer();
    }
    void STDMETHODCALLTYPE CopyResource(ID3D11Resource* dst, ID3D11Resource* src) override
    {
        if (!dst || !src) return;
        if (IsBuffer(dst))
        {
            Buffer* d = static_cast<Buffer*>(static_cast<ID3D11Buffer*>(dst));
            Buffer* s = static_cast<Buffer*>(static_cast<ID3D11Buffer*>(src));
            CopyBuffer(d, 0, s, 0, std::min(d->desc.ByteWidth, s->desc.ByteWidth));
            return;
        }
        Texture* d = static_cast<Texture*>(static_cast<ID3D11Texture2D*>(dst));
        const UINT subs = std::max(1u, d->desc.MipLevels) * std::max(1u, d->desc.ArraySize);
        for (UINT i = 0; i < subs; ++i) CopySubresourceRegion(dst, i, 0, 0, 0, src, i, nullptr);
    }
    void CopyBuffer(Buffer* d, UINT dOff, Buffer* s, UINT sOff, UINT size)
    {
        if (!size || (!d->Handle() && !d->map)) return;
        if (s->desc.Usage == D3D11_USAGE_STAGING) return;
        VkCommandBuffer c = Transfer();
        VkBufferCopy r{ s->Base() + sOff, d->Base() + dOff, size };
        vkCmdCopyBuffer(c, s->Handle(), d->Handle(), 1, &r);
        if (d->desc.Usage == D3D11_USAGE_STAGING) d->lastWrite = core.next.load();
        AfterTransfer();
    }
    void STDMETHODCALLTYPE UpdateSubresource(ID3D11Resource* dst, UINT sub, const D3D11_BOX* box, const void* data, UINT rowPitch, UINT depthPitch) override
    {
        if (!dst || !data) return;
        if (IsBuffer(dst))
        {
            Buffer* d = static_cast<Buffer*>(static_cast<ID3D11Buffer*>(dst));
            const UINT off = box ? box->left : 0, size = box ? box->right - box->left : d->desc.ByteWidth;
            if (!size) return;
            if (d->Dynamic()) { std::memcpy(d->slice.ptr() + off, data, size); return; }     // not done by the renderer; best effort
            Slice s = device->heap.Alloc(size, 16);
            if (!s.chunk) return;
            std::memcpy(s.ptr(), data, size);
            VkCommandBuffer c = Transfer();
            VkBufferCopy r{ s.off, off, size };
            vkCmdCopyBuffer(c, s.buf(), d->buf, 1, &r);
            device->heap.Retire(s);
            AfterTransfer();
            return;
        }
        Texture* t = static_cast<Texture*>(static_cast<ID3D11Texture2D*>(dst));
        if (t->Staging()) return;
        const UINT mip = sub % std::max(1u, t->desc.MipLevels), layer = sub / std::max(1u, t->desc.MipLevels);
        UINT x = 0, y = 0, w = t->W(mip), h = t->H(mip);
        if (box) { x = box->left; y = box->top; w = box->right - box->left; h = box->bottom - box->top; }
        if (!w || !h) return;
        const uint32_t bw = t->fmt.block;
        const uint32_t rows = (h + bw - 1) / bw, rowBytes = (w + bw - 1) / bw * t->fmt.bytes;
        Slice s = device->heap.Alloc(VkDeviceSize(rows) * rowBytes, 16);
        if (!s.chunk) return;
        for (uint32_t r = 0; r < rows; ++r) std::memcpy(s.ptr() + size_t(r) * rowBytes, static_cast<const uint8_t*>(data) + size_t(r) * rowPitch, rowBytes);
        VkCommandBuffer c = Transfer();
        VkBufferImageCopy r{};
        r.bufferOffset = s.off;
        r.bufferRowLength = (w + bw - 1) / bw * bw;
        r.bufferImageHeight = rows * bw;
        r.imageSubresource = { t->fmt.aspect, mip, layer, 1 };
        r.imageOffset = { int32_t(x), int32_t(y), 0 };
        r.imageExtent = { std::min(w, t->W(mip) - x), std::min(h, t->H(mip) - y), 1 };
        vkCmdCopyBufferToImage(c, s.buf(), t->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        device->heap.Retire(s);
        AfterTransfer();
        (void)depthPitch;
    }
    void STDMETHODCALLTYPE UpdateSubresource1(ID3D11Resource* dst, UINT sub, const D3D11_BOX* box, const void* data, UINT rowPitch, UINT depthPitch, UINT) override
    {
        UpdateSubresource(dst, sub, box, data, rowPitch, depthPitch);
    }
    void STDMETHODCALLTYPE GenerateMips(ID3D11ShaderResourceView* v) override
    {
        Srv* sv = static_cast<Srv*>(v);
        if (!sv || !sv->tex) return;
        Texture* t = sv->tex;
        const uint32_t levels = std::max(1u, t->desc.MipLevels);
        if (levels < 2) return;
        VkCommandBuffer c = Transfer();
        for (uint32_t l = 1; l < levels; ++l)
        {
            VkImageBlit b{};
            b.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, l - 1, 0, std::max(1u, t->desc.ArraySize) };
            b.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, l, 0, std::max(1u, t->desc.ArraySize) };
            b.srcOffsets[1] = { int32_t(t->W(l - 1)), int32_t(t->H(l - 1)), 1 };
            b.dstOffsets[1] = { int32_t(t->W(l)), int32_t(t->H(l)), 1 };
            vkCmdBlitImage(c, t->image, VK_IMAGE_LAYOUT_GENERAL, t->image, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_LINEAR);
            Barrier();
        }
    }

    // ---------------------------------------------------------------- map
    HRESULT STDMETHODCALLTYPE Map(ID3D11Resource* res, UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* out) override
    {
        if (!res || !out) return E_INVALIDARG;
        if (IsBuffer(res))
        {
            Buffer* b = static_cast<Buffer*>(static_cast<ID3D11Buffer*>(res));
            if (b->Dynamic())
            {
                if (type == D3D11_MAP_WRITE_DISCARD || !b->slice.chunk)
                {
                    device->heap.Retire(b->slice);
                    b->slice = device->heap.Alloc(b->desc.ByteWidth, 256);
                    ++b->version;
                    // Bindings that use the buffer read the new slice from the next draw.
                    vsStage.dirty = psStage.dirty = true;
                }
                if (!b->slice.chunk) return E_OUTOFMEMORY;
                out->pData = b->slice.ptr();
                out->RowPitch = out->DepthPitch = b->desc.ByteWidth;
                return S_OK;
            }
            if (b->desc.Usage == D3D11_USAGE_STAGING && b->map)
            {
                if (!Ready(b->lastWrite, flags)) return DXGI_ERROR_WAS_STILL_DRAWING;
                out->pData = b->map;
                out->RowPitch = out->DepthPitch = b->desc.ByteWidth;
                return S_OK;
            }
            return E_INVALIDARG;
        }
        Texture* t = static_cast<Texture*>(static_cast<ID3D11Texture2D*>(res));
        if (!t->Staging() || !t->smap || sub >= t->soffset.size()) return E_INVALIDARG;
        if (!Ready(t->lastWrite, flags)) return DXGI_ERROR_WAS_STILL_DRAWING;
        if (!t->scoherent)
        {
            VkMappedMemoryRange r{ VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
            r.memory = t->smem;
            r.size = VK_WHOLE_SIZE;
            vkInvalidateMappedMemoryRanges(core.dev, 1, &r);
        }
        const UINT mip = sub % std::max(1u, t->desc.MipLevels);
        out->pData = t->smap + t->soffset[sub];
        out->RowPitch = t->RowPitch(mip);
        out->DepthPitch = t->RowPitch(mip) * t->Rows(mip);
        return S_OK;
    }
    // A CPU read of something the GPU wrote at submission `id`: submitted now if still open; waits unless DO_NOT_WAIT.
    bool Ready(uint64_t id, UINT flags)
    {
        if (!id) return true;
        if (id >= core.next.load()) Submit();
        Poll();
        if (core.completed.load() >= id) return true;
        if (flags & D3D11_MAP_FLAG_DO_NOT_WAIT) return false;
        WaitFor(id);
        return true;
    }
    void STDMETHODCALLTYPE Unmap(ID3D11Resource*, UINT) override {}

    // ---------------------------------------------------------------- queries, flush
    void STDMETHODCALLTYPE Begin(ID3D11Asynchronous*) override {}
    void STDMETHODCALLTYPE End(ID3D11Asynchronous* a) override
    {
        Query* q = static_cast<Query*>(static_cast<ID3D11Query*>(a));
        q->at = core.next.load();
        q->ended = true;
    }
    HRESULT STDMETHODCALLTYPE GetData(ID3D11Asynchronous* a, void* data, UINT size, UINT flags) override
    {
        Query* q = static_cast<Query*>(static_cast<ID3D11Query*>(a));
        switch (q->desc.Query)
        {
        case D3D11_QUERY_EVENT:
        {
            if (!q->ended) return S_FALSE;
            if (q->at >= core.next.load() && !(flags & D3D11_ASYNC_GETDATA_DONOTFLUSH)) Submit();
            Poll();
            const bool done = core.completed.load() >= q->at;
            if (data && size >= sizeof(BOOL)) *static_cast<BOOL*>(data) = done ? TRUE : FALSE;
            return done ? S_OK : S_FALSE;
        }
        case D3D11_QUERY_TIMESTAMP_DISJOINT:
            if (data && size >= sizeof(D3D11_QUERY_DATA_TIMESTAMP_DISJOINT))
                *static_cast<D3D11_QUERY_DATA_TIMESTAMP_DISJOINT*>(data) = { 1000000000ull, TRUE };     // no GPU timing here
            return S_OK;
        default:
            if (data && size) std::memset(data, 0, size);
            return S_OK;
        }
    }
    void STDMETHODCALLTYPE Flush() override { Submit(); }
};

// ================================================================ device: init and creation
bool Device::Init()
{
    heap.core = &core;
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0) SDL_InitSubSystem(SDL_INIT_VIDEO);
    if (SDL_Vulkan_LoadLibrary(nullptr) != 0 && std::strstr(SDL_GetError(), "already") == nullptr)
        Log("SDL_Vulkan_LoadLibrary: %s (continuing with the linked loader)", SDL_GetError());
    // Instance: the surface extensions SDL needs for the game's window.
    unsigned extCount = 0;
    std::vector<const char*> exts;
    if (SDL_Vulkan_GetInstanceExtensions(nullptr, &extCount, nullptr) && extCount)
    {
        exts.resize(extCount);
        SDL_Vulkan_GetInstanceExtensions(nullptr, &extCount, exts.data());
    }
    else
    {
        exts.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
#ifdef __ANDROID__
        exts.push_back("VK_KHR_android_surface");
#endif
    }
    uint32_t apiVersion = VK_API_VERSION_1_0;
    {
        auto enumVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
        if (enumVersion) enumVersion(&apiVersion);
    }
    VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "Tekken Tag Tournament 2";
    app.pEngineName = "ttt2 native";
    app.apiVersion = apiVersion >= VK_API_VERSION_1_1 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0;
    VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = uint32_t(exts.size());
    ici.ppEnabledExtensionNames = exts.data();
    VkResult r = vkCreateInstance(&ici, nullptr, &core.inst);
    if (r != VK_SUCCESS) { Log("vkCreateInstance failed (%d)", int(r)); return false; }
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(core.inst, &n, nullptr);
    if (!n) { Log("no Vulkan device"); return false; }
    std::vector<VkPhysicalDevice> phys(n);
    vkEnumeratePhysicalDevices(core.inst, &n, phys.data());
    core.phys = phys[0];
    for (VkPhysicalDevice p : phys)
    {
        VkPhysicalDeviceProperties pr;
        vkGetPhysicalDeviceProperties(p, &pr);
        if (pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU || pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { core.phys = p; break; }
    }
    vkGetPhysicalDeviceProperties(core.phys, &core.props);
    vkGetPhysicalDeviceMemoryProperties(core.phys, &core.memProps);
    gpuName = core.props.deviceName;
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(core.phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(core.phys, &qn, qf.data());
    core.family = ~0u;
    for (uint32_t i = 0; i < qn; ++i) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { core.family = i; break; }
    if (core.family == ~0u) { Log("no graphics queue"); return false; }
    // Device: swap chain; maintenance1 (negative viewport height) is core from 1.1.
    std::vector<const char*> dexts{ VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    const bool v11 = core.props.apiVersion >= VK_API_VERSION_1_1 && app.apiVersion >= VK_API_VERSION_1_1;
    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(core.phys, nullptr, &en, nullptr);
    std::vector<VkExtensionProperties> avail(en);
    vkEnumerateDeviceExtensionProperties(core.phys, nullptr, &en, avail.data());
    auto has = [&](const char* e) { for (auto& a : avail) if (std::strcmp(a.extensionName, e) == 0) return true; return false; };
    if (!has(VK_KHR_SWAPCHAIN_EXTENSION_NAME)) { Log("no VK_KHR_swapchain"); return false; }
    if (!v11)
    {
        if (!has("VK_KHR_maintenance1")) { Log("Vulkan 1.0 without VK_KHR_maintenance1"); return false; }
        dexts.push_back("VK_KHR_maintenance1");
    }
    core.maintenance1 = true;
    VkPhysicalDeviceFeatures sup{};
    vkGetPhysicalDeviceFeatures(core.phys, &sup);
    core.enabled.samplerAnisotropy = sup.samplerAnisotropy;
    core.enabled.textureCompressionBC = sup.textureCompressionBC;
    core.enabled.depthClamp = sup.depthClamp;
    core.enabled.depthBiasClamp = sup.depthBiasClamp;
    core.enabled.independentBlend = sup.independentBlend;
    core.enabled.fillModeNonSolid = sup.fillModeNonSolid;
    core.enabled.dualSrcBlend = sup.dualSrcBlend;
    core.enabled.fragmentStoresAndAtomics = sup.fragmentStoresAndAtomics;
    core.enabled.shaderImageGatherExtended = sup.shaderImageGatherExtended;
    core.enabled.shaderStorageImageExtendedFormats = sup.shaderStorageImageExtendedFormats;
    core.enabled.imageCubeArray = sup.imageCubeArray;
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = core.family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(dexts.size());
    dci.ppEnabledExtensionNames = dexts.data();
    dci.pEnabledFeatures = &core.enabled;
    r = vkCreateDevice(core.phys, &dci, nullptr, &core.dev);
    if (r != VK_SUCCESS) { Log("vkCreateDevice failed (%d)", int(r)); return false; }
    vkGetDeviceQueue(core.dev, core.family, 0, &core.queue);
    VkPipelineCacheCreateInfo pci{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    vkCreatePipelineCache(core.dev, &pci, nullptr, &core.pipelineCache);
    VkDescriptorSetLayoutCreateInfo empty{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    vkCreateDescriptorSetLayout(core.dev, &empty, nullptr, &emptySetLayout);
    VkCommandPoolCreateInfo cp{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = core.family;
    vkCreateCommandPool(core.dev, &cp, nullptr, &oneOffPool);
    Log("device: %s, Vulkan %u.%u.%u, driver %08X%s%s", gpuName.c_str(), VK_VERSION_MAJOR(core.props.apiVersion), VK_VERSION_MINOR(core.props.apiVersion),
        VK_VERSION_PATCH(core.props.apiVersion), core.props.driverVersion, sup.textureCompressionBC ? "" : " (no BC textures!)",
        sup.depthClamp ? "" : " (no depth clamp)");
    return true;
}

Device::~Device()
{
    if (context) { context->Submit(); context->WaitFor(core.next.load() - 1); }
    if (core.dev) vkDeviceWaitIdle(core.dev);
    core.completed = ~0ull >> 1;
    core.RunDeferred();
    // Process exit takes the rest (the renderer's device lives as long as the game).
}

void* Device::Find(REFIID riid)
{
    if (Same(riid, __uuidof(ID3D11Device)) || Same(riid, __uuidof(ID3D11Device1))) return static_cast<ID3D11Device1*>(this);
    return nullptr;     // IDXGIDevice: Device::QueryInterface below
}

void Device::OneOff(const std::function<void(VkCommandBuffer)>& record)
{
    std::lock_guard l(oneOffMutex);
    VkCommandBufferAllocateInfo a{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    a.commandPool = oneOffPool;
    a.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    a.commandBufferCount = 1;
    VkCommandBuffer c;
    vkAllocateCommandBuffers(core.dev, &a, &c);
    VkCommandBufferBeginInfo b{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    b.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(c, &b);
    record(c);
    vkEndCommandBuffer(c);
    VkFenceCreateInfo fi{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence f;
    vkCreateFence(core.dev, &fi, nullptr, &f);
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &c;
    {
        std::lock_guard q(core.queueMutex);
        vkQueueSubmit(core.queue, 1, &si, f);
    }
    vkWaitForFences(core.dev, 1, &f, VK_TRUE, 10000000000ull);
    vkDestroyFence(core.dev, f, nullptr);
    vkFreeCommandBuffers(core.dev, oneOffPool, 1, &c);
}

HRESULT Device::CreateBuffer(const D3D11_BUFFER_DESC* desc, const D3D11_SUBRESOURCE_DATA* init, ID3D11Buffer** out)
{
    if (!desc || !desc->ByteWidth) return E_INVALIDARG;
    if (!out) return S_FALSE;
    auto* b = new Buffer(this);
    b->desc = *desc;
    if (desc->Usage == D3D11_USAGE_DYNAMIC)
    {
        b->slice = heap.Alloc(desc->ByteWidth, 256);
        if (!b->slice.chunk) { b->Release(); return E_OUTOFMEMORY; }
        if (init && init->pSysMem) std::memcpy(b->slice.ptr(), init->pSysMem, desc->ByteWidth);
        *out = b;
        return S_OK;
    }
    const bool staging = desc->Usage == D3D11_USAGE_STAGING;
    VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    ci.size = (desc->ByteWidth + 15) & ~15u;
    ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (!staging)
        ci.usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                    VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (vkCreateBuffer(core.dev, &ci, nullptr, &b->buf) != VK_SUCCESS) { b->Release(); return E_OUTOFMEMORY; }
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(core.dev, b->buf, &r);
    b->mem = staging ? core.Allocate(r, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                     : core.Allocate(r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (!b->mem) { b->Release(); return E_OUTOFMEMORY; }
    vkBindBufferMemory(core.dev, b->buf, b->mem, 0);
    if (staging)
    {
        void* p = nullptr;
        vkMapMemory(core.dev, b->mem, 0, VK_WHOLE_SIZE, 0, &p);
        b->map = static_cast<uint8_t*>(p);
        if (init && init->pSysMem) std::memcpy(b->map, init->pSysMem, desc->ByteWidth);
    }
    else if (init && init->pSysMem)
    {
        Slice s = heap.Alloc(desc->ByteWidth, 16);
        std::memcpy(s.ptr(), init->pSysMem, desc->ByteWidth);
        VkBuffer dst = b->buf;
        OneOff([&](VkCommandBuffer c) { VkBufferCopy cp{ s.off, 0, desc->ByteWidth }; vkCmdCopyBuffer(c, s.buf(), dst, 1, &cp); });
        heap.Retire(s);
    }
    else
    {
        VkBuffer dst = b->buf;
        const VkDeviceSize size = ci.size;
        OneOff([&](VkCommandBuffer c) { vkCmdFillBuffer(c, dst, 0, size, 0); });
    }
    *out = b;
    return S_OK;
}

HRESULT Device::CreateTexture2D(const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out)
{
    if (!desc || !desc->Width || !desc->Height) return E_INVALIDARG;
    const FormatInfo fi = Format(desc->Format);
    if (fi.vk == VK_FORMAT_UNDEFINED) { Log("CreateTexture2D: format %u not supported", unsigned(desc->Format)); return E_INVALIDARG; }
    if (!out) return S_FALSE;
    auto* t = new Texture(this);
    t->desc = *desc;
    t->fmt = fi;
    if (!t->desc.MipLevels)
    {
        uint32_t m = 1, s = std::max(desc->Width, desc->Height);
        while (s > 1) { s >>= 1; ++m; }
        t->desc.MipLevels = m;
    }
    if (!t->desc.ArraySize) t->desc.ArraySize = 1;
    const uint32_t subs = t->desc.MipLevels * t->desc.ArraySize;
    if (desc->Usage == D3D11_USAGE_STAGING)
    {
        VkDeviceSize total = 0;
        for (uint32_t a = 0; a < t->desc.ArraySize; ++a)
            for (uint32_t m = 0; m < t->desc.MipLevels; ++m)
            {
                t->soffset.push_back(total);
                total += (VkDeviceSize(t->RowPitch(m)) * t->Rows(m) + 255) & ~VkDeviceSize(255);
            }
        VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        ci.size = total;
        ci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (vkCreateBuffer(core.dev, &ci, nullptr, &t->sbuf) != VK_SUCCESS) { t->Release(); return E_OUTOFMEMORY; }
        VkMemoryRequirements r;
        vkGetBufferMemoryRequirements(core.dev, t->sbuf, &r);
        int32_t type = core.FindMemory(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (type >= 0)
        {
            t->scoherent = (core.memProps.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
            t->smem = core.Allocate(r, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        }
        if (!t->smem) { t->scoherent = true; t->smem = core.Allocate(r, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT); }
        if (!t->smem) { t->Release(); return E_OUTOFMEMORY; }
        vkBindBufferMemory(core.dev, t->sbuf, t->smem, 0);
        void* p = nullptr;
        vkMapMemory(core.dev, t->smem, 0, VK_WHOLE_SIZE, 0, &p);
        t->smap = static_cast<uint8_t*>(p);
        *out = t;
        return S_OK;
    }
    VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fi.vk;
    ci.extent = { desc->Width, desc->Height, 1 };
    ci.mipLevels = t->desc.MipLevels;
    ci.arrayLayers = t->desc.ArraySize;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (desc->BindFlags & D3D11_BIND_RENDER_TARGET) ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (desc->BindFlags & D3D11_BIND_DEPTH_STENCIL) ci.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (desc->MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE && t->desc.ArraySize >= 6) ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (desc->Format == DXGI_FORMAT_R8G8B8A8_TYPELESS || desc->Format == DXGI_FORMAT_B8G8R8A8_TYPELESS || desc->Format == DXGI_FORMAT_R32_TYPELESS)
        ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult r = vkCreateImage(core.dev, &ci, nullptr, &t->image);
    if (r != VK_SUCCESS) { Log("vkCreateImage %ux%u format %u failed (%d)", desc->Width, desc->Height, unsigned(fi.vk), int(r)); t->Release(); return E_OUTOFMEMORY; }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(core.dev, t->image, &req);
    t->mem = core.Allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (!t->mem) { t->Release(); return E_OUTOFMEMORY; }
    vkBindImageMemory(core.dev, t->image, t->mem, 0);
    if (init)
    {
        // Initial data: every subresource uploaded in one submission of its own, ending in GENERAL.
        std::vector<Slice> slices;
        std::vector<VkBufferImageCopy> copies;
        for (uint32_t a = 0; a < t->desc.ArraySize; ++a)
            for (uint32_t m = 0; m < t->desc.MipLevels; ++m)
            {
                const D3D11_SUBRESOURCE_DATA& d = init[a * t->desc.MipLevels + m];
                if (!d.pSysMem) continue;
                const uint32_t rows = t->Rows(m), rowBytes = t->RowPitch(m);
                Slice s = heap.Alloc(VkDeviceSize(rows) * rowBytes, 16);
                for (uint32_t y = 0; y < rows; ++y)
                    std::memcpy(s.ptr() + size_t(y) * rowBytes, static_cast<const uint8_t*>(d.pSysMem) + size_t(y) * d.SysMemPitch, rowBytes);
                VkBufferImageCopy c{};
                c.bufferOffset = s.off;
                c.imageSubresource = { fi.aspect, m, a, 1 };
                c.imageExtent = { t->W(m), t->H(m), 1 };
                copies.push_back(c);
                slices.push_back(s);
            }
        VkImage img = t->image;
        const VkImageAspectFlags aspect = FullAspect(fi.vk);
        OneOff([&](VkCommandBuffer c) {
            VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = img;
            b.subresourceRange = { aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };
            vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
            for (size_t i = 0; i < copies.size(); ++i) vkCmdCopyBufferToImage(c, slices[i].buf(), img, VK_IMAGE_LAYOUT_GENERAL, 1, &copies[i]);
            VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
            vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
        });
        for (auto& s : slices) heap.Retire(s);
    }
    else NewImage(t->image, FullAspect(fi.vk));
    (void)subs;
    *out = t;
    return S_OK;
}

static ID3D11Texture2D* AsTexture(ID3D11Resource* r)
{
    D3D11_RESOURCE_DIMENSION d;
    r->GetType(&d);
    return d == D3D11_RESOURCE_DIMENSION_TEXTURE2D ? static_cast<ID3D11Texture2D*>(r) : nullptr;
}

HRESULT Device::CreateShaderResourceView(ID3D11Resource* res, const D3D11_SHADER_RESOURCE_VIEW_DESC* desc, ID3D11ShaderResourceView** out)
{
    if (!res) return E_INVALIDARG;
    auto* v = new Srv(this);
    v->resource = res;
    if (ID3D11Texture2D* t2 = AsTexture(res))
    {
        Texture* t = static_cast<Texture*>(t2);
        v->tex = t;
        D3D11_SHADER_RESOURCE_VIEW_DESC d{};
        if (desc) d = *desc;
        else
        {
            d.Format = t->desc.Format;
            if (t->desc.ArraySize > 1) { d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY; d.Texture2DArray.MipLevels = t->desc.MipLevels; d.Texture2DArray.ArraySize = t->desc.ArraySize; }
            else { d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; d.Texture2D.MipLevels = t->desc.MipLevels; }
        }
        v->desc = d;
        FormatInfo f = Format(d.Format);
        if (f.vk == VK_FORMAT_UNDEFINED) f = t->fmt;
        VkImageViewCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        ci.image = t->image;
        ci.format = IsDepth(t->fmt.vk) ? t->fmt.vk : f.vk;
        VkImageAspectFlags aspect = IsDepth(t->fmt.vk) ? (f.aspect == VK_IMAGE_ASPECT_STENCIL_BIT ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT) : VK_IMAGE_ASPECT_COLOR_BIT;
        uint32_t mip = 0, mips = t->desc.MipLevels, layer = 0, layers = 1;
        if (d.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY)
        {
            ci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            mip = d.Texture2DArray.MostDetailedMip; mips = d.Texture2DArray.MipLevels;
            layer = d.Texture2DArray.FirstArraySlice; layers = d.Texture2DArray.ArraySize;
        }
        else if (d.ViewDimension == D3D11_SRV_DIMENSION_TEXTURECUBE)
        {
            ci.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
            mip = d.TextureCube.MostDetailedMip; mips = d.TextureCube.MipLevels; layers = 6;
        }
        else
        {
            ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            mip = d.Texture2D.MostDetailedMip; mips = d.Texture2D.MipLevels;
        }
        if (mips == ~0u || mips == 0 || mip + mips > t->desc.MipLevels) mips = t->desc.MipLevels - std::min(mip, t->desc.MipLevels - 1);
        if (layers == ~0u || layer + layers > t->desc.ArraySize) layers = t->desc.ArraySize - std::min(layer, t->desc.ArraySize - 1);
        ci.subresourceRange = { aspect, mip, mips, layer, layers };
        // DXGI's B8G8R8X8 / A8 as Vulkan formats: alpha forced to one / colour from red.
        if (d.Format == DXGI_FORMAT_B8G8R8X8_UNORM) ci.components.a = VK_COMPONENT_SWIZZLE_ONE;
        if (d.Format == DXGI_FORMAT_A8_UNORM) ci.components = { VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R };
        if (vkCreateImageView(core.dev, &ci, nullptr, &v->view) != VK_SUCCESS) { v->Release(); return E_INVALIDARG; }
    }
    else
    {
        Buffer* b = static_cast<Buffer*>(static_cast<ID3D11Buffer*>(res));
        v->buffer = b;
        if (!desc) { v->Release(); return E_INVALIDARG; }
        v->desc = *desc;
        UINT first = 0, num = 0;
        bool raw = false;
        if (desc->ViewDimension == D3D11_SRV_DIMENSION_BUFFEREX) { first = desc->BufferEx.FirstElement; num = desc->BufferEx.NumElements; raw = (desc->BufferEx.Flags & D3D11_BUFFEREX_SRV_FLAG_RAW) != 0; }
        else { first = desc->Buffer.FirstElement; num = desc->Buffer.NumElements; }
        const FormatInfo f = raw ? Format(DXGI_FORMAT_R32_UINT) : Format(desc->Format);
        v->bfmt = f.vk == VK_FORMAT_UNDEFINED ? VK_FORMAT_R32_UINT : f.vk;
        const uint32_t stride = f.bytes ? f.bytes : 4;
        v->bfirst = VkDeviceSize(first) * stride;
        v->brange = VkDeviceSize(num) * stride;
    }
    if (!out) { v->Release(); return S_FALSE; }
    *out = v;
    return S_OK;
}

HRESULT Device::CreateRenderTargetView(ID3D11Resource* res, const D3D11_RENDER_TARGET_VIEW_DESC* desc, ID3D11RenderTargetView** out)
{
    ID3D11Texture2D* t2 = res ? AsTexture(res) : nullptr;
    if (!t2) return E_INVALIDARG;
    Texture* t = static_cast<Texture*>(t2);
    auto* v = new Rtv(this);
    v->resource = res;
    v->tex = t;
    D3D11_RENDER_TARGET_VIEW_DESC d{};
    if (desc) d = *desc;
    else { d.Format = t->desc.Format; d.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D; }
    v->desc = d;
    FormatInfo f = Format(d.Format);
    if (f.vk == VK_FORMAT_UNDEFINED) f = t->fmt;
    v->format = f.vk;
    if (d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY) { v->mip = d.Texture2DArray.MipSlice; v->layer = d.Texture2DArray.FirstArraySlice; }
    else v->mip = d.Texture2D.MipSlice;
    VkImageViewCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ci.image = t->image;
    ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ci.format = f.vk;
    ci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, v->mip, 1, v->layer, 1 };
    if (vkCreateImageView(core.dev, &ci, nullptr, &v->view) != VK_SUCCESS) { v->Release(); return E_INVALIDARG; }
    if (!out) { v->Release(); return S_FALSE; }
    *out = v;
    return S_OK;
}

HRESULT Device::CreateDepthStencilView(ID3D11Resource* res, const D3D11_DEPTH_STENCIL_VIEW_DESC* desc, ID3D11DepthStencilView** out)
{
    ID3D11Texture2D* t2 = res ? AsTexture(res) : nullptr;
    if (!t2) return E_INVALIDARG;
    Texture* t = static_cast<Texture*>(t2);
    auto* v = new Dsv(this);
    v->resource = res;
    v->tex = t;
    D3D11_DEPTH_STENCIL_VIEW_DESC d{};
    if (desc) d = *desc;
    else { d.Format = t->desc.Format; d.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D; }
    v->desc = d;
    v->format = t->fmt.vk;
    if (d.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DARRAY) { v->mip = d.Texture2DArray.MipSlice; v->layer = d.Texture2DArray.FirstArraySlice; }
    else v->mip = d.Texture2D.MipSlice;
    VkImageViewCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ci.image = t->image;
    ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ci.format = t->fmt.vk;
    ci.subresourceRange = { FullAspect(t->fmt.vk), v->mip, 1, v->layer, 1 };
    if (vkCreateImageView(core.dev, &ci, nullptr, &v->view) != VK_SUCCESS) { v->Release(); return E_INVALIDARG; }
    if (!out) { v->Release(); return S_FALSE; }
    *out = v;
    return S_OK;
}

HRESULT Device::CreateSamplerState(const D3D11_SAMPLER_DESC* desc, ID3D11SamplerState** out)
{
    if (!desc) return E_INVALIDARG;
    if (!out) return S_FALSE;
    auto* s = new Sampler(this);
    s->desc = *desc;
    auto addr = [](D3D11_TEXTURE_ADDRESS_MODE m) {
        switch (m)
        {
        case D3D11_TEXTURE_ADDRESS_MIRROR: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case D3D11_TEXTURE_ADDRESS_CLAMP: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case D3D11_TEXTURE_ADDRESS_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        case D3D11_TEXTURE_ADDRESS_MIRROR_ONCE: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
        default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        }
    };
    const unsigned f = unsigned(desc->Filter);
    VkSamplerCreateInfo ci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    const bool aniso = (f & 0x40) != 0;           // D3D11_FILTER_ANISOTROPIC bits
    ci.magFilter = (aniso || (f & 0x04)) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.minFilter = (aniso || (f & 0x10)) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = (aniso || (f & 0x01)) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = addr(desc->AddressU);
    ci.addressModeV = addr(desc->AddressV);
    ci.addressModeW = addr(desc->AddressW);
    if (ci.addressModeU == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE) ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    if (ci.addressModeV == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE) ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    if (ci.addressModeW == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE) ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    ci.mipLodBias = desc->MipLODBias;
    ci.anisotropyEnable = (aniso && core.enabled.samplerAnisotropy) ? VK_TRUE : VK_FALSE;
    ci.maxAnisotropy = std::min<float>(float(std::max(1u, desc->MaxAnisotropy)), core.props.limits.maxSamplerAnisotropy);
    ci.compareEnable = (f & 0x80) ? VK_TRUE : VK_FALSE;
    ci.compareOp = Compare(desc->ComparisonFunc);
    ci.minLod = desc->MinLOD;
    ci.maxLod = desc->MaxLOD >= 1000.0f ? VK_LOD_CLAMP_NONE : desc->MaxLOD;
    const float* bc = desc->BorderColor;
    ci.borderColor = bc[3] < 0.5f ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
                   : (bc[0] + bc[1] + bc[2]) > 1.5f ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    if (vkCreateSampler(core.dev, &ci, nullptr, &s->sampler) != VK_SUCCESS) { s->Release(); return E_INVALIDARG; }
    *out = s;
    return S_OK;
}

// ---------------------------------------------------------------- shaders through vkd3d-shader
bool Device::CompileShader(const void* code, SIZE_T size, bool vertex, ShaderCode& out)
{
    vkd3d_shader_compile_info info{};
    info.type = VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;
    info.source.code = code;
    info.source.size = size;
    info.source_type = VKD3D_SHADER_SOURCE_DXBC_TPF;
    info.target_type = VKD3D_SHADER_TARGET_SPIRV_BINARY;
    info.log_level = VKD3D_SHADER_LOG_WARNING;
    // 1) Which registers the shader uses.
    vkd3d_shader_scan_descriptor_info scan{};
    scan.type = VKD3D_SHADER_STRUCTURE_TYPE_SCAN_DESCRIPTOR_INFO;
    info.next = &scan;
    char* messages = nullptr;
    int rc = vkd3d_shader_scan(&info, &messages);
    if (messages) { if (rc < 0) Log("shader scan: %s", messages); vkd3d_shader_free_messages(messages); messages = nullptr; }
    if (rc < 0) return false;
    const unsigned set = vertex ? 0 : 1;
    const vkd3d_shader_visibility vis = vertex ? VKD3D_SHADER_VISIBILITY_VERTEX : VKD3D_SHADER_VISIBILITY_PIXEL;
    std::vector<vkd3d_shader_resource_binding> binds;
    std::vector<VkDescriptorSetLayoutBinding> layoutBinds;
    for (unsigned i = 0; i < scan.descriptor_count; ++i)
    {
        const vkd3d_shader_descriptor_info& d = scan.descriptors[i];
        Binding b{};
        b.reg = d.register_index;
        b.binding = uint32_t(binds.size());
        vkd3d_shader_resource_binding rb{};
        rb.type = d.type;
        rb.register_space = d.register_space;
        rb.register_index = d.register_index;
        rb.shader_visibility = vis;
        rb.binding.set = set;
        rb.binding.binding = b.binding;
        rb.binding.count = 1;
        switch (d.type)
        {
        case VKD3D_SHADER_DESCRIPTOR_TYPE_CBV:
            b.kind = 0; b.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; rb.flags = VKD3D_SHADER_BINDING_FLAG_BUFFER; break;
        case VKD3D_SHADER_DESCRIPTOR_TYPE_SRV:
            b.kind = 1;
            if (d.resource_type == VKD3D_SHADER_RESOURCE_BUFFER) { b.type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER; rb.flags = VKD3D_SHADER_BINDING_FLAG_BUFFER; }
            else
            {
                b.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; rb.flags = VKD3D_SHADER_BINDING_FLAG_IMAGE;
            }
            break;
        case VKD3D_SHADER_DESCRIPTOR_TYPE_SAMPLER:
            b.kind = 2; b.type = VK_DESCRIPTOR_TYPE_SAMPLER; rb.flags = 0; break;
        default:
            Log("shader uses a UAV - not supported by the native renderer");
            vkd3d_shader_free_scan_descriptor_info(&scan);
            return false;
        }
        binds.push_back(rb);
        VkDescriptorSetLayoutBinding lb{};
        lb.binding = b.binding;
        lb.descriptorType = b.type;
        lb.descriptorCount = 1;
        lb.stageFlags = vertex ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
        layoutBinds.push_back(lb);
        // Unbound array textures get the array dummy: remembered in the high bit of a copy of the binding.
        if (b.kind == 1 && d.resource_type == VKD3D_SHADER_RESOURCE_TEXTURE_2DARRAY) b.binding |= 0x8000;
        out.bindings.push_back(b);
    }
    vkd3d_shader_free_scan_descriptor_info(&scan);
    // 2) SPIR-V with that binding map.
    vkd3d_shader_spirv_target_info spirv{};
    spirv.type = VKD3D_SHADER_STRUCTURE_TYPE_SPIRV_TARGET_INFO;
    spirv.environment = VKD3D_SHADER_SPIRV_ENVIRONMENT_VULKAN_1_0;
    vkd3d_shader_interface_info iface{};
    iface.type = VKD3D_SHADER_STRUCTURE_TYPE_INTERFACE_INFO;
    iface.next = &spirv;
    iface.bindings = binds.data();
    iface.binding_count = unsigned(binds.size());
    info.next = &iface;
    vkd3d_shader_code spv{};
    rc = vkd3d_shader_compile(&info, &spv, &messages);
    if (messages) { if (rc < 0) Log("shader compile: %s", messages); vkd3d_shader_free_messages(messages); }
    if (rc < 0) return false;
    VkShaderModuleCreateInfo mi{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    mi.codeSize = spv.size;
    mi.pCode = static_cast<const uint32_t*>(spv.code);
    const VkResult vr = vkCreateShaderModule(core.dev, &mi, nullptr, &out.module);
    vkd3d_shader_free_shader_code(&spv);
    if (vr != VK_SUCCESS) return false;
    VkDescriptorSetLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    lci.bindingCount = uint32_t(layoutBinds.size());
    lci.pBindings = layoutBinds.data();
    vkCreateDescriptorSetLayout(core.dev, &lci, nullptr, &out.layout);
    // 3) Vertex inputs: semantic -> register (the SPIR-V location).
    if (vertex)
    {
        vkd3d_shader_code dxbc{ code, size };
        vkd3d_shader_signature sig{};
        if (vkd3d_shader_parse_input_signature(&dxbc, &sig, nullptr) >= 0)
        {
            for (unsigned i = 0; i < sig.element_count; ++i)
            {
                std::string name = sig.elements[i].semantic_name;
                for (char& ch : name) ch = char(std::toupper(static_cast<unsigned char>(ch)));
                out.inputs.emplace_back(name + std::to_string(sig.elements[i].semantic_index), sig.elements[i].register_index);
            }
            vkd3d_shader_free_shader_signature(&sig);
        }
    }
    out.id = ids++;
    return true;
}

HRESULT Device::CreateVertexShader(const void* code, SIZE_T size, ID3D11ClassLinkage*, ID3D11VertexShader** out)
{
    auto* s = new VertexShader(this);
    if (!CompileShader(code, size, true, s->code)) { s->Release(); return E_INVALIDARG; }
    if (!out) { s->Release(); return S_FALSE; }
    *out = s;
    return S_OK;
}
HRESULT Device::CreatePixelShader(const void* code, SIZE_T size, ID3D11ClassLinkage*, ID3D11PixelShader** out)
{
    auto* s = new PixelShader(this);
    if (!CompileShader(code, size, false, s->code)) { s->Release(); return E_INVALIDARG; }
    if (!out) { s->Release(); return S_FALSE; }
    *out = s;
    return S_OK;
}

HRESULT Device::CreateInputLayout(const D3D11_INPUT_ELEMENT_DESC* elems, UINT count, const void* code, SIZE_T size, ID3D11InputLayout** out)
{
    ShaderCode sc;
    std::vector<std::pair<std::string, uint32_t>> inputs;
    {
        vkd3d_shader_code dxbc{ code, size };
        vkd3d_shader_signature sig{};
        if (vkd3d_shader_parse_input_signature(&dxbc, &sig, nullptr) < 0) return E_INVALIDARG;
        for (unsigned i = 0; i < sig.element_count; ++i)
        {
            std::string name = sig.elements[i].semantic_name;
            for (char& ch : name) ch = char(std::toupper(static_cast<unsigned char>(ch)));
            inputs.emplace_back(name + std::to_string(sig.elements[i].semantic_index), sig.elements[i].register_index);
        }
        vkd3d_shader_free_shader_signature(&sig);
    }
    auto* l = new InputLayout(this);
    UINT offsets[16] = {};
    for (UINT i = 0; i < count; ++i)
    {
        const D3D11_INPUT_ELEMENT_DESC& e = elems[i];
        std::string name = e.SemanticName;
        for (char& ch : name) ch = char(std::toupper(static_cast<unsigned char>(ch)));
        name += std::to_string(e.SemanticIndex);
        const FormatInfo f = Format(e.Format);
        const UINT slot = std::min<UINT>(e.InputSlot, 15);
        const UINT off = e.AlignedByteOffset == D3D11_APPEND_ALIGNED_ELEMENT ? offsets[slot] : e.AlignedByteOffset;
        offsets[slot] = off + f.bytes;
        for (auto& in : inputs)
            if (in.first == name) { l->attrs.push_back({ in.second, slot, off, f.vk, e.InputSlotClass == D3D11_INPUT_PER_INSTANCE_DATA }); break; }
    }
    l->id = ids++;
    if (!out) { l->Release(); return S_FALSE; }
    *out = l;
    return S_OK;
}

void Device::GetImmediateContext(ID3D11DeviceContext** out) { *out = context; context->AddRef(); }
void Device::GetImmediateContext1(ID3D11DeviceContext1** out) { *out = context; context->AddRef(); }

// ---------------------------------------------------------------- context init (dummies)
bool Context::Init()
{
    VkCommandPoolCreateInfo cp{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = core.family;
    if (vkCreateCommandPool(core.dev, &cp, nullptr, &pool) != VK_SUCCESS) return false;
    // A 1x1x2 black RGBA image (2D and 2D-array views), a 64 KB zero buffer, a texel view of it, a default sampler.
    VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = { 1, 1, 1 };
    ci.mipLevels = 1;
    ci.arrayLayers = 2;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    vkCreateImage(core.dev, &ci, nullptr, &dummyImage);
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(core.dev, dummyImage, &r);
    dummyMem = core.Allocate(r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    vkBindImageMemory(core.dev, dummyImage, dummyMem, 0);
    VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = dummyImage;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCreateImageView(core.dev, &vi, nullptr, &dummyView);
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.subresourceRange.layerCount = 2;
    vkCreateImageView(core.dev, &vi, nullptr, &dummyArrayView);
    VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = 65536;
    bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    vkCreateBuffer(core.dev, &bi, nullptr, &dummyBuffer);
    vkGetBufferMemoryRequirements(core.dev, dummyBuffer, &r);
    dummyBufMem = core.Allocate(r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    vkBindBufferMemory(core.dev, dummyBuffer, dummyBufMem, 0);
    VkBufferViewCreateInfo bv{ VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO };
    bv.buffer = dummyBuffer;
    bv.format = VK_FORMAT_R32_UINT;
    bv.range = 65536;
    vkCreateBufferView(core.dev, &bv, nullptr, &dummyTexel);
    VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = VK_LOD_CLAMP_NONE;
    vkCreateSampler(core.dev, &si, nullptr, &dummySampler);
    VkImage img = dummyImage;
    VkBuffer buf = dummyBuffer;
    device->OneOff([&](VkCommandBuffer c) {
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2 };
        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkClearColorValue zero{};
        VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2 };
        vkCmdClearColorImage(c, img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        vkCmdFillBuffer(c, buf, 0, VK_WHOLE_SIZE, 0);
        VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    });
    return dummyView && dummyArrayView && dummyTexel && dummySampler;
}

// ================================================================ DXGI
struct Factory : Com<Stub_IDXGIFactory2<IDXGIFactory2>>
{
    Device* device;
    explicit Factory(Device* d) : device(d) { d->AddRef(); }
    ~Factory() override { device->Release(); }
    void* Find(REFIID riid) override
    {
        if (Same(riid, __uuidof(IDXGIFactory)) || Same(riid, __uuidof(IDXGIFactory1)) || Same(riid, __uuidof(IDXGIFactory2)) || Same(riid, __uuidof(IDXGIObject)))
            return static_cast<IDXGIFactory2*>(this);
        return nullptr;
    }
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND, UINT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown* dev, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1** out) override;
    BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override { return FALSE; }
    BOOL STDMETHODCALLTYPE IsCurrent() override { return TRUE; }
};

struct Adapter : Com<Stub_IDXGIAdapter1<IDXGIAdapter1>>
{
    Device* device;
    explicit Adapter(Device* d) : device(d) { d->AddRef(); }
    ~Adapter() override { device->Release(); }
    void* Find(REFIID riid) override
    {
        if (Same(riid, __uuidof(IDXGIAdapter)) || Same(riid, __uuidof(IDXGIAdapter1)) || Same(riid, __uuidof(IDXGIObject))) return static_cast<IDXGIAdapter1*>(this);
        return nullptr;
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** out) override
    {
        auto* f = new Factory(device);
        const HRESULT hr = f->QueryInterface(riid, out);
        f->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_ADAPTER_DESC* d) override
    {
        std::memset(d, 0, sizeof(*d));
        const std::string name = "Native Vulkan: " + device->gpuName;
        for (size_t i = 0; i < name.size() && i < 127; ++i) d->Description[i] = WCHAR(static_cast<unsigned char>(name[i]));
        d->VendorId = device->core.props.vendorID;
        d->DeviceId = device->core.props.deviceID;
        VkDeviceSize local = 0;
        for (uint32_t i = 0; i < device->core.memProps.memoryHeapCount; ++i)
            if (device->core.memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) local += device->core.memProps.memoryHeaps[i].size;
        d->DedicatedVideoMemory = SIZE_T(local);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_ADAPTER_DESC1* d) override
    {
        DXGI_ADAPTER_DESC a;
        GetDesc(&a);
        std::memset(d, 0, sizeof(*d));
        std::memcpy(d->Description, a.Description, sizeof(a.Description));
        d->VendorId = a.VendorId;
        d->DeviceId = a.DeviceId;
        d->DedicatedVideoMemory = a.DedicatedVideoMemory;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumOutputs(UINT, IDXGIOutput**) override { return DXGI_ERROR_NOT_FOUND; }
};

struct DxgiDevice : Com<Stub_IDXGIDevice1<IDXGIDevice1>>
{
    Device* device;
    explicit DxgiDevice(Device* d) : device(d) { d->AddRef(); }
    ~DxgiDevice() override { device->Release(); }
    void* Find(REFIID riid) override
    {
        if (Same(riid, __uuidof(IDXGIDevice)) || Same(riid, __uuidof(IDXGIDevice1)) || Same(riid, __uuidof(IDXGIObject))) return static_cast<IDXGIDevice1*>(this);
        return nullptr;
    }
    HRESULT STDMETHODCALLTYPE GetAdapter(IDXGIAdapter** out) override { *out = new Adapter(device); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** out) override
    {
        auto* a = new Adapter(device);
        const HRESULT hr = a->QueryInterface(riid, out);
        a->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT n) override { latency = std::clamp<UINT>(n, 1, 3); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* n) override { *n = latency; return S_OK; }
    static inline UINT latency = 2;
};

struct SwapChain : Com<Stub_IDXGISwapChain1<IDXGISwapChain1>>
{
    Device* device;
    SDL_Window* window;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    Texture* back = nullptr;                 // the app's back buffer (refs counted on the swap chain)
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> images;
    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    UINT interval = 1;
    std::vector<VkSemaphore> acquireSems, doneSems;
    uint32_t semIndex = 0;
    std::deque<uint64_t> frames;              // submissions of the last presents (frame latency)
    bool warnedNoWindow = false;

    SwapChain(Device* d, SDL_Window* w, const DXGI_SWAP_CHAIN_DESC1& dsc) : device(d), window(w), desc(dsc) { d->AddRef(); }
    ~SwapChain() override
    {
        device->context->Submit();
        if (device->core.dev) vkQueueWaitIdle(device->core.queue);
        DestroySwapchain();
        if (surface) vkDestroySurfaceKHR(device->core.inst, surface, nullptr);
        if (back) { back->external = false; back->Release(); }
        for (VkSemaphore s : acquireSems) vkDestroySemaphore(device->core.dev, s, nullptr);
        for (VkSemaphore s : doneSems) vkDestroySemaphore(device->core.dev, s, nullptr);
        device->Release();
    }
    void* Find(REFIID riid) override
    {
        if (Same(riid, __uuidof(IDXGISwapChain)) || Same(riid, __uuidof(IDXGISwapChain1)) || Same(riid, __uuidof(IDXGIDeviceSubObject)) || Same(riid, __uuidof(IDXGIObject)))
            return static_cast<IDXGISwapChain1*>(this);
        return nullptr;
    }
    bool MakeBack()
    {
        D3D11_TEXTURE2D_DESC t{};
        t.Width = std::max(1u, desc.Width);
        t.Height = std::max(1u, desc.Height);
        t.MipLevels = t.ArraySize = 1;
        t.Format = desc.Format == DXGI_FORMAT_UNKNOWN ? DXGI_FORMAT_B8G8R8A8_UNORM : desc.Format;
        t.SampleDesc.Count = 1;
        t.Usage = D3D11_USAGE_DEFAULT;
        t.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ID3D11Texture2D* tex = nullptr;
        if (FAILED(device->CreateTexture2D(&t, nullptr, &tex))) return false;
        back = static_cast<Texture*>(tex);
        return true;
    }
    void DestroySwapchain()
    {
        if (swapchain) vkDestroySwapchainKHR(device->core.dev, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
        images.clear();
    }
    // Surface + swap chain for the window as it is now. False: no window (the app is in the background).
    bool Ensure()
    {
        Core& c = device->core;
        if (!surface)
        {
            if (!window || SDL_Vulkan_CreateSurface(window, c.inst, &surface) != SDL_TRUE)
            {
                surface = VK_NULL_HANDLE;
                if (!warnedNoWindow) { Log("no window surface for now (%s) - presents skipped", SDL_GetError()); warnedNoWindow = true; }
                return false;
            }
            warnedNoWindow = false;
            VkBool32 ok = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(c.phys, c.family, surface, &ok);
            if (!ok) { Log("the queue cannot present to this surface"); return false; }
        }
        if (swapchain) return true;
        VkSurfaceCapabilitiesKHR caps;
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(c.phys, surface, &caps) != VK_SUCCESS) { vkDestroySurfaceKHR(c.inst, surface, nullptr); surface = VK_NULL_HANDLE; return false; }
        uint32_t n = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(c.phys, surface, &n, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(n);
        vkGetPhysicalDeviceSurfaceFormatsKHR(c.phys, surface, &n, formats.data());
        VkSurfaceFormatKHR pick = formats.empty() ? VkSurfaceFormatKHR{ VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR } : formats[0];
        for (auto& f : formats) if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) { pick = f; break; }
        uint32_t m = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(c.phys, surface, &m, nullptr);
        std::vector<VkPresentModeKHR> modes(m);
        vkGetPhysicalDeviceSurfacePresentModesKHR(c.phys, surface, &m, modes.data());
        mode = VK_PRESENT_MODE_FIFO_KHR;
        if (interval == 0)
            for (VkPresentModeKHR pm : modes) if (pm == VK_PRESENT_MODE_MAILBOX_KHR || pm == VK_PRESENT_MODE_IMMEDIATE_KHR) { mode = pm; if (pm == VK_PRESENT_MODE_MAILBOX_KHR) break; }
        extent = caps.currentExtent;
        if (extent.width == 0xFFFFFFFFu) extent = { desc.Width, desc.Height };
        if (!extent.width || !extent.height) return false;
        VkSwapchainCreateInfoKHR ci{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
        ci.surface = surface;
        ci.minImageCount = std::max(caps.minImageCount, 3u);
        if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
        ci.imageFormat = pick.format;
        ci.imageColorSpace = pick.colorSpace;
        ci.imageExtent = extent;
        ci.imageArrayLayers = 1;
        ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
        ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
        ci.presentMode = mode;
        ci.clipped = VK_TRUE;
        const VkResult r = vkCreateSwapchainKHR(c.dev, &ci, nullptr, &swapchain);
        if (r != VK_SUCCESS)
        {
            Log("vkCreateSwapchainKHR failed (%d)", int(r));
            swapchain = VK_NULL_HANDLE;
            if (r == VK_ERROR_SURFACE_LOST_KHR || r == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR) { vkDestroySurfaceKHR(c.inst, surface, nullptr); surface = VK_NULL_HANDLE; }
            return false;
        }
        format = pick.format;
        uint32_t count = 0;
        vkGetSwapchainImagesKHR(c.dev, swapchain, &count, nullptr);
        images.resize(count);
        vkGetSwapchainImagesKHR(c.dev, swapchain, &count, images.data());
        while (acquireSems.size() < count + 1)
        {
            VkSemaphoreCreateInfo si{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            VkSemaphore a, d;
            vkCreateSemaphore(c.dev, &si, nullptr, &a);
            vkCreateSemaphore(c.dev, &si, nullptr, &d);
            acquireSems.push_back(a);
            doneSems.push_back(d);
        }
        Log("swap chain %ux%u, %u images, format %d, %s", extent.width, extent.height, count, int(format),
            mode == VK_PRESENT_MODE_FIFO_KHR ? "vsync" : mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "immediate");
        return true;
    }
    HRESULT STDMETHODCALLTYPE Present(UINT syncInterval, UINT flags) override
    {
        Context* ctx = device->context;
        Core& c = device->core;
        if (syncInterval != interval) { interval = syncInterval; if (swapchain) { ctx->Submit(); vkQueueWaitIdle(c.queue); DestroySwapchain(); } }
        if (flags & DXGI_PRESENT_TEST) return Ensure() ? S_OK : DXGI_STATUS_OCCLUDED;
        if (!Ensure()) { ctx->Submit(); Pace(); return S_OK; }
        const uint32_t s = semIndex++ % uint32_t(acquireSems.size());
        uint32_t index = 0;
        VkResult r = vkAcquireNextImageKHR(c.dev, swapchain, 2000000000ull, acquireSems[s], VK_NULL_HANDLE, &index);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR)
        {
            ctx->Submit();
            vkQueueWaitIdle(c.queue);
            DestroySwapchain();
            if (r == VK_ERROR_SURFACE_LOST_KHR) { vkDestroySurfaceKHR(c.inst, surface, nullptr); surface = VK_NULL_HANDLE; }
            return S_OK;
        }
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { ctx->Submit(); return S_OK; }
        // Copy the back buffer into the swap-chain image (scaled, any format), then present it.
        VkCommandBuffer cmd = ctx->Transfer();
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = images[index];
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkImageBlit blit{};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.srcOffsets[1] = { int32_t(back->desc.Width), int32_t(back->desc.Height), 1 };
        blit.dstOffsets[1] = { int32_t(extent.width), int32_t(extent.height), 1 };
        vkCmdBlitImage(cmd, back->image, VK_IMAGE_LAYOUT_GENERAL, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = 0;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        ctx->Submit(acquireSems[s], doneSems[s]);
        VkPresentInfoKHR pi{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &doneSems[s];
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain;
        pi.pImageIndices = &index;
        {
            std::lock_guard l(c.queueMutex);
            r = vkQueuePresentKHR(c.queue, &pi);
        }
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_SURFACE_LOST_KHR)
        {
            vkQueueWaitIdle(c.queue);
            DestroySwapchain();
            if (r == VK_ERROR_SURFACE_LOST_KHR) { vkDestroySurfaceKHR(c.inst, surface, nullptr); surface = VK_NULL_HANDLE; }
        }
        Pace();
        return S_OK;
    }
    // At most `latency` frames queued on the GPU.
    void Pace()
    {
        Context* ctx = device->context;
        frames.push_back(device->core.next.load() - 1);
        while (frames.size() > DxgiDevice::latency) { ctx->WaitFor(frames.front()); frames.pop_front(); }
    }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT, REFIID riid, void** out) override { return back->QueryInterface(riid, out); }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT, UINT w, UINT h, DXGI_FORMAT f, UINT) override
    {
        device->context->Submit();
        vkQueueWaitIdle(device->core.queue);
        DestroySwapchain();
        if (w) desc.Width = w;
        if (h) desc.Height = h;
        if (f != DXGI_FORMAT_UNKNOWN) desc.Format = f;
        if (back) { back->Release(); back = nullptr; }
        return MakeBack() ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL, IDXGIOutput*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* fs, IDXGIOutput** out) override { if (fs) *fs = FALSE; if (out) *out = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput**) override { return DXGI_ERROR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* d) override { *d = desc; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* d) override
    {
        std::memset(d, 0, sizeof(*d));
        d->BufferDesc.Width = desc.Width;
        d->BufferDesc.Height = desc.Height;
        d->BufferDesc.Format = desc.Format;
        d->SampleDesc.Count = 1;
        d->BufferUsage = desc.BufferUsage;
        d->BufferCount = desc.BufferCount;
        d->OutputWindow = reinterpret_cast<HWND>(window);
        d->Windowed = TRUE;
        d->SwapEffect = desc.SwapEffect;
        d->Flags = desc.Flags;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* h) override { *h = reinterpret_cast<HWND>(window); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** out) override { return device->QueryInterface(riid, out); }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* n) override { *n = semIndex; return S_OK; }
};

HRESULT Factory::CreateSwapChainForHwnd(IUnknown*, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,
                                        IDXGIOutput*, IDXGISwapChain1** out)
{
    if (!desc || !out) return E_INVALIDARG;
    DXGI_SWAP_CHAIN_DESC1 d = *desc;
    SDL_Window* w = reinterpret_cast<SDL_Window*>(hwnd);
    if ((!d.Width || !d.Height) && w) { int ww = 0, hh = 0; SDL_GetWindowSize(w, &ww, &hh); d.Width = UINT(ww); d.Height = UINT(hh); }
    auto* s = new SwapChain(device, w, d);
    if (!s->MakeBack()) { s->Release(); return E_OUTOFMEMORY; }
    *out = s;
    return S_OK;
}

} // namespace ttt2vk

// Device's IDXGIDevice: answered here, after DxgiDevice is complete.
namespace ttt2vk {
struct DeviceQi
{
    static HRESULT Query(Device* d, REFIID riid, void** out)
    {
        if (Same(riid, __uuidof(IDXGIDevice)) || Same(riid, __uuidof(IDXGIDevice1)) || Same(riid, __uuidof(IDXGIObject)))
        {
            auto* x = new DxgiDevice(d);
            const HRESULT hr = x->QueryInterface(riid, out);
            x->Release();
            return hr;
        }
        return E_NOINTERFACE;
    }
};
struct DeviceImpl : Device
{
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        const HRESULT hr = Device::QueryInterface(riid, out);
        if (hr != E_NOINTERFACE) return hr;
        return DeviceQi::Query(this, riid, out);
    }
};
} // namespace ttt2vk

bool Ttt2VkRequested()
{
    const char* e = std::getenv("TTT2_RENDERER");
    return e && (std::strcmp(e, "native") == 0 || std::strcmp(e, "vulkan") == 0);
}

HRESULT Ttt2VkCreateDevice(ID3D11Device** device, ID3D11DeviceContext** context)
{
    using namespace ttt2vk;
    auto* d = new DeviceImpl();
    if (!d->Init()) { d->Release(); return E_FAIL; }
    auto* c = new Context(d);
    d->context = c;
    if (!c->Init()) { Log("context setup failed"); d->Release(); return E_FAIL; }
    if (device) *device = AddRefd(static_cast<ID3D11Device*>(d));
    if (context) { *context = c; c->AddRef(); }
    d->Release();
    return S_OK;
}
