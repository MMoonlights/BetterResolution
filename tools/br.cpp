







#include <br/br.h>
#include <br/br_shot.h>

#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <fcntl.h>
#include <io.h>
#endif

using brjson::Value;
int br_text_command(const std::vector<std::string>& args);
int br_vision_command(const std::vector<std::string>& args);
int br_read_command(const std::vector<std::string>& args);

namespace {

[[noreturn]] void die(const std::string& msg);





struct Args {
    std::vector<std::string> positional;
    std::vector<std::pair<std::string, std::string>> options; 

    bool has(const char* name) const {
        for (const auto& o : options) if (o.first == name) return true;
        return false;
    }
    std::string get(const char* name, const std::string& def = "") const {
        for (auto it = options.rbegin(); it != options.rend(); ++it)
            if (it->first == name) return it->second;
        return def;
    }
    long long num(const char* name, long long def) const {
        const std::string v = get(name);
        if (!has(name)) return def;
        char* end = nullptr;
        errno = 0;
        const auto result = std::strtoll(v.c_str(), &end, 10);
        if (v.empty() || errno == ERANGE || end != v.c_str() + v.size()) die(std::string("invalid integer for ") + name);
        return result;
    }
    double real(const char* name, double def) const {
        const std::string v = get(name);
        if (!has(name)) return def;
        char* end = nullptr;
        errno = 0;
        const auto result = std::strtod(v.c_str(), &end);
        if (v.empty() || errno == ERANGE || end != v.c_str() + v.size() || !std::isfinite(result))
            die(std::string("invalid number for ") + name);
        return result;
    }
};


const char* const kValued[] = {"-o", "--out", "--out-dir", "--suffix", "-w", "--width", "-h", "--height", "--scale",
                               "--long-edge", "--short-edge", "--max-pixels", "--multiple-of", "--filter", "--mode",
                               "--threads", "-q", "--quality", "--effort", "--palette", "--colors", "--format", "--region",
                               "--threshold", "--gap", "--step", "--monitor", "--window", "--hwnd", "--backend",
                               "--labels", "--marks", "--timeout", "--repeat", "--settle",
                               "--settle-timeout", "--keep-alive", "--profile"};

bool takes_value(const std::string& s) {
    for (const char* v : kValued) if (s == v) return true;
    return false;
}

std::string canonical(const std::string& s) {
    if (s == "-o") return "--out";
    if (s == "-w") return "--width";
    if (s == "-h") return "--height";
    if (s == "-q") return "--quality";
    return s;
}

bool parse_args(const std::vector<std::string>& in, Args& a, std::string& err) {
    for (size_t i = 0; i < in.size(); ++i) {
        const std::string& s = in[i];
        if (s == "--fit" || s.rfind("--fit=", 0) == 0) {
            err = "--fit was removed; use --width, --height, --scale or --long-edge";
            return false;
        }
        if (s.size() > 1 && s[0] == '-' && !(s.size() > 1 && (std::isdigit(static_cast<unsigned char>(s[1])) != 0))) {
            std::string name = s, value;
            const size_t eq = s.find('=');
            if (eq != std::string::npos && s.rfind("--", 0) == 0) {
                name = s.substr(0, eq);
                value = s.substr(eq + 1);
            } else if (takes_value(s)) {
                if (i + 1 >= in.size()) { err = "option " + s + " needs a value"; return false; }
                value = in[++i];
            }
            a.options.emplace_back(canonical(name), value);
        } else {
            a.positional.push_back(s);
        }
    }
    return true;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

[[noreturn]] void die(const std::string& msg) {
    std::fprintf(stderr, "br: %s\n", msg.c_str());
    std::exit(1);
}

void check(br_status st, const std::string& what) {
    if (st == BR_OK) return;
    std::string m = what + ": " + br_status_string(st);
    const char* d = br_last_error();
    if (d && *d) m += " (" + std::string(d) + ")";
    die(m);
}





br_filter filter_by_name(const std::string& s0) {
    const std::string f = lower(s0);
    if (f == "box" || f == "area") return BR_FILTER_BOX;
    if (f == "triangle" || f == "bilinear" || f == "linear") return BR_FILTER_TRIANGLE;
    if (f == "bspline") return BR_FILTER_CUBIC_BSPLINE;
    if (f == "catmull-rom" || f == "catmullrom" || f == "bicubic" || f == "cubic") return BR_FILTER_CATMULL_ROM;
    if (f == "mitchell") return BR_FILTER_MITCHELL;
    if (f == "lanczos2") return BR_FILTER_LANCZOS2;
    if (f == "lanczos" || f == "lanczos3") return BR_FILTER_LANCZOS3;
    if (f == "lanczos4") return BR_FILTER_LANCZOS4;
    if (f == "point" || f == "nearest") return BR_FILTER_POINT;
    if (f == "auto" || f.empty()) return BR_FILTER_AUTO;
    die("unknown filter '" + s0 + "' (box, triangle, bspline, catmull-rom, mitchell, lanczos2/3/4, point, auto)");
}

br_resize_options resize_options(const Args& a, br_resize_mode def_mode) {
    br_resize_mode mode = def_mode;
    if (a.has("--mode")) {
        const std::string m = lower(a.get("--mode"));
        if (m == "quality" || m == "photo") mode = BR_RESIZE_QUALITY;
        else if (m == "balanced") mode = BR_RESIZE_BALANCED;
        else if (m == "fast") mode = BR_RESIZE_FAST;
        else if (m == "text" || m == "ui") mode = BR_RESIZE_UI_TEXT;
        else die("unknown mode '" + m + "' (text, quality, balanced, fast)");
    }
    br_resize_options o = br_resize_options_for(mode);
    if (a.has("--filter")) o.filter = filter_by_name(a.get("--filter"));
    if (a.has("--linear")) o.linear_light = 1;
    if (a.has("--gamma")) o.linear_light = 0;
    if (a.has("--no-antiring")) o.antiring = 0;
    if (a.has("--no-multistage")) o.multistage = 0;
    o.threads = static_cast<uint32_t>(a.num("--threads", 0));
    return o;
}


bool size_limits_from(const Args& a, br_fit_options& f) {
    f = br_fit_options{};
    bool any = false;
    if (a.has("--long-edge")) { f.max_long_edge = static_cast<uint32_t>(a.num("--long-edge", 0)); any = true; }
    if (a.has("--short-edge")) { f.max_short_edge = static_cast<uint32_t>(a.num("--short-edge", 0)); any = true; }
    if (a.has("--max-pixels")) { f.max_pixels = static_cast<uint64_t>(a.num("--max-pixels", 0)); any = true; }
    if (a.has("--multiple-of")) { f.multiple_of = static_cast<uint32_t>(a.num("--multiple-of", 0)); any = true; }
    if (a.has("--upscale")) f.allow_upscale = 1;
    return any;
}


bool target_size(const Args& a, uint32_t w, uint32_t h, uint32_t& ow, uint32_t& oh) {
    const long long W = a.num("--width", 0), H = a.num("--height", 0);
    if (a.has("--scale")) {
        const double s = a.real("--scale", 1.0);
        if (!(s > 0)) die("--scale must be positive");
        ow = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(w * s)));
        oh = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(h * s)));
        return true;
    }
    if (W > 0 || H > 0) {
        ow = W > 0 ? static_cast<uint32_t>(W) : std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(static_cast<double>(w) * H / h)));
        oh = H > 0 ? static_cast<uint32_t>(H) : std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(static_cast<double>(h) * W / w)));
        return true;
    }
    br_fit_options f{};
    if (!size_limits_from(a, f)) return false;
    br_fit_dimensions(w, h, &f, &ow, &oh);
    return true;
}

