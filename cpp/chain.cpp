// chain.cpp — see chain.h.
#include "chain.h"

#include "jsjson.h"
#include "sha256.h"
#include "util.h"

#include <filesystem>
#include <stdexcept>
#include <string>

namespace pocev {

const char* const STORE_DIR = ".poc-evidence";
const char* const CHAIN_FILE = ".poc-evidence/chain.jsonl";
const char* const ARTIFACTS_DIR = ".poc-evidence/artifacts";

namespace {

std::string artifactsPath(const std::string& cwd) {
    return pathJoin(cwd, ARTIFACTS_DIR);
}

// String.prototype.trim() whitespace (JS view), used to skip blank lines.
bool isJsBlank(const std::string& line) {
    for (unsigned char c : line) {
        switch (c) {
            case '\t': case '\n': case '\v': case '\f': case '\r': case ' ':
                continue;
            default:
                return false;
        }
    }
    return true;
}

}  // namespace

std::string chainFilePath(const std::string& cwd) {
    return pathJoin(cwd, CHAIN_FILE);
}

std::string genesis() {
    return std::string(64, '0');
}

void ensureStore(const std::string& cwd) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(artifactsPath(cwd)), ec);
}

std::vector<Json> readChain(const std::string& cwd) {
    std::vector<Json> records;
    std::string text;
    try {
        text = readFileBytes(chainFilePath(cwd));
    } catch (const std::runtime_error&) {
        return records;
    }
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        std::string line = text.substr(start, nl == std::string::npos ? std::string::npos
                                                                      : nl - start);
        start = nl == std::string::npos ? text.size() + 1 : nl + 1;
        if (isJsBlank(line)) continue;
        Json rec;
        if (!Json::parse(line, rec)) {
            throw std::runtime_error("Unexpected token in JSON: " + line.substr(0, 32));
        }
        records.push_back(std::move(rec));
    }
    return records;
}

std::string chainHead(const std::string& cwd) {
    std::vector<Json> records = readChain(cwd);
    if (records.empty()) return genesis();
    const Json* h = records.back().get("hash");
    return h && h->isStr() ? h->str : genesis();
}

Json appendRecord(const std::string& cwd, const std::string& type, const Json& payload) {
    ensureStore(cwd);
    std::vector<Json> records = readChain(cwd);
    std::string prev = genesis();
    if (!records.empty()) {
        const Json* h = records.back().get("hash");
        if (h && h->isStr()) prev = h->str;
    }
    long seq = 0;
    for (const auto& r : records) {
        const Json* t = r.get("type");
        if (t && t->isStr() && t->str == type) seq++;
    }
    seq++;
    std::string id = type.substr(0, 3) + "_";
    if (seq < 10) id += "00";
    else if (seq < 100) id += "0";
    id += std::to_string(seq);

    Json body = Json::object();
    body.set("id", Json::string(id));
    body.set("type", Json::string(type));
    body.set("ts", Json::string(nowIso()));
    body.set("prev", Json::string(prev));
    body.set("payload", payload);
    std::string hash = sha256Hex(stableStringify(body));

    Json record = body;
    record.set("hash", Json::string(hash));
    appendFileBytes(chainFilePath(cwd), record.dump() + "\n");
    return record;
}

Json verifyChain(const std::string& cwd) {
    std::string text;
    try {
        text = readFileBytes(chainFilePath(cwd));
    } catch (const std::runtime_error&) {
        Json out = Json::object();
        out.set("ok", Json::boolean(true));
        out.set("count", Json::number(0));
        out.set("head", Json::string(genesis()));
        return out;
    }

    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        std::string line = text.substr(start, nl == std::string::npos ? std::string::npos
                                                                      : nl - start);
        start = nl == std::string::npos ? text.size() + 1 : nl + 1;
        if (isJsBlank(line)) continue;
        lines.push_back(line);
    }

    std::string prev = genesis();
    for (size_t i = 0; i < lines.size(); i++) {
        Json record;
        if (!Json::parse(lines[i], record)) {
            Json out = Json::object();
            out.set("ok", Json::boolean(false));
            out.set("index", Json::number(static_cast<double>(i)));
            out.set("id", Json::string("line " + std::to_string(i + 1)));
            out.set("reason", Json::string("malformed JSON"));
            return out;
        }
        const Json* h = record.isObj() ? record.get("hash") : nullptr;
        if (!record.isObj() || !h || !h->isStr()) {
            Json out = Json::object();
            out.set("ok", Json::boolean(false));
            out.set("index", Json::number(static_cast<double>(i)));
            out.set("id", Json::string("line " + std::to_string(i + 1)));
            out.set("reason", Json::string("malformed record"));
            return out;
        }
        Json body = Json::object();
        for (const auto& kv : record.obj) {
            if (kv.first == "hash") continue;
            body.obj.push_back(kv);
        }
        if (sha256Hex(stableStringify(body)) != h->str) {
            Json out = Json::object();
            out.set("ok", Json::boolean(false));
            out.set("index", Json::number(static_cast<double>(i)));
            const Json* id = record.get("id");
            out.set("id", Json::string(id && id->isStr() ? id->str : "undefined"));
            out.set("reason", Json::string("record hash mismatch"));
            return out;
        }
        const Json* pv = record.get("prev");
        if (!pv || !pv->isStr() || pv->str != prev) {
            Json out = Json::object();
            out.set("ok", Json::boolean(false));
            out.set("index", Json::number(static_cast<double>(i)));
            const Json* id = record.get("id");
            out.set("id", Json::string(id && id->isStr() ? id->str : "undefined"));
            out.set("reason", Json::string("chain link broken"));
            return out;
        }
        prev = h->str;
    }
    Json out = Json::object();
    out.set("ok", Json::boolean(true));
    out.set("count", Json::number(static_cast<double>(lines.size())));
    out.set("head", Json::string(prev));
    return out;
}

}  // namespace pocev
