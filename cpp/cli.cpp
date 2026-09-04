// cli.cpp — mirrors bin/poc-evidence.mjs: frozen usage, hand-rolled option
// parsing and the per-command console output.
#include "anchor.h"
#include "bundle.h"
#include "capture.h"
#include "chain.h"
#include "jsjson.h"
#include "jsre.h"
#include "replay.h"
#include "util.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace pocev {
namespace {

void out(const std::string& s) {
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fputc('\n', stdout);
}

void errOut(const std::string& s) {
    std::fwrite(s.data(), 1, s.size(), stderr);
    std::fputc('\n', stderr);
}

[[noreturn]] void fail(const std::string& message) {
    errOut("error: " + message);
    std::exit(2);
}

std::string usage() {
    return "poc-evidence v" + std::string(VERSION) +
           " — evidence-grade PoC capture, replay & timestamping\n"
           "\n"
           "Usage:\n"
           "  poc-evidence capture [options] -- <command> [args...]\n"
           "      Run a command and record verifiable evidence of what it did.\n"
           "        --input <path>       file or directory hashed before the run (repeatable)\n"
           "        --output <path>      file or directory hashed after the run (repeatable)\n"
           "        --redact <regex>     strip matches from captured output (repeatable,\n"
           "                             default replacement: [REDACTED])\n"
           "        --normalize <spec>   '<regex>=><replacement>' applied before hashing\n"
           "                             (repeatable; regex may be written /pattern/flags)\n"
           "        --label <text>       free-form label for the finding\n"
           "        --env-values         store env VALUES instead of hashes (dangerous)\n"
           "\n"
           "  poc-evidence replay <captureId>\n"
           "      Re-run a captured command and check every digest (exit 0 = MATCH).\n"
           "\n"
           "  poc-evidence verify\n"
           "      Recompute every digest in the evidence chain (exit 0 = intact).\n"
           "\n"
           "  poc-evidence bundle <captureId> [--out <dir>]\n"
           "      Export a self-contained evidence bundle for submission.\n"
           "\n"
           "  poc-evidence anchor [--message <text>]\n"
           "      Append an anchor record and commit the chain head to git.\n"
           "      Push to a remote to obtain a third-party timestamp.\n"
           "\n"
           "Examples:\n"
           "  poc-evidence capture --label \"XSS in /search\" --input fixtures/ -- node exploit.js\n"
           "  poc-evidence capture --normalize 'time=\\d+=>time=X' -- node report.js\n"
           "  poc-evidence replay cap_001 && poc-evidence bundle cap_001 && poc-evidence anchor\n"
           "\n"
           "Notes:\n"
           "  - Captured stdout/stderr are stored redacted; env values are hashed by default.\n"
           "  - chain.jsonl contains command lines and file paths — check repository\n"
           "    visibility before pushing anchors.\n";
}

// ------------------------------------------------------------ JS-ish helpers

size_t utf16Units(const std::string& s) {
    size_t units = 0, i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            i += 1;
            units += 1;
        } else if ((c & 0xE0) == 0xC0) {
            i += 2;
            units += 1;
        } else if ((c & 0xF0) == 0xE0) {
            i += 3;
            units += 1;
        } else if ((c & 0xF8) == 0xF0) {
            i += 4;
            units += 2;
        } else {
            i += 1;
            units += 1;
        }
    }
    return units;
}

std::string jsPadEnd(const std::string& s, size_t width) {
    size_t units = utf16Units(s);
    if (units >= width) return s;
    return s + std::string(width - units, ' ');
}

std::string jsSlice16(const std::string& s, size_t n) {
    size_t units = 0, i = 0;
    while (i < s.size() && units < n) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            i += 1;
            units += 1;
        } else if ((c & 0xE0) == 0xC0) {
            if (i + 1 > s.size()) break;
            i += 2;
            units += 1;
        } else if ((c & 0xF0) == 0xE0) {
            if (i + 2 > s.size()) break;
            i += 3;
            units += 1;
        } else if ((c & 0xF8) == 0xF0) {
            if (i + 3 > s.size()) break;
            i += 4;
            units += 2;
        } else {
            i += 1;
            units += 1;
        }
    }
    return utf8Sanitize(s.substr(0, i));
}

