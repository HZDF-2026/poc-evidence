// anchor.cpp — see anchor.h.
#include "anchor.h"

#include "chain.h"
#include "jsjson.h"
#include "util.h"

#include <stdexcept>

namespace pocev {

namespace {

std::string findGit() {
    static const char* candidates[] = {
        "git",
        "C:\\Program Files\\Git\\cmd\\git.exe",
        "C:\\Program Files (x86)\\Git\\cmd\\git.exe",
    };
    for (const char* candidate : candidates) {
        try {
            CommandResult r = runCommand({candidate, "--version"}, "");
            if (!r.signaled && r.code == 0) return candidate;
        } catch (const std::runtime_error&) {
            // try next candidate
        }
    }
    return std::string();
}

}  // namespace

AnchorResult runAnchor(const std::string& cwd, const std::string& message, bool hasMessage) {
    if (!pathExists(pathJoin(cwd, ".git"))) {
        throw std::runtime_error("not a git repository — run `git init` first");
    }
    std::string git = findGit();
    if (git.empty()) throw std::runtime_error("git executable not found");

    std::string head = chainHead(cwd);
    Json payload = Json::object();
    payload.set("head", Json::string(head));
    payload.set("message", hasMessage ? Json::string(message) : Json::null());
    Json record = appendRecord(cwd, "anchor", payload);

    std::string subject = "poc-evidence anchor " + head.substr(0, 16);
    if (hasMessage) subject += ": " + message;

    CommandResult result = runCommand({git, "add", "-f", CHAIN_FILE}, cwd);
    if (result.signaled || result.code != 0) {
        std::string why = result.err.empty() ? "unknown error" : result.err;
        throw std::runtime_error("git add failed: " + why);
    }
    result = runCommand({git, "commit", "--allow-empty", "-m", subject}, cwd);
    if (result.signaled || result.code != 0) {
        std::string why = result.err.empty() ? "unknown error" : result.err;
        throw std::runtime_error("git commit failed: " + why);
    }
    result = runCommand({git, "rev-parse", "HEAD"}, cwd);
    std::string commit;
    size_t start = 0;
    while (start < result.out.size()) {
        size_t nl = result.out.find('\n', start);
        std::string line = result.out.substr(
            start, nl == std::string::npos ? std::string::npos : nl - start);
        size_t s = line.find_first_not_of(" \t\r\n");
        size_t e = line.find_last_not_of(" \t\r\n");
        if (s != std::string::npos) {
            commit = line.substr(s, e - s + 1);
            break;
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }

    AnchorResult out;
    out.record = record;
    out.commit = commit;
    out.head = head;
    out.subject = subject;
    return out;
}

}  // namespace pocev
