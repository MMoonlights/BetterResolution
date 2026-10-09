#include <br/br_observation.h>
#include "observation_internal.hpp"
#include "core/common.hpp"
#include "core/frame.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace {
constexpr size_t max_bytes = 64u * 1024u * 1024u;
struct Item {
    uint64_t id;
    uint32_t source, kind;
    std::string text, label;
    br_rect_i32 bounds;
    double confidence;
    br_observation_item view() const {
        return {id, source, kind, {text.data(), text.size()}, {label.data(), label.size()}, bounds, confidence};
    }
};
struct Frame {
    br_image image{};
    ~Frame() { br_image_free(&image); }
};
bool valid_map(br_transform t) {
    return std::isfinite(t.sx) && std::isfinite(t.sy) && std::isfinite(t.tx) && std::isfinite(t.ty) && t.sx && t.sy;
}
br_rect_i32 clip(br_rect_i32 b, uint32_t w, uint32_t h, uint32_t padding = 0) {
    const int64_t l = std::clamp(int64_t(b.x) - padding, int64_t(0), int64_t(w));
    const int64_t t = std::clamp(int64_t(b.y) - padding, int64_t(0), int64_t(h));
    const int64_t r = std::clamp(int64_t(b.x) + b.width + padding, int64_t(0), int64_t(w));
    const int64_t d = std::clamp(int64_t(b.y) + b.height + padding, int64_t(0), int64_t(h));
    return {int32_t(l), int32_t(t), int32_t(std::max(int64_t(0), r-l)), int32_t(std::max(int64_t(0), d-t))};
}
void quote(std::string& out, std::string_view text) {
    constexpr char hex[] = "0123456789abcdef";
    out += '"';
    for (const unsigned char c : text) {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += char(c);
    }
    out += '"';
}
void number(std::string& out, double value) {
    char buf[64]; auto r = std::to_chars(buf, buf+sizeof(buf), value, std::chars_format::general);
    if (r.ec != std::errc()) br::raise(BR_E_INTERNAL, "number conversion failed");
    out.append(buf, r.ptr);
}
br_status copy_string(const std::string& text, char* output, size_t capacity, size_t* size) {
    if (!size) return br::fail(BR_E_INVALID_ARGUMENT, "null output size");
    *size = text.size() + 1;
    if (!output) return BR_OK;
    if (capacity < *size) return br::fail(BR_E_BUFFER_TOO_SMALL, "observation buffer too small");
    std::memcpy(output, text.c_str(), *size); return BR_OK;
}
}
struct br_observation {
    std::vector<Item> items;
    std::shared_ptr<Frame> frame;
    br_transform map{1,1,0,0};
    uint32_t complete=1;
    mutable std::once_flag text_once, json_once;
    mutable std::string text_cache, json_cache;
};