bool format_by_name(const std::string& s0, br_encoded_format& f) {
    const std::string s = lower(s0);
    if (s == "png") f = BR_ENCODE_PNG;
    else if (s == "jpg" || s == "jpeg") f = BR_ENCODE_JPEG;
    else if (s == "bmp") f = BR_ENCODE_BMP;
    else if (s == "qoi") f = BR_ENCODE_QOI;
    else if (s == "ppm" || s == "pgm" || s == "pnm") f = BR_ENCODE_PNM;
    else return false;
    return true;
}

const char* format_ext(br_encoded_format f) {
    switch (f) {
    case BR_ENCODE_JPEG: return ".jpg";
    case BR_ENCODE_BMP: return ".bmp";
    case BR_ENCODE_QOI: return ".qoi";
    case BR_ENCODE_PNM: return ".ppm";
    default: return ".png";
    }
}

br_encoded_format format_for_path(const std::string& path, const Args& a, bool require_extension = false) {
    br_encoded_format f;
    if (a.has("--format")) {
        if (!format_by_name(a.get("--format"), f)) die("unknown --format '" + a.get("--format") + "'");
        return f;
    }
    const size_t dot = path.find_last_of('.'), separator = path.find_last_of("/\\");
    if (dot != std::string::npos && (separator == std::string::npos || dot > separator) &&
        format_by_name(path.substr(dot + 1), f)) return f;
    if (require_extension) die("use a known output extension or --format png|jpeg|bmp|qoi|pnm");
    return BR_ENCODE_PNG;
}

br_encode_options encode_options(br_encoded_format f, const Args& a) {
    br_encode_options o = br_encode_options_default(f);
    o.quality = static_cast<int32_t>(a.num("--quality", f == BR_ENCODE_JPEG ? 90 : 90));
    o.effort = static_cast<int32_t>(a.num("--effort", o.effort));
    if (a.has("--420")) o.force_444 = 0;
    if (a.has("--fast-jpeg")) o.jpeg_optimize = 0;
    if (a.has("--palette")) {
        const std::string p = lower(a.get("--palette"));
        o.png_palette = p == "off" ? BR_PALETTE_OFF : p == "quantize" ? BR_PALETTE_QUANTIZE : BR_PALETTE_AUTO;
    }
    if (a.has("--colors")) {
        o.png_palette = BR_PALETTE_QUANTIZE;
        o.max_colors = static_cast<uint32_t>(a.num("--colors", 256));
    }
    return o;
}

size_t save(const br_image_view& img, const std::string& path, const Args& a) {
    const br_encoded_format f = format_for_path(path, a);
    const br_encode_options o = encode_options(f, a);
    uint8_t* data = nullptr;
    size_t size = 0;
    check(br_encode_alloc(&img, &o, &data, &size), "encode " + path);
    std::FILE* fp = nullptr;
    if (path == "-") {
        fp = stdout;
#if defined(_WIN32)
        _setmode(_fileno(stdout), _O_BINARY);
#endif
    } else {
#if defined(_WIN32)
        const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        std::wstring w(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), n);
        fp = _wfopen(w.c_str(), L"wb");
#else
        fp = std::fopen(path.c_str(), "wb");
#endif
    }
    if (!fp) { br_free(data); die("cannot write " + path); }
    const bool ok = std::fwrite(data, 1, size, fp) == size;
    if (fp != stdout) std::fclose(fp);
    else std::fflush(stdout);
    br_free(data);
    if (!ok) die("write failed: " + path);
    return size;
}

