#include "codecs/deflate.hpp"
#include "codecs/huffman.hpp"

#include "core/simd.hpp"
#include "core/cpu.hpp"
#include <limits>

#include <algorithm>
#include <bit>
#include <cstring>
#include <vector>

namespace br::deflate {
using codec::build_code_lengths;
namespace {

constexpr int kWindow = 32768;
constexpr int64_t kWindowMask = kWindow - 1;
constexpr int kMinMatch = 3;
constexpr int kMaxMatch = 258;
constexpr int kHashBits = 15;
constexpr int kFastHashBits = 15;
constexpr size_t kBufCap = 4u * kWindow + kMaxMatch;
constexpr size_t kMaxBlockSyms = 1u << 15;

constexpr uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                   35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
                                    513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
constexpr uint8_t kClOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

struct CodeTables {
    uint8_t len_code[kMaxMatch + 1]{};
    uint8_t dist_code[512]{};
    uint8_t fixed_lit_len[288]{};
    uint16_t fixed_lit_code[288]{};
    uint8_t fixed_dist_len[30]{};
    uint16_t fixed_dist_code[30]{};
    CodeTables();
};

uint16_t reverse_bits(uint32_t v, int n) noexcept {
    uint32_t r = 0;
    for (int i = 0; i < n; ++i) { r = (r << 1) | (v & 1u); v >>= 1; }
    return static_cast<uint16_t>(r);
}

// Канонические коды Хаффмана обращаются битами для вывода от младшего бита к старшему.
void canonical_codes(const uint8_t* lens, int n, uint16_t* codes) noexcept {
    uint16_t bl_count[16]{};
    for (int i = 0; i < n; ++i) bl_count[lens[i]]++;
    bl_count[0] = 0;
    uint16_t next[16]{};
    uint32_t code = 0;
    for (int b = 1; b < 16; ++b) {
        code = (code + bl_count[b - 1]) << 1;
        next[b] = static_cast<uint16_t>(code);
    }
    for (int i = 0; i < n; ++i) {
        const int l = lens[i];
        codes[i] = l ? reverse_bits(next[l]++, l) : 0;
    }
}

CodeTables::CodeTables() {
    for (int c = 0; c < 29; ++c)
        for (int l = kLenBase[c]; l < kLenBase[c] + (1 << kLenExtra[c]) && l <= kMaxMatch; ++l) len_code[l] = static_cast<uint8_t>(c);
    len_code[kMaxMatch] = 28;
    for (int c = 0; c < 30; ++c) {
        for (int d = kDistBase[c]; d < kDistBase[c] + (1 << kDistExtra[c]); ++d) {
            const int dm1 = d - 1;
            if (dm1 < 256) dist_code[dm1] = static_cast<uint8_t>(c);
            else dist_code[256 + (dm1 >> 7)] = static_cast<uint8_t>(c);
        }
    }
    for (int i = 0; i < 288; ++i) fixed_lit_len[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
    canonical_codes(fixed_lit_len, 288, fixed_lit_code);
    for (int i = 0; i < 30; ++i) fixed_dist_len[i] = 5;
    canonical_codes(fixed_dist_len, 30, fixed_dist_code);
}

const CodeTables& tables() {
    static const CodeTables t;
    return t;
}


struct Params {
    int chain;
    int nice;
    int good;
    int max_insert;
    bool lazy;
};

constexpr Params kParams[10] = {
    {0, 0, 0, 0, false},
    {4, 16, 4, 4, false},
    {8, 32, 8, 8, false},
    {32, 64, 16, 16, false},
    {16, 32, 4, 0, true},
    {32, 64, 8, 0, true},
    {128, 128, 8, 0, true},
    {256, 128, 32, 0, true},
    {1024, 258, 32, 0, true},
    {4096, 258, 32, 0, true},
};

inline uint32_t load32(const uint8_t* p) noexcept {
    return load_le32(p);
}

inline uint32_t fast_hash(uint32_t v) noexcept { return (v * 0x9E3779B1u) >> (32 - kFastHashBits); }

inline int match_length(const uint8_t* a, const uint8_t* b, int max_len, bool simd) noexcept {
    int len = 0;
#if BR_SIMD_X86
    if (simd) while (len + 16 <= max_len) {
        const __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a + len));
        const __m128i y = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + len));
        const unsigned diff = static_cast<unsigned>(_mm_movemask_epi8(_mm_cmpeq_epi8(x, y))) ^ 0xffffu;
        if (diff) return len + std::countr_zero(diff);
        len += 16;
    }
#endif
    (void)simd;
    while (len + 8 <= max_len) {
        uint64_t x, y;
        std::memcpy(&x, a + len, 8);
        std::memcpy(&y, b + len, 8);
        const uint64_t d = x ^ y;
        if (d) {
            if constexpr (std::endian::native == std::endian::little)
                return len + (std::countr_zero(d) >> 3);
            else
                return len + (std::countl_zero(d) >> 3);
        }
        len += 8;
    }
    while (len < max_len && a[len] == b[len]) ++len;
    return len;
}

}