std::string jsToString(const Json* v) {
    if (!v) return "undefined";
    switch (v->t) {
        case Json::T::Str: return v->str;
        case Json::T::Num: return jsNumberToString(v->num);
        case Json::T::Bool: return v->b ? "true" : "false";
        case Json::T::Null: return "null";
        case Json::T::Arr: {
            // Array.prototype.toString(): comma-joined element strings.
            std::string joined;
            for (size_t i = 0; i < v->arr.size(); i++) {
                if (i) joined += ",";
                const Json& e = v->arr[i];
                if (e.isNull()) continue;
                joined += jsToString(&e);
            }
            return joined;
        }
        default: return "[object Object]";
    }
}

// ------------------------------------------------------------ option parsing

struct OptionSpec {
    std::vector<std::string> multi;
    std::vector<std::string> single;
    std::vector<std::string> bools;
};

struct Flags {
    std::map<std::string, std::vector<std::string>> multi;
    std::map<std::string, std::string> single;
    std::map<std::string, bool> bools;
};

bool contains(const std::vector<std::string>& v, const std::string& key) {
    return std::find(v.begin(), v.end(), key) != v.end();
}

struct ParsedOption {
    std::string key;
    std::string value;  // resolved: "1"/"" for bool flags, text otherwise
    bool isBool = false;
    int nextIndex = 0;
};

ParsedOption parseOptionToken(const std::vector<std::string>& argv, int i,
                              const OptionSpec& spec) {
    const std::string& token = argv[static_cast<size_t>(i)];
    std::string key, value;
    bool hasValue = false;
    size_t eq = token.find('=');
    if (eq != std::string::npos) {
        key = token.substr(2, eq - 2);
        value = token.substr(eq + 1);
        hasValue = true;
    } else {
        key = token.substr(2);
    }
    ParsedOption opt;
    if (contains(spec.bools, key)) {
        opt.key = key;
        opt.isBool = true;
        opt.value = hasValue ? (value != "false" ? "true" : "false") : "true";
        opt.nextIndex = i + 1;
        return opt;
    }
    if (!hasValue) {
        if (static_cast<size_t>(i + 1) >= argv.size()) {
            fail("--" + key + " requires a value");
        }
        value = argv[static_cast<size_t>(i + 1)];
        opt.nextIndex = i + 2;
    } else {
        opt.nextIndex = i + 1;
    }
    opt.key = key;
    opt.value = value;
    return opt;
}

void applyOption(Flags& flags, const OptionSpec& spec, const ParsedOption& opt) {
    if (contains(spec.multi, opt.key)) {
        flags.multi[opt.key].push_back(opt.value);
    } else if (contains(spec.single, opt.key)) {
        flags.single[opt.key] = opt.value;
    } else if (contains(spec.bools, opt.key)) {
        flags.bools[opt.key] = opt.value == "true";
    } else {
        fail("unknown option --" + opt.key + " (see 'poc-evidence help')");
    }
}

std::vector<std::string> sliceFrom(const std::vector<std::string>& argv, size_t i) {
    if (i >= argv.size()) return std::vector<std::string>();
    return std::vector<std::string>(argv.begin() + static_cast<long>(i), argv.end());
}

// capture: options before '--' or before the first positional token; the
// remainder (everything after that point, verbatim) is the command.
void parseCaptureArgs(const std::vector<std::string>& argv, Flags& flags,
                      std::vector<std::string>& command) {
    OptionSpec spec;
    spec.multi = {"input", "output", "redact", "normalize"};
    spec.single = {"label"};
    spec.bools = {"env-values"};
    command.clear();
    for (size_t i = 0; i < argv.size(); i++) {
        const std::string& token = argv[i];
        if (command.empty() && token == "--") {
            command = sliceFrom(argv, i + 1);
            break;
        }
        if (command.empty() && token.rfind("--", 0) == 0 && token.size() > 2) {
            ParsedOption opt = parseOptionToken(argv, static_cast<int>(i), spec);
            applyOption(flags, spec, opt);
            i = static_cast<size_t>(opt.nextIndex - 1);
        } else {
            command = sliceFrom(argv, i);
            break;
        }
    }
}

void parseSimple(const std::vector<std::string>& argv, const OptionSpec& spec, Flags& flags,
                 std::vector<std::string>& positional) {
    for (size_t i = 0; i < argv.size(); i++) {
        const std::string& token = argv[i];
        if (token.rfind("--", 0) == 0 && token.size() > 2) {
            ParsedOption opt = parseOptionToken(argv, static_cast<int>(i), spec);
            applyOption(flags, spec, opt);
            i = static_cast<size_t>(opt.nextIndex - 1);
        } else {
            positional.push_back(token);
        }
    }
}

