// capture.h — mirrors lib/capture.mjs.
#ifndef pocev_CAPTURE_H
#define pocev_CAPTURE_H

#include "jsjson.h"
#include "jsre.h"

#include <string>
#include <vector>

namespace pocev {

struct CaptureOpts {
    std::string cwd;
    std::vector<std::string> command;
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;
    std::vector<Substitution> redactions;    // {pattern, flags, replacement}
    std::vector<Substitution> normalizers;   // {pattern, flags, replacement}
    std::string label;                       // empty = null
    bool hasLabel = false;
    bool storeEnvValues = false;
};

// Appends the capture record and writes the redacted artifacts; returns the
// record ({id, type, ts, prev, payload, hash}).
Json runCapture(const CaptureOpts& opts);

}  // namespace pocev

#endif  // pocev_CAPTURE_H