struct Deflater::State {
    Bytes& out;
    int level;
    bool zlib;
    Params p;
    uint32_t adler{1};
    std::vector<uint8_t> buf; // Буфер потока; непрерывный вход используется без копирования.
    const uint8_t* input{};
    size_t pending{0};
    const CodeTables& codes{tables()};
    const bool simd{cpu_features().sse2};
    uint32_t lf[286]{}, df[30]{};
    uint64_t extra_bits{0};
    int64_t base{0}, end{0}, pos{0}, emitted{0}, block_start{0};
    std::vector<int64_t> head;
    std::vector<int64_t> prev;
    std::vector<uint32_t> ftab; // Уровни 1..3: последнее смещение для хеша четырёх байтов.
    int hint[2]{0, 0};
    std::vector<uint32_t> syms;
    bool match_available{false};
    int prev_len{0}, prev_dist{0};
    uint64_t bitbuf{0};
    int bitcount{0};
    bool finished{false};

    State(Bytes& o, int lvl, bool z) : out(o), level(std::clamp(lvl, 0, 9)), zlib(z), p(kParams[std::clamp(lvl, 0, 9)]) {
        if (level >= 4) {
            head.assign(size_t(1) << kHashBits, -1);
            prev.assign(kWindow, -1);
        } else if (level >= 1) {
            ftab.assign(size_t(1) << kFastHashBits, 0);
        }
        syms.reserve(kMaxBlockSyms + 4);
        if (zlib) {
            out.push(0x78);
            out.push(level <= 1 ? 0x01 : level <= 5 ? 0x5e : level <= 6 ? 0x9c : 0xda);
        }
    }

    void put(uint32_t bits, int n) {
        bitbuf |= static_cast<uint64_t>(bits) << bitcount;
        bitcount += n;
        if (bitcount >= 32) {
            out.le32(static_cast<uint32_t>(bitbuf));
            bitbuf >>= 32;
            bitcount -= 32;
        }
    }
    void align() {
        while (bitcount > 0) {
            out.push(static_cast<uint8_t>(bitbuf));
            bitbuf >>= 8;
            bitcount = std::max(0, bitcount - 8);
        }
        bitbuf = 0;
        bitcount = 0;
    }

    inline uint32_t hash_at(int64_t at) const noexcept {
        const uint8_t* q = &input[static_cast<size_t>(at - base)];
        const uint32_t v = (uint32_t(q[0]) << 16) | (uint32_t(q[1]) << 8) | q[2];
        return (v * 0x9E3779B1u) >> (32 - kHashBits);
    }
    inline int64_t insert(int64_t at) noexcept {
        const uint32_t h = hash_at(at);
        const int64_t cand = head[h];
        prev[static_cast<size_t>(at & kWindowMask)] = cand;
        head[h] = at;
        return cand;
    }
    void longest_match(int64_t at, int64_t cand, int chain, int best, int& out_len, int& out_dist) const noexcept {
        out_len = 0;
        out_dist = 0;
        const int max_len = static_cast<int>(std::min<int64_t>(kMaxMatch, end - at));
        if (best < kMinMatch - 1) best = kMinMatch - 1;
        if (best >= max_len) return;
        const uint8_t* scan = &input[static_cast<size_t>(at - base)];
        int best_dist = 0;
        const int64_t history = std::min<int64_t>(at - base, kWindow);
        for (const int d : hint) {
            if (best >= max_len) break;
            if (d <= 0 || d > history) continue;
            const uint8_t* m = scan - d;
            if (m[best] != scan[best] || m[0] != scan[0] || m[1] != scan[1]) continue;
            const int len = match_length(scan, m, max_len, simd);
            if (len > best) {
                best = len;
                best_dist = d;
                if (len >= p.nice || len >= max_len) cand = -1;
            }
        }
        while (cand >= 0 && chain-- > 0) {
            const int64_t dist = at - cand;
            if (dist <= 0 || dist > kWindow) break;
            const uint8_t* m = &input[static_cast<size_t>(cand - base)];
            if (m[best] == scan[best] && m[0] == scan[0] && m[1] == scan[1]) {
                const int len = match_length(scan, m, max_len, simd);
                if (len > best) {
                    best = len;
                    best_dist = static_cast<int>(dist);
                    if (len >= p.nice || len >= max_len) break;
                }
            }
            const int64_t nxt = prev[static_cast<size_t>(cand & kWindowMask)];
            if (nxt >= cand) break;
            cand = nxt;
        }
        if (best_dist && best >= kMinMatch) {
            out_len = best;
            out_dist = best_dist;
        }
    }

