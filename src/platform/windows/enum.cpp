#if defined(_WIN32)

#include "platform/windows/enum.hpp"
#include "core/common.hpp"

#include <algorithm>
#include <cstring>

namespace br::win {
namespace {

BOOL CALLBACK monitor_proc(HMONITOR hmon, HDC, LPRECT, LPARAM lp) {
    auto* out = reinterpret_cast<std::vector<MonitorEntry>*>(lp);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hmon, &mi)) return TRUE;
    MonitorEntry e{};
    e.handle = hmon;
    e.device = mi.szDevice;
    br_monitor_info& m = e.info;
    m.bounds = to_rect(mi.rcMonitor);
    m.work_area = to_rect(mi.rcWork);
    m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) ? 1 : 0;
    UINT dx = 96, dy = 96;
    if (api().GetDpiForMonitor && SUCCEEDED(api().GetDpiForMonitor(hmon, 0, &dx, &dy))) m.dpi = dx;
    else m.dpi = 96;
    m.scale = static_cast<float>(m.dpi) / 96.0f;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) m.rotation = static_cast<int32_t>(dm.dmDisplayOrientation) * 90;
    const std::string name = utf8(mi.szDevice);
    std::snprintf(m.name, sizeof(m.name), "%s", name.c_str());
    m.adapter_index = m.output_index = UINT32_MAX;
    out->push_back(e);
    return TRUE;
}

struct WinCtx {
    std::vector<br_window_info>* out;
    HWND foreground;
};

BOOL CALLBACK window_proc(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<WinCtx*>(lp);
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (api().DwmGetWindowAttribute) {
        int cloaked = 0;
        if (SUCCEEDED(api().DwmGetWindowAttribute(hwnd, 14 /* DWMWA_CLOAKED */, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;
    }
    const RECT fr = window_frame(hwnd);
    if (fr.right - fr.left <= 0 || fr.bottom - fr.top <= 0) return TRUE;
    wchar_t title[256];
    const int tl = GetWindowTextW(hwnd, title, 256);
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (tl <= 0 && (ex & WS_EX_TOOLWINDOW)) return TRUE;
    br_window_info w{};
    w.handle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(hwnd));
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    w.process_id = pid;
    w.bounds = to_rect(fr);
    w.client = to_rect(client_rect_screen(hwnd));
    w.dpi = api().GetDpiForWindow ? api().GetDpiForWindow(hwnd) : 96;
    w.visible = 1;
    w.minimized = IsIconic(hwnd) ? 1 : 0;
    w.maximized = IsZoomed(hwnd) ? 1 : 0;
    w.foreground = hwnd == ctx->foreground ? 1 : 0;
    const std::string t = utf8(title, tl > 0 ? tl : 0);
    std::snprintf(w.title, sizeof(w.title), "%s", t.c_str());
    wchar_t cls[128];
    const int cl = GetClassNameW(hwnd, cls, 128);
    const std::string c = utf8(cls, cl > 0 ? cl : 0);
    std::snprintf(w.class_name, sizeof(w.class_name), "%s", c.c_str());
    ctx->out->push_back(w);
    return TRUE;
}

}

br_status enum_monitors(std::vector<MonitorEntry>& out) {
    DpiScope dpi;
    out.clear();
    EnumDisplayMonitors(nullptr, nullptr, monitor_proc, reinterpret_cast<LPARAM>(&out));
    // Сопоставляет индексы адаптера и выхода DXGI по имени устройства GDI.
    if (api().CreateDXGIFactory1) {
        Com<IDXGIFactory1> factory;
        if (SUCCEEDED(api().CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.put_void()))) {
            Com<IDXGIAdapter1> adapter;
            for (UINT a = 0; factory->EnumAdapters1(a, adapter.put()) != DXGI_ERROR_NOT_FOUND; ++a) {
                Com<IDXGIOutput> output;
                for (UINT o = 0; adapter->EnumOutputs(o, output.put()) != DXGI_ERROR_NOT_FOUND; ++o) {
                    DXGI_OUTPUT_DESC d{};
                    if (FAILED(output->GetDesc(&d))) continue;
                    for (MonitorEntry& e : out) {
                        if (e.info.adapter_index == UINT32_MAX && e.device == d.DeviceName) {
                            e.info.adapter_index = a;
                            e.info.output_index = o;
                        }
                    }
                }
            }
        }
    }
    // Сначала основной монитор, затем слева направо и сверху вниз.
    std::stable_sort(out.begin(), out.end(), [](const MonitorEntry& a, const MonitorEntry& b) {
        if (a.info.primary != b.info.primary) return a.info.primary > b.info.primary;
        if (a.info.bounds.x != b.info.bounds.x) return a.info.bounds.x < b.info.bounds.x;
        return a.info.bounds.y < b.info.bounds.y;
    });
    for (size_t i = 0; i < out.size(); ++i) out[i].info.index = static_cast<uint32_t>(i);
    if (out.empty()) return fail(BR_E_NOT_FOUND, "no monitors found");
    return BR_OK;
}

br_status enum_windows(std::vector<br_window_info>& out) {
    DpiScope dpi;
    out.clear();
    WinCtx ctx{&out, GetForegroundWindow()};
    EnumWindows(window_proc, reinterpret_cast<LPARAM>(&ctx));
    return BR_OK;
}

br_rect_i32 virtual_desktop() {
    DpiScope dpi;
    return {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), GetSystemMetrics(SM_CXVIRTUALSCREEN),
            GetSystemMetrics(SM_CYVIRTUALSCREEN)};
}

}

#endif
