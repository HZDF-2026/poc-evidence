// jsre.cpp — see jsre.h. The engine works on UTF-16 code units like V8's
// non-unicode mode: '.' matches a surrogate half, lengths and indices are in
// code units, and lone surrogates are ordinary characters. With the u flag the
// known behavioral differences are kept small ('.' and atoms consume whole
// surrogate pairs).
#include "jsre.h"

#include "jsfold.h"
#include "jsprops.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace pocev {

namespace {

// ------------------------------------------------------------------ helpers

std::u16string utf8ToU16(const std::string& s) {
    std::u16string out;
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
            out.push_back(static_cast<char16_t>(0xD800 + (v >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (v & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(r));
        }
        i += len;
    }
    return out;
}

std::string u16SliceToUtf8(const std::u16string& s, int from, int to) {
    std::string out;
    if (to <= from) return out;
    out.reserve(static_cast<size_t>(to - from));
    for (int i = from; i < to; i++) {
        uint32_t r = static_cast<uint32_t>(s[static_cast<size_t>(i)]);
        if (r < 0x80) {
            out += static_cast<char>(r);
        } else if (r < 0x800) {
            out += static_cast<char>(0xC0 | (r >> 6));
            out += static_cast<char>(0x80 | (r & 0x3F));
        } else if (r < 0x10000) {
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
    return out;
}

bool isLineTerminator(uint16_t c) {
    return c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029;
}

// Case handling comes from the generated tables in jsfold.cpp:
//   canonNu — ES5.1 Canonicalize (non-unicode i mode), full BMP coverage.
//   foldRep — fold-class representative (unicode i mode); two code points
//             match case-insensitively iff their representatives are equal.

bool isJsWordChar(uint16_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_';
}

struct Span {
    uint16_t lo, hi;
};

struct Flags {
    bool g = false, i = false, m = false, s = false, u = false, y = false;
};

// ---------------------------------------------------------------- AST nodes

struct Node {
    virtual ~Node() = default;
};
using Seq = std::vector<Node*>;

struct Lit : Node {
    uint32_t cp;  // > 0xFFFF only in u mode (\u{...} astral): matches a pair
    explicit Lit(uint32_t c) : cp(c) {}
};

struct Cls : Node {
    std::vector<Span> spans;
    std::vector<uint32_t> cps;  // u-mode astral members (\u{...} in classes)
    std::vector<std::pair<uint32_t, uint32_t>> astral;  // astral ranges (\p{...})
    bool neg = false;

    bool matchesRaw(uint16_t c) const {
        size_t lo = 0, hi = spans.size();
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            const Span& s = spans[mid];
            if (c < s.lo) hi = mid;
            else if (c > s.hi) lo = mid + 1;
            else return true;
        }
        return false;
    }

    bool matchesAstral(uint32_t cp) const {
        for (uint32_t want : cps) {
            if (want == cp) return true;
        }
        size_t lo = 0, hi = astral.size();
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (cp < astral[mid].first) hi = mid;
            else if (cp > astral[mid].second) lo = mid + 1;
            else return true;
        }
        return false;
    }
};

struct Dot : Node {};
struct Caret : Node {};
struct Dollar : Node {};
struct WordB : Node { bool neg = false; };

struct Alt : Node {
    std::vector<Seq> alts;
};

struct Group : Node {
    int idx = 0;  // 0 = non-capturing
    std::vector<Seq> alts;
};

struct Look : Node {
    std::vector<Seq> alts;
    bool behind = false;
    bool neg = false;
};

struct Rep : Node {
    Node* node = nullptr;
    int min = 0;
    int max = -1;
    bool lazy = false;
};

struct Backref : Node {
    int idx = 0;
    explicit Backref(int i) : idx(i) {}
};

// ----------------------------------------------------------------- matching

struct State {
    const std::u16string& s;
    std::vector<std::optional<std::pair<int, int>>>& caps;
    const Flags& f;

    State(const std::u16string& subject,
          std::vector<std::optional<std::pair<int, int>>>& groups, const Flags& flags)
        : s(subject), caps(groups), f(flags) {}

    using Cont = std::function<bool(int)>;

    bool matchSeq(const Seq& nodes, size_t from, int pos, const Cont& cont) const;
    bool matchRep(const Rep& v, int pos, int count, const Cont& cont) const;
    bool matchAtom(const Node& n, int pos, const Cont& cont) const;
    bool eqChar(uint16_t a, uint16_t b) const {
        if (!f.i) return a == b;
        if (f.u) return foldRep(a) == foldRep(b);
        return canonNu(a) == canonNu(b);
    }

    // One unit of consumption, u-aware: whole surrogate pairs in u mode.
    int unitLen(int pos) const {
        if (f.u && pos + 1 < static_cast<int>(s.size()) &&
            s[static_cast<size_t>(pos)] >= 0xD800 && s[static_cast<size_t>(pos)] <= 0xDBFF &&
            s[static_cast<size_t>(pos + 1)] >= 0xDC00 && s[static_cast<size_t>(pos + 1)] <= 0xDFFF) {
            return 2;
        }
        return 1;
    }

    uint32_t codePointAt(int pos) const {
        uint16_t c = s[static_cast<size_t>(pos)];
        if (c >= 0xD800 && c <= 0xDBFF && pos + 1 < static_cast<int>(s.size()) &&
            s[static_cast<size_t>(pos + 1)] >= 0xDC00 &&
            s[static_cast<size_t>(pos + 1)] <= 0xDFFF) {
            return (static_cast<uint32_t>(c - 0xD800) << 10) +
                   (s[static_cast<size_t>(pos + 1)] - 0xDC00) + 0x10000;
        }
        return c;
    }
};

bool State::matchSeq(const Seq& nodes, size_t from, int pos, const Cont& cont) const {
    if (from >= nodes.size()) return cont(pos);
    const Node& n = *nodes[from];
    const size_t rest = from + 1;
    const int len = static_cast<int>(s.size());

    if (auto* v = dynamic_cast<const Lit*>(&n)) {
        if (f.u && v->cp > 0xFFFF) {
            if (pos + 1 < len) {
                uint16_t hi = s[static_cast<size_t>(pos)];
                uint16_t lo = s[static_cast<size_t>(pos + 1)];
                if (hi >= 0xD800 && hi <= 0xDBFF && lo >= 0xDC00 && lo <= 0xDFFF) {
                    uint32_t got = (static_cast<uint32_t>(hi - 0xD800) << 10) +
                                   (lo - 0xDC00) + 0x10000;
                    bool eq = f.i ? foldRep(got) == foldRep(v->cp) : got == v->cp;
                    if (eq) return matchSeq(nodes, rest, pos + 2, cont);
                }
            }
            return false;
        }
        if (pos < len) {
            uint16_t c = s[static_cast<size_t>(pos)];
            // u mode: a lone-surrogate atom never matches the high half of a
            // valid pair — the current code point is the whole pair.
            bool paired = f.u && c >= 0xD800 && c <= 0xDBFF && pos + 1 < len &&
                          s[static_cast<size_t>(pos + 1)] >= 0xDC00 &&
                          s[static_cast<size_t>(pos + 1)] <= 0xDFFF;
            if (!paired && eqChar(static_cast<uint16_t>(v->cp), c)) {
                return matchSeq(nodes, rest, pos + 1, cont);
            }
        }
        return false;
    }
    if (auto* v = dynamic_cast<const Cls*>(&n)) {
        if (pos >= len) return false;
        // u mode: when the position starts a valid pair the code point is
        // astral — only astral class members can match (BMP spans never do).
        if (f.u && pos + 1 < len && s[static_cast<size_t>(pos)] >= 0xD800 &&
            s[static_cast<size_t>(pos)] <= 0xDBFF &&
            s[static_cast<size_t>(pos + 1)] >= 0xDC00 &&
            s[static_cast<size_t>(pos + 1)] <= 0xDFFF) {
            uint32_t cp = (static_cast<uint32_t>(s[static_cast<size_t>(pos)] - 0xD800) << 10) +
                          (s[static_cast<size_t>(pos + 1)] - 0xDC00) + 0x10000;
            bool in = v->matchesAstral(f.i ? foldRep(cp) : cp);
            if (in != v->neg) return matchSeq(nodes, rest, pos + 2, cont);
            return false;
        }
        uint16_t c = s[static_cast<size_t>(pos)];
        // i mode: the class stores member images (canonNu non-unicode,
        // foldRep unicode) and the subject is mapped through the same
        // idempotent map before the membership test.
        uint16_t cc = c;
        if (f.i) cc = f.u ? static_cast<uint16_t>(foldRep(c)) : canonNu(c);
        bool in = v->matchesRaw(cc);
        if (in != v->neg) {
            return matchSeq(nodes, rest, pos + unitLen(pos), cont);
        }
        return false;
    }
    if (dynamic_cast<const Dot*>(&n)) {
        if (pos < len && (f.s || !isLineTerminator(s[static_cast<size_t>(pos)]))) {
            return matchSeq(nodes, rest, pos + unitLen(pos), cont);
        }
        return false;
    }
    if (dynamic_cast<const Caret*>(&n)) {
        if (pos == 0 || (f.m && pos > 0 && isLineTerminator(s[static_cast<size_t>(pos - 1)]))) {
            return matchSeq(nodes, rest, pos, cont);
        }
        return false;
    }
    if (dynamic_cast<const Dollar*>(&n)) {
        if (pos == len || (f.m && isLineTerminator(s[static_cast<size_t>(pos)]))) {
            return matchSeq(nodes, rest, pos, cont);
        }
        return false;
    }
    if (auto* v = dynamic_cast<const WordB*>(&n)) {
        bool before = pos > 0 && isJsWordChar(s[static_cast<size_t>(pos - 1)]);
        bool after = pos < len && isJsWordChar(s[static_cast<size_t>(pos)]);
        bool boundary = before != after;
        if (boundary != v->neg) {
            return matchSeq(nodes, rest, pos, cont);
        }
        return false;
    }
    if (auto* v = dynamic_cast<const Alt*>(&n)) {
        for (const Seq& alt : v->alts) {
            if (matchSeq(alt, 0, pos,
                         [&](int end) { return matchSeq(nodes, rest, end, cont); })) {
                return true;
            }
        }
        return false;
    }
    if (auto* v = dynamic_cast<const Group*>(&n)) {
        for (const Seq& alt : v->alts) {
            auto saved = v->idx > 0 ? caps[static_cast<size_t>(v->idx)] : std::nullopt;
            bool ok = matchSeq(alt, 0, pos, [&](int end) {
                if (v->idx > 0) caps[static_cast<size_t>(v->idx)] = std::make_pair(pos, end);
                if (matchSeq(nodes, rest, end, cont)) return true;
                if (v->idx > 0) caps[static_cast<size_t>(v->idx)] = saved;
                return false;
            });
            if (ok) return true;
            if (v->idx > 0) caps[static_cast<size_t>(v->idx)] = saved;
        }
        return false;
    }
    if (auto* v = dynamic_cast<const Look*>(&n)) {
        bool ok;
        if (v->behind) {
            ok = false;
            for (int start = pos; start >= 0 && !ok; start--) {
                if (f.u && start > 0 && start < len &&
                    s[static_cast<size_t>(start)] >= 0xDC00 &&
                    s[static_cast<size_t>(start)] <= 0xDFFF &&
                    s[static_cast<size_t>(start - 1)] >= 0xD800 &&
                    s[static_cast<size_t>(start - 1)] <= 0xDBFF) {
                    continue;  // mid-pair: not a code point boundary in u mode
                }
                for (const Seq& alt : v->alts) {
                    if (matchSeq(alt, 0, start, [&](int end) { return end == pos; })) {
                        ok = true;
                        break;
                    }
                }
            }
        } else {
            ok = false;
            for (const Seq& alt : v->alts) {
                if (matchSeq(alt, 0, pos, [](int) { return true; })) {
                    ok = true;
                    break;
                }
            }
        }
        if (ok != v->neg) {
            return matchSeq(nodes, rest, pos, cont);
        }
        return false;
    }
    if (auto* v = dynamic_cast<const Backref*>(&n)) {
        const auto& cap = caps[static_cast<size_t>(v->idx)];
        if (!cap) return matchSeq(nodes, rest, pos, cont);  // unset group: empty
        if (f.u) {
            // Code-point-wise: surrogate pairs are atomic units, and i folds
            // whole code points (probe: /(\u{10400})\1/iu matches 10400+10428).
            int p1 = cap->first, p2 = pos;
            while (p1 < cap->second) {
                if (p2 >= len) return false;
                uint32_t c1 = codePointAt(p1), c2 = codePointAt(p2);
                if (f.i ? foldRep(c1) != foldRep(c2) : c1 != c2) return false;
                p1 += unitLen(p1);
                p2 += unitLen(p2);
            }
            return matchSeq(nodes, rest, p2, cont);
        }
        int blen = cap->second - cap->first;
        if (pos + blen > len) return false;
        for (int k = 0; k < blen; k++) {
            if (!eqChar(s[static_cast<size_t>(cap->first + k)],
                        s[static_cast<size_t>(pos + k)])) {
                return false;
            }
        }
        return matchSeq(nodes, rest, pos + blen, cont);
    }
    if (auto* v = dynamic_cast<const Rep*>(&n)) {
        return matchRep(*v, pos, 0, [&](int p) { return matchSeq(nodes, rest, p, cont); });
    }
    throw std::runtime_error("jsre: unknown node");
}

bool State::matchRep(const Rep& v, int pos, int count, const Cont& cont) const {
    bool canMore = v.max < 0 || count < v.max;
    if (v.lazy) {
        if (count >= v.min && cont(pos)) return true;
        if (canMore) {
            return matchAtom(*v.node, pos, [&](int p) {
                if (p == pos && count >= v.min) return false;  // empty-loop guard
                return matchRep(v, p, count + 1, cont);
            });
        }
        return false;
    }
    if (canMore) {
        if (matchAtom(*v.node, pos, [&](int p) {
                if (p == pos && count >= v.min) return false;  // empty-loop guard
                return matchRep(v, p, count + 1, cont);
            })) {
            return true;
        }
    }
    return count >= v.min && cont(pos);
}

bool State::matchAtom(const Node& n, int pos, const Cont& cont) const {
    Seq one{const_cast<Node*>(&n)};
    return matchSeq(one, 0, pos, cont);
}

// ------------------------------------------------------------------ parser

// Lexical pre-scan: total capture groups and whether any named group exists.
// V8 resolves backreferences and \k against the whole pattern (forward
// references included), so both facts must be known before parsing starts.
struct PreScan {
    int totalGroups = 0;
    bool hasNamed = false;
};

PreScan preScan(const std::u16string& src) {
    PreScan out;
    size_t i = 0;
    const size_t n = src.size();
    while (i < n) {
        uint16_t c = src[i];
        if (c == '\\') {
            i += 2;
            continue;
        }
        if (c == '[') {
            i++;
            if (i < n && src[i] == '^') i++;
            if (i < n && src[i] == ']') i++;  // leading ']' is literal
            while (i < n && src[i] != ']') {
                if (src[i] == '\\') i++;
                i++;
            }
            i++;
            continue;
        }
        if (c == '(') {
            if (i + 1 < n && src[i + 1] == '?') {
                if (i + 2 < n && src[i + 2] == '<' && i + 3 < n && src[i + 3] != '=' &&
                    src[i + 3] != '!') {
                    out.totalGroups++;
                    out.hasNamed = true;
                }
            } else {
                out.totalGroups++;
            }
        }
        i++;
    }
    return out;
}

struct Parser {
    std::u16string src;
    size_t pos = 0;
    int ngroups = 0;
    std::map<std::string, int> named;
    std::vector<std::unique_ptr<Node>> owned;
    PreScan scan;
    Flags f;

    Parser(std::u16string p, Flags flags) : src(std::move(p)), f(flags) {
        scan = preScan(src);
    }

    template <typename T, typename... Args>
    T* make(Args&&... args) {
        owned.push_back(std::make_unique<T>(std::forward<Args>(args)...));
        return static_cast<T*>(owned.back().get());
    }

    bool eof() const { return pos >= src.size(); }
    uint16_t peek() const { return eof() ? uint16_t(0xFFFF) : src[pos]; }
    uint16_t peekAt(size_t n) const {
        return pos + n >= src.size() ? uint16_t(0xFFFF) : src[pos + n];
    }
    uint16_t next() { return src[pos++]; }

    [[noreturn]] void fail(const std::string& reason) const {
        throw std::runtime_error(reason);
    }

    std::vector<Seq> parseAlternation() {
        std::vector<Seq> alts;
        while (true) {
            alts.push_back(parseSeq());
            if (peek() == '|') {
                next();
                continue;
            }
            return alts;
        }
    }

    Seq parseSeq() {
        Seq seq;
        while (!eof()) {
            uint16_t c = peek();
            if (c == '|' || c == ')') return seq;
            Node* atom = parseAtom();
            atom = parseQuantifier(atom);
            seq.push_back(atom);
        }
        return seq;
    }

    Node* parseAtom() {
        uint16_t c = next();
        switch (c) {
            case '.':
                return make<Dot>();
            case '^':
                return make<Caret>();
            case '$':
                return make<Dollar>();
            case '(': {
                if (peek() == '?') {
                    next();
                    uint16_t k = peek();
                    if (k == ':') {
                        next();
                        auto* g = make<Group>();
                        g->alts = parseAlternation();
                        if (next() != ')') fail("Unterminated group");
                        return g;
                    }
                    if (k == '=' || k == '!') {
                        next();
                        auto* l = make<Look>();
                        l->neg = (k == '!');
                        l->alts = parseAlternation();
                        if (next() != ')') fail("Unterminated group");
                        return l;
                    }
                    if (peek() == '<') {
                        uint16_t k2 = peekAt(1);
                        if (k2 == '=') {
                            pos += 2;
                            auto* l = make<Look>();
                            l->behind = true;
                            l->alts = parseAlternation();
                            if (next() != ')') fail("Unterminated group");
                            return l;
                        }
                        if (k2 == '!') {
                            pos += 2;
                            auto* l = make<Look>();
                            l->behind = true;
                            l->neg = true;
                            l->alts = parseAlternation();
                            if (next() != ')') fail("Unterminated group");
                            return l;
                        }
                        // (?<name>...)
                        pos++;  // '<'
                        std::string name;
                        if (!parseGroupName(name)) fail("Invalid capture group name");
                        ngroups++;
                        int idx = ngroups;
                        auto* g = make<Group>();
                        g->idx = idx;
                        g->alts = parseAlternation();
                        if (next() != ')') fail("Unterminated group");
                        // V8 registers the name when the group closes, so body
                        // errors surface before the dup check and later syntax
                        // errors (quantifier, class, escape) after it.
                        if (named.count(name)) fail("Duplicate capture group name");
                        named[name] = idx;
                        return g;
                    }
                    fail("Invalid group");
                }
                ngroups++;
                int idx = ngroups;
                auto* g = make<Group>();
                g->idx = idx;
                g->alts = parseAlternation();
                if (next() != ')') fail("Unterminated group");
                return g;
            }
            case '[':
                return parseClass();
            case '\\':
                return parseEscapeAtom();
            case '{': {
                // A complete quantifier form with no atom to repeat errors;
                // anything else is a literal brace in non-unicode mode and
                // "Lone quantifier brackets" in unicode mode.
                if (quantBraceAhead()) fail("Nothing to repeat");
                if (f.u) fail("Lone quantifier brackets");
                return make<Lit>(uint32_t(c));
            }
            case '}':
            case ']':
                if (f.u) fail("Lone quantifier brackets");
                return make<Lit>(uint32_t(c));
            case uint16_t(0xFFFF):
                fail("Unexpected end of pattern");
            default:
                if (c == '*' || c == '+' || c == '?') fail("Nothing to repeat");
                return make<Lit>(uint32_t(c));
        }
        return nullptr;  // unreachable
    }

    bool parseGroupName(std::string& out) {
        // IdentifierName approximation: ASCII ID chars exactly, non-ASCII
        // units accepted loosely (letters/marks in practice).
        auto isStart = [](uint16_t c) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$')
                return true;
            return c >= 0x80;
        };
        auto isPart = [](uint16_t c) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_' || c == '$')
                return true;
            return c >= 0x80;
        };
        if (eof() || !isStart(peek())) return false;
        while (!eof()) {
            uint16_t c = peek();
            if (c == '>') break;
            if (!isPart(c)) return false;
            if (c < 0x80) {
                out += static_cast<char>(c);
            } else if (c < 0x800) {
                out += static_cast<char>(0xC0 | (c >> 6));
                out += static_cast<char>(0x80 | (c & 0x3F));
            } else {
                out += static_cast<char>(0xE0 | (c >> 12));
                out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (c & 0x3F));
            }
            next();
        }
        if (peek() != '>') return false;
        next();
        return !out.empty();
    }

    Cls* classOfSpans(const std::vector<Span>& spans, bool neg) {
        auto* cls = make<Cls>();
        cls->spans = spans;
        cls->neg = neg;
        return cls;
    }

    Node* parseEscapeAtom() {
        if (eof()) fail("\\ at end of pattern");
        uint16_t c = next();
        switch (c) {
            case 's': return classOfSpans(jsSpaceSpans(), false);
            case 'S': return classOfSpans(jsSpaceSpans(), true);
            case 'w': return classOfSpans(jsWordSpans(), false);
            case 'W': return classOfSpans(jsWordSpans(), true);
            case 'd': return classOfSpans(jsDigitSpans(), false);
            case 'D': return classOfSpans(jsDigitSpans(), true);
            case 'b': return make<WordB>();
            case 'B': {
                auto* w = make<WordB>();
                w->neg = true;
                return w;
            }
            case 'n': return make<Lit>('\n');
            case 'r': return make<Lit>('\r');
            case 't': return make<Lit>('\t');
            case 'f': return make<Lit>('\f');
            case 'v': return make<Lit>(0x0B);
            case 'u': {
                if (f.u) {
                    if (peek() == '{') {
                        next();
                        uint32_t v = 0;
                        bool any = false;
                        while (!eof() && peek() != '}') {
                            int d = hexVal(peek());
                            if (d < 0) fail("Invalid Unicode escape");
                            v = v * 16 + static_cast<uint32_t>(d);
                            if (v > 0x10FFFF) fail("Invalid Unicode escape");
                            next();
                            any = true;
                        }
                        if (!any || peek() != '}') fail("Invalid Unicode escape");
                        next();
                        return make<Lit>(v);
                    }
                    if (!hasHex(4)) fail("Invalid Unicode escape");
                    return make<Lit>(parseHex(4));
                }
                // Non-u: \u{...} and short \uNNNN degrade to a literal 'u'
                // (the following characters reparse normally).
                if (peek() == '{') return make<Lit>(uint16_t('u'));
                if (!hasHex(4)) return make<Lit>(uint16_t('u'));
                return make<Lit>(parseHex(4));
            }
            case 'x': {
                if (!f.u && !hasHex(2)) return make<Lit>(uint16_t('x'));
                if (f.u && !hasHex(2)) fail("Invalid escape");
                return make<Lit>(parseHex(2));
            }
            case 'c': {
                auto isLetter = [](uint16_t k) {
                    return (k >= 'a' && k <= 'z') || (k >= 'A' && k <= 'Z');
                };
                if (f.u) {
                    if (eof()) fail("Invalid Unicode escape");
                    uint16_t k = next();
                    if (!isLetter(k)) fail("Invalid Unicode escape");
                    return make<Lit>(static_cast<uint32_t>(k % 32));
                }
                // Annex B: \c + non-letter is a literal backslash ('c' and the
                // rest are reparsed as ordinary characters).
                if (eof() || !isLetter(peek())) {
                    pos--;  // reparse 'c' as an ordinary literal
                    return make<Lit>(uint32_t('\\'));
                }
                uint16_t k = next();
                return make<Lit>(static_cast<uint32_t>(k % 32));
            }
            case 'k': {
                // \k<name>: named backref when the pattern has named groups
                // (forward references allowed); bare 'k' otherwise.
                if (peek() == '<' && (f.u || scan.hasNamed)) {
                    next();
                    std::string name;
                    if (!parseGroupName(name)) fail("Invalid capture group name");
                    auto it = named.find(name);
                    if (it != named.end()) return make<Backref>(it->second);
                    auto* br = make<Backref>(0);
                    pendingNamed_.push_back({br, name});
                    return br;
                }
                if (f.u) fail("Invalid named reference");
                return make<Lit>(uint16_t('k'));
            }
            case 'p':
            case 'P': {
                if (!f.u) return make<Lit>(c);  // literal 'p'/'P'
                return parsePropertyEscape(c == 'P');
            }
            default:
                if (c >= '0' && c <= '9') {
                    return parseNumericEscape(c);
                }
                if (f.u) {
                    // Identity escapes restricted to SyntaxCharacter and '/'.
                    switch (c) {
                        case '^': case '$': case '\\': case '.': case '*':
                        case '+': case '?': case '(': case ')': case '[':
                        case ']': case '{': case '}': case '|': case '/':
                            return make<Lit>(c);
                        default:
                            fail("Invalid escape");
                    }
                }
                return make<Lit>(c);  // Annex B: any character (letters too)
        }
    }

    // Decimal escapes. V8 reads the full digit run, resolves it as a backref
    // against the whole pattern's group count (forward refs allowed), and
    // falls back to legacy octal (<= 3 digits, value <= 0xFF) otherwise; \8
    // and \9 degrade to literal digits.
    Node* parseNumericEscape(uint16_t first) {
        if (first == '0') {
            if (f.u) {
                if (peek() >= '0' && peek() <= '9') fail("Invalid decimal escape");
                return make<Lit>(0);
            }
            uint32_t v = 0;
            while (!eof() && peek() >= '0' && peek() <= '7' && v <= 0x1F) {
                v = v * 8 + static_cast<uint32_t>(next() - '0');
            }
            return make<Lit>(v);
        }
        if (first >= '1' && first <= '9') {
            size_t save = pos;
            long long n = first - '0';
            while (!eof() && peek() >= '0' && peek() <= '9') {
                n = n * 10 + (next() - '0');
                if (n > 1000000) break;
            }
            if (n >= 1 && n <= scan.totalGroups) {
                return make<Backref>(static_cast<int>(n));
            }
            if (f.u) fail("Invalid escape");
            // Legacy octal fallback: re-read as up to 3 octal digits <= 0xFF.
            pos = save;
            if (first > '7') return make<Lit>(first);  // \8, \9
            uint32_t v = first - '0';
            while (!eof() && peek() >= '0' && peek() <= '7' && v <= 0x1F) {
                v = v * 8 + static_cast<uint32_t>(next() - '0');
            }
            return make<Lit>(v);
        }
        return make<Lit>(first);
    }

    struct PendingNamedBackref {
        Backref* node;
        std::string name;
    };
    std::vector<PendingNamedBackref> pendingNamed_;

    uint32_t parseHex(int n) {
        if (pos + static_cast<size_t>(n) > src.size()) fail("Invalid escape");
        uint32_t v = 0;
        for (int i = 0; i < n; i++) {
            int d = hexVal(next());
            if (d < 0) fail("Invalid escape");
            v = v * 16 + static_cast<uint32_t>(d);
        }
        return v;
    }

    bool hasHex(int n) const {
        if (pos + static_cast<size_t>(n) > src.size()) return false;
        for (int i = 0; i < n; i++) {
            if (hexVal(src[pos + static_cast<size_t>(i)]) < 0) return false;
        }
        return true;
    }

    // \p{...} / \P{...} — General_Category shorthands plus a few binary
    // properties, backed by the tables in jsprops.cpp.
    Node* parsePropertyEscape(bool negate) {
        if (peek() != '{') fail("Invalid property name");
        next();
        std::string name;
        while (!eof() && peek() != '}') {
            uint16_t ch = next();
            if (ch == '=') fail("Invalid property name");  // key=value unsupported
            name += static_cast<char>(ch);
        }
        if (peek() != '}') fail("Invalid property name");
        next();
        auto* cls = make<Cls>();
        std::vector<PropRange> ranges;
        if (!propertyRangesFor(name, ranges)) fail("Invalid property name");
        for (const auto& r : ranges) {
            uint32_t lo = r.lo, hi = r.hi;
            if (hi <= 0xFFFF) {
                cls->spans.push_back(Span{static_cast<uint16_t>(lo), static_cast<uint16_t>(hi)});
            } else {
                if (lo <= 0xFFFF) {
                    cls->spans.push_back(Span{static_cast<uint16_t>(lo), 0xFFFF});
                    lo = 0x10000;
                }
                cls->astral.push_back({lo, hi});
            }
        }
        if (f.i) {
            canonicalizeSpans(cls->spans);
            if (f.u) canonicalizeAstral(cls->cps, cls->astral);
        }
        cls->neg = negate;
        return cls;
    }

    static int hexVal(uint16_t c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    // pos is just past '{': true when a complete quantifier form follows
    // ({digits}, {digits,}, {digits,digits}).
    bool quantBraceAhead() const {
        size_t i = pos;
        auto digit = [&](size_t k) {
            return k < src.size() && src[k] >= '0' && src[k] <= '9';
        };
        if (!digit(i)) return false;
        while (digit(i)) i++;
        if (i < src.size() && src[i] == ',') {
            i++;
            while (digit(i)) i++;
        }
        return i < src.size() && src[i] == '}';
    }

    Node* parseQuantifier(Node* atom) {
        if (eof()) return atom;
        // Bare assertions (^ $ \b \B) are never quantified by this engine:
        // whatever follows is handled at the next term's atom position
        // ("Nothing to repeat" for *+? and complete braces, "Lone quantifier
        // brackets" for malformed ones in u mode).
        if (dynamic_cast<const Caret*>(atom) || dynamic_cast<const Dollar*>(atom) ||
            dynamic_cast<const WordB*>(atom)) {
            return atom;
        }
        uint16_t c = peek();
        // Lookarounds quantify only in non-unicode forward form.
        auto lookForbidden = [&]() {
            auto* l = dynamic_cast<const Look*>(atom);
            return l && (f.u || l->behind);
        };
        switch (c) {
            case '*':
                if (lookForbidden()) fail("Invalid quantifier");
                next();
                return rep(atom, 0, -1);
            case '+':
                if (lookForbidden()) fail("Invalid quantifier");
                next();
                return rep(atom, 1, -1);
            case '?':
                if (lookForbidden()) fail("Invalid quantifier");
                next();
                return rep(atom, 0, 1);
            case '{':
                return parseBrace(atom);
            default:
                return atom;
        }
    }

    Rep* rep(Node* node, int min, int max) {
        auto* r = make<Rep>();
        r->node = node;
        r->min = min;
        r->max = max;
        if (peek() == '?') {
            next();
            r->lazy = true;
        }
        return r;
    }

    // {m}, {m,}, {m,n} with pos at the '{'. A malformed form is "Incomplete
    // quantifier" in u mode; otherwise pos rewinds and the '{' is left for the
    // next term (literal brace in non-unicode mode).
    Node* parseBrace(Node* atom) {
        size_t save = pos;
        next();  // '{'
        int lo = 0;
        if (!parseDecimal(lo)) {
            if (f.u) fail("Incomplete quantifier");
            pos = save;
            return atom;
        }
        int hi = lo;
        if (peek() == ',') {
            next();
            hi = -1;
            if (peek() != '}') {
                int v = 0;
                if (!parseDecimal(v)) {
                    if (f.u) fail("Incomplete quantifier");
                    pos = save;
                    return atom;
                }
                hi = v;
            }
        }
        if (peek() != '}') {
            if (f.u) fail("Incomplete quantifier");
            pos = save;
            return atom;
        }
        next();
        if (hi >= 0 && lo > hi) fail("numbers out of order in {} quantifier");
        if (auto* l = dynamic_cast<const Look*>(atom)) {
            if (f.u || l->behind) fail("Invalid quantifier");
        }
        return rep(atom, lo, hi);
    }

    bool parseDecimal(int& out) {
        size_t start = pos;
        long long v = 0;
        while (!eof() && peek() >= '0' && peek() <= '9') {
            v = v * 10 + (next() - '0');
            if (v > 999999) break;
        }
        out = static_cast<int>(v);
        return pos > start;
    }

    struct ClassItem {
        uint16_t ch = 0;
        bool shorthand = false;   // \s \w \d \S \W \D
        bool prop = false;         // \p{...} (propSpans/propAstral filled)
        uint32_t cp = 0;           // astral member when astralMember
        bool astralMember = false; // \u{...} > 0xFFFF inside a class (u mode)
        std::vector<Span> propSpans;
        std::vector<std::pair<uint32_t, uint32_t>> propAstral;
    };

    // '[' has been consumed.
    Node* parseClass() {
        bool neg = false;
        if (peek() == '^') {
            next();
            neg = true;
        }
        std::vector<Span> spans;
        std::vector<uint32_t> cps;                     // astral members
        std::vector<std::pair<uint32_t, uint32_t>> astral;  // astral ranges
        while (true) {
            if (eof()) fail("Unterminated character class");
            uint16_t c = next();
            if (c == ']') break;  // ']' first (or right after '^') closes an empty class
            ClassItem item = parseClassItem(c);
            auto rangeAhead = [&]() {
                return peek() == '-' && peekAt(1) != ']' &&
                       peekAt(1) != uint16_t(0xFFFF);
            };
            if (item.prop) {
                if (f.u && rangeAhead()) fail("Invalid character class");
                mergeSpans(spans, item.propSpans);
                astral.insert(astral.end(), item.propAstral.begin(), item.propAstral.end());
                continue;
            }
            if (item.shorthand) {
                if (f.u && rangeAhead()) fail("Invalid character class");
                mergeSpans(spans, shorthandSpansFor(item.ch));
                continue;
            }
            if (item.astralMember) {
                // Possible astral range: [\u{X}-\u{Y}] or [\u{X}-z].
                if (peek() == '-' && peekAt(1) != ']' && peekAt(1) != uint16_t(0xFFFF)) {
                    uint16_t dash = next();
                    (void)dash;
                    uint16_t hc = next();
                    ClassItem hi = parseClassItem(hc);
                    if (hi.shorthand || hi.prop) {
                        if (f.u) fail("Invalid character class");
                        cps.push_back(item.cp);
                        addChar(spans, '-');
                        if (hi.prop) {
                            mergeSpans(spans, hi.propSpans);
                            astral.insert(astral.end(), hi.propAstral.begin(), hi.propAstral.end());
                        } else {
                            mergeSpans(spans, shorthandSpansFor(hi.ch));
                        }
                        continue;
                    }
                    uint32_t hiCp = hi.astralMember ? hi.cp : hi.ch;
                    if (hiCp < item.cp) fail("Range out of order in character class");
                    if (item.cp <= 0xFFFF && hiCp <= 0xFFFF) {
                        addRange(spans, static_cast<uint16_t>(item.cp),
                                 static_cast<uint16_t>(hiCp));
                    } else {
                        uint32_t lo = item.cp, hi2 = hiCp;
                        if (lo <= 0xFFFF) {
                            spans.push_back(Span{static_cast<uint16_t>(lo), 0xFFFF});
                            lo = 0x10000;
                        }
                        astral.push_back({lo, hi2});
                    }
                    continue;
                }
                cps.push_back(item.cp);
                continue;
            }
            uint16_t lo = item.ch;
            // Range 'lo-hi' only when '-' is followed by another item.
            if (peek() == '-' && peekAt(1) != ']' && peekAt(1) != uint16_t(0xFFFF)) {
                next();  // '-'
                uint16_t hc = next();
                ClassItem hi = parseClassItem(hc);
                if (hi.shorthand) {
                    // Annex B: a shorthand on the range's right side makes the
                    // '-' literal (non-unicode mode).
                    if (f.u) fail("Invalid character class");
                    addChar(spans, lo);
                    addChar(spans, '-');
                    mergeSpans(spans, shorthandSpansFor(hi.ch));
                    continue;
                }
                if (hi.prop) {
                    if (f.u) fail("Invalid character class");
                    addChar(spans, lo);
                    addChar(spans, '-');
                    mergeSpans(spans, hi.propSpans);
                    astral.insert(astral.end(), hi.propAstral.begin(), hi.propAstral.end());
                    continue;
                }
                if (hi.astralMember) {
                    if (hi.cp < lo) fail("Range out of order in character class");
                    spans.push_back(Span{lo, 0xFFFF});
                    astral.push_back({0x10000, hi.cp});
                    continue;
                }
                if (hi.ch < lo) fail("Range out of order in character class");
                addRange(spans, lo, hi.ch);
                continue;
            }
            addChar(spans, lo);
        }
        std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) {
            return a.lo < b.lo;
        });
        // Union-merge so matchesRaw's binary search stays valid on overlaps
        // (e.g. [\S\d]: \d sits inside a \S span).
        {
            std::vector<Span> merged;
            merged.reserve(spans.size());
            for (const auto& s : spans) {
                if (!merged.empty() && s.lo <= merged.back().hi + 1) {
                    if (s.hi > merged.back().hi) merged.back().hi = s.hi;
                } else {
                    merged.push_back(s);
                }
            }
            spans = std::move(merged);
        }
        std::sort(astral.begin(), astral.end());
        if (f.i) {
            canonicalizeSpans(spans);
            if (f.u) canonicalizeAstral(cps, astral);
        }
        auto* cls = classOfSpans(spans, neg);
        cls->cps = std::move(cps);
        cls->astral = std::move(astral);
        return cls;
    }

    // One class item.
    ClassItem parseClassItem(uint16_t c) {
        ClassItem item;
        item.ch = c;
        if (c != '\\') return item;
        if (eof()) fail("\\ at end of pattern");
        uint16_t e = next();
        switch (e) {
            case 's': case 'w': case 'd':
            case 'S': case 'W': case 'D':
                item.shorthand = true;
                item.ch = e;
                return item;
            case 'n': item.ch = '\n'; return item;
            case 'r': item.ch = '\r'; return item;
            case 't': item.ch = '\t'; return item;
            case 'f': item.ch = '\f'; return item;
            case 'v': item.ch = 0x0B; return item;
            case 'b': item.ch = 0x08; return item;  // [\b] is backspace
            case '0': {
                if (f.u) {
                    if (peek() >= '0' && peek() <= '9') fail("Invalid class escape");
                    item.ch = 0;
                    return item;
                }
                uint32_t v = 0;
                while (!eof() && peek() >= '0' && peek() <= '7' && v <= 0x1F)
                    v = v * 8 + static_cast<uint32_t>(next() - '0');
                item.ch = static_cast<uint16_t>(v);
                return item;
            }
            case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': {
                if (f.u) fail("Invalid class escape");
                uint32_t v = e - '0';
                while (!eof() && peek() >= '0' && peek() <= '7' && v <= 0x1F)
                    v = v * 8 + static_cast<uint32_t>(next() - '0');
                item.ch = static_cast<uint16_t>(v);
                return item;
            }
            case '8': case '9':
                if (f.u) fail("Invalid escape");
                item.ch = e;  // literal digit
                return item;
            case 'B':
                if (f.u) fail("Invalid escape");
                item.ch = e;
                return item;
            case 'x': {
                if (f.u) {
                    if (!hasHex(2)) fail("Invalid escape");
                    item.ch = static_cast<uint16_t>(parseHex(2));
                    return item;
                }
                if (!hasHex(2)) {
                    item.ch = 'x';
                    return item;
                }
                item.ch = static_cast<uint16_t>(parseHex(2));
                return item;
            }
            case 'u': {
                if (f.u && peek() == '{') {
                    next();
                    uint32_t v = 0;
                    bool any = false;
                    while (!eof() && peek() != '}') {
                        int d = hexVal(peek());
                        if (d < 0) fail("Invalid Unicode escape");
                        v = v * 16 + static_cast<uint32_t>(d);
                        if (v > 0x10FFFF) fail("Invalid Unicode escape");
                        next();
                        any = true;
                    }
                    if (!any || peek() != '}') fail("Invalid Unicode escape");
                    next();
                    if (v > 0xFFFF) {
                        item.astralMember = true;
                        item.cp = v;
                    } else {
                        item.ch = static_cast<uint16_t>(v);
                    }
                    return item;
                }
                if (f.u) {
                    if (!hasHex(4)) fail("Invalid Unicode escape");
                    item.ch = static_cast<uint16_t>(parseHex(4));
                    return item;
                }
                if (peek() == '{' || !hasHex(4)) {
                    item.ch = 'u';
                    return item;
                }
                item.ch = static_cast<uint16_t>(parseHex(4));
                return item;
            }
            case 'c': {
                auto isLetter = [](uint16_t k) {
                    return (k >= 'a' && k <= 'z') || (k >= 'A' && k <= 'Z');
                };
                if (f.u) {
                    if (eof()) fail("Invalid Unicode escape");
                    uint16_t k = next();
                    if (!isLetter(k)) fail("Invalid Unicode escape");
                    item.ch = static_cast<uint16_t>(k % 32);
                    return item;
                }
                if (eof() || peek() == ']') {
                    item.ch = 'c';
                    return item;
                }
                uint16_t k = next();
                item.ch = static_cast<uint16_t>(k % 32);
                return item;
            }
            case 'p':
            case 'P': {
                if (!f.u) {
                    item.ch = e;
                    return item;
                }
                // \p{...} inside a class.
                if (peek() != '{') fail("Invalid property name in character class");
                next();
                std::string name;
                while (!eof() && peek() != '}') {
                    uint16_t ch = next();
                    if (ch == '=') fail("Invalid property name in character class");
                    name += static_cast<char>(ch);
                }
                if (peek() != '}') fail("Invalid property name in character class");
                next();
                std::vector<PropRange> ranges;
                if (!propertyRangesFor(name, ranges))
                    fail("Invalid property name in character class");
                item.prop = true;
                if (e == 'P') {
                    // Negated property inside a class: build the complement.
                    std::vector<Span> pos;
                    std::vector<std::pair<uint32_t, uint32_t>> posAstral;
                    for (const auto& r : ranges) {
                        uint32_t lo = r.lo, hi = r.hi;
                        if (hi <= 0xFFFF) {
                            pos.push_back(Span{static_cast<uint16_t>(lo), static_cast<uint16_t>(hi)});
                        } else {
                            if (lo <= 0xFFFF) {
                                pos.push_back(Span{static_cast<uint16_t>(lo), 0xFFFF});
                                lo = 0x10000;
                            }
                            posAstral.push_back({lo, hi});
                        }
                    }
                    complementSpans(pos, item.propSpans);
                    // astral complement
                    uint32_t next2 = 0x10000;
                    for (const auto& r : posAstral) {
                        if (r.first > next2) item.propAstral.push_back({next2, r.first - 1});
                        next2 = r.second + 1;
                    }
                    if (next2 <= 0x10FFFF) item.propAstral.push_back({next2, 0x10FFFF});
                } else {
                    for (const auto& r : ranges) {
                        uint32_t lo = r.lo, hi = r.hi;
                        if (hi <= 0xFFFF) {
                            item.propSpans.push_back(Span{static_cast<uint16_t>(lo), static_cast<uint16_t>(hi)});
                        } else {
                            if (lo <= 0xFFFF) {
                                item.propSpans.push_back(Span{static_cast<uint16_t>(lo), 0xFFFF});
                                lo = 0x10000;
                            }
                            item.propAstral.push_back({lo, hi});
                        }
                    }
                }
                return item;
            }
            case 'k':
                if (f.u) fail("Invalid escape");
                item.ch = e;
                return item;
            default:
                if (f.u) {
                    switch (e) {
                        case '^': case '$': case '\\': case '.': case '*':
                        case '+': case '?': case '(': case ')': case '[':
                        case ']': case '{': case '}': case '|': case '/':
                            item.ch = e;
                            return item;
                        default:
                            fail("Invalid escape");
                    }
                }
                item.ch = e;  // \], \-, \\ and identity escapes
                return item;
        }
    }

    static const std::vector<Span>& jsSpaceSpans() {
        static const std::vector<Span> spans = {
            {0x0009, 0x000D}, {0x0020, 0x0020}, {0x00A0, 0x00A0}, {0x1680, 0x1680},
            {0x2000, 0x200A}, {0x2028, 0x2029}, {0x202F, 0x202F}, {0x205F, 0x205F},
            {0x3000, 0x3000}, {0xFEFF, 0xFEFF},
        };
        return spans;
    }
    static const std::vector<Span>& jsWordSpans() {
        static const std::vector<Span> spans = {
            {'0', '9'}, {'A', 'Z'}, {'_', '_'}, {'a', 'z'},
        };
        return spans;
    }
    static const std::vector<Span>& jsDigitSpans() {
        static const std::vector<Span> spans = {{'0', '9'}};
        return spans;
    }

    static const std::vector<Span>& jsNonSpaceSpans() {
        static const std::vector<Span> spans = complementOf(jsSpaceSpans());
        return spans;
    }
    static const std::vector<Span>& jsNonWordSpans() {
        static const std::vector<Span> spans = complementOf(jsWordSpans());
        return spans;
    }
    static const std::vector<Span>& jsNonDigitSpans() {
        static const std::vector<Span> spans = complementOf(jsDigitSpans());
        return spans;
    }

    static const std::vector<Span>& shorthandSpansFor(uint16_t e) {
        switch (e) {
            case 's': return jsSpaceSpans();
            case 'S': return jsNonSpaceSpans();
            case 'w': return jsWordSpans();
            case 'W': return jsNonWordSpans();
            case 'd': return jsDigitSpans();
            default: return jsNonDigitSpans();  // D
        }
    }

    static std::vector<Span> complementOf(const std::vector<Span>& in) {
        std::vector<Span> out;
        complementSpans(in, out);
        return out;
    }

    void addChar(std::vector<Span>& spans, uint16_t c) {
        spans.push_back(Span{c, c});
    }
    void addRange(std::vector<Span>& spans, uint16_t lo, uint16_t hi) {
        spans.push_back(Span{lo, hi});
    }
    void mergeSpans(std::vector<Span>& spans, const std::vector<Span>& more) {
        spans.insert(spans.end(), more.begin(), more.end());
    }

    // Complement of a sorted, non-overlapping span set over [0, 0xFFFF].
    static void complementSpans(const std::vector<Span>& in, std::vector<Span>& out) {
        out.clear();
        uint32_t next = 0;
        for (const Span& s : in) {
            if (s.lo > next) out.push_back(Span{static_cast<uint16_t>(next),
                                                static_cast<uint16_t>(s.lo - 1)});
            if (static_cast<uint32_t>(s.hi) >= next) next = static_cast<uint32_t>(s.hi) + 1;
            if (next > 0xFFFF) return;
        }
        if (next <= 0xFFFF) out.push_back(Span{static_cast<uint16_t>(next), 0xFFFF});
    }

    // i-mode class images. The matcher canonicalizes the subject (canonNu in
    // non-unicode mode, foldRep in unicode mode); both maps are idempotent,
    // so queries only ever touch fixed points. Keeping the raw members and
    // adding the image of every table entry that falls inside a member span
    // is therefore exact: raw non-fixed points can never be queried.
    void canonicalizeSpans(std::vector<Span>& spans) {
        std::vector<Span> extra;
        auto covered = [&](uint16_t c) {
            size_t lo = 0, hi = spans.size();
            while (lo < hi) {
                size_t mid = (lo + hi) / 2;
                if (c < spans[mid].lo) hi = mid;
                else if (c > spans[mid].hi) lo = mid + 1;
                else return true;
            }
            return false;
        };
        if (f.u) {
            for (size_t i = 0; i < kFoldRepCount; i++) {
                uint32_t from = kFoldRep[i].from, to = kFoldRep[i].to;
                if (from > 0xFFFF) continue;  // astral entries: class parser
                if (covered(static_cast<uint16_t>(from)) && to <= 0xFFFF) {
                    extra.push_back(Span{static_cast<uint16_t>(to),
                                         static_cast<uint16_t>(to)});
                }
            }
        } else {
            for (size_t i = 0; i < kCanonNuCount; i++) {
                if (covered(kCanonNu[i].from)) {
                    extra.push_back(Span{kCanonNu[i].to, kCanonNu[i].to});
                }
            }
        }
        spans.insert(spans.end(), extra.begin(), extra.end());
        std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) {
            return a.lo < b.lo;
        });
        std::vector<Span> out;
        for (const auto& s : spans) {
            if (!out.empty() && s.lo <= out.back().hi + 1) {
                if (s.hi > out.back().hi) out.back().hi = s.hi;
            } else {
                out.push_back(s);
            }
        }
        spans = std::move(out);
    }

    // u i mode: same image for astral members and ranges.
    void canonicalizeAstral(std::vector<uint32_t>& cps,
                            std::vector<std::pair<uint32_t, uint32_t>>& astral) {
        for (auto& c : cps) c = foldRep(c);
        std::vector<std::pair<uint32_t, uint32_t>> extra;
        for (size_t i = 0; i < kFoldRepCount; i++) {
            uint32_t from = kFoldRep[i].from;
            if (from < 0x10000) continue;
            bool covered = false;
            size_t lo = 0, hi = astral.size();
            while (lo < hi) {
                size_t mid = (lo + hi) / 2;
                if (from < astral[mid].first) hi = mid;
                else if (from > astral[mid].second) lo = mid + 1;
                else {
                    covered = true;
                    break;
                }
            }
            if (covered) extra.push_back({kFoldRep[i].to, kFoldRep[i].to});
        }
        astral.insert(astral.end(), extra.begin(), extra.end());
        std::sort(astral.begin(), astral.end());
        std::vector<std::pair<uint32_t, uint32_t>> out;
        for (const auto& s : astral) {
            if (!out.empty() && s.first <= out.back().second + 1) {
                if (s.second > out.back().second) out.back().second = s.second;
            } else {
                out.push_back(s);
            }
        }
        astral = std::move(out);
    }
};