    void emit_lit(uint8_t b) {
        syms.push_back(b);
        ++lf[b];
        emitted += 1;
    }
    void emit_match(int len, int dist) {
        const int lcode = codes.len_code[len];
        const int dm1 = dist - 1;
        const int dcode = dm1 < 256 ? codes.dist_code[dm1] : codes.dist_code[256 + (dm1 >> 7)];
        syms.push_back(0x80000000u | (static_cast<uint32_t>(dcode) << 25) |
                       (static_cast<uint32_t>(len) << 16) | static_cast<uint32_t>(dist));
        ++lf[257 + lcode];
        ++df[dcode];
        extra_bits += kLenExtra[lcode] + kDistExtra[dcode];
        emitted += len;
    }

    // Для устаревших или переполненных записей хеша проверяются расстояние в окне и совпадение байтов.
    void process_fast(bool final) {
        const int64_t limit = final ? end : end - kMaxMatch;
        const uint8_t* const b = input;
        uint32_t* const tab = ftab.data();
        const int ins_step = level == 1 ? 0 : level == 2 ? 4 : 1;
        while (pos < limit) {
            const int64_t avail = end - pos;
            const uint8_t* q = b + (pos - base);
            if (avail < 4) {
                emit_lit(*q);
                ++pos;
                continue;
            }
            const int max_len = static_cast<int>(std::min<int64_t>(kMaxMatch, avail));
            const uint32_t history = static_cast<uint32_t>(std::min<int64_t>(pos - base, kWindow));
            const uint32_t v = load32(q);
            int best = 0;
            uint32_t best_dist = 0;
            for (const int d : hint) {
                if (d <= 0 || static_cast<uint32_t>(d) > history || ((load32(q - d) ^ v) & 0xffffffu)) continue;
                if (best && (best >= max_len || q[best - d] != q[best])) continue;
                const int len = match_length(q, q - d, max_len, simd);
                if (len > best) {
                    best = len;
                    best_dist = static_cast<uint32_t>(d);
                }
            }
            uint32_t* slot = &tab[fast_hash(v)];
            const uint32_t dist = static_cast<uint32_t>(pos) - *slot;
            *slot = static_cast<uint32_t>(pos);
            if (best < max_len && dist - 1u < history && load32(q - dist) == v && q[best - static_cast<int64_t>(dist)] == q[best]) {
                const int len = match_length(q, q - dist, max_len, simd);
                if (len > best) {
                    best = len;
                    best_dist = dist;
                }
            }
            if (best >= 4 || (best == 3 && best_dist <= 16)) {
                emit_match(best, static_cast<int>(best_dist));
                const int64_t stop = pos + best;
                if (ins_step) {
                    const int64_t ins_end = std::min(stop, end - 3);
                    for (int64_t k = pos + 1; k < ins_end; k += ins_step)
                        tab[fast_hash(load32(b + (k - base)))] = static_cast<uint32_t>(k);
                }
                pos = stop;
            } else {
                emit_lit(*q);
                ++pos;
            }
            if (syms.size() >= kMaxBlockSyms) flush_block(false);
        }
    }

    void process(bool final) {
        const int64_t limit = final ? end : end - kMaxMatch;
        if (level == 0) {
            if (limit > pos) { pos = limit; emitted = limit; }
            return;
        }
        if (level <= 3) {
            process_fast(final);
            return;
        }
        while (pos < limit) {
            int cur_len = 0, cur_dist = 0;
            if (end - pos >= kMinMatch) {
                const int64_t cand = insert(pos);
                if ((cand >= 0 || hint[0] > 0) && (!p.lazy || prev_len < p.nice)) {
                    const int chain = (p.lazy && prev_len >= p.good) ? std::max(1, p.chain >> 2) : p.chain;
                    longest_match(pos, cand, chain, p.lazy ? prev_len : 0, cur_len, cur_dist);
                    if (cur_len == kMinMatch && cur_dist > 4096) cur_len = 0;
                }
            }
            if (!p.lazy) {
                if (cur_len >= kMinMatch) {
                    emit_match(cur_len, cur_dist);
                    const int64_t stop = pos + cur_len;
                    if (cur_len <= p.max_insert)
                        for (int64_t q = pos + 1; q < stop && end - q >= kMinMatch; ++q) insert(q);
                    pos = stop;
                } else {
                    emit_lit(input[static_cast<size_t>(pos - base)]);
                    ++pos;
                }
            } else if (prev_len >= kMinMatch && cur_len <= prev_len) {
                emit_match(prev_len, prev_dist);
                const int64_t stop = pos - 1 + prev_len;
                for (int64_t q = pos + 1; q < stop && end - q >= kMinMatch; ++q) insert(q);
                pos = stop;
                match_available = false;
                prev_len = 0;
            } else if (match_available) {
                emit_lit(input[static_cast<size_t>(pos - 1 - base)]);
                prev_len = cur_len;
                prev_dist = cur_dist;
                ++pos;
            } else {
                match_available = true;
                prev_len = cur_len;
                prev_dist = cur_dist;
                ++pos;
            }
            if (syms.size() >= kMaxBlockSyms) flush_block(false);
        }
    }

