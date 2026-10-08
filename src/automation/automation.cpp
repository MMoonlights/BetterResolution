#include <br/br_automation.h>
#include "windows_provider.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
constexpr uint32_t states = 127u;
constexpr uint64_t action_bits = ((UINT64_C(1) << (BR_AUTO_SELECT_NAMED + 1)) - 2);
std::atomic<uint64_t> next_id{1};
uint64_t unique_id() {
    uint64_t n = next_id.load(std::memory_order_relaxed);
    for (;;) {
        if (n == UINT64_MAX) throw std::bad_alloc();
        if (next_id.compare_exchange_weak(n, n + 1, std::memory_order_relaxed)) return n;
    }
}
uint64_t elapsed(Clock::time_point start) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
}
br_auto_string view(const std::string& s) { return {s.data(), s.size()}; }
bool valid_string(br_auto_string s, size_t limit) {
    if (s.size > limit || (!s.data && s.size)) return false;
    // Неверный UTF-8 и встроенные NUL отвергаются до передачи в native API.
    for (size_t i = 0; i < s.size;) {
        const auto c = static_cast<unsigned char>(s.data[i++]);
        if (!c) return false;
        if (c < 0x80) continue;
        uint32_t cp; size_t extra; uint32_t minimum;
        if (c >= 0xc2 && c <= 0xdf) { cp = c & 31u; extra = 1; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { cp = c & 15u; extra = 2; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { cp = c & 7u; extra = 3; minimum = 0x10000; }
        else return false;
        if (extra > s.size - i) return false;
        while (extra--) {
            const auto t = static_cast<unsigned char>(s.data[i++]);
            if ((t & 0xc0u) != 0x80u) return false;
            cp = (cp << 6u) | (t & 63u);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
    }
    return true;
}
std::string copy(br_auto_string s) { return s.size ? std::string(s.data, s.size) : std::string{}; }
bool equal(br_auto_string s, const std::string& b) {
    return s.size == b.size() && (!s.size || std::memcmp(s.data, b.data(), s.size) == 0);
}
bool selector_valid(const br_auto_selector& s, size_t limit) {
    return valid_string(s.role, limit) && valid_string(s.name, limit) && valid_string(s.automation_id, limit) &&
        !((s.required_state | s.forbidden_state) & ~uint32_t(95)) && !(s.required_state & s.forbidden_state);
}
bool target_valid(const br_auto_target& t, size_t limit) {
    return (t.element_id != 0) != (t.selector != nullptr) && (!t.selector || selector_valid(*t.selector, limit));
}
bool condition_valid(const br_auto_condition& c, size_t limit) {
    return target_valid(c.target, limit) && c.property >= BR_AUTO_EXISTS && c.property <= BR_AUTO_IS_FOCUSED &&
        c.expected <= 1 && valid_string(c.value, limit);
}
bool action_valid(const br_auto_action& a, size_t limit) {
    return a.struct_size == sizeof(a) && a.kind >= BR_AUTO_INVOKE && a.kind <= BR_AUTO_SELECT_NAMED &&
        target_valid(a.target, limit) && valid_string(a.value, limit) && valid_string(a.custom_action, limit) &&
        (a.kind != BR_AUTO_CUSTOM || a.custom_action.size != 0) &&
        (a.kind == BR_AUTO_CUSTOM || a.custom_action.size == 0) && a.dry_run <= 1 &&
        (a.kind != BR_AUTO_SELECT_NAMED || a.value.size != 0) &&
        (!a.before || condition_valid(*a.before, limit)) && (!a.after || condition_valid(*a.after, limit));
}
struct Node {
    uint64_t native = 0, incarnation = 0, parent_native = 0, id = 0, revision = 0, parent = 0;
    std::string role, name, automation_id, value;
    br_rect_i32 bounds{};
    uint32_t known = 0, state = 0;
    uint64_t actions = 0;
    br_auto_element element() const {
        return {id, revision, parent, view(role), view(name), view(automation_id), view(value), bounds, known, state, actions};
    }
};
bool same(const Node& a, const Node& b) {
    return a.id == b.id && a.parent == b.parent && a.role == b.role && a.name == b.name &&
        a.automation_id == b.automation_id && a.value == b.value && a.known == b.known &&
        a.state == b.state && a.actions == b.actions && a.bounds.x == b.bounds.x && a.bounds.y == b.bounds.y &&
        a.bounds.width == b.bounds.width && a.bounds.height == b.bounds.height;
}
struct State {
    uint64_t id = 0;
    bool complete = false;
    std::vector<Node> nodes;
    std::unordered_map<uint64_t, size_t> by_id, by_native;
};
template<class F> br_auto_status guarded(F&& f) noexcept {
    try { return f(); }
    catch (const std::bad_alloc&) { return BR_AUTO_OUT_OF_MEMORY; }
    catch (...) { return BR_AUTO_PROVIDER_ERROR; }
}
}

struct br_auto_session {
    br_auto_provider provider{};
    br_auto_options options{};
    std::string name;
    std::shared_ptr<const State> last, published;
    std::mutex gate, wait_mutex;
    std::condition_variable wake;
    std::atomic<bool> cancelled{false};
};
struct br_auto_operation { br_auto_session* session; Clock::time_point deadline; };
struct br_auto_snapshot {
    std::shared_ptr<const State> current, previous;
    std::vector<br_auto_change> changes;
    br_auto_snapshot_info info{};
};

namespace {
br_auto_operation operation(br_auto_session* s, uint32_t ms) {
    return {s, Clock::now() + std::chrono::milliseconds(ms ? ms : s->options.default_timeout_ms)};
}
struct Collector {
    const br_auto_operation* op;
    const br_auto_options* options;
    std::shared_ptr<State> state = std::make_shared<State>();
    size_t bytes = 0;
    br_auto_status failure = BR_AUTO_OK;
    static br_auto_status emit(void* user, const br_auto_node* n) noexcept {
        auto& c = *static_cast<Collector*>(user);
        if (c.failure != BR_AUTO_OK) return c.failure;
        c.failure = guarded([&]() -> br_auto_status {
            const auto status = br_auto_operation_status(c.op);
            if (status != BR_AUTO_OK) return status;
            if (!n || !n->native_id || n->native_id == n->parent_native_id ||
                (n->known & ~states) || (n->state & ~n->known) || (n->actions & ~action_bits) ||
                n->bounds.width < 0 || n->bounds.height < 0) return BR_AUTO_PROVIDER_ERROR;
            if (c.state->nodes.size() >= c.options->max_nodes) return BR_AUTO_LIMIT_EXCEEDED;
            size_t size = 0;
            for (auto s : {n->role, n->name, n->automation_id, n->value}) {
                if (!valid_string(s, c.options->max_text_bytes)) return BR_AUTO_PROVIDER_ERROR;
                if (s.size > c.options->max_text_bytes - size) return BR_AUTO_LIMIT_EXCEEDED;
                size += s.size;
            }
            if (size > c.options->max_text_bytes - c.bytes) return BR_AUTO_LIMIT_EXCEEDED;
            if (!c.state->by_native.emplace(n->native_id, c.state->nodes.size()).second) return BR_AUTO_PROVIDER_ERROR;
            Node v;
            v.native = n->native_id; v.incarnation = n->incarnation; v.parent_native = n->parent_native_id;
            v.role = copy(n->role); v.name = copy(n->name); v.automation_id = copy(n->automation_id); v.value = copy(n->value);
            v.bounds = n->bounds; v.known = n->known; v.state = n->state; v.actions = n->actions;
            c.state->nodes.push_back(std::move(v)); c.bytes += size;
            return BR_AUTO_OK;
        });
        return c.failure;
    }
};
br_auto_status refresh(br_auto_session* s, const br_auto_operation& op, uint64_t since = 0, br_auto_snapshot** out = nullptr) {
    auto status = br_auto_operation_status(&op);
    if (status != BR_AUTO_OK) return status;
    const auto start = Clock::now();
    Collector c{&op, &s->options};
    br_auto_observe_info info{};
    const br_auto_observe_request request{s->options.max_nodes, s->options.max_text_bytes};
    status = s->provider.observe(s->provider.user, &request, &op, &Collector::emit, &c, &info);
    if (c.failure != BR_AUTO_OK) return c.failure;
    if (status != BR_AUTO_OK) return status;
    status = br_auto_operation_status(&op);
    if (status != BR_AUTO_OK) return status;
    if (info.complete > 1) return BR_AUTO_PROVIDER_ERROR;
    auto& state = *c.state;
    state.id = unique_id(); state.complete = info.complete != 0;
    const auto old = s->last;
    for (auto& n : state.nodes) {
        if (old) {
            const auto it = old->by_native.find(n.native);
            if (it != old->by_native.end()) {
                const auto& prev = old->nodes[it->second];
                if (prev.incarnation == n.incarnation) { n.id = prev.id; n.revision = prev.revision; }
            }
        }
        if (!n.id) { n.id = unique_id(); n.revision = unique_id(); }
        state.by_id.emplace(n.id, static_cast<size_t>(&n - state.nodes.data()));
    }
    for (auto& n : state.nodes) {
        const auto parent = state.by_native.find(n.parent_native);
        if (parent != state.by_native.end()) n.parent = state.nodes[parent->second].id;
        if (old) {
            const auto it = old->by_id.find(n.id);
            if (it != old->by_id.end() && !same(n, old->nodes[it->second])) n.revision = unique_id();
        }
    }
    std::unique_ptr<br_auto_snapshot> snapshot;
    if (out) {
        snapshot = std::make_unique<br_auto_snapshot>();
        snapshot->current = c.state;
        const auto baseline = s->published;
        const bool delta = baseline && baseline->complete && state.complete && since == baseline->id;
        snapshot->info = {state.id, delta ? baseline->id : 0, elapsed(start), state.nodes.size(), 0,
                          delta ? 1u : 0u, state.complete ? 1u : 0u};
        if (delta) snapshot->previous = baseline;
        for (const auto& n : state.nodes) {
            uint32_t kind = BR_AUTO_PRESENT;
            if (delta) {
                const auto it = baseline->by_id.find(n.id);
                if (it == baseline->by_id.end()) kind = BR_AUTO_ADDED;
                else if (n.revision != baseline->nodes[it->second].revision) kind = BR_AUTO_UPDATED;
                else continue;
            }
            snapshot->changes.push_back({kind, n.element()});
        }
        if (delta) for (const auto& n : baseline->nodes)
            if (!state.by_id.count(n.id)) snapshot->changes.push_back({BR_AUTO_REMOVED, n.element()});
        snapshot->info.change_count = snapshot->changes.size();
    }
    s->last = std::move(c.state);
    if (out) { s->published = s->last; *out = snapshot.release(); }
    return BR_AUTO_OK;
}
br_auto_status find_node(const State& state, const br_auto_selector& sel, const Node*& out) {
    out = nullptr;
    bool unknown = !state.complete;
    for (const auto& n : state.nodes) {
        if ((sel.role.size && !equal(sel.role, n.role)) || (sel.name.size && !equal(sel.name, n.name)) ||
            (sel.automation_id.size && !equal(sel.automation_id, n.automation_id))) continue;
        if ((sel.required_state & n.known & ~n.state) || (sel.forbidden_state & n.state)) continue;
        if ((sel.required_state | sel.forbidden_state) & ~n.known) { unknown = true; continue; }
        if (out) return BR_AUTO_AMBIGUOUS;
        out = &n;
    }
    if (unknown) return BR_AUTO_INCOMPLETE;
    return out ? BR_AUTO_OK : BR_AUTO_NOT_FOUND;
}
br_auto_status locate(const State& state, const br_auto_target& target, const Node*& out) {
    if (target.selector) return find_node(state, *target.selector, out);
    const auto it = state.by_id.find(target.element_id);
    out = it == state.by_id.end() ? nullptr : &state.nodes[it->second];
    return out ? BR_AUTO_OK : state.complete ? BR_AUTO_STALE : BR_AUTO_INCOMPLETE;
}
br_auto_status evaluate(const State& state, const br_auto_condition& c, uint32_t& result) {
    result = BR_AUTO_UNKNOWN;
    const Node* n = nullptr;
    const auto status = locate(state, c.target, n);
    if (c.property == BR_AUTO_EXISTS && (status == BR_AUTO_OK || status == BR_AUTO_STALE || status == BR_AUTO_NOT_FOUND)) {
        result = ((status == BR_AUTO_OK) == (c.expected != 0)) ? BR_AUTO_SATISFIED : BR_AUTO_UNSATISFIED;
        return BR_AUTO_OK;
    }
    if (status != BR_AUTO_OK) return status;
    if (c.property == BR_AUTO_VALUE_EQUALS) {
        if (!(n->known & BR_AUTO_VALUE_KNOWN)) return BR_AUTO_INCOMPLETE;
        result = equal(c.value, n->value) ? BR_AUTO_SATISFIED : BR_AUTO_UNSATISFIED;
    } else {
        const uint32_t bits[] = {BR_AUTO_ENABLED, BR_AUTO_VISIBLE, BR_AUTO_SELECTED, BR_AUTO_TOGGLED, BR_AUTO_FOCUSED};
        const uint32_t bit = bits[c.property - BR_AUTO_IS_ENABLED];
        if (!(n->known & bit)) return BR_AUTO_INCOMPLETE;
        result = (((n->state & bit) != 0) == (c.expected != 0)) ? BR_AUTO_SATISFIED : BR_AUTO_UNSATISFIED;
    }
    return BR_AUTO_OK;
}
br_auto_status read_target(br_auto_session* s, uint64_t id, const br_auto_operation& op, Node& out) {
    auto status = br_auto_operation_status(&op);
    if (status != BR_AUTO_OK) return status;
    if (!s->last) return BR_AUTO_STALE;
    const auto it = s->last->by_id.find(id);
    if (it == s->last->by_id.end()) return s->last->complete ? BR_AUTO_STALE : BR_AUTO_INCOMPLETE;
    const auto& old = s->last->nodes[it->second];
    Collector c{&op, &s->options};
    status = s->provider.read(s->provider.user, old.native, old.incarnation, &op, &Collector::emit, &c);
    if (c.failure != BR_AUTO_OK) return c.failure;
    if (status != BR_AUTO_OK) return status;
    status = br_auto_operation_status(&op);
    if (status != BR_AUTO_OK) return status;
    if (c.state->nodes.size() != 1) return BR_AUTO_PROVIDER_ERROR;
    out = std::move(c.state->nodes.front());
    if (out.native != old.native || out.incarnation != old.incarnation) return BR_AUTO_STALE;
    out.id = old.id; out.parent = old.parent; out.revision = old.revision;
    const auto parent = s->last->by_native.find(out.parent_native);
    out.parent = parent == s->last->by_native.end() ? 0 : s->last->nodes[parent->second].id;
    if (!same(out, old)) out.revision = unique_id();
    return BR_AUTO_OK;
}
br_auto_status check_condition(br_auto_session* s, const br_auto_condition& c, const br_auto_operation& op, uint32_t& v) {
    if (!s->provider.read || !c.target.element_id) {
        const auto st = refresh(s, op);
        if (st != BR_AUTO_OK) return st;
        return evaluate(*s->last, c, v);
    }
    Node node;
    const auto st = read_target(s, c.target.element_id, op, node);
    if (st == BR_AUTO_UNSUPPORTED) {
        const auto full = refresh(s, op);
        return full == BR_AUTO_OK ? evaluate(*s->last, c, v) : full;
    }
    if (st != BR_AUTO_OK && st != BR_AUTO_STALE && st != BR_AUTO_NOT_FOUND) { v = BR_AUTO_UNKNOWN; return st; }
    State single;
    single.complete = true;
    if (st == BR_AUTO_OK) { single.by_id.emplace(node.id, 0); single.nodes.push_back(std::move(node)); }
    return evaluate(single, c, v);
}
br_auto_status wait_for(br_auto_session* s, const br_auto_condition& c, const br_auto_operation& op, uint32_t& verification) {
    verification = BR_AUTO_UNKNOWN;
    uint32_t pause = 2;
    for (;;) {
        auto status = check_condition(s, c, op, verification);
        if (status == BR_AUTO_OK && verification == BR_AUTO_SATISFIED) return BR_AUTO_OK;
        if (status != BR_AUTO_OK && status != BR_AUTO_INCOMPLETE && status != BR_AUTO_NOT_FOUND && status != BR_AUTO_STALE) return status;
        status = br_auto_operation_status(&op);
        if (status != BR_AUTO_OK) return status;
        const uint32_t ms = std::min(pause, br_auto_operation_remaining_ms(&op));
        bool poll = !s->provider.wait_event;
        if (!poll) {
            status = s->provider.wait_event(s->provider.user, ms, &op);
            poll = status == BR_AUTO_UNSUPPORTED;
            if (status != BR_AUTO_OK && status != BR_AUTO_TIMEOUT && !poll) return status;
        }
        if (poll) {
            std::unique_lock<std::mutex> lock(s->wait_mutex);
            s->wake.wait_for(lock, std::chrono::milliseconds(ms), [&] { return s->cancelled.load(); });
        }
        pause = std::min(pause * 2, 32u);
    }
}
br_auto_status do_act(br_auto_session* s, const br_auto_action& a, br_auto_operation op, br_auto_action_result& r) {
    if (a.timeout_ms) op.deadline = std::min(op.deadline, Clock::now() + std::chrono::milliseconds(a.timeout_ms));
    if (!s->options.allow_actions) return BR_AUTO_DENIED;
    if (!s->provider.act || !(s->provider.actions & BR_AUTO_ACTION_BIT(a.kind))) return BR_AUTO_UNSUPPORTED;
    br_auto_status status;
    Node targeted;
    const Node* n = nullptr;
    if (s->provider.read && a.target.element_id && !a.before) {
        status = read_target(s, a.target.element_id, op, targeted);
        n = &targeted;
        if (status == BR_AUTO_UNSUPPORTED) {
            status = refresh(s, op);
            if (status == BR_AUTO_OK) status = locate(*s->last, a.target, n);
        }
    } else {
        status = refresh(s, op);
        if (status == BR_AUTO_OK) status = locate(*s->last, a.target, n);
    }
    if (status != BR_AUTO_OK) return status;
    r.element_id = n->id;
    if (a.expected_revision && a.expected_revision != n->revision) return BR_AUTO_STALE;
    if (!(n->actions & BR_AUTO_ACTION_BIT(a.kind))) return BR_AUTO_UNSUPPORTED;
    if (!(n->known & BR_AUTO_ENABLED)) return BR_AUTO_INCOMPLETE;
    if (!(n->state & BR_AUTO_ENABLED)) return BR_AUTO_PRECONDITION_FAILED;
    if (a.before) {
        uint32_t v = BR_AUTO_UNKNOWN;
        status = evaluate(*s->last, *a.before, v);
        if (status != BR_AUTO_OK) return status;
        if (v != BR_AUTO_SATISFIED) return BR_AUTO_PRECONDITION_FAILED;
    }
    const auto element = n->element();
    if (s->options.authorize) {
        status = s->options.authorize(s->options.authorize_user, &a, &element);
        if (status != BR_AUTO_OK) return status;
    }
    status = br_auto_operation_status(&op);
    if (status != BR_AUTO_OK || a.dry_run) return status;
    r.effect = BR_AUTO_EFFECT_UNKNOWN;
    status = s->provider.act(s->provider.user, n->native, n->incarnation, &a, &op, &r.effect);
    if (r.effect > BR_AUTO_EFFECT_UNKNOWN) { r.effect = BR_AUTO_EFFECT_UNKNOWN; return BR_AUTO_PROVIDER_ERROR; }
    if (status != BR_AUTO_OK) return status;
    if (r.effect != BR_AUTO_DISPATCHED) return BR_AUTO_PROVIDER_ERROR;
    br_auto_condition automatic{};
    const br_auto_condition* after = a.after;
    if (!after && (a.kind == BR_AUTO_SET_VALUE || a.kind == BR_AUTO_SELECT || a.kind == BR_AUTO_SELECT_NAMED)) {
        automatic.target.element_id = r.element_id;
        automatic.property = a.kind == BR_AUTO_SELECT ? BR_AUTO_IS_SELECTED : BR_AUTO_VALUE_EQUALS;
        automatic.value = a.value; automatic.expected = 1; after = &automatic;
    }
    if (after) return wait_for(s, *after, op, r.verification);
    return br_auto_operation_status(&op);
}
}

extern "C" {
br_auto_options br_auto_options_default(void) {
    return {sizeof(br_auto_options), BR_AUTO_ABI_VERSION, 4096, 2000, 4u * 1024u * 1024u, 0, 128, nullptr, nullptr};
}
br_auto_action br_auto_action_default(uint32_t kind) {
    br_auto_action a{}; a.struct_size = sizeof(a); a.kind = kind; return a;
}
const char* br_auto_status_string(br_auto_status status) {
    static const char* names[] = {"ok", "invalid_argument", "out_of_memory", "unsupported", "not_found", "ambiguous",
        "stale", "incomplete", "timeout", "cancelled", "denied", "precondition_failed", "provider_error", "busy", "limit_exceeded", "skipped"};
    const auto n = static_cast<unsigned>(status);
    return n < sizeof(names) / sizeof(*names) ? names[n] : "unknown_status";
}
br_auto_status br_auto_session_create(const br_auto_provider* p, const br_auto_options* options, br_auto_session** out) {
    if (!out) return BR_AUTO_INVALID_ARGUMENT;
    *out = nullptr;
    if (options && options->struct_size != sizeof(br_auto_options)) return BR_AUTO_INVALID_ARGUMENT;
    const auto o = options ? *options : br_auto_options_default();
    if (!p || p->struct_size != sizeof(*p) || p->abi_version != BR_AUTO_ABI_VERSION || !p->observe || !p->name ||
        !*p->name || (p->actions & ~action_bits) || (p->actions && !p->act) ||
        o.struct_size != sizeof(o) || o.abi_version != BR_AUTO_ABI_VERSION || !o.max_nodes || o.max_nodes > 1000000 ||
        !o.max_text_bytes || o.max_text_bytes > 64u * 1024u * 1024u || !o.default_timeout_ms ||
        o.allow_actions > 1 || !o.max_batch_steps || o.max_batch_steps > 4096) return BR_AUTO_INVALID_ARGUMENT;
    return guarded([&] {
        size_t name_size = 0;
        while (name_size <= 1024 && p->name[name_size]) ++name_size;
        if (name_size > 1024) return BR_AUTO_INVALID_ARGUMENT;
        auto s = std::make_unique<br_auto_session>(); s->provider = *p; s->options = o; s->name.assign(p->name, name_size);
        if (!valid_string(view(s->name), 1024)) return BR_AUTO_INVALID_ARGUMENT;
        s->provider.name = s->name.c_str(); *out = s.release(); return BR_AUTO_OK;
    });
}
void br_auto_session_destroy(br_auto_session* s) {
    if (!s) return;
    if (s->provider.destroy) { try { s->provider.destroy(s->provider.user); } catch (...) {} }
    delete s;
}
const char* br_auto_session_provider(const br_auto_session* s) { return s ? s->name.c_str() : ""; }
uint64_t br_auto_session_actions(const br_auto_session* s) { return s ? s->provider.actions : 0; }
void br_auto_cancel(br_auto_session* s) { if (s) { s->cancelled.store(true); s->wake.notify_all(); } }
br_auto_status br_auto_reset_cancel(br_auto_session* s) {
    if (!s) return BR_AUTO_INVALID_ARGUMENT;
    return guarded([&] {
        std::unique_lock<std::mutex> lock(s->gate, std::try_to_lock);
        if (!lock.owns_lock()) return BR_AUTO_BUSY;
        s->cancelled.store(false); return BR_AUTO_OK;
    });
}
br_auto_status br_auto_operation_status(const br_auto_operation* op) {
    if (!op) return BR_AUTO_INVALID_ARGUMENT;
    if (op->session->cancelled.load()) return BR_AUTO_CANCELLED;
    return Clock::now() >= op->deadline ? BR_AUTO_TIMEOUT : BR_AUTO_OK;
}
uint32_t br_auto_operation_remaining_ms(const br_auto_operation* op) {
    if (!op || br_auto_operation_status(op) != BR_AUTO_OK) return 0;
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(op->deadline - Clock::now()).count();
    return static_cast<uint32_t>(std::min<int64_t>(UINT32_MAX, std::max<int64_t>(1, (us + 999) / 1000)));
}
br_auto_status br_auto_observe(br_auto_session* s, uint64_t since, br_auto_snapshot** out) {
    if (!out) return BR_AUTO_INVALID_ARGUMENT;
    *out = nullptr;
    if (!s) return BR_AUTO_INVALID_ARGUMENT;
    return guarded([&] {
        std::unique_lock<std::mutex> lock(s->gate, std::try_to_lock);
        if (!lock.owns_lock()) return BR_AUTO_BUSY;
        return refresh(s, operation(s, 0), since, out);
    });
}
void br_auto_snapshot_destroy(br_auto_snapshot* snapshot) { delete snapshot; }
br_auto_snapshot_info br_auto_snapshot_get_info(const br_auto_snapshot* s) { return s ? s->info : br_auto_snapshot_info{}; }
br_auto_status br_auto_snapshot_element(const br_auto_snapshot* s, size_t index, br_auto_element* out) {
    if (!out) return BR_AUTO_INVALID_ARGUMENT;
    *out = {};
    if (!s || index >= s->current->nodes.size()) return BR_AUTO_INVALID_ARGUMENT;
    *out = s->current->nodes[index].element(); return BR_AUTO_OK;
}
br_auto_status br_auto_snapshot_change(const br_auto_snapshot* s, size_t index, br_auto_change* out) {
    if (!out) return BR_AUTO_INVALID_ARGUMENT;
    *out = {};
    if (!s || index >= s->changes.size()) return BR_AUTO_INVALID_ARGUMENT;
    *out = s->changes[index]; return BR_AUTO_OK;
}
br_auto_status br_auto_snapshot_find_id(const br_auto_snapshot* s, uint64_t id, br_auto_element* out) {
    if (!out) return BR_AUTO_INVALID_ARGUMENT;
    *out = {};
    if (!s || !id) return BR_AUTO_INVALID_ARGUMENT;
    const auto it = s->current->by_id.find(id);
    if (it == s->current->by_id.end()) return s->current->complete ? BR_AUTO_NOT_FOUND : BR_AUTO_INCOMPLETE;
    *out = s->current->nodes[it->second].element();
    return BR_AUTO_OK;
}
br_auto_status br_auto_find(const br_auto_snapshot* s, const br_auto_selector* sel, br_auto_element* out) {
    if (!out) return BR_AUTO_INVALID_ARGUMENT;
    *out = {};
    if (!s || !sel || !selector_valid(*sel, 64u * 1024u * 1024u)) return BR_AUTO_INVALID_ARGUMENT;
    const Node* node = nullptr;
    const auto status = find_node(*s->current, *sel, node);
    if (status == BR_AUTO_OK) *out = node->element();
    return status;
}
br_auto_status br_auto_act(br_auto_session* s, const br_auto_action* a, br_auto_action_result* out) {
    if (!out) return BR_AUTO_INVALID_ARGUMENT;
    *out = {}; out->status = BR_AUTO_INVALID_ARGUMENT;
    if (!s || !a || !action_valid(*a, s->options.max_text_bytes)) return out->status;
    const auto start = Clock::now();
    out->status = guarded([&] {
        std::unique_lock<std::mutex> lock(s->gate, std::try_to_lock);
        if (!lock.owns_lock()) return BR_AUTO_BUSY;
        return do_act(s, *a, operation(s, a->timeout_ms), *out);
    });
    out->elapsed_us = elapsed(start); return out->status;
}
br_auto_status br_auto_wait(br_auto_session* s, const br_auto_condition* c, uint32_t ms, uint32_t* out) {
    if (!out) return BR_AUTO_INVALID_ARGUMENT;
    *out = BR_AUTO_UNKNOWN;
    if (!s || !c || !condition_valid(*c, s->options.max_text_bytes)) return BR_AUTO_INVALID_ARGUMENT;
    return guarded([&] {
        std::unique_lock<std::mutex> lock(s->gate, std::try_to_lock);
        if (!lock.owns_lock()) return BR_AUTO_BUSY;
        return wait_for(s, *c, operation(s, ms), *out);
    });
}
br_auto_status br_auto_batch(br_auto_session* s, const br_auto_step* steps, size_t count, uint32_t ms,
                              br_auto_action_result* results, size_t capacity, br_auto_batch_result* out) {
    if (!out) return BR_AUTO_INVALID_ARGUMENT;
    *out = {}; out->status = BR_AUTO_INVALID_ARGUMENT; out->failed_index = SIZE_MAX;
    if (!s || !steps || !results || !count || count > s->options.max_batch_steps || capacity < count) return out->status;
    for (size_t i = 0; i < count; ++i) {
        const auto& step = steps[i];
        const bool valid = step.kind == BR_AUTO_STEP_ACT ? action_valid(step.action, s->options.max_text_bytes) :
            (step.kind == BR_AUTO_STEP_WAIT || step.kind == BR_AUTO_STEP_ASSERT) && condition_valid(step.condition, s->options.max_text_bytes);
        if (!valid) { out->failed_index = i; return out->status; }
    }
    for (size_t i = 0; i < count; ++i) { results[i] = {}; results[i].status = BR_AUTO_SKIPPED; }
    const auto start = Clock::now();
    out->status = guarded([&]() -> br_auto_status {
        std::unique_lock<std::mutex> lock(s->gate, std::try_to_lock);
        if (!lock.owns_lock()) return BR_AUTO_BUSY;
        const auto op = operation(s, ms);
        for (size_t i = 0; i < count; ++i) {
            out->failed_index = i;
            const auto t = Clock::now();
            auto& r = results[i];
            r.status = guarded([&]() -> br_auto_status {
                auto st = br_auto_operation_status(&op);
                if (st != BR_AUTO_OK) return st;
                if (steps[i].kind == BR_AUTO_STEP_ACT) return do_act(s, steps[i].action, op, r);
                if (steps[i].kind == BR_AUTO_STEP_WAIT) return wait_for(s, steps[i].condition, op, r.verification);
                st = check_condition(s, steps[i].condition, op, r.verification);
                return st == BR_AUTO_OK && r.verification != BR_AUTO_SATISFIED ? BR_AUTO_PRECONDITION_FAILED : st;
            });
            r.elapsed_us = elapsed(t);
            if (r.status != BR_AUTO_OK) return r.status;
            ++out->completed_steps;
        }
        out->failed_index = SIZE_MAX; return BR_AUTO_OK;
    });
    out->elapsed_us = elapsed(start); return out->status;
}
}

br_auto_status br::automation::with_operation(br_auto_session* session, uint32_t timeout_ms,
                                             OperationFn fn, void* user) {
    const auto op = operation(session, timeout_ms);
    return fn(user, &op);
}
