#include <br/br_automation_win.h>
#include "windows_provider.hpp"
#include "windows_wire.hpp"

#if defined(_WIN32) && defined(BR_HAS_WINDOWS_AUTOMATION)
#include <windows.h>
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

namespace {
using namespace br::automation;
constexpr uint64_t supported_actions = BR_AUTO_ACTION_BIT(BR_AUTO_INVOKE) | BR_AUTO_ACTION_BIT(BR_AUTO_SET_VALUE) |
    BR_AUTO_ACTION_BIT(BR_AUTO_TOGGLE) | BR_AUTO_ACTION_BIT(BR_AUTO_SELECT) | BR_AUTO_ACTION_BIT(BR_AUTO_FOCUS) |
    BR_AUTO_ACTION_BIT(BR_AUTO_SELECT_NAMED);
struct Handle {
    HANDLE h = nullptr;
    Handle() = default;
    explicit Handle(HANDLE value) : h(value) {}
    ~Handle() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
struct Mapping {
    void* p = nullptr;
    ~Mapping() { if (p) UnmapViewOfFile(p); }
};
struct Attributes {
    std::vector<unsigned char> bytes;
    LPPROC_THREAD_ATTRIBUTE_LIST list = nullptr;
    ~Attributes() { if (list) DeleteProcThreadAttributeList(list); }
    bool init(HANDLE* handles, size_t count) {
        SIZE_T size = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        bytes.resize(size); auto p = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(bytes.data());
        if (!InitializeProcThreadAttributeList(p, 1, 0, &size)) return false;
        list = p;
        return UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            handles, count * sizeof(HANDLE), nullptr, nullptr) != FALSE;
    }
};
std::wstring object_name(HANDLE object) {
    DWORD n = 0; GetUserObjectInformationW(object, UOI_NAME, nullptr, 0, &n);
    if (!n || n > 65536) throw wire::Error{BR_AUTO_PROVIDER_ERROR};
    std::vector<wchar_t> value(n / sizeof(wchar_t) + 1);
    if (!GetUserObjectInformationW(object, UOI_NAME, value.data(), n, &n)) throw wire::Error{BR_AUTO_PROVIDER_ERROR};
    return value.data();
}
std::wstring helper_path(br_auto_string s) {
    if (!s.data || !s.size || s.size > 32767 || std::memchr(s.data, '\0', s.size))
        throw wire::Error{BR_AUTO_INVALID_ARGUMENT};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data, static_cast<int>(s.size), nullptr, 0);
    if (!n) throw wire::Error{BR_AUTO_INVALID_ARGUMENT};
    std::wstring path(size_t(n), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data, static_cast<int>(s.size), path.data(), n))
        throw wire::Error{BR_AUTO_INVALID_ARGUMENT};
    const bool drive = path.size() >= 3 && ((path[0] >= L'A' && path[0] <= L'Z') ||
        (path[0] >= L'a' && path[0] <= L'z')) && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
    const bool unc = path.size() > 4 && path[0] == L'\\' && path[1] == L'\\';
    if ((!drive && !unc) || path.find(L'"') != std::wstring::npos) throw wire::Error{BR_AUTO_INVALID_ARGUMENT};
    return path;
}
struct Reply {
    br_auto_status status = BR_AUTO_PROVIDER_ERROR;
    uint32_t effect = BR_AUTO_NOT_DISPATCHED, complete = 0;
    std::vector<unsigned char> bytes;
    std::vector<br_auto_node> nodes;
};
template<class F> br_auto_status ipc_call(F&& fn) {
    try { return fn(); }
    catch (const wire::Error& e) { return e.status; }
}
class Isolated {
    Handle mapping_, request_, response_, job_, process_;
    Mapping view_;
    uint32_t cap_;
    uint64_t sequence_ = 0;
    bool broken_ = false;
public:
    explicit Isolated(uint32_t cap) : cap_(cap) {}
    ~Isolated() { stop(); }
    void stop() noexcept {
        broken_ = true;
        if (job_.h) TerminateJobObject(job_.h, 124);
        if (process_.h && WaitForSingleObject(process_.h, 0) == WAIT_TIMEOUT) TerminateProcess(process_.h, 124);
    }
    wire::Writer request(uint32_t command, uint32_t timeout) {
        if (sequence_ == UINT64_MAX) throw wire::Error{BR_AUTO_LIMIT_EXCEEDED};
        wire::Writer w(cap_); w.u32(wire::version); w.u64(sequence_ + 1); w.u32(command); w.u32(timeout); return w;
    }
    br_auto_status transact(const wire::Writer& w, const br_auto_operation* op, uint32_t startup_ms, Reply& reply, uint32_t* effect = nullptr) {
        if (broken_) return BR_AUTO_PROVIDER_ERROR;
        auto st = op ? br_auto_operation_status(op) : BR_AUTO_OK;
        if (st != BR_AUTO_OK) return st;
        try {
            ResetEvent(response_.h); wire::store(view_.p, w); MemoryBarrier();
            if (!SetEvent(request_.h)) { stop(); return BR_AUTO_PROVIDER_ERROR; }
            ++sequence_;
            // После отправки исход действия неизвестен до получения корректного ответа.
            if (effect) *effect = BR_AUTO_EFFECT_UNKNOWN;
            const auto deadline = GetTickCount64() + startup_ms;
            HANDLE events[] = {process_.h, response_.h};
            for (;;) {
                st = op ? br_auto_operation_status(op) : (GetTickCount64() >= deadline ? BR_AUTO_TIMEOUT : BR_AUTO_OK);
                if (st != BR_AUTO_OK) { stop(); return st; }
                const auto remaining = op ? br_auto_operation_remaining_ms(op) :
                    static_cast<uint32_t>(std::min<ULONGLONG>(UINT32_MAX, deadline - GetTickCount64()));
                const auto result = WaitForMultipleObjects(2, events, FALSE, std::min(remaining, 10u));
                if (result == WAIT_TIMEOUT) continue;
                if (result != WAIT_OBJECT_0 + 1) { stop(); return BR_AUTO_PROVIDER_ERROR; }
                MemoryBarrier(); reply.bytes = wire::load(view_.p, cap_);
                wire::Reader r(reply.bytes.data(), reply.bytes.size());
                if (r.u32() != wire::version || r.u64() != sequence_) throw wire::Error{BR_AUTO_PROVIDER_ERROR};
                const auto code = r.u32(); reply.effect = r.u32(); reply.complete = r.u32();
                const auto actions = r.u64(); const auto count = r.u32();
                if (code > BR_AUTO_SKIPPED || reply.effect > BR_AUTO_EFFECT_UNKNOWN || reply.complete > 1 ||
                    actions != supported_actions || count > (reply.bytes.size() - r.pos) / 72)
                    throw wire::Error{BR_AUTO_PROVIDER_ERROR};
                reply.status = static_cast<br_auto_status>(code);
                reply.nodes.reserve(count);
                for (uint32_t i = 0; i < count; ++i) reply.nodes.push_back(r.node());
                r.end();
                if (reply.status != BR_AUTO_OK && count) throw wire::Error{BR_AUTO_PROVIDER_ERROR};
                if (effect) *effect = reply.effect;
                if (reply.status == BR_AUTO_TIMEOUT || reply.status == BR_AUTO_CANCELLED) stop();
                return reply.status;
            }
        } catch (const std::bad_alloc&) { stop(); return BR_AUTO_OUT_OF_MEMORY; }
          catch (...) { stop(); return BR_AUTO_PROVIDER_ERROR; }
    }
    br_auto_status start(const br_auto_windows_isolated_options& o, const br_auto_options& options, const std::wstring& path) {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        mapping_.h = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, cap_ + 4, nullptr);
        request_.h = CreateEventW(&sa, FALSE, FALSE, nullptr); response_.h = CreateEventW(&sa, FALSE, FALSE, nullptr);
        if (!mapping_.h || !request_.h || !response_.h) return BR_AUTO_PROVIDER_ERROR;
        view_.p = MapViewOfFile(mapping_.h, FILE_MAP_ALL_ACCESS, 0, 0, cap_ + 4);
        if (!view_.p) return BR_AUTO_PROVIDER_ERROR;
        job_.h = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job_.h || !SetInformationJobObject(job_.h, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            return BR_AUTO_PROVIDER_ERROR;
        Handle nul(CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr));
        if (nul.h == INVALID_HANDLE_VALUE) return BR_AUTO_PROVIDER_ERROR;
        HANDLE inherited[] = {mapping_.h, request_.h, response_.h, nul.h};
        Attributes attrs; if (!attrs.init(inherited, 4)) return BR_AUTO_PROVIDER_ERROR;
        auto desktop = object_name(GetProcessWindowStation()) + L"\\" + object_name(GetThreadDesktop(GetCurrentThreadId()));
        std::wstring command = L"\"" + path + L"\" --br-uia " + std::to_wstring(reinterpret_cast<uintptr_t>(mapping_.h)) +
            L" " + std::to_wstring(reinterpret_cast<uintptr_t>(request_.h)) + L" " +
            std::to_wstring(reinterpret_cast<uintptr_t>(response_.h)) + L" " + std::to_wstring(cap_);
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.lpDesktop = desktop.data(); startup.lpAttributeList = attrs.list;
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = nul.h;
        PROCESS_INFORMATION pi{};
        const auto launched = CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, TRUE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup.StartupInfo, &pi);
        for (auto h : inherited) SetHandleInformation(h, HANDLE_FLAG_INHERIT, 0);
        if (!launched) return BR_AUTO_PROVIDER_ERROR;
        process_.h = pi.hProcess; Handle thread(pi.hThread);
        // Helper начинает работу только после присоединения к нашему job.
        if (!AssignProcessToJobObject(job_.h, process_.h) || ResumeThread(thread.h) == DWORD(-1)) {
            stop(); return BR_AUTO_PROVIDER_ERROR;
        }
        auto w = request(wire::init, o.startup_timeout_ms);
        w.u64(o.target.window); w.u32(o.target.max_depth); w.u32(o.target.connection_timeout_ms);
        w.u32(o.target.transaction_timeout_ms); w.u32(options.max_nodes); w.u64(options.max_text_bytes);
        Reply reply; const auto st = transact(w, nullptr, o.startup_timeout_ms, reply);
        if (st == BR_AUTO_OK && (!reply.nodes.empty() || reply.effect != BR_AUTO_NOT_DISPATCHED || reply.complete)) {
            stop(); return BR_AUTO_PROVIDER_ERROR;
        }
        return st;
    }
    br_auto_status observe(const br_auto_observe_request& req, const br_auto_operation* op, br_auto_emit_fn emit, void* sink, br_auto_observe_info* info) {
        auto w = request(wire::observe, br_auto_operation_remaining_ms(op)); w.u32(req.max_nodes); w.u64(req.max_text_bytes);
        Reply reply; auto st = transact(w, op, 0, reply);
        if (st != BR_AUTO_OK) return st;
        if (reply.nodes.size() > req.max_nodes || reply.effect != BR_AUTO_NOT_DISPATCHED) { stop(); return BR_AUTO_PROVIDER_ERROR; }
        for (const auto& n : reply.nodes) { st = emit(sink, &n); if (st != BR_AUTO_OK) return st; }
        info->complete = reply.complete; return BR_AUTO_OK;
    }
    br_auto_status read(uint64_t id, uint64_t gen, const br_auto_operation* op, br_auto_emit_fn emit, void* sink) {
        auto w = request(wire::read, br_auto_operation_remaining_ms(op)); w.u64(id); w.u64(gen);
        Reply reply; const auto st = transact(w, op, 0, reply);
        if (st != BR_AUTO_OK) return st;
        if (reply.nodes.size() != 1 || reply.nodes[0].native_id != id || reply.nodes[0].incarnation != gen ||
            reply.effect != BR_AUTO_NOT_DISPATCHED || reply.complete) { stop(); return BR_AUTO_PROVIDER_ERROR; }
        return emit(sink, &reply.nodes[0]);
    }
    br_auto_status act(uint64_t id, uint64_t gen, const br_auto_action& a, const br_auto_operation* op, uint32_t* effect) {
        *effect = BR_AUTO_NOT_DISPATCHED;
        auto w = request(wire::act, br_auto_operation_remaining_ms(op)); w.u64(id); w.u64(gen); w.u32(a.kind); w.string(a.value);
        Reply reply; const auto st = transact(w, op, 0, reply, effect);
        if (!reply.nodes.empty() || reply.complete) { *effect = BR_AUTO_EFFECT_UNKNOWN; stop(); return BR_AUTO_PROVIDER_ERROR; }
        return st;
    }
};
struct Service {
    br_auto_provider provider{};
    br_auto_session* session = nullptr;
    uint32_t nodes_limit = 0;
    size_t text_limit = 0;
    ~Service() { if (session) br_auto_session_destroy(session); else if (provider.destroy) provider.destroy(provider.user); }
};
struct Call {
    Service& service;
    wire::Reader& request;
    wire::Writer nodes;
    uint32_t command, effect = 0, complete = 0, count = 0;
    Call(Service& s, wire::Reader& r, uint32_t cap, uint32_t cmd) : service(s), request(r), nodes(cap), command(cmd) {}
    static br_auto_status emit(void* user, const br_auto_node* n) {
        auto& c = *static_cast<Call*>(user);
        if (c.count >= c.service.nodes_limit) return BR_AUTO_LIMIT_EXCEEDED;
        try { c.nodes.node(*n); ++c.count; return BR_AUTO_OK; }
        catch (const wire::Error& e) { return e.status; }
    }
    static br_auto_status run(void* user, const br_auto_operation* op) {
        auto& c = *static_cast<Call*>(user); auto& r = c.request; auto& p = c.service.provider;
        if (c.command == wire::observe) {
            br_auto_observe_request req{}; req.max_nodes = r.u32(); const auto text = r.u64(); r.end();
            if (!req.max_nodes || req.max_nodes > c.service.nodes_limit || !text || text > c.service.text_limit)
                return BR_AUTO_INVALID_ARGUMENT;
            req.max_text_bytes = static_cast<size_t>(text); br_auto_observe_info info{};
            const auto st = p.observe(p.user, &req, op, emit, &c, &info); c.complete = info.complete; return st;
        }
        const auto id = r.u64(), gen = r.u64();
        if (c.command == wire::read) { r.end(); return p.read(p.user, id, gen, op, emit, &c); }
        if (c.command == wire::act) {
            auto a = br_auto_action_default(r.u32()); a.value = r.string(); r.end();
            if (a.kind < BR_AUTO_INVOKE || a.kind > BR_AUTO_SELECT_NAMED || !(p.actions & BR_AUTO_ACTION_BIT(a.kind)) ||
                a.value.size > c.service.text_limit) return BR_AUTO_INVALID_ARGUMENT;
            return p.act(p.user, id, gen, &a, op, &c.effect);
        }
        return BR_AUTO_PROVIDER_ERROR;
    }
};
}
#endif

