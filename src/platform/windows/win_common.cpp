#if defined(_WIN32)

#include "platform/windows/win_common.hpp"
#include "core/common.hpp"

#include <cstdio>
#include <vector>

namespace br::win {
namespace {
template <class T>
T proc(HMODULE m, const char* name) {
    return reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(m, name)));
}
}

const Api& api() {
    static const Api a = [] {
        Api x;
        auto sys = [](const wchar_t* name) -> HMODULE {
            if (HMODULE m = LoadLibraryExW(name, nullptr, 0x00000800 /* Поиск библиотеки только в SYSTEM32. */)) return m;
            wchar_t path[MAX_PATH];
            const UINT n = GetSystemDirectoryW(path, MAX_PATH);
            if (!n || n + 2 + wcslen(name) >= MAX_PATH) return nullptr;
            path[n] = L'\\';
            wcscpy(path + n + 1, name);
            return LoadLibraryW(path);
        };
        if (HMODULE m = sys(L"d3d11.dll")) {
            x.D3D11CreateDevice = proc<PFN_D3D11_CREATE_DEVICE>(m, "D3D11CreateDevice");
            x.CreateDirect3D11DeviceFromDXGIDevice = proc<PFN_CreateDirect3D11DeviceFromDXGIDevice>(m, "CreateDirect3D11DeviceFromDXGIDevice");
        }
        if (HMODULE m = sys(L"dxgi.dll"))
            x.CreateDXGIFactory1 = proc<PFN_CreateDXGIFactory1>(m, "CreateDXGIFactory1");
        if (HMODULE m = sys(L"d3dcompiler_47.dll"))
            x.D3DCompile = proc<PFN_D3DCompile>(m, "D3DCompile");
        if (HMODULE m = sys(L"combase.dll")) {
            x.RoInitialize = proc<PFN_RoInitialize>(m, "RoInitialize");
            x.RoGetActivationFactory = proc<PFN_RoGetActivationFactory>(m, "RoGetActivationFactory");
            x.WindowsCreateString = proc<PFN_WindowsCreateString>(m, "WindowsCreateString");
            x.WindowsDeleteString = proc<PFN_WindowsDeleteString>(m, "WindowsDeleteString");
        }
        if (HMODULE m = sys(L"dwmapi.dll"))
            x.DwmGetWindowAttribute = proc<PFN_DwmGetWindowAttribute>(m, "DwmGetWindowAttribute");
        if (HMODULE m = sys(L"shcore.dll"))
            x.GetDpiForMonitor = proc<PFN_GetDpiForMonitor>(m, "GetDpiForMonitor");
        if (HMODULE m = GetModuleHandleW(L"user32.dll")) {
            x.SetThreadDpiAwarenessContext = proc<PFN_SetThreadDpiAwarenessContext>(m, "SetThreadDpiAwarenessContext");
            x.GetDpiForWindow = proc<PFN_GetDpiForWindow>(m, "GetDpiForWindow");
        }
        return x;
    }();
    return a;
}

DpiScope::DpiScope() {
    if (api().SetThreadDpiAwarenessContext)
        previous_ = api().SetThreadDpiAwarenessContext(reinterpret_cast<HANDLE>(static_cast<intptr_t>(-4))); // Режим DPI PER_MONITOR_AWARE_V2.
}

DpiScope::~DpiScope() {
    if (previous_ && api().SetThreadDpiAwarenessContext) api().SetThreadDpiAwarenessContext(previous_);
}

std::string utf8(const wchar_t* s, int len) {
    if (!s) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, len, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string r(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, len, r.data(), n, nullptr, nullptr);
    if (len < 0 && !r.empty() && r.back() == '\0') r.pop_back();
    return r;
}

std::wstring utf16(const char* s) {
    if (!s) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring r(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, r.data(), n);
    r.pop_back();
    return r;
}

br_status hr_fail(br_status status, const char* what, HRESULT hr) {
    set_errorf("%s failed (HRESULT 0x%08lx)", what, static_cast<unsigned long>(hr));
    return status;
}

RECT window_frame(HWND hwnd) {
    RECT r{};
    if (api().DwmGetWindowAttribute &&
        SUCCEEDED(api().DwmGetWindowAttribute(hwnd, 9 /* Расширенные границы рамки окна DWM. */, &r, sizeof(r))) &&
        r.right > r.left && r.bottom > r.top)
        return r;
    GetWindowRect(hwnd, &r);
    return r;
}

