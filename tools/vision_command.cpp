#include <br/br.hpp>
#include <br/br_vision.h>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
uint64_t number(const std::string& text) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("expected non-negative decimal integer");
    size_t end = 0;
    const auto value = std::stoull(text, &end);
    if (end != text.size()) throw std::runtime_error("invalid integer");
    return value;
}
uint32_t small_number(const std::string& text) {
    const auto value = number(text);
    if (value > UINT32_MAX) throw std::runtime_error("integer exceeds uint32");
    return static_cast<uint32_t>(value);
}
br_rect_i32 region(const std::string& text) {
    br_rect_i32 r{}; int32_t* fields[] = {&r.x, &r.y, &r.width, &r.height};
    size_t start = 0;
    for (int i = 0; i < 4; ++i) {
        const size_t end = text.find(',', start);
        if ((i < 3) == (end == std::string::npos)) throw std::runtime_error("region must be x,y,w,h");
        const auto part = text.substr(start, end - start); size_t used = 0;
        const auto value = std::stoll(part, &used);
        if (used != part.size() || value < INT32_MIN || value > INT32_MAX) throw std::runtime_error("invalid region integer");
        *fields[i] = static_cast<int32_t>(value); start = end + 1;
    }
    return r;
}
}

int br_vision_command(const std::vector<std::string>& args) {
    try {
        if (args.size() < 2) {
            std::fprintf(stderr, "br detail <original> <out.png> [--intent text|icon|native] [--region x,y,w,h]\n"
                " [--scale 1..8] [--context 0..256] [--border 0..256] [--max-pixels N] [--long-edge N]\n"
                "Color PNG from the ORIGINAL frame; integer zoom, context margins, exact coordinates.\n");
            return 1;
        }
        br_vision_preset preset = BR_VISION_TEXT;
        for (size_t i = 2; i < args.size(); i += 2) {
            if (i + 1 >= args.size()) throw std::runtime_error("detail option needs a value");
            if (args[i] == "--intent") {
                if (args[i + 1] == "text") preset = BR_VISION_TEXT;
                else if (args[i + 1] == "icon") preset = BR_VISION_ICON;
                else if (args[i + 1] == "native") preset = BR_VISION_NATIVE;
                else throw std::runtime_error("intent must be text, icon or native");
            }
        }
        auto options = br_vision_options_default(preset); br_rect_i32 requested{};
        for (size_t i = 2; i < args.size(); i += 2) {
            const auto& key = args[i]; const auto& value = args[i + 1];
            if (key == "--intent") continue;
            if (key == "--region") requested = region(value);
            else if (key == "--scale") options.scale = small_number(value);
            else if (key == "--context") options.context_margin = small_number(value);
            else if (key == "--border") options.border = small_number(value);
            else if (key == "--max-pixels") options.max_pixels = number(value);
            else if (key == "--long-edge") options.max_long_edge = small_number(value);
            else throw std::runtime_error("unknown detail option");
        }
        auto original = br::Image::load(args[0]); const auto view = original.view();
        br_image raw{}; br_vision_plan plan{};
        br::check(br_vision_prepare(nullptr, &view, requested, &options, &raw, &plan), "detail prepare");
        br::Image output(raw); const auto out = output.view();
        const auto encode = br_encode_options_default(BR_ENCODE_PNG);
        br::check(br_save(args[1].c_str(), &out, &encode), "detail PNG");
        const auto& r = plan.requested_region; const auto& s = plan.source_region;
        const auto& c = plan.content_rect; const auto& t = plan.image_to_source;
        std::printf("{\"width\":%u,\"height\":%u,\"intent\":\"%s\",\"requested_region\":[%d,%d,%d,%d],"
            "\"source_region\":[%d,%d,%d,%d],\"content_rect\":[%d,%d,%d,%d],"
            "\"image_to_source\":[%.17g,%.17g,%.17g,%.17g],\"scale\":%u,\"desired_scale\":%u,"
            "\"limited\":%s}\n", output.width(), output.height(),
            preset == BR_VISION_ICON ? "icon" : preset == BR_VISION_TEXT ? "text" : "native",
            r.x, r.y, r.width, r.height, s.x, s.y, s.width, s.height, c.x, c.y, c.width, c.height,
            t.sx, t.sy, t.tx, t.ty, plan.scale, plan.desired_scale, plan.chose_limited ? "true" : "false");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "br detail: %s\n", error.what()); return 1; }
}
