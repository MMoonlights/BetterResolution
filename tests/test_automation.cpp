#include "test_framework.hpp"
#include <br/br_automation.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {
br_auto_string str(const char* text) { return {text, std::strlen(text)}; }
struct Item {
    uint64_t native = 1, generation = 1;
    std::string name = "Search", value;
    uint32_t known = BR_AUTO_ENABLED | BR_AUTO_VISIBLE | BR_AUTO_VALUE_KNOWN | BR_AUTO_SELECTED;
    uint32_t state = BR_AUTO_ENABLED | BR_AUTO_VISIBLE;
};
struct Fake {
    std::vector<Item> items{Item{}, Item{2, 1, "Submit", ""}};
    std::atomic<int> reads{0};
    int calls = 0, destroys = 0, event_waits = 0;
    bool complete = true, ignore_set = false, throw_read = false, throw_act = false;
    bool duplicate = false, ignore_emit_error = false, bad_effect = false, fail_act = false;
    bool named = false;
    br_auto_session* session = nullptr;
    br_auto_status nested_status = BR_AUTO_OK;
    static br_auto_status observe(void* user, const br_auto_observe_request*, const br_auto_operation* op,
                                  br_auto_emit_fn emit, void* sink, br_auto_observe_info* info) {
        auto& f = *static_cast<Fake*>(user); ++f.reads;
        if (f.throw_read) throw std::runtime_error("provider exception");
        for (const auto& item : f.items) {
            if (br_auto_operation_status(op) != BR_AUTO_OK) return br_auto_operation_status(op);
            br_auto_node n{};
            n.native_id = f.duplicate ? 1 : item.native; n.incarnation = item.generation;
            n.name = {item.name.data(), item.name.size()}; n.role = str("control");
            n.value = {item.value.data(), item.value.size()}; n.bounds = {-100, 20, 40, 20};
            n.known = item.known; n.state = item.state;
            n.actions = BR_AUTO_ACTION_BIT(BR_AUTO_SET_VALUE) | BR_AUTO_ACTION_BIT(BR_AUTO_INVOKE) | BR_AUTO_ACTION_BIT(BR_AUTO_SELECT);
            if (f.named) n.actions |= BR_AUTO_ACTION_BIT(BR_AUTO_SELECT_NAMED);
            const auto st = emit(sink, &n);
            if (st != BR_AUTO_OK && !f.ignore_emit_error) return st;
        }
        info->complete = f.complete ? 1u : 0u;
        return BR_AUTO_OK;
    }
    static br_auto_status act(void* user, uint64_t id, uint64_t generation, const br_auto_action* a,
                              const br_auto_operation*, uint32_t* effect) {
        auto& f = *static_cast<Fake*>(user); ++f.calls;
        if (f.throw_act) throw std::runtime_error("unknown action outcome");
        for (auto& item : f.items) if (item.native == id && item.generation == generation) {
            *effect = BR_AUTO_DISPATCHED;
            if ((a->kind == BR_AUTO_SET_VALUE || a->kind == BR_AUTO_SELECT_NAMED) && !f.ignore_set) item.value = a->value.size ? std::string(a->value.data, a->value.size) : "";
            if (a->kind == BR_AUTO_SELECT) item.state |= BR_AUTO_SELECTED;
            if (f.bad_effect) *effect = 100;
            return f.fail_act ? BR_AUTO_PROVIDER_ERROR : BR_AUTO_OK;
        }
        *effect = BR_AUTO_NOT_DISPATCHED; return BR_AUTO_STALE;
    }
    static br_auto_status event(void* user, uint32_t, const br_auto_operation*) {
        auto& f = *static_cast<Fake*>(user); ++f.event_waits; f.items[0].value = "ready"; return BR_AUTO_OK;
    }
    static void destroy(void* user) { ++static_cast<Fake*>(user)->destroys; }
    br_auto_provider provider() {
        return {sizeof(br_auto_provider), BR_AUTO_ABI_VERSION, "custom-test-provider", this,
            BR_AUTO_ACTION_BIT(BR_AUTO_SET_VALUE) | BR_AUTO_ACTION_BIT(BR_AUTO_INVOKE) | BR_AUTO_ACTION_BIT(BR_AUTO_SELECT),
            &observe, &act, nullptr, nullptr, &destroy};
    }
};
struct Session {
    Fake fake;
    br_auto_session* p = nullptr;
    explicit Session(bool actions = true, uint32_t nodes = 64, size_t bytes = 65536) {
        auto provider = fake.provider(); auto o = br_auto_options_default();
        o.allow_actions = actions ? 1u : 0u; o.default_timeout_ms = 500; o.max_nodes = nodes; o.max_text_bytes = bytes;
        CHECK(br_auto_session_create(&provider, &o, &p) == BR_AUTO_OK);
        fake.session = p;
    }
    ~Session() { br_auto_session_destroy(p); }
};
using Snapshot = std::unique_ptr<br_auto_snapshot, decltype(&br_auto_snapshot_destroy)>;
Snapshot observe(Session& s, uint64_t since = 0) {
    br_auto_snapshot* raw = nullptr;
    CHECK(br_auto_observe(s.p, since, &raw) == BR_AUTO_OK);
    return Snapshot(raw, &br_auto_snapshot_destroy);
}
br_auto_element element(const Snapshot& s, size_t index = 0) {
    br_auto_element e{}; CHECK(br_auto_snapshot_element(s.get(), index, &e) == BR_AUTO_OK); return e;
}
br_auto_action action(uint64_t id, uint32_t kind = BR_AUTO_INVOKE) {
    auto a = br_auto_action_default(kind); a.target.element_id = id; return a;
}
br_auto_condition condition(uint64_t id, uint32_t property = BR_AUTO_VALUE_EQUALS, const char* value = "ready") {
    br_auto_condition c{}; c.target.element_id = id; c.property = property; c.value = str(value); c.expected = 1; return c;
}
}

