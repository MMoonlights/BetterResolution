#pragma once
#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <inspectable.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <br/br.h>

#include <string>
#include <utility>

namespace br::win {

// Указатель COM с управлением владением через AddRef/Release.
template <class T>
class Com {
public:
    Com() = default;
    Com(std::nullptr_t) {}
    explicit Com(T* p, bool add_ref = false) : p_(p) { if (p_ && add_ref) p_->AddRef(); }
    Com(const Com& o) : p_(o.p_) { if (p_) p_->AddRef(); }
    Com(Com&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    ~Com() { reset(); }
    Com& operator=(const Com& o) { if (this != &o) { if (o.p_) o.p_->AddRef(); reset(); p_ = o.p_; } return *this; }
    Com& operator=(Com&& o) noexcept { if (this != &o) { reset(); p_ = o.p_; o.p_ = nullptr; } return *this; }
    void reset() { if (p_) { p_->Release(); p_ = nullptr; } }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    T** put() { reset(); return &p_; }
    void** put_void() { reset(); return reinterpret_cast<void**>(&p_); }
    explicit operator bool() const { return p_ != nullptr; }
    T* detach() { T* p = p_; p_ = nullptr; return p; }
    template <class U>
    HRESULT as(REFIID iid, Com<U>& out) const { return p_ ? p_->QueryInterface(iid, out.put_void()) : E_POINTER; }

private:
    T* p_{};
};

using PFN_CreateDXGIFactory1 = HRESULT(WINAPI*)(REFIID, void**);
using PFN_CreateDirect3D11DeviceFromDXGIDevice = HRESULT(WINAPI*)(IDXGIDevice*, IInspectable**);
using PFN_D3DCompile = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
using PFN_RoInitialize = HRESULT(WINAPI*)(int);
using PFN_RoGetActivationFactory = HRESULT(WINAPI*)(HSTRING, REFIID, void**);
using PFN_WindowsCreateString = HRESULT(WINAPI*)(PCWSTR, UINT32, HSTRING*);
using PFN_WindowsDeleteString = HRESULT(WINAPI*)(HSTRING);
using PFN_DwmGetWindowAttribute = HRESULT(WINAPI*)(HWND, DWORD, PVOID, DWORD);
using PFN_GetDpiForMonitor = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
using PFN_SetThreadDpiAwarenessContext = HANDLE(WINAPI*)(HANDLE);
using PFN_GetDpiForWindow = UINT(WINAPI*)(HWND);

// Графические функции и функции DPI, адреса которых находятся во время выполнения.
struct Api {
    PFN_D3D11_CREATE_DEVICE D3D11CreateDevice{};
    PFN_CreateDXGIFactory1 CreateDXGIFactory1{};
    PFN_CreateDirect3D11DeviceFromDXGIDevice CreateDirect3D11DeviceFromDXGIDevice{};
    PFN_D3DCompile D3DCompile{};
    PFN_RoInitialize RoInitialize{};
    PFN_RoGetActivationFactory RoGetActivationFactory{};
    PFN_WindowsCreateString WindowsCreateString{};
    PFN_WindowsDeleteString WindowsDeleteString{};
    PFN_DwmGetWindowAttribute DwmGetWindowAttribute{};
    PFN_GetDpiForMonitor GetDpiForMonitor{};
    PFN_SetThreadDpiAwarenessContext SetThreadDpiAwarenessContext{};
    PFN_GetDpiForWindow GetDpiForWindow{};
};
const Api& api();

// На время жизни объекта включает для вызывающего потока режим DPI для каждого монитора (физические пиксели).
class DpiScope {
public:
    DpiScope();
    ~DpiScope();
    DpiScope(const DpiScope&) = delete;
    DpiScope& operator=(const DpiScope&) = delete;

private:
    HANDLE previous_{};
};

std::string utf8(const wchar_t* s, int len = -1);
std::wstring utf16(const char* s);
br_status hr_fail(br_status status, const char* what, HRESULT hr);

// Видимая рамка окна (расширенные границы DWM; при сбое используется GetWindowRect).
RECT window_frame(HWND hwnd);
RECT client_rect_screen(HWND hwnd);
br_rect_i32 to_rect(const RECT& r);

// Осторожно проверяет видимость и перекрытие в физических пикселях рабочего стола; при сомнении возвращает false.
bool window_is_unobstructed(HWND hwnd, const RECT& area);

// Ожидание с высокой точностью; при недоступности использует timeBeginPeriod(1)/Sleep.
void precise_sleep_ms(uint32_t ms);

// Создаёт аппаратное устройство D3D11 с поддержкой BGRA на `adapter` (nullptr - адаптер по умолчанию).
br_status create_device(IDXGIAdapter* adapter, Com<ID3D11Device>& device, Com<ID3D11DeviceContext>& context);

}

#endif
