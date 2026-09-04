// jsjson.h — a JSON value with V8 `JSON.stringify`/`JSON.parse` semantics and
// the tool's `stableStringify` (recursive key sort, JS default sort order).
#ifndef pocev_JSJSON_H
#define pocev_JSJSON_H

#include <string>
#include <utility>
#include <vector>

namespace pocev {

class Json {
public:
    enum class T { Null, Bool, Num, Str, Arr, Obj };

    T t = T::Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;  // insertion order

    static Json null() { return Json(); }
    static Json boolean(bool v);
    static Json number(double v);
    static Json string(std::string v);
    static Json array(std::vector<Json> v = {});
    static Json object();

    bool isNull() const { return t == T::Null; }
    bool isObj() const { return t == T::Obj; }
    bool isArr() const { return t == T::Arr; }
    bool isStr() const { return t == T::Str; }
    bool isNum() const { return t == T::Num; }
    bool isBool() const { return t == T::Bool; }

    // Insertion-ordered lookup; nullptr when absent.
    Json* get(const std::string& key);
    const Json* get(const std::string& key) const;

    // Replace the value when the key exists (keeping its position), else append.
    void set(const std::string& key, Json v);
    void push(Json v) { arr.push_back(std::move(v)); }

    // JSON.stringify(v) — compact. NaN/Infinity become null.
    std::string dump() const;
    // JSON.stringify(v, null, 2).
    std::string dumpIndent() const;

    // JSON.parse(text): false on any malformed input (V8 throws).
    static bool parse(const std::string& text, Json& out);
};

// util.mjs stableStringify: deep key sort (UTF-16 code-unit order), then
// JSON.stringify.
std::string stableStringify(const Json& v);

// String(value) for numbers — ECMA-262 Number::toString layout. NaN/Infinity
// spell out like the JS namesake.
std::string jsNumberToString(double v);

// Array.prototype.sort() default string comparison (UTF-16 code units).
bool jsStringLess(const std::string& a, const std::string& b);

}  // namespace pocev

#endif  // pocev_JSJSON_H
