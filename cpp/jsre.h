// jsre.h — a backtracking regex engine reproducing V8 (ECMAScript) RegExp
// semantics over UTF-16 code units, including String.prototype.replace's
// $-template substitution rules.
#ifndef pocev_JSRE_H
#define pocev_JSRE_H

#include <memory>
#include <string>
#include <vector>

namespace pocev {

class JsRegexp {
public:
    // V8-style "Invalid regular expression: /pat/flags: reason" on error.
    static std::unique_ptr<JsRegexp> compile(const std::string& pattern,
                                             const std::string& flags);

    // String.prototype.replace(regex, replacement) — global when the pattern
    // carries the g flag, sticky-aware, empty-match rules per spec.
    std::string replace(const std::string& subject, const std::string& replacement) const;

    // RegExp.prototype.test.
    bool test(const std::string& subject) const;

    ~JsRegexp();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    JsRegexp();
};

// util.mjs parseRegexSpec: '/pattern/flags' form when it parses, plain pattern
// otherwise; 'g' is always present in the returned flags.
struct RegexSpec {
    std::string pattern;
    std::string flags;
};
RegexSpec parseRegexSpec(const std::string& spec);

// util.mjs applySubstitutions: sequential text.replace(new RegExp(...), ...).
struct Substitution {
    std::string pattern;
    std::string flags;
    std::string replacement;
};
std::string applySubstitutions(const std::string& text,
                               const std::vector<Substitution>& substitutions);

}  // namespace pocev

#endif  // pocev_JSRE_H