br_image load(const std::string& path) {
    br_image img{};
    check(br_load(path.c_str(), BR_PIXEL_UNKNOWN, &img), "load " + path);
    return img;
}

Value rect_json(const br_rect_i32& r) {
    Value v = Value::array();
    v.push(r.x);
    v.push(r.y);
    v.push(r.width);
    v.push(r.height);
    return v;
}

bool parse_rect(const std::string& s, br_rect_i32& r) {
    int x, y, w, h;
    if (std::sscanf(s.c_str(), "%d,%d,%d,%d", &x, &y, &w, &h) != 4) return false;
    r = {x, y, w, h};
    return w > 0 && h > 0;
}

double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

std::string base_name(const std::string& p) {
    const size_t s = p.find_last_of("/\\");
    std::string b = s == std::string::npos ? p : p.substr(s + 1);
    const size_t dot = b.find_last_of('.');
    return dot == std::string::npos ? b : b.substr(0, dot);
}

const char* format_name(br_encoded_format f) {
    switch (f) {
    case BR_ENCODE_PNG: return "png";
    case BR_ENCODE_JPEG: return "jpeg";
    case BR_ENCODE_BMP: return "bmp";
    case BR_ENCODE_QOI: return "qoi";
    case BR_ENCODE_PNM: return "pnm";
    default: return "raw";
    }
}





int cmd_info(const Args& a) {
    if (a.positional.empty()) die("usage: br info <file>...");
    Value all = Value::array();
    for (const auto& path : a.positional) {
        std::FILE* fp = std::fopen(path.c_str(), "rb");
        if (!fp) die("cannot open " + path);
        std::vector<uint8_t> bytes;
        uint8_t buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) bytes.insert(bytes.end(), buf, buf + n);
        std::fclose(fp);
        br_image_info info{};
        check(br_probe(bytes.data(), bytes.size(), &info), "probe " + path);
        Value v = Value::object();
        v.set("file", path);
        v.set("format", format_name(info.format));
        v.set("width", info.width);
        v.set("height", info.height);
        v.set("channels", info.channels);
        v.set("bit_depth", static_cast<uint32_t>(info.bit_depth));
        v.set("alpha", info.has_alpha != 0);
        v.set("progressive", info.progressive != 0);
        v.set("bytes", static_cast<uint64_t>(bytes.size()));
        
        Value parts = Value::array();
        if (info.format == BR_ENCODE_PNG) {
            for (size_t off = 8; off + 12 <= bytes.size();) {
                const uint32_t len = (uint32_t(bytes[off]) << 24) | (uint32_t(bytes[off + 1]) << 16) | (uint32_t(bytes[off + 2]) << 8) | bytes[off + 3];
                parts.push(std::string(reinterpret_cast<const char*>(&bytes[off + 4]), 4) + ":" + std::to_string(len));
                if (std::memcmp(&bytes[off + 4], "IEND", 4) == 0 || static_cast<uint64_t>(len) + 12 > bytes.size() - off) break;
                off += 12 + len;
            }
        } else if (info.format == BR_ENCODE_JPEG) {
            for (size_t off = 2; off + 4 <= bytes.size();) {
                if (bytes[off] != 0xff) { ++off; continue; }
                const uint8_t m = bytes[off + 1];
                if (m == 0xd9 || m == 0xda) { parts.push(m == 0xda ? "SOS" : "EOI"); break; }
                const uint32_t len = (uint32_t(bytes[off + 2]) << 8) | bytes[off + 3];
                char name[16];
                std::snprintf(name, sizeof(name), "FF%02X:%u", m, len);
                parts.push(std::string(name));
                off += 2 + len;
            }
        }
        if (!parts.items().empty()) v.set("segments", std::move(parts));
        if (a.has("--json")) {
            all.push(std::move(v));
        } else {
            std::printf("%s: %s %ux%u, %u channel(s), %u-bit%s%s, %zu bytes\n", path.c_str(), format_name(info.format), info.width,
                        info.height, info.channels, info.bit_depth, info.has_alpha ? ", alpha" : "",
                        info.progressive ? (info.format == BR_ENCODE_PNG ? ", interlaced" : ", progressive") : "", bytes.size());
            if (v.has("segments")) {
                std::printf("  segments:");
                for (const auto& s : v["segments"].items()) std::printf(" %s", s.as_string().c_str());
                std::printf("\n");
            }
        }
    }
    if (a.has("--json")) std::printf("%s\n", (a.positional.size() == 1 ? all[size_t{0}] : all).dump().c_str());
    return 0;
}


