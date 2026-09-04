// replay.h — mirrors lib/replay.mjs.
#ifndef pocev_REPLAY_H
#define pocev_REPLAY_H

#include "jsjson.h"

#include <string>
#include <vector>

namespace pocev {

struct ReplayResult {
    Json record;       // {id, type, ts, prev, payload, hash}
    std::string verdict;  // MATCH, DRIFT, INVALID_INPUTS
    Json fields;       // [{field, expected, actual, match}]
    Json inputChecks;  // [{path, expected, actual, match}]
};

ReplayResult runReplay(const std::string& cwd, const std::string& captureId);

}  // namespace pocev

#endif  // pocev_REPLAY_H
