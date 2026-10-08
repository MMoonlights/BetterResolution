#include "test_framework.hpp"
#include "codecs/deflate.hpp"
#include "core/common.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace {
using br::Bytes;
using br::deflate::Deflater;

std::vector<uint8_t> payload(size_t n, int kind) {
    brt::Rng rng(123 + static_cast<unsigned>(kind));
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) {
        if (kind == 0) v[i] = static_cast<uint8_t>(rng.next());
        else if (kind == 1) v[i] = static_cast<uint8_t>(i % 7);
        else if (kind == 2) v[i] = 111;
        else v[i] = i > 32768 && (i % 1031) ? v[i - 32768] : static_cast<uint8_t>(rng.next());
    }
    return v;
}
void verify(const Bytes& compressed, const std::vector<uint8_t>& expected, bool framed = true) {
    Bytes out;
    // Ссылки должны отсчитываться от нового результата, а не от этого префикса.
    out.append("prefix", 6);
    size_t consumed = 0;
    CHECK_OK(br::deflate::inflate(compressed.data(), compressed.size(), framed, out, expected.size(), &consumed));
    CHECK(consumed == compressed.size());
    CHECK(out.size() == expected.size() + 6);
    if (out.size() == expected.size() + 6 && !expected.empty())
        CHECK(std::memcmp(out.data() + 6, expected.data(), expected.size()) == 0);
    CHECK(std::memcmp(out.data(), "prefix", 6) == 0);
}

struct Bits {
    Bytes bytes;
    uint64_t buf = 0;
    int count = 0;
    void put(unsigned x, int n) {
        buf |= static_cast<uint64_t>(x) << count;
        count += n;
        while (count >= 8) { bytes.push(static_cast<uint8_t>(buf)); buf >>= 8; count -= 8; }
    }
    void end() { if (count) bytes.push(static_cast<uint8_t>(buf)); }
    void huff(unsigned code, int n) {
        unsigned rev = 0;
        for (int i = 0; i < n; ++i) { rev = (rev << 1) | (code & 1); code >>= 1; }
        put(rev, n);
    }
    void literal(unsigned symbol) {
        if (symbol <= 143) huff(48 + symbol, 8);
        else if (symbol <= 255) huff(400 + symbol - 144, 9);
        else if (symbol <= 279) huff(symbol - 256, 7);
        else huff(192 + symbol - 280, 8);
    }
};
constexpr unsigned lengths[] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
constexpr int lextra[] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
constexpr unsigned distances[] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
constexpr int dextra[] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
void match(Bits& b, unsigned len, unsigned dist) {
    unsigned l = 28, d = 29;
    while (lengths[l] > len) --l;
    while (distances[d] > dist) --d;
    b.literal(257 + l); b.put(len - lengths[l], lextra[l]);
    b.huff(d, 5); b.put(dist - distances[d], dextra[d]);
}
}

TEST(deflate_streaming_and_contiguous_all_levels) {
    for (int level = 0; level <= 9; ++level) {
        for (int kind = 0; kind < 4; ++kind) {
            const auto v = payload(160137, kind);
            Bytes direct;
            Deflater d(direct, level, true);
            d.set_hint_distances(3, 1031);
            d.finish_contiguous(v.data(), v.size());
            verify(direct, v);
            Bytes streamed;
            Deflater z(streamed, level, true);
            z.set_hint_distances(3, 1031);
            size_t pos = 0;
            for (size_t chunk : {size_t(1),size_t(2),size_t(257),size_t(32769),size_t(97),size_t(1000000)}) {
                const size_t n = std::min(chunk, v.size() - pos);
                z.write(v.data() + pos, n); pos += n;
            }
            z.finish(); z.finish();
            verify(streamed, v);
        }
    }
}

TEST(deflate_empty_and_short_inputs) {
    for (int level = 0; level <= 9; ++level) {
        for (size_t n = 0; n <= 35; ++n) {
            const auto v = payload(n, 1);
            Bytes out;
            Deflater d(out, level, true);
            d.finish_contiguous(v.data(), v.size());
            verify(out, v);
        }
        Bytes out;
        Deflater d(out, level, true);
        d.finish();
        verify(out, {});
    }
}

TEST(deflate_dictionary_sync_flush_roundtrip) {
    const auto v = payload(270021, 3);
    for (int level : {0,1,2,3,4,6,9}) {
        Bytes out;
        size_t from = 0;
        const size_t ends[] = {29,65535,140071,v.size()};
        for (size_t to : ends) {
            const size_t history = std::min<size_t>(from, 32768);
            Deflater d(out, level, false);
            d.set_hint_distances(3, 1031);
            d.finish_contiguous(v.data() + from - history, to - from + history, history, to == v.size());
            from = to;
        }
        verify(out, v, false);
    }
}

