#include "json.hpp"

#include <cmath>
#include <cstdio>

namespace brjson {

const Value& Value::null_value() {
    static const Value v;
    return v;
}

bool Value::has(const std::string& k) const {
    for (const auto& m : o_) if (m.first == k) return true;
    return false;
}

const Value& Value::operator[](const std::string& k) const {
    for (const auto& m : o_) if (m.first == k) return m.second;
    return null_value();
}

Value& Value::set(const std::string& k, Value v) {
    type_ = Type::Object;
    for (auto& m : o_) {
        if (m.first == k) { m.second = std::move(v); return m.second; }
    }
    o_.emplace_back(k, std::move(v));
    return o_.back().second;
}

void escape_string(const std::string& s, std::string& out) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out.push_back('"');
}

void Value::dump_to(std::string& out) const {
    switch (type_) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += b_ ? "true" : "false"; break;
    case Type::Number: {
        if (!std::isfinite(n_)) { out += "null"; break; }
        char buf[40];
        if (std::fabs(n_) < 9.007199254740992e15 && n_ == std::floor(n_)) std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(n_));
        else std::snprintf(buf, sizeof(buf), "%.17g", n_);
        out += buf;
        break;
    }
    case Type::String: escape_string(s_, out); break;
    case Type::Array:
        out.push_back('[');
        for (size_t i = 0; i < a_.size(); ++i) {
            if (i) out.push_back(',');
            a_[i].dump_to(out);
        }
        out.push_back(']');
        break;
    case Type::Object:
        out.push_back('{');
        for (size_t i = 0; i < o_.size(); ++i) {
            if (i) out.push_back(',');
            escape_string(o_[i].first, out);
            out.push_back(':');
            o_[i].second.dump_to(out);
        }
        out.push_back('}');
        break;
    }
}

std::string Value::dump() const {
    std::string s;
    dump_to(s);
    return s;
}

} 
