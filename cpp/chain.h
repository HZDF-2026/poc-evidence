// chain.h — mirrors lib/chain.mjs: the append-only hash chain stored as JSONL.
#ifndef pocev_CHAIN_H
#define pocev_CHAIN_H

#include "jsjson.h"

#include <string>
#include <vector>

namespace pocev {

extern const char* const STORE_DIR;     // ".poc-evidence"
extern const char* const CHAIN_FILE;    // ".poc-evidence/chain.jsonl" (native seps)
extern const char* const ARTIFACTS_DIR; // ".poc-evidence/artifacts"

std::string chainFilePath(const std::string& cwd);
std::string genesis();  // 64 zeros

void ensureStore(const std::string& cwd);

// Parsed records; empty when the store has no chain yet. Throws on malformed
// JSON lines (like JSON.parse does).
std::vector<Json> readChain(const std::string& cwd);

std::string chainHead(const std::string& cwd);

// Returns {id, type, ts, prev, payload, hash} with hash covering
// stableStringify(body).
Json appendRecord(const std::string& cwd, const std::string& type, const Json& payload);

// {ok:true,count,head} or {ok:false,index,id,reason}.
Json verifyChain(const std::string& cwd);

}  // namespace pocev

#endif  // pocev_CHAIN_H