TEST(automation_create_contract_and_ownership) {
    Fake f; auto p = f.provider(); br_auto_session* s = nullptr;
    auto o = br_auto_options_default();
    p.abi_version = 99;
    CHECK(br_auto_session_create(&p, &o, &s) == BR_AUTO_INVALID_ARGUMENT);
    CHECK(s == nullptr && f.destroys == 0);
    p.abi_version = BR_AUTO_ABI_VERSION;
    CHECK(br_auto_session_create(&p, nullptr, &s) == BR_AUTO_OK);
    CHECK(std::strcmp(br_auto_session_provider(s), "custom-test-provider") == 0);
    CHECK(br_auto_session_actions(s) == p.actions);
    br_auto_session_destroy(s);
    CHECK(f.destroys == 1);
    br_auto_session_destroy(nullptr);
}
TEST(automation_snapshot_lifetime_and_borrowed_utf8) {
    Session s; s.fake.items[0].name = "Поиск 🔍";
    auto snap = observe(s); const auto e = element(snap);
    s.fake.items[0].name = "changed";
    CHECK(std::string(e.name.data, e.name.size) == "Поиск 🔍");
    br_auto_session_destroy(s.p); s.p = nullptr;
    CHECK(std::string(element(snap).name.data, element(snap).name.size) == "Поиск 🔍");
}
TEST(automation_delta_baseline_identity_and_removal) {
    Session s; auto a = observe(s); auto info = br_auto_snapshot_get_info(a.get());
    auto b = observe(s, info.id); auto bi = br_auto_snapshot_get_info(b.get());
    CHECK(bi.is_delta && bi.base_id == info.id && bi.change_count == 0);
    CHECK(element(a).id == element(b).id && element(a).revision == element(b).revision);
    s.fake.items[0].value = "new"; s.fake.items.pop_back(); s.fake.items.push_back({3, 1, "Third", ""});
    auto c = observe(s, bi.id); CHECK(br_auto_snapshot_get_info(c.get()).change_count == 3);
    int added = 0, updated = 0, removed = 0;
    for (size_t i = 0; i < 3; ++i) {
        br_auto_change v{}; CHECK(br_auto_snapshot_change(c.get(), i, &v) == BR_AUTO_OK);
        added += v.kind == BR_AUTO_ADDED; updated += v.kind == BR_AUTO_UPDATED; removed += v.kind == BR_AUTO_REMOVED;
    }
    CHECK(added == 1 && updated == 1 && removed == 1);
    auto stale_base = observe(s, info.id);
    CHECK(!br_auto_snapshot_get_info(stale_base.get()).is_delta);
}
TEST(automation_set_value_verification_keeps_public_delta_base) {
    Session s; auto snap = observe(s); auto e = element(snap);
    auto a = action(e.id, BR_AUTO_SET_VALUE); a.value = str("Привет 🌙");
    br_auto_action_result result{};
    CHECK(br_auto_act(s.p, &a, &result) == BR_AUTO_OK);
    CHECK(result.effect == BR_AUTO_DISPATCHED && result.verification == BR_AUTO_SATISFIED && s.fake.calls == 1);
    auto diff = observe(s, br_auto_snapshot_get_info(snap.get()).id);
    CHECK(br_auto_snapshot_get_info(diff.get()).is_delta && br_auto_snapshot_get_info(diff.get()).change_count == 1);
    a.value = {nullptr, 0};
    CHECK(br_auto_act(s.p, &a, &result) == BR_AUTO_OK && s.fake.items[0].value.empty());
}
TEST(automation_aba_stale_and_wrong_session_ids) {
    Session s, other; auto before = observe(s); auto e = element(before);
    s.fake.items[0].generation = 2;
    auto a = action(e.id); br_auto_action_result result{};
    CHECK(br_auto_act(s.p, &a, &result) == BR_AUTO_STALE && s.fake.calls == 0);
    CHECK(br_auto_act(other.p, &a, &result) == BR_AUTO_STALE && other.fake.calls == 0);
    CHECK(result.effect == BR_AUTO_NOT_DISPATCHED);
}
TEST(automation_revision_and_preconditions) {
    Session s; auto snap = observe(s); auto e = element(snap);
    auto a = action(e.id); a.expected_revision = e.revision; s.fake.items[0].value = "changed";
    br_auto_action_result result{};
    CHECK(br_auto_act(s.p, &a, &result) == BR_AUTO_STALE && s.fake.calls == 0);
    a.expected_revision = 0; auto before = condition(e.id); a.before = &before;
    CHECK(br_auto_act(s.p, &a, &result) == BR_AUTO_PRECONDITION_FAILED && s.fake.calls == 0);
    a.before = nullptr; s.fake.items[0].state &= ~BR_AUTO_ENABLED;
    CHECK(br_auto_act(s.p, &a, &result) == BR_AUTO_PRECONDITION_FAILED && s.fake.calls == 0);
}
TEST(automation_no_false_success_and_no_action_retry) {
    Session s; auto snap = observe(s); auto a = action(element(snap).id, BR_AUTO_SET_VALUE);
    a.value = str("ready"); a.timeout_ms = 15; s.fake.ignore_set = true;
    br_auto_action_result result{};
    CHECK(br_auto_act(s.p, &a, &result) == BR_AUTO_TIMEOUT);
    CHECK(result.effect == BR_AUTO_DISPATCHED && result.verification == BR_AUTO_UNSATISFIED && s.fake.calls == 1);
    a.kind = BR_AUTO_INVOKE;
    CHECK(br_auto_act(s.p, &a, &result) == BR_AUTO_OK);
    CHECK(result.verification == BR_AUTO_NOT_REQUESTED);
}
TEST(automation_incomplete_snapshot_never_proves_absence_or_uniqueness) {
    Session s; auto a = observe(s); s.fake.complete = false;
    auto partial = observe(s, br_auto_snapshot_get_info(a.get()).id);
    CHECK(!br_auto_snapshot_get_info(partial.get()).complete && !br_auto_snapshot_get_info(partial.get()).is_delta);
    br_auto_selector sel{}; sel.name = str("Search"); br_auto_element found{};
    CHECK(br_auto_find(partial.get(), &sel, &found) == BR_AUTO_INCOMPLETE && !found.id);
    auto c = condition(UINT64_MAX, BR_AUTO_EXISTS); c.expected = 0; uint32_t v = 99;
    CHECK(br_auto_wait(s.p, &c, 10, &v) == BR_AUTO_TIMEOUT && v == BR_AUTO_UNKNOWN);
}
TEST(automation_find_ambiguous_and_unknown_state) {
    Session s; s.fake.items[1].name = "Search";
    auto snap = observe(s); br_auto_selector sel{}; sel.name = str("Search"); br_auto_element found{};
    CHECK(br_auto_find(snap.get(), &sel, &found) == BR_AUTO_AMBIGUOUS);
    s.fake.items[1].name = "Other"; s.fake.items[0].known &= ~BR_AUTO_SELECTED;
    snap = observe(s); sel.forbidden_state = BR_AUTO_SELECTED;
    CHECK(br_auto_find(snap.get(), &sel, &found) == BR_AUTO_INCOMPLETE);
}
TEST(automation_limits_and_invalid_provider_output) {
    Session s(true, 1); br_auto_snapshot* snap = nullptr;
    CHECK(br_auto_observe(s.p, 0, &snap) == BR_AUTO_LIMIT_EXCEEDED && !snap);
    s.fake.ignore_emit_error = true;
    CHECK(br_auto_observe(s.p, 0, &snap) == BR_AUTO_LIMIT_EXCEEDED && !snap);
    Session duplicate; duplicate.fake.duplicate = true;
    CHECK(br_auto_observe(duplicate.p, 0, &snap) == BR_AUTO_PROVIDER_ERROR && !snap);
    Session bytes(true, 64, 4);
    CHECK(br_auto_observe(bytes.p, 0, &snap) != BR_AUTO_OK && !snap);
    Session invalid; invalid.fake.items[0].name = std::string("bad\0name", 8);
    CHECK(br_auto_observe(invalid.p, 0, &snap) == BR_AUTO_PROVIDER_ERROR && !snap);
    invalid.fake.items[0].name = std::string("\xc0\xaf", 2);
    CHECK(br_auto_observe(invalid.p, 0, &snap) == BR_AUTO_PROVIDER_ERROR && !snap);
}
TEST(automation_provider_exceptions_and_ambiguous_effects) {
    Session s; auto snap = observe(s); auto a = action(element(snap).id); br_auto_action_result r{};
    s.fake.throw_act = true;
    CHECK(br_auto_act(s.p, &a, &r) == BR_AUTO_PROVIDER_ERROR && r.effect == BR_AUTO_EFFECT_UNKNOWN);
    s.fake.throw_act = false; s.fake.bad_effect = true;
    CHECK(br_auto_act(s.p, &a, &r) == BR_AUTO_PROVIDER_ERROR && r.effect == BR_AUTO_EFFECT_UNKNOWN);
    s.fake.bad_effect = false; s.fake.throw_read = true; br_auto_snapshot* raw = nullptr;
    CHECK(br_auto_observe(s.p, 0, &raw) == BR_AUTO_PROVIDER_ERROR && !raw);
    CHECK(element(snap).id != 0);
}
TEST(automation_read_only_dry_run_and_policy) {
    Session s(false); auto snap = observe(s); auto a = action(element(snap).id); br_auto_action_result r{};
    CHECK(br_auto_act(s.p, &a, &r) == BR_AUTO_DENIED && !s.fake.calls);
    Session writable; auto w = observe(writable); a = action(element(w).id); a.dry_run = 1;
    CHECK(br_auto_act(writable.p, &a, &r) == BR_AUTO_OK);
    CHECK(r.effect == BR_AUTO_NOT_DISPATCHED && r.verification == BR_AUTO_NOT_REQUESTED && !writable.fake.calls);
    Fake f; auto p = f.provider(); auto o = br_auto_options_default(); o.allow_actions = 1;
    o.authorize = [](void*, const br_auto_action*, const br_auto_element*) { return BR_AUTO_DENIED; };
    br_auto_session* custom = nullptr; CHECK(br_auto_session_create(&p, &o, &custom) == BR_AUTO_OK);
    br_auto_selector sel{}; sel.name = str("Search"); a.target = {0, &sel}; a.dry_run = 0;
    CHECK(br_auto_act(custom, &a, &r) == BR_AUTO_DENIED && !f.calls);
    br_auto_session_destroy(custom);
}
TEST(automation_batch_dynamic_selectors_and_fail_stop) {
    Session s; br_auto_selector sel{}; sel.name = str("Search");
    br_auto_step steps[3]{};
    steps[0].kind = BR_AUTO_STEP_ACT; steps[0].action = br_auto_action_default(BR_AUTO_SET_VALUE);
    steps[0].action.target.selector = &sel; steps[0].action.value = str("ready");
    steps[1].kind = BR_AUTO_STEP_ASSERT; steps[1].condition.target.selector = &sel;
    steps[1].condition.property = BR_AUTO_VALUE_EQUALS; steps[1].condition.value = str("ready");
    steps[2] = steps[0]; steps[2].action.kind = BR_AUTO_INVOKE;
    br_auto_action_result results[3]{}; br_auto_batch_result r{};
    CHECK(br_auto_batch(s.p, steps, 3, 500, results, 3, &r) == BR_AUTO_OK);
    CHECK(r.completed_steps == 3 && r.failed_index == SIZE_MAX && s.fake.calls == 2);
    steps[1].condition.value = str("wrong"); s.fake.calls = 0;
    CHECK(br_auto_batch(s.p, steps, 3, 500, results, 3, &r) == BR_AUTO_PRECONDITION_FAILED);
    CHECK(r.completed_steps == 1 && r.failed_index == 1 && s.fake.calls == 1);
    CHECK(results[2].effect == BR_AUTO_NOT_DISPATCHED && results[2].status == BR_AUTO_SKIPPED);
}
TEST(automation_batch_prevalidates_entire_input) {
    Session s; auto snap = observe(s); br_auto_step steps[2]{};
    steps[0].kind = BR_AUTO_STEP_ACT; steps[0].action = action(element(snap).id);
    steps[1] = steps[0]; steps[1].action.target = {};
    br_auto_action_result results[2]{}; br_auto_batch_result r{};
    CHECK(br_auto_batch(s.p, steps, 2, 100, results, 2, &r) == BR_AUTO_INVALID_ARGUMENT && !s.fake.calls);
    steps[1] = steps[0];
    CHECK(br_auto_batch(s.p, steps, 2, 100, results, 1, &r) == BR_AUTO_INVALID_ARGUMENT && !s.fake.calls);
}
TEST(automation_cooperative_cancel_and_concurrent_busy) {
    Session s; auto snap = observe(s); auto c = condition(element(snap).id);
    const int old_reads = s.fake.reads.load(); uint32_t verification = 0; br_auto_status status = BR_AUTO_OK;
    std::thread worker([&] { status = br_auto_wait(s.p, &c, 1000, &verification); });
    while (s.fake.reads.load() <= old_reads) std::this_thread::yield();
    br_auto_snapshot* concurrent = nullptr;
    CHECK(br_auto_observe(s.p, 0, &concurrent) == BR_AUTO_BUSY && !concurrent);
    br_auto_cancel(s.p); worker.join();
    CHECK(status == BR_AUTO_CANCELLED);
    CHECK(br_auto_observe(s.p, 0, &concurrent) == BR_AUTO_CANCELLED);
    CHECK(br_auto_reset_cancel(s.p) == BR_AUTO_OK);
    CHECK(br_auto_observe(s.p, 0, &concurrent) == BR_AUTO_OK); br_auto_snapshot_destroy(concurrent);
}
TEST(automation_optional_event_wait_and_recheck) {
    Fake f; auto p = f.provider(); p.wait_event = &Fake::event; br_auto_session* s = nullptr;
    CHECK(br_auto_session_create(&p, nullptr, &s) == BR_AUTO_OK);
    br_auto_selector sel{}; sel.name = str("Search"); br_auto_condition c{};
    c.target.selector = &sel; c.property = BR_AUTO_VALUE_EQUALS; c.value = str("ready"); uint32_t v = 0;
    CHECK(br_auto_wait(s, &c, 500, &v) == BR_AUTO_OK && v == BR_AUTO_SATISFIED);
    CHECK(f.event_waits == 1 && f.reads == 2);
    br_auto_session_destroy(s);
}
TEST(automation_input_contract_and_unknown_property) {
    Session s; auto snap = observe(s); auto a = action(element(snap).id); br_auto_action_result r{};
    br_auto_selector sel{}; a.target.selector = &sel;
    CHECK(br_auto_act(s.p, &a, &r) == BR_AUTO_INVALID_ARGUMENT);
    a.target.selector = nullptr; a.struct_size = 0;
    CHECK(br_auto_act(s.p, &a, &r) == BR_AUTO_INVALID_ARGUMENT);
    auto c = condition(element(snap).id, BR_AUTO_IS_FOCUSED); uint32_t v = 0;
    CHECK(br_auto_wait(s.p, &c, 10, &v) == BR_AUTO_TIMEOUT && v == BR_AUTO_UNKNOWN);
    CHECK(br_auto_operation_status(nullptr) == BR_AUTO_INVALID_ARGUMENT);
    CHECK(br_auto_operation_remaining_ms(nullptr) == 0);
}

