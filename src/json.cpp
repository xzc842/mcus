#include "mcus/json.h"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <cctype>

namespace mcus {

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : text_(text) {}

    JsonValue parse() {
        skip_ws();
        JsonValue v = parse_value();
        skip_ws();
        if (pos_ != text_.size()) throw std::runtime_error("trailing JSON");
        return v;
    }

private:
    const std::string& text_;
    size_t pos_ = 0;

    void skip_ws() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) pos_++;
    }

    char peek() {
        if (pos_ >= text_.size()) throw std::runtime_error("unexpected EOF");
        return text_[pos_];
    }

    char get() {
        if (pos_ >= text_.size()) throw std::runtime_error("unexpected EOF");
        return text_[pos_++];
    }

    void expect(char c) {
        if (get() != c) throw std::runtime_error(std::string("expected ") + c);
    }

    JsonValue parse_value() {
        skip_ws();
        char c = peek();
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == '"') return JsonValue(parse_string());
        if (c == 't') { parse_literal("true"); return JsonValue(true); }
        if (c == 'f') { parse_literal("false"); return JsonValue(false); }
        if (c == 'n') { parse_literal("null"); return JsonValue(); }
        return parse_number();
    }

    void parse_literal(const char* lit) {
        for (const char* p = lit; *p; ++p) {
            if (get() != *p) throw std::runtime_error("bad literal");
        }
    }

    JsonValue parse_object() {
        JsonValue v;
        v.type_ = JsonValue::Object;
        expect('{');
        skip_ws();
        if (peek() == '}') { get(); return v; }
        while (true) {
            skip_ws();
            std::string key = parse_string();
            skip_ws();
            expect(':');
            v.obj_[key] = parse_value();
            skip_ws();
            char c = get();
            if (c == '}') break;
            if (c != ',') throw std::runtime_error("expected , or }");
        }
        return v;
    }

    JsonValue parse_array() {
        JsonValue v;
        v.type_ = JsonValue::Array;
        expect('[');
        skip_ws();
        if (peek() == ']') { get(); return v; }
        while (true) {
            v.arr_.push_back(parse_value());
            skip_ws();
            char c = get();
            if (c == ']') break;
            if (c != ',') throw std::runtime_error("expected , or ]");
        }
        return v;
    }

    std::string parse_string() {
        expect('"');
        std::string s;
        while (true) {
            char c = get();
            if (c == '"') break;
            if (c == '\\') {
                char e = get();
                switch (e) {
                    case '"': s += '"'; break;
                    case '\\': s += '\\'; break;
                    case '/': s += '/'; break;
                    case 'n': s += '\n'; break;
                    case 't': s += '\t'; break;
                    case 'r': s += '\r'; break;
                    case 'b': s += '\b'; break;
                    case 'f': s += '\f'; break;
                    default: throw std::runtime_error("bad escape");
                }
            } else {
                s += c;
            }
        }
        return s;
    }

    JsonValue parse_number() {
        size_t start = pos_;
        if (peek() == '-') get();
        while (pos_ < text_.size() && (std::isdigit(static_cast<unsigned char>(text_[pos_])) || text_[pos_] == '.' ||
                                       text_[pos_] == 'e' || text_[pos_] == 'E' || text_[pos_] == '+' || text_[pos_] == '-')) pos_++;
        return JsonValue(std::stod(text_.substr(start, pos_ - start)));
    }
};

JsonValue JsonValue::parse(const std::string& text) {
    JsonParser p(text);
    return p.parse();
}

JsonValue JsonValue::parse_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::stringstream ss;
    ss << f.rdbuf();
    return parse(ss.str());
}

} // namespace mcus