    void slide() {
        flush_block(false);
        const int64_t new_base = std::max(base, pos - kWindow - 1);
        const int64_t shift = new_base - base;
        if (shift <= 0) return;
        std::memmove(buf.data(), buf.data() + shift, static_cast<size_t>(end - new_base));
        base = new_base;
    }

    void write_stored(int64_t from, int64_t to, bool final) {
        do {
            const int64_t n = std::min<int64_t>(65535, to - from);
            const bool last = final && from + n >= to;
            put(last ? 1u : 0u, 1);
            put(0, 2);
            align();
            out.le16(static_cast<uint32_t>(n));
            out.le16(static_cast<uint32_t>(~n & 0xffff));
            if (n) out.append(input + static_cast<size_t>(from - base), static_cast<size_t>(n));
            from += n;
        } while (from < to);
    }

    // Резервирует до 48 бит на символ, включая дополнительные биты.
    void write_symbols(const uint8_t* ll, const uint16_t* lc, const uint8_t* dl, const uint16_t* dc) {
        const CodeTables& t = tables();
        uint32_t len_bits[kMaxMatch + 1];
        uint8_t len_n[kMaxMatch + 1];
        for (int len = kMinMatch; len <= kMaxMatch; ++len) {
            const int c = t.len_code[len];
            len_bits[len] = lc[257 + c] | (static_cast<uint32_t>(len - kLenBase[c]) << ll[257 + c]);
            len_n[len] = static_cast<uint8_t>(ll[257 + c] + kLenExtra[c]);
        }
        const size_t at = out.size();
        out.resize(at + syms.size() * 6 + 24);
        uint8_t* o = out.data() + at;
        uint64_t bb = bitbuf;
        int bc = bitcount;
        auto flush = [&] {
            if constexpr (std::endian::native == std::endian::little) std::memcpy(o, &bb, 8);
            else for (int k = 0; k < 8; ++k) o[k] = static_cast<uint8_t>(bb >> (8 * k));
            o += bc >> 3;
            bb >>= (bc & 56);
            bc &= 7;
        };
        flush();
        for (const uint32_t s : syms) {
            if (!(s & 0x80000000u)) {
                bb |= static_cast<uint64_t>(lc[s]) << bc;
                bc += ll[s];
            } else {
                const int len = static_cast<int>((s >> 16) & 0x1ff);
                const int dist = static_cast<int>(s & 0xffff);
                bb |= static_cast<uint64_t>(len_bits[len]) << bc;
                bc += len_n[len];
                const int d = static_cast<int>((s >> 25) & 31);
                bb |= static_cast<uint64_t>(dc[d] | (static_cast<uint32_t>(dist - kDistBase[d]) << dl[d])) << bc;
                bc += dl[d] + kDistExtra[d];
            }
            flush();
        }
        bb |= static_cast<uint64_t>(lc[256]) << bc;
        bc += ll[256];
        flush();
        out.resize(static_cast<size_t>(o - out.data()));
        bitbuf = bb;
        bitcount = bc;
    }

