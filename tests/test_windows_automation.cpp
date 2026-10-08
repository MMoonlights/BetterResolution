#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <br/br_automation_win.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <thread>
#include <sstream>
#include "automation/windows_wire.hpp"

namespace {
constexpr wchar_t prefix[] = L"BR_UIA_TEST_";
constexpr UINT get_counter = WM_APP + 1, delete_control = WM_APP + 2, get_blank = WM_APP + 3,
    hang_set = WM_APP + 4, hang_read = WM_APP + 5;
constexpr int edit_id = 101, invoke_id = 102, check_id = 103, disabled_id = 104, stale_id = 105, tabs_id = 106;
int counter = 0;
HWND blank_window = nullptr;
bool stall_set = false, stall_read = false;
WNDPROC original_edit = nullptr;
LRESULT CALLBACK edit_proc(HWND window, UINT msg, WPARAM w, LPARAM l) {
    if ((msg == WM_SETTEXT && stall_set) || (msg == WM_GETTEXT && stall_read)) {
        stall_set = stall_read = false;
        Sleep(2500);
    }
    return CallWindowProcW(original_edit, window, msg, w, l);
}
void require(bool yes, const char* what) { if (!yes) throw std::runtime_error(what); }
std::wstring object_name(HANDLE object) {
    DWORD bytes = 0;
    GetUserObjectInformationW(object, UOI_NAME, nullptr, 0, &bytes);
    require(bytes >= sizeof(wchar_t), "desktop name size");
    std::vector<wchar_t> name(bytes / sizeof(wchar_t) + 1);
    require(GetUserObjectInformationW(object, UOI_NAME, name.data(), bytes, &bytes) != FALSE, "desktop name");
    return name.data();
}
void private_only(const std::wstring& expected) {
    const auto current = object_name(GetThreadDesktop(GetCurrentThreadId()));
    require(current == expected && current.rfind(prefix, 0) == 0, "refusing non-test desktop");
    HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    require(input != nullptr, "cannot verify input desktop - refusing test");
    std::wstring active;
    try { active = object_name(input); } catch (...) { CloseDesktop(input); throw; }
    CloseDesktop(input);
    require(active != current, "refusing input desktop");
}
std::wstring executable() {
    std::vector<wchar_t> path(32768);
    const auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    require(size && size < path.size(), "executable path");
    return {path.data(), size};
}
struct Desktop {
    HDESK handle = nullptr;
    std::wstring name;
    Desktop() {
        name = std::wstring(prefix) + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
        SetLastError(0);
        handle = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, DESKTOP_CREATEWINDOW | DESKTOP_READOBJECTS |
            DESKTOP_WRITEOBJECTS | DESKTOP_ENUMERATE, nullptr);
        const auto error = GetLastError();
        if (!handle || error == ERROR_ALREADY_EXISTS) {
            if (handle) CloseDesktop(handle);
            handle = nullptr;
            throw std::runtime_error("create new private desktop");
        }
    }
    ~Desktop() { if (handle) CloseDesktop(handle); }
};
struct Child {
    HANDLE process = nullptr, output = nullptr;
    DWORD pid = 0;
    std::string text;
    Child() = default;
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    ~Child() {
        if (process) {
            if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
                TerminateProcess(process, 124);
                WaitForSingleObject(process, 5000);
            }
            CloseHandle(process);
        }
        if (output) CloseHandle(output);
    }
    void start(const std::wstring& desktop, const std::wstring& args) {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE writer = nullptr;
        require(CreatePipe(&output, &writer, &sa, 0) != FALSE, "create output pipe");
        SetHandleInformation(output, HANDLE_FLAG_INHERIT, 0);
        HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        if (input == INVALID_HANDLE_VALUE) { CloseHandle(writer); throw std::runtime_error("open child input"); }
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        std::vector<unsigned char> memory(size);
        auto attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(memory.data());
        if (!InitializeProcThreadAttributeList(attrs, 1, 0, &size)) {
            CloseHandle(input); CloseHandle(writer); throw std::runtime_error("child attributes");
        }
        HANDLE allowed[] = {writer, input};
        const BOOL updated = UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            allowed, sizeof(allowed), nullptr, nullptr);
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup);
        std::wstring desktop_path = L"winsta0\\" + desktop;
        startup.StartupInfo.lpDesktop = desktop_path.data();
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = input;
        startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = writer;
        startup.lpAttributeList = attrs;
        auto path = executable();
        std::wstring command = L"\"" + path + L"\" " + args;
        PROCESS_INFORMATION pi{};
        const BOOL started = updated && CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, TRUE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr, &startup.StartupInfo, &pi);
        DeleteProcThreadAttributeList(attrs); CloseHandle(writer); CloseHandle(input);
        require(started != FALSE, "start private-desktop child");
        process = pi.hProcess; pid = pi.dwProcessId; CloseHandle(pi.hThread);
    }
    void drain() {
        DWORD available = 0;
        while (PeekNamedPipe(output, nullptr, 0, nullptr, &available, nullptr) && available) {
            char data[4096]; DWORD read = 0;
            require(ReadFile(output, data, std::min<DWORD>(available, sizeof(data)), &read, nullptr) != FALSE, "read child output");
            text.append(data, read);
            require(text.size() <= 1024 * 1024, "child output limit");
        }
    }
    DWORD finish(DWORD timeout) {
        const auto start = GetTickCount64();
        while (WaitForSingleObject(process, 10) == WAIT_TIMEOUT) {
            drain();
            require(GetTickCount64() - start < timeout, "private test child timeout");
        }
        drain(); DWORD code = 0;
        require(GetExitCodeProcess(process, &code) != FALSE, "child exit status");
        return code;
    }
};
LRESULT CALLBACK fixture_proc(HWND window, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_COMMAND && HIWORD(w) == BN_CLICKED && LOWORD(w) == invoke_id) { ++counter; return 0; }
    if (msg == get_counter) return counter;
    if (msg == get_blank) return reinterpret_cast<LRESULT>(blank_window);
    if (msg == hang_set) { stall_set = true; return 1; }
    if (msg == hang_read) { stall_read = true; return 1; }
    if (msg == delete_control) return DestroyWindow(GetDlgItem(window, stale_id));
    if (msg == WM_CLOSE) { DestroyWindow(window); return 0; }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, msg, w, l);
}
LRESULT CALLBACK blank_proc(HWND window, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_PRINT || msg == WM_PRINTCLIENT) {
        RECT r{}; GetClientRect(window, &r); FillRect(reinterpret_cast<HDC>(w), &r, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH))); return 0;
    }
    if (msg == WM_PAINT) { PAINTSTRUCT p{}; auto dc = BeginPaint(window, &p); FillRect(dc, &p.rcPaint, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH))); EndPaint(window, &p); return 0; }
    return DefWindowProcW(window, msg, w, l);
}
int fixture(const std::wstring& desktop) {
    private_only(desktop);
    WNDCLASSW wc{}; wc.lpfnWndProc = fixture_proc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"BRPrivateUIAFixture";
    require(RegisterClassW(&wc) != 0, "register fixture window");
    WNDCLASSW black{}; black.lpfnWndProc = blank_proc; black.hInstance = wc.hInstance; black.lpszClassName = L"BRBlankCaptureFixture";
    require(RegisterClassW(&black) != 0, "blank fixture class");
    blank_window = CreateWindowExW(0, black.lpszClassName, L"", WS_POPUP, 700, 30, 180, 120, nullptr, nullptr, wc.hInstance, nullptr);
    require(blank_window != nullptr, "blank capture fixture"); ShowWindow(blank_window, SW_SHOWNOACTIVATE);
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"BR private UIA fixture", WS_OVERLAPPEDWINDOW,
        30, 30, 500, 330, nullptr, nullptr, wc.hInstance, nullptr);
    require(window != nullptr, "create fixture window");
    auto control = [&](const wchar_t* cls, const wchar_t* title, DWORD style, int id, int y) {
        HWND h = CreateWindowExW(0, cls, title, WS_CHILD | WS_VISIBLE | style, 20, y, 350, 28,
            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), wc.hInstance, nullptr);
        require(h != nullptr, "create fixture control"); return h;
    };
    auto edit = control(L"EDIT", L"initial", WS_BORDER | ES_AUTOHSCROLL, edit_id, 20);
    original_edit = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(edit_proc)));
    require(original_edit != nullptr, "edit subclass");
    control(L"BUTTON", L"Apply", BS_PUSHBUTTON, invoke_id, 65);
    control(L"BUTTON", L"Option", BS_AUTOCHECKBOX, check_id, 110);
    EnableWindow(control(L"BUTTON", L"Disabled", BS_PUSHBUTTON, disabled_id, 155), FALSE);
    control(L"BUTTON", L"Delete me", BS_PUSHBUTTON, stale_id, 200);
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_TAB_CLASSES};
    require(InitCommonControlsEx(&common) != FALSE, "tab classes");
    HWND tabs = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE, 20, 235, 350, 45,
        window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(tabs_id)), wc.hInstance, nullptr);
    require(tabs != nullptr, "tab fixture");
    const wchar_t* names[] = {L"Details", L"History", L"Duplicate", L"Duplicate"};
    for (int i = 0; i < 4; ++i) {
        TCITEMW item{}; item.mask = TCIF_TEXT; item.pszText = const_cast<wchar_t*>(names[i]);
        require(SendMessageW(tabs, TCM_INSERTITEMW, i, reinterpret_cast<LPARAM>(&item)) >= 0, "tab item");
    }
    ShowWindow(window, SW_SHOWNOACTIVATE); UpdateWindow(window);
    // HWND передаётся только родителю через отдельный канал вывода.
    std::printf("HWND %llu\n", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(window)));
    std::fflush(stdout);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    return 0;
}
br_auto_string str(const char* s) { return {s, std::strlen(s)}; }
using Snapshot = std::unique_ptr<br_auto_snapshot, decltype(&br_auto_snapshot_destroy)>;
using Session = std::unique_ptr<br_auto_session, decltype(&br_auto_session_destroy)>;
void status(br_auto_status actual, br_auto_status expected, const char* label) {
    if (actual != expected) throw std::runtime_error(std::string(label) + ": " + br_auto_status_string(actual));
}
Snapshot observe(br_auto_session* session, uint64_t since = 0) {
    br_auto_snapshot* result = nullptr;
    status(br_auto_observe(session, since, &result), BR_AUTO_OK, "observe");
    return Snapshot(result, br_auto_snapshot_destroy);
}
br_auto_element find(const Snapshot& snapshot, const char* role, const char* name = nullptr) {
    br_auto_selector selector{}; selector.role = str(role); if (name) selector.name = str(name);
    br_auto_element result{};
    status(br_auto_find(snapshot.get(), &selector, &result), BR_AUTO_OK, "find"); return result;
}
DWORD_PTR message(HWND window, UINT msg, WPARAM w = 0, LPARAM l = 0) {
    DWORD_PTR result = 0;
    require(SendMessageTimeoutW(window, msg, w, l, SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &result) != 0, "fixture message timeout");
    return result;
}
bool isolated_mode = false;
std::string utf8_path(const std::wstring& path) {
    const auto size = WideCharToMultiByte(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), nullptr, 0, nullptr, nullptr);
    require(size > 0, "UTF-8 helper path"); std::string out(size_t(size), '\0');
    require(WideCharToMultiByte(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), out.data(), size, nullptr, nullptr) != 0,
        "UTF-8 helper path conversion"); return out;
}
std::string helper() {
    const auto path = executable(); return utf8_path(path.substr(0, path.find_last_of(L"\\/")) + L"\\br-uia-helper.exe");
}
Session open(HWND window, bool actions) {
    auto native = br_auto_windows_options_default(reinterpret_cast<uintptr_t>(window));
    native.connection_timeout_ms = 2000; native.transaction_timeout_ms = 2000;
    auto options = br_auto_options_default(); options.allow_actions = actions ? 1u : 0u;
    options.default_timeout_ms = 3000;
    br_auto_session* session = nullptr;
    if (isolated_mode) {
        const auto path = helper(); auto target = br_auto_windows_isolated_options_default(native.window, str(path.c_str()));
        target.target = native; target.target.transaction_timeout_ms = 10000;
        status(br_auto_windows_open_isolated(&target, &options, &session), BR_AUTO_OK, "isolated UIA open");
        require(std::strcmp(br_auto_session_provider(session), "windows-uia-isolated") == 0, "isolated provider name");
    } else status(br_auto_windows_open(&native, &options, &session), BR_AUTO_OK, "UIA open");
    return Session(session, br_auto_session_destroy);
}
br_auto_action action(uint64_t id, uint32_t kind) {
    auto result = br_auto_action_default(kind); result.target.element_id = id; return result;
}
void timing(const char* name, std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const double median = values.size() % 2 ? values[values.size() / 2] :
        (values[values.size() / 2 - 1] + values[values.size() / 2]) / 2;
    const auto rank = [&](double q) { return values[size_t(std::ceil(q * double(values.size()))) - 1]; };
    std::printf("TIMING %s samples=%zu median_ms=%.3f p95_ms=%.3f p99_ms=%.3f min_ms=%.3f max_ms=%.3f\n",
        name, values.size(), median, rank(0.95), rank(0.99), values.front(), values.back());
}
int faulty_helper(char** argv) {
    namespace wire = br::automation::wire;
    private_only(object_name(GetThreadDesktop(GetCurrentThreadId())));
    const auto cap = static_cast<uint32_t>(std::stoul(argv[5]));
    if (cap != 4097 && cap != 4098) { Sleep(INFINITE); return 0; }
    auto handle = [](const char* s) { return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::stoull(s))); };
    const auto mapping = handle(argv[2]), request = handle(argv[3]), response = handle(argv[4]);
    auto memory = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, cap + 4);
    require(memory != nullptr, "fault helper inherited mapping");
    const auto pid = std::to_string(GetCurrentProcessId());
    const auto mask = BR_AUTO_ACTION_BIT(BR_AUTO_INVOKE) | BR_AUTO_ACTION_BIT(BR_AUTO_SET_VALUE) |
        BR_AUTO_ACTION_BIT(BR_AUTO_TOGGLE) | BR_AUTO_ACTION_BIT(BR_AUTO_SELECT) | BR_AUTO_ACTION_BIT(BR_AUTO_FOCUS) |
        BR_AUTO_ACTION_BIT(BR_AUTO_SELECT_NAMED);
    while (WaitForSingleObject(request, INFINITE) == WAIT_OBJECT_0) {
        if (cap == 4097) {
            std::memset(memory, 255, 4); SetEvent(response); Sleep(INFINITE); return 0;
        }
        const auto data = wire::load(memory, cap); wire::Reader r(data.data(), data.size());
        require(r.u32() == wire::version, "fault helper request version"); const auto seq = r.u64();
        const auto command = r.u32();
        if (command == wire::act) ExitProcess(123);
        wire::Writer out(cap); out.u32(wire::version); out.u64(seq); out.u32(BR_AUTO_OK); out.u32(BR_AUTO_NOT_DISPATCHED);
        out.u32(command == wire::observe ? 1 : 0); out.u64(mask); out.u32(command == wire::init ? 0 : 1);
        if (command != wire::init) {
            br_auto_node n{}; n.native_id = 1; n.incarnation = 1; n.role = str("button"); n.name = str("Synthetic helper");
            n.value = str(pid.c_str()); n.known = n.state = BR_AUTO_ENABLED | BR_AUTO_VISIBLE;
            n.actions = BR_AUTO_ACTION_BIT(BR_AUTO_INVOKE); out.node(n);
        }
        wire::store(memory, out); SetEvent(response);
    }
    UnmapViewOfFile(memory); return 3;
}
int client(const std::wstring& desktop, HWND window, DWORD fixture_pid) {
    private_only(desktop);
    DWORD actual_pid = 0;
    require(IsWindow(window) && GetWindowThreadProcessId(window, &actual_pid) && actual_pid == fixture_pid,
        "fixture identity mismatch");
    wchar_t classname[80]{};
    require(GetClassNameW(window, classname, 80) && std::wstring(classname) == L"BRPrivateUIAFixture", "fixture class mismatch");
    auto started = std::chrono::steady_clock::now();
    auto session = open(window, true);
    const double cold_open_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    std::printf("TIMING cold_open samples=1 elapsed_ms=%.3f\n", cold_open_ms); std::fflush(stdout);
    started = std::chrono::steady_clock::now();
    auto baseline = observe(session.get());
    const double first_observe_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    std::printf("TIMING first_observe samples=1 elapsed_ms=%.3f\n", first_observe_ms); std::fflush(stdout);
    const auto info = br_auto_snapshot_get_info(baseline.get());
    std::printf("OBSERVE complete=%u delta=%u nodes=%zu changes=%zu\n", info.complete, info.is_delta, info.element_count, info.change_count);
    std::fflush(stdout);
    require(info.complete && !info.is_delta && info.element_count >= 6, "complete fixture observation");
    if (std::strstr(br_features(), "gdi")) {
        auto capture = br_capture_options_default(); capture.target = BR_TARGET_WINDOW_FULL;
        capture.window = reinterpret_cast<uintptr_t>(window); capture.backend = BR_BACKEND_GDI_PRINT; capture.include_cursor = 0;
        br_image image{}; br_frame_info frame{};
        require(br_screenshot(&capture, &image, &frame) == BR_OK, "full PrintWindow capture");
        RECT outer{}; require(GetWindowRect(window, &outer), "fixture bounds");
        require(image.width == uint32_t(outer.right - outer.left) && image.height == uint32_t(outer.bottom - outer.top), "full capture exact geometry");
        require(frame.backend == BR_BACKEND_GDI_PRINT && frame.screen_rect.x == outer.left, "PrintWindow metadata");
        br_image_free(&image);
        capture.window = message(window, get_blank);
        require(br_screenshot(&capture, &image, &frame) == BR_E_UNSUPPORTED && !image.data, "explicit PrintWindow refuses blank without screen fallback");
    }
    {
        auto native = br_auto_windows_options_default(reinterpret_cast<uintptr_t>(window));
        native.max_depth = 0;
        br_auto_session* raw = nullptr;
        status(br_auto_windows_open(&native, nullptr, &raw), BR_AUTO_OK, "depth-limited open");
        Session limited(raw, br_auto_session_destroy); auto partial = observe(limited.get());
        require(!br_auto_snapshot_get_info(partial.get()).complete, "depth limit reports incomplete");
        br_auto_selector selector{}; selector.name = str("Apply"); br_auto_element unknown{};
        status(br_auto_find(partial.get(), &selector, &unknown), BR_AUTO_INCOMPLETE, "partial never proves absence");
        limited.reset(); native.max_depth = 32;
        auto limit_options = br_auto_options_default(); limit_options.max_text_bytes = 1;
        status(br_auto_windows_open(&native, &limit_options, &raw), BR_AUTO_OK, "text-limited open");
        Session text_limited(raw, br_auto_session_destroy); br_auto_snapshot* rejected = nullptr;
        status(br_auto_observe(text_limited.get(), 0, &rejected), BR_AUTO_LIMIT_EXCEEDED, "text budget rejects truncation");
        require(rejected == nullptr, "text-limited observation has no output");
    }
    const auto edit = find(baseline, "edit"), button = find(baseline, "button", "Apply");
    const auto checkbox = find(baseline, "checkbox", "Option"), disabled = find(baseline, "button", "Disabled");
    const auto removed = find(baseline, "button", "Delete me");
    const auto tabs = find(baseline, "tab");
    require((tabs.known & BR_AUTO_ADDRESSABLE) && (tabs.state & BR_AUTO_ADDRESSABLE), "tab stable identity");
    auto named = action(tabs.id, BR_AUTO_SELECT_NAMED); named.value = str("History");
    br_auto_action_result named_result{};
    status(br_auto_act(session.get(), &named, &named_result), BR_AUTO_OK, "named tab transition");
    require(named_result.effect == BR_AUTO_DISPATCHED && named_result.verification == BR_AUTO_SATISFIED &&
        message(GetDlgItem(window, tabs_id), TCM_GETCURSEL) == 1, "independent named tab state");
    named.value = str("Duplicate");
    status(br_auto_act(session.get(), &named, &named_result), BR_AUTO_AMBIGUOUS, "named duplicate refuses");
    require(named_result.effect == BR_AUTO_NOT_DISPATCHED && message(GetDlgItem(window, tabs_id), TCM_GETCURSEL) == 1,
        "ambiguous selection did not dispatch");
    named.value = str("Missing");
    status(br_auto_act(session.get(), &named, &named_result), BR_AUTO_NOT_FOUND, "named missing refuses");
    require(named_result.effect == BR_AUTO_NOT_DISPATCHED, "missing selection did not dispatch");
    named.value = {};
    status(br_auto_act(session.get(), &named, &named_result), BR_AUTO_INVALID_ARGUMENT, "named empty refuses");
    named.value = str("Details");
    status(br_auto_act(session.get(), &named, &named_result), BR_AUTO_OK, "named tab restore");
    require(message(GetDlgItem(window, tabs_id), TCM_GETCURSEL) == 0, "independent tab restore");
    br_auto_selector missing{}; missing.name = str("No such fixture element");
    br_auto_element found{};
    status(br_auto_find(baseline.get(), &missing, &found), BR_AUTO_NOT_FOUND, "missing selector");
    br_auto_selector ambiguous{}; ambiguous.role = str("button");
    status(br_auto_find(baseline.get(), &ambiguous, &found), BR_AUTO_AMBIGUOUS, "ambiguous selector");
    require((edit.actions & BR_AUTO_ACTION_BIT(BR_AUTO_SET_VALUE)) != 0, "edit value capability");
    require((button.actions & BR_AUTO_ACTION_BIT(BR_AUTO_INVOKE)) != 0, "invoke capability");
    require((disabled.known & BR_AUTO_ENABLED) && !(disabled.state & BR_AUTO_ENABLED), "disabled state");
    auto unchanged = observe(session.get(), info.id);
    const auto unchanged_info = br_auto_snapshot_get_info(unchanged.get());
    require(unchanged_info.is_delta && unchanged_info.base_id == info.id && unchanged_info.change_count == 0, "unchanged delta baseline");
    auto set = action(edit.id, BR_AUTO_SET_VALUE);
    const char unicode[] = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xF0\x9F\x8C\x99";
    set.value = str(unicode); br_auto_action_result result{};
    status(br_auto_act(session.get(), &set, &result), BR_AUTO_OK, "unicode set");
    require(result.effect == BR_AUTO_DISPATCHED && result.verification == BR_AUTO_SATISFIED, "set verified");
    wchar_t native_text[128]{};
    message(GetDlgItem(window, edit_id), WM_GETTEXT, 128, reinterpret_cast<LPARAM>(native_text));
    require(std::wstring(native_text) == L"\u041F\u0440\u0438\u0432\u0435\u0442 \U0001F319", "independent Unicode value");
    auto changed = observe(session.get(), unchanged_info.id);
    const auto changed_info = br_auto_snapshot_get_info(changed.get());
    require(changed_info.is_delta && changed_info.base_id == unchanged_info.id, "set preserves delta baseline");
    bool saw_update = false;
    for (size_t i = 0; i < changed_info.change_count; ++i) {
        br_auto_change c{}; status(br_auto_snapshot_change(changed.get(), i, &c), BR_AUTO_OK, "delta getter");
        if (c.kind == BR_AUTO_UPDATED && c.element.id == edit.id && c.element.value.size == sizeof(unicode) - 1 &&
            std::memcmp(c.element.value.data, unicode, sizeof(unicode) - 1) == 0) saw_update = true;
    }
    require(saw_update, "Unicode delta update");
    auto invoke = action(button.id, BR_AUTO_INVOKE);
    const auto before = message(window, get_counter);
    status(br_auto_act(session.get(), &invoke, &result), BR_AUTO_OK, "invoke");
    require(result.effect == BR_AUTO_DISPATCHED, "invoke dispatched");
    const auto deadline = GetTickCount64() + 3000;
    while (message(window, get_counter) == before && GetTickCount64() < deadline) Sleep(10);
    require(message(window, get_counter) == before + 1, "independent WM_COMMAND counter");
    auto toggle = action(checkbox.id, BR_AUTO_TOGGLE);
    br_auto_condition toggled{}; toggled.target.element_id = checkbox.id;
    toggled.property = BR_AUTO_IS_TOGGLED; toggled.expected = 1; toggle.after = &toggled;
    status(br_auto_act(session.get(), &toggle, &result), BR_AUTO_OK, "toggle");
    require(result.verification == BR_AUTO_SATISFIED && message(GetDlgItem(window, check_id), BM_GETCHECK) == BST_CHECKED,
        "independent checkbox state");
    auto denied = action(disabled.id, BR_AUTO_INVOKE);
    status(br_auto_act(session.get(), &denied, &result), BR_AUTO_PRECONDITION_FAILED, "disabled action");
    require(result.effect == BR_AUTO_NOT_DISPATCHED, "disabled not dispatched");
    auto read_only = open(window, false); auto ro_snapshot = observe(read_only.get());
    auto ro_button = find(ro_snapshot, "button", "Apply"); auto ro_action = action(ro_button.id, BR_AUTO_INVOKE);
    const auto ro_before = message(window, get_counter);
    status(br_auto_act(read_only.get(), &ro_action, &result), BR_AUTO_DENIED, "read-only denies");
    require(result.effect == BR_AUTO_NOT_DISPATCHED && message(window, get_counter) == ro_before, "read-only no mutation");
    const char* batch_values[] = {"batch 0", "batch 1", "batch 2", "batch 3", "batch 4"};
    br_auto_step batch_steps[10]{};
    for (size_t i = 0; i < 5; ++i) {
        batch_steps[i * 2].kind = BR_AUTO_STEP_ACT;
        batch_steps[i * 2].action = action(edit.id, BR_AUTO_SET_VALUE);
        batch_steps[i * 2].action.value = str(batch_values[i]);
        batch_steps[i * 2 + 1].kind = BR_AUTO_STEP_ACT;
        batch_steps[i * 2 + 1].action = invoke;
    }
    br_auto_action_result batch_results[10]{}; br_auto_batch_result batch_result{};
    std::vector<double> batch_times, completed_times;
    for (int sample = 0; sample < 20; ++sample) {
        const auto batch_before = message(window, get_counter);
        started = std::chrono::steady_clock::now();
        status(br_auto_batch(session.get(), batch_steps, 10, 15000, batch_results, 10, &batch_result), BR_AUTO_OK, "batch");
        const double batch_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        require(batch_result.completed_steps == 10 && batch_result.failed_index == SIZE_MAX, "batch completes all steps");
        for (size_t i = 0; i < 10; ++i) {
            require(batch_results[i].status == BR_AUTO_OK && batch_results[i].effect == BR_AUTO_DISPATCHED, "batch action dispatch");
            if (i % 2 == 0) require(batch_results[i].verification == BR_AUTO_SATISFIED, "batch set verified");
        }
        const auto batch_deadline = GetTickCount64() + 3000;
        while (message(window, get_counter) < batch_before + 5 && GetTickCount64() < batch_deadline) Sleep(10);
        require(message(window, get_counter) == batch_before + 5, "independent batch invoke count");
        message(GetDlgItem(window, edit_id), WM_GETTEXT, 128, reinterpret_cast<LPARAM>(native_text));
        require(std::wstring(native_text) == L"batch 4", "independent final batch value");
        batch_times.push_back(batch_ms);
        completed_times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
    }
    timing("batch_10_actions", batch_times);
    timing("batch_10_verified_end_to_end", completed_times);
    require(message(window, delete_control) != 0, "delete stale fixture control");
    auto stale = action(removed.id, BR_AUTO_INVOKE);
    status(br_auto_act(session.get(), &stale, &result), BR_AUTO_STALE, "deleted element stale");
    require(result.effect == BR_AUTO_NOT_DISPATCHED, "stale not dispatched");
    auto deletion = observe(session.get(), changed_info.id);
    const auto deletion_info = br_auto_snapshot_get_info(deletion.get());
    require(deletion_info.is_delta && deletion_info.base_id == changed_info.id, "deletion delta baseline");
    bool saw_removal = false;
    for (size_t i = 0; i < deletion_info.change_count; ++i) {
        br_auto_change c{}; status(br_auto_snapshot_change(deletion.get(), i, &c), BR_AUTO_OK, "removal getter");
        saw_removal |= c.kind == BR_AUTO_REMOVED && c.element.id == removed.id;
    }
    require(saw_removal, "deleted element removal");
    std::vector<double> observe_times, set_times;
    for (int i = 0; i < 15; ++i) {
        auto start = std::chrono::steady_clock::now(); auto sample = observe(session.get());
        observe_times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        set.value = str(i % 2 ? "value A" : "value B"); start = std::chrono::steady_clock::now();
        status(br_auto_act(session.get(), &set, &result), BR_AUTO_OK, "timed verified set");
        require(result.verification == BR_AUTO_SATISFIED, "timed set verified");
        set_times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    timing("full_observe", observe_times); timing("set_and_verify", set_times);
    if (isolated_mode) {
        const auto path = helper(); auto isolated = br_auto_windows_isolated_options_default(reinterpret_cast<uintptr_t>(window), str(path.c_str()));
        auto options = br_auto_options_default(); options.allow_actions = 1; options.default_timeout_ms = 3000;
        br_auto_session* rejected = nullptr;
        isolated.helper_path = str("br-uia-helper.exe");
        status(br_auto_windows_open_isolated(&isolated, &options, &rejected), BR_AUTO_INVALID_ARGUMENT, "relative helper refused");
        require(!rejected, "failed open clears output");
        const auto self = utf8_path(executable()); isolated.helper_path = str(self.c_str()); isolated.startup_timeout_ms = 100;
        started = std::chrono::steady_clock::now();
        status(br_auto_windows_open_isolated(&isolated, &options, &rejected), BR_AUTO_TIMEOUT, "stalled helper startup deadline");
        require(!rejected && std::chrono::steady_clock::now() - started < std::chrono::seconds(2), "startup bounded");
        isolated.startup_timeout_ms = 3000; isolated.max_ipc_bytes = 4097;
        status(br_auto_windows_open_isolated(&isolated, &options, &rejected), BR_AUTO_PROVIDER_ERROR, "malformed helper response refused");
        require(!rejected, "corrupt response has no session");
        isolated.max_ipc_bytes = 4098;
        status(br_auto_windows_open_isolated(&isolated, &options, &rejected), BR_AUTO_OK, "synthetic crash helper open");
        Session crashing(rejected, br_auto_session_destroy); auto fake_snapshot = observe(crashing.get());
        const auto fake_button = find(fake_snapshot, "button", "Synthetic helper");
        const auto fake_pid = static_cast<DWORD>(std::stoul(std::string(fake_button.value.data, fake_button.value.size)));
        HANDLE fake_process = OpenProcess(SYNCHRONIZE, FALSE, fake_pid);
        require(fake_process != nullptr, "retain own fault helper identity");
        auto fake_act = action(fake_button.id, BR_AUTO_INVOKE);
        status(br_auto_act(crashing.get(), &fake_act, &result), BR_AUTO_PROVIDER_ERROR, "helper exits during dispatch");
        require(result.effect == BR_AUTO_EFFECT_UNKNOWN, "crashed dispatch outcome unknown");
        crashing.reset();
        const auto fake_exit = WaitForSingleObject(fake_process, 3000); CloseHandle(fake_process);
        require(fake_exit == WAIT_OBJECT_0, "own helper has no orphan process");
        isolated.helper_path = str(path.c_str()); isolated.max_ipc_bytes = 4096;
        status(br_auto_windows_open_isolated(&isolated, &options, &rejected), BR_AUTO_OK, "small transport open");
        Session bounded(rejected, br_auto_session_destroy); auto bounded_snapshot = observe(bounded.get());
        const auto bounded_edit = find(bounded_snapshot, "edit");
        auto large_action = action(bounded_edit.id, BR_AUTO_SET_VALUE);
        std::string too_large(4100, 'x'); large_action.value = str(too_large.c_str());
        status(br_auto_act(bounded.get(), &large_action, &result), BR_AUTO_LIMIT_EXCEEDED, "request payload bounded");
        require(result.effect == BR_AUTO_NOT_DISPATCHED, "oversized request not sent");
        auto after_refusal = observe(bounded.get());
        std::string large_value(3800, 'y'); large_action.value = str(large_value.c_str());
        status(br_auto_act(bounded.get(), &large_action, &result), BR_AUTO_OK, "single large value fits");
        br_auto_snapshot* no_partial = nullptr;
        status(br_auto_observe(bounded.get(), 0, &no_partial), BR_AUTO_LIMIT_EXCEEDED, "response payload bounded");
        require(!no_partial, "oversized response not published partially");
        large_action.value = str("restored");
        status(br_auto_act(bounded.get(), &large_action, &result), BR_AUTO_OK, "payload refusal preserves transport sequence");
        auto restored = observe(bounded.get()); bounded.reset();
        isolated.max_ipc_bytes = 8u * 1024u * 1024u;
        unsigned approval_calls = 0;
        options.authorize_user = &approval_calls;
        options.authorize = [](void* u, const br_auto_action*, const br_auto_element*) {
            ++*static_cast<unsigned*>(u); return BR_AUTO_DENIED;
        };
        status(br_auto_windows_open_isolated(&isolated, &options, &rejected), BR_AUTO_OK, "host authorize hook open");
        Session gated(rejected, br_auto_session_destroy); auto gated_snapshot = observe(gated.get());
        auto gated_action = action(find(gated_snapshot, "button", "Apply").id, BR_AUTO_INVOKE);
        const auto gated_count = message(window, get_counter);
        status(br_auto_act(gated.get(), &gated_action, &result), BR_AUTO_DENIED, "host gate denies before dispatch");
        require(approval_calls == 1 && result.effect == BR_AUTO_NOT_DISPATCHED && message(window, get_counter) == gated_count,
            "helper cannot bypass host gate"); gated.reset();
        auto doomed = open(window, true); auto before_hang = observe(doomed.get());
        auto hanging = action(find(before_hang, "edit").id, BR_AUTO_SET_VALUE); hanging.value = str("delayed own fixture"); hanging.timeout_ms = 250;
        require(message(window, hang_set) == 1, "arm own fixture SetValue stall");
        started = std::chrono::steady_clock::now();
        status(br_auto_act(doomed.get(), &hanging, &result), BR_AUTO_TIMEOUT, "foreign COM timeout");
        const auto timeout_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        require(timeout_ms < 1500 && result.effect == BR_AUTO_EFFECT_UNKNOWN, "timed out dispatch has unknown outcome");
        br_auto_snapshot* failed = nullptr;
        status(br_auto_observe(doomed.get(), 0, &failed), BR_AUTO_PROVIDER_ERROR, "failed helper cannot restart silently");
        require(!failed, "dead helper publishes no snapshot");
        started = std::chrono::steady_clock::now(); doomed.reset();
        const auto destroy_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        require(destroy_ms < 500, "destroy does not join hung COM");
        Sleep(2600); require(message(window, get_counter) > 0, "own fixture recovered");
        auto cancellable = open(window, true); auto current = observe(cancellable.get());
        const auto old_id = find(before_hang, "edit").id;
        auto old_action = action(old_id, BR_AUTO_SET_VALUE); old_action.value = str("old session id");
        status(br_auto_act(cancellable.get(), &old_action, &result), BR_AUTO_STALE, "old IDs cannot cross recovery");
        require(result.effect == BR_AUTO_NOT_DISPATCHED, "old ID not dispatched");
        require(message(window, hang_read) == 1, "arm own fixture read stall");
        std::thread cancel([&] { Sleep(100); br_auto_cancel(cancellable.get()); });
        started = std::chrono::steady_clock::now();
        const auto cancelled = br_auto_observe(cancellable.get(), 0, &failed); cancel.join();
        status(cancelled, BR_AUTO_CANCELLED, "foreign COM cancellation");
        require(!failed && std::chrono::steady_clock::now() - started < std::chrono::milliseconds(1500), "cancel bounded");
        status(br_auto_reset_cancel(cancellable.get()), BR_AUTO_OK, "cancel reset");
        status(br_auto_observe(cancellable.get(), 0, &failed), BR_AUTO_PROVIDER_ERROR, "cancelled helper remains failed");
        cancellable.reset(); Sleep(2600);
        std::printf("ISOLATION timeout_ms=%.3f destroy_ms=%.3f checks=startup,COM_timeout,cancel,unknown_effect,no_restart,old_ids,crash,corrupt_response,payload_limits,no_orphan,host_authorize\n", timeout_ms, destroy_ms);
    }
    std::printf("PASS private_desktop=%ls nodes=%zu invoke_count=%llu verified_sets=116 checks=observe,find,unicode,invoke,toggle,disabled,read_only,batch,delta,stale\n",
        desktop.c_str(), info.element_count, static_cast<unsigned long long>(message(window, get_counter)));
    read_only.reset(); session.reset();
    require(PostMessageW(window, WM_CLOSE, 0, 0) != FALSE, "close own fixture on private desktop");
    return 0;
}
int parent() {
    require(object_name(GetProcessWindowStation()) == L"WinSta0", "test requires interactive window station");
#ifdef BR_TEST_UIA_HELPER
    constexpr int modes = 2;
#else
    constexpr int modes = 1;
#endif
    for (int mode = 0; mode < modes; ++mode) {
        Desktop desktop;
        Child host; host.start(desktop.name, L"--fixture " + desktop.name);
        const auto deadline = GetTickCount64() + 15000;
        while (host.text.find('\n') == std::string::npos) {
            host.drain();
            require(WaitForSingleObject(host.process, 0) == WAIT_TIMEOUT, "fixture exited before handshake");
            require(GetTickCount64() < deadline, "fixture handshake timeout"); Sleep(10);
        }
        uint64_t handle = 0; std::string tag; std::istringstream handshake(host.text);
        require(static_cast<bool>(handshake >> tag >> handle) && tag == "HWND" && handle, "fixture HWND handshake");
        Child test; test.start(desktop.name, std::wstring(mode ? L"--client-isolated " : L"--client ") +
            desktop.name + L" " + std::to_wstring(handle) + L" " + std::to_wstring(host.pid));
        const auto code = test.finish(60000); std::cout << test.text << std::flush;
        require(code == 0, "UIA integration client failed");
        require(host.finish(5000) == 0, "fixture cleanup");
    }
    return 0;
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 1) return parent();
        // Имена созданного рабочего стола состоят только из ASCII.
        auto wide = [](const char* text) { std::string s(text); return std::wstring(s.begin(), s.end()); };
        if (argc == 3 && std::strcmp(argv[1], "--fixture") == 0) return fixture(wide(argv[2]));
        if (argc == 6 && std::strcmp(argv[1], "--br-uia") == 0) {
            return faulty_helper(argv);
        }
        if (argc == 5 && (!std::strcmp(argv[1], "--client") || !std::strcmp(argv[1], "--client-isolated"))) {
            isolated_mode = !std::strcmp(argv[1], "--client-isolated");
            const auto handle = std::stoull(argv[3]), pid = std::stoull(argv[4]);
            require(pid <= MAXDWORD, "fixture PID range");
            return client(wide(argv[2]), reinterpret_cast<HWND>(static_cast<uintptr_t>(handle)), static_cast<DWORD>(pid));
        }
        throw std::runtime_error("invalid private test arguments");
    } catch (const std::exception& e) { std::fprintf(stderr, "FAIL %s\n", e.what()); return 1; }
}