// ------------------------------------------------------------ substitution

// ECMA-262 GetSubstitution: $&, $`, $', $$, $n, $nn, $<name>.
std::u16string getSubstitution(const std::u16string& s, int index, int end,
                               const std::vector<std::optional<std::pair<int, int>>>& caps,
                               const std::map<std::string, int>& named,
                               const std::u16string& replacement) {
    std::u16string out;
    const size_t rn = replacement.size();
    for (size_t i = 0; i < rn; i++) {
        uint16_t ch = replacement[i];
        if (ch != '$' || i + 1 >= rn) {
            out += ch;
            continue;
        }
        uint16_t c2 = replacement[i + 1];
        if (c2 == '$') {
            out += '$';
            i++;
        } else if (c2 == '&') {
            out += s.substr(static_cast<size_t>(index),
                            static_cast<size_t>(end - index));
            i++;
        } else if (c2 == '`') {
            out += s.substr(0, static_cast<size_t>(index));
            i++;
        } else if (c2 == '\'') {
            out += s.substr(static_cast<size_t>(end));
            i++;
        } else if (c2 == '<') {
            // $<name> — literal unless the pattern has named groups.
            size_t close = replacement.find('>', i + 2);
            if (close == std::u16string::npos) {
                out += '$';
            } else if (named.empty()) {
                out += '$';  // rest of the token emits literally
            } else {
                std::string name = u16SliceToUtf8(replacement,
                                                  static_cast<int>(i + 2),
                                                  static_cast<int>(close));
                auto it = named.find(name);
                if (it != named.end()) {
                    const auto& cap = caps[static_cast<size_t>(it->second)];
                    if (cap) {
                        out += s.substr(static_cast<size_t>(cap->first),
                                        static_cast<size_t>(cap->second - cap->first));
                    }
                }
                i = close;
            }
        } else if (c2 >= '0' && c2 <= '9') {
            int d1 = c2 - '0';
            int num = d1;
            size_t next = i + 2;
            if (next < rn && replacement[next] >= '0' && replacement[next] <= '9') {
                int two = d1 * 10 + (replacement[next] - '0');
                if (two <= static_cast<int>(caps.size()) - 1) {
                    num = two;
                    next++;
                }
            }
            if (num >= 1 && num <= static_cast<int>(caps.size()) - 1) {
                const auto& cap = caps[static_cast<size_t>(num)];
                if (cap) {
                    out += s.substr(static_cast<size_t>(cap->first),
                                    static_cast<size_t>(cap->second - cap->first));
                }
                i = next - 1;
            } else {
                out += '$';  // digits fall through as literals
            }
        } else {
            out += '$';
        }
    }
    return out;
}

}  // namespace