    void flush_block(bool final) {
        const int64_t from = block_start, to = emitted;
        if (level == 0 || (syms.empty() && to > from)) {
            if (to > from || final) write_stored(from, to, final);
            block_start = emitted;
            syms.clear();
            std::fill(std::begin(lf), std::end(lf), 0u);
            std::fill(std::begin(df), std::end(df), 0u);
            extra_bits = 0;
            return;
        }
        if (syms.empty() && !final) return;

        const CodeTables& t = tables();
        lf[256] = 1;

        uint8_t ll[286], dl[30];
        build_code_lengths(lf, 286, 15, ll);
        build_code_lengths(df, 30, 15, dl);
        int hlit = 286;
        while (hlit > 257 && !ll[hlit - 1]) --hlit;
        int hdist = 30;
        while (hdist > 1 && !dl[hdist - 1]) --hdist;

        uint8_t all[316];
        std::memcpy(all, ll, static_cast<size_t>(hlit));
        std::memcpy(all + hlit, dl, static_cast<size_t>(hdist));
        const int total = hlit + hdist;
        std::vector<uint16_t> rle; // (символ | дополнительные биты << 8)
        rle.reserve(static_cast<size_t>(total));
        for (int i = 0; i < total;) {
            const uint8_t cur = all[i];
            int run = 1;
            while (i + run < total && all[i + run] == cur) ++run;
            i += run;
            if (cur == 0) {
                while (run >= 11) { const int r = std::min(run, 138); rle.push_back(static_cast<uint16_t>(18 | ((r - 11) << 8))); run -= r; }
                if (run >= 3) { rle.push_back(static_cast<uint16_t>(17 | ((run - 3) << 8))); run = 0; }
                while (run-- > 0) rle.push_back(0);
            } else {
                rle.push_back(cur);
                --run;
                while (run >= 3) { const int r = std::min(run, 6); rle.push_back(static_cast<uint16_t>(16 | ((r - 3) << 8))); run -= r; }
                while (run-- > 0) rle.push_back(cur);
            }
        }
        uint32_t cf[19]{};
        for (uint16_t r : rle) cf[r & 0xff]++;
        uint8_t cl[19];
        build_code_lengths(cf, 19, 7, cl);
        int hclen = 19;
        while (hclen > 4 && !cl[kClOrder[hclen - 1]]) --hclen;

        uint64_t dyn = 3 + 5 + 5 + 4 + 3ull * static_cast<uint64_t>(hclen) + extra_bits;
        for (uint16_t r : rle) {
            const int s = r & 0xff;
            dyn += cl[s] + (s == 16 ? 2 : s == 17 ? 3 : s == 18 ? 7 : 0);
        }
        uint64_t fix = 3 + extra_bits;
        for (int i = 0; i < 286; ++i) { dyn += static_cast<uint64_t>(lf[i]) * ll[i]; fix += static_cast<uint64_t>(lf[i]) * t.fixed_lit_len[i]; }
        for (int i = 0; i < 30; ++i) { dyn += static_cast<uint64_t>(df[i]) * dl[i]; fix += static_cast<uint64_t>(df[i]) * 5u; }
        const uint64_t stored_bytes = static_cast<uint64_t>(to - from);
        const uint64_t stored_blocks = std::max<uint64_t>(1, (stored_bytes + 65534) / 65535);
        const uint64_t padding = static_cast<uint64_t>((8 - ((bitcount + 3) & 7)) & 7);
        const uint64_t stored = stored_bytes * 8 + 3 + padding + 32 + 40 * (stored_blocks - 1);
        const bool can_store = from >= base;

        if (can_store && stored <= dyn && stored <= fix) {
            write_stored(from, to, final);
        } else if (fix <= dyn) {
            put(final ? 1u : 0u, 1);
            put(1, 2);
            write_symbols(t.fixed_lit_len, t.fixed_lit_code, t.fixed_dist_len, t.fixed_dist_code);
        } else {
            uint16_t lc[286], dc[30], cc[19];
            canonical_codes(ll, 286, lc);
            canonical_codes(dl, 30, dc);
            canonical_codes(cl, 19, cc);
            put(final ? 1u : 0u, 1);
            put(2, 2);
            put(static_cast<uint32_t>(hlit - 257), 5);
            put(static_cast<uint32_t>(hdist - 1), 5);
            put(static_cast<uint32_t>(hclen - 4), 4);
            for (int i = 0; i < hclen; ++i) put(cl[kClOrder[i]], 3);
            for (uint16_t r : rle) {
                const int s = r & 0xff;
                put(cc[s], cl[s]);
                if (s == 16) put(r >> 8, 2);
                else if (s == 17) put(r >> 8, 3);
                else if (s == 18) put(r >> 8, 7);
            }
            write_symbols(ll, lc, dl, dc);
        }
        block_start = emitted;
        syms.clear();
        std::fill(std::begin(lf), std::end(lf), 0u);
        std::fill(std::begin(df), std::end(df), 0u);
        extra_bits = 0;
    }
};

Deflater::Deflater(Bytes& out, int level, bool zlib_framing) : s_(std::make_unique<State>(out, level, zlib_framing)) {}
Deflater::~Deflater() = default;

uint8_t* Deflater::prepare_write(size_t size) {
    State& s = *s_;
    if (s.finished || s.pending) raise(BR_E_INTERNAL, "invalid deflate write reservation");
    if (!size || size > static_cast<size_t>(std::numeric_limits<int64_t>::max() - s.end))
        raise(BR_E_INVALID_ARGUMENT, "invalid deflate input size");
    if (s.buf.empty()) { s.buf.resize(std::max(kBufCap, size)); s.input = s.buf.data(); }
    size_t used = static_cast<size_t>(s.end - s.base);
    if (size > s.buf.size() - used) {
        s.process(false);
        s.slide();
        used = static_cast<size_t>(s.end - s.base);
        if (size > std::numeric_limits<size_t>::max() - used) throw std::bad_alloc();
        if (size > s.buf.size() - used) { s.buf.resize(used + size); s.input = s.buf.data(); }
    }
    s.pending = size;
    return s.buf.data() + used;
}

void Deflater::commit_write() {
    State& s = *s_;
    if (!s.pending || s.finished) raise(BR_E_INTERNAL, "no pending deflate write");
    if (s.zlib) s.adler = adler32_update(s.adler, s.input + static_cast<size_t>(s.end - s.base), s.pending);
    s.end += static_cast<int64_t>(s.pending);
    s.pending = 0;
}

