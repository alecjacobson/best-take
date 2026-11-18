// Minimal JSON value, writer, and parser for persisted engine records.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace bt::json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;  ///< Insertion-ordered.

    Value() = default;
    Value(bool b) : type(Type::Bool), boolean(b) {}
    Value(double n) : type(Type::Number), number(n) {}
    Value(int n) : type(Type::Number), number(n) {}
    Value(const char* s) : type(Type::String), string(s) {}
    Value(std::string s) : type(Type::String), string(std::move(s)) {}
    static Value make_array() { Value v; v.type = Type::Array; return v; }
    static Value make_object() { Value v; v.type = Type::Object; return v; }

    void set(const std::string& key, Value v);
    /// Member lookup; throws std::invalid_argument when absent or not an object.
    const Value& at(const std::string& key) const;
    bool has(const std::string& key) const;
    double as_number() const;
    const std::string& as_string() const;
    const std::vector<Value>& as_array() const;
};

/// Serialize with round-trip-exact numbers and stable member order.
std::string dump(const Value& v);
/// Parse a document; throws std::invalid_argument on malformed input.
Value parse(const std::string& text);

}  // namespace bt::json
