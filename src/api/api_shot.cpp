#include <br/br_shot.h>

#include "api/capture_internal.hpp"
#include "api/context.hpp"
#include "core/common.hpp"
#include "core/frame.hpp"
#include "platform/capture.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

using namespace br;

namespace {

constexpr size_t kMaxCached = 4;
constexpr uint32_t kDefaultTimeoutMs = 2000;
constexpr uint32_t kDefaultSettleTimeoutMs = 1000;
constexpr uint32_t kDefaultKeepAliveMs = 30000;

struct Entry {
    br_capture_options key{};
    br_capture* cap{};
    br_image scaled{}; // Повторно используется между вызовами, если размер не изменился.
    uint64_t last_us{0};
    uint64_t keep_us{0};
    Bytes encoded, encoded_pixels;
    br_encode_options encoded_options{};
    uint32_t encoded_width{0}, encoded_height{0};
    br_color_space encoded_color_space{};
    uint8_t encoded_premultiplied{0};
    Entry() = default;
    Entry(const Entry&) = delete;
    Entry& operator=(const Entry&) = delete;
    ~Entry() {
        if (cap) br_capture_destroy(cap);
        free_image(scaled);
    }
};

// Намеренно не освобождается: так COM-сеансы не закрываются при статическом уничтожении или выгрузке DLL.
std::mutex& cache_mutex() {
    static auto* m = new std::mutex;
    return *m;
}
std::vector<std::unique_ptr<Entry>>& cache() {
    static auto* v = new std::vector<std::unique_ptr<Entry>>;
    return *v;
}

bool same_key(const br_capture_options& a, const br_capture_options& b) {
    return a.target == b.target && a.monitor == b.monitor && a.window == b.window && a.region.x == b.region.x &&
           a.region.y == b.region.y && a.region.width == b.region.width && a.region.height == b.region.height &&
           a.backend == b.backend && a.include_cursor == b.include_cursor && a.client_area == b.client_area &&
           a.border == b.border;
}

bool same_encode(const br_encode_options& a, const br_encode_options& b) {
    // Сравнивает поля, а не заполнение публичной структуры FFI.
    return a.format == b.format && a.quality == b.quality && a.strip_metadata == b.strip_metadata &&
           a.force_444 == b.force_444 && a.png_palette == b.png_palette && a.jpeg_optimize == b.jpeg_optimize &&
           a.effort == b.effort && a.max_colors == b.max_colors;
}

bool same_pixels(const Entry& e, const br_image& img, const br_encode_options& opt) {
    if (e.encoded.empty() || !same_encode(e.encoded_options, opt) || e.encoded_width != img.width ||
        e.encoded_height != img.height || e.encoded_color_space != img.color_space ||
        e.encoded_premultiplied != img.premultiplied_alpha) return false;
    const size_t row_bytes = static_cast<size_t>(img.width) * 4;
    if (e.encoded_pixels.size() != row_bytes * img.height) return false;
    for (uint32_t y = 0; y < img.height; ++y)
        if (std::memcmp(e.encoded_pixels.data() + row_bytes * y, row_ptr(img, y), row_bytes)) return false;
    return true;
}

void remember_encoding(Entry& e, const br_encode_options& opt, const br_shot_result& r) {
    // Не более четырёх сеансов. Кэш снимков ограничен 16 МиБ пикселей и 8 МиБ закодированных данных;
    // аллокатор буфера может резервировать дополнительную память. Более крупные кадры кодируются без кэширования.
    constexpr size_t kPixelLimit = 16 * 1024 * 1024, kEncodedLimit = 8 * 1024 * 1024;
    const size_t row_bytes = static_cast<size_t>(e.scaled.width) * 4;
    const size_t bytes = row_bytes * e.scaled.height;
    if (bytes > kPixelLimit || r.size > kEncodedLimit) {
        e.encoded = Bytes{};
        e.encoded_pixels = Bytes{};
        return;
    }
    e.encoded.resize(r.size);
    std::memcpy(e.encoded.data(), r.data, r.size);
    e.encoded_pixels.resize(bytes);
    for (uint32_t y = 0; y < e.scaled.height; ++y)
        std::memcpy(e.encoded_pixels.data() + row_bytes * y, row_ptr(e.scaled, y), row_bytes);
    e.encoded_options = opt;
    e.encoded_width = e.scaled.width;
    e.encoded_height = e.scaled.height;
    e.encoded_color_space = e.scaled.color_space;
    e.encoded_premultiplied = e.scaled.premultiplied_alpha;
}

void evict_expired(std::vector<std::unique_ptr<Entry>>& c, uint64_t now) {
    c.erase(std::remove_if(c.begin(), c.end(), [&](const std::unique_ptr<Entry>& e) { return now - e->last_us > e->keep_us; }), c.end());
}

// Ждёт, пока цель не будет выдавать новые кадры в течение settle_ms, либо до истечения предельного времени.
br_status settle(br_capture* c, uint32_t first_timeout_ms, uint32_t settle_ms, uint32_t bound_ms) {
    const uint64_t settle_us = static_cast<uint64_t>(settle_ms) * 1000;
    const uint64_t start = monotonic_us();
    const uint64_t limit = start + static_cast<uint64_t>(bound_ms) * 1000;
    uint64_t quiet_since = start;
    bool first = true;
    for (;;) {
        br_frame_info fi{};
        c->dirty.clear();
        const br_status st = c->session->grab(first ? first_timeout_ms : 0, c->scratch, fi, c->dirty);
        if (st != BR_OK) return st;
        const uint64_t now = monotonic_us();
        if (first || fi.changed) quiet_since = now;
        first = false;
        if (now - quiet_since >= settle_us || now >= limit) return BR_OK;
        capture::sleep_ms(1);
    }
}

}