void Deflater::write(const uint8_t* data, size_t size) {
    State& s = *s_;
    if (s.finished || s.pending) raise(BR_E_INTERNAL, "deflater finished or write pending");
    if (!data && size) raise(BR_E_INVALID_ARGUMENT, "null deflate input");
    if (s.buf.empty()) { s.buf.resize(kBufCap); s.input = s.buf.data(); }
    if (s.zlib) s.adler = adler32_update(s.adler, data, size);
    while (size) {
        const size_t used = static_cast<size_t>(s.end - s.base);
        size_t space = s.buf.size() - used;
        if (!space) {
            s.process(false);
            s.slide();
            continue;
        }
        const size_t n = std::min(space, size);
        std::memcpy(s.buf.data() + used, data, n);
        s.end += static_cast<int64_t>(n);
        data += n;
        size -= n;
    }
}

void Deflater::set_hint_distances(int d1, int d2) {
    s_->hint[0] = d1 > 0 && d1 <= kWindow ? d1 : 0;
    s_->hint[1] = d2 > 0 && d2 <= kWindow ? d2 : 0;
    if (!s_->hint[0]) std::swap(s_->hint[0], s_->hint[1]);
}

void Deflater::finish_partial() {
    State& s = *s_;
    if (s.finished) return;
    if (s.pending) raise(BR_E_INTERNAL, "uncommitted deflate input");
    s.process(true);
    if (s.match_available) {
        s.emit_lit(s.input[static_cast<size_t>(s.pos - 1 - s.base)]);
        s.match_available = false;
    }
    s.flush_block(false);
    s.write_stored(s.emitted, s.emitted, false); // Пустой сохранённый блок выполняет синхронную очистку буфера.
    s.align();
    s.finished = true;
}

void Deflater::finish() {
    State& s = *s_;
    if (s.finished) return;
    if (s.pending) raise(BR_E_INTERNAL, "uncommitted deflate input");
    s.process(true);
    if (s.match_available) {
        s.emit_lit(s.input[static_cast<size_t>(s.pos - 1 - s.base)]);
        s.match_available = false;
    }
    s.flush_block(true);
    s.align();
    if (s.zlib) s.out.be32(s.adler);
    s.finished = true;
}

void Deflater::finish_contiguous(const uint8_t* data, size_t size, size_t history, bool final) {
    State& s = *s_;
    if (s.finished || s.end || !s.buf.empty()) raise(BR_E_INTERNAL, "contiguous input must be the first operation");
    if ((!data && size) || history > size || history > kWindow ||
        size > static_cast<size_t>(std::numeric_limits<int64_t>::max()))
        raise(BR_E_INVALID_ARGUMENT, "invalid contiguous deflate input");
    if (s.zlib && history) raise(BR_E_INVALID_ARGUMENT, "zlib preset dictionary is unsupported");
    static constexpr uint8_t empty = 0;
    s.input = data ? data : &empty;
    s.end = static_cast<int64_t>(size);
    s.pos = s.emitted = s.block_start = static_cast<int64_t>(history);
    if (s.level >= 4) {
        for (size_t q = 0; q + kMinMatch <= history; ++q) s.insert(static_cast<int64_t>(q));
    } else if (s.level >= 1) {
        for (size_t q = 0; q + 4 <= history; ++q)
            s.ftab[fast_hash(load32(s.input + q))] = static_cast<uint32_t>(q);
    }
    if (s.zlib) s.adler = adler32_update(1, s.input, size);
    if (final) finish(); else finish_partial();
    s.input = nullptr; // Перестаёт хранить указатель на входные данные.
}

void zlib_compress(const uint8_t* data, size_t size, int level, Bytes& out) {
    Deflater d(out, level, true);
    d.finish_contiguous(data, size);
}

namespace {

constexpr int kFastBits = 11;

struct Huffman {
    uint16_t fast[1 << kFastBits]; // (symbol << 4) | length; ноль включает медленный путь.
    uint16_t count[16];
    uint16_t symbols[320];