bool br::observe::valid_options(const br_observation_options& o) {
    return o.struct_size == sizeof(o) && o.version == BR_OBSERVATION_VERSION && o.complete<=1 && o.max_items &&
        o.max_items <= 1000000 && o.max_text_bytes && o.max_text_bytes <= max_bytes && o.max_pixels && o.max_pixels <= max_bytes;
}
bool br::observe::valid_string(br_observation_string s, size_t cap) {
    if ((!s.data && s.size) || s.size > cap) return false;
    for (size_t i = 0; i < s.size;) {
        const auto c = static_cast<unsigned char>(s.data[i++]);
        if (!c) return false;
        if (c < 128) continue;
        uint32_t cp = 0; size_t n = 0;
        if (c >= 0xc2 && c <= 0xdf) { cp = c & 31; n = 1; }
        else if (c >= 0xe0 && c <= 0xef) { cp = c & 15; n = 2; }
        else if (c >= 0xf0 && c <= 0xf4) { cp = c & 7; n = 3; }
        else return false;
        if (n > s.size-i) return false;
        for (size_t j = 0; j < n; ++j) {
            const auto d = static_cast<unsigned char>(s.data[i++]);
            if ((d & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (d & 63);
        }
        if ((n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
    }
    return true;
}

extern "C" {
br_observation_options br_observation_options_default() {
    return {sizeof(br_observation_options), BR_OBSERVATION_VERSION, 1, 65536, 8u*1024u*1024u, 16u*1024u*1024u};
}
br_observation_query br_observation_query_default() { return {sizeof(br_observation_query), BR_OBSERVATION_VERSION, 0, 0, {}, {}}; }
br_observation_render_options br_observation_render_options_default() {
    return {sizeof(br_observation_render_options), BR_OBSERVATION_VERSION, BR_OBSERVATION_RECT, 0xffe6194b, 2, 3, 1};
}
br_observation_ocr_options br_observation_ocr_options_default() {
    return {sizeof(br_observation_ocr_options), BR_OBSERVATION_VERSION, 0, {}, br_observation_options_default()};
}
br_status br_observation_create(const br_observation_item* items, size_t count, const br_image_view* image,
    const br_transform* map, const br_observation_options* options, br_observation** out) {
    if (!out) return br::fail(BR_E_INVALID_ARGUMENT, "null observation output");
    *out = nullptr;
    if(options && options->struct_size!=sizeof(*options)) return br::fail(BR_E_INVALID_ARGUMENT,"observation options size mismatch");
    const auto o = options ? *options : br_observation_options_default();
    const auto t = map ? *map : br_transform{1,1,0,0};
    if (!br::observe::valid_options(o) || !valid_map(t) || (!items && count) || count > o.max_items ||
        (image && (!br::validate_image(*image) || image->width > INT32_MAX || image->height > INT32_MAX ||
            uint64_t(image->width)*image->height > o.max_pixels))) return br::fail(BR_E_INVALID_ARGUMENT, "invalid observation input or limits");
    return br::guarded([&] {
        auto result = std::make_unique<br_observation>(); result->map = t; result->complete=o.complete;
        size_t bytes = 0;
        for (size_t i = 0; i < count; ++i) {
            const auto& n = items[i];
            if (n.source < BR_OBSERVATION_UIA || n.source > BR_OBSERVATION_CUSTOM || n.kind < BR_OBSERVATION_TEXT || n.kind > BR_OBSERVATION_OBJECT ||
                n.bounds.width < 0 || n.bounds.height < 0 || !std::isfinite(n.confidence) || (n.confidence != -1 && (n.confidence < 0 || n.confidence > 1)) ||
                !br::observe::valid_string(n.text, max_bytes) || !br::observe::valid_string(n.label, max_bytes))
                return br::fail(BR_E_INVALID_ARGUMENT, "invalid observation item");
            if (n.text.size > o.max_text_bytes-bytes) return br::fail(BR_E_BUFFER_TOO_SMALL, "observation text limit");
            bytes += n.text.size;
            if (n.label.size > o.max_text_bytes-bytes) return br::fail(BR_E_BUFFER_TOO_SMALL, "observation text limit");
            bytes += n.label.size;
        }
        result->items.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const auto& n = items[i];
            result->items.push_back({n.id, n.source, n.kind, n.text.size ? std::string(n.text.data,n.text.size) : "",
                n.label.size ? std::string(n.label.data,n.label.size) : "", n.bounds, n.confidence});
        }
        if (image) {
            result->frame = std::make_shared<Frame>();
            const auto st = br_image_clone(image, BR_PIXEL_UNKNOWN, &result->frame->image);
            if (st != BR_OK) return st;
        }
        *out = result.release(); return BR_OK;
    });
}
void br_observation_destroy(br_observation* r) { delete r; }
br_status br_observation_get_info(const br_observation* r, br_observation_info* out) {
    if (!r || !out) return br::fail(BR_E_INVALID_ARGUMENT, "null observation info");
    const auto& f = r->frame;
    *out = {r->items.size(), f ? f->image.width : 0, f ? f->image.height : 0, f ? 1u : 0u, r->complete, r->map}; return BR_OK;
}
br_status br_observation_item_at(const br_observation* r, size_t index, br_observation_item* out) {
    if (!r || !out) return br::fail(BR_E_INVALID_ARGUMENT, "null observation item");
    if (index >= r->items.size()) return br::fail(BR_E_NOT_FOUND, "observation index not found");
    *out = r->items[index].view(); return BR_OK;
}
br_status br_observation_select(const br_observation* r, const br_observation_query* query, br_observation** out) {
    if (!out) return br::fail(BR_E_INVALID_ARGUMENT, "null selection output");
    *out = nullptr;
    if(query && query->struct_size!=sizeof(*query)) return br::fail(BR_E_INVALID_ARGUMENT,"observation query size mismatch");
    const auto q = query ? *query : br_observation_query_default();
    if (!r || q.struct_size != sizeof(q) || q.version != BR_OBSERVATION_VERSION || q.source > BR_OBSERVATION_CUSTOM ||
        q.kind > BR_OBSERVATION_OBJECT || q.region.width < 0 || q.region.height < 0 ||
        ((q.region.width == 0) != (q.region.height == 0)) || !br::observe::valid_string(q.contains, max_bytes))
        return br::fail(BR_E_INVALID_ARGUMENT, "invalid observation query");
    return br::guarded([&] {
        auto result = std::make_unique<br_observation>(); result->frame = r->frame; result->map = r->map; result->complete=r->complete;
        const std::string_view text(q.contains.data ? q.contains.data : "", q.contains.size);
        for (const auto& n : r->items) {
            if ((q.source && q.source != n.source) || (q.kind && q.kind != n.kind) ||
                (!text.empty() && n.text.find(text) == std::string::npos && n.label.find(text) == std::string::npos)) continue;
            const auto b = n.bounds;
            if (q.region.width && q.region.height && (b.width == 0 || b.height == 0 ||
                int64_t(b.x)+b.width <= q.region.x || int64_t(q.region.x)+q.region.width <= b.x ||
                int64_t(b.y)+b.height <= q.region.y || int64_t(q.region.y)+q.region.height <= b.y)) continue;
            result->items.push_back(n);
        }
        *out = result.release(); return BR_OK;
    });
}
br_status br_observation_text(const br_observation* r, char* output, size_t capacity, size_t* size) {
    if (!r || !size) return br::fail(BR_E_INVALID_ARGUMENT, "null observation text");
    return br::guarded([&] {
        std::call_once(r->text_once,[&] {
        std::string s;
        for (const auto& n : r->items) {
            if (n.text.empty() && n.label.empty()) continue;
            if (!s.empty()) s += '\n';
            if (!n.label.empty() && n.label != n.text) { s += n.label; if (!n.text.empty()) s += ": "; }
            s += n.text;
        }
        r->text_cache = std::move(s);
        });
        return copy_string(r->text_cache, output, capacity, size);
    });
}
br_status br_observation_json(const br_observation* r, char* output, size_t capacity, size_t* size) {
    if (!r || !size) return br::fail(BR_E_INVALID_ARGUMENT, "null observation JSON");
    return br::guarded([&] {
        std::call_once(r->json_once,[&] {
        br_observation_info info{}; br_observation_get_info(r, &info);
        std::string s = std::string("{\"version\":1,\"complete\":") + (r->complete ? "true" : "false") + ",\"width\":" + std::to_string(info.width) + ",\"height\":" + std::to_string(info.height) +
            ",\"image_to_screen\":[";
        number(s,r->map.sx); s += ','; number(s,r->map.sy); s += ','; number(s,r->map.tx); s += ','; number(s,r->map.ty);
        s += "],\"items\":[";
        for (size_t i = 0; i < r->items.size(); ++i) {
            const auto& n = r->items[i]; if (i) s += ',';
            s += "{\"number\":" + std::to_string(i+1) + ",\"id\":";
            if (n.id) quote(s,std::to_string(n.id)); else s += "null";
            s += ",\"source\":"; quote(s,n.source == BR_OBSERVATION_UIA ? "uia" : n.source == BR_OBSERVATION_OCR ? "ocr" : "custom");
            s += ",\"kind\":"; quote(s,n.kind == BR_OBSERVATION_TEXT ? "text" : n.kind == BR_OBSERVATION_CONTROL ? "control" : "object");
            s += ",\"text\":"; quote(s,n.text); s += ",\"label\":"; quote(s,n.label);
            s += ",\"bounds\":[" + std::to_string(n.bounds.x) + ',' + std::to_string(n.bounds.y) + ',' +
                std::to_string(n.bounds.width) + ',' + std::to_string(n.bounds.height) + "],\"confidence\":";
            if (n.confidence == -1) s += "null"; else number(s,n.confidence); s += '}';
            if (s.size() > max_bytes) br::raise(BR_E_BUFFER_TOO_SMALL, "observation JSON limit");
        }
        s += "]}"; r->json_cache = std::move(s);
        });
        return copy_string(r->json_cache, output, capacity, size);
    });
}
br_status br_observation_image(const br_observation* r, const br_observation_render_options* options, br_image* output) {
    if (!r || !output || output->data) return br::fail(BR_E_INVALID_ARGUMENT, "invalid observation image output");
    if (!r->frame) return br::fail(BR_E_UNSUPPORTED, "observation has no image");
    if (options && (options->struct_size != sizeof(*options) || options->version != BR_OBSERVATION_VERSION ||
        (options->mark != BR_OBSERVATION_RECT && options->mark != BR_OBSERVATION_CIRCLE) || !options->thickness ||
        options->thickness > 64 || options->padding > 256 || options->labels > 1)) return br::fail(BR_E_INVALID_ARGUMENT, "invalid mark options");
    return br::guarded([&] {
        const auto src = br_image_as_view(&r->frame->image);
        Frame result;
        if (!options) {
            const auto st = br_image_clone(&src, BR_PIXEL_UNKNOWN, &result.image); if (st != BR_OK) return st;
        } else {
            auto st = br_image_create(src.width,src.height,BR_PIXEL_BGRA8,&result.image); if (st != BR_OK) return st;
            result.image.color_space = src.color_space;
            st = br_convert(&src,&result.image); if (st != BR_OK) return st;
            for (size_t i = 0; i < r->items.size(); ++i) {
                const auto bounds = r->items[i].bounds; if (!bounds.width || !bounds.height) continue;
                const auto b = clip(bounds, src.width, src.height, options->padding); if (!b.width || !b.height) continue;
                auto oval=b;
                if(options->mark==BR_OBSERVATION_CIRCLE) {
                    const int32_t w=int32_t(std::ceil(b.width*std::sqrt(2.0))), h=int32_t(std::ceil(b.height*std::sqrt(2.0)));
                    oval={b.x-(w-b.width)/2,b.y-(h-b.height)/2,w,h};
                }
                st = options->mark == BR_OBSERVATION_RECT ? br_draw_rect(&result.image,b,options->argb,int32_t(options->thickness)) :
                    br_draw_ellipse(&result.image,oval,options->argb,int32_t(options->thickness));
                if (st != BR_OK) return st;
                if (options->labels) {
                    const auto label = std::to_string(i+1);
                    const int32_t label_width=int32_t(label.size()*6+2);
                    if(b.y>=9) st=br_draw_text(&result.image,b.x,b.y-9,label.c_str(),0xffffffff,options->argb,1);
                    else if(b.x>=label_width+2) st=br_draw_text(&result.image,b.x-label_width-2,b.y,label.c_str(),0xffffffff,options->argb,1);
                    if (st != BR_OK) return st;
                }
            }
        }
        *output = result.image; result.image = {}; return BR_OK;
    });
}
br_status br_observation_crop(const br_observation* r, size_t index, uint32_t padding, br_image* output, br_transform* map) {
    if (!r || !output || output->data || padding > 256) return br::fail(BR_E_INVALID_ARGUMENT, "invalid observation crop output");
    if (index >= r->items.size()) return br::fail(BR_E_NOT_FOUND, "observation index not found");
    if (!r->frame) return br::fail(BR_E_UNSUPPORTED, "observation has no image");
    const auto b = r->items[index].bounds;
    if (!b.width || !b.height) return br::fail(BR_E_NOT_FOUND, "item has no image region");
    const auto region = clip(b,r->frame->image.width,r->frame->image.height,padding);
    if (!region.width || !region.height) return br::fail(BR_E_NOT_FOUND, "item is outside image");
    const br_transform t{r->map.sx,r->map.sy,r->map.tx + region.x*r->map.sx,r->map.ty + region.y*r->map.sy};
    if (!valid_map(t)) return br::fail(BR_E_INVALID_ARGUMENT, "crop coordinate map overflow");
    br_image_view src{}; const auto view = br_image_as_view(&r->frame->image);
    auto st = br_image_crop(&view,region,&src); if (st != BR_OK) return st;
    st = br_image_clone(&src,BR_PIXEL_UNKNOWN,output); if (st == BR_OK && map) *map = t; return st;
}
br_status br_observation_from_tsv(br_observation_string input, const br_image_view* image, const br_transform* map,
    const br_observation_options* options, br_observation** out) {
    if (!out) return br::fail(BR_E_INVALID_ARGUMENT,"null TSV output");
    *out = nullptr;
    if(options && options->struct_size!=sizeof(*options)) return br::fail(BR_E_INVALID_ARGUMENT,"TSV options size mismatch");
    const auto o = options ? *options : br_observation_options_default();
    if (!br::observe::valid_options(o) || !br::observe::valid_string(input,max_bytes)) return br::fail(BR_E_INVALID_ARGUMENT,"invalid TSV input");
    return br::guarded([&] {
        std::string_view text(input.data ? input.data : "",input.size);
        if (text.starts_with("\xef\xbb\xbf")) text.remove_prefix(3);
        std::vector<br_observation_item> nodes;
        size_t start = 0, row = 0, text_bytes = 0;
        while (start < text.size()) {
            auto end = text.find('\n',start); if (end == std::string_view::npos) end = text.size();
            auto line = text.substr(start,end-start); start = end + 1;
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (!row++) {
                if (line != "level\tpage_num\tblock_num\tpar_num\tline_num\tword_num\tleft\ttop\twidth\theight\tconf\ttext")
                    return br::fail(BR_E_INVALID_ARGUMENT,"invalid Tesseract TSV header");
                continue;
            }
            if (line.empty()) continue;
            std::string_view f[12]; size_t pos = 0;
            for (size_t i = 0; i < 11; ++i) {
                const auto tab = line.find('\t',pos);
                if (tab == std::string_view::npos) return br::fail(BR_E_INVALID_ARGUMENT,"invalid TSV row");
                f[i] = line.substr(pos,tab-pos); pos = tab+1;
            }
            f[11] = line.substr(pos);
            const auto integer = [](std::string_view v, int32_t& n) { const auto p=std::from_chars(v.data(),v.data()+v.size(),n); return p.ec==std::errc() && p.ptr==v.data()+v.size(); };
            int32_t level=0; if (!integer(f[0],level) || level < 1 || level > 5) return br::fail(BR_E_INVALID_ARGUMENT,"invalid TSV level");
            if (level != 5 || f[11].empty()) continue;
            int32_t page=0; br_rect_i32 b{}; double confidence=0;
            const auto parsed = std::from_chars(f[10].data(),f[10].data()+f[10].size(),confidence);
            if (!integer(f[1],page) || page!=1 || !integer(f[6],b.x) || !integer(f[7],b.y) || !integer(f[8],b.width) ||
                !integer(f[9],b.height) || b.width<0 || b.height<0 || parsed.ec!=std::errc() || parsed.ptr!=f[10].data()+f[10].size() ||
                !std::isfinite(confidence) || (confidence != -1 && (confidence<0 || confidence>100))) return br::fail(BR_E_INVALID_ARGUMENT,"invalid TSV word");
            if (nodes.size() >= o.max_items || f[11].size()>o.max_text_bytes-text_bytes) return br::fail(BR_E_BUFFER_TOO_SMALL,"TSV limits");
            text_bytes += f[11].size();
            nodes.push_back({0,BR_OBSERVATION_OCR,BR_OBSERVATION_TEXT,{f[11].data(),f[11].size()},{},b,confidence==-1 ? -1 : confidence/100});
        }
        if (!row) return br::fail(BR_E_INVALID_ARGUMENT,"missing TSV header");
        return br_observation_create(nodes.data(),nodes.size(),image,map,&o,out);
    });
}
}
