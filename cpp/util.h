// util.h — mirrors lib/util.mjs: file hashing, ISO timestamps, command
// execution and file collection with Node/libuv-visible semantics.
#ifndef pocev_UTIL_H
#define pocev_UTIL_H

#include <string>
#include <vector>

namespace pocev {

// package.json version, kept in sync by hand.
extern const char* const VERSION;

// Node's Date.prototype.toISOString(): YYYY-MM-DDTHH:MM:SS.mmmZ (UTC).
std::string nowIso();

// sha256 of the file's bytes; throws std::runtime_error when unreadable.
std::string sha256File(const std::string& path);

// Node spawn(cmd, args, {cwd, shell:false}) with utf8-decoded output.
struct CommandResult {
    std::string out;  // stdout, sanitized like Buffer.toString('utf8')
    std::string err;
    long long code;  // exit status
    bool signaled;   // killed by a signal: Node reports code === null
};
CommandResult runCommand(const std::vector<std::string>& command, const std::string& cwd);

// util.mjs collectFiles: relative (forward-slash) paths, sorted the way JS
// Array.prototype.sort() sorts strings; throws on missing/unsupported targets.
std::vector<std::string> collectFiles(const std::string& target, const std::string& baseDir);

// Environment variable names in process.env order (UTF-16 sorted, '='-prefixed
// pseudo-entries excluded) paired with their values.
std::vector<std::pair<std::string, std::string>> sortedEnv();

// Re-encode bytes the way Buffer.toString('utf8') would (invalid sequences
// become U+FFFD).
std::string utf8Sanitize(const std::string& bytes);

// Path helpers mirroring node:path resolve/relative/join for the tool's needs.
std::string pathResolve(const std::string& base, const std::string& target);
std::string pathRelative(const std::string& from, const std::string& to);
std::string pathJoin(const std::string& a, const std::string& b);
std::string pathSep();
bool pathExists(const std::string& p);

// Raw byte file IO (no newline translation, no BOM) — fs/promises equivalents.
void writeFileBytes(const std::string& path, const std::string& data);
void appendFileBytes(const std::string& path, const std::string& data);
std::string readFileBytes(const std::string& path);  // throws when unreadable

}  // namespace pocev

#endif  // pocev_UTIL_H