std::vector<Substitution> buildRedactions(const std::vector<std::string>& specs) {
    std::vector<Substitution> outSubs;
    for (const auto& spec : specs) {
        RegexSpec rs = parseRegexSpec(spec);
        try {
            JsRegexp::compile(rs.pattern, rs.flags);
        } catch (const std::runtime_error& e) {
            fail("invalid --redact regex \"" + spec + "\": " + e.what());
        }
        Substitution s;
        s.pattern = rs.pattern;
        s.flags = rs.flags;
        s.replacement = "[REDACTED]";
        outSubs.push_back(s);
    }
    return outSubs;
}

std::vector<Substitution> buildNormalizers(const std::vector<std::string>& specs) {
    std::vector<Substitution> outSubs;
    for (const auto& spec : specs) {
        size_t sep = spec.find("=>");
        if (sep == std::string::npos) {
            fail("--normalize expects '<regex>=><replacement>' (got \"" + spec + "\")");
        }
        std::string patternPart = spec.substr(0, sep);
        std::string replacement = spec.substr(sep + 2);
        RegexSpec rs = parseRegexSpec(patternPart);
        try {
            JsRegexp::compile(rs.pattern, rs.flags);
        } catch (const std::runtime_error& e) {
            fail("invalid --normalize regex \"" + spec + "\": " + e.what());
        }
        Substitution s;
        s.pattern = rs.pattern;
        s.flags = rs.flags;
        s.replacement = replacement;
        outSubs.push_back(s);
    }
    return outSubs;
}

std::vector<std::string> multiOr(const Flags& flags, const char* key) {
    auto it = flags.multi.find(key);
    return it == flags.multi.end() ? std::vector<std::string>() : it->second;
}

std::string stringOf(const Json* v) {
    return v && v->isStr() ? v->str : "";
}

}  // namespace