#include <br/br_automation.hpp>
#include <br/br_automation_win.h>
TEST(automation_cpp20_raii_and_native_factory_contract) {
    Fake f;
    {
        auto p = f.provider(); br::AutomationSession session(p);
        auto first = session.observe(); br::AutomationSnapshot moved(std::move(first));
        CHECK(!first.get() && moved.element(0).id);
        auto a = action(moved.element(0).id); CHECK(session.act(a).status == BR_AUTO_DENIED);
        auto options = br_auto_windows_options_default(1);
        CHECK(options.struct_size == sizeof(options) && options.window == 1);
        br_auto_session* invalid = nullptr;
        options.struct_size = 0;
        CHECK(br_auto_windows_open(&options, nullptr, &invalid) == BR_AUTO_INVALID_ARGUMENT && !invalid);
#if !defined(_WIN32) || !defined(BR_HAS_WINDOWS_AUTOMATION)
        options = br_auto_windows_options_default(1);
        CHECK(br_auto_windows_open(&options, nullptr, &invalid) == BR_AUTO_UNSUPPORTED && !invalid);
#endif
    }
    CHECK(f.destroys == 1);
}
TEST(automation_read_fast_path_validates_identity_and_emit_count) {
    Fake f; auto p = f.provider();
    p.read = [](void* u, uint64_t id, uint64_t generation, const br_auto_operation*, br_auto_emit_fn emit, void* sink) {
        auto& fake = *static_cast<Fake*>(u);
        for (const auto& item : fake.items) if (item.native == id && item.generation == generation) {
            br_auto_node n{}; n.native_id = item.native; n.incarnation = item.generation;
            n.role = str("control"); n.name = {item.name.data(), item.name.size()};
            n.value = {item.value.data(), item.value.size()}; n.known = item.known; n.state = item.state;
            n.bounds = {-100, 20, 40, 20};
            n.actions = BR_AUTO_ACTION_BIT(BR_AUTO_SET_VALUE) | BR_AUTO_ACTION_BIT(BR_AUTO_INVOKE) | BR_AUTO_ACTION_BIT(BR_AUTO_SELECT);
            return emit(sink, &n);
        }
        return BR_AUTO_STALE;
    };
    auto o = br_auto_options_default(); o.allow_actions = 1; br::AutomationSession session(p, o);
    auto snapshot = session.observe(); const auto id = snapshot.element(0).id;
    auto a = action(id, BR_AUTO_SET_VALUE); a.value = str("ready");
    auto result = session.act(a);
    CHECK(result.status == BR_AUTO_OK && result.verification == BR_AUTO_SATISFIED);
    CHECK(f.reads == 1 && f.calls == 1);
    f.items[0].generation++;
    CHECK(session.act(a).status == BR_AUTO_STALE && f.calls == 1);
}
TEST(automation_read_error_cannot_report_verified_and_bad_batch_never_retries) {
    Fake f; auto p = f.provider();
    p.read = [](void*, uint64_t, uint64_t, const br_auto_operation*, br_auto_emit_fn, void*) { return BR_AUTO_OK; };
    auto o = br_auto_options_default(); o.allow_actions = 1; br::AutomationSession session(p, o);
    auto snapshot = session.observe(); auto a = action(snapshot.element(0).id);
    CHECK(session.act(a).status == BR_AUTO_PROVIDER_ERROR && f.calls == 0);
    Session s; auto snap = observe(s); s.fake.fail_act = true;
    br_auto_step steps[2]{}; steps[0].kind = BR_AUTO_STEP_ACT; steps[0].action = action(element(snap).id); steps[1] = steps[0];
    br_auto_action_result result[2]{}; br_auto_batch_result batch{};
    CHECK(br_auto_batch(s.p, steps, 2, 100, result, 2, &batch) == BR_AUTO_PROVIDER_ERROR);
    CHECK(s.fake.calls == 1 && result[0].effect == BR_AUTO_DISPATCHED && result[1].status == BR_AUTO_SKIPPED);
}