void resize_one(br_context* ctx, const std::string& in, const std::string& out, const Args& a, bool require_size) {
    const auto t0 = std::chrono::steady_clock::now();
    br_image img = load(in);
    br_image_view src = br_image_as_view(&img);
    br_image cropped_holder{};
    br_rect_i32 region;
    if (a.has("--region")) {
        if (!parse_rect(a.get("--region"), region)) die("--region must be x,y,w,h");
        br_image_view crop;
        check(br_image_crop(&src, region, &crop), "crop");
        src = crop;
    }
    uint32_t w = src.width, h = src.height;
    const bool sized = target_size(a, src.width, src.height, w, h);
    if (require_size && !sized && !a.has("--region")) die("give a target size: --width/--height, --scale, --long-edge or --max-pixels");
    br_image out_img{};
    if (w != src.width || h != src.height) {
        const br_resize_options o = resize_options(a, BR_RESIZE_UI_TEXT);
        check(br_resize_to(ctx, &src, w, h, &o, &out_img), "resize " + in);
    } else {
        check(br_image_clone(&src, BR_PIXEL_UNKNOWN, &out_img), "copy");
    }
    if (a.has("--grid")) {
        br_grid_options g = br_grid_options_default();
        
        g.label_transform = {static_cast<double>(src.width) / w, static_cast<double>(src.height) / h,
                             a.has("--region") ? static_cast<double>(region.x) : 0.0, a.has("--region") ? static_cast<double>(region.y) : 0.0};
        g.step = static_cast<uint32_t>(a.num("--step", 0));
        if (g.step) g.step = std::max<uint32_t>(1, static_cast<uint32_t>(g.step * static_cast<double>(w) / src.width));
        check(br_draw_grid(&out_img, &g), "grid");
    }
    const br_image_view ov = br_image_as_view(&out_img);
    const size_t bytes = save(ov, out, a);
    if (!a.has("--quiet") && out != "-")
        std::fprintf(stderr, "%s -> %s  %ux%u -> %ux%u  %zu bytes  %.1f ms\n", in.c_str(), out.c_str(), img.width, img.height,
                     out_img.width, out_img.height, bytes, ms_since(t0));
    br_image_free(&out_img);
    br_image_free(&cropped_holder);
    br_image_free(&img);
}

int cmd_resize(const Args& a) {
    br_context* ctx = nullptr;
    check(br_context_create(&ctx), "context");
    check(br_context_set_threads(ctx, static_cast<uint32_t>(a.num("--threads", 0))), "threads");
    if (a.has("--out-dir")) {
        const std::string dir = a.get("--out-dir");
        const std::string suffix = a.get("--suffix", "");
        for (const auto& in : a.positional) {
            br_encoded_format f;
            std::string ext;
            if (a.has("--format") && format_by_name(a.get("--format"), f)) ext = format_ext(f);
            else {
                const size_t dot = in.find_last_of('.');
                ext = dot == std::string::npos ? ".png" : in.substr(dot);
            }
            const std::string out = dir + "/" + base_name(in) + suffix + ext;
            resize_one(ctx, in, out, a, true);
        }
    } else {
        if (a.positional.size() != 2) die("usage: br resize <in> <out> SIZE [options]  (or several inputs with --out-dir DIR)");
        resize_one(ctx, a.positional[0], a.positional[1], a, true);
    }
    br_context_destroy(ctx);
    return 0;
}

int cmd_format(const Args& a) {
    if (a.positional.size() != 2) die("usage: br format <in> <out> [--format png|jpeg|bmp|qoi|pnm] [-q N]");
    const std::vector<std::string> allowed = {"--format", "--quality", "--effort", "--palette", "--colors",
        "--threads", "--420", "--fast-jpeg", "--quiet"};
    for (const auto& option : a.options)
        if (std::find(allowed.begin(), allowed.end(), option.first) == allowed.end())
            die("unsupported format option '" + option.first + "'");
    auto range = [&](const char* name, long long minimum, long long maximum) {
        if (a.has(name)) {
            const auto value = a.num(name, 0);
            if (value < minimum || value > maximum) die(std::string(name) + " is out of range");
        }
    };
    range("--quality", 1, 100); range("--effort", 0, 9); range("--colors", 2, 256);
    if (a.has("--palette") && a.get("--palette") != "auto" && a.get("--palette") != "off" && a.get("--palette") != "quantize")
        die("--palette must be auto, off or quantize");
    (void)format_for_path(a.positional[1], a, true);
    br_image image = load(a.positional[0]);
    const auto view = br_image_as_view(&image);
    const size_t bytes = save(view, a.positional[1], a);
    if (!a.has("--quiet") && a.positional[1] != "-")
        std::fprintf(stderr, "%s -> %s  %ux%u  %zu bytes\n", a.positional[0].c_str(), a.positional[1].c_str(),
                     image.width, image.height, bytes);
    br_image_free(&image);
    return 0;
}

int cmd_diff(const Args& a) {
    if (a.positional.size() != 2) die("usage: br diff <before> <after> [--out annotated.png] [--threshold N] [--gap PX] [--json]");
    br_image x = load(a.positional[0]), y = load(a.positional[1]);
    const br_image_view xv = br_image_as_view(&x), yv = br_image_as_view(&y);
    br_diff_options o = br_diff_options_default();
    o.threshold = static_cast<uint8_t>(a.num("--threshold", 0));
    o.merge_gap = static_cast<uint32_t>(a.num("--gap", 8));
    std::vector<br_rect_i32> rects(4096);
    br_diff_result r{};
    check(br_diff(&xv, &yv, &o, rects.data(), rects.size(), &r), "diff");
    rects.resize(std::min<size_t>(r.rect_count, rects.size()));
    if (a.has("--json")) {
        Value v = Value::object();
        v.set("changed_pixels", r.changed_pixels);
        v.set("changed_fraction", r.changed_fraction);
        v.set("bounds", rect_json(r.bounds));
        Value list = Value::array();
        for (const auto& rc : rects) list.push(rect_json(rc));
        v.set("rects", std::move(list));
        std::printf("%s\n", v.dump().c_str());
    } else {
        std::printf("changed pixels: %llu (%.4f%%), %u region(s)\n", static_cast<unsigned long long>(r.changed_pixels),
                    r.changed_fraction * 100.0, r.rect_count);
        for (size_t i = 0; i < rects.size(); ++i)
            std::printf("  #%zu  x=%d y=%d w=%d h=%d\n", i + 1, rects[i].x, rects[i].y, rects[i].width, rects[i].height);
    }
    if (a.has("--out")) {
        br_draw_marks(&y, rects.data(), rects.size(), 1, 0);
        save(yv, a.get("--out"), a);
    }
    br_image_free(&x);
    br_image_free(&y);
    return r.changed_pixels ? 2 : 0;
}

