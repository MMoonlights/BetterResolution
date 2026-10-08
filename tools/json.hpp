#pragma once



#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace brjson {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : type_(Type::Bool), b_(b) {}
    Value(int v) : type_(Type::Number), n_(v) {}
    Value(int64_t v) : type_(Type::Number), n_(static_cast<double>(v)) {}
    Value(uint64_t v) : type_(Type::Number), n_(static_cast<double>(v)) {}
    Value(uint32_t v) : type_(Type::Number), n_(v) {}
    Value(double v) : type_(Type::Number), n_(v) {}
    Value(const char* s) : type_(Type::String), s_(s ? s : "") {}
    Value(std::string s) : type_(Type::String), s_(std::move(s)) {}

    static Value array() { Value v; v.type_ = Type::Array; return v; }
    static Value object() { Value v; v.type_ = Type::Object; return v; }

    const std::string& as_string() const { return s_; }

    const Value& operator[](size_t i) const { return i < a_.size() ? a_[i] : null_value(); }
    Value& push(Value v) { type_ = Type::Array; a_.push_back(std::move(v)); return a_.back(); }
    const std::vector<Value>& items() const { return a_; }

    // Объекты с сохранением порядка добавления элементов.
    bool has(const std::string& k) const;
    const Value& operator[](const std::string& k) const;
    const Value& operator[](const char* k) const { return (*this)[std::string(k)]; }
    Value& set(const std::string& k, Value v);

    std::string dump() const;
    void dump_to(std::string& out) const;

    static const Value& null_value();

private:
    Type type_{Type::Null};
    bool b_{false};
    double n_{0};
    std::string s_;
    std::vector<Value> a_;
    std::vector<std::pair<std::string, Value>> o_;
};

void escape_string(const std::string& s, std::string& out);

} 