    bool build(const uint8_t* lens, int n) {
        std::memset(count, 0, sizeof(count));
        std::memset(fast, 0, sizeof(fast));
        for (int i = 0; i < n; ++i) count[lens[i]]++;
        count[0] = 0;
        int left = 1;
        for (int l = 1; l < 16; ++l) {
            left <<= 1;
            left -= count[l];
            if (left < 0) return false; // Дерево с избыточным числом кодов.
        }
        uint16_t offs[16];
        offs[1] = 0;
        for (int l = 1; l < 15; ++l) offs[l + 1] = static_cast<uint16_t>(offs[l] + count[l]);
        for (int i = 0; i < n; ++i) if (lens[i]) symbols[offs[lens[i]]++] = static_cast<uint16_t>(i);
        uint32_t code = 0;
        int idx = 0;
        for (int l = 1; l < 16; ++l) {
            for (int k = 0; k < count[l]; ++k, ++idx, ++code) {
                if (l > kFastBits) continue;
                const uint32_t rev = reverse_bits(code, l);
                const uint16_t entry = static_cast<uint16_t>((symbols[idx] << 4) | l);
                for (uint32_t f = rev; f < (1u << kFastBits); f += (1u << l)) fast[f] = entry;
            }
            code <<= 1;
        }
        return true;
    }
};

struct BitReader {
    const uint8_t* p;
    const uint8_t* end;
    uint64_t buf{0};
    int cnt{0};
    size_t overrun{0};

