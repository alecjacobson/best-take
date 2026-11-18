#include "bt/json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace bt::json {

void Value::set(const std::string& key, Value v) {
    if (type != Type::Object) throw std::invalid_argument("json: set on a non-object");
    for (auto& kv : object)
        if (kv.first == key) {
            kv.second = std::move(v);
            return;
        }
    object.emplace_back(key, std::move(v));
}

const Value& Value::at(const std::string& key) const {
    if (type != Type::Object) throw std::invalid_argument("json: member lookup on a non-object");
    for (const auto& kv : object)
        if (kv.first == key) return kv.second;
    throw std::invalid_argument("json: missing member '" + key + "'");
}

bool Value::has(const std::string& key) const {
    if (type != Type::Object) return false;
    for (const auto& kv : object)
        if (kv.first == key) return true;
    return false;
}

double Value::as_number() const {
    if (type != Type::Number) throw std::invalid_argument("json: expected a number");
    return number;
}
const std::string& Value::as_string() const {
    if (type != Type::String) throw std::invalid_argument("json: expected a string");
    return string;
}
const std::vector<Value>& Value::as_array() const {
    if (type != Type::Array) throw std::invalid_argument("json: expected an array");
    return array;
}

static void dump_string(const std::string& s, std::string& out) {
    out += '"';
    for (unsigned char ch : s) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", ch);
                    out += buf;
                } else {
                    out += char(ch);
                }
        }
    }
    out += '"';
}

static void dump_value(const Value& v, std::string& out) {
    switch (v.type) {
        case Value::Type::Null: out += "null"; break;
        case Value::Type::Bool: out += v.boolean ? "true" : "false"; break;
        case Value::Type::Number: {
            if (!std::isfinite(v.number)) throw std::invalid_argument("json: non-finite number");
            char buf[40];
            std::snprintf(buf, sizeof buf, "%.17g", v.number);
            out += buf;
            break;
        }
        case Value::Type::String: dump_string(v.string, out); break;
        case Value::Type::Array:
            out += '[';
            for (size_t i = 0; i < v.array.size(); ++i) {
                if (i) out += ',';
                dump_value(v.array[i], out);
            }
            out += ']';
            break;
        case Value::Type::Object:
            out += '{';
            for (size_t i = 0; i < v.object.size(); ++i) {
                if (i) out += ',';
                dump_string(v.object[i].first, out);
                out += ':';
                dump_value(v.object[i].second, out);
            }
            out += '}';
            break;
    }
}

std::string dump(const Value& v) {
    std::string out;
    dump_value(v, out);
    return out;
}

namespace {
struct Parser {
    const std::string& s;
    size_t i = 0;
    [[noreturn]] void fail(const char* what) const {
        throw std::invalid_argument("json: " + std::string(what) + " at offset " + std::to_string(i));
    }
    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\t' || s[i] == '\r')) ++i;
    }
    bool eat(char c) {
        ws();
        if (i < s.size() && s[i] == c) {
            ++i;
            return true;
        }
        return false;
    }
    std::string str() {
        if (!eat('"')) fail("expected string");
        std::string out;
        while (true) {
            if (i >= s.size()) fail("unterminated string");
            char c = s[i++];
            if (c == '"') break;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= s.size()) fail("unterminated escape");
            char e = s[i++];
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    if (i + 4 > s.size()) fail("short unicode escape");
                    unsigned cp = unsigned(std::strtoul(s.substr(i, 4).c_str(), nullptr, 16));
                    i += 4;
                    if (cp < 0x80) {
                        out += char(cp);
                    } else if (cp < 0x800) {
                        out += char(0xC0 | (cp >> 6));
                        out += char(0x80 | (cp & 0x3F));
                    } else {
                        out += char(0xE0 | (cp >> 12));
                        out += char(0x80 | ((cp >> 6) & 0x3F));
                        out += char(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: out += e;
            }
        }
        return out;
    }
    Value value() {
        ws();
        if (i >= s.size()) fail("unexpected end");
        char c = s[i];
        if (c == '{') {
            ++i;
            Value v = Value::make_object();
            if (eat('}')) return v;
            do {
                std::string k = str();
                if (!eat(':')) fail("expected ':'");
                v.object.emplace_back(std::move(k), value());
            } while (eat(','));
            if (!eat('}')) fail("expected '}'");
            return v;
        }
        if (c == '[') {
            ++i;
            Value v = Value::make_array();
            if (eat(']')) return v;
            do v.array.push_back(value());
            while (eat(','));
            if (!eat(']')) fail("expected ']'");
            return v;
        }
        if (c == '"') return Value(str());
        if (s.compare(i, 4, "true") == 0) return i += 4, Value(true);
        if (s.compare(i, 5, "false") == 0) return i += 5, Value(false);
        if (s.compare(i, 4, "null") == 0) return i += 4, Value();
        const char* begin = s.c_str() + i;
        char* end = nullptr;
        double d = std::strtod(begin, &end);
        if (end == begin) fail("unexpected token");
        i += size_t(end - begin);
        return Value(d);
    }
};
}  // namespace

Value parse(const std::string& text) {
    Parser p{text};
    Value v = p.value();
    p.ws();
    if (p.i != text.size()) p.fail("trailing characters");
    return v;
}

}  // namespace bt::json
