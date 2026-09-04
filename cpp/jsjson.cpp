// jsjson.cpp — see jsjson.h. Byte-compatible with V8's JSON.stringify
// (compact and 2-space indent), JSON.parse and the tool's stableStringify.
#include "jsjson.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace pocev {

namespace {

// ------------------------------------------------------------------ UTF-16

// Decode UTF-8 to a UTF-16 code-unit sequence (replacement chars on garbage),
// preserving lone surrogates as-is (they round-trip through JS strings).
std::vector<uint16_t> utf8ToUtf16(const std::string& s) {
    std::vector<uint16_t> out;
    out.reserve(s.size());
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t r = 0xFFFD;
        size_t len = 1;
        if (c < 0x80) {
            r = c;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < n &&
                   (static_cast<unsigned char>(s[i + 1]) & 0xC0) == 0x80) {
            r = (static_cast<uint32_t>(c & 0x1F) << 6) |
                (static_cast<unsigned char>(s[i + 1]) & 0x3F);
            len = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < n &&
                   (static_cast<unsigned char>(s[i + 1]) & 0xC0) == 0x80 &&
                   (static_cast<unsigned char>(s[i + 2]) & 0xC0) == 0x80) {
            r = (static_cast<uint32_t>(c & 0x0F) << 12) |
                (static_cast<uint32_t>(static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
                (static_cast<unsigned char>(s[i + 2]) & 0x3F);
            len = 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < n &&
                   (static_cast<unsigned char>(s[i + 1]) & 0xC0) == 0x80 &&
                   (static_cast<unsigned char>(s[i + 2]) & 0xC0) == 0x80 &&
                   (static_cast<unsigned char>(s[i + 3]) & 0xC0) == 0x80) {
            r = (static_cast<uint32_t>(c & 0x07) << 18) |
                (static_cast<uint32_t>(static_cast<unsigned char>(s[i + 1]) & 0x3F) << 12) |
                (static_cast<uint32_t>(static_cast<unsigned char>(s[i + 2]) & 0x3F) << 6) |
                (static_cast<unsigned char>(s[i + 3]) & 0x3F);
            len = 4;
        }
        if (r > 0xFFFF) {
            uint32_t v = r - 0x10000;
            out.push_back(static_cast<uint16_t>(0xD800 + (v >> 10)));
            out.push_back(static_cast<uint16_t>(0xDC00 + (v & 0x3FF)));
        } else {
            out.push_back(static_cast<uint16_t>(r));
        }
        i += len;
    }
    return out;
}

}  // namespace

bool jsStringLess(const std::string& a, const std::string& b) {
    std::vector<uint16_t> ua = utf8ToUtf16(a);
    std::vector<uint16_t> ub = utf8ToUtf16(b);
    size_t n = std::min(ua.size(), ub.size());
    for (size_t i = 0; i < n; i++) {
        if (ua[i] != ub[i]) return ua[i] < ub[i];
    }
    return ua.size() < ub.size();
}

// ------------------------------------------------------------------ numbers

std::string jsNumberToString(double v) {
    if (std::isnan(v)) return "NaN";
    if (std::isinf(v)) return v > 0 ? "Infinity" : "-Infinity";
    if (v == 0) return "0";  // covers +0 and -0

    // Shortest round-tripping digits via printf + strtod probe.
    char buf[64];
    for (int prec = 1; prec <= 17; prec++) {
        std::snprintf(buf, sizeof buf, "%.*e", prec - 1, v);
        double w = std::strtod(buf, nullptr);
        if (std::memcmp(&w, &v, 8) == 0) break;
    }
    const char* s = buf;
    bool neg = (*s == '-');
    if (neg) s++;
    std::string digits;
    digits += *s++;
    if (*s == '.') {
        s++;
        while (*s != 'e' && *s != '\0') digits += *s++;
    }
    while (*s != 'e' && *s != '\0') s++;
    int exp10 = std::atoi(s + 1);
    int n = exp10 + 1;   // decimal point position: value = 0.digits x 10^n
    int k = static_cast<int>(digits.size());

    std::string out;
    if (k <= n && n <= 21) {
        out = digits + std::string(static_cast<size_t>(n - k), '0');
    } else if (0 < n && n <= 21) {
        out = digits.substr(0, static_cast<size_t>(n)) + "." +
              digits.substr(static_cast<size_t>(n));
    } else if (-6 < n && n <= 0) {
        out = "0." + std::string(static_cast<size_t>(-n), '0') + digits;
    } else {
        out = digits.substr(0, 1);
        if (k > 1) out += "." + digits.substr(1);
        int e = n - 1;
        char ebuf[16];
        std::snprintf(ebuf, sizeof ebuf, "e%c%d", e < 0 ? '-' : '+', e < 0 ? -e : e);
        out += ebuf;
    }
    return neg ? "-" + out : out;
}

namespace {

// ---------------------------------------------------------------- stringify

void quoteJsonString(const std::string& s, std::string& out) {
    out += '"';
    std::vector<uint16_t> u = utf8ToUtf16(s);
    char hex[8];
    for (size_t i = 0; i < u.size(); i++) {
        uint16_t cu = u[i];
        switch (cu) {
            case '"': out += "\\\""; continue;
            case '\\': out += "\\\\"; continue;
            case '\b': out += "\\b"; continue;
            case '\t': out += "\\t"; continue;
            case '\n': out += "\\n"; continue;
            case '\f': out += "\\f"; continue;
            case '\r': out += "\\r"; continue;
            default: break;
        }
        if (cu < 0x20) {
            std::snprintf(hex, sizeof hex, "\\u%04x", cu);
            out += hex;
            continue;
        }
        if (cu >= 0xD800 && cu <= 0xDFFF) {
            // Well-formed stringify: valid pairs pass through, lone
            // surrogates escape.
            bool paired = cu <= 0xDBFF && i + 1 < u.size() && u[i + 1] >= 0xDC00 &&
                          u[i + 1] <= 0xDFFF;
            if (!paired) {
                std::snprintf(hex, sizeof hex, "\\u%04x", cu);
                out += hex;
                continue;
            }
            uint32_t cp = (static_cast<uint32_t>(cu - 0xD800) << 10) +
                          (u[i + 1] - 0xDC00) + 0x10000;
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
            i++;
            continue;
        }
        if (cu < 0x80) {
            out += static_cast<char>(cu);
        } else if (cu < 0x800) {
            out += static_cast<char>(0xC0 | (cu >> 6));
            out += static_cast<char>(0x80 | (cu & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (cu >> 12));
            out += static_cast<char>(0x80 | ((cu >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cu & 0x3F));
        }
    }
    out += '"';
}

void dumpValue(const Json& v, std::string& out, int depth, bool indent) {
    switch (v.t) {
        case Json::T::Null:
            out += "null";
            break;
        case Json::T::Bool:
            out += v.b ? "true" : "false";
            break;
        case Json::T::Num:
            if (std::isnan(v.num) || std::isinf(v.num)) {
                out += "null";  // JSON.stringify maps non-finite numbers to null
            } else {
                out += jsNumberToString(v.num);
            }
            break;
        case Json::T::Str:
            quoteJsonString(v.str, out);
            break;
        case Json::T::Arr: {
            if (v.arr.empty()) {
                out += "[]";
                break;
            }
            out += "[";
            for (size_t i = 0; i < v.arr.size(); i++) {
                if (i > 0) out += ",";
                if (indent) out += "\n" + std::string(static_cast<size_t>(depth + 1) * 2, ' ');
                dumpValue(v.arr[i], out, depth + 1, indent);
            }
            if (indent) out += "\n" + std::string(static_cast<size_t>(depth) * 2, ' ');
            out += "]";
            break;
        }
        case Json::T::Obj: {
            if (v.obj.empty()) {
                out += "{}";
                break;
            }
            out += "{";
            for (size_t i = 0; i < v.obj.size(); i++) {
                if (i > 0) out += ",";
                if (indent) out += "\n" + std::string(static_cast<size_t>(depth + 1) * 2, ' ');
                quoteJsonString(v.obj[i].first, out);
                out += indent ? ": " : ":";
                dumpValue(v.obj[i].second, out, depth + 1, indent);
            }
            if (indent) out += "\n" + std::string(static_cast<size_t>(depth) * 2, ' ');
            out += "}";
            break;
        }
    }
}

// ------------------------------------------------------------------- parse

struct Parser {
    const std::string& s;
    size_t pos = 0;
    int depth = 0;

    explicit Parser(const std::string& src) : s(src) {}

    bool ws() {
        while (pos < s.size()) {
            char c = s[pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') pos++;
            else break;
        }
        return true;
    }

    bool literal(const char* lit) {
        size_t n = std::strlen(lit);
        if (s.compare(pos, n, lit) != 0) return false;
        pos += n;
        return true;
    }

    bool parseValue(Json& out, bool& ok) {
        if (++depth > 1000) { ok = false; return false; }
        ws();
        if (pos >= s.size()) { ok = false; return false; }
        char c = s[pos];
        bool good = false;
        if (c == '{') good = parseObject(out, ok);
        else if (c == '[') good = parseArray(out, ok);
        else if (c == '"') { std::string str; good = parseString(str, ok); if (good) { out = Json::string(std::move(str)); } }
        else if (c == 't') { good = literal("true"); if (good) out = Json::boolean(true); }
        else if (c == 'f') { good = literal("false"); if (good) out = Json::boolean(false); }
        else if (c == 'n') { good = literal("null"); if (good) out = Json::null(); }
        else if (c == '-' || (c >= '0' && c <= '9')) good = parseNumber(out, ok);
        if (!good) ok = false;
        depth--;
        return good;
    }

    bool parseNumber(Json& out, bool& ok) {
        size_t start = pos;
        if (pos < s.size() && s[pos] == '-') pos++;
        if (pos >= s.size() || s[pos] < '0' || s[pos] > '9') { ok = false; return false; }
        if (s[pos] == '0') {
            pos++;
        } else {
            while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') pos++;
        }
        if (pos < s.size() && s[pos] == '.') {
            pos++;
            if (pos >= s.size() || s[pos] < '0' || s[pos] > '9') { ok = false; return false; }
            while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') pos++;
        }
        if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
            pos++;
            if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) pos++;
            if (pos >= s.size() || s[pos] < '0' || s[pos] > '9') { ok = false; return false; }
            while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') pos++;
        }
        std::string tok = s.substr(start, pos - start);
        out = Json::number(std::strtod(tok.c_str(), nullptr));
        return true;
    }

    bool hex4(uint32_t& v) {
        if (pos + 4 > s.size()) return false;
        v = 0;
        for (int i = 0; i < 4; i++) {
            char c = s[pos + static_cast<size_t>(i)];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else return false;
        }
        pos += 4;
        return true;
    }

    void pushUtf16(std::string& out, uint32_t r) {
        if (r < 0x80) {
            out += static_cast<char>(r);
        } else if (r < 0x800) {
            out += static_cast<char>(0xC0 | (r >> 6));
            out += static_cast<char>(0x80 | (r & 0x3F));
        } else if (r < 0x10000) {
            // BMP — includes lone surrogates, kept verbatim like JS strings do.
            out += static_cast<char>(0xE0 | (r >> 12));
            out += static_cast<char>(0x80 | ((r >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (r & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (r >> 18));
            out += static_cast<char>(0x80 | ((r >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((r >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (r & 0x3F));
        }
    }

    bool parseString(std::string& out, bool& ok) {
        pos++;  // opening quote
        std::vector<uint32_t> units;  // UTF-16 code units, pairs joined below
        while (true) {
            if (pos >= s.size()) { ok = false; return false; }
            char32_t c = static_cast<unsigned char>(s[pos]);
            if (c == '"') {
                pos++;
                break;
            }
            if (c == '\\') {
                pos++;
                if (pos >= s.size()) { ok = false; return false; }
                char e = s[pos];
                switch (e) {
                    case '"': units.push_back('"'); pos++; continue;
                    case '\\': units.push_back('\\'); pos++; continue;
                    case '/': units.push_back('/'); pos++; continue;
                    case 'b': units.push_back('\b'); pos++; continue;
                    case 'f': units.push_back('\f'); pos++; continue;
                    case 'n': units.push_back('\n'); pos++; continue;
                    case 'r': units.push_back('\r'); pos++; continue;
                    case 't': units.push_back('\t'); pos++; continue;
                    case 'u': {
                        pos++;
                        uint32_t v;
                        if (!hex4(v)) { ok = false; return false; }
                        units.push_back(v);
                        continue;
                    }
                    default:
                        ok = false;
                        return false;
                }
            }
            if (c < 0x20) { ok = false; return false; }
            // Raw UTF-8 rune.
            size_t len = 1;
            uint32_t r = c;
            if ((c & 0xE0) == 0xC0 && pos + 1 < s.size() &&
                (static_cast<unsigned char>(s[pos + 1]) & 0xC0) == 0x80) {
                r = (static_cast<uint32_t>(c & 0x1F) << 6) |
                    (static_cast<unsigned char>(s[pos + 1]) & 0x3F);
                len = 2;
            } else if ((c & 0xF0) == 0xE0 && pos + 2 < s.size() &&
                       (static_cast<unsigned char>(s[pos + 1]) & 0xC0) == 0x80 &&
                       (static_cast<unsigned char>(s[pos + 2]) & 0xC0) == 0x80) {
                r = (static_cast<uint32_t>(c & 0x0F) << 12) |
                    (static_cast<uint32_t>(static_cast<unsigned char>(s[pos + 1]) & 0x3F) << 6) |
                    (static_cast<unsigned char>(s[pos + 2]) & 0x3F);
                len = 3;
            } else if ((c & 0xF8) == 0xF0 && pos + 3 < s.size() &&
                       (static_cast<unsigned char>(s[pos + 1]) & 0xC0) == 0x80 &&
                       (static_cast<unsigned char>(s[pos + 2]) & 0xC0) == 0x80 &&
                       (static_cast<unsigned char>(s[pos + 3]) & 0xC0) == 0x80) {
                r = (static_cast<uint32_t>(c & 0x07) << 18) |
                    (static_cast<uint32_t>(static_cast<unsigned char>(s[pos + 1]) & 0x3F) << 12) |
                    (static_cast<uint32_t>(static_cast<unsigned char>(s[pos + 2]) & 0x3F) << 6) |
                    (static_cast<unsigned char>(s[pos + 3]) & 0x3F);
                len = 4;
                // Store surrogate pair so lone-surrogate escapes stay pairable.
                units.push_back(0xD800 + ((r - 0x10000) >> 10));
                units.push_back(0xDC00 + ((r - 0x10000) & 0x3FF));
                pos += len;
                continue;
            }
            units.push_back(r);
            pos += len;
        }
        // Join UTF-16 units into UTF-8, pairing surrogates when they line up.
        for (size_t i = 0; i < units.size(); i++) {
            uint32_t u = units[i];
            if (u >= 0xD800 && u <= 0xDBFF && i + 1 < units.size() &&
                units[i + 1] >= 0xDC00 && units[i + 1] <= 0xDFFF) {
                uint32_t cp = 0x10000 + ((u - 0xD800) << 10) + (units[i + 1] - 0xDC00);
                pushUtf16(out, cp);
                i++;
            } else {
                pushUtf16(out, u);
            }
        }
        return true;
    }

    bool parseArray(Json& out, bool& ok) {
        pos++;  // '['
        out = Json::array();
        ws();
        if (pos < s.size() && s[pos] == ']') {
            pos++;
            return true;
        }
        while (true) {
            Json item;
            if (!parseValue(item, ok) || !ok) return false;
            out.arr.push_back(std::move(item));
            ws();
            if (pos >= s.size()) { ok = false; return false; }
            if (s[pos] == ',') {
                pos++;
                continue;
            }
            if (s[pos] == ']') {
                pos++;
                return true;
            }
            ok = false;
            return false;
        }
    }

    bool parseObject(Json& out, bool& ok) {
        pos++;  // '{'
        out = Json::object();
        ws();
        if (pos < s.size() && s[pos] == '}') {
            pos++;
            return true;
        }
        while (true) {
            ws();
            if (pos >= s.size() || s[pos] != '"') { ok = false; return false; }
            std::string key;
            if (!parseString(key, ok) || !ok) return false;
            ws();
            if (pos >= s.size() || s[pos] != ':') { ok = false; return false; }
            pos++;
            Json item;
            if (!parseValue(item, ok) || !ok) return false;
            out.set(key, std::move(item));  // duplicate keys: last wins, first slot
            ws();
            if (pos >= s.size()) { ok = false; return false; }
            if (s[pos] == ',') {
                pos++;
                continue;
            }
            if (s[pos] == '}') {
                pos++;
                return true;
            }
            ok = false;
            return false;
        }
    }
};

// -------------------------------------------------------- stableStringify

Json sortedDeep(const Json& v) {
    switch (v.t) {
        case Json::T::Arr: {
            Json out = Json::array();
            out.arr.reserve(v.arr.size());
            for (const auto& item : v.arr) out.arr.push_back(sortedDeep(item));
            return out;
        }
        case Json::T::Obj: {
            Json out = Json::object();
            std::vector<std::pair<std::string, Json>> entries = v.obj;
            std::stable_sort(entries.begin(), entries.end(),
                             [](const auto& a, const auto& b) {
                                 return jsStringLess(a.first, b.first);
                             });
            for (auto& e : entries) out.obj.emplace_back(e.first, sortedDeep(e.second));
            return out;
        }
        default:
            return v;
    }
}

}  // namespace

Json Json::boolean(bool v) {
    Json j;
    j.t = T::Bool;
    j.b = v;
    return j;
}

Json Json::number(double v) {
    Json j;
    j.t = T::Num;
    j.num = v;
    return j;
}

Json Json::string(std::string v) {
    Json j;
    j.t = T::Str;
    j.str = std::move(v);
    return j;
}

Json Json::array(std::vector<Json> v) {
    Json j;
    j.t = T::Arr;
    j.arr = std::move(v);
    return j;
}

Json Json::object() {
    Json j;
    j.t = T::Obj;
    return j;
}

Json* Json::get(const std::string& key) {
    for (auto& e : obj) {
        if (e.first == key) return &e.second;
    }
    return nullptr;
}

const Json* Json::get(const std::string& key) const {
    for (const auto& e : obj) {
        if (e.first == key) return &e.second;
    }
    return nullptr;
}

void Json::set(const std::string& key, Json v) {
    for (auto& e : obj) {
        if (e.first == key) {
            e.second = std::move(v);
            return;
        }
    }
    obj.emplace_back(key, std::move(v));
}

std::string Json::dump() const {
    std::string out;
    dumpValue(*this, out, 0, false);
    return out;
}

std::string Json::dumpIndent() const {
    std::string out;
    dumpValue(*this, out, 0, true);
    return out;
}

bool Json::parse(const std::string& text, Json& out) {
    Parser p(text);
    bool ok = true;
    if (!p.parseValue(out, ok) || !ok) return false;
    p.ws();
    return p.pos == text.size();
}

std::string stableStringify(const Json& v) {
    return sortedDeep(v).dump();
}

}  // namespace pocev