// ------------------------------------------------------------------- Impl

struct JsRegexp::Impl {
    Flags f;
    std::vector<Seq> prog;  // alternatives
    int ngroups = 0;
    std::map<std::string, int> named;
    std::vector<std::unique_ptr<Node>> owned;  // keeps the AST alive
};

JsRegexp::JsRegexp() : impl_(new Impl) {}
JsRegexp::~JsRegexp() = default;

std::unique_ptr<JsRegexp> JsRegexp::compile(const std::string& pattern,
                                            const std::string& flags) {
    Flags f;
    std::string pat16 = pattern;
    std::string fl = flags;
    auto err = [](const std::string& p, const std::string& fl, const std::string& reason) {
        return "Invalid regular expression: /" + p + "/" + fl + ": " + reason;
    };
    for (char c : fl) {
        switch (c) {
            case 'g': if (f.g) throw std::runtime_error(err(pattern, flags, "Duplicate flags")); f.g = true; break;
            case 'i': if (f.i) throw std::runtime_error(err(pattern, flags, "Duplicate flags")); f.i = true; break;
            case 'm': if (f.m) throw std::runtime_error(err(pattern, flags, "Duplicate flags")); f.m = true; break;
            case 's': if (f.s) throw std::runtime_error(err(pattern, flags, "Duplicate flags")); f.s = true; break;
            case 'u': if (f.u) throw std::runtime_error(err(pattern, flags, "Duplicate flags")); f.u = true; break;
            case 'y': if (f.y) throw std::runtime_error(err(pattern, flags, "Duplicate flags")); f.y = true; break;
            default: throw std::runtime_error(err(pattern, flags, "Invalid flags"));
        }
    }
    (void)pat16;

    auto re = std::unique_ptr<JsRegexp>(new JsRegexp());
    Parser p(utf8ToU16(pattern), f);
    try {
        re->impl_->prog = p.parseAlternation();
        if (p.pos != p.src.size()) {
            if (p.peek() == ')') throw std::runtime_error("Unmatched ')'");
            throw std::runtime_error("Unexpected internal error");
        }
        for (auto& pending : p.pendingNamed_) {
            auto it = p.named.find(pending.name);
            if (it == p.named.end()) {
                throw std::runtime_error("Invalid named capture referenced");
            }
            pending.node->idx = it->second;
        }
    } catch (const std::runtime_error& e) {
        throw std::runtime_error(err(pattern, flags, e.what()));
    }
    re->impl_->f = f;
    re->impl_->ngroups = p.ngroups;
    re->impl_->named = p.named;
    re->impl_->owned = std::move(p.owned);
    return re;
}

