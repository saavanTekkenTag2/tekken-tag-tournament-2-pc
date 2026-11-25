// Microsoft::WRL::ComPtr (the subset the runtime uses).
#pragma once
#include <windows.h>
#include <utility>
namespace Microsoft { namespace WRL {
template <typename T> class ComPtr {
public:
    typedef T InterfaceType;
    ComPtr() noexcept = default;
    ComPtr(std::nullptr_t) noexcept {}
    template <typename U> ComPtr(U* p) noexcept : ptr_(p) { AddRefInternal(); }
    ComPtr(const ComPtr& o) noexcept : ptr_(o.ptr_) { AddRefInternal(); }
    template <typename U> ComPtr(const ComPtr<U>& o) noexcept : ptr_(o.Get()) { AddRefInternal(); }
    ComPtr(ComPtr&& o) noexcept : ptr_(o.ptr_) { o.ptr_ = nullptr; }
    template <typename U> ComPtr(ComPtr<U>&& o) noexcept : ptr_(o.Detach()) {}
    ~ComPtr() { ReleaseInternal(); }
    ComPtr& operator=(std::nullptr_t) noexcept { ReleaseInternal(); return *this; }
    ComPtr& operator=(T* p) noexcept { if (ptr_ != p) { ComPtr(p).Swap(*this); } return *this; }
    ComPtr& operator=(const ComPtr& o) noexcept { if (ptr_ != o.ptr_) { ComPtr(o).Swap(*this); } return *this; }
    template <typename U> ComPtr& operator=(const ComPtr<U>& o) noexcept { ComPtr(o).Swap(*this); return *this; }
    ComPtr& operator=(ComPtr&& o) noexcept { ComPtr(std::move(o)).Swap(*this); return *this; }
    template <typename U> ComPtr& operator=(ComPtr<U>&& o) noexcept { ComPtr(std::move(o)).Swap(*this); return *this; }
    void Swap(ComPtr& o) noexcept { std::swap(ptr_, o.ptr_); }
    void Swap(ComPtr&& o) noexcept { std::swap(ptr_, o.ptr_); }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }
    T* Get() const noexcept { return ptr_; }
    T* operator->() const noexcept { return ptr_; }
    T** operator&() noexcept { ReleaseInternal(); return &ptr_; }
    T* const* GetAddressOf() const noexcept { return &ptr_; }
    T** GetAddressOf() noexcept { return &ptr_; }
    T** ReleaseAndGetAddressOf() noexcept { ReleaseInternal(); return &ptr_; }
    T* Detach() noexcept { T* p = ptr_; ptr_ = nullptr; return p; }
    void Attach(T* p) noexcept { ReleaseInternal(); ptr_ = p; }
    unsigned long Reset() noexcept { return ReleaseInternal(); }
    HRESULT CopyTo(T** p) const noexcept { AddRefInternal(); *p = ptr_; return S_OK; }
    template <typename U> HRESULT As(ComPtr<U>* p) const noexcept { return ptr_->QueryInterface(__uuidof(U), reinterpret_cast<void**>(p->ReleaseAndGetAddressOf())); }
    template <typename U> HRESULT As(U** p) const noexcept { return ptr_->QueryInterface(__uuidof(U), reinterpret_cast<void**>(p)); }
private:
    void AddRefInternal() const noexcept { if (ptr_) ptr_->AddRef(); }
    unsigned long ReleaseInternal() noexcept { unsigned long r = 0; T* p = ptr_; if (p) { ptr_ = nullptr; r = p->Release(); } return r; }
    T* ptr_ = nullptr;
};
template <typename T, typename U> bool operator==(const ComPtr<T>& a, const ComPtr<U>& b) { return a.Get() == b.Get(); }
template <typename T> bool operator==(const ComPtr<T>& a, std::nullptr_t) { return a.Get() == nullptr; }
template <typename T> bool operator!=(const ComPtr<T>& a, std::nullptr_t) { return a.Get() != nullptr; }
}} // namespace Microsoft::WRL
