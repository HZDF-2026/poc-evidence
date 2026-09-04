// replay.cpp — see replay.h.
#include "replay.h"

#include "chain.h"
#include "jsjson.h"
#include "jsre.h"
#include "sha256.h"
#include "util.h"

#include <stdexcept>

namespace pocev {

namespace {

std::string payloadString(const Json* payload, const char* key, const std::string& fallback = "") {
    if (!payload) return fallback;
    const Json* v = payload->get(key);
    return v && v->isStr() ? v->str : fallback;
}

}  // namespace

ReplayResult runReplay(const std::string& cwd, const std::string& captureId) {
    std::vector<Json> records = readChain(cwd);
    const Json* capture = nullptr;
    for (const auto& r : records) {
        const Json* id = r.get("id");
        const Json* type = r.get("type");
        if (id && id->isStr() && id->str == captureId && type && type->isStr() &&
            type->str == "capture") {
            capture = &r;
            break;
        }
    }
    if (!capture) throw std::runtime_error("capture not found: " + captureId);
    const Json* captured = capture->get("payload");

    Json inputChecks = Json::array();
    const Json* inputFiles = captured->get("inputFiles");
    bool inputsChanged = false;
    if (inputFiles && inputFiles->isArr()) {
        for (const auto& file : inputFiles->arr) {
            const Json* path = file.get("path");
            const Json* expected = file.get("sha256");
            std::string actual;
            bool present = false;
            if (path && path->isStr()) {
                try {
                    actual = sha256File(pathJoin(cwd, path->str));
                    present = true;
                } catch (const std::runtime_error&) {
                }
            }
            bool match = present && expected && expected->isStr() && expected->str == actual;
            if (!match) inputsChanged = true;
            Json check = Json::object();
            check.set("path", path ? *path : Json::null());
            check.set("expected", expected ? *expected : Json::null());
            check.set("actual", present ? Json::string(actual) : Json::null());
            check.set("match", Json::boolean(match));
            inputChecks.push(check);
        }
    }

    Json outputFields = Json::array();
    const Json* outputFiles = captured->get("outputFiles");
    if (outputFiles && outputFiles->isArr()) {
        for (const auto& file : outputFiles->arr) {
            const Json* path = file.get("path");
            const Json* expected = file.get("sha256");
            std::string actual;
            bool present = false;
            if (path && path->isStr()) {
                try {
                    actual = sha256File(pathJoin(cwd, path->str));
                    present = true;
                } catch (const std::runtime_error&) {
                }
            }
            bool match = present && expected && expected->isStr() && expected->str == actual;
            Json f = Json::object();
            f.set("field", Json::string("out:" + (path && path->isStr() ? path->str : "")));
            f.set("expected", expected ? *expected : Json::null());
            f.set("actual", present ? Json::string(actual) : Json::null());
            f.set("match", Json::boolean(match));
            outputFields.push(f);
        }
    }

    std::string recordedCwd = payloadString(captured, "cwd");
    if (!pathExists(recordedCwd)) {
        throw std::runtime_error("recorded working directory no longer exists: " + recordedCwd);
    }

    std::vector<std::string> command;
    const Json* commandArr = captured->get("command");
    if (commandArr && commandArr->isArr()) {
        for (const auto& c : commandArr->arr) {
            if (c.isStr()) command.push_back(c.str);
        }
    }
    CommandResult run = runCommand(command, recordedCwd);
    std::vector<Substitution> redactions, normalizers;
    auto readSubs = [](const Json* list, std::vector<Substitution>& out) {
        if (!list || !list->isArr()) return;
        for (const auto& s : list->arr) {
            Substitution sub;
            const Json* p = s.get("pattern");
            const Json* f = s.get("flags");
            const Json* r = s.get("replacement");
            sub.pattern = p && p->isStr() ? p->str : "";
            sub.flags = f && f->isStr() ? f->str : "";
            sub.replacement = r && r->isStr() ? r->str : "";
            out.push_back(sub);
        }
    };
    readSubs(captured->get("redactions"), redactions);
    readSubs(captured->get("normalizers"), normalizers);
    std::string stdoutFinal = applySubstitutions(applySubstitutions(run.out, redactions),
                                                normalizers);
    std::string stderrFinal = applySubstitutions(applySubstitutions(run.err, redactions),
                                                normalizers);

    std::string stdoutHash = sha256Hex(stdoutFinal);
    std::string stderrHash = sha256Hex(stderrFinal);

    Json fields = Json::array();
    auto addField = [&](const std::string& name, const Json* expected, Json actual, bool match) {
        Json f = Json::object();
        f.set("field", Json::string(name));
        f.set("expected", expected ? *expected : Json::null());
        f.set("actual", std::move(actual));
        f.set("match", Json::boolean(match));
        fields.push(f);
    };
    const Json* exitExpected = captured->get("exitCode");
    Json exitActual = run.signaled ? Json::null() : Json::number(static_cast<double>(run.code));
    // captured.exitCode === exitCode, strict: null === null matches too.
    bool exitMatch = false;
    if (exitExpected && exitExpected->isNum() && !run.signaled) {
        exitMatch = exitExpected->num == static_cast<double>(run.code);
    } else if (exitExpected && exitExpected->isNull() && run.signaled) {
        exitMatch = true;
    }
    addField("exitCode", exitExpected, exitActual, exitMatch);
    const Json* stdoutExpected = captured->get("stdoutHash");
    addField("stdout", stdoutExpected, Json::string(stdoutHash),
             stdoutExpected && stdoutExpected->isStr() && stdoutExpected->str == stdoutHash);
    const Json* stderrExpected = captured->get("stderrHash");
    addField("stderr", stderrExpected, Json::string(stderrHash),
             stderrExpected && stderrExpected->isStr() && stderrExpected->str == stderrHash);
    for (const auto& f : outputFields.arr) fields.push(f);

    bool allMatch = true;
    for (const auto& f : fields.arr) {
        const Json* m = f.get("match");
        if (!m || !m->isBool() || !m->b) {
            allMatch = false;
            break;
        }
    }
    std::string verdict = inputsChanged ? "INVALID_INPUTS" : (allMatch ? "MATCH" : "DRIFT");

    Json payload = Json::object();
    payload.set("ref", Json::string(captureId));
    payload.set("verdict", Json::string(verdict));
    payload.set("inputsChanged", Json::boolean(inputsChanged));
    payload.set("fields", fields);
    Json runtime = Json::object();
#ifdef _WIN32
    runtime.set("platform", Json::string("win32"));
#elif defined(__APPLE__)
    runtime.set("platform", Json::string("darwin"));
#else
    runtime.set("platform", Json::string("linux"));
#endif
    runtime.set("node", Json::string("cpp"));
    payload.set("runtime", runtime);

    Json record = appendRecord(cwd, "replay", payload);

    writeFileBytes(
        pathJoin(pathJoin(cwd, ARTIFACTS_DIR), record.get("id")->str + ".stdout.txt"),
        stdoutFinal);
    writeFileBytes(
        pathJoin(pathJoin(cwd, ARTIFACTS_DIR), record.get("id")->str + ".stderr.txt"),
        stderrFinal);

    ReplayResult out;
    out.record = record;
    out.verdict = verdict;
    out.fields = fields;
    out.inputChecks = inputChecks;
    return out;
}

}  // namespace pocev