TEST(automation_named_selection_verifies_value_and_prevalidates_empty_name) {
    Fake f; f.named = true; auto p = f.provider();
    p.actions |= BR_AUTO_ACTION_BIT(BR_AUTO_SELECT_NAMED);
    auto o = br_auto_options_default(); o.allow_actions = 1; o.default_timeout_ms = 30;
    br::AutomationSession session(p, o); auto snap = session.observe();
    auto a = action(snap.element(0).id, BR_AUTO_SELECT_NAMED); a.value = str("History");
    auto r = session.act(a);
    CHECK(r.status == BR_AUTO_OK && r.effect == BR_AUTO_DISPATCHED && r.verification == BR_AUTO_SATISFIED);
    CHECK(f.calls == 1 && f.items[0].value == "History");
    a.value = {}; r = session.act(a);
    CHECK(r.status == BR_AUTO_INVALID_ARGUMENT && r.effect == BR_AUTO_NOT_DISPATCHED && f.calls == 1);
    a.value = str("Details"); f.ignore_set = true; r = session.act(a);
    CHECK(r.status == BR_AUTO_TIMEOUT && r.effect == BR_AUTO_DISPATCHED && r.verification == BR_AUTO_UNSATISFIED);
    CHECK(f.calls == 2);
}

TEST(automation_addressability_is_explicit_and_selector_unknown_is_not_false) {
    Session s; s.fake.items[0].known |= BR_AUTO_ADDRESSABLE;
    s.fake.items[0].state |= BR_AUTO_ADDRESSABLE;
    s.fake.items[1].known |= BR_AUTO_ADDRESSABLE;
    auto snap = observe(s); br_auto_selector sel{}; sel.required_state = BR_AUTO_ADDRESSABLE;
    br_auto_element found{};
    CHECK(br_auto_find(snap.get(), &sel, &found) == BR_AUTO_OK && found.id == element(snap).id);
    s.fake.items[1].known &= ~BR_AUTO_ADDRESSABLE; snap = observe(s);
    CHECK(br_auto_find(snap.get(), &sel, &found) == BR_AUTO_INCOMPLETE && !found.id);
}