namespace {

struct ExecMatch {
    int index = 0;
    int end = 0;
    std::vector<std::optional<std::pair<int, int>>> caps;  // 0 = whole match
};

// AdvanceStringIndex: one code unit, or a whole surrogate pair in u mode.
int unitLenAt(const std::u16string& s, int pos, bool u) {
    if (u && pos + 1 < static_cast<int>(s.size()) &&
        s[static_cast<size_t>(pos)] >= 0xD800 && s[static_cast<size_t>(pos)] <= 0xDBFF &&
        s[static_cast<size_t>(pos + 1)] >= 0xDC00 &&
        s[static_cast<size_t>(pos + 1)] <= 0xDFFF) {
        return 2;
    }
    return 1;
}

bool execFrom(const Flags& f, const std::vector<Seq>& prog, int ngroups,
              const std::u16string& s, int searchFrom, bool sticky, ExecMatch& out) {
    std::vector<std::optional<std::pair<int, int>>> caps(
        static_cast<size_t>(ngroups) + 1);
    State st(s, caps, f);
    int len = static_cast<int>(s.size());
    int from = searchFrom;
    while (from <= len) {
        caps.assign(caps.size(), std::nullopt);
        for (const Seq& alt : prog) {
            int end = -1;
            if (st.matchSeq(alt, 0, from, [&](int e) {
                    end = e;
                    return true;
                })) {
                out.index = from;
                out.end = end;
                out.caps = caps;
                out.caps[0] = std::make_pair(from, end);
                return true;
            }
        }
        if (sticky) return false;
        from += st.unitLen(from);
    }
    return false;
}

}  // namespace

