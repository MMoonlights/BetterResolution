#include "core/io.hpp"

#include <cstdio>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace br {
namespace {

std::FILE* open_utf8(const char* path, bool write) {
#if defined(_WIN32)
    const int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    if (n <= 0) return nullptr;
    std::vector<wchar_t> w(static_cast<size_t>(n));
    MultiByteToWideChar(CP_UTF8, 0, path, -1, w.data(), n);
    return _wfopen(w.data(), write ? L"wb" : L"rb");
#else
    return std::fopen(path, write ? "wb" : "rb");
#endif
}

}

br_status read_file(const char* path, Bytes& out, size_t max_size) {
    if (!path || !*path) return fail(BR_E_INVALID_ARGUMENT, "empty path");
    std::FILE* f = open_utf8(path, false);
    if (!f) { set_errorf("cannot open '%s' for reading", path); return BR_E_IO; }
    out.clear();
    uint8_t chunk[1 << 16];
    for (;;) {
        const size_t n = std::fread(chunk, 1, sizeof(chunk), f);
        if (n) {
            if (out.size() + n > max_size) { std::fclose(f); return fail(BR_E_IO, "file too large"); }
            out.append(chunk, n);
        }
        if (n < sizeof(chunk)) break;
    }
    const bool err = std::ferror(f) != 0;
    std::fclose(f);
    if (err) { set_errorf("error reading '%s'", path); return BR_E_IO; }
    return BR_OK;
}

br_status write_file(const char* path, const uint8_t* data, size_t size) {
    if (!path || !*path) return fail(BR_E_INVALID_ARGUMENT, "empty path");
    std::FILE* f = open_utf8(path, true);
    if (!f) { set_errorf("cannot open '%s' for writing", path); return BR_E_IO; }
    const size_t n = size ? std::fwrite(data, 1, size, f) : 0;
    const bool ok = n == size && std::fclose(f) == 0;
    if (!ok) { set_errorf("error writing '%s'", path); return BR_E_IO; }
    return BR_OK;
}

size_t base64_size(size_t n) noexcept { return (n + 2) / 3 * 4; }

void base64_encode(const uint8_t* d, size_t n, char* out) noexcept {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        const uint32_t v = (uint32_t(d[i]) << 16) | (uint32_t(d[i + 1]) << 8) | d[i + 2];
        *out++ = tbl[v >> 18];
        *out++ = tbl[(v >> 12) & 63];
        *out++ = tbl[(v >> 6) & 63];
        *out++ = tbl[v & 63];
    }
    if (n - i == 1) {
        const uint32_t v = uint32_t(d[i]) << 16;
        *out++ = tbl[v >> 18];
        *out++ = tbl[(v >> 12) & 63];
        *out++ = '=';
        *out++ = '=';
    } else if (n - i == 2) {
        const uint32_t v = (uint32_t(d[i]) << 16) | (uint32_t(d[i + 1]) << 8);
        *out++ = tbl[v >> 18];
        *out++ = tbl[(v >> 12) & 63];
        *out++ = tbl[(v >> 6) & 63];
        *out++ = '=';
    }
}

}