extern "C" {

br_shot_options br_shot_options_default(void) {
    br_shot_options o{};
    o.struct_size = sizeof(br_shot_options);
    o.capture = br_capture_options_default();
    o.resize = br_resize_options_for(BR_RESIZE_UI_TEXT);
    o.encode = br_encode_options_default(BR_ENCODE_JPEG);
    return o;
}

br_shot_options br_shot_options_for(br_shot_profile profile) {
    br_shot_options o = br_shot_options_default();
    if (profile == BR_SHOT_VISION || profile == BR_SHOT_COMPACT) {
        o.encode = br_encode_options_default(profile == BR_SHOT_VISION ? BR_ENCODE_PNG : BR_ENCODE_JPEG);
        if (profile == BR_SHOT_COMPACT) o.encode.quality = 95;
        o.flags |= BR_SHOT_REUSE_ENCODED;
    }
    return o;
}

br_status br_shot_simple(uint64_t window, uint32_t monitor, uint32_t max_width, uint32_t max_height, int32_t jpeg_quality,
                         uint32_t settle_ms, br_shot_result* out) {
    if (!out) return fail(BR_E_INVALID_ARGUMENT, "null result");
    if (jpeg_quality < 0 || jpeg_quality > 100) return fail(BR_E_INVALID_ARGUMENT, "jpeg_quality must be 0 (PNG) or 1..100");
    br_shot_options o = br_shot_options_default();
    if (window) {
        o.capture.target = BR_TARGET_WINDOW;
        o.capture.window = window;
    } else {
        o.capture.target = BR_TARGET_MONITOR;
        o.capture.monitor = monitor;
    }
    o.fit.max_width = max_width;
    o.fit.max_height = max_height;
    if (jpeg_quality > 0) {
        o.encode.quality = jpeg_quality;
    } else {
        o.encode = br_encode_options_default(BR_ENCODE_PNG);
    }
    o.settle_ms = settle_ms;
    return br_shot(nullptr, &o, out);
}

br_status br_shot_flat(uint32_t target, uint64_t window, uint32_t monitor, const int32_t* region, double scale, uint32_t max_width,
                       uint32_t max_height, int32_t jpeg_quality, uint32_t settle_ms, uint32_t flags, uint8_t** out_data,
                       size_t* out_size, uint32_t* out_width, uint32_t* out_height, int32_t* out_screen_rect,
                       double* out_image_to_screen, uint64_t* out_timings_us, uint32_t* out_info) {
    if (!out_data || !out_size) return fail(BR_E_INVALID_ARGUMENT, "null output");
    *out_data = nullptr;
    *out_size = 0;
    if (target > static_cast<uint32_t>(BR_TARGET_DESKTOP)) return fail(BR_E_INVALID_ARGUMENT, "unknown capture target");
    if (jpeg_quality < 0 || jpeg_quality > 100) return fail(BR_E_INVALID_ARGUMENT, "jpeg_quality must be 0 (PNG) or 1..100");
    if (flags & ~static_cast<uint32_t>(BR_SHOT_NO_CACHE | BR_SHOT_REUSE_ENCODED))
        return fail(BR_E_INVALID_ARGUMENT, "br_shot_flat accepts only NO_CACHE and REUSE_ENCODED (it always returns encoded bytes)");
    br_shot_options o = br_shot_options_default();
    o.capture.target = static_cast<br_capture_target>(target);
    o.capture.window = window;
    o.capture.monitor = monitor;
    if (region) o.capture.region = br_rect_i32{region[0], region[1], region[2], region[3]};
    else if (target == BR_TARGET_REGION) return fail(BR_E_INVALID_ARGUMENT, "BR_TARGET_REGION requires a region");
    o.scale = scale;
    o.fit.max_width = max_width;
    o.fit.max_height = max_height;
    if (jpeg_quality > 0) o.encode.quality = jpeg_quality;
    else o.encode = br_encode_options_default(BR_ENCODE_PNG);
    o.settle_ms = settle_ms;
    o.flags = flags;
    br_shot_result r{};
    const br_status st = br_shot(nullptr, &o, &r);
    if (st != BR_OK) return st;
    *out_data = r.data; // Владение передаётся вызывающему коду.
    *out_size = r.size;
    r.data = nullptr;
    if (out_width) *out_width = r.width;
    if (out_height) *out_height = r.height;
    if (out_screen_rect) {
        out_screen_rect[0] = r.frame.screen_rect.x;
        out_screen_rect[1] = r.frame.screen_rect.y;
        out_screen_rect[2] = r.frame.screen_rect.width;
        out_screen_rect[3] = r.frame.screen_rect.height;
    }
    if (out_image_to_screen) {
        out_image_to_screen[0] = r.frame.image_to_screen.sx;
        out_image_to_screen[1] = r.frame.image_to_screen.sy;
        out_image_to_screen[2] = r.frame.image_to_screen.tx;
        out_image_to_screen[3] = r.frame.image_to_screen.ty;
    }
    if (out_timings_us) {
        out_timings_us[0] = r.timings.open_us;
        out_timings_us[1] = r.timings.settle_us;
        out_timings_us[2] = r.timings.capture_us;
        out_timings_us[3] = r.timings.resize_us;
        out_timings_us[4] = r.timings.encode_us;
        out_timings_us[5] = r.timings.total_us;
    }
    if (out_info) {
        out_info[0] = static_cast<uint32_t>(r.frame.backend);
        out_info[1] = r.session_reused;
        out_info[2] = r.gpu_scaled;
    }
    br_shot_result_free(&r);
    return BR_OK;
}

void br_shot_result_free(br_shot_result* r) {
    if (!r) return;
    if (r->data) br_free(r->data);
    br_image_free(&r->image);
    *r = br_shot_result{};
}

void br_shot_cache_clear(void) {
    std::vector<std::unique_ptr<Entry>> doomed;
    {
        std::lock_guard<std::mutex> lock(cache_mutex());
        doomed.swap(cache());
    }
    // Уничтожает сеансы за пределами блокировки.
}

br_status br_capture_prewarm(void) {
    return guarded([] { return capture::prewarm(); });
}

br_status br_shot(br_context* ctx, const br_shot_options* opt, br_shot_result* out) {
    if (!opt || !out) return fail(BR_E_INVALID_ARGUMENT, "null argument");
    if (opt->struct_size != sizeof(br_shot_options))
        return fail(BR_E_INVALID_ARGUMENT, "br_shot_options.struct_size mismatch (initialise with br_shot_options_default)");
    if (!(opt->scale >= 0.0 && opt->scale <= 8.0)) return fail(BR_E_INVALID_ARGUMENT, "br_shot_options.scale must be 0 (off) or in (0, 8]");
    if (opt->flags & ~static_cast<uint32_t>(BR_SHOT_NO_CACHE | BR_SHOT_RETURN_IMAGE | BR_SHOT_NO_ENCODE |
                                         BR_SHOT_CUSTOM_RESIZE | BR_SHOT_REUSE_ENCODED))
        return fail(BR_E_INVALID_ARGUMENT, "unknown br_shot flags");
    *out = br_shot_result{};
    const br_status status = guarded([&]() -> br_status {
        const uint64_t t_start = monotonic_us();
        br_context* context = resolve_context(ctx);
        const bool use_cache = !(opt->flags & BR_SHOT_NO_CACHE);
        const uint32_t timeout_ms = opt->timeout_ms ? opt->timeout_ms : kDefaultTimeoutMs;
        const uint64_t keep_us = static_cast<uint64_t>(opt->keep_alive_ms ? opt->keep_alive_ms : kDefaultKeepAliveMs) * 1000;
        const br_resize_options resize = (opt->flags & BR_SHOT_CUSTOM_RESIZE) ? opt->resize : br_resize_options_for(BR_RESIZE_UI_TEXT);

        // Удаляет используемый сеанс из кэша, чтобы другие цели могли обрабатываться параллельно.
        std::unique_ptr<Entry> entry;
        if (use_cache) {
            std::lock_guard<std::mutex> lock(cache_mutex());
            auto& c = cache();
            evict_expired(c, t_start);
            for (auto it = c.begin(); it != c.end(); ++it) {
                if (same_key((*it)->key, opt->capture)) {
                    entry = std::move(*it);
                    c.erase(it);
                    break;
                }
            }
        }
        bool reused = entry != nullptr;
        uint64_t open_us = 0;
        br_shot_timings tm{};
        GrabTimings gt;
        br_frame_info info{};

        for (int attempt = 0;; ++attempt) {
            if (!entry) {
                auto e = std::make_unique<Entry>();
                e->key = opt->capture;
                const uint64_t t = monotonic_us();
                const br_status st = open_capture(context, opt->capture, true, &e->cap);
                if (st != BR_OK) return st;
                open_us = monotonic_us() - t;
                entry = std::move(e);
                reused = false;
            }
            entry->cap->context = context;
            br_status st = BR_OK;
            if (opt->settle_ms) {
                const uint64_t t = monotonic_us();
                st = settle(entry->cap, timeout_ms, opt->settle_ms, opt->settle_timeout_ms ? opt->settle_timeout_ms : kDefaultSettleTimeoutMs);
                tm.settle_us = monotonic_us() - t;
            }
            if (st == BR_OK) st = grab_fit_timed(entry->cap, timeout_ms, &opt->fit, &resize, &entry->scaled, &info, &gt, opt->scale);
            if (st == BR_OK) break;
            // При пересоздании окна или потере устройства выполняется одна повторная попытка с новым сеансом.
            if (reused && attempt == 0 && st != BR_E_INVALID_ARGUMENT) {
                entry.reset();
                reused = false;
                continue;
            }
            return st;
        }

        br_shot_result& r = *out;
        r.width = entry->scaled.width;
        r.height = entry->scaled.height;
        r.frame = info;
        r.session_reused = reused ? 1 : 0;
        r.gpu_scaled = gt.gpu_scaled ? 1 : 0;
        tm.open_us = open_us;
        tm.capture_us = gt.capture_us;
        tm.resize_us = gt.resize_us;

        if (!(opt->flags & BR_SHOT_NO_ENCODE)) {
            const uint64_t t = monotonic_us();
            const br_image_view v = br_image_as_view(&entry->scaled);
            const bool reuse_encoding = use_cache && (opt->flags & BR_SHOT_REUSE_ENCODED);
            if (reuse_encoding && same_pixels(*entry, entry->scaled, opt->encode)) {
                Bytes copy;
                copy.append(entry->encoded.data(), entry->encoded.size());
                r.size = copy.size();
                r.data = copy.release();
                // Затраты на сравнение и копирование входят в total_us; кодировщик не запускался.
                tm.encode_us = 0;
            } else {
                const br_status st = br_encode_alloc(&v, &opt->encode, &r.data, &r.size);
                if (st != BR_OK) return st;
                tm.encode_us = monotonic_us() - t;
                if (reuse_encoding) {
                    // Ошибка выделения памяти для необязательного кэша не должна срывать успешный снимок.
                    try { remember_encoding(*entry, opt->encode, r); }
                    catch (const std::bad_alloc&) { entry->encoded = Bytes{}; entry->encoded_pixels = Bytes{}; }
                } else {
                    entry->encoded = Bytes{};
                    entry->encoded_pixels = Bytes{};
                }
            }
        }
        if (opt->flags & BR_SHOT_RETURN_IMAGE) {
            r.image = entry->scaled; // Владение передаётся вызывающему коду; при следующем вызове запись выделит память заново.
            entry->scaled = br_image{};
        }
        const uint64_t t_end = monotonic_us();
        tm.total_us = t_end - t_start;
        r.timings = tm;

        if (use_cache) {
            entry->last_us = t_end;
            entry->keep_us = keep_us;
            std::unique_ptr<Entry> evicted;
            {
                std::lock_guard<std::mutex> lock(cache_mutex());
                auto& c = cache();
                // Сохраняет один сеанс, если параллельный вызов уже поместил эту цель в кэш.
                c.erase(std::remove_if(c.begin(), c.end(), [&](const std::unique_ptr<Entry>& e) { return same_key(e->key, entry->key); }), c.end());
                if (c.size() >= kMaxCached) {
                    auto oldest = std::min_element(c.begin(), c.end(), [](const auto& a, const auto& b) { return a->last_us < b->last_us; });
                    evicted = std::move(*oldest);
                    c.erase(oldest);
                }
                c.push_back(std::move(entry));
            }
        }
        return BR_OK;
    });
    if (status != BR_OK) br_shot_result_free(out);
    return status;
}

}
