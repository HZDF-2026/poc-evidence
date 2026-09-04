// capture.cpp — see capture.h.
#include "capture.h"

#include "chain.h"
#include "jsjson.h"
#include "jsre.h"
#include "sha256.h"
#include "util.h"

#include <chrono>
#include <stdexcept>

namespace pocev {

namespace {

Json fileEntry(const std::string& rel, const std::string& hash) {
    Json f = Json::object();
    f.set("path", Json::string(rel));
    f.set("sha256", Json::string(hash));
    return f;
}

long long wallMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

const char* platformName() {
#ifdef _WIN32
    return "win32";
#elif defined(__APPLE__)
    return "darwin";
#else
    return "linux";
#endif
}

const char* archName() {
#if defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#elif defined(_M_IX86) || defined(__i386__)
    return "ia32";
#else
    return "x64";
#endif
}

Json hashFileList(const std::vector<std::string>& targets, const std::string& cwd) {
    Json out = Json::array();
    for (const auto& target : targets) {
        for (const auto& rel : collectFiles(target, cwd)) {
            out.push(fileEntry(rel, sha256File(pathJoin(cwd, rel))));
        }
    }
    return out;
}

}  // namespace

Json runCapture(const CaptureOpts& opts) {
    if (opts.command.empty()) {
        throw std::runtime_error("command must be a non-empty array");
    }
    const std::string& cwd = opts.cwd;

    ensureStore(cwd);

    Json inputFiles = hashFileList(opts.inputs, cwd);

    long long startedAt = wallMs();
    CommandResult run = runCommand(opts.command, cwd);
    long long durationMs = wallMs() - startedAt;

    std::string stdoutFinal = applySubstitutions(applySubstitutions(run.out, opts.redactions),
                                                 opts.normalizers);
    std::string stderrFinal = applySubstitutions(applySubstitutions(run.err, opts.redactions),
                                                 opts.normalizers);

    Json outputFiles = hashFileList(opts.outputs, cwd);

    Json env = Json::object();
    for (const auto& kv : sortedEnv()) {
        env.set(kv.first,
                Json::string(opts.storeEnvValues ? kv.second : sha256Hex(kv.second)));
    }

    Json payload = Json::object();
    payload.set("label", opts.hasLabel ? Json::string(opts.label) : Json::null());
    Json command = Json::array();
    for (const auto& c : opts.command) command.push(Json::string(c));
    payload.set("command", command);
    payload.set("cwd", Json::string(cwd));
    payload.set("exitCode", run.signaled ? Json::null() : Json::number(static_cast<double>(run.code)));
    payload.set("durationMs", Json::number(static_cast<double>(durationMs)));
    payload.set("stdoutHash", Json::string(sha256Hex(stdoutFinal)));
    payload.set("stderrHash", Json::string(sha256Hex(stderrFinal)));
    payload.set("stdoutBytes", Json::number(static_cast<double>(stdoutFinal.size())));
    payload.set("stderrBytes", Json::number(static_cast<double>(stderrFinal.size())));
    payload.set("envMode", Json::string(opts.storeEnvValues ? "values" : "hashes"));
    payload.set("env", env);
    Json redactions = Json::array();
    for (const auto& r : opts.redactions) {
        Json j = Json::object();
        j.set("pattern", Json::string(r.pattern));
        j.set("flags", Json::string(r.flags));
        j.set("replacement", Json::string(r.replacement));
        redactions.push(j);
    }
    payload.set("redactions", redactions);
    Json normalizers = Json::array();
    for (const auto& r : opts.normalizers) {
        Json j = Json::object();
        j.set("pattern", Json::string(r.pattern));
        j.set("flags", Json::string(r.flags));
        j.set("replacement", Json::string(r.replacement));
        normalizers.push(j);
    }
    payload.set("normalizers", normalizers);
    payload.set("inputFiles", inputFiles);
    payload.set("outputFiles", outputFiles);
    Json runtime = Json::object();
    runtime.set("platform", Json::string(platformName()));
    runtime.set("arch", Json::string(archName()));
    runtime.set("node", Json::string("cpp"));
    payload.set("runtime", runtime);

    Json record = appendRecord(cwd, "capture", payload);

    writeFileBytes(
        pathJoin(pathJoin(cwd, ARTIFACTS_DIR), record.get("id")->str + ".stdout.txt"),
        stdoutFinal);
    writeFileBytes(
        pathJoin(pathJoin(cwd, ARTIFACTS_DIR), record.get("id")->str + ".stderr.txt"),
        stderrFinal);
    return record;
}

}  // namespace pocev