bool JsRegexp::test(const std::string& subject) const {
    std::u16string s = utf8ToU16(subject);
    ExecMatch m;
    return execFrom(impl_->f, impl_->prog, impl_->ngroups, s, 0, impl_->f.y, m);
}

std::string JsRegexp::replace(const std::string& subject,
                              const std::string& replacement) const {
    std::u16string s = utf8ToU16(subject);
    std::u16string repl = utf8ToU16(replacement);
    std::u16string out;
    int sourcePos = 0;  // next uncopied unit
    int searchPos = 0;  // exec start
    const int len = static_cast<int>(s.size());
    bool global = impl_->f.g;

    while (searchPos <= len) {
        ExecMatch m;
        // Sticky (y) means the match must start exactly at searchPos — with g
        // the loop stops at the first miss (probe45), without g it just fails.
        if (!execFrom(impl_->f, impl_->prog, impl_->ngroups, s, searchPos, impl_->f.y,
                      m)) break;
        out += s.substr(static_cast<size_t>(sourcePos),
                        static_cast<size_t>(m.index - sourcePos));
        out += getSubstitution(s, m.index, m.end, m.caps, impl_->named, repl);
        sourcePos = m.end;
        int mlen = m.end - m.index;
        // Empty match: advance one code point (pair-aware) per the spec.
        searchPos = mlen == 0 ? m.index + unitLenAt(s, m.index, impl_->f.u) : m.end;
        if (!global) break;
    }
    out += s.substr(static_cast<size_t>(sourcePos));
    return u16SliceToUtf8(out, 0, static_cast<int>(out.size()));
}

