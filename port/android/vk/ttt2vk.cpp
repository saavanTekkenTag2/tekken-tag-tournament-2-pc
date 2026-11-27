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
#include <SDL_syswm.h>
#include <vkd3d_shader.h>

#include <algorithm>
#include <condition_variable>
#include <thread>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
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

static std::mutex& RecentMutex() { static std::mutex m; return m; }
static std::string& Recent() { static std::string s; return s; }
static void Log(const char* fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "[native-vk] %s\n", line);
    // r26: kept for the GPU self-test's report (it runs before the game's log is open).
    std::lock_guard l(RecentMutex());
    std::string& r = Recent();
    if (r.size() < 16384) { r += line; r += '\n'; }
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
    // r27: GPU timestamps (D3D11 TIMESTAMP queries): the game's frame timer reports the GPU's time per frame.
    VkQueryPool tsPool = VK_NULL_HANDLE;
    uint64_t tsMask = ~0ull;
    double tsPeriodNs = 1.0;
    std::mutex tsMutex;
    std::vector<int> tsFree;
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
        const VkResult vr = vkAllocateMemory(dev, &a, nullptr, &m);
        if (vr != VK_SUCCESS)
        {
            // r30: textures that cannot get memory are missing in the game: say so (phones with little RAM).
            static std::atomic<int> failures{ 0 };
            const int n = ++failures;
            if (n <= 20 || n % 200 == 0)
                std::fprintf(stderr, "[native-vk] out of GPU memory: %.1f MB could not be allocated (error %d, %d time(s))\n",
                             double(r.size) / 1048576.0, int(vr), n);
            return VK_NULL_HANDLE;
        }
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
// r29: decode = this DXGI format is stored decoded (1 BC1 -> A1R5G5B5, 2 BC2 / 3 BC3 -> RGBA8, 4 BC4 -> R8, 5 BC5 -> RG8):
// GPUs without BC textures (every Mali: "no BC textures!") were given BC images anyway, which the Vulkan spec leaves
// undefined - on Mali-G57/G68 the driver crashed on the command processor thread at boot, or textures were missing.
struct FormatInfo { VkFormat vk; uint32_t bytes; uint32_t block; VkImageAspectFlags aspect; uint8_t decode = 0; };
static bool g_decodeBC = false;         // set once at device creation, before any texture
static bool g_d24AsD32 = false;         // r29: D24S8 cannot be a depth attachment here: D32S8 instead
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
    case DXGI_FORMAT_BC1_UNORM: if (g_decodeBC) return { VK_FORMAT_A1R5G5B5_UNORM_PACK16, 2, 1, C, 1 };
                                return { VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8, 4, C };
    case DXGI_FORMAT_BC2_UNORM: if (g_decodeBC) return { VK_FORMAT_R8G8B8A8_UNORM, 4, 1, C, 2 };
                                return { VK_FORMAT_BC2_UNORM_BLOCK, 16, 4, C };
    case DXGI_FORMAT_BC3_UNORM: if (g_decodeBC) return { VK_FORMAT_R8G8B8A8_UNORM, 4, 1, C, 3 };
                                return { VK_FORMAT_BC3_UNORM_BLOCK, 16, 4, C };
    case DXGI_FORMAT_BC4_UNORM: if (g_decodeBC) return { VK_FORMAT_R8_UNORM, 1, 1, C, 4 };
                                return { VK_FORMAT_BC4_UNORM_BLOCK, 8, 4, C };
    case DXGI_FORMAT_BC5_UNORM: if (g_decodeBC) return { VK_FORMAT_R8G8_UNORM, 2, 1, C, 5 };
                                return { VK_FORMAT_BC5_UNORM_BLOCK, 16, 4, C };
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return { VK_FORMAT_D32_SFLOAT_S8_UINT, 8, 1, DS };
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return { VK_FORMAT_D32_SFLOAT_S8_UINT, 8, 1, VK_IMAGE_ASPECT_DEPTH_BIT };
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT: return { VK_FORMAT_D32_SFLOAT_S8_UINT, 8, 1, VK_IMAGE_ASPECT_STENCIL_BIT };
    case DXGI_FORMAT_D32_FLOAT: return { VK_FORMAT_D32_SFLOAT, 4, 1, VK_IMAGE_ASPECT_DEPTH_BIT };
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT: if (g_d24AsD32) return { VK_FORMAT_D32_SFLOAT_S8_UINT, 8, 1, DS };
                                        return { VK_FORMAT_D24_UNORM_S8_UINT, 4, 1, DS };
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: if (g_d24AsD32) return { VK_FORMAT_D32_SFLOAT_S8_UINT, 8, 1, VK_IMAGE_ASPECT_DEPTH_BIT };
                                            return { VK_FORMAT_D24_UNORM_S8_UINT, 4, 1, VK_IMAGE_ASPECT_DEPTH_BIT };
    default: return { VK_FORMAT_UNDEFINED, 0, 1, C };
    }
}
// r29: BC1-5 blocks to the decoded formats above, on the CPU at upload (src: rows of 4x4 blocks, srcPitch bytes each).
namespace bcdec {
static void Color(const uint8_t* b, bool four, uint8_t out[16][4])
{
    const uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
    uint8_t p[4][4];
    auto unpack = [](uint16_t c, uint8_t* o) {
        const uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, bl = c & 31;
        o[0] = uint8_t(r << 3 | r >> 2); o[1] = uint8_t(g << 2 | g >> 4); o[2] = uint8_t(bl << 3 | bl >> 2); o[3] = 255;
    };
    unpack(c0, p[0]); unpack(c1, p[1]);
    if (four || c0 > c1)
        for (int k = 0; k < 3; ++k)
        {
            p[2][k] = uint8_t((2 * p[0][k] + p[1][k] + 1) / 3);
            p[3][k] = uint8_t((p[0][k] + 2 * p[1][k] + 1) / 3);
        }
    else
        for (int k = 0; k < 3; ++k) { p[2][k] = uint8_t((p[0][k] + p[1][k] + 1) / 2); p[3][k] = 0; }
    p[2][3] = 255;
    p[3][3] = (four || c0 > c1) ? 255 : 0;
    const uint32_t idx = uint32_t(b[4] | b[5] << 8 | b[6] << 16 | uint32_t(b[7]) << 24);
    for (int i = 0; i < 16; ++i) std::memcpy(out[i], p[(idx >> (2 * i)) & 3], 4);
}
static void Channel(const uint8_t* b, uint8_t out[16][4], int ch)      // a BC4 block into channel ch
{
    const uint32_t a0 = b[0], a1 = b[1];
    uint8_t v[8];
    v[0] = uint8_t(a0); v[1] = uint8_t(a1);
    if (a0 > a1) for (int i = 1; i < 7; ++i) v[i + 1] = uint8_t(((7 - i) * a0 + i * a1 + 3) / 7);
    else { for (int i = 1; i < 5; ++i) v[i + 1] = uint8_t(((5 - i) * a0 + i * a1 + 2) / 5); v[6] = 0; v[7] = 255; }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= uint64_t(b[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i) out[i][ch] = v[(bits >> (3 * i)) & 7];
}
static void Decode(uint8_t kind, const uint8_t* src, uint32_t srcPitch, uint32_t w, uint32_t h, uint8_t* dst, uint32_t dstPitch)
{
    const uint32_t blockBytes = (kind == 1 || kind == 4) ? 8 : 16;
    for (uint32_t by = 0; by * 4 < h; ++by)
        for (uint32_t bx = 0; bx * 4 < w; ++bx)
        {
            const uint8_t* b = src + size_t(by) * srcPitch + size_t(bx) * blockBytes;
            uint8_t t[16][4];
            switch (kind)
            {
            case 1: Color(b, false, t); break;
            case 2:
                Color(b + 8, true, t);
                for (int i = 0; i < 16; ++i) { const uint32_t a = (b[i / 2] >> (4 * (i & 1))) & 15; t[i][3] = uint8_t(a * 17); }
                break;
            case 3: Color(b + 8, true, t); Channel(b, t, 3); break;
            case 4: Channel(b, t, 0); break;
            default: Channel(b, t, 0); Channel(b + 8, t, 1); break;
            }
            for (uint32_t y = 0; y < 4 && by * 4 + y < h; ++y)
                for (uint32_t x = 0; x < 4 && bx * 4 + x < w; ++x)
                {
                    const uint8_t* c = t[y * 4 + x];
                    uint8_t* o = dst + size_t(by * 4 + y) * dstPitch;
                    const uint32_t px = bx * 4 + x;
                    switch (kind)
                    {
                    case 1:
                    {
                        const uint16_t v = uint16_t((c[3] >= 128 ? 0x8000u : 0u) | (uint32_t(c[0] >> 3) << 10) | (uint32_t(c[1] >> 3) << 5) | (c[2] >> 3));
                        std::memcpy(o + px * 2, &v, 2);
                        break;
                    }
                    case 4: o[px] = c[0]; break;
                    case 5: o[px * 2] = c[0]; o[px * 2 + 1] = c[1]; break;
                    default: std::memcpy(o + px * 4, c, 4); break;
                    }
                }
        }
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
    uint64_t lastRtFrame = 0;                // r26: frames in a row drawn into (async pipelines only for those)
    uint32_t rtStreak = 0;
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
    // r32: the view's format can't be filtered (no SAMPLED_IMAGE_FILTER_LINEAR on this GPU - RGBA32F/R32F
    // on Adreno, RGBA16/RG16 on some Mali): a linear sampler on it is undefined and came out white
    // (the "white clothes"); the sampler's nearest twin is bound instead.
    bool noFilter = false;
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
    uint64_t id = 0;
    explicit BlendState(Device* d) : Child(d) {}
    void* Find(REFIID riid) override { return Same(riid, __uuidof(ID3D11BlendState)) || Same(riid, __uuidof(ID3D11DeviceChild)) ? static_cast<ID3D11BlendState*>(this) : nullptr; }
    void STDMETHODCALLTYPE GetDesc(D3D11_BLEND_DESC* d) override { *d = desc; }
};
struct RasterState : Child<Stub_ID3D11RasterizerState<ID3D11RasterizerState>>
{
    D3D11_RASTERIZER_DESC desc{};
    uint64_t id = 0;
    explicit RasterState(Device* d) : Child(d) {}
    void* Find(REFIID riid) override { return Same(riid, __uuidof(ID3D11RasterizerState)) || Same(riid, __uuidof(ID3D11DeviceChild)) ? static_cast<ID3D11RasterizerState*>(this) : nullptr; }
    void STDMETHODCALLTYPE GetDesc(D3D11_RASTERIZER_DESC* d) override { *d = desc; }
};
struct DepthState : Child<Stub_ID3D11DepthStencilState<ID3D11DepthStencilState>>
{
    D3D11_DEPTH_STENCIL_DESC desc{};
    uint64_t id = 0;
    explicit DepthState(Device* d) : Child(d) {}
    void* Find(REFIID riid) override { return Same(riid, __uuidof(ID3D11DepthStencilState)) || Same(riid, __uuidof(ID3D11DeviceChild)) ? static_cast<ID3D11DepthStencilState*>(this) : nullptr; }
    void STDMETHODCALLTYPE GetDesc(D3D11_DEPTH_STENCIL_DESC* d) override { *d = desc; }
};
struct Sampler : Child<Stub_ID3D11SamplerState<ID3D11SamplerState>>
{
    D3D11_SAMPLER_DESC desc{};
    VkSampler sampler = VK_NULL_HANDLE;
    VkSampler nearest = VK_NULL_HANDLE;   // r32: for textures that can't be filtered (null: already nearest)
    VkSampler For(bool noFilter) const { return noFilter && nearest ? nearest : sampler; }
    explicit Sampler(Device* d) : Child(d) {}
    ~Sampler() override;
    void* Find(REFIID riid) override { return Same(riid, __uuidof(ID3D11SamplerState)) || Same(riid, __uuidof(ID3D11DeviceChild)) ? static_cast<ID3D11SamplerState*>(this) : nullptr; }
    void STDMETHODCALLTYPE GetDesc(D3D11_SAMPLER_DESC* d) override { *d = desc; }
};

// ---------------------------------------------------------------- shaders
struct Binding { uint32_t kind; uint32_t reg; uint32_t binding; VkDescriptorType type; };   // kind: 0 cbv 1 srv 2 sampler
// r26: a pipeline's description, complete in itself - made on a worker thread (async pipelines) or at once.
struct PipeDesc
{
    Ref<ID3D11VertexShader> vsRef;            // keep the modules alive while a worker builds
    Ref<ID3D11PixelShader> psRef;
    VkShaderModule vsMod = VK_NULL_HANDLE, psMod = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkPrimitiveTopology topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkBool32 restart = VK_FALSE;
    std::vector<VkVertexInputBindingDescription> vbind;
    std::vector<VkVertexInputAttributeDescription> vattr;
    VkPipelineRasterizationStateCreateInfo rs{};
    VkPipelineMultisampleStateCreateInfo ms{};
    VkSampleMask mask = ~0u;
    VkPipelineDepthStencilStateCreateInfo ds{};
    VkPipelineColorBlendAttachmentState cba[8]{};
    uint32_t colorCount = 0;
};
struct PipeKey
{
    uint64_t vs = 0, ps = 0, il = 0, blend = 0, raster = 0, depth = 0, pass = 0;
    uint32_t topo = 0, mask = 0, flags = 0, pad = 0;
    uint32_t strides[8] = {};
    bool operator==(const PipeKey& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
};
struct PipeKeyHash
{
    size_t operator()(const PipeKey& k) const
    {
        const auto* b = reinterpret_cast<const unsigned char*>(&k);
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i < sizeof(k); ++i) { h ^= b[i]; h *= 1099511628211ull; }
        return size_t(h);
    }
};
struct PipeEntry { std::atomic<VkPipeline> pipe{ VK_NULL_HANDLE }; std::atomic<int> state{ 0 }; };   // 0 building 1 ready 2 failed
static VkPipeline CreatePipeline(VkDevice dev, VkPipelineCache cache, const PipeDesc& d)
{
    VkPipelineShaderStageCreateInfo stages[2]{};
    uint32_t n = 0;
    stages[n] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[n].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[n].module = d.vsMod;
    stages[n].pName = "main";
    ++n;
    if (d.psMod)
    {
        stages[n] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        stages[n].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[n].module = d.psMod;
        stages[n].pName = "main";
        ++n;
    }
    VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount = uint32_t(d.vbind.size());
    vi.pVertexBindingDescriptions = d.vbind.data();
    vi.vertexAttributeDescriptionCount = uint32_t(d.vattr.size());
    vi.pVertexAttributeDescriptions = d.vattr.data();
    VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = d.topo;
    ia.primitiveRestartEnable = d.restart;
    VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineMultisampleStateCreateInfo ms = d.ms;
    ms.pSampleMask = &d.mask;
    VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = d.colorCount;
    cb.pAttachments = d.cba;
    const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_REFERENCE };
    VkPipelineDynamicStateCreateInfo dy{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = 4;
    dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo ci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    ci.stageCount = n;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &d.rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &d.ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = d.layout;
    ci.renderPass = d.pass;
    VkPipeline pl = VK_NULL_HANDLE;
    const VkResult r = vkCreateGraphicsPipelines(dev, cache, 1, &ci, nullptr, &pl);
    if (r != VK_SUCCESS) { Log("vkCreateGraphicsPipelines failed (%d)", int(r)); return VK_NULL_HANDLE; }
    return pl;
}
struct ShaderCode
{
    VkShaderModule module = VK_NULL_HANDLE;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    std::vector<Binding> bindings;
    std::vector<std::pair<std::string, uint32_t>> inputs;     // vertex: "SEMANTICn" -> register
    uint32_t dynamicCount = 0;                                // r26: constant buffers bound with dynamic offsets
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
    uint64_t at = 0;            // EVENT: the submission that must finish; TIMESTAMP: the one that wrote it
    bool ended = false;
    int slot = -1;              // r27: TIMESTAMP: its query in the device's timestamp pool
    explicit Query(Device* d) : Child(d) {}
    ~Query() override;
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
    std::atomic<bool> vsSamplesImages{ false };     // r28: a vertex shader with a texture: pass barriers include vertex shaders
    VkCommandPool oneOffPool = VK_NULL_HANDLE;
    std::string gpuName;
    // r26: pipelines built on worker threads (async) and a pipeline cache kept on disk between runs.
    struct PipeJob { PipeDesc d; std::shared_ptr<PipeEntry> e; };
    std::mutex jobMutex;
    std::condition_variable jobCv;
    std::deque<PipeJob> jobs;
    std::atomic<uint64_t> pipelinesMade{ 0 };
    uint64_t pipelinesSaved = 0;
    std::string cachePath;
    void StartWorkers();
    void SavePipelineCache(bool wait);

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
    void NoteVertexTextures(const ShaderCode& c)
    {
        for (const Binding& b : c.bindings)
            if (b.type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && !vsSamplesImages.exchange(true))
                std::fprintf(stderr, "[native-vk] a vertex shader samples a texture: render pass barriers include vertex shaders\n");
    }

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
        s->id = ids++;
        *out = s;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC* desc, ID3D11DepthStencilState** out) override
    {
        if (!out) return S_FALSE;
        auto* s = new DepthState(this);
        s->desc = *desc;
        s->id = ids++;
        *out = s;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateRasterizerState(const D3D11_RASTERIZER_DESC* desc, ID3D11RasterizerState** out) override
    {
        if (!out) return S_FALSE;
        auto* s = new RasterState(this);
        s->desc = *desc;
        s->id = ids++;
        *out = s;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateSamplerState(const D3D11_SAMPLER_DESC* desc, ID3D11SamplerState** out) override;
    bool Filterable(VkFormat f);
    HRESULT STDMETHODCALLTYPE CreateQuery(const D3D11_QUERY_DESC* desc, ID3D11Query** out) override
    {
        if (!out) return S_FALSE;
        if (desc->Query == D3D11_QUERY_TIMESTAMP)
        {
            std::lock_guard l(core.tsMutex);
            if (!core.tsPool || core.tsFree.empty()) return E_FAIL;
            auto* q = new Query(this);
            q->desc = *desc;
            q->slot = core.tsFree.back();
            core.tsFree.pop_back();
            *out = q;
            return S_OK;
        }
        if (desc->Query == D3D11_QUERY_TIMESTAMP_DISJOINT && !core.tsPool) return E_FAIL;
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
Query::~Query()
{
    if (slot < 0) return;
    std::lock_guard l(device->core.tsMutex);
    device->core.tsFree.push_back(slot);
}
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
static void ForgetTargetView(Device* d, VkImageView v);     // after Context
Rtv::~Rtv() { ForgetTargetView(device, view); Core* c = &device->core; VkImageView v = view; c->Defer([c, v] { if (v) vkDestroyImageView(c->dev, v, nullptr); }); }
Dsv::~Dsv() { ForgetTargetView(device, view); Core* c = &device->core; VkImageView v = view; c->Defer([c, v] { if (v) vkDestroyImageView(c->dev, v, nullptr); }); }
Sampler::~Sampler() { Core* c = &device->core; VkSampler s = sampler, n = nearest; c->Defer([c, s, n] { if (s) vkDestroySampler(c->dev, s, nullptr); if (n) vkDestroySampler(c->dev, n, nullptr); }); }
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
        // r26: the last descriptor set written for this stage, reused while only constant-buffer offsets change
        // (they are dynamic offsets): the renderer moves its constant ring at every draw.
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        std::vector<uint64_t> sig;
        uint32_t offsets[16] = {};
        uint32_t offsetCount = 0;
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
        vsStage.set = psStage.set = VK_NULL_HANDLE;
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
        ++statSubmits;
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
        // Sets came from the pools just handed to that submission: written again from here.
        vsStage.set = psStage.set = VK_NULL_HANDLE;
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
        PassBarrier();
    }
    // r28: after a render pass, only its attachment writes are made visible to what reads them next (fragment
    // shaders, attachments, copies). The full ALL_COMMANDS barrier made the GPU go idle between each of ~43 passes a
    // frame, so the next pass's vertex work never overlapped the last one's pixels. Vertex shaders are included only
    // once one samples an image (none did in testing); TTT2_NATIVE_FULL_BARRIERS=1 brings the full barrier back.
    void PassBarrier()
    {
        static const bool full = [] { const char* e = std::getenv("TTT2_NATIVE_FULL_BARRIERS"); return e && *e == '1'; }();
        if (full) { Barrier(); return; }
        VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                           VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT;
        VkPipelineStageFlags dst = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                   VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT;
        if (device->vsSamplesImages.load(std::memory_order_relaxed)) dst |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, dst, 0, 1, &mb, 0, nullptr, 0, nullptr);
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
        FlushClears();
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
                    { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4096 }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 8192 },
                    { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8192 },
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
                    bi.offset = b.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ? 0 : buf->Base() + first;
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
                imgs[i].sampler = s ? s->For(NoFilter(st, b.reg)) : dummySampler;
                w.pImageInfo = &imgs[i];
            }
        }
        vkUpdateDescriptorSets(core.dev, uint32_t(writes.size()), writes.data(), 0, nullptr);
        return set;
    }

    // r32: does a texture this sampler slot serves (slot s pairs with sampler s & 15) lack linear filtering?
    static bool NoFilter(const Stage& st, uint32_t reg)
    {
        for (uint32_t r = reg; r < MaxSrv; r += 16)
            if (const Srv* v = static_cast<const Srv*>(st.srv[r].get())) if (v->noFilter) return true;
        return false;
    }
    // Signature of what a stage's set would hold (dynamic offsets left out) and those offsets.
    void StageState(const ShaderCode& sc, Stage& st, std::vector<uint64_t>& sig, uint32_t* offsets, uint32_t& count)
    {
        sig.clear();
        count = 0;
        const VkDeviceSize maxUbo = core.props.limits.maxUniformBufferRange;
        for (const Binding& b : sc.bindings)
        {
            if (b.kind == 0)
            {
                Buffer* buf = b.reg < MaxCb ? static_cast<Buffer*>(st.cb[b.reg].buffer.get()) : nullptr;
                const Cb& c = st.cb[b.reg < MaxCb ? b.reg : 0];
                uint64_t h = 0, range = 0, off = 0;
                if (buf && buf->Handle())
                {
                    h = uint64_t(reinterpret_cast<uintptr_t>(buf->Handle()));
                    const VkDeviceSize first = c.ranged ? VkDeviceSize(c.first) * 16 : 0;
                    VkDeviceSize r = c.ranged ? VkDeviceSize(c.num) * 16 : buf->desc.ByteWidth;
                    if (first + r > buf->desc.ByteWidth) r = buf->desc.ByteWidth > first ? buf->desc.ByteWidth - first : 16;
                    range = std::min(r, maxUbo);
                    off = buf->Base() + first;
                }
                if (b.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) { if (count < 16) offsets[count++] = uint32_t(off); sig.push_back(h); sig.push_back(range); }
                else { sig.push_back(h); sig.push_back(range ^ (off << 24)); }
            }
            else if (b.kind == 1)
            {
                Srv* v = b.reg < MaxSrv ? static_cast<Srv*>(st.srv[b.reg].get()) : nullptr;
                if (b.type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) sig.push_back(v ? uint64_t(reinterpret_cast<uintptr_t>(v->BufferView())) : 1);
                else sig.push_back(v ? uint64_t(reinterpret_cast<uintptr_t>(v->view)) : 2);
            }
            else
            {
                Sampler* sm = b.reg < MaxSampler ? static_cast<Sampler*>(st.sampler[b.reg].get()) : nullptr;
                sig.push_back(sm ? uint64_t(reinterpret_cast<uintptr_t>(sm->For(NoFilter(st, b.reg)))) : 3);
            }
        }
    }
    // Binds the stage's set (set index `index`), writing a new one only when a texture, sampler or buffer changed.
    void BindStage(const ShaderCode* sc, Stage& st, uint32_t index, VkPipelineLayout layout, bool layoutChanged)
    {
        if (!sc || !sc->layout || sc->bindings.empty()) return;
        std::vector<uint64_t> sig;
        uint32_t offsets[16];
        uint32_t count = 0;
        StageState(*sc, st, sig, offsets, count);
        bool rebind = layoutChanged;
        if (!st.set || st.setLayout != sc->layout || sig != st.sig)
        {
            st.set = WriteSet(*sc, st);
            st.setLayout = sc->layout;
            st.sig.swap(sig);
            rebind = true;
        }
        if (!rebind && (count != st.offsetCount || std::memcmp(offsets, st.offsets, count * sizeof(uint32_t)) != 0)) rebind = true;
        if (!rebind || !st.set) return;
        std::memcpy(st.offsets, offsets, count * sizeof(uint32_t));
        st.offsetCount = count;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, index, 1, &st.set, count, offsets);
    }

    // ---------------------------------------------------------------- render pass
    // loadMask: bit i = colour attachment i starts cleared, bit 8 = depth, bit 9 = stencil (r26: a clear right before
    // a pass is the pass's load op - on a tiled phone GPU the clear costs nothing then).
    VkRenderPass RenderPass(const std::vector<VkFormat>& colors, VkFormat depthFmt, uint32_t loadMask = 0)
    {
        std::vector<uint64_t> key;
        for (VkFormat f : colors) key.push_back(uint64_t(f));
        key.push_back(0xD0000000ull | uint64_t(depthFmt));
        key.push_back(loadMask);
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
            a.loadOp = (loadMask & (1u << i)) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
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
            a.loadOp = (loadMask & 0x100) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            a.stencilLoadOp = (loadMask & 0x200) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
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
        // r26: the GPU gets work as the frame goes, not only at its end (a CPU read mid-frame waited ~30 ms for
        // everything recorded since the last present).
        if (commandsRecorded >= 256) Submit();
        std::vector<VkFormat> colors;
        std::vector<VkImageView> views;
        std::vector<Texture*> textures;
        uint32_t w = ~0u, h = ~0u, maxW = 0, maxH = 0;
        int last = -1;
        for (int i = 0; i < 8; ++i) if (rtv[i]) last = i;
        for (int i = 0; i <= last; ++i)
        {
            Rtv* r = static_cast<Rtv*>(rtv[i].get());
            if (!r) { colors.push_back(VK_FORMAT_UNDEFINED); continue; }
            colors.push_back(r->format);
            views.push_back(r->view);
            textures.push_back(r->tex);
            w = std::min(w, r->tex->W(r->mip)); maxW = std::max(maxW, r->tex->W(r->mip));
            h = std::min(h, r->tex->H(r->mip)); maxH = std::max(maxH, r->tex->H(r->mip));
        }
        VkFormat df = VK_FORMAT_UNDEFINED;
        Dsv* d = static_cast<Dsv*>(dsv.get());
        if (d)
        {
            df = d->format;
            views.push_back(d->view);
            textures.push_back(d->tex);
            w = std::min(w, d->tex->W(d->mip)); maxW = std::max(maxW, d->tex->W(d->mip));
            h = std::min(h, d->tex->H(d->mip)); maxH = std::max(maxH, d->tex->H(d->mip));
        }
        if (views.empty()) return false;
        // Targets drawn into in consecutive frames (async pipelines are for those).
        passHot = true;
        for (Texture* t : textures)
        {
            if (t->lastRtFrame != frameNo) { t->rtStreak = t->lastRtFrame + 1 == frameNo ? t->rtStreak + 1 : 1; t->lastRtFrame = frameNo; }
            if (t->rtStreak < 5) passHot = false;
        }
        // Pending clears of these attachments become the pass's load ops (when every attachment has the same size,
        // so the render area is the whole of each); every other pending clear is done now.
        uint32_t loadMask = 0;
        VkClearValue clears[9]{};
        const bool sameSize = w == maxW && h == maxH;
        if (sameSize && !pendingClears.empty())
        {
            uint32_t ai = 0;
            for (int i = 0; i <= last; ++i)
            {
                Rtv* r = static_cast<Rtv*>(rtv[i].get());
                if (!r) continue;
                auto it = pendingClears.find(r->view);
                if (it != pendingClears.end()) { loadMask |= 1u << i; clears[ai] = it->second.value; pendingClears.erase(it); }
                ++ai;
            }
            if (d)
            {
                auto it = pendingClears.find(d->view);
                if (it != pendingClears.end())
                {
                    const VkImageAspectFlags full = FullAspect(d->format);
                    if (it->second.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) loadMask |= 0x100;
                    if (it->second.aspect & VK_IMAGE_ASPECT_STENCIL_BIT) loadMask |= 0x200;
                    // Only the stencil asked for, on a depth+stencil target: the depth must be kept - done as a clear.
                    if ((full & VK_IMAGE_ASPECT_STENCIL_BIT) && !(loadMask & 0x100) && (loadMask & 0x200)) loadMask &= ~0x200u;
                    if (loadMask & 0x300) { clears[ai] = it->second.value; pendingClears.erase(it); }
                }
            }
        }
        FlushClears();
        FlushNewImages();
        basePass = RenderPass(colors, df, 0);
        pass = loadMask ? RenderPass(colors, df, loadMask) : basePass;
        std::vector<uint64_t> fkey{ uint64_t(reinterpret_cast<uintptr_t>(basePass)), w, h };
        for (VkImageView v : views) fkey.push_back(uint64_t(reinterpret_cast<uintptr_t>(v)));
        VkFramebuffer fb;
        auto it = framebuffers.find(fkey);
        if (it != framebuffers.end()) fb = it->second;
        else
        {
            VkFramebufferCreateInfo ci{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            ci.renderPass = basePass;
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
        bi.clearValueCount = loadMask ? uint32_t(views.size()) : 0;
        bi.pClearValues = loadMask ? clears : nullptr;
        vkCmdBeginRenderPass(Cmd(), &bi, VK_SUBPASS_CONTENTS_INLINE);
        ++statPasses;
        for (int i = 0; i <= last; ++i) if (rtv[i]) { if (loadMask & (1u << i)) ++statClearLoads; else ++statColorLoads; }
        if (d) { if (loadMask & 0x100) ++statClearLoads; else ++statDepthLoads; }
        inPass = true;
        passExtent = { w, h };
        boundPipeline = VK_NULL_HANDLE;
        return true;
    }

    // ---- pending clears (r26)
    struct PendingClear { VkImage image; uint32_t mip, layer; VkImageAspectFlags aspect; VkClearValue value; };
    std::unordered_map<VkImageView, PendingClear> pendingClears;
    // Every pending clear as a transfer clear (outside a pass).
    void FlushClears()
    {
        if (pendingClears.empty()) return;
        EndPass();
        VkCommandBuffer c = Cmd();
        for (auto& [v, pc] : pendingClears)
        {
            const VkImageSubresourceRange range{ pc.aspect, pc.mip, 1, pc.layer, 1 };
            if (pc.aspect & VK_IMAGE_ASPECT_COLOR_BIT) vkCmdClearColorImage(c, pc.image, VK_IMAGE_LAYOUT_GENERAL, &pc.value.color, 1, &range);
            else vkCmdClearDepthStencilImage(c, pc.image, VK_IMAGE_LAYOUT_GENERAL, &pc.value.depthStencil, 1, &range);
        }
        pendingClears.clear();
        Barrier();
    }
    // A view went away (its handle may be reused): no clear for it, no framebuffer with it.
    void ForgetTarget(VkImageView v) { pendingClears.erase(v); ForgetView(v); }
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
    // r26: pipelines keyed by the ids of the immutable state objects (no descriptor copies per draw), built on worker
    // threads for targets drawn into for 5+ frames in a row (a draw waits a frame or two then), at once otherwise
    // (render-to-texture bakes the CPU reads back must not lose a draw).
    uint64_t frameNo = 1, skippedDraws = 0, syncPipelines = 0, asyncPipelines = 0;
    uint64_t statPasses = 0, statColorLoads = 0, statDepthLoads = 0, statClearLoads = 0, statSubmits = 0;   // r27
    VkRenderPass basePass = VK_NULL_HANDLE;           // the LOAD variant: framebuffers and pipelines use it
    bool passHot = false;                            // the current targets were drawn into in the last 5 frames
    std::unordered_map<PipeKey, std::shared_ptr<PipeEntry>, PipeKeyHash> pipelines;
    static bool AsyncOn() { static const bool on = [] { const char* e = std::getenv("TTT2_NATIVE_ASYNC"); return !(e && *e == '0'); }(); return on; }

    bool ClipForced() const
    {
        const char* fe = std::getenv("DXVK_ANDROID_FORCE_DEPTH_CLIP");
#ifdef __ANDROID__
        return !(fe && fe[0] == '0') && !std::getenv("TTT2_NATIVE_DEPTH_CLAMP");
#else
        return fe && fe[0] == '1';
#endif
    }
    void Describe(PipeDesc& o, VkPipelineLayout layout)
    {
        auto* v = static_cast<VertexShader*>(vs.get());
        auto* p = static_cast<PixelShader*>(ps.get());
        auto* l = static_cast<InputLayout*>(il.get());
        auto* b = static_cast<BlendState*>(blend.get());
        auto* r = static_cast<RasterState*>(raster.get());
        auto* d = static_cast<DepthState*>(depth.get());
        D3D11_BLEND_DESC bd{};
        if (b) bd = b->desc;
        else for (auto& t : bd.RenderTarget) t.RenderTargetWriteMask = 0xF;
        D3D11_RASTERIZER_DESC rd{};
        if (r) rd = r->desc;
        else { rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_BACK; rd.DepthClipEnable = TRUE; }
        D3D11_DEPTH_STENCIL_DESC dd{};
        if (d) dd = d->desc;
        else { dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dd.DepthFunc = D3D11_COMPARISON_LESS; dd.StencilReadMask = dd.StencilWriteMask = 0xFF; }
        o.vsRef = vs;
        o.psRef = ps;
        o.vsMod = v->code.module;
        o.psMod = p ? p->code.module : VK_NULL_HANDLE;
        o.layout = layout;
        o.pass = basePass;
        switch (topology)
        {
        case D3D11_PRIMITIVE_TOPOLOGY_POINTLIST: o.topo = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
        case D3D11_PRIMITIVE_TOPOLOGY_LINELIST: o.topo = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
        case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP: o.topo = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; o.restart = VK_TRUE; break;
        case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: o.topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; o.restart = VK_TRUE; break;
        default: o.topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
        }
        if (l)
            for (auto& a : l->attrs)
            {
                o.vattr.push_back({ a.location, a.slot, a.format, a.offset });
                bool have = false;
                for (auto& bnd : o.vbind) if (bnd.binding == a.slot) have = true;
                if (!have) o.vbind.push_back({ a.slot, vb[a.slot].stride, a.instance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX });
            }
        VkPipelineRasterizationStateCreateInfo& rs = o.rs;
        rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        // Adreno drops primitives drawn with depth clamp (self-test 7a): depth clip stays on, as on the DXVK path,
        // unless DXVK_ANDROID_FORCE_DEPTH_CLIP=0.
        const bool clip = rd.DepthClipEnable || ClipForced();
        rs.depthClampEnable = (!clip && core.enabled.depthClamp) ? VK_TRUE : VK_FALSE;
        rs.polygonMode = (rd.FillMode == D3D11_FILL_WIREFRAME && core.enabled.fillModeNonSolid) ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
        rs.cullMode = rd.CullMode == D3D11_CULL_FRONT ? VK_CULL_MODE_FRONT_BIT : rd.CullMode == D3D11_CULL_BACK ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
        // The viewport is flipped (negative height), so D3D's winding names hold as they are.
        rs.frontFace = rd.FrontCounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
        rs.depthBiasEnable = (rd.DepthBias != 0 || rd.SlopeScaledDepthBias != 0.0f) ? VK_TRUE : VK_FALSE;
        rs.depthBiasConstantFactor = float(rd.DepthBias);
        rs.depthBiasSlopeFactor = rd.SlopeScaledDepthBias;
        rs.depthBiasClamp = core.enabled.depthBiasClamp ? rd.DepthBiasClamp : 0.0f;
        rs.lineWidth = 1.0f;
        o.ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        o.ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        o.ms.alphaToCoverageEnable = bd.AlphaToCoverageEnable ? VK_TRUE : VK_FALSE;
        o.mask = sampleMask;
        VkPipelineDepthStencilStateCreateInfo& ds = o.ds;
        ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        const bool hasDepth = dsv.get() != nullptr;
        ds.depthTestEnable = (hasDepth && dd.DepthEnable) ? VK_TRUE : VK_FALSE;
        ds.depthWriteEnable = (hasDepth && dd.DepthEnable && dd.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL) ? VK_TRUE : VK_FALSE;
        ds.depthCompareOp = Compare(dd.DepthFunc);
        ds.stencilTestEnable = (hasDepth && dd.StencilEnable) ? VK_TRUE : VK_FALSE;
        auto face = [&](const D3D11_DEPTH_STENCILOP_DESC& f) {
            VkStencilOpState st{};
            st.failOp = StencilOp(f.StencilFailOp);
            st.passOp = StencilOp(f.StencilPassOp);
            st.depthFailOp = StencilOp(f.StencilDepthFailOp);
            st.compareOp = Compare(f.StencilFunc);
            st.compareMask = dd.StencilReadMask;
            st.writeMask = dd.StencilWriteMask;
            return st;
        };
        ds.front = face(dd.FrontFace);
        ds.back = face(dd.BackFace);
        ds.maxDepthBounds = 1.0f;
        int colorCount = 0;
        for (int i = 0; i < 8; ++i) if (rtv[i]) colorCount = i + 1;
        o.colorCount = uint32_t(colorCount);
        for (int i = 0; i < colorCount; ++i)
        {
            const D3D11_RENDER_TARGET_BLEND_DESC& t = bd.RenderTarget[bd.IndependentBlendEnable ? i : 0];
            VkPipelineColorBlendAttachmentState& c = o.cba[i];
            c.blendEnable = t.BlendEnable ? VK_TRUE : VK_FALSE;
            c.srcColorBlendFactor = Blend(t.SrcBlend);
            c.dstColorBlendFactor = Blend(t.DestBlend);
            c.colorBlendOp = BlendOp(t.BlendOp);
            c.srcAlphaBlendFactor = AlphaOnly(Blend(t.SrcBlendAlpha));
            c.dstAlphaBlendFactor = AlphaOnly(Blend(t.DestBlendAlpha));
            c.alphaBlendOp = BlendOp(t.BlendOpAlpha);
            c.colorWriteMask = t.RenderTargetWriteMask & 0xF;
        }
    }
    VkPipeline Pipeline(VkPipelineLayout layout)
    {
        auto* v = static_cast<VertexShader*>(vs.get());
        auto* p = static_cast<PixelShader*>(ps.get());
        auto* l = static_cast<InputLayout*>(il.get());
        PipeKey k;
        k.vs = v->code.id;
        k.ps = p ? p->code.id : 0;
        k.il = l ? l->id : 0;
        k.blend = blend ? static_cast<BlendState*>(blend.get())->id : 0;
        k.raster = raster ? static_cast<RasterState*>(raster.get())->id : 0;
        k.depth = depth ? static_cast<DepthState*>(depth.get())->id : 0;
        k.pass = uint64_t(reinterpret_cast<uintptr_t>(basePass));
        k.topo = uint32_t(topology);
        k.mask = sampleMask;
        k.flags = (dsv ? 1u : 0u) | (ClipForced() ? 2u : 0u);
        if (l) for (auto& a : l->attrs) k.strides[a.slot & 7] = vb[a.slot].stride;
        auto it = pipelines.find(k);
        if (it != pipelines.end())
        {
            PipeEntry& e = *it->second;
            const int st = e.state.load();
            if (st == 1) return e.pipe.load();
            if (st == 2) return VK_NULL_HANDLE;
            if (passHot) { ++skippedDraws; return VK_NULL_HANDLE; }
            // Still building and this target is new: wait for it (a bake must not lose a draw).
            while (e.state.load() == 0) std::this_thread::sleep_for(std::chrono::microseconds(200));
            return e.pipe.load();
        }
        auto e = std::make_shared<PipeEntry>();
        pipelines.emplace(k, e);
        PipeDesc d;
        Describe(d, layout);
        if (passHot && AsyncOn())
        {
            ++asyncPipelines;
            {
                std::lock_guard g(device->jobMutex);
                device->jobs.push_back({ std::move(d), e });
            }
            device->jobCv.notify_one();
            ++skippedDraws;
            return VK_NULL_HANDLE;
        }
        ++syncPipelines;
        const VkPipeline pl = CreatePipeline(core.dev, core.pipelineCache, d);
        e->pipe = pl;
        e->state = pl ? 1 : 2;
        ++device->pipelinesMade;
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
        const bool layoutChanged = layout != boundLayout;
        boundLayout = layout;
        BindStage(&v->code, vsStage, 0, layout, layoutChanged);
        BindStage(p ? &p->code : nullptr, psStage, 1, layout, layoutChanged);
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
    // r26: clears are recorded and done as the next render pass's load op (free on a tiled GPU) - or with
    // vkCmdClearAttachments when the view is an attachment of the pass already open - instead of a transfer each.
    void STDMETHODCALLTYPE ClearRenderTargetView(ID3D11RenderTargetView* v, const FLOAT color[4]) override
    {
        Rtv* r = static_cast<Rtv*>(v);
        if (!r || !r->tex) return;
        VkClearValue cv{};
        std::memcpy(cv.color.float32, color, sizeof(cv.color.float32));
        if (r->format == VK_FORMAT_R32G32B32A32_UINT || r->format == VK_FORMAT_R32_UINT || r->format == VK_FORMAT_R8G8B8A8_UINT)
            for (int i = 0; i < 4; ++i) cv.color.uint32[i] = uint32_t(color[i]);
        if (inPass && !targetsDirty && r->tex->W(r->mip) == passExtent.width && r->tex->H(r->mip) == passExtent.height)
        {
            uint32_t idx = 0;
            for (int i = 0; i < 8; ++i)
            {
                if (rtv[i].get() == v)
                {
                    VkClearAttachment ca{ VK_IMAGE_ASPECT_COLOR_BIT, idx, cv };
                    VkClearRect rect{ { { 0, 0 }, passExtent }, 0, 1 };
                    vkCmdClearAttachments(cmd, 1, &ca, 1, &rect);
                    ++commandsRecorded;
                    return;
                }
                if (rtv[i]) ++idx;
            }
        }
        pendingClears[r->view] = { r->tex->image, r->mip, r->layer, VK_IMAGE_ASPECT_COLOR_BIT, cv };
        if (inPass) targetsDirty = true;          // a draw in this pass may sample it: the pass restarts after the clear
    }
    void STDMETHODCALLTYPE ClearDepthStencilView(ID3D11DepthStencilView* v, UINT flags, FLOAT depthValue, UINT8 stencil) override
    {
        Dsv* d = static_cast<Dsv*>(v);
        if (!d || !d->tex) return;
        VkImageAspectFlags aspect = 0;
        if (flags & D3D11_CLEAR_DEPTH) aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if ((flags & D3D11_CLEAR_STENCIL) && (FullAspect(d->format) & VK_IMAGE_ASPECT_STENCIL_BIT)) aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
        if (!aspect) return;
        VkClearValue cv{};
        cv.depthStencil = { depthValue, stencil };
        if (inPass && !targetsDirty && dsv.get() == v && d->tex->W(d->mip) == passExtent.width && d->tex->H(d->mip) == passExtent.height)
        {
            uint32_t idx = 0;
            for (int i = 0; i < 8; ++i) if (rtv[i]) ++idx;
            VkClearAttachment ca{ aspect, idx, cv };
            VkClearRect rect{ { { 0, 0 }, passExtent }, 0, 1 };
            vkCmdClearAttachments(cmd, 1, &ca, 1, &rect);
            ++commandsRecorded;
            return;
        }
        auto it = pendingClears.find(d->view);
        if (it != pendingClears.end() && it->second.aspect != aspect)
        {
            // A depth clear then a stencil clear (or the reverse): merged.
            if (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) it->second.value.depthStencil.depth = depthValue;
            if (aspect & VK_IMAGE_ASPECT_STENCIL_BIT) it->second.value.depthStencil.stencil = stencil;
            it->second.aspect |= aspect;
        }
        else pendingClears[d->view] = { d->tex->image, d->mip, d->layer, aspect, cv };
        if (inPass) targetsDirty = true;
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
        if (t->fmt.decode) bcdec::Decode(t->fmt.decode, static_cast<const uint8_t*>(data), rowPitch, w, h, s.ptr(), rowBytes);
        else
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
        if (q->desc.Query == D3D11_QUERY_TIMESTAMP && q->slot >= 0 && core.tsPool)
        {
            // Reset outside a render pass (the frame timer's points are at present, between passes).
            EndPass();
            FlushClears();
            vkCmdResetQueryPool(Cmd(), core.tsPool, uint32_t(q->slot), 1);
            vkCmdWriteTimestamp(Cmd(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, core.tsPool, uint32_t(q->slot));
            ++commandsRecorded;
        }
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
                *static_cast<D3D11_QUERY_DATA_TIMESTAMP_DISJOINT*>(data) =
                    { core.tsPool ? uint64_t(1e9 / core.tsPeriodNs + 0.5) : 1000000000ull, core.tsPool ? FALSE : TRUE };
            return S_OK;
        case D3D11_QUERY_TIMESTAMP:
        {
            if (!q->ended || q->slot < 0 || !core.tsPool) return S_FALSE;
            if (q->at >= core.next.load() && !(flags & D3D11_ASYNC_GETDATA_DONOTFLUSH)) Submit();
            Poll();
            if (core.completed.load() < q->at) return S_FALSE;
            uint64_t v = 0;
            if (vkGetQueryPoolResults(core.dev, core.tsPool, uint32_t(q->slot), 1, sizeof(v), &v, sizeof(v), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
                return S_FALSE;
            if (data && size >= sizeof(UINT64)) *static_cast<UINT64*>(data) = v & core.tsMask;
            return S_OK;
        }
        default:
            if (data && size) std::memset(data, 0, size);
            return S_OK;
        }
    }
    void STDMETHODCALLTYPE Flush() override { Submit(); }
};

static void ForgetTargetView(Device* d, VkImageView v) { if (d->context && v) d->context->ForgetTarget(v); }

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
    {
        const char* e = std::getenv("TTT2_NATIVE_DECODE_BC");     // 1 = decode even where BC is supported (testing)
        g_decodeBC = !sup.textureCompressionBC || (e && *e == '1');
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(core.phys, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
        g_d24AsD32 = !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
    }
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
    if (qf[core.family].timestampValidBits && core.props.limits.timestampPeriod > 0.0f && !std::getenv("TTT2_NATIVE_NO_TIMESTAMPS"))
    {
        VkQueryPoolCreateInfo qi{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 64;
        if (vkCreateQueryPool(core.dev, &qi, nullptr, &core.tsPool) == VK_SUCCESS)
        {
            const uint32_t bits = qf[core.family].timestampValidBits;
            core.tsMask = bits >= 64 ? ~0ull : ((1ull << bits) - 1);
            core.tsPeriodNs = core.props.limits.timestampPeriod;
            for (int i = 63; i >= 0; --i) core.tsFree.push_back(i);
        }
        else core.tsPool = VK_NULL_HANDLE;
    }
    // r26: the pipeline cache from the last run (TTT2Recomp/native_pipelines.bin): pipelines built before are
    // not built again (the first second of a match spent 347 ms in vkCreateGraphicsPipelines, r25 log).
    std::vector<char> cacheData;
    if (const char* la = std::getenv("LOCALAPPDATA"))
    {
        cachePath = std::string(la) + "/TTT2Recomp/native_pipelines.bin";
        if (FILE* f = std::fopen(cachePath.c_str(), "rb"))
        {
            std::fseek(f, 0, SEEK_END);
            const long n = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            if (n > 0 && n < (256l << 20)) { cacheData.resize(size_t(n)); if (std::fread(cacheData.data(), 1, size_t(n), f) != size_t(n)) cacheData.clear(); }
            std::fclose(f);
        }
    }
    VkPipelineCacheCreateInfo pci{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    pci.initialDataSize = cacheData.size();
    pci.pInitialData = cacheData.empty() ? nullptr : cacheData.data();
    if (vkCreatePipelineCache(core.dev, &pci, nullptr, &core.pipelineCache) != VK_SUCCESS)
    {
        pci.initialDataSize = 0;
        pci.pInitialData = nullptr;
        vkCreatePipelineCache(core.dev, &pci, nullptr, &core.pipelineCache);
        cacheData.clear();
    }
    Log("pipeline cache: %zu KB from the last run", cacheData.size() / 1024);
    StartWorkers();
    VkDescriptorSetLayoutCreateInfo empty{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    vkCreateDescriptorSetLayout(core.dev, &empty, nullptr, &emptySetLayout);
    VkCommandPoolCreateInfo cp{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = core.family;
    vkCreateCommandPool(core.dev, &cp, nullptr, &oneOffPool);
    Log("device: %s, Vulkan %u.%u.%u, driver %08X%s%s", gpuName.c_str(), VK_VERSION_MAJOR(core.props.apiVersion), VK_VERSION_MINOR(core.props.apiVersion),
        VK_VERSION_PATCH(core.props.apiVersion), core.props.driverVersion, sup.textureCompressionBC ? "" : " (no BC textures!)",
        sup.depthClamp ? "" : " (no depth clamp)");
    // r29: what this GPU cannot do with the formats the game uses, so a phone's log shows why something is missing.
    if (std::getenv("TTT2_GFX_COMPAT")) Log("Graphics fix: on (safest path: full barriers, waited uploads, decoded BC, no texture memo)");
    if (g_decodeBC) Log("BC textures are decoded on upload (BC1 -> A1R5G5B5, BC2/BC3 -> RGBA8, BC4 -> R8, BC5 -> RG8)");
    if (g_d24AsD32) Log("D24S8 is not a depth format here: D32S8 is used instead");
    {
        struct Want { DXGI_FORMAT f; const char* name; bool rt; };
        static const Want wants[] = {
            { DXGI_FORMAT_R8G8B8A8_UNORM, "RGBA8", true }, { DXGI_FORMAT_B8G8R8A8_UNORM, "BGRA8", true },
            { DXGI_FORMAT_R10G10B10A2_UNORM, "RGB10A2", true }, { DXGI_FORMAT_R11G11B10_FLOAT, "R11G11B10F", true },
            { DXGI_FORMAT_R16G16B16A16_FLOAT, "RGBA16F", true }, { DXGI_FORMAT_R16G16B16A16_UNORM, "RGBA16", true },
            { DXGI_FORMAT_R32G32B32A32_FLOAT, "RGBA32F", false }, { DXGI_FORMAT_R32_FLOAT, "R32F", true },
            { DXGI_FORMAT_R16G16_FLOAT, "RG16F", true }, { DXGI_FORMAT_R16G16_UNORM, "RG16", true }, { DXGI_FORMAT_R8_UNORM, "R8", true },
            { DXGI_FORMAT_R8G8_UNORM, "RG8", true }, { DXGI_FORMAT_B5G6R5_UNORM, "R5G6B5", false }, { DXGI_FORMAT_B5G5R5A1_UNORM, "A1R5G5B5", false },
            { DXGI_FORMAT_R16_UNORM, "R16", false }, { DXGI_FORMAT_R16_FLOAT, "R16F", true }, { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, "RGBA8 sRGB", true },
            { DXGI_FORMAT_BC1_UNORM, "BC1", false }, { DXGI_FORMAT_BC3_UNORM, "BC3", false }, { DXGI_FORMAT_BC5_UNORM, "BC5", false },
            { DXGI_FORMAT_D24_UNORM_S8_UINT, "D24S8", true }, { DXGI_FORMAT_D32_FLOAT_S8X24_UINT, "D32S8", true }, { DXGI_FORMAT_D32_FLOAT, "D32", true },
        };
        std::string missing;
        for (const Want& w : wants)
        {
            const FormatInfo fi = Format(w.f);
            VkFormatProperties fp{};
            vkGetPhysicalDeviceFormatProperties(core.phys, fi.vk, &fp);
            const VkFormatFeatureFlags ft = fp.optimalTilingFeatures;
            const bool depth = IsDepth(fi.vk);
            std::string m;
            if (!(ft & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) m += " sample";
            else if (!depth && !(ft & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) m += " filter";
            if (w.rt && depth && !(ft & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) m += " depth";
            if (w.rt && !depth && !(ft & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) m += " render";
            if (w.rt && !depth && (ft & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) && !(ft & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)) m += " blend";
            if (!m.empty()) missing += std::string(missing.empty() ? "" : ";") + " " + w.name + ":" + m;
        }
        if (!missing.empty()) Log("formats without:%s", missing.c_str());
        else Log("formats: everything the game uses is supported");
    }
    return true;
}

Device::~Device()
{
    SavePipelineCache(true);
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

void Device::StartWorkers()
{
    const char* e = std::getenv("TTT2_NATIVE_PIPELINE_THREADS");
    // r27: 3 by default (r26 log: 2 threads left 17263 draws waiting for pipelines in the first seconds of a match).
    const int n = e && std::atoi(e) > 0 ? std::min(std::atoi(e), 4) : 3;
    for (int i = 0; i < n; ++i)
        std::thread([this] {
            setpriority(PRIO_PROCESS, 0, 5);
            for (;;)
            {
                PipeJob job;
                {
                    std::unique_lock l(jobMutex);
                    jobCv.wait(l, [this] { return !jobs.empty(); });
                    job = std::move(jobs.front());
                    jobs.pop_front();
                }
                const VkPipeline p = CreatePipeline(core.dev, core.pipelineCache, job.d);
                job.e->pipe = p;
                job.e->state = p ? 1 : 2;
                ++pipelinesMade;
            }
        }).detach();
}

void Device::SavePipelineCache(bool wait)
{
    if (cachePath.empty() || !core.pipelineCache) return;
    const uint64_t made = pipelinesMade.load();
    if (made == pipelinesSaved) return;
    pipelinesSaved = made;
    auto save = [this] {
        size_t n = 0;
        if (vkGetPipelineCacheData(core.dev, core.pipelineCache, &n, nullptr) != VK_SUCCESS || !n) return;
        std::vector<char> data(n);
        if (vkGetPipelineCacheData(core.dev, core.pipelineCache, &n, data.data()) != VK_SUCCESS) return;
        const std::string tmp = cachePath + ".tmp";
        if (FILE* f = std::fopen(tmp.c_str(), "wb"))
        {
            const bool ok = std::fwrite(data.data(), 1, n, f) == n;
            std::fclose(f);
            if (ok && std::rename(tmp.c_str(), cachePath.c_str()) == 0) Log("pipeline cache saved: %zu KB", n / 1024);
            else Log("pipeline cache NOT saved (%s)", cachePath.c_str());
        }
        else Log("pipeline cache NOT saved: cannot write %s", tmp.c_str());
    };
    static std::mutex saveMutex;     // r27: one save at a time (two used to write the same .tmp)
    if (wait) { std::lock_guard l(saveMutex); save(); }
    else std::thread([save] { std::lock_guard l(saveMutex); save(); }).detach();
}

void Device::OneOff(const std::function<void(VkCommandBuffer)>& record)
{
    // r28: no longer waits. A texture or buffer created with data used to wait for the GPU here - behind the whole
    // frame already queued (r27 log: "textures 18.6 ms, of which uploads 17.4" frames at match start). The work is
    // submitted now; the queue runs it before anything submitted later, and the barrier at its end makes its writes
    // visible to that later work. Its command buffer and fence are freed once the context's next submission is done
    // (a fence signals only after all work submitted before it).
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
    VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    vkEndCommandBuffer(c);
    VkFenceCreateInfo fi{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence f;
    vkCreateFence(core.dev, &fi, nullptr, &f);
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &c;
    static const bool wait = [] { const char* e = std::getenv("TTT2_NATIVE_ONEOFF_WAIT"); return e && *e == '1'; }();
    uint64_t after;
    {
        std::lock_guard q(core.queueMutex);
        vkQueueSubmit(core.queue, 1, &si, f);
        after = core.next.load();       // the context's next submission: queued after this one
    }
    if (wait)
    {
        vkWaitForFences(core.dev, 1, &f, VK_TRUE, 10000000000ull);
        vkDestroyFence(core.dev, f, nullptr);
        vkFreeCommandBuffers(core.dev, oneOffPool, 1, &c);
        return;
    }
    VkDevice dev = core.dev;
    VkCommandPool pool = oneOffPool;
    std::mutex* mtx = &oneOffMutex;
    std::lock_guard d(core.deferMutex);
    core.deferred.emplace_back(after, [dev, pool, mtx, c, f] {
        vkWaitForFences(dev, 1, &f, VK_TRUE, 10000000000ull);   // done already (its successor finished); cheap
        vkDestroyFence(dev, f, nullptr);
        std::lock_guard l(*mtx);
        vkFreeCommandBuffers(dev, pool, 1, &c);
    });
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
                if (fi.decode)
                    bcdec::Decode(fi.decode, static_cast<const uint8_t*>(d.pSysMem), d.SysMemPitch, t->W(m), t->H(m), s.ptr(), rowBytes);
                else
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
        v->noFilter = !Filterable(ci.format);
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

// r32: can this GPU filter the format linearly? Asked once per format; the first one that can't is logged.
bool Device::Filterable(VkFormat f)
{
    static std::mutex m;
    static std::map<VkFormat, bool> known;
    std::lock_guard l(m);
    auto it = known.find(f);
    if (it != known.end()) return it->second;
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(core.phys, f, &fp);
    const bool ok = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    known[f] = ok;
    if (!ok) Log("format %d can't be filtered here: sampled nearest", int(f));
    return ok;
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
    if (ci.magFilter == VK_FILTER_LINEAR || ci.minFilter == VK_FILTER_LINEAR || ci.mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR || ci.anisotropyEnable)
    {
        ci.magFilter = ci.minFilter = VK_FILTER_NEAREST;
        ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ci.anisotropyEnable = VK_FALSE;
        ci.maxAnisotropy = 1.0f;
        if (vkCreateSampler(core.dev, &ci, nullptr, &s->nearest) != VK_SUCCESS) s->nearest = VK_NULL_HANDLE;
    }
    *out = s;
    return S_OK;
}

// ---------------------------------------------------------------- shaders through vkd3d-shader
// r28: SPIR-V kept on disk (TTT2Recomp/native_spv/<hash>.spv): the start-up shader warm-up converted all 5632 pixel
// shaders from DXBC again on every run (r27 log: 17.9 s on 4 threads, competing with the menu for the CPU).
namespace spvcache {
static std::string Dir()
{
    static const std::string d = [] {
        const char* la = std::getenv("LOCALAPPDATA");
        if (!la || std::getenv("TTT2_NATIVE_NO_SPV_CACHE")) return std::string();
        std::string p = std::string(la) + "/TTT2Recomp/native_spv";
        mkdir((std::string(la) + "/TTT2Recomp").c_str(), 0755);
        mkdir(p.c_str(), 0755);
        return p;
    }();
    return d;
}
static std::string Path(const void* code, size_t size, bool vertex, uint32_t dynLimit)
{
    const std::string d = Dir();
    if (d.empty()) return d;
    uint64_t h = 1469598103934665603ull, h2 = 0x9E3779B97F4A7C15ull;
    const uint8_t* p = static_cast<const uint8_t*>(code);
    for (size_t i = 0; i < size; ++i) { h = (h ^ p[i]) * 1099511628211ull; h2 = (h2 + p[i]) * 0x100000001B3ull ^ (h2 >> 29); }
    char name[96];
    std::snprintf(name, sizeof(name), "/%016llx%08llx_%zx_%c%u_v1.spv", (unsigned long long)h, (unsigned long long)(h2 & 0xFFFFFFFFull),
                  size, vertex ? 'v' : 'p', dynLimit);
    return d + name;
}
struct Rec { std::vector<Binding> bindings; std::vector<std::pair<std::string, uint32_t>> inputs; uint32_t dynamicCount = 0; std::vector<uint32_t> spv; };
static bool Load(const std::string& path, Rec& r)
{
    if (path.empty()) return false;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> b;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 16 && n < (8l << 20)) { b.resize(size_t(n)); if (std::fread(b.data(), 1, b.size(), f) != b.size()) b.clear(); }
    std::fclose(f);
    size_t at = 0;
    auto u32 = [&](uint32_t& v) { if (at + 4 > b.size()) return false; std::memcpy(&v, b.data() + at, 4); at += 4; return true; };
    uint32_t magic, nb, ni, ns;
    if (!u32(magic) || magic != 0x56505354u || !u32(r.dynamicCount) || !u32(nb) || nb > 256) return false;
    for (uint32_t i = 0; i < nb; ++i)
    {
        Binding x{}; uint32_t t;
        if (!u32(x.kind) || !u32(x.reg) || !u32(x.binding) || !u32(t)) return false;
        x.type = VkDescriptorType(t);
        r.bindings.push_back(x);
    }
    if (!u32(ni) || ni > 64) return false;
    for (uint32_t i = 0; i < ni; ++i)
    {
        uint32_t len, reg;
        if (!u32(len) || len > 256 || at + len > b.size()) return false;
        std::string name(reinterpret_cast<const char*>(b.data() + at), len);
        at += len;
        if (!u32(reg)) return false;
        r.inputs.emplace_back(name, reg);
    }
    if (!u32(ns) || !ns || ns % 4 || at + ns != b.size()) return false;
    r.spv.resize(ns / 4);
    std::memcpy(r.spv.data(), b.data() + at, ns);
    return r.spv[0] == 0x07230203u;     // SPIR-V magic
}
static void Store(const std::string& path, const ShaderCode& c, const void* spv, size_t spvSize)
{
    if (path.empty()) return;
    std::vector<uint8_t> b;
    auto u32 = [&](uint32_t v) { const size_t at = b.size(); b.resize(at + 4); std::memcpy(b.data() + at, &v, 4); };
    u32(0x56505354u); u32(c.dynamicCount); u32(uint32_t(c.bindings.size()));
    for (const Binding& x : c.bindings) { u32(x.kind); u32(x.reg); u32(x.binding); u32(uint32_t(x.type)); }
    u32(uint32_t(c.inputs.size()));
    for (const auto& in : c.inputs) { u32(uint32_t(in.first.size())); b.insert(b.end(), in.first.begin(), in.first.end()); u32(in.second); }
    u32(uint32_t(spvSize));
    b.insert(b.end(), static_cast<const uint8_t*>(spv), static_cast<const uint8_t*>(spv) + spvSize);
    char tmp[32];
    std::snprintf(tmp, sizeof(tmp), ".%lx.tmp", (long)gettid());
    const std::string t = path + tmp;
    if (FILE* f = std::fopen(t.c_str(), "wb"))
    {
        const bool ok = std::fwrite(b.data(), 1, b.size(), f) == b.size();
        std::fclose(f);
        if (!ok || std::rename(t.c_str(), path.c_str()) != 0) std::remove(t.c_str());
    }
}
static std::atomic<uint64_t> hits{0}, misses{0};
}

// The cached form back into a shader: module, set layout (from the bindings), inputs.
static bool FromCache(Core& core, bool vertex, spvcache::Rec& r, ShaderCode& out)
{
    VkShaderModuleCreateInfo mi{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    mi.codeSize = r.spv.size() * 4;
    mi.pCode = r.spv.data();
    if (vkCreateShaderModule(core.dev, &mi, nullptr, &out.module) != VK_SUCCESS) return false;
    std::vector<VkDescriptorSetLayoutBinding> lb;
    for (const Binding& b : r.bindings)
    {
        VkDescriptorSetLayoutBinding l{};
        l.binding = b.binding & 0x7FFF;
        l.descriptorType = b.type;
        l.descriptorCount = 1;
        l.stageFlags = vertex ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
        lb.push_back(l);
    }
    VkDescriptorSetLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    lci.bindingCount = uint32_t(lb.size());
    lci.pBindings = lb.data();
    vkCreateDescriptorSetLayout(core.dev, &lci, nullptr, &out.layout);
    out.bindings = std::move(r.bindings);
    out.inputs = std::move(r.inputs);
    out.dynamicCount = r.dynamicCount;
    return true;
}

bool Device::CompileShader(const void* code, SIZE_T size, bool vertex, ShaderCode& out)
{
    const uint32_t dynLimit = std::min<uint32_t>(8, core.props.limits.maxDescriptorSetUniformBuffersDynamic / 2);
    const std::string cachePath = spvcache::Path(code, size, vertex, dynLimit);
    {
        spvcache::Rec rec;
        if (spvcache::Load(cachePath, rec) && FromCache(core, vertex, rec, out))
        {
            ++spvcache::hits;
            if (vertex) NoteVertexTextures(out);
            out.id = ids++;
            return true;
        }
    }
    ++spvcache::misses;
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
            b.kind = 0; rb.flags = VKD3D_SHADER_BINDING_FLAG_BUFFER;
            // r26: dynamic offsets (the set is reused while only the constant ring's offset moves), within the
            // device's limit shared by the two stages.
            if (out.dynamicCount < dynLimit)
            { b.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; ++out.dynamicCount; }
            else b.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            break;
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
    std::vector<uint8_t> spvCopy(static_cast<const uint8_t*>(spv.code), static_cast<const uint8_t*>(spv.code) + spv.size);
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
    spvcache::Store(cachePath, out, spvCopy.data(), spvCopy.size());
    if (vertex) NoteVertexTextures(out);
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
            bool haveWindow = window != nullptr;
#if defined(__ANDROID__) && defined(SDL_VIDEO_DRIVER_ANDROID)
            // r25: the game's window has no ANativeWindow while the app is in the background; a surface made then
            // crashed inside the Vulkan loader (r24 crash.txt: SIGSEGV at 0x98 in libvulkan.so after SURFACE_LOST).
            if (haveWindow)
            {
                SDL_SysWMinfo wm;
                SDL_VERSION(&wm.version);
                haveWindow = SDL_GetWindowWMInfo(window, &wm) && wm.info.android.window != nullptr;
            }
#endif
            if (!haveWindow || SDL_Vulkan_CreateSurface(window, c.inst, &surface) != SDL_TRUE)
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
        // r25: SUBOPTIMAL is fine - a phone in landscape answers it at every present (its surface is rotated 90 degrees
        // from the display's natural portrait and the compositor rotates). r24 rebuilt the swap chain at each one:
        // 6384 rebuilds in one session, ~27 ms a present, 30 fps (MEASURED 2026-11-26, Adreno 810).
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR)
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
        ++ctx->frameNo;
        if (ctx->frameNo % 300 == 0)
        {
            if (ctx->skippedDraws || ctx->asyncPipelines || ctx->syncPipelines)
                Log("last 300 frames: %llu pipelines built at once, %llu in the background, %llu draws waited for one",
                    (unsigned long long)ctx->syncPipelines, (unsigned long long)ctx->asyncPipelines, (unsigned long long)ctx->skippedDraws);
            if (spvcache::misses.load() || spvcache::hits.load())
                Log("shaders: %llu from the SPIR-V cache, %llu converted (and stored)",
                    (unsigned long long)spvcache::hits.exchange(0), (unsigned long long)spvcache::misses.exchange(0));
            Log("per frame: %.1f render passes (attachments loaded: %.1f colour, %.1f depth; %.1f cleared at load), %.1f submits",
                ctx->statPasses / 300.0, ctx->statColorLoads / 300.0, ctx->statDepthLoads / 300.0, ctx->statClearLoads / 300.0,
                ctx->statSubmits / 300.0);
            ctx->skippedDraws = ctx->asyncPipelines = ctx->syncPipelines = 0;
            ctx->statPasses = ctx->statColorLoads = ctx->statDepthLoads = ctx->statClearLoads = ctx->statSubmits = 0;
            device->SavePipelineCache(false);
        }
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

std::string Ttt2VkTakeRecentLog()
{
    std::lock_guard l(ttt2vk::RecentMutex());
    std::string r;
    r.swap(ttt2vk::Recent());
    return r;
}

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
