#include "test_framework.hpp"
#include "automation/windows_wire.hpp"
#include <br/br_automation_win.h>
#include <cstring>

namespace {
using namespace br::automation::wire;
template<class F> bool rejects(F&& fn, br_auto_status expected) {
    try { fn(); return false; } catch (const Error& e) { return e.status == expected; }
}
}
TEST(automation_wire_preserves_identity_unicode_and_signed_geometry) {
    br_auto_node n{};
    const char name[] = "\xD0\xA2\xD0\xB5\xD1\x81\xD1\x82 \xF0\x9F\x8C\x99";
    n.native_id = UINT64_MAX - 7; n.incarnation = UINT64_C(0x123456789abcdef0); n.parent_native_id = 987;
    n.known = 127; n.state = 65; n.actions = BR_AUTO_ACTION_BIT(BR_AUTO_SELECT_NAMED);
    n.bounds = {-1800, -20, 400, 15}; n.role = {"edit", 4}; n.name = {name, sizeof(name) - 1};
    Writer w(4096); w.node(n); Reader r(w.bytes.data(), w.bytes.size()); auto decoded = r.node(); r.end();
    CHECK(decoded.native_id == n.native_id && decoded.incarnation == n.incarnation && decoded.parent_native_id == 987);
    CHECK(decoded.bounds.x == -1800 && decoded.bounds.y == -20 && decoded.bounds.width == 400);
    CHECK(decoded.known == n.known && decoded.state == n.state && decoded.actions == n.actions);
    CHECK(decoded.name.size == sizeof(name) - 1 && !std::memcmp(decoded.name.data, name, sizeof(name) - 1));
    CHECK(decoded.value.size == 0);
    for (size_t i = 0; i < w.bytes.size(); ++i) {
        CHECK(rejects([&] { Reader short_input(w.bytes.data(), i); short_input.node(); }, BR_AUTO_PROVIDER_ERROR));
    }
    w.u32(1); Reader extra(w.bytes.data(), w.bytes.size()); extra.node();
    CHECK(rejects([&] { extra.end(); }, BR_AUTO_PROVIDER_ERROR));
}
TEST(automation_wire_caps_payloads_and_rejects_corrupt_lengths) {
    Writer w(8); w.u64(1);
    CHECK(rejects([&] { w.u32(2); }, BR_AUTO_LIMIT_EXCEEDED));
    Writer small(7);
    CHECK(rejects([&] { small.string({"four", 4}); }, BR_AUTO_LIMIT_EXCEEDED));
    unsigned char bad[] = {255, 255, 255, 255};
    CHECK(rejects([&] { Reader r(bad, sizeof(bad)); r.string(); }, BR_AUTO_PROVIDER_ERROR));
    CHECK(rejects([&] { load(bad, 4096); }, BR_AUTO_PROVIDER_ERROR));
    unsigned char zero[4]{};
    CHECK(rejects([&] { load(zero, 4096); }, BR_AUTO_PROVIDER_ERROR));
    Writer valid(32); valid.u64(UINT64_MAX); unsigned char map[36]{}; store(map, valid);
    auto copied = load(map, 32); CHECK(copied == valid.bytes);
}
TEST(automation_isolated_api_validates_c_options_without_launching) {
    auto options = br_auto_windows_isolated_options_default(1, {"helper", 6});
    CHECK(options.struct_size == sizeof(options) && options.abi_version == BR_AUTO_ABI_VERSION);
    br_auto_session* session = nullptr;
    options.max_ipc_bytes = 4095;
    CHECK(br_auto_windows_open_isolated(&options, nullptr, &session) == BR_AUTO_INVALID_ARGUMENT && !session);
    options.max_ipc_bytes = max_bytes + 1;
    CHECK(br_auto_windows_open_isolated(&options, nullptr, &session) == BR_AUTO_INVALID_ARGUMENT);
    options.max_ipc_bytes = 4096; options.target.max_depth = 129;
    CHECK(br_auto_windows_open_isolated(&options, nullptr, &session) == BR_AUTO_INVALID_ARGUMENT);
    options.target.max_depth = 32; options.startup_timeout_ms = 0;
    CHECK(br_auto_windows_open_isolated(&options, nullptr, &session) == BR_AUTO_INVALID_ARGUMENT);
    CHECK(br_auto_windows_open_isolated(nullptr, nullptr, &session) == BR_AUTO_INVALID_ARGUMENT);
}