RECT client_rect_screen(HWND hwnd) {
    RECT r{};
    GetClientRect(hwnd, &r);
    POINT tl{r.left, r.top}, br{r.right, r.bottom};
    ClientToScreen(hwnd, &tl);
    ClientToScreen(hwnd, &br);
    return {tl.x, tl.y, br.x, br.y};
}

br_rect_i32 to_rect(const RECT& r) {
    return {static_cast<int32_t>(r.left), static_cast<int32_t>(r.top), static_cast<int32_t>(r.right - r.left),
            static_cast<int32_t>(r.bottom - r.top)};
}

namespace {
bool is_cloaked(HWND h) {
    int cloaked = 0;
    return api().DwmGetWindowAttribute &&
           SUCCEEDED(api().DwmGetWindowAttribute(h, 14 /* Проверяет, скрыто ли окно механизмом DWM. */, &cloaked, sizeof(cloaked))) && cloaked != 0;
}
}

bool window_is_unobstructed(HWND hwnd, const RECT& area) {
    if (!IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd) || is_cloaked(hwnd)) return false;
    if (area.right <= area.left || area.bottom <= area.top) return false;
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (area.left < vx || area.top < vy || area.right > vx + vw || area.bottom > vy + vh) return false;
    int budget = 1024; // Ограничивает обход окон по z-порядку.
    for (HWND w = GetWindow(hwnd, GW_HWNDPREV); w && budget-- > 0; w = GetWindow(w, GW_HWNDPREV)) {
        // GetWindowRect охватывает видимую рамку; до запросов DWM отбрасывает окна без пересечения.
        RECT gr, x;
        if (!GetWindowRect(w, &gr) || !IntersectRect(&x, &gr, &area)) continue;
        if (!IsWindowVisible(w) || IsIconic(w) || is_cloaked(w)) continue;
        const RECT r = window_frame(w);
        if (IntersectRect(&x, &r, &area)) return false;
    }
    return budget > 0;
}

namespace {
using PFN_TimeBeginPeriod = UINT(WINAPI*)(UINT);

struct ThreadTimer {
    HANDLE handle{};
    ThreadTimer() {
        // Таймер CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Windows 10 1803+); в старых системах недоступен.
        handle = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS);
    }
    ~ThreadTimer() {
        if (handle) CloseHandle(handle);
    }
};

void raise_timer_resolution() {
    static const bool once = [] {
        if (HMODULE m = LoadLibraryW(L"winmm.dll"))
            if (auto f = proc<PFN_TimeBeginPeriod>(m, "timeBeginPeriod")) f(1);
        return true;
    }();
    (void)once;
}
}

void precise_sleep_ms(uint32_t ms) {
    if (!ms) ms = 1;
    thread_local ThreadTimer timer;
    if (timer.handle) {
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(ms) * 10000; // Относительное время в единицах по 100 нс.
        if (SetWaitableTimer(timer.handle, &due, 0, nullptr, nullptr, FALSE) && WaitForSingleObject(timer.handle, ms + 50) == WAIT_OBJECT_0)
            return;
    }
    raise_timer_resolution();
    Sleep(ms);
}

br_status create_device(IDXGIAdapter* adapter, Com<ID3D11Device>& device, Com<ID3D11DeviceContext>& context) {
    if (!api().D3D11CreateDevice) return fail(BR_E_UNSUPPORTED, "d3d11.dll is not available");
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                               D3D_FEATURE_LEVEL_10_0};
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL got{};
    const D3D_DRIVER_TYPE type = adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
    HRESULT hr = api().D3D11CreateDevice(adapter, type, nullptr, flags, levels, 4, D3D11_SDK_VERSION, device.put(), &got, context.put());
    if (hr == E_INVALIDARG) // В средах до 11.1 уровень 11_1 не поддерживается.
        hr = api().D3D11CreateDevice(adapter, type, nullptr, flags, levels + 1, 3, D3D11_SDK_VERSION, device.put(), &got, context.put());
    if (FAILED(hr)) return hr_fail(BR_E_UNSUPPORTED, "D3D11CreateDevice", hr);
    return BR_OK;
}

}

#endif
