// anchor.h — mirrors lib/anchor.mjs.
#ifndef pocev_ANCHOR_H
#define pocev_ANCHOR_H

#include "jsjson.h"

#include <string>

namespace pocev {

struct AnchorResult {
    Json record;        // {id, type, ts, prev, payload, hash}
    std::string commit; // git rev-parse HEAD
    std::string head;   // chain head before the anchor record
    std::string subject;
};

AnchorResult runAnchor(const std::string& cwd, const std::string& message, bool hasMessage);

}  // namespace pocev

#endif  // pocev_ANCHOR_H
