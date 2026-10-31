#pragma once
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cstdint>

namespace mcus {

class JsonValue {
public:
    enum Type { Null, Bool, Number, String, Array, Object };

    JsonValue() : type_(Null) {}
    explicit JsonValue(bool b) : type_(Bool), bool_(b) {}
    explicit JsonValue(double n) : type_(Number), num_(n) {}
    explicit JsonValue(const std::string& s) : type_(String), str_(s) {}

    Type type() const { return type_; }
    bool is_null() const { return type_ == Null; }
    bool as_bool() const { return bool_; }
    double as_number() const { return num_; }
    int64_t as_int() const { return static_cast<int64_t>(num_); }
    const std::string& as_string() const { return str_; }

    const std::vector<JsonValue>& as_array() const { return arr_; }
    const std::map<std::string, JsonValue>& as_object() const { return obj_; }

    bool has(const std::string& key) const {
        return type_ == Object && obj_.count(key) > 0;
    }
    const JsonValue& operator[](const std::string& key) const {
        static JsonValue null_val;
        auto it = obj_.find(key);
        return it == obj_.end() ? null_val : it->second;
    }
    const JsonValue& operator[](size_t i) const {
        static JsonValue null_val;
        return i < arr_.size() ? arr_[i] : null_val;
    }
    size_t size() const {
        if (type_ == Array) return arr_.size();
        if (type_ == Object) return obj_.size();
        return 0;
    }

    static JsonValue parse(const std::string& text);
    static JsonValue parse_file(const std::string& path);

private:
    friend class JsonParser;
    Type type_;
    bool bool_ = false;
    double num_ = 0;
    std::string str_;
    std::vector<JsonValue> arr_;
    std::map<std::string, JsonValue> obj_;
};

} // namespace mcus