extern "C" {
br_auto_windows_isolated_options br_auto_windows_isolated_options_default(uint64_t window, br_auto_string path) {
    return {sizeof(br_auto_windows_isolated_options), BR_AUTO_ABI_VERSION,
        br_auto_windows_options_default(window), path, 5000, 8u * 1024u * 1024u};
}
br_auto_status br_auto_windows_open_isolated(const br_auto_windows_isolated_options* o, const br_auto_options* options, br_auto_session** out) {
    if (!out) return BR_AUTO_INVALID_ARGUMENT;
    *out = nullptr;
    if (!o || o->struct_size != sizeof(*o) || o->abi_version != BR_AUTO_ABI_VERSION ||
        o->target.struct_size != sizeof(o->target) || o->target.abi_version != BR_AUTO_ABI_VERSION ||
        !o->target.window || o->target.window > UINTPTR_MAX || o->target.max_depth > 128 ||
        !o->target.connection_timeout_ms || !o->target.transaction_timeout_ms || !o->startup_timeout_ms ||
        o->max_ipc_bytes < 4096 || o->max_ipc_bytes > br::automation::wire::max_bytes ||
        (options && (options->struct_size != sizeof(*options) || options->abi_version != BR_AUTO_ABI_VERSION)))
        return BR_AUTO_INVALID_ARGUMENT;
#if defined(_WIN32) && defined(BR_HAS_WINDOWS_AUTOMATION)
    try {
        const auto path = helper_path(o->helper_path);
        auto host = std::make_unique<Isolated>(o->max_ipc_bytes); auto* transport = host.get();
        br_auto_provider p{}; p.struct_size = sizeof(p); p.abi_version = BR_AUTO_ABI_VERSION;
        p.name = "windows-uia-isolated"; p.user = host.get(); p.actions = supported_actions;
        p.observe = [](void* u, const br_auto_observe_request* r, const br_auto_operation* op,
                       br_auto_emit_fn emit, void* sink, br_auto_observe_info* info) {
            return ipc_call([&] { return static_cast<Isolated*>(u)->observe(*r, op, emit, sink, info); });
        };
        p.read = [](void* u, uint64_t id, uint64_t gen, const br_auto_operation* op, br_auto_emit_fn emit, void* sink) {
            return ipc_call([&] { return static_cast<Isolated*>(u)->read(id, gen, op, emit, sink); });
        };
        p.act = [](void* u, uint64_t id, uint64_t gen, const br_auto_action* a, const br_auto_operation* op, uint32_t* effect) {
            return ipc_call([&] { return static_cast<Isolated*>(u)->act(id, gen, *a, op, effect); });
        };
        p.destroy = [](void* u) { delete static_cast<Isolated*>(u); };
        br_auto_session* raw = nullptr; auto st = br_auto_session_create(&p, options, &raw);
        if (st != BR_AUTO_OK) return st;
        host.release(); std::unique_ptr<br_auto_session, decltype(&br_auto_session_destroy)> session(raw, br_auto_session_destroy);
        st = transport->start(*o, options ? *options : br_auto_options_default(), path);
        if (st != BR_AUTO_OK) return st;
        *out = session.release(); return BR_AUTO_OK;
    } catch (const wire::Error& e) { return e.status; }
      catch (const std::bad_alloc&) { return BR_AUTO_OUT_OF_MEMORY; }
      catch (...) { return BR_AUTO_PROVIDER_ERROR; }
#else
    (void)options; return BR_AUTO_UNSUPPORTED;
#endif
}
}