int cmd_annotate(const Args& a) {
    if (a.positional.size() != 2) die("usage: br annotate <in> <out> [--grid] [--step N] [--marks \"x,y,w,h;...\"]");
    br_image img = load(a.positional[0]);
    br_image rgb{};
    const br_image_view v = br_image_as_view(&img);
    check(br_image_clone(&v, BR_PIXEL_RGBA8, &rgb), "convert");
    if (a.has("--grid") || a.has("--step")) {
        br_grid_options g = br_grid_options_default();
        g.step = static_cast<uint32_t>(a.num("--step", 0));
        check(br_draw_grid(&rgb, &g), "grid");
    }
    if (a.has("--marks")) {
        std::vector<br_rect_i32> rects;
        std::string s = a.get("--marks");
        size_t pos = 0;
        while (pos <= s.size()) {
            const size_t end = s.find(';', pos);
            br_rect_i32 r;
            if (parse_rect(s.substr(pos, end == std::string::npos ? std::string::npos : end - pos), r)) rects.push_back(r);
            if (end == std::string::npos) break;
            pos = end + 1;
        }
        check(br_draw_marks(&rgb, rects.data(), rects.size(), 1, 0), "marks");
    }
    const br_image_view ov = br_image_as_view(&rgb);
    save(ov, a.positional[1], a);
    br_image_free(&rgb);
    br_image_free(&img);
    return 0;
}

int cmd_monitors(const Args& a) {
    br_monitor_info list[32];
    size_t n = 0;
    check(br_monitors(list, 32, &n), "monitors");
    Value arr = Value::array();
    for (size_t i = 0; i < std::min<size_t>(n, 32); ++i) {
        const auto& m = list[i];
        if (a.has("--json")) {
            Value o = Value::object();
            o.set("index", m.index);
            o.set("name", m.name);
            o.set("primary", m.primary != 0);
            o.set("bounds", rect_json(m.bounds));
            o.set("work_area", rect_json(m.work_area));
            o.set("dpi", m.dpi);
            o.set("scale", static_cast<double>(m.scale));
            o.set("rotation", m.rotation);
            arr.push(std::move(o));
        } else {
            std::printf("#%u %s%s  %dx%d at (%d,%d)  dpi %u (%.0f%%)  rotation %d\n", m.index, m.name, m.primary ? " [primary]" : "",
                        m.bounds.width, m.bounds.height, m.bounds.x, m.bounds.y, m.dpi, m.scale * 100.0, m.rotation);
        }
    }
    if (a.has("--json")) std::printf("%s\n", arr.dump().c_str());
    return 0;
}

int cmd_windows(const Args& a) {
    std::vector<br_window_info> list(1024);
    size_t n = 0;
    check(br_windows(list.data(), list.size(), &n), "windows");
    list.resize(std::min(n, list.size()));
    const std::string filter = lower(a.get("--filter"));
    Value arr = Value::array();
    for (const auto& w : list) {
        if (!w.title[0] && !a.has("--all")) continue;
        if (!filter.empty() && lower(w.title).find(filter) == std::string::npos) continue;
        if (a.has("--json")) {
            Value o = Value::object();
            o.set("handle", w.handle);
            o.set("title", w.title);
            o.set("class", w.class_name);
            o.set("pid", w.process_id);
            o.set("bounds", rect_json(w.bounds));
            o.set("client", rect_json(w.client));
            o.set("dpi", w.dpi);
            o.set("minimized", w.minimized != 0);
            o.set("maximized", w.maximized != 0);
            o.set("foreground", w.foreground != 0);
            arr.push(std::move(o));
        } else {
            std::printf("0x%llx  pid %-6u %s%-5s %dx%d at (%d,%d)  %s  [%s]\n", static_cast<unsigned long long>(w.handle), w.process_id,
                        w.foreground ? "*" : " ", w.minimized ? "min" : "", w.bounds.width, w.bounds.height, w.bounds.x, w.bounds.y,
                        w.title, w.class_name);
        }
    }
    if (a.has("--json")) std::printf("%s\n", arr.dump().c_str());
    return 0;
}

const char* const kBackendNames[] = {"auto", "dxgi", "wgc", "gdi", "gdi-screen", "gdi-print"};

const char* backend_name(uint32_t b) { return kBackendNames[std::min<uint32_t>(b, 5)]; }

br_capture_backend backend_by_name(const std::string& s0) {
    const std::string s = lower(s0);
    for (uint32_t i = 0; i < 6; ++i)
        if (s == kBackendNames[i]) return static_cast<br_capture_backend>(i);
    die("unknown backend '" + s0 + "' (auto, dxgi, wgc, gdi, gdi-screen, gdi-print)");
}

