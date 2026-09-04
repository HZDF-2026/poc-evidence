// test_golden.cpp — golden-table tests against tests/golden/golden.json, which
// is generated from the Node reference implementation by gen_golden.mjs.
// Every vector must reproduce bit-for-bit.
#include "chain.h"
#include "jsjson.h"
#include "jsre.h"
#include "sha256.h"
#include "util.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace {

using pocev::Json;

int failures = 0;
int checks = 0;

void report(const std::string& group, const std::string& name, const std::string& expected,
            const std::string& actual) {
    failures++;
    std::fprintf(stderr, "FAIL [%s] %s\n  expected: %s\n  actual:   %s\n", group.c_str(),
                 name.c_str(), expected.c_str(), actual.c_str());
}

std::string jdump(const Json& v) {
    return v.dump();
}

void expectEq(const std::string& group, const std::string& name, const std::string& expected,
              const std::string& actual) {
    checks++;
    if (expected != actual) report(group, name, expected, actual);
}

const Json* field(const Json& v, const char* key) {
    return v.isObj() ? v.get(key) : nullptr;
}

// ------------------------------------------------------------------------- io

std::string loadGolden(int argc, char** argv) {
    std::vector<std::string> candidates;
    if (argc > 1) candidates.push_back(argv[1]);
    candidates.push_back("../tests/golden/golden.json");
    candidates.push_back("tests/golden/golden.json");
    for (const auto& c : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(std::filesystem::path(c), ec)) {
            try {
                return pocev::readFileBytes(c);
            } catch (const std::runtime_error&) {
            }
        }
    }
    std::fprintf(stderr, "golden.json not found (run from cpp/ or pass its path)\n");
    std::exit(2);
}

// ---------------------------------------------------------------------- groups

void testSha256(const Json& golden) {
    const Json* vec = field(golden, "sha256");
    if (!vec || !vec->isArr()) return;
    for (const auto& v : vec->arr) {
        const Json* in = field(v, "input");
        const Json* hex = field(v, "hex");
        if (!in || !hex || !in->isStr() || !hex->isStr()) continue;
        expectEq("sha256", in->str, hex->str, pocev::sha256Hex(in->str));
    }
}

void testStableStringify(const Json& golden) {
    const Json* vec = field(golden, "stableStringify");
    if (!vec || !vec->isArr()) return;
    int i = 0;
    for (const auto& v : vec->arr) {
        const Json* value = field(v, "value");
        const Json* out = field(v, "out");
        if (!value || !out || !out->isStr()) continue;
        expectEq("stableStringify", "vector " + std::to_string(i++), out->str,
                 pocev::stableStringify(*value));
    }
}

void testJsonParse(const Json& golden) {
    const Json* vec = field(golden, "jsonParse");
    if (!vec || !vec->isArr()) return;
    int i = 0;
    for (const auto& v : vec->arr) {
        const Json* text = field(v, "text");
        const Json* ok = field(v, "ok");
        const Json* out = field(v, "out");
        if (!text || !text->isStr() || !ok || !ok->isBool()) continue;
        std::string name = "vector " + std::to_string(i++) + " " + text->str.substr(0, 24);
        Json parsed;
        bool accepted = Json::parse(text->str, parsed);
        checks++;
        if (accepted != ok->b) {
            report("jsonParse", name, ok->b ? "accepted" : "rejected",
                   accepted ? "accepted" : "rejected");
            continue;
        }
        if (ok->b) {
            std::string expected = out && out->isStr() ? out->str : std::string();
            expectEq("jsonParse", name, expected, parsed.dump());
        }
    }
}

void testParseRegexSpec(const Json& golden) {
    const Json* vec = field(golden, "parseRegexSpec");
    if (!vec || !vec->isArr()) return;
    for (const auto& v : vec->arr) {
        const Json* spec = field(v, "spec");
        const Json* pattern = field(v, "pattern");
        const Json* flags = field(v, "flags");
        if (!spec || !spec->isStr() || !pattern || !pattern->isStr() || !flags || !flags->isStr()) {
            continue;
        }
        pocev::RegexSpec rs = pocev::parseRegexSpec(spec->str);
        checks++;
        if (rs.pattern != pattern->str || rs.flags != flags->str) {
            report("parseRegexSpec", spec->str, pattern->str + " /" + flags->str,
                   rs.pattern + " /" + rs.flags);
        }
    }
}