// -------------------------------------------------------- util.mjs helpers

RegexSpec parseRegexSpec(const std::string& spec) {
    // /^\/(.+)\/([gimsuy]*)$/ — greedy body, flags restricted to [gimsuy],
    // no line terminators inside the body ('.' never matches them).
    std::u16string u = utf8ToU16(spec);
    if (!u.empty() && u[0] == '/') {
        for (size_t i = u.size() - 1; i >= 2; i--) {
            if (u[i] != '/') continue;
            bool flagsOk = true;
            for (size_t k = i + 1; k < u.size(); k++) {
                uint16_t c = u[k];
                if (c != 'g' && c != 'i' && c != 'm' && c != 's' && c != 'u' && c != 'y') {
                    flagsOk = false;
                    break;
                }
            }
            if (!flagsOk) continue;
            bool bodyOk = true;
            for (size_t k = 1; k < i; k++) {
                if (isLineTerminator(u[k])) {
                    bodyOk = false;
                    break;
                }
            }
            if (!bodyOk) continue;
            RegexSpec out;
            std::u16string body = u.substr(1, i - 1);
            out.pattern = u16SliceToUtf8(body, 0, static_cast<int>(body.size()));
            std::u16string flags = u.substr(i + 1);
            bool hasG = flags.find('g') != std::u16string::npos;
            std::string fl = u16SliceToUtf8(flags, 0, static_cast<int>(flags.size()));
            if (!hasG) fl += 'g';
            out.flags = fl;
            return out;
        }
    }
    return RegexSpec{spec, "g"};
}

std::string applySubstitutions(const std::string& text,
                               const std::vector<Substitution>& substitutions) {
    std::string out = text;
    for (const auto& sub : substitutions) {
        auto re = JsRegexp::compile(sub.pattern, sub.flags);
        out = re->replace(out, sub.replacement);
    }
    return out;
}

}  // namespace pocev