br_capture_options capture_options_from(const Args& a) {
    br_capture_options o = br_capture_options_default();
    if (a.has("--window") || a.has("--hwnd")) {
        o.target = BR_TARGET_WINDOW;
        if (a.has("--hwnd")) {
            o.window = std::strtoull(a.get("--hwnd").c_str(), nullptr, 0);
        } else {
            br_window_info w{};
            check(br_find_window(a.get("--window").c_str(), &w), "find window '" + a.get("--window") + "'");
            o.window = w.handle;
        }
        o.client_area = a.has("--client") ? 1 : 0;
        if (a.has("--region") && !parse_rect(a.get("--region"), o.region)) die("--region must be x,y,w,h");
    } else if (a.has("--desktop")) {
        o.target = BR_TARGET_DESKTOP;
    } else if (a.has("--region") && !a.has("--monitor")) {
        o.target = BR_TARGET_REGION;
        if (!parse_rect(a.get("--region"), o.region)) die("--region must be x,y,w,h (desktop pixels)");
    } else {
        o.target = BR_TARGET_MONITOR;
        o.monitor = static_cast<uint32_t>(a.num("--monitor", 0));
        if (a.has("--region") && !parse_rect(a.get("--region"), o.region)) die("--region must be x,y,w,h");
    }
    o.backend = backend_by_name(a.get("--backend", "auto"));
    o.include_cursor = a.has("--cursor") ? 1 : 0;
    o.border = a.has("--border") ? 1 : 0;

    return o;
}

int cmd_capture(const Args& a) {
    const auto t0 = std::chrono::steady_clock::now();
    br_capture_options o = capture_options_from(a);
    br_image frame{};
    br_frame_info info{};
    if (a.has("--timeout")) {
        br_capture* cap = nullptr;
        check(br_capture_open(nullptr, &o, &cap), "open capture");
        check(br_capture_grab(cap, static_cast<uint32_t>(a.num("--timeout", 2000)), &frame, &info, nullptr, 0), "grab");
        br_capture_destroy(cap);
    } else {
        
        check(br_screenshot(&o, &frame, &info), "capture");
    }
    const double t_grab = ms_since(t0);

    br_image_view fv = br_image_as_view(&frame);
    uint32_t w = frame.width, h = frame.height;
    target_size(a, frame.width, frame.height, w, h);
    br_image out{};
    if (w != frame.width || h != frame.height) {
        const br_resize_options ro = resize_options(a, BR_RESIZE_UI_TEXT);
        check(br_resize_to(nullptr, &fv, w, h, &ro, &out), "resize");
    } else {
        out = frame;
        frame = br_image{};
    }
    const br_transform image_to_screen = br_transform_compose(
        {static_cast<double>(fv.width) / out.width, static_cast<double>(fv.height) / out.height, 0.0, 0.0}, info.image_to_screen);
    if (a.has("--grid")) {
        br_grid_options g = br_grid_options_default();
        g.label_transform = image_to_screen;
        const long long step = a.num("--step", 0);
        if (step > 0) g.step = std::max<uint32_t>(1, static_cast<uint32_t>(step / image_to_screen.sx));
        check(br_draw_grid(&out, &g), "grid");
    }
    const std::string path = a.get("--out", "screenshot.png");
    const br_image_view ov = br_image_as_view(&out);
    const size_t bytes = save(ov, path, a);
    if (a.has("--json")) {
        Value v = Value::object();
        v.set("file", path);
        v.set("width", out.width);
        v.set("height", out.height);
        v.set("bytes", static_cast<uint64_t>(bytes));
        v.set("backend", backend_name(info.backend));
        v.set("screen", rect_json(info.screen_rect));
        Value t = Value::object();
        t.set("sx", image_to_screen.sx);
        t.set("sy", image_to_screen.sy);
        t.set("tx", image_to_screen.tx);
        t.set("ty", image_to_screen.ty);
        v.set("image_to_screen", std::move(t));
        v.set("capture_ms", t_grab);
        v.set("total_ms", ms_since(t0));
        std::printf("%s\n", v.dump().c_str());
    } else {
        std::fprintf(stderr, "%s: %ux%u (screen %dx%d at %d,%d via %s) %zu bytes, capture %.1f ms, total %.1f ms\n", path.c_str(), out.width,
                     out.height, info.screen_rect.width, info.screen_rect.height, info.screen_rect.x, info.screen_rect.y,
                     backend_name(info.backend), bytes, t_grab, ms_since(t0));
        std::fprintf(stderr, "image -> screen: x * %.6g + %.6g, y * %.6g + %.6g\n", image_to_screen.sx, image_to_screen.tx,
                     image_to_screen.sy, image_to_screen.ty);
    }
    br_image_free(&out);
    br_image_free(&frame);
    return 0;
}

bool write_bytes(const std::string& path, const uint8_t* data, size_t size) {
    if (path == "-") {
#if defined(_WIN32)
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        return std::fwrite(data, 1, size, stdout) == size;
    }
    const std::u8string u8(path.begin(), path.end());
    std::ofstream f(std::filesystem::path(u8), std::ios::binary);
    f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(f);
}

double us_to_ms(uint64_t us) { return static_cast<double>(us) / 1000.0; }