int runCli(const std::vector<std::string>& args) {
    std::string cwd = std::filesystem::current_path().string();
    std::vector<std::string> rest = sliceFrom(args, 1);
    std::string cmd = args.empty() ? std::string() : args[0];

    if (cmd == "capture") {
        Flags flags;
        std::vector<std::string> command;
        parseCaptureArgs(rest, flags, command);
        if (command.empty()) {
            fail("no command given — usage: poc-evidence capture [options] -- <command>");
        }
        CaptureOpts opts;
        opts.cwd = cwd;
        opts.command = command;
        opts.inputs = multiOr(flags, "input");
        opts.outputs = multiOr(flags, "output");
        opts.redactions = buildRedactions(multiOr(flags, "redact"));
        opts.normalizers = buildNormalizers(multiOr(flags, "normalize"));
        auto labelIt = flags.single.find("label");
        if (labelIt != flags.single.end()) {
            opts.label = labelIt->second;
            opts.hasLabel = true;
        }
        auto envValues = flags.bools.find("env-values");
        opts.storeEnvValues = envValues != flags.bools.end() && envValues->second;
        Json record;
        try {
            record = runCapture(opts);
        } catch (const std::runtime_error& e) {
            fail(e.what());
        }
        const Json* p = record.get("payload");
        std::string label;
        if (p) {
            const Json* l = p->get("label");
            if (l && l->isStr() && !l->str.empty()) label = l->str;
        }
        out("captured " + stringOf(record.get("id")) +
            (label.empty() ? std::string() : " (" + label + ")"));
        std::string commandJoined;
        if (p) {
            const Json* c = p->get("command");
            if (c && c->isArr()) {
                for (size_t i = 0; i < c->arr.size(); i++) {
                    if (i) commandJoined += " ";
                    commandJoined += jsToString(&c->arr[i]);
                }
            }
        }
        out("  command : " + commandJoined);
        out("  exit    : " + jsToString(p ? p->get("exitCode") : nullptr) + " (" +
            jsToString(p ? p->get("durationMs") : nullptr) + " ms)");
        out("  stdout  : sha256:" + stringOf(p ? p->get("stdoutHash") : nullptr));
        out("  stderr  : sha256:" + stringOf(p ? p->get("stderrHash") : nullptr));
        const Json* inFiles = p ? p->get("inputFiles") : nullptr;
        const Json* outFiles = p ? p->get("outputFiles") : nullptr;
        out("  files   : " +
            jsNumberToString(static_cast<double>(inFiles && inFiles->isArr() ? inFiles->arr.size()
                                                                            : 0)) +
            " in / " +
            jsNumberToString(static_cast<double>(outFiles && outFiles->isArr()
                                                    ? outFiles->arr.size()
                                                    : 0)) +
            " out");
        out("  record  : " + stringOf(record.get("hash")));
        const Json* exitCode = p ? p->get("exitCode") : nullptr;
        return exitCode && exitCode->isNum() ? static_cast<int>(exitCode->num) : 1;
    }

    if (cmd == "replay") {
        OptionSpec spec;
        Flags flags;
        std::vector<std::string> positional;
        parseSimple(rest, spec, flags, positional);
        if (positional.empty()) fail("usage: poc-evidence replay <captureId>");
        ReplayResult result;
        try {
            result = runReplay(cwd, positional[0]);
        } catch (const std::runtime_error& e) {
            fail(e.what());
        }
        out("replay " + stringOf(result.record.get("id")) + " of " + positional[0] + ": " +
            result.verdict);
        for (const auto& field : result.fields.arr) {
            const Json* match = field.get("match");
            if (match && match->isBool() && match->b) {
                out("  " + jsPadEnd(stringOf(field.get("field")), 24) + " match");
            } else {
                const Json* expected = field.get("expected");
                const Json* actual = field.get("actual");
                std::string expectedStr =
                    jsSlice16(expected ? jsToString(expected) : "undefined", 16);
                std::string actualStr =
                    actual && actual->isNull() ? "missing" : jsSlice16(jsToString(actual), 16);
                out("  " + jsPadEnd(stringOf(field.get("field")), 24) + " DRIFT (expected " +
                    expectedStr + "… got " + actualStr + "…)");
            }
        }
        for (const auto& check : result.inputChecks.arr) {
            const Json* match = check.get("match");
            if (match && match->isBool() && !match->b) {
                out("  input " + stringOf(check.get("path")) + " changed or missing (expected " +
                    jsSlice16(stringOf(check.get("expected")), 16) + "…)");
            }
        }
        return result.verdict == "MATCH" ? 0 : 1;
    }

    if (cmd == "verify") {
        Json verdict;
        try {
            verdict = verifyChain(cwd);
        } catch (const std::runtime_error& e) {
            fail(e.what());
        }
        const Json* ok = verdict.get("ok");
        if (ok && ok->isBool() && ok->b) {
            out("chain intact: " + jsToString(verdict.get("count")) + " record(s), head " +
                stringOf(verdict.get("head")));
            return 0;
        }
        const Json* index = verdict.get("index");
        Json indexOne = index ? Json::number(index->num + 1) : Json::null();
        errOut("chain TAMPERED at record " + jsToString(&indexOne) + " (" +
               stringOf(verdict.get("id")) + "): " + stringOf(verdict.get("reason")));
        return 1;
    }

    if (cmd == "bundle") {
        OptionSpec spec;
        spec.single = {"out"};
        Flags flags;
        std::vector<std::string> positional;
        parseSimple(rest, spec, flags, positional);
        if (positional.empty()) {
            fail("usage: poc-evidence bundle <captureId> [--out <dir>]");
        }
        BundleResult built;
        try {
            auto outDirIt = flags.single.find("out");
            built = buildBundle(cwd, positional[0],
                                outDirIt == flags.single.end() ? "" : outDirIt->second);
        } catch (const std::runtime_error& e) {
            fail(e.what());
        }
        std::string rel = pathRelative(cwd, built.out);
        out("bundle written: " + (rel.empty() ? built.out : rel));
        const Json* files = built.manifest.get("files");
        if (files && files->isArr()) {
            for (const auto& f : files->arr) {
                out("  " + stringOf(f.get("path")) + "  " + stringOf(f.get("sha256")));
            }
        }
        return 0;
    }

    if (cmd == "anchor") {
        OptionSpec spec;
        spec.single = {"message"};
        Flags flags;
        std::vector<std::string> positional;
        parseSimple(rest, spec, flags, positional);
        AnchorResult anchored;
        try {
            auto msgIt = flags.single.find("message");
            anchored = runAnchor(cwd, msgIt == flags.single.end() ? "" : msgIt->second,
                                 msgIt != flags.single.end());
        } catch (const std::runtime_error& e) {
            fail(e.what());
        }
        out("anchored head " + jsSlice16(anchored.head, 16) + "… at commit " +
            jsSlice16(anchored.commit, 16) + "…");
        out("  note: push to a remote to obtain a third-party timestamp");
        out("  note: chain.jsonl contains command lines — check repo visibility before pushing");
        return 0;
    }

    if (cmd == "help" || cmd == "--help" || cmd == "-h" || cmd.empty()) {
        out(usage());
        return 0;
    }

    if (cmd == "version" || cmd == "--version" || cmd == "-v") {
        out(VERSION);
        return 0;
    }

    fail("unknown command \"" + cmd + "\" (see 'poc-evidence help')");
}

}  // namespace pocev