int br::automation::windows_helper(int argc, char** argv) {
#if defined(_WIN32) && defined(BR_HAS_WINDOWS_AUTOMATION)
    try {
        if (argc != 6 || std::strcmp(argv[1], "--br-uia")) return 2;
        auto number = [](const char* text) {
            if (!text || !*text) throw wire::Error{BR_AUTO_INVALID_ARGUMENT};
            uint64_t n = 0;
            for (auto p = text; *p; ++p) {
                if (*p < '0' || *p > '9' || n > (UINT64_MAX - uint64_t(*p - '0')) / 10)
                    throw wire::Error{BR_AUTO_INVALID_ARGUMENT};
                n = n * 10 + uint64_t(*p - '0');
            }
            return n;
        };
        auto inherited = [&](const char* text) {
            const auto n = number(text);
            if (!n || n > UINTPTR_MAX || n == UINTPTR_MAX) throw wire::Error{BR_AUTO_INVALID_ARGUMENT};
            const auto h = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(n)); DWORD flags = 0;
            if (!GetHandleInformation(h, &flags) || !(flags & HANDLE_FLAG_INHERIT)) throw wire::Error{BR_AUTO_INVALID_ARGUMENT};
            SetHandleInformation(h, HANDLE_FLAG_INHERIT, 0); return h;
        };
        Handle mapping(inherited(argv[2])), request(inherited(argv[3])), response(inherited(argv[4]));
        const auto size = number(argv[5]); if (size < 4096 || size > wire::max_bytes) return 2;
        const auto cap = static_cast<uint32_t>(size);
        Mapping view; view.p = MapViewOfFile(mapping.h, FILE_MAP_ALL_ACCESS, 0, 0, cap + 4);
        if (!view.p) return 2;
        Service service; uint64_t sequence = 0;
        while (WaitForSingleObject(request.h, INFINITE) == WAIT_OBJECT_0) {
            MemoryBarrier(); const auto data = wire::load(view.p, cap); wire::Reader r(data.data(), data.size());
            if (r.u32() != wire::version || r.u64() != ++sequence) return 3;
            const auto cmd = r.u32(), timeout = r.u32(); if (!timeout) return 3;
            br_auto_status st = BR_AUTO_PROVIDER_ERROR; Call call(service, r, cap - 36, cmd);
            try {
                if (cmd == wire::init && !service.session && sequence == 1) {
                    auto native = br_auto_windows_options_default(r.u64()); native.max_depth = r.u32();
                    native.connection_timeout_ms = r.u32(); native.transaction_timeout_ms = r.u32();
                    auto options = br_auto_options_default(); options.max_nodes = r.u32(); const auto text = r.u64(); r.end();
                    if (!text || text > wire::max_bytes || !options.max_nodes || options.max_nodes > 1000000) return 3;
                    options.max_text_bytes = static_cast<size_t>(text); service.nodes_limit = options.max_nodes; service.text_limit = options.max_text_bytes;
                    st = windows_provider(native, options.max_text_bytes, service.provider);
                    if (st == BR_AUTO_OK) st = br_auto_session_create(&service.provider, &options, &service.session);
                } else if (service.session) st = with_operation(service.session, timeout, Call::run, &call);
            } catch (const wire::Error& e) { st = e.status; }
              catch (const std::bad_alloc&) { st = BR_AUTO_OUT_OF_MEMORY; }
              catch (...) { st = BR_AUTO_PROVIDER_ERROR; }
            wire::Writer reply(cap); reply.u32(wire::version); reply.u64(sequence); reply.u32(st);
            reply.u32(call.effect); reply.u32(st == BR_AUTO_OK ? call.complete : 0); reply.u64(supported_actions);
            reply.u32(st == BR_AUTO_OK ? call.count : 0);
            if (st == BR_AUTO_OK) { reply.reserve(call.nodes.bytes.size()); reply.bytes.insert(reply.bytes.end(), call.nodes.bytes.begin(), call.nodes.bytes.end()); }
            wire::store(view.p, reply); MemoryBarrier(); if (!SetEvent(response.h)) return 3;
        }
        return 3;
    } catch (...) { return 3; }
#else
    (void)argc; (void)argv; return 2;
#endif
}