int cmd_shot(const Args& a) {
    const std::string profile = lower(a.get("--profile", "vision"));
    if (profile != "vision" && profile != "compact" && profile != "legacy")
        die("--profile must be vision, compact or legacy");
    br_shot_options o = br_shot_options_for(profile == "vision" ? BR_SHOT_VISION :
                                          profile == "compact" ? BR_SHOT_COMPACT : BR_SHOT_LEGACY);
    o.capture = capture_options_from(a);
    if (a.has("--scale")) {
        o.scale = a.real("--scale", 0.0);
        if (!(o.scale > 0.0 && o.scale <= 8.0)) die("--scale must be in (0, 8]");
    }
    br_fit_options fit{};
    if (!size_limits_from(a, fit)) fit = br_fit_options{};
    if (a.has("--width")) { fit.max_width = static_cast<uint32_t>(a.num("--width", 0)); fit.allow_upscale = 1; }
    if (a.has("--height")) { fit.max_height = static_cast<uint32_t>(a.num("--height", 0)); fit.allow_upscale = 1; }
    o.fit = fit;
    o.resize = resize_options(a, BR_RESIZE_UI_TEXT);
    br_encoded_format output_format = o.encode.format;
    if (a.has("--format") && !format_by_name(a.get("--format"), output_format)) die("unknown --format");
    const std::string path = a.get("--out", std::string("screenshot") + format_ext(output_format));
    o.encode = encode_options(format_for_path(path, a), a);
    if (profile == "compact" && o.encode.format == BR_ENCODE_JPEG && !a.has("--quality")) o.encode.quality = 95;
    o.timeout_ms = static_cast<uint32_t>(a.num("--timeout", 2000));
    o.settle_ms = static_cast<uint32_t>(a.num("--settle", 0));
    o.settle_timeout_ms = static_cast<uint32_t>(a.num("--settle-timeout", 0));
    o.keep_alive_ms = static_cast<uint32_t>(a.num("--keep-alive", 0));
    o.flags |= BR_SHOT_CUSTOM_RESIZE | (a.has("--no-cache") ? BR_SHOT_NO_CACHE : 0);
    if (a.has("--reuse-encoded")) o.flags |= BR_SHOT_REUSE_ENCODED;
    if (a.has("--no-reuse-encoded")) o.flags &= ~static_cast<uint32_t>(BR_SHOT_REUSE_ENCODED);
    const long long repeat = a.num("--repeat", 1);
    if (repeat < 1 || repeat > 100000) die("--repeat must be in 1..100000");

    double prewarm_ms = -1;
    if (a.has("--prewarm")) {
        const auto t = std::chrono::steady_clock::now();
        const br_status st = br_capture_prewarm();
        prewarm_ms = ms_since(t);
        if (st != BR_OK && !a.has("--json")) std::fprintf(stderr, "prewarm: %s\n", br_status_string(st));
    }
    Value rows = Value::array();
    br_shot_result last{};
    for (long long i = 0; i < repeat; ++i) {
        br_shot_result r{};
        check(br_shot(nullptr, &o, &r), "shot");
        const br_shot_timings& t = r.timings;
        if (a.has("--json")) {
            Value row = Value::object();
            row.set("open_ms", us_to_ms(t.open_us));
            row.set("settle_ms", us_to_ms(t.settle_us));
            row.set("capture_ms", us_to_ms(t.capture_us));
            row.set("resize_ms", us_to_ms(t.resize_us));
            row.set("encode_ms", us_to_ms(t.encode_us));
            row.set("total_ms", us_to_ms(t.total_us));
            row.set("session_reused", r.session_reused != 0);
            rows.push(std::move(row));
        } else {
            std::fprintf(stderr, "#%-3lld open %6.2f  settle %6.2f  capture %6.2f  resize %6.2f  encode %6.2f  total %7.2f ms  %s%s\n", i + 1,
                         us_to_ms(t.open_us), us_to_ms(t.settle_us), us_to_ms(t.capture_us), us_to_ms(t.resize_us), us_to_ms(t.encode_us),
                         us_to_ms(t.total_us), r.session_reused ? "cached session" : "new session", r.gpu_scaled ? ", GPU scale" : "");
        }
        br_shot_result_free(&last);
        last = r;
    }
    if (!write_bytes(path, last.data, last.size)) die("cannot write '" + path + "'");
    const br_transform& m = last.frame.image_to_screen;
    if (a.has("--json")) {
        Value v = Value::object();
        v.set("file", path);
        v.set("width", last.width);
        v.set("height", last.height);
        v.set("bytes", static_cast<uint64_t>(last.size));
        v.set("profile", profile);
        v.set("format", format_name(o.encode.format));
        v.set("lossless_encoding", o.encode.format == BR_ENCODE_PNG && o.encode.png_palette != BR_PALETTE_QUANTIZE);
        v.set("backend", backend_name(last.frame.backend));
        v.set("screen", rect_json(last.frame.screen_rect));
        Value t = Value::object();
        t.set("sx", m.sx); t.set("sy", m.sy); t.set("tx", m.tx); t.set("ty", m.ty);
        v.set("image_to_screen", std::move(t));
        if (prewarm_ms >= 0) v.set("prewarm_ms", prewarm_ms);
        v.set("runs", std::move(rows));
        std::printf("%s\n", v.dump().c_str());
    } else {
        if (prewarm_ms >= 0) std::fprintf(stderr, "prewarm %.1f ms\n", prewarm_ms);
        std::fprintf(stderr, "%s: %ux%u, %zu bytes via %s (screen %dx%d at %d,%d)\n", path.c_str(), last.width, last.height, last.size,
                     backend_name(last.frame.backend), last.frame.screen_rect.width, last.frame.screen_rect.height, last.frame.screen_rect.x,
                     last.frame.screen_rect.y);
        std::fprintf(stderr, "image -> screen: x * %.6g + %.6g, y * %.6g + %.6g\n", m.sx, m.tx, m.sy, m.ty);
    }
    br_shot_result_free(&last);
    br_shot_cache_clear();
    return 0;
}