void testReplace(const Json& golden) {
    const Json* vec = field(golden, "replace");
    if (!vec || !vec->isArr()) return;
    for (const auto& v : vec->arr) {
        const Json* subject = field(v, "subject");
        const Json* pattern = field(v, "pattern");
        const Json* flags = field(v, "flags");
        const Json* replacement = field(v, "replacement");
        if (!subject || !subject->isStr() || !pattern || !pattern->isStr() || !flags ||
            !flags->isStr() || !replacement || !replacement->isStr()) {
            continue;
        }
        std::string name = "/" + pattern->str + "/" + flags->str + " -> " +
                           jdump(*replacement).substr(0, 24);
        const Json* error = field(v, "error");
        if (error && error->isStr()) continue;  // compile errors are E2E-tested
        const Json* out = field(v, "out");
        if (!out || !out->isStr()) continue;
        try {
            auto re = pocev::JsRegexp::compile(pattern->str, flags->str);
            std::string got = re->replace(subject->str, replacement->str);
            expectEq("replace", name, out->str, got);
        } catch (const std::runtime_error& e) {
            report("replace", name, out->str, std::string("threw: ") + e.what());
        }
    }
}

void testRegexError(const Json& golden) {
    const Json* vec = field(golden, "regexError");
    if (!vec || !vec->isArr()) return;
    for (const auto& v : vec->arr) {
        const Json* pattern = field(v, "pattern");
        const Json* flags = field(v, "flags");
        const Json* message = field(v, "message");
        if (!pattern || !pattern->isStr() || !flags || !flags->isStr() || !message ||
            !message->isStr()) {
            continue;
        }
        std::string name = "/" + pattern->str + "/" + flags->str;
        try {
            auto re = pocev::JsRegexp::compile(pattern->str, flags->str);
            (void)re;
            report("regexError", name, message->str, "<compiled>");
        } catch (const std::runtime_error& e) {
            expectEq("regexError", name, message->str, e.what());
        }
    }
}

void testChain(const Json& golden) {
    const Json* vec = field(golden, "chain");
    if (!vec || !vec->isArr()) return;
    namespace fs = std::filesystem;
    fs::path base = fs::temp_directory_path();
    std::random_device rd;
    int i = 0;
    for (const auto& v : vec->arr) {
        const Json* text = field(v, "text");
        const Json* expect = field(v, "expect");
        if (!text || !text->isStr() || !expect || !expect->isObj()) continue;
        std::string name = "vector " + std::to_string(i++);
        fs::path dir = base / ("pocev-gold-" + std::to_string(rd()));
        std::error_code ec;
        fs::path store = dir / ".poc-evidence";
        fs::create_directories(store, ec);
        pocev::writeFileBytes((store / "chain.jsonl").string(), text->str);
        try {
            Json verdict = pocev::verifyChain(dir.string());
            for (const auto& kv : expect->obj) {
                checks++;
                const Json* got = verdict.get(kv.first);
                std::string expected = kv.second.dump();
                std::string actual = got ? got->dump() : std::string("<missing>");
                if (expected != actual) {
                    report("chain", name + " field " + kv.first, expected, actual);
                }
            }
        } catch (const std::runtime_error& e) {
            failures++;
            std::fprintf(stderr, "FAIL [chain] %s threw: %s\n", name.c_str(), e.what());
        }
        fs::remove_all(dir, ec);
    }
}

}  // namespace

int main(int argc, char** argv) {
    Json golden;
    if (!Json::parse(loadGolden(argc, argv), golden)) {
        std::fprintf(stderr, "golden.json failed to parse\n");
        return 2;
    }
    testSha256(golden);
    testStableStringify(golden);
    testJsonParse(golden);
    testParseRegexSpec(golden);
    testReplace(golden);
    testRegexError(golden);
    testChain(golden);
    std::printf("golden: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