    void refill() noexcept {
        if (cnt <= 32 && static_cast<size_t>(end - p) >= 4) {
            buf |= static_cast<uint64_t>(load_le32(p)) << cnt;
            p += 4;
            cnt += 32;
            return;
        }
        while (cnt <= 56) {
            if (p < end) buf |= static_cast<uint64_t>(*p++) << cnt;
            else ++overrun;
            cnt += 8;
        }
    }
    uint32_t bits(int n) noexcept {
        if (cnt < n) refill();
        const uint32_t v = static_cast<uint32_t>(buf & ((uint64_t(1) << n) - 1));
        buf >>= n;
        cnt -= n;
        return v;
    }
    bool overflowed() const noexcept { return overrun * 8 > static_cast<size_t>(cnt); }
    int decode(const Huffman& h) noexcept {
        if (cnt < 16) refill();
        const uint16_t e = h.fast[buf & ((1u << kFastBits) - 1)];
        if (e) {
            const int l = e & 15;
            buf >>= l;
            cnt -= l;
            return e >> 4;
        }
        int code = 0, first = 0, index = 0;
        for (int l = 1; l < 16; ++l) {
            code |= static_cast<int>(buf & 1u);
            buf >>= 1;
            --cnt;
            const int c = h.count[l];
            if (code - first < c) return h.symbols[index + code - first];
            index += c;
            first += c;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }
};

const Huffman& fixed_lit() {
    static const Huffman h = [] {
        Huffman x{};
        uint8_t l[288];
        for (int i = 0; i < 288; ++i) l[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
        x.build(l, 288);
        return x;
    }();
    return h;
}
const Huffman& fixed_dist() {
    static const Huffman h = [] {
        Huffman x{};
        uint8_t l[30];
        for (int i = 0; i < 30; ++i) l[i] = 5;
        x.build(l, 30);
        return x;
    }();
    return h;
}

}

br_status inflate(const uint8_t* data, size_t size, bool zlib_framing, Bytes& out, size_t max_output, size_t* consumed) {
    if (!data && size) return fail(BR_E_INVALID_ARGUMENT, "null input");
    if (consumed) *consumed = 0;
    static constexpr uint8_t empty = 0;
    if (!data) data = &empty;
    const size_t start = out.size();
    BitReader br{data, data + size};
    if (zlib_framing) {
        if (size < 6) return fail(BR_E_DECODE, "zlib stream too short");
        const uint32_t cmf = data[0], flg = data[1];
        if ((cmf & 0x0f) != 8 || (cmf >> 4) > 7 || ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20))
            return fail(BR_E_DECODE, "invalid zlib header");
        br.p += 2;
    }
    auto huffs = std::make_unique<Huffman[]>(2);
    Huffman& dyn_lit = huffs[0];
    Huffman& dyn_dist = huffs[1];
    bool final = false;
    while (!final) {
        final = br.bits(1) != 0;
        const uint32_t type = br.bits(2);
        if (type == 0) {
            // Сохранённые блоки начинаются с границы байта.
            br.bits(br.cnt & 7);
            const uint32_t len = br.bits(16);
            const uint32_t nlen = br.bits(16);
            if ((len ^ 0xffffu) != nlen) return fail(BR_E_DECODE, "corrupt stored block");
            if (len > max_output - (out.size() - start)) return fail(BR_E_DECODE, "inflated data exceeds limit");
            uint32_t n = len;
            while (n && br.cnt >= 8) { out.push(static_cast<uint8_t>(br.bits(8))); --n; }
            if (n) {
                if (static_cast<size_t>(br.end - br.p) < n) return fail(BR_E_DECODE, "truncated stored block");
                out.append(br.p, n);
                br.p += n;
            }
            continue;
        }
        const Huffman* lit = nullptr;
        const Huffman* dist = nullptr;
        if (type == 1) {
            lit = &fixed_lit();
            dist = &fixed_dist();
        } else if (type == 2) {
            const int hlit = static_cast<int>(br.bits(5)) + 257;
            const int hdist = static_cast<int>(br.bits(5)) + 1;
            const int hclen = static_cast<int>(br.bits(4)) + 4;
            if (hlit > 286 || hdist > 32) return fail(BR_E_DECODE, "bad dynamic header");
            uint8_t cl[19]{};
            for (int i = 0; i < hclen; ++i) cl[kClOrder[i]] = static_cast<uint8_t>(br.bits(3));
            Huffman clh{};
            if (!clh.build(cl, 19)) return fail(BR_E_DECODE, "bad code length code");
            uint8_t lens[320]{};
            for (int i = 0; i < hlit + hdist;) {
                const int sym = br.decode(clh);
                if (sym < 0) return fail(BR_E_DECODE, "bad code length symbol");
                if (sym < 16) { lens[i++] = static_cast<uint8_t>(sym); continue; }
                int rep = 0;
                uint8_t val = 0;
                if (sym == 16) {
                    if (!i) return fail(BR_E_DECODE, "repeat without previous length");
                    val = lens[i - 1];
                    rep = 3 + static_cast<int>(br.bits(2));
                } else if (sym == 17) {
                    rep = 3 + static_cast<int>(br.bits(3));
                } else {
                    rep = 11 + static_cast<int>(br.bits(7));
                }
                if (i + rep > hlit + hdist) return fail(BR_E_DECODE, "code lengths overflow");
                while (rep--) lens[i++] = val;
            }
            if (!lens[256]) return fail(BR_E_DECODE, "missing end-of-block code");
            if (!dyn_lit.build(lens, hlit) || !dyn_dist.build(lens + hlit, hdist)) return fail(BR_E_DECODE, "bad huffman tree");
            lit = &dyn_lit;
            dist = &dyn_dist;
        } else {
            return fail(BR_E_DECODE, "invalid block type");
        }
        for (;;) {
            const int sym = br.decode(*lit);
            if (sym < 0) return fail(BR_E_DECODE, "bad literal/length code");
            if (sym < 256) {
                if (out.size() - start >= max_output) return fail(BR_E_DECODE, "inflated data exceeds limit");
                out.push(static_cast<uint8_t>(sym));
                continue;
            }
            if (sym == 256) break;
            const int li = sym - 257;
            if (li >= 29) return fail(BR_E_DECODE, "bad length symbol");
            const int len = kLenBase[li] + static_cast<int>(br.bits(kLenExtra[li]));
            const int ds = br.decode(*dist);
            if (ds < 0 || ds >= 30) return fail(BR_E_DECODE, "bad distance symbol");
            const size_t d = kDistBase[ds] + br.bits(kDistExtra[ds]);
            const size_t produced = out.size() - start;
            if (d > produced) return fail(BR_E_DECODE, "distance too far back");
            if (static_cast<size_t>(len) > max_output - produced) return fail(BR_E_DECODE, "inflated data exceeds limit");
            const size_t at = out.size();
            out.resize(at + static_cast<size_t>(len));
            uint8_t* dst = out.data() + at;
            const uint8_t* src = dst - d;
            size_t left = static_cast<size_t>(len), distance = d;
            if (distance >= left) {
                std::memcpy(dst, src, left);
            } else if (distance == 1) {
                std::memset(dst, *src, left);
            } else {
                // Добавляет короткие повторы; области каждого memcpy не должны перекрываться.
                if (distance < 8) {
                    while (distance < 8) distance *= 2;
                    const size_t seed = std::min(distance, left);
                    for (size_t k = 0; k < seed; ++k) dst[k] = src[k];
                    dst += seed;
                    left -= seed;
                }
                if (left) src = dst - distance;
                if (distance >= 16) {
                    while (left >= 16) { std::memcpy(dst, src, 16); dst += 16; src += 16; left -= 16; }
                }
                while (left >= 8) { std::memcpy(dst, src, 8); dst += 8; src += 8; left -= 8; }
                while (left--) *dst++ = *src++;
            }
        }
        if (br.overflowed()) return fail(BR_E_DECODE, "truncated deflate stream");
    }
    if (br.overflowed()) return fail(BR_E_DECODE, "truncated deflate stream");
    // Не учитывает целые байты из буфера в числе обработанных входных байтов.
    const size_t buffered = static_cast<size_t>(br.cnt / 8);
    const size_t unread = buffered - std::min(br.overrun, buffered);
    size_t used = static_cast<size_t>(br.p - data) - unread;
    if (zlib_framing) {
        if (used > size || size - used < 4) return fail(BR_E_DECODE, "truncated zlib checksum");
        const uint32_t expected = load_be32(data + used);
        const uint32_t actual = adler32_update(1, out.data() ? out.data() + start : nullptr, out.size() - start);
        if (expected != actual) return fail(BR_E_DECODE, "zlib checksum mismatch");
        used += 4;
    }
    if (consumed) *consumed = used;
    return BR_OK;
}

}