void usage() {
    std::printf(
        "br %s - BetterResolution command line (features: %s)\n\n"
        "Images:\n"
        "  br resize <in> <out> SIZE [RESIZE] [ENCODE]     change resolution\n"
        "  br resize <in...> --out-dir DIR SIZE [...]      batch (optional --suffix, --format)\n"
        "  br format <in> <out> [ENCODE]                  change format; size unchanged\n"
        "  br convert <in> <out> [ENCODE]                 alias for format\n"
        "  br info <file...> [--json]                      format, size, container segments\n"
        "  br diff <before> <after> [--out marked.png] [--threshold N] [--gap PX] [--json]\n"
        "  br annotate <in> <out> [--grid] [--step N] [--marks \"x,y,w,h;...\"]\n"
        "  br text <in> <out> [--preset screenshot|ocr] [--scale F] [--region x,y,w,h]\n"
        "  br detail <original> <out.png> [--intent text|icon|native] [--region x,y,w,h]\n"
        "          [--context N] [--scale 1..8] [--border N] [--max-pixels N] [--long-edge N]\n"
        "          [--filter area|triangle|catmull|mitchell|lanczos2|lanczos3|lanczos4|point]\n"
        "  br read in.png [--json] [--match TEXT] [--image marked.png] [--mark circle|rect] [--crop crop.png]\n"
        "          omit --match to mark all detected text\n"
        "Screen (Windows):\n"
        "  br capture [-o out.png] [--monitor N | --window TITLE | --hwnd H | --region x,y,w,h | --desktop]\n"
        "             [--client] [--backend auto|dxgi|wgc|gdi|gdi-screen|gdi-print] [--cursor] [--border] SIZE [--grid [--step PX]] [--json]\n"
        "  br shot [TARGET] [-o out.png] SIZE [RESIZE] [ENCODE] [--profile vision|compact|legacy]\n"
        "          [--repeat N] [--settle MS] [--no-cache] [--no-reuse-encoded] [--prewarm] [--json]\n"
        "             capture + fit + encode with a persistent session\n"
        "  br monitors [--json]\n"
        "  br windows [--filter TEXT] [--all] [--json]\n\n"
        "SIZE:    -w/--width N, -h/--height N, --scale F, --long-edge N, --short-edge N,\n"
        "         --max-pixels N, --multiple-of N, --upscale\n"
        "RESIZE:  --mode text|quality|balanced|fast (default text), --filter box|triangle|bspline|catmull-rom|mitchell|\n"
        "         lanczos2|lanczos3|lanczos4|point, --linear | --gamma, --no-antiring, --no-multistage, --threads N\n"
        "ENCODE:  --format png|jpeg|bmp|qoi|ppm (default: from extension), -q/--quality N, --420, --fast-jpeg,\n"
        "         --effort 0-9, --palette auto|off|quantize, --colors N\n"
        "Other:   --quiet, use '-' as output to write to stdout\n",
        br_version_string(), br_features());
}

} 

int main(int argc, char** argv) {
    std::vector<std::string> raw;
#if defined(_WIN32)
    // Аргументы и вывод в UTF-8 (названия окон часто на кириллице), координаты в физических пикселях.
    SetConsoleOutputCP(CP_UTF8);
    if (HMODULE u = GetModuleHandleW(L"user32.dll")) {
        using Fn = BOOL(WINAPI*)(HANDLE);
        if (auto fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(u, "SetProcessDpiAwarenessContext"))))
            fn(reinterpret_cast<HANDLE>(static_cast<intptr_t>(-4)));
    }
    int wargc = 0;
    if (LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc)) {
        for (int i = 1; i < wargc; ++i) {
            const int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
            std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
            if (n > 1) WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, s.data(), n, nullptr, nullptr);
            raw.push_back(s);
        }
        LocalFree(wargv);
    }
    (void)argc;
    (void)argv;
#else
    for (int i = 1; i < argc; ++i) raw.emplace_back(argv[i]);
#endif
    if (raw.empty() || raw[0] == "help" || raw[0] == "--help" || raw[0] == "-?") {
        usage();
        return raw.empty() ? 1 : 0;
    }
    const std::string cmd = raw[0];
    raw.erase(raw.begin());
    if (cmd == "text" || cmd == "detail")
        for (size_t i = 2; i < raw.size(); ++i)
            if (raw[i] == "--fit" || raw[i].rfind("--fit=", 0) == 0)
                die("--fit was removed; use explicit dimensions or --scale");
    if (cmd == "text") return br_text_command(raw);
    if (cmd == "detail") return br_vision_command(raw);
    if (cmd == "read") return br_read_command(raw);
    Args a;
    std::string err;
    if (!parse_args(raw, a, err)) die(err);
    if (a.get("--out") == "-" && a.has("--json")) die("stdout image output cannot be combined with --json");
    if (cmd == "version" || cmd == "--version") {
        std::printf("%s (%s)\n", br_version_string(), br_features());
        return 0;
    }
    if (a.has("--threads")) {
        const long long threads = a.num("--threads", 0);
        if (threads < 0 || threads > 64) die("--threads must be in 0..64");
        check(br_context_set_threads(nullptr, static_cast<uint32_t>(threads)), "threads");
    }
    if (cmd == "resize") return cmd_resize(a);
    if (cmd == "format" || cmd == "convert") return cmd_format(a);
    if (cmd == "info") return cmd_info(a);
    if (cmd == "diff") return cmd_diff(a);
    if (cmd == "annotate") return cmd_annotate(a);
    if (cmd == "capture" || cmd == "screenshot") return cmd_capture(a);
    if (cmd == "shot") return cmd_shot(a);
    if (cmd == "monitors") return cmd_monitors(a);
    if (cmd == "windows") return cmd_windows(a);
    die("unknown command '" + cmd + "' (run `br help`)");
}
