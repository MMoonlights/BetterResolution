#include "codecs/huffman.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <stdexcept>

namespace br::codec {

void build_code_lengths(const uint32_t* freq_in, int n, int limit, uint8_t* lens) {
    constexpr int capacity = 288; // Наибольший поддерживаемый алфавит кодов.
    if (n < 2 || n > capacity || limit < 1 || limit > 16) throw std::invalid_argument("invalid Huffman alphabet");
    std::array<uint32_t, capacity> freq;
    std::copy_n(freq_in, n, freq.begin());
    std::fill(lens, lens + n, uint8_t(0));
    std::array<int, capacity> used_storage;
    size_t count = 0;
    for (int i = 0; i < n; ++i) if (freq[i]) used_storage[count++] = i;
    for (int i = 0; count < 2 && i < n; ++i) {
        if (!freq[i]) { freq[i] = 1; used_storage[count++] = i; }
    }
    if (count > (size_t(1) << limit)) throw std::invalid_argument("Huffman limit is too small");
    const std::span<int> used(used_storage.data(), count);
    std::sort(used.begin(), used.end(), [&](int a, int b) { return freq[a] != freq[b] ? freq[a] < freq[b] : a < b; });
    const int m = static_cast<int>(used.size());
    std::array<uint64_t, 2 * capacity> w;
    std::array<int, 2 * capacity> parent;
    for (int i = 0; i < m; ++i) w[static_cast<size_t>(i)] = freq[static_cast<size_t>(used[static_cast<size_t>(i)])];
    int q1 = 0, q2 = m, next = m;
    auto pick = [&]() {
        if (q1 < m && (q2 >= next || w[static_cast<size_t>(q1)] <= w[static_cast<size_t>(q2)])) return q1++;
        return q2++;
    };
    for (int k = 0; k < m - 1; ++k) {
        const int a = pick(), b = pick();
        w[static_cast<size_t>(next)] = w[static_cast<size_t>(a)] + w[static_cast<size_t>(b)];
        parent[static_cast<size_t>(a)] = parent[static_cast<size_t>(b)] = next;
        ++next;
    }
    std::array<int, 2 * capacity> depth;
    depth[static_cast<size_t>(next - 1)] = 0;
    for (int i = next - 2; i >= 0; --i) depth[static_cast<size_t>(i)] = depth[static_cast<size_t>(parent[static_cast<size_t>(i)])] + 1;
    int max_len = 0;
    for (int i = 0; i < m; ++i) {
        const int d = depth[static_cast<size_t>(i)];
        lens[used[static_cast<size_t>(i)]] = static_cast<uint8_t>(std::min(d, 255));
        max_len = std::max(max_len, d);
    }
    if (max_len <= limit) return;

    // Корректирует сумму Крафта после ограничения длин кодов.
    uint64_t kraft = 0;
    const uint64_t target = uint64_t(1) << limit;
    for (int s : used) {
        if (lens[s] > limit) lens[s] = static_cast<uint8_t>(limit);
        kraft += uint64_t(1) << (limit - lens[s]);
    }
    while (kraft > target) {
        int best = -1;
        for (int s : used) {
            if (lens[s] >= limit) continue;
            if (best < 0 || lens[s] > lens[best] || (lens[s] == lens[best] && freq[static_cast<size_t>(s)] < freq[static_cast<size_t>(best)])) best = s;
        }
        kraft -= uint64_t(1) << (limit - lens[best] - 1);
        lens[best]++;
    }
    // Достраивает дерево: zlib отклоняет неполные деревья Хаффмана.
    while (kraft < target) {
        int best = -1;
        for (int s : used) {
            if (lens[s] <= 1) continue;
            const uint64_t inc = uint64_t(1) << (limit - lens[s]);
            if (kraft + inc > target) continue;
            if (best < 0 || lens[s] > lens[best] || (lens[s] == lens[best] && freq[static_cast<size_t>(s)] > freq[static_cast<size_t>(best)])) best = s;
        }
        if (best < 0) break;
        kraft += uint64_t(1) << (limit - lens[best]);
        lens[best]--;
    }
}

}