TEST(inflate_overlap_all_short_distances_and_lengths) {
    // Создаёт потоки с фиксированными кодами Хаффмана независимо от нашего компрессора.
    // Длины хвостов выбраны так, чтобы пересекать границы копирования в 8 и 16 байт.
    for (unsigned dist = 1; dist <= 40; ++dist) {
        for (unsigned len = 3; len <= 258; ++len) {
            Bits b; b.put(3, 3); // Последний блок, фиксированные коды.
            std::vector<uint8_t> expected(dist + len);
            for (unsigned i = 0; i < dist; ++i) { expected[i] = static_cast<uint8_t>(i * 17 + 3); b.literal(expected[i]); }
            for (unsigned i = dist; i < dist + len; ++i) expected[i] = expected[i - dist];
            match(b, len, dist); b.literal(256); b.end();
            verify(b.bytes, expected, false);
        }
    }
    for (unsigned dist : {255u,256u,257u,4095u,8192u,32767u,32768u}) {
        Bits b; b.put(3, 3);
        auto expected = payload(dist, 0);
        for (uint8_t v : expected) b.literal(v);
        for (unsigned i = 0; i < 258; ++i) expected.push_back(expected[i]);
        match(b, 258, dist); b.literal(256); b.end();
        verify(b.bytes, expected, false);
    }
}

TEST(inflate_rejects_truncation_checksum_and_output_overflow) {
    auto v = payload(321, 0);
    Bytes compressed;
    br::deflate::zlib_compress(v.data(), v.size(), 6, compressed);
    for (size_t cut = 0; cut < compressed.size(); ++cut) {
        Bytes out;
        CHECK(br::deflate::inflate(compressed.data(), cut, true, out, v.size()) == BR_E_DECODE);
    }
    for (size_t i = compressed.size() - 4; i < compressed.size(); ++i) {
        compressed[i] ^= 1;
        Bytes out;
        CHECK(br::deflate::inflate(compressed.data(), compressed.size(), true, out, v.size()) == BR_E_DECODE);
        compressed[i] ^= 1;
    }
    Bytes out;
    CHECK(br::deflate::inflate(compressed.data(), compressed.size(), true, out, v.size() - 1) == BR_E_DECODE);
    out.clear();
    CHECK(br::deflate::inflate(nullptr, 0, false, out, 1024) == BR_E_DECODE);
    Bits bad; bad.put(3,3); match(bad,3,1); bad.literal(256); bad.end();
    out.push(42);
    CHECK(br::deflate::inflate(bad.bytes.data(), bad.bytes.size(), false, out, 1024) == BR_E_DECODE);
}

TEST(deflate_writable_window_small_and_large_rows) {
    for (int level : {0,1,2,3,4,9}) {
        for (size_t row : {size_t(1),size_t(3),size_t(5761),size_t(200003)}) {
            const auto data = payload(row * (row == 1 ? 530 : 5), 3);
            Bytes out;
            Deflater d(out,level,true);
            for (size_t at=0; at<data.size(); at+=row) {
                std::memcpy(d.prepare_write(row),data.data()+at,row);
                d.commit_write();
            }
            d.finish();
            verify(out,data);
        }
    }
}

TEST(checksums_match_independent_bytewise_reference) {
    const auto data = payload(65539,0);
    uint32_t crc = 0xffffffffu;
    uint32_t a = 1, b = 0;
    for (size_t n=0; n<=data.size(); ++n) {
        if (n < 64 || n % 127 == 0 || n == data.size()) {
            CHECK(br::crc32(data.data(),n) == ~crc);
            CHECK(br::adler32_update(1,data.data(),n) == ((b<<16)|a));
        }
        if (n == data.size()) break;
        crc ^= data[n];
        for (int k=0; k<8; ++k) crc = (crc>>1) ^ ((crc & 1) ? 0xedb88320u : 0);
        a = (a + data[n]) % 65521; b = (b + a) % 65521;
    }
    for (size_t split : {size_t(1),size_t(7),size_t(31),size_t(5552),size_t(16011)}) {
        const auto c = br::crc32(data.data(),split);
        CHECK(br::crc32_update(c,data.data()+split,data.size()-split) == ~crc);
        const auto d = br::adler32_update(1,data.data(),split);
        CHECK(br::adler32_update(d,data.data()+split,data.size()-split) == ((b<<16)|a));
    }
}
