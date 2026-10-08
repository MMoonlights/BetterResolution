#pragma once
#include <br/br_automation.h>
#include <bit>
#include <cstring>
#include <vector>

namespace br::automation::wire {
constexpr uint32_t version = 1, max_bytes = 64u * 1024u * 1024u;
enum Command : uint32_t { init = 1, observe = 2, read = 3, act = 4 };
struct Error { br_auto_status status; };
struct Writer {
    std::vector<unsigned char> bytes;
    size_t limit;
    explicit Writer(size_t cap) : limit(cap) {}
    void reserve(size_t n) {
        if (n > limit - bytes.size()) throw Error{BR_AUTO_LIMIT_EXCEEDED};
    }
    void u32(uint32_t n) {
        reserve(4); const auto begin = bytes.size(); bytes.resize(begin + 4);
        for (unsigned i = 0; i < 4; ++i) bytes[begin + i] = static_cast<unsigned char>(n >> (8 * i));
    }
    void u64(uint64_t n) { u32(static_cast<uint32_t>(n)); u32(static_cast<uint32_t>(n >> 32)); }
    void string(br_auto_string s) {
        if (s.size > UINT32_MAX || (!s.data && s.size)) throw Error{BR_AUTO_PROVIDER_ERROR};
        reserve(s.size + 4); u32(static_cast<uint32_t>(s.size));
        if (s.size) bytes.insert(bytes.end(), s.data, s.data + s.size);
    }
    void node(const br_auto_node& n) {
        u64(n.native_id); u64(n.incarnation); u64(n.parent_native_id);
        u32(n.known); u32(n.state); u64(n.actions);
        u32(std::bit_cast<uint32_t>(n.bounds.x)); u32(std::bit_cast<uint32_t>(n.bounds.y));
        u32(std::bit_cast<uint32_t>(n.bounds.width)); u32(std::bit_cast<uint32_t>(n.bounds.height));
        string(n.role); string(n.name); string(n.automation_id); string(n.value);
    }
};
struct Reader {
    const unsigned char* data;
    size_t size, pos = 0;
    Reader(const void* p, size_t n) : data(static_cast<const unsigned char*>(p)), size(n) {}
    void need(size_t n) const { if (n > size - pos) throw Error{BR_AUTO_PROVIDER_ERROR}; }
    uint32_t u32() {
        need(4); uint32_t n = 0;
        for (unsigned i = 0; i < 4; ++i) n |= uint32_t(data[pos++]) << (8 * i);
        return n;
    }
    uint64_t u64() { const uint64_t low = u32(); return low | (uint64_t(u32()) << 32); }
    br_auto_string string() {
        const auto n = u32(); need(n);
        const br_auto_string s{reinterpret_cast<const char*>(data + pos), n}; pos += n; return s;
    }
    br_auto_node node() {
        br_auto_node n{};
        n.native_id = u64(); n.incarnation = u64(); n.parent_native_id = u64();
        n.known = u32(); n.state = u32(); n.actions = u64();
        n.bounds.x = std::bit_cast<int32_t>(u32()); n.bounds.y = std::bit_cast<int32_t>(u32());
        n.bounds.width = std::bit_cast<int32_t>(u32()); n.bounds.height = std::bit_cast<int32_t>(u32());
        n.role = string(); n.name = string(); n.automation_id = string(); n.value = string(); return n;
    }
    void end() const { if (pos != size) throw Error{BR_AUTO_PROVIDER_ERROR}; }
};
inline void store(void* mapping, const Writer& w) {
    auto p = static_cast<unsigned char*>(mapping);
    const auto n = static_cast<uint32_t>(w.bytes.size());
    for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<unsigned char>(n >> (8 * i));
    if (n) std::memcpy(p + 4, w.bytes.data(), n);
}
inline std::vector<unsigned char> load(const void* mapping, uint32_t cap) {
    Reader header(mapping, 4); const auto n = header.u32();
    if (!n || n > cap) throw Error{BR_AUTO_PROVIDER_ERROR};
    const auto p = static_cast<const unsigned char*>(mapping) + 4;
    return {p, p + n};
}
}
